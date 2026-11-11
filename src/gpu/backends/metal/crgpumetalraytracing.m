//
// CRGPU_EXT_RAY_QUERY on Metal: Metal 3's acceleration structures and
// intersection queries, with indirect instances. What every backend checks
// the same way is in src/gpu/crgpushared.c; this file translates.
//
// Written 2026-10-31 on Windows against Apple's documentation, to the same
// contract as the Vulkan and D3D12 files; not compiled. The Mac's pass
// compiles it, runs crgpu_objects_test and cr_noalloc_tests under Metal
// API and shader validation, and checks the 72-byte instance layout against
// MTLIndirectAccelerationStructureInstanceDescriptor. Compiled and run on
// the Mac on 2026-11-01 (Apple M5 Pro, macOS 27), under Metal API and
// shader validation with warnings fatal.
//
// A structure is an MTLAccelerationStructure from the device
// (newAccelerationStructureWithSize:) or a heap
// (newAccelerationStructureWithSize:offset:). Each call that records opens
// one MTLAccelerationStructureCommandEncoder, after ending any compute or
// blit encoder, and ends it before returning. A top level's instances are
// MTLAccelerationStructureInstanceDescriptorTypeIndirect, naming bottom
// levels by gpuResourceID, which tells Metal nothing of residency: the
// bottom levels a top level's build named (crgpu___acceleration_structure_state)
// are kept as Metal objects on the top level (_bottom_levels_mtl) and made
// resident with one useResources:count:usage: on the encoder that builds or
// copies it, and on every encoder a set holding it is bound to by
// crgpu_command_list_bind_graphics_descriptor_sets(_compute) (crgpumetal___use_bottom_levels). Barriers
// record nothing: Metal tracks the hazards of a structure made from the
// device, and of one in a heap, which this backend makes tracked.

#if CRGPU_COMPILE_METAL_API

#include "crgpu.h"
#include "crcore.h"
#include "crgpumetal.h"
#include <Metal/Metal.h>
#include <stddef.h>
#include <string.h>
#include "../../../crstructbase.h"

// The layout crgpu___internal_instance_layout() states for Metal, and the
// offsets crgpu___internal_write_instances() writes each field at.
_Static_assert(sizeof(MTLIndirectAccelerationStructureInstanceDescriptor) == 72,
               "CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72 is Metal's indirect instance descriptor");
_Static_assert(_Alignof(MTLIndirectAccelerationStructureInstanceDescriptor) == 8, "the layout's alignment");
_Static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor, transformationMatrix) == 0, "four columns of three floats");
_Static_assert(sizeof(MTLPackedFloat4x3) == 48, "four columns of three floats");
_Static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor, options) == 48, "the flags");
_Static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor, mask) == 52, "the mask");
_Static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor, intersectionFunctionTableOffset) == 56, "the table offset");
_Static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor, userID) == 60, "the user index");
_Static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor, accelerationStructureID) == 64, "the bottom level's reference");
_Static_assert(sizeof(MTLResourceID) == 8, "the bottom level's reference");

void crgpumetal___use_bottom_levels(id<MTLCommandEncoder> encoder, bool render, MTLRenderStages stages,
                                      const struct crgpu_acceleration_structure_t *top) {
	const uint32_t count = top->_state.bottom_level_count;
	if (count == 0)
		return;
	if (render)
		[(id<MTLRenderCommandEncoder>)encoder useResources:top->_bottom_levels_mtl count:count usage:MTLResourceUsageRead stages:stages];
	else
		[(id<MTLComputeCommandEncoder>)encoder useResources:top->_bottom_levels_mtl count:count usage:MTLResourceUsageRead];
}

static MTLAccelerationStructureUsage crgpumetal___rq_usage(CRGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
	// Metal has no fast-trace usage: it is the default. Compaction needs no
	// usage either.
	MTLAccelerationStructureUsage usage = MTLAccelerationStructureUsageNone;
	if (flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD)
		usage |= MTLAccelerationStructureUsagePreferFastBuild;
	if (flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE)
		usage |= MTLAccelerationStructureUsageRefit;
	return usage;
}

// Fills a geometry descriptor of [source]'s kind (made by the caller) for
// [source], which has passed its checks. Every property a build sets is set,
// so a descriptor filled before for another build keeps nothing of it.
static void crgpumetal___rq_fill_geometry(MTLAccelerationStructureGeometryDescriptor *descriptor,
                                            const crgpu_acceleration_structure_geometry *source) {
	descriptor.opaque = (source->flags & CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE) != 0;
	descriptor.allowDuplicateIntersectionFunctionInvocation =
	    (source->flags & CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_CANDIDATES) == 0;
	if (source->kind == CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES) {
		MTLAccelerationStructureTriangleGeometryDescriptor *g = (MTLAccelerationStructureTriangleGeometryDescriptor *)descriptor;
		g.vertexBuffer       = ((crgpu_buffer_metal *)source->data)->_buffer_mtl;
		g.vertexBufferOffset = (NSUInteger)source->data_offset;
		g.vertexStride       = (NSUInteger)source->data_stride;
		g.vertexFormat       = MTLAttributeFormatFloat3;
		if (source->index_format != CRGPU_FORMAT_UNKNOWN) {
			g.indexBuffer       = ((crgpu_buffer_metal *)source->indices)->_buffer_mtl;
			g.indexBufferOffset = (NSUInteger)source->index_offset;
			g.indexType         = source->index_format == CRGPU_FORMAT_R16_UINT ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
			g.triangleCount     = source->index_count / 3;
		} else {
			g.indexBuffer       = nil;
			g.indexBufferOffset = 0;
			g.triangleCount     = source->data_count / 3;
		}
	} else {
		MTLAccelerationStructureBoundingBoxGeometryDescriptor *g = (MTLAccelerationStructureBoundingBoxGeometryDescriptor *)descriptor;
		g.boundingBoxBuffer       = ((crgpu_buffer_metal *)source->data)->_buffer_mtl;
		g.boundingBoxBufferOffset = (NSUInteger)source->data_offset;
		g.boundingBoxStride       = (NSUInteger)source->data_stride;
		g.boundingBoxCount        = source->data_count;
	}
}

static Class crgpumetal___rq_geometry_class(CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_KIND kind) {
	return kind == CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES ? [MTLAccelerationStructureTriangleGeometryDescriptor class]
	                                                                 : [MTLAccelerationStructureBoundingBoxGeometryDescriptor class];
}

// Fills an instance descriptor for [inputs], a top level that has passed its
// checks.
static void crgpumetal___rq_fill_top(MTLInstanceAccelerationStructureDescriptor *d, const crgpu_acceleration_structure_inputs *inputs) {
	d.usage                          = crgpumetal___rq_usage(inputs->flags);
	d.instanceDescriptorType         = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
	d.instanceDescriptorBuffer       = ((crgpu_buffer_metal *)inputs->instances)->_buffer_mtl;
	d.instanceDescriptorBufferOffset = (NSUInteger)inputs->instance_offset;
	d.instanceDescriptorStride       = sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
	d.instanceCount                  = inputs->instance_count;
}

// A new descriptor for [inputs], which have passed their checks:
// autoreleased, so made inside the caller's pool. Only for
// crgpu_device_get_acceleration_structure_build_sizes(), which has no structure
// to keep one on.
static MTLAccelerationStructureDescriptor *crgpumetal___rq_new_descriptor(const crgpu_acceleration_structure_inputs *inputs) {
	if (inputs->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP) {
		MTLInstanceAccelerationStructureDescriptor *d = [MTLInstanceAccelerationStructureDescriptor descriptor];
		crgpumetal___rq_fill_top(d, inputs);
		return d;
	}
	NSMutableArray<MTLAccelerationStructureGeometryDescriptor *> *geometries =
	    [NSMutableArray arrayWithCapacity:inputs->geometry_count];
	for (uint32_t i = 0; i < inputs->geometry_count; i++) {
		MTLAccelerationStructureGeometryDescriptor *g = [[crgpumetal___rq_geometry_class(inputs->geometries[i].kind) alloc] init];
		crgpumetal___rq_fill_geometry(g, &inputs->geometries[i]);
		[geometries addObject:g];
		[g release];
	}
	MTLPrimitiveAccelerationStructureDescriptor *d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
	d.usage                                        = crgpumetal___rq_usage(inputs->flags);
	d.geometryDescriptors                          = geometries;
	return d;
}

// Grows [pool] to [count] descriptors of [kind]. False when one couldn't
// be made.
static bool crgpumetal___rq_grow_pool(NSMutableArray *pool, Class kind, uint32_t count) {
	while (pool.count < count) {
		MTLAccelerationStructureGeometryDescriptor *g = [[kind alloc] init];
		if (!g)
			return false;
		[pool addObject:g];
		[g release];
	}
	return true;
}

// [cmd]'s descriptor for [inputs]'s level (a list of a device with
// CRGPU_FEATURE_RAY_QUERY), filled for [inputs], which have passed their
// head checks. A geometry descriptor is made only when [inputs] have more of
// its kind than any build [cmd] has filled before. Nil when one couldn't be
// made; a build checked by it before is filled again with nothing made.
static MTLAccelerationStructureDescriptor *crgpumetal___rq_list_descriptor(crgpu_command_list_metal *cmd,
                                                                             const crgpu_acceleration_structure_inputs *inputs) {
	if (inputs->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP) {
		crgpumetal___rq_fill_top(cmd->_rq_top_desc_mtl, inputs);
		return cmd->_rq_top_desc_mtl;
	}
	uint32_t triangle_count = 0;
	for (uint32_t i = 0; i < inputs->geometry_count; i++)
		triangle_count += inputs->geometries[i].kind == CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES;
	if (!crgpumetal___rq_grow_pool(cmd->_rq_triangles_mtl, [MTLAccelerationStructureTriangleGeometryDescriptor class], triangle_count) ||
	    !crgpumetal___rq_grow_pool(cmd->_rq_boxes_mtl, [MTLAccelerationStructureBoundingBoxGeometryDescriptor class],
	                                 inputs->geometry_count - triangle_count))
		return nil;
	NSMutableArray<MTLAccelerationStructureGeometryDescriptor *> *geometries = cmd->_rq_geometries_mtl;
	while (geometries.count > inputs->geometry_count)
		[geometries removeLastObject];
	uint32_t triangle = 0;
	uint32_t box      = 0;
	for (uint32_t i = 0; i < inputs->geometry_count; i++) {
		MTLAccelerationStructureGeometryDescriptor *g = inputs->geometries[i].kind == CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES
		                                                    ? cmd->_rq_triangles_mtl[triangle++]
		                                                    : cmd->_rq_boxes_mtl[box++];
		crgpumetal___rq_fill_geometry(g, &inputs->geometries[i]);
		if (i == geometries.count)
			[geometries addObject:g];
		else if (geometries[i] != g)
			[geometries replaceObjectAtIndex:i withObject:g];
	}
	cmd->_rq_bottom_desc_mtl.usage = crgpumetal___rq_usage(inputs->flags);
	return cmd->_rq_bottom_desc_mtl;
}

static void crgpumetal___rq_sizes(
    crgpu_device_metal *device,
    const crgpu_acceleration_structure_inputs *inputs,
    MTLAccelerationStructureDescriptor *descriptor,
    crgpu_acceleration_structure_sizes *out_sizes) {
	const MTLAccelerationStructureSizes sizes = [device->_device_mtl accelerationStructureSizesWithDescriptor:descriptor];
	out_sizes->storage_size                   = sizes.accelerationStructureSize;
	out_sizes->build_scratch_size             = sizes.buildScratchBufferSize;
	out_sizes->update_scratch_size =
	    (inputs->flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE) ? sizes.refitScratchBufferSize : 0;
}

// An acceleration structure command encoder on [cmd]'s command buffer, any
// compute or blit encoder ended first (a build is outside a rendering pass,
// so no render encoder is open). The caller ends it.
static id<MTLAccelerationStructureCommandEncoder> crgpumetal___rq_encoder(crgpu_command_list_metal *cmd) {
	crgpumetal___internal_end_active_compute_encoder(cmd);
	crgpumetal___internal_end_active_blit_encoder(cmd);
	return [cmd->_command_buffer_mtl accelerationStructureCommandEncoder];
}

static void crgpumetal___rq_release(crgpu_acceleration_structure_metal *structure) {
	[structure->_structure_mtl release];
	const CR_ALLOCATOR allocator = structure->head.allocator;
	if (structure->_bottom_levels_mtl)
		CR_DELETE_ARRAY(&allocator, id<MTLResource>, structure->_bottom_levels_mtl, structure->_state.max_bottom_levels);
	crgpu___internal_acceleration_structure_state_free(&structure->_state, &allocator);
	cr_string_free(&allocator, structure->head.debug_name);
	CR_DELETE(&allocator, crgpu_acceleration_structure_metal, structure);
}

// crgpu_acceleration_structure_create and the placed one after their
// argument checks: from the device when [heap] is NULL, at [offset] of it
// otherwise.
static CR_RESULT crgpumetal___rq_create(
    crgpu_device_metal *device,
    crgpu_heap_metal *heap,
    uint64_t offset,
    const crgpu_acceleration_structure_desc *desc,
    crgpu_acceleration_structure *out_structure) {
	const CR_ALLOCATOR *allocator = heap ? &heap->head.allocator : &device->head.allocator;
	crgpu_acceleration_structure_metal *structure = CR_NEW_ZEROED(allocator, crgpu_acceleration_structure_metal);
	if (!structure)
		return CR_RESULT_OUT_OF_MEMORY;
	structure->head.allocator = *allocator;
	CR_BIRTH(structure);
	structure->_parent_device = device;
	structure->_heap          = (crgpu_heap)heap;
	if (!crgpu___internal_acceleration_structure_state_init(&structure->_state, &structure->head.allocator, desc)) {
		crgpumetal___rq_release(structure);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	if (desc->max_bottom_levels > 0) {
		structure->_bottom_levels_mtl = CR_NEW_ARRAY(&structure->head.allocator, id<MTLResource>, desc->max_bottom_levels);
		if (!structure->_bottom_levels_mtl) {
			crgpumetal___rq_release(structure);
			return CR_RESULT_OUT_OF_MEMORY;
		}
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
			crgpumetal___rq_release(structure);
			return CR_RESULT_GPU_PLACEMENT_MISALIGNED;
		}
		if ((uint64_t)need.size > covered) {
			crgpumetal___rq_release(structure);
			return CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE;
		}
		structure->_structure_mtl = [heap->_heap_mtl newAccelerationStructureWithSize:(NSUInteger)desc->size offset:(NSUInteger)offset];
	} else {
		structure->_structure_mtl = [device->_device_mtl newAccelerationStructureWithSize:(NSUInteger)desc->size];
	}
	if (!structure->_structure_mtl) {
		crgpumetal___rq_release(structure);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	structure->_resource_id = structure->_structure_mtl.gpuResourceID._impl;
	if (cr_debug_name_set(structure, desc->debug_name) != CR_SUCCESS) {
		crgpumetal___rq_release(structure);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	if (structure->head.debug_name) {
		// Allocated and released here rather than autoreleased: a caller in
		// plain C may have no autorelease pool.
		NSString *label = [[NSString alloc] initWithUTF8String:structure->head.debug_name];
		structure->_structure_mtl.label = label;
		[label release];
	}
	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_ACCELERATION_STRUCTURE);
#if _DEBUG
	if (heap)
		heap->_placed_count++;
#endif
	*out_structure = (crgpu_acceleration_structure)structure;
	return CR_SUCCESS;
}

CR_RESULT crgpu_device_get_acceleration_structure_build_sizes(
    crgpu_device device,
    const crgpu_acceleration_structure_inputs *inputs,
    crgpu_acceleration_structure_sizes *out_sizes) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_device_get_acceleration_structure_build_sizes", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!inputs)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT inputs_base_sr = cr_struct_check(&inputs->base, CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS);
	if (inputs_base_sr != CR_SUCCESS)
		return inputs_base_sr;
	if (!out_sizes)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT out_base_sr = cr_struct_out_check(&out_sizes->base, CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES);
	if (out_base_sr != CR_SUCCESS)
		return out_base_sr;
	crgpu_device_metal *device_metal = (crgpu_device_metal *)device;
	if (!(device_metal->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT inputs_sr = crgpu___internal_check_acceleration_structure_inputs(&device_metal->_adapter->_facts, inputs);
	if (inputs_sr != CR_SUCCESS)
		return inputs_sr;
	@autoreleasepool {
		crgpu_acceleration_structure_sizes sizes = *out_sizes;
		crgpumetal___rq_sizes(device_metal, inputs, crgpumetal___rq_new_descriptor(inputs), &sizes);
		*out_sizes = sizes;
	}
	return CR_SUCCESS;
}

CR_RESULT crgpu_acceleration_structure_create(
    crgpu_device device,
    const crgpu_acceleration_structure_desc *desc,
    crgpu_acceleration_structure *out_structure) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_acceleration_structure_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_structure)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	crgpu_device_metal *device_metal = (crgpu_device_metal *)device;
	if (!(device_metal->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT desc_sr = crgpu___internal_check_acceleration_structure_desc(desc);
	if (desc_sr != CR_SUCCESS)
		return desc_sr;
	return crgpumetal___rq_create(device_metal, NULL, 0, desc, out_structure);
}

CR_RESULT crgpu_acceleration_structure_create_placed(
    crgpu_heap heap,
    uint64_t offset,
    const crgpu_acceleration_structure_desc *desc,
    crgpu_acceleration_structure *out_structure) {
	if (!heap)
		return CR_RESULT_GPU_INVALID_HEAP;
	if (CR_STALE(heap, "crgpu_acceleration_structure_create_placed", "heap"))
		return CR_RESULT_GPU_INVALID_HEAP;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_structure)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	crgpu_heap_metal *heap_metal     = (crgpu_heap_metal *)heap;
	crgpu_device_metal *device_metal = heap_metal->_parent_device;
	if (!(device_metal->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT desc_sr = crgpu___internal_check_acceleration_structure_desc(desc);
	if (desc_sr != CR_SUCCESS)
		return desc_sr;
	if (heap_metal->_desc.resource_class != CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES)
		return CR_RESULT_GPU_HEAP_MISMATCH;
	const uint64_t alignment = device_metal->_adapter->_facts.ray_query_placement_alignment;
	if (offset % alignment != 0)
		return CR_RESULT_GPU_PLACEMENT_MISALIGNED;
	// The size rounded up to the alignment, from the offset, inside the
	// heap, with nothing that can wrap.
	if (offset > heap_metal->_desc.size)
		return CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	const uint64_t room = heap_metal->_desc.size - offset;
	if (desc->size > room || (desc->size + (alignment - desc->size % alignment) % alignment) > room)
		return CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	return crgpumetal___rq_create(device_metal, heap_metal, offset, desc, out_structure);
}

void crgpu_acceleration_structure_destroy(crgpu_acceleration_structure structure) {
	if (!structure)
		return;
	if (CR_STALE(structure, "crgpu_acceleration_structure_destroy", "structure"))
		return;
	crgpu_acceleration_structure_metal *structure_metal = (crgpu_acceleration_structure_metal *)structure;
#if _DEBUG
	if (structure_metal->_heap)
		((crgpu_heap_metal *)structure_metal->_heap)->_placed_count--;
#endif
	CRGPU___LIVE_REMOVE(&structure_metal->_parent_device->_live, CRGPU___OBJECT_ACCELERATION_STRUCTURE);
	CR_KILL(structure);
	crgpumetal___rq_release(structure_metal);
}

CR_RESULT crgpu_acceleration_structure_get_reference(
    crgpu_acceleration_structure bottom_level,
    uint64_t *out_reference) {
	if (!bottom_level)
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (CR_STALE(bottom_level, "crgpu_acceleration_structure_get_reference", "bottom_level"))
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	const crgpu_acceleration_structure_metal *structure = (const crgpu_acceleration_structure_metal *)bottom_level;
	if (structure->_state.level != CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM)
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (!out_reference)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_reference = structure->_resource_id;
	return CR_SUCCESS;
}

CR_RESULT crgpu_device_get_acceleration_structure_instance_layout(
    crgpu_device device,
    crgpu_acceleration_structure_instance_layout *out_layout) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_device_get_acceleration_structure_instance_layout", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!out_layout)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!(((crgpu_device_metal *)device)->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	*out_layout = crgpu___internal_instance_layout();
	return CR_SUCCESS;
}

CR_RESULT crgpu_device_write_acceleration_structure_instances(
    crgpu_device device,
    const crgpu_acceleration_structure_instance *instances,
    uint32_t count,
    void *out_bytes) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_device_write_acceleration_structure_instances", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!(((crgpu_device_metal *)device)->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	return crgpu___internal_write_instances(crgpu___internal_instance_layout(), instances, count, out_bytes);
}

CR_RESULT crgpu_command_list_build_acceleration_structures(
    crgpu_command_list cmd,
    const crgpu_acceleration_structure_build *builds,
    uint32_t count) {
	if (!cmd)
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	if (CR_STALE(cmd, "crgpu_command_list_build_acceleration_structures", "cmd"))
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	crgpu_device_metal *device          = cmd_metal->_parent_allocator->_parent_device;
	if (!(device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT list_sr = crgpu___internal_check_acceleration_structure_list(
	    cmd_metal->_parent_allocator->_desc.type, cmd_metal->_active_render_encoder != nil);
	if (list_sr != CR_SUCCESS)
		return list_sr;
	if (!builds)
		return CR_RESULT_NULL_DESC;
	if (count == 0)
		return CR_RESULT_ZERO_SIZE;
	const crgpu___adapter_facts *facts = &device->_adapter->_facts;

	@autoreleasepool {
		// Every build checked, in order, before anything is recorded, and
		// the destination's layout room made after its last check, so the
		// recording below can't fail.
		for (uint32_t b = 0; b < count; b++) {
			const crgpu_acceleration_structure_build *build = &builds[b];
			const CR_RESULT head_sr                         = crgpu___internal_check_build_head(facts, build);
			if (head_sr != CR_SUCCESS)
				return head_sr;
			MTLAccelerationStructureDescriptor *descriptor = crgpumetal___rq_list_descriptor(cmd_metal, build->inputs);
			if (!descriptor)
				return CR_RESULT_OUT_OF_MEMORY;
			crgpu_acceleration_structure_sizes sizes;
			memset(&sizes, 0, sizeof(sizes));
			crgpumetal___rq_sizes(device, build->inputs, descriptor, &sizes);
			const CR_RESULT tail_sr = crgpu___internal_check_build_tail(facts, build, &sizes);
			if (tail_sr != CR_SUCCESS)
				return tail_sr;
			const CR_RESULT layout_sr = crgpu___internal_reserve_build_layout(build->destination, build->inputs);
			if (layout_sr != CR_SUCCESS)
				return layout_sr;
		}

		id<MTLAccelerationStructureCommandEncoder> encoder = crgpumetal___rq_encoder(cmd_metal);
		for (uint32_t b = 0; b < count; b++) {
			const crgpu_acceleration_structure_build *build = &builds[b];
			const crgpu_acceleration_structure_inputs *inputs = build->inputs;
			crgpu_acceleration_structure_metal *destination_metal = (crgpu_acceleration_structure_metal *)build->destination;
			// A top level's bottom levels, kept as the build records them
			// (checked against max_bottom_levels above) and resident while it
			// is built.
			if (inputs->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP && inputs->bottom_level_count > 0) {
				for (uint32_t i = 0; i < inputs->bottom_level_count; i++)
					destination_metal->_bottom_levels_mtl[i] = ((crgpu_acceleration_structure_metal *)inputs->bottom_levels[i])->_structure_mtl;
				[encoder useResources:destination_metal->_bottom_levels_mtl count:inputs->bottom_level_count usage:MTLResourceUsageRead];
			}
			// Filled again, since a build checked after this one filled it
			// last: the checks made every descriptor it takes.
			MTLAccelerationStructureDescriptor *descriptor        = crgpumetal___rq_list_descriptor(cmd_metal, inputs);
			id<MTLAccelerationStructure> destination              = destination_metal->_structure_mtl;
			id<MTLBuffer> scratch                                 = ((crgpu_buffer_metal *)build->scratch)->_buffer_mtl;
			if (build->mode == CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE) {
				// In place when the source is the destination: Metal takes nil
				// as the destination for that.
				id<MTLAccelerationStructure> source = ((crgpu_acceleration_structure_metal *)build->source)->_structure_mtl;
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
			crgpu___internal_record_build(build);
		}
		[encoder endEncoding];
	}
	return CR_SUCCESS;
}

CR_RESULT crgpu_command_list_copy_acceleration_structure(
    crgpu_command_list cmd,
    crgpu_acceleration_structure source,
    crgpu_acceleration_structure destination,
    CRGPU_ACCELERATION_STRUCTURE_COPY_MODE mode) {
	if (!cmd)
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	if (CR_STALE(cmd, "crgpu_command_list_copy_acceleration_structure", "cmd"))
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	if (!(cmd_metal->_parent_allocator->_parent_device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT list_sr = crgpu___internal_check_acceleration_structure_list(
	    cmd_metal->_parent_allocator->_desc.type, cmd_metal->_active_render_encoder != nil);
	if (list_sr != CR_SUCCESS)
		return list_sr;
	const CR_RESULT sr = crgpu___internal_check_copy(source, destination, mode);
	if (sr != CR_SUCCESS)
		return sr;
	@autoreleasepool {
		const crgpu_acceleration_structure_metal *from = (const crgpu_acceleration_structure_metal *)source;
		crgpu_acceleration_structure_metal *to         = (crgpu_acceleration_structure_metal *)destination;
		id<MTLAccelerationStructureCommandEncoder> encoder = crgpumetal___rq_encoder(cmd_metal);
		// The copy names what its source's last build named (checked against
		// the destination's max_bottom_levels above), resident while copied.
		const uint32_t bottom_level_count = from->_state.bottom_level_count;
		if (bottom_level_count > 0) {
			memcpy(to->_bottom_levels_mtl, from->_bottom_levels_mtl, sizeof(id<MTLResource>) * bottom_level_count);
			[encoder useResources:from->_bottom_levels_mtl count:bottom_level_count usage:MTLResourceUsageRead];
		}
		if (mode == CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT)
			[encoder copyAndCompactAccelerationStructure:from->_structure_mtl toAccelerationStructure:to->_structure_mtl];
		else
			[encoder copyAccelerationStructure:from->_structure_mtl toAccelerationStructure:to->_structure_mtl];
		[encoder endEncoding];
	}
	crgpu___internal_record_copy(source, destination);
	return CR_SUCCESS;
}

CR_RESULT crgpu_command_list_write_acceleration_structure_compacted_sizes(
    crgpu_command_list cmd,
    const crgpu_acceleration_structure *structures,
    uint32_t count,
    crgpu_buffer buffer,
    uint64_t offset) {
	if (!cmd)
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	if (CR_STALE(cmd, "crgpu_command_list_write_acceleration_structure_compacted_sizes", "cmd"))
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	if (!(cmd_metal->_parent_allocator->_parent_device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT list_sr = crgpu___internal_check_acceleration_structure_list(
	    cmd_metal->_parent_allocator->_desc.type, cmd_metal->_active_render_encoder != nil);
	if (list_sr != CR_SUCCESS)
		return list_sr;
	const CR_RESULT sr = crgpu___internal_check_compacted_sizes(structures, count, buffer, offset);
	if (sr != CR_SUCCESS)
		return sr;
	@autoreleasepool {
		id<MTLAccelerationStructureCommandEncoder> encoder = crgpumetal___rq_encoder(cmd_metal);
		id<MTLBuffer> buffer_mtl                           = ((crgpu_buffer_metal *)buffer)->_buffer_mtl;
		for (uint32_t i = 0; i < count; i++)
			[encoder writeCompactedAccelerationStructureSize:((const crgpu_acceleration_structure_metal *)structures[i])->_structure_mtl
			                                        toBuffer:buffer_mtl
			                                          offset:(NSUInteger)(offset + (uint64_t)i * 8u)
			                                    sizeDataType:MTLDataTypeULong];
		[encoder endEncoding];
	}
	return CR_SUCCESS;
}

void crgpu_command_list_acceleration_structure_barrier(
    crgpu_command_list cmd,
    const crgpu_acceleration_structure_barrier *barriers,
    uint32_t count) {
	if (!cmd)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_acceleration_structure_barrier", "cmd"))
		return;
	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	if (!(cmd_metal->_parent_allocator->_parent_device->_features & CRGPU_FEATURE_RAY_QUERY))
		return;
	// The same checks as every backend, and then nothing: Metal tracks a
	// structure's hazards itself (see the top of this file).
	(void)crgpu___internal_check_acceleration_structure_barriers(barriers, count);
}

#endif // CRGPU_COMPILE_METAL_API
