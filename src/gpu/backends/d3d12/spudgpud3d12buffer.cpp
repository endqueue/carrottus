
#if SPUDGPU_COMPILE_D3D12_API

#include "spudgpud3d12.hpp"
#include "../../../spudstructbase.h"

CD3DX12_RESOURCE_DESC spudgpu_d3d12_create_resource_desc_from_buffer(
    const spudgpu_buffer_desc *desc) {
	D3D12_RESOURCE_FLAGS d3dResourceFlags =
	    spudgpu_d3d12_get_buffer_resource_flags(desc->usage);
	// A CBV's SizeInBytes must be a 256-byte-aligned multiple
	// (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), and CreateConstantBufferView
	// rounds up to that regardless of the resource's real size -- so the backing
	// resource itself has to be allocated at least that large or the CBV's declared
	// range runs past the end of the resource. Padding is allocation-only: the
	// caller-visible spudgpu_buffer_desc::size (returned by spudgpu_get_buffer_desc)
	// is left exactly as requested.
	uint64_t allocSize = desc->size;
	if (desc->usage == SPUDGPU_BUFFER_USAGE_UNIFORM)
		allocSize = (allocSize + 255) & ~(uint64_t)255;
	CD3DX12_RESOURCE_DESC result =
	    CD3DX12_RESOURCE_DESC::Buffer(allocSize, d3dResourceFlags);
	return result;
}

SPUDRESULT spudgpu_d3d12_check_buffer_desc(
    SPUDGPU_FEATURES features,
    const spudgpu_buffer_desc *desc) {
	if (desc->size == 0)
		return SPUDRESULT_GPU_ZERO_BUFFER_SIZE;
	if (desc->usage == SPUDGPU_BUFFER_USAGE_NONE)
		return SPUDRESULT_GPU_INVALID_BUFFER_USAGE;
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

extern "C" {

SPUDRESULT spudgpu_create_buffer(
    spudgpu_device device,
    const spudgpu_buffer_desc *desc,
    spudgpu_buffer *out_buffer) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_buffer", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_BUFFER_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_buffer)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT descResult = spudgpu_d3d12_check_buffer_desc(device->_features, desc);
	if (descResult != SPUD_SUCCESS)
		return descResult;

	spudgpu_buffer_d3d12 *pResult =
	    SPUD_NEW(&device->head.allocator, spudgpu_buffer_d3d12);
	if (!pResult)
		return SPUDRESULT_OUT_OF_MEMORY;
	pResult                 = new (pResult) spudgpu_buffer_d3d12();
	pResult->head.allocator = device->head.allocator;
	SPUD_BIRTH(pResult);
	pResult->_device        = device;
	pResult->_desc          = *desc;

	pResult->_d3d_resource_desc =
	    spudgpu_d3d12_create_resource_desc_from_buffer(desc);
	D3D12_HEAP_FLAGS d3dHeapFlags =
	    spudgpu_d3d12_get_heap_flags();
	D3D12_HEAP_PROPERTIES d3dHeapProperties =
	    spudgpu_d3d12_get_heap_properties_from_memory_kind(desc->memory_kind);
	D3D12_RESOURCE_STATES d3dInitialState =
	    spudgpu_d3d12_get_initial_buffer_state(desc->memory_kind);
	if (device->_d3d_device->CreateCommittedResource(
	        &d3dHeapProperties, d3dHeapFlags, &pResult->_d3d_resource_desc,
	        d3dInitialState, nullptr,
	        IID_PPV_ARGS(&pResult->_d3d_resource))) {
		pResult->~spudgpu_buffer_d3d12();
		SPUD_DELETE(&device->head.allocator, spudgpu_buffer_d3d12, pResult);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	// spudgpu_get_buffer_gpu_address hands this back (see spudgpu.h).
	pResult->_d3d_gpu_address = pResult->_d3d_resource->GetGPUVirtualAddress();

	// From here spudgpu_destroy_buffer frees it, and takes the count back.
	SPUDGPU___LIVE_ADD(&device->_live, SPUDGPU___OBJECT_BUFFER);
	if (spud_debug_name_set(pResult, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_buffer(pResult);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	pResult->_desc.debug_name = pResult->head.debug_name;

	*out_buffer = pResult;
	return SPUD_SUCCESS;
}
void spudgpu_destroy_buffer(spudgpu_buffer buffer) {
	if (!buffer)
		return;
	if (SPUD_STALE(buffer, "spudgpu_destroy_buffer", "buffer"))
		return;
#if _DEBUG
	if (buffer->_heap)
		buffer->_heap->_placed_count--;
#endif
	SPUDGPU___LIVE_REMOVE(&buffer->_device->_live, SPUDGPU___OBJECT_BUFFER);
	SPUD_KILL(buffer);
	const SPUD_ALLOCATOR allocator = buffer->head.allocator;
	spud_string_free(&allocator, buffer->head.debug_name);
	buffer->~spudgpu_buffer_d3d12();
	SPUD_DELETE(&allocator, spudgpu_buffer_d3d12, buffer);
}
SPUDRESULT
spudgpu_get_buffer_desc(spudgpu_buffer buffer, spudgpu_buffer_desc *out_desc) {
	if (!buffer)
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (SPUD_STALE(buffer, "spudgpu_get_buffer_desc", "buffer"))
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (!out_desc)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = buffer->_desc;
	out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_BUFFER_DESC);
	return SPUD_SUCCESS;
}

SPUDRESULT
spudgpu_get_buffer_gpu_address(spudgpu_buffer buffer, uint64_t *out_address) {
	if (!buffer)
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (SPUD_STALE(buffer, "spudgpu_get_buffer_gpu_address", "buffer"))
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (!out_address)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_address = buffer->_d3d_gpu_address;
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_create_buffer_view(
    spudgpu_buffer buffer,
    const spudgpu_buffer_view_desc *desc,
    spudgpu_buffer_view *out_buffer_view) {
	if (!buffer)
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (SPUD_STALE(buffer, "spudgpu_create_buffer_view", "buffer"))
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_buffer_view)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (desc->size == 0)
		return SPUDRESULT_GPU_ZERO_BUFFER_SIZE;
	if (desc->offset_from_parent_buffer + desc->size > buffer->_desc.size)
		return SPUDRESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;

	spudgpu_buffer_view_d3d12 *pResult = SPUD_NEW_ZEROED(
	    &buffer->_device->head.allocator, spudgpu_buffer_view_d3d12);
	if (!pResult)
		return SPUDRESULT_OUT_OF_MEMORY;
	pResult->head.allocator = buffer->_device->head.allocator;
	SPUD_BIRTH(pResult);
	pResult->_buffer        = buffer;
	pResult->_desc          = *desc;

	// spudgpu_buffer_desc::usage is a bitmask (a buffer can legitimately carry
	// more than one usage, e.g. UNIFORM | VERTEX for a per-instance buffer
	// also read by a compute pass) -- test membership, not exact equality,
	// or a multi-usage buffer falls through to default below. VERTEX/INDEX
	// are checked first since they're what spudgpu_create_buffer_view is
	// actually for; a buffer's UNIFORM/STORAGE binding goes through
	// spudgpu_write_descriptor_set instead and never reaches here in
	// practice, but the case is kept for a caller that does.
	if (buffer->_desc.usage & SPUDGPU_BUFFER_USAGE_VERTEX) {
		pResult->_d3d_view._vb.BufferLocation =
		    buffer->_d3d_gpu_address + desc->offset_from_parent_buffer;
		pResult->_d3d_view._vb.StrideInBytes  = desc->stride;
		pResult->_d3d_view._vb.SizeInBytes    = desc->size;
	} else if (buffer->_desc.usage & SPUDGPU_BUFFER_USAGE_INDEX) {
		pResult->_d3d_view._ib.BufferLocation =
		    buffer->_d3d_gpu_address + desc->offset_from_parent_buffer;
		if (desc->stride == 4)
			pResult->_d3d_view._ib.Format = DXGI_FORMAT_R32_UINT;
		else if (desc->stride == 2)
			pResult->_d3d_view._ib.Format = DXGI_FORMAT_R16_UINT;
		else {
			SPUD_DELETE(&buffer->_device->head.allocator, spudgpu_buffer_view_d3d12, pResult);
			return SPUDRESULT_GPU_INVALID_INDEX_STRIDE;
		}
		pResult->_d3d_view._ib.SizeInBytes = desc->size;
	} else if (buffer->_desc.usage & SPUDGPU_BUFFER_USAGE_UNIFORM) {
		pResult->_d3d_view._cb.BufferLocation =
		    buffer->_d3d_gpu_address + desc->offset_from_parent_buffer;
		pResult->_d3d_view._cb.SizeInBytes    = desc->size;
	}
	// Any other buffer (storage, indirect arguments, copies only) has no
	// D3D12 view object: what reads it through the view - a storage
	// descriptor written by spudgpu_bindless_register_storage_buffer - is
	// made from _desc's range. Vulkan and Metal make a view of a buffer of
	// any usage, and so does this backend.

	pResult->_device = buffer->_device;
	SPUDGPU___LIVE_ADD(&pResult->_device->_live, SPUDGPU___OBJECT_BUFFER_VIEW);
	*out_buffer_view = pResult;
	return SPUD_SUCCESS;
}
void spudgpu_destroy_buffer_view(spudgpu_buffer_view buffer) {
	if (!buffer)
		return;
	if (SPUD_STALE(buffer, "spudgpu_destroy_buffer_view", "buffer"))
		return;
	SPUDGPU___LIVE_REMOVE(&buffer->_device->_live, SPUDGPU___OBJECT_BUFFER_VIEW);
	SPUD_KILL(buffer);
	const SPUD_ALLOCATOR allocator = buffer->head.allocator;
	spud_string_free(&allocator, buffer->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_buffer_view_d3d12, buffer);
}
SPUDRESULT spudgpu_get_buffer_view_desc(
    spudgpu_buffer_view view, spudgpu_buffer_view_desc *out_desc) {
	if (!view)
		return SPUDRESULT_GPU_INVALID_BUFFER_VIEW;
	if (SPUD_STALE(view, "spudgpu_get_buffer_view_desc", "view"))
		return SPUDRESULT_GPU_INVALID_BUFFER_VIEW;
	if (!out_desc)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = view->_desc;
	out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC);
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_map_buffer(
    spudgpu_buffer buffer, uint64_t offset, uint64_t size, void **ppData) {
	if (!buffer)
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (SPUD_STALE(buffer, "spudgpu_map_buffer", "buffer"))
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (offset + size > buffer->_desc.size)
		return SPUDRESULT_GPU_MAP_OUT_OF_RANGE;
	if (!ppData)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (buffer->_desc.memory_kind == SPUDGPU_MEMORY_KIND_DEVICE)
		return SPUDRESULT_GPU_INVALID_MEMORY_KIND;
	// The read range says what the CPU may read through the pointer. An
	// UPLOAD buffer is never read (see SPUDGPU_MEMORY_KIND), which is the
	// empty range; the other kinds may read all of what they map. 0 means
	// "map the entire buffer" per spudgpu.h.
	uint64_t mapSize       = (size == 0) ? buffer->_desc.size : size;
	CD3DX12_RANGE d3dRange = (buffer->_desc.memory_kind == SPUDGPU_MEMORY_KIND_UPLOAD)
	                             ? CD3DX12_RANGE(0, 0)
	                             : CD3DX12_RANGE(offset, offset + mapSize);
	void *mapped           = nullptr;
	if (FAILED(buffer->_d3d_resource->Map(0, &d3dRange, &mapped)))
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	// Map returns the start of the resource whatever the read range is: the
	// range is never added to the pointer.
	*ppData = (uint8_t *)mapped + offset;
	return SPUD_SUCCESS;
}
void spudgpu_unmap_buffer(spudgpu_buffer buffer) {
	if (!buffer) return;
	if (SPUD_STALE(buffer, "spudgpu_unmap_buffer", "buffer")) return;
	buffer->_d3d_resource->Unmap(0, nullptr);
}

// D3D12 has no flush or invalidate call: a mapped resource is coherent with
// the GPU as it is, and the ranges Map() and Unmap() take are hints. Both
// calls make the checks every backend makes and then have nothing to do.
SPUDRESULT spudgpu_flush_buffer(spudgpu_buffer buffer, uint64_t offset, uint64_t size) {
	if (!buffer)
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (SPUD_STALE(buffer, "spudgpu_flush_buffer", "buffer"))
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (buffer->_desc.memory_kind != SPUDGPU_MEMORY_KIND_UPLOAD &&
	    buffer->_desc.memory_kind != SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE)
		return SPUDRESULT_GPU_INVALID_MEMORY_KIND;
	if (offset + size > buffer->_desc.size)
		return SPUDRESULT_GPU_MAP_OUT_OF_RANGE;
	return SPUD_SUCCESS;
}
SPUDRESULT spudgpu_invalidate_buffer(spudgpu_buffer buffer, uint64_t offset, uint64_t size) {
	if (!buffer)
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (SPUD_STALE(buffer, "spudgpu_invalidate_buffer", "buffer"))
		return SPUDRESULT_GPU_INVALID_BUFFER;
	if (buffer->_desc.memory_kind != SPUDGPU_MEMORY_KIND_READBACK &&
	    buffer->_desc.memory_kind != SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE)
		return SPUDRESULT_GPU_INVALID_MEMORY_KIND;
	if (offset + size > buffer->_desc.size)
		return SPUDRESULT_GPU_MAP_OUT_OF_RANGE;
	return SPUD_SUCCESS;
}
}

#endif // SPUDGPU_COMPILE_D3D12_API
