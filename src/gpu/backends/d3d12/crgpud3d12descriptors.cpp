
#if CRGPU_COMPILE_D3D12_API

#include "crgpud3d12.hpp"
#include <cstring>
#include "../../../crstructbase.h"

// Maps CRGPU_DESCRIPTOR_TYPE to D3D12_DESCRIPTOR_RANGE_TYPE.
// Returns -1 for sampler (handled separately) and -2 for unknown.
static int crgpu_d3d12_descriptor_range_type(CRGPU_DESCRIPTOR_TYPE type) {
	switch (type) {
	case CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER:       return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
	case CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER:       return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	case CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE:        return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	case CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV; // SRV half; sampler tracked separately
	case CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE:        return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	case CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	case CRGPU_DESCRIPTOR_TYPE_SAMPLER:              return -1; // sampler heap
	default:                                           return -2;
	}
}

static bool crgpu_d3d12_is_sampler_type(CRGPU_DESCRIPTOR_TYPE type) {
	return type == CRGPU_DESCRIPTOR_TYPE_SAMPLER ||
	       type == CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

static D3D12_TEXTURE_ADDRESS_MODE crgpu_d3d12_address_mode(CRGPU_ADDRESS_MODE mode) {
	switch (mode) {
	case CRGPU_ADDRESS_MODE_MIRRORED_REPEAT: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
	case CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE:   return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	case CRGPU_ADDRESS_MODE_CLAMP_TO_BORDER: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	case CRGPU_ADDRESS_MODE_REPEAT:
	default:                                   return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	}
}

// D3D12_FILTER packs min/mag/mip into one enum rather than three independent
// values — translate the three CRGPU_FILTER fields into the matching named
// combination (anisotropic wins over all three when max_anisotropy > 1).
static D3D12_FILTER crgpu_d3d12_filter(
    CRGPU_FILTER min_filter, CRGPU_FILTER mag_filter, CRGPU_FILTER mip_filter, float max_anisotropy) {
	if (max_anisotropy > 1.0f)
		return D3D12_FILTER_ANISOTROPIC;

	bool minL = min_filter == CRGPU_FILTER_LINEAR;
	bool magL = mag_filter == CRGPU_FILTER_LINEAR;
	bool mipL = mip_filter == CRGPU_FILTER_LINEAR;

	if (!minL && !magL && !mipL) return D3D12_FILTER_MIN_MAG_MIP_POINT;
	if (!minL && !magL &&  mipL) return D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
	if (!minL &&  magL && !mipL) return D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
	if (!minL &&  magL &&  mipL) return D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
	if ( minL && !magL && !mipL) return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
	if ( minL && !magL &&  mipL) return D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
	if ( minL &&  magL && !mipL) return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
}

// Frees the pool's sets from the head of its list down to, but not
// including, `until`. Sets are pushed at the head, so passing the head a
// call started from frees exactly the sets that call allocated; nullptr
// frees them all.
static void crgpu_d3d12___free_descriptor_sets(
    crgpu_descriptor_pool_d3d12 *pool, crgpu_descriptor_set_d3d12 *until) {
	while (pool->_sets != until) {
		crgpu_descriptor_set_d3d12 *set = pool->_sets;
		pool->_sets                       = set->_next;
		CR_KILL(set);
		const CR_ALLOCATOR allocator    = set->head.allocator;
		cr_string_free(&allocator, set->head.debug_name);
		CR_RETIRE(CR_QUARANTINE_OF(set), &allocator, crgpu_descriptor_set_d3d12, set);
	}
}

// ============================================================================
//  The device's shader-visible heaps
// ============================================================================

// Makes [heap] a shader-visible heap of [type] holding [capacity]
// descriptors, all unused, its free list made through [allocator] (the
// device's). Leaves it empty on failure.
static CR_RESULT crgpu_d3d12___create_shader_heap(
    ID3D12Device *d3dDevice,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    uint32_t capacity,
    crgpu_d3d12_shader_heap &heap) {
	const crgpu_d3d12_heap_range whole = {0, capacity};
	if (eq_array_push(&heap.free_ranges, &whole) != EQ_SUCCESS)
		return CR_RESULT_OUT_OF_MEMORY;
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type                       = type;
	heapDesc.NumDescriptors             = capacity;
	heapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	heapDesc.NodeMask                   = 0;
	if (FAILED(d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap.heap)))) {
		eq_array_free(&heap.free_ranges);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	heap.increment = d3dDevice->GetDescriptorHandleIncrementSize(type);
	return CR_SUCCESS;
}

// Releases [heap] and its free list, leaving it empty.
static void crgpu_d3d12___release_shader_heap(crgpu_d3d12_shader_heap &heap) {
	heap.heap.Reset();
	eq_array_free(&heap.free_ranges);
}

CR_RESULT crgpu_d3d12___create_shader_heaps(
    crgpu_device_d3d12 *device,
    uint32_t max_descriptors,
    uint32_t max_samplers) {
	ID3D12Device *d3dDevice = device->_d3d_device.Get();
	CR_RESULT sr           = CR_SUCCESS;
	// A budget of 0 is no heap: D3D12 makes no heap of 0 descriptors, and
	// every pool that asks for one of that kind is refused.
	if (max_descriptors > 0)
		sr = crgpu_d3d12___create_shader_heap(d3dDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, max_descriptors, device->_csv_heap);
	if (sr != CR_SUCCESS)
		return sr;
	if (max_samplers > 0)
		sr = crgpu_d3d12___create_shader_heap(d3dDevice, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, max_samplers, device->_sampler_heap);
	if (sr != CR_SUCCESS) {
		// Both or neither.
		crgpu_d3d12___release_shader_heap(device->_csv_heap);
		return sr;
	}
	return CR_SUCCESS;
}

// Takes [count] descriptors of [heap], first fit. A count of 0 takes
// nothing and gives offset 0. The caller holds device->_shader_heap_mutex.
static CR_RESULT crgpu_d3d12___heap_take(crgpu_d3d12_shader_heap &heap, uint32_t count, uint32_t *out_offset) {
	*out_offset = 0;
	if (count == 0)
		return CR_SUCCESS;
	for (size_t i = 0; i < heap.free_ranges.count; ++i) {
		crgpu_d3d12_heap_range &range = heap.free_ranges.data[i];
		if (range.count < count)
			continue;
		*out_offset = range.offset;
		range.offset += count;
		range.count -= count;
		// An index inside the array, so the removal can't fail.
		if (range.count == 0)
			(void)eq_array_remove(&heap.free_ranges, i, 1);
		return CR_SUCCESS;
	}
	// The device's budget, which is this heap, has no run of [count]
	// descriptors left: too few, or what destroyed pools gave back is in
	// pieces (crgpu.h allows the latter on every backend). No heap at all
	// (a budget of 0) has no free range either.
	return CR_RESULT_GPU_OUT_OF_DESCRIPTORS;
}

// Gives [count] descriptors at [offset] back to [heap], merged with the
// unused ranges either side; a longer free list is made through the device's
// allocator, which the list holds. The caller holds
// device->_shader_heap_mutex.
static void crgpu_d3d12___heap_give_back(
    crgpu_d3d12_shader_heap &heap,
    uint32_t offset,
    uint32_t count) {
	if (count == 0)
		return;
	crgpu_d3d12_heap_ranges &ranges = heap.free_ranges;
	// The first unused range after the one given back.
	size_t next = 0;
	while (next < ranges.count && ranges.data[next].offset < offset)
		++next;
	const bool joins_previous = next > 0 && ranges.data[next - 1].offset + ranges.data[next - 1].count == offset;
	const bool joins_next     = next < ranges.count && offset + count == ranges.data[next].offset;
	if (joins_previous && joins_next) {
		ranges.data[next - 1].count += count + ranges.data[next].count;
		// An index inside the array, so the removal can't fail.
		(void)eq_array_remove(&ranges, next, 1);
		return;
	}
	if (joins_previous) {
		ranges.data[next - 1].count += count;
		return;
	}
	if (joins_next) {
		ranges.data[next].offset = offset;
		ranges.data[next].count += count;
		return;
	}
	// Out of memory: the list is untouched and the range stays taken. The
	// heap is smaller from now on, and nothing else is wrong.
	const crgpu_d3d12_heap_range given = {offset, count};
	(void)eq_array_insert(&ranges, next, &given, 1);
}

// Takes [csv_count] CBV/SRV/UAV and [sampler_count] sampler descriptors of
// the device's shader-visible heaps: both or neither.
static CR_RESULT crgpu_d3d12___take_shader_heap_ranges(
    crgpu_device_d3d12 *device,
    uint32_t csv_count,
    uint32_t sampler_count,
    uint32_t *out_csv_offset,
    uint32_t *out_sampler_offset) {
	std::lock_guard<std::mutex> lock(device->_shader_heap_mutex);
	CR_RESULT sr = crgpu_d3d12___heap_take(device->_csv_heap, csv_count, out_csv_offset);
	if (sr != CR_SUCCESS)
		return sr;
	sr = crgpu_d3d12___heap_take(device->_sampler_heap, sampler_count, out_sampler_offset);
	if (sr != CR_SUCCESS) {
		crgpu_d3d12___heap_give_back(device->_csv_heap, *out_csv_offset, csv_count);
		return sr;
	}
	return CR_SUCCESS;
}

static void crgpu_d3d12___give_back_shader_heap_ranges(
    crgpu_device_d3d12 *device,
    uint32_t csv_offset,
    uint32_t csv_count,
    uint32_t sampler_offset,
    uint32_t sampler_count) {
	std::lock_guard<std::mutex> lock(device->_shader_heap_mutex);
	crgpu_d3d12___heap_give_back(device->_csv_heap, csv_offset, csv_count);
	crgpu_d3d12___heap_give_back(device->_sampler_heap, sampler_offset, sampler_count);
}

// Descriptor [index] of [heap], for the CPU to write and for a root table.
static D3D12_CPU_DESCRIPTOR_HANDLE crgpu_d3d12___cpu_handle(const crgpu_d3d12_shader_heap &heap, uint32_t index) {
	D3D12_CPU_DESCRIPTOR_HANDLE handle = heap.heap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += (SIZE_T)index * heap.increment;
	return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE crgpu_d3d12___gpu_handle(const crgpu_d3d12_shader_heap &heap, uint32_t index) {
	D3D12_GPU_DESCRIPTOR_HANDLE handle = heap.heap->GetGPUDescriptorHandleForHeapStart();
	handle.ptr += (UINT64)index * heap.increment;
	return handle;
}

// Sets the device's shader-visible heaps on [cmd_list]. Every call that
// binds descriptor tables sets the same ones, so a table bound earlier stays
// in a heap that is set. A heap of a budget of 0 doesn't exist, and nothing
// can be bound from it.
static void crgpu_d3d12___set_shader_heaps(ID3D12GraphicsCommandList *cmd_list, crgpu_device_d3d12 *device) {
	ID3D12DescriptorHeap *heaps[2];
	UINT count = 0;
	if (device->_csv_heap.heap)
		heaps[count++] = device->_csv_heap.heap.Get();
	if (device->_sampler_heap.heap)
		heaps[count++] = device->_sampler_heap.heap.Get();
	if (count)
		cmd_list->SetDescriptorHeaps(count, heaps);
}

extern "C" {

CR_RESULT crgpu_sampler_create(
    crgpu_device device,
    const crgpu_sampler_desc *desc,
    crgpu_sampler *out_sampler) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_sampler_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_SAMPLER_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_sampler)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	crgpu_sampler_d3d12 *pResult =
	    CR_NEW_ZEROED(&device->head.allocator, crgpu_sampler_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult->head.allocator   = device->head.allocator;
	CR_BIRTH_IN(pResult, &device->_quarantine);
	pResult->_d3d_desc.Filter = crgpu_d3d12_filter(
	    desc->min_filter, desc->mag_filter, desc->mipmap_filter, desc->max_anisotropy);
	pResult->_d3d_desc.AddressU       = crgpu_d3d12_address_mode(desc->address_mode_u);
	pResult->_d3d_desc.AddressV       = crgpu_d3d12_address_mode(desc->address_mode_v);
	pResult->_d3d_desc.AddressW       = crgpu_d3d12_address_mode(desc->address_mode_w);
	pResult->_d3d_desc.MipLODBias     = desc->mip_lod_bias;
	pResult->_d3d_desc.MaxAnisotropy  = desc->max_anisotropy > 1.0f ? (UINT)desc->max_anisotropy : 1;
	// A comparison sampler is the same filter with the comparison
	// reduction (D3D12_FILTER_COMPARISON_* is the basic filter plus 0x80),
	// and D3D12's comparison functions are Carrottus GPU's plus one.
	if (desc->compare_enable) {
		pResult->_d3d_desc.Filter         = (D3D12_FILTER)(pResult->_d3d_desc.Filter | 0x80);
		pResult->_d3d_desc.ComparisonFunc = (D3D12_COMPARISON_FUNC)(desc->compare_op + 1);
	} else {
		pResult->_d3d_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NONE;
	}
	pResult->_d3d_desc.MinLOD         = desc->min_lod;
	pResult->_d3d_desc.MaxLOD         = desc->max_lod;
	pResult->_device                  = device;

	// From here crgpu_sampler_destroy frees it, and takes the count back.
	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_SAMPLER);
	if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
		crgpu_sampler_destroy(pResult);
		return CR_RESULT_OUT_OF_MEMORY;
	}

	*out_sampler = pResult;
	return CR_SUCCESS;
}

void crgpu_sampler_destroy(crgpu_sampler sampler) {
	if (!sampler)
		return;
	if (CR_STALE(sampler, "crgpu_sampler_destroy", "sampler"))
		return;
	CRGPU___LIVE_REMOVE(&sampler->_device->_live, CRGPU___OBJECT_SAMPLER);
	CR_KILL(sampler);
	const CR_ALLOCATOR allocator = sampler->head.allocator;
	cr_string_free(&allocator, sampler->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(sampler), &allocator, crgpu_sampler_d3d12, sampler);
}

CR_RESULT crgpu_descriptor_set_layout_create(
    crgpu_device device,
    const crgpu_descriptor_set_layout_desc *desc,
    crgpu_descriptor_set_layout *out_layout) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_descriptor_set_layout_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_layout)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (desc->binding_count > CRGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET)
		return CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_BINDINGS;
	// An unknown type, or one the device has no feature for, the same way
	// on every backend.
	for (uint32_t i = 0; i < desc->binding_count; ++i) {
		const CR_RESULT type_sr = crgpu___internal_check_descriptor_type(desc->bindings[i].descriptor_type, device->_features);
		if (type_sr != CR_SUCCESS)
			return type_sr;
	}

	crgpu_descriptor_set_layout_d3d12 *pResult =
	    CR_NEW_ZEROED(&device->head.allocator, crgpu_descriptor_set_layout_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult->head.allocator = device->head.allocator;
	CR_BIRTH_IN(pResult, &device->_quarantine);
	pResult->_device        = device;
	pResult->_desc          = *desc;

	// Pre-compute per-binding offsets into the set's heap range.
	uint32_t csv_cursor     = 0;
	uint32_t sampler_cursor = 0;
	for (uint32_t i = 0; i < desc->binding_count; ++i) {
		const crgpu_descriptor_binding_desc &b = desc->bindings[i];
		crgpu_d3d12_binding_slot &slot         = pResult->_slots[i];

		bool needsSampler = crgpu_d3d12_is_sampler_type(b.descriptor_type);
		bool needsCsv     = (b.descriptor_type != CRGPU_DESCRIPTOR_TYPE_SAMPLER);

		if (needsCsv) {
			slot.cbv_srv_uav_offset = csv_cursor;
			csv_cursor += b.count;
		}
		if (needsSampler) {
			slot.sampler_offset = sampler_cursor;
			sampler_cursor += b.count;
		}
		slot.is_sampler = (b.descriptor_type == CRGPU_DESCRIPTOR_TYPE_SAMPLER);
	}
	pResult->_cbv_srv_uav_count = csv_cursor;
	pResult->_sampler_count     = sampler_cursor;

	CRGPU___LIVE_ADD(&pResult->_device->_live, CRGPU___OBJECT_DESCRIPTOR_SET_LAYOUT);
	if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
		crgpu_descriptor_set_layout_destroy(pResult);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	pResult->_desc.debug_name = pResult->head.debug_name;

	*out_layout = pResult;
	return CR_SUCCESS;
}

void crgpu_descriptor_set_layout_destroy(crgpu_descriptor_set_layout layout) {
	if (!layout)
		return;
	if (CR_STALE(layout, "crgpu_descriptor_set_layout_destroy", "layout"))
		return;
	CRGPU___LIVE_REMOVE(&layout->_device->_live, CRGPU___OBJECT_DESCRIPTOR_SET_LAYOUT);
	CR_KILL(layout);
	const CR_ALLOCATOR allocator = layout->head.allocator;
	cr_string_free(&allocator, layout->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(layout), &allocator, crgpu_descriptor_set_layout_d3d12, layout);
}

CR_RESULT crgpu_descriptor_pool_create(
    crgpu_device device,
    const crgpu_descriptor_pool_desc *desc,
    crgpu_descriptor_pool *out_pool) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_descriptor_pool_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_pool)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (desc->pool_size_count > CRGPU_MAX_DESCRIPTOR_POOL_SIZES)
		return CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_POOLS;
	for (uint32_t i = 0; i < desc->pool_size_count; ++i) {
		const CR_RESULT type_sr = crgpu___internal_check_descriptor_type(desc->pool_sizes[i].descriptor_type, device->_features);
		if (type_sr != CR_SUCCESS)
			return type_sr;
	}

	// What the pool takes out of the device's budget, which on D3D12 is its
	// shader-visible heaps: the same count on every backend.
	uint64_t descriptors = 0;
	uint64_t samplers    = 0;
	crgpu___internal_pool_descriptor_counts(desc, &descriptors, &samplers);
	// No heap is larger than a budget, which is at most UINT32_MAX.
	if (descriptors > UINT32_MAX || samplers > UINT32_MAX)
		return CR_RESULT_GPU_OUT_OF_DESCRIPTORS;
	const uint32_t csvCapacity     = (uint32_t)descriptors;
	const uint32_t samplerCapacity = (uint32_t)samplers;
	uint32_t csvOffset             = 0;
	uint32_t samplerOffset         = 0;
	CR_RESULT sr = crgpu_d3d12___take_shader_heap_ranges(device, csvCapacity, samplerCapacity, &csvOffset, &samplerOffset);
	if (sr != CR_SUCCESS)
		return sr;

	crgpu_descriptor_pool_d3d12 *pResult =
	    CR_NEW(&device->head.allocator, crgpu_descriptor_pool_d3d12);
	if (!pResult) {
		crgpu_d3d12___give_back_shader_heap_ranges(device, csvOffset, csvCapacity, samplerOffset, samplerCapacity);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	pResult                        = new (pResult) crgpu_descriptor_pool_d3d12();
	pResult->head.allocator        = device->head.allocator;
	CR_BIRTH_IN(pResult, &device->_quarantine);
	pResult->_device               = device;
	pResult->_desc                 = *desc;
	pResult->_cbv_srv_uav_capacity = csvCapacity;
	pResult->_sampler_capacity     = samplerCapacity;
	pResult->_cbv_srv_uav_cursor   = 0;
	pResult->_sampler_cursor       = 0;
	// Ranges of the device's shader-visible heaps, not heaps of its own:
	// see crgpu_d3d12_shader_heap.
	pResult->_cbv_srv_uav_offset = csvOffset;
	pResult->_sampler_offset     = samplerOffset;

	// From here crgpu_descriptor_pool_destroy frees it, gives its ranges
	// back and takes the count back.
	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_DESCRIPTOR_POOL);
	if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
		crgpu_descriptor_pool_destroy(pResult);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	pResult->_desc.debug_name = pResult->head.debug_name;

	*out_pool = pResult;
	return CR_SUCCESS;
}

void crgpu_descriptor_pool_reset(crgpu_descriptor_pool pool) {
	if (!pool)
		return;
	if (CR_STALE(pool, "crgpu_descriptor_pool_reset", "pool"))
		return;
	// Release every set and reset the allocation cursors; existing
	// descriptor set handles become invalid.
	crgpu_d3d12___free_descriptor_sets(pool, nullptr);
	pool->_cbv_srv_uav_cursor = 0;
	pool->_sampler_cursor     = 0;
}

void crgpu_descriptor_pool_destroy(crgpu_descriptor_pool pool) {
	if (!pool)
		return;
	if (CR_STALE(pool, "crgpu_descriptor_pool_destroy", "pool"))
		return;
	crgpu_descriptor_pool_d3d12 *p = (crgpu_descriptor_pool_d3d12 *)pool;
	crgpu_d3d12___free_descriptor_sets(p, nullptr);
	crgpu_d3d12___give_back_shader_heap_ranges(
	    p->_device, p->_cbv_srv_uav_offset, p->_cbv_srv_uav_capacity, p->_sampler_offset, p->_sampler_capacity);
	CRGPU___LIVE_REMOVE(&p->_device->_live, CRGPU___OBJECT_DESCRIPTOR_POOL);
	CR_KILL(pool);
	const CR_ALLOCATOR allocator = p->head.allocator;
	cr_string_free(&allocator, p->head.debug_name);
	CR_QUARANTINE_KEEP(quarantine, p);
	p->~crgpu_descriptor_pool_d3d12();
	CR_RETIRE(quarantine, &allocator, crgpu_descriptor_pool_d3d12, p);
}

CR_RESULT crgpu_descriptor_set_create(
    crgpu_device device,
    const crgpu_descriptor_set_desc *desc,
    crgpu_descriptor_set *out_sets) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_descriptor_set_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_sets)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!desc->pool)
		return CR_RESULT_GPU_INVALID_DESCRIPTOR_POOL;
	if (!desc->set_count)
		return CR_RESULT_GPU_ZERO_DESCRIPTOR_SET_LAYOUTS;
	if (desc->set_count > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_SET_LAYOUTS;

	crgpu_descriptor_pool_d3d12 *pool =
	    (crgpu_descriptor_pool_d3d12 *)desc->pool;

	// The pool as this call found it. A failure part-way through puts it
	// back: the sets this call allocated are freed and their heap ranges
	// returned, so the call allocates every set or none.
	crgpu_descriptor_set_d3d12 *firstSet = pool->_sets;
	uint32_t firstCsvCursor                = pool->_cbv_srv_uav_cursor;
	uint32_t firstSamplerCursor            = pool->_sampler_cursor;
	CR_RESULT sr                          = CR_SUCCESS;

	for (uint32_t i = 0; i < desc->set_count; ++i) {
		if (!desc->set_layouts[i]) {
			sr = CR_RESULT_NULL_DESC;
			break;
		}

		crgpu_descriptor_set_layout_d3d12 *layout =
		    (crgpu_descriptor_set_layout_d3d12 *)desc->set_layouts[i];

		// Check pool has capacity.
		if (pool->_cbv_srv_uav_cursor + layout->_cbv_srv_uav_count >
		    pool->_cbv_srv_uav_capacity) {
			sr = CR_RESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL;
			break;
		}
		if (pool->_sampler_cursor + layout->_sampler_count >
		    pool->_sampler_capacity) {
			sr = CR_RESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL;
			break;
		}

		crgpu_descriptor_set_d3d12 *pSet =
		    CR_NEW_ZEROED(&pool->head.allocator, crgpu_descriptor_set_d3d12);
		if (!pSet) {
			sr = CR_RESULT_OUT_OF_MEMORY;
			break;
		}
		pSet->head.allocator    = pool->head.allocator;
		CR_BIRTH_IN(pSet, &pool->_device->_quarantine);
		pSet->_pool             = pool;
		pSet->_layout           = layout;
		pSet->_cbv_srv_uav_base = pool->_cbv_srv_uav_cursor;
		pSet->_sampler_base     = pool->_sampler_cursor;
		pSet->_next             = pool->_sets;
		pool->_sets             = pSet;

		pool->_cbv_srv_uav_cursor += layout->_cbv_srv_uav_count;
		pool->_sampler_cursor     += layout->_sampler_count;

		out_sets[i] = (crgpu_descriptor_set)pSet;
	}

	if (sr != CR_SUCCESS) {
		crgpu_d3d12___free_descriptor_sets(pool, firstSet);
		pool->_cbv_srv_uav_cursor = firstCsvCursor;
		pool->_sampler_cursor     = firstSamplerCursor;
		for (uint32_t i = 0; i < desc->set_count; ++i)
			out_sets[i] = nullptr;
		return sr;
	}

	return CR_SUCCESS;
}

void crgpu_device_update_descriptor_sets(
    crgpu_device device,
    const crgpu_write_descriptor_set *writes,
    uint32_t write_count) {
	if (!device || !writes || !write_count)
		return;
	if (CR_STALE(device, "crgpu_device_update_descriptor_sets", "device"))
		return;
	for (uint32_t writes_i = 0; writes_i < write_count; ++writes_i)
		if (cr_struct_check(&writes[writes_i].base, CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET) != CR_SUCCESS)
			return;
	// A storage view of a cube is a 2D array view of its faces on every
	// backend (crgpu.h): D3D12 has no storage cube. Checked before the
	// first write, so a refused call writes nothing.
	for (uint32_t writes_i = 0; writes_i < write_count; ++writes_i) {
		const crgpu_write_descriptor_set *write = &writes[writes_i];
		if (write->descriptor_type != CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE || !write->image_info)
			continue;
		for (uint32_t k = 0; k < write->descriptor_count; ++k) {
			const crgpu_image_view view = write->image_info[k].image_view;
			if (view && (view->_desc.type == CRGPU_IMAGE_VIEW_TYPE_CUBE || view->_desc.type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY))
				return;
		}
	}
	// Acceleration structure writes: the feature, and every element a live
	// top level (crgpu.h), checked the same way on every backend.
	if (!crgpu___internal_check_acceleration_structure_writes(device->_features, writes, write_count))
		return;

	ID3D12Device *d3dDevice = device->_d3d_device.Get();

	for (uint32_t w = 0; w < write_count; ++w) {
		const crgpu_write_descriptor_set &wr = writes[w];
		if (!wr.dst_set)
			continue;

		crgpu_descriptor_set_d3d12 *set =
		    (crgpu_descriptor_set_d3d12 *)wr.dst_set;
		crgpu_descriptor_set_layout_d3d12 *layout = set->_layout;
		crgpu_descriptor_pool_d3d12 *pool         = set->_pool;

		// Find the binding slot index in the layout matching dst_binding.
		uint32_t slotIdx = UINT32_MAX;
		for (uint32_t b = 0; b < layout->_desc.binding_count; ++b) {
			if (layout->_desc.bindings[b].binding == wr.dst_binding) {
				slotIdx = b;
				break;
			}
		}
		if (slotIdx == UINT32_MAX)
			continue;

		const crgpu_d3d12_binding_slot &slot = layout->_slots[slotIdx];
		CRGPU_DESCRIPTOR_TYPE dtype          = wr.descriptor_type;

		// Write CBV/SRV/UAV descriptors.
		if (dtype != CRGPU_DESCRIPTOR_TYPE_SAMPLER && pool->_cbv_srv_uav_capacity) {
			uint32_t baseSlot = pool->_cbv_srv_uav_offset +
			                    set->_cbv_srv_uav_base +
			                    slot.cbv_srv_uav_offset +
			                    wr.dst_array_element;

			for (uint32_t d = 0; d < wr.descriptor_count; ++d) {
				D3D12_CPU_DESCRIPTOR_HANDLE handle = crgpu_d3d12___cpu_handle(pool->_device->_csv_heap, baseSlot + d);

				if (dtype == CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
				    wr.buffer_info) {
					crgpu_buffer_d3d12 *buf =
					    (crgpu_buffer_d3d12 *)wr.buffer_info->buffer;
					D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
					cbvDesc.BufferLocation =
					    buf->_d3d_gpu_address + wr.buffer_info->offset;
					// CBV size must be 256-byte aligned.
					uint64_t range = wr.buffer_info->range
					                     ? wr.buffer_info->range
					                     : (buf->_desc.size - wr.buffer_info->offset);
					cbvDesc.SizeInBytes = (UINT)((range + 255) & ~255ULL);
					d3dDevice->CreateConstantBufferView(&cbvDesc, handle);

				} else if (dtype == CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
				           wr.buffer_info) {
					crgpu_buffer_d3d12 *buf =
					    (crgpu_buffer_d3d12 *)wr.buffer_info->buffer;
					uint64_t range = wr.buffer_info->range
					                     ? wr.buffer_info->range
					                     : (buf->_desc.size - wr.buffer_info->offset);
					D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
					uavDesc.Format                    = DXGI_FORMAT_R32_TYPELESS;
					uavDesc.ViewDimension             = D3D12_UAV_DIMENSION_BUFFER;
					uavDesc.Buffer.FirstElement       =
					    wr.buffer_info->offset / sizeof(uint32_t);
					uavDesc.Buffer.NumElements        = (UINT)(range / sizeof(uint32_t));
					uavDesc.Buffer.Flags              = D3D12_BUFFER_UAV_FLAG_RAW;
					d3dDevice->CreateUnorderedAccessView(
					    buf->_d3d_resource.Get(), nullptr, &uavDesc, handle);

				} else if ((dtype == CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
				            dtype == CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
				           wr.image_info) {
					crgpu_image_view_d3d12 *view =
					    (crgpu_image_view_d3d12 *)wr.image_info->image_view;
					D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc =
					    view->_d3d_srv;
					d3dDevice->CreateShaderResourceView(
					    view->_image->_d3d_resource.Get(), &srvDesc, handle);

				} else if (dtype == CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
				           wr.image_info) {
					crgpu_image_view_d3d12 *view =
					    (crgpu_image_view_d3d12 *)wr.image_info->image_view;
					D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc =
					    view->_d3d_uav;
					d3dDevice->CreateUnorderedAccessView(
					    view->_image->_d3d_resource.Get(), nullptr, &uavDesc, handle);
				} else if (dtype == CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE) {
					// A top level is an SRV of its address, with no
					// resource: D3D12's rule for acceleration structures.
					D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc          = {};
					srvDesc.Format                                   = DXGI_FORMAT_UNKNOWN;
					srvDesc.ViewDimension                            = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
					srvDesc.Shader4ComponentMapping                  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					srvDesc.RaytracingAccelerationStructure.Location = wr.acceleration_structures[d]->_d3d_gpu_address;
					d3dDevice->CreateShaderResourceView(nullptr, &srvDesc, handle);
				}
			}
		}

		// Write sampler descriptors (SAMPLER or COMBINED_IMAGE_SAMPLER).
		if (crgpu_d3d12_is_sampler_type(dtype) && wr.sampler && pool->_sampler_capacity) {
			uint32_t baseSlot = pool->_sampler_offset + set->_sampler_base + slot.sampler_offset + wr.dst_array_element;
			D3D12_CPU_DESCRIPTOR_HANDLE handle = crgpu_d3d12___cpu_handle(pool->_device->_sampler_heap, baseSlot);

			crgpu_sampler_d3d12 *sampler = (crgpu_sampler_d3d12 *)wr.sampler;
			d3dDevice->CreateSampler(&sampler->_d3d_desc, handle);
		}
	}
}

void crgpu_command_list_bind_graphics_descriptor_sets(
    crgpu_command_list cmd,
    crgpu_graphics_pipeline pipeline,
    uint32_t first_set,
    const crgpu_descriptor_set *sets,
    uint32_t set_count) {
	if (!cmd || !pipeline || !sets || !set_count)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_bind_graphics_descriptor_sets", "cmd"))
		return;
	if (CR_STALE(pipeline, "crgpu_command_list_bind_graphics_descriptor_sets", "pipeline"))
		return;
	if (set_count > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (first_set > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS - set_count)
		return;

	ID3D12GraphicsCommandList *cmdList = cmd->_d3d_cmd_list.Get();

	// Every pool's sets, and the bindless state, are in the device's two
	// shader-visible heaps, so setting them never moves a table bound before
	// out of a set heap, and sets of different pools can be bound together.
	crgpu_d3d12___set_shader_heaps(cmdList, cmd->_allocator->_device);

	// Each set has a table in each heap it uses, at the root parameters its
	// pipeline's root signature put them at (crgpu_d3d12_root_layout).
	const crgpu_d3d12_root_layout &root = pipeline->_root_layout;
	for (uint32_t i = 0; i < set_count; ++i) {
		crgpu_descriptor_set_d3d12 *set   = (crgpu_descriptor_set_d3d12 *)sets[i];
		crgpu_descriptor_pool_d3d12 *pool = set->_pool;
		const uint32_t s                    = first_set + i;
		if (root.cbv_srv_uav_param[s] != CRGPU_D3D12_NO_ROOT_PARAMETER && pool->_cbv_srv_uav_capacity)
			cmdList->SetGraphicsRootDescriptorTable(
			    root.cbv_srv_uav_param[s],
			    crgpu_d3d12___gpu_handle(pool->_device->_csv_heap, pool->_cbv_srv_uav_offset + set->_cbv_srv_uav_base));
		if (root.sampler_param[s] != CRGPU_D3D12_NO_ROOT_PARAMETER && pool->_sampler_capacity)
			cmdList->SetGraphicsRootDescriptorTable(
			    root.sampler_param[s],
			    crgpu_d3d12___gpu_handle(pool->_device->_sampler_heap, pool->_sampler_offset + set->_sampler_base));
	}
}

void crgpu_command_list_bind_compute_descriptor_sets(
    crgpu_command_list cmd,
    crgpu_compute_pipeline pipeline,
    uint32_t first_set,
    const crgpu_descriptor_set *sets,
    uint32_t set_count) {
	if (!cmd || !pipeline || !sets || !set_count)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_bind_compute_descriptor_sets", "cmd"))
		return;
	if (CR_STALE(pipeline, "crgpu_command_list_bind_compute_descriptor_sets", "pipeline"))
		return;
	if (set_count > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (first_set > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS - set_count)
		return;

	ID3D12GraphicsCommandList *cmdList = cmd->_d3d_cmd_list.Get();

	// As in crgpu_command_list_bind_graphics_descriptor_sets.
	crgpu_d3d12___set_shader_heaps(cmdList, cmd->_allocator->_device);

	const crgpu_d3d12_root_layout &root = pipeline->_root_layout;
	for (uint32_t i = 0; i < set_count; ++i) {
		crgpu_descriptor_set_d3d12 *set   = (crgpu_descriptor_set_d3d12 *)sets[i];
		crgpu_descriptor_pool_d3d12 *pool = set->_pool;
		const uint32_t s                    = first_set + i;
		if (root.cbv_srv_uav_param[s] != CRGPU_D3D12_NO_ROOT_PARAMETER && pool->_cbv_srv_uav_capacity)
			cmdList->SetComputeRootDescriptorTable(
			    root.cbv_srv_uav_param[s],
			    crgpu_d3d12___gpu_handle(pool->_device->_csv_heap, pool->_cbv_srv_uav_offset + set->_cbv_srv_uav_base));
		if (root.sampler_param[s] != CRGPU_D3D12_NO_ROOT_PARAMETER && pool->_sampler_capacity)
			cmdList->SetComputeRootDescriptorTable(
			    root.sampler_param[s],
			    crgpu_d3d12___gpu_handle(pool->_device->_sampler_heap, pool->_sampler_offset + set->_sampler_base));
	}
}

// ============================================================================
//  Bindless / Descriptor Indexing
// ============================================================================

// Made by crgpu_device_create when CRGPU_FEATURE_BINDLESS is asked for,
// before the device is handed out, and never replaced: every call after reads
// device->_bindless with no lock. crgpu_device_create checked that the table
// fits in the descriptor budget.
CR_RESULT crgpu_d3d12___create_bindless_state(crgpu_device_d3d12 *device) {
	// A range of the device's shader-visible CBV/SRV/UAV heap, which the
	// descriptor pools share, so bindless and pool sets bind together. Kept
	// for the device's life.
	uint32_t offset   = 0;
	uint32_t noOffset = 0;
	CR_RESULT sr     = crgpu_d3d12___take_shader_heap_ranges(device, 3 * CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS, 0, &offset, &noOffset);
	if (sr != CR_SUCCESS)
		return sr;

	// Sentinel layout: only binding_count/bindings[] matter — that's all
	// crgpu_d3d12_build_root_signature (crgpud3d12shader.cpp) reads to
	// generate the matching root-signature ranges when a caller includes
	// this handle in their own descriptor_set_layouts[]. Root signature
	// version 1.0 (what that builder already serializes with) treats every
	// range as DESCRIPTORS_VOLATILE | DATA_VOLATILE by default, so the
	// update-after-bind write pattern below is already valid with zero
	// changes to that shared helper.
	auto *layout = CR_NEW_ZEROED(&device->head.allocator, crgpu_descriptor_set_layout_d3d12);
	if (!layout) {
		crgpu_d3d12___give_back_shader_heap_ranges(device, offset, 3 * CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS, 0, 0);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	layout->head.allocator       = device->head.allocator;
	CR_BIRTH(layout);
	layout->_device              = device;
	layout->_desc.binding_count  = 3;

	CRGPU_SHADER_STAGE allStages = (CRGPU_SHADER_STAGE)(
	    CRGPU_SHADER_STAGE_VERTEX | CRGPU_SHADER_STAGE_FRAGMENT |
	    CRGPU_SHADER_STAGE_COMPUTE | CRGPU_SHADER_STAGE_GEOMETRY |
	    CRGPU_SHADER_STAGE_TESSELLATION_CONTROL |
	    CRGPU_SHADER_STAGE_TESSELLATION_EVALUATION);

	// HLSL's UAV ("u#") register space is shared between texture UAVs
	// (STORAGE_IMAGE) and buffer UAVs (STORAGE_BUFFER) -- unlike Vulkan,
	// where these are just independently numbered descriptor-set slots with
	// no register-namespace concept at all. crgpu_d3d12_build_root_signature
	// maps each binding's .binding field straight to BaseShaderRegister, so
	// with CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS-sized (4096) ranges, using
	// sequential binding numbers (1, 2) here made the two UAV ranges overlap
	// (u1..u4096 fully covers u2) -- D3D12SerializeRootSignature rejects
	// that outright. SAMPLED_IMAGE (SRV, "t#") doesn't need to make room for
	// either, since SRV and UAV are already separate register classes.
	layout->_desc.bindings[0] = {
	    0, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
	    CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS, allStages};
	layout->_desc.bindings[1] = {
	    0, CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	    CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS, allStages};
	layout->_desc.bindings[2] = {
	    CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS, CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	    CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS, allStages};

	// Both are released with the device (crgpud3d12context.cpp).
	crgpu_bindless_state_d3d12 *state =
	    CR_NEW(&device->head.allocator, crgpu_bindless_state_d3d12);
	if (!state) {
		CR_DELETE(&device->head.allocator, crgpu_descriptor_set_layout_d3d12, layout);
		crgpu_d3d12___give_back_shader_heap_ranges(device, offset, 3 * CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS, 0, 0);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	state                            = new (state) crgpu_bindless_state_d3d12();
	state->sampled_image_free_stack  = eq::array<uint32_t>(&device->_eq_allocator);
	state->storage_image_free_stack  = eq::array<uint32_t>(&device->_eq_allocator);
	state->storage_buffer_free_stack = eq::array<uint32_t>(&device->_eq_allocator);
	state->offset                    = offset;
	state->layout = layout;

	device->_bindless = state;
	return CR_SUCCESS;
}

// Called with the state's lock held.
static CR_RESULT crgpud3d12___bindless_alloc_index(
    uint32_t *next_unused,
    eq::array<uint32_t> &free_stack,
    uint32_t *out_index) {
	if (!free_stack.empty()) {
		*out_index = free_stack.data[free_stack.count - 1];
		// The last element, so the removal can't fail.
		(void)free_stack.remove(free_stack.count - 1);
		return CR_SUCCESS;
	}
	if (*next_unused >= CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS)
		return CR_RESULT_GPU_BINDLESS_OUT_OF_SLOTS;
	*out_index = (*next_unused)++;
	return CR_SUCCESS;
}

crgpu_descriptor_set_layout crgpu_device_get_bindless_descriptor_set_layout(crgpu_device device) {
	if (!device)
		return nullptr;
	if (CR_STALE(device, "crgpu_device_get_bindless_descriptor_set_layout", "device"))
		return nullptr;
	if (!device->_bindless)
		return nullptr;
	return (crgpu_descriptor_set_layout)device->_bindless->layout;
}

CR_RESULT crgpu_bindless_register_sampled_image(
    crgpu_device device,
    crgpu_image_view view,
    uint32_t *out_index) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_bindless_register_sampled_image", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!view)
		return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
	if (CR_STALE(view, "crgpu_bindless_register_sampled_image", "view"))
		return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
	if (!out_index)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!device->_bindless)
		return CR_RESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED;

	crgpu_bindless_state_d3d12 *state = device->_bindless;
	CR_RESULT sr;
	{
		std::lock_guard<std::mutex> slots_lock(state->lock);
		sr = crgpud3d12___bindless_alloc_index(
		    &state->sampled_image_next_unused, state->sampled_image_free_stack, out_index);
	}
	if (sr != CR_SUCCESS)
		return sr;

	crgpu_image_view_d3d12 *vView = (crgpu_image_view_d3d12 *)view;
	D3D12_CPU_DESCRIPTOR_HANDLE handle =
	    crgpu_d3d12___cpu_handle(device->_csv_heap, state->offset + *out_index); // class base 0

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = vView->_d3d_srv;
	device->_d3d_device->CreateShaderResourceView(
	    vView->_image->_d3d_resource.Get(), &srvDesc, handle);
	return CR_SUCCESS;
}

void crgpu_bindless_unregister_sampled_image(crgpu_device device, uint32_t index) {
	if (!device)
		return;
	if (CR_STALE(device, "crgpu_bindless_unregister_sampled_image", "device"))
		return;
	crgpu_bindless_state_d3d12 *state = device->_bindless;
	if (!state)
		return;
	if (index == CRGPU_BINDLESS_INVALID_INDEX)
		return;
	std::lock_guard<std::mutex> slots_lock(state->lock);
	// Out of memory: the slot isn't reused, and the table is a slot
	// smaller from now on.
	(void)state->sampled_image_free_stack.push(index);
}

CR_RESULT crgpu_bindless_register_storage_image(
    crgpu_device device,
    crgpu_image_view view,
    uint32_t *out_index) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_bindless_register_storage_image", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!view)
		return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
	if (CR_STALE(view, "crgpu_bindless_register_storage_image", "view"))
		return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
	if (!out_index)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!device->_bindless)
		return CR_RESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED;

	crgpu_bindless_state_d3d12 *state = device->_bindless;
	CR_RESULT sr;
	{
		std::lock_guard<std::mutex> slots_lock(state->lock);
		sr = crgpud3d12___bindless_alloc_index(
		    &state->storage_image_next_unused, state->storage_image_free_stack, out_index);
	}
	if (sr != CR_SUCCESS)
		return sr;

	crgpu_image_view_d3d12 *vView = (crgpu_image_view_d3d12 *)view;
	D3D12_CPU_DESCRIPTOR_HANDLE handle = crgpu_d3d12___cpu_handle(
	    device->_csv_heap, state->offset + CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS + *out_index); // class base 1

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = vView->_d3d_uav;
	device->_d3d_device->CreateUnorderedAccessView(
	    vView->_image->_d3d_resource.Get(), nullptr, &uavDesc, handle);
	return CR_SUCCESS;
}

void crgpu_bindless_unregister_storage_image(crgpu_device device, uint32_t index) {
	if (!device)
		return;
	if (CR_STALE(device, "crgpu_bindless_unregister_storage_image", "device"))
		return;
	crgpu_bindless_state_d3d12 *state = device->_bindless;
	if (!state)
		return;
	if (index == CRGPU_BINDLESS_INVALID_INDEX)
		return;
	std::lock_guard<std::mutex> slots_lock(state->lock);
	// Out of memory: the slot isn't reused, and the table is a slot
	// smaller from now on.
	(void)state->storage_image_free_stack.push(index);
}

CR_RESULT crgpu_bindless_register_storage_buffer(
    crgpu_device device,
    crgpu_buffer_view view,
    uint32_t *out_index) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_bindless_register_storage_buffer", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!view)
		return CR_RESULT_GPU_INVALID_BUFFER_VIEW;
	if (CR_STALE(view, "crgpu_bindless_register_storage_buffer", "view"))
		return CR_RESULT_GPU_INVALID_BUFFER_VIEW;
	if (!out_index)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!device->_bindless)
		return CR_RESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED;

	crgpu_bindless_state_d3d12 *state = device->_bindless;
	CR_RESULT sr;
	{
		std::lock_guard<std::mutex> slots_lock(state->lock);
		sr = crgpud3d12___bindless_alloc_index(
		    &state->storage_buffer_next_unused, state->storage_buffer_free_stack, out_index);
	}
	if (sr != CR_SUCCESS)
		return sr;

	crgpu_buffer_view_d3d12 *vView = (crgpu_buffer_view_d3d12 *)view;
	crgpu_buffer_d3d12 *vBuf       = vView->_buffer;

	// Raw byte-address UAV, matching crgpu_device_update_descriptor_sets'
	// STORAGE_BUFFER handling above — buffer views have no pre-built UAV
	// desc (CRGPU_D3D12_BUFFER_VIEW only carries VB/IB/CB/SO views).
	uint64_t range = vView->_desc.size
	                     ? vView->_desc.size
	                     : (vBuf->_desc.size - vView->_desc.offset_from_parent_buffer);

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format              = DXGI_FORMAT_R32_TYPELESS;
	uavDesc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.FirstElement = vView->_desc.offset_from_parent_buffer / sizeof(uint32_t);
	uavDesc.Buffer.NumElements  = (UINT)(range / sizeof(uint32_t));
	uavDesc.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;

	D3D12_CPU_DESCRIPTOR_HANDLE handle = crgpu_d3d12___cpu_handle(
	    device->_csv_heap, state->offset + 2 * CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS + *out_index); // class base 2

	device->_d3d_device->CreateUnorderedAccessView(
	    vBuf->_d3d_resource.Get(), nullptr, &uavDesc, handle);
	return CR_SUCCESS;
}

void crgpu_bindless_unregister_storage_buffer(crgpu_device device, uint32_t index) {
	if (!device)
		return;
	if (CR_STALE(device, "crgpu_bindless_unregister_storage_buffer", "device"))
		return;
	crgpu_bindless_state_d3d12 *state = device->_bindless;
	if (!state)
		return;
	if (index == CRGPU_BINDLESS_INVALID_INDEX)
		return;
	std::lock_guard<std::mutex> slots_lock(state->lock);
	// Out of memory: the slot isn't reused, and the table is a slot
	// smaller from now on.
	(void)state->storage_buffer_free_stack.push(index);
}

void crgpu_command_list_bind_graphics_bindless_resources(
    crgpu_command_list cmd,
    crgpu_graphics_pipeline pipeline,
    uint32_t set_index) {
	if (!cmd || !pipeline)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_bind_graphics_bindless_resources", "cmd"))
		return;
	if (CR_STALE(pipeline, "crgpu_command_list_bind_graphics_bindless_resources", "pipeline"))
		return;
	// The bindless set has only CBV/SRV/UAV descriptors: one table.
	if (set_index >= CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (pipeline->_root_layout.cbv_srv_uav_param[set_index] == CRGPU_D3D12_NO_ROOT_PARAMETER)
		return;
	crgpu_device_d3d12 *device = cmd->_allocator->_device;
	crgpu_bindless_state_d3d12 *state = device->_bindless;
	if (!state)
		return;

	ID3D12GraphicsCommandList *cmdList = cmd->_d3d_cmd_list.Get();

	// D3D12 allows one CBV_SRV_UAV heap set at a time. The bindless range and
	// every pool's are in the device's same two heaps, so binding pool sets
	// after this, or before, leaves this table in a heap that is set.
	crgpu_d3d12___set_shader_heaps(cmdList, device);
	cmdList->SetGraphicsRootDescriptorTable(
	    pipeline->_root_layout.cbv_srv_uav_param[set_index], crgpu_d3d12___gpu_handle(device->_csv_heap, state->offset));
}

void crgpu_command_list_bind_compute_bindless_resources(
    crgpu_command_list cmd,
    crgpu_compute_pipeline pipeline,
    uint32_t set_index) {
	if (!cmd || !pipeline)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_bind_compute_bindless_resources", "cmd"))
		return;
	if (CR_STALE(pipeline, "crgpu_command_list_bind_compute_bindless_resources", "pipeline"))
		return;
	// The bindless set has only CBV/SRV/UAV descriptors: one table.
	if (set_index >= CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (pipeline->_root_layout.cbv_srv_uav_param[set_index] == CRGPU_D3D12_NO_ROOT_PARAMETER)
		return;
	crgpu_device_d3d12 *device = cmd->_allocator->_device;
	crgpu_bindless_state_d3d12 *state = device->_bindless;
	if (!state)
		return;

	ID3D12GraphicsCommandList *cmdList = cmd->_d3d_cmd_list.Get();

	// As in crgpu_command_list_bind_graphics_bindless_resources.
	crgpu_d3d12___set_shader_heaps(cmdList, device);
	cmdList->SetComputeRootDescriptorTable(
	    pipeline->_root_layout.cbv_srv_uav_param[set_index], crgpu_d3d12___gpu_handle(device->_csv_heap, state->offset));
}

} // extern "C"

#endif // CRGPU_COMPILE_D3D12_API
