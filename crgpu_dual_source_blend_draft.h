/*
 * crgpu_dual_source_blend_draft.h - Carrottus GPU's dual-source blending:
 * a feature bit and the four SRC1 blend factors (card S50, decided
 * 2026-11-17: a), and the three faults on their path found drafting it.
 *
 * DRAFT, 2026-11-17, version 1 (seven critique passes).
 * APPROVED 2026-11-17 with the user, every question as recommended.
 *
 * Written unattended on the Mac (Apple M5 Pro, macOS 27) against
 * include/crgpu.h, include/crcore.h, src/crcore.c, src/gpu/crgpushared.c
 * and .h, the three backends' pipeline code (crgpumetalshader.m,
 * crgpuvulkanshader.c, crgpud3d12shader.cpp) and adapter reports
 * (crgpumetalcontext.m, crgpuvulkancontext.c, crgpud3d12context.cpp),
 * cmake/cr_shaders.cmake, abi.txt and the tests, as read on 2026-11-17
 * (carrottus 0.4.0, untagged). Not built, not in any target, not
 * compilable: it repeats declarations of crgpu.h and crcore.h to show them
 * changed. Every declaration below is marked [ADDED], [CHANGED] or
 * [REMOVED]; nothing unmarked changes. Nothing is built until it is
 * approved; once approved it is frozen (ROADMAP.md, section 1, "Work built
 * from approved drafts").
 *
 * ---------------------------------------------------------------------------
 * Revision notes (version 1 written, then seven passes, each fixed before
 * the next; one lens a pass, as card S50 asked)
 * ---------------------------------------------------------------------------
 *
 * Pass 1 (wrong, missing, more than needed). 1. Missing: what a disabled
 * attachment's factors do. crgpu.h says a disabled attachment's blend is
 * "ignored entirely"; the draft now says the checks read only the first
 * color_attachment_count attachments with blend_enable true, so an SRC1
 * value left in a disabled one is neither refused nor sent anywhere, and
 * fault 3 (D3D12 sending it anyway) is the backend's half of that rule.
 * 2. Missing: a pipeline with no colour attachment (count 0) names no
 * factor by that rule; said. 3. Wrong: version 1 said D3D12 is valid
 * "only with that one target bound"; a Carrottus pipeline's
 * color_attachment_count is D3D12's NumRenderTargets, and a pass that
 * draws with it has the same count (crgpu.h), so the rule is the
 * pipeline's count, checked at creation. Reworded. 4. Missing: the
 * questions had no costs and the tests no expected values, no shader, no
 * machines; the records the build updates weren't named. Added (tests
 * finished in pass 7). 5. Looked at for "more than needed": the blend op
 * refusal (fault 1's second half) touches no SRC1 path. Kept, as Q3's (a),
 * with (b) the factor alone: the loop that reads the factors reads the
 * ops beside them, the code (291) is reserved for exactly this, and
 * without it one bad input still has three behaviours. "Who else uses
 * it" kept to three lines.
 *
 * Pass 2 (the platforms and industry practice). 1. Vulkan's four VUIDs
 * (VkPipelineColorBlendAttachmentState, 00608 to 00611) bind every factor
 * field whatever blendEnable says; the Vulkan backend leaves a disabled
 * attachment's fields zeroed (crgpuvulkanshader.c, the `if
 * (ba->blend_enable)` around the factors), so it already keeps pass 1's
 * rule, and D3D12 is brought to it. 2. wgpu's D3D12 backend writes a
 * colour factor in an alpha slot as its alpha twin (wgpu-hal's dx12
 * conv, map_blend_factor's is_alpha), and WebGPU's "src" in an alpha slot
 * means the source's alpha: Q4 (a) is the industry's answer, not a choice
 * of ours. Added to "What was compared". 3. WebGPU: WGSL's @blend_src is
 * allowed only with @location(0). Whether the specification also limits
 * the colour targets to one was not read here (the specification's text
 * on it wasn't reached); said so rather than claimed. 4. Android (card
 * S47): dualSrcBlend is optional on Vulkan and absent on some mobile GPUs,
 * so the bit is how a caller there knows; added to "Who else uses it".
 * 5. bgfx, sokol_gfx and SDL_GPU have no SRC1 factor; NVRHI has the four
 * with no capability query (it targets desktop D3D and Vulkan). Kept as
 * written.
 *
 * Pass 3 (the C API, CONVENTIONS.md). 1. Version 1's values had no
 * Doxygen; each has its `/**< ... * /` now, and the [ADDED] group its why,
 * ABI and tests, as the ASTC draft's do. 2. The feature macro's comment
 * now follows its neighbours: what is refused, with which result, and each
 * API's source. 3. crgpu_graphics_pipeline_create()'s @retval list was
 * "..."; it is written whole, in the order the checks run, the three
 * changed lines marked. 4. The check order is in field order where it can
 * be (depth_bias_clamp, then blend_attachments in index order and each
 * attachment's fields in declaration order, then the counts), with the
 * existing count bound first because it bounds the loop. 5. Missing:
 * crcore.h's 290 and 291 lose "Reserved" and say what they mean, and 226's
 * meaning grows a clause: each [CHANGED]. crgpu_blend_attachment_desc and
 * the pipeline desc's blend_attachments get the rules in their comments
 * ([CHANGED], comments only; the frozen struct's layout untouched).
 *
 * Pass 4 (no hidden policy). 1. Fault 1 is hidden policy as built: an
 * unknown factor silently becomes ZERO. Refused now (Q3). 2. Checked: the
 * D3D12 alpha-slot mapping (Q4 a) is translation, not choice: Vulkan's
 * specification gives SRC_COLOR in an alpha slot the factor As0, Metal
 * the same, and D3D12 spells that factor SRC_ALPHA. (b), refusing on every
 * backend what two APIs and WebGPU allow, kept as the alternative. 3.
 * Checked: D3D12's disabled target written ONE, ZERO, ADD (D3D12's own
 * default blend state, which d3dx12's CD3DX12_BLEND_DESC writes) is a
 * value the API ignores, not a behaviour; said so. 4. Checked: the Vulkan
 * bit also reads maxFragmentDualSrcAttachments >= 1. The specification
 * promises it where dualSrcBlend is true; reading it means the bit is never
 * claimed on a driver that says otherwise. Not a choice. 5. Looked at: no
 * limit field. The rule is one attachment on every backend (the narrowest
 * contract every API keeps, carrottus/CLAUDE.md), so a field could only say
 * 1; a Vulkan driver allowing more is not exposed, and Left out says so.
 *
 * Pass 5 (threads and errors). 1. Checked: nothing new is an object; the
 * bit is read at instance creation with the other features (lock-free
 * thereafter, carrottus/CLAUDE.md "Threads"); crgpu_graphics_pipeline_create
 * keeps the device's rule. No Thread: line changes. 2. Every refusal is
 * Carrottus's own check before any platform call, its own `if` and code
 * (none joined), so nothing reaches the instance's message callback (card
 * S10's channel) and the call changes nothing: *out_pipeline is NULL as
 * the existing refusals leave it. 3. Q1 compared against CONVENTIONS.md
 * ("a general result code first; a new code only for a failure a caller
 * would branch on"): the caller asked for the features its device has, so
 * the general CR_RESULT_GPU_FEATURE_NOT_SUPPORTED says enough; (a) kept.
 * 4. A module/pipeline mismatch (SRC1 named with no second output, or a
 * second output with no SRC1) is outside the contract, undefined, as a ray
 * query module on a device without the feature is; D3D12 may refuse it at
 * pipeline creation (CR_RESULT_API_SPECIFIC_FAILURE, as any refusal by the
 * API). Said in the factors' comment.
 *
 * Pass 6 (the code as built; every file cited re-read). 1. The checks'
 * home is crgpu___internal_check_pipeline_outputs() (crgpushared.c line
 * 91), called by all three backends (crgpumetalshader.m 341,
 * crgpuvulkanshader.c 308, crgpud3d12shader.cpp 663): one change. Its
 * order today is geometry, tessellation, count above
 * CRGPU_MAX_COLOR_ATTACHMENTS, depth bias clamp, independent blend; the
 * new checks go after the depth bias clamp and the value checks before
 * the independent-blend check, which compares attachments: version 1 left
 * the order unsaid, and after it two attachments differing only by a
 * garbage factor would have come back INDEPENDENT_BLEND_NOT_SUPPORTED.
 * 2. D3D12's blend desc (crgpu_d3d12___blend_desc, crgpud3d12shader.cpp
 * line 40) serves both pipeline paths (lines 364 and 756); it writes
 * target 0 even with no colour attachment ("With none, target 0 still has
 * a valid state, unused") from blend_attachments[0], which may hold any
 * value: with fault 3's fix that target is written as disabled. 3.
 * Confirmed as built: CR_RESULT_GPU_INVALID_BLEND_FACTOR = 290 and
 * _BLEND_OP = 291 "Reserved: no call returns it" (crcore.h), named already
 * in cr_result_str() (crcore.c lines 228 to 231); Vulkan's op is cast
 * ((VkBlendOp)ba->color_blend_op), D3D12's op + 1, Metal's switch defaults
 * to Add; every factor switch defaults to ZERO (Metal's has no ZERO case
 * at all, its default being it). 4. Confirmed: the Metal backend lists only
 * Metal 3 GPUs, Apple7 and later and Mac2 (crgpumetalcontext.m's comment
 * above CRGPU_FEATURE_LAYERED_RENDERING), every one with the Source1
 * factors (macOS 10.12, iOS 11). 5. abi.txt holds the blend factor values
 * (lines 157 to 166) but no CRGPU_FEATURE_* macro (none is in it today):
 * the update adds four constants; the bit is checked by the tests, not
 * the snapshot. Said under the breaks.
 *
 * Pass 7 (tests and the build). 1. The test shader is named, with its
 * declaration (the one measured), added to CR_TEST_SHADERS on the default
 * Metal route (Slang's own writes [[color(1)]], measured), its input the
 * draw vertex shader's location 0 (crgpu.h's linking rule), its X and T
 * from the existing push block. 2. Expected values are exact: an
 * R16G16B16A16_FLOAT target and values that are halves exactly, so the
 * readback is compared bit for bit on every backend. 3. Each factor is
 * seen in a draw, the D3D12 alpha mapping included, and every refusal with
 * its order. 4. The misuse harness: no call is added or changed in shape,
 * so the regenerated file is expected unchanged; it is regenerated and the
 * diff checked, as the rule says. 5. Where it runs: built and run on the
 * Mac; Vulkan type-checked there against the SDK's headers; D3D12 written,
 * not compiled; Windows and Fedora owed, each Vulkan driver's two values
 * recorded.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * Blending multiplies the attachment by a factor per channel. Without
 * dual-source blending that factor comes from the fragment's one output
 * (SRC_COLOR, SRC_ALPHA) or from the attachment, so a draw can't add a
 * colour X and multiply what is behind by an unrelated per-channel T in
 * one pass: target = X * 1 + target * T. Every graphics API has the
 * answer, a second fragment output read only by the blender (Vulkan's
 * dualSrcBlend, D3D12's SRC1 factors, Metal's Source1 factors), and
 * Carrottus GPU exposes none of it: CRGPU_BLEND_FACTOR stops at
 * ONE_MINUS_DST_COLOR (9). A caller's coloured transmittance through a
 * medium waits on it (card S50).
 *
 * Three faults in the code as built were found drafting this and are
 * fixed here, each on the path the new factors take:
 *
 *  1. An unknown CRGPU_BLEND_FACTOR becomes ZERO in every backend's switch
 *     (crgpumetalshader.m, crgpuvulkanshader.c, crgpud3d12shader.cpp, each
 *     `default:`): a choice made for the caller, with no sign of it.
 *     CR_RESULT_GPU_INVALID_BLEND_FACTOR (290) exists and no call returns
 *     it. An unknown CRGPU_BLEND_OP is cast straight to VkBlendOp, becomes
 *     D3D12's op + 1, and Metal's Add: three behaviours for one input,
 *     with CR_RESULT_GPU_INVALID_BLEND_OP (291) reserved too.
 *  2. D3D12 takes no *_COLOR factor in an alpha slot
 *     (D3D12_RENDER_TARGET_BLEND_DESC: "Blend options that end in _COLOR
 *     are not allowed" for SrcBlendAlpha and DestBlendAlpha). Vulkan and
 *     Metal take one and use its alpha. crgpud3d12shader.cpp passes the
 *     colour factor through, so a pipeline valid on two backends is
 *     invalid on the third, today with SRC_COLOR and DST_COLOR and with
 *     SRC1_COLOR once added.
 *  3. crgpud3d12shader.cpp writes a target's factors when its
 *     blend_enable is false (and target 0's when there is no colour
 *     attachment), though crgpu.h says a disabled attachment's blend is
 *     ignored entirely: an ignored SRC1 value would reach D3D12.
 *
 * ---------------------------------------------------------------------------
 * What changes, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * CRGPU_FEATURE_DUAL_SOURCE_BLEND (bit 19) says an adapter blends with a
 * fragment's second output. CRGPU_BLEND_FACTOR gains SRC1_COLOR (10),
 * ONE_MINUS_SRC1_COLOR (11), SRC1_ALPHA (12) and ONE_MINUS_SRC1_ALPHA
 * (13). A graphics pipeline with an enabled blend attachment that names
 * one is refused on a device created without the bit
 * (CR_RESULT_GPU_FEATURE_NOT_SUPPORTED) and with more than one colour
 * attachment (CR_RESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS), the limit every
 * backend shares. A factor or op that is no value of its enum is refused
 * with the two reserved codes, now returned. On D3D12 a *_COLOR factor in
 * an alpha slot is written as its *_ALPHA twin, the factor the other two
 * backends apply, and a disabled target is written as disabled.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 *  - Vulkan (vulkan_core.h, Vulkan SDK 1.4.363.0 on this machine; the
 *    specification): VK_BLEND_FACTOR_SRC1_COLOR (15),
 *    _ONE_MINUS_SRC1_COLOR (16), _SRC1_ALPHA (17), _ONE_MINUS_SRC1_ALPHA
 *    (18) (lines 2463 to 2466); VkPhysicalDeviceFeatures::dualSrcBlend
 *    (line 3534), optional, enabled at device creation;
 *    VkPhysicalDeviceLimits::maxFragmentDualSrcAttachments (line 3634),
 *    at least 1 where the feature is. Without the feature no factor field
 *    of a VkPipelineColorBlendAttachmentState may name an SRC1 factor
 *    (VUIDs ...-srcColorBlendFactor-00608, -dstColorBlendFactor-00609,
 *    -srcAlphaBlendFactor-00610, -dstAlphaBlendFactor-00611), whatever
 *    blendEnable says. With it, a fragment shader using dual-source
 *    blending writes at most maxFragmentDualSrcAttachments attachments.
 *    The shader's second output is SPIR-V `Location 0, Index 1`. A colour
 *    factor in an alpha slot is that colour's alpha (SRC_COLOR: As0).
 *  - D3D12 (DirectX-Headers' d3d12.h, the copy under
 *    .refs/HazelRenderer2025/Hazel/Hazel/vendor/nvrhi/thirdparty/, lines
 *    1720 to 1723; Microsoft's documentation): D3D12_BLEND_SRC1_COLOR
 *    (16), _INV_SRC1_COLOR (17), _SRC1_ALPHA (18), _INV_SRC1_ALPHA (19), at
 *    every feature level (Carrottus lists 12_0 and up). Dual-source
 *    blending reads the pixel shader's SV_Target0 and SV_Target1 as the
 *    two sources of render target 0, with one render target. No *_COLOR
 *    factor in SrcBlendAlpha or DestBlendAlpha.
 *  - Metal (MTLRenderPipeline.h, the macOS 27 SDK on this machine, lines
 *    41 to 44; the Metal Shading Language specification):
 *    MTLBlendFactorSource1Color (15), OneMinusSource1Color (16),
 *    Source1Alpha (17), OneMinusSource1Alpha (18), macOS 10.12 and iOS 11;
 *    no property to read and nothing to switch on. The second output is
 *    `[[color(0), index(1)]]`; only colour attachment 0 blends with two
 *    sources.
 *  - WebGPU (the W3C specification; Chrome's notes for version 130): the
 *    feature "dual-source-blending", the factors "src1",
 *    "one-minus-src1", "src1-alpha" and "one-minus-src1-alpha", and WGSL's
 *    `@location(0) @blend_src(0)` and `@location(0) @blend_src(1)`:
 *    @blend_src only at location 0. "src" in an alpha slot is the
 *    source's alpha. Whether its text also limits a pipeline to one colour
 *    target was not read here (pass 2).
 *  - Industry practice: wgpu (Features::DUAL_SOURCE_BLENDING,
 *    BlendFactor::Src1, OneMinusSrc1, Src1Alpha, OneMinusSrc1Alpha; its
 *    D3D12 backend writes a colour factor in an alpha slot as its alpha
 *    twin, Q4's (a)); NVRHI (BlendFactor::Src1Color, InvSrc1Color,
 *    Src1Alpha, InvSrc1Alpha, nvrhi.h line 906, no capability query).
 *    bgfx's BGFX_STATE_BLEND_*, sokol_gfx's sg_blend_factor and SDL_GPU's
 *    SDL_GPUBlendFactor have no SRC1 factor: each keeps to what its
 *    smallest backend (WebGL, GLES) has, which none of Carrottus's
 *    backends needs.
 *
 * Hazel is not compared: Carrottus is not measured against an engine
 * (the workspace's CLAUDE.md, "External references").
 *
 * ---------------------------------------------------------------------------
 * The shader's second output, measured (Slang, on this Mac)
 * ---------------------------------------------------------------------------
 *
 * slangc 2026.18.3-2 (the Vulkan SDK 1.4.363.0's, the one
 * cmake/cr_shaders.cmake finds), with cr_add_shader()'s own flags, on a
 * throwaway fragment shader in a scratch directory, deleted after. The
 * declaration that works on every target:
 *
 *     struct FragmentOut {
 *         [[vk::location(0)]]               float4 color : SV_Target0;
 *         [[vk::location(0), vk::index(1)]] float4 blend : SV_Target1;
 *     };
 *
 *  - SPIR-V (-target spirv -profile spirv_1_5 -warnings-as-errors all):
 *    `color` Location 0; `blend` Location 0 and Index 1. spirv-val
 *    --target-env vulkan1.2 passes.
 *  - DXIL (-target hlsl -profile ps_6_0, then the SDK's dxc 1.9.0.5512
 *    -T ps_6_0 -WX): the output signature is SV_Target 0 and SV_Target 1,
 *    registers 0 and 1, which D3D12 reads as render target 0's two
 *    sources when its blend names an SRC1 factor.
 *  - Metal by the build's default route (SPIR-V to tools/crgpu_msl, on
 *    SPIRV-Cross, whose spirv_msl.cpp reads DecorationIndex): `color`
 *    `[[color(0)]]`, `blend` `[[color(0), index(1)]]`. xcrun metal
 *    -std=metal3.1 -Werror compiles it for macOS and iOS, and metallib
 *    links it.
 *  - Metal by Slang's own route (-target metal, cr_add_shader()'s
 *    METAL_ROUTE SLANG): `blend` comes out `[[color(1)]]`, a second
 *    attachment, not a second source. A dual-source shader can't take that
 *    route; the default one is right.
 *  - Both attributes on both members are needed: `[[vk::index(1)]]` alone
 *    is an error under -warnings-as-errors ("vk::index without
 *    vk::location", the index ignored), and an explicit location on the
 *    second member alone is refused ("mixing implicit and explicit
 *    varying bindings").
 *
 * Carrottus GPU reads no module's code (crgpu.h, crgpu_shader_module_desc),
 * so whether a fragment shader declares the second output is the caller's
 * to get right, as it is for a ray query.
 *
 * ---------------------------------------------------------------------------
 * What the caller decides, and what Carrottus GPU only reports
 * ---------------------------------------------------------------------------
 *
 * Reported: whether an adapter blends with two sources (the bit). Decided
 * by the caller: whether its device has the feature, which factors each
 * pipeline blends with, what its shader writes to the second output, and
 * what to do on an adapter without it. Carrottus GPU never substitutes a
 * factor, never falls back to a second pass, and never drops an
 * attachment.
 *
 * ---------------------------------------------------------------------------
 * Who else uses it
 * ---------------------------------------------------------------------------
 *
 *  - A renderer's coloured transmittance (card S50's caller): X added,
 *    the target multiplied by T per channel; ONE and SRC1_COLOR, alpha
 *    ONE and SRC1_ALPHA. Covered by the four factors.
 *  - Subpixel text: a glyph's coverage per channel as the second output,
 *    blended SRC1_COLOR / ONE_MINUS_SRC1_COLOR.
 *  - Coloured translucency in one pass (glass tinting what is behind it)
 *    and weighted blended transparency.
 *  - Carrottus on Android (card S47): dualSrcBlend is optional on mobile
 *    Vulkan, so the bit is how a caller there knows.
 *
 * ---------------------------------------------------------------------------
 * Left out, and why
 * ---------------------------------------------------------------------------
 *
 *  - The other factors every API has and CRGPU_BLEND_FACTOR lacks
 *    (CONSTANT_COLOR and its blend constants, SRC_ALPHA_SATURATE): no
 *    caller needs them; appended when one does.
 *  - A limit field (max_fragment_dual_source_attachments): one attachment
 *    on every backend, so it could only say 1. A Vulkan driver reporting
 *    more is not exposed (pass 4).
 *  - Which formats blend at all (card S51, open): a per-format question
 *    apart from this one. A dual-source pipeline blends into its one
 *    attachment under the same rule as any blend; when S51 is built, its
 *    blend bit covers dual-source pipelines too, with nothing here to
 *    change.
 *  - Checking a module's outputs against the pipeline's factors: Carrottus
 *    GPU reads no module's code on Vulkan and D3D12; undefined (pass 5).
 *
 * ---------------------------------------------------------------------------
 * The version and the breaks
 * ---------------------------------------------------------------------------
 *
 *  - CRGPU_FEATURES: bit 19 appended. S48 has 16 and 17, S49 18. The
 *    macros are not in abi.txt (none is today).
 *  - CRGPU_BLEND_FACTOR: values 10 to 13 appended; abi.txt gains four
 *    constants. Append-only, no value changes.
 *  - crcore.h: CR_RESULT_GPU_INVALID_BLEND_FACTOR (290) and
 *    _INVALID_BLEND_OP (291) returned where they were reserved;
 *    CR_RESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS (226) gains a clause. No
 *    value changes.
 *  - crgpu_graphics_pipeline_create() refuses what it passed before, a
 *    behaviour break, listed: a factor past 13 or an op past 4 on an
 *    enabled attachment (ZERO, ADD or cast raw before). Searched: no
 *    caller in the workspace passes one (every caller names the enum's
 *    values); the building session rechecks and lists any it finds.
 *  - On D3D12, a *_COLOR factor in an alpha slot is now accepted (it was
 *    invalid there), and a disabled target's factors no longer reach the
 *    API. No caller depended on either.
 *  - No version raise: carrottus' 0.4.0 is untagged, so this lands in it
 *    (card X2). ./check.sh --update-abi lists the four constants; the
 *    misuse generator is rerun.
 *
 * New handles: none. New struct types: none. Struct layouts: unchanged.
 * Stored formats touched: none.
 *
 * Thread: nothing changes. The bit is an adapter fact, read when the
 * instance lists its adapters; crgpu_graphics_pipeline_create() keeps the
 * device's rule. Allocation: none added.
 *
 * Video: nothing here touches formats, queues or the colour space draft.
 *
 * ---------------------------------------------------------------------------
 * Questions (each with options and a recommendation)
 * ---------------------------------------------------------------------------
 *
 * Q1. THE REFUSAL WITHOUT THE FEATURE.
 *     (a) RECOMMENDED. CR_RESULT_GPU_FEATURE_NOT_SUPPORTED (212), whose
 *         comment already reads "a call needs one the device wasn't
 *         created with and has no result of its own for it", as the
 *         approved ASTC draft's image creates and CRGPU_FEATURE_PRESENT's
 *         calls use it. The caller chose its features, so the general code
 *         says enough (CONVENTIONS.md, "Codes").
 *     (b) A code of its own, CR_RESULT_GPU_DUAL_SOURCE_BLEND_NOT_SUPPORTED,
 *         as INDEPENDENT_BLEND has 228: one more code for a failure no
 *         caller branches on differently.
 *
 * Q2. MORE THAN ONE COLOUR ATTACHMENT.
 *     (a) RECOMMENDED. CR_RESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS (226), its
 *         meaning grown by a clause: it is exactly "more colour attachments
 *         than this pipeline may have".
 *     (b) CR_RESULT_DESC_INVALID_PARAMETERS: right but vaguer, and the
 *         specific code exists.
 *
 * Q3. AN UNKNOWN FACTOR OR OP.
 *     (a) RECOMMENDED. Both refused, with the reserved
 *         CR_RESULT_GPU_INVALID_BLEND_FACTOR and _INVALID_BLEND_OP, on
 *         enabled attachments, before the independent-blend check. Removes
 *         a silent ZERO (Carrottus's no-hidden-policy rule) and three
 *         backends' three answers for one bad op; costs a loop over at most
 *         8 attachments at pipeline creation.
 *     (b) The factor alone, since only the factor's enum grows here; the
 *         op keeps its three behaviours until someone touches it.
 *     (c) Neither: an unknown value, SRC1 ones on an older build
 *         included, stays a silent ZERO.
 *
 * Q4. A *_COLOR FACTOR IN AN ALPHA SLOT ON D3D12.
 *     (a) RECOMMENDED. D3D12 writes its *_ALPHA twin (SRC_COLOR as
 *         SRC_ALPHA, ONE_MINUS_SRC_COLOR as INV_SRC_ALPHA, DST_COLOR as
 *         DEST_ALPHA, ONE_MINUS_DST_COLOR as INV_DEST_ALPHA, SRC1_COLOR as
 *         SRC1_ALPHA, ONE_MINUS_SRC1_COLOR as INV_SRC1_ALPHA): the factor
 *         Vulkan and Metal apply to alpha for the same value, as wgpu does.
 *         One meaning on every backend, no refusal.
 *     (b) Refuse a *_COLOR factor in an alpha slot on every backend
 *         (CR_RESULT_GPU_INVALID_BLEND_FACTOR): the narrowest contract,
 *         but it refuses what Vulkan, Metal and WebGPU define.
 *
 * Bit 19 and the name CRGPU_FEATURE_DUAL_SOURCE_BLEND are as card S50
 * set them (CRGPU_DEVICE_TODO.md), not a question: the name follows
 * CRGPU_FEATURE_INDEPENDENT_BLEND's and WebGPU's "dual-source-blending".
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 *  - crgpu_device_test, on every adapter of each backend: the bit as each
 *    API reports it (Metal and D3D12: on every adapter; Vulkan: exactly
 *    when dualSrcBlend is true and maxFragmentDualSrcAttachments is 1 or
 *    more, read through crgpu_get_vk_physical_device(), with both values
 *    printed for CRGPU_DEVICE_TODO.md); a device created with it reports it
 *    from crgpu_device_get_features(), one created without doesn't.
 *  - A test shader, tests/shaders/objects_dual_source_frag.slang, in
 *    CR_TEST_SHADERS as "crgpu_test_objects_dual_source_frag|
 *    objects_dual_source_frag|ps_6_0|", on the default Metal route (never
 *    METAL_ROUTE SLANG, which writes [[color(1)]]): input
 *    `[[vk::location(0)]] float2 uv : TEXCOORD0` (the draw vertex shader's
 *    output, crgpu.h's linking rule), outputs as measured above, X the
 *    push block's `color` and T its `params`, the same block and range the
 *    draw vertex shader uses.
 *  - crgpu_objects_test, on a device created with the bit where the
 *    adapter has it (every Metal and D3D12 adapter): an
 *    R16G16B16A16_FLOAT target of 8 x 8 cleared to D = (0.5, 0.25, 1.0,
 *    0.5), a draw of one cell per pipeline, read back and compared bit for
 *    bit (every value below is a half exactly):
 *      A. colour ONE / SRC1_COLOR, alpha ONE / SRC1_ALPHA, X = (0.25, 0.5,
 *         0, 0), T = (0.5, 1, 0.25, 0.5): (0.5, 0.75, 0.25, 0.25).
 *      B. colour ZERO / ONE_MINUS_SRC1_COLOR, alpha ZERO /
 *         ONE_MINUS_SRC1_ALPHA, same X and T: (0.25, 0, 0.75, 0.25).
 *      C. colour SRC1_ALPHA / ZERO, alpha ZERO / SRC1_COLOR (Q4 on
 *         D3D12): X = (1, 1, 1, 1), T = (0.5, 0.5, 0.5, 0.25):
 *         (0.25, 0.25, 0.25, 0.125).
 *      D. colour ZERO / SRC1_COLOR, alpha ZERO / SRC_COLOR (fault 2's
 *         existing factor in an alpha slot, through the same shader so
 *         the pipeline stays dual-source, as the shader's second output
 *         requires), X = (0, 0, 0, 0.5), T = (1, 1, 1, 1):
 *         (0.5, 0.25, 1.0, 0.25).
 *    Cells left undrawn keep D. Each pipeline also made and drawn on
 *    Vulkan where the adapter has the bit.
 *  - Refusals, on a device created without the bit (every adapter can
 *    make one): each of the four SRC1 factors in each of the four factor
 *    fields of attachment 0, sixteen pipelines, each
 *    CR_RESULT_GPU_FEATURE_NOT_SUPPORTED with out_pipeline NULL; an SRC1
 *    factor on a disabled attachment, accepted (ignored); with the bit,
 *    SRC1 on attachment 0 of two (blend equal, independent blend asked
 *    for and not), CR_RESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS; a factor of
 *    14 and of UINT32_MAX, CR_RESULT_GPU_INVALID_BLEND_FACTOR, an op of 5,
 *    CR_RESULT_GPU_INVALID_BLEND_OP, on enabled attachments and accepted
 *    on a disabled one; the order: a garbage factor beside an SRC1 factor
 *    without the bit gives INVALID_BLEND_FACTOR, and two attachments
 *    differing only by a garbage factor on a device without independent
 *    blend give INVALID_BLEND_FACTOR, not INDEPENDENT_BLEND_NOT_SUPPORTED.
 *  - The misuse harness: regenerated (python3
 *    tests/misuse/make_misuse_tests.py after ./check.sh --update-abi); no
 *    call is added or changes shape, so the file is expected unchanged,
 *    and the diff is checked.
 *  - Where each runs: the Mac (Apple M5 Pro; macos-metal Debug and
 *    Release under Metal API and shader validation with warnings fatal,
 *    -asan, -tsan). Vulkan's half type-checked on the Mac against the
 *    SDK's headers, as card S33's was; D3D12's written, not compiled.
 *    Owed: Windows 11 (Iris Xe, RTX A2000 and WARP over D3D12 with the
 *    debug layer and GPU-based validation; the Iris Xe and the A2000 over
 *    Vulkan under validation and synchronization validation) and Fedora
 *    (ANV, NVK, RADV over Vulkan), each driver's dualSrcBlend and
 *    maxFragmentDualSrcAttachments recorded.
 *  - ./check.sh: abi, doxygen, conventions, caller-names.
 *
 * ---------------------------------------------------------------------------
 * crgpu.h
 * ---------------------------------------------------------------------------
 */

/** [ADDED] A graphics pipeline that blends with the fragment shader's
 * second output: an enabled blend attachment naming
 * CRGPU_BLEND_FACTOR_SRC1_COLOR, _ONE_MINUS_SRC1_COLOR, _SRC1_ALPHA or
 * _ONE_MINUS_SRC1_ALPHA, refused with CR_RESULT_GPU_FEATURE_NOT_SUPPORTED on
 * a device created without it. Vulkan: `VkPhysicalDeviceFeatures::dualSrcBlend`
 * with `VkPhysicalDeviceLimits::maxFragmentDualSrcAttachments` 1 or more,
 * enabled with it. D3D12 and Metal: every adapter.
 *
 * Why: card S50, a colour added and the target multiplied per channel by
 * an unrelated value in one draw. ABI: a bit appended (S48 has 16 and 17,
 * S49 18). Tests: crgpu_device_test, crgpu_objects_test. */
#define CRGPU_FEATURE_DUAL_SOURCE_BLEND (UINT64_C(1) << 19)

/**
 * @brief Factor a source or destination color or alpha is multiplied by when
 * blending.
 *
 * [CHANGED] A *_COLOR factor in an alpha slot (src_alpha_blend_factor,
 * dst_alpha_blend_factor) is that colour's alpha, on every backend: SRC_COLOR
 * there is the source's alpha, as SRC_ALPHA is (Q4 a). A value that is none
 * of these is refused by crgpu_graphics_pipeline_create() with
 * CR_RESULT_GPU_INVALID_BLEND_FACTOR (Q3 a). Only appended to.
 */
typedef uint32_t CRGPU_BLEND_FACTOR;

enum {
	/* ... CRGPU_BLEND_FACTOR_ZERO (0) to CRGPU_BLEND_FACTOR_ONE_MINUS_DST_COLOR
	 * (9), unchanged ... */

	/*
	 * [ADDED] The fragment shader's second output, source 1: the output at
	 * location 0 index 1 (SPIR-V `Location 0, Index 1`; HLSL SV_Target1;
	 * MSL `[[color(0), index(1)]]`), which the blender reads and no
	 * attachment receives. Named by a pipeline only on a device created with
	 * CRGPU_FEATURE_DUAL_SOURCE_BLEND, and only with one colour attachment.
	 * A pipeline naming one whose fragment shader writes no second output,
	 * or one not naming one whose shader writes a second output, is the
	 * caller's error, and what it blends is undefined. Vulkan:
	 * VK_BLEND_FACTOR_SRC1_COLOR and on. D3D12: D3D12_BLEND_SRC1_COLOR and
	 * on. Metal: MTLBlendFactorSource1Color and on.
	 *
	 * Why: card S50. ABI: values appended at 10 to 13. Tests:
	 * crgpu_objects_test (each drawn and refused).
	 */
	CRGPU_BLEND_FACTOR_SRC1_COLOR           = 10, /**< Source 1's color, per channel */
	CRGPU_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR = 11, /**< 1 - source 1's color, per channel */
	CRGPU_BLEND_FACTOR_SRC1_ALPHA           = 12, /**< Source 1's alpha */
	CRGPU_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA = 13  /**< 1 - source 1's alpha */
};

/**
 * @brief Per-attachment blend configuration.
 * Ignored entirely when blend_enable is false.
 *
 * [CHANGED] (comment only) Ignored entirely means not checked either: a
 * disabled attachment's factors and ops are never refused and never reach
 * the graphics API, whatever they hold. On an enabled one each factor is a
 * CRGPU_BLEND_FACTOR and each op a CRGPU_BLEND_OP, or the pipeline is
 * refused. An SRC1 factor in any of its four factor fields, whatever its
 * ops, makes the pipeline dual-source (CRGPU_FEATURE_DUAL_SOURCE_BLEND).
 */
/* Frozen: an array element of crgpu_graphics_pipeline_desc. It never changes; a new need gets a new type. */
typedef struct crgpu_blend_attachment_desc {
	/* ... every member unchanged ... */
} crgpu_blend_attachment_desc;

/*
 * crgpu_graphics_pipeline_desc::blend_attachments, [CHANGED] (comment only):
 *
 *	/**
 *	 * Blend configuration for each color attachment: entry i for
 *	 * #color_attachment_formats[i]. Only the first #color_attachment_count
 *	 * are read, and of those only the ones with blend_enable. Entries that
 *	 * differ from entry 0 need a device created with
 *	 * CRGPU_FEATURE_INDEPENDENT_BLEND. An entry naming an SRC1 factor needs
 *	 * a device created with CRGPU_FEATURE_DUAL_SOURCE_BLEND and a
 *	 * #color_attachment_count of 1: dual-source blending writes attachment 0
 *	 * alone, on every backend.
 *	 * @see crgpu_blend_attachment_desc
 *	 * /
 */

/**
 * @brief Creates a graphics pipeline.
 *
 * ... the description unchanged ...
 *
 * [CHANGED] its results, in the order checked (three lines added, one
 * grown; every other line as today):
 *
 * @retval CR_SUCCESS The pipeline was created.
 * @retval CR_RESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_pipeline` is NULL.
 * @retval CR_RESULT_GPU_VERTEX_AND_FRAGMENT_SHADER_REQUIRED
 *         `desc->fragment_module` is NULL, or both `desc->vertex_module` and
 *         `desc->mesh_module` are.
 * @retval CR_RESULT_GPU_INVALID_SHADER_STAGE `desc->vertex_module` and
 *         `desc->mesh_module` are both set.
 * @retval CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED `desc->mesh_module`
 *         is set and the device wasn't created with
 *         CRGPU_FEATURE_MESH_SHADING.
 * @retval CR_RESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS
 *         `desc->color_attachment_count` is above
 *         CRGPU_MAX_COLOR_ATTACHMENTS.
 * @retval CR_RESULT_GPU_DEPTH_BIAS_CLAMP_NOT_SUPPORTED
 *         `desc->depth_bias_clamp` is not 0 and the device wasn't created
 *         with CRGPU_FEATURE_DEPTH_BIAS_CLAMP.
 * @retval CR_RESULT_GPU_INVALID_BLEND_FACTOR [ADDED] A factor of one of the
 *         first `desc->color_attachment_count` blend attachments with
 *         blend_enable is no CRGPU_BLEND_FACTOR. Attachments in index order,
 *         each one's fields in declaration order with its ops.
 * @retval CR_RESULT_GPU_INVALID_BLEND_OP [ADDED] An op of such an
 *         attachment is no CRGPU_BLEND_OP.
 * @retval CR_RESULT_GPU_INDEPENDENT_BLEND_NOT_SUPPORTED Two of the first
 *         `desc->color_attachment_count` blend attachments differ and the
 *         device wasn't created with CRGPU_FEATURE_INDEPENDENT_BLEND.
 * @retval CR_RESULT_GPU_FEATURE_NOT_SUPPORTED [ADDED] Such an attachment
 *         names an SRC1 factor and the device wasn't created with
 *         CRGPU_FEATURE_DUAL_SOURCE_BLEND.
 * @retval CR_RESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS [CHANGED] ... or such an
 *         attachment names an SRC1 factor and
 *         `desc->color_attachment_count` is above 1.
 * @retval CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH On Metal, ... (unchanged).
 * @return Another CR_RESULT if the backend fails, such as
 *         CR_RESULT_OUT_OF_MEMORY or CR_RESULT_API_SPECIFIC_FAILURE.
 */
CR_RESULT crgpu_graphics_pipeline_create(
    crgpu_device device,
    const crgpu_graphics_pipeline_desc *desc,
    crgpu_graphics_pipeline *out_pipeline);

/*
 * ---------------------------------------------------------------------------
 * crcore.h
 * ---------------------------------------------------------------------------
 *
 *	/** [CHANGED] More color attachments than CRGPU_MAX_COLOR_ATTACHMENTS, or
 *	 * more than one in a pipeline that blends with two sources
 *	 * (CRGPU_FEATURE_DUAL_SOURCE_BLEND). * /
 *	CR_RESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS                  = 226,
 *
 *	/** [CHANGED] A pipeline's enabled blend attachment has a factor that is
 *	 * no CRGPU_BLEND_FACTOR. Since 0.4.0 (reserved before). * /
 *	CR_RESULT_GPU_INVALID_BLEND_FACTOR                        = 290,
 *	/** [CHANGED] A pipeline's enabled blend attachment has an op that is no
 *	 * CRGPU_BLEND_OP. Since 0.4.0 (reserved before). * /
 *	CR_RESULT_GPU_INVALID_BLEND_OP                            = 291,
 *
 * CR_RESULT_GPU_FEATURE_NOT_SUPPORTED (212) unchanged: its comment covers
 * this call already (Q1 a).
 *
 * ---------------------------------------------------------------------------
 * Where each backend does it (not in a header)
 * ---------------------------------------------------------------------------
 *
 * - src/gpu/crgpushared.c, crgpu___internal_check_pipeline_outputs()
 *   (called by all three backends): after the depth bias clamp, for each
 *   of the first color_attachment_count attachments with blend_enable, its
 *   factors at most CRGPU_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA and its ops at
 *   most CRGPU_BLEND_OP_MAX, in field order; then the independent-blend
 *   check as today; then, if an enabled attachment names a factor from 10
 *   to 13, the feature, then the count. One `if` per code.
 * - Metal (crgpumetalshader.m, crgpumetalcontext.m): the four factors in
 *   crgpumetal___internal_blend_factor(), and ZERO given its own case;
 *   the switches' defaults become unreachable (the check runs first) and
 *   assert in Debug. The bit with the other every-adapter features.
 * - Vulkan (crgpuvulkanshader.c, crgpuvulkancontext.c): the four factors
 *   in crgpuvulkan___blend_factor(), its default asserting; the bit where
 *   the adapter's facts are read, from `available.dualSrcBlend` and
 *   `props->limits.maxFragmentDualSrcAttachments >= 1`;
 *   `deviceFeatures.dualSrcBlend` on only for a device created with it.
 *   A disabled attachment's fields already stay zeroed.
 * - D3D12 (crgpud3d12shader.cpp, crgpud3d12context.cpp): the four factors
 *   in crgpu_d3d12_blend_factor(), which takes whether it fills an alpha
 *   slot and writes a *_COLOR factor there as its *_ALPHA twin (Q4 a);
 *   crgpu_d3d12___blend_desc() writes a target whose attachment is
 *   disabled, and target 0 with no colour attachment, as D3D12's default
 *   (BlendEnable FALSE, ONE, ZERO, ADD for colour and alpha). The bit with
 *   the other every-adapter features.
 *
 * ---------------------------------------------------------------------------
 * Records when built
 * ---------------------------------------------------------------------------
 *
 * carrottus/CLAUDE.md, "Carrottus GPU conventions" or "Known gaps": a line
 * on dual-source blending (the bit, one attachment, the shader's
 * declaration, the default Metal route only) and on the blend checks now
 * in crgpushared.c. cmake/cr_shaders.cmake's comment on METAL_ROUTE SLANG:
 * Slang's own Metal writes a second output as [[color(1)]], so a
 * dual-source shader takes the default route. CRGPU_DEVICE_TODO.md's S50
 * section: built, each machine's run and each Vulkan driver's two values.
 * The workspace's DECISIONS.md card S50: the draft approved and built.
 */
