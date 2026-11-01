//
// SpudGPU Metal backend - heaps and the buffers and images placed in them.
//
// A heap is an MTLHeap of MTLHeapTypePlacement, so each resource goes at
// the offset the caller gives, and a placed buffer or image is made with the
// heap's newBufferWithLength:options:offset: or
// newTextureWithDescriptor:offset:. The heap's storage and CPU cache modes
// are those of a buffer of its kind (spudgpumetal___buffer_resource_options).
//
// Hazard tracking. A heap is untracked unless asked, and its resources take
// the heap's mode. This backend's barriers do nothing
// (spudgpu_cmd_pipeline_barrier in spudgpumetalrenderpass.m): they rely on
// Metal tracking every resource. So every heap is made tracked, and its
// resources are then ordered by Metal as resources made on the device are.
// Metal tracks a heap's resources together, as far as is known: work on one
// placed resource may wait for work on another in the same heap that it has
// no dependency on. Correct, and possibly slower than it needs to be. An
// untracked heap needs real barriers in this backend first.
//
// Resources reached through an argument buffer are made resident with
// useResource:usage: per descriptor set (spudgpumetaldescriptors.m), which
// covers placed resources as it covers any other.
//

#if SPUDGPU_COMPILE_METAL_API

#include "spudgpu.h"
#include "spudcore.h"
#include "spudgpumetal.h"
#include <Metal/Metal.h>
#include <assert.h>
#include <stdlib.h>
#include "../../../spudstructbase.h"

// The checks spudgpu_get_buffer_placement_requirements and
// spudgpu_create_placed_buffer make after their argument checks.
static SPUDRESULT spudgpumetal___internal_check_buffer_placement(
    spudgpu_heap_metal *heap,
    const spudgpu_buffer_desc *desc) {
	const SPUDRESULT sr = spudgpumetal___check_buffer_desc(heap->_parent_device->_features, desc);
	if (sr != SPUD_SUCCESS)
		return sr;
	if (heap->_desc.resource_class != SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
	    desc->memory_kind != heap->_desc.memory_kind)
		return SPUDRESULT_GPU_HEAP_MISMATCH;
	return SPUD_SUCCESS;
}

// The checks spudgpu_get_image_placement_requirements and
// spudgpu_create_placed_image make after their argument checks.
static SPUDRESULT spudgpumetal___internal_check_image_placement(
    spudgpu_heap_metal *heap,
    const spudgpu_image_desc *desc) {
	const SPUDRESULT sr = spudgpumetal___check_image_desc(desc);
	if (sr != SPUD_SUCCESS)
		return sr;
	if (desc->usage & (SPUDGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT | SPUDGPU_IMAGE_USAGE_PRESENTABLE))
		return SPUDRESULT_GPU_INVALID_IMAGE_USAGE;
	const SPUDGPU_HEAP_RESOURCE_CLASS image_class =
	    (desc->usage & (SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT | SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT))
	        ? SPUDGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES
	        : SPUDGPU_HEAP_RESOURCE_CLASS_IMAGES;
	if (heap->_desc.resource_class != image_class)
		return SPUDRESULT_GPU_HEAP_MISMATCH;
	return SPUD_SUCCESS;
}

// The texture descriptor of an image of [desc] placed in a heap, which the
// caller releases: the one spudgpu_create_image uses, tracked as the heap
// is.
static MTLTextureDescriptor *spudgpumetal___internal_new_placed_texture_descriptor(const spudgpu_image_desc *desc) {
	MTLTextureDescriptor *texture_desc = spudgpumetal___new_texture_descriptor(desc);
	texture_desc.hazardTrackingMode    = MTLHazardTrackingModeTracked;
	return texture_desc;
}

// Metal has no error for a size query it can't answer: a size or alignment
// of 0 is treated as one.
static SPUDRESULT spudgpumetal___internal_placement_requirements(
    MTLSizeAndAlign size_and_align,
    spudgpu_placement_requirements *out_requirements) {
	if (size_and_align.size == 0 || size_and_align.align == 0)
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	out_requirements->size      = (uint64_t)size_and_align.size;
	out_requirements->alignment = (uint64_t)size_and_align.align;
	return SPUD_SUCCESS;
}

// The checks after the requirements are known, shared by both placed
// creates.
static SPUDRESULT spudgpumetal___internal_check_placement(
    spudgpu_heap_metal *heap,
    uint64_t offset,
    const spudgpu_placement_requirements *placement) {
	if (offset % placement->alignment)
		return SPUDRESULT_GPU_PLACEMENT_MISALIGNED;
	if (offset > heap->_desc.size || placement->size > heap->_desc.size - offset)
		return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_create_heap(
    spudgpu_device device,
    const spudgpu_heap_desc *desc,
    spudgpu_heap *out_heap) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_heap", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_HEAP_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_heap)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (desc->size == 0)
		return SPUDRESULT_ZERO_SIZE;
	if (desc->size % SPUDGPU_HEAP_SIZE_GRANULARITY)
		return SPUDRESULT_GPU_INVALID_HEAP_SIZE;
	if (desc->resource_class < SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
	    desc->resource_class > SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES)
		return SPUDRESULT_GPU_INVALID_HEAP_RESOURCE_CLASS;
	if (desc->memory_kind < SPUDGPU_MEMORY_KIND_DEVICE || desc->memory_kind > SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE)
		return SPUDRESULT_GPU_INVALID_MEMORY_KIND;
	if (desc->resource_class != SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS && desc->memory_kind != SPUDGPU_MEMORY_KIND_DEVICE)
		return SPUDRESULT_GPU_INVALID_MEMORY_KIND;

	spudgpu_device_metal *device_metal = (spudgpu_device_metal *)device;
	if (desc->memory_kind != SPUDGPU_MEMORY_KIND_DEVICE && !(device_metal->_features & SPUDGPU_FEATURE_MAPPABLE_HEAPS))
		return SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
	if (desc->memory_kind == SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE && !(device_metal->_features & SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY))
		return SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
	if (desc->resource_class == SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES && !(device_metal->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;

	spudgpu_heap_metal *heap = SPUD_NEW_ZEROED(&device_metal->head.allocator, spudgpu_heap_metal);
	if (!heap)
		return SPUDRESULT_OUT_OF_MEMORY;
	heap->head.allocator        = device_metal->head.allocator;
	SPUD_BIRTH(heap);
	heap->_parent_device        = device_metal;
	heap->_desc                 = *desc;
	heap->_resource_options_mtl =
	    spudgpumetal___buffer_resource_options(desc->memory_kind) | MTLResourceHazardTrackingModeTracked;

	{
		MTLHeapDescriptor *heap_desc = [[MTLHeapDescriptor alloc] init];
		heap_desc.type               = MTLHeapTypePlacement;
		heap_desc.size               = (NSUInteger)desc->size;
		// Storage mode, CPU cache mode and hazard tracking together.
		heap_desc.resourceOptions = heap->_resource_options_mtl;
		heap->_heap_mtl           = [device_metal->_device_mtl newHeapWithDescriptor:heap_desc];
		[heap_desc release];
	}
	if (!heap->_heap_mtl) {
		SPUD_DELETE(&device_metal->head.allocator, spudgpu_heap_metal, heap);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	SPUDGPU___LIVE_ADD(&device_metal->_live, SPUDGPU___OBJECT_HEAP);

	if (spud_debug_name_set(heap, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_heap((spudgpu_heap)heap);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	heap->_desc.debug_name = heap->head.debug_name;

	*out_heap = (spudgpu_heap)heap;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_heap(spudgpu_heap heap) {
	spudgpu_heap_metal *heap_metal = (spudgpu_heap_metal *)heap;
	if (!heap_metal)
		return;
	if (SPUD_STALE(heap, "spudgpu_destroy_heap", "heap"))
		return;
#if _DEBUG
	// Every buffer and image placed in the heap is destroyed first (see
	// spudgpu.h).
	assert(heap_metal->_placed_count == 0);
#endif
	SPUDGPU___LIVE_REMOVE(&heap_metal->_parent_device->_live, SPUDGPU___OBJECT_HEAP);
	SPUD_KILL(heap);
	const SPUD_ALLOCATOR allocator = heap_metal->head.allocator;
	spud_string_free(&allocator, heap_metal->head.debug_name);
	[heap_metal->_heap_mtl release];
	SPUD_DELETE(&allocator, spudgpu_heap_metal, heap_metal);
}

SPUDRESULT spudgpu_get_heap_desc(
    spudgpu_heap heap,
    spudgpu_heap_desc *out_desc) {
	if (!heap)
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (SPUD_STALE(heap, "spudgpu_get_heap_desc", "heap"))
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (!out_desc)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = ((spudgpu_heap_metal *)heap)->_desc;
	out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_HEAP_DESC);
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_buffer_placement_requirements(
    spudgpu_heap heap,
    const spudgpu_buffer_desc *desc,
    spudgpu_placement_requirements *out_requirements) {
	if (!heap)
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (SPUD_STALE(heap, "spudgpu_get_buffer_placement_requirements", "heap"))
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT desc_base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_BUFFER_DESC);
	if (desc_base_sr != SPUD_SUCCESS)
		return desc_base_sr;
	if (!out_requirements)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT out_requirements_base_sr = spud_struct_out_check(&out_requirements->base, SPUD_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS);
	if (out_requirements_base_sr != SPUD_SUCCESS)
		return out_requirements_base_sr;
	spudgpu_heap_metal *heap_metal = (spudgpu_heap_metal *)heap;
	const SPUDRESULT sr            = spudgpumetal___internal_check_buffer_placement(heap_metal, desc);
	if (sr != SPUD_SUCCESS)
		return sr;
	return spudgpumetal___internal_placement_requirements(
	    [heap_metal->_parent_device->_device_mtl heapBufferSizeAndAlignWithLength:(NSUInteger)desc->size
	                                                                       options:heap_metal->_resource_options_mtl],
	    out_requirements);
}

SPUDRESULT spudgpu_get_image_placement_requirements(
    spudgpu_heap heap,
    const spudgpu_image_desc *desc,
    spudgpu_placement_requirements *out_requirements) {
	if (!heap)
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (SPUD_STALE(heap, "spudgpu_get_image_placement_requirements", "heap"))
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT desc_base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_IMAGE_DESC);
	if (desc_base_sr != SPUD_SUCCESS)
		return desc_base_sr;
	if (!out_requirements)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT out_requirements_base_sr = spud_struct_out_check(&out_requirements->base, SPUD_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS);
	if (out_requirements_base_sr != SPUD_SUCCESS)
		return out_requirements_base_sr;
	spudgpu_heap_metal *heap_metal = (spudgpu_heap_metal *)heap;
	const SPUDRESULT sr            = spudgpumetal___internal_check_image_placement(heap_metal, desc);
	if (sr != SPUD_SUCCESS)
		return sr;
	MTLTextureDescriptor *texture_desc = spudgpumetal___internal_new_placed_texture_descriptor(desc);
	const SPUDRESULT query_result      = spudgpumetal___internal_placement_requirements(
	    [heap_metal->_parent_device->_device_mtl heapTextureSizeAndAlignWithDescriptor:texture_desc],
	    out_requirements);
	[texture_desc release];
	return query_result;
}

SPUDRESULT spudgpu_create_placed_buffer(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_buffer_desc *desc,
    spudgpu_buffer *out_buffer) {
	if (!heap)
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (SPUD_STALE(heap, "spudgpu_create_placed_buffer", "heap"))
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_BUFFER_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_buffer)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	spudgpu_heap_metal *heap_metal = (spudgpu_heap_metal *)heap;
	SPUDRESULT sr                  = spudgpumetal___internal_check_buffer_placement(heap_metal, desc);
	if (sr != SPUD_SUCCESS)
		return sr;

	spudgpu_placement_requirements placement;
	sr = spudgpumetal___internal_placement_requirements(
	    [heap_metal->_parent_device->_device_mtl heapBufferSizeAndAlignWithLength:(NSUInteger)desc->size
	                                                                       options:heap_metal->_resource_options_mtl],
	    &placement);
	if (sr != SPUD_SUCCESS)
		return sr;
	sr = spudgpumetal___internal_check_placement(heap_metal, offset, &placement);
	if (sr != SPUD_SUCCESS)
		return sr;

	spudgpu_buffer_metal *buffer_metal = SPUD_NEW_ZEROED(&heap_metal->head.allocator, spudgpu_buffer_metal);
	if (!buffer_metal)
		return SPUDRESULT_OUT_OF_MEMORY;
	buffer_metal->head.allocator = heap_metal->head.allocator;
	SPUD_BIRTH(buffer_metal);
	buffer_metal->_parent_device = heap_metal->_parent_device;
	buffer_metal->_desc          = *desc;
	buffer_metal->_buffer_mtl    = [heap_metal->_heap_mtl newBufferWithLength:(NSUInteger)desc->size
	                                                                   options:heap_metal->_resource_options_mtl
	                                                                    offset:(NSUInteger)offset];
	if (!buffer_metal->_buffer_mtl) {
		SPUD_DELETE(&heap_metal->head.allocator, spudgpu_buffer_metal, buffer_metal);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	buffer_metal->_heap = heap;
	SPUDGPU___LIVE_ADD(&buffer_metal->_parent_device->_live, SPUDGPU___OBJECT_BUFFER);

#if _DEBUG
	// Counted before the name is copied: a failed copy destroys the buffer,
	// which takes it off the count again.
	heap_metal->_placed_count++;
#endif
	if (spud_debug_name_set(buffer_metal, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_buffer((spudgpu_buffer)buffer_metal);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	buffer_metal->_desc.debug_name = buffer_metal->head.debug_name;

	*out_buffer = (spudgpu_buffer)buffer_metal;
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_create_placed_image(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_image_desc *desc,
    spudgpu_image *out_image) {
	if (!heap)
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (SPUD_STALE(heap, "spudgpu_create_placed_image", "heap"))
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_IMAGE_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_image)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	spudgpu_heap_metal *heap_metal = (spudgpu_heap_metal *)heap;
	SPUDRESULT sr                  = spudgpumetal___internal_check_image_placement(heap_metal, desc);
	if (sr != SPUD_SUCCESS)
		return sr;

	MTLTextureDescriptor *texture_desc = spudgpumetal___internal_new_placed_texture_descriptor(desc);
	spudgpu_placement_requirements placement;
	sr = spudgpumetal___internal_placement_requirements(
	    [heap_metal->_parent_device->_device_mtl heapTextureSizeAndAlignWithDescriptor:texture_desc],
	    &placement);
	if (sr == SPUD_SUCCESS)
		sr = spudgpumetal___internal_check_placement(heap_metal, offset, &placement);
	if (sr != SPUD_SUCCESS) {
		[texture_desc release];
		return sr;
	}

	spudgpu_image_metal *image_metal = SPUD_NEW_ZEROED(&heap_metal->head.allocator, spudgpu_image_metal);
	if (!image_metal) {
		[texture_desc release];
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	image_metal->head.allocator = heap_metal->head.allocator;
	SPUD_BIRTH(image_metal);
	image_metal->_parent_device = heap_metal->_parent_device;
	image_metal->_desc          = *desc;
	image_metal->_texture_mtl   = [heap_metal->_heap_mtl newTextureWithDescriptor:texture_desc offset:(NSUInteger)offset];
	[texture_desc release];
	if (!image_metal->_texture_mtl) {
		SPUD_DELETE(&heap_metal->head.allocator, spudgpu_image_metal, image_metal);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	image_metal->_heap = heap;
	SPUDGPU___LIVE_ADD(&image_metal->_parent_device->_live, SPUDGPU___OBJECT_IMAGE);

#if _DEBUG
	heap_metal->_placed_count++;
#endif
	if (spud_debug_name_set(image_metal, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_image((spudgpu_image)image_metal);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	image_metal->_desc.debug_name = image_metal->head.debug_name;

	*out_image = (spudgpu_image)image_metal;
	return SPUD_SUCCESS;
}

#endif // SPUDGPU_COMPILE_METAL_API
