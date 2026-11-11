/*
 * crgpu_shaderbindings_draft.h - a module declares every binding of each
 * set it reads, and the Metal backend refuses one that doesn't (card S27,
 * option a, decided 2026-11-01 with the user; the workspace's DECISIONS.md,
 * "Decided").
 *
 * APPROVED 2026-11-01 by the user, as written here. To be built into crgpu.h
 * and crcore.h; where building finds this draft wrong or incomplete, a
 * card says so and this file is not edited to match.
 *
 * DRAFT, 2026-11-01. Not compilable: it repeats part of
 * crgpu_shader_module_desc's comment, one result and two calls' result
 * lists from crgpu.h and crcore.h. Every declaration below is marked
 * [ADDED], [CHANGED] or [REMOVED]; nothing unmarked changes. It adds to the
 * approved crgpu_shaderformat_draft.h (card S17's binding layout per
 * backend) and changes nothing that draft decided.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * The Metal backend writes a set's argument buffer from every binding of its
 * descriptor set layout, in binding order. Metal lays out a function's
 * argument buffer struct from the members the function declares, packed in
 * id order (measured 2026-11-01: a struct of [[id(5)]] and [[id(8)]] alone is
 * 16 bytes with id 5 at byte 0; the whole set's is 72 with id 5 at byte 40).
 * A module that leaves out a binding of a set it reads therefore reads the
 * wrong members, silently. Vulkan and D3D12 bind per binding, so a module
 * there may declare fewer bindings than its layout, and the same module is
 * right on two backends and wrong on the third.
 *
 * So the rule is stated for every backend, which keeps a caller's modules
 * portable, and checked where a backend can see it. Metal can: a pipeline's
 * reflection gives each set's argument buffer struct. Vulkan and D3D12
 * can't without reflecting the module, which Carrottus GPU doesn't do (it
 * translates nothing, card A9), and there a module that breaks the rule
 * still draws correctly.
 *
 * ---------------------------------------------------------------------------
 * What a caller's build does (not in a header)
 * ---------------------------------------------------------------------------
 *
 * Slang leaves out of its output every binding an entry point doesn't read,
 * and its -preserve-params keeps nothing in SPIR-V (2026.18.3). Its
 * reflection (-reflection-json) still lists every binding the source
 * declares, with its kind and a "used" flag (checked 2026-11-01 on the
 * three-binding shader in claude_made_decisions.md). carrottus's shader step
 * (cmake/cr_slang.cmake) passes that list to tools/crgpu_msl, which
 * moves to SPIRV-Cross's C++ API (MSLResourceBinding's basetype and count,
 * which the C API's spvc_msl_resource_binding_2 lacks) and pads each set's
 * argument buffer with pad_argument_buffer_resources. The step's arguments
 * don't change. A caller whose source declares each set whole (a shared
 * bindings module) gets modules that keep the rule; one that doesn't is
 * refused at pipeline creation on Metal.
 *
 * ---------------------------------------------------------------------------
 * crcore.h
 * ---------------------------------------------------------------------------
 */

/* [ADDED] The next free GPU value (310-330 are taken). */
/** A module's bindings for a set it reads aren't its descriptor set
 * layout's: it leaves out a binding of the layout, declares one the layout
 * doesn't hold, or declares one of another type or count
 * (crgpu_shader_module_desc, "Every binding of a set"). Returned only
 * where the backend can see the module's bindings: Metal. */
CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH = 331,

/*
 * ---------------------------------------------------------------------------
 * crgpu.h
 * ---------------------------------------------------------------------------
 */

/**
 * [CHANGED] crgpu_shader_module_desc's comment gains a paragraph after
 * "Where a module's bindings are" and its three backends' lists; the rest
 * is as it is.
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
 * (CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH). A compiler that leaves unread
 * bindings out of its output has them written back by the caller's build:
 * SPIRV-Cross pads a set's argument buffer from each binding's type and
 * count (`pad_argument_buffer_resources`).
 */
typedef struct crgpu_shader_module_desc crgpu_shader_module_desc;

/**
 * [CHANGED] crgpu_graphics_pipeline_create()'s result list gains, after
 * CR_RESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND's place in the order:
 *
 * @retval CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH On Metal, a module's
 *         argument buffer for a set it reads isn't that set's layout
 *         (crgpu_shader_module_desc, "Every binding of a set"): a binding
 *         of the layout left out, one it doesn't hold, or one of another
 *         type or count. Checked for each stage's module and each of its
 *         sets. Not returned by Vulkan or D3D12.
 */
CR_RESULT crgpu_graphics_pipeline_create(
    crgpu_device device,
    const crgpu_graphics_pipeline_desc *desc,
    crgpu_graphics_pipeline *out_pipeline);

/**
 * [CHANGED] crgpu_compute_pipeline_create()'s result list gains the same
 * line, for its one module.
 */
CR_RESULT crgpu_compute_pipeline_create(
    crgpu_device device,
    const crgpu_compute_pipeline_desc *desc,
    crgpu_compute_pipeline *out_pipeline);

/*
 * ---------------------------------------------------------------------------
 * ABI breaks
 * ---------------------------------------------------------------------------
 *
 * None: one enumerator appended to CR_RESULT, no struct or signature
 * changed (`check.sh abi` lists the addition). Behaviour: a Metal pipeline
 * whose module leaves out a binding of its set, created before and drawing
 * wrong, is now refused. Every caller rebuilds its .metallibs through the
 * padded step in the same change.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * carrottus's GPU tests, each through the tool's route, on a set layout of a
 * uniform buffer at 0, a sampled image at 5 and a sampler at 8:
 *   - a module declaring all three, reading 5 and 8: created, and its draw
 *     reads the image's texel (the case that read wrong before);
 *   - the same module built with the padding off (the step's old output):
 *     CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH on Metal, created on Vulkan and
 *     D3D12;
 *   - a module declaring a binding at 9 the layout lacks: refused on Metal;
 *   - a module declaring 5 as an array of 2 where the layout's count is 1:
 *     refused on Metal;
 *   - a compute pipeline, the same three refusals;
 *   - a module that reads no binding of a set the pipeline's layouts name:
 *     created (the rule is per set read).
 * The misuse harness gains nothing (no new call).
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 * Vulkan: a pipeline layout may hold more bindings than a module uses;
 *   every binding a module uses must be in it (VUID-...-layout-07988),
 *   checked by the validation layer, not the driver.
 * D3D12: a root signature may hold more than a shader uses; one a shader
 *   uses and the root signature lacks fails pipeline creation.
 * Metal: an argument buffer's layout is the function's (tier 1, and tier 2
 *   with an encoder); newArgumentEncoderWithBufferIndex: and the pipeline's
 *   reflection give it. MoltenVK meets the same gap by compiling MSL at
 *   pipeline creation with the layout in hand and padding
 *   (pad_argument_buffer_resources); Carrottus GPU compiles nothing (card A9), so
 *   the padding moves to the caller's build and the backend checks.
 * WebGPU: an explicit bind group layout may hold more than a shader uses;
 *   its Metal implementation binds per binding, not through argument
 *   buffers, so it never meets this.
 * Rejected: re-encoding a set per module shape inside Carrottus GPU (card S27,
 *   b: hidden memory and work, card S2), a keep-alive read in every shader
 *   (c), waiting for Slang (d; reported either way).
 */
