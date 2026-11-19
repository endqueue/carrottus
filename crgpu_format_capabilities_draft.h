/*
 * crgpu_format_capabilities_draft.h - Carrottus GPU's per-format
 * capabilities: what an adapter can do with each CRGPU_FORMAT, read from
 * the graphics API, and a pipeline blending into a format that can't blend
 * refused (card S51, decided 2026-11-17: a).
 *
 * DRAFT, 2026-11-18, version 1 (seven critique passes), for the user's
 * approval. Nothing built yet.
 * APPROVED 2026-11-18 with the user, every question as recommended.
 *
 * Written unattended on the Mac (Apple M5 Pro, macOS 27) against
 * include/crgpu.h, include/crcore.h, src/gpu/crgpushared.c and .h,
 * src/gpu/crgpuformat.c, the three backends' format tables and adapter
 * reports (crgpuvulkancontext.c, crgpud3d12context.cpp, crgpud3d12image.cpp,
 * crgpumetalformat.m, crgpumetalcontext.m), their pipeline creation
 * (crgpuvulkanshader.c, crgpud3d12shader.cpp, crgpumetalshader.m) and
 * buffer-image copies (the three *renderpass files), abi.txt, the Vulkan
 * SDK 1.4.363.0's vulkan_core.h, vk.xml and validusage.json, DirectX-Headers'
 * d3d12.h (the copy under .refs/HazelRenderer2025/.../nvrhi/thirdparty/,
 * read as a header only) and the macOS 27 SDK's MTLDevice.h, as read on
 * 2026-11-18 (carrottus 0.4.0, untagged). Not built, not in any target, not
 * compilable: it repeats declarations of crgpu.h and crcore.h to show them
 * changed. Every declaration below is marked [ADDED], [CHANGED] or
 * [REMOVED]; nothing unmarked changes. Once approved it is frozen
 * (ROADMAP.md, section 1, "Work built from approved drafts").
 *
 * Read with card S52's draft (crgpu_copy_rows_draft.h, the same day): it
 * takes result codes 372 to 374, so this draft's Q2 (b), if chosen, takes
 * 375. S49's approved draft holds 371.
 *
 * ---------------------------------------------------------------------------
 * Revision notes (version 1 written, then seven passes, each fixed before
 * the next; one lens a pass, as the workspace's critique
 * prompts run them)
 * ---------------------------------------------------------------------------
 *
 * Pass 1 (wrong, missing, more than needed).
 *  1. Missing: storage reads. CRGPU_IMAGE_USAGE_STORAGE says "read and
 *     written by a shader" (crgpu.h line 2357), but D3D12 loads a typed
 *     UAV of most formats only with D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD,
 *     Metal reads a read_write texture only of its tier's formats
 *     (readWriteTextureSupport), and Vulkan reads one whose shader leaves
 *     the format unknown only with VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT.
 *     "storage" as the card lists it can't mean both on every API. A
 *     ninth bit, STORAGE_READ, is Q1 (a); (b) keeps the card's eight and
 *     says storage reads are undefined until a later card.
 *  2. Wrong: version 1 said a TYPELESS format is answered as the API
 *     answers it. The three backends disagree on what such an image is
 *     (Metal refuses one, crgpumetalformat.m line 24; Vulkan makes the
 *     UNORM or FLOAT format instead, crgpuvulkancontext.c line 54 and on;
 *     D3D12 makes a typeless resource with no typed view in Carrottus's
 *     view desc to read it through), so a bit read from any one API is a
 *     promise only that backend keeps. Q3 now.
 *  3. Missing: the video and palette formats. Vulkan samples its _422 and
 *     planar formats only through a sampler Y'CbCr conversion, which
 *     Carrottus doesn't make (card S7 left it as a possible EXT), and
 *     D3D12 reads its video formats through per-plane views Carrottus
 *     doesn't make (card S11). Folded into Q3: every bit but COPY is 0.
 *  4. Missing: the combined depth-stencil formats' COPY. A buffer-image
 *     copy takes one aspect on every API and crgpu_image_buffer_copy_desc
 *     names none; see "Found in the code", fault 2. COPY is 0 for them.
 *  5. Looked at for "more than needed": refusing the other uses (a
 *     colour attachment, a depth attachment, a storage view, a vertex
 *     attribute, a copy without its bit). The card decided the blend
 *     refusal alone; each other refusal is a behaviour break with its own
 *     tests. Q6 (a) cards them; nothing else here refuses anything.
 *  6. Missing: multisampling and resolving. No Carrottus image is
 *     multisampled yet; left out, said so.
 *
 * Pass 2 (the platforms and industry practice).
 *  1. Vulkan: the bits come from optimalTilingFeatures (every Carrottus
 *     image is VK_IMAGE_TILING_OPTIMAL, crgpuvulkanimage.c line 134) and
 *     VERTEX from bufferFeatures, read through vkGetPhysicalDeviceFormatProperties2
 *     with VkFormatProperties3 (core in 1.3, Carrottus's floor) so the
 *     64-bit STORAGE_*_WITHOUT_FORMAT bits are there. The blend refusal is
 *     Vulkan's own rule: VUID-VkGraphicsPipelineCreateInfo-renderPass-06062
 *     (dynamic rendering: blendEnable must be false for an attachment
 *     whose format lacks VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT).
 *  2. Vulkan's storage writes: Carrottus reads no module, so it can't know
 *     whether the shader declares the image's format. A shader that
 *     doesn't needs STORAGE_WRITE_WITHOUT_FORMAT (and from 1.3 nothing
 *     else: the per-format bit stands for the old device feature). The
 *     bit that holds whatever the shader declares is both, so STORAGE is
 *     STORAGE_IMAGE_BIT and VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT,
 *     and STORAGE_READ adds _READ_WITHOUT_FORMAT_BIT. Narrower than a
 *     shader that declares its format needs; never wider.
 *  3. D3D12: CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT) per format
 *     (d3d12.h line 2375), Support1 and Support2, on the probe device the
 *     instance already makes per adapter (crgpud3d12context.cpp line 91).
 *     Asked of the format D3D12 is handed for that use, which for the
 *     depth formats is not the image's: the SRV format
 *     (crgpu_d3d12_srv_format(), crgpud3d12image.cpp line 25) for SAMPLED
 *     and FILTERED, the DSV format for DEPTH_STENCIL_ATTACHMENT. D3D12 has
 *     no copy bit: COPY is any of TEXTURE1D/2D/3D (a texture CopyTextureRegion
 *     takes). Not settled here and owed to the Windows run: whether D3D12
 *     copies a region smaller than a depth subresource (D3D11's
 *     CopySubresourceRegion didn't); the D3D12 COPY of D16_UNORM and
 *     D32_FLOAT follows what that run finds, and the draft's test says so.
 *  4. Metal has no per-format query: MTLDevice.h offers
 *     supportsBCTextureCompression, supports32BitFloatFiltering,
 *     supports32BitMSAA and readWriteTextureSupport (lines 560 to 602 of the
 *     macOS 27 SDK's), and nothing else per format. The rest is Apple's
 *     "Metal feature set tables" (developer.apple.com, its pixel format
 *     capabilities by GPU family), which is how Metal itself states them.
 *     That document is not on this machine: the build session transcribes
 *     it, citing its date, one column per family the backend lists
 *     (Apple7 to Apple10, Mac2). Q7.
 *  5. WebGPU: GPUTextureFormat's "texture format capabilities" table
 *     (render attachment, blendable, multisampling, storage with
 *     write-only, read-only and read-write access, the last three per
 *     format), with "float32-filterable" and "float32-blendable" as
 *     features where the core table says no. The same split of storage
 *     access as Q1 (a), and the blend refusal is WebGPU's validation of a
 *     pipeline (a blend on a non-blendable target is an error).
 *  6. Industry: wgpu (Adapter::get_texture_format_features, TextureFormatFeatureFlags:
 *     FILTERABLE, BLENDABLE, STORAGE_READ_WRITE, multisample bits, and
 *     TextureUsages beside them), NVRHI (IDevice::queryFormatSupport,
 *     FormatSupport: Texture, DepthStencil, RenderTarget, Blendable,
 *     ShaderLoad, ShaderSample, ShaderUavLoad, ShaderUavStore, Buffer,
 *     IndexBuffer, VertexBuffer...), SDL_GPU (SDL_GPUTextureSupportsFormat,
 *     per type and usage), bgfx (bgfx::getCaps()->formats[], BGFX_CAPS_FORMAT_TEXTURE_*:
 *     2D, sRGB, vertex, image read/write, framebuffer, MSAA, mip
 *     autogen), sokol_gfx (sg_query_pixelformat: sample, filter, render,
 *     blend, msaa, depth, compressed, read, write). Every one reports per
 *     format; NVRHI's split of ShaderUavLoad from Store is Q1 (a)'s. As
 *     remembered of each project's public API: none is on this machine
 *     but NVRHI's DirectX-Headers copy, read for d3d12.h only.
 *
 * Pass 3 (the C API, CONVENTIONS.md).
 *  1. Shape: (adapter, format, out_flags), out_ last, a CR_RESULT; the
 *     name and the output's name as the card has them (out_capabilities
 *     was considered for the type's name and left: the card decided it).
 *  2. CRGPU_FORMAT_CAPABILITIES follows its neighbour CRGPU_IMAGE_USAGE:
 *     a uint32_t with an anonymous enum of values (not macros, as
 *     CRGPU_FEATURES must be past 32 bits), so the values reach abi.txt.
 *     Each value's Doxygen says what it promises and each API's source.
 *  3. The depth bit is named for the usage it answers,
 *     DEPTH_STENCIL_ATTACHMENT (CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT),
 *     not the card's shorter "depth attachment"; COLOR_ATTACHMENT and
 *     SAMPLED likewise match their usages.
 *  4. Checks in parameter order: the adapter (null, then destroyed in
 *     Debug, its own `if`), the format, then the output; nothing written
 *     on a refusal.
 *  5. [CHANGED] comments: CRGPU_IMAGE_USAGE_STORAGE (what reads need),
 *     crgpu_graphics_pipeline_desc's blend_attachments (the format's BLEND
 *     bit), crgpu_graphics_pipeline_create()'s @retval list (one line
 *     grown, in the order the checks run), crcore.h's 207 (Q2 a).
 *
 * Pass 4 (no hidden policy).
 *  1. Every bit is the API's own answer for the native format the backend
 *     makes for that use, and 0 where the backend makes none. Nothing is
 *     ranked, preferred or substituted, and no "best format" call exists.
 *  2. Q3's 0 is not a policy: it says Carrottus can't promise the use on
 *     this backend, which is the fact. The alternative (b), the stand-in's
 *     answer, would promise a use whose meaning differs by backend (an A8
 *     image sampled as (r, 0, 0, 1) on Vulkan and (0, 0, 0, a) on D3D12).
 *  3. Found while checking: Vulkan's format table stands one format in for
 *     another (crgpuvulkancontext.c: every TYPELESS line, A8_UNORM as R8
 *     at line 101 with an identity swizzle, crgpuvulkanimage.c line 338,
 *     B8G8R8X8 as B8G8R8A8 at lines 114 to 118, the depth view formats as
 *     the combined depth-stencil format) and maps three packed 16-bit
 *     formats to their channel-reversed twins (fault 1 below). The stand-
 *     ins are hidden policy as built; refusing them at image creation is a
 *     card of their own (Q6), the query here only declines to vouch for
 *     them. Fault 1 is a wrong translation and is fixed here, since the
 *     query would otherwise answer for the wrong format.
 *  4. Metal's table (Q7 a) is a transcription of what Apple states, not a
 *     choice; where MTLDevice answers (BC, 32-bit float filtering, the
 *     read-write tier) the property wins over the table.
 *
 * Pass 5 (threads and errors).
 *  1. Read once, when the instance lists its adapters, into the adapter's
 *     facts (crgpu___adapter_facts, crgpushared.h line 108), never
 *     changed: the call reads a table, lock-free, from any thread, like
 *     crgpu_adapter_get_properties(). No Thread: exception beyond that
 *     call's.
 *  2. Its failures are Carrottus's own checks, each its own `if` and code;
 *     no platform call is made by the call, so nothing reaches the
 *     instance's message callback (card S10). A failed CheckFeatureSupport
 *     at instance creation is that format's 0, not a failed instance: D3D12
 *     answers E_FAIL for a format the adapter has no support for at all,
 *     which is an answer. Said in the D3D12 notes.
 *  3. The pipeline refusal is before any platform call and leaves
 *     *out_pipeline NULL, as the existing refusals do.
 *  4. Q2 checked against CONVENTIONS.md, "Codes": the caller can ask the
 *     adapter before it builds the pipeline, so no caller branches on the
 *     refusal that the query didn't already let it branch on: (a) the
 *     general CR_RESULT_GPU_INVALID_FORMAT, whose meaning ("one the call or
 *     the backend can't use there") already covers it and which the same
 *     function returns for a block-compressed attachment.
 *
 * Pass 6 (the code as built; every file cited re-read, line numbers as of
 * 2026-11-18).
 *  1. The pipeline checks: crgpu___internal_check_pipeline_outputs(desc,
 *     features), crgpushared.c line 95, called by crgpumetalshader.m line
 *     354, crgpuvulkanshader.c line 319 and crgpud3d12shader.cpp line 693.
 *     It has no adapter: it gains the device's adapter facts (each
 *     backend's device holds `_adapter`: crgpuvulkan.h line 123,
 *     crgpumetal.h line 105, crgpud3d12.hpp line 251). The blend-format
 *     check goes last, after the dual-source count (line 154), so every
 *     existing refusal keeps its place in the order.
 *  2. The facts: one array of 161 uint16_t (322 bytes) in
 *     crgpu___adapter_facts, indexed 0 to 132 by value and 133 to 160 for
 *     ASTC's 1000 to 1027, as crgpuformat.c's texel-block table is split
 *     (its line 362, and the ASTC table after it). The format values are
 *     contiguous from 0 to 115, then 130 to 132, then 1000 to 1027
 *     (checked against crgpu.h's enum); 116 to 129 are unnamed.
 *  3. Vulkan: convert_cr_to_vulkan_format(), crgpuvulkancontext.c line
 *     13, read in crgpuvulkan___read_adapter_facts() (line 368), which
 *     already reads the adapter's limits and features once. Fault 1's
 *     three lines are 111, 112 and 119; vk.xml lists VK_FORMAT_B5G6R5_UNORM_PACK16's
 *     components B, G, R from the high bits, so its R is DXGI's B.
 *  4. Metal: crgpumetalformat.m's table (lines 46 to 130 and the ASTC
 *     table at 134) gains no column: the capabilities go in a second table
 *     beside it, per family, read in crgpumetal___internal_read_adapter_facts()
 *     (crgpumetalcontext.m line 18). Its entries say which formats have a
 *     pixel format at all (TYPELESS never, line 24; D24_UNORM_S8_UINT
 *     never, line 28) and which are vertex-only (the three 96-bit ones,
 *     lines 88 to 90).
 *  5. D3D12: crgpu_d3d12___read_adapter_facts(), crgpud3d12context.cpp
 *     line 91, holds the probe device; crgpu_d3d12_get_dxgi_format()
 *     (crgpud3d12.hpp line 70) and crgpu_d3d12_srv_format()
 *     (crgpud3d12image.cpp line 25) give the formats asked about.
 *  6. abi.txt: gains the function and nine constants; no line changes.
 *     crgpu_adapter_get_properties is its line 864, for form.
 *  7. Version 1's line numbers were re-read and four corrected (Metal's
 *     tables, its D24 and vertex-only lines, the dual-source count at
 *     crgpushared.c line 154, the swizzle at crgpuvulkanimage.c line
 *     338); and a claim that a caller's three targets all blend on the M5 Pro
 *     replaced by what the build will know (the version and the breaks).
 *
 * Pass 7 (tests, records, the build).
 *  1. Exact expected values only where the answer is certain on the
 *     machine named (Metal on the M5 Pro for formats whose answer is
 *     fixed by the pixel format's kind: a TYPELESS 0, a vertex-only format
 *     VERTEX alone, R32_UINT and BC7 below); elsewhere the invariants
 *     every API keeps, and the whole table printed for the TODO.
 *  2. A wrong "yes" in Metal's transcription is caught by using it: every
 *     bit an adapter reports is exercised under the API's validation with
 *     warnings fatal. A wrong "no" is caught by no test; the TODO records
 *     the table's source and date so it can be read again.
 *  3. The misuse harness gains the new call (a new function: regenerated
 *     and the diff expected to add it, nothing else).
 *  4. A caller's renderer: its pipelines blending into R16G16B16A16_FLOAT,
 *     R32G32B32A32_FLOAT and R11G11B10_FLOAT (it accepts all three as a
 *     blended target) are refused on a Vulkan adapter
 *     without their BLEND bit once this is built; its GPU tests rerun,
 *     and the behaviour on each Vulkan driver recorded.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * Carrottus GPU says nothing about what an adapter can do with a format.
 * A caller learns it by trying, and many tries aren't refused but reach
 * the API: Vulkan leaves blending into R32G32B32A32_SFLOAT and
 * B10G11R11_UFLOAT_PACK32 optional (neither is in the Vulkan
 * specification's table of required format support for
 * COLOR_ATTACHMENT_BLEND), and a caller's renderer blends over both today
 * (card A55's third critique round found it). On such a driver that
 * pipeline is outside Vulkan's rules (VUID-VkGraphicsPipelineCreateInfo-renderPass-06062);
 * Metal's validation ends the process for a blend into a pixel format
 * without "Blend"; D3D12's debug layer reports a blend into a format
 * without D3D12_FORMAT_SUPPORT1_BLENDABLE. Every API answers per format;
 * Carrottus passes none of it on.
 *
 * Found in the code drafting it (each a fault on this path):
 *
 *  1. Vulkan's format table maps CRGPU_FORMAT_B5G6R5_UNORM,
 *     _B5G5R5A1_UNORM and _B4G4R4A4_UNORM (DXGI's, whose names run from
 *     the low bits: B in bits 0 to 4) to VK_FORMAT_B5G6R5_UNORM_PACK16,
 *     _B5G5R5A1_UNORM_PACK16 and _B4G4R4A4_UNORM_PACK16 (Vulkan's packed
 *     names run from the high bits: B in bits 11 to 15). Red and blue are
 *     swapped on Vulkan against D3D12, and the 5551 and 4444 alpha moves.
 *     The right twins are VK_FORMAT_R5G6B5_UNORM_PACK16,
 *     _A1R5G5B5_UNORM_PACK16 and _A4R4G4B4_UNORM_PACK16 (the last core in
 *     1.3, its support optional, which the query then reports). Fixed
 *     here.
 *  2. A copy of a combined depth-stencil image (D24_UNORM_S8_UINT,
 *     D32_FLOAT_S8X24_UINT) passes the copies' checks (its texel block is
 *     4 or 8 bytes) and then does three different things: Vulkan sends
 *     both aspects in one region (cr_format_aspect(),
 *     crgpuvulkanrenderpass.c lines 99, 362 and 394), outside
 *     VUID-VkBufferImageCopy-aspectMask-09103 (one aspect); D3D12 copies
 *     plane 0 in a footprint of the combined format; Metal copies a
 *     Depth32Float_Stencil8 texture with no MTLBlitOption, which needs one.
 *     Not fixed here (COPY reports 0 for them); a card (Q6).
 *  3. CRGPU_IMAGE_USAGE_STORAGE promises reads that D3D12 and Metal keep
 *     for a few formats only (pass 1, 1). The usage's comment is changed
 *     here; nothing refuses yet (Q6).
 *
 * ---------------------------------------------------------------------------
 * What changes, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * crgpu_adapter_get_format_capabilities(adapter, format, out_flags) writes
 * a CRGPU_FORMAT_CAPABILITIES: SAMPLED, FILTERED, STORAGE, COLOR_ATTACHMENT,
 * BLEND, DEPTH_STENCIL_ATTACHMENT, VERTEX, COPY and (Q1 a) STORAGE_READ,
 * each the graphics API's own answer for the native format the backend
 * makes for that use, read when the instance lists its adapters. A
 * TYPELESS format, a format a backend stands another in for, and a video
 * or palette format report COPY alone at most (Q3 a). A graphics pipeline
 * whose enabled blend attachment's format lacks BLEND on the device's
 * adapter is refused with CR_RESULT_GPU_INVALID_FORMAT (Q2 a), checked
 * last. Vulkan's three packed 16-bit formats are mapped to the right
 * Vulkan formats.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 *  - Vulkan (vulkan_core.h and the registry of the SDK 1.4.363.0 on this
 *    machine): VkFormatFeatureFlagBits, SAMPLED_IMAGE (0x1), STORAGE_IMAGE
 *    (0x2), VERTEX_BUFFER (0x40), COLOR_ATTACHMENT (0x80),
 *    COLOR_ATTACHMENT_BLEND (0x100), DEPTH_STENCIL_ATTACHMENT (0x200),
 *    SAMPLED_IMAGE_FILTER_LINEAR (0x1000), TRANSFER_SRC (0x4000),
 *    TRANSFER_DST (0x8000) (lines 2715 to 2729); VkFormatFeatureFlagBits2's
 *    STORAGE_READ_WITHOUT_FORMAT (0x80000000, line 7344) and
 *    STORAGE_WRITE_WITHOUT_FORMAT, read through VkFormatProperties3.
 *    Optimal tiling for images, buffer features for vertices. The
 *    blend VUID above; VUID-vkCmdDraw-OpTypeImage-07028 for reading a
 *    storage image of unknown format.
 *  - D3D12 (d3d12.h, DirectX-Headers): D3D12_FEATURE_FORMAT_SUPPORT (3),
 *    D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER (0x2), TEXTURE2D (0x20),
 *    TEXTURE3D (0x40), SHADER_LOAD (0x100), SHADER_SAMPLE (0x200),
 *    RENDER_TARGET (0x4000), BLENDABLE (0x8000), DEPTH_STENCIL (0x10000),
 *    TYPED_UNORDERED_ACCESS_VIEW (0x2000000); D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD
 *    (0x40) and _STORE (0x80) (lines 2466 to 2508);
 *    D3D12_FEATURE_DATA_D3D12_OPTIONS::TypedUAVLoadAdditionalFormats
 *    (line 2574), which Support2's load bit already reflects per format.
 *  - Metal (MTLDevice.h, macOS 27 SDK): no per-format query; the device
 *    properties above; MTLReadWriteTextureTier1 (R32Float, R32Uint,
 *    R32Sint) and Tier2 (adds the RGBA32, RGBA16, RGBA8, R16 and R8 float,
 *    unorm, uint and sint formats Apple lists) for STORAGE_READ; Apple's
 *    feature set tables for the rest (pass 2, 4).
 *  - WebGPU and industry practice: pass 2, 5 and 6.
 *
 * Hazel is not compared: Carrottus is not measured against an engine (the
 * workspace's CLAUDE.md, "External references").
 *
 * ---------------------------------------------------------------------------
 * Each bit, on each backend
 * ---------------------------------------------------------------------------
 *
 * Read for the format the backend makes for that use; 0 where it makes
 * none (an unmapped format, a pixel format Metal lacks). Vulkan: optimal
 * tiling features unless said. D3D12: Support1/Support2 of the format
 * named. Metal: the union of the transcribed columns of the families the
 * GPU is in, then the MTLDevice property where one is named.
 *
 *  SAMPLED          Vulkan SAMPLED_IMAGE. D3D12 SHADER_LOAD of the SRV
 *                   format. Metal: every pixel format the table maps
 *                   (Metal reads every pixel format a texture can have).
 *  FILTERED         Vulkan SAMPLED_IMAGE_FILTER_LINEAR. D3D12 SHADER_SAMPLE
 *                   of the SRV format. Metal "Filter", and for R32Float,
 *                   RG32Float and RGBA32Float supports32BitFloatFiltering.
 *  STORAGE          Vulkan STORAGE_IMAGE and STORAGE_WRITE_WITHOUT_FORMAT.
 *                   D3D12 TYPED_UNORDERED_ACCESS_VIEW and UAV_TYPED_STORE.
 *                   Metal "Write".
 *  COLOR_ATTACHMENT Vulkan COLOR_ATTACHMENT. D3D12 RENDER_TARGET. Metal
 *                   "Color".
 *  BLEND            Vulkan COLOR_ATTACHMENT_BLEND. D3D12 BLENDABLE. Metal
 *                   "Blend".
 *  DEPTH_STENCIL_ATTACHMENT  Vulkan DEPTH_STENCIL_ATTACHMENT. D3D12
 *                   DEPTH_STENCIL of the depth format. Metal: Depth16Unorm,
 *                   Depth32Float and Depth32Float_Stencil8 (every Metal 3
 *                   GPU), none other.
 *  VERTEX           Vulkan bufferFeatures VERTEX_BUFFER. D3D12
 *                   IA_VERTEX_BUFFER. Metal: the table's MTLVertexFormat is
 *                   valid (the same on every Metal GPU).
 *  COPY             The format has a texel block (crgpu_format_get_texel_block())
 *                   and one aspect, and: Vulkan TRANSFER_SRC and
 *                   TRANSFER_DST; D3D12 TEXTURE1D, TEXTURE2D or TEXTURE3D
 *                   (and, for D16_UNORM and D32_FLOAT, what the Windows run
 *                   finds of a partial region; pass 2, 3); Metal a mapped
 *                   pixel format.
 *  STORAGE_READ     (Q1 a) Vulkan STORAGE_IMAGE and
 *                   STORAGE_READ_WITHOUT_FORMAT. D3D12
 *                   TYPED_UNORDERED_ACCESS_VIEW and UAV_TYPED_LOAD. Metal
 *                   the format in readWriteTextureSupport's tier.
 *
 * The block-compressed formats report what the adapter does with them:
 * BC7 is SAMPLED, FILTERED and COPY on an adapter with
 * CRGPU_FEATURE_TEXTURE_COMPRESSION_BC whether or not a device was created
 * with it; an image of it still needs a device created with the bit.
 *
 * ---------------------------------------------------------------------------
 * What the caller decides, and what Carrottus GPU only reports
 * ---------------------------------------------------------------------------
 *
 * Reported: each bit, per adapter and format. Decided by the caller: which
 * formats to make images of, which to render and blend into, and what to
 * do where a bit is missing (another format, no blend, another pass).
 * Carrottus GPU never picks a format, never substitutes one, and never
 * turns blending off.
 *
 * ---------------------------------------------------------------------------
 * Who else uses it
 * ---------------------------------------------------------------------------
 *
 *  - A renderer choosing its HDR target: R16G16B16A16_FLOAT blends on
 *    every API; R32G32B32A32_FLOAT and R11G11B10_FLOAT may not (card A55's
 *    caller, refused once this is built where they don't).
 *  - Storage writes and reads per format (a compute pass's read-write
 *    image), linear filtering of 32-bit float data (a height field), and
 *    the vertex formats a mesh loader may use.
 *  - Carrottus on Android (card S47), where optional format support is
 *    wider than on desktop.
 *
 * ---------------------------------------------------------------------------
 * Left out, and why
 * ---------------------------------------------------------------------------
 *
 *  - Multisampling, resolving and per-sample-count support: no Carrottus
 *    image is multisampled.
 *  - Atomics on storage images, sparse residency, linear tiling, a
 *    format's largest image size (vkGetPhysicalDeviceImageFormatProperties):
 *    no caller asks; each a bit or call of its own when one does.
 *  - Texel buffers (uniform and storage texel buffer views): their own
 *    bits when a caller needs them.
 *  - A "best format for" call: a choice, the caller's.
 *  - Refusing the other uses by the same table: Q6.
 *
 * ---------------------------------------------------------------------------
 * The version and the breaks
 * ---------------------------------------------------------------------------
 *
 *  - Added: crgpu_adapter_get_format_capabilities(), the type
 *    CRGPU_FORMAT_CAPABILITIES and nine constants; abi.txt gains ten
 *    lines.
 *  - crgpu_graphics_pipeline_create() refuses what it passed before, a
 *    behaviour break, listed: an enabled blend attachment whose format
 *    can't blend on the adapter (an integer format, UNKNOWN, a format
 *    with no colour attachment support, a float format a driver can't
 *    blend). Searched: a caller's renderer blends into R16G16B16A16_FLOAT,
 *    R32G32B32A32_FLOAT or R11G11B10_FLOAT targets;
 *    whether each blends on the M5 Pro is what the transcribed table and
 *    the test say (one that doesn't is refused there too, and recorded),
 *    and on a Vulkan driver without the bit they are refused where they
 *    were outside Vulkan's rules before. The building session reruns
 *    that caller's GPU tests and lists what each driver refuses.
 *  - On Vulkan, B5G6R5_UNORM, B5G5R5A1_UNORM and B4G4R4A4_UNORM images
 *    now hold their channels where D3D12's do (fault 1). No caller in the
 *    workspace uses them (searched: none outside carrottus' format tables).
 *  - No version raise: carrottus' 0.4.0 is untagged (card X2).
 *    ./check.sh --update-abi lists the additions; the misuse generator is
 *    rerun.
 *
 * New handles: none. New struct types: none (the output is a plain
 * integer). Struct layouts: unchanged. Stored formats touched: none.
 *
 * Thread: crgpu_adapter_get_format_capabilities() as
 * crgpu_adapter_get_properties(): may overlap any call on the adapter or
 * its instance but crgpu_instance_destroy(). crgpu_graphics_pipeline_create()
 * keeps the device's rule.
 *
 * Allocation: none at the call. The table is 322 bytes inside each
 * adapter's facts, which the instance already allocates.
 *
 * Instance creation: Vulkan asks 161 vkGetPhysicalDeviceFormatProperties2
 * per adapter; D3D12 up to four CheckFeatureSupport per format (the image's,
 * SRV, depth and vertex format where they differ) on the probe device it
 * already makes; Metal reads a table. The building session measures the
 * instance's creation before and after on each machine.
 *
 * ---------------------------------------------------------------------------
 * Questions (each with options and a recommendation)
 * ---------------------------------------------------------------------------
 *
 * Q1. READING A STORAGE IMAGE.
 *     (a) RECOMMENDED. A ninth bit, STORAGE_READ: STORAGE says the format
 *         is written through a storage binding, STORAGE_READ that it is
 *         also read there. Each API reports the two apart (D3D12's
 *         UAV_TYPED_LOAD and _STORE, Metal's read-write tiers, Vulkan's
 *         _WITHOUT_FORMAT pair), WebGPU and NVRHI split them, and
 *         CRGPU_IMAGE_USAGE_STORAGE's "read and written" is today a
 *         promise for most formats on no backend but Vulkan.
 *     (b) The card's eight: STORAGE means written, and what a shader reads
 *         from a storage image is undefined until a later card.
 *
 * Q2. THE REFUSAL'S CODE.
 *     (a) RECOMMENDED. CR_RESULT_GPU_INVALID_FORMAT (207), its comment
 *         grown by a clause: the general code (CONVENTIONS.md, "Codes"),
 *         the one this function returns for a block-compressed attachment,
 *         and no caller branches on the refusal that couldn't ask first.
 *     (b) A code of its own, CR_RESULT_GPU_FORMAT_NOT_BLENDABLE (375,
 *         after S52's 372 to 374), for a caller that builds first and asks
 *         after.
 *
 * Q3. FORMATS WHOSE USE DIFFERS BY BACKEND: every TYPELESS format, the
 *     formats a backend makes as another (Vulkan's A8_UNORM, B8G8R8X8_UNORM
 *     and _SRGB, and the depth view formats R32_FLOAT_X8X24_TYPELESS,
 *     X32_TYPELESS_G8X24_UINT, R24_UNORM_X8_TYPELESS, X24_TYPELESS_G8_UINT,
 *     which are TYPELESS-named already), and the video and palette formats
 *     (the 4:2:2 packed, the planar, AYUV, Y410, Y416, AI44, IA44, P8,
 *     A8P8).
 *     (a) RECOMMENDED. Every bit but COPY is 0 for them on every backend;
 *         COPY is read as for any format (bytes copy the same through a
 *         stand-in). The query never vouches for a use whose meaning
 *         differs by backend. The stand-ins themselves stay creatable
 *         until Q6's card.
 *     (b) The API's answer for whatever the backend makes: true per
 *         backend, but a Vulkan A8 image "sampled" reads (r, 0, 0, 1).
 *
 * Q4. A VALUE CRGPU_FORMAT DOESN'T NAME, AND UNKNOWN.
 *     (a) RECOMMENDED. Refused with CR_RESULT_GPU_INVALID_FORMAT, nothing
 *         written: "not a format" is told apart from "a format this
 *         adapter does nothing with" (CR_SUCCESS and 0).
 *     (b) CR_SUCCESS and 0 for both.
 *
 * Q5. WHEN THE TABLE IS READ.
 *     (a) RECOMMENDED. At instance creation, with the adapter's other
 *         facts, into a fixed array: the call is lock-free and allocation
 *         free, and D3D12 can only answer through the probe device it
 *         releases before the next adapter.
 *     (b) At each call on Vulkan and Metal: no table, but D3D12 can't
 *         (no device is held), so the backends would differ in cost and
 *         in what may fail.
 *
 * Q6. THE OTHER USES THE TABLE COULD REFUSE (an image's usages against its
 *     format's bits; a pipeline's colour and depth formats and vertex
 *     attributes; a combined depth-stencil copy, fault 2; Vulkan's
 *     stand-ins, refused at creation as Metal refuses TYPELESS).
 *     (a) RECOMMENDED. A card of its own, after this is built, each
 *         refusal with its test on every backend: each is a behaviour
 *         break that may land on a caller's working path.
 *     (b) All of them in this draft.
 *
 * Q7. HOW METAL'S ANSWERS ARE KNOWN.
 *     (a) RECOMMENDED. Apple's feature set tables transcribed into the
 *         backend, one column per family it lists (Apple7 to Apple10,
 *         Mac2), their date in the file; the MTLDevice property where one
 *         exists; every "yes" exercised by the test under validation.
 *     (b) Probing at instance creation (a 1 x 1 texture and pipeline per
 *         format and use): Metal's validation ends the process on a
 *         refused combination, and a probe that "works" without
 *         validation proves nothing.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 *  - crgpu_device_test, every adapter of each backend:
 *     - every named format (146 and UNKNOWN's refusal): CR_SUCCESS, the
 *       table printed (format, hex flags) for CRGPU_DEVICE_TODO.md;
 *     - refused, the output's sentinel 0xDEADBEEF unchanged: a NULL
 *       adapter (CR_RESULT_GPU_INVALID_ADAPTER), a NULL out_flags
 *       (CR_RESULT_NULL_OUTPUT_PARAMETER), and formats 0, 116, 129, 133,
 *       999, 1028 and UINT32_MAX (CR_RESULT_GPU_INVALID_FORMAT, Q4 a); the
 *       order with two wrong (NULL adapter and format 116: INVALID_ADAPTER;
 *       format 116 and NULL out: INVALID_FORMAT);
 *     - invariants on every adapter: FILTERED implies SAMPLED; BLEND
 *       implies COLOR_ATTACHMENT; STORAGE_READ implies STORAGE; Q3's
 *       formats have no bit but COPY; R1_UNORM and the eight planar
 *       formats have no COPY (no texel block); D24_UNORM_S8_UINT and
 *       D32_FLOAT_S8X24_UINT have no COPY (fault 2); a BC or ASTC format
 *       has nothing but SAMPLED, FILTERED and COPY, and nothing without
 *       its family's feature bit; no integer format has FILTERED or
 *       BLEND.
 *     - exact on the M5 Pro (Metal), where the pixel format's kind fixes
 *       it: R32G32B32_FLOAT 0x40 (VERTEX: a vertex-only entry);
 *       R8G8B8A8_TYPELESS 0; D24_UNORM_S8_UINT 0 (no pixel format on Apple
 *       Silicon); BC7_UNORM and ASTC_4X4_UNORM 0x83 (SAMPLED, FILTERED,
 *       COPY); R32_UINT 0x1CD (SAMPLED, STORAGE, COLOR_ATTACHMENT,
 *       VERTEX, COPY, STORAGE_READ: Tier 1 has R32Uint). Every other
 *       format compared with the transcribed table by the test itself.
 *  - crgpu_objects_test, every adapter: each bit an adapter reports is
 *    used, under Metal API and shader validation with warnings fatal,
 *    D3D12's debug layer and GPU-based validation, Vulkan's validation and
 *    synchronization validation: SAMPLED read by Load, FILTERED sampled
 *    linearly between two texels, STORAGE written and STORAGE_READ read
 *    back by a compute shader (one per component class: float, uint,
 *    sint; the format unknown to the shader), COLOR_ATTACHMENT cleared and
 *    drawn, BLEND drawn with ONE / ONE over a known value, DEPTH_STENCIL_ATTACHMENT
 *    cleared and depth-tested, VERTEX fetched by a draw, COPY up and back
 *    a 4 x 4 region; each image read back and compared where its value is
 *    exact. Each use's absence is not tried (a refused combination ends
 *    Metal's process).
 *  - The refusal, every backend: a pipeline of one R32_UINT attachment
 *    with blend_enable true, refused CR_RESULT_GPU_INVALID_FORMAT with
 *    out_pipeline NULL (no API blends an integer format), and accepted
 *    with blend_enable false; an UNKNOWN attachment blended, refused; a
 *    two-attachment pipeline (independent blend) of R8G8B8A8_UNORM blended
 *    and R32_UINT unblended, accepted; the order: R32_UINT blended with a
 *    factor of 14 gives CR_RESULT_GPU_INVALID_BLEND_FACTOR, with an SRC1
 *    factor on a device without CRGPU_FEATURE_DUAL_SOURCE_BLEND
 *    CR_RESULT_GPU_FEATURE_NOT_SUPPORTED, both before the format.
 *    On a Vulkan driver without BLEND for R32G32B32A32_FLOAT, that
 *    pipeline refused (recorded per driver).
 *  - Fault 1, Vulkan: a 1 x 1 B5G6R5_UNORM image cleared to (1, 0, 0, 1)
 *    and copied out reads 0xF800 as a 16-bit word (red in the high bits,
 *    as DXGI's B5G6R5 lays it out), on D3D12 as well.
 *  - cr_noalloc_tests: the call once a frame, none of ours.
 *  - The misuse harness: regenerated; the diff adds the new call's
 *    entries (zeroes, and its adapter as a stale handle where the harness
 *    makes one), nothing else.
 *  - A caller's GPU tests (the renderer above) on every adapter: unchanged on the
 *    Mac; what each Vulkan driver now refuses recorded.
 *  - Where each runs: the Mac (Apple M5 Pro; macos-metal Debug and
 *    Release under Metal validation, -asan, -tsan). Vulkan's half
 *    type-checked there against the SDK's headers; D3D12's written, not
 *    compiled. Owed: Windows 11 (Iris Xe, RTX A2000 and WARP over D3D12;
 *    the Iris Xe and the A2000 over Vulkan) and Fedora (ANV, NVK, RADV),
 *    each adapter's table recorded.
 *  - ./check.sh: abi, doxygen, conventions, caller-names.
 *
 * ---------------------------------------------------------------------------
 * crgpu.h
 * ---------------------------------------------------------------------------
 */

/**
 * @brief [ADDED] What an adapter can do with a format: a bitmask of
 * CRGPU_FORMAT_CAPABILITY_* values, from crgpu_adapter_get_format_capabilities().
 *
 * Each bit is the graphics API's own answer for the format the backend
 * makes for that use, read when the instance lists its adapters. A bit's
 * meaning never changes; new uses are new bits.
 *
 * Why: card S51. ABI: a type and nine values added. Tests:
 * crgpu_device_test, crgpu_objects_test.
 */
typedef uint32_t CRGPU_FORMAT_CAPABILITIES;

/** @{ */
enum {
	/** [ADDED] Read by a shader through a sampled image binding
	 * (CRGPU_IMAGE_USAGE_SAMPLED): loaded, or sampled with nearest
	 * filtering. Vulkan: `SAMPLED_IMAGE`. D3D12: `SHADER_LOAD` of the
	 * format a shader reads it in (a depth format's readable twin). Metal:
	 * every pixel format the backend maps. */
	CRGPU_FORMAT_CAPABILITY_SAMPLED                  = 1u << 0,
	/** [ADDED] Sampled with linear filtering. Never without SAMPLED.
	 * Vulkan: `SAMPLED_IMAGE_FILTER_LINEAR`. D3D12: `SHADER_SAMPLE`. Metal:
	 * the feature set tables' "Filter", and for the 32-bit float formats
	 * `supports32BitFloatFiltering`. */
	CRGPU_FORMAT_CAPABILITY_FILTERED                 = 1u << 1,
	/** [ADDED] Written by a shader through a storage image binding
	 * (CRGPU_IMAGE_USAGE_STORAGE), whether or not the shader declares the
	 * format. Vulkan: `STORAGE_IMAGE` and `STORAGE_WRITE_WITHOUT_FORMAT`.
	 * D3D12: `TYPED_UNORDERED_ACCESS_VIEW` and `UAV_TYPED_STORE`. Metal:
	 * "Write". */
	CRGPU_FORMAT_CAPABILITY_STORAGE                  = 1u << 2,
	/** [ADDED] Rendered into as a colour attachment
	 * (CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT). Vulkan: `COLOR_ATTACHMENT`.
	 * D3D12: `RENDER_TARGET`. Metal: "Color". */
	CRGPU_FORMAT_CAPABILITY_COLOR_ATTACHMENT         = 1u << 3,
	/** [ADDED] Blended into as a colour attachment: a graphics pipeline
	 * whose enabled blend attachment has a format without it is refused
	 * with CR_RESULT_GPU_INVALID_FORMAT. Never without COLOR_ATTACHMENT.
	 * Vulkan: `COLOR_ATTACHMENT_BLEND`. D3D12: `BLENDABLE`. Metal:
	 * "Blend". */
	CRGPU_FORMAT_CAPABILITY_BLEND                    = 1u << 4,
	/** [ADDED] Rendered into as the depth/stencil attachment
	 * (CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT). Vulkan:
	 * `DEPTH_STENCIL_ATTACHMENT`. D3D12: `DEPTH_STENCIL`. Metal: the
	 * depth formats it maps (D16_UNORM, D32_FLOAT, D32_FLOAT_S8X24_UINT). */
	CRGPU_FORMAT_CAPABILITY_DEPTH_STENCIL_ATTACHMENT = 1u << 5,
	/** [ADDED] A vertex attribute's format
	 * (crgpu_vertex_attribute_desc::format). Vulkan: the buffer features'
	 * `VERTEX_BUFFER`. D3D12: `IA_VERTEX_BUFFER`. Metal: an MTLVertexFormat
	 * the backend maps. */
	CRGPU_FORMAT_CAPABILITY_VERTEX                   = 1u << 6,
	/** [ADDED] Copied both ways between a buffer and an image by
	 * crgpu_command_list_copy_buffer_to_image() and _copy_image_to_buffer():
	 * the format has a texel block and one aspect (never a combined
	 * depth-stencil format, whose aspect no copy desc names), and Vulkan
	 * `TRANSFER_SRC` and `TRANSFER_DST`; D3D12 a texture format; Metal a
	 * pixel format it maps. */
	CRGPU_FORMAT_CAPABILITY_COPY                     = 1u << 7,
	/** [ADDED] (Q1 a) Read, as well as written, by a shader through a
	 * storage image binding, whether or not the shader declares the format.
	 * Never without STORAGE. Vulkan: `STORAGE_IMAGE` and
	 * `STORAGE_READ_WITHOUT_FORMAT`. D3D12: `UAV_TYPED_LOAD` with
	 * STORAGE's bits. Metal: the format is in the device's
	 * `readWriteTextureSupport` tier. */
	CRGPU_FORMAT_CAPABILITY_STORAGE_READ             = 1u << 8
};
/** @} */

/**
 * @brief [ADDED] Reports what an adapter can do with a format.
 *
 * Each bit is the graphics API's answer for the native format this backend
 * makes for that use, read when the instance was created: Vulkan's format
 * properties, D3D12's `CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT)`,
 * and on Metal, which has no such query, Apple's feature set tables by GPU
 * family with the device's own properties where it has one
 * (CRGPU_FORMAT_CAPABILITIES). A format the backend makes nothing of
 * reports 0. A TYPELESS format, a format a backend makes as another, and a
 * video or palette format report CRGPU_FORMAT_CAPABILITY_COPY at most: what
 * the rest would mean differs by backend.
 *
 * The answer is the adapter's: an image of a block-compressed format still
 * needs a device created with its family's feature bit. Two adapters, or
 * two backends, may answer differently; that is what the call is for.
 *
 * Checks `adapter`, then `format`, then `out_flags`; nothing is written on a
 * refusal. A destroyed adapter's instance is refused as a NULL one in
 * Debug.
 *
 * Thread: may overlap any call on the same adapter or its instance,
 * crgpu_device_create() on it included, except crgpu_instance_destroy().
 * It reads what the instance recorded. Since 0.4.0.
 *
 * Why: card S51. ABI: a function added. Tests: crgpu_device_test,
 * crgpu_objects_test, the misuse harness.
 *
 * @param[in]  adapter   Adapter to ask about.
 * @param[in]  format    Format to ask about: any value CRGPU_FORMAT names
 *                       but CRGPU_FORMAT_UNKNOWN.
 * @param[out] out_flags Receives its CRGPU_FORMAT_CAPABILITIES; 0 is an
 *                       answer, not a failure.
 *
 * @retval CR_SUCCESS `out_flags` was written.
 * @retval CR_RESULT_GPU_INVALID_ADAPTER `adapter` is NULL.
 * @retval CR_RESULT_GPU_INVALID_FORMAT `format` is CRGPU_FORMAT_UNKNOWN or a
 *         value CRGPU_FORMAT doesn't name (116 to 129, 133 to 999, past
 *         1027). Nothing is written.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_flags` is NULL.
 */
CR_RESULT crgpu_adapter_get_format_capabilities(
    crgpu_adapter adapter,
    CRGPU_FORMAT format,
    CRGPU_FORMAT_CAPABILITIES *out_flags);

/*
 * CRGPU_IMAGE_USAGE_STORAGE, [CHANGED] (comment only, Q1 a):
 *
 *	/** Written by a shader through a storage image binding, and read there
 *	 * where the format has CRGPU_FORMAT_CAPABILITY_STORAGE_READ on the
 *	 * device's adapter; what a shader reads from a storage image of
 *	 * another format is undefined. * /
 *	CRGPU_IMAGE_USAGE_STORAGE                  = 1 << 3,
 *
 * crgpu_graphics_pipeline_desc::blend_attachments, [CHANGED] (comment
 * only), a sentence appended:
 *
 *	 * ... An entry with blend_enable needs its format
 *	 * (#color_attachment_formats[i]) to have CRGPU_FORMAT_CAPABILITY_BLEND
 *	 * on the device's adapter (crgpu_adapter_get_format_capabilities()).
 */

/**
 * @brief Creates a graphics pipeline.
 *
 * ... the description and every other result unchanged ...
 *
 * [CHANGED] one result grown, the check placed last of the blend checks:
 *
 * @retval CR_RESULT_GPU_INVALID_FORMAT A block-compressed (BC or ASTC)
 *         format ... (unchanged, checked where it is); or, checked after
 *         every blend check above (the dual-source count included), the
 *         format of one of the first `desc->color_attachment_count`
 *         attachments with blend_enable lacks CRGPU_FORMAT_CAPABILITY_BLEND
 *         on the device's adapter, attachments in index order. Since 0.4.0.
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
 *	/** [CHANGED] A format is CRGPU_FORMAT_UNKNOWN, or one the call or the
 *	 * backend can't use there, or one the adapter can't use that way
 *	 * (crgpu_adapter_get_format_capabilities()). * /
 *	CR_RESULT_GPU_INVALID_FORMAT                              = 207,
 *
 * No new code (Q2 a).
 *
 * ---------------------------------------------------------------------------
 * Where each backend does it (not in a header)
 * ---------------------------------------------------------------------------
 *
 * - src/gpu/crgpushared.h and .c: crgpu___adapter_facts gains
 *   `uint16_t format_capabilities[161]`; crgpu___internal_format_index(fmt)
 *   gives 0 to 132 by value and 133 to 160 for 1000 to 1027, or -1 for a
 *   value not named (116 to 129 included); crgpu___internal_get_format_capabilities()
 *   makes the call's checks after the adapter's and reads the table (Q3's
 *   formats masked to COPY there, once, for every backend);
 *   crgpu___internal_check_pipeline_outputs() takes the adapter's facts
 *   and checks each enabled attachment's format last.
 * - Vulkan (crgpuvulkancontext.c): in crgpuvulkan___read_adapter_facts(),
 *   for each named format, vkGetPhysicalDeviceFormatProperties2 with
 *   VkFormatProperties3 for convert_cr_to_vulkan_format()'s VkFormat
 *   (UNDEFINED: 0), the bits as listed above. Fault 1: lines 111, 112 and
 *   119 return VK_FORMAT_R5G6B5_UNORM_PACK16, _A1R5G5B5_UNORM_PACK16 and
 *   _A4R4G4B4_UNORM_PACK16.
 * - D3D12 (crgpud3d12context.cpp): in crgpu_d3d12___read_adapter_facts(),
 *   on the probe device, CheckFeatureSupport(FORMAT_SUPPORT) for the
 *   image's DXGI format, its SRV format (crgpu_d3d12_srv_format(), moved
 *   to crgpud3d12.hpp to be shared) and, for vertex attributes, the same
 *   DXGI format; a failed call is that format's 0.
 * - Metal (crgpumetalformat.m, crgpumetalcontext.m): a second table beside
 *   crgpumetal___formats, the capabilities per family column, transcribed
 *   from Apple's feature set tables with their date; in
 *   crgpumetal___internal_read_adapter_facts(), the union of the columns
 *   of the families the GPU is in (supportsFamily), then BC from
 *   supportsBCTextureCompression, ASTC from MTLGPUFamilyApple2, the 32-bit
 *   float filtering and the read-write tier from MTLDevice.
 * - crgpu_adapter_get_format_capabilities() in each backend's context file:
 *   the adapter's null and stale checks, then the shared function.
 *
 * ---------------------------------------------------------------------------
 * Records when built
 * ---------------------------------------------------------------------------
 *
 * carrottus/CLAUDE.md, "Carrottus GPU conventions": a line on per-format
 * capabilities (read at instance creation; Metal's from Apple's tables,
 * the date; Q3's formats COPY at most; the blend refusal) and on fault 1.
 * CRGPU_DEVICE_TODO.md's S51 section: built, each adapter's table on each
 * machine. The card for Q6. The workspace's DECISIONS.md card S51: the
 * draft approved and built; ROADMAP.md as the session's record says.
 */
