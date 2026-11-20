/*
 * crgpu_format_uses_draft.h - Carrottus GPU refuses each use of a format
 * the adapter's format table says it can't take (an image's usages, a
 * pipeline's attachments and vertex attributes, a copy), copies a combined
 * depth-stencil image one aspect at a time, the aspect named by the caller
 * in a chained struct, and maps Metal's FloatRG11B10 and BGRA8 vertex
 * formats (card S58).
 *
 * APPROVED by the user 2026-11-20 after a second seven-pass critique, every
 * question as recommended (Q1 a to Q4 a). Version 3, 2026-11-20 (version 1,
 * seven critique passes, version 2; then passes 8 to 14, each fixed before
 * the next); not built. Frozen from here (ROADMAP.md, section 1).
 * Card S58 decided 2026-11-20 (a): "one Carrottus draft: each use (image
 * usages, attachments, vertex attributes, copies) refused without its
 * format's bit, a combined depth-stencil copy refused unless the caller
 * chooses an aspect (a chained struct), Metal's FloatRG11B10 and BGRA8
 * vertex formats mapped." From card S51's Q6 (crgpu_format_capabilities_draft.h,
 * approved and built 2026-11-18; its "Found in the code" 2 and 3).
 *
 * No version raise while carrottus' 0.4.0 is untagged (card X2): every
 * break below is listed against the ABI snapshot.
 *
 * Written unattended on the Mac (Apple M5 Pro, macOS 27) against
 * include/crgpu.h (CRGPU_FORMAT_CAPABILITIES and its nine bits, lines 1060
 * to 1180; CRGPU_IMAGE_USAGE, 2460 to 2525; crgpu_image_create(), 2646 to
 * 2728; CRGPU_IMAGE_ASPECT, 2799 to 2815; crgpu_image_buffer_copy_desc,
 * 7360 to 7447, its Frozen line 7409; the two copies, 7487 to 7582;
 * crgpu_image_get_buffer_copy_size() and the two alignment queries, 7584 to
 * 7660; crgpu_graphics_pipeline_create()'s results, 5601 to 5655),
 * include/crcore.h (207, 222; the GPU struct types to 1035; CR_STRUCT_BASE's
 * rules, 612 to 620), src/gpu/crgpushared.c (crgpu___internal_check_pipeline_outputs(),
 * line 96; crgpu___internal_format_capabilities(), 173, whose mask at 184 to
 * 186 drops COPY for the two-aspect family; crgpu___internal_check_image_block(),
 * 1175; crgpu___internal_check_buffer_image_copy(), 1224), src/gpu/crgpuformat.c
 * (crgpu___internal_format_two_aspects(), 594),
 * src/gpu/backends/metal/crgpumetalformat.m (the vertex column, lines 48 to
 * 112: B8G8R8A8_UNORM and R11G11B10_FLOAT MTLVertexFormatInvalid), the
 * three backends' image creation and copy paths (crgpuvulkanrenderpass.c
 * cr_format_aspect() at 99 and its two copy uses, the D3D12 footprint in
 * crgpud3d12renderpass.cpp, crgpumetalrenderpass.m's copies at 168 and
 * 216), abi.txt; the Vulkan SDK 1.4.363.0's validusage.json (read here);
 * the macOS SDK's MTLVertexDescriptor.h, MTLStageInputOutputDescriptor.h
 * and MTLBlitCommandEncoder.h (read here); Microsoft's documentation of
 * CopyTextureRegion and planar depth-stencil footprints, as remembered.
 * carrottus 5b71f5d, 0.4.0, untagged; passes 8 to 14 rechecked against
 * ad3d5ab (its sources those of 5b71f5d). Not built, not in any target, not
 * compilable. Every declaration below is marked [ADDED], [CHANGED] or
 * [REMOVED]; nothing unmarked changes.
 *
 * Result codes: crgpu_draw_dispatch_checks_draft.h (cards S55 and S56, the
 * same day) takes 379 to 385; this draft takes 386 after them (379 if that
 * draft is declined). Struct types: 1036 and 1037, the next free after
 * CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER (1035).
 *
 * ---------------------------------------------------------------------------
 * Revision notes (version 1 written, then seven passes, each fixed before
 * the next; one lens a pass)
 * ---------------------------------------------------------------------------
 *
 * Pass 1 (correctness against the built code).
 *  1. Wrong in version 1: the aspect was chained onto
 *     crgpu_image_buffer_copy_desc, which is frozen (no base: "a copy
 *     region, passed in arrays", crgpu.h 7451). Nothing can chain onto it.
 *     The card's "a chained struct" needs the desc to be based first: the
 *     desc gains CR_STRUCT_BASE as its first member (a listed break before
 *     1.0; every caller of the two copies sets the base), and the aspect is
 *     CRGPU_IMAGE_COPY_ASPECT chained on it. Its "Frozen" line's reason was
 *     stale besides: the desc is passed one per call, by pointer, never in
 *     an array. Q1 keeps the field-at-the-end shape on record.
 *  2. Found: the table masks COPY off for the eight formats of the
 *     depth-stencil family (crgpushared.c 197 to 199), so with image usages
 *     refused by their bits (this card), a D32_FLOAT_S8X24_UINT image with
 *     TRANSFER_SRC would be refused at creation, before any copy could name
 *     an aspect. Changed: the two depth-stencil formats (D24_UNORM_S8_UINT,
 *     D32_FLOAT_S8X24_UINT) report COPY where the API copies each aspect
 *     (Vulkan both TRANSFER bits; D3D12 always; Metal its pixel format
 *     mapped: Depth32Float_Stencil8 on every Apple GPU, Depth24 none), and
 *     the bit's comment says a copy of them names an aspect. Their six
 *     TYPELESS relatives keep "COPY at most", masked to none.
 *  3. Found: crgpu.h's crgpu_image_create() says Metal alone refuses a
 *     TYPELESS format at creation (INVALID_FORMAT). With usages refused by
 *     the table, every backend refuses a TYPELESS image of any usage but
 *     the two transfers (masked to COPY at most), and Metal those too (its
 *     COPY is 0 for a format it makes no texture of). One rule, read from
 *     the table; Metal's own sentence goes ([CHANGED]).
 *  4. Found: an image view has no format of its own (crgpu_image_view_desc
 *     names none), so the image's format is the one every use reads, and
 *     checking the image's usages at creation covers views, attachments
 *     and bindings; nothing per view is needed.
 *  5. Missing in version 1: crgpu_image_create_placed() takes the same desc
 *     checks; it gains the same refusal in the same place.
 *
 * Pass 2 (the C API's conventions: based or frozen, no hidden policy, no
 * default that decides, errors as codes).
 *  1. No default aspect: a copy of a two-aspect image with no
 *     CRGPU_IMAGE_COPY_ASPECT chained is refused (386), never "depth by
 *     default". A one-aspect image takes the chain or not; with one, its
 *     aspect must be the format's (refused otherwise, 386): a caller may
 *     always name it, never name a wrong one.
 *  2. The refusals by bit return CR_RESULT_GPU_INVALID_FORMAT (207), whose
 *     text already reads "or one the adapter can't use that way
 *     (crgpu_adapter_get_format_capabilities())", as S51's blend refusal
 *     does (Q3).
 *  3. Order: each refusal after the checks that can return the same code
 *     for a reason more particular (UNKNOWN, the block family's rules), so
 *     no existing refusal moves; listed per call below.
 *  4. One aspect a copy: the chained struct holds one CRGPU_IMAGE_ASPECT
 *     value (a bit, not a mask of two), refused if 0 or more than one bit.
 *
 * Pass 3 (the platform APIs on all three backends).
 *  1. Vulkan (validusage.json, read here): an image's usage must be one
 *     vkGetPhysicalDeviceImageFormatProperties allows for its format and
 *     tiling (VUID-VkImageCreateInfo-imageCreateImageFormatPropertiesList-02251),
 *     which needs each usage's format feature of optimal tiling (the bits
 *     S51 reads);
 *     VUID-VkGraphicsPipelineCreateInfo-renderPass-06582 (a colour format of
 *     dynamic rendering with COLOR_ATTACHMENT), -06587 (the depth format's
 *     DEPTH_STENCIL_ATTACHMENT); VUID-VkVertexInputAttributeDescription-format-00623
 *     (VERTEX_BUFFER of the buffer features);
 *     VUID-VkBufferImageCopy-aspectMask-09103 (one aspect a region) and
 *     VUID-vkCmdCopyBufferToImage-dstImage-07978 (a depth/stencil image's
 *     bufferOffset a multiple of 4, the image-to-buffer twin likewise); the depth
 *     aspect of D24_UNORM_S8_UINT copied as X8_D24 (32 bits a texel, depth
 *     in the low 24), of D32_SFLOAT_S8_UINT as D32_SFLOAT, the stencil as
 *     S8_UINT (one byte), per the specification's table of depth/stencil
 *     aspect copies; a depth copy into an image takes values 0 to 1 only
 *     without VK_EXT_depth_range_unrestricted.
 *  2. D3D12: a depth-stencil format is planar for copies: plane 0 the
 *     depth, plane 1 the stencil, each its own subresource (D3D12CalcSubresource
 *     with the plane slice) and its own footprint from GetCopyableFootprints
 *     (depth 4 bytes a texel, stencil 1, as remembered of Microsoft's
 *     planar layout pages; to be read on Windows when built); a copy to or
 *     from a depth-stencil resource covers a whole subresource
 *     (CopyTextureRegion's remarks, as remembered: Q4). CheckFeatureSupport's
 *     FORMAT_SUPPORT gives RENDER_TARGET, DEPTH_STENCIL and IA_VERTEX_BUFFER
 *     as S51 reads them.
 *  3. Metal (read here): MTLBlitOptionDepthFromDepthStencil and
 *     MTLBlitOptionStencilFromDepthStencil on copyFromBuffer:...options:
 *     and copyFromTexture:...options: (MTLBlitCommandEncoder.h 28 to 31,
 *     79); a Depth32Float_Stencil8 copy with neither is outside Metal's
 *     rules (the fault S51 found). MTLVertexFormatUChar4Normalized_BGRA
 *     (MTLVertexDescriptor.h 76, macOS 10.13) and MTLVertexFormatFloatRG11B10
 *     (90, macOS 14 / iOS 17; the deployment target is 26.5, so no
 *     availability guard). R10G10B10A2_UINT has no Metal vertex format
 *     (only the normalized 1010102 ones): it stays unmapped and reports no
 *     VERTEX, which the refusal now holds.
 *  4. Version 1 refused a format without SAMPLED for a usage of SAMPLED on
 *     a depth format; D3D12's SAMPLED is read from the SRV twin
 *     (R32_FLOAT for D32_FLOAT), as S51 built: checked that the table
 *     already answers so. Kept.
 *
 * Pass 4 (industry practice).
 *  1. WebGPU: createTexture refuses a usage its format's capabilities
 *     lack (RENDER_ATTACHMENT, STORAGE_BINDING per format table);
 *     createRenderPipeline refuses a colour target without render
 *     capability and a vertex format outside its list; a buffer-texture
 *     copy of a depth-stencil texture names `aspect` ("depth-only" or
 *     "stencil-only", "all" refused for a combined format), the depth
 *     aspect copyable only for some formats. The shape here is WebGPU's:
 *     one aspect a copy, named, never inferred.
 *  2. SDL_GPU: SDL_GPUTextureSupportsFormat(device, format, type, usage)
 *     and texture creation refused without it. NVRHI: per-format support
 *     queries; its copies of depth-stencil take a plane. bgfx: format caps
 *     flags checked at creation in debug builds. As remembered of each.
 *  3. Kept: the check at creation (images, pipelines), not at use; one
 *     aspect a copy.
 *
 * Pass 5 (performance).
 *  1. Every refusal reads the adapter's 161-slot table once per use at
 *     creation time (an index and a mask): images and pipelines are made
 *     once, not per frame. No hot path changes.
 *  2. A copy reads its chain only for its aspect: one pointer test, then a
 *     based struct's check where a chain is given. The copies stay
 *     allocation-free (cr_noalloc_tests).
 *  3. Metal's two vertex formats change nothing for formats already mapped.
 *
 * Pass 6 (tests: each failing first, on every backend).
 *  1. Each refusal written so it fails against the built code: images of
 *     each usage with a format whose table bit is 0 on the running adapter
 *     (found at run time from crgpu_adapter_get_format_capabilities(), not
 *     hard-coded, so each backend's table drives its own cases) are created
 *     today and fail the test.
 *  2. The combined depth-stencil copy: on Metal (Depth32Float_Stencil8),
 *     Vulkan and D3D12 (both formats where the adapter has them), each
 *     aspect copied out of a cleared image and its bytes read (depth 0.25
 *     as a float, stencil 0x5A), and back in, read by a shader; the
 *     no-aspect refusal fails first against today's code (which records the
 *     copy; S51 found Metal's runs with no option).
 *  3. Metal's vertex formats: a two-triangle draw reading a BGRA8 colour
 *     and an RG11B10 attribute, its pixels read back; fails first (the
 *     attribute maps to MTLVertexFormatInvalid today, which Metal's
 *     pipeline creation refuses, so the test's pipeline fails), passes
 *     after.
 *  4. Vulkan written and type-checked here; D3D12 written, not compiled.
 *
 * Pass 7 (what callers above see).
 *  1. Source: every caller of the two copies sets the desc's base
 *     ({CR_STRUCT_TYPE_GPU_IMAGE_BUFFER_COPY_DESC, NULL}); a desc left
 *     zeroed is refused with CR_RESULT_STRUCT_WRONG_TYPE, after `desc`'s
 *     NULL check. ABI: the desc's size and every offset in it change.
 *  2. Behaviour: an image, pipeline or copy the adapter can't take, which
 *     reached the driver before (Vulkan outside its rules, Metal's
 *     validation ending the process, D3D12's debug layer), is refused with
 *     207 at creation. A caller that picks formats by trying is pointed at
 *     the query.
 *  3. A caller that sizes copies with crgpu_image_get_buffer_copy_size() for
 *     a depth-stencil image gets one layout that serves either aspect (Q2).
 *
 * ---------------------------------------------------------------------------
 * Revision notes, second round (version 2, then passes 8 to 14, each fixed
 * before the next; version 3)
 * ---------------------------------------------------------------------------
 *
 * Pass 8 (correctness against today's code, all three backends' sources).
 *  1. Wrong line numbers in version 2: the desc's Frozen line is crgpu.h
 *     7409 (pass 1, 1 said 7451); crgpushared.c's capabilities are at 173,
 *     the two-aspect mask at 184 to 186 (pass 1, 2 said 197 to 199).
 *     Corrected in the list of what was read; nothing else moves.
 *  2. Found: D32_FLOAT_S8X24_UINT's texel block is 8 bytes today
 *     (crgpuformat.c 382, DXGI's size), and the three copy queries answer
 *     from it. Under Q2 (a) they answer the depth aspect's 4: a changed
 *     answer of crgpu_image_get_buffer_copy_size() and the two alignment
 *     queries for that format, now listed in "The version and the breaks".
 *     D24_UNORM_S8_UINT's block is 4 bytes already (407).
 *  3. Checked: Vulkan's copy offset alignment is the least common multiple
 *     of the block's bytes and 4 (crgpuvulkanrenderpass.c 277 to 287), which
 *     keeps VUID-vkCmdCopyBufferToImage-dstImage-07978 (read here) for
 *     D16_UNORM and for a stencil copy (a 1-byte block gives 4), as Q2 (a)
 *     says.
 *  4. Checked: Vulkan's copies take the aspect from the image's format
 *     (cr_format_aspect(), crgpuvulkanrenderpass.c 362 and its twin), which
 *     for a two-aspect format is DEPTH | STENCIL, outside
 *     VUID-VkBufferImageCopy-aspectMask-09103 (read here) today; the chained
 *     aspect replaces it ("Where it is done").
 *  5. Found (counted outside the draft): a caller in the workspace adds both
 *     transfer usages to every 2D and 3D image it makes, depth ones
 *     included. With usages refused by the table, an image of a format
 *     without COPY would be refused where it was made: no format with a
 *     texel block that a backend copies; the two-aspect pair reports COPY
 *     where each aspect copies (pass 1, 2); D24_UNORM_S8_UINT has no Metal
 *     texture at all (refused already). Said in pass 14.
 *
 * Pass 9 (conventions: based or frozen, no hidden policy, codes, append-only).
 *  1. Wrong in version 2: the copies' results gave
 *     CR_RESULT_STRUCT_WRONG_TYPE for a chained struct of another type.
 *     CR_STRUCT_BASE's rules (crcore.h 612 to 620) give
 *     CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN for a chained struct the call
 *     doesn't take there and CR_RESULT_STRUCT_DUPLICATE_IN_CHAIN for one type
 *     twice. Corrected: WRONG_TYPE for the desc's own base; 13 for a foreign
 *     struct anywhere in the chain; 14 for a second aspect. Test 3 gains the
 *     second aspect.
 *  2. Checked: CRGPU_IMAGE_COPY_ASPECT is upper case as the other based value
 *     structs a caller fills (CR_STRUCT_BASE, not _OUT); 1036 and 1037 are
 *     the next struct types and no other draft holds them; 386 follows the
 *     S55/S56 draft's 385, appended.
 *  3. Checked: no default aspect, no flag, no substitution of a format or a
 *     usage. The refusal names no usage (Carrottus has the code alone,
 *     CONVENTIONS.md "Error handling"): the usage bits' order is the order
 *     of the checks, and the caller asks the table for the fact.
 *
 * Pass 10 (the platform APIs).
 *  1. Read here: VUID-vkCmdCopyBufferToImage-imageSubresource-09105 (the
 *     aspect is one the image has) and VUID-VkBufferImageCopy-aspectMask-09103
 *     (one bit): 386's three cases are Vulkan's two rules, for every backend.
 *  2. Read here (MTLBlitCommandEncoder.h 28 to 31, 79): the depth and stencil
 *     options are the buffer-texture copies'; unchanged from pass 3.
 *  3. D3D12, as remembered (to be read on Windows): plane 1 of either
 *     depth-stencil format is 1 byte a texel in its footprint, plane 0 4
 *     bytes; and Q4's whole-subresource rule. Unchanged.
 *  4. Checked: no VUID or Metal rule asks the stencil copy's row pitch to be
 *     other than a multiple of its 1-byte texel above Q2 (a)'s alignments
 *     (as read in pass 3; D3D12's 256 as remembered).
 *
 * Pass 11 (industry practice).
 *  1. WebGPU (as remembered): a copy of a depth-stencil texture names one
 *     aspect; depth32float's depth aspect copies out but not in, a narrowing
 *     for the APIs under it. Here a copy goes both ways wherever the
 *     adapter's table gives COPY for the format: the table is per adapter,
 *     so no narrowing is built in; a backend that can't copy an aspect in
 *     reports no COPY. Kept.
 *  2. SDL_GPU and NVRHI (as remembered) refuse a texture of a usage its
 *     format lacks at creation, as here. No change.
 *
 * Pass 12 (performance).
 *  1. A copy: one compare of the desc's base and one pointer test of its
 *     chain; a chain is walked only where given (a depth-stencil copy).
 *     Copies run per frame in a caller's uploads; the cost is a few
 *     instructions, no allocation (cr_noalloc_tests).
 *  2. An image create: at most six table reads, once. A pipeline create: a
 *     table read per attachment and attribute, once. Nothing per draw.
 *
 * Pass 13 (tests: each failing first, figures by name).
 *  1. Named: crgpu_objects_test's objects_format_image_uses (1),
 *     objects_format_pipeline_uses (2), objects_copy_aspect_refusals and
 *     objects_copy_aspect_depth_stencil (3), objects_vertex_formats (4);
 *     crgpu_device_test's device_format_copy_queries (the queries' answers,
 *     pass 8, 2: 8 bytes a texel to 4 for D32_FLOAT_S8X24_UINT, failing
 *     first).
 *  2. Added to 3: a second aspect chained after the first (14), a foreign
 *     struct after it (13).
 *  3. Said per backend: Metal built, run and seen failing first here; Vulkan
 *     written and type-checked, not run; D3D12 written, not compiled.
 *
 * Pass 14 (callers above, and the ABI list).
 *  1. Every caller of the two copies sets the desc's base: counted outside
 *     the draft (no caller is named here; ./check.sh caller-names passes),
 *     three call sites of one caller's image uploads and read-backs, three
 *     samples, and carrottus's own tests. A caller that copies a two-aspect
 *     image now names the aspect; one that never does changes the base only.
 *  2. A caller that adds both transfer usages to every image it makes sees a
 *     refusal only for a format the table gives no COPY; one that wants its
 *     own refusal reads the table first (its own record).
 *  3. The ABI list: crgpu_image_buffer_copy_desc gains a 16-byte base first
 *     (56 bytes to 72 on a 64-bit target, every field 16 later);
 *     CRGPU_IMAGE_COPY_ASPECT added; CR_STRUCT_TYPE_GPU_IMAGE_BUFFER_COPY_DESC
 *     (1036), _IMAGE_COPY_ASPECT (1037) and CR_RESULT_GPU_COPY_ASPECT (386)
 *     added; the copy queries' answer for D32_FLOAT_S8X24_UINT (behaviour).
 *     ./check.sh --update-abi lists them; the misuse generator is rerun.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * Card S51 gave every adapter a table of what each format can do and used
 * it for one refusal (blending). Every other use the table answers still
 * reaches the driver: on Vulkan outside its rules, on Metal ending the
 * process under validation, on D3D12 a debug-layer error. And a copy of a
 * combined depth-stencil image does three different things (Vulkan sends
 * both aspects in one region, D3D12 copies plane 0 in the combined format's
 * footprint, Metal copies with no blit option), because no desc can say
 * which aspect. One rule, read from the table, everywhere.
 *
 * ---------------------------------------------------------------------------
 * What changes, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * [CHANGED] crgpu_image_create() and _create_placed() refuse (207) a usage
 * whose capability bit the device's adapter lacks for the format: SAMPLED,
 * COLOR_ATTACHMENT, DEPTH_STENCIL_ATTACHMENT, STORAGE, and the two
 * TRANSFERs (COPY). [CHANGED] crgpu_graphics_pipeline_create() refuses (207)
 * a colour attachment format without COLOR_ATTACHMENT, a depth format
 * without DEPTH_STENCIL_ATTACHMENT and a vertex attribute format without
 * VERTEX. [CHANGED] crgpu_image_buffer_copy_desc gains a base;
 * [ADDED] CRGPU_IMAGE_COPY_ASPECT, chained on it, names the one aspect a
 * copy moves; [CHANGED] the two copies refuse a two-aspect image's copy
 * without one, and an aspect the format lacks (386), and lay out an
 * aspect's texels as its own block (depth 4 bytes, stencil 1). [CHANGED]
 * The table reports COPY for the two depth-stencil formats where each
 * aspect copies. Metal maps B8G8R8A8_UNORM and R11G11B10_FLOAT as vertex
 * formats.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 *  - Vulkan (validusage.json, SDK 1.4.363.0, and the specification's
 *    depth/stencil copy table as remembered): pass 3, 1.
 *  - D3D12 (Microsoft's documentation, as remembered): pass 3, 2.
 *  - Metal (the macOS SDK's headers, read here): pass 3, 3.
 *  - WebGPU, SDL_GPU, NVRHI, bgfx (as remembered): pass 4.
 * Hazel is not compared (Carrottus is not measured against an engine).
 *
 * ---------------------------------------------------------------------------
 * What the caller decides, and what Carrottus GPU only reports
 * ---------------------------------------------------------------------------
 *
 * The caller asks the table (crgpu_adapter_get_format_capabilities()) and
 * chooses formats; Carrottus GPU refuses what the table says no to and
 * never substitutes a format, a usage or an aspect. Which aspect a copy
 * moves is the caller's, named each time.
 *
 * ---------------------------------------------------------------------------
 * Left out, and why
 * ---------------------------------------------------------------------------
 *
 *  - Filtering refused for a sampler over a format without FILTERED: a
 *    sampler names no format; the pairing is made in a descriptor set
 *    write, per draw. A card if a caller asks.
 *  - STORAGE_READ refused: a storage usage covers writes; reads without the
 *    bit stay undefined, as crgpu.h's CRGPU_IMAGE_USAGE_STORAGE already says.
 *  - Bundle inheritance formats (crgpu_bundle_inheritance_desc): its begin
 *    is void; it mirrors a pipeline's formats, which are checked.
 *  - Blits (crgpu_command_list_blit_image()) by the table: blits are the
 *    caller's rule today, unchecked (crgpu.h); a card of their own.
 *  - Sample counts per format: not in the table.
 *
 * ---------------------------------------------------------------------------
 * The version and the breaks
 * ---------------------------------------------------------------------------
 *
 *  - crgpu.h: crgpu_image_buffer_copy_desc gains `base` first (its size and
 *    every offset change: an ABI break, listed); CRGPU_IMAGE_COPY_ASPECT
 *    added; comments of CRGPU_FORMAT_CAPABILITY_COPY, crgpu_image_create(),
 *    _create_placed(), crgpu_graphics_pipeline_create(), the two copies and
 *    the three copy queries changed.
 *  - crcore.h: CR_STRUCT_TYPE_GPU_IMAGE_BUFFER_COPY_DESC (1036) and
 *    CR_STRUCT_TYPE_GPU_IMAGE_COPY_ASPECT (1037) added; 386 added.
 *  - Behaviour: creations and copies the adapter can't take are refused
 *    (they reached the driver); D24_UNORM_S8_UINT and D32_FLOAT_S8X24_UINT
 *    report COPY where each aspect copies (they reported none); the three
 *    copy queries answer 4 bytes a texel for D32_FLOAT_S8X24_UINT (8
 *    before; pass 8, 2).
 *  - No version raise: 0.4.0 untagged (card X2). ./check.sh --update-abi;
 *    the misuse generator rerun (the copies' desc).
 *
 * New handles: none. New struct types: two (the desc's, the chain's).
 * Stored formats: none. Thread: nothing changes. Allocation: none added.
 *
 * ---------------------------------------------------------------------------
 * Questions (each with options and a recommendation)
 * ---------------------------------------------------------------------------
 *
 * Q1. HOW THE ASPECT IS NAMED.
 *     (a) RECOMMENDED. As the card decided: the copy desc becomes based
 *         (it has grown twice before 1.0, so it is a struct that grows) and
 *         CRGPU_IMAGE_COPY_ASPECT chains on it. After 1.0 a further need
 *         chains too.
 *     (b) A field `aspect` at the desc's end (0 meaning the format's only
 *         aspect, refused for a two-aspect format): one listed break, no
 *         base for callers to set; and the desc stays frozen after 1.0,
 *         so the next need is a new call.
 *
 * Q2. THE COPY-SIZE AND ALIGNMENT QUERIES FOR A TWO-ASPECT IMAGE (they take
 *     an image, no aspect).
 *     (a) RECOMMENDED. They answer for the depth aspect (4 bytes a texel),
 *         and the header says that answer serves the stencil aspect too: a
 *         row pitch of whole 4-byte texels is a whole number of 1-byte
 *         ones (the stencil copy's row length is pitch / 1), D3D12's 256 and
 *         512 hold for both, and Vulkan's and Metal's offset alignment of 4
 *         is a multiple of the stencil's. No new call.
 *     (b) A query per aspect (an aspect parameter): one new call, exact
 *         sizes for the stencil (a quarter), a public addition.
 *     (c) The queries keep refusing such an image: a caller then can't size
 *         a copy the copies now take.
 *
 * Q3. THE CODE FOR A USE THE TABLE REFUSES.
 *     (a) RECOMMENDED. CR_RESULT_GPU_INVALID_FORMAT (207), as S51's blend
 *         refusal and as 207's text already says.
 *     (b) A new code per use (a usage, an attachment, a vertex format): the
 *         caller knows the use from the call; the code would repeat it.
 *
 * Q4. A DEPTH OR STENCIL COPY OF PART OF A LEVEL (D3D12, as remembered,
 *     copies a depth-stencil resource's subresource whole only).
 *     (a) RECOMMENDED. Read Microsoft's CopyTextureRegion remarks when this
 *         is built on Windows; if they say so, a copy of a depth or stencil
 *         aspect (of any depth format, one aspect or two) covers its whole
 *         level on every backend (CR_RESULT_DESC_INVALID_PARAMETERS, in the
 *         region's checks), stated and tested; if not, no rule.
 *     (b) No rule now: D3D12 may then copy part of a level wrongly where
 *         the others don't.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * crgpu_objects_test (and crgpu_device_test where no list is needed), every
 * backend, every adapter the run takes:
 *  1. Images: for every named format and each of the six usages, the
 *     table's bit read at run time; where it is 0, crgpu_image_create() and
 *     _create_placed() refuse with 207 and leave no handle, and where it is
 *     1 for a simple 2D 4 x 4 image, the image is made. Two wrong at once:
 *     a block format with COLOR_ATTACHMENT keeps its earlier
 *     INVALID_IMAGE_USAGE; UNKNOWN keeps 207 first.
 *  2. Pipelines: a colour attachment of a format without COLOR_ATTACHMENT
 *     (R32G32B32_FLOAT, which no backend renders to), a depth format of
 *     R8_UNORM, a vertex attribute of a format without VERTEX (R10G10B10A2_UINT
 *     on Metal; a format the running adapter's table gives none on each
 *     other backend): 207; the blend refusal (S51) still last.
 *  3. Copies: the desc's base wrong (STRUCT_WRONG_TYPE), a chain of a
 *     foreign type (STRUCT_UNKNOWN_IN_CHAIN), the same after an aspect, and
 *     a second aspect after the first (STRUCT_DUPLICATE_IN_CHAIN); a
 *     two-aspect image's copy
 *     with no chain, with aspect 0, with DEPTH | STENCIL, and with COLOR
 *     (386 each); a one-aspect image with its own aspect chained (accepted)
 *     and another (386). Then each aspect of a 4 x 4 D32_FLOAT_S8X24_UINT
 *     image (and D24_UNORM_S8_UINT where the adapter has it) cleared to
 *     depth 0.25 and stencil 0x5A, copied out and read: 16 floats 0.25 and
 *     16 bytes 0x5A (D24: 16 words whose low 24 bits are 0x400000), and
 *     copied in (depth 0.75, stencil 0x33), read by a draw's depth and
 *     stencil tests. The queries (Q2): the answer for each aspect's copy.
 *  4. Metal's vertex formats: a draw reading a B8G8R8A8_UNORM colour
 *     (bytes 0xFF, 0x80, 0x40, 0x20 in memory: b, g, r, a as DXGI orders
 *     them, read as r 0.251, g 0.502, b 1.0, a 0.125) and an
 *     R11G11B10_FLOAT attribute (1.0, 0.5, 2.0),
 *     each written to a pixel and read back; on Vulkan and D3D12 where the
 *     table gives VERTEX.
 *  5. cr_misuse_tests regenerated; cr_noalloc_tests' frame copies with a
 *     based desc, 0 of ours.
 *  6. device_format_copy_queries (crgpu_device_test): the three copy
 *     queries for a D32_FLOAT_S8X24_UINT image answer the depth aspect's 4
 *     bytes a texel (8 today: fails first), and a stencil copy at that
 *     pitch and offset is accepted.
 * By name (pass 13, 1): 1 objects_format_image_uses, 2
 * objects_format_pipeline_uses, 3 objects_copy_aspect_refusals and
 * objects_copy_aspect_depth_stencil, 4 objects_vertex_formats.
 * Seen failing first: 1, 2 and 3's refusals pass today's code wrongly (the
 * creates succeed; the copies record), and 3's reads and 4's draw fail on
 * Metal (no option; the attribute unmapped).
 * Where each runs: the Mac (macos-metal, Debug and Release, Metal's
 * validation with warnings fatal, -asan, -tsan); Vulkan type-checked here;
 * D3D12 written, not compiled. Owed on Windows 11 (Iris Xe, RTX A2000,
 * WARP; Q4 read there) and Fedora (ANV, NVK, RADV). ./check.sh: abi,
 * doxygen, conventions, struct-bases, caller-names.
 *
 * ---------------------------------------------------------------------------
 * crgpu.h
 * ---------------------------------------------------------------------------
 */

/*
 * [CHANGED] (comment only) in CRGPU_FORMAT_CAPABILITIES:
 *
 *	/** Copied both ways between a buffer and an image by
 *	 * crgpu_command_list_copy_buffer_to_image() and _copy_image_to_buffer(),
 *	 * and so the format of an image created with CRGPU_IMAGE_USAGE_TRANSFER_SRC
 *	 * or _TRANSFER_DST: the format has a texel block, and Vulkan
 *	 * `TRANSFER_SRC` and `TRANSFER_DST`; D3D12 a texture format; Metal a
 *	 * pixel format it maps. A depth-and-stencil format has it where each of
 *	 * its aspects copies, and every copy of it names one aspect
 *	 * (CRGPU_IMAGE_COPY_ASPECT). * /
 *	CRGPU_FORMAT_CAPABILITY_COPY                     = 1 << 7,
 *
 * and in each of SAMPLED, COLOR_ATTACHMENT, DEPTH_STENCIL_ATTACHMENT,
 * STORAGE, VERTEX: "An image, pipeline or attribute that needs it is
 * refused without it (CR_RESULT_GPU_INVALID_FORMAT)."
 */

/**
 * [CHANGED] (results only)
 * @brief Allocates a GPU image (texture).
 *
 * ... unchanged, but:
 * @retval CR_RESULT_GPU_INVALID_FORMAT `desc->format` is
 *         CRGPU_FORMAT_UNKNOWN; or, after the block-format checks and
 *         before the size checks, `desc->usage` has a usage whose
 *         capability the device's adapter lacks for the format
 *         (crgpu_adapter_get_format_capabilities()): SAMPLED, COLOR_ATTACHMENT,
 *         DEPTH_STENCIL_ATTACHMENT, STORAGE, and COPY for TRANSFER_SRC or
 *         TRANSFER_DST, checked in the usage bits' order. So a TYPELESS
 *         format is refused with any usage but the two transfers, on every
 *         backend, and with those too where the backend makes no texture of
 *         it. [REMOVED] "or, on Metal and after the block-format checks, a
 *         format Metal has no texture of (every TYPELESS one, BC's
 *         included)": the table's answer now, on every backend.
 * crgpu_image_create_placed() takes the same check in the same place.
 */
CR_RESULT crgpu_image_create(
    crgpu_device device,
    const crgpu_image_desc *desc,
    crgpu_image *out_image);

/**
 * [CHANGED] (results only)
 * @brief Creates a graphics pipeline.
 *
 * ... unchanged, but CR_RESULT_GPU_INVALID_FORMAT is returned, after the
 * block-format refusals and before the depth bias clamp's, for: a colour
 * attachment format (one of the first `color_attachment_count`) without
 * CRGPU_FORMAT_CAPABILITY_COLOR_ATTACHMENT; a depth format other than
 * CRGPU_FORMAT_UNKNOWN without _DEPTH_STENCIL_ATTACHMENT; a vertex
 * attribute's format (a pipeline with a vertex module) without _VERTEX;
 * in that order, each on the device's adapter. The blend refusal stays
 * last.
 */

/**
 * [ADDED]
 * @brief Which one aspect of an image a copy between it and a buffer moves,
 * chained onto crgpu_image_buffer_copy_desc.
 *
 * Needed for an image of a depth-and-stencil format
 * (CRGPU_FORMAT_D24_UNORM_S8_UINT, CRGPU_FORMAT_D32_FLOAT_S8X24_UINT),
 * whose copy moves its depth or its stencil, never both; allowed for any
 * other image, naming its format's one aspect.
 *
 * In the buffer, an aspect's texels are its own block, on every backend:
 * - the depth of D32_FLOAT_S8X24_UINT: 4 bytes a texel, a 32-bit float;
 * - the depth of D24_UNORM_S8_UINT: 4 bytes a texel, the depth in the low
 *   24 bits as an unsigned normalized value, the top 8 undefined when read
 *   and ignored when written;
 * - the stencil: 1 byte a texel.
 * A depth copied into an image is 0 to 1.
 *
 * Why: the one aspect each API's copy takes (Vulkan's aspectMask, one bit;
 * D3D12's plane slice; Metal's blit option), named by the caller (card
 * S58). Since 0.4.0.
 *
 * Maps to: VkBufferImageCopy::imageSubresource.aspectMask (Vulkan); the
 * plane slice of the subresource and its own footprint (D3D12);
 * MTLBlitOptionDepthFromDepthStencil or _StencilFromDepthStencil (Metal).
 */
typedef struct CRGPU_IMAGE_COPY_ASPECT {
	/** `type` is CR_STRUCT_TYPE_GPU_IMAGE_COPY_ASPECT; `next` is NULL
	 * (nothing chains onto it yet). Set by the caller. See CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** One CRGPU_IMAGE_ASPECT bit the image's format has: COLOR, DEPTH or
	 * STENCIL. Not 0, not two. */
	CRGPU_IMAGE_ASPECT aspect;
} CRGPU_IMAGE_COPY_ASPECT;

/**
 * [CHANGED] based (Q1 a): `base` added first; its "Frozen" line [REMOVED].
 * @brief Describes a rectangular texel region and target mip/layer used for
 * a buffer-image copy.
 *
 * ... unchanged, and: an image of a depth-and-stencil format is copied one
 * aspect at a time, named by a CRGPU_IMAGE_COPY_ASPECT chained on `base`;
 * its texel block is then the aspect's (CRGPU_IMAGE_COPY_ASPECT), for
 * every rule below that names the block.
 */
typedef struct crgpu_image_buffer_copy_desc {
	/** [ADDED] `type` is CR_STRUCT_TYPE_GPU_IMAGE_BUFFER_COPY_DESC; `next`
	 * is NULL or a CRGPU_IMAGE_COPY_ASPECT. Set by the caller. See
	 * CR_STRUCT_BASE. Since 0.4.0. */
	CR_STRUCT_BASE base;
	/* ... buffer_offset, buffer_row_length, buffer_image_height, mip_level,
	 * base_array_layer, array_layer_count, image_x/y/z, width, height,
	 * depth: unchanged ... */
} crgpu_image_buffer_copy_desc;

/**
 * [CHANGED] (results)
 * @brief Records a copy from an image subresource region into a buffer.
 *
 * ... unchanged, but checked in this order:
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST ...
 * @retval CR_RESULT_GPU_INVALID_IMAGE ...
 * @retval CR_RESULT_GPU_INVALID_BUFFER ...
 * @retval CR_RESULT_NULL_DESC ...
 * @retval CR_RESULT_STRUCT_WRONG_TYPE [ADDED] `desc->base.type` is not
 *         CR_STRUCT_TYPE_GPU_IMAGE_BUFFER_COPY_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN [ADDED] A chained struct of any
 *         type but CR_STRUCT_TYPE_GPU_IMAGE_COPY_ASPECT.
 * @retval CR_RESULT_STRUCT_DUPLICATE_IN_CHAIN [ADDED] Two
 *         CRGPU_IMAGE_COPY_ASPECTs chained.
 * @retval CR_RESULT_GPU_INVALID_FORMAT ... (the image's format: no texel
 *         block, as before)
 * @retval CR_RESULT_GPU_COPY_ASPECT [ADDED] The image's format has two
 *         aspects and no CRGPU_IMAGE_COPY_ASPECT is chained; or the one
 *         chained is 0, has two bits, or names an aspect the format lacks.
 * @retval CR_RESULT_GPU_INVALID_IMAGE_USAGE ...
 * @retval CR_RESULT_GPU_INVALID_BUFFER_USAGE ...
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS ... (the region, against the
 *         aspect's block)
 * ... 372, 373, 374 and the offset, pitch and range as before.
 */
CR_RESULT crgpu_command_list_copy_image_to_buffer(
    crgpu_command_list cmd,
    crgpu_image src_image,
    crgpu_buffer dst_buffer,
    const crgpu_image_buffer_copy_desc *desc);

/**
 * [CHANGED] (results, as crgpu_command_list_copy_image_to_buffer()).
 */
CR_RESULT crgpu_command_list_copy_buffer_to_image(
    crgpu_command_list cmd,
    crgpu_buffer src_buffer,
    crgpu_image dst_image,
    const crgpu_image_buffer_copy_desc *desc);

/**
 * [CHANGED] (comment only; Q2 a), in crgpu_image_get_buffer_copy_size(),
 * crgpu_image_get_buffer_copy_offset_alignment() and
 * crgpu_image_get_buffer_copy_row_pitch_alignment():
 *
 * "For an image of a depth-and-stencil format, the answer is the depth
 * aspect's (4 bytes a texel), and serves a copy of either aspect: a
 * stencil copy at that row pitch has a row length of the pitch in texels
 * (one byte each), and the alignments are multiples of the stencil's."
 */

/*
 * ---------------------------------------------------------------------------
 * crcore.h
 * ---------------------------------------------------------------------------
 *
 * [ADDED] after 1035:
 *	/** crgpu_image_buffer_copy_desc (CR_STRUCT_BASE). * /
 *	CR_STRUCT_TYPE_GPU_IMAGE_BUFFER_COPY_DESC     = 1036,
 *	/** CRGPU_IMAGE_COPY_ASPECT (CR_STRUCT_BASE). * /
 *	CR_STRUCT_TYPE_GPU_IMAGE_COPY_ASPECT          = 1037,
 *
 * [ADDED] after 385 (crgpu_draw_dispatch_checks_draft.h; 379 if it is
 * declined). Why: a fact the caller acts on (name the aspect). Tests:
 * crgpu_objects_test 3.
 *	/** A copy of an image names no aspect where its format has two, or an
 *	 * aspect that is 0, two, or one the format lacks. Since 0.4.0. * /
 *	CR_RESULT_GPU_COPY_ASPECT                                 = 386,
 *
 * cr_result_str() names it.
 *
 * ---------------------------------------------------------------------------
 * Where it is done (not in a header)
 * ---------------------------------------------------------------------------
 *
 * - src/gpu/crgpushared.c: crgpu___internal_check_image_uses(adapter facts,
 *   desc) after crgpu___internal_check_image_block(), in every backend's
 *   desc check (create and create_placed); crgpu___internal_check_pipeline_outputs()
 *   gains the three loops after its block loops;
 *   crgpu___internal_format_capabilities()'s two-aspect mask narrowed to
 *   the six TYPELESS relatives; crgpu___internal_check_buffer_image_copy()
 *   gains the base and chain checks (cr_struct_check, then the chain's),
 *   the aspect's check after the format's, and takes the aspect's block
 *   (a small table: depth 4, stencil 1) in place of the format's for a
 *   two-aspect image; crgpu___copy_layout gains the aspect for the
 *   backends.
 * - Vulkan: cr_format_aspect() for copies replaced by the layout's aspect
 *   (crgpuvulkanrenderpass.c 99 and its two uses).
 * - D3D12: the subresource's plane slice from the aspect (1 for stencil),
 *   the footprint asked of GetCopyableFootprints for that plane.
 * - Metal: the blit option from the aspect for Depth32Float_Stencil8; the
 *   two vertex formats in crgpumetalformat.m's table (B8G8R8A8_UNORM's
 *   PRESENT row and R11G11B10_FLOAT's PIX row), so the table reports VERTEX
 *   for both, read from the same row the pipeline maps.
 *
 * ---------------------------------------------------------------------------
 * Records when built
 * ---------------------------------------------------------------------------
 *
 * carrottus/CLAUDE.md: the format capabilities bullet ("Nothing else is
 * refused by the table yet") rewritten; the block-format bullet's copy
 * sentence. CRGPU_DEVICE_TODO.md, "Per-format capabilities": S58 built,
 * each machine's run, Q4's reading. The workspace's DECISIONS.md: a card for
 * Q4 if Microsoft's remarks don't settle it; cards for "Left out" if a
 * caller asks.
 */
