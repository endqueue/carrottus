
#if CRGPU_COMPILE_VULKAN_API

#include <vulkan/vulkan.h>
#include "crgpu.h"
#include "crgpuvulkan.h"
#include <stdlib.h>
#include <string.h>
#include "../../../crstructbase.h"

extern uint32_t crgpuvulkan___find_memory_type_internal(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties);

static void crgpuvulkan___image_usage_flags_internal(
    uint32_t __cr_gpu_image_usage,
    VkImageUsageFlags *output) {
    VkImageUsageFlags flags = 0;

    if (__cr_gpu_image_usage & CRGPU_IMAGE_USAGE_SAMPLED)
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;

    if (__cr_gpu_image_usage & CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT)
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    if (__cr_gpu_image_usage & CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT)
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    if (__cr_gpu_image_usage & CRGPU_IMAGE_USAGE_STORAGE)
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;

    if (__cr_gpu_image_usage & CRGPU_IMAGE_USAGE_TRANSFER_SRC)
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (__cr_gpu_image_usage & CRGPU_IMAGE_USAGE_TRANSFER_DST)
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (__cr_gpu_image_usage & CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT) {
        // crgpu_image_create already rejected combining this with any
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

static void crgpuvulkan___image_type_internal(
    uint32_t ___cr_gpu_image_type,
    VkImageType *output) {
    VkImageType flags = 0;
    if (___cr_gpu_image_type & CRGPU_IMAGE_TYPE_1D)
        flags |= VK_IMAGE_TYPE_1D;
    if (___cr_gpu_image_type & CRGPU_IMAGE_TYPE_2D)
        flags |= VK_IMAGE_TYPE_2D;
    if (___cr_gpu_image_type & CRGPU_IMAGE_TYPE_3D)
        flags |= VK_IMAGE_TYPE_3D;
    *output = flags;
}

static void crgpuvulkan___image_view_type_internal(
    uint32_t ___cr_gpu_image_view_type,
    VkImageViewType *output) {
    VkImageViewType flags = 0;
    if (___cr_gpu_image_view_type & CRGPU_IMAGE_VIEW_TYPE_1D)
        flags |= VK_IMAGE_VIEW_TYPE_1D;
    if (___cr_gpu_image_view_type & CRGPU_IMAGE_VIEW_TYPE_2D)
        flags |= VK_IMAGE_VIEW_TYPE_2D;
    if (___cr_gpu_image_view_type & CRGPU_IMAGE_VIEW_TYPE_3D)
        flags |= VK_IMAGE_VIEW_TYPE_3D;
    if (___cr_gpu_image_view_type & CRGPU_IMAGE_VIEW_TYPE_CUBE)
        flags |= VK_IMAGE_VIEW_TYPE_CUBE;
    if (___cr_gpu_image_view_type & CRGPU_IMAGE_VIEW_TYPE_1D_ARRAY)
        flags |= VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    if (___cr_gpu_image_view_type & CRGPU_IMAGE_VIEW_TYPE_2D_ARRAY)
        flags |= VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (___cr_gpu_image_view_type & CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        flags |= VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    *output = flags;
}

CR_RESULT crgpuvulkan___check_image_desc(const crgpu_image_desc *desc) {
    if (!desc->width || !desc->height || !desc->depth || !desc->array_layers || !desc->mip_levels)
        return CR_RESULT_DESC_INVALID_PARAMETERS;
    if (desc->usage == 0) return CR_RESULT_GPU_INVALID_IMAGE_USAGE;

    // CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT's contract (see
    // crgpu.h) is enforced identically on every backend, not just the
    // ones that act on it - Vulkan's own spec imposes the same
    // restriction on VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT (it must
    // not be combined with any usage besides the attachment bits), so
    // this isn't a Metal-specific rule being forced onto Vulkan.
    if (desc->usage & CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT) {
        if (!(desc->usage & (CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT)))
            return CR_RESULT_GPU_INVALID_IMAGE_USAGE;
        if (desc->usage & (CRGPU_IMAGE_USAGE_SAMPLED | CRGPU_IMAGE_USAGE_STORAGE |
                            CRGPU_IMAGE_USAGE_TRANSFER_SRC | CRGPU_IMAGE_USAGE_TRANSFER_DST |
                            CRGPU_IMAGE_USAGE_PRESENTABLE))
            return CR_RESULT_GPU_INVALID_IMAGE_USAGE;
    }
    // crgpu_image_desc::cube_capable's contract (see crgpu.h) is the
    // same on every backend, though only Vulkan acts on it at creation.
    if (desc->cube_capable) {
        if (desc->type != CRGPU_IMAGE_TYPE_2D) return CR_RESULT_GPU_INVALID_IMAGE_TYPE;
        if (desc->width != desc->height || desc->array_layers % 6 != 0)
            return CR_RESULT_DESC_INVALID_PARAMETERS;
    }
    if (desc->format == CRGPU_FORMAT_UNKNOWN) return CR_RESULT_GPU_INVALID_FORMAT;
    return CR_SUCCESS;
}

void crgpuvulkan___image_create_info(
    const crgpu_image_desc *desc,
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
    out_info->format = convert_cr_to_vulkan_format(desc->format);
    out_info->tiling = VK_IMAGE_TILING_OPTIMAL;
    out_info->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    out_info->samples = VK_SAMPLE_COUNT_1_BIT;
    out_info->sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    crgpuvulkan___image_usage_flags_internal(desc->usage, &out_info->usage);
    crgpuvulkan___image_type_internal(desc->type, &out_info->imageType);

    // Determine the Image Type and Width Height Depth
    out_info->extent.width = desc->width;
    out_info->extent.height = desc->height;
    out_info->extent.depth = desc->depth;
}

#if __cplusplus
extern "C" {
#endif

CR_RESULT crgpu_image_create(
    crgpu_device device,
    const crgpu_image_desc *desc,
    crgpu_image *out_image) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_image_create", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!desc) return CR_RESULT_NULL_DESC;
    const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_IMAGE_DESC);
    if (base_sr != CR_SUCCESS) return base_sr;
    if (!out_image) return CR_RESULT_NULL_OUTPUT_PARAMETER;

    const CR_RESULT descResult = crgpuvulkan___check_image_desc(desc);
    if (descResult != CR_SUCCESS) return descResult;

    // Create the result struct
    crgpu_image_vulkan result = {0};
    result.head.allocator = device->head.allocator;
    CR_BIRTH_IN(&result, &device->_shared->quarantine);
    result._device = *device;
    result._desc = *desc;
    result._format_vk = convert_cr_to_vulkan_format(result._desc.format);

    // Get native Vulkan device handles

    VkDevice vk_device = result._device._logical_device_vk;
    VkPhysicalDevice vk_physical_device = result._device._physical_device_vk;

    VkImageCreateInfo imageInfo;
    crgpuvulkan___image_create_info(&result._desc, &imageInfo);

    if (vkCreateImage(vk_device, &imageInfo, NULL, &result._image_vk) != VK_SUCCESS) {
        //throw std::runtime_error("Failed to create vulkan image!");
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

    // Create the Vulkan Memory
    {
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(vk_device, result._image_vk, &memRequirements);

        uint32_t memoryTypeIndex = UINT32_MAX;
        if (result._desc.usage & CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT) {
            // Lazily-allocated memory is an optimization opportunity, not a
            // guarantee - implementations without tile-memory-backed
            // allocation (most desktop GPUs) simply don't expose a memory
            // type with this property, and VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT
            // still permits falling back to a normal DEVICE_LOCAL
            // allocation in that case (unlike Metal's MTLStorageModeMemoryless,
            // which is a hard requirement with no such fallback).
            memoryTypeIndex = crgpuvulkan___find_memory_type_internal(
                vk_physical_device, memRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        if (memoryTypeIndex == UINT32_MAX) {
            memoryTypeIndex = crgpuvulkan___find_memory_type_internal(
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
            return CR_RESULT_API_SPECIFIC_FAILURE;
        }

        if (vkBindImageMemory(vk_device, result._image_vk, result._memory_vk, 0) != VK_SUCCESS) {
            vkDestroyImage(vk_device, result._image_vk, NULL);
            vkFreeMemory(vk_device, result._memory_vk, NULL);
            return CR_RESULT_API_SPECIFIC_FAILURE;
        }
    }

    // If all successful, return a memcpy'ed heap pointer
    crgpu_image_vulkan *pResult = CR_NEW(&result.head.allocator, crgpu_image_vulkan);
    if (!pResult) {
        vkDestroyImage(vk_device, result._image_vk, NULL);
        vkFreeMemory(vk_device, result._memory_vk, NULL);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    memcpy(pResult, &result, sizeof(crgpu_image_vulkan));
    // From here crgpu_image_destroy frees it, and takes the count back.
    CRGPU___LIVE_ADD(&pResult->_device._shared->live, CRGPU___OBJECT_IMAGE);
    if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
        crgpu_image_destroy(pResult);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    pResult->_desc.debug_name = pResult->head.debug_name;

    *out_image = pResult;
    return CR_SUCCESS;
}

CR_RESULT crgpu_image_get_desc(
    crgpu_image image,
    crgpu_image_desc *out_desc) {
    if (!image) return CR_RESULT_GPU_INVALID_IMAGE;
    if (CR_STALE(image, "crgpu_image_get_desc", "image")) return CR_RESULT_GPU_INVALID_IMAGE;
    if (!out_desc) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    *out_desc = image->_desc;
    out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_IMAGE_DESC);
    return CR_SUCCESS;
}

void crgpu_image_destroy(
    crgpu_image image) {
    if (!image) return;
    if (CR_STALE(image, "crgpu_image_destroy", "image")) return;
    vkDestroyImage(image->_device._logical_device_vk, image->_image_vk, NULL);
    // A placed image's memory is its heap's.
    if (!image->_heap)
        vkFreeMemory(image->_device._logical_device_vk, image->_memory_vk, NULL);
#if _DEBUG
    if (image->_heap)
        image->_heap->_placed_count--;
#endif
    CRGPU___LIVE_REMOVE(&image->_device._shared->live, CRGPU___OBJECT_IMAGE);
    CR_KILL(image);
    const CR_ALLOCATOR allocator = image->head.allocator;
    cr_string_free(&allocator, image->head.debug_name);
    CR_RETIRE(CR_QUARANTINE_OF(image), &allocator, crgpu_image_vulkan, image);
}

// The aspects [format] has (CRGPU_IMAGE_ASPECT). The same table in every
// backend: it decides which views crgpu_image_view_create refuses.
static uint64_t crgpuvulkan___internal_format_aspects(CRGPU_FORMAT format) {
    switch (format) {
    case CRGPU_FORMAT_D24_UNORM_S8_UINT:
    case CRGPU_FORMAT_D32_FLOAT_S8X24_UINT:
        return CRGPU_IMAGE_ASPECT_DEPTH | CRGPU_IMAGE_ASPECT_STENCIL;
    case CRGPU_FORMAT_D16_UNORM:
    case CRGPU_FORMAT_D32_FLOAT:
        return CRGPU_IMAGE_ASPECT_DEPTH;
    default:
        return CRGPU_IMAGE_ASPECT_COLOR;
    }
}

CR_RESULT crgpu_image_view_create(
    crgpu_image image,
    const crgpu_image_view_desc *desc,
    crgpu_image_view *out_image_view) {
    if (!image) return CR_RESULT_GPU_INVALID_IMAGE;
    if (CR_STALE(image, "crgpu_image_view_create", "image")) return CR_RESULT_GPU_INVALID_IMAGE;
    if (!desc) return CR_RESULT_NULL_DESC;
    const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC);
    if (base_sr != CR_SUCCESS) return base_sr;
    if (!out_image_view) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    // The cube view rules of crgpu.h, the same on every backend.
    if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE || desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        if (!image->_desc.cube_capable) return CR_RESULT_GPU_INVALID_IMAGE_VIEW_TYPE;
        // Without the feature imageCubeArray was never switched on.
        if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY && !(image->_device._features & CRGPU_FEATURE_CUBE_ARRAY_VIEWS))
            return CR_RESULT_GPU_INVALID_IMAGE_VIEW_TYPE;
        if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE &&
            (desc->subresource_range.base_array_layer != 0 || desc->subresource_range.array_layer_count != 6))
            return CR_RESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;
        if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY &&
            (desc->subresource_range.array_layer_count == 0 || desc->subresource_range.array_layer_count % 6 != 0))
            return CR_RESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;
    }
    // The aspect rule of crgpu.h, the same on every backend.
    if (desc->subresource_range.aspect_mask == CRGPU_IMAGE_ASPECT_NONE) return CR_RESULT_DESC_INVALID_PARAMETERS;
    if (desc->subresource_range.aspect_mask & ~crgpuvulkan___internal_format_aspects(image->_desc.format))
        return CR_RESULT_DESC_INVALID_PARAMETERS;

    crgpu_image_view_vulkan result = {0};
    result.head.allocator = image->head.allocator;
    CR_BIRTH_IN(&result, &image->_device._shared->quarantine);
    result._desc = *desc;
    result._desc.parent_image = image;

    VkImageViewCreateInfo viewInfo = {0};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.pNext = NULL;
    viewInfo.image = image->_image_vk;
    viewInfo.format = image->_format_vk;

    // Map image type to view type
    crgpuvulkan___image_view_type_internal(result._desc.type, &viewInfo.viewType);

    // Standard RGBA component swizzle (identity)
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    // Subresource range — which mip levels and array layers this view covers.
    // CRGPU_IMAGE_ASPECT's values are VkImageAspectFlagBits', so this is
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
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

    // If all successful, return a memcpy'ed heap pointer
    crgpu_image_view_vulkan *pResult = CR_NEW(&result.head.allocator, crgpu_image_view_vulkan);
    if (!pResult) {
        vkDestroyImageView(image->_device._logical_device_vk, result._image_view_vk, NULL);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    memcpy(pResult, &result, sizeof(crgpu_image_view_vulkan));
    pResult->_shared = image->_device._shared;
    CRGPU___LIVE_ADD(&pResult->_shared->live, CRGPU___OBJECT_IMAGE_VIEW);
    *out_image_view = pResult;
    return CR_SUCCESS;
}

void crgpu_image_view_destroy(crgpu_image_view image_view) {
    if (!image_view) return;
    if (CR_STALE(image_view, "crgpu_image_view_destroy", "image_view")) return;
    vkDestroyImageView(image_view->_desc.parent_image->_device._logical_device_vk, image_view->_image_view_vk, NULL);
    CRGPU___LIVE_REMOVE(&image_view->_shared->live, CRGPU___OBJECT_IMAGE_VIEW);
    CR_KILL(image_view);
    const CR_ALLOCATOR allocator = image_view->head.allocator;
    cr_string_free(&allocator, image_view->head.debug_name);
    CR_RETIRE(CR_QUARANTINE_OF(image_view), &allocator, crgpu_image_view_vulkan, image_view);
}

CR_RESULT crgpu_image_view_get_desc(
    crgpu_image_view image_view,
    crgpu_image_view_desc *out_desc) {
    if (!image_view) return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
    if (CR_STALE(image_view, "crgpu_image_view_get_desc", "image_view")) return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
    if (!out_desc) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    *out_desc = image_view->_desc;
    out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC);
    return CR_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // CRGPU_COMPILE_VULKAN_API
