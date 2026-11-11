//
// Created by Nathan on 5/23/2026.
//

/**
 * @file crgpu_vulkan_natives.h
 * @brief Escape hatch: the raw Vulkan handles behind Carrottus GPU's opaque
 * objects.
 *
 * Use only for interop with third-party Vulkan libraries (e.g. ImGui,
 * RenderDoc). Carrottus GPU has no visibility into Vulkan calls made with these
 * handles. Declared only when CRGPU_COMPILE_VULKAN_API is set.
 *
 * Every accessor returns a handle Carrottus GPU still owns: it stays valid for as
 * long as the Carrottus GPU object it came from, and the caller never destroys it.
 * Each returns NULL (VK_NULL_HANDLE, or the value its own doc names) when
 * given a NULL object.
 */

#ifndef CARROTTUS_CRGPU_VULKAN_NATIVES_H
#define CARROTTUS_CRGPU_VULKAN_NATIVES_H

#if CRGPU_COMPILE_VULKAN_API

#include <vulkan/vulkan.h>
#include <crgpu.h>

#ifdef __cplusplus
extern "C" {

#endif

/**
 * @brief Returns the VkInstance behind @p instance.
 *
 * @param[in] instance The Carrottus GPU instance.
 *
 * @return Its VkInstance, or NULL if @p instance is NULL.
 */
VkInstance crgpu_get_vk_instance(crgpu_instance instance);

/**
 * @brief Returns the VkPhysicalDevice behind @p adapter.
 *
 * An adapter is a VkPhysicalDevice.
 *
 * @param[in] adapter The adapter, as crgpu_adapter_enumerate() gave it.
 *
 * @return Its VkPhysicalDevice, or NULL if @p adapter is NULL.
 */
VkPhysicalDevice crgpu_get_vk_physical_device(crgpu_adapter adapter);

/**
 * @brief Returns the VkDevice behind @p device.
 *
 * @param[in] device The Carrottus GPU device.
 *
 * @return Its VkDevice, or NULL if @p device is NULL.
 */
VkDevice crgpu_get_vk_device(crgpu_device device);

/**
 * @brief Returns the VkQueue behind @p queue.
 *
 * The caller synchronizes a queue (see crgpu_command_queue_submit_lists()), and
 * a use of the VkQueue itself - a submit, present or wait by an ImGui or
 * OpenXR backend, say - is one more use of the same queue under the same
 * rule.
 *
 * @param[in] queue The Carrottus GPU command queue.
 *
 * @return Its VkQueue, or NULL if @p queue is NULL.
 *
 * @see crgpu_get_vk_queue_family_index()
 */
VkQueue crgpu_get_vk_queue(crgpu_command_queue queue);

/**
 * @brief Returns the Vulkan queue family @p queue was created from.
 *
 * @param[in] queue The Carrottus GPU command queue.
 *
 * @return The queue family index, or UINT32_MAX if @p queue is NULL.
 */
uint32_t crgpu_get_vk_queue_family_index(crgpu_command_queue queue);

/**
 * @brief Returns the VkCommandBuffer behind @p cmd.
 *
 * @param[in] cmd The Carrottus GPU command list.
 *
 * @return Its VkCommandBuffer, or NULL if @p cmd is NULL.
 */
VkCommandBuffer crgpu_get_vk_command_buffer(crgpu_command_list cmd);

/**
 * @brief Returns the VkImageView behind @p view.
 *
 * @param[in] view The Carrottus GPU image view.
 *
 * @return Its VkImageView, or VK_NULL_HANDLE if @p view is NULL.
 */
VkImageView crgpu_get_vk_image_view(crgpu_image_view view);

/**
 * @brief Returns the VkDeviceMemory a heap is.
 *
 * The one allocation a heap is, which its placed buffers and images are
 * bound to at their offsets. A heap of a mappable kind keeps this memory
 * mapped for as long as it exists, so it is never mapped again with
 * vkMapMemory.
 *
 * @param[in] heap The Carrottus GPU heap.
 *
 * @return Its VkDeviceMemory, or VK_NULL_HANDLE if @p heap is NULL.
 *
 * @see crgpu_heap_create()
 */
VkDeviceMemory crgpu_get_vk_heap_memory(crgpu_heap heap);

/**
 * @brief Returns the VkFormat of @p swap_chain's images.
 *
 * @param[in] swap_chain The Carrottus GPU swap chain.
 *
 * @return Its images' VkFormat, or VK_FORMAT_UNDEFINED if @p swap_chain is
 *         NULL.
 */
VkFormat crgpu_get_vk_swap_chain_format(crgpu_swap_chain swap_chain);

#ifdef __cplusplus
}
#endif

#endif // CRGPU_COMPILE_VULKAN_API

#endif //CARROTTUS_CRGPU_VULKAN_NATIVES_H
