//
// Carrottus GPU Metal backend - images/textures and texture views.
// Implemented and verified against real Apple hardware.
//

#if CRGPU_COMPILE_METAL_API

#include "crcore.h"
#include "crgpu.h"
#include "crgpumetal.h"
#include <Metal/Metal.h>
#include "../../../crstructbase.h"

static MTLTextureUsage crgpumetal___internal_image_texture_usage(CRGPU_IMAGE_USAGE usage) {
	MTLTextureUsage result = MTLTextureUsageUnknown;
	if (usage & CRGPU_IMAGE_USAGE_SAMPLED)
		result |= MTLTextureUsageShaderRead;
	if (usage & (CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT))
		result |= MTLTextureUsageRenderTarget;
	if (usage & CRGPU_IMAGE_USAGE_STORAGE)
		result |= (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
	// TRANSFER_SRC/TRANSFER_DST/PRESENTABLE need no MTLTextureUsage bit -
	// blit-encoder copies and drawable presentation both work regardless of
	// usage.

	// A transient attachment is only ever touched through the render pass
	// attachment mechanism itself (in-tile-memory programmable blending),
	// never through ordinary shader resource binding or a differently-
	// formatted view - crgpu_image_create already rejects combining this
	// with SAMPLED/STORAGE/etc, so RenderTarget (set above) is genuinely the
	// only usage bit that ever applies, and MTLTextureUsagePixelFormatView
	// below doesn't.
	if (usage & CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT)
		return result;

	// Always allow format-reinterpreting views. CRGPU_IMAGE_USAGE has no
	// flag for "I will need a differently-formatted (or stencil-aspect)
	// view of this image later" for a caller to opt into, and
	// -newTextureViewWithPixelFormat:... fails outright on a texture that
	// wasn't created with this usage - there is no way to add it after the
	// fact. Setting it unconditionally is the only choice that doesn't
	// silently foreclose crgpu_image_view_create ever reinterpreting this
	// image's format down the line; the cost is only a possible loss of a
	// GPU-internal lossless compression optimization on some formats, which
	// CRGPU never promises anywhere in its contract.
	result |= MTLTextureUsagePixelFormatView;
	return result;
}

static MTLTextureType crgpumetal___internal_image_texture_type(
    CRGPU_IMAGE_TYPE type,
    uint32_t array_layers) {
	if (type & CRGPU_IMAGE_TYPE_3D)
		return MTLTextureType3D; // Metal has no 3D texture arrays.
	if (type & CRGPU_IMAGE_TYPE_1D)
		return (array_layers > 1) ? MTLTextureType1DArray : MTLTextureType1D;
	// Default/CRGPU_IMAGE_TYPE_2D. Cube maps have no dedicated
	// CRGPU_IMAGE_TYPE value - like Vulkan, they're a 2D array of 6 (or
	// 6*N) layers, reinterpreted as CRGPU_IMAGE_VIEW_TYPE_CUBE via
	// crgpu_image_view_create - so the base texture is created as a plain
	// (array) 2D texture regardless, cube_capable or not: a 2D array
	// texture can be viewed as a cube or a cube array, and as single 2D
	// layers to render its faces.
	return (array_layers > 1) ? MTLTextureType2DArray : MTLTextureType2D;
}

CR_RESULT crgpumetal___check_image_desc(const crgpu_image_desc *desc) {
	if (!desc->width || !desc->height || !desc->depth || !desc->array_layers || !desc->mip_levels)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->usage == CRGPU_IMAGE_USAGE_NONE)
		return CR_RESULT_GPU_INVALID_IMAGE_USAGE;
	// CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT's contract (see crgpu.h) is
	// enforced identically on every backend, not just the ones that act on
	// it, so an invalid desc doesn't silently behave differently depending
	// on which backend happens to be compiled in.
	if (desc->usage & CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT) {
		if (!(desc->usage & (CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT)))
			return CR_RESULT_GPU_INVALID_IMAGE_USAGE;
		if (desc->usage & (CRGPU_IMAGE_USAGE_SAMPLED | CRGPU_IMAGE_USAGE_STORAGE |
		                   CRGPU_IMAGE_USAGE_TRANSFER_SRC | CRGPU_IMAGE_USAGE_TRANSFER_DST |
		                   CRGPU_IMAGE_USAGE_PRESENTABLE))
			return CR_RESULT_GPU_INVALID_IMAGE_USAGE;
	}
	// crgpu_image_desc::cube_capable's contract (see crgpu.h) is the
	// same on every backend, though only Vulkan acts on it at creation.
	if (desc->cube_capable) {
		if (desc->type != CRGPU_IMAGE_TYPE_2D)
			return CR_RESULT_GPU_INVALID_IMAGE_TYPE;
		if (desc->width != desc->height || desc->array_layers % 6 != 0)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
	}
	// The format table (crgpumetalformat.m) says why a format has none.
	if (crgpumetal___format_info(desc->format)->pixel == MTLPixelFormatInvalid)
		return CR_RESULT_GPU_INVALID_FORMAT;
	return CR_SUCCESS;
}

MTLTextureDescriptor *crgpumetal___new_texture_descriptor(const crgpu_image_desc *desc) {
	MTLTextureDescriptor *texture_desc = [[MTLTextureDescriptor alloc] init];
	texture_desc.textureType           = crgpumetal___internal_image_texture_type(desc->type, desc->array_layers);
	texture_desc.pixelFormat           = crgpumetal___format_info(desc->format)->pixel;
	texture_desc.width                 = desc->width;
	texture_desc.height                = desc->height;
	texture_desc.depth                 = (texture_desc.textureType == MTLTextureType3D) ? desc->depth : 1;
	texture_desc.arrayLength           = (texture_desc.textureType == MTLTextureType3D) ? 1 : desc->array_layers;
	texture_desc.mipmapLevelCount      = desc->mip_levels;
	texture_desc.usage                 = crgpumetal___internal_image_texture_usage(desc->usage);
	// An image has no memory kind (see crgpu_image_desc): it is in
	// the device's memory and never mapped, which is Private storage.
	// A transient attachment has no memory at all outside its pass.
	if (desc->usage & CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT) {
		texture_desc.storageMode = MTLStorageModeMemoryless;
	} else {
		texture_desc.storageMode = MTLStorageModePrivate;
	}

	return texture_desc;
}

CR_RESULT crgpu_image_create(
    crgpu_device device,
    const crgpu_image_desc *desc,
    crgpu_image *out_image) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_image_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_IMAGE_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_image)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT desc_result = crgpumetal___check_image_desc(desc);
	if (desc_result != CR_SUCCESS)
		return desc_result;

	crgpu_device_metal *device_metal = (crgpu_device_metal *)device;
	CR_RESULT sr                      = CR_SUCCESS;

	crgpu_image_metal *image_metal = CR_NEW_ZEROED(&device_metal->head.allocator, crgpu_image_metal);
	if (!image_metal) {
		sr = CR_RESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	image_metal->head.allocator = device_metal->head.allocator;
	CR_BIRTH_IN(image_metal, &device_metal->_quarantine);
	image_metal->_parent_device = device_metal;
	image_metal->_desc          = *desc;

	{
		MTLTextureDescriptor *texture_desc = crgpumetal___new_texture_descriptor(desc);
		image_metal->_texture_mtl          = [device_metal->_device_mtl newTextureWithDescriptor:texture_desc];
		[texture_desc release];
	}
	if (!image_metal->_texture_mtl) {
		sr = CR_RESULT_API_SPECIFIC_FAILURE;
		goto failedattempt;
	}
	// From here crgpu_image_destroy frees it, and takes the count back.
	CRGPU___LIVE_ADD(&device_metal->_live, CRGPU___OBJECT_IMAGE);

	if (cr_debug_name_set(image_metal, desc->debug_name) != CR_SUCCESS) {
		crgpu_image_destroy((crgpu_image)image_metal);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	image_metal->_desc.debug_name = image_metal->head.debug_name;

	*out_image = (crgpu_image)image_metal;

	return sr;

failedattempt:
	CR_DELETE(&device_metal->head.allocator, crgpu_image_metal, image_metal);
	return sr;
}

void crgpu_image_destroy(crgpu_image image) {
	crgpu_image_metal *image_metal = (crgpu_image_metal *)image;
	if (image_metal) {
		if (CR_STALE(image, "crgpu_image_destroy", "image"))
			return;
		if (image_metal->_texture_mtl) {
			[image_metal->_texture_mtl release];
		}
#if _DEBUG
		if (image_metal->_heap)
			image_metal->_heap->_placed_count--;
#endif
		CRGPU___LIVE_REMOVE(&image_metal->_parent_device->_live, CRGPU___OBJECT_IMAGE);
		CR_KILL(image);
		const CR_ALLOCATOR allocator = image_metal->head.allocator;
		cr_string_free(&allocator, image_metal->head.debug_name);
		CR_RETIRE(CR_QUARANTINE_OF(image_metal), &allocator, crgpu_image_metal, image_metal);
	}
}

CR_RESULT crgpu_image_get_desc(
    crgpu_image image,
    crgpu_image_desc *out_desc) {
	crgpu_image_metal *image_metal = (crgpu_image_metal *)image;
	if (!image_metal)
		return CR_RESULT_GPU_INVALID_IMAGE;
	if (CR_STALE(image, "crgpu_image_get_desc", "image"))
		return CR_RESULT_GPU_INVALID_IMAGE;
	if (!out_desc)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = image_metal->_desc;
	out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_IMAGE_DESC);
	return CR_SUCCESS;
}

static MTLTextureType crgpumetal___internal_image_view_texture_type(
    CRGPU_IMAGE_VIEW_TYPE type,
    MTLTextureType parent_type) {
	switch (type) {
	case CRGPU_IMAGE_VIEW_TYPE_1D:
		return MTLTextureType1D;
	case CRGPU_IMAGE_VIEW_TYPE_1D_ARRAY:
		return MTLTextureType1DArray;
	case CRGPU_IMAGE_VIEW_TYPE_2D:
		return MTLTextureType2D;
	case CRGPU_IMAGE_VIEW_TYPE_2D_ARRAY:
		return MTLTextureType2DArray;
	case CRGPU_IMAGE_VIEW_TYPE_3D:
		return MTLTextureType3D;
	// Cube maps have no dedicated CRGPU_IMAGE_TYPE at the base-image level
	// (see crgpumetal___internal_image_texture_type) - this is where that
	// reinterpretation actually happens, same as Vulkan's
	// VK_IMAGE_VIEW_TYPE_CUBE over a plain 2D-array VkImage.
	case CRGPU_IMAGE_VIEW_TYPE_CUBE:
		return MTLTextureTypeCube;
	case CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY:
		return MTLTextureTypeCubeArray;
	default:
		return parent_type; // CRGPU_IMAGE_VIEW_TYPE_NONE: same shape as parent.
	}
}

CR_RESULT crgpu_image_view_create(
    crgpu_image image,
    const crgpu_image_view_desc *desc,
    crgpu_image_view *out_image_view) {
	if (!image)
		return CR_RESULT_GPU_INVALID_IMAGE;
	if (CR_STALE(image, "crgpu_image_view_create", "image"))
		return CR_RESULT_GPU_INVALID_IMAGE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_image_view)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	crgpu_image_metal *parent_image_metal = (crgpu_image_metal *)image;
	// The cube view rules of crgpu.h, the same on every backend.
	if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE || desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
		if (!parent_image_metal->_desc.cube_capable)
			return CR_RESULT_GPU_INVALID_IMAGE_VIEW_TYPE;
		if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY &&
		    !(parent_image_metal->_parent_device->_features & CRGPU_FEATURE_CUBE_ARRAY_VIEWS))
			return CR_RESULT_GPU_INVALID_IMAGE_VIEW_TYPE;
		if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE &&
		    (desc->subresource_range.base_array_layer != 0 || desc->subresource_range.array_layer_count != 6))
			return CR_RESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;
		if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY &&
		    (desc->subresource_range.array_layer_count == 0 || desc->subresource_range.array_layer_count % 6 != 0))
			return CR_RESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;
	}
	// The aspect rule of crgpu.h, the same on every backend: the format's
	// aspects are the format table's.
	const crgpumetal_format_info *format_info = crgpumetal___format_info(parent_image_metal->_desc.format);
	if (desc->subresource_range.aspect_mask == CRGPU_IMAGE_ASPECT_NONE)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->subresource_range.aspect_mask & ~format_info->aspects)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (!parent_image_metal->_texture_mtl)
		return CR_RESULT_GPU_INVALID_IMAGE;

	CR_RESULT sr = CR_SUCCESS;

	crgpu_image_view_metal *view_metal = CR_NEW_ZEROED(&parent_image_metal->head.allocator, crgpu_image_view_metal);
	if (!view_metal) {
		sr = CR_RESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	view_metal->head.allocator     = parent_image_metal->head.allocator;
	CR_BIRTH_IN(view_metal, &parent_image_metal->_parent_device->_quarantine);
	view_metal->_parent_image      = parent_image_metal;
	view_metal->_parent_device     = parent_image_metal->_parent_device;
	view_metal->_desc              = *desc;
	view_metal->_desc.parent_image = image; // Authoritative - same reasoning as crgpu_buffer_view_metal.

	{
		MTLTextureType view_type = crgpumetal___internal_image_view_texture_type(desc->type, parent_image_metal->_texture_mtl.textureType);
		NSRange levels           = NSMakeRange((NSUInteger)desc->subresource_range.base_mip_level, (NSUInteger)desc->subresource_range.mip_level_count);
		NSRange slices           = NSMakeRange((NSUInteger)desc->subresource_range.base_array_layer, (NSUInteger)desc->subresource_range.array_layer_count);

		// The parent's pixel format, except for a view of the stencil alone
		// (CRGPU_IMAGE_ASPECT_STENCIL), which is what a shader reads the
		// stencil through: Metal reaches the stencil of a depth and stencil
		// texture as a view in the matching stencil format, the format
		// table's stencil_view. The parent was created with
		// MTLTextureUsagePixelFormatView for this (see crgpu_image_create).
		// crgpu_image_view_desc has no format override beyond that.
		MTLPixelFormat view_format = parent_image_metal->_texture_mtl.pixelFormat;
		if (desc->subresource_range.aspect_mask == CRGPU_IMAGE_ASPECT_STENCIL && format_info->stencil_view != MTLPixelFormatInvalid)
			view_format = format_info->stencil_view;
		view_metal->_texture_view_mtl = [parent_image_metal->_texture_mtl newTextureViewWithPixelFormat:view_format
		                                                                                    textureType:view_type
		                                                                                         levels:levels
		                                                                                         slices:slices];
	}
	if (!view_metal->_texture_view_mtl) {
		sr = CR_RESULT_API_SPECIFIC_FAILURE;
		goto failedattempt;
	}

	CRGPU___LIVE_ADD(&view_metal->_parent_device->_live, CRGPU___OBJECT_IMAGE_VIEW);
	*out_image_view = (crgpu_image_view)view_metal;

	return sr;

failedattempt:
	CR_DELETE(&parent_image_metal->head.allocator, crgpu_image_view_metal, view_metal);
	return sr;
}

void crgpu_image_view_destroy(crgpu_image_view image_view) {
	crgpu_image_view_metal *view_metal = (crgpu_image_view_metal *)image_view;
	if (view_metal) {
		if (CR_STALE(image_view, "crgpu_image_view_destroy", "image_view"))
			return;
		if (view_metal->_texture_view_mtl) {
			[view_metal->_texture_view_mtl release];
		}
		CRGPU___LIVE_REMOVE(&view_metal->_parent_device->_live, CRGPU___OBJECT_IMAGE_VIEW);
		CR_KILL(image_view);
		const CR_ALLOCATOR allocator = view_metal->head.allocator;
		cr_string_free(&allocator, view_metal->head.debug_name);
		CR_RETIRE(CR_QUARANTINE_OF(view_metal), &allocator, crgpu_image_view_metal, view_metal);
	}
}

CR_RESULT crgpu_image_view_get_desc(
    crgpu_image_view image_view,
    crgpu_image_view_desc *out_desc) {
	crgpu_image_view_metal *view_metal = (crgpu_image_view_metal *)image_view;
	if (!view_metal)
		return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
	if (CR_STALE(image_view, "crgpu_image_view_get_desc", "image_view"))
		return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
	if (!out_desc)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = view_metal->_desc;
	out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC);
	return CR_SUCCESS;
}

#endif // CRGPU_COMPILE_METAL_API
