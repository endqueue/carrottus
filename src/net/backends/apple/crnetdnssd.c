
#if CARROTTUS_PLATFORM_APPLE

#include "../../crnetshared.h"
#include "crnet.h"

#if CRNET_EXT_DISCOVERY

/*
 * Service discovery on dns_sd, libSystem's client of mDNSResponder, on
 * every Apple platform that has sockets (crnet.h, "Service discovery"; the
 * front end is src/net/crnetdiscovery.c).
 *
 * Each object is one dns_sd operation with its own connection to
 * mDNSResponder, whose descriptor (DNSServiceRefSockFD) is polled beside a
 * pipe an abort writes to. When it is readable, DNSServiceProcessResult
 * reads one reply and runs its callback on this thread, inside the caller's
 * `_next`: nothing here runs on a thread of the system's, and nothing is
 * called back to the caller. The callbacks hand what they were given to
 * the front end's crnet_discovery_report_* calls.
 *
 * - An advert is DNSServiceRegister with kDNSServiceFlagsNoAutoRename, so a
 *   taken name ends it rather than becoming "Name (2)".
 * - A browser is DNSServiceBrowse.
 * - A resolver is a connection of its own (DNSServiceCreateConnection) and
 *   two operations on it, kDNSServiceFlagsShareConnection: the resolve, and
 *   once that has answered, DNSServiceGetAddrInfo on the host it named,
 *   made again if a later answer names another host. One descriptor
 *   carries both; deallocating the connection ends both.
 *
 * dns_sd's codes are recorded as the object's error under
 * CR_PLATFORM_ERROR_SOURCE_OTHER, the text starting "dns_sd": crcore.h has
 * no source of their own yet (claude_made_decisions.md, 2026-11-23).
 */

#include <dns_sd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if __cplusplus
extern "C" {
#endif

struct crnet_dnssd {
	// The operation (advert, browser) or the shared connection (resolver):
	// the reference whose descriptor is polled and whose replies are read.
	DNSServiceRef primary;
	// The resolver's two operations on its connection, or NULL.
	DNSServiceRef resolve;
	DNSServiceRef addresses;
	// The host the addresses are looked up for, and the service's port.
	char     host[CRNET_SERVICE_HOST_CAPACITY];
	uint16_t port;
	// What an abort writes to; -1 until made.
	int wake[2];
};

static struct crnet_dnssd *crnet_dnssd_of(struct crnet_discovery_object *object) { return (struct crnet_dnssd *)object->backend; }

/* A dns_sd failure as the object's end. */
static void crnet_dnssd_end(struct crnet_discovery_object *object, CR_RESULT result, DNSServiceErrorType code) {
	char text[64];
	snprintf(text, sizeof(text), "dns_sd: DNSServiceErrorType %d", (int)code);
	crnet_discovery_report_end(object, result, CR_PLATFORM_ERROR_SOURCE_OTHER, code, text);
}

/* What a failure dns_sd reports is, by its code: refused, the service not
 * there or gone, or anything else. */
static CR_RESULT crnet_dnssd_result(DNSServiceErrorType code, bool at_create) {
	switch (code) {
	case kDNSServiceErr_NoAuth:
	case kDNSServiceErr_PolicyDenied:
		return CR_RESULT_NET_DISCOVERY_DENIED;
	case kDNSServiceErr_ServiceNotRunning:
		return at_create ? CR_RESULT_NET_DISCOVERY_UNAVAILABLE : CR_RESULT_NET_DISCOVERY_FAILED;
	case kDNSServiceErr_NameConflict:
		return CR_RESULT_NET_SERVICE_NAME_CONFLICT;
	default:
		return CR_RESULT_NET_DISCOVERY_FAILED;
	}
}

/* A failure in a reply. kDNSServiceErr_PolicyDenied is no end: the
 * operation waits while the person is asked (TN3179), so it is an event. */
static void crnet_dnssd_reply_failed(struct crnet_discovery_object *object, DNSServiceErrorType code) {
	if (code == kDNSServiceErr_PolicyDenied)
		crnet_discovery_report_not_allowed(object);
	else
		crnet_dnssd_end(object, crnet_dnssd_result(code, false), code);
}

/* A create's failure, recorded in `out_error` and returned. */
static CR_RESULT crnet_dnssd_start_failed(crnet_error *out_error, DNSServiceErrorType code) {
	char text[64];
	snprintf(text, sizeof(text), "dns_sd: DNSServiceErrorType %d", (int)code);
	return crnet_error_record(out_error, crnet_dnssd_result(code, true), CR_PLATFORM_ERROR_SOURCE_OTHER, code, text);
}

/* The backend's state and its abort pipe, before the operation is begun. */
static CR_RESULT crnet_dnssd_make(struct crnet_discovery_object *object, crnet_error *out_error) {
	struct crnet_dnssd *state = CR_NEW_ZEROED(&object->head.allocator, struct crnet_dnssd);
	if (!state)
		return CR_RESULT_OUT_OF_MEMORY;
	state->wake[0]  = -1;
	state->wake[1]  = -1;
	object->backend = state;
	int ends[2];
	if (pipe(ends) != 0)
		return crnet_error_record(out_error, CR_RESULT_NET_DISCOVERY_FAILED, CR_PLATFORM_ERROR_SOURCE_ERRNO, errno, NULL);
	state->wake[0] = ends[0];
	state->wake[1] = ends[1];
	for (int i = 0; i < 2; ++i) {
		const int flags = fcntl(ends[i], F_GETFL, 0);
		if (flags < 0 || fcntl(ends[i], F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(ends[i], F_SETFD, FD_CLOEXEC) < 0)
			return crnet_error_record(out_error, CR_RESULT_NET_DISCOVERY_FAILED, CR_PLATFORM_ERROR_SOURCE_ERRNO, errno, NULL);
	}
	return CR_SUCCESS;
}

// --------------------------------------------------------------------------
// The callbacks, each run inside DNSServiceProcessResult on the caller's
// thread
// --------------------------------------------------------------------------

static void DNSSD_API crnet_dnssd_registered(DNSServiceRef ref, DNSServiceFlags flags, DNSServiceErrorType error, const char *name, const char *type,
                                             const char *domain, void *context) {
	(void)ref;
	(void)flags;
	(void)type;
	(void)domain;
	struct crnet_discovery_object *object = (struct crnet_discovery_object *)context;
	if (error != kDNSServiceErr_NoError) {
		crnet_dnssd_reply_failed(object, error);
		return;
	}
	// No rename was allowed, so the name is the one asked for; a reply that
	// names another is no registration of this advert's.
	if (name && strcmp(name, object->name) == 0)
		crnet_discovery_report_registered(object);
}

static void DNSSD_API crnet_dnssd_browsed(DNSServiceRef ref, DNSServiceFlags flags, uint32_t interface_index, DNSServiceErrorType error, const char *name,
                                          const char *type, const char *domain, void *context) {
	(void)ref;
	(void)domain;
	struct crnet_discovery_object *object = (struct crnet_discovery_object *)context;
	if (error != kDNSServiceErr_NoError) {
		crnet_dnssd_reply_failed(object, error);
		return;
	}
	crnet_discovery_report_found(object, (flags & kDNSServiceFlagsAdd) != 0, interface_index, name, type);
}

static void DNSSD_API crnet_dnssd_address(DNSServiceRef ref, DNSServiceFlags flags, uint32_t interface_index, DNSServiceErrorType error, const char *host,
                                          const struct sockaddr *address, uint32_t ttl, void *context) {
	(void)ref;
	(void)host;
	(void)ttl;
	struct crnet_discovery_object *object = (struct crnet_discovery_object *)context;
	// No record of one family is an answer for that family, not a failure.
	if (error == kDNSServiceErr_NoSuchRecord)
		return;
	if (error != kDNSServiceErr_NoError) {
		crnet_dnssd_reply_failed(object, error);
		return;
	}
	crnet_address value;
	if (!address || !crnet_address_from_sockaddr(address, address->sa_len, interface_index, &value, NULL))
		return;
	if ((object->family == CRNET_ADDRESS_FAMILY_IPV4 && value.family != CRNET_ADDRESS_FAMILY_IPV4) ||
	    (object->family == CRNET_ADDRESS_FAMILY_IPV6 && value.family != CRNET_ADDRESS_FAMILY_IPV6))
		return;
	value.port = crnet_dnssd_of(object)->port;
	crnet_discovery_report_address(object, (flags & kDNSServiceFlagsAdd) != 0, &value);
}

static DNSServiceProtocol crnet_dnssd_protocol(CRNET_ADDRESS_FAMILY family) {
	switch (family) {
	case CRNET_ADDRESS_FAMILY_IPV4:
		return kDNSServiceProtocol_IPv4;
	case CRNET_ADDRESS_FAMILY_IPV6:
		return kDNSServiceProtocol_IPv6;
	default:
		return kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6;
	}
}

static void DNSSD_API crnet_dnssd_resolved(DNSServiceRef ref, DNSServiceFlags flags, uint32_t interface_index, DNSServiceErrorType error,
                                           const char *full_name, const char *host, uint16_t port, uint16_t txt_size, const unsigned char *txt,
                                           void *context) {
	(void)ref;
	(void)flags;
	struct crnet_discovery_object *object = (struct crnet_discovery_object *)context;
	struct crnet_dnssd            *state  = crnet_dnssd_of(object);
	if (error != kDNSServiceErr_NoError) {
		crnet_dnssd_reply_failed(object, error);
		return;
	}
	const uint16_t service_port = ntohs(port);
	if (!crnet_discovery_report_service(object, full_name, host, service_port, txt, txt_size))
		return;
	state->port = service_port;
	// The host's addresses, looked up on the same connection; again if this
	// answer names another host than the last.
	if (state->addresses && strcmp(state->host, host) == 0)
		return;
	if (state->addresses) {
		DNSServiceRefDeallocate(state->addresses);
		state->addresses = NULL;
	}
	memcpy(state->host, host, strlen(host) + 1);
	DNSServiceRef             addresses = state->primary;
	const DNSServiceErrorType started   = DNSServiceGetAddrInfo(&addresses, kDNSServiceFlagsShareConnection, interface_index,
	                                                            crnet_dnssd_protocol(object->family), host, crnet_dnssd_address, object);
	if (started != kDNSServiceErr_NoError) {
		crnet_dnssd_end(object, crnet_dnssd_result(started, false), started);
		return;
	}
	state->addresses = addresses;
}

// --------------------------------------------------------------------------
// The verbs
// --------------------------------------------------------------------------

CR_RESULT crnet_discovery_backend_advert_start(struct crnet_discovery_object *object, const uint8_t *txt, size_t txt_size, crnet_error *out_error) {
	const CR_RESULT made = crnet_dnssd_make(object, out_error);
	if (CR_FAILED(made))
		return made;
	struct crnet_dnssd       *state   = crnet_dnssd_of(object);
	const DNSServiceErrorType started = DNSServiceRegister(&state->primary, kDNSServiceFlagsNoAutoRename, object->interface_index, object->name, object->type,
	                                                       "local.", NULL, htons(object->port), (uint16_t)txt_size, txt_size ? txt : NULL,
	                                                       crnet_dnssd_registered, object);
	if (started != kDNSServiceErr_NoError) {
		state->primary = NULL;
		return crnet_dnssd_start_failed(out_error, started);
	}
	return CR_SUCCESS;
}

CR_RESULT crnet_discovery_backend_browser_start(struct crnet_discovery_object *object, crnet_error *out_error) {
	const CR_RESULT made = crnet_dnssd_make(object, out_error);
	if (CR_FAILED(made))
		return made;
	struct crnet_dnssd       *state   = crnet_dnssd_of(object);
	const DNSServiceErrorType started = DNSServiceBrowse(&state->primary, 0, object->interface_index, object->type, "local.", crnet_dnssd_browsed, object);
	if (started != kDNSServiceErr_NoError) {
		state->primary = NULL;
		return crnet_dnssd_start_failed(out_error, started);
	}
	return CR_SUCCESS;
}

CR_RESULT crnet_discovery_backend_resolver_start(struct crnet_discovery_object *object, crnet_error *out_error) {
	const CR_RESULT made = crnet_dnssd_make(object, out_error);
	if (CR_FAILED(made))
		return made;
	struct crnet_dnssd *state     = crnet_dnssd_of(object);
	DNSServiceErrorType started   = DNSServiceCreateConnection(&state->primary);
	if (started != kDNSServiceErr_NoError) {
		state->primary = NULL;
		return crnet_dnssd_start_failed(out_error, started);
	}
	DNSServiceRef resolve = state->primary;
	started               = DNSServiceResolve(&resolve, kDNSServiceFlagsShareConnection, object->interface_index, object->name, object->type, "local.",
	                                          crnet_dnssd_resolved, object);
	if (started != kDNSServiceErr_NoError)
		return crnet_dnssd_start_failed(out_error, started);
	state->resolve = resolve;
	return CR_SUCCESS;
}

void crnet_discovery_backend_stop(struct crnet_discovery_object *object) {
	struct crnet_dnssd *state = crnet_dnssd_of(object);
	if (!state)
		return;
	// The operations on a shared connection go first; deallocating the
	// connection then ends whatever is left.
	if (state->addresses)
		DNSServiceRefDeallocate(state->addresses);
	if (state->resolve)
		DNSServiceRefDeallocate(state->resolve);
	if (state->primary)
		DNSServiceRefDeallocate(state->primary);
	if (state->wake[0] >= 0)
		close(state->wake[0]);
	if (state->wake[1] >= 0)
		close(state->wake[1]);
	object->backend = NULL;
	CR_DELETE(&object->head.allocator, struct crnet_dnssd, state);
}

void crnet_discovery_backend_wake(struct crnet_discovery_object *object) {
	struct crnet_dnssd *state = crnet_dnssd_of(object);
	char                byte  = 1;
	if (write(state->wake[1], &byte, 1) < 0) {
		// Full means it is already readable, which is all this is for.
	}
}

static uint64_t crnet_dnssd_now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

CR_RESULT crnet_discovery_backend_pump(struct crnet_discovery_object *object, uint32_t timeout_ms) {
	struct crnet_dnssd *state = crnet_dnssd_of(object);
	const uint64_t      start = crnet_dnssd_now_ms();
	for (;;) {
		if (crnet_atomic_load32(&object->aborted))
			return CR_RESULT_NET_ABORTED;
		int remaining = -1;
		if (timeout_ms != CRNET_WAIT_FOREVER) {
			const uint64_t elapsed = crnet_dnssd_now_ms() - start;
			remaining              = elapsed >= timeout_ms ? 0 : (int)(timeout_ms - elapsed > (uint64_t)INT32_MAX ? INT32_MAX : timeout_ms - elapsed);
		}
		struct pollfd waited[2];
		waited[0].fd      = DNSServiceRefSockFD(state->primary);
		waited[0].events  = POLLIN;
		waited[0].revents = 0;
		waited[1].fd      = state->wake[0];
		waited[1].events  = POLLIN;
		waited[1].revents = 0;
		const int ready   = poll(waited, 2, remaining);
		if (ready < 0 && errno == EINTR)
			continue;
		if (crnet_atomic_load32(&object->aborted))
			return CR_RESULT_NET_ABORTED;
		if (ready < 0) {
			crnet_discovery_report_end(object, CR_RESULT_NET_DISCOVERY_FAILED, CR_PLATFORM_ERROR_SOURCE_ERRNO, errno, NULL);
			return CR_SUCCESS;
		}
		if (ready == 0 || waited[0].revents == 0)
			return CR_RESULT_NET_TIMED_OUT;
		// One reply, read and its callback run here. A connection that has
		// gone (mDNSResponder restarted) fails here, and the object is over.
		const DNSServiceErrorType processed = DNSServiceProcessResult(state->primary);
		if (processed != kDNSServiceErr_NoError)
			crnet_dnssd_end(object, CR_RESULT_NET_DISCOVERY_FAILED, processed);
		return CR_SUCCESS;
	}
}

void crnet_discovery_backend_wait_prepare(struct crnet_discovery_object *object, struct crnet_wait_poll *poll) {
	struct crnet_dnssd *state = crnet_dnssd_of(object);
	if (state && state->primary)
		crnet_wait_poll_add(poll, DNSServiceRefSockFD(state->primary), true, false, false);
}

#if __cplusplus
}
#endif

#endif // CRNET_EXT_DISCOVERY

#endif // CARROTTUS_PLATFORM_APPLE
