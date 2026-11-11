
#if CRGPU_COMPILE_VULKAN_API

#include "crgpuvulkan.h"
#include <stdlib.h>
#include <stdio.h>

#if __cplusplus
extern "C" {
#endif

// A timestamp set is a VK_QUERY_TYPE_TIMESTAMP query pool. A rendering pass
// writes two of its queries (crgpuvulkanrenderpass.c), each reset first
// with vkCmdResetQueryPool, and the results are read on the CPU where they
// were written: there is nothing to resolve.
CR_RESULT crgpu_timestamp_set_create(
    crgpu_device device,
    crgpu_command_queue queue,
    uint32_t count,
    crgpu_timestamp_set *out_set) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_timestamp_set_create", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!queue) return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
    if (CR_STALE(queue, "crgpu_timestamp_set_create", "queue")) return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
    if (count == 0) return CR_RESULT_ZERO_SIZE;
    if (!out_set) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    if (!(device->_features & CRGPU_FEATURE_PASS_TIMESTAMPS)) return CR_RESULT_GPU_TIMESTAMPS_NOT_SUPPORTED;

    crgpu_timestamp_set_vulkan *set = CR_NEW_ZEROED(&device->head.allocator, crgpu_timestamp_set_vulkan);
    if (!set) return CR_RESULT_OUT_OF_MEMORY;
    set->head.allocator = device->head.allocator;
    CR_BIRTH_IN(set, &device->_shared->quarantine);

    VkQueryPoolCreateInfo info = {0};
    info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;
    VkResult r = vkCreateQueryPool(device->_logical_device_vk, &info, NULL, &set->_query_pool_vk);
    if (r != VK_SUCCESS) {
        printf("crgpu: vkCreateQueryPool failed (%d)\n", r);
        CR_DELETE(&device->head.allocator, crgpu_timestamp_set_vulkan, set);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }
    set->_logical_device_vk = device->_logical_device_vk;
    set->_count             = count;
    set->_period_ns         = device->_properties_vk.limits.timestampPeriod;
    set->_shared            = device->_shared;
    CRGPU___LIVE_ADD(&set->_shared->live, CRGPU___OBJECT_TIMESTAMP_SET);
    *out_set = set;
    return CR_SUCCESS;
}

void crgpu_timestamp_set_destroy(crgpu_timestamp_set set) {
    if (!set) return;
    if (CR_STALE(set, "crgpu_timestamp_set_destroy", "set")) return;
    vkDestroyQueryPool(set->_logical_device_vk, set->_query_pool_vk, NULL);
    CRGPU___LIVE_REMOVE(&set->_shared->live, CRGPU___OBJECT_TIMESTAMP_SET);
    CR_KILL(set);
    const CR_ALLOCATOR allocator = set->head.allocator;
    cr_string_free(&allocator, set->head.debug_name);
    CR_RETIRE(CR_QUARANTINE_OF(set), &allocator, crgpu_timestamp_set_vulkan, set);
}

void crgpu_command_list_reset_timestamps(
    crgpu_command_list cmd,
    crgpu_timestamp_set set,
    uint32_t first,
    uint32_t count) {
    if (!cmd || !set || count == 0) return;
    if (CR_STALE(cmd, "crgpu_command_list_reset_timestamps", "cmd")) return;
    if (CR_STALE(set, "crgpu_command_list_reset_timestamps", "set")) return;
    if (first >= set->_count || count > set->_count - first) return;
    vkCmdResetQueryPool(cmd->_command_buffer_vk, set->_query_pool_vk, first, count);
}

void crgpu_command_list_resolve_timestamps(
    crgpu_command_list cmd,
    crgpu_timestamp_set set,
    uint32_t first,
    uint32_t count) {
    // The results are read from the pool itself (crgpu_timestamp_set_get).
    if (!cmd || !set)
        return;
    if (CR_STALE(cmd, "crgpu_command_list_resolve_timestamps", "cmd"))
        return;
    if (CR_STALE(set, "crgpu_command_list_resolve_timestamps", "set"))
        return;
    (void)first;
    (void)count;
}

CR_RESULT crgpu_timestamp_set_get(
    crgpu_timestamp_set set,
    uint32_t first,
    uint32_t count,
    uint64_t *out_ns) {
    if (!set) return CR_RESULT_GPU_INVALID_TIMESTAMP_SET;
    if (CR_STALE(set, "crgpu_timestamp_set_get", "set")) return CR_RESULT_GPU_INVALID_TIMESTAMP_SET;
    if (count == 0) return CR_RESULT_ZERO_SIZE;
    if (first >= set->_count || count > set->_count - first) return CR_RESULT_INDEX_OUT_OF_RANGE;
    if (!out_ns) return CR_RESULT_NULL_OUTPUT_PARAMETER;

    // No wait: the caller reads after the resolving submission has finished
    // (crgpu.h). VK_NOT_READY leaves an unwritten entry's value as it was,
    // which crgpu.h calls undefined.
    VkResult r = vkGetQueryPoolResults(set->_logical_device_vk, set->_query_pool_vk, first, count, sizeof(uint64_t) * count, out_ns,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY) return CR_RESULT_API_SPECIFIC_FAILURE;
    for (uint32_t i = 0; i < count; ++i)
        out_ns[i] = (uint64_t)((double)out_ns[i] * (double)set->_period_ns);
    return CR_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // CRGPU_COMPILE_VULKAN_API
