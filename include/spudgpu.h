//
// Created by nathanmoore on 5/16/26.
//

/**
 * @file spudgpu.h
 * @brief SpudGPU: one C interface over the Vulkan, Direct3D 12 and Metal
 * graphics APIs.
 *
 * Every object is an opaque handle the caller creates, owns and destroys.
 * Functions that can fail return a SPUDRESULT (see spudcore.h); the
 * `spudgpu_cmd_*` functions record into a command list and return nothing.
 *
 * @par Threads
 * The library rule holds (@ref spud_threads "Threads", spudcore.h): every
 * handle - an instance, a device, a queue, a command allocator, a command
 * list, a swap chain - is used by one caller at a time, on every backend,
 * whatever the graphics API underneath would allow. A `spudgpu_cmd_*` call
 * is a use of the command list it records into. What SpudGPU keeps behind
 * the handles (an adapter's one device, a device's descriptor budget, the
 * bindless table, D3D12's shared descriptor heaps) it guards itself. The
 * calls that may overlap others on one object say so with `Thread:`.
 */

#ifndef SPUDLIB_SPUDGPU_H
#define SPUDLIB_SPUDGPU_H

#include "spudcore.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {

#endif // __cplusplus

#if SPUDGPU_COMPILE_VULKAN_API
#endif

#if SPUDGPU_COMPILE_D3D12_API
#endif

#if SPUDGPU_COMPILE_METAL_API
#endif

/**
 * @brief Pixel and vertex-attribute data format.
 *
 * Numbered like DXGI_FORMAT.
 */
typedef uint32_t SPUDGPU_FORMAT;
enum {
	SPUDGPU_FORMAT_UNKNOWN                    = 0, /**< No format. */
	SPUDGPU_FORMAT_R32G32B32A32_TYPELESS      = 1, /**< DXGI_FORMAT_R32G32B32A32_TYPELESS. */
	SPUDGPU_FORMAT_R32G32B32A32_FLOAT         = 2, /**< DXGI_FORMAT_R32G32B32A32_FLOAT. */
	SPUDGPU_FORMAT_R32G32B32A32_UINT          = 3, /**< DXGI_FORMAT_R32G32B32A32_UINT. */
	SPUDGPU_FORMAT_R32G32B32A32_SINT          = 4, /**< DXGI_FORMAT_R32G32B32A32_SINT. */
	SPUDGPU_FORMAT_R32G32B32_TYPELESS         = 5, /**< DXGI_FORMAT_R32G32B32_TYPELESS. */
	SPUDGPU_FORMAT_R32G32B32_FLOAT            = 6, /**< DXGI_FORMAT_R32G32B32_FLOAT. */
	SPUDGPU_FORMAT_R32G32B32_UINT             = 7, /**< DXGI_FORMAT_R32G32B32_UINT. */
	SPUDGPU_FORMAT_R32G32B32_SINT             = 8, /**< DXGI_FORMAT_R32G32B32_SINT. */
	SPUDGPU_FORMAT_R16G16B16A16_TYPELESS      = 9, /**< DXGI_FORMAT_R16G16B16A16_TYPELESS. */
	SPUDGPU_FORMAT_R16G16B16A16_FLOAT         = 10, /**< DXGI_FORMAT_R16G16B16A16_FLOAT. */
	SPUDGPU_FORMAT_R16G16B16A16_UNORM         = 11, /**< DXGI_FORMAT_R16G16B16A16_UNORM. */
	SPUDGPU_FORMAT_R16G16B16A16_UINT          = 12, /**< DXGI_FORMAT_R16G16B16A16_UINT. */
	SPUDGPU_FORMAT_R16G16B16A16_SNORM         = 13, /**< DXGI_FORMAT_R16G16B16A16_SNORM. */
	SPUDGPU_FORMAT_R16G16B16A16_SINT          = 14, /**< DXGI_FORMAT_R16G16B16A16_SINT. */
	SPUDGPU_FORMAT_R32G32_TYPELESS            = 15, /**< DXGI_FORMAT_R32G32_TYPELESS. */
	SPUDGPU_FORMAT_R32G32_FLOAT               = 16, /**< DXGI_FORMAT_R32G32_FLOAT. */
	SPUDGPU_FORMAT_R32G32_UINT                = 17, /**< DXGI_FORMAT_R32G32_UINT. */
	SPUDGPU_FORMAT_R32G32_SINT                = 18, /**< DXGI_FORMAT_R32G32_SINT. */
	SPUDGPU_FORMAT_R32G8X24_TYPELESS          = 19, /**< DXGI_FORMAT_R32G8X24_TYPELESS. */
	SPUDGPU_FORMAT_D32_FLOAT_S8X24_UINT       = 20, /**< DXGI_FORMAT_D32_FLOAT_S8X24_UINT. */
	SPUDGPU_FORMAT_R32_FLOAT_X8X24_TYPELESS   = 21, /**< DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS. */
	SPUDGPU_FORMAT_X32_TYPELESS_G8X24_UINT    = 22, /**< DXGI_FORMAT_X32_TYPELESS_G8X24_UINT. */
	SPUDGPU_FORMAT_R10G10B10A2_TYPELESS       = 23, /**< DXGI_FORMAT_R10G10B10A2_TYPELESS. */
	SPUDGPU_FORMAT_R10G10B10A2_UNORM          = 24, /**< DXGI_FORMAT_R10G10B10A2_UNORM. */
	SPUDGPU_FORMAT_R10G10B10A2_UINT           = 25, /**< DXGI_FORMAT_R10G10B10A2_UINT. */
	SPUDGPU_FORMAT_R11G11B10_FLOAT            = 26, /**< DXGI_FORMAT_R11G11B10_FLOAT. */
	SPUDGPU_FORMAT_R8G8B8A8_TYPELESS          = 27, /**< DXGI_FORMAT_R8G8B8A8_TYPELESS. */
	SPUDGPU_FORMAT_R8G8B8A8_UNORM             = 28, /**< DXGI_FORMAT_R8G8B8A8_UNORM. */
	SPUDGPU_FORMAT_R8G8B8A8_UNORM_SRGB        = 29, /**< DXGI_FORMAT_R8G8B8A8_UNORM_SRGB. */
	SPUDGPU_FORMAT_R8G8B8A8_UINT              = 30, /**< DXGI_FORMAT_R8G8B8A8_UINT. */
	SPUDGPU_FORMAT_R8G8B8A8_SNORM             = 31, /**< DXGI_FORMAT_R8G8B8A8_SNORM. */
	SPUDGPU_FORMAT_R8G8B8A8_SINT              = 32, /**< DXGI_FORMAT_R8G8B8A8_SINT. */
	SPUDGPU_FORMAT_R16G16_TYPELESS            = 33, /**< DXGI_FORMAT_R16G16_TYPELESS. */
	SPUDGPU_FORMAT_R16G16_FLOAT               = 34, /**< DXGI_FORMAT_R16G16_FLOAT. */
	SPUDGPU_FORMAT_R16G16_UNORM               = 35, /**< DXGI_FORMAT_R16G16_UNORM. */
	SPUDGPU_FORMAT_R16G16_UINT                = 36, /**< DXGI_FORMAT_R16G16_UINT. */
	SPUDGPU_FORMAT_R16G16_SNORM               = 37, /**< DXGI_FORMAT_R16G16_SNORM. */
	SPUDGPU_FORMAT_R16G16_SINT                = 38, /**< DXGI_FORMAT_R16G16_SINT. */
	SPUDGPU_FORMAT_R32_TYPELESS               = 39, /**< DXGI_FORMAT_R32_TYPELESS. */
	SPUDGPU_FORMAT_D32_FLOAT                  = 40, /**< DXGI_FORMAT_D32_FLOAT. */
	SPUDGPU_FORMAT_R32_FLOAT                  = 41, /**< DXGI_FORMAT_R32_FLOAT. */
	SPUDGPU_FORMAT_R32_UINT                   = 42, /**< DXGI_FORMAT_R32_UINT. */
	SPUDGPU_FORMAT_R32_SINT                   = 43, /**< DXGI_FORMAT_R32_SINT. */
	SPUDGPU_FORMAT_R24G8_TYPELESS             = 44, /**< DXGI_FORMAT_R24G8_TYPELESS. */
	SPUDGPU_FORMAT_D24_UNORM_S8_UINT          = 45, /**< DXGI_FORMAT_D24_UNORM_S8_UINT. */
	SPUDGPU_FORMAT_R24_UNORM_X8_TYPELESS      = 46, /**< DXGI_FORMAT_R24_UNORM_X8_TYPELESS. */
	SPUDGPU_FORMAT_X24_TYPELESS_G8_UINT       = 47, /**< DXGI_FORMAT_X24_TYPELESS_G8_UINT. */
	SPUDGPU_FORMAT_R8G8_TYPELESS              = 48, /**< DXGI_FORMAT_R8G8_TYPELESS. */
	SPUDGPU_FORMAT_R8G8_UNORM                 = 49, /**< DXGI_FORMAT_R8G8_UNORM. */
	SPUDGPU_FORMAT_R8G8_UINT                  = 50, /**< DXGI_FORMAT_R8G8_UINT. */
	SPUDGPU_FORMAT_R8G8_SNORM                 = 51, /**< DXGI_FORMAT_R8G8_SNORM. */
	SPUDGPU_FORMAT_R8G8_SINT                  = 52, /**< DXGI_FORMAT_R8G8_SINT. */
	SPUDGPU_FORMAT_R16_TYPELESS               = 53, /**< DXGI_FORMAT_R16_TYPELESS. */
	SPUDGPU_FORMAT_R16_FLOAT                  = 54, /**< DXGI_FORMAT_R16_FLOAT. */
	SPUDGPU_FORMAT_D16_UNORM                  = 55, /**< DXGI_FORMAT_D16_UNORM. */
	SPUDGPU_FORMAT_R16_UNORM                  = 56, /**< DXGI_FORMAT_R16_UNORM. */
	SPUDGPU_FORMAT_R16_UINT                   = 57, /**< DXGI_FORMAT_R16_UINT. */
	SPUDGPU_FORMAT_R16_SNORM                  = 58, /**< DXGI_FORMAT_R16_SNORM. */
	SPUDGPU_FORMAT_R16_SINT                   = 59, /**< DXGI_FORMAT_R16_SINT. */
	SPUDGPU_FORMAT_R8_TYPELESS                = 60, /**< DXGI_FORMAT_R8_TYPELESS. */
	SPUDGPU_FORMAT_R8_UNORM                   = 61, /**< DXGI_FORMAT_R8_UNORM. */
	SPUDGPU_FORMAT_R8_UINT                    = 62, /**< DXGI_FORMAT_R8_UINT. */
	SPUDGPU_FORMAT_R8_SNORM                   = 63, /**< DXGI_FORMAT_R8_SNORM. */
	SPUDGPU_FORMAT_R8_SINT                    = 64, /**< DXGI_FORMAT_R8_SINT. */
	SPUDGPU_FORMAT_A8_UNORM                   = 65, /**< DXGI_FORMAT_A8_UNORM. */
	SPUDGPU_FORMAT_R1_UNORM                   = 66, /**< DXGI_FORMAT_R1_UNORM. */
	SPUDGPU_FORMAT_R9G9B9E5_SHAREDEXP         = 67, /**< DXGI_FORMAT_R9G9B9E5_SHAREDEXP. */
	SPUDGPU_FORMAT_R8G8_B8G8_UNORM            = 68, /**< DXGI_FORMAT_R8G8_B8G8_UNORM. */
	SPUDGPU_FORMAT_G8R8_G8B8_UNORM            = 69, /**< DXGI_FORMAT_G8R8_G8B8_UNORM. */
	SPUDGPU_FORMAT_BC1_TYPELESS               = 70, /**< DXGI_FORMAT_BC1_TYPELESS. */
	SPUDGPU_FORMAT_BC1_UNORM                  = 71, /**< DXGI_FORMAT_BC1_UNORM. */
	SPUDGPU_FORMAT_BC1_UNORM_SRGB             = 72, /**< DXGI_FORMAT_BC1_UNORM_SRGB. */
	SPUDGPU_FORMAT_BC2_TYPELESS               = 73, /**< DXGI_FORMAT_BC2_TYPELESS. */
	SPUDGPU_FORMAT_BC2_UNORM                  = 74, /**< DXGI_FORMAT_BC2_UNORM. */
	SPUDGPU_FORMAT_BC2_UNORM_SRGB             = 75, /**< DXGI_FORMAT_BC2_UNORM_SRGB. */
	SPUDGPU_FORMAT_BC3_TYPELESS               = 76, /**< DXGI_FORMAT_BC3_TYPELESS. */
	SPUDGPU_FORMAT_BC3_UNORM                  = 77, /**< DXGI_FORMAT_BC3_UNORM. */
	SPUDGPU_FORMAT_BC3_UNORM_SRGB             = 78, /**< DXGI_FORMAT_BC3_UNORM_SRGB. */
	SPUDGPU_FORMAT_BC4_TYPELESS               = 79, /**< DXGI_FORMAT_BC4_TYPELESS. */
	SPUDGPU_FORMAT_BC4_UNORM                  = 80, /**< DXGI_FORMAT_BC4_UNORM. */
	SPUDGPU_FORMAT_BC4_SNORM                  = 81, /**< DXGI_FORMAT_BC4_SNORM. */
	SPUDGPU_FORMAT_BC5_TYPELESS               = 82, /**< DXGI_FORMAT_BC5_TYPELESS. */
	SPUDGPU_FORMAT_BC5_UNORM                  = 83, /**< DXGI_FORMAT_BC5_UNORM. */
	SPUDGPU_FORMAT_BC5_SNORM                  = 84, /**< DXGI_FORMAT_BC5_SNORM. */
	SPUDGPU_FORMAT_B5G6R5_UNORM               = 85, /**< DXGI_FORMAT_B5G6R5_UNORM. */
	SPUDGPU_FORMAT_B5G5R5A1_UNORM             = 86, /**< DXGI_FORMAT_B5G5R5A1_UNORM. */
	SPUDGPU_FORMAT_B8G8R8A8_UNORM             = 87, /**< DXGI_FORMAT_B8G8R8A8_UNORM. */
	SPUDGPU_FORMAT_B8G8R8X8_UNORM             = 88, /**< DXGI_FORMAT_B8G8R8X8_UNORM. */
	SPUDGPU_FORMAT_R10G10B10_XR_BIAS_A2_UNORM = 89, /**< DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM. */
	SPUDGPU_FORMAT_B8G8R8A8_TYPELESS          = 90, /**< DXGI_FORMAT_B8G8R8A8_TYPELESS. */
	SPUDGPU_FORMAT_B8G8R8A8_UNORM_SRGB        = 91, /**< DXGI_FORMAT_B8G8R8A8_UNORM_SRGB. */
	SPUDGPU_FORMAT_B8G8R8X8_TYPELESS          = 92, /**< DXGI_FORMAT_B8G8R8X8_TYPELESS. */
	SPUDGPU_FORMAT_B8G8R8X8_UNORM_SRGB        = 93, /**< DXGI_FORMAT_B8G8R8X8_UNORM_SRGB. */
	SPUDGPU_FORMAT_BC6H_TYPELESS              = 94, /**< DXGI_FORMAT_BC6H_TYPELESS. */
	SPUDGPU_FORMAT_BC6H_UF16                  = 95, /**< DXGI_FORMAT_BC6H_UF16. */
	SPUDGPU_FORMAT_BC6H_SF16                  = 96, /**< DXGI_FORMAT_BC6H_SF16. */
	SPUDGPU_FORMAT_BC7_TYPELESS               = 97, /**< DXGI_FORMAT_BC7_TYPELESS. */
	SPUDGPU_FORMAT_BC7_UNORM                  = 98, /**< DXGI_FORMAT_BC7_UNORM. */
	SPUDGPU_FORMAT_BC7_UNORM_SRGB             = 99, /**< DXGI_FORMAT_BC7_UNORM_SRGB. */
	SPUDGPU_FORMAT_AYUV                       = 100, /**< DXGI_FORMAT_AYUV. */
	SPUDGPU_FORMAT_Y410                       = 101, /**< DXGI_FORMAT_Y410. */
	SPUDGPU_FORMAT_Y416                       = 102, /**< DXGI_FORMAT_Y416. */
	SPUDGPU_FORMAT_NV12                       = 103, /**< DXGI_FORMAT_NV12. */
	SPUDGPU_FORMAT_P010                       = 104, /**< DXGI_FORMAT_P010. */
	SPUDGPU_FORMAT_P016                       = 105, /**< DXGI_FORMAT_P016. */
	SPUDGPU_FORMAT_420_OPAQUE                 = 106, /**< DXGI_FORMAT_420_OPAQUE. */
	SPUDGPU_FORMAT_YUY2                       = 107, /**< DXGI_FORMAT_YUY2. */
	SPUDGPU_FORMAT_Y210                       = 108, /**< DXGI_FORMAT_Y210. */
	SPUDGPU_FORMAT_Y216                       = 109, /**< DXGI_FORMAT_Y216. */
	SPUDGPU_FORMAT_NV11                       = 110, /**< DXGI_FORMAT_NV11. */
	SPUDGPU_FORMAT_AI44                       = 111, /**< DXGI_FORMAT_AI44. */
	SPUDGPU_FORMAT_IA44                       = 112, /**< DXGI_FORMAT_IA44. */
	SPUDGPU_FORMAT_P8                         = 113, /**< DXGI_FORMAT_P8. */
	SPUDGPU_FORMAT_A8P8                       = 114, /**< DXGI_FORMAT_A8P8. */
	SPUDGPU_FORMAT_B4G4R4A4_UNORM             = 115, /**< DXGI_FORMAT_B4G4R4A4_UNORM. */

	SPUDGPU_FORMAT_P208 = 130, /**< DXGI_FORMAT_P208. */
	SPUDGPU_FORMAT_V208 = 131, /**< DXGI_FORMAT_V208. */
	SPUDGPU_FORMAT_V408 = 132, /**< DXGI_FORMAT_V408. */

	SPUDGPU_FORMAT_FORCE_UINT = -1 /**< 0xffffffff */
};

/**
 * @brief Returns the size of one pixel of a format, in bits.
 *
 * Divide the result by 8 for the size in bytes.
 *
 * @param[in] fmt Format to query.
 *
 * @return Bits per pixel, or 0 if `fmt` is not a format with a known pixel
 *         size.
 */
uint32_t spudgpu_format_bit_count(SPUDGPU_FORMAT fmt);

/**
 * @brief Looks up a format by its name.
 *
 * @param[in] fmt_str Null-terminated format name.
 *
 * @return The matching SPUDGPU_FORMAT, or SPUDGPU_FORMAT_UNKNOWN if `fmt_str`
 *         is NULL or does not match a known format name.
 */
SPUDGPU_FORMAT spudgpu_format_from_string(const char *fmt_str);

/** Opaque handle to a SpudGPU instance: one connection to the compiled-in graphics API. */
typedef struct spudgpu_instance_t *spudgpu_instance;
/** Opaque handle to a GPU the graphics API lists: what it is and what it can
 * do. Owned by the instance; listing one creates nothing on it. */
typedef struct spudgpu_adapter_t *spudgpu_adapter;
/** Opaque handle to a device: the caller's connection to one adapter, with
 * the features and queues it asked for. */
typedef struct spudgpu_device_t *spudgpu_device;
/** Opaque handle to a GPU buffer. */
typedef struct spudgpu_buffer_t *spudgpu_buffer;
/** Opaque handle to a view of a range of a buffer. */
typedef struct spudgpu_buffer_view_t *spudgpu_buffer_view;
/** Opaque handle to a GPU image (texture). */
typedef struct spudgpu_image_t *spudgpu_image;
/** Opaque handle to a view of an image's subresources. */
typedef struct spudgpu_image_view_t *spudgpu_image_view;
/** Opaque handle to a heap: one device allocation buffers and images are placed in. */
typedef struct spudgpu_heap_t *spudgpu_heap;
/** Opaque handle to a graphics pipeline. */
typedef struct spudgpu_shader_pipeline_t *spudgpu_shader_pipeline;
/** Opaque handle to a command list. */
typedef struct spudgpu_command_list_t *spudgpu_command_list;
/** Opaque handle to a command allocator, the memory command lists record into. */
typedef struct spudgpu_command_allocator_t *spudgpu_command_allocator;
/** Opaque handle to a command queue. */
typedef struct spudgpu_command_queue_t *spudgpu_command_queue;
/** Opaque handle to a swap chain. */
typedef struct spudgpu_swap_chain_t *spudgpu_swap_chain;
/** Opaque handle to a shader module. */
typedef struct spudgpu_shader_module_t *spudgpu_shader_module;
/** Opaque handle to a compute pipeline. */
typedef struct spudgpu_compute_pipeline_t *spudgpu_compute_pipeline;
/** Opaque handle to a window surface a swap chain presents to. */
typedef struct spudgpu_surface_t *spudgpu_surface;
/** Opaque handle to a semaphore, for GPU-to-GPU synchronization. */
typedef struct spudgpu_semaphore_t *spudgpu_semaphore;
/** Opaque handle to a fence, for CPU-to-GPU synchronization. */
typedef struct spudgpu_fence_t *spudgpu_fence;
/** Opaque handle to a set of GPU timestamps, written as rendering passes start and end. */
typedef struct spudgpu_timestamp_set_t *spudgpu_timestamp_set;
/** Opaque handle to a descriptor set layout. */
typedef struct spudgpu_descriptor_set_layout_t *spudgpu_descriptor_set_layout;
/** Opaque handle to a descriptor pool. */
typedef struct spudgpu_descriptor_pool_t *spudgpu_descriptor_pool;
/** Opaque handle to a descriptor set. */
typedef struct spudgpu_descriptor_set_t *spudgpu_descriptor_set;
/** Opaque handle to a pipeline layout. */
typedef struct spudgpu_pipeline_layout_t *spudgpu_pipeline_layout;
/** Opaque handle to an acceleration structure: a bottom level (triangles or
 * AABBs) or a top level (instances of bottom levels). Declared on every
 * backend, so a struct can name it; its calls exist only with
 * SPUDGPU_EXT_RAY_QUERY. */
typedef struct spudgpu_acceleration_structure_t *spudgpu_acceleration_structure;

/**
 * @brief The graphics API a build of SpudGPU runs on.
 *
 * A build compiles in exactly one backend; spudgpu_get_native_gpu_api()
 * reports which.
 */
typedef uint32_t SPUDGPU_NATIVE_API;

/**
 * @name Supported Graphics APIs
 * @anchor SPUDGPU_API_Constants
 *
 * The graphics APIs SpudGPU has a backend for.
 */
/** @{ */
enum {
	/**
	 * Placeholder representing an uninitialized, unsupported, or invalid
	 * graphics API.
	 */
	SPUDGPU_NATIVE_API_NONE = 0,

	/** Vulkan. */
	SPUDGPU_NATIVE_API_VULKAN = 1,

	/** Direct3D 12. */
	SPUDGPU_NATIVE_API_D3D12 = 2,

	/** Metal. */
	SPUDGPU_NATIVE_API_METAL = 3
};

/** @} */

/**
 * @brief What the caller decides when creating a SpudGPU instance. No field
 * has a default: a zeroed desc is refused, for its `base`, and one with only
 * its `base` set is refused for its `allocator`.
 */
typedef struct spudgpu_instance_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_INSTANCE_DESC; `next` is NULL (nothing
	 * is chained onto this struct yet). Set by the caller. See
	 * SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** Where the memory of the instance, and of every object made from it,
	 * comes from (SPUD_ALLOCATOR). Copied, so the struct need not outlive
	 * the call; its functions and `user_data` must outlive the instance and
	 * every object made from it. */
	const SPUD_ALLOCATOR *allocator;
	/** Null-terminated name of the application. */
	const char *application_name;
	/** Packed 32-bit application version. */
	uint32_t application_version;
	/** Null-terminated name of the engine. */
	const char *engine_name;
	/** Packed 32-bit engine version. */
	uint32_t engine_version;
} spudgpu_instance_desc;

/**
 * @brief Creates a SpudGPU instance on the graphics API this build was
 * compiled for.
 *
 * Call this before any other SpudGPU function. The instance lists the
 * graphics API's adapters as it is created (spudgpu_enumerate_adapters()); the
 * caller then creates a device on the one it chooses
 * (spudgpu_create_device()).
 *
 * @param[in]  desc         What the caller decides.
 * @param[out] out_instance Receives the new instance on success; NULL on
 *                          failure.
 *
 * @retval SPUD_SUCCESS The instance was created.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_INSTANCE_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_instance` is NULL.
 * @retval SPUDRESULT_NULL_ALLOCATOR `desc->allocator` is NULL.
 * @retval SPUDRESULT_INVALID_ALLOCATOR `desc->allocator` lacks a function.
 * @retval SPUDRESULT_GPU_DEVICE_ENUMERATION_FAILURE The graphics API could
 *         not list its GPUs. No GPU is not a failure: it is an instance with
 *         no adapters.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_instance()
 */
SPUDRESULT spudgpu_create_instance(
    const spudgpu_instance_desc *desc,
    spudgpu_instance *out_instance);

/**
 * @brief Destroys a SpudGPU instance.
 *
 * @warning Every device created on one of its adapters must be destroyed
 * first (spudgpu_destroy_device()), and so must every surface created from
 * it.
 *
 * @param[in] instance Instance to destroy.
 *
 * @return SPUD_SUCCESS, including when `instance` is NULL.
 */
SPUDRESULT spudgpu_destroy_instance(spudgpu_instance instance);

/**
 * @name GPU vendor IDs
 * PCI vendor IDs, for comparing against SPUDGPU_ADAPTER_PROPERTIES::vendor_id.
 */
/** @{ */
#define SPUDGPU_VENDOR_INTEL 0x8086 /**< Intel. */
#define SPUDGPU_VENDOR_AMD_ATI 0x1002 /**< AMD (formerly ATI). */
#define SPUDGPU_VENDOR_NVIDIA 0X10DE /**< NVIDIA. */
#define SPUDGPU_VENDOR_ARM 0x13B5 /**< Arm. */
#define SPUDGPU_VENDOR_QUALCOMM 0x5143 /**< Qualcomm. */
#define SPUDGPU_VENDOR_IMGTEC 0x1010 /**< Imagination Technologies. */
#define SPUDGPU_VENDOR_APPLE 0x106B /**< Apple. */
#define SPUDGPU_VENDOR_VMWARE 0x15AD /**< VMware. */
/** @} */

/**
 * @brief Returns the graphics API an instance runs on.
 *
 * @param[in] instance Instance to query.
 *
 * @return The SPUDGPU_NATIVE_API of the compiled-in backend, or
 *         SPUDGPU_NATIVE_API_NONE if `instance` is NULL.
 */
SPUDGPU_NATIVE_API spudgpu_get_native_gpu_api(spudgpu_instance instance);

// ============================================================================
//  Adapters and devices
// ============================================================================

/**
 * @brief What kind of GPU an adapter is, as the graphics API reports it.
 */
typedef uint32_t SPUDGPU_ADAPTER_TYPE;
enum {
	/** The API reports no kind, or one not listed here. */
	SPUDGPU_ADAPTER_TYPE_OTHER = 0,
	/** A GPU sharing the system's memory and package (Vulkan
	 * `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU`; D3D12
	 * `D3D12_FEATURE_DATA_ARCHITECTURE1::UMA`; Metal `hasUnifiedMemory`). */
	SPUDGPU_ADAPTER_TYPE_INTEGRATED = 1,
	/** A GPU with memory of its own (Vulkan
	 * `VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU`; D3D12 not UMA; Metal not
	 * `hasUnifiedMemory`). */
	SPUDGPU_ADAPTER_TYPE_DISCRETE = 2,
	/** A GPU a hypervisor presents (Vulkan
	 * `VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU`). */
	SPUDGPU_ADAPTER_TYPE_VIRTUAL = 3,
	/** A renderer that runs on the CPU (Vulkan
	 * `VK_PHYSICAL_DEVICE_TYPE_CPU`, such as lavapipe or SwiftShader; D3D12
	 * `DXGI_ADAPTER_FLAG3_SOFTWARE`, WARP). Listed like any other: whether to
	 * use one is the caller's. */
	SPUDGPU_ADAPTER_TYPE_SOFTWARE = 4
};

/**
 * @brief Capabilities an adapter may or may not have, which a device has only
 * when it was created with them.
 *
 * A bit set in SPUDGPU_ADAPTER_PROPERTIES::supported_features says the
 * adapter has it. A device has exactly the features in its
 * spudgpu_device_desc::required_features, on every backend: using one the
 * device was not created with is refused with that feature's result, as if
 * the adapter lacked it, even where the graphics API underneath would allow
 * it. Vulkan switches most of these on when its device is created; D3D12 and
 * Metal have nothing to switch on, and hold the device to the same rule. So a
 * feature used without being asked for fails on the machine the code was
 * written on, not only on a GPU that lacks it.
 *
 * A capability that some call refuses without it is a feature. A fact that
 * makes no call refuse (SPUDGPU_ADAPTER_PROPERTIES::unified_memory) is a
 * property, which the caller reads and nothing switches on.
 *
 * The bits are always defined, whatever the build. A feature whose
 * SPUDGPU_EXT_* macro is 0 in this build is never in any adapter's
 * supported features, so code that asks for it compiles on every backend and
 * asks for nothing there. New features are new bits; a bit's meaning never
 * changes. They are macros, not an enum: before C23 an enumerator can't go
 * past an int, and the set may grow past 32.
 */
typedef uint64_t SPUDGPU_FEATURES;

/** spudgpu_rendering_begin_desc::layer_count above 0, with the vertex shader
 * choosing each primitive's layer, in SPIR-V of any version (before 1.5 a
 * vertex shader's `gl_Layer` is the `ShaderViewportIndexLayerEXT`
 * capability, from 1.5 `ShaderLayer`). Vulkan:
 * `VkPhysicalDeviceVulkan12Features::shaderOutputLayer` and
 * `::shaderOutputViewportIndex`, and `VK_EXT_shader_viewport_index_layer`,
 * all enabled with it. D3D12:
 * `D3D12_FEATURE_DATA_D3D12_OPTIONS::VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation`.
 * Metal: `MTLGPUFamilyApple5` or `MTLGPUFamilyMac2`. */
#define SPUDGPU_FEATURE_LAYERED_RENDERING (UINT64_C(1) << 0)
/** spudgpu_shader_pipeline_desc::depth_bias_clamp other than 0; refused with
 * SPUDRESULT_GPU_DEPTH_BIAS_CLAMP_NOT_SUPPORTED. Vulkan:
 * `VkPhysicalDeviceFeatures::depthBiasClamp`. D3D12 and Metal: every
 * adapter. */
#define SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP (UINT64_C(1) << 1)
/** Color attachments of one pipeline that blend differently from each other;
 * refused with SPUDRESULT_GPU_INDEPENDENT_BLEND_NOT_SUPPORTED. Vulkan:
 * `VkPhysicalDeviceFeatures::independentBlend`. D3D12 and Metal: every
 * adapter. */
#define SPUDGPU_FEATURE_INDEPENDENT_BLEND (UINT64_C(1) << 2)
/** An indirect draw of more than one entry per call, up to
 * SPUDGPU_ADAPTER_PROPERTIES::max_draw_indirect_count. Without it a call takes
 * 1 entry at most. Vulkan: `VkPhysicalDeviceFeatures::multiDrawIndirect`.
 * D3D12 and Metal: every adapter. */
#define SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT (UINT64_C(1) << 3)
/** SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY views, and shaders that declare a cube
 * array; such a view is refused with SPUDRESULT_GPU_INVALID_IMAGE_VIEW_TYPE.
 * Vulkan: `VkPhysicalDeviceFeatures::imageCubeArray`. D3D12 and Metal: every
 * adapter. */
#define SPUDGPU_FEATURE_CUBE_ARRAY_VIEWS (UINT64_C(1) << 4)
/** spudgpu_create_timestamp_set(); refused with
 * SPUDRESULT_GPU_TIMESTAMPS_NOT_SUPPORTED. Vulkan: the direct queues'
 * family has `timestampValidBits` above 0. D3D12: every adapter. Metal: the
 * device has the timestamp counter set and
 * `supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary`. */
#define SPUDGPU_FEATURE_PASS_TIMESTAMPS (UINT64_C(1) << 5)
/** Buffers of SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE; refused with
 * SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED. Only ever on an adapter with
 * SPUDGPU_ADAPTER_PROPERTIES::unified_memory, and not implied by it. Vulkan: a
 * memory type that is both `DEVICE_LOCAL` and `HOST_VISIBLE`. D3D12 and Metal:
 * every unified-memory adapter. */
#define SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY (UINT64_C(1) << 6)
/** Heaps (spudgpu_create_heap()) of SPUDGPU_MEMORY_KIND_UPLOAD, _READBACK or
 * _DEVICE_MAPPABLE (the last also needs SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY);
 * refused with SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED, and buffers of those
 * kinds are then created with spudgpu_create_buffer(). Vulkan and D3D12:
 * every adapter. Metal: an adapter in an `MTLGPUFamilyApple` family. */
#define SPUDGPU_FEATURE_MAPPABLE_HEAPS (UINT64_C(1) << 7)
/** The device's bindless table and every bindless call (SPUDGPU_EXT_BINDLESS);
 * refused with SPUDRESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED. The table is made
 * with the device, only when this is asked for, and its size comes out of the
 * device's descriptor budget. Vulkan: the descriptor indexing features the
 * table needs, which are switched on only with this. D3D12: resource binding
 * tier 3. Metal: never (SPUDGPU_EXT_BINDLESS is 0). */
#define SPUDGPU_FEATURE_BINDLESS (UINT64_C(1) << 8)
/** Mesh-shader pipelines and spudgpu_cmd_dispatch_mesh()
 * (SPUDGPU_EXT_MESH_SHADING); a pipeline, and a mesh or task shader module,
 * is refused with SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED (on Vulkan the
 * module's SPIR-V is itself outside the rules without the extension).
 * Vulkan: `VK_EXT_mesh_shader` with `meshShader`. D3D12:
 * `D3D12_FEATURE_DATA_D3D12_OPTIONS7::MeshShaderTier`.
 * Metal: every adapter. */
#define SPUDGPU_FEATURE_MESH_SHADING (UINT64_C(1) << 9)
/** The depth bounds test (SPUDGPU_EXT_DEPTH_BOUNDS_TEST). Vulkan:
 * `VkPhysicalDeviceFeatures::depthBounds`. D3D12:
 * `D3D12_FEATURE_DATA_D3D12_OPTIONS2::DepthBoundsTestSupported`. Metal:
 * `MTLGPUFamilyApple10`. */
#define SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST (UINT64_C(1) << 10)
/** spudgpu_create_swap_chain() and spudgpu_get_surface_present_scalings(),
 * each refused with SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED, and
 * spudgpu_adapter_supports_surface() answering true. Vulkan:
 * `VK_KHR_swapchain`, and `VK_KHR_swapchain_maintenance1` (or its EXT
 * original) too where the adapter has it, for the present scalings it
 * offers. D3D12 and Metal: every adapter. A device that never presents
 * (tests, an offline render) need not ask, and a GPU with no display is
 * still listed for compute. */
#define SPUDGPU_FEATURE_PRESENT (UINT64_C(1) << 11)
/** An indirect draw whose arguments' first_instance is other than 0
 * (spudgpu_draw_indirect_args, spudgpu_draw_indexed_indirect_args). Without
 * it every first_instance an indirect draw reads must be 0; what a draw with
 * another value does is undefined, on every backend, since the arguments are
 * the GPU's to read and no call can refuse them. Vulkan:
 * `VkPhysicalDeviceFeatures::drawIndirectFirstInstance`. D3D12 and Metal:
 * every adapter. */
#define SPUDGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE (UINT64_C(1) << 12)
/** The adapter can build acceleration structures and run ray queries in
 * compute and fragment shaders (SPUDGPU_EXT_RAY_QUERY); every call of the
 * extension on a device created without it is refused with
 * SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED. Vulkan:
 * `VK_KHR_acceleration_structure` and `VK_KHR_ray_query` (with
 * `VK_KHR_deferred_host_operations`, which the first requires and SpudGPU
 * never uses) and their `accelerationStructure` and `rayQuery` features, all
 * enabled with it. D3D12: `D3D12_FEATURE_DATA_D3D12_OPTIONS5::RaytracingTier`
 * 1.1 or later. Metal: `supportsRaytracing` and `supportsRaytracingFromRender`
 * (macOS 14, iOS 17 or later; never set on an earlier system) on an
 * `MTLGPUFamilyApple7` or later GPU.
 *
 * A shader module whose code makes ray queries, used on a device created
 * without it, is the caller's error: no backend reads a module's code for
 * it, and what the pipeline does is undefined. */
#define SPUDGPU_FEATURE_RAY_QUERY (UINT64_C(1) << 13)
/** The adapter runs geometry shaders (SPUDGPU_EXT_GEOMETRY_SHADER). Vulkan:
 * the geometryShader feature. D3D12: every adapter. Metal: never (the EXT
 * is 0). A geometry module, or a pipeline naming one, on a device created
 * without it is refused with
 * SPUDRESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED. */
#define SPUDGPU_FEATURE_GEOMETRY_SHADER (UINT64_C(1) << 14)
/** The adapter runs tessellation control and evaluation shaders
 * (SPUDGPU_EXT_TESSELLATION). Vulkan: the tessellationShader feature. D3D12:
 * every adapter. Metal: never (the EXT is 0). A tessellation module, or a
 * pipeline naming one, on a device created without it is refused with
 * SPUDRESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED. */
#define SPUDGPU_FEATURE_TESSELLATION (UINT64_C(1) << 15)

/** @brief The kinds of queue, as bits, for SPUDGPU_QUEUE_POOL. */
typedef uint32_t SPUDGPU_QUEUE_KINDS;
/** Queues of SPUDGPU_COMMAND_LIST_TYPE_DIRECT. */
#define SPUDGPU_QUEUE_KIND_DIRECT (1u << 0)
/** Queues of SPUDGPU_COMMAND_LIST_TYPE_COMPUTE. */
#define SPUDGPU_QUEUE_KIND_COMPUTE (1u << 1)
/** Queues of SPUDGPU_COMMAND_LIST_TYPE_COPY. */
#define SPUDGPU_QUEUE_KIND_COPY (1u << 2)

/**
 * @brief What the graphics API says about where a pool's queues run.
 *
 * No value promises that work on the pool runs at the same time as graphics
 * work: no API promises that.
 */
typedef uint32_t SPUDGPU_QUEUE_POOL_ENGINE;
enum {
	/** The API doesn't say. D3D12 and Metal: every pool. */
	SPUDGPU_QUEUE_POOL_ENGINE_UNKNOWN = 0,
	/** The family graphics work runs on (Vulkan: a family with
	 * `VK_QUEUE_GRAPHICS_BIT`). Always the pool with DIRECT in it. */
	SPUDGPU_QUEUE_POOL_ENGINE_GRAPHICS = 1,
	/** A family the driver presents as separate from graphics (Vulkan: a
	 * family without `VK_QUEUE_GRAPHICS_BIT`). */
	SPUDGPU_QUEUE_POOL_ENGINE_SEPARATE = 2
};

/**
 * @brief One pool of queues, as SPUDGPU_ADAPTER_PROPERTIES lists them.
 */
/* Frozen: an array element, held by value in SPUDGPU_ADAPTER_PROPERTIES. It never changes; a new need gets a new type. */
typedef struct SPUDGPU_QUEUE_POOL {
	/** The kinds made from this pool. Every kind is in exactly one pool. */
	SPUDGPU_QUEUE_KINDS kinds;
	/** The most queues of those kinds, together, one device may have: the
	 * desc's counts for them add up to at most this. Vulkan: the family's
	 * `queueCount`. D3D12 and Metal: UINT32_MAX. */
	uint32_t max_queues;
	/** What the API says about where this pool's queues run. */
	SPUDGPU_QUEUE_POOL_ENGINE engine;
} SPUDGPU_QUEUE_POOL;

/**
 * @brief The encoding of a shader module's code.
 *
 * Each backend takes exactly one: SPIR-V on Vulkan, DXIL on D3D12, a
 * .metallib on Metal, reported per adapter in
 * SPUDGPU_ADAPTER_PROPERTIES::shader_formats. SpudGPU compiles and
 * translates none of them.
 */
typedef uint32_t SPUDGPU_SHADER_FORMAT;
enum {
	/** Not a format: a zeroed desc has not chosen, and is refused. */
	SPUDGPU_SHADER_FORMAT_NONE = 0,
	/** SPIR-V words (Vulkan). 1.0 to the device's Vulkan version's
	 * highest; a module using an extension or capability needs the device
	 * feature that enables it (a mesh shader, SPUDGPU_FEATURE_MESH_SHADING). */
	SPUDGPU_SHADER_FORMAT_SPIRV = 1,
	/** A DXIL container as DXC writes it, validated and signed (D3D12).
	 * Its shader model must be one the device runs. */
	SPUDGPU_SHADER_FORMAT_DXIL = 2,
	/** A .metallib: a Metal library as Apple's metallib tool writes it
	 * (Metal). Its Metal language version must be one the device runs. */
	SPUDGPU_SHADER_FORMAT_METALLIB = 3
};

/** @brief A set of SPUDGPU_SHADER_FORMAT values: bit (1 << format). */
typedef uint32_t SPUDGPU_SHADER_FORMATS;

/**
 * @brief What an adapter is and what it can do, as the graphics API reports
 * it.
 *
 * Facts only: nothing here is chosen. Limits that only matter with a feature
 * are in structs chained onto `base.next` (SPUDGPU_MESH_SHADING_LIMITS,
 * SPUDGPU_BINDLESS_LIMITS), each declared only where its SPUDGPU_EXT_* macro
 * is 1, and all 0 when the adapter lacks the feature. A limit counts only on
 * a device created with its feature.
 */
typedef struct SPUDGPU_ADAPTER_PROPERTIES {
	/** `type` is SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES; `next` may chain
	 * SPUDGPU_MESH_SHADING_LIMITS, SPUDGPU_BINDLESS_LIMITS and
	 * SPUDGPU_RAY_QUERY_LIMITS, each at most once, in any order. The caller builds the chain and sets each type;
	 * SpudGPU fills in the rest. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE_OUT base;
	/** The adapter's name, null-terminated. */
	char description[128];
	/** PCI vendor ID, e.g. SPUDGPU_VENDOR_NVIDIA. Metal: SPUDGPU_VENDOR_APPLE
	 * (an Apple GPU is the only kind it lists on the platforms SpudGPU
	 * builds for). */
	uint32_t vendor_id;
	/** PCI device ID. 0 on Metal, which does not report one. */
	uint32_t device_id;
	/** PCI subsystem ID. D3D12 only; 0 on Vulkan and Metal. */
	uint32_t subsystem_id;
	/** PCI revision. D3D12 only; 0 on Vulkan and Metal. */
	uint32_t revision;
	/** Names this adapter's GPU: two adapters with equal ids, from any
	 * instances in any processes, are the same GPU. The reverse does not
	 * hold: a GPU may get a new id when the system restarts, when its driver
	 * is restarted (updated, reinstalled, or reset by the OS), or when it is
	 * plugged in again. Never store one; a choice saved across runs is
	 * matched by the hardware IDs and #description. Opaque: compare all 16
	 * bytes. Vulkan: `VkPhysicalDeviceIDProperties::deviceUUID`. D3D12:
	 * `DXGI_ADAPTER_DESC3::AdapterLuid`, then zeros. Metal: `registryID`,
	 * then zeros. */
	uint8_t adapter_id[16];
	/** The adapter's Windows LUID, for interop that names an adapter by one
	 * (DXGI, OpenXR's D3D12 binding). Valid only when #luid_valid is true.
	 * D3D12: `AdapterLuid`. Vulkan: `deviceLUID`. Metal: never valid. */
	uint8_t luid[8];
	/** D3D12: always true. Vulkan: `deviceLUIDValid`. Metal: false. */
	bool luid_valid;
	/** What kind of GPU it is. */
	SPUDGPU_ADAPTER_TYPE adapter_type;
	/** Every SPUDGPU_FEATURE_* the adapter has. */
	SPUDGPU_FEATURES supported_features;
	/** The shader formats a device of this adapter takes, as
	 * (1u << SPUDGPU_SHADER_FORMAT_*): exactly one bit, the backend's own
	 * (SPIR-V on Vulkan, DXIL on D3D12, a .metallib on Metal). */
	SPUDGPU_SHADER_FORMATS shader_formats;

	/**
	 * Bytes of memory that are the GPU's own.
	 *
	 * D3D12: `DXGI_ADAPTER_DESC3::DedicatedVideoMemory`. Vulkan: the sum of
	 * the heaps flagged `VK_MEMORY_HEAP_DEVICE_LOCAL_BIT`, which is nonzero
	 * on an integrated GPU too. Metal: `recommendedMaxWorkingSetSize` on a
	 * GPU without unified memory, 0 on one with it.
	 */
	uint64_t dedicated_video_memory;
	/**
	 * Bytes of system memory reserved for the GPU.
	 *
	 * D3D12: `DXGI_ADAPTER_DESC3::DedicatedSystemMemory`. 0 on Vulkan and
	 * Metal, which do not report one.
	 */
	uint64_t dedicated_system_memory;
	/**
	 * Bytes of system memory the GPU may share with the CPU.
	 *
	 * D3D12: `DXGI_ADAPTER_DESC3::SharedSystemMemory`. Vulkan: the sum of the
	 * heaps not flagged `VK_MEMORY_HEAP_DEVICE_LOCAL_BIT`. Metal:
	 * `recommendedMaxWorkingSetSize` on a GPU with unified memory, 0 on one
	 * without.
	 */
	uint64_t shared_system_memory;
	/**
	 * Whether the GPU has no memory of its own and uses the system's, as the
	 * graphics API reports it. A fact about this adapter, not about the
	 * platform or the backend: one machine can have one of each. A property,
	 * not a feature: nothing is switched on by it, and it only says which
	 * path is faster.
	 *
	 * Metal: `MTLDevice.hasUnifiedMemory`. D3D12:
	 * `D3D12_FEATURE_DATA_ARCHITECTURE1::UMA`. Vulkan has no such query:
	 * true when the GPU's type is `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU`.
	 */
	bool unified_memory;

	/**
	 * What the offset of a uniform buffer range in a descriptor write
	 * (spudgpu_descriptor_buffer_info::offset, for
	 * SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER) is a multiple of. Any other
	 * offset is outside the graphics API's rules. A power of two.
	 *
	 * Vulkan: `VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment`.
	 * D3D12: 256 (`D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT`). Metal has
	 * no query: 256, the largest any Metal GPU family asks of a constant
	 * buffer's offset.
	 */
	uint32_t uniform_buffer_offset_alignment;
	/**
	 * The same for a storage buffer range
	 * (SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER). A power of two.
	 *
	 * Vulkan: `VkPhysicalDeviceLimits::minStorageBufferOffsetAlignment`.
	 * D3D12: 16 (`D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT`: a storage buffer is a
	 * raw view). Metal has no query: 256, as for a uniform buffer.
	 */
	uint32_t storage_buffer_offset_alignment;
	/**
	 * The largest `draw_count` spudgpu_cmd_draw_indirect() and
	 * spudgpu_cmd_draw_indexed_indirect() take in one call, on a device
	 * created with SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT; 1 on one without,
	 * whatever this says. A call with a larger count records nothing; split
	 * it into calls at advancing offsets.
	 *
	 * Vulkan: `VkPhysicalDeviceLimits::maxDrawIndirectCount`. D3D12 and
	 * Metal: UINT32_MAX.
	 */
	uint32_t max_draw_indirect_count;
	/**
	 * The most a device's spudgpu_device_desc::max_descriptors may be.
	 *
	 * D3D12: 1,000,000, the shader-visible CBV/SRV/UAV heap's limit below
	 * resource binding tier 3, kept on tier 3 too. Vulkan and Metal:
	 * UINT32_MAX (neither has a device-wide limit).
	 */
	uint32_t max_descriptors;
	/**
	 * The most a device's spudgpu_device_desc::max_samplers may be.
	 *
	 * D3D12: 2048 (`D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE`). Vulkan and
	 * Metal: UINT32_MAX.
	 */
	uint32_t max_samplers;

	/** The pools a device's queues come from, #queue_pool_count of them (1
	 * to 3). Vulkan: one per family the rule in spudgpu_device_desc picks.
	 * D3D12 and Metal: three, one kind each. */
	SPUDGPU_QUEUE_POOL queue_pools[3];
	/** The number of entries of #queue_pools in use. */
	uint32_t queue_pool_count;
} SPUDGPU_ADAPTER_PROPERTIES;

/**
 * @brief The device's ray query limits, chained onto
 * SPUDGPU_ADAPTER_PROPERTIES. All 0 on an adapter without
 * SPUDGPU_FEATURE_RAY_QUERY.
 *
 * Declared on every backend, as the feature bit is, so that a chain naming
 * it compiles everywhere. Each count is a limit of one build; every
 * alignment is a power of two.
 */
typedef struct SPUDGPU_RAY_QUERY_LIMITS {
	/** `type` is SPUD_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS; `next` is the rest of
	 * the adapter properties' chain. The caller sets the type; SpudGPU fills
	 * in the rest. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE_OUT base;
	/** Most geometries in one bottom level
	 * (spudgpu_acceleration_structure_inputs::geometry_count). Vulkan
	 * `maxGeometryCount`; D3D12 2^24; Metal 2^24 - 1 (the narrower of
	 * Apple's documented limits). */
	uint64_t max_geometries;
	/** Most instances in one top level
	 * (spudgpu_acceleration_structure_inputs::instance_count). Vulkan
	 * `maxInstanceCount`; D3D12 2^24; Metal 2^24 - 1. */
	uint64_t max_instances;
	/** Most triangles and AABBs in one bottom level, every geometry's
	 * summed. Vulkan `maxPrimitiveCount`; D3D12 2^29; Metal 2^28 - 1. */
	uint64_t max_primitives;
	/** What a build's scratch address (the scratch buffer's GPU address plus
	 * spudgpu_acceleration_structure_build::scratch_offset) is a multiple of.
	 * Vulkan `minAccelerationStructureScratchOffsetAlignment`; D3D12 256;
	 * Metal 256. */
	uint32_t scratch_offset_alignment;
	/** What a placed structure's offset in its heap is a multiple of
	 * (spudgpu_create_placed_acceleration_structure()), and what its size is
	 * rounded up to for the range it covers there. Vulkan 256; D3D12 65536
	 * (`D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT`: a placed buffer's); Metal
	 * the alignment `heapAccelerationStructureSizeAndAlignWithSize:` gives
	 * the smallest structure (1024 on an Apple M5 Pro). Where a graphics API asks more of one structure
	 * than this rule, the create refuses it rather than cover more than the
	 * rounded range (spudgpu_create_placed_acceleration_structure()). */
	uint32_t placement_alignment;
} SPUDGPU_RAY_QUERY_LIMITS;

/**
 * @brief Lists the adapters an instance can create devices on.
 *
 * The list is read when the instance is created and never changes. It is
 * in the order the graphics API gives
 * (Vulkan's loader, DXGI's `EnumAdapters1`, `MTLCopyAllDevices`): SpudGPU
 * neither sorts nor filters it by preference. An adapter is listed only if a
 * device can be created on it (Vulkan: the physical device's `apiVersion`
 * is 1.3 or later; D3D12: a device of feature level 12_0 can be created on
 * it; Metal: the GPU is in `MTLGPUFamilyMetal3`). Software adapters are
 * listed, with SPUDGPU_ADAPTER_TYPE_SOFTWARE. No adapter is a count of 0, not
 * a failure.
 *
 * A GPU added after the instance was created is not listed, and one removed
 * stays listed with no device creatable on it; a new instance lists the
 * system as it is then. Listing keeps nothing on an adapter. D3D12 answers an
 * adapter's features only through a device, so on D3D12
 * spudgpu_create_instance() creates a device on each adapter in turn and
 * releases it before the next: a discrete GPU may wake briefly then.
 *
 * The adapters and the array belong to the instance and are released by
 * spudgpu_destroy_instance(); the caller frees neither.
 *
 * Thread: may overlap any call on the same instance, and on its adapters,
 * except spudgpu_destroy_instance(). It only reads the list.
 *
 * @param[in]  instance           Instance to list adapters of.
 * @param[out] out_adapters       Receives a pointer to the instance's array,
 *                                or NULL when there are none.
 * @param[out] out_adapters_count Receives the number of adapters in it.
 *
 * @retval SPUD_SUCCESS Both outputs were written.
 * @retval SPUDRESULT_GPU_INVALID_INSTANCE `instance` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_adapters` or
 *         `out_adapters_count` is NULL.
 */
SPUDRESULT spudgpu_enumerate_adapters(
    spudgpu_instance instance,
    const spudgpu_adapter **out_adapters,
    uint32_t *out_adapters_count);

/**
 * @brief Reads what an adapter is and can do.
 *
 * Creates nothing on the adapter.
 *
 * Thread: may overlap any call on the same adapter or its instance,
 * spudgpu_create_device() on it included, except
 * spudgpu_destroy_instance(). The properties were read when the instance
 * was created.
 *
 * @param[in]  adapter        Adapter to query.
 * @param[out] out_properties Receives its properties, and those of every
 *                            struct chained onto it.
 *
 * @retval SPUD_SUCCESS `out_properties` and its chain were filled in.
 * @retval SPUDRESULT_GPU_INVALID_ADAPTER `adapter` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_properties` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `out_properties->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN A struct in the chain is not one
 *         of the limits structs this build declares.
 * @retval SPUDRESULT_STRUCT_DUPLICATE_IN_CHAIN One limits struct is in the
 *         chain twice.
 */
SPUDRESULT spudgpu_get_adapter_properties(
    spudgpu_adapter adapter,
    SPUDGPU_ADAPTER_PROPERTIES *out_properties);

/**
 * @brief Reports whether a device on an adapter can present to a surface.
 *
 * Vulkan: `vkGetPhysicalDeviceSurfaceSupportKHR` for the family direct
 * queues are made from. D3D12 and Metal: true. False on every backend for an
 * adapter without SPUDGPU_FEATURE_PRESENT.
 *
 * @param[in]  adapter       Adapter to ask about.
 * @param[in]  surface       Surface to present to.
 * @param[out] out_supported Receives the answer. False is an answer, not a
 *                           failure.
 *
 * @retval SPUD_SUCCESS `out_supported` was written.
 * @retval SPUDRESULT_GPU_INVALID_ADAPTER `adapter` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_SURFACE `surface` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_supported` is NULL.
 * @return SPUDRESULT_API_SPECIFIC_FAILURE if the graphics API fails.
 */
SPUDRESULT spudgpu_adapter_supports_surface(
    spudgpu_adapter adapter,
    spudgpu_surface surface,
    bool *out_supported);

/**
 * @brief What the caller decides when creating a device. No field has a
 * default: a zeroed desc asks for no features and no queues, which is
 * refused.
 */
typedef struct spudgpu_device_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_DEVICE_DESC; `next` is NULL (nothing is
	 * chained onto this struct yet). Set by the caller. See
	 * SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** The features the device has (SPUDGPU_FEATURE_*), each of which the
	 * adapter must support. Anything not asked for here the device lacks. */
	SPUDGPU_FEATURES required_features;
	/**
	 * How many queues of each kind the device has. For each of the adapter's
	 * SPUDGPU_ADAPTER_PROPERTIES::queue_pools, the counts of the kinds it
	 * serves add up to at most its `max_queues`. Their sum is at least 1.
	 * Vulkan makes each kind from one queue family: direct queues from the
	 * first family with graphics; compute queues from the first family with
	 * compute and without graphics, else the direct family; copy queues from
	 * the first family with transfer and neither graphics nor compute, else
	 * the compute family. That rule is what the adapter's pools report.
	 *
	 * SpudGPU sets no queue priority: Vulkan gets the same value for every
	 * queue, D3D12 `D3D12_COMMAND_QUEUE_PRIORITY_NORMAL`. The order work on
	 * different queues runs in is the driver's. No kind promises hardware of
	 * its own: see SPUDGPU_QUEUE_POOL_ENGINE.
	 */
	uint32_t direct_queue_count;
	/** As #direct_queue_count, for SPUDGPU_COMMAND_LIST_TYPE_COMPUTE. */
	uint32_t compute_queue_count;
	/** As #direct_queue_count, for SPUDGPU_COMMAND_LIST_TYPE_COPY. */
	uint32_t copy_queue_count;
	/**
	 * The most descriptors other than samplers (sampled and storage images,
	 * uniform and storage buffers, texel buffer views) the device's
	 * descriptor pools and bindless table hold at once, together. At most the
	 * adapter's SPUDGPU_ADAPTER_PROPERTIES::max_descriptors. A pool's share
	 * is the sum of its spudgpu_descriptor_pool_desc::pool_sizes counts, a
	 * SAMPLER count going to #max_samplers instead and a
	 * COMBINED_IMAGE_SAMPLER count to both. A pool that would go past it is
	 * refused with SPUDRESULT_GPU_OUT_OF_DESCRIPTORS, on every backend; what a
	 * pool's destroy gives back may be fragmented, so a pool can be refused
	 * even when the count left would hold it. 0 is a device with no
	 * descriptors: every pool is refused. With SPUDGPU_FEATURE_BINDLESS, the
	 * table's size (the three counts of SPUDGPU_BINDLESS_LIMITS) comes out of
	 * it when the device is created.
	 *
	 * D3D12: the size of the shader-visible CBV/SRV/UAV heap, made with the
	 * device. Vulkan and Metal: a count the device keeps.
	 */
	uint32_t max_descriptors;
	/** As #max_descriptors, for samplers, against
	 * SPUDGPU_ADAPTER_PROPERTIES::max_samplers. D3D12: the size of the
	 * shader-visible sampler heap. */
	uint32_t max_samplers;
	/** Name for debuggers and validation messages, or NULL. Copied, in every
	 * build; see spud_debug_name_set(). */
	const char *debug_name;
} spudgpu_device_desc;

/**
 * @brief Creates a device on an adapter, with the features, queues and
 * descriptor budget the caller asks for.
 *
 * One device per adapter at a time, on every backend: D3D12 hands back the
 * same device to a second `D3D12CreateDevice` on one adapter, and Metal has
 * one `MTLDevice` per GPU, so a second device would not be a separate one
 * there.
 *
 * Thread: may overlap any call on the same adapter, this one included,
 * except spudgpu_destroy_instance(). Of two creates on one adapter at once,
 * one gets the device and the other SPUDRESULT_GPU_ADAPTER_IN_USE (the
 * claim is an atomic exchange on the adapter).
 *
 * The rule is one device per GPU in the process, across instances too, and
 * that part is the caller's: two instances list two adapters for one GPU,
 * which SpudGPU does not compare. On D3D12 a device made through each would
 * be one native device under two handles. Compare
 * SPUDGPU_ADAPTER_PROPERTIES::adapter_id.
 *
 * Everything the device needs is made here, not on first use: its queues,
 * its bindless table when SPUDGPU_FEATURE_BINDLESS is asked for, and on D3D12
 * its shader-visible descriptor heaps, sized by the budget.
 *
 * Vulkan: `vkCreateDevice` with the base set SpudGPU relies on
 * (`dynamicRendering`, `synchronization2`, `timelineSemaphore`,
 * `bufferDeviceAddress`, all required by Vulkan 1.3) and the extensions and
 * feature bits `required_features` needs, and nothing else. D3D12:
 * `D3D12CreateDevice` at feature level 12_0. Metal: the queues on the
 * adapter's `MTLDevice`.
 *
 * @param[in]  adapter    Adapter to create the device on.
 * @param[in]  desc       What the caller decides.
 * @param[out] out_device Receives the new device on success; NULL on
 *                        failure.
 *
 * @retval SPUD_SUCCESS The device was created.
 * @retval SPUDRESULT_GPU_INVALID_ADAPTER `adapter` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_DEVICE_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_device` is NULL.
 * @retval SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED A bit of
 *         `desc->required_features` is not in the adapter's
 *         `supported_features`.
 * @retval SPUDRESULT_ZERO_SIZE The three queue counts are all 0.
 * @retval SPUDRESULT_GPU_TOO_MANY_QUEUES The counts of the kinds one of the
 *         adapter's pools serves add up to more than its `max_queues`.
 * @retval SPUDRESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE `desc->max_descriptors`
 *         or `desc->max_samplers` is above the adapter's limit.
 * @retval SPUDRESULT_GPU_OUT_OF_DESCRIPTORS SPUDGPU_FEATURE_BINDLESS is asked
 *         for and the bindless table doesn't fit in the budget.
 * @retval SPUDRESULT_GPU_ADAPTER_IN_USE The adapter already has a device.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_device()
 */
SPUDRESULT spudgpu_create_device(
    spudgpu_adapter adapter,
    const spudgpu_device_desc *desc,
    spudgpu_device *out_device);

/**
 * @brief Destroys a device and its queues, and frees its adapter for
 * another.
 *
 * Also the way back from a lost device (a GPU hang or reset, a driver update,
 * an unplugged GPU):
 *
 * 1. Destroy everything made from the lost device, then the device.
 * 2. Create a device on the same adapter.
 * 3. If that fails, whatever the result, destroy the instance and create a
 *    new one; find the GPU there by SPUDGPU_ADAPTER_PROPERTIES::adapter_id,
 *    else by its hardware IDs, else choose again. A driver update or an
 *    unplugged GPU leaves the old adapter unusable; a hang or reset usually
 *    does not.
 *
 * @warning Every object created from the device must be destroyed first, and
 * every submission to its queues must have finished (or the device be lost).
 * Nothing else may be using the device or its queues: no call on either is
 * in progress. A debug build checks the first: it prints the kinds of
 * object still alive, with their counts, and asserts.
 *
 * @param[in] device Device to destroy. NULL does nothing.
 */
void spudgpu_destroy_device(spudgpu_device device);

/**
 * @brief Returns the adapter a device was created on.
 *
 * @param[in] device Device to query.
 *
 * @return The adapter, or NULL if `device` is NULL.
 */
spudgpu_adapter spudgpu_get_device_adapter(spudgpu_device device);

/**
 * @brief Returns the features a device was created with: its
 * spudgpu_device_desc::required_features.
 *
 * @param[in] device Device to query.
 *
 * @return The features, or 0 if `device` is NULL.
 */
SPUDGPU_FEATURES spudgpu_get_device_features(spudgpu_device device);

/**
 * @brief The kind of work a command list, allocator or queue carries.
 */
typedef uint32_t SPUDGPU_COMMAND_LIST_TYPE;
enum {
	/** Graphics, compute and copy commands. */
	SPUDGPU_COMMAND_LIST_TYPE_DIRECT = 0,
	/** Copy commands only. */
	SPUDGPU_COMMAND_LIST_TYPE_COPY = 1,
	/**
	 * A bundle: draw state recorded once and replayed inside a direct
	 * command list. @see SPUDGPU_EXT_BUNDLES
	 */
	SPUDGPU_COMMAND_LIST_TYPE_BUNDLE = 2,
	/** Compute and copy commands. */
	SPUDGPU_COMMAND_LIST_TYPE_COMPUTE = 3
};

/**
 * @brief Returns one of the queues a device was created with.
 *
 * The device owns the queue; it is not destroyed by the caller. The same kind
 * and index always give the same queue.
 *
 * @param[in]  device    Device to get the queue from.
 * @param[in]  type      Kind of queue: DIRECT, COMPUTE or COPY.
 * @param[in]  index     Which queue of that kind, below the count the device
 *                       was created with.
 * @param[out] out_queue Receives the queue on success.
 *
 * @retval SPUD_SUCCESS `out_queue` was written.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_LIST_TYPE `type` is BUNDLE, or not a
 *         kind of command list.
 * @retval SPUDRESULT_INDEX_OUT_OF_RANGE `index` is not below the count the
 *         device was created with for `type`.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_queue` is NULL.
 */
SPUDRESULT spudgpu_get_command_queue(
    spudgpu_device device,
    SPUDGPU_COMMAND_LIST_TYPE type,
    uint32_t index,
    spudgpu_command_queue *out_queue);

/**
 * @brief Submits closed command lists to a queue for execution.
 *
 * Performs no synchronization with a swap chain; use
 * spudgpu_submit_command_lists_synced() when rendering to one, or
 * spudgpu_queue_submit() to name the semaphores and fence yourself.
 *
 * A queue is an object of its own, used by one caller at a time on every
 * backend (@ref spud_threads "Threads"), and independent of its device and
 * of the other queues. This, spudgpu_submit_command_lists_synced(),
 * spudgpu_queue_submit(), spudgpu_queue_wait_idle() and
 * spudgpu_swap_chain_present() on the swap chain's queue are each a use of
 * the queue, and so is any use of its native queue. Vulkan requires this
 * (its queue is externally synchronized) and SpudGPU takes no lock for it;
 * D3D12 and Metal would allow more, but the rule is the same everywhere so
 * that code written on one runs on all. Two different queues may be used
 * at once. A debug build asserts on every backend when two uses of one
 * queue overlap.
 *
 * @param[in] queue          Queue to submit to.
 * @param[in] cmd_lists      Array of command lists, each closed with
 *                           spudgpu_end_command_list().
 * @param[in] cmd_list_count Number of elements in `cmd_lists`.
 *
 * @retval SPUD_SUCCESS The command lists were submitted.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_QUEUE `queue` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_LIST `cmd_lists` is NULL.
 * @retval SPUDRESULT_ZERO_SIZE `cmd_list_count` is 0.
 * @retval SPUDRESULT_GPU_DEVICE_LOST The device was lost: nothing is
 *         submitted.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 */
SPUDRESULT spudgpu_submit_command_lists(
    spudgpu_command_queue queue,
    spudgpu_command_list *cmd_lists,
    uint32_t cmd_list_count);

/**
 * @brief Submits command lists synchronized with a swap chain.
 *
 * Waits for the swap chain's acquired image and signals what
 * spudgpu_swap_chain_present() and the next
 * spudgpu_swap_chain_acquire_next_image() wait on. Call this instead of
 * spudgpu_submit_command_lists() when rendering to a swap chain. It is
 * spudgpu_queue_submit() with only the command lists and
 * spudgpu_submit_desc::swap_chain set; use that to signal a fence as well.
 *
 * @param[in] queue          Queue to submit to.
 * @param[in] cmd_lists      Array of closed command lists.
 * @param[in] cmd_list_count Number of elements in `cmd_lists`.
 * @param[in] swap_chain     Swap chain to synchronize with.
 *
 * @retval SPUD_SUCCESS The command lists were submitted.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_QUEUE `queue` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_LIST `cmd_lists` is NULL.
 * @retval SPUDRESULT_ZERO_SIZE `cmd_list_count` is 0.
 * @retval SPUDRESULT_GPU_INVALID_SWAP_CHAIN `swap_chain` is NULL.
 * @retval SPUDRESULT_GPU_DEVICE_LOST The device was lost: nothing is
 *         submitted.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 */
SPUDRESULT spudgpu_submit_command_lists_synced(
    spudgpu_command_queue queue,
    spudgpu_command_list *cmd_lists,
    uint32_t cmd_list_count,
    spudgpu_swap_chain swap_chain);

/**
 * @brief What spudgpu_queue_submit() submits and how it is synchronized.
 */
typedef struct spudgpu_submit_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_SUBMIT_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** Array of closed command lists to submit. */
	spudgpu_command_list *cmd_lists;
	/** Number of elements in cmd_lists. */
	uint32_t cmd_list_count;

	/** GPU-to-GPU sync: semaphores to wait on before executing. */
	spudgpu_semaphore *wait_semaphores;
	/** Number of elements in wait_semaphores. */
	uint32_t wait_semaphore_count;
	/**
	 * One SPUDGPU_PIPELINE_STAGE mask per wait semaphore. Required when
	 * wait_semaphore_count > 0.
	 */
	uint32_t *wait_stage_masks;
	/** GPU-to-GPU sync: semaphores to signal when execution completes. */
	spudgpu_semaphore *signal_semaphores;
	/** Number of elements in signal_semaphores. */
	uint32_t signal_semaphore_count;

	/**
	 * CPU-to-GPU sync: fence to signal when execution completes. Optional,
	 * can be NULL.
	 */
	spudgpu_fence signal_fence;

	/**
	 * The value #signal_fence is signaled to once every command list of
	 * this submission has run. Ignored if #signal_fence is NULL. It is the
	 * caller's to choose, and must be greater than every value the fence has
	 * been signaled to or has been asked to be signaled to by a submission
	 * that hasn't finished: a fence's value only goes up.
	 */
	uint64_t signal_fence_value;

	/**
	 * Swap chain the submission renders to. Optional, can be NULL. When set,
	 * the submission is synchronized with it as
	 * spudgpu_submit_command_lists_synced() does - it waits for the acquired
	 * image and signals what spudgpu_swap_chain_present() and the next
	 * spudgpu_swap_chain_acquire_next_image() wait on - in addition to the
	 * semaphores and the fence above.
	 */
	spudgpu_swap_chain swap_chain;
} spudgpu_submit_desc;

/**
 * @brief Submits command lists with caller-chosen synchronization.
 *
 * @param[in] queue Queue to submit to.
 * @param[in] desc  Command lists to submit, the semaphores to wait on and
 *                  signal, the fence to signal and the value to signal it
 *                  to, and the swap chain to synchronize with.
 *
 * @retval SPUD_SUCCESS The command lists were submitted.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_QUEUE `queue` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_SUBMIT_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_LIST `desc->cmd_lists` is NULL.
 * @retval SPUDRESULT_ZERO_SIZE `desc->cmd_list_count` is 0.
 * @retval SPUDRESULT_GPU_DEVICE_LOST The device was lost: nothing is
 *         submitted.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 */
SPUDRESULT spudgpu_queue_submit(
    spudgpu_command_queue queue,
    const spudgpu_submit_desc *desc);

/**
 * @brief Blocks the calling thread until all work submitted to a queue has
 * completed.
 *
 * Equivalent to vkQueueWaitIdle. Prefer fence-based synchronization in hot
 * paths.
 *
 * A use of the queue: nothing else may use the queue until this returns
 * (see spudgpu_submit_command_lists()).
 *
 * @param[in] queue Queue to wait on.
 */
void spudgpu_queue_wait_idle(spudgpu_command_queue queue);

/**
 * @brief Configuration descriptor for a command allocator.
 */
typedef struct spudgpu_command_allocator_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/**
	 * The kind of command list recorded into the allocator.
	 * @see SPUDGPU_COMMAND_LIST_TYPE
	 */
	SPUDGPU_COMMAND_LIST_TYPE type;
	/** Reserved: no backend reads it. Set to 0. */
	uint32_t flags;
	// uint32_t queue_family_index;
} spudgpu_command_allocator_desc;

/**
 * @brief Creates a command allocator on a device.
 *
 * @param[in]  device        Device to create the allocator on.
 * @param[in]  desc          Allocator configuration.
 * @param[out] out_allocator Receives the new allocator on success.
 *
 * @retval SPUD_SUCCESS The allocator was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_allocator` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_LIST_TYPE `desc->type` is not a kind
 *         of command list, or the device was created with no queue of that
 *         kind (spudgpu_device_desc): its lists would have nowhere to be
 *         submitted. A BUNDLE allocator needs a DIRECT queue.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_command_allocator()
 */
SPUDRESULT spudgpu_create_command_allocator(
    spudgpu_device device,
    const spudgpu_command_allocator_desc *desc,
    spudgpu_command_allocator *out_allocator);

/**
 * @brief Destroys a command allocator.
 *
 * @param[in] allocator Allocator to destroy.
 */
void spudgpu_destroy_command_allocator(spudgpu_command_allocator allocator);

/**
 * @brief Resets a command allocator so its memory can be recorded into
 * again.
 *
 * Until an allocator is reset, every recording made into it keeps the
 * memory it took, on a backend that pools command memory in the allocator
 * (D3D12; Vulkan's pool with its buffers reset one at a time). Opening a
 * command list again does not give that memory back. A caller that records
 * every frame resets each allocator once the GPU is done with it, or memory
 * grows for as long as the allocator lives.
 *
 * Two rules hold on every backend:
 *
 * - Reset only when no recording made into the allocator is still
 *   executing on the GPU, and no command list of the allocator is open for
 *   recording.
 * - A recording made into the allocator before the reset must not be
 *   submitted after it. Open the list and record again.
 *
 * @param[in] allocator Allocator to reset.
 *
 * @retval SPUD_SUCCESS The allocator was reset.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_ALLOCATOR `allocator` is NULL.
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE The graphics API failed to reset
 *         it.
 */
SPUDRESULT spudgpu_reset_command_allocator(spudgpu_command_allocator allocator);

/**
 * @brief Creates a command list that records into a command allocator.
 *
 * @param[in]  allocator    Allocator the command list records into.
 * @param[out] out_cmd_list Receives the new command list on success.
 *
 * @retval SPUD_SUCCESS The command list was created.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_ALLOCATOR `allocator` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_cmd_list` is NULL.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_command_list()
 */
SPUDRESULT spudgpu_create_command_list(
    spudgpu_command_allocator allocator,
    spudgpu_command_list *out_cmd_list);

/**
 * @brief Destroys a command list.
 *
 * @param[in] cmd Command list to destroy.
 */
void spudgpu_destroy_command_list(spudgpu_command_list cmd);

/**
 * @brief Opens a command list for recording.
 *
 * Resets the command list's state tracking and puts it in recording mode.
 *
 * What is recorded between this and spudgpu_end_command_list() is submitted
 * once. To run the same commands again, open the list and record them
 * again: a Metal command buffer is committed once, and Vulkan's is begun as
 * a one-time submit.
 *
 * @warning A command list is recorded by one caller at a time: no two calls
 * on it, this one and every `spudgpu_cmd_*` call included, overlap
 * (@ref spud_threads "Threads").
 *
 * @warning Submitting one recording a second time is undefined behavior.
 *
 * @warning Recording a spudgpu_cmd_*() call into a list that is not open
 * (before this call, or after spudgpu_end_command_list()) is undefined
 * behavior: Vulkan and D3D12 both refuse it, and a driver may fault.
 *
 * @warning Do not open a list while its last recording may still be
 * executing on the GPU: Vulkan cannot reset a command buffer that is
 * pending. To record while that one runs, record into another list.
 *
 * @warning At most one command list of an allocator is open at a time: end
 * it before opening another list of the same allocator. D3D12 refuses to
 * open a second list on an allocator another open list is recording into,
 * and the list is left closed, so what is then recorded into it is
 * undefined. To keep two lists open at once, make them from two
 * allocators. A debug build asserts this on every backend, and asserts in
 * spudgpu_reset_command_allocator() that no list of the allocator is open.
 *
 * @param[in] cmd Command list to open.
 */
void spudgpu_begin_command_list(spudgpu_command_list cmd);

/**
 * @brief Closes a command list, making it ready to submit to a queue.
 *
 * @param[in] cmd Command list to close.
 */
void spudgpu_end_command_list(spudgpu_command_list cmd);

/**
 * @brief Bitmask of fence creation flags.
 */
typedef uint32_t SPUDGPU_FENCE_FLAGS;
enum {
	/** No flags. */
	SPUDGPU_FENCE_FLAG_NONE = 0,
	/**
	 * The fence can be shared with another process or graphics API. D3D12:
	 * `D3D12_FENCE_FLAG_SHARED`. Vulkan and Metal have nothing to select at
	 * creation and ignore it.
	 */
	SPUDGPU_FENCE_FLAG_SHARED = 1 << 0
};

/**
 * @brief Creates a fence: a counter the GPU raises as submitted work
 * finishes, and the CPU reads and waits on.
 *
 * A fence holds one 64-bit value that only goes up. A submission raises it
 * to a value the caller chooses (spudgpu_submit_desc::signal_fence_value)
 * once its command lists have run; spudgpu_signal_fence() raises it from the
 * CPU. spudgpu_get_fence_value() reads it and spudgpu_wait_for_fences()
 * blocks until it reaches a value. There is nothing to reset: a fence is
 * reused by signaling it to a higher value.
 *
 * Maps to: a timeline VkSemaphore (Vulkan), ID3D12Fence (D3D12),
 * MTLSharedEvent (Metal).
 *
 * @param[in]  device        Device that owns the fence.
 * @param[in]  flags         Fence creation flags.
 * @param[in]  initial_value Value the fence starts at.
 * @param[out] out_fence     Receives the new fence on success.
 *
 * @retval SPUD_SUCCESS The fence was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_fence` is NULL.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_fence()
 */
SPUDRESULT spudgpu_create_fence(
    spudgpu_device device,
    SPUDGPU_FENCE_FLAGS flags,
    uint64_t initial_value,
    spudgpu_fence *out_fence);

/**
 * @brief Destroys a fence.
 *
 * @warning Every submission that signals the fence must have finished, and
 * no spudgpu_wait_for_fences() may be waiting on it.
 *
 * @param[in] fence Fence to destroy.
 */
void spudgpu_destroy_fence(spudgpu_fence fence);

/**
 * @brief Reads the value a fence has reached.
 *
 * It is the highest value signaled so far by work that has finished or by
 * spudgpu_signal_fence(), not a value a submission still running has been
 * asked to signal.
 *
 * Thread: may overlap any call on the fence and its device except
 * spudgpu_destroy_fence() and destroying the device.
 *
 * @param[in] fence Fence to read.
 *
 * @return The fence's value, or 0 if `fence` is NULL or the graphics API
 *         fails to read it.
 */
uint64_t spudgpu_get_fence_value(spudgpu_fence fence);

/**
 * @brief Signals a fence to a value from the CPU.
 *
 * Thread: may overlap spudgpu_wait_for_fences() and
 * spudgpu_get_fence_value() on the same fence, and any call on the device
 * except destroying it. Two signals of one fence at once are the caller's
 * to prevent.
 *
 * @param[in] device Device that owns the fence.
 * @param[in] fence  Fence to signal.
 * @param[in] value  Value to signal the fence to. Greater than its current
 *                   value and than every value a submission that hasn't
 *                   finished has been asked to signal it to.
 *
 * @retval SPUD_SUCCESS The fence was signaled.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_FENCE `fence` is NULL.
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE The graphics API failed to signal
 *         it.
 */
SPUDRESULT spudgpu_signal_fence(
    spudgpu_device device,
    spudgpu_fence fence,
    uint64_t value);

/**
 * @brief Blocks the calling thread until one or all of the given fences have
 * reached the given values.
 *
 * `fences[i]` is waited on until its value is at least `values[i]`. A fence
 * already there counts at once, so a wait for a value of 0 never blocks.
 *
 * Thread: may overlap any call on the device and on the fences - another
 * wait, spudgpu_get_fence_value(), spudgpu_signal_fence(), a submission
 * that signals them - except destroying the device or one of the fences.
 * Each wait has its own native wait object.
 *
 * @param[in] device      Device that owns the fences.
 * @param[in] fences      Array of fences to wait on. No entry may be NULL.
 * @param[in] values      Array of the value to wait for on each fence.
 * @param[in] fence_count Number of elements in `fences` and in `values`. At
 *                        most 64.
 * @param[in] wait_all    If true, waits for every fence; if false, for at
 *                        least one.
 * @param[in] timeout_ns  Longest time to wait, in nanoseconds. UINT64_MAX waits
 *                        without limit; 0 only checks.
 *
 * @retval SPUD_SUCCESS The fences reached their values within the timeout.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_FENCE `fences` is NULL, or one of its
 *         entries is.
 * @retval SPUDRESULT_NULL_OBJECT `values` is NULL.
 * @retval SPUDRESULT_ZERO_SIZE `fence_count` is 0.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS `fence_count` is above 64.
 * @retval SPUDRESULT_GPU_FENCE_WAIT_TIMED_OUT The timeout ran out first.
 * @retval SPUDRESULT_GPU_DEVICE_LOST The device was lost, before the wait or
 *         during it: the fences will never reach their values. A wait with
 *         no limit still ends then.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 */
SPUDRESULT spudgpu_wait_for_fences(
    spudgpu_device device,
    spudgpu_fence *fences,
    const uint64_t *values,
    uint32_t fence_count,
    bool wait_all,
    uint64_t timeout_ns);

/**
 * @brief Creates a binary semaphore for GPU-to-GPU synchronization.
 *
 * @param[in]  device        Device that owns the semaphore.
 * @param[out] out_semaphore Receives the new semaphore on success.
 *
 * @retval SPUD_SUCCESS The semaphore was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_semaphore` is NULL.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_semaphore()
 */
SPUDRESULT spudgpu_create_semaphore(
    spudgpu_device device,
    spudgpu_semaphore *out_semaphore);

/**
 * @brief Destroys a semaphore.
 *
 * @param[in] semaphore Semaphore to destroy.
 */
void spudgpu_destroy_semaphore(spudgpu_semaphore semaphore);

/**
 * @brief The kind of memory a buffer is allocated in.
 *
 * One value, not a bitmask. Each kind is a contract that holds on every
 * backend: what the CPU may do with the buffer, what the GPU may do with it,
 * and which usages it may be created with. Where the memory physically is
 * follows from the kind and the device, and is not part of the contract.
 *
 * | Kind            | Map | CPU writes | CPU reads | GPU reads | GPU writes |
 * |-----------------|-----|------------|-----------|-----------|------------|
 * | DEVICE          | no  | -          | -         | yes       | yes        |
 * | UPLOAD          | yes | yes        | no        | yes       | no         |
 * | READBACK        | yes | no         | yes       | no        | copies in  |
 * | DEVICE_MAPPABLE | yes | yes        | yes       | yes       | yes        |
 *
 * Two rules hold for a mapped buffer on every backend, whatever the memory
 * underneath does by itself:
 *
 * - After writing through a mapping, call spudgpu_flush_buffer() over the
 *   bytes written, while the buffer is still mapped and before submitting
 *   work that reads them. What the GPU reads from a byte written and not
 *   flushed is undefined.
 * - Before reading through a mapping, call spudgpu_invalidate_buffer() over
 *   the bytes to read, while the buffer is mapped and after the work that
 *   wrote them has finished. What the CPU reads from a byte the GPU wrote and
 *   that was not invalidated since is undefined.
 *
 * No kind and no device makes either call optional.
 *
 * Images have no memory kind. An image is always in the device's memory, is
 * never mapped, and is written and read with spudgpu_cmd_copy_buffer_to_image()
 * and spudgpu_cmd_copy_image_to_buffer().
 *
 * @see SPUDGPU_MEMORY_KIND_Constants
 */
typedef uint32_t SPUDGPU_MEMORY_KIND;

/**
 * @name Memory Kinds
 * @anchor SPUDGPU_MEMORY_KIND_Constants
 */
/** @{ */
enum {
	/**
	 * Not a kind. spudgpu_create_buffer() returns
	 * SPUDRESULT_GPU_INVALID_MEMORY_KIND for it, as for any value not listed
	 * here: a zeroed desc has not chosen its memory.
	 */
	SPUDGPU_MEMORY_KIND_NONE = 0,

	/**
	 * @brief The device's memory. Never mapped.
	 *
	 * Valid with every usage. The GPU reads and writes it; the CPU reaches
	 * it only through copies (spudgpu_cmd_copy_buffer() from an UPLOAD
	 * buffer, or into a READBACK one). spudgpu_map_buffer() returns
	 * SPUDRESULT_GPU_INVALID_MEMORY_KIND.
	 *
	 * Vulkan: a memory type with `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`.
	 * D3D12: `D3D12_HEAP_TYPE_DEFAULT`. Metal: `MTLStorageModePrivate`.
	 */
	SPUDGPU_MEMORY_KIND_DEVICE = 1,

	/**
	 * @brief Memory the CPU writes and the GPU reads.
	 *
	 * Mappable. The CPU writes through the mapping and flushes; reading
	 * through the mapping is outside the contract. The GPU only reads: the
	 * usage must not include SPUDGPU_BUFFER_USAGE_STORAGE or
	 * SPUDGPU_BUFFER_USAGE_TRANSFER_DST, and the buffer is never given to
	 * spudgpu_cmd_pipeline_barrier(): it is readable in every way its usage
	 * allows from creation on (SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT
	 * included: a build reads it as it is).
	 *
	 * Vulkan: a memory type with `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`.
	 * D3D12: `D3D12_HEAP_TYPE_UPLOAD`. Metal: `MTLStorageModeShared` with
	 * `MTLCPUCacheModeWriteCombined`.
	 */
	SPUDGPU_MEMORY_KIND_UPLOAD = 2,

	/**
	 * @brief Memory the GPU copies into and the CPU reads.
	 *
	 * Mappable. Its usage is exactly SPUDGPU_BUFFER_USAGE_TRANSFER_DST: the
	 * only thing the GPU does with it is copy into it
	 * (spudgpu_cmd_copy_buffer(), spudgpu_cmd_copy_image_to_buffer()). The
	 * CPU invalidates and reads through the mapping; writing through it is
	 * outside the contract. The buffer is never given to
	 * spudgpu_cmd_pipeline_barrier(): it is a copy destination from creation
	 * on.
	 *
	 * Vulkan: a memory type with `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`, one
	 * that is also `VK_MEMORY_PROPERTY_HOST_CACHED_BIT` if the buffer can
	 * use one. D3D12: `D3D12_HEAP_TYPE_READBACK`. Metal:
	 * `MTLStorageModeShared` with `MTLCPUCacheModeDefaultCache`.
	 */
	SPUDGPU_MEMORY_KIND_READBACK = 3,

	/**
	 * @brief The device's memory, mapped: everything
	 * SPUDGPU_MEMORY_KIND_DEVICE allows, and the CPU reads and writes it
	 * too.
	 *
	 * Valid with every usage, shader-writable ones included, and moved
	 * between resource states like a DEVICE buffer. The flush and invalidate
	 * rules apply as to any mapped buffer. Nothing orders a CPU access
	 * against GPU work that is still running: that is the caller's to do.
	 *
	 * Exists only on a device created with
	 * SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY, which only an adapter without
	 * memory of its own supports. On any other device
	 * spudgpu_create_buffer() returns
	 * SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED, and the same data goes in a
	 * DEVICE buffer written through an UPLOAD one.
	 *
	 * Vulkan: a memory type with both `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`
	 * and `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`. D3D12:
	 * `D3D12_HEAP_TYPE_CUSTOM` with `D3D12_MEMORY_POOL_L0` and
	 * `D3D12_CPU_PAGE_PROPERTY_WRITE_BACK`. Metal: `MTLStorageModeShared`
	 * with `MTLCPUCacheModeDefaultCache`.
	 */
	SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE = 4
};

/** @} */

/**
 * @brief Bitmask defining the intended pipeline binding targets for a GPU
 * buffer.
 *
 * Informs the driver how the memory will be bound within hardware command
 * buffers to allow internal optimization.
 *
 * @see SPUDGPU_BUFFER_USAGE_Constants
 */
typedef uint32_t SPUDGPU_BUFFER_USAGE;

/**
 * @name Buffer Pipeline Usage Flags
 * @anchor SPUDGPU_BUFFER_USAGE_Constants
 *
 * Bitmask flags denoting buffer compatibility. A single buffer can fulfill
 * multiple roles by combining flags via bitwise OR (`|`).
 */
/** @{ */
enum {
	/** No hardware binding target specified. */
	SPUDGPU_BUFFER_USAGE_NONE = 0,

	/**
	 * Buffer is compatible for binding as a Input Assembler Vertex Stream
	 * (Vertex Buffer).
	 */
	SPUDGPU_BUFFER_USAGE_VERTEX = 1 << 0,

	/**
	 * Buffer contains index arrays (16-bit or 32-bit indices) for indexed draw
	 * calls (Index Buffer).
	 */
	SPUDGPU_BUFFER_USAGE_INDEX = 1 << 1,

	/**
	 * @brief Buffer acts as a Uniform/Constant Buffer block (Uniform Buffer
	 * or Constant Buffer).
	 *
	 * Optimized for uniform, read-only shader data accessed globally across
	 * shader stages.
	 */
	SPUDGPU_BUFFER_USAGE_UNIFORM = 1 << 2,

	/**
	 * @brief Buffer acts as a Shader Storage Buffer Object (SSBO) /
	 * Structured Buffer.
	 *
	 * Typically used for large, variable-sized data structures requiring
	 * read/write capabilities within compute or graphics shaders.
	 */
	SPUDGPU_BUFFER_USAGE_STORAGE = 1 << 3,

	/**
	 * Buffer can be used as the source of a copy operation (e.g. a staging
	 * buffer read by spudgpu_cmd_copy_buffer_to_image).
	 */
	SPUDGPU_BUFFER_USAGE_TRANSFER_SRC = 1 << 4,

	/**
	 * Buffer can be used as the destination of a copy operation (e.g. a
	 * readback buffer written by spudgpu_cmd_copy_image_to_buffer).
	 */
	SPUDGPU_BUFFER_USAGE_TRANSFER_DST = 1 << 5,

	/**
	 * @brief The buffer holds what an acceleration structure build reads:
	 * vertices, indices, AABBs or instances (SPUDGPU_EXT_RAY_QUERY).
	 *
	 * A structure's own storage is its handle's
	 * (spudgpu_create_acceleration_structure()), and a build's scratch is a
	 * SPUDGPU_BUFFER_USAGE_STORAGE buffer. Vulkan:
	 * `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`,
	 * on a device created with SPUDGPU_FEATURE_RAY_QUERY (on any other device
	 * the bit is kept and no build can read the buffer). D3D12 and Metal have
	 * nothing to set.
	 */
	SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT = 1 << 6,

	/**
	 * @brief Buffer holds spudgpu_draw_indirect_args /
	 * spudgpu_draw_indexed_indirect_args consumed by spudgpu_cmd_draw_indirect
	 * / spudgpu_cmd_draw_indexed_indirect.
	 *
	 * Vulkan-specific creation-time fact (maps to
	 * VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) — D3D12/Metal have no equivalent
	 * creation-time bit for indirect-argument buffers (D3D12 only cares about
	 * the resource *state* at draw time; Metal takes any id<MTLBuffer>), so
	 * this is a no-op on those two backends.
	 */
	SPUDGPU_BUFFER_USAGE_INDIRECT = 1 << 7
};

/** @} */

/**
 * @brief Descriptor used for a physical GPU buffer.
 */
typedef struct spudgpu_buffer_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_BUFFER_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/**
	 * Every way the buffer will be bound or copied, as a bitmask. Which
	 * usages are allowed depends on #memory_kind.
	 * @see SPUDGPU_BUFFER_USAGE
	 */
	SPUDGPU_BUFFER_USAGE usage;

	/**
	 * The kind of memory the buffer is allocated in. Decides whether it can
	 * be mapped and which usages it may have.
	 * @see SPUDGPU_MEMORY_KIND
	 */
	SPUDGPU_MEMORY_KIND memory_kind;

	/** Total allocated size of the buffer resource in bytes. */
	uint64_t size;

	/** @brief A name for diagnostics, or NULL. Copied, in every build; see
	 * spud_debug_name_set(). */
	const char *debug_name;
} spudgpu_buffer_desc;

/**
 * @brief Allocates a GPU buffer.
 *
 * The buffer's contents are undefined until written, on every backend: what
 * the CPU or the GPU reads from a byte nothing has written is not zero or
 * any other value the caller can rely on.
 *
 * @param[in]  device     Device the buffer is allocated on.
 * @param[in]  desc       Buffer configuration.
 * @param[out] out_buffer Receives the new buffer on success.
 *
 * @retval SPUD_SUCCESS The buffer was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_BUFFER_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_buffer` is NULL.
 * @retval SPUDRESULT_GPU_ZERO_BUFFER_SIZE `desc->size` is 0.
 * @retval SPUDRESULT_GPU_INVALID_BUFFER_USAGE `desc->usage` is
 *         SPUDGPU_BUFFER_USAGE_NONE, or is one `desc->memory_kind` does not
 *         allow (see SPUDGPU_MEMORY_KIND).
 * @retval SPUDRESULT_GPU_INVALID_MEMORY_KIND `desc->memory_kind` is
 *         SPUDGPU_MEMORY_KIND_NONE or not a SPUDGPU_MEMORY_KIND.
 * @retval SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED `desc->memory_kind` is
 *         SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE and the device wasn't created
 *         with SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * The checks run in the order listed: SPUDGPU_BUFFER_USAGE_NONE, then the
 * memory kind, then the usage against the kind, then the device.
 *
 * @see spudgpu_destroy_buffer()
 */
SPUDRESULT spudgpu_create_buffer(
    spudgpu_device device,
    const spudgpu_buffer_desc *desc,
    spudgpu_buffer *out_buffer);

/**
 * @brief Destroys a GPU buffer and frees its memory.
 *
 * A buffer placed in a heap (spudgpu_create_placed_buffer()) frees nothing
 * to the device: its bytes stay the heap's.
 *
 * @warning Views of the buffer must be destroyed first, and command lists that
 * reference it must have finished executing.
 *
 * @param[in] buffer Buffer to destroy.
 */
void spudgpu_destroy_buffer(spudgpu_buffer buffer);

/**
 * @brief Reads the descriptor a buffer was created with.
 *
 * `out_desc->debug_name` is the buffer's own copy of its name (see
 * spud_debug_name_set()): valid until the buffer is renamed or destroyed,
 * and NULL if it has none.
 *
 * @param[in]  buffer   Buffer to query.
 * @param[out] out_desc Receives the buffer's descriptor.
 *
 * @retval SPUD_SUCCESS `out_desc` was filled in.
 * @retval SPUDRESULT_GPU_INVALID_BUFFER `buffer` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_desc` is NULL.
 */
SPUDRESULT spudgpu_get_buffer_desc(
    spudgpu_buffer buffer,
    spudgpu_buffer_desc *out_desc);

/**
 * @brief Reads a buffer's GPU virtual address.
 *
 * Every buffer has one, on every backend, from the moment it is created
 * until it is destroyed: D3D12's GetGPUVirtualAddress, Vulkan's
 * vkGetBufferDeviceAddress, Metal's gpuAddress. A buffer placed in a heap
 * has one too.
 *
 * @param[in]  buffer      Buffer to query.
 * @param[out] out_address Receives the address.
 *
 * @retval SPUD_SUCCESS `out_address` was filled in.
 * @retval SPUDRESULT_GPU_INVALID_BUFFER `buffer` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_address` is NULL.
 */
SPUDRESULT spudgpu_get_buffer_gpu_address(
    spudgpu_buffer buffer,
    uint64_t *out_address);

/**
 * @brief Configuration descriptor defining a sub-allocated window (view) into
 * an existing buffer.
 *
 * Buffer views are lightweight abstractions allowing you to reinterpret
 * specific segments of a parent buffer without duplicating or allocating
 * additional physical device memory.
 */
typedef struct spudgpu_buffer_view_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** Handle to the GPU buffer that this view window reads from or writes to. */
	spudgpu_buffer parent_buffer;

	/**
	 * The byte offset to where this specific view window begins inside the
	 * parent buffer.
	 */
	uint64_t offset_from_parent_buffer;

	/**
	 * @brief The data structure element stride in bytes.
	 *
	 * Crucial for structured storage arrays or vertex inputs (e.g., byte size
	 * of a single instance of your custom Vertex struct). Pass `0` for raw,
	 * unformatted buffers.
	 */
	uint64_t stride;

	/** The span/length of this specific buffer view window in bytes. */
	uint64_t size;
} spudgpu_buffer_view_desc;

/**
 * @brief Creates a view of a range of an existing GPU buffer.
 *
 * @param[in]  buffer          Buffer the view looks into.
 * @param[in]  desc            View configuration.
 * @param[out] out_buffer_view Receives the new view on success.
 *
 * @retval SPUD_SUCCESS The view was created.
 * @retval SPUDRESULT_GPU_INVALID_BUFFER `buffer` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_buffer_view` is NULL.
 * @retval SPUDRESULT_GPU_ZERO_BUFFER_SIZE `desc->size` is 0.
 * @retval SPUDRESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE
 *         `desc->offset_from_parent_buffer + desc->size` runs past the end of
 *         the buffer.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_buffer_view()
 */
SPUDRESULT spudgpu_create_buffer_view(
    spudgpu_buffer buffer,
    const spudgpu_buffer_view_desc *desc,
    spudgpu_buffer_view *out_buffer_view);

/**
 * @brief Destroys a GPU buffer view.
 *
 * The parent buffer and its contents are not affected.
 *
 * @param[in] buffer_view View to destroy.
 */
void spudgpu_destroy_buffer_view(spudgpu_buffer_view buffer_view);

/**
 * @brief Reads the descriptor a buffer view was created with.
 *
 * @param[in]  view     Buffer view to query.
 * @param[out] out_desc Receives the view's descriptor.
 *
 * @retval SPUD_SUCCESS `out_desc` was filled in.
 * @retval SPUDRESULT_GPU_INVALID_BUFFER_VIEW `view` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_desc` is NULL.
 */
SPUDRESULT spudgpu_get_buffer_view_desc(
    spudgpu_buffer_view view,
    spudgpu_buffer_view_desc *out_desc);

/**
 * @brief Maps a buffer's memory into CPU-accessible address space.
 *
 * Valid on a buffer of every SPUDGPU_MEMORY_KIND except
 * SPUDGPU_MEMORY_KIND_DEVICE. The kind decides what may be done through the
 * pointer: see SPUDGPU_MEMORY_KIND.
 *
 * A buffer has one mapping at a time: it is unmapped before it is mapped
 * again. It may stay mapped for as long as it exists, including while the GPU
 * uses it.
 *
 * @param[in]  buffer Buffer to map.
 * @param[in]  offset Byte offset into the buffer where the mapped region
 *                    begins.
 * @param[in]  size   Number of bytes to map. Pass 0 to map the entire buffer.
 * @param[out] ppData Receives the CPU address of the byte at `offset`.
 *
 * @retval SPUD_SUCCESS The buffer was mapped.
 * @retval SPUDRESULT_GPU_INVALID_BUFFER `buffer` is NULL.
 * @retval SPUDRESULT_GPU_MAP_OUT_OF_RANGE `offset + size` runs past the end of
 *         the buffer.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `ppData` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_MEMORY_KIND The buffer is of
 *         SPUDGPU_MEMORY_KIND_DEVICE.
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE The graphics API failed to map it.
 *
 * @see spudgpu_unmap_buffer()
 */
SPUDRESULT spudgpu_map_buffer(
    spudgpu_buffer buffer,
    uint64_t offset,
    uint64_t size,
    void **ppData);

/**
 * @brief Unmaps a mapped buffer; the pointer from spudgpu_map_buffer() is no
 * longer valid.
 *
 * Unmapping flushes nothing: bytes written through the mapping are flushed
 * with spudgpu_flush_buffer() first.
 *
 * @param[in] buffer Buffer to unmap.
 */
void spudgpu_unmap_buffer(spudgpu_buffer buffer);

/**
 * @brief Makes bytes written through a mapping visible to the GPU.
 *
 * Called after every write through a mapping, on every backend and every
 * device, over the bytes written: while the buffer is still mapped, and
 * before submitting work that reads them. `offset` is from the start of the
 * buffer, not of the mapping, and the range lies inside what was mapped.
 *
 * Valid on a buffer of SPUDGPU_MEMORY_KIND_UPLOAD or
 * SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE, the kinds the CPU writes.
 *
 * @param[in] buffer Buffer whose writes need flushing.
 * @param[in] offset Byte offset of the written region.
 * @param[in] size   Byte length of the written region. Pass 0 for everything
 *                   from `offset` to the end of the buffer.
 *
 * @retval SPUD_SUCCESS The range was flushed.
 * @retval SPUDRESULT_GPU_INVALID_BUFFER `buffer` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_MEMORY_KIND The buffer is of
 *         SPUDGPU_MEMORY_KIND_DEVICE or SPUDGPU_MEMORY_KIND_READBACK.
 * @retval SPUDRESULT_GPU_MAP_OUT_OF_RANGE `offset + size` runs past the end of
 *         the buffer.
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE The graphics API failed to flush
 *         it.
 */
SPUDRESULT spudgpu_flush_buffer(
    spudgpu_buffer buffer,
    uint64_t offset,
    uint64_t size);

/**
 * @brief Makes bytes the GPU wrote visible through a mapping.
 *
 * Called before every read through a mapping of bytes the GPU has written
 * since they were last invalidated, on every backend and every device: while
 * the buffer is mapped, and after the work that wrote them has finished.
 * `offset` is from the start of the buffer, not of the mapping, and the
 * range lies inside what was mapped.
 *
 * Valid on a buffer of SPUDGPU_MEMORY_KIND_READBACK or
 * SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE, the kinds the CPU reads.
 *
 * @param[in] buffer Buffer to invalidate.
 * @param[in] offset Byte offset of the region to invalidate.
 * @param[in] size   Byte length of the region. Pass 0 for everything from
 *                   `offset` to the end of the buffer.
 *
 * @retval SPUD_SUCCESS The range was invalidated.
 * @retval SPUDRESULT_GPU_INVALID_BUFFER `buffer` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_MEMORY_KIND The buffer is of
 *         SPUDGPU_MEMORY_KIND_DEVICE or SPUDGPU_MEMORY_KIND_UPLOAD.
 * @retval SPUDRESULT_GPU_MAP_OUT_OF_RANGE `offset + size` runs past the end of
 *         the buffer.
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE The graphics API failed to
 *         invalidate it.
 */
SPUDRESULT spudgpu_invalidate_buffer(
    spudgpu_buffer buffer,
    uint64_t offset,
    uint64_t size);

/**
 * @brief Dimensionality of an image.
 *
 * One value: the constants are numbered as bits, and are not combined.
 */
typedef uint32_t SPUDGPU_IMAGE_TYPE;

enum {
	/** Not a type. */
	SPUDGPU_IMAGE_TYPE_NONE = 0,
	/** One dimension: `height` and `depth` are 1. */
	SPUDGPU_IMAGE_TYPE_1D   = 1 << 0,
	/**
	 * Two dimensions: `depth` is 1. Also the type of a cube's image: a cube
	 * is not an image type but a way of viewing six layers of a 2D image
	 * created with spudgpu_image_desc::cube_capable.
	 */
	SPUDGPU_IMAGE_TYPE_2D   = 1 << 1,
	/** Three dimensions. */
	SPUDGPU_IMAGE_TYPE_3D   = 1 << 2
};

/**
 * @brief Bitmask determining the type and capabilities of a GPU image object.
 *
 * @see SPUDGPU_IMAGE_USAGE_Constants
 */
typedef uint32_t SPUDGPU_IMAGE_USAGE;

/**
 * @name Image Usage Flags
 * @anchor SPUDGPU_IMAGE_USAGE_Constants
 *
 * Bitmask configurations dictating how the GPU image object is structured and
 * sampled.
 */
/** @{ */
enum {
	/** No usage target specified. Uninitialized configuration state. */
	SPUDGPU_IMAGE_USAGE_NONE = 0,

	/** Read by a shader through a sampled image binding. */
	SPUDGPU_IMAGE_USAGE_SAMPLED = 1 << 0,

	/** Rendered into as a color attachment. */
	SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT         = 1 << 1,
	/** Rendered into as the depth/stencil attachment. */
	SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT = 1 << 2,
	/** Read and written by a shader through a storage image binding. */
	SPUDGPU_IMAGE_USAGE_STORAGE                  = 1 << 3,
	/** Read by a copy or blit. */
	SPUDGPU_IMAGE_USAGE_TRANSFER_SRC             = 1 << 4,
	/** Written by a copy or blit. */
	SPUDGPU_IMAGE_USAGE_TRANSFER_DST             = 1 << 5,

	/**
	 * Image is compatible with being presented to a swap chain surface.
	 * Reserved for future callers creating a presentable image outside of
	 * spudgpu_swap_chain's own internally-owned backbuffers — no current
	 * caller in this workspace sets it.
	 */
	SPUDGPU_IMAGE_USAGE_PRESENTABLE               = 1 << 6,

	/**
	 * @brief Marks a render-pass attachment as transient: written and
	 * read only within the render pass that produces it, never sampled,
	 * read back, or persisted afterward.
	 *
	 * On tile-based GPUs (all Metal devices, and mobile Vulkan hardware such
	 * as Mali/Adreno) this lets the implementation back the image with
	 * on-chip tile memory instead of a real allocation, entirely skipping
	 * device-memory traffic — the standard technique for a multisampled
	 * render target that only exists to be resolved, or a deferred-shading
	 * G-buffer read back via programmable blending within the same pass.
	 * Maps to Metal's `MTLStorageModeMemoryless` and Vulkan's
	 * `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` +
	 * `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT`; D3D12 has no equivalent and
	 * treats it as a no-op hint.
	 *
	 * Must be combined with `SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT` and/or
	 * `SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT`, and must not be
	 * combined with `SPUDGPU_IMAGE_USAGE_SAMPLED`, `_STORAGE`,
	 * `_TRANSFER_SRC`, `_TRANSFER_DST`, or `_PRESENTABLE` — all of those
	 * require the image to be readable/writable outside the render pass
	 * that produced it,
	 * which a transient attachment structurally cannot be. Enforced at
	 * creation on every backend, not just the ones that act on it, so a
	 * desc that's invalid here doesn't silently behave differently
	 * depending on which backend happens to be compiled in.
	 */
	SPUDGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT      = 1 << 7
};


/**
 * @brief A depth value and a stencil value, used together as a clear value.
 */
/* Frozen: a value, held by value in SPUDGPU_CLEAR_VALUE. It never changes; a new need gets a new type. */
typedef struct SPUDGPU_DEPTH_STENCIL_VALUE {
	/** Depth, 0 to 1. */
	float depth;
	/** Stencil. */
	uint8_t stencil;
} SPUDGPU_DEPTH_STENCIL_VALUE;

/**
 * @brief The value an image is expected to be cleared to: a color, or a
 * depth/stencil pair, depending on `format`.
 */
/* Frozen: a value, held by value in spudgpu_image_desc. It never changes; a new need gets a new type. */
typedef struct SPUDGPU_CLEAR_VALUE {
	/** The format the value is for. */
	SPUDGPU_FORMAT format;
	union {
		/** RGBA, for a color format. */
		float color[4];
		/** For a depth/stencil format. */
		SPUDGPU_DEPTH_STENCIL_VALUE depth_stencil;
	};
} SPUDGPU_CLEAR_VALUE;

/** @} */

/**
 * @brief Configuration descriptor used to allocate a physical GPU image
 * resource (texture/surface).
 *
 * There is no memory kind to choose: an image is always in the device's
 * memory and is never mapped (see SPUDGPU_MEMORY_KIND).
 */
typedef struct spudgpu_image_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_IMAGE_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/**
	 * Every way the image will be used, as a bitmask.
	 * @see SPUDGPU_IMAGE_USAGE
	 */
	SPUDGPU_IMAGE_USAGE usage;

	/**
	 * The image's dimensionality.
	 * @see SPUDGPU_IMAGE_TYPE
	 */
	SPUDGPU_IMAGE_TYPE type;

	/**
	 * The value an attachment will most often be cleared to. D3D12 creates
	 * a color or depth/stencil attachment with it as the optimized clear
	 * value, taking the format from #format and not from
	 * SPUDGPU_CLEAR_VALUE::format. Vulkan and Metal do not read it.
	 */
	SPUDGPU_CLEAR_VALUE clear_value;

	/**
	 * The texel data layout and channel bit-depth configuration (e.g.,
	 * RGBA8_UNORM, R32_SFLOAT).
	 * @see SPUDGPU_FORMAT
	 */
	SPUDGPU_FORMAT format;

	/** The horizontal width the image base layer in texels/pixels. */
	uint32_t width;

	/** The vertical height of the image base layer in texels/pixels. */
	uint32_t height;

	/**
	 * @brief The volumetric depth of the image in texels.
	 *
	 * Must be set to `1` for 1D and 2D images. The third axis of a
	 * SPUDGPU_IMAGE_TYPE_3D image.
	 */
	uint32_t depth;

	/**
	 * @brief Number of layers inside a texture array allocation.
	 *
	 * Allows for texture arrays (e.g., a series of 2D textures grouped into a
	 * single bindable allocation). Must be set to at least `1` for standard
	 * standalone textures.
	 */
	uint32_t array_layers;

	/**
	 * @brief Total number of downsampled mipmap resolution steps allocated
	 * for this texture.
	 *
	 * Pass `1` for no mipmaps. Level [0] represents the original
	 * full-resolution image data.
	 */
	uint32_t mip_levels;
	/**
	 * @brief Whether the image's layers can be viewed as the faces of cubes.
	 *
	 * Required for a view of SPUDGPU_IMAGE_VIEW_TYPE_CUBE or
	 * SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY, and it is the same rule on every
	 * backend, though only one graphics API has to be told when the image is
	 * created: a cube view of an image created without it is refused
	 * everywhere. Such an image is SPUDGPU_IMAGE_TYPE_2D, has `width` equal
	 * to `height`, and has `array_layers` a multiple of six, each six in a
	 * row being the +X, -X, +Y, -Y, +Z and -Z faces of one cube. It is
	 * otherwise an ordinary layered 2D image: one layer can still be viewed
	 * as SPUDGPU_IMAGE_VIEW_TYPE_2D, to render a face, and several as
	 * SPUDGPU_IMAGE_VIEW_TYPE_2D_ARRAY.
	 */
	bool cube_capable;
	/** @brief A name for diagnostics, or NULL. Copied, in every build; see
	 * spud_debug_name_set(). */
	const char *debug_name;
} spudgpu_image_desc;

/**
 * @brief Allocates a GPU image (texture).
 *
 * The image's contents are undefined until written or cleared, on every
 * backend: what is read from a texel nothing has written is not zero or any
 * other value the caller can rely on.
 *
 * @param[in]  device    Device the image is allocated on.
 * @param[in]  desc      Image configuration.
 * @param[out] out_image Receives the new image on success.
 *
 * @retval SPUD_SUCCESS The image was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_IMAGE_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_image` is NULL.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS `desc->width`, `height`, `depth`,
 *         `array_layers` or `mip_levels` is 0; or, checked after the usage
 *         and type checks below, `desc->cube_capable` is set and
 *         `desc->width` is not `desc->height`, or `desc->array_layers` is not
 *         a multiple of six.
 * @retval SPUDRESULT_GPU_INVALID_IMAGE_USAGE `desc->usage` is
 *         SPUDGPU_IMAGE_USAGE_NONE, or combines
 *         SPUDGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT with usages it does not
 *         allow.
 * @retval SPUDRESULT_GPU_INVALID_IMAGE_TYPE `desc->cube_capable` is set and
 *         `desc->type` is not SPUDGPU_IMAGE_TYPE_2D.
 * @retval SPUDRESULT_GPU_INVALID_FORMAT `desc->format` is
 *         SPUDGPU_FORMAT_UNKNOWN.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_image()
 */
SPUDRESULT spudgpu_create_image(
    spudgpu_device device,
    const spudgpu_image_desc *desc,
    spudgpu_image *out_image);

/**
 * @brief Destroys a GPU image and frees its memory.
 *
 * An image placed in a heap (spudgpu_create_placed_image()) frees nothing
 * to the device: its bytes stay the heap's.
 *
 * @warning Views of the image must be destroyed first, and command lists that
 * reference it must have finished executing.
 *
 * @param[in] image Image to destroy.
 */
void spudgpu_destroy_image(spudgpu_image image);

/**
 * @brief Reads the descriptor an image was created with.
 *
 * `out_desc->debug_name` is the image's own copy of its name (see
 * spud_debug_name_set()): valid until the image is renamed or destroyed,
 * and NULL if it has none.
 *
 * @param[in]  image    Image to query.
 * @param[out] out_desc Receives the image's descriptor.
 *
 * @retval SPUD_SUCCESS `out_desc` was filled in.
 * @retval SPUDRESULT_GPU_INVALID_IMAGE `image` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_desc` is NULL.
 */
SPUDRESULT spudgpu_get_image_desc(
    spudgpu_image image,
    spudgpu_image_desc *out_desc);

/**
 * @brief How an image view presents its image to shaders.
 *
 * One value: the constants are numbered as bits, and are not combined.
 */
typedef uint32_t SPUDGPU_IMAGE_VIEW_TYPE;

enum {
	/** Not a type. */
	SPUDGPU_IMAGE_VIEW_TYPE_NONE       = 0,
	/** One layer of a 1D image. */
	SPUDGPU_IMAGE_VIEW_TYPE_1D         = 1 << 0,
	/** One layer of a 2D image. */
	SPUDGPU_IMAGE_VIEW_TYPE_2D         = 1 << 1,
	/** A 3D image. */
	SPUDGPU_IMAGE_VIEW_TYPE_3D         = 1 << 2,
	/**
	 * Six layers of a 2D image, as the faces of a cube. The image is
	 * spudgpu_image_desc::cube_capable, and the view's range is exactly
	 * layers 0 to 5: `base_array_layer` 0 and `array_layer_count` 6. One
	 * cube out of several is reached with SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY,
	 * since not every graphics API can start a single cube's view at
	 * another layer.
	 */
	SPUDGPU_IMAGE_VIEW_TYPE_CUBE       = 1 << 3,
	/** Several layers of a 1D image. */
	SPUDGPU_IMAGE_VIEW_TYPE_1D_ARRAY   = 1 << 4,
	/** Several layers of a 2D image. */
	SPUDGPU_IMAGE_VIEW_TYPE_2D_ARRAY   = 1 << 5,
	/**
	 * A multiple of six layers of a 2D image, as an array of cubes. The
	 * image is spudgpu_image_desc::cube_capable, and `array_layer_count` is
	 * a multiple of six and not 0.
	 */
	SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY = 1 << 6
};

/**
 * @brief The aspects an image's format can have, for
 * spudgpu_image_view_desc_subresource_range::aspect_mask.
 *
 * The values are `VkImageAspectFlagBits`' (Vulkan). D3D12 reaches the
 * stencil as plane 1 of the resource through a view format of its own, and
 * Metal through a view in a stencil pixel format.
 */
typedef uint64_t SPUDGPU_IMAGE_ASPECT;
enum {
	/** No aspect. Refused wherever an aspect mask is taken. */
	SPUDGPU_IMAGE_ASPECT_NONE    = 0,
	/** The color of a color format. */
	SPUDGPU_IMAGE_ASPECT_COLOR   = 1,
	/** The depth of a depth format, or of a depth and stencil format. */
	SPUDGPU_IMAGE_ASPECT_DEPTH   = 2,
	/** The stencil of a depth and stencil format. */
	SPUDGPU_IMAGE_ASPECT_STENCIL = 4
};

/**
 * @brief Defines a specific subsection (slice) of a multi-layered or mipmapped
 * texture.
 *
 * Allows a pipeline stage to isolate and bind specific layers or mip levels
 * of a parent image rather than the entire allocation.
 */
/* Frozen: held by value in spudgpu_image_view_desc. It never changes; a new need gets a new type. */
typedef struct spudgpu_image_view_desc_subresource_range {
	/**
	 * @brief Which aspects of the image the range covers, as
	 * SPUDGPU_IMAGE_ASPECT bits.
	 *
	 * For a view (spudgpu_create_image_view()) these rules hold on every
	 * backend, whatever the graphics API underneath would also accept:
	 *
	 * - Never 0, and never an aspect the image's format lacks: a color
	 *   format has SPUDGPU_IMAGE_ASPECT_COLOR alone, a depth format
	 *   SPUDGPU_IMAGE_ASPECT_DEPTH alone, and a depth and stencil format
	 *   SPUDGPU_IMAGE_ASPECT_DEPTH and SPUDGPU_IMAGE_ASPECT_STENCIL. A view
	 *   that breaks this is refused.
	 * - A view given to spudgpu_cmd_begin_rendering() as the depth/stencil
	 *   attachment covers every aspect of its format.
	 * - A view written into a descriptor set covers exactly one aspect. Of
	 *   a depth and stencil format that is the depth, read as a float in
	 *   the first component, or the stencil, read as an unsigned integer
	 *   in the first component. What a shader reads through a view of both
	 *   aspects is undefined.
	 * - What a shader reads from the components after the first, through a
	 *   depth or a stencil view, is undefined.
	 *
	 * So a depth and stencil image that is rendered to and sampled has a
	 * view for each use.
	 */
	uint64_t aspect_mask;

	/**
	 * The starting mipmap level index for this view window (0 being the
	 * highest resolution).
	 */
	uint64_t base_mip_level;

	/**
	 * The total number of downsampled mipmap levels to include in this view
	 * range.
	 */
	uint64_t mip_level_count;

	/**
	 * The starting array layer index for this view window (used for texture
	 * arrays/cubemaps).
	 */
	uint64_t base_array_layer;

	/** The total number of layers to include in this view range. */
	uint64_t array_layer_count;
} spudgpu_image_view_desc_subresource_range;

/**
 * @brief Configuration descriptor defining a read/write window (view) into an
 * existing GPU image.
 *
 * Like buffer views, image views do not allocate raw VRAM; they wrap existing
 * physical images to declare how shader stages should interpret their
 * boundaries and contents.
 *
 * @todo Component swizzle is not exposed: a view reads its image's channels
 * in their own order.
 */
typedef struct spudgpu_image_view_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** Handle to the physical GPU image containing the pixel data. */
	spudgpu_image parent_image;

	/**
	 * The image view type
	 * @see SPUDGPU_IMAGE_VIEW_TYPE
	 */
	uint32_t type;

	/**
	 * The isolated subresource layer and mip boundary definitions for this
	 * view.
	 */
	spudgpu_image_view_desc_subresource_range subresource_range;
} spudgpu_image_view_desc;

/**
 * @brief Creates a view of a subresource range of a GPU image.
 *
 * @param[in]  image          Image the view looks into.
 * @param[in]  desc           View configuration.
 * @param[out] out_image_view Receives the new view on success.
 *
 * @retval SPUD_SUCCESS The view was created.
 * @retval SPUDRESULT_GPU_INVALID_IMAGE `image` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_image_view` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_IMAGE_VIEW_TYPE `desc->type` is
 *         SPUDGPU_IMAGE_VIEW_TYPE_CUBE or SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY
 *         and the image was not created with
 *         spudgpu_image_desc::cube_capable, or it is
 *         SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY on a device created without
 *         SPUDGPU_FEATURE_CUBE_ARRAY_VIEWS.
 * @retval SPUDRESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE `desc->type`
 *         is SPUDGPU_IMAGE_VIEW_TYPE_CUBE and the range is not layers 0 to
 *         5, or it is SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY and
 *         `array_layer_count` is 0 or not a multiple of six.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS
 *         `desc->subresource_range.aspect_mask` is 0 or names an aspect the
 *         image's format lacks.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_image_view()
 */
SPUDRESULT spudgpu_create_image_view(
    spudgpu_image image,
    const spudgpu_image_view_desc *desc,
    spudgpu_image_view *out_image_view);

/**
 * @brief Destroys a GPU image view.
 *
 * The parent image and its contents are not affected.
 *
 * @param[in] image_view View to destroy.
 */
void spudgpu_destroy_image_view(spudgpu_image_view image_view);

/**
 * @brief Reads the descriptor an image view was created with.
 *
 * @param[in]  image_view Image view to query.
 * @param[out] out_desc   Receives the view's descriptor.
 *
 * @retval SPUD_SUCCESS `out_desc` was filled in.
 * @retval SPUDRESULT_GPU_INVALID_IMAGE_VIEW `image_view` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_desc` is NULL.
 */
SPUDRESULT spudgpu_get_image_view_desc(
    spudgpu_image_view image_view,
    spudgpu_image_view_desc *out_desc);

// ============================================================================
//  Heaps and placed resources
//  Maps to: VkDeviceMemory (Vulkan), ID3D12Heap (D3D12), MTLHeap (Metal)
// ============================================================================

/**
 * @brief What a heap holds: buffers, images that are never attachments, or
 * images that are.
 *
 * A heap holds one class and nothing else, on every backend. That is D3D12's
 * resource heap tier 1 made the rule (D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS,
 * _ALLOW_ONLY_NON_RT_DS_TEXTURES, _ALLOW_ONLY_RT_DS_TEXTURES, set on every
 * D3D12 device, tier 2 included). Vulkan picks a heap's memory type by its
 * class, and never has buffers and images in one allocation.
 *
 * Which class an image is in follows from its usage, not a choice: one with
 * SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT or
 * SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT goes in an
 * SPUDGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES heap, any other in an
 * SPUDGPU_HEAP_RESOURCE_CLASS_IMAGES heap.
 *
 * @see SPUDGPU_HEAP_RESOURCE_CLASS_Constants
 */
typedef uint32_t SPUDGPU_HEAP_RESOURCE_CLASS;

/**
 * @name Heap Resource Classes
 * @anchor SPUDGPU_HEAP_RESOURCE_CLASS_Constants
 */
/** @{ */
enum {
	/**
	 * Not a class. spudgpu_create_heap() returns
	 * SPUDRESULT_GPU_INVALID_HEAP_RESOURCE_CLASS for it, as for any value
	 * not listed here: a zeroed desc has not chosen.
	 */
	SPUDGPU_HEAP_RESOURCE_CLASS_NONE = 0,
	/** Buffers of the heap's memory kind. */
	SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS = 1,
	/** Images with neither attachment usage. The heap's kind is
	 *  SPUDGPU_MEMORY_KIND_DEVICE. */
	SPUDGPU_HEAP_RESOURCE_CLASS_IMAGES = 2,
	/** Images with a color or depth/stencil attachment usage. The heap's
	 *  kind is SPUDGPU_MEMORY_KIND_DEVICE. */
	SPUDGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES = 3,
	/** Acceleration structures (spudgpu_create_placed_acceleration_structure(),
	 *  SPUDGPU_EXT_RAY_QUERY). The heap's kind is SPUDGPU_MEMORY_KIND_DEVICE,
	 *  and spudgpu_create_heap() refuses the class with
	 *  SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED on a device created without
	 *  SPUDGPU_FEATURE_RAY_QUERY. */
	SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES = 4
};
/** @} */

/**
 * Every heap's size is a multiple of this many bytes. It is D3D12's heap
 * alignment (`D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT`) made the rule.
 */
#define SPUDGPU_HEAP_SIZE_GRANULARITY 65536u

/**
 * @brief Describes a heap: one device allocation that buffers and images are
 * placed in at offsets the caller chooses.
 *
 * SpudGPU keeps no record of what is placed where in a heap. Which heap a
 * resource goes in, at which offset, and when a heap is created or destroyed
 * are the caller's.
 */
typedef struct spudgpu_heap_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_HEAP_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/**
	 * The memory every resource placed in the heap is in, with the contract
	 * SPUDGPU_MEMORY_KIND states for a buffer of that kind. An image heap's
	 * is SPUDGPU_MEMORY_KIND_DEVICE: images have no other.
	 * @see SPUDGPU_MEMORY_KIND
	 */
	SPUDGPU_MEMORY_KIND memory_kind;

	/** What the heap holds. @see SPUDGPU_HEAP_RESOURCE_CLASS */
	SPUDGPU_HEAP_RESOURCE_CLASS resource_class;

	/** Bytes. A multiple of SPUDGPU_HEAP_SIZE_GRANULARITY, not 0. */
	uint64_t size;
	/** @brief A name for diagnostics, or NULL. Copied, in every build; see
	 * spud_debug_name_set(). */
	const char *debug_name;
} spudgpu_heap_desc;

/**
 * @brief Allocates a heap.
 *
 * Its contents are undefined, as a new buffer's are: placing a resource in it
 * does not clear the bytes the resource covers.
 *
 * @param[in]  device   Device the heap is allocated on.
 * @param[in]  desc     Heap configuration.
 * @param[out] out_heap Receives the new heap on success.
 *
 * @retval SPUD_SUCCESS The heap was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_HEAP_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_heap` is NULL.
 * @retval SPUDRESULT_ZERO_SIZE `desc->size` is 0.
 * @retval SPUDRESULT_GPU_INVALID_HEAP_SIZE `desc->size` is not a multiple of
 *         SPUDGPU_HEAP_SIZE_GRANULARITY.
 * @retval SPUDRESULT_GPU_INVALID_HEAP_RESOURCE_CLASS `desc->resource_class`
 *         is SPUDGPU_HEAP_RESOURCE_CLASS_NONE or not a class.
 * @retval SPUDRESULT_GPU_INVALID_MEMORY_KIND `desc->memory_kind` is
 *         SPUDGPU_MEMORY_KIND_NONE or not a kind, or the class is not
 *         SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS and the kind is not
 *         SPUDGPU_MEMORY_KIND_DEVICE.
 * @retval SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED The kind is
 *         SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_MEMORY_KIND_READBACK or
 *         SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE and the device wasn't created
 *         with SPUDGPU_FEATURE_MAPPABLE_HEAPS, or it is
 *         SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE and the device wasn't created
 *         with SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY.
 * @retval SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED The class is
 *         SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES and the device
 *         wasn't created with SPUDGPU_FEATURE_RAY_QUERY.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE (the
 *         device having no room for a heap that size is the usual one).
 *
 * The checks run in the order listed.
 *
 * @see spudgpu_destroy_heap()
 */
SPUDRESULT spudgpu_create_heap(
    spudgpu_device device,
    const spudgpu_heap_desc *desc,
    spudgpu_heap *out_heap);

/**
 * @brief Frees a heap's memory.
 *
 * @warning Every buffer and image placed in the heap must be destroyed first,
 * which means the command lists that reference them have finished. A debug
 * build asserts that none is left.
 *
 * @param[in] heap Heap to destroy. NULL does nothing.
 */
void spudgpu_destroy_heap(spudgpu_heap heap);

/**
 * @brief Reads the descriptor a heap was created with.
 *
 * @param[in]  heap     Heap to query.
 * @param[out] out_desc Receives the heap's descriptor.
 *
 * @retval SPUD_SUCCESS `out_desc` was filled in.
 * @retval SPUDRESULT_GPU_INVALID_HEAP `heap` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_desc` is NULL.
 */
SPUDRESULT spudgpu_get_heap_desc(
    spudgpu_heap heap,
    spudgpu_heap_desc *out_desc);

/**
 * @brief What a resource needs from a heap: how many bytes from its offset,
 * and what its offset is a multiple of.
 */
typedef struct spudgpu_placement_requirements {
	/** `type` is SPUD_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS; `next` is NULL (nothing is chained onto
	 * this struct yet). The caller builds the chain and sets each type; SpudGPU fills in the rest. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE_OUT base;
	/**
	 * Bytes the resource covers from its offset. Never less than a buffer's
	 * size, and often more: on D3D12 every buffer covers a multiple of
	 * 64 KiB.
	 */
	uint64_t size;
	/** A power of two the offset is a multiple of. */
	uint64_t alignment;
} spudgpu_placement_requirements;

/**
 * @brief Reports what a buffer of `desc` needs to be placed in `heap`.
 *
 * The heap is asked, not the device, because the answer is its memory's. In
 * a heap of a mappable kind, the size and alignment are both multiples of
 * what the device flushes and invalidates in, so spudgpu_flush_buffer() and
 * spudgpu_invalidate_buffer() on one placed buffer never touch a byte of
 * another.
 *
 * The same `desc` and heap give the same answer for as long as the heap
 * exists. Nothing is allocated in the heap.
 *
 * @param[in]  heap             Heap the buffer would be placed in.
 * @param[in]  desc             The buffer, as spudgpu_create_placed_buffer()
 *                              would be given it.
 * @param[out] out_requirements Receives the size and alignment.
 *
 * @retval SPUD_SUCCESS `out_requirements` was filled in.
 * @retval SPUDRESULT_GPU_INVALID_HEAP `heap` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_BUFFER_DESC; or, checked after
 *         `out_requirements` is found not NULL, `out_requirements->base.type`
 *         is not SPUD_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL;
 *         or, checked after `out_requirements->base.type`,
 *         `out_requirements->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_requirements` is NULL.
 * @retval SPUDRESULT_GPU_ZERO_BUFFER_SIZE `desc->size` is 0.
 * @retval SPUDRESULT_GPU_INVALID_BUFFER_USAGE `desc->usage` is
 *         SPUDGPU_BUFFER_USAGE_NONE, or is one `desc->memory_kind` does not
 *         allow.
 * @retval SPUDRESULT_GPU_INVALID_MEMORY_KIND `desc->memory_kind` is
 *         SPUDGPU_MEMORY_KIND_NONE or not a kind.
 * @retval SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED As for
 *         spudgpu_create_buffer().
 * @retval SPUDRESULT_GPU_HEAP_MISMATCH The heap is not a
 *         SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS heap, or `desc->memory_kind`
 *         is not the heap's.
 * @return SPUDRESULT_API_SPECIFIC_FAILURE if the graphics API's query fails.
 *
 * The desc checks are spudgpu_create_buffer()'s, in its order; the heap
 * checks come after them.
 */
SPUDRESULT spudgpu_get_buffer_placement_requirements(
    spudgpu_heap heap,
    const spudgpu_buffer_desc *desc,
    spudgpu_placement_requirements *out_requirements);

/**
 * @brief Reports what an image of `desc` needs to be placed in `heap`.
 *
 * The same `desc` and heap give the same answer for as long as the heap
 * exists. Nothing is allocated in the heap.
 *
 * @param[in]  heap             Heap the image would be placed in.
 * @param[in]  desc             The image, as spudgpu_create_placed_image()
 *                              would be given it.
 * @param[out] out_requirements Receives the size and alignment.
 *
 * @retval SPUD_SUCCESS `out_requirements` was filled in.
 * @retval SPUDRESULT_GPU_INVALID_HEAP `heap` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_IMAGE_DESC; or, checked after
 *         `out_requirements` is found not NULL, `out_requirements->base.type`
 *         is not SPUD_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL;
 *         or, checked after `out_requirements->base.type`,
 *         `out_requirements->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_requirements` is NULL.
 * @retval (each result of spudgpu_create_image()'s desc checks, in its
 *         order.)
 * @retval SPUDRESULT_GPU_INVALID_IMAGE_USAGE `desc->usage` has
 *         SPUDGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT or
 *         SPUDGPU_IMAGE_USAGE_PRESENTABLE. A transient attachment's memory
 *         is the tile's or lazily allocated and a presentable image's is the
 *         swap chain's, never a heap's.
 * @retval SPUDRESULT_GPU_HEAP_MISMATCH The heap's class is not the image's
 *         (see SPUDGPU_HEAP_RESOURCE_CLASS).
 * @return SPUDRESULT_API_SPECIFIC_FAILURE if the graphics API's query fails.
 */
SPUDRESULT spudgpu_get_image_placement_requirements(
    spudgpu_heap heap,
    const spudgpu_image_desc *desc,
    spudgpu_placement_requirements *out_requirements);

/**
 * @brief Creates a buffer in `heap`'s memory at `offset`.
 *
 * The result is a spudgpu_buffer like any other: its views, binding, copies,
 * barriers, spudgpu_get_buffer_desc() and spudgpu_destroy_buffer() are
 * unchanged, and spudgpu_get_buffer_gpu_address() gives its address. Destroying it frees
 * nothing to the device; the bytes stay the heap's, and once the buffer is
 * destroyed another resource may be placed over them. Its contents are
 * undefined until written, as any new buffer's are, whatever was in those
 * bytes before.
 *
 * Mapping keeps its contract: a placed buffer has one mapping at a time and
 * may stay mapped for as long as it exists. Several buffers of one heap may
 * be mapped at once.
 *
 * Nothing checks that the bytes are free. Two live resources placed over one
 * byte is undefined, on every backend.
 *
 * @param[in]  heap       Heap to place the buffer in.
 * @param[in]  offset     Byte offset in the heap: a multiple of the
 *                        alignment spudgpu_get_buffer_placement_requirements()
 *                        reports for `desc`.
 * @param[in]  desc       Buffer configuration. `memory_kind` is the heap's.
 * @param[out] out_buffer Receives the new buffer on success.
 *
 * @retval SPUD_SUCCESS The buffer was created.
 * @retval (each result of spudgpu_get_buffer_placement_requirements(), in
 *         its order, with `out_buffer` in place of `out_requirements`, and
 *         without the two about `out_requirements->base`.)
 * @retval SPUDRESULT_GPU_PLACEMENT_MISALIGNED `offset` is not a multiple of
 *         the reported alignment.
 * @retval SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE `offset` plus the reported
 *         size runs past the end of the heap.
 * @return Another SPUDRESULT if the backend fails.
 *
 * @see spudgpu_destroy_buffer()
 */
SPUDRESULT spudgpu_create_placed_buffer(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_buffer_desc *desc,
    spudgpu_buffer *out_buffer);

/**
 * @brief Creates an image in `heap`'s memory at `offset`.
 *
 * The result is a spudgpu_image like any other, destroyed with
 * spudgpu_destroy_image(), with a placed buffer's rules for what destroying
 * it frees and what overlapping placements are.
 *
 * An image in a SPUDGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES heap has one
 * rule more, on every backend: the first thing done with it writes all of
 * every subresource, by a rendering pass that clears each attachment over
 * the whole image (depth and stencil both, for a format with the two) or by
 * copies into all of every subresource. What anything
 * else done first reads or leaves behind is undefined. (D3D12 requires a
 * placed render target or depth/stencil resource to be initialized by a
 * clear, a full copy or a discard before any other use.)
 *
 * @param[in]  heap      Heap to place the image in.
 * @param[in]  offset    Byte offset in the heap: a multiple of the alignment
 *                       spudgpu_get_image_placement_requirements() reports
 *                       for `desc`.
 * @param[in]  desc      Image configuration.
 * @param[out] out_image Receives the new image on success.
 *
 * @retval SPUD_SUCCESS The image was created.
 * @retval (each result of spudgpu_get_image_placement_requirements(), in
 *         its order, with `out_image` in place of `out_requirements`, and
 *         without the two about `out_requirements->base`.)
 * @retval SPUDRESULT_GPU_PLACEMENT_MISALIGNED `offset` is not a multiple of
 *         the reported alignment.
 * @retval SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE `offset` plus the reported
 *         size runs past the end of the heap.
 * @return Another SPUDRESULT if the backend fails.
 *
 * @see spudgpu_destroy_image()
 */
SPUDRESULT spudgpu_create_placed_image(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_image_desc *desc,
    spudgpu_image *out_image);

// ============================================================================
//  Image Layout
//  Maps to: VkImageLayout (Vulkan)
// ============================================================================

/**
 * @brief The layout an image is in, which decides what it may be used for.
 *
 * Maps to VkImageLayout on Vulkan.
 */
typedef uint32_t SPUDGPU_IMAGE_LAYOUT;

enum {
	/**
	 * No layout: what a new image is in. As the old layout of a barrier, it
	 * says the image's contents need not be kept.
	 */
	SPUDGPU_IMAGE_LAYOUT_UNDEFINED                        = 0,
	/**
	 * What a storage image is read and written in through a storage slot.
	 * That is all it promises: one graphics API lets an image in this layout
	 * be used for anything, and another has no such state, so a caller that
	 * samples it, renders to it or copies it moves it to the layout for
	 * that first. For an image created without SPUDGPU_IMAGE_USAGE_STORAGE
	 * it is only a layout with nothing in particular allowed.
	 *
	 * A barrier whose old and new layouts are both this is not a no-op: see
	 * spudgpu_cmd_image_barrier().
	 */
	SPUDGPU_IMAGE_LAYOUT_GENERAL                          = 1,
	/** Rendered into as a color attachment. */
	SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL         = 2,
	/** Rendered into as the depth/stencil attachment. */
	SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3,
	/** Sampled by a shader. */
	SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY                 = 4,
	/** The source of a copy or blit. */
	SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC                     = 5,
	/** The destination of a copy or blit. */
	SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST                     = 6,
	/** Ready to present: what a swap chain image is handed back in. */
	SPUDGPU_IMAGE_LAYOUT_PRESENT_SRC                      = 7,
};

// ============================================================================
//  Image Barrier
//  Maps to: vkCmdPipelineBarrier (Vulkan)
// ============================================================================

/**
 * @brief Transitions an image from one layout to another, inserting the
 * pipeline barrier dependent stages need to wait correctly.
 *
 * Call before spudgpu_cmd_begin_rendering() to move a swap chain image from
 * UNDEFINED/PRESENT_SRC to COLOR_ATTACHMENT_OPTIMAL, and after
 * spudgpu_cmd_end_rendering() to move it back to PRESENT_SRC.
 *
 * Three rules hold on every backend, for this call and its two variants:
 *
 * - It moves every mip level and array layer of the image. To move some of
 *   them, use spudgpu_cmd_image_barrier_subresource(); once that has left
 *   them in different layouts, this call's `old_layout` can no longer be
 *   true of all of them, and the caller moves each range from its own.
 * - `old_layout` is the layout the image is in, or
 *   SPUDGPU_IMAGE_LAYOUT_UNDEFINED to say its contents need not be kept.
 * - A barrier from SPUDGPU_IMAGE_LAYOUT_GENERAL to
 *   SPUDGPU_IMAGE_LAYOUT_GENERAL is not a no-op. It moves the image
 *   nowhere and orders its accesses: every write a shader made to it
 *   through a storage slot before the barrier is complete before any read
 *   or write after it. It is the caller's to record between two draws or
 *   dispatches when the second reads or overwrites what the first wrote.
 *   A barrier between any other two equal layouts does nothing.
 *
 * @param[in] cmd        Command list being recorded.
 * @param[in] image      Image whose layout is changed.
 * @param[in] old_layout Layout the image is in now.
 * @param[in] new_layout Layout to transition into.
 */
void spudgpu_cmd_image_barrier(
    spudgpu_command_list cmd,
    spudgpu_image image,
    SPUDGPU_IMAGE_LAYOUT old_layout,
    SPUDGPU_IMAGE_LAYOUT new_layout);

/**
 * @brief Variant of spudgpu_cmd_image_barrier() that takes an image view.
 *
 * For swap chain images, which are not created through spudgpu_create_image()
 * and so have no spudgpu_image handle.
 *
 * @param[in] cmd        Command list being recorded.
 * @param[in] image_view View whose parent image is transitioned.
 * @param[in] old_layout Layout the image is in now.
 * @param[in] new_layout Layout to transition into.
 */
void spudgpu_cmd_image_barrier_view(
    spudgpu_command_list cmd,
    spudgpu_image_view image_view,
    SPUDGPU_IMAGE_LAYOUT old_layout,
    SPUDGPU_IMAGE_LAYOUT new_layout);

/**
 * @brief Transitions one mip/layer range of an image, leaving its other
 * subresources in the layout they were already in.
 *
 * Needed whenever subresources of one image must be in different layouts at
 * once, most notably mip chain generation with spudgpu_cmd_blit_image(), where
 * mip level N is SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC while level N+1 is
 * SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST. spudgpu_cmd_image_barrier() transitions
 * the whole image and cannot express this.
 *
 * @param[in] cmd        Command list being recorded.
 * @param[in] image      Image whose subresource range is changed.
 * @param[in] range      Mip levels and array layers to transition.
 * @param[in] old_layout Layout the range is in now.
 * @param[in] new_layout Layout to transition into.
 */
void spudgpu_cmd_image_barrier_subresource(
    spudgpu_command_list cmd,
    spudgpu_image image,
    const spudgpu_image_view_desc_subresource_range *range,
    SPUDGPU_IMAGE_LAYOUT old_layout,
    SPUDGPU_IMAGE_LAYOUT new_layout);
/**
 * @brief What a buffer or image is being used as, for
 * spudgpu_cmd_pipeline_barrier().
 */
typedef uint32_t SPUDGPU_RESOURCE_STATE;
enum {
	SPUDGPU_RESOURCE_STATE_COMMON = 0,       /**< No particular use; what a new SPUDGPU_MEMORY_KIND_DEVICE buffer is in, and what every buffer is in at the start of each submission (see spudgpu_buffer_barrier) */
	SPUDGPU_RESOURCE_STATE_VERTEX_BUFFER,    /**< Bound as a vertex or uniform buffer */
	SPUDGPU_RESOURCE_STATE_INDEX_BUFFER,     /**< Bound as an index buffer */
	SPUDGPU_RESOURCE_STATE_RENDER_TARGET,    /**< Rendered into as a color attachment */
	SPUDGPU_RESOURCE_STATE_DEPTH_WRITE,      /**< Rendered into as the depth/stencil attachment */
	SPUDGPU_RESOURCE_STATE_SHADER_RESOURCE,  /**< Read-only in shader */
	SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, /**< Read/Write (SSBO/UAV) */
	SPUDGPU_RESOURCE_STATE_PRESENT,          /**< Ready to present */
	SPUDGPU_RESOURCE_STATE_INDIRECT_ARGUMENT, /**< Read by spudgpu_cmd_draw_indirect / _indexed_indirect */
	SPUDGPU_RESOURCE_STATE_COPY_SOURCE,       /**< Read by a copy command */
	SPUDGPU_RESOURCE_STATE_COPY_DEST,         /**< Written by a copy command */
	/** Read by an acceleration structure build: its vertices, indices,
	 * AABBs or instances (SPUDGPU_EXT_RAY_QUERY). D3D12
	 * `NON_PIXEL_SHADER_RESOURCE`; Vulkan a shader read at the build stage. */
	SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT,
	/** A build's scratch, read and written by the build
	 * (SPUDGPU_EXT_RAY_QUERY). D3D12 `UNORDERED_ACCESS`; Vulkan the build
	 * stage's acceleration structure reads and writes. */
	SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH,
	/** Written by spudgpu_cmd_write_acceleration_structure_compacted_sizes()
	 * (SPUDGPU_EXT_RAY_QUERY). D3D12 `UNORDERED_ACCESS`; Vulkan a transfer
	 * write. */
	SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SIZE_DEST
};

/**
 * @brief Pipeline stage flags used in semaphore wait masks on queue submission.
 *
 * Values are bit-compatible with VkPipelineStageFlagBits and may be OR'd
 * together. One mask must be provided per wait semaphore in
 * spudgpu_submit_desc.
 */
typedef uint32_t SPUDGPU_PIPELINE_STAGE;
enum {
	SPUDGPU_PIPELINE_STAGE_TOP_OF_PIPE             = 0x00000001, /**< Before any stage: wait for nothing */
	SPUDGPU_PIPELINE_STAGE_DRAW_INDIRECT           = 0x00000002, /**< Where indirect draw arguments are read */
	SPUDGPU_PIPELINE_STAGE_VERTEX_INPUT            = 0x00000004, /**< Where vertex and index buffers are read */
	SPUDGPU_PIPELINE_STAGE_VERTEX_SHADER           = 0x00000008, /**< The vertex shader */
	SPUDGPU_PIPELINE_STAGE_TESSELLATION_CONTROL    = 0x00000010, /**< The tessellation control shader */
	SPUDGPU_PIPELINE_STAGE_TESSELLATION_EVALUATION = 0x00000020, /**< The tessellation evaluation shader */
	SPUDGPU_PIPELINE_STAGE_GEOMETRY_SHADER         = 0x00000040, /**< The geometry shader */
	SPUDGPU_PIPELINE_STAGE_FRAGMENT_SHADER         = 0x00000080, /**< The fragment shader */
	SPUDGPU_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS    = 0x00000100, /**< Depth and stencil tests before the fragment shader */
	SPUDGPU_PIPELINE_STAGE_LATE_FRAGMENT_TESTS     = 0x00000200, /**< Depth and stencil tests after the fragment shader */
	SPUDGPU_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT = 0x00000400, /**< Where color attachments are written; the stage to wait at for a swap chain image */
	SPUDGPU_PIPELINE_STAGE_COMPUTE_SHADER          = 0x00000800, /**< The compute shader */
	SPUDGPU_PIPELINE_STAGE_TRANSFER                = 0x00001000, /**< Copies and blits */
	SPUDGPU_PIPELINE_STAGE_BOTTOM_OF_PIPE          = 0x00002000, /**< After every stage */
	SPUDGPU_PIPELINE_STAGE_ALL_GRAPHICS            = 0x00008000, /**< Every graphics stage */
	SPUDGPU_PIPELINE_STAGE_ALL_COMMANDS            = 0x00010000, /**< Every stage of every command */
};

/**
 * @brief One buffer state transition for spudgpu_cmd_pipeline_barrier().
 *
 * A buffer's state lasts until the end of the submission that moved it,
 * and no further. A submission is one call of
 * spudgpu_submit_command_lists(), spudgpu_submit_command_lists_synced() or
 * spudgpu_queue_submit(): within it, a state set by one command list is the
 * state the next list in the call starts in. At the start of each
 * submission every buffer is in SPUDGPU_RESOURCE_STATE_COMMON, whatever an
 * earlier submission left it in, so the first barrier a submission records
 * for a buffer has `state_before` COMMON.
 *
 * A barrier out of COMMON is ordered after everything earlier submissions
 * on the queue did with the buffer: their writes are complete and visible,
 * and their reads are finished, before the barrier's `state_after` use
 * begins. Nothing more is needed to overwrite a buffer the last submission
 * read or wrote.
 *
 * The rule is buffers only. An image stays in the state or layout it was
 * last moved to, across submissions, until the caller moves it again.
 *
 * Maps to: D3D12's buffer state decay, at the end of each
 * ExecuteCommandLists; on Vulkan, a barrier out of COMMON waits on every
 * earlier command and makes every earlier write available. Metal records no
 * barriers.
 */
typedef struct spudgpu_buffer_barrier {
	/** `type` is SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/**
	 * The buffer to move. Never one of SPUDGPU_MEMORY_KIND_UPLOAD or
	 * SPUDGPU_MEMORY_KIND_READBACK, which stay in the state they are
	 * created in.
	 */
	spudgpu_buffer buffer;
	/** The state the buffer is in when the barrier executes: COMMON for the
	 * first barrier of a buffer in a submission (see above). */
	SPUDGPU_RESOURCE_STATE state_before;
	/** The state the buffer is in after it. */
	SPUDGPU_RESOURCE_STATE state_after;
} spudgpu_buffer_barrier;

/**
 * @brief One image state transition for spudgpu_cmd_pipeline_barrier().
 *
 * The whole image is moved: there is no range of mip levels or array layers
 * here. spudgpu_cmd_image_barrier_subresource() moves a range between
 * layouts.
 */
typedef struct spudgpu_image_barrier {
	/** `type` is SPUD_STRUCT_TYPE_GPU_IMAGE_BARRIER; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** The image to move. */
	spudgpu_image image;
	/** The state the image is in when the barrier executes. */
	SPUDGPU_RESOURCE_STATE state_before;
	/** The state the image is in after it. */
	SPUDGPU_RESOURCE_STATE state_after;
} spudgpu_image_barrier;

/**
 * @brief Records an execution and memory barrier moving buffers and images
 * between resource states.
 *
 * Three rules hold on every backend, whatever the graphics API underneath
 * does with a given pair of states:
 *
 * - An entry whose `state_before` and `state_after` are both
 *   SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS is not a no-op. It moves the
 *   resource nowhere, and orders its accesses: every write made through a
 *   storage slot before the barrier is complete before any read or write
 *   after it.
 * - That barrier is the caller's to record between two draws or dispatches
 *   when the second reads or overwrites what the first wrote to the resource
 *   through a storage slot. Without it, what the second sees is undefined.
 *   It is never recorded on the caller's behalf: two writes that don't depend
 *   on each other need none, and only the caller knows which they are.
 * - A buffer entry whose `state_before` and `state_after` are both
 *   SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH (SPUDGPU_EXT_RAY_QUERY)
 *   is not a no-op either: it orders two builds that share a scratch range,
 *   every write of the build before it complete before any read or write of
 *   the build after it (card S25). Two builds in one
 *   spudgpu_cmd_build_acceleration_structures() call never share a range
 *   (the caller's to keep apart; nothing checks it); two calls that do need
 *   this barrier between them.
 * - An entry whose two states are equal and are anything else does nothing.
 *
 * Maps to: vkCmdPipelineBarrier (Vulkan); ID3D12GraphicsCommandList::
 * ResourceBarrier, with a transition barrier between two states and a UAV
 * barrier between any two states that are both D3D12's UNORDERED_ACCESS
 * (UNORDERED_ACCESS, ACCELERATION_STRUCTURE_SCRATCH and
 * ACCELERATION_STRUCTURE_SIZE_DEST, equal or not), which D3D12 has no
 * transition between (D3D12). Records nothing on Metal, which orders access
 * to a resource itself.
 *
 * Nothing is recorded if an element of `buffer_barriers` has a `base` other than {SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, NULL} or an element of `image_barriers` has a `base` other than {SPUD_STRUCT_TYPE_GPU_IMAGE_BARRIER, NULL}
 * (see SPUD_STRUCT_BASE).
 *
 * @param[in] cmd                  Command list being recorded.
 * @param[in] buffer_barriers      Array of buffer state transitions. May be
 *                                 NULL when `buffer_barrier_count` is 0.
 * @param[in] buffer_barrier_count Number of elements in `buffer_barriers`.
 * @param[in] image_barriers       Array of image state transitions. May be
 *                                 NULL when `image_barrier_count` is 0.
 * @param[in] image_barrier_count  Number of elements in `image_barriers`.
 */
void spudgpu_cmd_pipeline_barrier(
    spudgpu_command_list cmd,
    const spudgpu_buffer_barrier *buffer_barriers,
    uint32_t buffer_barrier_count,
    const spudgpu_image_barrier *image_barriers,
    uint32_t image_barrier_count);

/**
 * @brief Creates a surface for a native window, for a swap chain to present
 * to.
 *
 * @param[in]  instance       Instance to create the surface on.
 * @param[in]  window_handle  The platform's native window object:
 *                            - Windows (Win32): `HWND`.
 *                            - macOS (AppKit): `NSWindow*` or `CAMetalLayer*`.
 *                            - Linux (X11 / Wayland): `Window` or
 *                              `wl_surface*`.
 * @param[in]  display_handle The platform's display connection:
 *                            - Linux Wayland: `wl_display*`.
 *                            - Linux X11: `Display*`.
 *                            - Windows: unused, pass NULL.
 * @param[out] out_surface    Receives the new surface on success.
 *
 * @retval SPUD_SUCCESS The surface was created.
 * @retval SPUDRESULT_GPU_INVALID_INSTANCE `instance` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_WINDOW_HANDLE `window_handle` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_DISPLAY_HANDLE `display_handle` is NULL where
 *         the platform needs one (Vulkan, other than on Windows).
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_surface` is NULL.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_surface()
 */
SPUDRESULT spudgpu_create_surface(
    spudgpu_instance instance,
    void *window_handle,
    void *display_handle,
    spudgpu_surface *out_surface);

/**
 * @brief Destroys a surface.
 *
 * @warning Destroy any swap chain created on the surface first.
 *
 * @param[in] surface Surface to destroy.
 */
void spudgpu_destroy_surface(spudgpu_surface surface);

/**
 * @brief Callback that creates the native surface for
 * spudgpu_create_surface_from_callback().
 *
 * The native types are passed as `void *` to keep this header free of Vulkan
 * types.
 *
 * @param[in]  vk_instance The instance's `VkInstance`.
 * @param[in]  user_data   The `user_data` given to
 *                         spudgpu_create_surface_from_callback().
 * @param[out] out_surface A `VkSurfaceKHR *` that receives the created
 *                         surface.
 *
 * @return true if the surface was created and written to `out_surface`,
 *         false otherwise.
 */
typedef bool (*spudgpu_surface_create_fn)(
    void *vk_instance,
    void *user_data,
    void *out_surface);

/**
 * @brief Creates a surface by having the caller make the native surface
 * object.
 *
 * For windowing libraries that create the native surface themselves: `create_fn`
 * is called once, during this call, with the instance's native handle.
 *
 * @note Not implemented by the D3D12 backend, where it returns NULL.
 *
 * @param[in] instance  Instance to create the surface on.
 * @param[in] user_data Passed through to `create_fn` unchanged.
 * @param[in] create_fn Callback that creates the native surface.
 *
 * @return The new surface, or NULL if `instance` or `create_fn` is NULL, the
 *         surface could not be allocated, or `create_fn` returned false.
 */
spudgpu_surface spudgpu_create_surface_from_callback(
    spudgpu_instance instance,
    void *user_data,
    spudgpu_surface_create_fn create_fn);

/**
 * @brief Dictates the sync relationship between the GPU frame completion and
 * the monitor's refresh cycle.
 *
 * Governs whether presentation waits for the display to prevent tearing or
 * uses extra back buffers to reduce input latency.
 *
 * @see SPUDGPU_PRESENT_MODE_Constants
 */
typedef uint32_t SPUDGPU_PRESENT_MODE;

/**
 * @name Presentation Sync Modes
 * @anchor SPUDGPU_PRESENT_MODE_Constants
 *
 * Configurations mapped to underlying driver swap rules (e.g.,
 * VkPresentModeKHR or NSOpenGLContext).
 */
/** @{ */
enum {
	/**
	 * @brief Immediate Mode (V-Sync Disabled).
	 *
	 * The GPU transfers completed frames to the screen instantly. Offers the
	 * lowest possible input latency but results in visible screen tearing as
	 * the monitor splits images mid-refresh.
	 */
	SPUDGPU_PRESENT_MODE_IMMEDIATE = 0,

	/**
	 * @brief FIFO Mode (V-Sync Enabled - First In, First Out).
	 *
	 * Frames are queued up and synchronized strictly with the display's
	 * vertical refresh rate (e.g., 60Hz/144Hz). Completely eliminates screen
	 * tearing, but will throttle the CPU/GPU thread if the queue fills up.
	 */
	SPUDGPU_PRESENT_MODE_FIFO = 1,

	/**
	 * @brief Mailbox Mode (Triple Buffering / Ultra-Low Latency V-Sync).
	 *
	 * Synchronizes with the vertical refresh rate to eliminate tearing, but
	 * does not block the application when the queue is full. Instead, the
	 * newest completed frame continuously replaces unrendered frames in the
	 * queue, ensuring the monitor always pulls the absolute freshest data.
	 */
	SPUDGPU_PRESENT_MODE_MAILBOX = 2
};

/**
 * @brief How a swap chain's window occupies the display.
 */
typedef uint8_t SPUDGPU_FULLSCREEN_MODE;
enum {
	/** An ordinary window. */
	SPUDGPU_FULLSCREEN_MODE_WINDOWED   = 0,
	/** Exclusive fullscreen: the swap chain owns the display. */
	SPUDGPU_FULLSCREEN_MODE_FULLSCREEN = 1,
	/** A borderless window covering the display. */
	SPUDGPU_FULLSCREEN_MODE_BORDERLESS = 2,
};

/** @} */

/**
 * @brief How the compositor shows a back buffer whose size isn't the
 * surface's.
 *
 * A swap chain names exactly one (spudgpu_swap_chain_desc::present_scaling),
 * and there is no default. Each value is the narrowest rule all three graphics
 * APIs keep, and holds whenever the back buffer and the surface differ in
 * size, as during a resize before the caller has made a new swap chain at the
 * new size. When they match, all three show the back buffer as it is.
 *
 * Neither the filter the compositor scales with nor the color of what the
 * back buffer leaves uncovered is promised: neither can be chosen on every
 * backend. A caller that needs either does its own scaling with a draw into
 * back buffers of the surface's size and names ONE_TO_ONE.
 *
 * Which values a surface takes is per surface:
 * spudgpu_get_surface_present_scalings(). A pixel of the surface is the
 * window's own: on Metal, the layer's `contentsScale` says how many of the
 * display's it covers.
 *
 * @see SPUDGPU_PRESENT_SCALING_Constants
 */
typedef uint32_t SPUDGPU_PRESENT_SCALING;

/**
 * @name Present Scalings
 * @anchor SPUDGPU_PRESENT_SCALING_Constants
 *
 * One bit each, so that spudgpu_get_surface_present_scalings() can report a
 * set; a swap chain names exactly one.
 *
 * | Value      | D3D12 (flip model, HWND) | Vulkan (`VK_KHR_swapchain_maintenance1`) | Metal (`CAMetalLayer`) |
 * |------------|--------------------------|------------------------------------------|------------------------|
 * | ONE_TO_ONE | `DXGI_SCALING_NONE`      | `ONE_TO_ONE`, gravity `MIN` on X and Y, or no scaling with the surface's size | `contentsGravity` top left |
 * | STRETCH    | `DXGI_SCALING_STRETCH`   | `STRETCH`                                | `kCAGravityResize` |
 * | ASPECT_FIT | not offered              | `ASPECT_RATIO_STRETCH`, gravity `CENTERED`; not offered on Win32 | `kCAGravityResizeAspect` |
 */
/** @{ */
enum {
	/** Refused with SPUDRESULT_GPU_INVALID_PRESENT_SCALING: a zeroed desc
	 *  has not chosen. */
	SPUDGPU_PRESENT_SCALING_NONE       = 0,
	/** Pixel for pixel, the back buffer's top-left pixel at the surface's
	 *  top-left. What of the surface the back buffer doesn't cover, and what
	 *  of the back buffer falls outside the surface, is unspecified. */
	SPUDGPU_PRESENT_SCALING_ONE_TO_ONE = 1u << 0,
	/** Scaled to fill the surface, each axis on its own: the aspect ratio is
	 *  not kept. */
	SPUDGPU_PRESENT_SCALING_STRETCH    = 1u << 1,
	/** Scaled to fit inside the surface with the aspect ratio kept, and
	 *  centred. The bars beside or above and below it are unspecified. */
	SPUDGPU_PRESENT_SCALING_ASPECT_FIT = 1u << 2,
};
/** @} */

/**
 * @brief Configuration descriptor used to initialize a rendering surface target
 * linked to the OS windowing system.
 *
 * Handled by the driver to allocate a ring buffer of textures (back buffers)
 * that flip onto the screen.
 */
typedef struct spudgpu_swap_chain_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** The window surface the swap chain presents to. */
	spudgpu_surface surface;

	/**
	 * @brief The queue this swap chain's presentation is ordered against.
	 *
	 * Required on every backend, not just a hint: D3D12 must bind the
	 * DXGI swap chain to a specific ID3D12CommandQueue at creation time and
	 * cannot rebind it later, so this is where that queue comes from instead
	 * of a backend silently assuming "the graphics queue." Render all work
	 * destined for this swap chain's images on this same queue before
	 * calling spudgpu_swap_chain_present - presentation is ordered relative
	 * to it, not to whichever queue you happen to submit on.
	 */
	spudgpu_command_queue queue;

	/**
	 * Requested back-buffer width in pixels. Usually matches the window client
	 * area width.
	 */
	uint32_t width;

	/**
	 * Requested back-buffer height in pixels. Usually matches the window
	 * client area height.
	 */
	uint32_t height;

	/**
	 * @brief Total number of images in the swap chain ring buffer.
	 *
	 * Typically configured as `2` for simple Double Buffering or `3` for
	 * Mailbox Triple Buffering layouts.
	 */
	uint32_t buffer_count;

	/**
	 * The color and pixel format schema requested for the surface (e.g.,
	 * BGRA8_UNORM or RGBA16_SFLOAT).
	 */
	SPUDGPU_FORMAT format;

	/**
	 * The sync and presentation timing rule to apply when presenting completed
	 * frames.
	 * @see SPUDGPU_PRESENT_MODE
	 */
	SPUDGPU_PRESENT_MODE present_mode;

	/**
	 * @brief How the compositor shows a back buffer of another size than the
	 * surface: exactly one SPUDGPU_PRESENT_SCALING, and one the surface takes
	 * (spudgpu_get_surface_present_scalings()). Required: NONE is refused.
	 * @see SPUDGPU_PRESENT_SCALING
	 */
	SPUDGPU_PRESENT_SCALING present_scaling;

	/**
	 * @brief Toggles whether the graphics device initializes in exclusive
	 * fullscreen monitor mode or stays bounded inside a standard desktop window
	 * framework.
	 */
	SPUDGPU_FULLSCREEN_MODE fullscreen_mode;

	/**
	 * @brief Usages the back buffers need beyond being rendered into.
	 *
	 * Back buffers are always usable as a color attachment; this adds to
	 * that, e.g. SPUDGPU_IMAGE_USAGE_TRANSFER_DST to copy/blit a finished
	 * image into them with spudgpu_cmd_blit_image. Leave 0 for none.
	 * spudgpu_create_swap_chain fails if the surface can't support a
	 * requested usage.
	 * @see SPUDGPU_IMAGE_USAGE
	 */
	SPUDGPU_IMAGE_USAGE usage;
} spudgpu_swap_chain_desc;

/**
 * @brief Reports which present scalings a swap chain on a surface may name.
 *
 * The answer is per surface and per device, and is the same for every
 * present mode: a scaling it reports is taken by spudgpu_create_swap_chain()
 * whatever `present_mode` the desc names. Ask again for a surface made again.
 *
 * - D3D12: ONE_TO_ONE and STRETCH. ASPECT_FIT is never offered:
 *   `DXGI_SCALING_ASPECT_RATIO_STRETCH` acts as a stretch on a window's swap
 *   chain. ONE_TO_ONE is not offered for a window with `WS_EX_LAYOUTRTL`,
 *   where `DXGI_SCALING_NONE` aligns the right edges; the window's style is
 *   read at this call and at spudgpu_create_swap_chain().
 * - Vulkan: ONE_TO_ONE always. Where the device has
 *   `VK_KHR_swapchain_maintenance1`, also what the surface's
 *   `VkSurfacePresentScalingCapabilitiesKHR` offers for every present mode
 *   the surface has (STRETCH; ASPECT_FIT with centred gravity on both axes,
 *   but never on a Win32 surface, where a driver offering it was seen to
 *   stretch, as DXGI does). Without it, ONE_TO_ONE alone: the back buffers are then the surface's own
 *   size where the surface fixes one, and nothing is ever scaled.
 * - Metal: all three.
 *
 * @param[in]  device       Device a swap chain would be created on.
 * @param[in]  surface      Surface it would present to.
 * @param[out] out_scalings Receives the SPUDGPU_PRESENT_SCALING bits the
 *                          surface takes; never 0 on success.
 *
 * @retval SPUD_SUCCESS `out_scalings` was written.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_SURFACE `surface` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_scalings` is NULL.
 * @retval SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED The device was created without
 *         SPUDGPU_FEATURE_PRESENT.
 * @return SPUDRESULT_API_SPECIFIC_FAILURE if the graphics API fails, or
 *         SPUDRESULT_OUT_OF_MEMORY.
 */
SPUDRESULT spudgpu_get_surface_present_scalings(
    spudgpu_device device,
    spudgpu_surface surface,
    uint32_t *out_scalings);

/**
 * @brief Creates a swap chain for rendering to a surface.
 *
 * @param[in]  device         Device to create the swap chain on.
 * @param[in]  desc           Swap chain configuration.
 * @param[out] out_swap_chain Receives the new swap chain on success.
 *
 * @retval SPUD_SUCCESS The swap chain was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_swap_chain` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_SURFACE `desc->surface` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_QUEUE `desc->queue` is NULL.
 * @retval SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED The device was created without
 *         SPUDGPU_FEATURE_PRESENT.
 * @retval SPUDRESULT_GPU_INVALID_PRESENT_SCALING `desc->present_scaling` is
 *         SPUDGPU_PRESENT_SCALING_NONE or not exactly one
 *         SPUDGPU_PRESENT_SCALING. Checked before the surface is read.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS The backend cannot create a swap
 *         chain with this configuration (Metal: `desc->buffer_count` other than
 *         1, or SPUDGPU_PRESENT_MODE_MAILBOX).
 * @retval SPUDRESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED The surface doesn't
 *         take `desc->present_scaling`: it is not among what
 *         spudgpu_get_surface_present_scalings() reports.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_swap_chain()
 */
SPUDRESULT spudgpu_create_swap_chain(
    spudgpu_device device,
    const spudgpu_swap_chain_desc *desc,
    spudgpu_swap_chain *out_swap_chain);

/**
 * @brief Destroys a swap chain with its back buffer images, views,
 * semaphores and fences.
 *
 * @param[in] swap_chain Swap chain to destroy.
 */
void spudgpu_destroy_swap_chain(spudgpu_swap_chain swap_chain);

/**
 * @brief Reads a swap chain's configuration as actually created.
 *
 * This can differ from what was requested: `width`/`height` are the real
 * back-buffer size (on Vulkan the surface can dictate it, e.g. a window still
 * 1x1 before layout: its size where a ONE_TO_ONE swap chain has no
 * `VK_KHR_swapchain_maintenance1` scaling, otherwise the request clamped to
 * the range the surface takes; on Metal the size of the drawable last
 * acquired), and `buffer_count` the real image count where a backend
 * reports it.
 *
 * @param[in]  swap_chain Swap chain to query.
 * @param[out] out_desc   Receives the swap chain's descriptor.
 *
 * @retval SPUD_SUCCESS `out_desc` was filled in.
 * @retval SPUDRESULT_GPU_INVALID_SWAP_CHAIN `swap_chain` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_desc` is NULL.
 */
SPUDRESULT spudgpu_get_swap_chain_desc(
    spudgpu_swap_chain swap_chain,
    spudgpu_swap_chain_desc *out_desc);

/**
 * @brief Acquires the next back buffer image to render into.
 *
 * Blocks while the GPU is still working on the frame slot being reused.
 *
 * @param[in] swap_chain Swap chain to acquire from.
 *
 * @return The index of the acquired back buffer, for
 *         spudgpu_get_swap_chain_image_view(), or SPUD_UINT32_MAX if
 *         `swap_chain` is NULL or no image could be acquired.
 */
uint32_t spudgpu_swap_chain_acquire_next_image(spudgpu_swap_chain swap_chain);

/**
 * @brief Presents the current back buffer to the screen.
 *
 * Call after recording and submitting the frame's commands to the queue given
 * as spudgpu_swap_chain_desc::queue. Presentation is ordered against that
 * queue's work, not against whichever queue was submitted on.
 *
 * A use of that queue like a submit, under the same rule (see
 * spudgpu_submit_command_lists()). The swap chain itself is used by one
 * caller at a time, as every object is.
 *
 * @param[in] swap_chain Swap chain holding the image to display.
 */
void spudgpu_swap_chain_present(spudgpu_swap_chain swap_chain);

/**
 * @brief Returns the image view of one of a swap chain's back buffers.
 *
 * The swap chain owns the view; do not destroy it.
 *
 * @param[in] swap_chain  Swap chain to get the view from.
 * @param[in] image_index Back buffer index, as returned by
 *                        spudgpu_swap_chain_acquire_next_image().
 *
 * @return The image view, or NULL if `swap_chain` is NULL or `image_index` is
 *         not one of its back buffers.
 */
spudgpu_image_view spudgpu_get_swap_chain_image_view(
    spudgpu_swap_chain swap_chain,
    uint32_t image_index);

/**
 * @brief Returns the image-available semaphore of the swap chain's current
 * frame slot.
 *
 * It is signaled when the acquired image is ready to render into. Pass it as a
 * wait semaphore in spudgpu_submit_desc. The swap chain owns it; do not destroy
 * it.
 *
 * @note Not implemented by the D3D12 backend, where it returns NULL.
 *
 * @param[in] swap_chain Swap chain to get the semaphore from.
 *
 * @return The semaphore, or NULL if `swap_chain` is NULL.
 */
spudgpu_semaphore spudgpu_swap_chain_get_image_available_semaphore(spudgpu_swap_chain swap_chain);

/**
 * @brief Returns the render-finished semaphore of the swap chain's current
 * frame slot.
 *
 * Pass it as a signal semaphore in spudgpu_submit_desc so presentation waits
 * for rendering. The swap chain owns it; do not destroy it.
 *
 * @note Not implemented by the D3D12 backend, where it returns NULL.
 *
 * @param[in] swap_chain Swap chain to get the semaphore from.
 *
 * @return The semaphore, or NULL if `swap_chain` is NULL.
 */
spudgpu_semaphore spudgpu_swap_chain_get_render_finished_semaphore(spudgpu_swap_chain swap_chain);

// ============================================================================
//  Descriptor Set Layout
//  Maps to: VkDescriptorSetLayout (Vulkan) / MTLArgumentEncoder schema (Metal)
// ============================================================================


/**
 * @brief Enumerates the kinds of resources a shader binding slot can hold.
 *
 * Maps directly to VkDescriptorType on Vulkan and the corresponding
 * MTLArgumentEncoder argument kinds on Metal.
 */
typedef uint32_t SPUDGPU_DESCRIPTOR_TYPE;

enum {
	/** Read-only structured constant block (UBO / constant buffer). */
	SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER = 0,

	/** Read/write large data array (SSBO / structured buffer). */
	SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER = 1,

	/** Combined image + sampler in a single binding (texture2D + sampler). */
	SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 2,

	/** Sampled image without a sampler (use alongside SAMPLER binding). */
	SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE = 3,

	/** Standalone sampler object. */
	SPUDGPU_DESCRIPTOR_TYPE_SAMPLER = 4,

	/** Image the shader can both read and write (compute UAV / storage image). */
	SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE = 5,

	/** A top level acceleration structure, read by ray queries
	 * (SPUDGPU_EXT_RAY_QUERY). D3D12 an SRV (`t<b>`), Metal an argument
	 * buffer member, Vulkan `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`.
	 * A layout or pool naming it on a device created without
	 * SPUDGPU_FEATURE_RAY_QUERY is refused with
	 * SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED (card S23). */
	SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE = 6,
};

/**
 * @brief Bitmask of shader stages.
 */
typedef uint32_t SPUDGPU_SHADER_STAGE;

enum {
	/** No stage. */
	SPUDGPU_SHADER_STAGE_NONE                    = 0,
	/** Vertex shader. */
	SPUDGPU_SHADER_STAGE_VERTEX                  = 1 << 0,
	/** Fragment (pixel) shader. */
	SPUDGPU_SHADER_STAGE_FRAGMENT                = 1 << 1,
	/** Compute shader. */
	SPUDGPU_SHADER_STAGE_COMPUTE                 = 1 << 2,
	/** Geometry shader. */
	SPUDGPU_SHADER_STAGE_GEOMETRY                = 1 << 3,
	/** Tessellation control (hull) shader. */
	SPUDGPU_SHADER_STAGE_TESSELLATION_CONTROL    = 1 << 4,
	/** Tessellation evaluation (domain) shader. */
	SPUDGPU_SHADER_STAGE_TESSELLATION_EVALUATION = 1 << 5,

	/** Mesh shader. @see SPUDGPU_EXT_MESH_SHADING */
	SPUDGPU_SHADER_STAGE_MESH                    = 1 << 6,
	/**
	 * Amplification/task shader, upstream of a mesh shader. Not used by any
	 * sample yet — declared alongside MESH for completeness.
	 * @see SPUDGPU_EXT_MESH_SHADING
	 */
	SPUDGPU_SHADER_STAGE_TASK                    = 1 << 7
};

/**
 * @brief Describes a single binding slot within a descriptor set layout.
 *
 * Each entry corresponds to one `layout(set=N, binding=M)` declaration in GLSL.
 */
/* Frozen: an array element, held by value in spudgpu_descriptor_set_layout_desc. It never changes; a new need gets a new type. */
typedef struct spudgpu_descriptor_binding_desc {
	/**
	 * Slot index matching `layout(binding = N)` in GLSL. Must be unique within
	 * a layout.
	 */
	uint32_t binding;

	/**
	 * Resource type expected at this binding slot.
	 * @see SPUDGPU_DESCRIPTOR_TYPE
	 */
	SPUDGPU_DESCRIPTOR_TYPE descriptor_type;

	/**
	 * Number of resources in this binding. Use 1 for a single resource,
	 * or N for a fixed-size array (e.g. `uniform sampler2D textures[8]`).
	 */
	uint32_t count;

	/**
	 * Bitmask of shader stages that can access this binding.
	 * @see SPUDGPU_SHADER_STAGE
	 */
	SPUDGPU_SHADER_STAGE stage_flags;
} spudgpu_descriptor_binding_desc;

/**
 * @brief Capacity of spudgpu_descriptor_set_layout_desc::bindings.
 */
#define SPUDGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET 16

/**
 * @brief Configuration descriptor for a descriptor set layout.
 *
 * Describes the binding slots that make up one set. Pass this to
 * spudgpu_create_descriptor_set_layout(), then hand the resulting
 * handle into spudgpu_shader_pipeline_desc::descriptor_set_layouts[].
 */
typedef struct spudgpu_descriptor_set_layout_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** @brief A name for diagnostics, or NULL. Copied, in every build; see
	 * spud_debug_name_set(). */
	const char *debug_name;

	/** The set's binding slots. Only the first #binding_count are read. */
	spudgpu_descriptor_binding_desc bindings[SPUDGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET];
	/** Number of valid entries in #bindings. */
	uint32_t binding_count;
} spudgpu_descriptor_set_layout_desc;

/**
 * @brief Creates a descriptor set layout: the fixed list of binding slots a
 * descriptor set exposes to shaders.
 *
 * The layout cannot be changed after creation. Pass it to a pipeline desc to
 * declare the binding shape the pipeline expects. The caller owns the returned
 * layout and releases it with spudgpu_destroy_descriptor_set_layout().
 *
 * Backend mapping:
 * - Vulkan: creates a VkDescriptorSetLayout.
 * - Metal: creates an MTLArgumentEncoder describing the bindings.
 * - D3D12: creates no native object; records each binding's offset into the
 *   set's CBV/SRV/UAV and sampler heap ranges.
 *
 * @note Immutable samplers are not supported. On Metal every texture binding
 * is declared as a 2D texture.
 *
 * @param[in]  device     Device the layout is created on.
 * @param[in]  desc       Binding slots to declare. The struct and its
 *                        `debug_name` string are copied; neither needs to
 *                        outlive the call.
 * @param[out] out_layout Receives the new layout on success. Not written on
 *                        failure.
 *
 * @retval SPUD_SUCCESS The layout was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_layout` is NULL.
 * @retval SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_BINDINGS `desc->binding_count`
 *         exceeds SPUDGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET.
 * @retval SPUDRESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE A
 *         binding's `descriptor_type` is not a SPUDGPU_DESCRIPTOR_TYPE
 *         (every backend).
 * @retval SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED A binding's
 *         `descriptor_type` is SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE
 *         and the device wasn't created with SPUDGPU_FEATURE_RAY_QUERY. The
 *         bindings are checked in order, each for both.
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE The native API failed to create the
 *         object (Vulkan and Metal only).
 *
 * @see spudgpu_destroy_descriptor_set_layout()
 */
SPUDRESULT spudgpu_create_descriptor_set_layout(
    spudgpu_device device,
    const spudgpu_descriptor_set_layout_desc *desc,
    spudgpu_descriptor_set_layout *out_layout);

/**
 * @brief Destroys a descriptor set layout.
 *
 * @warning All descriptor sets allocated from this layout, and all pipelines
 * referencing it, must be destroyed before calling this.
 *
 * @param[in] layout Layout to destroy.
 */
void spudgpu_destroy_descriptor_set_layout(spudgpu_descriptor_set_layout layout);
// ============================================================================
//  Descriptor Pool
//  Maps to: VkDescriptorPool (Vulkan) / heap of argument buffers (Metal)
// ============================================================================

/**
 * @brief Declares how many descriptors of each type a pool should pre-allocate.
 *
 * Vulkan requires knowing the total capacity up front. Size your pool to cover
 * the worst-case count across all frames-in-flight.
 */
/* Frozen: an array element, held by value in spudgpu_descriptor_pool_desc. It never changes; a new need gets a new type. */
typedef struct spudgpu_descriptor_pool_size {
	/** The type of descriptor counted. */
	SPUDGPU_DESCRIPTOR_TYPE descriptor_type;
	/** How many descriptors of that type the pool holds, across all its sets. */
	uint32_t count;
} spudgpu_descriptor_pool_size;

/**
 * @brief Capacity of spudgpu_descriptor_pool_desc::pool_sizes.
 */
#define SPUDGPU_MAX_DESCRIPTOR_POOL_SIZES 8

/**
 * @brief Configuration descriptor for a descriptor pool.
 */
typedef struct spudgpu_descriptor_pool_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** @brief A name for diagnostics, or NULL. Copied, in every build; see
	 * spud_debug_name_set(). */
	const char *debug_name;

	/** Maximum number of descriptor sets that can be allocated from this pool. */
	uint32_t max_sets;

	/** Capacity per descriptor type. Only the first #pool_size_count are read. */
	spudgpu_descriptor_pool_size pool_sizes[SPUDGPU_MAX_DESCRIPTOR_POOL_SIZES];
	/** Number of valid entries in #pool_sizes. */
	uint32_t pool_size_count;
} spudgpu_descriptor_pool_desc;

/**
 * @brief Creates a descriptor pool, the backing memory for descriptor sets.
 *
 * Create one pool per frame-in-flight (or one large shared pool) and reset it
 * each frame rather than allocating and freeing individual sets every frame.
 *
 * @param[in]  device   Device to create the pool on.
 * @param[in]  desc     Pool capacity.
 * @param[out] out_pool Receives the new pool on success.
 *
 * @retval SPUD_SUCCESS The pool was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_pool` is NULL.
 * @retval SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_POOLS `desc->pool_size_count`
 *         exceeds SPUDGPU_MAX_DESCRIPTOR_POOL_SIZES.
 * @retval SPUDRESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE A pool
 *         size's `descriptor_type` is not a SPUDGPU_DESCRIPTOR_TYPE (every
 *         backend).
 * @retval SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED A pool size's
 *         `descriptor_type` is SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE
 *         and the device wasn't created with SPUDGPU_FEATURE_RAY_QUERY. The
 *         pool sizes are checked in order, each for both.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_descriptor_pool()
 */
SPUDRESULT spudgpu_create_descriptor_pool(
    spudgpu_device device,
    const spudgpu_descriptor_pool_desc *desc,
    spudgpu_descriptor_pool *out_pool);

/**
 * @brief Resets a pool, freeing every set allocated from it at once.
 *
 * Cheaper than freeing sets individually. Call once per frame before writing
 * new descriptors.
 *
 * @param[in] pool Pool to reset.
 */
void spudgpu_reset_descriptor_pool(spudgpu_descriptor_pool pool);

/**
 * @brief Destroys a descriptor pool and all sets allocated from it.
 *
 * @warning Command lists using sets from this pool must have finished
 * executing before calling this.
 *
 * @param[in] pool Pool to destroy.
 */
void spudgpu_destroy_descriptor_pool(spudgpu_descriptor_pool pool);
// ============================================================================
//  Descriptor Set
//  Maps to: VkDescriptorSet (Vulkan) / MTLBuffer argument buffer (Metal)
// ============================================================================

/**
 * @brief Most descriptor set layouts a pipeline can declare, and most sets one
 * spudgpu_create_descriptor_sets() call can allocate.
 */
#define SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS 4

/**
 * @brief Allocation descriptor for one or more descriptor sets.
 *
 * All sets in a single call are allocated from the same pool in one
 * driver round-trip (matches vkAllocateDescriptorSets semantics).
 */
typedef struct spudgpu_descriptor_set_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** The pool the sets are allocated from, and released by. */
	spudgpu_descriptor_pool pool;

	/** Each element describes the layout for one set being allocated. */
	spudgpu_descriptor_set_layout set_layouts[SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	/** Number of sets to allocate: the valid entries in #set_layouts. */
	uint32_t set_count;
} spudgpu_descriptor_set_desc;

/**
 * @brief Allocates descriptor sets from a pool.
 *
 * The sets are released by their pool, when it is reset or destroyed.
 *
 * @param[in]  device   Device that owns the pool.
 * @param[in]  desc     Pool to allocate from and the layout of each set.
 * @param[out] out_sets Caller-supplied array of at least `desc->set_count`
 *                      elements that receives the set handles.
 *
 * @retval SPUD_SUCCESS The sets were allocated.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_sets` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_DESCRIPTOR_POOL `desc->pool` is NULL.
 * @retval SPUDRESULT_GPU_ZERO_DESCRIPTOR_SET_LAYOUTS `desc->set_count` is 0.
 * @retval SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_SET_LAYOUTS `desc->set_count`
 *         exceeds SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS.
 * @retval SPUDRESULT_GPU_INVALID_DESCRIPTOR_SET_LAYOUT An entry of
 *         `desc->set_layouts` is NULL.
 * @retval SPUDRESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL The sets
 *         could not be allocated, e.g. the pool is out of capacity.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 */
SPUDRESULT spudgpu_create_descriptor_sets(
    spudgpu_device device,
    const spudgpu_descriptor_set_desc *desc,
    spudgpu_descriptor_set *out_sets);

// ============================================================================
//  Sampler
//  Maps to: VkSampler (Vulkan) / a D3D12_SAMPLER_DESC written into a sampler
//  descriptor heap slot at write time, not a standalone device object
//  (D3D12) / id<MTLSamplerState> (Metal).
// ============================================================================

/**
 * @brief Opaque handle to a texture sampler.
 */
typedef struct spudgpu_sampler_t *spudgpu_sampler;

/**
 * @brief Sampling filter used when a blit's source and destination regions
 * differ in size, or (see spudgpu_sampler_desc below) when a shader samples
 * a texture between texels/mip levels.
 */
typedef uint32_t SPUDGPU_FILTER;
enum {
	/** The nearest texel, or mip level. */
	SPUDGPU_FILTER_NEAREST = 0,
	/** A linear blend of the neighbouring texels, or mip levels. */
	SPUDGPU_FILTER_LINEAR = 1
};

/**
 * @brief How a sampler handles texture coordinates outside [0, 1].
 */
typedef uint32_t SPUDGPU_ADDRESS_MODE;
enum {
	/** The texture repeats. */
	SPUDGPU_ADDRESS_MODE_REPEAT          = 0,
	/** The texture repeats, mirrored every other time. */
	SPUDGPU_ADDRESS_MODE_MIRRORED_REPEAT = 1,
	/** The edge texel extends outwards. */
	SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE   = 2,
	/** A border color is read outside the texture. */
	SPUDGPU_ADDRESS_MODE_CLAMP_TO_BORDER = 3,
};

/**
 * @brief Comparison function, used by the depth test and by a comparison
 * sampler (spudgpu_sampler_desc::compare_enable).
 *
 * Each compares the incoming value against the stored one and passes when
 * the comparison holds.
 */
typedef uint32_t SPUDGPU_COMPARE_OP;

enum {
	SPUDGPU_COMPARE_OP_NEVER            = 0, /**< Never passes */
	SPUDGPU_COMPARE_OP_LESS             = 1, /**< Passes when incoming < stored */
	SPUDGPU_COMPARE_OP_EQUAL            = 2, /**< Passes when incoming == stored */
	SPUDGPU_COMPARE_OP_LESS_OR_EQUAL    = 3, /**< Passes when incoming <= stored */
	SPUDGPU_COMPARE_OP_GREATER          = 4, /**< Passes when incoming > stored */
	SPUDGPU_COMPARE_OP_NOT_EQUAL        = 5, /**< Passes when incoming != stored */
	SPUDGPU_COMPARE_OP_GREATER_OR_EQUAL = 6, /**< Passes when incoming >= stored */
	SPUDGPU_COMPARE_OP_ALWAYS           = 7  /**< Always passes */
};

/**
 * @brief Configuration descriptor for a texture sampler.
 */
typedef struct spudgpu_sampler_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_SAMPLER_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** Filter when a texel covers more than one pixel (magnification). */
	SPUDGPU_FILTER mag_filter;
	/** Filter when a pixel covers more than one texel (minification). */
	SPUDGPU_FILTER min_filter;
	/** Filter between mip levels. */
	SPUDGPU_FILTER mipmap_filter;

	/** Addressing outside [0, 1] along U. */
	SPUDGPU_ADDRESS_MODE address_mode_u;
	/** Addressing outside [0, 1] along V. */
	SPUDGPU_ADDRESS_MODE address_mode_v;
	/** Addressing outside [0, 1] along W. */
	SPUDGPU_ADDRESS_MODE address_mode_w;

	/** Added to the computed level of detail before the mip level is chosen. */
	float mip_lod_bias;
	/** Lowest level of detail sampled: 0 is the full-size mip. */
	float min_lod;
	/** Highest level of detail sampled. */
	float max_lod;

	/** 1.0 disables anisotropic filtering. */
	float max_anisotropy;

	/**
	 * A comparison sampler: each texel read is compared with a reference
	 * the shader gives (GLSL's `sampler2DShadow` family, `texture()` with a
	 * depth to compare), and the read returns 1 where #compare_op holds and
	 * 0 where it doesn't, filtered as the texels would be - so a linear
	 * filter gives the share of four texels that pass. For depth textures.
	 * false for an ordinary sampler, whose #compare_op is not read.
	 */
	bool compare_enable;
	/** What the reference is compared with the texel by, the reference first: SPUDGPU_COMPARE_OP_LESS_OR_EQUAL passes where reference <= texel. */
	SPUDGPU_COMPARE_OP compare_op;
	/** @brief A name for diagnostics, or NULL. Copied, in every build; see
	 * spud_debug_name_set(). */
	const char *debug_name;
} spudgpu_sampler_desc;

/**
 * @brief Creates a sampler.
 *
 * @param[in]  device      Device to create the sampler on.
 * @param[in]  desc        Sampler configuration.
 * @param[out] out_sampler Receives the new sampler on success.
 *
 * @retval SPUD_SUCCESS The sampler was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_SAMPLER_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_sampler` is NULL.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_sampler()
 */
SPUDRESULT spudgpu_create_sampler(
    spudgpu_device device,
    const spudgpu_sampler_desc *desc,
    spudgpu_sampler *out_sampler);

/**
 * @brief Destroys a sampler.
 *
 * @param[in] sampler Sampler to destroy.
 */
void spudgpu_destroy_sampler(spudgpu_sampler sampler);

// ============================================================================
//  Descriptor Writes
//  Wires actual GPU resources into the allocated binding slots.
// ============================================================================

/**
 * @brief Describes a buffer range to write into a binding slot.
 */
/* Frozen: an array element of spudgpu_write_descriptor_set. It never changes; a new need gets a new type. */
typedef struct spudgpu_descriptor_buffer_info {
	/** The buffer to bind. */
	spudgpu_buffer buffer;

	/** Byte offset from the start of the buffer to begin the binding window. */
	uint64_t offset;

	/** Byte size of the binding window. Pass 0 to bind the entire buffer. */
	uint64_t range;
} spudgpu_descriptor_buffer_info;

/**
 * @brief Describes an image view + sampler to write into a binding slot.
 */
/* Frozen: an array element of spudgpu_write_descriptor_set. It never changes; a new need gets a new type. */
typedef struct spudgpu_descriptor_image_info {
	/** The image view to bind. */
	spudgpu_image_view image_view;

	/**
	 * @brief The layout the image is expected to be in when shaders access it.
	 *
	 * A SPUDGPU_IMAGE_LAYOUT value, never a native one. Common values:
	 *   - SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY for sampled textures.
	 *   - SPUDGPU_IMAGE_LAYOUT_GENERAL for storage images (read/write).
	 *
	 * @see SPUDGPU_IMAGE_LAYOUT
	 */
	SPUDGPU_IMAGE_LAYOUT image_layout;
} spudgpu_descriptor_image_info;

/**
 * @brief A single write operation targeting one binding slot in a descriptor
 * set.
 *
 * Fill either buffer_info or image_info depending on the descriptor_type.
 * The unused field is ignored by the backend.
 */
typedef struct spudgpu_write_descriptor_set {
	/** `type` is SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** The descriptor set to write into. */
	spudgpu_descriptor_set dst_set;

	/**
	 * The binding slot index to update (matches
	 * spudgpu_descriptor_binding_desc::binding).
	 */
	uint32_t dst_binding;

	/** First array element to update. Use 0 for non-array bindings. */
	uint32_t dst_array_element;

	/** Number of descriptors to update starting at dst_array_element. */
	uint32_t descriptor_count;

	/**
	 * The type of descriptor being written. Must match the layout's declared
	 * type.
	 */
	SPUDGPU_DESCRIPTOR_TYPE descriptor_type;

	/** Set when writing UNIFORM_BUFFER or STORAGE_BUFFER descriptors. */
	const spudgpu_descriptor_buffer_info *buffer_info;

	/**
	 * Set when writing SAMPLED_IMAGE, STORAGE_IMAGE, or COMBINED_IMAGE_SAMPLER
	 * descriptors.
	 */
	const spudgpu_descriptor_image_info *image_info;

	/** Set when writing SAMPLER or COMBINED_IMAGE_SAMPLER descriptors. */
	spudgpu_sampler sampler;

	/** Set when writing SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE
	 * descriptors: `descriptor_count` top levels
	 * (SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP), none NULL. Borrowed for the
	 * call. */
	const spudgpu_acceleration_structure *acceleration_structures;
} spudgpu_write_descriptor_set;

/**
 * @brief Writes resource handles into one or more descriptor sets.
 *
 * The equivalent of vkUpdateDescriptorSets. Call after allocating sets and
 * before binding them to a command list.
 *
 * Nothing is recorded if an element of `writes` has a `base` other than {SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET, NULL}
 * (see SPUD_STRUCT_BASE).
 *
 * Nothing is written either if a write puts an image view of type
 * SPUDGPU_IMAGE_VIEW_TYPE_CUBE or SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY in a
 * SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE descriptor: a storage view of a
 * cube is a SPUDGPU_IMAGE_VIEW_TYPE_2D_ARRAY view of its faces, on every
 * backend (D3D12 has no storage cube), and a shader declares it as a 2D
 * array image whose layer is face + 6 x cube. Every write is checked before
 * the first is made.
 *
 * Nor if a SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE write is made on a
 * device created without SPUDGPU_FEATURE_RAY_QUERY, or has a NULL
 * `acceleration_structures` or an element of it that is NULL (or, in a
 * Debug build, destroyed) or not a top level.
 *
 * @param[in] device      Device that owns the sets.
 * @param[in] writes      Array of write operations.
 * @param[in] write_count Number of elements in `writes`.
 */
void spudgpu_update_descriptor_sets(
    spudgpu_device device,
    const spudgpu_write_descriptor_set *writes,
    uint32_t write_count);

// ============================================================================
//  Command list binding
// ============================================================================

/**
 * @brief Binds descriptor sets for subsequent draws with a graphics pipeline.
 *
 * Maps to vkCmdBindDescriptorSets. Call after binding the pipeline and before
 * the draw.
 *
 * @param[in] cmd       Command list being recorded.
 * @param[in] pipeline  Graphics pipeline whose layout defines the set slots.
 * @param[in] first_set Set index of the first element of `sets` (usually 0).
 * @param[in] sets      Array of descriptor sets to bind.
 * @param[in] set_count Number of elements in `sets`.
 */
void spudgpu_cmd_bind_descriptor_sets(
    spudgpu_command_list cmd,
    spudgpu_shader_pipeline pipeline,
    uint32_t first_set,
    const spudgpu_descriptor_set *sets,
    uint32_t set_count);

/**
 * @brief Compute-pipeline variant of spudgpu_cmd_bind_descriptor_sets().
 *
 * @param[in] cmd       Command list being recorded.
 * @param[in] pipeline  Compute pipeline whose layout defines the set slots.
 * @param[in] first_set Set index of the first element of `sets` (usually 0).
 * @param[in] sets      Array of descriptor sets to bind.
 * @param[in] set_count Number of elements in `sets`.
 */
void spudgpu_cmd_bind_descriptor_sets_compute(
    spudgpu_command_list cmd,
    spudgpu_compute_pipeline pipeline,
    uint32_t first_set,
    const spudgpu_descriptor_set *sets,
    uint32_t set_count);

// ============================================================================
//  Bindless / Descriptor Indexing
// ============================================================================

/**
 * @brief 1 when the backend compiled into this build implements bindless (Vulkan,
 * D3D12), 0 otherwise (Metal). It says nothing about any GPU: whether one has
 * bindless is SPUDGPU_FEATURE_BINDLESS in its adapter's supported_features,
 * and whether a device may use it is that bit in the device's features.
 *
 * Where a descriptor set has one schema per shader and is bound per draw,
 * bindless is one table per resource class, per device, that every pipeline
 * reads by a plain integer index.
 *
 * The classes are kept apart (sampled image, storage image, storage buffer)
 * and each has its own index space: Vulkan's descriptors are typed per
 * binding and cannot mix types in one array, so a single shared index space
 * could not be kept on every backend. Samplers are not part of it: a
 * spudgpu_sampler is bound the ordinary way, through a descriptor set. Every
 * class uses a plain 0-based index.
 *
 * Maps to update-after-bind descriptor arrays (Vulkan 1.2 descriptor
 * indexing) and to descriptor tables under resource binding tier 3 (D3D12).
 * D3D12 uses large descriptor tables, one per class, and not Shader Model
 * 6.6's `ResourceDescriptorHeap[]`: its backend cross-compiles the same SPIR-V
 * the Vulkan backend runs, through SPIRV-Cross, which emits a table for a
 * GLSL runtime-sized binding array. The Metal counterpart, Argument Buffers
 * Tier 2 with `MTLResourceID`, is not implemented: it needs an
 * `MTLHeap`-backed resource allocator SpudGPU does not have.
 *
 * Where the macro is 0 nothing in the section is declared, and calling one of
 * its functions is a compile error and not a silent no-op. Where it is 1, the
 * calls return SPUDRESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED on a device created
 * without SPUDGPU_FEATURE_BINDLESS. The table is made with the device, when
 * the feature is asked for, and takes the three counts of
 * SPUDGPU_BINDLESS_LIMITS out of the device's descriptor budget.
 */
#if SPUDGPU_COMPILE_VULKAN_API || SPUDGPU_COMPILE_D3D12_API
#define SPUDGPU_EXT_BINDLESS 1
#else
#define SPUDGPU_EXT_BINDLESS 0
#endif

#if SPUDGPU_EXT_BINDLESS

/** An index that never names a registered bindless slot. */
#define SPUDGPU_BINDLESS_INVALID_INDEX 0xFFFFFFFFu

/**
 * @brief The size of the bindless table, chained onto
 * SPUDGPU_ADAPTER_PROPERTIES. All 0 when the adapter lacks
 * SPUDGPU_FEATURE_BINDLESS.
 */
typedef struct SPUDGPU_BINDLESS_LIMITS {
	/** `type` is SPUD_STRUCT_TYPE_GPU_BINDLESS_LIMITS; `next` is the rest of
	 * the adapter properties' chain. The caller sets the type; SpudGPU fills
	 * in the rest. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE_OUT base;
	/** Number of slots in the sampled image table. */
	uint32_t max_sampled_images;
	/** Number of slots in the storage image table. */
	uint32_t max_storage_images;
	/** Number of slots in the storage buffer table. */
	uint32_t max_storage_buffers;
} SPUDGPU_BINDLESS_LIMITS;

/**
 * @brief Returns the device's single global bindless descriptor set layout.
 *
 * Include this handle in spudgpu_shader_pipeline_desc::descriptor_set_layouts
 * (or spudgpu_compute_pipeline_desc's) at whichever set index the shaders
 * declare the unbounded arrays at (binding 0 = sampled images, binding 1 =
 * storage images, binding 2 = storage buffers), then pass that same index as
 * `set_index` to spudgpu_cmd_bind_bindless_resources().
 *
 * D3D12: the handle describes three large descriptor-table ranges (one per
 * resource class) rather than SM6.6 ResourceDescriptorHeap[] dynamic
 * resources; see SPUDGPU_EXT_BINDLESS for why.
 *
 * @warning Do not pass the returned handle to
 * spudgpu_destroy_descriptor_set_layout(); the device owns it for its
 * lifetime.
 *
 * @param[in] device Device to get the layout from.
 *
 * @return The layout, or NULL if `device` is NULL or was created without
 *         SPUDGPU_FEATURE_BINDLESS.
 */
spudgpu_descriptor_set_layout spudgpu_get_bindless_descriptor_set_layout(spudgpu_device device);

/**
 * @brief Registers a sampled-image view in the device's global bindless
 * table and returns a stable index for it.
 *
 * The index, and the underlying image and view, must stay valid until
 * spudgpu_bindless_unregister_sampled_image() is called. Registration uses
 * update-after-bind writes, so it is safe to call between frames, or while
 * command lists referencing *other* indices are in flight; do not rewrite an
 * index that a submitted but not yet completed command list is reading.
 *
 * @param[in]  device    Device whose bindless table is written.
 * @param[in]  view      Image view to register.
 * @param[out] out_index Receives the index shaders use to reach the view.
 *
 * @retval SPUD_SUCCESS `out_index` was written.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_IMAGE_VIEW `view` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_index` is NULL.
 * @retval SPUDRESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED The device was created
 *         without SPUDGPU_FEATURE_BINDLESS.
 * @retval SPUDRESULT_GPU_BINDLESS_OUT_OF_SLOTS Every slot of this resource
 *         class is registered.
 */
SPUDRESULT spudgpu_bindless_register_sampled_image(
    spudgpu_device device,
    spudgpu_image_view view,
    uint32_t *out_index);

/**
 * @brief Frees a registered sampled-image slot for reuse.
 *
 * @param[in] device Device whose bindless table is written.
 * @param[in] index  Index returned by
 *                   spudgpu_bindless_register_sampled_image().
 */
void spudgpu_bindless_unregister_sampled_image(
    spudgpu_device device,
    uint32_t index);

/**
 * @brief Registers a storage-image view in the device's global bindless
 * table and returns a stable index for it.
 *
 * Same rules as spudgpu_bindless_register_sampled_image().
 *
 * @param[in]  device    Device whose bindless table is written.
 * @param[in]  view      Image view to register.
 * @param[out] out_index Receives the index shaders use to reach the view.
 *
 * @retval SPUD_SUCCESS `out_index` was written.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_IMAGE_VIEW `view` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_index` is NULL.
 * @retval SPUDRESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED The device was created
 *         without SPUDGPU_FEATURE_BINDLESS.
 * @retval SPUDRESULT_GPU_BINDLESS_OUT_OF_SLOTS Every slot of this resource
 *         class is registered.
 */
SPUDRESULT spudgpu_bindless_register_storage_image(
    spudgpu_device device,
    spudgpu_image_view view,
    uint32_t *out_index);

/**
 * @brief Frees a registered storage-image slot for reuse.
 *
 * @param[in] device Device whose bindless table is written.
 * @param[in] index  Index returned by
 *                   spudgpu_bindless_register_storage_image().
 */
void spudgpu_bindless_unregister_storage_image(
    spudgpu_device device,
    uint32_t index);

/**
 * @brief Registers a storage-buffer view in the device's global bindless
 * table and returns a stable index for it.
 *
 * Same rules as spudgpu_bindless_register_sampled_image().
 *
 * @param[in]  device    Device whose bindless table is written.
 * @param[in]  view      Buffer view to register.
 * @param[out] out_index Receives the index shaders use to reach the view.
 *
 * @retval SPUD_SUCCESS `out_index` was written.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_BUFFER_VIEW `view` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_index` is NULL.
 * @retval SPUDRESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED The device was created
 *         without SPUDGPU_FEATURE_BINDLESS.
 * @retval SPUDRESULT_GPU_BINDLESS_OUT_OF_SLOTS Every slot of this resource
 *         class is registered.
 */
SPUDRESULT spudgpu_bindless_register_storage_buffer(
    spudgpu_device device,
    spudgpu_buffer_view view,
    uint32_t *out_index);

/**
 * @brief Frees a registered storage-buffer slot for reuse.
 *
 * @param[in] device Device whose bindless table is written.
 * @param[in] index  Index returned by
 *                   spudgpu_bindless_register_storage_buffer().
 */
void spudgpu_bindless_unregister_storage_buffer(
    spudgpu_device device,
    uint32_t index);

/**
 * @brief Makes the device's global bindless resource tables visible to
 * subsequent draws on a command list.
 *
 * - Vulkan: vkCmdBindDescriptorSets against the global set at `set_index`.
 * - D3D12: ID3D12GraphicsCommandList::SetDescriptorHeaps with the device's
 *   global CBV/SRV/UAV heap; `set_index` is ignored.
 * - Metal: marks every currently-registered resource resident on this command
 *   buffer (useResource:usage:); `set_index` is ignored.
 *
 * Call once per command list before the first bindless-indexed draw.
 *
 * @param[in] cmd       Command list being recorded.
 * @param[in] pipeline  The bound graphics pipeline.
 * @param[in] set_index Slot in the pipeline's `descriptor_set_layouts` that
 *                      holds the handle from
 *                      spudgpu_get_bindless_descriptor_set_layout().
 */
void spudgpu_cmd_bind_bindless_resources(
    spudgpu_command_list cmd,
    spudgpu_shader_pipeline pipeline,
    uint32_t set_index);

/**
 * @brief Compute-pipeline variant of spudgpu_cmd_bind_bindless_resources().
 *
 * @param[in] cmd       Command list being recorded.
 * @param[in] pipeline  The bound compute pipeline.
 * @param[in] set_index Slot in the pipeline's `descriptor_set_layouts` that
 *                      holds the handle from
 *                      spudgpu_get_bindless_descriptor_set_layout().
 */
void spudgpu_cmd_bind_bindless_resources_compute(
    spudgpu_command_list cmd,
    spudgpu_compute_pipeline pipeline,
    uint32_t set_index);

#endif // SPUDGPU_EXT_BINDLESS

/**
 * @brief How vertices are assembled into primitives.
 */
typedef uint32_t SPUDGPU_PRIMITIVE_TOPOLOGY;

enum {
	SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST  = 0, /**< Every three vertices are a triangle */
	SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP = 1, /**< Each vertex after the second makes a triangle with the two before it */
	SPUDGPU_PRIMITIVE_TOPOLOGY_LINE_LIST      = 2, /**< Every two vertices are a line */
	SPUDGPU_PRIMITIVE_TOPOLOGY_LINE_STRIP     = 3, /**< Each vertex after the first makes a line with the one before it */
	SPUDGPU_PRIMITIVE_TOPOLOGY_POINT_LIST     = 4, /**< Every vertex is a point */
	SPUDGPU_PRIMITIVE_TOPOLOGY_PATCH_LIST     = 5  /**< Tessellation patches of spudgpu_shader_pipeline_desc::patch_control_points vertices */
};

/**
 * @brief Which triangle faces are discarded before rasterization.
 */
typedef uint32_t SPUDGPU_CULL_MODE;

enum {
	SPUDGPU_CULL_MODE_NONE  = 0, /**< No face is discarded */
	SPUDGPU_CULL_MODE_FRONT = 1, /**< Front faces are discarded */
	SPUDGPU_CULL_MODE_BACK  = 2  /**< Back faces are discarded */
};


/**
 * @brief Capacity of spudgpu_shader_pipeline_desc::vertex_attributes.
 */
#define SPUDGPU_MAX_VERTEX_ATTRIBUTES 16

/**
 * @brief Capacity of spudgpu_shader_pipeline_desc::vertex_bindings.
 */
#define SPUDGPU_MAX_VERTEX_BINDINGS 8

/**
 * @brief Capacity of a pipeline descriptor's push_constant_ranges array.
 */
#define SPUDGPU_MAX_PUSH_CONSTANT_RANGES 4

/**
 * @brief Upper bound on simultaneous color attachments: in one rendering
 * pass, and in one pipeline's spudgpu_shader_pipeline_desc.
 */
#define SPUDGPU_MAX_COLOR_ATTACHMENTS 8

/**
 * @brief One vertex attribute: where it sits in a vertex and which shader
 * input it feeds.
 */
/* Frozen: an array element of spudgpu_shader_pipeline_desc. It never changes; a new need gets a new type. */
typedef struct spudgpu_vertex_attribute_desc {
	/**
	 * Which shader location slot this attribute binds to (layout(location =
	 * N)).
	 */
	uint32_t location;

	/** Which vertex buffer binding slot this attribute is sourced from. */
	uint32_t binding;

	/**
	 * The data layout and channel bit-depth of this attribute (e.g.,
	 * SPUDGPU_FORMAT_R32G32B32_FLOAT).
	 */
	SPUDGPU_FORMAT format;

	/** Byte offset of this attribute from the start of a single vertex element. */
	uint32_t offset;
} spudgpu_vertex_attribute_desc;

/**
 * @brief One vertex buffer binding slot: its stride and whether it advances
 * per vertex or per instance.
 */
/* Frozen: an array element of spudgpu_shader_pipeline_desc. It never changes; a new need gets a new type. */
typedef struct spudgpu_vertex_binding_desc {
	/** The binding slot index this entry targets. */
	uint32_t binding;

	/**
	 * Byte distance between consecutive elements in the buffer (sizeof your
	 * vertex struct).
	 */
	uint32_t stride;

	/**
	 * When true, advances per-instance rather than per-vertex (instanced
	 * rendering).
	 */
	bool per_instance;
} spudgpu_vertex_binding_desc;

/**
 * @brief A byte range of the push constant block and the shader stages that
 * read it.
 */
/* Frozen: an array element of the pipeline descs. It never changes; a new need gets a new type. */
typedef struct spudgpu_push_constant_range_desc {
	/**
	 * Bitmask of shader stages that can read this push constant range.
	 * @see SPUDGPU_SHADER_STAGE
	 */
	SPUDGPU_SHADER_STAGE stage_flags;

	/** Byte offset within the push constant block. */
	uint32_t offset;

	/** Byte size of this push constant range. */
	uint32_t size;
} spudgpu_push_constant_range_desc;

/**
 * @brief Factor a source or destination color or alpha is multiplied by when
 * blending.
 */
typedef uint32_t SPUDGPU_BLEND_FACTOR;

enum {
	SPUDGPU_BLEND_FACTOR_ZERO                = 0, /**< 0 */
	SPUDGPU_BLEND_FACTOR_ONE                 = 1, /**< 1 */
	SPUDGPU_BLEND_FACTOR_SRC_ALPHA           = 2, /**< Source alpha */
	SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 3, /**< 1 - source alpha */
	SPUDGPU_BLEND_FACTOR_DST_ALPHA           = 4, /**< Destination alpha */
	SPUDGPU_BLEND_FACTOR_ONE_MINUS_DST_ALPHA = 5, /**< 1 - destination alpha */
	SPUDGPU_BLEND_FACTOR_SRC_COLOR           = 6, /**< Source color, per channel */
	SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_COLOR = 7, /**< 1 - source color, per channel */
	SPUDGPU_BLEND_FACTOR_DST_COLOR           = 8, /**< Destination color, per channel */
	SPUDGPU_BLEND_FACTOR_ONE_MINUS_DST_COLOR = 9  /**< 1 - destination color, per channel */
};

/**
 * @brief How the weighted source and destination values are combined when
 * blending.
 */
typedef uint32_t SPUDGPU_BLEND_OP;

enum {
	SPUDGPU_BLEND_OP_ADD              = 0, /**< source + destination */
	SPUDGPU_BLEND_OP_SUBTRACT         = 1, /**< source - destination */
	SPUDGPU_BLEND_OP_REVERSE_SUBTRACT = 2, /**< destination - source */
	SPUDGPU_BLEND_OP_MIN              = 3, /**< The smaller of the two, factors ignored */
	SPUDGPU_BLEND_OP_MAX              = 4  /**< The larger of the two, factors ignored */
};

/**
 * @brief Per-attachment blend configuration.
 * Ignored entirely when blend_enable is false.
 */
/* Frozen: an array element of spudgpu_shader_pipeline_desc. It never changes; a new need gets a new type. */
typedef struct spudgpu_blend_attachment_desc {
	/** When false the fragment's color replaces the attachment's. */
	bool blend_enable;

	/** What the fragment's RGB is multiplied by. */
	SPUDGPU_BLEND_FACTOR src_color_blend_factor;
	/** What the attachment's RGB is multiplied by. */
	SPUDGPU_BLEND_FACTOR dst_color_blend_factor;
	/** How the two weighted RGB values are combined. */
	SPUDGPU_BLEND_OP color_blend_op;

	/** What the fragment's alpha is multiplied by. */
	SPUDGPU_BLEND_FACTOR src_alpha_blend_factor;
	/** What the attachment's alpha is multiplied by. */
	SPUDGPU_BLEND_FACTOR dst_alpha_blend_factor;
	/** How the two weighted alpha values are combined. */
	SPUDGPU_BLEND_OP alpha_blend_op;
} spudgpu_blend_attachment_desc;

/**
 * @brief Describes a shader module: one entry point, in the device's own
 * shader format.
 *
 * **Where a module's bindings are.** SpudGPU builds each pipeline's binding
 * layout from its descriptor set layouts and push constant ranges, never
 * from the module, so the module must put each resource where its backend
 * looks. For a binding @c b of set @c s (spudgpu_descriptor_set_layout_binding::binding,
 * and the set's index in the pipeline desc's descriptor_set_layouts):
 *
 * - **Vulkan (SPIR-V):** DescriptorSet @c s, Binding @c b. The push
 *   constants are the module's push constant block.
 * - **D3D12 (DXIL):** register @c b of space @c s, of the class its
 *   descriptor type takes: a uniform buffer `b<b>`; a sampled image `t<b>`;
 *   a storage buffer or storage image `u<b>` (never an SRV, even read
 *   only); a sampler `s<b>`; a combined image sampler both `t<b>` and
 *   `s<b>` (and, with SPUDGPU_EXT_RAY_QUERY, an acceleration structure
 *   `t<b>`). The push constants are a
 *   constant buffer at @c b0 of space SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS
 *   (4). A vertex input at location @c n has the semantic @c TEXCOORD<n>.
 * - **Metal (.metallib):** set @c s is an argument buffer (tier 1, members
 *   in binding order) at buffer index 26 + @c s, each binding at
 *   @c [[id(b)]], an array of @c n at @c b to @c b + @c n - 1, and a
 *   combined image sampler's sampler at @c [[id(1024 + b)]] (so no binding
 *   of such a set reaches 1024). The push constants are a constant buffer
 *   at buffer index 30. A vertex buffer binding @c v is at buffer index
 *   @c v, so vertex bindings stay below 26.
 *
 * A compiler writes the first two from per-resource bindings (in Slang,
 * `[[vk::binding(b, s)]]` with `register(<class><b>, space<s>)`); SPIR-V
 * cross-compiled with SPIRV-Cross under these remaps gives the third.
 *
 * **Every binding of a set.** A module that reads any binding of set @c s
 * declares every binding of set @c s's layout, each with the layout's
 * descriptor type and count, and no other binding in that set, whether or
 * not its entry point reads them. Metal needs it: a set's argument buffer
 * is written from the whole layout, and a module's argument buffer struct
 * is laid out from what the module declares, so a module that leaves one
 * out reads the wrong members. Vulkan and D3D12 don't need it, and their
 * backends don't check it; a module kept to it runs on all three. Metal's
 * backend checks it when a pipeline is created
 * (SPUDRESULT_GPU_SHADER_LAYOUT_MISMATCH).
 *
 * **Clip space** is +Y up on every backend: a vertex or mesh shader's
 * position (x, y) = (-1, +1) is the viewport's top left. The Vulkan backend
 * keeps it by viewport (spudgpu_cmd_set_viewports()); no backend changes a
 * shader.
 *
 * **A vertex shader's vertex and instance index** (SV_VertexID and
 * SV_InstanceID) are defined only for draws whose first vertex (an indexed
 * draw's vertex offset too) and first instance are 0: Vulkan and Metal
 * count them from the buffers' start, D3D12 from the draw's first. Every
 * Vulkan device has what a module reading them needs (shaderDrawParameters).
 *
 * **Stages' outputs and inputs** are matched by location on Vulkan and
 * Metal and by register in order on D3D12: declare every output from
 * location 0 up to the last one read (the rule spudgpu_create_shader_pipeline()
 * already states).
 */
typedef struct spudgpu_shader_module_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_SHADER_MODULE_DESC; `next` is NULL.
	 * Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** The stage the entry point is for. */
	SPUDGPU_SHADER_STAGE stage;
	/** What #code is: the format the device takes
	 * (SPUDGPU_ADAPTER_PROPERTIES::shader_formats). */
	SPUDGPU_SHADER_FORMAT format;
	/** The module's bytes, 4-byte aligned. Borrowed for the call: the
	 * module keeps a copy. */
	const void *code;
	/** Bytes in #code, not 0; a multiple of 4 for SPIR-V and DXIL. */
	uint64_t code_size;
	/** The entry point's name, null-terminated: the module must have one
	 * of that name for #stage. Required. Copied. */
	const char *entry_point;
	/** The entry point's workgroup size (x, y, z), as the shader declares
	 * it: each 1 or more for SPUDGPU_SHADER_STAGE_COMPUTE, _MESH
	 * and _TASK, all 0 for every other stage. A size other than the
	 * shader's is undefined (Metal can't check it, so no backend does). */
	uint32_t workgroup_size[3];
	/** @brief A name for diagnostics, or NULL. Copied, in every build; see
	 * spud_debug_name_set(). */
	const char *debug_name;
} spudgpu_shader_module_desc;

/**
 * @brief Creates a shader module from code in the device's own shader
 * format. Translates and compiles nothing.
 *
 * @param[in]  device     Device to create the module on.
 * @param[in]  desc       The stage, format, code, entry point and workgroup
 *                        size.
 * @param[out] out_module Receives the module; untouched on failure.
 *
 * Checked in this order, on every backend:
 * @retval SPUD_SUCCESS The module was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL (or, in Debug,
 *         destroyed).
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE / SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN
 *         `desc->base` is wrong.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_module` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_SHADER_STAGE `desc->stage` is not one
 *         known stage.
 * @retval SPUDRESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED `desc->format`
 *         is not in the device's shader_formats (SPUDGPU_SHADER_FORMAT_NONE
 *         included).
 * @retval SPUDRESULT_GPU_NULL_SHADER_CODE `desc->code` is NULL or
 *         `desc->code_size` is 0.
 * @retval SPUDRESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT
 *         `desc->code` is not 4-byte aligned, or `desc->code_size` is not a
 *         multiple of 4 for SPIR-V or DXIL.
 * @retval SPUDRESULT_GPU_NULL_ENTRY_POINT `desc->entry_point` is
 *         NULL or empty.
 * @retval SPUDRESULT_GPU_INVALID_WORKGROUP_SIZE a compute, mesh or
 *         task stage with a 0 in `desc->workgroup_size`, or another stage
 *         with anything but 0.
 * @retval SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED a mesh or task
 *         stage on a device created without SPUDGPU_FEATURE_MESH_SHADING.
 * @retval SPUDRESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED a geometry stage
 *         on a device created without SPUDGPU_FEATURE_GEOMETRY_SHADER.
 * @retval SPUDRESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED a tessellation
 *         control or evaluation stage on a device created without
 *         SPUDGPU_FEATURE_TESSELLATION.
 * @retval SPUDRESULT_GPU_INVALID_SHADER_CODE the code isn't the format
 *         (SPIR-V's magic word, a DXIL container's 'DXBC' and its PSV0 part, a
 *         library Metal refuses).
 * @retval SPUDRESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND the module has
 *         no entry point named `desc->entry_point` for `desc->stage`.
 * @retval SPUDRESULT_OUT_OF_MEMORY the copy or the module couldn't be
 *         allocated.
 * @return SPUDRESULT_API_SPECIFIC_FAILURE if the graphics API fails
 *         otherwise.
 */
SPUDRESULT spudgpu_create_shader_module(
    spudgpu_device device,
    const spudgpu_shader_module_desc *desc,
    spudgpu_shader_module *out_module);

/**
 * @brief Destroys a shader module.
 *
 * @param[in] shader_module Module to destroy.
 */
void spudgpu_destroy_shader_module(spudgpu_shader_module shader_module);

/**
 * @brief Complete configuration descriptor for creating a graphics shader
 * pipeline.
 *
 * Fill in the stage modules you need, leave optional ones NULL.
 * All arrays are inline and fixed-capacity; use the corresponding _count
 * field to indicate how many entries are valid.
 */
typedef struct spudgpu_shader_pipeline_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;

	/** @brief A name for diagnostics, or NULL. Copied, in every build; see
	 * spud_debug_name_set(). */
	const char *debug_name;

	// -----------------------------------------------------------------------
	// Shader stages
	// -----------------------------------------------------------------------

	/**
	 * Compiled vertex shader module. Required unless mesh_module is set —
	 * a pipeline is either vertex-fetch-based (vertex_module +
	 * vertex_attributes/vertex_bindings below) or mesh-shader-based
	 * (mesh_module, which supplies its own geometry with no vertex input
	 * state at all), never both. @see SPUDGPU_EXT_MESH_SHADING
	 */
	spudgpu_shader_module vertex_module;

	/**
	 * Compiled mesh shader module. Mutually exclusive with vertex_module —
	 * set this instead to build a mesh-shader pipeline (no vertex input
	 * layout/input-assembly stage). Optional — leave NULL for a classic
	 * vertex-fetch pipeline. @see SPUDGPU_EXT_MESH_SHADING
	 */
	spudgpu_shader_module mesh_module;

	/**
	 * Compiled amplification/task shader module, upstream of mesh_module.
	 * Optional — leave NULL to dispatch mesh shader workgroups directly
	 * with no task stage (this is what spudgpu_cmd_dispatch_mesh does).
	 * Ignored unless mesh_module is also set. @see SPUDGPU_EXT_MESH_SHADING
	 */
	spudgpu_shader_module task_module;

	/** Compiled fragment shader module. Required. */
	spudgpu_shader_module fragment_module;

	/** Compiled geometry shader module. Optional — leave NULL to skip. A
	 * device created without SPUDGPU_FEATURE_GEOMETRY_SHADER refuses one
	 * (SPUDGPU_EXT_GEOMETRY_SHADER). */
	spudgpu_shader_module geometry_module;

	/**
	 * Compiled tessellation control shader module. Optional — leave NULL to
	 * skip.
	 */
	spudgpu_shader_module tess_control_module;

	/**
	 * Compiled tessellation evaluation shader module. Optional — leave NULL to
	 * skip.
	 */
	spudgpu_shader_module tess_eval_module;

	// -----------------------------------------------------------------------
	// Vertex input layout
	// -----------------------------------------------------------------------

	/**
	 * The vertex shader's inputs. Only the first #vertex_attribute_count are
	 * read. Unused by a mesh-shader pipeline.
	 */
	spudgpu_vertex_attribute_desc vertex_attributes[SPUDGPU_MAX_VERTEX_ATTRIBUTES];
	/** Number of valid entries in #vertex_attributes. */
	uint32_t vertex_attribute_count;

	/**
	 * The vertex buffer slots the attributes read from. Only the first
	 * #vertex_binding_count are read. Unused by a mesh-shader pipeline.
	 */
	spudgpu_vertex_binding_desc vertex_bindings[SPUDGPU_MAX_VERTEX_BINDINGS];
	/** Number of valid entries in #vertex_bindings. */
	uint32_t vertex_binding_count;

	// -----------------------------------------------------------------------
	// Input assembly
	// -----------------------------------------------------------------------

	/**
	 * How raw vertices are assembled into primitives before rasterization.
	 * @see SPUDGPU_PRIMITIVE_TOPOLOGY
	 */
	SPUDGPU_PRIMITIVE_TOPOLOGY primitive_topology;

	// -----------------------------------------------------------------------
	// Rasterizer
	// -----------------------------------------------------------------------

	/**
	 * Which triangle faces to discard before fragment shading.
	 * @see SPUDGPU_CULL_MODE
	 */
	SPUDGPU_CULL_MODE cull_mode;

	/**
	 * When true, triangles with counter-clockwise winding are treated as
	 * front-facing.
	 */
	bool front_face_ccw;

	/**
	 * When true, geometry is rasterized as wireframe lines instead of filled
	 * triangles.
	 */
	bool wireframe;

	/**
	 * Depth bias, added to each fragment's depth before the depth test and
	 * the depth write: what keeps a shadow map's surfaces from shadowing
	 * themselves. The bias is
	 *
	 *     depth_bias_constant * r + depth_bias_slope * m
	 *
	 * clamped to #depth_bias_clamp, where m is the triangle's greatest depth
	 * slope and r the smallest difference the depth format resolves. All
	 * three 0 is no bias. One rule holds on every backend: for a UNORM depth
	 * format r is 1 / 2^bits; for a float format each graphics API derives
	 * it from the triangle's exponent in its own way, so the same constant
	 * may move a float depth by a different amount per backend. What a
	 * bias does to line and point primitives differs between the APIs and
	 * is undefined here: bias triangles.
	 *
	 * An integer because D3D12's constant is one. Vulkan:
	 * `VkPipelineRasterizationStateCreateInfo::depthBias*`. D3D12:
	 * `D3D12_RASTERIZER_DESC::DepthBias`, `SlopeScaledDepthBias`,
	 * `DepthBiasClamp`. Metal: `setDepthBias:slopeScale:clamp:` on the
	 * encoder, set by spudgpu_cmd_bind_pipeline().
	 */
	int32_t depth_bias_constant;
	/** The depth slope's factor in the bias. See #depth_bias_constant. */
	float depth_bias_slope;
	/**
	 * The largest bias, if positive, or the smallest, if negative; 0 for no
	 * clamp. Anything but 0 needs a device created with
	 * SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP. See
	 * #depth_bias_constant.
	 */
	float depth_bias_clamp;

	// -----------------------------------------------------------------------
	// Depth / stencil
	// -----------------------------------------------------------------------

	/** Enable depth testing against the depth attachment. */
	bool depth_test_enable;

	/** Allow the depth test to write new values into the depth attachment. */
	bool depth_write_enable;

	/**
	 * The comparison function used when depth testing a fragment.
	 * @see SPUDGPU_COMPARE_OP
	 */
	SPUDGPU_COMPARE_OP depth_compare_op;

	/**
	 * Discards a fragment whose depth attachment value (already primed by
	 * an earlier draw, not this fragment's own depth) falls outside
	 * spudgpu_cmd_set_depth_bounds' [min, max] range. Independent of
	 * depth_test_enable/depth_compare_op above — the two mechanisms compose
	 * (both run when both are enabled) rather than one replacing the other.
	 * The range is set by spudgpu_cmd_set_depth_bounds after the pipeline
	 * is bound; only a draw with a pipeline created with this true is
	 * tested, on every backend. On a device
	 * created without SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST (see
	 * SPUDGPU_EXT_DEPTH_BOUNDS_TEST) the pipeline behaves as if this is
	 * false.
	 */
	bool depth_bounds_test_enable;

	// -----------------------------------------------------------------------
	// Blend state
	// -----------------------------------------------------------------------

	/**
	 * Blend configuration for each color attachment: entry i for
	 * #color_attachment_formats[i]. Only the first #color_attachment_count
	 * are read. Entries that differ from entry 0 need a device created with
	 * SPUDGPU_FEATURE_INDEPENDENT_BLEND.
	 * @see spudgpu_blend_attachment_desc
	 */
	spudgpu_blend_attachment_desc blend_attachments[SPUDGPU_MAX_COLOR_ATTACHMENTS];

	// -----------------------------------------------------------------------
	// Attachment formats
	// -----------------------------------------------------------------------

	/**
	 * Pixel format of each color attachment the pipeline writes, in the
	 * order of the fragment shader's outputs (location i writes attachment
	 * i) and of spudgpu_rendering_begin_desc::color_attachments in a pass
	 * that draws with it, which has the same count and formats. Only the
	 * first #color_attachment_count are read.
	 * @see SPUDGPU_FORMAT
	 */
	SPUDGPU_FORMAT color_attachment_formats[SPUDGPU_MAX_COLOR_ATTACHMENTS];
	/**
	 * Number of valid entries in #color_attachment_formats and
	 * #blend_attachments. 0 for a pipeline that writes depth only. At most
	 * SPUDGPU_MAX_COLOR_ATTACHMENTS.
	 */
	uint32_t color_attachment_count;

	/**
	 * Pixel format of the depth attachment. Set to SPUDGPU_FORMAT_UNKNOWN for
	 * no depth.
	 * @see SPUDGPU_FORMAT
	 */
	SPUDGPU_FORMAT depth_format;

	// -----------------------------------------------------------------------
	// Pipeline layout
	// -----------------------------------------------------------------------

	/**
	 * Opaque spudgpu_descriptor_set_layout handles, from
	 * spudgpu_create_descriptor_set_layout() - declared as void* here
	 * rather than the proper typedef, but each backend casts entries back
	 * to its own internal descriptor-set-layout type, the same as every
	 * other opaque handle in this header. Not a raw native handle of any
	 * specific backend's API.
	 */
	void *descriptor_set_layouts[SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	/** Number of valid entries in #descriptor_set_layouts. */
	uint32_t descriptor_set_layout_count;

	/**
	 * The ranges of the push constant block the pipeline's stages read. Only
	 * the first #push_constant_range_count are read.
	 */
	spudgpu_push_constant_range_desc push_constant_ranges[SPUDGPU_MAX_PUSH_CONSTANT_RANGES];
	/** Number of valid entries in #push_constant_ranges. */
	uint32_t push_constant_range_count;

	// -----------------------------------------------------------------------
	// Tessellation
	// -----------------------------------------------------------------------

	/**
	 * Number of control points per patch. Only used when both tess stages are
	 * present. Defaults to 3 when set to 0.
	 */
	uint32_t patch_control_points;
} spudgpu_shader_pipeline_desc;

/**
 * @brief Creates a graphics pipeline.
 *
 * One rule links the stages on every backend, whatever the graphics API
 * underneath matches by: the fragment shader declares the inputs the vertex
 * (or mesh) shader outputs from location 0 up to the last one it reads, each
 * at the same location with the same type and interpolation, whether it
 * reads it or not. It may leave out only the outputs after that. Vulkan and
 * Metal match inputs to outputs by location, but D3D12 matches them by
 * hardware register, in order, so a fragment shader that skips a lower
 * location doesn't link there, and the pipeline fails with
 * SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @param[in]  device       Device to create the pipeline on.
 * @param[in]  desc         Pipeline configuration.
 * @param[out] out_pipeline Receives the new pipeline on success.
 *
 * @retval SPUD_SUCCESS The pipeline was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_pipeline` is NULL.
 * @retval SPUDRESULT_GPU_VERTEX_AND_FRAGMENT_SHADER_REQUIRED
 *         `desc->fragment_module` is NULL, or both `desc->vertex_module` and
 *         `desc->mesh_module` are.
 * @retval SPUDRESULT_GPU_INVALID_SHADER_STAGE `desc->vertex_module` and
 *         `desc->mesh_module` are both set.
 * @retval SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED `desc->mesh_module`
 *         is set and the device wasn't created with
 *         SPUDGPU_FEATURE_MESH_SHADING.
 * @retval SPUDRESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS
 *         `desc->color_attachment_count` is above
 *         SPUDGPU_MAX_COLOR_ATTACHMENTS.
 * @retval SPUDRESULT_GPU_DEPTH_BIAS_CLAMP_NOT_SUPPORTED
 *         `desc->depth_bias_clamp` is not 0 and the device wasn't created
 *         with SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP.
 * @retval SPUDRESULT_GPU_INDEPENDENT_BLEND_NOT_SUPPORTED Two of the first
 *         `desc->color_attachment_count` blend attachments differ and the
 *         device wasn't created with SPUDGPU_FEATURE_INDEPENDENT_BLEND.
 * @retval SPUDRESULT_GPU_SHADER_LAYOUT_MISMATCH On Metal, a module's
 *         argument buffer for a set it reads isn't that set's layout
 *         (spudgpu_shader_module_desc, "Every binding of a set"): a binding
 *         of the layout left out, one it doesn't hold, or one of another
 *         type or count. Checked for each stage's module and each of its
 *         sets. Not returned by Vulkan or D3D12.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_shader_pipeline()
 */
SPUDRESULT spudgpu_create_shader_pipeline(
    spudgpu_device device,
    const spudgpu_shader_pipeline_desc *desc,
    spudgpu_shader_pipeline *out_pipeline);

/**
 * @brief Destroys a graphics pipeline.
 *
 * @warning Command lists that bind the pipeline must have finished executing.
 *
 * @param[in] pipeline Pipeline to destroy.
 */
void spudgpu_destroy_shader_pipeline(spudgpu_shader_pipeline pipeline);

/**
 * @brief Binds a graphics pipeline for subsequent draws.
 *
 * Call inside a rendering pass, after spudgpu_cmd_begin_rendering().
 *
 * @param[in] cmd      Command list being recorded.
 * @param[in] pipeline Graphics pipeline to bind.
 */
void spudgpu_cmd_bind_pipeline(
    spudgpu_command_list cmd,
    spudgpu_shader_pipeline pipeline);

/**
 * @brief Pushes constant data to the shader stages declared in the pipeline
 * layout.
 *
 * Stage flags are derived from the pipeline's push constant ranges: any range
 * that overlaps [offset, offset+size) contributes its stages.
 *
 * Graphics and compute pipelines do not have a push constant block each:
 * after spudgpu_cmd_push_constants_compute(), what a draw reads from the
 * block is undefined until it is pushed again with this call.
 *
 * @param[in] cmd      Command list being recorded.
 * @param[in] pipeline The bound pipeline, used to look up the layout.
 * @param[in] offset   Byte offset within the push constant block.
 * @param[in] size     Number of bytes to update. Must be a non-zero multiple
 *                     of 4.
 * @param[in] data     Pointer to `size` bytes of data to push.
 *
 * @see spudgpu_cmd_push_constants_compute()
 */
void spudgpu_cmd_push_constants(
    spudgpu_command_list cmd,
    spudgpu_shader_pipeline pipeline,
    uint32_t offset,
    uint32_t size,
    const void *data);

/**
 * @brief Defines the normalized window transformation dimensions for rendering
 * output coordinates.
 *
 * Maps normalized device coordinates (NDC) ranging [-1, 1] horizontally and
 * vertically directly into a target render target screen pixel domain.
 *
 * Clip space is the same on every backend: NDC +1 in Y is the viewport's top
 * edge and -1 its bottom (+Y points up, as D3D12 and Metal have it), and
 * depth runs 0 to 1. The Vulkan backend keeps it by viewport
 * (spudgpu_cmd_set_viewports()), so the same shader and the same vertices
 * give the same picture, the same way up, on all three; no backend changes
 * a shader.
 */
/* Frozen: a value. It never changes; a new need gets a new type. */
typedef struct SPUDGPU_VIEWPORT {
	/** X-coordinate of the upper-left corner of the viewport region in pixels. */
	float x;

	/** Y-coordinate of the upper-left corner of the viewport region in pixels. */
	float y;

	/** Total width of the targeted viewport frame in pixels. */
	float width;

	/** Total height of the targeted viewport frame in pixels. */
	float height;

	/**
	 * Minimum depth boundary slice. Usually maps to `0.0f` (near clipping
	 * plane).
	 */
	float minDepth;

	/**
	 * Maximum depth boundary slice. Usually maps to `1.0f` (far clipping
	 * plane).
	 */
	float maxDepth;
} SPUDGPU_VIEWPORT;

/**
 * @brief Sets viewports for subsequent draws.
 *
 * A viewport's (x, y) is its top-left corner in framebuffer coordinates,
 * which run right and down from the target's top left, on every backend;
 * clip space's +Y is toward the viewport's top (spudgpu_shader_module_desc).
 * The Vulkan backend gives Vulkan y + height and -height to keep that.
 *
 * @param[in] cmd            Command list being recorded.
 * @param[in] first_viewport Index of the first viewport slot to set.
 * @param[in] viewport_count Number of elements in `viewports`.
 * @param[in] viewports      Array of viewports.
 */
void spudgpu_cmd_set_viewports(
    spudgpu_command_list cmd,
    uint32_t first_viewport,
    uint32_t viewport_count,
    const SPUDGPU_VIEWPORT *viewports);

/**
 * @brief Defines a dynamic screen space bounding box for rasterization
 * discarding.
 *
 * Pixels falling outside the scissor rectangle bounds are completely dropped
 * by the rasterizer, optimizing rendering execution for partial UI elements or
 * UI windows.
 */
/* Frozen: a value. It never changes; a new need gets a new type. */
typedef struct SPUDGPU_SCISSOR_RECT {
	/** Leftmost X coordinate of the scissor bounding box in pixels. */
	float x;

	/** Topmost Y coordinate of the scissor bounding box in pixels. */
	float y;

	/** Total horizontal layout width of the box in pixels. */
	float width;

	/** Total vertical layout height of the box in pixels. */
	float height;
} SPUDGPU_SCISSOR_RECT;

/**
 * @brief Sets scissor rectangles for subsequent draws.
 *
 * @param[in] cmd                Command list being recorded.
 * @param[in] first_scissor_rect Index of the first scissor slot to set.
 * @param[in] scissor_rect_count Number of elements in `scissor_rects`.
 * @param[in] scissor_rects      Array of scissor rectangles.
 */
void spudgpu_cmd_set_scissor_rects(
    spudgpu_command_list cmd,
    uint32_t first_scissor_rect,
    uint32_t scissor_rect_count,
    const SPUDGPU_SCISSOR_RECT *scissor_rects);

/**
 * @brief Binds vertex buffers to consecutive input assembler slots.
 *
 * @param[in] cmd          Command list being recorded.
 * @param[in] start_slot   First vertex buffer slot to set.
 * @param[in] view_count   Number of elements in `buffer_views`.
 * @param[in] buffer_views Array of buffer views holding vertex data.
 */
void spudgpu_cmd_set_vertex_buffers(
    spudgpu_command_list cmd,
    uint32_t start_slot,
    uint32_t view_count,
    spudgpu_buffer_view *buffer_views);

/**
 * @brief Binds the index buffer used by indexed draws.
 *
 * @param[in] cmd         Command list being recorded.
 * @param[in] buffer_view Buffer view holding index data.
 */
void spudgpu_cmd_set_index_buffer(
    spudgpu_command_list cmd,
    spudgpu_buffer_view buffer_view);

/**
 * @brief Records a non-indexed, non-instanced draw.
 *
 * @param[in] cmd                   Command list being recorded.
 * @param[in] vertex_count          Number of vertices to draw.
 * @param[in] start_vertex_location Index of the first vertex.
 */
void spudgpu_cmd_draw(
    spudgpu_command_list cmd,
    uint32_t vertex_count,
    uint32_t start_vertex_location);

/**
 * @brief Records an indexed, non-instanced draw.
 *
 * @param[in] cmd                  Command list being recorded.
 * @param[in] index_count          Number of indices to read from the bound
 *                                 index buffer.
 * @param[in] start_index_location Index of the first index to read.
 * @param[in] base_vertex_location Value added to each index before it selects
 *                                 a vertex.
 */
void spudgpu_cmd_draw_indexed(
    spudgpu_command_list cmd,
    uint32_t index_count,
    uint32_t start_index_location,
    int32_t base_vertex_location);

/**
 * @brief Records a non-indexed, instanced draw.
 *
 * @param[in] cmd                       Command list being recorded.
 * @param[in] vertex_count_per_instance Number of vertices to draw per
 *                                      instance.
 * @param[in] instance_count            Number of instances to draw.
 * @param[in] start_vertex_location     Index of the first vertex.
 * @param[in] start_instance_location   Value added to each instance index
 *                                      before per-instance data is read.
 */
void spudgpu_cmd_draw_instanced(
    spudgpu_command_list cmd,
    uint32_t vertex_count_per_instance,
    uint32_t instance_count,
    uint32_t start_vertex_location,
    uint32_t start_instance_location);

/**
 * @brief Records an indexed, instanced draw.
 *
 * @param[in] cmd                      Command list being recorded.
 * @param[in] index_count_per_instance Number of indices to read per instance.
 * @param[in] instance_count           Number of instances to draw.
 * @param[in] start_index_location     Index of the first index to read.
 * @param[in] base_vertex_location     Value added to each index before it
 *                                     selects a vertex.
 * @param[in] start_instance_location  Value added to each instance index
 *                                     before per-instance data is read.
 */
void spudgpu_cmd_draw_indexed_instanced(
    spudgpu_command_list cmd,
    uint32_t index_count_per_instance,
    uint32_t instance_count,
    uint32_t start_index_location,
    int32_t base_vertex_location,
    uint32_t start_instance_location);

/**
 * @brief Per-draw argument layout consumed by spudgpu_cmd_draw_indirect.
 *
 * Bit-for-bit identical to VkDrawIndirectCommand / D3D12_DRAW_ARGUMENTS /
 * MTLDrawPrimitivesIndirectArguments — a buffer of these can be written
 * directly by a compute shader and consumed by any of the three backends
 * with no repacking.
 */
/* Frozen: read by the GPU, so its layout is the GPU's. It never changes; a new need gets a new type. */
typedef struct spudgpu_draw_indirect_args {
	/** Number of vertices to draw. */
	uint32_t vertex_count;
	/** Number of instances to draw. */
	uint32_t instance_count;
	/** Index of the first vertex. */
	uint32_t first_vertex;
	/** Index of the first instance. 0 unless the device was created with
	 * SPUDGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE. */
	uint32_t first_instance;
} spudgpu_draw_indirect_args;

/**
 * @brief Per-draw argument layout consumed by
 * spudgpu_cmd_draw_indexed_indirect.
 *
 * Bit-for-bit identical to VkDrawIndexedIndirectCommand /
 * D3D12_DRAW_INDEXED_ARGUMENTS / MTLDrawIndexedPrimitivesIndirectArguments.
 */
/* Frozen: read by the GPU, so its layout is the GPU's. It never changes; a new need gets a new type. */
typedef struct spudgpu_draw_indexed_indirect_args {
	/** Number of indices to draw. */
	uint32_t index_count;
	/** Number of instances to draw. */
	uint32_t instance_count;
	/** Position of the first index in the index buffer. */
	uint32_t first_index;
	/** Added to each index before it selects a vertex. */
	int32_t base_vertex;
	/** Index of the first instance. 0 unless the device was created with
	 * SPUDGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE. */
	uint32_t first_instance;
} spudgpu_draw_indexed_indirect_args;

/**
 * @brief Records `draw_count` non-indexed draws, each reading its arguments
 * from a consecutive spudgpu_draw_indirect_args entry in a buffer.
 *
 * The buffer must be in SPUDGPU_RESOURCE_STATE_INDIRECT_ARGUMENT (see
 * spudgpu_cmd_pipeline_barrier()) and have been created with
 * SPUDGPU_BUFFER_USAGE_INDIRECT.
 *
 * Maps to: vkCmdDrawIndirect (Vulkan), ID3D12GraphicsCommandList::ExecuteIndirect
 * against an internal draw-only command signature (D3D12),
 * drawPrimitives:indirectBuffer:indirectBufferOffset: looped `draw_count` times
 * (Metal, which has no native multi-draw-from-buffer primitive).
 *
 * @param[in] cmd        Command list being recorded.
 * @param[in] buffer     Buffer holding `draw_count` consecutive
 *                       spudgpu_draw_indirect_args entries.
 * @param[in] offset     Byte offset of the first entry.
 * @param[in] draw_count Number of draws to issue, at most
 *                       SPUDGPU_ADAPTER_PROPERTIES::max_draw_indirect_count
 *                       on a device created with
 *                       SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT, else 1; with
 *                       more, nothing is recorded.
 * @param[in] stride     Byte stride between consecutive entries: a multiple
 *                       of 4, and at least
 *                       sizeof(spudgpu_draw_indirect_args); otherwise
 *                       nothing is recorded. Pass that size for a tightly
 *                       packed buffer; a larger stride skips the caller's
 *                       own data between entries, on every backend.
 */
void spudgpu_cmd_draw_indirect(
    spudgpu_command_list cmd,
    spudgpu_buffer buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride);

/**
 * @brief Indexed variant of spudgpu_cmd_draw_indirect().
 *
 * Reads spudgpu_draw_indexed_indirect_args entries and draws with the index
 * buffer bound by spudgpu_cmd_set_index_buffer().
 *
 * @param[in] cmd        Command list being recorded.
 * @param[in] buffer     Buffer holding `draw_count` consecutive
 *                       spudgpu_draw_indexed_indirect_args entries.
 * @param[in] offset     Byte offset of the first entry.
 * @param[in] draw_count Number of draws to issue, at most
 *                       SPUDGPU_ADAPTER_PROPERTIES::max_draw_indirect_count
 *                       on a device created with
 *                       SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT, else 1; with
 *                       more, nothing is recorded.
 * @param[in] stride     Byte stride between consecutive entries: a multiple
 *                       of 4, and at least
 *                       sizeof(spudgpu_draw_indexed_indirect_args);
 *                       otherwise nothing is recorded. Pass that size for a
 *                       tightly packed buffer; a larger stride skips the
 *                       caller's own data between entries, on every backend.
 */
void spudgpu_cmd_draw_indexed_indirect(
    spudgpu_command_list cmd,
    spudgpu_buffer buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride);

// ============================================================================
//  Mesh Shading
// ============================================================================

/**
 * @brief Geometry shaders and tessellation (control and evaluation
 * shaders, patches): 1 where the backend in this build implements them,
 * Vulkan and D3D12. 0 on Metal, which has no geometry stage and whose
 * tessellation is a compute kernel and a post-tessellation vertex function,
 * not these stages. Says nothing about a GPU: each pairs with its feature
 * bit (SPUDGPU_FEATURE_GEOMETRY_SHADER, SPUDGPU_FEATURE_TESSELLATION), which
 * a GPU of a backend where the macro is 0 never has. The desc fields
 * (spudgpu_shader_pipeline_desc::geometry_module, ::tess_control_module,
 * ::tess_eval_module, ::patch_control_points) are declared on every
 * backend; naming one without the device's bit is refused.
 */
#if SPUDGPU_COMPILE_VULKAN_API || SPUDGPU_COMPILE_D3D12_API
#define SPUDGPU_EXT_GEOMETRY_SHADER 1
/** @brief Tessellation (control and evaluation shaders, patches): 1 where
 * the backend in this build implements it, as SPUDGPU_EXT_GEOMETRY_SHADER
 * says. */
#define SPUDGPU_EXT_TESSELLATION 1
#else
#define SPUDGPU_EXT_GEOMETRY_SHADER 0
#define SPUDGPU_EXT_TESSELLATION 0
#endif

/**
 * @brief 1 when the compiled-in backend implements mesh shading and this
 * section is declared, 0 when it is compiled out.
 *
 * A mesh-shader pipeline replaces vertex fetch and input assembly with a
 * compute-like stage (spudgpu_shader_pipeline_desc::mesh_module) that emits
 * meshlet geometry itself. It has no vertex attributes or bindings and no
 * bound vertex or index buffer: the mesh shader reads what it needs from
 * descriptor-bound buffers by index, as a compute shader would.
 *
 * Maps to `VK_EXT_mesh_shader` (Vulkan), mesh-shader pipeline state objects
 * with Shader Model 6.5 and up (D3D12), and `MTLMeshRenderPipelineDescriptor`
 * in Metal 3 (Metal).
 *
 * 1 when the backend compiled into this build implements mesh shading, 0
 * otherwise; it is 1 on all three backends today. It says nothing about any
 * GPU: whether one has mesh shading is SPUDGPU_FEATURE_MESH_SHADING in its
 * adapter's supported_features, and whether a device may use it is that bit
 * in the device's features. A mesh-shader pipeline on a device created
 * without it is refused with SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED.
 */
#if SPUDGPU_COMPILE_VULKAN_API || SPUDGPU_COMPILE_D3D12_API || SPUDGPU_COMPILE_METAL_API
#define SPUDGPU_EXT_MESH_SHADING 1
#else
#define SPUDGPU_EXT_MESH_SHADING 0
#endif

#if SPUDGPU_EXT_MESH_SHADING

/**
 * @brief Mesh shading's limits, chained onto SPUDGPU_ADAPTER_PROPERTIES. All 0
 * when the adapter lacks SPUDGPU_FEATURE_MESH_SHADING.
 *
 * Vulkan: VkPhysicalDeviceMeshShaderPropertiesEXT. D3D12: the limits Shader
 * Model 6.5 guarantees on every mesh shader tier (it has no query). Metal:
 * the limits of Metal 3's mesh shaders on every Apple GPU.
 */
typedef struct SPUDGPU_MESH_SHADING_LIMITS {
	/** `type` is SPUD_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS; `next` is the rest
	 * of the adapter properties' chain. The caller sets the type; SpudGPU
	 * fills in the rest. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE_OUT base;
	/** Most vertices one mesh shader workgroup may emit. */
	uint32_t max_mesh_output_vertices;
	/** Most primitives one mesh shader workgroup may emit. */
	uint32_t max_mesh_output_primitives;
	/** Most invocations in one mesh shader workgroup. */
	uint32_t max_mesh_workgroup_invocations;
} SPUDGPU_MESH_SHADING_LIMITS;

/**
 * @brief Records a mesh-shader dispatch against the bound mesh-shader
 * pipeline.
 *
 * Callable only inside a rendering pass (between spudgpu_cmd_begin_rendering()
 * and spudgpu_cmd_end_rendering()), unlike spudgpu_cmd_dispatch(), which
 * targets a compute pipeline outside one. Does nothing if the bound pipeline
 * is not a mesh-shader pipeline, or if the device was created without
 * SPUDGPU_FEATURE_MESH_SHADING (and a debug build asserts then).
 *
 * Maps to: vkCmdDrawMeshTasksEXT (Vulkan), ID3D12GraphicsCommandList6::
 * DispatchMesh (D3D12), drawMeshThreadgroups:threadsPerObjectThreadgroup:
 * threadsPerMeshThreadgroup: with a nil object function (Metal, since
 * spudgpu_shader_pipeline_desc has no task_module-driven path yet).
 *
 * The group counts are in workgroups, not threads: they match the
 * [numthreads]/local_size the pipeline's mesh module declares, the same
 * convention spudgpu_cmd_dispatch() uses for compute.
 *
 * @param[in] cmd           Command list being recorded.
 * @param[in] group_count_x Number of mesh shader workgroups in X.
 * @param[in] group_count_y Number of mesh shader workgroups in Y.
 * @param[in] group_count_z Number of mesh shader workgroups in Z.
 */
void spudgpu_cmd_dispatch_mesh(
    spudgpu_command_list cmd,
    uint32_t group_count_x,
    uint32_t group_count_y,
    uint32_t group_count_z);

#endif // SPUDGPU_EXT_MESH_SHADING

// ============================================================================
//  Depth Bounds Test
// ============================================================================

/**
 * @brief 1 when the compiled-in backend implements the depth bounds test and
 * this section is declared, 0 when it is compiled out.
 *
 * The test discards a fragment by the value the depth attachment already
 * holds at that pixel, written by an earlier draw: it passes when that value
 * is inside a [min, max] range the caller sets. It is independent of the
 * ordinary depth test (spudgpu_shader_pipeline_desc::depth_test_enable and
 * ::depth_compare_op) and runs alongside it. The usual shape is a depth-only
 * pass that primes the attachment, then a pass whose visible region the
 * range clips.
 *
 * Maps to `vkCmdSetDepthBounds` with
 * `VkPipelineDepthStencilStateCreateInfo::depthBoundsTestEnable` (Vulkan),
 * `ID3D12GraphicsCommandList1::OMSetDepthBounds` with
 * `D3D12_DEPTH_STENCIL_DESC1::DepthBoundsTestEnable` (D3D12), and
 * `-[MTLRenderCommandEncoder setDepthTestMinBound:maxBound:]` (Metal).
 *
 * 1 when the backend compiled into this build implements the test, 0
 * otherwise; it is 1 on all three backends today. It says nothing about any
 * GPU: whether one has the test is SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST in its
 * adapter's supported_features (from `VkPhysicalDeviceFeatures::depthBounds`,
 * `D3D12_FEATURE_DATA_D3D12_OPTIONS2::DepthBoundsTestSupported`, or
 * `MTLGPUFamilyApple10`), and whether a device may use it is that bit in the
 * device's features.
 *
 * spudgpu_shader_pipeline_desc::depth_bounds_test_enable is declared whether
 * or not this macro is 1: a draw with a pipeline created with it is tested
 * against the range spudgpu_cmd_set_depth_bounds() set after the pipeline was
 * bound, and a draw with one created without it is not, on every backend.
 */
#if SPUDGPU_COMPILE_VULKAN_API || SPUDGPU_COMPILE_D3D12_API || SPUDGPU_COMPILE_METAL_API
#define SPUDGPU_EXT_DEPTH_BOUNDS_TEST 1
#else
#define SPUDGPU_EXT_DEPTH_BOUNDS_TEST 0
#endif

#if SPUDGPU_EXT_DEPTH_BOUNDS_TEST

/**
 * @brief Sets the [min, max] depth-attachment-value range the depth bounds
 * test clips against for subsequent draws.
 *
 * Two rules hold on every backend, whatever the graphics API underneath
 * keeps:
 *
 * - Call after spudgpu_cmd_bind_pipeline() of a pipeline created with
 *   spudgpu_shader_pipeline_desc::depth_bounds_test_enable, and before the
 *   draws it should clip. A pipeline bind leaves the range undefined until
 *   this is called again.
 * - A draw with a pipeline created without depth_bounds_test_enable is not
 *   tested, whatever range was set; calling this while one is bound has no
 *   effect on its draws.
 *
 * Does nothing on a device created without SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST
 * (and a debug build asserts then).
 *
 * Maps to: vkCmdSetDepthBounds (Vulkan), ID3D12GraphicsCommandList1::
 * OMSetDepthBounds (D3D12), [MTLRenderCommandEncoder setDepthTestMinBound:
 * maxBound:] (Metal).
 *
 * @param[in] cmd              Command list being recorded.
 * @param[in] min_depth_bounds Lower bound, in [0, 1], no greater than
 *                             `max_depth_bounds`.
 * @param[in] max_depth_bounds Upper bound, in [0, 1].
 */
void spudgpu_cmd_set_depth_bounds(
    spudgpu_command_list cmd,
    float min_depth_bounds,
    float max_depth_bounds);

#endif // SPUDGPU_EXT_DEPTH_BOUNDS_TEST

// ============================================================================
//  Ray queries: acceleration structures
// ============================================================================

/**
 * @brief 1 when the compiled-in backend implements acceleration structures
 * and inline ray queries and this section is declared, 0 when it is compiled
 * out. 1 on Vulkan, D3D12 and Metal.
 *
 * It says nothing about any GPU: whether one can build acceleration
 * structures and trace is SPUDGPU_FEATURE_RAY_QUERY in its adapter's
 * supported_features, and whether a device may is that bit in the device's
 * features. Every call below returns
 * SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED on a device created without it
 * (the void barrier call records nothing).
 *
 * **The shape.** An acceleration structure is a handle: a bottom level over
 * triangles or AABBs, or a top level over instances of bottom levels. The
 * caller asks how large one must be and how much scratch its build needs
 * (spudgpu_get_acceleration_structure_build_sizes()), creates it at that
 * size, committed or placed in a heap of its own, and records builds and
 * updates in a command list, many in one call, with scratch from a buffer of
 * its own. Instances are written in the device's own layout: the caller asks
 * for the layout (spudgpu_get_acceleration_structure_instance_layout()) and
 * either writes instances with spudgpu_write_acceleration_structure_instances()
 * or writes the bytes on the GPU itself. A top level is bound as a
 * SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE descriptor; shaders query it
 * with inline ray queries (SPIR-V `SPV_KHR_ray_query`, DXIL shader model 6.5
 * `RayQuery`, Metal `intersection_query`) in compute and fragment shaders.
 * SpudGPU keeps no pool, no cache and no scratch of its own, and chooses no
 * build flag: storage, scratch, instance and geometry buffers are all the
 * caller's, sized from the size query.
 *
 * **The rules every backend is held to**, whatever the graphics API
 * underneath allows:
 *
 * - Alignments are the device's, reported in SPUDGPU_RAY_QUERY_LIMITS: a
 *   build's scratch address, a placed structure's offset, and the instance
 *   buffer's offset (spudgpu_acceleration_structure_instance_layout::alignment).
 *   AABB data: offset and stride multiples of 8, stride at least 24. Vertex
 *   data: offset and stride multiples of 4, stride at least 12. Index data:
 *   offset a multiple of the index size. Every stride below 2^32.
 * - Every range is inside its buffer, and every count within its limit,
 *   checked with overflow-safe arithmetic before anything is recorded. What
 *   the GPU reads from the buffers is not checked: an index past its
 *   geometry's vertices, or an instance whose reference is not a live bottom
 *   level's, is undefined.
 * - A build's input buffers (vertices, indices, AABBs, instances) and its
 *   scratch live, unchanged, until the build's submission completes.
 * - A bottom level outlives every use of every top level whose last build
 *   named it, and every top level built over it is destroyed or rebuilt
 *   without it before it is destroyed.
 * - An update keeps the last build's geometry kinds, counts, flags and index
 *   layout; only positions, AABBs and instance contents change; the source
 *   was built with SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE. It is
 *   made in place (source = destination) or into another structure of at
 *   least the update's size.
 * - A structure is used in a trace only after a barrier from BUILD_WRITE to
 *   TRACE; a build, copy or size write that reads one (a top level over
 *   bottom levels, an update's source, a copy's source) only after a barrier
 *   from BUILD_WRITE to BUILD_READ
 *   (spudgpu_cmd_acceleration_structure_barrier()). Input buffers move to
 *   SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT and scratch to
 *   SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH with
 *   spudgpu_cmd_pipeline_barrier(), as other buffers move.
 * - Ray queries run in compute and fragment shaders. A module whose code
 *   queries is created only on a device with SPUDGPU_FEATURE_RAY_QUERY.
 *
 * Every structure is used like a buffer, by one caller at a time, through
 * the command lists that build it and the passes that read it (@ref
 * spud_threads "Threads"). Builds are recorded on the GPU, never run on the
 * host: Vulkan's deferred host operations are not used.
 *
 * Maps to `VK_KHR_acceleration_structure` and `VK_KHR_ray_query` (Vulkan),
 * DXR 1.1's inline ray tracing (D3D12), and Metal 3's acceleration
 * structures with indirect instances (Metal).
 */
#if SPUDGPU_COMPILE_VULKAN_API || SPUDGPU_COMPILE_D3D12_API || SPUDGPU_COMPILE_METAL_API
#define SPUDGPU_EXT_RAY_QUERY 1
#else
#define SPUDGPU_EXT_RAY_QUERY 0
#endif

#if SPUDGPU_EXT_RAY_QUERY

/** @brief A bottom or a top level. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_LEVEL;
enum {
	/** Not a level: a zeroed desc has not chosen, and is refused. */
	SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_NONE   = 0,
	/** Triangles or AABBs. */
	SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM = 1,
	/** Instances of bottom levels. */
	SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP    = 2
};

/**
 * @brief A build's flags: only those Vulkan, D3D12 and Metal all keep.
 *
 * None is set for the caller. Metal has no fast-trace flag (it is Metal's
 * default) and can always compact; the rules below hold on every backend
 * all the same. There is no minimize-memory flag, which Metal lacks.
 */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS;
enum {
	/** Trace fast at the cost of build time. Not with PREFER_FAST_BUILD. */
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE = 1u << 0,
	/** Build fast at the cost of trace time. Not with PREFER_FAST_TRACE. */
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD = 1u << 1,
	/** The structure may be updated (refitted) from this build:
	 * SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE. Metal's refit usage. */
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE      = 1u << 2,
	/** The structure's compacted size may be written and a compacted copy
	 * made from this build. */
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION  = 1u << 3
};

/** @brief A geometry's flags: the two all three APIs have. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS;
enum {
	/** Its hits are committed without the shader's say, unless an instance
	 * or the query forces otherwise. */
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE                  = 1u << 0,
	/** A primitive is a candidate at most once per query. */
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_CANDIDATES = 1u << 1
};

/** @brief What a geometry is. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_KIND;
enum {
	/** Not a kind: refused. */
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_NONE      = 0,
	/** Triangles: positions, and indices or none. */
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES = 1,
	/** Procedural: each box a candidate the shader decides. */
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_AABBS     = 2
};

/**
 * @brief One geometry of a bottom level.
 *
 * Geometry has no transform of its own: Vulkan's and D3D12's 3x4 transform
 * buffer and Metal's packed 4x3 one differ in layout, and instances carry
 * transforms on all three. Vertex positions are three 32-bit floats
 * (SPUDGPU_FORMAT_R32G32B32_FLOAT, the one format all three take on every
 * device).
 */
/* Frozen: an array element of spudgpu_acceleration_structure_inputs. It never changes; a new need gets a new type. */
typedef struct spudgpu_acceleration_structure_geometry {
	/** Triangles or AABBs. Every geometry of one bottom level is of one
	 * kind. */
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_KIND kind;
	/** SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS; no other bit. */
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS flags;
	/** Triangles: float3 positions. AABBs: boxes of six floats, (min x,
	 * min y, min z, max x, max y, max z). A buffer with
	 * SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT. Borrowed until
	 * the build's submission completes. */
	spudgpu_buffer data;
	/** Byte offset of the first vertex or box: a multiple of 4 for
	 * triangles, 8 for AABBs. */
	uint64_t data_offset;
	/** Bytes from one vertex or box to the next: a multiple of 4 and at
	 * least 12 for triangles, a multiple of 8 and at least 24 for AABBs;
	 * below 2^32. */
	uint64_t data_stride;
	/** Triangles: vertices (with no indices, a multiple of 3, the triangles
	 * being every three in order). AABBs: boxes, the primitives. Not 0. */
	uint32_t data_count;
	/** Triangles: SPUDGPU_FORMAT_R16_UINT or SPUDGPU_FORMAT_R32_UINT with an
	 * index buffer, as index buffer views name their width, or
	 * SPUDGPU_FORMAT_UNKNOWN for three vertices per triangle. AABBs:
	 * SPUDGPU_FORMAT_UNKNOWN. */
	SPUDGPU_FORMAT index_format;
	/** The index buffer (with
	 * SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT), or NULL with
	 * SPUDGPU_FORMAT_UNKNOWN. Every index is below #data_count. Borrowed until
	 * the build's submission completes. */
	spudgpu_buffer indices;
	/** Byte offset of the first index: a multiple of the index size. 0
	 * without indices. */
	uint64_t index_offset;
	/** Triangles, with indices: a non-zero multiple of 3, index_count / 3
	 * being the triangle count. 0 without indices. */
	uint32_t index_count;
} spudgpu_acceleration_structure_geometry;

/**
 * @brief What a build reads: a bottom level's geometries or a top level's
 * instances. The same inputs give the size query and the build.
 *
 * The fields of the other level are 0 and NULL.
 */
typedef struct spudgpu_acceleration_structure_inputs {
	/** `type` is SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS; `next` is
	 * NULL (nothing is chained onto this struct yet). Set by the caller. See
	 * SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** The level built: SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM or _TOP. */
	SPUDGPU_ACCELERATION_STRUCTURE_LEVEL level;
	/** SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS; at most one of the two
	 * PREFER flags, no other bit. */
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags;
	/** Bottom level: its geometries, all of one kind, #geometry_count of
	 * them. Borrowed for the call. */
	const spudgpu_acceleration_structure_geometry *geometries;
	/** Bottom level: 1 to SPUDGPU_RAY_QUERY_LIMITS::max_geometries, with at
	 * most SPUDGPU_RAY_QUERY_LIMITS::max_primitives triangles and boxes in
	 * all. */
	uint32_t geometry_count;
	/** Top level: instances in the device's layout
	 * (spudgpu_get_acceleration_structure_instance_layout()), in a buffer
	 * with SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT. Borrowed
	 * until the build's submission completes. */
	spudgpu_buffer instances;
	/** Top level: byte offset of the first instance, a multiple of the
	 * layout's alignment. */
	uint64_t instance_offset;
	/** Top level: 0 to SPUDGPU_RAY_QUERY_LIMITS::max_instances, the layout's
	 * size apart, inside #instances. */
	uint32_t instance_count;
	/** Top level: every bottom level its instances reference, each a bottom
	 * level, #bottom_level_count of them: at most the destination's
	 * spudgpu_acceleration_structure_desc::max_bottom_levels. Copied into
	 * the destination by the build (with no allocation); the size query
	 * reads only the levels. Metal makes them resident wherever the top
	 * level is built or traced, which nothing in the instance bytes could
	 * tell it; Vulkan and D3D12 check them and read them no further. */
	const spudgpu_acceleration_structure *bottom_levels;
	/** Top level: the number of entries of #bottom_levels. */
	uint32_t bottom_level_count;
} spudgpu_acceleration_structure_inputs;

/** @brief What a structure and its builds need. */
typedef struct spudgpu_acceleration_structure_sizes {
	/** `type` is SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES; `next` is
	 * NULL. The caller sets the type; SpudGPU fills in the rest. See
	 * SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE_OUT base;
	/** Bytes the structure is created with, at least. */
	uint64_t storage_size;
	/** Scratch bytes a build (SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD)
	 * needs. */
	uint64_t build_scratch_size;
	/** Scratch bytes an update needs; 0 without
	 * SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE. */
	uint64_t update_scratch_size;
} spudgpu_acceleration_structure_sizes;

/**
 * @brief Reports the sizes a build of `inputs` needs.
 *
 * Records nothing and creates nothing. May allocate temporary memory
 * through the device's allocator, freed before it returns. Maps to
 * vkGetAccelerationStructureBuildSizesKHR (Vulkan),
 * GetRaytracingAccelerationStructurePrebuildInfo (D3D12) and
 * accelerationStructureSizesWithDescriptor: (Metal).
 *
 * @param[in]  device    Device the structure would be built on.
 * @param[in]  inputs    What the build would read.
 * @param[out] out_sizes Receives the sizes; untouched on failure.
 *
 * @return One of these, checked in this order on every backend:
 * - SPUD_SUCCESS `out_sizes` was filled in.
 * - SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL (or, in Debug,
 *         destroyed).
 * - SPUDRESULT_NULL_DESC `inputs` is NULL.
 * - SPUDRESULT_STRUCT_WRONG_TYPE / SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN
 *         `inputs->base` is wrong.
 * - SPUDRESULT_NULL_OUTPUT_PARAMETER `out_sizes` is NULL.
 * - SPUDRESULT_STRUCT_WRONG_TYPE / SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN
 *         `out_sizes->base` is wrong.
 * - SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED The device wasn't
 *         created with SPUDGPU_FEATURE_RAY_QUERY.
 * - (the inputs' checks, in field order:)
 * - SPUDRESULT_DESC_INVALID_PARAMETERS `inputs->level` is not a
 *         level; `inputs->flags` has an unknown bit or both PREFER flags.
 * - (a bottom level, then each geometry in order:)
 * - SPUDRESULT_DESC_INVALID_PARAMETERS The top level's fields are not
 *         0 and NULL.
 * - SPUDRESULT_NULL_DESC `geometries` is NULL.
 * - SPUDRESULT_ZERO_SIZE `geometry_count` is 0.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT `geometry_count` is
 *         past max_geometries.
 * - SPUDRESULT_DESC_INVALID_PARAMETERS A geometry's kind is not a
 *         kind, or not the first geometry's; its flags have an unknown bit.
 * - SPUDRESULT_GPU_INVALID_BUFFER `data` is NULL (or destroyed).
 * - SPUDRESULT_GPU_INVALID_BUFFER_USAGE `data` lacks
 *         SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT `data_offset` or
 *         `data_stride` breaks the rule above.
 * - SPUDRESULT_ZERO_SIZE `data_count` is 0.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE The data runs past
 *         the end of its buffer.
 * - SPUDRESULT_DESC_INVALID_PARAMETERS `index_format` is not one the
 *         kind takes; `indices` is not NULL without an index format.
 * - SPUDRESULT_GPU_INVALID_BUFFER `indices` is NULL (or destroyed)
 *         with an index format.
 * - SPUDRESULT_GPU_INVALID_BUFFER_USAGE `indices` lacks
 *         SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT `index_offset` is
 *         not a multiple of the index size.
 * - SPUDRESULT_ZERO_SIZE `index_count` is 0 with indices.
 * - SPUDRESULT_DESC_INVALID_PARAMETERS `index_count` (with indices)
 *         or `data_count` (triangles without) is not a multiple of 3, or
 *         `index_offset` or `index_count` is not 0 without indices.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE The indices run past
 *         the end of their buffer.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT The primitives so far
 *         are past max_primitives.
 * - (a top level:)
 * - SPUDRESULT_DESC_INVALID_PARAMETERS The bottom level's fields are
 *         not 0 and NULL.
 * - SPUDRESULT_GPU_INVALID_BUFFER `instances` is NULL (or destroyed).
 * - SPUDRESULT_GPU_INVALID_BUFFER_USAGE `instances` lacks
 *         SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT `instance_offset`
 *         is not a multiple of the instance layout's alignment.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT `instance_count` is
 *         past max_instances.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE The instances run
 *         past the end of their buffer.
 * - SPUDRESULT_NULL_DESC `bottom_levels` is NULL with a count.
 * - SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE An entry of
 *         `bottom_levels` is NULL (or destroyed) or not a bottom level.
 * - SPUDRESULT_OUT_OF_MEMORY The temporary memory couldn't be
 *         allocated.
 * - SPUDRESULT_API_SPECIFIC_FAILURE if the graphics API fails.
 */
SPUDRESULT spudgpu_get_acceleration_structure_build_sizes(
    spudgpu_device device,
    const spudgpu_acceleration_structure_inputs *inputs,
    spudgpu_acceleration_structure_sizes *out_sizes);

/** @brief Describes an acceleration structure to create. */
typedef struct spudgpu_acceleration_structure_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC; `next` is
	 * NULL. Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM or _TOP: what every build
	 * into it builds. */
	SPUDGPU_ACCELERATION_STRUCTURE_LEVEL level;
	/** Bytes, not 0: a size query's storage_size, or a compacted size read
	 * back (spudgpu_cmd_write_acceleration_structure_compacted_sizes()). */
	uint64_t size;
	/** Top level: the most bottom levels one build may name
	 * (spudgpu_acceleration_structure_inputs::bottom_levels); the list is
	 * kept with the structure, made with it. 0 for a bottom level. */
	uint32_t max_bottom_levels;
	/** Whether a compacted size may be written for it. Vulkan makes the one
	 * query that holds the size with it, freed with it. */
	bool allow_compaction;
	/** A name for diagnostics, or NULL. Copied, in every build; see
	 * spud_debug_name_set(). */
	const char *debug_name;
} spudgpu_acceleration_structure_desc;

/**
 * @brief Creates an acceleration structure with storage of its own.
 *
 * Its contents are undefined until a build or copy writes it. Allocates the
 * handle, a top level's list of max_bottom_levels entries, and the debug
 * name, through the device's allocator. Vulkan: a buffer of the size, its
 * own memory, and the `VkAccelerationStructureKHR` over it. D3D12: a
 * committed buffer in `RAYTRACING_ACCELERATION_STRUCTURE` state. Metal:
 * newAccelerationStructureWithSize:.
 *
 * @param[in]  device        Device to create it on.
 * @param[in]  desc          Its level, size, limit and name.
 * @param[out] out_structure Receives the structure; untouched on failure.
 *
 * @return One of these, checked in this order on every backend:
 * - SPUD_SUCCESS The structure was created.
 * - SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL (or, in Debug,
 *         destroyed).
 * - SPUDRESULT_NULL_DESC `desc` is NULL.
 * - SPUDRESULT_STRUCT_WRONG_TYPE / SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN
 *         `desc->base` is wrong.
 * - SPUDRESULT_NULL_OUTPUT_PARAMETER `out_structure` is NULL.
 * - SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED The device wasn't
 *         created with SPUDGPU_FEATURE_RAY_QUERY.
 * - SPUDRESULT_DESC_INVALID_PARAMETERS `desc->level` is not a level,
 *         or a bottom level has a max_bottom_levels.
 * - SPUDRESULT_ZERO_SIZE `desc->size` is 0.
 * - SPUDRESULT_OUT_OF_MEMORY The handle or list couldn't be
 *         allocated.
 * - SPUDRESULT_API_SPECIFIC_FAILURE The graphics API fails (the
 *         device having no room for it is the usual one).
 * - SPUDRESULT_OUT_OF_MEMORY The name couldn't be copied.
 *
 * @see spudgpu_destroy_acceleration_structure()
 */
SPUDRESULT spudgpu_create_acceleration_structure(
    spudgpu_device device,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure);

/**
 * @brief Creates an acceleration structure placed at `offset` in `heap`, for
 * a caller whose allocator places everything.
 *
 * The structure covers `desc->size` rounded up to
 * SPUDGPU_RAY_QUERY_LIMITS::placement_alignment bytes from `offset`.
 * SpudGPU checks no overlap, as for placed buffers: two live resources over
 * one byte are undefined. Destroying it frees nothing to the device. Vulkan:
 * a buffer bound at the offset of the heap's memory. D3D12: a placed buffer.
 * Metal: newAccelerationStructureWithSize:offset:.
 *
 * @param[in]  heap          A heap of
 *                           SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES.
 * @param[in]  offset        Byte offset in the heap: a multiple of
 *                           placement_alignment.
 * @param[in]  desc          As for spudgpu_create_acceleration_structure().
 * @param[out] out_structure Receives the structure; untouched on failure.
 *
 * @return One of these, checked in this order on every backend:
 * - SPUD_SUCCESS The structure was created.
 * - SPUDRESULT_GPU_INVALID_HEAP `heap` is NULL (or, in Debug,
 *         destroyed).
 * - (each result of spudgpu_create_acceleration_structure() from
 *         SPUDRESULT_NULL_DESC on, in its order, for the heap's device.)
 * - SPUDRESULT_GPU_HEAP_MISMATCH The heap is not of
 *         SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES.
 * - SPUDRESULT_GPU_PLACEMENT_MISALIGNED `offset` is not a multiple of
 *         placement_alignment.
 * - SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE The range it covers runs
 *         past the end of the heap.
 * - SPUDRESULT_OUT_OF_MEMORY The handle or its list couldn't be
 *         allocated.
 * - SPUDRESULT_GPU_PLACEMENT_MISALIGNED The graphics API asks a larger
 *         alignment of this structure than placement_alignment, and `offset`
 *         is not a multiple of it (none has, on the GPUs tested).
 * - SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE The graphics API's own size
 *         for this structure is larger than `desc->size` rounded up to
 *         placement_alignment, the range the caller placed it over (none
 *         has, on the GPUs tested): it is refused rather than cover bytes
 *         past that range.
 * - SPUDRESULT_API_SPECIFIC_FAILURE The graphics API fails.
 * - SPUDRESULT_OUT_OF_MEMORY The name couldn't be copied.
 */
SPUDRESULT spudgpu_create_placed_acceleration_structure(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure);

/**
 * @brief Destroys an acceleration structure.
 *
 * The caller has waited for every submission that builds, copies or traces
 * it, and every top level built over it is destroyed or rebuilt without it
 * first. A placed structure's bytes stay the heap's.
 *
 * @param[in] structure Structure to destroy. NULL does nothing.
 */
void spudgpu_destroy_acceleration_structure(spudgpu_acceleration_structure structure);

/**
 * @brief Reads a bottom level's reference, what an instance names it by.
 *
 * Vulkan its device address (vkGetAccelerationStructureDeviceAddressKHR),
 * D3D12 its GPU virtual address, Metal its `gpuResourceID`. Constant for the
 * structure's life, built or not, so a caller writing instances on the GPU
 * reads it once.
 *
 * @param[in]  bottom_level  A bottom level.
 * @param[out] out_reference Receives the reference.
 *
 * @retval SPUD_SUCCESS `out_reference` was written.
 * @retval SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE `bottom_level` is
 *         NULL (or, in Debug, destroyed) or a top level.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_reference` is NULL.
 */
SPUDRESULT spudgpu_get_acceleration_structure_reference(
    spudgpu_acceleration_structure bottom_level,
    uint64_t *out_reference);

/** @brief How the device lays an instance out. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT;
enum {
	/** Not a layout. */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_NONE           = 0,
	/** 64 bytes (Vulkan, D3D12): a row-major 3x4 float transform (48
	 * bytes); a 32-bit word of the 24-bit user index (low bits) and the
	 * 8-bit mask; a 32-bit word of the 24-bit table offset (low bits) and 8
	 * bits of flags; the 64-bit reference. */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64    = 1,
	/** 72 bytes (Metal's indirect instance descriptor): a column-major
	 * packed 4x3 float transform (48 bytes, four columns of three floats);
	 * 32-bit options (the flags), mask, table offset and user index; the
	 * 64-bit reference. */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72 = 2
};

/** @brief An instance's layout on a device. */
/* Frozen: a value read back. It never changes; a new need gets a new type. */
typedef struct spudgpu_acceleration_structure_instance_layout {
	/** Which layout. */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT kind;
	/** Bytes per instance, and the stride between them. */
	uint32_t size;
	/** What the instance buffer's offset is a multiple of
	 * (spudgpu_acceleration_structure_inputs::instance_offset). */
	uint32_t alignment;
} spudgpu_acceleration_structure_instance_layout;

/**
 * @brief Reads how `device` lays an instance out, for a caller that writes
 * instance bytes on the GPU, one writer per layout kind.
 *
 * @param[in]  device     Device to ask.
 * @param[out] out_layout Receives the layout.
 *
 * Checked in this order, on every backend:
 * @retval SPUD_SUCCESS `out_layout` was written.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL (or, in Debug,
 *         destroyed).
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_layout` is NULL.
 * @retval SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED The device wasn't
 *         created with SPUDGPU_FEATURE_RAY_QUERY.
 */
SPUDRESULT spudgpu_get_acceleration_structure_instance_layout(
    spudgpu_device device,
    spudgpu_acceleration_structure_instance_layout *out_layout);

/** @brief The flags an instance has on all three APIs. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FLAGS;
enum {
	/** Its triangles are not culled by facing. */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_DISABLE_TRIANGLE_CULLING = 1u << 0,
	/** Its triangles' front faces are counter-clockwise. */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FRONT_COUNTERCLOCKWISE   = 1u << 1,
	/** Every geometry of it is opaque. Not with FORCE_NON_OPAQUE. */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE             = 1u << 2,
	/** No geometry of it is opaque. Not with FORCE_OPAQUE. */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_NON_OPAQUE         = 1u << 3
};

/** @brief One instance, portable: what
 * spudgpu_write_acceleration_structure_instances() writes in the device's
 * layout. The narrowest contract: an 8-bit mask, a 24-bit user index and
 * table offset, and the flags all three share. */
/* Frozen: an array element of spudgpu_write_acceleration_structure_instances(). It never changes; a new need gets a new type. */
typedef struct spudgpu_acceleration_structure_instance {
	/** Object to world, row-major 3x4: rows of (x, y, z, translation). */
	float transform[12];
	/** Below 2^24: the shader's committed instance id
	 * (`CommittedInstanceID`). */
	uint32_t user_index;
	/** Tested against a query's cull mask: the instance is skipped where
	 * the two have no bit in common. */
	uint8_t mask;
	/** Below 2^24. Unused by ray queries; kept for a ray tracing pipeline,
	 * which this extension doesn't have. */
	uint32_t table_offset;
	/** SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FLAGS; no other bit. */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FLAGS flags;
	/** spudgpu_get_acceleration_structure_reference() of its bottom level. */
	uint64_t bottom_level_reference;
} spudgpu_acceleration_structure_instance;

/**
 * @brief Writes `count` instances in `device`'s layout into `out_bytes`: a
 * CPU translation, with no state and nothing recorded.
 *
 * Batch shaped: a caller writes a top level's instances in one call, into a
 * mapped instance buffer or any memory it copies from, never one call per
 * instance. Every instance is checked before the first byte is written.
 *
 * @param[in]  device    Device whose layout is written.
 * @param[in]  instances `count` instances. Borrowed for the call.
 * @param[in]  count     1 or more.
 * @param[out] out_bytes `count` x the layout's size bytes, at the layout's
 *                       alignment; written whole, or not at all on failure.
 *
 * Checked in this order, on every backend:
 * @retval SPUD_SUCCESS Every instance was written.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL (or, in Debug,
 *         destroyed).
 * @retval SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED The device wasn't
 *         created with SPUDGPU_FEATURE_RAY_QUERY.
 * @retval SPUDRESULT_NULL_DESC `instances` is NULL.
 * @retval SPUDRESULT_ZERO_SIZE `count` is 0.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_bytes` is NULL.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS An instance, the first in
 *         order, has a user index or table offset of 2^24 or more, an
 *         unknown flag, or both opacity flags.
 */
SPUDRESULT spudgpu_write_acceleration_structure_instances(
    spudgpu_device device,
    const spudgpu_acceleration_structure_instance *instances,
    uint32_t count,
    void *out_bytes);

/** @brief Build or update. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE;
enum {
	/** Built from nothing; `source` is NULL. */
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD  = 1,
	/** Updated (refitted) from `source`, last built with ALLOW_UPDATE and
	 * the same layout of inputs. */
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE = 2
};

/** @brief One build in spudgpu_cmd_build_acceleration_structures(). */
typedef struct spudgpu_acceleration_structure_build {
	/** `type` is SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD; `next` is
	 * NULL. Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** Build or update. */
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE mode;
	/** What it reads. Borrowed for the call; the buffers it names until
	 * the build's submission completes. */
	const spudgpu_acceleration_structure_inputs *inputs;
	/** UPDATE: the structure updated from, of the inputs' level (may be
	 * #destination). BUILD: NULL. */
	spudgpu_acceleration_structure source;
	/** The structure written, of the inputs' level, at least storage_size
	 * bytes. */
	spudgpu_acceleration_structure destination;
	/** Scratch: a SPUDGPU_BUFFER_USAGE_STORAGE buffer, in
	 * SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH. */
	spudgpu_buffer scratch;
	/** Byte offset of the scratch range: its address a multiple of
	 * scratch_offset_alignment, the mode's scratch size inside the buffer. */
	uint64_t scratch_offset;
} spudgpu_acceleration_structure_build;

/**
 * @brief Records `count` builds and updates.
 *
 * Builds in one call may run at once: no two of them write the same
 * structure or scratch range, and none reads what another writes. Every
 * build is checked before the first is recorded, and a refused call records
 * nothing. A top level's build copies its bottom_levels into the
 * destination as it is recorded. Vulkan one
 * vkCmdBuildAccelerationStructuresKHR; D3D12 one
 * BuildRaytracingAccelerationStructure each; Metal one acceleration
 * structure command encoder.
 *
 * After it, the list's compute pipeline, descriptor sets and push constants
 * are undefined until bound again (Metal ends the compute encoder they were
 * set on); a dispatch recorded next binds them first. The same holds after
 * spudgpu_cmd_copy_acceleration_structure() and
 * spudgpu_cmd_write_acceleration_structure_compacted_sizes().
 *
 * A bottom level built with ALLOW_UPDATE keeps each geometry's kind, flags,
 * counts and index format on its handle, for an update to compare exactly:
 * an array grown through the structure's allocator the first time a build
 * or copy records more geometries than it holds, and never again for the
 * same layout.
 *
 * @param[in] cmd    Command list being recorded, of a direct or compute
 *                   queue, outside a rendering pass.
 * @param[in] builds `count` builds. Borrowed for the call.
 * @param[in] count  1 or more.
 *
 * @return One of these, checked in this order on every backend:
 * - SPUD_SUCCESS Every build was recorded.
 * - SPUDRESULT_GPU_INVALID_COMMAND_LIST `cmd` is NULL (or, in Debug,
 *         destroyed).
 * - SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED The device wasn't
 *         created with SPUDGPU_FEATURE_RAY_QUERY.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE `cmd` is
 *         of a copy queue or a bundle (card S26).
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING `cmd` is
 *         between spudgpu_cmd_begin_rendering() and
 *         spudgpu_cmd_end_rendering() (card S26).
 * - SPUDRESULT_NULL_DESC `builds` is NULL.
 * - SPUDRESULT_ZERO_SIZE `count` is 0.
 * - (then each build in order, its fields in order:)
 * - SPUDRESULT_STRUCT_WRONG_TYPE / SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN
 *         its `base` is wrong.
 * - SPUDRESULT_DESC_INVALID_PARAMETERS `mode` is not a mode.
 * - SPUDRESULT_NULL_DESC `inputs` is NULL.
 * - (the base and every check of
 *         spudgpu_get_acceleration_structure_build_sizes() on `inputs`, in
 *         its order.)
 * - SPUDRESULT_OUT_OF_MEMORY The command list's arrays couldn't grow
 *         to ask this build's sizes and record the builds so far (Vulkan
 *         and D3D12; Metal keeps none).
 * - SPUDRESULT_DESC_INVALID_PARAMETERS `source` is not NULL for a
 *         BUILD.
 * - SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE `source` is NULL
 *         (or destroyed) or not of the inputs' level for an UPDATE.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED An UPDATE's
 *         source was never built, was last built without ALLOW_UPDATE, or
 *         with other flags or another layout of inputs.
 * - SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE `destination` is
 *         NULL (or destroyed) or not of the inputs' level.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL `destination` is
 *         smaller than storage_size.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT A top level names
 *         more bottom levels than the destination's max_bottom_levels.
 * - SPUDRESULT_GPU_INVALID_BUFFER `scratch` is NULL (or destroyed).
 * - SPUDRESULT_GPU_INVALID_BUFFER_USAGE `scratch` lacks
 *         SPUDGPU_BUFFER_USAGE_STORAGE.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT The scratch
 *         address is not a multiple of scratch_offset_alignment.
 * - SPUDRESULT_GPU_ACCELERATION_STRUCTURE_SCRATCH_TOO_SMALL The
 *         scratch range is smaller than the mode's scratch size.
 * - SPUDRESULT_OUT_OF_MEMORY A bottom level built with ALLOW_UPDATE:
 *         the destination's kept layout couldn't grow to its geometries.
 *
 * Once every build has passed, the call records them and can't fail.
 */
SPUDRESULT spudgpu_cmd_build_acceleration_structures(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure_build *builds,
    uint32_t count);

/** @brief What a copy makes. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE;
enum {
	/** The same structure, into one of at least the source's size. */
	SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE   = 1,
	/** A compacted copy, into one of at least the compacted size the
	 * source's last build has (read back, not checked); the source built with
	 * ALLOW_COMPACTION. */
	SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT = 2
};

/**
 * @brief Records a copy of one acceleration structure into another.
 *
 * The destination takes the source's last build as its own: it can be
 * updated, compacted or copied as the source could. A top level's
 * bottom-level list is copied with it. Vulkan
 * vkCmdCopyAccelerationStructureKHR; D3D12
 * CopyRaytracingAccelerationStructure; Metal copy or copyAndCompact.
 *
 * After it, the list's compute pipeline, descriptor sets and push constants
 * are undefined until bound again, as after
 * spudgpu_cmd_build_acceleration_structures().
 *
 * @param[in] cmd         Command list being recorded, of a direct or
 *                        compute queue, outside a rendering pass.
 * @param[in] source      The structure read, built, after a barrier to
 *                        BUILD_READ.
 * @param[in] destination The structure written, of the source's level.
 * @param[in] mode        Clone or compact.
 *
 * Checked in this order, on every backend:
 * @retval SPUD_SUCCESS The copy was recorded.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_LIST `cmd` is NULL (or, in Debug,
 *         destroyed).
 * @retval SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED The device wasn't
 *         created with SPUDGPU_FEATURE_RAY_QUERY.
 * @retval SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE `cmd` is
 *         of a copy queue or a bundle (card S26).
 * @retval SPUDRESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING `cmd` is
 *         between spudgpu_cmd_begin_rendering() and
 *         spudgpu_cmd_end_rendering() (card S26).
 * @retval SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE `source` is NULL (or
 *         destroyed) or has no build recorded; `destination` is NULL (or
 *         destroyed), `source` itself, or of another level.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS `mode` is not a mode.
 * @retval SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED A
 *         COMPACT whose source was last built without ALLOW_COMPACTION.
 * @retval SPUDRESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL A CLONE into a
 *         destination smaller than the source.
 * @retval SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT A top level's source
 *         names more bottom levels than the destination's
 *         max_bottom_levels.
 * @retval SPUDRESULT_OUT_OF_MEMORY The source keeps a layout (a bottom
 *         level built with ALLOW_UPDATE) and the destination's couldn't grow
 *         to it.
 */
SPUDRESULT spudgpu_cmd_copy_acceleration_structure(
    spudgpu_command_list cmd,
    spudgpu_acceleration_structure source,
    spudgpu_acceleration_structure destination,
    SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE mode);

/**
 * @brief Records writing each structure's compacted size, a uint64_t, into
 * `buffer` from `offset`, in the order of `structures`.
 *
 * The caller reads the sizes back and creates each compacted structure at
 * its size, then copies into it (SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT).
 * D3D12 EmitRaytracingAccelerationStructurePostbuildInfo; Metal
 * writeCompactedAccelerationStructureSize:toBuffer:offset:sizeDataType:;
 * Vulkan the structure's own query, reset, written and copied into the
 * buffer.
 *
 * After it, the list's compute pipeline, descriptor sets and push constants
 * are undefined until bound again, as after
 * spudgpu_cmd_build_acceleration_structures().
 *
 * @param[in] cmd        Command list being recorded, of a direct or
 *                       compute queue, outside a rendering pass.
 * @param[in] structures `count` structures, each created with
 *                       allow_compaction and last built with
 *                       ALLOW_COMPACTION, after a barrier to BUILD_READ.
 *                       Borrowed for the call.
 * @param[in] count      1 or more.
 * @param[in] buffer     A buffer with SPUDGPU_BUFFER_USAGE_STORAGE and
 *                       SPUDGPU_BUFFER_USAGE_TRANSFER_DST, in
 *                       SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SIZE_DEST.
 * @param[in] offset     Byte offset: a multiple of 8, with `count` x 8 bytes
 *                       inside the buffer.
 *
 * Checked in this order, on every backend:
 * @retval SPUD_SUCCESS The writes were recorded.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_LIST `cmd` is NULL (or, in Debug,
 *         destroyed).
 * @retval SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED The device wasn't
 *         created with SPUDGPU_FEATURE_RAY_QUERY.
 * @retval SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE `cmd` is
 *         of a copy queue or a bundle (card S26).
 * @retval SPUDRESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING `cmd` is
 *         between spudgpu_cmd_begin_rendering() and
 *         spudgpu_cmd_end_rendering() (card S26).
 * @retval SPUDRESULT_NULL_DESC `structures` is NULL.
 * @retval SPUDRESULT_ZERO_SIZE `count` is 0.
 * @retval SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE An entry is NULL (or
 *         destroyed).
 * @retval SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED An entry
 *         wasn't created with allow_compaction or last built with
 *         ALLOW_COMPACTION.
 * @retval SPUDRESULT_GPU_INVALID_BUFFER `buffer` is NULL (or destroyed).
 * @retval SPUDRESULT_GPU_INVALID_BUFFER_USAGE `buffer` lacks STORAGE or
 *         TRANSFER_DST.
 * @retval SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT `offset` is not a
 *         multiple of 8.
 * @retval SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE The sizes run past
 *         the end of the buffer.
 * @retval SPUDRESULT_OUT_OF_MEMORY The command list's array of addresses
 *         couldn't grow to `count` (D3D12; Vulkan and Metal keep none).
 */
SPUDRESULT spudgpu_cmd_write_acceleration_structure_compacted_sizes(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure *structures,
    uint32_t count,
    spudgpu_buffer buffer,
    uint64_t offset);

/** @brief How a structure is being used, for its barriers. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_ACCESS;
enum {
	/** Not yet used in this submission. A barrier out of it is ordered
	 * after everything earlier submissions on the queue did with the
	 * structure, as a buffer's out of SPUDGPU_RESOURCE_STATE_COMMON is. */
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE        = 0,
	/** Built, updated, or copied into. */
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE = 1,
	/** Read by a build (a top level's bottom levels, an update's source), a
	 * copy or a size write. */
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ  = 2,
	/** Read by ray queries in compute and fragment shaders. */
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE       = 3
};

/** @brief One structure's barrier. */
typedef struct spudgpu_acceleration_structure_barrier {
	/** `type` is SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER; `next`
	 * is NULL. Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** The structure. */
	spudgpu_acceleration_structure structure;
	/** How it was used before the barrier. */
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS access_before;
	/** How it is used after it. */
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS access_after;
} spudgpu_acceleration_structure_barrier;

/**
 * @brief Records barriers between structures' accesses.
 *
 * Kept apart from spudgpu_cmd_pipeline_barrier(): a structure moves between
 * accesses no resource state names. Vulkan one memory barrier from the
 * stages before (the build stage, or compute and fragment shaders) to the
 * stages after; D3D12 a UAV barrier on each structure's buffer; Metal
 * nothing beyond its own tracking. Void, as spudgpu_cmd_pipeline_barrier()
 * is: nothing is recorded if `cmd` is NULL (or destroyed), the device wasn't
 * created with SPUDGPU_FEATURE_RAY_QUERY, `barriers` is NULL with a count,
 * or an element has a `base` other than
 * {SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER, NULL}, a NULL (or
 * destroyed) structure, or an access that is not one.
 *
 * @param[in] cmd      Command list being recorded, outside a rendering
 *                     pass.
 * @param[in] barriers `count` barriers. Borrowed for the call.
 * @param[in] count    Number of elements of `barriers`; 0 records nothing.
 */
void spudgpu_cmd_acceleration_structure_barrier(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure_barrier *barriers,
    uint32_t count);

#endif // SPUDGPU_EXT_RAY_QUERY

/**
 * @brief Complete configuration descriptor for creating a compute shader
 * pipeline.
 *
 * Simpler than the graphics pipeline desc — no vertex input, rasterizer,
 * blend state, or render pass. Just a single compute stage and its layout.
 */
typedef struct spudgpu_compute_pipeline_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** Compiled compute shader module. Required. */
	spudgpu_shader_module compute_module;

	// -----------------------------------------------------------------------
	// Pipeline layout
	// -----------------------------------------------------------------------

	/**
	 * Opaque spudgpu_descriptor_set_layout handles, from
	 * spudgpu_create_descriptor_set_layout() - declared as void* here
	 * rather than the proper typedef, but each backend casts entries back
	 * to its own internal descriptor-set-layout type, the same as every
	 * other opaque handle in this header. Not a raw native handle of any
	 * specific backend's API.
	 */
	void *descriptor_set_layouts[SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	/** Number of valid entries in #descriptor_set_layouts. */
	uint32_t descriptor_set_layout_count;

	/**
	 * The ranges of the push constant block the pipeline's stages read. Only
	 * the first #push_constant_range_count are read.
	 */
	spudgpu_push_constant_range_desc push_constant_ranges[SPUDGPU_MAX_PUSH_CONSTANT_RANGES];
	/** Number of valid entries in #push_constant_ranges. */
	uint32_t push_constant_range_count;

	/** @brief A name for diagnostics, or NULL. Copied, in every build; see
	 * spud_debug_name_set(). */
	const char *debug_name;
} spudgpu_compute_pipeline_desc;

/**
 * @brief Creates a compute pipeline.
 *
 * @param[in]  device       Device to create the pipeline on.
 * @param[in]  desc         Pipeline configuration.
 * @param[out] out_pipeline Receives the new pipeline on success.
 *
 * @retval SPUD_SUCCESS The pipeline was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_pipeline` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_COMPUTE_MODULE `desc->compute_module` is
 *         NULL.
 * @retval SPUDRESULT_GPU_SHADER_LAYOUT_MISMATCH On Metal, the module's
 *         argument buffer for a set it reads isn't that set's layout
 *         (spudgpu_shader_module_desc, "Every binding of a set"): a binding
 *         of the layout left out, one it doesn't hold, or one of another
 *         type or count. Checked for each of its sets. Not returned by
 *         Vulkan or D3D12.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 *
 * @see spudgpu_destroy_compute_pipeline()
 */
SPUDRESULT spudgpu_create_compute_pipeline(
    spudgpu_device device,
    const spudgpu_compute_pipeline_desc *desc,
    spudgpu_compute_pipeline *out_pipeline);

/**
 * @brief Destroys a compute pipeline.
 *
 * @warning Command lists that dispatch against the pipeline must have
 * finished executing.
 *
 * @param[in] pipeline Pipeline to destroy.
 */
void spudgpu_destroy_compute_pipeline(spudgpu_compute_pipeline pipeline);

/**
 * @brief Reads the descriptor a compute pipeline was created with.
 *
 * @param[in]  pipeline Compute pipeline to query.
 * @param[out] out_desc Receives the pipeline's descriptor.
 *
 * @retval SPUD_SUCCESS `out_desc` was filled in.
 * @retval SPUDRESULT_GPU_INVALID_COMPUTE_PIPELINE `pipeline` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_desc` is NULL.
 */
SPUDRESULT spudgpu_get_compute_pipeline_desc(
    spudgpu_compute_pipeline pipeline,
    spudgpu_compute_pipeline_desc *out_desc);

/**
 * @brief Binds a compute pipeline for subsequent spudgpu_cmd_dispatch()
 * calls.
 *
 * Compute counterpart of spudgpu_cmd_bind_pipeline(). Bind descriptor sets
 * (spudgpu_cmd_bind_descriptor_sets_compute()) or bindless resources after
 * this, then dispatch.
 *
 * @param[in] cmd      Command list being recorded.
 * @param[in] pipeline Compute pipeline to bind.
 */
void spudgpu_cmd_bind_compute_pipeline(
    spudgpu_command_list cmd,
    spudgpu_compute_pipeline pipeline);

/**
 * @brief Compute-pipeline variant of spudgpu_cmd_push_constants(): pushes
 * constant data to the compute shader of a compute pipeline.
 *
 * Two rules hold on every backend, whatever the graphics API underneath
 * keeps or shares:
 *
 * - Push after spudgpu_cmd_bind_compute_pipeline() and before the
 *   spudgpu_cmd_dispatch() that reads the data. What a dispatch reads from a
 *   byte that has not been pushed since the compute pipeline was last bound
 *   is undefined.
 * - Graphics and compute pipelines do not have a push constant block each:
 *   after this call, what a draw reads from the block is undefined until it
 *   is pushed again with spudgpu_cmd_push_constants(), and the other way
 *   round.
 *
 * Does nothing if `cmd`, `pipeline` or `data` is NULL, `size` is 0, or the
 * pipeline declares no push constant ranges.
 *
 * Maps to: vkCmdPushConstants (Vulkan),
 * ID3D12GraphicsCommandList::SetComputeRoot32BitConstants (D3D12),
 * setBytes:length:atIndex: on the compute encoder (Metal).
 *
 * @param[in] cmd      Command list being recorded.
 * @param[in] pipeline The bound compute pipeline, used to look up the layout.
 * @param[in] offset   Byte offset within the push constant block. A multiple
 *                     of 4.
 * @param[in] size     Number of bytes to update. Must be a non-zero multiple
 *                     of 4, and `offset + size` must lie inside the ranges
 *                     the pipeline declares
 *                     (spudgpu_compute_pipeline_desc::push_constant_ranges).
 * @param[in] data     Pointer to `size` bytes of data to push.
 */
void spudgpu_cmd_push_constants_compute(
    spudgpu_command_list cmd,
    spudgpu_compute_pipeline pipeline,
    uint32_t offset,
    uint32_t size,
    const void *data);

/**
 * @brief Records a compute dispatch against the bound compute pipeline.
 *
 * Maps to: vkCmdDispatch (Vulkan), ID3D12GraphicsCommandList::Dispatch
 * (D3D12), dispatchThreadgroups:threadsPerThreadgroup: (Metal).
 *
 * The group counts are in workgroups, not threads: they match the
 * [numthreads]/local_size declared in the compute shader.
 *
 * @param[in] cmd           Command list being recorded.
 * @param[in] group_count_x Number of workgroups in X.
 * @param[in] group_count_y Number of workgroups in Y.
 * @param[in] group_count_z Number of workgroups in Z.
 */
void spudgpu_cmd_dispatch(
    spudgpu_command_list cmd,
    uint32_t group_count_x,
    uint32_t group_count_y,
    uint32_t group_count_z);

// ============================================================================
//  Image <-> Buffer Copies
//  Maps to: vkCmdCopyImageToBuffer / vkCmdCopyBufferToImage (Vulkan)
// ============================================================================

/**
 * @brief Describes a rectangular texel region and target mip/layer used for a
 * copy between an image and a buffer.
 *
 * Maps to Vulkan's VkBufferImageCopy / D3D12's PlacedFootprint region.
 */
/* Frozen: a copy region, passed in arrays. It never changes; a new need gets a new type. */
typedef struct spudgpu_image_buffer_copy_desc {
	/** Byte offset into the buffer where this region starts (or is read from). */
	uint64_t buffer_offset;

	/**
	 * Row length in texels, used to interpret the buffer's layout.
	 * Pass 0 for tightly packed rows (== region width).
	 */
	uint32_t buffer_row_length;

	/**
	 * Image height in texels, used to interpret the buffer's 2D/3D layout.
	 * Pass 0 for tightly packed rows (== region height).
	 */
	uint32_t buffer_image_height;

	/** Mip level of the image being copied. */
	uint32_t mip_level;

	/** Starting array layer of the image being copied. */
	uint32_t base_array_layer;

	/** Number of array layers to copy. */
	uint32_t array_layer_count;

	/** Texel-space offset into the image where the region begins: x. */
	uint32_t image_x, /** y. */ image_y, /** z. */ image_z;

	/** Texel-space size of the copied region: width. */
	uint32_t width, /** Height. */ height, /** Depth. */ depth;
} spudgpu_image_buffer_copy_desc;

/**
 * @brief Records a copy of `size` bytes from one buffer to another.
 *
 * `src_buffer` must have been created with SPUDGPU_BUFFER_USAGE_TRANSFER_SRC,
 * `dst_buffer` with SPUDGPU_BUFFER_USAGE_TRANSFER_DST. The caller is
 * responsible for any pipeline barrier `dst_buffer` needs after the copy
 * before a later stage reads or writes it (see spudgpu_cmd_pipeline_barrier());
 * this call does no synchronization beyond ordering the copy within the
 * command list. To use `dst_buffer` in the same submission as the copy, move
 * it into SPUDGPU_RESOURCE_STATE_COPY_DEST before the copy and into the state
 * it is used in after.
 *
 * The primary use is writing data into a SPUDGPU_MEMORY_KIND_DEVICE buffer,
 * which is never mapped: create a SPUDGPU_MEMORY_KIND_UPLOAD staging buffer
 * with SPUDGPU_BUFFER_USAGE_TRANSFER_SRC, map it, write the data, flush and
 * unmap, then copy from the staging buffer into the device buffer with this
 * call. Reading one back is the same the other way, into a
 * SPUDGPU_MEMORY_KIND_READBACK buffer.
 *
 * Maps to: vkCmdCopyBuffer (Vulkan), CopyBufferRegion (D3D12),
 * copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size: (Metal).
 *
 * @param[in] cmd        Command list being recorded.
 * @param[in] src_buffer Buffer to read bytes from.
 * @param[in] dst_buffer Buffer to write bytes into.
 * @param[in] src_offset Byte offset into `src_buffer` to start reading at.
 * @param[in] dst_offset Byte offset into `dst_buffer` to start writing at.
 * @param[in] size       Number of bytes to copy.
 */
void spudgpu_cmd_copy_buffer(
    spudgpu_command_list cmd,
    spudgpu_buffer src_buffer,
    spudgpu_buffer dst_buffer,
    uint64_t src_offset,
    uint64_t dst_offset,
    uint64_t size);

/**
 * @brief Records a copy from an image subresource region into a buffer.
 *
 * The image must be in SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC. Use
 * spudgpu_get_image_buffer_copy_size() to size the destination buffer first.
 *
 * Maps to: vkCmdCopyImageToBuffer (Vulkan), CopyTextureRegion (D3D12),
 * copyFromTexture:toBuffer: (Metal).
 *
 * @param[in] cmd        Command list being recorded.
 * @param[in] src_image  Image to read texel data from.
 * @param[in] dst_buffer Buffer that receives the copied bytes.
 * @param[in] desc       Region, offset and subresource to copy.
 */
void spudgpu_cmd_copy_image_to_buffer(
    spudgpu_command_list cmd,
    spudgpu_image src_image,
    spudgpu_buffer dst_buffer,
    const spudgpu_image_buffer_copy_desc *desc);

/**
 * @brief Records a copy of buffer bytes into an image subresource region.
 * Used for texture uploads.
 *
 * The image must be in SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST. Use
 * spudgpu_get_image_buffer_copy_size() for the expected source row pitch and
 * total size.
 *
 * Maps to: vkCmdCopyBufferToImage (Vulkan), CopyTextureRegion (D3D12),
 * copyFromBuffer:toTexture: (Metal).
 *
 * @param[in] cmd        Command list being recorded.
 * @param[in] src_buffer Buffer to read bytes from.
 * @param[in] dst_image  Image to write texel data into.
 * @param[in] desc       Region, offset and subresource to copy.
 */
void spudgpu_cmd_copy_buffer_to_image(
    spudgpu_command_list cmd,
    spudgpu_buffer src_buffer,
    spudgpu_image dst_image,
    const spudgpu_image_buffer_copy_desc *desc);

/**
 * @brief Computes the buffer row pitch and total size needed to receive (or
 * supply) a copy of an image mip level.
 *
 * Some backends (D3D12) require a specific row-pitch alignment for buffer
 * footprints; the values returned are already aligned, so the caller can
 * allocate a correctly sized staging buffer without hardcoding backend rules.
 *
 * Does nothing if `image` is NULL.
 *
 * @param[in]  image          Image the copy targets.
 * @param[in]  mip_level      Mip level being copied.
 * @param[out] out_row_pitch  Receives the aligned row pitch, in bytes.
 * @param[out] out_total_size Receives the required buffer size, in bytes.
 */
void spudgpu_get_image_buffer_copy_size(
    spudgpu_image image,
    uint32_t mip_level,
    uint64_t *out_row_pitch,
    uint64_t *out_total_size);

/**
 * @brief Gives the alignment the buffer offset of a copy between a buffer
 * and an image must have.
 *
 * spudgpu_image_buffer_copy_desc::buffer_offset of a copy to or from `image`
 * must be a multiple of the value returned, so a caller that places several
 * copies in one buffer knows where each may start. The value is the
 * requirement and no more: an offset of 0 always meets it.
 *
 * It depends on the image's format and the graphics API, and is not always a
 * power of two (a 12-byte texel can give 12).
 *
 * @param[in]  image         Image the copy reads or writes.
 * @param[out] out_alignment Receives the alignment, in bytes.
 *
 * @retval SPUD_SUCCESS `out_alignment` was written.
 * @retval SPUDRESULT_GPU_INVALID_IMAGE `image` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_alignment` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_FORMAT The image's format has no whole
 *         number of bytes to a texel or block (see
 *         spudgpu_format_bit_count()), so no copy of it has a known layout.
 */
SPUDRESULT spudgpu_get_image_buffer_copy_offset_alignment(
    spudgpu_image image,
    uint64_t *out_alignment);

// ============================================================================
//  Image Blit
//  Maps to: vkCmdBlitImage (Vulkan)
// ============================================================================

/**
 * @brief Describes a source and destination subresource/region pair for an
 * image blit.
 *
 * Each region is given as two opposite corners (x0,y0,z0)-(x1,y1,z1) rather
 * than an offset+extent, matching Vulkan's VkImageBlit. This is what allows
 * the source and destination regions to be different sizes (the whole point
 * of a blit vs. a plain copy) — most commonly src = mip N at full size, dst =
 * mip N+1 at half size, when generating a mip chain.
 */
/* Frozen: a blit region, passed in arrays. It never changes; a new need gets a new type. */
typedef struct spudgpu_image_blit_desc {
	/** Mip level read from. */
	uint32_t src_mip_level;
	/** First array layer read from. */
	uint32_t src_base_array_layer;
	/** Number of array layers read. */
	uint32_t src_array_layer_count;
	/** One corner of the source region, in texels of #src_mip_level: x. */
	uint32_t src_x0, /** y. */ src_y0, /** z. */ src_z0;
	/** The opposite corner of the source region: x. */
	uint32_t src_x1, /** y. */ src_y1, /** z. */ src_z1;

	/** Mip level written to. */
	uint32_t dst_mip_level;
	/** First array layer written to. */
	uint32_t dst_base_array_layer;
	/** Number of array layers written. */
	uint32_t dst_array_layer_count;
	/** One corner of the destination region, in texels of #dst_mip_level: x. */
	uint32_t dst_x0, /** y. */ dst_y0, /** z. */ dst_z0;
	/** The opposite corner of the destination region: x. */
	uint32_t dst_x1, /** y. */ dst_y1, /** z. */ dst_z1;

	/** Filter applied when src and dst region sizes differ. */
	SPUDGPU_FILTER filter;
} spudgpu_image_blit_desc;

/**
 * @brief Records a (possibly scaling) copy between two image regions.
 *
 * Unlike spudgpu_cmd_copy_image_to_buffer(), source and destination regions
 * may differ in size; the driver resamples using `desc->filter`. The primary
 * use is mip chain generation: blit mip level N (full size) into mip level
 * N+1 (half size) of the same image, one level pair at a time.
 *
 * `src_image` must be in SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC and `dst_image` in
 * SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST. They may be the same image.
 *
 * Maps to: vkCmdBlitImage (Vulkan). Neither D3D12 nor Metal exposes a direct
 * blit command with filtering equivalent to Vulkan's.
 *
 * @note Not yet implemented by the Metal backend.
 *
 * @param[in] cmd       Command list being recorded.
 * @param[in] src_image Image to read from.
 * @param[in] dst_image Image to write into.
 * @param[in] desc      Source and destination subresources, regions and
 *                      filter.
 */
void spudgpu_cmd_blit_image(
    spudgpu_command_list cmd,
    spudgpu_image src_image,
    spudgpu_image dst_image,
    const spudgpu_image_blit_desc *desc);

// ============================================================================
//  Rendering
// ============================================================================

/**
 * @brief What happens to an attachment's contents when a rendering pass
 * begins.
 */
typedef uint32_t SPUDGPU_LOAD_OP;
enum {
	/** Keep the attachment's existing contents. */
	SPUDGPU_LOAD_OP_LOAD = 0,
	/** Clear to the attachment's clear value before drawing. */
	SPUDGPU_LOAD_OP_CLEAR,
	/** Contents are undefined; the driver may skip the load entirely. */
	SPUDGPU_LOAD_OP_DONT_CARE,
};

/**
 * @brief What happens to an attachment's contents when a rendering pass ends.
 */
typedef uint32_t SPUDGPU_STORE_OP;
enum {
	/** Write the render result back to the attachment. */
	SPUDGPU_STORE_OP_STORE = 0,
	/** Discard the result; the driver may skip the writeback. */
	SPUDGPU_STORE_OP_DONT_CARE,
};

/**
 * @brief One color attachment bound for a rendering pass.
 */
/* Frozen: an array element, held by value in spudgpu_rendering_begin_desc. It never changes; a new need gets a new type. */
typedef struct spudgpu_color_attachment_desc {
	/** The view to render into. */
	spudgpu_image_view image_view;

	/** @see SPUDGPU_LOAD_OP */
	SPUDGPU_LOAD_OP load_op;
	/** @see SPUDGPU_STORE_OP */
	SPUDGPU_STORE_OP store_op;

	/** RGBA clear color; only read when load_op == SPUDGPU_LOAD_OP_CLEAR. */
	float clear_color[4];
} spudgpu_color_attachment_desc;

/**
 * @brief The depth/stencil attachment bound for a rendering pass.
 */
/* Frozen: held by value in spudgpu_rendering_begin_desc. It never changes; a new need gets a new type. */
typedef struct spudgpu_depth_attachment_desc {
	/** NULL for no depth/stencil attachment. */
	spudgpu_image_view image_view;

	/** What happens to the depth values when the pass begins. */
	SPUDGPU_LOAD_OP depth_load_op;
	/** What happens to the depth values when the pass ends. */
	SPUDGPU_STORE_OP depth_store_op;
	/** What happens to the stencil values when the pass begins. Ignored
	 *  when the view's format has no stencil. */
	SPUDGPU_LOAD_OP stencil_load_op;
	/** What happens to the stencil values when the pass ends. Ignored when
	 *  the view's format has no stencil. */
	SPUDGPU_STORE_OP stencil_store_op;

	/** Only read when depth_load_op == SPUDGPU_LOAD_OP_CLEAR. */
	float clear_depth;
	/** Only read when stencil_load_op == SPUDGPU_LOAD_OP_CLEAR. */
	uint32_t clear_stencil;
} spudgpu_depth_attachment_desc;

/**
 * @brief Describes the attachments and render area for one rendering pass.
 *
 * Rendering is dynamic: there is no render pass or framebuffer object, and
 * a pipeline is not created against one. The attachments and their load and
 * store operations are given per call, here.
 *
 * Carries no pipeline reference — pipelines are bound mid-pass with
 * spudgpu_cmd_bind_pipeline like any other draw-time state, not tied to a
 * specific render pass object.
 *
 * Maps to `vkCmdBeginRendering` / `vkCmdEndRendering` (Vulkan 1.3, or
 * `VK_KHR_dynamic_rendering`), `ID3D12GraphicsCommandList4::BeginRenderPass`
 * / `EndRenderPass` (D3D12), and a render command encoder made from an
 * `MTLRenderPassDescriptor` (Metal).
 *
 * @todo A Vulkan device without Vulkan 1.3 or `VK_KHR_dynamic_rendering` is
 * not supported: there is no `VkRenderPass` / `VkFramebuffer` path.
 */
typedef struct spudgpu_rendering_begin_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** The color attachments. Only the first #color_attachment_count are read. */
	spudgpu_color_attachment_desc color_attachments[SPUDGPU_MAX_COLOR_ATTACHMENTS];
	/** Number of valid entries in #color_attachments. */
	uint32_t color_attachment_count;

	/** depth_attachment.image_view == NULL means no depth/stencil attachment. */
	spudgpu_depth_attachment_desc depth_attachment;

	/**
	 * Left edge of the render area, in pixels. Not read by any backend
	 * yet: the render area starts at 0. Set to 0.
	 */
	int32_t x;
	/**
	 * Top edge of the render area, in pixels. Not read by any backend yet:
	 * the render area starts at 0. Set to 0.
	 */
	int32_t y;

	/** Render area width in pixels. Usually the attachments' width. */
	uint32_t width;

	/** Render area height in pixels. Usually the attachments' height. */
	uint32_t height;

	/**
	 * Number of layers the pass renders to, or 0 for a pass that is not
	 * layered.
	 *
	 * These rules hold on every backend, whatever the graphics API
	 * underneath would also accept:
	 *
	 * - 0: every attachment's view covers one array layer, and every
	 *   primitive is drawn to it. What a shader's layer output does is
	 *   undefined.
	 * - Not 0: the device was created with
	 *   SPUDGPU_FEATURE_LAYERED_RENDERING, and every attachment's view,
	 *   depth/stencil included, is
	 *   SPUDGPU_IMAGE_VIEW_TYPE_2D_ARRAY over one mip level and exactly
	 *   this many array layers. 1 is a layered pass of one layer, not the
	 *   same as 0.
	 * - In a layered pass the vertex shader (or the mesh shader, per
	 *   primitive) writes the layer a primitive is drawn to, counted from
	 *   the view's first layer. A primitive whose layer is not written is
	 *   drawn to layer 0; one written as this count or more is undefined.
	 *   No other stage chooses the layer.
	 * - A load operation of SPUDGPU_LOAD_OP_CLEAR clears every layer, and
	 *   the store operation applies to every layer.
	 * - The image layout each attachment must be in applies to every layer
	 *   its view covers.
	 *
	 * Maps to `VkRenderingInfo::layerCount` (1 for a pass that is not
	 * layered) and the `Layer` built-in (Vulkan), the array size of the
	 * render target and depth stencil views and
	 * `SV_RenderTargetArrayIndex` (D3D12), and
	 * `MTLRenderPassDescriptor.renderTargetArrayLength` and
	 * `[[render_target_array_index]]` (Metal).
	 */
	uint32_t layer_count;

	/**
	 * Set true only if a bundle (SPUDGPU_COMMAND_LIST_TYPE_BUNDLE, see
	 * SPUDGPU_EXT_BUNDLES) will be executed via spudgpu_cmd_execute_bundle
	 * inside this rendering pass. Required on Vulkan, where secondary
	 * command buffers may only be recorded into a scope that was opened
	 * with VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT — SpudLib
	 * never infers this from whether a bundle happens to get executed
	 * later, since the caller always knows this decision up front and
	 * SpudLib does not guess on the caller's behalf. No-op on backends
	 * without a bundle/secondary-command-buffer distinction.
	 */
	bool will_execute_bundles;

	/**
	 * A timestamp set the pass writes two timestamps into: one as its
	 * first work starts and one as its last work ends, or NULL for none.
	 * The set was created for the queue the command list is submitted on,
	 * and both entries were reset since they were last written
	 * (spudgpu_cmd_reset_timestamps()). See "Timestamps".
	 */
	spudgpu_timestamp_set timestamp_set;
	/** The entry written as the pass starts. Read only with #timestamp_set. */
	uint32_t timestamp_begin_index;
	/**
	 * The entry written as the pass ends, at spudgpu_cmd_end_rendering().
	 * Not #timestamp_begin_index. Read only with #timestamp_set.
	 */
	uint32_t timestamp_end_index;
} spudgpu_rendering_begin_desc;

/**
 * @brief Begins a rendering pass, binding the color and optional
 * depth/stencil attachments and issuing their load operations.
 *
 * Must be matched with spudgpu_cmd_end_rendering() before submitting. Bind a
 * pipeline with spudgpu_cmd_bind_pipeline() after this call, not before.
 *
 * Nothing is recorded if `desc->base` is not {SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC, NULL}
 * (see SPUD_STRUCT_BASE).
 *
 * @param[in] cmd  Command list being recorded.
 * @param[in] desc Attachments and render area.
 */
void spudgpu_cmd_begin_rendering(
    spudgpu_command_list cmd,
    const spudgpu_rendering_begin_desc *desc);

/**
 * @brief Ends the current rendering pass, issuing the attachments' store
 * operations.
 *
 * @param[in] cmd Command list being recorded.
 */
void spudgpu_cmd_end_rendering(spudgpu_command_list cmd);

/**
 * @brief Clears a single color attachment to a solid color.
 *
 * Wraps spudgpu_cmd_begin_rendering() / spudgpu_cmd_end_rendering() with
 * load_op == CLEAR and no draws; no pipeline is required. The attachment must
 * already be in SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL (see
 * spudgpu_cmd_image_barrier()).
 *
 * @param[in] cmd        Command list being recorded.
 * @param[in] attachment View to clear.
 * @param[in] r          Red component of the clear color.
 * @param[in] g          Green component of the clear color.
 * @param[in] b          Blue component of the clear color.
 * @param[in] a          Alpha component of the clear color.
 * @param[in] width      Attachment width in pixels.
 * @param[in] height     Attachment height in pixels.
 */
void spudgpu_cmd_clear_color_attachment(
    spudgpu_command_list cmd,
    spudgpu_image_view attachment,
    float r,
    float g,
    float b,
    float a,
    uint32_t width,
    uint32_t height);

/**
 * @brief Clears a depth/stencil attachment.
 *
 * Wraps spudgpu_cmd_begin_rendering() / spudgpu_cmd_end_rendering() with no
 * draws; no pipeline is required. The attachment must already be in
 * SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL (see
 * spudgpu_cmd_image_barrier()).
 *
 * @param[in] cmd           Command list being recorded.
 * @param[in] attachment    Depth/stencil view to clear.
 * @param[in] clear_depth   Whether to clear the depth aspect.
 * @param[in] clear_stencil Whether to clear the stencil aspect.
 * @param[in] depth         Depth clear value (typically 1.0f).
 * @param[in] stencil       Stencil clear value (typically 0).
 * @param[in] width         Attachment width in pixels.
 * @param[in] height        Attachment height in pixels.
 */
void spudgpu_cmd_clear_depth_attachment(
    spudgpu_command_list cmd,
    spudgpu_image_view attachment,
    bool clear_depth,
    bool clear_stencil,
    float depth,
    uint32_t stencil,
    uint32_t width,
    uint32_t height);

// ============================================================================
//  Bundles
// ============================================================================

/**
 * @brief 1 when the compiled-in backend implements bundles and this section
 * is declared, 0 when it is compiled out.
 *
 * A bundle is a command list made from a SPUDGPU_COMMAND_LIST_TYPE_BUNDLE
 * allocator (spudgpu_command_allocator_desc::type). It records a fixed
 * sequence of pipeline, descriptor and draw state once, and is replayed
 * unchanged from inside a spudgpu_cmd_begin_rendering() /
 * spudgpu_cmd_end_rendering() scope of a direct command list, as often as
 * wanted, with nothing recorded again. It draws into the attachments of the
 * direct list's pass and never begins or ends rendering itself.
 *
 * Two rules hold on every backend, whatever the graphics API inherits:
 *
 * - A bundle draws with one viewport and one scissor rectangle, named when
 *   it is recorded (spudgpu_bundle_inheritance_desc), and the direct list
 *   has set the same two, at index 0, when it executes the bundle. A bundle
 *   never calls spudgpu_cmd_set_viewports() or
 *   spudgpu_cmd_set_scissor_rects(): D3D12 refuses both in a bundle and
 *   takes them from the direct list; a Vulkan secondary command buffer
 *   inherits neither and is given them when its recording begins.
 * - After spudgpu_cmd_execute_bundle(), the direct list's pipeline, vertex
 *   and index buffers, descriptor sets and push constants are undefined
 *   until set again: D3D12 leaves the bundle's in place and Vulkan leaves
 *   none.
 *
 * Maps to `ID3D12GraphicsCommandList::ExecuteBundle` (D3D12) and to
 * `vkCmdExecuteCommands` over a secondary command buffer (Vulkan).
 *
 * 1 when the backend compiled into this build implements bundles, 0
 * otherwise. It says nothing about any GPU, and needs no feature bit: every
 * Vulkan and D3D12 GPU records bundles. It is 0 on Metal, which has nothing to
 * record once and replay: a command buffer and its encoders are used once.
 * That is a gap in what the API can do, so the section is compiled out there
 * and not emulated.
 */
#if SPUDGPU_COMPILE_VULKAN_API || SPUDGPU_COMPILE_D3D12_API
#define SPUDGPU_EXT_BUNDLES 1
#else
#define SPUDGPU_EXT_BUNDLES 0
#endif

// ============================================================================
//  Debug labels
// ============================================================================

/**
 * @brief Opens a labelled region of a command list, for a capture or
 * profiling tool to show.
 *
 * A label changes nothing the GPU does. Where no tool reads it, or the
 * graphics API offers no way to record it, the call does nothing.
 *
 * Two rules hold on every backend:
 *
 * - Regions nest. spudgpu_cmd_end_debug_label() closes the region opened
 *   last, and every region opened in a command list is closed in it.
 * - A region opened inside a rendering pass closes inside the same pass,
 *   and one opened outside every pass closes outside every pass, with any
 *   pass inside it wholly inside it.
 *
 * Vulkan: `vkCmdBeginDebugUtilsLabelEXT`, when the instance has
 * `VK_EXT_debug_utils` (spudgpu_create_instance() enables it wherever the
 * Vulkan loader offers it). D3D12: `BeginEvent` in PIX's ANSI encoding.
 * Metal: `pushDebugGroup:` on the render encoder inside a pass and on the
 * command buffer outside one.
 *
 * @param[in] cmd   Command list being recorded.
 * @param[in] label Null-terminated text. Only read during the call. NULL is
 *                  taken as an empty label, so the region still opens.
 */
void spudgpu_cmd_begin_debug_label(
    spudgpu_command_list cmd,
    const char *label);

/**
 * @brief Closes the region spudgpu_cmd_begin_debug_label() opened last.
 *
 * @param[in] cmd Command list being recorded.
 */
void spudgpu_cmd_end_debug_label(spudgpu_command_list cmd);

// ============================================================================
//  Timestamps
// ============================================================================

/**
 * @brief Creates a set of GPU timestamps, written as rendering passes start
 * and end.
 *
 * A timestamp is written only at the start and the end of a rendering pass
 * (spudgpu_rendering_begin_desc::timestamp_set), because that is what every
 * backend can do: Apple GPUs sample their counters at the boundaries of a
 * pass's stages and nowhere in between. One entry is one timestamp. The
 * entries are used in this order on every backend:
 *
 * 1. spudgpu_cmd_reset_timestamps(), outside every pass, before a pass
 *    writes them.
 * 2. A pass writes them.
 * 3. spudgpu_cmd_resolve_timestamps(), outside every pass, after the pass,
 *    in the same command list or a later one on the same queue.
 * 4. Once the submission that resolved them has finished, which is the
 *    caller's to know (a fence), spudgpu_get_timestamps() reads them.
 *
 * An entry read without having been through those steps since it was last
 * reset holds an undefined value.
 *
 * Vulkan: a `VK_QUERY_TYPE_TIMESTAMP` query pool. D3D12: a timestamp query
 * heap and a readback buffer the resolve copies into. Metal: an
 * `MTLCounterSampleBuffer` of the timestamp counter set, sampled at stage
 * boundaries.
 *
 * @param[in]  device  Device to create the set on.
 * @param[in]  queue   The queue the command lists that write and resolve
 *                     the set are submitted on. D3D12 counts time per
 *                     queue.
 * @param[in]  count   Number of entries. Not 0.
 * @param[out] out_set Receives the new set on success.
 *
 * @retval SPUD_SUCCESS The set was created.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_COMMAND_QUEUE `queue` is NULL.
 * @retval SPUDRESULT_ZERO_SIZE `count` is 0.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_set` is NULL.
 * @retval SPUDRESULT_GPU_TIMESTAMPS_NOT_SUPPORTED The device wasn't created
 *         with SPUDGPU_FEATURE_PASS_TIMESTAMPS.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_OUT_OF_MEMORY or SPUDRESULT_API_SPECIFIC_FAILURE.
 */
SPUDRESULT spudgpu_create_timestamp_set(
    spudgpu_device device,
    spudgpu_command_queue queue,
    uint32_t count,
    spudgpu_timestamp_set *out_set);

/**
 * @brief Destroys a timestamp set.
 *
 * @warning Command lists that write or resolve it must have finished
 * executing.
 *
 * @param[in] set Set to destroy. NULL does nothing.
 */
void spudgpu_destroy_timestamp_set(spudgpu_timestamp_set set);

/**
 * @brief Records the reset of entries of a timestamp set, before a pass
 * writes them.
 *
 * Outside every rendering pass. Vulkan: `vkCmdResetQueryPool`. D3D12 and
 * Metal have nothing to reset, and record nothing.
 *
 * @param[in] cmd   Command list being recorded.
 * @param[in] set   Timestamp set.
 * @param[in] first First entry.
 * @param[in] count Number of entries, all below the set's count.
 */
void spudgpu_cmd_reset_timestamps(
    spudgpu_command_list cmd,
    spudgpu_timestamp_set set,
    uint32_t first,
    uint32_t count);

/**
 * @brief Records what makes written entries of a timestamp set readable by
 * spudgpu_get_timestamps().
 *
 * Outside every rendering pass, after the passes that wrote them. D3D12:
 * `ResolveQueryData` into the set's readback buffer. Vulkan and Metal read
 * the results where they were written, and record nothing.
 *
 * @param[in] cmd   Command list being recorded.
 * @param[in] set   Timestamp set.
 * @param[in] first First entry.
 * @param[in] count Number of entries, all below the set's count.
 */
void spudgpu_cmd_resolve_timestamps(
    spudgpu_command_list cmd,
    spudgpu_timestamp_set set,
    uint32_t first,
    uint32_t count);

/**
 * @brief Reads resolved entries of a timestamp set, in nanoseconds.
 *
 * Call once the submission that resolved them has finished. The values are
 * nanoseconds from an origin of the backend's: only the difference between
 * two entries of one set means anything, such as a pass's end less its
 * start. Vulkan: the ticks times `VkPhysicalDeviceLimits::timestampPeriod`.
 * D3D12: the ticks over the queue's `GetTimestampFrequency`. Metal: the
 * GPU's ticks set against the CPU's clock with `sampleTimestamps:` at the
 * set's creation and at this call, as Apple describes for converting GPU
 * timestamps.
 *
 * @param[in]  set     Timestamp set.
 * @param[in]  first   First entry.
 * @param[in]  count   Number of entries, all below the set's count.
 * @param[out] out_ns  Receives `count` values.
 *
 * @retval SPUD_SUCCESS `out_ns` was written.
 * @retval SPUDRESULT_GPU_INVALID_TIMESTAMP_SET `set` is NULL.
 * @retval SPUDRESULT_ZERO_SIZE `count` is 0.
 * @retval SPUDRESULT_INDEX_OUT_OF_RANGE An entry is at or past the set's
 *         count.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_ns` is NULL.
 * @return Another SPUDRESULT if the backend fails, such as
 *         SPUDRESULT_API_SPECIFIC_FAILURE.
 */
SPUDRESULT spudgpu_get_timestamps(
    spudgpu_timestamp_set set,
    uint32_t first,
    uint32_t count,
    uint64_t *out_ns);

#if SPUDGPU_EXT_BUNDLES

/**
 * @brief Describes the attachments, viewport and scissor rectangle a bundle
 * will be replayed under.
 *
 * Required up front on Vulkan, where a bundle is a secondary command buffer
 * and VK_KHR_dynamic_rendering requires attachment formats at recording
 * time via VkCommandBufferInheritanceRenderingInfo — there is no
 * attachment/framebuffer object to inherit them from implicitly — and where
 * a secondary command buffer inherits no viewport or scissor. Mirrors
 * the attachment format fields on spudgpu_shader_pipeline_desc; must match
 * whatever the direct command list's spudgpu_cmd_begin_rendering call
 * actually binds, and the viewport and scissor it has set, when the bundle
 * is executed.
 */
typedef struct spudgpu_bundle_inheritance_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_BUNDLE_INHERITANCE_DESC; `next` is NULL (nothing is chained onto
	 * this struct yet). Set by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/**
	 * The pass's color attachment formats, in its order. Only the first
	 * #color_attachment_count are read.
	 * @see SPUDGPU_FORMAT
	 */
	SPUDGPU_FORMAT color_attachment_formats[SPUDGPU_MAX_COLOR_ATTACHMENTS];
	/** Number of valid entries in #color_attachment_formats. */
	uint32_t color_attachment_count;

	/**
	 * SPUDGPU_FORMAT_UNKNOWN for no depth/stencil attachment.
	 * @see SPUDGPU_FORMAT
	 */
	SPUDGPU_FORMAT depth_format;

	/**
	 * The viewport the bundle's draws use: the one the direct list has set
	 * at index 0 when it executes the bundle. Vulkan records it into the
	 * bundle; D3D12 takes it from the direct list.
	 */
	SPUDGPU_VIEWPORT viewport;
	/**
	 * The scissor rectangle the bundle's draws use, with the same rule as
	 * #viewport.
	 */
	SPUDGPU_SCISSOR_RECT scissor_rect;
} spudgpu_bundle_inheritance_desc;

/**
 * @brief Begins recording a bundle command list (one created against a
 * SPUDGPU_COMMAND_LIST_TYPE_BUNDLE allocator).
 *
 * Use this instead of spudgpu_begin_command_list() for a bundle. It is the
 * same operation on D3D12 (`desc` is ignored there; a bundle needs no
 * attachment info up front and takes its viewport and scissor from the
 * direct list), but Vulkan needs `desc` to fill in a secondary command
 * buffer's inheritance info and to set its viewport and scissor. Close the
 * bundle with the shared spudgpu_end_command_list(); there is no separate
 * "end bundle" call.
 *
 * Nothing is recorded if `desc->base` is not {SPUD_STRUCT_TYPE_GPU_BUNDLE_INHERITANCE_DESC, NULL}
 * (see SPUD_STRUCT_BASE).
 *
 * @param[in] bundle Command list created against an allocator of type
 *                   SPUDGPU_COMMAND_LIST_TYPE_BUNDLE.
 * @param[in] desc   Attachment formats, viewport and scissor rectangle the
 *                   bundle will be executed under.
 */
void spudgpu_begin_bundle_command_list(
    spudgpu_command_list bundle,
    const spudgpu_bundle_inheritance_desc *desc);

/**
 * @brief Replays a recorded, closed bundle into a direct command list that
 * is recording.
 *
 * Must be called between spudgpu_cmd_begin_rendering() (with
 * will_execute_bundles set true) and spudgpu_cmd_end_rendering() on `cmd`,
 * with attachment formats matching the bundle's
 * spudgpu_bundle_inheritance_desc, and with its viewport and scissor
 * rectangle set at index 0. Afterwards, the direct list's pipeline, vertex
 * and index buffers, descriptor sets and push constants are undefined until
 * set again. The bundle must already be closed
 * (spudgpu_end_command_list() called on it) and must not be re-recorded while
 * a submission that references it is in flight.
 *
 * Maps to: ID3D12GraphicsCommandList::ExecuteBundle (D3D12),
 * vkCmdExecuteCommands (Vulkan).
 *
 * @param[in] cmd    Direct command list being recorded.
 * @param[in] bundle Closed bundle to replay.
 */
void spudgpu_cmd_execute_bundle(
    spudgpu_command_list cmd,
    spudgpu_command_list bundle);

#endif // SPUDGPU_EXT_BUNDLES

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // SPUDLIB_SPUDGPU_H
