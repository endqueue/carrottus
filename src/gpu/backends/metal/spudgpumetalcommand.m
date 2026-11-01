//
// SpudGPU Metal backend - command queues/buffers and submission.
// spudgpu_command_queue, spudgpu_command_allocator, and spudgpu_command_list
// are implemented and verified against real Apple hardware. Command list
// encoding (viewports, draws, pipeline binds, ...) below is still a
// placeholder pending its own pass - see the remaining METAL API CODE markers.
//

#if SPUDGPU_COMPILE_METAL_API

#include "spudgpu.h"
#include "spudcore.h"
#include "spudgpumetal.h"
#include <Metal/Metal.h>
#include <assert.h>
#include <string.h>
#include "../../../spudstructbase.h"

SPUDRESULT spudgpu_get_command_queue(
    spudgpu_device device,
    SPUDGPU_COMMAND_LIST_TYPE type,
    uint32_t index,
    spudgpu_command_queue *out_queue) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_get_command_queue", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	spudgpu_command_queue *queues = NULL;
	uint32_t count                = 0;
	switch (type) {
	case SPUDGPU_COMMAND_LIST_TYPE_DIRECT:
		queues = device->_cmd_queues_direct;
		count  = device->_cmd_queue_count_direct;
		break;
	case SPUDGPU_COMMAND_LIST_TYPE_COMPUTE:
		queues = device->_cmd_queues_compute;
		count  = device->_cmd_queue_count_compute;
		break;
	case SPUDGPU_COMMAND_LIST_TYPE_COPY:
		queues = device->_cmd_queues_copy;
		count  = device->_cmd_queue_count_copy;
		break;
	default:
		// Bundles aren't submitted to a queue, on any backend.
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST_TYPE;
	}
	if (index >= count)
		return SPUDRESULT_INDEX_OUT_OF_RANGE;
	if (!out_queue)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_queue = queues[index];
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_submit_command_lists(
    spudgpu_command_queue queue,
    spudgpu_command_list *cmd_lists,
    uint32_t cmd_list_count) {
	if (!queue)
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	if (SPUD_STALE(queue, "spudgpu_submit_command_lists", "queue"))
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	if (!cmd_lists)
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	if (cmd_list_count == 0)
		return SPUDRESULT_ZERO_SIZE;

	// Metal command buffers are bound to the queue they were created from
	// (see spudgpu_create_command_list) rather than submitted to an arbitrary
	// queue at commit time the way Vulkan/D3D12 command lists are - there is
	// no per-submit queue selection to make here beyond committing each buffer.
	spudgpumetal___queue_enter((spudgpu_command_queue_metal *)queue);
	for (uint32_t i = 0; i < cmd_list_count; i++) {
		spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd_lists[i];
		if (cmd_list_metal && cmd_list_metal->_command_buffer_mtl) {
			[cmd_list_metal->_command_buffer_mtl commit];
		}
	}
	spudgpumetal___queue_leave((spudgpu_command_queue_metal *)queue);

	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_submit_command_lists_synced(
    spudgpu_command_queue queue,
    spudgpu_command_list *cmd_lists,
    uint32_t cmd_list_count,
    spudgpu_swap_chain swap_chain) {
	if (!queue)
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	if (SPUD_STALE(queue, "spudgpu_submit_command_lists_synced", "queue"))
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	if (!cmd_lists)
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	if (cmd_list_count == 0)
		return SPUDRESULT_ZERO_SIZE;
	if (!swap_chain)
		return SPUDRESULT_GPU_INVALID_SWAP_CHAIN;
	if (SPUD_STALE(swap_chain, "spudgpu_submit_command_lists_synced", "swap_chain"))
		return SPUDRESULT_GPU_INVALID_SWAP_CHAIN;

	// spudgpu_queue_submit with only the command lists and the swap chain set.
	spudgpu_submit_desc desc = {0};
	desc.base.type = SPUD_STRUCT_TYPE_GPU_SUBMIT_DESC;
	desc.cmd_lists           = cmd_lists;
	desc.cmd_list_count      = cmd_list_count;
	desc.swap_chain          = swap_chain;
	return spudgpu_queue_submit(queue, &desc);
}

// The queue a command allocator's lists pull their command buffers from:
// the first of its type the device has, a bundle's the first direct one
// (Metal has no bundle, let alone a bundle queue). NULL when the device has
// none of that type, which spudgpu_create_command_allocator refuses.
static spudgpu_command_queue_metal *spudgpumetal___internal_resolve_allocator_queue(
    spudgpu_device_metal *device,
    SPUDGPU_COMMAND_LIST_TYPE type) {
	switch (type) {
	case SPUDGPU_COMMAND_LIST_TYPE_COPY:
		return device->_cmd_queue_count_copy ? (spudgpu_command_queue_metal *)device->_cmd_queues_copy[0] : NULL;
	case SPUDGPU_COMMAND_LIST_TYPE_COMPUTE:
		return device->_cmd_queue_count_compute ? (spudgpu_command_queue_metal *)device->_cmd_queues_compute[0] : NULL;
	case SPUDGPU_COMMAND_LIST_TYPE_DIRECT:
	case SPUDGPU_COMMAND_LIST_TYPE_BUNDLE:
		return device->_cmd_queue_count_direct ? (spudgpu_command_queue_metal *)device->_cmd_queues_direct[0] : NULL;
	default:
		return NULL;
	}
}

// Pulls a fresh id<MTLCommandBuffer> for cmd_list_metal from its allocator's
// queue, releasing whatever buffer it already held. Metal command buffers are
// single-use - once committed they can't be reset and re-recorded the way a
// VkCommandBuffer or ID3D12GraphicsCommandList can - so this is what backs
// both spudgpu_create_command_list's initial buffer and every subsequent
// spudgpu_begin_command_list call that reuses the same public handle across
// frames. -commandBuffer returns an autoreleased object (unlike -newCommandQueue),
// so it must be retained to survive past the current autorelease pool.
//
// The pool here is drained before this returns, which leaves the retain
// above as the buffer's only reference. Without it the autoreleased
// reference waits for a pool of the caller's, and a caller that is plain C
// with no run loop has none that ever drains: a buffer released without
// being committed (the initial one of every command list, replaced at its
// first begin) would then never be deallocated, and a buffer that is never
// committed only gives its place back to the queue when it is. A queue
// hands out a fixed number at once, so after that many -commandBuffer
// blocks for good.
static void spudgpumetal___internal_acquire_command_buffer(
    spudgpu_command_list_metal *cmd_list_metal) {
	spudgpu_command_allocator_metal *allocator_metal = cmd_list_metal->_parent_allocator;
	if (!allocator_metal || !allocator_metal->_parent_queue)
		return;
	id<MTLCommandQueue> queue_mtl = allocator_metal->_parent_queue->_command_queue_mtl;
	if (!queue_mtl)
		return;

	if (cmd_list_metal->_command_buffer_mtl) {
		[cmd_list_metal->_command_buffer_mtl release];
	}
	@autoreleasepool {
		cmd_list_metal->_command_buffer_mtl = [[queue_mtl commandBuffer] retain];
	}
}

SPUDRESULT spudgpu_create_command_allocator(
    spudgpu_device device,
    const spudgpu_command_allocator_desc *desc,
    spudgpu_command_allocator *out_allocator) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_command_allocator", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_allocator)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	spudgpu_command_queue_metal *queue = spudgpumetal___internal_resolve_allocator_queue(device, desc->type);
	if (!queue)
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST_TYPE;

	spudgpu_command_allocator_metal *allocator_metal =
	    SPUD_NEW_ZEROED(&device->head.allocator, spudgpu_command_allocator_metal);
	if (!allocator_metal)
		return SPUDRESULT_OUT_OF_MEMORY;
	allocator_metal->head.allocator = device->head.allocator;
	SPUD_BIRTH(allocator_metal);
	allocator_metal->_parent_device = (spudgpu_device_metal *)device;
	allocator_metal->_desc          = *desc;
	allocator_metal->_parent_queue  = queue;

	SPUDGPU___LIVE_ADD(&allocator_metal->_parent_device->_live, SPUDGPU___OBJECT_COMMAND_ALLOCATOR);
	*out_allocator = (spudgpu_command_allocator)allocator_metal;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_command_allocator(spudgpu_command_allocator allocator) {
	spudgpu_command_allocator_metal *allocator_metal = (spudgpu_command_allocator_metal *)allocator;
	if (allocator_metal) {
		if (SPUD_STALE(allocator, "spudgpu_destroy_command_allocator", "allocator"))
			return;
		// _parent_queue is borrowed from the device, which releases it.
		SPUDGPU___LIVE_REMOVE(&allocator_metal->_parent_device->_live, SPUDGPU___OBJECT_COMMAND_ALLOCATOR);
		SPUD_KILL(allocator);
		const SPUD_ALLOCATOR memory_allocator = allocator_metal->head.allocator;
		spud_string_free(&memory_allocator, allocator_metal->head.debug_name);
		SPUD_DELETE(&memory_allocator, spudgpu_command_allocator_metal, allocator_metal);
	}
}

SPUDRESULT spudgpu_reset_command_allocator(spudgpu_command_allocator allocator) {
	if (!allocator)
		return SPUDRESULT_GPU_INVALID_COMMAND_ALLOCATOR;
	if (SPUD_STALE(allocator, "spudgpu_reset_command_allocator", "allocator"))
		return SPUDRESULT_GPU_INVALID_COMMAND_ALLOCATOR;
#if _DEBUG
	// spudgpu.h: no list of the allocator is open when it is reset.
	assert(!((spudgpu_command_allocator_metal *)allocator)->_open_list &&
	       "spudgpu_reset_command_allocator: a list of this allocator is still open");
#endif

	// Nothing to reset: this allocator holds no pooled Metal object of its
	// own (see spudgpu_command_allocator_metal in spudgpumetal.h) - every
	// command list pulls a brand new id<MTLCommandBuffer> from the queue on
	// creation and on every spudgpu_begin_command_list instead.

	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_create_command_list(
    spudgpu_command_allocator allocator,
    spudgpu_command_list *out_cmd_list) {
	if (!allocator)
		return SPUDRESULT_GPU_INVALID_COMMAND_ALLOCATOR;
	if (SPUD_STALE(allocator, "spudgpu_create_command_list", "allocator"))
		return SPUDRESULT_GPU_INVALID_COMMAND_ALLOCATOR;
	if (!out_cmd_list)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;

	SPUDRESULT sr = SPUD_SUCCESS;

	spudgpu_command_list_metal *cmd_list_metal =
	    SPUD_NEW_ZEROED(&allocator->head.allocator, spudgpu_command_list_metal);
	if (!cmd_list_metal) {
		sr = SPUDRESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	cmd_list_metal->head.allocator    = allocator->head.allocator;
	SPUD_BIRTH(cmd_list_metal);
	cmd_list_metal->_parent_allocator = (spudgpu_command_allocator_metal *)allocator;

	cmd_list_metal->_pass_desc_mtl = [[MTLRenderPassDescriptor alloc] init];
	bool made                      = cmd_list_metal->_pass_desc_mtl != nil;
	if (made && (cmd_list_metal->_parent_allocator->_parent_device->_features & SPUDGPU_FEATURE_RAY_QUERY)) {
		cmd_list_metal->_rq_top_desc_mtl    = [[MTLInstanceAccelerationStructureDescriptor alloc] init];
		cmd_list_metal->_rq_bottom_desc_mtl = [[MTLPrimitiveAccelerationStructureDescriptor alloc] init];
		cmd_list_metal->_rq_geometries_mtl  = [[NSMutableArray alloc] init];
		cmd_list_metal->_rq_triangles_mtl   = [[NSMutableArray alloc] init];
		cmd_list_metal->_rq_boxes_mtl       = [[NSMutableArray alloc] init];
		made = cmd_list_metal->_rq_top_desc_mtl && cmd_list_metal->_rq_bottom_desc_mtl && cmd_list_metal->_rq_geometries_mtl &&
		       cmd_list_metal->_rq_triangles_mtl && cmd_list_metal->_rq_boxes_mtl;
		if (made)
			cmd_list_metal->_rq_bottom_desc_mtl.geometryDescriptors = cmd_list_metal->_rq_geometries_mtl;
	}
	if (!made) {
		[cmd_list_metal->_pass_desc_mtl release];
		[cmd_list_metal->_rq_top_desc_mtl release];
		[cmd_list_metal->_rq_bottom_desc_mtl release];
		[cmd_list_metal->_rq_geometries_mtl release];
		[cmd_list_metal->_rq_triangles_mtl release];
		[cmd_list_metal->_rq_boxes_mtl release];
		const SPUD_ALLOCATOR list_allocator = cmd_list_metal->head.allocator;
		SPUD_KILL(cmd_list_metal);
		SPUD_DELETE(&list_allocator, spudgpu_command_list_metal, cmd_list_metal);
		sr = SPUDRESULT_OUT_OF_MEMORY;
		goto failedattempt;
	}
	spudgpumetal___internal_acquire_command_buffer(cmd_list_metal);

	SPUDGPU___LIVE_ADD(&cmd_list_metal->_parent_allocator->_parent_device->_live, SPUDGPU___OBJECT_COMMAND_LIST);
	*out_cmd_list = (spudgpu_command_list)cmd_list_metal;

	return sr;

failedattempt:
	return sr;
}

void spudgpu_destroy_command_list(spudgpu_command_list cmd) {
	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (cmd_list_metal) {
		if (SPUD_STALE(cmd, "spudgpu_destroy_command_list", "cmd"))
			return;
		if (cmd_list_metal->_command_buffer_mtl) {
			[cmd_list_metal->_command_buffer_mtl release];
		}
		[cmd_list_metal->_pass_desc_mtl release];
		[cmd_list_metal->_rq_top_desc_mtl release];
		[cmd_list_metal->_rq_bottom_desc_mtl release];
		[cmd_list_metal->_rq_geometries_mtl release];
		[cmd_list_metal->_rq_triangles_mtl release];
		[cmd_list_metal->_rq_boxes_mtl release];
#if _DEBUG
		// A list destroyed open is not open any more.
		if (cmd_list_metal->_parent_allocator->_open_list == cmd)
			cmd_list_metal->_parent_allocator->_open_list = NULL;
#endif
		SPUDGPU___LIVE_REMOVE(&cmd_list_metal->_parent_allocator->_parent_device->_live, SPUDGPU___OBJECT_COMMAND_LIST);
		SPUD_KILL(cmd);
		const SPUD_ALLOCATOR allocator = cmd_list_metal->head.allocator;
		spud_string_free(&allocator, cmd_list_metal->head.debug_name);
		SPUD_DELETE(&allocator, spudgpu_command_list_metal, cmd_list_metal);
	}
}

void spudgpu_begin_command_list(spudgpu_command_list cmd) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_begin_command_list", "cmd"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
#if _DEBUG
	// spudgpu.h: one list of an allocator open at a time. Metal would
	// allow this; D3D12 would refuse it.
	assert(!cmd_list_metal->_parent_allocator->_open_list &&
	       "spudgpu_begin_command_list: a list of this allocator is already open; end it first");
	cmd_list_metal->_parent_allocator->_open_list = cmd;
#endif
	// A recording starts with nothing pushed, as a Vulkan command buffer does.
	cmd_list_metal->_push_constants_pending_stages = 0;
	spudgpumetal___internal_acquire_command_buffer(cmd_list_metal);
}

void spudgpu_end_command_list(spudgpu_command_list cmd) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_end_command_list", "cmd"))
		return;
#if _DEBUG
	{
		spudgpu_command_allocator_metal *allocator_metal = ((spudgpu_command_list_metal *)cmd)->_parent_allocator;
		if (allocator_metal->_open_list == cmd)
			allocator_metal->_open_list = NULL;
	}
#endif

	// Metal command buffers have no vkEndCommandBuffer/Close() equivalent -
	// encoding simply stops once the last active encoder is ended, and the
	// buffer is submittable as-is. Rendering already requires the caller to
	// end it explicitly via spudgpu_cmd_end_rendering (there's no equivalent
	// "end dispatch"/"end copy" call in the public API for compute or blit
	// encoders, so this is the one place that's guaranteed to run before
	// every commit - see spudgpu_cmd_dispatch/spudgpu_cmd_bind_compute_pipeline
	// below and spudgpu_cmd_copy_buffer/etc in spudgpumetalrenderpass.m).
	spudgpumetal___internal_end_active_compute_encoder((spudgpu_command_list_metal *)cmd);
	spudgpumetal___internal_end_active_blit_encoder((spudgpu_command_list_metal *)cmd);
}

// A label inside a pass goes on the render encoder; one outside every pass
// on the command buffer, which takes a debug group only while no encoder is
// open, so an open compute or blit encoder is ended first.
void spudgpu_cmd_begin_debug_label(spudgpu_command_list cmd, const char *label) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_begin_debug_label", "cmd"))
		return;
	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	// Allocated and released here rather than autoreleased: a caller in
	// plain C may have no autorelease pool.
	NSString *text = [[NSString alloc] initWithUTF8String:label ? label : ""];
	if (!text)
		text = [[NSString alloc] initWithUTF8String:""];
	if (cmd_list_metal->_active_render_encoder) {
		[cmd_list_metal->_active_render_encoder pushDebugGroup:text];
	} else {
		spudgpumetal___internal_end_active_compute_encoder(cmd_list_metal);
		spudgpumetal___internal_end_active_blit_encoder(cmd_list_metal);
		[cmd_list_metal->_command_buffer_mtl pushDebugGroup:text];
	}
	[text release];
}

void spudgpu_cmd_end_debug_label(spudgpu_command_list cmd) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_end_debug_label", "cmd"))
		return;
	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (cmd_list_metal->_active_render_encoder) {
		[cmd_list_metal->_active_render_encoder popDebugGroup];
	} else {
		spudgpumetal___internal_end_active_compute_encoder(cmd_list_metal);
		spudgpumetal___internal_end_active_blit_encoder(cmd_list_metal);
		[cmd_list_metal->_command_buffer_mtl popDebugGroup];
	}
}

// Ends and releases _active_compute_encoder if one is active. Shared with
// spudgpu_cmd_begin_rendering (spudgpumetalrenderpass.m) - see this
// function's declaration in spudgpumetal.h.
void spudgpumetal___internal_end_active_compute_encoder(spudgpu_command_list_metal *cmd_list_metal) {
	if (!cmd_list_metal || !cmd_list_metal->_active_compute_encoder)
		return;
	[cmd_list_metal->_active_compute_encoder endEncoding];
	[cmd_list_metal->_active_compute_encoder release];
	cmd_list_metal->_active_compute_encoder         = nil;
	cmd_list_metal->_encoder_compute_pipeline_state = nil;
	for (uint32_t i = 0; i < SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS; i++)
		cmd_list_metal->_encoder_compute_sets[i] = nil;
}

// Returns the active compute encoder, lazily opening one (ending any active
// render encoder first - Metal disallows two live encoders of different
// kinds on one command buffer at once, see _active_compute_encoder's comment
// in spudgpumetal.h) if none exists yet.
static id<MTLComputeCommandEncoder> spudgpumetal___internal_ensure_compute_encoder(
    spudgpu_command_list_metal *cmd_list_metal) {
	if (cmd_list_metal->_active_compute_encoder)
		return cmd_list_metal->_active_compute_encoder;

	if (cmd_list_metal->_active_render_encoder) {
		[cmd_list_metal->_active_render_encoder endEncoding];
		[cmd_list_metal->_active_render_encoder release];
		cmd_list_metal->_active_render_encoder = nil;
	}
	spudgpumetal___internal_end_active_blit_encoder(cmd_list_metal);

	id<MTLComputeCommandEncoder> encoder = [cmd_list_metal->_command_buffer_mtl computeCommandEncoder];
	cmd_list_metal->_active_compute_encoder = [encoder retain];
	return cmd_list_metal->_active_compute_encoder;
}

void spudgpu_cmd_set_viewports(
    spudgpu_command_list cmd,
    uint32_t first_viewport,
    uint32_t viewport_count,
    const SPUDGPU_VIEWPORT *viewports) {
	if (!cmd || !viewports || viewport_count == 0)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_set_viewports", "cmd"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (first_viewport + viewport_count > SPUDGPU_METAL_MAX_VIEWPORTS)
		return;

	// -setViewports:count: always sets from slot 0 with no first_viewport-
	// style offset, unlike vkCmdSetViewport - patch the persistent array in
	// the requested sub-range and resend everything set so far.
	for (uint32_t i = 0; i < viewport_count; i++) {
		cmd_list_metal->_viewports[first_viewport + i] = viewports[i];
	}
	uint32_t new_high_water = first_viewport + viewport_count;
	if (new_high_water > cmd_list_metal->_viewport_count)
		cmd_list_metal->_viewport_count = new_high_water;

	if (!cmd_list_metal->_active_render_encoder)
		return;

	MTLViewport mtl_viewports[SPUDGPU_METAL_MAX_VIEWPORTS];
	for (uint32_t i = 0; i < cmd_list_metal->_viewport_count; i++) {
		const SPUDGPU_VIEWPORT *v = &cmd_list_metal->_viewports[i];
		mtl_viewports[i] = (MTLViewport){
		    .originX = v->x,
		    .originY = v->y,
		    .width   = v->width,
		    .height  = v->height,
		    .znear   = v->minDepth,
		    .zfar    = v->maxDepth,
		};
	}
	const uint32_t count = cmd_list_metal->_viewport_count;
	if (count == cmd_list_metal->_encoder_viewport_count &&
	    memcmp(mtl_viewports, cmd_list_metal->_encoder_viewports, sizeof(MTLViewport) * count) == 0)
		return;
	[cmd_list_metal->_active_render_encoder setViewports:mtl_viewports count:count];
	memcpy(cmd_list_metal->_encoder_viewports, mtl_viewports, sizeof(MTLViewport) * count);
	cmd_list_metal->_encoder_viewport_count = count;
}

void spudgpu_cmd_set_scissor_rects(
    spudgpu_command_list cmd,
    uint32_t first_scissor_rect,
    uint32_t scissor_rect_count,
    const SPUDGPU_SCISSOR_RECT *scissor_rects) {
	if (!cmd || !scissor_rects || scissor_rect_count == 0)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_set_scissor_rects", "cmd"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (first_scissor_rect + scissor_rect_count > SPUDGPU_METAL_MAX_VIEWPORTS)
		return;

	// Same reasoning as spudgpu_cmd_set_viewports above -
	// -setScissorRects:count: has no offset either.
	for (uint32_t i = 0; i < scissor_rect_count; i++) {
		cmd_list_metal->_scissor_rects[first_scissor_rect + i] = scissor_rects[i];
	}
	uint32_t new_high_water = first_scissor_rect + scissor_rect_count;
	if (new_high_water > cmd_list_metal->_scissor_rect_count)
		cmd_list_metal->_scissor_rect_count = new_high_water;

	if (!cmd_list_metal->_active_render_encoder)
		return;

	MTLScissorRect mtl_rects[SPUDGPU_METAL_MAX_VIEWPORTS];
	for (uint32_t i = 0; i < cmd_list_metal->_scissor_rect_count; i++) {
		const SPUDGPU_SCISSOR_RECT *r = &cmd_list_metal->_scissor_rects[i];
		mtl_rects[i] = (MTLScissorRect){
		    .x      = (NSUInteger)r->x,
		    .y      = (NSUInteger)r->y,
		    .width  = (NSUInteger)r->width,
		    .height = (NSUInteger)r->height,
		};
	}
	const uint32_t count = cmd_list_metal->_scissor_rect_count;
	if (count == cmd_list_metal->_encoder_scissor_rect_count &&
	    memcmp(mtl_rects, cmd_list_metal->_encoder_scissor_rects, sizeof(MTLScissorRect) * count) == 0)
		return;
	[cmd_list_metal->_active_render_encoder setScissorRects:mtl_rects count:count];
	memcpy(cmd_list_metal->_encoder_scissor_rects, mtl_rects, sizeof(MTLScissorRect) * count);
	cmd_list_metal->_encoder_scissor_rect_count = count;
}

void spudgpu_cmd_set_vertex_buffers(
    spudgpu_command_list cmd,
    uint32_t start_slot,
    uint32_t view_count,
    spudgpu_buffer_view *buffer_views) {
	if (!cmd || !buffer_views || view_count == 0)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_set_vertex_buffers", "cmd"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (!cmd_list_metal->_active_render_encoder)
		return;

	// Each slot set on its own, and only where it changes: Metal's validation
	// refuses setting a buffer the encoder already has at that index, and a
	// caller rebinding the same buffer between draws is ordinary.
	id<MTLRenderCommandEncoder> encoder = cmd_list_metal->_active_render_encoder;
	for (uint32_t i = 0; i < view_count; i++) {
		spudgpu_buffer_view_metal *view_metal = (spudgpu_buffer_view_metal *)buffer_views[i];
		spudgpu_buffer_metal *parent_buffer_metal =
		    view_metal ? (spudgpu_buffer_metal *)view_metal->_desc.parent_buffer : NULL;
		id<MTLBuffer> buffer   = parent_buffer_metal ? parent_buffer_metal->_buffer_mtl : nil;
		NSUInteger offset      = view_metal ? (NSUInteger)view_metal->_desc.offset_from_parent_buffer : 0;
		const uint32_t slot    = start_slot + i;
		if (slot >= SPUDGPU_MAX_VERTEX_BINDINGS) {
			[encoder setVertexBuffer:buffer offset:offset atIndex:slot];
			continue;
		}
		if (buffer && buffer == cmd_list_metal->_encoder_vertex_buffers[slot]) {
			if (offset != cmd_list_metal->_encoder_vertex_offsets[slot])
				[encoder setVertexBufferOffset:offset atIndex:slot];
		} else {
			[encoder setVertexBuffer:buffer offset:offset atIndex:slot];
		}
		cmd_list_metal->_encoder_vertex_buffers[slot] = buffer;
		cmd_list_metal->_encoder_vertex_offsets[slot] = offset;
	}
}

void spudgpu_cmd_set_index_buffer(
    spudgpu_command_list cmd,
    spudgpu_buffer_view buffer_view) {
	if (!cmd || !buffer_view)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_set_index_buffer", "cmd"))
		return;
	if (SPUD_STALE(buffer_view, "spudgpu_cmd_set_index_buffer", "buffer_view"))
		return;

	// Metal has no bind-ahead-of-time index buffer state the way Vulkan's
	// vkCmdBindIndexBuffer/D3D12's IASetIndexBuffer do - the index buffer,
	// its offset, and its element type are all passed directly as arguments
	// to -drawIndexedPrimitives:... itself (see spudgpu_cmd_draw_indexed/
	// _instanced below). This just remembers the view for that call to use.
	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	cmd_list_metal->_bound_index_buffer_view    = (spudgpu_buffer_view_metal *)buffer_view;
}

// SPUDGPU_PRIMITIVE_TOPOLOGY_PATCH_LIST has no MTLPrimitiveType: Metal has no
// primitive type for tessellation patches at all - post-tessellation
// drawing goes through -drawPatches:.../-drawIndexedPatches:..., an
// entirely different call this backend doesn't implement yet. Falls back to
// Triangle rather than silently misinterpreting patch data as points.
static MTLPrimitiveType spudgpumetal___internal_primitive_type(SPUDGPU_PRIMITIVE_TOPOLOGY topology) {
	switch (topology) {
	case SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  return MTLPrimitiveTypeTriangle;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_LINE_LIST:      return MTLPrimitiveTypeLine;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return MTLPrimitiveTypeLineStrip;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_POINT_LIST:     return MTLPrimitiveTypePoint;
	default:                                        return MTLPrimitiveTypeTriangle;
	}
}

// Metal has no separate index-format enum the way SPUDGPU_FORMAT/DXGI_FORMAT
// do - only two widths exist, inferred from the bound view's stride, same
// convention the D3D12 backend uses for its own index buffer views.
static bool spudgpumetal___internal_resolve_index_type(uint32_t stride, MTLIndexType *out_type) {
	if (stride == 4) {
		*out_type = MTLIndexTypeUInt32;
		return true;
	}
	if (stride == 2) {
		*out_type = MTLIndexTypeUInt16;
		return true;
	}
	return false;
}

// The push constant block, to the stages spudgpu_cmd_push_constants has
// written for since the last draw: what Vulkan's and D3D12's command
// state hold until a draw reads it. Sent at the draw, not at the push, so a
// render encoder never carries bytes no draw of it used (Metal's
// validation, with warnings fatal, stops at such an encoder's end); and a
// block pushed before the encoder opened reaches its first draw, as it
// would on Vulkan. Called by every draw after its encoder check.
static void spudgpumetal___internal_send_push_constants(spudgpu_command_list_metal *cmd_list_metal) {
	uint32_t stages = cmd_list_metal->_push_constants_pending_stages;
	if (!stages)
		return;
	cmd_list_metal->_push_constants_pending_stages = 0;
	// Only to the stages whose function in the bound pipeline reads them
	// (its reflection, spudgpu_shader_pipeline_metal::_non_fragment_set_mask).
	const spudgpu_shader_pipeline_metal *pipeline = (const spudgpu_shader_pipeline_metal *)cmd_list_metal->_bound_pipeline;
	if (pipeline) {
		if (!(pipeline->_non_fragment_set_mask & SPUDGPU_METAL_PUSH_CONSTANTS_MASK_BIT))
			stages &= ~(uint32_t)(SPUDGPU_SHADER_STAGE_VERTEX | SPUDGPU_SHADER_STAGE_MESH);
		if (!(pipeline->_fragment_set_mask & SPUDGPU_METAL_PUSH_CONSTANTS_MASK_BIT))
			stages &= ~(uint32_t)SPUDGPU_SHADER_STAGE_FRAGMENT;
	}
	if (stages & SPUDGPU_SHADER_STAGE_VERTEX) {
		[cmd_list_metal->_active_render_encoder setVertexBytes:cmd_list_metal->_push_constants_scratch
		                                                 length:cmd_list_metal->_push_constants_extent
		                                                atIndex:SPUDGPU_METAL_PUSH_CONSTANTS_BUFFER_INDEX];
	}
	if (stages & SPUDGPU_SHADER_STAGE_FRAGMENT) {
		[cmd_list_metal->_active_render_encoder setFragmentBytes:cmd_list_metal->_push_constants_scratch
		                                                   length:cmd_list_metal->_push_constants_extent
		                                                  atIndex:SPUDGPU_METAL_PUSH_CONSTANTS_BUFFER_INDEX];
	}
#if SPUDGPU_EXT_MESH_SHADING
	if (stages & SPUDGPU_SHADER_STAGE_MESH) {
		[cmd_list_metal->_active_render_encoder setMeshBytes:cmd_list_metal->_push_constants_scratch
		                                               length:cmd_list_metal->_push_constants_extent
		                                              atIndex:SPUDGPU_METAL_PUSH_CONSTANTS_BUFFER_INDEX];
	}
#endif
}

void spudgpu_cmd_draw(
    spudgpu_command_list cmd,
    uint32_t vertex_count,
    uint32_t start_vertex_location) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_draw", "cmd"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (!cmd_list_metal->_active_render_encoder)
		return;
	spudgpumetal___internal_send_push_constants(cmd_list_metal);
	spudgpu_shader_pipeline_metal *pipeline_metal = (spudgpu_shader_pipeline_metal *)cmd_list_metal->_bound_pipeline;
	if (!pipeline_metal)
		return;

	[cmd_list_metal->_active_render_encoder
	    drawPrimitives:spudgpumetal___internal_primitive_type(pipeline_metal->_desc.primitive_topology)
	       vertexStart:start_vertex_location
	       vertexCount:vertex_count];
}

void spudgpu_cmd_draw_indexed(
    spudgpu_command_list cmd,
    uint32_t index_count,
    uint32_t start_index_location,
    int32_t base_vertex_location) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_draw_indexed", "cmd"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (!cmd_list_metal->_active_render_encoder)
		return;
	spudgpumetal___internal_send_push_constants(cmd_list_metal);
	spudgpu_shader_pipeline_metal *pipeline_metal = (spudgpu_shader_pipeline_metal *)cmd_list_metal->_bound_pipeline;
	spudgpu_buffer_view_metal *index_view         = cmd_list_metal->_bound_index_buffer_view;
	if (!pipeline_metal || !index_view)
		return;
	spudgpu_buffer_metal *index_buffer_metal = (spudgpu_buffer_metal *)index_view->_desc.parent_buffer;
	if (!index_buffer_metal)
		return;

	MTLIndexType index_type;
	if (!spudgpumetal___internal_resolve_index_type(index_view->_desc.stride, &index_type))
		return;
	NSUInteger index_size    = (index_type == MTLIndexTypeUInt32) ? 4 : 2;
	NSUInteger buffer_offset = (NSUInteger)index_view->_desc.offset_from_parent_buffer +
	                           (NSUInteger)start_index_location * index_size;

	[cmd_list_metal->_active_render_encoder
	    drawIndexedPrimitives:spudgpumetal___internal_primitive_type(pipeline_metal->_desc.primitive_topology)
	               indexCount:index_count
	                indexType:index_type
	              indexBuffer:index_buffer_metal->_buffer_mtl
	        indexBufferOffset:buffer_offset
	            instanceCount:1
	               baseVertex:base_vertex_location
	             baseInstance:0];
}

void spudgpu_cmd_draw_instanced(
    spudgpu_command_list cmd,
    uint32_t vertex_count_per_instance,
    uint32_t instance_count,
    uint32_t start_vertex_location,
    uint32_t start_instance_location) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_draw_instanced", "cmd"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (!cmd_list_metal->_active_render_encoder)
		return;
	spudgpumetal___internal_send_push_constants(cmd_list_metal);
	spudgpu_shader_pipeline_metal *pipeline_metal = (spudgpu_shader_pipeline_metal *)cmd_list_metal->_bound_pipeline;
	if (!pipeline_metal)
		return;

	[cmd_list_metal->_active_render_encoder
	    drawPrimitives:spudgpumetal___internal_primitive_type(pipeline_metal->_desc.primitive_topology)
	       vertexStart:start_vertex_location
	       vertexCount:vertex_count_per_instance
	     instanceCount:instance_count
	      baseInstance:start_instance_location];
}

void spudgpu_cmd_draw_indexed_instanced(
    spudgpu_command_list cmd,
    uint32_t index_count_per_instance,
    uint32_t instance_count,
    uint32_t start_index_location,
    int32_t base_vertex_location,
    uint32_t start_instance_location) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_draw_indexed_instanced", "cmd"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (!cmd_list_metal->_active_render_encoder)
		return;
	spudgpumetal___internal_send_push_constants(cmd_list_metal);
	spudgpu_shader_pipeline_metal *pipeline_metal = (spudgpu_shader_pipeline_metal *)cmd_list_metal->_bound_pipeline;
	spudgpu_buffer_view_metal *index_view         = cmd_list_metal->_bound_index_buffer_view;
	if (!pipeline_metal || !index_view)
		return;
	spudgpu_buffer_metal *index_buffer_metal = (spudgpu_buffer_metal *)index_view->_desc.parent_buffer;
	if (!index_buffer_metal)
		return;

	MTLIndexType index_type;
	if (!spudgpumetal___internal_resolve_index_type(index_view->_desc.stride, &index_type))
		return;
	NSUInteger index_size    = (index_type == MTLIndexTypeUInt32) ? 4 : 2;
	NSUInteger buffer_offset = (NSUInteger)index_view->_desc.offset_from_parent_buffer +
	                           (NSUInteger)start_index_location * index_size;

	[cmd_list_metal->_active_render_encoder
	    drawIndexedPrimitives:spudgpumetal___internal_primitive_type(pipeline_metal->_desc.primitive_topology)
	               indexCount:index_count_per_instance
	                indexType:index_type
	              indexBuffer:index_buffer_metal->_buffer_mtl
	        indexBufferOffset:buffer_offset
	            instanceCount:instance_count
	               baseVertex:base_vertex_location
	             baseInstance:start_instance_location];
}

static MTLCullMode spudgpumetal___internal_cull_mode(SPUDGPU_CULL_MODE mode) {
	switch (mode) {
	case SPUDGPU_CULL_MODE_FRONT: return MTLCullModeFront;
	case SPUDGPU_CULL_MODE_BACK:  return MTLCullModeBack;
	default:                      return MTLCullModeNone;
	}
}

#if SPUDGPU_EXT_DEPTH_BOUNDS_TEST
// Sets the render encoder's depth bounds to [min, max] where they differ
// from what it has, on a device created with the test.
static void spudgpumetal___internal_set_encoder_depth_bounds(spudgpu_command_list_metal *cmd_list_metal, float min_bound, float max_bound) {
	if ((cmd_list_metal->_parent_allocator->_parent_device->_features & SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST) == 0)
		return;
	if (cmd_list_metal->_encoder_depth_bounds[0] == min_bound && cmd_list_metal->_encoder_depth_bounds[1] == max_bound)
		return;
	if (@available(macOS 26.0, iOS 26.0, *)) {
		[cmd_list_metal->_active_render_encoder setDepthTestMinBound:min_bound maxBound:max_bound];
		cmd_list_metal->_encoder_depth_bounds[0] = min_bound;
		cmd_list_metal->_encoder_depth_bounds[1] = max_bound;
	}
}
#endif

void spudgpu_cmd_bind_pipeline(
    spudgpu_command_list cmd,
    spudgpu_shader_pipeline pipeline) {
	if (!cmd || !pipeline)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_bind_pipeline", "cmd"))
		return;
	if (SPUD_STALE(pipeline, "spudgpu_cmd_bind_pipeline", "pipeline"))
		return;

	spudgpu_command_list_metal *cmd_list_metal    = (spudgpu_command_list_metal *)cmd;
	spudgpu_shader_pipeline_metal *pipeline_metal = (spudgpu_shader_pipeline_metal *)pipeline;
	cmd_list_metal->_bound_pipeline                = (struct spudgpu_shader_pipeline_t *)pipeline_metal;

	if (cmd_list_metal->_active_render_encoder) {
		// Each state is set only if the encoder doesn't have it already;
		// see _encoder_depth_stencil_state_mtl.
		if (cmd_list_metal->_encoder_pipeline_state_mtl != pipeline_metal->_render_pipeline_state_mtl) {
			[cmd_list_metal->_active_render_encoder setRenderPipelineState:pipeline_metal->_render_pipeline_state_mtl];
			cmd_list_metal->_encoder_pipeline_state_mtl = pipeline_metal->_render_pipeline_state_mtl;
		}
		// Vulkan/D3D12 bake depth/stencil test config into the same
		// monolithic pipeline object as everything else; Metal keeps it as
		// a wholly separate native object (see spudgpu_create_shader_pipeline
		// in spudgpumetalshader.m), bound alongside the render pipeline
		// state rather than as part of binding it.
		if (cmd_list_metal->_encoder_depth_stencil_state_mtl != pipeline_metal->_depth_stencil_state_mtl) {
			[cmd_list_metal->_active_render_encoder setDepthStencilState:pipeline_metal->_depth_stencil_state_mtl];
			cmd_list_metal->_encoder_depth_stencil_state_mtl = pipeline_metal->_depth_stencil_state_mtl;
		}
		// Likewise the rasterizer state Vulkan/D3D12 bake into the pipeline
		// (spudgpu_shader_pipeline_desc::cull_mode, front_face_ccw,
		// wireframe): Metal has it on the encoder, and an encoder starts
		// with no culling, clockwise fronts and filled triangles whatever
		// the pipeline was created with. So it is set with every bind.
		// Each only when it differs from what the encoder has.
		MTLCullMode cull_mode   = spudgpumetal___internal_cull_mode(pipeline_metal->_desc.cull_mode);
		MTLWinding winding      = pipeline_metal->_desc.front_face_ccw ? MTLWindingCounterClockwise : MTLWindingClockwise;
		MTLTriangleFillMode fill = pipeline_metal->_desc.wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
		bool known              = cmd_list_metal->_encoder_rasterizer_set;
		if (!known || cmd_list_metal->_encoder_cull_mode != cull_mode)
			[cmd_list_metal->_active_render_encoder setCullMode:cull_mode];
		if (!known || cmd_list_metal->_encoder_winding != winding)
			[cmd_list_metal->_active_render_encoder setFrontFacingWinding:winding];
		if (!known || cmd_list_metal->_encoder_fill_mode != fill)
			[cmd_list_metal->_active_render_encoder setTriangleFillMode:fill];
		// The depth bias too, so a pipeline without one doesn't keep the
		// last pipeline's.
		float depth_bias[3] = {(float)pipeline_metal->_desc.depth_bias_constant, pipeline_metal->_desc.depth_bias_slope,
		                       pipeline_metal->_desc.depth_bias_clamp};
		if (!known || memcmp(cmd_list_metal->_encoder_depth_bias, depth_bias, sizeof(depth_bias)) != 0)
			[cmd_list_metal->_active_render_encoder setDepthBias:depth_bias[0] slopeScale:depth_bias[1] clamp:depth_bias[2]];
		cmd_list_metal->_encoder_rasterizer_set = true;
#if SPUDGPU_EXT_DEPTH_BOUNDS_TEST
		// No pipeline switch for the depth bounds test on Metal: a pipeline
		// without it gets the whole range, so the last one's bounds don't
		// clip its draws. One with it gets its range from
		// spudgpu_cmd_set_depth_bounds after this bind (spudgpu.h).
		if (!pipeline_metal->_desc.depth_bounds_test_enable)
			spudgpumetal___internal_set_encoder_depth_bounds(cmd_list_metal, 0.0f, 1.0f);
#endif
		cmd_list_metal->_encoder_cull_mode      = cull_mode;
		cmd_list_metal->_encoder_winding        = winding;
		cmd_list_metal->_encoder_fill_mode      = fill;
		memcpy(cmd_list_metal->_encoder_depth_bias, depth_bias, sizeof(depth_bias));
	}
}

void spudgpu_cmd_bind_compute_pipeline(
    spudgpu_command_list cmd,
    spudgpu_compute_pipeline pipeline) {
	if (!cmd || !pipeline)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_bind_compute_pipeline", "cmd"))
		return;
	if (SPUD_STALE(pipeline, "spudgpu_cmd_bind_compute_pipeline", "pipeline"))
		return;

	spudgpu_command_list_metal *cmd_list_metal      = (spudgpu_command_list_metal *)cmd;
	spudgpu_compute_pipeline_metal *pipeline_metal  = (spudgpu_compute_pipeline_metal *)pipeline;
	cmd_list_metal->_bound_compute_pipeline          = (struct spudgpu_compute_pipeline_t *)pipeline_metal;

	id<MTLComputeCommandEncoder> encoder = spudgpumetal___internal_ensure_compute_encoder(cmd_list_metal);
	// Binding the pipeline already bound is allowed, and leaves the encoder
	// as it is: Metal's validation refuses setting the same state twice.
	if (cmd_list_metal->_encoder_compute_pipeline_state == pipeline_metal->_compute_pipeline_state_mtl)
		return;
	[encoder setComputePipelineState:pipeline_metal->_compute_pipeline_state_mtl];
	cmd_list_metal->_encoder_compute_pipeline_state = pipeline_metal->_compute_pipeline_state_mtl;
}

void spudgpu_cmd_dispatch(
    spudgpu_command_list cmd,
    uint32_t group_count_x,
    uint32_t group_count_y,
    uint32_t group_count_z) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_dispatch", "cmd"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	spudgpu_compute_pipeline_metal *pipeline_metal =
	    (spudgpu_compute_pipeline_metal *)cmd_list_metal->_bound_compute_pipeline;
	if (!cmd_list_metal->_active_compute_encoder || !pipeline_metal)
		return;

	// See spudgpu_shader_module_metal::_local_size_x/y/z in spudgpumetal.h -
	// Metal has no way to read this back out of the bound pipeline the way
	// vkCmdDispatch/Dispatch do, so it travels with the pipeline instead.
	// Clamped to 1 in case reflection ever comes back empty (e.g. a compute
	// shader that never declared a local_size layout) - 0 in any dimension
	// is a guaranteed Metal validation failure.
	MTLSize threads_per_threadgroup = MTLSizeMake(
	    pipeline_metal->_local_size_x ? pipeline_metal->_local_size_x : 1,
	    pipeline_metal->_local_size_y ? pipeline_metal->_local_size_y : 1,
	    pipeline_metal->_local_size_z ? pipeline_metal->_local_size_z : 1);
	MTLSize threadgroups_per_grid = MTLSizeMake(group_count_x, group_count_y, group_count_z);

	[cmd_list_metal->_active_compute_encoder dispatchThreadgroups:threadgroups_per_grid
	                                          threadsPerThreadgroup:threads_per_threadgroup];
}

#if SPUDGPU_EXT_MESH_SHADING
void spudgpu_cmd_dispatch_mesh(
    spudgpu_command_list cmd,
    uint32_t group_count_x,
    uint32_t group_count_y,
    uint32_t group_count_z) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_dispatch_mesh", "cmd"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	spudgpu_shader_pipeline_metal *pipeline_metal =
	    (spudgpu_shader_pipeline_metal *)cmd_list_metal->_bound_pipeline;
	if (!cmd_list_metal->_active_render_encoder || !pipeline_metal || !pipeline_metal->_is_mesh_pipeline)
		return;
	spudgpumetal___internal_send_push_constants(cmd_list_metal);

	// See spudgpu_shader_pipeline_metal::_mesh_local_size_x/y/z in
	// spudgpumetal.h - same "no equivalent of reading this out of the bound
	// pipeline at dispatch time" reasoning as spudgpu_cmd_dispatch above.
	MTLSize threads_per_mesh_threadgroup = MTLSizeMake(
	    pipeline_metal->_mesh_local_size_x ? pipeline_metal->_mesh_local_size_x : 1,
	    pipeline_metal->_mesh_local_size_y ? pipeline_metal->_mesh_local_size_y : 1,
	    pipeline_metal->_mesh_local_size_z ? pipeline_metal->_mesh_local_size_z : 1);
	MTLSize threadgroups_per_grid = MTLSizeMake(group_count_x, group_count_y, group_count_z);

	// No object/task shader (spudgpu_shader_pipeline_desc::task_module isn't
	// wired to a real pipeline path yet - see spudgpu.h) - Metal's own
	// threadsPerObjectThreadgroup argument is documented as ignored when the
	// pipeline has no object function, so {1,1,1} is a safe placeholder.
	[cmd_list_metal->_active_render_encoder drawMeshThreadgroups:threadgroups_per_grid
	                                   threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
	                                     threadsPerMeshThreadgroup:threads_per_mesh_threadgroup];
}
#endif // SPUDGPU_EXT_MESH_SHADING

#if SPUDGPU_EXT_DEPTH_BOUNDS_TEST
void spudgpu_cmd_set_depth_bounds(
    spudgpu_command_list cmd,
    float min_depth_bounds,
    float max_depth_bounds) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_set_depth_bounds", "cmd"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (!cmd_list_metal->_active_render_encoder)
		return;

	// Without the feature, setDepthTestMinBound:maxBound: may be invalid on
	// this GPU (MTLGPUFamilyApple10 only).
	const bool enabled = (cmd_list_metal->_parent_allocator->_parent_device->_features & SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST) != 0;
	assert(enabled && "spudgpu_cmd_set_depth_bounds: the device was created without SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST");
	if (!enabled)
		return;

	// A pipeline created without the test isn't tested (spudgpu.h).
	spudgpu_shader_pipeline_metal *pipeline_metal = (spudgpu_shader_pipeline_metal *)cmd_list_metal->_bound_pipeline;
	if (!pipeline_metal || !pipeline_metal->_desc.depth_bounds_test_enable)
		return;
	spudgpumetal___internal_set_encoder_depth_bounds(cmd_list_metal, min_depth_bounds, max_depth_bounds);
}
#endif // SPUDGPU_EXT_DEPTH_BOUNDS_TEST

void spudgpu_cmd_draw_indirect(
    spudgpu_command_list cmd,
    spudgpu_buffer buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride) {
	if (!cmd || !buffer || draw_count == 0)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_draw_indirect", "cmd"))
		return;
	if (SPUD_STALE(buffer, "spudgpu_cmd_draw_indirect", "buffer"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (draw_count > spudgpumetal___max_draw_indirect_count(cmd_list_metal->_parent_allocator->_parent_device))
		return;
	if (stride % 4 != 0 || stride < sizeof(spudgpu_draw_indirect_args))
		return;
	if (!cmd_list_metal->_active_render_encoder)
		return;
	spudgpumetal___internal_send_push_constants(cmd_list_metal);
	spudgpu_shader_pipeline_metal *pipeline_metal = (spudgpu_shader_pipeline_metal *)cmd_list_metal->_bound_pipeline;
	if (!pipeline_metal)
		return;
	spudgpu_buffer_metal *buffer_metal = (spudgpu_buffer_metal *)buffer;

	MTLPrimitiveType primitive_type =
	    spudgpumetal___internal_primitive_type(pipeline_metal->_desc.primitive_topology);

	// -drawPrimitives:indirectBuffer:indirectBufferOffset: issues exactly one
	// draw per call, reading its args from one offset in the buffer - unlike
	// vkCmdDrawIndirect/ExecuteIndirect, Metal has no native "draw_count
	// consecutive structs, one call" primitive, so this loops internally.
	// Purely a mechanical backend difference; the public call signature is
	// identical across all three backends.
	for (uint32_t i = 0; i < draw_count; i++) {
		[cmd_list_metal->_active_render_encoder
		    drawPrimitives:primitive_type
		    indirectBuffer:buffer_metal->_buffer_mtl
		    indirectBufferOffset:offset + (uint64_t)i * stride];
	}
}

void spudgpu_cmd_draw_indexed_indirect(
    spudgpu_command_list cmd,
    spudgpu_buffer buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride) {
	if (!cmd || !buffer || draw_count == 0)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_draw_indexed_indirect", "cmd"))
		return;
	if (SPUD_STALE(buffer, "spudgpu_cmd_draw_indexed_indirect", "buffer"))
		return;

	spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)cmd;
	if (draw_count > spudgpumetal___max_draw_indirect_count(cmd_list_metal->_parent_allocator->_parent_device))
		return;
	if (stride % 4 != 0 || stride < sizeof(spudgpu_draw_indexed_indirect_args))
		return;
	if (!cmd_list_metal->_active_render_encoder)
		return;
	spudgpumetal___internal_send_push_constants(cmd_list_metal);
	spudgpu_shader_pipeline_metal *pipeline_metal = (spudgpu_shader_pipeline_metal *)cmd_list_metal->_bound_pipeline;
	spudgpu_buffer_view_metal *index_view       = cmd_list_metal->_bound_index_buffer_view;
	if (!pipeline_metal || !index_view)
		return;
	spudgpu_buffer_metal *index_buffer_metal = (spudgpu_buffer_metal *)index_view->_desc.parent_buffer;
	if (!index_buffer_metal)
		return;
	spudgpu_buffer_metal *buffer_metal = (spudgpu_buffer_metal *)buffer;

	MTLIndexType index_type;
	if (!spudgpumetal___internal_resolve_index_type(index_view->_desc.stride, &index_type))
		return;

	MTLPrimitiveType primitive_type =
	    spudgpumetal___internal_primitive_type(pipeline_metal->_desc.primitive_topology);

	for (uint32_t i = 0; i < draw_count; i++) {
		[cmd_list_metal->_active_render_encoder
		    drawIndexedPrimitives:primitive_type
		               indexType:index_type
		             indexBuffer:index_buffer_metal->_buffer_mtl
		       indexBufferOffset:(NSUInteger)index_view->_desc.offset_from_parent_buffer
		          indirectBuffer:buffer_metal->_buffer_mtl
		    indirectBufferOffset:offset + (uint64_t)i * stride];
	}
}

void spudgpu_cmd_push_constants(
    spudgpu_command_list cmd,
    spudgpu_shader_pipeline pipeline,
    uint32_t offset,
    uint32_t size,
    const void *data) {
	if (!cmd || !pipeline || !data || size == 0)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_push_constants", "cmd"))
		return;
	if (SPUD_STALE(pipeline, "spudgpu_cmd_push_constants", "pipeline"))
		return;
	if (offset + size > SPUDGPU_METAL_PUSH_CONSTANTS_SIZE)
		return;

	spudgpu_command_list_metal *cmd_list_metal    = (spudgpu_command_list_metal *)cmd;
	spudgpu_shader_pipeline_metal *pipeline_metal = (spudgpu_shader_pipeline_metal *)pipeline;

	// Metal has no persistent push-constant-block state to patch a
	// sub-range of - see SPUDGPU_METAL_PUSH_CONSTANTS_SIZE - so this always
	// writes into the command list's own scratch copy of the whole block,
	// which the next draw sends.
	memcpy(cmd_list_metal->_push_constants_scratch + offset, data, size);
	uint32_t new_extent = offset + size;
	if (new_extent > cmd_list_metal->_push_constants_extent)
		cmd_list_metal->_push_constants_extent = new_extent;

	// Metal has no per-call stage selection the way Vulkan's stage_flags
	// does - -setVertexBytes:/-setFragmentBytes: are separate calls - so
	// collect which stages any range overlapping [offset, offset+size)
	// actually declares (same overlap test the Vulkan backend uses); the
	// next draw sends the block to each of those.
	uint32_t range_count = pipeline_metal->_desc.push_constant_range_count;
	for (uint32_t i = 0; i < range_count; i++) {
		const spudgpu_push_constant_range_desc *r = &pipeline_metal->_desc.push_constant_ranges[i];
		if (r->offset < offset + size && r->offset + r->size > offset)
			cmd_list_metal->_push_constants_pending_stages |=
			    r->stage_flags & (SPUDGPU_SHADER_STAGE_VERTEX | SPUDGPU_SHADER_STAGE_FRAGMENT | SPUDGPU_SHADER_STAGE_MESH);
	}
}

void spudgpu_cmd_push_constants_compute(
    spudgpu_command_list cmd,
    spudgpu_compute_pipeline pipeline,
    uint32_t offset,
    uint32_t size,
    const void *data) {
	if (!cmd || !pipeline || !data || size == 0)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_push_constants_compute", "cmd"))
		return;
	if (SPUD_STALE(pipeline, "spudgpu_cmd_push_constants_compute", "pipeline"))
		return;

	spudgpu_command_list_metal *cmd_list_metal     = (spudgpu_command_list_metal *)cmd;
	spudgpu_compute_pipeline_metal *pipeline_metal = (spudgpu_compute_pipeline_metal *)pipeline;
	if (!pipeline_metal->_desc.push_constant_range_count)
		return;
	if (offset + size > SPUDGPU_METAL_PUSH_CONSTANTS_SIZE)
		return;

	// As spudgpu_cmd_push_constants: a sub-range can't be patched in place,
	// so the write goes into the command list's scratch copy of the compute
	// block and the whole of it is sent again.
	memcpy(cmd_list_metal->_compute_push_constants_scratch + offset, data, size);
	uint32_t new_extent = offset + size;
	if (new_extent > cmd_list_metal->_compute_push_constants_extent)
		cmd_list_metal->_compute_push_constants_extent = new_extent;

	// The header's rule is to push after spudgpu_cmd_bind_compute_pipeline,
	// which is what opens the compute encoder. Without one there is nothing
	// to send to, and opening one here would end a render pass in progress.
	if (!cmd_list_metal->_active_compute_encoder)
		return;

	// The compute shader's push constant block is cross-compiled to the same
	// fixed buffer index as a graphics stage's (spudgpumetalshader.m).
	[cmd_list_metal->_active_compute_encoder setBytes:cmd_list_metal->_compute_push_constants_scratch
	                                           length:cmd_list_metal->_compute_push_constants_extent
	                                          atIndex:SPUDGPU_METAL_PUSH_CONSTANTS_BUFFER_INDEX];
}

SPUDRESULT spudgpu_queue_submit(
    spudgpu_command_queue queue,
    const spudgpu_submit_desc *desc) {
	if (!queue)
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	if (SPUD_STALE(queue, "spudgpu_queue_submit", "queue"))
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_SUBMIT_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!desc->cmd_lists)
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	if (desc->cmd_list_count == 0)
		return SPUDRESULT_ZERO_SIZE;

	// With a swap chain, its own events join the caller's: a wait on
	// image_available, and signals of render_finished and of its in-flight
	// fence, each of which counts its own submissions.
	spudgpu_swap_chain_metal *swap_chain_metal = (spudgpu_swap_chain_metal *)desc->swap_chain;
	spudgpu_semaphore_metal *image_available   = swap_chain_metal ? swap_chain_metal->_image_available_semaphore : NULL;
	spudgpu_semaphore_metal *render_finished   = swap_chain_metal ? swap_chain_metal->_render_finished_semaphore : NULL;
	spudgpu_fence_metal *in_flight             = swap_chain_metal ? swap_chain_metal->_in_flight_fence : NULL;

	// Semaphores are plain id<MTLEvent> timelines and a fence is an
	// id<MTLSharedEvent> (see spudgpumetal.h) - not Vulkan-style binary
	// semaphores. wait_stage_masks is accepted for API symmetry with
	// Vulkan/D3D12 and unused - Metal has no separate wait-stage-mask
	// concept, only a value to wait for. There is no single batch submit
	// call spanning multiple command buffers on Metal, so the waits are
	// encoded on the first buffer (nothing in the batch starts before
	// they're satisfied) and the signals on the last (they fire only once
	// every buffer in the batch has run, since a queue executes committed
	// buffers in commit order) to match Vulkan/D3D12's single-submit
	// semantics.
	//
	// The batch's commits, and the semaphore counts it bumps, are one use of
	// the queue (spudgpu.h).
	spudgpumetal___queue_enter((spudgpu_command_queue_metal *)queue);
	for (uint32_t i = 0; i < desc->cmd_list_count; i++) {
		spudgpu_command_list_metal *cmd_list_metal = (spudgpu_command_list_metal *)desc->cmd_lists[i];
		if (!cmd_list_metal || !cmd_list_metal->_command_buffer_mtl)
			continue;
		id<MTLCommandBuffer> cmd_buffer = cmd_list_metal->_command_buffer_mtl;

		if (i == 0) {
			for (uint32_t w = 0; w < desc->wait_semaphore_count; w++) {
				spudgpu_semaphore_metal *sem = (spudgpu_semaphore_metal *)desc->wait_semaphores[w];
				if (sem && sem->_event_mtl) {
					[cmd_buffer encodeWaitForEvent:sem->_event_mtl value:sem->_signal_value];
				}
			}
			if (image_available && image_available->_event_mtl) {
				[cmd_buffer encodeWaitForEvent:image_available->_event_mtl
				                          value:image_available->_signal_value];
			}
		}
		if (i == desc->cmd_list_count - 1) {
			for (uint32_t s = 0; s < desc->signal_semaphore_count; s++) {
				spudgpu_semaphore_metal *sem = (spudgpu_semaphore_metal *)desc->signal_semaphores[s];
				if (sem && sem->_event_mtl) {
					sem->_signal_value += 1;
					[cmd_buffer encodeSignalEvent:sem->_event_mtl value:sem->_signal_value];
				}
			}
			if (render_finished && render_finished->_event_mtl) {
				render_finished->_signal_value += 1;
				[cmd_buffer encodeSignalEvent:render_finished->_event_mtl
				                         value:render_finished->_signal_value];
			}
			if (in_flight && in_flight->_shared_event_mtl) {
				in_flight->_signal_value += 1;
				[cmd_buffer encodeSignalEvent:in_flight->_shared_event_mtl
				                         value:in_flight->_signal_value];
			}
			if (desc->signal_fence) {
				// The caller's fence, signaled to the caller's value.
				spudgpu_fence_metal *fence = (spudgpu_fence_metal *)desc->signal_fence;
				[cmd_buffer encodeSignalEvent:fence->_shared_event_mtl value:desc->signal_fence_value];
			}
		}

		[cmd_buffer commit];
	}
	spudgpumetal___queue_leave((spudgpu_command_queue_metal *)queue);

	return SPUD_SUCCESS;
}

void spudgpu_queue_wait_idle(spudgpu_command_queue queue) {
	if (!queue)
		return;
	if (SPUD_STALE(queue, "spudgpu_queue_wait_idle", "queue"))
		return;

	spudgpu_command_queue_metal *queue_metal = (spudgpu_command_queue_metal *)queue;

	// Metal has no direct wait-idle call on MTLCommandQueue (unlike
	// vkQueueWaitIdle / a fence-wait loop on ID3D12CommandQueue) - commit an
	// empty buffer and block on it. Buffers committed to one queue execute in
	// commit order, so this buffer's completion implies every buffer
	// committed before it on this queue has also completed.
	spudgpumetal___queue_enter(queue_metal);
	id<MTLCommandBuffer> idle_buffer = [queue_metal->_command_queue_mtl commandBuffer];
	[idle_buffer commit];
	[idle_buffer waitUntilCompleted];
	spudgpumetal___queue_leave(queue_metal);
}

#endif // SPUDGPU_COMPILE_METAL_API
