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
// - The common uncompressed formats, not the whole DXGI-shaped enum, and
//   the block-compressed ones (card S48): every BC format but the TYPELESS
//   ones (MTLPixelFormatBC1_RGBA to BC7_RGBAUnorm_sRGB, sampled where
//   supportsBCTextureCompression) and the 28 ASTC LDR formats
//   (MTLPixelFormatASTC_<w>x<h>_LDR and _sRGB, on every Apple family), in a
//   second table indexed from CRGPU_FORMAT_ASTC_4X4_UNORM (1000). Whether
//   the device samples one is the creates' feature check
//   (crgpu___internal_check_image_block), made before this table is read.
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
#define BLOCK(pixel) {(pixel), MTLPixelFormatInvalid, MTLVertexFormatInvalid, CRGPU_IMAGE_ASPECT_COLOR, false}

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

	// BC, sampled only. BC1 is MTLPixelFormatBC1_RGBA, DXGI's BC1 (alpha
	// from the block's 1-bit mode); BC6H's signed and unsigned are
	// BC6H_RGBFloat and BC6H_RGBUfloat.
	[CRGPU_FORMAT_BC1_UNORM]      = BLOCK(MTLPixelFormatBC1_RGBA),
	[CRGPU_FORMAT_BC1_UNORM_SRGB] = BLOCK(MTLPixelFormatBC1_RGBA_sRGB),
	[CRGPU_FORMAT_BC2_UNORM]      = BLOCK(MTLPixelFormatBC2_RGBA),
	[CRGPU_FORMAT_BC2_UNORM_SRGB] = BLOCK(MTLPixelFormatBC2_RGBA_sRGB),
	[CRGPU_FORMAT_BC3_UNORM]      = BLOCK(MTLPixelFormatBC3_RGBA),
	[CRGPU_FORMAT_BC3_UNORM_SRGB] = BLOCK(MTLPixelFormatBC3_RGBA_sRGB),
	[CRGPU_FORMAT_BC4_UNORM]      = BLOCK(MTLPixelFormatBC4_RUnorm),
	[CRGPU_FORMAT_BC4_SNORM]      = BLOCK(MTLPixelFormatBC4_RSnorm),
	[CRGPU_FORMAT_BC5_UNORM]      = BLOCK(MTLPixelFormatBC5_RGUnorm),
	[CRGPU_FORMAT_BC5_SNORM]      = BLOCK(MTLPixelFormatBC5_RGSnorm),
	[CRGPU_FORMAT_BC6H_UF16]      = BLOCK(MTLPixelFormatBC6H_RGBUfloat),
	[CRGPU_FORMAT_BC6H_SF16]      = BLOCK(MTLPixelFormatBC6H_RGBFloat),
	[CRGPU_FORMAT_BC7_UNORM]      = BLOCK(MTLPixelFormatBC7_RGBAUnorm),
	[CRGPU_FORMAT_BC7_UNORM_SRGB] = BLOCK(MTLPixelFormatBC7_RGBAUnorm_sRGB),
};

// ASTC LDR, from CRGPU_FORMAT_ASTC_4X4_UNORM on, in CRGPU_FORMAT's order:
// each size's UNORM (Metal's _LDR) and then its sRGB.
static const crgpumetal_format_info crgpumetal___astc_formats[] = {
	BLOCK(MTLPixelFormatASTC_4x4_LDR),   BLOCK(MTLPixelFormatASTC_4x4_sRGB),   BLOCK(MTLPixelFormatASTC_5x4_LDR),
	BLOCK(MTLPixelFormatASTC_5x4_sRGB),  BLOCK(MTLPixelFormatASTC_5x5_LDR),    BLOCK(MTLPixelFormatASTC_5x5_sRGB),
	BLOCK(MTLPixelFormatASTC_6x5_LDR),   BLOCK(MTLPixelFormatASTC_6x5_sRGB),   BLOCK(MTLPixelFormatASTC_6x6_LDR),
	BLOCK(MTLPixelFormatASTC_6x6_sRGB),  BLOCK(MTLPixelFormatASTC_8x5_LDR),    BLOCK(MTLPixelFormatASTC_8x5_sRGB),
	BLOCK(MTLPixelFormatASTC_8x6_LDR),   BLOCK(MTLPixelFormatASTC_8x6_sRGB),   BLOCK(MTLPixelFormatASTC_8x8_LDR),
	BLOCK(MTLPixelFormatASTC_8x8_sRGB),  BLOCK(MTLPixelFormatASTC_10x5_LDR),   BLOCK(MTLPixelFormatASTC_10x5_sRGB),
	BLOCK(MTLPixelFormatASTC_10x6_LDR),  BLOCK(MTLPixelFormatASTC_10x6_sRGB),  BLOCK(MTLPixelFormatASTC_10x8_LDR),
	BLOCK(MTLPixelFormatASTC_10x8_sRGB), BLOCK(MTLPixelFormatASTC_10x10_LDR),  BLOCK(MTLPixelFormatASTC_10x10_sRGB),
	BLOCK(MTLPixelFormatASTC_12x10_LDR), BLOCK(MTLPixelFormatASTC_12x10_sRGB), BLOCK(MTLPixelFormatASTC_12x12_LDR),
	BLOCK(MTLPixelFormatASTC_12x12_sRGB),
};
_Static_assert(sizeof(crgpumetal___astc_formats) / sizeof(crgpumetal___astc_formats[0]) ==
                   CRGPU_FORMAT_ASTC_12X12_UNORM_SRGB - CRGPU_FORMAT_ASTC_4X4_UNORM + 1,
               "one ASTC entry for each CRGPU_FORMAT_ASTC_* value");

#undef PIX
#undef PRESENT
#undef VERTEX_ONLY
#undef BLOCK

// What each format can do (card S51). Metal has no per-format query: its
// answers are Apple's "Metal Feature Set Tables" (developer.apple.com,
// Metal-Feature-Set-Tables.pdf), the edition dated October 23, 2026, pages
// 11 to 15, "Texture capabilities by pixel format", transcribed here for
// every pixel format the table above maps: each one's Filter, Write, Color
// and Blend in the Apple7, Apple8, Apple9 and Apple10 columns ("All" is all
// four, with MSAA, Sparse and Resolve, which no bit reports). That edition
// has no Mac2 column; its note 1 says every Metal 3 GPU has at least the
// Apple7 column's, which is what a GPU in none of Apple7 to Apple10 is
// given here. A family the tables add later takes the highest one listed,
// as the GPU is in that one too. Where the device answers itself, it wins:
// supportsBCTextureCompression for BC (the tables' "Varies" for Apple7 and
// Apple8), MTLGPUFamilyApple2 for ASTC, supports32BitFloatFiltering for
// R32Float, RG32Float and RGBA32Float, and readWriteTextureSupport (the
// MTLReadWriteTextureTier documentation's lists) for STORAGE_READ.
#define CAP_F 1u
#define CAP_W 2u
#define CAP_C 4u
#define CAP_B 8u
#define CAP_ALL (CAP_F | CAP_W | CAP_C | CAP_B)
#define CAP_WC (CAP_W | CAP_C)
#define SAME(letters) {(letters), (letters), (letters), (letters)}
// The Apple7 and Apple8 columns, then the Apple9 and Apple10 columns.
#define SPLIT(early, late) {(early), (early), (late), (late)}

typedef struct crgpumetal___format_columns {
	uint8_t apple[4]; // Apple7, Apple8, Apple9, Apple10
} crgpumetal___format_columns;

static const crgpumetal___format_columns crgpumetal___format_caps[] = {
	// Ordinary 8-bit (page 11), 16-bit (pages 11 and 12), 32-bit (page 12)
	// and 64-bit and 128-bit (page 14) pixel formats.
	[CRGPU_FORMAT_R8_UNORM]            = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R8_SNORM]            = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R8_UINT]             = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R8_SINT]             = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R8G8_UNORM]          = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R8G8_SNORM]          = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R8G8_UINT]           = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R8G8_SINT]           = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R8G8B8A8_UNORM]      = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R8G8B8A8_UNORM_SRGB] = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R8G8B8A8_SNORM]      = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R8G8B8A8_UINT]       = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R8G8B8A8_SINT]       = {SAME(CAP_WC)},
	[CRGPU_FORMAT_B8G8R8A8_UNORM]      = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_B8G8R8A8_UNORM_SRGB] = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R16_FLOAT]           = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R16_UNORM]           = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R16_SNORM]           = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R16_UINT]            = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R16_SINT]            = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R16G16_FLOAT]        = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R16G16_UNORM]        = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R16G16_SNORM]        = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R16G16_UINT]         = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R16G16_SINT]         = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R16G16B16A16_FLOAT]  = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R16G16B16A16_UNORM]  = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R16G16B16A16_SNORM]  = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R16G16B16A16_UINT]   = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R16G16B16A16_SINT]   = {SAME(CAP_WC)},
	// R32Float, RG32Float and RGBA32Float filter from Apple9 (and where
	// supports32BitFloatFiltering says so).
	[CRGPU_FORMAT_R32_FLOAT]           = {SPLIT(CAP_WC | CAP_B, CAP_ALL)},
	[CRGPU_FORMAT_R32_UINT]            = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R32_SINT]            = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R32G32_FLOAT]        = {SPLIT(CAP_WC | CAP_B, CAP_ALL)},
	[CRGPU_FORMAT_R32G32_UINT]         = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R32G32_SINT]         = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R32G32B32A32_FLOAT]  = {SPLIT(CAP_WC | CAP_B, CAP_ALL)},
	[CRGPU_FORMAT_R32G32B32A32_UINT]   = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R32G32B32A32_SINT]   = {SAME(CAP_WC)},
	// Packed 32-bit (page 13).
	[CRGPU_FORMAT_R10G10B10A2_UNORM]   = {SAME(CAP_ALL)},
	[CRGPU_FORMAT_R10G10B10A2_UINT]    = {SAME(CAP_WC)},
	[CRGPU_FORMAT_R11G11B10_FLOAT]     = {SAME(CAP_ALL)},
	// Depth and stencil (page 15): Filter alone, Depth32Float's and
	// Depth32Float_Stencil8's from Apple9.
	[CRGPU_FORMAT_D16_UNORM]            = {SAME(CAP_F)},
	[CRGPU_FORMAT_D32_FLOAT]            = {SPLIT(0, CAP_F)},
	[CRGPU_FORMAT_D32_FLOAT_S8X24_UINT] = {SPLIT(0, CAP_F)},
};

#undef SAME
#undef SPLIT

CRGPU_FORMAT_CAPABILITIES crgpumetal___format_capabilities(
    id<MTLDevice> device,
    CRGPU_FORMAT format) {
	const crgpumetal_format_info *info = crgpumetal___format_info(format);
	CRGPU_FORMAT_CAPABILITIES caps     = 0;
	if (info->vertex != MTLVertexFormatInvalid)
		caps |= CRGPU_FORMAT_CAPABILITY_VERTEX;
	if (info->pixel == MTLPixelFormatInvalid)
		return caps;
	// The highest of the families the tables list that the GPU is in; the
	// Apple7 column for a GPU in none of them (note 1).
	uint32_t column = 0;
	if ([device supportsFamily:MTLGPUFamilyApple8])
		column = 1;
	if ([device supportsFamily:MTLGPUFamilyApple9])
		column = 2;
	if ([device supportsFamily:MTLGPUFamilyApple10])
		column = 3;
	uint32_t letters = 0;
	if (format < sizeof(crgpumetal___format_caps) / sizeof(crgpumetal___format_caps[0]))
		letters = crgpumetal___format_caps[format].apple[column];
	// The compressed formats (page 14): Filter in every column, where the
	// device samples the family at all.
	const bool bc   = format >= CRGPU_FORMAT_BC1_TYPELESS && format <= CRGPU_FORMAT_BC7_UNORM_SRGB &&
	                  !(format >= CRGPU_FORMAT_B5G6R5_UNORM && format <= CRGPU_FORMAT_B8G8R8X8_UNORM_SRGB);
	const bool astc = format >= CRGPU_FORMAT_ASTC_4X4_UNORM && format <= CRGPU_FORMAT_ASTC_12X12_UNORM_SRGB;
	if (bc && !device.supportsBCTextureCompression)
		return caps;
	if (astc && ![device supportsFamily:MTLGPUFamilyApple2])
		return caps;
	if (bc || astc)
		letters = CAP_F;
	// Every pixel format is read by a shader (the tables' note under
	// "Texture capabilities by pixel format"), and copied.
	caps |= CRGPU_FORMAT_CAPABILITY_SAMPLED | CRGPU_FORMAT_CAPABILITY_COPY;
	const bool float32 = format == CRGPU_FORMAT_R32_FLOAT || format == CRGPU_FORMAT_R32G32_FLOAT || format == CRGPU_FORMAT_R32G32B32A32_FLOAT;
	if ((letters & CAP_F) || (float32 && device.supports32BitFloatFiltering))
		caps |= CRGPU_FORMAT_CAPABILITY_FILTERED;
	if (letters & CAP_W)
		caps |= CRGPU_FORMAT_CAPABILITY_STORAGE;
	if (letters & CAP_C)
		caps |= CRGPU_FORMAT_CAPABILITY_COLOR_ATTACHMENT;
	if (letters & CAP_B)
		caps |= CRGPU_FORMAT_CAPABILITY_BLEND;
	// Every Metal 3 GPU has Depth16Unorm, Depth32Float and
	// Depth32Float_Stencil8.
	if (info->aspects & CRGPU_IMAGE_ASPECT_DEPTH)
		caps |= CRGPU_FORMAT_CAPABILITY_DEPTH_STENCIL_ATTACHMENT;
	// Read-write textures: Tier 1 is R32Float, R32Uint and R32Sint; Tier 2
	// adds the RGBA32, RGBA16, RGBA8 and R16 and R8 formats below.
	bool read_write = false;
	switch (info->pixel) {
	case MTLPixelFormatR32Float:
	case MTLPixelFormatR32Uint:
	case MTLPixelFormatR32Sint:
		read_write = device.readWriteTextureSupport >= MTLReadWriteTextureTier1;
		break;
	case MTLPixelFormatRGBA32Float:
	case MTLPixelFormatRGBA32Uint:
	case MTLPixelFormatRGBA32Sint:
	case MTLPixelFormatRGBA16Float:
	case MTLPixelFormatRGBA16Uint:
	case MTLPixelFormatRGBA16Sint:
	case MTLPixelFormatRGBA8Unorm:
	case MTLPixelFormatRGBA8Uint:
	case MTLPixelFormatRGBA8Sint:
	case MTLPixelFormatR16Float:
	case MTLPixelFormatR16Uint:
	case MTLPixelFormatR16Sint:
	case MTLPixelFormatR8Unorm:
	case MTLPixelFormatR8Uint:
	case MTLPixelFormatR8Sint:
		read_write = device.readWriteTextureSupport >= MTLReadWriteTextureTier2;
		break;
	default:
		break;
	}
	if (read_write && (letters & CAP_W))
		caps |= CRGPU_FORMAT_CAPABILITY_STORAGE_READ;
	return caps;
}

#undef CAP_F
#undef CAP_W
#undef CAP_C
#undef CAP_B
#undef CAP_ALL
#undef CAP_WC

// A format with no entry: a color format Metal has nothing for here.
static const crgpumetal_format_info crgpumetal___no_format = {
    MTLPixelFormatInvalid, MTLPixelFormatInvalid, MTLVertexFormatInvalid, CRGPU_IMAGE_ASPECT_COLOR, false};

const crgpumetal_format_info *crgpumetal___format_info(CRGPU_FORMAT format) {
	if (format >= CRGPU_FORMAT_ASTC_4X4_UNORM && format <= CRGPU_FORMAT_ASTC_12X12_UNORM_SRGB)
		return &crgpumetal___astc_formats[format - CRGPU_FORMAT_ASTC_4X4_UNORM];
	if (format >= sizeof(crgpumetal___formats) / sizeof(crgpumetal___formats[0]))
		return &crgpumetal___no_format;
	const crgpumetal_format_info *info = &crgpumetal___formats[format];
	// An index the table skips is zeroed, with no aspects: it has no entry.
	return info->aspects ? info : &crgpumetal___no_format;
}

#endif // CRGPU_COMPILE_METAL_API
