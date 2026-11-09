
#if SPUDGPU_COMPILE_VULKAN_API

#include "spudgpuvulkan.h"
#include <stdlib.h>
#include <stdio.h>

#if __cplusplus
extern "C" {
#endif

// A timestamp set is a VK_QUERY_TYPE_TIMESTAMP query pool. A rendering pass
// writes two of its queries (spudgpuvulkanrenderpass.c), each reset first
// with vkCmdResetQueryPool, and the results are read on the CPU where they
// were written: there is nothing to resolve.
SPUDRESULT spudgpu_create_timestamp_set(
    spudgpu_device device,
    spudgpu_command_queue queue,
    uint32_t count,
    spudgpu_timestamp_set *out_set) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_create_timestamp_set", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!queue) return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
    if (SPUD_STALE(queue, "spudgpu_create_timestamp_set", "queue")) return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
    if (count == 0) return SPUDRESULT_ZERO_SIZE;
    if (!out_set) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    if (!(device->_features & SPUDGPU_FEATURE_PASS_TIMESTAMPS)) return SPUDRESULT_GPU_TIMESTAMPS_NOT_SUPPORTED;

    spudgpu_timestamp_set_vulkan *set = SPUD_NEW_ZEROED(&device->head.allocator, spudgpu_timestamp_set_vulkan);
    if (!set) return SPUDRESULT_OUT_OF_MEMORY;
    set->head.allocator = device->head.allocator;
    SPUD_BIRTH_IN(set, &device->_shared->quarantine);

    VkQueryPoolCreateInfo info = {0};
    info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;
    VkResult r = vkCreateQueryPool(device->_logical_device_vk, &info, NULL, &set->_query_pool_vk);
    if (r != VK_SUCCESS) {
        printf("spudgpu: vkCreateQueryPool failed (%d)\n", r);
        SPUD_DELETE(&device->head.allocator, spudgpu_timestamp_set_vulkan, set);
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }
    set->_logical_device_vk = device->_logical_device_vk;
    set->_count             = count;
    set->_period_ns         = device->_properties_vk.limits.timestampPeriod;
    set->_shared            = device->_shared;
    SPUDGPU___LIVE_ADD(&set->_shared->live, SPUDGPU___OBJECT_TIMESTAMP_SET);
    *out_set = set;
    return SPUD_SUCCESS;
}

void spudgpu_destroy_timestamp_set(spudgpu_timestamp_set set) {
    if (!set) return;
    if (SPUD_STALE(set, "spudgpu_destroy_timestamp_set", "set")) return;
    vkDestroyQueryPool(set->_logical_device_vk, set->_query_pool_vk, NULL);
    SPUDGPU___LIVE_REMOVE(&set->_shared->live, SPUDGPU___OBJECT_TIMESTAMP_SET);
    SPUD_KILL(set);
    const SPUD_ALLOCATOR allocator = set->head.allocator;
    spud_string_free(&allocator, set->head.debug_name);
    SPUD_RETIRE(SPUD_QUARANTINE_OF(set), &allocator, spudgpu_timestamp_set_vulkan, set);
}

void spudgpu_cmd_reset_timestamps(
    spudgpu_command_list cmd,
    spudgpu_timestamp_set set,
    uint32_t first,
    uint32_t count) {
    if (!cmd || !set || count == 0) return;
    if (SPUD_STALE(cmd, "spudgpu_cmd_reset_timestamps", "cmd")) return;
    if (SPUD_STALE(set, "spudgpu_cmd_reset_timestamps", "set")) return;
    if (first >= set->_count || count > set->_count - first) return;
    vkCmdResetQueryPool(cmd->_command_buffer_vk, set->_query_pool_vk, first, count);
}

void spudgpu_cmd_resolve_timestamps(
    spudgpu_command_list cmd,
    spudgpu_timestamp_set set,
    uint32_t first,
    uint32_t count) {
    // The results are read from the pool itself (spudgpu_get_timestamps).
    if (!cmd || !set)
        return;
    if (SPUD_STALE(cmd, "spudgpu_cmd_resolve_timestamps", "cmd"))
        return;
    if (SPUD_STALE(set, "spudgpu_cmd_resolve_timestamps", "set"))
        return;
    (void)first;
    (void)count;
}

SPUDRESULT spudgpu_get_timestamps(
    spudgpu_timestamp_set set,
    uint32_t first,
    uint32_t count,
    uint64_t *out_ns) {
    if (!set) return SPUDRESULT_GPU_INVALID_TIMESTAMP_SET;
    if (SPUD_STALE(set, "spudgpu_get_timestamps", "set")) return SPUDRESULT_GPU_INVALID_TIMESTAMP_SET;
    if (count == 0) return SPUDRESULT_ZERO_SIZE;
    if (first >= set->_count || count > set->_count - first) return SPUDRESULT_INDEX_OUT_OF_RANGE;
    if (!out_ns) return SPUDRESULT_NULL_OUTPUT_PARAMETER;

    // No wait: the caller reads after the resolving submission has finished
    // (spudgpu.h). VK_NOT_READY leaves an unwritten entry's value as it was,
    // which spudgpu.h calls undefined.
    VkResult r = vkGetQueryPoolResults(set->_logical_device_vk, set->_query_pool_vk, first, count, sizeof(uint64_t) * count, out_ns,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY) return SPUDRESULT_API_SPECIFIC_FAILURE;
    for (uint32_t i = 0; i < count; ++i)
        out_ns[i] = (uint64_t)((double)out_ns[i] * (double)set->_period_ns);
    return SPUD_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // SPUDGPU_COMPILE_VULKAN_API
