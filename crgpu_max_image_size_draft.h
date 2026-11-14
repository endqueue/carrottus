/*
 * crgpu_max_image_size_draft.h - the largest image an adapter creates, read
 * from the graphics API and reported in CRGPU_ADAPTER_PROPERTIES (card S33,
 * decided 2026-11-02 with the user: a; CRGPU_DEVICE_TODO.md, "The largest
 * 2D texture").
 *
 * APPROVED by the user 2026-11-14, version 7. Written unattended
 * on the Mac (Apple M5 Pro), the workspace's session prompt of 2026-11-14, part 4c;
 * revised by the workspace's review prompt of 2026-11-14, whose record has
 * each finding (C<cycle>.<n>).
 *  - Version 2 (stage A): Q1-Q4 answered with the user, each (a) as
 *    recommended; crcore.h's note of what Q1 (b) would have added is
 *    removed (the option stays in Q1 for the record).
 *  - Version 3 (cycle 1, the library's rules): "What reaches a caller
 *    above" named a caller; it says "the caller" (C1.8).
 *  - Version 4 (cycle 2, the platforms): each field an upper bound, a
 *    format allowing less on Vulkan (C2.2); Metal's refusal past the
 *    limits is an abort, put to the user (C2.1); the DirectX-Headers'
 *    path (C2.3); Metal 3's Mac2 GPUs (C2.4); a cube-capable image's layers
 *    measured (C2.5); the Why's Vulkan example (C2.6).
 *  - Version 5 (cycle 5, versions): what a caller built against the old
 *    layout meets, carrottus being static only (C5.4).
 *  - Version 6 (cycle 7, tests): every machine and driver the test runs on
 *    named, the iOS simulator's skip (C7.6).
 *  - Version 7 (the user's answer to C2.1, 2026-11-14): crgpu_image_create
 *    and crgpu_image_create_placed refuse an image past these limits on
 *    every backend, with a new CR_RESULT_GPU_IMAGE_TOO_LARGE (370), so a
 *    caller's size never reaches Metal's assertion.
 * Cycles 3, 4 and 6 changed only the TODO or found nothing to change.
 * Approved as version 7; frozen. Its SHA-256 is in CRGPU_DEVICE_TODO.md,
 * under S33 (card X23).
 * Not compilable: it repeats CRGPU_ADAPTER_PROPERTIES from crgpu.h to show
 * it changed. Every declaration below is marked [ADDED], [CHANGED] or
 * [REMOVED]; nothing unmarked changes. Nothing is built until it is
 * approved; once approved it is frozen (ROADMAP.md, section 1, "Work built
 * from approved drafts").
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * A caller sizing an image it makes from its own numbers (an atlas of
 * shadow maps whose tiles it packs, a texture array of layers it chooses,
 * a render target at a multiple of a window) needs to know, before it
 * creates the image, the largest one the adapter takes. Today nothing in
 * Carrottus GPU says it: the caller guesses (16384 is wrong on an Apple10
 * GPU, which takes 32768, and on a Vulkan driver at the required minimum,
 * which may report 4096; C2.6), or finds out from a failed create, which
 * on Metal is no failure but an abort of the process (C2.1, below). Each
 * graphics API reports the limit as a fact of the adapter; this draft
 * passes it through. A translation, no policy: the caller decides what to
 * do with it. [CHANGED by the user's answer to C2.1, 2026-11-14: version 1
 * said "Carrottus GPU compares nothing against it (the create calls'
 * checks don't change)".] The image creates check a size against it before
 * the graphics API sees it, as they check every other field, since Metal
 * ends the process on one past it: a fact the platform states, checked so
 * bad input never reaches a driver, not a choice made for the caller.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * The three APIs, each read on this machine tonight, never from memory:
 *
 *  - Vulkan: VkPhysicalDeviceLimits' maxImageDimension1D, -2D, -3D,
 *    maxImageDimensionCube and maxImageArrayLayers (vulkan_core.h, Vulkan
 *    SDK 1.4.363.0, lines 3585-3589), per physical device. The required
 *    minimums, from the SDK's VP_LUNARG_minimum_requirements.json
 *    ("vulkan10requirements", which Vulkan 1.3, Carrottus' floor, keeps):
 *    1D, 2D and cube 4096, 3D 256, array layers 256. Vulkan 1.4
 *    ("vulkan14requirements") raises them to 8192, 8192, 512, 8192 and 2048.
 *  - D3D12: no query; fixed for feature level 11_0 and up (Carrottus lists
 *    12_0 and up), from d3d12.h (DirectX-Headers, the copy at
 *    .refs/HazelRenderer2025/Hazel/Hazel/vendor/nvrhi/thirdparty/
 *    DirectX-Headers/include/directx/d3d12.h, lines 1294-1304, read again
 *    2026-11-14; C2.3):
 *    D3D12_REQ_TEXTURE1D_U_DIMENSION 16384,
 *    D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION 16384,
 *    D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION 2048,
 *    D3D12_REQ_TEXTURECUBE_DIMENSION 16384,
 *    D3D12_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION and
 *    D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION 2048.
 *  - Metal: no query; by the GPU's family, from Apple's "Metal Feature Set
 *    Tables" (developer.apple.com/metal/Metal-Feature-Set-Tables.pdf, the
 *    edition dated October 23, 2026, page 8, "Resources"), its columns
 *    Apple2 to Apple11 (the table has no Mac2 column in this edition):
 *      "Maximum 1D texture width"                    Apple2 8192 px;
 *                                                    Apple3-Apple9 16,384;
 *                                                    Apple10, Apple11 32,768
 *      "Maximum 2D texture width and height"         the same
 *      "Maximum cube map texture width and height"   the same
 *      "Maximum 3D texture width, height, and depth" 2048 in every family
 *      "Maximum number of layers per 1D texture array, 2D texture array,
 *       or 3D texture array"                         2048 in every family
 *    Carrottus' Metal backend lists only GPUs in MTLGPUFamilyMetal3
 *    (crgpumetalcontext.m, crgpumetal___internal_list_adapters): every
 *    Apple GPU from Apple7, and the Mac2-class GPUs of an Intel Mac that
 *    reach Metal 3 (C2.4). The values it reports are Apple7-Apple9's and
 *    Apple10-Apple11's; a Mac2 GPU takes 16384, as Apple7's [unconfirmed
 *    (C2.4): this edition has no Mac2 column, and MTLGPUFamilyMac2 is
 *    deprecated in the macOS 27 SDK (MTLDevice.h, "replaced by
 *    MTLGPUFamilyApple7"); no Intel Mac was at hand]. Page 2 of the same
 *    edition places the M5 series (this Mac) in Apple10.
 *    Measured on this Mac (C2.1, C2.5; Apple M5 Pro, macOS 27, a scratch
 *    program, 2026-11-14): supportsFamily answers yes to Metal3, Apple7,
 *    Apple9 and Apple10, no to Apple11 (and yes to the deprecated Mac2).
 *    newTextureWithDescriptor made, in R8Unorm and private storage, a 1D
 *    texture 32768 wide, 2D ones 32768 wide and 32768 tall, a 3D one 2048
 *    wide and 2048 deep, 1D and 2D arrays of 2048 layers, a cube array of
 *    341 cubes (2046 layers), and sized a 32768 cube; one texel or layer
 *    past each (a cube array of 342: 2052 layers) was refused by Metal's
 *    descriptor validation with an assertion that ended the process (exit
 *    134), with Metal's API validation off as well as on.
 *  - Industry practice: WebGPU's GPUSupportedLimits (maxTextureDimension1D,
 *    -2D, -3D and maxTextureArrayLayers, per adapter) and wgpu's Limits
 *    (the same four) report the size per dimension and the layers apart;
 *    neither folds them into one number. WebGPU has no cube limit of its
 *    own; Vulkan and D3D12 do.
 *
 * ---------------------------------------------------------------------------
 * The version and the breaks
 * ---------------------------------------------------------------------------
 *
 * CRGPU_ADAPTER_PROPERTIES gains fields at its end (question 1): its layout
 * changes, a listed break against carrottus/abi.txt before 1.0
 * (CARROTTUS_VERSION 0.3.0 to 0.4.0, a minor for the break; the workspace's
 * CLAUDE.md, "Versions and releases"). A caller that rebuilds against the
 * new header is unaffected. carrottus is static only (the workspace's
 * "Versions and releases"), so a caller is always compiled against the
 * header of the library it links; only an object file left from before a
 * header change and linked with the new library would pass a struct the
 * library writes past, which a clean build, and the dep locks' rebuild,
 * never does (C5.4). No call or enum changes; one result is added,
 * CR_RESULT_GPU_IMAGE_TOO_LARGE (370), and crgpu_image_create and
 * crgpu_image_create_placed return it (below). A caller that made an image
 * past the limits got an abort on Metal, undefined behaviour on Vulkan and
 * E_INVALIDARG on D3D12; every backend now refuses it the same way.
 *
 * Thread: no new rule. crgpu_adapter_get_properties()'s line stands (it
 * may overlap any call on the adapter or its instance but
 * crgpu_instance_destroy()); the values are read when the instance lists
 * its adapters, as every other property is.
 *
 * New handles: none. Formats touched: none (no file, no stored layout).
 *
 * What reaches a caller above: five numbers it reads from the adapter's
 * properties and compares its own sizes against, before creating. The
 * caller passes them on or checks against them as it chooses; nothing here
 * says who does (C1.8).
 *
 * ANSWERED 2026-11-14 with the user (C2.1): on Metal an image past these
 * limits wasn't refused but ended the process, inside crgpu_image_create
 * (measured above), against "bad input never reaches a driver"
 * (CONVENTIONS.md, "Data handling") and "never an abort on the caller's
 * input" ("Error handling"). The creates refuse it now, on every backend,
 * with a result of its own (crgpu.h's and crcore.h's sections below).
 *
 * ---------------------------------------------------------------------------
 * Questions (numbered; each with options and a recommendation)
 * ---------------------------------------------------------------------------
 *
 * Q1. WHERE IT GOES.
 *     ANSWERED 2026-11-14 with the user: (a), as recommended; no change
 *     to the option.
 *     (a) RECOMMENDED. Fields at the end of CRGPU_ADAPTER_PROPERTIES. The
 *         struct's comment already says what belongs where: "Limits that
 *         only matter with a feature are in structs chained onto
 *         base.next" (mesh shading, bindless, ray queries). These matter on
 *         every adapter with no feature, as max_draw_indirect_count,
 *         max_descriptors and max_samplers do, which sit in the struct
 *         itself. The struct-base rule ("a released layout never changes: a
 *         new field is a chained struct") binds from 1.0; before it, a field
 *         at the end is allowed as a listed break (CONVENTIONS.md, "Struct
 *         bases"), and a break now costs less than a chain every caller
 *         must build for a limit every caller wants.
 *     (b) A chained CRGPU_IMAGE_LIMITS (CR_STRUCT_BASE_OUT, its type
 *         registered as CR_STRUCT_TYPE_GPU_IMAGE_LIMITS = 1031). No break,
 *         but a feature-less limit outside the struct, against the
 *         struct's own rule, and a second place a caller looks for the
 *         same kind of fact.
 *
 * Q2. THE OTHER DIMENSIONS.
 *     ANSWERED 2026-11-14 with the user: (a), as recommended; no change
 *     to the option.
 *     (a) RECOMMENDED. All five at once: 1D, 2D, 3D, cube and array
 *         layers. Each API reports each of them (above), each is a single
 *         read in each backend, and the next caller's needs are already in
 *         sight: a cube shadow map's face size (the cube limit, which
 *         Vulkan reports apart from 2D), a 3D colour lookup or a volume
 *         (2048 on D3D12 and Metal against 2D's 16384), a texture array's
 *         layers (2048, the bound a caller's atlas of layers meets first).
 *         Before 1.0 each field added later is a break of its own; five now
 *         are one.
 *     (b) Only the 2D size S33 named. The smallest change, and the cube,
 *         3D and layer limits a break each when a caller asks.
 *
 * Q3. METAL'S VALUES.
 *     ANSWERED 2026-11-14 with the user: (a), as recommended; no change
 *     to the option.
 *     (a) RECOMMENDED. By the device's family, read when the instance lists
 *         the adapter: [device supportsFamily:MTLGPUFamilyApple10] (and
 *         Apple11, the same values) gives 32768 for 1D, 2D and cube; any
 *         other listed GPU (Apple7-Apple9) 16384; 3D 2048 and layers 2048
 *         for every one. The values and their source as above, kept beside
 *         the read in the backend with the table's edition and page. A
 *         family the table adds later (Apple12) takes the highest known
 *         family's values until the table is read again and the backend
 *         updated; the test (below) reads every adapter, so a new GPU is
 *         seen at least to be no smaller than Apple7's.
 *     (b) Apple7's values (16384) for every GPU: true on all, but an
 *         Apple10 GPU's 32768 never reported, which is a fact withheld.
 *
 * Q4. THE TESTS.
 *     ANSWERED 2026-11-14 with the user: (a), as recommended; no change
 *     to the option.
 *     crgpu_device_test, on every adapter of each backend, reads the five
 *     fields and checks each against the API's guaranteed minimum:
 *       Vulkan (1.3): 1D, 2D, cube >= 4096; 3D >= 256; layers >= 256;
 *       D3D12: exactly 16384, 16384, 2048, 16384, 2048;
 *       Metal: 1D, 2D, cube 16384 or 32768, 32768 exactly when the device
 *       is in MTLGPUFamilyApple10; 3D and layers 2048.
 *     And on every adapter: 2D >= 1D is not assumed (no API promises it);
 *     each is non-zero. [ADDED by the answer to C2.1] crgpu_objects_test,
 *     on every adapter: crgpu_image_create and crgpu_image_create_placed
 *     refuse an image one past each limit (a 1D width, a 2D width and a 2D
 *     height, a 3D width, height and depth, a cube's width, the layers;
 *     each on its own) with CR_RESULT_GPU_IMAGE_TOO_LARGE, before any memory
 *     is asked for, so the test needs none; and a small image of every type
 *     still made. On the Mac this is the run that shows Metal's abort is
 *     never reached. The misuse harness needs no new call; its generator is
 *     rerun for the new result. Run where each backend runs (C7.6): the Mac (Apple10;
 *     plain, -asan, -tsan); Windows' Iris Xe, RTX A2000 and WARP over
 *     D3D12, and the Iris Xe and the A2000 over Vulkan (Debug, Release,
 *     -asan); Linux over Vulkan on the ThinkPad (ANV, NVK, llvmpipe) and the
 *     Fedora desktop (RADV on the RX 7800 XT). Each Vulkan driver's own
 *     five numbers are recorded in CRGPU_DEVICE_TODO.md. The iOS simulator
 *     lists no adapter (its GPU isn't Metal 3), so the test is skipped
 *     there (exit 77), as today; no device run is planned for it.
 *     (a) RECOMMENDED as above.
 *     (b) Also a create of an image at the 2D limit on each adapter: it
 *         shows the number is real, but takes 1 GiB at 16384x16384 RGBA8
 *         (4 GiB at 32768), which a CI machine may refuse for memory, not
 *         for size; a create one texel past the limit isn't refused by
 *         every driver (Vulkan's validation layer reports it, the driver
 *         may take it). Not recommended.
 *
 * ---------------------------------------------------------------------------
 * crgpu.h
 * ---------------------------------------------------------------------------
 */

/* [CHANGED] Five fields at the end (Q1 a, Q2 a). Everything above them is
 * as crgpu.h has it; the fields are shown alone. */
typedef struct CRGPU_ADAPTER_PROPERTIES {
	/* ... every field as before, up to queue_pool_count ... */

	/*
	 * Every field below is an upper bound for its kind of image, whatever
	 * its format, usage or mip levels (C2.2): no image past it is created
	 * on the adapter, and crgpu_image_create and crgpu_image_create_placed
	 * refuse one with CR_RESULT_GPU_IMAGE_TOO_LARGE on every backend (the
	 * user's answer to C2.1). Vulkan may allow less for a given format, usage and
	 * tiling (vkGetPhysicalDeviceImageFormatProperties' maxExtent and
	 * maxArrayLayers, the specification's "Image Creation Limits"), which
	 * Carrottus GPU doesn't report; such a create fails. D3D12's and Metal's
	 * hold for every format. Within the bound, memory may still refuse
	 * (16384 x 16384 RGBA8 is 1 GiB; a 32768 cube of R8 6.2 GiB).
	 */
	/**
	 * [ADDED] The largest width of a CRGPU_IMAGE_TYPE_1D image the adapter
	 * creates, in texels: an upper bound (above).
	 *
	 * Vulkan: `VkPhysicalDeviceLimits::maxImageDimension1D`. D3D12: 16384
	 * (`D3D12_REQ_TEXTURE1D_U_DIMENSION`). Metal: 32768 on a GPU in
	 * `MTLGPUFamilyApple10`, 16384 on every other it lists (Apple's Metal
	 * feature set tables, "Maximum 1D texture width").
	 *
	 * Why: a caller's 1D lookup sized from its data. ABI: part of the
	 * struct's break (above). Tests: Q4.
	 */
	uint32_t max_image_width_1d;
	/**
	 * [ADDED] The largest width or height of a CRGPU_IMAGE_TYPE_2D image the
	 * adapter creates, in texels, for an image that isn't cube-capable: an
	 * upper bound (above).
	 *
	 * Vulkan: `VkPhysicalDeviceLimits::maxImageDimension2D`. D3D12: 16384
	 * (`D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION`). Metal: 32768 on a GPU in
	 * `MTLGPUFamilyApple10`, 16384 on every other it lists ("Maximum 2D
	 * texture width and height").
	 *
	 * Why: S33's own need, a caller sizing an atlas or a render target from
	 * its own numbers. ABI: part of the struct's break. Tests: Q4.
	 */
	uint32_t max_image_size_2d;
	/**
	 * [ADDED] The largest width, height or depth of a CRGPU_IMAGE_TYPE_3D
	 * image the adapter creates, in texels: an upper bound (above).
	 *
	 * Vulkan: `VkPhysicalDeviceLimits::maxImageDimension3D`. D3D12: 2048
	 * (`D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION`). Metal: 2048 ("Maximum 3D
	 * texture width, height, and depth").
	 *
	 * Why: a volume or a 3D lookup, whose limit is an eighth of 2D's on
	 * D3D12 and Metal. ABI: part of the struct's break. Tests: Q4.
	 */
	uint32_t max_image_size_3d;
	/**
	 * [ADDED] The largest width (equal to its height) of a cube-capable
	 * image (crgpu_image_desc::cube_capable), in texels: an upper bound
	 * (above).
	 *
	 * Vulkan: `VkPhysicalDeviceLimits::maxImageDimensionCube`. D3D12: 16384
	 * (`D3D12_REQ_TEXTURECUBE_DIMENSION`). Metal: 32768 on a GPU in
	 * `MTLGPUFamilyApple10`, 16384 on every other it lists ("Maximum cube
	 * map texture width and height").
	 *
	 * Why: a cube shadow map or environment map; Vulkan reports it apart
	 * from 2D's. ABI: part of the struct's break. Tests: Q4.
	 */
	uint32_t max_image_size_cube;
	/**
	 * [ADDED] The most array layers (crgpu_image_desc::array_layers) an
	 * image of CRGPU_IMAGE_TYPE_1D or CRGPU_IMAGE_TYPE_2D has: an upper
	 * bound (above). A cube-capable image's faces count, six layers a cube,
	 * and its layers are a multiple of six, so at 2048 it holds 341 cubes
	 * (2046 layers; C2.5, measured on Metal).
	 *
	 * Vulkan: `VkPhysicalDeviceLimits::maxImageArrayLayers`. D3D12: 2048
	 * (`D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION`, the 1D array's the
	 * same). Metal: 2048 ("Maximum number of layers per 1D texture array,
	 * 2D texture array, or 3D texture array").
	 *
	 * Why: a caller's atlas of layers, which reaches this bound before the
	 * 2D one. ABI: part of the struct's break. Tests: Q4.
	 */
	uint32_t max_image_array_layers;
} CRGPU_ADAPTER_PROPERTIES;

/*
 * [CHANGED] crgpu_image_create()'s and crgpu_image_create_placed()'s result
 * lists gain, after every check they list today and before the backend
 * makes anything (the user's answer to C2.1):
 *
 *   @retval CR_RESULT_GPU_IMAGE_TOO_LARGE A dimension or the layer count is
 *           past the device's adapter's limit for the image's type
 *           (CRGPU_ADAPTER_PROPERTIES): for CRGPU_IMAGE_TYPE_1D, `width`
 *           past max_image_width_1d; for CRGPU_IMAGE_TYPE_2D, `width` or
 *           `height` past max_image_size_2d, or, with `cube_capable`, past
 *           max_image_size_cube; for CRGPU_IMAGE_TYPE_3D, `width`,
 *           `height` or `depth` past max_image_size_3d; for any type,
 *           `array_layers` past max_image_array_layers. Nothing is made.
 *
 * Checked once, in src/gpu/crgpushared.c, called by every backend's two
 * creates at the same place, so the same image gets the same answer
 * everywhere; one `if` per field, in the order above. A size within the
 * limit may still fail for memory, or on Vulkan for a format that allows
 * less (C2.2), as before.
 *
 * Why: Metal ends the process on an image past its limit (measured,
 * C2.1); Vulkan leaves it undefined; D3D12 refuses with E_INVALIDARG. A
 * caller branches on it (a smaller atlas), so it has a result of its own.
 * ABI: a result added. Tests: Q4.
 *
 * ---------------------------------------------------------------------------
 * crcore.h
 * ---------------------------------------------------------------------------
 *
 * [CHANGED] CARROTTUS_VERSION_MINOR 3 to 4 (the layout break).
 *
 * [ADDED] CR_RESULT_GPU_IMAGE_TOO_LARGE = 370, after
 * CR_RESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING (369), its comment
 * "An image's size or layer count is past its adapter's limit
 * (CRGPU_ADAPTER_PROPERTIES::max_image_*)"; cr_result_str() names it.
 *
 * ---------------------------------------------------------------------------
 * Where each backend reads it (not in a header)
 * ---------------------------------------------------------------------------
 *
 * Vulkan (crgpuvulkancontext.c, crgpuvulkan___read_adapter_facts): the five
 * from the VkPhysicalDeviceProperties it already reads. D3D12
 * (crgpud3d12context.cpp): the five constants, beside the fixed values it
 * already reports (max_samplers' 2048). Metal (crgpumetalcontext.m): the
 * family check, beside the facts it reads when the instance lists the
 * device. Each the same field in the same place, so the property is
 * filled whether or not a device is ever made.
 */
