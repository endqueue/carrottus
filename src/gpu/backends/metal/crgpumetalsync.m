//
// Carrottus GPU Metal backend - fences (id<MTLSharedEvent>) and semaphores
// (plain id<MTLEvent> - crgpu_semaphore never exposes a CPU-side accessor,
// so it doesn't need MTLSharedEvent's CPU-visibility on top). Implemented and
// verified against real Apple hardware.
//

#if CRGPU_COMPILE_METAL_API

#include "crgpu.h"
#include "crcore.h"
#include "crgpumetal.h"
#include <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <time.h>

static uint64_t crgpumetal___internal_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

CR_RESULT crgpu_fence_create(
    crgpu_device device,
    CRGPU_FENCE_FLAGS flags,
    uint64_t initial_value,
    crgpu_fence *out_fence) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_fence_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!out_fence)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	CR_RESULT sr = CR_SUCCESS;

	crgpu_fence_metal *fence_metal = CR_NEW_ZEROED(&device->head.allocator, crgpu_fence_metal);
	if (!fence_metal) {
		sr = CR_RESULT_OUT_OF_MEMORY;
		goto failedattempt;
	}
	fence_metal->head.allocator = device->head.allocator;
	CR_BIRTH_IN(fence_metal, &device->_quarantine);
	fence_metal->_parent_device = (crgpu_device_metal *)device;
	fence_metal->_flags         = flags;

	// CRGPU_FENCE_FLAG_SHARED needs no special creation-time flag on Metal
	// the way D3D12_FENCE_FLAG_SHARED does - any MTLSharedEvent can hand out
	// a cross-process MTLSharedEventHandle via -newSharedEventHandle on
	// demand. flags is kept on the struct for API symmetry and future
	// natives-header use.
	fence_metal->_shared_event_mtl = [fence_metal->_parent_device->_device_mtl newSharedEvent];
	if (!fence_metal->_shared_event_mtl) {
		sr = CR_RESULT_API_SPECIFIC_FAILURE;
		goto failedattempt;
	}
	fence_metal->_shared_event_mtl.signaledValue = initial_value;

	CRGPU___LIVE_ADD(&fence_metal->_parent_device->_live, CRGPU___OBJECT_FENCE);
	*out_fence = (crgpu_fence)fence_metal;

	return sr;

failedattempt:
	CR_DELETE(&device->head.allocator, crgpu_fence_metal, fence_metal);
	return sr;
}

void crgpu_fence_destroy(crgpu_fence fence) {
	crgpu_fence_metal *fence_metal = (crgpu_fence_metal *)fence;
	if (fence_metal) {
		if (CR_STALE(fence, "crgpu_fence_destroy", "fence"))
			return;
		if (fence_metal->_shared_event_mtl) {
			[fence_metal->_shared_event_mtl release];
		}
		CRGPU___LIVE_REMOVE(&fence_metal->_parent_device->_live, CRGPU___OBJECT_FENCE);
		CR_KILL(fence);
		const CR_ALLOCATOR allocator = fence_metal->head.allocator;
		cr_string_free(&allocator, fence_metal->head.debug_name);
		CR_RETIRE(CR_QUARANTINE_OF(fence_metal), &allocator, crgpu_fence_metal, fence_metal);
	}
}

uint64_t crgpu_fence_get_value(crgpu_fence fence) {
	crgpu_fence_metal *fence_metal = (crgpu_fence_metal *)fence;
	if (!fence_metal)
		return 0;
	if (CR_STALE(fence, "crgpu_fence_get_value", "fence"))
		return 0;
	if (!fence_metal->_shared_event_mtl)
		return 0;
	// The value the GPU (or a prior crgpu_fence_signal call) has actually
	// reached, not one a submission still running has been asked to signal.
	return fence_metal->_shared_event_mtl.signaledValue;
}

CR_RESULT crgpu_fence_signal(
    crgpu_device device,
    crgpu_fence fence,
    uint64_t value) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_fence_signal", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!fence)
		return CR_RESULT_GPU_INVALID_FENCE;
	if (CR_STALE(fence, "crgpu_fence_signal", "fence"))
		return CR_RESULT_GPU_INVALID_FENCE;

	crgpu_fence_metal *fence_metal = (crgpu_fence_metal *)fence;
	fence_metal->_shared_event_mtl.signaledValue = value;

	return CR_SUCCESS;
}

CR_RESULT crgpu_fence_wait(
    crgpu_device device,
    crgpu_fence *fences,
    const uint64_t *values,
    uint32_t fence_count,
    bool wait_all,
    uint64_t timeout_ns) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_fence_wait", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!fences)
		return CR_RESULT_GPU_INVALID_FENCE;
	if (!values)
		return CR_RESULT_NULL_OBJECT;
	if (fence_count == 0)
		return CR_RESULT_ZERO_SIZE;
	if (fence_count > 64)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	for (uint32_t i = 0; i < fence_count; i++) {
		if (!fences[i])
			return CR_RESULT_GPU_INVALID_FENCE;
	}

	// Rounded up, so a wait shorter than a millisecond still waits.
	uint64_t timeout_ms = (timeout_ns == UINT64_MAX) ? UINT64_MAX : timeout_ns / 1000000ULL + (timeout_ns % 1000000ULL ? 1 : 0);

	if (wait_all) {
		// MTLSharedEvent only exposes a single-event blocking wait
		// (-waitUntilSignaledValue:timeoutMS:), unlike vkWaitSemaphores/
		// WaitForMultipleObjects - wait_all is composed by waiting on each
		// event in turn against a shared remaining-time budget, which is
		// equivalent to waiting on all of them at once since the call can't
		// return success before every one of them is satisfied anyway.
		uint64_t remaining_ms = timeout_ms;
		for (uint32_t i = 0; i < fence_count; i++) {
			crgpu_fence_metal *fence_metal = (crgpu_fence_metal *)fences[i];

			// Already there: no wait, and none of the budget spent.
			if (fence_metal->_shared_event_mtl.signaledValue >= values[i])
				continue;

			uint64_t started_ms = crgpumetal___internal_now_ms();
			BOOL signaled = [fence_metal->_shared_event_mtl
			    waitUntilSignaledValue:values[i]
			                 timeoutMS:remaining_ms];
			if (!signaled)
				return CR_RESULT_GPU_FENCE_WAIT_TIMED_OUT;

			if (timeout_ms != UINT64_MAX) {
				uint64_t elapsed_ms = crgpumetal___internal_now_ms() - started_ms;
				remaining_ms         = (elapsed_ms >= remaining_ms) ? 0 : remaining_ms - elapsed_ms;
			}
		}
		return CR_SUCCESS;
	}

	// wait_all == false: block until ANY one fence reaches its value. One
	// already there settles it without a listener.
	for (uint32_t i = 0; i < fence_count; i++) {
		crgpu_fence_metal *fence_metal = (crgpu_fence_metal *)fences[i];
		if (fence_metal->_shared_event_mtl.signaledValue >= values[i])
			return CR_SUCCESS;
	}

	// There is no native any-of-N wait either, so this is composed from a
	// per-fence async listener that releases one shared dispatch semaphore
	// as soon as the first of them fires.
	dispatch_queue_t listener_queue =
	    dispatch_queue_create("crgpu.metal.wait_for_fences", DISPATCH_QUEUE_SERIAL);
	MTLSharedEventListener *listener =
	    [[MTLSharedEventListener alloc] initWithDispatchQueue:listener_queue];
	dispatch_semaphore_t any_signaled = dispatch_semaphore_create(0);

	for (uint32_t i = 0; i < fence_count; i++) {
		crgpu_fence_metal *fence_metal = (crgpu_fence_metal *)fences[i];
		[fence_metal->_shared_event_mtl
		    notifyListener:listener
		            atValue:values[i]
		              block:^(id<MTLSharedEvent> event, uint64_t value) {
			              dispatch_semaphore_signal(any_signaled);
		              }];
	}

	dispatch_time_t deadline = (timeout_ns == UINT64_MAX)
	                               ? DISPATCH_TIME_FOREVER
	                               : dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ns);
	long wait_result = dispatch_semaphore_wait(any_signaled, deadline);

	[any_signaled release];
	[listener release];
	[listener_queue release];

	return (wait_result == 0) ? CR_SUCCESS : CR_RESULT_GPU_FENCE_WAIT_TIMED_OUT;
}

CR_RESULT crgpu_semaphore_create(
    crgpu_device device,
    crgpu_semaphore *out_semaphore) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_semaphore_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!out_semaphore)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	CR_RESULT sr = CR_SUCCESS;

	crgpu_semaphore_metal *semaphore_metal = CR_NEW_ZEROED(&device->head.allocator, crgpu_semaphore_metal);
	if (!semaphore_metal) {
		sr = CR_RESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	semaphore_metal->head.allocator = device->head.allocator;
	CR_BIRTH_IN(semaphore_metal, &device->_quarantine);
	semaphore_metal->_parent_device = (crgpu_device_metal *)device;

	semaphore_metal->_event_mtl = [semaphore_metal->_parent_device->_device_mtl newEvent];
	if (!semaphore_metal->_event_mtl) {
		sr = CR_RESULT_API_SPECIFIC_FAILURE;
		goto failedattempt;
	}

	CRGPU___LIVE_ADD(&semaphore_metal->_parent_device->_live, CRGPU___OBJECT_SEMAPHORE);
	*out_semaphore = (crgpu_semaphore)semaphore_metal;

	return sr;

failedattempt:
	CR_DELETE(&device->head.allocator, crgpu_semaphore_metal, semaphore_metal);
	return sr;
}

void crgpu_semaphore_destroy(crgpu_semaphore semaphore) {
	crgpu_semaphore_metal *semaphore_metal = (crgpu_semaphore_metal *)semaphore;
	if (semaphore_metal) {
		if (CR_STALE(semaphore, "crgpu_semaphore_destroy", "semaphore"))
			return;
		if (semaphore_metal->_event_mtl) {
			[semaphore_metal->_event_mtl release];
		}
		CRGPU___LIVE_REMOVE(&semaphore_metal->_parent_device->_live, CRGPU___OBJECT_SEMAPHORE);
		CR_KILL(semaphore);
		const CR_ALLOCATOR allocator = semaphore_metal->head.allocator;
		cr_string_free(&allocator, semaphore_metal->head.debug_name);
		CR_RETIRE(CR_QUARANTINE_OF(semaphore_metal), &allocator, crgpu_semaphore_metal, semaphore_metal);
	}
}

#endif // CRGPU_COMPILE_METAL_API
