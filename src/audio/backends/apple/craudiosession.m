#if CRAUDIO_COMPILE_REMOTEIO

/*
 * The AVAudioSession half of the RemoteIO backend: see craudiosession.h.
 *
 * Reads the session and sets nothing of it. The
 * notifications are heard through block observers of the default
 * notification center, each on an NSOperationQueue whose underlying queue
 * is a serial dispatch queue made here, so an observer's events reach its
 * C function one at a time, in order, on a queue that can be drained.
 * There is no Objective-C class of Carrottus Audio's own.
 *
 * Plain Objective-C with manual retain/release, like the Metal backend.
 */

#if __has_feature(objc_arc)
#error "craudiosession.m is written for manual retain/release; compile it without -fobjc-arc."
#endif

#include "craudiosession.h"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>

#include <dispatch/dispatch.h>
#include <string.h>
#include "../../../crallocator.h"

struct craudio_session_observer {
	CR_ALLOCATOR allocator; // what this block was allocated with
	craudio_session_event_fn fn;
	void *context;
	dispatch_queue_t queue;
	NSOperationQueue *operations; // over `queue`
	id tokens[4];                 // one per notification observed
};

// Its address is the queue-specific key that marks an observer's queue;
// the value stored under it is the observer. Only the address is used.
static const char craudio_session_queue_key = 0;

static void session_copy_string(NSString *s, char *out, size_t out_size) {
	out[0] = '\0';
	if (s)
		[s getCString:out maxLength:out_size encoding:NSUTF8StringEncoding];
	// getCString: writes nothing where the string doesn't fit; take what
	// does, cut at a character boundary.
	if (s && out[0] == '\0' && s.length) {
		NSUInteger used = 0;
		[s getBytes:out maxLength:out_size - 1 usedLength:&used encoding:NSUTF8StringEncoding options:0 range:NSMakeRange(0, s.length) remainingRange:NULL];
		out[used] = '\0';
	}
}

void craudio_session_read(craudio_session_state *out) {
	@autoreleasepool {
		AVAudioSession *session    = [AVAudioSession sharedInstance];
		out->sample_rate           = session.sampleRate;
		out->io_buffer_duration    = session.IOBufferDuration;
		out->output_latency        = session.outputLatency;
		out->input_latency         = session.inputLatency;
	}
}

// The ports Carrottus Audio enumerates in `direction`: the route's outputs, or
// its inputs where input is available at all.
static NSArray<AVAudioSessionPortDescription *> *session_ports(AVAudioSession *session, CRAUDIO_DIRECTION direction) {
	AVAudioSessionRouteDescription *route = session.currentRoute;
	if (direction == CRAUDIO_DIRECTION_OUTPUT)
		return route.outputs;
	return session.inputAvailable ? route.inputs : @[];
}

static void session_describe_port(
    const CR_ALLOCATOR *allocator,
    AVAudioSession *session,
    CRAUDIO_DIRECTION direction,
    AVAudioSessionPortDescription *port,
    craudio_session_port *out) {
	memset(out, 0, sizeof(*out));
	session_copy_string(port.UID, out->persistent_id, sizeof(out->persistent_id));
	session_copy_string(port.portName, out->name, sizeof(out->name));

	NSArray<AVAudioSessionChannelDescription *> *channels = port.channels;
	NSUInteger count                                      = channels.count;
	if (count == 0) {
		const NSInteger session_count = direction == CRAUDIO_DIRECTION_OUTPUT ? session.outputNumberOfChannels : session.inputNumberOfChannels;
		count                         = session_count > 0 ? (NSUInteger)session_count : 0;
	}
	if (count > CRAUDIO_MAX_CHANNELS)
		count = CRAUDIO_MAX_CHANNELS;
	out->channel_count = (uint32_t)count;
	if (count == 0 || channels.count != count)
		return; // no labels to read: no layout

	// The labels as an AudioChannelLayout of descriptions, read as
	// CoreAudio's are.
	struct {
		AudioChannelLayout layout;
		AudioChannelDescription more[CRAUDIO_MAX_CHANNELS - 1];
	} described;
	memset(&described, 0, sizeof(described));
	described.layout.mChannelLayoutTag          = kAudioChannelLayoutTag_UseChannelDescriptions;
	described.layout.mNumberChannelDescriptions = (UInt32)count;
	for (NSUInteger i = 0; i < count; ++i)
		described.layout.mChannelDescriptions[i].mChannelLabel = channels[i].channelLabel;
	CRAUDIO_FORMAT format;
	memset(&format, 0, sizeof(format));
	format.channel_count = (uint32_t)count;
	craudio_apple_layout_to_positions(allocator, &described.layout, &format);
	memcpy(out->positions, format.positions, sizeof(out->positions));
}

CR_RESULT craudio_session_route_ports(
    const CR_ALLOCATOR *allocator,
    CRAUDIO_DIRECTION direction,
    craudio_session_port **out_ports,
    uint32_t *out_count,
    uint32_t *out_capacity) {
	*out_ports    = NULL;
	*out_count    = 0;
	*out_capacity = 0;
	@autoreleasepool {
		AVAudioSession *session                          = [AVAudioSession sharedInstance];
		NSArray<AVAudioSessionPortDescription *> *ports = session_ports(session, direction);
		const uint32_t count                             = (uint32_t)ports.count;
		if (count == 0)
			return CR_SUCCESS;
		craudio_session_port *list = CR_NEW_ARRAY_ZEROED(allocator, craudio_session_port, count);
		if (!list)
			return CR_RESULT_OUT_OF_MEMORY;
		for (uint32_t i = 0; i < count; ++i)
			session_describe_port(allocator, session, direction, ports[i], &list[i]);
		*out_ports    = list;
		*out_count    = count;
		*out_capacity = count;
	}
	return CR_SUCCESS;
}

bool craudio_session_find_port(
    const CR_ALLOCATOR *allocator,
    CRAUDIO_DIRECTION direction,
    const char *persistent_id,
    craudio_session_port *out_port,
    uint32_t *out_route_port_count) {
	bool found = false;
	@autoreleasepool {
		AVAudioSession *session                          = [AVAudioSession sharedInstance];
		NSArray<AVAudioSessionPortDescription *> *ports = session_ports(session, direction);
		if (out_route_port_count)
			*out_route_port_count = (uint32_t)ports.count;
		for (AVAudioSessionPortDescription *port in ports) {
			char uid[256];
			session_copy_string(port.UID, uid, sizeof(uid));
			if (strcmp(uid, persistent_id) != 0)
				continue;
			if (out_port)
				session_describe_port(allocator, session, direction, port, out_port);
			found = true;
			break;
		}
	}
	return found;
}

bool craudio_session_record_permission_granted(void) {
	bool granted = false;
	@autoreleasepool {
		granted = [AVAudioApplication sharedInstance].recordPermission == AVAudioApplicationRecordPermissionGranted;
	}
	return granted;
}

// ---------------------------------------------------------------------------
// Observers
// ---------------------------------------------------------------------------

static void session_drain(void *context) { (void)context; }

void craudio_session_unobserve(craudio_session_observer *o) {
	if (!o)
		return;
	@autoreleasepool {
		NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
		for (size_t i = 0; i < sizeof(o->tokens) / sizeof(o->tokens[0]); ++i)
			if (o->tokens[i]) {
				[center removeObserver:o->tokens[i]];
				[o->tokens[i] release];
			}
		// Removal doesn't wait for an event already queued: waiting for the
		// operations, then running an empty task on the queue under them,
		// does.
		if (o->operations) {
			[o->operations waitUntilAllOperationsAreFinished];
			[o->operations release];
		}
		if (o->queue) {
			dispatch_sync_f(o->queue, NULL, session_drain);
			dispatch_release(o->queue);
		}
	}
	const CR_ALLOCATOR allocator = o->allocator;
	CR_DELETE(&allocator, craudio_session_observer, o);
}

CR_RESULT craudio_session_observe(
    const CR_ALLOCATOR *allocator,
    const char *queue_label,
    craudio_session_event_fn fn,
    void *context,
    craudio_session_observer **out_observer) {
	*out_observer                  = NULL;
	craudio_session_observer *o = CR_NEW_ZEROED(allocator, craudio_session_observer);
	if (!o)
		return CR_RESULT_OUT_OF_MEMORY;
	o->allocator = *allocator;
	o->fn        = fn;
	o->context   = context;
	o->queue     = dispatch_queue_create(queue_label, DISPATCH_QUEUE_SERIAL);
	if (!o->queue) {
		craudio_session_unobserve(o);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	dispatch_queue_set_specific(o->queue, &craudio_session_queue_key, o, NULL);

	@autoreleasepool {
		o->operations = [[NSOperationQueue alloc] init];
		if (!o->operations) {
			craudio_session_unobserve(o);
			return CR_RESULT_OUT_OF_MEMORY;
		}
		o->operations.maxConcurrentOperationCount = 1;
		o->operations.underlyingQueue             = o->queue;

		NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
		AVAudioSession *session      = [AVAudioSession sharedInstance];
		o->tokens[0] = [[center addObserverForName:AVAudioSessionRouteChangeNotification
		                                    object:session
		                                     queue:o->operations
		                                usingBlock:^(NSNotification *note) {
			                                (void)note;
			                                fn(CRAUDIO_SESSION_EVENT_ROUTE_CHANGE, context);
		                                }] retain];
		o->tokens[1] = [[center addObserverForName:AVAudioSessionInterruptionNotification
		                                    object:session
		                                     queue:o->operations
		                                usingBlock:^(NSNotification *note) {
			                                NSNumber *type = note.userInfo[AVAudioSessionInterruptionTypeKey];
			                                fn(type.unsignedIntegerValue == AVAudioSessionInterruptionTypeBegan ? CRAUDIO_SESSION_EVENT_INTERRUPTION_BEGAN
			                                                                                                    : CRAUDIO_SESSION_EVENT_INTERRUPTION_ENDED,
			                                    context);
		                                }] retain];
		o->tokens[2] = [[center addObserverForName:AVAudioSessionMediaServicesWereLostNotification
		                                    object:session
		                                     queue:o->operations
		                                usingBlock:^(NSNotification *note) {
			                                (void)note;
			                                fn(CRAUDIO_SESSION_EVENT_MEDIA_SERVICES_LOST, context);
		                                }] retain];
		o->tokens[3] = [[center addObserverForName:AVAudioSessionMediaServicesWereResetNotification
		                                    object:session
		                                     queue:o->operations
		                                usingBlock:^(NSNotification *note) {
			                                (void)note;
			                                fn(CRAUDIO_SESSION_EVENT_MEDIA_SERVICES_RESET, context);
		                                }] retain];
		for (size_t i = 0; i < sizeof(o->tokens) / sizeof(o->tokens[0]); ++i)
			if (!o->tokens[i]) {
				craudio_session_unobserve(o);
				return CR_RESULT_OUT_OF_MEMORY;
			}
	}
	*out_observer = o;
	return CR_SUCCESS;
}

void craudio_session_run_on_queue(craudio_session_observer *observer, void (*task)(void *), void *context) {
	// The operations first, so an event they hold runs before the task.
	@autoreleasepool {
		[observer->operations waitUntilAllOperationsAreFinished];
	}
	dispatch_sync_f(observer->queue, context, task);
}

bool craudio_session_on_queue(const craudio_session_observer *observer) {
	return observer && dispatch_get_specific(&craudio_session_queue_key) == observer;
}

#else
// Not built without CRAUDIO_COMPILE_REMOTEIO (CMake leaves this file
// out); this keeps a stray build of it a valid, empty translation unit.
typedef int craudio_session_not_compiled;
#endif // CRAUDIO_COMPILE_REMOTEIO
