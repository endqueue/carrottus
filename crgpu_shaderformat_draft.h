/*
 * crgpu_shaderformat_draft.h - each graphics API's own shader format, and
 * no compiler in Carrottus (card A9, revised 2026-10-28; the source language
 * is card A29, 2026-10-31). For review: prompts/ray_tracing.md, Phase 2.
 *
 * APPROVED 2026-10-31 by the user, as written here, with cards S17, S18,
 * S19 and S20 as recommended (the workspace's DECISIONS.md, "Decided").
 * Built into crgpu.h and crcore.h in Phase 3; where building finds this draft wrong
 * or incomplete, a card says so and this file is not edited to match
 * (card S21: F4's sentence on Slang's own Metal output).
 *
 * DRAFT, 2026-10-31. Not compilable: it redeclares
 * crgpu_shader_module_desc and two results that crgpu.h and crcore.h
 * declare. Every declaration below is marked [ADDED], [CHANGED] or
 * [REMOVED]; nothing unmarked changes. Three points rest on cards written
 * the same day, each shown here as its recommendation:
 *   - S17, what a module carries besides its bytes (option a);
 *   - S19, clip space's Y (option a);
 *   - S20, where the Metal fallback's cross-compiler lives (option a).
 * Card S18 (specialization constants) is not part of this change: they are
 * still unbuilt, as A9 left them.
 *
 * ---------------------------------------------------------------------------
 * The shape, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * A shader module is made from the bytes the device's own graphics API
 * takes: SPIR-V on Vulkan, DXIL on D3D12, a .metallib on Metal. Its desc
 * names that format, the module's entry point and, for a compute, mesh or
 * task shader, its workgroup size; any other format is refused. Carrottus GPU
 * translates nothing and compiles nothing: SPIRV-Cross and DXC leave the
 * library, and with them every choice they made for the caller (a shader
 * model, a register mapping, an MSL version, a flipped Y). Where each
 * binding sits in a module is a rule per backend, stated in this header
 * and already the one the backends build their root signatures and
 * argument encoders by from the caller's set layouts; the caller's
 * compiler follows it. Clip space becomes +Y up on every backend, which
 * D3D12 and Metal are natively and Vulkan reaches by a negative viewport
 * height, so no shader is flipped anywhere.
 *
 * ---------------------------------------------------------------------------
 * What this decides that no card has (each becomes a card if overturned)
 * ---------------------------------------------------------------------------
 *
 *  F1. One format per backend: SPIR-V (Vulkan), DXIL (D3D12), a .metallib
 *      (Metal). A device reports the one it takes as a bit in
 *      CRGPU_ADAPTER_PROPERTIES::shader_formats, a mask so that a backend
 *      can take a second later without a new field. Metal source (Metal's
 *      newLibraryWithSource:, compiled by Metal at run time) is not added:
 *      no caller needs it until a runtime compiler (A9's ApShaderc) exists,
 *      and it is the one route on which Metal, not the caller, compiles.
 *  F2. A module has one entry point, named in its desc, required and copied
 *      (no "main" default: Metal reserves main, and Slang renames it
 *      main_0). Each backend checks the module has an entry point of that
 *      name for the desc's stage, the same check returning the same code:
 *      Vulkan by its OpEntryPoint and execution model, D3D12 by the DXIL
 *      container's PSV0 part (its EntryFunctionName and shader kind; DXC
 *      writes them, version 3, since 1.8), Metal by newFunctionWithName: and
 *      the function's type. The pipelines' seven *_entry_point fields and
 *      the compute pipeline's one go.
 *  F3. A compute, mesh or task module's desc gives its workgroup size,
 *      which Metal needs at each dispatch and can't read from a library
 *      (card S17). Each element is 1 or more for those stages, 0 for the
 *      others, checked on every backend. Whether it equals what the shader
 *      declares is the caller's to keep: Metal can't check it, so no
 *      backend does; a size that differs is undefined.
 *  F4. The binding layout each backend reads, as rules the module's
 *      compiler follows (card S17), stated once on the module desc below.
 *      No reflection is read: the root signature and argument encoders are
 *      built from the set layouts and push constant ranges, as now.
 *  F5. Clip space is +Y up on every backend (card S19): a shader writes the
 *      same position for every target. The Vulkan backend sets each
 *      viewport with y at its bottom edge and a negative height (core since
 *      Vulkan 1.1). Winding is decided in framebuffer coordinates on all
 *      three, so front_face_ccw means the same everywhere. Framebuffer
 *      coordinates (a fragment's position) are top-left on all three, as
 *      now.
 *  F6. A storage view of a cube is a 2D array view on every backend: D3D12
 *      has no RWTextureCube, and with DXIL in, no backend rewrites the
 *      shader's image type any more (crgpu_d3d12_storage_cubes_as_arrays
 *      goes). A view has no usage of its own, so the refusal is where it
 *      is bound: crgpu_device_update_descriptor_sets() (a void call) writes
 *      nothing at all when any of its writes puts a CUBE or CUBE_ARRAY
 *      view in a STORAGE_IMAGE descriptor, checked before the first write,
 *      the same on every backend. A shader declares the cube's storage
 *      binding as a 2D array image whose layer is face (+ 6 x cube), as it
 *      reads on D3D12 today.
 *  F7. The module copies its code: the caller's bytes are borrowed for the
 *      call. Vulkan's VkShaderModule copies; D3D12 keeps a copy on the
 *      device's allocator until the module is destroyed (a pipeline takes
 *      the bytecode at creation); Metal's library is made from a copy
 *      (dispatch_data with the default destructor).
 *
 * ---------------------------------------------------------------------------
 * What leaves each backend
 * ---------------------------------------------------------------------------
 *
 *  D3D12: SPIRV-Cross (spirv_hlsl), DXC (dxcompiler.lib, and dxcompiler.dll
 *    beside every application), the storage-cube SPIR-V rewrite, flip_vert_y,
 *    and SPIRV-Cross's root constant layout. CMakeLists.txt stops fetching
 *    SPIRV-Cross for D3D12 and stops linking dxcompiler.lib.
 *  Metal: SPIRV-Cross (its C API, crgpumetal___internal_cross_compile_spirv_to_msl),
 *    newLibraryWithSource:, the LocalSize reflection, the combined-sampler
 *    remap and flip. What they set becomes the binding layout below, kept
 *    by the caller's compiler (Slang's own Metal, or card S20's tool over
 *    SPIRV-Cross for the fallback route, which carries those exact options
 *    out of the backend).
 *  Vulkan: nothing leaves; it gains the format and entry point checks and
 *    the negative viewport (F5).
 *
 * ---------------------------------------------------------------------------
 * Every caller's change (one change, A9)
 * ---------------------------------------------------------------------------
 *
 *  carrottus: tests/crgpu_objects_test.c and its four shaders (.slang, built
 *    by cmake/cr_slang.cmake; tests/crgpu_test_shaders.h and
 *    tests/shaders/make_test_shaders.py go); the misuse harness regenerated.
 *  Every other caller of crgpu_shader_module_create(), and every shader
 *    and clip-space position above Carrottus (F5), in the same change: the
 *    workspace's ROADMAP.md lists them (section 2, this session's entry).
 *
 * ---------------------------------------------------------------------------
 * ABI breaks (abi.txt)
 * ---------------------------------------------------------------------------
 *
 *  - crgpu_shader_module_desc: spirv_code and spirv_size renamed and
 *    moved; format, entry_point and workgroup_size added.
 *  - crgpu_graphics_pipeline_desc: the seven *_entry_point fields removed.
 *  - crgpu_compute_pipeline_desc: compute_entry_point removed.
 *  - CRGPU_ADAPTER_PROPERTIES: shader_formats added.
 *  - CR_RESULT_GPU_NULL_SPIRV (304), CR_RESULT_GPU_INVALID_SPIRV_ALIGNMENT
 *    (305) and CR_RESULT_GPU_SHADER_COMPILATION_FAILED (307) renamed, their
 *    values kept; three results added.
 *  - Behaviour: clip space's Y (F5) and storage cube views (F6).
 *
 * ---------------------------------------------------------------------------
 * Tests (crgpu_objects_test unless named)
 * ---------------------------------------------------------------------------
 *
 *  - Every refusal of crgpu_shader_module_create, in parameter order, on
 *    every backend: a zero format; another backend's format
 *    (SHADER_FORMAT_NOT_SUPPORTED); NULL or empty code; a SPIR-V or DXIL
 *    size not a multiple of 4; a NULL entry point; a name the module lacks,
 *    and the right name for another stage (ENTRY_POINT_NOT_FOUND); a
 *    compute module with a 0 in its workgroup size, and a vertex module
 *    with a non-zero one (INVALID_WORKGROUP_SIZE); bytes that aren't the
 *    format (a SPIR-V magic word wrong, a DXIL container without its
 *    'DXBC'; INVALID_SHADER_CODE).
 *  - The drawn grid and compute dispatch the test already reads back, from
 *    the native module: the same values as before the change.
 *  - The adapter's shader_formats: exactly its backend's bit.
 *  - Clip space (F5): a triangle written at clip y = +0.5 lands in the
 *    image's upper half, read back, on every backend; a culled-back
 *    pipeline keeps the triangle facing the same way on all three.
 *  - Storage cubes (F6): a write of a CUBE view to a STORAGE_IMAGE
 *    descriptor writes nothing (the set's earlier contents read back); a
 *    2D array view of the cube's faces is written by a compute shader and
 *    read back.
 *  - cr_misuse_tests: the module desc's new fields zeroed.
 *  - The existing read-back suites of every caller (each shader against its
 *    GLSL predecessor's images).
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 *  Vulkan: VkShaderModuleCreateInfo (SPIR-V words, copied at creation;
 *    pName at pipeline creation, any name). D3D12: D3D12_SHADER_BYTECODE
 *    (DXIL bytes at pipeline creation; one entry per container, named in
 *    PSV0; register spaces as the root signature says). Metal:
 *    newLibraryWithData:error: and newFunctionWithName:; a kernel's
 *    threadsPerThreadgroup at dispatch, which nothing reflects. WebGPU
 *    takes WGSL and names an entry point per stage at pipeline creation;
 *    NVRHI, bgfx and Unreal's RHI take the platform's binary, each with a
 *    binding layout fixed by convention (NVRHI's register spaces, bgfx's
 *    shaderc remaps), as F4 does.
 */

#ifndef CRGPU_SHADERFORMAT_DRAFT_H
#define CRGPU_SHADERFORMAT_DRAFT_H

#include "include/crgpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * [ADDED] CRGPU_SHADER_FORMAT and CRGPU_SHADER_FORMATS.
 * Earns its place: a desc must say what its bytes are, and a caller must be
 * able to ask which format a device takes before building a package for it.
 * ------------------------------------------------------------------------- */

/**
 * @brief The encoding of a shader module's code.
 *
 * Each backend takes exactly one: SPIR-V on Vulkan, DXIL on D3D12, a
 * .metallib on Metal, reported per adapter in
 * CRGPU_ADAPTER_PROPERTIES::shader_formats. Carrottus GPU compiles and
 * translates none of them.
 */
typedef uint32_t CRGPU_SHADER_FORMAT;
enum {
	/** Not a format: a zeroed desc has not chosen, and is refused. */
	CRGPU_SHADER_FORMAT_NONE = 0,
	/** SPIR-V words (Vulkan). 1.0 to the device's Vulkan version's
	 * highest; a module using an extension or capability needs the device
	 * feature that enables it (a ray query, CRGPU_FEATURE_RAY_QUERY). */
	CRGPU_SHADER_FORMAT_SPIRV = 1,
	/** A DXIL container as DXC writes it, validated and signed (D3D12).
	 * Its shader model must be one the device runs. */
	CRGPU_SHADER_FORMAT_DXIL = 2,
	/** A .metallib: a Metal library as Apple's metallib tool writes it
	 * (Metal). Its Metal language version must be one the device runs. */
	CRGPU_SHADER_FORMAT_METALLIB = 3
};

/** @brief A set of CRGPU_SHADER_FORMAT values: bit (1 << format). */
typedef uint32_t CRGPU_SHADER_FORMATS;

/* ---------------------------------------------------------------------------
 * [CHANGED] CRGPU_ADAPTER_PROPERTIES gains one field, after
 * supported_features (the rest unchanged and not repeated here).
 * ------------------------------------------------------------------------- */
#if 0
	/** The shader formats a device of this adapter takes, as
	 * (1u << CRGPU_SHADER_FORMAT_*): exactly one bit, the backend's own. */
	CRGPU_SHADER_FORMATS shader_formats;          /* [ADDED] */
#endif

/* ---------------------------------------------------------------------------
 * [CHANGED] crgpu_shader_module_desc.
 * ------------------------------------------------------------------------- */

/**
 * @brief Describes a shader module: one entry point, in the device's own
 * shader format.
 *
 * **Where a module's bindings are.** Carrottus GPU builds each pipeline's binding
 * layout from its descriptor set layouts and push constant ranges, never
 * from the module, so the module must put each resource where its backend
 * looks. For a binding @c b of set @c s (crgpu_descriptor_set_layout_binding::binding,
 * and the set's index in the pipeline desc's descriptor_set_layouts):
 *
 * - **Vulkan (SPIR-V):** DescriptorSet @c s, Binding @c b. The push
 *   constants are the module's push constant block.
 * - **D3D12 (DXIL):** register @c b of space @c s, of the class its
 *   descriptor type takes: a uniform buffer @c b<b>; a sampled image @c t<b>;
 *   a storage buffer or storage image @c u<b> (never an SRV, even read
 *   only); a sampler @c s<b>; a combined image sampler both @c t<b> and
 *   @c s<b> (and, with CRGPU_EXT_RAY_QUERY, an acceleration structure
 *   @c t<b>). The push constants are a
 *   constant buffer at @c b0 of space CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS
 *   (4). A vertex input at location @c n has the semantic @c TEXCOORD<n>.
 * - **Metal (.metallib):** set @c s is an argument buffer (tier 1, members
 *   in binding order) at buffer index 26 + @c s, each binding at
 *   @c [[id(b)]], an array of @c n at @c b to @c b + @c n - 1, and a
 *   combined image sampler's sampler at @c [[id(1024 + b)]] (so no binding
 *   of such a set reaches 1024). The push constants are a constant buffer
 *   at buffer index 30. A vertex buffer binding @c v is at buffer index
 *   @c v, so vertex bindings stay below 26.
 *
 * In Slang, `[[vk::binding(b, s)]]` with `register(<class><b>, space<s>)`
 * on each resource gives the first two; the Metal layout is what Slang
 * writes from one ParameterBlock per set declared in set order with its
 * members in binding order, or what card S20's tool writes from SPIR-V.
 *
 * **Clip space** is +Y up on every backend: a vertex or mesh shader's
 * position (x, y) = (-1, +1) is the viewport's top left. The Vulkan backend
 * keeps it by viewport (crgpu_command_list_set_viewports()); no backend changes a
 * shader.
 *
 * **Stages' outputs and inputs** are matched by location on Vulkan and
 * Metal and by register in order on D3D12: declare every output from
 * location 0 up to the last one read (the rule crgpu_graphics_pipeline_create()
 * already states).
 */
typedef struct crgpu_shader_module_desc {
	/** `type` is CR_STRUCT_TYPE_GPU_SHADER_MODULE_DESC; `next` is NULL.
	 * Set by the caller. See CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** The stage the entry point is for. */
	CRGPU_SHADER_STAGE stage;
	/** [ADDED] What #code is: the format the device takes
	 * (CRGPU_ADAPTER_PROPERTIES::shader_formats). */
	CRGPU_SHADER_FORMAT format;
	/** [CHANGED: was spirv_code] The module's bytes, 4-byte aligned.
	 * Borrowed for the call: the module keeps a copy. */
	const void *code;
	/** [CHANGED: was spirv_size] Bytes in #code, not 0; a multiple of 4
	 * for SPIR-V and DXIL. */
	uint64_t code_size;
	/** [ADDED] The entry point's name, null-terminated: the module must
	 * have one of that name for #stage. Required. Copied. */
	const char *entry_point;
	/** [ADDED] The entry point's workgroup size (x, y, z), as the shader
	 * declares it: each 1 or more for CRGPU_SHADER_STAGE_COMPUTE, _MESH
	 * and _TASK, all 0 for every other stage. A size other than the
	 * shader's is undefined (Metal can't check it, so no backend does). */
	uint32_t workgroup_size[3];
	/** @brief A name for diagnostics, or NULL. Copied, in every build; see
	 * cr_debug_name_set(). */
	const char *debug_name;
} crgpu_shader_module_desc;

/**
 * @brief [CHANGED] Creates a shader module from code in the device's own
 * shader format. Translates and compiles nothing.
 *
 * Thread: @p device may be used by other calls meanwhile (a module is
 * created, not recorded); the new module is the caller's.
 *
 * @param[in]  device     Device to create the module on.
 * @param[in]  desc       The stage, format, code, entry point and workgroup
 *                        size.
 * @param[out] out_module Receives the module; untouched on failure.
 *
 * Checked in this order, on every backend:
 * @retval CR_SUCCESS The module was created.
 * @retval CR_RESULT_GPU_INVALID_DEVICE `device` is NULL (or, in Debug,
 *         destroyed).
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE / CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN
 *         `desc->base` is wrong.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_module` is NULL.
 * @retval CR_RESULT_GPU_INVALID_SHADER_STAGE `desc->stage` is not one
 *         known stage.
 * @retval CR_RESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED [ADDED] `desc->format`
 *         is not in the device's shader_formats (CRGPU_SHADER_FORMAT_NONE
 *         included).
 * @retval CR_RESULT_GPU_NULL_SHADER_CODE [CHANGED] `desc->code` is NULL or
 *         `desc->code_size` is 0.
 * @retval CR_RESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT [CHANGED]
 *         `desc->code` is not 4-byte aligned, or `desc->code_size` is not a
 *         multiple of 4 for SPIR-V or DXIL.
 * @retval CR_RESULT_GPU_NULL_ENTRY_POINT [ADDED] `desc->entry_point` is
 *         NULL or empty.
 * @retval CR_RESULT_GPU_INVALID_WORKGROUP_SIZE [ADDED] a compute, mesh or
 *         task stage with a 0 in `desc->workgroup_size`, or another stage
 *         with anything but 0.
 * @retval CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED a mesh or task
 *         stage on a device created without CRGPU_FEATURE_MESH_SHADING.
 * @retval CR_RESULT_GPU_INVALID_SHADER_CODE [CHANGED: was
 *         SHADER_COMPILATION_FAILED] the code isn't the format (SPIR-V's
 *         magic word, a DXIL container's 'DXBC' and its PSV0 part, a
 *         library Metal refuses).
 * @retval CR_RESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND [ADDED] the module has
 *         no entry point named `desc->entry_point` for `desc->stage`.
 * @retval CR_RESULT_OUT_OF_MEMORY the copy or the module couldn't be
 *         allocated.
 * @return CR_RESULT_API_SPECIFIC_FAILURE if the graphics API fails
 *         otherwise.
 */
CR_RESULT crgpu_shader_module_create(
    crgpu_device device,
    const crgpu_shader_module_desc *desc,
    crgpu_shader_module *out_module);

/* ---------------------------------------------------------------------------
 * [REMOVED] from crgpu_graphics_pipeline_desc: vertex_entry_point,
 * mesh_entry_point, task_entry_point, fragment_entry_point,
 * geometry_entry_point, tess_control_entry_point, tess_eval_entry_point.
 * [REMOVED] from crgpu_compute_pipeline_desc: compute_entry_point.
 * Why: a module has one entry point, named in its desc (F2); a second name
 * per pipeline could only disagree with it, and NULL meant "main", a
 * default that hides a choice.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * [CHANGED] crgpu_command_list_set_viewports(): its comment gains F5's rule.
 * ------------------------------------------------------------------------- */
#if 0
 * A viewport's (x, y) is its top-left corner in framebuffer coordinates,
 * which run right and down from the target's top left, on every backend;
 * clip space's +Y is toward the viewport's top (crgpu_shader_module_desc).
 * The Vulkan backend gives Vulkan y + height and -height to keep that.
#endif

/* ---------------------------------------------------------------------------
 * [CHANGED] crgpu_device_update_descriptor_sets() (F6): when any write puts a
 * view of type CRGPU_IMAGE_VIEW_TYPE_CUBE or _CUBE_ARRAY in a
 * CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE descriptor, the call writes nothing
 * (it is void; every write is checked before the first is made), on every
 * backend. A cube's faces are written through a
 * CRGPU_IMAGE_VIEW_TYPE_2D_ARRAY view of them.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * crcore.h, CR_RESULT
 * ------------------------------------------------------------------------- */
#if 0
	/** [CHANGED: was CR_RESULT_GPU_NULL_SPIRV] A shader module desc's code
	 * is NULL or 0 bytes long. */
	CR_RESULT_GPU_NULL_SHADER_CODE                            = 304,
	/** [CHANGED: was CR_RESULT_GPU_INVALID_SPIRV_ALIGNMENT] A shader
	 * module desc's code is not 4-byte aligned, or a SPIR-V or DXIL size
	 * is not a multiple of 4. */
	CR_RESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT               = 305,
	/** [CHANGED: was CR_RESULT_GPU_SHADER_COMPILATION_FAILED] A shader
	 * module's code isn't the format its desc names, or the graphics API
	 * refused it. */
	CR_RESULT_GPU_INVALID_SHADER_CODE                         = 307,
	/** [ADDED] A shader module desc's format is not one the device takes. */
	CR_RESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED                 = 308,
	/** [ADDED] A shader module has no entry point of the desc's name for
	 * its stage. */
	CR_RESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND                = 309,
	/** [ADDED] A shader module desc's entry point is NULL or empty. */
	CR_RESULT_GPU_NULL_ENTRY_POINT                            = 284,
	/** [ADDED] A shader module desc's workgroup size is 0 in a dimension
	 * for a compute, mesh or task stage, or not 0 for another. */
	CR_RESULT_GPU_INVALID_WORKGROUP_SIZE                      = 285,
#endif

#ifdef __cplusplus
}
#endif

#endif /* CRGPU_SHADERFORMAT_DRAFT_H */
