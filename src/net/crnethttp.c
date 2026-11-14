
/*
 * Carrottus Net HTTP transfer - the part that is the same on every stack.
 *
 * crnet.h gives a transfer an order of use, a set of arguments it
 * refuses, a count to keep of a sized request body, and a response's
 * status and headers to hold until it is destroyed. None of that depends
 * on which stack carries the request, so it is here once: every
 * crnet_http_transfer_* in crnet.h is defined in this file, and each
 * does its checking and then hands the stack the one thing left to do,
 * through the crnet_http_backend_* functions in crnetshared.h.
 *
 * A backend can therefore take for granted that it is only asked for what
 * crnet.h allows next, and this file can take for granted that a
 * backend's result is the network's answer. The rule that joins the two:
 * any failure but CR_RESULT_NET_TIMED_OUT ends the transfer, and after
 * that nothing reaches the backend but destroy. A start that returns
 * CR_RESULT_NET_TIMED_OUT has started the transfer (crnet.h, "Steps"):
 * the backend's next call carries it on.
 */

#include "crnetshared.h"
#include "crnet.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../crstructbase.h"

#if __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------
// A response's status and headers
// --------------------------------------------------------------------------

/* Frees an entry's two strings. Each was allocated with its length and a
 * terminator, kept beside it: a value from the network may hold a '\0' of
 * its own, so strlen would not always find the size. */
static void crnet_head_free_entry(const CR_ALLOCATOR *allocator, const struct crnet_http_header_entry *entry) {
	cr_free(allocator, entry->name, entry->name_length + 1, 1);
	cr_free(allocator, entry->value, entry->value_length + 1, 1);
}

/* Points the entries at eqstd's view of `allocator`, kept in the head. */
static void crnet_head_bind(const CR_ALLOCATOR *allocator, struct crnet_http_head *head) {
	head->allocator         = cr_eq_allocator(allocator);
	head->entries.allocator = &head->allocator;
}

void crnet_head_clear(const CR_ALLOCATOR *allocator, struct crnet_http_head *head) {
	for (size_t i = 0; i < head->entries.count; ++i)
		crnet_head_free_entry(allocator, &head->entries.data[i]);
	crnet_head_bind(allocator, head);
	eq_array_free(&head->entries);
	CR_DELETE_ARRAY(allocator, uint32_t, head->slots, head->slot_count);
	memset(head, 0, sizeof(*head));
}

/* Drops the headers and keeps the room they were in. */
static void crnet_head_drop_headers(const CR_ALLOCATOR *allocator, struct crnet_http_head *head) {
	for (size_t i = 0; i < head->entries.count; ++i)
		crnet_head_free_entry(allocator, &head->entries.data[i]);
	eq_array_clear(&head->entries);
	if (head->slots)
		memset(head->slots, 0, head->slot_count * sizeof(head->slots[0]));
}

/* FNV-1a over the name with its letters made lower case, so that names
 * that differ only in case land in the same slot. */
static uint32_t crnet_head_hash(const char *name, size_t length) {
	uint32_t hash = 2166136261u;
	for (size_t i = 0; i < length; ++i) {
		unsigned char c = (unsigned char)name[i];
		if (c >= 'A' && c <= 'Z')
			c = (unsigned char)(c - 'A' + 'a');
		hash = (hash ^ c) * 16777619u;
	}
	return hash;
}

/* Puts entry `index` in the first free slot from its name's own. The table
 * is never more than half full, so there always is one. */
static void crnet_head_place(struct crnet_http_head *head, uint32_t index) {
	const uint32_t mask = head->slot_count - 1;
	uint32_t       slot = crnet_head_hash(head->entries.data[index].name, head->entries.data[index].name_length) & mask;
	while (head->slots[slot] != 0)
		slot = (slot + 1) & mask;
	head->slots[slot] = index + 1;
}

static struct crnet_http_header_entry *crnet_head_find(const struct crnet_http_head *head, const char *name, size_t name_length) {
	if (head->slot_count == 0)
		return NULL;
	const uint32_t mask = head->slot_count - 1;
	for (uint32_t slot = crnet_head_hash(name, name_length) & mask; head->slots[slot] != 0; slot = (slot + 1) & mask) {
		struct crnet_http_header_entry *entry = &head->entries.data[head->slots[slot] - 1];
		if (entry->name_length == name_length && crnet_ascii_equal_no_case(entry->name, name, name_length))
			return entry;
	}
	return NULL;
}

/* `length` bytes of `text` and a terminator, or NULL. */
static char *crnet_head_copy(const CR_ALLOCATOR *allocator, const char *text, size_t length) {
	if (length == SIZE_MAX)
		return NULL;
	char *copy = (char *)cr_allocate(allocator, length + 1, 1);
	if (!copy)
		return NULL;
	memcpy(copy, text, length);
	copy[length] = '\0';
	return copy;
}

bool crnet_head_add_header(
    const CR_ALLOCATOR *allocator,
    struct crnet_http_head *head,
    const char *name,
    size_t name_length,
    const char *value,
    size_t value_length) {
	// A name that is there already gets the new value joined onto its own.
	struct crnet_http_header_entry *entry = crnet_head_find(head, name, name_length);
	if (entry) {
		size_t old_length = entry->value_length;
		if (value_length > SIZE_MAX - 3 - old_length)
			return false;
		size_t joined_length = old_length + 2 + value_length;
		char  *joined        = (char *)cr_allocate(allocator, joined_length + 1, 1);
		if (!joined)
			return false;
		memcpy(joined, entry->value, old_length);
		memcpy(joined + old_length, ", ", 2);
		memcpy(joined + old_length + 2, value, value_length);
		joined[joined_length] = '\0';
		cr_free(allocator, entry->value, old_length + 1, 1);
		entry->value        = joined;
		entry->value_length = joined_length;
		return true;
	}

	crnet_head_bind(allocator, head);
	if (head->entries.count == head->entries.capacity) {
		// Room for twice as many entries, and a table twice that size (a
		// power of two, as 16 doubled is), filled again from the entries.
		// The entries are reserved exactly, not left to eqstd's own
		// growth, so the table stays twice their capacity.
		if (head->entries.capacity > CR_UINT32_MAX / 4)
			return false;
		uint32_t  capacity   = head->entries.capacity ? (uint32_t)head->entries.capacity * 2 : 16;
		uint32_t  slot_count = capacity * 2;
		uint32_t *slots      = CR_NEW_ARRAY_ZEROED(allocator, uint32_t, slot_count);
		if (!slots)
			return false;
		if (eq_array_reserve(&head->entries, capacity) != EQ_SUCCESS) {
			CR_DELETE_ARRAY(allocator, uint32_t, slots, slot_count);
			return false;
		}
		CR_DELETE_ARRAY(allocator, uint32_t, head->slots, head->slot_count);
		head->slots      = slots;
		head->slot_count = slot_count;
		for (uint32_t i = 0; i < head->entries.count; ++i)
			crnet_head_place(head, i);
	}
	char *name_copy  = crnet_head_copy(allocator, name, name_length);
	char *value_copy = crnet_head_copy(allocator, value, value_length);
	if (!name_copy || !value_copy) {
		cr_free(allocator, name_copy, name_length + 1, 1);
		cr_free(allocator, value_copy, value_length + 1, 1);
		return false;
	}
	// Room was made above, so the push can't fail; it is still checked.
	const struct crnet_http_header_entry added = {name_copy, name_length, value_copy, value_length};
	if (eq_array_push(&head->entries, &added) != EQ_SUCCESS) {
		cr_free(allocator, name_copy, name_length + 1, 1);
		cr_free(allocator, value_copy, value_length + 1, 1);
		return false;
	}
	crnet_head_place(head, (uint32_t)(head->entries.count - 1));
	return true;
}

bool crnet_head_add_header_line(
    const CR_ALLOCATOR *allocator,
    struct crnet_http_head *head,
    const char *line,
    size_t length) {
	while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n'))
		length--;

	if (length >= 5 && crnet_ascii_equal_no_case(line, "HTTP/", 5)) {
		crnet_head_drop_headers(allocator, head);
		return true;
	}

	size_t colon = 0;
	while (colon < length && line[colon] != ':')
		colon++;
	if (colon == 0 || colon == length)
		return true; // blank line, or nothing a header looks like

	const char *value        = line + colon + 1;
	size_t      value_length = length - colon - 1;
	while (value_length > 0 && (*value == ' ' || *value == '\t')) {
		value++;
		value_length--;
	}
	while (value_length > 0 && (value[value_length - 1] == ' ' || value[value_length - 1] == '\t'))
		value_length--;

	return crnet_head_add_header(allocator, head, line, colon, value, value_length);
}

const char *crnet_head_get_header(const struct crnet_http_head *head, const char *name) {
	struct crnet_http_header_entry *entry = crnet_head_find(head, name, strlen(name));
	return entry ? entry->value : NULL;
}

// --------------------------------------------------------------------------
// What a request's headers may and may not say
// --------------------------------------------------------------------------

/* True when a header's name is `name`, without regard to case. */
static bool crnet_header_is(const crnet_header *header, const char *name) {
	size_t length = strlen(name);
	return strlen(header->name) == length && crnet_ascii_equal_no_case(header->name, name, length);
}

enum crnet_accept_encoding crnet_headers_accept_encoding(
    const crnet_header *headers,
    uint32_t header_count) {
	static const char identity[] = "identity";

	enum crnet_accept_encoding found = CRNET_ACCEPT_ENCODING_STACK;
	for (uint32_t i = 0; i < header_count; ++i) {
		if (!headers[i].name || !headers[i].value || !crnet_header_is(&headers[i], "Accept-Encoding"))
			continue;

		// The value with the blanks around it left off.
		const char *value  = headers[i].value;
		size_t      length = strlen(value);
		while (length > 0 && (*value == ' ' || *value == '\t')) {
			value++;
			length--;
		}
		while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '\t'))
			length--;
		if (length != sizeof(identity) - 1 || !crnet_ascii_equal_no_case(value, identity, length))
			return CRNET_ACCEPT_ENCODING_INVALID;
		found = CRNET_ACCEPT_ENCODING_IDENTITY;
	}
	return found;
}

/* Everything crnet.h has crnet_http_transfer_start refuse before
 * anything is sent. `*out_encoding` is what the headers say of the
 * response's encoding. */
static CR_RESULT crnet_http_desc_check(const crnet_http_transfer_desc *desc, enum crnet_accept_encoding *out_encoding) {
	*out_encoding = CRNET_ACCEPT_ENCODING_STACK;
	if (!desc->method || desc->method[0] == '\0' || !desc->url || (desc->header_count > 0 && !desc->headers))
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	for (uint32_t i = 0; i < desc->header_count; ++i) {
		if (!desc->headers[i].name || !desc->headers[i].value)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		// These two follow from the desc's body and are written for it.
		if (crnet_header_is(&desc->headers[i], "Content-Length") || crnet_header_is(&desc->headers[i], "Transfer-Encoding"))
			return CR_RESULT_DESC_INVALID_PARAMETERS;
	}
	*out_encoding = crnet_headers_accept_encoding(desc->headers, desc->header_count);
	if (*out_encoding == CRNET_ACCEPT_ENCODING_INVALID)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	switch (desc->body) {
	case CRNET_HTTP_BODY_SIZED:
		break;
	case CRNET_HTTP_BODY_NONE:
	case CRNET_HTTP_BODY_UNSIZED:
		// A size that would be ignored is refused instead.
		if (desc->body_size != 0)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		break;
	default:
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	}
	// A redirect may want the body sent again, and it has passed through.
	if (desc->follow_redirects && desc->body != CRNET_HTTP_BODY_NONE)
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	if (!crnet_url_has_scheme(desc->url, "http") && !crnet_url_has_scheme(desc->url, "https"))
		return CR_RESULT_NET_INVALID_URL;
	return CR_SUCCESS;
}

// --------------------------------------------------------------------------
// The transfer
// --------------------------------------------------------------------------

/* What a backend returned, after the transfer has been marked for it:
 * anything but success and a time-out, which every call may be tried again
 * after, leaves only destroy. */
static CR_RESULT crnet_http_settle(struct crnet_http_transfer_t *transfer, CR_RESULT result) {
	if (CR_FAILED(result) && result != CR_RESULT_NET_TIMED_OUT)
		transfer->phase = CRNET_HTTP_PHASE_FAILED;
	return result;
}

CR_RESULT crnet_http_transfer_create(
    crnet_http_client client,
    crnet_http_transfer *out_transfer) {
	if (!out_transfer)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_transfer = NULL;
	if (!client)
		return CR_RESULT_NET_INVALID_HTTP_CLIENT;
	if (CR_STALE(client, "crnet_http_transfer_create", "client"))
		return CR_RESULT_NET_INVALID_HTTP_CLIENT;

	// Every backend's client starts with its head (see crnetshared.h), so
	// the client's allocator is reached without knowing the backend's type.
	const CR_ALLOCATOR *allocator = &((const cr_object_head *)client)->allocator;

	struct crnet_http_transfer_t *transfer = CR_NEW_ZEROED(allocator, struct crnet_http_transfer_t);
	if (!transfer)
		return CR_RESULT_OUT_OF_MEMORY;
	transfer->head.allocator = *allocator;
	// Its client's owner, the instance, is its own (crstale.h).
	CR_BIRTH_IN(transfer, CR_HEAD_OF(client)->debug_quarantine);
	transfer->client         = client;
	transfer->phase          = CRNET_HTTP_PHASE_CREATED;
	CR_RESULT result        = crnet_http_backend_create(transfer);
	if (CR_FAILED(result)) {
		CR_DELETE(allocator, struct crnet_http_transfer_t, transfer);
		return result;
	}

	*out_transfer = transfer;
	return CR_SUCCESS;
}

void crnet_http_transfer_destroy(crnet_http_transfer transfer) {
	if (!transfer)
		return;
	if (CR_STALE(transfer, "crnet_http_transfer_destroy", "transfer"))
		return;
	crnet_http_backend_destroy(transfer);
	CR_KILL(transfer);
	const CR_ALLOCATOR allocator = transfer->head.allocator;
	crnet_head_clear(&allocator, &transfer->response);
	cr_string_free(&allocator, transfer->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(transfer), &allocator, struct crnet_http_transfer_t, transfer);
}

void crnet_http_transfer_abort(crnet_http_transfer transfer) {
	if (!transfer)
		return;
	if (CR_STALE(transfer, "crnet_http_transfer_abort", "transfer"))
		return;
	crnet_http_backend_abort(transfer);
}

void crnet_http_transfer_get_error(
    crnet_http_transfer transfer,
    crnet_error *out_error) {
	if (!transfer) {
		crnet_error_get(NULL, out_error);
		return;
	}
	if (CR_STALE(transfer, "crnet_http_transfer_get_error", "transfer")) {
		crnet_error_get(NULL, out_error);
		return;
	}
	crnet_error_get(&transfer->error, out_error);
}

CR_RESULT crnet_http_transfer_start(
    crnet_http_transfer transfer,
    const crnet_http_transfer_desc *desc) {
	if (!transfer)
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	if (CR_STALE(transfer, "crnet_http_transfer_start", "transfer"))
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (crnet_http_backend_aborted(transfer))
		return CR_RESULT_NET_ABORTED;
	if (transfer->phase != CRNET_HTTP_PHASE_CREATED)
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;

	enum crnet_accept_encoding encoding = CRNET_ACCEPT_ENCODING_STACK;
	CR_RESULT                   result   = crnet_http_desc_check(desc, &encoding);
	if (CR_FAILED(result))
		return result;

	transfer->body             = desc->body;
	transfer->body_size        = desc->body_size;
	transfer->body_sent        = 0;
	transfer->is_head          = strcmp(desc->method, "HEAD") == 0;
	transfer->follow_redirects = desc->follow_redirects;
	// Started from here whatever the backend says: out of time means the
	// request is under way, and the next call carries it on.
	transfer->phase            = CRNET_HTTP_PHASE_SENDING;
	return crnet_http_settle(transfer, crnet_http_backend_start(transfer, desc, encoding));
}

CR_RESULT crnet_http_transfer_send(
    crnet_http_transfer transfer,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_sent) {
	if (!out_sent)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_sent = 0;
	if (!transfer)
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	if (CR_STALE(transfer, "crnet_http_transfer_send", "transfer"))
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	if (crnet_http_backend_aborted(transfer))
		return CR_RESULT_NET_ABORTED;
	if (transfer->phase != CRNET_HTTP_PHASE_SENDING || transfer->body == CRNET_HTTP_BODY_NONE)
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;

	// A sized body takes what it announced and not a byte more.
	uint64_t room = CR_UINT64_MAX;
	if (transfer->body == CRNET_HTTP_BODY_SIZED) {
		room = transfer->body_size - transfer->body_sent;
		if (room == 0)
			return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	}
	if (!data || size == 0)
		return CR_RESULT_ZERO_SIZE;
	if (size > room)
		size = room;
	if (size > SIZE_MAX)
		size = SIZE_MAX;

	size_t     sent   = 0;
	CR_RESULT result = crnet_http_settle(transfer, crnet_http_backend_send(transfer, data, (size_t)size, timeout_ms, &sent));
	if (CR_FAILED(result))
		return result;
	transfer->body_sent += sent;
	*out_sent            = sent;
	return CR_SUCCESS;
}

CR_RESULT crnet_http_transfer_receive_response(
    crnet_http_transfer transfer,
    uint32_t timeout_ms) {
	if (!transfer)
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	if (CR_STALE(transfer, "crnet_http_transfer_receive_response", "transfer"))
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	if (crnet_http_backend_aborted(transfer))
		return CR_RESULT_NET_ABORTED;
	if (transfer->phase != CRNET_HTTP_PHASE_SENDING && transfer->phase != CRNET_HTTP_PHASE_AWAITING)
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	// A request that stops short of the length it announced can't be
	// finished. It isn't ended either: the rest can still be sent.
	if (transfer->phase == CRNET_HTTP_PHASE_SENDING && transfer->body == CRNET_HTTP_BODY_SIZED && transfer->body_sent < transfer->body_size)
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;

	transfer->phase   = CRNET_HTTP_PHASE_AWAITING;
	CR_RESULT result = crnet_http_settle(transfer, crnet_http_backend_receive_response(transfer, timeout_ms));
	if (!CR_FAILED(result))
		transfer->phase = CRNET_HTTP_PHASE_RESPONDED;
	return result;
}

uint32_t crnet_http_transfer_get_status(crnet_http_transfer transfer) {
	if (!transfer)
		return 0;
	if (CR_STALE(transfer, "crnet_http_transfer_get_status", "transfer"))
		return 0;
	return transfer->response.status;
}

const char *crnet_http_transfer_get_header(
    crnet_http_transfer transfer,
    const char *name) {
	if (!transfer || !name)
		return NULL;
	return crnet_head_get_header(&transfer->response, name);
}

uint32_t crnet_http_transfer_get_header_count(crnet_http_transfer transfer) {
	if (!transfer)
		return 0;
	if (CR_STALE(transfer, "crnet_http_transfer_get_header_count", "transfer"))
		return 0;
	return (uint32_t)transfer->response.entries.count;
}

CR_RESULT crnet_http_transfer_get_header_at(
    crnet_http_transfer transfer,
    uint32_t index,
    const char **out_name,
    const char **out_value) {
	if (!out_name || !out_value)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_name  = NULL;
	*out_value = NULL;
	if (!transfer)
		return CR_RESULT_INDEX_OUT_OF_RANGE;
	if (CR_STALE(transfer, "crnet_http_transfer_get_header_at", "transfer"))
		return CR_RESULT_INDEX_OUT_OF_RANGE;
	if (index >= transfer->response.entries.count)
		return CR_RESULT_INDEX_OUT_OF_RANGE;
	*out_name  = transfer->response.entries.data[index].name;
	*out_value = transfer->response.entries.data[index].value;
	return CR_SUCCESS;
}

CR_RESULT crnet_http_transfer_recv(
    crnet_http_transfer transfer,
    void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_received) {
	if (!out_received)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_received = 0;
	if (!transfer)
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	if (CR_STALE(transfer, "crnet_http_transfer_recv", "transfer"))
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	if (crnet_http_backend_aborted(transfer))
		return CR_RESULT_NET_ABORTED;
	// Past the body's end every call says so again.
	if (transfer->phase == CRNET_HTTP_PHASE_ENDED)
		return CR_SUCCESS;
	if (transfer->phase != CRNET_HTTP_PHASE_RESPONDED)
		return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
	if (!data || size == 0)
		return CR_RESULT_ZERO_SIZE;
	if (size > SIZE_MAX)
		size = SIZE_MAX;

	size_t     received = 0;
	CR_RESULT result   = crnet_http_settle(transfer, crnet_http_backend_recv(transfer, data, (size_t)size, timeout_ms, &received));
	if (CR_FAILED(result))
		return result;
	if (received == 0)
		transfer->phase = CRNET_HTTP_PHASE_ENDED;
	*out_received = received;
	return CR_SUCCESS;
}

uint32_t crnet_http_transfer_wait_prepare(struct crnet_http_transfer_t *transfer, uint32_t wait_for, struct crnet_wait_poll *poll) {
	// Every call on an ended, failed or aborted transfer returns at once.
	if (transfer->phase == CRNET_HTTP_PHASE_ENDED || transfer->phase == CRNET_HTTP_PHASE_FAILED || crnet_http_backend_aborted(transfer))
		return wait_for;
	return crnet_http_backend_wait_prepare(transfer, wait_for, poll) & wait_for;
}

#if __cplusplus
}
#endif
