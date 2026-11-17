/*
 * crgpu_astc_draft.h - Carrottus GPU's block-compressed formats: ASTC
 * added, BC finished on every backend, and the copies that move them
 * (card S48, which names the session and the first caller: a caller
 * shipping BC7 packs for desktop GPUs and ASTC 4x4 packs for iPad and
 * iPhone, Apple Silicon Macs either).
 *
 * DRAFT, 2026-11-16, version 8 (version 1, then seven critique passes,
 * each fixed before the next: the revision notes below), for the user's
 * approval. Written unattended
 * on the Mac (Apple M5 Pro) against include/crgpu.h, include/crcore.h,
 * src/gpu/crgpuformat.c and the three backends' format tables and copy
 * code (crgpumetalformat.m, crgpumetalrenderpass.m, crgpuvulkancontext.c,
 * crgpuvulkanrenderpass.c, crgpud3d12.hpp, crgpud3d12renderpass.cpp) as
 * read on 2026-11-16 (carrottus 0.4.0). Not built, not in any target, not
 * compilable: it repeats declarations of crgpu.h to show them changed.
 * Every declaration below is marked [ADDED], [CHANGED] or [REMOVED];
 * nothing unmarked changes. Nothing is built until it is approved; once
 * approved it is frozen (ROADMAP.md, section 1, "Work built from approved
 * drafts").
 *
 * ---------------------------------------------------------------------------
 * Revision notes (the seven passes, 2026-11-16; lenses as the workspace's
 * critique prompt's, named on card S48)
 * ---------------------------------------------------------------------------
 *
 * 2026-11-17: caller names removed (check.sh caller-names), meaning kept.
 *
 * Pass 1 (wrong, missing, more than the need). 1. Version 1 said ASTC was
 * absent on "a Mac2 GPU": an Apple GPU answers yes to MTLGPUFamilyMac2 as
 * well (measured, pass 5), so the absence is a GPU in Mac2 and in no Apple
 * family (an Intel or AMD Mac). Changed in "What was compared" and the
 * bit's comment. 2. Missing: a pipeline's colour or depth attachment
 * formats and its vertex attribute formats are CRGPU_FORMAT values too,
 * and D3D12's pipeline creation casts them as the image create does: an
 * ASTC value there would reach DXGI. Changed: the pipeline creates refuse
 * a block format as an attachment or a vertex attribute
 * (CR_RESULT_GPU_INVALID_FORMAT), in crgpushared.c beside the colour
 * attachment checks already there.
 *
 * Pass 2 (what breaks an existing caller). 1. Rend's
 * format-to-scalar-type function takes every value from CRGPU_FORMAT_AYUV
 * up for a video format: ASTC's 1000 and on fall into it. Not Carrottus's
 * to change; named under "The version and the breaks", and fixed in
 * Rabbittus' block-formats draft (card A54). 2.
 * Checked: Carrottus's own code range-tests formats only in
 * crgpu___internal_format_block_bytes() (BC's two ranges, replaced by the
 * table) and Metal's table bound (an index past it is no entry): neither
 * mistakes 1000 for something else. 3. Checked: the misuse harness calls
 * both copies with destroyed and null handles (cr_misuse_tests.c, 2512 and
 * 4396); regenerated, it expects their results. No CR_RESULT is marked
 * unused-result, so callers that ignore the copies' results (Rend,
 * crgpusamples' HelloTexture, CrGPUHeaps) compile unchanged. 4. Searched
 * every test and sample: none creates a BC image, so the BC bit breaks no
 * test.
 *
 * Pass 3 (CONVENTIONS.md). 1. crgpu_format_get_texel_block() listed its
 * output check before its format check; checks go in parameter order.
 * Changed: `format`, then each output in order, nothing written on a
 * refusal. 2. The copies didn't say what a destroyed handle gives, nor
 * that `desc` is read during the call only. Changed: a destroyed handle
 * is refused as a null one (CR_STALE, after each null check), `desc`
 * borrowed for the call. 3. Looked at and kept: the block table is a
 * fixed property of each format, a named table beside the code that reads
 * it (crgpuformat.c), not a limit a caller could need to change.
 *
 * Pass 4 (what a reviewer would refuse). 1. "Why two bits, not a query per
 * format, as Vulkan's vkGetPhysicalDeviceFormatProperties and D3D12's
 * CheckFeatureSupport(FORMAT_SUPPORT) have?" Because each API's feature is
 * per family, not per format: Vulkan's textureCompressionBC and
 * textureCompressionASTC_LDR each promise every format of the family
 * sampled, filtered and copied (the specification's required format
 * support for each feature), Metal's supportsBCTextureCompression and
 * family tables the same, D3D12 every BC format at 11_0. A per-format
 * query would say the same thing 46 times; the broader query (which
 * formats render, blend, filter, store) is a design of its own, carded in
 * S48's notes if a caller needs it. Changed: said under "What was
 * compared". 2. "A GPU encoder (a game compressing a texture it rendered,
 * card A53's liveries) writes blocks through an uncompressed view of the
 * compressed image (Vulkan's BLOCK_TEXEL_VIEW_COMPATIBLE, D3D12's format
 * casting)." Left out, with its reason, under "Left out": no caller
 * encodes on the GPU, and Metal has no such view of a compressed texture.
 *
 * Pass 5 (the platforms, measured). Measured on this Mac (Apple M5 Pro,
 * macOS 27, the MacOSX27.2 SDK, a scratch program in the system's
 * temporary directory, deleted after; Metal's API validation on): it
 * reports supportsBCTextureCompression YES and supportsFamily YES for
 * Apple2, Apple6, Apple7, Apple10 and the deprecated Mac2. Private
 * textures of BC7, ASTC 4x4 and ASTC 5x4 with full chains were made;
 * level 0 (2 x 2 blocks) and the last level (1 x 1 texel: a size that
 * stops at the level's edge, one block in the buffer) were copied in from
 * a buffer with bytes per row of blocks (32, 16) and back out, and came
 * back equal, with no validation message. A BC7 texture of 6 x 6 texels
 * was made too: Metal allows a size that isn't whole blocks, so Q5's rule
 * is a choice made for one answer on every backend, as Q5 says, not
 * Metal's. Changed: "What was compared" says so. Vulkan's and D3D12's
 * facts are from their headers on this machine; no Windows or Linux
 * machine was at hand.
 *
 * Pass 6 (tests and what they can see). 1. A copy in and back out in the
 * same shape can't see a row pitch wrong the same way both times: the two
 * errors cancel. Changed: each level is copied in whole and read back in
 * a different shape (a block row at a time into separate offsets, and a
 * sub-region at a block-aligned offset), with a padded buffer_row_length
 * one way; D3D12's 256-byte pitch from crgpu_image_get_buffer_copy_size()
 * one way and a tight pitch the other on Vulkan and Metal. What the GPU
 * samples stays Rend's test. 2. Added: the pipeline refusals of pass 1.
 *
 * Pass 7 (who else uses it, records, the card). 1. No "who else" section.
 * Changed: added (Rend; a game engine's cooked textures; CAD
 * viewports' textured materials later; Android, card S47, where ASTC is
 * the common format; crgpusamples). 2. The records the build updates
 * weren't named. Changed: "Records when built". 3. The card (S48) carries
 * Q1 to Q8, the Video line and these passes in a line.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * A game's textures ship block-compressed: BC7 on desktop GPUs, ASTC on
 * Apple's mobile GPUs (card L44). Carrottus GPU can't carry them today:
 *
 *  1. It has no ASTC format. CRGPU_FORMAT is numbered like DXGI_FORMAT,
 *     and DXGI has no ASTC.
 *  2. Its BC formats are declared and half reach the GPU. Vulkan maps them
 *     (crgpuvulkancontext.c) but never switches on textureCompressionBC
 *     when it creates the device, so a BC image is outside Vulkan's rules
 *     on every Vulkan device; Metal's table has no BC entry at all
 *     (crgpumetalformat.m: "The block-compressed formats ... have no
 *     caller in this workspace yet"), so the Mac refuses them; D3D12
 *     passes them through by its cast.
 *  3. The copies between a buffer and an image work in texels and bytes
 *     per texel (crgpu_format_get_bit_count() / 8), which is 0 for BC1
 *     and BC4 and wrong for every other block format: Metal's bytes per
 *     row, D3D12's footprint row pitch and crgpu_image_get_buffer_copy_size()
 *     on Vulkan and Metal all come out wrong for a block format. Nothing
 *     has ever copied one: crgpusamples' CrGPUDynamicIndexing decodes its
 *     BC1 texture on the CPU for that reason (its README).
 *  4. No adapter says whether it samples BC or ASTC: Vulkan's mobile
 *     drivers and Apple's iPhone GPUs lack BC, every desktop GPU but
 *     Apple's lacks ASTC, and D3D12 has no ASTC at all.
 *
 * This draft passes through what each API says (which formats exist, which
 * an adapter samples, how a block is laid out in a copy) and nothing else:
 * which format a texture is in, and what to do on an adapter without it,
 * stay the caller's.
 *
 * ---------------------------------------------------------------------------
 * What changes, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * CRGPU_FORMAT gains the 28 ASTC LDR formats (14 block sizes, UNORM and
 * sRGB), numbered from 1000, past anything DXGI numbers. Two feature bits
 * say whether an adapter samples BC and ASTC LDR; a device created
 * without one refuses an image of its formats, as it refuses every
 * feature it wasn't created with. One call, crgpu_format_get_texel_block(),
 * says a format's block (width and height in texels, bytes), which is
 * what a caller sizes block data by. A block-format image is a 2D image
 * (arrays and cubes included) used only to be sampled and copied, its
 * level 0 a whole number of blocks. The two copies between a buffer and
 * an image return a result and check their region against the block, and
 * every backend lays a block format's rows out as rows of blocks.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 *  - Metal (Apple's "Metal Feature Set Tables", the edition the
 *    max-image-size draft read, 2026-10-23; MTLPixelFormat.h and
 *    MTLDevice.h of the macOS 27 SDK on this machine): ASTC LDR in every
 *    Apple family from Apple2 (MTLPixelFormatASTC_4x4_LDR to _12x12_LDR,
 *    and _sRGB), ASTC HDR from Apple6 (_HDR); no ASTC on a Mac2 GPU (an
 *    Intel or AMD Mac). BC (MTLPixelFormatBC1_RGBA to BC7_RGBAUnorm_sRGB)
 *    where MTLDevice.supportsBCTextureCompression is true: every Apple
 *    Silicon Mac, iPads from the M1, no iPhone. An Apple GPU answers yes
 *    to MTLGPUFamilyMac2 as well (measured, pass 5), so "no ASTC" is a GPU
 *    in Mac2 and in no Apple family. Metal has no feature to switch on: a
 *    format is either sampled by the device or not, and any texture size
 *    is allowed (a 6 x 6 BC7 texture, measured). Blits
 *    between a buffer and a compressed texture take bytes per row of
 *    blocks and a size in texels that may stop at the level's edge.
 *  - Vulkan (vulkan_core.h, Vulkan SDK 1.4.363.0 on this machine):
 *    VK_FORMAT_ASTC_4x4_UNORM_BLOCK (157) to VK_FORMAT_ASTC_12x12_SRGB_BLOCK
 *    (184), sampled with VkPhysicalDeviceFeatures::textureCompressionASTC_LDR
 *    (line 3548), and VK_FORMAT_BC1_RGB_UNORM_BLOCK (131) to
 *    VK_FORMAT_BC7_SRGB_BLOCK (146) with ::textureCompressionBC (line
 *    3549); each optional and each to be enabled at device creation. ASTC
 *    HDR is VK_FORMAT_ASTC_*_SFLOAT_BLOCK (1000066000 and on) with
 *    VkPhysicalDeviceVulkan13Features::textureCompressionASTC_HDR. A copy
 *    of a block format (VkBufferImageCopy) has bufferRowLength and
 *    bufferImageHeight in texels, multiples of the block's extent, an
 *    image offset a multiple of the extent, and an extent a multiple of it
 *    or reaching the subresource's edge; bufferOffset a multiple of the
 *    block's bytes (and of 4 on a transfer-only queue). Any image size is
 *    allowed for a block format.
 *  - D3D12 (DirectX-Headers' dxgiformat.h and d3d12.h, the copy under
 *    .refs/HazelRenderer2025/Hazel/Hazel/vendor/nvrhi/thirdparty/):
 *    BC1 to BC7 required at every feature level Carrottus lists (12_0 and
 *    up); no ASTC format in DXGI and no ASTC in D3D12: DXGI_FORMAT stops
 *    at 132 and resumes at 189 (SAMPLER_FEEDBACK_*) and 191
 *    (A4B4G4R4_UNORM). A BC texture's level 0 is a whole number of blocks
 *    unless D3D12_FEATURE_DATA_D3D12_OPTIONS8::UnalignedBlockTexturesSupported
 *    (d3d12.h line 2848). A copy's placed footprint has its RowPitch a
 *    multiple of 256 (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) and its Offset
 *    of 512, its Width and Height whole blocks.
 *  - WebGPU (the W3C specification): features "texture-compression-bc"
 *    and "texture-compression-astc" (LDR only; 14 sizes, unorm and
 *    unorm-srgb), "-sliced-3d" variants for 3D textures apart; a
 *    compressed texture's level 0 is a whole number of blocks; a copy's
 *    bytesPerRow counts bytes of a row of blocks and rowsPerImage rows of
 *    blocks, its origin and size whole blocks of the level's physical
 *    size. Its usages for a compressed format: TEXTURE_BINDING and the
 *    copies, never RENDER_ATTACHMENT or STORAGE_BINDING.
 *  - Industry practice: wgpu (TextureFormat::Astc { block, channel },
 *    Features::TEXTURE_COMPRESSION_ASTC and _BC, block_dimensions() and
 *    block_copy_size() per format); SDL_GPU (SDL_GPUTextureFormat's
 *    ASTC_4x4_UNORM to _12x12 and _UNORM_SRGB and _FLOAT,
 *    SDL_GPUTextureFormatTexelBlockSize(), SDL_GPUTextureSupportsFormat());
 *    NVRHI, under Hazel (nvrhi::FormatInfo's bytesPerBlock and a square
 *    blockSize; BC only, no ASTC, so a 5x4 block can't be said in it);
 *    bgfx (bimg's ImageBlockInfo: blockWidth, blockHeight, blockSize);
 *    Vulkan's own "texel block" (size and extent per format, the
 *    specification's compatible-formats table), whose words this draft
 *    takes.
 *  - Why two bits and not a query per format (pass 4): each API's
 *    capability is per family. Vulkan's textureCompressionBC and
 *    textureCompressionASTC_LDR each promise every format of their family
 *    sampled, linearly filtered and copied (the specification's required
 *    format support for each feature); Metal's supportsBCTextureCompression
 *    and its family tables the same; D3D12 every BC format at 11_0. A
 *    general format-capabilities query (which formats a device renders to,
 *    blends, filters, stores) is a design of its own, carded when a caller
 *    needs it.
 *
 * Hazel is not compared: Carrottus is not measured against an engine
 * (the workspace's CLAUDE.md, "External references").
 *
 * ---------------------------------------------------------------------------
 * What the caller decides, and what Carrottus GPU only reports
 * ---------------------------------------------------------------------------
 *
 * Reported: whether an adapter samples BC and ASTC LDR (two feature bits);
 * a format's block (crgpu_format_get_texel_block()); a level's copy layout
 * (crgpu_image_get_buffer_copy_size(), as today). Decided by the caller:
 * which features its device has, which format each image is, where its
 * data comes from, and what to do on an adapter that lacks a format (load
 * another pack, decode on the CPU, refuse). Carrottus GPU never picks a
 * format, never decodes a block, never pads an image, and never falls back
 * from one format to another.
 *
 * ---------------------------------------------------------------------------
 * Who else uses it
 * ---------------------------------------------------------------------------
 *
 *  - Rend (Rabbittus' block-formats draft, card A54): uploads block-format
 *    textures with their levels, reading the two bits from the device and
 *    the block from crgpu_format_get_texel_block().
 *  - A game engine's cooked textures: BC7 packs on Windows and Linux, ASTC
 *    4x4 on iPad, iPhone and the Mac; the first caller (card S48).
 *  - CAD viewports, when a material takes an image
 *    (a site photo draped on terrain, a material's texture): the same
 *    formats, chosen by them.
 *  - Carrottus on Android (card S47), whose GPUs take ASTC and mostly not
 *    BC: the bits are how a caller there knows.
 *  - crgpusamples: CrGPUDynamicIndexing's BC1 texture, uploaded as
 *    Microsoft's sample does.
 *
 * ---------------------------------------------------------------------------
 * Left out, and why
 * ---------------------------------------------------------------------------
 *
 *  - ASTC HDR (Q2) and 3D textures of block formats (Q7): appended when a
 *    caller has one.
 *  - Writing blocks through an uncompressed view of a compressed image
 *    (Vulkan's VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT, D3D12's
 *    format casting), what a GPU encoder needs (pass 4): no caller encodes
 *    on the GPU, and Metal has no view of a compressed texture in an
 *    uncompressed format. A draft of its own when one does.
 *  - ETC2 and EAC (Vulkan's textureCompressionETC2; Metal's Apple GPUs;
 *    WebGPU's "texture-compression-etc2"): no caller ships them (card L44
 *    chose BC7 and ASTC); appended with a bit of their own when one does.
 *  - ASTC's decode mode (VK_EXT_astc_decode_mode): an optimisation one API
 *    has.
 *
 * ---------------------------------------------------------------------------
 * The version and the breaks
 * ---------------------------------------------------------------------------
 *
 *  - CRGPU_FORMAT: 28 values appended (from 1000). No existing value
 *    changes. Append-only, as every enum here.
 *  - CRGPU_FEATURES: two bits appended (16, 17). No bit's meaning changes.
 *  - crgpu_format_get_texel_block(): added.
 *  - crgpu_command_list_copy_buffer_to_image() and
 *    crgpu_command_list_copy_image_to_buffer() return CR_RESULT where they
 *    returned void (Q6): a source-compatible change in C (a call whose
 *    result is ignored still compiles), a listed ABI break in abi.txt.
 *  - crgpu_image_create() and crgpu_image_create_placed() refuse what they
 *    passed before: a BC image on a device created without
 *    CRGPU_FEATURE_TEXTURE_COMPRESSION_BC (on D3D12, where BC worked, and on
 *    Vulkan, where it was outside the rules), a block-format image with an
 *    attachment or storage usage, of a 1D or 3D type, or whose level 0
 *    isn't whole blocks. A behaviour break, listed. No caller in the
 *    workspace makes a BC image today (searched: Rabbittus names BC formats
 *    only to say they have no shader scalar type; the callers above it
 *    name none).
 *  - A block format as a pipeline's attachment or vertex attribute format
 *    is refused (CR_RESULT_GPU_INVALID_FORMAT) where it was passed to the
 *    API (pass 1). No caller does it.
 *  - A caller that range-tests CRGPU_FORMAT meets values from 1000: Rend's
 *    format-to-scalar-type function takes everything from
 *    CRGPU_FORMAT_AYUV up for a video format, and ASTC would fall into it
 *    (pass 2); fixed in Rend's draft, not here.
 *  - crgpu_image_get_buffer_copy_size(): its answers for a block format
 *    become right (rows of blocks); no caller depended on the wrong ones.
 *  - CARROTTUS_VERSION_MINOR goes up by one when it is built, unless the
 *    building session finds it already raised and untagged since (card
 *    X2). ./check.sh --update-abi lists the breaks; the misuse generator
 *    is rerun for the new call and the two changed ones.
 *
 * New handles: none. New struct types: none. Stored formats touched: none
 * (CRGPU_FORMAT's numbers are written into no file Carrottus owns; a
 * caller that stores them (the first does not) inherits their
 * append-only rule).
 *
 * Thread: crgpu_format_get_texel_block() reads nothing but its argument
 * and may be called from any thread at any time. The copies keep the
 * command list's rule. Allocation: none of the new or changed calls
 * allocates.
 *
 * Video: block-compressed formats are not video formats, and nothing here
 * touches the planar YUV values (NV12 to V408), the queues or the colour
 * space draft (crgpu_room_before_1_0_draft.h). Numbering formats DXGI
 * lacks from 1000 (Q3) leaves the room a later YUV format Vulkan has and
 * DXGI lacks (a 10-bit 4:2:2 planar, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_*)
 * would need: video stays open after 1.0.
 *
 * ---------------------------------------------------------------------------
 * Questions (numbered; each with options and a recommendation)
 * ---------------------------------------------------------------------------
 *
 * Q1. WHICH ASTC FORMATS.
 *     (a) RECOMMENDED. All 14 2D block sizes of ASTC LDR (4x4, 5x4, 5x5,
 *         6x5, 6x6, 8x5, 8x6, 8x8, 10x5, 10x6, 10x8, 10x10, 12x10, 12x12),
 *         each UNORM and sRGB: 28 values. Each is one format of every API
 *         that has ASTC (Metal, Vulkan, WebGPU), a row in each backend's
 *         table and nothing more; the block size is the caller's trade
 *         between quality and memory (a 6x6 terrain texture is 0.44 of a
 *         4x4's), and the non-square ones (5x4, 10x5) are what a test of
 *         a block's width against its height needs.
 *     (b) 4x4 alone, UNORM and sRGB: card L44's need and no more. Each
 *         later size an append, no break; but a caller's other sizes wait
 *         on a draft each, and the copy rules go untested on a block whose
 *         width isn't its height.
 *
 * Q2. ASTC HDR.
 *     (a) RECOMMENDED. Not now. No caller has an HDR texture to ship
 *         (Rend's environments are made on the GPU; card L44 is LDR), and
 *         D3D12, WebGPU and every desktop GPU but Apple's have no ASTC HDR.
 *         Appended later with its own bit
 *         (CRGPU_FEATURE_TEXTURE_COMPRESSION_ASTC_HDR: Vulkan's
 *         textureCompressionASTC_HDR, Metal's Apple6 family) and its 14
 *         SFLOAT values: no break then.
 *     (b) Now, 14 more values and a third bit, for a caller that has none.
 *
 * Q3. THE NUMBERS.
 *     (a) RECOMMENDED. A block of Carrottus's own for formats DXGI doesn't
 *         number, from 1000: ASTC_4X4_UNORM 1000, ASTC_4X4_UNORM_SRGB
 *         1001, and so on in Vulkan's order of sizes, to
 *         ASTC_12X12_UNORM_SRGB 1027. The D3D12 backend's straight cast
 *         (crgpu_d3d12_get_dxgi_format) never sees one: the feature check
 *         refuses them first (Q4), and the cast gains a guard for values
 *         at or past 1000 all the same, so no path hands DXGI a number it
 *         may one day mean something else by.
 *     (b) DXGI's unused 133 to 188, which fits 28 values: but DXGI may
 *         number those one day, and a straight cast would then give D3D12
 *         a format the caller never named.
 *
 * Q4. AN EXT MACRO FOR ASTC?
 *     (a) RECOMMENDED. No. The formats are values, always declared; the
 *         bits are always defined (crgpu.h's rule for features); D3D12
 *         never sets CRGPU_FEATURE_TEXTURE_COMPRESSION_ASTC_LDR, so every
 *         ASTC image is refused there by the same feature check as on an
 *         adapter that lacks it, with CR_RESULT_GPU_FEATURE_NOT_SUPPORTED,
 *         as CRGPU_FEATURE_PRESENT's calls refuse. An EXT macro wraps a
 *         group of functions and types so calling what the build lacks is
 *         a compile error (carrottus/CLAUDE.md); here there is no function
 *         or type to wrap, only values a caller's backend-agnostic code
 *         (Rend's tables) names on every backend.
 *     (b) CRGPU_EXT_TEXTURE_COMPRESSION_ASTC, 0 on D3D12, with its paired
 *         CR_RESULT_GPU_EXT_..._NOT_SUPPORTED: a macro that gates nothing
 *         at compile time and a second result for what the feature's
 *         refusal already says.
 *
 * Q5. AN IMAGE'S SIZE IN BLOCKS.
 *     (a) RECOMMENDED. A block-format image's level-0 width and height are
 *         each a whole number of its blocks, on every backend
 *         (CR_RESULT_DESC_INVALID_PARAMETERS otherwise). WebGPU's rule, and
 *         D3D12's for BC without OPTIONS8; Vulkan and Metal allow any size.
 *         The same image gets the same answer everywhere, as the
 *         workspace's checks are written. Smaller levels are whatever the
 *         halving gives (a 4x4 BC image's level 2 is 1x1 texel, one block
 *         in memory), on every API.
 *     (b) Each API's own: any size on Vulkan and Metal, D3D12 asked
 *         through a third bit (CRGPU_FEATURE_UNALIGNED_BLOCK_IMAGES from
 *         OPTIONS8). An image that works on the Mac is refused on a
 *         Windows machine without the bit: the difference reaches the
 *         person, not the developer.
 *
 * Q6. THE COPIES REFUSE.
 *     (a) RECOMMENDED. crgpu_command_list_copy_buffer_to_image() and
 *         crgpu_command_list_copy_image_to_buffer() return CR_RESULT,
 *         check their arguments (handles, the region against the image's
 *         level and layers, the block's alignment of offset, extent and
 *         row length, the buffer offset's alignment) and record nothing on
 *         a refusal. A misaligned block copy is undefined on Vulkan, a
 *         device removal on D3D12 and an abort under Metal's validation:
 *         bad input reaching the driver (CONVENTIONS.md, "Data handling").
 *         The acceleration structures' build and copy return a result the
 *         same way.
 *     (b) Keep them void and write the rules as the caller's, undefined
 *         when broken, as an indirect draw's arguments are. The arguments
 *         here are the CPU's, which a check can see; an indirect draw's
 *         are the GPU's, which none can.
 *
 * Q7. WHICH IMAGE TYPES.
 *     (a) RECOMMENDED. 2D only: arrays and cube-capable images included,
 *         1D and 3D refused (CR_RESULT_GPU_INVALID_IMAGE_TYPE). A 1D BC
 *         texture doesn't exist on D3D12; a 3D one made of 2D blocks needs
 *         a feature of its own on WebGPU ("-sliced-3d") and Vulkan's
 *         format properties to allow it. A later bit appends it when a
 *         caller has a block-compressed volume.
 *     (b) 3D too, refused only where the API refuses: a create that works
 *         on one backend and fails on another, found by the person.
 *
 * Q8. crgpu_format_get_bit_count() FOR ASTC.
 *     (a) RECOMMENDED. 0 for every ASTC format, as for any format with no
 *         whole number of bits to a texel: a 5x4 block is 6.4 bits a
 *         texel. BC formats keep their averages (4 and 8, DirectXTex's
 *         BitsPerPixel), which existing callers read. The block is
 *         crgpu_format_get_texel_block()'s to say.
 *     (b) The average where it is whole (4x4: 8, 8x8: 2, 12x12: 0), 0
 *         where it isn't: a value some sizes have and others not.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 *  - crgpu_device_test, on every adapter of each backend: the two bits as
 *    each API reports them (Metal: BC exactly when
 *    supportsBCTextureCompression, ASTC LDR exactly when the device is in
 *    MTLGPUFamilyApple2; D3D12: BC on every adapter, ASTC on none; Vulkan:
 *    VkPhysicalDeviceFeatures' two booleans); a device created with each
 *    bit the adapter has reports it, one created without doesn't.
 *  - crgpu_objects_test, on every adapter, for each block format the
 *    adapter samples (every BC format but TYPELESS ones, and the 28 ASTC):
 *    a 2D image with a full chain, a 2D array of 3 layers and a cube, of
 *    SAMPLED | TRANSFER_SRC | TRANSFER_DST; every level of every layer
 *    written from a buffer of known bytes (block i's bytes a function of
 *    its level, layer and index), the smallest levels (2x2, 1x1 texels) as
 *    one block each, read back into a readback buffer and compared byte
 *    for byte: a copy moves blocks unchanged, so equality is exact. The
 *    region rules each refused once (offset not a multiple of the block,
 *    extent neither a multiple nor reaching the edge, row length not a
 *    multiple of the block's width, buffer offset misaligned, a level or
 *    layer past the image) with nothing recorded; a region that stops at
 *    the level's edge accepted. crgpu_format_get_texel_block() for every
 *    CRGPU_FORMAT value: the block of each BC and ASTC format, 1 x 1 and
 *    the texel's bytes for an uncompressed one, CR_RESULT_GPU_INVALID_FORMAT
 *    for UNKNOWN, R1_UNORM and the planar video formats.
 *  - Layout, not only round trips (pass 6): each level is copied in whole
 *    and read back in another shape (a row of blocks at a time into
 *    separate offsets, and a block-aligned sub-region), with a padded
 *    buffer_row_length one way, so a row pitch wrong the same way in both
 *    directions can't pass; on D3D12 the 256-byte pitch of
 *    crgpu_image_get_buffer_copy_size() one way and Vulkan's and Metal's
 *    tight pitch the other.
 *  - Pipelines (pass 1): a graphics pipeline with a BC or ASTC colour
 *    attachment format, and a vertex attribute of one, refused with
 *    CR_RESULT_GPU_INVALID_FORMAT on every backend.
 *  - Refusals at creation, on every adapter: a BC and an ASTC image on a
 *    device created without each bit (every adapter can make one: the bits
 *    are asked for, not given) with CR_RESULT_GPU_FEATURE_NOT_SUPPORTED;
 *    COLOR_ATTACHMENT, DEPTH_STENCIL_ATTACHMENT and STORAGE usages; 1D and
 *    3D types; a level 0 of 6 x 8 in BC7 and of 4 x 4 in ASTC 5x5. Each
 *    before the API sees it: on D3D12 no ASTC value reaches the cast.
 *  - cr_noalloc_tests: a block copy recorded, and the format query,
 *    allocate nothing.
 *  - The misuse harness: regenerated for crgpu_format_get_texel_block()
 *    and the copies' results.
 *  - crgpusamples' CrGPUDynamicIndexing uploads its BC1 texture as
 *    Microsoft's sample does and its CPU decoder goes, on D3D12 and Vulkan
 *    (it doesn't build on Metal, which has no bindless).
 *  - Sampling a block (that the GPU decodes what was copied) is Rend's
 *    test (Rabbittus' block-formats draft, card A54), through rabbittustests,
 *    which draws.
 *  - Where each runs: the Mac (Apple M5 Pro: BC and ASTC; plain, -asan,
 *    -tsan; Metal's API validation on); Windows' Iris Xe, RTX A2000 and
 *    WARP over D3D12 (BC; every ASTC create refused), the Iris Xe and the
 *    A2000 over Vulkan (BC; neither has ASTC LDR); Fedora over Vulkan (ANV,
 *    NVK, RADV; each driver's two booleans recorded in
 *    CRGPU_DEVICE_TODO.md). No GPU at hand samples ASTC through Vulkan,
 *    so Vulkan's ASTC path is written against the specification and run
 *    only for its refusals until one is (said in the TODO). The iOS
 *    simulator lists no adapter.
 *
 * ---------------------------------------------------------------------------
 * crgpu.h
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Pixel and vertex-attribute data format.
 *
 * [CHANGED] Numbered like DXGI_FORMAT, where DXGI has the format; a
 * format DXGI lacks is numbered from 1000 (ASTC, Q3), past any value DXGI
 * uses. Only appended to.
 */
typedef uint32_t CRGPU_FORMAT;
enum {
	/* ... every value as before, CRGPU_FORMAT_UNKNOWN (0) to
	 * CRGPU_FORMAT_V408 (132) ... */

	/*
	 * [ADDED] ASTC LDR (Q1 a): every 2D block size, each UNORM and sRGB, in
	 * Vulkan's order. A block is 16 bytes whatever its size. Sampled on a
	 * device created with CRGPU_FEATURE_TEXTURE_COMPRESSION_ASTC_LDR; never
	 * on D3D12. Metal: MTLPixelFormatASTC_<w>x<h>_LDR and _sRGB. Vulkan:
	 * VK_FORMAT_ASTC_<w>x<h>_UNORM_BLOCK and _SRGB_BLOCK.
	 *
	 * Why: card L44's ASTC 4x4 packs for iPad and iPhone, and any caller's
	 * ASTC texture. ABI: values appended. Tests: crgpu_objects_test.
	 */
	CRGPU_FORMAT_ASTC_4X4_UNORM        = 1000, /**< ASTC LDR, 4x4 texels a block, linear. */
	CRGPU_FORMAT_ASTC_4X4_UNORM_SRGB   = 1001, /**< ASTC LDR, 4x4, sRGB-encoded colour. */
	CRGPU_FORMAT_ASTC_5X4_UNORM        = 1002, /**< ASTC LDR, 5x4 (5 wide, 4 tall), linear. */
	CRGPU_FORMAT_ASTC_5X4_UNORM_SRGB   = 1003, /**< ASTC LDR, 5x4, sRGB. */
	CRGPU_FORMAT_ASTC_5X5_UNORM        = 1004, /**< ASTC LDR, 5x5, linear. */
	CRGPU_FORMAT_ASTC_5X5_UNORM_SRGB   = 1005, /**< ASTC LDR, 5x5, sRGB. */
	CRGPU_FORMAT_ASTC_6X5_UNORM        = 1006, /**< ASTC LDR, 6x5, linear. */
	CRGPU_FORMAT_ASTC_6X5_UNORM_SRGB   = 1007, /**< ASTC LDR, 6x5, sRGB. */
	CRGPU_FORMAT_ASTC_6X6_UNORM        = 1008, /**< ASTC LDR, 6x6, linear. */
	CRGPU_FORMAT_ASTC_6X6_UNORM_SRGB   = 1009, /**< ASTC LDR, 6x6, sRGB. */
	CRGPU_FORMAT_ASTC_8X5_UNORM        = 1010, /**< ASTC LDR, 8x5, linear. */
	CRGPU_FORMAT_ASTC_8X5_UNORM_SRGB   = 1011, /**< ASTC LDR, 8x5, sRGB. */
	CRGPU_FORMAT_ASTC_8X6_UNORM        = 1012, /**< ASTC LDR, 8x6, linear. */
	CRGPU_FORMAT_ASTC_8X6_UNORM_SRGB   = 1013, /**< ASTC LDR, 8x6, sRGB. */
	CRGPU_FORMAT_ASTC_8X8_UNORM        = 1014, /**< ASTC LDR, 8x8, linear. */
	CRGPU_FORMAT_ASTC_8X8_UNORM_SRGB   = 1015, /**< ASTC LDR, 8x8, sRGB. */
	CRGPU_FORMAT_ASTC_10X5_UNORM       = 1016, /**< ASTC LDR, 10x5, linear. */
	CRGPU_FORMAT_ASTC_10X5_UNORM_SRGB  = 1017, /**< ASTC LDR, 10x5, sRGB. */
	CRGPU_FORMAT_ASTC_10X6_UNORM       = 1018, /**< ASTC LDR, 10x6, linear. */
	CRGPU_FORMAT_ASTC_10X6_UNORM_SRGB  = 1019, /**< ASTC LDR, 10x6, sRGB. */
	CRGPU_FORMAT_ASTC_10X8_UNORM       = 1020, /**< ASTC LDR, 10x8, linear. */
	CRGPU_FORMAT_ASTC_10X8_UNORM_SRGB  = 1021, /**< ASTC LDR, 10x8, sRGB. */
	CRGPU_FORMAT_ASTC_10X10_UNORM      = 1022, /**< ASTC LDR, 10x10, linear. */
	CRGPU_FORMAT_ASTC_10X10_UNORM_SRGB = 1023, /**< ASTC LDR, 10x10, sRGB. */
	CRGPU_FORMAT_ASTC_12X10_UNORM      = 1024, /**< ASTC LDR, 12x10, linear. */
	CRGPU_FORMAT_ASTC_12X10_UNORM_SRGB = 1025, /**< ASTC LDR, 12x10, sRGB. */
	CRGPU_FORMAT_ASTC_12X12_UNORM      = 1026, /**< ASTC LDR, 12x12, linear. */
	CRGPU_FORMAT_ASTC_12X12_UNORM_SRGB = 1027, /**< ASTC LDR, 12x12, sRGB. */

	CRGPU_FORMAT_FORCE_UINT = -1 /**< 0xffffffff */
};

/**
 * @brief Returns the size of one pixel of a format, in bits.
 *
 * [CHANGED] Its comment only (Q8 a): a block-compressed BC format gives
 * its average over the block's 16 texels (4 or 8), as before; an ASTC
 * format gives 0, having no whole number of bits to a texel in general.
 * crgpu_format_get_texel_block() describes a block.
 *
 * @param[in] fmt Format to query.
 *
 * @return Bits per pixel, or 0 if `fmt` is not a format with a known whole
 *         pixel size.
 */
uint32_t crgpu_format_get_bit_count(CRGPU_FORMAT fmt);

/**
 * @brief [ADDED] Says how a format's data is laid out in blocks of texels:
 * the smallest unit an image of it is copied in.
 *
 * A block-compressed format's block is its compression block (BC: 4 x 4
 * texels in 8 or 16 bytes; ASTC: its size, in 16 bytes). An uncompressed
 * format's block is one texel, of its bytes. A row of an image's level in
 * a copy is a row of blocks: ceil(width / block width) blocks of `bytes`
 * each; a level has ceil(height / block height) such rows. Vulkan's "texel
 * block": its size and extent.
 *
 * Checks `format` first, then each output in order; nothing is written on
 * a refusal (pass 3).
 *
 * Why: a caller sizes block data (a staging buffer, a file's level) and
 * aligns a copy's region by the block, which crgpu_format_get_bit_count()
 * can't say (an ASTC 5x4 block is 6.4 bits a texel). Every API above
 * Carrottus that has these formats has the query: wgpu's
 * block_dimensions(), SDL_GPU's SDL_GPUTextureFormatTexelBlockSize(),
 * NVRHI's FormatInfo. ABI: a call added. Tests: crgpu_objects_test, for
 * every value.
 *
 * Thread: any, at any time; it reads nothing but `format`.
 *
 * @param[in]  format     Format to describe.
 * @param[out] out_width  Receives the block's width in texels: 1 for an
 *                        uncompressed format, 4 for BC, the size's first
 *                        number for ASTC.
 * @param[out] out_height Receives the block's height in texels.
 * @param[out] out_bytes  Receives a block's bytes: the texel's for an
 *                        uncompressed format, 8 (BC1, BC4) or 16 (the
 *                        other BC formats, every ASTC format).
 *
 * @retval CR_SUCCESS The three outputs were written.
 * @retval CR_RESULT_GPU_INVALID_FORMAT `format` is CRGPU_FORMAT_UNKNOWN, a
 *         value CRGPU_FORMAT doesn't name, or a format with no block of
 *         whole bytes (CRGPU_FORMAT_R1_UNORM, the planar and packed video
 *         formats from CRGPU_FORMAT_AYUV to CRGPU_FORMAT_V408 and
 *         CRGPU_FORMAT_R8G8_B8G8_UNORM / _G8R8_G8B8_UNORM, whose layouts a
 *         block of texels can't describe). Nothing is written.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER An output is NULL. Nothing is
 *         written.
 */
CR_RESULT crgpu_format_get_texel_block(
    CRGPU_FORMAT format,
    uint32_t *out_width,
    uint32_t *out_height,
    uint32_t *out_bytes);

/*
 * [CHANGED] crgpu_format_from_string(): the 28 ASTC names, as their
 * values are named without the prefix ("ASTC_4X4_UNORM",
 * "ASTC_4X4_UNORM_SRGB", ...). Its contract is unchanged.
 */

/**
 * [ADDED] The adapter samples the BC formats (CRGPU_FORMAT_BC1_* to
 * CRGPU_FORMAT_BC7_*): an image of one, on a device created without it, is
 * refused with CR_RESULT_GPU_FEATURE_NOT_SUPPORTED. Vulkan:
 * `VkPhysicalDeviceFeatures::textureCompressionBC`, enabled with it. D3D12:
 * every adapter (BC is required at feature level 11_0 and up). Metal:
 * `MTLDevice.supportsBCTextureCompression` (every Apple Silicon Mac, iPads
 * from the M1; no iPhone).
 *
 * Why: Vulkan's mobile drivers and Apple's iPhone GPUs lack BC, so a
 * caller choosing BC must know before it creates an image; Vulkan needs the
 * feature enabled for a BC image to be within its rules at all. ABI: a bit
 * appended. Tests: crgpu_device_test, crgpu_objects_test.
 */
#define CRGPU_FEATURE_TEXTURE_COMPRESSION_BC (UINT64_C(1) << 16)
/**
 * [ADDED] The adapter samples the ASTC LDR formats (CRGPU_FORMAT_ASTC_*):
 * an image of one, on a device created without it, is refused with
 * CR_RESULT_GPU_FEATURE_NOT_SUPPORTED. Vulkan:
 * `VkPhysicalDeviceFeatures::textureCompressionASTC_LDR`, enabled with it.
 * D3D12: never (D3D12 has no ASTC). Metal: an adapter in
 * `MTLGPUFamilyApple2` or later (every Apple GPU; never a GPU in
 * MTLGPUFamilyMac2 alone, an Intel or AMD Mac's).
 *
 * Why: card L44's packs for Apple's mobile GPUs; desktop GPUs other than
 * Apple's lack ASTC. "_LDR" because ASTC HDR is a separate capability on
 * every API that has it (Q2). ABI: a bit appended. Tests: as above.
 */
#define CRGPU_FEATURE_TEXTURE_COMPRESSION_ASTC_LDR (UINT64_C(1) << 17)

/*
 * [CHANGED] crgpu_image_create()'s and crgpu_image_create_placed()'s
 * result lists (and so crgpu_heap_get_image_placement_requirements()'s,
 * which lists each result of crgpu_image_create()'s desc checks) gain,
 * after the format check (CR_RESULT_GPU_INVALID_FORMAT for UNKNOWN) and
 * before the size limits (CR_RESULT_GPU_IMAGE_TOO_LARGE), for a desc whose
 * `format` is a BC or ASTC format, in this order:
 *
 *   @retval CR_RESULT_GPU_FEATURE_NOT_SUPPORTED `desc->format` is a BC
 *           format and the device wasn't created with
 *           CRGPU_FEATURE_TEXTURE_COMPRESSION_BC, or an ASTC format and it
 *           wasn't created with CRGPU_FEATURE_TEXTURE_COMPRESSION_ASTC_LDR
 *           (on D3D12, every ASTC format). Since 0.5.0.
 *   @retval CR_RESULT_GPU_INVALID_IMAGE_USAGE `desc->usage` has anything
 *           but CRGPU_IMAGE_USAGE_SAMPLED, _TRANSFER_SRC and _TRANSFER_DST
 *           for a block-compressed format: a block format is never
 *           rendered to or written by a shader, on any API. Since 0.5.0.
 *   @retval CR_RESULT_GPU_INVALID_IMAGE_TYPE `desc->type` isn't
 *           CRGPU_IMAGE_TYPE_2D for a block-compressed format (Q7 a).
 *           Since 0.5.0.
 *   @retval CR_RESULT_DESC_INVALID_PARAMETERS `desc->width` or
 *           `desc->height` isn't a whole number of the format's blocks
 *           (Q5 a). Since 0.5.0.
 *
 * Checked once, in src/gpu/crgpushared.c, called by every backend's two
 * creates at the same place, so the same desc gets the same answer
 * everywhere and no ASTC value reaches D3D12's cast. A BC TYPELESS
 * format is still refused by Metal, which has no typeless texture, as
 * every TYPELESS format is there (crgpumetalformat.m), with
 * CR_RESULT_GPU_INVALID_FORMAT after these checks.
 *
 * Why: the first so a device is held to the features it was created with
 * (crgpu.h's rule), and so D3D12 never receives a value DXGI lacks; the
 * others because each API refuses them, in different ways (Vulkan's
 * validation, D3D12's E_INVALIDARG, Metal's abort under its validation).
 * ABI: refusals of what was accepted (above). Tests: crgpu_objects_test.
 */

/* ============================================================================
 *  Image <-> Buffer Copies
 * ========================================================================= */

/**
 * @brief Describes a rectangular texel region and target mip/layer used for a
 * copy between an image and a buffer.
 *
 * [CHANGED] Its comments only; the layout is the same (it is frozen). For
 * a block-compressed format (crgpu_format_get_texel_block() gives a block
 * larger than one texel) every texel count below is still in texels, as
 * Vulkan's VkBufferImageCopy has them, and:
 *  - #image_x and #image_y are multiples of the block's width and height;
 *  - #width is a multiple of the block's width, or reaches the level's
 *    right edge (#image_x + #width is the level's width); #height the same
 *    with the block's height and the bottom edge. So the last level of a
 *    chain, 1 x 1 texels, is copied as its one block with a #width and
 *    #height of 1;
 *  - #buffer_row_length, when not 0, is a multiple of the block's width,
 *    and #buffer_image_height, when not 0, of its height;
 *  - in the buffer, a row is a row of blocks, ceil(width / block width)
 *    blocks of the block's bytes, and a layer is
 *    ceil(height / block height) such rows, from #buffer_offset; with
 *    #buffer_row_length and #buffer_image_height 0 they are packed tight
 *    at those counts.
 * #depth is 1 and #image_z 0 for a block-compressed format, whose images
 * are 2D.
 */
/* Frozen: a copy region, passed in arrays. It never changes; a new need gets a new type. */
typedef struct crgpu_image_buffer_copy_desc {
	/** Byte offset into the buffer where this region starts (or is read
	 * from): a multiple of crgpu_image_get_buffer_copy_offset_alignment(). */
	uint64_t buffer_offset;
	/** Row length in texels, used to interpret the buffer's layout. Pass 0
	 * for tightly packed rows (== region width, rounded up to whole blocks
	 * for a block format). */
	uint32_t buffer_row_length;
	/** Image height in texels, used to interpret the buffer's 2D/3D layout.
	 * Pass 0 for tightly packed rows (== region height, rounded up to whole
	 * blocks for a block format). */
	uint32_t buffer_image_height;
	/** Mip level of the image being copied. */
	uint32_t mip_level;
	/** Starting array layer of the image being copied. */
	uint32_t base_array_layer;
	/** Number of array layers to copy. */
	uint32_t array_layer_count;
	/** Texel-space offset into the image where the region begins: x. */
	uint32_t image_x, /** y. */ image_y, /** z. */ image_z;
	/** Texel-space size of the copied region: width. */
	uint32_t width, /** Height. */ height, /** Depth. */ depth;
} crgpu_image_buffer_copy_desc;

/**
 * @brief Records a copy from an image subresource region into a buffer.
 *
 * [CHANGED] Returns a result (Q6 a), and checks its arguments before it
 * records anything, in parameter order; a refused call records nothing.
 * A destroyed handle is refused as a null one. `desc` is read during the
 * call only. The image must be in
 * CRGPU_IMAGE_LAYOUT_TRANSFER_SRC, as before (not checked: the layout is
 * the caller's to track). Use crgpu_image_get_buffer_copy_size() to size
 * the destination buffer first.
 *
 * Maps to: vkCmdCopyImageToBuffer (Vulkan), CopyTextureRegion (D3D12),
 * copyFromTexture:toBuffer: (Metal).
 *
 * @param[in] cmd        Command list being recorded.
 * @param[in] src_image  Image to read texel data from.
 * @param[in] dst_buffer Buffer that receives the copied bytes.
 * @param[in] desc       Region, offset and subresource to copy.
 *
 * @retval CR_SUCCESS The copy was recorded.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST `cmd` is NULL.
 * @retval CR_RESULT_GPU_INVALID_IMAGE `src_image` is NULL.
 * @retval CR_RESULT_GPU_INVALID_BUFFER `dst_buffer` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_GPU_INVALID_FORMAT The image's format has no texel
 *         block (crgpu_format_get_texel_block()), so no copy of it has a
 *         known layout.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS The region isn't within the
 *         image: `mip_level` past its levels, the layers past its layers
 *         or `array_layer_count` 0, a size of 0, or the region past the
 *         level's width, height or depth; or it breaks the block's rules
 *         (crgpu_image_buffer_copy_desc); or `buffer_offset` isn't a
 *         multiple of the copy offset alignment. Checked in that order.
 */
CR_RESULT crgpu_command_list_copy_image_to_buffer(
    crgpu_command_list cmd,
    crgpu_image src_image,
    crgpu_buffer dst_buffer,
    const crgpu_image_buffer_copy_desc *desc);

/**
 * @brief Records a copy of buffer bytes into an image subresource region.
 * Used for texture uploads.
 *
 * [CHANGED] As crgpu_command_list_copy_image_to_buffer(): returns a result,
 * checks first, records nothing on a refusal. The image must be in
 * CRGPU_IMAGE_LAYOUT_TRANSFER_DST. Use crgpu_image_get_buffer_copy_size()
 * for the expected source row pitch and total size.
 *
 * Maps to: vkCmdCopyBufferToImage (Vulkan), CopyTextureRegion (D3D12),
 * copyFromBuffer:toTexture: (Metal).
 *
 * @param[in] cmd        Command list being recorded.
 * @param[in] src_buffer Buffer to read bytes from.
 * @param[in] dst_image  Image to write texel data into.
 * @param[in] desc       Region, offset and subresource to copy.
 *
 * @return As crgpu_command_list_copy_image_to_buffer(), with
 *         CR_RESULT_GPU_INVALID_BUFFER for `src_buffer` and
 *         CR_RESULT_GPU_INVALID_IMAGE for `dst_image`.
 */
CR_RESULT crgpu_command_list_copy_buffer_to_image(
    crgpu_command_list cmd,
    crgpu_buffer src_buffer,
    crgpu_image dst_image,
    const crgpu_image_buffer_copy_desc *desc);

/**
 * @brief Computes the buffer row pitch and total size needed to receive (or
 * supply) a copy of an image mip level.
 *
 * [CHANGED] Its comment, and its answers for a block format, which were
 * wrong: a row is a row of blocks (crgpu_format_get_texel_block()), so the
 * row pitch is at least ceil(level width / block width) times the block's
 * bytes, and the total covers ceil(level height / block height) rows a
 * layer of depth 1. D3D12's row pitch is that rounded up to 256 bytes
 * (GetCopyableFootprints, as today); Vulkan's and Metal's is that, tight.
 * The row length a copy then takes, in texels, is
 * `row_pitch / block bytes * block width`.
 *
 * Does nothing if `image` is NULL.
 *
 * @param[in]  image          Image the copy targets.
 * @param[in]  mip_level      Mip level being copied.
 * @param[out] out_row_pitch  Receives the aligned row pitch, in bytes: one
 *                            row of blocks.
 * @param[out] out_total_size Receives the required buffer size, in bytes,
 *                            for one layer of the level.
 */
void crgpu_image_get_buffer_copy_size(
    crgpu_image image,
    uint32_t mip_level,
    uint64_t *out_row_pitch,
    uint64_t *out_total_size);

/*
 * [CHANGED] crgpu_image_get_buffer_copy_offset_alignment(): its answers for
 * an ASTC image (16 on Metal, the block's bytes; 16 on Vulkan, the least
 * common multiple of the block's bytes and 4; never asked on D3D12, where
 * no ASTC image exists). Its contract is unchanged: it already gives a
 * block format's alignment from the block's bytes.
 *
 * [CHANGED] crgpu_command_list_blit_image(): its comment gains "Neither
 * image may be in a block-compressed format: no API blits into one, and
 * Vulkan's blit from one depends on format features Carrottus GPU doesn't
 * report." It returns nothing, so the rule is the caller's, as its other
 * rules are today (a refusal there is card S48's note, not this draft).
 *
 * [CHANGED] crgpu_graphics_pipeline_create() and the mesh pipeline's create
 * (pass 1): a block-compressed format in `color_attachment_formats`, as
 * `depth_format`, or as a vertex attribute's format is refused with
 * CR_RESULT_GPU_INVALID_FORMAT, in crgpushared.c beside the colour
 * attachment checks already there, before any backend translates it. Why:
 * no API renders to or fetches vertices from a block format, and D3D12's
 * pipeline creation casts these values as its image create does. ABI: a
 * refusal of what was accepted, which no caller does. Tests: above.
 *
 * ---------------------------------------------------------------------------
 * crcore.h
 * ---------------------------------------------------------------------------
 *
 * [CHANGED] CARROTTUS_VERSION_MINOR 4 to 5 (the copies' signatures and the
 * creates' new refusals), as "The version and the breaks" says.
 *
 * No result is added: the feature's refusal is
 * CR_RESULT_GPU_FEATURE_NOT_SUPPORTED (212), already the refusal for a
 * feature a device wasn't created with (CRGPU_FEATURE_PRESENT's), and a
 * caller branches on it the same way (another format, another pack).
 *
 * ---------------------------------------------------------------------------
 * Where each backend does it (not in a header)
 * ---------------------------------------------------------------------------
 *
 * - src/gpu/crgpuformat.c: the block table (width, height, bytes) for every
 *   value, read by crgpu_format_get_texel_block(), the copies' checks and
 *   each backend's copy layout; the 28 names; crgpu_format_get_bit_count()
 *   0 for ASTC. crgpu___internal_format_block_bytes() is replaced by the
 *   table.
 * - src/gpu/crgpushared.c: the creates' block checks (above) and the
 *   copies' region checks, once, called by every backend.
 * - Metal (crgpumetalformat.m, crgpumetalcontext.m,
 *   crgpumetalrenderpass.m): the BC formats (BC1_RGBA and _sRGB, BC2, BC3,
 *   BC4_RUnorm and _RSnorm, BC5_RGUnorm and _RGSnorm, BC6H_RGBUfloat and
 *   _RGBFloat, BC7_RGBAUnorm and _sRGB; no TYPELESS) and the 28 ASTC
 *   formats in its table, the ASTC values in a second array indexed from
 *   1000; the two bits read where the adapter's facts are; bytes per row
 *   and per image from the block.
 * - Vulkan (crgpuvulkancontext.c, crgpuvulkanrenderpass.c): the 28 ASTC
 *   formats mapped; the two features read with the others and enabled
 *   only for a device created with each bit; the copy size from the block.
 *   bufferRowLength and bufferImageHeight pass through in texels, as
 *   Vulkan takes them.
 * - D3D12 (crgpud3d12context.cpp, crgpud3d12renderpass.cpp, crgpud3d12.hpp):
 *   the BC bit on every adapter, the ASTC bit on none; the footprint's
 *   RowPitch from the block (rows of blocks), its Width and Height rounded
 *   up to whole blocks; the cast refuses a value at or past 1000 with
 *   DXGI_FORMAT_UNKNOWN, a guard behind the create's check.
  *
 * ---------------------------------------------------------------------------
 * Records when built (pass 7)
 * ---------------------------------------------------------------------------
 *
 * carrottus/CLAUDE.md, "Carrottus GPU conventions": a bullet on block
 * formats (the bits, the block query, 2D only, whole blocks at level 0,
 * the copies' rules and results, ASTC numbered from 1000).
 * CRGPU_DEVICE_TODO.md: a section for S48, each machine's run and each
 * Vulkan driver's two booleans, Vulkan's ASTC written and not run until a
 * GPU that samples it is at hand. crgpumetalformat.m's comment on what its
 * table holds. crgpusamples' README, CrGPUDynamicIndexing's decoder gone.
 * The workspace's DECISIONS.md card S48 moves to "Decided".
 */
