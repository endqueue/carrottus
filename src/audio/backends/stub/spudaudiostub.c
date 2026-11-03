// Only when no native API is compiled in (see spudaudio.h).
#if !(SPUDAUDIO_COMPILE_WASAPI || SPUDAUDIO_COMPILE_ALSA || SPUDAUDIO_COMPILE_PIPEWIRE || SPUDAUDIO_COMPILE_COREAUDIO || SPUDAUDIO_COMPILE_REMOTEIO)

#include "../../spudaudiointernal.h"
#include <stddef.h>
#include "../../../spudallocator.h"
#include "../../../spudstructbase.h"

/* Placeholder backend for platforms with no native API compiled in yet
 * (currently watchOS). Argument checks match
 * what the real backends will do, so callers get the same error for a bad
 * call either way; anything past the checks reports
 * SPUDRESULT_NOT_IMPLEMENTED_YET. Replace per-platform in CMakeLists.txt as
 * each backend lands. */

#if __cplusplus
extern "C" {
#endif

SPUDRESULT spudaudio_create_instance(
    const SPUDAUDIO_INSTANCE_DESC *desc,
    spudaudio_instance *out_instance) {
	const SPUDRESULT desc_result = spudaudio_validate_instance_desc(desc, out_instance);
	if (desc_result != SPUD_SUCCESS)
		return desc_result;
	if (desc->native_api == SPUDAUDIO_NATIVE_API_NONE)
		return SPUDRESULT_INVALID_API;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDRESULT spudaudio_destroy_instance(spudaudio_instance instance) {
	if (!instance)
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudaudio_destroy_instance", "instance"))
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDAUDIO_NATIVE_API spudaudio_get_native_api(spudaudio_instance instance) {
	(void)instance;
	return SPUDAUDIO_NATIVE_API_NONE;
}

SPUDRESULT spudaudio_enumerate_devices(
    spudaudio_instance instance,
    SPUDAUDIO_DIRECTION direction,
    spudaudio_device **out_devices,
    uint32_t *out_device_count) {
	if (!out_devices || !out_device_count)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_devices      = NULL;
	*out_device_count = 0;
	if (!instance)
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudaudio_enumerate_devices", "instance"))
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (direction != SPUDAUDIO_DIRECTION_OUTPUT && direction != SPUDAUDIO_DIRECTION_INPUT)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDRESULT spudaudio_get_device_properties(
    spudaudio_device device,
    SPUDAUDIO_DEVICE_PROPERTIES *out_properties) {
	if (!out_properties)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_sr = spud_struct_out_check(&out_properties->base, SPUD_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!device)
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudaudio_get_device_properties", "device"))
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDRESULT spudaudio_get_timing_caps(
    spudaudio_device device,
    const SPUDAUDIO_FORMAT *format,
    SPUDAUDIO_STREAM_FLAGS flags,
    SPUDAUDIO_TIMING_CAPS *out_caps) {
	(void)flags;
	if (!out_caps)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_sr = spud_struct_out_check(&out_caps->base, SPUD_STRUCT_TYPE_AUDIO_TIMING_CAPS);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	*out_caps = (SPUDAUDIO_TIMING_CAPS){0};
	out_caps->base = spud_struct_base_out(SPUD_STRUCT_TYPE_AUDIO_TIMING_CAPS);
	if (!format)
		return SPUDRESULT_NULL_DESC;
	if (!device)
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudaudio_get_timing_caps", "device"))
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDRESULT spudaudio_probe_stream(
    const SPUDAUDIO_STREAM_DESC *desc,
    SPUDAUDIO_STREAM_CONFIG *out_config) {
	if (!out_config)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_sr = spud_struct_out_check(&out_config->base, SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	*out_config  = (SPUDAUDIO_STREAM_CONFIG){0};
	out_config->base = spud_struct_base_out(SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	SPUDRESULT r = spudaudio_validate_stream_desc(desc);
	if (SPUDFAIL(r))
		return r;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDRESULT spudaudio_create_stream(
    const SPUDAUDIO_STREAM_DESC *desc,
    spudaudio_stream *out_stream) {
	if (!out_stream)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_stream  = NULL;
	SPUDRESULT r = spudaudio_validate_stream_desc(desc);
	if (SPUDFAIL(r))
		return r;
	if (!desc->callback)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

void spudaudio_destroy_stream(spudaudio_stream stream) { (void)stream; }

SPUDRESULT spudaudio_stream_start(spudaudio_stream stream) {
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_start", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDRESULT spudaudio_stream_stop(spudaudio_stream stream) {
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_stop", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDRESULT spudaudio_stream_get_config(
    spudaudio_stream stream,
    SPUDAUDIO_STREAM_CONFIG *out_config) {
	if (!out_config)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_sr = spud_struct_out_check(&out_config->base, SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_get_config", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDRESULT spudaudio_stream_get_latency_frames(
    spudaudio_stream stream,
    uint32_t *out_latency_frames) {
	if (!out_latency_frames)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_get_latency_frames", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDRESULT spudaudio_stream_get_status(
    spudaudio_stream stream,
    SPUDAUDIO_STREAM_STATUS *out_status) {
	if (!out_status)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_get_status", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDRESULT spudaudio_set_device_sample_rate(spudaudio_device device, uint32_t sample_rate) {
	if (!device)
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudaudio_set_device_sample_rate", "device"))
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (sample_rate == 0)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

SPUDRESULT spudaudio_set_device_event_callback(
    spudaudio_instance instance,
    SPUDAUDIO_DEVICE_EVENT_CALLBACK callback,
    void *user_data) {
	(void)callback;
	(void)user_data;
	if (!instance)
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudaudio_set_device_event_callback", "instance"))
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

#if __cplusplus
}
#endif

#else
typedef int spudaudio_stub_not_compiled; // a native backend is compiled in
#endif
