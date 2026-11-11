//
// Carrottus GPU Metal backend - the format table: what each CRGPU_FORMAT is to
// Metal. Every function of this backend that turns a CRGPU_FORMAT into a
// Metal format, or asks what a format is, reads it here.
//

#if CRGPU_COMPILE_METAL_API

#include "crcore.h"
#include "crgpu.h"
#include "crgpumetal.h"
#include <Metal/Metal.h>

// Which formats are here, and why the others aren't:
//
// - The common uncompressed formats, not the whole DXGI-shaped enum. The
//   block-compressed formats and the many *_TYPELESS ones have no caller
//   in this workspace yet.
// - TYPELESS formats are never mapped. Metal has no typeless resource the
//   way D3D12 does: every texture needs one concrete MTLPixelFormat at
//   creation, so a typeless request is refused rather than given an
//   interpretation the caller didn't ask for.
// - CRGPU_FORMAT_D24_UNORM_S8_UINT has no pixel format:
//   MTLPixelFormatDepth24Unorm_Stencil8 exists only on Intel and AMD Macs,
//   not on Apple Silicon, this backend's target. There is no honest way to
//   hand back a 24-bit depth resource; CRGPU_FORMAT_D32_FLOAT_S8X24_UINT
//   is Depth32Float_Stencil8. Its aspects are listed all the same, so a
//   view's aspects are judged the same way on every backend.
// - Vertex formats include the 3-component 32-bit ones, which have no
//   pixel format: vertex fetch doesn't go through the texture unit.
// - Presentable: what a CAMetalLayer's pixelFormat may be set to, of the
//   formats here. Apple lists BGRA8Unorm, BGRA8Unorm_sRGB, RGBA16Float,
//   RGB10A2Unorm, BGR10A2Unorm and the extended-range BGR10 formats; the
//   last two kinds have no CRGPU_FORMAT. RGBA8Unorm is not on the list.

#define PIX(pixel, vertex) {(pixel), MTLPixelFormatInvalid, (vertex), CRGPU_IMAGE_ASPECT_COLOR, false}
#define PRESENT(pixel, vertex) {(pixel), MTLPixelFormatInvalid, (vertex), CRGPU_IMAGE_ASPECT_COLOR, true}
#define VERTEX_ONLY(vertex) {MTLPixelFormatInvalid, MTLPixelFormatInvalid, (vertex), CRGPU_IMAGE_ASPECT_COLOR, false}

static const crgpumetal_format_info crgpumetal___formats[] = {
	// 8 bits per channel
	[CRGPU_FORMAT_R8_UNORM]            = PIX(MTLPixelFormatR8Unorm, MTLVertexFormatUCharNormalized),
	[CRGPU_FORMAT_R8_SNORM]            = PIX(MTLPixelFormatR8Snorm, MTLVertexFormatCharNormalized),
	[CRGPU_FORMAT_R8_UINT]             = PIX(MTLPixelFormatR8Uint, MTLVertexFormatUChar),
	[CRGPU_FORMAT_R8_SINT]             = PIX(MTLPixelFormatR8Sint, MTLVertexFormatChar),
	[CRGPU_FORMAT_R8G8_UNORM]          = PIX(MTLPixelFormatRG8Unorm, MTLVertexFormatUChar2Normalized),
	[CRGPU_FORMAT_R8G8_SNORM]          = PIX(MTLPixelFormatRG8Snorm, MTLVertexFormatChar2Normalized),
	[CRGPU_FORMAT_R8G8_UINT]           = PIX(MTLPixelFormatRG8Uint, MTLVertexFormatUChar2),
	[CRGPU_FORMAT_R8G8_SINT]           = PIX(MTLPixelFormatRG8Sint, MTLVertexFormatChar2),
	[CRGPU_FORMAT_R8G8B8A8_UNORM]      = PIX(MTLPixelFormatRGBA8Unorm, MTLVertexFormatUChar4Normalized),
	[CRGPU_FORMAT_R8G8B8A8_UNORM_SRGB] = PIX(MTLPixelFormatRGBA8Unorm_sRGB, MTLVertexFormatInvalid),
	[CRGPU_FORMAT_R8G8B8A8_SNORM]      = PIX(MTLPixelFormatRGBA8Snorm, MTLVertexFormatChar4Normalized),
	[CRGPU_FORMAT_R8G8B8A8_UINT]       = PIX(MTLPixelFormatRGBA8Uint, MTLVertexFormatUChar4),
	[CRGPU_FORMAT_R8G8B8A8_SINT]       = PIX(MTLPixelFormatRGBA8Sint, MTLVertexFormatChar4),
	[CRGPU_FORMAT_B8G8R8A8_UNORM]      = PRESENT(MTLPixelFormatBGRA8Unorm, MTLVertexFormatInvalid),
	[CRGPU_FORMAT_B8G8R8A8_UNORM_SRGB] = PRESENT(MTLPixelFormatBGRA8Unorm_sRGB, MTLVertexFormatInvalid),

	// 16 bits per channel
	[CRGPU_FORMAT_R16_FLOAT]           = PIX(MTLPixelFormatR16Float, MTLVertexFormatHalf),
	[CRGPU_FORMAT_R16_UNORM]           = PIX(MTLPixelFormatR16Unorm, MTLVertexFormatUShortNormalized),
	[CRGPU_FORMAT_R16_SNORM]           = PIX(MTLPixelFormatR16Snorm, MTLVertexFormatShortNormalized),
	[CRGPU_FORMAT_R16_UINT]            = PIX(MTLPixelFormatR16Uint, MTLVertexFormatUShort),
	[CRGPU_FORMAT_R16_SINT]            = PIX(MTLPixelFormatR16Sint, MTLVertexFormatShort),
	[CRGPU_FORMAT_R16G16_FLOAT]        = PIX(MTLPixelFormatRG16Float, MTLVertexFormatHalf2),
	[CRGPU_FORMAT_R16G16_UNORM]        = PIX(MTLPixelFormatRG16Unorm, MTLVertexFormatUShort2Normalized),
	[CRGPU_FORMAT_R16G16_SNORM]        = PIX(MTLPixelFormatRG16Snorm, MTLVertexFormatShort2Normalized),
	[CRGPU_FORMAT_R16G16_UINT]         = PIX(MTLPixelFormatRG16Uint, MTLVertexFormatUShort2),
	[CRGPU_FORMAT_R16G16_SINT]         = PIX(MTLPixelFormatRG16Sint, MTLVertexFormatShort2),
	[CRGPU_FORMAT_R16G16B16A16_FLOAT]  = PRESENT(MTLPixelFormatRGBA16Float, MTLVertexFormatHalf4),
	[CRGPU_FORMAT_R16G16B16A16_UNORM]  = PIX(MTLPixelFormatRGBA16Unorm, MTLVertexFormatUShort4Normalized),
	[CRGPU_FORMAT_R16G16B16A16_SNORM]  = PIX(MTLPixelFormatRGBA16Snorm, MTLVertexFormatShort4Normalized),
	[CRGPU_FORMAT_R16G16B16A16_UINT]   = PIX(MTLPixelFormatRGBA16Uint, MTLVertexFormatUShort4),
	[CRGPU_FORMAT_R16G16B16A16_SINT]   = PIX(MTLPixelFormatRGBA16Sint, MTLVertexFormatShort4),

	// 32 bits per channel
	[CRGPU_FORMAT_R32_FLOAT]           = PIX(MTLPixelFormatR32Float, MTLVertexFormatFloat),
	[CRGPU_FORMAT_R32_UINT]            = PIX(MTLPixelFormatR32Uint, MTLVertexFormatUInt),
	[CRGPU_FORMAT_R32_SINT]            = PIX(MTLPixelFormatR32Sint, MTLVertexFormatInt),
	[CRGPU_FORMAT_R32G32_FLOAT]        = PIX(MTLPixelFormatRG32Float, MTLVertexFormatFloat2),
	[CRGPU_FORMAT_R32G32_UINT]         = PIX(MTLPixelFormatRG32Uint, MTLVertexFormatUInt2),
	[CRGPU_FORMAT_R32G32_SINT]         = PIX(MTLPixelFormatRG32Sint, MTLVertexFormatInt2),
	[CRGPU_FORMAT_R32G32B32_FLOAT]     = VERTEX_ONLY(MTLVertexFormatFloat3),
	[CRGPU_FORMAT_R32G32B32_UINT]      = VERTEX_ONLY(MTLVertexFormatUInt3),
	[CRGPU_FORMAT_R32G32B32_SINT]      = VERTEX_ONLY(MTLVertexFormatInt3),
	[CRGPU_FORMAT_R32G32B32A32_FLOAT]  = PIX(MTLPixelFormatRGBA32Float, MTLVertexFormatFloat4),
	[CRGPU_FORMAT_R32G32B32A32_UINT]   = PIX(MTLPixelFormatRGBA32Uint, MTLVertexFormatUInt4),
	[CRGPU_FORMAT_R32G32B32A32_SINT]   = PIX(MTLPixelFormatRGBA32Sint, MTLVertexFormatInt4),

	// Packed
	[CRGPU_FORMAT_R10G10B10A2_UNORM]   = PRESENT(MTLPixelFormatRGB10A2Unorm, MTLVertexFormatUInt1010102Normalized),
	[CRGPU_FORMAT_R10G10B10A2_UINT]    = PIX(MTLPixelFormatRGB10A2Uint, MTLVertexFormatInvalid),
	[CRGPU_FORMAT_R11G11B10_FLOAT]     = PIX(MTLPixelFormatRG11B10Float, MTLVertexFormatInvalid),

	// Depth and stencil
	[CRGPU_FORMAT_D16_UNORM] = {MTLPixelFormatDepth16Unorm, MTLPixelFormatInvalid, MTLVertexFormatInvalid,
	                              CRGPU_IMAGE_ASPECT_DEPTH, false},
	[CRGPU_FORMAT_D32_FLOAT] = {MTLPixelFormatDepth32Float, MTLPixelFormatInvalid, MTLVertexFormatInvalid,
	                              CRGPU_IMAGE_ASPECT_DEPTH, false},
	[CRGPU_FORMAT_D24_UNORM_S8_UINT] = {MTLPixelFormatInvalid, MTLPixelFormatInvalid, MTLVertexFormatInvalid,
	                                      CRGPU_IMAGE_ASPECT_DEPTH | CRGPU_IMAGE_ASPECT_STENCIL, false},
	// The stencil of a Depth32Float_Stencil8 texture is read through an
	// X32_Stencil8 view of it.
	[CRGPU_FORMAT_D32_FLOAT_S8X24_UINT] = {MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatX32_Stencil8,
	                                         MTLVertexFormatInvalid,
	                                         CRGPU_IMAGE_ASPECT_DEPTH | CRGPU_IMAGE_ASPECT_STENCIL, false},
};

#undef PIX
#undef PRESENT
#undef VERTEX_ONLY

// A format with no entry: a color format Metal has nothing for here.
static const crgpumetal_format_info crgpumetal___no_format = {
    MTLPixelFormatInvalid, MTLPixelFormatInvalid, MTLVertexFormatInvalid, CRGPU_IMAGE_ASPECT_COLOR, false};

const crgpumetal_format_info *crgpumetal___format_info(CRGPU_FORMAT format) {
	if (format >= sizeof(crgpumetal___formats) / sizeof(crgpumetal___formats[0]))
		return &crgpumetal___no_format;
	const crgpumetal_format_info *info = &crgpumetal___formats[format];
	// An index the table skips is zeroed, with no aspects: it has no entry.
	return info->aspects ? info : &crgpumetal___no_format;
}

#endif // CRGPU_COMPILE_METAL_API
