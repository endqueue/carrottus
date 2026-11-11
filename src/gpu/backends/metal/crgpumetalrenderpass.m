//
// Carrottus GPU Metal backend - render pass / render pipeline descriptors.
// crgpu_command_list_begin_rendering/end_rendering and the buffer/image copy
// functions below (via MTLBlitCommandEncoder) are implemented against real
// Apple hardware. crgpu_command_list_blit_image implements only a blit that is a
// copy (same size, orientation and pixel format); a scaling, flipping or
// format-converting one is reported and records nothing.
//

#if CRGPU_COMPILE_METAL_API

#include "crgpu.h"
#include "crcore.h"
#include "crgpumetal.h"
#include "../../crgpushared.h"
#include <Metal/Metal.h>
#include <assert.h>
#include <stdio.h>
#include "../../../crstructbase.h"

// CRGPU_IMAGE_LAYOUT is a Vulkan/D3D12-shaped state machine (UNDEFINED,
// COLOR_ATTACHMENT_OPTIMAL, SHADER_READ_ONLY, ...) that Metal has no
// equivalent to at all - a texture is usable as a render target,
// shader-read resource, or blit source/destination interchangeably with no
// transition call, ever. The three barrier functions below are correctly
// no-ops for any recognized layout value; this only rejects (silently, with
// a diagnostic) a value outside the defined enum range, which is a caller
// bug (garbage/uninitialized input) on every backend, not a Metal
// limitation - there is nothing in the recognized range Metal "doesn't
// support," since none of it does anything here regardless.
static bool crgpumetal___internal_layout_recognized(CRGPU_IMAGE_LAYOUT layout) {
	return layout <= CRGPU_IMAGE_LAYOUT_PRESENT_SRC;
}

void crgpu_command_list_image_barrier(
    crgpu_command_list cmd,
    crgpu_image image,
    CRGPU_IMAGE_LAYOUT old_layout,
    CRGPU_IMAGE_LAYOUT new_layout) {
	if (!cmd || !image)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_image_barrier", "cmd"))
		return;
	if (CR_STALE(image, "crgpu_command_list_image_barrier", "image"))
		return;
	if (!crgpumetal___internal_layout_recognized(old_layout) ||
	    !crgpumetal___internal_layout_recognized(new_layout)) {
		printf("crgpu: crgpu_command_list_image_barrier - unrecognized CRGPU_IMAGE_LAYOUT value\n");
		return;
	}

	// No-op - see the note above crgpumetal___internal_layout_recognized.
}

void crgpu_command_list_image_barrier_view(
    crgpu_command_list cmd,
    crgpu_image_view image_view,
    CRGPU_IMAGE_LAYOUT old_layout,
    CRGPU_IMAGE_LAYOUT new_layout) {
	if (!cmd || !image_view)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_image_barrier_view", "cmd"))
		return;
	if (CR_STALE(image_view, "crgpu_command_list_image_barrier_view", "image_view"))
		return;
	if (!crgpumetal___internal_layout_recognized(old_layout) ||
	    !crgpumetal___internal_layout_recognized(new_layout)) {
		printf("crgpu: crgpu_command_list_image_barrier_view - unrecognized CRGPU_IMAGE_LAYOUT value\n");
		return;
	}

	// No-op - see the note above crgpumetal___internal_layout_recognized.
}

void crgpu_command_list_image_barrier_subresource(
    crgpu_command_list cmd,
    crgpu_image image,
    const crgpu_image_view_desc_subresource_range *range,
    CRGPU_IMAGE_LAYOUT old_layout,
    CRGPU_IMAGE_LAYOUT new_layout) {
	if (!cmd || !image || !range)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_image_barrier_subresource", "cmd"))
		return;
	if (CR_STALE(image, "crgpu_command_list_image_barrier_subresource", "image"))
		return;
	if (!crgpumetal___internal_layout_recognized(old_layout) ||
	    !crgpumetal___internal_layout_recognized(new_layout)) {
		printf("crgpu: crgpu_command_list_image_barrier_subresource - unrecognized CRGPU_IMAGE_LAYOUT value\n");
		return;
	}

	// No-op - see the note above crgpumetal___internal_layout_recognized.
}

// Ends and releases _active_blit_encoder if one is active. Shared with
// crgpu_command_list_begin_rendering below and
// crgpumetal___internal_ensure_compute_encoder (crgpumetalcommand.m) -
// see this function's declaration in crgpumetal.h.
void crgpumetal___internal_end_active_blit_encoder(crgpu_command_list_metal *cmd_list_metal) {
	if (!cmd_list_metal || !cmd_list_metal->_active_blit_encoder)
		return;
	[cmd_list_metal->_active_blit_encoder endEncoding];
	[cmd_list_metal->_active_blit_encoder release];
	cmd_list_metal->_active_blit_encoder = nil;
}

// Returns the active blit encoder, lazily opening one (ending any active
// render/compute encoder first - Metal disallows two live encoders of
// different kinds on one command buffer at once, see _active_blit_encoder's
// comment in crgpumetal.h) if none exists yet.
static id<MTLBlitCommandEncoder> crgpumetal___internal_ensure_blit_encoder(
    crgpu_command_list_metal *cmd_list_metal) {
	if (cmd_list_metal->_active_blit_encoder)
		return cmd_list_metal->_active_blit_encoder;

	if (cmd_list_metal->_active_render_encoder) {
		[cmd_list_metal->_active_render_encoder endEncoding];
		[cmd_list_metal->_active_render_encoder release];
		cmd_list_metal->_active_render_encoder = nil;
	}
	crgpumetal___internal_end_active_compute_encoder(cmd_list_metal);

	id<MTLBlitCommandEncoder> encoder = [cmd_list_metal->_command_buffer_mtl blitCommandEncoder];
	cmd_list_metal->_active_blit_encoder = [encoder retain];
	return cmd_list_metal->_active_blit_encoder;
}

void crgpu_command_list_copy_buffer(
    crgpu_command_list cmd,
    crgpu_buffer src_buffer,
    crgpu_buffer dst_buffer,
    uint64_t src_offset,
    uint64_t dst_offset,
    uint64_t size) {
	if (!cmd || !src_buffer || !dst_buffer)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_copy_buffer", "cmd"))
		return;
	if (CR_STALE(src_buffer, "crgpu_command_list_copy_buffer", "src_buffer"))
		return;
	if (CR_STALE(dst_buffer, "crgpu_command_list_copy_buffer", "dst_buffer"))
		return;

	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	crgpu_buffer_metal *src_metal       = (crgpu_buffer_metal *)src_buffer;
	crgpu_buffer_metal *dst_metal       = (crgpu_buffer_metal *)dst_buffer;

	id<MTLBlitCommandEncoder> encoder = crgpumetal___internal_ensure_blit_encoder(cmd_metal);
	[encoder copyFromBuffer:src_metal->_buffer_mtl
	            sourceOffset:src_offset
	                toBuffer:dst_metal->_buffer_mtl
	       destinationOffset:dst_offset
	                    size:size];
}

void crgpu_command_list_copy_image_to_buffer(
    crgpu_command_list cmd,
    crgpu_image src_image,
    crgpu_buffer dst_buffer,
    const crgpu_image_buffer_copy_desc *desc) {
	if (!cmd || !src_image || !dst_buffer || !desc)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_copy_image_to_buffer", "cmd"))
		return;
	if (CR_STALE(src_image, "crgpu_command_list_copy_image_to_buffer", "src_image"))
		return;
	if (CR_STALE(dst_buffer, "crgpu_command_list_copy_image_to_buffer", "dst_buffer"))
		return;

	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	crgpu_image_metal *src_metal        = (crgpu_image_metal *)src_image;
	crgpu_buffer_metal *dst_metal       = (crgpu_buffer_metal *)dst_buffer;

	uint64_t bytes_per_row = desc->buffer_row_length
	                             ? (uint64_t) desc->buffer_row_length * (crgpu_format_get_bit_count(src_metal->_desc.format) / 8)
	                             : (uint64_t) desc->width * (crgpu_format_get_bit_count(src_metal->_desc.format) / 8);
	uint64_t bytes_per_image = bytes_per_row * (desc->buffer_image_height ? desc->buffer_image_height : desc->height);

	id<MTLBlitCommandEncoder> encoder = crgpumetal___internal_ensure_blit_encoder(cmd_metal);
	for (uint32_t layer = 0; layer < desc->array_layer_count; layer++) {
		[encoder copyFromTexture:src_metal->_texture_mtl
		                  sourceSlice:desc->base_array_layer + layer
		                  sourceLevel:desc->mip_level
		                 sourceOrigin:MTLOriginMake(desc->image_x, desc->image_y, desc->image_z)
		                   sourceSize:MTLSizeMake(desc->width, desc->height, desc->depth)
		                     toBuffer:dst_metal->_buffer_mtl
		            destinationOffset:desc->buffer_offset + layer * bytes_per_image
		       destinationBytesPerRow:bytes_per_row
		     destinationBytesPerImage:bytes_per_image];
	}
}

void crgpu_command_list_copy_buffer_to_image(
    crgpu_command_list cmd,
    crgpu_buffer src_buffer,
    crgpu_image dst_image,
    const crgpu_image_buffer_copy_desc *desc) {
	if (!cmd || !src_buffer || !dst_image || !desc)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_copy_buffer_to_image", "cmd"))
		return;
	if (CR_STALE(src_buffer, "crgpu_command_list_copy_buffer_to_image", "src_buffer"))
		return;
	if (CR_STALE(dst_image, "crgpu_command_list_copy_buffer_to_image", "dst_image"))
		return;

	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	crgpu_buffer_metal *src_metal       = (crgpu_buffer_metal *)src_buffer;
	crgpu_image_metal *dst_metal        = (crgpu_image_metal *)dst_image;

	uint64_t bytes_per_row = desc->buffer_row_length
	                             ? (uint64_t) desc->buffer_row_length * (crgpu_format_get_bit_count(dst_metal->_desc.format) / 8)
	                             : (uint64_t) desc->width * (crgpu_format_get_bit_count(dst_metal->_desc.format) / 8);
	uint64_t bytes_per_image = bytes_per_row * (desc->buffer_image_height ? desc->buffer_image_height : desc->height);

	id<MTLBlitCommandEncoder> encoder = crgpumetal___internal_ensure_blit_encoder(cmd_metal);
	for (uint32_t layer = 0; layer < desc->array_layer_count; layer++) {
		[encoder copyFromBuffer:src_metal->_buffer_mtl
		             sourceOffset:desc->buffer_offset + layer * bytes_per_image
		        sourceBytesPerRow:bytes_per_row
		      sourceBytesPerImage:bytes_per_image
		               sourceSize:MTLSizeMake(desc->width, desc->height, desc->depth)
		                toTexture:dst_metal->_texture_mtl
		         destinationSlice:desc->base_array_layer + layer
		         destinationLevel:desc->mip_level
		        destinationOrigin:MTLOriginMake(desc->image_x, desc->image_y, desc->image_z)];
	}
}

// Metal has no row-pitch alignment requirement for buffer<->texture copies
// (unlike D3D12's 256-byte footprint alignment) - this always reports the
// tightly-packed size for the given mip level, matching the Vulkan backend's
// own reasoning in crgpuvulkanrenderpass.c.
void crgpu_image_get_buffer_copy_size(
    crgpu_image image,
    uint32_t mip_level,
    uint64_t *out_row_pitch,
    uint64_t *out_total_size) {
	if (!image)
		return;
	if (CR_STALE(image, "crgpu_image_get_buffer_copy_size", "image"))
		return;

	crgpu_image_metal *image_metal = (crgpu_image_metal *)image;

	uint32_t mip_width  = image_metal->_desc.width >> mip_level;
	uint32_t mip_height = image_metal->_desc.height >> mip_level;
	uint32_t mip_depth  = image_metal->_desc.depth >> mip_level;
	if (!mip_width) mip_width = 1;
	if (!mip_height) mip_height = 1;
	if (!mip_depth) mip_depth = 1;

	uint64_t row_pitch = (uint64_t) mip_width * (crgpu_format_get_bit_count(image_metal->_desc.format) / 8);

	if (out_row_pitch)
		*out_row_pitch = row_pitch;
	if (out_total_size)
		*out_total_size = row_pitch * mip_height * mip_depth;
}

CR_RESULT crgpu_image_get_buffer_copy_offset_alignment(
    crgpu_image image,
    uint64_t *out_alignment) {
	if (!image)
		return CR_RESULT_GPU_INVALID_IMAGE;
	if (CR_STALE(image, "crgpu_image_get_buffer_copy_offset_alignment", "image"))
		return CR_RESULT_GPU_INVALID_IMAGE;
	if (!out_alignment)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	crgpu_image_metal *image_metal = (crgpu_image_metal *)image;
	const uint64_t block             = crgpu___internal_format_block_bytes(image_metal->_desc.format);
	if (block == 0)
		return CR_RESULT_GPU_INVALID_FORMAT;
	// The blit encoder's sourceOffset/destinationOffset: a multiple of the
	// pixel size, or of the block size for a compressed format.
	*out_alignment = block;
	return CR_SUCCESS;
}

void crgpu_command_list_blit_image(
    crgpu_command_list cmd,
    crgpu_image src_image,
    crgpu_image dst_image,
    const crgpu_image_blit_desc *desc) {
	if (!cmd || !src_image || !dst_image || !desc)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_blit_image", "cmd"))
		return;
	if (CR_STALE(src_image, "crgpu_command_list_blit_image", "src_image"))
		return;
	if (CR_STALE(dst_image, "crgpu_command_list_blit_image", "dst_image"))
		return;

	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	crgpu_image_metal *src_metal        = (crgpu_image_metal *)src_image;
	crgpu_image_metal *dst_metal        = (crgpu_image_metal *)dst_image;
	if (!src_metal->_texture_mtl || !dst_metal->_texture_mtl)
		return;

	// Only a blit that is really a copy is implemented: regions of the same
	// size and orientation, between textures of one pixel format, which
	// MTLBlitCommandEncoder copies directly (desc->filter never applies).
	// Resampling or converting needs the shader-based pass crgpu.h
	// describes for this backend, which doesn't exist yet - such a blit is
	// reported and records nothing.
	bool same_size = desc->src_x1 > desc->src_x0 && desc->src_y1 > desc->src_y0 && desc->src_z1 > desc->src_z0 &&
	                 desc->src_x1 - desc->src_x0 == desc->dst_x1 - desc->dst_x0 &&
	                 desc->src_y1 - desc->src_y0 == desc->dst_y1 - desc->dst_y0 &&
	                 desc->src_z1 - desc->src_z0 == desc->dst_z1 - desc->dst_z0 &&
	                 desc->dst_x1 > desc->dst_x0 && desc->dst_y1 > desc->dst_y0 && desc->dst_z1 > desc->dst_z0 &&
	                 desc->src_array_layer_count == desc->dst_array_layer_count;
	if (!same_size || src_metal->_texture_mtl.pixelFormat != dst_metal->_texture_mtl.pixelFormat) {
		printf("crgpu: crgpu_command_list_blit_image - scaling, flipping and format-converting blits aren't implemented on Metal\n");
		return;
	}

	id<MTLBlitCommandEncoder> encoder = crgpumetal___internal_ensure_blit_encoder(cmd_metal);
	for (uint32_t layer = 0; layer < desc->src_array_layer_count; layer++) {
		[encoder copyFromTexture:src_metal->_texture_mtl
		             sourceSlice:desc->src_base_array_layer + layer
		             sourceLevel:desc->src_mip_level
		            sourceOrigin:MTLOriginMake(desc->src_x0, desc->src_y0, desc->src_z0)
		              sourceSize:MTLSizeMake(desc->src_x1 - desc->src_x0, desc->src_y1 - desc->src_y0, desc->src_z1 - desc->src_z0)
		               toTexture:dst_metal->_texture_mtl
		        destinationSlice:desc->dst_base_array_layer + layer
		        destinationLevel:desc->dst_mip_level
		       destinationOrigin:MTLOriginMake(desc->dst_x0, desc->dst_y0, desc->dst_z0)];
	}
}

static MTLLoadAction crgpumetal___internal_load_action(CRGPU_LOAD_OP op) {
	switch (op) {
	case CRGPU_LOAD_OP_LOAD:  return MTLLoadActionLoad;
	case CRGPU_LOAD_OP_CLEAR: return MTLLoadActionClear;
	default:                    return MTLLoadActionDontCare;
	}
}

static MTLStoreAction crgpumetal___internal_store_action(CRGPU_STORE_OP op) {
	switch (op) {
	case CRGPU_STORE_OP_STORE: return MTLStoreActionStore;
	default:                     return MTLStoreActionDontCare;
	}
}

void crgpu_command_list_begin_rendering(
    crgpu_command_list cmd,
    const crgpu_rendering_begin_desc *desc) {
	if (!cmd || !desc)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_begin_rendering", "cmd"))
		return;
	if (cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC) != CR_SUCCESS)
		return;

	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;

	// Metal disallows two live encoders of different kinds on one command
	// buffer at once - end any compute or blit encoder left active by
	// crgpu_command_list_bind_compute_pipeline/crgpu_command_list_dispatch or
	// crgpu_command_list_copy_buffer/_copy_buffer_to_image/_copy_image_to_buffer
	// before opening the render encoder below (crgpumetalcommand.m).
	crgpumetal___internal_end_active_compute_encoder(cmd_metal);
	crgpumetal___internal_end_active_blit_encoder(cmd_metal);

	// The list's own descriptor, every field this call sets cleared first:
	// an attachment a pass doesn't name has no texture, and no sample buffer
	// is attached unless the pass asks for timestamps.
	MTLRenderPassDescriptor *pass_desc = cmd_metal->_pass_desc_mtl;
	for (uint32_t i = 0; i < CRGPU_MAX_COLOR_ATTACHMENTS; i++)
		pass_desc.colorAttachments[i].texture = nil;
	pass_desc.depthAttachment.texture                  = nil;
	pass_desc.stencilAttachment.texture                = nil;
	pass_desc.sampleBufferAttachments[0].sampleBuffer = nil;

	uint32_t color_count = desc->color_attachment_count;
	if (color_count > CRGPU_MAX_COLOR_ATTACHMENTS)
		color_count = CRGPU_MAX_COLOR_ATTACHMENTS;

	for (uint32_t i = 0; i < color_count; i++) {
		const crgpu_color_attachment_desc *src = &desc->color_attachments[i];
		crgpu_image_view_metal *view           = (crgpu_image_view_metal *)src->image_view;
		if (!view || !view->_texture_view_mtl)
			continue;

		MTLRenderPassColorAttachmentDescriptor *attach = pass_desc.colorAttachments[i];
		attach.texture                                 = view->_texture_view_mtl;
		attach.loadAction                              = crgpumetal___internal_load_action(src->load_op);
		attach.storeAction                             = crgpumetal___internal_store_action(src->store_op);
		attach.clearColor = MTLClearColorMake(src->clear_color[0], src->clear_color[1], src->clear_color[2], src->clear_color[3]);
	}

	if (desc->depth_attachment.image_view) {
		crgpu_image_view_metal *view = (crgpu_image_view_metal *)desc->depth_attachment.image_view;
		if (view->_texture_view_mtl) {
			pass_desc.depthAttachment.texture     = view->_texture_view_mtl;
			pass_desc.depthAttachment.loadAction  = crgpumetal___internal_load_action(desc->depth_attachment.depth_load_op);
			pass_desc.depthAttachment.storeAction = crgpumetal___internal_store_action(desc->depth_attachment.depth_store_op);
			pass_desc.depthAttachment.clearDepth  = desc->depth_attachment.clear_depth;

			// A view of a depth and stencil texture in its own format is the
			// stencil attachment too: the format table's stencil aspect
			// (Depth32Float_Stencil8, the only such format it has). A view of
			// the stencil alone is in another format and isn't.
			const crgpumetal_format_info *depth_info = crgpumetal___format_info(view->_parent_image->_desc.format);
			if ((depth_info->aspects & CRGPU_IMAGE_ASPECT_STENCIL) && view->_texture_view_mtl.pixelFormat == depth_info->pixel) {
				pass_desc.stencilAttachment.texture      = view->_texture_view_mtl;
				pass_desc.stencilAttachment.loadAction   = crgpumetal___internal_load_action(desc->depth_attachment.stencil_load_op);
				pass_desc.stencilAttachment.storeAction  = crgpumetal___internal_store_action(desc->depth_attachment.stencil_store_op);
				pass_desc.stencilAttachment.clearStencil = desc->depth_attachment.clear_stencil;
			}
		}
	}

	pass_desc.renderTargetWidth  = desc->width;
	pass_desc.renderTargetHeight = desc->height;
	// 0 is Metal's own value for a pass that is not layered. A layered pass
	// needs a device created with CRGPU_FEATURE_LAYERED_RENDERING, on every
	// backend (crgpu.h, crgpu_rendering_begin_desc::layer_count).
	assert((desc->layer_count == 0 ||
	        (cmd_metal->_parent_allocator->_parent_device->_features & CRGPU_FEATURE_LAYERED_RENDERING)) &&
	       "crgpu_command_list_begin_rendering: a layered pass on a device created without CRGPU_FEATURE_LAYERED_RENDERING");
	pass_desc.renderTargetArrayLength = desc->layer_count;

	// The pass's start and end are the boundaries of its stages: the
	// vertex stage's start and the fragment stage's end, which Apple GPUs
	// sample and nothing in between (crgpu.h, "Timestamps").
	if (desc->timestamp_set) {
		MTLRenderPassSampleBufferAttachmentDescriptor *sample = pass_desc.sampleBufferAttachments[0];
		sample.sampleBuffer               = desc->timestamp_set->_sample_buffer_mtl;
		sample.startOfVertexSampleIndex   = desc->timestamp_begin_index;
		sample.endOfVertexSampleIndex     = MTLCounterDontSample;
		sample.startOfFragmentSampleIndex = MTLCounterDontSample;
		sample.endOfFragmentSampleIndex   = desc->timestamp_end_index;
	}

	// -renderCommandEncoderWithDescriptor: returns an autoreleased encoder
	// (same as -nextDrawable in crgpumetalswapchain.m) - retain it so it
	// survives past this call, to be released in crgpu_command_list_end_rendering.
	id<MTLRenderCommandEncoder> encoder = [cmd_metal->_command_buffer_mtl renderCommandEncoderWithDescriptor:pass_desc];
	cmd_metal->_active_render_encoder   = [encoder retain];
	// The encoder has what it needs; the kept descriptor holds no texture
	// past the pass.
	for (uint32_t i = 0; i < color_count; i++)
		pass_desc.colorAttachments[i].texture = nil;
	pass_desc.depthAttachment.texture                  = nil;
	pass_desc.stencilAttachment.texture                = nil;
	pass_desc.sampleBufferAttachments[0].sampleBuffer = nil;
	cmd_metal->_encoder_pipeline_state_mtl      = nil;
	cmd_metal->_encoder_depth_stencil_state_mtl = nil;
	cmd_metal->_encoder_rasterizer_set          = false;
	cmd_metal->_encoder_depth_bounds[0]         = 0.0f;
	cmd_metal->_encoder_depth_bounds[1]         = 1.0f;
	cmd_metal->_encoder_viewport_count          = 0;
	cmd_metal->_encoder_scissor_rect_count      = 0;
	memset(cmd_metal->_encoder_vertex_buffers, 0, sizeof(cmd_metal->_encoder_vertex_buffers));
	memset(cmd_metal->_encoder_vertex_sets, 0, sizeof(cmd_metal->_encoder_vertex_sets));
	memset(cmd_metal->_encoder_mesh_sets, 0, sizeof(cmd_metal->_encoder_mesh_sets));
	memset(cmd_metal->_encoder_fragment_sets, 0, sizeof(cmd_metal->_encoder_fragment_sets));
}

void crgpu_command_list_end_rendering(crgpu_command_list cmd) {
	if (!cmd)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_end_rendering", "cmd"))
		return;

	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	if (!cmd_metal->_active_render_encoder)
		return;

	[cmd_metal->_active_render_encoder endEncoding];
	[cmd_metal->_active_render_encoder release];
	cmd_metal->_active_render_encoder = nil;
}

// crgpu_command_list_clear_color_attachment / crgpu_command_list_clear_depth_attachment —
// Metal's native idiom for clearing an attachment already *is*
// MTLRenderPassDescriptor.colorAttachments[i].loadAction = .clear, which is
// exactly what begin/end-with-LOAD_OP_CLEAR expresses; there is no separate
// Metal clear command to call instead the way D3D12/Vulkan have. This is
// pure composition of the two functions above (no new Metal API surface),
// so it's correct today even though begin/end_rendering are still stubs —
// nothing here needs revisiting once those are implemented for real.
void crgpu_command_list_clear_color_attachment(
    crgpu_command_list cmd,
    crgpu_image_view attachment,
    float r,
    float g,
    float b,
    float a,
    uint32_t width,
    uint32_t height) {
	if (!cmd || !attachment)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_clear_color_attachment", "cmd"))
		return;
	if (CR_STALE(attachment, "crgpu_command_list_clear_color_attachment", "attachment"))
		return;

	crgpu_color_attachment_desc color = {0};
	color.image_view     = attachment;
	color.load_op        = CRGPU_LOAD_OP_CLEAR;
	color.store_op       = CRGPU_STORE_OP_STORE;
	color.clear_color[0] = r;
	color.clear_color[1] = g;
	color.clear_color[2] = b;
	color.clear_color[3] = a;

	crgpu_rendering_begin_desc desc = {0};
	desc.base.type = CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
	desc.color_attachments[0]   = color;
	desc.color_attachment_count = 1;
	desc.width                  = width;
	desc.height                 = height;

	crgpu_command_list_begin_rendering(cmd, &desc);
	crgpu_command_list_end_rendering(cmd);
}

void crgpu_command_list_clear_depth_attachment(
    crgpu_command_list cmd,
    crgpu_image_view attachment,
    bool clear_depth,
    bool clear_stencil,
    float depth,
    uint32_t stencil,
    uint32_t width,
    uint32_t height) {
	if (!cmd || !attachment)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_clear_depth_attachment", "cmd"))
		return;
	if (CR_STALE(attachment, "crgpu_command_list_clear_depth_attachment", "attachment"))
		return;
	if (!clear_depth && !clear_stencil)
		return;

	crgpu_rendering_begin_desc desc = {0};
	desc.base.type = CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
	desc.depth_attachment.image_view       = attachment;
	desc.depth_attachment.depth_load_op    = clear_depth ? CRGPU_LOAD_OP_CLEAR : CRGPU_LOAD_OP_LOAD;
	desc.depth_attachment.depth_store_op   = CRGPU_STORE_OP_STORE;
	desc.depth_attachment.stencil_load_op  = clear_stencil ? CRGPU_LOAD_OP_CLEAR : CRGPU_LOAD_OP_LOAD;
	desc.depth_attachment.stencil_store_op = CRGPU_STORE_OP_STORE;
	desc.depth_attachment.clear_depth      = depth;
	desc.depth_attachment.clear_stencil    = stencil;
	desc.width  = width;
	desc.height = height;

	crgpu_command_list_begin_rendering(cmd, &desc);
	crgpu_command_list_end_rendering(cmd);
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

	// No-op, deliberately - same underlying reason as the image_barrier
	// functions above (crgpumetal___internal_layout_recognized): resources
	// created against a device (not a heap, which is the only way to opt out
	// via MTLHazardTrackingModeUntracked - see crgpumetalbuffer.m/
	// crgpumetalimage.m, neither of which does) are automatically
	// hazard-tracked by Metal, which inserts the required execution
	// dependency between encoders on the same command buffer that
	// read/write the same resource (Write-After-Write, Write-After-Read,
	// Read-After-Write) with no explicit call needed - this is what makes
	// "compute writes a buffer, then a later render pass on the same command
	// buffer reads it as an indirect-draw argument buffer" (see
	// CrGPUExecuteIndirect) correct with zero code here.
	//
	// This does NOT cover a dependency spanning two different command
	// buffers or queues, where automatic tracking can't reach - that case
	// needs an explicit id<MTLFence> (update on the writer's encoder before
	// it ends, wait on the reader's encoder after it begins - see
	// ../../../CLAUDE.md's note on MTLFence belonging here, not in fence/
	// semaphore). Left unimplemented until a caller actually needs
	// cross-command-buffer buffer/image synchronization; this call's single
	// (cmd, before, after) shape has no active encoder to attach
	// update/waitForFence calls to at the point it's invoked, so it can't be
	// expressed generically for that case without a bigger signature change.
}

#endif // CRGPU_COMPILE_METAL_API
