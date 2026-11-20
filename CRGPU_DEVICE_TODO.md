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

**Drafted 2026-11-14** (`crgpu_max_image_size_draft.h`, version 1, written
unattended on the Mac; waiting on your review): five fields at the end of
`CRGPU_ADAPTER_PROPERTIES` (1D, 2D, 3D, cube, array layers; a listed
layout break, 0.3.0 to 0.4.0), Metal's values by family from Apple's
Metal feature set tables (the October 23, 2026 edition, page 8), its four
questions numbered there. A caller sizing an atlas checks against it.

**Reviewed 2026-11-14** (the workspace's review prompt of 2026-11-14; its record, in
the workspace's `prompts/`, has every finding):

- **Your answers (stage A):** Q1 a (fields at the struct's end), Q2 a
  (all five), Q3 a (Metal by family), Q4 a (read and checked against each
  API's floor).
- **Seven cycles of critique,** each writing its fixes in. The draft
  reached **version 6**. Measured on this Mac: Metal makes 32768 (1D, 2D,
  cube) and 2048 (3D, layers; 341 cubes) and **aborts the process** one
  texel or layer past, validation on or off (C2.1). **Answered
  2026-11-14 with you:** `crgpu_image_create` and
  `crgpu_image_create_placed` refuse past these limits on every backend
  with `CR_RESULT_GPU_IMAGE_TOO_LARGE` (370), so a caller's size never
  reaches Metal's assertion; **version 7**. Each field is now stated as an upper bound: a
  Vulkan format may allow less (C2.2).
- **APPROVED by you 2026-11-14, version 7;** frozen. SHA-256 (carriage
  returns removed) of `crgpu_max_image_size_draft.h`:
  `025c845f32cd4a1fdc15381406e31e05d93179f0d49ed82473605fd8686347f5`.
  Ready to build, from the plan below: the Mac first,
  `../prompts/crgpu_image_size_mac.md`, which writes the Windows and
  Fedora prompts.
- **The build plan** (tests first):
  1. `tests/crgpu_device_test.c`: the five fields read on every adapter
     and checked against each API's floor (Q4), each Vulkan driver's
     numbers printed for this file; seen failing (the fields absent).
  2. `include/crgpu.h`: the five fields at `CRGPU_ADAPTER_PROPERTIES`'s
     end, with their comments; `include/crcore.h`:
     `CARROTTUS_VERSION_MINOR` 3 to 4.
  3. Vulkan (`src/gpu/backends/vulkan/crgpuvulkancontext.c`,
     `crgpuvulkan___read_adapter_facts`): the five from the limits it
     already reads. D3D12 (`crgpud3d12context.cpp`): the five constants
     beside `max_samplers`. Metal (`crgpumetalcontext.m`,
     `crgpumetal___internal_read_adapter_facts`): the family check beside
     the depth bounds test's, with the table's edition and page.
  4. The size check in `src/gpu/crgpushared.c` (written once, called by
     every backend's two image creates at the same place), its result in
     `crcore.h` and `cr_result_str()`, the refusals in `crgpu_objects_test`
     first.
  5. `./check.sh --update-abi` (the layout break listed), the misuse
     generator rerun, `./check.sh`; built on the Mac, then Windows and
     Fedora, each driver's numbers recorded here.
- **Built on the Mac 2026-11-15** (card X25: Windows and Linux built below;
  `../prompts/crgpu_image_size_mac.md`, its Progress has the run). Each
  step of the plan: 1 built on the Mac; 2 built on the Mac; 3 Metal built
  on the Mac, Vulkan written and type-checked on the Mac (`clang
  -fsyntax-only` against Vulkan SDK 1.4.363.0's headers, not linked or
  run), D3D12 written, not compiled; 4 built on the Mac, the Vulkan and
  D3D12 call sites written as above; 5 built on the Mac.
  - **The numbers, per adapter and driver:**
    - Apple M5 Pro (Apple10, macOS 27), Metal: 1D 32768, 2D 32768, 3D
      2048, cube 32768, layers 2048.
    - Windows 11 (build 26300, the ThinkPad P1 Gen 5), D3D12, the
      constants on every adapter: the Iris Xe (driver 32.0.101.7092), the
      RTX A2000 (32.0.15.9730) and WARP each 1D 16384, 2D 16384, 3D 2048,
      cube 16384, layers 2048.
    - Windows 11, Vulkan: the Iris Xe (Intel 101.7092, Vulkan 1.4.323)
      1D 16384, 2D 16384, 3D 2048, cube 16384, layers 2048; the RTX A2000
      (NVIDIA 597.30, Vulkan 1.4.329) 1D 32768, 2D 32768, 3D 16384, cube
      32768, layers 2048.
    - Fedora 44 (the ThinkPad P1 Gen 5, Mesa 26.2.6, Vulkan 1.4.354): ANV
      on the Iris Xe 1D 16384, 2D 16384, 3D 2048, cube 16384, layers 2048;
      NVK on the RTX A2000 1D 32768, 2D 32768, 3D 16384, cube 32768,
      layers 2048; llvmpipe (LLVM 22.1.8) 1D 16384, 2D 16384, 3D 4096,
      cube 32768, layers 2048 (its cube past its 2D, which Vulkan
      allows). Every one at or above Vulkan 1.3's floors.
    - Owed: RADV on the Fedora desktop, not reachable from the ThinkPad.
  - **Built on Windows 2026-11-16 and 17** (the ThinkPad P1 Gen 5; MSVC
    14.51, Vulkan SDK 1.4.363.0; `../prompts/crgpu_image_size_windows.md`,
    its Progress). Each step of the plan: 1 to 5 built on Windows. The
    D3D12 half compiled as written, and the Vulkan half linked and ran as
    type-checked: nothing in carrottus changed. `windows-d3d12` and
    `-vulkan`, Debug and Release, and both `-asan` presets, no warning,
    15 of 15 ctests in each; `crgpu_device_test` and `crgpu_objects_test`
    on every adapter (D3D12 the Iris Xe, the A2000 and WARP under the
    debug layer and again under GPU-based validation; Vulkan the Iris Xe
    and the A2000 under validation with performance warnings and again
    under synchronization validation; Release and ASan on each): every
    refusal reached on both creates and every small image made, no
    message, no report. `cr_misuse_tests` ten times on each backend
    (285 calls with zeroes, 258 with a destroyed handle, no failure);
    `cr_noalloc_tests` none of ours on either.
  - **Built on Linux 2026-11-17** (Fedora 44, the ThinkPad P1 Gen 5; GCC
    16.2.1 and clang 22.1.8, Vulkan SDK 1.4.363.1;
    `../prompts/crgpu_image_size_fedora.md`, its Progress). Each step of
    the plan: 1 to 5 built on Linux. The Vulkan half linked and ran as
    type-checked on the Mac: nothing in the library changed.
    `linux-vulkan` Debug and Release, `-asan` and `-tsan` with GCC and
    with clang, no warning, 15 of 15 ctests in each;
    `crgpu_device_test` and `crgpu_objects_test` on ANV, NVK and
    llvmpipe under validation with performance warnings and again under
    synchronization validation, in all six builds: every refusal reached
    on both creates and every small image made, the layer silent.
    `cr_misuse_tests` ten times (244 calls with a destroyed handle, no
    failure); `cr_noalloc_tests` none of ours, Debug and Release. One
    fix, in the test: llvmpipe reports ray queries since Mesa 26.2, and
    its one queue family holds one queue, so the ray query device's copy
    queue is asked only where the pools hold it, and its list's refusals
    skipped (and said) where not. Under the sanitizers llvmpipe alone
    still reports its own leak and fence race (card S37), named and not
    counted.
  - **Seen failing first:** the fields didn't compile; a 2D image 32769
    wide made through `crgpu_image_create` ended the process inside
    `newTextureWithDescriptor` (exit 134, "MTLTextureDescriptor has width
    (32769) greater than the maximum allowed size of 32768"). Now every
    case of `crgpu_objects_test` (a 1D width, a 2D width and height, a 3D
    width, height and depth, a cube's width, a 2D array's and a cube
    array's layers, each on both creates) is refused with
    `CR_RESULT_GPU_IMAGE_TOO_LARGE` under Metal API and shader validation,
    warnings fatal, and a small image of each type is still made.
  - **Where the placed create checks:** before the size and alignment
    query the offset's checks need, so between the requirement query's
    results and `CR_RESULT_GPU_PLACEMENT_MISALIGNED`, on every backend:
    Metal's query (`heapTextureSizeAndAlignWithDescriptor`) ends the
    process for the same image too (measured, exit 134).
  - **For you:** `crgpu_heap_get_image_placement_requirements` also ends
    the process on Metal for an image past the limits (the same query,
    measured on the M5 Pro). The approved contract gives the refusal to
    the two creates only, so it wasn't added there: a caller that asks the
    requirements of an image past the limits still aborts on Metal. The
    fix would be the same check in that call, `CR_RESULT_GPU_IMAGE_TOO_LARGE`
    added to its result list (a draft change, a card).

## A swap chain's image count (card S13, decided 2026-11-12 with you: b)

`crgpu_swap_chain_desc::buffer_count` is the caller's on every backend,
refused where the surface can't take it, with a query to choose by:

- `crgpu_surface_get_image_count_range(device, surface, &min, &max)`,
  beside `crgpu_surface_get_present_scalings()`: Vulkan's
  `VkSurfaceCapabilitiesKHR::minImageCount` and `maxImageCount` (0, no
  limit, reported as `UINT32_MAX`); D3D12's flip model's 2 to
  `DXGI_MAX_SWAP_CHAIN_BUFFERS` (16); Metal 1 to 1 while its backend takes
  only 1 (`CAMetalLayer`'s `maximumDrawableCount` of 2 or 3 is the room if
  it ever takes more).
- Vulkan's `crgpu_swap_chain_create` asks for exactly `buffer_count`
  (`minImageCount` in `VkSwapchainCreateInfoKHR`), no longer
  `minImageCount + 1` (`crgpuvulkanswapchain.c`); a count outside the
  range is refused with `CR_RESULT_DESC_INVALID_PARAMETERS` on every
  backend, checked the same way and in the same order. The driver may
  still make more images than asked (Vulkan's rule), which the desc's
  getter already reports.
- A draft for approval first (card X21): the new call, the contract change
  of `buffer_count` (a listed break: a caller asking for fewer than its
  surface's minimum now fails), its tests (the range on a hidden window's
  surface on every adapter, a count past each end refused, the count got
  back). Each caller asks the query and chooses. Not built.

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
  Drafted 2026-11-18: `carrottus/crgpu_dispatch_indirect_draft.h` (version 1, seven critique passes), not approved; nothing built. **Approved 2026-11-18 with the user**, every question as recommended. **The compute half built on the Mac 2026-11-18** (below, "Indirect dispatch"); the mesh half stopped, Apple's tables giving indirect mesh draws to Apple9 and later only.
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

## Anisotropic filtering: a feature bit and a limit (card S49, decided 2026-11-17: a)

Found 2026-11-17: `CRGPU_SAMPLER_DESC::max_anisotropy` reaches every
backend, but no device reports whether it filters anisotropically or its
largest level, and the Vulkan device never enables `samplerAnisotropy`, so
a sampler above 1 on Vulkan is outside Vulkan's rules today. Card S49
recommends a feature bit asked for at device creation and
`max_sampler_anisotropy` in the adapter's limits, a sampler past either
refused (no clamp). Decided 2026-11-17 with you as recommended. A draft
first, then the caller's sampler checks after it.

Its feature bit is **bit 18** (`CRGPU_FEATURE_SAMPLER_ANISOTROPY`, the
name its draft confirms): S48 takes 16 and 17, S50 19, so the three
drafts don't collide (S48's third critique round, 2026-11-17).

**Draft approved 2026-11-17 with the user** (`crgpu_anisotropy_draft.h` v1,
every question as recommended). Not built.

## Block-compressed formats: ASTC added, BC finished (card S48, decided 2026-11-17)

Decided 2026-11-17 with you: `crgpu_astc_draft.h` approved, every
question as recommended: the 14 ASTC LDR sizes, UNORM and sRGB (Q1a);
HDR later with its own bit (Q2a); numbered from 1000 (Q3a); no EXT macro,
the feature bits gating (Q4a); level 0 whole blocks on every backend
(Q5a); the buffer and image copies returning `CR_RESULT` and refusing a
bad region (Q6a); block formats 2D only (Q7a); `crgpu_format_get_bit_count`
0 for ASTC (Q8a). It fixes the three faults found drafting it: the copies'
row pitch for block formats on all three backends, Metal's missing BC
formats, Vulkan never enabling `textureCompressionBC`. No version raise
while 0.4 is untagged (card X2). Built on the Mac in full; BC and ASTC's
refusals on Windows and Fedora (no GPU at hand samples ASTC over Vulkan).
Cards S49 (anisotropy) and S50 (dual-source blending) are drafted in
the same session, for approval, not built.

**Amended by the third critique round, 2026-11-17 (the draft's version
9, approval kept):**
- **A new call, `crgpu_image_get_buffer_copy_row_pitch_alignment(image,
  out_alignment)`** [ADDED]: the alignment a buffer-image copy's row pitch
  must meet for that image (256 bytes on D3D12, the texel block's bytes on
  Vulkan and Metal); the copies refuse a pitch that misses it.
- **The copies' checks:** the buffer's and the image's transfer usage,
  the buffer range (existing codes); a layer's start a multiple of the
  offset alignment (512 on D3D12); a layer count or depth of 0 refused (a
  listed break). The offset-alignment query reads the texel block;
  `crgpu_format_get_buffer_copy_size` writes 0 for a format with no texel
  block.
- **D3D12's footprint fixes** (`make_footprint`, three errors found):
  the row pitch from the bit count, the layer step counted in texel rows,
  and a readback box not of whole blocks.
- **Texel blocks for packed formats:** single-texel formats 1 x 1;
  4:2:2 packed 2 x 1 (`R8G8_B8G8`, `G8R8_G8B8` and `YUY2` 4 bytes, `Y210`
  and `Y216` 8 bytes); refused for `UNKNOWN`, `R1_UNORM`, the eight planar
  formats and any unnamed value.
- Noted, not changed: `MTLGPUFamilyMac2` is deprecated in the macOS 27
  SDK. A general per-format capabilities query is card S51 (open).
- The build regenerates the misuse harness for the two new calls.

**Built on the Mac, 2026-11-17 (Apple M5 Pro, macOS 27; the `macos-metal`
trees), from the draft as approved (version 9).** Metal built and run;
Vulkan written and type-checked (`clang -fsyntax-only` against the Vulkan
SDK 1.4.363.0 headers here), not compiled; D3D12 written, not compiled.
- **Public (`crgpu.h`):** the 28 ASTC LDR formats at 1000 to 1027; bits 16
  (`CRGPU_FEATURE_TEXTURE_COMPRESSION_BC`) and 17 (`_ASTC_LDR`);
  `crgpu_format_get_texel_block()`; `crgpu_image_get_buffer_copy_row_pitch_alignment()`;
  both copies returning `CR_RESULT`; the copy desc's, copy size's, offset
  query's, image create's, pipeline's and blit's comments as the draft has
  them; the 28 names in `crgpu_format_from_string()`. No version raise (0.4.0
  untagged).
- **Shared (`src/gpu/`):** one table of every format's texel block and its
  family (`crgpuformat.c`, by value up to `V408`, ASTC's 28 from 1000; it
  replaces `crgpu___internal_format_block_bytes()`); the creates' block checks
  (`crgpu___internal_check_image_block`: feature, usage, type, whole blocks,
  after the `UNKNOWN` check, in every backend's desc check, so the placed
  create and the placement query get them too); the pipeline's refusal of a
  block format as a colour or depth attachment or (with a vertex module) a
  vertex attribute, beside the colour attachment checks; and the copies'
  checks after their handles and desc (`crgpu___internal_check_buffer_image_copy`:
  format, usages in parameter order, then the region: level, layers (0
  refused), sizes (0 refused), extent, the block's rules, offset, row pitch,
  each later layer's start, the buffer's range, in 64-bit arithmetic held
  at its maximum where it would wrap), which also hands each backend the
  layout in rows of blocks; the tight copy size for Vulkan and Metal.
- **Metal:** BC (every format but TYPELESS) and the 28 ASTC formats in the
  table, ASTC in a second array from 1000; BC from
  `supportsBCTextureCompression`, ASTC LDR from `MTLGPUFamilyApple2`; bytes
  per row and per image from the layout; both alignments the block's bytes.
- **Vulkan (written):** the 28 ASTC formats mapped; `textureCompressionBC` and
  `textureCompressionASTC_LDR` read with the other features and enabled only
  for a device created with each bit (it never enabled BC before); copies
  pass `bufferRowLength`/`bufferImageHeight` in texels, `layerCount` and depth
  as given (0 no longer becomes 1); offset alignment the least common
  multiple of the block's bytes and 4, row pitch the block's bytes. The swap
  chain's images now carry a desc (format, size, one level and layer, 2D,
  the colour attachment and caller's usages), as D3D12's and Metal's do: they
  were zeroed, which the copies' checks would have refused as a format with
  no block.
- **D3D12 (written):** BC on every adapter, ASTC on none; the cast returns
  `DXGI_FORMAT_UNKNOWN` from 1000 on; `crgpu_d3d12_make_footprint()` from the
  layout (row pitch in rows of blocks, width and height whole blocks, each
  layer `layer_bytes` on), the readback's box right and bottom rounded up to
  the block; the copy size 0 for a planar format; alignments 512 and 256.
- **Tests, each seen failing first:** before the header, 32 compile errors
  (the new calls, values and the copies' `void`). With the header and
  Metal's new signatures alone: `crgpu_device_test`'s bits unreported (2
  checks); `crgpu_objects_test`'s BC and ASTC creates refused as
  `CR_RESULT_GPU_INVALID_FORMAT` where `_FEATURE_NOT_SUPPORTED` was due, and
  a BC7 colour attachment reaching Metal's pipeline creation, which its
  validation ended (exit on assert). A mutation run (Metal's bytes per row
  one block too wide in both directions) failed the round trips from each
  level's second row of blocks, as pass 6 says it must.
- **Run, Debug, Release, `-asan` and `-tsan`:** 15 ctests each, passing,
  under Metal API and shader validation with errors and warnings as
  asserts. `crgpu_device_test`: BC yes, ASTC LDR yes on the M5 Pro, as Metal
  says; a device with each bit reports it. `crgpu_objects_test`: 126 images
  (14 BC and 28 ASTC formats, each a 2D image, a 3-layer array and a cube
  with full chains) written in whole with padded rows and read back a row of
  blocks at a time and as a sub-region, 4,642 copies, 22,700 blocks
  compared, none wrong; every creation, pipeline and copy refusal in order,
  nothing recorded (an image read back unchanged, a sentinel buffer kept);
  the tight 64-byte row and the 3-layer copy of a 1-block level taken.
  `cr_misuse_tests` ten times in Debug: 276 calls with zeroes, 240 with a
  destroyed handle, no failure. `cr_noalloc_tests`, Release: a 64 x 64 BC7
  level written each frame with its block and row pitch alignment asked,
  none of ours (21 a frame of Metal's own), 151 us a frame.
- **Owed:** Windows 11 (D3D12 on the Iris Xe, A2000 and WARP: BC, every ASTC
  create refused; Vulkan on the Iris Xe and A2000) and Fedora (Vulkan on
  ANV, NVK, RADV), each Vulkan driver's two booleans recorded here; Vulkan's
  ASTC path runs only for its refusals until a GPU that samples ASTC over
  Vulkan is at hand. crgpusamples' `CrGPUDynamicIndexing` uploading its BC1
  texture (Windows). The iOS simulator was not built this session.
- **Found while building, not in the draft (for a card):** a
  `buffer_row_length` below `width`, or a `buffer_image_height` below
  `height`, is not refused, so rows or slices overlap and reach the API
  (Vulkan's valid usage, Metal's validation and D3D12's footprint each
  refuse it); and a 3D copy whose `buffer_image_height` exceeds its height is
  laid out differently on D3D12, whose footprint has no slice pitch of its
  own (as before this change). Card S52, built on the Mac 2026-11-18 (its
  section below).

**Drafted 2026-11-17:** the draft is `carrottus/crgpu_anisotropy_draft.h`, version 1 (2026-11-17, seven critique passes), for approval: the bit (18), `max_sampler_anisotropy` in the adapter's properties (16 on D3D12 and Metal, Vulkan's limit, 1 without the bit), `CR_RESULT_GPU_SAMPLER_ANISOTROPY_TOO_LARGE` (371), and Q1 to Q5 with recommendations; nothing is built. Found while drafting (each in the draft): D3D12's `crgpu_d3d12_filter()` ignores min/mag/mip above level 1; every backend turns a level of 0 into 1, and D3D12 and Metal truncate fractions where Vulkan keeps them; Metal's API validation ends the process at 0, 17 and 32; `crgpu.h`'s "feature limits are always chained" is already contradicted by `max_draw_indirect_count`. It recommends a card of its own for the sampler's other unchecked fields (an unknown filter or address mode, a NaN level of detail, min_lod above max_lod).

## Dual-source blending (card S50, decided 2026-11-17: a)

Opened 2026-11-17: a feature bit asked for at device creation and the
four `SRC1` blend factors appended, a pipeline naming one refused without
the bit. Needed for a caller's coloured transmittance in one draw. A
draft first, confirming the shader compiler emits a fragment's second
output on each backend. Decided 2026-11-17 with you as recommended.

Its feature bit is **bit 19** (`CRGPU_FEATURE_DUAL_SOURCE_BLEND`, the
name its draft confirms; S48 has 16 and 17, S49 18). The draft must state
the limit every backend shares: **one colour attachment** when a
pipeline's blend names a `SRC1` factor (Vulkan's
`maxFragmentDualSrcAttachments` is 1 where the feature exists; D3D12 and
Metal blend dual-source into render target 0 only), a pipeline with more
refused; and how the shader's second output is declared (Slang's, read on
Metal, D3D12 and Vulkan). Asked for by A55's third critique round.

**Drafted 2026-11-17:** the draft is `carrottus/crgpu_dual_source_blend_draft.h` (2026-11-17, version 1, seven critique passes, not approved): bit 19 and the four SRC1 factors (10 to 13), one colour attachment, and fixes for three faults found in the code as built: an unknown blend factor silently becomes ZERO (an unknown op three different ways), now refused; D3D12 handed a `*_COLOR` factor in an alpha slot; D3D12 writing a disabled target's factors. Slang's second output confirmed on SPIR-V, DXIL and Metal's default route (`[[vk::location(0)]] SV_Target0` and `[[vk::location(0), vk::index(1)]] SV_Target1`), not `METAL_ROUTE SLANG` (which emits a second attachment); questions Q1 to Q4, each recommended (a). Nothing is built.

**Draft approved 2026-11-17 with the user** (`crgpu_dual_source_blend_draft.h`
v1, every question as recommended). Not built (the meta repo's
`ROADMAP.md` names the session that builds it).

**Built on the Mac 2026-11-17** (card A55's session, `../ROADMAP.md` names its prompt;
card X25: Windows and Linux owed), from the approved draft as written:
- `CRGPU_FEATURE_DUAL_SOURCE_BLEND` (bit 19), the four `SRC1` factors (10
  to 13), `CR_RESULT_GPU_INVALID_BLEND_FACTOR` (290) and `_BLEND_OP` (291)
  returned where they were reserved, `CR_RESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS`
  (226) grown by the one-attachment rule. The checks are in
  `crgpu___internal_check_pipeline_outputs()` (`src/gpu/crgpushared.c`,
  every backend): after the depth bias clamp, each enabled attachment's
  factors and ops in index and field order, then independent blend, then
  the feature, then the count. Every backend's factor and op switch now
  asserts in its default (unreachable).
- **Metal** built and run: the four `Source1` factors, the bit on every
  adapter. **Vulkan** written and type-checked on the Mac (`clang
  -fsyntax-only` against Vulkan SDK 1.4.363.0's headers): the factors, the
  bit from `dualSrcBlend` and `maxFragmentDualSrcAttachments >= 1`,
  `dualSrcBlend` enabled only with the bit. **D3D12** written, not
  compiled: the factors, a `*_COLOR` factor in an alpha slot written as its
  `*_ALPHA` twin (Q4 a), a disabled target and target 0 with no colour
  attachment written as D3D12's default blend state, the bit on every
  adapter.
- Tests: `crgpu_device_test` (the bit as each API reports it, a device with
  and without it), `crgpu_objects_test` (blends A to D drawn into an
  `R16G16B16A16_FLOAT` target and read back bit for bit, 0 values wrong;
  16 of 16 SRC1 placements refused without the bit; the count with and
  without independent blend; garbage factors and ops in each field refused
  on an enabled attachment and accepted on a disabled one; the order), each
  seen failing first. The test shader
  `tests/shaders/objects_dual_source_frag.slang` on the default Metal
  route. carrottus 15/15 Debug and Release under Metal API and shader
  validation with warnings fatal (Apple M5 Pro, macOS 27). The misuse
  harness regenerated: unchanged, as expected. ABI: four constants and the
  bit's macro added, nothing else.
- Owed: Windows 11 (Iris Xe, RTX A2000, WARP over D3D12 under the debug
  layer and GPU-based validation; the Iris Xe and the A2000 over Vulkan
  under validation and synchronization validation) and Fedora (ANV, NVK,
  RADV), each Vulkan driver's `dualSrcBlend` and
  `maxFragmentDualSrcAttachments` recorded here (the device test prints
  both).

## Per-format capabilities (card S51, decided 2026-11-17: a)

Found 2026-11-17 by A55's third critique round: Carrottus GPU reports no
per-format capabilities, though Vulkan leaves blending into
`R32G32B32A32_SFLOAT` and `B10G11R11_UFLOAT` optional and a caller blends
over both unchecked. Decided with the user:
`crgpu_adapter_get_format_capabilities(adapter, format, out_flags)`,
sampled, filtered, storage, colour attachment, blend, depth attachment,
vertex and copy, each bit read from the API (Vulkan's format properties,
D3D12's `CheckFeatureSupport(FORMAT_SUPPORT)`, Metal's documented
pixel-format tables by GPU family); a pipeline blending into a format
without the blend bit refused. A Carrottus draft first (seven critique
passes), read with S48, S49 and S50.

Drafted 2026-11-18: `carrottus/crgpu_format_capabilities_draft.h` (version 1, seven critique passes), not approved; nothing built.
**Approved 2026-11-18 with the user**, every question as recommended.

**Built on the Mac, 2026-11-18 (Apple M5 Pro, macOS 27; the `macos-metal`
tree), from the draft as approved.** Metal built and run; Vulkan written
and type-checked (`clang -fsyntax-only` against the Vulkan SDK 1.4.363.0
headers here), not compiled; D3D12 written, not compiled.
- **Public (`crgpu.h`):** `CRGPU_FORMAT_CAPABILITIES` and its nine values
  (written `1 << n`, as `CRGPU_IMAGE_USAGE`'s: `check_abi.py` reads an
  enumerator written `1u << n` as the one before it plus 1, so the
  snapshot had 0 to 8 until they were respelled; the values are the
  draft's), `crgpu_adapter_get_format_capabilities()`; the comments of
  `CRGPU_IMAGE_USAGE_STORAGE`, `blend_attachments` and
  `crgpu_graphics_pipeline_create()`'s `CR_RESULT_GPU_INVALID_FORMAT`, and
  `crcore.h`'s 207, as the draft has them. ABI: eleven lines added (the
  function, the typedef and nine constants; the draft said ten), nothing
  changed.
- **Shared (`crgpushared.c`, `crgpuformat.c`):** a 161-slot table in the
  adapter's facts (`crgpu___internal_format_index()`), the masks every
  backend shares (Q3's formats COPY at most; no COPY without a texel block
  or for the eight formats of the depth-stencil family, two aspects on some
  backend: the two depth-stencil formats, their TYPELESS resource formats
  and their four view formats), the call's checks after the adapter's
  (`CR_RESULT_GPU_INVALID_FORMAT`, then `CR_RESULT_NULL_OUTPUT_PARAMETER`),
  and the blend refusal last in `crgpu___internal_check_pipeline_outputs()`,
  which now takes the adapter's facts. Q3's "4:2:2 packed" formats are the
  five with a 2 x 1 texel block (R8G8_B8G8, G8R8_G8B8, YUY2, Y210, Y216):
  Vulkan makes all five `_422` formats.
- **Metal (`crgpumetalformat.m`):** Apple's "Metal Feature Set Tables",
  the edition dated **October 23, 2026** (downloaded from
  developer.apple.com/metal/Metal-Feature-Set-Tables.pdf on 2026-11-18;
  pages 11 to 15, "Texture capabilities by pixel format"), transcribed by
  family for Apple7 to Apple10. The edition has no Mac2 column; its note 1
  ("All Metal 3 and Metal 4 devices support at least the texture
  capabilities listed in the Apple7 column") gives a GPU in none of
  Apple7 to Apple10 the Apple7 column. The device wins where it answers:
  `supportsBCTextureCompression`, `MTLGPUFamilyApple2` (ASTC),
  `supports32BitFloatFiltering` (R32, RG32 and RGBA32 float), and
  `readWriteTextureSupport` with the MTLReadWriteTextureTier documentation's
  lists (Tier 1: R32Float, R32Uint, R32Sint; Tier 2 adds RGBA32 and RGBA16
  float, uint and sint, RGBA8Unorm, RGBA8Uint, RGBA8Sint, R16 float, uint
  and sint, R8Unorm, R8Uint, R8Sint). Table differences between columns,
  for the formats mapped: R32, RG32 and RGBA32 float and Depth32Float and
  Depth32Float_Stencil8 filter from Apple9.
- **Vulkan:** `vkGetPhysicalDeviceFormatProperties2` with
  `VkFormatProperties3` per named format, in
  `crgpuvulkan___read_adapter_facts()`. Fault 1 fixed:
  `CRGPU_FORMAT_B5G6R5_UNORM`, `_B5G5R5A1_UNORM` and `_B4G4R4A4_UNORM` are
  `VK_FORMAT_R5G6B5_UNORM_PACK16`, `_A1R5G5B5_UNORM_PACK16` and
  `_A4R4G4B4_UNORM_PACK16` (DXGI's layout).
- **D3D12:** `CheckFeatureSupport(FORMAT_SUPPORT)` on the probe device per
  named format, SAMPLED and FILTERED of `crgpu_d3d12_srv_format()` (moved
  to `crgpud3d12.hpp`), a failed call that format's 0. COPY of D16_UNORM
  and D32_FLOAT is TEXTURE1D/2D/3D as for any format; whether D3D12 copies
  a region smaller than such a subresource is owed to the Windows run.
- **Tests, each seen failing first:** against a stub that wrote 0 and
  checked nothing, `crgpu_device_test` failed 114 checks (every refusal,
  90 formats against the transcribed table and the invariants, four of the
  exact values), and `crgpu_objects_test` ended at Metal's validation
  (exit 134: "Blending is enabled for render target 0; however, the
  pixelformat MTLPixelFormatR32Uint for this render target is not
  blendable"), the refusal not yet made. Then:
  - `crgpu_device_test`, every adapter: all 146 named formats answered and
    the table printed, the refusals (NULL adapter, NULL output, formats 0,
    116, 129, 133, 999, 1028, UINT32_MAX; the two orders) with the sentinel
    unchanged, the invariants, on Metal every format against the test's
    own transcription, and the draft's exact values on an Apple10 GPU.
  - `crgpu_objects_test`: the refusal (an R32_UINT attachment blended,
    UNKNOWN blended, R8G8B8A8_UNORM blended beside an unblended R32_UINT,
    and attachment 1 blended as R32_UINT; the order behind a bad factor and
    behind an SRC1 factor without the feature; every named format blended,
    made exactly where BLEND is reported: 23 made, 123 refused on the M5
    Pro), then every bit the adapter reports used once under Metal API and
    shader validation with warnings fatal: 90 formats, 451 uses, 260 read
    back and compared (Load per class, a linear sample between two
    columns, a storage write and a read-write read per class with the
    format unknown to the shader, a cleared and drawn attachment, ONE / ONE
    over a known value, a depth test with the depth loaded and filtered, a
    vertex fetch of an encoded value, a copy up and back). Every "yes" of
    the transcription ran clean. B5G6R5_UNORM's layout check skips on
    Metal (the backend maps no such format); on Vulkan and D3D12 it reads
    the cleared texel as a 16-bit word, 0xF800.
  - `cr_noalloc_tests`: the call once an offscreen frame, none of ours.
  - The misuse harness regenerated: the new call added (zeroes, and a
    destroyed adapter), nothing else.
- **The M5 Pro's table** (Apple10, `supports32BitFloatFiltering` yes,
  read-write tier 2; SAMPLED 0x1, FILTERED 0x2, STORAGE 0x4,
  COLOR_ATTACHMENT 0x8, BLEND 0x10, DEPTH_STENCIL_ATTACHMENT 0x20, VERTEX
  0x40, COPY 0x80, STORAGE_READ 0x100): 0x1df for R32G32B32A32_FLOAT,
  R16G16B16A16_FLOAT, R8G8B8A8_UNORM, R32_FLOAT, R16_FLOAT, R8_UNORM;
  0x0df for the other unorm, snorm and float formats with a vertex format
  (R16G16B16A16_UNORM and _SNORM, R32G32_FLOAT, R10G10B10A2_UNORM,
  R8G8B8A8_SNORM, R16G16_FLOAT, _UNORM, _SNORM, R8G8_UNORM, _SNORM,
  R16_UNORM, _SNORM, R8_SNORM); 0x09f for R11G11B10_FLOAT,
  R8G8B8A8_UNORM_SRGB, B8G8R8A8_UNORM and _SRGB; 0x1cd for the uint and
  sint formats of the read-write tier (RGBA32, RGBA16, RGBA8, R32, R16,
  R8); 0x0cd for the other uint and sint formats with a vertex format
  (R32G32, R16G16, R8G8); 0x08d for R10G10B10A2_UINT; 0x040 for the three
  R32G32B32 formats; 0x0a3 for D16_UNORM and D32_FLOAT, 0x023 for
  D32_FLOAT_S8X24_UINT; 0x083 for every BC format but TYPELESS and every
  ASTC LDR format; 0 for the other 56.
- **Callers, rerun on the M5 Pro under Metal API and shader validation
  with warnings fatal, Debug and Release:** a caller's 47 ctests and its
  GPU tests, and a second caller's 165 ctests, all pass; nothing they
  blend into is refused on Metal (the float targets R16G16B16A16_FLOAT,
  R32G32B32A32_FLOAT and R11G11B10_FLOAT a caller's renderer blends into
  all report BLEND on Apple10). What a Vulkan driver
  without BLEND for R32G32B32A32_FLOAT or R11G11B10_FLOAT now refuses is
  owed to the Windows and Fedora runs.
- **Instance creation, timed on the M5 Pro (Release, 50 creations):**
  best 1.0 us before the table, 3.0 us with it; the means (120 to 200 us)
  are noise either way. Owed on each other machine (Vulkan's 146 format
  queries, D3D12's up to two `CheckFeatureSupport` per format).
- **Owed:** Windows 11 (Iris Xe, RTX A2000 and WARP over D3D12 under the
  debug layer and GPU-based validation; the Iris Xe and the A2000 over
  Vulkan under validation and synchronization validation) and Fedora (ANV,
  NVK, RADV): compile both backends, each adapter's table printed by
  `crgpu_device_test` recorded here, `crgpu_objects_test` on every adapter
  (each reported bit used under validation; B5G6R5's 0xF800, fault 1's
  test), D3D12's COPY of D16_UNORM and D32_FLOAT against a partial region,
  instance creation timed before and after, and a caller's GPU tests on
  every adapter with what each Vulkan driver now refuses (a blend into
  R32G32B32A32_FLOAT or R11G11B10_FLOAT where its BLEND bit is missing).
- **For a card (the draft's Q6):** the other refusals the table allows
  (an image's usages against its format's bits; a pipeline's colour and
  depth formats and vertex attributes; a combined depth-stencil copy,
  the draft's fault 2; Vulkan's stand-ins refused at creation).

**Card S58 decided 2026-11-20 with the user (a):** one Carrottus draft
(seven critique passes), **not yet written**: each use refused without
its format's bit, read from the table, everywhere (an image's usages,
colour and depth attachments, vertex attributes, copies); a combined
depth-stencil image's copy refused unless the caller names one aspect (a
chained struct on the copy), so the three backends no longer do three
different things; Metal's FloatRG11B10 and BGRA8 vertex formats mapped.

**Card S58 decided 2026-11-20 with the user (a).** Drafted 2026-11-20: `carrottus/crgpu_format_uses_draft.h` version 2, seven passes, for approval; not built.

## A copy's buffer rows shorter than the image's (card S52, decided 2026-11-17: a)

Found 2026-11-17 by S48's build: a copy's `buffer_row_length` or
`buffer_image_height` above 0 and below the region's isn't refused, so
overlapping rows reach the API (Vulkan forbids it, D3D12 can't express
it); D3D12 lays out a 3D copy's taller image height differently from
Vulkan and Metal. Decided with the user: each refused in every build with
its own code, as S48's other copy checks (in texels rounded to whole
blocks); D3D12's taller 3D slice pitch refused until a backend can lay it
out as the others. A listed break: a Carrottus draft first (seven
critique passes).

Drafted 2026-11-18: `carrottus/crgpu_copy_rows_draft.h` (version 1, seven critique passes), not approved; nothing built.
**Approved 2026-11-18 with the user**, every question as recommended.

**Built on the Mac, 2026-11-18 (Apple M5 Pro, macOS 27; the `macos-metal`
tree), from the draft as approved, after S51.** The checks are shared
(`crgpu___internal_check_buffer_image_copy()`, `crgpushared.c`), so the
Metal build runs them for every backend; no backend's translation
changed, and Vulkan's and D3D12's files are untouched.
- **Public:** `CR_RESULT_GPU_COPY_ROW_LENGTH_TOO_SHORT` (372),
  `_IMAGE_HEIGHT_TOO_SHORT` (373) and `_SLICE_PITCH_NOT_SUPPORTED` (374)
  in `crcore.h`, named by `cr_result_str()` (371 left to S49's draft); the
  copy desc's rules and its two fields' comments, and both copies'
  results, as the draft has them. ABI: three constants added, nothing
  changed. The misuse harness regenerated: unchanged, as expected.
- **The checks, in the draft's place (Q2 a):** after the block's rules and
  before the offset, in blocks (the region's blocks across and down worked
  out before them, the values unchanged): a row length above 0 of fewer
  blocks than the region's, an image height above 0 of fewer rows of
  blocks, a copy of depth above 1 with an image height above its height
  (Q1 a, every backend); after the row pitch's alignment, a pitch above
  2^31 - 1 bytes, `CR_RESULT_DESC_INVALID_PARAMETERS` (Q3 a).
- **Tests, each seen failing first:** before the checks, under Metal's
  validation the first short row length ended the process (exit 134:
  "sourceBytesPerRow(16) must be >= (32)"); without validation 29 checks
  failed, every refusal case accepted and its bytes moved (the short row
  length and image height cases both ways, BC7's, the 3D taller slice, the
  3D short image height, the order case behind a misaligned offset
  answering `CR_RESULT_DESC_INVALID_PARAMETERS`, and the 2 GiB pitch
  accepted both ways). Then `crgpu_objects_test`'s `test_copy_rows`,
  under Metal API and shader validation with warnings fatal, Debug and
  Release: 28 cases, 13 refused with nothing recorded (the region written
  beforehand reads back unchanged and a sentinel-filled buffer keeps every
  byte), 15 accepted and round-tripped byte for byte (the gaps between
  rows keeping the sentinel): R8G8B8A8_UNORM 8 x 8 (row length 4 and 7
  refused; 8, 0 and 64 taken; image height 4 refused; 8, 0 and 12 taken),
  BC7_UNORM 12 x 12's level 1, 6 x 6 (row length 4 refused, 6 the block
  rule's, 8 and 64 taken, image height 4 refused, 8 taken, the edge block
  alone at row length 4 and 64 taken), a 3D 4 x 4 x 4 (depth 4 with image
  height 8 refused, 4 and 0 taken, depth 1 at z 2 with 8 taken, depth 4
  with 2 the short height's), a 2D array of 3 with image height 8 taken,
  the order (a short row length before a misaligned offset; a missing
  transfer usage before a short row length; a short row length before a
  short image height), and a row pitch of 2^31 bytes (a DEVICE buffer of
  2^31 + 256 bytes made on the M5 Pro) refused both ways, its first bytes
  keeping their sentinel. G8R8_G8B8_UNORM's case skips on Metal (no such
  image: `CR_RESULT_GPU_INVALID_FORMAT`). S48's region refusals keep
  `CR_RESULT_DESC_INVALID_PARAMETERS`, unchanged. `crcore_test` checks the
  three names.
- **Callers, rerun on the M5 Pro under Metal API and shader validation
  with warnings fatal, Debug and Release:** a caller's 47 ctests and its
  GPU tests, and a second caller's 165 ctests, all pass: none of their
  uploads or readbacks is refused.
- **Owed:** Windows 11 (Iris Xe, RTX A2000 and WARP over D3D12 under the
  debug layer and GPU-based validation; the Iris Xe and the A2000 over
  Vulkan under validation and synchronization validation) and Fedora (ANV,
  NVK, RADV): `crgpu_objects_test` there (the packed 2 x 1 case where
  G8R8_G8B8_UNORM can be made; the accepted cases at D3D12's 256-byte
  pitch), and a caller's GPU tests on every adapter.
- **For a card (the draft's Q1 c):** laying out a 3D copy's taller slice
  pitch on D3D12 (a footprint as tall as the image height with a source
  box, or one copy per slice where each start meets 512 bytes), measured
  on Windows; then the refusal lifted on every backend at once.
  **Card S59 decided 2026-11-20 with the user (b):** the refusal
  (`CR_RESULT_GPU_COPY_SLICE_PITCH_NOT_SUPPORTED`) is kept on every
  backend until a caller needs such a copy; nothing to build until then.

## Indirect dispatch (card A15, decided 2026-10-28; draft approved 2026-11-18)

`crgpu_dispatch_indirect_draft.h`, approved with the user on 2026-11-18,
every question as recommended (Q1 mesh indirect in; Q2 Metal's threadgroup
size from the bound pipeline's module; Q3 the counts' rule; Q4 the bound
state's rule, no check; Q5 four codes, 375 to 378).

**Built on the Mac, 2026-11-18 (Apple M5 Pro, macOS 27; the `macos-metal`
tree): the compute half.** Metal built and run; Vulkan's files
type-checked against the Vulkan SDK 1.4.363.0's headers (`clang
-fsyntax-only`, Debug and Release); D3D12's written, not compiled.
- **Public:** `crgpu_command_list_dispatch_indirect()` (cmd, buffer,
  offset; a `CR_RESULT`) and the frozen `crgpu_dispatch_indirect_args` (12
  bytes, after `crgpu_command_list_dispatch()`);
  `CR_RESULT_GPU_COMMAND_INSIDE_RENDERING` (375),
  `_INDIRECT_OFFSET_ALIGNMENT` (377) and `_INDIRECT_RANGE` (378), named by
  `cr_result_str()`; 210's comment widened. 376
  (`_COMMAND_OUTSIDE_RENDERING`) is held for the mesh half, which alone
  returns it. Comments: `CRGPU_BUFFER_USAGE_INDIRECT`,
  `CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT`,
  `CRGPU_PIPELINE_STAGE_DRAW_INDIRECT`; `crgpu_command_list_dispatch()`
  gains Q3's and Q4's rules; `crgpu_command_list_dispatch_mesh()` loses
  its promised Debug assert (the draft's "Found in the code" 7) and gains
  Q3's rule: each count at most 65535, their product at most **1024**
  (below), and Q4's sentence. ABI: three constants, one struct and one
  function added, nothing changed. The misuse harness regenerated: the
  call with zeroes, and with its list destroyed.
- **The checks,** written once (`crgpu___internal_check_dispatch_indirect()`,
  `crgpushared.c`, with the struct's layout asserted) after each backend's
  null and stale checks of the list, then the buffer: a COPY or BUNDLE
  list, a pass open, no `CRGPU_BUFFER_USAGE_INDIRECT`, an offset not a
  multiple of 4, an offset past the size or fewer than 12 bytes after it.
- **Each backend:** Vulkan `vkCmdDispatchIndirect`; D3D12
  `ExecuteIndirect` with a DISPATCH signature made by
  `crgpu_device_create()` beside the tightly packed draw ones
  (`_dispatch_signature`; the signature maker now takes the argument type,
  `crgpu_d3d12___create_indirect_signature()`), released with them; Metal
  `dispatchThreadgroupsWithIndirectBuffer:` with the bound pipeline's
  workgroup size, unclamped, nothing recorded where Q4's rule is broken
  (no compute encoder or pipeline). Each file asserts the API's own
  struct is 12 bytes. Vulkan: a device created with mesh shading whose
  `vkCmdDrawMeshTasksEXT` loads as null now fails with
  `CR_RESULT_API_SPECIFIC_FAILURE` ("Found in the code" 8).
- **Q3's mesh limit, read from Apple's "Metal Feature Set Tables"** (the
  edition dated October 23, 2026, fetched again from
  developer.apple.com/metal/Metal-Feature-Set-Tables.pdf on 2026-11-18;
  page 9, "Maximum threadgroups per mesh shader grid"): Apple7 1024,
  Apple8 1024, Apple9 1,048,575, Apple10 4,194,303, Apple11 4,194,303;
  note 1, "All Metal 3 and Metal 4 devices support at least the limits
  listed in the Apple7 column". The narrowest every backend keeps is
  1024, written onto `crgpu_command_list_dispatch_mesh()`.
- **Stopped: the mesh half (Q1).** The same edition's page 8 lists
  "Indirect mesh draw arguments" (`drawMeshThreadgroupsWithIndirectBuffer:`)
  from Apple9 only, while every Metal adapter of this backend reports
  `CRGPU_FEATURE_MESH_SHADING` (Apple7 and Apple8 included: M1, M2).
  The draft's contract (refused only on a device without the feature)
  can't be kept on an Apple7 or Apple8 GPU, so
  `crgpu_command_list_dispatch_mesh_indirect()` was not built: no
  declaration, no PFN, no D3D12 mesh signature, no test 8. Its options
  are a card's.
- **Tests, seen failing first** (the call declared, Metal's body a stub
  returning `CR_SUCCESS` and recording nothing): without validation
  `crgpu_objects_test` failed 25 checks (the counters 0 against 12, 4, 4,
  2, 1 and 65535; every refusal answered `CR_SUCCESS`); under Metal's
  validation the first run ended at `unused binding in compute encoder`,
  which reshaped the refusal cases (below); `crcore_test` failed the three
  names. The zero-count case passes against a stub by its nature, and the
  no-allocation scenario can't fail on one; both were run after.
  Then, Debug and Release under Metal API and shader validation with
  warnings fatal (`test_dispatch_indirect`): objects_dispatch_ids_comp
  over {3, 2, 2} from an UPLOAD buffer the CPU wrote, the counter 12,
  each of 12 slots its own value (slot 12 65795), slot 13 the sentinel,
  equal byte for byte to the direct dispatch's; {4, 1, 1} written by
  objects_dispatch_args_comp, a barrier to INDIRECT_ARGUMENT, 4 groups, on
  a DIRECT and a COMPUTE list; offsets 16 and 52 of a 64-byte buffer
  ({1000, 1000, 1000} at 0 not run); {0, 5, 5}, {5, 0, 5}, {5, 5, 0}
  nothing run and no validation message (Q3's "a 0 dispatches nothing"
  holds on Metal); {65535, 1, 1} with objects_dispatch_count_comp, 65535;
  every refusal in the header's order, each submitted with the counter
  shader bound and a one-group direct dispatch after it, read back as the
  counter 1 and every slot its sentinel (0 on a COPY list): cmd NULL,
  buffer NULL, a destroyed buffer (Debug), a COPY list, a pass open, a
  buffer of STORAGE alone, offsets 2 and 6, offsets 56, 64, 68 and
  UINT64_MAX - 3, and the four pairs of the order; a BUNDLE list's
  refusal where `CRGPU_EXT_BUNDLES` is 1 (not on Metal). `cr_noalloc_tests`:
  the dispatch writing counts, the barrier and the indirect dispatch,
  submitted and waited, 0 of ours (5 a frame of Metal's).
  `cr_misuse_tests` ten times in Debug, no failure. The three shaders are
  in `CR_TEST_SHADERS` (cs_6_0).
- **Callers, rerun on the M5 Pro under Metal API and shader validation
  with warnings fatal, Debug and Release:** a caller's 47 ctests and its
  GPU tests, and a second caller's 165 ctests.
- **Owed:** Windows 11 (Iris Xe, RTX A2000 and WARP over D3D12 under the
  debug layer and GPU-based validation, D3D12's file compiled for the
  first time; the Iris Xe and the A2000 over Vulkan under validation and
  synchronization validation, the COMPUTE list on a compute-only family:
  "Found in the code" 5) and Fedora (ANV, NVK, RADV).
- **For cards (the draft's "Found in the code"):** 1 Metal's compute
  state lost at every ended encoder; 2 Metal's `_bound_compute_pipeline`
  never cleared at `crgpu_command_list_begin()`; 3 D3D12's one
  pipeline-state slot; 4 the indirect draws' missing usage, offset and
  range checks and their void shape; 5 Vulkan's barrier stages on a
  compute-only queue; 6 Metal's dead 0-to-1 workgroup clamp in the direct
  dispatches; and Q4's check of the bound state. Q3's CPU check of the
  direct dispatches' counts. The mesh half's options (above).

**Card S54 decided 2026-11-18 with the user (c now, a later):** indirect mesh dispatch is left out (code 376 held) until a caller needs it; then a feature bit of its own, `CRGPU_FEATURE_MESH_SHADING_INDIRECT` (Metal: Apple9 and later, per Apple's Metal Feature Set Tables of 2026-10-23, page 8), by a short draft.

**Card S55 decided 2026-11-20 with the user (a):** the indirect draws
(`crgpu_command_list_draw_indirect` and its siblings) return `CR_RESULT`
and take the indirect dispatch's checks, written once beside
`crgpu___internal_check_dispatch_indirect()`: no
`CRGPU_BUFFER_USAGE_INDIRECT` (221), an offset not a multiple of 4 (377),
a range past the buffer (378), on every backend ("Found in the code" 4).
A signature change on four calls, so a Carrottus draft first (seven
critique passes); **not yet written**.
Drafted 2026-11-20: `carrottus/crgpu_draw_dispatch_checks_draft.h` version 2, seven passes, for approval; not built.

**Card S56 decided 2026-11-20 with the user (a):** every backend tracks
what is bound and refuses a dispatch (and a draw) with nothing valid
bound, each its own code: Metal's compute state lost at an ended encoder
and its `_bound_compute_pipeline` cleared at
`crgpu_command_list_begin()`, D3D12's one pipeline-state slot told apart
by kind ("Found in the code" 1 to 3, Q4's check); the direct dispatches'
counts checked on the CPU against 65,535 (Q3's). Three backends behaving
three ways breaks Carrottus's rule. A Carrottus draft first (seven
critique passes); **not yet written**.
Drafted 2026-11-20: `carrottus/crgpu_draw_dispatch_checks_draft.h` version 2, seven passes, for approval; not built.

**Card S57 decided 2026-11-20 with the user (a):** Vulkan's barrier stage
masks are taken from the list's queue family, graphics stages only on a
family that has them ("Found in the code" 5;
`crgpuvulkancommand.c` ~821-832). Verified first on the Windows and
Fedora Vulkan runs (the COMPUTE list on a compute-only family, under
synchronization validation), then fixed; a fix within the contract, no
public change.

## A recording dropped without a submit (card S53, decided 2026-11-18: a)

Found 2026-11-17 and 18 by a caller's GPU tests: on Metal, a command list
begun, recorded into and then begun again, its allocator reset or the
list destroyed, never submitted, kept its `MTLCommandBuffer`'s place in
its queue. A queue hands out a fixed number at once (`maxCommandBufferCount`,
64 by default), after which `-commandBuffer` blocks for good. Decided with
the user (a): the backend finds what keeps such a buffer alive and lets
go of it at the list's next begin, its allocator's reset and its destroy,
ending an open encoder first and never committing it (that would run what
was recorded). A fix within `crgpu.h`'s contract (a recording is
submitted once, or not at all); no public change, no ABI change.

**Cause, measured 2026-11-18 on the Mac (Apple M5 Pro, macOS 27).** Three:
1. Every encoder (`-renderCommandEncoderWithDescriptor:`,
   `-computeCommandEncoder`, `-blitCommandEncoder`) was returned
   autoreleased and retained by the list; the list released its retain at
   the encoder's end, but the autoreleased reference waited for a pool of
   the caller's, which a plain C caller with no run loop never drains. An
   encoder keeps its command buffer, so every recorded buffer outlived
   its release, and one never committed never gave its place back. (The
   buffer itself was already taken in a pool of the backend's own; the
   acceleration-structure encoders were already made in one.) With only
   this fixed, a list begun again, reset or destroyed after a whole
   recording ran 200 times each.
2. A list destroyed (or begun again) with an encoder still open (a
   recording stopped inside a pass, as a refused one is) released its
   buffer but not the open encoder, which kept the buffer for good, and
   the encoder was never ended. With 1 fixed, 200 lists destroyed inside
   a pass still blocked.
3. A list took its buffer when created and held it, uncommitted, until
   its first begin, and after a submit held the committed one until its
   next begin. Found by the fix's first shape, which gave a reset list a
   fresh buffer at once: a caller's GPU suite then blocked in the reset
   itself (`crgpu_command_allocator_reset`, its many pooled lists each
   holding an uncommitted buffer). The old code had the same limit at
   creation: a 65th list alive and never begun blocked
   `crgpu_command_list_create()`. 200 lists of one allocator alive at once
   blocked so.

**Fix (`crgpumetalcommand.m`, `crgpumetalrenderpass.m`,
`crgpumetalswapchain.m`, `crgpumetal.h`).**
- Each encoder is made inside an `@autoreleasepool` of its own and
  retained there, so the retain is its only reference when the call
  returns.
- A list holds a command buffer only while it holds a recording: none
  when created; a new one taken at each `crgpu_command_list_begin()`.
- `crgpumetal___internal_drop_recording()`: ends and releases whichever
  encoder is open, then releases the buffer, never committing it (a
  committed one Metal keeps until it has run). Called by
  `crgpu_command_list_begin()` before it takes the new buffer, by
  `crgpu_command_allocator_reset()` for each of the allocator's lists,
  and by `crgpu_command_list_destroy()`. An allocator now links its lists
  (`_first_list`; `_next_in_allocator` / `_prev_in_allocator`) so its
  reset can reach them.
- `crgpu_get_mtl_command_buffer()` (`crgpu_metal_natives.h`): its comment
  now says it is the current recording's buffer, nil before the list's
  first begin and after its allocator's reset (before, a created list
  had one). No signature changed; nothing in the workspace calls it.
- `crgpu_command_queue_wait_idle()`'s buffer and the swap chain's two
  (at creation and each present) are taken inside pools of their own too:
  committed, they never blocked, but each leaked for a plain C caller.

**Vulkan and D3D12: no analogue, unchanged.** A Vulkan list's begin
resets its buffer (`vkResetCommandBuffer`, legal in the recording or
executable state), the allocator's reset is `vkResetCommandPool` and a
destroy `vkFreeCommandBuffers`, legal in any state but pending; D3D12's
begin is `Reset()` on the closed list, its destroy a `Release()`, legal
open. Neither has a per-queue count of command buffers, so a recording
never submitted holds nothing beyond its allocator's memory, which the
allocator's reset gives back as `crgpu.h` says.

**Test (`tests/crgpu_objects_test.c`, `begun_again_without_submit()`).**
Run on every backend, after the draw test's uploads and before its
frame's and the compute test's real submits (which are then still drawn
and read back right), for a whole recording (the draw test's frame; the
compute test's copies and dispatch): begun again after its end 200
times; the allocator reset before each of 200; 200 lists created,
recorded and destroyed; 200 lists of the allocator alive at once, each
recorded, submitted and waited for, the allocator reset, twice; and 200
lists destroyed open (the frame stopped inside its pass; the compute
recording before its end), never submitted. A watchdog thread fails the
test after 60 s rather than leave it blocked. Seen failing first: before
any fix, blocked in the frame's loops (the watchdog's message after 60 s,
exit 1); with only the encoders' pools, the three whole-recording loops
passed for both recordings and the frame's destroyed-open loop blocked;
with the first shape of the fix (a buffer taken at creation and at
reset), the 200 lists alive at once blocked.

**Built and run on the Mac, 2026-11-18 (Apple M5 Pro, macOS 27; the
`macos-metal` tree), Debug and Release, under Metal's API and shader
validation with errors and warnings as asserts:** carrottus' 15 ctests;
a caller's 53 ctests; that caller's GPU suite on both of its device
passes, three times in a row in each configuration, with neither of the
workarounds the bug had forced on it (a pool drained round every test, a
two-thread test cut to 3 rounds of 20); a second caller's 173 ctests (171
run in Release). `check.sh abi` shows no change. **Owed:** Windows 11 and
Fedora run the new test over D3D12 and Vulkan (no backend file changed
there; the test is backend-neutral and holds every list's contract).

## Compute that overlaps on Metal (card S60, decided 2026-11-20: a)

Found 2026-11-19 by a caller's measurements of a GPU particle step: the
Metal backend records compute into one serial encoder and its barriers
are no-ops (it relies on Metal's hazard tracking), so independent
dispatches never overlap. 100 small systems in one command cost half of
100 commands' (1.2 against 2.5 ms on the M5 Pro), not the sixth the
caller expected, and a step of 100,000 particles ran three to five times
over its estimate. Vulkan and D3D12 overlap dispatches between barriers.
Decided with the user (a): the compute encoder is made with
`MTLDispatchTypeConcurrent`, and `crgpu_command_list_pipeline_barrier()`
becomes Metal's `memoryBarrierWithScope:` (buffers, textures), with
untracked resources where the caller's barriers already order them, so
Carrottus GPU's barriers are real on Metal as they are on the other two.
Not b (leave it, the cost recorded). A Carrottus draft first (seven
critique passes; what changes for a caller that leaned on Metal's
tracking, and the heaps' tracked mode above); **not yet written**.
Drafted 2026-11-20: `carrottus/crgpu_concurrent_compute_draft.h` version 2, seven passes, for approval; not built.
