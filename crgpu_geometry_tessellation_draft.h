/*
 * crgpu_geometry_tessellation_draft.h - geometry and tessellation shaders
 * behind EXTs (ROADMAP.md, section 3, carrottus; found ready 2026-10-31). For
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
 * crgpu_graphics_pipeline_desc already has geometry_module,
 * tess_control_module, tess_eval_module and patch_control_points, and the
 * stages exist in CRGPU_SHADER_STAGE, but nothing says which device can
 * run them: Vulkan's geometryShader and tessellationShader are optional
 * features never enabled, D3D12 has both at every feature level Carrottus GPU
 * lists, and the Metal backend refuses both at pipeline creation (Metal has
 * no geometry stage, and its tessellation is a compute kernel and a
 * post-tessellation vertex function, not a translation of the other two's).
 * So each becomes an EXT with a feature bit and a refusal, as mesh shading
 * is, and the fields stay declared (no #if in a struct, as
 * depth_bounds_test_enable).
 *
 *  G1. CRGPU_EXT_GEOMETRY_SHADER and CRGPU_EXT_TESSELLATION are 1 on
 *      Vulkan and D3D12 and 0 on Metal. Metal's tessellation is a different
 *      model, not a gap one backend fills later under this name; if it is
 *      ever translated it is its own draft.
 *  G2. The bits are read when adapters are listed: Vulkan's geometryShader
 *      and tessellationShader; D3D12 always (feature level 12_0 has both);
 *      never on Metal. A device enables each only when created with it.
 *  G3. The refusal is where mesh shading's is, the same on every backend:
 *      crgpu_shader_module_create() refuses a geometry module, or a
 *      tessellation control or evaluation module, on a device created
 *      without the bit (Vulkan refuses such SPIR-V even as a module), after
 *      the mesh check; crgpu_graphics_pipeline_create() refuses a desc that
 *      names one, after the mesh check. With the EXT 0 (Metal) the device
 *      never has the bit, so the same refusal holds.
 *
 * ABI: additions only (two bits, two results, two macros).
 *
 * Tests (crgpu_objects_test): on a device created without each bit, a
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

/** [ADDED] The adapter runs geometry shaders (CRGPU_EXT_GEOMETRY_SHADER).
 * Vulkan: geometryShader. D3D12: always. Metal: never. */
#define CRGPU_FEATURE_GEOMETRY_SHADER (UINT64_C(1) << 14)
/** [ADDED] The adapter runs tessellation control and evaluation shaders
 * (CRGPU_EXT_TESSELLATION). Vulkan: tessellationShader. D3D12: always.
 * Metal: never. */
#define CRGPU_FEATURE_TESSELLATION (UINT64_C(1) << 15)

/* [ADDED] Compile-time: 1 where the backend implements the stage. */
#if CRGPU_COMPILE_VULKAN_API || CRGPU_COMPILE_D3D12_API
#define CRGPU_EXT_GEOMETRY_SHADER 1
#define CRGPU_EXT_TESSELLATION 1
#else
#define CRGPU_EXT_GEOMETRY_SHADER 0
#define CRGPU_EXT_TESSELLATION 0
#endif

/* [ADDED] crcore.h, the EXT block:
 *   CR_RESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED = 329,
 *   CR_RESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED    = 330.
 * (328 is CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED, approved.) */

/* [CHANGED] crgpu_shader_module_create(): a new @retval for each, after
 * CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED.
 * [CHANGED] crgpu_graphics_pipeline_create(): the same two, after its mesh
 * check; the geometry_module, tess_control_module, tess_eval_module and
 * patch_control_points comments name the EXTs and bits. */
