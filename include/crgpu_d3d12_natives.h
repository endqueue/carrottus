/**
 * @file crgpu_d3d12_natives.h
 * @brief Escape hatch: the raw Direct3D 12 and DXGI objects behind Carrottus GPU's
 * opaque objects.
 *
 * Use only for interop with third-party Direct3D 12 libraries. Carrottus GPU has
 * no visibility into calls made with these objects. Declared only when
 * CRGPU_COMPILE_D3D12_API is set.
 *
 * Every accessor returns an object Carrottus GPU still owns, without adding a
 * reference: it stays valid for as long as the Carrottus GPU object it came from,
 * and the caller never releases it (or calls AddRef() to keep it longer).
 * Each returns NULL when given a NULL object.
 */

#ifndef CRGPU_D3D12_NATIVES_H
#define CRGPU_D3D12_NATIVES_H

#if CRGPU_COMPILE_D3D12_API

#include <d3d12.h>
#include <dxgi1_6.h>
#include <crgpu.h>

#if __cplusplus
extern "C" {
#endif

/**
 * @brief Returns the DXGI factory behind @p instance.
 *
 * @param[in] instance The Carrottus GPU instance.
 *
 * @return Its IDXGIFactory7, or NULL if @p instance is NULL.
 */
IDXGIFactory7 *crgpu_get_dxgi_factory(crgpu_instance instance);

/**
 * @brief Returns the IDXGIAdapter4 behind @p adapter.
 *
 * An adapter is an IDXGIAdapter4.
 *
 * @param[in] adapter The adapter, as crgpu_adapter_enumerate() gave it.
 *
 * @return Its IDXGIAdapter4, or NULL if @p adapter is NULL.
 */
IDXGIAdapter4 *crgpu_get_dxgi_adapter(crgpu_adapter adapter);
/**
 * @brief Returns the ID3D12Device14 behind @p device.
 *
 * @param[in] device The Carrottus GPU device.
 *
 * @return Its ID3D12Device14, or NULL if @p device is NULL.
 */
ID3D12Device14 *crgpu_get_d3d12_device(crgpu_device device);

/**
 * @brief Returns the ID3D12CommandQueue behind @p queue.
 *
 * The caller synchronizes a queue (see crgpu_command_queue_submit_lists()), and
 * a use of the ID3D12CommandQueue itself is one more use of the same queue
 * under the same rule.
 *
 * @param[in] queue The Carrottus GPU command queue.
 *
 * @return Its ID3D12CommandQueue, or NULL if @p queue is NULL.
 */
ID3D12CommandQueue *
crgpu_get_d3d12_command_queue(crgpu_command_queue queue);

/**
 * @brief Returns the ID3D12CommandAllocator behind @p allocator.
 *
 * @param[in] allocator The Carrottus GPU command allocator.
 *
 * @return Its ID3D12CommandAllocator, or NULL if @p allocator is NULL.
 */
ID3D12CommandAllocator *
crgpu_get_d3d12_command_allocator(crgpu_command_allocator allocator);

/**
 * @brief Returns the ID3D12GraphicsCommandList10 behind @p cmd.
 *
 * @param[in] cmd The Carrottus GPU command list.
 *
 * @return Its ID3D12GraphicsCommandList10, or NULL if @p cmd is NULL.
 */
ID3D12GraphicsCommandList10 *
crgpu_get_d3d12_graphics_command_list(crgpu_command_list cmd);

/**
 * @brief Returns the ID3D12Resource2 behind @p buffer.
 *
 * @param[in] buffer The Carrottus GPU buffer.
 *
 * @return Its ID3D12Resource2, or NULL if @p buffer is NULL.
 */
ID3D12Resource2 *crgpu_get_d3d12_buffer(crgpu_buffer buffer);
/**
 * @brief The Direct3D 12 view a Carrottus GPU buffer view is, as filled in by
 * crgpu_get_d3d12_buffer_view().
 *
 * Direct3D 12 has no buffer-view object, only a description per kind of
 * binding; one member of this union holds it.
 */
/* Frozen: a fact of the D3D12 API. It never changes; a new need gets a new type. */
typedef union CRGPU_D3D12_BUFFER_VIEW {
	/** The view of a vertex buffer. */
	D3D12_VERTEX_BUFFER_VIEW _vb;
	/** The view of an index buffer. */
	D3D12_INDEX_BUFFER_VIEW _ib;
	/** The view of a constant (uniform) buffer. */
	D3D12_CONSTANT_BUFFER_VIEW_DESC _cb;
	/** The view of a stream-output buffer. Carrottus GPU never fills it. */
	D3D12_STREAM_OUTPUT_BUFFER_VIEW _so;
} CRGPU_D3D12_BUFFER_VIEW;
/**
 * @brief Copies the Direct3D 12 view behind @p view into @p out.
 *
 * Which member of the union is filled depends on the parent buffer's usage,
 * tested in this order: `_vb` for CRGPU_BUFFER_USAGE_VERTEX, `_ib` for
 * CRGPU_BUFFER_USAGE_INDEX, `_cb` for CRGPU_BUFFER_USAGE_UNIFORM. A
 * buffer of any other usage has no D3D12 view object, and `*out` is then
 * not meaningful.
 *
 * @param[in]  view The Carrottus GPU buffer view.
 * @param[out] out  Receives the view.
 *
 * @return true if @p out was written; false if @p view or @p out is NULL.
 */
bool crgpu_get_d3d12_buffer_view(
    crgpu_buffer_view view,
    CRGPU_D3D12_BUFFER_VIEW *out);

/**
 * @brief Returns the ID3D12Resource2 behind @p image.
 *
 * @param[in] image The Carrottus GPU image.
 *
 * @return Its ID3D12Resource2, or NULL if @p image is NULL.
 */
ID3D12Resource2 *crgpu_get_d3d12_image(crgpu_image image);
/**
 * @brief The Direct3D 12 view description a Carrottus GPU image view is, as filled
 * in by crgpu_get_d3d12_image_view().
 *
 * Direct3D 12 describes a view of an image per kind of binding; one member
 * of this union holds it.
 */
/* Frozen: a fact of the D3D12 API. It never changes; a new need gets a new type. */
typedef union CRGPU_D3D12_IMAGE_VIEW {
	/** A shader resource (sampled) view. */
	D3D12_SHADER_RESOURCE_VIEW_DESC _srv;
	/** A render target (color attachment) view. */
	D3D12_RENDER_TARGET_VIEW_DESC _rtv;
	/** A depth-stencil attachment view. */
	D3D12_DEPTH_STENCIL_VIEW_DESC _dsv;
	/** An unordered access (storage) view. */
	D3D12_UNORDERED_ACCESS_VIEW_DESC _uav;
} CRGPU_D3D12_IMAGE_VIEW;
/**
 * @brief Copies the Direct3D 12 view description behind @p view into @p out.
 *
 * Which member of the union is filled depends on the image's usage, tested
 * in this order: `_dsv` for CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT,
 * `_rtv` for CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT, `_uav` for
 * CRGPU_IMAGE_USAGE_STORAGE, and `_srv` otherwise. A swap chain image's
 * view fills `_rtv`.
 *
 * @param[in]  view The Carrottus GPU image view.
 * @param[out] out  Receives the view description.
 *
 * @return true if @p out was written; false if @p view or @p out is NULL.
 */
bool crgpu_get_d3d12_image_view(
    crgpu_image_view view,
    CRGPU_D3D12_IMAGE_VIEW *out);

/**
 * @brief Returns the ID3D12Heap behind @p heap.
 *
 * @param[in] heap The Carrottus GPU heap.
 *
 * @return Its ID3D12Heap, or NULL if @p heap is NULL.
 *
 * @see crgpu_heap_create()
 */
ID3D12Heap *crgpu_get_d3d12_heap(crgpu_heap heap);

/**
 * @brief Returns the root signature of @p pipeline.
 *
 * @param[in] pipeline The Carrottus GPU shader pipeline.
 *
 * @return Its ID3D12RootSignature, or NULL if @p pipeline is NULL.
 */
ID3D12RootSignature *crgpu_get_d3d12_root_signature(crgpu_graphics_pipeline pipeline);
/**
 * @brief Returns the pipeline state object of @p pipeline.
 *
 * @param[in] pipeline The Carrottus GPU shader pipeline.
 *
 * @return Its ID3D12PipelineState, or NULL if @p pipeline is NULL.
 */
ID3D12PipelineState *crgpu_get_d3d12_pipeline_state(crgpu_graphics_pipeline pipeline);

/**
 * @brief Returns the ID3D12Fence1 behind @p fence.
 *
 * @param[in] fence The Carrottus GPU fence.
 *
 * @return Its ID3D12Fence1, or NULL if @p fence is NULL.
 */
ID3D12Fence1 *crgpu_get_d3d12_fence(crgpu_fence fence);
/**
 * @brief Returns the ID3D12Fence1 behind @p semaphore.
 *
 * A Carrottus GPU semaphore is a Direct3D 12 fence too.
 *
 * @param[in] semaphore The Carrottus GPU semaphore.
 *
 * @return Its ID3D12Fence1, or NULL if @p semaphore is NULL.
 */
ID3D12Fence1 *crgpu_get_d3d12_fence_from_semaphore(crgpu_semaphore semaphore);

#if __cplusplus
}
#endif

#endif // CRGPU_COMPILE_D3D12_API

#endif // CRGPU_D3D12_NATIVES_H
