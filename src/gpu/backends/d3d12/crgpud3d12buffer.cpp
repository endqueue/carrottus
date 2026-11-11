
#if CRGPU_COMPILE_D3D12_API

#include "crgpud3d12.hpp"
#include "../../../crstructbase.h"

CD3DX12_RESOURCE_DESC crgpu_d3d12_create_resource_desc_from_buffer(
    const crgpu_buffer_desc *desc) {
	D3D12_RESOURCE_FLAGS d3dResourceFlags =
	    crgpu_d3d12_get_buffer_resource_flags(desc->usage);
	// A CBV's SizeInBytes must be a 256-byte-aligned multiple
	// (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), and CreateConstantBufferView
	// rounds up to that regardless of the resource's real size -- so the backing
	// resource itself has to be allocated at least that large or the CBV's declared
	// range runs past the end of the resource. Padding is allocation-only: the
	// caller-visible crgpu_buffer_desc::size (returned by crgpu_buffer_get_desc)
	// is left exactly as requested.
	uint64_t allocSize = desc->size;
	if (desc->usage == CRGPU_BUFFER_USAGE_UNIFORM)
		allocSize = (allocSize + 255) & ~(uint64_t)255;
	CD3DX12_RESOURCE_DESC result =
	    CD3DX12_RESOURCE_DESC::Buffer(allocSize, d3dResourceFlags);
	return result;
}

CR_RESULT crgpu_d3d12_check_buffer_desc(
    CRGPU_FEATURES features,
    const crgpu_buffer_desc *desc) {
	if (desc->size == 0)
		return CR_RESULT_GPU_ZERO_BUFFER_SIZE;
	if (desc->usage == CRGPU_BUFFER_USAGE_NONE)
		return CR_RESULT_GPU_INVALID_BUFFER_USAGE;
	// CRGPU_MEMORY_KIND's contract (see crgpu.h), the same checks in the
	// same order on every backend.
	if (desc->memory_kind < CRGPU_MEMORY_KIND_DEVICE || desc->memory_kind > CRGPU_MEMORY_KIND_DEVICE_MAPPABLE)
		return CR_RESULT_GPU_INVALID_MEMORY_KIND;
	if (desc->memory_kind == CRGPU_MEMORY_KIND_UPLOAD &&
	    (desc->usage & (CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_DST)))
		return CR_RESULT_GPU_INVALID_BUFFER_USAGE;
	if (desc->memory_kind == CRGPU_MEMORY_KIND_READBACK && desc->usage != CRGPU_BUFFER_USAGE_TRANSFER_DST)
		return CR_RESULT_GPU_INVALID_BUFFER_USAGE;
	if (desc->memory_kind == CRGPU_MEMORY_KIND_DEVICE_MAPPABLE && !(features & CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY))
		return CR_RESULT_GPU_MEMORY_KIND_NOT_SUPPORTED;
	return CR_SUCCESS;
}

extern "C" {

CR_RESULT crgpu_buffer_create(
    crgpu_device device,
    const crgpu_buffer_desc *desc,
    crgpu_buffer *out_buffer) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_buffer_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_BUFFER_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_buffer)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT descResult = crgpu_d3d12_check_buffer_desc(device->_features, desc);
	if (descResult != CR_SUCCESS)
		return descResult;

	crgpu_buffer_d3d12 *pResult =
	    CR_NEW(&device->head.allocator, crgpu_buffer_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult                 = new (pResult) crgpu_buffer_d3d12();
	pResult->head.allocator = device->head.allocator;
	CR_BIRTH_IN(pResult, &device->_quarantine);
	pResult->_device        = device;
	pResult->_desc          = *desc;

	pResult->_d3d_resource_desc =
	    crgpu_d3d12_create_resource_desc_from_buffer(desc);
	D3D12_HEAP_FLAGS d3dHeapFlags =
	    crgpu_d3d12_get_heap_flags();
	D3D12_HEAP_PROPERTIES d3dHeapProperties =
	    crgpu_d3d12_get_heap_properties_from_memory_kind(desc->memory_kind);
	D3D12_RESOURCE_STATES d3dInitialState =
	    crgpu_d3d12_get_initial_buffer_state(desc->memory_kind);
	if (device->_d3d_device->CreateCommittedResource(
	        &d3dHeapProperties, d3dHeapFlags, &pResult->_d3d_resource_desc,
	        d3dInitialState, nullptr,
	        IID_PPV_ARGS(&pResult->_d3d_resource))) {
		pResult->~crgpu_buffer_d3d12();
		CR_DELETE(&device->head.allocator, crgpu_buffer_d3d12, pResult);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	// crgpu_buffer_get_gpu_address hands this back (see crgpu.h).
	pResult->_d3d_gpu_address = pResult->_d3d_resource->GetGPUVirtualAddress();

	// From here crgpu_buffer_destroy frees it, and takes the count back.
	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_BUFFER);
	if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
		crgpu_buffer_destroy(pResult);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	pResult->_desc.debug_name = pResult->head.debug_name;

	*out_buffer = pResult;
	return CR_SUCCESS;
}
void crgpu_buffer_destroy(crgpu_buffer buffer) {
	if (!buffer)
		return;
	if (CR_STALE(buffer, "crgpu_buffer_destroy", "buffer"))
		return;
#if _DEBUG
	if (buffer->_heap)
		buffer->_heap->_placed_count--;
#endif
	CRGPU___LIVE_REMOVE(&buffer->_device->_live, CRGPU___OBJECT_BUFFER);
	CR_KILL(buffer);
	const CR_ALLOCATOR allocator = buffer->head.allocator;
	cr_string_free(&allocator, buffer->head.debug_name);
	CR_QUARANTINE_KEEP(quarantine, buffer);
	buffer->~crgpu_buffer_d3d12();
	CR_RETIRE(quarantine, &allocator, crgpu_buffer_d3d12, buffer);
}
CR_RESULT
crgpu_buffer_get_desc(crgpu_buffer buffer, crgpu_buffer_desc *out_desc) {
	if (!buffer)
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (CR_STALE(buffer, "crgpu_buffer_get_desc", "buffer"))
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (!out_desc)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = buffer->_desc;
	out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_BUFFER_DESC);
	return CR_SUCCESS;
}

CR_RESULT
crgpu_buffer_get_gpu_address(crgpu_buffer buffer, uint64_t *out_address) {
	if (!buffer)
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (CR_STALE(buffer, "crgpu_buffer_get_gpu_address", "buffer"))
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (!out_address)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_address = buffer->_d3d_gpu_address;
	return CR_SUCCESS;
}

CR_RESULT crgpu_buffer_view_create(
    crgpu_buffer buffer,
    const crgpu_buffer_view_desc *desc,
    crgpu_buffer_view *out_buffer_view) {
	if (!buffer)
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (CR_STALE(buffer, "crgpu_buffer_view_create", "buffer"))
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_buffer_view)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (desc->size == 0)
		return CR_RESULT_GPU_ZERO_BUFFER_SIZE;
	if (desc->offset_from_parent_buffer + desc->size > buffer->_desc.size)
		return CR_RESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;

	crgpu_buffer_view_d3d12 *pResult = CR_NEW_ZEROED(
	    &buffer->_device->head.allocator, crgpu_buffer_view_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult->head.allocator = buffer->_device->head.allocator;
	CR_BIRTH_IN(pResult, &buffer->_device->_quarantine);
	pResult->_buffer        = buffer;
	pResult->_desc          = *desc;

	// crgpu_buffer_desc::usage is a bitmask (a buffer can legitimately carry
	// more than one usage, e.g. UNIFORM | VERTEX for a per-instance buffer
	// also read by a compute pass) -- test membership, not exact equality,
	// or a multi-usage buffer falls through to default below. VERTEX/INDEX
	// are checked first since they're what crgpu_buffer_view_create is
	// actually for; a buffer's UNIFORM/STORAGE binding goes through
	// crgpu_write_descriptor_set instead and never reaches here in
	// practice, but the case is kept for a caller that does.
	if (buffer->_desc.usage & CRGPU_BUFFER_USAGE_VERTEX) {
		pResult->_d3d_view._vb.BufferLocation =
		    buffer->_d3d_gpu_address + desc->offset_from_parent_buffer;
		pResult->_d3d_view._vb.StrideInBytes  = desc->stride;
		pResult->_d3d_view._vb.SizeInBytes    = desc->size;
	} else if (buffer->_desc.usage & CRGPU_BUFFER_USAGE_INDEX) {
		pResult->_d3d_view._ib.BufferLocation =
		    buffer->_d3d_gpu_address + desc->offset_from_parent_buffer;
		if (desc->stride == 4)
			pResult->_d3d_view._ib.Format = DXGI_FORMAT_R32_UINT;
		else if (desc->stride == 2)
			pResult->_d3d_view._ib.Format = DXGI_FORMAT_R16_UINT;
		else {
			CR_DELETE(&buffer->_device->head.allocator, crgpu_buffer_view_d3d12, pResult);
			return CR_RESULT_GPU_INVALID_INDEX_STRIDE;
		}
		pResult->_d3d_view._ib.SizeInBytes = desc->size;
	} else if (buffer->_desc.usage & CRGPU_BUFFER_USAGE_UNIFORM) {
		pResult->_d3d_view._cb.BufferLocation =
		    buffer->_d3d_gpu_address + desc->offset_from_parent_buffer;
		pResult->_d3d_view._cb.SizeInBytes    = desc->size;
	}
	// Any other buffer (storage, indirect arguments, copies only) has no
	// D3D12 view object: what reads it through the view - a storage
	// descriptor written by crgpu_bindless_register_storage_buffer - is
	// made from _desc's range. Vulkan and Metal make a view of a buffer of
	// any usage, and so does this backend.

	pResult->_device = buffer->_device;
	CRGPU___LIVE_ADD(&pResult->_device->_live, CRGPU___OBJECT_BUFFER_VIEW);
	*out_buffer_view = pResult;
	return CR_SUCCESS;
}
void crgpu_buffer_view_destroy(crgpu_buffer_view buffer) {
	if (!buffer)
		return;
	if (CR_STALE(buffer, "crgpu_buffer_view_destroy", "buffer"))
		return;
	CRGPU___LIVE_REMOVE(&buffer->_device->_live, CRGPU___OBJECT_BUFFER_VIEW);
	CR_KILL(buffer);
	const CR_ALLOCATOR allocator = buffer->head.allocator;
	cr_string_free(&allocator, buffer->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(buffer), &allocator, crgpu_buffer_view_d3d12, buffer);
}
CR_RESULT crgpu_buffer_view_get_desc(
    crgpu_buffer_view view, crgpu_buffer_view_desc *out_desc) {
	if (!view)
		return CR_RESULT_GPU_INVALID_BUFFER_VIEW;
	if (CR_STALE(view, "crgpu_buffer_view_get_desc", "view"))
		return CR_RESULT_GPU_INVALID_BUFFER_VIEW;
	if (!out_desc)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = view->_desc;
	out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC);
	return CR_SUCCESS;
}

CR_RESULT crgpu_buffer_map(
    crgpu_buffer buffer, uint64_t offset, uint64_t size, void **ppData) {
	if (!buffer)
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (CR_STALE(buffer, "crgpu_buffer_map", "buffer"))
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (offset + size > buffer->_desc.size)
		return CR_RESULT_GPU_MAP_OUT_OF_RANGE;
	if (!ppData)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (buffer->_desc.memory_kind == CRGPU_MEMORY_KIND_DEVICE)
		return CR_RESULT_GPU_INVALID_MEMORY_KIND;
	// The read range says what the CPU may read through the pointer. An
	// UPLOAD buffer is never read (see CRGPU_MEMORY_KIND), which is the
	// empty range; the other kinds may read all of what they map. 0 means
	// "map the entire buffer" per crgpu.h.
	uint64_t mapSize       = (size == 0) ? buffer->_desc.size : size;
	CD3DX12_RANGE d3dRange = (buffer->_desc.memory_kind == CRGPU_MEMORY_KIND_UPLOAD)
	                             ? CD3DX12_RANGE(0, 0)
	                             : CD3DX12_RANGE(offset, offset + mapSize);
	void *mapped           = nullptr;
	if (FAILED(buffer->_d3d_resource->Map(0, &d3dRange, &mapped)))
		return CR_RESULT_API_SPECIFIC_FAILURE;
	// Map returns the start of the resource whatever the read range is: the
	// range is never added to the pointer.
	*ppData = (uint8_t *)mapped + offset;
	return CR_SUCCESS;
}
void crgpu_buffer_unmap(crgpu_buffer buffer) {
	if (!buffer) return;
	if (CR_STALE(buffer, "crgpu_buffer_unmap", "buffer")) return;
	buffer->_d3d_resource->Unmap(0, nullptr);
}

// D3D12 has no flush or invalidate call: a mapped resource is coherent with
// the GPU as it is, and the ranges Map() and Unmap() take are hints. Both
// calls make the checks every backend makes and then have nothing to do.
CR_RESULT crgpu_buffer_flush(crgpu_buffer buffer, uint64_t offset, uint64_t size) {
	if (!buffer)
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (CR_STALE(buffer, "crgpu_buffer_flush", "buffer"))
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (buffer->_desc.memory_kind != CRGPU_MEMORY_KIND_UPLOAD &&
	    buffer->_desc.memory_kind != CRGPU_MEMORY_KIND_DEVICE_MAPPABLE)
		return CR_RESULT_GPU_INVALID_MEMORY_KIND;
	if (offset + size > buffer->_desc.size)
		return CR_RESULT_GPU_MAP_OUT_OF_RANGE;
	return CR_SUCCESS;
}
CR_RESULT crgpu_buffer_invalidate(crgpu_buffer buffer, uint64_t offset, uint64_t size) {
	if (!buffer)
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (CR_STALE(buffer, "crgpu_buffer_invalidate", "buffer"))
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (buffer->_desc.memory_kind != CRGPU_MEMORY_KIND_READBACK &&
	    buffer->_desc.memory_kind != CRGPU_MEMORY_KIND_DEVICE_MAPPABLE)
		return CR_RESULT_GPU_INVALID_MEMORY_KIND;
	if (offset + size > buffer->_desc.size)
		return CR_RESULT_GPU_MAP_OUT_OF_RANGE;
	return CR_SUCCESS;
}
}

#endif // CRGPU_COMPILE_D3D12_API
