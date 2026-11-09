
#if SPUDGPU_COMPILE_D3D12_API

#include "spudgpud3d12.hpp"
#include <cassert>
#include "../../../spudstructbase.h"

// SpudGPU heaps on D3D12.
//
// A heap is an ID3D12Heap, and a placed buffer or image is a resource made
// in it with CreatePlacedResource. The heap's properties are the ones a
// committed buffer of its kind gets (spudgpu_d3d12_get_heap_properties_from_memory_kind),
// and its flags are those of every buffer and image
// (spudgpu_d3d12_get_heap_flags) plus the ALLOW_ONLY_ flag of its class. The
// ALLOW_ONLY_ flag is what resource heap tier 1 requires; it is set on tier 2
// as well, so a heap holds one class on every device (see
// SPUDGPU_HEAP_RESOURCE_CLASS).
//
// A resource's size and alignment are GetResourceAllocationInfo's for the
// D3D12_RESOURCE_DESC the committed path builds, with D3D12 choosing the
// alignment: 64 KiB for every buffer and every texture. Textures could ask
// for D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT (4 KiB) when they are small
// enough, but asking for it for one that isn't is a debug layer error, and
// telling them apart needs each format's tile shape. Not done yet.
//
// D3D12 maps placed resources one by one, as it does committed ones, so
// spudgpu_map_buffer is unchanged for a placed buffer.

static D3D12_HEAP_FLAGS spudgpu_d3d12_heap_class_flags(SPUDGPU_HEAP_RESOURCE_CLASS resource_class) {
	switch (resource_class) {
	// An acceleration structure is a buffer (spudgpud3d12raytracing.cpp).
	case SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS:
	case SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES:
		return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
	case SPUDGPU_HEAP_RESOURCE_CLASS_IMAGES:
		return D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
	default:
		return D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
	}
}

// The checks spudgpu_get_buffer_placement_requirements and
// spudgpu_create_placed_buffer make after their argument checks.
static SPUDRESULT spudgpu_d3d12_check_buffer_placement(
    spudgpu_heap heap,
    const spudgpu_buffer_desc *desc) {
	const SPUDRESULT sr = spudgpu_d3d12_check_buffer_desc(heap->_device->_features, desc);
	if (sr != SPUD_SUCCESS)
		return sr;
	if (heap->_desc.resource_class != SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS ||
	    desc->memory_kind != heap->_desc.memory_kind)
		return SPUDRESULT_GPU_HEAP_MISMATCH;
	return SPUD_SUCCESS;
}

// The checks spudgpu_get_image_placement_requirements and
// spudgpu_create_placed_image make after their argument checks.
static SPUDRESULT spudgpu_d3d12_check_image_placement(
    spudgpu_heap heap,
    const spudgpu_image_desc *desc) {
	const SPUDRESULT sr = spudgpu_d3d12_check_image_desc(desc);
	if (sr != SPUD_SUCCESS)
		return sr;
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

// The size and alignment of a resource of [d3d_desc].
static SPUDRESULT spudgpu_d3d12_placement_requirements(
    spudgpu_heap heap,
    const D3D12_RESOURCE_DESC *d3d_desc,
    spudgpu_placement_requirements *out_requirements) {
	const D3D12_RESOURCE_ALLOCATION_INFO info =
	    heap->_device->_d3d_device->GetResourceAllocationInfo(0, 1, d3d_desc);
	if (info.SizeInBytes == UINT64_MAX)
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	out_requirements->size      = info.SizeInBytes;
	out_requirements->alignment = info.Alignment;
	return SPUD_SUCCESS;
}

// The checks after the requirements are known, shared by both placed
// creates.
static SPUDRESULT spudgpu_d3d12_check_placement(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_placement_requirements *placement) {
	if (offset % placement->alignment)
		return SPUDRESULT_GPU_PLACEMENT_MISALIGNED;
	if (offset > heap->_desc.size || placement->size > heap->_desc.size - offset)
		return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	return SPUD_SUCCESS;
}

extern "C" {

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
	if (desc->memory_kind != SPUDGPU_MEMORY_KIND_DEVICE && !(device->_features & SPUDGPU_FEATURE_MAPPABLE_HEAPS))
		return SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
	if (desc->memory_kind == SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE && !(device->_features & SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY))
		return SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
	if (desc->resource_class == SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES && !(device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;

	spudgpu_heap_d3d12 *heap = SPUD_NEW(&device->head.allocator, spudgpu_heap_d3d12);
	if (!heap)
		return SPUDRESULT_OUT_OF_MEMORY;
	heap                 = new (heap) spudgpu_heap_d3d12();
	heap->head.allocator = device->head.allocator;
	SPUD_BIRTH_IN(heap, &device->_quarantine);
	heap->_device        = device;
	heap->_desc          = *desc;

	D3D12_HEAP_DESC d3dHeapDesc = {};
	d3dHeapDesc.SizeInBytes     = desc->size;
	d3dHeapDesc.Properties      = spudgpu_d3d12_get_heap_properties_from_memory_kind(desc->memory_kind);
	d3dHeapDesc.Alignment       = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	d3dHeapDesc.Flags           = spudgpu_d3d12_get_heap_flags() | spudgpu_d3d12_heap_class_flags(desc->resource_class);
	if (FAILED(device->_d3d_device->CreateHeap(&d3dHeapDesc, IID_PPV_ARGS(&heap->_d3d_heap)))) {
		heap->~spudgpu_heap_d3d12();
		SPUD_DELETE(&device->head.allocator, spudgpu_heap_d3d12, heap);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}

	SPUDGPU___LIVE_ADD(&device->_live, SPUDGPU___OBJECT_HEAP);
	if (spud_debug_name_set(heap, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_heap(heap);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	heap->_desc.debug_name = heap->head.debug_name;

	*out_heap = heap;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_heap(spudgpu_heap heap) {
	if (!heap)
		return;
	if (SPUD_STALE(heap, "spudgpu_destroy_heap", "heap"))
		return;
#if _DEBUG
	// Every buffer and image placed in the heap is destroyed first (see
	// spudgpu.h).
	assert(heap->_placed_count == 0);
#endif
	SPUDGPU___LIVE_REMOVE(&heap->_device->_live, SPUDGPU___OBJECT_HEAP);
	SPUD_KILL(heap);
	const SPUD_ALLOCATOR allocator = heap->head.allocator;
	spud_string_free(&allocator, heap->head.debug_name);
	SPUD_QUARANTINE_KEEP(quarantine, heap);
	heap->~spudgpu_heap_d3d12();
	SPUD_RETIRE(quarantine, &allocator, spudgpu_heap_d3d12, heap);
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
	*out_desc = heap->_desc;
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
	const SPUDRESULT sr = spudgpu_d3d12_check_buffer_placement(heap, desc);
	if (sr != SPUD_SUCCESS)
		return sr;
	const D3D12_RESOURCE_DESC d3dDesc = spudgpu_d3d12_create_resource_desc_from_buffer(desc);
	return spudgpu_d3d12_placement_requirements(heap, &d3dDesc, out_requirements);
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
	SPUDRESULT sr = spudgpu_d3d12_check_image_placement(heap, desc);
	if (sr != SPUD_SUCCESS)
		return sr;
	D3D12_RESOURCE_DESC d3dDesc = {};
	sr = spudgpu_d3d12_create_resource_desc_from_image(desc, &d3dDesc);
	if (sr != SPUD_SUCCESS)
		return sr;
	return spudgpu_d3d12_placement_requirements(heap, &d3dDesc, out_requirements);
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
	SPUDRESULT sr = spudgpu_d3d12_check_buffer_placement(heap, desc);
	if (sr != SPUD_SUCCESS)
		return sr;

	const D3D12_RESOURCE_DESC d3dDesc = spudgpu_d3d12_create_resource_desc_from_buffer(desc);
	spudgpu_placement_requirements placement;
	sr = spudgpu_d3d12_placement_requirements(heap, &d3dDesc, &placement);
	if (sr != SPUD_SUCCESS)
		return sr;
	sr = spudgpu_d3d12_check_placement(heap, offset, &placement);
	if (sr != SPUD_SUCCESS)
		return sr;

	spudgpu_buffer_d3d12 *buffer = SPUD_NEW(&heap->_device->head.allocator, spudgpu_buffer_d3d12);
	if (!buffer)
		return SPUDRESULT_OUT_OF_MEMORY;
	buffer                     = new (buffer) spudgpu_buffer_d3d12();
	buffer->head.allocator     = heap->_device->head.allocator;
	SPUD_BIRTH_IN(buffer, &heap->_device->_quarantine);
	buffer->_device            = heap->_device;
	buffer->_desc              = *desc;
	buffer->_d3d_resource_desc = d3dDesc;
	if (FAILED(heap->_device->_d3d_device->CreatePlacedResource(
	        heap->_d3d_heap.Get(), offset, &buffer->_d3d_resource_desc,
	        spudgpu_d3d12_get_initial_buffer_state(desc->memory_kind), nullptr,
	        IID_PPV_ARGS(&buffer->_d3d_resource)))) {
		buffer->~spudgpu_buffer_d3d12();
		SPUD_DELETE(&heap->_device->head.allocator, spudgpu_buffer_d3d12, buffer);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	buffer->_d3d_gpu_address = buffer->_d3d_resource->GetGPUVirtualAddress();
	buffer->_heap                      = heap;
	SPUDGPU___LIVE_ADD(&heap->_device->_live, SPUDGPU___OBJECT_BUFFER);

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
	SPUDRESULT sr = spudgpu_d3d12_check_image_placement(heap, desc);
	if (sr != SPUD_SUCCESS)
		return sr;

	D3D12_RESOURCE_DESC d3dDesc = {};
	sr = spudgpu_d3d12_create_resource_desc_from_image(desc, &d3dDesc);
	if (sr != SPUD_SUCCESS)
		return sr;
	spudgpu_placement_requirements placement;
	sr = spudgpu_d3d12_placement_requirements(heap, &d3dDesc, &placement);
	if (sr != SPUD_SUCCESS)
		return sr;
	sr = spudgpu_d3d12_check_placement(heap, offset, &placement);
	if (sr != SPUD_SUCCESS)
		return sr;

	spudgpu_image_d3d12 *image = SPUD_NEW(&heap->_device->head.allocator, spudgpu_image_d3d12);
	if (!image)
		return SPUDRESULT_OUT_OF_MEMORY;
	image                      = new (image) spudgpu_image_d3d12();
	image->head.allocator      = heap->_device->head.allocator;
	SPUD_BIRTH_IN(image, &heap->_device->_quarantine);
	image->_subresource_states = eq::array<D3D12_RESOURCE_STATES>(&heap->_device->_eq_allocator);
	image->_device            = heap->_device;
	image->_desc              = *desc;
	image->_d3d_resource_desc = d3dDesc;

	// The initial state and optimized clear value of a committed image of
	// the same desc.
	const D3D12_RESOURCE_STATES initialState = spudgpu_d3d12_get_initial_image_state(desc->usage);
	D3D12_CLEAR_VALUE clearValue             = {};
	const bool hasClearValue                 = spudgpu_d3d12_get_image_clear_value(desc, &clearValue);
	if (FAILED(heap->_device->_d3d_device->CreatePlacedResource(
	        heap->_d3d_heap.Get(), offset, &image->_d3d_resource_desc,
	        initialState, hasClearValue ? &clearValue : nullptr,
	        IID_PPV_ARGS(&image->_d3d_resource)))) {
		image->~spudgpu_image_d3d12();
		SPUD_DELETE(&heap->_device->head.allocator, spudgpu_image_d3d12, image);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	image->_current_state = initialState;
	image->_heap          = heap;
	SPUDGPU___LIVE_ADD(&heap->_device->_live, SPUDGPU___OBJECT_IMAGE);

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
}

#endif // SPUDGPU_COMPILE_D3D12_API
