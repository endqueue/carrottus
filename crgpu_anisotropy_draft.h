/*
 * crgpu_anisotropy_draft.h - Carrottus GPU's anisotropic filtering: a
 * feature bit asked for at device creation, the adapter's largest level,
 * and a sampler past either refused, never clamped (card S49, decided
 * 2026-11-17: a; the card names the session and the first caller).
 *
 * DRAFT, 2026-11-17, version 1 (seven critique passes).
 * APPROVED 2026-11-17 with the user, every question as recommended.
 * Written unattended on the Mac (Apple M5 Pro, macOS 27.0.1) against
 * include/crgpu.h, include/crcore.h, abi.txt, src/crcore.c,
 * src/gpu/crgpushared.c and .h, the three backends' sampler creates
 * (crgpuvulkandescriptors.c, crgpud3d12descriptors.cpp,
 * crgpumetaldescriptors.m), adapter facts and device creation
 * (crgpuvulkancontext.c, crgpud3d12context.cpp, crgpumetalcontext.m), the
 * tests that make samplers (crgpu_objects_test.c, crgpu_device_test.c,
 * cr_misuse_tests.c and tests/misuse/make_misuse_tests.py), as read on
 * 2026-11-17 (carrottus 0.4.0, untagged). Not built, not in any target,
 * not compilable: it repeats declarations of crgpu.h and crcore.h to show
 * them changed. Every declaration below is marked [ADDED], [CHANGED] or
 * [REMOVED]; nothing unmarked changes. Nothing is built until it is
 * approved; once approved it is frozen (ROADMAP.md, section 1, "Work built
 * from approved drafts").
 *
 * ---------------------------------------------------------------------------
 * Revision notes (version 1 written, then seven passes, each fixed before
 * the next; one lens a pass, as card S49's session asked)
 * ---------------------------------------------------------------------------
 *
 * Pass 1 (wrong, missing, more than the need). 1. Version 1 refused "a
 * level below 1" and said nothing of what isn't a number or isn't whole:
 * NaN, an infinity and 1.5 passed its rule. Changed: a level is a whole
 * number from 1 (Q1), NaN and infinities refused with it. 2. It said
 * nothing of a zeroed desc, whose level is 0 and which every backend
 * takes today as 1. Changed: 0 is refused (a zeroed desc hasn't chosen,
 * crgpu.h's rule for a desc), listed as a break; searched: no caller in
 * the workspace leaves it 0. 3. The limit on an adapter without the bit
 * was Vulkan's raw maxSamplerAnisotropy, which a driver may report as 16
 * without the feature. Changed: 1 there, on every backend (Q5). 4.
 * Missing: who else uses it, what is left out, the follow-on in the
 * layer above (card S49's second half), and what is recorded when built.
 * Added. 5. Checked, nothing more than the need: one bit, one field, one
 * result; no query, no new struct, no new handle.
 *
 * Pass 2 (the platform APIs and industry practice compared). 1. WebGPU's
 * createSampler() refuses maxAnisotropy above 1 unless magFilter,
 * minFilter and mipmapFilter are all "linear", and sokol_gfx copies the
 * rule (VALIDATE_SAMPLERDESC_ANISTROPIC_REQUIRES_LINEAR_FILTERING). The
 * reason is D3D12: D3D12_FILTER_ANISOTROPIC is one value for minification,
 * magnification and mip filtering together, and this backend's
 * crgpu_d3d12_filter() returns it for any level above 1 whatever the three
 * filters say, while Vulkan and Metal honour them. The same desc filters
 * differently per backend today. Changed: Q2, recommending WebGPU's rule.
 * 2. D3D12 has an anisotropic filter with point mip filtering
 * (D3D12_FILTER_MIN_MAG_ANISOTROPIC_MIP_POINT, 0x54) only where
 * D3D12_FEATURE_DATA_D3D12_OPTIONS19::AnisoFilterWithPointMipSupported
 * says so: left out, appendable with a bit of its own (under "Left out").
 * 3. Added under "What was compared": sokol (uint32_t, 1 to 16), bgfx
 * (one level for every sampler, the device's largest, and the
 * anisotropic flags dropped on a Vulkan device without samplerAnisotropy:
 * both silent), SDL_GPU (a float and a bool, and a device creation
 * property that enables samplerAnisotropy on Vulkan), and why Carrottus
 * GPU doesn't take WebGPU's clamp. 4. The level's type: an integer in
 * D3D12, Metal, WebGPU and sokol, a float in Vulkan and SDL_GPU. Q1 asks
 * it.
 *
 * Pass 3 (the C API to standard). 1. Version 1 gave the new results no
 * order. Changed: after the existing checks, the desc's own rules first
 * (the level's value, then the filters it needs), the same answer on any
 * device; then the device (the bit, then the limit). One `if` per
 * condition, in every backend, written once in crgpushared.c. 2. The
 * create's Doxygen listed only the new results. Changed: the whole list,
 * existing ones included, with its params, and "nothing is written to
 * `*out_sampler` on a refusal", which every backend already keeps. 3.
 * CR_STRUCT_BASE: no struct added. The field is appended to
 * CRGPU_ADAPTER_PROPERTIES (CR_STRUCT_BASE_OUT), not chained (Q3). Read
 * from abi.txt: the struct is 320 bytes, align 8, its last field
 * (max_image_array_layers) at 312, so the new uint32_t sits at 316 in
 * what was tail padding and the size stays 320 on all three targets: a
 * caller built against the old header passes a struct as large. 4.
 * crcore.h: CR_RESULT_GPU_IMAGE_TOO_LARGE (370) sits inside the
 * "Acceleration structures" Doxygen group, which it isn't; 371 beside it
 * would inherit the wrong group. Changed: the group closes after 369, and
 * 370 and 371 go under a group of their own ("Past an adapter's limit"),
 * a comment change with no ABI effect. 5. "Since 0.4.0 (card X2)" on each
 * added item, as the ASTC draft does while 0.4 is untagged.
 *
 * Pass 4 (no hidden policy). 1. The backends hold three choices made for
 * the caller today, all in `desc->max_anisotropy > 1.0f ? ... : 1`: 0
 * and anything below 1 taken as 1; a fraction truncated on D3D12 and
 * Metal and kept on Vulkan; and D3D12's filter overriding the three
 * filters. Each becomes a refusal (Q1, Q2), none a quiet mapping. 2.
 * The limit of 1 on an adapter without the bit (Q5) is a fact, not a
 * default: a device without the feature filters at 1, on every API. 3.
 * The bit switches something on only on Vulkan (samplerAnisotropy); on
 * D3D12 and Metal the device is held to it all the same, as crgpu.h's
 * rule for every feature says. That is not "a flag only one platform acts
 * on" (CONVENTIONS.md): every backend refuses without it. 4. "16x means
 * the most the device has" is a caller's rule (a quality setting): the
 * caller reads the limit and lowers its own level. Said under "What the
 * caller decides"; nothing here lowers one.
 *
 * Pass 5 (threads and errors). 1. The new checks read the desc, the
 * device's features (set at its creation) and the adapter's facts (read
 * when the instance was created, never changed): no lock, no new
 * `Thread:` exception; crgpu_sampler_create() keeps the library's rule.
 * 2. Errors are the code alone (card A2), one code for each kind of
 * failure. Under card S10's M2 a refusal of Carrottus's own sends no
 * message. Carrottus GPU's instance has no message callback yet (S10 is
 * built for Carrottus Files only); when it has, a failing vkCreateSampler
 * (its VkResult) and Metal's nil state are messages, and D3D12's create,
 * which calls nothing (CreateSampler runs when the sampler is written into
 * a heap), has none. That is one more reason the level is checked at the
 * create: on D3D12 a bad one would surface only at a later descriptor
 * write, as a debug layer message far from its cause. Said in the
 * create's comment.
 *
 * Pass 6 (correctness against the code as built; every file named at the
 * head re-read). 1. crgpu.h's CRGPU_ADAPTER_PROPERTIES comment says
 * "Limits that only matter with a feature are in structs chained onto
 * `base.next`"; max_draw_indirect_count (a field, meaningful only with
 * CRGPU_FEATURE_MULTI_DRAW_INDIRECT) already contradicts it, and the new
 * field would too. Changed: [CHANGED] the comment, saying which limits are
 * chained (those of a feature behind a CRGPU_EXT_* macro) and which are
 * fields. 2. D3D12's comparison sampler is the basic filter | 0x80
 * (crgpud3d12descriptors.cpp): with ANISOTROPIC (0x55) that is
 * D3D12_FILTER_COMPARISON_ANISOTROPIC (0xd5), right; kept. 3. Metal's
 * truncating cast and its 0-to-1 mapping are where read
 * (crgpumetaldescriptors.m line 86); 17 measured to end the process under
 * validation. 4. Vulkan reads its features in
 * crgpuvulkan___read_adapter_facts() (vkGetPhysicalDeviceFeatures into
 * `available`) and enables them in crgpuvulkan___create_logical_device()
 * (`deviceFeatures`): one line each for the bit, as for imageCubeArray.
 * 5. crgpu___internal_check_device_desc() already refuses a bit the
 * adapter lacks: nothing to add there. 6. Every sampler made in the
 * workspace sets 1.0f with nearest or linear filters (the objects test
 * twice, the misuse fixture, three crgpusamples samples, three in the
 * layer above (its fourth passes its caller's level through), one in its
 * GPU tests, one further up): none breaks. 7. The
 * layer above maps CR_RESULTs to its own; 371 would fall to its generic
 * GPU failure. Said in the follow-on.
 *
 * Pass 7 (tests and the build). 1. Version 1's tests were three lines.
 * Changed: each case named, on which device, with what it expects,
 * including the order (a desc wrong in two ways), nothing written on a
 * refusal, and nothing allocated by one (the objects test's counting
 * allocator). 2. Where each runs: the Mac builds and runs Metal; no
 * Vulkan preset exists on the Mac, but the two Vulkan files this changes
 * pass `clang -fsyntax-only` there against SDK 1.4.363.0 today (tried
 * 2026-11-17, unchanged files, 0 errors), so the building session
 * syntax-checks them after its edit; D3D12 is written, not compiled, until
 * Windows. 3. The misuse harness: no new entry point; its sampler fixture
 * (nearest filters, 1.0f) stays valid, so the generator is rerun and its
 * output expected unchanged. 4. crcore_test names the new result through
 * cr_result_str(). 5. Checked: no word check.sh's caller-names refuses in
 * carrottus appears in this file.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * crgpu_sampler_desc::max_anisotropy reaches every backend today, and no
 * part of the API says what a device may be asked for:
 *
 *  1. No adapter reports whether it filters anisotropically or its largest
 *     level. A caller can't choose a level a device takes.
 *  2. The Vulkan backend never enables `samplerAnisotropy`
 *     (crgpuvulkancontext.c, crgpuvulkan___create_logical_device()), yet
 *     passes anisotropyEnable VK_TRUE for any level above 1
 *     (crgpuvulkandescriptors.c): outside Vulkan's rules on every device
 *     (VUID-VkSamplerCreateInfo-anisotropyEnable-01070), and a level past
 *     maxSamplerAnisotropy is outside them again (-01071).
 *  3. A level past 16 reaches D3D12 (MaxAnisotropy, which its debug layer
 *     checks when the sampler is written into a heap, not at the create)
 *     and Metal (maxAnisotropy, whose API validation ends the process:
 *     measured, below).
 *  4. The same desc filters differently per backend: above 1, D3D12 is
 *     anisotropic in every direction whatever the three filters say;
 *     Vulkan and Metal honour them. A fraction is kept by Vulkan and
 *     truncated by D3D12 and Metal; 0 is taken as 1 by all three.
 *
 * This draft passes through what each API says (whether the adapter
 * filters anisotropically, its largest level) and refuses a sampler the
 * device can't take, or one the backends would read differently. Which
 * level to ask for, and what to do on a device that takes less, stay the
 * caller's.
 *
 * ---------------------------------------------------------------------------
 * What changes, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * One feature bit, CRGPU_FEATURE_SAMPLER_ANISOTROPY (bit 18: S48 has 16
 * and 17, S50 19), on every D3D12 and Metal adapter and on a Vulkan
 * adapter with samplerAnisotropy, asked for at device creation as every
 * feature is. One limit, CRGPU_ADAPTER_PROPERTIES::max_sampler_anisotropy
 * (16 on D3D12 and Metal, Vulkan's maxSamplerAnisotropy, 1 without the
 * bit), in the struct's tail padding. crgpu_sampler_create() refuses a
 * level that isn't a whole number from 1, a level above 1 whose three
 * filters aren't all linear, a level above 1 on a device created without
 * the bit, and a level past the limit (a result of its own); it never
 * lowers or raises one. One result added, CR_RESULT_GPU_SAMPLER_
 * ANISOTROPY_TOO_LARGE (371).
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 *  - Vulkan (vulkan_core.h and validusage.json of SDK 1.4.363.0 on this
 *    machine): VkPhysicalDeviceFeatures::samplerAnisotropy (line 3546),
 *    optional, enabled at device creation;
 *    VkPhysicalDeviceLimits::maxSamplerAnisotropy (line 3646), a float, at
 *    least 16 where the feature is supported (the specification's required
 *    limits); VkSamplerCreateInfo::anisotropyEnable (line 4151) and
 *    maxAnisotropy. VUID-VkSamplerCreateInfo-anisotropyEnable-01070:
 *    without the feature, anisotropyEnable must be VK_FALSE. -01071: when
 *    enabled, maxAnisotropy is from 1.0 to maxSamplerAnisotropy inclusive.
 *    The other anisotropy VUIDs (-01076 unnormalized coordinates, -01646
 *    YCbCr conversion, -01081 cubic filters, -02578 and -06969 flags) name
 *    things crgpu_sampler_desc has none of. With anisotropy on, Vulkan
 *    still reads magFilter, minFilter and mipmapMode; how the filtering is
 *    done is the implementation's.
 *  - D3D12 (d3d12.h of DirectX-Headers, the copy under .refs/, SDK version
 *    616): anisotropic filtering is D3D12_SAMPLER_DESC::Filter
 *    D3D12_FILTER_ANISOTROPIC (0x55) or D3D12_FILTER_COMPARISON_ANISOTROPIC
 *    (0xd5), one value for minification, magnification and mips; with point
 *    mips, D3D12_FILTER_MIN_MAG_ANISOTROPIC_MIP_POINT (0x54) where
 *    D3D12_FEATURE_DATA_D3D12_OPTIONS19::AnisoFilterWithPointMipSupported.
 *    MaxAnisotropy is a UINT; D3D12_MAX_MAXANISOTROPY 16 and
 *    D3D12_REQ_MAXANISOTROPY 16 (every device of feature level 11_0 and
 *    up; this backend lists 12_0 and up). No feature to switch on, no
 *    query.
 *  - Metal (MTLSampler.h of the macOS 27 SDK; measured): maxAnisotropy is
 *    an NSUInteger, default 1; the header gives no range. Measured on this
 *    Mac (Apple M5 Pro, macOS 27.0.1, a scratch program in the system's
 *    temporary directory, deleted after; Metal's API validation on): 1, 2,
 *    3 and 16 make a sampler, with linear or nearest filters and mips; 0,
 *    17 and 32 end the process ("MTLSamplerDescriptor's maxAnisotropy value
 *    (17) is invalid. Should be in [1, 16] range"). No feature, no query.
 *  - WebGPU (the W3C specification, GPUSamplerDescriptor and
 *    createSampler()): maxAnisotropy an unsigned short, default 1; below 1
 *    refused; above 1, magFilter, minFilter and mipmapFilter must be
 *    "linear"; and "the provided maxAnisotropy value will be clamped to the
 *    maximum value that the platform supports". WebGPU exposes no
 *    anisotropy limit and no feature, so a page can't learn what the device
 *    takes: clamping is the only portable answer it has. Carrottus GPU
 *    reports the limit, so the caller can lower its own level and know what
 *    it got; a clamp here would be a choice made for it, and the level the
 *    GPU used would be one the caller never named.
 *  - Industry practice: sokol_gfx (sg_sampler_desc::max_anisotropy a
 *    uint32_t, "must be 1..16", WebGPU's linear-filter rule as a
 *    validation error; its GL backend lowers a level past the device's
 *    silently, its Vulkan and Metal backends pass it through); bgfx (one
 *    level for every sampler, the device's largest, set by
 *    BGFX_RESET_MAXANISOTROPY, and the anisotropic flags dropped on a
 *    Vulkan device without samplerAnisotropy: both made for the caller);
 *    SDL_GPU (SDL_GPUSamplerCreateInfo's float max_anisotropy and bool
 *    enable_anisotropy, and SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_
 *    BOOLEAN enabling samplerAnisotropy at device creation, the nearest
 *    to this draft's bit).
 *
 * Hazel is not compared: Carrottus is not measured against an engine (the
 * workspace's CLAUDE.md, "External references").
 *
 * ---------------------------------------------------------------------------
 * What the caller decides, and what Carrottus GPU only reports
 * ---------------------------------------------------------------------------
 *
 * Reported: whether the adapter filters anisotropically (the bit) and its
 * largest level (the limit). Decided by the caller: whether its device has
 * the feature, each sampler's level and filters, and what to do on a
 * device without the feature or with a lower limit (lower its level, show
 * the person what it got, refuse). Carrottus GPU never lowers a level,
 * never raises one, never turns anisotropy on or off and never changes a
 * filter for the caller.
 *
 * ---------------------------------------------------------------------------
 * Who else uses it
 * ---------------------------------------------------------------------------
 *
 *  - A renderer above that wraps samplers: it checks the same first and
 *    says why in its own words (below, "The follow-on above").
 *  - A game's terrain and ground materials, seen at grazing angles (card
 *    S49's first caller): a quality setting from 1 to 16, lowered by that
 *    caller to the device's limit and read back as the level in use.
 *  - CAD viewports, when a material takes an image (a floor or a site
 *    texture at a low angle): the same bit and limit.
 *  - Carrottus on Android (card S47), where samplerAnisotropy is optional
 *    on Vulkan: the bit is how a caller there knows.
 *  - crgpusamples: every sampler at 1; unchanged.
 *
 * ---------------------------------------------------------------------------
 * Left out, and why
 * ---------------------------------------------------------------------------
 *
 *  - Point mip filtering with anisotropy: D3D12 needs OPTIONS19's
 *    AnisoFilterWithPointMipSupported for it, so it would be a bit of its
 *    own, appended when a caller needs a sharp mip step under anisotropic
 *    filtering. Until then Q2's rule refuses it everywhere.
 *  - The other fields' checks (a filter, address mode or compare op that
 *    isn't one of its values, a NaN level of detail, min_lod above
 *    max_lod): each backend maps an unknown filter or mode to a default of
 *    its own today. Not this card's; recommended as a card of its own, the
 *    same shape as this one.
 *  - Immutable samplers (card S7, decided, not built): when built, their
 *    desc carries the same level under the same checks; nothing here
 *    changes for them.
 *  - A per-format question (which formats filter anisotropically): no API
 *    has one; anisotropy applies to every format that filters linearly.
 *
 * ---------------------------------------------------------------------------
 * The version and the breaks
 * ---------------------------------------------------------------------------
 *
 *  - CRGPU_FEATURES: bit 18 appended. No bit's meaning changes.
 *  - CRGPU_ADAPTER_PROPERTIES: max_sampler_anisotropy added at offset 316,
 *    in what was tail padding: sizeof stays 320, align 8, on Apple, Linux
 *    and Windows (abi.txt). A binary built against the old header passes a
 *    320-byte struct as before; the new value lands in its padding. Not a
 *    layout break; listed in abi.txt as an added field.
 *  - crgpu_sampler_create() refuses what it passed before, a behaviour
 *    break, listed:
 *      - a level above 1 on a device created without the bit (worked on
 *        D3D12 and Metal; outside the rules on Vulkan);
 *      - a level that isn't a whole number from 1: 0 and anything below 1
 *        (taken as 1 by every backend), a fraction (kept by Vulkan,
 *        truncated by D3D12 and Metal), NaN and infinities;
 *      - a level above 1 with a filter that isn't CRGPU_FILTER_LINEAR
 *        (D3D12 filtered linearly anyway; Vulkan and Metal honoured it)
 *        (Q2 a);
 *      - a level past the limit (outside every API's rules: a validation
 *        error on Vulkan and D3D12, the process ended on Metal).
 *    Searched: every sampler made in the workspace passes 1.0f, so none
 *    breaks; the building session searches again and lists any it finds.
 *  - CR_RESULT: 371 appended. The Doxygen group around 370 changes, with
 *    no value moved.
 *  - No version raise: carrottus' 0.4.0 is untagged, so this lands in it
 *    (card X2). ./check.sh --update-abi lists the field, the macro and the
 *    value.
 *
 * New handles: none. New struct types: none. Stored formats touched: none.
 *
 * Thread: unchanged. crgpu_sampler_create() keeps the library's rule (one
 * caller at a time on the device's objects); its new checks read the desc,
 * the device's features and the adapter's facts, none of which changes
 * after creation. Allocation: a refusal allocates nothing; a create
 * allocates as before.
 *
 * ---------------------------------------------------------------------------
 * Questions (numbered; each with options and a recommendation)
 * ---------------------------------------------------------------------------
 *
 * Q1. THE LEVEL'S VALUES AND TYPE.
 *     (a) RECOMMENDED. Keep `float max_anisotropy`; it must be a whole
 *         number from 1 to the limit: 0, anything below 1, a fraction,
 *         NaN and infinities refused (CR_RESULT_DESC_INVALID_PARAMETERS).
 *         D3D12 and Metal take only whole numbers and Vulkan takes them
 *         exactly, so one rule gives one result everywhere; no layout or
 *         type changes, and every caller's 1.0f stands. A zeroed desc is
 *         refused, as one that hasn't chosen.
 *     (b) Change the field to uint32_t, as D3D12, Metal, WebGPU and sokol
 *         have it: no fraction or NaN to refuse, but a type change every
 *         caller rebuilds for, a float caller converting silently, and a
 *         binary caller's 16.0f read as 1,098,907,648. Only 0 then needs
 *         refusing.
 *     (c) Today's mapping: below 1 taken as 1, fractions as each API takes
 *         them. A default and a per-backend difference.
 *
 * Q2. FILTERS WITH ANISOTROPY.
 *     (a) RECOMMENDED. Above 1, mag_filter, min_filter and mipmap_filter
 *         are all CRGPU_FILTER_LINEAR, or the create is refused
 *         (CR_RESULT_DESC_INVALID_PARAMETERS). WebGPU's rule and sokol's,
 *         for the same reason: D3D12 has one anisotropic filter for all
 *         three, so it is the one combination every backend gives the same
 *         way. A caller wanting point mips under anisotropy waits for the
 *         bit under "Left out".
 *     (b) Any filters, each backend its own: D3D12 linear in every
 *         direction, Vulkan and Metal as asked. The same desc gives
 *         different images on different machines, found by the person.
 *     (c) (a) plus point mips now, with a second bit from OPTIONS19 on
 *         D3D12 (every Vulkan and Metal device has it): a bit and a path
 *         no caller has asked for.
 *
 * Q3. WHERE THE LIMIT LIVES.
 *     (a) RECOMMENDED. A field of CRGPU_ADAPTER_PROPERTIES, after
 *         max_image_array_layers: what card S49 decided ("in the adapter's
 *         limits"), as max_draw_indirect_count is for its feature, in the
 *         tail padding so no size changes. The struct's comment is
 *         corrected to say chained limits are those of a feature behind a
 *         CRGPU_EXT_* macro.
 *     (b) A chained CRGPU_SAMPLER_LIMITS struct: no field added, but a
 *         struct type, a CR_STRUCT_TYPE, a chain check and a caller's
 *         chain for one number of a feature every backend implements.
 *
 * Q4. THE RESULT PAST THE LIMIT.
 *     (a) RECOMMENDED. CR_RESULT_GPU_SAMPLER_ANISOTROPY_TOO_LARGE (371),
 *         as CR_RESULT_GPU_IMAGE_TOO_LARGE (370) is for an image past its
 *         limits: the one refusal a caller recovers from by changing a
 *         number (lower the level, retry), so it gets its own code
 *         (Carrottus's "each check has its own code", card A2).
 *     (b) CR_RESULT_DESC_INVALID_PARAMETERS: no new value, but the same
 *         code as a NaN level or a nearest filter, which a caller fixes in
 *         its code, not at run time.
 *
 * Q5. THE LIMIT ON AN ADAPTER WITHOUT THE BIT.
 *     (a) RECOMMENDED. 1, on every backend: the largest level a sampler on
 *         a device of that adapter can have, which is what the field says.
 *         Never 0, which is no level.
 *     (b) 0, as a chained limits struct is without its feature: a value no
 *         sampler can have, so a caller special-cases it.
 *     (c) Whatever the API reports (Vulkan may report 16 without the
 *         feature): a number that promises what the device refuses.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 *  - crgpu_device_test, on every adapter (a test_sampler_anisotropy_limit()
 *    beside test_image_limits()): the bit and the limit printed. Vulkan:
 *    the bit exactly when vkGetPhysicalDeviceFeatures' samplerAnisotropy
 *    (read through crgpu_vulkan_natives.h's physical device), and then the
 *    limit at least 16 (the required minimum); 1 without it. D3D12: the
 *    bit and 16 on every adapter. Metal: the bit and 16 on every adapter. A
 *    device created with the bit reports it (crgpu_device_get_features()),
 *    one created without it doesn't.
 *  - crgpu_objects_test, on the adapter chosen (--adapter), its main device
 *    made with the bit added to the features it asks for where the adapter
 *    has it, each expected result checked and, on a refusal,
 *    `*out_sampler` left as the sentinel set before the call and the
 *    counting allocator's count unchanged:
 *      - level 1 with nearest filters (as today) and with linear ones:
 *        made;
 *      - level 2, and the limit, all three filters linear: made; the limit
 *        again with compare_enable (D3D12's 0xd5 path): made;
 *      - limit + 1, linear: CR_RESULT_GPU_SAMPLER_ANISOTROPY_TOO_LARGE;
 *      - 0, 0.5, 1.5, -1, NaN, +infinity: CR_RESULT_DESC_INVALID_PARAMETERS;
 *      - level 4 with each one filter NEAREST (mag, min, mip):
 *        CR_RESULT_DESC_INVALID_PARAMETERS;
 *      - order: level 1.5 with a nearest filter and level limit + 1 with a
 *        nearest filter both give CR_RESULT_DESC_INVALID_PARAMETERS (the
 *        desc's own rules first);
 *    then, after that device is destroyed (one device per adapter), on a
 *    device created without the bit, made on every adapter (a bit is asked
 *    for, not given):
 *      - level 1: made; level 2, linear: CR_RESULT_GPU_FEATURE_NOT_SUPPORTED;
 *      - level limit + 1, linear: CR_RESULT_GPU_FEATURE_NOT_SUPPORTED (the
 *        bit before the limit);
 *      - level 1.5: CR_RESULT_DESC_INVALID_PARAMETERS (the desc first).
 *    Under Vulkan's validation layer, D3D12's debug layer (the samplers
 *    made are written into a descriptor set once, so D3D12's CreateSampler
 *    runs) and Metal's API validation: silent.
 *  - crcore_test: cr_result_str(CR_RESULT_GPU_SAMPLER_ANISOTROPY_TOO_LARGE)
 *    is its name.
 *  - The misuse harness: no new entry point. Its crgpu_sampler fixture
 *    (nearest filters, 1.0f) stays valid; tests/misuse/make_misuse_tests.py
 *    is rerun and cr_misuse_tests.c is expected unchanged; it runs ten
 *    times in Debug as the build's rule says.
 *  - What the GPU filters (that a level above 1 changes the image) is a
 *    caller's test, in the layer above's GPU tests, which draw (below).
 *  - Where each runs: the Mac (Apple M5 Pro: Metal; plain, -asan, -tsan;
 *    Metal's API validation on), and the iOS simulator built (it lists no
 *    adapter, so the GPU tests skip). The two Vulkan files changed
 *    (crgpuvulkancontext.c, crgpuvulkandescriptors.c) syntax-checked on the
 *    Mac with `clang -fsyntax-only -std=c11 -DCRGPU_COMPILE_VULKAN_API=1
 *    -Iinclude -Isrc -Isrc/gpu -I../eqstd/include -I<SDK>/macOS/include`,
 *    which passes on both files today. D3D12 written, not compiled, there.
 *    Owed: Windows 11 (the Iris Xe, the RTX A2000 and WARP over D3D12 under
 *    the debug layer and GPU-based validation; the Iris Xe and the A2000
 *    over Vulkan under validation), and Fedora over Vulkan (ANV, NVK,
 *    RADV), each driver's samplerAnisotropy and maxSamplerAnisotropy
 *    recorded in CRGPU_DEVICE_TODO.md.
 *
 * ---------------------------------------------------------------------------
 * crgpu.h
 * ---------------------------------------------------------------------------
 */

/**
 * [ADDED] A sampler whose crgpu_sampler_desc::max_anisotropy is above 1,
 * up to CRGPU_ADAPTER_PROPERTIES::max_sampler_anisotropy; refused with
 * CR_RESULT_GPU_FEATURE_NOT_SUPPORTED on a device created without it.
 * Vulkan: `VkPhysicalDeviceFeatures::samplerAnisotropy`, enabled with it.
 * D3D12 and Metal: every adapter. Since 0.4.0 (card X2).
 *
 * Why: Vulkan's anisotropic filtering is an optional feature that must be
 * enabled for any sampler above 1 to be within its rules, and a caller on
 * a device without it must know before it chooses a level. Named as
 * Vulkan names it. ABI: a bit appended (18; S48 holds 16 and 17, S50 19).
 * Tests: crgpu_device_test, crgpu_objects_test.
 */
#define CRGPU_FEATURE_SAMPLER_ANISOTROPY (UINT64_C(1) << 18)

/**
 * @brief What an adapter is and what it can do, as the graphics API reports
 * it.
 *
 * [CHANGED] Its comment only, where it says where limits live (Q3 a):
 *
 * Facts only: nothing here is chosen. The limits of a feature behind a
 * CRGPU_EXT_* macro are in structs chained onto `base.next`
 * (CRGPU_MESH_SHADING_LIMITS, CRGPU_BINDLESS_LIMITS,
 * CRGPU_RAY_QUERY_LIMITS), each all 0 when the adapter lacks the feature. A
 * limit of a feature every backend implements is a field here
 * (#max_draw_indirect_count, #max_sampler_anisotropy), whose comment says
 * its value without the feature. A limit counts only on a device created
 * with its feature.
 */
typedef struct CRGPU_ADAPTER_PROPERTIES {
	/* ... every field as before, `base` to max_image_array_layers ... */

	/**
	 * [ADDED] The largest crgpu_sampler_desc::max_anisotropy a sampler
	 * on a device of this adapter may have, on a device created with
	 * CRGPU_FEATURE_SAMPLER_ANISOTROPY; 1 on an adapter without the
	 * feature (Q5 a). A sampler past it is refused with
	 * CR_RESULT_GPU_SAMPLER_ANISOTROPY_TOO_LARGE; Carrottus GPU never
	 * lowers a level to it.
	 *
	 * Vulkan: `VkPhysicalDeviceLimits::maxSamplerAnisotropy`, rounded down
	 * to a whole number (at least 16 where the feature is supported). D3D12:
	 * 16 (`D3D12_REQ_MAXANISOTROPY`). Metal has no query: 16, the range
	 * Metal's validation enforces on every GPU. Since 0.4.0 (card X2).
	 *
	 * Why: a caller chooses a level a device takes only if it can read
	 * the largest (card S49). ABI: at offset 316, in the struct's former
	 * tail padding; sizeof unchanged (320). Tests: crgpu_device_test.
	 */
	uint32_t max_sampler_anisotropy;
} CRGPU_ADAPTER_PROPERTIES;

/* crgpu_sampler_desc: every field as before; one comment changes. */

	/**
	 * [CHANGED] Its comment only (Q1 a, Q2 a). The largest ratio of
	 * anisotropy the sampler filters with: 1 for none. A whole number
	 * from 1 to CRGPU_ADAPTER_PROPERTIES::max_sampler_anisotropy (0, a
	 * fraction, NaN and infinities are refused, so a zeroed desc must set
	 * it). Above 1, the device was created with
	 * CRGPU_FEATURE_SAMPLER_ANISOTROPY, and #mag_filter, #min_filter and
	 * #mipmap_filter are all CRGPU_FILTER_LINEAR: the one combination
	 * every backend filters the same way. How the GPU spends the samples
	 * is the graphics API's.
	 *
	 * Was: "1.0 disables anisotropic filtering." Why: the backends took 0
	 * as 1, truncated fractions on D3D12 and Metal, and on D3D12 ignored
	 * the filters above 1. ABI: no layout change; a behaviour break, listed.
	 */
	float max_anisotropy;

/**
 * @brief Creates a sampler.
 *
 * [CHANGED] Its result list, and two paragraphs. Every check is made
 * before anything is allocated, in this order, the same in every backend:
 * the arguments, then the desc's own rules (the same answer on any
 * device), then the desc against the device.
 *
 * The level is checked here on every backend. D3D12 calls nothing at
 * this point (its sampler is made when written into a heap), so a level
 * a device can't take would otherwise surface only at a later descriptor
 * write, as a debug layer message; Metal's validation ends the process on
 * one.
 *
 * Nothing is written to `*out_sampler` unless the call succeeds.
 *
 * @param[in]  device      Device to create the sampler on.
 * @param[in]  desc        Sampler configuration. Read during the call only.
 * @param[out] out_sampler Receives the new sampler on success.
 *
 * @retval CR_SUCCESS The sampler was created.
 * @retval CR_RESULT_GPU_INVALID_DEVICE `device` is NULL (or, in a Debug
 *         build, destroyed).
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_GPU_SAMPLER_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_sampler` is NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS [ADDED] `desc->max_anisotropy`
 *         isn't a whole number from 1: 0, below 1, a fraction, NaN or an
 *         infinity (Q1 a). Since 0.4.0.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS [ADDED] `desc->max_anisotropy`
 *         is above 1 and `mag_filter`, `min_filter` or `mipmap_filter`
 *         isn't CRGPU_FILTER_LINEAR (Q2 a). Since 0.4.0.
 * @retval CR_RESULT_GPU_FEATURE_NOT_SUPPORTED [ADDED]
 *         `desc->max_anisotropy` is above 1 and the device wasn't created
 *         with CRGPU_FEATURE_SAMPLER_ANISOTROPY. Since 0.4.0.
 * @retval CR_RESULT_GPU_SAMPLER_ANISOTROPY_TOO_LARGE [ADDED]
 *         `desc->max_anisotropy` is above the device's adapter's
 *         CRGPU_ADAPTER_PROPERTIES::max_sampler_anisotropy (Q4 a). Since
 *         0.4.0.
 * @return Another CR_RESULT if the backend fails, such as
 *         CR_RESULT_OUT_OF_MEMORY or CR_RESULT_API_SPECIFIC_FAILURE.
 *
 * Why: the device is held to the feature it was created with (crgpu.h's
 * rule for features), and no level reaches a graphics API that refuses it
 * or a backend that would read it differently. ABI: the signature is
 * unchanged; refusals of what was accepted (listed). Tests:
 * crgpu_objects_test.
 *
 * @see crgpu_sampler_destroy()
 */
CR_RESULT crgpu_sampler_create(
    crgpu_device device,
    const crgpu_sampler_desc *desc,
    crgpu_sampler *out_sampler);

/*
 * ---------------------------------------------------------------------------
 * crcore.h
 * ---------------------------------------------------------------------------
 */

	/* [CHANGED] The "Acceleration structures (CRGPU_EXT_RAY_QUERY)" group
	 * closes after CR_RESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING
	 * (369), and 370 moves into a group of its own, with 371 (pass 3).
	 * Comments only; no value moves. */

	/**
	 * @name Past an adapter's limit
	 * @{
	 */
	/** An image's size or layer count is past its adapter's limit
	 * (CRGPU_ADAPTER_PROPERTIES::max_image_*). Since 0.4.0. */
	CR_RESULT_GPU_IMAGE_TOO_LARGE                             = 370,
	/** [ADDED] A sampler's max_anisotropy is past its adapter's limit
	 * (CRGPU_ADAPTER_PROPERTIES::max_sampler_anisotropy). Since 0.4.0
	 * (card X2).
	 *
	 * Why: the one sampler refusal a caller recovers from at run time, by
	 * a lower level (Q4 a). ABI: a value appended. Tests: crgpu_objects_test,
	 * crcore_test. */
	CR_RESULT_GPU_SAMPLER_ANISOTROPY_TOO_LARGE                = 371,
	/** @} */

/*
 * ---------------------------------------------------------------------------
 * Where each backend does it (not in a header)
 * ---------------------------------------------------------------------------
 *
 * - src/gpu/crgpushared.c and .h: crgpu___internal_check_sampler_desc(desc,
 *   features, max_sampler_anisotropy), the four new checks in the order
 *   above, one `if` each, called by every backend's crgpu_sampler_create()
 *   right after its out_sampler check, before anything is allocated, with
 *   the device's `_features` and its adapter's
 *   `_facts.properties.max_sampler_anisotropy`. A whole number is
 *   `isfinite(v) && v >= 1.0f && v == floorf(v)`; past the limit is
 *   `v > (float)limit` (both exact for every level a uint32_t limit names
 *   up to 2^24).
 * - src/crcore.c: cr_result_str()'s case for 371.
 * - Vulkan (crgpuvulkancontext.c): crgpuvulkan___read_adapter_facts() sets
 *   the bit where `available.samplerAnisotropy`, and the limit
 *   (uint32_t)floorf(limits.maxSamplerAnisotropy) there, else 1;
 *   crgpuvulkan___create_logical_device() sets
 *   `deviceFeatures.samplerAnisotropy` from the bit, beside
 *   imageCubeArray. (crgpuvulkandescriptors.c): anisotropyEnable is
 *   `level > 1`, maxAnisotropy the level; the `> 1.0f` test stays and its
 *   meaning is now exact.
 * - D3D12 (crgpud3d12context.cpp): crgpu_d3d12___read_adapter_facts() adds
 *   the bit to the every-adapter set and sets the limit
 *   D3D12_REQ_MAXANISOTROPY. (crgpud3d12descriptors.cpp):
 *   crgpu_d3d12_filter()'s ANISOTROPIC for a level above 1 is now the
 *   mapping of linear filters with anisotropy, not an override (the check
 *   has refused anything else); MaxAnisotropy is (UINT)level, the `: 1`
 *   fallback gone; the comparison path's | 0x80 kept.
 * - Metal (crgpumetalcontext.m): crgpumetal___internal_read_adapter_facts()
 *   adds the bit to the every-GPU set and sets the limit 16, with a comment
 *   that Metal has no query and its validation enforces [1, 16] (measured,
 *   2026-11-17). (crgpumetaldescriptors.m): maxAnisotropy is
 *   (NSUInteger)level, the `: 1` fallback gone.
 *
 * ---------------------------------------------------------------------------
 * The follow-on above (card S49's second half; not built here)
 * ---------------------------------------------------------------------------
 *
 * Written in the terms of the layer above, which keeps its own sampler
 * desc mirroring this one and reports every failure in a per-call error
 * struct, in developer English, naming the part that failed (the
 * workspace's CLAUDE.md, "Errors"). Its half is named in its own approved
 * draft (card L68, approved 2026-11-17, "a prerequisite"): its sampler
 * create refuses a level the device can't take, with its invalid-argument
 * result and a message naming the level and the limit, for every sampler.
 * After this is built:
 *
 *  - It reads the device's features (crgpu_device_get_features()) and its
 *    adapter's limit (crgpu_device_get_adapter(),
 *    crgpu_adapter_get_properties()) once, when its instance is made on
 *    the device the caller hands it, and keeps both.
 *  - Its sampler create checks its desc's level before calling
 *    crgpu_sampler_create(), each its own check and message: not a whole
 *    number from 1; above 1 with a filter not linear (if Q2 a is approved,
 *    a rule that layer's draft doesn't yet name, so it is added there in
 *    the same words); above 1 on a device without
 *    CRGPU_FEATURE_SAMPLER_ANISOTROPY ("the device was created without
 *    anisotropic filtering"); above the limit ("level 32 is past the
 *    device's 16"). It never lowers a level either.
 *  - Its table from CR_RESULT to its own results maps 371 to its
 *    invalid-argument result (today 371 would fall to its generic GPU
 *    failure), though its own check comes first.
 *  - Its built-in samplers stay at 1, so nothing of its own breaks.
 *  - Its GPU tests draw a plane at a grazing angle with a fine
 *    high-contrast texture through a sampler at 1 and one at the device's
 *    limit, on a device created with the bit, and check the far half
 *    differs between the two (what the GPU filters is not compared to an
 *    image: each implementation filters its own way); and its refusals,
 *    each with its message.
 *
 * ---------------------------------------------------------------------------
 * Records when built
 * ---------------------------------------------------------------------------
 *
 * carrottus/CLAUDE.md, "Known gaps": an entry for anisotropic filtering
 * (the bit, the limit per backend, the four refusals and their order, no
 * clamp, which machines ran it). CRGPU_DEVICE_TODO.md: S49's section, each
 * machine's run and each Vulkan driver's samplerAnisotropy and
 * maxSamplerAnisotropy. abi.txt by ./check.sh --update-abi, its breaks
 * named in the commit. The workspace's DECISIONS.md card S49 marked built,
 * and the layer above's TODO item for its half moved to ready.
 */
