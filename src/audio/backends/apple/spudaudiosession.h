#ifndef SPUDLIB_SPUDAUDIOSESSION_H
#define SPUDLIB_SPUDAUDIOSESSION_H

/*
 * The AVAudioSession half of the RemoteIO backend (iOS and iPadOS), in
 * Objective-C (spudaudiosession.m) behind these C calls, so that the audio
 * unit's half (spudaudioremoteio.c) is plain C and its render callback
 * never sends an Objective-C message.
 *
 * Everything here reads the process's session as it is: nothing of it is
 * ever set, the session being the caller's.
 *
 * Every call may be made from any thread: AVAudioSession's properties are
 * readable from any thread, and each call drains its own autorelease pool,
 * so a plain C thread with no run loop is fine.
 */

#include "spudaudioapple.h"

#if __cplusplus
extern "C" {
#endif

// One port of the session's current route, as SpudAudio reads it.
typedef struct spudaudio_session_port {
	char persistent_id[256]; // AVAudioSessionPortDescription::UID
	char name[128];          // ::portName
	// SPUDAUDIO_DEVICE_PROPERTIES::native_format's channels: the port's
	// channel descriptions, or the session's channel count where the port
	// gives none; at most SPUDAUDIO_MAX_CHANNELS.
	uint32_t channel_count;
	// From the descriptions' channelLabel; all UNSPECIFIED where the port
	// gives none or they don't make a layout.
	SPUDAUDIO_CHANNEL_POSITION positions[SPUDAUDIO_MAX_CHANNELS];
} spudaudio_session_port;

// The session's properties a stream depends on, read together.
typedef struct spudaudio_session_state {
	double sample_rate;             // sampleRate, Hz
	double io_buffer_duration;      // IOBufferDuration, seconds
	double output_latency;          // outputLatency, seconds
	double input_latency;           // inputLatency, seconds
} spudaudio_session_state;

void spudaudio_session_read(spudaudio_session_state *out);

// Every port of the current route in `direction`, in a block of
// `allocator` with room for `*out_capacity` (freed with it; NULL and 0 for
// none). Inputs only where the session has input available.
SPUDRESULT spudaudio_session_route_ports(
    const SPUD_ALLOCATOR *allocator,
    SPUDAUDIO_DIRECTION direction,
    spudaudio_session_port **out_ports,
    uint32_t *out_count,
    uint32_t *out_capacity);

// The port of the current route in `direction` whose UID is
// `persistent_id`, written to `out_port` (may be NULL), and how many ports
// the route has in that direction. False, and nothing written, where the
// route doesn't have it. Allocates nothing of SpudAudio's.
bool spudaudio_session_find_port(
    const SPUD_ALLOCATOR *allocator,
    SPUDAUDIO_DIRECTION direction,
    const char *persistent_id,
    spudaudio_session_port *out_port,
    uint32_t *out_route_port_count);

// AVAudioApplication.recordPermission is granted.
bool spudaudio_session_record_permission_granted(void);

// What a session observer is told.
enum {
	SPUDAUDIO_SESSION_EVENT_ROUTE_CHANGE        = 0, // AVAudioSessionRouteChangeNotification
	SPUDAUDIO_SESSION_EVENT_INTERRUPTION_BEGAN  = 1, // ...InterruptionNotification, type Began
	SPUDAUDIO_SESSION_EVENT_INTERRUPTION_ENDED  = 2, // ...InterruptionNotification, type Ended
	SPUDAUDIO_SESSION_EVENT_MEDIA_SERVICES_LOST = 3, // ...MediaServicesWereLostNotification
	SPUDAUDIO_SESSION_EVENT_MEDIA_SERVICES_RESET = 4 // ...MediaServicesWereResetNotification
};

typedef void (*spudaudio_session_event_fn)(uint32_t event, void *context);

// The session's notifications, passed to `fn` on a serial dispatch queue
// this makes. One per instance watch and one per stream.
typedef struct spudaudio_session_observer spudaudio_session_observer;

SPUDRESULT spudaudio_session_observe(
    const SPUD_ALLOCATOR *allocator,
    const char *queue_label,
    spudaudio_session_event_fn fn,
    void *context,
    spudaudio_session_observer **out_observer);

// Returns once `fn` is not running and will not run again. NULL is a no-op.
// Never from inside `fn`.
void spudaudio_session_unobserve(spudaudio_session_observer *observer);

// Runs `task` on the observer's queue and returns once it has run, after
// any event queued before it.
void spudaudio_session_run_on_queue(spudaudio_session_observer *observer, void (*task)(void *), void *context);

// True on the observer's own queue, where waiting for it would never end.
bool spudaudio_session_on_queue(const spudaudio_session_observer *observer);

#if __cplusplus
}
#endif

#endif // SPUDLIB_SPUDAUDIOSESSION_H
