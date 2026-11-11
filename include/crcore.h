/**
 * @file crcore.h
 * @brief Carrottus Core: what every Carrottus module shares.
 *
 * The CR_RESULT enum all modules return, the library's version, the
 * fixed-width integer limits, struct bases, the caller's allocator and the
 * debug-name functions. Platform-agnostic: nothing here calls a platform
 * API.
 */

#ifndef CRCORE_H
#define CRCORE_H

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
#define CR_UINT8_MAX 0xFFU
/** @brief Largest value of a uint16_t. */
#define CR_UINT16_MAX 0xFFFFU
/** @brief Largest value of a uint32_t. */
#define CR_UINT32_MAX 0xFFFFFFFFU
/** @brief Largest value of a uint64_t. */
#define CR_UINT64_MAX 0xFFFFFFFFFFFFFFFFULL
/** @} */

/**
 * @defgroup cr_version Version
 * @brief Which carrottus a caller was built against, and which it runs on.
 *
 * One version for the whole library: carrottus is built and linked as one
 * archive, so its modules can't be mixed from different versions.
 *
 * What a version promises, from 1.0.0 on:
 *
 * | Change in | May add | May change or remove |
 * |-----------|---------|----------------------|
 * | patch     | nothing | nothing: fixes only, no header change but comments |
 * | minor     | functions, CR_RESULT values, CR_STRUCT_TYPE values, chained structs, enumerators | nothing |
 * | major     | anything | anything |
 *
 * "Released" means a commit tagged vMAJOR.MINOR.PATCH. Before 1.0.0, a
 * minor may change anything; nothing is promised.
 *
 * A struct that can be chained says in its comment which version first
 * takes it ("Since 1.2.0"), so that a caller can ask cr_get_version()
 * before chaining it. A caller that doesn't ask loses nothing but the
 * question: a carrottus that doesn't take a struct refuses the chain with
 * CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN.
 * @{
 */

/** @brief Packs a version into one number that compares with < and >:
 * 10 bits of major, 10 of minor, 12 of patch (Vulkan's layout, without its
 * variant bits). */
#define CARROTTUS_MAKE_VERSION(major, minor, patch) \
	((((uint32_t)(major)) << 22U) | (((uint32_t)(minor)) << 12U) | ((uint32_t)(patch)))
/** @brief The major part of a packed version. */
#define CARROTTUS_VERSION_GET_MAJOR(version) ((uint32_t)(version) >> 22U)
/** @brief The minor part of a packed version. */
#define CARROTTUS_VERSION_GET_MINOR(version) (((uint32_t)(version) >> 12U) & 0x3FFU)
/** @brief The patch part of a packed version. */
#define CARROTTUS_VERSION_GET_PATCH(version) ((uint32_t)(version) & 0xFFFU)

/* The one place the version is written. CMakeLists.txt reads these three
 * lines, so each stays "#define NAME <number>" and nothing else. */
/** @brief The major version of these headers. */
#define CARROTTUS_VERSION_MAJOR 0
/** @brief The minor version of these headers. */
#define CARROTTUS_VERSION_MINOR 2
/** @brief The patch version of these headers. */
#define CARROTTUS_VERSION_PATCH 0

/** @brief The version of the headers the caller is compiled against. */
#define CARROTTUS_VERSION \
	CARROTTUS_MAKE_VERSION(CARROTTUS_VERSION_MAJOR, CARROTTUS_VERSION_MINOR, CARROTTUS_VERSION_PATCH)

/**
 * @brief Returns the version of the carrottus the caller is running on: the
 * CARROTTUS_VERSION it was compiled with.
 *
 * It can differ from the caller's own CARROTTUS_VERSION only when carrottus was
 * built separately from the caller. Which one to accept is the caller's
 * decision; carrottus refuses no caller for its version.
 *
 * @return The packed version (CARROTTUS_MAKE_VERSION). Never 0.
 */
uint32_t cr_get_version(void);

/** @} */

/**
 * @defgroup cr_threads Threads
 * @brief Every Carrottus object is used by one caller at a time.
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
 *   it has; Carrottus takes no lock for it.
 * - Different objects are independent: calls on two objects may overlap,
 *   wherever each was made from. What Carrottus keeps for itself behind the
 *   handles (a shared table, an adapter's one device) it guards itself.
 * - A call that takes no object (Carrottus Perf's and Carrottus Memory's calls, and
 *   Carrottus Files' path calls) has nothing to order and may overlap any call.
 * - The exceptions are few and each is marked on the call itself, by a
 *   line that begins `Thread:` - a call that may overlap others on the
 *   same object (an abort, a status read), a function of the caller's
 *   that Carrottus or the platform calls on a thread of its own, and a call
 *   the platform confines to one thread. A module's `@par Threads`
 *   paragraph says only what is particular to that module.
 *
 * Checking it: a Debug build asserts when two calls overlap on one
 * Carrottus GPU queue (crgpu_command_queue_submit_lists()). Not yet for any other
 * object.
 */

/**
 * @brief The result of a Carrottus call, shared by every module.
 *
 * CR_SUCCESS is 0 and the only success value; every other enumerator names
 * one specific failure. Test a result with CR_FAILED() and print it with
 * cr_result_str().
 *
 * The values are grouped by module: 1-16 are general, 199-369 are Carrottus GPU
 * (`CR_RESULT_GPU_*`), 1001-1024 Carrottus Files (`CR_RESULT_CRFILES_*`), 2027-2028
 * Carrottus Memory (`CR_RESULT_MEM_*`), 3001-3033 Carrottus Net (`CR_RESULT_CRNET_*`),
 * 4001-4015 Carrottus Audio (`CR_RESULT_SAUD_*`) and 5001-5005 Carrottus Input
 * (`CR_RESULT_CRINPUT_*`). A value is never reused for
 * a different meaning.
 */
typedef enum CR_RESULT {
	/** The call succeeded. */
	CR_SUCCESS                         = 0,
	/** The call failed and no more specific result applies. */
	CR_RESULT_GENERAL_FAILURE           = 1,
	/** A call into the platform's own API failed. */
	CR_RESULT_API_SPECIFIC_FAILURE      = 2,
	/** The native API asked for is not one this build can use. */
	CR_RESULT_INVALID_API               = 3,
	/** A desc pointer is NULL. */
	CR_RESULT_NULL_DESC                 = 4,
	/** An output pointer is NULL. */
	CR_RESULT_NULL_OUTPUT_PARAMETER     = 5,
	/** A desc holds a value the call cannot accept. */
	CR_RESULT_DESC_INVALID_PARAMETERS   = 6,
	/** An index is past the end of what it indexes. */
	CR_RESULT_INDEX_OUT_OF_RANGE        = 7,
	/** A size or count is zero. */
	CR_RESULT_ZERO_SIZE                 = 8,
	/** An allocation failed. */
	CR_RESULT_OUT_OF_MEMORY             = 9,
	/** The call, or the option asked of it, is not implemented yet. */
	CR_RESULT_NOT_IMPLEMENTED_YET       = 10,
	/** An object handle is NULL. */
	CR_RESULT_NULL_OBJECT               = 11,
	/** A struct's `base.type` is not the type the call takes there (see
	 * CR_STRUCT_BASE). 0 is never a valid type. */
	CR_RESULT_STRUCT_WRONG_TYPE         = 12,
	/** A struct in a chain is of a type the call does not take in that
	 * chain, or one this build does not know. Never skipped. */
	CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN   = 13,
	/** One type appears twice in a chain. */
	CR_RESULT_STRUCT_DUPLICATE_IN_CHAIN = 14,
	/** A desc's allocator pointer is NULL (see CR_ALLOCATOR). */
	CR_RESULT_NULL_ALLOCATOR            = 15,
	/** An allocator's `allocate` or `free` is NULL. */
	CR_RESULT_INVALID_ALLOCATOR         = 16,

	/** crgpu_instance_create(): the graphics API could not list its GPUs.
	 * No GPU is not this: it is an instance with no adapters. */
	CR_RESULT_GPU_DEVICE_ENUMERATION_FAILURE                  = 199,
	/** An instance argument is NULL. */
	CR_RESULT_GPU_INVALID_INSTANCE                            = 200,
	/** A device argument is NULL. */
	CR_RESULT_GPU_INVALID_DEVICE                              = 201,
	/** A command queue argument is NULL. */
	CR_RESULT_GPU_INVALID_COMMAND_QUEUE                       = 202,
	/** A command allocator argument is NULL. */
	CR_RESULT_GPU_INVALID_COMMAND_ALLOCATOR                   = 203,
	/** A command list argument, or an array of them, is NULL. */
	CR_RESULT_GPU_INVALID_COMMAND_LIST                        = 204,
	/** A swap chain argument is NULL. */
	CR_RESULT_GPU_INVALID_SWAP_CHAIN                          = 205,
	/** A surface argument, or a desc's surface, is NULL. */
	CR_RESULT_GPU_INVALID_SURFACE                             = 206,
	/** A format is CRGPU_FORMAT_UNKNOWN, or one the call or the backend
	 * can't use there. */
	CR_RESULT_GPU_INVALID_FORMAT                              = 207,
	/** A surface's window handle is NULL. */
	CR_RESULT_GPU_INVALID_WINDOW_HANDLE                       = 208,
	/** A surface's display handle is NULL where the platform needs one. */
	CR_RESULT_GPU_INVALID_DISPLAY_HANDLE                      = 209,
	/** A command list type the call can't take there: a bundle submitted to
	 * a queue, or a type no queue of the device runs. */
	CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE                   = 210,
	/** An adapter argument is NULL. */
	CR_RESULT_GPU_INVALID_ADAPTER                             = 211,
	/** A CRGPU_FEATURE_* bit was asked for that the adapter doesn't
	 * support, or a call needs one the device wasn't created with and has
	 * no result of its own for it. */
	CR_RESULT_GPU_FEATURE_NOT_SUPPORTED                       = 212,
	/** A device desc asks for more queues of one of the adapter's queue
	 * pools than the pool holds. */
	CR_RESULT_GPU_TOO_MANY_QUEUES                             = 213,
	/** The adapter already has a device: one device per adapter at a time. */
	CR_RESULT_GPU_ADAPTER_IN_USE                              = 214,
	/* 215 is kept for CR_RESULT_GPU_DEVICE_LOST (CRGPU_DEVICE_TODO.md). */
	/** A descriptor pool, or the bindless table, doesn't fit in what is left
	 * of the device's descriptor budget. */
	CR_RESULT_GPU_OUT_OF_DESCRIPTORS                          = 216,
	/** A device desc's descriptor budget is above the adapter's limit. */
	CR_RESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE                 = 217,
	/** A swap chain desc's present_scaling is CRGPU_PRESENT_SCALING_NONE
	 * or not exactly one CRGPU_PRESENT_SCALING. */
	CR_RESULT_GPU_INVALID_PRESENT_SCALING                     = 218,
	/** The surface doesn't take the present scaling a swap chain desc names
	 * (crgpu_surface_get_present_scalings()). */
	CR_RESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED               = 219,
	/** A memory kind is CRGPU_MEMORY_KIND_NONE or not a kind, or not one
	 * the call takes (mapping a buffer that isn't mappable, say). */
	CR_RESULT_GPU_INVALID_MEMORY_KIND                         = 220,
	/** A buffer usage is CRGPU_BUFFER_USAGE_NONE, or one its memory kind
	 * does not allow. */
	CR_RESULT_GPU_INVALID_BUFFER_USAGE                        = 221,
	/** An image usage is CRGPU_IMAGE_USAGE_NONE, or a combination the call
	 * does not allow. */
	CR_RESULT_GPU_INVALID_IMAGE_USAGE                         = 222,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_BUFFER_STRIDE                       = 223,
	/** An index buffer view's stride is not 2 or 4 bytes. */
	CR_RESULT_GPU_INVALID_INDEX_STRIDE                        = 224,
	/** The device can't give the memory kind asked for: it wasn't created
	 * with the feature that kind needs. */
	CR_RESULT_GPU_MEMORY_KIND_NOT_SUPPORTED                   = 225,
	/** More color attachments than CRGPU_MAX_COLOR_ATTACHMENTS. */
	CR_RESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS                  = 226,
	/** A pipeline desc's depth bias clamp is not 0 on a device without
	 * CRGPU_FEATURE_DEPTH_BIAS_CLAMP. */
	CR_RESULT_GPU_DEPTH_BIAS_CLAMP_NOT_SUPPORTED              = 227,
	/** A pipeline desc's color attachments blend differently on a device
	 * without CRGPU_FEATURE_INDEPENDENT_BLEND. */
	CR_RESULT_GPU_INDEPENDENT_BLEND_NOT_SUPPORTED             = 228,
	/** A timestamp set on a device without CRGPU_FEATURE_PASS_TIMESTAMPS. */
	CR_RESULT_GPU_TIMESTAMPS_NOT_SUPPORTED                    = 229,
	/** A buffer argument is NULL. */
	CR_RESULT_GPU_INVALID_BUFFER                              = 230,
	/** A buffer view argument is NULL. */
	CR_RESULT_GPU_INVALID_BUFFER_VIEW                         = 231,
	/** An image argument is NULL. */
	CR_RESULT_GPU_INVALID_IMAGE                               = 232,
	/** An image view argument is NULL. */
	CR_RESULT_GPU_INVALID_IMAGE_VIEW                          = 233,
	/** An image view type the image can't be viewed as, such as a cube view
	 * of an image that isn't cube-capable, or a cube array view on a device
	 * without CRGPU_FEATURE_CUBE_ARRAY_VIEWS. */
	CR_RESULT_GPU_INVALID_IMAGE_VIEW_TYPE                     = 234,
	/** An image type the call can't take, such as a cube-capable image that
	 * isn't CRGPU_IMAGE_TYPE_2D. */
	CR_RESULT_GPU_INVALID_IMAGE_TYPE                          = 235,
	/** A buffer's, or a buffer view's, size is 0. */
	CR_RESULT_GPU_ZERO_BUFFER_SIZE                            = 236,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_BUFFER_SIZE                         = 237,
	/** A view's range is not within its parent buffer or image, or is not a
	 * range its view type can have (a cube view not of six layers, say). */
	CR_RESULT_GPU_BUFFER_OR_IMAGE_VIEW_RANGE_OUT_OF_SCOPE     = 238,
	/** A mapped, flushed or invalidated range runs past the end of the
	 * buffer. */
	CR_RESULT_GPU_MAP_OUT_OF_RANGE                            = 239,
	/** A heap argument is NULL. */
	CR_RESULT_GPU_INVALID_HEAP                                = 240,
	/** A heap desc's resource class is not a CRGPU_HEAP_RESOURCE_CLASS. */
	CR_RESULT_GPU_INVALID_HEAP_RESOURCE_CLASS                 = 241,
	/** A heap's size is not a multiple of CRGPU_HEAP_SIZE_GRANULARITY. */
	CR_RESULT_GPU_INVALID_HEAP_SIZE                           = 242,
	/** A resource is not of the heap's memory kind or resource class. */
	CR_RESULT_GPU_HEAP_MISMATCH                               = 243,
	/** A placement's offset is not a multiple of the resource's alignment. */
	CR_RESULT_GPU_PLACEMENT_MISALIGNED                        = 244,
	/** A placement runs past the end of its heap. */
	CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE                      = 245,
	/** A timestamp set argument is NULL. */
	CR_RESULT_GPU_INVALID_TIMESTAMP_SET                       = 246,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_SEMAPHORE                           = 250,
	/** A fence argument, or an array of them, is NULL. */
	CR_RESULT_GPU_INVALID_FENCE                               = 251,
	/** A fence wait's timeout ran out before the fences reached their
	 * values. */
	CR_RESULT_GPU_FENCE_WAIT_TIMED_OUT                        = 252,
	/** The device is lost: removed, reset after a hang, or its driver
	 * stopped. Nothing more it runs completes, and every object made from
	 * it is only for destroying; a caller that goes on makes a new device.
	 * Returned by crgpu_fence_wait() and the submissions, on Vulkan
	 * and D3D12. Never on Metal: a device on Apple Silicon isn't lost (a
	 * command buffer that faults fails alone, and the device goes on). */
	CR_RESULT_GPU_DEVICE_LOST                                 = 253,
	/** A shader stage the call can't take, or a pipeline desc that names
	 * both a vertex and a mesh module. */
	CR_RESULT_GPU_INVALID_SHADER_STAGE                        = 280,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_PRIMITIVE_TOPOLOGY                  = 281,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_CULL_MODE                           = 282,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_COMPARE_OP                          = 283,
	/** A shader module desc's entry point is NULL or empty. */
	CR_RESULT_GPU_NULL_ENTRY_POINT                            = 284,
	/** A shader module desc's workgroup size has a 0 for a compute, mesh or
	 * task stage, or anything but 0 for another stage. */
	CR_RESULT_GPU_INVALID_WORKGROUP_SIZE                      = 285,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_BLEND_FACTOR                        = 290,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_BLEND_OP                            = 291,
	/** A shader module is NULL, or has no shader the call can use. */
	CR_RESULT_GPU_INVALID_SHADER_MODULE                       = 300,
	/** A shader pipeline argument is NULL. */
	CR_RESULT_GPU_INVALID_GRAPHICS_PIPELINE                     = 301,
	/** A compute pipeline desc's compute module is NULL. */
	CR_RESULT_GPU_INVALID_COMPUTE_MODULE                      = 302,
	/** A compute pipeline argument is NULL. */
	CR_RESULT_GPU_INVALID_COMPUTE_PIPELINE                    = 303,
	/** A shader module desc's code is NULL or 0 bytes long. */
	CR_RESULT_GPU_NULL_SHADER_CODE                            = 304,
	/** A shader module desc's code is not 4-byte aligned, or a SPIR-V or
	 * DXIL size is not a multiple of 4. */
	CR_RESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT               = 305,
	/** A pipeline desc has no fragment module, or neither a vertex nor a
	 * mesh module. */
	CR_RESULT_GPU_VERTEX_AND_FRAGMENT_SHADER_REQUIRED         = 306,
	/** A shader module's code isn't the format its desc names, or the
	 * graphics API refused it or a pipeline made from it. */
	CR_RESULT_GPU_INVALID_SHADER_CODE                         = 307,
	/** A shader module desc's format is not one the device takes. */
	CR_RESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED                 = 308,
	/** A shader module has no entry point of the desc's name for its
	 * stage. */
	CR_RESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND                = 309,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_DESCRIPTOR_TYPE                     = 310,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_DESCRIPTOR_POOL_SIZE                = 311,
	/** A descriptor pool argument, or a desc's pool, is NULL. */
	CR_RESULT_GPU_INVALID_DESCRIPTOR_POOL                     = 312,
	/** A descriptor set layout is NULL, or one the call can't use. */
	CR_RESULT_GPU_INVALID_DESCRIPTOR_SET_LAYOUT               = 313,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_DESCRIPTOR_SET                      = 314,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_DESCRIPTOR_BUFFER_INFO              = 315,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_DESCRIPTOR_IMAGE_INFO               = 316,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_WRITE_DESCRIPTOR_SET                = 317,
	/** A set layout has more bindings than
	 * CRGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET. */
	CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_BINDINGS                = 318,
	/** A descriptor pool desc has more pool sizes than
	 * CRGPU_MAX_DESCRIPTOR_POOL_SIZES. */
	CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_POOLS                   = 319,
	/** A descriptor set allocation names no set layouts. */
	CR_RESULT_GPU_ZERO_DESCRIPTOR_SET_LAYOUTS                 = 320,
	/** A descriptor set allocation names more set layouts than
	 * CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS. */
	CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_SET_LAYOUTS             = 321,
	/** The pool has no room left for the sets asked for, or the graphics
	 * API's allocation of them failed. */
	CR_RESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL     = 322,
	/** A binding's descriptor type, or its place in the set, has no
	 * equivalent in the graphics API. */
	CR_RESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE = 323,
	/** The bindless table has no free slot left in that resource's class. */
	CR_RESULT_GPU_BINDLESS_OUT_OF_SLOTS                       = 324,

	/**
	 * @name Extension not supported by the device
	 *
	 * One CR_RESULT_GPU_EXT_<NAME>_NOT_SUPPORTED per CRGPU_EXT_<NAME>
	 * compile-time capability macro in crgpu.h that pairs with a
	 * CRGPU_FEATURE_<NAME> bit. Returned by that extension's entry points
	 * when the device wasn't created with the feature, which covers a GPU
	 * that lacks it, since such a GPU can't be asked for it. Never returned
	 * when the macro is 0: the extension's functions aren't declared at all
	 * in that case, so calling one is a compile/link error, not a runtime
	 * result.
	 * @{
	 */
	CR_RESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED          = 325, /**< CRGPU_EXT_BINDLESS: the device has no CRGPU_FEATURE_BINDLESS. */
	CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED      = 326, /**< CRGPU_EXT_MESH_SHADING: the device has no CRGPU_FEATURE_MESH_SHADING. */
	CR_RESULT_GPU_EXT_DEPTH_BOUNDS_TEST_NOT_SUPPORTED = 327, /**< CRGPU_EXT_DEPTH_BOUNDS_TEST. Reserved: no call returns it; the depth bounds test does nothing on a device without the feature. */
	CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED         = 328, /**< CRGPU_EXT_RAY_QUERY: the device has no CRGPU_FEATURE_RAY_QUERY. */
	CR_RESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED   = 329, /**< CRGPU_EXT_GEOMETRY_SHADER: the device has no CRGPU_FEATURE_GEOMETRY_SHADER. */
	CR_RESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED      = 330, /**< CRGPU_EXT_TESSELLATION: the device has no CRGPU_FEATURE_TESSELLATION. */
	/** @} */

	/** A module's bindings for a set it reads aren't its descriptor set
	 * layout's: it leaves out a binding of the layout, declares one the
	 * layout doesn't hold, or declares one of another type or count
	 * (crgpu_shader_module_desc, "Every binding of a set"). Returned only
	 * where the backend can see the module's bindings: Metal. */
	CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH                      = 331,

	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_RESOURCE_STATE                      = 350,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_IMAGE_LAYOUT                        = 351,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_PIPELINE_STAGE                      = 352,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_BUFFER_BARRIER                      = 353,
	/** Reserved: no call returns it. */
	CR_RESULT_GPU_INVALID_IMAGE_BARRIER                       = 354,

	/**
	 * @name Acceleration structures (CRGPU_EXT_RAY_QUERY)
	 * @{
	 */
	/** An acceleration structure is NULL (or, in a Debug build, destroyed),
	 * or not of the level the call takes there. */
	CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE              = 360,
	/** An acceleration structure is smaller than the build or copy writing
	 * it needs. */
	CR_RESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL            = 361,
	/** A build's scratch range is smaller than the build or update needs. */
	CR_RESULT_GPU_ACCELERATION_STRUCTURE_SCRATCH_TOO_SMALL    = 362,
	/** A range a build or a size write reads or writes runs past the end of
	 * its buffer. */
	CR_RESULT_GPU_ACCELERATION_STRUCTURE_RANGE                = 363,
	/** An offset, address or stride is not a multiple of what the call
	 * requires of it, or a stride is below its element's size. */
	CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT            = 364,
	/** A count is past the device's limit (CRGPU_RAY_QUERY_LIMITS) or the
	 * structure's own (crgpu_acceleration_structure_desc::max_bottom_levels). */
	CR_RESULT_GPU_ACCELERATION_STRUCTURE_LIMIT                = 365,
	/** An update its source can't take: the source was never built, was
	 * built without CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE, or with
	 * other flags, geometry kinds, counts or index layout than the update's. */
	CR_RESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED       = 366,
	/** A compaction, or a compacted size, its structure wasn't made for: not
	 * created with allow_compaction, or not last built with
	 * CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION. */
	CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED   = 367,
	/** A build, copy or compacted-size write on a command list of a queue
	 * type that can't record one: a copy list or a bundle. */
	CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE    = 368,
	/** A build, copy or compacted-size write between
	 * crgpu_command_list_begin_rendering() and crgpu_command_list_end_rendering(). */
	CR_RESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING     = 369,
	/** @} */

	CR_RESULT_FILES_NULL_PATH = 1001, /**< A path is NULL or empty. */
	CR_RESULT_FILES_INVALID_FILE = 1002, /**< A file argument is NULL. */
	CR_RESULT_FILES_INVALID_MAPPING = 1003, /**< Not returned since 2026-10-22, when crfiles_mapping_release() became void; not reused. */
	CR_RESULT_FILES_NOT_READABLE = 1004, /**< Mapping a file not opened for reading. */
	CR_RESULT_FILES_DIFFERENT_VOLUME = 1005, /**< Replace across volumes (EXDEV / ERROR_NOT_SAME_DEVICE). */
	CR_RESULT_FILES_IN_USE = 1006, /**< Replace target in use or read-only (Windows). */
	CR_RESULT_FILES_FLUSH_LEVEL_NOT_SUPPORTED = 1007, /**< The file system can't flush to the level asked for. */
	CR_RESULT_FILES_INVALID_INSTANCE = 1008, /**< A Carrottus Files instance is NULL. */
	CR_RESULT_FILES_NOT_FOUND = 1009, /**< Nothing is at the path, or a directory on the way to it isn't. */
	CR_RESULT_FILES_ALREADY_EXISTS = 1010, /**< Something is at the path already. */
	CR_RESULT_FILES_ACCESS_DENIED = 1011, /**< The platform's permissions refuse it. */
	CR_RESULT_FILES_NOT_A_FILE = 1012, /**< A directory, or another kind of entry, is where a file is wanted. */
	CR_RESULT_FILES_NOT_WRITABLE = 1013, /**< Writing to a file not opened for writing. */
	CR_RESULT_FILES_END_OF_LISTING = 1014, /**< A listing has given every entry. An answer, not a failure. */
	CR_RESULT_FILES_NOT_A_DIRECTORY = 1015, /**< A file, or another kind of entry, is where a directory is wanted. */
	CR_RESULT_FILES_LOCKED = 1016, /**< Another lock holds the file, and the call was not to wait. */
	CR_RESULT_FILES_INVALID_LISTING = 1017, /**< A listing argument is NULL. */
	CR_RESULT_FILES_ALREADY_LOCKED = 1018, /**< The file holds a lock taken through it already. */
	CR_RESULT_FILES_NOT_LOCKED = 1019, /**< The file holds no lock taken through it. */
	CR_RESULT_FILES_NOT_EMPTY = 1020, /**< A directory to remove holds entries. */
	CR_RESULT_FILES_INVALID_LIBRARY = 1021, /**< A library argument is NULL. */
	CR_RESULT_FILES_LIBRARY_NOT_LOADED = 1022, /**< The platform's loader didn't load the library; the instance's message callback has its words. */
	CR_RESULT_FILES_SYMBOL_NOT_FOUND = 1023, /**< The library exports no function of that name. */
	CR_RESULT_FILES_LIBRARY_OPTION_NOT_SUPPORTED = 1024, /**< The platform's loader has no such choice. */

	/* 2001-2003 and 2026 (ZERO_CAPACITY, INSUFFICIENT_CAPACITY,
	 * PLAT_COMMIT_FAIL, INVALID_ARENA) went with Carrottus Memory's arena and are
	 * not reused. */
	CR_RESULT_MEM_NULL_ADDRESS = 2027, /**< An address passed in is NULL. */
	CR_RESULT_MEM_NOT_PAGE_ALIGNED = 2028, /**< A size or address is not a multiple of the page size. */

	CR_RESULT_NET_STARTUP_FAILED        = 3001, /**< crnet_instance_create(): the platform's networking wouldn't start. */
	CR_RESULT_NET_INVALID_TCP_SOCKET    = 3002, /**< A TCP socket is NULL, not open, or already connected. */
	CR_RESULT_NET_RESOLVE_FAILED        = 3003, /**< A host name could not be resolved to an address. */
	CR_RESULT_NET_CONNECT_FAILED        = 3004, /**< The connection to the other end could not be made. */
	CR_RESULT_NET_LISTEN_FAILED         = 3005, /**< A listener could not be bound or set listening. */
	CR_RESULT_NET_ACCEPT_FAILED         = 3006, /**< A connection could not be accepted. */
	CR_RESULT_NET_SEND_FAILED           = 3007, /**< Data could not be sent. */
	CR_RESULT_NET_RECV_FAILED           = 3008, /**< Data could not be received. */
	/* 3009 and 3010 (WOULD_BLOCK, SET_BLOCKING_FAILED) went with Carrottus Net's
	 * non-blocking mode and are not reused. */
	CR_RESULT_NET_INVALID_URL           = 3011, /**< Not a URL, or a scheme the call doesn't carry. */
	CR_RESULT_NET_INVALID_HTTP_CLIENT   = 3012, /**< An HTTP client argument is NULL. */
	CR_RESULT_NET_INVALID_WEBSOCKET     = 3013, /**< A WebSocket is NULL, not open, or already connected. */
	CR_RESULT_NET_TLS_FAILED            = 3014, /**< Handshake or certificate check failed. */
	CR_RESULT_NET_TIMED_OUT             = 3015, /**< The call's own time limit ran out, and nothing else. */
	CR_RESULT_NET_HTTP_FAILED           = 3016, /**< An HTTP transfer's start or its wait for the response failed. */
	CR_RESULT_NET_WS_UPGRADE_REFUSED    = 3017, /**< The server answered, but not with 101. */
	CR_RESULT_NET_WS_CLOSED             = 3018, /**< Closed by a close frame, either side's. */
	CR_RESULT_NET_WS_MESSAGE_TOO_BIG    = 3019, /**< Over the connect desc's max_message_size. */
	CR_RESULT_NET_UNSUPPORTED           = 3020, /**< This build's platform stack doesn't have it. */
	CR_RESULT_NET_ABORTED               = 3022, /**< The object's _abort was called. */
	CR_RESULT_NET_INVALID_TCP_LISTENER  = 3024, /**< A TCP listener argument is NULL. */
	CR_RESULT_NET_INVALID_ADDRESS       = 3025, /**< Not a numeric IPv4 or IPv6 address. */
	CR_RESULT_NET_ADDRESS_IN_USE        = 3026, /**< Another listener already has that address and port. */
	CR_RESULT_NET_INVALID_WAIT_SET      = 3027, /**< A wait set argument is NULL. */
	CR_RESULT_NET_ALREADY_IN_WAIT_SET   = 3028, /**< The object is in this set already, or a WebSocket in another. */
	CR_RESULT_NET_INVALID_RESOLVER      = 3029, /**< NULL, or one whose lookup has already been run. */
	CR_RESULT_NET_SYSTEM_TIMED_OUT      = 3030, /**< TCP: the operating system gave up waiting for the other end. */
	CR_RESULT_NET_STACK_TIMED_OUT       = 3031, /**< HTTP, WebSocket: the client stack gave up waiting, or says something under it did. */
	CR_RESULT_NET_INVALID_HTTP_TRANSFER = 3032, /**< NULL, or a call out of the transfer's order of use. */
	CR_RESULT_NET_INVALID_INSTANCE      = 3033, /**< NULL, or not the instance the other object was made with. */

	CR_RESULT_AUDIO_INVALID_INSTANCE      = 4001, /**< A Carrottus Audio instance argument is NULL. */
	CR_RESULT_AUDIO_INVALID_DEVICE        = 4002, /**< An audio device argument, or a desc's device, is NULL. */
	CR_RESULT_AUDIO_INVALID_STREAM        = 4003, /**< An audio stream argument is NULL. */
	CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED  = 4004, /**< The endpoint can't take the format as written. */
	CR_RESULT_AUDIO_EXCLUSIVE_UNAVAILABLE = 4005, /**< Exclusive mode is held by another client, or the backend has none. */
	CR_RESULT_AUDIO_DEVICE_LOST           = 4006, /**< The device disappeared since enumeration. */
	CR_RESULT_AUDIO_PERIOD_OUT_OF_RANGE   = 4007, /**< A period of 0, or one the OS doesn't grant. */
	CR_RESULT_AUDIO_BUFFER_OUT_OF_RANGE   = 4008, /**< A buffer size below the period, set where the backend has no separate buffer, or one the OS doesn't grant. */
	CR_RESULT_AUDIO_THREAD_PRIORITY_UNSUPPORTED = 4009, /**< The backend doesn't have the scheduling mechanism asked for. */
	CR_RESULT_AUDIO_THREAD_PRIORITY_DENIED      = 4010, /**< The OS refused the thread priority asked for when the stream's thread started. */
	CR_RESULT_AUDIO_SAMPLE_RATE_NOT_SETTABLE    = 4011, /**< The platform has no device-wide sample rate to set. */
	CR_RESULT_AUDIO_OS_CONVERSION_UNAVAILABLE   = 4012, /**< ALLOW_OS_CONVERSION was set where the platform can't convert for the request. */
	CR_RESULT_AUDIO_ACCESS_DENIED               = 4013, /**< The platform hasn't granted this process use of the endpoint (iOS: record permission not granted). */
	CR_RESULT_AUDIO_INTERRUPTED                 = 4014, /**< The platform's audio session took the endpoint from this process, or won't give it now: an interruption (a call, an alarm) or another app's audio that doesn't mix (iOS). */
	CR_RESULT_AUDIO_SESSION_INACTIVE            = 4015, /**< The platform's audio session, which the caller owns, isn't active, and the platform won't run a stream on it (iOS). */

	CR_RESULT_INPUT_INVALID_INSTANCE      = 5001, /**< A Carrottus Input instance argument is NULL. */
	CR_RESULT_INPUT_GAMEPAD_NOT_CONNECTED = 5002, /**< The id names no gamepad as of the last poll. */
	CR_RESULT_INPUT_UNSUPPORTED           = 5003, /**< The native API doesn't offer the choice asked of it. */
	CR_RESULT_INPUT_FEATURE_NOT_PRESENT   = 5004, /**< The device doesn't have what the call is about. */
	CR_RESULT_INPUT_ACCESS_DENIED         = 5005, /**< The platform won't let this process read or write the device. */

} CR_RESULT;

/**
 * @brief Tests whether a CR_RESULT is a failure.
 *
 * @param[in] sr The CR_RESULT to test. Evaluated once.
 *
 * @return Nonzero if `sr` is anything other than CR_SUCCESS, 0 if it is
 *         CR_SUCCESS.
 */
#define CR_FAILED(sr) ((sr) != CR_SUCCESS)

/**
 * @defgroup cr_struct_base Struct bases
 * @brief How a struct that may gain fields is extended without changing
 * its layout.
 *
 * An extensible struct's first member is a CR_STRUCT_BASE (one the
 * caller fills in) or a CR_STRUCT_BASE_OUT (one Carrottus fills in), named
 * `base`. `base.type` says which struct it is; `base.next` points to the
 * next struct in its chain, or is NULL. A struct's layout never changes
 * once it is released: a new field comes as a new struct with a base of
 * its own, which the caller chains on. A struct with no base is frozen and
 * never changes at all; a new need gets a new type.
 *
 * - The caller sets every `base.type` in a chain, the root's included.
 *   A type that isn't the one the call takes is refused with
 *   CR_RESULT_STRUCT_WRONG_TYPE.
 * - A chained struct the call doesn't take there, or this build doesn't
 *   know, is refused with CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN, never
 *   skipped: skipping it would decide for the caller that it didn't
 *   matter. One type twice in a chain is refused with
 *   CR_RESULT_STRUCT_DUPLICATE_IN_CHAIN.
 * - Leaving a chained struct out means that addition isn't used. Each
 *   chained struct's fields have no defaults, as no desc's do.
 *
 * ```c
 * crgpu_image_desc desc = {
 * 	.base = { CR_STRUCT_TYPE_GPU_IMAGE_DESC, NULL },
 * 	...
 * };
 * ```
 *
 * The member is nested, not two loose fields, so that a pointer to any based
 * struct converts to a pointer to its base and back (C11 6.7.2.1p15): a
 * chain is walked through CR_STRUCT_BASE pointers with defined behaviour.
 * @{
 */

/**
 * @brief Which struct a based struct is.
 *
 * Numbered in blocks per module, as CR_RESULT is: 1-999 Carrottus Core, 1000+ Carrottus GPU, 2000+
 * Carrottus Audio, 3000+ Carrottus Net, 4000+ Carrottus Input, 5000+ Carrottus Files. A value is
 * never renumbered or reused, a retired struct's included.
 */
typedef enum CR_STRUCT_TYPE {
	/** Never valid: a struct left zeroed is refused, not misread. */
	CR_STRUCT_TYPE_UNKNOWN  = 0,

	/* Carrottus Core: 1-999. */
	/** CR_MESSAGE (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_MESSAGE                        = 1,

	/* Carrottus GPU. Retired, never reused: 1000 (CRGPU_DEVICE_PROPERTIES),
	 * 1017 (crgpu_bindless_capabilities), 1020
	 * (crgpu_mesh_shading_capabilities), 1021
	 * (crgpu_depth_bounds_capabilities). */
	/** crgpu_submit_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_SUBMIT_DESC                = 1001,
	/** crgpu_command_allocator_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC     = 1002,
	/** crgpu_buffer_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_BUFFER_DESC                = 1003,
	/** crgpu_buffer_view_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC           = 1004,
	/** crgpu_image_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_IMAGE_DESC                 = 1005,
	/** crgpu_image_view_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC            = 1006,
	/** crgpu_heap_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_HEAP_DESC                  = 1007,
	/** crgpu_placement_requirements (CR_STRUCT_BASE_OUT). */
	CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS     = 1008,
	/** crgpu_buffer_barrier (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_BUFFER_BARRIER             = 1009,
	/** crgpu_image_barrier (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_IMAGE_BARRIER              = 1010,
	/** crgpu_swap_chain_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC            = 1011,
	/** crgpu_descriptor_set_layout_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC = 1012,
	/** crgpu_descriptor_pool_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC       = 1013,
	/** crgpu_descriptor_set_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC        = 1014,
	/** crgpu_sampler_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_SAMPLER_DESC               = 1015,
	/** crgpu_write_descriptor_set (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET       = 1016,
	/** crgpu_shader_module_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_SHADER_MODULE_DESC         = 1018,
	/** crgpu_graphics_pipeline_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC       = 1019,
	/** crgpu_compute_pipeline_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC      = 1022,
	/** crgpu_rendering_begin_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC       = 1023,
	/** crgpu_bundle_inheritance_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_BUNDLE_INHERITANCE_DESC    = 1024,
	/** crgpu_instance_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_INSTANCE_DESC              = 1025,
	/** CRGPU_ADAPTER_PROPERTIES (CR_STRUCT_BASE_OUT). */
	CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES         = 1026,
	/** crgpu_device_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_DEVICE_DESC                = 1027,
	/** CRGPU_MESH_SHADING_LIMITS (CR_STRUCT_BASE_OUT), chained onto
	 * CRGPU_ADAPTER_PROPERTIES. */
	CR_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS        = 1028,
	/** CRGPU_BINDLESS_LIMITS (CR_STRUCT_BASE_OUT), chained onto
	 * CRGPU_ADAPTER_PROPERTIES. */
	CR_STRUCT_TYPE_GPU_BINDLESS_LIMITS            = 1029,
	/** CRGPU_RAY_QUERY_LIMITS (CR_STRUCT_BASE_OUT), chained onto
	 * CRGPU_ADAPTER_PROPERTIES. */
	CR_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS           = 1030,
	/** crgpu_acceleration_structure_inputs (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS  = 1031,
	/** crgpu_acceleration_structure_sizes (CR_STRUCT_BASE_OUT). */
	CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES   = 1032,
	/** crgpu_acceleration_structure_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC    = 1033,
	/** crgpu_acceleration_structure_build (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD   = 1034,
	/** crgpu_acceleration_structure_barrier (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER = 1035,

	/* Carrottus Audio */
	/** CRAUDIO_DEVICE_PROPERTIES (CR_STRUCT_BASE_OUT). */
	CR_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES        = 2000,
	/** CRAUDIO_TIMING_CAPS (CR_STRUCT_BASE_OUT). */
	CR_STRUCT_TYPE_AUDIO_TIMING_CAPS              = 2001,
	/** CRAUDIO_STREAM_DESC (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_AUDIO_STREAM_DESC              = 2002,
	/** CRAUDIO_STREAM_CONFIG (CR_STRUCT_BASE_OUT). */
	CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG            = 2003,
	/** CRAUDIO_INSTANCE_DESC (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_AUDIO_INSTANCE_DESC            = 2004,

	/* Carrottus Net */
	/** crnet_resolver_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_NET_RESOLVER_DESC              = 3000,
	/** crnet_tcp_listener_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC          = 3001,
	/** crnet_tcp_socket_connect_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC    = 3002,
	/** crnet_http_client_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC           = 3003,
	/** crnet_http_transfer_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC         = 3004,
	/** crnet_websocket_connect_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC     = 3005,
	/** crnet_instance_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_NET_INSTANCE_DESC              = 3006,

	/* Carrottus Input */
	/** CRINPUT_INSTANCE_DESC (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_INPUT_INSTANCE_DESC            = 4000,
	/** CRINPUT_GAMEPAD_PROPERTIES (CR_STRUCT_BASE_OUT). */
	CR_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES       = 4001,

	/* Carrottus Files */
	/** crfiles_open_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_FILES_OPEN_DESC                = 5000,
	/** crfiles_instance_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_FILES_INSTANCE_DESC            = 5001,
	/** CRFILES_PATH_INFO (CR_STRUCT_BASE_OUT). */
	CR_STRUCT_TYPE_FILES_PATH_INFO                = 5002,
	/** crfiles_listing_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_FILES_LISTING_DESC             = 5003,
	/** CRFILES_LISTING_ENTRY (CR_STRUCT_BASE_OUT). */
	CR_STRUCT_TYPE_FILES_LISTING_ENTRY            = 5004,
	/** crfiles_library_desc (CR_STRUCT_BASE). */
	CR_STRUCT_TYPE_FILES_LIBRARY_DESC             = 5005,

	/** Keeps the enum 32 bits wide on every compiler. Never a valid type. */
	CR_STRUCT_TYPE_MAX_ENUM = 0x7FFFFFFF
} CR_STRUCT_TYPE;

/** @brief The first member, `base`, of every extensible struct the caller
 * fills in. */
/* Frozen: the base every based struct starts with. It never changes; a new need gets a new type. */
typedef struct CR_STRUCT_BASE {
	/** This struct's own type. */
	CR_STRUCT_TYPE type;
	/** The next struct in the chain, or NULL. Carrottus only reads it. */
	const struct CR_STRUCT_BASE *next;
} CR_STRUCT_BASE;

/** @brief The first member, `base`, of every extensible struct Carrottus
 * fills in for the caller. The caller builds the chain and sets each
 * `type`; Carrottus writes the other fields and leaves `type` and `next` as
 * they are. */
/* Frozen: the base every based struct starts with. It never changes; a new need gets a new type. */
typedef struct CR_STRUCT_BASE_OUT {
	/** This struct's own type. */
	CR_STRUCT_TYPE type;
	/** The next struct in the chain, or NULL. */
	struct CR_STRUCT_BASE_OUT *next;
} CR_STRUCT_BASE_OUT;

/** @} */

/**
 * @defgroup cr_messages Messages
 * @brief What the platform said when it refused a call, delivered to the
 * caller's callback.
 *
 * A CR_RESULT is the same on every platform; the platform's own number
 * behind a failure (a GetLastError value, an errno, a VkResult, an
 * HRESULT, an OSStatus, a loader's words) is not. It reaches the caller as
 * a message, through a callback the caller gives the module's instance.
 * The rules, for every module whose instance desc takes one (Carrottus Files'
 * crfiles_instance_desc so far):
 *
 * - M1. A module's instance desc takes one message callback and its user
 *   data. NULL means no messages. Every object made from the instance
 *   reports to it.
 * - M2. Every failure a platform call reports is a message
 *   (CR_MESSAGE_ORIGIN_PLATFORM, severity ERROR), whatever CR_RESULT the
 *   call returns for it: CR_RESULT_API_SPECIFIC_FAILURE, an out-of-memory
 *   result, and equally a code of its own (CR_RESULT_FILES_NOT_FOUND,
 *   from ENOENT or ENOTDIR, which the message tells apart). A `void` call
 *   sends the failure the platform reported to it. None is judged too
 *   routine to send: the caller filters on `result` in its handler.
 *   Nothing is sent for an answer the platform gives as an outcome
 *   (would-block, the end of a listing, a wait's own timeout, a peer's
 *   orderly close), for a time limit Carrottus keeps itself, or for a refused
 *   argument or another check of Carrottus's own. At every validation level,
 *   off included.
 * - M3. The callback is called inside the call that failed, on the thread
 *   that made it, before the call returns. Calls on different objects may
 *   run at the same time (@ref cr_threads "Threads"), so the callback may
 *   be called from several of the caller's threads at once, and must allow
 *   it. Carrottus never calls it from a thread of its own or a platform's.
 *
 * Thread: a message callback is called on the thread of the call that
 * failed, so on several of the caller's threads at once where the caller
 * makes calls on different objects at once (M3).
 *
 * - M4. The callback makes no call into the module whose call is reporting
 *   (a Carrottus Files message's handler makes no Carrottus Files call); another
 *   module is fine. It returns normally: no C++ exception, Swift or C#
 *   error, or longjmp unwinds through Carrottus. Carrottus holds no lock of its
 *   own while calling it.
 * - M5. Everything the message points to is valid only during the
 *   callback: the caller copies what it keeps. Sending a message allocates
 *   nothing.
 * - M6. A call with no instance to report to takes a
 *   CR_PLATFORM_ERROR *out_error instead, which may be NULL: the number
 *   alone.
 * - M7. Messages go to whoever created the instance. A caller that is only
 *   handed an object made from it doesn't see them: the failing call's
 *   CR_RESULT is what it has.
 * @{
 */

/** @brief Which of the platform's numberings a code is in. The number means
 * nothing without it. */
typedef enum CR_PLATFORM_ERROR_SOURCE {
	/** No platform code: the message isn't a platform failure, or nothing
	 * was recorded. */
	CR_PLATFORM_ERROR_SOURCE_NONE = 0,
	/** GetLastError and WSAGetLastError values, ERROR_WINHTTP_* among them,
	 * and HRESULTs (D3D12, DXGI, WASAPI, GameInput, CryptoAPI's CERT_E_* and
	 * TRUST_E_*). FormatMessage reads all of them. */
	CR_PLATFORM_ERROR_SOURCE_WIN32 = 1,
	/** errno, from POSIX calls, ALSA's negative returns (negated) and evdev,
	 * or reported through NSPOSIXErrorDomain. */
	CR_PLATFORM_ERROR_SOURCE_ERRNO = 2,
	/** getaddrinfo's EAI_* (Linux and Apple; Winsock's resolver reports
	 * WSA* codes, which are CR_PLATFORM_ERROR_SOURCE_WIN32). */
	CR_PLATFORM_ERROR_SOURCE_RESOLVER = 3,
	/** A libcurl CURLcode. */
	CR_PLATFORM_ERROR_SOURCE_CURL = 4,
	/** NSURLErrorDomain. */
	CR_PLATFORM_ERROR_SOURCE_NSURL = 5,
	/** An OSStatus: Core Audio, Security, NSOSStatusErrorDomain. */
	CR_PLATFORM_ERROR_SOURCE_OSSTATUS = 6,
	/** The network stack had no complaint; the certificate matched none of
	 * the pins it was given. The code is 0. */
	CR_PLATFORM_ERROR_SOURCE_TLS_PIN = 7,
	/** An NSError from a domain not listed here; the text starts with the
	 * domain's name. */
	CR_PLATFORM_ERROR_SOURCE_OTHER = 8,
	/** A VkResult. */
	CR_PLATFORM_ERROR_SOURCE_VKRESULT = 9,
	/** A dynamic loader's message with no number (dlerror on Linux and
	 * Apple): the code is 0 and the text is the loader's. */
	CR_PLATFORM_ERROR_SOURCE_DLERROR = 10,
} CR_PLATFORM_ERROR_SOURCE;

/** @brief Who a message comes from. */
typedef enum CR_MESSAGE_ORIGIN {
	/** The platform refused a call: `platform_source` and `platform_code`
	 * say how. */
	CR_MESSAGE_ORIGIN_PLATFORM = 1,
	/** The module's own check layer. */
	CR_MESSAGE_ORIGIN_CHECK = 2,
	/** The platform's validation layer: D3D12's debug layer, Vulkan's
	 * validation layers, Metal's API validation. */
	CR_MESSAGE_ORIGIN_NATIVE_VALIDATION = 3,
	/** GPU-based validation. */
	CR_MESSAGE_ORIGIN_GPU_VALIDATION = 4,
} CR_MESSAGE_ORIGIN;

/** @brief How bad a message is: every level a platform's own layers report,
 * so none is merged into another. */
typedef enum CR_MESSAGE_SEVERITY {
	/** Vulkan's VERBOSE, D3D12's MESSAGE. */
	CR_MESSAGE_SEVERITY_VERBOSE = 1,
	/** Information. */
	CR_MESSAGE_SEVERITY_INFO = 2,
	/** A warning. */
	CR_MESSAGE_SEVERITY_WARNING = 3,
	/** An error; every platform failure. */
	CR_MESSAGE_SEVERITY_ERROR = 4,
	/** D3D12's CORRUPTION: the layer judges the device's state corrupt. */
	CR_MESSAGE_SEVERITY_CORRUPTION = 5,
} CR_MESSAGE_SEVERITY;

/** @brief One message, handed to the callback; read only during it (M5).
 * Set by Carrottus: a newer Carrottus may chain more onto `base.next`, which a
 * caller that doesn't know the type skips. */
typedef struct CR_MESSAGE {
	/** `type` is CR_STRUCT_TYPE_MESSAGE; `next` is NULL, or a struct a
	 * later version chains. Set by Carrottus. See CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** Who the message comes from. */
	CR_MESSAGE_ORIGIN origin;
	/** How bad it is; ERROR for every platform failure. */
	CR_MESSAGE_SEVERITY severity;
	/** For a platform failure, which numbering `platform_code` is in;
	 * CR_PLATFORM_ERROR_SOURCE_NONE otherwise. */
	CR_PLATFORM_ERROR_SOURCE platform_source;
	/** For a platform failure, what the call returns for it (M2), or for a
	 * `void` call what it would have returned; CR_SUCCESS for the other
	 * origins. */
	CR_RESULT result;
	/** The check's or layer's message id; 0 for a platform failure. */
	uint32_t id;
	/** For a failure sent from an object's pending slot, how many more the
	 * slot overwrote since the last one sent; 0 otherwise. */
	uint32_t dropped;
	/** The platform's number, as it gave it; 0 otherwise. */
	int64_t platform_code;
	/** The call that was running, a literal ("crfiles_file_open"). */
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
} CR_MESSAGE;

/** @brief A caller's handler for messages (M1-M7).
 * @param[in] message   The message, valid only during the call.
 * @param[in] user_data The instance desc's `message_user_data`. */
typedef void (*CR_MESSAGE_CALLBACK)(const CR_MESSAGE *message, void *user_data);

/** @brief The platform's error behind a failure, for a call with no
 * instance to send a message to (M6). */
/* Frozen: filled in per call, like every error struct. It never changes; a new need gets a new type. */
typedef struct CR_PLATFORM_ERROR {
	/** What the call returned; CR_SUCCESS if it didn't fail on the
	 * platform. */
	CR_RESULT result;
	/** Which numbering `code` is in; NONE when nothing is recorded. */
	CR_PLATFORM_ERROR_SOURCE source;
	/** The platform's number, as it gave it. */
	int64_t code;
} CR_PLATFORM_ERROR;

/** @} */

/**
 * @defgroup cr_allocator The caller's allocator
 * @brief Where Carrottus's own memory comes from.
 *
 * Carrottus never chooses where its memory comes from. Each module's
 * instance is made with a CR_ALLOCATOR, and Carrottus takes every block
 * it needs for that instance, and for every object made from it, from that
 * allocator alone: the handles, the copies it keeps (debug names, descs),
 * and its own lists and tables. An object never has an allocator of its
 * own, and a destroy call never takes one: an object frees through the
 * allocator of the instance it was made from.
 *
 * Memory that the platform, the operating system or a graphics driver
 * allocates for itself is not covered, because those offer Carrottus no way
 * to pass an allocator on.
 *
 * There is no default. A create call given no allocator refuses it with
 * CR_RESULT_NULL_ALLOCATOR, and one missing either function with
 * CR_RESULT_INVALID_ALLOCATOR. cr_allocator_system() is the system heap,
 * for a caller that chooses it.
 *
 * The rules an allocator keeps:
 *
 * - `allocate` returns a block of at least `size` bytes whose address is a
 *   multiple of `alignment`, or NULL if it can't. Carrottus then fails the
 *   call it was making with CR_RESULT_OUT_OF_MEMORY and leaves everything
 *   as it was before the call. The block's contents are undefined.
 * - Carrottus never asks for 0 bytes. `alignment` is always a power of two,
 *   1 or more. An allocator that can't meet an alignment returns NULL.
 * - `free` is called once for each block, with the same `size` and
 *   `alignment` the block was allocated with, so an allocator need not
 *   record either. It is never called with NULL, and it can't fail.
 * - Thread: either function may be called on any thread, and by calls
 *   that overlap: different objects of one instance are independent
 *   (@ref cr_threads "Threads"), so their calls may allocate at once,
 *   and a few calls allocate on a thread that is not the caller's, each
 *   marked so in its module's header (Carrottus Audio's device-event watch,
 *   Carrottus Input's GameInput device callback). An allocator that can't take
 *   overlapping calls is the caller's to keep to itself, by using that
 *   instance and everything made from it one call at a time and leaving
 *   those calls out.
 * - Neither function may call Carrottus.
 * - Carrottus copies the CR_ALLOCATOR when the instance is made, so the
 *   caller's struct need not outlive the call. The functions and
 *   `user_data` must stay valid until the instance and every object made
 *   from it are destroyed, and by then every block Carrottus took from them
 *   has been freed. Where a module lets an object's memory outlast its
 *   destroy call, its header says until when (a Carrottus Net resolver whose
 *   lookup is still running).
 * @{
 */

/**
 * @brief Allocates a block for Carrottus.
 *
 * @param[in] user_data The allocator's `user_data`, passed as it was given.
 * @param[in] size      The bytes wanted. Never 0.
 * @param[in] alignment What the address must be a multiple of. A power of
 *                      two, 1 or more.
 *
 * @return The block, or NULL if it can't be given.
 */
typedef void *(*CR_ALLOCATE_FUNCTION)(
    void *user_data,
    size_t size,
    size_t alignment);

/**
 * @brief Frees a block CR_ALLOCATE_FUNCTION gave.
 *
 * @param[in] user_data The allocator's `user_data`, passed as it was given.
 * @param[in] memory    The block. Never NULL.
 * @param[in] size      The `size` it was allocated with.
 * @param[in] alignment The `alignment` it was allocated with.
 */
typedef void (*CR_FREE_FUNCTION)(
    void *user_data,
    void *memory,
    size_t size,
    size_t alignment);

/** @brief The caller's allocator: two functions and the state they share. */
/* Frozen: a table of functions. A new function or hint is a new struct. */
typedef struct CR_ALLOCATOR {
	/** Passed to both functions as it is. Carrottus never reads it. May be NULL. */
	void *user_data;
	/** Gives a block. Not NULL. */
	CR_ALLOCATE_FUNCTION allocate;
	/** Frees a block `allocate` gave. Not NULL. */
	CR_FREE_FUNCTION free;
} CR_ALLOCATOR;

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
CR_ALLOCATOR cr_allocator_system(void);

/** @} */

/**
 * @brief Returns the name of a CR_RESULT enumerator as a string.
 *
 * For logs and error messages. The string is the enumerator's identifier
 * exactly as it is spelled in this header, such as "CR_RESULT_NULL_DESC".
 *
 * @param[in] r The result to name.
 *
 * @return A null-terminated string literal: valid for the life of the
 *         program, and not to be freed or modified. Never NULL.
 * @retval "CR_RESULT_UNKNOWN" `r` is not one of CR_RESULT's values. Every
 *         value has its own name.
 */
const char *cr_result_str(CR_RESULT r);

/**
 * @brief Gives a Carrottus object a debug name, replacing any it already has.
 *
 * The string is copied, through the allocator of the instance the object
 * was made from (CR_ALLOCATOR), so it need not outlive the call. The
 * object's previous name is freed, and the object's own destroy call frees the name it
 * holds at that point. Exists in every build, and the name is kept in every
 * build: a debug name never changes a struct's layout or a library's symbols.
 *
 * @param[in,out] object A handle to an object that carries a debug name.
 *                       Every Carrottus GPU, Carrottus Audio and Carrottus Net handle does, and
 *                       so does an crfiles_file. The name it holds is replaced.
 * @param[in]     name   The new name, a null-terminated string, or NULL to
 *                       remove the current one. May be the object's own
 *                       current name.
 *
 * @retval CR_SUCCESS The name was replaced.
 * @retval CR_RESULT_NULL_OBJECT `object` is NULL.
 * @retval CR_RESULT_OUT_OF_MEMORY The copy could not be allocated. The
 *         object keeps the name it had.
 */
CR_RESULT cr_debug_name_set(void *object, const char *name);

/**
 * @brief Calls cr_debug_name_set(), casting the handle to `void *`.

 *
 * @param[in] obj  A handle of any type that carries a debug name.
 * @param[in] name The new name, or NULL to remove the current one.
 *
 * @return What cr_debug_name_set() returns.
 */
#define CR_SET_DEBUG_NAME(obj, name)                                         \
	cr_debug_name_set((void *)(obj), (name))

/**
 * @brief Returns an object's debug name.

 *
 * @param[in] object A handle to an object that carries a debug name (see
 *                   cr_debug_name_set()), or NULL.
 *
 * @return The object's name, a null-terminated string. It belongs to the
 *         object: it stays valid until the name is next set or the object is
 *         destroyed, and is not to be freed or modified.
 * @retval NULL `object` is NULL, or the object has no name.
 */
const char *cr_debug_name_get(void *object);

/**
 * @brief Calls cr_debug_name_get(), casting the handle to `void *`.

 *
 * @param[in] obj A handle of any type that carries a debug name, or NULL.
 *
 * @return What cr_debug_name_get() returns.
 */
#define CR_GET_DEBUG_NAME(obj) cr_debug_name_get((void *)(obj))

#if __cplusplus
}
#endif

#endif // CRCORE_H
