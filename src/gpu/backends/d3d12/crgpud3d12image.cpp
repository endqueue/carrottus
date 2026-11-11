
#if CRGPU_COMPILE_D3D12_API

#include "crgpud3d12.hpp"
#include "../../../crstructbase.h"

// The aspects [format] has (CRGPU_IMAGE_ASPECT). The same table in every
// backend: it decides which views crgpu_image_view_create refuses.
static uint64_t crgpu_d3d12_format_aspects(CRGPU_FORMAT format) {
	switch (format) {
	case CRGPU_FORMAT_D24_UNORM_S8_UINT:
	case CRGPU_FORMAT_D32_FLOAT_S8X24_UINT:
		return CRGPU_IMAGE_ASPECT_DEPTH | CRGPU_IMAGE_ASPECT_STENCIL;
	case CRGPU_FORMAT_D16_UNORM:
	case CRGPU_FORMAT_D32_FLOAT:
		return CRGPU_IMAGE_ASPECT_DEPTH;
	default:
		return CRGPU_IMAGE_ASPECT_COLOR;
	}
}

// The format a shader reads [aspect_mask] of an image of [format] in. A
// depth format is not a shader resource format: each has a readable one in
// its family, and a second for the stencil.
static DXGI_FORMAT crgpu_d3d12_srv_format(CRGPU_FORMAT format, uint64_t aspect_mask) {
	const bool stencil = aspect_mask == CRGPU_IMAGE_ASPECT_STENCIL;
	switch (format) {
	case CRGPU_FORMAT_D16_UNORM:
		return DXGI_FORMAT_R16_UNORM;
	case CRGPU_FORMAT_D32_FLOAT:
		return DXGI_FORMAT_R32_FLOAT;
	case CRGPU_FORMAT_D24_UNORM_S8_UINT:
		return stencil ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case CRGPU_FORMAT_D32_FLOAT_S8X24_UINT:
		return stencil ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	default:
		return crgpu_d3d12_get_dxgi_format(format);
	}
}

// The typeless format of a depth format's family, or [format] itself for any
// other. See crgpu_d3d12_create_resource_desc_from_image.
static DXGI_FORMAT crgpu_d3d12_depth_typeless_format(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_D16_UNORM:
		return DXGI_FORMAT_R16_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT:
		return DXGI_FORMAT_R32_TYPELESS;
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
		return DXGI_FORMAT_R24G8_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		return DXGI_FORMAT_R32G8X24_TYPELESS;
	default:
		return format;
	}
}

// The view type D3D12 is given for [desc]. A D3D12 TEXTURE2D view has no
// array slice and always sees layer 0, so a 2D view of any other layer is
// made as a 2D array view of its one layer, which Vulkan's and Metal's 2D
// views of that layer are. Only shader resource views have cube dimensions
// ([has_cube]); for a render target, depth or storage view a cube or cube
// array is the 2D array of its layers, every face, as on Vulkan.
static CRGPU_IMAGE_VIEW_TYPE crgpu_d3d12_view_type(const crgpu_image_view_desc *desc, bool has_cube) {
	if (desc->type == CRGPU_IMAGE_VIEW_TYPE_2D && desc->subresource_range.base_array_layer != 0)
		return CRGPU_IMAGE_VIEW_TYPE_2D_ARRAY;
	if (!has_cube && (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE || desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY))
		return CRGPU_IMAGE_VIEW_TYPE_2D_ARRAY;
	return desc->type;
}

// Fills the unordered access view desc of [desc] over an image of [format].
static void crgpu_d3d12_fill_uav(
    D3D12_UNORDERED_ACCESS_VIEW_DESC &uav,
    CRGPU_FORMAT format,
    const crgpu_image_view_desc *desc) {
	const crgpu_image_view_desc_subresource_range &sr = desc->subresource_range;
	uav.Format = crgpu_d3d12_get_dxgi_format(format);
	switch (crgpu_d3d12_view_type(desc, false)) {
	case CRGPU_IMAGE_VIEW_TYPE_1D:
		uav.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE1D;
		uav.Texture1D.MipSlice = (UINT)sr.base_mip_level;
		break;
	case CRGPU_IMAGE_VIEW_TYPE_1D_ARRAY:
		uav.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
		uav.Texture1DArray.MipSlice        = (UINT)sr.base_mip_level;
		uav.Texture1DArray.FirstArraySlice = (UINT)sr.base_array_layer;
		uav.Texture1DArray.ArraySize       = (UINT)sr.array_layer_count;
		break;
	case CRGPU_IMAGE_VIEW_TYPE_3D:
		uav.ViewDimension         = D3D12_UAV_DIMENSION_TEXTURE3D;
		uav.Texture3D.MipSlice    = (UINT)sr.base_mip_level;
		uav.Texture3D.FirstWSlice = (UINT)sr.base_array_layer;
		uav.Texture3D.WSize       = (UINT)sr.array_layer_count;
		break;
	case CRGPU_IMAGE_VIEW_TYPE_2D_ARRAY:
		uav.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
		uav.Texture2DArray.MipSlice        = (UINT)sr.base_mip_level;
		uav.Texture2DArray.FirstArraySlice = (UINT)sr.base_array_layer;
		uav.Texture2DArray.ArraySize       = (UINT)sr.array_layer_count;
		break;
	default:
		uav.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
		uav.Texture2D.MipSlice = (UINT)sr.base_mip_level;
		break;
	}
}

// Fills the shader resource view desc of [desc] over an image of [format]:
// the format and the plane are the ones its aspect is read through.
static void crgpu_d3d12_fill_srv(
    D3D12_SHADER_RESOURCE_VIEW_DESC &srv,
    CRGPU_FORMAT format,
    const crgpu_image_view_desc *desc) {
	const crgpu_image_view_desc_subresource_range &sr = desc->subresource_range;
	const bool stencil                                  = sr.aspect_mask == CRGPU_IMAGE_ASPECT_STENCIL;
	const UINT plane                                    = stencil ? 1 : 0;
	srv.Format                  = crgpu_d3d12_srv_format(format, sr.aspect_mask);
	// The stencil is the second component of its view format. It is
	// handed to the shader as the first, where crgpu.h says a stencil view
	// is read.
	srv.Shader4ComponentMapping = stencil ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
	                                            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
	                                            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
	                                            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
	                                            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1)
	                                      : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	switch (crgpu_d3d12_view_type(desc, true)) {
	case CRGPU_IMAGE_VIEW_TYPE_1D:
		srv.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE1D;
		srv.Texture1D.MostDetailedMip     = (UINT)sr.base_mip_level;
		srv.Texture1D.MipLevels           = (UINT)sr.mip_level_count;
		srv.Texture1D.ResourceMinLODClamp = 0.0f;
		break;
	case CRGPU_IMAGE_VIEW_TYPE_1D_ARRAY:
		srv.ViewDimension                      = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
		srv.Texture1DArray.MostDetailedMip     = (UINT)sr.base_mip_level;
		srv.Texture1DArray.MipLevels           = (UINT)sr.mip_level_count;
		srv.Texture1DArray.FirstArraySlice     = (UINT)sr.base_array_layer;
		srv.Texture1DArray.ArraySize           = (UINT)sr.array_layer_count;
		srv.Texture1DArray.ResourceMinLODClamp = 0.0f;
		break;
	case CRGPU_IMAGE_VIEW_TYPE_3D:
		srv.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE3D;
		srv.Texture3D.MostDetailedMip     = (UINT)sr.base_mip_level;
		srv.Texture3D.MipLevels           = (UINT)sr.mip_level_count;
		srv.Texture3D.ResourceMinLODClamp = 0.0f;
		break;
	case CRGPU_IMAGE_VIEW_TYPE_CUBE:
		srv.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBE;
		srv.TextureCube.MostDetailedMip     = (UINT)sr.base_mip_level;
		srv.TextureCube.MipLevels           = (UINT)sr.mip_level_count;
		srv.TextureCube.ResourceMinLODClamp = 0.0f;
		break;
	case CRGPU_IMAGE_VIEW_TYPE_2D_ARRAY:
		srv.ViewDimension                       = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		srv.Texture2DArray.MostDetailedMip      = (UINT)sr.base_mip_level;
		srv.Texture2DArray.MipLevels            = (UINT)sr.mip_level_count;
		srv.Texture2DArray.FirstArraySlice      = (UINT)sr.base_array_layer;
		srv.Texture2DArray.ArraySize            = (UINT)sr.array_layer_count;
		srv.Texture2DArray.PlaneSlice           = plane;
		srv.Texture2DArray.ResourceMinLODClamp  = 0.0f;
		break;
	case CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY:
		srv.ViewDimension                          = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
		srv.TextureCubeArray.MostDetailedMip       = (UINT)sr.base_mip_level;
		srv.TextureCubeArray.MipLevels             = (UINT)sr.mip_level_count;
		srv.TextureCubeArray.First2DArrayFace      = (UINT)sr.base_array_layer;
		srv.TextureCubeArray.NumCubes              = (UINT)(sr.array_layer_count / 6);
		srv.TextureCubeArray.ResourceMinLODClamp   = 0.0f;
		break;
	default:
		srv.ViewDimension                  = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MostDetailedMip      = (UINT)sr.base_mip_level;
		srv.Texture2D.MipLevels            = (UINT)sr.mip_level_count;
		srv.Texture2D.PlaneSlice           = plane;
		srv.Texture2D.ResourceMinLODClamp  = 0.0f;
		break;
	}
}

CR_RESULT crgpu_d3d12_create_resource_desc_from_image(
    const crgpu_image_desc *desc,
    D3D12_RESOURCE_DESC *out_d3d_resource_desc) {
	if (!desc)
		return CR_RESULT_NULL_DESC;
	if (!out_d3d_resource_desc)
		return CR_SUCCESS;
	D3D12_RESOURCE_FLAGS d3dResourceFlags =
	    crgpu_d3d12_get_image_resource_flags(desc->usage);
	DXGI_FORMAT dxgiFmt = crgpu_d3d12_get_dxgi_format(desc->format);
	// A depth image that a shader also reads is created in the typeless
	// format of its family, so that both a depth stencil view (in the depth
	// format) and a shader resource view (in the family's readable format)
	// can be made of it. The clear value and the views keep the typed one.
	if (desc->usage & CRGPU_IMAGE_USAGE_SAMPLED)
		dxgiFmt = crgpu_d3d12_depth_typeless_format(dxgiFmt);
	CD3DX12_RESOURCE_DESC result;
	switch (desc->type) {
	case CRGPU_IMAGE_TYPE_1D:
		result = CD3DX12_RESOURCE_DESC::Tex1D(
		    dxgiFmt, desc->width,
		    (UINT16)desc->array_layers, (UINT16)desc->mip_levels,
		    d3dResourceFlags);
		break;
	case CRGPU_IMAGE_TYPE_2D:
		result = CD3DX12_RESOURCE_DESC::Tex2D(
		    dxgiFmt, desc->width, desc->height,
		    (UINT16)desc->array_layers, (UINT16)desc->mip_levels,
		    1, 0, d3dResourceFlags);
		break;
	case CRGPU_IMAGE_TYPE_3D:
		result = CD3DX12_RESOURCE_DESC::Tex3D(
		    dxgiFmt, desc->width, desc->height, (UINT16)desc->depth,
		    (UINT16)desc->mip_levels, d3dResourceFlags);
		break;
	default:
		return CR_RESULT_GPU_INVALID_IMAGE_TYPE;
	}
	*out_d3d_resource_desc = result;
	return CR_SUCCESS;
}

static D3D12_CLEAR_VALUE
crgpu_d3d12_get_clear_value(const CRGPU_CLEAR_VALUE *c) {
	D3D12_CLEAR_VALUE result = {};
	result.Format            = crgpu_d3d12_get_dxgi_format(c->format);
	result.DepthStencil.Depth   = c->depth_stencil.depth;
	result.DepthStencil.Stencil = c->depth_stencil.stencil;
	memcpy(&result.Color, &c->color, sizeof(float) * 4);
	return result;
}

CR_RESULT crgpu_d3d12_check_image_desc(const crgpu_image_desc *desc) {
	if (!desc->width || !desc->height || !desc->depth || !desc->array_layers || !desc->mip_levels)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->usage == CRGPU_IMAGE_USAGE_NONE)
		return CR_RESULT_GPU_INVALID_IMAGE_USAGE;
	// CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT's contract (see crgpu.h) is
	// enforced identically on every backend, not just the ones that act on
	// it.
	if (desc->usage & CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT) {
		if (!(desc->usage & (CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT)))
			return CR_RESULT_GPU_INVALID_IMAGE_USAGE;
		if (desc->usage & (CRGPU_IMAGE_USAGE_SAMPLED | CRGPU_IMAGE_USAGE_STORAGE |
		                   CRGPU_IMAGE_USAGE_TRANSFER_SRC | CRGPU_IMAGE_USAGE_TRANSFER_DST |
		                   CRGPU_IMAGE_USAGE_PRESENTABLE))
			return CR_RESULT_GPU_INVALID_IMAGE_USAGE;
	}
	// crgpu_image_desc::cube_capable's contract (see crgpu.h) is the
	// same on every backend, though only Vulkan acts on it at creation: a
	// D3D12 cube is a view of a Texture2D with six array slices.
	if (desc->cube_capable) {
		if (desc->type != CRGPU_IMAGE_TYPE_2D)
			return CR_RESULT_GPU_INVALID_IMAGE_TYPE;
		if (desc->width != desc->height || desc->array_layers % 6 != 0)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
	}
	if (desc->format == CRGPU_FORMAT_UNKNOWN)
		return CR_RESULT_GPU_INVALID_FORMAT;
	return CR_SUCCESS;
}

bool crgpu_d3d12_get_image_clear_value(
    const crgpu_image_desc *desc,
    D3D12_CLEAR_VALUE *out_clear_value) {
	if (!(desc->usage & (CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT)))
		return false;
	*out_clear_value = crgpu_d3d12_get_clear_value(&desc->clear_value);
	// D3D12 requires the clear value format to match the resource format exactly.
	out_clear_value->Format = crgpu_d3d12_get_dxgi_format(desc->format);
	return true;
}

extern "C" {

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
	const CR_RESULT descResult = crgpu_d3d12_check_image_desc(desc);
	if (descResult != CR_SUCCESS)
		return descResult;

	crgpu_image_d3d12 *pResult =
	    CR_NEW(&device->head.allocator, crgpu_image_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult                      = new (pResult) crgpu_image_d3d12();
	pResult->head.allocator      = device->head.allocator;
	CR_BIRTH_IN(pResult, &device->_quarantine);
	pResult->_subresource_states = eq::array<D3D12_RESOURCE_STATES>(&device->_eq_allocator);
	pResult->_device        = device;
	pResult->_desc          = *desc;

	CR_RESULT sr = crgpu_d3d12_create_resource_desc_from_image(
	    desc, &pResult->_d3d_resource_desc);
	if (sr != CR_SUCCESS) {
		pResult->~crgpu_image_d3d12();
		CR_DELETE(&device->head.allocator, crgpu_image_d3d12, pResult);
		return sr;
	}

	// An image is always in the device's memory (see crgpu_image_desc).
	D3D12_HEAP_PROPERTIES d3dHeapProperties =
	    crgpu_d3d12_get_heap_properties_from_memory_kind(CRGPU_MEMORY_KIND_DEVICE);
	D3D12_HEAP_FLAGS d3dHeapFlags = crgpu_d3d12_get_heap_flags();
	if (desc->usage & CRGPU_IMAGE_USAGE_PRESENTABLE)
		d3dHeapFlags |= D3D12_HEAP_FLAG_ALLOW_DISPLAY;
	D3D12_RESOURCE_STATES d3dInitialState =
	    crgpu_d3d12_get_initial_image_state(desc->usage);

	D3D12_CLEAR_VALUE d3dClearValue = {};
	const bool hasOptimizedClearValue = crgpu_d3d12_get_image_clear_value(desc, &d3dClearValue);

	HRESULT hr = device->_d3d_device->CreateCommittedResource1(
	    &d3dHeapProperties, d3dHeapFlags, &pResult->_d3d_resource_desc,
	    d3dInitialState, hasOptimizedClearValue ? &d3dClearValue : nullptr,
	    nullptr, IID_PPV_ARGS(&pResult->_d3d_resource));
	if (FAILED(hr)) {
		printf("crgpu: CreateCommittedResource1 failed (%ux%u, fmt=%u, flags=0x%x, heapFlags=0x%x, initialState=0x%x): hr=0x%08lx\n",
		    desc->width, desc->height, (unsigned)pResult->_d3d_resource_desc.Format, (unsigned)pResult->_d3d_resource_desc.Flags,
		    (unsigned)d3dHeapFlags, (unsigned)d3dInitialState, (unsigned long)hr);
		pResult->~crgpu_image_d3d12();
		CR_DELETE(&device->head.allocator, crgpu_image_d3d12, pResult);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	pResult->_current_state = d3dInitialState;

	// From here crgpu_image_destroy frees it, and takes the count back.
	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_IMAGE);
	if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
		crgpu_image_destroy(pResult);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	pResult->_desc.debug_name = pResult->head.debug_name;

	*out_image = pResult;

	return CR_SUCCESS;
}
void crgpu_image_destroy(crgpu_image image) {
    if (!image) return;
    if (CR_STALE(image, "crgpu_image_destroy", "image")) return;
#if _DEBUG
    if (image->_heap)
        image->_heap->_placed_count--;
#endif
    CRGPU___LIVE_REMOVE(&image->_device->_live, CRGPU___OBJECT_IMAGE);
    CR_KILL(image);
    const CR_ALLOCATOR allocator = image->head.allocator;
    cr_string_free(&allocator, image->head.debug_name);
    CR_QUARANTINE_KEEP(quarantine, image);
    image->~crgpu_image_d3d12();
    CR_RETIRE(quarantine, &allocator, crgpu_image_d3d12, image);
}
CR_RESULT crgpu_image_get_desc(
	crgpu_image image,
	crgpu_image_desc *out_desc) {
    if (!image) return CR_RESULT_GPU_INVALID_IMAGE;
    if (CR_STALE(image, "crgpu_image_get_desc", "image")) return CR_RESULT_GPU_INVALID_IMAGE;
    if (!out_desc) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    *out_desc = image->_desc;
    out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_IMAGE_DESC);
    return CR_SUCCESS;
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
	// The cube view rules of crgpu.h, the same on every backend. The
	// range rule for a single cube is this API's: a TEXTURECUBE view has no
	// first face to give, so it is always array slices 0 to 5.
	if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE || desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
		if (!image->_desc.cube_capable)
			return CR_RESULT_GPU_INVALID_IMAGE_VIEW_TYPE;
		if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY && !(image->_device->_features & CRGPU_FEATURE_CUBE_ARRAY_VIEWS))
			return CR_RESULT_GPU_INVALID_IMAGE_VIEW_TYPE;
		if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE &&
		    (desc->subresource_range.base_array_layer != 0 || desc->subresource_range.array_layer_count != 6))
			return CR_RESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;
		if (desc->type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY &&
		    (desc->subresource_range.array_layer_count == 0 || desc->subresource_range.array_layer_count % 6 != 0))
			return CR_RESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE;
	}
	// The aspect rule of crgpu.h, the same on every backend.
	if (desc->subresource_range.aspect_mask == CRGPU_IMAGE_ASPECT_NONE)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->subresource_range.aspect_mask & ~crgpu_d3d12_format_aspects(image->_desc.format))
		return CR_RESULT_DESC_INVALID_PARAMETERS;

	crgpu_image_view_d3d12 *pResult = CR_NEW_ZEROED(
	    &image->_device->head.allocator, crgpu_image_view_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult->head.allocator = image->_device->head.allocator;
	CR_BIRTH_IN(pResult, &image->_device->_quarantine);
	pResult->_image = image;
	pResult->_desc  = *desc;
	pResult->_desc.parent_image = image;

	DXGI_FORMAT dxgiFmt = crgpu_d3d12_get_dxgi_format(image->_desc.format);
	const crgpu_image_view_desc_subresource_range &sr = desc->subresource_range;
	CRGPU_IMAGE_USAGE usage = image->_desc.usage;

	if (usage & CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT) {
		D3D12_DEPTH_STENCIL_VIEW_DESC &dsv = pResult->_d3d_view_desc._dsv;
		dsv.Format = dxgiFmt;
		switch (crgpu_d3d12_view_type(desc, false)) {
		case CRGPU_IMAGE_VIEW_TYPE_1D:
			dsv.ViewDimension      = D3D12_DSV_DIMENSION_TEXTURE1D;
			dsv.Texture1D.MipSlice = (UINT)sr.base_mip_level;
			break;
		case CRGPU_IMAGE_VIEW_TYPE_1D_ARRAY:
			dsv.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
			dsv.Texture1DArray.MipSlice        = (UINT)sr.base_mip_level;
			dsv.Texture1DArray.FirstArraySlice = (UINT)sr.base_array_layer;
			dsv.Texture1DArray.ArraySize       = (UINT)sr.array_layer_count;
			break;
		case CRGPU_IMAGE_VIEW_TYPE_2D_ARRAY:
			dsv.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
			dsv.Texture2DArray.MipSlice        = (UINT)sr.base_mip_level;
			dsv.Texture2DArray.FirstArraySlice = (UINT)sr.base_array_layer;
			dsv.Texture2DArray.ArraySize       = (UINT)sr.array_layer_count;
			break;
		default:
			dsv.ViewDimension      = D3D12_DSV_DIMENSION_TEXTURE2D;
			dsv.Texture2D.MipSlice = (UINT)sr.base_mip_level;
			break;
		}
	} else if (usage & CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT) {
		D3D12_RENDER_TARGET_VIEW_DESC &rtv = pResult->_d3d_view_desc._rtv;
		rtv.Format = dxgiFmt;
		switch (crgpu_d3d12_view_type(desc, false)) {
		case CRGPU_IMAGE_VIEW_TYPE_1D:
			rtv.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE1D;
			rtv.Texture1D.MipSlice = (UINT)sr.base_mip_level;
			break;
		case CRGPU_IMAGE_VIEW_TYPE_1D_ARRAY:
			rtv.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
			rtv.Texture1DArray.MipSlice        = (UINT)sr.base_mip_level;
			rtv.Texture1DArray.FirstArraySlice = (UINT)sr.base_array_layer;
			rtv.Texture1DArray.ArraySize       = (UINT)sr.array_layer_count;
			break;
		case CRGPU_IMAGE_VIEW_TYPE_3D:
			rtv.ViewDimension         = D3D12_RTV_DIMENSION_TEXTURE3D;
			rtv.Texture3D.MipSlice    = (UINT)sr.base_mip_level;
			rtv.Texture3D.FirstWSlice = (UINT)sr.base_array_layer;
			rtv.Texture3D.WSize       = (UINT)sr.array_layer_count;
			break;
		case CRGPU_IMAGE_VIEW_TYPE_2D_ARRAY:
			rtv.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			rtv.Texture2DArray.MipSlice        = (UINT)sr.base_mip_level;
			rtv.Texture2DArray.FirstArraySlice = (UINT)sr.base_array_layer;
			rtv.Texture2DArray.ArraySize       = (UINT)sr.array_layer_count;
			break;
		default:
			rtv.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
			rtv.Texture2D.MipSlice = (UINT)sr.base_mip_level;
			break;
		}
	} else if (usage & CRGPU_IMAGE_USAGE_STORAGE) {
		crgpu_d3d12_fill_uav(pResult->_d3d_view_desc._uav, image->_desc.format, desc);
	} else {
		crgpu_d3d12_fill_srv(pResult->_d3d_view_desc._srv, image->_desc.format, desc);
	}
	// The view descs a descriptor write reads. An image with several usages
	// (rendered to and sampled, or sampled and written as a storage image)
	// has one view object for all of them, and _d3d_view_desc above is a
	// union that holds only the first that applies.
	crgpu_d3d12_fill_srv(pResult->_d3d_srv, image->_desc.format, desc);
	crgpu_d3d12_fill_uav(pResult->_d3d_uav, image->_desc.format, desc);

	pResult->_device = image->_device;
	CRGPU___LIVE_ADD(&pResult->_device->_live, CRGPU___OBJECT_IMAGE_VIEW);
	*out_image_view = pResult;
    return CR_SUCCESS;
}
void crgpu_image_view_destroy(crgpu_image_view image_view) {
	if (!image_view)
		return;
	if (CR_STALE(image_view, "crgpu_image_view_destroy", "image_view"))
		return;
	CRGPU___LIVE_REMOVE(&image_view->_device->_live, CRGPU___OBJECT_IMAGE_VIEW);
	CR_KILL(image_view);
	const CR_ALLOCATOR allocator = image_view->head.allocator;
	cr_string_free(&allocator, image_view->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(image_view), &allocator, crgpu_image_view_d3d12, image_view);
}
CR_RESULT crgpu_image_view_get_desc(
    crgpu_image_view image_view, crgpu_image_view_desc *out_desc) {
	if (!image_view)
		return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
	if (CR_STALE(image_view, "crgpu_image_view_get_desc", "image_view"))
		return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
	if (!out_desc)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = image_view->_desc;
	out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC);
	return CR_SUCCESS;
}
}

#endif // CRGPU_COMPILE_D3D12_API
