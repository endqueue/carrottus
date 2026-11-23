
#if CARROTTUS_PLATFORM_WIN32

#include "../../crnetshared.h"
#include "crnet.h"

/*
 * Carrottus Net's local-network parts on Windows, not yet built: the
 * interface listing (GetAdaptersAddresses and GetIfTable2), UDP sockets
 * (Winsock2) and service discovery (Windows' DNS-SD calls in dnsapi.dll).
 * Each verb here refuses with CR_RESULT_NOT_IMPLEMENTED_YET, so that
 * carrottus builds and links on Windows, and the front ends' argument checks
 * run there, until prompts/crnet_local_network_windows.md replaces them.
 */

#if __cplusplus
extern "C" {
#endif

#if CRNET_EXT_TCP

CR_RESULT crnet_interface_backend_enumerate(crnet_instance instance, struct crnet_interface_list_t **out_list, crnet_error *out_error) {
	(void)instance;
	(void)out_error;
	*out_list = NULL;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

#endif // CRNET_EXT_TCP

#if CRNET_EXT_UDP

void crnet_udp_backend_reset(struct crnet_udp_socket_t *socket) {
	socket->native  = -1;
	socket->wake[0] = -1;
	socket->wake[1] = -1;
}

CR_RESULT crnet_udp_backend_open(struct crnet_udp_socket_t *socket, const crnet_udp_socket_desc *desc, crnet_error *out_error) {
	(void)socket;
	(void)desc;
	(void)out_error;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

void crnet_udp_backend_close(struct crnet_udp_socket_t *socket) { (void)socket; }

void crnet_udp_backend_wake(struct crnet_udp_socket_t *socket) { (void)socket; }

// No socket is ever opened here, so none of the verbs below is reached.

CR_RESULT crnet_udp_backend_send_to(struct crnet_udp_socket_t *socket, const crnet_address *to, const void *data, uint32_t size, uint32_t timeout_ms) {
	(void)socket;
	(void)to;
	(void)data;
	(void)size;
	(void)timeout_ms;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT crnet_udp_backend_recv_from(struct crnet_udp_socket_t *socket, void *buffer, uint32_t capacity, uint32_t timeout_ms, uint32_t *out_size,
                                       crnet_address *out_from) {
	(void)socket;
	(void)buffer;
	(void)capacity;
	(void)timeout_ms;
	(void)out_size;
	(void)out_from;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT crnet_udp_backend_local_address(struct crnet_udp_socket_t *socket, crnet_address *out_address) {
	(void)socket;
	(void)out_address;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT crnet_udp_backend_buffer_sizes(struct crnet_udp_socket_t *socket, uint32_t *out_receive_buffer_size, uint32_t *out_send_buffer_size) {
	(void)socket;
	(void)out_receive_buffer_size;
	(void)out_send_buffer_size;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

#endif // CRNET_EXT_UDP

#if __cplusplus
}
#endif

#endif // CARROTTUS_PLATFORM_WIN32
