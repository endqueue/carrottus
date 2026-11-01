
#if SPUDGPU_COMPILE_D3D12_API

#include "spudgpud3d12.hpp"
#include "../../spudgpushared.h"
#include <cassert>
#include <cstdio>
#include "../../../spudstructbase.h"

// Maps SPUDGPU_IMAGE_LAYOUT to D3D12_RESOURCE_STATES.
static D3D12_RESOURCE_STATES spudgpu_d3d12_layout_to_state(SPUDGPU_IMAGE_LAYOUT layout) {
	switch (layout) {
	case SPUDGPU_IMAGE_LAYOUT_UNDEFINED:
	case SPUDGPU_IMAGE_LAYOUT_GENERAL:
		return D3D12_RESOURCE_STATE_COMMON;
	case SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		return D3D12_RESOURCE_STATE_RENDER_TARGET;
	case SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		return D3D12_RESOURCE_STATE_DEPTH_WRITE;
	case SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY:
		return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
		       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	case SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC:
		return D3D12_RESOURCE_STATE_COPY_SOURCE;
	case SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST:
		return D3D12_RESOURCE_STATE_COPY_DEST;
	case SPUDGPU_IMAGE_LAYOUT_PRESENT_SRC:
		return D3D12_RESOURCE_STATE_PRESENT;
	default:
		return D3D12_RESOURCE_STATE_COMMON;
	}
}

// The "before" state always comes from the image's own tracked
// _current_state (spudgpud3d12.hpp), never from old_layout. old_layout
// follows the Vulkan convention where SPUDGPU_IMAGE_LAYOUT_UNDEFINED means
// "whatever it is right now, don't care" -- Vulkan can express that directly
// (VK_IMAGE_LAYOUT_UNDEFINED), but D3D12's strict before/after state
// tracking has no "don't care" transition, so the declared "before" state
// has to be the resource's real current state or the debug layer flags a
// BEFORE_AFTER_MISMATCH (and the bad transition can corrupt driver-side
// state badly enough to crash, not just warn). A static usage-based guess
// only gets this right for the first transition after creation; a resource
// that's cycled through several transitions already (a swap chain back
// buffer, every frame) needs the real current value, which is exactly what
// _current_state holds. Trusting our own tracking over a caller-supplied
// old_layout is strictly more robust, not a policy decision -- every state
// change on a spudgpu_image goes through these functions, so the tracked
// value is always accurate as long as the caller doesn't also mutate the
// resource state via the native escape hatch (spudgpu_d3d12_natives.h).
//
// The tracked state is one value for the whole image until
// spudgpu_cmd_image_barrier_subresource moves part of it, and one for each
// (mip, layer) from then until they agree again: see
// spudgpu_image_d3d12::_subresource_states.

// The state [layout] is for [image]. GENERAL is the storage image layout
// (spudgpu.h): for an image that can be a storage image that is
// UNORDERED_ACCESS, the one state a shader can write a texture in. For any
// other image GENERAL allows nothing in particular, and stays COMMON.
static D3D12_RESOURCE_STATES spudgpu_d3d12_image_layout_state(
    const spudgpu_image_d3d12 *image,
    SPUDGPU_IMAGE_LAYOUT layout) {
	if (layout == SPUDGPU_IMAGE_LAYOUT_GENERAL && (image->_desc.usage & SPUDGPU_IMAGE_USAGE_STORAGE))
		return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	return spudgpu_d3d12_layout_to_state(layout);
}

// The header's ordering barrier for a storage image: nothing to transition,
// and writes before it must be complete before accesses after it. D3D12's
// UAV barrier is for the resource, not for one subresource of it.
static D3D12_RESOURCE_BARRIER spudgpu_d3d12_uav_barrier(spudgpu_image_d3d12 *image) {
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.UAV.pResource          = image->_d3d_resource.Get();
	return barrier;
}

// How many planes each (mip, layer) of [format] has: a depth format with
// stencil keeps the two in separate planes, and so in separate subresources.
static UINT spudgpu_d3d12_format_plane_count(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
		return 2;
	default:
		return 1;
	}
}

// One transition barrier for every plane of (mip, layer) of [image]. False,
// with some of them pushed, if [barriers] couldn't grow.
static bool spudgpu_d3d12_push_subresource_barriers(
    eq::array<D3D12_RESOURCE_BARRIER> &barriers,
    spudgpu_image_d3d12 *image,
    UINT mip,
    UINT layer,
    D3D12_RESOURCE_STATES stateBefore,
    D3D12_RESOURCE_STATES stateAfter) {
	const UINT mipLevels = image->_desc.mip_levels;
	const UINT arraySize = image->_desc.array_layers;
	const UINT planes    = spudgpu_d3d12_format_plane_count(image->_d3d_resource_desc.Format);
	for (UINT plane = 0; plane < planes; ++plane) {
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource   = image->_d3d_resource.Get();
		barrier.Transition.StateBefore = stateBefore;
		barrier.Transition.StateAfter  = stateAfter;
		barrier.Transition.Subresource = D3D12CalcSubresource(mip, layer, plane, mipLevels, arraySize);
		if (barriers.push(barrier) != EQ_SUCCESS)
			return false;
	}
	return true;
}

static void spudgpu_d3d12_resource_barrier(
    spudgpu_command_list_d3d12 *cmd,
    spudgpu_image_d3d12 *image,
    SPUDGPU_IMAGE_LAYOUT new_layout) {
	D3D12_RESOURCE_STATES stateAfter = spudgpu_d3d12_image_layout_state(image, new_layout);
	// Already there and staying: for the storage state that is the ordering
	// barrier, for any other state it is nothing.
	const bool is_storage_state = stateAfter == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	if (!image->_subresource_states.empty()) {
		// The subresources are in different states: each that isn't in the
		// new one already is moved from its own, and the image is whole
		// again afterwards.
		const UINT mipLevels = image->_desc.mip_levels;
		const UINT arraySize = image->_desc.array_layers;
		eq::array<D3D12_RESOURCE_BARRIER> &barriers = cmd->_scratch_barriers;
		barriers.clear();
		bool built = true;
		for (UINT layer = 0; layer < arraySize && built; ++layer) {
			for (UINT mip = 0; mip < mipLevels && built; ++mip) {
				const D3D12_RESOURCE_STATES stateBefore = image->_subresource_states.data[mip + (size_t)layer * mipLevels];
				if (stateBefore != stateAfter)
					built = spudgpu_d3d12_push_subresource_barriers(barriers, image, mip, layer, stateBefore, stateAfter);
			}
		}
		if (built && barriers.empty() && is_storage_state)
			built = barriers.push(spudgpu_d3d12_uav_barrier(image)) == EQ_SUCCESS;
		// Out of memory: nothing is recorded, and the tracked states stay as
		// they were.
		if (!built) {
			printf("spudgpu: spudgpu_cmd_image_barrier - out of memory building barriers; nothing recorded\n");
			return;
		}
		if (!barriers.empty())
			cmd->_d3d_cmd_list->ResourceBarrier((UINT)barriers.size(), barriers.data);
		image->_subresource_states.clear();
		image->_current_state = stateAfter;
		return;
	}

	D3D12_RESOURCE_STATES stateBefore = image->_current_state;
	if (stateBefore == stateAfter) {
		if (is_storage_state) {
			D3D12_RESOURCE_BARRIER barrier = spudgpu_d3d12_uav_barrier(image);
			cmd->_d3d_cmd_list->ResourceBarrier(1, &barrier);
		}
		return;
	}

	D3D12_RESOURCE_BARRIER barrier           = {};
	barrier.Type                             = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags                            = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource             = image->_d3d_resource.Get();
	barrier.Transition.StateBefore           = stateBefore;
	barrier.Transition.StateAfter            = stateAfter;
	barrier.Transition.Subresource           = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->_d3d_cmd_list->ResourceBarrier(1, &barrier);
	image->_current_state = stateAfter;
}

extern "C" {

void spudgpu_cmd_image_barrier(
    spudgpu_command_list cmd,
    spudgpu_image image,
    SPUDGPU_IMAGE_LAYOUT old_layout,
    SPUDGPU_IMAGE_LAYOUT new_layout) {
	(void)old_layout;
	if (!cmd || !image)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_image_barrier", "cmd"))
		return;
	if (SPUD_STALE(image, "spudgpu_cmd_image_barrier", "image"))
		return;
	spudgpu_d3d12_resource_barrier(cmd, image, new_layout);
}

void spudgpu_cmd_image_barrier_view(
    spudgpu_command_list cmd,
    spudgpu_image_view image_view,
    SPUDGPU_IMAGE_LAYOUT old_layout,
    SPUDGPU_IMAGE_LAYOUT new_layout) {
	(void)old_layout;
	if (!cmd || !image_view)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_image_barrier_view", "cmd"))
		return;
	if (SPUD_STALE(image_view, "spudgpu_cmd_image_barrier_view", "image_view"))
		return;
	if (!image_view->_image)
		return;
	spudgpu_d3d12_resource_barrier(cmd, image_view->_image, new_layout);
}

void spudgpu_cmd_image_barrier_subresource(
    spudgpu_command_list cmd,
    spudgpu_image image,
    const spudgpu_image_view_desc_subresource_range *range,
    SPUDGPU_IMAGE_LAYOUT old_layout,
    SPUDGPU_IMAGE_LAYOUT new_layout) {
	(void)old_layout;
	if (!cmd || !image || !range)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_image_barrier_subresource", "cmd"))
		return;
	if (SPUD_STALE(image, "spudgpu_cmd_image_barrier_subresource", "image"))
		return;

	const D3D12_RESOURCE_STATES stateAfter = spudgpu_d3d12_image_layout_state(image, new_layout);
	const UINT mipLevels                   = image->_desc.mip_levels;
	const UINT arraySize                   = image->_desc.array_layers;

	// A range past the end of the image has nothing to move. Checked before
	// any arithmetic, so an oversized count can't wrap.
	if (range->base_mip_level >= mipLevels || range->base_array_layer >= arraySize)
		return;
	if (range->mip_level_count > mipLevels - range->base_mip_level)
		return;
	if (range->array_layer_count > arraySize - range->base_array_layer)
		return;

	// The whole image, and nothing of it out of step: the one-barrier path.
	if (image->_subresource_states.empty() && range->base_mip_level == 0 && range->mip_level_count == mipLevels &&
	    range->base_array_layer == 0 && range->array_layer_count == arraySize) {
		spudgpu_d3d12_resource_barrier(cmd, image, new_layout);
		return;
	}

	// The states are about to differ across the image, so each (mip, layer)
	// gets its own from here until they agree again. As for the whole image
	// (see spudgpu_d3d12_resource_barrier), the state before is the one
	// tracked here, never old_layout.
	if (image->_subresource_states.empty()) {
		if (image->_subresource_states.resize((size_t)mipLevels * arraySize) != EQ_SUCCESS) {
			printf("spudgpu: spudgpu_cmd_image_barrier_subresource - out of memory tracking subresource states; nothing recorded\n");
			return;
		}
		for (D3D12_RESOURCE_STATES &state : image->_subresource_states)
			state = image->_current_state;
	}

	// Room for every barrier this can make is taken first, so that nothing
	// below can fail once the tracked states start to change.
	eq::array<D3D12_RESOURCE_BARRIER> &barriers = cmd->_scratch_barriers;
	barriers.clear();
	// One more than the transitions, for the ordering barrier below.
	if (barriers.reserve((size_t)range->mip_level_count * range->array_layer_count * spudgpu_d3d12_format_plane_count(image->_d3d_resource_desc.Format) +
	                     1) != EQ_SUCCESS) {
		printf("spudgpu: spudgpu_cmd_image_barrier_subresource - out of memory building barriers; nothing recorded\n");
		return;
	}
	for (UINT layer = 0; layer < (UINT)range->array_layer_count; ++layer) {
		for (UINT mip = 0; mip < (UINT)range->mip_level_count; ++mip) {
			const UINT m                   = (UINT)range->base_mip_level + mip;
			const UINT l                   = (UINT)range->base_array_layer + layer;
			D3D12_RESOURCE_STATES &tracked = image->_subresource_states.data[m + (size_t)l * mipLevels];
			if (tracked == stateAfter)
				continue;
			// Room was made above, so the pushes can't fail.
			(void)spudgpu_d3d12_push_subresource_barriers(barriers, image, m, l, tracked, stateAfter);
			tracked = stateAfter;
		}
	}

	// Nothing in the range had to move and it is in the storage state: the
	// header's ordering barrier.
	if (barriers.empty() && stateAfter == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		(void)barriers.push(spudgpu_d3d12_uav_barrier(image));

	if (!barriers.empty())
		cmd->_d3d_cmd_list->ResourceBarrier((UINT)barriers.size(), barriers.data);

	// Back in one state everywhere: return to tracking the image as a whole.
	bool uniform = true;
	for (D3D12_RESOURCE_STATES state : image->_subresource_states) {
		if (state != stateAfter) {
			uniform = false;
			break;
		}
	}
	if (uniform) {
		image->_subresource_states.clear();
		image->_current_state = stateAfter;
	}
}

// Builds a D3D12_PLACED_SUBRESOURCE_FOOTPRINT describing the buffer-side
// layout for one array layer of a buffer<->image copy region. Uses the
// caller-supplied row/image length as-is (0 = tightly packed) rather than
// forcing D3D12's 256-byte row-pitch alignment — a caller that wants a
// portable, correctly-aligned staging buffer should size it via
// spudgpu_get_image_buffer_copy_size first and pass that pitch in.
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT spudgpu_d3d12_make_footprint(
    SPUDGPU_FORMAT format,
    const spudgpu_image_buffer_copy_desc *desc) {
	UINT bytesPerTexel = spudgpu_format_bit_count(format) / 8;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	footprint.Offset             = desc->buffer_offset;
	footprint.Footprint.Format   = spudgpu_d3d12_get_dxgi_format(format);
	footprint.Footprint.Width    = desc->width;
	footprint.Footprint.Height   = desc->height;
	footprint.Footprint.Depth    = desc->depth ? desc->depth : 1;
	footprint.Footprint.RowPitch = (desc->buffer_row_length ? desc->buffer_row_length : desc->width) * bytesPerTexel;
	return footprint;
}

void spudgpu_cmd_copy_buffer(
    spudgpu_command_list cmd,
    spudgpu_buffer src_buffer,
    spudgpu_buffer dst_buffer,
    uint64_t src_offset,
    uint64_t dst_offset,
    uint64_t size) {
	if (!cmd || !src_buffer || !dst_buffer)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_copy_buffer", "cmd"))
		return;
	if (SPUD_STALE(src_buffer, "spudgpu_cmd_copy_buffer", "src_buffer"))
		return;
	if (SPUD_STALE(dst_buffer, "spudgpu_cmd_copy_buffer", "dst_buffer"))
		return;
	cmd->_d3d_cmd_list->CopyBufferRegion(
	    dst_buffer->_d3d_resource.Get(), dst_offset,
	    src_buffer->_d3d_resource.Get(), src_offset, size);
}

void spudgpu_cmd_copy_buffer_to_image(
    spudgpu_command_list cmd,
    spudgpu_buffer src_buffer,
    spudgpu_image dst_image,
    const spudgpu_image_buffer_copy_desc *desc) {
	if (!cmd || !src_buffer || !dst_image || !desc)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_copy_buffer_to_image", "cmd"))
		return;
	if (SPUD_STALE(src_buffer, "spudgpu_cmd_copy_buffer_to_image", "src_buffer"))
		return;
	if (SPUD_STALE(dst_image, "spudgpu_cmd_copy_buffer_to_image", "dst_image"))
		return;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint =
	    spudgpu_d3d12_make_footprint(dst_image->_desc.format, desc);

	UINT layerCount        = desc->array_layer_count ? desc->array_layer_count : 1;
	UINT bufferImageHeight = desc->buffer_image_height ? desc->buffer_image_height : desc->height;
	UINT64 layerPitchBytes = (UINT64)footprint.Footprint.RowPitch * bufferImageHeight;

	for (UINT layer = 0; layer < layerCount; ++layer) {
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT layerFootprint = footprint;
		layerFootprint.Offset += (UINT64)layer * layerPitchBytes;

		UINT dstSubresource = D3D12CalcSubresource(
		    desc->mip_level, desc->base_array_layer + layer, 0,
		    dst_image->_desc.mip_levels, dst_image->_desc.array_layers);

		CD3DX12_TEXTURE_COPY_LOCATION srcLoc(src_buffer->_d3d_resource.Get(), layerFootprint);
		CD3DX12_TEXTURE_COPY_LOCATION dstLoc(dst_image->_d3d_resource.Get(), dstSubresource);

		cmd->_d3d_cmd_list->CopyTextureRegion(
		    &dstLoc, desc->image_x, desc->image_y, desc->image_z, &srcLoc, nullptr);
	}
}

void spudgpu_cmd_copy_image_to_buffer(
    spudgpu_command_list cmd,
    spudgpu_image src_image,
    spudgpu_buffer dst_buffer,
    const spudgpu_image_buffer_copy_desc *desc) {
	if (!cmd || !src_image || !dst_buffer || !desc)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_copy_image_to_buffer", "cmd"))
		return;
	if (SPUD_STALE(src_image, "spudgpu_cmd_copy_image_to_buffer", "src_image"))
		return;
	if (SPUD_STALE(dst_buffer, "spudgpu_cmd_copy_image_to_buffer", "dst_buffer"))
		return;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint =
	    spudgpu_d3d12_make_footprint(src_image->_desc.format, desc);

	UINT layerCount        = desc->array_layer_count ? desc->array_layer_count : 1;
	UINT bufferImageHeight = desc->buffer_image_height ? desc->buffer_image_height : desc->height;
	UINT64 layerPitchBytes = (UINT64)footprint.Footprint.RowPitch * bufferImageHeight;

	D3D12_BOX srcBox;
	srcBox.left   = desc->image_x;
	srcBox.top    = desc->image_y;
	srcBox.front  = desc->image_z;
	srcBox.right  = desc->image_x + desc->width;
	srcBox.bottom = desc->image_y + desc->height;
	srcBox.back   = desc->image_z + (desc->depth ? desc->depth : 1);

	for (UINT layer = 0; layer < layerCount; ++layer) {
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT layerFootprint = footprint;
		layerFootprint.Offset += (UINT64)layer * layerPitchBytes;

		UINT srcSubresource = D3D12CalcSubresource(
		    desc->mip_level, desc->base_array_layer + layer, 0,
		    src_image->_desc.mip_levels, src_image->_desc.array_layers);

		CD3DX12_TEXTURE_COPY_LOCATION srcLoc(src_image->_d3d_resource.Get(), srcSubresource);
		CD3DX12_TEXTURE_COPY_LOCATION dstLoc(dst_buffer->_d3d_resource.Get(), layerFootprint);

		cmd->_d3d_cmd_list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);
	}
}

// GetCopyableFootprints reports D3D12's actual required (already-aligned)
// row pitch and total buffer size for the given mip level — unlike the raw
// copy commands above, this always returns a portably-correct value.
void spudgpu_get_image_buffer_copy_size(
    spudgpu_image image,
    uint32_t mip_level,
    uint64_t *out_row_pitch,
    uint64_t *out_total_size) {
	if (!image)
		return;
	if (SPUD_STALE(image, "spudgpu_get_image_buffer_copy_size", "image"))
		return;

	D3D12_RESOURCE_DESC resDesc = image->_d3d_resource_desc;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT64 totalBytes = 0;

	image->_device->_d3d_device->GetCopyableFootprints(
	    &resDesc, mip_level, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

	if (out_row_pitch)
		*out_row_pitch = footprint.Footprint.RowPitch;
	if (out_total_size)
		*out_total_size = totalBytes;
}

SPUDRESULT spudgpu_get_image_buffer_copy_offset_alignment(
    spudgpu_image image,
    uint64_t *out_alignment) {
	if (!image)
		return SPUDRESULT_GPU_INVALID_IMAGE;
	if (SPUD_STALE(image, "spudgpu_get_image_buffer_copy_offset_alignment", "image"))
		return SPUDRESULT_GPU_INVALID_IMAGE;
	if (!out_alignment)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (spudgpu___internal_format_block_bytes(image->_desc.format) == 0)
		return SPUDRESULT_GPU_INVALID_FORMAT;
	// A placed footprint's Offset, whatever the format.
	*out_alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
	return SPUD_SUCCESS;
}

void spudgpu_cmd_blit_image(
    spudgpu_command_list cmd,
    spudgpu_image src_image,
    spudgpu_image dst_image,
    const spudgpu_image_blit_desc *desc) {
	if (!cmd || !src_image || !dst_image || !desc)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_blit_image", "cmd"))
		return;
	if (SPUD_STALE(src_image, "spudgpu_cmd_blit_image", "src_image"))
		return;
	if (SPUD_STALE(dst_image, "spudgpu_cmd_blit_image", "dst_image"))
		return;

	UINT srcWidth  = desc->src_x1 - desc->src_x0;
	UINT srcHeight = desc->src_y1 - desc->src_y0;
	UINT srcDepth  = desc->src_z1 - desc->src_z0;
	UINT dstWidth  = desc->dst_x1 - desc->dst_x0;
	UINT dstHeight = desc->dst_y1 - desc->dst_y0;
	UINT dstDepth  = desc->dst_z1 - desc->dst_z0;

	if (srcWidth != dstWidth || srcHeight != dstHeight || srcDepth != dstDepth) {
		// D3D12 has no native filtered/scaling blit equivalent to vkCmdBlitImage
		// — CopyTextureRegion requires matching src/dst extents. A scaling blit
		// would need a shader-based resample pass, which isn't implemented here.
		printf("spudgpu: spudgpu_cmd_blit_image with differing src/dst extents is not supported on D3D12 (needs a shader-based resample pass)\n");
		return;
	}

	D3D12_BOX srcBox;
	srcBox.left   = desc->src_x0;
	srcBox.top    = desc->src_y0;
	srcBox.front  = desc->src_z0;
	srcBox.right  = desc->src_x1;
	srcBox.bottom = desc->src_y1;
	srcBox.back   = desc->src_z1;

	UINT layerCount = desc->src_array_layer_count ? desc->src_array_layer_count : 1;
	for (UINT layer = 0; layer < layerCount; ++layer) {
		UINT srcSubresource = D3D12CalcSubresource(
		    desc->src_mip_level, desc->src_base_array_layer + layer, 0,
		    src_image->_desc.mip_levels, src_image->_desc.array_layers);
		UINT dstSubresource = D3D12CalcSubresource(
		    desc->dst_mip_level, desc->dst_base_array_layer + layer, 0,
		    dst_image->_desc.mip_levels, dst_image->_desc.array_layers);

		CD3DX12_TEXTURE_COPY_LOCATION srcLoc(src_image->_d3d_resource.Get(), srcSubresource);
		CD3DX12_TEXTURE_COPY_LOCATION dstLoc(dst_image->_d3d_resource.Get(), dstSubresource);

		cmd->_d3d_cmd_list->CopyTextureRegion(
		    &dstLoc, desc->dst_x0, desc->dst_y0, desc->dst_z0, &srcLoc, &srcBox);
	}
}

static D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE spudgpu_d3d12_load_op_to_beginning_access(SPUDGPU_LOAD_OP op) {
	switch (op) {
	case SPUDGPU_LOAD_OP_CLEAR:
		return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
	case SPUDGPU_LOAD_OP_DONT_CARE:
		return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
	case SPUDGPU_LOAD_OP_LOAD:
	default:
		return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
	}
}

static D3D12_RENDER_PASS_ENDING_ACCESS_TYPE spudgpu_d3d12_store_op_to_ending_access(SPUDGPU_STORE_OP op) {
	switch (op) {
	case SPUDGPU_STORE_OP_DONT_CARE:
		return D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
	case SPUDGPU_STORE_OP_STORE:
	default:
		return D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
	}
}

// spudgpu_cmd_begin_rendering / spudgpu_cmd_end_rendering — dynamic
// rendering. D3D12's BeginRenderPass/EndRenderPass never needed a
// precompiled render-pass object or pipeline-compatibility contract to begin
// with (unlike classic Vulkan render passes), so this maps onto it directly:
// per-attachment beginning/ending access is passed inline, and the pipeline
// is bound separately at draw time via spudgpu_cmd_bind_pipeline. Viewport/
// scissor are not touched here — like Vulkan (VK_DYNAMIC_STATE_VIEWPORT/
// SCISSOR), that's the caller's job via spudgpu_set_viewports/
// spudgpu_set_scissor_rects, called after begin_rendering.
void spudgpu_cmd_begin_rendering(
    spudgpu_command_list cmd,
    const spudgpu_rendering_begin_desc *desc) {
	if (!cmd || !desc)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_begin_rendering", "cmd"))
		return;
	if (spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC) != SPUD_SUCCESS)
		return;
	// A layered pass needs a device created with
	// SPUDGPU_FEATURE_LAYERED_RENDERING, on every backend (spudgpu.h,
	// spudgpu_rendering_begin_desc::layer_count).
	assert((desc->layer_count == 0 || (cmd->_allocator->_device->_features & SPUDGPU_FEATURE_LAYERED_RENDERING)) &&
	       "spudgpu_cmd_begin_rendering: a layered pass on a device created without SPUDGPU_FEATURE_LAYERED_RENDERING");
	// Color attachments, a depth attachment, or both: a depth-only pass (a
	// shadow map, a depth pre-pass) has no color attachment at all, as on
	// Vulkan and Metal.
	UINT colorCount = desc->color_attachment_count;
	if (colorCount > SPUDGPU_MAX_COLOR_ATTACHMENTS)
		colorCount = SPUDGPU_MAX_COLOR_ATTACHMENTS;
	if (colorCount > 0 && !desc->color_attachments[0].image_view)
		return;
	if (colorCount == 0 && !desc->depth_attachment.image_view)
		return;

	spudgpu_image_view_d3d12 *firstView =
	    (spudgpu_image_view_d3d12 *)(colorCount > 0 ? desc->color_attachments[0].image_view : desc->depth_attachment.image_view);
	ID3D12Device *device = firstView->_image->_device->_d3d_device.Get();
	ID3D12GraphicsCommandList4 *cmdList = cmd->_d3d_cmd_list.Get();

	// Reuse this command list's persistent RTV heap, growing (recreating)
	// it only if this pass needs more color attachments than it currently
	// holds. CPU descriptor handles are cheap to overwrite in place every
	// call; it's the heap object itself that's expensive to keep
	// recreating every render pass.
	if (colorCount > 0 && colorCount > cmd->_rtv_heap_capacity) {
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heapDesc.NumDescriptors = colorCount;
		heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&cmd->_rtv_heap))))
			return;
		cmd->_rtv_heap_capacity = colorCount;
	}
	UINT rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = {};
	if (colorCount > 0)
		rtvStart = cmd->_rtv_heap->GetCPUDescriptorHandleForHeapStart();

	D3D12_RENDER_PASS_RENDER_TARGET_DESC rtDescs[SPUDGPU_MAX_COLOR_ATTACHMENTS] = {};
	for (UINT i = 0; i < colorCount; ++i) {
		const spudgpu_color_attachment_desc &src = desc->color_attachments[i];
		spudgpu_image_view_d3d12 *colorView = (spudgpu_image_view_d3d12 *)src.image_view;

		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvStart;
		rtvHandle.ptr += (SIZE_T)i * rtvInc;
		device->CreateRenderTargetView(
		    colorView->_image->_d3d_resource.Get(),
		    &colorView->_d3d_view_desc._rtv,
		    rtvHandle);

		rtDescs[i].cpuDescriptor                              = rtvHandle;
		rtDescs[i].BeginningAccess.Type                       = spudgpu_d3d12_load_op_to_beginning_access(src.load_op);
		rtDescs[i].BeginningAccess.Clear.ClearValue.Format    = colorView->_d3d_view_desc._rtv.Format;
		rtDescs[i].BeginningAccess.Clear.ClearValue.Color[0]  = src.clear_color[0];
		rtDescs[i].BeginningAccess.Clear.ClearValue.Color[1]  = src.clear_color[1];
		rtDescs[i].BeginningAccess.Clear.ClearValue.Color[2]  = src.clear_color[2];
		rtDescs[i].BeginningAccess.Clear.ClearValue.Color[3]  = src.clear_color[3];
		rtDescs[i].EndingAccess.Type                          = spudgpu_d3d12_store_op_to_ending_access(src.store_op);
	}

	// Optional depth/stencil attachment. cmd->_dsv_heap only ever needs one
	// descriptor, so it's created once on first use and reused thereafter.
	D3D12_RENDER_PASS_DEPTH_STENCIL_DESC dsDesc = {};
	bool hasDepth = (desc->depth_attachment.image_view != nullptr);

	if (hasDepth) {
		spudgpu_image_view_d3d12 *depthView =
		    (spudgpu_image_view_d3d12 *)desc->depth_attachment.image_view;

		if (!cmd->_dsv_heap) {
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
			heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			heapDesc.NumDescriptors = 1;
			heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&cmd->_dsv_heap))))
				return;
		}

		D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
		    cmd->_dsv_heap->GetCPUDescriptorHandleForHeapStart();
		device->CreateDepthStencilView(
		    depthView->_image->_d3d_resource.Get(),
		    &depthView->_d3d_view_desc._dsv,
		    dsvHandle);

		DXGI_FORMAT depthFmt = depthView->_d3d_view_desc._dsv.Format;

		dsDesc.cpuDescriptor                                                = dsvHandle;
		dsDesc.DepthBeginningAccess.Type                                    =
		    spudgpu_d3d12_load_op_to_beginning_access(desc->depth_attachment.depth_load_op);
		dsDesc.DepthBeginningAccess.Clear.ClearValue.Format                 = depthFmt;
		dsDesc.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth     = desc->depth_attachment.clear_depth;
		dsDesc.StencilBeginningAccess.Type                                  =
		    spudgpu_d3d12_load_op_to_beginning_access(desc->depth_attachment.stencil_load_op);
		dsDesc.StencilBeginningAccess.Clear.ClearValue.Format               = depthFmt;
		dsDesc.StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil =
		    (UINT8)desc->depth_attachment.clear_stencil;
		dsDesc.DepthEndingAccess.Type                                       =
		    spudgpu_d3d12_store_op_to_ending_access(desc->depth_attachment.depth_store_op);
		dsDesc.StencilEndingAccess.Type                                     =
		    spudgpu_d3d12_store_op_to_ending_access(desc->depth_attachment.stencil_store_op);
		// A format with no stencil has no stencil to load or store, and
		// D3D12 refuses any access to it but none. The stencil ops are
		// ignored there, as Vulkan ignores them.
		if (spudgpu_d3d12_format_plane_count(depthFmt) == 1) {
			dsDesc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
			dsDesc.StencilEndingAccess.Type    = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
		}
	}

	// The pass's start, written before it begins; its end after it ends.
	cmd->_end_timestamp_heap = nullptr;
	if (desc->timestamp_set) {
		cmdList->EndQuery(desc->timestamp_set->_d3d_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, desc->timestamp_begin_index);
		cmd->_end_timestamp_heap  = desc->timestamp_set->_d3d_query_heap.Get();
		cmd->_end_timestamp_index = desc->timestamp_end_index;
	}

	// D3D12 disallows ExecuteBundle (and several other calls) between
	// BeginRenderPass/EndRenderPass -- it's a documented, hard restriction of
	// the render-pass API, not a policy choice. A pass that's going to
	// execute a bundle has to fall back to the classic OMSetRenderTargets +
	// ClearRenderTargetView/ClearDepthStencilView path instead, which has no
	// such restriction. will_execute_bundles is exactly the signal Vulkan
	// already uses for its own equivalent requirement (VK_SUBPASS_CONTENTS_
	// SECONDARY_COMMAND_BUFFERS, see spudgpuvulkanrenderpass.c) -- this is
	// D3D12's side of honoring the same caller-supplied fact.
	if (desc->will_execute_bundles) {
		cmdList->OMSetRenderTargets(colorCount, colorCount > 0 ? &rtvStart : nullptr, TRUE, hasDepth ? &dsDesc.cpuDescriptor : nullptr);
		for (UINT i = 0; i < colorCount; ++i) {
			if (rtDescs[i].BeginningAccess.Type != D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
				continue;
			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvStart;
			rtvHandle.ptr += (SIZE_T)i * rtvInc;
			cmdList->ClearRenderTargetView(rtvHandle, rtDescs[i].BeginningAccess.Clear.ClearValue.Color, 0, nullptr);
		}
		if (hasDepth) {
			UINT clearFlags = 0;
			if (dsDesc.DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
				clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
			if (dsDesc.StencilBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
				clearFlags |= D3D12_CLEAR_FLAG_STENCIL;
			if (clearFlags) {
				cmdList->ClearDepthStencilView(
				    dsDesc.cpuDescriptor, (D3D12_CLEAR_FLAGS)clearFlags,
				    dsDesc.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth,
				    dsDesc.StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil,
				    0, nullptr);
			}
		}
		cmd->_using_render_pass = false;
		cmd->_rendering_active  = true;
		return;
	}

	cmdList->BeginRenderPass(
	    colorCount, colorCount > 0 ? rtDescs : nullptr,
	    hasDepth ? &dsDesc : nullptr,
	    D3D12_RENDER_PASS_FLAG_NONE);
	cmd->_using_render_pass = true;
	cmd->_rendering_active  = true;
}

void spudgpu_cmd_end_rendering(spudgpu_command_list cmd) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_end_rendering", "cmd"))
		return;
	if (cmd->_using_render_pass)
		cmd->_d3d_cmd_list->EndRenderPass();
	cmd->_rendering_active = false;
	if (cmd->_end_timestamp_heap) {
		cmd->_d3d_cmd_list->EndQuery(cmd->_end_timestamp_heap, D3D12_QUERY_TYPE_TIMESTAMP, cmd->_end_timestamp_index);
		cmd->_end_timestamp_heap = nullptr;
	}
}

// spudgpu_cmd_clear_color_attachment / spudgpu_cmd_clear_depth_attachment —
// D3D12's own implementation (every backend implements this pair natively;
// there is no shared cross-backend fallback). ClearRenderTargetView/
// ClearDepthStencilView need no active render pass at all, so a standalone
// clear here skips BeginRenderPass/EndRenderPass entirely instead of paying
// for a full (if transient) render-pass setup just to clear one view.
void spudgpu_cmd_clear_color_attachment(
    spudgpu_command_list cmd,
    spudgpu_image_view attachment,
    float r,
    float g,
    float b,
    float a,
    uint32_t width,
    uint32_t height) {
	(void)width;
	(void)height; // ClearRenderTargetView clears the view's full extent.
	if (!cmd || !attachment)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_clear_color_attachment", "cmd"))
		return;
	if (SPUD_STALE(attachment, "spudgpu_cmd_clear_color_attachment", "attachment"))
		return;

	spudgpu_image_view_d3d12 *view = (spudgpu_image_view_d3d12 *)attachment;
	ID3D12Device *device = view->_image->_device->_d3d_device.Get();
	ID3D12GraphicsCommandList *cmdList = cmd->_d3d_cmd_list.Get();

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	heapDesc.NumDescriptors = 1;
	heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
	if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap))))
		return;

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(
	    view->_image->_d3d_resource.Get(), &view->_d3d_view_desc._rtv, rtvHandle);

	float clearColor[4] = {r, g, b, a};
	cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	// rtvHeap released here — safe: ClearRenderTargetView records the CPU
	// descriptor's contents by value, same as BeginRenderPass above.
}

void spudgpu_cmd_clear_depth_attachment(
    spudgpu_command_list cmd,
    spudgpu_image_view attachment,
    bool clear_depth,
    bool clear_stencil,
    float depth,
    uint32_t stencil,
    uint32_t width,
    uint32_t height) {
	(void)width;
	(void)height; // ClearDepthStencilView clears the view's full extent.
	if (!cmd || !attachment)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_clear_depth_attachment", "cmd"))
		return;
	if (SPUD_STALE(attachment, "spudgpu_cmd_clear_depth_attachment", "attachment"))
		return;
	if (!clear_depth && !clear_stencil)
		return;

	spudgpu_image_view_d3d12 *view = (spudgpu_image_view_d3d12 *)attachment;
	ID3D12Device *device = view->_image->_device->_d3d_device.Get();
	ID3D12GraphicsCommandList *cmdList = cmd->_d3d_cmd_list.Get();

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	heapDesc.NumDescriptors = 1;
	heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
	if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap))))
		return;

	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
	device->CreateDepthStencilView(
	    view->_image->_d3d_resource.Get(), &view->_d3d_view_desc._dsv, dsvHandle);

	UINT flags = 0;
	if (clear_depth)
		flags |= D3D12_CLEAR_FLAG_DEPTH;
	if (clear_stencil)
		flags |= D3D12_CLEAR_FLAG_STENCIL;

	cmdList->ClearDepthStencilView(
	    dsvHandle, (D3D12_CLEAR_FLAGS)flags, depth, (UINT8)stencil, 0, nullptr);
}

} // extern "C"

#endif // SPUDGPU_COMPILE_D3D12_API
