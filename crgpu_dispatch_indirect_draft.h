/*
 * crgpu_dispatch_indirect_draft.h - Carrottus GPU's indirect dispatch: a
 * compute dispatch, and a mesh-shader dispatch where the device has mesh
 * shading, whose group counts the GPU reads from a buffer (card A15,
 * decided 2026-10-28: "indirect dispatch is taken, in Carrottus GPU first
 * ...; a draw count from the GPU and task shaders stay deferred").
 *
 * APPROVED 2026-11-18 with the user, every question as recommended (Q1 to
 * Q5); Q3's mesh-grid limit is read from Apple's feature set tables when
 * built, the smaller of it and 2^22.
 * DRAFT, 2026-11-18, version 1 (seven critique passes), for the user's
 * approval. Its compute half built on the Mac 2026-11-18; its mesh half
 * stopped (Apple9 needed for indirect mesh draws: card S54).
 *
 * Written unattended on the Mac (Apple M5 Pro, macOS 27) against
 * include/crgpu.h (crgpu_command_list_dispatch(), _dispatch_mesh(), the
 * draw_indirect family, CRGPU_BUFFER_USAGE_INDIRECT, the resource states
 * and stages, the shader module desc's workgroup_size), include/crcore.h,
 * src/gpu/crgpushared.c and .h, the three backends' command files
 * (crgpuvulkancommand.c, crgpud3d12command.cpp, crgpumetalcommand.m), their
 * device creation (crgpuvulkancontext.c, crgpud3d12context.cpp) and
 * internal headers, abi.txt, tests/crgpu_objects_test.c and the test
 * shader list in CMakeLists.txt; the Vulkan SDK 1.4.363.0's
 * validusage.json, the macOS 27.2 SDK's MTLComputeCommandEncoder.h,
 * MTLRenderCommandEncoder.h and MTLRenderPipeline.h, and Microsoft's
 * DirectX-Headers d3d12.h (the copy under .refs/.../nvrhi/thirdparty/,
 * read for D3D12's own declarations only, as crgpu_format_capabilities_draft.h
 * did), as read on 2026-11-18 (carrottus 0.4.0, untagged). Not built, not
 * in any target, not compilable: it repeats declarations of crgpu.h and
 * crcore.h to show them changed. Every declaration below is marked
 * [ADDED], [CHANGED] or [REMOVED]; nothing unmarked changes. Once approved
 * it is frozen (ROADMAP.md, section 1, "Work built from approved drafts").
 *
 * Result codes: S49's approved draft holds 371, S52's (crgpu_copy_rows_draft.h)
 * 372 to 374, and S51's was approved with its Q2 (a), taking none; so this
 * draft takes the next free, 375 to 378 (Q5).
 *
 * The caller's own command over these calls is not this draft (see "Left
 * out").
 *
 * ---------------------------------------------------------------------------
 * Revision notes (version 1 written, then seven passes, each fixed before
 * the next; one lens a pass, as the workspace's critique prompts run them)
 * ---------------------------------------------------------------------------
 *
 * Pass 1 (wrong, missing, more than needed).
 *  1. More than needed: version 1 had two argument structs, one per call.
 *     Their layouts are the same three uint32_t on every API
 *     (VkDispatchIndirectCommand and VkDrawMeshTasksIndirectCommandEXT;
 *     D3D12_DISPATCH_ARGUMENTS and D3D12_DISPATCH_MESH_ARGUMENTS;
 *     MTLDispatchThreadgroupsIndirectArguments for both of Metal's calls).
 *     One struct now, named for what it holds, as a GPU writer writes one
 *     layout for both.
 *  2. More than needed: version 1 gave the mesh call Vulkan's draw_count
 *     and stride (vkCmdDrawMeshTasksIndirectEXT takes them). One dispatch
 *     a call now, as for compute: several from one buffer, and a count the
 *     GPU writes, are the deferred "draw count from the GPU" (card A15).
 *  3. Missing: the range check's overflow. `offset + 12 > size` wraps for
 *     an offset near 2^64; written as two conditions, the first guarding
 *     the second (offset past the size, or fewer than 12 bytes after it).
 *  4. Missing: what a count of 0 does. All three APIs take a 0 in any
 *     dimension as no work (Vulkan's VkDispatchIndirectCommand has no rule
 *     against 0; D3D12's Dispatch documents it; Metal's indirect read has
 *     none). Stated: a 0 dispatches nothing, and tested on each backend
 *     (Metal's validation is the one to watch: tests, 4).
 *  5. Wrong: version 1 left Metal's threadgroup size as an open question
 *     between reflection and a parameter. It is not open: every compute,
 *     mesh and task module states its workgroup size at creation
 *     (crgpu_shader_module_desc::workgroup_size, crgpu.h line 5128, refused
 *     as 0) and Metal's direct dispatch already reads it from the bound
 *     pipeline (crgpumetalshader.m lines 450 and 621). Q2 keeps the
 *     alternatives on record with (a) recommended.
 *
 * Pass 2 (the platforms and industry practice).
 *  1. Vulkan (validusage.json): vkCmdDispatchIndirect's buffer-02709
 *     (INDIRECT_BUFFER_BIT), offset-02710 (a multiple of 4) and offset-00407
 *     (offset + sizeof(VkDispatchIndirectCommand) within the buffer), and
 *     outside a render pass (the command's scope: compute, outside render
 *     pass instances); VkDispatchIndirectCommand-x-00417, -y-00418,
 *     -z-00419 (each at most maxComputeWorkGroupCount). For
 *     vkCmdDrawMeshTasksIndirectEXT the same three, -renderpass (inside a
 *     render pass instance), and VkDrawMeshTasksIndirectCommandEXT-TaskEXT-
 *     07326 to -07329 without a task shader (each count at most
 *     maxMeshWorkGroupCount, their product at most
 *     maxMeshWorkGroupTotalCount). Vulkan's required minimums: 65535 each
 *     for maxComputeWorkGroupCount and maxMeshWorkGroupCount, 2^22 for
 *     maxMeshWorkGroupTotalCount. Added the product rule to the mesh call
 *     (version 1 had the per-dimension one only).
 *  2. D3D12 (d3d12.h): D3D12_DISPATCH_ARGUMENTS (line 4640) and
 *     D3D12_DISPATCH_MESH_ARGUMENTS (line 31593), the argument types
 *     DISPATCH (line 4666) and DISPATCH_MESH (line 4674), and
 *     D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION, 65535 (line 825),
 *     a fixed limit with no query. ExecuteIndirect's ArgumentBufferOffset
 *     a multiple of 4 and the arguments within the buffer (Microsoft's
 *     documentation of ExecuteIndirect, as remembered; not on this
 *     machine). DispatchMesh's limits (each at most 65535, the product at
 *     most 2^22) from Microsoft's mesh shader specification, as
 *     remembered.
 *  3. Metal (macOS 27.2 SDK): dispatchThreadgroupsWithIndirectBuffer:
 *     indirectBufferOffset:threadsPerThreadgroup: (MTLComputeCommandEncoder.h
 *     line 236) and drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:
 *     threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:
 *     (MTLRenderCommandEncoder.h line 695), both "indirectBufferOffset must
 *     be a multiple of 4", both reading MTLDispatchThreadgroupsIndirectArguments
 *     (MTLComputeCommandEncoder.h line 29). The threadgroup's size is the
 *     call's, never read from the buffer. A mesh grid's own limit
 *     (Apple's Metal feature set tables, "Maximum threadgroups per mesh
 *     grid") is not in the SDK's headers (MTLRenderPipeline.h line 475
 *     reports only an object shader's): Q3.
 *  4. WebGPU: dispatchWorkgroupsIndirect(indirectBuffer, indirectOffset),
 *     the buffer with INDIRECT usage, the offset a multiple of 4 and 12
 *     bytes within the buffer, each refused at encoding; a count above
 *     maxComputeWorkgroupsPerDimension (65535 by default) makes the
 *     dispatch do nothing, which its implementations keep by a validation
 *     pass of their own over the arguments (Dawn's and wgpu's, as
 *     remembered of each project, not read on this machine). Added as Q3
 *     (c), and not recommended: a pass Carrottus GPU would add and a
 *     choice (to do nothing) it would make for the caller.
 *  5. Industry: SDL_GPU (SDL_DispatchGPUComputeIndirect(pass, buffer,
 *     offset), SDL_GPUIndirectDispatchCommand of three counts, the
 *     threadgroup size given once at the compute pipeline's creation, as
 *     Q2 (a) does); NVRHI (ICommandList::dispatchIndirect(offsetBytes),
 *     the buffer named in its ComputeState; no mesh indirect); bgfx
 *     (bgfx::dispatch(view, program, indirectHandle, start, num), several
 *     dispatches from a fixed-stride buffer); sokol_gfx (compute, no
 *     indirect dispatch). As remembered of each project's public API; none
 *     is on this machine. The common shape is one dispatch, a buffer and
 *     an offset: kept.
 *
 * Pass 3 (the C API, CONVENTIONS.md).
 *  1. Wrong in version 1: both calls were void, copying the draw_indirect
 *     family's shape, where a bad call "records nothing" and says nothing.
 *     CONVENTIONS.md, "Data handling": "A failed check returns its code
 *     and records nothing". Both return CR_RESULT now, as the copies do
 *     since S48; the void draw calls are a card ("Left out").
 *  2. Names follow the neighbours: crgpu_command_list_dispatch_indirect()
 *     beside _dispatch(), _dispatch_mesh_indirect() beside
 *     _dispatch_mesh(), inside #if CRGPU_EXT_MESH_SHADING as its neighbour
 *     is. The struct crgpu_dispatch_indirect_args, as
 *     crgpu_draw_indirect_args; frozen, its line written.
 *  3. Each @retval in the order the checks run, the general codes first
 *     where one fits (210, 221, 326); the four new ones named for what
 *     they say, not for these calls, so a later call may return them
 *     (Q5).
 *  4. Parameter order: cmd, buffer, offset, as the draws'; no out_.
 *
 * Pass 4 (no hidden policy).
 *  1. Nothing is clamped, zeroed or skipped on the GPU: a count past the
 *     limit is the caller's to avoid (Q3 a), not one Carrottus GPU
 *     rewrites (Q3 c).
 *  2. No barrier is implied: the caller moves the buffer to
 *     CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT after the writes, as for the
 *     draws.
 *  3. Wrong in version 1: Metal's indirect dispatch, finding its compute
 *     encoder ended (by a copy, a pass or a debug label), opened one and
 *     set the bound pipeline again. That hid a loss the other state (the
 *     sets, the push constants) still suffered, and was a fix in one
 *     backend for a rule crgpu.h doesn't state. Removed; the rule is Q4.
 *  4. Metal's direct dispatch turns a 0 workgroup size into 1
 *     (crgpumetalcommand.m lines 929 to 932, the mesh one 960 to 963): a
 *     default that can't fire, since module creation refuses a 0. The new
 *     calls don't copy it; the dead clamp is noted for removal when this
 *     is built ("Found in the code").
 *  5. D3D12's command signatures are mechanism, not a choice: one fixed
 *     object per argument kind, made with the device.
 *
 * Pass 5 (threads and errors).
 *  1. No thread rule changes. The D3D12 signatures are made by
 *     crgpu_device_create() (the tightly packed draw ones' place,
 *     carrottus/CLAUDE.md, "Check the handle"), so recording reads them
 *     with no lock; the mesh one only for a device created with
 *     CRGPU_FEATURE_MESH_SHADING, read only after that feature's check.
 *  2. A failed signature at device creation fails the device with
 *     CR_RESULT_API_SPECIFIC_FAILURE, as the draw signatures' do
 *     (crgpud3d12context.cpp lines 589 to 595); its HRESULT goes to the
 *     instance's message callback once card S10 is built.
 *  3. Vulkan: vkCmdDrawMeshTasksIndirectEXT is loaded at device creation
 *     beside vkCmdDrawMeshTasksEXT, and (found here) neither pointer is
 *     checked: a device made with mesh shading whose loader returns null
 *     is a half-built handle. Both checked now, the device refused with
 *     CR_RESULT_API_SPECIFIC_FAILURE.
 *  4. Every refusal is Carrottus's own check before any platform call:
 *     nothing is recorded, nothing reaches the message callback, and each
 *     check is its own `if` but the range's, whose two conditions return
 *     the same code. A Debug build refuses a destroyed cmd or buffer as a
 *     null one (CR_STALE), each its own `if`, in parameter order.
 *  5. No allocation: no backend makes anything at record time (Metal's
 *     calls allocate nothing of ours; cr_noalloc_tests holds it, tests).
 *
 * Pass 6 (the code as built; every file cited re-read, line numbers as of
 * 2026-11-18).
 *  1. crgpu.h: CRGPU_BUFFER_USAGE_INDIRECT line 1993 (comment 1982 to
 *     1992); CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT line 3330;
 *     CRGPU_PIPELINE_STAGE_DRAW_INDIRECT line 3357; crgpu_draw_indirect_args
 *     line 5776; crgpu_command_list_draw_indirect() line 5839;
 *     CRGPU_MESH_SHADING_LIMITS line 5939; crgpu_command_list_dispatch_mesh()
 *     line 5976; crgpu_command_list_dispatch() line 7121;
 *     crgpu_shader_module_desc::workgroup_size line 5128.
 *  2. crcore.h: 210 (line 216), 221 (245), 326 (415), 370 (477, the last
 *     value written); abi.txt: CR_RESULT_GPU_IMAGE_TOO_LARGE line 623,
 *     crgpu_command_list_dispatch line 908, _dispatch_mesh 909,
 *     _draw_indirect 914, crgpu_draw_indirect_args 1655.
 *  3. The list's type and pass, as the ray tracing calls read them
 *     (crgpu___internal_check_acceleration_structure_list(), crgpushared.c
 *     line 669): Vulkan cmd->_allocator._type and cmd->_rendering_active
 *     (crgpuvulkan.h line 280), D3D12 cmd->_allocator->_type and
 *     cmd->_rendering_active (crgpud3d12.hpp line 385), Metal
 *     _parent_allocator->_desc.type and _active_render_encoder != nil
 *     (crgpumetalraytracing.m line 469). The buffer's desc as the copies
 *     read it (&buffer->_desc on each backend; crgpuvulkanrenderpass.c line
 *     358, crgpud3d12renderpass.cpp line 379, crgpumetalrenderpass.m line
 *     191).
 *  4. Vulkan: dispatch line 687, dispatch_mesh 736 (its PFN on the device,
 *     crgpuvulkan.h line 138, loaded at crgpuvulkancontext.c lines 1143 to
 *     1145), the state maps 797 and 833 (INDIRECT_ARGUMENT is
 *     INDIRECT_COMMAND_READ at DRAW_INDIRECT, which Vulkan defines as the
 *     stage where indirect dispatch arguments are read too: no change).
 *     D3D12: dispatch line 431, dispatch_mesh 444, the signature maker 484
 *     (its comment already names DISPATCH), the draws' ExecuteIndirect 564
 *     and 588, the device's tight signatures crgpud3d12.hpp lines 286 and
 *     287, made at crgpud3d12context.cpp line 590 and released at 509.
 *     Metal: bind_compute_pipeline line 884 (_bound_compute_pipeline set at
 *     896), dispatch 907, dispatch_mesh 940, draw_indirect 1005, the
 *     compute encoder ended at 405.
 *  5. Found (each in "Found in the code" below): Metal's compute state lost
 *     at every ended encoder; Metal's bound compute pipeline never cleared;
 *     D3D12's one pipeline-state slot; the draws' unchecked usage,
 *     alignment and range; Vulkan's barrier stages on a compute-only queue;
 *     the dead clamp; dispatch_mesh's promised assert; Vulkan's unchecked
 *     mesh PFN.
 *  6. Version 1's line numbers corrected against the files: Metal's
 *     dispatch (907, not 900), D3D12's signature getter (512), the Vulkan
 *     PFN load (1143 to 1145).
 *
 * Pass 7 (tests, records, the build).
 *  1. Wrong in version 1: the limit test dispatched 65536 groups, past the
 *     contract and so undefined, with an expected count. Now 65535, the
 *     largest the contract allows, and nothing past it is tested.
 *  2. Wrong in version 1: the group-ID test wrote each group's ID to a
 *     slot and counted by reading the slots back, which can't tell a group
 *     run twice. An atomic counter beside the slots now, both checked.
 *  3. Missing: the mesh test needs a pass with a target and a mesh shader
 *     that emits no primitive (it writes its group ID and sets 0 vertices
 *     and 0 primitives); its shader is added to CR_TEST_SHADERS
 *     (ms_6_5, spvMeshShadingEXT) beside objects_draw_mesh.
 *  4. Missing: the misuse harness (regenerated from abi.txt, two calls
 *     added), cr_noalloc_tests (a scenario), and the records to write when
 *     built.
 *  5. The machines named: Metal built and run here; Vulkan type-checked
 *     here against the SDK's headers; D3D12 written, not compiled.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * A compute pass whose size the GPU decides (how many particles are alive
 * after the last step, how many lights fell in a tile, how many items a
 * cull kept) has its count in a buffer. With direct dispatch only, the
 * caller reads it back a frame late and dispatches a guess, or dispatches
 * the most it could ever be with every group past the count returning at
 * once. All three APIs read the counts from a buffer themselves; Carrottus
 * GPU has no call for it. Card A15 took it. A mesh shader's group count is
 * the same thing inside a pass (meshlets kept by a cull), with the same
 * three counts, so the mesh call comes with it (Q1), on the devices that
 * have mesh shading.
 *
 * ---------------------------------------------------------------------------
 * What changes, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * [ADDED] crgpu_command_list_dispatch_indirect(cmd, buffer, offset) records
 * one compute dispatch whose three group counts the GPU reads, when it
 * runs, from a crgpu_dispatch_indirect_args at `offset` in `buffer`; and
 * crgpu_command_list_dispatch_mesh_indirect(cmd, buffer, offset), inside
 * CRGPU_EXT_MESH_SHADING, records one mesh-shader dispatch from the same
 * struct (Q1). Each refuses, before recording anything, on every backend in
 * the same order: a null or destroyed list or buffer; (mesh) a device
 * without CRGPU_FEATURE_MESH_SHADING; a list of a type that can't hold it;
 * a pass open (compute) or not open (mesh); a buffer without
 * CRGPU_BUFFER_USAGE_INDIRECT; an offset not a multiple of 4; 12 bytes not
 * within the buffer. The counts the GPU reads are the caller's to keep
 * within 65535 a dimension (mesh: and 2^22 in all) (Q3); the bound state a
 * dispatch reads is stated once for every dispatch (Q4). Four codes are
 * added (375 to 378, Q5). Vulkan: vkCmdDispatchIndirect,
 * vkCmdDrawMeshTasksIndirectEXT. D3D12: ExecuteIndirect with a DISPATCH or
 * DISPATCH_MESH signature made with the device. Metal:
 * dispatchThreadgroupsWithIndirectBuffer: and
 * drawMeshThreadgroupsWithIndirectBuffer:, the threadgroup size from the
 * bound pipeline's module (Q2).
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 *  - Vulkan (the SDK 1.4.363.0's validusage.json): pass 2, 1.
 *  - D3D12 (d3d12.h as named above; Microsoft's documentation of
 *    ExecuteIndirect, command signatures and DispatchMesh, as remembered):
 *    pass 2, 2.
 *  - Metal (macOS 27.2 SDK headers): pass 2, 3.
 *  - WebGPU (dispatchWorkgroupsIndirect), wgpu, NVRHI, bgfx, sokol_gfx,
 *    SDL_GPU: pass 2, 4 and 5.
 *
 * Hazel is not compared: Carrottus is not measured against an engine (the
 * workspace's CLAUDE.md, "External references").
 *
 * ---------------------------------------------------------------------------
 * What the caller decides, and what Carrottus GPU only reports
 * ---------------------------------------------------------------------------
 *
 * The caller decides what writes the counts (a dispatch, a copy, the CPU
 * through an UPLOAD buffer), where they are, the barrier that orders the
 * write before the read, and that each count is within the limit.
 * Carrottus GPU checks what it can see when the call is recorded and passes
 * the buffer and offset to the API; it never reads, clamps or rewrites the
 * counts.
 *
 * ---------------------------------------------------------------------------
 * Who else uses it
 * ---------------------------------------------------------------------------
 *
 *  - A caller's GPU-driven compute: a particle step whose dead and alive
 *    lists are counted on the GPU, each stage after the first (simulate,
 *    and a radix sort's count, scan and scatter) dispatched over the count
 *    the stage before wrote, a small dispatch turning a count into the
 *    next arguments.
 *  - GPU culling: a pass keeping instances or meshlets, then a compute or
 *    mesh dispatch over what it kept (the mesh call).
 *  - Tiled or clustered lighting, and any work list, sized by a count of
 *    tiles a previous pass found busy.
 *
 * ---------------------------------------------------------------------------
 * Left out, and why
 * ---------------------------------------------------------------------------
 *
 *  - The caller's own command over these calls: the caller's design, after
 *    this one is approved and built.
 *  - A count read from the GPU (several dispatches or draws from one
 *    buffer, their number in another): deferred by card A15. So is a task
 *    stage (Metal's object function): the mesh call dispatches mesh
 *    groups only.
 *  - Several dispatches a call (bgfx's num, Vulkan's mesh draw_count):
 *    the same deferred family.
 *  - Checking the counts on the CPU for crgpu_command_list_dispatch() and
 *    _dispatch_mesh(), both void today: a card (Q3 states their rule).
 *  - The draw_indirect family's missing checks (usage, offset, range) and
 *    its void shape: a card ("Found in the code", 4).
 *  - A check of the bound state a dispatch or draw reads: a card (Q4).
 *  - An adapter limit for the group counts: Q3 (b), not recommended.
 *
 * ---------------------------------------------------------------------------
 * Found in the code (each to be carded when the user takes this draft)
 * ---------------------------------------------------------------------------
 *
 *  1. Metal ends its compute encoder at a copy or blit, a debug label,
 *     crgpu_command_list_begin_rendering() and an acceleration structure
 *     build or copy (crgpumetalcommand.m lines 359, 380 and 396,
 *     crgpumetalrenderpass.m lines 122 and 389, crgpumetalraytracing.m line
 *     214), and with it the encoder's pipeline state and sets (line 405 on).
 *     A dispatch after one of them without binding again records nothing
 *     (its `if (!..._active_compute_encoder ...) return;`, line 920), where
 *     Vulkan keeps the pipeline and sets bound. crgpu.h states no rule for
 *     it: Q4 writes one.
 *  2. Metal's _bound_compute_pipeline is set at a bind (line 896) and
 *     never cleared, not at crgpu_command_list_begin() (line 319): a list
 *     recorded again keeps the last recording's pipeline, possibly
 *     destroyed since.
 *  3. D3D12 has one pipeline-state slot for both kinds:
 *     crgpu_command_list_bind_graphics_pipeline() and
 *     _bind_compute_pipeline() both call SetPipelineState
 *     (crgpud3d12shader.cpp lines 913 and 932), so a dispatch after a
 *     graphics bind runs with a graphics state. Vulkan keeps one per bind
 *     point. Q4's rule covers it.
 *  4. The indirect draws check neither CRGPU_BUFFER_USAGE_INDIRECT, nor the
 *     offset's multiple of 4, nor the entries' range, on any backend
 *     (crgpuvulkancommand.c lines 697 to 733, crgpud3d12command.cpp 544 to
 *     590, crgpumetalcommand.m 1005 to 1093), against Vulkan's buffer-02709,
 *     offset-02710 and the range VUIDs: a bad call reaches the driver.
 *  5. Vulkan's barrier stages for UNORDERED_ACCESS, SHADER_RESOURCE and
 *     VERTEX_BUFFER name the vertex and fragment shader stages
 *     (crgpuvulkancommand.c lines 821 to 832), which a compute-only queue
 *     family doesn't support (a barrier's stage masks hold only the
 *     queue's stages). To be checked against the families a COMPUTE list
 *     gets; it bears on the barrier before an indirect dispatch on a
 *     compute queue.
 *  6. Metal's 0-to-1 workgroup clamp (pass 4, 4): dead since module
 *     creation refuses a 0; to go.
 *  7. crgpu_command_list_dispatch_mesh()'s comment promises a Debug assert
 *     on a device without mesh shading (crgpu.h line 5960): CONVENTIONS.md
 *     allows no assert on the caller's input.
 *  8. Vulkan's mesh PFN unchecked at device creation (pass 5, 3): fixed by
 *     this draft's build, since the new PFN is loaded on the same lines.
 *
 * ---------------------------------------------------------------------------
 * The version and the breaks
 * ---------------------------------------------------------------------------
 *
 *  - crgpu.h: two calls and one frozen struct added; comments of
 *    CRGPU_BUFFER_USAGE_INDIRECT, CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT,
 *    CRGPU_PIPELINE_STAGE_DRAW_INDIRECT, crgpu_command_list_dispatch() and
 *    _dispatch_mesh() changed (Q3, Q4).
 *  - crcore.h: four values added (375 to 378); 210's comment widened.
 *  - Behaviour: none of an existing call changes. The rules Q3 and Q4
 *    write onto the direct dispatches state what was already outside one
 *    API's rules (D3D12's 65535, D3D12's shared slot, Metal's ended
 *    encoder); no call starts refusing.
 *  - Device creation: D3D12 makes one more signature (two with mesh
 *    shading), Vulkan loads one more PFN with mesh shading; each failure
 *    is CR_RESULT_API_SPECIFIC_FAILURE, already in the create's list.
 *  - No version raise: carrottus' 0.4.0 is untagged (card X2).
 *    ./check.sh --update-abi lists the additions; the misuse generator is
 *    rerun.
 *
 * New handles: none. New struct types: one, frozen, not based. Struct
 * layouts: unchanged. Stored formats touched: none. Thread: nothing
 * changes. Allocation: none added.
 *
 * ---------------------------------------------------------------------------
 * Questions (each with options and a recommendation)
 * ---------------------------------------------------------------------------
 *
 * Q1. MESH-SHADER INDIRECT DISPATCH IN THIS DRAFT.
 *     (a) RECOMMENDED. In, beside the compute call, inside
 *         CRGPU_EXT_MESH_SHADING: one dispatch of mesh groups (no task
 *         stage), on a DIRECT list inside a pass (bundles refused, Q1 is
 *         not where bundles' rules grow), refused on a device without
 *         CRGPU_FEATURE_MESH_SHADING. Card A15's entry in
 *         CRGPU_DEVICE_TODO.md names it ("and mesh-shader dispatch where
 *         the device has mesh shading"), the struct is the same, and each
 *         API has the call; testable here (the M5 Pro has mesh shading).
 *     (b) Compute only, mesh indirect a later draft when a caller draws
 *         meshlets culled on the GPU. Smaller; leaves the card's entry
 *         half done, and the first caller waiting on this uses compute
 *         only.
 *
 * Q2. METAL'S THREADGROUP SIZE (Metal takes it at the call; Vulkan and
 *     D3D12 from the pipeline).
 *     (a) RECOMMENDED. From the bound pipeline's module, its
 *         crgpu_shader_module_desc::workgroup_size, as Metal's direct
 *         dispatch and dispatch_mesh do (pass 1, 5). No public change, and
 *         SDL_GPU's shape.
 *     (b) A parameter of the call: a value only Metal reads, which
 *         CONVENTIONS.md ("a flag only one platform acts on") rules out,
 *         and one that could disagree with the shader.
 *     (c) Reflection of the Metal function: Metal reports no declared
 *         threadgroup size for a compute function (only an object
 *         shader's grid limit), so there is nothing to read.
 *
 * Q3. THE COUNTS THE GPU READS (each API leaves a count past its limit
 *     undefined, and no call can see one).
 *     (a) RECOMMENDED. One rule, the narrowest every API keeps: a compute
 *         dispatch's counts are each at most 65535 (D3D12's fixed limit,
 *         Vulkan's guaranteed minimum); a mesh dispatch's each at most
 *         65535 and their product at most 2^22 (D3D12's and Vulkan's
 *         minimum), or Apple's "Maximum threadgroups per mesh grid" if
 *         smaller (to be read from Apple's tables before approval; not in
 *         the SDK's headers). A 0 in any dispatches nothing. Past the rule
 *         is undefined on every backend. Written also onto
 *         crgpu_command_list_dispatch() and _dispatch_mesh() (comment
 *         only: their counts were always within D3D12's rule or outside
 *         it), their CPU check a card.
 *     (b) Adapter limits (max_compute_group_count[3], the mesh ones on
 *         CRGPU_MESH_SHADING_LIMITS): Vulkan's own, D3D12's 65535, Metal's
 *         table. More for Vulkan and Metal callers, a new layout to
 *         freeze, and a caller writing for every backend holds to 65535
 *         anyway.
 *     (c) WebGPU's: a dispatch past the limit does nothing, kept by a
 *         validation pass Carrottus GPU records before each dispatch. A
 *         hidden pass and a choice made for the caller; rejected.
 *
 * Q4. THE BOUND STATE A DISPATCH READS (found: Metal loses it at every
 *     ended encoder, D3D12 at a graphics bind; Vulkan keeps it).
 *     (a) RECOMMENDED. One rule written once, on every dispatch call,
 *         direct and indirect: a dispatch reads the compute pipeline,
 *         compute descriptor sets and compute push constants recorded
 *         since the last of crgpu_command_list_begin(), a rendering pass,
 *         a copy, blit or clear outside a pass, a debug label, an
 *         acceleration structure build, copy or size write, and a graphics
 *         pipeline bind; with any of them not recorded since, what the
 *         dispatch does is undefined (Metal records nothing). The same
 *         narrowest-contract form as the push constant rule. No check:
 *         checking it for these two calls alone would be a rule held by
 *         two calls of many; a card checks it for every dispatch and draw.
 *     (b) Each backend tracks whether a compute pipeline is bound and
 *         intact, clearing it at every event above, and both indirect
 *         calls refuse with CR_RESULT_GPU_INVALID_COMPUTE_PIPELINE (303,
 *         widened). Right in the end, but it touches every recording call
 *         of three backends, for two calls.
 *
 * Q5. THE CODES.
 *     (a) RECOMMENDED. Four, each named for what it says, so a later call
 *         (the draws' card) returns it too:
 *         CR_RESULT_GPU_COMMAND_INSIDE_RENDERING (375),
 *         _COMMAND_OUTSIDE_RENDERING (376), _INDIRECT_OFFSET_ALIGNMENT
 *         (377), _INDIRECT_RANGE (378); with 210 (list type), 221 (usage)
 *         and 326 (mesh feature) as they are. As the ray tracing calls
 *         split their alignment (364) and range (363).
 *     (b) None new: the pass state as 210 (widened to "a list type, or a
 *         list's state, the call can't take"), the offset and range as
 *         CR_RESULT_INDEX_OUT_OF_RANGE. Fewer codes; each would then say
 *         less than the fact, which carrottus/CLAUDE.md asks of a result.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * crgpu_objects_test, every backend, the first adapter and any with mesh
 * shading, each refusal followed by a submit and a read-back showing
 * nothing ran (the counter 0, the slots their sentinel 0xFFFFFFFF):
 *  1. Group IDs. A compute shader objects_dispatch_ids_comp (Slang,
 *     workgroup size 1 x 1 x 1) atomically adds 1 to word 0 of a storage
 *     buffer and writes, at word 1 + (z * Y + y) * X + x, the value
 *     (x | y << 8 | z << 16) + 1, X and Y pushed as constants. Arguments
 *     {3, 2, 2} in an UPLOAD buffer of usage INDIRECT written by the CPU:
 *     the counter 12; each of the 12 slots its own value (slot 1 is 1,
 *     slot 12 is (2 | 1 << 8 | 1 << 16) + 1 = 65795); slot 13 0xFFFFFFFF.
 *     The same shader dispatched directly with (3, 2, 2) into a second
 *     buffer: both buffers equal byte for byte.
 *  2. Counts the GPU wrote. A first dispatch (direct, 1 group) of
 *     objects_dispatch_args_comp writes {4, 1, 1} into a DEVICE buffer of
 *     usage STORAGE | INDIRECT; a buffer barrier UNORDERED_ACCESS to
 *     INDIRECT_ARGUMENT; then the indirect dispatch of test 1's shader:
 *     the counter 4, slots 1 to 4 set, slot 5 0xFFFFFFFF. On a COMPUTE
 *     list too, where the device has a compute queue.
 *  3. The offset. A buffer of 64 bytes, {1000, 1000, 1000} at 0 and
 *     {2, 1, 1} at 16: offset 16 gives the counter 2. Offset 52 (the last
 *     12 bytes, holding {1, 1, 1}): the counter 1.
 *  4. Zeros. {0, 5, 5}, {5, 0, 5} and {5, 5, 0}: the counter 0 after each,
 *     every slot 0xFFFFFFFF, and no message from the validation or debug
 *     layer (Metal's validation with warnings fatal included; if it
 *     refuses a 0, Q3's "a 0 dispatches nothing" returns as a question).
 *  5. The limit. {65535, 1, 1} with objects_dispatch_count_comp (the
 *     counter only): the counter 65535. Nothing past the limit is run (it
 *     is undefined).
 *  6. Refusals, compute, each with its code: cmd NULL,
 *     CR_RESULT_GPU_INVALID_COMMAND_LIST; buffer NULL,
 *     CR_RESULT_GPU_INVALID_BUFFER; a COPY list,
 *     CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE (and a BUNDLE list where
 *     CRGPU_EXT_BUNDLES is 1); inside a pass,
 *     CR_RESULT_GPU_COMMAND_INSIDE_RENDERING; a buffer of usage STORAGE
 *     alone, CR_RESULT_GPU_INVALID_BUFFER_USAGE; offsets 2 and 6,
 *     CR_RESULT_GPU_INDIRECT_OFFSET_ALIGNMENT; with the 64-byte buffer,
 *     offsets 56, 64, 68 and UINT64_MAX - 3 (a multiple of 4),
 *     CR_RESULT_GPU_INDIRECT_RANGE.
 *  7. The order, two wrong at once: inside a pass with offset 2,
 *     INSIDE_RENDERING; usage missing with offset 2, INVALID_BUFFER_USAGE;
 *     offset 2 with offset + 12 past the end (offset 62),
 *     INDIRECT_OFFSET_ALIGNMENT; a COPY list with a buffer lacking the
 *     usage, INVALID_COMMAND_LIST_TYPE.
 *  8. Mesh, where the adapter has CRGPU_FEATURE_MESH_SHADING (the M5 Pro
 *     does; skipped, and said so, elsewhere): objects_dispatch_ids_mesh
 *     (mesh stage, 1 x 1 x 1, 0 vertices and 0 primitives) does test 1's
 *     writes inside a pass of one 4 x 4 target; arguments {3, 2, 1}: the
 *     counter 6, slots 1 to 6 set. Refusals: a device created without the
 *     bit, CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED (before the list
 *     type: tested on a COMPUTE list); a COMPUTE list,
 *     INVALID_COMMAND_LIST_TYPE; outside a pass,
 *     CR_RESULT_GPU_COMMAND_OUTSIDE_RENDERING; usage, offset and range as
 *     test 6.
 *  9. cr_misuse_tests: regenerated from abi.txt (the two calls with zeroes
 *     and, in Debug, each handle destroyed: the null check's code).
 * 10. cr_noalloc_tests: a scenario of test 2's frame (a direct dispatch,
 *     a barrier, an indirect dispatch, submitted and waited) held to no
 *     allocation of ours.
 * The three shaders are added to CR_TEST_SHADERS (cs_6_0, and ms_6_5 with
 * spvMeshShadingEXT for the mesh one).
 * Where each runs: the Mac (Apple M5 Pro; macos-metal Debug and Release
 * under Metal API and shader validation with warnings fatal, -asan,
 * -tsan). Vulkan's half type-checked here against the SDK's headers (as
 * S33's was); D3D12's written, not compiled. Owed on Windows 11 (Iris Xe,
 * RTX A2000 and WARP over D3D12 under the debug layer and GPU-based
 * validation; the Iris Xe and the A2000 over Vulkan under validation and
 * synchronization validation; the mesh tests on the A2000 only, the Iris
 * Xe having no mesh shading) and Fedora (ANV, NVK, RADV). ./check.sh:
 * abi, doxygen, conventions, struct-bases, caller-names.
 *
 * ---------------------------------------------------------------------------
 * crgpu.h
 * ---------------------------------------------------------------------------
 */

/**
 * [CHANGED] (comment only)
 * @brief Buffer holds crgpu_draw_indirect_args,
 * crgpu_draw_indexed_indirect_args or crgpu_dispatch_indirect_args read by
 * crgpu_command_list_draw_indirect(), _draw_indexed_indirect(),
 * _dispatch_indirect() and _dispatch_mesh_indirect(). The two dispatches
 * refuse a buffer without it (CR_RESULT_GPU_INVALID_BUFFER_USAGE).
 *
 * ... the rest unchanged (Vulkan's VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
 * nothing to set on D3D12 and Metal) ...
 */
/* CRGPU_BUFFER_USAGE_INDIRECT = 1 << 7 (value unchanged) */

/*
 * [CHANGED] (comments only) in CRGPU_RESOURCE_STATE and CRGPU_PIPELINE_STAGE:
 *
 *	CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT, /**< Read by an indirect draw or
 *	    dispatch (crgpu_command_list_draw_indirect(), _draw_indexed_indirect(),
 *	    _dispatch_indirect(), _dispatch_mesh_indirect()) * /
 *	CRGPU_PIPELINE_STAGE_DRAW_INDIRECT = 0x00000002, /**< Where indirect draw
 *	    and dispatch arguments are read * /
 */

/**
 * [ADDED]
 * @brief The three group counts an indirect dispatch reads
 * (crgpu_command_list_dispatch_indirect(),
 * crgpu_command_list_dispatch_mesh_indirect()).
 *
 * Bit for bit VkDispatchIndirectCommand and
 * VkDrawMeshTasksIndirectCommandEXT, D3D12_DISPATCH_ARGUMENTS and
 * D3D12_DISPATCH_MESH_ARGUMENTS, and MTLDispatchThreadgroupsIndirectArguments:
 * a shader writes these 12 bytes and any backend reads them as they are.
 * Counts in workgroups, as crgpu_command_list_dispatch() takes them. Each
 * call states the most a count may be; a 0 in any dispatches nothing.
 * Why: the layout the GPU writes, named once for both calls.
 */
/* Frozen: read by the GPU, so its layout is the GPU's. It never changes; a new need gets a new type. */
typedef struct crgpu_dispatch_indirect_args {
	/** Workgroups in X. */
	uint32_t group_count_x;
	/** Workgroups in Y. */
	uint32_t group_count_y;
	/** Workgroups in Z. */
	uint32_t group_count_z;
} crgpu_dispatch_indirect_args;

/**
 * @brief Records a compute dispatch against the bound compute pipeline.
 *
 * ... the mapping and units unchanged ...
 *
 * [CHANGED] (comment only; Q3 a, Q4 a) Two rules hold on every backend,
 * whatever the graphics API underneath keeps:
 *
 * - Each group count is at most 65535. What a dispatch with a larger one
 *   does is undefined. A 0 in any dispatches nothing.
 * - A dispatch reads the compute pipeline, the compute descriptor sets and
 *   the compute push constants recorded since the last of these:
 *   crgpu_command_list_begin(), a rendering pass
 *   (crgpu_command_list_begin_rendering() to _end_rendering()), a copy,
 *   blit or clear outside a pass, a debug label, an acceleration structure
 *   build, copy or compacted-size write, and
 *   crgpu_command_list_bind_graphics_pipeline(). With any of them not
 *   recorded since, what the dispatch does is undefined: bind them again.
 *
 * ... parameters unchanged ...
 */
void crgpu_command_list_dispatch(
    crgpu_command_list cmd,
    uint32_t group_count_x,
    uint32_t group_count_y,
    uint32_t group_count_z);

/**
 * [ADDED]
 * @brief Records a compute dispatch against the bound compute pipeline
 * whose group counts the GPU reads, when the dispatch runs, from a
 * crgpu_dispatch_indirect_args in a buffer.
 *
 * Why: a count the GPU decides (a previous dispatch's output) dispatched
 * with no read-back (card A15).
 *
 * The rules of crgpu_command_list_dispatch() hold for the counts read
 * (each at most 65535, a 0 dispatching nothing) and for the bound state.
 * The caller writes the arguments before the dispatch runs and moves the
 * buffer to CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT
 * (crgpu_command_list_pipeline_barrier()) after a GPU write; a buffer of
 * CRGPU_MEMORY_KIND_UPLOAD is readable so from creation. The threadgroup's
 * size is the bound pipeline's compute module's
 * (crgpu_shader_module_desc::workgroup_size), as for a direct dispatch.
 *
 * Maps to: vkCmdDispatchIndirect (Vulkan); ExecuteIndirect with a command
 * signature of one D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, made with the
 * device (D3D12); dispatchThreadgroupsWithIndirectBuffer:
 * indirectBufferOffset:threadsPerThreadgroup: (Metal).
 *
 * @param[in] cmd    Command list being recorded: of a DIRECT or COMPUTE
 *                   allocator, outside a rendering pass.
 * @param[in] buffer The buffer holding the arguments, created with
 *                   CRGPU_BUFFER_USAGE_INDIRECT. Borrowed: it lives until
 *                   the list's submission has finished.
 * @param[in] offset Byte offset of the arguments: a multiple of 4, with the
 *                   12 bytes from it within the buffer's size.
 *
 * Checked in this order, on every backend; a refused call records nothing:
 * @retval CR_SUCCESS The dispatch was recorded.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST `cmd` is NULL (or, in Debug,
 *         destroyed).
 * @retval CR_RESULT_GPU_INVALID_BUFFER `buffer` is NULL (or, in Debug,
 *         destroyed).
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE `cmd` is of a COPY or
 *         BUNDLE allocator.
 * @retval CR_RESULT_GPU_COMMAND_INSIDE_RENDERING A rendering pass is open
 *         on `cmd`.
 * @retval CR_RESULT_GPU_INVALID_BUFFER_USAGE `buffer` wasn't created with
 *         CRGPU_BUFFER_USAGE_INDIRECT.
 * @retval CR_RESULT_GPU_INDIRECT_OFFSET_ALIGNMENT `offset` is not a
 *         multiple of 4.
 * @retval CR_RESULT_GPU_INDIRECT_RANGE `offset` is past the buffer's size,
 *         or fewer than 12 bytes of it follow `offset`.
 */
CR_RESULT crgpu_command_list_dispatch_indirect(
    crgpu_command_list cmd,
    crgpu_buffer buffer,
    uint64_t offset);

#if CRGPU_EXT_MESH_SHADING

/**
 * @brief Records a mesh-shader dispatch against the bound mesh-shader
 * pipeline.
 *
 * ... unchanged, but: [CHANGED] (comment only) "(and a debug build asserts
 * then)" removed (found in the code, 7, carded); and (Q3 a, Q4 a) the
 * group counts are each at most 65535 and their product at most 4,194,304
 * (2^22) [or Apple's mesh grid limit, if smaller: Q3]; what a dispatch past
 * that does is undefined, and a 0 in any dispatches nothing. A mesh
 * dispatch reads the graphics pipeline and state bound in the pass, as a
 * draw does.
 */
void crgpu_command_list_dispatch_mesh(
    crgpu_command_list cmd,
    uint32_t group_count_x,
    uint32_t group_count_y,
    uint32_t group_count_z);

/**
 * [ADDED] (Q1 a)
 * @brief Records a mesh-shader dispatch against the bound mesh-shader
 * pipeline whose group counts the GPU reads, when it runs, from a
 * crgpu_dispatch_indirect_args in a buffer.
 *
 * Why: meshlets or groups kept by a cull on the GPU, drawn with no
 * read-back (card A15, "and mesh-shader dispatch where the device has mesh
 * shading").
 *
 * crgpu_command_list_dispatch_mesh()'s rules hold for the counts read
 * (each at most 65535, their product at most 2^22, a 0 dispatching
 * nothing) and for the bound state. Mesh groups only: no task stage runs
 * (task shaders are deferred). The arguments' state and lifetime as
 * crgpu_command_list_dispatch_indirect(). The mesh threadgroup's size is
 * the bound pipeline's mesh module's.
 *
 * Maps to: vkCmdDrawMeshTasksIndirectEXT with a draw count of 1 (Vulkan);
 * ExecuteIndirect with a command signature of one
 * D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH, made with a device created
 * with CRGPU_FEATURE_MESH_SHADING (D3D12);
 * drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:
 * threadsPerObjectThreadgroup:threadsPerMeshThreadgroup: (Metal).
 *
 * @param[in] cmd    Command list being recorded: of a DIRECT allocator,
 *                   inside a rendering pass.
 * @param[in] buffer As crgpu_command_list_dispatch_indirect().
 * @param[in] offset As crgpu_command_list_dispatch_indirect().
 *
 * Checked in this order, on every backend; a refused call records nothing:
 * @retval CR_SUCCESS The dispatch was recorded.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST `cmd` is NULL (or, in Debug,
 *         destroyed).
 * @retval CR_RESULT_GPU_INVALID_BUFFER `buffer` is NULL (or, in Debug,
 *         destroyed).
 * @retval CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED The device was
 *         created without CRGPU_FEATURE_MESH_SHADING.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE `cmd` is of a COPY,
 *         COMPUTE or BUNDLE allocator.
 * @retval CR_RESULT_GPU_COMMAND_OUTSIDE_RENDERING No rendering pass is open
 *         on `cmd`.
 * @retval CR_RESULT_GPU_INVALID_BUFFER_USAGE `buffer` wasn't created with
 *         CRGPU_BUFFER_USAGE_INDIRECT.
 * @retval CR_RESULT_GPU_INDIRECT_OFFSET_ALIGNMENT `offset` is not a
 *         multiple of 4.
 * @retval CR_RESULT_GPU_INDIRECT_RANGE `offset` is past the buffer's size,
 *         or fewer than 12 bytes of it follow `offset`.
 */
CR_RESULT crgpu_command_list_dispatch_mesh_indirect(
    crgpu_command_list cmd,
    crgpu_buffer buffer,
    uint64_t offset);

#endif // CRGPU_EXT_MESH_SHADING

/*
 * ---------------------------------------------------------------------------
 * crcore.h
 * ---------------------------------------------------------------------------
 *
 * [CHANGED] (comment only) 210:
 *
 *	/** A command list type the call can't take there: a bundle submitted to
 *	 * a queue, a type no queue of the device runs, or a list of a type that
 *	 * can't record the command (an indirect dispatch's). * /
 *	CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE                   = 210,
 *
 * [ADDED] (Q5 a), after S52's 374. Why: each a fact the caller can act on
 * (end the pass, begin one, align the offset, size the buffer), named so
 * a later call returns it too. ABI: four values appended. Tests:
 * crgpu_objects_test.
 *
 *	/** A command recorded between crgpu_command_list_begin_rendering() and
 *	 * crgpu_command_list_end_rendering() that a rendering pass can't hold.
 *	 * Since 0.4.0. * /
 *	CR_RESULT_GPU_COMMAND_INSIDE_RENDERING                    = 375,
 *	/** A command recorded outside a rendering pass that only a pass can
 *	 * hold. Since 0.4.0. * /
 *	CR_RESULT_GPU_COMMAND_OUTSIDE_RENDERING                   = 376,
 *	/** An indirect command's offset is not a multiple of 4. Since 0.4.0. * /
 *	CR_RESULT_GPU_INDIRECT_OFFSET_ALIGNMENT                   = 377,
 *	/** An indirect command's arguments run past the end of their buffer.
 *	 * Since 0.4.0. * /
 *	CR_RESULT_GPU_INDIRECT_RANGE                              = 378,
 *
 * cr_result_str() (crcore.c) names the four.
 *
 * ---------------------------------------------------------------------------
 * Where it is done (not in a header)
 * ---------------------------------------------------------------------------
 *
 * - src/gpu/crgpushared.c and .h, beside
 *   crgpu___internal_check_acceleration_structure_list() (line 669):
 *
 *	// The checks of crgpu_command_list_dispatch_indirect() after its
 *	// handles, in the header's order. No allocation.
 *	CR_RESULT crgpu___internal_check_dispatch_indirect(
 *	    CRGPU_COMMAND_LIST_TYPE type, bool rendering,
 *	    const crgpu_buffer_desc *buffer, uint64_t offset) {
 *		if (type != CRGPU_COMMAND_LIST_TYPE_DIRECT && type != CRGPU_COMMAND_LIST_TYPE_COMPUTE)
 *			return CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE;
 *		if (rendering)
 *			return CR_RESULT_GPU_COMMAND_INSIDE_RENDERING;
 *		return crgpu___check_indirect_arguments(buffer, offset);
 *	}
 *	// ..._dispatch_mesh_indirect(): the feature, then DIRECT only, then
 *	// !rendering -> CR_RESULT_GPU_COMMAND_OUTSIDE_RENDERING, then the same.
 *	static CR_RESULT crgpu___check_indirect_arguments(
 *	    const crgpu_buffer_desc *buffer, uint64_t offset) {
 *		if (!(buffer->usage & CRGPU_BUFFER_USAGE_INDIRECT))
 *			return CR_RESULT_GPU_INVALID_BUFFER_USAGE;
 *		if (offset % 4 != 0)
 *			return CR_RESULT_GPU_INDIRECT_OFFSET_ALIGNMENT;
 *		// offset past the size: stops there, so the subtraction can't wrap.
 *		if (offset > buffer->size || buffer->size - offset < sizeof(crgpu_dispatch_indirect_args))
 *			return CR_RESULT_GPU_INDIRECT_RANGE;
 *		return CR_SUCCESS;
 *	}
 *
 *   A static_assert that the struct is 12 bytes with its counts at 0, 4
 *   and 8 (every backend's file includes it once, beside the API's own
 *   struct where one is visible: VkDispatchIndirectCommand,
 *   D3D12_DISPATCH_ARGUMENTS, MTLDispatchThreadgroupsIndirectArguments).
 * - Each backend: cmd null, CR_STALE, buffer null, CR_STALE (each its own
 *   `if`), then (mesh) the device's feature, then the shared check with
 *   its list type and pass flag (pass 6, 3), then the API call:
 *   - Vulkan (crgpuvulkancommand.c, after dispatch at line 687):
 *     vkCmdDispatchIndirect(cmd->_command_buffer_vk, buffer->_buffer_vk,
 *     offset); vkCmdDrawMeshTasksIndirectEXT(..., offset, 1,
 *     sizeof(crgpu_dispatch_indirect_args)) through
 *     device._vkCmdDrawMeshTasksIndirectEXT, loaded with
 *     _vkCmdDrawMeshTasksEXT at crgpuvulkancontext.c line 1144, both
 *     checked there (pass 5, 3).
 *   - D3D12 (crgpud3d12command.cpp): crgpu_d3d12___create_draw_indirect_signature
 *     generalised to take the argument type (it becomes
 *     crgpu_d3d12___create_indirect_signature(device, type, stride));
 *     crgpu_device_d3d12 gains _dispatch_signature and
 *     _dispatch_mesh_signature beside _tight_draw_signature (crgpud3d12.hpp
 *     line 286), made in crgpu_device_create (crgpud3d12context.cpp line
 *     590; the mesh one only with the feature) and released with the draw
 *     ones (line 509); each call is ExecuteIndirect(signature, 1,
 *     buffer->_d3d_resource.Get(), offset, nullptr, 0).
 *   - Metal (crgpumetalcommand.m, after dispatch at line 907): the bound
 *     compute pipeline's _local_size_x/y/z (no clamp) and
 *     dispatchThreadgroupsWithIndirectBuffer: on _active_compute_encoder;
 *     the mesh call sends the push constants as dispatch_mesh does (line
 *     955) and calls drawMeshThreadgroupsWithIndirectBuffer: on
 *     _active_render_encoder with threadsPerObjectThreadgroup (1, 1, 1)
 *     (Metal ignores it with no object function) and the pipeline's
 *     _mesh_local_size_x/y/z. Where Q4's rule is broken (no encoder, no
 *     pipeline) the call records nothing and returns CR_SUCCESS, as its
 *     undefined outcome on Metal.
 *
 * ---------------------------------------------------------------------------
 * Records when built
 * ---------------------------------------------------------------------------
 *
 * carrottus/CLAUDE.md: a bullet under "Carrottus GPU conventions" (indirect
 * dispatch: the calls, the checks, the limits rule, the bound-state rule,
 * D3D12's signatures made with the device), and "Known gaps"' indirect
 * draw bullet pointed at the draws' card. CRGPU_DEVICE_TODO.md's indirect
 * dispatch entry ("Later"): built, each machine's run. The workspace's
 * DECISIONS.md: card A15's Carrottus half built; cards for "Found in the
 * code" 1 to 7 and Q4's check. The caller's own records are the caller's
 * to write.
 */
