
/*
 * Carrottus Net: UDP sockets' front end (crnet.h, "UDP sockets").
 *
 * Every argument check and every rule that is the same on each platform is
 * here, once: the desc's, a destination's family and port, the empty
 * datagram, the order of the checks. What reaches the system - opening and
 * binding, sendto, recvmsg, the wait for room or a datagram, the abort's
 * wake - is the backend's, behind the crnet_udp_backend_* verbs of
 * crnetshared.h: BSD sockets in backends/posix/crnetsockets.c (Apple,
 * Linux), Winsock on Windows.
 */

#include "crnetshared.h"
#include "crnet.h"
#include "../crstructbase.h"

#if CRNET_EXT_UDP

#include <string.h>

#if __cplusplus
extern "C" {
#endif

static bool crnet_udp_sharing_valid(CRNET_UDP_PORT_SHARING sharing) {
	return sharing == CRNET_UDP_PORT_SHARING_EXCLUSIVE || sharing == CRNET_UDP_PORT_SHARING_SHARED;
}

crnet_instance crnet_udp_socket_instance(crnet_udp_socket socket) { return socket ? socket->instance : NULL; }

CR_RESULT crnet_udp_socket_create(
    crnet_instance instance,
    const crnet_udp_socket_desc *desc,
    crnet_udp_socket *out_socket,
    crnet_error *out_error) {
	crnet_error_clear(out_error);
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_udp_socket_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_UDP_SOCKET_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_socket)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_socket = NULL;
	if (!crnet_address_valid(&desc->local))
		return CR_RESULT_NET_INVALID_ADDRESS;
	if (!crnet_udp_sharing_valid(desc->sharing) || desc->receive_buffer_size == 0 || desc->send_buffer_size == 0)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	struct crnet_udp_socket_t *socket = CR_NEW_ZEROED(&instance->head.allocator, struct crnet_udp_socket_t);
	if (!socket)
		return CR_RESULT_OUT_OF_MEMORY;
	socket->head.allocator = instance->head.allocator;
	CR_BIRTH_IN(socket, &instance->quarantine);
	socket->instance = instance;
	socket->family   = desc->local.family;
	crnet_udp_backend_reset(socket);
	const CR_RESULT opened = crnet_udp_backend_open(socket, desc, out_error);
	if (CR_FAILED(opened)) {
		crnet_udp_backend_close(socket);
		CR_DELETE(&instance->head.allocator, struct crnet_udp_socket_t, socket);
		return opened;
	}
	*out_socket = socket;
	return CR_SUCCESS;
}

void crnet_udp_socket_destroy(crnet_udp_socket socket) {
	if (!socket)
		return;
	if (CR_STALE(socket, "crnet_udp_socket_destroy", "socket"))
		return;
	crnet_udp_backend_close(socket);
	CR_KILL(socket);
	const CR_ALLOCATOR allocator = socket->head.allocator;
	cr_string_free(&allocator, socket->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(socket), &allocator, struct crnet_udp_socket_t, socket);
}

void crnet_udp_socket_abort(crnet_udp_socket socket) {
	if (!socket)
		return;
	if (CR_STALE(socket, "crnet_udp_socket_abort", "socket"))
		return;
	crnet_atomic_store32(&socket->aborted, 1);
	crnet_udp_backend_wake(socket);
}

void crnet_udp_socket_get_error(
    crnet_udp_socket socket,
    CRNET_ERROR_SIDE side,
    crnet_error *out_error) {
	if (!socket) {
		crnet_error_get(NULL, out_error);
		return;
	}
	if (CR_STALE(socket, "crnet_udp_socket_get_error", "socket")) {
		crnet_error_get(NULL, out_error);
		return;
	}
	const bool known = side == CRNET_ERROR_SIDE_RECEIVING || side == CRNET_ERROR_SIDE_SENDING;
	crnet_error_get(known ? &socket->errors[side] : NULL, out_error);
}

CR_RESULT crnet_udp_socket_get_local_address(
    crnet_udp_socket socket,
    crnet_address *out_address) {
	if (!socket)
		return CR_RESULT_NET_INVALID_UDP_SOCKET;
	if (CR_STALE(socket, "crnet_udp_socket_get_local_address", "socket"))
		return CR_RESULT_NET_INVALID_UDP_SOCKET;
	if (!out_address)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	return crnet_udp_backend_local_address(socket, out_address);
}

CR_RESULT crnet_udp_socket_get_buffer_sizes(
    crnet_udp_socket socket,
    uint32_t *out_receive_buffer_size,
    uint32_t *out_send_buffer_size) {
	if (!socket)
		return CR_RESULT_NET_INVALID_UDP_SOCKET;
	if (CR_STALE(socket, "crnet_udp_socket_get_buffer_sizes", "socket"))
		return CR_RESULT_NET_INVALID_UDP_SOCKET;
	if (!out_receive_buffer_size || !out_send_buffer_size)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	return crnet_udp_backend_buffer_sizes(socket, out_receive_buffer_size, out_send_buffer_size);
}

CR_RESULT crnet_udp_socket_send_to(
    crnet_udp_socket socket,
    const crnet_address *to,
    const void *data,
    uint32_t size,
    uint32_t timeout_ms) {
	if (!socket)
		return CR_RESULT_NET_INVALID_UDP_SOCKET;
	if (CR_STALE(socket, "crnet_udp_socket_send_to", "socket"))
		return CR_RESULT_NET_INVALID_UDP_SOCKET;
	if (!crnet_address_valid(to) || to->family != socket->family || to->port == 0)
		return CR_RESULT_NET_INVALID_ADDRESS;
	if (!data && size != 0)
		return CR_RESULT_ZERO_SIZE;
	if (crnet_atomic_load32(&socket->aborted))
		return CR_RESULT_NET_ABORTED;
	return crnet_udp_backend_send_to(socket, to, data, size, timeout_ms);
}

CR_RESULT crnet_udp_socket_recv_from(
    crnet_udp_socket socket,
    void *buffer,
    uint32_t capacity,
    uint32_t timeout_ms,
    uint32_t *out_size,
    crnet_address *out_from) {
	// Cleared first, as TCP's receive does, so it is 0 on every failure.
	if (!out_size)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_size = 0;
	if (!socket)
		return CR_RESULT_NET_INVALID_UDP_SOCKET;
	if (CR_STALE(socket, "crnet_udp_socket_recv_from", "socket"))
		return CR_RESULT_NET_INVALID_UDP_SOCKET;
	if (!buffer || capacity == 0)
		return CR_RESULT_ZERO_SIZE;
	if (!out_from)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (crnet_atomic_load32(&socket->aborted))
		return CR_RESULT_NET_ABORTED;
	return crnet_udp_backend_recv_from(socket, buffer, capacity, timeout_ms, out_size, out_from);
}

#if __cplusplus
}
#endif

#endif // CRNET_EXT_UDP
