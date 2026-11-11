#ifndef CRGPUSHARED_H
#define CRGPUSHARED_H

/* Internal header - never included outside carrottus/src/gpu/. What every
 * backend shares: format facts the public header doesn't expose, and the
 * checks of a call that are the same on every backend, written once so that
 * all three return the same result for the same condition. */

#include "crgpu.h"
#include "../crallocator.h"
#include "../crthread.h"

#if __cplusplus
extern "C" {
#endif

/*
 * The checks every backend's create makes of its desc, in this order, before
 * anything else: CR_RESULT_NULL_DESC, the struct base's,
 * CR_RESULT_NULL_OUTPUT_PARAMETER (and *out set to NULL from there on), then
 * the allocator's (CR_RESULT_NULL_ALLOCATOR, CR_RESULT_INVALID_ALLOCATOR).
 */
CR_RESULT crgpu___internal_check_instance_desc(
    const crgpu_instance_desc *desc,
    crgpu_instance *out_instance);

/* The size of one texel block of [fmt] in bytes: a texel for an uncompressed
 * format, a 4x4 block for a block-compressed one (BC1-BC7). 0 for a format
 * whose texels aren't a whole number of bytes or have no known size. */
uint32_t crgpu___internal_format_block_bytes(CRGPU_FORMAT fmt);

/* The one shader format the backend compiled in takes: SPIR-V (Vulkan),
 * DXIL (D3D12) or a .metallib (Metal). */
CRGPU_SHADER_FORMAT crgpu___internal_shader_format(void);

/* The checks of crgpu_shader_module_create() every backend makes the same
 * way, in the header's order, after the device, desc, base and out_module
 * checks: CR_RESULT_GPU_INVALID_SHADER_STAGE,
 * CR_RESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED, CR_RESULT_GPU_NULL_SHADER_CODE,
 * CR_RESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT,
 * CR_RESULT_GPU_NULL_ENTRY_POINT, CR_RESULT_GPU_INVALID_WORKGROUP_SIZE,
 * CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED,
 * CR_RESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED and
 * CR_RESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED. What is left (whether the
 * code is its format, and has the entry point) only the backend can read.
 * [features] are the device's. */
CR_RESULT crgpu___internal_check_shader_module_desc(
    const crgpu_shader_module_desc *desc,
    CRGPU_FEATURES features);

/* The checks of crgpu_graphics_pipeline_create() on its geometry and
 * tessellation stages, its color attachments and its depth bias, in this
 * order, after the mesh check:
 * CR_RESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED,
 * CR_RESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED,
 * CR_RESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS,
 * CR_RESULT_GPU_DEPTH_BIAS_CLAMP_NOT_SUPPORTED,
 * CR_RESULT_GPU_INDEPENDENT_BLEND_NOT_SUPPORTED. [features] are the
 * device's. */
CR_RESULT crgpu___internal_check_pipeline_outputs(
    const crgpu_graphics_pipeline_desc *desc,
    CRGPU_FEATURES features);

/* Whether blend attachment [a] and [b] blend the same way. Two that don't
 * blend are the same whatever their factors. */
bool crgpu___internal_blend_attachments_equal(
    const crgpu_blend_attachment_desc *a,
    const crgpu_blend_attachment_desc *b);

/* ---------------------------------------------------------------------------
 * Adapters and devices
 * ------------------------------------------------------------------------ */

/* What every backend keeps of an adapter: its properties as
 * crgpu_adapter_get_properties() hands them out (base set to its own type,
 * no chain), and the limits of the chained structs, which this build may not
 * declare. Filled in once, when the instance is created, and never changed,
 * so it is read from any thread with no lock. */
typedef struct crgpu___adapter_facts {
	CRGPU_ADAPTER_PROPERTIES properties;
	uint32_t mesh_max_output_vertices;
	uint32_t mesh_max_output_primitives;
	uint32_t mesh_max_workgroup_invocations;
	uint32_t bindless_max_sampled_images;
	uint32_t bindless_max_storage_images;
	uint32_t bindless_max_storage_buffers;
	// CRGPU_RAY_QUERY_LIMITS, all 0 without CRGPU_FEATURE_RAY_QUERY.
	uint64_t ray_query_max_geometries;
	uint64_t ray_query_max_instances;
	uint64_t ray_query_max_primitives;
	uint32_t ray_query_scratch_offset_alignment;
	uint32_t ray_query_placement_alignment;
} crgpu___adapter_facts;

/* crgpu_adapter_get_properties() after its adapter check, the same on
 * every backend: CR_RESULT_NULL_OUTPUT_PARAMETER, the base's type, then the
 * chain (CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN, _DUPLICATE_IN_CHAIN), all before
 * anything is written; then every struct of the chain is filled in. */
CR_RESULT crgpu___internal_get_adapter_properties(
    const crgpu___adapter_facts *facts,
    CRGPU_ADAPTER_PROPERTIES *out_properties);

/* The checks of crgpu_device_create() after its adapter check, in the
 * header's order: CR_RESULT_NULL_DESC, the base's,
 * CR_RESULT_NULL_OUTPUT_PARAMETER (and *out_device NULL from there on),
 * CR_RESULT_GPU_FEATURE_NOT_SUPPORTED, CR_RESULT_ZERO_SIZE,
 * CR_RESULT_GPU_TOO_MANY_QUEUES, CR_RESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE,
 * CR_RESULT_GPU_OUT_OF_DESCRIPTORS (the bindless table past the budget).
 * The backend then claims the adapter (CR_RESULT_GPU_ADAPTER_IN_USE). */
CR_RESULT crgpu___internal_check_device_desc(
    const crgpu___adapter_facts *facts,
    const crgpu_device_desc *desc,
    crgpu_device *out_device);

/* ---------------------------------------------------------------------------
 * Swap chains
 * ------------------------------------------------------------------------ */

/* The checks of crgpu_swap_chain_create() after its device check, in the
 * header's order, none of which reads the surface: CR_RESULT_NULL_DESC, the
 * base's, CR_RESULT_NULL_OUTPUT_PARAMETER, CR_RESULT_GPU_INVALID_SURFACE,
 * CR_RESULT_GPU_INVALID_COMMAND_QUEUE, CR_RESULT_GPU_FEATURE_NOT_SUPPORTED
 * (no CRGPU_FEATURE_PRESENT in the device's [features]),
 * CR_RESULT_GPU_INVALID_PRESENT_SCALING. The backend's own checks follow,
 * then CR_RESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED, the first that reads
 * the surface. */
CR_RESULT crgpu___internal_check_swap_chain_desc(
    CRGPU_FEATURES features,
    const crgpu_swap_chain_desc *desc,
    const crgpu_swap_chain *out_swap_chain);

/* The checks of crgpu_surface_get_present_scalings() after its device
 * check: CR_RESULT_GPU_INVALID_SURFACE (also for a destroyed surface, in a
 * Debug build), CR_RESULT_NULL_OUTPUT_PARAMETER,
 * CR_RESULT_GPU_FEATURE_NOT_SUPPORTED. */
CR_RESULT crgpu___internal_check_surface_present_scalings(
    CRGPU_FEATURES features,
    const void *surface,
    const uint32_t *out_scalings);

/* How many queues of [type] [desc] asks for; 0 for BUNDLE or a value that
 * is not a type. */
uint32_t crgpu___internal_queue_count(
    const crgpu_device_desc *desc,
    CRGPU_COMMAND_LIST_TYPE type);

/* The descriptors and samplers a pool of [desc] takes out of its device's
 * budget (crgpu_device_desc::max_descriptors). [desc] has passed the
 * pool's own checks. */
void crgpu___internal_pool_descriptor_counts(
    const crgpu_descriptor_pool_desc *desc,
    uint64_t *out_descriptors,
    uint64_t *out_samplers);

/* A device's descriptor budget, as Vulkan and Metal keep it (D3D12 keeps it
 * as its shader-visible heaps). Pools are made and destroyed on any thread,
 * and the budget is the device's, not the caller's, so it has its own lock.
 * Lives where it is never copied. */
typedef struct crgpu___descriptor_budget {
	cr_mutex lock;
	uint64_t max_descriptors;
	uint64_t max_samplers;
	uint64_t used_descriptors;
	uint64_t used_samplers;
} crgpu___descriptor_budget;

/* False when the lock can't be made (CR_RESULT_GENERAL_FAILURE). */
bool crgpu___internal_budget_init(
    crgpu___descriptor_budget *budget,
    uint32_t max_descriptors,
    uint32_t max_samplers);
void crgpu___internal_budget_destroy(crgpu___descriptor_budget *budget);
/* Takes both or neither: CR_RESULT_GPU_OUT_OF_DESCRIPTORS when either
 * doesn't fit in what is left. */
CR_RESULT crgpu___internal_budget_take(
    crgpu___descriptor_budget *budget,
    uint64_t descriptors,
    uint64_t samplers);
void crgpu___internal_budget_give_back(
    crgpu___descriptor_budget *budget,
    uint64_t descriptors,
    uint64_t samplers);

/* ---------------------------------------------------------------------------
 * Acceleration structures (CRGPU_EXT_RAY_QUERY)
 *
 * What every backend checks the same way, written once. A backend's
 * acceleration structure starts with the members of
 * crgpu___acceleration_structure_common, in that order, so the checks
 * below read any backend's through it.
 * ------------------------------------------------------------------------ */

#if CRGPU_EXT_RAY_QUERY

/* What an update must keep of one geometry of a bottom level's last build:
 * the exact values, so no update with another layout reaches the driver.
 * Plain data, 20 bytes. */
typedef struct crgpu___geometry_layout {
	CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_KIND kind;
	CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS flags;
	uint32_t data_count;
	CRGPU_FORMAT index_format;
	uint32_t index_count;
} crgpu___geometry_layout;

typedef EQ_ARRAY(crgpu___geometry_layout) crgpu___geometry_layouts;

/* What a structure is and what it was last built from: everything the
 * checks read, kept on the handle. The hot part (size to the bottom-level
 * count, 48 bytes on a 64-bit target) follows the head's 32 (40 in Debug)
 * on the handle's first two cache lines; the geometry layouts an update
 * compares (cold: read only by an update of a bottom level) after them. Set
 * at creation, and by every build or copy as it is recorded. Lives where it
 * is never copied: the layouts' array points at the allocator beside it. */
typedef struct crgpu___acceleration_structure_state {
	/* The desc's size, level, list capacity and compaction choice. */
	uint64_t size;
	CRGPU_ACCELERATION_STRUCTURE_LEVEL level;
	uint32_t max_bottom_levels;
	bool allow_compaction;
	/* Whether a build or copy into it has been recorded. */
	bool built;
	/* The last build's flags, and its instance count for a top level: with
	 * the geometry layouts below, what an update must keep. */
	CRGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS build_flags;
	uint32_t instance_count;
	/* A top level's bottom levels as its last build named them (R5 of the
	 * draft): max_bottom_levels entries made with the structure, through its
	 * allocator, bottom_level_count of them in use. NULL for a bottom level
	 * or a capacity of 0. */
	crgpu_acceleration_structure *bottom_levels;
	uint32_t bottom_level_count;
	/* A bottom level's last build's geometries, one each, kept only when it
	 * allows updates (no update of another can be taken). Grown through
	 * eq_allocator, the head's allocator, when a build or copy with more
	 * geometries than ever before is recorded: a bottom level's first
	 * update-allowing build allocates it once, and a refit loop never. */
	EQ_ALLOCATOR eq_allocator;
	crgpu___geometry_layouts geometries;
} crgpu___acceleration_structure_state;

/* The front of every backend's struct crgpu_acceleration_structure_t. */
typedef struct crgpu___acceleration_structure_common {
	cr_object_head head;
	crgpu___acceleration_structure_state state;
} crgpu___acceleration_structure_common;

/* The state of [structure], a live handle of any backend. */
static inline crgpu___acceleration_structure_state *crgpu___as_state(crgpu_acceleration_structure structure) {
	return &((crgpu___acceleration_structure_common *)(void *)structure)->state;
}

/* The device's instance layout: the backend's, fixed at compile time. */
crgpu_acceleration_structure_instance_layout crgpu___internal_instance_layout(void);

/* The checks of crgpu_acceleration_structure_create() after its handle,
 * desc, base and out checks and the feature check: the level, a bottom
 * level's max_bottom_levels, the size. */
CR_RESULT crgpu___internal_check_acceleration_structure_desc(const crgpu_acceleration_structure_desc *desc);

/* Makes [state] for [desc] in place: the list of a top level from
 * [allocator], which is the handle's head's (the layouts' array keeps a
 * pointer to the state's copy of it). False when it can't be allocated. */
bool crgpu___internal_acceleration_structure_state_init(
    crgpu___acceleration_structure_state *state,
    const CR_ALLOCATOR *allocator,
    const crgpu_acceleration_structure_desc *desc);
/* Frees what init made. */
void crgpu___internal_acceleration_structure_state_free(
    crgpu___acceleration_structure_state *state,
    const CR_ALLOCATOR *allocator);

/* Every check of crgpu_device_get_acceleration_structure_build_sizes() on
 * [inputs], from its base on, in the header's order. [facts] are the
 * device's adapter's. Reads buffers through the public calls, so a
 * destroyed one is refused as the header says. */
CR_RESULT crgpu___internal_check_acceleration_structure_inputs(
    const crgpu___adapter_facts *facts,
    const crgpu_acceleration_structure_inputs *inputs);

/* The checks of a command list for a build, copy or compacted-size write,
 * after the feature check (card S26): its queue type
 * (CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE for a copy list
 * or a bundle) and whether a rendering pass is open
 * (CR_RESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING). */
CR_RESULT crgpu___internal_check_acceleration_structure_list(CRGPU_COMMAND_LIST_TYPE type, bool rendering);

/* Room in [destination]'s geometry layouts for what a build of [inputs]
 * (which have passed their checks) records: CR_RESULT_OUT_OF_MEMORY when
 * the array can't grow, the structure otherwise unchanged. */
CR_RESULT crgpu___internal_reserve_build_layout(
    crgpu_acceleration_structure destination,
    const crgpu_acceleration_structure_inputs *inputs);

/* The checks of one build of crgpu_command_list_build_acceleration_structures()
 * up to and including its inputs': the base, the mode, inputs NULL, then
 * crgpu___internal_check_acceleration_structure_inputs. */
CR_RESULT crgpu___internal_check_build_head(
    const crgpu___adapter_facts *facts,
    const crgpu_acceleration_structure_build *build);

/* The rest of one build's checks, once the backend knows its [sizes]:
 * source, destination (its size included), the bottom-level limit, then
 * scratch. */
CR_RESULT crgpu___internal_check_build_tail(
    const crgpu___adapter_facts *facts,
    const crgpu_acceleration_structure_build *build,
    const crgpu_acceleration_structure_sizes *sizes);

/* What a recorded build leaves on its destination: built, its flags and
 * layout hash, a top level's bottom levels. */
void crgpu___internal_record_build(const crgpu_acceleration_structure_build *build);

/* crgpu_command_list_copy_acceleration_structure()'s checks after cmd, the
 * feature and the list's, in the header's order, then room in the
 * destination's geometry layouts for the source's
 * (CR_RESULT_OUT_OF_MEMORY). */
CR_RESULT crgpu___internal_check_copy(
    crgpu_acceleration_structure source,
    crgpu_acceleration_structure destination,
    CRGPU_ACCELERATION_STRUCTURE_COPY_MODE mode);
/* What a recorded copy leaves on its destination: the source's build. */
void crgpu___internal_record_copy(crgpu_acceleration_structure source, crgpu_acceleration_structure destination);

/* crgpu_command_list_write_acceleration_structure_compacted_sizes()'s checks after
 * cmd and the feature, in the header's order. */
CR_RESULT crgpu___internal_check_compacted_sizes(
    const crgpu_acceleration_structure *structures,
    uint32_t count,
    crgpu_buffer buffer,
    uint64_t offset);

/* crgpu_command_list_acceleration_structure_barrier()'s element checks: false when
 * the call records nothing. */
bool crgpu___internal_check_acceleration_structure_barriers(
    const crgpu_acceleration_structure_barrier *barriers,
    uint32_t count);

/* crgpu_device_write_acceleration_structure_instances() after its device and
 * feature checks: the argument checks, every instance's, then the writes in
 * [layout]. */
CR_RESULT crgpu___internal_write_instances(
    crgpu_acceleration_structure_instance_layout layout,
    const crgpu_acceleration_structure_instance *instances,
    uint32_t count,
    void *out_bytes);

/* The descriptor write checks every backend makes of an
 * CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE write before any write:
 * false when the call writes nothing. [features] are the device's. */
bool crgpu___internal_check_acceleration_structure_writes(
    CRGPU_FEATURES features,
    const crgpu_write_descriptor_set *writes,
    uint32_t write_count);

#endif // CRGPU_EXT_RAY_QUERY

/* Whether a descriptor set layout or pool may name [type] on a device of
 * [features]: CR_RESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE for
 * a type that is not one, CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED for
 * CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE without
 * CRGPU_FEATURE_RAY_QUERY (card S23). */
CR_RESULT crgpu___internal_check_descriptor_type(CRGPU_DESCRIPTOR_TYPE type, CRGPU_FEATURES features);

/* ---------------------------------------------------------------------------
 * Live objects (debug builds)
 *
 * A debug build counts, per device, the objects of each kind made from it
 * and not yet destroyed, and crgpu_device_destroy() reports any left and
 * asserts. Release builds have no counter: the macros below compile to
 * nothing there, and the counter members are under #if _DEBUG.
 * ------------------------------------------------------------------------ */

typedef enum CRGPU___OBJECT_KIND {
	CRGPU___OBJECT_BUFFER = 0,
	CRGPU___OBJECT_BUFFER_VIEW,
	CRGPU___OBJECT_IMAGE,
	CRGPU___OBJECT_IMAGE_VIEW,
	CRGPU___OBJECT_SAMPLER,
	CRGPU___OBJECT_HEAP,
	CRGPU___OBJECT_SHADER_MODULE,
	CRGPU___OBJECT_SHADER_PIPELINE,
	CRGPU___OBJECT_COMPUTE_PIPELINE,
	CRGPU___OBJECT_DESCRIPTOR_SET_LAYOUT,
	CRGPU___OBJECT_DESCRIPTOR_POOL,
	CRGPU___OBJECT_COMMAND_ALLOCATOR,
	CRGPU___OBJECT_COMMAND_LIST,
	CRGPU___OBJECT_FENCE,
	CRGPU___OBJECT_SEMAPHORE,
	CRGPU___OBJECT_SWAP_CHAIN,
	CRGPU___OBJECT_TIMESTAMP_SET,
	CRGPU___OBJECT_ACCELERATION_STRUCTURE,
	CRGPU___OBJECT_KIND_COUNT
} CRGPU___OBJECT_KIND;

#if _DEBUG
typedef struct crgpu___live_objects {
	cr_mutex lock;
	uint64_t counts[CRGPU___OBJECT_KIND_COUNT];
} crgpu___live_objects;

bool crgpu___internal_live_init(crgpu___live_objects *live);
void crgpu___internal_live_destroy(crgpu___live_objects *live);
void crgpu___internal_live_add(crgpu___live_objects *live, CRGPU___OBJECT_KIND kind);
void crgpu___internal_live_remove(crgpu___live_objects *live, CRGPU___OBJECT_KIND kind);
/* Prints every kind still alive on the device named [device_name] (or
 * unnamed) and asserts none is. */
void crgpu___internal_live_check_none(crgpu___live_objects *live, const char *device_name);

#define CRGPU___LIVE_ADD(live, kind) crgpu___internal_live_add((live), (kind))
#define CRGPU___LIVE_REMOVE(live, kind) crgpu___internal_live_remove((live), (kind))
#else
#define CRGPU___LIVE_ADD(live, kind) ((void)0)
#define CRGPU___LIVE_REMOVE(live, kind) ((void)0)
#endif

#if __cplusplus
}
#endif

#endif // CRGPUSHARED_H
