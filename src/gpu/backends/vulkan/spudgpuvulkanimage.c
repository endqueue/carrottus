
#if SPUDGPU_COMPILE_VULKAN_API

#include <vulkan/vulkan.h>
#include "spudgpu.h"
#include "spudgpuvulkan.h"
#include <stdlib.h>
#include <string.h>
#include "../../../spudstructbase.h"

extern uint32_t spudgpuvulkan___find_memory_type_internal(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties);

static void spudgpuvulkan___image_usage_flags_internal(
    uint32_t __spud_gpu_image_usage,
    VkImageUsageFlags *output) {
    VkImageUsageFlags flags = 0;

    if (__spud_gpu_image_usage & SPUDGPU_IMAGE_USAGE_SAMPLED)
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;

    if (__spud_gpu_image_usage & SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT)
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    if (__spud_gpu_image_usage & SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT)
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    if (__spud_gpu_image_usage & SPUDGPU_IMAGE_USAGE_STORAGE)
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;

    if (__spud_gpu_image_usage & SPUDGPU_IMAGE_USAGE_TRANSFER_SRC)
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (__spud_gpu_image_usage & SPUDGPU_IMAGE_USAGE_TRANSFER_DST)
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (__spud_gpu_image_usage & SPUDGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT) {
        // spudgpu_create_image already rejected combining this with any
        // other usage bit, so none of the above ran - only the transient
        // bit itself and whichever attachment bit(s) validation required
        // are relevant here.
        flags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        *output = flags;
        return;
    }

    // Uploading data to device-local images always requires transfer dst
    flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    *output = flags;
}

static void spudgpuvulkan___image_type_internal(
    uint32_t ___spud_gpu_image_type,
    VkImageType *output) {
    VkImageType flags = 0;
    if (___spud_gpu_image_type & SPUDGPU_IMAGE_TYPE_1D)
        flags |= VK_IMAGE_TYPE_1D;
    if (___spud_gpu_image_type & SPUDGPU_IMAGE_TYPE_2D)
        flags |= VK_IMAGE_TYPE_2D;
    if (___spud_gpu_image_type & SPUDGPU_IMAGE_TYPE_3D)
        flags |= VK_IMAGE_TYPE_3D;
    *output = flags;
}

static void spudgpuvulkan___image_view_type_internal(
    uint32_t ___spud_gpu_image_view_type,
    VkImageViewType *output) {
    VkImageViewType flags = 0;
    if (___spud_gpu_image_view_type & SPUDGPU_IMAGE_VIEW_TYPE_1D)
        flags |= VK_IMAGE_VIEW_TYPE_1D;
    if (___spud_gpu_image_view_type & SPUDGPU_IMAGE_VIEW_TYPE_2D)
        flags |= VK_IMAGE_VIEW_TYPE_2D;
    if (___spud_gpu_image_view_type & SPUDGPU_IMAGE_VIEW_TYPE_3D)
        flags |= VK_IMAGE_VIEW_TYPE_3D;
    if (___spud_gpu_image_view_type & SPUDGPU_IMAGE_VIEW_TYPE_CUBE)
        flags |= VK_IMAGE_VIEW_TYPE_CUBE;
    if (___spud_gpu_image_view_type & SPUDGPU_IMAGE_VIEW_TYPE_1D_ARRAY)
        flags |= VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    if (___spud_gpu_image_view_type & SPUDGPU_IMAGE_VIEW_TYPE_2D_ARRAY)
        flags |= VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (___spud_gpu_image_view_type & SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        flags |= VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    *output = flags;
}

SPUDRESULT spudgpuvulkan___check_image_desc(const spudgpu_image_desc *desc) {
    if (!desc->width || !desc->height || !desc->depth || !desc->array_layers || !desc->mip_levels)
        return SPUDRESULT_DESC_INVALID_PARAMETERS;
    if (desc->usage == 0) return SPUDRESULT_GPU_INVALID_IMAGE_USAGE;

    // SPUDGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT's contract (see
    // spudgpu.h) is enforced identically on every backend, not just the
    // ones that act on it - Vulkan's own spec imposes the same
    // restriction on VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT (it must
    // not be combined with any usage besides the attachment bits), so
    // this isn't a Metal-specific rule being forced onto Vulkan.
    if (desc->usage & SPUDGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT) {
        if (!(desc->usage & (SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT | SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT)))
            return SPUDRESULT_GPU_INVALID_IMAGE_USAGE;
        if (desc->usage & (SPUDGPU_IMAGE_USAGE_SAMPLED | SPUDGPU_IMAGE_USAGE_STORAGE |
                            SPUDGPU_IMAGE_USAGE_TRANSFER_SRC | SPUDGPU_IMAGE_USAGE_TRANSFER_DST |
                            SPUDGPU_IMAGE_USAGE_PRESENTABLE))
            return SPUDRESULT_GPU_INVALID_IMAGE_USAGE;
    }
    // spudgpu_image_desc::cube_capable's contract (see spudgpu.h) is the
    // same on every backend, though only Vulkan acts on it at creation.
    if (desc->cube_capable) {
        if (desc->type != SPUDGPU_IMAGE_TYPE_2D) return SPUDRESULT_GPU_INVALID_IMAGE_TYPE;
        if (desc->width != desc->height || desc->array_layers % 6 != 0)
            return SPUDRESULT_DESC_INVALID_PARAMETERS;
    }
    if (desc->format == SPUDGPU_FORMAT_UNKNOWN) return SPUDRESULT_GPU_INVALID_FORMAT;
    return SPUD_SUCCESS;
}

void spudgpuvulkan___image_create_info(
    const spudgpu_image_desc *desc,
    VkImageCreateInfo *out_info) {
    memset(out_info, 0, sizeof(*out_info));
    out_info->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    out_info->pNext = NULL;
    // A cube view can only be made of an image created with this bit. It is
    // the one thing here the caller's cube_capable changes: Vulkan is the
    // API that has to be told at creation.
    out_info->flags = desc->cube_capable ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    out_info->mipLevels = desc->mip_levels;
    out_info->arrayLayers = desc->array_layers;
    out_info->format = convert_spud_to_vulkan_format(desc->format);
    out_info->tiling = VK_IMAGE_TILING_OPTIMAL;
    out_info->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    out_info->samples = VK_SAMPLE_COUNT_1_BIT;
    out_info->sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    spudgpuvulkan___image_usage_flags_internal(desc->usage, &out_info->usage);
    spudgpuvulkan___image_type_internal(desc->type, &out_info->imageType);

    // Determine the Image Type and Width Height Depth
    out_info->extent.width = desc->width;
    out_info->extent.height = desc->height;
    out_info->extent.depth = desc->depth;
}

#if __cplusplus
extern "C" {
#endif

SPUDRESULT spudgpu_create_image(
    spudgpu_device device,
    const spudgpu_image_desc *desc,
    spudgpu_image *out_image) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_create_image", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!desc) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_IMAGE_DESC);
    if (base_sr != SPUD_SUCCESS) return base_sr;
    if (!out_image) return SPUDRESULT_NULL_OUTPUT_PARAMETER;

    const SPUDRESULT descResult = spudgpuvulkan___check_image_desc(desc);
    if (descResult != SPUD_SUCCESS) return descResult;

    // Create the result struct
    spudgpu_image_vulkan result = {0};
    result.head.allocator = device->head.allocator;
    SPUD_BIRTH_IN(&result, &device->_shared->quarantine);
    result._device = *device;
    result._desc = *desc;
    result._format_vk = convert_spud_to_vulkan_format(result._desc.format);

    // Get native Vulkan device handles

    VkDevice vk_device = result._device._logical_device_vk;
    VkPhysicalDevice vk_physical_device = result._device._physical_device_vk;

    VkImageCreateInfo imageInfo;
    spudgpuvulkan___image_create_info(&result._desc, &imageInfo);

    if (vkCreateImage(vk_device, &imageInfo, NULL, &result._image_vk) != VK_SUCCESS) {
        //throw std::runtime_error("Failed to create vulkan image!");
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }

    // Create the Vulkan Memory
    {
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(vk_device, result._image_vk, &memRequirements);

        uint32_t memoryTypeIndex = UINT32_MAX;
        if (result._desc.usage & SPUDGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT) {
            // Lazily-allocated memory is an optimization opportunity, not a
            // guarantee - implementations without tile-memory-backed
            // allocation (most desktop GPUs) simply don't expose a memory
            // type with this property, and VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT
            // still permits falling back to a normal DEVICE_LOCAL
            // allocation in that case (unlike Metal's MTLStorageModeMemoryless,
            // which is a hard requirement with no such fallback).
            memoryTypeIndex = spudgpuvulkan___find_memory_type_internal(
                vk_physical_device, memRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        if (memoryTypeIndex == UINT32_MAX) {
            memoryTypeIndex = spudgpuvulkan___find_memory_type_internal(
                vk_physical_device, memRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }

        VkMemoryAllocateInfo allocInfo = {0};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = memoryTypeIndex;
        if (vkAllocateMemory(vk_device, &allocInfo, NULL, &result._memory_vk) != VK_SUCCESS) {
            //throw std::runtime_error("Failed to allocate image memory!");
            vkDestroyImage(vk_device, result._image_vk, NULL);
            return SPUDRESULT_API_SPECIFIC_FAILURE;
        }

        if (vkBindImageMemory(vk_device, result._image_vk, result._memory_vk, 0) != VK_SUCCESS) {
            vkDestroyImage(vk_device, result._image_vk, NULL);
            vkFreeMemory(vk_device, result._memory_vk, NULL);
            return SPUDRESULT_API_SPECIFIC_FAILURE;
        }
    }

    // If all successful, return a memcpy'ed heap pointer
    spudgpu_image_vulkan *pResult = SPUD_NEW(&result.head.allocator, spudgpu_image_vulkan);
    if (!pResult) {
        vkDestroyImage(vk_device, result._image_vk, NULL);
        vkFreeMemory(vk_device, result._memory_vk, NULL);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    memcpy(pResult, &result, sizeof(spudgpu_image_vulkan));
    // From here spudgpu_destroy_image frees it, and takes the count back.
    SPUDGPU___LIVE_ADD(&pResult->_device._shared->live, SPUDGPU___OBJECT_IMAGE);
    if (spud_debug_name_set(pResult, desc->debug_name) != SPUD_SUCCESS) {
        spudgpu_destroy_image(pResult);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    pResult->_desc.debug_name = pResult->head.debug_name;

    *out_image = pResult;
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_image_desc(
    spudgpu_image image,
    spudgpu_image_desc *out_desc) {
    if (!image) return SPUDRESULT_GPU_INVALID_IMAGE;
    if (SPUD_STALE(image, "spudgpu_get_image_desc", "image")) return SPUDRESULT_GPU_INVALID_IMAGE;
    if (!out_desc) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    *out_desc = image->_desc;
    out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_IMAGE_DESC);
    return SPUD_SUCCESS;
}

void spudgpu_destroy_image(
    spudgpu_image image) {
    if (!image) return;
    if (SPUD_STALE(image, "spudgpu_destroy_image", "image")) return;
    vkDestroyImage(image->_device._logical_device_vk, image->_image_vk, NULL);
    // A placed image's memory is its heap's.
    if (!image->_heap)
        vkFreeMemory(image->_device._logical_device_vk, image->_memory_vk, NULL);
#if _DEBUG
    if (image->_heap)
        image->_heap->_placed_count--;
#endif
    SPUDGPU___LIVE_REMOVE(&image->_device._shared->live, SPUDGPU___OBJECT_IMAGE);
    SPUD_KILL(image);
    const SPUD_ALLOCATOR allocator = image->head.allocator;
    spud_string_free(&allocator, image->head.debug_name);
    SPUD_RETIRE(SPUD_QUARANTINE_OF(image), &allocator, spudgpu_image_vulkan, image);
}

// The aspects [format] has (SPUDGPU_IMAGE_ASPECT). The same table in every
// backend: it decides which views spudgpu_create_image_view refuses.
static uint64_t spudgpuvulkan___internal_format_aspects(SPUDGPU_FORMAT format) {
    switch (format) {
    case SPUDGPU_FORMAT_D24_UNORM_S8_UINT:
    case SPUDGPU_FORMAT_D32_FLOAT_S8X24_UINT:
        return SPUDGPU_IMAGE_ASPECT_DEPTH | SPUDGPU_IMAGE_ASPECT_STENCIL;
    case SPUDGPU_FORMAT_D16_UNORM:
    case SPUDGPU_FORMAT_D32_FLOAT:
        return SPUDGPU_IMAGE_ASPECT_DEPTH;
    default:
        return SPUDGPU_IMAGE_ASPECT_COLOR;
    }
}

SPUDRESULT spudgpu_create_image_view(
    spudgpu_image image,
    const spudgpu_image_view_desc *desc,
    spudgpu_image_view *out_image_view) {
    if (!image) return SPUDRESULT_GPU_INVALID_IMAGE;
    if (SPUD_STALE(image, "spudgpu_create_image_view", "image")) return SPUDRESULT_GPU_INVALID_IMAGE;
    if (!desc) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC);
    if (base_sr != SPUD_SUCCESS) return base_sr;
    if (!out_image_view) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    // The cube view rules of spudgpu.h, the same on every backend.
    if (desc->type == SPUDGPU_IMAGE_VIEW_TYPE_CUBE || desc->type == SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        if (!image->_desc.cube_capable) return SPUDRESULT_GPU_INVALID_IMAGE_VIEW_TYPE;
        // Without the feature imageCubeArray was never switched on.
        if (desc->type == SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY && !(image->_device._features & SPUDGPU_FEATURE_CUBE_ARRAY_VIEWS))
            return SPUDRESULT_GPU_INVALID_IMAGE_VIEW_TYPE;
        if (desc->type == SPUDGPU_IMAGE_VIEW_TYPE_CUBE &&
            (desc->subresource_range.base_array_layer != 0 || desc->subresource_range.array_layer_count != 6))
            return SPUDRESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;
        if (desc->type == SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY &&
            (desc->subresource_range.array_layer_count == 0 || desc->subresource_range.array_layer_count % 6 != 0))
            return SPUDRESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;
    }
    // The aspect rule of spudgpu.h, the same on every backend.
    if (desc->subresource_range.aspect_mask == SPUDGPU_IMAGE_ASPECT_NONE) return SPUDRESULT_DESC_INVALID_PARAMETERS;
    if (desc->subresource_range.aspect_mask & ~spudgpuvulkan___internal_format_aspects(image->_desc.format))
        return SPUDRESULT_DESC_INVALID_PARAMETERS;

    spudgpu_image_view_vulkan result = {0};
    result.head.allocator = image->head.allocator;
    SPUD_BIRTH_IN(&result, &image->_device._shared->quarantine);
    result._desc = *desc;
    result._desc.parent_image = image;

    VkImageViewCreateInfo viewInfo = {0};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.pNext = NULL;
    viewInfo.image = image->_image_vk;
    viewInfo.format = image->_format_vk;

    // Map image type to view type
    spudgpuvulkan___image_view_type_internal(result._desc.type, &viewInfo.viewType);

    // Standard RGBA component swizzle (identity)
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    // Subresource range — which mip levels and array layers this view covers.
    // SPUDGPU_IMAGE_ASPECT's values are VkImageAspectFlagBits', so this is
    // a direct passthrough. A view of the stencil alone is what a shader
    // samples the stencil through.
    viewInfo.subresourceRange.aspectMask = (VkImageAspectFlags)desc->subresource_range.aspect_mask;
    viewInfo.subresourceRange.baseMipLevel = desc->subresource_range.base_mip_level;
    viewInfo.subresourceRange.levelCount = desc->subresource_range.mip_level_count;
    viewInfo.subresourceRange.baseArrayLayer = desc->subresource_range.base_array_layer;
    viewInfo.subresourceRange.layerCount = desc->subresource_range.array_layer_count;

    if (vkCreateImageView(
            image->_device._logical_device_vk,
            &viewInfo, NULL,
            &result._image_view_vk) != VK_SUCCESS) {
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }

    // If all successful, return a memcpy'ed heap pointer
    spudgpu_image_view_vulkan *pResult = SPUD_NEW(&result.head.allocator, spudgpu_image_view_vulkan);
    if (!pResult) {
        vkDestroyImageView(image->_device._logical_device_vk, result._image_view_vk, NULL);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    memcpy(pResult, &result, sizeof(spudgpu_image_view_vulkan));
    pResult->_shared = image->_device._shared;
    SPUDGPU___LIVE_ADD(&pResult->_shared->live, SPUDGPU___OBJECT_IMAGE_VIEW);
    *out_image_view = pResult;
    return SPUD_SUCCESS;
}

void spudgpu_destroy_image_view(spudgpu_image_view image_view) {
    if (!image_view) return;
    if (SPUD_STALE(image_view, "spudgpu_destroy_image_view", "image_view")) return;
    vkDestroyImageView(image_view->_desc.parent_image->_device._logical_device_vk, image_view->_image_view_vk, NULL);
    SPUDGPU___LIVE_REMOVE(&image_view->_shared->live, SPUDGPU___OBJECT_IMAGE_VIEW);
    SPUD_KILL(image_view);
    const SPUD_ALLOCATOR allocator = image_view->head.allocator;
    spud_string_free(&allocator, image_view->head.debug_name);
    SPUD_RETIRE(SPUD_QUARANTINE_OF(image_view), &allocator, spudgpu_image_view_vulkan, image_view);
}

SPUDRESULT spudgpu_get_image_view_desc(
    spudgpu_image_view image_view,
    spudgpu_image_view_desc *out_desc) {
    if (!image_view) return SPUDRESULT_GPU_INVALID_IMAGE_VIEW;
    if (SPUD_STALE(image_view, "spudgpu_get_image_view_desc", "image_view")) return SPUDRESULT_GPU_INVALID_IMAGE_VIEW;
    if (!out_desc) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    *out_desc = image_view->_desc;
    out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC);
    return SPUD_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // SPUDGPU_COMPILE_VULKAN_API
