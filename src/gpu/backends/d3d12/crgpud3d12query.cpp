#if CRGPU_COMPILE_D3D12_API

#include "crgpud3d12.hpp"
#include <cstring>

// A timestamp set is a timestamp query heap and a readback buffer.
// EndQuery writes an entry as a pass starts or ends
// (crgpud3d12renderpass.cpp), ResolveQueryData copies entries into the
// buffer, and the CPU reads them there. Timestamp queries need no reset.

extern "C" {

CR_RESULT crgpu_timestamp_set_create(
    crgpu_device device,
    crgpu_command_queue queue,
    uint32_t count,
    crgpu_timestamp_set *out_set) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_timestamp_set_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!queue)
		return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
	if (CR_STALE(queue, "crgpu_timestamp_set_create", "queue"))
		return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
	if (count == 0)
		return CR_RESULT_ZERO_SIZE;
	if (!out_set)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!(device->_features & CRGPU_FEATURE_PASS_TIMESTAMPS))
		return CR_RESULT_GPU_TIMESTAMPS_NOT_SUPPORTED;

	UINT64 frequency = 0;
	if (FAILED(queue->_d3d_cmd_queue->GetTimestampFrequency(&frequency)) || frequency == 0)
		return CR_RESULT_API_SPECIFIC_FAILURE;

	crgpu_timestamp_set_d3d12 *set = CR_NEW(&device->head.allocator, crgpu_timestamp_set_d3d12);
	if (!set)
		return CR_RESULT_OUT_OF_MEMORY;
	set                 = new (set) crgpu_timestamp_set_d3d12();
	set->head.allocator = device->head.allocator;
	CR_BIRTH_IN(set, &device->_quarantine);
	set->_count         = count;
	set->_frequency     = frequency;
	set->_device        = device;

	D3D12_QUERY_HEAP_DESC heapDesc = {};
	heapDesc.Type                  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	heapDesc.Count                 = count;
	if (FAILED(device->_d3d_device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&set->_d3d_query_heap)))) {
		set->~crgpu_timestamp_set_d3d12();
		CR_DELETE(&device->head.allocator, crgpu_timestamp_set_d3d12, set);
		return CR_RESULT_API_SPECIFIC_FAILURE;
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
	// Contents undefined until written, as every buffer's (crgpu.h).
	if (FAILED(device->_d3d_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &bufferDesc,
	                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&set->_d3d_readback)))) {
		set->~crgpu_timestamp_set_d3d12();
		CR_DELETE(&device->head.allocator, crgpu_timestamp_set_d3d12, set);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_TIMESTAMP_SET);
	*out_set = set;
	return CR_SUCCESS;
}

void crgpu_timestamp_set_destroy(crgpu_timestamp_set set) {
	if (!set)
		return;
	if (CR_STALE(set, "crgpu_timestamp_set_destroy", "set"))
		return;
	CRGPU___LIVE_REMOVE(&set->_device->_live, CRGPU___OBJECT_TIMESTAMP_SET);
	CR_KILL(set);
	const CR_ALLOCATOR allocator = set->head.allocator;
	cr_string_free(&allocator, set->head.debug_name);
	CR_QUARANTINE_KEEP(quarantine, set);
	set->~crgpu_timestamp_set_d3d12();
	CR_RETIRE(quarantine, &allocator, crgpu_timestamp_set_d3d12, set);
}

void crgpu_command_list_reset_timestamps(
    crgpu_command_list cmd,
    crgpu_timestamp_set set,
    uint32_t first,
    uint32_t count) {
	// A timestamp query has no begin and nothing to reset.
	if (!cmd || !set)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_reset_timestamps", "cmd"))
		return;
	if (CR_STALE(set, "crgpu_command_list_reset_timestamps", "set"))
		return;
	(void)first;
	(void)count;
}

void crgpu_command_list_resolve_timestamps(
    crgpu_command_list cmd,
    crgpu_timestamp_set set,
    uint32_t first,
    uint32_t count) {
	if (!cmd || !set || count == 0)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_resolve_timestamps", "cmd"))
		return;
	if (CR_STALE(set, "crgpu_command_list_resolve_timestamps", "set"))
		return;
	if (first >= set->_count || count > set->_count - first)
		return;
	cmd->_d3d_cmd_list->ResolveQueryData(set->_d3d_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, count, set->_d3d_readback.Get(),
	                                     (UINT64)first * sizeof(uint64_t));
}

CR_RESULT crgpu_timestamp_set_get(
    crgpu_timestamp_set set,
    uint32_t first,
    uint32_t count,
    uint64_t *out_ns) {
	if (!set)
		return CR_RESULT_GPU_INVALID_TIMESTAMP_SET;
	if (CR_STALE(set, "crgpu_timestamp_set_get", "set"))
		return CR_RESULT_GPU_INVALID_TIMESTAMP_SET;
	if (count == 0)
		return CR_RESULT_ZERO_SIZE;
	if (first >= set->_count || count > set->_count - first)
		return CR_RESULT_INDEX_OUT_OF_RANGE;
	if (!out_ns)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	D3D12_RANGE read = {(SIZE_T)first * sizeof(uint64_t), (SIZE_T)(first + count) * sizeof(uint64_t)};
	void *mapped     = nullptr;
	if (FAILED(set->_d3d_readback->Map(0, &read, &mapped)))
		return CR_RESULT_API_SPECIFIC_FAILURE;
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
	return CR_SUCCESS;
}

} // extern "C"

#endif // CRGPU_COMPILE_D3D12_API
