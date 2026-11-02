/*
 * spudgpu_room_before_1_0_draft.h - four public shapes SpudGPU settles
 * before 1.0 so that later work (hardware decode and encode, HDR output,
 * presentation timing, shared resources) is added without a break (card S11,
 * decided 2026-11-01 with the user, a, c, d and f as revised when presented;
 * the workspace's DECISIONS.md, "Decided"; SPUDGPU_DEVICE_TODO.md, "Room
 * before 1.0").
 *
 * APPROVED 2026-11-01 by the user, as written here. To be built into
 * spudgpu.h and spudcore.h; where building finds this draft wrong or
 * incomplete, a card says so and this file is not edited to match.
 *
 * DRAFT, 2026-11-01. Not compilable: it names types
 * spudgpu.h and spudcore.h declare. Every declaration below is marked
 * [ADDED], [CHANGED] or [REMOVED]; nothing unmarked changes.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * From 1.0 a released struct layout is frozen and grows only by chained
 * structs (the workspace's "Versions and releases"). Four shapes today
 * can't grow that way:
 *
 *   (a) SPUDGPU_ADAPTER_PROPERTIES::queue_pools is a fixed array of 3. A
 *       queue kind added later (a decode or encode queue, which Vulkan
 *       lists as families of their own) needs more entries than any fixed
 *       count promises.
 *   (c) spudgpu_swap_chain_desc has a format and no colour space. Added
 *       later as a chained struct, its absence would mean sRGB: a default
 *       that hides a choice. It must be a required field before the
 *       freeze. And no call says which formats a surface takes.
 *   (d) spudgpu_swap_chain_present takes no struct, so nothing can be
 *       chained onto it later (a time to present at; HDR metadata per
 *       frame), and returns nothing, so an out-of-date swap chain or a
 *       lost device goes unreported.
 *   (f) Two sync objects where D3D12 and Metal have one: a fence (a counter
 *       on every backend) and spudgpu_semaphore (binary on Vulkan, a hidden
 *       counter on D3D12 and Metal), whose waits mean different things per
 *       backend. A queue waiting on a fence's value is what every API does
 *       natively.
 *
 * Not in this draft (card S11's b, e and g, written in SPUDGPU_DEVICE_TODO.md):
 * plane views (bits appended to SPUDGPU_IMAGE_ASPECT, no break), the import
 * and export EXT group, and decode and encode sessions.
 *
 * ---------------------------------------------------------------------------
 * (a) Queue pools: spudgpu.h
 * ---------------------------------------------------------------------------
 */

/* [CHANGED] in SPUDGPU_ADAPTER_PROPERTIES, the same names, the array
 * becoming a pointer: */
	/** The pools a device's queues come from, #queue_pool_count of them:
	 * the adapter's own array, made when the instance listed its adapters
	 * and never changed (as spudgpu_enumerate_adapters() hands out the
	 * instance's), valid until the instance is destroyed; do not free it.
	 * Every queue kind the backend makes is in exactly one pool. How many
	 * there are is the API's: Vulkan one per queue family the device desc's
	 * rule takes kinds from; D3D12 and Metal three, one kind each. A kind
	 * added later adds pools here, with no change to the struct. */
	const SPUDGPU_QUEUE_POOL *queue_pools;
	/** The number of entries of #queue_pools, 1 or more. */
	uint32_t queue_pool_count;
/* A caller's `properties.queue_pools[i]` reads the same; only a copy of
 * the properties kept past the instance's life would read freed memory. No
 * caller in the workspace reads the field today; spudlib's front end does
 * (`spudgpushared.c`, the device desc's checks), from the adapter. */

/* SPUDGPU_QUEUE_POOL stays as it is (frozen, an array element). Its comment
 * changes from "held by value in SPUDGPU_ADAPTER_PROPERTIES" to "an element
 * of the adapter's array SPUDGPU_ADAPTER_PROPERTIES points to". Kind bits for decode and
 * encode are not defined until those queues are built: a bit is appended
 * then, which breaks nothing. */

/*
 * ---------------------------------------------------------------------------
 * (c) Colour space and surface formats: spudgpu.h, spudcore.h
 * ---------------------------------------------------------------------------
 */

/* [ADDED] */
/**
 * @brief How the display reads a swap chain's pixel values: the colour
 * primaries, the transfer function and the range together.
 *
 * Not the format's business, and the two never stand in for each other:
 * - SPUDGPU_FORMAT says how a pixel is laid out and what the GPU does to a
 *   value as a shader reads or writes it. An `_SRGB` format has the GPU
 *   apply the sRGB curve itself (a shader writes linear values, memory
 *   holds encoded ones); it says nothing of what colour the numbers are.
 * - SPUDGPU_COLOR_SPACE says what the compositor and the display take the
 *   numbers in memory to mean: which red 1.0 is (Rec.709 or Rec.2020), how a
 *   number becomes light (the sRGB curve, linear, PQ), and whether values
 *   past 1.0 count.
 * So B8G8R8A8_UNORM and B8G8R8A8_UNORM_SRGB are both SRGB_NONLINEAR: in the
 * first the shader writes encoded values, in the second the GPU encodes
 * them. A surface takes some pairs and not others, which
 * spudgpu_get_surface_formats() reports.
 *
 * Exactly one value; NONE is refused (a zeroed desc has not chosen). The
 * names are Vulkan's:
 * - SRGB_NONLINEAR: Rec.709 primaries, the sRGB curve. Vulkan
 *   `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`; DXGI
 *   `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709`; Metal `kCGColorSpaceSRGB`.
 * - EXTENDED_SRGB_LINEAR: Rec.709 primaries, linear, values outside [0, 1]
 *   kept (1.0 the SDR white). Vulkan
 *   `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT`; DXGI
 *   `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`; Metal
 *   `kCGColorSpaceExtendedLinearSRGB` with `wantsExtendedDynamicRangeContent`.
 * - HDR10_ST2084: Rec.2020 primaries, the PQ curve (SMPTE ST 2084). Vulkan
 *   `VK_COLOR_SPACE_HDR10_ST2084_EXT`; DXGI
 *   `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`; Metal
 *   `kCGColorSpaceITUR_2100_PQ` with `wantsExtendedDynamicRangeContent`.
 * Vulkan's last two need the instance extension
 * `VK_EXT_swapchain_colorspace`, enabled where the loader has it; without it
 * a surface reports SRGB_NONLINEAR only. A value appended later (HLG,
 * Display P3) breaks nothing.
 *
 * Metal today sets no `colorspace` on the layer (nil: no colour matching),
 * so sRGB content shows over-saturated on a P3 display. Tagging
 * SRGB_NONLINEAR is the translation, and a visible change on such a display:
 * a little less saturated, as the other backends already are.
 */
typedef uint32_t SPUDGPU_COLOR_SPACE;
enum {
	SPUDGPU_COLOR_SPACE_NONE = 0,
	SPUDGPU_COLOR_SPACE_SRGB_NONLINEAR = 1,
	SPUDGPU_COLOR_SPACE_EXTENDED_SRGB_LINEAR = 2,
	SPUDGPU_COLOR_SPACE_HDR10_ST2084 = 3,
};

/* [ADDED] */
/* Frozen: an array element, filled in by spudgpu_get_surface_formats(). It never changes; a new need gets a new type. */
typedef struct SPUDGPU_SURFACE_FORMAT {
	/** A back buffer format the surface takes with #color_space. */
	SPUDGPU_FORMAT format;
	/** The colour space the surface takes it in. */
	SPUDGPU_COLOR_SPACE color_space;
} SPUDGPU_SURFACE_FORMAT;

/* [ADDED] */
/**
 * @brief Reports the format and colour space pairs a swap chain on a
 * surface may name.
 *
 * Per surface and per device, as spudgpu_get_surface_present_scalings() is;
 * ask again for a surface made again, and after the window moves to another
 * display (whether a display shows HDR is the display's). Writes at most
 * `capacity` pairs and always sets `*out_count` to how many there are, so
 * a call with `capacity` 0 and `out_formats` NULL asks the count. The order
 * is the API's, and means nothing. A pair whose format has no
 * SPUDGPU_FORMAT is left out.
 * - Vulkan: vkGetPhysicalDeviceSurfaceFormatsKHR, translated.
 * - D3D12: with SRGB_NONLINEAR, B8G8R8A8_UNORM, B8G8R8A8_UNORM_SRGB,
 *   R8G8B8A8_UNORM, R8G8B8A8_UNORM_SRGB, R10G10B10A2_UNORM and
 *   R16G16B16A16_FLOAT; with EXTENDED_SRGB_LINEAR, R16G16B16A16_FLOAT; with
 *   HDR10_ST2084, R10G10B10A2_UNORM, where the window's output reports an
 *   HDR colour space (IDXGIOutput6::GetDesc1). The flip model refuses an
 *   `_SRGB` buffer format, so an `_SRGB` swap chain's buffers are made in
 *   the UNORM format and its back buffer views in the `_SRGB` one (DXGI's
 *   own practice): the same pairs as Vulkan and Metal, where today D3D12
 *   alone fails such a swap chain with SPUDRESULT_API_SPECIFIC_FAILURE. DXGI
 *   checks a colour space only on a swap chain (CheckColorSpaceSupport):
 *   creation checks again, and refuses what DXGI refuses.
 * - Metal: with SRGB_NONLINEAR, BGRA8_UNORM, BGRA8_UNORM_SRGB and
 *   RGBA16_FLOAT; with EXTENDED_SRGB_LINEAR, RGBA16_FLOAT; with
 *   HDR10_ST2084, RGB10A2_UNORM and RGBA16_FLOAT, where the screen's
 *   maximum EDR value is above 1.
 *
 * @param[in]  device      Device a swap chain would be created on.
 * @param[in]  surface     Surface it would present to.
 * @param[in]  capacity    The number of entries of `out_formats`.
 * @param[out] out_formats Receives up to `capacity` pairs; may be NULL when
 *                         `capacity` is 0.
 * @param[out] out_count   Receives the number of pairs the surface takes.
 *
 * @retval SPUD_SUCCESS `*out_count` was written, and `min(capacity,
 *         *out_count)` pairs.
 * @retval SPUDRESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval SPUDRESULT_GPU_INVALID_SURFACE `surface` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_count` is NULL, or
 *         `out_formats` is NULL and `capacity` isn't 0.
 * @retval SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED The device was created without
 *         SPUDGPU_FEATURE_PRESENT.
 * @return SPUDRESULT_API_SPECIFIC_FAILURE if the graphics API fails, or
 *         SPUDRESULT_OUT_OF_MEMORY.
 */
SPUDRESULT spudgpu_get_surface_formats(
    spudgpu_device device,
    spudgpu_surface surface,
    uint32_t capacity,
    SPUDGPU_SURFACE_FORMAT *out_formats,
    uint32_t *out_count);

/* [CHANGED] spudgpu_swap_chain_desc gains, after `format`:
 *
 *	/ ** How the display reads the back buffers: exactly one
 *	 * SPUDGPU_COLOR_SPACE, and with `format` a pair
 *	 * spudgpu_get_surface_formats() reports. Required: NONE is refused.
 *	 * HDR metadata (mastering display, content light levels) is a struct
 *	 * chained later. * /
 *	SPUDGPU_COLOR_SPACE color_space;
 *
 * spudgpu_create_swap_chain() gains, after SPUDRESULT_GPU_INVALID_PRESENT_SCALING
 * and before the surface is read, SPUDRESULT_GPU_INVALID_COLOR_SPACE; and,
 * after SPUDRESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED,
 * SPUDRESULT_GPU_SURFACE_FORMAT_NOT_SUPPORTED. spudgpu_get_swap_chain_desc()
 * reports the colour space as created.
 *
 * [REMOVED] behaviour: the Vulkan backend's format fallback
 * (`spudgpuvulkan___choose_surface_format_internal`), which, when the
 * requested format isn't offered in VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
 * makes the swap chain in B8G8R8A8_SRGB instead: a choice made for the
 * caller, which a pipeline made for the requested format then
 * mismatches. The pair is now taken exactly or refused. */

/* [ADDED] spudcore.h, after SPUDRESULT_GPU_SHADER_LAYOUT_MISMATCH (331,
 * card S27's draft): */
	/** A swap chain desc's color_space is SPUDGPU_COLOR_SPACE_NONE or not
	 * one SPUDGPU_COLOR_SPACE. */
	SPUDRESULT_GPU_INVALID_COLOR_SPACE = 332,
	/** The surface doesn't take the desc's format in its colour space: the
	 * pair is not among what spudgpu_get_surface_formats() reports. */
	SPUDRESULT_GPU_SURFACE_FORMAT_NOT_SUPPORTED = 333,

/*
 * ---------------------------------------------------------------------------
 * (d) Present: spudgpu.h, spudcore.h
 * ---------------------------------------------------------------------------
 */

/* [ADDED] SPUD_STRUCT_TYPE_GPU_PRESENT_DESC registered in spudcore.h's
 * block. */
/**
 * @brief What a present asks beyond the swap chain's current image.
 *
 * Nothing of its own yet: the struct exists so that what a present may later
 * take is chained onto it (a time to present at, when a frame was shown,
 * per-frame HDR metadata).
 */
typedef struct spudgpu_present_desc {
	/** `type` is SPUD_STRUCT_TYPE_GPU_PRESENT_DESC; `next` is NULL (nothing
	 * is chained onto this struct yet). Set by the caller. See
	 * SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
} spudgpu_present_desc;

/* [CHANGED] was `void spudgpu_swap_chain_present(spudgpu_swap_chain)`. */
/**
 * @brief Presents the current back buffer to the screen.
 *
 * As before: after the frame's commands are submitted to
 * spudgpu_swap_chain_desc::queue, ordered against that queue, and a use of
 * it like a submit. Today Vulkan drops vkQueuePresentKHR's result and D3D12
 * Present's HRESULT; this returns them, translated.
 *
 * @param[in] swap_chain Swap chain holding the image to display.
 * @param[in] desc       What the present asks beyond it; required.
 *
 * @retval SPUD_SUCCESS The image was queued for presentation. Vulkan's
 *         VK_SUBOPTIMAL_KHR is a success: the image is presented.
 * @retval SPUDRESULT_GPU_INVALID_SWAP_CHAIN `swap_chain` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_GPU_PRESENT_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_GPU_SWAP_CHAIN_OUT_OF_DATE The surface changed so that
 *         the swap chain can no longer present to it (Vulkan's
 *         VK_ERROR_OUT_OF_DATE_KHR; D3D12 and Metal never return it). Make
 *         the swap chain again.
 * @retval SPUDRESULT_GPU_SURFACE_LOST The surface is gone (Vulkan's
 *         VK_ERROR_SURFACE_LOST_KHR).
 * @retval SPUDRESULT_GPU_DEVICE_LOST The device was lost.
 * @return SPUDRESULT_API_SPECIFIC_FAILURE if the graphics API fails.
 */
SPUDRESULT spudgpu_swap_chain_present(
    spudgpu_swap_chain swap_chain,
    const spudgpu_present_desc *desc);

/* [ADDED] spudcore.h: */
	/** The swap chain can no longer present to its surface; make it again. */
	SPUDRESULT_GPU_SWAP_CHAIN_OUT_OF_DATE = 334,
	/** The surface a swap chain presents to is gone. */
	SPUDRESULT_GPU_SURFACE_LOST = 335,

/* spudgpu_swap_chain_acquire_next_image(), which returns an index and no
 * result (so loses Vulkan's OUT_OF_DATE there too), is not changed here; it
 * is written into SPUDGPU_DEVICE_TODO.md beside this, for whoever next
 * changes acquire. */

/*
 * ---------------------------------------------------------------------------
 * (f) One sync object: spudgpu.h, spudcore.h
 * ---------------------------------------------------------------------------
 */

/* [REMOVED] spudgpu_semaphore, spudgpu_create_semaphore(),
 * spudgpu_destroy_semaphore(),
 * spudgpu_swap_chain_get_image_available_semaphore(),
 * spudgpu_swap_chain_get_render_finished_semaphore(), and from
 * spudgpu_submit_desc `wait_semaphores`, `wait_semaphore_count`,
 * `wait_stage_masks`, `signal_semaphores` and `signal_semaphore_count`.
 * SPUDRESULT_GPU_INVALID_SEMAPHORE (250) is kept unused, its number never
 * reused. A swap chain's own sync stays inside it, reached through
 * spudgpu_submit_desc::swap_chain (Vulkan's binary semaphores, which acquire
 * and present need, are internal to its swap chain as they are today). */

/* [CHANGED] spudgpu_submit_desc gains, where the semaphores were: */
	/**
	 * GPU-to-GPU sync: fences this submission waits on, each until it
	 * reaches its value in #wait_fence_values, before any of its commands
	 * runs. A fence may be waited on by any queue of its device. The
	 * submission that signals a value is submitted before any submission
	 * that waits on it: waiting on a value nothing has been asked to signal
	 * is outside the contract (Vulkan, D3D12 and Metal each allow it, with
	 * rules that differ; a value never signaled stops the queue for good).
	 * Vulkan: a timeline semaphore wait at
	 * VK_PIPELINE_STAGE_ALL_COMMANDS_BIT (the backend submits with
	 * vkQueueSubmit); D3D12: ID3D12CommandQueue::Wait; Metal: see below.
	 */
	spudgpu_fence *wait_fences;
	/** One value per entry of #wait_fences. */
	const uint64_t *wait_fence_values;
	/** Number of entries of #wait_fences and #wait_fence_values. */
	uint32_t wait_fence_count;
/* Metal: a command list's command buffer holds its commands from the
 * moment they are recorded, and encodeWaitForEvent:value: holds back only
 * what is encoded after it. So the waits go in a command buffer of their
 * own, holding nothing else, committed ahead of the lists' buffers on the
 * same queue. Today's semaphore waits are encoded into the first list's
 * buffer at submit (spudgpumetalcommand.m), after its work, and likely
 * hold back none of it; the test below is written to fail on that code.
 * Whether Metal holds later command buffers of a queue behind a waiting
 * one is confirmed on the Mac by that test before this is built; if it
 * doesn't, the wait moves to the start of each list's command buffer,
 * taken lazily at its first recorded command, and the waits are named
 * when the list begins. */
/* `signal_fence` and `signal_fence_value` stay as they are; one fence
 * signaled per submission, waited on by as many submissions and CPU waits
 * as need it. */

/* spudgpu_queue_submit() gains, in order where the semaphore checks were:
 *   @retval SPUDRESULT_GPU_INVALID_FENCE `wait_fence_count` is not 0 and
 *           `wait_fences` or `wait_fence_values` is NULL, or an entry of
 *           `wait_fences` is NULL.
 * A fence of another device is outside the contract, as `signal_fence` of
 * another device is today (no check, no result). */

/*
 * ---------------------------------------------------------------------------
 * ABI breaks (before 1.0; listed by `check.sh abi`)
 * ---------------------------------------------------------------------------
 *
 * - SPUDGPU_ADAPTER_PROPERTIES: `queue_pools` a pointer, not an array of 3.
 * - spudgpu_swap_chain_desc gains `color_space` after `format`.
 * - spudgpu_swap_chain_present takes a desc and returns SPUDRESULT.
 * - spudgpu_submit_desc: the semaphore fields become the fence waits.
 * - spudgpu_semaphore and its four calls removed.
 *
 * Every caller changes in the same change (the callers in the workspace are
 * listed in its ROADMAP.md, not here; one caller alone uses the semaphores
 * and their getters, and moves to `spudgpu_submit_desc::swap_chain`), and
 * spudlib's own tests (`spudgpu_device_test`, `spudgpu_objects_test`, `spud_noalloc_tests`,
 * `spud_misuse_tests` and its generator). The card S10 draft's example of a
 * `void` call reporting through a message (a present) becomes a returned
 * result; its other `void` calls are unchanged.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * - spudgpu_device_test: the pools read through the pointer as they were
 *   through the array (3 on D3D12 and Metal; Vulkan's per family), every
 *   kind in exactly one; the same pointer from two reads of the
 *   properties.
 * - spudgpu_objects_test:
 *   - spudgpu_get_surface_formats: the count with capacity 0, then the
 *     pairs; a capacity of 1 writes one and reports the total; every pair
 *     makes a swap chain (the windowless CAMetalLayer on Metal, the hidden
 *     window elsewhere); a pair not reported is refused with
 *     SURFACE_FORMAT_NOT_SUPPORTED (on Vulkan, where it was replaced by
 *     B8G8R8A8_SRGB before); NONE with INVALID_COLOR_SPACE, before the
 *     surface is read. SRGB_NONLINEAR with B8G8R8A8_UNORM and with
 *     B8G8R8A8_UNORM_SRGB reported on every surface here, D3D12 included,
 *     and an `_SRGB` swap chain read back encoded (a linear 0.5 written,
 *     about 188 read).
 *   - Metal: the layer's colorspace is sRGB after creation.
 *   - Present with a desc returns SPUD_SUCCESS frame after frame; a wrong
 *     type and a chain are refused.
 *   - Two queues of a device: a copy queue's submission signals a fence
 *     to 1, a direct queue's submission waits on it at 1 and reads what the
 *     copy wrote (read back), on every backend; the waits checked in order.
 *     The copy is made long (a large buffer) so a wait that holds nothing
 *     back reads stale data; run against today's Metal semaphore waits
 *     first, where it is expected to fail.
 * - Where a test machine has an HDR display (the MacBook's XDR screen):
 *   HDR10_ST2084 and EXTENDED_SRGB_LINEAR reported and a swap chain made with
 *   each. Windows and Linux: reported only if the display is in HDR.
 * - A window resized under a Vulkan swap chain until present returns
 *   OUT_OF_DATE, where the platform gives one (X11, Win32), the swap chain
 *   made again and presenting after.
 * - spud_misuse_tests: the new calls with zeroes and destroyed handles.
 * - spud_noalloc_tests: the present loop still allocates nothing of ours.
 * Run on macOS, Windows (D3D12, Vulkan, both GPUs) and Linux (Vulkan).
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 * Vulkan: queue families listed by a two-call query; VkSurfaceFormatKHR
 *   pairs a format with a colour space, and a swapchain names both; present
 *   takes VkPresentInfoKHR with pNext (where present timing and ids chain)
 *   and returns OUT_OF_DATE, SURFACE_LOST, DEVICE_LOST; timeline semaphores
 *   waited on by queues. The model for each.
 * D3D12: three queue types, a fence per signal and ID3D12CommandQueue::Wait
 *   (one sync object); a swap chain's colour space set by SetColorSpace1.
 * Metal: CAMetalLayer's colorspace and EDR flag; MTLSharedEvent waited on by
 *   command buffers (one sync object); presentDrawable:atTime: for timing.
 * Rejected: a fixed `queue_pools[8]` (still a guess at drivers' families);
 *   a call of its own for the pools (more change than a pointer, for the
 *   same lifetime); colour space names of SpudGPU's own (SRGB beside an
 *   `_SRGB` format reads as one thing; Vulkan's names don't);
 *   making spudgpu_semaphore a timeline on Vulkan (a second counter beside
 *   the fence, with nothing the fence can't do); a colour space chained
 *   later (absent would mean sRGB).
 */
