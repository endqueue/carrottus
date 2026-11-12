//
// Carrottus GPU Metal backend - internal opaque type definitions.
//

#if CRGPU_COMPILE_METAL_API

#ifndef CARROTTUS_CRGPUMETAL_H
#define CARROTTUS_CRGPUMETAL_H

#include "crgpu.h"
#include "../../../crallocator.h"
#include "../../crgpushared.h"
#include <Metal/Metal.h>
#include <stdatomic.h>
#include <QuartzCore/QuartzCore.h>

// Metal's setViewports:count:/setScissorRects:count: always set from slot 0
// with no first_viewport-style offset (unlike Vulkan's vkCmdSetViewport),
// so crgpu_command_list_set_viewports/set_scissor_rects patch a persistent
// per-command-list array up to this many slots and resend the whole thing -
// see crgpu_command_list_metal below. 16 matches Metal's real multi-
// viewport/vertex-amplification hardware limit.
#define CRGPU_METAL_MAX_VIEWPORTS 16

// Metal has no persistent push-constant-block state the way Vulkan/D3D12 do -
// -setVertexBytes:/-setFragmentBytes: replaces the entire bound region at an
// index each call, so a caller updating one sub-range at a time needs the
// full block re-sent every time. This is the scratch buffer size backing
// that; comfortably above Vulkan's guaranteed minimum maxPushConstantsSize
// (128 bytes).
#define CRGPU_METAL_PUSH_CONSTANTS_SIZE 256

// The Metal Shading Language side of whatever cross-compiles this project's
// shaders must bind its push-constant struct at this fixed buffer/argument
// index (e.g. `[[buffer(30)]]`) for crgpu_command_list_push_graphics_constants below to
// reach it - Metal has no other channel for push-constant-style data.
#define CRGPU_METAL_PUSH_CONSTANTS_BUFFER_INDEX 30

// Metal shares one buffer-argument-index namespace across vertex-attribute
// buffers and everything else bound via -setVertexBuffer:/-setFragmentBuffer:
// - unlike Vulkan/D3D12, which keep vertex bindings and descriptor sets in
// separate namespaces. Descriptor sets are pinned to the CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS
// indices directly below the push-constant buffer: set N's whole argument
// buffer at buffer(CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE + N), as
// the binding layout crgpu.h states on crgpu_shader_module_desc, which
// a caller's library follows; indices below are left for vertex buffers.
#define CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE \
	(CRGPU_METAL_PUSH_CONSTANTS_BUFFER_INDEX - CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)

// The bit of a pipeline's per-stage masks (crgpu_shader_pipeline_metal::
// _non_fragment_set_mask, _fragment_set_mask) that says the stage's
// function reads the push constants, above the sets' bits.
#define CRGPU_METAL_PUSH_CONSTANTS_MASK_BIT (1u << CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)

// Where the sampler half of a CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
// sits in its set's argument buffer: a combined binding B of N elements has
// its textures at [[id(B)]] to [[id(B + N - 1)]], as every binding has its
// elements at its own number, and its samplers at [[id(BASE + B)]] on. A
// combined descriptor is two members of a Metal argument buffer, and they
// can't share one id short of Metal 3's aliasing; so the sampler goes where
// no binding is, by a remap of each combined sampler at cross-compilation
// (crgpumetalshader.m), a second argument in the layout's encoder and a
// second write (crgpumetaldescriptors.m). A layout with a combined
// binding refuses any binding at or past BASE.
#define CRGPU_METAL_COMBINED_SAMPLER_INDEX_BASE 1024u

typedef struct crgpu_instance_t {
	cr_object_head head;
	// Metal has no top-level instance/factory object (unlike VkInstance or
	// IDXGIFactory): the GPUs come straight from MTLCopyAllDevices(). This
	// holds the adapters listed when it was made, which never change, and
	// frees them with itself.
	crgpu_adapter *_adapters;
	uint32_t _adapter_count;
#if _DEBUG
	// Its destroyed devices' and surfaces' blocks, freed with it
	// (crstale.h, card X27).
	cr_quarantine _quarantine;
#endif
} crgpu_instance_metal;

typedef struct crgpu_adapter_t {
	cr_object_head head;
	// Retained for the adapter's life. Metal has one MTLDevice per GPU, which
	// a device created on the adapter shares.
	id<MTLDevice> _device_mtl;
	// Read when the instance was made and never changed.
	crgpu___adapter_facts _facts;
	// Whether a device exists on the adapter: one at a time, claimed with an
	// exchange by crgpu_device_create and given back by
	// crgpu_device_destroy. The adapter is shared, hidden state, so this is
	// Carrottus GPU's to guard.
	atomic_bool _in_use;
#if _DEBUG
	// The instance that listed it, whose quarantine takes its destroyed
	// devices (crstale.h, card X27).
	struct crgpu_instance_t *_debug_instance;
#endif
} crgpu_adapter_metal;

typedef struct crgpu_device_t {
	cr_object_head head;
	// The adapter's, retained again for the device's life.
	id<MTLDevice> _device_mtl;
	crgpu_adapter_metal *_adapter;
	// crgpu_device_desc::required_features: what every call checks.
	CRGPU_FEATURES _features;
	// The queues the desc asked for, by kind, each array as long as its
	// count (NULL for 0). Metal queues are not partitioned into families
	// (see the natives-header asymmetries in ../../../CLAUDE.md): each is a
	// plain MTLCommandQueue, and the kinds only keep
	// crgpu_command_queue_get's (type, index) shape the same on every
	// backend.
	crgpu_command_queue *_cmd_queues_direct;
	uint32_t _cmd_queue_count_direct;
	crgpu_command_queue *_cmd_queues_compute;
	uint32_t _cmd_queue_count_compute;
	crgpu_command_queue *_cmd_queues_copy;
	uint32_t _cmd_queue_count_copy;
	// crgpu_device_desc::max_descriptors and ::max_samplers: what the
	// device's descriptor pools take out of.
	crgpu___descriptor_budget _budget;
#if _DEBUG
	// Objects made from the device and not yet destroyed, by kind;
	// crgpu_device_destroy reports and asserts on any left.
	crgpu___live_objects _live;
	// The blocks of the objects made from the device and destroyed, freed
	// with it (crstale.h, card X27).
	cr_quarantine _quarantine;
#endif
} crgpu_device_metal;

typedef struct crgpu_buffer_t {
	cr_object_head head;
	id<MTLBuffer> _buffer_mtl;
	crgpu_device_metal * _parent_device;
	crgpu_buffer_desc _desc;
	// NULL for a buffer made on the device; the heap it was placed in
	// otherwise (crgpu_buffer_create_placed).
	crgpu_heap _heap;
} crgpu_buffer_metal;

typedef struct crgpu_buffer_view_t {
	cr_object_head head;
	// The parent buffer lives in _desc.parent_buffer (set from the
	// authoritative `buffer` parameter at creation, not necessarily whatever
	// the caller's desc.parent_buffer was - see crgpu_buffer_view_create)
	// - no separate redundant pointer field here.
	crgpu_buffer_view_desc _desc;
	// The buffer's device, kept here because a view may be destroyed after
	// its buffer (its live count is the device's).
	crgpu_device_metal *_parent_device;
} crgpu_buffer_view_metal;

typedef struct crgpu_image_t {
	cr_object_head head;
	id<MTLTexture> _texture_mtl;
	crgpu_device_metal *_parent_device;
	crgpu_image_desc _desc;
	// NULL for an image made on the device; the heap it was placed in
	// otherwise (crgpu_image_create_placed).
	crgpu_heap _heap;
} crgpu_image_metal;

typedef struct crgpu_heap_t {
	cr_object_head head;
#if _DEBUG
	// Buffers and images placed in the heap and not yet destroyed.
	// crgpu_heap_destroy asserts it is 0. Atomic, because resources in
	// one heap may be placed and destroyed on several threads at once.
	_Atomic uint64_t _placed_count;
#endif
	// MTLHeapTypePlacement, hazard tracked (see crgpumetalheap.m).
	id<MTLHeap> _heap_mtl;
	// The resource options every buffer placed in the heap is made with:
	// the heap's storage and CPU cache modes, and its hazard tracking.
	MTLResourceOptions _resource_options_mtl;
	crgpu_device_metal *_parent_device;
	crgpu_heap_desc _desc;
} crgpu_heap_metal;

// Shared by crgpu_buffer_create / crgpu_image_create and the heap calls
// (crgpumetalheap.m), so a buffer or image is checked and described the
// same way whether it is made on the device or placed in a heap.

// crgpu_buffer_create's desc checks, in its order, after its argument
// checks: size, usage, memory kind, usage against kind, device.
CR_RESULT crgpumetal___check_buffer_desc(
    CRGPU_FEATURES features,
    const crgpu_buffer_desc *desc);
// The storage and CPU cache modes a buffer of [memory_kind] is made with.
MTLResourceOptions crgpumetal___buffer_resource_options(CRGPU_MEMORY_KIND memory_kind);
// crgpu_image_create's desc checks, in its order, after its argument
// checks.
CR_RESULT crgpumetal___check_image_desc(const crgpu_image_desc *desc);
// A texture descriptor for an image of [desc], which the caller releases.
// [desc] has passed crgpumetal___check_image_desc.
MTLTextureDescriptor *crgpumetal___new_texture_descriptor(const crgpu_image_desc *desc);

typedef struct crgpu_image_view_t {
	cr_object_head head;
	// Metal represents a texture view as another id<MTLTexture> (see
	// crgpu_get_mtl_texture_view in crgpu_metal_natives.h) rather than a
	// distinct view object - this is that view texture, not the parent's.
	id<MTLTexture> _texture_view_mtl;
	crgpu_image_metal *_parent_image;
	// The image's device, kept here because a view may be destroyed after
	// its image (its live count is the device's).
	crgpu_device_metal *_parent_device;
	crgpu_image_view_desc _desc;
} crgpu_image_view_metal;

typedef struct crgpu_command_queue_t {
	cr_object_head head;
	id<MTLCommandQueue> _command_queue_mtl;
	crgpu_device_metal *_parent_device;
#if _DEBUG
	// How many threads are in a call that uses the queue: a debug build
	// asserts it is never more than one (crgpumetal___queue_enter).
	atomic_int _debug_users;
#endif
} crgpu_command_queue_metal;

// Brackets every Carrottus GPU call that uses a queue (a submit, a present, a
// wait-idle). Metal itself would allow two threads at once, but crgpu.h
// makes a queue the caller's to synchronize on every backend, since Vulkan
// requires it; a debug build asserts here so that code that breaks the rule
// is caught on Metal too. Nothing in a release build.
#if _DEBUG
static inline void crgpumetal___queue_enter(crgpu_command_queue_metal *queue) {
	const int others = atomic_fetch_add(&queue->_debug_users, 1);
	assert(others == 0 && "crgpu: two threads used one command queue at once; the caller synchronizes a queue");
	(void)others;
}
static inline void crgpumetal___queue_leave(crgpu_command_queue_metal *queue) { atomic_fetch_sub(&queue->_debug_users, 1); }
#else
static inline void crgpumetal___queue_enter(crgpu_command_queue_metal *queue) { (void)queue; }
static inline void crgpumetal___queue_leave(crgpu_command_queue_metal *queue) { (void)queue; }
#endif

typedef struct crgpu_command_allocator_t {
	cr_object_head head;
	// No native Metal handle: command buffers are requested fresh from the
	// queue each time (see crgpu_metal_natives.h) rather than reset from a
	// persistent allocator/pool object the way Vulkan/D3D12 do. _parent_queue
	// is that queue - resolved once from _desc.type at creation time (a
	// borrowed reference into the device's cached queue families, not owned
	// here) - and is what every crgpu_command_list_create/
	// crgpu_command_list_begin call for lists made from this allocator
	// pulls a fresh id<MTLCommandBuffer> from.
	crgpu_device_metal *_parent_device;
	crgpu_command_queue_metal *_parent_queue;
	crgpu_command_allocator_desc _desc;
#if _DEBUG
	// The list of this allocator that is open for recording, or NULL. One
	// at a time on every backend (crgpu_command_list_begin): Metal has no
	// allocator to contend for, D3D12 does, and a debug build asserts so the
	// rule is caught here too. Not atomic: an allocator is used from one
	// thread at a time.
	crgpu_command_list _open_list;
#endif
} crgpu_command_allocator_metal;

typedef struct crgpu_command_list_t {
	cr_object_head head;
	id<MTLCommandBuffer> _command_buffer_mtl;
	crgpu_command_allocator_metal *_parent_allocator;

	// Set by crgpu_command_list_begin_rendering, cleared by crgpu_command_list_end_rendering
	// (crgpumetalrenderpass.m, not yet implemented) - all of
	// crgpu_command_list_set_viewports/set_scissor_rects/set_vertex_buffers/
	// set_index_buffer/draw*/bind_pipeline/push_constants below encode
	// against this. Metal has no command-buffer-level draw/state API the
	// way Vulkan/D3D12 do - everything is a method on the render encoder
	// itself, which only exists between begin/end_rendering.
	id<MTLRenderCommandEncoder> _active_render_encoder;
	// The pass descriptor crgpu_command_list_begin_rendering fills, made with the
	// list and kept (retained) so a pass makes no descriptor or attachment
	// descriptor of its own: every field a pass may set is set or cleared
	// at each begin, and the textures cleared once the encoder is made.
	MTLRenderPassDescriptor *_pass_desc_mtl;
	// The descriptors crgpu_command_list_build_acceleration_structures fills for
	// each build's sizes and again for its recording (Metal reads one when
	// the build is encoded), made with the list on a device with
	// CRGPU_FEATURE_RAY_QUERY and nil otherwise, so a build makes no
	// descriptor: one for a top level, and one for a bottom level, whose
	// geometry array (_rq_geometries_mtl, retained by it) is made of the
	// pools' descriptors in a build's order. A pool of each geometry kind
	// only grows, when a build has more of that kind than any before; the
	// checks of a build call grow them for all of its builds, so its
	// recording makes nothing.
	MTLInstanceAccelerationStructureDescriptor *_rq_top_desc_mtl;
	MTLPrimitiveAccelerationStructureDescriptor *_rq_bottom_desc_mtl;
	NSMutableArray<MTLAccelerationStructureGeometryDescriptor *> *_rq_geometries_mtl;
	NSMutableArray<MTLAccelerationStructureTriangleGeometryDescriptor *> *_rq_triangles_mtl;
	NSMutableArray<MTLAccelerationStructureBoundingBoxGeometryDescriptor *> *_rq_boxes_mtl;
	// What crgpu_command_list_bind_graphics_pipeline last set on _active_render_encoder,
	// cleared when a render encoder is made. Not retained. Metal's
	// validation refuses setting a state the encoder already has, and two
	// pipelines can share one depth-stencil state, since Metal returns one
	// object for equal descriptors.
	id<MTLRenderPipelineState> _encoder_pipeline_state_mtl;
	id<MTLDepthStencilState> _encoder_depth_stencil_state_mtl;
	// Likewise the rasterizer state; meaningful only while
	// _encoder_rasterizer_set is true, which a new encoder clears.
	bool _encoder_rasterizer_set;
	MTLCullMode _encoder_cull_mode;
	MTLWinding _encoder_winding;
	MTLTriangleFillMode _encoder_fill_mode;
	float _encoder_depth_bias[3];
	// The depth bounds _active_render_encoder tests against, {0, 1} (no
	// test) for a new encoder. Metal has no pipeline switch for the test,
	// so crgpu_command_list_bind_graphics_pipeline sets {0, 1} for a pipeline created
	// without depth_bounds_test_enable, and crgpu_command_list_set_depth_bounds
	// sets the caller's range only while one created with it is bound.
	float _encoder_depth_bounds[2];
	// The buffer and offset _active_render_encoder has at each vertex
	// buffer slot, nil for none: Metal's validation refuses setting the same
	// buffer again, so crgpu_command_list_set_vertex_buffers sets a slot only when
	// it changes, and only the offset when the buffer is the same. Cleared
	// with the encoder.
	id<MTLBuffer> _encoder_vertex_buffers[CRGPU_MAX_VERTEX_BINDINGS];
	NSUInteger _encoder_vertex_offsets[CRGPU_MAX_VERTEX_BINDINGS];
	// The argument buffer _active_render_encoder has at each descriptor
	// set's index, per stage (vertex, mesh, fragment), nil for none, as
	// _encoder_compute_sets is for compute. Cleared with the encoder.
	id<MTLBuffer> _encoder_vertex_sets[CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	id<MTLBuffer> _encoder_mesh_sets[CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	id<MTLBuffer> _encoder_fragment_sets[CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS];

	// Persistent viewport/scissor state - see CRGPU_METAL_MAX_VIEWPORTS.
	CRGPU_VIEWPORT _viewports[CRGPU_METAL_MAX_VIEWPORTS];
	uint32_t _viewport_count; // High-water mark of slots ever set, not a "count set this call".
	CRGPU_SCISSOR_RECT _scissor_rects[CRGPU_METAL_MAX_VIEWPORTS];
	uint32_t _scissor_rect_count;
	// The viewports and scissor rectangles _active_render_encoder has, the
	// first _encoder_viewport_count / _encoder_scissor_rect_count of each, 0
	// for none: Metal's validation refuses setting the same ones again, so
	// they are set only when they differ. Cleared with the encoder.
	MTLViewport _encoder_viewports[CRGPU_METAL_MAX_VIEWPORTS];
	uint32_t _encoder_viewport_count;
	MTLScissorRect _encoder_scissor_rects[CRGPU_METAL_MAX_VIEWPORTS];
	uint32_t _encoder_scissor_rect_count;

	// Borrowed (not owned) - the currently bound pipeline/index buffer view,
	// needed because Metal's draw calls take primitive type and index
	// buffer/type as direct arguments rather than consuming previously-set
	// encoder state the way Vulkan/D3D12 do. crgpu_shader_pipeline_metal
	// isn't defined yet at this point in the file (it's declared below) -
	// the struct tag is forward-referenced here, which is fine for a
	// pointer to an as-yet-incomplete type.
	struct crgpu_graphics_pipeline_t *_bound_pipeline;
	crgpu_buffer_view_metal *_bound_index_buffer_view;

	// Compute counterpart to _active_render_encoder above. Metal disallows
	// two live encoders of different kinds on one command buffer at once, so
	// crgpu_command_list_bind_compute_pipeline/crgpu_command_list_dispatch (crgpumetalcommand.m)
	// end _active_render_encoder before opening this, and crgpu_command_list_begin_rendering
	// (crgpumetalrenderpass.m) ends this before opening a render encoder -
	// see crgpumetal___internal_end_active_compute_encoder. Set by the
	// first of those two calls per compute pass, cleared by
	// crgpumetal___internal_end_active_compute_encoder (called from
	// crgpu_command_list_begin_rendering and crgpu_command_list_end).
	id<MTLComputeCommandEncoder> _active_compute_encoder;
	struct crgpu_compute_pipeline_t *_bound_compute_pipeline;
	// The pipeline state _active_compute_encoder has set, nil for a new
	// encoder: Metal's validation refuses setting the same state again, so
	// crgpu_command_list_bind_compute_pipeline sets it only when it differs.
	// Cleared with the encoder.
	id<MTLComputePipelineState> _encoder_compute_pipeline_state;
	// The argument buffer _active_compute_encoder has at each descriptor
	// set's index, nil for none: binding a set already bound there is
	// allowed and leaves the encoder as it is, since Metal's validation
	// refuses setting the same buffer again. Cleared with the encoder.
	id<MTLBuffer> _encoder_compute_sets[CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS];

	// Blit counterpart to _active_render_encoder/_active_compute_encoder
	// above, backing crgpu_command_list_copy_buffer/_copy_buffer_to_image/
	// _copy_image_to_buffer (crgpumetalrenderpass.m) via an
	// MTLBlitCommandEncoder. Same "Metal disallows two live encoders of
	// different kinds on one command buffer at once" rule applies - opening
	// this ends any active render/compute encoder first, and
	// crgpu_command_list_begin_rendering/crgpumetal___internal_ensure_compute_encoder
	// end this before opening theirs. Cleared by
	// crgpumetal___internal_end_active_blit_encoder (called from
	// crgpu_command_list_end and from those two encoder-open paths).
	id<MTLBlitCommandEncoder> _active_blit_encoder;

	// Push-constant scratch block - see CRGPU_METAL_PUSH_CONSTANTS_SIZE.
	uint8_t _push_constants_scratch[CRGPU_METAL_PUSH_CONSTANTS_SIZE];
	uint32_t _push_constants_extent; // High-water mark of bytes ever written.
	// The CRGPU_SHADER_STAGE bits of the ranges pushed for since the last
	// draw, which that draw sends the block to
	// (crgpumetal___internal_send_push_constants).
	uint32_t _push_constants_pending_stages;

	// The same for crgpu_command_list_push_compute_constants, kept apart so a
	// compute push never rewrites what the render encoder was last sent.
	uint8_t _compute_push_constants_scratch[CRGPU_METAL_PUSH_CONSTANTS_SIZE];
	uint32_t _compute_push_constants_extent;
} crgpu_command_list_metal;

typedef struct crgpu_timestamp_set_t {
	cr_object_head head;
	// Of the device's timestamp counter set, in shared storage so the CPU
	// can resolve it. A pass samples it at its stage boundaries
	// (crgpumetalrenderpass.m).
	id<MTLCounterSampleBuffer> _sample_buffer_mtl;
	id<MTLDevice> _device_mtl;
	crgpu_device_metal *_parent_device;
	uint32_t _count;
	// sampleTimestamps:gpuTimestamp: at creation: the CPU and GPU clocks at
	// one moment, which crgpu_timestamp_set_get sets against a second pair.
	MTLTimestamp _cpu_origin;
	MTLTimestamp _gpu_origin;
} crgpu_timestamp_set_metal;

// What one CRGPU_FORMAT is to Metal: an entry of the format table in
// crgpumetalformat.m, which every function of this backend that
// translates a format or asks about one reads, so an image, a view, a
// pipeline's attachments and a swap chain of one format always agree.
typedef struct crgpumetal_format_info {
	// The texture and attachment format. Invalid: no image, view or
	// attachment of this format can be made.
	MTLPixelFormat pixel;
	// The format of a view of the stencil aspect alone. Invalid where the
	// format has no stencil.
	MTLPixelFormat stencil_view;
	// The vertex attribute format. Invalid: not an attribute format.
	MTLVertexFormat vertex;
	// The aspects the format has (CRGPU_IMAGE_ASPECT): what views of it
	// crgpu_image_view_create takes.
	CRGPU_IMAGE_ASPECT aspects;
	// A CAMetalLayer can be set to #pixel: a swap chain of it can be made.
	bool presentable;
} crgpumetal_format_info;

// Defined in crgpumetalformat.m. The entry for [format]; for a format
// with none (and any value out of range), one with every format Invalid,
// the color aspect, and not presentable. Never NULL.
const crgpumetal_format_info *crgpumetal___format_info(CRGPU_FORMAT format);

// Defined in crgpumetalcommand.m - ends and releases cmd_list_metal's
// _active_compute_encoder if one is active, else a no-op. Shared with
// crgpu_command_list_begin_rendering (crgpumetalrenderpass.m), which must end any
// active compute encoder before opening a render encoder (Metal disallows
// two live encoders of different kinds on one command buffer at once) - see
// _active_compute_encoder's comment above.
void crgpumetal___internal_end_active_compute_encoder(crgpu_command_list_metal *cmd_list_metal);

// Defined in crgpumetalrenderpass.m - ends and releases cmd_list_metal's
// _active_blit_encoder if one is active, else a no-op. Shared with
// crgpu_command_list_begin_rendering (crgpumetalrenderpass.m) and
// crgpumetal___internal_ensure_compute_encoder (crgpumetalcommand.m),
// both of which must end any active blit encoder before opening their own -
// see _active_blit_encoder's comment above.
void crgpumetal___internal_end_active_blit_encoder(crgpu_command_list_metal *cmd_list_metal);

// The largest draw_count one indirect draw on [device] takes:
// CRGPU_ADAPTER_PROPERTIES::max_draw_indirect_count with
// CRGPU_FEATURE_MULTI_DRAW_INDIRECT, else 1.
static inline uint32_t crgpumetal___max_draw_indirect_count(const crgpu_device_metal *device) {
	return (device->_features & CRGPU_FEATURE_MULTI_DRAW_INDIRECT) ? device->_adapter->_facts.properties.max_draw_indirect_count : 1;
}

// The device's timestamp counter set (MTLCommonCounterSetTimestamp),
// retained, which the caller releases; or nil if it has none. Defined in
// crgpumetalquery.m.
id<MTLCounterSet> crgpumetal___internal_timestamp_counter_set(id<MTLDevice> device);

typedef struct crgpu_shader_module_t {
	cr_object_head head;
	crgpu_device_metal *_parent_device;
	crgpu_shader_module_desc _desc;
	// The caller's .metallib, as a library, and its entry point's function,
	// looked up by the desc's name when the module is made.
	id<MTLLibrary> _library_mtl;
	id<MTLFunction> _function_mtl;
	// CRGPU_SHADER_STAGE_COMPUTE, _MESH and _TASK only: the desc's
	// workgroup size. Metal's -dispatchThreadgroups:threadsPerThreadgroup:
	// and -drawMeshThreadgroups:... take it at every call and no library
	// records it; crgpu_compute_pipeline_create and the mesh pipeline copy
	// it onto the pipeline, so a dispatch never needs the module. 0 for the
	// other stages.
	uint32_t _workgroup_size_x, _workgroup_size_y, _workgroup_size_z;
} crgpu_shader_module_metal;

typedef struct crgpu_graphics_pipeline_t {
	cr_object_head head;
	id<MTLRenderPipelineState> _render_pipeline_state_mtl;
	// Vulkan/D3D12 bake depth/stencil test config into the same monolithic
	// pipeline object as everything else; Metal keeps it as a wholly
	// separate native object, bound alongside (not instead of) the render
	// pipeline state - see crgpu_command_list_bind_graphics_pipeline in crgpumetalcommand.m.
	id<MTLDepthStencilState> _depth_stencil_state_mtl;
	crgpu_device_metal *_parent_device;
	crgpu_graphics_pipeline_desc _desc;
	// True when this pipeline was built from a MTLMeshRenderPipelineDescriptor
	// (desc.mesh_module set) rather than a classic MTLRenderPipelineDescriptor
	// - _render_pipeline_state_mtl/_depth_stencil_state_mtl are reused as-is
	// either way (newRenderPipelineStateWithMeshDescriptor:... returns the
	// same id<MTLRenderPipelineState> type), but binding buffers/bytes to a
	// mesh pipeline's render encoder goes through -setMeshBuffer:/
	// -setMeshBytes: instead of -setVertexBuffer:/-setVertexBytes: (see
	// crgpu_command_list_bind_graphics_descriptor_sets/crgpu_command_list_push_graphics_constants), so
	// callers need to know which shape is bound.
	bool _is_mesh_pipeline;
	// Copied from mesh_module at pipeline-creation time when _is_mesh_pipeline
	// - see crgpu_shader_module_metal::_workgroup_size_x/y/z above.
	uint32_t _mesh_local_size_x, _mesh_local_size_y, _mesh_local_size_z;
	// The descriptor sets each stage's function reads, bit n for set n, from
	// Metal's reflection of this pipeline: the vertex (or mesh and object)
	// stage's and the fragment stage's. crgpu_command_list_bind_graphics_descriptor_sets
	// binds a set only to the stages that read it - a set's layout may name
	// a stage whose function in this pipeline takes no argument for it (a
	// layout shared by pipelines that read it from different stages), and
	// Metal's validation fails a binding no argument takes. All bits set
	// when the reflection is missing, which leaves the layout's stages to
	// decide alone. CRGPU_METAL_PUSH_CONSTANTS_MASK_BIT in each says the
	// stage's function reads the push constants, which a draw sends only to
	// the stages that do: Metal's validation fails a buffer no function
	// argument takes, and a module declares only what it reads.
	uint32_t _non_fragment_set_mask;
	uint32_t _fragment_set_mask;
} crgpu_shader_pipeline_metal;

typedef struct crgpu_compute_pipeline_t {
	cr_object_head head;
	id<MTLComputePipelineState> _compute_pipeline_state_mtl;
	crgpu_device_metal *_parent_device;
	crgpu_compute_pipeline_desc _desc;
	// Copied from the compute module at creation time - see
	// crgpu_shader_module_metal::_workgroup_size_x/y/z above.
	uint32_t _local_size_x, _local_size_y, _local_size_z;
} crgpu_compute_pipeline_metal;

typedef struct crgpu_fence_t {
	cr_object_head head;
	// Fences map to id<MTLSharedEvent>, not MTLFence - see
	// crgpu_metal_natives.h for why (MTLFence can't be CPU-waited).
	id<MTLSharedEvent> _shared_event_mtl;
	crgpu_device_metal *_parent_device;
	CRGPU_FENCE_FLAGS _flags;
	// Used only by a swap chain's own in-flight fence, which counts its
	// submissions itself (crgpu_command_queue_submit with a swap chain). A fence
	// the caller created has no counter here: the value a submission signals
	// is the caller's (crgpu_submit_desc::signal_fence_value), and the
	// value reached is the shared event's signaledValue.
	uint64_t _signal_value;
} crgpu_fence_metal;

typedef struct crgpu_semaphore_t {
	cr_object_head head;
	// Semaphores map to plain id<MTLEvent>, not id<MTLSharedEvent> - unlike
	// crgpu_fence, crgpu_semaphore's public API never exposes a CPU-side
	// wait/signal/read, only GPU wait/signal across queue submissions, which
	// is all MTLEvent (MTLSharedEvent's CPU-visibility-free base class) does.
	id<MTLEvent> _event_mtl;
	crgpu_device_metal *_parent_device;
	uint64_t _signal_value;
} crgpu_semaphore_metal;

typedef struct crgpu_sampler_t {
	cr_object_head head;
	id<MTLSamplerState> _sampler_state_mtl;
	crgpu_device_metal *_parent_device;
} crgpu_sampler_metal;

// A run of a set's argument buffer: the [[id]]s first_id to first_id +
// count - 1, each a member of Metal's kind data_type (an MTLDataType: a
// texture, a sampler, a pointer for a buffer, an acceleration structure).
// A set layout keeps its own (_ranges) and a pipeline reads a module's from
// Metal's reflection; the two are compared to refuse a module whose
// argument buffer isn't its layout's (crgpu_shader_module_desc, "Every
// binding of a set").
typedef struct crgpumetal_set_range {
	uint32_t first_id;
	uint32_t count;
	uint32_t data_type;
} crgpumetal_set_range;

// The most runs a set holds: each binding one, a combined image sampler's
// samplers another.
#define CRGPU_METAL_MAX_SET_RANGES (2 * CRGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET)

// Sorts [ranges] by first id and joins each run to the one after it where
// that one starts at its end with the same kind: one array member of n and
// n members of one are the same argument buffer. Returns the count left.
uint32_t crgpumetal___set_ranges_normalize(crgpumetal_set_range *ranges, uint32_t count);

typedef struct crgpu_descriptor_set_layout_t {
	cr_object_head head;
	crgpu_device_metal *_parent_device;
	crgpu_descriptor_set_layout_desc _desc;
	// The argument buffer _argument_encoder_mtl writes, as runs of ids,
	// normalized (crgpumetal___set_ranges_normalize).
	crgpumetal_set_range _ranges[CRGPU_METAL_MAX_SET_RANGES];
	uint32_t _range_count;
	// Metal has no VkDescriptorSetLayout-equivalent schema object independent
	// of a concrete backing buffer - the schema lives entirely in this
	// MTLArgumentEncoder, built once here from the binding list and reused
	// (redirected via -setArgumentBuffer:offset:) to encode every descriptor
	// set allocated from this layout, per Apple's documented pattern for
	// writing into many argument buffer instances from one shared encoder.
	id<MTLArgumentEncoder> _argument_encoder_mtl;
} crgpu_descriptor_set_layout_metal;

typedef struct crgpu_descriptor_pool_t {
	cr_object_head head;
	crgpu_device_metal *_parent_device;
	crgpu_descriptor_pool_desc _desc;
	// Metal needs no real backing arena the way Vulkan's VkDescriptorPool
	// does - each descriptor set below owns its own small MTLBuffer sized to
	// its layout's encodedLength. This only enforces the caller's declared
	// _desc.max_sets cap (see crgpu_descriptor_set_create), matching
	// vkAllocateDescriptorSets' out-of-pool-memory failure without an actual
	// pooled allocator behind it.
	uint32_t _allocated_set_count;
	// What the pool took out of its device's descriptor budget, given back
	// when it is destroyed.
	uint64_t _budget_descriptors;
	uint64_t _budget_samplers;
	// Owned array of every live set allocated from this pool (length
	// _desc.max_sets) - purely so crgpu_descriptor_pool_reset/
	// crgpu_descriptor_pool_destroy can free each set's MTLBuffer in bulk,
	// matching those calls' "free all sets allocated from this pool"
	// contract instead of leaking GPU memory every reset.
	struct crgpu_descriptor_set_t **_tracked_sets_mtl;
} crgpu_descriptor_pool_metal;

// Upper bound on distinct underlying resources (buffers/textures) tracked
// per descriptor set for residency (see crgpu_command_list_bind_graphics_descriptor_sets in
// crgpumetalcommand.m) - anything reached only indirectly through an
// argument buffer must be marked resident with -useResource:usage: on every
// encoder that uses it, unlike a directly-bound resource. Sized well above
// CRGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET to leave room for small resource
// arrays within a single binding.
#define CRGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET 32

typedef struct crgpu_descriptor_set_t {
	cr_object_head head;
	crgpu_descriptor_pool_metal *_parent_pool;
	crgpu_descriptor_set_layout_metal *_layout;
	// The argument buffer itself, written into by crgpu_device_update_descriptor_sets
	// via _layout's shared MTLArgumentEncoder.
	id<MTLBuffer> _argument_buffer_mtl;
	// See CRGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET above. One entry per
	// argument index that has been written: writing an index again replaces
	// its entry, so a set never marks resident what it no longer references.
	// Not retained - as with the argument buffer's own reference, a resource
	// must outlive every bind of a set that still references it.
	uint32_t _tracked_argument_indices[CRGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET];
	id<MTLResource> _tracked_resources_mtl[CRGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET];
	MTLResourceUsage _tracked_resource_usages_mtl[CRGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET];
	// The top level acceleration structure at each tracked entry, NULL for
	// any other resource: a bind also makes resident every bottom level its
	// last build named (crgpu.h, R5 of the ray query draft), which nothing
	// in the instance bytes could tell the backend.
	struct crgpu_acceleration_structure_t *_tracked_structures[CRGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET];
	uint32_t _tracked_resource_count;
} crgpu_descriptor_set_metal;

// An acceleration structure (crgpumetalraytracing.m): an
// MTLAccelerationStructure from the device or a heap. Its first two members
// are crgpu___acceleration_structure_common's (crgpushared.h), which
// the shared checks read.
typedef struct crgpu_acceleration_structure_t {
	cr_object_head head;
	crgpu___acceleration_structure_state _state;
	id<MTLAccelerationStructure> _structure_mtl;
	// A top level's bottom levels as Metal objects, beside _state's list and
	// in its order: _state.max_bottom_levels entries made with the structure,
	// _state.bottom_level_count of them filled where a build or copy into it
	// is recorded, so making them resident is one useResources:count: call
	// on any encoder. NULL for a bottom level or a capacity of 0.
	id<MTLResource> *_bottom_levels_mtl;
	// gpuResourceID: what an instance names a bottom level by.
	uint64_t _resource_id;
	crgpu_device_metal *_parent_device;
	// NULL for one made on the device; its heap otherwise.
	crgpu_heap _heap;
} crgpu_acceleration_structure_metal;

// Defined in crgpumetalraytracing.m: marks resident on [encoder] every
// bottom level [top]'s last build named, for [stages] of a render encoder
// (a compute encoder when [render] is NO).
void crgpumetal___use_bottom_levels(id<MTLCommandEncoder> encoder, bool render, MTLRenderStages stages,
                                      const struct crgpu_acceleration_structure_t *top);

typedef struct crgpu_surface_t {
	cr_object_head head;
	crgpu_instance_metal *_parent_instance;
	void *_window_handle;
	void *_display_handle;
	// Attached to _window_handle's view by crgpu_surface_create. NULL until
	// that's implemented - crgpu_swap_chain_create checks for this and
	// fails rather than assuming it exists.
	CAMetalLayer *_metal_layer;
} crgpu_surface_metal;

typedef struct crgpu_swap_chain_t {
	cr_object_head head;
	crgpu_device_metal *_parent_device;
	crgpu_swap_chain_desc _desc;
	// Borrowed from _desc.surface->_metal_layer - not owned here.
	CAMetalLayer *_metal_layer;
	// From _desc.queue - see crgpu_swap_chain_desc::queue. Borrowed, not
	// owned (the device's cached queue families outlive this swap chain).
	crgpu_command_queue_metal *_present_queue;
	// The drawable currently held between acquire and present. Metal command
	// buffers/drawables are single-use, unlike Vulkan/D3D12's stable N-image
	// array - there is exactly one of these at a time, not one per
	// _desc.buffer_count slot (buffer_count is forced to 1 on this backend;
	// see crgpu_swap_chain_create).
	id<CAMetalDrawable> _current_drawable;
	// Single-element arrays (buffer_count == 1 on Metal) so
	// crgpu_swap_chain_get_image_view's existing index-0 lookup keeps
	// working unchanged. Repointed at _current_drawable.texture on every
	// acquire.
	crgpu_image_metal *_back_buffer_images;
	crgpu_image_view_metal *_back_buffer_image_views;
	// Signaled once at creation and never touched again - see
	// crgpu_swap_chain_create. Kept only so
	// crgpu_swap_chain_get_image_available_semaphore returns something
	// valid for callers following the Vulkan-shaped pattern; Metal's own
	// acquire/present path never waits on it, since nextDrawable already
	// blocks until the image is genuinely available before acquire returns.
	crgpu_semaphore_metal *_image_available_semaphore;
	// Genuinely signaled per-frame by crgpu_command_queue_submit/
	// crgpu_command_queue_submit_lists_synced when a caller includes it, but
	// Metal's own crgpu_swap_chain_present doesn't consume it either -
	// present ordering comes from committing to _present_queue after the
	// render work, the same guarantee crgpu_command_queue_wait_idle relies on
	// (mirrors the D3D12 backend, which doesn't consume it in present() at
	// all). Kept for API symmetry with Vulkan, which does consume it.
	crgpu_semaphore_metal *_render_finished_semaphore;
	crgpu_fence_metal *_in_flight_fence;
} crgpu_swap_chain_metal;

#endif // CARROTTUS_CRGPUMETAL_H

#endif // CRGPU_COMPILE_METAL_API
