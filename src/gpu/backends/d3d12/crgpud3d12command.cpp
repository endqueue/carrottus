
#if CRGPU_COMPILE_D3D12_API

#include "crgpud3d12.hpp"
#include <cassert>
#include <cstring>
#include "../../../crstructbase.h"

extern "C" {

CR_RESULT crgpu_command_queue_get(
    crgpu_device device,
    CRGPU_COMMAND_LIST_TYPE type,
    uint32_t index,
    crgpu_command_queue *out_queue) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_command_queue_get", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	crgpu_command_queue *queues = nullptr;
	uint32_t count                = 0;
	switch (type) {
	case CRGPU_COMMAND_LIST_TYPE_DIRECT:
		queues = device->_cmd_queues_direct;
		count  = device->_cmd_queue_count_direct;
		break;
	case CRGPU_COMMAND_LIST_TYPE_COMPUTE:
		queues = device->_cmd_queues_compute;
		count  = device->_cmd_queue_count_compute;
		break;
	case CRGPU_COMMAND_LIST_TYPE_COPY:
		queues = device->_cmd_queues_copy;
		count  = device->_cmd_queue_count_copy;
		break;
	default:
		// Bundles aren't submitted to a queue, on any backend.
		return CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE;
	}
	if (index >= count)
		return CR_RESULT_INDEX_OUT_OF_RANGE;
	if (!out_queue)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_queue = queues[index];
	return CR_SUCCESS;
}

CR_RESULT crgpu_command_queue_submit_lists(
    crgpu_command_queue queue,
    crgpu_command_list *cmd_lists,
    uint32_t cmd_list_count) {
	if (!queue)
		return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
	if (CR_STALE(queue, "crgpu_command_queue_submit_lists", "queue"))
		return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
	if (!cmd_lists)
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	if (cmd_list_count == 0)
		return CR_RESULT_ZERO_SIZE;
	if (FAILED(queue->_device->_d3d_device->GetDeviceRemovedReason()))
		return CR_RESULT_GPU_DEVICE_LOST;
	crgpu_d3d12___queue_enter(queue);
	queue->_scratch_lists.clear();
	if (queue->_scratch_lists.reserve(cmd_list_count) != EQ_SUCCESS) {
		crgpu_d3d12___queue_leave(queue);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	for (uint32_t i = 0; i < cmd_list_count; ++i)
		(void)queue->_scratch_lists.push(cmd_lists[i]->_d3d_cmd_list.Get());
	queue->_d3d_cmd_queue->ExecuteCommandLists(
	    static_cast<UINT>(cmd_list_count), queue->_scratch_lists.data);
	crgpu_d3d12___queue_leave(queue);
	return CR_SUCCESS;
}

CR_RESULT crgpu_command_queue_submit_lists_synced(
    crgpu_command_queue queue,
    crgpu_command_list *cmd_lists,
    uint32_t cmd_list_count,
    crgpu_swap_chain swap_chain) {
	if (!queue)
		return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
	if (CR_STALE(queue, "crgpu_command_queue_submit_lists_synced", "queue"))
		return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
	if (!cmd_lists)
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	if (cmd_list_count == 0)
		return CR_RESULT_ZERO_SIZE;
	if (!swap_chain)
		return CR_RESULT_GPU_INVALID_SWAP_CHAIN;
	if (CR_STALE(swap_chain, "crgpu_command_queue_submit_lists_synced", "swap_chain"))
		return CR_RESULT_GPU_INVALID_SWAP_CHAIN;

	// crgpu_command_queue_submit with only the command lists and the swap chain set.
	crgpu_submit_desc desc = {};
	desc.base.type = CR_STRUCT_TYPE_GPU_SUBMIT_DESC;
	desc.cmd_lists           = cmd_lists;
	desc.cmd_list_count      = cmd_list_count;
	desc.swap_chain          = swap_chain;
	return crgpu_command_queue_submit(queue, &desc);
}

CR_RESULT crgpu_command_allocator_create(
    crgpu_device device,
    const crgpu_command_allocator_desc *desc,
    crgpu_command_allocator *out_allocator) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_command_allocator_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_allocator)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	// The lists of an allocator are submitted to a queue of its kind, a
	// bundle's run inside a direct list: the device must have one.
	uint32_t queues = 0;
	switch (desc->type) {
	case CRGPU_COMMAND_LIST_TYPE_DIRECT:
	case CRGPU_COMMAND_LIST_TYPE_BUNDLE:
		queues = device->_cmd_queue_count_direct;
		break;
	case CRGPU_COMMAND_LIST_TYPE_COMPUTE:
		queues = device->_cmd_queue_count_compute;
		break;
	case CRGPU_COMMAND_LIST_TYPE_COPY:
		queues = device->_cmd_queue_count_copy;
		break;
	default:
		break;
	}
	if (queues == 0)
		return CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE;
	crgpu_command_allocator_d3d12 *pResult =
	    CR_NEW(&device->head.allocator, crgpu_command_allocator_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult                 = new (pResult) crgpu_command_allocator_d3d12();
	pResult->head.allocator = device->head.allocator;
	CR_BIRTH_IN(pResult, &device->_quarantine);
	pResult->_device        = device;
    pResult->_d3d_cmd_list_type = crgpu_d3d12_get_command_list_type(desc->type);
    pResult->_type = desc->type;
	if (FAILED(device->_d3d_device->CreateCommandAllocator(
	        pResult->_d3d_cmd_list_type,
	        IID_PPV_ARGS(&pResult->_d3d_cmd_allocator)))) {
		pResult->~crgpu_command_allocator_d3d12();
		CR_DELETE(&device->head.allocator, crgpu_command_allocator_d3d12, pResult);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_COMMAND_ALLOCATOR);
	*out_allocator = pResult;
	return CR_SUCCESS;
}

void crgpu_command_allocator_destroy(crgpu_command_allocator allocator) {
	if (!allocator)
		return;
	if (CR_STALE(allocator, "crgpu_command_allocator_destroy", "allocator"))
		return;
	CRGPU___LIVE_REMOVE(&allocator->_device->_live, CRGPU___OBJECT_COMMAND_ALLOCATOR);
	CR_KILL(allocator);
	const CR_ALLOCATOR memory_allocator = allocator->head.allocator;
	cr_string_free(&memory_allocator, allocator->head.debug_name);
	CR_QUARANTINE_KEEP(quarantine, allocator);
	allocator->~crgpu_command_allocator_d3d12();
	CR_RETIRE(quarantine, &memory_allocator, crgpu_command_allocator_d3d12, allocator);
}

CR_RESULT
crgpu_command_allocator_reset(crgpu_command_allocator allocator) {
	if (!allocator)
		return CR_RESULT_GPU_INVALID_COMMAND_ALLOCATOR;
	if (CR_STALE(allocator, "crgpu_command_allocator_reset", "allocator"))
		return CR_RESULT_GPU_INVALID_COMMAND_ALLOCATOR;
#if _DEBUG
	// crgpu.h: no list of the allocator is open when it is reset.
	assert(!allocator->_open_list && "crgpu_command_allocator_reset: a list of this allocator is still open");
#endif
	if (FAILED(allocator->_d3d_cmd_allocator->Reset()))
		return CR_RESULT_API_SPECIFIC_FAILURE;
	return CR_SUCCESS;
}

CR_RESULT crgpu_command_list_create(
    crgpu_command_allocator allocator, crgpu_command_list *out_cmd_list) {
	if (!allocator)
		return CR_RESULT_GPU_INVALID_COMMAND_ALLOCATOR;
	if (CR_STALE(allocator, "crgpu_command_list_create", "allocator"))
		return CR_RESULT_GPU_INVALID_COMMAND_ALLOCATOR;
    if (!out_cmd_list)
        return CR_RESULT_NULL_OUTPUT_PARAMETER;
    crgpu_command_list_d3d12 *pResult = CR_NEW(&allocator->_device->head.allocator, crgpu_command_list_d3d12);
    if (!pResult)
        return CR_RESULT_OUT_OF_MEMORY;
    pResult = new (pResult) crgpu_command_list_d3d12();
    pResult->head.allocator = allocator->_device->head.allocator;
    CR_BIRTH_IN(pResult, &allocator->_device->_quarantine);
    pResult->_allocator = allocator;
    pResult->_scratch_barriers = eq::array<D3D12_RESOURCE_BARRIER>(&allocator->_device->_eq_allocator);
    pResult->_scratch_rects = eq::array<D3D12_RECT>(&allocator->_device->_eq_allocator);
    pResult->_scratch_vb_views = eq::array<D3D12_VERTEX_BUFFER_VIEW>(&allocator->_device->_eq_allocator);
    pResult->_scratch_geometries = eq::array<D3D12_RAYTRACING_GEOMETRY_DESC>(&allocator->_device->_eq_allocator);
    pResult->_scratch_addresses = eq::array<D3D12_GPU_VIRTUAL_ADDRESS>(&allocator->_device->_eq_allocator);
    if (FAILED(pResult->_allocator->_device->_d3d_device->CreateCommandList1(
        1, allocator->_d3d_cmd_list_type, D3D12_COMMAND_LIST_FLAG_NONE,
        IID_PPV_ARGS(&pResult->_d3d_cmd_list)
    ))) {
        pResult->~crgpu_command_list_d3d12();
        CR_DELETE(&allocator->_device->head.allocator, crgpu_command_list_d3d12, pResult);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }
	CRGPU___LIVE_ADD(&allocator->_device->_live, CRGPU___OBJECT_COMMAND_LIST);
	*out_cmd_list = pResult;
    return CR_SUCCESS;
}
void crgpu_command_list_destroy(crgpu_command_list cmd) {
    if (!cmd) return;
    if (CR_STALE(cmd, "crgpu_command_list_destroy", "cmd")) return;
#if _DEBUG
    // A list destroyed open is not open any more.
    if (cmd->_allocator->_open_list == cmd)
        cmd->_allocator->_open_list = nullptr;
#endif
    CRGPU___LIVE_REMOVE(&cmd->_allocator->_device->_live, CRGPU___OBJECT_COMMAND_LIST);
    CR_KILL(cmd);
    const CR_ALLOCATOR allocator = cmd->head.allocator;
    cr_string_free(&allocator, cmd->head.debug_name);
    CR_QUARANTINE_KEEP(quarantine, cmd);
    cmd->~crgpu_command_list_d3d12();
    CR_RETIRE(quarantine, &allocator, crgpu_command_list_d3d12, cmd);
}
void crgpu_command_list_begin(crgpu_command_list cmd)
{
	if (!cmd) return;
	if (CR_STALE(cmd, "crgpu_command_list_begin", "cmd")) return;
#if _DEBUG
	// crgpu.h: one list of an allocator open at a time. D3D12 would
	// refuse the Reset below and leave the list closed.
	assert(!cmd->_allocator->_open_list && "crgpu_command_list_begin: a list of this allocator is already open; end it first");
	cmd->_allocator->_open_list = cmd;
#endif
	cmd->_d3d_cmd_list->Reset(
	    cmd->_allocator->_d3d_cmd_allocator.Get(), nullptr);
}
void crgpu_command_list_end(crgpu_command_list cmd)
{
	if (!cmd) return;
	if (CR_STALE(cmd, "crgpu_command_list_end", "cmd")) return;
	cmd->_d3d_cmd_list->Close();
#if _DEBUG
	if (cmd->_allocator->_open_list == cmd)
		cmd->_allocator->_open_list = nullptr;
#endif
}

// PIX's encoding of an event's text: metadata 1 is a null-terminated ANSI
// string (PIX_EVENT_ANSI_VERSION in pix3.h), which PIX and the debug layer
// read without WinPixEventRuntime.
static const UINT crgpu_d3d12_pix_event_ansi = 1;

void crgpu_command_list_begin_debug_label(crgpu_command_list cmd, const char *label) {
	if (!cmd) return;
	if (CR_STALE(cmd, "crgpu_command_list_begin_debug_label", "cmd")) return;
	const char *text = label ? label : "";
	cmd->_d3d_cmd_list->BeginEvent(crgpu_d3d12_pix_event_ansi, text, (UINT)(strlen(text) + 1));
}

void crgpu_command_list_end_debug_label(crgpu_command_list cmd) {
	if (!cmd) return;
	if (CR_STALE(cmd, "crgpu_command_list_end_debug_label", "cmd")) return;
	cmd->_d3d_cmd_list->EndEvent();
}
void crgpu_command_list_set_viewports(
    crgpu_command_list cmd,
    uint32_t first_viewport,
    uint32_t viewport_count,
    const CRGPU_VIEWPORT *viewports) {
	if (!cmd || !viewports || !viewport_count)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_set_viewports", "cmd"))
		return;
	//D3D12_VIEWPORT *d3dViewports;
	cmd->_d3d_cmd_list->RSSetViewports(
	    static_cast<UINT>(viewport_count),
	    reinterpret_cast<D3D12_VIEWPORT *>(
	        const_cast<CRGPU_VIEWPORT *>(viewports)));
}
void crgpu_command_list_set_scissor_rects(
    crgpu_command_list cmd,
    uint32_t first_scissor_rect,
    uint32_t scissor_rect_count,
    const CRGPU_SCISSOR_RECT *scissor_rects) {
	if (!cmd || !scissor_rects || !scissor_rect_count)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_set_scissor_rects", "cmd"))
		return;
	// CRGPU_SCISSOR_RECT is {x, y, width, height} floats; D3D12_RECT is
	// {left, top, right, bottom} LONGs -- not layout-compatible, needs a
	// real per-rect conversion rather than a reinterpret_cast.
	cmd->_scratch_rects.clear();
	if (cmd->_scratch_rects.resize(scissor_rect_count) != EQ_SUCCESS)
		return;
	D3D12_RECT *d3dRects = cmd->_scratch_rects.data;
	for (uint32_t i = 0; i < scissor_rect_count; ++i) {
		d3dRects[i].left   = (LONG)scissor_rects[i].x;
		d3dRects[i].top    = (LONG)scissor_rects[i].y;
		d3dRects[i].right  = (LONG)(scissor_rects[i].x + scissor_rects[i].width);
		d3dRects[i].bottom = (LONG)(scissor_rects[i].y + scissor_rects[i].height);
	}
	cmd->_d3d_cmd_list->RSSetScissorRects(
	    static_cast<UINT>(scissor_rect_count), d3dRects);
}
void crgpu_command_list_set_vertex_buffers(
    crgpu_command_list cmd,
    uint32_t start_slot,
    uint32_t view_count,
    crgpu_buffer_view *buffer_views) {
	if (!cmd || !view_count || !buffer_views)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_set_vertex_buffers", "cmd"))
		return;
	cmd->_scratch_vb_views.clear();
	if (cmd->_scratch_vb_views.resize(view_count) != EQ_SUCCESS)
		return;
	D3D12_VERTEX_BUFFER_VIEW *d3dVbViews = cmd->_scratch_vb_views.data;
	for (uint32_t i = 0; i < view_count; ++i)
		d3dVbViews[i] = buffer_views[i]->_d3d_view._vb;
	cmd->_d3d_cmd_list->IASetVertexBuffers(
		static_cast<UINT>(start_slot),
		static_cast<UINT>(view_count),
		d3dVbViews
	);
}
void crgpu_command_list_set_index_buffer(
    crgpu_command_list cmd,
    crgpu_buffer_view buffer_view) {
	if (!cmd || !buffer_view)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_set_index_buffer", "cmd"))
		return;
	if (CR_STALE(buffer_view, "crgpu_command_list_set_index_buffer", "buffer_view"))
		return;
	cmd->_d3d_cmd_list->IASetIndexBuffer(&buffer_view->_d3d_view._ib);
}
void crgpu_command_list_draw(
    crgpu_command_list cmd,
    uint32_t vertex_count,
    uint32_t start_vertex_location) {
	if (!cmd)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_draw", "cmd"))
		return;
	cmd->_d3d_cmd_list->DrawInstanced(
	    vertex_count, 1, start_vertex_location, 0);
}
void crgpu_command_list_draw_indexed(
    crgpu_command_list cmd,
    uint32_t index_count,
    uint32_t start_index_location,
    int32_t base_vertex_location) {
	if (!cmd)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_draw_indexed", "cmd"))
		return;
	cmd->_d3d_cmd_list->DrawIndexedInstanced(
		index_count, 1, start_index_location, base_vertex_location, 0);
}
void crgpu_command_list_draw_instanced(
    crgpu_command_list cmd,
    uint32_t vertex_count_per_instance,
    uint32_t instance_count,
    uint32_t start_vertex_location,
    uint32_t start_instance_location) {
	if (!cmd)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_draw_instanced", "cmd"))
		return;
	cmd->_d3d_cmd_list->DrawInstanced(
		vertex_count_per_instance,
		instance_count,
		start_vertex_location,
		start_instance_location);
}
void crgpu_command_list_draw_indexed_instanced(
    crgpu_command_list cmd,
    uint32_t index_count_per_instance,
    uint32_t instance_count,
    uint32_t start_index_location,
    int32_t base_vertex_location,
    uint32_t start_instance_location) {
	if (!cmd)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_draw_indexed_instanced", "cmd"))
		return;
	cmd->_d3d_cmd_list->DrawIndexedInstanced(
		index_count_per_instance,
		instance_count,
		start_index_location,
		base_vertex_location,
		start_instance_location);
}
/*
void crgpu_command_list_bind_graphics_pipeline(
    crgpu_command_list cmd,
    crgpu_graphics_pipeline pipeline) {
	if (!cmd || !pipeline)
		return;
	ID3D12GraphicsCommandList *cmdList = cmd->_d3d_cmd_list.Get();
	cmdList->SetPipelineState(pipeline->_d3d_pipeline_state.Get());
	cmdList->SetGraphicsRootSignature(pipeline->_d3d_root_signature.Get());
	cmdList->IASetPrimitiveTopology(pipeline->_d3d_primitive_topology);
}
void crgpu_command_list_push_graphics_constants(
    crgpu_command_list cmd,
    crgpu_graphics_pipeline pipeline,
    uint32_t offset,
    uint32_t size,
    const void *data) {
	if (!cmd || !pipeline || !data || size == 0)
		return;
	cmd->_d3d_cmd_list->SetGraphicsRoot32BitConstants(
	    0,
	    size / sizeof(uint32_t),
	    data,
	    offset / sizeof(uint32_t));
}*/

void crgpu_command_list_dispatch(
    crgpu_command_list cmd,
    uint32_t group_count_x,
    uint32_t group_count_y,
    uint32_t group_count_z) {
	if (!cmd)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_dispatch", "cmd"))
		return;
	cmd->_d3d_cmd_list->Dispatch(group_count_x, group_count_y, group_count_z);
}

#if CRGPU_EXT_MESH_SHADING
void crgpu_command_list_dispatch_mesh(
    crgpu_command_list cmd,
    uint32_t group_count_x,
    uint32_t group_count_y,
    uint32_t group_count_z) {
	if (!cmd)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_dispatch_mesh", "cmd"))
		return;
	// DispatchMesh is declared on ID3D12GraphicsCommandList6 - _d3d_cmd_list
	// is stored as the newer ID3D12GraphicsCommandList10 (crgpud3d12.hpp),
	// which already inherits it, so no QueryInterface is needed here.
	cmd->_d3d_cmd_list->DispatchMesh(group_count_x, group_count_y, group_count_z);
}
#endif

#if CRGPU_EXT_DEPTH_BOUNDS_TEST
void crgpu_command_list_set_depth_bounds(
    crgpu_command_list cmd,
    float min_depth_bounds,
    float max_depth_bounds) {
	if (!cmd)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_set_depth_bounds", "cmd"))
		return;
	// Without the feature the device may not have the test (OPTIONS2), and
	// the device has exactly what it was created with.
	const bool enabled = (cmd->_allocator->_device->_features & CRGPU_FEATURE_DEPTH_BOUNDS_TEST) != 0;
	assert(enabled && "crgpu_command_list_set_depth_bounds: the device was created without CRGPU_FEATURE_DEPTH_BOUNDS_TEST");
	if (!enabled)
		return;
	// OMSetDepthBounds is declared on ID3D12GraphicsCommandList1 - _d3d_cmd_list
	// is stored as the newer ID3D12GraphicsCommandList10 (crgpud3d12.hpp),
	// which already inherits it, so no QueryInterface is needed here.
	cmd->_d3d_cmd_list->OMSetDepthBounds(min_depth_bounds, max_depth_bounds);
}
#endif

} // extern "C"

ID3D12CommandSignature *crgpu_d3d12___create_draw_indirect_signature(
    ID3D12Device *d3d_device, bool indexed, uint32_t stride) {
	D3D12_INDIRECT_ARGUMENT_DESC argument_desc = {};
	argument_desc.Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED
	                              : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

	D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
	signature_desc.pArgumentDescs = &argument_desc;
	signature_desc.NumArgumentDescs = 1;
	signature_desc.ByteStride       = stride;

	// rootSignature is nullptr: only valid (and required) when every argument
	// in the signature is DRAW/DRAW_INDEXED/DISPATCH — no root-parameter
	// update arguments, which Carrottus GPU's indirect draw never uses.
	ID3D12CommandSignature *signature = nullptr;
	if (FAILED(d3d_device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature))))
		return nullptr;
	return signature;
}

// The root-signature-less command signature crgpu_command_list_draw_indirect /
// _indexed_indirect execute with, for one kind of draw and one stride.
// Mechanical translation, not a policy choice: D3D12 needs this object for
// any ExecuteIndirect call, and its ByteStride is where the caller's stride
// goes - ExecuteIndirect takes no stride of its own. The tightly packed pair
// is made with the device and read with no lock; any other stride is made
// and kept on the device the first time it is asked for. Null if it can't be
// made.
static ID3D12CommandSignature *crgpu_d3d12___get_draw_indirect_signature(
    crgpu_device_d3d12 *device, bool indexed, uint32_t stride) {
	if (!indexed && stride == sizeof(crgpu_draw_indirect_args))
		return device->_tight_draw_signature;
	if (indexed && stride == sizeof(crgpu_draw_indexed_indirect_args))
		return device->_tight_draw_indexed_signature;
	std::lock_guard<std::mutex> lock(device->_indirect_signature_mutex);
	crgpu_d3d12_indirect_signatures &kept = device->_indirect_signatures;
	for (size_t i = 0; i < kept.count; ++i)
		if (kept.data[i].indexed == indexed && kept.data[i].stride == stride)
			return kept.data[i].signature;

	// Room first, doubling from 4, so that a signature made is never one the
	// array can't hold. Out of memory: the array is untouched and still the
	// device's.
	if (kept.count == kept.capacity && eq_array_reserve(&kept, kept.capacity ? kept.capacity * 2 : 4) != EQ_SUCCESS)
		return nullptr;
	ID3D12CommandSignature *signature =
	    crgpu_d3d12___create_draw_indirect_signature(device->_d3d_device.Get(), indexed, stride);
	if (!signature)
		return nullptr;
	const crgpu_d3d12_indirect_signature added = {signature, stride, indexed};
	// Room was made above, so the push can't fail; it is still checked.
	if (eq_array_push(&kept, &added) != EQ_SUCCESS) {
		signature->Release();
		return nullptr;
	}
	return signature;
}

extern "C" {

void crgpu_command_list_draw_indirect(
    crgpu_command_list cmd,
    crgpu_buffer buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride) {
	if (!cmd || !buffer || draw_count == 0)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_draw_indirect", "cmd"))
		return;
	if (CR_STALE(buffer, "crgpu_command_list_draw_indirect", "buffer"))
		return;
	if (draw_count > crgpu_d3d12___max_draw_indirect_count(cmd->_allocator->_device))
		return;
	if (stride % 4 != 0 || stride < sizeof(crgpu_draw_indirect_args))
		return;
	ID3D12CommandSignature *signature =
	    crgpu_d3d12___get_draw_indirect_signature(cmd->_allocator->_device, false, stride);
	if (!signature)
		return;
	cmd->_d3d_cmd_list->ExecuteIndirect(
	    signature, draw_count, buffer->_d3d_resource.Get(), offset, nullptr, 0);
}

void crgpu_command_list_draw_indexed_indirect(
    crgpu_command_list cmd,
    crgpu_buffer buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride) {
	if (!cmd || !buffer || draw_count == 0)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_draw_indexed_indirect", "cmd"))
		return;
	if (CR_STALE(buffer, "crgpu_command_list_draw_indexed_indirect", "buffer"))
		return;
	if (draw_count > crgpu_d3d12___max_draw_indirect_count(cmd->_allocator->_device))
		return;
	if (stride % 4 != 0 || stride < sizeof(crgpu_draw_indexed_indirect_args))
		return;
	ID3D12CommandSignature *signature =
	    crgpu_d3d12___get_draw_indirect_signature(cmd->_allocator->_device, true, stride);
	if (!signature)
		return;
	cmd->_d3d_cmd_list->ExecuteIndirect(
	    signature, draw_count, buffer->_d3d_resource.Get(), offset, nullptr, 0);
}

// ---------------------------------------------------------------------------
// crgpu_command_list_pipeline_barrier — CRGPU_RESOURCE_STATE -> D3D12_RESOURCE_STATES.
// Distinct from the image-only crgpu_command_list_image_barrier mechanism (which
// uses CRGPU_IMAGE_LAYOUT and already works for swap chain/render target
// transitions) — this is the more general resource-state model that also
// covers buffers.
// ---------------------------------------------------------------------------

static D3D12_RESOURCE_STATES crgpu_d3d12___resource_state(CRGPU_RESOURCE_STATE state) {
	switch (state) {
	case CRGPU_RESOURCE_STATE_VERTEX_BUFFER:
		return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
	case CRGPU_RESOURCE_STATE_INDEX_BUFFER:
		return D3D12_RESOURCE_STATE_INDEX_BUFFER;
	case CRGPU_RESOURCE_STATE_RENDER_TARGET:
		return D3D12_RESOURCE_STATE_RENDER_TARGET;
	case CRGPU_RESOURCE_STATE_DEPTH_WRITE:
		return D3D12_RESOURCE_STATE_DEPTH_WRITE;
	case CRGPU_RESOURCE_STATE_SHADER_RESOURCE:
		return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	case CRGPU_RESOURCE_STATE_UNORDERED_ACCESS:
		return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	case CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT:
		return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
	case CRGPU_RESOURCE_STATE_PRESENT:
		return D3D12_RESOURCE_STATE_PRESENT;
	case CRGPU_RESOURCE_STATE_COPY_SOURCE:
		return D3D12_RESOURCE_STATE_COPY_SOURCE;
	case CRGPU_RESOURCE_STATE_COPY_DEST:
		return D3D12_RESOURCE_STATE_COPY_DEST;
	// A build reads its inputs as a non-pixel shader resource and uses its
	// scratch, and a size write its destination, as unordered access.
	case CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT:
		return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	case CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH:
	case CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SIZE_DEST:
		return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	case CRGPU_RESOURCE_STATE_COMMON:
	default:
		return D3D12_RESOURCE_STATE_COMMON;
	}
}

void crgpu_command_list_pipeline_barrier(
    crgpu_command_list cmd,
    const crgpu_buffer_barrier *buffer_barriers,
    uint32_t buffer_barrier_count,
    const crgpu_image_barrier *image_barriers,
    uint32_t image_barrier_count) {
	if (!cmd)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_pipeline_barrier", "cmd"))
		return;
	if (buffer_barrier_count > 0 && !buffer_barriers)
		return;
	for (uint32_t buffer_barriers_i = 0; buffer_barriers_i < buffer_barrier_count; ++buffer_barriers_i)
		if (cr_struct_check(&buffer_barriers[buffer_barriers_i].base, CR_STRUCT_TYPE_GPU_BUFFER_BARRIER) != CR_SUCCESS)
			return;
	if (image_barrier_count > 0 && !image_barriers)
		return;
	for (uint32_t image_barriers_i = 0; image_barriers_i < image_barrier_count; ++image_barriers_i)
		if (cr_struct_check(&image_barriers[image_barriers_i].base, CR_STRUCT_TYPE_GPU_IMAGE_BARRIER) != CR_SUCCESS)
			return;
	uint32_t total = buffer_barrier_count + image_barrier_count;
	if (total == 0)
		return;

	// Room for every barrier first, so that none of the pushes below can
	// fail. Out of memory: nothing is recorded.
	eq::array<D3D12_RESOURCE_BARRIER> &barriers = cmd->_scratch_barriers;
	barriers.clear();
	if (barriers.reserve(total) != EQ_SUCCESS) {
		printf("crgpu: crgpu_command_list_pipeline_barrier - out of memory building barriers; nothing recorded\n");
		return;
	}

	for (uint32_t i = 0; i < buffer_barrier_count; i++) {
		const crgpu_buffer_barrier &b = buffer_barriers[i];
		D3D12_RESOURCE_STATES before = crgpu_d3d12___resource_state(b.state_before);
		D3D12_RESOURCE_STATES after = crgpu_d3d12___resource_state(b.state_after);
		// Two states that are both D3D12's UNORDERED_ACCESS (storage, a
		// build's scratch, a size write's destination), the same or not,
		// order the writes before against the accesses after, which Vulkan
		// does with its stages: D3D12 has no transition between them, so a
		// UAV barrier orders it. SCRATCH to SCRATCH orders two builds sharing
		// a scratch range (card S25).
		if (before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
			(void)barriers.push(CD3DX12_RESOURCE_BARRIER::UAV(b.buffer->_d3d_resource.Get()));
			continue;
		}
		// Any other pair of equal states does nothing, as the header says.
		if (before == after)
			continue;
		(void)barriers.push(CD3DX12_RESOURCE_BARRIER::Transition(
		    b.buffer->_d3d_resource.Get(), before, after));
	}
	for (uint32_t i = 0; i < image_barrier_count; i++) {
		const crgpu_image_barrier &b = image_barriers[i];
		// As for a buffer, above.
		if (b.state_before == CRGPU_RESOURCE_STATE_UNORDERED_ACCESS &&
		    b.state_after == CRGPU_RESOURCE_STATE_UNORDERED_ACCESS) {
			(void)barriers.push(CD3DX12_RESOURCE_BARRIER::UAV(b.image->_d3d_resource.Get()));
			continue;
		}
		D3D12_RESOURCE_STATES before = crgpu_d3d12___resource_state(b.state_before);
		D3D12_RESOURCE_STATES after = crgpu_d3d12___resource_state(b.state_after);
		if (before == after)
			continue;
		(void)barriers.push(CD3DX12_RESOURCE_BARRIER::Transition(
		    b.image->_d3d_resource.Get(), before, after));
	}

	if (!barriers.empty())
		cmd->_d3d_cmd_list->ResourceBarrier(
		    static_cast<UINT>(barriers.size()), barriers.data);
}
CR_RESULT crgpu_command_queue_submit(
    crgpu_command_queue queue, const crgpu_submit_desc *desc) {
	if (!queue)
		return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
	if (CR_STALE(queue, "crgpu_command_queue_submit", "queue"))
		return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_SUBMIT_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!desc->cmd_lists)
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	if (desc->cmd_list_count == 0)
		return CR_RESULT_ZERO_SIZE;
	if (FAILED(queue->_device->_d3d_device->GetDeviceRemovedReason()))
		return CR_RESULT_GPU_DEVICE_LOST;
	// The waits, the lists and the signals are queued by separate calls, so
	// the whole of this is one use of the queue (crgpu.h).
	crgpu_d3d12___queue_enter(queue);
	// Room for the lists before the waits are queued, so a failed
	// allocation leaves the queue untouched.
	queue->_scratch_lists.clear();
	if (queue->_scratch_lists.reserve(desc->cmd_list_count) != EQ_SUCCESS) {
		crgpu_d3d12___queue_leave(queue);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	for (uint32_t i = 0; i < desc->wait_semaphore_count; ++i) {
		crgpu_semaphore sem = desc->wait_semaphores[i];
		queue->_d3d_cmd_queue->Wait(sem->_d3d_fence.Get(), sem->_signal_value);
	}
	for (uint32_t i = 0; i < desc->cmd_list_count; ++i)
		(void)queue->_scratch_lists.push(desc->cmd_lists[i]->_d3d_cmd_list.Get());
	queue->_d3d_cmd_queue->ExecuteCommandLists(
	    static_cast<UINT>(desc->cmd_list_count), queue->_scratch_lists.data);

	// Everything below is queued behind the command lists, so it is
	// signaled once they have run.
	CR_RESULT result = CR_SUCCESS;
	for (uint32_t i = 0; i < desc->signal_semaphore_count; ++i) {
		crgpu_semaphore sem = desc->signal_semaphores[i];
		if (FAILED(queue->_d3d_cmd_queue->Signal(sem->_d3d_fence.Get(), ++sem->_signal_value)))
			result = CR_RESULT_API_SPECIFIC_FAILURE;
	}
	if (desc->swap_chain) {
		// See crgpu_swap_chain_d3d12 (crgpud3d12.hpp) for why this is one
		// monotonic value per submit rather than Vulkan/Metal's
		// per-back-buffer bookkeeping, and
		// crgpu_swap_chain_acquire_next_image (crgpud3d12swapchain.cpp)
		// for where it gets waited on.
		uint64_t value = ++desc->swap_chain->_frame_fence_next_value;
		if (FAILED(queue->_d3d_cmd_queue->Signal(desc->swap_chain->_frame_fence.Get(), value)))
			result = CR_RESULT_API_SPECIFIC_FAILURE;
	}
	if (desc->signal_fence) {
		if (FAILED(queue->_d3d_cmd_queue->Signal(
		        desc->signal_fence->_d3d_fence.Get(), desc->signal_fence_value)))
			result = CR_RESULT_API_SPECIFIC_FAILURE;
	}
	crgpu_d3d12___queue_leave(queue);
	// A signal refused because the device was removed meanwhile.
	if (result != CR_SUCCESS && FAILED(queue->_device->_d3d_device->GetDeviceRemovedReason()))
		return CR_RESULT_GPU_DEVICE_LOST;
	return result;
}
void crgpu_command_queue_wait_idle(crgpu_command_queue queue) {
	if (!queue)
		return;
	if (CR_STALE(queue, "crgpu_command_queue_wait_idle", "queue"))
		return;
	Microsoft::WRL::ComPtr<ID3D12Fence> fence;
	if (FAILED(queue->_device->_d3d_device->CreateFence(
	        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
		return;
	// The whole wait is a use of the queue (crgpu.h).
	crgpu_d3d12___queue_enter(queue);
	queue->_d3d_cmd_queue->Signal(fence.Get(), 1);
	if (fence->GetCompletedValue() < 1) {
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		fence->SetEventOnCompletion(1, event);
		WaitForSingleObject(event, INFINITE);
		CloseHandle(event);
	}
	crgpu_d3d12___queue_leave(queue);
}

#if CRGPU_EXT_BUNDLES
void crgpu_command_list_begin_bundle(
    crgpu_command_list bundle, const crgpu_bundle_inheritance_desc *desc) {
	// D3D12 bundles need no attachment info up front - they inherit whatever
	// render targets are bound on the direct list that executes them, and its
	// viewport and scissor, which a bundle may not set. desc's viewport and
	// scissor_rect are for Vulkan, whose secondary buffers inherit neither.
	if (!bundle || !desc)
		return;
	if (CR_STALE(bundle, "crgpu_command_list_begin_bundle", "bundle"))
		return;
	if (cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_BUNDLE_INHERITANCE_DESC) != CR_SUCCESS)
		return;
	crgpu_command_list_begin(bundle);
}

void crgpu_command_list_execute_bundle(
    crgpu_command_list cmd, crgpu_command_list bundle) {
	if (!cmd || !bundle)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_execute_bundle", "cmd"))
		return;
	if (CR_STALE(bundle, "crgpu_command_list_execute_bundle", "bundle"))
		return;
	cmd->_d3d_cmd_list->ExecuteBundle(bundle->_d3d_cmd_list.Get());
}
#endif // CRGPU_EXT_BUNDLES
}

#endif // CRGPU_COMPILE_D3D12_API
