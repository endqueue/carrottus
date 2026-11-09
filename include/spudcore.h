/**
 * @file spudcore.h
 * @brief SpudCore: what every SpudLib module shares.
 *
 * The SPUDRESULT enum all modules return, the library's version, the
 * fixed-width integer limits, struct bases, the caller's allocator and the
 * debug-name functions. Platform-agnostic: nothing here calls a platform
 * API.
 */

#ifndef SPUDCORE_H
#define SPUDCORE_H

#include <stddef.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/**
 * @name Unsigned integer limits
 *
 * The same values as UINT8_MAX, UINT16_MAX, UINT32_MAX and UINT64_MAX in
 * stdint.h. Defined here because VSCode IntelliSense on Windows does not
 * recognise the suffixes ('ui32', 'ui64', ...) that stdint.h ends those
 * definitions with.
 * @{
 */
/** @brief Largest value of a uint8_t. */
#define SPUD_UINT8_MAX 0xFFU
/** @brief Largest value of a uint16_t. */
#define SPUD_UINT16_MAX 0xFFFFU
/** @brief Largest value of a uint32_t. */
#define SPUD_UINT32_MAX 0xFFFFFFFFU
/** @brief Largest value of a uint64_t. */
#define SPUD_UINT64_MAX 0xFFFFFFFFFFFFFFFFULL
/** @} */

/**
 * @defgroup spud_version Version
 * @brief Which spudlib a caller was built against, and which it runs on.
 *
 * One version for the whole library: spudlib is built and linked as one
 * archive, so its modules can't be mixed from different versions.
 *
 * What a version promises, from 1.0.0 on:
 *
 * | Change in | May add | May change or remove |
 * |-----------|---------|----------------------|
 * | patch     | nothing | nothing: fixes only, no header change but comments |
 * | minor     | functions, SPUDRESULT values, SPUD_STRUCT_TYPE values, chained structs, enumerators | nothing |
 * | major     | anything | anything |
 *
 * "Released" means a commit tagged vMAJOR.MINOR.PATCH. Before 1.0.0, a
 * minor may change anything; nothing is promised.
 *
 * A struct that can be chained says in its comment which version first
 * takes it ("Since 1.2.0"), so that a caller can ask spud_get_version()
 * before chaining it. A caller that doesn't ask loses nothing but the
 * question: a spudlib that doesn't take a struct refuses the chain with
 * SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN.
 * @{
 */

/** @brief Packs a version into one number that compares with < and >:
 * 10 bits of major, 10 of minor, 12 of patch (Vulkan's layout, without its
 * variant bits). */
#define SPUDLIB_MAKE_VERSION(major, minor, patch) \
	((((uint32_t)(major)) << 22U) | (((uint32_t)(minor)) << 12U) | ((uint32_t)(patch)))
/** @brief The major part of a packed version. */
#define SPUDLIB_VERSION_GET_MAJOR(version) ((uint32_t)(version) >> 22U)
/** @brief The minor part of a packed version. */
#define SPUDLIB_VERSION_GET_MINOR(version) (((uint32_t)(version) >> 12U) & 0x3FFU)
/** @brief The patch part of a packed version. */
#define SPUDLIB_VERSION_GET_PATCH(version) ((uint32_t)(version) & 0xFFFU)

/* The one place the version is written. CMakeLists.txt reads these three
 * lines, so each stays "#define NAME <number>" and nothing else. */
/** @brief The major version of these headers. */
#define SPUDLIB_VERSION_MAJOR 0
/** @brief The minor version of these headers. */
#define SPUDLIB_VERSION_MINOR 2
/** @brief The patch version of these headers. */
#define SPUDLIB_VERSION_PATCH 0

/** @brief The version of the headers the caller is compiled against. */
#define SPUDLIB_VERSION \
	SPUDLIB_MAKE_VERSION(SPUDLIB_VERSION_MAJOR, SPUDLIB_VERSION_MINOR, SPUDLIB_VERSION_PATCH)

/**
 * @brief Returns the version of the spudlib the caller is running on: the
 * SPUDLIB_VERSION it was compiled with.
 *
 * It can differ from the caller's own SPUDLIB_VERSION only when spudlib was
 * built separately from the caller. Which one to accept is the caller's
 * decision; spudlib refuses no caller for its version.
 *
 * @return The packed version (SPUDLIB_MAKE_VERSION). Never 0.
 */
uint32_t spud_get_version(void);

/** @} */

/**
 * @defgroup spud_threads Threads
 * @brief Every SpudLib object is used by one caller at a time.
 *
 * The rule for every module, stated here once:
 *
 * - Every object (an instance, a device, a queue, a stream, a socket, a
 *   file, any handle a `_create` gives) is used by one caller at a time.
 *   A use is any call that takes the object, or an object it hands out
 *   that is not an object of its own, from the moment the call is made
 *   until it returns. Vulkan calls this "externally synchronized".
 * - The rule is serialization, not thread identity. No object belongs to
 *   a thread: it may be called from any thread, and from a different one
 *   each time (a Swift actor, a C# continuation), so long as no two calls
 *   on it overlap. Ordering the calls is the caller's, by whatever means
 *   it has; SpudLib takes no lock for it.
 * - Different objects are independent: calls on two objects may overlap,
 *   wherever each was made from. What SpudLib keeps for itself behind the
 *   handles (a shared table, an adapter's one device) it guards itself.
 * - A call that takes no object (SpudPerf's and SpudMemory's calls, and
 *   SpudFiles' path calls) has nothing to order and may overlap any call.
 * - The exceptions are few and each is marked on the call itself, by a
 *   line that begins `Thread:` - a call that may overlap others on the
 *   same object (an abort, a status read), a function of the caller's
 *   that SpudLib or the platform calls on a thread of its own, and a call
 *   the platform confines to one thread. A module's `@par Threads`
 *   paragraph says only what is particular to that module.
 *
 * Checking it: a Debug build asserts when two calls overlap on one
 * SpudGPU queue (spudgpu_submit_command_lists()). Not yet for any other
 * object.
 */

/**
 * @brief The result of a SpudLib call, shared by every module.
 *
 * SPUD_SUCCESS is 0 and the only success value; every other enumerator names
 * one specific failure. Test a result with SPUDFAIL() and print it with
 * spudresult_str().
 *
 * The values are grouped by module: 1-16 are general, 199-369 are SpudGPU
 * (`SPUDRESULT_GPU_*`), 1001-1024 SpudFiles (`SPUDRESULT_SPUDFILE_*`), 2027-2028
 * SpudMemory (`SPUDRESULT_SPUDMEM_*`), 3001-3033 SpudNet (`SPUDRESULT_SPUDNET_*`),
 * 4001-4015 SpudAudio (`SPUDRESULT_SAUD_*`) and 5001-5005 SpudInput
 * (`SPUDRESULT_SPUDINPUT_*`). A value is never reused for
 * a different meaning.
 */
typedef enum SPUDRESULT {
	/** The call succeeded. */
	SPUD_SUCCESS                         = 0,
	/** The call failed and no more specific result applies. */
	SPUDRESULT_GENERAL_FAILURE           = 1,
	/** A call into the platform's own API failed. */
	SPUDRESULT_API_SPECIFIC_FAILURE      = 2,
	/** The native API asked for is not one this build can use. */
	SPUDRESULT_INVALID_API               = 3,
	/** A desc pointer is NULL. */
	SPUDRESULT_NULL_DESC                 = 4,
	/** An output pointer is NULL. */
	SPUDRESULT_NULL_OUTPUT_PARAMETER     = 5,
	/** A desc holds a value the call cannot accept. */
	SPUDRESULT_DESC_INVALID_PARAMETERS   = 6,
	/** An index is past the end of what it indexes. */
	SPUDRESULT_INDEX_OUT_OF_RANGE        = 7,
	/** A size or count is zero. */
	SPUDRESULT_ZERO_SIZE                 = 8,
	/** An allocation failed. */
	SPUDRESULT_OUT_OF_MEMORY             = 9,
	/** The call, or the option asked of it, is not implemented yet. */
	SPUDRESULT_NOT_IMPLEMENTED_YET       = 10,
	/** An object handle is NULL. */
	SPUDRESULT_NULL_OBJECT               = 11,
	/** A struct's `base.type` is not the type the call takes there (see
	 * SPUD_STRUCT_BASE). 0 is never a valid type. */
	SPUDRESULT_STRUCT_WRONG_TYPE         = 12,
	/** A struct in a chain is of a type the call does not take in that
	 * chain, or one this build does not know. Never skipped. */
	SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN   = 13,
	/** One type appears twice in a chain. */
	SPUDRESULT_STRUCT_DUPLICATE_IN_CHAIN = 14,
	/** A desc's allocator pointer is NULL (see SPUD_ALLOCATOR). */
	SPUDRESULT_NULL_ALLOCATOR            = 15,
	/** An allocator's `allocate` or `free` is NULL. */
	SPUDRESULT_INVALID_ALLOCATOR         = 16,

	/** spudgpu_create_instance(): the graphics API could not list its GPUs.
	 * No GPU is not this: it is an instance with no adapters. */
	SPUDRESULT_GPU_DEVICE_ENUMERATION_FAILURE                  = 199,
	/** An instance argument is NULL. */
	SPUDRESULT_GPU_INVALID_INSTANCE                            = 200,
	/** A device argument is NULL. */
	SPUDRESULT_GPU_INVALID_DEVICE                              = 201,
	/** A command queue argument is NULL. */
	SPUDRESULT_GPU_INVALID_COMMAND_QUEUE                       = 202,
	/** A command allocator argument is NULL. */
	SPUDRESULT_GPU_INVALID_COMMAND_ALLOCATOR                   = 203,
	/** A command list argument, or an array of them, is NULL. */
	SPUDRESULT_GPU_INVALID_COMMAND_LIST                        = 204,
	/** A swap chain argument is NULL. */
	SPUDRESULT_GPU_INVALID_SWAP_CHAIN                          = 205,
	/** A surface argument, or a desc's surface, is NULL. */
	SPUDRESULT_GPU_INVALID_SURFACE                             = 206,
	/** A format is SPUDGPU_FORMAT_UNKNOWN, or one the call or the backend
	 * can't use there. */
	SPUDRESULT_GPU_INVALID_FORMAT                              = 207,
	/** A surface's window handle is NULL. */
	SPUDRESULT_GPU_INVALID_WINDOW_HANDLE                       = 208,
	/** A surface's display handle is NULL where the platform needs one. */
	SPUDRESULT_GPU_INVALID_DISPLAY_HANDLE                      = 209,
	/** A command list type the call can't take there: a bundle submitted to
	 * a queue, or a type no queue of the device runs. */
	SPUDRESULT_GPU_INVALID_COMMAND_LIST_TYPE                   = 210,
	/** An adapter argument is NULL. */
	SPUDRESULT_GPU_INVALID_ADAPTER                             = 211,
	/** A SPUDGPU_FEATURE_* bit was asked for that the adapter doesn't
	 * support, or a call needs one the device wasn't created with and has
	 * no result of its own for it. */
	SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED                       = 212,
	/** A device desc asks for more queues of one of the adapter's queue
	 * pools than the pool holds. */
	SPUDRESULT_GPU_TOO_MANY_QUEUES                             = 213,
	/** The adapter already has a device: one device per adapter at a time. */
	SPUDRESULT_GPU_ADAPTER_IN_USE                              = 214,
	/* 215 is kept for SPUDRESULT_GPU_DEVICE_LOST (SPUDGPU_DEVICE_TODO.md). */
	/** A descriptor pool, or the bindless table, doesn't fit in what is left
	 * of the device's descriptor budget. */
	SPUDRESULT_GPU_OUT_OF_DESCRIPTORS                          = 216,
	/** A device desc's descriptor budget is above the adapter's limit. */
	SPUDRESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE                 = 217,
	/** A swap chain desc's present_scaling is SPUDGPU_PRESENT_SCALING_NONE
	 * or not exactly one SPUDGPU_PRESENT_SCALING. */
	SPUDRESULT_GPU_INVALID_PRESENT_SCALING                     = 218,
	/** The surface doesn't take the present scaling a swap chain desc names
	 * (spudgpu_get_surface_present_scalings()). */
	SPUDRESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED               = 219,
	/** A memory kind is SPUDGPU_MEMORY_KIND_NONE or not a kind, or not one
	 * the call takes (mapping a buffer that isn't mappable, say). */
	SPUDRESULT_GPU_INVALID_MEMORY_KIND                         = 220,
	/** A buffer usage is SPUDGPU_BUFFER_USAGE_NONE, or one its memory kind
	 * does not allow. */
	SPUDRESULT_GPU_INVALID_BUFFER_USAGE                        = 221,
	/** An image usage is SPUDGPU_IMAGE_USAGE_NONE, or a combination the call
	 * does not allow. */
	SPUDRESULT_GPU_INVALID_IMAGE_USAGE                         = 222,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_BUFFER_STRIDE                       = 223,
	/** An index buffer view's stride is not 2 or 4 bytes. */
	SPUDRESULT_GPU_INVALID_INDEX_STRIDE                        = 224,
	/** The device can't give the memory kind asked for: it wasn't created
	 * with the feature that kind needs. */
	SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED                   = 225,
	/** More color attachments than SPUDGPU_MAX_COLOR_ATTACHMENTS. */
	SPUDRESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS                  = 226,
	/** A pipeline desc's depth bias clamp is not 0 on a device without
	 * SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP. */
	SPUDRESULT_GPU_DEPTH_BIAS_CLAMP_NOT_SUPPORTED              = 227,
	/** A pipeline desc's color attachments blend differently on a device
	 * without SPUDGPU_FEATURE_INDEPENDENT_BLEND. */
	SPUDRESULT_GPU_INDEPENDENT_BLEND_NOT_SUPPORTED             = 228,
	/** A timestamp set on a device without SPUDGPU_FEATURE_PASS_TIMESTAMPS. */
	SPUDRESULT_GPU_TIMESTAMPS_NOT_SUPPORTED                    = 229,
	/** A buffer argument is NULL. */
	SPUDRESULT_GPU_INVALID_BUFFER                              = 230,
	/** A buffer view argument is NULL. */
	SPUDRESULT_GPU_INVALID_BUFFER_VIEW                         = 231,
	/** An image argument is NULL. */
	SPUDRESULT_GPU_INVALID_IMAGE                               = 232,
	/** An image view argument is NULL. */
	SPUDRESULT_GPU_INVALID_IMAGE_VIEW                          = 233,
	/** An image view type the image can't be viewed as, such as a cube view
	 * of an image that isn't cube-capable, or a cube array view on a device
	 * without SPUDGPU_FEATURE_CUBE_ARRAY_VIEWS. */
	SPUDRESULT_GPU_INVALID_IMAGE_VIEW_TYPE                     = 234,
	/** An image type the call can't take, such as a cube-capable image that
	 * isn't SPUDGPU_IMAGE_TYPE_2D. */
	SPUDRESULT_GPU_INVALID_IMAGE_TYPE                          = 235,
	/** A buffer's, or a buffer view's, size is 0. */
	SPUDRESULT_GPU_ZERO_BUFFER_SIZE                            = 236,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_BUFFER_SIZE                         = 237,
	/** A view's range is not within its parent buffer or image, or is not a
	 * range its view type can have (a cube view not of six layers, say). */
	SPUDRESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE     = 238,
	/** A mapped, flushed or invalidated range runs past the end of the
	 * buffer. */
	SPUDRESULT_GPU_MAP_OUT_OF_RANGE                            = 239,
	/** A heap argument is NULL. */
	SPUDRESULT_GPU_INVALID_HEAP                                = 240,
	/** A heap desc's resource class is not a SPUDGPU_HEAP_RESOURCE_CLASS. */
	SPUDRESULT_GPU_INVALID_HEAP_RESOURCE_CLASS                 = 241,
	/** A heap's size is not a multiple of SPUDGPU_HEAP_SIZE_GRANULARITY. */
	SPUDRESULT_GPU_INVALID_HEAP_SIZE                           = 242,
	/** A resource is not of the heap's memory kind or resource class. */
	SPUDRESULT_GPU_HEAP_MISMATCH                               = 243,
	/** A placement's offset is not a multiple of the resource's alignment. */
	SPUDRESULT_GPU_PLACEMENT_MISALIGNED                        = 244,
	/** A placement runs past the end of its heap. */
	SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE                      = 245,
	/** A timestamp set argument is NULL. */
	SPUDRESULT_GPU_INVALID_TIMESTAMP_SET                       = 246,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_SEMAPHORE                           = 250,
	/** A fence argument, or an array of them, is NULL. */
	SPUDRESULT_GPU_INVALID_FENCE                               = 251,
	/** A fence wait's timeout ran out before the fences reached their
	 * values. */
	SPUDRESULT_GPU_FENCE_WAIT_TIMED_OUT                        = 252,
	/** The device is lost: removed, reset after a hang, or its driver
	 * stopped. Nothing more it runs completes, and every object made from
	 * it is only for destroying; a caller that goes on makes a new device.
	 * Returned by spudgpu_wait_for_fences() and the submissions, on Vulkan
	 * and D3D12. Never on Metal: a device on Apple Silicon isn't lost (a
	 * command buffer that faults fails alone, and the device goes on). */
	SPUDRESULT_GPU_DEVICE_LOST                                 = 253,
	/** A shader stage the call can't take, or a pipeline desc that names
	 * both a vertex and a mesh module. */
	SPUDRESULT_GPU_INVALID_SHADER_STAGE                        = 280,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_PRIMITIVE_TOPOLOGY                  = 281,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_CULL_MODE                           = 282,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_COMPARE_OP                          = 283,
	/** A shader module desc's entry point is NULL or empty. */
	SPUDRESULT_GPU_NULL_ENTRY_POINT                            = 284,
	/** A shader module desc's workgroup size has a 0 for a compute, mesh or
	 * task stage, or anything but 0 for another stage. */
	SPUDRESULT_GPU_INVALID_WORKGROUP_SIZE                      = 285,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_BLEND_FACTOR                        = 290,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_BLEND_OP                            = 291,
	/** A shader module is NULL, or has no shader the call can use. */
	SPUDRESULT_GPU_INVALID_SHADER_MODULE                       = 300,
	/** A shader pipeline argument is NULL. */
	SPUDRESULT_GPU_INVALID_SHADER_PIPELINE                     = 301,
	/** A compute pipeline desc's compute module is NULL. */
	SPUDRESULT_GPU_INVALID_COMPUTE_MODULE                      = 302,
	/** A compute pipeline argument is NULL. */
	SPUDRESULT_GPU_INVALID_COMPUTE_PIPELINE                    = 303,
	/** A shader module desc's code is NULL or 0 bytes long. */
	SPUDRESULT_GPU_NULL_SHADER_CODE                            = 304,
	/** A shader module desc's code is not 4-byte aligned, or a SPIR-V or
	 * DXIL size is not a multiple of 4. */
	SPUDRESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT               = 305,
	/** A pipeline desc has no fragment module, or neither a vertex nor a
	 * mesh module. */
	SPUDRESULT_GPU_VERTEX_AND_FRAGMENT_SHADER_REQUIRED         = 306,
	/** A shader module's code isn't the format its desc names, or the
	 * graphics API refused it or a pipeline made from it. */
	SPUDRESULT_GPU_INVALID_SHADER_CODE                         = 307,
	/** A shader module desc's format is not one the device takes. */
	SPUDRESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED                 = 308,
	/** A shader module has no entry point of the desc's name for its
	 * stage. */
	SPUDRESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND                = 309,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_DESCRIPTOR_TYPE                     = 310,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_DESCRIPTOR_POOL_SIZE                = 311,
	/** A descriptor pool argument, or a desc's pool, is NULL. */
	SPUDRESULT_GPU_INVALID_DESCRIPTOR_POOL                     = 312,
	/** A descriptor set layout is NULL, or one the call can't use. */
	SPUDRESULT_GPU_INVALID_DESCRIPTOR_SET_LAYOUT               = 313,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_DESCRIPTOR_SET                      = 314,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_DESCRIPTOR_BUFFER_INFO              = 315,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_DESCRIPTOR_IMAGE_INFO               = 316,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_WRITE_DESCRIPTOR_SET                = 317,
	/** A set layout has more bindings than
	 * SPUDGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET. */
	SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_BINDINGS                = 318,
	/** A descriptor pool desc has more pool sizes than
	 * SPUDGPU_MAX_DESCRIPTOR_POOL_SIZES. */
	SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_POOLS                   = 319,
	/** A descriptor set allocation names no set layouts. */
	SPUDRESULT_GPU_ZERO_DESCRIPTOR_SET_LAYOUTS                 = 320,
	/** A descriptor set allocation names more set layouts than
	 * SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS. */
	SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_SET_LAYOUTS             = 321,
	/** The pool has no room left for the sets asked for, or the graphics
	 * API's allocation of them failed. */
	SPUDRESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL     = 322,
	/** A binding's descriptor type, or its place in the set, has no
	 * equivalent in the graphics API. */
	SPUDRESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE = 323,
	/** The bindless table has no free slot left in that resource's class. */
	SPUDRESULT_GPU_BINDLESS_OUT_OF_SLOTS                       = 324,

	/**
	 * @name Extension not supported by the device
	 *
	 * One SPUDRESULT_GPU_EXT_<NAME>_NOT_SUPPORTED per SPUDGPU_EXT_<NAME>
	 * compile-time capability macro in spudgpu.h that pairs with a
	 * SPUDGPU_FEATURE_<NAME> bit. Returned by that extension's entry points
	 * when the device wasn't created with the feature, which covers a GPU
	 * that lacks it, since such a GPU can't be asked for it. Never returned
	 * when the macro is 0: the extension's functions aren't declared at all
	 * in that case, so calling one is a compile/link error, not a runtime
	 * result.
	 * @{
	 */
	SPUDRESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED          = 325, /**< SPUDGPU_EXT_BINDLESS: the device has no SPUDGPU_FEATURE_BINDLESS. */
	SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED      = 326, /**< SPUDGPU_EXT_MESH_SHADING: the device has no SPUDGPU_FEATURE_MESH_SHADING. */
	SPUDRESULT_GPU_EXT_DEPTH_BOUNDS_TEST_NOT_SUPPORTED = 327, /**< SPUDGPU_EXT_DEPTH_BOUNDS_TEST. Reserved: no call returns it; the depth bounds test does nothing on a device without the feature. */
	SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED         = 328, /**< SPUDGPU_EXT_RAY_QUERY: the device has no SPUDGPU_FEATURE_RAY_QUERY. */
	SPUDRESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED   = 329, /**< SPUDGPU_EXT_GEOMETRY_SHADER: the device has no SPUDGPU_FEATURE_GEOMETRY_SHADER. */
	SPUDRESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED      = 330, /**< SPUDGPU_EXT_TESSELLATION: the device has no SPUDGPU_FEATURE_TESSELLATION. */
	/** @} */

	/** A module's bindings for a set it reads aren't its descriptor set
	 * layout's: it leaves out a binding of the layout, declares one the
	 * layout doesn't hold, or declares one of another type or count
	 * (spudgpu_shader_module_desc, "Every binding of a set"). Returned only
	 * where the backend can see the module's bindings: Metal. */
	SPUDRESULT_GPU_SHADER_LAYOUT_MISMATCH                      = 331,

	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_RESOURCE_STATE                      = 350,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_IMAGE_LAYOUT                        = 351,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_PIPELINE_STAGE                      = 352,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_BUFFER_BARRIER                      = 353,
	/** Reserved: no call returns it. */
	SPUDRESULT_GPU_INVALID_IMAGE_BARRIER                       = 354,

	/**
	 * @name Acceleration structures (SPUDGPU_EXT_RAY_QUERY)
	 * @{
	 */
	/** An acceleration structure is NULL (or, in a Debug build, destroyed),
	 * or not of the level the call takes there. */
	SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE              = 360,
	/** An acceleration structure is smaller than the build or copy writing
	 * it needs. */
	SPUDRESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL            = 361,
	/** A build's scratch range is smaller than the build or update needs. */
	SPUDRESULT_GPU_ACCELERATION_STRUCTURE_SCRATCH_TOO_SMALL    = 362,
	/** A range a build or a size write reads or writes runs past the end of
	 * its buffer. */
	SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE                = 363,
	/** An offset, address or stride is not a multiple of what the call
	 * requires of it, or a stride is below its element's size. */
	SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT            = 364,
	/** A count is past the device's limit (SPUDGPU_RAY_QUERY_LIMITS) or the
	 * structure's own (spudgpu_acceleration_structure_desc::max_bottom_levels). */
	SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT                = 365,
	/** An update its source can't take: the source was never built, was
	 * built without SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE, or with
	 * other flags, geometry kinds, counts or index layout than the update's. */
	SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED       = 366,
	/** A compaction, or a compacted size, its structure wasn't made for: not
	 * created with allow_compaction, or not last built with
	 * SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION. */
	SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED   = 367,
	/** A build, copy or compacted-size write on a command list of a queue
	 * type that can't record one: a copy list or a bundle. */
	SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE    = 368,
	/** A build, copy or compacted-size write between
	 * spudgpu_cmd_begin_rendering() and spudgpu_cmd_end_rendering(). */
	SPUDRESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING     = 369,
	/** @} */

	SPUDRESULT_SPUDFILE_NULL_PATH = 1001, /**< A path is NULL or empty. */
	SPUDRESULT_SPUDFILE_INVALID_FILE = 1002, /**< A file argument is NULL. */
	SPUDRESULT_SPUDFILE_INVALID_MAPPING = 1003, /**< Not returned since 2026-10-22, when spudfile_mapping_release() became void; not reused. */
	SPUDRESULT_SPUDFILE_NOT_READABLE = 1004, /**< Mapping a file not opened for reading. */
	SPUDRESULT_SPUDFILE_DIFFERENT_VOLUME = 1005, /**< Replace across volumes (EXDEV / ERROR_NOT_SAME_DEVICE). */
	SPUDRESULT_SPUDFILE_IN_USE = 1006, /**< Replace target in use or read-only (Windows). */
	SPUDRESULT_SPUDFILE_FLUSH_LEVEL_NOT_SUPPORTED = 1007, /**< The file system can't flush to the level asked for. */
	SPUDRESULT_SPUDFILE_INVALID_INSTANCE = 1008, /**< A SpudFiles instance is NULL. */
	SPUDRESULT_SPUDFILE_NOT_FOUND = 1009, /**< Nothing is at the path, or a directory on the way to it isn't. */
	SPUDRESULT_SPUDFILE_ALREADY_EXISTS = 1010, /**< Something is at the path already. */
	SPUDRESULT_SPUDFILE_ACCESS_DENIED = 1011, /**< The platform's permissions refuse it. */
	SPUDRESULT_SPUDFILE_NOT_A_FILE = 1012, /**< A directory, or another kind of entry, is where a file is wanted. */
	SPUDRESULT_SPUDFILE_NOT_WRITABLE = 1013, /**< Writing to a file not opened for writing. */
	SPUDRESULT_SPUDFILE_END_OF_LISTING = 1014, /**< A listing has given every entry. An answer, not a failure. */
	SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY = 1015, /**< A file, or another kind of entry, is where a directory is wanted. */
	SPUDRESULT_SPUDFILE_LOCKED = 1016, /**< Another lock holds the file, and the call was not to wait. */
	SPUDRESULT_SPUDFILE_INVALID_LISTING = 1017, /**< A listing argument is NULL. */
	SPUDRESULT_SPUDFILE_ALREADY_LOCKED = 1018, /**< The file holds a lock taken through it already. */
	SPUDRESULT_SPUDFILE_NOT_LOCKED = 1019, /**< The file holds no lock taken through it. */
	SPUDRESULT_SPUDFILE_NOT_EMPTY = 1020, /**< A directory to remove holds entries. */
	SPUDRESULT_SPUDFILE_INVALID_LIBRARY = 1021, /**< A library argument is NULL. */
	SPUDRESULT_SPUDFILE_LIBRARY_NOT_LOADED = 1022, /**< The platform's loader didn't load the library; the instance's message callback has its words. */
	SPUDRESULT_SPUDFILE_SYMBOL_NOT_FOUND = 1023, /**< The library exports no function of that name. */
	SPUDRESULT_SPUDFILE_LIBRARY_OPTION_NOT_SUPPORTED = 1024, /**< The platform's loader has no such choice. */

	/* 2001-2003 and 2026 (ZERO_CAPACITY, INSUFFICIENT_CAPACITY,
	 * PLAT_COMMIT_FAIL, INVALID_ARENA) went with SpudMemory's arena and are
	 * not reused. */
	SPUDRESULT_SPUDMEM_NULL_ADDRESS = 2027, /**< An address passed in is NULL. */
	SPUDRESULT_SPUDMEM_NOT_PAGE_ALIGNED = 2028, /**< A size or address is not a multiple of the page size. */

	SPUDRESULT_SPUDNET_STARTUP_FAILED        = 3001, /**< spudnet_instance_create(): the platform's networking wouldn't start. */
	SPUDRESULT_SPUDNET_INVALID_TCP_SOCKET    = 3002, /**< A TCP socket is NULL, not open, or already connected. */
	SPUDRESULT_SPUDNET_RESOLVE_FAILED        = 3003, /**< A host name could not be resolved to an address. */
	SPUDRESULT_SPUDNET_CONNECT_FAILED        = 3004, /**< The connection to the other end could not be made. */
	SPUDRESULT_SPUDNET_LISTEN_FAILED         = 3005, /**< A listener could not be bound or set listening. */
	SPUDRESULT_SPUDNET_ACCEPT_FAILED         = 3006, /**< A connection could not be accepted. */
	SPUDRESULT_SPUDNET_SEND_FAILED           = 3007, /**< Data could not be sent. */
	SPUDRESULT_SPUDNET_RECV_FAILED           = 3008, /**< Data could not be received. */
	/* 3009 and 3010 (WOULD_BLOCK, SET_BLOCKING_FAILED) went with SpudNet's
	 * non-blocking mode and are not reused. */
	SPUDRESULT_SPUDNET_INVALID_URL           = 3011, /**< Not a URL, or a scheme the call doesn't carry. */
	SPUDRESULT_SPUDNET_INVALID_HTTP_CLIENT   = 3012, /**< An HTTP client argument is NULL. */
	SPUDRESULT_SPUDNET_INVALID_WEBSOCKET     = 3013, /**< A WebSocket is NULL, not open, or already connected. */
	SPUDRESULT_SPUDNET_TLS_FAILED            = 3014, /**< Handshake or certificate check failed. */
	SPUDRESULT_SPUDNET_TIMED_OUT             = 3015, /**< The call's own time limit ran out, and nothing else. */
	SPUDRESULT_SPUDNET_HTTP_FAILED           = 3016, /**< An HTTP transfer's start or its wait for the response failed. */
	SPUDRESULT_SPUDNET_WS_UPGRADE_REFUSED    = 3017, /**< The server answered, but not with 101. */
	SPUDRESULT_SPUDNET_WS_CLOSED             = 3018, /**< Closed by a close frame, either side's. */
	SPUDRESULT_SPUDNET_WS_MESSAGE_TOO_BIG    = 3019, /**< Over the connect desc's max_message_size. */
	SPUDRESULT_SPUDNET_UNSUPPORTED           = 3020, /**< This build's platform stack doesn't have it. */
	SPUDRESULT_SPUDNET_ABORTED               = 3022, /**< The object's _abort was called. */
	SPUDRESULT_SPUDNET_INVALID_TCP_LISTENER  = 3024, /**< A TCP listener argument is NULL. */
	SPUDRESULT_SPUDNET_INVALID_ADDRESS       = 3025, /**< Not a numeric IPv4 or IPv6 address. */
	SPUDRESULT_SPUDNET_ADDRESS_IN_USE        = 3026, /**< Another listener already has that address and port. */
	SPUDRESULT_SPUDNET_INVALID_WAIT_SET      = 3027, /**< A wait set argument is NULL. */
	SPUDRESULT_SPUDNET_ALREADY_IN_WAIT_SET   = 3028, /**< The object is in this set already, or a WebSocket in another. */
	SPUDRESULT_SPUDNET_INVALID_RESOLVER      = 3029, /**< NULL, or one whose lookup has already been run. */
	SPUDRESULT_SPUDNET_SYSTEM_TIMED_OUT      = 3030, /**< TCP: the operating system gave up waiting for the other end. */
	SPUDRESULT_SPUDNET_STACK_TIMED_OUT       = 3031, /**< HTTP, WebSocket: the client stack gave up waiting, or says something under it did. */
	SPUDRESULT_SPUDNET_INVALID_HTTP_TRANSFER = 3032, /**< NULL, or a call out of the transfer's order of use. */
	SPUDRESULT_SPUDNET_INVALID_INSTANCE      = 3033, /**< NULL, or not the instance the other object was made with. */

	SPUDRESULT_SAUD_INVALID_INSTANCE      = 4001, /**< A SpudAudio instance argument is NULL. */
	SPUDRESULT_SAUD_INVALID_DEVICE        = 4002, /**< An audio device argument, or a desc's device, is NULL. */
	SPUDRESULT_SAUD_INVALID_STREAM        = 4003, /**< An audio stream argument is NULL. */
	SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED  = 4004, /**< The endpoint can't take the format as written. */
	SPUDRESULT_SAUD_EXCLUSIVE_UNAVAILABLE = 4005, /**< Exclusive mode is held by another client, or the backend has none. */
	SPUDRESULT_SAUD_DEVICE_LOST           = 4006, /**< The device disappeared since enumeration. */
	SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE   = 4007, /**< A period of 0, or one the OS doesn't grant. */
	SPUDRESULT_SAUD_BUFFER_OUT_OF_RANGE   = 4008, /**< A buffer size below the period, set where the backend has no separate buffer, or one the OS doesn't grant. */
	SPUDRESULT_SAUD_THREAD_PRIORITY_UNSUPPORTED = 4009, /**< The backend doesn't have the scheduling mechanism asked for. */
	SPUDRESULT_SAUD_THREAD_PRIORITY_DENIED      = 4010, /**< The OS refused the thread priority asked for when the stream's thread started. */
	SPUDRESULT_SAUD_SAMPLE_RATE_NOT_SETTABLE    = 4011, /**< The platform has no device-wide sample rate to set. */
	SPUDRESULT_SAUD_OS_CONVERSION_UNAVAILABLE   = 4012, /**< ALLOW_OS_CONVERSION was set where the platform can't convert for the request. */
	SPUDRESULT_SAUD_ACCESS_DENIED               = 4013, /**< The platform hasn't granted this process use of the endpoint (iOS: record permission not granted). */
	SPUDRESULT_SAUD_INTERRUPTED                 = 4014, /**< The platform's audio session took the endpoint from this process, or won't give it now: an interruption (a call, an alarm) or another app's audio that doesn't mix (iOS). */
	SPUDRESULT_SAUD_SESSION_INACTIVE            = 4015, /**< The platform's audio session, which the caller owns, isn't active, and the platform won't run a stream on it (iOS). */

	SPUDRESULT_SPUDINPUT_INVALID_INSTANCE      = 5001, /**< A SpudInput instance argument is NULL. */
	SPUDRESULT_SPUDINPUT_GAMEPAD_NOT_CONNECTED = 5002, /**< The id names no gamepad as of the last poll. */
	SPUDRESULT_SPUDINPUT_UNSUPPORTED           = 5003, /**< The native API doesn't offer the choice asked of it. */
	SPUDRESULT_SPUDINPUT_FEATURE_NOT_PRESENT   = 5004, /**< The device doesn't have what the call is about. */
	SPUDRESULT_SPUDINPUT_ACCESS_DENIED         = 5005, /**< The platform won't let this process read or write the device. */

} SPUDRESULT;

/**
 * @brief Tests whether a SPUDRESULT is a failure.
 *
 * @param[in] sr The SPUDRESULT to test. Evaluated once.
 *
 * @return Nonzero if `sr` is anything other than SPUD_SUCCESS, 0 if it is
 *         SPUD_SUCCESS.
 */
#define SPUDFAIL(sr) ((sr) != SPUD_SUCCESS)

/**
 * @defgroup spud_struct_base Struct bases
 * @brief How a struct that may gain fields is extended without changing
 * its layout.
 *
 * An extensible struct's first member is a SPUD_STRUCT_BASE (one the
 * caller fills in) or a SPUD_STRUCT_BASE_OUT (one SpudLib fills in), named
 * `base`. `base.type` says which struct it is; `base.next` points to the
 * next struct in its chain, or is NULL. A struct's layout never changes
 * once it is released: a new field comes as a new struct with a base of
 * its own, which the caller chains on. A struct with no base is frozen and
 * never changes at all; a new need gets a new type.
 *
 * - The caller sets every `base.type` in a chain, the root's included.
 *   A type that isn't the one the call takes is refused with
 *   SPUDRESULT_STRUCT_WRONG_TYPE.
 * - A chained struct the call doesn't take there, or this build doesn't
 *   know, is refused with SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN, never
 *   skipped: skipping it would decide for the caller that it didn't
 *   matter. One type twice in a chain is refused with
 *   SPUDRESULT_STRUCT_DUPLICATE_IN_CHAIN.
 * - Leaving a chained struct out means that addition isn't used. Each
 *   chained struct's fields have no defaults, as no desc's do.
 *
 * ```c
 * spudgpu_image_desc desc = {
 * 	.base = { SPUD_STRUCT_TYPE_GPU_IMAGE_DESC, NULL },
 * 	...
 * };
 * ```
 *
 * The member is nested, not two loose fields, so that a pointer to any based
 * struct converts to a pointer to its base and back (C11 6.7.2.1p15): a
 * chain is walked through SPUD_STRUCT_BASE pointers with defined behaviour.
 * @{
 */

/**
 * @brief Which struct a based struct is.
 *
 * Numbered in blocks per module, as SPUDRESULT is: 1-999 SpudCore, 1000+ SpudGPU, 2000+
 * SpudAudio, 3000+ SpudNet, 4000+ SpudInput, 5000+ SpudFiles. A value is
 * never renumbered or reused, a retired struct's included.
 */
typedef enum SPUD_STRUCT_TYPE {
	/** Never valid: a struct left zeroed is refused, not misread. */
	SPUD_STRUCT_TYPE_UNKNOWN  = 0,

	/* SpudCore: 1-999. */
	/** SPUD_MESSAGE (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_MESSAGE                        = 1,

	/* SpudGPU. Retired, never reused: 1000 (SPUDGPU_DEVICE_PROPERTIES),
	 * 1017 (spudgpu_bindless_capabilities), 1020
	 * (spudgpu_mesh_shading_capabilities), 1021
	 * (spudgpu_depth_bounds_capabilities). */
	/** spudgpu_submit_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_SUBMIT_DESC                = 1001,
	/** spudgpu_command_allocator_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC     = 1002,
	/** spudgpu_buffer_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_BUFFER_DESC                = 1003,
	/** spudgpu_buffer_view_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC           = 1004,
	/** spudgpu_image_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_IMAGE_DESC                 = 1005,
	/** spudgpu_image_view_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC            = 1006,
	/** spudgpu_heap_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_HEAP_DESC                  = 1007,
	/** spudgpu_placement_requirements (SPUD_STRUCT_BASE_OUT). */
	SPUD_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS     = 1008,
	/** spudgpu_buffer_barrier (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER             = 1009,
	/** spudgpu_image_barrier (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_IMAGE_BARRIER              = 1010,
	/** spudgpu_swap_chain_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC            = 1011,
	/** spudgpu_descriptor_set_layout_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC = 1012,
	/** spudgpu_descriptor_pool_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC       = 1013,
	/** spudgpu_descriptor_set_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC        = 1014,
	/** spudgpu_sampler_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_SAMPLER_DESC               = 1015,
	/** spudgpu_write_descriptor_set (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET       = 1016,
	/** spudgpu_shader_module_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_SHADER_MODULE_DESC         = 1018,
	/** spudgpu_shader_pipeline_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC       = 1019,
	/** spudgpu_compute_pipeline_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC      = 1022,
	/** spudgpu_rendering_begin_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC       = 1023,
	/** spudgpu_bundle_inheritance_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_BUNDLE_INHERITANCE_DESC    = 1024,
	/** spudgpu_instance_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_INSTANCE_DESC              = 1025,
	/** SPUDGPU_ADAPTER_PROPERTIES (SPUD_STRUCT_BASE_OUT). */
	SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES         = 1026,
	/** spudgpu_device_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_DEVICE_DESC                = 1027,
	/** SPUDGPU_MESH_SHADING_LIMITS (SPUD_STRUCT_BASE_OUT), chained onto
	 * SPUDGPU_ADAPTER_PROPERTIES. */
	SPUD_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS        = 1028,
	/** SPUDGPU_BINDLESS_LIMITS (SPUD_STRUCT_BASE_OUT), chained onto
	 * SPUDGPU_ADAPTER_PROPERTIES. */
	SPUD_STRUCT_TYPE_GPU_BINDLESS_LIMITS            = 1029,
	/** SPUDGPU_RAY_QUERY_LIMITS (SPUD_STRUCT_BASE_OUT), chained onto
	 * SPUDGPU_ADAPTER_PROPERTIES. */
	SPUD_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS           = 1030,
	/** spudgpu_acceleration_structure_inputs (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS  = 1031,
	/** spudgpu_acceleration_structure_sizes (SPUD_STRUCT_BASE_OUT). */
	SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES   = 1032,
	/** spudgpu_acceleration_structure_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC    = 1033,
	/** spudgpu_acceleration_structure_build (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD   = 1034,
	/** spudgpu_acceleration_structure_barrier (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER = 1035,

	/* SpudAudio */
	/** SPUDAUDIO_DEVICE_PROPERTIES (SPUD_STRUCT_BASE_OUT). */
	SPUD_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES        = 2000,
	/** SPUDAUDIO_TIMING_CAPS (SPUD_STRUCT_BASE_OUT). */
	SPUD_STRUCT_TYPE_AUDIO_TIMING_CAPS              = 2001,
	/** SPUDAUDIO_STREAM_DESC (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_AUDIO_STREAM_DESC              = 2002,
	/** SPUDAUDIO_STREAM_CONFIG (SPUD_STRUCT_BASE_OUT). */
	SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG            = 2003,
	/** SPUDAUDIO_INSTANCE_DESC (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_AUDIO_INSTANCE_DESC            = 2004,

	/* SpudNet */
	/** spudnet_resolver_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_NET_RESOLVER_DESC              = 3000,
	/** spudnet_tcp_listener_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_NET_TCP_LISTENER_DESC          = 3001,
	/** spudnet_tcp_socket_connect_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC    = 3002,
	/** spudnet_http_client_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_NET_HTTP_CLIENT_DESC           = 3003,
	/** spudnet_http_transfer_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC         = 3004,
	/** spudnet_websocket_connect_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC     = 3005,
	/** spudnet_instance_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_NET_INSTANCE_DESC              = 3006,

	/* SpudInput */
	/** SPUDINPUT_INSTANCE_DESC (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_INPUT_INSTANCE_DESC            = 4000,
	/** SPUDINPUT_GAMEPAD_PROPERTIES (SPUD_STRUCT_BASE_OUT). */
	SPUD_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES       = 4001,

	/* SpudFiles */
	/** spudfile_open_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_FILES_OPEN_DESC                = 5000,
	/** spudfile_instance_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_FILES_INSTANCE_DESC            = 5001,
	/** SPUDFILE_PATH_INFO (SPUD_STRUCT_BASE_OUT). */
	SPUD_STRUCT_TYPE_FILES_PATH_INFO                = 5002,
	/** spudfile_listing_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_FILES_LISTING_DESC             = 5003,
	/** SPUDFILE_LISTING_ENTRY (SPUD_STRUCT_BASE_OUT). */
	SPUD_STRUCT_TYPE_FILES_LISTING_ENTRY            = 5004,
	/** spudfile_library_desc (SPUD_STRUCT_BASE). */
	SPUD_STRUCT_TYPE_FILES_LIBRARY_DESC             = 5005,

	/** Keeps the enum 32 bits wide on every compiler. Never a valid type. */
	SPUD_STRUCT_TYPE_MAX_ENUM = 0x7FFFFFFF
} SPUD_STRUCT_TYPE;

/** @brief The first member, `base`, of every extensible struct the caller
 * fills in. */
/* Frozen: the base every based struct starts with. It never changes; a new need gets a new type. */
typedef struct SPUD_STRUCT_BASE {
	/** This struct's own type. */
	SPUD_STRUCT_TYPE type;
	/** The next struct in the chain, or NULL. SpudLib only reads it. */
	const struct SPUD_STRUCT_BASE *next;
} SPUD_STRUCT_BASE;

/** @brief The first member, `base`, of every extensible struct SpudLib
 * fills in for the caller. The caller builds the chain and sets each
 * `type`; SpudLib writes the other fields and leaves `type` and `next` as
 * they are. */
/* Frozen: the base every based struct starts with. It never changes; a new need gets a new type. */
typedef struct SPUD_STRUCT_BASE_OUT {
	/** This struct's own type. */
	SPUD_STRUCT_TYPE type;
	/** The next struct in the chain, or NULL. */
	struct SPUD_STRUCT_BASE_OUT *next;
} SPUD_STRUCT_BASE_OUT;

/** @} */

/**
 * @defgroup spud_messages Messages
 * @brief What the platform said when it refused a call, delivered to the
 * caller's callback.
 *
 * A SPUDRESULT is the same on every platform; the platform's own number
 * behind a failure (a GetLastError value, an errno, a VkResult, an
 * HRESULT, an OSStatus, a loader's words) is not. It reaches the caller as
 * a message, through a callback the caller gives the module's instance.
 * The rules, for every module whose instance desc takes one (SpudFiles'
 * spudfile_instance_desc so far):
 *
 * - M1. A module's instance desc takes one message callback and its user
 *   data. NULL means no messages. Every object made from the instance
 *   reports to it.
 * - M2. Every failure a platform call reports is a message
 *   (SPUD_MESSAGE_ORIGIN_PLATFORM, severity ERROR), whatever SPUDRESULT the
 *   call returns for it: SPUDRESULT_API_SPECIFIC_FAILURE, an out-of-memory
 *   result, and equally a code of its own (SPUDRESULT_SPUDFILE_NOT_FOUND,
 *   from ENOENT or ENOTDIR, which the message tells apart). A `void` call
 *   sends the failure the platform reported to it. None is judged too
 *   routine to send: the caller filters on `result` in its handler.
 *   Nothing is sent for an answer the platform gives as an outcome
 *   (would-block, the end of a listing, a wait's own timeout, a peer's
 *   orderly close), for a time limit SpudLib keeps itself, or for a refused
 *   argument or another check of SpudLib's own. At every validation level,
 *   off included.
 * - M3. The callback is called inside the call that failed, on the thread
 *   that made it, before the call returns. Calls on different objects may
 *   run at the same time (@ref spud_threads "Threads"), so the callback may
 *   be called from several of the caller's threads at once, and must allow
 *   it. SpudLib never calls it from a thread of its own or a platform's.
 *
 * Thread: a message callback is called on the thread of the call that
 * failed, so on several of the caller's threads at once where the caller
 * makes calls on different objects at once (M3).
 *
 * - M4. The callback makes no call into the module whose call is reporting
 *   (a SpudFiles message's handler makes no SpudFiles call); another
 *   module is fine. It returns normally: no C++ exception, Swift or C#
 *   error, or longjmp unwinds through SpudLib. SpudLib holds no lock of its
 *   own while calling it.
 * - M5. Everything the message points to is valid only during the
 *   callback: the caller copies what it keeps. Sending a message allocates
 *   nothing.
 * - M6. A call with no instance to report to takes a
 *   SPUD_PLATFORM_ERROR *out_error instead, which may be NULL: the number
 *   alone.
 * - M7. Messages go to whoever created the instance. A caller that is only
 *   handed an object made from it doesn't see them: the failing call's
 *   SPUDRESULT is what it has.
 * @{
 */

/** @brief Which of the platform's numberings a code is in. The number means
 * nothing without it. */
typedef enum SPUD_PLATFORM_ERROR_SOURCE {
	/** No platform code: the message isn't a platform failure, or nothing
	 * was recorded. */
	SPUD_PLATFORM_ERROR_SOURCE_NONE = 0,
	/** GetLastError and WSAGetLastError values, ERROR_WINHTTP_* among them,
	 * and HRESULTs (D3D12, DXGI, WASAPI, GameInput, CryptoAPI's CERT_E_* and
	 * TRUST_E_*). FormatMessage reads all of them. */
	SPUD_PLATFORM_ERROR_SOURCE_WIN32 = 1,
	/** errno, from POSIX calls, ALSA's negative returns (negated) and evdev,
	 * or reported through NSPOSIXErrorDomain. */
	SPUD_PLATFORM_ERROR_SOURCE_ERRNO = 2,
	/** getaddrinfo's EAI_* (Linux and Apple; Winsock's resolver reports
	 * WSA* codes, which are SPUD_PLATFORM_ERROR_SOURCE_WIN32). */
	SPUD_PLATFORM_ERROR_SOURCE_RESOLVER = 3,
	/** A libcurl CURLcode. */
	SPUD_PLATFORM_ERROR_SOURCE_CURL = 4,
	/** NSURLErrorDomain. */
	SPUD_PLATFORM_ERROR_SOURCE_NSURL = 5,
	/** An OSStatus: Core Audio, Security, NSOSStatusErrorDomain. */
	SPUD_PLATFORM_ERROR_SOURCE_OSSTATUS = 6,
	/** The network stack had no complaint; the certificate matched none of
	 * the pins it was given. The code is 0. */
	SPUD_PLATFORM_ERROR_SOURCE_TLS_PIN = 7,
	/** An NSError from a domain not listed here; the text starts with the
	 * domain's name. */
	SPUD_PLATFORM_ERROR_SOURCE_OTHER = 8,
	/** A VkResult. */
	SPUD_PLATFORM_ERROR_SOURCE_VKRESULT = 9,
	/** A dynamic loader's message with no number (dlerror on Linux and
	 * Apple): the code is 0 and the text is the loader's. */
	SPUD_PLATFORM_ERROR_SOURCE_DLERROR = 10,
} SPUD_PLATFORM_ERROR_SOURCE;

/** @brief Who a message comes from. */
typedef enum SPUD_MESSAGE_ORIGIN {
	/** The platform refused a call: `platform_source` and `platform_code`
	 * say how. */
	SPUD_MESSAGE_ORIGIN_PLATFORM = 1,
	/** The module's own check layer. */
	SPUD_MESSAGE_ORIGIN_CHECK = 2,
	/** The platform's validation layer: D3D12's debug layer, Vulkan's
	 * validation layers, Metal's API validation. */
	SPUD_MESSAGE_ORIGIN_NATIVE_VALIDATION = 3,
	/** GPU-based validation. */
	SPUD_MESSAGE_ORIGIN_GPU_VALIDATION = 4,
} SPUD_MESSAGE_ORIGIN;

/** @brief How bad a message is: every level a platform's own layers report,
 * so none is merged into another. */
typedef enum SPUD_MESSAGE_SEVERITY {
	/** Vulkan's VERBOSE, D3D12's MESSAGE. */
	SPUD_MESSAGE_SEVERITY_VERBOSE = 1,
	/** Information. */
	SPUD_MESSAGE_SEVERITY_INFO = 2,
	/** A warning. */
	SPUD_MESSAGE_SEVERITY_WARNING = 3,
	/** An error; every platform failure. */
	SPUD_MESSAGE_SEVERITY_ERROR = 4,
	/** D3D12's CORRUPTION: the layer judges the device's state corrupt. */
	SPUD_MESSAGE_SEVERITY_CORRUPTION = 5,
} SPUD_MESSAGE_SEVERITY;

/** @brief One message, handed to the callback; read only during it (M5).
 * Set by SpudLib: a newer SpudLib may chain more onto `base.next`, which a
 * caller that doesn't know the type skips. */
typedef struct SPUD_MESSAGE {
	/** `type` is SPUD_STRUCT_TYPE_MESSAGE; `next` is NULL, or a struct a
	 * later version chains. Set by SpudLib. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** Who the message comes from. */
	SPUD_MESSAGE_ORIGIN origin;
	/** How bad it is; ERROR for every platform failure. */
	SPUD_MESSAGE_SEVERITY severity;
	/** For a platform failure, which numbering `platform_code` is in;
	 * SPUD_PLATFORM_ERROR_SOURCE_NONE otherwise. */
	SPUD_PLATFORM_ERROR_SOURCE platform_source;
	/** For a platform failure, what the call returns for it (M2), or for a
	 * `void` call what it would have returned; SPUD_SUCCESS for the other
	 * origins. */
	SPUDRESULT result;
	/** The check's or layer's message id; 0 for a platform failure. */
	uint32_t id;
	/** For a failure sent from an object's pending slot, how many more the
	 * slot overwrote since the last one sent; 0 otherwise. */
	uint32_t dropped;
	/** The platform's number, as it gave it; 0 otherwise. */
	int64_t platform_code;
	/** The call that was running, a literal ("spudfile_file_open"). */
	const char *call;
	/** The handle the call was made on (for a create, the handle it creates
	 * from), or NULL for an instance create. */
	const void *object;
	/** `object`'s debug name, or NULL. */
	const char *object_debug_name;
	/** For a create, the debug name its desc gave the object it was making,
	 * or NULL; NULL for other calls. */
	const char *subject_debug_name;
	/** The wording, UTF-8, null-terminated, whole. For a platform failure:
	 * the platform's own where it has some (a loader's dlerror text, an
	 * NSError's description); else, for a code no system function reads,
	 * the code's name, a literal; else empty (errno and Win32 codes, which
	 * strerror and FormatMessage read). For the other origins, the layer's
	 * or check's message. In the system's language; not for parsing. Never
	 * NULL. */
	const char *text;
} SPUD_MESSAGE;

/** @brief A caller's handler for messages (M1-M7).
 * @param[in] message   The message, valid only during the call.
 * @param[in] user_data The instance desc's `message_user_data`. */
typedef void (*SPUD_MESSAGE_CALLBACK)(const SPUD_MESSAGE *message, void *user_data);

/** @brief The platform's error behind a failure, for a call with no
 * instance to send a message to (M6). */
/* Frozen: filled in per call, like every error struct. It never changes; a new need gets a new type. */
typedef struct SPUD_PLATFORM_ERROR {
	/** What the call returned; SPUD_SUCCESS if it didn't fail on the
	 * platform. */
	SPUDRESULT result;
	/** Which numbering `code` is in; NONE when nothing is recorded. */
	SPUD_PLATFORM_ERROR_SOURCE source;
	/** The platform's number, as it gave it. */
	int64_t code;
} SPUD_PLATFORM_ERROR;

/** @} */

/**
 * @defgroup spud_allocator The caller's allocator
 * @brief Where SpudLib's own memory comes from.
 *
 * SpudLib never chooses where its memory comes from. Each module's
 * instance is made with a SPUD_ALLOCATOR, and SpudLib takes every block
 * it needs for that instance, and for every object made from it, from that
 * allocator alone: the handles, the copies it keeps (debug names, descs),
 * and its own lists and tables. An object never has an allocator of its
 * own, and a destroy call never takes one: an object frees through the
 * allocator of the instance it was made from.
 *
 * Memory that the platform, the operating system or a graphics driver
 * allocates for itself is not covered, because those offer SpudLib no way
 * to pass an allocator on.
 *
 * There is no default. A create call given no allocator refuses it with
 * SPUDRESULT_NULL_ALLOCATOR, and one missing either function with
 * SPUDRESULT_INVALID_ALLOCATOR. spud_allocator_system() is the system heap,
 * for a caller that chooses it.
 *
 * The rules an allocator keeps:
 *
 * - `allocate` returns a block of at least `size` bytes whose address is a
 *   multiple of `alignment`, or NULL if it can't. SpudLib then fails the
 *   call it was making with SPUDRESULT_OUT_OF_MEMORY and leaves everything
 *   as it was before the call. The block's contents are undefined.
 * - SpudLib never asks for 0 bytes. `alignment` is always a power of two,
 *   1 or more. An allocator that can't meet an alignment returns NULL.
 * - `free` is called once for each block, with the same `size` and
 *   `alignment` the block was allocated with, so an allocator need not
 *   record either. It is never called with NULL, and it can't fail.
 * - Thread: either function may be called on any thread, and by calls
 *   that overlap: different objects of one instance are independent
 *   (@ref spud_threads "Threads"), so their calls may allocate at once,
 *   and a few calls allocate on a thread that is not the caller's, each
 *   marked so in its module's header (SpudAudio's device-event watch,
 *   SpudInput's GameInput device callback). An allocator that can't take
 *   overlapping calls is the caller's to keep to itself, by using that
 *   instance and everything made from it one call at a time and leaving
 *   those calls out.
 * - Neither function may call SpudLib.
 * - SpudLib copies the SPUD_ALLOCATOR when the instance is made, so the
 *   caller's struct need not outlive the call. The functions and
 *   `user_data` must stay valid until the instance and every object made
 *   from it are destroyed, and by then every block SpudLib took from them
 *   has been freed. Where a module lets an object's memory outlast its
 *   destroy call, its header says until when (a SpudNet resolver whose
 *   lookup is still running).
 * @{
 */

/**
 * @brief Allocates a block for SpudLib.
 *
 * @param[in] user_data The allocator's `user_data`, passed as it was given.
 * @param[in] size      The bytes wanted. Never 0.
 * @param[in] alignment What the address must be a multiple of. A power of
 *                      two, 1 or more.
 *
 * @return The block, or NULL if it can't be given.
 */
typedef void *(*SPUD_ALLOCATE_FUNCTION)(
    void *user_data,
    size_t size,
    size_t alignment);

/**
 * @brief Frees a block SPUD_ALLOCATE_FUNCTION gave.
 *
 * @param[in] user_data The allocator's `user_data`, passed as it was given.
 * @param[in] memory    The block. Never NULL.
 * @param[in] size      The `size` it was allocated with.
 * @param[in] alignment The `alignment` it was allocated with.
 */
typedef void (*SPUD_FREE_FUNCTION)(
    void *user_data,
    void *memory,
    size_t size,
    size_t alignment);

/** @brief The caller's allocator: two functions and the state they share. */
/* Frozen: a table of functions. A new function or hint is a new struct. */
typedef struct SPUD_ALLOCATOR {
	/** Passed to both functions as it is. SpudLib never reads it. May be NULL. */
	void *user_data;
	/** Gives a block. Not NULL. */
	SPUD_ALLOCATE_FUNCTION allocate;
	/** Frees a block `allocate` gave. Not NULL. */
	SPUD_FREE_FUNCTION free;
} SPUD_ALLOCATOR;

/**
 * @brief Returns an allocator over the system heap.
 *
 * Exists so that a caller choosing the system heap says so by name. It is
 * the C library's heap (the CRT's aligned functions on Windows), with a
 * NULL `user_data`. Calling it allocates nothing; it only fills in the
 * struct.
 *
 * Thread: the allocator it returns takes overlapping calls from any
 * threads, as the C library's heap does.
 *
 * @return The allocator, by value.
 */
SPUD_ALLOCATOR spud_allocator_system(void);

/** @} */

/**
 * @brief Returns the name of a SPUDRESULT enumerator as a string.
 *
 * For logs and error messages. The string is the enumerator's identifier
 * exactly as it is spelled in this header, such as "SPUDRESULT_NULL_DESC".
 *
 * @param[in] r The result to name.
 *
 * @return A null-terminated string literal: valid for the life of the
 *         program, and not to be freed or modified. Never NULL.
 * @retval "SPUDRESULT_UNKNOWN" `r` is not one of SPUDRESULT's values. Every
 *         value has its own name.
 */
const char *spudresult_str(SPUDRESULT r);

/**
 * @brief Gives a SpudLib object a debug name, replacing any it already has.
 *
 * The string is copied, through the allocator of the instance the object
 * was made from (SPUD_ALLOCATOR), so it need not outlive the call. The
 * object's previous name is freed, and the object's own destroy call frees the name it
 * holds at that point. Exists in every build, and the name is kept in every
 * build: a debug name never changes a struct's layout or a library's symbols.
 *
 * @param[in,out] object A handle to an object that carries a debug name.
 *                       Every SpudGPU, SpudAudio and SpudNet handle does, and
 *                       so does an spudfile_file. The name it holds is replaced.
 * @param[in]     name   The new name, a null-terminated string, or NULL to
 *                       remove the current one. May be the object's own
 *                       current name.
 *
 * @retval SPUD_SUCCESS The name was replaced.
 * @retval SPUDRESULT_NULL_OBJECT `object` is NULL.
 * @retval SPUDRESULT_OUT_OF_MEMORY The copy could not be allocated. The
 *         object keeps the name it had.
 */
SPUDRESULT spud_debug_name_set(void *object, const char *name);

/**
 * @brief Calls spud_debug_name_set(), casting the handle to `void *`.

 *
 * @param[in] obj  A handle of any type that carries a debug name.
 * @param[in] name The new name, or NULL to remove the current one.
 *
 * @return What spud_debug_name_set() returns.
 */
#define SPUD_SET_DEBUG_NAME(obj, name)                                         \
	spud_debug_name_set((void *)(obj), (name))

/**
 * @brief Returns an object's debug name.

 *
 * @param[in] object A handle to an object that carries a debug name (see
 *                   spud_debug_name_set()), or NULL.
 *
 * @return The object's name, a null-terminated string. It belongs to the
 *         object: it stays valid until the name is next set or the object is
 *         destroyed, and is not to be freed or modified.
 * @retval NULL `object` is NULL, or the object has no name.
 */
const char *spud_debug_name_get(void *object);

/**
 * @brief Calls spud_debug_name_get(), casting the handle to `void *`.

 *
 * @param[in] obj A handle of any type that carries a debug name, or NULL.
 *
 * @return What spud_debug_name_get() returns.
 */
#define SPUD_GET_DEBUG_NAME(obj) spud_debug_name_get((void *)(obj))

#if __cplusplus
}
#endif

#endif // SPUDCORE_H
