#ifndef CARROTTUS_CRAUDIOINTERNAL_H
#define CARROTTUS_CRAUDIOINTERNAL_H

#include "craudio.h"

#if __cplusplus
extern "C" {
#endif

// The checks every backend's create makes of its desc, in this order, before
// anything else: CR_RESULT_NULL_DESC, the struct base's,
// CR_RESULT_NULL_OUTPUT_PARAMETER (and *out set to NULL from there on), then
// the allocator's (CR_RESULT_NULL_ALLOCATOR, CR_RESULT_INVALID_ALLOCATOR).
// The native API is each backend's to check, after these.
CR_RESULT craudio_validate_instance_desc(const CRAUDIO_INSTANCE_DESC *desc, craudio_instance *out_instance);

// Backend-independent CRAUDIO_STREAM_DESC checks shared by every
// backend's probe and create. Doesn't check `callback` (the probe ignores
// it) or whether buffer_frames == 0 is required/forbidden (that's a
// per-backend fact) — only the buffer >= period half.
CR_RESULT craudio_validate_stream_desc(const CRAUDIO_STREAM_DESC *desc);

// True when the format carries a channel layout (positions[0] isn't
// UNSPECIFIED). Only meaningful on a format that passed validation.
bool craudio_format_has_layout(const CRAUDIO_FORMAT *format);

// Last labeled (speaker) position; FL..this is the speaker range.
#define CRAUDIO_CHANNEL_POSITION_LAST_SPEAKER CRAUDIO_CHANNEL_POSITION_BRC

// The layout rules from craudio.h: all UNSPECIFIED, or all specified
// with no duplicates except NA, and MONO only on a single channel.
bool craudio_validate_positions(const CRAUDIO_FORMAT *format);

#if __cplusplus
}
#endif

#endif // CARROTTUS_CRAUDIOINTERNAL_H
