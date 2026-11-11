
#if CRGPU_COMPILE_D3D12_API

#include "crgpu_d3d12_natives.h"
#include "crgpud3d12.hpp"

extern "C" {

IDXGIFactory7 *crgpu_get_dxgi_factory(crgpu_instance instance) {
    return instance ? instance->_dxgi_factory.Get() : nullptr;
}

IDXGIAdapter4 *crgpu_get_dxgi_adapter(crgpu_adapter adapter) {
    return adapter ? adapter->_dxgi_adapter.Get() : nullptr;
}
ID3D12Device14 *crgpu_get_d3d12_device(crgpu_device device) {
    return device ? device->_d3d_device.Get() : nullptr;
}

ID3D12CommandQueue *
crgpu_get_d3d12_command_queue(crgpu_command_queue queue) {
    return queue ? queue->_d3d_cmd_queue.Get() : nullptr;
}

ID3D12CommandAllocator *
crgpu_get_d3d12_command_allocator(crgpu_command_allocator allocator) {
    return allocator ? allocator->_d3d_cmd_allocator.Get() : nullptr;
}

ID3D12GraphicsCommandList10 *
crgpu_get_d3d12_graphics_command_list(crgpu_command_list cmd) {
    return cmd ? cmd->_d3d_cmd_list.Get() : nullptr;
}

ID3D12Resource2 *crgpu_get_d3d12_buffer(crgpu_buffer buffer) {
    return buffer ? buffer->_d3d_resource.Get() : nullptr;
}
bool crgpu_get_d3d12_buffer_view(
    crgpu_buffer_view view,
    CRGPU_D3D12_BUFFER_VIEW *out) {
	if (view && out) {
		*out = view->_d3d_view;
		return true;
	} else
		return false;
}

ID3D12Heap *crgpu_get_d3d12_heap(crgpu_heap heap) {
    return heap ? heap->_d3d_heap.Get() : nullptr;
}
ID3D12Resource2 *crgpu_get_d3d12_image(crgpu_image image) {
    return image ? image->_d3d_resource.Get() : nullptr;
}
bool crgpu_get_d3d12_image_view(
    crgpu_image_view view, CRGPU_D3D12_IMAGE_VIEW *out) {
	if (view && out) {
		*out = view->_d3d_view_desc;
		return true;
	} else
		return false;
}

ID3D12RootSignature *crgpu_get_d3d12_root_signature(crgpu_graphics_pipeline pipeline) {
    return pipeline ? pipeline->_d3d_root_signature.Get() : nullptr;
}
ID3D12PipelineState *crgpu_get_d3d12_pipeline_state(crgpu_graphics_pipeline pipeline) {
    return pipeline ? pipeline->_d3d_pipeline_state.Get() : nullptr;
}

ID3D12Fence1 *crgpu_get_d3d12_fence(crgpu_fence fence) {
    return fence ? fence->_d3d_fence.Get() : nullptr;
}
ID3D12Fence1 *crgpu_get_d3d12_fence_from_semaphore(crgpu_semaphore semaphore) {
    return semaphore ? semaphore->_d3d_fence.Get() : nullptr;
}

} // extern "C"

#endif // CRGPU_COMPILE_D3D12_API
