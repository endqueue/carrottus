
#if CRGPU_COMPILE_D3D12_API

#include "crgpud3d12.hpp"
#include <cassert>
#include "../../../crstructbase.h"

// Carrottus GPU heaps on D3D12.
//
// A heap is an ID3D12Heap, and a placed buffer or image is a resource made
// in it with CreatePlacedResource. The heap's properties are the ones a
// committed buffer of its kind gets (crgpu_d3d12_get_heap_properties_from_memory_kind),
// and its flags are those of every buffer and image
// (crgpu_d3d12_get_heap_flags) plus the ALLOW_ONLY_ flag of its class. The
// ALLOW_ONLY_ flag is what resource heap tier 1 requires; it is set on tier 2
// as well, so a heap holds one class on every device (see
// CRGPU_HEAP_RESOURCE_CLASS).
//
// A resource's size and alignment are GetResourceAllocationInfo's for the
// D3D12_RESOURCE_DESC the committed path builds, with D3D12 choosing the
// alignment: 64 KiB for every buffer and every texture. Textures could ask
// for D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT (4 KiB) when they are small
// enough, but asking for it for one that isn't is a debug layer error, and
// telling them apart needs each format's tile shape. Not done yet.
//
// D3D12 maps placed resources one by one, as it does committed ones, so
// crgpu_buffer_map is unchanged for a placed buffer.

static D3D12_HEAP_FLAGS crgpu_d3d12_heap_class_flags(CRGPU_HEAP_RESOURCE_CLASS resource_class) {
	switch (resource_class) {
	// An acceleration structure is a buffer (crgpud3d12raytracing.cpp).
	case CRGPU_HEAP_RESOURCE_CLASS_BUFFERS:
	case CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES:
		return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
	case CRGPU_HEAP_RESOURCE_CLASS_IMAGES:
		return D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
	default:
		return D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
	}
}

// The checks crgpu_heap_get_buffer_placement_requirements and
// crgpu_buffer_create_placed make after their argument checks.
static CR_RESULT crgpu_d3d12_check_buffer_placement(
    crgpu_heap heap,
    const crgpu_buffer_desc *desc) {
	const CR_RESULT sr = crgpu_d3d12_check_buffer_desc(heap->_device->_features, desc);
	if (sr != CR_SUCCESS)
		return sr;
	if (heap->_desc.resource_class != CRGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
	    desc->memory_kind != heap->_desc.memory_kind)
		return CR_RESULT_GPU_HEAP_MISMATCH;
	return CR_SUCCESS;
}

// The checks crgpu_heap_get_image_placement_requirements and
// crgpu_image_create_placed make after their argument checks.
static CR_RESULT crgpu_d3d12_check_image_placement(
    crgpu_heap heap,
    const crgpu_image_desc *desc) {
	const CR_RESULT sr = crgpu_d3d12_check_image_desc(desc);
	if (sr != CR_SUCCESS)
		return sr;
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

// The size and alignment of a resource of [d3d_desc].
static CR_RESULT crgpu_d3d12_placement_requirements(
    crgpu_heap heap,
    const D3D12_RESOURCE_DESC *d3d_desc,
    crgpu_placement_requirements *out_requirements) {
	const D3D12_RESOURCE_ALLOCATION_INFO info =
	    heap->_device->_d3d_device->GetResourceAllocationInfo(0, 1, d3d_desc);
	if (info.SizeInBytes == UINT64_MAX)
		return CR_RESULT_API_SPECIFIC_FAILURE;
	out_requirements->size      = info.SizeInBytes;
	out_requirements->alignment = info.Alignment;
	return CR_SUCCESS;
}

// The checks after the requirements are known, shared by both placed
// creates.
static CR_RESULT crgpu_d3d12_check_placement(
    crgpu_heap heap,
    uint64_t offset,
    const crgpu_placement_requirements *placement) {
	if (offset % placement->alignment)
		return CR_RESULT_GPU_PLACEMENT_MISALIGNED;
	if (offset > heap->_desc.size || placement->size > heap->_desc.size - offset)
		return CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	return CR_SUCCESS;
}

extern "C" {

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
	if (desc->memory_kind != CRGPU_MEMORY_KIND_DEVICE && !(device->_features & CRGPU_FEATURE_MAPPABLE_HEAPS))
		return CR_RESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
	if (desc->memory_kind == CRGPU_MEMORY_KIND_DEVICE_MAPPABLE && !(device->_features & CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY))
		return CR_RESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
	if (desc->resource_class == CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES && !(device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;

	crgpu_heap_d3d12 *heap = CR_NEW(&device->head.allocator, crgpu_heap_d3d12);
	if (!heap)
		return CR_RESULT_OUT_OF_MEMORY;
	heap                 = new (heap) crgpu_heap_d3d12();
	heap->head.allocator = device->head.allocator;
	CR_BIRTH_IN(heap, &device->_quarantine);
	heap->_device        = device;
	heap->_desc          = *desc;

	D3D12_HEAP_DESC d3dHeapDesc = {};
	d3dHeapDesc.SizeInBytes     = desc->size;
	d3dHeapDesc.Properties      = crgpu_d3d12_get_heap_properties_from_memory_kind(desc->memory_kind);
	d3dHeapDesc.Alignment       = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	d3dHeapDesc.Flags           = crgpu_d3d12_get_heap_flags() | crgpu_d3d12_heap_class_flags(desc->resource_class);
	if (FAILED(device->_d3d_device->CreateHeap(&d3dHeapDesc, IID_PPV_ARGS(&heap->_d3d_heap)))) {
		heap->~crgpu_heap_d3d12();
		CR_DELETE(&device->head.allocator, crgpu_heap_d3d12, heap);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_HEAP);
	if (cr_debug_name_set(heap, desc->debug_name) != CR_SUCCESS) {
		crgpu_heap_destroy(heap);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	heap->_desc.debug_name = heap->head.debug_name;

	*out_heap = heap;
	return CR_SUCCESS;
}

void crgpu_heap_destroy(crgpu_heap heap) {
	if (!heap)
		return;
	if (CR_STALE(heap, "crgpu_heap_destroy", "heap"))
		return;
#if _DEBUG
	// Every buffer and image placed in the heap is destroyed first (see
	// crgpu.h).
	assert(heap->_placed_count == 0);
#endif
	CRGPU___LIVE_REMOVE(&heap->_device->_live, CRGPU___OBJECT_HEAP);
	CR_KILL(heap);
	const CR_ALLOCATOR allocator = heap->head.allocator;
	cr_string_free(&allocator, heap->head.debug_name);
	CR_QUARANTINE_KEEP(quarantine, heap);
	heap->~crgpu_heap_d3d12();
	CR_RETIRE(quarantine, &allocator, crgpu_heap_d3d12, heap);
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
	*out_desc = heap->_desc;
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
	const CR_RESULT sr = crgpu_d3d12_check_buffer_placement(heap, desc);
	if (sr != CR_SUCCESS)
		return sr;
	const D3D12_RESOURCE_DESC d3dDesc = crgpu_d3d12_create_resource_desc_from_buffer(desc);
	return crgpu_d3d12_placement_requirements(heap, &d3dDesc, out_requirements);
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
	CR_RESULT sr = crgpu_d3d12_check_image_placement(heap, desc);
	if (sr != CR_SUCCESS)
		return sr;
	D3D12_RESOURCE_DESC d3dDesc = {};
	sr = crgpu_d3d12_create_resource_desc_from_image(desc, &d3dDesc);
	if (sr != CR_SUCCESS)
		return sr;
	return crgpu_d3d12_placement_requirements(heap, &d3dDesc, out_requirements);
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
	CR_RESULT sr = crgpu_d3d12_check_buffer_placement(heap, desc);
	if (sr != CR_SUCCESS)
		return sr;

	const D3D12_RESOURCE_DESC d3dDesc = crgpu_d3d12_create_resource_desc_from_buffer(desc);
	crgpu_placement_requirements placement;
	sr = crgpu_d3d12_placement_requirements(heap, &d3dDesc, &placement);
	if (sr != CR_SUCCESS)
		return sr;
	sr = crgpu_d3d12_check_placement(heap, offset, &placement);
	if (sr != CR_SUCCESS)
		return sr;

	crgpu_buffer_d3d12 *buffer = CR_NEW(&heap->_device->head.allocator, crgpu_buffer_d3d12);
	if (!buffer)
		return CR_RESULT_OUT_OF_MEMORY;
	buffer                     = new (buffer) crgpu_buffer_d3d12();
	buffer->head.allocator     = heap->_device->head.allocator;
	CR_BIRTH_IN(buffer, &heap->_device->_quarantine);
	buffer->_device            = heap->_device;
	buffer->_desc              = *desc;
	buffer->_d3d_resource_desc = d3dDesc;
	if (FAILED(heap->_device->_d3d_device->CreatePlacedResource(
	        heap->_d3d_heap.Get(), offset, &buffer->_d3d_resource_desc,
	        crgpu_d3d12_get_initial_buffer_state(desc->memory_kind), nullptr,
	        IID_PPV_ARGS(&buffer->_d3d_resource)))) {
		buffer->~crgpu_buffer_d3d12();
		CR_DELETE(&heap->_device->head.allocator, crgpu_buffer_d3d12, buffer);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	buffer->_d3d_gpu_address = buffer->_d3d_resource->GetGPUVirtualAddress();
	buffer->_heap                      = heap;
	CRGPU___LIVE_ADD(&heap->_device->_live, CRGPU___OBJECT_BUFFER);

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
	CR_RESULT sr = crgpu_d3d12_check_image_placement(heap, desc);
	if (sr != CR_SUCCESS)
		return sr;

	D3D12_RESOURCE_DESC d3dDesc = {};
	sr = crgpu_d3d12_create_resource_desc_from_image(desc, &d3dDesc);
	if (sr != CR_SUCCESS)
		return sr;
	crgpu_placement_requirements placement;
	sr = crgpu_d3d12_placement_requirements(heap, &d3dDesc, &placement);
	if (sr != CR_SUCCESS)
		return sr;
	sr = crgpu_d3d12_check_placement(heap, offset, &placement);
	if (sr != CR_SUCCESS)
		return sr;

	crgpu_image_d3d12 *image = CR_NEW(&heap->_device->head.allocator, crgpu_image_d3d12);
	if (!image)
		return CR_RESULT_OUT_OF_MEMORY;
	image                      = new (image) crgpu_image_d3d12();
	image->head.allocator      = heap->_device->head.allocator;
	CR_BIRTH_IN(image, &heap->_device->_quarantine);
	image->_subresource_states = eq::array<D3D12_RESOURCE_STATES>(&heap->_device->_eq_allocator);
	image->_device            = heap->_device;
	image->_desc              = *desc;
	image->_d3d_resource_desc = d3dDesc;

	// The initial state and optimized clear value of a committed image of
	// the same desc.
	const D3D12_RESOURCE_STATES initialState = crgpu_d3d12_get_initial_image_state(desc->usage);
	D3D12_CLEAR_VALUE clearValue             = {};
	const bool hasClearValue                 = crgpu_d3d12_get_image_clear_value(desc, &clearValue);
	if (FAILED(heap->_device->_d3d_device->CreatePlacedResource(
	        heap->_d3d_heap.Get(), offset, &image->_d3d_resource_desc,
	        initialState, hasClearValue ? &clearValue : nullptr,
	        IID_PPV_ARGS(&image->_d3d_resource)))) {
		image->~crgpu_image_d3d12();
		CR_DELETE(&heap->_device->head.allocator, crgpu_image_d3d12, image);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	image->_current_state = initialState;
	image->_heap          = heap;
	CRGPU___LIVE_ADD(&heap->_device->_live, CRGPU___OBJECT_IMAGE);

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
}

#endif // CRGPU_COMPILE_D3D12_API
