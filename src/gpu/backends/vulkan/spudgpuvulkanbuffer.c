
#if SPUDGPU_COMPILE_VULKAN_API

#include <vulkan/vulkan.h>
#include "spudgpu.h"
#include "spudgpuvulkan.h"
#include <stdlib.h>
#include <string.h>
#include "../../../spudstructbase.h"

uint32_t spudgpuvulkan___find_memory_type_internal(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return (uint32_t) -1;
    //throw std::runtime_error("SpudGPU Vulkan: failed to find suitable memory type!");
}

void spudgpuvulkan___buffer_usage_flags_internal(
    uint32_t __spud_gpu_buffer_usage,
    VkBufferUsageFlags *output) {
    VkBufferUsageFlags *const outputValue = output;
    if (__spud_gpu_buffer_usage & SPUDGPU_BUFFER_USAGE_VERTEX)
        *outputValue |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (__spud_gpu_buffer_usage & SPUDGPU_BUFFER_USAGE_INDEX)
        *outputValue |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (__spud_gpu_buffer_usage & SPUDGPU_BUFFER_USAGE_UNIFORM)
        *outputValue |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (__spud_gpu_buffer_usage & SPUDGPU_BUFFER_USAGE_STORAGE)
        *outputValue |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (__spud_gpu_buffer_usage & SPUDGPU_BUFFER_USAGE_TRANSFER_SRC)
        *outputValue |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (__spud_gpu_buffer_usage & SPUDGPU_BUFFER_USAGE_TRANSFER_DST)
        *outputValue |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (__spud_gpu_buffer_usage & SPUDGPU_BUFFER_USAGE_INDIRECT)
        *outputValue |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
}

// The first memory type in typeFilter that has every bit of [properties], and
// that type's own flags. UINT32_MAX if there is none.
static uint32_t spudgpuvulkan___internal_find_memory_type_with_flags(
    const VkPhysicalDeviceMemoryProperties *memProperties,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties,
    VkMemoryPropertyFlags *out_flags) {
    for (uint32_t i = 0; i < memProperties->memoryTypeCount; i++) {
        if (!(typeFilter & (1u << i))) continue;
        if ((memProperties->memoryTypes[i].propertyFlags & properties) != properties) continue;
        *out_flags = memProperties->memoryTypes[i].propertyFlags;
        return i;
    }
    return UINT32_MAX;
}

// The memory type a buffer of [kind] is allocated from, or a heap of [kind]
// (spudgpuvulkanheap.c): the properties each SPUDGPU_MEMORY_KIND documents
// in spudgpu.h. Nothing here asks for HOST_COHERENT: the caller flushes and
// invalidates whatever it was given.
uint32_t spudgpuvulkan___memory_type_for_kind(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    SPUDGPU_MEMORY_KIND kind,
    VkMemoryPropertyFlags *out_flags) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    switch (kind) {
    case SPUDGPU_MEMORY_KIND_DEVICE:
        return spudgpuvulkan___internal_find_memory_type_with_flags(
            &memProperties, typeFilter,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out_flags);
    case SPUDGPU_MEMORY_KIND_UPLOAD:
        return spudgpuvulkan___internal_find_memory_type_with_flags(
            &memProperties, typeFilter,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, out_flags);
    case SPUDGPU_MEMORY_KIND_READBACK: {
        // HOST_CACHED is what makes CPU reads fast, and no device has to
        // offer it: without it the contract still holds on plain
        // HOST_VISIBLE memory.
        uint32_t index = spudgpuvulkan___internal_find_memory_type_with_flags(
            &memProperties, typeFilter,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, out_flags);
        if (index != UINT32_MAX) return index;
        return spudgpuvulkan___internal_find_memory_type_with_flags(
            &memProperties, typeFilter,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, out_flags);
    }
    case SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE:
        return spudgpuvulkan___internal_find_memory_type_with_flags(
            &memProperties, typeFilter,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, out_flags);
    default:
        return UINT32_MAX;
    }
}

// [offset, offset + size) of a buffer as a range vkFlushMappedMemoryRanges
// and vkInvalidateMappedMemoryRanges accept: moved to where the buffer is in
// its memory, widened to multiples of nonCoherentAtomSize, and stopped at
// the end of the bytes the buffer covers. Those bytes start at a multiple of
// nonCoherentAtomSize (0 for a buffer's own allocation; a placed buffer's
// alignment is at least that, see spudgpuvulkanheap.c), so the widened
// range never reaches another buffer's. What is mapped is the buffer's whole
// allocation or the whole heap, so it is always inside the mapping.
static VkMappedMemoryRange spudgpuvulkan___internal_buffer_memory_range(
    spudgpu_buffer buffer,
    uint64_t offset,
    uint64_t size) {
    const VkDeviceSize atom = buffer->_device._properties_vk.limits.nonCoherentAtomSize;
    const VkDeviceSize base = buffer->_memory_offset_vk;
    VkDeviceSize begin = base + (VkDeviceSize) offset;
    VkDeviceSize end = base + (VkDeviceSize) (offset + size);
    begin -= begin % atom;
    if (end % atom) end += atom - end % atom;
    if (end > base + buffer->_memory_size_vk) end = base + buffer->_memory_size_vk;

    VkMappedMemoryRange range = {0};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.pNext = NULL;
    range.memory = buffer->_memory_vk;
    range.offset = begin;
    range.size = end - begin;
    return range;
}

SPUDRESULT spudgpuvulkan___check_buffer_desc(
    SPUDGPU_FEATURES features,
    const spudgpu_buffer_desc *desc) {
    if (desc->size == 0) return SPUDRESULT_GPU_ZERO_BUFFER_SIZE;
    if (desc->usage == 0) return SPUDRESULT_GPU_INVALID_BUFFER_USAGE;
    // SPUDGPU_MEMORY_KIND's contract (see spudgpu.h), the same checks in the
    // same order on every backend.
    if (desc->memory_kind < SPUDGPU_MEMORY_KIND_DEVICE || desc->memory_kind > SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE)
        return SPUDRESULT_GPU_INVALID_MEMORY_KIND;
    if (desc->memory_kind == SPUDGPU_MEMORY_KIND_UPLOAD &&
        (desc->usage & (SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_TRANSFER_DST)))
        return SPUDRESULT_GPU_INVALID_BUFFER_USAGE;
    if (desc->memory_kind == SPUDGPU_MEMORY_KIND_READBACK && desc->usage != SPUDGPU_BUFFER_USAGE_TRANSFER_DST)
        return SPUDRESULT_GPU_INVALID_BUFFER_USAGE;
    if (desc->memory_kind == SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE && !(features & SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY))
        return SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
    return SPUD_SUCCESS;
}

void spudgpuvulkan___buffer_create_info(
    SPUDGPU_FEATURES features,
    const spudgpu_buffer_desc *desc,
    VkBufferCreateInfo *out_info) {
    memset(out_info, 0, sizeof(*out_info));
    out_info->sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    out_info->pNext = NULL;
    out_info->size = desc->size;
    out_info->sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // Covert SPUDGPU_BUFFER_USAGE to native Vulkan usage
    spudgpuvulkan___buffer_usage_flags_internal(desc->usage, &out_info->usage);
    // A build's inputs: the bit exists only with the extension, which a
    // device has only with the feature (spudgpu.h); without it no build can
    // read the buffer, and the bit is kept in the desc alone.
    if ((desc->usage & SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT) && (features & SPUDGPU_FEATURE_RAY_QUERY))
        out_info->usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

    // Every spudgpu_buffer has a GPU address after creation
    // (spudgpu_get_buffer_gpu_address), mirroring D3D12's always-available
    // GetGPUVirtualAddress — so every buffer needs this usage bit.
    out_info->usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
}

#if __cplusplus
extern "C" {
#endif

SPUDRESULT spudgpu_create_buffer(
    spudgpu_device device,
    const spudgpu_buffer_desc *desc,
    spudgpu_buffer *out_buffer) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_create_buffer", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!desc) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_BUFFER_DESC);
    if (base_sr != SPUD_SUCCESS) return base_sr;
    if (!out_buffer) return SPUDRESULT_NULL_OUTPUT_PARAMETER;

    const SPUDRESULT descResult = spudgpuvulkan___check_buffer_desc(device->_features, desc);
    if (descResult != SPUD_SUCCESS) return descResult;

    // Create the result struct
    spudgpu_buffer_vulkan result = {0};
    result.head.allocator = device->head.allocator;
    SPUD_BIRTH(&result);
    result._device = *device;
    result._desc = *desc;

    // Get native Vulkan device handles
    VkDevice vk_device = result._device._logical_device_vk;
    VkPhysicalDevice vk_physical_device = result._device._physical_device_vk;

    VkBufferCreateInfo bufferInfo;
    spudgpuvulkan___buffer_create_info(device->_features, desc, &bufferInfo);

    // Create buffer
    if (vkCreateBuffer(vk_device, &bufferInfo, NULL, &result._buffer_vk) != VK_SUCCESS) {
        //throw std::runtime_error("SpudGPU Vulkan: failed to create buffer!");
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }

    // Allocate memory
    {
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(vk_device, result._buffer_vk, &memRequirements);

        // Fails when the buffer can use no memory type that has the kind's
        // properties.
        const uint32_t memoryTypeIndex = spudgpuvulkan___memory_type_for_kind(
            vk_physical_device, memRequirements.memoryTypeBits,
            desc->memory_kind, &result._memory_properties_vk);
        if (memoryTypeIndex == UINT32_MAX) {
            vkDestroyBuffer(vk_device, result._buffer_vk, NULL);
            return SPUDRESULT_API_SPECIFIC_FAILURE;
        }
        result._memory_size_vk = memRequirements.size;

        // Required whenever the buffer was created with
        // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT (see bufferInfo.usage
        // above) — vkGetBufferDeviceAddress below requires the backing
        // memory to have been allocated with this flag set.
        VkMemoryAllocateFlagsInfo allocFlagsInfo = {0};
        allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo allocInfo = {0};
        allocInfo.pNext = &allocFlagsInfo;
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = memoryTypeIndex;

        if (vkAllocateMemory(vk_device, &allocInfo, NULL, &result._memory_vk) != VK_SUCCESS) {
            //throw std::runtime_error("SpudGPU Vulkan: failed to allocate buffer memory!");
            vkDestroyBuffer(vk_device, result._buffer_vk, NULL);
            return SPUDRESULT_API_SPECIFIC_FAILURE;
        }

        if (vkBindBufferMemory(vk_device, result._buffer_vk, result._memory_vk, 0) != VK_SUCCESS) {
            vkDestroyBuffer(vk_device, result._buffer_vk, NULL);
            vkFreeMemory(vk_device, result._memory_vk, NULL);
            return SPUDRESULT_API_SPECIFIC_FAILURE;
        }
    }

    // The address spudgpu_get_buffer_gpu_address hands back
    {
        VkBufferDeviceAddressInfoKHR info = {0};
        info.pNext = NULL;
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = result._buffer_vk;
        result._gpu_address_vk = vkGetBufferDeviceAddress(vk_device, &info);
    }

    // If all successful, return a memcpy'ed heap pointer
    spudgpu_buffer_vulkan *pResult = SPUD_NEW(&result.head.allocator, spudgpu_buffer_vulkan);
    if (!pResult) {
        vkDestroyBuffer(vk_device, result._buffer_vk, NULL);
        vkFreeMemory(vk_device, result._memory_vk, NULL);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    memcpy(pResult, &result, sizeof(spudgpu_buffer_vulkan));
    // From here spudgpu_destroy_buffer frees it, and takes the count back.
    SPUDGPU___LIVE_ADD(&device->_shared->live, SPUDGPU___OBJECT_BUFFER);
    if (spud_debug_name_set(pResult, desc->debug_name) != SPUD_SUCCESS) {
        spudgpu_destroy_buffer(pResult);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    pResult->_desc.debug_name = pResult->head.debug_name;

    *out_buffer = pResult;
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_buffer_desc(
    spudgpu_buffer buffer,
    spudgpu_buffer_desc *out_desc) {
    if (!buffer) return SPUDRESULT_GPU_INVALID_BUFFER;
    if (SPUD_STALE(buffer, "spudgpu_get_buffer_desc", "buffer")) return SPUDRESULT_GPU_INVALID_BUFFER;
    if (!out_desc) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    *out_desc = buffer->_desc;
    out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_BUFFER_DESC);
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_buffer_gpu_address(
    spudgpu_buffer buffer,
    uint64_t *out_address) {
    if (!buffer) return SPUDRESULT_GPU_INVALID_BUFFER;
    if (SPUD_STALE(buffer, "spudgpu_get_buffer_gpu_address", "buffer")) return SPUDRESULT_GPU_INVALID_BUFFER;
    if (!out_address) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    *out_address = buffer->_gpu_address_vk;
    return SPUD_SUCCESS;
}

void spudgpu_destroy_buffer(spudgpu_buffer buffer) {
    if (!buffer) return;
    if (SPUD_STALE(buffer, "spudgpu_destroy_buffer", "buffer")) return;
    vkDestroyBuffer(buffer->_device._logical_device_vk, buffer->_buffer_vk, NULL);
    // A placed buffer's memory is its heap's.
    if (!buffer->_heap)
        vkFreeMemory(buffer->_device._logical_device_vk, buffer->_memory_vk, NULL);
#if _DEBUG
    if (buffer->_heap)
        buffer->_heap->_placed_count--;
#endif
    SPUDGPU___LIVE_REMOVE(&buffer->_device._shared->live, SPUDGPU___OBJECT_BUFFER);
    SPUD_KILL(buffer);
    const SPUD_ALLOCATOR allocator = buffer->head.allocator;
    spud_string_free(&allocator, buffer->head.debug_name);
    SPUD_DELETE(&allocator, spudgpu_buffer_vulkan, buffer);
}

SPUDRESULT spudgpu_create_buffer_view(
    spudgpu_buffer buffer,
    const spudgpu_buffer_view_desc *desc,
    spudgpu_buffer_view *out_view) {
    if (!buffer) return SPUDRESULT_GPU_INVALID_BUFFER;
    if (SPUD_STALE(buffer, "spudgpu_create_buffer_view", "buffer")) return SPUDRESULT_GPU_INVALID_BUFFER;
    if (!desc) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC);
    if (base_sr != SPUD_SUCCESS) return base_sr;
    if (!out_view) return SPUDRESULT_NULL_OUTPUT_PARAMETER;

	// Validate desc
	if (desc->size == 0)
		return SPUDRESULT_GPU_ZERO_BUFFER_SIZE;

	// Validate that the view range fits within the parent buffer
	if (desc->offset_from_parent_buffer + desc->size > buffer->_desc.size)
		return SPUDRESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;

	spudgpu_buffer_view_vulkan result = {0};
    result.head.allocator = buffer->head.allocator;
    SPUD_BIRTH(&result);
    result._desc = *desc;
    result._desc.parent_buffer = buffer;
    result._buffer_view_vk = VK_NULL_HANDLE; // Only needed for texel buffers

    /*
     * Vulkan buffer views are not needed for anything other than a texel buffer.
     * This is because buffer handles + offsets are passed directly into Vulkan commands.
     * Unlike D3D12 which has explicit D3D12*_VIEW handles.
     */

    // If all successful, return a memcpy'ed heap pointer
    spudgpu_buffer_view pResult = SPUD_NEW(&result.head.allocator, spudgpu_buffer_view_vulkan);
    if (!pResult) return SPUDRESULT_OUT_OF_MEMORY;
    memcpy(pResult, &result, sizeof(spudgpu_buffer_view_vulkan));
    pResult->_shared = buffer->_device._shared;
    SPUDGPU___LIVE_ADD(&pResult->_shared->live, SPUDGPU___OBJECT_BUFFER_VIEW);
    *out_view = pResult;
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_buffer_view_desc(
    spudgpu_buffer_view buffer_view,
    spudgpu_buffer_view_desc *out_desc) {
    if (!buffer_view) return SPUDRESULT_GPU_INVALID_BUFFER_VIEW;
    if (SPUD_STALE(buffer_view, "spudgpu_get_buffer_view_desc", "buffer_view")) return SPUDRESULT_GPU_INVALID_BUFFER_VIEW;
    if (!out_desc) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    *out_desc = buffer_view->_desc;
    out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC);
    return SPUD_SUCCESS;
}

void spudgpu_destroy_buffer_view(spudgpu_buffer_view buffer_view) {
    if (!buffer_view) return;
    if (SPUD_STALE(buffer_view, "spudgpu_destroy_buffer_view", "buffer_view")) return;
	vkDestroyBufferView(
	    buffer_view->_desc.parent_buffer->_device._logical_device_vk,
	    buffer_view->_buffer_view_vk, NULL);
	SPUDGPU___LIVE_REMOVE(&buffer_view->_shared->live, SPUDGPU___OBJECT_BUFFER_VIEW);
	SPUD_KILL(buffer_view);
	const SPUD_ALLOCATOR allocator = buffer_view->head.allocator;
	spud_string_free(&allocator, buffer_view->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_buffer_view_vulkan, buffer_view);
}

SPUDRESULT spudgpu_map_buffer(
    spudgpu_buffer buffer,
    uint64_t offset,
    uint64_t size,
    void **ppData) {
    if (!buffer) return SPUDRESULT_GPU_INVALID_BUFFER;
    if (SPUD_STALE(buffer, "spudgpu_map_buffer", "buffer")) return SPUDRESULT_GPU_INVALID_BUFFER;
    //if (!size) return SPUDRESULT_GPU_ZERO_BUFFER_SIZE;
    if (offset + size > buffer->_desc.size) return SPUDRESULT_GPU_MAP_OUT_OF_RANGE;
    if (!ppData) return SPUDRESULT_NULL_OUTPUT_PARAMETER;

    if (buffer->_desc.memory_kind == SPUDGPU_MEMORY_KIND_DEVICE)
        return SPUDRESULT_GPU_INVALID_MEMORY_KIND;

    // A placed buffer's heap mapped all of its memory when it was created
    // (Vulkan allows one mapping of a VkDeviceMemory at a time, and other
    // buffers of the heap may be mapped too): the buffer's bytes are already
    // in that mapping.
    if (buffer->_heap) {
        *ppData = (uint8_t *) buffer->_heap->_mapped + buffer->_memory_offset_vk + offset;
        return SPUD_SUCCESS;
    }

    // The whole allocation is mapped, whatever range was asked for: a flush
    // or invalidate range has to be widened to nonCoherentAtomSize (see
    // spudgpuvulkan___internal_buffer_memory_range) and still lie inside
    // the mapping.
    void *mapped = NULL;
    if (vkMapMemory(
        buffer->_device._logical_device_vk,
        buffer->_memory_vk,
        0,
        VK_WHOLE_SIZE,
        0, // flags — reserved, must be 0
        &mapped) != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;

    *ppData = (uint8_t *) mapped + offset;
	return SPUD_SUCCESS;
}

void spudgpu_unmap_buffer(spudgpu_buffer buffer) {
    if (!buffer) return;
    if (SPUD_STALE(buffer, "spudgpu_unmap_buffer", "buffer")) return;
    // The heap's mapping lasts as long as the heap does.
    if (buffer->_heap) return;
    vkUnmapMemory(
        buffer->_device._logical_device_vk,
        buffer->_memory_vk);
}

SPUDRESULT spudgpu_flush_buffer(
    spudgpu_buffer buffer,
    uint64_t offset,
    uint64_t size) {
    if (!buffer) return SPUDRESULT_GPU_INVALID_BUFFER;
    if (SPUD_STALE(buffer, "spudgpu_flush_buffer", "buffer")) return SPUDRESULT_GPU_INVALID_BUFFER;
    if (buffer->_desc.memory_kind != SPUDGPU_MEMORY_KIND_UPLOAD &&
        buffer->_desc.memory_kind != SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE)
        return SPUDRESULT_GPU_INVALID_MEMORY_KIND;
    if (offset + size > buffer->_desc.size) return SPUDRESULT_GPU_MAP_OUT_OF_RANGE;

    // Coherent memory has nothing to flush.
    if (buffer->_memory_properties_vk & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        return SPUD_SUCCESS;

    const VkMappedMemoryRange range = spudgpuvulkan___internal_buffer_memory_range(
        buffer, offset, (size == 0) ? buffer->_desc.size - offset : size);
    if (vkFlushMappedMemoryRanges(
        buffer->_device._logical_device_vk,
        1, &range) != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;

    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_invalidate_buffer(
    spudgpu_buffer buffer,
    uint64_t offset,
    uint64_t size) {
    if (!buffer) return SPUDRESULT_GPU_INVALID_BUFFER;
    if (SPUD_STALE(buffer, "spudgpu_invalidate_buffer", "buffer")) return SPUDRESULT_GPU_INVALID_BUFFER;
    if (buffer->_desc.memory_kind != SPUDGPU_MEMORY_KIND_READBACK &&
        buffer->_desc.memory_kind != SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE)
        return SPUDRESULT_GPU_INVALID_MEMORY_KIND;
    if (offset + size > buffer->_desc.size) return SPUDRESULT_GPU_MAP_OUT_OF_RANGE;

    // Coherent memory has nothing to invalidate.
    if (buffer->_memory_properties_vk & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        return SPUD_SUCCESS;

    const VkMappedMemoryRange range = spudgpuvulkan___internal_buffer_memory_range(
        buffer, offset, (size == 0) ? buffer->_desc.size - offset : size);
    if (vkInvalidateMappedMemoryRanges(
        buffer->_device._logical_device_vk,
        1, &range) != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;

    return SPUD_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // SPUDGPU_COMPILE_VULKAN_API
