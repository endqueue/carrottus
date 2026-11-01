
#if SPUDGPU_COMPILE_D3D12_API

#include "spudgpud3d12.hpp"
#include <cstring>
#include "../../../spudstructbase.h"

// Maps SPUDGPU_DESCRIPTOR_TYPE to D3D12_DESCRIPTOR_RANGE_TYPE.
// Returns -1 for sampler (handled separately) and -2 for unknown.
static int spudgpu_d3d12_descriptor_range_type(SPUDGPU_DESCRIPTOR_TYPE type) {
	switch (type) {
	case SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER:       return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
	case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER:       return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	case SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE:        return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	case SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV; // SRV half; sampler tracked separately
	case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE:        return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	case SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	case SPUDGPU_DESCRIPTOR_TYPE_SAMPLER:              return -1; // sampler heap
	default:                                           return -2;
	}
}

static bool spudgpu_d3d12_is_sampler_type(SPUDGPU_DESCRIPTOR_TYPE type) {
	return type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLER ||
	       type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

static D3D12_TEXTURE_ADDRESS_MODE spudgpu_d3d12_address_mode(SPUDGPU_ADDRESS_MODE mode) {
	switch (mode) {
	case SPUDGPU_ADDRESS_MODE_MIRRORED_REPEAT: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
	case SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE:   return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	case SPUDGPU_ADDRESS_MODE_CLAMP_TO_BORDER: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	case SPUDGPU_ADDRESS_MODE_REPEAT:
	default:                                   return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	}
}

// D3D12_FILTER packs min/mag/mip into one enum rather than three independent
// values — translate the three SPUDGPU_FILTER fields into the matching named
// combination (anisotropic wins over all three when max_anisotropy > 1).
static D3D12_FILTER spudgpu_d3d12_filter(
    SPUDGPU_FILTER min_filter, SPUDGPU_FILTER mag_filter, SPUDGPU_FILTER mip_filter, float max_anisotropy) {
	if (max_anisotropy > 1.0f)
		return D3D12_FILTER_ANISOTROPIC;

	bool minL = min_filter == SPUDGPU_FILTER_LINEAR;
	bool magL = mag_filter == SPUDGPU_FILTER_LINEAR;
	bool mipL = mip_filter == SPUDGPU_FILTER_LINEAR;

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
static void spudgpu_d3d12___free_descriptor_sets(
    spudgpu_descriptor_pool_d3d12 *pool, spudgpu_descriptor_set_d3d12 *until) {
	while (pool->_sets != until) {
		spudgpu_descriptor_set_d3d12 *set = pool->_sets;
		pool->_sets                       = set->_next;
		SPUD_KILL(set);
		const SPUD_ALLOCATOR allocator    = set->head.allocator;
		spud_string_free(&allocator, set->head.debug_name);
		SPUD_DELETE(&allocator, spudgpu_descriptor_set_d3d12, set);
	}
}

// ============================================================================
//  The device's shader-visible heaps
// ============================================================================

// Makes [heap] a shader-visible heap of [type] holding [capacity]
// descriptors, all unused, its free list made through [allocator] (the
// device's). Leaves it empty on failure.
static SPUDRESULT spudgpu_d3d12___create_shader_heap(
    ID3D12Device *d3dDevice,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    uint32_t capacity,
    spudgpu_d3d12_shader_heap &heap) {
	const spudgpu_d3d12_heap_range whole = {0, capacity};
	if (eq_array_push(&heap.free_ranges, &whole) != EQ_SUCCESS)
		return SPUDRESULT_OUT_OF_MEMORY;
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type                       = type;
	heapDesc.NumDescriptors             = capacity;
	heapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	heapDesc.NodeMask                   = 0;
	if (FAILED(d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap.heap)))) {
		eq_array_free(&heap.free_ranges);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	heap.increment = d3dDevice->GetDescriptorHandleIncrementSize(type);
	return SPUD_SUCCESS;
}

// Releases [heap] and its free list, leaving it empty.
static void spudgpu_d3d12___release_shader_heap(spudgpu_d3d12_shader_heap &heap) {
	heap.heap.Reset();
	eq_array_free(&heap.free_ranges);
}

SPUDRESULT spudgpu_d3d12___create_shader_heaps(
    spudgpu_device_d3d12 *device,
    uint32_t max_descriptors,
    uint32_t max_samplers) {
	ID3D12Device *d3dDevice = device->_d3d_device.Get();
	SPUDRESULT sr           = SPUD_SUCCESS;
	// A budget of 0 is no heap: D3D12 makes no heap of 0 descriptors, and
	// every pool that asks for one of that kind is refused.
	if (max_descriptors > 0)
		sr = spudgpu_d3d12___create_shader_heap(d3dDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, max_descriptors, device->_csv_heap);
	if (sr != SPUD_SUCCESS)
		return sr;
	if (max_samplers > 0)
		sr = spudgpu_d3d12___create_shader_heap(d3dDevice, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, max_samplers, device->_sampler_heap);
	if (sr != SPUD_SUCCESS) {
		// Both or neither.
		spudgpu_d3d12___release_shader_heap(device->_csv_heap);
		return sr;
	}
	return SPUD_SUCCESS;
}

// Takes [count] descriptors of [heap], first fit. A count of 0 takes
// nothing and gives offset 0. The caller holds device->_shader_heap_mutex.
static SPUDRESULT spudgpu_d3d12___heap_take(spudgpu_d3d12_shader_heap &heap, uint32_t count, uint32_t *out_offset) {
	*out_offset = 0;
	if (count == 0)
		return SPUD_SUCCESS;
	for (size_t i = 0; i < heap.free_ranges.count; ++i) {
		spudgpu_d3d12_heap_range &range = heap.free_ranges.data[i];
		if (range.count < count)
			continue;
		*out_offset = range.offset;
		range.offset += count;
		range.count -= count;
		// An index inside the array, so the removal can't fail.
		if (range.count == 0)
			(void)eq_array_remove(&heap.free_ranges, i, 1);
		return SPUD_SUCCESS;
	}
	// The device's budget, which is this heap, has no run of [count]
	// descriptors left: too few, or what destroyed pools gave back is in
	// pieces (spudgpu.h allows the latter on every backend). No heap at all
	// (a budget of 0) has no free range either.
	return SPUDRESULT_GPU_OUT_OF_DESCRIPTORS;
}

// Gives [count] descriptors at [offset] back to [heap], merged with the
// unused ranges either side; a longer free list is made through the device's
// allocator, which the list holds. The caller holds
// device->_shader_heap_mutex.
static void spudgpu_d3d12___heap_give_back(
    spudgpu_d3d12_shader_heap &heap,
    uint32_t offset,
    uint32_t count) {
	if (count == 0)
		return;
	spudgpu_d3d12_heap_ranges &ranges = heap.free_ranges;
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
	const spudgpu_d3d12_heap_range given = {offset, count};
	(void)eq_array_insert(&ranges, next, &given, 1);
}

// Takes [csv_count] CBV/SRV/UAV and [sampler_count] sampler descriptors of
// the device's shader-visible heaps: both or neither.
static SPUDRESULT spudgpu_d3d12___take_shader_heap_ranges(
    spudgpu_device_d3d12 *device,
    uint32_t csv_count,
    uint32_t sampler_count,
    uint32_t *out_csv_offset,
    uint32_t *out_sampler_offset) {
	std::lock_guard<std::mutex> lock(device->_shader_heap_mutex);
	SPUDRESULT sr = spudgpu_d3d12___heap_take(device->_csv_heap, csv_count, out_csv_offset);
	if (sr != SPUD_SUCCESS)
		return sr;
	sr = spudgpu_d3d12___heap_take(device->_sampler_heap, sampler_count, out_sampler_offset);
	if (sr != SPUD_SUCCESS) {
		spudgpu_d3d12___heap_give_back(device->_csv_heap, *out_csv_offset, csv_count);
		return sr;
	}
	return SPUD_SUCCESS;
}

static void spudgpu_d3d12___give_back_shader_heap_ranges(
    spudgpu_device_d3d12 *device,
    uint32_t csv_offset,
    uint32_t csv_count,
    uint32_t sampler_offset,
    uint32_t sampler_count) {
	std::lock_guard<std::mutex> lock(device->_shader_heap_mutex);
	spudgpu_d3d12___heap_give_back(device->_csv_heap, csv_offset, csv_count);
	spudgpu_d3d12___heap_give_back(device->_sampler_heap, sampler_offset, sampler_count);
}

// Descriptor [index] of [heap], for the CPU to write and for a root table.
static D3D12_CPU_DESCRIPTOR_HANDLE spudgpu_d3d12___cpu_handle(const spudgpu_d3d12_shader_heap &heap, uint32_t index) {
	D3D12_CPU_DESCRIPTOR_HANDLE handle = heap.heap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += (SIZE_T)index * heap.increment;
	return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE spudgpu_d3d12___gpu_handle(const spudgpu_d3d12_shader_heap &heap, uint32_t index) {
	D3D12_GPU_DESCRIPTOR_HANDLE handle = heap.heap->GetGPUDescriptorHandleForHeapStart();
	handle.ptr += (UINT64)index * heap.increment;
	return handle;
}

// Sets the device's shader-visible heaps on [cmd_list]. Every call that
// binds descriptor tables sets the same ones, so a table bound earlier stays
// in a heap that is set. A heap of a budget of 0 doesn't exist, and nothing
// can be bound from it.
static void spudgpu_d3d12___set_shader_heaps(ID3D12GraphicsCommandList *cmd_list, spudgpu_device_d3d12 *device) {
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

SPUDRESULT spudgpu_create_sampler(
    spudgpu_device device,
    const spudgpu_sampler_desc *desc,
    spudgpu_sampler *out_sampler) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_sampler", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_SAMPLER_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_sampler)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;

	spudgpu_sampler_d3d12 *pResult =
	    SPUD_NEW_ZEROED(&device->head.allocator, spudgpu_sampler_d3d12);
	if (!pResult)
		return SPUDRESULT_OUT_OF_MEMORY;
	pResult->head.allocator   = device->head.allocator;
	SPUD_BIRTH(pResult);
	pResult->_d3d_desc.Filter = spudgpu_d3d12_filter(
	    desc->min_filter, desc->mag_filter, desc->mipmap_filter, desc->max_anisotropy);
	pResult->_d3d_desc.AddressU       = spudgpu_d3d12_address_mode(desc->address_mode_u);
	pResult->_d3d_desc.AddressV       = spudgpu_d3d12_address_mode(desc->address_mode_v);
	pResult->_d3d_desc.AddressW       = spudgpu_d3d12_address_mode(desc->address_mode_w);
	pResult->_d3d_desc.MipLODBias     = desc->mip_lod_bias;
	pResult->_d3d_desc.MaxAnisotropy  = desc->max_anisotropy > 1.0f ? (UINT)desc->max_anisotropy : 1;
	// A comparison sampler is the same filter with the comparison
	// reduction (D3D12_FILTER_COMPARISON_* is the basic filter plus 0x80),
	// and D3D12's comparison functions are SpudGPU's plus one.
	if (desc->compare_enable) {
		pResult->_d3d_desc.Filter         = (D3D12_FILTER)(pResult->_d3d_desc.Filter | 0x80);
		pResult->_d3d_desc.ComparisonFunc = (D3D12_COMPARISON_FUNC)(desc->compare_op + 1);
	} else {
		pResult->_d3d_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NONE;
	}
	pResult->_d3d_desc.MinLOD         = desc->min_lod;
	pResult->_d3d_desc.MaxLOD         = desc->max_lod;
	pResult->_device                  = device;

	// From here spudgpu_destroy_sampler frees it, and takes the count back.
	SPUDGPU___LIVE_ADD(&device->_live, SPUDGPU___OBJECT_SAMPLER);
	if (spud_debug_name_set(pResult, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_sampler(pResult);
		return SPUDRESULT_OUT_OF_MEMORY;
	}

	*out_sampler = pResult;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_sampler(spudgpu_sampler sampler) {
	if (!sampler)
		return;
	if (SPUD_STALE(sampler, "spudgpu_destroy_sampler", "sampler"))
		return;
	SPUDGPU___LIVE_REMOVE(&sampler->_device->_live, SPUDGPU___OBJECT_SAMPLER);
	SPUD_KILL(sampler);
	const SPUD_ALLOCATOR allocator = sampler->head.allocator;
	spud_string_free(&allocator, sampler->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_sampler_d3d12, sampler);
}

SPUDRESULT spudgpu_create_descriptor_set_layout(
    spudgpu_device device,
    const spudgpu_descriptor_set_layout_desc *desc,
    spudgpu_descriptor_set_layout *out_layout) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_descriptor_set_layout", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_layout)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (desc->binding_count > SPUDGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET)
		return SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_BINDINGS;
	// An unknown type, or one the device has no feature for, the same way
	// on every backend.
	for (uint32_t i = 0; i < desc->binding_count; ++i) {
		const SPUDRESULT type_sr = spudgpu___internal_check_descriptor_type(desc->bindings[i].descriptor_type, device->_features);
		if (type_sr != SPUD_SUCCESS)
			return type_sr;
	}

	spudgpu_descriptor_set_layout_d3d12 *pResult =
	    SPUD_NEW_ZEROED(&device->head.allocator, spudgpu_descriptor_set_layout_d3d12);
	if (!pResult)
		return SPUDRESULT_OUT_OF_MEMORY;
	pResult->head.allocator = device->head.allocator;
	SPUD_BIRTH(pResult);
	pResult->_device        = device;
	pResult->_desc          = *desc;

	// Pre-compute per-binding offsets into the set's heap range.
	uint32_t csv_cursor     = 0;
	uint32_t sampler_cursor = 0;
	for (uint32_t i = 0; i < desc->binding_count; ++i) {
		const spudgpu_descriptor_binding_desc &b = desc->bindings[i];
		spudgpu_d3d12_binding_slot &slot         = pResult->_slots[i];

		bool needsSampler = spudgpu_d3d12_is_sampler_type(b.descriptor_type);
		bool needsCsv     = (b.descriptor_type != SPUDGPU_DESCRIPTOR_TYPE_SAMPLER);

		if (needsCsv) {
			slot.cbv_srv_uav_offset = csv_cursor;
			csv_cursor += b.count;
		}
		if (needsSampler) {
			slot.sampler_offset = sampler_cursor;
			sampler_cursor += b.count;
		}
		slot.is_sampler = (b.descriptor_type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLER);
	}
	pResult->_cbv_srv_uav_count = csv_cursor;
	pResult->_sampler_count     = sampler_cursor;

	SPUDGPU___LIVE_ADD(&pResult->_device->_live, SPUDGPU___OBJECT_DESCRIPTOR_SET_LAYOUT);
	if (spud_debug_name_set(pResult, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_descriptor_set_layout(pResult);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	pResult->_desc.debug_name = pResult->head.debug_name;

	*out_layout = pResult;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_descriptor_set_layout(spudgpu_descriptor_set_layout layout) {
	if (!layout)
		return;
	if (SPUD_STALE(layout, "spudgpu_destroy_descriptor_set_layout", "layout"))
		return;
	SPUDGPU___LIVE_REMOVE(&layout->_device->_live, SPUDGPU___OBJECT_DESCRIPTOR_SET_LAYOUT);
	SPUD_KILL(layout);
	const SPUD_ALLOCATOR allocator = layout->head.allocator;
	spud_string_free(&allocator, layout->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_descriptor_set_layout_d3d12, layout);
}

SPUDRESULT spudgpu_create_descriptor_pool(
    spudgpu_device device,
    const spudgpu_descriptor_pool_desc *desc,
    spudgpu_descriptor_pool *out_pool) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_descriptor_pool", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_pool)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (desc->pool_size_count > SPUDGPU_MAX_DESCRIPTOR_POOL_SIZES)
		return SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_POOLS;
	for (uint32_t i = 0; i < desc->pool_size_count; ++i) {
		const SPUDRESULT type_sr = spudgpu___internal_check_descriptor_type(desc->pool_sizes[i].descriptor_type, device->_features);
		if (type_sr != SPUD_SUCCESS)
			return type_sr;
	}

	// What the pool takes out of the device's budget, which on D3D12 is its
	// shader-visible heaps: the same count on every backend.
	uint64_t descriptors = 0;
	uint64_t samplers    = 0;
	spudgpu___internal_pool_descriptor_counts(desc, &descriptors, &samplers);
	// No heap is larger than a budget, which is at most UINT32_MAX.
	if (descriptors > UINT32_MAX || samplers > UINT32_MAX)
		return SPUDRESULT_GPU_OUT_OF_DESCRIPTORS;
	const uint32_t csvCapacity     = (uint32_t)descriptors;
	const uint32_t samplerCapacity = (uint32_t)samplers;
	uint32_t csvOffset             = 0;
	uint32_t samplerOffset         = 0;
	SPUDRESULT sr = spudgpu_d3d12___take_shader_heap_ranges(device, csvCapacity, samplerCapacity, &csvOffset, &samplerOffset);
	if (sr != SPUD_SUCCESS)
		return sr;

	spudgpu_descriptor_pool_d3d12 *pResult =
	    SPUD_NEW(&device->head.allocator, spudgpu_descriptor_pool_d3d12);
	if (!pResult) {
		spudgpu_d3d12___give_back_shader_heap_ranges(device, csvOffset, csvCapacity, samplerOffset, samplerCapacity);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	pResult                        = new (pResult) spudgpu_descriptor_pool_d3d12();
	pResult->head.allocator        = device->head.allocator;
	SPUD_BIRTH(pResult);
	pResult->_device               = device;
	pResult->_desc                 = *desc;
	pResult->_cbv_srv_uav_capacity = csvCapacity;
	pResult->_sampler_capacity     = samplerCapacity;
	pResult->_cbv_srv_uav_cursor   = 0;
	pResult->_sampler_cursor       = 0;
	// Ranges of the device's shader-visible heaps, not heaps of its own:
	// see spudgpu_d3d12_shader_heap.
	pResult->_cbv_srv_uav_offset = csvOffset;
	pResult->_sampler_offset     = samplerOffset;

	// From here spudgpu_destroy_descriptor_pool frees it, gives its ranges
	// back and takes the count back.
	SPUDGPU___LIVE_ADD(&device->_live, SPUDGPU___OBJECT_DESCRIPTOR_POOL);
	if (spud_debug_name_set(pResult, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_descriptor_pool(pResult);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	pResult->_desc.debug_name = pResult->head.debug_name;

	*out_pool = pResult;
	return SPUD_SUCCESS;
}

void spudgpu_reset_descriptor_pool(spudgpu_descriptor_pool pool) {
	if (!pool)
		return;
	if (SPUD_STALE(pool, "spudgpu_reset_descriptor_pool", "pool"))
		return;
	// Release every set and reset the allocation cursors; existing
	// descriptor set handles become invalid.
	spudgpu_d3d12___free_descriptor_sets(pool, nullptr);
	pool->_cbv_srv_uav_cursor = 0;
	pool->_sampler_cursor     = 0;
}

void spudgpu_destroy_descriptor_pool(spudgpu_descriptor_pool pool) {
	if (!pool)
		return;
	if (SPUD_STALE(pool, "spudgpu_destroy_descriptor_pool", "pool"))
		return;
	spudgpu_descriptor_pool_d3d12 *p = (spudgpu_descriptor_pool_d3d12 *)pool;
	spudgpu_d3d12___free_descriptor_sets(p, nullptr);
	spudgpu_d3d12___give_back_shader_heap_ranges(
	    p->_device, p->_cbv_srv_uav_offset, p->_cbv_srv_uav_capacity, p->_sampler_offset, p->_sampler_capacity);
	SPUDGPU___LIVE_REMOVE(&p->_device->_live, SPUDGPU___OBJECT_DESCRIPTOR_POOL);
	SPUD_KILL(pool);
	const SPUD_ALLOCATOR allocator = p->head.allocator;
	spud_string_free(&allocator, p->head.debug_name);
	p->~spudgpu_descriptor_pool_d3d12();
	SPUD_DELETE(&allocator, spudgpu_descriptor_pool_d3d12, p);
}

SPUDRESULT spudgpu_create_descriptor_sets(
    spudgpu_device device,
    const spudgpu_descriptor_set_desc *desc,
    spudgpu_descriptor_set *out_sets) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_descriptor_sets", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_sets)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!desc->pool)
		return SPUDRESULT_GPU_INVALID_DESCRIPTOR_POOL;
	if (!desc->set_count)
		return SPUDRESULT_GPU_ZERO_DESCRIPTOR_SET_LAYOUTS;
	if (desc->set_count > SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_SET_LAYOUTS;

	spudgpu_descriptor_pool_d3d12 *pool =
	    (spudgpu_descriptor_pool_d3d12 *)desc->pool;

	// The pool as this call found it. A failure part-way through puts it
	// back: the sets this call allocated are freed and their heap ranges
	// returned, so the call allocates every set or none.
	spudgpu_descriptor_set_d3d12 *firstSet = pool->_sets;
	uint32_t firstCsvCursor                = pool->_cbv_srv_uav_cursor;
	uint32_t firstSamplerCursor            = pool->_sampler_cursor;
	SPUDRESULT sr                          = SPUD_SUCCESS;

	for (uint32_t i = 0; i < desc->set_count; ++i) {
		if (!desc->set_layouts[i]) {
			sr = SPUDRESULT_NULL_DESC;
			break;
		}

		spudgpu_descriptor_set_layout_d3d12 *layout =
		    (spudgpu_descriptor_set_layout_d3d12 *)desc->set_layouts[i];

		// Check pool has capacity.
		if (pool->_cbv_srv_uav_cursor + layout->_cbv_srv_uav_count >
		    pool->_cbv_srv_uav_capacity) {
			sr = SPUDRESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL;
			break;
		}
		if (pool->_sampler_cursor + layout->_sampler_count >
		    pool->_sampler_capacity) {
			sr = SPUDRESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL;
			break;
		}

		spudgpu_descriptor_set_d3d12 *pSet =
		    SPUD_NEW_ZEROED(&pool->head.allocator, spudgpu_descriptor_set_d3d12);
		if (!pSet) {
			sr = SPUDRESULT_OUT_OF_MEMORY;
			break;
		}
		pSet->head.allocator    = pool->head.allocator;
		SPUD_BIRTH(pSet);
		pSet->_pool             = pool;
		pSet->_layout           = layout;
		pSet->_cbv_srv_uav_base = pool->_cbv_srv_uav_cursor;
		pSet->_sampler_base     = pool->_sampler_cursor;
		pSet->_next             = pool->_sets;
		pool->_sets             = pSet;

		pool->_cbv_srv_uav_cursor += layout->_cbv_srv_uav_count;
		pool->_sampler_cursor     += layout->_sampler_count;

		out_sets[i] = (spudgpu_descriptor_set)pSet;
	}

	if (sr != SPUD_SUCCESS) {
		spudgpu_d3d12___free_descriptor_sets(pool, firstSet);
		pool->_cbv_srv_uav_cursor = firstCsvCursor;
		pool->_sampler_cursor     = firstSamplerCursor;
		for (uint32_t i = 0; i < desc->set_count; ++i)
			out_sets[i] = nullptr;
		return sr;
	}

	return SPUD_SUCCESS;
}

void spudgpu_update_descriptor_sets(
    spudgpu_device device,
    const spudgpu_write_descriptor_set *writes,
    uint32_t write_count) {
	if (!device || !writes || !write_count)
		return;
	if (SPUD_STALE(device, "spudgpu_update_descriptor_sets", "device"))
		return;
	for (uint32_t writes_i = 0; writes_i < write_count; ++writes_i)
		if (spud_struct_check(&writes[writes_i].base, SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET) != SPUD_SUCCESS)
			return;
	// A storage view of a cube is a 2D array view of its faces on every
	// backend (spudgpu.h): D3D12 has no storage cube. Checked before the
	// first write, so a refused call writes nothing.
	for (uint32_t writes_i = 0; writes_i < write_count; ++writes_i) {
		const spudgpu_write_descriptor_set *write = &writes[writes_i];
		if (write->descriptor_type != SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE || !write->image_info)
			continue;
		for (uint32_t k = 0; k < write->descriptor_count; ++k) {
			const spudgpu_image_view view = write->image_info[k].image_view;
			if (view && (view->_desc.type == SPUDGPU_IMAGE_VIEW_TYPE_CUBE || view->_desc.type == SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY))
				return;
		}
	}
	// Acceleration structure writes: the feature, and every element a live
	// top level (spudgpu.h), checked the same way on every backend.
	if (!spudgpu___internal_check_acceleration_structure_writes(device->_features, writes, write_count))
		return;

	ID3D12Device *d3dDevice = device->_d3d_device.Get();

	for (uint32_t w = 0; w < write_count; ++w) {
		const spudgpu_write_descriptor_set &wr = writes[w];
		if (!wr.dst_set)
			continue;

		spudgpu_descriptor_set_d3d12 *set =
		    (spudgpu_descriptor_set_d3d12 *)wr.dst_set;
		spudgpu_descriptor_set_layout_d3d12 *layout = set->_layout;
		spudgpu_descriptor_pool_d3d12 *pool         = set->_pool;

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

		const spudgpu_d3d12_binding_slot &slot = layout->_slots[slotIdx];
		SPUDGPU_DESCRIPTOR_TYPE dtype          = wr.descriptor_type;

		// Write CBV/SRV/UAV descriptors.
		if (dtype != SPUDGPU_DESCRIPTOR_TYPE_SAMPLER && pool->_cbv_srv_uav_capacity) {
			uint32_t baseSlot = pool->_cbv_srv_uav_offset +
			                    set->_cbv_srv_uav_base +
			                    slot.cbv_srv_uav_offset +
			                    wr.dst_array_element;

			for (uint32_t d = 0; d < wr.descriptor_count; ++d) {
				D3D12_CPU_DESCRIPTOR_HANDLE handle = spudgpu_d3d12___cpu_handle(pool->_device->_csv_heap, baseSlot + d);

				if (dtype == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
				    wr.buffer_info) {
					spudgpu_buffer_d3d12 *buf =
					    (spudgpu_buffer_d3d12 *)wr.buffer_info->buffer;
					D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
					cbvDesc.BufferLocation =
					    buf->_d3d_gpu_address + wr.buffer_info->offset;
					// CBV size must be 256-byte aligned.
					uint64_t range = wr.buffer_info->range
					                     ? wr.buffer_info->range
					                     : (buf->_desc.size - wr.buffer_info->offset);
					cbvDesc.SizeInBytes = (UINT)((range + 255) & ~255ULL);
					d3dDevice->CreateConstantBufferView(&cbvDesc, handle);

				} else if (dtype == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
				           wr.buffer_info) {
					spudgpu_buffer_d3d12 *buf =
					    (spudgpu_buffer_d3d12 *)wr.buffer_info->buffer;
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

				} else if ((dtype == SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
				            dtype == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
				           wr.image_info) {
					spudgpu_image_view_d3d12 *view =
					    (spudgpu_image_view_d3d12 *)wr.image_info->image_view;
					D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc =
					    view->_d3d_srv;
					d3dDevice->CreateShaderResourceView(
					    view->_image->_d3d_resource.Get(), &srvDesc, handle);

				} else if (dtype == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
				           wr.image_info) {
					spudgpu_image_view_d3d12 *view =
					    (spudgpu_image_view_d3d12 *)wr.image_info->image_view;
					D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc =
					    view->_d3d_uav;
					d3dDevice->CreateUnorderedAccessView(
					    view->_image->_d3d_resource.Get(), nullptr, &uavDesc, handle);
				} else if (dtype == SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE) {
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
		if (spudgpu_d3d12_is_sampler_type(dtype) && wr.sampler && pool->_sampler_capacity) {
			uint32_t baseSlot = pool->_sampler_offset + set->_sampler_base + slot.sampler_offset + wr.dst_array_element;
			D3D12_CPU_DESCRIPTOR_HANDLE handle = spudgpu_d3d12___cpu_handle(pool->_device->_sampler_heap, baseSlot);

			spudgpu_sampler_d3d12 *sampler = (spudgpu_sampler_d3d12 *)wr.sampler;
			d3dDevice->CreateSampler(&sampler->_d3d_desc, handle);
		}
	}
}

void spudgpu_cmd_bind_descriptor_sets(
    spudgpu_command_list cmd,
    spudgpu_shader_pipeline pipeline,
    uint32_t first_set,
    const spudgpu_descriptor_set *sets,
    uint32_t set_count) {
	if (!cmd || !pipeline || !sets || !set_count)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_bind_descriptor_sets", "cmd"))
		return;
	if (SPUD_STALE(pipeline, "spudgpu_cmd_bind_descriptor_sets", "pipeline"))
		return;
	if (set_count > SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (first_set > SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS - set_count)
		return;

	ID3D12GraphicsCommandList *cmdList = cmd->_d3d_cmd_list.Get();

	// Every pool's sets, and the bindless state, are in the device's two
	// shader-visible heaps, so setting them never moves a table bound before
	// out of a set heap, and sets of different pools can be bound together.
	spudgpu_d3d12___set_shader_heaps(cmdList, cmd->_allocator->_device);

	// Each set has a table in each heap it uses, at the root parameters its
	// pipeline's root signature put them at (spudgpu_d3d12_root_layout).
	const spudgpu_d3d12_root_layout &root = pipeline->_root_layout;
	for (uint32_t i = 0; i < set_count; ++i) {
		spudgpu_descriptor_set_d3d12 *set   = (spudgpu_descriptor_set_d3d12 *)sets[i];
		spudgpu_descriptor_pool_d3d12 *pool = set->_pool;
		const uint32_t s                    = first_set + i;
		if (root.cbv_srv_uav_param[s] != SPUDGPU_D3D12_NO_ROOT_PARAMETER && pool->_cbv_srv_uav_capacity)
			cmdList->SetGraphicsRootDescriptorTable(
			    root.cbv_srv_uav_param[s],
			    spudgpu_d3d12___gpu_handle(pool->_device->_csv_heap, pool->_cbv_srv_uav_offset + set->_cbv_srv_uav_base));
		if (root.sampler_param[s] != SPUDGPU_D3D12_NO_ROOT_PARAMETER && pool->_sampler_capacity)
			cmdList->SetGraphicsRootDescriptorTable(
			    root.sampler_param[s],
			    spudgpu_d3d12___gpu_handle(pool->_device->_sampler_heap, pool->_sampler_offset + set->_sampler_base));
	}
}

void spudgpu_cmd_bind_descriptor_sets_compute(
    spudgpu_command_list cmd,
    spudgpu_compute_pipeline pipeline,
    uint32_t first_set,
    const spudgpu_descriptor_set *sets,
    uint32_t set_count) {
	if (!cmd || !pipeline || !sets || !set_count)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_bind_descriptor_sets_compute", "cmd"))
		return;
	if (SPUD_STALE(pipeline, "spudgpu_cmd_bind_descriptor_sets_compute", "pipeline"))
		return;
	if (set_count > SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (first_set > SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS - set_count)
		return;

	ID3D12GraphicsCommandList *cmdList = cmd->_d3d_cmd_list.Get();

	// As in spudgpu_cmd_bind_descriptor_sets.
	spudgpu_d3d12___set_shader_heaps(cmdList, cmd->_allocator->_device);

	const spudgpu_d3d12_root_layout &root = pipeline->_root_layout;
	for (uint32_t i = 0; i < set_count; ++i) {
		spudgpu_descriptor_set_d3d12 *set   = (spudgpu_descriptor_set_d3d12 *)sets[i];
		spudgpu_descriptor_pool_d3d12 *pool = set->_pool;
		const uint32_t s                    = first_set + i;
		if (root.cbv_srv_uav_param[s] != SPUDGPU_D3D12_NO_ROOT_PARAMETER && pool->_cbv_srv_uav_capacity)
			cmdList->SetComputeRootDescriptorTable(
			    root.cbv_srv_uav_param[s],
			    spudgpu_d3d12___gpu_handle(pool->_device->_csv_heap, pool->_cbv_srv_uav_offset + set->_cbv_srv_uav_base));
		if (root.sampler_param[s] != SPUDGPU_D3D12_NO_ROOT_PARAMETER && pool->_sampler_capacity)
			cmdList->SetComputeRootDescriptorTable(
			    root.sampler_param[s],
			    spudgpu_d3d12___gpu_handle(pool->_device->_sampler_heap, pool->_sampler_offset + set->_sampler_base));
	}
}

// ============================================================================
//  Bindless / Descriptor Indexing
// ============================================================================

// Made by spudgpu_create_device when SPUDGPU_FEATURE_BINDLESS is asked for,
// before the device is handed out, and never replaced: every call after reads
// device->_bindless with no lock. spudgpu_create_device checked that the table
// fits in the descriptor budget.
SPUDRESULT spudgpu_d3d12___create_bindless_state(spudgpu_device_d3d12 *device) {
	// A range of the device's shader-visible CBV/SRV/UAV heap, which the
	// descriptor pools share, so bindless and pool sets bind together. Kept
	// for the device's life.
	uint32_t offset   = 0;
	uint32_t noOffset = 0;
	SPUDRESULT sr     = spudgpu_d3d12___take_shader_heap_ranges(device, 3 * SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS, 0, &offset, &noOffset);
	if (sr != SPUD_SUCCESS)
		return sr;

	// Sentinel layout: only binding_count/bindings[] matter — that's all
	// spudgpu_d3d12_build_root_signature (spudgpud3d12shader.cpp) reads to
	// generate the matching root-signature ranges when a caller includes
	// this handle in their own descriptor_set_layouts[]. Root signature
	// version 1.0 (what that builder already serializes with) treats every
	// range as DESCRIPTORS_VOLATILE | DATA_VOLATILE by default, so the
	// update-after-bind write pattern below is already valid with zero
	// changes to that shared helper.
	auto *layout = SPUD_NEW_ZEROED(&device->head.allocator, spudgpu_descriptor_set_layout_d3d12);
	if (!layout) {
		spudgpu_d3d12___give_back_shader_heap_ranges(device, offset, 3 * SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS, 0, 0);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	layout->head.allocator       = device->head.allocator;
	SPUD_BIRTH(layout);
	layout->_device              = device;
	layout->_desc.binding_count  = 3;

	SPUDGPU_SHADER_STAGE allStages = (SPUDGPU_SHADER_STAGE)(
	    SPUDGPU_SHADER_STAGE_VERTEX | SPUDGPU_SHADER_STAGE_FRAGMENT |
	    SPUDGPU_SHADER_STAGE_COMPUTE | SPUDGPU_SHADER_STAGE_GEOMETRY |
	    SPUDGPU_SHADER_STAGE_TESSELLATION_CONTROL |
	    SPUDGPU_SHADER_STAGE_TESSELLATION_EVALUATION);

	// HLSL's UAV ("u#") register space is shared between texture UAVs
	// (STORAGE_IMAGE) and buffer UAVs (STORAGE_BUFFER) -- unlike Vulkan,
	// where these are just independently numbered descriptor-set slots with
	// no register-namespace concept at all. spudgpu_d3d12_build_root_signature
	// maps each binding's .binding field straight to BaseShaderRegister, so
	// with SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS-sized (4096) ranges, using
	// sequential binding numbers (1, 2) here made the two UAV ranges overlap
	// (u1..u4096 fully covers u2) -- D3D12SerializeRootSignature rejects
	// that outright. SAMPLED_IMAGE (SRV, "t#") doesn't need to make room for
	// either, since SRV and UAV are already separate register classes.
	layout->_desc.bindings[0] = {
	    0, SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
	    SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS, allStages};
	layout->_desc.bindings[1] = {
	    0, SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	    SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS, allStages};
	layout->_desc.bindings[2] = {
	    SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS, SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	    SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS, allStages};

	// Both are released with the device (spudgpud3d12context.cpp).
	spudgpu_bindless_state_d3d12 *state =
	    SPUD_NEW(&device->head.allocator, spudgpu_bindless_state_d3d12);
	if (!state) {
		SPUD_DELETE(&device->head.allocator, spudgpu_descriptor_set_layout_d3d12, layout);
		spudgpu_d3d12___give_back_shader_heap_ranges(device, offset, 3 * SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS, 0, 0);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	state                            = new (state) spudgpu_bindless_state_d3d12();
	state->sampled_image_free_stack  = eq::array<uint32_t>(&device->_eq_allocator);
	state->storage_image_free_stack  = eq::array<uint32_t>(&device->_eq_allocator);
	state->storage_buffer_free_stack = eq::array<uint32_t>(&device->_eq_allocator);
	state->offset                    = offset;
	state->layout = layout;

	device->_bindless = state;
	return SPUD_SUCCESS;
}

// Called with the state's lock held.
static SPUDRESULT spudgpud3d12___bindless_alloc_index(
    uint32_t *next_unused,
    eq::array<uint32_t> &free_stack,
    uint32_t *out_index) {
	if (!free_stack.empty()) {
		*out_index = free_stack.data[free_stack.count - 1];
		// The last element, so the removal can't fail.
		(void)free_stack.remove(free_stack.count - 1);
		return SPUD_SUCCESS;
	}
	if (*next_unused >= SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS)
		return SPUDRESULT_GPU_BINDLESS_OUT_OF_SLOTS;
	*out_index = (*next_unused)++;
	return SPUD_SUCCESS;
}

spudgpu_descriptor_set_layout spudgpu_get_bindless_descriptor_set_layout(spudgpu_device device) {
	if (!device)
		return nullptr;
	if (SPUD_STALE(device, "spudgpu_get_bindless_descriptor_set_layout", "device"))
		return nullptr;
	if (!device->_bindless)
		return nullptr;
	return (spudgpu_descriptor_set_layout)device->_bindless->layout;
}

SPUDRESULT spudgpu_bindless_register_sampled_image(
    spudgpu_device device,
    spudgpu_image_view view,
    uint32_t *out_index) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_bindless_register_sampled_image", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!view)
		return SPUDRESULT_GPU_INVALID_IMAGE_VIEW;
	if (SPUD_STALE(view, "spudgpu_bindless_register_sampled_image", "view"))
		return SPUDRESULT_GPU_INVALID_IMAGE_VIEW;
	if (!out_index)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!device->_bindless)
		return SPUDRESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED;

	spudgpu_bindless_state_d3d12 *state = device->_bindless;
	SPUDRESULT sr;
	{
		std::lock_guard<std::mutex> slots_lock(state->lock);
		sr = spudgpud3d12___bindless_alloc_index(
		    &state->sampled_image_next_unused, state->sampled_image_free_stack, out_index);
	}
	if (sr != SPUD_SUCCESS)
		return sr;

	spudgpu_image_view_d3d12 *vView = (spudgpu_image_view_d3d12 *)view;
	D3D12_CPU_DESCRIPTOR_HANDLE handle =
	    spudgpu_d3d12___cpu_handle(device->_csv_heap, state->offset + *out_index); // class base 0

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = vView->_d3d_srv;
	device->_d3d_device->CreateShaderResourceView(
	    vView->_image->_d3d_resource.Get(), &srvDesc, handle);
	return SPUD_SUCCESS;
}

void spudgpu_bindless_unregister_sampled_image(spudgpu_device device, uint32_t index) {
	if (!device)
		return;
	if (SPUD_STALE(device, "spudgpu_bindless_unregister_sampled_image", "device"))
		return;
	spudgpu_bindless_state_d3d12 *state = device->_bindless;
	if (!state)
		return;
	if (index == SPUDGPU_BINDLESS_INVALID_INDEX)
		return;
	std::lock_guard<std::mutex> slots_lock(state->lock);
	// Out of memory: the slot isn't reused, and the table is a slot
	// smaller from now on.
	(void)state->sampled_image_free_stack.push(index);
}

SPUDRESULT spudgpu_bindless_register_storage_image(
    spudgpu_device device,
    spudgpu_image_view view,
    uint32_t *out_index) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_bindless_register_storage_image", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!view)
		return SPUDRESULT_GPU_INVALID_IMAGE_VIEW;
	if (SPUD_STALE(view, "spudgpu_bindless_register_storage_image", "view"))
		return SPUDRESULT_GPU_INVALID_IMAGE_VIEW;
	if (!out_index)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!device->_bindless)
		return SPUDRESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED;

	spudgpu_bindless_state_d3d12 *state = device->_bindless;
	SPUDRESULT sr;
	{
		std::lock_guard<std::mutex> slots_lock(state->lock);
		sr = spudgpud3d12___bindless_alloc_index(
		    &state->storage_image_next_unused, state->storage_image_free_stack, out_index);
	}
	if (sr != SPUD_SUCCESS)
		return sr;

	spudgpu_image_view_d3d12 *vView = (spudgpu_image_view_d3d12 *)view;
	D3D12_CPU_DESCRIPTOR_HANDLE handle = spudgpu_d3d12___cpu_handle(
	    device->_csv_heap, state->offset + SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS + *out_index); // class base 1

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = vView->_d3d_uav;
	device->_d3d_device->CreateUnorderedAccessView(
	    vView->_image->_d3d_resource.Get(), nullptr, &uavDesc, handle);
	return SPUD_SUCCESS;
}

void spudgpu_bindless_unregister_storage_image(spudgpu_device device, uint32_t index) {
	if (!device)
		return;
	if (SPUD_STALE(device, "spudgpu_bindless_unregister_storage_image", "device"))
		return;
	spudgpu_bindless_state_d3d12 *state = device->_bindless;
	if (!state)
		return;
	if (index == SPUDGPU_BINDLESS_INVALID_INDEX)
		return;
	std::lock_guard<std::mutex> slots_lock(state->lock);
	// Out of memory: the slot isn't reused, and the table is a slot
	// smaller from now on.
	(void)state->storage_image_free_stack.push(index);
}

SPUDRESULT spudgpu_bindless_register_storage_buffer(
    spudgpu_device device,
    spudgpu_buffer_view view,
    uint32_t *out_index) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_bindless_register_storage_buffer", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!view)
		return SPUDRESULT_GPU_INVALID_BUFFER_VIEW;
	if (SPUD_STALE(view, "spudgpu_bindless_register_storage_buffer", "view"))
		return SPUDRESULT_GPU_INVALID_BUFFER_VIEW;
	if (!out_index)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!device->_bindless)
		return SPUDRESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED;

	spudgpu_bindless_state_d3d12 *state = device->_bindless;
	SPUDRESULT sr;
	{
		std::lock_guard<std::mutex> slots_lock(state->lock);
		sr = spudgpud3d12___bindless_alloc_index(
		    &state->storage_buffer_next_unused, state->storage_buffer_free_stack, out_index);
	}
	if (sr != SPUD_SUCCESS)
		return sr;

	spudgpu_buffer_view_d3d12 *vView = (spudgpu_buffer_view_d3d12 *)view;
	spudgpu_buffer_d3d12 *vBuf       = vView->_buffer;

	// Raw byte-address UAV, matching spudgpu_update_descriptor_sets'
	// STORAGE_BUFFER handling above — buffer views have no pre-built UAV
	// desc (SPUDGPU_D3D12_BUFFER_VIEW only carries VB/IB/CB/SO views).
	uint64_t range = vView->_desc.size
	                     ? vView->_desc.size
	                     : (vBuf->_desc.size - vView->_desc.offset_from_parent_buffer);

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format              = DXGI_FORMAT_R32_TYPELESS;
	uavDesc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.FirstElement = vView->_desc.offset_from_parent_buffer / sizeof(uint32_t);
	uavDesc.Buffer.NumElements  = (UINT)(range / sizeof(uint32_t));
	uavDesc.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;

	D3D12_CPU_DESCRIPTOR_HANDLE handle = spudgpu_d3d12___cpu_handle(
	    device->_csv_heap, state->offset + 2 * SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS + *out_index); // class base 2

	device->_d3d_device->CreateUnorderedAccessView(
	    vBuf->_d3d_resource.Get(), nullptr, &uavDesc, handle);
	return SPUD_SUCCESS;
}

void spudgpu_bindless_unregister_storage_buffer(spudgpu_device device, uint32_t index) {
	if (!device)
		return;
	if (SPUD_STALE(device, "spudgpu_bindless_unregister_storage_buffer", "device"))
		return;
	spudgpu_bindless_state_d3d12 *state = device->_bindless;
	if (!state)
		return;
	if (index == SPUDGPU_BINDLESS_INVALID_INDEX)
		return;
	std::lock_guard<std::mutex> slots_lock(state->lock);
	// Out of memory: the slot isn't reused, and the table is a slot
	// smaller from now on.
	(void)state->storage_buffer_free_stack.push(index);
}

void spudgpu_cmd_bind_bindless_resources(
    spudgpu_command_list cmd,
    spudgpu_shader_pipeline pipeline,
    uint32_t set_index) {
	if (!cmd || !pipeline)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_bind_bindless_resources", "cmd"))
		return;
	if (SPUD_STALE(pipeline, "spudgpu_cmd_bind_bindless_resources", "pipeline"))
		return;
	// The bindless set has only CBV/SRV/UAV descriptors: one table.
	if (set_index >= SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (pipeline->_root_layout.cbv_srv_uav_param[set_index] == SPUDGPU_D3D12_NO_ROOT_PARAMETER)
		return;
	spudgpu_device_d3d12 *device = cmd->_allocator->_device;
	spudgpu_bindless_state_d3d12 *state = device->_bindless;
	if (!state)
		return;

	ID3D12GraphicsCommandList *cmdList = cmd->_d3d_cmd_list.Get();

	// D3D12 allows one CBV_SRV_UAV heap set at a time. The bindless range and
	// every pool's are in the device's same two heaps, so binding pool sets
	// after this, or before, leaves this table in a heap that is set.
	spudgpu_d3d12___set_shader_heaps(cmdList, device);
	cmdList->SetGraphicsRootDescriptorTable(
	    pipeline->_root_layout.cbv_srv_uav_param[set_index], spudgpu_d3d12___gpu_handle(device->_csv_heap, state->offset));
}

void spudgpu_cmd_bind_bindless_resources_compute(
    spudgpu_command_list cmd,
    spudgpu_compute_pipeline pipeline,
    uint32_t set_index) {
	if (!cmd || !pipeline)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_bind_bindless_resources_compute", "cmd"))
		return;
	if (SPUD_STALE(pipeline, "spudgpu_cmd_bind_bindless_resources_compute", "pipeline"))
		return;
	// The bindless set has only CBV/SRV/UAV descriptors: one table.
	if (set_index >= SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (pipeline->_root_layout.cbv_srv_uav_param[set_index] == SPUDGPU_D3D12_NO_ROOT_PARAMETER)
		return;
	spudgpu_device_d3d12 *device = cmd->_allocator->_device;
	spudgpu_bindless_state_d3d12 *state = device->_bindless;
	if (!state)
		return;

	ID3D12GraphicsCommandList *cmdList = cmd->_d3d_cmd_list.Get();

	// As in spudgpu_cmd_bind_bindless_resources.
	spudgpu_d3d12___set_shader_heaps(cmdList, device);
	cmdList->SetComputeRootDescriptorTable(
	    pipeline->_root_layout.cbv_srv_uav_param[set_index], spudgpu_d3d12___gpu_handle(device->_csv_heap, state->offset));
}

} // extern "C"

#endif // SPUDGPU_COMPILE_D3D12_API
