/*
 * crgpu_copy_rows_draft.h - Carrottus GPU's buffer-image copies: a buffer
 * row length or image height shorter than the copied region refused, each
 * with its own code, and a 3D copy's taller slice pitch, which D3D12 lays
 * out differently, refused (card S52, decided 2026-11-17: a).
 *
 * DRAFT, 2026-11-18, version 1 (seven critique passes), for the user's
 * approval. Nothing built yet.
 * APPROVED 2026-11-18 with the user, every question as recommended.
 *
 * Written unattended on the Mac (Apple M5 Pro, macOS 27) against
 * include/crgpu.h (the copy desc and both copies), include/crcore.h,
 * src/gpu/crgpushared.c and .h (crgpu___internal_check_buffer_image_copy()
 * and its layout), the three backends' copies (crgpuvulkanrenderpass.c,
 * crgpud3d12renderpass.cpp, crgpumetalrenderpass.m), abi.txt, the tests
 * (crgpu_objects_test.c, cr_noalloc_tests.c), the copies' callers in the
 * workspace (a caller's image uploads and readbacks, crgpusamples), the
 * Vulkan SDK 1.4.363.0's validusage.json and the macOS 27 SDK's
 * MTLBlitCommandEncoder.h, as read on 2026-11-18 (carrottus 0.4.0,
 * untagged). Not built, not in any target, not compilable: it repeats
 * declarations of crgpu.h and crcore.h to show them changed. Every
 * declaration below is marked [ADDED], [CHANGED] or [REMOVED]; nothing
 * unmarked changes. Once approved it is frozen (ROADMAP.md, section 1,
 * "Work built from approved drafts").
 *
 * Read with card S51's draft (crgpu_format_capabilities_draft.h, the same
 * day): this one takes result codes 372 to 374; S49's approved draft holds
 * 371; S51's Q2 (b), if chosen, takes 375.
 *
 * ---------------------------------------------------------------------------
 * Revision notes (version 1 written, then seven passes, each fixed before
 * the next; one lens a pass, as the workspace's critique
 * prompts run them)
 * ---------------------------------------------------------------------------
 *
 * Pass 1 (wrong, missing, more than needed).
 *  1. Wrong: version 1 compared the row length in texels with the
 *     region's width. For a region that stops at a level's edge inside a
 *     block (a BC level 6 texels wide copied whole: 2 blocks) a row length
 *     of 4 is one block, too short, and 4 < 6 refuses it; but so would a
 *     row length of 6, which the block rule already refuses as not whole
 *     blocks. Stated in blocks now, as the card says ("in texels rounded to
 *     whole blocks"): row length / block width below the region's blocks
 *     across. Equal to Vulkan's texel comparison for every row length the
 *     block rule lets through (a multiple of the block's width is at least
 *     the width exactly when it is at least the width rounded up).
 *  2. Missing: whether a short image height is refused when only one slice
 *     of one layer is copied, where it changes no byte's place. Vulkan's
 *     VUID-VkBufferImageCopy-bufferImageHeight-09102 refuses it whatever
 *     the depth and layer count, so it is refused on every backend: one
 *     rule, no case.
 *  3. Missing: the D3D12 slice pitch's exact condition. D3D12's footprint
 *     has no slice pitch: a slice is RowPitch times the footprint's Height
 *     (crgpu_d3d12_make_footprint(), crgpud3d12renderpass.cpp line 323,
 *     Height the region's rows of blocks). Layers are placed one footprint
 *     each at layout.layer_bytes, so a 2D array's taller image height is
 *     laid out as Vulkan's and Metal's. Only a copy of depth above 1 with
 *     an image height above its height differs. That, alone, is refused.
 *  4. Found on the same path, a fault of the same kind (a layout that
 *     reaches the API wrong): a row pitch above 2^31 - 1 bytes. Vulkan
 *     refuses it (VUID-vkCmdCopyBufferToImage-bufferRowLength-09108: row
 *     length / block width x block bytes at most 2^31 - 1) and D3D12's
 *     footprint truncates it to a UINT ((UINT)layout->row_pitch,
 *     crgpud3d12renderpass.cpp line 333). Only a buffer over 2 GiB can
 *     reach it past the range check. Q3.
 *  5. Looked at for "more than needed": a code for each of three refusals
 *     is the card's decision, not this draft's; kept. Nothing else in the
 *     copies changes.
 *
 * Pass 2 (the platforms and industry practice).
 *  1. Vulkan: VUID-VkBufferImageCopy-bufferRowLength-09101 ("0, or greater
 *     than or equal to the width member of imageExtent") and
 *     -bufferImageHeight-09102 (the same for height), read in the SDK's
 *     validusage.json; -09106 and -09107 (multiples of the texel block's
 *     extent) are S48's block rule, kept.
 *  2. Metal: copyFromBuffer:...sourceBytesPerRow:sourceBytesPerImage: and
 *     copyFromTexture:...destinationBytesPerRow:destinationBytesPerImage:
 *     take byte strides; Metal's validation refuses a bytes per row below
 *     the region's row and a bytes per image below bytes per row times its
 *     rows (S48's build saw both, CRGPU_DEVICE_TODO.md, S48's "Found while
 *     building"). Metal lays a 3D copy's slices bytesPerImage apart, as
 *     Vulkan does bufferImageHeight rows apart.
 *  3. D3D12: D3D12_SUBRESOURCE_FOOTPRINT has Width, Height, Depth and
 *     RowPitch and no slice pitch; a row pitch below the footprint's row
 *     can't be expressed (RowPitch must hold Width's texels), so short
 *     rows have no D3D12 meaning at all. How D3D12 could lay out the taller
 *     slice (a footprint Height of the image height and a source box, or
 *     one copy per slice) is Q1 (c), not measured here.
 *  4. WebGPU: GPUTexelCopyBufferLayout's bytesPerRow and rowsPerImage;
 *     validation refuses bytesPerRow below the copy's row of blocks and
 *     rowsPerImage below its rows of blocks ("validating linear texture
 *     data"). WebGPU honours rowsPerImage for 3D copies on every backend,
 *     so its D3D12 implementations split such a copy themselves (Dawn by
 *     its copy-split code, as remembered of its source, not read on this
 *     machine): Q1 (c)'s kind of work, in a layer that chose to own it.
 *  5. Industry: wgpu (wgpu-core's linear texture data validation: the
 *     same two refusals, each its own TransferError, InvalidBytesPerRow
 *     and InvalidRowsPerImage); SDL_GPU (SDL_GPUTextureTransferInfo's
 *     pixels_per_row and rows_per_layer, 0 for tight); NVRHI (writeTexture
 *     takes a row and a depth pitch of the caller's CPU memory and repacks
 *     into its own staging, so no caller's buffer layout reaches an API);
 *     bgfx (updateTexture2D takes a pitch, updateTexture3D none: tight
 *     slices only); sokol_gfx (whole subimages, no region copy). As
 *     remembered of each project's public API; none is on this machine.
 *
 * Pass 3 (the C API, CONVENTIONS.md).
 *  1. Codes: the card decided each its own; CONVENTIONS.md, "Codes", is
 *     met since a caller can branch on them (a short row length is the
 *     caller's arithmetic, a taller slice a layout to change), and each is
 *     named for its field: CR_RESULT_GPU_COPY_ROW_LENGTH_TOO_SHORT (372),
 *     _COPY_IMAGE_HEIGHT_TOO_SHORT (373), _COPY_SLICE_PITCH_NOT_SUPPORTED
 *     (374), after S49's 371, in crcore.h's GPU block.
 *  2. The @retval lists of both copies are written whole, in the order the
 *     checks run, the new lines marked; DESC_INVALID_PARAMETERS's line
 *     keeps its list and gains "Q3's row pitch" in its place.
 *  3. The copy desc's comments on buffer_row_length and buffer_image_height
 *     say the rule for the caller ([CHANGED], comments only; the frozen
 *     struct's layout untouched).
 *
 * Pass 4 (no hidden policy).
 *  1. Nothing is clamped or rounded up: a short row length is refused, not
 *     widened to the region (which would be a choice of layout made for
 *     the caller). 0 keeps its one meaning, tight, as before.
 *  2. Q1 (a) refuses on every backend what two of them could do: the
 *     narrowest contract every API keeps (carrottus/CLAUDE.md, "one rule
 *     that holds on all of them"), not a per-backend answer. (b) is the
 *     per-backend answer the rule forbids for one input; (c) is emulation
 *     in a backend (one copy per slice), which carrottus/CLAUDE.md also
 *     names, as a second choice, and which Q1 leaves for a later card when
 *     a caller needs the layout.
 *
 * Pass 5 (threads and errors).
 *  1. No thread rule changes: the checks read the image's and buffer's
 *     descs, fixed at creation, and the caller's desc during the call.
 *  2. Every refusal is Carrottus's own check before anything is recorded,
 *     its own `if` and code, none joined; a refused copy records nothing
 *     and touches no platform call, so nothing reaches the instance's
 *     message callback.
 *  3. The arithmetic: the comparisons are in 32 bits of blocks (a row
 *     length / block width against blocks across, both at most 2^32 - 1),
 *     with no product, so nothing wraps; Q3's pitch is the existing 64-bit
 *     row_pitch.
 *
 * Pass 6 (the code as built; every file cited re-read, line numbers as of
 * 2026-11-18).
 *  1. crgpu___internal_check_buffer_image_copy(), crgpushared.c line 1158:
 *     its block rule for the two fields is line 1202, the offset check
 *     1205, the row pitch's 1212; blocks_wide and blocks_high are worked
 *     out after the layer check (lines 1222 and 1223) and move up to
 *     before the new checks (the values unchanged). The new checks go
 *     between 1202 and 1205.
 *  2. The checks' comment in crgpushared.h (lines 152 to 173) lists the
 *     order; it gains the three codes.
 *  3. D3D12: crgpu_d3d12_make_footprint(), crgpud3d12renderpass.cpp line
 *     323 (Height from region_blocks_high, line 331; RowPitch's cast, line
 *     333); its layer loops (lines 384 and 442) step layout.layer_bytes.
 *     Metal: bytes per image from layout.slice_bytes
 *     (crgpumetalrenderpass.m lines 208 and 248). Vulkan: bufferImageHeight
 *     passed as given (crgpuvulkanrenderpass.c line 300). So after the
 *     checks no backend changes.
 *  4. Callers (every repository of the workspace searched for the two
 *     fields): a caller's renderer sets buffer_image_height, for a 3D
 *     image, to the region's rows of blocks times the block's height:
 *     exactly the region's height, accepted; its row length comes from a
 *     pitch at least the tight row, accepted. crgpusamples'
 *     HelloTexture, CrGPUDynamicIndexing and CrGPUHeaps pass the image's
 *     height and a pitch-derived row length for 2D copies of the whole
 *     image: accepted. crgpu_objects_test's block copies pass 0 or a
 *     taller image height for 2D copies of depth 1 (line 2956): accepted;
 *     its region refusals (lines 3325 to 3360) keep their expected
 *     CR_RESULT_DESC_INVALID_PARAMETERS (each fails an earlier check).
 *  5. abi.txt: three constants added after CR_RESULT_GPU_IMAGE_TOO_LARGE
 *     (its line 623) and S49's 371; no line changes.
 *  6. Version 1's line numbers re-read against the files and corrected:
 *     crgpushared.c's block rule, offset, row pitch and blocks lines (each
 *     8 to 9 lines later than version 1 had them), the footprint's
 *     RowPitch (333), D3D12's layer loops (384, 442) and Metal's bytes
 *     per image (208, 248).
 *
 * Pass 7 (tests, records, the build).
 *  1. Each refusal in both directions (they share the check, but each
 *     copy's backend code is separate), each format kind (one texel, a BC
 *     block, a packed 2 x 1 block where the device has one), and the edge
 *     of a level inside a block.
 *  2. The accepted neighbours read back byte for byte: the equal row
 *     length, a longer one, 0; a taller image height on a 2D array of 3
 *     layers and on a 3D copy of depth 1 (the layouts D3D12 does keep).
 *  3. The order, with two fields wrong at once, and nothing recorded on a
 *     refusal (an image and a sentinel buffer read back unchanged, as S48's
 *     tests check).
 *  4. The misuse harness: no call changes shape; regenerated, expected
 *     unchanged. cr_noalloc_tests: unchanged, rerun.
 *  5. Wrong in version 1's tests: its accepted cases used tight rows
 *     (8 texels of RGBA8, 8 of BC7), which D3D12 refuses by its 256-byte
 *     row pitch alignment, and its packed-block case leaned on S51's COPY
 *     bit, not built. Accepted cases now use a pitch every backend takes,
 *     and the packed case is skipped where the image can't be made.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * A copy between a buffer and an image names the buffer's layout by a row
 * length and an image height, in texels, 0 meaning tight. S48's checks
 * hold both to whole blocks and the whole layout to the buffer's range,
 * but not to the region: a row length above 0 and below the region's
 * width, or an image height below its height, passes and reaches the API
 * with rows or slices that overlap. Vulkan forbids both
 * (VUID-VkBufferImageCopy-bufferRowLength-09101, -bufferImageHeight-09102),
 * Metal's validation refuses both (and ends the process with validation
 * on), and D3D12 has no way to say either: its footprint's RowPitch holds
 * at least a row, so the backend hands D3D12 a footprint that doesn't
 * describe the caller's bytes. Three backends, three outcomes, for one
 * input.
 *
 * And one layout that every check passes is laid out differently: a copy
 * of depth above 1 whose image height is above its height. Vulkan and
 * Metal step each slice the image height's rows of blocks; D3D12's
 * footprint has no slice pitch, so its slices are the region's rows apart,
 * and the copy moves bytes from the wrong places with no error anywhere.
 *
 * ---------------------------------------------------------------------------
 * What changes, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * crgpu_command_list_copy_buffer_to_image() and _copy_image_to_buffer()
 * refuse, after the block's rules and before the offset, a
 * buffer_row_length above 0 whose blocks are fewer than the region's
 * blocks across (CR_RESULT_GPU_COPY_ROW_LENGTH_TOO_SHORT, 372), a
 * buffer_image_height above 0 whose rows of blocks are fewer than the
 * region's (CR_RESULT_GPU_COPY_IMAGE_HEIGHT_TOO_SHORT, 373), and a copy of
 * depth above 1 whose buffer_image_height is above its height
 * (CR_RESULT_GPU_COPY_SLICE_PITCH_NOT_SUPPORTED, 374), on every backend
 * (Q1 a), in every build; and (Q3 a) a row pitch above 2^31 - 1 bytes
 * (CR_RESULT_DESC_INVALID_PARAMETERS). Nothing else about the copies
 * changes, and no backend's translation does.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 *  - Vulkan (the SDK 1.4.363.0's validusage.json): the four VUIDs in pass
 *    2, 1 and -09108 (pass 1, 4). VkBufferImageCopy::bufferImageHeight
 *    steps both a 3D image's slices and an array's layers.
 *  - D3D12 (Microsoft's documentation of D3D12_SUBRESOURCE_FOOTPRINT and
 *    CopyTextureRegion; d3dx12.h's CD3DX12_TEXTURE_COPY_LOCATION, in the
 *    backend): no slice pitch; one footprint per subresource; RowPitch a
 *    multiple of 256 and a UINT; Offset a multiple of 512.
 *  - Metal (MTLBlitCommandEncoder.h, macOS 27 SDK): byte strides per row
 *    and per image, the latter for 3D textures (and 2D arrays, per slice).
 *  - WebGPU and industry practice: pass 2, 4 and 5.
 *
 * Hazel is not compared: Carrottus is not measured against an engine (the
 * workspace's CLAUDE.md, "External references").
 *
 * ---------------------------------------------------------------------------
 * What the caller decides, and what Carrottus GPU only reports
 * ---------------------------------------------------------------------------
 *
 * The caller decides the buffer's layout: tight (0), or rows and slices at
 * least the region's, padded as it likes, with a 3D copy's slices tight in
 * rows. Carrottus GPU refuses what it can't lay out the same way on every
 * backend and never widens, clamps or reorders a layout.
 *
 * ---------------------------------------------------------------------------
 * Who else uses it
 * ---------------------------------------------------------------------------
 *
 *  - A caller's renderer's image uploads and readbacks, whose layout
 *    already meets every rule (pass 6, 4).
 *  - A caller streaming a sub-region of a larger CPU image: rows padded to
 *    the CPU image's width (a row length above the region's), which stays
 *    accepted.
 *  - A volume (a 3D noise or density texture) uploaded slice-padded: the
 *    one layout refused, until Q1 (c)'s card; it pads rows instead, or
 *    copies one slice at a time (a copy of depth 1 takes any image
 *    height).
 *
 * ---------------------------------------------------------------------------
 * Left out, and why
 * ---------------------------------------------------------------------------
 *
 *  - Laying out the taller slice on D3D12: Q1 (c), a later card.
 *  - The combined depth-stencil formats' copies (one aspect per copy on
 *    every API; crgpu_image_buffer_copy_desc names none): found by S51's
 *    draft (its fault 2), carded there.
 *  - Image-to-image copies and blits: other calls, other checks.
 *
 * ---------------------------------------------------------------------------
 * The version and the breaks
 * ---------------------------------------------------------------------------
 *
 *  - crcore.h: three values added (372, 373, 374); abi.txt gains three
 *    constants.
 *  - Both copies refuse what they recorded before, a behaviour break,
 *    listed: a short row length or image height (each reached the API
 *    outside its rules) and a 3D copy of depth above 1 with a taller image
 *    height (taken by Vulkan and Metal, laid out wrong by D3D12). Searched
 *    (pass 6, 4): no caller in the workspace passes any of them.
 *  - Q3 (a): a row pitch above 2^31 - 1 bytes refused with
 *    CR_RESULT_DESC_INVALID_PARAMETERS (reached past the range check only
 *    with a buffer over 2 GiB).
 *  - No version raise: carrottus' 0.4.0 is untagged (card X2).
 *    ./check.sh --update-abi lists the three constants; the misuse
 *    generator is rerun.
 *
 * New handles: none. New struct types: none. Struct layouts: unchanged.
 * Stored formats touched: none. Thread: nothing changes. Allocation: none
 * added.
 *
 * ---------------------------------------------------------------------------
 * Questions (each with options and a recommendation)
 * ---------------------------------------------------------------------------
 *
 * Q1. THE TALLER SLICE PITCH (depth above 1, image height above height).
 *     (a) RECOMMENDED. Refused on every backend with
 *         CR_RESULT_GPU_COPY_SLICE_PITCH_NOT_SUPPORTED: one rule the
 *         header states and every backend keeps, lifted for all three at
 *         once when D3D12 can lay it out. Costs Vulkan and Metal a layout
 *         they could take; a caller pads rows or copies a slice at a time.
 *     (b) Refused on D3D12 only, the same code; Vulkan and Metal take it.
 *         One input, two answers by backend, which carrottus/CLAUDE.md's
 *         "same value for the same condition in every backend" rules out
 *         except for a capability a backend lacks, and this one has no
 *         query to say so.
 *     (c) D3D12 lays it out now: a footprint whose Height is the image
 *         height's rows of blocks and a source box of the region (both
 *         directions), with the range check counting the last slice's
 *         whole image height on every backend; or one copy per slice
 *         where each slice's start meets the 512-byte offset alignment.
 *         Not measured: whether D3D12 accepts a footprint taller than the
 *         copied box and where its debug layer bounds the footprint
 *         against the buffer. A card of its own, measured on Windows.
 *
 * Q2. WHERE THE THREE CHECKS RUN IN THE ORDER.
 *     (a) RECOMMENDED. After the block's rules (which hold the same two
 *         fields to whole blocks) and before buffer_offset's alignment:
 *         every existing refusal keeps its place, and a field's whole-
 *         block refusal comes before its shortness, which is measured in
 *         blocks.
 *     (b) In the desc's field order, before the region checks
 *         (buffer_row_length and buffer_image_height are its second and
 *         third fields): moves every existing region refusal behind them,
 *         a second order change for no caller's gain.
 *
 * Q3. A ROW PITCH ABOVE 2^31 - 1 BYTES.
 *     (a) RECOMMENDED. Refused now on every backend with
 *         CR_RESULT_DESC_INVALID_PARAMETERS, after the row pitch's
 *         alignment: Vulkan's bound, and D3D12's UINT truncation, on the
 *         same path; the general code, since no caller branches on a row
 *         of 2 GiB.
 *     (b) A card of its own.
 *
 * The three codes and their numbers are the card's decision ("each its own
 * code"), not a question; their names follow pass 3.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * crgpu_objects_test, every backend, both copies (buffer to image and
 * image to buffer), each refusal with the image read back unchanged and a
 * sentinel-filled buffer unchanged (nothing recorded):
 *  - Every accepted case below uses a row length whose pitch meets
 *    crgpu_image_get_buffer_copy_row_pitch_alignment(): on D3D12 a pitch
 *    of 256 (64 texels of R8G8B8A8_UNORM, 64 of BC7_UNORM); on Vulkan and
 *    Metal also 0 and the tight row. A refused case's pitch may miss it:
 *    the new checks run first (Q2 a), so the same code comes back on
 *    every backend.
 *  - One texel: an R8G8B8A8_UNORM 2D image 8 x 8. Row length 4 for the
 *    whole width: CR_RESULT_GPU_COPY_ROW_LENGTH_TOO_SHORT; 7:
 *    the same; 8 and 0 (Vulkan, Metal) and 64: accepted, round trip byte
 *    for byte. Image height 4 (one
 *    layer, depth 1): CR_RESULT_GPU_COPY_IMAGE_HEIGHT_TOO_SHORT; 8, 0 and
 *    12: accepted.
 *  - A BC block (CRGPU_FEATURE_TEXTURE_COMPRESSION_BC where the adapter
 *    has it): BC7_UNORM 12 x 12 with 2 levels; level 1 is 6 x 6, two
 *    blocks each way, the second partly past the edge. Whole level, row
 *    length 4: ROW_LENGTH_TOO_SHORT; row length 6: DESC_INVALID_PARAMETERS
 *    (the block rule, first: Q2 a); 8 (Vulkan, Metal) and 64 accepted.
 *    Image height 4: IMAGE_HEIGHT_TOO_SHORT; 8 accepted. The region x 4,
 *    width 2 (the edge block alone) with row length 4 (Vulkan, Metal) or
 *    64: accepted (one block across).
 *  - A packed 2 x 1 block, where an image of G8R8_G8B8_UNORM can be made
 *    with the transfer usages (none on Metal, which maps no such format;
 *    skipped, and said so, where its creation is refused): width 4, row
 *    length 2: ROW_LENGTH_TOO_SHORT.
 *  - The slice pitch: an R8G8B8A8_UNORM 3D image 4 x 4 x 4. Depth 4,
 *    image height 8: CR_RESULT_GPU_COPY_SLICE_PITCH_NOT_SUPPORTED; image
 *    height 4 and 0: accepted, round trip byte for byte. Depth 1 at
 *    image_z 2, image height 8: accepted, round trip (D3D12's one slice).
 *    A 2D array of 3 layers 4 x 4, image height 8, every layer: accepted
 *    and read back byte for byte from its padded layout (each layer at
 *    its own offset, the layer step 128 bytes; on D3D12 with a row length
 *    of 64 texels so the pitch is 256 and each layer 2048, a multiple of
 *    512).
 *  - Q3 (a): where a DEVICE buffer of 2^31 + 256 bytes can be made (the
 *    test skips, and says so, where it can't), a copy of one row whose
 *    row length makes the pitch 2^31 bytes: DESC_INVALID_PARAMETERS, the
 *    buffer never written.
 *  - The order, two fields wrong: a short row length with a misaligned
 *    buffer_offset gives ROW_LENGTH_TOO_SHORT (before the offset); with
 *    the image's transfer usage missing, the usage's code (before the
 *    region); a short row length and a short image height,
 *    ROW_LENGTH_TOO_SHORT; a short image height on a 3D copy of depth 4,
 *    IMAGE_HEIGHT_TOO_SHORT (the slice pitch check sees only taller ones).
 *  - The existing region refusals (crgpu_objects_test.c lines 3325 to
 *    3360) keep CR_RESULT_DESC_INVALID_PARAMETERS, rerun unchanged.
 *  - A caller's GPU tests (the renderer above): its uploads and readbacks
 *    unchanged (pass 6, 4), rerun on every adapter.
 *  - The misuse harness: regenerated, expected unchanged.
 *  - Where each runs: the Mac (Apple M5 Pro; macos-metal Debug and
 *    Release under Metal API and shader validation with warnings fatal,
 *    -asan, -tsan). Vulkan's and D3D12's backends are not changed, but the
 *    checks are shared: owed on Windows 11 (Iris Xe, RTX A2000 and WARP
 *    over D3D12 under the debug layer and GPU-based validation; the Iris
 *    Xe and the A2000 over Vulkan under validation and synchronization
 *    validation) and Fedora (ANV, NVK, RADV).
 *  - ./check.sh: abi, doxygen, conventions, caller-names.
 *
 * ---------------------------------------------------------------------------
 * crgpu.h
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Describes a rectangular texel region and target mip/layer used for a
 * copy between an image and a buffer.
 *
 * ... unchanged to the list of rules ...
 *
 * For every format, the rules each backend's API needs, stated once and
 * checked by both copies:
 *  - `array_layer_count` and `depth` are at least 1;
 *  - [ADDED] `buffer_row_length`, when not 0, is at least the region's
 *    width in whole blocks (ceil(width / block width) blocks), and
 *    `buffer_image_height`, when not 0, at least its height in whole rows of
 *    blocks: a buffer's rows and images never overlap;
 *  - [ADDED] with `depth` above 1, `buffer_image_height` is 0 or the
 *    region's height: a 3D copy's slices follow each other with no rows
 *    between them, the one layout every backend lays out the same way
 *    (D3D12 has no slice pitch of its own). A 2D array's layers, and a 3D
 *    copy of one slice, take any image height at least the region's;
 *  - the row pitch the copy implies is a multiple of
 *    crgpu_image_get_buffer_copy_row_pitch_alignment(), [CHANGED] and at
 *    most 2^31 - 1 bytes (Q3 a), and the pitch
 *    crgpu_image_get_buffer_copy_size() gives always is;
 *  - ... the layer and range rules unchanged.
 */
/* Frozen: a copy region, passed in arrays. It never changes; a new need gets a new type. */
typedef struct crgpu_image_buffer_copy_desc {
	/* ... buffer_offset unchanged ... */

	/**
	 * [CHANGED] (comment only) Row length in texels, used to interpret the
	 * buffer's layout. Pass 0 for tightly packed rows (== region width,
	 * rounded up to whole blocks for a block format); otherwise a multiple
	 * of the block's width, at least the region's width rounded up to whole
	 * blocks.
	 */
	uint32_t buffer_row_length;

	/**
	 * [CHANGED] (comment only) Image height in texels, used to interpret
	 * the buffer's 2D/3D layout: the rows from one layer's start (or one 3D
	 * slice's) to the next. Pass 0 for tightly packed rows (== region
	 * height, rounded up to whole blocks for a block format); otherwise a
	 * multiple of the block's height, at least the region's height rounded
	 * up to whole blocks, and with `depth` above 1 exactly the region's
	 * height.
	 */
	uint32_t buffer_image_height;

	/* ... every other member unchanged ... */
} crgpu_image_buffer_copy_desc;

/**
 * @brief Records a copy from an image subresource region into a buffer.
 *
 * ... the description unchanged ...
 *
 * [CHANGED] its results, in the order checked (three lines added, one
 * grown; every other line as today):
 *
 * @retval CR_SUCCESS The copy was recorded.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST `cmd` is NULL.
 * @retval CR_RESULT_GPU_INVALID_IMAGE `src_image` is NULL.
 * @retval CR_RESULT_GPU_INVALID_BUFFER `dst_buffer` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_GPU_INVALID_FORMAT The image's format has no texel
 *         block (crgpu_format_get_texel_block()), so no copy of it has a
 *         known layout.
 * @retval CR_RESULT_GPU_INVALID_IMAGE_USAGE `src_image` wasn't created with
 *         CRGPU_IMAGE_USAGE_TRANSFER_SRC.
 * @retval CR_RESULT_GPU_INVALID_BUFFER_USAGE `dst_buffer` wasn't created
 *         with CRGPU_BUFFER_USAGE_TRANSFER_DST.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS The region isn't within the
 *         image: `mip_level` past its levels, the layers past its layers or
 *         `array_layer_count` 0, a size of 0 (`depth` included), or the
 *         region past the level's width, height or depth; or it breaks the
 *         block's rules (crgpu_image_buffer_copy_desc). Checked in that
 *         order, each its own check.
 * @retval CR_RESULT_GPU_COPY_ROW_LENGTH_TOO_SHORT [ADDED]
 *         `desc->buffer_row_length` is not 0 and is fewer blocks than the
 *         region's width in whole blocks. Since 0.4.0.
 * @retval CR_RESULT_GPU_COPY_IMAGE_HEIGHT_TOO_SHORT [ADDED]
 *         `desc->buffer_image_height` is not 0 and is fewer rows of blocks
 *         than the region's height in whole blocks. Since 0.4.0.
 * @retval CR_RESULT_GPU_COPY_SLICE_PITCH_NOT_SUPPORTED [ADDED]
 *         `desc->depth` is above 1 and `desc->buffer_image_height` is above
 *         `desc->height`: slices with rows between them, which not every
 *         backend can lay out. Since 0.4.0.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS [CHANGED] (then, as before)
 *         `buffer_offset` isn't a multiple of the copy offset alignment; or
 *         the row pitch isn't a multiple of the row pitch alignment, or is
 *         above 2^31 - 1 bytes (Q3 a); or a later layer's start isn't a
 *         multiple of the offset alignment; or the bytes the region covers
 *         run past the buffer's size. Checked in that order, each its own
 *         check.
 */
CR_RESULT crgpu_command_list_copy_image_to_buffer(
    crgpu_command_list cmd,
    crgpu_image src_image,
    crgpu_buffer dst_buffer,
    const crgpu_image_buffer_copy_desc *desc);

/**
 * @brief Records a copy of buffer bytes into an image subresource region.
 *
 * [CHANGED] (comment only) @return As crgpu_command_list_copy_image_to_buffer(),
 * the three new results included in the same place, with
 * CR_RESULT_GPU_INVALID_BUFFER for `src_buffer`, CR_RESULT_GPU_INVALID_IMAGE
 * for `dst_image`, and the usages CRGPU_BUFFER_USAGE_TRANSFER_SRC on
 * `src_buffer` and CRGPU_IMAGE_USAGE_TRANSFER_DST on `dst_image`. ... the
 * rest unchanged.
 */
CR_RESULT crgpu_command_list_copy_buffer_to_image(
    crgpu_command_list cmd,
    crgpu_buffer src_buffer,
    crgpu_image dst_image,
    const crgpu_image_buffer_copy_desc *desc);

/*
 * ---------------------------------------------------------------------------
 * crcore.h
 * ---------------------------------------------------------------------------
 *
 * [ADDED], after CR_RESULT_GPU_IMAGE_TOO_LARGE (370) and S49's
 * CR_RESULT_GPU_SAMPLER_ANISOTROPY_TOO_LARGE (371). Why: card S52, each
 * refusal its own code. ABI: three values appended. Tests:
 * crgpu_objects_test.
 *
 *	/** A buffer-image copy's buffer_row_length is not 0 and is shorter than
 *	 * the copied region's width in whole blocks: its rows would overlap.
 *	 * Since 0.4.0. * /
 *	CR_RESULT_GPU_COPY_ROW_LENGTH_TOO_SHORT                   = 372,
 *	/** A buffer-image copy's buffer_image_height is not 0 and is shorter
 *	 * than the copied region's height in whole blocks: its images would
 *	 * overlap. Since 0.4.0. * /
 *	CR_RESULT_GPU_COPY_IMAGE_HEIGHT_TOO_SHORT                 = 373,
 *	/** A buffer-image copy of depth above 1 has a buffer_image_height above
 *	 * its height: a slice pitch not every backend lays out (D3D12 has none
 *	 * of its own). Since 0.4.0. * /
 *	CR_RESULT_GPU_COPY_SLICE_PITCH_NOT_SUPPORTED              = 374,
 *
 * cr_result_str() (crcore.c) names the three.
 *
 * ---------------------------------------------------------------------------
 * Where it is done (not in a header)
 * ---------------------------------------------------------------------------
 *
 * - src/gpu/crgpushared.c, crgpu___internal_check_buffer_image_copy(),
 *   after the block rule for the two fields (line 1202) and before the
 *   offset (line 1205), blocks_wide and blocks_high worked out first:
 *
 *	if (desc->buffer_row_length != 0 && desc->buffer_row_length / block.width < blocks_wide)
 *		return CR_RESULT_GPU_COPY_ROW_LENGTH_TOO_SHORT;
 *	if (desc->buffer_image_height != 0 && desc->buffer_image_height / block.height < blocks_high)
 *		return CR_RESULT_GPU_COPY_IMAGE_HEIGHT_TOO_SHORT;
 *	if (desc->depth > 1 && desc->buffer_image_height > desc->height)
 *		return CR_RESULT_GPU_COPY_SLICE_PITCH_NOT_SUPPORTED;
 *
 *   (A block taller than one texel is 2D only, depth 1, so the last
 *   comparison is in texels and rows of blocks at once.) After the row
 *   pitch's alignment: `if (row_pitch > INT32_MAX) return
 *   CR_RESULT_DESC_INVALID_PARAMETERS;` (Q3 a). Its comment in
 *   crgpushared.h lists the new checks in order. Every backend calls it;
 *   none changes otherwise.
 *
 * ---------------------------------------------------------------------------
 * Records when built
 * ---------------------------------------------------------------------------
 *
 * carrottus/CLAUDE.md, "Block-compressed formats" bullet on the copies'
 * checks: the three refusals and the 3D slice rule. CRGPU_DEVICE_TODO.md's
 * S52 section: built, each machine's run; S48's "Found while building"
 * pointed at it. The workspace's DECISIONS.md card S52: the draft approved
 * and built; Q1 (c)'s card if (a) is chosen.
 */
