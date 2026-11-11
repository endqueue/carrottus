#pragma once

/* The WinHTTP backend's reading of a response's header block, on its own so
 * that tests/fuzz can drive it from bytes. */

#include "../../crnetshared.h"

#include <stddef.h>

/* The raw block WinHTTP gives for WINHTTP_QUERY_RAW_HEADERS_CRLF, `wide_count`
 * UTF-16 code units of it, into `head`: made UTF-8 and handed to
 * crnet_head_add_header_line a line at a time, in the order sent, each
 * without its '\n'. What follows the last '\n' is not a line and is left
 * out. Everything is made and freed through `allocator`. Fails with
 * CR_RESULT_OUT_OF_MEMORY, or CR_RESULT_NET_HTTP_FAILED for a block of
 * more than INT_MAX code units. */
CR_RESULT crnet_win_add_raw_headers(const CR_ALLOCATOR *allocator, const wchar_t *raw, size_t wide_count, struct crnet_http_head *head);
