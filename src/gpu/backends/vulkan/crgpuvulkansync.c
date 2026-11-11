
#if CRGPU_COMPILE_VULKAN_API

#include "crgpuvulkan.h"
#include <stdlib.h>
#include <stdio.h>

#if __cplusplus
extern "C" {
#endif

// crgpu_fence is a timeline semaphore: one 64-bit value that only goes up,
// signaled by a queue submission (crgpu_command_queue_submit, crgpuvulkancommand.c)
// or from the CPU, and read and waited on from the CPU. Core since Vulkan 1.2;
// the timelineSemaphore feature is enabled at device creation
// (crgpuvulkancontext.c).
CR_RESULT crgpu_fence_create(
    crgpu_device device,
    CRGPU_FENCE_FLAGS flags,
    uint64_t initial_value,
    crgpu_fence *out_fence) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_fence_create", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!out_fence) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    // CRGPU_FENCE_FLAG_SHARED has nothing to select here: sharing a
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

    crgpu_fence_vulkan *fence = CR_NEW_ZEROED(&device->head.allocator, crgpu_fence_vulkan);
    if (!fence) return CR_RESULT_OUT_OF_MEMORY;
    fence->head.allocator = device->head.allocator;
    CR_BIRTH_IN(fence, &device->_shared->quarantine);

    VkResult r = vkCreateSemaphore(device->_logical_device_vk, &info, NULL, &fence->_semaphore_vk);
    if (r != VK_SUCCESS) {
        printf("crgpu: vkCreateSemaphore (fence) failed (%d)\n", r);
        CR_DELETE(&device->head.allocator, crgpu_fence_vulkan, fence);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }
    fence->_device_vk = device->_logical_device_vk;
    fence->_shared    = device->_shared;
    CRGPU___LIVE_ADD(&fence->_shared->live, CRGPU___OBJECT_FENCE);
    *out_fence = fence;
    return CR_SUCCESS;
}

void crgpu_fence_destroy(crgpu_fence fence) {
    if (!fence) return;
    if (CR_STALE(fence, "crgpu_fence_destroy", "fence")) return;
    vkDestroySemaphore(fence->_device_vk, fence->_semaphore_vk, NULL);
    CRGPU___LIVE_REMOVE(&fence->_shared->live, CRGPU___OBJECT_FENCE);
    CR_KILL(fence);
    const CR_ALLOCATOR allocator = fence->head.allocator;
    cr_string_free(&allocator, fence->head.debug_name);
    CR_RETIRE(CR_QUARANTINE_OF(fence), &allocator, crgpu_fence_vulkan, fence);
}

uint64_t crgpu_fence_get_value(crgpu_fence fence) {
    if (!fence) return 0;
    if (CR_STALE(fence, "crgpu_fence_get_value", "fence")) return 0;
    uint64_t value = 0;
    if (vkGetSemaphoreCounterValue(fence->_device_vk, fence->_semaphore_vk, &value) != VK_SUCCESS)
        return 0;
    return value;
}

CR_RESULT crgpu_fence_signal(
    crgpu_device device,
    crgpu_fence fence,
    uint64_t value) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_fence_signal", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!fence) return CR_RESULT_GPU_INVALID_FENCE;
    if (CR_STALE(fence, "crgpu_fence_signal", "fence")) return CR_RESULT_GPU_INVALID_FENCE;

    VkSemaphoreSignalInfo info = {0};
    info.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    info.semaphore = fence->_semaphore_vk;
    info.value     = value;
    if (vkSignalSemaphore(device->_logical_device_vk, &info) != VK_SUCCESS)
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
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_fence_wait", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!fences) return CR_RESULT_GPU_INVALID_FENCE;
    if (!values) return CR_RESULT_NULL_OBJECT;
    if (fence_count == 0) return CR_RESULT_ZERO_SIZE;
    if (fence_count > 64) return CR_RESULT_DESC_INVALID_PARAMETERS;

    VkSemaphore vk_semaphores[64];
    for (uint32_t i = 0; i < fence_count; i++) {
        if (!fences[i]) return CR_RESULT_GPU_INVALID_FENCE;
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
        return CR_RESULT_GPU_FENCE_WAIT_TIMED_OUT;
    if (r == VK_ERROR_DEVICE_LOST)
        return CR_RESULT_GPU_DEVICE_LOST;
    if (r != VK_SUCCESS)
        return CR_RESULT_API_SPECIFIC_FAILURE;
    return CR_SUCCESS;
}

CR_RESULT crgpu_semaphore_create(
    crgpu_device device,
    crgpu_semaphore *out_semaphore) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_semaphore_create", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!out_semaphore) return CR_RESULT_NULL_OUTPUT_PARAMETER;

    VkSemaphoreCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    crgpu_semaphore_vulkan *sem = CR_NEW_ZEROED(&device->head.allocator, crgpu_semaphore_vulkan);
    if (!sem) return CR_RESULT_GENERAL_FAILURE;
    sem->head.allocator = device->head.allocator;
    CR_BIRTH_IN(sem, &device->_shared->quarantine);

    VkResult r = vkCreateSemaphore(device->_logical_device_vk, &info, NULL, &sem->_semaphore_vk);
    if (r != VK_SUCCESS) {
        printf("crgpu: vkCreateSemaphore failed (%d)\n", r);
        CR_DELETE(&device->head.allocator, crgpu_semaphore_vulkan, sem);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }
    sem->_device_vk = device->_logical_device_vk;
    sem->_shared    = device->_shared;
    CRGPU___LIVE_ADD(&sem->_shared->live, CRGPU___OBJECT_SEMAPHORE);
    *out_semaphore = sem;
    return CR_SUCCESS;
}

void crgpu_semaphore_destroy(crgpu_semaphore semaphore) {
    if (!semaphore) return;
    if (CR_STALE(semaphore, "crgpu_semaphore_destroy", "semaphore")) return;
    vkDestroySemaphore(semaphore->_device_vk, semaphore->_semaphore_vk, NULL);
    CRGPU___LIVE_REMOVE(&semaphore->_shared->live, CRGPU___OBJECT_SEMAPHORE);
    CR_KILL(semaphore);
    const CR_ALLOCATOR allocator = semaphore->head.allocator;
    cr_string_free(&allocator, semaphore->head.debug_name);
    CR_RETIRE(CR_QUARANTINE_OF(semaphore), &allocator, crgpu_semaphore_vulkan, semaphore);
}

#if __cplusplus
}
#endif

#endif // CRGPU_COMPILE_VULKAN_API
