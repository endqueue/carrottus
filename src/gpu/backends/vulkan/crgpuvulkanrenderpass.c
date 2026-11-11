
#if CRGPU_COMPILE_VULKAN_API

#include "crgpuvulkan.h"
#include "../../crgpushared.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../../crstructbase.h"

// ---------------------------------------------------------------------------
// Internal helpers: map Carrottus GPU layout enum → VkImageLayout
// ---------------------------------------------------------------------------

VkImageLayout cr_layout_to_vk(CRGPU_IMAGE_LAYOUT layout) {
	switch (layout) {
	case CRGPU_IMAGE_LAYOUT_UNDEFINED:
		return VK_IMAGE_LAYOUT_UNDEFINED;
	case CRGPU_IMAGE_LAYOUT_GENERAL:
		return VK_IMAGE_LAYOUT_GENERAL;
	case CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	case CRGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	case CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY:
		return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	case CRGPU_IMAGE_LAYOUT_TRANSFER_SRC:
		return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	case CRGPU_IMAGE_LAYOUT_TRANSFER_DST:
		return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	case CRGPU_IMAGE_LAYOUT_PRESENT_SRC:
		return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	default:
		return VK_IMAGE_LAYOUT_UNDEFINED;
	}
}

// Infer the right access mask and pipeline stage from a layout,
// so callers don't have to spell it out every time.
static VkAccessFlags layout_to_access(VkImageLayout layout) {
	switch (layout) {
	// The storage image layout (crgpu.h): a shader reads and writes it.
	// With no access here a barrier into, out of or within GENERAL would
	// order nothing, and GENERAL to GENERAL is the header's ordering barrier.
	case VK_IMAGE_LAYOUT_GENERAL:
		return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	// An attachment is read as well as written: a load op of LOAD, blending
	// and the depth test all read it, so a barrier into one must make it
	// visible to reads and one out of it must wait for them.
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		return VK_ACCESS_SHADER_READ_BIT;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		return VK_ACCESS_TRANSFER_READ_BIT;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		return VK_ACCESS_TRANSFER_WRITE_BIT;
	case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
		return 0; // presentation engine handles its own sync
	default:
		return 0;
	}
}

static VkPipelineStageFlags layout_to_stage(VkImageLayout layout) {
	switch (layout) {
	// The stages a storage slot can be used from, as
	// crgpu_command_list_pipeline_barrier uses for CRGPU_RESOURCE_STATE_UNORDERED_ACCESS.
	case VK_IMAGE_LAYOUT_GENERAL:
		return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	// Depth and stencil are tested and written at the early fragment tests,
	// or the late ones when a shader writes depth or discards.
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		return VK_PIPELINE_STAGE_TRANSFER_BIT;
	case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
	case VK_IMAGE_LAYOUT_UNDEFINED:
		return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	default:
		return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	}
}

// ---------------------------------------------------------------------------
// Internal helper: map a Carrottus GPU/DXGI-numbered format to the aspect(s) it has.
// Used for buffer<->image copies, blits, and subresource barriers, where we
// have an actual image handle (and thus its real format) rather than just a
// layout to guess from.
// ---------------------------------------------------------------------------

static VkImageAspectFlags cr_format_aspect(VkFormat fmt) {
	switch (fmt) {
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
		return VK_IMAGE_ASPECT_DEPTH_BIT;
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	default:
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}
}

static VkFilter cr_filter_to_vk(CRGPU_FILTER filter) {
	switch (filter) {
	case CRGPU_FILTER_LINEAR:
		return VK_FILTER_LINEAR;
	case CRGPU_FILTER_NEAREST:
	default:
		return VK_FILTER_NEAREST;
	}
}

// ---------------------------------------------------------------------------
// Internal helper: issue a VkImageMemoryBarrier for a given VkImage/range
// ---------------------------------------------------------------------------

static void crgpuvulkan___cmd_image_barrier_raw(
    VkCommandBuffer cmd_buf,
    VkImage image,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    VkImageSubresourceRange range) {
	VkImageMemoryBarrier barrier = {0};
	barrier.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout            = old_layout;
	barrier.newLayout            = new_layout;
	barrier.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                = image;
	barrier.subresourceRange     = range;
	barrier.srcAccessMask        = layout_to_access(old_layout);
	barrier.dstAccessMask        = layout_to_access(new_layout);

	// Out of UNDEFINED or PRESENT_SRC there is nothing to make visible, but
	// the layout transition is a write and still has to wait for what last
	// used the image: a swap chain image's acquire semaphore, which a submit
	// waits on at COLOR_ATTACHMENT_OUTPUT, or earlier work in the queue. A
	// source stage of TOP_OF_PIPE waits for neither; the destination stage
	// chains to both (synchronization validation's WRITE_AFTER_READ against
	// vkAcquireNextImageKHR otherwise).
	VkPipelineStageFlags src_stage       = layout_to_stage(old_layout);
	const VkPipelineStageFlags dst_stage = layout_to_stage(new_layout);
	if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED || old_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
		src_stage = dst_stage;

	vkCmdPipelineBarrier(
	    cmd_buf,
	    src_stage,                   // srcStageMask
	    dst_stage,                   // dstStageMask
	    0,                           // dependencyFlags
	    0, NULL,                     // memory barriers
	    0, NULL,                     // buffer barriers
	    1, &barrier);                // image barriers
}

#if __cplusplus
extern "C" {

#endif

// ---------------------------------------------------------------------------
// Public API: crgpu_command_list_image_barrier  (crgpu_image variant)
// ---------------------------------------------------------------------------

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

	VkImageLayout vk_old = cr_layout_to_vk(old_layout);
	VkImageLayout vk_new = cr_layout_to_vk(new_layout);

	VkImageSubresourceRange range = {0};
	range.aspectMask              = cr_format_aspect(image->_format_vk);
	// Every mip level and array layer: the whole image, as the header says.
	range.baseMipLevel            = 0;
	range.levelCount              = VK_REMAINING_MIP_LEVELS;
	range.baseArrayLayer          = 0;
	range.layerCount              = VK_REMAINING_ARRAY_LAYERS;

	crgpuvulkan___cmd_image_barrier_raw(cmd->_command_buffer_vk, image->_image_vk, vk_old, vk_new, range);
}

// ---------------------------------------------------------------------------
// Public API: crgpu_command_list_image_barrier_view (crgpu_image_view variant)
// For swap chain images that have no crgpu_image handle.
// ---------------------------------------------------------------------------

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
	crgpu_command_list_vulkan *cl = (crgpu_command_list_vulkan *)cmd;
	crgpu_image_view_vulkan *view = (crgpu_image_view_vulkan *)image_view;

	VkImageLayout vk_old = cr_layout_to_vk(old_layout);
	VkImageLayout vk_new = cr_layout_to_vk(new_layout);

	VkImageSubresourceRange range = {0};
	range.aspectMask              = cr_format_aspect(view->_desc.parent_image->_format_vk);
	// Every mip level and array layer: the whole image, as the header says.
	range.baseMipLevel            = 0;
	range.levelCount              = VK_REMAINING_MIP_LEVELS;
	range.baseArrayLayer          = 0;
	range.layerCount              = VK_REMAINING_ARRAY_LAYERS;

	// Pull the raw VkImage out of the parent image stored in the view struct.
	crgpuvulkan___cmd_image_barrier_raw(cmd->_command_buffer_vk, view->_desc.parent_image->_image_vk, vk_old, vk_new, range);
}

// ---------------------------------------------------------------------------
// Public API: crgpu_command_list_image_barrier_subresource
// ---------------------------------------------------------------------------

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

	VkImageLayout vk_old = cr_layout_to_vk(old_layout);
	VkImageLayout vk_new = cr_layout_to_vk(new_layout);

	VkImageSubresourceRange vk_range = {0};
	vk_range.aspectMask              = cr_format_aspect(image->_format_vk);
	vk_range.baseMipLevel            = (uint32_t)range->base_mip_level;
	vk_range.levelCount              = (uint32_t)range->mip_level_count;
	vk_range.baseArrayLayer          = (uint32_t)range->base_array_layer;
	vk_range.layerCount              = (uint32_t)range->array_layer_count;

	crgpuvulkan___cmd_image_barrier_raw(cmd->_command_buffer_vk, image->_image_vk, vk_old, vk_new, vk_range);
}

// ---------------------------------------------------------------------------
// Public API: crgpu_command_list_copy_buffer_to_image / crgpu_command_list_copy_image_to_buffer
// ---------------------------------------------------------------------------

static VkBufferImageCopy crgpuvulkan___make_buffer_image_copy(
    VkImageAspectFlags aspect,
    const crgpu_image_buffer_copy_desc *desc) {
	VkBufferImageCopy region               = {0};
	region.bufferOffset                    = (VkDeviceSize)desc->buffer_offset;
	region.bufferRowLength                 = desc->buffer_row_length;
	region.bufferImageHeight               = desc->buffer_image_height;
	region.imageSubresource.aspectMask     = aspect;
	region.imageSubresource.mipLevel       = desc->mip_level;
	region.imageSubresource.baseArrayLayer = desc->base_array_layer;
	region.imageSubresource.layerCount     = desc->array_layer_count ? desc->array_layer_count : 1;
	region.imageOffset.x                   = (int32_t)desc->image_x;
	region.imageOffset.y                   = (int32_t)desc->image_y;
	region.imageOffset.z                   = (int32_t)desc->image_z;
	region.imageExtent.width               = desc->width;
	region.imageExtent.height              = desc->height;
	region.imageExtent.depth               = desc->depth ? desc->depth : 1;
	return region;
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
	VkBufferCopy region = {0};
	region.srcOffset    = (VkDeviceSize)src_offset;
	region.dstOffset    = (VkDeviceSize)dst_offset;
	region.size         = (VkDeviceSize)size;
	vkCmdCopyBuffer(cmd->_command_buffer_vk, src_buffer->_buffer_vk, dst_buffer->_buffer_vk, 1, &region);
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

	VkBufferImageCopy region = crgpuvulkan___make_buffer_image_copy(cr_format_aspect(src_image->_format_vk), desc);

	vkCmdCopyImageToBuffer(cmd->_command_buffer_vk, src_image->_image_vk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_buffer->_buffer_vk, 1, &region);
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

	VkBufferImageCopy region = crgpuvulkan___make_buffer_image_copy(cr_format_aspect(dst_image->_format_vk), desc);

	vkCmdCopyBufferToImage(cmd->_command_buffer_vk, src_buffer->_buffer_vk, dst_image->_image_vk, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

// ---------------------------------------------------------------------------
// Public API: crgpu_image_get_buffer_copy_size
// Vulkan has no row-pitch alignment requirement for buffer<->image copies
// (bufferRowLength/bufferImageHeight of 0 means tightly packed), so this
// always reports the tightly-packed size for the given mip level.
// ---------------------------------------------------------------------------

void crgpu_image_get_buffer_copy_size(
    crgpu_image image,
    uint32_t mip_level,
    uint64_t *out_row_pitch,
    uint64_t *out_total_size) {
	if (!image)
		return;
	if (CR_STALE(image, "crgpu_image_get_buffer_copy_size", "image"))
		return;

	uint32_t mip_width  = image->_desc.width >> mip_level;
	uint32_t mip_height = image->_desc.height >> mip_level;
	uint32_t mip_depth  = image->_desc.depth >> mip_level;
	if (!mip_width)
		mip_width = 1;
	if (!mip_height)
		mip_height = 1;
	if (!mip_depth)
		mip_depth = 1;

	uint64_t row_pitch = (uint64_t)mip_width * (crgpu_format_get_bit_count(image->_desc.format) / 8);

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
	const uint64_t block = crgpu___internal_format_block_bytes(image->_desc.format);
	if (block == 0)
		return CR_RESULT_GPU_INVALID_FORMAT;
	// VkBufferImageCopy::bufferOffset: a multiple of the texel block size,
	// and of 4 for a depth/stencil format or on a queue with neither
	// graphics nor compute. The least common multiple of the two meets every
	// case, on whatever queue the copy is recorded for.
	uint64_t a = block, b = 4;
	while (b) {
		const uint64_t r = a % b;
		a                = b;
		b                = r;
	}
	*out_alignment = block / a * 4;
	return CR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Public API: crgpu_command_list_blit_image
// ---------------------------------------------------------------------------

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

	VkImageBlit region                   = {0};
	region.srcSubresource.aspectMask     = cr_format_aspect(src_image->_format_vk);
	region.srcSubresource.mipLevel       = desc->src_mip_level;
	region.srcSubresource.baseArrayLayer = desc->src_base_array_layer;
	region.srcSubresource.layerCount     = desc->src_array_layer_count ? desc->src_array_layer_count : 1;
	region.srcOffsets[0].x               = (int32_t)desc->src_x0;
	region.srcOffsets[0].y               = (int32_t)desc->src_y0;
	region.srcOffsets[0].z               = (int32_t)desc->src_z0;
	region.srcOffsets[1].x               = (int32_t)desc->src_x1;
	region.srcOffsets[1].y               = (int32_t)desc->src_y1;
	region.srcOffsets[1].z               = (int32_t)desc->src_z1;

	region.dstSubresource.aspectMask     = cr_format_aspect(dst_image->_format_vk);
	region.dstSubresource.mipLevel       = desc->dst_mip_level;
	region.dstSubresource.baseArrayLayer = desc->dst_base_array_layer;
	region.dstSubresource.layerCount     = desc->dst_array_layer_count ? desc->dst_array_layer_count : 1;
	region.dstOffsets[0].x               = (int32_t)desc->dst_x0;
	region.dstOffsets[0].y               = (int32_t)desc->dst_y0;
	region.dstOffsets[0].z               = (int32_t)desc->dst_z0;
	region.dstOffsets[1].x               = (int32_t)desc->dst_x1;
	region.dstOffsets[1].y               = (int32_t)desc->dst_y1;
	region.dstOffsets[1].z               = (int32_t)desc->dst_z1;

	vkCmdBlitImage(
	    cmd->_command_buffer_vk, src_image->_image_vk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image->_image_vk, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	    &region, cr_filter_to_vk(desc->filter));
}

// ---------------------------------------------------------------------------
// Internal helpers: map Carrottus GPU load/store op → Vulkan
// ---------------------------------------------------------------------------

static VkAttachmentLoadOp cr_load_op_to_vk(CRGPU_LOAD_OP op) {
	switch (op) {
	case CRGPU_LOAD_OP_CLEAR:
		return VK_ATTACHMENT_LOAD_OP_CLEAR;
	case CRGPU_LOAD_OP_DONT_CARE:
		return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	case CRGPU_LOAD_OP_LOAD:
	default:
		return VK_ATTACHMENT_LOAD_OP_LOAD;
	}
}

static VkAttachmentStoreOp cr_store_op_to_vk(CRGPU_STORE_OP op) {
	switch (op) {
	case CRGPU_STORE_OP_DONT_CARE:
		return VK_ATTACHMENT_STORE_OP_DONT_CARE;
	case CRGPU_STORE_OP_STORE:
	default:
		return VK_ATTACHMENT_STORE_OP_STORE;
	}
}

// ---------------------------------------------------------------------------
// Public API: crgpu_command_list_begin_rendering / crgpu_command_list_end_rendering
// Vulkan 1.3 core dynamic rendering (vkCmdBeginRendering/vkCmdEndRendering) —
// no VkRenderPass/VkFramebuffer object, no pipeline-compatibility coupling.
// See the TODO in crgpu.h re: a classic VkRenderPass fallback for devices
// without Vulkan 1.3 / VK_KHR_dynamic_rendering support.
// ---------------------------------------------------------------------------

void crgpu_command_list_begin_rendering(
    crgpu_command_list cmd,
    const crgpu_rendering_begin_desc *desc) {
	if (!cmd || !desc)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_begin_rendering", "cmd"))
		return;
	if (cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC) != CR_SUCCESS)
		return;
	// A layered pass needs a device created with
	// CRGPU_FEATURE_LAYERED_RENDERING, which is what switched
	// shaderOutputLayer on (crgpu.h, crgpu_rendering_begin_desc::layer_count).
	assert((desc->layer_count == 0 || (cmd->_allocator._device._features & CRGPU_FEATURE_LAYERED_RENDERING)) &&
	       "crgpu_command_list_begin_rendering: a layered pass on a device created without CRGPU_FEATURE_LAYERED_RENDERING");

	uint32_t color_count = desc->color_attachment_count;
	if (color_count > CRGPU_MAX_COLOR_ATTACHMENTS)
		color_count = CRGPU_MAX_COLOR_ATTACHMENTS;

	VkRenderingAttachmentInfo color_infos[CRGPU_MAX_COLOR_ATTACHMENTS] = {0};
	for (uint32_t i = 0; i < color_count; ++i) {
		const crgpu_color_attachment_desc *src = &desc->color_attachments[i];
		crgpu_image_view_vulkan *view          = (crgpu_image_view_vulkan *)src->image_view;

		VkRenderingAttachmentInfo *info   = &color_infos[i];
		info->sType                       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		info->imageView                   = view->_image_view_vk;
		info->imageLayout                 = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		info->loadOp                      = cr_load_op_to_vk(src->load_op);
		info->storeOp                     = cr_store_op_to_vk(src->store_op);
		info->clearValue.color.float32[0] = src->clear_color[0];
		info->clearValue.color.float32[1] = src->clear_color[1];
		info->clearValue.color.float32[2] = src->clear_color[2];
		info->clearValue.color.float32[3] = src->clear_color[3];
	}

	VkRenderingAttachmentInfo depth_info   = {0};
	VkRenderingAttachmentInfo stencil_info = {0};
	int has_depth                          = (desc->depth_attachment.image_view != NULL);
	int has_stencil                        = 0;
	if (has_depth) {
		crgpu_image_view_vulkan *view = (crgpu_image_view_vulkan *)desc->depth_attachment.image_view;
		has_stencil                     = (cr_format_aspect(view->_desc.parent_image->_format_vk) & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;

		depth_info.sType                         = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depth_info.imageView                     = view->_image_view_vk;
		depth_info.imageLayout                   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth_info.loadOp                        = cr_load_op_to_vk(desc->depth_attachment.depth_load_op);
		depth_info.storeOp                       = cr_store_op_to_vk(desc->depth_attachment.depth_store_op);
		depth_info.clearValue.depthStencil.depth = desc->depth_attachment.clear_depth;

		if (has_stencil) {
			stencil_info                                 = depth_info;
			stencil_info.loadOp                          = cr_load_op_to_vk(desc->depth_attachment.stencil_load_op);
			stencil_info.storeOp                         = cr_store_op_to_vk(desc->depth_attachment.stencil_store_op);
			stencil_info.clearValue.depthStencil.stencil = desc->depth_attachment.clear_stencil;
		}
	}

	VkRenderingInfo rendering_info          = {0};
	rendering_info.sType                    = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering_info.renderArea.offset.x      = 0;
	rendering_info.renderArea.offset.y      = 0;
	rendering_info.renderArea.extent.width  = desc->width;
	rendering_info.renderArea.extent.height = desc->height;
	// A pass that is not layered is one layer to Vulkan.
	rendering_info.layerCount               = desc->layer_count ? desc->layer_count : 1;
	rendering_info.colorAttachmentCount     = color_count;
	rendering_info.pColorAttachments        = color_infos;
	rendering_info.pDepthAttachment         = has_depth ? &depth_info : NULL;
	rendering_info.pStencilAttachment       = has_stencil ? &stencil_info : NULL;
	// Only set when the caller says a bundle will be executed inside this
	// scope (see crgpu_rendering_begin_desc::will_execute_bundles) —
	// vkCmdExecuteCommands requires it, but Carrottus never sets it on the
	// caller's behalf.
	rendering_info.flags                    = desc->will_execute_bundles
	                                               ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
	                                               : 0;

    // The pass's start: when the first of its work reaches the top of the
    // pipe. Written outside the rendering instance, as its end is.
    cmd->_end_timestamp_pool_vk = VK_NULL_HANDLE;
    if (desc->timestamp_set) {
        vkCmdWriteTimestamp(cmd->_command_buffer_vk, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            desc->timestamp_set->_query_pool_vk, desc->timestamp_begin_index);
        cmd->_end_timestamp_pool_vk = desc->timestamp_set->_query_pool_vk;
        cmd->_end_timestamp_index   = desc->timestamp_end_index;
    }

    vkCmdBeginRendering(cmd->_command_buffer_vk, &rendering_info);

    // Recorded so crgpu_command_list_clear_color_attachment/depth_attachment can
    // take the vkCmdClearAttachments fast path below instead of nesting a
    // redundant (and invalid) begin/end pair.
    cmd->_rendering_active = true;
    cmd->_bound_layer_count = rendering_info.layerCount;
    cmd->_bound_color_attachment_count = color_count;
    for (uint32_t i = 0; i < color_count; ++i)
        cmd->_bound_color_attachments[i] = color_infos[i].imageView;
    cmd->_bound_depth_attachment_view = has_depth ? depth_info.imageView : VK_NULL_HANDLE;
}

void crgpu_command_list_end_rendering(crgpu_command_list cmd) {
    if (!cmd)
        return;
    if (CR_STALE(cmd, "crgpu_command_list_end_rendering", "cmd"))
        return;
    vkCmdEndRendering(cmd->_command_buffer_vk);
    // The pass's end: once all of its work has left the pipe.
    if (cmd->_end_timestamp_pool_vk) {
        vkCmdWriteTimestamp(cmd->_command_buffer_vk, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            cmd->_end_timestamp_pool_vk, cmd->_end_timestamp_index);
        cmd->_end_timestamp_pool_vk = VK_NULL_HANDLE;
    }
    cmd->_rendering_active = false;
    cmd->_bound_color_attachment_count = 0;
    cmd->_bound_depth_attachment_view = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Public API: crgpu_command_list_begin_bundle / crgpu_command_list_execute_bundle
// A bundle is a VK_COMMAND_BUFFER_LEVEL_SECONDARY buffer (see
// crgpu_command_list_create, which allocates at this level for a
// CRGPU_COMMAND_LIST_TYPE_BUNDLE allocator). VK_KHR_dynamic_rendering
// requires its attachment formats up front via
// VkCommandBufferInheritanceRenderingInfo, since there is no VkRenderPass/
// VkFramebuffer object to inherit them from implicitly, and a secondary
// buffer inherits no viewport or scissor, so those are set at its start.
// ---------------------------------------------------------------------------

#if CRGPU_EXT_BUNDLES
void crgpu_command_list_begin_bundle(
    crgpu_command_list bundle, const crgpu_bundle_inheritance_desc *desc) {
    if (!bundle || !desc)
        return;
    if (CR_STALE(bundle, "crgpu_command_list_begin_bundle", "bundle"))
        return;
    if (cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_BUNDLE_INHERITANCE_DESC) != CR_SUCCESS)
        return;
#if _DEBUG
    crgpuvulkan___debug_list_opened(bundle);
#endif

    vkResetCommandBuffer(bundle->_command_buffer_vk, 0);

    // Past CRGPU_MAX_COLOR_ATTACHMENTS is not read.
    uint32_t color_count = desc->color_attachment_count;
    if (color_count > CRGPU_MAX_COLOR_ATTACHMENTS)
        color_count = CRGPU_MAX_COLOR_ATTACHMENTS;
    VkFormat color_formats[CRGPU_MAX_COLOR_ATTACHMENTS] = {0};
    for (uint32_t i = 0; i < color_count; ++i)
        color_formats[i] = convert_cr_to_vulkan_format(desc->color_attachment_formats[i]);
    VkFormat depth_format = (desc->depth_format != CRGPU_FORMAT_UNKNOWN)
        ? convert_cr_to_vulkan_format(desc->depth_format)
        : VK_FORMAT_UNDEFINED;
    bool has_stencil = depth_format != VK_FORMAT_UNDEFINED &&
        (cr_format_aspect(depth_format) & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;

    VkCommandBufferInheritanceRenderingInfo inheritance_rendering = {0};
    inheritance_rendering.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
    inheritance_rendering.colorAttachmentCount = color_count;
    inheritance_rendering.pColorAttachmentFormats = color_formats;
    inheritance_rendering.depthAttachmentFormat = depth_format;
    inheritance_rendering.stencilAttachmentFormat = has_stencil ? depth_format : VK_FORMAT_UNDEFINED;
    inheritance_rendering.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkCommandBufferInheritanceInfo inheritance = {0};
    inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritance.pNext = &inheritance_rendering;

    // RENDER_PASS_CONTINUE_BIT is required for a secondary buffer that will
    // be executed inside an active rendering instance (dynamic rendering
    // included, per VK_KHR_dynamic_rendering). SIMULTANEOUS_USE_BIT is
    // required because a bundle is recorded once and resubmitted via
    // vkCmdExecuteCommands every frame without ever being re-recorded --
    // without it, resubmitting the buffer while a prior submission may
    // still be pending on the GPU is invalid usage.
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    begin.pInheritanceInfo = &inheritance;
    vkBeginCommandBuffer(bundle->_command_buffer_vk, &begin);

    // A secondary command buffer inherits no dynamic state, and every
    // pipeline's viewport and scissor are dynamic: the bundle is given the
    // ones the direct list will have set (crgpu.h's bundle rules). D3D12
    // takes them from the direct list instead.
    crgpu_command_list_set_viewports(bundle, 0, 1, &desc->viewport);
    crgpu_command_list_set_scissor_rects(bundle, 0, 1, &desc->scissor_rect);
}

void crgpu_command_list_execute_bundle(crgpu_command_list cmd, crgpu_command_list bundle) {
    if (!cmd || !bundle)
        return;
    if (CR_STALE(cmd, "crgpu_command_list_execute_bundle", "cmd"))
        return;
    if (CR_STALE(bundle, "crgpu_command_list_execute_bundle", "bundle"))
        return;
    vkCmdExecuteCommands(cmd->_command_buffer_vk, 1, &bundle->_command_buffer_vk);
}
#endif // CRGPU_EXT_BUNDLES

// ---------------------------------------------------------------------------
// Public API: crgpu_command_list_clear_color_attachment / crgpu_command_list_clear_depth_attachment
// Vulkan's own implementation (every backend implements this pair natively;
// there is no shared cross-backend fallback): when a rendering instance is
// already active and the target view is one of
// its bound attachments, clear it in place via vkCmdClearAttachments instead
// of opening a nested rendering instance — which vkCmdBeginRendering forbids
// outright while another is active on the same command buffer, so this
// isn't just an optimization, it's the only way to support that case at all.
// Falls back to opening a throwaway rendering instance just for the clear
// when no rendering instance is currently open.
// ---------------------------------------------------------------------------

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

    crgpu_image_view_vulkan *view = (crgpu_image_view_vulkan *) attachment;

    if (cmd->_rendering_active) {
        for (uint32_t i = 0; i < cmd->_bound_color_attachment_count; ++i) {
            if (cmd->_bound_color_attachments[i] != view->_image_view_vk)
                continue;

            VkClearAttachment clear_attachment = {0};
            clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clear_attachment.colorAttachment = i;
            clear_attachment.clearValue.color.float32[0] = r;
            clear_attachment.clearValue.color.float32[1] = g;
            clear_attachment.clearValue.color.float32[2] = b;
            clear_attachment.clearValue.color.float32[3] = a;

            VkClearRect clear_rect = {0};
            clear_rect.rect.extent.width = width;
            clear_rect.rect.extent.height = height;
            // Every layer of the pass, as a clear load operation does.
            clear_rect.layerCount = cmd->_bound_layer_count;

            vkCmdClearAttachments(cmd->_command_buffer_vk, 1, &clear_attachment, 1, &clear_rect);
            return;
        }
        // Not one of the currently-bound attachments — this function has no
        // way to end/resume the active rendering instance to reach it, and
        // nesting a new one is invalid. No-op: callers should only clear
        // this way against an attachment they are currently rendering into,
        // or before any rendering instance is open at all (handled below).
        return;
    }

    crgpu_color_attachment_desc color = {0};
    color.image_view = attachment;
    color.load_op = CRGPU_LOAD_OP_CLEAR;
    color.store_op = CRGPU_STORE_OP_STORE;
    color.clear_color[0] = r;
    color.clear_color[1] = g;
    color.clear_color[2] = b;
    color.clear_color[3] = a;

    crgpu_rendering_begin_desc desc = {0};
    desc.base.type = CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
    desc.color_attachments[0] = color;
    desc.color_attachment_count = 1;
    desc.width = width;
    desc.height = height;

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

    crgpu_image_view_vulkan *view = (crgpu_image_view_vulkan *) attachment;

    if (cmd->_rendering_active) {
        if (cmd->_bound_depth_attachment_view == view->_image_view_vk) {
            VkClearAttachment clear_attachment = {0};
            clear_attachment.aspectMask =
                (clear_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                (clear_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            clear_attachment.clearValue.depthStencil.depth = depth;
            clear_attachment.clearValue.depthStencil.stencil = stencil;

            VkClearRect clear_rect = {0};
            clear_rect.rect.extent.width = width;
            clear_rect.rect.extent.height = height;
            // Every layer of the pass, as a clear load operation does.
            clear_rect.layerCount = cmd->_bound_layer_count;

            vkCmdClearAttachments(cmd->_command_buffer_vk, 1, &clear_attachment, 1, &clear_rect);
        }
        // Bound depth view doesn't match — same no-op rationale as color above.
        return;
    }

    crgpu_rendering_begin_desc desc = {0};
    desc.base.type = CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
    desc.depth_attachment.image_view = attachment;
    desc.depth_attachment.depth_load_op = clear_depth ? CRGPU_LOAD_OP_CLEAR : CRGPU_LOAD_OP_LOAD;
    desc.depth_attachment.depth_store_op = CRGPU_STORE_OP_STORE;
    desc.depth_attachment.stencil_load_op = clear_stencil ? CRGPU_LOAD_OP_CLEAR : CRGPU_LOAD_OP_LOAD;
    desc.depth_attachment.stencil_store_op = CRGPU_STORE_OP_STORE;
    desc.depth_attachment.clear_depth = depth;
    desc.depth_attachment.clear_stencil = stencil;
    desc.width = width;
    desc.height = height;

    crgpu_command_list_begin_rendering(cmd, &desc);
    crgpu_command_list_end_rendering(cmd);
}

#if __cplusplus
}
#endif

#endif // CRGPU_COMPILE_VULKAN_API
