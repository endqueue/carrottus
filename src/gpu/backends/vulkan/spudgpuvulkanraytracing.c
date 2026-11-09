
#if SPUDGPU_COMPILE_VULKAN_API

// SPUDGPU_EXT_RAY_QUERY on Vulkan: VK_KHR_acceleration_structure and
// VK_KHR_ray_query. What every backend checks the same way is in
// src/gpu/spudgpushared.c; this file translates.
//
// A structure is a VkBuffer of its size with
// VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, in memory of its
// own or at the caller's offset of a heap's, and the
// VkAccelerationStructureKHR over the whole buffer. Builds are one
// vkCmdBuildAccelerationStructuresKHR per call, never run on the host: the
// deferred host operations Vulkan requires the extension to come with are
// never used. A command list's arrays for a call come from its scratch
// block (spudgpuvulkan___scratch), which keeps its capacity, so a steady
// stream of builds allocates nothing.

#include <vulkan/vulkan.h>
#include "spudgpu.h"
#include "spudgpuvulkan.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../../../spudstructbase.h"

// The layout spudgpu___internal_instance_layout() states for Vulkan.
_Static_assert(sizeof(VkAccelerationStructureInstanceKHR) == 64,
               "SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64 is Vulkan's instance");

// SpudGPU's build flags in Vulkan's bits.
static VkBuildAccelerationStructureFlagsKHR spudgpuvulkan___rq_build_flags(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
    VkBuildAccelerationStructureFlagsKHR vk = 0;
    if (flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE) vk |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD) vk |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    if (flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE) vk |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    if (flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION) vk |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    return vk;
}

static VkGeometryFlagsKHR spudgpuvulkan___rq_geometry_flags(SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS flags) {
    VkGeometryFlagsKHR vk = 0;
    if (flags & SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE) vk |= VK_GEOMETRY_OPAQUE_BIT_KHR;
    if (flags & SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_CANDIDATES) vk |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
    return vk;
}

static VkAccelerationStructureTypeKHR spudgpuvulkan___rq_type(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL level) {
    return level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                                                             : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
}

// How many VkAccelerationStructureGeometryKHR [inputs] are: a bottom level's
// geometries, or a top level's one instances geometry.
static uint32_t spudgpuvulkan___rq_geometry_slots(const spudgpu_acceleration_structure_inputs *inputs) {
    return inputs->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP ? 1u : inputs->geometry_count;
}

// Fills [geometries] and [ranges], spudgpuvulkan___rq_geometry_slots of
// each, for [inputs], which have passed their checks. Every offset is folded
// into its address, so each range's offsets are 0.
static void spudgpuvulkan___rq_fill(
    const spudgpu_acceleration_structure_inputs *inputs,
    VkAccelerationStructureGeometryKHR *geometries,
    VkAccelerationStructureBuildRangeInfoKHR *ranges) {
    if (inputs->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP) {
        VkAccelerationStructureGeometryKHR *g = &geometries[0];
        memset(g, 0, sizeof(*g));
        g->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g->geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        g->geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        g->geometry.instances.arrayOfPointers = VK_FALSE;
        g->geometry.instances.data.deviceAddress = inputs->instances->_gpu_address_vk + inputs->instance_offset;
        memset(&ranges[0], 0, sizeof(ranges[0]));
        ranges[0].primitiveCount = inputs->instance_count;
        return;
    }
    for (uint32_t i = 0; i < inputs->geometry_count; i++) {
        const spudgpu_acceleration_structure_geometry *source = &inputs->geometries[i];
        VkAccelerationStructureGeometryKHR *g = &geometries[i];
        memset(g, 0, sizeof(*g));
        memset(&ranges[i], 0, sizeof(ranges[i]));
        g->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g->flags = spudgpuvulkan___rq_geometry_flags(source->flags);
        const VkDeviceAddress data = source->data->_gpu_address_vk + source->data_offset;
        switch (source->kind) {
        case SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES: {
            VkAccelerationStructureGeometryTrianglesDataKHR *t = &g->geometry.triangles;
            g->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            t->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t->vertexData.deviceAddress = data;
            t->vertexStride = source->data_stride;
            t->maxVertex = source->data_count - 1;
            if (source->index_format == SPUDGPU_FORMAT_UNKNOWN) {
                t->indexType = VK_INDEX_TYPE_NONE_KHR;
                ranges[i].primitiveCount = source->data_count / 3;
            } else {
                t->indexType = source->index_format == SPUDGPU_FORMAT_R16_UINT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
                t->indexData.deviceAddress = source->indices->_gpu_address_vk + source->index_offset;
                ranges[i].primitiveCount = source->index_count / 3;
            }
            break;
        }
        case SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_AABBS:
            g->geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
            g->geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
            g->geometry.aabbs.data.deviceAddress = data;
            g->geometry.aabbs.stride = source->data_stride;
            ranges[i].primitiveCount = source->data_count;
            break;
        default:
            break;
        }
    }
}

// The build geometry info of [inputs] over [geometries], without the
// structures and scratch a build adds.
static VkAccelerationStructureBuildGeometryInfoKHR spudgpuvulkan___rq_info(
    const spudgpu_acceleration_structure_inputs *inputs,
    const VkAccelerationStructureGeometryKHR *geometries) {
    VkAccelerationStructureBuildGeometryInfoKHR info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    info.type = spudgpuvulkan___rq_type(inputs->level);
    info.flags = spudgpuvulkan___rq_build_flags(inputs->flags);
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = spudgpuvulkan___rq_geometry_slots(inputs);
    info.pGeometries = geometries;
    return info;
}

// [inputs]' sizes, from [geometries] and [ranges] as spudgpuvulkan___rq_fill
// made them. An update needs no scratch without ALLOW_UPDATE (spudgpu.h).
static void spudgpuvulkan___rq_sizes(
    const spudgpu_device_vulkan *device,
    const spudgpu_acceleration_structure_inputs *inputs,
    const VkAccelerationStructureGeometryKHR *geometries,
    const VkAccelerationStructureBuildRangeInfoKHR *ranges,
    uint32_t *max_primitives,
    spudgpu_acceleration_structure_sizes *out_sizes) {
    const uint32_t slots = spudgpuvulkan___rq_geometry_slots(inputs);
    for (uint32_t i = 0; i < slots; i++) max_primitives[i] = ranges[i].primitiveCount;
    const VkAccelerationStructureBuildGeometryInfoKHR info = spudgpuvulkan___rq_info(inputs, geometries);
    VkAccelerationStructureBuildSizesInfoKHR sizes;
    memset(&sizes, 0, sizeof(sizes));
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    device->_vkGetAccelerationStructureBuildSizesKHR(
        device->_logical_device_vk, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, max_primitives, &sizes);
    out_sizes->storage_size = sizes.accelerationStructureSize;
    out_sizes->build_scratch_size = sizes.buildScratchSize;
    out_sizes->update_scratch_size =
        (inputs->flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE) ? sizes.updateScratchSize : 0;
}

SPUDRESULT spudgpuvulkan___acceleration_structure_memory_bits(
    const spudgpu_device_vulkan *device,
    uint64_t size,
    uint32_t *out_bits,
    VkMemoryRequirements *out_requirements) {
    VkBufferCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device->_logical_device_vk, &info, NULL, &buffer) != VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device->_logical_device_vk, buffer, &requirements);
    vkDestroyBuffer(device->_logical_device_vk, buffer, NULL);
    *out_bits = requirements.memoryTypeBits;
    if (out_requirements) *out_requirements = requirements;
    return SPUD_SUCCESS;
}

// Releases what a structure holds of Vulkan's, each part made or null, and
// its handle: what destroy does and what a create that fails part way does.
static void spudgpuvulkan___rq_release(spudgpu_acceleration_structure_vulkan *structure) {
    const spudgpu_device_vulkan *device = structure->_device;
    VkDevice vk = device->_logical_device_vk;
    if (structure->_compacted_size_query_vk) vkDestroyQueryPool(vk, structure->_compacted_size_query_vk, NULL);
    if (structure->_structure_vk) device->_vkDestroyAccelerationStructureKHR(vk, structure->_structure_vk, NULL);
    if (structure->_buffer_vk) vkDestroyBuffer(vk, structure->_buffer_vk, NULL);
    if (structure->_memory_vk) vkFreeMemory(vk, structure->_memory_vk, NULL);
    const SPUD_ALLOCATOR allocator = structure->head.allocator;
    spudgpu___internal_acceleration_structure_state_free(&structure->_state, &allocator);
    spud_string_free(&allocator, structure->head.debug_name);
    SPUD_RETIRE(SPUD_QUARANTINE_OF(structure), &allocator, spudgpu_acceleration_structure_vulkan, structure);
}

// spudgpu_create_acceleration_structure and the placed one after their
// argument checks: committed when [heap] is NULL, placed at [offset] of it
// otherwise (its offset already found a multiple of placement_alignment and
// the rounded range inside the heap).
static SPUDRESULT spudgpuvulkan___rq_create(
    const spudgpu_device_vulkan *device,
    const spud_object_head *owner,
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure) {
    VkDevice vk = device->_logical_device_vk;
    spudgpu_acceleration_structure_vulkan *structure =
        SPUD_NEW_ZEROED(&owner->allocator, spudgpu_acceleration_structure_vulkan);
    if (!structure) return SPUDRESULT_OUT_OF_MEMORY;
    structure->head.allocator = owner->allocator;
    SPUD_BIRTH_IN(structure, &device->_shared->quarantine);
    structure->_device = device;
    structure->_heap = heap;
    if (!spudgpu___internal_acceleration_structure_state_init(&structure->_state, &structure->head.allocator, desc)) {
        spudgpuvulkan___rq_release(structure);
        return SPUDRESULT_OUT_OF_MEMORY;
    }

    VkBufferCreateInfo bufferInfo = {0};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc->size;
    bufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk, &bufferInfo, NULL, &structure->_buffer_vk) != VK_SUCCESS) {
        spudgpuvulkan___rq_release(structure);
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk, structure->_buffer_vk, &requirements);

    if (heap) {
        // The buffer's own alignment and size, where the graphics API asks
        // more than the reported rule (none has, on the GPUs tested): the
        // bytes it covers stay inside the range the caller placed it over,
        // its size rounded up to placement_alignment.
        const uint64_t alignment = device->_adapter->_facts.ray_query_placement_alignment;
        const uint64_t covered = desc->size + (alignment - desc->size % alignment) % alignment;
        if (offset % requirements.alignment != 0) {
            spudgpuvulkan___rq_release(structure);
            return SPUDRESULT_GPU_PLACEMENT_MISALIGNED;
        }
        if (requirements.size > covered) {
            spudgpuvulkan___rq_release(structure);
            return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
        }
        if (!(requirements.memoryTypeBits & (1u << heap->_memory_type_index_vk)) ||
            vkBindBufferMemory(vk, structure->_buffer_vk, heap->_memory_vk, offset) != VK_SUCCESS) {
            spudgpuvulkan___rq_release(structure);
            return SPUDRESULT_API_SPECIFIC_FAILURE;
        }
    } else {
        VkMemoryPropertyFlags flags = 0;
        const uint32_t type = spudgpuvulkan___memory_type_for_kind(
            device->_physical_device_vk, requirements.memoryTypeBits, SPUDGPU_MEMORY_KIND_DEVICE, &flags);
        if (type == UINT32_MAX) {
            spudgpuvulkan___rq_release(structure);
            return SPUDRESULT_API_SPECIFIC_FAILURE;
        }
        // The structure's device address needs its memory allocated so.
        VkMemoryAllocateFlagsInfo allocFlags = {0};
        allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocInfo = {0};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.pNext = &allocFlags;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = type;
        if (vkAllocateMemory(vk, &allocInfo, NULL, &structure->_memory_vk) != VK_SUCCESS ||
            vkBindBufferMemory(vk, structure->_buffer_vk, structure->_memory_vk, 0) != VK_SUCCESS) {
            spudgpuvulkan___rq_release(structure);
            return SPUDRESULT_API_SPECIFIC_FAILURE;
        }
    }

    VkAccelerationStructureCreateInfoKHR createInfo = {0};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = structure->_buffer_vk;
    createInfo.offset = 0;
    createInfo.size = desc->size;
    createInfo.type = spudgpuvulkan___rq_type(desc->level);
    if (device->_vkCreateAccelerationStructureKHR(vk, &createInfo, NULL, &structure->_structure_vk) != VK_SUCCESS) {
        spudgpuvulkan___rq_release(structure);
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }
    VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {0};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = structure->_structure_vk;
    structure->_address_vk = device->_vkGetAccelerationStructureDeviceAddressKHR(vk, &addressInfo);

    if (desc->allow_compaction) {
        VkQueryPoolCreateInfo queryInfo = {0};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        queryInfo.queryCount = 1;
        if (vkCreateQueryPool(vk, &queryInfo, NULL, &structure->_compacted_size_query_vk) != VK_SUCCESS) {
            spudgpuvulkan___rq_release(structure);
            return SPUDRESULT_API_SPECIFIC_FAILURE;
        }
    }

    if (spud_debug_name_set(structure, desc->debug_name) != SPUD_SUCCESS) {
        spudgpuvulkan___rq_release(structure);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    SPUDGPU___LIVE_ADD(&device->_shared->live, SPUDGPU___OBJECT_ACCELERATION_STRUCTURE);
#if _DEBUG
    if (heap) heap->_placed_count++;
#endif
    *out_structure = structure;
    return SPUD_SUCCESS;
}

#if __cplusplus
extern "C" {
#endif

SPUDRESULT spudgpu_get_acceleration_structure_build_sizes(
    spudgpu_device device,
    const spudgpu_acceleration_structure_inputs *inputs,
    spudgpu_acceleration_structure_sizes *out_sizes) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_get_acceleration_structure_build_sizes", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!inputs) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT inputs_base_sr = spud_struct_check(&inputs->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS);
    if (inputs_base_sr != SPUD_SUCCESS) return inputs_base_sr;
    if (!out_sizes) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    const SPUDRESULT out_base_sr = spud_struct_out_check(&out_sizes->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES);
    if (out_base_sr != SPUD_SUCCESS) return out_base_sr;
    if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY)) return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
    const SPUDRESULT inputs_sr = spudgpu___internal_check_acceleration_structure_inputs(&device->_adapter->_facts, inputs);
    if (inputs_sr != SPUD_SUCCESS) return inputs_sr;

    // The geometries, ranges and counts Vulkan's query reads, for this call
    // only.
    const uint32_t slots = spudgpuvulkan___rq_geometry_slots(inputs);
    const SPUD_ALLOCATOR *allocator = &device->head.allocator;
    VkAccelerationStructureGeometryKHR *geometries = SPUD_NEW_ARRAY(allocator, VkAccelerationStructureGeometryKHR, slots);
    VkAccelerationStructureBuildRangeInfoKHR *ranges = SPUD_NEW_ARRAY(allocator, VkAccelerationStructureBuildRangeInfoKHR, slots);
    uint32_t *max_primitives = SPUD_NEW_ARRAY(allocator, uint32_t, slots);
    SPUDRESULT sr = SPUD_SUCCESS;
    if (geometries && ranges && max_primitives) {
        spudgpuvulkan___rq_fill(inputs, geometries, ranges);
        spudgpu_acceleration_structure_sizes sizes = *out_sizes;
        spudgpuvulkan___rq_sizes(device, inputs, geometries, ranges, max_primitives, &sizes);
        *out_sizes = sizes;
    } else {
        sr = SPUDRESULT_OUT_OF_MEMORY;
    }
    SPUD_DELETE_ARRAY(allocator, uint32_t, max_primitives, slots);
    SPUD_DELETE_ARRAY(allocator, VkAccelerationStructureBuildRangeInfoKHR, ranges, slots);
    SPUD_DELETE_ARRAY(allocator, VkAccelerationStructureGeometryKHR, geometries, slots);
    return sr;
}

SPUDRESULT spudgpu_create_acceleration_structure(
    spudgpu_device device,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_create_acceleration_structure", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!desc) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC);
    if (base_sr != SPUD_SUCCESS) return base_sr;
    if (!out_structure) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY)) return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
    const SPUDRESULT desc_sr = spudgpu___internal_check_acceleration_structure_desc(desc);
    if (desc_sr != SPUD_SUCCESS) return desc_sr;
    return spudgpuvulkan___rq_create(device, &device->head, NULL, 0, desc, out_structure);
}

SPUDRESULT spudgpu_create_placed_acceleration_structure(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure) {
    if (!heap) return SPUDRESULT_GPU_INVALID_HEAP;
    if (SPUD_STALE(heap, "spudgpu_create_placed_acceleration_structure", "heap")) return SPUDRESULT_GPU_INVALID_HEAP;
    if (!desc) return SPUDRESULT_NULL_DESC;
    const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC);
    if (base_sr != SPUD_SUCCESS) return base_sr;
    if (!out_structure) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    const spudgpu_device_vulkan *device = &heap->_device;
    if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY)) return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
    const SPUDRESULT desc_sr = spudgpu___internal_check_acceleration_structure_desc(desc);
    if (desc_sr != SPUD_SUCCESS) return desc_sr;
    if (heap->_desc.resource_class != SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES) return SPUDRESULT_GPU_HEAP_MISMATCH;
    const uint64_t alignment = device->_adapter->_facts.ray_query_placement_alignment;
    if (offset % alignment != 0) return SPUDRESULT_GPU_PLACEMENT_MISALIGNED;
    // The size rounded up to the alignment, from the offset, inside the
    // heap, with nothing that can wrap.
    if (offset > heap->_desc.size) return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
    const uint64_t room = heap->_desc.size - offset;
    if (desc->size > room || (desc->size + (alignment - desc->size % alignment) % alignment) > room)
        return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
    return spudgpuvulkan___rq_create(device, &heap->head, heap, offset, desc, out_structure);
}

void spudgpu_destroy_acceleration_structure(spudgpu_acceleration_structure structure) {
    if (!structure) return;
    if (SPUD_STALE(structure, "spudgpu_destroy_acceleration_structure", "structure")) return;
#if _DEBUG
    if (structure->_heap) structure->_heap->_placed_count--;
#endif
    SPUDGPU___LIVE_REMOVE(&structure->_device->_shared->live, SPUDGPU___OBJECT_ACCELERATION_STRUCTURE);
    SPUD_KILL(structure);
    spudgpuvulkan___rq_release(structure);
}

SPUDRESULT spudgpu_get_acceleration_structure_reference(
    spudgpu_acceleration_structure bottom_level,
    uint64_t *out_reference) {
    if (!bottom_level) return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
    if (SPUD_STALE(bottom_level, "spudgpu_get_acceleration_structure_reference", "bottom_level"))
        return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
    if (bottom_level->_state.level != SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM)
        return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
    if (!out_reference) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    *out_reference = bottom_level->_address_vk;
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_acceleration_structure_instance_layout(
    spudgpu_device device,
    spudgpu_acceleration_structure_instance_layout *out_layout) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_get_acceleration_structure_instance_layout", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!out_layout) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY)) return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
    *out_layout = spudgpu___internal_instance_layout();
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_write_acceleration_structure_instances(
    spudgpu_device device,
    const spudgpu_acceleration_structure_instance *instances,
    uint32_t count,
    void *out_bytes) {
    if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (SPUD_STALE(device, "spudgpu_write_acceleration_structure_instances", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
    if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY)) return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
    return spudgpu___internal_write_instances(spudgpu___internal_instance_layout(), instances, count, out_bytes);
}

// Adds [n] elements of [size] bytes to *[bytes], rounded to the scratch
// block's words: false where the sum can't be held in a size_t.
static bool spudgpuvulkan___rq_grow(size_t *bytes, size_t n, size_t size) {
    if (size != 0 && n > (SIZE_MAX - 8u - *bytes) / size) return false;
    *bytes = SPUDGPUVULKAN___SCRATCH_ROUND(*bytes + n * size);
    return true;
}

// The scratch block a build call needs at most so far: one build's size
// query ([query_slots] geometries, their ranges and counts) or the
// recording block ([count] build infos and range pointers, then [slots]
// geometries and ranges), whichever is larger. False where either can't be
// held in a size_t.
static bool spudgpuvulkan___rq_block_bytes(size_t query_slots, uint32_t count, size_t slots, size_t *out_bytes) {
    size_t query = 0;
    if (!spudgpuvulkan___rq_grow(&query, query_slots, sizeof(VkAccelerationStructureGeometryKHR)) ||
        !spudgpuvulkan___rq_grow(&query, query_slots, sizeof(VkAccelerationStructureBuildRangeInfoKHR)) ||
        !spudgpuvulkan___rq_grow(&query, query_slots, sizeof(uint32_t)))
        return false;
    size_t record = 0;
    if (!spudgpuvulkan___rq_grow(&record, count, sizeof(VkAccelerationStructureBuildGeometryInfoKHR)) ||
        !spudgpuvulkan___rq_grow(&record, count, sizeof(VkAccelerationStructureBuildRangeInfoKHR *)) ||
        !spudgpuvulkan___rq_grow(&record, slots, sizeof(VkAccelerationStructureGeometryKHR)) ||
        !spudgpuvulkan___rq_grow(&record, slots, sizeof(VkAccelerationStructureBuildRangeInfoKHR)))
        return false;
    *out_bytes = query > record ? query : record;
    return true;
}

SPUDRESULT spudgpu_cmd_build_acceleration_structures(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure_build *builds,
    uint32_t count) {
    if (!cmd) return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
    if (SPUD_STALE(cmd, "spudgpu_cmd_build_acceleration_structures", "cmd")) return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
    const spudgpu_device_vulkan *device = &cmd->_allocator._device;
    if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY)) return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
    const SPUDRESULT list_sr = spudgpu___internal_check_acceleration_structure_list(cmd->_allocator._type, cmd->_rendering_active);
    if (list_sr != SPUD_SUCCESS) return list_sr;
    if (!builds) return SPUDRESULT_NULL_DESC;
    if (count == 0) return SPUDRESULT_ZERO_SIZE;
    const spudgpu___adapter_facts *facts = &device->_adapter->_facts;

    // Every build checked, in order, before anything is recorded. Each
    // one's geometries are made in the list's scratch block to ask its
    // sizes, then thrown away; the block is grown there to what the
    // recording below needs of every build so far, and the destination's
    // layout room made after the build's last check, so the recording
    // itself can't fail.
    size_t total_slots = 0;
    for (uint32_t b = 0; b < count; b++) {
        const spudgpu_acceleration_structure_build *build = &builds[b];
        const SPUDRESULT head_sr = spudgpu___internal_check_build_head(facts, build);
        if (head_sr != SPUD_SUCCESS) return head_sr;
        const uint32_t slots = spudgpuvulkan___rq_geometry_slots(build->inputs);
        if (slots > SIZE_MAX - total_slots) return SPUDRESULT_OUT_OF_MEMORY;
        total_slots += slots;
        size_t block_bytes = 0;
        if (!spudgpuvulkan___rq_block_bytes(slots, count, total_slots, &block_bytes)) return SPUDRESULT_OUT_OF_MEMORY;
        uint8_t *block = (uint8_t *) spudgpuvulkan___scratch_take(&cmd->_scratch, block_bytes);
        if (!block) return SPUDRESULT_OUT_OF_MEMORY;
        const size_t ranges_at = SPUDGPUVULKAN___SCRATCH_ROUND(sizeof(VkAccelerationStructureGeometryKHR) * (size_t) slots);
        const size_t counts_at = ranges_at + SPUDGPUVULKAN___SCRATCH_ROUND(sizeof(VkAccelerationStructureBuildRangeInfoKHR) * (size_t) slots);
        VkAccelerationStructureGeometryKHR *geometries = (VkAccelerationStructureGeometryKHR *) block;
        VkAccelerationStructureBuildRangeInfoKHR *ranges = (VkAccelerationStructureBuildRangeInfoKHR *) (block + ranges_at);
        spudgpuvulkan___rq_fill(build->inputs, geometries, ranges);
        spudgpu_acceleration_structure_sizes sizes;
        memset(&sizes, 0, sizeof(sizes));
        spudgpuvulkan___rq_sizes(device, build->inputs, geometries, ranges, (uint32_t *) (block + counts_at), &sizes);
        const SPUDRESULT tail_sr = spudgpu___internal_check_build_tail(facts, build, &sizes);
        if (tail_sr != SPUD_SUCCESS) return tail_sr;
        const SPUDRESULT layout_sr = spudgpu___internal_reserve_build_layout(build->destination, build->inputs);
        if (layout_sr != SPUD_SUCCESS) return layout_sr;
    }

    // One block for the call, within what the checks grew it to: the build
    // infos, the range pointers, then every build's geometries and ranges,
    // back to back.
    size_t block_bytes = 0;
    (void) spudgpuvulkan___rq_block_bytes(0, count, total_slots, &block_bytes);
    uint8_t *block = (uint8_t *) spudgpuvulkan___scratch_take(&cmd->_scratch, block_bytes);
    assert(block && "spudgpu: the checks grew the build's block");
    const size_t pointers_at = SPUDGPUVULKAN___SCRATCH_ROUND(sizeof(VkAccelerationStructureBuildGeometryInfoKHR) * (size_t) count);
    const size_t geometries_at = pointers_at + SPUDGPUVULKAN___SCRATCH_ROUND(sizeof(VkAccelerationStructureBuildRangeInfoKHR *) * (size_t) count);
    const size_t ranges_at = geometries_at + SPUDGPUVULKAN___SCRATCH_ROUND(sizeof(VkAccelerationStructureGeometryKHR) * total_slots);
    VkAccelerationStructureBuildGeometryInfoKHR *infos = (VkAccelerationStructureBuildGeometryInfoKHR *) block;
    const VkAccelerationStructureBuildRangeInfoKHR **range_pointers =
        (const VkAccelerationStructureBuildRangeInfoKHR **) (block + pointers_at);
    VkAccelerationStructureGeometryKHR *geometries = (VkAccelerationStructureGeometryKHR *) (block + geometries_at);
    VkAccelerationStructureBuildRangeInfoKHR *ranges = (VkAccelerationStructureBuildRangeInfoKHR *) (block + ranges_at);
    size_t slot = 0;
    for (uint32_t b = 0; b < count; b++) {
        const spudgpu_acceleration_structure_build *build = &builds[b];
        spudgpuvulkan___rq_fill(build->inputs, &geometries[slot], &ranges[slot]);
        infos[b] = spudgpuvulkan___rq_info(build->inputs, &geometries[slot]);
        if (build->mode == SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE) {
            infos[b].mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            infos[b].srcAccelerationStructure = build->source->_structure_vk;
        }
        infos[b].dstAccelerationStructure = build->destination->_structure_vk;
        infos[b].scratchData.deviceAddress = build->scratch->_gpu_address_vk + build->scratch_offset;
        range_pointers[b] = &ranges[slot];
        slot += spudgpuvulkan___rq_geometry_slots(build->inputs);
    }
    device->_vkCmdBuildAccelerationStructuresKHR(cmd->_command_buffer_vk, count, infos, range_pointers);
    for (uint32_t b = 0; b < count; b++) spudgpu___internal_record_build(&builds[b]);
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_cmd_copy_acceleration_structure(
    spudgpu_command_list cmd,
    spudgpu_acceleration_structure source,
    spudgpu_acceleration_structure destination,
    SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE mode) {
    if (!cmd) return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
    if (SPUD_STALE(cmd, "spudgpu_cmd_copy_acceleration_structure", "cmd")) return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
    const spudgpu_device_vulkan *device = &cmd->_allocator._device;
    if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY)) return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
    const SPUDRESULT list_sr = spudgpu___internal_check_acceleration_structure_list(cmd->_allocator._type, cmd->_rendering_active);
    if (list_sr != SPUD_SUCCESS) return list_sr;
    const SPUDRESULT sr = spudgpu___internal_check_copy(source, destination, mode);
    if (sr != SPUD_SUCCESS) return sr;
    VkCopyAccelerationStructureInfoKHR info = {0};
    info.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
    info.src = source->_structure_vk;
    info.dst = destination->_structure_vk;
    info.mode = mode == SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT ? VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR
                                                                        : VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
    device->_vkCmdCopyAccelerationStructureKHR(cmd->_command_buffer_vk, &info);
    spudgpu___internal_record_copy(source, destination);
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_cmd_write_acceleration_structure_compacted_sizes(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure *structures,
    uint32_t count,
    spudgpu_buffer buffer,
    uint64_t offset) {
    if (!cmd) return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
    if (SPUD_STALE(cmd, "spudgpu_cmd_write_acceleration_structure_compacted_sizes", "cmd")) return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
    const spudgpu_device_vulkan *device = &cmd->_allocator._device;
    if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY)) return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
    const SPUDRESULT list_sr = spudgpu___internal_check_acceleration_structure_list(cmd->_allocator._type, cmd->_rendering_active);
    if (list_sr != SPUD_SUCCESS) return list_sr;
    const SPUDRESULT sr = spudgpu___internal_check_compacted_sizes(structures, count, buffer, offset);
    if (sr != SPUD_SUCCESS) return sr;
    // Each structure's own query: reset, written, and copied into the
    // caller's buffer as a 64-bit value once the write is done.
    for (uint32_t i = 0; i < count; i++) {
        const spudgpu_acceleration_structure_vulkan *structure = structures[i];
        vkCmdResetQueryPool(cmd->_command_buffer_vk, structure->_compacted_size_query_vk, 0, 1);
        device->_vkCmdWriteAccelerationStructuresPropertiesKHR(
            cmd->_command_buffer_vk, 1, &structure->_structure_vk,
            VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, structure->_compacted_size_query_vk, 0);
        vkCmdCopyQueryPoolResults(
            cmd->_command_buffer_vk, structure->_compacted_size_query_vk, 0, 1, buffer->_buffer_vk,
            offset + (uint64_t) i * 8u, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    }
    return SPUD_SUCCESS;
}

// Where and how an access reads or writes, before a barrier (as its source)
// or after it (as its destination). [compute_only] for a list of a compute
// queue, which has no fragment stage.
static void spudgpuvulkan___rq_access(
    SPUDGPU_ACCELERATION_STRUCTURE_ACCESS access,
    bool before,
    bool compute_only,
    VkPipelineStageFlags *stages,
    VkAccessFlags *accesses) {
    switch (access) {
    case SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE:
        // Out of NONE: after everything earlier submissions did, as a
        // buffer's barrier out of COMMON is.
        if (before) {
            *stages |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            *accesses |= VK_ACCESS_MEMORY_WRITE_BIT;
        }
        break;
    case SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE:
        *stages |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        *accesses |= VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        break;
    case SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ:
        *stages |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        // The build that reads it also writes its own destination and
        // scratch, which the barrier orders after the earlier writes too.
        *accesses |= before ? VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
                            : VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        break;
    case SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE:
        *stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        if (!compute_only) *stages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        *accesses |= VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        break;
    default:
        break;
    }
}

void spudgpu_cmd_acceleration_structure_barrier(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure_barrier *barriers,
    uint32_t count) {
    if (!cmd) return;
    if (SPUD_STALE(cmd, "spudgpu_cmd_acceleration_structure_barrier", "cmd")) return;
    if (!(cmd->_allocator._device._features & SPUDGPU_FEATURE_RAY_QUERY)) return;
    if (!spudgpu___internal_check_acceleration_structure_barriers(barriers, count)) return;
    const bool compute_only = cmd->_allocator._type == SPUDGPU_COMMAND_LIST_TYPE_COMPUTE;
    VkPipelineStageFlags src_stages = 0, dst_stages = 0;
    VkMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    for (uint32_t i = 0; i < count; i++) {
        spudgpuvulkan___rq_access(barriers[i].access_before, true, compute_only, &src_stages, &barrier.srcAccessMask);
        spudgpuvulkan___rq_access(barriers[i].access_after, false, compute_only, &dst_stages, &barrier.dstAccessMask);
    }
    if (!src_stages) src_stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (!dst_stages) dst_stages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    vkCmdPipelineBarrier(cmd->_command_buffer_vk, src_stages, dst_stages, 0, 1, &barrier, 0, NULL, 0, NULL);
}

#if __cplusplus
}
#endif

#endif // SPUDGPU_COMPILE_VULKAN_API
