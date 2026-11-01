/*
 * spudgpu_geometry_tessellation_draft.h - geometry and tessellation shaders
 * behind EXTs (ROADMAP.md, section 3, spudlib; found ready 2026-10-31). For
 * review: prompts/ray_tracing.md, Phase 2.
 *
 * APPROVED 2026-10-31 by the user, as written here (the workspace's
 * DECISIONS.md, "Decided"). Built in Phase 4.
 *
 * DRAFT, 2026-10-31. Every declaration is marked [ADDED] or
 * [CHANGED]; nothing is removed.
 *
 * ---------------------------------------------------------------------------
 * The shape
 * ---------------------------------------------------------------------------
 *
 * spudgpu_shader_pipeline_desc already has geometry_module,
 * tess_control_module, tess_eval_module and patch_control_points, and the
 * stages exist in SPUDGPU_SHADER_STAGE, but nothing says which device can
 * run them: Vulkan's geometryShader and tessellationShader are optional
 * features never enabled, D3D12 has both at every feature level SpudGPU
 * lists, and the Metal backend refuses both at pipeline creation (Metal has
 * no geometry stage, and its tessellation is a compute kernel and a
 * post-tessellation vertex function, not a translation of the other two's).
 * So each becomes an EXT with a feature bit and a refusal, as mesh shading
 * is, and the fields stay declared (no #if in a struct, as
 * depth_bounds_test_enable).
 *
 *  G1. SPUDGPU_EXT_GEOMETRY_SHADER and SPUDGPU_EXT_TESSELLATION are 1 on
 *      Vulkan and D3D12 and 0 on Metal. Metal's tessellation is a different
 *      model, not a gap one backend fills later under this name; if it is
 *      ever translated it is its own draft.
 *  G2. The bits are read when adapters are listed: Vulkan's geometryShader
 *      and tessellationShader; D3D12 always (feature level 12_0 has both);
 *      never on Metal. A device enables each only when created with it.
 *  G3. The refusal is where mesh shading's is, the same on every backend:
 *      spudgpu_create_shader_module() refuses a geometry module, or a
 *      tessellation control or evaluation module, on a device created
 *      without the bit (Vulkan refuses such SPIR-V even as a module), after
 *      the mesh check; spudgpu_create_shader_pipeline() refuses a desc that
 *      names one, after the mesh check. With the EXT 0 (Metal) the device
 *      never has the bit, so the same refusal holds.
 *
 * ABI: additions only (two bits, two results, two macros).
 *
 * Tests (spudgpu_objects_test): on a device created without each bit, a
 * module of that stage and a pipeline naming one are refused with its
 * result, on both Windows backends; the bits reported per adapter are
 * printed and checked against the API's own (Vulkan's features, D3D12
 * always). A pipeline using either stage, drawn and read back, is not part
 * of this change: no caller uses them, and the samples have none (a sample
 * when a caller asks).
 *
 * Compared: Vulkan VkPhysicalDeviceFeatures::geometryShader and
 * ::tessellationShader; D3D12's feature levels; Metal's tessellation
 * (MTLRenderPipelineDescriptor tessellation properties, a compute kernel
 * writing tessellation factors) and its absence of geometry shaders.
 */

/** [ADDED] The adapter runs geometry shaders (SPUDGPU_EXT_GEOMETRY_SHADER).
 * Vulkan: geometryShader. D3D12: always. Metal: never. */
#define SPUDGPU_FEATURE_GEOMETRY_SHADER (UINT64_C(1) << 14)
/** [ADDED] The adapter runs tessellation control and evaluation shaders
 * (SPUDGPU_EXT_TESSELLATION). Vulkan: tessellationShader. D3D12: always.
 * Metal: never. */
#define SPUDGPU_FEATURE_TESSELLATION (UINT64_C(1) << 15)

/* [ADDED] Compile-time: 1 where the backend implements the stage. */
#if SPUDGPU_COMPILE_VULKAN_API || SPUDGPU_COMPILE_D3D12_API
#define SPUDGPU_EXT_GEOMETRY_SHADER 1
#define SPUDGPU_EXT_TESSELLATION 1
#else
#define SPUDGPU_EXT_GEOMETRY_SHADER 0
#define SPUDGPU_EXT_TESSELLATION 0
#endif

/* [ADDED] spudcore.h, the EXT block:
 *   SPUDRESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED = 329,
 *   SPUDRESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED    = 330.
 * (328 is SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED, approved.) */

/* [CHANGED] spudgpu_create_shader_module(): a new @retval for each, after
 * SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED.
 * [CHANGED] spudgpu_create_shader_pipeline(): the same two, after its mesh
 * check; the geometry_module, tess_control_module, tess_eval_module and
 * patch_control_points comments name the EXTs and bits. */
