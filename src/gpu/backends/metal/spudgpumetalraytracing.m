//
// SPUDGPU_EXT_RAY_QUERY on Metal: Metal 3's acceleration structures and
// intersection queries, with indirect instances. What every backend checks
// the same way is in src/gpu/spudgpushared.c; this file translates.
//
// Written 2026-10-31 on Windows against Apple's documentation, to the same
// contract as the Vulkan and D3D12 files; not compiled. The Mac's pass
// compiles it, runs spudgpu_objects_test and spud_noalloc_tests under Metal
// API and shader validation, and checks the 72-byte instance layout against
// MTLIndirectAccelerationStructureInstanceDescriptor.
//
// A structure is an MTLAccelerationStructure from the device
// (newAccelerationStructureWithSize:) or a heap
// (newAccelerationStructureWithSize:offset:). Each call that records opens
// one MTLAccelerationStructureCommandEncoder, after ending any compute or
// blit encoder, and ends it before returning. A top level's instances are
// MTLAccelerationStructureInstanceDescriptorTypeIndirect, naming bottom
// levels by gpuResourceID, which tells Metal nothing of residency: the
// bottom levels a top level's build named (spudgpu___acceleration_structure_state)
// are made resident with useResource: on the encoder that builds or copies
// it, and by spudgpu_cmd_bind_descriptor_sets(_compute) on every encoder a
// set holding it is bound to (spudgpumetal___use_bottom_levels). Barriers
// record nothing: Metal tracks the hazards of a structure made from the
// device, and of one in a heap, which this backend makes tracked.

#if SPUDGPU_COMPILE_METAL_API

#include "spudgpu.h"
#include "spudcore.h"
#include "spudgpumetal.h"
#include <Metal/Metal.h>
#include "../../../spudstructbase.h"

// The layout spudgpu___internal_instance_layout() states for Metal.
_Static_assert(sizeof(MTLIndirectAccelerationStructureInstanceDescriptor) == 72,
               "SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72 is Metal's indirect instance descriptor");

void spudgpumetal___use_bottom_levels(id<MTLCommandEncoder> encoder, bool render, MTLRenderStages stages,
                                      const struct spudgpu_acceleration_structure_t *top) {
	const spudgpu___acceleration_structure_state *state = &top->_state;
	for (uint32_t i = 0; i < state->bottom_level_count; i++) {
		const spudgpu_acceleration_structure_metal *bottom = state->bottom_levels[i];
		if (render)
			[(id<MTLRenderCommandEncoder>)encoder useResource:bottom->_structure_mtl usage:MTLResourceUsageRead stages:stages];
		else
			[(id<MTLComputeCommandEncoder>)encoder useResource:bottom->_structure_mtl usage:MTLResourceUsageRead];
	}
}

static MTLAccelerationStructureUsage spudgpumetal___rq_usage(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
	// Metal has no fast-trace usage: it is the default. Compaction needs no
	// usage either.
	MTLAccelerationStructureUsage usage = MTLAccelerationStructureUsageNone;
	if (flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD)
		usage |= MTLAccelerationStructureUsagePreferFastBuild;
	if (flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE)
		usage |= MTLAccelerationStructureUsageRefit;
	return usage;
}

// Metal's descriptor for [inputs], which have passed their checks:
// autoreleased, so made inside the caller's pool.
static MTLAccelerationStructureDescriptor *spudgpumetal___rq_descriptor(const spudgpu_acceleration_structure_inputs *inputs) {
	if (inputs->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP) {
		MTLInstanceAccelerationStructureDescriptor *d = [MTLInstanceAccelerationStructureDescriptor descriptor];
		d.usage                                       = spudgpumetal___rq_usage(inputs->flags);
		d.instanceDescriptorType                      = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
		d.instanceDescriptorBuffer                    = ((spudgpu_buffer_metal *)inputs->instances)->_buffer_mtl;
		d.instanceDescriptorBufferOffset              = (NSUInteger)inputs->instance_offset;
		d.instanceDescriptorStride                    = sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
		d.instanceCount                               = inputs->instance_count;
		return d;
	}
	NSMutableArray<MTLAccelerationStructureGeometryDescriptor *> *geometries =
	    [NSMutableArray arrayWithCapacity:inputs->geometry_count];
	for (uint32_t i = 0; i < inputs->geometry_count; i++) {
		const spudgpu_acceleration_structure_geometry *source = &inputs->geometries[i];
		const bool opaque    = (source->flags & SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE) != 0;
		const bool duplicate = (source->flags & SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_CANDIDATES) == 0;
		if (source->kind == SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES) {
			MTLAccelerationStructureTriangleGeometryDescriptor *g = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
			g.vertexBuffer       = ((spudgpu_buffer_metal *)source->data)->_buffer_mtl;
			g.vertexBufferOffset = (NSUInteger)source->data_offset;
			g.vertexStride       = (NSUInteger)source->data_stride;
			g.vertexFormat       = MTLAttributeFormatFloat3;
			if (source->index_format != SPUDGPU_FORMAT_UNKNOWN) {
				g.indexBuffer       = ((spudgpu_buffer_metal *)source->indices)->_buffer_mtl;
				g.indexBufferOffset = (NSUInteger)source->index_offset;
				g.indexType         = source->index_format == SPUDGPU_FORMAT_R16_UINT ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
				g.triangleCount     = source->index_count / 3;
			} else {
				g.triangleCount = source->data_count / 3;
			}
			g.opaque                                      = opaque;
			g.allowDuplicateIntersectionFunctionInvocation = duplicate;
			[geometries addObject:g];
		} else {
			MTLAccelerationStructureBoundingBoxGeometryDescriptor *g = [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
			g.boundingBoxBuffer                          = ((spudgpu_buffer_metal *)source->data)->_buffer_mtl;
			g.boundingBoxBufferOffset                    = (NSUInteger)source->data_offset;
			g.boundingBoxStride                          = (NSUInteger)source->data_stride;
			g.boundingBoxCount                           = source->data_count;
			g.opaque                                     = opaque;
			g.allowDuplicateIntersectionFunctionInvocation = duplicate;
			[geometries addObject:g];
		}
	}
	MTLPrimitiveAccelerationStructureDescriptor *d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
	d.usage                                        = spudgpumetal___rq_usage(inputs->flags);
	d.geometryDescriptors                          = geometries;
	return d;
}

static void spudgpumetal___rq_sizes(
    spudgpu_device_metal *device,
    const spudgpu_acceleration_structure_inputs *inputs,
    MTLAccelerationStructureDescriptor *descriptor,
    spudgpu_acceleration_structure_sizes *out_sizes) {
	const MTLAccelerationStructureSizes sizes = [device->_device_mtl accelerationStructureSizesWithDescriptor:descriptor];
	out_sizes->storage_size                   = sizes.accelerationStructureSize;
	out_sizes->build_scratch_size             = sizes.buildScratchBufferSize;
	out_sizes->update_scratch_size =
	    (inputs->flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE) ? sizes.refitScratchBufferSize : 0;
}

// An acceleration structure command encoder on [cmd]'s command buffer, any
// compute or blit encoder ended first (a build is outside a rendering pass,
// so no render encoder is open). The caller ends it.
static id<MTLAccelerationStructureCommandEncoder> spudgpumetal___rq_encoder(spudgpu_command_list_metal *cmd) {
	spudgpumetal___internal_end_active_compute_encoder(cmd);
	spudgpumetal___internal_end_active_blit_encoder(cmd);
	return [cmd->_command_buffer_mtl accelerationStructureCommandEncoder];
}

static void spudgpumetal___rq_release(spudgpu_acceleration_structure_metal *structure) {
	[structure->_structure_mtl release];
	const SPUD_ALLOCATOR allocator = structure->head.allocator;
	spudgpu___internal_acceleration_structure_state_free(&structure->_state, &allocator);
	spud_string_free(&allocator, structure->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_acceleration_structure_metal, structure);
}

// spudgpu_create_acceleration_structure and the placed one after their
// argument checks: from the device when [heap] is NULL, at [offset] of it
// otherwise.
static SPUDRESULT spudgpumetal___rq_create(
    spudgpu_device_metal *device,
    spudgpu_heap_metal *heap,
    uint64_t offset,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure) {
	const SPUD_ALLOCATOR *allocator = heap ? &heap->head.allocator : &device->head.allocator;
	spudgpu_acceleration_structure_metal *structure = SPUD_NEW_ZEROED(allocator, spudgpu_acceleration_structure_metal);
	if (!structure)
		return SPUDRESULT_OUT_OF_MEMORY;
	structure->head.allocator = *allocator;
	SPUD_BIRTH(structure);
	structure->_parent_device = device;
	structure->_heap          = (spudgpu_heap)heap;
	if (!spudgpu___internal_acceleration_structure_state_init(&structure->_state, &structure->head.allocator, desc)) {
		spudgpumetal___rq_release(structure);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	if (heap) {
		// Metal's own alignment and size for one of this size, where it
		// asks more than the reported rule: the bytes it covers stay inside
		// the range the caller placed it over, its size rounded up to
		// placement_alignment.
		const uint64_t alignment   = device->_adapter->_facts.ray_query_placement_alignment;
		const uint64_t covered     = desc->size + (alignment - desc->size % alignment) % alignment;
		const MTLSizeAndAlign need = [device->_device_mtl heapAccelerationStructureSizeAndAlignWithSize:(NSUInteger)desc->size];
		if (offset % need.align != 0) {
			spudgpumetal___rq_release(structure);
			return SPUDRESULT_GPU_PLACEMENT_MISALIGNED;
		}
		if ((uint64_t)need.size > covered) {
			spudgpumetal___rq_release(structure);
			return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
		}
		structure->_structure_mtl = [heap->_heap_mtl newAccelerationStructureWithSize:(NSUInteger)desc->size offset:(NSUInteger)offset];
	} else {
		structure->_structure_mtl = [device->_device_mtl newAccelerationStructureWithSize:(NSUInteger)desc->size];
	}
	if (!structure->_structure_mtl) {
		spudgpumetal___rq_release(structure);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	structure->_resource_id = structure->_structure_mtl.gpuResourceID._impl;
	if (spud_debug_name_set(structure, desc->debug_name) != SPUD_SUCCESS) {
		spudgpumetal___rq_release(structure);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	if (structure->head.debug_name) {
		// Allocated and released here rather than autoreleased: a caller in
		// plain C may have no autorelease pool.
		NSString *label = [[NSString alloc] initWithUTF8String:structure->head.debug_name];
		structure->_structure_mtl.label = label;
		[label release];
	}
	SPUDGPU___LIVE_ADD(&device->_live, SPUDGPU___OBJECT_ACCELERATION_STRUCTURE);
#if _DEBUG
	if (heap)
		heap->_placed_count++;
#endif
	*out_structure = (spudgpu_acceleration_structure)structure;
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_acceleration_structure_build_sizes(
    spudgpu_device device,
    const spudgpu_acceleration_structure_inputs *inputs,
    spudgpu_acceleration_structure_sizes *out_sizes) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_get_acceleration_structure_build_sizes", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!inputs)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT inputs_base_sr = spud_struct_check(&inputs->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS);
	if (inputs_base_sr != SPUD_SUCCESS)
		return inputs_base_sr;
	if (!out_sizes)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT out_base_sr = spud_struct_out_check(&out_sizes->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES);
	if (out_base_sr != SPUD_SUCCESS)
		return out_base_sr;
	spudgpu_device_metal *device_metal = (spudgpu_device_metal *)device;
	if (!(device_metal->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT inputs_sr = spudgpu___internal_check_acceleration_structure_inputs(&device_metal->_adapter->_facts, inputs);
	if (inputs_sr != SPUD_SUCCESS)
		return inputs_sr;
	@autoreleasepool {
		spudgpu_acceleration_structure_sizes sizes = *out_sizes;
		spudgpumetal___rq_sizes(device_metal, inputs, spudgpumetal___rq_descriptor(inputs), &sizes);
		*out_sizes = sizes;
	}
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_create_acceleration_structure(
    spudgpu_device device,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_acceleration_structure", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_structure)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	spudgpu_device_metal *device_metal = (spudgpu_device_metal *)device;
	if (!(device_metal->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT desc_sr = spudgpu___internal_check_acceleration_structure_desc(desc);
	if (desc_sr != SPUD_SUCCESS)
		return desc_sr;
	return spudgpumetal___rq_create(device_metal, NULL, 0, desc, out_structure);
}

SPUDRESULT spudgpu_create_placed_acceleration_structure(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure) {
	if (!heap)
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (SPUD_STALE(heap, "spudgpu_create_placed_acceleration_structure", "heap"))
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_structure)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	spudgpu_heap_metal *heap_metal     = (spudgpu_heap_metal *)heap;
	spudgpu_device_metal *device_metal = heap_metal->_parent_device;
	if (!(device_metal->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT desc_sr = spudgpu___internal_check_acceleration_structure_desc(desc);
	if (desc_sr != SPUD_SUCCESS)
		return desc_sr;
	if (heap_metal->_desc.resource_class != SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES)
		return SPUDRESULT_GPU_HEAP_MISMATCH;
	const uint64_t alignment = device_metal->_adapter->_facts.ray_query_placement_alignment;
	if (offset % alignment != 0)
		return SPUDRESULT_GPU_PLACEMENT_MISALIGNED;
	// The size rounded up to the alignment, from the offset, inside the
	// heap, with nothing that can wrap.
	if (offset > heap_metal->_desc.size)
		return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	const uint64_t room = heap_metal->_desc.size - offset;
	if (desc->size > room || (desc->size + (alignment - desc->size % alignment) % alignment) > room)
		return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	return spudgpumetal___rq_create(device_metal, heap_metal, offset, desc, out_structure);
}

void spudgpu_destroy_acceleration_structure(spudgpu_acceleration_structure structure) {
	if (!structure)
		return;
	if (SPUD_STALE(structure, "spudgpu_destroy_acceleration_structure", "structure"))
		return;
	spudgpu_acceleration_structure_metal *structure_metal = (spudgpu_acceleration_structure_metal *)structure;
#if _DEBUG
	if (structure_metal->_heap)
		((spudgpu_heap_metal *)structure_metal->_heap)->_placed_count--;
#endif
	SPUDGPU___LIVE_REMOVE(&structure_metal->_parent_device->_live, SPUDGPU___OBJECT_ACCELERATION_STRUCTURE);
	SPUD_KILL(structure);
	spudgpumetal___rq_release(structure_metal);
}

SPUDRESULT spudgpu_get_acceleration_structure_reference(
    spudgpu_acceleration_structure bottom_level,
    uint64_t *out_reference) {
	if (!bottom_level)
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (SPUD_STALE(bottom_level, "spudgpu_get_acceleration_structure_reference", "bottom_level"))
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	const spudgpu_acceleration_structure_metal *structure = (const spudgpu_acceleration_structure_metal *)bottom_level;
	if (structure->_state.level != SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM)
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (!out_reference)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_reference = structure->_resource_id;
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_acceleration_structure_instance_layout(
    spudgpu_device device,
    spudgpu_acceleration_structure_instance_layout *out_layout) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_get_acceleration_structure_instance_layout", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!out_layout)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!(((spudgpu_device_metal *)device)->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	*out_layout = spudgpu___internal_instance_layout();
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_write_acceleration_structure_instances(
    spudgpu_device device,
    const spudgpu_acceleration_structure_instance *instances,
    uint32_t count,
    void *out_bytes) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_write_acceleration_structure_instances", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!(((spudgpu_device_metal *)device)->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	return spudgpu___internal_write_instances(spudgpu___internal_instance_layout(), instances, count, out_bytes);
}

SPUDRESULT spudgpu_cmd_build_acceleration_structures(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure_build *builds,
    uint32_t count) {
	if (!cmd)
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	if (SPUD_STALE(cmd, "spudgpu_cmd_build_acceleration_structures", "cmd"))
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	spudgpu_command_list_metal *cmd_metal = (spudgpu_command_list_metal *)cmd;
	spudgpu_device_metal *device          = cmd_metal->_parent_allocator->_parent_device;
	if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT list_sr = spudgpu___internal_check_acceleration_structure_list(
	    cmd_metal->_parent_allocator->_desc.type, cmd_metal->_active_render_encoder != nil);
	if (list_sr != SPUD_SUCCESS)
		return list_sr;
	if (!builds)
		return SPUDRESULT_NULL_DESC;
	if (count == 0)
		return SPUDRESULT_ZERO_SIZE;
	const spudgpu___adapter_facts *facts = &device->_adapter->_facts;

	@autoreleasepool {
		// Every build checked, in order, before anything is recorded, and
		// the destination's layout room made after its last check, so the
		// recording below can't fail.
		for (uint32_t b = 0; b < count; b++) {
			const spudgpu_acceleration_structure_build *build = &builds[b];
			const SPUDRESULT head_sr                         = spudgpu___internal_check_build_head(facts, build);
			if (head_sr != SPUD_SUCCESS)
				return head_sr;
			spudgpu_acceleration_structure_sizes sizes;
			memset(&sizes, 0, sizeof(sizes));
			spudgpumetal___rq_sizes(device, build->inputs, spudgpumetal___rq_descriptor(build->inputs), &sizes);
			const SPUDRESULT tail_sr = spudgpu___internal_check_build_tail(facts, build, &sizes);
			if (tail_sr != SPUD_SUCCESS)
				return tail_sr;
			const SPUDRESULT layout_sr = spudgpu___internal_reserve_build_layout(build->destination, build->inputs);
			if (layout_sr != SPUD_SUCCESS)
				return layout_sr;
		}

		id<MTLAccelerationStructureCommandEncoder> encoder = spudgpumetal___rq_encoder(cmd_metal);
		for (uint32_t b = 0; b < count; b++) {
			const spudgpu_acceleration_structure_build *build = &builds[b];
			const spudgpu_acceleration_structure_inputs *inputs = build->inputs;
			// A top level's bottom levels, resident while it is built.
			if (inputs->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP)
				for (uint32_t i = 0; i < inputs->bottom_level_count; i++)
					[encoder useResource:((spudgpu_acceleration_structure_metal *)inputs->bottom_levels[i])->_structure_mtl
					               usage:MTLResourceUsageRead];
			MTLAccelerationStructureDescriptor *descriptor        = spudgpumetal___rq_descriptor(inputs);
			id<MTLAccelerationStructure> destination              = ((spudgpu_acceleration_structure_metal *)build->destination)->_structure_mtl;
			id<MTLBuffer> scratch                                 = ((spudgpu_buffer_metal *)build->scratch)->_buffer_mtl;
			if (build->mode == SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE) {
				// In place when the source is the destination: Metal takes nil
				// as the destination for that.
				id<MTLAccelerationStructure> source = ((spudgpu_acceleration_structure_metal *)build->source)->_structure_mtl;
				[encoder refitAccelerationStructure:source
				                         descriptor:descriptor
				                        destination:(build->source == build->destination ? nil : destination)
				                      scratchBuffer:scratch
				                scratchBufferOffset:(NSUInteger)build->scratch_offset];
			} else {
				[encoder buildAccelerationStructure:destination
				                         descriptor:descriptor
				                      scratchBuffer:scratch
				                scratchBufferOffset:(NSUInteger)build->scratch_offset];
			}
			spudgpu___internal_record_build(build);
		}
		[encoder endEncoding];
	}
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_cmd_copy_acceleration_structure(
    spudgpu_command_list cmd,
    spudgpu_acceleration_structure source,
    spudgpu_acceleration_structure destination,
    SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE mode) {
	if (!cmd)
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	if (SPUD_STALE(cmd, "spudgpu_cmd_copy_acceleration_structure", "cmd"))
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	spudgpu_command_list_metal *cmd_metal = (spudgpu_command_list_metal *)cmd;
	if (!(cmd_metal->_parent_allocator->_parent_device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT list_sr = spudgpu___internal_check_acceleration_structure_list(
	    cmd_metal->_parent_allocator->_desc.type, cmd_metal->_active_render_encoder != nil);
	if (list_sr != SPUD_SUCCESS)
		return list_sr;
	const SPUDRESULT sr = spudgpu___internal_check_copy(source, destination, mode);
	if (sr != SPUD_SUCCESS)
		return sr;
	@autoreleasepool {
		const spudgpu_acceleration_structure_metal *from = (const spudgpu_acceleration_structure_metal *)source;
		const spudgpu_acceleration_structure_metal *to   = (const spudgpu_acceleration_structure_metal *)destination;
		id<MTLAccelerationStructureCommandEncoder> encoder = spudgpumetal___rq_encoder(cmd_metal);
		for (uint32_t i = 0; i < from->_state.bottom_level_count; i++)
			[encoder useResource:((spudgpu_acceleration_structure_metal *)from->_state.bottom_levels[i])->_structure_mtl
			               usage:MTLResourceUsageRead];
		if (mode == SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT)
			[encoder copyAndCompactAccelerationStructure:from->_structure_mtl toAccelerationStructure:to->_structure_mtl];
		else
			[encoder copyAccelerationStructure:from->_structure_mtl toAccelerationStructure:to->_structure_mtl];
		[encoder endEncoding];
	}
	spudgpu___internal_record_copy(source, destination);
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_cmd_write_acceleration_structure_compacted_sizes(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure *structures,
    uint32_t count,
    spudgpu_buffer buffer,
    uint64_t offset) {
	if (!cmd)
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	if (SPUD_STALE(cmd, "spudgpu_cmd_write_acceleration_structure_compacted_sizes", "cmd"))
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	spudgpu_command_list_metal *cmd_metal = (spudgpu_command_list_metal *)cmd;
	if (!(cmd_metal->_parent_allocator->_parent_device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT list_sr = spudgpu___internal_check_acceleration_structure_list(
	    cmd_metal->_parent_allocator->_desc.type, cmd_metal->_active_render_encoder != nil);
	if (list_sr != SPUD_SUCCESS)
		return list_sr;
	const SPUDRESULT sr = spudgpu___internal_check_compacted_sizes(structures, count, buffer, offset);
	if (sr != SPUD_SUCCESS)
		return sr;
	@autoreleasepool {
		id<MTLAccelerationStructureCommandEncoder> encoder = spudgpumetal___rq_encoder(cmd_metal);
		id<MTLBuffer> buffer_mtl                           = ((spudgpu_buffer_metal *)buffer)->_buffer_mtl;
		for (uint32_t i = 0; i < count; i++)
			[encoder writeCompactedAccelerationStructureSize:((const spudgpu_acceleration_structure_metal *)structures[i])->_structure_mtl
			                                        toBuffer:buffer_mtl
			                                          offset:(NSUInteger)(offset + (uint64_t)i * 8u)
			                                    sizeDataType:MTLDataTypeULong];
		[encoder endEncoding];
	}
	return SPUD_SUCCESS;
}

void spudgpu_cmd_acceleration_structure_barrier(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure_barrier *barriers,
    uint32_t count) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_acceleration_structure_barrier", "cmd"))
		return;
	spudgpu_command_list_metal *cmd_metal = (spudgpu_command_list_metal *)cmd;
	if (!(cmd_metal->_parent_allocator->_parent_device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return;
	// The same checks as every backend, and then nothing: Metal tracks a
	// structure's hazards itself (see the top of this file).
	(void)spudgpu___internal_check_acceleration_structure_barriers(barriers, count);
}

#endif // SPUDGPU_COMPILE_METAL_API
