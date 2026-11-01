
#if SPUDGPU_COMPILE_VULKAN_API

#include <vulkan/vulkan.h>
#include "spudgpu.h"
#include "spudgpuvulkan.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../../../spudstructbase.h"

// SpudGPU heaps on Vulkan.
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
//   SpudGPU sets, except that a depth/stencil format may have bits of its
//   own. So one color image, and one image of each depth/stencil format an
//   image of the class can be made in. Every image spudgpu_create_image
//   makes has VK_IMAGE_USAGE_TRANSFER_DST_BIT (see
//   spudgpuvulkan___image_create_info), so an IMAGES heap probes with that
//   usage alone, and a format that can't take it can't be an image at all;
//   an ATTACHMENT_IMAGES heap probes with the attachment usage, which every
//   image in it has.
//
// None of the probes is kept. A heap of a mappable kind is mapped, whole,
// from creation to destruction: see spudgpu_heap_vulkan::_mapped.

// Every SPUDGPU_BUFFER_USAGE a buffer of [kind] may have (see
// SPUDGPU_MEMORY_KIND in spudgpu.h, and spudgpuvulkan___check_buffer_desc).
static SPUDGPU_BUFFER_USAGE spudgpuvulkan___internal_kind_buffer_usages(SPUDGPU_MEMORY_KIND kind) {
    const SPUDGPU_BUFFER_USAGE all =
        SPUDGPU_BUFFER_USAGE_VERTEX | SPUDGPU_BUFFER_USAGE_INDEX | SPUDGPU_BUFFER_USAGE_UNIFORM |
        SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_TRANSFER_SRC | SPUDGPU_BUFFER_USAGE_TRANSFER_DST |
        SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT | SPUDGPU_BUFFER_USAGE_INDIRECT;
    switch (kind) {
    case SPUDGPU_MEMORY_KIND_UPLOAD:
        return all & ~(SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_TRANSFER_DST);
    case SPUDGPU_MEMORY_KIND_READBACK:
        return SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
    default:
        return all;
    }
}

// The memoryTypeBits of a probe image of [format] with [usage], or all bits
// if the device can't make one (an image the device can't make is never
// placed either, so it narrows nothing). The image is 2D, 16 by 16, made as
// spudgpu_create_image would make it apart from the format.
static SPUDRESULT spudgpuvulkan___internal_probe_image_bits(
    const spudgpu_device_vulkan *device,
    VkFormat format,
    SPUDGPU_IMAGE_USAGE usage,
    VkFormatFeatureFlags needed_features,
    uint32_t *out_bits) {
    *out_bits = UINT32_MAX;

    VkFormatProperties formatProperties;
    vkGetPhysicalDeviceFormatProperties(device->_physical_device_vk, format, &formatProperties);
    if ((formatProperties.optimalTilingFeatures & needed_features) != needed_features)
        return SPUD_SUCCESS;

    spudgpu_image_desc probe = {0};
    probe.usage = usage;
    probe.type = SPUDGPU_IMAGE_TYPE_2D;
    probe.format = SPUDGPU_FORMAT_R8G8B8A8_UNORM; // replaced below
    probe.width = 16;
    probe.height = 16;
    probe.depth = 1;
    probe.array_layers = 1;
    probe.mip_levels = 1;
    VkImageCreateInfo info;
    spudgpuvulkan___image_create_info(&probe, &info);
    info.format = format;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device->_logical_device_vk, &info, NULL, &image) != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device->_logical_device_vk, image, &requirements);
    vkDestroyImage(device->_logical_device_vk, image, NULL);
    *out_bits = requirements.memoryTypeBits;
    return SPUD_SUCCESS;
}

// The memory types every resource of [desc]'s class and kind can be bound
// to. See the top of this file.
static SPUDRESULT spudgpuvulkan___internal_heap_type_bits(
    const spudgpu_device_vulkan *device,
    const spudgpu_heap_desc *desc,
    uint32_t *out_bits) {
    if (desc->resource_class == SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS) {
        spudgpu_buffer_desc probe = {0};
        probe.usage = spudgpuvulkan___internal_kind_buffer_usages(desc->memory_kind);
        probe.memory_kind = desc->memory_kind;
        probe.size = SPUDGPU_HEAP_SIZE_GRANULARITY;
        VkBufferCreateInfo info;
        spudgpuvulkan___buffer_create_info(device->_features, &probe, &info);

        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(device->_logical_device_vk, &info, NULL, &buffer) != VK_SUCCESS)
            return SPUDRESULT_API_SPECIFIC_FAILURE;
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device->_logical_device_vk, buffer, &requirements);
        vkDestroyBuffer(device->_logical_device_vk, buffer, NULL);
        *out_bits = requirements.memoryTypeBits;
        return SPUD_SUCCESS;
    }
    // An acceleration structure's buffer, as spudgpuvulkanraytracing.c makes
    // one, which every structure placed in the heap is.
    if (desc->resource_class == SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES) {
        uint32_t bits = 0;
        const SPUDRESULT sr = spudgpuvulkan___acceleration_structure_memory_bits(device, SPUDGPU_HEAP_SIZE_GRANULARITY, &bits, NULL);
        *out_bits = bits;
        return sr;
    }

    const bool attachments = desc->resource_class == SPUDGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES;

    // The color probe. R8G8B8A8_UNORM is one every device has to support
    // for both.
    uint32_t bits = UINT32_MAX;
    SPUDRESULT sr = spudgpuvulkan___internal_probe_image_bits(
        device, VK_FORMAT_R8G8B8A8_UNORM,
        attachments ? SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT : SPUDGPU_IMAGE_USAGE_TRANSFER_DST,
        (attachments ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0) | VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
        &bits);
    if (sr != SPUD_SUCCESS) return sr;

    // Every Vulkan depth/stencil format, not only those SpudGPU names: more
    // than one SPUDGPU_FORMAT converts to a depth/stencil VkFormat.
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
        sr = spudgpuvulkan___internal_probe_image_bits(
            device, depthFormats[i],
            attachments ? SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT : SPUDGPU_IMAGE_USAGE_TRANSFER_DST,
            (attachments ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : 0) | VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
            &formatBits);
        if (sr != SPUD_SUCCESS) return sr;
        bits &= formatBits;
    }
    *out_bits = bits;
    return SPUD_SUCCESS;
}

// A resource's VkMemoryRequirements as SpudGPU reports them for [heap]. In
// a heap of a mappable kind the size and alignment are raised to multiples
// of nonCoherentAtomSize (a power of two, as the alignment is), so that a
// flush or invalidate of one placed buffer, widened to whole atoms, stays in
// that buffer's bytes (see spudgpuvulkan___internal_buffer_memory_range in
// spudgpuvulkanbuffer.c).
static void spudgpuvulkan___internal_placement_requirements(
    const spudgpu_heap_vulkan *heap,
    const VkMemoryRequirements *requirements,
    spudgpu_placement_requirements *out_requirements) {
    uint64_t size = requirements->size;
    uint64_t alignment = requirements->alignment;
    if (heap->_desc.memory_kind != SPUDGPU_MEMORY_KIND_DEVICE) {
        const uint64_t atom = heap->_device._properties_vk.limits.nonCoherentAtomSize;
        if (alignment < atom) alignment = atom;
        if (size % atom) size += atom - size % atom;
    }
    out_requirements->size = size;
    out_requirements->alignment = alignment;
}

// The checks spudgpu_get_buffer_placement_requirements and
// spudgpu_create_placed_buffer make after their argument checks.
static SPUDRESULT spudgpuvulkan___internal_check_buffer_placement(
    spudgpu_heap heap,
    const spudgpu_buffer_desc *desc) {
    const SPUDRESULT sr = spudgpuvulkan___check_buffer_desc(heap->_device._features, desc);
    if (sr != SPUD_SUCCESS) return sr;
    if (heap->_desc.resource_class != SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
        desc->memory_kind != heap->_desc.memory_kind)
        return SPUDRESULT_GPU_HEAP_MISMATCH;
    return SPUD_SUCCESS;
}

// The checks spudgpu_get_image_placement_requirements and
// spudgpu_create_placed_image make after their argument checks.
static SPUDRESULT spudgpuvulkan___internal_check_image_placement(
    spudgpu_heap heap,
    const spudgpu_image_desc *desc) {
    const SPUDRESULT sr = spudgpuvulkan___check_image_desc(desc);
    if (sr != SPUD_SUCCESS) return sr;
    if (desc->usage & (SPUDGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT | SPUDGPU_IMAGE_USAGE_PRESENTABLE))
        return SPUDRESULT_GPU_INVALID_IMAGE_USAGE;
    const SPUDGPU_HEAP_RESOURCE_CLASS imageClass =
        (desc->usage & (SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT | SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT))
            ? SPUDGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES
            : SPUDGPU_HEAP_RESOURCE_CLASS_IMAGES;
    if (heap->_desc.resource_class != imageClass)
        return SPUDRESULT_GPU_HEAP_MISMATCH;
    return SPUD_SUCCESS;
}

// The checks after the requirements are known, shared by both placed
// creates: the heap's memory type is one the resource can use (the probes
// make sure of it; a driver that broke the spec would land here), the offset
// is aligned and the resource fits.
static SPUDRESULT spudgpuvulkan___internal_check_placement(
    const spudgpu_heap_vulkan *heap,
    uint64_t offset,
    const VkMemoryRequirements *requirements,
    const spudgpu_placement_requirements *placement) {
    if (!(requirements->memoryTypeBits & (1u << heap->_memory_type_index_vk)))
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    if (offset % placement->alignment)
        return SPUDRESULT_GPU_PLACEMENT_MISALIGNED;
    if (offset > heap->_desc.size || placement->size > heap->_desc.size - offset)
        return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
    return SPUD_SUCCESS;
}

#if __cplusplus
extern "C" {
#endif

SPUDRESULT spudgpu_create_heap(
    spudgpu_device device,
    const spudgpu_heap_desc *desc,
    spudgpu_heap *out_heap) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_create_heap", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!desc) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_HEAP_DESC);
    if (base_sr != SPUD_SUCCESS) return base_sr;
    if (!out_heap) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    if (desc->size == 0) return SPUDRESULT_ZERO_SIZE;
    if (desc->size % SPUDGPU_HEAP_SIZE_GRANULARITY) return SPUDRESULT_GPU_INVALID_HEAP_SIZE;
    if (desc->resource_class < SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
        desc->resource_class > SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES)
        return SPUDRESULT_GPU_INVALID_HEAP_RESOURCE_CLASS;
    if (desc->memory_kind < SPUDGPU_MEMORY_KIND_DEVICE || desc->memory_kind > SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE)
        return SPUDRESULT_GPU_INVALID_MEMORY_KIND;
    if (desc->resource_class != SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS && desc->memory_kind != SPUDGPU_MEMORY_KIND_DEVICE)
        return SPUDRESULT_GPU_INVALID_MEMORY_KIND;
    if (desc->memory_kind != SPUDGPU_MEMORY_KIND_DEVICE && !(device->_features & SPUDGPU_FEATURE_MAPPABLE_HEAPS))
        return SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
    if (desc->memory_kind == SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE && !(device->_features & SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY))
        return SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
    if (desc->resource_class == SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES && !(device->_features & SPUDGPU_FEATURE_RAY_QUERY))
        return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;

    VkDevice vk_device = device->_logical_device_vk;

    uint32_t typeBits = 0;
    SPUDRESULT sr = spudgpuvulkan___internal_heap_type_bits(device, desc, &typeBits);
    if (sr != SPUD_SUCCESS) return sr;
    VkMemoryPropertyFlags memoryProperties = 0;
    const uint32_t memoryTypeIndex = spudgpuvulkan___memory_type_for_kind(
        device->_physical_device_vk, typeBits, desc->memory_kind, &memoryProperties);
    if (memoryTypeIndex == UINT32_MAX) return SPUDRESULT_API_SPECIFIC_FAILURE;

    // Every buffer is created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    // (see spudgpuvulkan___buffer_create_info), which needs its memory
    // allocated with this flag.
    VkMemoryAllocateFlagsInfo allocFlagsInfo = {0};
    allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = (desc->resource_class == SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
                       desc->resource_class == SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES)
                          ? &allocFlagsInfo
                          : NULL;
    allocInfo.allocationSize = desc->size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(vk_device, &allocInfo, NULL, &memory) != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;

    void *mapped = NULL;
    if (desc->memory_kind != SPUDGPU_MEMORY_KIND_DEVICE &&
        vkMapMemory(vk_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(vk_device, memory, NULL);
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }

    spudgpu_heap_vulkan *heap = SPUD_NEW_ZEROED(&device->head.allocator, spudgpu_heap_vulkan);
    if (!heap) {
        // Freeing mapped memory unmaps it.
        vkFreeMemory(vk_device, memory, NULL);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    heap->head.allocator = device->head.allocator;
    SPUD_BIRTH(heap);
    heap->_memory_vk = memory;
    heap->_memory_type_index_vk = memoryTypeIndex;
    heap->_memory_properties_vk = memoryProperties;
    heap->_mapped = mapped;
    heap->_desc = *desc;
    heap->_device = *device;

    SPUDGPU___LIVE_ADD(&device->_shared->live, SPUDGPU___OBJECT_HEAP);
    if (spud_debug_name_set(heap, desc->debug_name) != SPUD_SUCCESS) {
        spudgpu_destroy_heap(heap);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    heap->_desc.debug_name = heap->head.debug_name;

    *out_heap = heap;
    return SPUD_SUCCESS;
}

void spudgpu_destroy_heap(spudgpu_heap heap) {
    if (!heap) return;
    if (SPUD_STALE(heap, "spudgpu_destroy_heap", "heap")) return;
#if _DEBUG
    // Every buffer and image placed in the heap is destroyed first (see
    // spudgpu.h).
    assert(heap->_placed_count == 0);
#endif
    if (heap->_mapped)
        vkUnmapMemory(heap->_device._logical_device_vk, heap->_memory_vk);
    vkFreeMemory(heap->_device._logical_device_vk, heap->_memory_vk, NULL);
    SPUDGPU___LIVE_REMOVE(&heap->_device._shared->live, SPUDGPU___OBJECT_HEAP);
    SPUD_KILL(heap);
    const SPUD_ALLOCATOR allocator = heap->head.allocator;
    spud_string_free(&allocator, heap->head.debug_name);
    SPUD_DELETE(&allocator, spudgpu_heap_vulkan, heap);
}

SPUDRESULT spudgpu_get_heap_desc(
    spudgpu_heap heap,
    spudgpu_heap_desc *out_desc) {
    if (!heap) return SPUDRESULT_GPU_INVALID_HEAP;
    if (SPUD_STALE(heap, "spudgpu_get_heap_desc", "heap")) return SPUDRESULT_GPU_INVALID_HEAP;
    if (!out_desc) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    *out_desc = heap->_desc;
    out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_HEAP_DESC);
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_buffer_placement_requirements(
    spudgpu_heap heap,
    const spudgpu_buffer_desc *desc,
    spudgpu_placement_requirements *out_requirements) {
    if (!heap) return SPUDRESULT_GPU_INVALID_HEAP;
    if (SPUD_STALE(heap, "spudgpu_get_buffer_placement_requirements", "heap")) return SPUDRESULT_GPU_INVALID_HEAP;
    if (!desc) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT desc_base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_BUFFER_DESC);
    if (desc_base_sr != SPUD_SUCCESS) return desc_base_sr;
    if (!out_requirements) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    const SPUDRESULT out_requirements_base_sr = spud_struct_out_check(&out_requirements->base, SPUD_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS);
    if (out_requirements_base_sr != SPUD_SUCCESS) return out_requirements_base_sr;
    const SPUDRESULT sr = spudgpuvulkan___internal_check_buffer_placement(heap, desc);
    if (sr != SPUD_SUCCESS) return sr;

    // The buffer spudgpu_create_placed_buffer would create, made only to be
    // asked.
    VkDevice vk_device = heap->_device._logical_device_vk;
    VkBufferCreateInfo info;
    spudgpuvulkan___buffer_create_info(heap->_device._features, desc, &info);
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk_device, &info, NULL, &buffer) != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk_device, buffer, &requirements);
    vkDestroyBuffer(vk_device, buffer, NULL);
    if (!(requirements.memoryTypeBits & (1u << heap->_memory_type_index_vk)))
        return SPUDRESULT_API_SPECIFIC_FAILURE;

    spudgpuvulkan___internal_placement_requirements(heap, &requirements, out_requirements);
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_image_placement_requirements(
    spudgpu_heap heap,
    const spudgpu_image_desc *desc,
    spudgpu_placement_requirements *out_requirements) {
    if (!heap) return SPUDRESULT_GPU_INVALID_HEAP;
    if (SPUD_STALE(heap, "spudgpu_get_image_placement_requirements", "heap")) return SPUDRESULT_GPU_INVALID_HEAP;
    if (!desc) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT desc_base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_IMAGE_DESC);
    if (desc_base_sr != SPUD_SUCCESS) return desc_base_sr;
    if (!out_requirements) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    const SPUDRESULT out_requirements_base_sr = spud_struct_out_check(&out_requirements->base, SPUD_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS);
    if (out_requirements_base_sr != SPUD_SUCCESS) return out_requirements_base_sr;
    const SPUDRESULT sr = spudgpuvulkan___internal_check_image_placement(heap, desc);
    if (sr != SPUD_SUCCESS) return sr;

    VkDevice vk_device = heap->_device._logical_device_vk;
    VkImageCreateInfo info;
    spudgpuvulkan___image_create_info(desc, &info);
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(vk_device, &info, NULL, &image) != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk_device, image, &requirements);
    vkDestroyImage(vk_device, image, NULL);
    if (!(requirements.memoryTypeBits & (1u << heap->_memory_type_index_vk)))
        return SPUDRESULT_API_SPECIFIC_FAILURE;

    spudgpuvulkan___internal_placement_requirements(heap, &requirements, out_requirements);
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_create_placed_buffer(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_buffer_desc *desc,
    spudgpu_buffer *out_buffer) {
    if (!heap) return SPUDRESULT_GPU_INVALID_HEAP;
    if (SPUD_STALE(heap, "spudgpu_create_placed_buffer", "heap")) return SPUDRESULT_GPU_INVALID_HEAP;
    if (!desc) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_BUFFER_DESC);
    if (base_sr != SPUD_SUCCESS) return base_sr;
    if (!out_buffer) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    SPUDRESULT sr = spudgpuvulkan___internal_check_buffer_placement(heap, desc);
    if (sr != SPUD_SUCCESS) return sr;

    VkDevice vk_device = heap->_device._logical_device_vk;
    VkBufferCreateInfo info;
    spudgpuvulkan___buffer_create_info(heap->_device._features, desc, &info);
    VkBuffer vk_buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(vk_device, &info, NULL, &vk_buffer) != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk_device, vk_buffer, &requirements);
    spudgpu_placement_requirements placement;
    spudgpuvulkan___internal_placement_requirements(heap, &requirements, &placement);
    sr = spudgpuvulkan___internal_check_placement(heap, offset, &requirements, &placement);
    if (sr != SPUD_SUCCESS) {
        vkDestroyBuffer(vk_device, vk_buffer, NULL);
        return sr;
    }
    if (vkBindBufferMemory(vk_device, vk_buffer, heap->_memory_vk, offset) != VK_SUCCESS) {
        vkDestroyBuffer(vk_device, vk_buffer, NULL);
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }

    spudgpu_buffer_vulkan *buffer = SPUD_NEW_ZEROED(&heap->head.allocator, spudgpu_buffer_vulkan);
    if (!buffer) {
        vkDestroyBuffer(vk_device, vk_buffer, NULL);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    buffer->head.allocator = heap->head.allocator;
    SPUD_BIRTH(buffer);
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
    SPUDGPU___LIVE_ADD(&heap->_device._shared->live, SPUDGPU___OBJECT_BUFFER);

#if _DEBUG
    // Counted before the name is copied: a failed copy destroys the buffer,
    // which takes it off the count again.
    heap->_placed_count++;
#endif
    if (spud_debug_name_set(buffer, desc->debug_name) != SPUD_SUCCESS) {
        spudgpu_destroy_buffer(buffer);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    buffer->_desc.debug_name = buffer->head.debug_name;

    *out_buffer = buffer;
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_create_placed_image(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_image_desc *desc,
    spudgpu_image *out_image) {
    if (!heap) return SPUDRESULT_GPU_INVALID_HEAP;
    if (SPUD_STALE(heap, "spudgpu_create_placed_image", "heap")) return SPUDRESULT_GPU_INVALID_HEAP;
    if (!desc) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_IMAGE_DESC);
    if (base_sr != SPUD_SUCCESS) return base_sr;
    if (!out_image) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    SPUDRESULT sr = spudgpuvulkan___internal_check_image_placement(heap, desc);
    if (sr != SPUD_SUCCESS) return sr;

    VkDevice vk_device = heap->_device._logical_device_vk;
    VkImageCreateInfo info;
    spudgpuvulkan___image_create_info(desc, &info);
    VkImage vk_image = VK_NULL_HANDLE;
    if (vkCreateImage(vk_device, &info, NULL, &vk_image) != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk_device, vk_image, &requirements);
    spudgpu_placement_requirements placement;
    spudgpuvulkan___internal_placement_requirements(heap, &requirements, &placement);
    sr = spudgpuvulkan___internal_check_placement(heap, offset, &requirements, &placement);
    if (sr != SPUD_SUCCESS) {
        vkDestroyImage(vk_device, vk_image, NULL);
        return sr;
    }
    if (vkBindImageMemory(vk_device, vk_image, heap->_memory_vk, offset) != VK_SUCCESS) {
        vkDestroyImage(vk_device, vk_image, NULL);
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }

    spudgpu_image_vulkan *image = SPUD_NEW_ZEROED(&heap->head.allocator, spudgpu_image_vulkan);
    if (!image) {
        vkDestroyImage(vk_device, vk_image, NULL);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    image->head.allocator = heap->head.allocator;
    SPUD_BIRTH(image);
    image->_image_vk = vk_image;
    image->_memory_vk = heap->_memory_vk;
    image->_format_vk = info.format;
    image->_desc = *desc;
    image->_device = heap->_device;
    image->_heap = heap;
    SPUDGPU___LIVE_ADD(&heap->_device._shared->live, SPUDGPU___OBJECT_IMAGE);

#if _DEBUG
    heap->_placed_count++;
#endif
    if (spud_debug_name_set(image, desc->debug_name) != SPUD_SUCCESS) {
        spudgpu_destroy_image(image);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    image->_desc.debug_name = image->head.debug_name;

    *out_image = image;
    return SPUD_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // SPUDGPU_COMPILE_VULKAN_API
