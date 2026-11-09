#if SPUDGPU_COMPILE_D3D12_API

#include "spudgpud3d12.hpp"
#include <cstring>

// A timestamp set is a timestamp query heap and a readback buffer.
// EndQuery writes an entry as a pass starts or ends
// (spudgpud3d12renderpass.cpp), ResolveQueryData copies entries into the
// buffer, and the CPU reads them there. Timestamp queries need no reset.

extern "C" {

SPUDRESULT spudgpu_create_timestamp_set(
    spudgpu_device device,
    spudgpu_command_queue queue,
    uint32_t count,
    spudgpu_timestamp_set *out_set) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_timestamp_set", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!queue)
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	if (SPUD_STALE(queue, "spudgpu_create_timestamp_set", "queue"))
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	if (count == 0)
		return SPUDRESULT_ZERO_SIZE;
	if (!out_set)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!(device->_features & SPUDGPU_FEATURE_PASS_TIMESTAMPS))
		return SPUDRESULT_GPU_TIMESTAMPS_NOT_SUPPORTED;

	UINT64 frequency = 0;
	if (FAILED(queue->_d3d_cmd_queue->GetTimestampFrequency(&frequency)) || frequency == 0)
		return SPUDRESULT_API_SPECIFIC_FAILURE;

	spudgpu_timestamp_set_d3d12 *set = SPUD_NEW(&device->head.allocator, spudgpu_timestamp_set_d3d12);
	if (!set)
		return SPUDRESULT_OUT_OF_MEMORY;
	set                 = new (set) spudgpu_timestamp_set_d3d12();
	set->head.allocator = device->head.allocator;
	SPUD_BIRTH_IN(set, &device->_quarantine);
	set->_count         = count;
	set->_frequency     = frequency;
	set->_device        = device;

	D3D12_QUERY_HEAP_DESC heapDesc = {};
	heapDesc.Type                  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	heapDesc.Count                 = count;
	if (FAILED(device->_d3d_device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&set->_d3d_query_heap)))) {
		set->~spudgpu_timestamp_set_d3d12();
		SPUD_DELETE(&device->head.allocator, spudgpu_timestamp_set_d3d12, set);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}

	D3D12_HEAP_PROPERTIES heapProperties = {};
	heapProperties.Type                  = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC bufferDesc       = {};
	bufferDesc.Dimension                 = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width                     = (UINT64)count * sizeof(uint64_t);
	bufferDesc.Height                    = 1;
	bufferDesc.DepthOrArraySize          = 1;
	bufferDesc.MipLevels                 = 1;
	bufferDesc.Format                    = DXGI_FORMAT_UNKNOWN;
	bufferDesc.SampleDesc.Count          = 1;
	bufferDesc.Layout                    = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	// Contents undefined until written, as every buffer's (spudgpu.h).
	if (FAILED(device->_d3d_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &bufferDesc,
	                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&set->_d3d_readback)))) {
		set->~spudgpu_timestamp_set_d3d12();
		SPUD_DELETE(&device->head.allocator, spudgpu_timestamp_set_d3d12, set);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}

	SPUDGPU___LIVE_ADD(&device->_live, SPUDGPU___OBJECT_TIMESTAMP_SET);
	*out_set = set;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_timestamp_set(spudgpu_timestamp_set set) {
	if (!set)
		return;
	if (SPUD_STALE(set, "spudgpu_destroy_timestamp_set", "set"))
		return;
	SPUDGPU___LIVE_REMOVE(&set->_device->_live, SPUDGPU___OBJECT_TIMESTAMP_SET);
	SPUD_KILL(set);
	const SPUD_ALLOCATOR allocator = set->head.allocator;
	spud_string_free(&allocator, set->head.debug_name);
	SPUD_QUARANTINE_KEEP(quarantine, set);
	set->~spudgpu_timestamp_set_d3d12();
	SPUD_RETIRE(quarantine, &allocator, spudgpu_timestamp_set_d3d12, set);
}

void spudgpu_cmd_reset_timestamps(
    spudgpu_command_list cmd,
    spudgpu_timestamp_set set,
    uint32_t first,
    uint32_t count) {
	// A timestamp query has no begin and nothing to reset.
	if (!cmd || !set)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_reset_timestamps", "cmd"))
		return;
	if (SPUD_STALE(set, "spudgpu_cmd_reset_timestamps", "set"))
		return;
	(void)first;
	(void)count;
}

void spudgpu_cmd_resolve_timestamps(
    spudgpu_command_list cmd,
    spudgpu_timestamp_set set,
    uint32_t first,
    uint32_t count) {
	if (!cmd || !set || count == 0)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_resolve_timestamps", "cmd"))
		return;
	if (SPUD_STALE(set, "spudgpu_cmd_resolve_timestamps", "set"))
		return;
	if (first >= set->_count || count > set->_count - first)
		return;
	cmd->_d3d_cmd_list->ResolveQueryData(set->_d3d_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, count, set->_d3d_readback.Get(),
	                                     (UINT64)first * sizeof(uint64_t));
}

SPUDRESULT spudgpu_get_timestamps(
    spudgpu_timestamp_set set,
    uint32_t first,
    uint32_t count,
    uint64_t *out_ns) {
	if (!set)
		return SPUDRESULT_GPU_INVALID_TIMESTAMP_SET;
	if (SPUD_STALE(set, "spudgpu_get_timestamps", "set"))
		return SPUDRESULT_GPU_INVALID_TIMESTAMP_SET;
	if (count == 0)
		return SPUDRESULT_ZERO_SIZE;
	if (first >= set->_count || count > set->_count - first)
		return SPUDRESULT_INDEX_OUT_OF_RANGE;
	if (!out_ns)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;

	D3D12_RANGE read = {(SIZE_T)first * sizeof(uint64_t), (SIZE_T)(first + count) * sizeof(uint64_t)};
	void *mapped     = nullptr;
	if (FAILED(set->_d3d_readback->Map(0, &read, &mapped)))
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	memcpy(out_ns, (const uint8_t *)mapped + read.Begin, (size_t)count * sizeof(uint64_t));
	D3D12_RANGE written = {0, 0};
	set->_d3d_readback->Unmap(0, &written);

	// Ticks to nanoseconds without overflowing 64 bits for any tick count a
	// machine reaches: the whole seconds and the rest apart.
	const uint64_t frequency = set->_frequency;
	for (uint32_t i = 0; i < count; ++i) {
		const uint64_t ticks = out_ns[i];
		out_ns[i]            = (ticks / frequency) * 1000000000ull + (ticks % frequency) * 1000000000ull / frequency;
	}
	return SPUD_SUCCESS;
}

} // extern "C"

#endif // SPUDGPU_COMPILE_D3D12_API
