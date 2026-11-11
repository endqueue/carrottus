#if CRGPU_COMPILE_VULKAN_API

#include "crgpuvulkan.h"
#include <stdint.h>

#if __cplusplus
extern "C" {

#endif

VkInstance crgpu_get_vk_instance(crgpu_instance instance) {
    return instance ? instance->_instance_vk : NULL;
}

VkPhysicalDevice crgpu_get_vk_physical_device(crgpu_adapter adapter) {
    return adapter ? adapter->_physical_device_vk : NULL;
}

VkDevice crgpu_get_vk_device(crgpu_device device) {
    return device ? device->_logical_device_vk : NULL;
}

VkQueue crgpu_get_vk_queue(crgpu_command_queue queue) {
    return queue ? queue->_queue_vk : NULL;
}

uint32_t crgpu_get_vk_queue_family_index(crgpu_command_queue queue) {
    return queue ? queue->_queue_family_index : UINT32_MAX;
}

VkCommandBuffer crgpu_get_vk_command_buffer(crgpu_command_list cmd) {
    return cmd ? cmd->_command_buffer_vk : NULL;
}

VkImageView crgpu_get_vk_image_view(crgpu_image_view view) {
    if (!view) return NULL;
    // VkImageView is a non-dispatchable handle (uint64_t in the Vulkan spec).
    // The reinterpret through uintptr_t is lossless on 64-bit targets.
    return ((crgpu_image_view_vulkan *) view)->_image_view_vk;
}

VkDeviceMemory crgpu_get_vk_heap_memory(crgpu_heap heap) {
    return heap ? heap->_memory_vk : VK_NULL_HANDLE;
}

VkFormat crgpu_get_vk_swap_chain_format(crgpu_swap_chain swap_chain) {
    return swap_chain ? swap_chain->_format_vk : VK_FORMAT_UNDEFINED;
}

#if __cplusplus
}
#endif

#endif // CRGPU_COMPILE_VULKAN_API
