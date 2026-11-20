/*
 * crgpu_draw_dispatch_checks_draft.h - Carrottus GPU's draws and dispatches
 * checked before they are recorded, on every backend alike: the indirect
 * draws take the indirect dispatch's argument checks and return a result
 * (card S55), and every backend tracks what is bound and refuses a draw or
 * dispatch with nothing valid bound, the direct dispatches' group counts
 * checked on the CPU (card S56). One draft because both cards change the
 * same eight calls.
 *
 * DRAFT, 2026-11-20, version 2 (version 1, then seven critique passes, each
 * fixed before the next), for the user's approval; not built.
 * Card S55 decided 2026-11-20 (a): "crgpu_command_list_draw_indirect and
 * its siblings return CR_RESULT and take the indirect dispatch's checks
 * (no CRGPU_BUFFER_USAGE_INDIRECT 221, an offset not a multiple of 4 377, a
 * range past the buffer 378) on every backend". Card S56 decided 2026-11-20
 * (a): "every backend tracks what is bound and refuses a dispatch (and a
 * draw) with nothing valid bound, each its own code, and the direct
 * dispatches' counts checked on the CPU against 65,535". Both found
 * drafting card A15's indirect dispatch (crgpu_dispatch_indirect_draft.h,
 * "Found in the code" 1 to 4 and its Q3 and Q4).
 *
 * No version raise while carrottus' 0.4.0 is untagged (card X2): every
 * break below is listed against the ABI snapshot (./check.sh --update-abi).
 *
 * Written unattended on the Mac (Apple M5 Pro, macOS 27) against
 * include/crgpu.h (the draws, lines 5840 to 5898; the indirect draws and
 * their args, 5900 to 6005; crgpu_command_list_dispatch_mesh(), 6119;
 * crgpu_command_list_dispatch(), 7278; _dispatch_indirect(), 7349;
 * _bind_graphics_pipeline(), 5681; _bind_compute_pipeline(), 7206;
 * _set_index_buffer(), 5829; _execute_bundle(), 8316;
 * CRGPU_ADAPTER_PROPERTIES::max_draw_indirect_count, 861), include/crcore.h
 * (codes 204, 210, 221, 230, 375, 377, 378; 376 held), src/gpu/crgpushared.c
 * and .h (crgpu___internal_check_dispatch_indirect(), line 725), the three
 * backends' command files (crgpumetalcommand.m: begin 358, the draws 707 to
 * 838, bind_graphics_pipeline 863, bind_compute_pipeline 931, dispatch 954,
 * dispatch_indirect 990, dispatch_mesh 1027, draw_indirect 1092,
 * draw_indexed_indirect 1135, the compute encoder's opening 462 to 484;
 * crgpuvulkancommand.c: begin 254, the draws 392 to 474, binds 406 and 674,
 * dispatch 687, dispatch_indirect 701, the indirect draws 716 and 735,
 * dispatch_mesh 755; crgpud3d12command.cpp: begin 235, set_index_buffer
 * 337, the draws 348 to 404, dispatch 431, dispatch_mesh 444,
 * dispatch_indirect 548, the indirect draws 571 and 595, execute_bundle
 * 837; crgpud3d12shader.cpp's binds, 913 and 932), crgpumetalrenderpass.m
 * (begin_rendering 375, the blit encoder's opening 108 to 130), the
 * internal headers' list structs (crgpumetal.h 280 to 410, crgpuvulkan.h
 * 247 to 290, crgpud3d12.hpp 370 to 420), abi.txt; the Vulkan SDK
 * 1.4.363.0's validusage.json (read here); the macOS SDK's
 * MTLRenderCommandEncoder.h and MTLComputeCommandEncoder.h (read here); and
 * Microsoft's documentation of ExecuteIndirect, DrawInstanced and Dispatch
 * as remembered (not on this machine). carrottus 5b71f5d, 0.4.0, untagged.
 * Not built, not in any target, not compilable: it repeats declarations of
 * crgpu.h and crcore.h to show them changed. Every declaration below is
 * marked [ADDED], [CHANGED] or [REMOVED]; nothing unmarked changes. Once
 * approved it is frozen (ROADMAP.md, section 1, "Work built from approved
 * drafts").
 *
 * Result codes: 375, 377 and 378 are built (card A15); 376 is held by A15
 * for the mesh indirect call (card S54) and is built here, named for what
 * it says, as A15's Q5 meant; this draft takes 379 to 385, the next free.
 * crgpu_format_uses_draft.h (card S58, drafted the same day) takes 386 after
 * them; if this draft is declined, S58 takes 379 instead.
 *
 * ---------------------------------------------------------------------------
 * Revision notes (version 1 written, then seven passes, each fixed before
 * the next; one lens a pass)
 * ---------------------------------------------------------------------------
 *
 * Pass 1 (correctness against the built code).
 *  1. Wrong in version 1: it said "four calls", after the card. crgpu.h has
 *     two indirect draws (draw_indirect, draw_indexed_indirect); the card
 *     counted with S56's. S55 changes two; S56 changes the four direct
 *     draws, the direct dispatch and the direct mesh dispatch: eight calls
 *     go from void to CR_RESULT in all, and dispatch_indirect gains a
 *     refusal. Corrected throughout.
 *  2. Missing in version 1: the indirect draws already refuse silently on
 *     every backend (draw_count 0, above the device's limit, a stride not a
 *     multiple of 4 or below the args' size: crgpuvulkancommand.c 722 to
 *     724, the same in the other two). Each now has a code (Q2): 384 and
 *     385, not dropped and not folded into 378.
 *  3. Found: D3D12 makes a signature for a stride other than the tightly
 *     packed one at record time (crgpu_d3d12___get_draw_indirect_signature(),
 *     a lazily made member per carrottus/CLAUDE.md, "Check the handle") and
 *     a failure is a silent `return` (crgpud3d12command.cpp 588 to 590).
 *     Now said: CR_RESULT_API_SPECIFIC_FAILURE (or OUT_OF_MEMORY for the
 *     eq::array's growth), after every argument check, nothing recorded.
 *     The other two backends can't fail there.
 *  4. Found: Metal's crgpu_command_list_bind_compute_pipeline() opens a
 *     compute encoder, and so ends a render encoder that is open
 *     (crgpumetalcommand.m 467 to 471): a compute bind inside a pass ends
 *     the pass on Metal, and every later draw of it records nothing, while
 *     Vulkan and D3D12 keep the pass. Fixed here as part of the tracking
 *     (section "The tracked state", event 6): a compute bind inside a pass
 *     records nothing on every backend. The bind stays void (Q5).
 *  5. Found: a graphics pipeline bound outside a pass on Metal is kept in
 *     _bound_pipeline but never set on the next pass's encoder
 *     (begin_rendering clears _encoder_pipeline_state_mtl, 483); a draw
 *     then reads the primitive type from a pipeline the encoder doesn't
 *     have, and Metal's validation stops the draw. crgpu.h already says
 *     "Call inside a rendering pass" (5674); the tracking makes the rule
 *     held: a pass's begin clears the graphics pipeline on every backend.
 *  6. Found: an index buffer view whose stride is neither 2 nor 4 is
 *     treated as 32-bit by Vulkan (crgpuvulkancommand.c 381 to 383), refused
 *     silently at the draw by Metal (resolve_index_type, 655), and passed to
 *     D3D12 as its view says. Tracked: such a view binds no index buffer, so
 *     an indexed draw after it is refused with 382 on every backend.
 *
 * Pass 2 (the C API's conventions: based or frozen, no hidden policy, no
 * default that decides, errors as codes).
 *  1. Wrong in version 1: a count of 0 (vertices, instances, indices, a
 *     draw_count, a group count) was "refused with CR_RESULT_ZERO_SIZE".
 *     Every API takes a 0 as no work (Vulkan's vkCmdDraw has no VUID
 *     against 0; D3D12's Dispatch and DrawInstanced document it; A15's test
 *     4 ran zeros on Metal with no message), and a GPU-driven caller hands
 *     on counts it computed. Now: every check runs, then a 0 records nothing
 *     and returns CR_SUCCESS, on every backend (no API call is made, so no
 *     backend's validation sees a 0). The same rule A15 states for the
 *     indirect counts; Q3 keeps the refusal on record, not recommended.
 *  2. No struct is added or changed; no based struct; the codes are the
 *     only new public items besides the return types.
 *  3. No default: the checks refuse; nothing is bound again on the
 *     caller's behalf (Metal's lost state is not restored into a new
 *     encoder: A15's pass 4, 3 rejected that as hidden policy, and card
 *     S56 decided to refuse, not to emulate).
 *  4. Each check its own `if`, in parameter order, the list's own state
 *     (type, pass, what is bound) before the other arguments' properties,
 *     as the copies and A15 order them. The indirect dispatch's built order
 *     (type, pass, usage, offset, range) gains its pipeline check after the
 *     pass and before the usage: listed as a change of order in "The
 *     version and the breaks".
 *  5. Codes: one for each fact a caller can act on (Q4): no graphics
 *     pipeline, no compute pipeline, the graphics pipeline of the wrong kind
 *     (a mesh pipeline for a draw, a vertex one for a mesh dispatch), no
 *     index buffer, a group count past the limit, a stride, a draw count.
 *
 * Pass 3 (the platform APIs on all three backends).
 *  1. Vulkan (validusage.json, read here): vkCmdDrawIndirect's
 *     buffer-02709 (INDIRECT_BUFFER_BIT), offset-02710 (a multiple of 4),
 *     drawCount-02718 (1 at most without multiDrawIndirect), -02719 (at
 *     most maxDrawIndirectCount), -00476 (drawCount above 1: the stride a
 *     multiple of 4 and at least the command's size), -00487 (drawCount 1:
 *     offset + sizeof within the buffer), -00488 (above 1: stride x (count
 *     - 1) + offset + sizeof within it), -renderpass (inside a render pass
 *     instance), None-08606 (a valid pipeline bound); the indexed twin's
 *     -00528, -00539, -00540 and None-07312 (a valid index buffer bound,
 *     without maintenance6). vkCmdDraw's None-08606 and -renderpass;
 *     vkCmdDispatch's groupCountX-00386 to Z-00388 and None-08606.
 *     Taken whole, with two narrowings for every backend: the stride rule
 *     holds for a draw_count of 1 too (Vulkan checks it only above 1;
 *     Metal's loop and D3D12's signature read it at any count), and the
 *     range rule is written once for any count of 1 or more.
 *  2. D3D12: ExecuteIndirect's ArgumentBufferOffset a multiple of 4 and the
 *     arguments within the buffer (as remembered); a command signature's
 *     ByteStride a multiple of 4 and at least the arguments' size (as
 *     remembered); no resource flag for indirect use (the usage bit is
 *     Carrottus GPU's own, refused alike); Dispatch's 65535 a dimension
 *     (D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION, A15's pass 2, 2);
 *     one pipeline-state slot shared by graphics and compute
 *     (SetPipelineState), and a list Reset() with a null initial state.
 *  3. Metal (macOS SDK, read here): drawPrimitives:indirectBuffer:
 *     indirectBufferOffset: and the indexed twin, "indirectBufferOffset must
 *     be a multiple of 4" (MTLRenderCommandEncoder.h 775 and 787); one draw
 *     a call (the backend loops draw_count times); a render encoder exists
 *     only inside a pass and ends at the pass's end; a compute encoder ends
 *     at any other encoder's opening and takes its pipeline and buffers
 *     with it.
 *  4. Each backend's own ordering of what it keeps is why the rule is the
 *     union of every backend's losses (the narrowest contract,
 *     carrottus/CLAUDE.md, "Where the graphics APIs behave differently
 *     underneath"): Vulkan keeps a pipeline per bind point across passes
 *     and copies, so a Vulkan caller now rebinds where Metal needs it.
 *
 * Pass 4 (industry practice).
 *  1. WebGPU: setPipeline is required before draw/dispatch (a validation
 *     error otherwise, recorded on the encoder); drawIndirect's buffer has
 *     INDIRECT usage, an offset a multiple of 4 and 16 (20 indexed) bytes
 *     within the buffer; an indexed draw needs setIndexBuffer; a render
 *     pass encoder starts with nothing bound (state is per pass encoder);
 *     dispatchWorkgroups past maxComputeWorkgroupsPerDimension (65535) is a
 *     validation error. The tracked rule here is WebGPU's shape: state per
 *     pass, checked at the draw.
 *  2. SDL_GPU: SDL_BindGPUGraphicsPipeline inside a render pass, state per
 *     pass; SDL_DrawGPUPrimitivesIndirect(pass, buffer, offset, draw_count)
 *     with a tight stride; debug-mode checks of a bound pipeline. NVRHI:
 *     setGraphicsState/setComputeState carry the whole state, so nothing is
 *     "unbound"; its draw takes the state's indirect buffer. bgfx: state
 *     set per submit. As remembered of each project's public API; none is
 *     on this machine.
 *  3. The common result: a draw with no pipeline, or an indexed draw with
 *     no index buffer, is an error the API says, not a silent no-op.
 *     Kept. Vertex buffers: WebGPU checks every slot the pipeline declares;
 *     Q6 keeps that on record, not recommended here.
 *
 * Pass 5 (performance).
 *  1. A draw is the hottest recording call. Version 1 had each backend
 *     keep its own flags. Now one 4-byte record (crgpu___bound_state, in
 *     crgpushared.h) beside each list's hot fields, read and written by
 *     inline helpers: a draw's added cost is three to five predictable
 *     branches and one load of a cache line the call already touches
 *     (Metal reads _active_render_encoder, Vulkan _command_buffer_vk,
 *     D3D12 _d3d_cmd_list beside it). Measured when built (Tests, 10):
 *     1,000,000 draws recorded, Release, before and after, on the M5 Pro.
 *  2. The events that clear the record are already calls (a copy, a pass,
 *     a label): one byte written each. No allocation anywhere.
 *  3. A Vulkan or D3D12 caller now rebinds a compute pipeline after a copy
 *     where it didn't need to: one vkCmdBindPipeline / SetPipelineState
 *     per compute stretch, which the APIs make cheap for the same object.
 *     Said in the header as the rule's cost.
 *  4. Metal's indirect draws still loop draw_count calls (Metal has no
 *     multi-draw from a buffer outside indirect command buffers): unchanged,
 *     noted in "Left out".
 *
 * Pass 6 (tests: each failing first, on every backend).
 *  1. Every refusal test is written so that it fails against the code as
 *     built: today's calls are void, so the tests are first compiled
 *     against the new signatures with stub bodies that return CR_SUCCESS
 *     and record as today (the A15 pattern), and every refusal case fails.
 *  2. Each refusal is followed by a submit and a read-back proving nothing
 *     ran (a counter shader for dispatches; a 4 x 4 target cleared to a
 *     sentinel for draws), so "records nothing" is tested, not assumed.
 *  3. The events: one test per clearing event, on every backend: bind,
 *     the event, draw or dispatch refused; bind again, accepted and run.
 *     On Vulkan and D3D12 these fail first too (they keep the state today,
 *     the stubs accept), which is the point of the narrowest rule.
 *  4. Vulkan written and type-checked here against the SDK's headers
 *     (clang -fsyntax-only, as A15's was); D3D12 written, not compiled;
 *     both owed a run on Windows 11 and Fedora.
 *
 * Pass 7 (what callers above see).
 *  1. Source: a C or C++ caller that ignores the result still compiles
 *     (no [[nodiscard]] in a C header); a binding generated from the
 *     headers changes eight signatures. ABI: eight return types change.
 *  2. Behaviour: a caller that followed crgpu.h's rules sees no refusal.
 *     One that leaned on Vulkan's or D3D12's kept state (a dispatch after a
 *     copy with no rebind, a draw with a pipeline bound before the pass)
 *     is refused on every backend now, where before it worked on two and
 *     recorded nothing on Metal. The rerun of the callers' suites under
 *     each backend's validation is the build's gate (Tests, 11); every
 *     refusal it finds in a caller is that caller's to fix, recorded in
 *     its own TODO, not worked round here.
 *  3. A caller that records commands from its own command stream checks
 *     each result and fails its own compile on a refusal, naming the call:
 *     the caller's design, after this is approved.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * Today the same mistake does three things. A dispatch after a copy with no
 * rebind runs on Vulkan, records nothing on Metal (its compute encoder,
 * and with it the pipeline and sets, ended at the copy), and runs on D3D12
 * with whatever pipeline the list's one slot last took, graphics included.
 * A draw with no pipeline bound reaches Vulkan's driver (None-08606) and
 * D3D12's, and is silently dropped by Metal. An indirect draw with a buffer
 * lacking the usage, a misaligned offset or a range past the buffer reaches
 * every driver unchecked. And none of these calls can say so: they are
 * void. Carrottus's rule is one behaviour for one condition on every
 * backend, each failure its own code (carrottus/CLAUDE.md, "A function
 * returns the same value for the same condition in every backend").
 *
 * ---------------------------------------------------------------------------
 * What changes, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * [CHANGED] crgpu_command_list_draw(), _draw_indexed(), _draw_instanced(),
 * _draw_indexed_instanced(), _draw_indirect(), _draw_indexed_indirect(),
 * _dispatch() and _dispatch_mesh() return CR_RESULT. Every backend keeps one
 * small record of what is bound (crgpu___bound_state: the graphics
 * pipeline's kind, a compute pipeline, an index buffer), cleared at the
 * same events on all three: the union of what any backend loses (section
 * "The tracked state"). A draw is refused on a list that can't hold it, on
 * a direct list outside a pass, with no graphics pipeline bound since the
 * pass began, with a mesh pipeline bound, and (indexed) with no index
 * buffer; a dispatch on a list that can't hold it, inside a pass, with no
 * compute pipeline, and (direct) with a group count above 65535; a mesh
 * dispatch with no mesh pipeline or counts past its rule. The indirect
 * draws take the indirect dispatch's argument checks (221, 377, 378) and
 * codes for the stride and the draw count they already refused silently.
 * [CHANGED] crgpu_command_list_dispatch_indirect() refuses with no compute
 * pipeline (it recorded nothing and returned CR_SUCCESS on Metal). A count
 * of 0 records nothing and succeeds. Seven codes are added (379 to 385) and
 * 376 is built. A compute bind inside a pass records nothing on every
 * backend (Metal ended the pass).
 *
 * ---------------------------------------------------------------------------
 * The tracked state (the rule, written once; held by every backend)
 * ---------------------------------------------------------------------------
 *
 * Internal, crgpushared.h (not public):
 *
 *	// What a list has bound that a draw or dispatch reads, by the rule
 *	// crgpu.h states: cleared at the same events on every backend. 4 bytes,
 *	// plain data, beside each backend's hot list fields.
 *	typedef struct crgpu___bound_state {
 *		uint8_t graphics;     // CRGPU___BOUND_NONE, _VERTEX, _MESH
 *		uint8_t compute;      // 0 or 1
 *		uint8_t index_buffer; // 0 or 1: a view of stride 2 or 4 is bound
 *		uint8_t reserved;     // 0
 *	} crgpu___bound_state;
 *
 * Events, each one call of a shared inline helper at the same point of the
 * same call in every backend (a refused or recording-nothing call changes
 * nothing):
 *  1. crgpu_command_list_begin(), crgpu_command_list_begin_bundle(): all
 *     cleared. (D3D12 resets with a null state; Metal takes a new command
 *     buffer; a Vulkan secondary buffer inherits nothing.)
 *  2. crgpu_command_list_begin_rendering(): graphics and compute cleared
 *     (Metal's new render encoder starts empty; its compute encoder ends).
 *     The index buffer is kept: every backend keeps it across passes
 *     (Metal holds it as the list's, not the encoder's).
 *  3. A copy, blit or clear recorded outside a pass, a debug label begun
 *     or ended outside a pass, an acceleration structure build, copy or
 *     compacted-size write: compute cleared (Metal ends its compute encoder
 *     at each).
 *  4. crgpu_command_list_execute_bundle(): graphics and index buffer
 *     cleared (crgpu.h already makes them undefined, 8304 to 8306).
 *  5. crgpu_command_list_bind_graphics_pipeline() inside a pass, or on a
 *     bundle: graphics set to the pipeline's kind; compute cleared (D3D12's
 *     shared slot). Outside a pass on a direct list it records nothing, on
 *     every backend: crgpu.h's "Call inside a rendering pass" held (pass 1,
 *     5).
 *  6. crgpu_command_list_bind_compute_pipeline() outside a pass: compute
 *     set; graphics cleared (D3D12's shared slot). Inside a pass it records
 *     nothing, on every backend (pass 1, 4).
 *  7. crgpu_command_list_set_index_buffer(): index_buffer set where the
 *     view's stride is 2 or 4, cleared otherwise (pass 1, 6).
 *  8. Not events: barriers (pipeline, image, acceleration structure),
 *     timestamps, viewports, scissors, vertex buffers, sets, push
 *     constants, depth bounds. None ends a Metal encoder (card S60's
 *     barriers are recorded into the open encoder).
 *
 * Descriptor sets and push constants keep the rule crgpu.h states (read
 * since the last event above and the last pipeline bind), unchecked: Q6.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 *  - Vulkan (validusage.json, SDK 1.4.363.0): pass 3, 1.
 *  - D3D12 (Microsoft's documentation, as remembered): pass 3, 2.
 *  - Metal (the macOS SDK's headers, read here): pass 3, 3.
 *  - WebGPU, SDL_GPU, NVRHI, bgfx (as remembered): pass 4.
 *
 * Hazel is not compared: Carrottus is not measured against an engine (the
 * workspace's CLAUDE.md, "External references").
 *
 * ---------------------------------------------------------------------------
 * What the caller decides, and what Carrottus GPU only reports
 * ---------------------------------------------------------------------------
 *
 * The caller decides what to bind and when, and rebinds after each event
 * the rule names. Carrottus GPU records what is bound, refuses a call the
 * rule says reads nothing valid, and never binds, restores or skips
 * anything on the caller's behalf. The counts the GPU reads from a buffer
 * stay the caller's to keep (A15's rule): never read, clamped or rewritten.
 *
 * ---------------------------------------------------------------------------
 * Left out, and why
 * ---------------------------------------------------------------------------
 *
 *  - The bind calls' own results (bind_*_pipeline, set_index_buffer,
 *    set_vertex_buffers, bind_*_descriptor_sets stay void): Q5.
 *  - Descriptor sets, push constants and vertex buffers checked at the
 *    draw: Q6.
 *  - A draw count from the GPU (vkCmdDrawIndirectCount and its kin):
 *    deferred by card A15.
 *  - Metal's per-draw loop for draw_count above 1: an indirect command
 *    buffer would make it one call; not needed by a caller yet.
 *  - The mesh indirect call: card S54.
 *  - The caller's own handling of the results: the caller's design.
 *
 * ---------------------------------------------------------------------------
 * The version and the breaks
 * ---------------------------------------------------------------------------
 *
 *  - crgpu.h: eight calls' return types from void to CR_RESULT (ABI
 *    breaks, each listed by ./check.sh --update-abi); their comments; the
 *    comments of crgpu_command_list_bind_graphics_pipeline(),
 *    _bind_compute_pipeline(), _set_index_buffer(), _dispatch_indirect()
 *    (a refusal added, and its order: the pipeline after the pass, before
 *    the usage) and _execute_bundle().
 *  - crcore.h: 376 built (its comment unchanged from A15's); 379 to 385
 *    added; cr_result_str() names each.
 *  - Behaviour: calls that recorded something invalid or nothing now
 *    refuse with a code; a 0 count records nothing on every backend (it
 *    reached the API before); a compute bind inside a pass and a graphics
 *    bind outside one record nothing on every backend.
 *  - No version raise: carrottus' 0.4.0 is untagged (card X2). The misuse
 *    generator is rerun (eight changed calls).
 *
 * New handles: none. New public struct types: none. Struct layouts:
 * unchanged. Stored formats: none. Thread: nothing changes (a list is used
 * by one caller at a time; the record is the list's). Allocation: none
 * added (D3D12's lazily made signature already allocated; now its failure
 * is said).
 *
 * ---------------------------------------------------------------------------
 * Questions (each with options and a recommendation)
 * ---------------------------------------------------------------------------
 *
 * Q1. WHERE THE TRACKED RECORD LIVES.
 *     (a) RECOMMENDED. One internal struct and its helpers in
 *         crgpushared.h, a member of each backend's list, the events and
 *         checks one call each: the rule written once, held the same way
 *         by all three (as the copies' checks are).
 *     (b) Each backend its own flags. Three copies of one rule, which is
 *         how the backends came to differ.
 *
 * Q2. THE INDIRECT DRAWS' SILENT REFUSALS (stride, draw count).
 *     (a) RECOMMENDED. Each its own code: CR_RESULT_GPU_INDIRECT_STRIDE
 *         (384) and CR_RESULT_GPU_INDIRECT_DRAW_COUNT (385), named for the
 *         indirect family so a later call (a draw count from the GPU)
 *         returns them too.
 *     (b) Both as CR_RESULT_DESC_INVALID_PARAMETERS: fewer codes, and the
 *         caller can't tell a stride from a count.
 *
 * Q3. A COUNT OF 0.
 *     (a) RECOMMENDED. Every check runs, then nothing is recorded and the
 *         call returns CR_SUCCESS, on every backend: what every API does
 *         with a 0, said once, and no API sees it.
 *     (b) Refused with CR_RESULT_ZERO_SIZE: a GPU-driven caller would test
 *         every count it hands on; no API asks it.
 *     (c) Passed to the API as today: Metal's validation behaviour for a 0
 *         direct draw or dispatch is not documented in its headers.
 *
 * Q4. THE CODES.
 *     (a) RECOMMENDED. Seven new, each a fact the caller acts on:
 *         CR_RESULT_GPU_NO_GRAPHICS_PIPELINE (379),
 *         _NO_COMPUTE_PIPELINE (380), _GRAPHICS_PIPELINE_KIND (381),
 *         _NO_INDEX_BUFFER (382), _DISPATCH_GROUP_COUNT (383),
 *         _INDIRECT_STRIDE (384), _INDIRECT_DRAW_COUNT (385); 376 built.
 *     (b) Widen 301 and 303 ("a pipeline argument is NULL") to "or none is
 *         bound": two codes would then mean an argument and a list's state,
 *         which carrottus/CLAUDE.md asks a code not to blur.
 *
 * Q5. THE BIND CALLS (they stay void; card S56 named the draws and
 *     dispatches).
 *     (a) RECOMMENDED. Void, as now; a bind the rule can't take (a
 *         graphics bind outside a pass, a compute bind inside one) records
 *         nothing on every backend and leaves the record as it was, so the
 *         draw or dispatch after it is refused with its own code. A card
 *         for results on the binds, if a caller asks.
 *     (b) The binds return CR_RESULT too: six more signature changes,
 *         beyond both cards.
 *
 * Q6. DESCRIPTOR SETS, PUSH CONSTANTS AND VERTEX BUFFERS AT THE DRAW.
 *     (a) RECOMMENDED. Not checked: the rule stays as crgpu.h writes it.
 *         A set check either refuses what Vulkan and D3D12 allow (sets kept
 *         across pipelines of a compatible layout) or tracks every layout's
 *         compatibility per set; bindless fills set slots too; vertex
 *         buffers would need each pipeline's bindings compared at each draw.
 *     (b) A bound-set mask per bind point against the pipeline's set count,
 *         cleared at the events and at every pipeline bind: one AND at the
 *         draw, and it refuses a legal Vulkan pattern (sets bound once,
 *         pipelines switched).
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * crgpu_objects_test, every backend, the first adapter and any with mesh
 * shading; each refusal submitted and read back: a dispatch's counter
 * shader (A15's objects_dispatch_count_comp) shows 0, a draw's 4 x 4
 * target keeps its clear sentinel; each acceptance shows its work.
 *  1. Direct draws, refusals in order: cmd NULL (204), a COPY list and a
 *     COMPUTE list (210), outside a pass (376), no pipeline (379), a mesh
 *     pipeline bound (381, where the adapter has mesh shading), and for
 *     the two indexed draws no index buffer (382) and a view of stride 12
 *     bound (382). Each of the four draws.
 *  2. The events, one case each on every backend: a pipeline bound in an
 *     earlier pass (379); a pipeline bound outside any pass, then a pass
 *     (379); a compute bind inside the pass, then a draw (accepted: the
 *     compute bind recorded nothing; the draw's pixels are the graphics
 *     pipeline's); a bundle executed, then a draw (379) and an indexed draw
 *     after rebinding the pipeline only (382), where CRGPU_EXT_BUNDLES is 1.
 *  3. Direct dispatch, refusals in order: a COPY list (210), inside a pass
 *     (375), no compute pipeline (380), a count of 65536 in each of x, y, z
 *     (383); accepted at 65535 (the counter 65535, as A15's test 5).
 *  4. The compute events, each on every backend: bind, then each of a copy,
 *     a blit, a clear outside a pass, a label begun, a label ended, an
 *     acceleration structure build (where CRGPU_FEATURE_RAY_QUERY), a pass,
 *     a graphics bind (in a pass), crgpu_command_list_begin(): a dispatch
 *     after it 380, and after a rebind accepted and run.
 *  5. Indirect dispatch: no compute pipeline (380, in its new place:
 *     after INSIDE_RENDERING, before INVALID_BUFFER_USAGE); A15's tests 1
 *     to 7 unchanged otherwise.
 *  6. Mesh dispatch (where mesh shading): outside a pass (376), no pipeline
 *     (379), a vertex pipeline bound (381), counts 65536 and a product of
 *     1025 (383); 1024 accepted.
 *  7. Indirect draws, both, refusals in the header's order: cmd NULL,
 *     buffer NULL, a COMPUTE list, outside a pass, no pipeline, (indexed)
 *     no index buffer, usage STORAGE alone (221), offsets 2 and 6 (377),
 *     strides 0, 2, 12 (384; 16 and 20 the tight sizes accepted), a
 *     draw_count of 2 on a device without CRGPU_FEATURE_MULTI_DRAW_INDIRECT
 *     and of max_draw_indirect_count + 1 with it (385, where it isn't
 *     UINT32_MAX), the range at counts 1 and 3 (378: a 64-byte buffer,
 *     offset 52 count 1 accepted for 12 bytes? no: 16 bytes needed, so 52
 *     is refused and 48 accepted; stride 32 count 3 from 0 needs 80 bytes,
 *     refused; offset UINT64_MAX - 3 refused without wrapping); and two
 *     wrong at once for each adjacent pair. Accepted: 3 draws at stride 32
 *     writing three known pixels.
 *  8. A count of 0 in each of the eight calls and the indirect dispatch's
 *     buffer: CR_SUCCESS, nothing drawn or counted, no validation message
 *     (Metal's validation with warnings fatal, Vulkan's validation and
 *     synchronization validation, D3D12's debug layer and GPU-based
 *     validation).
 *  9. cr_misuse_tests regenerated (eight changed calls; in Debug each with
 *     its list destroyed: 204). cr_noalloc_tests: a frame of each draw
 *     kind, a dispatch and the binds and events, 0 of ours.
 * 10. Measured (Release, no validation, the M5 Pro; each backend's run
 *     owed elsewhere): 1,000,000 draw_instanced recorded inside one pass,
 *     and 100,000 dispatches between binds, CPU time before and after, three
 *     rounds; the figures into CRGPU_DEVICE_TODO.md.
 * 11. Callers rerun under Metal API and shader validation with warnings
 *     fatal, Debug and Release; every refusal a caller's suite hits is
 *     listed for that caller's TODO.
 * Seen failing first: the eight calls declared with the new return types,
 * each body today's plus `return CR_SUCCESS;` (and the events not yet
 * wired): every refusal case of tests 1 to 7 fails on every backend, and
 * test 8's Metal case is the one that may fail for a different reason (a 0
 * reaching the API), recorded either way.
 * Where each runs: the Mac (macos-metal, Debug and Release, Metal's
 * validation with warnings fatal, -asan, -tsan); Vulkan type-checked here;
 * D3D12 written, not compiled. Owed on Windows 11 (Iris Xe, RTX A2000, WARP
 * over D3D12 under the debug layer and GPU-based validation; Iris Xe and
 * A2000 over Vulkan under validation and synchronization validation) and
 * Fedora (ANV, NVK, RADV). ./check.sh: abi, doxygen, conventions,
 * struct-bases, caller-names.
 *
 * ---------------------------------------------------------------------------
 * crgpu.h
 * ---------------------------------------------------------------------------
 */

/**
 * [CHANGED] (comment only; event 5)
 * @brief Binds a graphics pipeline for subsequent draws.
 *
 * Call inside a rendering pass, after crgpu_command_list_begin_rendering(),
 * or on a bundle. Outside a pass on a direct list it records nothing, on
 * every backend. What a draw reads is stated once, on
 * crgpu_command_list_draw(); this bind ends any compute pipeline's binding
 * (graphics and compute share one pipeline slot on some backends).
 *
 * ... parameters unchanged ...
 */
void crgpu_command_list_bind_graphics_pipeline(
    crgpu_command_list cmd,
    crgpu_graphics_pipeline pipeline);

/**
 * [CHANGED] (comment only; event 7)
 * @brief Binds the index buffer used by indexed draws.
 *
 * A view of stride 2 (16-bit indices) or 4 (32-bit) binds it; a view of
 * any other stride binds nothing, and an indexed draw after it is refused
 * with CR_RESULT_GPU_NO_INDEX_BUFFER, on every backend. The binding lasts
 * across passes until crgpu_command_list_begin(), _begin_bundle() or
 * _execute_bundle().
 *
 * ... parameters unchanged ...
 */
void crgpu_command_list_set_index_buffer(
    crgpu_command_list cmd,
    crgpu_buffer_view buffer_view);

/**
 * [CHANGED] returns CR_RESULT (was void); the rule of what a draw reads.
 * @brief Records a non-indexed, non-instanced draw.
 *
 * What every draw reads, on every backend, whatever the graphics API
 * underneath keeps:
 * - the graphics pipeline bound since the pass began (or since the
 *   bundle's begin), not ended by crgpu_command_list_execute_bundle() or a
 *   compute bind;
 * - for an indexed draw, the index buffer bound since
 *   crgpu_command_list_begin() (or the bundle's begin, or the last
 *   execute_bundle());
 * - the descriptor sets, push constants and vertex buffers bound since
 *   the pipeline, whose absence is unchecked and undefined.
 * The first two are checked; a draw without them records nothing and
 * returns its code. A count of 0 records nothing and returns CR_SUCCESS,
 * after every check.
 *
 * Maps to: vkCmdDraw (Vulkan); DrawInstanced with one instance (D3D12);
 * drawPrimitives:vertexStart:vertexCount: (Metal).
 *
 * @param[in] cmd                   Command list being recorded: of a DIRECT
 *                                  allocator inside a rendering pass, or a
 *                                  BUNDLE allocator.
 * @param[in] vertex_count          Number of vertices to draw.
 * @param[in] start_vertex_location Index of the first vertex.
 *
 * Checked in this order, on every backend; a refused call records nothing:
 * @retval CR_SUCCESS The draw was recorded, or a count is 0.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST `cmd` is NULL (or, in Debug,
 *         destroyed).
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE `cmd` is of a COPY or
 *         COMPUTE allocator.
 * @retval CR_RESULT_GPU_COMMAND_OUTSIDE_RENDERING `cmd` is of a DIRECT
 *         allocator and no rendering pass is open on it.
 * @retval CR_RESULT_GPU_NO_GRAPHICS_PIPELINE No graphics pipeline is bound
 *         by the rule above.
 * @retval CR_RESULT_GPU_GRAPHICS_PIPELINE_KIND The bound pipeline is a mesh
 *         pipeline (crgpu_graphics_pipeline_desc::mesh_module).
 */
CR_RESULT crgpu_command_list_draw(
    crgpu_command_list cmd,
    uint32_t vertex_count,
    uint32_t start_vertex_location);

/**
 * [CHANGED] returns CR_RESULT (was void).
 * @brief Records an indexed, non-instanced draw.
 *
 * The rule of crgpu_command_list_draw() holds. Maps to: vkCmdDrawIndexed
 * (Vulkan); DrawIndexedInstanced with one instance (D3D12);
 * drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:
 * instanceCount:baseVertex:baseInstance: (Metal).
 *
 * ... parameters unchanged ...
 *
 * @return As crgpu_command_list_draw(), then:
 * @retval CR_RESULT_GPU_NO_INDEX_BUFFER No index buffer is bound by the
 *         rule (or the one bound has a stride other than 2 or 4).
 */
CR_RESULT crgpu_command_list_draw_indexed(
    crgpu_command_list cmd,
    uint32_t index_count,
    uint32_t start_index_location,
    int32_t base_vertex_location);

/**
 * [CHANGED] returns CR_RESULT (was void).
 * @brief Records a non-indexed, instanced draw.
 *
 * The rule and results of crgpu_command_list_draw(); a 0
 * `instance_count` records nothing too.
 *
 * ... parameters unchanged ...
 */
CR_RESULT crgpu_command_list_draw_instanced(
    crgpu_command_list cmd,
    uint32_t vertex_count_per_instance,
    uint32_t instance_count,
    uint32_t start_vertex_location,
    uint32_t start_instance_location);

/**
 * [CHANGED] returns CR_RESULT (was void).
 * @brief Records an indexed, instanced draw.
 *
 * The rule and results of crgpu_command_list_draw_indexed().
 *
 * ... parameters unchanged ...
 */
CR_RESULT crgpu_command_list_draw_indexed_instanced(
    crgpu_command_list cmd,
    uint32_t index_count_per_instance,
    uint32_t instance_count,
    uint32_t start_index_location,
    int32_t base_vertex_location,
    uint32_t start_instance_location);

/**
 * [CHANGED] returns CR_RESULT (was void); checks added (card S55).
 * @brief Records `draw_count` non-indexed draws, each reading its arguments
 * from a consecutive crgpu_draw_indirect_args entry in a buffer.
 *
 * The rule of crgpu_command_list_draw() holds for what is bound. The
 * caller writes the entries before the draws run and moves the buffer to
 * CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT after a GPU write (a buffer of
 * CRGPU_MEMORY_KIND_UPLOAD is readable so from creation). The counts the
 * GPU reads are the caller's to keep; Carrottus GPU never reads, clamps or
 * rewrites them. A `draw_count` of 0 records nothing and returns
 * CR_SUCCESS, after every check.
 *
 * Maps to: vkCmdDrawIndirect (Vulkan); ExecuteIndirect with a draw
 * signature of `stride` (made with the device for the tight stride, on
 * first use for any other) (D3D12); drawPrimitives:indirectBuffer:
 * indirectBufferOffset: once per entry (Metal).
 *
 * @param[in] cmd        Command list being recorded: of a DIRECT allocator
 *                       inside a rendering pass, or a BUNDLE allocator.
 * @param[in] buffer     Buffer holding the entries, created with
 *                       CRGPU_BUFFER_USAGE_INDIRECT. Borrowed until the
 *                       list's submission has finished.
 * @param[in] offset     Byte offset of the first entry: a multiple of 4.
 * @param[in] draw_count Number of draws: at most 1 on a device created
 *                       without CRGPU_FEATURE_MULTI_DRAW_INDIRECT, else at
 *                       most CRGPU_ADAPTER_PROPERTIES::max_draw_indirect_count.
 * @param[in] stride     Byte stride between entries: a multiple of 4 and at
 *                       least sizeof(crgpu_draw_indirect_args), whatever
 *                       `draw_count` is.
 *
 * Checked in this order, on every backend; a refused call records nothing:
 * @retval CR_SUCCESS The draws were recorded, or `draw_count` is 0.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST `cmd` is NULL (or, in Debug,
 *         destroyed).
 * @retval CR_RESULT_GPU_INVALID_BUFFER `buffer` is NULL (or, in Debug,
 *         destroyed).
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE `cmd` is of a COPY or
 *         COMPUTE allocator.
 * @retval CR_RESULT_GPU_COMMAND_OUTSIDE_RENDERING `cmd` is of a DIRECT
 *         allocator and no rendering pass is open on it.
 * @retval CR_RESULT_GPU_NO_GRAPHICS_PIPELINE As crgpu_command_list_draw().
 * @retval CR_RESULT_GPU_GRAPHICS_PIPELINE_KIND As crgpu_command_list_draw().
 * @retval CR_RESULT_GPU_INVALID_BUFFER_USAGE `buffer` wasn't created with
 *         CRGPU_BUFFER_USAGE_INDIRECT.
 * @retval CR_RESULT_GPU_INDIRECT_OFFSET_ALIGNMENT `offset` is not a multiple
 *         of 4.
 * @retval CR_RESULT_GPU_INDIRECT_STRIDE `stride` is not a multiple of 4, or
 *         is less than sizeof(crgpu_draw_indirect_args).
 * @retval CR_RESULT_GPU_INDIRECT_DRAW_COUNT `draw_count` is above 1 on a
 *         device created without CRGPU_FEATURE_MULTI_DRAW_INDIRECT, or above
 *         max_draw_indirect_count.
 * @retval CR_RESULT_GPU_INDIRECT_RANGE `draw_count` is 1 or more and
 *         `offset` + `stride` x (`draw_count` - 1) +
 *         sizeof(crgpu_draw_indirect_args) is past the buffer's size
 *         (worked out with no wrap).
 * @return CR_RESULT_API_SPECIFIC_FAILURE or CR_RESULT_OUT_OF_MEMORY where
 *         D3D12 makes the signature for a stride other than the tight one
 *         and fails, after every check; nothing is recorded.
 */
CR_RESULT crgpu_command_list_draw_indirect(
    crgpu_command_list cmd,
    crgpu_buffer buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride);

/**
 * [CHANGED] returns CR_RESULT (was void); checks added (card S55).
 * @brief Indexed variant of crgpu_command_list_draw_indirect().
 *
 * Reads crgpu_draw_indexed_indirect_args entries and draws with the index
 * buffer bound by crgpu_command_list_set_index_buffer().
 *
 * ... parameters as crgpu_command_list_draw_indirect(), the entry
 * sizeof(crgpu_draw_indexed_indirect_args) ...
 *
 * @return As crgpu_command_list_draw_indirect(), with
 *         CR_RESULT_GPU_NO_INDEX_BUFFER after
 *         CR_RESULT_GPU_GRAPHICS_PIPELINE_KIND, and the stride and range
 *         against sizeof(crgpu_draw_indexed_indirect_args) (20 bytes).
 */
CR_RESULT crgpu_command_list_draw_indexed_indirect(
    crgpu_command_list cmd,
    crgpu_buffer buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride);

#if CRGPU_EXT_MESH_SHADING

/**
 * [CHANGED] returns CR_RESULT (was void); "Does nothing if the bound
 * pipeline is not a mesh-shader pipeline, or if the device was created
 * without CRGPU_FEATURE_MESH_SHADING" [REMOVED], replaced by the results.
 * @brief Records a mesh-shader dispatch against the bound mesh-shader
 * pipeline.
 *
 * ... the mapping and the counts' rule unchanged (each at most 65535, their
 * product at most 1024), now checked on the CPU ...
 *
 * Checked in this order, on every backend; a refused call records nothing:
 * @retval CR_SUCCESS Recorded, or a count is 0.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST `cmd` is NULL (or, in Debug,
 *         destroyed).
 * @retval CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED The device was
 *         created without CRGPU_FEATURE_MESH_SHADING.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE `cmd` is of a COPY,
 *         COMPUTE or BUNDLE allocator.
 * @retval CR_RESULT_GPU_COMMAND_OUTSIDE_RENDERING No pass is open on `cmd`.
 * @retval CR_RESULT_GPU_NO_GRAPHICS_PIPELINE No graphics pipeline is bound
 *         by crgpu_command_list_draw()'s rule.
 * @retval CR_RESULT_GPU_GRAPHICS_PIPELINE_KIND The bound pipeline is not a
 *         mesh pipeline.
 * @retval CR_RESULT_GPU_DISPATCH_GROUP_COUNT A count is above 65535, or
 *         their product above 1024 (worked out in 64 bits).
 */
CR_RESULT crgpu_command_list_dispatch_mesh(
    crgpu_command_list cmd,
    uint32_t group_count_x,
    uint32_t group_count_y,
    uint32_t group_count_z);

#endif // CRGPU_EXT_MESH_SHADING

/**
 * [CHANGED] (comment only; event 6)
 * @brief Binds a compute pipeline for subsequent dispatches.
 *
 * Call outside a rendering pass; inside one it records nothing, on every
 * backend. This bind ends any graphics pipeline's binding. What a dispatch
 * reads is stated once, on crgpu_command_list_dispatch().
 *
 * ... parameters unchanged ...
 */
void crgpu_command_list_bind_compute_pipeline(
    crgpu_command_list cmd,
    crgpu_compute_pipeline pipeline);

/**
 * [CHANGED] returns CR_RESULT (was void); A15's two rules now checked.
 * @brief Records a compute dispatch against the bound compute pipeline.
 *
 * ... the mapping and units unchanged ...
 *
 * What every dispatch reads, on every backend, whatever the graphics API
 * underneath keeps: the compute pipeline, the compute descriptor sets and
 * the compute push constants recorded since the last of
 * crgpu_command_list_begin(), a rendering pass, a copy, blit or clear
 * outside a pass, a debug label begun or ended outside a pass, an
 * acceleration structure build, copy or compacted-size write, and
 * crgpu_command_list_bind_graphics_pipeline(). The pipeline is checked; a
 * dispatch with none records nothing and returns its code. The sets and
 * push constants are not checked: without them what the dispatch does is
 * undefined. So after any of those events, bind again (a cost on backends
 * that would have kept the state, paid for one behaviour everywhere).
 *
 * Each group count is at most 65535, checked; a 0 in any records nothing
 * and returns CR_SUCCESS.
 *
 * @param[in] cmd           Command list being recorded: of a DIRECT or
 *                          COMPUTE allocator, outside a rendering pass.
 * @param[in] group_count_x Number of workgroups in X, at most 65535.
 * @param[in] group_count_y Number of workgroups in Y, at most 65535.
 * @param[in] group_count_z Number of workgroups in Z, at most 65535.
 *
 * Checked in this order, on every backend; a refused call records nothing:
 * @retval CR_SUCCESS Recorded, or a count is 0.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST `cmd` is NULL (or, in Debug,
 *         destroyed).
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE `cmd` is of a COPY or
 *         BUNDLE allocator.
 * @retval CR_RESULT_GPU_COMMAND_INSIDE_RENDERING A pass is open on `cmd`.
 * @retval CR_RESULT_GPU_NO_COMPUTE_PIPELINE No compute pipeline is bound by
 *         the rule above.
 * @retval CR_RESULT_GPU_DISPATCH_GROUP_COUNT A count is above 65535.
 */
CR_RESULT crgpu_command_list_dispatch(
    crgpu_command_list cmd,
    uint32_t group_count_x,
    uint32_t group_count_y,
    uint32_t group_count_z);

/**
 * [CHANGED] (one refusal added, in a place of its own)
 * @brief Records a compute dispatch whose group counts the GPU reads ...
 *
 * ... unchanged, but the results, checked in this order:
 * @retval CR_SUCCESS The dispatch was recorded.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST ...
 * @retval CR_RESULT_GPU_INVALID_BUFFER ...
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE ...
 * @retval CR_RESULT_GPU_COMMAND_INSIDE_RENDERING ...
 * @retval CR_RESULT_GPU_NO_COMPUTE_PIPELINE [ADDED] No compute pipeline is
 *         bound by crgpu_command_list_dispatch()'s rule. (Metal recorded
 *         nothing and returned CR_SUCCESS; Vulkan and D3D12 reached the
 *         driver.)
 * @retval CR_RESULT_GPU_INVALID_BUFFER_USAGE ...
 * @retval CR_RESULT_GPU_INDIRECT_OFFSET_ALIGNMENT ...
 * @retval CR_RESULT_GPU_INDIRECT_RANGE ...
 */
CR_RESULT crgpu_command_list_dispatch_indirect(
    crgpu_command_list cmd,
    crgpu_buffer buffer,
    uint64_t offset);

/**
 * [CHANGED] (comment only; event 4)
 * @brief Replays a recorded, closed bundle into a direct command list that
 * is recording.
 *
 * ... unchanged, but "Afterwards, the direct list's pipeline, vertex and
 * index buffers, descriptor sets and push constants are undefined until set
 * again" gains: "a draw before the pipeline and (indexed) the index buffer
 * are bound again is refused (crgpu_command_list_draw())." ...
 */

/*
 * ---------------------------------------------------------------------------
 * crcore.h
 * ---------------------------------------------------------------------------
 *
 * 376 built as held (A15's comment):
 *
 *	/** A command recorded outside a rendering pass that only a pass can
 *	 * hold. Since 0.4.0. * /
 *	CR_RESULT_GPU_COMMAND_OUTSIDE_RENDERING                   = 376,
 *
 * [ADDED] (Q4 a), after 378. Why: each a fact the caller acts on (bind a
 * pipeline, bind one of the other kind, bind an index buffer, split a
 * dispatch, fix a stride, split a draw count). ABI: seven values
 * appended. Tests: crgpu_objects_test 1 to 7.
 *
 *	/** A draw or mesh dispatch with no graphics pipeline bound since its
 *	 * pass began (crgpu_command_list_draw()'s rule). Since 0.4.0. * /
 *	CR_RESULT_GPU_NO_GRAPHICS_PIPELINE                        = 379,
 *	/** A dispatch with no compute pipeline bound by
 *	 * crgpu_command_list_dispatch()'s rule. Since 0.4.0. * /
 *	CR_RESULT_GPU_NO_COMPUTE_PIPELINE                         = 380,
 *	/** A draw with a mesh pipeline bound, or a mesh dispatch with a
 *	 * pipeline of vertex input bound. Since 0.4.0. * /
 *	CR_RESULT_GPU_GRAPHICS_PIPELINE_KIND                      = 381,
 *	/** An indexed draw with no index buffer bound, or one bound through a
 *	 * view of a stride other than 2 or 4. Since 0.4.0. * /
 *	CR_RESULT_GPU_NO_INDEX_BUFFER                             = 382,
 *	/** A dispatch's group count above the call's limit. Since 0.4.0. * /
 *	CR_RESULT_GPU_DISPATCH_GROUP_COUNT                        = 383,
 *	/** An indirect command's stride is not a multiple of 4, or is smaller
 *	 * than its arguments. Since 0.4.0. * /
 *	CR_RESULT_GPU_INDIRECT_STRIDE                             = 384,
 *	/** An indirect command's count is above what the device takes in one
 *	 * call. Since 0.4.0. * /
 *	CR_RESULT_GPU_INDIRECT_DRAW_COUNT                         = 385,
 *
 * cr_result_str() (crcore.c) names 376 and the seven.
 *
 * ---------------------------------------------------------------------------
 * Where it is done (not in a header)
 * ---------------------------------------------------------------------------
 *
 * - src/gpu/crgpushared.h: crgpu___bound_state (above), static_assert'd 4
 *   bytes and plain data; inline event helpers (crgpu___bound_begin(),
 *   _pass_begin(), _compute_break(), _bundle_executed(),
 *   _graphics_bound(kind), _compute_bound(), _index_bound(stride)) and the
 *   checks, each its own `if`:
 *
 *	CR_RESULT crgpu___internal_check_draw(
 *	    CRGPU_COMMAND_LIST_TYPE type, bool rendering,
 *	    const crgpu___bound_state *bound, bool indexed);
 *	CR_RESULT crgpu___internal_check_dispatch(
 *	    CRGPU_COMMAND_LIST_TYPE type, bool rendering,
 *	    const crgpu___bound_state *bound);
 *	CR_RESULT crgpu___internal_check_draw_indirect(
 *	    const CRGPU_ADAPTER_PROPERTIES *limits, CRGPU_FEATURES features,
 *	    const crgpu_buffer_desc *buffer, uint64_t offset,
 *	    uint32_t draw_count, uint32_t stride, uint32_t entry_size);
 *
 *   The range without a wrap: offset > size refuses; then for count n >= 1
 *   the bytes needed are (n - 1) x stride + entry, each step checked
 *   against size - offset (a 32 x 32-bit product fits 64 bits, so only the
 *   sum is guarded). crgpu___internal_check_dispatch_indirect() gains the
 *   bound record after its pass check.
 * - Each backend: its list struct gains `crgpu___bound_state _bound;`
 *   beside its hot fields (Metal: beside _active_render_encoder; Vulkan:
 *   beside _command_buffer_vk; D3D12: beside _d3d_cmd_list); each event
 *   call writes it at the place named in "The tracked state"; each draw and
 *   dispatch: cmd null, CR_STALE, (indirect) buffer null, CR_STALE, (mesh)
 *   the feature, the shared check, then a 0 count returns CR_SUCCESS, then
 *   the API call.
 *   - Metal: _bound_pipeline and _bound_compute_pipeline cleared where the
 *     record is ("Found in the code" 2 of A15, cleared at begin); the
 *     0-to-1 workgroup clamp in dispatch and dispatch_mesh removed (A15's
 *     "Found in the code" 6); bind_compute_pipeline returns before opening
 *     an encoder while _active_render_encoder is set (pass 1, 4);
 *     bind_graphics_pipeline outside a pass records nothing (pass 1, 5);
 *     the draws' and dispatches' `if (!encoder || !pipeline) return;`
 *     become unreachable (the record refuses first) and are asserted.
 *   - Vulkan: no API change; the record alone.
 *   - D3D12: the draw_indirect signature's failure returned (pass 1, 3).
 * - No new file; no allocation; no lock.
 *
 * ---------------------------------------------------------------------------
 * Found in the code (each to be carded when the user takes this draft)
 * ---------------------------------------------------------------------------
 *
 *  1. Metal's compute bind inside a pass ends the pass (pass 1, 4): fixed
 *     here.
 *  2. Metal's graphics bind outside a pass never reaches the next pass's
 *     encoder (pass 1, 5): held by the rule here.
 *  3. Index views of stride other than 2 or 4 behave three ways (pass 1,
 *     6): held here at the draw; whether set_index_buffer should refuse is
 *     Q5's card.
 *  4. D3D12's lazily made draw signature fails silently (pass 1, 3): said
 *     here.
 *  5. crgpu_command_list_set_depth_bounds() asserts on the caller's input
 *     on a device without the feature (crgpuvulkancommand.c 783,
 *     crgpumetalcommand.m 631; CONVENTIONS.md allows no assert there): a
 *     card.
 *  6. Metal's begin_rendering asserts on a layered pass without the
 *     feature (crgpumetalrenderpass.m 448): the same, a card.
 *
 * ---------------------------------------------------------------------------
 * Records when built
 * ---------------------------------------------------------------------------
 *
 * carrottus/CLAUDE.md: a bullet under "Carrottus GPU conventions" (draws
 * and dispatches checked: the tracked record and its events, the codes);
 * the indirect dispatch bullet's "unchecked" sentence replaced.
 * CRGPU_DEVICE_TODO.md, "Indirect dispatch": S55 and S56 built, each
 * machine's run, the measured figures. The workspace's DECISIONS.md: cards
 * for "Found in the code" 3, 5, 6 and Q5's and Q6's options if a caller asks.
 * The callers' own records are the callers' to write.
 */
