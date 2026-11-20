/*
 * crgpu_device_test — Carrottus GPU's adapters and devices.
 *
 * Lists the adapters, reads the first one's properties (chained structs
 * included), and creates and destroys devices on it: the argument checks of
 * every call, a device's queues by kind, features asked for and not, the
 * descriptor budget, one device per adapter, and a device made again after
 * one is destroyed. On every adapter: its image limits, and its two
 * block-format bits (card S48) against what the graphics API says, each
 * asked for at device creation; and every format's capabilities (card S51),
 * printed, with their refusals and the rules every API keeps, and on Metal
 * each against Apple's feature set tables as transcribed here. Everything goes through a checking allocator, which must
 * end with nothing left.
 *
 * Needs a GPU but no window, nobody watching and nothing drawn, so it is a
 * ctest; with no adapter it exits 77, which ctest reports as skipped.
 *
 * Exits 0 when every check passes, 1 when one fails.
 */

#include <crgpu.h>

#include "crtest_allocator.h"
#if CRGPU_COMPILE_METAL_API
#include "crgpu_test_metal_family.h"
#endif
#if CRGPU_COMPILE_VULKAN_API
#include <crgpu_vulkan_natives.h>
#endif

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

#define SKIPPED 77

static CRGPU_ADAPTER_PROPERTIES properties_of(crgpu_adapter adapter) {
	CRGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_SUCCESS);
	return properties;
}

static crgpu_device_desc device_desc(void) {
	crgpu_device_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	desc.direct_queue_count = 1;
	desc.max_descriptors    = 16;
	desc.max_samplers       = 4;
	desc.debug_name         = "crgpu_device_test";
	return desc;
}

static void test_enumerate(crgpu_instance instance) {
	const crgpu_adapter *adapters = NULL;
	uint32_t count                  = 0;
	CHECK(crgpu_adapter_enumerate(NULL, &adapters, &count) == CR_RESULT_GPU_INVALID_INSTANCE);
	CHECK(crgpu_adapter_enumerate(instance, NULL, &count) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK(crgpu_adapter_enumerate(instance, &adapters, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);

	// The list is read once: every call hands out the same array.
	const crgpu_adapter *again = NULL;
	uint32_t again_count         = 0;
	CHECK(crgpu_adapter_enumerate(instance, &adapters, &count) == CR_SUCCESS);
	CHECK(crgpu_adapter_enumerate(instance, &again, &again_count) == CR_SUCCESS);
	CHECK(adapters == again);
	CHECK(count == again_count);
}

static void test_adapter_properties(crgpu_adapter adapter) {
	CRGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	CHECK(crgpu_adapter_get_properties(NULL, &properties) == CR_RESULT_GPU_INVALID_ADAPTER);
	CHECK(crgpu_adapter_get_properties(adapter, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);

	properties.base.type = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_RESULT_STRUCT_WRONG_TYPE);
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;

	// A struct the call doesn't take there, anywhere in the chain, refuses
	// the whole call and writes nothing.
	CR_STRUCT_BASE_OUT stranger = {CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS, NULL};
	properties.base.next          = &stranger;
	properties.description[0]     = '\x7f';
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
	CHECK(properties.description[0] == '\x7f');
	properties.base.next = NULL;

#if CRGPU_EXT_MESH_SHADING
	CRGPU_MESH_SHADING_LIMITS mesh_a, mesh_b;
	memset(&mesh_a, 0, sizeof(mesh_a));
	memset(&mesh_b, 0, sizeof(mesh_b));
	mesh_a.base.type     = CR_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS;
	mesh_b.base.type     = CR_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS;
	mesh_a.base.next     = &mesh_b.base;
	properties.base.next = &mesh_a.base;
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_RESULT_STRUCT_DUPLICATE_IN_CHAIN);

	// One limits struct: filled in, and the chain left as the caller built it.
	mesh_a.base.next = NULL;
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_SUCCESS);
	CHECK(properties.base.type == CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES);
	CHECK(properties.base.next == &mesh_a.base);
	CHECK(mesh_a.base.type == CR_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS);
	CHECK(mesh_a.base.next == NULL);
	if (properties.supported_features & CRGPU_FEATURE_MESH_SHADING) {
		CHECK(mesh_a.max_mesh_output_vertices > 0);
		CHECK(mesh_a.max_mesh_workgroup_invocations > 0);
	} else {
		CHECK(mesh_a.max_mesh_output_vertices == 0);
	}
	properties.base.next = NULL;
#endif

	properties = properties_of(adapter);
	printf("adapter: %s, vendor 0x%04x, type %u, features 0x%llx, %u queue pools\n", properties.description,
	    (unsigned)properties.vendor_id, (unsigned)properties.adapter_type,
	    (unsigned long long)properties.supported_features, (unsigned)properties.queue_pool_count);
	CHECK(properties.description[0] != '\0');
	CHECK(properties.adapter_type <= CRGPU_ADAPTER_TYPE_SOFTWARE);

	// Some byte of the id says which GPU.
	bool id_nonzero = false;
	for (size_t i = 0; i < sizeof(properties.adapter_id); ++i)
		id_nonzero = id_nonzero || properties.adapter_id[i] != 0;
	CHECK(id_nonzero);

	// Every kind is in exactly one pool, and no pool is empty.
	CHECK(properties.queue_pool_count >= 1 && properties.queue_pool_count <= 3);
	CRGPU_QUEUE_KINDS seen = 0;
	for (uint32_t i = 0; i < properties.queue_pool_count && i < 3; ++i) {
		CHECK(properties.queue_pools[i].kinds != 0);
		CHECK((seen & properties.queue_pools[i].kinds) == 0);
		CHECK(properties.queue_pools[i].max_queues > 0);
		seen |= properties.queue_pools[i].kinds;
	}
	CHECK(seen == (CRGPU_QUEUE_KIND_DIRECT | CRGPU_QUEUE_KIND_COMPUTE | CRGPU_QUEUE_KIND_COPY));

	CHECK(properties.uniform_buffer_offset_alignment > 0);
	CHECK((properties.uniform_buffer_offset_alignment & (properties.uniform_buffer_offset_alignment - 1)) == 0);
	CHECK(properties.storage_buffer_offset_alignment > 0);
	CHECK(properties.max_draw_indirect_count >= 1);
	CHECK(properties.max_descriptors > 0);
	CHECK(properties.max_samplers > 0);
#if !CRGPU_EXT_BINDLESS
	CHECK(!(properties.supported_features & CRGPU_FEATURE_BINDLESS));
#endif
	// A device-mappable kind needs memory that is the system's.
	if (properties.supported_features & CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY)
		CHECK(properties.unified_memory);

	bool supported = true;
	CHECK(crgpu_adapter_supports_surface(NULL, NULL, &supported) == CR_RESULT_GPU_INVALID_ADAPTER);
	CHECK(crgpu_adapter_supports_surface(adapter, NULL, &supported) == CR_RESULT_GPU_INVALID_SURFACE);
}

/* The largest image sizes (card S33) on one adapter: each non-zero, each at
 * least the floor its graphics API guarantees, and printed. No API promises
 * 2D >= 1D, so it isn't checked. */
static void test_image_limits(uint32_t index, crgpu_adapter adapter) {
	CRGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_SUCCESS);
	printf("crgpu_device_test: adapter %u (%s): image limits 1D %u, 2D %u, 3D %u, cube %u, layers %u\n", index,
	       properties.description, properties.max_image_width_1d, properties.max_image_size_2d, properties.max_image_size_3d,
	       properties.max_image_size_cube, properties.max_image_array_layers);
	CHECK(properties.max_image_width_1d > 0);
	CHECK(properties.max_image_size_2d > 0);
	CHECK(properties.max_image_size_3d > 0);
	CHECK(properties.max_image_size_cube > 0);
	CHECK(properties.max_image_array_layers > 0);
#if CRGPU_COMPILE_VULKAN_API
	// Vulkan 1.3's required minimums (Vulkan 1.0's, which 1.3 keeps).
	CHECK(properties.max_image_width_1d >= 4096);
	CHECK(properties.max_image_size_2d >= 4096);
	CHECK(properties.max_image_size_3d >= 256);
	CHECK(properties.max_image_size_cube >= 4096);
	CHECK(properties.max_image_array_layers >= 256);
#elif CRGPU_COMPILE_D3D12_API
	// Fixed for feature level 11_0 and up.
	CHECK(properties.max_image_width_1d == 16384);
	CHECK(properties.max_image_size_2d == 16384);
	CHECK(properties.max_image_size_3d == 2048);
	CHECK(properties.max_image_size_cube == 16384);
	CHECK(properties.max_image_array_layers == 2048);
#elif CRGPU_COMPILE_METAL_API
	// By family: 32768 exactly on an Apple10 GPU, else 16384.
	const uint32_t size = crgpu_test_metal_is_apple10(properties.adapter_id) ? 32768u : 16384u;
	CHECK(properties.max_image_width_1d == size);
	CHECK(properties.max_image_size_2d == size);
	CHECK(properties.max_image_size_cube == size);
	CHECK(properties.max_image_size_3d == 2048);
	CHECK(properties.max_image_array_layers == 2048);
#endif
}

/* The block-format bits (card S48) on one adapter: each as the graphics API
 * reports it (Metal: BC exactly when supportsBCTextureCompression, ASTC LDR
 * exactly when the GPU is in MTLGPUFamilyApple2; D3D12: BC on every
 * adapter, ASTC on none; Vulkan: VkPhysicalDeviceFeatures'
 * textureCompressionBC and textureCompressionASTC_LDR). A device created
 * with each bit the adapter has reports it, one created without doesn't,
 * and asking for one it lacks is refused. */
static void test_block_format_bits(
    uint32_t index,
    crgpu_adapter adapter) {
	const CRGPU_ADAPTER_PROPERTIES properties = properties_of(adapter);
	const bool bc                             = (properties.supported_features & CRGPU_FEATURE_TEXTURE_COMPRESSION_BC) != 0;
	const bool astc                           = (properties.supported_features & CRGPU_FEATURE_TEXTURE_COMPRESSION_ASTC_LDR) != 0;
	printf("crgpu_device_test: adapter %u (%s): BC %s, ASTC LDR %s\n", index, properties.description, bc ? "yes" : "no", astc ? "yes" : "no");
#if CRGPU_COMPILE_METAL_API
	bool metal_bc = false, metal_astc = false;
	CHECK(crgpu_test_metal_block_formats(properties.adapter_id, &metal_bc, &metal_astc));
	CHECK(bc == metal_bc);
	CHECK(astc == metal_astc);
#elif CRGPU_COMPILE_D3D12_API
	CHECK(bc);
	CHECK(!astc);
#elif CRGPU_COMPILE_VULKAN_API
	VkPhysicalDeviceFeatures available;
	memset(&available, 0, sizeof(available));
	vkGetPhysicalDeviceFeatures(crgpu_get_vk_physical_device(adapter), &available);
	CHECK(bc == (available.textureCompressionBC == VK_TRUE));
	CHECK(astc == (available.textureCompressionASTC_LDR == VK_TRUE));
#endif

	const CRGPU_FEATURES bits[2] = {CRGPU_FEATURE_TEXTURE_COMPRESSION_BC, CRGPU_FEATURE_TEXTURE_COMPRESSION_ASTC_LDR};
	for (uint32_t i = 0; i < 2; ++i) {
		crgpu_device_desc desc = device_desc();
		desc.required_features = bits[i];
		crgpu_device device    = (crgpu_device)(uintptr_t)1;
		if (properties.supported_features & bits[i]) {
			CHECK(crgpu_device_create(adapter, &desc, &device) == CR_SUCCESS);
			CHECK(crgpu_device_get_features(device) == bits[i]);
			crgpu_device_destroy(device);
		} else {
			CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_GPU_FEATURE_NOT_SUPPORTED);
			CHECK(device == NULL);
		}
	}
	// Without either: neither reported.
	crgpu_device_desc desc = device_desc();
	crgpu_device device    = NULL;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_SUCCESS);
	CHECK((crgpu_device_get_features(device) & bits[0]) == 0);
	CHECK((crgpu_device_get_features(device) & bits[1]) == 0);
	crgpu_device_destroy(device);
}

/* The dual-source blending bit (card S50) on one adapter, as the graphics
 * API reports it (Metal and D3D12: every adapter; Vulkan: exactly when
 * dualSrcBlend is true and maxFragmentDualSrcAttachments is 1 or more, both
 * printed). A device created with it reports it, one created without
 * doesn't. */
static void test_dual_source_bit(
    uint32_t index,
    crgpu_adapter adapter) {
	const CRGPU_ADAPTER_PROPERTIES properties = properties_of(adapter);
	const bool dual                           = (properties.supported_features & CRGPU_FEATURE_DUAL_SOURCE_BLEND) != 0;
	printf("crgpu_device_test: adapter %u (%s): dual-source blending %s\n", index, properties.description, dual ? "yes" : "no");
#if CRGPU_COMPILE_METAL_API || CRGPU_COMPILE_D3D12_API
	CHECK(dual);
#elif CRGPU_COMPILE_VULKAN_API
	VkPhysicalDeviceFeatures available;
	memset(&available, 0, sizeof(available));
	vkGetPhysicalDeviceFeatures(crgpu_get_vk_physical_device(adapter), &available);
	VkPhysicalDeviceProperties vk_properties;
	memset(&vk_properties, 0, sizeof(vk_properties));
	vkGetPhysicalDeviceProperties(crgpu_get_vk_physical_device(adapter), &vk_properties);
	printf("crgpu_device_test: adapter %u: dualSrcBlend %u, maxFragmentDualSrcAttachments %u\n", index, (unsigned)available.dualSrcBlend,
	    (unsigned)vk_properties.limits.maxFragmentDualSrcAttachments);
	CHECK(dual == (available.dualSrcBlend == VK_TRUE && vk_properties.limits.maxFragmentDualSrcAttachments >= 1));
#endif
	crgpu_device_desc desc = device_desc();
	crgpu_device device    = (crgpu_device)(uintptr_t)1;
	desc.required_features = CRGPU_FEATURE_DUAL_SOURCE_BLEND;
	if (dual) {
		CHECK(crgpu_device_create(adapter, &desc, &device) == CR_SUCCESS);
		CHECK(crgpu_device_get_features(device) == CRGPU_FEATURE_DUAL_SOURCE_BLEND);
		crgpu_device_destroy(device);
	} else {
		CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_GPU_FEATURE_NOT_SUPPORTED);
		CHECK(device == NULL);
	}
	desc   = device_desc();
	device = NULL;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_SUCCESS);
	CHECK((crgpu_device_get_features(device) & CRGPU_FEATURE_DUAL_SOURCE_BLEND) == 0);
	crgpu_device_destroy(device);
}

/* ---- Format capabilities (card S51) ----------------------------------------
 *
 * Every format CRGPU_FORMAT names, by name, for the table printed per
 * adapter (CRGPU_DEVICE_TODO.md records it per machine). */
typedef struct named_format {
	const char *name;
	CRGPU_FORMAT format;
} named_format;

#define F(x) {#x, CRGPU_FORMAT_##x}
static const named_format all_formats[] = {
    F(R32G32B32A32_TYPELESS), F(R32G32B32A32_FLOAT), F(R32G32B32A32_UINT), F(R32G32B32A32_SINT), F(R32G32B32_TYPELESS),
    F(R32G32B32_FLOAT), F(R32G32B32_UINT), F(R32G32B32_SINT), F(R16G16B16A16_TYPELESS), F(R16G16B16A16_FLOAT),
    F(R16G16B16A16_UNORM), F(R16G16B16A16_UINT), F(R16G16B16A16_SNORM), F(R16G16B16A16_SINT), F(R32G32_TYPELESS),
    F(R32G32_FLOAT), F(R32G32_UINT), F(R32G32_SINT), F(R32G8X24_TYPELESS), F(D32_FLOAT_S8X24_UINT),
    F(R32_FLOAT_X8X24_TYPELESS), F(X32_TYPELESS_G8X24_UINT), F(R10G10B10A2_TYPELESS), F(R10G10B10A2_UNORM),
    F(R10G10B10A2_UINT), F(R11G11B10_FLOAT), F(R8G8B8A8_TYPELESS), F(R8G8B8A8_UNORM), F(R8G8B8A8_UNORM_SRGB),
    F(R8G8B8A8_UINT), F(R8G8B8A8_SNORM), F(R8G8B8A8_SINT), F(R16G16_TYPELESS), F(R16G16_FLOAT), F(R16G16_UNORM),
    F(R16G16_UINT), F(R16G16_SNORM), F(R16G16_SINT), F(R32_TYPELESS), F(D32_FLOAT), F(R32_FLOAT), F(R32_UINT),
    F(R32_SINT), F(R24G8_TYPELESS), F(D24_UNORM_S8_UINT), F(R24_UNORM_X8_TYPELESS), F(X24_TYPELESS_G8_UINT),
    F(R8G8_TYPELESS), F(R8G8_UNORM), F(R8G8_UINT), F(R8G8_SNORM), F(R8G8_SINT), F(R16_TYPELESS), F(R16_FLOAT),
    F(D16_UNORM), F(R16_UNORM), F(R16_UINT), F(R16_SNORM), F(R16_SINT), F(R8_TYPELESS), F(R8_UNORM), F(R8_UINT),
    F(R8_SNORM), F(R8_SINT), F(A8_UNORM), F(R1_UNORM), F(R9G9B9E5_SHAREDEXP), F(R8G8_B8G8_UNORM), F(G8R8_G8B8_UNORM),
    F(BC1_TYPELESS), F(BC1_UNORM), F(BC1_UNORM_SRGB), F(BC2_TYPELESS), F(BC2_UNORM), F(BC2_UNORM_SRGB), F(BC3_TYPELESS),
    F(BC3_UNORM), F(BC3_UNORM_SRGB), F(BC4_TYPELESS), F(BC4_UNORM), F(BC4_SNORM), F(BC5_TYPELESS), F(BC5_UNORM),
    F(BC5_SNORM), F(B5G6R5_UNORM), F(B5G5R5A1_UNORM), F(B8G8R8A8_UNORM), F(B8G8R8X8_UNORM),
    F(R10G10B10_XR_BIAS_A2_UNORM), F(B8G8R8A8_TYPELESS), F(B8G8R8A8_UNORM_SRGB), F(B8G8R8X8_TYPELESS),
    F(B8G8R8X8_UNORM_SRGB), F(BC6H_TYPELESS), F(BC6H_UF16), F(BC6H_SF16), F(BC7_TYPELESS), F(BC7_UNORM),
    F(BC7_UNORM_SRGB), F(AYUV), F(Y410), F(Y416), F(NV12), F(P010), F(P016), F(420_OPAQUE), F(YUY2), F(Y210), F(Y216),
    F(NV11), F(AI44), F(IA44), F(P8), F(A8P8), F(B4G4R4A4_UNORM), F(P208), F(V208), F(V408), F(ASTC_4X4_UNORM),
    F(ASTC_4X4_UNORM_SRGB), F(ASTC_5X4_UNORM), F(ASTC_5X4_UNORM_SRGB), F(ASTC_5X5_UNORM), F(ASTC_5X5_UNORM_SRGB),
    F(ASTC_6X5_UNORM), F(ASTC_6X5_UNORM_SRGB), F(ASTC_6X6_UNORM), F(ASTC_6X6_UNORM_SRGB), F(ASTC_8X5_UNORM),
    F(ASTC_8X5_UNORM_SRGB), F(ASTC_8X6_UNORM), F(ASTC_8X6_UNORM_SRGB), F(ASTC_8X8_UNORM), F(ASTC_8X8_UNORM_SRGB),
    F(ASTC_10X5_UNORM), F(ASTC_10X5_UNORM_SRGB), F(ASTC_10X6_UNORM), F(ASTC_10X6_UNORM_SRGB), F(ASTC_10X8_UNORM),
    F(ASTC_10X8_UNORM_SRGB), F(ASTC_10X10_UNORM), F(ASTC_10X10_UNORM_SRGB), F(ASTC_12X10_UNORM),
    F(ASTC_12X10_UNORM_SRGB), F(ASTC_12X12_UNORM), F(ASTC_12X12_UNORM_SRGB),
};
#undef F
#define FORMAT_COUNT (sizeof(all_formats) / sizeof(all_formats[0]))

#define CAP_SAMPLED CRGPU_FORMAT_CAPABILITY_SAMPLED
#define CAP_FILTERED CRGPU_FORMAT_CAPABILITY_FILTERED
#define CAP_STORAGE CRGPU_FORMAT_CAPABILITY_STORAGE
#define CAP_COLOR CRGPU_FORMAT_CAPABILITY_COLOR_ATTACHMENT
#define CAP_BLEND CRGPU_FORMAT_CAPABILITY_BLEND
#define CAP_DEPTH CRGPU_FORMAT_CAPABILITY_DEPTH_STENCIL_ATTACHMENT
#define CAP_VERTEX CRGPU_FORMAT_CAPABILITY_VERTEX
#define CAP_COPY CRGPU_FORMAT_CAPABILITY_COPY
#define CAP_STORAGE_READ CRGPU_FORMAT_CAPABILITY_STORAGE_READ

static bool name_has(const char *name, const char *part) {
	return strstr(name, part) != NULL;
}

/* Q3's formats, as the draft lists them: every TYPELESS one (the depth
 * view formats included), the formats a backend makes as another (A8_UNORM,
 * B8G8R8X8_UNORM and _SRGB), and the video and palette formats (the packed
 * 4:2:2 ones, the planar ones, AYUV, Y410, Y416, AI44, IA44, P8, A8P8). */
static bool differs_by_backend(CRGPU_FORMAT f) {
	switch (f) {
	case CRGPU_FORMAT_A8_UNORM:
	case CRGPU_FORMAT_B8G8R8X8_UNORM:
	case CRGPU_FORMAT_B8G8R8X8_UNORM_SRGB:
	case CRGPU_FORMAT_X32_TYPELESS_G8X24_UINT:
	case CRGPU_FORMAT_R8G8_B8G8_UNORM:
	case CRGPU_FORMAT_G8R8_G8B8_UNORM:
	case CRGPU_FORMAT_YUY2:
	case CRGPU_FORMAT_Y210:
	case CRGPU_FORMAT_Y216:
	case CRGPU_FORMAT_NV12:
	case CRGPU_FORMAT_P010:
	case CRGPU_FORMAT_P016:
	case CRGPU_FORMAT_420_OPAQUE:
	case CRGPU_FORMAT_NV11:
	case CRGPU_FORMAT_P208:
	case CRGPU_FORMAT_V208:
	case CRGPU_FORMAT_V408:
	case CRGPU_FORMAT_AYUV:
	case CRGPU_FORMAT_Y410:
	case CRGPU_FORMAT_Y416:
	case CRGPU_FORMAT_AI44:
	case CRGPU_FORMAT_IA44:
	case CRGPU_FORMAT_P8:
	case CRGPU_FORMAT_A8P8:
		return true;
	default:
		break;
	}
	for (size_t i = 0; i < FORMAT_COUNT; ++i)
		if (all_formats[i].format == f)
			return name_has(all_formats[i].name, "TYPELESS");
	return false;
}

static bool is_planar(CRGPU_FORMAT f) {
	return f == CRGPU_FORMAT_NV12 || f == CRGPU_FORMAT_P010 || f == CRGPU_FORMAT_P016 || f == CRGPU_FORMAT_420_OPAQUE ||
	       f == CRGPU_FORMAT_NV11 || f == CRGPU_FORMAT_P208 || f == CRGPU_FORMAT_V208 || f == CRGPU_FORMAT_V408;
}

static bool is_bc(CRGPU_FORMAT f) {
	return (f >= CRGPU_FORMAT_BC1_TYPELESS && f <= CRGPU_FORMAT_BC5_SNORM) || (f >= CRGPU_FORMAT_BC6H_TYPELESS && f <= CRGPU_FORMAT_BC7_UNORM_SRGB);
}

static bool is_astc(CRGPU_FORMAT f) {
	return f >= CRGPU_FORMAT_ASTC_4X4_UNORM && f <= CRGPU_FORMAT_ASTC_12X12_UNORM_SRGB;
}

/* An integer colour format: its name ends in _UINT or _SINT, and it isn't
 * a depth format (D24_UNORM_S8_UINT, D32_FLOAT_S8X24_UINT). */
static bool is_integer(const char *name) {
	const size_t n = strlen(name);
	return name[0] != 'D' && n > 5 && (strcmp(name + n - 5, "_UINT") == 0 || strcmp(name + n - 5, "_SINT") == 0);
}

#if CRGPU_COMPILE_METAL_API
/* Apple's "Metal Feature Set Tables" (the edition dated October 23, 2026,
 * pages 11 to 15, "Texture capabilities by pixel format"), transcribed here
 * apart from the backend's copy: for each format the backend maps to a
 * pixel format, its Filter, Write, Color and Blend in the Apple7 and Apple8
 * columns (which every Metal 3 GPU has at least, the table's note 1) and in
 * the Apple9 and Apple10 columns. "All" is all four. */
#define MTL_F 1u
#define MTL_W 2u
#define MTL_C 4u
#define MTL_B 8u
#define MTL_ALL (MTL_F | MTL_W | MTL_C | MTL_B)
typedef struct metal_expected {
	CRGPU_FORMAT format;
	uint8_t apple7_8;
	uint8_t apple9_10;
} metal_expected;
static const metal_expected metal_table[] = {
    {CRGPU_FORMAT_R8_UNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R8_SNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R8_UINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R8_SINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R8G8_UNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R8G8_SNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R8G8_UINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R8G8_SINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R8G8B8A8_UNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R8G8B8A8_UNORM_SRGB, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R8G8B8A8_SNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R8G8B8A8_UINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R8G8B8A8_SINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_B8G8R8A8_UNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_B8G8R8A8_UNORM_SRGB, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R16_FLOAT, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R16_UNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R16_SNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R16_UINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R16_SINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R16G16_FLOAT, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R16G16_UNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R16G16_SNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R16G16_UINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R16G16_SINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R16G16B16A16_FLOAT, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R16G16B16A16_UNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R16G16B16A16_SNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R16G16B16A16_UINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R16G16B16A16_SINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R32_FLOAT, MTL_W | MTL_C | MTL_B, MTL_ALL},
    {CRGPU_FORMAT_R32_UINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R32_SINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R32G32_FLOAT, MTL_W | MTL_C | MTL_B, MTL_ALL},
    {CRGPU_FORMAT_R32G32_UINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R32G32_SINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R32G32B32A32_FLOAT, MTL_W | MTL_C | MTL_B, MTL_ALL},
    {CRGPU_FORMAT_R32G32B32A32_UINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R32G32B32A32_SINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R10G10B10A2_UNORM, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_R10G10B10A2_UINT, MTL_W | MTL_C, MTL_W | MTL_C},
    {CRGPU_FORMAT_R11G11B10_FLOAT, MTL_ALL, MTL_ALL},
    {CRGPU_FORMAT_D16_UNORM, MTL_F, MTL_F},
    {CRGPU_FORMAT_D32_FLOAT, 0, MTL_F},
    {CRGPU_FORMAT_D32_FLOAT_S8X24_UINT, 0, MTL_F},
};

/* What the backend maps to an MTLVertexFormat. */
static bool metal_vertex(CRGPU_FORMAT f) {
	switch (f) {
	case CRGPU_FORMAT_R8_UNORM: case CRGPU_FORMAT_R8_SNORM: case CRGPU_FORMAT_R8_UINT: case CRGPU_FORMAT_R8_SINT:
	case CRGPU_FORMAT_R8G8_UNORM: case CRGPU_FORMAT_R8G8_SNORM: case CRGPU_FORMAT_R8G8_UINT: case CRGPU_FORMAT_R8G8_SINT:
	case CRGPU_FORMAT_R8G8B8A8_UNORM: case CRGPU_FORMAT_R8G8B8A8_SNORM: case CRGPU_FORMAT_R8G8B8A8_UINT: case CRGPU_FORMAT_R8G8B8A8_SINT:
	case CRGPU_FORMAT_R16_FLOAT: case CRGPU_FORMAT_R16_UNORM: case CRGPU_FORMAT_R16_SNORM: case CRGPU_FORMAT_R16_UINT: case CRGPU_FORMAT_R16_SINT:
	case CRGPU_FORMAT_R16G16_FLOAT: case CRGPU_FORMAT_R16G16_UNORM: case CRGPU_FORMAT_R16G16_SNORM: case CRGPU_FORMAT_R16G16_UINT:
	case CRGPU_FORMAT_R16G16_SINT:
	case CRGPU_FORMAT_R16G16B16A16_FLOAT: case CRGPU_FORMAT_R16G16B16A16_UNORM: case CRGPU_FORMAT_R16G16B16A16_SNORM:
	case CRGPU_FORMAT_R16G16B16A16_UINT: case CRGPU_FORMAT_R16G16B16A16_SINT:
	case CRGPU_FORMAT_R32_FLOAT: case CRGPU_FORMAT_R32_UINT: case CRGPU_FORMAT_R32_SINT:
	case CRGPU_FORMAT_R32G32_FLOAT: case CRGPU_FORMAT_R32G32_UINT: case CRGPU_FORMAT_R32G32_SINT:
	case CRGPU_FORMAT_R32G32B32_FLOAT: case CRGPU_FORMAT_R32G32B32_UINT: case CRGPU_FORMAT_R32G32B32_SINT:
	case CRGPU_FORMAT_R32G32B32A32_FLOAT: case CRGPU_FORMAT_R32G32B32A32_UINT: case CRGPU_FORMAT_R32G32B32A32_SINT:
	case CRGPU_FORMAT_R10G10B10A2_UNORM:
		return true;
	default:
		return false;
	}
}

/* MTLReadWriteTextureTier1 and Tier2's pixel formats (Apple's
 * MTLReadWriteTextureTier documentation), as CRGPU_FORMAT values. */
static bool metal_read_write(CRGPU_FORMAT f, uint32_t tier) {
	if (tier >= 1 && (f == CRGPU_FORMAT_R32_FLOAT || f == CRGPU_FORMAT_R32_UINT || f == CRGPU_FORMAT_R32_SINT))
		return true;
	if (tier < 2)
		return false;
	switch (f) {
	case CRGPU_FORMAT_R32G32B32A32_FLOAT: case CRGPU_FORMAT_R32G32B32A32_UINT: case CRGPU_FORMAT_R32G32B32A32_SINT:
	case CRGPU_FORMAT_R16G16B16A16_FLOAT: case CRGPU_FORMAT_R16G16B16A16_UINT: case CRGPU_FORMAT_R16G16B16A16_SINT:
	case CRGPU_FORMAT_R8G8B8A8_UNORM: case CRGPU_FORMAT_R8G8B8A8_UINT: case CRGPU_FORMAT_R8G8B8A8_SINT:
	case CRGPU_FORMAT_R16_FLOAT: case CRGPU_FORMAT_R16_UINT: case CRGPU_FORMAT_R16_SINT:
	case CRGPU_FORMAT_R8_UNORM: case CRGPU_FORMAT_R8_UINT: case CRGPU_FORMAT_R8_SINT:
		return true;
	default:
		return false;
	}
}

/* The capabilities the test expects of [f] on a Metal GPU of [family] (7
 * to 10, or 0 for a GPU in none of them, which has the Apple7 column),
 * from the table above and the GPU's own answers. */
static CRGPU_FORMAT_CAPABILITIES metal_expected_caps(CRGPU_FORMAT f, uint32_t family, bool float32_filtering, uint32_t tier, bool bc,
                                                     bool astc) {
	bool pixel     = false;
	uint8_t letters = 0;
	for (size_t i = 0; i < sizeof(metal_table) / sizeof(metal_table[0]); ++i)
		if (metal_table[i].format == f) {
			pixel   = true;
			letters = family >= 9 ? metal_table[i].apple9_10 : metal_table[i].apple7_8;
		}
	// Every BC format but TYPELESS where the GPU samples BC, every ASTC LDR
	// format where it is in Apple2: Filter in every column.
	if ((is_bc(f) && bc && !differs_by_backend(f)) || (is_astc(f) && astc)) {
		pixel   = true;
		letters = MTL_F;
	}
	CRGPU_FORMAT_CAPABILITIES caps = 0;
	if (pixel) {
		caps |= CAP_SAMPLED;
		if ((letters & MTL_F) || (float32_filtering && (f == CRGPU_FORMAT_R32_FLOAT || f == CRGPU_FORMAT_R32G32_FLOAT ||
		                                                f == CRGPU_FORMAT_R32G32B32A32_FLOAT)))
			caps |= CAP_FILTERED;
		if (letters & MTL_W)
			caps |= CAP_STORAGE;
		if (letters & MTL_C)
			caps |= CAP_COLOR;
		if (letters & MTL_B)
			caps |= CAP_BLEND;
		if (f == CRGPU_FORMAT_D16_UNORM || f == CRGPU_FORMAT_D32_FLOAT || f == CRGPU_FORMAT_D32_FLOAT_S8X24_UINT)
			caps |= CAP_DEPTH;
		if (f != CRGPU_FORMAT_D32_FLOAT_S8X24_UINT)
			caps |= CAP_COPY;
		if ((letters & MTL_W) && metal_read_write(f, tier))
			caps |= CAP_STORAGE_READ;
	}
	if (metal_vertex(f))
		caps |= CAP_VERTEX;
	return caps;
}
#endif

/* crgpu_adapter_get_format_capabilities() on one adapter: every named
 * format answered, the table printed; the refusals, nothing written; the
 * invariants every API keeps; and on Metal each format against the
 * transcribed table, with the draft's exact values on an Apple10 GPU. */
static void test_format_capabilities(
    uint32_t index,
    crgpu_adapter adapter) {
	const CRGPU_ADAPTER_PROPERTIES properties = properties_of(adapter);
	const bool bc                             = (properties.supported_features & CRGPU_FEATURE_TEXTURE_COMPRESSION_BC) != 0;
	const bool astc                           = (properties.supported_features & CRGPU_FEATURE_TEXTURE_COMPRESSION_ASTC_LDR) != 0;

	// The refusals, the output's sentinel unchanged.
	CRGPU_FORMAT_CAPABILITIES flags = 0xDEADBEEFu;
	CHECK(crgpu_adapter_get_format_capabilities(NULL, CRGPU_FORMAT_R8G8B8A8_UNORM, &flags) == CR_RESULT_GPU_INVALID_ADAPTER);
	CHECK(flags == 0xDEADBEEFu);
	CHECK(crgpu_adapter_get_format_capabilities(adapter, CRGPU_FORMAT_R8G8B8A8_UNORM, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	const uint32_t unnamed[] = {0u, 116u, 129u, 133u, 999u, 1028u, UINT32_MAX};
	for (size_t i = 0; i < sizeof(unnamed) / sizeof(unnamed[0]); ++i) {
		flags = 0xDEADBEEFu;
		CHECK(crgpu_adapter_get_format_capabilities(adapter, (CRGPU_FORMAT)unnamed[i], &flags) == CR_RESULT_GPU_INVALID_FORMAT);
		CHECK(flags == 0xDEADBEEFu);
	}
	// The order, two arguments wrong at once.
	flags = 0xDEADBEEFu;
	CHECK(crgpu_adapter_get_format_capabilities(NULL, (CRGPU_FORMAT)116u, &flags) == CR_RESULT_GPU_INVALID_ADAPTER);
	CHECK(flags == 0xDEADBEEFu);
	CHECK(crgpu_adapter_get_format_capabilities(adapter, (CRGPU_FORMAT)116u, NULL) == CR_RESULT_GPU_INVALID_FORMAT);

#if CRGPU_COMPILE_METAL_API
	uint32_t family        = 0;
	bool float32_filtering = false;
	uint32_t tier          = 0;
	CHECK(crgpu_test_metal_format_facts(properties.adapter_id, &family, &float32_filtering, &tier));
	printf("crgpu_device_test: adapter %u: Metal Apple family %u, supports32BitFloatFiltering %s, read-write texture tier %u\n", index,
	       family, float32_filtering ? "yes" : "no", tier);
#endif

	printf("crgpu_device_test: adapter %u (%s): format capabilities (SAMPLED 0x1, FILTERED 0x2, STORAGE 0x4, COLOR_ATTACHMENT 0x8, "
	       "BLEND 0x10, DEPTH_STENCIL_ATTACHMENT 0x20, VERTEX 0x40, COPY 0x80, STORAGE_READ 0x100):\n",
	       index, properties.description);
	uint32_t answered = 0;
	for (size_t i = 0; i < FORMAT_COUNT; ++i) {
		const CRGPU_FORMAT f = all_formats[i].format;
		const char *name     = all_formats[i].name;
		flags                = 0xDEADBEEFu;
		const CR_RESULT r    = crgpu_adapter_get_format_capabilities(adapter, f, &flags);
		CHECK(r == CR_SUCCESS);
		if (r != CR_SUCCESS)
			continue;
		++answered;
		printf("  %4u %-28s 0x%03x\n", (unsigned)f, name, (unsigned)flags);
		bool ok = (flags & ~0x1FFu) == 0;
		// Every API keeps these.
		ok = ok && (!(flags & CAP_FILTERED) || (flags & CAP_SAMPLED));
		ok = ok && (!(flags & CAP_BLEND) || (flags & CAP_COLOR));
		ok = ok && (!(flags & CAP_STORAGE_READ) || (flags & CAP_STORAGE));
		if (differs_by_backend(f))
			ok = ok && (flags & ~CAP_COPY) == 0;
		if (f == CRGPU_FORMAT_R1_UNORM || is_planar(f) || f == CRGPU_FORMAT_D24_UNORM_S8_UINT || f == CRGPU_FORMAT_D32_FLOAT_S8X24_UINT)
			ok = ok && !(flags & CAP_COPY);
		if (is_bc(f) || is_astc(f)) {
			ok = ok && (flags & ~(CAP_SAMPLED | CAP_FILTERED | CAP_COPY)) == 0;
			if ((is_bc(f) && !bc) || (is_astc(f) && !astc))
				ok = ok && flags == 0;
		}
		if (is_integer(name))
			ok = ok && !(flags & (CAP_FILTERED | CAP_BLEND));
#if CRGPU_COMPILE_METAL_API
		const CRGPU_FORMAT_CAPABILITIES want = metal_expected_caps(f, family, float32_filtering, tier, bc, astc);
		if (flags != want)
			fprintf(stderr, "  %s: 0x%03x, the transcribed table says 0x%03x\n", name, (unsigned)flags, (unsigned)want);
		ok = ok && flags == want;
#endif
		if (!ok)
			fprintf(stderr, "  %s: 0x%03x breaks a rule\n", name, (unsigned)flags);
		CHECK(ok);
	}
	CHECK(answered == FORMAT_COUNT);
#if CRGPU_COMPILE_METAL_API
	// Exact where the pixel format's kind fixes the answer (the draft's
	// list), on the M5 Pro's family.
	if (family >= 10) {
		const struct {
			CRGPU_FORMAT format;
			CRGPU_FORMAT_CAPABILITIES caps;
		} exact[] = {
		    {CRGPU_FORMAT_R32G32B32_FLOAT, 0x40},  {CRGPU_FORMAT_R8G8B8A8_TYPELESS, 0}, {CRGPU_FORMAT_D24_UNORM_S8_UINT, 0},
		    {CRGPU_FORMAT_BC7_UNORM, 0x83},        {CRGPU_FORMAT_ASTC_4X4_UNORM, 0x83}, {CRGPU_FORMAT_R32_UINT, 0x1CD},
		};
		for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); ++i) {
			flags = 0xDEADBEEFu;
			CHECK(crgpu_adapter_get_format_capabilities(adapter, exact[i].format, &flags) == CR_SUCCESS);
			if (flags != exact[i].caps)
				fprintf(stderr, "  format %u: 0x%03x, expected 0x%03x\n", (unsigned)exact[i].format, (unsigned)flags, (unsigned)exact[i].caps);
			CHECK(flags == exact[i].caps);
		}
	}
#endif
}

static void test_device_desc_checks(crgpu_adapter adapter) {
	const CRGPU_ADAPTER_PROPERTIES properties = properties_of(adapter);
	crgpu_device device                       = (crgpu_device)(uintptr_t)1;
	crgpu_device_desc desc                    = device_desc();

	CHECK(crgpu_device_create(NULL, &desc, &device) == CR_RESULT_GPU_INVALID_ADAPTER);
	CHECK(crgpu_device_create(adapter, NULL, &device) == CR_RESULT_NULL_DESC);
	desc.base.type = CR_STRUCT_TYPE_GPU_INSTANCE_DESC;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_STRUCT_WRONG_TYPE);
	desc = device_desc();
	CHECK(crgpu_device_create(adapter, &desc, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);

	// Refused from here: the output is NULL.
	device                 = (crgpu_device)(uintptr_t)1;
	desc.required_features = UINT64_C(1) << 63;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_GPU_FEATURE_NOT_SUPPORTED);
	CHECK(device == NULL);
	desc = device_desc();

	desc.direct_queue_count = 0;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_ZERO_SIZE);
	desc = device_desc();

	for (uint32_t i = 0; i < properties.queue_pool_count; ++i) {
		const CRGPU_QUEUE_POOL *pool = &properties.queue_pools[i];
		if (pool->max_queues == UINT32_MAX)
			continue;
		crgpu_device_desc too_many = device_desc();
		too_many.direct_queue_count  = 0;
		if (pool->kinds & CRGPU_QUEUE_KIND_DIRECT)
			too_many.direct_queue_count = pool->max_queues + 1;
		else if (pool->kinds & CRGPU_QUEUE_KIND_COMPUTE)
			too_many.compute_queue_count = pool->max_queues + 1;
		else
			too_many.copy_queue_count = pool->max_queues + 1;
		CHECK(crgpu_device_create(adapter, &too_many, &device) == CR_RESULT_GPU_TOO_MANY_QUEUES);
	}

	if (properties.max_descriptors < UINT32_MAX) {
		desc.max_descriptors = properties.max_descriptors + 1;
		CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE);
		desc = device_desc();
	}
	if (properties.max_samplers < UINT32_MAX) {
		desc.max_samplers = properties.max_samplers + 1;
		CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE);
		desc = device_desc();
	}

#if CRGPU_EXT_BINDLESS
	if (properties.supported_features & CRGPU_FEATURE_BINDLESS) {
		// The table comes out of the budget, which 16 doesn't hold.
		desc.required_features = CRGPU_FEATURE_BINDLESS;
		CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_GPU_OUT_OF_DESCRIPTORS);
		desc = device_desc();
	}
#endif
}

static void test_queues(crgpu_device device) {
	crgpu_command_queue queue = NULL;
	crgpu_command_queue again = NULL;
	CHECK(crgpu_command_queue_get(NULL, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue) == CR_RESULT_GPU_INVALID_DEVICE);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_BUNDLE, 0, &queue) ==
	      CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE);
	CHECK(crgpu_command_queue_get(device, 99, 0, &queue) == CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);

	// Two direct queues, one compute, no copy queue.
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue) == CR_SUCCESS);
	CHECK(queue != NULL);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &again) == CR_SUCCESS);
	CHECK(queue == again);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 1, &again) == CR_SUCCESS);
	CHECK(again != NULL && again != queue);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 2, &again) == CR_RESULT_INDEX_OUT_OF_RANGE);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_COMPUTE, 0, &again) == CR_SUCCESS);
	CHECK(again != NULL);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_COPY, 0, &again) == CR_RESULT_INDEX_OUT_OF_RANGE);

	// An allocator of a kind the device has no queue of is refused; a bundle
	// allocator needs a direct queue.
	crgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type      = CRGPU_COMMAND_LIST_TYPE_COPY;
	crgpu_command_allocator allocator = NULL;
	CHECK(crgpu_command_allocator_create(device, &allocator_desc, &allocator) == CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE);
	allocator_desc.type = CRGPU_COMMAND_LIST_TYPE_DIRECT;
	CHECK(crgpu_command_allocator_create(device, &allocator_desc, &allocator) == CR_SUCCESS);
	crgpu_command_list list = NULL;
	CHECK(crgpu_command_list_create(allocator, &list) == CR_SUCCESS);
	crgpu_command_list_destroy(list);
	crgpu_command_allocator_destroy(allocator);
}

static crgpu_descriptor_pool make_pool(crgpu_device device, CRGPU_DESCRIPTOR_TYPE type, uint32_t count, CR_RESULT *out_result) {
	crgpu_descriptor_pool_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                  = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	desc.max_sets                   = 1;
	desc.pool_size_count            = 1;
	desc.pool_sizes[0].descriptor_type = type;
	desc.pool_sizes[0].count        = count;
	crgpu_descriptor_pool pool    = NULL;
	*out_result                     = crgpu_descriptor_pool_create(device, &desc, &pool);
	return pool;
}

static void test_budget(crgpu_device device) {
	// The device's budget is 16 descriptors and 4 samplers.
	CR_RESULT result;
	crgpu_descriptor_pool first = make_pool(device, CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 12, &result);
	CHECK(result == CR_SUCCESS);
	make_pool(device, CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5, &result);
	CHECK(result == CR_RESULT_GPU_OUT_OF_DESCRIPTORS);
	crgpu_descriptor_pool second = make_pool(device, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4, &result);
	CHECK(result == CR_SUCCESS);
	make_pool(device, CRGPU_DESCRIPTOR_TYPE_SAMPLER, 5, &result);
	CHECK(result == CR_RESULT_GPU_OUT_OF_DESCRIPTORS);
	crgpu_descriptor_pool samplers = make_pool(device, CRGPU_DESCRIPTOR_TYPE_SAMPLER, 4, &result);
	CHECK(result == CR_SUCCESS);

	// What a destroyed pool held is given back.
	crgpu_descriptor_pool_destroy(first);
	crgpu_descriptor_pool third = make_pool(device, CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12, &result);
	CHECK(result == CR_SUCCESS);

	crgpu_descriptor_pool_destroy(third);
	crgpu_descriptor_pool_destroy(samplers);
	crgpu_descriptor_pool_destroy(second);
}

static void test_features(crgpu_adapter adapter, crgpu_device device) {
	const CRGPU_ADAPTER_PROPERTIES properties = properties_of(adapter);
	CHECK(crgpu_device_get_adapter(device) == adapter);
	CHECK(crgpu_device_get_adapter(NULL) == NULL);
	CHECK(crgpu_device_get_features(NULL) == 0);

	// The device has exactly what it was created with: here, nothing. A
	// feature the adapter has is still refused.
	CHECK(crgpu_device_get_features(device) == 0);
	if (properties.supported_features & CRGPU_FEATURE_PASS_TIMESTAMPS) {
		crgpu_command_queue queue = NULL;
		CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue) == CR_SUCCESS);
		crgpu_timestamp_set set = NULL;
		CHECK(crgpu_timestamp_set_create(device, queue, 2, &set) == CR_RESULT_GPU_TIMESTAMPS_NOT_SUPPORTED);
	}
}

static void test_timestamps_when_asked(crgpu_adapter adapter) {
	const CRGPU_ADAPTER_PROPERTIES properties = properties_of(adapter);
	if (!(properties.supported_features & CRGPU_FEATURE_PASS_TIMESTAMPS))
		return;
	crgpu_device_desc desc = device_desc();
	desc.required_features   = CRGPU_FEATURE_PASS_TIMESTAMPS;
	crgpu_device device    = NULL;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_SUCCESS);
	if (!device)
		return;
	CHECK(crgpu_device_get_features(device) == CRGPU_FEATURE_PASS_TIMESTAMPS);
	crgpu_command_queue queue = NULL;
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue) == CR_SUCCESS);
	crgpu_timestamp_set set = NULL;
	CHECK(crgpu_timestamp_set_create(device, queue, 2, &set) == CR_SUCCESS);
	crgpu_timestamp_set_destroy(set);
	crgpu_device_destroy(device);
}

/* The instance's create on an allocator that refuses its n-th request alone,
 * for each n until the create no longer asks that many: each refusal is
 * CR_RESULT_OUT_OF_MEMORY with nothing made and nothing left allocated
 * (the allocator's contract), so a failed request never passes for an
 * extension or an adapter missing; and the instance made when none is
 * refused lists all [adapter_count] adapters. Returns how many requests
 * the create made. */
static size_t test_instance_out_of_memory(uint32_t adapter_count) {
	size_t n = 1;
	for (;; ++n) {
		crtest_allocator tracker;
		CR_ALLOCATOR allocator  = crtest_allocator_make(&tracker);
		tracker.refuse_only     = n;
		crgpu_instance_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type          = CR_STRUCT_TYPE_GPU_INSTANCE_DESC;
		desc.allocator          = &allocator;
		desc.application_name   = "crgpu_device_test";
		desc.engine_name        = "crgpu_device_test";
		crgpu_instance instance = NULL;
		const CR_RESULT result  = crgpu_instance_create(&desc, &instance);
		if (result == CR_SUCCESS) {
			// Made although its n-th request was refused: the refusal
			// passed for something else.
			const bool refused = tracker.asked >= n;
			if (refused)
				fprintf(stderr, "  request %zu refused: the instance was made\n", n);
			CHECK(!refused);
			const crgpu_adapter *adapters = NULL;
			uint32_t count                = 0;
			crgpu_adapter_enumerate(instance, &adapters, &count);
			if (!refused && count != adapter_count)
				fprintf(stderr, "  %u adapters listed of %u\n", count, adapter_count);
			CHECK(refused || count == adapter_count);
			CHECK(crgpu_instance_destroy(instance) == CR_SUCCESS);
			CHECK(crtest_allocator_clean(&tracker));
			if (!refused)
				break;
			continue;
		}
		if (result != CR_RESULT_OUT_OF_MEMORY)
			fprintf(stderr, "  request %zu refused: %s\n", n, cr_result_str(result));
		CHECK(result == CR_RESULT_OUT_OF_MEMORY);
		CHECK(instance == NULL);
		CHECK(crtest_allocator_clean(&tracker));
		if (n > 100000)
			break;
	}
	return n - 1;
}

int main(void) {
	crtest_allocator tracker;
	CR_ALLOCATOR allocator = crtest_allocator_make(&tracker);

	crgpu_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type        = CR_STRUCT_TYPE_GPU_INSTANCE_DESC;
	instance_desc.allocator        = &allocator;
	instance_desc.application_name = "crgpu_device_test";
	instance_desc.engine_name      = "crgpu_device_test";
	crgpu_instance instance      = NULL;
	if (crgpu_instance_create(&instance_desc, &instance) != CR_SUCCESS) {
		fprintf(stderr, "crgpu_device_test: no instance\n");
		return 1;
	}

	test_enumerate(instance);
	const crgpu_adapter *adapters = NULL;
	uint32_t adapter_count          = 0;
	crgpu_adapter_enumerate(instance, &adapters, &adapter_count);
	if (adapter_count == 0) {
		printf("crgpu_device_test: no adapter, skipped\n");
		crgpu_instance_destroy(instance);
		return SKIPPED;
	}
	const crgpu_adapter adapter = adapters[0];

	for (uint32_t i = 0; i < adapter_count; ++i) {
		test_image_limits(i, adapters[i]);
		test_block_format_bits(i, adapters[i]);
		test_dual_source_bit(i, adapters[i]);
		test_format_capabilities(i, adapters[i]);
	}
	test_adapter_properties(adapter);
	test_device_desc_checks(adapter);

	// A device of two direct queues and one compute queue, no copy queue,
	// no features, a budget of 16 descriptors and 4 samplers.
	const CRGPU_ADAPTER_PROPERTIES properties = properties_of(adapter);
	crgpu_device_desc desc                    = device_desc();
	desc.direct_queue_count                     = 2;
	desc.compute_queue_count                    = 1;
	// Where compute shares the direct queues' pool, the pool must hold three.
	bool fits = true;
	for (uint32_t i = 0; i < properties.queue_pool_count; ++i) {
		const CRGPU_QUEUE_POOL *pool = &properties.queue_pools[i];
		uint64_t asked                 = 0;
		if (pool->kinds & CRGPU_QUEUE_KIND_DIRECT)
			asked += 2;
		if (pool->kinds & CRGPU_QUEUE_KIND_COMPUTE)
			asked += 1;
		fits = fits && asked <= pool->max_queues;
	}
	if (!fits) {
		printf("crgpu_device_test: the adapter's pools can't hold 2 direct and 1 compute queue; skipped the queue tests\n");
		desc.direct_queue_count  = 1;
		desc.compute_queue_count = 0;
	}

	crgpu_device device = NULL;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_SUCCESS);
	if (device) {
		// One device per adapter at a time.
		crgpu_device second = (crgpu_device)(uintptr_t)1;
		CHECK(crgpu_device_create(adapter, &desc, &second) == CR_RESULT_GPU_ADAPTER_IN_USE);
		CHECK(second == NULL);

		if (fits)
			test_queues(device);
		test_budget(device);
		test_features(adapter, device);
		crgpu_device_destroy(device);
	}
	crgpu_device_destroy(NULL);

	// The adapter is free again: a device is made on it as before, which is
	// also the way back from a lost device.
	test_timestamps_when_asked(adapter);
	device = NULL;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_SUCCESS);
	crgpu_device_destroy(device);

	CHECK(crgpu_instance_destroy(instance) == CR_SUCCESS);
	CHECK(crtest_allocator_clean(&tracker));

	const size_t requests = test_instance_out_of_memory(adapter_count);
	printf("crgpu_device_test: the instance's create, each of its %zu requests refused alone\n", requests);

	if (failures)
		fprintf(stderr, "crgpu_device_test: %d checks failed\n", failures);
	else
		printf("crgpu_device_test: all checks passed\n");
	return failures ? 1 : 0;
}
