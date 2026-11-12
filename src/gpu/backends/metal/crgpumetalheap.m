//
// Carrottus GPU Metal backend - heaps and the buffers and images placed in them.
//
// A heap is an MTLHeap of MTLHeapTypePlacement, so each resource goes at
// the offset the caller gives, and a placed buffer or image is made with the
// heap's newBufferWithLength:options:offset: or
// newTextureWithDescriptor:offset:. The heap's storage and CPU cache modes
// are those of a buffer of its kind (crgpumetal___buffer_resource_options).
//
// Hazard tracking. A heap is untracked unless asked, and its resources take
// the heap's mode. This backend's barriers do nothing
// (crgpu_command_list_pipeline_barrier in crgpumetalrenderpass.m): they rely on
// Metal tracking every resource. So every heap is made tracked, and its
// resources are then ordered by Metal as resources made on the device are.
// Metal tracks a heap's resources together, as far as is known: work on one
// placed resource may wait for work on another in the same heap that it has
// no dependency on. Correct, and possibly slower than it needs to be. An
// untracked heap needs real barriers in this backend first.
//
// Resources reached through an argument buffer are made resident with
// useResource:usage: per descriptor set (crgpumetaldescriptors.m), which
// covers placed resources as it covers any other.
//

#if CRGPU_COMPILE_METAL_API

#include "crgpu.h"
#include "crcore.h"
#include "crgpumetal.h"
#include <Metal/Metal.h>
#include <assert.h>
#include <stdlib.h>
#include "../../../crstructbase.h"

// The checks crgpu_heap_get_buffer_placement_requirements and
// crgpu_buffer_create_placed make after their argument checks.
static CR_RESULT crgpumetal___internal_check_buffer_placement(
    crgpu_heap_metal *heap,
    const crgpu_buffer_desc *desc) {
	const CR_RESULT sr = crgpumetal___check_buffer_desc(heap->_parent_device->_features, desc);
	if (sr != CR_SUCCESS)
		return sr;
	if (heap->_desc.resource_class != CRGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
	    desc->memory_kind != heap->_desc.memory_kind)
		return CR_RESULT_GPU_HEAP_MISMATCH;
	return CR_SUCCESS;
}

// The checks crgpu_heap_get_image_placement_requirements and
// crgpu_image_create_placed make after their argument checks.
static CR_RESULT crgpumetal___internal_check_image_placement(
    crgpu_heap_metal *heap,
    const crgpu_image_desc *desc) {
	const CR_RESULT sr = crgpumetal___check_image_desc(desc);
	if (sr != CR_SUCCESS)
		return sr;
	if (desc->usage & (CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT | CRGPU_IMAGE_USAGE_PRESENTABLE))
		return CR_RESULT_GPU_INVALID_IMAGE_USAGE;
	const CRGPU_HEAP_RESOURCE_CLASS image_class =
	    (desc->usage & (CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT))
	        ? CRGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES
	        : CRGPU_HEAP_RESOURCE_CLASS_IMAGES;
	if (heap->_desc.resource_class != image_class)
		return CR_RESULT_GPU_HEAP_MISMATCH;
	return CR_SUCCESS;
}

// The texture descriptor of an image of [desc] placed in a heap, which the
// caller releases: the one crgpu_image_create uses, tracked as the heap
// is.
static MTLTextureDescriptor *crgpumetal___internal_new_placed_texture_descriptor(const crgpu_image_desc *desc) {
	MTLTextureDescriptor *texture_desc = crgpumetal___new_texture_descriptor(desc);
	texture_desc.hazardTrackingMode    = MTLHazardTrackingModeTracked;
	return texture_desc;
}

// Metal has no error for a size query it can't answer: a size or alignment
// of 0 is treated as one.
static CR_RESULT crgpumetal___internal_placement_requirements(
    MTLSizeAndAlign size_and_align,
    crgpu_placement_requirements *out_requirements) {
	if (size_and_align.size == 0 || size_and_align.align == 0)
		return CR_RESULT_API_SPECIFIC_FAILURE;
	out_requirements->size      = (uint64_t)size_and_align.size;
	out_requirements->alignment = (uint64_t)size_and_align.align;
	return CR_SUCCESS;
}

// The checks after the requirements are known, shared by both placed
// creates.
static CR_RESULT crgpumetal___internal_check_placement(
    crgpu_heap_metal *heap,
    uint64_t offset,
    const crgpu_placement_requirements *placement) {
	if (offset % placement->alignment)
		return CR_RESULT_GPU_PLACEMENT_MISALIGNED;
	if (offset > heap->_desc.size || placement->size > heap->_desc.size - offset)
		return CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	return CR_SUCCESS;
}

CR_RESULT crgpu_heap_create(
    crgpu_device device,
    const crgpu_heap_desc *desc,
    crgpu_heap *out_heap) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_heap_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_HEAP_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_heap)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (desc->size == 0)
		return CR_RESULT_ZERO_SIZE;
	if (desc->size % CRGPU_HEAP_SIZE_GRANULARITY)
		return CR_RESULT_GPU_INVALID_HEAP_SIZE;
	if (desc->resource_class < CRGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
	    desc->resource_class > CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES)
		return CR_RESULT_GPU_INVALID_HEAP_RESOURCE_CLASS;
	if (desc->memory_kind < CRGPU_MEMORY_KIND_DEVICE || desc->memory_kind > CRGPU_MEMORY_KIND_DEVICE_MAPPABLE)
		return CR_RESULT_GPU_INVALID_MEMORY_KIND;
	if (desc->resource_class != CRGPU_HEAP_RESOURCE_CLASS_BUFFERS && desc->memory_kind != CRGPU_MEMORY_KIND_DEVICE)
		return CR_RESULT_GPU_INVALID_MEMORY_KIND;

	crgpu_device_metal *device_metal = (crgpu_device_metal *)device;
	if (desc->memory_kind != CRGPU_MEMORY_KIND_DEVICE && !(device_metal->_features & CRGPU_FEATURE_MAPPABLE_HEAPS))
		return CR_RESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
	if (desc->memory_kind == CRGPU_MEMORY_KIND_DEVICE_MAPPABLE && !(device_metal->_features & CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY))
		return CR_RESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
	if (desc->resource_class == CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES && !(device_metal->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;

	crgpu_heap_metal *heap = CR_NEW_ZEROED(&device_metal->head.allocator, crgpu_heap_metal);
	if (!heap)
		return CR_RESULT_OUT_OF_MEMORY;
	heap->head.allocator        = device_metal->head.allocator;
	CR_BIRTH_IN(heap, &device_metal->_quarantine);
	heap->_parent_device        = device_metal;
	heap->_desc                 = *desc;
	heap->_resource_options_mtl =
	    crgpumetal___buffer_resource_options(desc->memory_kind) | MTLResourceHazardTrackingModeTracked;

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
		CR_DELETE(&device_metal->head.allocator, crgpu_heap_metal, heap);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	CRGPU___LIVE_ADD(&device_metal->_live, CRGPU___OBJECT_HEAP);

	if (cr_debug_name_set(heap, desc->debug_name) != CR_SUCCESS) {
		crgpu_heap_destroy((crgpu_heap)heap);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	heap->_desc.debug_name = heap->head.debug_name;

	*out_heap = (crgpu_heap)heap;
	return CR_SUCCESS;
}

void crgpu_heap_destroy(crgpu_heap heap) {
	crgpu_heap_metal *heap_metal = (crgpu_heap_metal *)heap;
	if (!heap_metal)
		return;
	if (CR_STALE(heap, "crgpu_heap_destroy", "heap"))
		return;
#if _DEBUG
	// Every buffer and image placed in the heap is destroyed first (see
	// crgpu.h).
	assert(heap_metal->_placed_count == 0);
#endif
	CRGPU___LIVE_REMOVE(&heap_metal->_parent_device->_live, CRGPU___OBJECT_HEAP);
	CR_KILL(heap);
	const CR_ALLOCATOR allocator = heap_metal->head.allocator;
	cr_string_free(&allocator, heap_metal->head.debug_name);
	[heap_metal->_heap_mtl release];
	CR_RETIRE(CR_QUARANTINE_OF(heap_metal), &allocator, crgpu_heap_metal, heap_metal);
}

CR_RESULT crgpu_heap_get_desc(
    crgpu_heap heap,
    crgpu_heap_desc *out_desc) {
	if (!heap)
		return CR_RESULT_GPU_INVALID_HEAP;
	if (CR_STALE(heap, "crgpu_heap_get_desc", "heap"))
		return CR_RESULT_GPU_INVALID_HEAP;
	if (!out_desc)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = ((crgpu_heap_metal *)heap)->_desc;
	out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_HEAP_DESC);
	return CR_SUCCESS;
}

CR_RESULT crgpu_heap_get_buffer_placement_requirements(
    crgpu_heap heap,
    const crgpu_buffer_desc *desc,
    crgpu_placement_requirements *out_requirements) {
	if (!heap)
		return CR_RESULT_GPU_INVALID_HEAP;
	if (CR_STALE(heap, "crgpu_heap_get_buffer_placement_requirements", "heap"))
		return CR_RESULT_GPU_INVALID_HEAP;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT desc_base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_BUFFER_DESC);
	if (desc_base_sr != CR_SUCCESS)
		return desc_base_sr;
	if (!out_requirements)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT out_requirements_base_sr = cr_struct_out_check(&out_requirements->base, CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS);
	if (out_requirements_base_sr != CR_SUCCESS)
		return out_requirements_base_sr;
	crgpu_heap_metal *heap_metal = (crgpu_heap_metal *)heap;
	const CR_RESULT sr            = crgpumetal___internal_check_buffer_placement(heap_metal, desc);
	if (sr != CR_SUCCESS)
		return sr;
	return crgpumetal___internal_placement_requirements(
	    [heap_metal->_parent_device->_device_mtl heapBufferSizeAndAlignWithLength:(NSUInteger)desc->size
	                                                                       options:heap_metal->_resource_options_mtl],
	    out_requirements);
}

CR_RESULT crgpu_heap_get_image_placement_requirements(
    crgpu_heap heap,
    const crgpu_image_desc *desc,
    crgpu_placement_requirements *out_requirements) {
	if (!heap)
		return CR_RESULT_GPU_INVALID_HEAP;
	if (CR_STALE(heap, "crgpu_heap_get_image_placement_requirements", "heap"))
		return CR_RESULT_GPU_INVALID_HEAP;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT desc_base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_IMAGE_DESC);
	if (desc_base_sr != CR_SUCCESS)
		return desc_base_sr;
	if (!out_requirements)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT out_requirements_base_sr = cr_struct_out_check(&out_requirements->base, CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS);
	if (out_requirements_base_sr != CR_SUCCESS)
		return out_requirements_base_sr;
	crgpu_heap_metal *heap_metal = (crgpu_heap_metal *)heap;
	const CR_RESULT sr            = crgpumetal___internal_check_image_placement(heap_metal, desc);
	if (sr != CR_SUCCESS)
		return sr;
	MTLTextureDescriptor *texture_desc = crgpumetal___internal_new_placed_texture_descriptor(desc);
	const CR_RESULT query_result      = crgpumetal___internal_placement_requirements(
	    [heap_metal->_parent_device->_device_mtl heapTextureSizeAndAlignWithDescriptor:texture_desc],
	    out_requirements);
	[texture_desc release];
	return query_result;
}

CR_RESULT crgpu_buffer_create_placed(
    crgpu_heap heap,
    uint64_t offset,
    const crgpu_buffer_desc *desc,
    crgpu_buffer *out_buffer) {
	if (!heap)
		return CR_RESULT_GPU_INVALID_HEAP;
	if (CR_STALE(heap, "crgpu_buffer_create_placed", "heap"))
		return CR_RESULT_GPU_INVALID_HEAP;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_BUFFER_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_buffer)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	crgpu_heap_metal *heap_metal = (crgpu_heap_metal *)heap;
	CR_RESULT sr                  = crgpumetal___internal_check_buffer_placement(heap_metal, desc);
	if (sr != CR_SUCCESS)
		return sr;

	crgpu_placement_requirements placement;
	sr = crgpumetal___internal_placement_requirements(
	    [heap_metal->_parent_device->_device_mtl heapBufferSizeAndAlignWithLength:(NSUInteger)desc->size
	                                                                       options:heap_metal->_resource_options_mtl],
	    &placement);
	if (sr != CR_SUCCESS)
		return sr;
	sr = crgpumetal___internal_check_placement(heap_metal, offset, &placement);
	if (sr != CR_SUCCESS)
		return sr;

	crgpu_buffer_metal *buffer_metal = CR_NEW_ZEROED(&heap_metal->head.allocator, crgpu_buffer_metal);
	if (!buffer_metal)
		return CR_RESULT_OUT_OF_MEMORY;
	buffer_metal->head.allocator = heap_metal->head.allocator;
	CR_BIRTH_IN(buffer_metal, &heap_metal->_parent_device->_quarantine);
	buffer_metal->_parent_device = heap_metal->_parent_device;
	buffer_metal->_desc          = *desc;
	buffer_metal->_buffer_mtl    = [heap_metal->_heap_mtl newBufferWithLength:(NSUInteger)desc->size
	                                                                   options:heap_metal->_resource_options_mtl
	                                                                    offset:(NSUInteger)offset];
	if (!buffer_metal->_buffer_mtl) {
		CR_DELETE(&heap_metal->head.allocator, crgpu_buffer_metal, buffer_metal);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	buffer_metal->_heap = heap;
	CRGPU___LIVE_ADD(&buffer_metal->_parent_device->_live, CRGPU___OBJECT_BUFFER);

#if _DEBUG
	// Counted before the name is copied: a failed copy destroys the buffer,
	// which takes it off the count again.
	heap_metal->_placed_count++;
#endif
	if (cr_debug_name_set(buffer_metal, desc->debug_name) != CR_SUCCESS) {
		crgpu_buffer_destroy((crgpu_buffer)buffer_metal);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	buffer_metal->_desc.debug_name = buffer_metal->head.debug_name;

	*out_buffer = (crgpu_buffer)buffer_metal;
	return CR_SUCCESS;
}

CR_RESULT crgpu_image_create_placed(
    crgpu_heap heap,
    uint64_t offset,
    const crgpu_image_desc *desc,
    crgpu_image *out_image) {
	if (!heap)
		return CR_RESULT_GPU_INVALID_HEAP;
	if (CR_STALE(heap, "crgpu_image_create_placed", "heap"))
		return CR_RESULT_GPU_INVALID_HEAP;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_IMAGE_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_image)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	crgpu_heap_metal *heap_metal = (crgpu_heap_metal *)heap;
	CR_RESULT sr                  = crgpumetal___internal_check_image_placement(heap_metal, desc);
	if (sr != CR_SUCCESS)
		return sr;

	MTLTextureDescriptor *texture_desc = crgpumetal___internal_new_placed_texture_descriptor(desc);
	crgpu_placement_requirements placement;
	sr = crgpumetal___internal_placement_requirements(
	    [heap_metal->_parent_device->_device_mtl heapTextureSizeAndAlignWithDescriptor:texture_desc],
	    &placement);
	if (sr == CR_SUCCESS)
		sr = crgpumetal___internal_check_placement(heap_metal, offset, &placement);
	if (sr != CR_SUCCESS) {
		[texture_desc release];
		return sr;
	}

	crgpu_image_metal *image_metal = CR_NEW_ZEROED(&heap_metal->head.allocator, crgpu_image_metal);
	if (!image_metal) {
		[texture_desc release];
		return CR_RESULT_OUT_OF_MEMORY;
	}
	image_metal->head.allocator = heap_metal->head.allocator;
	CR_BIRTH_IN(image_metal, &heap_metal->_parent_device->_quarantine);
	image_metal->_parent_device = heap_metal->_parent_device;
	image_metal->_desc          = *desc;
	image_metal->_texture_mtl   = [heap_metal->_heap_mtl newTextureWithDescriptor:texture_desc offset:(NSUInteger)offset];
	[texture_desc release];
	if (!image_metal->_texture_mtl) {
		CR_DELETE(&heap_metal->head.allocator, crgpu_image_metal, image_metal);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	image_metal->_heap = heap;
	CRGPU___LIVE_ADD(&image_metal->_parent_device->_live, CRGPU___OBJECT_IMAGE);

#if _DEBUG
	heap_metal->_placed_count++;
#endif
	if (cr_debug_name_set(image_metal, desc->debug_name) != CR_SUCCESS) {
		crgpu_image_destroy((crgpu_image)image_metal);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	image_metal->_desc.debug_name = image_metal->head.debug_name;

	*out_image = (crgpu_image)image_metal;
	return CR_SUCCESS;
}

#endif // CRGPU_COMPILE_METAL_API
