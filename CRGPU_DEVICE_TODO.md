# Carrottus GPU adapters and devices TODO

Written on 2026-10-21; the header section agreed and implemented the same
day, in `crgpu.h` and all three backends.

**State (2026-10-22):**

- Metal: built on macOS (Apple M5 Pro), Debug and Release.
  `tests/crgpu_device_test` (new) passes, plain and under the `-asan` and
  `-tsan` presets; the seven samples the Metal build has ran 120 frames each
  in a Debug build, with the live-object check on (2026-10-21).
- D3D12 and Vulkan: run on Windows on 2026-10-22. Every Carrottus GPU sample ran
  on D3D12 (both devices and WARP) and on Vulkan (both devices), under
  validation. That found two Vulkan bugs, fixed in `c3cf5f7`: an image
  barrier out of `UNDEFINED` or `PRESENT_SRC` started at the top or bottom
  of the pipe, not chained to the acquire semaphore's wait; and
  `drawIndirectFirstInstance` was never enabled, now
  `CRGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE`.
- Every caller is converted (`../CRGPU_DEVICE_MIGRATION.md`, 2026-10-22).

The rest of this file is the design as agreed, kept as the record of why.

`crgpu_enumerate_devices` both lists the GPUs and creates a device on every
one of them. Every graphics API Carrottus GPU translates keeps those apart (Vulkan's
`vkEnumeratePhysicalDevices` / `vkCreateDevice`, DXGI's `EnumAdapters1` /
`D3D12CreateDevice`, WebGPU's `requestAdapter` / `requestDevice`), and this
splits them the same way: an **adapter** says what a GPU is and can do and
costs nothing to list; a **device** is created on one, by the caller, with the
features and queues the caller asks for.

## What is wrong now

- **A device on every GPU, made for the caller.** The first
  `crgpu_enumerate_devices` creates a logical device (and on Vulkan the
  bindless table) on every GPU the API lists. On a laptop with an integrated
  and a discrete GPU that powers up the discrete one, and on Vulkan each
  device costs driver memory and threads, whichever one the caller then uses.
- **Features switched on by Carrottus GPU, not the caller.** Vulkan enables
  features at `vkCreateDevice`, and the backend enables everything it finds
  (`shaderOutputLayer`, `depthBiasClamp`, `independentBlend`,
  `multiDrawIndirect`, `imageCubeArray`, depth bounds, mesh shading,
  descriptor indexing). That is a decision made for the caller, against the
  one rule. It also hides mistakes: a caller that uses a feature without
  checking for it works on the machine it was written on, where the GPU
  happens to have it, and fails only on a user's GPU that lacks it. With
  only what was asked for switched on, a forgotten request fails on the
  developer's own machine.
- **No destroy, so no recovery.** Devices belong to the instance and live as
  long as it does. A lost device (a TDR, a driver reset, an eGPU unplugged)
  can only be replaced by destroying the whole instance.
- **Queues fixed by Carrottus GPU.** Vulkan makes one graphics queue; D3D12 four of
  each kind (`CR_D3D12_COMMAND_QUEUE_COUNT_PER_FAMILY`). The caller can't
  ask for a compute or copy queue on Vulkan at all
  (`crgpu_command_queue_get` and `crgpu_get_max_queue_count` are not
  written there).
- **Lazy state that had to be locked.** The device array is made by the first
  enumerate call, which is why `_devices_lock` exists on Vulkan and the
  D3D12 and Metal first calls still race. D3D12's bindless table is made by
  the first bindless call, which is why `_bindless_create_mutex` exists.
- **Capabilities asked of the wrong object.** `crgpu_get_*_capabilities`
  and most of `CRGPU_DEVICE_PROPERTIES` describe the hardware, but are
  asked of a device; once a device exists the question that matters is what
  it was created with.

## What the callers use

Every call this replaces is used outside Carrottus, except
`crgpu_command_queue_get` and `crgpu_get_max_queue_count`, which
nothing calls. Every caller takes `devices[0]` or an index it was given,
and the graphics queue. Which callers use what, what each has to change and
in what order is in the workspace's `../CRGPU_DEVICE_MIGRATION.md`; this
file names none, as Carrottus names no caller.

## The proposal

The rules, then the header section.

- **Listing keeps nothing on an adapter.** The instance lists its adapters
  when it is created (`vkEnumeratePhysicalDevices`, `EnumAdapters1`,
  `MTLCopyAllDevices`) and never changes the list, so every adapter call may
  be made from any thread with no lock. The list is in the API's order,
  unsorted: preferring one GPU is the caller's. D3D12 is the one backend
  that has to create something to list: its features are only answered by a
  device (`CheckFeatureSupport`), so `crgpu_instance_create` creates a
  probe device on each adapter in turn, reads it and releases it before the
  next (see "D3D12's probe device" below). The header says so, so that a
  discrete GPU waking at instance creation has its reason written down.
- **An adapter is listed only if a device can be created on it.** Vulkan:
  the physical device's `apiVersion` is 1.3 or later (unchecked today: a
  1.2 device is listed and fails at `vkCreateDevice`). D3D12: the probe
  device at feature level 12_0 is created. Metal: Metal 3. Software
  adapters (WARP, lavapipe, SwiftShader) are listed and marked; filtering
  them is the caller's. (D3D12 skips WARP today: a choice made for the
  caller, which goes.)
- **The list never changes, and nothing reports a GPU added or removed.**
  A GPU plugged in after the instance was made is not in its list; one
  unplugged leaves an adapter no device can be created on. A new instance
  lists the system as it is then. A polled call that says the list is out
  of date is a later item (see "Later, not in this change").
- **An adapter names its GPU.** `adapter_id` is equal for two adapters, in
  any instances of any processes, only if they are the same GPU; the
  reverse does not hold (a restart, a driver restart or a replug may give
  the GPU a new one), so the caller never stores one. The Windows LUID is a
  separate field, for interop that names an adapter by it. A choice saved
  across runs is matched by the hardware IDs and the description: the
  caller's.
- **Supported is the adapter's, enabled is the device's.** An adapter reports
  `supported_features`; a device has exactly its `required_features`, on
  every backend. Using a feature the device wasn't created with is refused
  with the result it gets today when the device lacks it (such as
  `CR_RESULT_GPU_TIMESTAMPS_NOT_SUPPORTED`), or with
  `CR_RESULT_GPU_FEATURE_NOT_SUPPORTED` where there is none (a swap chain);
  a command a device can't record (a mesh dispatch, a depth bounds call)
  records nothing and asserts in a debug build. This is the header's usual
  answer to APIs that differ underneath: Vulkan needs features switched on,
  D3D12 and Metal don't, and the narrowest rule all three keep is "ask
  first".
- **Vulkan's base set is switched on for every device, and nothing else
  but what is asked for.** Carrottus GPU itself relies on `dynamicRendering`,
  `synchronization2` (barriers), `timelineSemaphore` (`crgpu_fence`) and
  `bufferDeviceAddress` (every buffer has an address); Vulkan 1.3 requires
  all four, so the version check covers them. Everything else, the
  descriptor-indexing bits included (today switched on unconditionally,
  though only the bindless code uses update-after-bind and partially
  bound), is switched on only by the feature bit that needs it.
- **The caller asks for queues.** By kind and count, up to what the adapter
  reports. The device makes them all at creation and owns them;
  `crgpu_command_queue_get` hands them out (and is written for Vulkan).
- **Queues come from pools, and the adapter lists them.** Vulkan makes each
  kind from one family, by the rule in `crgpu_device_desc`, and kinds
  that fall back to one family share its `queueCount` (Intel Iris Xe: one
  family of one queue for all three kinds). Three separate maxima would
  hide that, so the adapter lists pools: the kinds each serves and the most
  queues it holds, and the desc's counts for a pool's kinds add up to at
  most that. D3D12 and Metal report three pools, one kind each, with no
  limit. Each pool says what the API reports about where its queues run:
  Vulkan knows the graphics family from a family without graphics; D3D12
  and Metal say nothing, which is reported as unknown rather than left to
  look like three separate families. No API promises that a queue kind has
  hardware of its own, or runs at the same time as graphics, and the header
  promises nothing either. (Settled 2026-10-21.)
- **Carrottus GPU sets no queue priority.** Vulkan gets the same value for every
  queue, which in its relative scheme is no preference; D3D12 gets
  `NORMAL`, its own no preference. The order work on different queues runs
  in is the driver's. Choosing a priority is a later feature bit (see
  "Later, not in this change"), since Metal has none.
- **One device per adapter at a time.** D3D12 returns the same device to a
  second `D3D12CreateDevice` on one adapter, and Metal has one `MTLDevice`
  per GPU; Vulkan would allow more, but the narrowest rule is one. A flag on
  the adapter, set atomically, is the only state this needs, and it is
  Carrottus GPU's to guard (the adapter is shared, hidden state).
- **One device per GPU in the process, across instances: the caller's
  rule.** Two instances list two adapters for one GPU, and on D3D12 a
  device made through each would be one `ID3D12Device` under two handles.
  Carrottus GPU can't see across instances without process-wide state, which it
  doesn't hold, so the caller keeps this, by `adapter_id`. It matters most
  in a recovery: the lost device is destroyed before a device on the same
  GPU is made in a new instance.
- **Nothing is made later, except what is keyed by a value the caller
  passes at a call or belongs to an object the caller synchronizes.**
  Settled 2026-10-21, against what engines do (Unreal's D3D12 RHI and
  NVRHI set up their descriptor heaps and bindless state at device
  creation):
  - The bindless table is made by `crgpu_device_create` when
    `CRGPU_FEATURE_BINDLESS` is asked for, and not otherwise, on every
    backend. The pointer is set once and is null exactly when the feature
    wasn't asked for. `_devices_lock`, `_bindless_create_mutex` and D3D12's
    atomic `_bindless` go with the lazy state they guarded; the table keeps
    its own lock, since registering is done on any thread.
  - D3D12's shader-visible heaps (`_csv_heap`, `_sampler_heap`) are made by
    `crgpu_device_create`, sized by the descriptor budget below, and not
    made at all for a budget of 0. A failure to make them fails the create
    call, not someone's first pool. `_shader_heap_mutex` stays, guarding the
    free lists only.
  - D3D12's indirect command signatures for tightly packed arguments
    (stride = the size of `crgpu_draw_indirect_args`, and of
    `crgpu_draw_indexed_indirect_args`) are made by
    `crgpu_device_create`, so the common case takes no lock. A signature
    for any other stride is still made by the first draw that uses it,
    under `_indirect_signature_mutex`: it is keyed by the caller's stride,
    and making others ahead would be Carrottus GPU choosing strides. (Engines
    whose APIs fix the stride make every signature at init; a translator
    that takes a stride caches, as vkd3d-proton does the other way round.)
  - A command list's `_rtv_heap` and `_dsv_heap` stay lazy: the caller
    synchronizes the list, so this costs no lock and hides nothing shared.
    Engines write RTVs and DSVs once, at view creation, into a device-level
    CPU heap; that would need a shared allocator and its lock, and is not
    part of this change.
- **The caller sets the device's descriptor budget.** The shader-visible
  heaps' size was Carrottus GPU's choice (a million CBV/SRV/UAV descriptors and
  2048 samplers, made in full on first use), and Unreal and NVRHI both
  leave it to the application. A D3D12 heap size on the desc would be a
  field only one API acts on, so it is a rule all three keep instead: the
  desc's `max_descriptors` and `max_samplers` bound what the device's
  descriptor pools and bindless table hold at once, the adapter reports
  the most allowed, D3D12 sizes its heaps from the budget, and Vulkan and
  Metal count against it. A pool that doesn't fit is refused on every
  backend with `CR_RESULT_GPU_OUT_OF_DESCRIPTORS`. Today only D3D12 refuses
  a pool when the samplers run out (with
  `CR_RESULT_API_SPECIFIC_FAILURE`); this makes it one rule.
- **A debug build counts what is alive on a device.** Destroying a device
  with objects still made from it is the caller's mistake, and the
  graphics APIs report it unevenly (Vulkan's validation layer does; D3D12
  and Metal mostly don't). So a debug build keeps, per device, an atomic
  count of live objects for each kind of handle made from it (buffers,
  images, views, samplers, heaps, shader modules, pipelines, layouts,
  descriptor pools, command allocators and lists, fences, semaphores,
  swap chains, timestamp sets): each create call adds one when it
  succeeds, each destroy call takes one away. Atomic, because objects are
  made and destroyed on any thread. `crgpu_device_destroy` prints every
  kind whose count isn't 0 (`crgpu: device "name" destroyed with 3
  buffers, 1 pipeline alive`) and asserts. What has no destroy call of
  its own (descriptor sets, a swap chain's images and views) is counted
  through its owner, not separately; the queues are the device's own and
  not counted. A release build has no counter. This is the heaps' rule (a
  debug build counts live placements, and `crgpu_heap_destroy` asserts
  the count is 0) applied to the device. The counters are in the backends'
  internal structs only; no public struct changes between builds.
- **A device can be destroyed and made again, and that is the recovery
  from a lost one.** Destroy everything made from the device, then the
  device; create a device on the same adapter. If that fails, for any
  reason, destroy the instance, make a new one, and find the GPU there by
  `adapter_id`, else by the hardware IDs, else choose again. No result
  tells "the adapter is gone" from another failure: D3D12 can't say so
  reliably at `D3D12CreateDevice`, and the caller does the same either
  way. This is WebGPU's rule (an adapter is spent; `requestAdapter` again)
  and DXGI's (`IsCurrent()`, a new factory, enumerate again). It covers a
  hang or reset (TDR), a driver update or restart, and an unplugged GPU.

### The header section

To replace `crgpu.h` from `crgpu_enumerate_devices` through
`crgpu_command_queue_get`, keeping the vendor IDs and
`crgpu_instance_get_native_gpu_api`. It compiles as C11, C23 and C++20 against the
current `crgpu.h` (2026-10-21).

```c
/** Opaque handle to a GPU the graphics API lists: what it is and what it can
 * do. Owned by the instance; listing one creates nothing on it. */
typedef struct crgpu_adapter_t *crgpu_adapter;

/**
 * @brief What kind of GPU an adapter is, as the graphics API reports it.
 */
typedef uint32_t CRGPU_ADAPTER_TYPE;
enum {
	/** The API reports no kind, or one not listed here. */
	CRGPU_ADAPTER_TYPE_OTHER = 0,
	/** A GPU sharing the system's memory and package (Vulkan
	 * `INTEGRATED_GPU`; D3D12 `UMA`; Metal `hasUnifiedMemory` and not
	 * `isRemovable`). */
	CRGPU_ADAPTER_TYPE_INTEGRATED = 1,
	/** A GPU with memory of its own (Vulkan `DISCRETE_GPU`; D3D12 not
	 * `UMA`; Metal not `hasUnifiedMemory`). */
	CRGPU_ADAPTER_TYPE_DISCRETE = 2,
	/** A GPU a hypervisor presents (Vulkan `VIRTUAL_GPU`). */
	CRGPU_ADAPTER_TYPE_VIRTUAL = 3,
	/** A renderer that runs on the CPU (Vulkan `CPU`, such as lavapipe or
	 * SwiftShader; D3D12 `DXGI_ADAPTER_FLAG3_SOFTWARE`, WARP). Listed like
	 * any other: whether to use one is the caller's. */
	CRGPU_ADAPTER_TYPE_SOFTWARE = 4
};

/**
 * @brief Capabilities an adapter may or may not have, which a device has only
 * when it was created with them.
 *
 * A bit set in CRGPU_ADAPTER_PROPERTIES::supported_features says the
 * adapter has it. A device has exactly the features in its
 * crgpu_device_desc::required_features, on every backend: using one the
 * device was not created with is refused with that feature's result, as if
 * the adapter lacked it, even where the graphics API underneath would allow
 * it. Vulkan switches most of these on when its device is created, and some
 * cost something just by being on; D3D12 and Metal have nothing to switch on,
 * and hold the device to the same rule.
 *
 * A feature behind an CRGPU_EXT_* macro that is 0 in this build is never
 * supported. New features are new bits; a bit's meaning never changes.
 * The bits are macros, not an enum: before C23 an enumerator can't go past
 * an int, and the set may grow past 32.
 */
typedef uint64_t CRGPU_FEATURES;

/** crgpu_rendering_begin_desc::layer_count above 0. Was
 * CRGPU_DEVICE_PROPERTIES::layered_rendering. */
#define CRGPU_FEATURE_LAYERED_RENDERING (UINT64_C(1) << 0)
/** crgpu_graphics_pipeline_desc::depth_bias_clamp other than 0;
 * refused with CR_RESULT_GPU_DEPTH_BIAS_CLAMP_NOT_SUPPORTED. */
#define CRGPU_FEATURE_DEPTH_BIAS_CLAMP (UINT64_C(1) << 1)
/** Color attachments that blend differently from each other; refused
 * with CR_RESULT_GPU_INDEPENDENT_BLEND_NOT_SUPPORTED. */
#define CRGPU_FEATURE_INDEPENDENT_BLEND (UINT64_C(1) << 2)
/** An indirect draw of more than one entry per call, up to
 * CRGPU_ADAPTER_PROPERTIES::max_draw_indirect_count. Without it, 1. */
#define CRGPU_FEATURE_MULTI_DRAW_INDIRECT (UINT64_C(1) << 3)
/** CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY views, and shaders that declare a
 * cube array. */
#define CRGPU_FEATURE_CUBE_ARRAY_VIEWS (UINT64_C(1) << 4)
/** crgpu_timestamp_set_create(); refused with
 * CR_RESULT_GPU_TIMESTAMPS_NOT_SUPPORTED. */
#define CRGPU_FEATURE_PASS_TIMESTAMPS (UINT64_C(1) << 5)
/** Buffers of CRGPU_MEMORY_KIND_DEVICE_MAPPABLE; refused with
 * CR_RESULT_GPU_MEMORY_KIND_NOT_SUPPORTED. */
#define CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY (UINT64_C(1) << 6)
/** Heaps of a mappable CRGPU_MEMORY_KIND. Was
 * CRGPU_DEVICE_PROPERTIES::mappable_heaps. */
#define CRGPU_FEATURE_MAPPABLE_HEAPS (UINT64_C(1) << 7)
/** The device's bindless table and every crgpu_bindless_* call
 * (CRGPU_EXT_BINDLESS). The table is made with the
 * device, and only when this is asked for. */
#define CRGPU_FEATURE_BINDLESS (UINT64_C(1) << 8)
/** Mesh-shader pipelines and crgpu_command_list_dispatch_mesh()
 * (CRGPU_EXT_MESH_SHADING). */
#define CRGPU_FEATURE_MESH_SHADING (UINT64_C(1) << 9)
/** The depth bounds test (CRGPU_EXT_DEPTH_BOUNDS_TEST). */
#define CRGPU_FEATURE_DEPTH_BOUNDS_TEST (UINT64_C(1) << 10)
/** crgpu_swap_chain_create(), and crgpu_adapter_supports_surface()
 * answering true; refused with CR_RESULT_GPU_FEATURE_NOT_SUPPORTED.
 * Vulkan: VK_KHR_swapchain. D3D12 and Metal: always supported. A device that
 * never presents (tests, an offline render) need not ask, and a GPU with no
 * display (an NVIDIA card in TCC mode) is still listed for compute. */
#define CRGPU_FEATURE_PRESENT (UINT64_C(1) << 11)

/** The kinds of queue, as bits, for CRGPU_QUEUE_POOL. */
typedef uint32_t CRGPU_QUEUE_KINDS;
#define CRGPU_QUEUE_KIND_DIRECT (1u << 0)
#define CRGPU_QUEUE_KIND_COMPUTE (1u << 1)
#define CRGPU_QUEUE_KIND_COPY (1u << 2)

/**
 * @brief What the graphics API says about where a pool's queues run.
 *
 * No value promises that work on the pool runs at the same time as
 * graphics work: no API promises that.
 */
typedef uint32_t CRGPU_QUEUE_POOL_ENGINE;
enum {
	/** The API doesn't say. D3D12 and Metal: every pool. */
	CRGPU_QUEUE_POOL_ENGINE_UNKNOWN = 0,
	/** The family graphics work runs on (Vulkan: a family with
	 * VK_QUEUE_GRAPHICS_BIT). Always the pool with DIRECT in it. */
	CRGPU_QUEUE_POOL_ENGINE_GRAPHICS = 1,
	/** A family the driver presents as separate from graphics (Vulkan: a
	 * family without VK_QUEUE_GRAPHICS_BIT). */
	CRGPU_QUEUE_POOL_ENGINE_SEPARATE = 2
};

/* Frozen: one pool of queues, as CRGPU_ADAPTER_PROPERTIES lists them. */
typedef struct CRGPU_QUEUE_POOL {
	/** The kinds made from this pool; every kind is in exactly one pool. */
	CRGPU_QUEUE_KINDS kinds;
	/** The most queues of those kinds, together, one device may have: the
	 * desc's counts for them add up to at most this. Vulkan: the family's
	 * `queueCount`. D3D12 and Metal: UINT32_MAX. */
	uint32_t max_queues;
	/** What the API says about where this pool's queues run. */
	CRGPU_QUEUE_POOL_ENGINE engine;
} CRGPU_QUEUE_POOL;

/**
 * @brief What an adapter is and what it can do, as the graphics API reports
 * it.
 *
 * Facts only: nothing here is chosen. Limits that only matter with a feature
 * are chained onto `base.next` (CRGPU_MESH_SHADING_LIMITS,
 * CRGPU_BINDLESS_LIMITS) and filled in only when the adapter supports it.
 */
typedef struct CRGPU_ADAPTER_PROPERTIES {
	/** `type` is CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES; `next` may chain
	 * the limits structs. The caller builds the chain and sets each type;
	 * Carrottus GPU fills in the rest. See CR_STRUCT_BASE. */
	CR_STRUCT_BASE_OUT base;
	/** The adapter's name, null-terminated. */
	char description[128];
	/** PCI vendor ID, e.g. CRGPU_VENDOR_NVIDIA. */
	uint32_t vendor_id;
	/** PCI device ID. */
	uint32_t device_id;
	/** PCI subsystem ID. D3D12 only; 0 on Vulkan and Metal. */
	uint32_t subsystem_id;
	/** PCI revision. D3D12 only; 0 on Vulkan and Metal. */
	uint32_t revision;
	/** Names this adapter's GPU: two adapters with equal ids, from any
	 * instances in any processes, are the same GPU. The reverse does not
	 * hold: a GPU may get a new id when the system restarts, when its driver
	 * is restarted (updated, reinstalled, or reset by the OS), or when it is
	 * plugged in again. Never store one. Opaque: compare all 16 bytes.
	 * Vulkan: `VkPhysicalDeviceIDProperties::deviceUUID`. D3D12:
	 * `DXGI_ADAPTER_DESC3::AdapterLuid`, then zeros. Metal: `registryID`,
	 * then zeros. */
	uint8_t adapter_id[16];
	/** The adapter's Windows LUID, for interop that names an adapter by one
	 * (DXGI, OpenXR's D3D12 binding). Valid only when #luid_valid is true.
	 * D3D12: `AdapterLuid`. Vulkan: `deviceLUID`. Metal: never valid. */
	uint8_t luid[8];
	/** D3D12: always true. Vulkan: `deviceLUIDValid`. Metal: false. */
	bool luid_valid;
	/** What kind of GPU it is. */
	CRGPU_ADAPTER_TYPE adapter_type;
	/** Every CRGPU_FEATURE_* the adapter has. */
	CRGPU_FEATURES supported_features;

	/** As CRGPU_DEVICE_PROPERTIES::dedicated_video_memory today. */
	uint64_t dedicated_video_memory;
	/** As CRGPU_DEVICE_PROPERTIES::dedicated_system_memory today. */
	uint64_t dedicated_system_memory;
	/** As CRGPU_DEVICE_PROPERTIES::shared_system_memory today. */
	uint64_t shared_system_memory;
	/** As CRGPU_DEVICE_PROPERTIES::unified_memory today. A fact about the
	 * memory, not a feature: nothing is switched on by it. */
	bool unified_memory;

	/** As CRGPU_DEVICE_PROPERTIES::uniform_buffer_offset_alignment today. */
	uint32_t uniform_buffer_offset_alignment;
	/** As CRGPU_DEVICE_PROPERTIES::storage_buffer_offset_alignment today. */
	uint32_t storage_buffer_offset_alignment;
	/** The most entries one indirect draw takes on a device created with
	 * CRGPU_FEATURE_MULTI_DRAW_INDIRECT; 1 without it, whatever this says.
	 * Vulkan: `VkPhysicalDeviceLimits::maxDrawIndirectCount`. D3D12 and
	 * Metal: UINT32_MAX. */
	uint32_t max_draw_indirect_count;
	/** The most a device's crgpu_device_desc::max_descriptors may be.
	 * D3D12: 1,000,000, the shader-visible heap's limit below resource binding
	 * tier 3, kept on tier 3 too. Vulkan: UINT32_MAX (it has no device-wide
	 * limit). Metal: to be settled from its argument buffer limits when
	 * written. */
	uint32_t max_descriptors;
	/** The most a device's crgpu_device_desc::max_samplers may be. D3D12:
	 * 2048 (`D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE`). Vulkan:
	 * UINT32_MAX. Metal: to be settled from `maxArgumentBufferSamplerCount`
	 * when written. */
	uint32_t max_samplers;

	/** The pools a device's queues come from, #queue_pool_count of them
	 * (1 to 3). Vulkan: one per family the rule in crgpu_device_desc
	 * picks. D3D12 and Metal: three, one kind each. */
	CRGPU_QUEUE_POOL queue_pools[3];
	/** The number of entries of #queue_pools in use. */
	uint32_t queue_pool_count;
} CRGPU_ADAPTER_PROPERTIES;

/** @brief Mesh shading's limits, chained onto CRGPU_ADAPTER_PROPERTIES.
 * All 0 when the adapter lacks CRGPU_FEATURE_MESH_SHADING. Replaces
 * crgpu_mesh_shading_capabilities. */
typedef struct CRGPU_MESH_SHADING_LIMITS {
	/** `type` is CR_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS. */
	CR_STRUCT_BASE_OUT base;
	/** Most vertices one mesh shader workgroup may emit. */
	uint32_t max_mesh_output_vertices;
	/** Most primitives one mesh shader workgroup may emit. */
	uint32_t max_mesh_output_primitives;
	/** Most invocations in one mesh shader workgroup. */
	uint32_t max_mesh_workgroup_invocations;
} CRGPU_MESH_SHADING_LIMITS;

/** @brief The bindless table's size, chained onto
 * CRGPU_ADAPTER_PROPERTIES. All 0 when the adapter lacks
 * CRGPU_FEATURE_BINDLESS. Replaces crgpu_bindless_capabilities. */
typedef struct CRGPU_BINDLESS_LIMITS {
	/** `type` is CR_STRUCT_TYPE_GPU_BINDLESS_LIMITS. */
	CR_STRUCT_BASE_OUT base;
	/** Number of slots in the sampled image table. */
	uint32_t max_sampled_images;
	/** Number of slots in the storage image table. */
	uint32_t max_storage_images;
	/** Number of slots in the storage buffer table. */
	uint32_t max_storage_buffers;
} CRGPU_BINDLESS_LIMITS;

/**
 * @brief Lists the adapters an instance can create devices on.
 *
 * The list is read when the instance is created and never changes, so this
 * may be called from any thread. It is in the order the graphics API gives
 * (Vulkan's loader, DXGI's `EnumAdapters1`, `MTLCopyAllDevices`): Carrottus GPU
 * neither sorts nor filters it by preference. An adapter is listed only if a
 * device can be created on it; software adapters are listed, with
 * CRGPU_ADAPTER_TYPE_SOFTWARE. No adapter is a count of 0, not a failure.
 *
 * A GPU added after the instance was created is not listed, and one removed
 * stays listed with no device creatable on it; a new instance lists the
 * system as it is then. Listing keeps nothing on an adapter. D3D12 answers
 * an adapter's features only through a device, so on D3D12
 * crgpu_instance_create() creates a device on each adapter in turn and
 * releases it before the next: a discrete GPU may wake briefly then.
 *
 * The adapters and the array belong to the instance and are released by
 * crgpu_instance_destroy(); the caller frees neither.
 *
 * @param[in]  instance           Instance to list adapters of.
 * @param[out] out_adapters       Receives a pointer to the instance's array.
 * @param[out] out_adapters_count Receives the number of adapters in it.
 *
 * @retval CR_SUCCESS Both outputs were written.
 * @retval CR_RESULT_GPU_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_adapters` or
 *         `out_adapters_count` is NULL.
 */
CR_RESULT crgpu_adapter_enumerate(
    crgpu_instance instance,
    const crgpu_adapter **out_adapters,
    uint32_t *out_adapters_count);

/**
 * @brief Reads what an adapter is and can do.
 *
 * May be called from any thread. Creates nothing on the adapter.
 *
 * @param[in]  adapter        Adapter to query.
 * @param[out] out_properties Receives its properties, and those of every
 *                            struct chained onto it.
 *
 * @retval CR_SUCCESS `out_properties` and its chain were filled in.
 * @retval CR_RESULT_GPU_INVALID_ADAPTER `adapter` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_properties` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `out_properties->base.type` is not
 *         CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN A struct in the chain is not
 *         one of the limits structs.
 */
CR_RESULT crgpu_adapter_get_properties(
    crgpu_adapter adapter,
    CRGPU_ADAPTER_PROPERTIES *out_properties);

/**
 * @brief Reports whether a device on an adapter can present to a surface.
 *
 * Vulkan: `vkGetPhysicalDeviceSurfaceSupportKHR` for the family direct
 * queues are made from. D3D12 and Metal: always true. False on every backend
 * for an adapter without CRGPU_FEATURE_PRESENT.
 *
 * @param[in]  adapter       Adapter to ask about.
 * @param[in]  surface       Surface to present to.
 * @param[out] out_supported Receives the answer. False is an answer, not a
 *                           failure.
 *
 * @retval CR_SUCCESS `out_supported` was written.
 * @retval CR_RESULT_GPU_INVALID_ADAPTER `adapter` is NULL.
 * @retval CR_RESULT_GPU_INVALID_SURFACE `surface` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_supported` is NULL.
 * @return CR_RESULT_API_SPECIFIC_FAILURE if the graphics API fails.
 */
CR_RESULT crgpu_adapter_supports_surface(
    crgpu_adapter adapter,
    crgpu_surface surface,
    bool *out_supported);

/**
 * @brief What the caller decides when creating a device. No field has a
 * default: a zeroed desc asks for no features and no queues, which is
 * refused.
 */
typedef struct crgpu_device_desc {
	/** `type` is CR_STRUCT_TYPE_GPU_DEVICE_DESC; `next` is NULL (nothing
	 * is chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** The features the device has (CRGPU_FEATURE_*), each of which the
	 * adapter must support. Anything not asked for here the device lacks. */
	CRGPU_FEATURES required_features;
	/**
	 * How many queues of each kind the device has. For each of the
	 * adapter's CRGPU_ADAPTER_PROPERTIES::queue_pools, the counts of the
	 * kinds it serves add up to at most its `max_queues`. Their sum is at
	 * least 1. Vulkan makes each kind from one queue family: direct queues
	 * from the first family with graphics; compute queues from the first
	 * family with compute and without graphics, else the direct family; copy
	 * queues from the first family with transfer only, else the compute
	 * family. That rule is what the adapter's pools report.
	 *
	 * Carrottus GPU sets no queue priority: Vulkan gets the same value for every
	 * queue, D3D12 `NORMAL`. The order work on different queues runs in is
	 * the driver's.
	 */
	uint32_t direct_queue_count;
	/** As #direct_queue_count, for CRGPU_COMMAND_LIST_TYPE_COMPUTE. */
	uint32_t compute_queue_count;
	/** As #direct_queue_count, for CRGPU_COMMAND_LIST_TYPE_COPY. */
	uint32_t copy_queue_count;
	/**
	 * The most descriptors other than samplers (sampled and storage images,
	 * uniform and storage buffers, texel buffer views) the device's
	 * descriptor pools and bindless table hold at once, together. At most
	 * the adapter's CRGPU_ADAPTER_PROPERTIES::max_descriptors. A combined
	 * image sampler counts one here and one in #max_samplers. A pool that
	 * would go past it is refused with CR_RESULT_GPU_OUT_OF_DESCRIPTORS,
	 * on every backend. 0 is a device with no descriptors: every pool is
	 * refused. With CRGPU_FEATURE_BINDLESS, the table's size
	 * (CRGPU_BINDLESS_LIMITS, all three parts) comes out of it.
	 *
	 * D3D12: the size of the shader-visible CBV/SRV/UAV heap, made with the
	 * device. Vulkan and Metal: a count kept by the device.
	 */
	uint32_t max_descriptors;
	/** As #max_descriptors, for samplers, against
	 * CRGPU_ADAPTER_PROPERTIES::max_samplers. D3D12: the size of the
	 * shader-visible sampler heap. */
	uint32_t max_samplers;
	/** Name for debuggers and validation messages, or NULL. Copied. */
	const char *debug_name;
} crgpu_device_desc;

/**
 * @brief Creates a device on an adapter, with the features and queues the
 * caller asks for.
 *
 * One device per adapter at a time, on every backend: D3D12 hands back the
 * same device to a second `D3D12CreateDevice` on one adapter, and Metal has
 * one `MTLDevice` per GPU, so a second device would not be a separate one
 * there. Two threads creating on one adapter at once get one device and
 * CR_RESULT_GPU_ADAPTER_IN_USE.
 *
 * The rule is one device per GPU in the process, across instances too, and
 * that part is the caller's: two instances list two adapters for one GPU,
 * which Carrottus GPU does not compare. On D3D12 a device made through each would
 * be one native device under two handles. Compare
 * CRGPU_ADAPTER_PROPERTIES::adapter_id.
 *
 * Vulkan: `vkCreateDevice` with the base set Carrottus GPU relies on
 * (`dynamicRendering`, `synchronization2`, `timelineSemaphore`,
 * `bufferDeviceAddress`, all required by Vulkan 1.3) and the extensions and
 * feature bits `required_features` needs, and nothing else. D3D12:
 * `D3D12CreateDevice` at feature level 12_0, and the queues. Metal: the
 * queues on the adapter's `MTLDevice`.
 *
 * @param[in]  adapter    Adapter to create the device on.
 * @param[in]  desc       What the caller decides.
 * @param[out] out_device Receives the new device on success; NULL on
 *                        failure.
 *
 * @retval CR_SUCCESS The device was created.
 * @retval CR_RESULT_GPU_INVALID_ADAPTER `adapter` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_GPU_DEVICE_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_device` is NULL.
 * @retval CR_RESULT_GPU_FEATURE_NOT_SUPPORTED A bit of
 *         `desc->required_features` is not in the adapter's
 *         `supported_features`.
 * @retval CR_RESULT_ZERO_SIZE The three queue counts are all 0.
 * @retval CR_RESULT_GPU_TOO_MANY_QUEUES The counts of the kinds one of
 *         the adapter's pools serves add up to more than its `max_queues`.
 * @retval CR_RESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE `desc->max_descriptors`
 *         or `desc->max_samplers` is above the adapter's limit.
 * @retval CR_RESULT_GPU_OUT_OF_DESCRIPTORS CRGPU_FEATURE_BINDLESS is asked
 *         for and the bindless table doesn't fit in the budget.
 * @retval CR_RESULT_GPU_ADAPTER_IN_USE The adapter already has a device.
 * @return Another CR_RESULT if the backend fails, such as
 *         CR_RESULT_OUT_OF_MEMORY or CR_RESULT_API_SPECIFIC_FAILURE.
 *
 * @see crgpu_device_destroy()
 */
CR_RESULT crgpu_device_create(
    crgpu_adapter adapter,
    const crgpu_device_desc *desc,
    crgpu_device *out_device);

/**
 * @brief Destroys a device and its queues, and frees its adapter for
 * another.
 *
 * Also the way back from CR_RESULT_GPU_DEVICE_LOST:
 *
 * 1. Destroy everything made from the lost device, then the device.
 * 2. Create a device on the same adapter.
 * 3. If that fails, whatever the result, destroy the instance and create a
 *    new one; find the GPU there by CRGPU_ADAPTER_PROPERTIES::adapter_id,
 *    else by its hardware IDs, else choose again. A driver update or an
 *    unplugged GPU leaves the old adapter unusable; a hang or reset usually
 *    does not.
 *
 * @warning Every object created from the device must be destroyed first, and
 * every submission to its queues must have finished (or the device be lost).
 * No other thread may be using it. A debug build checks the first: it prints
 * the kinds of object still alive, with their counts, and asserts.
 *
 * @param[in] device Device to destroy. NULL does nothing.
 */
void crgpu_device_destroy(crgpu_device device);

/**
 * @brief Returns the adapter a device was created on.
 *
 * @param[in] device Device to query.
 *
 * @return The adapter, or NULL if `device` is NULL.
 */
crgpu_adapter crgpu_device_get_adapter(crgpu_device device);

/**
 * @brief Returns the features a device was created with: its
 * crgpu_device_desc::required_features.
 *
 * @param[in] device Device to query.
 *
 * @return The features, or 0 if `device` is NULL.
 */
CRGPU_FEATURES crgpu_device_get_features(crgpu_device device);

/**
 * @brief Returns one of the queues a device was created with.
 *
 * The device owns the queue; it is not destroyed by the caller. The same
 * kind and index always give the same queue. Replaces
 * crgpu_get_graphics_queue(), which is (DIRECT, 0).
 *
 * @param[in]  device    Device to get the queue from.
 * @param[in]  type      Kind of queue: DIRECT, COMPUTE or COPY.
 * @param[in]  index     Which queue of that kind, below the count the device
 *                       was created with.
 * @param[out] out_queue Receives the queue on success.
 *
 * @retval CR_SUCCESS `out_queue` was written.
 * @retval CR_RESULT_GPU_INVALID_DEVICE `device` is NULL.
 * @retval CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE `type` is BUNDLE, or not a
 *         kind of command list.
 * @retval CR_RESULT_INDEX_OUT_OF_RANGE `index` is not below the count the
 *         device was created with for `type`.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_queue` is NULL.
 */
CR_RESULT crgpu_command_queue_get(
    crgpu_device device,
    CRGPU_COMMAND_LIST_TYPE type,
    uint32_t index,
    crgpu_command_queue *out_queue);
```

### What goes, and what replaces it

| Now | Then |
|---|---|
| `crgpu_enumerate_devices` | `crgpu_adapter_enumerate`, then `crgpu_device_create` |
| `CRGPU_DEVICE_PROPERTIES`, `crgpu_get_device_properties` | `CRGPU_ADAPTER_PROPERTIES`, `crgpu_adapter_get_properties(crgpu_device_get_adapter(device), ...)` |
| its `layered_rendering`, `depth_bias_clamp`, `independent_blend`, `pass_timestamps`, `cube_array_views`, `device_mappable_memory`, `mappable_heaps` | `CRGPU_FEATURE_*` bits |
| its `subSys_id` | `subsystem_id` |
| `crgpu_get_bindless_capabilities`, `crgpu_bindless_capabilities` | `CRGPU_FEATURE_BINDLESS` and `CRGPU_BINDLESS_LIMITS` |
| `crgpu_get_mesh_shading_capabilities`, `crgpu_mesh_shading_capabilities` | `CRGPU_FEATURE_MESH_SHADING` and `CRGPU_MESH_SHADING_LIMITS` |
| `crgpu_get_depth_bounds_capabilities`, `crgpu_depth_bounds_capabilities` | `CRGPU_FEATURE_DEPTH_BOUNDS_TEST` |
| `crgpu_get_graphics_queue` | `crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue)` |
| `crgpu_get_max_queue_count` | the count the device was created with |
| `crgpu_get_vk_physical_device(device)` | `crgpu_get_vk_physical_device(adapter)`; D3D12 gains `crgpu_get_dxgi_adapter(adapter)`, Metal `crgpu_get_mtl_adapter_device(adapter)` |

### Build, GPU, device: EXT macros, feature bits and properties

Settled 2026-10-21. Three questions, each with its own answer in the
header, side by side:

| | Question | Known | If no |
|---|---|---|---|
| `CRGPU_EXT_*` macro | Does the backend in this build implement it? | at compile time | its functions and types don't exist: a compile error |
| bit in the adapter's `supported_features` | Does this GPU support it? | when the instance lists its adapters | the bit can't be asked for |
| bit in the device's features | Did the caller ask for it? | when the device is created | the call is refused |
| property | A fact about the GPU | when the instance lists its adapters | nothing is refused; the caller reads it |

**A feature or a property.** A capability that some call refuses without it
is a feature: a `CRGPU_FEATURE_*` bit the caller asks for when creating the
device. A fact that makes no call refuse is a property: a field of
`CRGPU_ADAPTER_PROPERTIES` the caller reads. So `DEVICE_MAPPABLE_MEMORY`
and `MAPPABLE_HEAPS` are bits although no API switches them on (a buffer or
heap of that kind is refused without them), and `unified_memory` is a
property (it only says which path is faster). A limit that only matters
with a feature is a property in that feature's chained limits struct, and
counts only on a device created with the feature. What Carrottus GPU itself
relies on (Vulkan's base set) is neither: it is required of an adapter.
Only the capabilities Carrottus GPU already tracks are bits, plus PRESENT; a new
one is sorted by this rule when it is added.

**Three cases for a capability:**

- *Only the backend varies*: an EXT macro, no bit. Bundles: Metal doesn't
  implement them, and every Vulkan and D3D12 GPU has them.
- *Only the GPU varies*: a bit, no EXT macro. Timestamps, layered
  rendering, PRESENT, depth bias clamp.
- *Both vary*: an EXT macro and a bit. Bindless: Metal doesn't implement
  it, and Vulkan and D3D12 GPUs differ (D3D12 needs binding tier 3).

**How they connect in the header:**

- The feature bits are always defined, never inside an `#if`: they are
  numbers. A bit whose EXT macro is 0 is never in any adapter's
  `supported_features`, so portable caller code asking for it compiles on
  every backend and asks for nothing there.
- An EXT group's functions and its limits struct stay inside its `#if`, as
  the capability structs are now. Their `CR_STRUCT_TYPE` values are in
  `crcore.h` unconditionally, as every struct type is.
- An EXT's result keeps its number and widens: `CR_RESULT_GPU_EXT_<NAME>_NOT_SUPPORTED`
  means the device wasn't created with the feature, which covers a GPU
  that lacks it, since that one can't be asked.
- Every EXT macro's comment says what it means and what it doesn't: "1
  when the backend compiled into this build implements <name>, 0
  otherwise. It says nothing about any GPU: whether one has it is
  CRGPU_FEATURE_<NAME> in its adapter's supported_features, and whether
  a device may use it is that bit in the device's features." (Every EXT
  macro, Carrottus GPU's and the other modules', gets its own version of the
  first sentence; the second only where a feature bit pairs with it.) The
  `EXT` prefix stays: it is the convention across Carrottus, and callers'
  own rules may name it.
- **Bindless is renamed so one name runs through it.** "Descriptor
  indexing" is Vulkan's word (D3D12 says binding tier 3, Metal argument
  buffers), and the bit and limits say "bindless":
  `CRGPU_EXT_BINDLESS_DESCRIPTOR_INDEXING` becomes `CRGPU_EXT_BINDLESS`,
  and `CR_RESULT_GPU_EXT_BINDLESS_DESCRIPTOR_INDEXING_NOT_SUPPORTED`
  becomes `CR_RESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED`, same value (325).
  Mesh shading and the depth bounds test already share one name across
  macro, bit and result.
- `CRGPU_EXT_MESH_SHADING` and `CRGPU_EXT_DEPTH_BOUNDS_TEST` are 1 on
  every backend, so they gate nothing today; both are capabilities of the
  second case in practice. They stay, as markers for a backend that may
  lack them (an OpenGL one, see `CLAUDE.md`); dropping them would be its
  own cleanup.

**The four cases, as a caller writes them:**

1. Only the backend varies (bundles): a compile-time choice, nothing asked
   for.

   ```c
   #if CRGPU_EXT_BUNDLES
   	crgpu_command_list_execute_bundle(cmd, static_scene_bundle);   /* recorded once */
   #else
   	record_static_scene(cmd);                               /* recorded every frame */
   #endif
   ```

2. Only the GPU varies (timestamps): no `#if`; asked for, then checked.

   ```c
   CRGPU_ADAPTER_PROPERTIES props = {.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES};
   crgpu_adapter_get_properties(adapter, &props);

   crgpu_device_desc desc = {.base.type = CR_STRUCT_TYPE_GPU_DEVICE_DESC};
   desc.required_features = props.supported_features & CRGPU_FEATURE_PASS_TIMESTAMPS;
   /* ... queues, budget ... */
   crgpu_device_create(adapter, &desc, &device);

   if (crgpu_device_get_features(device) & CRGPU_FEATURE_PASS_TIMESTAMPS)
   	crgpu_timestamp_set_create(device, queue, 2 * pass_count, &timestamps);
   else
   	timestamps = NULL;   /* "not available" */
   ```

   Without the bit asked for, `crgpu_timestamp_set_create` is refused even
   on a GPU that has timestamps: a forgotten request fails on the machine it
   was written on, not on a user's GPU that lacks the feature.

3. Both vary (bindless): asking compiles everywhere; using it is inside the
   `#if`.

   ```c
   /* On Metal, supported_features never has the bit: this asks for nothing. */
   desc.required_features = props.supported_features & CRGPU_FEATURE_BINDLESS;
   crgpu_device_create(adapter, &desc, &device);

   bool bindless = false;
   #if CRGPU_EXT_BINDLESS
   bindless = (crgpu_device_get_features(device) & CRGPU_FEATURE_BINDLESS) != 0;
   #endif

   if (bindless) {
   #if CRGPU_EXT_BINDLESS
   	uint32_t index;
   	crgpu_bindless_register_sampled_image(device, albedo_view, &index);
   	material.albedo_index = index;
   #endif
   } else {
   	write_material_descriptor_set(device, &material, albedo_view);   /* every backend */
   }
   ```

   Its limits are `CRGPU_BINDLESS_LIMITS`, chained onto the adapter
   properties, inside the same `#if`.

4. A property (`unified_memory`): no `#if`, no bit, nothing asked for.

   ```c
   CRGPU_ADAPTER_PROPERTIES props = {.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES};
   crgpu_adapter_get_properties(crgpu_device_get_adapter(device), &props);

   if (props.unified_memory)
   	write_vertices_directly(upload_buffer, vertices);           /* one copy */
   else
   	stage_then_copy(cmd, staging, device_buffer, vertices);     /* CPU, staging, GPU */
   ```

New `CR_RESULT`s, in the unused 211-219:
`CR_RESULT_GPU_INVALID_ADAPTER` (211), `_FEATURE_NOT_SUPPORTED` (212),
`_TOO_MANY_QUEUES` (213), `_ADAPTER_IN_USE` (214), `_DEVICE_LOST` (215,
for the step below), `_OUT_OF_DESCRIPTORS` (216: a pool, or the bindless
table, past the device's budget, on every backend) and
`_DESCRIPTOR_BUDGET_TOO_LARGE` (217). New `CR_STRUCT_TYPE`s:
`_GPU_ADAPTER_PROPERTIES` (1026), `_GPU_DEVICE_DESC` (1027),
`_GPU_MESH_SHADING_LIMITS` (1028), `_GPU_BINDLESS_LIMITS` (1029). The
retired types (1000, 1017, 1020, 1021) are never reused.
`CR_RESULT_GPU_DEVICE_ENUMERATION_FAILURE` becomes what
`crgpu_instance_create` returns when the API can't list its GPUs; no
adapter is a count of 0, not a failure.

### What a caller looks like

```c
const crgpu_adapter *adapters;
uint32_t adapter_count;
crgpu_adapter_enumerate(instance, &adapters, &adapter_count);
if (adapter_count == 0) /* no GPU: the caller's to handle */;

CRGPU_MESH_SHADING_LIMITS mesh = {.base.type = CR_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS};
CRGPU_ADAPTER_PROPERTIES props = {.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES,
                                    .base.next = &mesh.base};
crgpu_adapter_get_properties(adapters[0], &props);

crgpu_device_desc desc = {.base.type = CR_STRUCT_TYPE_GPU_DEVICE_DESC};
desc.required_features = props.supported_features &
    (CRGPU_FEATURE_MESH_SHADING | CRGPU_FEATURE_PASS_TIMESTAMPS);
desc.direct_queue_count = 1;

crgpu_device device;
crgpu_device_create(adapters[0], &desc, &device);
crgpu_command_queue queue;
crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue);
/* ... */
crgpu_device_destroy(device);
crgpu_instance_destroy(instance);
```

### Each backend

| | Adapter | `crgpu_device_create` | Features |
|---|---|---|---|
| Vulkan | `VkPhysicalDevice`, properties and features read at instance creation | `vkCreateDevice`: the queue families the desc's counts need, the base set, and only the extensions and feature bits `required_features` names (`VK_KHR_swapchain` for PRESENT, the descriptor-indexing bits for BINDLESS); then the queues, and the bindless table if asked | each bit maps to the `VkPhysicalDeviceFeatures*` member or extension it needs today |
| D3D12 | `IDXGIAdapter4`, with `DXGI_ADAPTER_DESC3` and the `CheckFeatureSupport` answers read through a probe device, released before the next adapter's | `D3D12CreateDevice` at 12_0, the queues, the debug callback, the shader-visible heaps sized by the budget, the tightly packed indirect signatures, and the bindless range if asked | nothing to switch on; the device checks its bits |
| Metal | `id<MTLDevice>` from `MTLCopyAllDevices` | the queues, and the bindless argument-buffer state if asked | nothing to switch on; the device checks its bits |

#### D3D12's probe device

Settled 2026-10-21. `DXGI_ADAPTER_DESC3` gives the name, IDs, memory sizes,
LUID and the software flag with no device, but every answer that becomes
`supported_features` (`OPTIONS`, `OPTIONS2`, `OPTIONS7`, the resource
binding tier, `ARCHITECTURE1`) is a `CheckFeatureSupport` call, a device
method; no device-free source has them. Probing on the first properties
call instead would bring back lazy state and a lock and save nothing, since
a caller choosing a GPU asks about all of them; keeping the probe for
`crgpu_device_create` would hold a device on every adapter, the first
complaint above. So:

1. `crgpu_instance_create`, per adapter in `EnumAdapters1` order:
   `GetDesc3`; `D3D12CreateDevice` at 12_0 into a local `ComPtr` (failure:
   the adapter isn't listed); the `CheckFeatureSupport` calls; release.
   One probe at a time, never all at once. The separate
   `D3D12CreateDevice(..., nullptr)` test goes: the probe answers it.
2. The probe is fully released before `crgpu_device_create` can run.
   D3D12 hands back the one live device per adapter, so a probe still held
   would become the caller's device, with what it carried.
3. The debug callback is registered by `crgpu_device_create` only, never
   on the probe.
4. The debug layer is switched on before the first probe, in
   `crgpu_instance_create`, where it is now (until the later item on
   validation moves the choice to the caller).
5. Software adapters (WARP) are listed, as CRGPU_ADAPTER_TYPE_SOFTWARE.
6. The header says what listing costs on D3D12 (in
   crgpu_adapter_enumerate()'s comment above).

A device made twice on the adapter the caller uses is the price, once per
run or per recovery. Measure instance creation on the Iris Xe and the RTX
A2000 when the D3D12 work is done.

### Device loss, a step after

`CR_RESULT_GPU_DEVICE_LOST` is only useful if every call that can see it
returns it. Today `crgpu_swap_chain_present` and `crgpu_command_queue_wait_idle`
return nothing and `crgpu_swap_chain_acquire_next_image` returns an index.
After this change: those return a `CR_RESULT` (acquire gives its index
through an output), and submit, present, acquire, wait-idle and
`crgpu_fence_wait` return `CR_RESULT_GPU_DEVICE_LOST` when the API
says so (`VK_ERROR_DEVICE_LOST`, `DXGI_ERROR_DEVICE_REMOVED` /
`_RESET`, `MTLCommandBufferErrorDeviceRemoved`). A separate change, since it
touches the frame loop of every caller.

## Open questions

- **`crgpu_get_graphics_queue`.** Decided: removed. It was
  `(DIRECT, 0)`; the callers ask `crgpu_command_queue_get` for that.
- **A migration shim.** A `crgpu_enumerate_devices` kept for a while, that
  creates a device on adapter 0 with every supported feature, would let the
  callers move one at a time. Proposed: no shim; change the callers in the
  same change, as the version policy allows before 1.0.0.

## A lost device (added 2026-10-30, for a caller's frame wait)

`CR_RESULT_GPU_DEVICE_LOST` (253): the device is lost, and nothing more
it runs completes. Returned by `crgpu_fence_wait` and the three
submissions on Vulkan (`VK_ERROR_DEVICE_LOST`) and D3D12
(`GetDeviceRemovedReason` asked before and after a fence wait, and around a
submission; a removed device signals every fence to UINT64_MAX, so a wait
would otherwise end as if it had succeeded). Never on Metal: Apple Silicon
has no lost device (`MTLCommandBufferErrorDeviceRemoved` can't occur there,
deprecated in the macOS 27 SDK), and a command buffer that faults fails
alone. Written on macOS 2026-10-30 and not compiled: there are no Vulkan or
D3D12 headers on that machine. Left: compile and run it on Windows (D3D12,
Vulkan) and Linux (Vulkan); nothing forces a device to be lost, so the
runs check the build and that nothing else changed. The present and the
acquire (`void`, and a `UINT32_MAX` index) still can't say they failed for
this reason: a change of their signatures, not made.

## The largest 2D texture (card S33, decided 2026-11-02 with you: a)

`CRGPU_ADAPTER_PROPERTIES` gains the largest width or height of a 2D
texture the adapter creates, in texels: Vulkan's
`VkPhysicalDeviceLimits::maxImageDimension2D`, D3D12's 16384 (feature level
11_0 and up, `D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION`), Metal's from the
GPU's family (Apple's feature set tables). A translation, no policy: a
caller compares its own sizes against it. Built when a caller first needs
it, with the struct's ABI break listed then. Not built.

## Later, not in this change

- **A polled "the adapters changed" call.** Recovery by a new instance is
  the common practice (WebGPU, DXGI's guidance, Chromium) and most engines
  stop there. Notification is for a caller that wants a GPU attached while
  it runs, or must let go of one being ejected. The shape, so it is decided:

  ```c
  /* Whether the system's GPUs differ from those this instance listed when
   * it was created. True changes nothing in the instance: the caller makes
   * a new one when it chooses, and finds what changed by adapter_id. */
  CR_RESULT crgpu_instance_adapters_changed(crgpu_instance instance,
                                               bool *out_changed);
  ```

  D3D12: `IDXGIFactory1::IsCurrent()`. Metal: an atomic flag the
  `MTLCopyAllDevicesWithObserver` block sets (Metal runs it on its own
  queue; Carrottus GPU starts no thread). Vulkan has nothing; the OS does: a
  udev monitor on Linux, `CM_Register_Notification` with
  `GUID_DEVINTERFACE_DISPLAY_ADAPTER` on Windows, read at each call, as
  Carrottus Input reads inotify at each poll. A waitable form (an fd or event for
  a wait set) only if a caller needs it. Build it when a caller asks.
- **Queue priority, an optional feature bit.** Vulkan (a relative float
  per queue, at least two distinct levels) and D3D12 (`NORMAL`, `HIGH`)
  have a priority within the device, both hints; Metal has none. So it is
  a capability, not a rule: `CRGPU_FEATURE_QUEUE_PRIORITY`, supported on
  Vulkan and D3D12 and never on Metal, and with it a struct chained onto
  `crgpu_device_desc` giving each queue `NORMAL` or `HIGH` (two levels,
  what every API with priorities guarantees). The chained struct without
  the bit is refused. A bit and not an `CRGPU_EXT_*` macro: it is a desc
  field, and a public struct holds no `#if`. Priority against other
  processes (`VK_KHR_global_priority`, D3D12's `GLOBAL_REALTIME`), which
  may need privileges, is a second bit with its own refusal for them. Build
  when a caller asks; one whose uploads on a copy queue shouldn't slow its
  frame is the likely first.
- **Validation is the caller's choice.** Both backends switch it on by
  themselves in a debug build: the D3D12 debug layer
  (`crgpud3d12context.cpp`, `EnableDebugLayer` in
  `crgpu_instance_create`) and `VK_LAYER_KHRONOS_validation`
  (`crgpuvulkancontext.c`). That is a decision made for the caller, and on
  D3D12 a process-wide one: switching the layer on removes every D3D12
  device the process already has. It becomes a field of
  `crgpu_instance_desc` (off, on, on with GPU-based validation), with no
  default, in every build. Its own change. **Decided 2026-10-28 with the
  user (card S3): build it now**, before 1.0, with every caller in the
  workspace in the same change. It needs Windows runs for D3D12 and Vulkan
  (and a Linux one for Vulkan); Metal's half maps to its API validation
  and shader validation where the process allows.
- **Carrottus GPU's own checks and where messages go. Decided 2026-10-28 with
  the user (card S2):** an opt-in check layer, built with S3 in the same
  change. Unreal's RHI validation (`-rhivalidation`) is the model: checks
  of Carrottus GPU's own rules on top of the native layers, which can't see them.
  - **One callback for every message.** `crgpu_instance_desc` takes a
    message callback and its context. A message carries a severity, a
    source (Carrottus GPU's own check, the native layer, GPU-based validation),
    an id, the text and the object's debug name (card S10, 2026-11-01,
    adds platform failures to it, the call's name and the platform's code,
    and accepts the callback with validation off;
    `cr_platform_error_draft.h`, approved). D3D12's info queue,
    Vulkan's debug messenger (none is registered today, so its layer
    prints on its own) and Metal's reports all go to it, and the `printf`
    in `crgpu_d3d12_debug_message` goes. Logging, breaking or asserting
    is the caller's choice.
  - **Levels are S3's field:** off, on (the native layer plus Carrottus GPU's
    own checks), on with GPU-based validation. Validation on with no
    callback is refused at creation, so no message is dropped unseen.
  - **In every build, switched at runtime:** off costs one predictable
    branch per call; Debug and Release don't differ, so a Release-only bug
    can be validated. The `void` calls stay `void` (option (c), results on
    every call, was turned down: it taxes every draw).
  - **What Carrottus GPU checks:** destroyed or foreign handles (another
    device's), recording outside begin and end, bindings past the
    descriptor budget, an EXT used without its feature bit, overlapping
    calls on one object (X3's Debug check, here for Carrottus GPU), and barrier
    states where cheap to track.
  - **A misuse test harness:** each misuse made on purpose, the test
    asserting the callback got the expected message id, on all three
    backends (`ROADMAP.md`'s misuse harness, extended to the `void`
    calls).

- **Dynamic uniform offsets. Decided 2026-10-28 with the user (card A3):**
  binding a uniform buffer from a byte offset given per draw, so a caller
  can write per-draw data into one buffer and point each draw at its part:
  Vulkan's dynamic offsets, a root CBV's GPU address on D3D12, a buffer
  offset on Metal. A translation, its own change, with a sample or test on
  all three backends. Within the rules because it only translates:
  Carrottus GPU keeps no ring buffer and hands out no offsets (that is the
  caller's memory policy); it reports the offset alignment the device
  requires and the most dynamic bindings a layout may have (D3D12's root
  signature is small), and refuses a misaligned offset or one too many,
  reported through S2's callback, never rounding.
- **MSAA and resolve. Decided 2026-10-28 with the user (card A4):** a
  translation of what all three APIs have. Not started.

  ```c
  /* Which sample counts a format supports: sample_count_mask in
   * crgpu_format_properties (card A13, below), bit 0 = 1 sample,
   * bit 1 = 2, bit 2 = 4, bit 3 = 8. Translates
   * vkGetPhysicalDeviceImageFormatProperties, D3D12's
   * CheckFeatureSupport(MULTISAMPLE_QUALITY_LEVELS) and Metal's
   * supportsTextureSampleCount:. Never picks one. */

  typedef struct crgpu_image_desc {
      /* ...existing fields... */
      uint32_t sample_count;          /* 1, 2, 4 or 8; one the query reported */
  } crgpu_image_desc;

  /* New usage bit: the image lives only within a render pass and is never
   * stored (Metal memoryless, Vulkan TRANSIENT_ATTACHMENT with lazily
   * allocated memory, D3D12: an ordinary texture). */
  CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT

  typedef struct crgpu_color_attachment_desc {
      /* ...existing view, load_op, store_op, clear... */
      crgpu_image_view   resolve_view;   /* NULL: no resolve */
      CRGPU_RESOLVE_MODE resolve_mode;   /* AVERAGE for color */
  } crgpu_color_attachment_desc;

  typedef struct crgpu_graphics_pipeline_desc {
      /* ...existing... */
      uint32_t sample_count;          /* must match the pass's targets */
      bool     alpha_to_coverage;     /* soft edges for foliage, cutouts */
  } crgpu_graphics_pipeline_desc;
  ```

  The depth attachment takes a resolve view and a mode too (SAMPLE_ZERO,
  MIN, MAX: Vulkan's depth resolve, D3D12's `ResolveSubresourceRegion`
  modes, Metal's depth resolve filter), each reported per device. Names
  are illustrative; the field placement follows the headers when built.
  A sample per backend, its readback checked.
- **Stencil test state. Decided 2026-10-28 with the user (card A5):**
  `crgpu_graphics_pipeline_desc` takes a stencil state - enable, compare
  function, pass, fail and depth-fail operations for front and back faces,
  read and write masks - and `crgpu_command_list_set_stencil_reference()` sets
  the reference per draw (`vkCmdSetStencilReference`,
  `OMSetStencilRef`, `setStencilReferenceValue:`). Depth-stencil formats
  supported are reported per device, never chosen (no D24_S8 on Apple
  GPUs; D32_FLOAT_S8 is). A translation; not started. A sample per
  backend, its readback checked.
- **Read-only depth layouts. Decided 2026-10-28 with the user (card
  A11):** `CRGPU_IMAGE_LAYOUT` gains read-only depth and depth-stencil
  layouts (and the mixed ones: depth read-only with stencil written, and
  the reverse), so a pass can test against a depth image and sample it:
  Vulkan's `DEPTH_READ_ONLY_OPTIMAL` and its stencil variants, D3D12's
  `DEPTH_READ` combined with shader-resource, and on Metal, which has no
  layouts, what Metal's own rules require. A translation; not started.
- **What a format supports. Decided 2026-10-28 with the user (card
  A13):** one query per device and format, taking in A4's sample counts.
  Not started.

  ```c
  typedef struct crgpu_format_properties {
      bool     sampled, filterable, color_target, blendable,
               depth_stencil_target, storage_read, storage_write;
      uint32_t sample_count_mask;   /* card A4's bit mask */
  } crgpu_format_properties;

  CR_RESULT crgpu_get_format_properties(crgpu_device device,
                                           CRGPU_FORMAT format,
                                           crgpu_format_properties *out);
  ```

  Vulkan's `vkGetPhysicalDeviceFormatProperties` and image format
  properties, D3D12's `CheckFeatureSupport(FORMAT_SUPPORT)` and
  multisample levels, and on Metal the GPU family's format table (Metal
  has no query, so the backend carries Apple's published table per
  family). A struct base when written, as Carrottus GPU's other structs.
- **Indirect dispatch. Decided 2026-10-28 with the user (card A15):**
  compute dispatch (and mesh-shader dispatch where the device has mesh
  shading) with the group counts read from a buffer:
  `vkCmdDispatchIndirect` / `vkCmdDrawMeshTasksIndirectEXT`, D3D12's
  `ExecuteIndirect` with a dispatch signature, Metal's
  `dispatchThreadgroupsWithIndirectBuffer:`. A translation; not started.
- **Bindless on Metal. Decided 2026-10-28 with the user (card A15):**
  `CRGPU_EXT_BINDLESS` on the Metal backend through argument buffers
  tier 2 (Apple Silicon), reported per device as the other backends'; then
  it is no longer compiled out on Metal. Not started.
- **Each API's own shader format; no compiler in Carrottus. Decided
  2026-10-28 with the user** (challenging card A9's "SPIR-V only").
  Today the D3D12 backend converts SPIR-V with SPIRV-Cross to HLSL and
  compiles it with DXC at run time, and the Metal backend converts it to
  Metal source and has Metal compile it: two compiles per pipeline at run
  time, DXC and SPIRV-Cross shipped in every application, options chosen
  inside Carrottus (a shader model, a register mapping) that are hidden
  policy (card S2's wording), native features SPIR-V can't express out of
  reach, and generated code in the platform's shader debugger.
  - `crgpu_shader_module_desc` names its format: SPIR-V (Vulkan), DXIL
    (D3D12), a `.metallib` or Metal source (Metal). A device reports the
    formats it takes and refuses another. Carrottus GPU compiles nothing:
    SPIRV-Cross and DXC leave its backends.
  - Cross-compilation is the caller's, at build time or with a runtime
    module of its own. As Unreal and bgfx ship native binaries.
  - Reflection the backends do today (D3D12's register mapping from
    SPIR-V) comes from the caller with the module, or from the native
    format's own reflection.
  - Every caller changes in the same change, before 1.0. Not started; the
    source language is Slang (card A29, decided 2026-10-31, in the
    workspace's `DECISIONS.md`).
  - **Decided 2026-10-31 with the user:** `crgpu_shaderformat_draft.h`
    as written (F1-F7), with cards S17, S19 and S20, each option a. A
    module desc names its format (one per backend, reported in
    `CRGPU_ADAPTER_PROPERTIES::shader_formats`), its entry point
    (required, checked on every backend) and, for compute, mesh and task,
    its workgroup size; the pipelines' entry point fields go. Each
    backend's binding layout is stated in the header and kept by the
    caller's compiler (S17). Clip space is +Y up on every backend, Vulkan
    by a negative viewport height (S19). A storage view of a cube is a 2D
    array view (F6). The Metal fallback's cross-compiler is a build-time
    tool in this repository's `tools/`, never part of the library (S20).
    Building found F4's sentence on Slang's own Metal output wrong: card
    S21.
  - **Decided 2026-10-31 with the user (cards S21, S22):** every shader's
    .metallib comes from `tools/crgpu_msl`'s source for now, Slang's own
    Metal output kept beside it, and a shader moves to Slang's route
    (`METAL_ROUTE SLANG`, with its reason) once Slang can write the layout
    from the sources the other formats take. `shaderDrawParameters` is in
    the Vulkan backend's base set (an adapter without it isn't listed), and
    a vertex shader's vertex and instance index are defined only for draws
    starting at vertex 0 and instance 0 (`crgpu.h`).
  - **Decided 2026-11-01 with the user (card X1c, its first entry):**
    `tools/crgpu_msl` is built, and the shader steps run it, only where a
    .metallib is made: an Apple target, or a build cross-compiling for one
    from a Mac. Windows and Linux builds neither build it nor write Metal
    source, unless configured with `-DCR_SHADERS_CHECK_METAL=ON` (off by
    default), which builds the tool and translates every shader to Metal
    source as a check, with no .metallib; the Linux CI runner turns it on,
    so a shader SPIRV-Cross can't translate fails there before a Mac sees
    it. `cmake/cr_slang.cmake` here; a caller's own step that runs the
    tool follows the same switch. Not built.
  - **Decided 2026-11-01 with the user (card X18, b; its build half, a,
    with S27):** one shader step for every caller,
    `cmake/cr_shaders.cmake` with `cr_add_shader()` (Slang to SPIR-V
    checked by `spirv-val`, DXIL and its validator on Windows,
    `-reflection-json`, and Metal per X1c's switch above, padded per S27),
    replacing `cmake/cr_slang.cmake`; a caller's own step wraps it rather
    than copying it. `tools/crgpu_msl` is always built as a program of
    its own for the build machine (`tools/crgpu_msl/CMakeLists.txt`
    through ExternalProject, native and cross-compiling alike), or taken
    from `CRGPU_MSL_EXECUTABLE` where the caller names one, so its
    SPIRV-Cross is never a target in the caller's build. Not built.
  - **Decided 2026-11-01 with the user (card S27, a):** a module declares
    every binding of each set it reads, with the layout's type and count,
    and no other, stated for every backend in `crgpu_shader_module_desc`;
    Metal's pipeline creation reflects each set's argument buffer and
    refuses a module that differs (`CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH`,
    331); Vulkan and D3D12 don't check it. The build keeps the rule:
    `cmake/cr_slang.cmake` reads Slang's reflection (`-reflection-json`
    lists every declared binding with its kind and a "used" flag, though
    the SPIR-V drops the unread ones; checked 2026-11-01) and passes it to
    `tools/crgpu_msl`, which moves to SPIRV-Cross's C++ API and pads each
    set (`pad_argument_buffer_resources` needs each binding's base type,
    which the C API's `spvc_msl_resource_binding_2` lacks); the step's
    arguments don't change. `crgpu_shaderbindings_draft.h` approved
    2026-11-01 by the user, as written. Slang's `-preserve-params`
    is reported to Slang either way (`../claude_made_decisions.md`,
    2026-11-01).
  - **Run on Windows 2026-11-02** (the ThinkPad, D3D12 and Vulkan, the
    Iris Xe, in a caller's Debug builds): `crgpu_objects_test`'s set
    bindings case expected D3D12 and Vulkan to make every pipeline,
    including the modules that break "Every binding of a set"; D3D12's
    runtime refused two (a binding past the set, a count of 2; "Root
    Signature doesn't match ... not fully bound"), with its debug layer's
    errors. S27 says Vulkan and D3D12 don't check, so such a module is the
    caller's error there: the test now makes the breaching modules on
    Metal only, and passes on D3D12 and Vulkan with no layer message. Not
    rerun on the Mac.
  - **Built 2026-11-01 on the Mac (X1c, X18's first step, S27; Apple M5
    Pro, macOS 27.0.1, Xcode 27.2, Slang 2026.18.3-2):**
    - `cmake/cr_shaders.cmake`, `cr_add_shader()`, in place of
      `cmake/cr_slang.cmake`: SPIR-V 1.5 checked by `spirv-val`, Slang's
      `-reflection-json` beside it, DXIL with DXC's validator on Windows,
      Metal source (both routes) only where a .metallib is made or under
      `CR_SHADERS_CHECK_METAL` (off by default), the .metallib for the
      build's SDK and deployment target on Apple platforms. It sets
      `<name>_FORMATS` for a caller's wrapper. carrottus names it in `CR_SHADERS_CMAKE` for a caller's
      build. Its switches are read inside the function (APPLE, the cached
      option): the file is read once, in whichever directory includes it
      first, and a plain variable set there isn't seen from the others
      (found when a caller's Metal sources weren't made).
    - `tools/crgpu_msl` is always its own program, built for the build
      machine through ExternalProject (`crgpu_msl_build`, Release), an
      imported executable `crgpu_msl` the steps run, or the one
      `CRGPU_MSL_EXECUTABLE` names. The two copies of SPIRV-Cross's
      options in `CMakeLists.txt` are gone; no SPIRV-Cross target is in a
      caller's build.
    - The padding is the tool's own, not SPIRV-Cross's
      `pad_argument_buffer_resources`, which can't make this layout: it
      walks argument buffer ids from 0 up and throws at a gap (a combined
      image sampler's sampler at 1024 + b, any sparse layout such as the
      test's 0, 5, 8), and adds nothing past the last member a module
      declares. `crgpu_msl` reads the reflection (`--reflection`, a
      bounded JSON reader of its own) and writes, into each set's argument
      buffer struct SPIRV-Cross wrote, a member at each binding's id that
      SPIRV-Cross left out, of the binding's Metal kind and count; a set
      the module reads none of stays out. It stays on SPIRV-Cross's C API,
      which is all this needs (`../claude_made_decisions.md`). The
      header's sentence naming `pad_argument_buffer_resources` was left
      out of `crgpu.h` and carded (S30), the draft unedited.
    - `CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH` (331) and the module desc's
      "Every binding of a set" in `crgpu.h`, the two creates' result
      lines. Metal's pipeline creation asks for buffer type information
      and compares each stage's argument buffer struct, for each set it
      reads, with the set's layout as runs of ids of one Metal kind
      (texture, sampler, buffer, acceleration structure; one array member
      of n and n members are the same buffer), kept on the layout when it
      is made (`_ranges`). Metal can't tell a uniform from a storage
      buffer or a sampled from a storage image in a struct, nor a texture's
      type: the check is of what it can see. A mismatch is the code
      alone, with no message: Carrottus writes none (`CLAUDE.md`,
      "Errors"; card S10's callback carries the platform's errors, not
      this).
    - Tests: `crgpu_objects_test`'s `test_set_bindings`, a set of a
      uniform buffer at 0, a sampled image at 5 and a sampler at 8 (and a
      storage buffer set for compute): the whole module made and its draw
      reading the image's texel; on Metal the set declared in part, a
      binding at 9 it lacks, 5 a buffer, and 5 an array of 2 refused, and
      the compute pipeline's three refusals; a compute module reading only
      set 1 made. Vulkan and D3D12 make every one (written, not run).
    - Ran: 15 ctests on `macos-metal` in Debug and Release under Metal API
      and shader validation with warnings fatal, `-asan` and `-tsan`;
      `cr_misuse_tests` ten times (217 calls with a destroyed handle, no
      failure; the generator found no new call); `cr_noalloc_tests` none
      of ours; `iossimulator-metal` built. `abi.txt`: one addition (331),
      no break. Windows and Linux not built.
  - **Configured on Windows 2026-11-05** (ThinkPad P1 Gen 5, Windows 11,
    MSVC 14.51.36231, Vulkan SDK 1.4.363.0, slangc 2026.17; X1c's switch
    and X18's first step): fresh build folders of temporary names, each
    configured and built in Debug, for `windows-d3d12` and
    `windows-vulkan`, and for two callers' builds that take
    `CR_SHADERS_CMAKE` on each: no `crgpu_msl` in any configure or
    build log, none built, no `.metal` or `.metallib` written (SPIR-V
    only, and DXIL on D3D12; carrottus's own 20 shaders, the callers' 73 and
    84). Then `windows-d3d12` once with `-DCR_SHADERS_CHECK_METAL=ON`:
    "Shaders: crgpu_msl built for the build machine", `crgpu_msl.exe`
    built through ExternalProject in `crgpu_msl_host/`, both routes'
    Metal source for all 20 shaders (20 `.slang.metal`, 20
    `.cross.metal`), no `.metallib`. Every build clean but MSBuild's
    MSB8029 (a build folder under `%TEMP%`). Folders deleted after. Not
    every build tree on the ThinkPad that includes this step was configured
    again that day, so the INTERNAL `CRGPU_MSL_EXECUTABLE` clearing
    below stays. Linux's configure is still owed. The same day `crgpu_objects_test`
    (`test_set_bindings` and every placed buffer and image among its
    checks) passed on D3D12 on the Iris Xe, the RTX A2000 and WARP (debug
    layer silent) and on Vulkan on both GPUs under synchronization
    validation (silent).
  - **Decided 2026-11-05 with the user (card S38, a): the first use of an
    attachment image, committed or placed.** The D3D12 backend makes every
    resource not zeroed, and D3D12 then requires a render target or
    depth/stencil resource, committed or placed, to be initialized by a
    clear, a full copy or a discard before anything else (debug layer
    message 1422, seen 2026-11-05 on Windows from a caller's test whose
    image was first written by a compute pass). `crgpu.h` stated that
    only for placed attachment images. It is to state it for every image
    with a color or depth/stencil attachment usage, on every backend, and
    the D3D12 backend keeps making them not zeroed; callers clear or copy
    first. Draft `crgpu_image_first_use_draft.h` (two comments, no
    declaration or ABI change; a test in `crgpu_objects_test`), written
    2026-11-05 for approval. **Approved and built 2026-11-05 on Windows:**
    `crgpu_image_create()`'s comment states the rule for every image with
    an attachment usage, and `crgpu_image_create_placed()`'s refers to it.
    `crgpu_objects_test`'s `test_attachment_first_use`
    (`tests/shaders/objects_first_use_comp.slang`): an 8 x 8 R32_FLOAT image
    with color attachment, storage and copy-source usage, cleared first,
    then its left half written by compute through a storage view, read
    back: the shader's values and the clear. Ran on Windows 11 (ThinkPad
    P1 Gen 5): `windows-d3d12` and `windows-vulkan`, Debug and Release, no
    warning, 15 ctests each; `crgpu_objects_test` on D3D12 (Iris Xe, RTX
    A2000, WARP; debug layer silent) and Vulkan (both GPUs, synchronization
    validation, silent), 0 texels wrong; with the clear taken out for one
    build, D3D12 reported 1422 and 32 texels were wrong, so the test sees a
    breach. Metal's half written, not compiled (its shader goes through the
    build's Metal route like the rest). A caller's test that broke the rule
    (its reflection texture written first by compute) clears it first now,
    and its D3D12 run is silent.
  - **Decided 2026-11-01 with the user (card S30, b):** the draft's
    sentence on padding stays out of `crgpu.h`, as built. The header
    states the contract (what a module declares, what Metal refuses); how
    a build keeps it is the build's, and `cmake/cr_shaders.cmake` says
    so. No header change.
  - **Reviewed 2026-11-01 with the user** (`../claude_made_decisions.md`):
    the padding by text in `crgpu_msl` is card S31 (pad the SPIR-V
    instead, so SPIRV-Cross writes every member); a structural refusal's
    message is card S32. `cmake/cr_shaders.cmake`'s clearing of an
    INTERNAL `CRGPU_MSL_EXECUTABLE` (lines 44-51) goes once the ThinkPad
    and Fedora have each configured.
- **Specialization constants. Decided 2026-10-28 with the user (card
  A9):** a pipeline desc takes (constant id, value) pairs for its shader
  modules: Vulkan's `VkSpecializationInfo`; Metal's function constants;
  D3D12 has none, so with native formats a variant is a DXIL the caller
  compiled for it (as Unreal's permutations are), and the EXT is reported
  unsupported there. **Settled 2026-10-31 with the user (card S18):**
  `CRGPU_EXT_SPECIALIZATION_CONSTANTS`, 0 on D3D12 (a gap in a backend,
  not a GPU). A translation; not started.
- **Pipeline caches, later (card A9):** `VkPipelineCache`, D3D12's
  `ID3D12PipelineLibrary`, Metal's `MTLBinaryArchive`, as bytes the caller
  is handed to keep and gives back; Carrottus GPU writes no file.
- **Hardware ray tracing. Decided 2026-10-28 with the user (card S12).**
  Not started.
  0. **First, a test:** that the shader path carries ray queries - Slang
     (card A29, decided 2026-10-31) `RayQuery` to SPIR-V and to DXIL (DXR
     1.1), and to Metal by both routes, Slang's own Metal output and Slang
     to SPIR-V to SPIRV-Cross (`intersection_query`), each run on a
     device. The route that passes on Metal is the one these shaders take;
     if neither does, that is settled before anything else.
     **Compiled and validated on Windows, 2026-10-31** (ThinkPad P1 Gen
     5, Windows 11; Slang 2026.17-25-gc7954ebd1, DXC 1.9.0.5512, SPIRV-Tools
     v2026.4, SPIRV-Cross from Vulkan SDK 1.4.363.0, all the SDK's; not yet
     run on a device, which needs step 1). Two shaders in
     `tests/shaders/`, built on every build by `cmake/cr_slang.cmake`:
     `rq_trace.slang`, a compute shader tracing one ray per pixel with
     its flags and cull mask given at run time, committing triangle
     candidates by hand (`CommitNonOpaqueTriangleHit`) and a procedural
     candidate (a sphere in an AABB) with
     `CommitProceduralPrimitiveHit`, and writing the committed hit's t,
     instance index, instance id, geometry index, primitive index and
     barycentrics; and `rq_shadow.slang`, a fragment shader tracing a
     shadow ray with `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
     RAY_FLAG_FORCE_OPAQUE` on the query's type and a cull mask.
     - **SPIR-V** (1.5, `SPV_KHR_ray_query`): both compile, `spirv-val`
       (Vulkan 1.2 rules) clean. Every query maps to its own
       `OpRayQuery*KHR`; the type's flags reach `OpRayQueryInitializeKHR`
       (5 for the shadow ray); `CommittedInstanceIndex` is
       `...InstanceIdKHR` and `CommittedInstanceID` `...CustomIndexKHR`, as
       they should be.
     - **DXIL** (`cs_6_5`, `ps_6_5`; Slang's HLSL compiled by DXC, whose
       validator signed both): both compile, flagged "Raytracing tier
       1.1"; `AllocateRayQuery` carries the type's flags (5); both commits,
       `Proceed` and `TraceRayInline` present. Bindings where Carrottus GPU's
       D3D12 backend puts them (set 0 binding n at register n of space 0,
       the push constants at b0 of space 4), from `register()` written
       beside each `[[vk::binding]]`.
     - **Metal, Slang's own source:** generated, not compiled (no Metal
       compiler on Windows). Two faults found by reading it. Its ray flag
       translation **drops `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`**
       (0x4 has no case; SPIRV-Cross makes it `accept_any_intersection`),
       so a query asking for any hit gets the closest one: right for a
       shadow ray's yes or no, wrong for a caller that reads which hit.
       And with no Metal binding written in the source, it puts the push
       constants and the top level both at `[[buffer(0)]]`. Its entry
       point `main` is renamed `main_0` (Metal reserves `main`). Candidate
       kinds and committed status map correctly, as do the committed
       getters.
     - **Metal, SPIRV-Cross's source** (from the SPIR-V above): generated,
       not compiled. Every query maps, flags included. Its binding indices
       come from Carrottus GPU's remaps (each set's argument buffer at
       `CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE` + set, push
       constants at 30, combined samplers at +1024), which only SPIRV-Cross's
       C API can set, not its command line; how that reaches a build step
       is card S17 (the native-format draft).
     Whether the faults are those of Slang issues 13408, 13609 or 13688
     wasn't looked up. On the Mac: both sources compiled with `metal`,
     and both run on the M5 Pro by step 1's test.
     **Run on a device, 2026-10-31** (step 1 built; Windows 11, ThinkPad
     P1 Gen 5, `windows-d3d12` and `windows-vulkan`, Debug and Release):
     `rq_trace` (compute) and `rq_shadow` (fragment) ran true from both
     targets on the NVIDIA RTX A2000, and from DXIL on WARP, checked by
     `crgpu_objects_test` against hits and shadows worked out on the CPU:
     a 16 x 8 grid of rays over a triangle and a sphere in an AABB, every
     ray's t, instance index and id, geometry and primitive index, kind and
     barycentrics within 1e-3, through cull masks 0xFF, 0x01 and 0x02, after
     a refit, through a clone, over a compacted bottom level and over
     GPU-written instances (128 rays, 0 wrong each time); `rq_shadow` over
     a 32 x 16 plane, the triangle's 28 shadowed pixels and every lit one
     right. `rq_trace`'s hit records became a byte-address buffer first
     (Carrottus GPU's D3D12 storage buffers are raw views). The procedural sphere
     casts no shadow in `rq_shadow`, as written: its query loop commits no
     procedural candidate, forced opaque or not. Metal: not run (the Mac).
  1. **`CRGPU_EXT_RAY_QUERY`**, paired with a device feature bit (card
     S3's), the narrow island `CLAUDE.md` expects: acceleration structures
     (bottom level per mesh, top level of instances) created, built,
     refitted and compacted; one as a shader binding; ray queries in
     compute and fragment shaders. Vulkan `VK_KHR_acceleration_structure`
     and `VK_KHR_ray_query`; D3D12 DXR 1.1; Metal acceleration structures
     and intersectors (hardware from Apple's M3). The caller's fallback on
     a GPU without it (an Iris Xe).
     **Instances in the device's own layout (card S14, decided 2026-10-31
     with the user).** Vulkan's and D3D12's instance is the same 64 bytes
     (row-major 3x4 transform, 24-bit custom index, 8-bit mask, 24-bit
     table offset, 8 bits of flags, the bottom level's address); Metal's
     indirect instance descriptor is 72 (column-major packed 4x3
     transform, 32-bit options, mask, table offset and user ID, the bottom
     level's `MTLResourceID`). Carrottus GPU converts nothing:
     - a query gives the device's instance size, alignment and layout
       kind;
     - `crgpu_write_acceleration_structure_instance()` writes one
       instance from a portable struct into memory the caller names: a
       CPU translation, no state;
     - one call returns a bottom level's reference as a `uint64_t`
       (Vulkan's device address, D3D12's GPU virtual address, Metal's
       resource ID), which the portable struct and a GPU writer use;
     - a GPU writer is the caller's, one per backend (a Slang target
       switch, card A29);
     - the header states the narrowest contract: an 8-bit mask, a 24-bit
       user index and table offset, and only the flags all three share.
     Not a portable layout the Metal backend converts on the GPU (hidden
     work and memory, against S2), nor a layout of Carrottus GPU's converted on
     the CPU (no GPU-written instances).
     **The API, approved 2026-10-31 by the user:** `crgpu_raytracing_draft.h`
     as written (R1-R8). Built in Phase 5 of `prompts/ray_tracing.md`.
     **Settled while building, 2026-10-31 with the user (cards S23-S26):**
     the heap class and descriptor type refused with
     `CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED` without the feature (the
     build-input usage kept as a binding fact); `placement_alignment` 256 on
     Vulkan, 64 KiB on D3D12, and on Metal what Metal gives (1024 on the M5
     Pro, read 2026-11-01); a SCRATCH
     to SCRATCH barrier orders builds sharing a scratch range; a build,
     copy or size write refused in a list of the wrong queue type or inside
     a rendering pass, each with its own code.
     **Built 2026-10-31:** `crgpu.h`'s ray query section and
     `crcore.h`'s results (328, 360-369), Vulkan and D3D12 run, Metal
     written, not compiled (`crgpu{vulkan,d3d12,metal}raytracing.*`, the
     shared checks in `crgpushared.c`), with the review's fixes and cards
     S23-S26 built the same day: D3D12 records a UAV barrier between any
     two states that are both its `UNORDERED_ACCESS`; an update is compared
     with its source's last layout value by value (kind, flags, counts and
     index format of each geometry; a top level's instance count), not a
     hash; every check of a build comes before the recording, which can't
     fail; a placed structure the API would make larger than its size
     rounded to `placement_alignment` is refused
     (`CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE`, the header's own code for a
     range past its bounds); a build, copy or size write is refused on a
     copy list or bundle (368) and inside a rendering pass (369); the
     compute bindings are undefined after one until bound again. Run on
     Windows 11 (ThinkPad P1 Gen 5), `windows-d3d12` and `windows-vulkan`,
     Debug, Release and `-asan`: `crgpu_objects_test` on the RTX A2000 and
     WARP (D3D12 debug layer and GPU-based validation; Vulkan validation,
     synchronization and GPU-assisted validation; all silent), including
     three builds sharing one scratch range between two dispatches writing
     the same buffer, updates of exactly a bottom level's last layout by it
     and its clone, and the copy-list and inside-a-pass refusals; the Iris
     Xe's refusals (no ray queries on either API), `cr_noalloc_tests`'
     refit-and-trace frame (none of ours), the misuse harness ten times.
     **On the Mac, 2026-11-01** (Apple M5 Pro, macOS 27.0.1, Xcode 27.2;
     `../prompts/ray_tracing_mac.md`): Metal's file compiled at once, with no
     warning, and `crgpu_objects_test` passed whole under Metal API and
     shader validation with warnings fatal (Debug and Release; `-asan` and
     `-tsan` passing too, without the layer): every build, refit (in place too), clone, compaction (640 to
     528 bytes), the CPU's and the GPU writer's 72-byte instances (the same
     144 bytes), cull masks, three builds sharing one scratch range, a placed
     bottom level, the copy-list refusals, `rq_trace` and `rq_shadow` (S12's
     step 0, by `crgpu_msl`'s route: 9 traces of 128 rays and 512 shadow
     pixels, 0 wrong; Slang's route can't be bound, its source having no
     argument buffer and two resources at buffer 0). Changed in Metal's
     file: `placement_alignment` read from
     `heapAccelerationStructureSizeAndAlignWithSize:` (1024 for every size,
     1 byte to 100 MB, the size never rounded; S24); the 72-byte layout
     asserted field by field against
     `MTLIndirectAccelerationStructureInstanceDescriptor` (transform 0,
     options 48, mask 52, table offset 56, user id 60, resource id 64,
     aligned 8); a top level's bottom levels kept as Metal objects and made
     resident with one `useResources:count:usage:` per encoder (R5); the
     build's descriptors kept on the command list, so a build makes none.
     `cr_noalloc_tests`' refit-and-trace frame: none of ours; 41 a frame
     of Metal's own (17 the driver's in the build, 6 in its size query, 9
     the encoder's, 9 the command buffer's, submit's and the compute
     encoder's and set's). Not probed beyond what the tests build: the
     alignment Metal asks of the vertex, index and box offsets (its
     documentation gives none; validation is silent over the tests' 4-, 8-
     and index-sized offsets). Left from the original list, settled by the
     runs: the `supportsRaytracingFromRender` gate (set on the M5 Pro, and
     `rq_shadow` traces from a fragment shader), the label made without a
     pool, and the set's descriptors written element by element. The
     original list, kept for the record: build and run all of the above on the M5 Pro; read
     `heapAccelerationStructureSizeAndAlignWithSize:` and set Metal's
     `placement_alignment` from it (card S24); confirm the alignment Metal
     asks of `vertexBufferOffset`, `indexBufferOffset` and
     `boundingBoxBufferOffset` (its documentation gives none; Carrottus GPU checks
     the rule Vulkan and D3D12 share: 4 bytes for vertices, 8 for boxes, an
     index's own size);
     check the feature bit's `supportsRaytracingFromRender` gate, the
     in-place refit (nil destination), the label made without an
     autorelease pool, and an array of acceleration structure descriptors
     written element by element.
     **Measured** (Release, RTX A2000, `tests/crgpu_rq_measure.c`: GPU
     work timed on the CPU, best of 20 submissions less an empty list's,
     50.4 us on Vulkan and 4.6 us on D3D12; so the small numbers are mostly
     submission, not GPU, time), Vulkan / D3D12:
     - a bottom level of a 12-triangle cube: built 115 / 183 us, 3,840
       bytes, 1,152 compacted (30%), scratch 2,688;
     - the Dragon of CrGPUMeshShaders (201,972 triangles, 101,166
       vertices): built 7.85 / 7.24 ms (25.7 / 27.9 Mtri/s), 14,150,784
       bytes, 5,932,928 compacted (42%), scratch 4,275,712; the compacting
       copy 662 / 188 us;
     - a top level of 1 instance: built 60 / 87 us, refitted 29 / 57 us;
       of 1,000: built 186 / 186 us, refitted 30 / 77 us, 349,184 bytes;
       of 100,000: built 2.09 / 2.13 ms, refitted 1.05 / 1.11 ms,
       34,683,008 bytes;
     - instances written on the CPU (the write call into a mapped buffer):
       2.5 / 2.3 us for 1,000, 307 / 309 us for 100,000 (3.1 ns each); on
       the GPU (`rq_instances_row64.slang`): 7 / 58 us for 1,000, 244 /
       229 us for 100,000;
     - `rq_trace`, one ray per pixel at 1920 x 1080 over the Dragon:
       824 / 909 us (2,518 / 2,282 Mrays/s).
     **The M5 Pro** (Release, Metal, no validation layer; an empty list
     16.6 us): a cube's bottom level built 355.5 us, 1,536 bytes, 912
     compacted (59%), scratch 256; the Dragon's built 3.93 ms (51.4 Mtri/s),
     19,236,096 bytes, 10,831,312 compacted (56%), scratch 7,504,000, the
     compacting copy 1.87 ms; a top level of 1 instance built 199.0 us,
     refitted 155.7 us, 832 bytes; of 1,000 built 391.4 us, refitted 169.0
     us, 219,968 bytes; of 100,000 built 3.18 ms, refitted 335.5 us,
     21,943,488 bytes; instances written on the CPU in 2.2 us for 1,000 and
     238.5 us for 100,000 (2.4 ns each), on the GPU (`rq_instances_col72`)
     in 110.7 us and 180.2 us; `rq_trace` over the Dragon at 1920 x 1080
     1,040 us (1,994 Mrays/s). Metal's structures are larger than the other
     two APIs' (the Dragon's 36% before compaction, 83% after), and its
     small builds are slow to start (the GPU waking), as its 100,000-instance
     refit is quick.
  - **Ray tracing pipelines are not part of it** (asked 2026-10-28). Vulkan's
    and D3D12's map nearly one to one (the same stages - ray generation,
    miss, closest-hit, any-hit, intersection, callable - shader tables of
    32-byte handles, recursion to a limit; they differ in per-record
    resources, D3D12's local root signatures against Vulkan's record data,
    in table alignment, and in adding shaders to a pipeline). Metal has no
    such pipeline: a compute kernel intersects and decides, with
    intersection and visible function tables for the swappable parts and
    no recursion, so carrying Vulkan's model onto Metal would be Carrottus
    inventing one. **The planned shape, if inline queries prove not
    enough** (measured on an Apple M5 Pro; alpha-tested geometry and a path
    tracer, card T16, are the likely cases), is **two sibling EXTs, each a
    straight translation:**
    - `CRGPU_EXT_RAY_TRACING_PIPELINE`, Vulkan and D3D12 only, compiled
      out on Metal as `CRGPU_EXT_BUNDLES` is on the other side;
    - `CRGPU_EXT_RAY_FUNCTION_TABLES` (named for its capability, card
      S15, 2026-10-31), Metal only today, translating Metal's own model: intersection function
      tables and Metal 4's intersection function buffers (alpha-tested and
      custom geometry), visible function tables (a material's function by
      index), linked into compute pipelines. Its shaders can't be SPIR-V
      (the attributes have no SPIR-V form); with native shader formats
      (below, 2026-10-28) a `.metallib` is an ordinary input on Metal, so
      this is no exception.
    A caller writes its pipeline work once per EXT and hides the
    difference from its own callers.
    **Metal 4, checked 2026-10-28:** it has *intersection function buffers*
    (`MTLIntersectionFunctionBufferArguments`: a buffer of
    intersection-function handles with a stride, 8-byte aligned, and a
    user-data pointer; functions declared for them), with an intersection
    function table offset per geometry and instance summed into the index,
    as a shader table's hit records are. Apple presents them, with Metal
    Shader Converter, as the way to map an existing shader binding table
    onto Metal (WWDC25 "Level up your games" and "Go further with Metal 4
    games"; the sample "Ray tracing with Intersection Function Buffer"
    emulates a DXR shader table). But they cover the **intersection side**
    (any-hit, intersection); closest-hit, miss and callable still go
    through visible function tables called from the kernel, and there is
    still no recursion - MoltenVK's work in progress (PR 2771, not merged
    when checked) splits it exactly so. So Metal 4 sits closer to the
    other two than older Metal, not close enough for one pipeline EXT on
    all three; the two-EXT shape stands, its Apple half built on Metal 4's
    buffers where available.
- **Geometry and tessellation behind EXTs. Approved 2026-10-31 with the
  user** (`crgpu_geometry_tessellation_draft.h`): `CRGPU_EXT_GEOMETRY_SHADER`
  and `CRGPU_EXT_TESSELLATION`, 1 on Vulkan and D3D12 and 0 on Metal, with
  `CRGPU_FEATURE_GEOMETRY_SHADER` and `CRGPU_FEATURE_TESSELLATION`
  (Vulkan's optional features; D3D12 always; Metal never) and their
  `_NOT_SUPPORTED` results; a module or pipeline of either stage on a device
  created without the bit is refused, as mesh shading's are. **Built
  2026-10-31** (Phase 4 of `prompts/ray_tracing.md`): the refusals in the
  shared checks (`crgpushared.c`), Vulkan's bits from `geometryShader` and
  `tessellationShader` (enabled only for a device created with them),
  D3D12's on every adapter, Metal's old refusal (`INVALID_SHADER_STAGE`)
  gone. `crgpu_objects_test` refuses a module of each stage and a pipeline
  naming one on a device created without the bits, and checks the bits
  per adapter (D3D12 both, Metal neither). Run on Windows 11, Iris Xe and
  RTX A2000, `windows-d3d12` and `windows-vulkan`; both GPUs have both bits
  on both APIs. Metal written, not compiled; Linux not built. A pipeline
  drawing with either stage is not tested: no caller uses one.
- **Tile shading and framebuffer fetch: not now (card S16, decided
  2026-10-31 with the user).** Metal's tile shaders and imageblocks and its
  programmable blending (Vulkan has a partial counterpart in
  `VK_KHR_dynamic_rendering_local_read`; D3D12 none) are not translated
  yet. Their caller renders forward with no G-buffer, so tile memory's
  main gain doesn't apply, and what is left is had through the planned
  `CRGPU_IMAGE_USAGE_TRANSIENT_ATTACHMENT` (memoryless on Metal) in the
  core API. Reopened when a measurement on an iPad (Instruments' GPU
  counters) shows a caller bound by memory bandwidth; the names are kept
  for then (card S15): `CRGPU_EXT_TILE_SHADING`,
  `CRGPU_EXT_FRAMEBUFFER_FETCH`.
- **A room check before Carrottus's 1.0** (card A15, in the workspace's
  `DECISIONS.md`): every deferred Carrottus GPU item with the
  way it would be added after 1.0, and the ones whose shape needs a field
  taken before the freeze. Card S11 started it for video.
- **Room before 1.0 (card S11, decided 2026-11-01 with you: all seven, as
  revised when presented).** What would otherwise break to add hardware
  decode and encode, HDR output, presentation timing or shared resources
  after 1.0:
  - Changed before 1.0, in `crgpu_room_before_1_0_draft.h` (approved
    2026-11-01, not built):
    - (a) `CRGPU_ADAPTER_PROPERTIES::queue_pools` becomes a pointer to
      the adapter's own array, of any length, same names (a fixed `[8]`
      would still guess at drivers' families);
    - (c) `crgpu_swap_chain_desc::color_space`, required
      (`CRGPU_COLOR_SPACE`, Vulkan's names: `SRGB_NONLINEAR`,
      `EXTENDED_SRGB_LINEAR`, `HDR10_ST2084`; what the display reads, where
      the format is what the GPU writes), with `crgpu_get_surface_formats()`
      reporting a surface's format and colour space pairs; none existed, and
      a caller chose a format blind. Fixed with it: Vulkan's silent fallback
      to `B8G8R8A8_SRGB`, D3D12 failing every `_SRGB` swap chain (buffers
      UNORM, views `_SRGB`), and Metal's untagged layer (no colour matching,
      over-saturated on a P3 display);
    - (d) `crgpu_swap_chain_present()` takes a based
      `crgpu_present_desc` (timing and per-frame HDR metadata chain onto
      it later) and returns a result (out of date, surface lost, device
      lost); it was `void` with no struct, and Vulkan and D3D12 drop their
      present results today;
    - (f) one sync object: a submission waits on fence values, and
      `crgpu_semaphore`, its calls and the swap chain's two semaphore
      getters go (one caller in the workspace used them). On Metal the
      waits go in a command buffer of their own ahead of the lists' (or at
      the start of each list's, if Metal doesn't hold later buffers behind
      it; the draft's test settles it): today's semaphore waits are encoded
      after the first list's recorded work, and likely wait for none of
      it.
  - Written, not changed, since none breaks anything later:
    - (b) plane views of NV12 and P010 are `CRGPU_IMAGE_ASPECT_PLANE_0..2`
      bits appended. Metal has no two-plane pixel format (NV12 maps to
      nothing there today), so its backend holds one texture per plane;
      built with a caller;
    - (e) the import and export `EXT` group takes Vulkan external memory
      (DMA-BUF, Win32 handles), D3D12 shared handles and IOSurface on Metal.
      Its first use is to be a caller's rendered texture composed by a UI
      framework (card H30, 2026-11-02), once a spike shows which handles
      that framework imports;
    - (g) decode and encode sessions (Vulkan Video, D3D12 Video; Metal has
      no GPU-API counterpart to VideoToolbox) and their queue kind bits wait
      for a caller.
  - Seen, not taken: `crgpu_swap_chain_acquire_next_image()` returns an
    index and no result, so Vulkan's OUT_OF_DATE from acquire is lost too;
    for whoever next changes acquire.

## Breaks

Every caller: listing devices and taking the graphics queue both change,
and so does reading properties and capabilities, and the Vulkan and D3D12
natives that take a device. Every caller that presents asks for
CRGPU_FEATURE_PRESENT, and every caller sets a descriptor budget. A
shader outside bindless that uses `nonuniformEXT` is outside Vulkan's rules
once the descriptor-indexing bits are off. The callers change in the same
change and run their tests (`../CRGPU_DEVICE_MIGRATION.md`). All three
backends change: build `macos-metal`, `linux-vulkan` and `windows-d3d12`.

## Done when

The steps every item of `MATURITY_TODO.md` ends with:

- build on macOS and run the tests, plain and under the `-asan` and `-tsan`
  presets;
- build every caller the change breaks, in the same change, and run its
  tests, on all three backends (`../CRGPU_DEVICE_MIGRATION.md` lists
  them);
- `./check.sh` from the workspace;
- after the carrottus commit is pushed, `./check.sh --update-locks`, and commit
  the locks with the repositories that use them;
- each changed repository's `.git/COMMIT_DRAFT` rewritten to describe its
  change;
- `CLAUDE.md`: the threading entry under "Known gaps" shortened (the device
  array and the lazy bindless table are gone); the "lazily created member"
  exception names `_rtv_heap`, `_dsv_heap` and the indirect signatures of
  other strides only; the known gap on D3D12's shader-visible heaps says
  they are made with the device, sized by the caller's budget, and that a
  pool past it is refused on every backend;
- `CLAUDE.md`, Carrottus GPU conventions: the `CRGPU_EXT_*` paragraph says an
  EXT pairs with a `CRGPU_FEATURE_*` bit where support varies by GPU,
  and its result is returned when the device wasn't created with the bit;
  the mesh shading paragraph and the `CRGPU_LEGACY_*` paragraph point to
  the feature bits instead of `crgpu_get_mesh_shading_capabilities` and
  `crgpu_bindless_capabilities::supported`; the bindless names follow the
  rename.
