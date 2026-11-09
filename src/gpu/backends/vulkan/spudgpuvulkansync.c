
#if SPUDGPU_COMPILE_VULKAN_API

#include "spudgpuvulkan.h"
#include <stdlib.h>
#include <stdio.h>

#if __cplusplus
extern "C" {
#endif

// spudgpu_fence is a timeline semaphore: one 64-bit value that only goes up,
// signaled by a queue submission (spudgpu_queue_submit, spudgpuvulkancommand.c)
// or from the CPU, and read and waited on from the CPU. Core since Vulkan 1.2;
// the timelineSemaphore feature is enabled at device creation
// (spudgpuvulkancontext.c).
SPUDRESULT spudgpu_create_fence(
    spudgpu_device device,
    SPUDGPU_FENCE_FLAGS flags,
    uint64_t initial_value,
    spudgpu_fence *out_fence) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_create_fence", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!out_fence) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    // SPUDGPU_FENCE_FLAG_SHARED has nothing to select here: sharing a
    // semaphore across processes is an export done on demand, not a
    // creation flag.
    (void)flags;

    VkSemaphoreTypeCreateInfo type = {0};
    type.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue  = initial_value;

    VkSemaphoreCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;

    spudgpu_fence_vulkan *fence = SPUD_NEW_ZEROED(&device->head.allocator, spudgpu_fence_vulkan);
    if (!fence) return SPUDRESULT_OUT_OF_MEMORY;
    fence->head.allocator = device->head.allocator;
    SPUD_BIRTH_IN(fence, &device->_shared->quarantine);

    VkResult r = vkCreateSemaphore(device->_logical_device_vk, &info, NULL, &fence->_semaphore_vk);
    if (r != VK_SUCCESS) {
        printf("spudgpu: vkCreateSemaphore (fence) failed (%d)\n", r);
        SPUD_DELETE(&device->head.allocator, spudgpu_fence_vulkan, fence);
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }
    fence->_device_vk = device->_logical_device_vk;
    fence->_shared    = device->_shared;
    SPUDGPU___LIVE_ADD(&fence->_shared->live, SPUDGPU___OBJECT_FENCE);
    *out_fence = fence;
    return SPUD_SUCCESS;
}

void spudgpu_destroy_fence(spudgpu_fence fence) {
    if (!fence) return;
    if (SPUD_STALE(fence, "spudgpu_destroy_fence", "fence")) return;
    vkDestroySemaphore(fence->_device_vk, fence->_semaphore_vk, NULL);
    SPUDGPU___LIVE_REMOVE(&fence->_shared->live, SPUDGPU___OBJECT_FENCE);
    SPUD_KILL(fence);
    const SPUD_ALLOCATOR allocator = fence->head.allocator;
    spud_string_free(&allocator, fence->head.debug_name);
    SPUD_RETIRE(SPUD_QUARANTINE_OF(fence), &allocator, spudgpu_fence_vulkan, fence);
}

uint64_t spudgpu_get_fence_value(spudgpu_fence fence) {
    if (!fence) return 0;
    if (SPUD_STALE(fence, "spudgpu_get_fence_value", "fence")) return 0;
    uint64_t value = 0;
    if (vkGetSemaphoreCounterValue(fence->_device_vk, fence->_semaphore_vk, &value) != VK_SUCCESS)
        return 0;
    return value;
}

SPUDRESULT spudgpu_signal_fence(
    spudgpu_device device,
    spudgpu_fence fence,
    uint64_t value) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_signal_fence", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!fence) return SPUDRESULT_GPU_INVALID_FENCE;
    if (SPUD_STALE(fence, "spudgpu_signal_fence", "fence")) return SPUDRESULT_GPU_INVALID_FENCE;

    VkSemaphoreSignalInfo info = {0};
    info.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    info.semaphore = fence->_semaphore_vk;
    info.value     = value;
    if (vkSignalSemaphore(device->_logical_device_vk, &info) != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_wait_for_fences(
    spudgpu_device device,
    spudgpu_fence *fences,
    const uint64_t *values,
    uint32_t fence_count,
    bool wait_all,
    uint64_t timeout_ns) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_wait_for_fences", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!fences) return SPUDRESULT_GPU_INVALID_FENCE;
    if (!values) return SPUDRESULT_NULL_OBJECT;
    if (fence_count == 0) return SPUDRESULT_ZERO_SIZE;
    if (fence_count > 64) return SPUDRESULT_DESC_INVALID_PARAMETERS;

    VkSemaphore vk_semaphores[64];
    for (uint32_t i = 0; i < fence_count; i++) {
        if (!fences[i]) return SPUDRESULT_GPU_INVALID_FENCE;
        vk_semaphores[i] = fences[i]->_semaphore_vk;
    }

    VkSemaphoreWaitInfo info = {0};
    info.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.flags          = wait_all ? 0 : VK_SEMAPHORE_WAIT_ANY_BIT;
    info.semaphoreCount = fence_count;
    info.pSemaphores    = vk_semaphores;
    info.pValues        = values;

    VkResult r = vkWaitSemaphores(device->_logical_device_vk, &info, timeout_ns);
    if (r == VK_TIMEOUT)
        return SPUDRESULT_GPU_FENCE_WAIT_TIMED_OUT;
    if (r == VK_ERROR_DEVICE_LOST)
        return SPUDRESULT_GPU_DEVICE_LOST;
    if (r != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_create_semaphore(
    spudgpu_device device,
    spudgpu_semaphore *out_semaphore) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_create_semaphore", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!out_semaphore) return SPUDRESULT_NULL_OUTPUT_PARAMETER;

    VkSemaphoreCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    spudgpu_semaphore_vulkan *sem = SPUD_NEW_ZEROED(&device->head.allocator, spudgpu_semaphore_vulkan);
    if (!sem) return SPUDRESULT_GENERAL_FAILURE;
    sem->head.allocator = device->head.allocator;
    SPUD_BIRTH_IN(sem, &device->_shared->quarantine);

    VkResult r = vkCreateSemaphore(device->_logical_device_vk, &info, NULL, &sem->_semaphore_vk);
    if (r != VK_SUCCESS) {
        printf("spudgpu: vkCreateSemaphore failed (%d)\n", r);
        SPUD_DELETE(&device->head.allocator, spudgpu_semaphore_vulkan, sem);
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }
    sem->_device_vk = device->_logical_device_vk;
    sem->_shared    = device->_shared;
    SPUDGPU___LIVE_ADD(&sem->_shared->live, SPUDGPU___OBJECT_SEMAPHORE);
    *out_semaphore = sem;
    return SPUD_SUCCESS;
}

void spudgpu_destroy_semaphore(spudgpu_semaphore semaphore) {
    if (!semaphore) return;
    if (SPUD_STALE(semaphore, "spudgpu_destroy_semaphore", "semaphore")) return;
    vkDestroySemaphore(semaphore->_device_vk, semaphore->_semaphore_vk, NULL);
    SPUDGPU___LIVE_REMOVE(&semaphore->_shared->live, SPUDGPU___OBJECT_SEMAPHORE);
    SPUD_KILL(semaphore);
    const SPUD_ALLOCATOR allocator = semaphore->head.allocator;
    spud_string_free(&allocator, semaphore->head.debug_name);
    SPUD_RETIRE(SPUD_QUARANTINE_OF(semaphore), &allocator, spudgpu_semaphore_vulkan, semaphore);
}

#if __cplusplus
}
#endif

#endif // SPUDGPU_COMPILE_VULKAN_API
