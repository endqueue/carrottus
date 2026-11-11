/**
 * @file crgpu_metal_natives.h
 * @brief Escape hatch: the raw Metal objects behind Carrottus GPU's opaque
 * objects.
 *
 * Use only for interop with third-party Metal libraries (e.g. ImGui,
 * MetalFX). Carrottus GPU has no visibility into Metal calls made with these
 * handles. Declared only when CRGPU_COMPILE_METAL_API is set.
 *
 * Every accessor returns an object Carrottus GPU still owns: it stays valid for as
 * long as the Carrottus GPU object it came from. Each returns nil (or the value
 * its own doc names) when given a NULL object.
 *
 * Metal's object model doesn't line up 1:1 with the Vulkan/D3D12 natives
 * headers in a few places - see the notes below rather than assuming a
 * missing accessor here is an oversight:
 *
 * - No crgpu_get_mtl_instance(): Metal has no top-level instance/factory
 *   object (unlike VkInstance or IDXGIFactory). Devices are enumerated
 *   directly via MTLCopyAllDevices(), so crgpu_instance has no native
 *   Metal handle to hand back.
 * - No crgpu_get_mtl_queue_family_index(): Metal command queues aren't
 *   partitioned into families the way Vulkan queues are.
 * - No native accessor for crgpu_command_allocator: Metal has no
 *   persistent allocator/pool object to reset between frames - command
 *   buffers are requested fresh from the queue each time.
 * - No crgpu_get_mtl_root_signature(): Metal has no runtime
 *   root-signature / pipeline-layout object - binding indices come from
 *   [[buffer(n)]] / [[texture(n)]] attributes baked into the shader source
 *   itself.
 */

#ifndef CARROTTUS_CRGPU_METAL_NATIVES_H
#define CARROTTUS_CRGPU_METAL_NATIVES_H

#if CRGPU_COMPILE_METAL_API

#include <Metal/Metal.h>
#include <crgpu.h>

#ifdef __cplusplus
extern "C" {

#endif

/**
 * @brief Returns the id<MTLDevice> behind @p device.
 *
 * @param[in] device The Carrottus GPU device.
 *
 * @return Its id<MTLDevice>, or nil if @p device is NULL.
 *
 * @see crgpu_get_mtl_adapter_device()
 */
id<MTLDevice> crgpu_get_mtl_device(crgpu_device device);

/**
 * @brief Returns the id<MTLDevice> behind @p adapter.
 *
 * No physical/logical device split: id<MTLDevice> already covers both
 * roles Vulkan splits into VkPhysicalDevice/VkDevice, so an adapter and the
 * device created on it hand back the same id<MTLDevice>.
 *
 * @param[in] adapter The adapter, as crgpu_adapter_enumerate() gave it.
 *
 * @return Its id<MTLDevice>, or nil if @p adapter is NULL.
 */
id<MTLDevice> crgpu_get_mtl_adapter_device(crgpu_adapter adapter);

/**
 * @brief Returns the id<MTLCommandQueue> behind @p queue.
 *
 * The caller synchronizes a queue (see crgpu_command_queue_submit_lists()), and
 * a use of the id<MTLCommandQueue> itself is one more use of the same queue
 * under the same rule.
 *
 * @param[in] queue The Carrottus GPU command queue.
 *
 * @return Its id<MTLCommandQueue>, or nil if @p queue is NULL.
 */
id<MTLCommandQueue> crgpu_get_mtl_command_queue(crgpu_command_queue queue);

/**
 * @brief Returns the id<MTLCommandBuffer> behind @p cmd.
 *
 * @param[in] cmd The Carrottus GPU command list.
 *
 * @return Its id<MTLCommandBuffer>, or nil if @p cmd is NULL.
 */
id<MTLCommandBuffer> crgpu_get_mtl_command_buffer(crgpu_command_list cmd);

/**
 * @brief Returns the id<MTLBuffer> behind @p buffer.
 *
 * @param[in] buffer The Carrottus GPU buffer.
 *
 * @return Its id<MTLBuffer>, or nil if @p buffer is NULL.
 */
id<MTLBuffer> crgpu_get_mtl_buffer(crgpu_buffer buffer);

/**
 * @brief What a Carrottus GPU buffer view is on Metal, as filled in by
 * crgpu_get_mtl_buffer_view().
 *
 * Metal has no separate buffer-view object (Vulkan's VkBufferView, D3D12's
 * `*_BUFFER_VIEW` descs) - buffers are bound directly with an offset/stride
 * at bind time. This struct carries the same information out of Carrottus GPU's
 * own view abstraction instead of a native handle.
 */
/* Frozen: a fact of the Metal API. It never changes; a new need gets a new type. */
typedef struct CRGPU_MTL_BUFFER_VIEW {
	/** The parent buffer's id<MTLBuffer>. */
	id<MTLBuffer> buffer;
	/** Byte offset of the view in #buffer. */
	NSUInteger offset;
	/** Size of the view in bytes. */
	NSUInteger length;
	/** Byte stride of one element, as the view was created with. */
	NSUInteger stride;
} CRGPU_MTL_BUFFER_VIEW;
/**
 * @brief Fills @p out with the buffer, offset, length and stride of @p view.
 *
 * @param[in]  view The Carrottus GPU buffer view.
 * @param[out] out  Receives the view.
 *
 * @return true if @p out was written; false if @p view or @p out is NULL or
 *         the view has no parent buffer.
 */
bool crgpu_get_mtl_buffer_view(
    crgpu_buffer_view view,
    CRGPU_MTL_BUFFER_VIEW *out);

/**
 * @brief Returns the id<MTLTexture> behind @p image.
 *
 * @param[in] image The Carrottus GPU image.
 *
 * @return Its id<MTLTexture>, or nil if @p image is NULL.
 *
 * @see crgpu_get_mtl_texture_view()
 */
id<MTLTexture> crgpu_get_mtl_texture(crgpu_image image);

/**
 * @brief Returns the id<MTLTexture> behind the image view @p view.
 *
 * Metal represents a "view of a texture" (reinterpreted format/mip/array
 * range, via -newTextureViewWithPixelFormat:...) as another
 * id<MTLTexture>, not a distinct lightweight descriptor type the way
 * Vulkan's VkImageView or D3D12's `*_VIEW_DESC` structs are - so this
 * returns the same native type as crgpu_get_mtl_texture(), just for the
 * view object rather than the base image.
 *
 * @param[in] view The Carrottus GPU image view.
 *
 * @return The view's id<MTLTexture>, or nil if @p view is NULL.
 */
id<MTLTexture> crgpu_get_mtl_texture_view(crgpu_image_view view);

/**
 * @brief Returns the id<MTLHeap> behind @p heap.
 *
 * A placement heap (MTLHeapTypePlacement), hazard tracked: see
 * crgpu_heap_create().
 *
 * @param[in] heap The Carrottus GPU heap.
 *
 * @return Its id<MTLHeap>, or nil if @p heap is NULL.
 */
id<MTLHeap> crgpu_get_mtl_heap(crgpu_heap heap);

/**
 * @brief Returns the id<MTLRenderPipelineState> behind @p pipeline.
 *
 * @param[in] pipeline The Carrottus GPU shader pipeline.
 *
 * @return Its id<MTLRenderPipelineState>, or nil if @p pipeline is NULL.
 */
id<MTLRenderPipelineState>
crgpu_get_mtl_render_pipeline_state(crgpu_graphics_pipeline pipeline);

/**
 * @brief Returns the id<MTLSharedEvent> behind @p fence.
 *
 * Neither fence nor semaphore comes back as MTLFence: plain MTLFence only
 * tracks hazards between encoders within a single command buffer/queue and
 * can't be waited on from the CPU or across queues, so it can't stand in for
 * either Vulkan/D3D12 concept - that role belongs to a pipeline barrier, not
 * a fence or semaphore.
 *
 * crgpu_fence needs CPU wait/signal/read (crgpu_fence_wait(),
 * crgpu_fence_signal(), crgpu_fence_get_value()) plus cross-process
 * sharing (CRGPU_FENCE_FLAG_SHARED), so it comes back as
 * id<MTLSharedEvent>.
 *
 * @param[in] fence The Carrottus GPU fence.
 *
 * @return Its id<MTLSharedEvent>, or nil if @p fence is NULL.
 *
 * @see crgpu_get_mtl_event_from_semaphore()
 */
id<MTLSharedEvent> crgpu_get_mtl_shared_event(crgpu_fence fence);
/**
 * @brief Returns the id<MTLEvent> behind @p semaphore.
 *
 * crgpu_semaphore's public API never exposes a CPU-side accessor - only
 * GPU wait/signal across queue submissions - so it comes back as plain
 * id<MTLEvent>, MTLSharedEvent's CPU-visibility-free base class, instead of
 * id<MTLSharedEvent> (see crgpu_get_mtl_shared_event()).
 *
 * @param[in] semaphore The Carrottus GPU semaphore.
 *
 * @return Its id<MTLEvent>, or nil if @p semaphore is NULL.
 */
id<MTLEvent> crgpu_get_mtl_event_from_semaphore(crgpu_semaphore semaphore);

/**
 * @brief Returns the MTLPixelFormat of @p swap_chain's images.
 *
 * The CAMetalLayer's pixelFormat, which crgpu_swap_chain_create() set
 * from the swap chain's CRGPU_FORMAT.
 *
 * @param[in] swap_chain The Carrottus GPU swap chain.
 *
 * @return The pixel format, or MTLPixelFormatInvalid if @p swap_chain is
 *         NULL.
 */
MTLPixelFormat crgpu_get_mtl_swap_chain_format(crgpu_swap_chain swap_chain);

#ifdef __cplusplus
}
#endif

#endif // CRGPU_COMPILE_METAL_API

#endif //CARROTTUS_CRGPU_METAL_NATIVES_H
