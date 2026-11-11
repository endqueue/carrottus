
#if CRGPU_COMPILE_VULKAN_API

#include <vulkan/vulkan.h>
#include "crgpu.h"
#include "crgpuvulkan.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../../../crstructbase.h"

// Carrottus GPU heaps on Vulkan.
//
// A heap is one vkAllocateMemory, and a placed buffer or image is a VkBuffer
// or VkImage bound to it at the caller's offset. The memory type has to be
// chosen when the heap is allocated, before any resource exists, and every
// resource later placed in the heap has to be able to use it. So the heap
// creates probe resources and takes a type that all of them allow:
//
// - Buffers: one buffer with every usage the heap's kind allows. The
//   memoryTypeBits of a buffer with fewer usages is a superset of that
//   buffer's (Vulkan spec, VkMemoryRequirements), so the type fits every
//   buffer of the kind.
// - Images: memoryTypeBits are the same for every optimal-tiling image
//   without VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT and with the flags
//   Carrottus GPU sets, except that a depth/stencil format may have bits of its
//   own. So one color image, and one image of each depth/stencil format an
//   image of the class can be made in. Every image crgpu_image_create
//   makes has VK_IMAGE_USAGE_TRANSFER_DST_BIT (see
//   crgpuvulkan___image_create_info), so an IMAGES heap probes with that
//   usage alone, and a format that can't take it can't be an image at all;
//   an ATTACHMENT_IMAGES heap probes with the attachment usage, which every
//   image in it has.
//
// None of the probes is kept. A heap of a mappable kind is mapped, whole,
// from creation to destruction: see crgpu_heap_vulkan::_mapped.

// Every CRGPU_BUFFER_USAGE a buffer of [kind] may have (see
// CRGPU_MEMORY_KIND in crgpu.h, and crgpuvulkan___check_buffer_desc).
static CRGPU_BUFFER_USAGE crgpuvulkan___internal_kind_buffer_usages(CRGPU_MEMORY_KIND kind) {
    const CRGPU_BUFFER_USAGE all =
        CRGPU_BUFFER_USAGE_VERTEX | CRGPU_BUFFER_USAGE_INDEX | CRGPU_BUFFER_USAGE_UNIFORM |
        CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_SRC | CRGPU_BUFFER_USAGE_TRANSFER_DST |
        CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT | CRGPU_BUFFER_USAGE_INDIRECT;
    switch (kind) {
    case CRGPU_MEMORY_KIND_UPLOAD:
        return all & ~(CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_DST);
    case CRGPU_MEMORY_KIND_READBACK:
        return CRGPU_BUFFER_USAGE_TRANSFER_DST;
    default:
        return all;
    }
}

// The memoryTypeBits of a probe image of [format] with [usage], or all bits
// if the device can't make one (an image the device can't make is never
// placed either, so it narrows nothing). The image is 2D, 16 by 16, made as
// crgpu_image_create would make it apart from the format.
static CR_RESULT crgpuvulkan___internal_probe_image_bits(
    const crgpu_device_vulkan *device,
    VkFormat format,
    CRGPU_IMAGE_USAGE usage,
    VkFormatFeatureFlags needed_features,
    uint32_t *out_bits) {
    *out_bits = UINT32_MAX;

    VkFormatProperties formatProperties;
    vkGetPhysicalDeviceFormatProperties(device->_physical_device_vk, format, &formatProperties);
    if ((formatProperties.optimalTilingFeatures & needed_features) != needed_features)
        return CR_SUCCESS;

    crgpu_image_desc probe = {0};
    probe.usage = usage;
    probe.type = CRGPU_IMAGE_TYPE_2D;
    probe.format = CRGPU_FORMAT_R8G8B8A8_UNORM; // replaced below
    probe.width = 16;
    probe.height = 16;
    probe.depth = 1;
    probe.array_layers = 1;
    probe.mip_levels = 1;
    VkImageCreateInfo info;
    crgpuvulkan___image_create_info(&probe, &info);
    info.format = format;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device->_logical_device_vk, &info, NULL, &image) != VK_SUCCESS)
        return CR_RESULT_API_SPECIFIC_FAILURE;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device->_logical_device_vk, image, &requirements);
    vkDestroyImage(device->_logical_device_vk, image, NULL);
    *out_bits = requirements.memoryTypeBits;
    return CR_SUCCESS;
}

// The memory types every resource of [desc]'s class and kind can be bound
// to. See the top of this file.
static CR_RESULT crgpuvulkan___internal_heap_type_bits(
    const crgpu_device_vulkan *device,
    const crgpu_heap_desc *desc,
    uint32_t *out_bits) {
    if (desc->resource_class == CRGPU_HEAP_RESOURCE_CLASS_BUFFERS) {
        crgpu_buffer_desc probe = {0};
        probe.usage = crgpuvulkan___internal_kind_buffer_usages(desc->memory_kind);
        probe.memory_kind = desc->memory_kind;
        probe.size = CRGPU_HEAP_SIZE_GRANULARITY;
        VkBufferCreateInfo info;
        crgpuvulkan___buffer_create_info(device->_features, &probe, &info);

        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(device->_logical_device_vk, &info, NULL, &buffer) != VK_SUCCESS)
            return CR_RESULT_API_SPECIFIC_FAILURE;
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device->_logical_device_vk, buffer, &requirements);
        vkDestroyBuffer(device->_logical_device_vk, buffer, NULL);
        *out_bits = requirements.memoryTypeBits;
        return CR_SUCCESS;
    }
    // An acceleration structure's buffer, as crgpuvulkanraytracing.c makes
    // one, which every structure placed in the heap is.
    if (desc->resource_class == CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES) {
        uint32_t bits = 0;
        const CR_RESULT sr = crgpuvulkan___acceleration_structure_memory_bits(device, CRGPU_HEAP_SIZE_GRANULARITY, &bits, NULL);
        *out_bits = bits;
        return sr;
    }

    const bool attachments = desc->resource_class == CRGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES;

    // The color probe. R8G8B8A8_UNORM is one every device has to support
    // for both.
    uint32_t bits = UINT32_MAX;
    CR_RESULT sr = crgpuvulkan___internal_probe_image_bits(
        device, VK_FORMAT_R8G8B8A8_UNORM,
        attachments ? CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT : CRGPU_IMAGE_USAGE_TRANSFER_DST,
        (attachments ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0) | VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
        &bits);
    if (sr != CR_SUCCESS) return sr;

    // Every Vulkan depth/stencil format, not only those Carrottus GPU names: more
    // than one CRGPU_FORMAT converts to a depth/stencil VkFormat.
    static const VkFormat depthFormats[] = {
        VK_FORMAT_D16_UNORM,
        VK_FORMAT_X8_D24_UNORM_PACK32,
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_S8_UINT,
        VK_FORMAT_D16_UNORM_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
    };
    for (uint32_t i = 0; i < sizeof(depthFormats) / sizeof(depthFormats[0]); i++) {
        uint32_t formatBits = UINT32_MAX;
        sr = crgpuvulkan___internal_probe_image_bits(
            device, depthFormats[i],
            attachments ? CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT : CRGPU_IMAGE_USAGE_TRANSFER_DST,
            (attachments ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : 0) | VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
            &formatBits);
        if (sr != CR_SUCCESS) return sr;
        bits &= formatBits;
    }
    *out_bits = bits;
    return CR_SUCCESS;
}

// A resource's VkMemoryRequirements as Carrottus GPU reports them for [heap]. In
// a heap of a mappable kind the size and alignment are raised to multiples
// of nonCoherentAtomSize (a power of two, as the alignment is), so that a
// flush or invalidate of one placed buffer, widened to whole atoms, stays in
// that buffer's bytes (see crgpuvulkan___internal_buffer_memory_range in
// crgpuvulkanbuffer.c).
static void crgpuvulkan___internal_placement_requirements(
    const crgpu_heap_vulkan *heap,
    const VkMemoryRequirements *requirements,
    crgpu_placement_requirements *out_requirements) {
    uint64_t size = requirements->size;
    uint64_t alignment = requirements->alignment;
    if (heap->_desc.memory_kind != CRGPU_MEMORY_KIND_DEVICE) {
        const uint64_t atom = heap->_device._properties_vk.limits.nonCoherentAtomSize;
        if (alignment < atom) alignment = atom;
        if (size % atom) size += atom - size % atom;
    }
    out_requirements->size = size;
    out_requirements->alignment = alignment;
}

// The checks crgpu_heap_get_buffer_placement_requirements and
// crgpu_buffer_create_placed make after their argument checks.
static CR_RESULT crgpuvulkan___internal_check_buffer_placement(
    crgpu_heap heap,
    const crgpu_buffer_desc *desc) {
    const CR_RESULT sr = crgpuvulkan___check_buffer_desc(heap->_device._features, desc);
    if (sr != CR_SUCCESS) return sr;
    if (heap->_desc.resource_class != CRGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
        desc->memory_kind != heap->_desc.memory_kind)
        return CR_RESULT_GPU_HEAP_MISMATCH;
    return CR_SUCCESS;
}

// The checks crgpu_heap_get_image_placement_requirements and
// crgpu_image_create_placed make after their argument checks.
static CR_RESULT crgpuvulkan___internal_check_image_placement(
    crgpu_heap heap,
    const crgpu_image_desc *desc) {
    const CR_RESULT sr = crgpuvulkan___check_image_desc(desc);
    if (sr != CR_SUCCESS) return sr;
    if (desc->usage & (CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT | CRGPU_IMAGE_USAGE_PRESENTABLE))
        return CR_RESULT_GPU_INVALID_IMAGE_USAGE;
    const CRGPU_HEAP_RESOURCE_CLASS imageClass =
        (desc->usage & (CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT))
            ? CRGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES
            : CRGPU_HEAP_RESOURCE_CLASS_IMAGES;
    if (heap->_desc.resource_class != imageClass)
        return CR_RESULT_GPU_HEAP_MISMATCH;
    return CR_SUCCESS;
}

// The checks after the requirements are known, shared by both placed
// creates: the heap's memory type is one the resource can use (the probes
// make sure of it; a driver that broke the spec would land here), the offset
// is aligned and the resource fits.
static CR_RESULT crgpuvulkan___internal_check_placement(
    const crgpu_heap_vulkan *heap,
    uint64_t offset,
    const VkMemoryRequirements *requirements,
    const crgpu_placement_requirements *placement) {
    if (!(requirements->memoryTypeBits & (1u << heap->_memory_type_index_vk)))
        return CR_RESULT_API_SPECIFIC_FAILURE;
    if (offset % placement->alignment)
        return CR_RESULT_GPU_PLACEMENT_MISALIGNED;
    if (offset > heap->_desc.size || placement->size > heap->_desc.size - offset)
        return CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE;
    return CR_SUCCESS;
}

#if __cplusplus
extern "C" {
#endif

CR_RESULT crgpu_heap_create(
    crgpu_device device,
    const crgpu_heap_desc *desc,
    crgpu_heap *out_heap) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_heap_create", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!desc) return CR_RESULT_NULL_DESC;
    const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_HEAP_DESC);
    if (base_sr != CR_SUCCESS) return base_sr;
    if (!out_heap) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    if (desc->size == 0) return CR_RESULT_ZERO_SIZE;
    if (desc->size % CRGPU_HEAP_SIZE_GRANULARITY) return CR_RESULT_GPU_INVALID_HEAP_SIZE;
    if (desc->resource_class < CRGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
        desc->resource_class > CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES)
        return CR_RESULT_GPU_INVALID_HEAP_RESOURCE_CLASS;
    if (desc->memory_kind < CRGPU_MEMORY_KIND_DEVICE || desc->memory_kind > CRGPU_MEMORY_KIND_DEVICE_MAPPABLE)
        return CR_RESULT_GPU_INVALID_MEMORY_KIND;
    if (desc->resource_class != CRGPU_HEAP_RESOURCE_CLASS_BUFFERS && desc->memory_kind != CRGPU_MEMORY_KIND_DEVICE)
        return CR_RESULT_GPU_INVALID_MEMORY_KIND;
    if (desc->memory_kind != CRGPU_MEMORY_KIND_DEVICE && !(device->_features & CRGPU_FEATURE_MAPPABLE_HEAPS))
        return CR_RESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
    if (desc->memory_kind == CRGPU_MEMORY_KIND_DEVICE_MAPPABLE && !(device->_features & CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY))
        return CR_RESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
    if (desc->resource_class == CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES && !(device->_features & CRGPU_FEATURE_RAY_QUERY))
        return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;

    VkDevice vk_device = device->_logical_device_vk;

    uint32_t typeBits = 0;
    CR_RESULT sr = crgpuvulkan___internal_heap_type_bits(device, desc, &typeBits);
    if (sr != CR_SUCCESS) return sr;
    VkMemoryPropertyFlags memoryProperties = 0;
    const uint32_t memoryTypeIndex = crgpuvulkan___memory_type_for_kind(
        device->_physical_device_vk, typeBits, desc->memory_kind, &memoryProperties);
    if (memoryTypeIndex == UINT32_MAX) return CR_RESULT_API_SPECIFIC_FAILURE;

    // Every buffer is created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    // (see crgpuvulkan___buffer_create_info), which needs its memory
    // allocated with this flag.
    VkMemoryAllocateFlagsInfo allocFlagsInfo = {0};
    allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = (desc->resource_class == CRGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
                       desc->resource_class == CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES)
                          ? &allocFlagsInfo
                          : NULL;
    allocInfo.allocationSize = desc->size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(vk_device, &allocInfo, NULL, &memory) != VK_SUCCESS)
        return CR_RESULT_API_SPECIFIC_FAILURE;

    void *mapped = NULL;
    if (desc->memory_kind != CRGPU_MEMORY_KIND_DEVICE &&
        vkMapMemory(vk_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(vk_device, memory, NULL);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

    crgpu_heap_vulkan *heap = CR_NEW_ZEROED(&device->head.allocator, crgpu_heap_vulkan);
    if (!heap) {
        // Freeing mapped memory unmaps it.
        vkFreeMemory(vk_device, memory, NULL);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    heap->head.allocator = device->head.allocator;
    CR_BIRTH_IN(heap, &device->_shared->quarantine);
    heap->_memory_vk = memory;
    heap->_memory_type_index_vk = memoryTypeIndex;
    heap->_memory_properties_vk = memoryProperties;
    heap->_mapped = mapped;
    heap->_desc = *desc;
    heap->_device = *device;

    CRGPU___LIVE_ADD(&device->_shared->live, CRGPU___OBJECT_HEAP);
    if (cr_debug_name_set(heap, desc->debug_name) != CR_SUCCESS) {
        crgpu_heap_destroy(heap);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    heap->_desc.debug_name = heap->head.debug_name;

    *out_heap = heap;
    return CR_SUCCESS;
}

void crgpu_heap_destroy(crgpu_heap heap) {
    if (!heap) return;
    if (CR_STALE(heap, "crgpu_heap_destroy", "heap")) return;
#if _DEBUG
    // Every buffer and image placed in the heap is destroyed first (see
    // crgpu.h).
    assert(heap->_placed_count == 0);
#endif
    if (heap->_mapped)
        vkUnmapMemory(heap->_device._logical_device_vk, heap->_memory_vk);
    vkFreeMemory(heap->_device._logical_device_vk, heap->_memory_vk, NULL);
    CRGPU___LIVE_REMOVE(&heap->_device._shared->live, CRGPU___OBJECT_HEAP);
    CR_KILL(heap);
    const CR_ALLOCATOR allocator = heap->head.allocator;
    cr_string_free(&allocator, heap->head.debug_name);
    CR_RETIRE(CR_QUARANTINE_OF(heap), &allocator, crgpu_heap_vulkan, heap);
}

CR_RESULT crgpu_heap_get_desc(
    crgpu_heap heap,
    crgpu_heap_desc *out_desc) {
    if (!heap) return CR_RESULT_GPU_INVALID_HEAP;
    if (CR_STALE(heap, "crgpu_heap_get_desc", "heap")) return CR_RESULT_GPU_INVALID_HEAP;
    if (!out_desc) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    *out_desc = heap->_desc;
    out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_HEAP_DESC);
    return CR_SUCCESS;
}

CR_RESULT crgpu_heap_get_buffer_placement_requirements(
    crgpu_heap heap,
    const crgpu_buffer_desc *desc,
    crgpu_placement_requirements *out_requirements) {
    if (!heap) return CR_RESULT_GPU_INVALID_HEAP;
    if (CR_STALE(heap, "crgpu_heap_get_buffer_placement_requirements", "heap")) return CR_RESULT_GPU_INVALID_HEAP;
    if (!desc) return CR_RESULT_NULL_DESC;
    const CR_RESULT desc_base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_BUFFER_DESC);
    if (desc_base_sr != CR_SUCCESS) return desc_base_sr;
    if (!out_requirements) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    const CR_RESULT out_requirements_base_sr = cr_struct_out_check(&out_requirements->base, CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS);
    if (out_requirements_base_sr != CR_SUCCESS) return out_requirements_base_sr;
    const CR_RESULT sr = crgpuvulkan___internal_check_buffer_placement(heap, desc);
    if (sr != CR_SUCCESS) return sr;

    // The buffer crgpu_buffer_create_placed would create, made only to be
    // asked.
    VkDevice vk_device = heap->_device._logical_device_vk;
    VkBufferCreateInfo info;
    crgpuvulkan___buffer_create_info(heap->_device._features, desc, &info);
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk_device, &info, NULL, &buffer) != VK_SUCCESS)
        return CR_RESULT_API_SPECIFIC_FAILURE;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk_device, buffer, &requirements);
    vkDestroyBuffer(vk_device, buffer, NULL);
    if (!(requirements.memoryTypeBits & (1u << heap->_memory_type_index_vk)))
        return CR_RESULT_API_SPECIFIC_FAILURE;

    crgpuvulkan___internal_placement_requirements(heap, &requirements, out_requirements);
    return CR_SUCCESS;
}

CR_RESULT crgpu_heap_get_image_placement_requirements(
    crgpu_heap heap,
    const crgpu_image_desc *desc,
    crgpu_placement_requirements *out_requirements) {
    if (!heap) return CR_RESULT_GPU_INVALID_HEAP;
    if (CR_STALE(heap, "crgpu_heap_get_image_placement_requirements", "heap")) return CR_RESULT_GPU_INVALID_HEAP;
    if (!desc) return CR_RESULT_NULL_DESC;
    const CR_RESULT desc_base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_IMAGE_DESC);
    if (desc_base_sr != CR_SUCCESS) return desc_base_sr;
    if (!out_requirements) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    const CR_RESULT out_requirements_base_sr = cr_struct_out_check(&out_requirements->base, CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS);
    if (out_requirements_base_sr != CR_SUCCESS) return out_requirements_base_sr;
    const CR_RESULT sr = crgpuvulkan___internal_check_image_placement(heap, desc);
    if (sr != CR_SUCCESS) return sr;

    VkDevice vk_device = heap->_device._logical_device_vk;
    VkImageCreateInfo info;
    crgpuvulkan___image_create_info(desc, &info);
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(vk_device, &info, NULL, &image) != VK_SUCCESS)
        return CR_RESULT_API_SPECIFIC_FAILURE;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk_device, image, &requirements);
    vkDestroyImage(vk_device, image, NULL);
    if (!(requirements.memoryTypeBits & (1u << heap->_memory_type_index_vk)))
        return CR_RESULT_API_SPECIFIC_FAILURE;

    crgpuvulkan___internal_placement_requirements(heap, &requirements, out_requirements);
    return CR_SUCCESS;
}

CR_RESULT crgpu_buffer_create_placed(
    crgpu_heap heap,
    uint64_t offset,
    const crgpu_buffer_desc *desc,
    crgpu_buffer *out_buffer) {
    if (!heap) return CR_RESULT_GPU_INVALID_HEAP;
    if (CR_STALE(heap, "crgpu_buffer_create_placed", "heap")) return CR_RESULT_GPU_INVALID_HEAP;
    if (!desc) return CR_RESULT_NULL_DESC;
    const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_BUFFER_DESC);
    if (base_sr != CR_SUCCESS) return base_sr;
    if (!out_buffer) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    CR_RESULT sr = crgpuvulkan___internal_check_buffer_placement(heap, desc);
    if (sr != CR_SUCCESS) return sr;

    VkDevice vk_device = heap->_device._logical_device_vk;
    VkBufferCreateInfo info;
    crgpuvulkan___buffer_create_info(heap->_device._features, desc, &info);
    VkBuffer vk_buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk_device, &info, NULL, &vk_buffer) != VK_SUCCESS)
        return CR_RESULT_API_SPECIFIC_FAILURE;

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk_device, vk_buffer, &requirements);
    crgpu_placement_requirements placement;
    crgpuvulkan___internal_placement_requirements(heap, &requirements, &placement);
    sr = crgpuvulkan___internal_check_placement(heap, offset, &requirements, &placement);
    if (sr != CR_SUCCESS) {
        vkDestroyBuffer(vk_device, vk_buffer, NULL);
        return sr;
    }
    if (vkBindBufferMemory(vk_device, vk_buffer, heap->_memory_vk, offset) != VK_SUCCESS) {
        vkDestroyBuffer(vk_device, vk_buffer, NULL);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

    crgpu_buffer_vulkan *buffer = CR_NEW_ZEROED(&heap->head.allocator, crgpu_buffer_vulkan);
    if (!buffer) {
        vkDestroyBuffer(vk_device, vk_buffer, NULL);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    buffer->head.allocator = heap->head.allocator;
    CR_BIRTH_IN(buffer, &heap->_device._shared->quarantine);
    buffer->_buffer_vk = vk_buffer;
    buffer->_memory_vk = heap->_memory_vk;
    buffer->_memory_offset_vk = offset;
    buffer->_memory_size_vk = placement.size;
    buffer->_memory_properties_vk = heap->_memory_properties_vk;
    buffer->_desc = *desc;
    buffer->_device = heap->_device;
    buffer->_heap = heap;

    VkBufferDeviceAddressInfo addressInfo = {0};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = vk_buffer;
    buffer->_gpu_address_vk = vkGetBufferDeviceAddress(vk_device, &addressInfo);
    CRGPU___LIVE_ADD(&heap->_device._shared->live, CRGPU___OBJECT_BUFFER);

#if _DEBUG
    // Counted before the name is copied: a failed copy destroys the buffer,
    // which takes it off the count again.
    heap->_placed_count++;
#endif
    if (cr_debug_name_set(buffer, desc->debug_name) != CR_SUCCESS) {
        crgpu_buffer_destroy(buffer);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    buffer->_desc.debug_name = buffer->head.debug_name;

    *out_buffer = buffer;
    return CR_SUCCESS;
}

CR_RESULT crgpu_image_create_placed(
    crgpu_heap heap,
    uint64_t offset,
    const crgpu_image_desc *desc,
    crgpu_image *out_image) {
    if (!heap) return CR_RESULT_GPU_INVALID_HEAP;
    if (CR_STALE(heap, "crgpu_image_create_placed", "heap")) return CR_RESULT_GPU_INVALID_HEAP;
    if (!desc) return CR_RESULT_NULL_DESC;
    const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_IMAGE_DESC);
    if (base_sr != CR_SUCCESS) return base_sr;
    if (!out_image) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    CR_RESULT sr = crgpuvulkan___internal_check_image_placement(heap, desc);
    if (sr != CR_SUCCESS) return sr;

    VkDevice vk_device = heap->_device._logical_device_vk;
    VkImageCreateInfo info;
    crgpuvulkan___image_create_info(desc, &info);
    VkImage vk_image = VK_NULL_HANDLE;
    if (vkCreateImage(vk_device, &info, NULL, &vk_image) != VK_SUCCESS)
        return CR_RESULT_API_SPECIFIC_FAILURE;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk_device, vk_image, &requirements);
    crgpu_placement_requirements placement;
    crgpuvulkan___internal_placement_requirements(heap, &requirements, &placement);
    sr = crgpuvulkan___internal_check_placement(heap, offset, &requirements, &placement);
    if (sr != CR_SUCCESS) {
        vkDestroyImage(vk_device, vk_image, NULL);
        return sr;
    }
    if (vkBindImageMemory(vk_device, vk_image, heap->_memory_vk, offset) != VK_SUCCESS) {
        vkDestroyImage(vk_device, vk_image, NULL);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

    crgpu_image_vulkan *image = CR_NEW_ZEROED(&heap->head.allocator, crgpu_image_vulkan);
    if (!image) {
        vkDestroyImage(vk_device, vk_image, NULL);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    image->head.allocator = heap->head.allocator;
    CR_BIRTH_IN(image, &heap->_device._shared->quarantine);
    image->_image_vk = vk_image;
    image->_memory_vk = heap->_memory_vk;
    image->_format_vk = info.format;
    image->_desc = *desc;
    image->_device = heap->_device;
    image->_heap = heap;
    CRGPU___LIVE_ADD(&heap->_device._shared->live, CRGPU___OBJECT_IMAGE);

#if _DEBUG
    heap->_placed_count++;
#endif
    if (cr_debug_name_set(image, desc->debug_name) != CR_SUCCESS) {
        crgpu_image_destroy(image);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    image->_desc.debug_name = image->head.debug_name;

    *out_image = image;
    return CR_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // CRGPU_COMPILE_VULKAN_API
