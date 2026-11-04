#if SPUDAUDIO_COMPILE_WASAPI

#include "../../spudaudiointernal.h"
#include "../../../spudallocator.h"
#include "../../../spudstructbase.h"

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <process.h>
#include <propidl.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if __cplusplus
extern "C" {
#endif

/*
 * COM: the instance holds a CoIncrementMTAUsage cookie for its lifetime,
 * which keeps the process MTA alive regardless of which threads come and
 * go. Threads that never called CoInitializeEx are then implicitly in the
 * MTA, so SpudAudio calls work from any of them without SpudAudio
 * initializing COM per call or per thread. A thread the caller already put
 * in an STA (e.g. SDL's main thread after OleInitialize) keeps its STA —
 * the MMDevice/IAudioClient objects used here are agile.
 */

/*
 * The GUIDs this backend uses, defined here rather than taken from the SDK:
 * whether the SDK headers define them or only declare them (leaving them to
 * uuid.lib / ksguid.lib / libksguid.a) differs between MSVC and mingw-w64,
 * and the KSDATAFORMAT subtypes stay undefined under mingw even with
 * initguid.h. Values from mmdeviceapi.h / audioclient.h / ksmedia.h.
 */
static const GUID spudaudio_clsid_mmdevice_enumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const GUID spudaudio_iid_immdevice_enumerator  = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const GUID spudaudio_iid_immendpoint           = {0x1be09788, 0x6894, 0x4089, {0x85, 0x86, 0x9a, 0x2a, 0x6c, 0x26, 0x5a, 0xc5}};
static const GUID spudaudio_iid_immnotificationclient = {0x7991eec9, 0x7e89, 0x4d85, {0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0}};
static const GUID spudaudio_iid_iunknown              = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID spudaudio_iid_iaudioclient          = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const GUID spudaudio_iid_iaudioclient3         = {0x7ed4ee07, 0x8e67, 0x4cd4, {0x8c, 0x1a, 0x2b, 0x7a, 0x59, 0x87, 0xad, 0x42}};
static const GUID spudaudio_iid_iaudiorenderclient    = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
static const GUID spudaudio_iid_iaudiocaptureclient   = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
static const GUID spudaudio_iid_iaudioclock           = {0xcd63314f, 0x3fba, 0x4a1b, {0x81, 0x2c, 0xef, 0x96, 0x35, 0x87, 0x28, 0xe7}};
static const GUID spudaudio_subtype_pcm               = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID spudaudio_subtype_ieee_float        = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// functiondiscoverykeys_devpkey.h
static const PROPERTYKEY spudaudio_pkey_device_friendly_name = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

struct spudaudio_device_t {
	spud_object_head head;                 // allocator: a copy of its instance's
	struct spudaudio_instance_t *instance; // for its enumerator
	IMMDevice *mm_device;
	SPUDAUDIO_DIRECTION direction;
	char persistent_id[256];
	struct spudaudio_device_t *next; // instance-owned list
};

/*
 * A created stream: an Initialize()d, event-driven IAudioClient that hasn't
 * been started, plus the services the audio thread will need. Status
 * fields are written by the audio thread (once it exists) and read from
 * any thread, so they're only touched with Interlocked* — lock-free, per
 * spudaudio_stream_get_status's contract.
 */
struct spudaudio_stream_t {
	spud_object_head head; // allocator: a copy of its device's
	IAudioClient *client;
	IAudioRenderClient *render;   // OUTPUT streams
	IAudioCaptureClient *capture; // INPUT streams
	IAudioClock *clock;           // device position + QPC for SPUDAUDIO_CALLBACK_INFO
	HANDLE event;                 // signalled by WASAPI each period
	SPUDAUDIO_DIRECTION direction;
	SPUDAUDIO_STREAM_CONFIG config;
	SPUDAUDIO_STREAM_CALLBACK callback;
	void *user_data;
	bool exclusive;
	UINT32 buffer_size;     // IAudioClient::GetBufferSize, frames
	UINT64 clock_frequency; // IAudioClock::GetFrequency, position units/s
	uint32_t block_align;   // bytes per frame
	BYTE *silence;          // capture: zeroed stand-in for SILENT packets, buffer_size frames

	SPUDAUDIO_THREAD_PRIORITY priority;
	WCHAR mmcss_task[SPUDAUDIO_MAX_MMCSS_TASK_NAME];
	int32_t mmcss_relative_priority;

	// Audio thread. Only start/stop/destroy touch these (the caller
	// serializes those), except startup_result, written by the thread
	// before it signals startup_event.
	HANDLE thread;
	// The audio thread's id, stored by the thread itself before its first
	// callback (not by _beginthreadex's out-param, which can land after the
	// thread has already called back) — 0 when no thread is running.
	volatile LONG audio_thread_id;
	HANDLE stop_event;    // manual-reset; set by stop
	HANDLE startup_event; // auto-reset; thread -> start handshake
	SPUDRESULT startup_result;

	// Audio-thread-only state.
	uint64_t frames_written; // render: stream index of the next frame
	bool first_packet;       // capture: first packet since start

	volatile LONG state;  // SPUDAUDIO_STREAM_STATE
	volatile LONG error;  // SPUDRESULT
	volatile LONG64 underflow_count;
	volatile LONG64 overflow_count;
};

// One active endpoint as the device-change watch last saw it. The direction
// is kept because an endpoint that has gone can no longer be asked for it.
struct wasapi_endpoint {
	SPUDAUDIO_DIRECTION direction;
	char persistent_id[256];
};
typedef EQ_ARRAY(struct wasapi_endpoint) wasapi_endpoint_array;

/*
 * Device-change notification for one instance: exists while a callback is
 * set. It is the IMMNotificationClient registered with the instance's
 * enumerator, so it is a COM object - reference-counted, and freed by the
 * last Release, which may be MMDevice's and come after the watch stopped.
 *
 * WASAPI reports a plug or unplug as a state change that carries only the
 * new state, and may also report the same endpoint as added or removed. The
 * watch keeps the ids of the active endpoints so that each one is reported
 * once: ADDED when it becomes active and wasn't, REMOVED when it stops
 * being active and was. That record is the watch's own - never the
 * instance's device list, which belongs to the thread that enumerates.
 */
struct wasapi_watch {
	IMMNotificationClient client; // first, so the interface pointer is the watch
	volatile LONG refs;
	// A copy of the instance's, not a pointer to it: the last Release, which
	// frees the watch, may come after the instance has gone.
	SPUD_ALLOCATOR allocator;
	// eqstd's view of `allocator` (spud_eq_allocator), which `endpoints`
	// points at.
	EQ_ALLOCATOR eq_allocator;
	SPUDAUDIO_DEVICE_EVENT_CALLBACK callback;
	void *user_data;
	IMMDeviceEnumerator *enumerator; // the instance's; not used once stopped

	volatile LONG stopped;            // set by destroy: notifications do nothing from then on
	volatile LONG active;             // notifications inside the watch right now
	volatile LONG callback_thread_id; // the thread inside the caller's callback, 0 if none

	SRWLOCK lock;       // the fields below
	bool have_baseline; // changes before the baseline is taken are not events
	wasapi_endpoint_array endpoints;
};

struct spudaudio_instance_t {
	spud_object_head head;
	CO_MTA_USAGE_COOKIE mta_cookie;
	IMMDeviceEnumerator *enumerator;
	struct spudaudio_device_t *devices; // every endpoint ever enumerated
	spudaudio_device *enumerated[2];    // latest array per direction
	uint32_t enumerated_capacity[2];    // what each was allocated, and is freed, with
	struct wasapi_watch *watch;         // NULL while no device-event callback is set
};

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static SPUDRESULT wasapi_result(HRESULT hr) {
	switch (hr) {
	case S_OK:
		return SPUD_SUCCESS;
	case AUDCLNT_E_UNSUPPORTED_FORMAT:
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	case AUDCLNT_E_DEVICE_IN_USE:
	case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
		return SPUDRESULT_SAUD_EXCLUSIVE_UNAVAILABLE;
	case AUDCLNT_E_DEVICE_INVALIDATED:
		return SPUDRESULT_SAUD_DEVICE_LOST;
	case AUDCLNT_E_INVALID_DEVICE_PERIOD:
		return SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
	case E_OUTOFMEMORY:
		return SPUDRESULT_OUT_OF_MEMORY;
	case E_INVALIDARG:
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	default:
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
}

// Rounded to nearest, matching Microsoft's own frames -> REFERENCE_TIME
// conversion in the IAudioClient::Initialize alignment sample, so a
// frame count WASAPI handed back converts back to the same frame count.
static REFERENCE_TIME wasapi_frames_to_hns(uint32_t frames, uint32_t rate) {
	return (REFERENCE_TIME)(((uint64_t)frames * 10000000ULL + rate / 2) / rate);
}

// Rounded up: used for minimum periods, where rounding down would suggest
// a period the device rejects.
static uint32_t wasapi_hns_to_frames_ceil(REFERENCE_TIME hns, uint32_t rate) {
	return (uint32_t)(((uint64_t)hns * rate + 9999999ULL) / 10000000ULL);
}

/*
 * Channel positions <-> WAVEFORMATEXTENSIBLE.dwChannelMask. The mask can
 * only say *which* of the 18 speakers are present: channel order is
 * implicitly ascending bit order, and channels past the mask's bit count
 * are unassigned. So a layout maps to a mask only when it uses those 18
 * (plus MONO, WASAPI's mono layout = front center), in ascending bit
 * order, with any NA/AUX channels last as the unassigned ones. FL..TRR
 * are declared in bit order, so speaker bit = position - FL.
 */
#define WASAPI_BIT_UNASSIGNED (-1)     // NA / AUX: a trailing unassigned channel
#define WASAPI_BIT_UNREPRESENTABLE (-2) // a label outside the 18 speakers

static int wasapi_speaker_bit(SPUDAUDIO_CHANNEL_POSITION p) {
	if (p == SPUDAUDIO_CHANNEL_POSITION_MONO)
		return SPUDAUDIO_CHANNEL_POSITION_FC - SPUDAUDIO_CHANNEL_POSITION_FL;
	if (p >= SPUDAUDIO_CHANNEL_POSITION_FL && p <= SPUDAUDIO_CHANNEL_POSITION_TRR)
		return p - SPUDAUDIO_CHANNEL_POSITION_FL;
	if (p == SPUDAUDIO_CHANNEL_POSITION_NA || p >= SPUDAUDIO_CHANNEL_POSITION_AUX0)
		return WASAPI_BIT_UNASSIGNED;
	return WASAPI_BIT_UNREPRESENTABLE;
}

typedef enum WASAPI_MASK_RESULT {
	WASAPI_MASK_OK,
	WASAPI_MASK_WRONG_ORDER,   // fixable by reordering
	WASAPI_MASK_UNREPRESENTABLE // uses a label WASAPI has no bit for
} WASAPI_MASK_RESULT;

static WASAPI_MASK_RESULT wasapi_positions_to_mask(const SPUDAUDIO_FORMAT *f, DWORD *out_mask) {
	*out_mask = 0;
	if (!spudaudio_format_has_layout(f))
		return WASAPI_MASK_OK; // 0 = no speaker assignment (KSAUDIO_SPEAKER_DIRECTOUT)
	for (uint32_t i = 0; i < f->channel_count; ++i)
		if (wasapi_speaker_bit(f->positions[i]) == WASAPI_BIT_UNREPRESENTABLE)
			return WASAPI_MASK_UNREPRESENTABLE;
	int last_bit    = -1;
	bool unassigned = false;
	for (uint32_t i = 0; i < f->channel_count; ++i) {
		int bit = wasapi_speaker_bit(f->positions[i]);
		if (bit == WASAPI_BIT_UNASSIGNED) {
			unassigned = true;
			continue;
		}
		if (unassigned || bit <= last_bit)
			return WASAPI_MASK_WRONG_ORDER;
		*out_mask |= (DWORD)1 << bit;
		last_bit = bit;
	}
	return WASAPI_MASK_OK;
}

// The same positions reordered the way a mask can carry them: speakers in
// ascending bit order, then NA/AUX channels in their original order. Only
// for layouts that aren't WASAPI_MASK_UNREPRESENTABLE.
static void wasapi_canonical_positions(const SPUDAUDIO_FORMAT *f, SPUDAUDIO_FORMAT *out) {
	*out       = *f;
	uint32_t n = 0;
	for (int bit = 0; bit < 18; ++bit)
		for (uint32_t i = 0; i < f->channel_count; ++i)
			if (wasapi_speaker_bit(f->positions[i]) == bit)
				out->positions[n++] = f->positions[i];
	for (uint32_t i = 0; i < f->channel_count; ++i)
		if (wasapi_speaker_bit(f->positions[i]) == WASAPI_BIT_UNASSIGNED)
			out->positions[n++] = f->positions[i];
}

static void wasapi_mask_to_positions(DWORD mask, SPUDAUDIO_FORMAT *out) {
	memset(out->positions, 0, sizeof(out->positions));
	if (mask == 0)
		return; // no layout
	uint32_t n = 0;
	for (int bit = 0; bit < 18 && n < out->channel_count; ++bit)
		if (mask & ((DWORD)1 << bit))
			out->positions[n++] = (SPUDAUDIO_CHANNEL_POSITION)(SPUDAUDIO_CHANNEL_POSITION_FL + bit);
	for (uint32_t aux = 0; n < out->channel_count && n < SPUDAUDIO_MAX_CHANNELS; ++aux)
		out->positions[n++] = (SPUDAUDIO_CHANNEL_POSITION)(SPUDAUDIO_CHANNEL_POSITION_AUX0 + aux);
}

// FORMAT_NOT_SUPPORTED when WASAPI can't carry the format. out_suggestion
// gets the reordered layout when only the order was wrong, or the same
// format with no layout when a label has no WASAPI speaker bit.
static SPUDRESULT wasapi_fill_format(const SPUDAUDIO_FORMAT *f, WAVEFORMATEXTENSIBLE *w, SPUDAUDIO_FORMAT *out_suggestion) {
	WORD bits, valid_bits;
	const GUID *sub_format;
	switch (f->sample_format) {
	case SPUDAUDIO_SAMPLE_FORMAT_S16:
		bits = 16, valid_bits = 16, sub_format = &spudaudio_subtype_pcm;
		break;
	case SPUDAUDIO_SAMPLE_FORMAT_S24_PACKED:
		bits = 24, valid_bits = 24, sub_format = &spudaudio_subtype_pcm;
		break;
	case SPUDAUDIO_SAMPLE_FORMAT_S24_32_MSB:
		// WAVEFORMATEXTENSIBLE's valid bits are always the high-order bits
		// of the container.
		bits = 32, valid_bits = 24, sub_format = &spudaudio_subtype_pcm;
		break;
	case SPUDAUDIO_SAMPLE_FORMAT_S24_32_LSB:
		// No WASAPI layout puts 24 bits low in 32; its 24-in-32 is MSB.
		*out_suggestion               = *f;
		out_suggestion->sample_format = SPUDAUDIO_SAMPLE_FORMAT_S24_32_MSB;
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	case SPUDAUDIO_SAMPLE_FORMAT_S32:
		bits = 32, valid_bits = 32, sub_format = &spudaudio_subtype_pcm;
		break;
	case SPUDAUDIO_SAMPLE_FORMAT_F32:
		bits = 32, valid_bits = 32, sub_format = &spudaudio_subtype_ieee_float;
		break;
	default:
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	}
	DWORD mask                  = 0;
	WASAPI_MASK_RESULT mask_res = wasapi_positions_to_mask(f, &mask);
	if (mask_res == WASAPI_MASK_WRONG_ORDER) {
		wasapi_canonical_positions(f, out_suggestion);
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	}
	if (mask_res == WASAPI_MASK_UNREPRESENTABLE) {
		*out_suggestion = *f; // same format, layout dropped
		memset(out_suggestion->positions, 0, sizeof(out_suggestion->positions));
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	}

	memset(w, 0, sizeof(*w));
	w->Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
	w->Format.nChannels            = (WORD)f->channel_count;
	w->Format.nSamplesPerSec       = f->sample_rate;
	w->Format.wBitsPerSample       = bits;
	w->Format.nBlockAlign          = (WORD)(f->channel_count * bits / 8);
	w->Format.nAvgBytesPerSec      = f->sample_rate * w->Format.nBlockAlign;
	w->Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	w->Samples.wValidBitsPerSample = valid_bits;
	w->dwChannelMask               = mask;
	w->SubFormat                   = *sub_format;
	return SPUD_SUCCESS;
}

// Maps a WASAPI-supplied format (e.g. an IsFormatSupported closest match)
// back. sample_format is UNKNOWN when it has no SpudAudio equivalent.
static void wasapi_read_format(const WAVEFORMATEX *wf, SPUDAUDIO_FORMAT *out) {
	memset(out, 0, sizeof(*out));
	out->sample_rate   = wf->nSamplesPerSec;
	out->channel_count = wf->nChannels;
	out->interleaved   = true;

	bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
	bool is_pcm   = wf->wFormatTag == WAVE_FORMAT_PCM;
	WORD valid    = wf->wBitsPerSample;
	DWORD mask    = 0;
	if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
		const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wf;
		is_float                        = IsEqualGUID(&ext->SubFormat, &spudaudio_subtype_ieee_float);
		is_pcm                          = IsEqualGUID(&ext->SubFormat, &spudaudio_subtype_pcm);
		valid                           = ext->Samples.wValidBitsPerSample;
		mask                            = ext->dwChannelMask;
	}
	wasapi_mask_to_positions(mask, out);

	if (is_float && wf->wBitsPerSample == 32)
		out->sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
	else if (is_pcm && wf->wBitsPerSample == 16)
		out->sample_format = SPUDAUDIO_SAMPLE_FORMAT_S16;
	else if (is_pcm && wf->wBitsPerSample == 24 && valid == 24)
		out->sample_format = SPUDAUDIO_SAMPLE_FORMAT_S24_PACKED;
	else if (is_pcm && wf->wBitsPerSample == 32 && valid == 24)
		out->sample_format = SPUDAUDIO_SAMPLE_FORMAT_S24_32_MSB;
	else if (is_pcm && wf->wBitsPerSample == 32 && valid == 32)
		out->sample_format = SPUDAUDIO_SAMPLE_FORMAT_S32;
	else
		out->sample_format = SPUDAUDIO_SAMPLE_FORMAT_UNKNOWN;
}

static DWORD wasapi_stream_flags(SPUDAUDIO_STREAM_FLAGS flags) {
	DWORD f = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
	if (flags & SPUDAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION)
		f |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
	return f;
}

// Shared-mode format check. With ALLOW_OS_CONVERSION, Initialize itself
// accepts any PCM format, and IsFormatSupported doesn't account for that
// flag — so the check is skipped and Initialize answers instead.
static SPUDRESULT wasapi_check_shared_format(
    IAudioClient *client,
    const SPUDAUDIO_STREAM_DESC *desc,
    const WAVEFORMATEXTENSIBLE *wfx,
    SPUDAUDIO_STREAM_CONFIG *out_config) {
	if (desc->flags & SPUDAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION)
		return SPUD_SUCCESS;

	WAVEFORMATEX *closest = NULL;
	HRESULT hr            = IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, &wfx->Format, &closest);
	if (hr == S_OK)
		return SPUD_SUCCESS;
	if (hr == S_FALSE && closest) {
		wasapi_read_format(closest, &out_config->format);
		CoTaskMemFree(closest);
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	}
	if (closest)
		CoTaskMemFree(closest);
	return hr == S_FALSE ? SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED : wasapi_result(hr);
}

// Nearest multiple of `step` within [lo, hi]. Used only to build a
// suggestion from the range/step the OS reported — never applied.
static uint32_t wasapi_nearest_step(uint32_t v, uint32_t lo, uint32_t hi, uint32_t step) {
	if (step == 0)
		step = 1;
	uint32_t first = ((lo + step - 1) / step) * step;
	uint32_t last  = (hi / step) * step;
	if (first > last)
		return 0;
	if (v <= first)
		return first;
	if (v >= last)
		return last;
	uint32_t below = (v / step) * step;
	uint32_t above = below + step;
	return (v - below <= above - v) ? below : above;
}

// --------------------------------------------------------------------------
// Probe paths
// --------------------------------------------------------------------------

/*
 * Exclusive: IAudioClient::Initialize in event mode, where WASAPI requires
 * hnsBufferDuration == hnsPeriodicity. Buffer alignment is the one
 * constraint nothing reports up front: Initialize fails with
 * AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED and GetBufferSize then returns the
 * aligned frame count. Microsoft's guidance is to re-Initialize at that
 * size; here it becomes the suggestion instead, so the caller decides.
 */
static SPUDRESULT wasapi_probe_exclusive(
    IAudioClient *client,
    const SPUDAUDIO_STREAM_DESC *desc,
    const WAVEFORMATEXTENSIBLE *wfx,
    SPUDAUDIO_STREAM_CONFIG *out_config) {
	const uint32_t rate = desc->format.sample_rate;

	HRESULT hr = IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE, &wfx->Format, NULL);
	if (hr != S_OK)
		return hr == S_FALSE ? SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED : wasapi_result(hr);

	out_config->format      = desc->format;
	out_config->period_mode = SPUDAUDIO_PERIOD_MODE_EXACT;

	REFERENCE_TIME default_hns, min_hns;
	hr = IAudioClient_GetDevicePeriod(client, &default_hns, &min_hns);
	if (FAILED(hr))
		return wasapi_result(hr);
	uint32_t min_frames = wasapi_hns_to_frames_ceil(min_hns, rate);
	if (desc->period_frames < min_frames) {
		out_config->period_frames = min_frames;
		out_config->buffer_frames = min_frames;
		return SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
	}
	out_config->period_frames = desc->period_frames;

	if (desc->buffer_frames != desc->period_frames) {
		out_config->buffer_frames = desc->period_frames;
		return SPUDRESULT_SAUD_BUFFER_OUT_OF_RANGE;
	}

	REFERENCE_TIME hns = wasapi_frames_to_hns(desc->period_frames, rate);
	hr                 = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE, wasapi_stream_flags(desc->flags), hns, hns, &wfx->Format, NULL);
	if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
		UINT32 aligned = 0;
		if (SUCCEEDED(IAudioClient_GetBufferSize(client, &aligned))) {
			out_config->period_frames = aligned;
			out_config->buffer_frames = aligned;
		} else {
			out_config->period_frames = 0;
		}
		return SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
	}
	if (hr == AUDCLNT_E_BUFFER_SIZE_ERROR) {
		out_config->period_frames = 0; // too large; WASAPI offers no maximum
		return SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
	}
	if (FAILED(hr))
		return wasapi_result(hr);

	UINT32 granted = 0;
	hr             = IAudioClient_GetBufferSize(client, &granted);
	if (FAILED(hr))
		return wasapi_result(hr);
	out_config->period_frames       = granted;
	out_config->buffer_frames       = granted;
	out_config->max_callback_frames = granted;
	return granted == desc->period_frames ? SPUD_SUCCESS : SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
}

/*
 * Shared, IAudioClient3 (Windows 10 1607+): the period is requested in
 * frames, on a fundamental step, with no separate buffer. The engine
 * period is shared across the endpoint, so another client already running
 * it at a different period wins — that shows up as a granted period that
 * differs from the request, i.e. a rejection suggesting the engine's.
 */
static SPUDRESULT wasapi_probe_shared3(
    IAudioClient *client,
    IAudioClient3 *client3,
    const SPUDAUDIO_STREAM_DESC *desc,
    const WAVEFORMATEXTENSIBLE *wfx,
    SPUDAUDIO_STREAM_CONFIG *out_config) {
	SPUDRESULT r = wasapi_check_shared_format(client, desc, wfx, out_config);
	if (SPUDFAIL(r))
		return r;
	out_config->format      = desc->format;
	out_config->period_mode = SPUDAUDIO_PERIOD_MODE_EXACT;

	UINT32 default_p, fundamental, min_p, max_p;
	HRESULT hr = IAudioClient3_GetSharedModeEnginePeriod(client3, &wfx->Format, &default_p, &fundamental, &min_p, &max_p);
	if (FAILED(hr))
		return wasapi_result(hr);
	uint32_t step = fundamental ? fundamental : 1;
	if (desc->period_frames < min_p || desc->period_frames > max_p || desc->period_frames % step != 0) {
		out_config->period_frames = wasapi_nearest_step(desc->period_frames, min_p, max_p, step);
		return SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
	}
	out_config->period_frames = desc->period_frames;

	if (desc->buffer_frames != 0) {
		out_config->buffer_frames = 0; // no separate buffer on this path
		return SPUDRESULT_SAUD_BUFFER_OUT_OF_RANGE;
	}

	hr = IAudioClient3_InitializeSharedAudioStream(client3, wasapi_stream_flags(desc->flags), desc->period_frames, &wfx->Format, NULL);
	if (FAILED(hr))
		return wasapi_result(hr);

	WAVEFORMATEX *engine_format = NULL;
	UINT32 engine_period        = 0;
	hr                          = IAudioClient3_GetCurrentSharedModeEnginePeriod(client3, &engine_format, &engine_period);
	if (FAILED(hr))
		return wasapi_result(hr);
	// The engine period is in engine-rate frames; with OS conversion the
	// stream rate can differ.
	uint32_t engine_rate = engine_format ? engine_format->nSamplesPerSec : desc->format.sample_rate;
	if (engine_format)
		CoTaskMemFree(engine_format);
	uint32_t granted_period = (uint32_t)(((uint64_t)engine_period * desc->format.sample_rate + engine_rate / 2) / engine_rate);

	UINT32 buffer_size = 0;
	hr                 = IAudioClient_GetBufferSize(client, &buffer_size);
	if (FAILED(hr))
		return wasapi_result(hr);
	out_config->period_frames       = granted_period;
	out_config->max_callback_frames = buffer_size;
	return granted_period == desc->period_frames ? SPUD_SUCCESS : SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;
}

/*
 * Shared, plain IAudioClient (Windows before 10 1607): the period is fixed
 * at the engine's default; only the buffer duration is requested, and
 * WASAPI enlarges it as it sees fit — so a buffer_frames of 0 asks for its
 * minimum, and whatever GetBufferSize reports is the suggestion.
 */
static SPUDRESULT wasapi_probe_shared_legacy(
    IAudioClient *client,
    const SPUDAUDIO_STREAM_DESC *desc,
    const WAVEFORMATEXTENSIBLE *wfx,
    SPUDAUDIO_STREAM_CONFIG *out_config) {
	const uint32_t rate = desc->format.sample_rate;

	SPUDRESULT r = wasapi_check_shared_format(client, desc, wfx, out_config);
	if (SPUDFAIL(r))
		return r;
	out_config->format      = desc->format;
	out_config->period_mode = SPUDAUDIO_PERIOD_MODE_FIXED;

	REFERENCE_TIME default_hns, min_hns;
	HRESULT hr = IAudioClient_GetDevicePeriod(client, &default_hns, &min_hns);
	if (FAILED(hr))
		return wasapi_result(hr);
	uint32_t engine_frames    = wasapi_hns_to_frames_ceil(default_hns, rate);
	out_config->period_frames = engine_frames;
	if (desc->period_frames != engine_frames)
		return SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE;

	hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, wasapi_stream_flags(desc->flags), wasapi_frames_to_hns(desc->buffer_frames, rate), 0, &wfx->Format, NULL);
	if (hr == AUDCLNT_E_BUFFER_SIZE_ERROR)
		return SPUDRESULT_SAUD_BUFFER_OUT_OF_RANGE;
	if (FAILED(hr))
		return wasapi_result(hr);

	UINT32 granted = 0;
	hr             = IAudioClient_GetBufferSize(client, &granted);
	if (FAILED(hr))
		return wasapi_result(hr);
	out_config->buffer_frames       = granted;
	out_config->max_callback_frames = granted;
	return granted == desc->buffer_frames ? SPUD_SUCCESS : SPUDRESULT_SAUD_BUFFER_OUT_OF_RANGE;
}

// --------------------------------------------------------------------------
// Device-change notification
// --------------------------------------------------------------------------

// WASAPI's default-endpoint roles, as SpudAudio roles.
static const struct {
	ERole role;
	SPUDAUDIO_DEFAULT_ROLES spud_role;
} wasapi_roles[] = {
    {eConsole, SPUDAUDIO_DEFAULT_ROLE_GENERAL},
    {eMultimedia, SPUDAUDIO_DEFAULT_ROLE_MULTIMEDIA},
    {eCommunications, SPUDAUDIO_DEFAULT_ROLE_COMMUNICATIONS},
};

// An endpoint id as the UTF-8 persistent id, the same conversion as
// spudaudio_enumerate_devices.
static void wasapi_persistent_id(LPCWSTR wide_id, char *out, size_t out_size) {
	memset(out, 0, out_size);
	WideCharToMultiByte(CP_UTF8, 0, wide_id, -1, out, (int)out_size - 1, NULL, NULL);
}

// Direction and state of the endpoint with this id. False if it can't be
// asked - it has been removed. Called from notifications, where it takes
// and releases an IMMDevice of its own; the enumerator outlives it.
static bool wasapi_endpoint_info(IMMDeviceEnumerator *enumerator, LPCWSTR wide_id, SPUDAUDIO_DIRECTION *out_direction, DWORD *out_state) {
	IMMDevice *mm_device = NULL;
	if (FAILED(IMMDeviceEnumerator_GetDevice(enumerator, wide_id, &mm_device)))
		return false;
	IMMEndpoint *endpoint = NULL;
	EDataFlow flow        = eRender;
	bool ok               = SUCCEEDED(IMMDevice_QueryInterface(mm_device, &spudaudio_iid_immendpoint, (void **)&endpoint));
	if (ok) {
		ok = SUCCEEDED(IMMEndpoint_GetDataFlow(endpoint, &flow));
		IMMEndpoint_Release(endpoint);
	}
	if (ok)
		ok = SUCCEEDED(IMMDevice_GetState(mm_device, out_state));
	IMMDevice_Release(mm_device);
	*out_direction = flow == eRender ? SPUDAUDIO_DIRECTION_OUTPUT : SPUDAUDIO_DIRECTION_INPUT;
	return ok;
}

// Index of an endpoint in the watch's record, or -1. Lock held. Endpoint
// ids are unique across both directions.
static int wasapi_watch_find(const struct wasapi_watch *w, const char *id) {
	for (size_t i = 0; i < w->endpoints.count; ++i)
		if (strcmp(w->endpoints.data[i].persistent_id, id) == 0)
			return (int)i;
	return -1;
}

// Lock held. False if the record couldn't grow. Called on MMDevice's
// notification threads as well as the caller's, so the allocator is called
// there too.
static bool wasapi_watch_add(struct wasapi_watch *w, const char *id, SPUDAUDIO_DIRECTION direction) {
	struct wasapi_endpoint e;
	memset(&e, 0, sizeof(e));
	e.direction = direction;
	strncpy(e.persistent_id, id, sizeof(e.persistent_id) - 1);
	return eq_array_push(&w->endpoints, &e) == EQ_SUCCESS;
}

// A notification enters the watch. False once the watch has stopped; the
// count is what destroy waits on, so every true is paired with a leave.
static bool wasapi_watch_enter(struct wasapi_watch *w) {
	InterlockedIncrement(&w->active);
	if (InterlockedCompareExchange(&w->stopped, 0, 0)) {
		InterlockedDecrement(&w->active);
		return false;
	}
	return true;
}

static void wasapi_watch_leave(struct wasapi_watch *w) {
	InterlockedDecrement(&w->active);
}

// The caller's callback, with no lock held.
static void wasapi_watch_call(struct wasapi_watch *w, SPUDAUDIO_DEVICE_EVENT event, const char *id, SPUDAUDIO_DIRECTION direction, SPUDAUDIO_DEFAULT_ROLES roles) {
	InterlockedExchange(&w->callback_thread_id, (LONG)GetCurrentThreadId());
	w->callback(event, id, direction, roles, w->user_data);
	InterlockedExchange(&w->callback_thread_id, 0);
}

// An endpoint is now active, or now isn't. Reported only when that differs
// from the record, so the several notifications WASAPI can send for one
// plug or unplug come out as one event.
static void wasapi_watch_update(struct wasapi_watch *w, LPCWSTR wide_id, bool active, SPUDAUDIO_DIRECTION direction) {
	char id[256];
	wasapi_persistent_id(wide_id, id, sizeof(id));
	bool report = false;
	AcquireSRWLockExclusive(&w->lock);
	if (w->have_baseline) {
		const int at = wasapi_watch_find(w, id);
		if (active && at < 0) {
			// Not recorded means not reported: a later REMOVED would
			// otherwise have nothing to match.
			report = wasapi_watch_add(w, id, direction);
		} else if (!active && at >= 0) {
			direction = w->endpoints.data[at].direction;
			// An index inside the record, so the removal can't fail.
			(void)eq_array_remove_swap(&w->endpoints, (size_t)at);
			report = true;
		}
	}
	ReleaseSRWLockExclusive(&w->lock);
	if (report)
		wasapi_watch_call(w, active ? SPUDAUDIO_DEVICE_EVENT_ADDED : SPUDAUDIO_DEVICE_EVENT_REMOVED, id, direction, SPUDAUDIO_DEFAULT_ROLE_NONE);
}

static HRESULT STDMETHODCALLTYPE wasapi_watch_query_interface(IMMNotificationClient *self, REFIID iid, void **out_object) {
	if (!out_object)
		return E_POINTER;
	if (memcmp(iid, &spudaudio_iid_iunknown, sizeof(GUID)) != 0 && memcmp(iid, &spudaudio_iid_immnotificationclient, sizeof(GUID)) != 0) {
		*out_object = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&((struct wasapi_watch *)self)->refs);
	*out_object = self;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE wasapi_watch_add_ref(IMMNotificationClient *self) {
	return (ULONG)InterlockedIncrement(&((struct wasapi_watch *)self)->refs);
}

static ULONG STDMETHODCALLTYPE wasapi_watch_release(IMMNotificationClient *self) {
	struct wasapi_watch *w = (struct wasapi_watch *)self;
	const LONG refs        = InterlockedDecrement(&w->refs);
	if (refs == 0) {
		const SPUD_ALLOCATOR allocator = w->allocator;
		eq_array_free(&w->endpoints);
		SPUD_DELETE(&allocator, struct wasapi_watch, w);
	}
	return (ULONG)refs;
}

// The usual plug and unplug: the endpoint stays known and its state changes.
static HRESULT STDMETHODCALLTYPE wasapi_watch_on_state_changed(IMMNotificationClient *self, LPCWSTR wide_id, DWORD new_state) {
	struct wasapi_watch *w = (struct wasapi_watch *)self;
	if (!wide_id || !wasapi_watch_enter(w))
		return S_OK;
	SPUDAUDIO_DIRECTION direction = SPUDAUDIO_DIRECTION_OUTPUT;
	DWORD state                   = 0;
	if (new_state != DEVICE_STATE_ACTIVE)
		wasapi_watch_update(w, wide_id, false, direction);
	else if (wasapi_endpoint_info(w->enumerator, wide_id, &direction, &state))
		wasapi_watch_update(w, wide_id, true, direction);
	wasapi_watch_leave(w);
	return S_OK;
}

// A new endpoint (a driver was installed). It counts once it is active.
static HRESULT STDMETHODCALLTYPE wasapi_watch_on_added(IMMNotificationClient *self, LPCWSTR wide_id) {
	struct wasapi_watch *w = (struct wasapi_watch *)self;
	if (!wide_id || !wasapi_watch_enter(w))
		return S_OK;
	SPUDAUDIO_DIRECTION direction = SPUDAUDIO_DIRECTION_OUTPUT;
	DWORD state                   = 0;
	if (wasapi_endpoint_info(w->enumerator, wide_id, &direction, &state) && state == DEVICE_STATE_ACTIVE)
		wasapi_watch_update(w, wide_id, true, direction);
	wasapi_watch_leave(w);
	return S_OK;
}

// The endpoint no longer exists (a driver was uninstalled).
static HRESULT STDMETHODCALLTYPE wasapi_watch_on_removed(IMMNotificationClient *self, LPCWSTR wide_id) {
	struct wasapi_watch *w = (struct wasapi_watch *)self;
	if (!wide_id || !wasapi_watch_enter(w))
		return S_OK;
	wasapi_watch_update(w, wide_id, false, SPUDAUDIO_DIRECTION_OUTPUT);
	wasapi_watch_leave(w);
	return S_OK;
}

// One call per role. A NULL id is a role left with no endpoint, passed on
// as a NULL persistent id.
static HRESULT STDMETHODCALLTYPE wasapi_watch_on_default_changed(IMMNotificationClient *self, EDataFlow flow, ERole role, LPCWSTR wide_id) {
	struct wasapi_watch *w = (struct wasapi_watch *)self;
	if ((flow != eRender && flow != eCapture) || !wasapi_watch_enter(w))
		return S_OK;
	AcquireSRWLockShared(&w->lock);
	const bool have_baseline = w->have_baseline;
	ReleaseSRWLockShared(&w->lock);
	char id[256];
	if (wide_id)
		wasapi_persistent_id(wide_id, id, sizeof(id));
	for (size_t i = 0; have_baseline && i < sizeof(wasapi_roles) / sizeof(wasapi_roles[0]); ++i)
		if (wasapi_roles[i].role == role)
			wasapi_watch_call(
			    w, SPUDAUDIO_DEVICE_EVENT_DEFAULT_CHANGED, wide_id ? id : NULL, flow == eRender ? SPUDAUDIO_DIRECTION_OUTPUT : SPUDAUDIO_DIRECTION_INPUT,
			    wasapi_roles[i].spud_role);
	wasapi_watch_leave(w);
	return S_OK;
}

// Not a SpudAudio event.
static HRESULT STDMETHODCALLTYPE wasapi_watch_on_property_changed(IMMNotificationClient *self, LPCWSTR wide_id, const PROPERTYKEY key) {
	(void)self, (void)wide_id, (void)key;
	return S_OK;
}

static IMMNotificationClientVtbl wasapi_watch_vtbl = {
    .QueryInterface         = wasapi_watch_query_interface,
    .AddRef                 = wasapi_watch_add_ref,
    .Release                = wasapi_watch_release,
    .OnDeviceStateChanged   = wasapi_watch_on_state_changed,
    .OnDeviceAdded          = wasapi_watch_on_added,
    .OnDeviceRemoved        = wasapi_watch_on_removed,
    .OnDefaultDeviceChanged = wasapi_watch_on_default_changed,
    .OnPropertyValueChanged = wasapi_watch_on_property_changed,
};

/*
 * Records the active endpoints of both directions - what
 * spudaudio_enumerate_devices returns. The lock is held across the
 * enumeration: a notification either finishes first (and is ignored, with
 * the enumeration after it seeing its effect) or waits and is then compared
 * against the record. Without that a change could fall between the two.
 */
static SPUDRESULT wasapi_watch_take_baseline(struct wasapi_watch *w) {
	static const struct {
		EDataFlow flow;
		SPUDAUDIO_DIRECTION direction;
	} flows[] = {
	    {eRender, SPUDAUDIO_DIRECTION_OUTPUT},
	    {eCapture, SPUDAUDIO_DIRECTION_INPUT},
	};
	SPUDRESULT r = SPUD_SUCCESS;
	AcquireSRWLockExclusive(&w->lock);
	for (size_t f = 0; f < sizeof(flows) / sizeof(flows[0]) && SPUD_SUCCESS == r; ++f) {
		IMMDeviceCollection *collection = NULL;
		HRESULT hr                      = IMMDeviceEnumerator_EnumAudioEndpoints(w->enumerator, flows[f].flow, DEVICE_STATE_ACTIVE, &collection);
		if (FAILED(hr)) {
			r = wasapi_result(hr);
			break;
		}
		UINT count = 0;
		IMMDeviceCollection_GetCount(collection, &count);
		for (UINT i = 0; i < count && SPUD_SUCCESS == r; ++i) {
			IMMDevice *mm_device = NULL;
			if (FAILED(IMMDeviceCollection_Item(collection, i, &mm_device)))
				continue; // removed between GetCount and Item
			LPWSTR wide_id = NULL;
			char id[256]   = {0};
			if (SUCCEEDED(IMMDevice_GetId(mm_device, &wide_id))) {
				wasapi_persistent_id(wide_id, id, sizeof(id));
				CoTaskMemFree(wide_id);
			}
			IMMDevice_Release(mm_device);
			if (wasapi_watch_find(w, id) < 0 && !wasapi_watch_add(w, id, flows[f].direction))
				r = SPUDRESULT_OUT_OF_MEMORY;
		}
		IMMDeviceCollection_Release(collection);
	}
	w->have_baseline = SPUD_SUCCESS == r;
	ReleaseSRWLockExclusive(&w->lock);
	return r;
}

// Returns once the caller's callback is not running and will not run again.
// NULL is a no-op. The memory goes with the last reference, which MMDevice
// may hold a little longer.
static void wasapi_watch_destroy(struct wasapi_watch *w) {
	if (!w)
		return;
	InterlockedExchange(&w->stopped, 1);
	IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(w->enumerator, &w->client);
	// A notification that got in before `stopped` was set is still counted.
	while (InterlockedCompareExchange(&w->active, 0, 0) != 0)
		Sleep(1);
	IMMNotificationClient_Release(&w->client);
}

static SPUDRESULT wasapi_watch_create(spudaudio_instance instance, SPUDAUDIO_DEVICE_EVENT_CALLBACK callback, void *user_data, struct wasapi_watch **out_watch) {
	struct wasapi_watch *w = SPUD_NEW_ZEROED(&instance->head.allocator, struct wasapi_watch);
	if (!w)
		return SPUDRESULT_OUT_OF_MEMORY;
	w->client.lpVtbl = &wasapi_watch_vtbl;
	w->refs          = 1;
	w->allocator     = instance->head.allocator;
	w->eq_allocator  = spud_eq_allocator(&w->allocator);
	eq_array_init(&w->endpoints, &w->eq_allocator);
	w->callback      = callback;
	w->user_data     = user_data;
	w->enumerator    = instance->enumerator;
	InitializeSRWLock(&w->lock);

	// Registered first, baseline second: see wasapi_watch_take_baseline.
	HRESULT hr = IMMDeviceEnumerator_RegisterEndpointNotificationCallback(w->enumerator, &w->client);
	if (FAILED(hr)) {
		SPUD_DELETE(&instance->head.allocator, struct wasapi_watch, w);
		return wasapi_result(hr);
	}
	SPUDRESULT r = wasapi_watch_take_baseline(w);
	if (SPUDFAIL(r)) {
		wasapi_watch_destroy(w);
		return r;
	}
	*out_watch = w;
	return SPUD_SUCCESS;
}

// True on the instance's own device-event callback, where waiting for that
// callback to return would never end.
static bool wasapi_in_device_event_callback(spudaudio_instance instance) {
	return instance->watch && (DWORD)InterlockedCompareExchange(&instance->watch->callback_thread_id, 0, 0) == GetCurrentThreadId();
}

// --------------------------------------------------------------------------
// Instance / devices
// --------------------------------------------------------------------------

SPUDRESULT spudaudio_create_instance(
    const SPUDAUDIO_INSTANCE_DESC *desc,
    spudaudio_instance *out_instance) {
	const SPUDRESULT desc_result = spudaudio_validate_instance_desc(desc, out_instance);
	if (desc_result != SPUD_SUCCESS)
		return desc_result;
	if (desc->native_api != SPUDAUDIO_NATIVE_API_WASAPI)
		return SPUDRESULT_INVALID_API;

	struct spudaudio_instance_t *inst = SPUD_NEW_ZEROED(desc->allocator, struct spudaudio_instance_t);
	if (!inst)
		return SPUDRESULT_OUT_OF_MEMORY;
	inst->head.allocator = *desc->allocator;
	SPUD_BIRTH(inst);

	HRESULT hr = CoIncrementMTAUsage(&inst->mta_cookie);
	if (FAILED(hr)) {
		SPUD_DELETE(desc->allocator, struct spudaudio_instance_t, inst);
		return wasapi_result(hr);
	}
	hr = CoCreateInstance(&spudaudio_clsid_mmdevice_enumerator, NULL, CLSCTX_ALL, &spudaudio_iid_immdevice_enumerator, (void **)&inst->enumerator);
	if (FAILED(hr)) {
		CoDecrementMTAUsage(inst->mta_cookie);
		SPUD_DELETE(desc->allocator, struct spudaudio_instance_t, inst);
		return wasapi_result(hr);
	}
	*out_instance = inst;
	return SPUD_SUCCESS;
}

SPUDRESULT spudaudio_destroy_instance(spudaudio_instance instance) {
	if (!instance)
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudaudio_destroy_instance", "instance"))
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (wasapi_in_device_event_callback(instance))
		return SPUDRESULT_GENERAL_FAILURE;
	wasapi_watch_destroy(instance->watch);
	SPUD_KILL(instance);
	const SPUD_ALLOCATOR allocator = instance->head.allocator;
	struct spudaudio_device_t *dev = instance->devices;
	while (dev) {
		struct spudaudio_device_t *next = dev->next;
		SPUD_KILL(dev);
		IMMDevice_Release(dev->mm_device);
		spud_string_free(&allocator, dev->head.debug_name);
		SPUD_DELETE(&allocator, struct spudaudio_device_t, dev);
		dev = next;
	}
	SPUD_DELETE_ARRAY(&allocator, spudaudio_device, instance->enumerated[0], instance->enumerated_capacity[0]);
	SPUD_DELETE_ARRAY(&allocator, spudaudio_device, instance->enumerated[1], instance->enumerated_capacity[1]);
	IMMDeviceEnumerator_Release(instance->enumerator);
	CoDecrementMTAUsage(instance->mta_cookie);
	spud_string_free(&allocator, instance->head.debug_name);
	SPUD_DELETE(&allocator, struct spudaudio_instance_t, instance);
	return SPUD_SUCCESS;
}

SPUDAUDIO_NATIVE_API spudaudio_get_native_api(spudaudio_instance instance) {
	if (!instance)
		return SPUDAUDIO_NATIVE_API_NONE;
	if (SPUD_STALE(instance, "spudaudio_get_native_api", "instance"))
		return SPUDAUDIO_NATIVE_API_NONE;
	return SPUDAUDIO_NATIVE_API_WASAPI;
}

// Returns the instance's existing handle for this endpoint, or adds one, so
// handles stay stable across enumerations.
static struct spudaudio_device_t *wasapi_find_or_add_device(
    spudaudio_instance instance,
    IMMDevice *mm_device,
    const char *id,
    SPUDAUDIO_DIRECTION direction) {
	for (struct spudaudio_device_t *d = instance->devices; d; d = d->next)
		if (d->direction == direction && strcmp(d->persistent_id, id) == 0)
			return d;

	struct spudaudio_device_t *d = SPUD_NEW_ZEROED(&instance->head.allocator, struct spudaudio_device_t);
	if (!d)
		return NULL;
	IMMDevice_AddRef(mm_device);
	d->head.allocator = instance->head.allocator;
	SPUD_BIRTH(d);
	d->instance       = instance;
	d->mm_device      = mm_device;
	d->direction      = direction;
	strncpy(d->persistent_id, id, sizeof(d->persistent_id) - 1);
	d->next           = instance->devices;
	instance->devices = d;
	return d;
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

	IMMDeviceCollection *collection = NULL;
	HRESULT hr                      = IMMDeviceEnumerator_EnumAudioEndpoints(instance->enumerator, direction == SPUDAUDIO_DIRECTION_OUTPUT ? eRender : eCapture, DEVICE_STATE_ACTIVE, &collection);
	if (FAILED(hr))
		return wasapi_result(hr);

	UINT count = 0;
	IMMDeviceCollection_GetCount(collection, &count);
	spudaudio_device *list = count ? SPUD_NEW_ARRAY_ZEROED(&instance->head.allocator, spudaudio_device, count) : NULL;
	if (count && !list) {
		IMMDeviceCollection_Release(collection);
		return SPUDRESULT_OUT_OF_MEMORY;
	}

	uint32_t written = 0;
	SPUDRESULT r     = SPUD_SUCCESS;
	for (UINT i = 0; i < count; ++i) {
		IMMDevice *mm_device = NULL;
		if (FAILED(IMMDeviceCollection_Item(collection, i, &mm_device)))
			continue; // removed between GetCount and Item
		LPWSTR wide_id = NULL;
		char id[256]   = {0};
		if (SUCCEEDED(IMMDevice_GetId(mm_device, &wide_id))) {
			WideCharToMultiByte(CP_UTF8, 0, wide_id, -1, id, (int)sizeof(id) - 1, NULL, NULL);
			CoTaskMemFree(wide_id);
		}
		struct spudaudio_device_t *d = wasapi_find_or_add_device(instance, mm_device, id, direction);
		IMMDevice_Release(mm_device);
		if (!d) {
			r = SPUDRESULT_OUT_OF_MEMORY;
			break;
		}
		list[written++] = d;
	}
	IMMDeviceCollection_Release(collection);
	if (SPUDFAIL(r)) {
		SPUD_DELETE_ARRAY(&instance->head.allocator, spudaudio_device, list, count);
		return r;
	}

	SPUD_DELETE_ARRAY(&instance->head.allocator, spudaudio_device, instance->enumerated[direction], instance->enumerated_capacity[direction]);
	instance->enumerated[direction]          = list;
	instance->enumerated_capacity[direction] = count;
	*out_devices                             = list;
	*out_device_count                        = written;
	return SPUD_SUCCESS;
}

SPUDRESULT spudaudio_get_device_properties(
    spudaudio_device device,
    SPUDAUDIO_DEVICE_PROPERTIES *out_properties) {
	if (!out_properties)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_sr = spud_struct_out_check(&out_properties->base, SPUD_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	memset(out_properties, 0, sizeof(*out_properties));
	out_properties->base = spud_struct_base_out(SPUD_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES);
	if (!device)
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudaudio_get_device_properties", "device"))
		return SPUDRESULT_SAUD_INVALID_DEVICE;

	out_properties->direction          = device->direction;
	out_properties->supports_exclusive = true;
	memcpy(out_properties->persistent_id, device->persistent_id, sizeof(out_properties->persistent_id));

	// Friendly name. A missing name isn't an error — it's left empty.
	IPropertyStore *store = NULL;
	if (SUCCEEDED(IMMDevice_OpenPropertyStore(device->mm_device, STGM_READ, &store))) {
		PROPVARIANT value;
		PropVariantInit(&value);
		if (SUCCEEDED(IPropertyStore_GetValue(store, &spudaudio_pkey_device_friendly_name, &value)) && value.vt == VT_LPWSTR)
			WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, out_properties->name, (int)sizeof(out_properties->name) - 1, NULL, NULL);
		PropVariantClear(&value);
		IPropertyStore_Release(store);
	}

	// One default per WASAPI role. A role with no default at all
	// (E_NOTFOUND, e.g. no capture devices) just isn't held by this one.
	EDataFlow flow = device->direction == SPUDAUDIO_DIRECTION_OUTPUT ? eRender : eCapture;
	for (size_t i = 0; i < sizeof(wasapi_roles) / sizeof(wasapi_roles[0]); ++i) {
		IMMDevice *default_device = NULL;
		if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(device->instance->enumerator, flow, wasapi_roles[i].role, &default_device)))
			continue;
		LPWSTR wide_id = NULL;
		char id[256]   = {0};
		if (SUCCEEDED(IMMDevice_GetId(default_device, &wide_id))) {
			WideCharToMultiByte(CP_UTF8, 0, wide_id, -1, id, (int)sizeof(id) - 1, NULL, NULL);
			CoTaskMemFree(wide_id);
		}
		if (strcmp(id, device->persistent_id) == 0)
			out_properties->default_roles |= wasapi_roles[i].spud_role;
		IMMDevice_Release(default_device);
	}

	// Native format: the shared-mode engine's mix format.
	IAudioClient *client = NULL;
	HRESULT hr           = IMMDevice_Activate(device->mm_device, &spudaudio_iid_iaudioclient, CLSCTX_ALL, NULL, (void **)&client);
	if (FAILED(hr))
		return wasapi_result(hr);
	WAVEFORMATEX *mix_format = NULL;
	hr                       = IAudioClient_GetMixFormat(client, &mix_format);
	if (SUCCEEDED(hr)) {
		wasapi_read_format(mix_format, &out_properties->native_format);
		CoTaskMemFree(mix_format);
	}
	IAudioClient_Release(client);
	return wasapi_result(hr);
}

// --------------------------------------------------------------------------
// Timing / probe
// --------------------------------------------------------------------------

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
	memset(out_caps, 0, sizeof(*out_caps));
	out_caps->base = spud_struct_base_out(SPUD_STRUCT_TYPE_AUDIO_TIMING_CAPS);
	if (!format)
		return SPUDRESULT_NULL_DESC;
	if (!device)
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudaudio_get_timing_caps", "device"))
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	return SPUDRESULT_NOT_IMPLEMENTED_YET;
}

// WASAPI's own thread runs the callback, so NORMAL and MMCSS apply;
// SCHED_FIFO and PLATFORM are other backends' mechanisms.
static SPUDRESULT wasapi_check_thread(const SPUDAUDIO_THREAD_DESC *t) {
	switch (t->priority) {
	case SPUDAUDIO_THREAD_PRIORITY_NORMAL:
		return SPUD_SUCCESS;
	case SPUDAUDIO_THREAD_PRIORITY_MMCSS:
		if (!t->mmcss_task || !t->mmcss_task[0] || strlen(t->mmcss_task) >= SPUDAUDIO_MAX_MMCSS_TASK_NAME)
			return SPUDRESULT_DESC_INVALID_PARAMETERS;
		if (t->mmcss_relative_priority < AVRT_PRIORITY_VERYLOW || t->mmcss_relative_priority > AVRT_PRIORITY_CRITICAL)
			return SPUDRESULT_DESC_INVALID_PARAMETERS;
		return SPUD_SUCCESS;
	default:
		return SPUDRESULT_SAUD_THREAD_PRIORITY_UNSUPPORTED;
	}
}

/*
 * The one path that turns a desc into an Initialize()d IAudioClient, used
 * by both probe and create so creation can never accept something the
 * probe would reject (or vice versa). On success *out_client is the
 * initialized, unstarted client and out_config what was granted; on
 * failure *out_client is NULL and out_config holds the suggestion.
 */
static SPUDRESULT wasapi_open(
    const SPUDAUDIO_STREAM_DESC *desc,
    SPUDAUDIO_STREAM_CONFIG *out_config,
    IAudioClient **out_client) {
	*out_client = NULL;

	// AUTOCONVERTPCM is shared-mode only; exclusive Initialize would reject
	// it as a bare E_INVALIDARG.
	if ((desc->flags & SPUDAUDIO_STREAM_FLAG_EXCLUSIVE) && (desc->flags & SPUDAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION))
		return SPUDRESULT_SAUD_OS_CONVERSION_UNAVAILABLE;

	// WASAPI only carries interleaved PCM; it has no planar alternative to
	// suggest.
	if (!desc->format.interleaved)
		return SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED;
	WAVEFORMATEXTENSIBLE wfx;
	SPUDRESULT r = wasapi_fill_format(&desc->format, &wfx, &out_config->format);
	if (SPUDFAIL(r))
		return r;

	IAudioClient *client = NULL;
	HRESULT hr           = IMMDevice_Activate(desc->device->mm_device, &spudaudio_iid_iaudioclient, CLSCTX_ALL, NULL, (void **)&client);
	if (FAILED(hr))
		return wasapi_result(hr);

	if (desc->flags & SPUDAUDIO_STREAM_FLAG_EXCLUSIVE) {
		r = wasapi_probe_exclusive(client, desc, &wfx, out_config);
	} else {
		IAudioClient3 *client3 = NULL;
		if (SUCCEEDED(IAudioClient_QueryInterface(client, &spudaudio_iid_iaudioclient3, (void **)&client3))) {
			r = wasapi_probe_shared3(client, client3, desc, &wfx, out_config);
			IAudioClient3_Release(client3);
		} else {
			r = wasapi_probe_shared_legacy(client, desc, &wfx, out_config);
		}
	}
	if (SPUDFAIL(r)) {
		// Releasing a never-started client hands the device straight
		// back, including an exclusive-mode hold.
		IAudioClient_Release(client);
		return r;
	}
	*out_client = client;
	return SPUD_SUCCESS;
}

SPUDRESULT spudaudio_probe_stream(
    const SPUDAUDIO_STREAM_DESC *desc,
    SPUDAUDIO_STREAM_CONFIG *out_config) {
	if (!out_config)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT base_sr = spud_struct_out_check(&out_config->base, SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	memset(out_config, 0, sizeof(*out_config));
	out_config->base = spud_struct_base_out(SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	SPUDRESULT r = spudaudio_validate_stream_desc(desc);
	if (SPUDFAIL(r))
		return r;
	r = wasapi_check_thread(&desc->thread);
	if (SPUDFAIL(r))
		return r;

	IAudioClient *client = NULL;
	r                    = wasapi_open(desc, out_config, &client);
	if (client)
		IAudioClient_Release(client);
	return r;
}

// --------------------------------------------------------------------------
// Streams
// --------------------------------------------------------------------------

static void wasapi_set_terminal(struct spudaudio_stream_t *s, SPUDRESULT r) {
	InterlockedExchange(&s->error, (LONG)r);
	InterlockedExchange(&s->state, r == SPUDRESULT_SAUD_DEVICE_LOST ? SPUDAUDIO_STREAM_STATE_DEVICE_LOST : SPUDAUDIO_STREAM_STATE_ERROR);
}

static void wasapi_free_stream(struct spudaudio_stream_t *s) {
	if (s->clock)
		IAudioClock_Release(s->clock);
	if (s->render)
		IAudioRenderClient_Release(s->render);
	if (s->capture)
		IAudioCaptureClient_Release(s->capture);
	if (s->client)
		IAudioClient_Release(s->client);
	if (s->event)
		CloseHandle(s->event);
	if (s->stop_event)
		CloseHandle(s->stop_event);
	if (s->startup_event)
		CloseHandle(s->startup_event);
	const SPUD_ALLOCATOR allocator = s->head.allocator;
	// Bytes read as the stream's samples.
	spud_free_array(&allocator, s->silence, s->buffer_size, s->block_align, SPUD_MAX_ALIGN);
	spud_string_free(&allocator, s->head.debug_name);
	SPUD_DELETE(&allocator, struct spudaudio_stream_t, s);
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
	r = wasapi_check_thread(&desc->thread);
	if (SPUDFAIL(r))
		return r;

	struct spudaudio_stream_t *s = SPUD_NEW_ZEROED(&desc->device->head.allocator, struct spudaudio_stream_t);
	if (!s)
		return SPUDRESULT_OUT_OF_MEMORY;
	s->head.allocator          = desc->device->head.allocator;
	SPUD_BIRTH(s);
	s->direction               = desc->device->direction;
	s->callback                = desc->callback;
	s->user_data               = desc->user_data;
	s->exclusive               = (desc->flags & SPUDAUDIO_STREAM_FLAG_EXCLUSIVE) != 0;
	s->priority                = desc->thread.priority;
	s->mmcss_relative_priority = desc->thread.mmcss_relative_priority;
	s->state                   = SPUDAUDIO_STREAM_STATE_STOPPED;
	s->error                   = SPUD_SUCCESS;
	if (s->priority == SPUDAUDIO_THREAD_PRIORITY_MMCSS &&
	    !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, desc->thread.mmcss_task, -1, s->mmcss_task, SPUDAUDIO_MAX_MMCSS_TASK_NAME)) {
		SPUD_DELETE(&desc->device->head.allocator, struct spudaudio_stream_t, s);
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	}

	r = wasapi_open(desc, &s->config, &s->client);
	if (SPUDFAIL(r)) {
		wasapi_free_stream(s);
		return r;
	}
	s->block_align = s->config.format.channel_count * spudaudio_sample_format_byte_size(s->config.format.sample_format);

	// Event-driven mode (AUDCLNT_STREAMFLAGS_EVENTCALLBACK, set in every
	// Initialize path): WASAPI signals this auto-reset event once per
	// period; it must be registered before Start.
	s->event         = CreateEventW(NULL, FALSE, FALSE, NULL);
	s->stop_event    = CreateEventW(NULL, TRUE, FALSE, NULL);
	s->startup_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!s->event || !s->stop_event || !s->startup_event) {
		wasapi_free_stream(s);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	HRESULT hr = IAudioClient_SetEventHandle(s->client, s->event);
	if (SUCCEEDED(hr))
		hr = IAudioClient_GetBufferSize(s->client, &s->buffer_size);
	if (SUCCEEDED(hr)) {
		if (s->direction == SPUDAUDIO_DIRECTION_OUTPUT)
			hr = IAudioClient_GetService(s->client, &spudaudio_iid_iaudiorenderclient, (void **)&s->render);
		else
			hr = IAudioClient_GetService(s->client, &spudaudio_iid_iaudiocaptureclient, (void **)&s->capture);
	}
	if (SUCCEEDED(hr))
		hr = IAudioClient_GetService(s->client, &spudaudio_iid_iaudioclock, (void **)&s->clock);
	if (SUCCEEDED(hr))
		hr = IAudioClock_GetFrequency(s->clock, &s->clock_frequency);
	if (FAILED(hr)) {
		wasapi_free_stream(s);
		return wasapi_result(hr);
	}
	if (s->direction == SPUDAUDIO_DIRECTION_INPUT) {
		// Made now, so the audio thread never allocates; wasapi_free_stream
		// frees it with buffer_size and block_align, which don't change.
		s->silence = (BYTE *)spud_allocate_array(&s->head.allocator, s->buffer_size, s->block_align, SPUD_MAX_ALIGN, true);
		if (!s->silence) {
			wasapi_free_stream(s);
			return SPUDRESULT_OUT_OF_MEMORY;
		}
	}

	*out_stream = s;
	return SPUD_SUCCESS;
}

// --------------------------------------------------------------------------
// Audio thread
// --------------------------------------------------------------------------

static SPUDRESULT wasapi_apply_priority(struct spudaudio_stream_t *s, HANDLE *out_avrt) {
	*out_avrt = NULL;
	if (s->priority != SPUDAUDIO_THREAD_PRIORITY_MMCSS)
		return SPUD_SUCCESS;
	DWORD task_index = 0;
	HANDLE avrt      = AvSetMmThreadCharacteristicsW(s->mmcss_task, &task_index);
	if (!avrt)
		return SPUDRESULT_SAUD_THREAD_PRIORITY_DENIED; // e.g. ERROR_INVALID_TASK_NAME
	if (!AvSetMmThreadPriority(avrt, (AVRT_PRIORITY)s->mmcss_relative_priority)) {
		AvRevertMmThreadCharacteristics(avrt);
		return SPUDRESULT_SAUD_THREAD_PRIORITY_DENIED;
	}
	*out_avrt = avrt;
	return SPUD_SUCCESS;
}

/*
 * One render pass. Shared mode fills whatever the engine has room for
 * (buffer - padding); exclusive event mode always exchanges the whole
 * buffer. `started` is false for the prefill before IAudioClient::Start,
 * when there's no device clock yet.
 *
 * Underruns: WASAPI render has no glitch flag, so they're inferred — the
 * device position overtaking what was written (frames played that the
 * callback never produced), or, in shared mode, the engine having drained
 * the buffer completely (padding 0) between wakeups.
 */
static SPUDRESULT wasapi_render(struct spudaudio_stream_t *s, bool started) {
	const uint32_t rate = s->config.format.sample_rate;
	UINT32 frames       = s->buffer_size;
	bool drained        = false;
	HRESULT hr;
	if (!s->exclusive) {
		UINT32 padding = 0;
		hr             = IAudioClient_GetCurrentPadding(s->client, &padding);
		if (FAILED(hr))
			return wasapi_result(hr);
		frames = s->buffer_size - padding;
		// An empty buffer at wakeup means the engine consumed everything —
		// unless the buffer only holds one period, where that's every wakeup.
		drained = started && padding == 0 && s->buffer_size > s->config.period_frames;
	}
	if (frames == 0)
		return SPUD_SUCCESS;

	SPUDAUDIO_CALLBACK_INFO info = {0};
	if (started) {
		UINT64 position = 0, qpc_100ns = 0;
		if (SUCCEEDED(IAudioClock_GetPosition(s->clock, &position, &qpc_100ns)) && s->clock_frequency) {
			uint64_t played = position * rate / s->clock_frequency;
			if (played > s->frames_written) {
				s->frames_written = played; // skip the frames the device played without us
				drained           = true;
			}
			if (qpc_100ns) {
				// qpc_100ns is when `played` was the frame at the endpoint;
				// this buffer's first frame follows everything queued.
				info.host_time_ns = qpc_100ns * 100 + (s->frames_written - played) * 1000000000ULL / rate;
				info.flags |= SPUDAUDIO_CALLBACK_FLAG_HOST_TIME_VALID;
			}
		}
	}
	if (drained) {
		info.flags |= SPUDAUDIO_CALLBACK_FLAG_DISCONTINUITY;
		InterlockedIncrement64(&s->underflow_count);
	}
	info.device_position_frames = s->frames_written;

	BYTE *data = NULL;
	hr         = IAudioRenderClient_GetBuffer(s->render, frames, &data);
	if (FAILED(hr))
		return wasapi_result(hr);
	s->callback(s, data, frames, &info, s->user_data);
	hr = IAudioRenderClient_ReleaseBuffer(s->render, frames, 0);
	if (FAILED(hr))
		return wasapi_result(hr);
	s->frames_written += frames;
	return SPUD_SUCCESS;
}

/*
 * Drains every captured packet. WASAPI flags each one: DATA_DISCONTINUITY
 * (data was lost before it — except on the first packet after Start,
 * where it only marks the stream's beginning), SILENT (treat as silence:
 * the callback gets a zeroed buffer instead of the packet's bytes), and
 * TIMESTAMP_ERROR (the QPC time is unreliable).
 */
static SPUDRESULT wasapi_capture(struct spudaudio_stream_t *s) {
	for (;;) {
		UINT32 packet = 0;
		HRESULT hr    = IAudioCaptureClient_GetNextPacketSize(s->capture, &packet);
		if (FAILED(hr))
			return wasapi_result(hr);
		if (packet == 0)
			return SPUD_SUCCESS;

		BYTE *data          = NULL;
		UINT32 frames       = 0;
		DWORD flags         = 0;
		UINT64 position     = 0;
		UINT64 qpc_100ns    = 0;
		hr                  = IAudioCaptureClient_GetBuffer(s->capture, &data, &frames, &flags, &position, &qpc_100ns);
		if (hr == AUDCLNT_S_BUFFER_EMPTY)
			return SPUD_SUCCESS;
		if (FAILED(hr))
			return wasapi_result(hr);

		SPUDAUDIO_CALLBACK_INFO info = {0};
		info.device_position_frames  = position;
		if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && !s->first_packet) {
			info.flags |= SPUDAUDIO_CALLBACK_FLAG_DISCONTINUITY;
			InterlockedIncrement64(&s->overflow_count);
		}
		s->first_packet = false;
		if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpc_100ns) {
			info.host_time_ns = qpc_100ns * 100;
			info.flags |= SPUDAUDIO_CALLBACK_FLAG_HOST_TIME_VALID;
		}
		void *frames_ptr = data;
		if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
			memset(s->silence, 0, (size_t)frames * s->block_align);
			frames_ptr = s->silence;
			info.flags |= SPUDAUDIO_CALLBACK_FLAG_SILENT;
		}
		s->callback(s, frames_ptr, frames, &info, s->user_data);
		hr = IAudioCaptureClient_ReleaseBuffer(s->capture, frames);
		if (FAILED(hr))
			return wasapi_result(hr);
	}
}

static unsigned __stdcall wasapi_audio_thread(void *arg) {
	struct spudaudio_stream_t *s = (struct spudaudio_stream_t *)arg;
	InterlockedExchange(&s->audio_thread_id, (LONG)GetCurrentThreadId());

	// --- Startup, reported back to spudaudio_stream_start. RUNNING before
	// the prefill: it calls the callback, and RUNNING is the state while
	// the callback is called.
	HANDLE avrt  = NULL;
	SPUDRESULT r = wasapi_apply_priority(s, &avrt);
	InterlockedExchange(&s->state, SPUDAUDIO_STREAM_STATE_RUNNING);
	if (SPUD_SUCCESS == r && s->direction == SPUDAUDIO_DIRECTION_OUTPUT)
		r = wasapi_render(s, false); // prefill, so Start doesn't begin on silence
	if (SPUD_SUCCESS == r)
		r = wasapi_result(IAudioClient_Start(s->client));
	if (SPUDFAIL(r)) {
		if (r == SPUDRESULT_SAUD_DEVICE_LOST)
			wasapi_set_terminal(s, r);
		else
			InterlockedExchange(&s->state, SPUDAUDIO_STREAM_STATE_STOPPED);
		IAudioClient_Reset(s->client);
		if (avrt)
			AvRevertMmThreadCharacteristics(avrt);
		s->startup_result = r;
		SetEvent(s->startup_event);
		return 0;
	}
	s->startup_result = SPUD_SUCCESS;
	SetEvent(s->startup_event);

	// --- Run. The timeout keeps polling a device that stopped signalling,
	// which is how a vanished endpoint surfaces (DEVICE_INVALIDATED).
	HANDLE waits[2] = {s->stop_event, s->event};
	for (;;) {
		DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
		if (w == WAIT_OBJECT_0)
			break; // stop requested
		r = w == WAIT_FAILED ? SPUDRESULT_API_SPECIFIC_FAILURE
		    : s->direction == SPUDAUDIO_DIRECTION_OUTPUT ? wasapi_render(s, true)
		                                                 : wasapi_capture(s);
		if (SPUDFAIL(r)) {
			wasapi_set_terminal(s, r);
			break;
		}
	}

	// --- Stop. Reset discards queued data and rewinds the stream position
	// to 0, so a later start begins clean.
	IAudioClient_Stop(s->client);
	IAudioClient_Reset(s->client);
	InterlockedCompareExchange(&s->state, SPUDAUDIO_STREAM_STATE_STOPPED, SPUDAUDIO_STREAM_STATE_RUNNING);
	if (avrt)
		AvRevertMmThreadCharacteristics(avrt);
	return 0;
}

static bool wasapi_on_audio_thread(struct spudaudio_stream_t *s) {
	return (DWORD)InterlockedCompareExchange(&s->audio_thread_id, 0, 0) == GetCurrentThreadId();
}

static void wasapi_join_thread(struct spudaudio_stream_t *s) {
	WaitForSingleObject(s->thread, INFINITE);
	CloseHandle(s->thread);
	s->thread = NULL;
	InterlockedExchange(&s->audio_thread_id, 0);
}

SPUDRESULT spudaudio_stream_start(spudaudio_stream stream) {
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_start", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (wasapi_on_audio_thread(stream))
		return SPUDRESULT_GENERAL_FAILURE; // would deadlock
	LONG state = InterlockedCompareExchange(&stream->state, 0, 0);
	if (state == SPUDAUDIO_STREAM_STATE_RUNNING)
		return SPUD_SUCCESS;
	if (state == SPUDAUDIO_STREAM_STATE_DEVICE_LOST || state == SPUDAUDIO_STREAM_STATE_ERROR)
		return (SPUDRESULT)InterlockedCompareExchange(&stream->error, 0, 0);

	ResetEvent(stream->stop_event);
	stream->frames_written = 0;
	stream->first_packet   = true;
	uintptr_t thread       = _beginthreadex(NULL, 0, wasapi_audio_thread, stream, 0, NULL);
	if (!thread)
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	stream->thread = (HANDLE)thread;

	WaitForSingleObject(stream->startup_event, INFINITE);
	if (SPUDFAIL(stream->startup_result)) {
		wasapi_join_thread(stream);
		return stream->startup_result;
	}
	return SPUD_SUCCESS;
}

SPUDRESULT spudaudio_stream_stop(spudaudio_stream stream) {
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_stop", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (wasapi_on_audio_thread(stream))
		return SPUDRESULT_GENERAL_FAILURE; // would deadlock
	if (!stream->thread)
		return SPUD_SUCCESS; // never started, or already stopped
	SetEvent(stream->stop_event);
	wasapi_join_thread(stream);
	return SPUD_SUCCESS;
}

void spudaudio_destroy_stream(spudaudio_stream stream) {
	if (!stream)
		return;
	if (SPUD_STALE(stream, "spudaudio_destroy_stream", "stream"))
		return;
	spudaudio_stream_stop(stream);
	SPUD_KILL(stream);
	wasapi_free_stream(stream);
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
	*out_config = stream->config; // fixed at creation
	out_config->base = spud_struct_base_out(SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG);
	return SPUD_SUCCESS;
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
	// Interlocked reads: atomic with respect to the audio thread's writes.
	out_status->state           = (SPUDAUDIO_STREAM_STATE)InterlockedCompareExchange(&stream->state, 0, 0);
	out_status->error           = (SPUDRESULT)InterlockedCompareExchange(&stream->error, 0, 0);
	out_status->underflow_count = (uint64_t)InterlockedCompareExchange64(&stream->underflow_count, 0, 0);
	out_status->overflow_count  = (uint64_t)InterlockedCompareExchange64(&stream->overflow_count, 0, 0);
	return SPUD_SUCCESS;
}

// Stream latency (IAudioClient::GetStreamLatency — the engine/device part)
// plus, for output, the frames queued ahead of the next one written.
SPUDRESULT spudaudio_stream_get_latency_frames(
    spudaudio_stream stream,
    uint32_t *out_latency_frames) {
	if (!out_latency_frames)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_latency_frames = 0;
	if (!stream)
		return SPUDRESULT_SAUD_INVALID_STREAM;
	if (SPUD_STALE(stream, "spudaudio_stream_get_latency_frames", "stream"))
		return SPUDRESULT_SAUD_INVALID_STREAM;
	REFERENCE_TIME latency_hns = 0;
	HRESULT hr                 = IAudioClient_GetStreamLatency(stream->client, &latency_hns);
	if (FAILED(hr))
		return wasapi_result(hr);
	uint64_t frames = (uint64_t)latency_hns * stream->config.format.sample_rate / 10000000ULL;
	if (stream->direction == SPUDAUDIO_DIRECTION_OUTPUT) {
		UINT32 padding = 0;
		hr             = IAudioClient_GetCurrentPadding(stream->client, &padding);
		if (FAILED(hr))
			return wasapi_result(hr);
		frames += padding;
	}
	*out_latency_frames = (uint32_t)frames;
	return SPUD_SUCCESS;
}

// The shared-mode rate is a user setting (Sound control panel) with no
// API; exclusive streams carry their own rate in their format.
SPUDRESULT spudaudio_set_device_sample_rate(spudaudio_device device, uint32_t sample_rate) {
	if (!device)
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudaudio_set_device_sample_rate", "device"))
		return SPUDRESULT_SAUD_INVALID_DEVICE;
	if (sample_rate == 0)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	return SPUDRESULT_SAUD_SAMPLE_RATE_NOT_SETTABLE;
}

SPUDRESULT spudaudio_set_device_event_callback(
    spudaudio_instance instance,
    SPUDAUDIO_DEVICE_EVENT_CALLBACK callback,
    void *user_data) {
	if (!instance)
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudaudio_set_device_event_callback", "instance"))
		return SPUDRESULT_SAUD_INVALID_INSTANCE;
	if (wasapi_in_device_event_callback(instance))
		return SPUDRESULT_GENERAL_FAILURE;

	// The new watch is built before the old one goes, so a failure leaves
	// the callback that was set still set.
	struct wasapi_watch *watch = NULL;
	if (callback) {
		SPUDRESULT r = wasapi_watch_create(instance, callback, user_data, &watch);
		if (SPUDFAIL(r))
			return r;
	}
	wasapi_watch_destroy(instance->watch);
	instance->watch = watch;
	return SPUD_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // SPUDAUDIO_COMPILE_WASAPI
