//
// Carrottus GPU Metal backend - implements the escape-hatch accessors declared in
// include/crgpu_metal_natives.h.
// Not yet implemented: no Apple hardware available to develop/verify against.
// Wired into the carrottus CMake target ahead of time - see CMakeLists.txt.
//

#if CRGPU_COMPILE_METAL_API

#include "crgpu.h"
#include "crcore.h"
#include "crgpumetal.h"
#include "crgpu_metal_natives.h"
#include <Metal/Metal.h>

id<MTLDevice> crgpu_get_mtl_device(crgpu_device device) {
	crgpu_device_metal *device_metal = (crgpu_device_metal *)device;
	if (!device_metal)
		return nil;
	return device_metal->_device_mtl;
}

id<MTLDevice> crgpu_get_mtl_adapter_device(crgpu_adapter adapter) {
	crgpu_adapter_metal *adapter_metal = (crgpu_adapter_metal *)adapter;
	if (!adapter_metal)
		return nil;
	return adapter_metal->_device_mtl;
}

id<MTLCommandQueue> crgpu_get_mtl_command_queue(crgpu_command_queue queue) {
	crgpu_command_queue_metal *queue_metal = (crgpu_command_queue_metal *)queue;
	if (!queue_metal)
		return nil;
	return queue_metal->_command_queue_mtl;
}

id<MTLCommandBuffer> crgpu_get_mtl_command_buffer(crgpu_command_list cmd) {
	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	if (!cmd_metal)
		return nil;
	return cmd_metal->_command_buffer_mtl;
}

id<MTLBuffer> crgpu_get_mtl_buffer(crgpu_buffer buffer) {
	crgpu_buffer_metal *buffer_metal = (crgpu_buffer_metal *)buffer;
	if (!buffer_metal)
		return nil;
	return buffer_metal->_buffer_mtl;
}

bool crgpu_get_mtl_buffer_view(
    crgpu_buffer_view view,
    CRGPU_MTL_BUFFER_VIEW *out) {
	crgpu_buffer_view_metal *view_metal = (crgpu_buffer_view_metal *)view;
	if (!view_metal || !out)
		return false;
	crgpu_buffer_metal *parent_buffer = (crgpu_buffer_metal *)view_metal->_desc.parent_buffer;
	if (!parent_buffer)
		return false;

	out->buffer = parent_buffer->_buffer_mtl;
	out->offset = (NSUInteger)view_metal->_desc.offset_from_parent_buffer;
	out->length = (NSUInteger)view_metal->_desc.size;
	out->stride = (NSUInteger)view_metal->_desc.stride;
	return true;
}

id<MTLTexture> crgpu_get_mtl_texture(crgpu_image image) {
	crgpu_image_metal *image_metal = (crgpu_image_metal *)image;
	if (!image_metal)
		return nil;
	return image_metal->_texture_mtl;
}

id<MTLHeap> crgpu_get_mtl_heap(crgpu_heap heap) {
	crgpu_heap_metal *heap_metal = (crgpu_heap_metal *)heap;
	if (!heap_metal)
		return nil;
	return heap_metal->_heap_mtl;
}

id<MTLTexture> crgpu_get_mtl_texture_view(crgpu_image_view view) {
	crgpu_image_view_metal *view_metal = (crgpu_image_view_metal *)view;
	if (!view_metal)
		return nil;
	return view_metal->_texture_view_mtl;
}

id<MTLRenderPipelineState> crgpu_get_mtl_render_pipeline_state(crgpu_graphics_pipeline pipeline) {
	crgpu_shader_pipeline_metal *pipeline_metal = (crgpu_shader_pipeline_metal *)pipeline;
	if (!pipeline_metal)
		return nil;
	return pipeline_metal->_render_pipeline_state_mtl;
}

id<MTLSharedEvent> crgpu_get_mtl_shared_event(crgpu_fence fence) {
	crgpu_fence_metal *fence_metal = (crgpu_fence_metal *)fence;
	if (!fence_metal)
		return nil;
	return fence_metal->_shared_event_mtl;
}

id<MTLEvent> crgpu_get_mtl_event_from_semaphore(crgpu_semaphore semaphore) {
	crgpu_semaphore_metal *semaphore_metal = (crgpu_semaphore_metal *)semaphore;
	if (!semaphore_metal)
		return nil;
	return semaphore_metal->_event_mtl;
}

MTLPixelFormat crgpu_get_mtl_swap_chain_format(crgpu_swap_chain swap_chain) {
	crgpu_swap_chain_metal *swap_chain_metal = (crgpu_swap_chain_metal *)swap_chain;
	if (!swap_chain_metal)
		return MTLPixelFormatInvalid;

	// The layer's pixel format, as crgpu_swap_chain_create set it from the
	// format table.
	return crgpumetal___format_info(swap_chain_metal->_desc.format)->pixel;
}

#endif // CRGPU_COMPILE_METAL_API
