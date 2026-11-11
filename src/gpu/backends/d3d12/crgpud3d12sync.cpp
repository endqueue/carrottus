
#if CRGPU_COMPILE_D3D12_API

#include "crgpud3d12.hpp"

static D3D12_FENCE_FLAGS
crgpu_d3d12_get_fence_flags(CRGPU_FENCE_FLAGS flags) {
	D3D12_FENCE_FLAGS result = D3D12_FENCE_FLAG_NONE;
	switch (flags) {
	case CRGPU_FENCE_FLAG_SHARED:
		result |= D3D12_FENCE_FLAG_SHARED;
		break;
	}
	return result;
}

extern "C" {

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

	crgpu_fence_d3d12 *pResult =
	    CR_NEW(&device->head.allocator, crgpu_fence_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult                       = new (pResult) crgpu_fence_d3d12();
	pResult->head.allocator       = device->head.allocator;
	CR_BIRTH_IN(pResult, &device->_quarantine);
	pResult->_device              = device;
	pResult->flags                = flags;

	if (FAILED(device->_d3d_device->CreateFence(
	        initial_value, crgpu_d3d12_get_fence_flags(flags),
	        IID_PPV_ARGS(&pResult->_d3d_fence)))) {
		pResult->~crgpu_fence_d3d12();
		CR_DELETE(&device->head.allocator, crgpu_fence_d3d12, pResult);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_FENCE);
	*out_fence = pResult;

	return CR_SUCCESS;
}

void crgpu_fence_destroy(crgpu_fence fence) {
	if (!fence)
		return;
	if (CR_STALE(fence, "crgpu_fence_destroy", "fence"))
		return;
	CRGPU___LIVE_REMOVE(&fence->_device->_live, CRGPU___OBJECT_FENCE);
	CR_KILL(fence);
	const CR_ALLOCATOR allocator = fence->head.allocator;
	cr_string_free(&allocator, fence->head.debug_name);
	CR_QUARANTINE_KEEP(quarantine, fence);
	fence->~crgpu_fence_d3d12();
	CR_RETIRE(quarantine, &allocator, crgpu_fence_d3d12, fence);
}

uint64_t crgpu_fence_get_value(crgpu_fence fence) {
	if (!fence)
		return 0;
	if (CR_STALE(fence, "crgpu_fence_get_value", "fence"))
		return 0;
	return fence->_d3d_fence->GetCompletedValue();
}
CR_RESULT crgpu_fence_signal(
    crgpu_device device, crgpu_fence fence, uint64_t value) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_fence_signal", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!fence)
		return CR_RESULT_GPU_INVALID_FENCE;
	if (CR_STALE(fence, "crgpu_fence_signal", "fence"))
		return CR_RESULT_GPU_INVALID_FENCE;
	if (FAILED(fence->_d3d_fence->Signal(value)))
		return CR_RESULT_API_SPECIFIC_FAILURE;
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
	// The header's limit, which is also MAXIMUM_WAIT_OBJECTS.
	if (fence_count > 64)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	for (uint32_t i = 0; i < fence_count; i++) {
		if (!fences[i])
			return CR_RESULT_GPU_INVALID_FENCE;
	}

	// A removed device signals every fence to UINT64_MAX, so a wait on it
	// returns at once: asked before the wait and after it.
	if (FAILED(device->_d3d_device->GetDeviceRemovedReason()))
		return CR_RESULT_GPU_DEVICE_LOST;

	HANDLE events[MAXIMUM_WAIT_OBJECTS];
	uint32_t events_created = 0;

	for (uint32_t i = 0; i < fence_count; i++) {
		events[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!events[i]) {
			for (uint32_t j = 0; j < events_created; j++)
				CloseHandle(events[j]);
			return CR_RESULT_API_SPECIFIC_FAILURE;
		}
		events_created++;
		// Sets the event at once if the fence is already at the value.
		if (FAILED(fences[i]->_d3d_fence->SetEventOnCompletion(values[i], events[i]))) {
			for (uint32_t j = 0; j < events_created; j++)
				CloseHandle(events[j]);
			return CR_RESULT_API_SPECIFIC_FAILURE;
		}
	}

	// Rounded up, so a wait shorter than a millisecond still waits; a
	// timeout too long for a DWORD waits without limit.
	DWORD timeout_ms = INFINITE;
	if (timeout_ns != UINT64_MAX) {
		uint64_t ms = timeout_ns / 1000000ULL + (timeout_ns % 1000000ULL ? 1 : 0);
		if (ms < INFINITE)
			timeout_ms = (DWORD)ms;
	}
	DWORD result = WaitForMultipleObjects(
	    fence_count, events, wait_all ? TRUE : FALSE, timeout_ms);

	for (uint32_t i = 0; i < fence_count; i++)
		CloseHandle(events[i]);

	if (result == WAIT_TIMEOUT)
		return CR_RESULT_GPU_FENCE_WAIT_TIMED_OUT;
	if (result == WAIT_FAILED)
		return CR_RESULT_API_SPECIFIC_FAILURE;
	if (FAILED(device->_d3d_device->GetDeviceRemovedReason()))
		return CR_RESULT_GPU_DEVICE_LOST;
	return CR_SUCCESS;
}

CR_RESULT crgpu_semaphore_create(
    crgpu_device device, crgpu_semaphore *out_semaphore) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_semaphore_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!out_semaphore)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	crgpu_semaphore_d3d12 *pResult =
	    CR_NEW(&device->head.allocator, crgpu_semaphore_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult                 = new (pResult) crgpu_semaphore_d3d12();
	pResult->head.allocator = device->head.allocator;
	CR_BIRTH_IN(pResult, &device->_quarantine);
	pResult->_device        = device;

	if (FAILED(device->_d3d_device->CreateFence(
	        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pResult->_d3d_fence)))) {
		pResult->~crgpu_semaphore_d3d12();
		CR_DELETE(&device->head.allocator, crgpu_semaphore_d3d12, pResult);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_SEMAPHORE);
	*out_semaphore = pResult;

	return CR_SUCCESS;
}

void crgpu_semaphore_destroy(crgpu_semaphore semaphore) {
	if (!semaphore)
		return;
	if (CR_STALE(semaphore, "crgpu_semaphore_destroy", "semaphore"))
		return;
	CRGPU___LIVE_REMOVE(&semaphore->_device->_live, CRGPU___OBJECT_SEMAPHORE);
	CR_KILL(semaphore);
	const CR_ALLOCATOR allocator = semaphore->head.allocator;
	cr_string_free(&allocator, semaphore->head.debug_name);
	CR_QUARANTINE_KEEP(quarantine, semaphore);
	semaphore->~crgpu_semaphore_d3d12();
	CR_RETIRE(quarantine, &allocator, crgpu_semaphore_d3d12, semaphore);
}
}

#endif // CRGPU_COMPILE_D3D12_API
