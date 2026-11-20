/*
 * crgpu_concurrent_compute_draft.h - Carrottus GPU's compute on Metal runs
 * dispatches side by side, as Vulkan and D3D12 do, and the caller's
 * barriers become Metal's own memory barriers (card S60).
 *
 * APPROVED by the user 2026-11-20 after a second seven-pass critique, every
 * question as recommended (S60's Q1 a included: every resource and heap
 * stays tracked, narrowing the card's "untracked resources"; Q2 as
 * recommended in this version, c: pass 10, 1 changed it from version 2's
 * a). Version 3, 2026-11-20 (version 1, seven critique passes, version 2;
 * then passes 8 to 14, each fixed before the next); not built. Frozen from
 * here (ROADMAP.md, section 1).
 * Card S60 decided 2026-11-20 (a): "a Metal compute encoder of
 * MTLDispatchTypeConcurrent, Carrottus GPU's barriers turned into Metal's
 * memoryBarrierWithScope: (untracked resources where the caller's barriers
 * already order them)". Found 2026-11-19 by a caller's measurement: 100
 * small compute dispatches in one command list cost half of 100 lists' (1.2
 * against 2.5 ms on the M5 Pro), not the sixth expected, since Metal's
 * serial encoder runs one dispatch after another.
 *
 * NO PUBLIC DECLARATION CHANGES. The change is the Metal backend's, and an
 * early return of a barrier inside a pass in all three backends (Q2 c);
 * what this draft changes in crgpu.h is comments: the contract a caller
 * already has (ordering within a list is the caller's barriers', on every
 * backend) stated where it was implied, a barrier inside a pass taken out
 * of it, and the "Maps to" lines that said Metal records no barrier. ABI:
 * none. It is a draft because it changes what a caller's work does on Metal
 * (dispatches with no barrier between them now overlap there, as they
 * always could on the other two), and the card asked for one.
 *
 * No version raise while carrottus' 0.4.0 is untagged (card X2).
 *
 * Written unattended on the Mac (Apple M5 Pro, macOS 27) against
 * include/crgpu.h (crgpu_command_list_image_barrier() and its GENERAL to
 * GENERAL rule, 3360 to 3400; crgpu_buffer_barrier, 3500 to 3545;
 * crgpu_command_list_pipeline_barrier(), 3560 to 3613; the dispatches,
 * 7278 and 7349), the Metal backend (crgpumetalcommand.m: the compute
 * encoder made at 462 to 484 with -computeCommandEncoder; dispatch 954;
 * crgpumetalrenderpass.m: the three image barriers, 35 to 95, no-ops that
 * print on an unknown layout; pipeline_barrier, 589 to 630, a no-op that
 * relies on hazard tracking; crgpumetalheap.m, heaps made tracked;
 * crgpumetalraytracing.m's acceleration structure barrier), carrottus/CLAUDE.md
 * ("Metal heaps are MTLHeapTypePlacement and hazard tracked, because this
 * backend's barriers do nothing"), and the macOS SDK's headers, read here:
 * MTLCommandBuffer.h (MTLDispatchType, 224 to 238: "Serial: dispatches are
 * executed in dispatched order"; "Concurrent: executed in parallel with
 * each other"; computeCommandEncoderWithDispatchType:, 397 to 402: "On
 * devices that do not support concurrent command encoders, this call is
 * equivalent to computeCommandEncoder"), MTLComputeCommandEncoder.h
 * (memoryBarrierWithScope: and memoryBarrierWithResources:count:, 315 to
 * 326: "ensures that all dispatches in the encoder have completed execution
 * and their side effects are visible to subsequent dispatches in that
 * encoder. Calling barrier on a serial encoder is allowed, but ignored"),
 * MTLRenderCommandEncoder.h (memoryBarrierWithScope:afterStages:beforeStages:,
 * 1032 to 1043, macOS 10.14, iOS 16), MTLCommandEncoder.h (MTLBarrierScope:
 * Buffers, Textures, RenderTargets (macOS only)), MTLResource.h
 * (MTLHazardTrackingMode, 91 to 155: "Tracked: command encoder dependencies
 * for this resource are tracked automatically"; "Untracked: ... tracked
 * manually with MTLFence"). carrottus 5b71f5d, 0.4.0, untagged; passes 8 to
 * 14 rechecked against ad3d5ab (its sources those of 5b71f5d), with
 * crgpuvulkancommand.c's pipeline_barrier (897 to 920), crgpumetalraytracing.m
 * (212 to 216, 614 to 627) and the Vulkan SDK 1.4.363.0's validusage.json
 * (vkCmdPipelineBarrier, read here). Not built, not in any target, not
 * compilable.
 *
 * ---------------------------------------------------------------------------
 * Revision notes (version 1 written, then seven passes, each fixed before
 * the next; one lens a pass)
 * ---------------------------------------------------------------------------
 *
 * Pass 1 (correctness against the built code).
 *  1. Wrong in version 1: it changed pipeline_barrier alone. crgpu.h's
 *     image barrier from GENERAL to GENERAL is "not a no-op ... it orders
 *     its accesses" (3385 to 3390), and Metal's three image barriers are
 *     no-ops too. They take the same mapping now.
 *  2. Found: the barrier's rule says an UNORDERED_ACCESS to UNORDERED_ACCESS
 *     entry is the caller's "between two draws or dispatches" (3567 to
 *     3577). Between two draws in one pass Metal's tracking orders nothing
 *     (it tracks between encoders, MTLResource.h's "command encoder
 *     dependencies"), so a storage write read by a later draw of the same
 *     pass was unordered on Metal before this card, with no barrier
 *     recorded. Inside a pass the barrier now maps to the render encoder's
 *     memoryBarrierWithScope:afterStages:beforeStages:. Q2.
 *  3. Found: crgpumetalrenderpass.m's image barriers print a line on a
 *     layout value outside the enum (49, 69, 90): a print on the caller's
 *     input, which CONVENTIONS.md allows nowhere. Removed here: such a
 *     barrier records nothing and prints nothing (the call is void), the
 *     same on every backend once they agree (a card: "Found in the code" 1).
 *  4. Checked and kept: the compute encoder is the only one whose dispatch
 *     type changes. Blit encoders keep their commands' order on tracked
 *     resources; render encoders keep primitive order for attachments.
 *  5. Checked: an indirect dispatch's arguments written by a dispatch in
 *     the same encoder are read after a barrier (A15's test 2 records
 *     UNORDERED_ACCESS to INDIRECT_ARGUMENT between them): with a concurrent
 *     encoder that barrier is now memoryBarrierWithScope:Buffers, and the
 *     test is the regression check that it suffices.
 *
 * Pass 2 (the C API's conventions: no hidden policy, no default that
 * decides, errors as codes).
 *  1. No barrier is added on the caller's behalf: dispatches with none
 *     between them may overlap, as the header already says the barrier is
 *     "never recorded on the caller's behalf" (3576). Version 1 put a
 *     barrier after every indirect dispatch's argument read "to be safe":
 *     removed, a hidden policy.
 *  2. No flag, mode or query for the dispatch type: concurrency is what
 *     every backend gives between barriers, so it is the rule, not an
 *     option only Metal acts on.
 *  3. No code changes; every call keeps its results.
 *
 * Pass 3 (the platform APIs on all three backends).
 *  1. Vulkan: dispatches in one command buffer with no barrier between them
 *     may overlap (the specification's execution dependencies: commands
 *     start in submission order and may complete out of it); a barrier of
 *     COMPUTE_SHADER to COMPUTE_SHADER with shader write to read access
 *     orders them. Unchanged.
 *  2. D3D12: dispatches overlap unless a UAV barrier (or a transition) is
 *     between them. Unchanged.
 *  3. Metal: the serial encoder orders each dispatch after the one before;
 *     the concurrent one doesn't, and its memoryBarrierWithScope: waits for
 *     every dispatch before it in the encoder ("all dispatches in the
 *     encoder have completed") for the scope's resources. Tracked resources
 *     are still ordered between encoders (a compute encoder then a render
 *     or blit one), so a barrier recorded with no compute or render encoder
 *     open needs nothing more. The scope (Buffers, Textures) from the
 *     barrier's entries: buffers if any buffer entry does something,
 *     textures if any image entry does (Q3).
 *  4. A render encoder's barrier: memoryBarrierWithScope:afterStages:
 *     beforeStages: with the stages the encoder's pipelines can write and
 *     read storage in (vertex and fragment; object and mesh on a device
 *     with mesh shading), both sides, since the entry names no stage.
 *     MTLBarrierScopeRenderTargets is not used (macOS only, and attachments
 *     are never written through storage slots).
 *
 * Pass 4 (industry practice).
 *  1. MoltenVK (as remembered): Vulkan compute maps to a concurrent
 *     encoder where the device has it, and vkCmdPipelineBarrier inside a
 *     compute encoder to memoryBarrierWithScope:/withResources:; render
 *     barriers to the render encoder's form. The same mapping as here.
 *  2. WebGPU over Metal (Dawn, as remembered): one serial compute pass
 *     encoder, its implicit barriers between dispatches, which WebGPU's
 *     model requires (usage scopes per dispatch); not the model here, where
 *     barriers are the caller's.
 *  3. SDL_GPU's Metal backend (as remembered): serial compute encoders,
 *     its API having no barriers. NVRHI over Metal: none shipped. Kept the
 *     MoltenVK shape: it is the one whose public model matches ours.
 *
 * Pass 5 (performance).
 *  1. The gain is the card's: independent dispatches overlap; measured when
 *     built (Tests, 3) against the caller's figures (1.2 ms for 100 small
 *     dispatches in one list against 2.5 for 100 lists, the expectation
 *     near a sixth).
 *  2. The cost: each barrier inside a compute encoder is a full wait for
 *     that scope, where the serial encoder waited after every dispatch;
 *     never more waits than before. memoryBarrierWithResources: would wait
 *     on fewer resources but needs the entries' Metal objects in a list each
 *     barrier (a stack array of 16, looped for more: no allocation); Q3
 *     keeps it on record.
 *  3. Hazard tracking's own CPU cost (Metal's bookkeeping per tracked
 *     resource and encoder) is kept: Q1.
 *  4. No allocation: one Objective-C message per barrier, made into the
 *     open encoder; cr_noalloc_tests holds it.
 *
 * Pass 6 (tests: each failing first, on every backend).
 *  1. Version 1's timing test asserted a speed-up: timing on a shared GPU
 *     can't fail reliably first. Now the measurement is recorded, not
 *     asserted (Tests, 3), and the test that fails first is the ordering
 *     one: a chain of 64 dispatches each reading the last one's output,
 *     with a barrier between each, built in two steps (the concurrent
 *     encoder first, the barrier mapping after): run between the steps, on
 *     the concurrent encoder with no-op barriers, its sum is expected
 *     wrong; if it is right by chance on the M5 Pro, that is recorded
 *     rather than claimed as a failure seen.
 *  2. The render barrier's test fails first today (no barrier recorded in
 *     a pass on Metal): a draw writing a storage buffer, the barrier, a
 *     draw reading it, the second's output read back. Whether Metal's
 *     current behaviour shows a stale value on the M5 Pro is to be seen;
 *     recorded either way.
 *  3. Vulkan and D3D12 are unchanged: their runs of the new ordering tests
 *     are owed on Windows and Fedora (they pass there already, the barriers
 *     real), as a check that the tests hold the contract on every backend.
 *
 * Pass 7 (what callers above see).
 *  1. No API change; no rebuild of a caller's bindings.
 *  2. A caller whose compute work on Metal leaned on the serial encoder
 *     (two dependent dispatches with no barrier between) was outside the
 *     contract on Vulkan and D3D12 already; on Metal it now races too. Such
 *     a caller is found by Vulkan's synchronization validation (owed on
 *     Windows and Fedora for every caller's GPU suite before this is
 *     built on the Mac: Q4) and, on Metal, by comparing every caller test's
 *     printed figures and hashes before and after the build, bit for bit.
 *  3. A caller that measures its own compute on Metal sees its numbers
 *     change; recorded in its own TODO.
 *
 * ---------------------------------------------------------------------------
 * Revision notes, second round (version 2, then passes 8 to 14, each fixed
 * before the next; version 3)
 * ---------------------------------------------------------------------------
 *
 * Pass 8 (correctness against today's code, all three backends' sources).
 *  1. Checked: the compute encoder is made at crgpumetalcommand.m 462 to 482
 *     with -computeCommandEncoder; the image barriers print at
 *     crgpumetalrenderpass.m 48 and its two twins; pipeline_barrier is the
 *     no-op at 589 to 630; heaps are tracked (crgpumetalheap.m 73, 145), and
 *     buffers and images made from the device take Metal's default,
 *     tracked.
 *  2. Found: the acceleration structure barrier (crgpumetalraytracing.m 614
 *     to 627) stays a no-op: every build, copy and size write is in an
 *     acceleration structure encoder of its own (212 to 216 end any compute
 *     or blit encoder first), so tracking orders it against the concurrent
 *     compute encoder after it. Said in "What changes".
 *  3. Found: a barrier with only a blit encoder open: blit encoders aren't
 *     concurrent and tracking orders the encoder after; nothing recorded.
 *     Said ("with no compute encoder open, nothing").
 *  4. Found: Vulkan's pipeline_barrier records vkCmdPipelineBarrier inside a
 *     pass as outside one (crgpuvulkancommand.c 897 to 920 never looks at
 *     _rendering_active): pass 10, 1.
 *
 * Pass 9 (conventions: no hidden policy, no default, codes, append-only).
 *  1. Found: version 2 named what a caller's work was for (its header and
 *     "Why" called the caller's dispatches by their rendering purpose):
 *     Carrottus has no rendering domain and assumes nothing of its caller.
 *     Reworded to dispatches.
 *  2. Checked for Q2 (c): a barrier inside a pass records nothing on every
 *     backend, an early return of a void call at the same point in all
 *     three; never emulated by ending and beginning the pass again (a
 *     decision for the caller, and a pass's load and store it never asked
 *     for).
 *  3. No code, no struct, no result changes.
 *
 * Pass 10 (the platform APIs).
 *  1. Read here (validusage.json): VUID-vkCmdPipelineBarrier-None-09553 (no
 *     vkCmdPipelineBarrier inside a render pass instance begun with
 *     vkCmdBeginRendering without dynamicRenderingLocalRead or a tile image
 *     feature), -09554 (with neither, no buffer or image barrier there) and
 *     -srcStageMask-09556 (framebuffer-space stages only). The Vulkan device
 *     enables none of these. So crgpu.h's barrier "between two draws" of a
 *     pass has no Vulkan mapping, and version 2's Q2 (a) would have Metal
 *     order what Vulkan can't. Changed: Q2 recommends (c), a barrier inside
 *     a pass records nothing on every backend, and crgpu.h says a draw never
 *     reads what another draw of the same pass wrote through a storage slot
 *     (end the pass, barrier, begin another). The render-encoder barrier and
 *     its stages are gone from "Where it is done".
 *  2. From memory, not read here: Apple's documentation of
 *     memoryBarrierWithScope:afterStages:beforeStages: says a tile-based GPU
 *     can't order a later draw's vertex stage after an earlier draw's
 *     fragment stage in one pass; another reason for (c).
 *  3. Read here: MTLComputeCommandEncoder.h 316 to 327 (a barrier "between
 *     currently dispatched kernels in a concurrent compute command encoder
 *     and any subsequent ones"), MTLCommandBuffer.h 398 to 399 (a device
 *     without concurrent encoders gets a serial one, its barriers ignored,
 *     its order kept).
 *  4. D3D12 (as remembered): a UAV barrier between two draws is legal there;
 *     narrowed by (c) like Metal.
 *
 * Pass 11 (industry practice).
 *  1. WebGPU (as remembered): no barriers; a render pass is one usage scope,
 *     and a storage buffer written by one draw and read by another of the
 *     same pass has no ordering: (c)'s rule.
 *  2. MoltenVK (as remembered): maps an in-pass vkCmdPipelineBarrier to the
 *     render encoder's memory barrier where Metal has it, beyond Vulkan
 *     core's own rule; not followed.
 *  3. Unreal's RHI (as remembered): overlapping UAV writes are declared for
 *     compute (BeginUAVOverlap), not between draws of a pass. Kept.
 *
 * Pass 12 (performance).
 *  1. (c) adds nothing per draw (version 2's render barrier is gone).
 *  2. A barrier inside a compute encoder is one message; with no encoder
 *     open, nothing. Concurrency's gain is Tests 3's figure, by name.
 *  3. The early return in a pass is one load of the list's pass flag on
 *     each backend.
 *
 * Pass 13 (tests: each failing first, figures by name).
 *  1. Test 4 rewritten for (c): a barrier inside a pass records nothing on
 *     every backend; it fails first on Vulkan, where validation reports
 *     09553 today (owed on Windows and Fedora); on Metal and D3D12 nothing
 *     can be seen, said rather than claimed.
 *  2. Named: crgpu_objects_test's objects_compute_chain (1),
 *     objects_compute_fan_out (2), objects_compute_measure (3, Release, run
 *     by hand, not a ctest), objects_barrier_in_pass (4),
 *     objects_compute_chain_image (5); A15's indirect cases (6); the figures
 *     into CRGPU_DEVICE_TODO.md by those names.
 *  3. Vulkan and D3D12 change now (the early return): Vulkan written and
 *     type-checked here, D3D12 written, not compiled.
 *
 * Pass 14 (callers above, and the ABI list).
 *  1. A caller that records a barrier inside a pass: today outside
 *     Vulkan's rules, nothing on Metal, real on D3D12; now nothing on every
 *     backend. One that leaned on D3D12's sees a draw read a stale storage
 *     value there; Q4's runs under Vulkan's validation find each such
 *     barrier (09553), listed for that caller's TODO. No caller is named
 *     here (./check.sh caller-names passes).
 *  2. The three image barriers take the same early return inside a pass: a
 *     layout change there was outside Vulkan's rules already
 *     (VUID-vkCmdPipelineBarrier-oldLayout-01181, read here), and Metal's
 *     were no-ops. A caller that moved a layout inside a pass is found the
 *     same way as in 1.
 *  3. A caller's compute on Metal: as pass 7, 2 and 3.
 *  4. The ABI list: none. crgpu.h's comments: pipeline_barrier (a rule
 *     added, the Metal mapping), crgpu_buffer_barrier's "Maps to", the image
 *     barriers' GENERAL to GENERAL rule, crgpu_command_list_dispatch().
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * Vulkan and D3D12 run dispatches side by side unless a barrier orders
 * them; Metal's backend made every compute encoder serial and its barriers
 * no-ops, relying on Metal's hazard tracking. So on Metal independent work
 * never overlaps (the caller's 100 small dispatches, the card's figures),
 * and the caller's barriers, which the contract makes the only ordering,
 * mean nothing there. One behaviour on every backend, and the caller's
 * barriers real on all three.
 *
 * ---------------------------------------------------------------------------
 * What changes, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * Metal: the compute encoder is made with
 * -computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent.
 * crgpu_command_list_pipeline_barrier() records, where any of its entries
 * does something by crgpu.h's rules (states that differ, or the two equal
 * pairs that order), memoryBarrierWithScope: into an open compute encoder
 * (Buffers and/or Textures by the entries), and nothing otherwise (Metal
 * orders encoders on a tracked resource itself). The three image barriers
 * do the same for a GENERAL to GENERAL barrier (Textures). The acceleration
 * structure barrier stays nothing (each build is an encoder of its own).
 * Resources and heaps stay hazard tracked (Q1). Every backend: a barrier
 * inside a pass records nothing (Q2 c; Vulkan's
 * VUID-vkCmdPipelineBarrier-None-09553). crgpu.h's comments say what holds
 * on every backend: work recorded with no barrier between may overlap, what
 * each barrier orders, and that no barrier orders two draws of one pass.
 *
 * ---------------------------------------------------------------------------
 * The contract the caller sees (unchanged in substance, stated once)
 * ---------------------------------------------------------------------------
 *
 * On every backend, within a command list:
 *  - Commands start in recording order. Two dispatches with no barrier
 *    between them may run at the same time and finish in any order: a
 *    write one makes is not visible to the other. Two draws of one pass
 *    likewise, and no barrier orders them: a barrier inside a pass records
 *    nothing (Q2 c), so a draw that reads what an earlier draw wrote through
 *    a storage slot needs the pass ended, a barrier and another pass.
 *  - A barrier entry that does something (crgpu_command_list_pipeline_barrier())
 *    orders every access to its resource recorded before it, of the stage
 *    its state_before names, before every access after it, of the stage its
 *    state_after names, and makes the writes visible.
 *  - Between a pass and the work after it, and a copy and the work after
 *    it, the caller's barrier is what orders a resource both touch, as on
 *    Vulkan; a backend that orders more (Metal's tracking between encoders)
 *    is not something to rely on.
 *  - Between submissions on one queue, buffers decay to COMMON
 *    (crgpu_buffer_barrier): unchanged.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 *  - Metal (the macOS SDK's headers, read here): pass 3, 3 and 4.
 *  - Vulkan and D3D12 (their execution models as the backends already
 *    translate them): pass 3, 1 and 2.
 *  - MoltenVK, Dawn, SDL_GPU (as remembered): pass 4.
 * Hazel is not compared (Carrottus is not measured against an engine).
 *
 * ---------------------------------------------------------------------------
 * Left out, and why
 * ---------------------------------------------------------------------------
 *
 *  - Untracked resources and heaps: Q1 (b), not recommended in this step.
 *  - A dependency across command buffers or queues on Metal (MTLFence or
 *    MTLEvent from a barrier): the queue's order and the semaphores already
 *    hold it; unchanged (pipeline_barrier's Metal comment said so).
 *  - Barriers of a stage mask on the public API: the states carry what is
 *    needed (Vulkan's backend maps them; Metal's scopes don't need stages
 *    in compute).
 *
 * ---------------------------------------------------------------------------
 * Questions (each with options and a recommendation)
 * ---------------------------------------------------------------------------
 *
 * Q1. UNTRACKED RESOURCES (the card's "untracked resources where the
 *     caller's barriers already order them").
 *     (a) RECOMMENDED for this step. Every resource and heap stays tracked.
 *         The overlap the card asked for is the concurrent encoder's, inside
 *         an encoder, where Metal's tracking orders nothing anyway; tracking
 *         still orders encoders and command buffers, which the contract
 *         lets the caller's barriers do but where Carrottus GPU has no
 *         barrier to place: a barrier recorded between encoders has no open
 *         encoder for an MTLFence's update and wait (the gap crgpu.h's
 *         pipeline_barrier comment already names), and crgpu_buffer_barrier's
 *         "ordered after every earlier submission" would need an MTLEvent
 *         between command buffers on untracked buffers. Untracked resources
 *         become their own measured step if tracking's CPU cost shows.
 *         This narrows the card's parenthesis; the user's yes is needed.
 *     (b) Untracked now: heaps and resources made untracked, an MTLFence per
 *         list updated at each encoder's end where a barrier was recorded
 *         and waited on at the next encoder's start, and an MTLEvent wait at
 *         a submission's start for its buffers' decay. Larger, and every
 *         ordering the caller didn't write becomes a race on Metal at once.
 *
 * Q2. A BARRIER INSIDE A PASS.
 *     (a) Recorded on Metal: the render encoder's
 *         memoryBarrierWithScope:afterStages:beforeStages:, as crgpu.h's
 *         "between two draws" says (pass 1, 2). Version 2's recommendation;
 *         Vulkan can't keep it (pass 10, 1: VUID-vkCmdPipelineBarrier-None-09553
 *         without dynamicRenderingLocalRead), so one backend would order
 *         what another can't.
 *     (b) Left a no-op on Metal, as now: the contract's "between two draws"
 *         false on one backend, and Vulkan's barrier outside its rules.
 *     (c) RECOMMENDED (since version 3). Records nothing on every backend,
 *         and crgpu.h's rule changes to say so: no barrier orders two draws
 *         of one pass; a draw reading what an earlier draw wrote through a
 *         storage slot needs the pass ended, a barrier and a new pass. The
 *         narrowest contract every API keeps (Vulkan core, Metal's
 *         tile-based GPUs, WebGPU's usage scopes); D3D12's UAV barrier
 *         between draws is given up. A rule change of a comment, no
 *         declaration.
 *
 * Q3. SCOPE OR RESOURCES.
 *     (a) RECOMMENDED. memoryBarrierWithScope: (the card's words): one
 *         message, Buffers and/or Textures from the entries.
 *     (b) memoryBarrierWithResources:count: with the entries' Metal objects:
 *         waits on fewer resources, a loop over the entries per barrier;
 *         worth measuring if a caller's barrier-heavy compute shows Q3 (a)'s
 *         waits.
 *
 * Q4. ORDER OF THE BUILD.
 *     (a) RECOMMENDED. Before building on the Mac, each caller's GPU suite
 *         runs on Vulkan under synchronization validation (Windows or
 *         Fedora), so a missing barrier is found by the tool made for it,
 *         not by a race on Metal; then the build, with every caller test's
 *         figures compared bit for bit before and after.
 *     (b) Build on the Mac first and rely on the bit-for-bit comparison: a
 *         race that happens not to show on the M5 Pro is missed.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * crgpu_objects_test, every backend (Metal built and run here; Vulkan and
 * D3D12 owed, their code unchanged):
 *  1. A chain: 64 dispatches of a 256-thread shader, dispatch i reading
 *     buffer slot i - 1 (4096 values) and writing slot i (each value + 1),
 *     a pipeline_barrier UNORDERED_ACCESS to UNORDERED_ACCESS between each;
 *     slot 63 read back: every value 64. Run 20 times a configuration.
 *  2. Fan-out: 100 dispatches each writing its own buffer, no barrier, then
 *     one barrier and a dispatch summing the 100: the sum exact.
 *  3. Measured, not asserted (Release, no validation, the M5 Pro): test 2's
 *     100 dispatches of 64 threads, one list, submit to idle, three rounds
 *     of the mean of 50 after 10, before and after; the figures into
 *     CRGPU_DEVICE_TODO.md beside the caller's.
 *  4. objects_barrier_in_pass (Q2 c): a barrier UNORDERED_ACCESS to
 *     UNORDERED_ACCESS recorded inside a pass between two draws records
 *     nothing, on every backend: Vulkan's validation (and synchronization
 *     validation) silent where it reports VUID-vkCmdPipelineBarrier-None-09553
 *     today (fails first there; owed on Windows and Fedora); D3D12's debug
 *     layer silent; on Metal nothing to see (said, not claimed). Then the
 *     same two draws in two passes with the barrier between them: every
 *     pixel the first draw's value, on every backend.
 *  5. Images: test 1 with a storage image and the image barrier GENERAL to
 *     GENERAL between each dispatch.
 *  6. A15's indirect dispatch tests, unchanged, pass on the concurrent
 *     encoder (test 2's barrier to INDIRECT_ARGUMENT).
 *  7. cr_noalloc_tests: test 1's frame, 0 of ours.
 *  8. Every caller's GPU suite, Debug and Release, under Metal's API and
 *     shader validation with warnings fatal: its printed figures and hashes
 *     the same as before the build, line for line (Q4).
 * Seen failing first: 4 on Vulkan (09553 reported today), owed there; 1 and
 * 5 between the build's two steps on Metal (pass 6, 1), recorded as seen or
 * not.
 * By name (pass 13, 2): 1 objects_compute_chain, 2 objects_compute_fan_out,
 * 3 objects_compute_measure, 4 objects_barrier_in_pass, 5
 * objects_compute_chain_image.
 * Where each runs: the Mac (macos-metal, Debug and Release, Metal's
 * validation with warnings fatal, -asan, -tsan). Vulkan written and
 * type-checked here, D3D12 written, not compiled (the in-pass early return).
 * Owed: the new tests on Windows 11 (D3D12 and Vulkan, synchronization
 * validation) and Fedora.
 *
 * ---------------------------------------------------------------------------
 * crgpu.h (comments only)
 * ---------------------------------------------------------------------------
 */

/**
 * [CHANGED] (comment only)
 * @brief Records an execution and memory barrier moving buffers and images
 * between resource states.
 *
 * ... the rules unchanged, and one stated before them:
 *
 * - Within a command list, on every backend, commands start in recording
 *   order and may overlap: two dispatches, or two draws of a pass, with no
 *   barrier between them may run at the same time. This call is what orders
 *   one dispatch before another, and a pass before the work after it.
 * - Inside a rendering pass this call records nothing, on every backend:
 *   no barrier orders two draws of one pass. A draw that reads what an
 *   earlier draw wrote through a storage slot needs the pass ended, this
 *   call, and another pass. [CHANGED] "between two draws or dispatches" in
 *   the UNORDERED_ACCESS rule becomes "between two dispatches, or between
 *   two passes or a pass and a dispatch".
 *
 * Maps to: vkCmdPipelineBarrier (Vulkan); ID3D12GraphicsCommandList::
 * ResourceBarrier ... (D3D12, unchanged); each outside a pass only.
 * [CHANGED] Metal: where an entry does something, memoryBarrierWithScope:
 * on an open compute encoder, the scope buffers and/or textures by the
 * entries; with none open, nothing (Metal orders a resource between its
 * encoders). [REMOVED] "Records nothing on Metal, which orders access to a
 * resource itself."
 */
void crgpu_command_list_pipeline_barrier(
    crgpu_command_list cmd,
    const crgpu_buffer_barrier *buffer_barriers,
    uint32_t buffer_barrier_count,
    const crgpu_image_barrier *image_barriers,
    uint32_t image_barrier_count);

/*
 * [CHANGED] (comment only) crgpu_buffer_barrier's "Maps to": "Metal records
 * no barriers" becomes "Metal: as crgpu_command_list_pipeline_barrier()";
 * the image barriers' GENERAL to GENERAL rule gains "Metal: as
 * crgpu_command_list_pipeline_barrier(), textures", and each image barrier
 * gains "Inside a rendering pass it records nothing, on every backend (as
 * crgpu_command_list_pipeline_barrier())"; and
 * crgpu_command_list_dispatch() gains "Dispatches with no barrier between
 * them may run at the same time (crgpu_command_list_pipeline_barrier())."
 *
 * ---------------------------------------------------------------------------
 * Where it is done (not in a header)
 * ---------------------------------------------------------------------------
 *
 * - crgpumetalcommand.m, crgpumetal___internal_ensure_compute_encoder():
 *   -computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent, in
 *   its pool as now.
 * - crgpumetalrenderpass.m: crgpumetal___internal_barrier(cmd, buffers,
 *   textures), one static helper: the compute encoder's
 *   memoryBarrierWithScope: where one is open, else nothing.
 *   pipeline_barrier returns after its checks while a render encoder is
 *   open (Q2 c), then decides `buffers` and `textures` from its entries by
 *   crgpu.h's rules (an entry whose two states differ, or which is one of
 *   the two ordering pairs); the image barriers do the same for GENERAL to
 *   GENERAL; their prints go (pass 1, 3).
 * - Vulkan (crgpuvulkancommand.c) and D3D12: pipeline_barrier and the
 *   three image barriers return after their checks while a pass is open
 *   (Vulkan's _rendering_active; D3D12's open-pass state), the same point
 *   in all three. No allocation; no new member.
 *
 * ---------------------------------------------------------------------------
 * Found in the code (each to be carded when the user takes this draft)
 * ---------------------------------------------------------------------------
 *
 *  1. Metal's image barriers print on a layout outside the enum (pass 1,
 *     3): removed here; Vulkan's and D3D12's handling of the same value to
 *     be checked against it (a card if they differ).
 *  2. A barrier inside a pass was a no-op on Metal, outside Vulkan's rules
 *     (VUID-vkCmdPipelineBarrier-None-09553) and real on D3D12 (pass 1, 2;
 *     pass 10, 1): one rule here, nothing on every backend (Q2 c).
 *
 * ---------------------------------------------------------------------------
 * Records when built
 * ---------------------------------------------------------------------------
 *
 * carrottus/CLAUDE.md: the heap bullet's "because this backend's barriers
 * do nothing" rewritten (heaps tracked by Q1; barriers real inside
 * encoders); a bullet on the concurrent compute encoder. CRGPU_DEVICE_TODO.md,
 * "Compute that overlaps on Metal": built, the measurements, Q4's runs. The
 * workspace's DECISIONS.md: a card for Q1 (b) if tracking's cost shows.
 */
