/*
 * crgpu_concurrent_compute_draft.h - Carrottus GPU's compute on Metal runs
 * dispatches side by side, as Vulkan and D3D12 do, and the caller's
 * barriers become Metal's own memory barriers (card S60).
 *
 * DRAFT, 2026-11-20, version 2 (version 1, then seven critique passes, each
 * fixed before the next), for the user's approval; not built.
 * Card S60 decided 2026-11-20 (a): "a Metal compute encoder of
 * MTLDispatchTypeConcurrent, Carrottus GPU's barriers turned into Metal's
 * memoryBarrierWithScope: (untracked resources where the caller's barriers
 * already order them)". Found 2026-11-19 by a caller's measurements of a
 * GPU particle step: 100 small systems in one command list cost half of 100
 * lists' (1.2 against 2.5 ms on the M5 Pro), not the sixth expected, since
 * Metal's serial encoder runs one dispatch after another.
 *
 * NO PUBLIC DECLARATION CHANGES. The change is the Metal backend's; what
 * this draft changes in crgpu.h is comments: the contract a caller already
 * has (ordering within a list is the caller's barriers', on every backend)
 * stated where it was implied, and the "Maps to" lines that said Metal
 * records no barrier. ABI: none. It is a draft because it changes what a
 * caller's work does on Metal (dispatches with no barrier between them now
 * overlap there, as they always could on the other two), and the card asked
 * for one.
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
 * manually with MTLFence"). carrottus 5b71f5d, 0.4.0, untagged. Not built,
 * not in any target, not compilable.
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
 * Why
 * ---------------------------------------------------------------------------
 *
 * Vulkan and D3D12 run dispatches side by side unless a barrier orders
 * them; Metal's backend made every compute encoder serial and its barriers
 * no-ops, relying on Metal's hazard tracking. So on Metal independent work
 * never overlaps (the caller's 100 particle systems, the card's figures),
 * and the caller's barriers, which the contract makes the only ordering,
 * mean nothing there. One behaviour on every backend, and the caller's
 * barriers real on all three.
 *
 * ---------------------------------------------------------------------------
 * What changes, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * Metal only. The compute encoder is made with
 * -computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent.
 * crgpu_command_list_pipeline_barrier() records, where any of its entries
 * does something by crgpu.h's rules (states that differ, or the two equal
 * pairs that order), memoryBarrierWithScope: into an open compute encoder
 * (Buffers and/or Textures by the entries), or
 * memoryBarrierWithScope:afterStages:beforeStages: into an open render
 * encoder, and nothing otherwise (Metal orders encoders on a tracked
 * resource itself). The three image barriers do the same for a GENERAL to
 * GENERAL barrier (Textures). Resources and heaps stay hazard tracked (Q1).
 * crgpu.h's comments say what holds on every backend: work recorded with no
 * barrier between may overlap, and what each barrier orders.
 *
 * ---------------------------------------------------------------------------
 * The contract the caller sees (unchanged in substance, stated once)
 * ---------------------------------------------------------------------------
 *
 * On every backend, within a command list:
 *  - Commands start in recording order. Two dispatches, or two draws in a
 *    pass, with no barrier between them may run at the same time and finish
 *    in any order: a write one makes is not visible to the other.
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
 *     (a) RECOMMENDED. Recorded: the render encoder's
 *         memoryBarrierWithScope:afterStages:beforeStages:, as the contract
 *         says the barrier orders two draws (pass 1, 2).
 *     (b) Left a no-op on Metal, as now: the contract's "between two draws"
 *         false on one backend.
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
 *  4. Inside a pass: a draw whose fragment shader writes a storage buffer
 *     at its pixel, a barrier UNORDERED_ACCESS to UNORDERED_ACCESS, a second
 *     draw reading it into the target; the target read back, every pixel the
 *     first draw's value.
 *  5. Images: test 1 with a storage image and the image barrier GENERAL to
 *     GENERAL between each dispatch.
 *  6. A15's indirect dispatch tests, unchanged, pass on the concurrent
 *     encoder (test 2's barrier to INDIRECT_ARGUMENT).
 *  7. cr_noalloc_tests: test 1's frame, 0 of ours.
 *  8. Every caller's GPU suite, Debug and Release, under Metal's API and
 *     shader validation with warnings fatal: its printed figures and hashes
 *     the same as before the build, line for line (Q4).
 * Seen failing first: 4 against today's no-op (whether a stale read shows on
 * the M5 Pro is recorded); 1 and 5 between the build's two steps (pass 6,
 * 1), recorded as seen or not.
 * Where each runs: the Mac (macos-metal, Debug and Release, Metal's
 * validation with warnings fatal, -asan, -tsan). Owed: the new tests on
 * Windows 11 (D3D12 and Vulkan, synchronization validation) and Fedora.
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
 *   one before the other.
 *
 * Maps to: vkCmdPipelineBarrier (Vulkan); ID3D12GraphicsCommandList::
 * ResourceBarrier ... (D3D12, unchanged). [CHANGED] Metal: where an entry
 * does something, memoryBarrierWithScope: on an open compute encoder, or
 * memoryBarrierWithScope:afterStages:beforeStages: on an open render
 * encoder (pass), the scope buffers and/or textures by the entries; with
 * neither open, nothing (Metal orders a resource between its encoders).
 * [REMOVED] "Records nothing on Metal, which orders access to a resource
 * itself."
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
 * crgpu_command_list_pipeline_barrier(), textures"; and
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
 *   memoryBarrierWithScope:, else the render encoder's with
 *   MTLRenderStageVertex | Fragment (| Object | Mesh on a device created
 *   with CRGPU_FEATURE_MESH_SHADING) on both sides, else nothing.
 *   pipeline_barrier decides `buffers` and `textures` from its entries by
 *   crgpu.h's rules (an entry whose two states differ, or which is one of
 *   the two ordering pairs); the image barriers call it for GENERAL to
 *   GENERAL; their prints go (pass 1, 3).
 * - Nothing in Vulkan or D3D12. No allocation; no new member.
 *
 * ---------------------------------------------------------------------------
 * Found in the code (each to be carded when the user takes this draft)
 * ---------------------------------------------------------------------------
 *
 *  1. Metal's image barriers print on a layout outside the enum (pass 1,
 *     3): removed here; Vulkan's and D3D12's handling of the same value to
 *     be checked against it (a card if they differ).
 *  2. A barrier inside a pass was a no-op on Metal (pass 1, 2): fixed here
 *     (Q2).
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
