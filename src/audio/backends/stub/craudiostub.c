// Only when no native API is compiled in (see craudio.h).
#if !(CRAUDIO_COMPILE_WASAPI || CRAUDIO_COMPILE_ALSA || CRAUDIO_COMPILE_PIPEWIRE || CRAUDIO_COMPILE_COREAUDIO || CRAUDIO_COMPILE_REMOTEIO)

#include "../../craudiointernal.h"
#include <stddef.h>
#include "../../../crallocator.h"
#include "../../../crstructbase.h"

/* Placeholder backend for platforms with no native API compiled in yet
 * (currently watchOS). Argument checks match
 * what the real backends will do, so callers get the same error for a bad
 * call either way; anything past the checks reports
 * CR_RESULT_NOT_IMPLEMENTED_YET. Replace per-platform in CMakeLists.txt as
 * each backend lands. */

#if __cplusplus
extern "C" {
#endif

CR_RESULT craudio_instance_create(
    const CRAUDIO_INSTANCE_DESC *desc,
    craudio_instance *out_instance) {
	const CR_RESULT desc_result = craudio_validate_instance_desc(desc, out_instance);
	if (desc_result != CR_SUCCESS)
		return desc_result;
	if (desc->native_api == CRAUDIO_NATIVE_API_NONE)
		return CR_RESULT_INVALID_API;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT craudio_instance_destroy(craudio_instance instance) {
	if (!instance)
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	if (CR_STALE(instance, "craudio_instance_destroy", "instance"))
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CRAUDIO_NATIVE_API craudio_instance_get_native_api(craudio_instance instance) {
	(void)instance;
	return CRAUDIO_NATIVE_API_NONE;
}

CR_RESULT craudio_device_enumerate(
    craudio_instance instance,
    CRAUDIO_DIRECTION direction,
    craudio_device **out_devices,
    uint32_t *out_device_count) {
	if (!out_devices || !out_device_count)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_devices      = NULL;
	*out_device_count = 0;
	if (!instance)
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	if (CR_STALE(instance, "craudio_device_enumerate", "instance"))
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	if (direction != CRAUDIO_DIRECTION_OUTPUT && direction != CRAUDIO_DIRECTION_INPUT)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT craudio_device_get_properties(
    craudio_device device,
    CRAUDIO_DEVICE_PROPERTIES *out_properties) {
	if (!out_properties)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_sr = cr_struct_out_check(&out_properties->base, CR_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!device)
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	if (CR_STALE(device, "craudio_device_get_properties", "device"))
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT craudio_device_get_timing_caps(
    craudio_device device,
    const CRAUDIO_FORMAT *format,
    CRAUDIO_STREAM_FLAGS flags,
    CRAUDIO_TIMING_CAPS *out_caps) {
	(void)flags;
	if (!out_caps)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_sr = cr_struct_out_check(&out_caps->base, CR_STRUCT_TYPE_AUDIO_TIMING_CAPS);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	*out_caps = (CRAUDIO_TIMING_CAPS){0};
	out_caps->base = cr_struct_base_out(CR_STRUCT_TYPE_AUDIO_TIMING_CAPS);
	if (!format)
		return CR_RESULT_NULL_DESC;
	if (!device)
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	if (CR_STALE(device, "craudio_device_get_timing_caps", "device"))
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT craudio_stream_probe(
    const CRAUDIO_STREAM_DESC *desc,
    CRAUDIO_STREAM_CONFIG *out_config) {
	if (!out_config)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_sr = cr_struct_out_check(&out_config->base, CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	*out_config  = (CRAUDIO_STREAM_CONFIG){0};
	out_config->base = cr_struct_base_out(CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	CR_RESULT r = craudio_validate_stream_desc(desc);
	if (CR_FAILED(r))
		return r;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT craudio_stream_create(
    const CRAUDIO_STREAM_DESC *desc,
    craudio_stream *out_stream) {
	if (!out_stream)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_stream  = NULL;
	CR_RESULT r = craudio_validate_stream_desc(desc);
	if (CR_FAILED(r))
		return r;
	if (!desc->callback)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

void craudio_stream_destroy(craudio_stream stream) { (void)stream; }

CR_RESULT craudio_stream_start(craudio_stream stream) {
	if (!stream)
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (CR_STALE(stream, "craudio_stream_start", "stream"))
		return CR_RESULT_AUDIO_INVALID_STREAM;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT craudio_stream_stop(craudio_stream stream) {
	if (!stream)
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (CR_STALE(stream, "craudio_stream_stop", "stream"))
		return CR_RESULT_AUDIO_INVALID_STREAM;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT craudio_stream_get_config(
    craudio_stream stream,
    CRAUDIO_STREAM_CONFIG *out_config) {
	if (!out_config)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT base_sr = cr_struct_out_check(&out_config->base, CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!stream)
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (CR_STALE(stream, "craudio_stream_get_config", "stream"))
		return CR_RESULT_AUDIO_INVALID_STREAM;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT craudio_stream_get_latency_frames(
    craudio_stream stream,
    uint32_t *out_latency_frames) {
	if (!out_latency_frames)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!stream)
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (CR_STALE(stream, "craudio_stream_get_latency_frames", "stream"))
		return CR_RESULT_AUDIO_INVALID_STREAM;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT craudio_stream_get_status(
    craudio_stream stream,
    CRAUDIO_STREAM_STATUS *out_status) {
	if (!out_status)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!stream)
		return CR_RESULT_AUDIO_INVALID_STREAM;
	if (CR_STALE(stream, "craudio_stream_get_status", "stream"))
		return CR_RESULT_AUDIO_INVALID_STREAM;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT craudio_device_set_sample_rate(craudio_device device, uint32_t sample_rate) {
	if (!device)
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	if (CR_STALE(device, "craudio_device_set_sample_rate", "device"))
		return CR_RESULT_AUDIO_INVALID_DEVICE;
	if (sample_rate == 0)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

CR_RESULT craudio_instance_set_device_event_callback(
    craudio_instance instance,
    CRAUDIO_DEVICE_EVENT_CALLBACK callback,
    void *user_data) {
	(void)callback;
	(void)user_data;
	if (!instance)
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	if (CR_STALE(instance, "craudio_instance_set_device_event_callback", "instance"))
		return CR_RESULT_AUDIO_INVALID_INSTANCE;
	return CR_RESULT_NOT_IMPLEMENTED_YET;
}

#if __cplusplus
}
#endif

#else
typedef int craudio_stub_not_compiled; // a native backend is compiled in
#endif
