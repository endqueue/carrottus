//
// Carrottus GPU Metal backend - buffers and buffer views.
// Implemented and verified against real Apple hardware.
//

#if CRGPU_COMPILE_METAL_API

#include "crgpu.h"
#include "crcore.h"
#include "crgpumetal.h"
#include <Metal/Metal.h>
#include "../../../crstructbase.h"

// The storage a buffer of [kind] gets: what each CRGPU_MEMORY_KIND
// documents in crgpu.h. Only two storage modes are used - Private for the
// one kind that is never mapped, Shared for the rest - and the CPU cache mode
// separates the kind the CPU only writes from the kinds it reads.
// MTLStorageModeManaged is not used: it keeps a second copy to synchronize,
// which no kind asks for. crgpu_buffer_create has already refused a value
// that is not a kind.
MTLResourceOptions crgpumetal___buffer_resource_options(CRGPU_MEMORY_KIND memory_kind) {
	switch (memory_kind) {
	case CRGPU_MEMORY_KIND_UPLOAD:
		return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
	case CRGPU_MEMORY_KIND_READBACK:
	case CRGPU_MEMORY_KIND_DEVICE_MAPPABLE:
		return MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache;
	default:
		return MTLResourceStorageModePrivate;
	}
}

CR_RESULT crgpumetal___check_buffer_desc(
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

	crgpu_device_metal *device_metal = (crgpu_device_metal *)device;
	CR_RESULT sr                      = crgpumetal___check_buffer_desc(device_metal->_features, desc);
	if (sr != CR_SUCCESS)
		return sr;

	crgpu_buffer_metal *buffer_metal = CR_NEW_ZEROED(&device_metal->head.allocator, crgpu_buffer_metal);
	if (!buffer_metal) {
		sr = CR_RESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	buffer_metal->head.allocator = device_metal->head.allocator;
	CR_BIRTH_IN(buffer_metal, &device_metal->_quarantine);
	buffer_metal->_parent_device = device_metal;
	buffer_metal->_desc          = *desc;

	buffer_metal->_buffer_mtl = [device_metal->_device_mtl
	    newBufferWithLength:desc->size
	                 options:crgpumetal___buffer_resource_options(desc->memory_kind)];
	if (!buffer_metal->_buffer_mtl) {
		sr = CR_RESULT_API_SPECIFIC_FAILURE;
		goto failedattempt;
	}
	// From here crgpu_buffer_destroy frees it, and takes the count back.
	CRGPU___LIVE_ADD(&device_metal->_live, CRGPU___OBJECT_BUFFER);

	if (cr_debug_name_set(buffer_metal, desc->debug_name) != CR_SUCCESS) {
		crgpu_buffer_destroy((crgpu_buffer)buffer_metal);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	buffer_metal->_desc.debug_name = buffer_metal->head.debug_name;

	*out_buffer = (crgpu_buffer)buffer_metal;

	return sr;

failedattempt:
	CR_DELETE(&device_metal->head.allocator, crgpu_buffer_metal, buffer_metal);
	return sr;
}
void crgpu_buffer_destroy(crgpu_buffer buffer) {
	crgpu_buffer_metal *buffer_metal = (crgpu_buffer_metal *)buffer;
	if (buffer_metal) {
		if (CR_STALE(buffer, "crgpu_buffer_destroy", "buffer"))
			return;
		if (buffer_metal->_buffer_mtl) {
			[buffer_metal->_buffer_mtl release];
		}
#if _DEBUG
		if (buffer_metal->_heap)
			buffer_metal->_heap->_placed_count--;
#endif
		CRGPU___LIVE_REMOVE(&buffer_metal->_parent_device->_live, CRGPU___OBJECT_BUFFER);
		CR_KILL(buffer);
		const CR_ALLOCATOR allocator = buffer_metal->head.allocator;
		cr_string_free(&allocator, buffer_metal->head.debug_name);
		CR_RETIRE(CR_QUARANTINE_OF(buffer_metal), &allocator, crgpu_buffer_metal, buffer_metal);
	}
}
CR_RESULT crgpu_buffer_get_desc(
    crgpu_buffer buffer,
    crgpu_buffer_desc *out_desc) {
	crgpu_buffer_metal *buffer_metal = (crgpu_buffer_metal *)buffer;
	if (!buffer_metal)
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (CR_STALE(buffer, "crgpu_buffer_get_desc", "buffer"))
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (!out_desc)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = buffer_metal->_desc;
	out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_BUFFER_DESC);
	return CR_SUCCESS;
}

CR_RESULT crgpu_buffer_get_gpu_address(
    crgpu_buffer buffer,
    uint64_t *out_address) {
	crgpu_buffer_metal *buffer_metal = (crgpu_buffer_metal *)buffer;
	if (!buffer_metal)
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (CR_STALE(buffer, "crgpu_buffer_get_gpu_address", "buffer"))
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (!out_address)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	// Every buffer has one, mirroring D3D12's always-available
	// GetGPUVirtualAddress: -[MTLBuffer gpuAddress], valid for any storage
	// mode.
	*out_address = buffer_metal->_buffer_mtl.gpuAddress;
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

	crgpu_buffer_metal *parent_buffer_metal = (crgpu_buffer_metal *)buffer;
	if (desc->offset_from_parent_buffer + desc->size > parent_buffer_metal->_desc.size)
		return CR_RESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;

	CR_RESULT sr = CR_SUCCESS;

	crgpu_buffer_view_metal *result = CR_NEW_ZEROED(&parent_buffer_metal->head.allocator, crgpu_buffer_view_metal);
	if (!result) {
		sr = CR_RESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	result->head.allocator      = parent_buffer_metal->head.allocator;
	CR_BIRTH_IN(result, &parent_buffer_metal->_parent_device->_quarantine);
	result->_desc               = *desc;
	result->_desc.parent_buffer = buffer; // Authoritative - see crgpu_buffer_view_metal.
	result->_parent_device      = parent_buffer_metal->_parent_device;

	CRGPU___LIVE_ADD(&result->_parent_device->_live, CRGPU___OBJECT_BUFFER_VIEW);
	*out_buffer_view = (crgpu_buffer_view)result;

	return sr;

failedattempt:
	return sr;
}

void crgpu_buffer_view_destroy(crgpu_buffer_view buffer_view) {
	if (!buffer_view)
		return;
	if (CR_STALE(buffer_view, "crgpu_buffer_view_destroy", "buffer_view"))
		return;
	CRGPU___LIVE_REMOVE(&buffer_view->_parent_device->_live, CRGPU___OBJECT_BUFFER_VIEW);
	CR_KILL(buffer_view);
	const CR_ALLOCATOR allocator = buffer_view->head.allocator;
	cr_string_free(&allocator, buffer_view->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(buffer_view), &allocator, crgpu_buffer_view_metal, buffer_view);
}
CR_RESULT crgpu_buffer_view_get_desc(
    crgpu_buffer_view buffer_view,
    crgpu_buffer_view_desc *out_desc) {
	if (!buffer_view)
		return CR_RESULT_GPU_INVALID_BUFFER_VIEW;
	if (CR_STALE(buffer_view, "crgpu_buffer_view_get_desc", "buffer_view"))
		return CR_RESULT_GPU_INVALID_BUFFER_VIEW;
	if (!out_desc)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = ((crgpu_buffer_view_metal *)buffer_view)->_desc;
	out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC);
	return CR_SUCCESS;
}

CR_RESULT crgpu_buffer_map(
    crgpu_buffer buffer,
    uint64_t offset,
    uint64_t size,
    void **ppData) {
	if (!buffer)
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (CR_STALE(buffer, "crgpu_buffer_map", "buffer"))
		return CR_RESULT_GPU_INVALID_BUFFER;

	crgpu_buffer_metal *buffer_metal = (crgpu_buffer_metal *)buffer;
	if (offset + size > buffer_metal->_desc.size)
		return CR_RESULT_GPU_MAP_OUT_OF_RANGE;
	if (!ppData)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	// Only a Shared-storage buffer has a CPU-visible pointer at all, which
	// is every kind but DEVICE - see
	// crgpumetal___buffer_resource_options.
	if (buffer_metal->_desc.memory_kind == CRGPU_MEMORY_KIND_DEVICE)
		return CR_RESULT_GPU_INVALID_MEMORY_KIND;

	uint64_t map_size = (size == 0) ? buffer_metal->_desc.size : size;
	if (offset + map_size > buffer_metal->_desc.size)
		return CR_RESULT_GPU_MAP_OUT_OF_RANGE;

	// Metal has no map/unmap operation to perform - a Shared-storage
	// buffer's CPU pointer is always available via -contents; "mapping" is
	// just handing back contents + offset.
	*ppData = (uint8_t *)buffer_metal->_buffer_mtl.contents + offset;

	return CR_SUCCESS;
}

void crgpu_buffer_unmap(crgpu_buffer buffer) {
	// Nothing to unmap - see crgpu_buffer_map.
	if (!buffer)
		return;
	if (CR_STALE(buffer, "crgpu_buffer_unmap", "buffer"))
		return;
}

// A Shared-storage MTLBuffer is one piece of memory the CPU and the GPU both
// see, so there is nothing to flush out of or invalidate - only a Managed
// buffer has a second copy, and no kind uses one (see
// crgpumetal___buffer_resource_options). Both calls make the
// checks every backend makes and then have nothing to do.
CR_RESULT crgpu_buffer_flush(
    crgpu_buffer buffer,
    uint64_t offset,
    uint64_t size) {
	if (!buffer)
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (CR_STALE(buffer, "crgpu_buffer_flush", "buffer"))
		return CR_RESULT_GPU_INVALID_BUFFER;

	crgpu_buffer_metal *buffer_metal = (crgpu_buffer_metal *)buffer;
	if (buffer_metal->_desc.memory_kind != CRGPU_MEMORY_KIND_UPLOAD &&
	    buffer_metal->_desc.memory_kind != CRGPU_MEMORY_KIND_DEVICE_MAPPABLE)
		return CR_RESULT_GPU_INVALID_MEMORY_KIND;
	if (offset + size > buffer_metal->_desc.size)
		return CR_RESULT_GPU_MAP_OUT_OF_RANGE;
	return CR_SUCCESS;
}

CR_RESULT crgpu_buffer_invalidate(
    crgpu_buffer buffer,
    uint64_t offset,
    uint64_t size) {
	if (!buffer)
		return CR_RESULT_GPU_INVALID_BUFFER;
	if (CR_STALE(buffer, "crgpu_buffer_invalidate", "buffer"))
		return CR_RESULT_GPU_INVALID_BUFFER;

	crgpu_buffer_metal *buffer_metal = (crgpu_buffer_metal *)buffer;
	if (buffer_metal->_desc.memory_kind != CRGPU_MEMORY_KIND_READBACK &&
	    buffer_metal->_desc.memory_kind != CRGPU_MEMORY_KIND_DEVICE_MAPPABLE)
		return CR_RESULT_GPU_INVALID_MEMORY_KIND;
	if (offset + size > buffer_metal->_desc.size)
		return CR_RESULT_GPU_MAP_OUT_OF_RANGE;
	return CR_SUCCESS;
}


#endif // CRGPU_COMPILE_METAL_API
