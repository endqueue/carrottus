
#include "audio/craudiointernal.h"
#include <stddef.h>
#include "crallocator.h"
#include "crstructbase.h"

/* Platform-independent Carrottus Audio helpers. Everything that touches a native
 * audio API lives in src/audio/backends/. */

#if __cplusplus
extern "C" {
#endif

uint32_t craudio_sample_format_get_byte_size(CRAUDIO_SAMPLE_FORMAT fmt) {
	switch (fmt) {
	case CRAUDIO_SAMPLE_FORMAT_S16:
		return 2;
	case CRAUDIO_SAMPLE_FORMAT_S24_PACKED:
		return 3;
	case CRAUDIO_SAMPLE_FORMAT_S24_32_MSB:
	case CRAUDIO_SAMPLE_FORMAT_S24_32_LSB:
	case CRAUDIO_SAMPLE_FORMAT_S32:
	case CRAUDIO_SAMPLE_FORMAT_F32:
		return 4;
	default:
		return 0;
	}
}

bool craudio_format_has_layout(const CRAUDIO_FORMAT *format) {
	return format->positions[0] != CRAUDIO_CHANNEL_POSITION_UNSPECIFIED;
}

bool craudio_validate_positions(const CRAUDIO_FORMAT *format) {
	const uint32_t n = format->channel_count;
	if (!craudio_format_has_layout(format)) {
		for (uint32_t i = 1; i < n; ++i)
			if (format->positions[i] != CRAUDIO_CHANNEL_POSITION_UNSPECIFIED)
				return false; // partly specified
		return true;
	}
	bool seen[CRAUDIO_CHANNEL_POSITION_AUX_LAST + 1] = {false};
	for (uint32_t i = 0; i < n; ++i) {
		CRAUDIO_CHANNEL_POSITION p = format->positions[i];
		if (p == CRAUDIO_CHANNEL_POSITION_NA)
			continue; // the one position that may repeat
		bool speaker = p >= CRAUDIO_CHANNEL_POSITION_FL && p <= CRAUDIO_CHANNEL_POSITION_LAST_SPEAKER;
		bool aux     = p >= CRAUDIO_CHANNEL_POSITION_AUX0 && p <= CRAUDIO_CHANNEL_POSITION_AUX_LAST;
		bool mono    = p == CRAUDIO_CHANNEL_POSITION_MONO && n == 1;
		if ((!speaker && !aux && !mono) || seen[p])
			return false;
		seen[p] = true;
	}
	return true;
}

CR_RESULT craudio_validate_instance_desc(const CRAUDIO_INSTANCE_DESC *desc, craudio_instance *out_instance) {
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_AUDIO_INSTANCE_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_instance)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_instance = NULL;
	return cr_allocator_check(desc->allocator);
}

CR_RESULT craudio_validate_stream_desc(const CRAUDIO_STREAM_DESC *desc) {
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_AUDIO_STREAM_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!desc->device)
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	if (desc->format.channel_count == 0 || desc->format.channel_count > CRAUDIO_MAX_CHANNELS ||
	    desc->format.sample_rate == 0 ||
	    craudio_sample_format_get_byte_size(desc->format.sample_format) == 0)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (!craudio_validate_positions(&desc->format))
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->period_frames == 0)
		return CR_RESULT_AUDIO_PERIOD_OUT_OF_RANGE;
	if (desc->buffer_frames != 0 && desc->buffer_frames < desc->period_frames)
		return CR_RESULT_AUDIO_BUFFER_OUT_OF_RANGE;
	// Which mechanisms exist, and their parameters, are per-backend facts;
	// here only "was one chosen at all".
	if (desc->thread.priority == CRAUDIO_THREAD_PRIORITY_UNSPECIFIED || desc->thread.priority > CRAUDIO_THREAD_PRIORITY_PLATFORM)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	return CR_SUCCESS;
}

#if __cplusplus
}
#endif
