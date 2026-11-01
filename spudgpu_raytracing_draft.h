/*
 * spudgpu_raytracing_draft.h - SPUDGPU_EXT_RAY_QUERY: acceleration structures
 * and inline ray queries in compute and fragment shaders (card S12, step 1;
 * instances by card S14; named by card S15). For review:
 * prompts/ray_tracing.md, Phase 2.
 *
 * APPROVED 2026-10-31 by the user, as written here (the workspace's
 * DECISIONS.md, "Decided"). Built into spudgpu.h and spudcore.h in Phase 5;
 * where building finds this draft wrong or incomplete, a card says so and
 * this file is not edited to match.
 *
 * DRAFT, 2026-10-31. Not compilable: it names fields to add
 * to structs spudgpu.h declares. Every declaration is marked [ADDED],
 * [CHANGED] or [REMOVED].
 *
 * ---------------------------------------------------------------------------
 * The shape, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * An acceleration structure is a handle: a bottom level over triangles or
 * AABBs, or a top level over instances of bottom levels. The caller asks
 * how large one must be and how much scratch its build needs
 * (spudgpu_get_acceleration_structure_build_sizes()), creates it at that
 * size (its storage committed, or placed in the caller's heap), and records
 * builds and updates in a command list, many in one call, with scratch from
 * a buffer of its own. Instances are written in the device's own layout
 * (S14): the caller asks for the layout, and either writes instances with
 * spudgpu_write_acceleration_structure_instances() (a CPU translation into
 * memory it names, no state) or writes the bytes itself on the GPU. A top
 * level is bound as a descriptor of its own type; shaders query it with
 * Slang's RayQuery (SPIR-V SPV_KHR_ray_query, DXIL 1.1 inline, Metal
 * intersection_query). Builds, compaction and traces are ordered by the
 * caller's barriers. SpudGPU keeps no pool, no cache and no scratch, and
 * chooses no build flag.
 *
 * ---------------------------------------------------------------------------
 * What this decides that no card has (each becomes a card if overturned)
 * ---------------------------------------------------------------------------
 *
 *  R1. A handle, not a range of a buffer. Vulkan has a VkAccelerationStructureKHR
 *      over a buffer range, D3D12 a GPU address in a buffer in the
 *      RAYTRACING_ACCELERATION_STRUCTURE state, Metal an
 *      MTLAccelerationStructure object (from the device or a heap), which
 *      no buffer range can stand for. So the narrowest contract is an
 *      object, created at the size a query gave, as a buffer is: committed
 *      (spudgpu_create_acceleration_structure: Vulkan a buffer and the
 *      object over it, D3D12 a committed buffer, Metal
 *      newAccelerationStructureWithSize:) or placed in the caller's heap of
 *      the new SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES (Vulkan a
 *      placed buffer, D3D12 a placed buffer, Metal
 *      newAccelerationStructureWithSize:offset:), for a caller whose
 *      allocator places everything.
 *  R2. Geometry has no per-geometry transform. Vulkan's and D3D12's 3x4
 *      row-major transform buffer and Metal's packed 4x3 column-major one
 *      differ in layout; instances carry transforms on all three. Vertex
 *      positions are three 32-bit floats (SPUDGPU_FORMAT_R32G32B32_FLOAT,
 *      the one format all three take on every device), indices 16 or 32 bits.
 *  R3. Build flags are the caller's and only those all three keep:
 *      PREFER_FAST_TRACE or PREFER_FAST_BUILD (at most one; Metal has no
 *      fast-trace flag, its default), ALLOW_UPDATE (Metal's refit usage),
 *      ALLOW_COMPACTION (Metal can always compact; the rule is the
 *      caller's on every backend and checked the same way). No
 *      MINIMIZE_MEMORY (Metal has none).
 *  R4. Builds, updates, copies and the compacted size are commands that
 *      return a SPUDRESULT and record nothing on a refusal. SpudGPU's draw
 *      and dispatch calls are void because they are per draw; these have
 *      many distinct failures (a range outside its buffer, too little
 *      storage or scratch, an update that changes counts) and are per
 *      build, so each failure has its code, checked before anything is
 *      recorded.
 *  R5. Metal residency, from the build's own input. A Metal top level that
 *      references bottom levels through indirect instances needs them made
 *      resident wherever it is built or traced, and nothing in the GPU's
 *      instance bytes can tell the backend which they are. So a top level's
 *      build names the bottom levels its instances reference (at most
 *      `max_bottom_levels`, given at its creation, so the list is copied
 *      into the top level with no allocation per build), on every backend;
 *      Metal makes them resident at the build and whenever a set holding
 *      the top level is bound; Vulkan and D3D12 check the count and keep the
 *      list unread. The lifetime rule in the header: every bottom level a
 *      top level's last build named outlives the top level's use.
 *  R6. Compaction writes the compacted size, a uint64_t, into the caller's
 *      buffer: D3D12's postbuild info, Metal's
 *      writeCompactedAccelerationStructureSize:...sizeDataType:ULong, and on
 *      Vulkan a query the acceleration structure owns (created with it when
 *      ALLOW_COMPACTION is set, one query, freed with it) written and copied
 *      into the buffer. The caller reads it back and creates the compacted
 *      structure at that size, then copies into it.
 *  R7. A new barrier call inside the EXT, not a third array on
 *      spudgpu_cmd_pipeline_barrier(): an acceleration structure moves
 *      between accesses (built, read by a build, traced) that no resource
 *      state names, and the core call stays as it is. Build inputs and
 *      scratch get two resource states of their own (appended to
 *      SPUDGPU_RESOURCE_STATE), since D3D12's NON_PIXEL_SHADER_RESOURCE and
 *      UNORDERED_ACCESS and Vulkan's ACCELERATION_STRUCTURE_BUILD stage
 *      don't map to an existing state's stage.
 *  R8. Instances (S14): the portable struct holds a row-major 3x4
 *      transform, a 24-bit user index, an 8-bit mask, a 24-bit table offset
 *      (unused by inline queries; kept for the pipeline EXTs, never built
 *      here), the four flags all three share, and the bottom level's
 *      reference. The write call takes many at once (batch shaped): a
 *      caller writes a top level's instances in one call into a mapped
 *      buffer, never one call per instance.
 *
 * ---------------------------------------------------------------------------
 * What each backend does
 * ---------------------------------------------------------------------------
 *
 *  Vulkan: VK_KHR_acceleration_structure, VK_KHR_ray_query and
 *    VK_KHR_deferred_host_operations (required by the first; never used:
 *    builds are recorded, never on the host), their features read when
 *    adapters are listed, enabled only on a device created with
 *    SPUDGPU_FEATURE_RAY_QUERY (bufferDeviceAddress is already in the base
 *    set). vkGetAccelerationStructureBuildSizesKHR,
 *    vkCmdBuildAccelerationStructuresKHR (one call for many builds),
 *    vkCmdCopyAccelerationStructureKHR, vkCmdWriteAccelerationStructuresPropertiesKHR.
 *  D3D12: D3D12_FEATURE_DATA_D3D12_OPTIONS5::RaytracingTier >= 1.1 (inline
 *    ray queries); ID3D12GraphicsCommandList4's
 *    BuildRaytracingAccelerationStructure (one call per build),
 *    CopyRaytracingAccelerationStructure, EmitRaytracingAccelerationStructurePostbuildInfo;
 *    GetRaytracingAccelerationStructurePrebuildInfo. Shader model 6.5.
 *  Metal (written, not compiled): supportsRaytracing and the Apple7 family
 *    or later; MTLAccelerationStructureCommandEncoder build, refit, copy,
 *    copyAndCompact and writeCompactedAccelerationStructureSize;
 *    accelerationStructureSizesWithDescriptor:; instance descriptors of
 *    MTLAccelerationStructureInstanceDescriptorTypeIndirect (72 bytes) with
 *    the bottom level's gpuResourceID; useResource: for residency (R5).
 *  The Iris Xe reports no ray query on either API; WARP's answer is
 *  recorded when it runs.
 *
 * ---------------------------------------------------------------------------
 * The rules every backend is held to (the narrowest contract)
 * ---------------------------------------------------------------------------
 *
 *  - Alignments are the device's, reported (SPUDGPU_RAY_QUERY_LIMITS):
 *    scratch offset; a placed structure's offset; the instance buffer's
 *    offset (the instance layout's alignment). AABB data: offset and stride
 *    multiples of 8, stride at least 24. Vertex data: offset and stride
 *    multiples of 4. Index data: offset a multiple of the index size.
 *  - Every range is inside its buffer, and every count within its limit,
 *    checked with overflow-safe arithmetic before anything is recorded.
 *  - A build's input buffers (vertices, indices, AABBs, instances) and its
 *    scratch live, unchanged, until the build's submission completes.
 *  - An update keeps the build's geometry kinds, counts, flags and index
 *    layout; only positions, AABBs and instance contents change; the source
 *    was built with ALLOW_UPDATE. In place (source = destination) or into
 *    another structure of at least the update's size.
 *  - A structure is used in a trace only after a barrier from BUILD_WRITE
 *    to TRACE; a build that reads one (a top level over bottom levels, an
 *    update's source, a copy's source) only after a barrier from
 *    BUILD_WRITE to BUILD_READ. Input buffers move to
 *    ACCELERATION_STRUCTURE_BUILD_INPUT and scratch to
 *    ACCELERATION_STRUCTURE_SCRATCH, as other buffers move.
 *  - Ray queries are in compute and fragment shaders.
 *
 * ---------------------------------------------------------------------------
 * ABI breaks (abi.txt)
 * ---------------------------------------------------------------------------
 *
 *  - SPUDGPU_BUFFER_USAGE_RAYTRACING_ACCELERATION_STRUCTURE renamed
 *    SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT, value kept
 *    (no caller set it; it meant storage, which is now the handle's).
 *  - spudgpu_write_descriptor_set gains `acceleration_structures`.
 *  - Additions only otherwise: a feature bit, a result block, two resource
 *    states, a heap class, a descriptor type, a chained limits struct, and
 *    the EXT's types and calls.
 *
 * ---------------------------------------------------------------------------
 * Tests (spudgpu_objects_test unless named; pending until built)
 * ---------------------------------------------------------------------------
 *
 *  - A bottom level of one triangle and one of an AABB; a top level of both;
 *    rq_trace.slang tracing a grid of rays, read back against hits worked
 *    out on the CPU (t, instance index and id, geometry, primitive,
 *    barycentrics, kind); rq_shadow.slang drawn over a plane, read back.
 *  - The top level's instance moved and refitted: the same grid's hits move.
 *  - Compaction: the size written, a compacted copy made, the same hits.
 *  - Instances written by the CPU call and by a compute shader (the caller's
 *    GPU writer), the same bytes for the same input.
 *  - Every refusal of every call, in parameter order, on every backend; on
 *    a device created without the feature (the Iris Xe is the real one)
 *    each call's SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED.
 *  - spud_noalloc_tests: build once, then refit the top level and trace in
 *    a loop: no allocation of SpudLib's own. Creating a structure allocates
 *    its handle only (the bytes are measured and stated).
 *  - The misuse harness regenerated, with a factory for the new handle.
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 *  Vulkan VK_KHR_acceleration_structure / ray_query; D3D12 DXR 1.1
 *  (inline); Metal's acceleration structures and intersection queries
 *  (Metal 3, indirect instances). NVRHI's rt::AccelStruct (a handle with a
 *  desc, builds on a command list), WebGPU's ray tracing proposal (a
 *  handle), Unreal's RHI (a buffer-backed object, sizes from a query).
 */

#ifndef SPUDGPU_RAYTRACING_DRAFT_H
#define SPUDGPU_RAYTRACING_DRAFT_H

#include "include/spudgpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Always declared (no #if in a struct; bits and values exist on every
 * backend, never set where the EXT is 0) ---------------------------------- */

/** [ADDED] The adapter can build acceleration structures and run ray queries
 * in compute and fragment shaders (SPUDGPU_EXT_RAY_QUERY). Vulkan:
 * VK_KHR_acceleration_structure and VK_KHR_ray_query with their features.
 * D3D12: raytracing tier 1.1. Metal: supportsRaytracing on an Apple7 or
 * later GPU. */
#define SPUDGPU_FEATURE_RAY_QUERY (UINT64_C(1) << 13)

/** [ADDED] An acceleration structure: a bottom level (triangles or AABBs) or
 * a top level (instances of bottom levels). Declared on every backend, so a
 * struct can name it; its calls exist only with SPUDGPU_EXT_RAY_QUERY. */
typedef struct spudgpu_acceleration_structure_t *spudgpu_acceleration_structure;

/* [ADDED] spudcore.h, in the EXT block after 327:
 *   SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED = 328 (the device wasn't
 *   created with SPUDGPU_FEATURE_RAY_QUERY).
 * [ADDED] spudcore.h, a block of their own (360 on):
 *   SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE         = 360, NULL, or the wrong level for the call
 *   SPUDRESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL       = 361, smaller than the build or copy needs
 *   SPUDRESULT_GPU_ACCELERATION_STRUCTURE_SCRATCH_TOO_SMALL = 362, scratch range smaller than needed
 *   SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE           = 363, an input range outside its buffer
 *   SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT       = 364, an offset or stride off its alignment
 *   SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT           = 365, a count past the device's limit
 *   SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED  = 366, an update its source can't take (R3, the update rule)
 *   SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED = 367, a compaction its source wasn't built for
 */

/* [CHANGED] SPUDGPU_BUFFER_USAGE_RAYTRACING_ACCELERATION_STRUCTURE (1 << 6)
 * becomes SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT: the buffer
 * holds vertices, indices, AABBs or instances a build reads (Vulkan
 * ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY and SHADER_DEVICE_ADDRESS).
 * Scratch is a SPUDGPU_BUFFER_USAGE_STORAGE buffer. */

/* [ADDED] SPUDGPU_RESOURCE_STATE, appended:
 *   SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT, read by a
 *     build (D3D12 NON_PIXEL_SHADER_RESOURCE; Vulkan the build stage's
 *     shader read);
 *   SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH, a build's scratch
 *     (D3D12 UNORDERED_ACCESS; Vulkan the build stage's reads and writes);
 *   SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SIZE_DEST, written by
 *     spudgpu_cmd_write_acceleration_structure_compacted_sizes() (D3D12
 *     UNORDERED_ACCESS; Vulkan a transfer write). */

/* [ADDED] SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES = 4: a heap of
 * placed acceleration structures (R1). */

/* [ADDED] SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE = 6: a top level,
 * read by ray queries. D3D12 an SRV (t<b>), Metal an argument buffer member,
 * Vulkan VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR. */

/* [CHANGED] spudgpu_write_descriptor_set gains, after `sampler`:
 *   const spudgpu_acceleration_structure *acceleration_structures;
 *   descriptor_count top levels, for SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
 *   borrowed for the call. */

/**
 * @brief [ADDED] Chained onto SPUDGPU_ADAPTER_PROPERTIES: the device's ray
 * query limits. Zero on an adapter without SPUDGPU_FEATURE_RAY_QUERY.
 */
typedef struct SPUDGPU_RAY_QUERY_LIMITS {
	/** `type` is SPUD_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS. Filled by SpudGPU. */
	SPUD_STRUCT_BASE_OUT base;
	/** Most geometries in one bottom level. Vulkan maxGeometryCount; D3D12
	 * 2^24; Metal 2^24 - 1 (narrower of Apple's documented limits). */
	uint64_t max_geometries;
	/** Most instances in one top level. Vulkan maxInstanceCount; D3D12
	 * 2^24; Metal 2^24 - 1. */
	uint64_t max_instances;
	/** Most triangles or AABBs in one bottom level, all geometries summed.
	 * Vulkan maxPrimitiveCount; D3D12 2^29; Metal 2^28 - 1. */
	uint64_t max_primitives;
	/** What a build's scratch offset is a multiple of. Vulkan
	 * minAccelerationStructureScratchOffsetAlignment; D3D12 256; Metal 256. */
	uint32_t scratch_offset_alignment;
	/** What a placed structure's offset in its heap is a multiple of. Vulkan
	 * 256; D3D12 256; Metal the heap's sizeAndAlign for the size. */
	uint32_t placement_alignment;
} SPUDGPU_RAY_QUERY_LIMITS;

#if SPUDGPU_EXT_RAY_QUERY /* [ADDED] 1 on Vulkan, D3D12 and Metal. */

/** @brief [ADDED] A bottom or top level. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_LEVEL;
enum {
	SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_NONE   = 0, /**< Refused. */
	SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM = 1, /**< Triangles or AABBs. */
	SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP    = 2  /**< Instances of bottom levels. */
};

/** @brief [ADDED] Build flags (R3). */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS;
enum {
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE = 1u << 0, /**< Not with PREFER_FAST_BUILD. */
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD = 1u << 1, /**< Not with PREFER_FAST_TRACE. */
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE      = 1u << 2, /**< It may be updated (refitted). */
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION  = 1u << 3  /**< It may be compacted. */
};

/** @brief [ADDED] A geometry's flags: the two all three APIs have. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS;
enum {
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE                = 1u << 0, /**< Its hits are committed without the shader's say. */
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_CANDIDATES = 1u << 1  /**< A primitive is a candidate at most once per query. */
};

/** @brief [ADDED] What a geometry is. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_KIND;
enum {
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_NONE      = 0, /**< Refused. */
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES = 1,
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_AABBS     = 2  /**< Procedural: each box a candidate the shader decides. */
};

/** @brief [ADDED] One geometry of a bottom level. */
/* Frozen: an array element of spudgpu_acceleration_structure_inputs. */
typedef struct spudgpu_acceleration_structure_geometry {
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_KIND kind;
	SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS flags;
	/** Triangles: float3 positions. AABBs: (min xyz, max xyz) floats.
	 * Borrowed until the build's submission completes. */
	spudgpu_buffer data;
	/** Byte offset of the first vertex or box. */
	uint64_t data_offset;
	/** Bytes from one vertex or box to the next. */
	uint64_t data_stride;
	/** Triangles: vertices. AABBs: boxes (the primitives). */
	uint32_t data_count;
	/** Triangles: SPUDGPU_FORMAT_R16_UINT or SPUDGPU_FORMAT_R32_UINT with
	 * an index buffer (as index buffer views name their width), or
	 * SPUDGPU_FORMAT_UNKNOWN for three vertices per triangle. AABBs:
	 * SPUDGPU_FORMAT_UNKNOWN. */
	SPUDGPU_FORMAT index_format;
	/** The index buffer, or NULL with SPUDGPU_FORMAT_UNKNOWN. */
	spudgpu_buffer indices;
	/** Byte offset of the first index. */
	uint64_t index_offset;
	/** Triangles, with indices: indices / 3 is the triangle count. */
	uint32_t index_count;
} spudgpu_acceleration_structure_geometry;

/**
 * @brief [ADDED] What a build reads: a bottom level's geometries or a top
 * level's instances. The same inputs give the size query and the build.
 */
typedef struct spudgpu_acceleration_structure_inputs {
	/** `type` is SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS. */
	SPUD_STRUCT_BASE base;
	SPUDGPU_ACCELERATION_STRUCTURE_LEVEL level;
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags;
	/** Bottom level: its geometries, all of one kind. Borrowed for the call. */
	const spudgpu_acceleration_structure_geometry *geometries;
	uint32_t geometry_count;
	/** Top level: instances in the device's layout
	 * (spudgpu_get_acceleration_structure_instance_layout()). Borrowed until
	 * the build's submission completes. */
	spudgpu_buffer instances;
	uint64_t instance_offset;
	uint32_t instance_count;
	/** Top level: every bottom level its instances reference (R5), at most
	 * the top level's max_bottom_levels. Copied by the build. */
	const spudgpu_acceleration_structure *bottom_levels;
	uint32_t bottom_level_count;
} spudgpu_acceleration_structure_inputs;

/** @brief [ADDED] What a structure and its builds need. */
typedef struct spudgpu_acceleration_structure_sizes {
	/** `type` is SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES. Filled by SpudGPU. */
	SPUD_STRUCT_BASE_OUT base;
	/** Bytes the structure is created with (at least). */
	uint64_t storage_size;
	/** Scratch bytes a build needs. */
	uint64_t build_scratch_size;
	/** Scratch bytes an update needs (0 without ALLOW_UPDATE). */
	uint64_t update_scratch_size;
} spudgpu_acceleration_structure_sizes;

/**
 * @brief [ADDED] The sizes a build of @p inputs needs. Vulkan
 * vkGetAccelerationStructureBuildSizesKHR; D3D12
 * GetRaytracingAccelerationStructurePrebuildInfo; Metal
 * accelerationStructureSizesWithDescriptor:. Records nothing.
 *
 * @return SPUD_SUCCESS; GPU_INVALID_DEVICE; NULL_DESC; the base's;
 * NULL_OUTPUT_PARAMETER; GPU_EXT_RAY_QUERY_NOT_SUPPORTED; the inputs'
 * checks (a level, a kind, flags, counts past limits, alignments, ranges),
 * each its code, in field order.
 */
SPUDRESULT spudgpu_get_acceleration_structure_build_sizes(
    spudgpu_device device,
    const spudgpu_acceleration_structure_inputs *inputs,
    spudgpu_acceleration_structure_sizes *out_sizes);

/** @brief [ADDED] Creates an acceleration structure. */
typedef struct spudgpu_acceleration_structure_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC. */
	SPUD_STRUCT_BASE base;
	SPUDGPU_ACCELERATION_STRUCTURE_LEVEL level;
	/** Bytes: a size query's storage_size, or a compacted size (R6). */
	uint64_t size;
	/** Top level: the most bottom levels a build may name (R5); 0 for a
	 * bottom level. */
	uint32_t max_bottom_levels;
	/** Whether a compacted size may be written for it (Vulkan: its query is
	 * made with it). */
	bool allow_compaction;
	/** A name for diagnostics, or NULL. Copied. */
	const char *debug_name;
} spudgpu_acceleration_structure_desc;

/** @brief [ADDED] Creates one with storage of its own (R1). */
SPUDRESULT spudgpu_create_acceleration_structure(
    spudgpu_device device,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure);

/** @brief [ADDED] Creates one placed at @p offset in @p heap, a heap of
 * SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES; the offset a multiple
 * of placement_alignment, the range inside the heap. SpudGPU checks no
 * overlap, as for placed buffers. */
SPUDRESULT spudgpu_create_placed_acceleration_structure(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure);

/** @brief [ADDED] Destroys one. The caller has waited for every submission
 * that builds, copies or traces it, and for every top level built over it
 * (R5) to be destroyed or rebuilt without it. NULL does nothing. */
void spudgpu_destroy_acceleration_structure(spudgpu_acceleration_structure structure);

/** @brief [ADDED] A bottom level's reference for an instance (S14): Vulkan
 * its device address, D3D12 its GPU virtual address, Metal its
 * gpuResourceID. Constant for the structure's life. */
SPUDRESULT spudgpu_get_acceleration_structure_reference(
    spudgpu_acceleration_structure bottom_level,
    uint64_t *out_reference);

/** @brief [ADDED] How the device lays an instance out (S14). */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT;
enum {
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_NONE = 0,
	/** 64 bytes: a row-major 3x4 float transform, a 24-bit user index and
	 * 8-bit mask, a 24-bit table offset and 8 bits of flags, the reference
	 * (Vulkan, D3D12). */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64 = 1,
	/** 72 bytes: a column-major packed 4x3 transform, 32-bit options, mask,
	 * table offset and user index, the reference (Metal's indirect
	 * instance descriptor). */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72 = 2
};

/** @brief [ADDED] An instance's layout on a device. */
/* Frozen: a value. */
typedef struct spudgpu_acceleration_structure_instance_layout {
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT kind;
	/** Bytes per instance, and the stride between them. */
	uint32_t size;
	/** What the instance buffer's offset is a multiple of. */
	uint32_t alignment;
} spudgpu_acceleration_structure_instance_layout;

SPUDRESULT spudgpu_get_acceleration_structure_instance_layout(
    spudgpu_device device,
    spudgpu_acceleration_structure_instance_layout *out_layout);

/** @brief [ADDED] The flags an instance has on all three APIs. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FLAGS;
enum {
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_DISABLE_TRIANGLE_CULLING = 1u << 0,
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FRONT_COUNTERCLOCKWISE   = 1u << 1,
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE             = 1u << 2, /**< Not with FORCE_NON_OPAQUE. */
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_NON_OPAQUE         = 1u << 3  /**< Not with FORCE_OPAQUE. */
};

/** @brief [ADDED] One instance, portable (R8). */
/* Frozen: an array element of spudgpu_write_acceleration_structure_instances(). */
typedef struct spudgpu_acceleration_structure_instance {
	/** Object to world, row-major 3x4: rows of (x, y, z, translation). */
	float transform[12];
	/** Below 2^24: the shader's CommittedInstanceID. */
	uint32_t user_index;
	/** Tested against a query's cull mask. */
	uint8_t mask;
	/** Below 2^24. Unused by ray queries. */
	uint32_t table_offset;
	SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FLAGS flags;
	/** spudgpu_get_acceleration_structure_reference() of its bottom level. */
	uint64_t bottom_level_reference;
} spudgpu_acceleration_structure_instance;

/**
 * @brief [ADDED] Writes @p count instances in @p device's layout into
 * @p out_bytes (count x layout size bytes, the layout's alignment): a CPU
 * translation, no state, nothing recorded (S14). A mapped instance buffer,
 * or any memory the caller copies from.
 *
 * @return SPUD_SUCCESS; GPU_INVALID_DEVICE; GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
 * NULL_DESC (@p instances NULL); ZERO_SIZE; NULL_OUTPUT_PARAMETER; then per
 * instance DESC_INVALID_PARAMETERS for a user index or table offset of
 * 2^24 or more, unknown flags or both opacity flags. Nothing is written on
 * a refusal.
 */
SPUDRESULT spudgpu_write_acceleration_structure_instances(
    spudgpu_device device,
    const spudgpu_acceleration_structure_instance *instances,
    uint32_t count,
    void *out_bytes);

/** @brief [ADDED] Build or update. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE;
enum {
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD  = 1,
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE = 2  /**< From `source`, built with ALLOW_UPDATE. */
};

/** @brief [ADDED] One build in spudgpu_cmd_build_acceleration_structures(). */
typedef struct spudgpu_acceleration_structure_build {
	/** `type` is SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD. */
	SPUD_STRUCT_BASE base;
	SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE mode;
	/** What it reads. */
	const spudgpu_acceleration_structure_inputs *inputs;
	/** UPDATE: the structure updated from (may be #destination). */
	spudgpu_acceleration_structure source;
	spudgpu_acceleration_structure destination;
	/** Scratch, at least the mode's scratch size, at scratch_offset_alignment. */
	spudgpu_buffer scratch;
	uint64_t scratch_offset;
} spudgpu_acceleration_structure_build;

/**
 * @brief [ADDED] Records @p count builds and updates (R4). Vulkan one
 * vkCmdBuildAccelerationStructuresKHR; D3D12 one BuildRaytracingAccelerationStructure
 * each; Metal one acceleration structure encoder. Builds in one call may run
 * at once: no two of them write the same structure or scratch range, and
 * none reads what another writes.
 *
 * @return SPUD_SUCCESS, or the first refusal (each build's checks in order,
 * the builds in order), with nothing recorded.
 */
SPUDRESULT spudgpu_cmd_build_acceleration_structures(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure_build *builds,
    uint32_t count);

/** @brief [ADDED] Copy kinds. */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE;
enum {
	SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE   = 1, /**< Into one of at least the source's size. */
	SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT = 2  /**< Into one of at least its compacted size; the source built with ALLOW_COMPACTION. */
};

/** @brief [ADDED] Records a copy. Returns as the build does. */
SPUDRESULT spudgpu_cmd_copy_acceleration_structure(
    spudgpu_command_list cmd,
    spudgpu_acceleration_structure source,
    spudgpu_acceleration_structure destination,
    SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE mode);

/**
 * @brief [ADDED] Records writing each structure's compacted size, a
 * uint64_t, into @p buffer from @p offset (8-byte aligned, count x 8 bytes
 * inside it; state ACCELERATION_STRUCTURE_SIZE_DEST) (R6). Each structure
 * created with allow_compaction and built with ALLOW_COMPACTION.
 */
SPUDRESULT spudgpu_cmd_write_acceleration_structure_compacted_sizes(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure *structures,
    uint32_t count,
    spudgpu_buffer buffer,
    uint64_t offset);

/** @brief [ADDED] How a structure is being used, for its barriers (R7). */
typedef uint32_t SPUDGPU_ACCELERATION_STRUCTURE_ACCESS;
enum {
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE        = 0, /**< Not yet used, or after a submission ends. */
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE = 1, /**< Built, updated, or copied into. */
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ  = 2, /**< Read by a build, an update, a copy or a size write. */
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE       = 3  /**< Read by ray queries in compute and fragment shaders. */
};

/** @brief [ADDED] One structure's barrier. */
typedef struct spudgpu_acceleration_structure_barrier {
	/** `type` is SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER. */
	SPUD_STRUCT_BASE base;
	spudgpu_acceleration_structure structure;
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS access_before;
	SPUDGPU_ACCELERATION_STRUCTURE_ACCESS access_after;
} spudgpu_acceleration_structure_barrier;

/**
 * @brief [ADDED] Records barriers between structures' accesses. Vulkan a
 * memory barrier from the build stage (or compute and fragment shaders) to
 * the next; D3D12 a UAV barrier on the structure's buffer; Metal nothing
 * beyond its own tracking. Void, as spudgpu_cmd_pipeline_barrier() is:
 * nothing is recorded if an element's base is wrong.
 */
void spudgpu_cmd_acceleration_structure_barrier(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure_barrier *barriers,
    uint32_t count);

#endif /* SPUDGPU_EXT_RAY_QUERY */

/* ---------------------------------------------------------------------------
 * NOT added (each would be its own draft): SPUDGPU_EXT_RAY_TRACING_PIPELINE
 * and SPUDGPU_EXT_RAY_FUNCTION_TABLES (S12); per-geometry transforms (R2);
 * vertex formats beyond float3; MINIMIZE_MEMORY; host-side builds
 * (Vulkan's deferred operations, never used); acceleration structure
 * serialization.
 * ------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* SPUDGPU_RAYTRACING_DRAFT_H */
