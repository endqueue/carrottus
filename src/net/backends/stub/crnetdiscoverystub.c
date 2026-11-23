
#if CARROTTUS_PLATFORM_LINUX || CARROTTUS_PLATFORM_WIN32

#include "../../crnetshared.h"
#include "crnet.h"

#if CRNET_EXT_DISCOVERY

/*
 * Service discovery on Linux (Avahi) and Windows (the DNS-SD calls in
 * dnsapi.dll), not yet built: each start refuses with
 * CR_RESULT_NOT_IMPLEMENTED_YET, so carrottus builds on both and the front
 * end's checks run there, until prompts/crnet_local_network_windows.md and
 * prompts/crnet_local_network_fedora.md replace this file with each
 * system's backend.
 */

#if __cplusplus
extern "C" {
#endif

CR_RESULT crnet_discovery_backend_advert_start(struct crnet_discovery_object *object, const uint8_t *txt, size_t txt_size, crnet_error *out_error) {
	(void)object;
	(void)txt;
	(void)txt_size;
	(void)out_error;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT crnet_discovery_backend_browser_start(struct crnet_discovery_object *object, crnet_error *out_error) {
	(void)object;
	(void)out_error;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT crnet_discovery_backend_resolver_start(struct crnet_discovery_object *object, crnet_error *out_error) {
	(void)object;
	(void)out_error;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

void crnet_discovery_backend_stop(struct crnet_discovery_object *object) { (void)object; }

// No object is ever started here, so none of the verbs below is reached.

void crnet_discovery_backend_wake(struct crnet_discovery_object *object) { (void)object; }

CR_RESULT crnet_discovery_backend_pump(struct crnet_discovery_object *object, uint32_t timeout_ms) {
	(void)object;
	(void)timeout_ms;
	return CR_RESULT_NET_TIMED_OUT;
}

void crnet_discovery_backend_wait_prepare(struct crnet_discovery_object *object, struct crnet_wait_poll *poll) {
	(void)object;
	(void)poll;
}

#if __cplusplus
}
#endif

#endif // CRNET_EXT_DISCOVERY

#endif // CARROTTUS_PLATFORM_LINUX || CARROTTUS_PLATFORM_WIN32
