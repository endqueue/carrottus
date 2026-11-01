# SpudLib

Hardware Abstraction Layer (HAL). This is a from-scratch
translation layer over GPU, file system, memory, network, and audio primitives — a general HAL,
not a GPU library with utilities bolted on. See `../CLAUDE.md` (the `eqdev` workspace
root) for how this repo fits into the workspace; this file covers
conventions specific to working inside `spudlib` itself. `MATURITY_TODO.md`
is the plan for bringing the parts written before these rules up to them
(a version, result names, the caller's allocator (done) and a stability
policy for every module; a rework of SpudFiles (done 2026-10-22); `bindings/`
is removed and SpudInput's struct bases are in).

## The one rule everything else follows

**SpudLib is pure mechanism, no hidden policy.** It translates a platform/GPU API into a
consistent C interface and stops there. Every choice is visible and the caller's
(worded so 2026-10-28, card S2): where a behaviour must exist at all, such as
where validation messages go, the caller supplies it. It never makes a decision on
the caller's behalf, never has a default that hides a choice, and holds no global state.

- Expose enumeration/query functions so the caller can inspect and
  decide — `spudgpu_enumerate_adapters`, not `spudgpu_select_best_device`.
- No "convenience" fallback that picks something for the caller. If a change feels
  like it wants one, that decision belongs to the caller, not here.
- Everything flows through explicit handles the caller owns and passes in — no
  library-owned globals.
- If a bug turns out to be SpudLib silently defaulting instead of translating a
  caller-supplied parameter (e.g. `spudgpu_create_image_view` once hardcoded
  `VK_IMAGE_ASPECT_COLOR_BIT` regardless of what the caller asked for), that's a
  design-principle violation, not just a bug — fix it by passing the real parameter
  through, not by adding a special case.
- When a fix spans SpudLib and its caller, make the correct structural change on
  both sides rather than working around a layering gap from one side only.
- **SpudLib doesn't know who calls it.** Nothing here - code, comments, headers,
  docs - names a caller or assumes which one it is. Write "the caller".

## Module map

Every module returns the shared `SPUDRESULT` enum (`spudcore.h`, 165 values with
`SPUD_SUCCESS`, as of 2026-10-23) rather than inventing its own error convention.

| Module | Header | Prefix | Backends |
|---|---|---|---|
| SpudGPU | `spudgpu.h` | `spudgpu_` | Vulkan, D3D12, Metal |
| SpudFiles | `spudfiles.h` | `spudfile_` | Windows, Linux, macOS |
| SpudMemory | `spudmemory.h` | `spudmem_` | Windows, POSIX (Linux + Apple, one file) |
| SpudNet | `spudnet.h` | `spudnet_` | TCP sockets: Winsock2, BSD sockets (Linux + Apple, one file). HTTP + WebSocket: WinHTTP, libcurl, NSURLSession |
| SpudAudio | `spudaudio.h` | `spudaudio_` / `SPUDRESULT_SAUD_*` | WASAPI (Windows), ALSA (Linux), CoreAudio (macOS); an argument-checking stub where no API is compiled in (iOS, watchOS) |
| SpudInput | `spudinput.h` | `spudinput_` / `SPUDRESULT_SPUDINPUT_*` | GameController (macOS, iOS, iPadOS), evdev (Linux), GameInput (Windows). Not written: XInput (Windows); with no API built in, no instance can be made |
| SpudCore | `spudcore.h` | `spud_` / `SPUDRESULT` | platform-agnostic |
| SpudPerf | `spudperf.h` | `spudperf_` | Windows, Linux, Apple |
| SpudRandom | `spudrandom.h` | `spudrandom_` | `BCryptGenRandom` (Windows), `getrandom` (Linux), `getentropy` (Apple). The operating system's secure random bytes, one call, no state: SpudLib has no generator of its own. Added 2026-10-31; macOS compiled and run (`spudrandom_test`). Run on Windows the same day (Windows 11, `windows-d3d12` and `windows-vulkan`, Debug, Release and `-asan`): its first compile there failed, `bcrypt.h` included before the `windows.h` it needs; fixed, `spudrandom_test` passes. Linux written, not compiled |

SpudGPU dominates the codebase (133 functions across 24 opaque handles) and is where
most work happens. The others are small, focused translation shims — don't let their
API surface grow beyond what they are: SpudNet is "move bytes," not a networking
framework (message framing, snapshot/delta semantics, host authority are all
the caller's problem). All three parts share one shape, set out at the
top of `spudnet.h` - `spudnet_<object>_<verb>` names, `_create`/`_destroy`, desc
structs with no defaults, one meaning for `timeout_ms` (`SPUDNET_NO_WAIT` stands in
for a non-blocking mode), `_abort` from any thread, and `SPUDRESULT_SPUDNET_*`
results - so a new call follows it rather than the platform API it wraps. TCP
addresses are numeric; name lookup is its own object (`spudnet_resolver`) because
the system's lookup can't be time-limited or interrupted: `spudnet_resolver_run`
blocks in it on a thread the caller supplies, `spudnet_resolver_wait` waits for
the answer on another. SpudNet itself starts no thread, anywhere. The HTTP and WebSocket clients follow the same
rule one level up: they translate the platform's own client stack (TLS, proxies and
framing are the stack's) and carry one request or one message, with every call
blocking on the caller's thread. HTTP is a streaming transfer
(`spudnet_http_transfer`: start, send the body in pieces, receive the response,
recv its body in pieces) - nothing of the caller's is buffered in SpudNet, and
there is no one-shot request to add back. Each backend keeps the caller's time limit itself,
as the whole call's allowance, with the stack's own timers switched off - don't hand
a limit to WinHTTP/NSURLSession/libcurl, they each mean something different by it -
and each blocking call can be ended from another thread
(`spudnet_http_transfer_abort`, `spudnet_websocket_abort`). Which server
certificates are accepted is the caller's, in a `spudnet_tls_desc` (trust mode,
DER roots, host-name switch, SHA-256 public-key pins) given per HTTP client and
per WebSocket connect; a zeroed one is the stack's own check. A failure's
`SPUDRESULT` is portable and the platform's own number behind it is not, so
each object keeps the latter in a `spudnet_error` for its `_get_error` - for
logs, never for deciding what to do - and a backend records it at the point it
returns the failure (`spudnet_error_record`). Waiting on several objects at
once is a `spudnet_wait_set` (`src/net/spudnetwaitset.c`, one `poll()`-based file
for every platform): it only says which objects are worth calling, and the
caller still makes the ordinary calls with `SPUDNET_NO_WAIT`. No retry, no
reconnect, no cookies, no keep-alive pings, and nothing that knows what a status
code or a message means. What is the same on every stack is written once: the
HTTP transfer's order of use, checks and header storage in
`src/net/spudnethttp.c`, with each stack behind the `spudnet_http_backend_*`
functions of `src/net/spudnetshared.h`; a new rule about a transfer goes in the
front end, not in three backends. Parts a platform doesn't allow are compiled
out by `SPUDNET_EXT_TCP` / `SPUDNET_EXT_WEBSOCKET` (both 0 on watchOS) - test
those, never a platform's name. SpudMemory is
the platform's virtual memory a page at a time (reserve, commit, decommit,
release), not an allocator: the arena it had decided alignment, growth and
zeroing for its caller and went on 2026-10-22 (`MATURITY_TODO.md`, item 3).
Nothing is rounded - a size or address that isn't a page multiple is
refused - and a committed page reads zero on every backend, after a decommit
too (POSIX decommits by mapping fresh pages over the range, since Apple's
`MADV_DONTNEED` keeps contents). The checks are in the front end,
`src/spudmemory.c`, and each backend only implements
`src/memory/spudmemoryshared.h`.
SpudAudio is "move PCM frames to and from an endpoint," not an audio engine: no
mixer, decoder, resampler, voices, spatialisation or volume policy (those are
the caller's). It never picks a device - the OS defaults
are reported as a property - and never silently converts a format: a stream the
endpoint can't take fails with `SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED` unless the
caller set `SPUDAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION`. Which native APIs are built
in is decided in `CMakeLists.txt` and published as PUBLIC
`SPUDAUDIO_PLATFORM_*` / `SPUDAUDIO_COMPILE_*` definitions, so a caller tests those
at compile time; the table is at the top of `spudaudio.h`.
SpudInput is "what devices are connected and what state they are in," read with
no window: gamepads only so far. Nothing that arrives as a window's event (keys,
the pointer, typed text) belongs in it, since SpudLib owns no window. It never
picks a device or numbers players, applies no dead zone or curve, lists only
what the platform itself presents as a gamepad (no table of other devices'
layouts), and starts no thread: the caller polls. A gamepad is named by a
`spudinput_gamepad_id` value, not a handle, so that one unplugged is told apart
from one still there. What a gamepad has beyond buttons and axes (rumble motors,
battery, motion sensors, touch surfaces, lights, labels) is a per-device feature
bit, and asking for one it lacks is `SPUDRESULT_SPUDINPUT_FEATURE_NOT_PRESENT`,
never a silent no-op. Outputs are set and held, with no durations or patterns;
motion is every sample between two polls, raw, in one stated frame, with no
orientation worked out; labels are only the platform's own words, and no table
of devices or button art lives here. The public API is one front end,
`src/spudinput.c`: every argument check and the table of gamepads and ids are
there, once, and a backend only finds devices and reads them into the front
end's records through the verbs listed in `src/input/spudinputshared.h`. A
new rule about a call goes in the front end, not in each backend. Any number
of native APIs may be built into one build (GameInput and XInput on Windows,
once XInput is written), and the caller's `native_api` chooses between them,
so the front end dispatches on it with one `switch` per verb, never a
function-pointer table. The backends are part of the front end's translation
unit: `src/spudinput.c` includes each one built in (CMake compiles none of
them alone, and the front end is compiled as Objective-C where GameController
is in), and each defines its verbs as `spudinput_<api>_<verb>` with
`SPUDINPUT_BACKEND_FUNCTION`, static and forced inline into its case. A
dispatched call is then a compare and a branch, with no call. A function the
compiler can't inline fails the build (always_inline on clang and GCC, C4714
as an error on MSVC), except that MSVC inlines nothing at `/Ob0`, its Debug
default. On 2026-10-20 the macOS Debug and Release objects held no
`spudinput_gamecontroller_*` function, only the blocks inside them; the
GameInput and evdev builds were not run.

## SpudGPU conventions

- **Opaque handles only.** Every resource type (`spudgpu_instance`, `spudgpu_device`,
  `spudgpu_buffer`, `spudgpu_command_list`, ...) is a forward-declared pointer with no
  caller-visible fields. Backend-private struct layout lives in an internal header per
  backend (`spudgpuvulkan.h`, `spudgpud3d12.hpp`) — never in the public `spudgpu.h`.
- **Dynamic rendering, not render-pass objects, as the default path.** The renderpass
  concept was removed in favor of `spudgpu_cmd_begin_rendering`/`cmd_end_rendering`
  (commit `e90f921`). Don't reintroduce a `VkRenderPass`/`VkFramebuffer`-style object
  into the public API as the default — D3D12's backend already emulates this begin/end
  shape on top of its native render-target-view binding, not the other way around.
  **Designed, not built:** no `SPUDGPU_LEGACY_*` macro, `spudgpu_framebuffer` or
  `spudgpu_cmd_begin_rendering_legacy` exists in the code yet; the rest of this
  bullet is the plan for when one is needed.
  If/when SpudLib targets hardware that lacks Vulkan 1.3/`VK_KHR_dynamic_rendering`
  (older/low-end Android, Wear OS smartwatches), the classic `VkRenderPass`/
  `VkFramebuffer` fallback this implies would be reached only through `SPUDGPU_LEGACY_*`
  (see below) — `spudgpu_cmd_begin_rendering`/`spudgpu_rendering_begin_desc` keep an
  identical calling convention on both paths, but the fallback itself would be exposed as a
  small opaque `spudgpu_framebuffer` handle (`SPUDGPU_LEGACY_FRAMEBUFFER`-gated,
  paired with `spudgpu_cmd_begin_rendering_legacy`) rather than hidden entirely
  inside the Vulkan backend. That object needs a real owner for its lifetime
  (creation, caching, invalidation on resize) — a private SpudLib-side cache keyed on
  attachment sets would be exactly the hidden global state/hidden decision the
  zero-policy rule above forbids. The owner is the caller: it holds raw image views
  on modern hardware or a `spudgpu_framebuffer` it created and caches on legacy
  hardware, and makes the same begin/end call either way.
- **`SPUDGPU_EXT_*` gates a capability gap between backends; `SPUDGPU_LEGACY_*`
  (designed, not built) gates one extra caller-owned object for constrained hardware
  within a backend.**
  Both are compile-time macros in `spudgpu.h`, computed once and gated on everywhere
  else — never scatter a raw `#if !SPUDGPU_COMPILE_<BACKEND>` check across call
  sites; if a second backend later lacks the same thing, that should be a one-line
  edit to the macro's own definition, not an audit of every use site. They answer
  different questions and must not be conflated:
  - `SPUDGPU_EXT_<NAME>` gates a capability that some `GRAPHICS_BACKEND` choices
    don't implement at all (bindless/descriptor indexing is the reference example —
    Metal needs a `MTLHeap`-backed allocator it doesn't have yet). Define it `1` only
    when a backend that implements it is the one compiled in
    (`SPUDGPU_COMPILE_VULKAN_API || SPUDGPU_COMPILE_D3D12_API`-style), `0` otherwise,
    and wrap the entire public function/type group in `#if SPUDGPU_EXT_<NAME>` so
    calling it against a build that lacks it is a compile/link error, not a silent
    no-op or a runtime NULL surprise discovered on the wrong platform. Pair every
    `SPUDGPU_EXT_<NAME>` with a `SPUDRESULT_GPU_EXT_<NAME>_NOT_SUPPORTED` in
    `spudcore.h`, returned when the backend compiles the extension in but the
    specific device/driver still doesn't support it — a distinct, later-discovered
    case from the macro itself being `0`. Two exceptions:
    `SPUDRESULT_GPU_EXT_DEPTH_BOUNDS_TEST_NOT_SUPPORTED` exists and is returned
    nowhere, since a pipeline's `depth_bounds_test_enable` is masked on a device
    without the feature (the pipeline behaves as if it is false, as `spudgpu.h`
    states); and `SPUDGPU_EXT_BUNDLES` has no paired result, since every GPU of
    a backend that implements bundles records them.
    `SPUDGPU_EXT_DESCRIPTOR_SETS`/`SPUDGPU_EXT_SUBPASS_MERGING` (see the OpenGL note
    below) are the next likely candidates whenever their real API surface gets
    built. An `EXT` has to stay a narrow, self-contained island (a handful of
    types/functions) — if gating a capability would mean wrapping most of
    `spudgpu_cmd_*` or another load-bearing chunk of the header, that's a sign the
    backend needs its own `GRAPHICS_BACKEND` entry instead of an `EXT` flag draped
    over nearly everything.
  - **An `EXT` is named for its capability, never for a platform or an API**
    (card S15, decided 2026-10-31 with the user): `SPUDGPU_EXT_BUNDLES`, not
    `D3D12_BUNDLES`, even where one backend is the only one that has it
    today. Another backend that gains the capability turns the macro to 1,
    with no rename and no caller change. So S12's Apple ray tracing sibling
    is `SPUDGPU_EXT_RAY_FUNCTION_TABLES`, and tile shading and framebuffer
    fetch, if built (card S16), `SPUDGPU_EXT_TILE_SHADING` and
    `SPUDGPU_EXT_FRAMEBUFFER_FETCH`.
  - `SPUDGPU_EXT_MESH_SHADING` (`SpudGPUMeshShaders` sample) is a second, distinct
    flavor of the same pattern: the macro itself is `1` on **every** backend — mesh
    shading has no structural per-backend gap the way bindless does — so the compile
    flag alone doesn't do the interesting work here. What still needs the full
    `EXT` treatment is *runtime* hardware/driver support, which genuinely varies even
    though the backend code exists on all three: Vulkan probes
    `VK_EXT_mesh_shader`'s presence and `VkPhysicalDeviceMeshShaderFeaturesEXT`
    when the instance lists its adapters (`spudgpuvulkan___read_adapter_facts`,
    `spudgpuvulkancontext.c`, the first optional device extension this backend
    has ever had to enumerate rather than assume) and enables it at device
    creation only for a device created with the feature, D3D12
    checks `D3D12_FEATURE_DATA_D3D12_OPTIONS7::MeshShaderTier`
    (`spudgpud3d12context.cpp`), and only Metal's is unconditionally true (Metal 3
    mesh shading is guaranteed on every Apple Silicon target this backend already
    requires). `SPUDGPU_FEATURE_MESH_SHADING` in an adapter's
    `supported_features`, and in the device's features when the device was
    created with it, surface that per-GPU fact to the caller, and
    `SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED` refuses a mesh pipeline on a
    device created without it — don't assume `SPUDGPU_EXT_MESH_SHADING == 1`
    means a given device can actually draw with it.
  - `SPUDGPU_EXT_RAY_QUERY` (card S12, built 2026-10-31 from the approved
    `spudgpu_raytracing_draft.h`, R1-R8) is the mesh shading flavour again:
    1 on all three backends, with `SPUDGPU_FEATURE_RAY_QUERY` per adapter
    (Vulkan `VK_KHR_acceleration_structure`, `VK_KHR_ray_query` and
    `VK_KHR_deferred_host_operations` listed, `accelerationStructure` and
    `rayQuery` on, all enabled only for a device created with the bit;
    D3D12 `OPTIONS5::RaytracingTier` 1.1 or later; Metal
    `supportsRaytracing` and `supportsRaytracingFromRender` on Apple7 or
    later, macOS 14 / iOS 17 or later) and
    `SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED` from every call of the
    section on a device created without it. Acceleration structures are a
    handle (`spudgpu_acceleration_structure`, committed or placed in a heap
    of `SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES`), sized by
    `spudgpu_get_acceleration_structure_build_sizes()`, built and updated
    many to a call (`spudgpu_cmd_build_acceleration_structures()`, nothing
    recorded on a refusal), copied, compacted (the size written into the
    caller's buffer) and ordered by a barrier call of their own. SpudGPU
    keeps no scratch, pool or cache and sets no build flag. Instances are in
    the device's own layout (card S14): a query
    (`spudgpu_get_acceleration_structure_instance_layout()`: 64 bytes row
    major on Vulkan and D3D12, Metal's 72-byte indirect descriptor) and a
    stateless CPU write of many at once; a GPU writer is the caller's, one
    per layout kind. Metal makes resident the bottom levels a top level's
    last build named (R5) wherever it is built or its set bound. What every
    backend checks the same way is in `src/gpu/spudgpushared.c`; each
    backend's translation is its own `*raytracing` file. Ray tracing
    *pipelines* (`SPUDGPU_EXT_RAY_TRACING_PIPELINE`,
    `SPUDGPU_EXT_RAY_FUNCTION_TABLES`) are not built (card S12). Settled
    while building (cards S23-S26, 2026-10-31): the heap class and the
    descriptor type are refused with the extension's result on a device
    without the feature; `placement_alignment` is 256 on Vulkan and 64 KiB
    on D3D12, and on Metal Metal's own (1024 on an Apple M5 Pro), and a
    placed structure the API would make larger than that rounding is
    refused; a SCRATCH to SCRATCH barrier orders two builds sharing a
    scratch range (D3D12 a UAV barrier between any two states that are both
    its `UNORDERED_ACCESS`); a build, copy or size write is refused on a
    copy list or bundle (`SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE`,
    368) and inside a rendering pass (`..._INSIDE_RENDERING`, 369), and
    leaves the compute bindings undefined until bound again. An update is
    taken only of exactly its source's last layout, kept on the handle as
    values (an array grown at a bottom level's first update-allowing build
    or copy), and a build call's checks make all the room it needs, so its
    recording can't fail.
  - Since 2026-10-21 (`SPUDGPU_DEVICE_TODO.md`): an `EXT` macro says what the
    backend in this build implements and nothing about any GPU, and every
    one's comment says so. Where support also varies by GPU it pairs with a
    `SPUDGPU_FEATURE_<NAME>` bit, and its `SPUDRESULT_GPU_EXT_<NAME>_NOT_SUPPORTED`
    means "the device wasn't created with the feature" (no depth bounds result
    is returned, and bundles have no bit or result; see above). The bits are always
    defined, never inside an `#if`; a bit whose macro is 0 is never in any
    adapter's supported features. Bindless is `SPUDGPU_EXT_BINDLESS` /
    `SPUDGPU_FEATURE_BINDLESS` / `SPUDRESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED`
    (it was `..._BINDLESS_DESCRIPTOR_INDEXING`).
  - `SPUDGPU_LEGACY_<NAME>` gates a fallback for hardware that some devices *within*
    a single backend's target range lack, even though every *currently* targeted
    device of that backend has it (dynamic rendering on old/low-end Vulkan hardware
    is the motivating case). This is a runtime device fact, not a `GRAPHICS_BACKEND`
    choice — the same compiled Vulkan backend has to run on both old and new
    hardware — so `SPUDGPU_LEGACY_<NAME>` must gate an opt-in build configuration for
    targeting that constrained hardware profile (e.g. a dedicated CMake option),
    never `GRAPHICS_BACKEND` itself. Which path a given device actually needs is
    surfaced to the caller as a feature bit of the adapter (as
    `SPUDGPU_FEATURE_BINDLESS` is), never resolved silently inside
    SpudLib — the caller decides which path to take per-device; SpudLib only reports
    the fact and does the mechanical object construction for whichever path is
    chosen. `SPUDGPU_LEGACY_<NAME>` may grow the public surface by exactly one
    narrow, opaque resource type when the fallback needs a persistent object with
    real lifetime (`SPUDGPU_LEGACY_FRAMEBUFFER` above) — that's still fundamentally
    different from `EXT`: the caller isn't adapting its behavior, it's just holding
    the handle SpudLib needs to do the identical job on older hardware.
- **OpenGL/OpenGL ES, if ever needed, is a fourth `GRAPHICS_BACKEND`
  (`SPUDGPU_COMPILE_OPENGL_API`), not reached through `SPUDGPU_LEGACY_*`.** Its gap
  from Vulkan/D3D12/Metal isn't one narrow fallback object — no true deferred
  command-list recording, no descriptor-set object, different synchronization
  primitives — which is a structural, broad difference, not a hardware-SKU fact
  within an existing backend. Express it the same way Metal's missing bindless is
  expressed today: a pile of `SPUDGPU_EXT_<NAME>` macros going to `0` for it
  (`SPUDGPU_EXT_DESCRIPTOR_SETS` is the clearest candidate), computed alongside
  Vulkan/D3D12/Metal as a genuine fourth backend choice — never squeezed under
  `SPUDGPU_LEGACY_*`'s opt-in constrained-hardware path, which exists for one small
  caller-owned object, not a structurally different API.
- **Backend file split mirrors the header, not guessed.** Each backend (Vulkan, D3D12,
  Metal) uses the same twelve-file-per-concern layout: `context`, `buffer`, `heap`,
  `image`, `shader`, `swapchain`, `descriptors`, `command`, `renderpass`, `native`,
  `sync`, `query` (timestamp sets, added 2026-10-15). If `spudgpu.h` grows a new
  concern, split a new file for it in every backend rather than dumping new
  functions into an existing one. What every backend checks the same way is
  written once in `src/gpu/spudgpushared.c` (`spudgpushared.h`), as SpudNet and
  SpudInput keep their front ends: the pipeline's colour-attachment and depth-bias
  checks are there.
- **A fence is a counter that only goes up, on every backend.** A submission
  signals it to a value the caller chooses
  (`spudgpu_submit_desc::signal_fence_value`), `spudgpu_wait_for_fences` waits
  for a value per fence, and there is no reset: a fence is reused by
  signaling it higher. That is `ID3D12Fence` and `MTLSharedEvent` as they
  are, and a timeline `VkSemaphore` on Vulkan - not a `VkFence`, which is one
  bit that has to be reset by hand and would make the same call behave
  differently per backend. SpudGPU keeps no counter of its own for a caller's
  fence; don't add one. The swap chain's own per-frame fences are internal to
  each backend (plain `VkFence`s on Vulkan, since acquire can't take a
  timeline semaphore) and are not `spudgpu_fence`s: a submission reaches them
  through `spudgpu_submit_desc::swap_chain`, which is also all that
  `spudgpu_submit_command_lists_synced` is. Written 2026-10-10, compiled
  for Metal only. `spudgpu_semaphore` is still binary on Vulkan and a hidden
  counter on D3D12 and Metal.
- **One list of an allocator is open at a time, on every backend.** D3D12
  refuses a second (`COMMAND_ALLOCATOR_CONTENTION`, and the list stays
  closed); Vulkan and Metal would allow it, so `spudgpu.h` states D3D12's
  rule for all three. A debug build keeps the open list in the allocator's
  `_open_list` and asserts in begin (bundle begin too) and in
  `spudgpu_reset_command_allocator`; end and destroy clear it. Vulkan's list
  holds its allocator by value, so it keeps `_debug_allocator` for this.
  Added 2026-10-16; run on D3D12 and on Vulkan on Windows, and on Metal
  (`aprend_gpu_tests`, Debug), never fired.
- **A buffer's memory is one `SPUDGPU_MEMORY_KIND`, a contract and not a
  set of Vulkan property bits.** `DEVICE`, `UPLOAD`, `READBACK` and
  `DEVICE_MAPPABLE` each say what the CPU and the GPU may do with the buffer
  and which usages it may have (the table is on the enum in `spudgpu.h`);
  each backend picks whatever memory keeps that contract, and the checks
  that enforce it are the same in all three. `SPUDGPU_MEMORY_KIND_NONE` is
  refused: a zeroed desc has not chosen. Don't add a kind, or a flag beside
  the kind, that only one API can tell apart (coherent, cached, write
  combined): the caller always calls `spudgpu_flush_buffer` after a mapped
  write and `spudgpu_invalidate_buffer` before a mapped read, and a backend
  that has nothing to do for one does the checks and returns. An image has
  no kind at all: it is in the device's memory and never mapped.
  `DEVICE_MAPPABLE` is the one kind a device may lack: it needs a device
  created with `SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY` and is refused with
  `SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED` otherwise; it has no `SPUDGPU_EXT_*`
  macro because every backend implements it. Written 2026-10-10, compiled
  for Metal only; D3D12's custom heap for it has never run.
- **A buffer or image desc carries no flag only one API acts on.**
  `SPUDGPU_HEAP_FLAGS` (D3D12's heap flags under another name) and
  `SPUDGPU_RESOURCE_FLAGS` (one D3D12 resource flag) were removed on
  2026-10-10: Vulkan read neither, Metal ignored both, and no caller set a
  bit. Don't bring a bit back onto the desc. Sharing a resource across
  processes or APIs, if it is ever needed, is an `SPUDGPU_EXT_*` group with
  its export and import calls, which all three APIs can back. Zeroing is
  settled by rule instead of by flag: a new buffer's or image's contents are
  undefined until written, on every backend, so the D3D12 backend always
  passes `D3D12_HEAP_FLAG_CREATE_NOT_ZEROED`.
- **A heap is one device allocation; SpudGPU places, the caller decides where.**
  `spudgpu_create_heap` makes a heap of one `SPUDGPU_MEMORY_KIND` and one
  `SPUDGPU_HEAP_RESOURCE_CLASS` (buffers, images, attachment images);
  `spudgpu_get_buffer/image_placement_requirements` ask the heap for a
  resource's size and alignment; `spudgpu_create_placed_buffer/image` make an
  ordinary handle at the caller's offset, destroyed with the ordinary destroy
  call. SpudGPU keeps no record of what is placed where and checks no
  overlap - which heap, which offset, when to make or drop a heap are the
  caller's. Settled on 2026-10-11, don't reopen without a reason:
  - One class per heap on every backend, D3D12 tier 2 included (D3D12 tier 1
    made the rule; Vulkan never has buffers and images in one allocation).
  - The requirement queries take the heap, not the device: Vulkan folds
    `nonCoherentAtomSize` into a mappable heap's answer there, so a flush of
    one placed buffer never reaches the next.
  - Vulkan chooses a heap's memory type when it is made, from probe
    resources (`spudgpuvulkanheap.c` explains which), and maps a mappable
    heap once for its life: Vulkan allows one mapping of a `VkDeviceMemory`.
  - Metal heaps are `MTLHeapTypePlacement` and hazard **tracked**, because
    this backend's barriers do nothing and rely on tracking. Metal may then
    order work on unrelated resources of one heap. Untracked heaps wait for
    real Metal barriers.
  - An image in an attachment-images heap is first written whole (a pass
    that clears it, depth and stencil both, or copies into all of it):
    D3D12's rule for placed render targets, without a discard command.
  - A debug build counts live placements per heap (atomically) and
    `spudgpu_destroy_heap` asserts the count is 0.
  - D3D12 places every texture at 64 KiB alignment. Asking for 4 KiB is a
    debug layer error for a texture too big for it, and telling them apart
    needs each format's tile shape; not done.
  Written 2026-10-11. Vulkan and D3D12 type-checked on Linux against the
  Khronos and DirectX headers, and the Vulkan heap, buffer and image files
  run against a mock Vulkan; the Metal files parsed with clang against a
  stub Metal header only. Nothing has run on a GPU.
- **Native escape hatches are interop-only.** `spudgpu_vulkan_natives.h` /
  `_d3d12_natives.h` / `_metal_natives.h` unwrap a handle back to its raw native type
  (`VkInstance`, `ID3D12Device14`, ...) for third-party libraries SpudLib doesn't wrap
  itself (ImGui, RenderDoc). SpudLib has no visibility into what the caller does with
  the handle afterward — that's the point, don't add tracking/validation around it.
- **Metal's natives header is not a 1:1 mirror of Vulkan/D3D12** — these asymmetries
  are deliberate, worked out before any Metal implementation exists, don't "fix" them
  later thinking they're gaps:
  - No instance-equivalent accessor (`MTLCopyAllDevices()` replaces it)
  - No physical/logical device split (`id<MTLDevice>` covers both)
  - No queue-family-index accessor (Metal queues aren't partitioned into families)
  - No command-allocator accessor (command buffers come fresh from the queue each time)
  - Buffer/image views aren't distinct native types (raw offset/stride binding; a
    texture view is just another `id<MTLTexture>`)
  - No root-signature accessor (binding indices come from `[[buffer(n)]]`/
    `[[texture(n)]]` shader attributes)
  - Fences map to `id<MTLSharedEvent>` (needs CPU wait/signal/read plus
    cross-process sharing); semaphores map to plain `id<MTLEvent>` (GPU-only
    wait/signal across queues, which is all `spudgpu_semaphore`'s public API
    ever asks for). Neither uses `MTLFence` — it can't be waited on from the
    CPU or across command queues, and belongs in `spudgpu_cmd_pipeline_barrier`'s
    implementation instead, not in fence/semaphore.

## Code conventions

- **The version is written once, in `spudcore.h`** (`SPUDLIB_VERSION_MAJOR`/
  `_MINOR`/`_PATCH`, since 2026-10-20, at 0.1.0). `CMakeLists.txt` reads
  those three lines for `project(VERSION)`, and `spud_get_version()` returns
  what the library was built with. One version for the whole library;
  what each part promises is the table in the header's "Version" group.
  Nothing is promised before 1.0.0, and "released" means a `vX.Y.Z` tag.
  A chained struct's comment says "Since X.Y.Z". spudlib refuses no caller
  for its version: comparing is the caller's.

- **Every struct that may grow starts with a `base`** (`SPUD_STRUCT_BASE`,
  or `SPUD_STRUCT_BASE_OUT` for one SpudLib fills in; `spudcore.h`). Every
  other public struct has a `/* Frozen: ... */` line above its `typedef`.
  A released layout never changes: a new field is a new struct chained on.
  A call checks a based argument with `spud_struct_check` /
  `spud_struct_out_check` (`src/spudstructbase.h`) right after its null
  check, in every backend. SpudGPU, SpudAudio, SpudNet and SpudInput are converted; the rest
  of the plan, and the rules, are in `STRUCT_BASE_TODO.md`. The workspace's
  `check.sh struct-bases` checks the headers' side: based or frozen, base
  first, no `#if` in a struct, no based struct held by value, and each type
  registered in `SPUD_STRUCT_TYPE` as the right kind and used in `src/`.

- **Argument checks are one `if` per distinct return.** They sit at the top of
  the function, in parameter order, before any allocation or platform call, and
  each returns the `SPUDRESULT` that names exactly what was wrong:

  ```c
  if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
  if (!desc) return SPUDRESULT_NULL_DESC;
  ```

  Conditions that would return different values are never joined. A joined check
  can only return one code, so the caller can't tell which argument failed, and
  SpudLib's job is to report the fact, not a summary of it.
- **Conditions may be joined with `||` when every one of them leads to the same
  return** - the same `SPUDRESULT`, or the same `bool`, null or plain `return`
  from a `void` function:

  ```c
  // device null: stops at !device, device->_bindless is never read.
  if (!device || !device->_bindless) return;
  // path null: stops at !path, path[0] is never read.
  if (!path || !path[0]) return false;
  ```

  `||` evaluates left to right and stops at the first true condition, in every C
  and C++ standard, so a later condition may rely on an earlier one: put the
  pointer check before the check that dereferences it.
- **Conditions may be joined with `&&` when the branch needs all of them to
  hold.** `&&` evaluates left to right as well and stops at the first false
  condition, so each condition guards the ones after it:

  ```cpp
  // result null: stops at result, GetErrorBuffer is never called.
  // GetErrorBuffer fails: stops there, errors is never read.
  // errors null: stops at errors, GetBufferSize is never called.
  if (result && SUCCEEDED(result->GetErrorBuffer(&errors)) && errors && errors->GetBufferSize())
  	printf("spudgpu: DXC compile failed (%ls): %s\n", profile,
  	    (const char *)errors->GetBufferPointer());
  else
  	printf("spudgpu: DXC compile failed (%ls): hr=0x%08lx\n", profile, (unsigned long)hr);
  ```

  Any condition being false takes the same `else`, which is what makes joining
  them correct. If two of them need different handling, they are separate `if`s.
- The shared return must be the right one for each condition on its own. Don't
  choose a vaguer code so that two checks can share a line; if one of them later
  earns a more specific code, split the line then.
- Some existing code joins checks that should return different codes. Split one
  only when you are already changing that function.
- **A function returns the same value for the same condition in every backend.**
  The header declares it once and the caller cannot see which backend is linked,
  so a null argument, a zero count or an out-of-range index gets the same result
  from all of them. That means the same checks, returning the same codes, in the
  same order - order matters, because it decides which code comes back when two
  arguments are wrong at once.

  ```c
  // spudgpu_submit_command_lists_synced, identical in Vulkan, D3D12 and Metal.
  if (!queue) return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
  if (!cmd_lists) return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
  if (cmd_list_count == 0) return SPUDRESULT_ZERO_SIZE;
  if (!swap_chain) return SPUDRESULT_GPU_INVALID_SWAP_CHAIN;
  ```

  This covers every kind of return, not only `SPUDRESULT`: where one backend
  returns `false`, null or 0 for a condition, the others do too, and a `void`
  function that returns early in one returns early in all.
- A check is changed in every backend in the same change. If the backends
  already disagree, decide which answer is correct and move all of them to it;
  the reference backend is the one to build against first, not the one that wins
  a disagreement.
- **Where the graphics APIs behave differently underneath, the header states
  one rule that holds on all of them, and every backend is held to it.** The
  rule is the narrowest contract every API can keep: what one API preserves
  and another doesn't is declared undefined for the caller, not exposed as a
  per-backend difference and not papered over with emulation. It is written
  in the function's comment as a rule the caller follows, not as a note on
  what each API happens to do.

  `spudgpu_cmd_push_constants_compute` is the example. Vulkan keeps one push
  constant state for a command list, D3D12 keeps one for graphics and one for
  compute, and Metal has none and resends the block to an encoder. The header
  says none of that. It says:

  ```c
  /* Two rules hold on every backend, whatever the graphics API underneath
   * keeps or shares:
   *
   * - Push after spudgpu_cmd_bind_compute_pipeline() and before the
   *   spudgpu_cmd_dispatch() that reads the data. What a dispatch reads from a
   *   byte that has not been pushed since the compute pipeline was last bound
   *   is undefined.
   * - Graphics and compute pipelines do not have a push constant block each:
   *   after this call, what a draw reads from the block is undefined until it
   *   is pushed again with spudgpu_cmd_push_constants(), and the other way
   *   round.
   */
  ```

  This is the preferred way to handle such a case. A caller that follows the
  rule gets the same result on every backend; one that leans on what a single
  API preserves is outside the contract. Reach for an `SPUDGPU_EXT_*` macro or
  a capability query only when a backend can't do the thing at all, not when
  the APIs merely keep different state. The fence model (a counter on every
  backend) is the same idea applied to an object.
- What may differ is what only one platform can produce: a failed platform call
  (`SPUDRESULT_API_SPECIFIC_FAILURE`), or a feature a backend doesn't have,
  which is reported through the result code or capability query the header
  documents for it.
- **Check the handle, not what is inside it.** A create call either returns a
  fully formed object or fails and returns none: every native object the handle
  needs is created, and its result checked, before the handle is handed out. A
  function that takes a handle checks the handle argument and then uses its
  members directly.

  ```cpp
  if (!cmd) return;
  cmd->_d3d_cmd_list->SetGraphicsRoot32BitConstants(...);
  ```

  Don't add `if (!cmd->_d3d_cmd_list)` at the point of use. It can only be null
  if the create call let a half-built object out, and a check there hides that
  bug instead of reporting it. Fix the create call.
- Where it helps to state the invariant, assert it:
  `assert(cmd->_d3d_cmd_list);`. An assert documents what create guarantees and
  compiles out of release builds; it is not a substitute for checking the handle
  argument.
- The exception is a member that is created lazily, after the handle exists:
  what is keyed by a value the caller passes at a call, or belongs to an object
  the caller synchronizes (in the D3D12 backend: a command list's `_rtv_heap`
  and `_dsv_heap`, and the device's indirect command signatures of strides
  other than the tightly packed ones). Null is a valid state for those, so the
  function that uses one checks it and creates it there. Everything else a
  device needs (its queues, its bindless table, D3D12's shader-visible heaps
  and tightly packed signatures) is made by `spudgpu_create_device`.
- **All of SpudLib's own memory comes from the caller's allocator** (`SPUD_ALLOCATOR`,
  `spudcore.h`, since 2026-10-21). A module's instance desc takes it, the
  instance keeps a copy, and every object made from the instance keeps a copy of
  its parent's in its head, so it frees through its own copy and no destroy call
  takes an allocator. Never `malloc`, `calloc`, `realloc`, `free` or `strdup`
  SpudLib's own memory, and no allocating `new` or `delete` in the C++ backends.
  The helpers are in `src/spudallocator.h`: `SPUD_NEW`, `SPUD_NEW_ZEROED`,
  `SPUD_DELETE`, `SPUD_NEW_ARRAY`, `SPUD_NEW_ARRAY_ZEROED`, `SPUD_DELETE_ARRAY`,
  `SPUD_RESIZE_ARRAY`, `spud_string_copy`/`spud_string_free`, and
  `spud_allocate`/`spud_free` for an untyped blob. An array that grows is an
  `EQ_ARRAY` from eqstd (below), on the object's `SPUD_ALLOCATOR` handed
  over by `spud_eq_allocator`. The workspace's
  `check.sh conventions` counts every call of a C allocation function and
  every allocating `new` and `delete` in `src/`; the ones allowed (the
  system allocator a caller may choose, memory the platform hands out,
  `d3dx12.h`) are in `check.baseline` with reasons.
- **A block is freed with the size and alignment it was allocated with.** The
  allocator is told both, so that a pool or an arena needs no header. Keep a
  capacity beside every array whose length isn't otherwise known when it is
  freed, and free it with that capacity. Two arrays that share one capacity
  are grown both or neither.
- **An array that grows is an `EQ_ARRAY` (eqstd), not a pointer, a count and
  a capacity grown by hand** (since 2026-10-29). eqstd is the workspace's
  header-only container library (`../eqstd`, belonging to no layer, never in
  a public API), a PRIVATE dependency of `spudlib`: no public header includes
  it. The object that owns the array keeps an `EQ_ALLOCATOR` made by
  `spud_eq_allocator(&head.allocator)` (`src/spudallocator.h`) beside it, since
  the array keeps a pointer to it; every eqstd call that grows returns an
  `EQ_RESULT`, and anything but `EQ_SUCCESS` becomes `SPUDRESULT_OUT_OF_MEMORY`
  (or the caller's false), the array unchanged. eqstd keeps
  `SPUD_ALLOCATOR`'s contract (never 0 bytes, each block freed with its size
  and alignment; an array's alignment is `EQ_ARRAY_ALIGNMENT` of the
  element's size, a multiple of its alignment). Where a growth pattern
  matters (a table sized from the capacity, a count that must fit a
  `uint32_t`), the capacity is chosen here and given to `eq_array_reserve`,
  which allocates exactly that; otherwise `eq_array_push` grows by doubling
  from 4. Converted: SpudNet's response headers (`src/net/spudnethttp.c`)
  and the wait set's members and poll entries (`src/net/spudnetwaitset.c`).
  Converted 2026-10-29 and run on Windows: WASAPI's endpoints, GameInput's
  devices, and the D3D12 backend's indirect signatures and heap free ranges
  (the device keeps their `EQ_ALLOCATOR`); the D3D12 backend's
  `std::vector`s too, as `eq::array` on the device's allocator (each image's
  subresource states, the bindless free stacks, the barrier lists, the
  adapter list), so none of them allocates outside the caller's allocator or
  throws `std::bad_alloc` into a C caller any more. SPIRV-Cross's own
  `std::vector` and `std::string` stay: its API takes and returns them.
  Still `SPUD_RESIZE_ARRAY`, to convert when it can be compiled and tested
  again: evdev's gamepads. A size of 0 is asked for and freed as 1 by the helpers, so a
  count that may be 0 needs no case of its own. `tests/spudtest_allocator.h`
  checks all of this in a test.
- **A destroyed handle is refused in Debug** (card X16, 2026-10-30;
  `src/spudstale.h`). A Debug build's `spud_object_head` holds a tag:
  `SPUD_BIRTH` sets it where a create fills the head, `SPUD_KILL` writes
  it dead at every destroy and owner release, before the allocator is
  copied out, and every public call checks each handle right after its
  null check, its own `if`, refusing as the null check does
  (`if (SPUD_STALE(x, "call", "x")) return ...;`, one line printed), the
  same checks in the same order in every backend. Release has no tag.
  Best effort: reading a destroyed handle can fault where the caller's
  allocator has given its page back. A new handle type gets a factory in
  `tests/misuse/make_misuse_tests.py`.
- **Every handle starts with a `spud_object_head`** (`src/spudallocator.h`): its
  debug name, then its copy of the allocator. A handle is freed by copying the
  allocator out of its head first, since the head is inside the memory being
  freed:

  ```c
  	const SPUD_ALLOCATOR allocator = buffer->head.allocator;
  	spud_string_free(&allocator, buffer->head.debug_name);
  	SPUD_DELETE(&allocator, spudgpu_buffer_vulkan, buffer);
  ```
- **Memory the platform allocates stays the platform's.** What `realpath`,
  CoreFoundation, COM, libcurl, ALSA's `snd_*_malloc` or `getaddrinfo` give is
  freed the way the platform says; Objective-C and COM reference counts are left
  alone. So is memory a graphics driver allocates: SpudGPU passes Vulkan no
  `VkAllocationCallbacks`, and D3D12 and Metal take none.
- **A C++ type with a constructor:** `SPUD_NEW`, check the pointer, then
  construct the object in place in that memory. `SPUD_NEW`, not
  `SPUD_NEW_ZEROED`, because the constructor is what initialises it. The
  in-place form needs `<new>`.

  ```cpp
  spudgpu_buffer_d3d12 *object = SPUD_NEW(&device->head.allocator, spudgpu_buffer_d3d12);
  if (!object) return SPUDRESULT_OUT_OF_MEMORY;
  object = new (object) spudgpu_buffer_d3d12();
  ```

  It is destroyed the same two steps in reverse: the destructor called explicitly,
  then `SPUD_DELETE`, with the allocator copied out first.
- **A type with no constructor:** `SPUD_NEW_ZEROED`, check the pointer, and that
  is all. The zeroed memory is its initial state, and nothing is constructed. It
  is destroyed with `SPUD_DELETE` alone.
- "Has a constructor" includes a type that only has members which do: a struct
  holding a `std::vector` or a `ComPtr` must be constructed in place and have its
  destructor called, or those members are never set up or released. In the D3D12
  backend that is most handle structs (instance, device, queue, allocator, command
  list, buffer, image, shader module, both pipelines, fence, semaphore, descriptor
  pool, swap chain). The plain ones are the buffer view, image view, sampler,
  descriptor set layout, descriptor set and surface.
- Every allocation has exactly one matching release, on every failure path as well
  as in the destroy call.
- A failed allocation is then a null pointer the function turns into a
  `SPUDRESULT_OUT_OF_MEMORY`, where an allocating `new` would throw, and no
  exception may cross SpudLib's C boundary.
- Objects with no destroy call of their own are released by their owner: the
  adapters by `spudgpu_destroy_instance`; a device's command queues and its
  bindless state by `spudgpu_destroy_device`;
  descriptor sets by their pool, on reset and on destroy; a swap chain's back
  buffer images and views by the swap chain.
- What is left: `std::vector` and `std::string` (function locals in
  `spudgpud3d12context.cpp`, `spudgpud3d12command.cpp`, `spudgpud3d12renderpass.cpp`
  and `spudgpud3d12shader.cpp`, the bindless free stacks, and an image's
  `_subresource_states` in `spudgpud3d12.hpp`) still allocate
  through the standard allocator, which reports failure by throwing. They
  become `eq::array` / `eq::string` on the device's allocator, and the
  `SPUD_RESIZE_ARRAY` arrays listed above `EQ_ARRAY`s, when those files are
  next built on their platforms (Windows, Linux); then `SPUD_RESIZE_ARRAY`
  goes.
- **A debug name is copied, never kept as the caller's pointer, in every
  build.** `debug_name` fields and the name member of a handle exist in
  Debug and Release alike (since 2026-10-18; before, only under
  `#if _DEBUG`, which made a desc's layout differ between the two). A
  build switch never changes a public struct's layout or which functions
  exist. Every handle that carries a name has it in its head
  (`head.debug_name`), and that member is either null or a copy SpudLib made
  through the handle's allocator. The caller may free or reuse its own string
  as soon as the call that took it returns. Storing the pointer
  (`pResult->head.debug_name = desc->debug_name;`) ties the object to memory
  SpudLib does not own.
- **`spud_debug_name_set` (`spudcore.c`) is the one place a name is copied.**
  It frees the name the object had, stores a copy of the new one made through
  the head's allocator, and returns `SPUDRESULT_OUT_OF_MEMORY` if the copy
  fails. A create call whose desc has a `debug_name` calls it, after the head's
  allocator is set; so does a caller renaming an object later.

  ```c
  	if (spud_debug_name_set(pResult, desc->debug_name) != SPUD_SUCCESS) {
  		spudgpu_destroy_buffer(pResult);
  		return SPUDRESULT_OUT_OF_MEMORY;
  	}
  	pResult->_desc.debug_name = pResult->head.debug_name;
  ```
- A null `debug_name` stays null: there is nothing to copy and it is not an
  error.
- The copy is the last step of the create call, after every native object
  exists, so a failed copy releases the finished object through its own destroy
  call and returns `SPUDRESULT_OUT_OF_MEMORY`.
- A handle that also keeps the caller's desc (`_desc = *desc`) points
  `_desc.debug_name` at the handle's copy. The struct copy alone still carries
  the caller's pointer, and a `spudgpu_get_*_desc` call would hand it back.
- **Because the setter frees the old name, a handle's name must be null from
  the moment the handle exists.** Allocate with `SPUD_NEW_ZEROED`, or copy from
  a zeroed struct, or value-initialise (`new (p) T()`); a handle from a bare
  `SPUD_NEW` sets it to null itself.
- **Whatever frees a handle frees its name first:** the destroy call, or the
  owner for a handle with no destroy call of its own (a device and its queues,
  descriptor sets, a swap chain's images, views, semaphores and fences). In C++
  the name is freed before the destructor is called.
- A name that is only passed to a platform call during the create call, and not
  stored, needs no copy.

## Backend status

All three backends have run a caller's full renderer on real hardware (as of
2026-10-12): D3D12 on Windows, Vulkan on Linux, Metal on macOS, the same caller
code on each. On 2026-10-25 Vulkan on Linux (Fedora 44, Intel Iris Xe on Mesa
26.2.4, and llvmpipe) ran a caller's GPU test suite clean under validation and
synchronization validation, after a fix here: a buffer barrier out of
`COMMON` now waits on every earlier command and makes every write available
(D3D12 orders submissions and decays buffers to `COMMON`; Vulkan had waited on
nothing), and `VERTEX_BUFFER` includes uniform reads in shaders, as
spudgpu.h says it does. Every sample in `spudgpusamples` but mesh shaders
ran there too. That covers the calls that caller makes. A call nothing above
SpudLib uses is only as verified as its sample in `spudgpusamples`. Vulkan
on Windows has run `apricottests`' `aprend_gpu_tests` (2026-10-16, Intel Iris
Xe and NVIDIA RTX A2000, validation layer clean), which found that the
Vulkan backend passed a descriptor write's image layout and a descriptor set
layout binding's stage flags to Vulkan untranslated; both are SpudGPU enum
values and now go through `spud_layout_to_vk` and
`spudgpuvulkan___shader_stages`. Metal ran the same tests on macOS the
same day (Apple M5 Pro) under Metal API and shader validation with warnings
fatal, clean after five fixes in the Metal backend: samplers now made with
`supportArgumentBuffers`; a descriptor set bound only to the stages its
bindings name (it was bound to vertex and fragment always); a pipeline's
depth-stencil and rasterizer state set on the encoder only when they change
(Metal shares one depth-stencil state between equal descriptors, and
validation refuses setting a state again); the timestamp counter set
retained out of the autorelease pool that looked it up, which had freed
it; and the array of devices made once and owned by the instance on every
backend (Vulkan and Metal leaked a copy to the caller, and made new devices
on each call). On 2026-10-18 a test that binds one compute pipeline for two
dispatches in a row found that Metal set the pipeline state on the encoder
at every `spudgpu_cmd_bind_compute_pipeline`, which validation refuses as
redundant; it is set only when it differs from the encoder's now
(`_encoder_compute_pipeline_state`), as the render encoder's state already
was. The same day `aprend_gpu_tests` ran on Vulkan under synchronization
validation (`VK_LAYER_VALIDATE_SYNC=1`, both devices), which found 15
hazards per device in the Vulkan backend's image barriers: `layout_to_access`
gave colour and depth attachments write access only (a pass loading its
target read it unordered after the transition), and `layout_to_stage` gave
depth the early fragment tests only (depth is also written at the late
ones). Attachments are read and written now, depth at both stages, and
`spudgpu_cmd_pipeline_barrier`'s RENDER_TARGET and DEPTH_WRITE states read
and write too; rerun, no message on either device. Also the same day:
the D3D12 debug layer's messages reached only an attached debugger, so a
plain Debug run showed none. A Debug build now registers an
`ID3D12InfoQueue1` message callback per device
(`spudgpu_d3d12_debug_message`, `spudgpud3d12context.cpp`) that prints
corruptions, errors and warnings to stdout as `D3D12 <SEVERITY>: [ id ]
text`, as Vulkan's validation layer does with no messenger; nothing is
registered when the layer is off or the runtime predates
`ID3D12InfoQueue1`. Run without a debugger on both devices: silent on the
tests as they are, and a mismatched clear value put in for the test
printed its 6 warnings on each. On 2026-10-22 every `spudgpusamples` sample
ran on D3D12 (both devices and WARP, GPU-based validation too) and Vulkan
(both devices, under synchronization and GPU-assisted validation as well),
which found two Vulkan bugs: an image barrier out of `UNDEFINED` or
`PRESENT_SRC` (`SPUDGPU_RESOURCE_STATE_PRESENT`/`_COMMON`) started at
`TOP_OF_PIPE`/`BOTTOM_OF_PIPE`, not chained to the acquire semaphore's wait,
and now starts at its destination stage; and `drawIndirectFirstInstance` was
never enabled, so an indirect draw's non-zero `first_instance` was outside
Vulkan's rules. That is `SPUDGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE` now
(every D3D12 and Metal adapter; the Metal line was compiled on macOS on
2026-10-22, where spudlib's tests pass over it, plain and under ASan and
TSan, though no test there reads the bit).
Rerun, every sample clean in every mode. The same day a caller's fragment
shader that read only locations 1 and 2 of its vertex shader's outputs
failed to link on D3D12 (debug layer error 660), which matches the stages
by register in order, while Vulkan and Metal match by location and took it;
`spudgpu_create_shader_pipeline`'s comment now states the rule that holds
on all three (declare every output from location 0 up to the last one
read). On 2026-10-23 a caller's GPU tests ran on Windows 11, Debug, on D3D12 (debug layer printing) and Vulkan (validation layer, plain
and under synchronization validation), on the Intel Iris Xe and the NVIDIA
RTX A2000, two devices each: the first runs off Metal of combined image
samplers, a layered pass with the layer from the vertex shader (D3D12's
`VPAndRTArrayIndexFromAnyShaderFeedingRasterizer...` on both GPUs), a mesh
shader drawn (RTX A2000; `quad.mesh`, SPIR-V 1.5, through SPIRV-Cross to
HLSL on D3D12), a stencil aspect sampled and a cube array view sampled.
Every check passed; D3D12 was silent, and synchronization validation
found no hazard. The Vulkan validation layer found two bugs in this
backend, both fixed and rerun clean: a mesh or task shader module was
made on a device without `SPUDGPU_FEATURE_MESH_SHADING` (its SPIR-V needs
`VK_EXT_mesh_shader` enabled even as a module), so
`spudgpu_create_shader_module` now refuses one with
`SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED`, on every backend, after
the alignment check; and a vertex shader's `gl_Layer` in SPIR-V before 1.5
(glslc's default) is the `ShaderViewportIndexLayerEXT` capability, which
needs `VK_EXT_shader_viewport_index_layer` and not only
`shaderOutputLayer`. A Vulkan adapter has `SPUDGPU_FEATURE_LAYERED_RENDERING`
now only with the extension and `shaderOutputViewportIndex` as well (the
extension, enabled beside `VkPhysicalDeviceVulkan12Features`, needs both
on: VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-02835), and a device
created with it enables all three; both GPUs here keep the bit. The
module check is compiled for Vulkan and D3D12 and no run has reached it
(the caller refuses a mesh shader first); Metal's line has not been
compiled. On 2026-10-24 a caller's exposure histogram, one compute set
bound for two dispatches in a row, found that Metal's backend set a
compute descriptor set's argument buffer again when it was already bound
at its index, which validation refuses as redundant (as it did the
compute pipeline state, 2026-10-18); the compute encoder keeps the
argument buffer at each index (`_encoder_compute_sets`, cleared with the
encoder) and sets it only when it differs. Run on Metal only; Vulkan and
D3D12 rebind as asked, which both allow.

On 2026-10-29 the Windows backlog ran on Windows 11 (ThinkPad P1 Gen 5:
Intel Iris Xe, NVIDIA RTX A2000, and WARP under D3D12; MSVC 14.51, Vulkan
SDK 1.4.363.0, which the build now finds through `VULKAN_SDK` rather than a
hard-coded 1.4.341.1). Both presets built clean, Debug and Release, as did
eqstd under MSVC (`/W4 /WX`, C11 and C++20/23). Every ctest passed on both
backends in both configurations, and under the new `windows-<backend>-asan`
presets (MSVC's AddressSanitizer, Debug); `spudnet_test` passed all 343
checks, Debug, Release and ASan. `spudgpu_device_test` and
`spudgpu_objects_test` ran on adapter 0 only (the Iris Xe; the tests take
the first adapter), debug layer and validation layer silent. A caller's GPU
tests ran on all three D3D12 adapters (debug layer, and GPU-based validation
once) and both Vulkan devices (validation, synchronization and GPU-assisted
validation): silent but GPU-based validation's `[942]` on the Iris Xe,
which `../claude_made_decisions.md` (2026-10-29) takes as the validator's
(a buffer barrier from COMMON after an ExecuteCommandLists, where D3D12
decays buffers to COMMON; clean on the A2000 and WARP). Every
`spudgpusamples` sample ran on the three D3D12 adapters (debug layer, and
GPU-based validation) and both Vulkan devices (all three validation modes),
clean; `SpudGPUMeshShaders` exits 1 on the Iris Xe, which has no mesh
shading. The run found one thing here: on Windows `spud_system_allocate`
handed a size no rounding can hold to `_aligned_malloc` and relied on the
CRT to refuse it, which AddressSanitizer turns into an abort; the refusal
is now SpudLib's, on every platform (`spudcore_test` under ASan is its
test). The D3D12 backend's arrays then moved to eqstd (below) and all of
the above ran again, as clean.

On 2026-10-30 the same machine ran 2026-10-30's work: both presets
built clean, Debug and Release; every ctest passed on both backends (14
since that day: `spud_misuse_tests`, every public function with zeroes and,
in Debug, with each handle destroyed, and `spud_noalloc_tests`, which holds
SpudGPU's submit and present loop, SpudAudio's callback, SpudNet's idle
connection and SpudInput's poll to no allocation of SpudLib's own), on both
adapters, Vulkan under validation; `spudnet_test` 343 of 343. Found and
fixed that day: D3D12 never defined `spudgpu_create_surface_from_callback`
or the two swap chain semaphore getters, so a caller naming one didn't link
there (they now refuse, as the header says); a recording call and a
submission allocated their converted arrays every time (now kept on the
command list or queue: one allocation a submit on D3D12, six on Vulkan,
before); Vulkan's swap chain desc reported the caller's image count, not
its own (card S13 asks whether it should honour the caller's at all). The
75 SpudGPU calls no test made are made by `spudgpu_objects_test` since that
day (a drawn grid read back, every draw kind, placed resources, compute, a
swap chain on a hidden window, the surface callback on Vulkan), on Windows
only; Metal, ALSA, CoreAudio, curl, POSIX, evdev, GameController and
URLSession files touched by X16 were written there, not compiled.

On 2026-10-30 the Mac (Apple M5 Pro, macOS 27) compiled and ran them:
`macos-metal` Debug and Release, `-asan` and `-tsan`, every ctest passing
in all four (14), `spudnet_test` 343 of 343 plain, Release and ASan,
`spud_misuse_tests` ten runs in Debug (206 calls with a destroyed handle,
no failure), and `spudgpu_objects_test` and `spud_noalloc_tests` under
Metal API and shader validation with warnings fatal. Found and fixed:
- `spudgpu_objects_test`'s mesh shader drew nothing on Metal: Metal's
  `setDepthTestMinBound:maxBound:` is encoder state with no pipeline
  switch, so the bounds set for the depth-bounded draw before it clipped
  the mesh pipeline, created without the test. `spudgpu.h` stated the
  difference per backend ("on Metal this call alone turns the test on");
  it now states one rule (set the range after binding a pipeline with
  `depth_bounds_test_enable`; a bind leaves it undefined; a pipeline
  without the test is never tested), and Metal keeps it: a bind of a
  pipeline without the test sets the encoder's bounds to [0, 1], and
  `spudgpu_cmd_set_depth_bounds` sets them only while a pipeline with it
  is bound (`_encoder_depth_bounds`). Vulkan and D3D12 kept it already.
- Metal's validation, with warnings fatal, refused the same vertex buffer
  set again between two draws, and a descriptor set's argument buffer set
  again for a second pipeline in a pass (the compute side was fixed so on
  2026-10-24): the render encoder now keeps each vertex buffer slot's
  buffer and offset (`setVertexBufferOffset:` where only the offset
  changes) and each stage's argument buffer per set, and sets only what
  changes.
- `spudgpu_cmd_begin_rendering` made a new `MTLRenderPassDescriptor` (and
  its attachment descriptors) every pass: 5 of Metal's 10 allocations of
  an offscreen frame. The list keeps one (`_pass_desc_mtl`), every field
  set or cleared at each begin and its textures cleared once the encoder
  is made: 6 a frame now, the command buffer's and encoders' own.
- The iOS SpudAudio stub (`spudaudiostub.c`) didn't compile: X16's
  `SPUD_STALE` without `spudallocator.h`.
- `tests/alloctrace.h` counted nothing in Release on macOS: clang takes
  `malloc` to read no global of the program's, so the store of
  `malloc_logger` before the counted code looked dead and was dropped.
  Both stores are volatile now; a planted malloc and calloc count 2 of 2
  in Debug and Release. Its table holds 16,384 stacks (1,024 before),
  since Metal's debug layer fills 1,024 in a frame and what has no room
  is counted as ours.
- `spud_noalloc_tests` asked Core Audio for `SPUDAUDIO_THREAD_PRIORITY_NORMAL`,
  which it refuses, so its SpudAudio scenario was skipped; it asks for
  `PLATFORM` there, as the other tests do, and the callback runs.
`spud_noalloc_tests` then: none of ours in SpudGPU's offscreen frame (6 a
frame of Metal's: command buffer, encoder, residency), its present loop
(about 24 a frame of Metal's and Core Animation's, most under
`nextDrawable`), SpudAudio's Core Audio callback (none at all), SpudNet's
idle round (none) and SpudInput's poll (2 of GameController's,
`[GCController controllers]`).

On 2026-10-31 SpudGPU took each API's own shader format and stopped
compiling (`spudgpu_shaderformat_draft.h`; cards A9, S17-S22): run on
Windows 11 on the Iris Xe and the RTX A2000 over D3D12 and Vulkan (and WARP
by a caller's tests), Debug, Release and `-asan`, 15 ctests passing,
`spud_misuse_tests` ten times on each backend, `spud_noalloc_tests` none of
ours, `spudgpu_objects_test` on both GPUs with the debug layer and
validation silent; clip space +Y up (Vulkan by negative viewport heights),
read back by the objects test and seen in every `spudgpusamples` sample;
`shaderDrawParameters` in Vulkan's base set. The Metal backend's module
path (a `.metallib`, the entry point by name and function type) is written,
not compiled.

On 2026-10-31 `SPUDGPU_EXT_RAY_QUERY` was built (card S12, step 1; the
approved `spudgpu_raytracing_draft.h`) and run on Windows 11 (ThinkPad P1
Gen 5): `windows-d3d12` and `windows-vulkan`, Debug and Release, 15
ctests each, and both `-asan` presets (VsDevCmd), 15 each; the NVIDIA RTX
A2000 builds, refits, compacts, clones and traces in
`spudgpu_objects_test` (`--adapter 1`) on both backends, every check
passing, under the D3D12 debug layer and GPU-based validation and under
Vulkan validation, synchronization validation and GPU-assisted
validation, all silent (GPU-AV's notices of its own settings aside); the
Intel Iris Xe has no `SPUDGPU_FEATURE_RAY_QUERY` on either API and every
call is refused on it, and on every adapter's first device; WARP (D3D12)
reports raytracing tier 1.1, and the whole test passes on it (its update
scratch is 0 bytes). `spud_noalloc_tests` holds the refit-and-trace frame
to no allocation of ours on the A2000 (both backends) and WARP;
`spud_misuse_tests` ten runs a backend (269 calls with zeroes, 242 with a
destroyed handle, no failure). Metal's file (`spudgpumetalraytracing.m`)
and the Metal halves of the context, heap and descriptor changes are
written, not compiled. Linux not built. ThreadSanitizer has not seen it.

On 2026-11-01 the Mac (Apple M5 Pro, macOS 27.0.1, Xcode 27.2, Slang
2026.18.3-2) compiled and ran both: the native-format path (a `.metallib`,
the entry point by name) and `SPUDGPU_EXT_RAY_QUERY` compiled at once, with
no warning. `macos-metal` Debug and Release under Metal API and shader
validation with warnings fatal, and `-asan` and `-tsan`: 15 ctests each,
passing; `spud_misuse_tests` ten times (258 calls with zeroes, 217 with a
destroyed handle, no failure); `spudgpu_objects_test` whole, ray queries
included (`SPUDGPU_DEVICE_TODO.md`, "Hardware ray tracing", has what);
both geometry and tessellation EXTs 0 and no adapter with either bit.
`iossimulator-metal` built and its tests run in an iPhone 17 Pro simulator
(iOS 26.5), all passing; SpudGPU's skip there, since the simulator's GPU
has no Metal 3 family. Changed:
- Metal's ray tracing file: `placement_alignment` from Metal's answer (1024;
  S24), the 72-byte instance asserted field by field, residency one
  `useResources:count:usage:` per encoder from a list of Metal objects kept
  on the top level, the build descriptors kept on the command list.
- The render encoder keeps its viewports and scissors and sets them only
  when they differ (validation refused a second equal one).
- Push constants go only to the stages whose function reads them, from the
  pipeline's reflection (validation refused one no argument takes).
- `tools/spudgpu_msl` builds for the build machine in a cross-compiling
  build too (ExternalProject, `tools/spudgpu_msl/CMakeLists.txt`), every
  shader step runs `${SPUDGPU_MSL_EXECUTABLE}` and names the build's own
  SDK and deployment target, and the tool tells SPIRV-Cross that iOS GPUs
  have base vertex and instance.
- SpudRandom on Apple is `SecRandomCopyBytes` (the iOS SDKs declare no
  `getentropy`); `spud_noalloc_tests` links as C++ for its demangler.
`spud_noalloc_tests`: none of ours, the refit-and-trace frame included (41
a frame of Metal's own there). Not changed and recorded as a known gap
since 2026-10-17, now a card (S27): Slang keeps only the bindings an entry
point reads, and the argument buffer the backend writes from a set's
layout doesn't match a module's that leaves bindings out.

- **Vulkan** — reference backend, plain C23, complete. Build against this first;
  D3D12 and Metal follow its shape.
- **D3D12** — complete, C++26. Cross-compiles the project's SPIR-V shaders to HLSL at
  runtime via **SPIRV-Cross**, built from source through `FetchContent` (deliberately
  not the Vulkan SDK's prebuilt libs — those are release-only `/MD` and mismatch a
  `/MDd` debug build; building from source lets it inherit `CMAKE_MSVC_RUNTIME_LIBRARY`
  correctly). Also pulls in Microsoft's vendored `d3dx12.h` (3,400+ lines, not
  authored here — don't "clean up" or reformat it as if it were project code).
- **Metal** — implemented, all twelve backend `.m` files, now that Apple hardware is
  available to validate against. Swap chain creation/present is verified end-to-end
  on real hardware (Apple M5 Pro) via `spudgpusamples/Samples/HelloTriangle`; the
  natives-header asymmetries above were worked out ahead of the implementation and
  held. Buffer/image copy (`spudgpu_cmd_copy_buffer`/`_copy_buffer_to_image`/
  `_copy_image_to_buffer`, `spudgpu_get_image_buffer_copy_size`) is also real now,
  via a new `_active_blit_encoder` on the command list following the same
  end-any-other-active-encoder-first pattern as `_active_compute_encoder` — verified
  end-to-end via `spudgpusamples/Samples/HelloTexture`'s texture upload, which
  silently rendered solid black until this landed (these were unimplemented
  placeholders before, per the file comment in `spudgpumetalrenderpass.m`).
  `spudgpu_cmd_blit_image` implements only a blit that is a copy (same size,
  orientation and format); a scaling, flipping or converting one is reported and
  records nothing. A pipeline's `cull_mode`, `front_face_ccw` and `wireframe` are applied since
  2026-10-13 (set on the encoder at `spudgpu_cmd_bind_pipeline`); before that Metal
  dropped all three and drew every triangle filled and unculled, which no caller had
  noticed because none culled. Back-face culling with counter-clockwise fronts is
  verified by a caller's read-back test; `SPUDGPU_CULL_MODE_FRONT`, clockwise fronts
  and wireframe are written and not run. A command list's command buffer is taken
  inside an autorelease pool of the backend's own, so a caller that is plain C with
  no run loop works; before, such a caller hung at its 65th command list.

## Build system

- **eqstd** (`EQ_ARRAY` and the rest; see "An array that grows") is linked
  PRIVATE. `CMakeLists.txt`'s `spud_add_workspace_or_fetch` adds the
  workspace's `../eqstd` checkout, edits included, when it is there (in its
  own binary dir, and only if no `eqstd` target exists yet), and otherwise
  fetches `endqueue/eqstd` at the commit pinned there (a hash, never a
  branch), so a clone of spudlib alone still builds. eqstd's own tests are
  built only when it is the top-level project. Moving the pin: change the
  hash in the call, with the code that needs the newer eqstd.
- `GRAPHICS_BACKEND` is a single-choice CMake cache variable (`Vulkan` / `D3D12` /
  `Metal`) — there is no multi-backend build. The source list is assembled per-platform
  and per-backend *before* `add_library`, so only one backend's translation units ever
  compile in.
- `CMAKE_MSVC_RUNTIME_LIBRARY` is **not** set here — it must already be force-set by
  the parent `eqdev` root `CMakeLists.txt` before `add_subdirectory(spudlib)` runs (see
  `../CLAUDE.md`). If you're building `spudlib` standalone rather than through the
  workspace root, set it yourself before configuring, matching whatever
  it will be linked with uses, or Debug builds will fail to link once combined.
- SDL3 integration is header-only glue (`spudgpu_sdl3.h`) — one inline
  `spudgpu_create_surface_from_sdl3` per backend (wraps `SDL_Vulkan_CreateSurface` on
  Vulkan, reads the raw `HWND` off SDL's window properties on D3D12). Don't grow this
  into a real SDL abstraction; it exists only to bridge surface creation.
- `.clang-format` is LLVM-based with tab indentation (`UseTab: ForIndentation`,
  `TabWidth`/`IndentWidth` 4), always-one-param-per-line, 160-column limit, and aligned
  consecutive assignments. Run it before committing.

## Working with in-progress changes

- If a fix/edit request touches a file that already has uncommitted work in
  progress (yours from an earlier session, or the user's own local changes),
  check `git diff` first. If the new fix's lines don't overlap that WIP, apply
  it directly to the working tree rather than isolating it on a separate
  branch/PR — that's extra ceremony for something that's going to land in the
  same working copy anyway. Only fall back to a standalone branch/PR when the
  tree is clean, or ask before proceeding if the fix's lines genuinely
  conflict with the WIP. Never discard or clobber the existing uncommitted
  work to make room for a fix.

## Known gaps (don't re-flag as surprises)

- **Threads in SpudGPU (2026-10-21): SpudLib locks what it hides; the
  caller synchronizes the handles it holds.** State the caller can't see is
  SpudGPU's to guard, on every backend: the bindless slot table, an
  adapter's one-device claim (an atomic exchange), a device's descriptor
  budget, D3D12's shared descriptor heaps and indirect signatures. Handles
  the caller holds are the caller's: a queue,
  like a command list, allocator or swap chain, is used by one thread at a
  time, on every backend, and SpudGPU takes no lock for it (`spudgpu.h`,
  `spudgpu_submit_command_lists`). A lock there would be a decision made for
  the caller, and a hidden stall: a present or wait-idle holding it blocks
  every other thread's submit. Done so far:
  - Every backend: submit, `spudgpu_queue_submit` (which the synced submit
    goes through), wait-idle and present bracket their use of the queue with
    a debug-build counter (`spudgpuvulkan___queue_enter`,
    `spudgpu_d3d12___queue_enter`, `spudgpumetal___queue_enter`) that
    asserts when two threads are in one queue at once; nothing in release.
    D3D12's `spudgpu_queue_submit` queues its waits, lists and signals by
    separate calls and bumps semaphore counts, so it was never safe from two
    threads at once either. Metal also uses its queue on its own when a
    swap chain is made and when a list takes its command buffer; Metal's
    queue allows that from any thread, and it has no Vulkan or D3D12
    counterpart.
  - Since 2026-10-21 nothing is made later: the instance lists its adapters
    when it is created and never changes the list, so every adapter call is
    lock-free; a device's queues and bindless table are made by
    `spudgpu_create_device`, before the device is handed out, and read with
    no lock after (the old `_devices_lock`, `_bindless_create_mutex` and D3D12's
    atomic `_bindless` went with the lazy state they guarded). One device per
    adapter is an atomic exchange on the adapter.
  - Bindless registering and unregistering hold the bindless state's lock
    (Vulkan's `spud_mutex` across the `vkUpdateDescriptorSets` too, since the
    set is externally synchronized; D3D12's around its slot stacks, with the
    `Create*View` writes outside it, as D3D12 allows).
  - A threaded test on a fake driver (before 2026-10-21) found the old lazy
    races (8 logical devices, a slot handed out twice, TSan reports) and none
    after the fix, TSan, ASan and UBSan clean; the debug assert fired on
    unsynchronized submits and not on synchronized ones.
  Not done: `spudgpu.h` states the queue rule, and nothing yet about bindless
  or the other calls.
- An indirect draw's `stride` is a multiple of 4 and at least the size of
  its args struct, on every backend (Vulkan's rule); otherwise nothing is
  recorded. A larger stride skips the caller's data between entries. Until
  2026-10-16 D3D12 ignored `stride`: its one cached command signature fixed
  `ByteStride` at the args size. It now keeps a signature per stride and
  kind on the device: the two tightly packed ones made with the device (so
  the common case takes no lock, since 2026-10-21), any other in
  `_indirect_signatures`, mutex-guarded, made at first use; all released with
  the device. `apricottests`' `test_draw_indirect_stride` draws with a
  32-byte stride; it fails on the old D3D12 code and passes on all three
  devices here. The same test found that D3D12's `spudgpu_create_buffer_view`
  refused a buffer that was not a vertex, index or uniform buffer (Vulkan and
  Metal make a view of any buffer); it now keeps the range for any usage.
- The D3D12 backend lists adapters of feature level 12_0 and up (it
  required 12_2, from its first commit, for no reason any code had; changed
  2026-10-16). 12_0 guarantees resource binding tier 2, which descriptor
  sets of more than 128 SRVs need. Everything past it is a feature bit,
  read per adapter at instance creation through a probe device released
  before the next adapter's (since 2026-10-21; `SPUDGPU_DEVICE_TODO.md`):
  mesh shading (OPTIONS7), the depth bounds test (OPTIONS2), and bindless,
  which needs resource binding tier 3. WARP is listed, as
  `SPUDGPU_ADAPTER_TYPE_SOFTWARE`. Run on an Iris Xe (12_1, tier 3) and an
  RTX A2000 before the adapter change, and every `spudgpusamples` sample on
  both and on WARP after it (2026-10-22); the tier-2 path has not run on any
  device.
- D3D12's shader-visible descriptors are in two heaps per device
  (`spudgpu_device_d3d12::_csv_heap`, `_sampler_heap`), and every pool and
  the bindless state take a range of them (`spudgpud3d12descriptors.cpp`).
  Before 2026-10-16 each pool had heaps of its own, so binding sets of two
  pools, or bindless and a pool, for one draw left a table in a heap no
  longer set. Since 2026-10-21 the heaps are made by `spudgpu_create_device`,
  sized by the caller's descriptor budget (`spudgpu_device_desc::max_descriptors`
  and `::max_samplers`, at most 1,000,000 and 2048), and a budget of 0 makes
  no heap. On every backend a pool past what is left of the budget is
  refused with `SPUDRESULT_GPU_OUT_OF_DESCRIPTORS` (Vulkan and Metal keep a
  count, D3D12 the heaps, whose free ranges may be fragmented, which the
  header allows everywhere). Run on D3D12 (NVIDIA RTX A2000) by the samples
  and `aprend_gpu_tests` before the budget, debug layer clean, and by every
  sample after it (2026-10-22, both devices and WARP).
- A D3D12 `TEXTURE2D` view has no array slice, so until 2026-10-16 every
  `SPUDGPU_IMAGE_VIEW_TYPE_2D` view on D3D12 saw layer 0 whatever its
  `base_array_layer` (render target, depth, shader resource and storage
  views alike). A 2D view of any other layer is now made as a 2D array view
  of that one layer (`spudgpu_d3d12_view_type`). Found by `apricottests`'
  `test_generate_mips`, whose layer 1 rendered into and sampled layer 0;
  it passes on both devices here, debug layer clean. A shader that
  declares a `Texture2D` and is handed such a view (a 2D view of a layer
  past the first) gets a `TEXTURE2DARRAY` view of one slice, which D3D12
  leaves undefined; the drivers here read it right. A caller that samples
  one layer of an array texture declares an array texture and passes a
  one-layer 2D array view, as a caller's downsample shader does since the
  same day; then the view and the shader agree on every backend. A run
  of `aprend_gpu_tests` with GPU-based validation on (a temporary
  `SetEnableGPUBasedValidation(TRUE)`, both devices, 2026-10-16) reported
  nothing, but it also reported nothing when a storage view was made
  deliberately of the wrong dimension, which the test itself caught; so
  this validation doesn't check view dimensions here, and the clean run
  doesn't settle the question. Render target, depth and storage
  views have no cube dimension in D3D12, and a `CUBE` or `CUBE_ARRAY` view
  of one of those kinds was the same single-layer `TEXTURE2D` view; it is
  now the 2D array of the view's layers, every face, as on Vulkan. HLSL
  has no `RWTextureCube` either, and SPIRV-Cross emitted a storage
  `imageCube` as a `TextureCube` at a `u` register, which DXC refuses; so
  before cross-compiling, the D3D12 backend makes every storage cube
  image type in the SPIR-V a 2D array image type
  (`spudgpu_d3d12_storage_cubes_as_arrays`), whose third coordinate is
  the face as the cube's was. An `imageSize()` of a cube takes the width
  and height of the array's three (not run). `apricottests`'
  `test_cube_storage_views` writes every face of two cubes through a
  `CUBE` and a `CUBE_ARRAY` storage view and reads all twelve layers back;
  against the old views it wrote layer 0 twice and nothing else. It passes
  on D3D12 (both devices, debug layer clean), on Vulkan, and on Metal (M5
  Pro, validation clean with warnings fatal). Shader resource
  views of cubes were right already: a `CUBE` view starts at layer 0 by the
  header's rule, and `CUBE_ARRAY` takes its first face.
- Until 2026-10-16 the Vulkan device never enabled `imageCubeArray`, so a
  `SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY` view, and any shader that declares a
  cube array, were outside Vulkan's rules on every device. It is enabled
  for a device created with `SPUDGPU_FEATURE_CUBE_ARRAY_VIEWS` (every D3D12
  and Metal adapter has it), and `spudgpu_create_image_view` refuses a
  `CUBE_ARRAY` view on a device without it, on every backend. Run on Vulkan by `test_cube_storage_views`
  on both devices here, validation layer clean, and on Metal, where the
  property is true and both cubes ran (validation clean).
- Since 2026-10-16 the Vulkan device can enable `multiDrawIndirect` (since
  2026-10-21 only with `SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT`), and
  `SPUDGPU_ADAPTER_PROPERTIES::max_draw_indirect_count` reports what one
  indirect call takes on a device with it (1 without it, on every backend;
  UINT32_MAX on D3D12 and Metal). A larger count records nothing, on every
  backend.

- The debug name rule (see "A debug name is copied") was
  applied to every module on 2026-10-09. A device's queues are the device's,
  made and freed with it, names and all (Vulkan's graphics queue had no
  release path, a new one on each call, until 2026-10-21). Descriptor sets had none either until 2026-10-21; a Vulkan pool now keeps its sets in a list
  and frees them on reset and destroy, as D3D12's does.
- Cross-backend return values (see "A function returns the same value for the
  same condition in every backend") were aligned across SpudGPU and SpudAudio on
  2026-10-09; SpudFiles, SpudMemory and SpudNet already matched. What still
  differs in SpudGPU, each a gap in one backend and not a choice:
  - Metal has no `spudgpu_get_shader_pipeline_desc`, bundle or bindless calls;
    D3D12 has no `spudgpu_create_surface_from_callback` or swap chain
    semaphore getters.
  - D3D12 returns `SPUDRESULT_GPU_INVALID_IMAGE_TYPE` for an image desc
    whose `type` is not an image type (from building the resource desc, so
    after every other check, and after `SPUDRESULT_GPU_HEAP_MISMATCH` for a
    placed image); Vulkan and Metal don't check it.
  - Metal: a set's argument buffer is laid out from every binding of its
    layout, and a shader's argument buffer struct from the bindings it
    declares (every declared one since 2026-10-17, used or not). A shader
    that declares only some of a set layout's bindings, and reads one past
    a binding it leaves out, reads the wrong argument. Every shader that
    reads a set declares the set's whole layout (a shared include does it);
    the fix is SPIRV-Cross's `pad_argument_buffer_resources`, which its C
    API doesn't expose, with the layout's bindings given at pipeline
    creation.
  - D3D12's `spudgpu_cmd_begin_rendering` returned early without a colour
    attachment until 2026-10-17, so a depth-only pass rendered on Vulkan
    and Metal only. It now begins a pass of a depth attachment alone;
    written on macOS, then run on D3D12 on 2026-10-17 by `apricottests`'
    `test_lit_mesh_shadow` (Intel Iris Xe and NVIDIA RTX A2000, debug
    layer clean). A caller's host built and ran over it on D3D12 and
    Vulkan the same day (reported), but that caller renders no depth-only
    pass, so that run doesn't exercise it.
  - Metal's `spudgpu_create_swap_chain` rejects `buffer_count != 1` and
    `SPUDGPU_PRESENT_MODE_MAILBOX`. Geometry and tessellation are behind
    `SPUDGPU_EXT_GEOMETRY_SHADER` and `SPUDGPU_EXT_TESSELLATION` since
    2026-10-31 (0 on Metal; a module or pipeline of either stage on a device
    without the bit is refused the same way on every backend, after the
    mesh check). Run on Windows 11 on the Iris Xe and the RTX A2000, both of
    which have both bits over D3D12 and Vulkan; the refusals tested on a
    device created without them; Metal written, not compiled.
- **Present scaling (2026-10-23).** How the compositor shows a back buffer
  of another size than the surface was decided silently before, differently
  per backend: D3D12 hardcoded `DXGI_SCALING_STRETCH`, Metal left
  `contentsGravity` at Core Animation's `kCAGravityResize`, and Vulkan took
  the surface's `currentExtent` where it is fixed (so nothing was scaled) and
  left a mismatch unspecified where it is free (Wayland). Now
  `spudgpu_swap_chain_desc::present_scaling` names one
  `SPUDGPU_PRESENT_SCALING` (ONE_TO_ONE, STRETCH, ASPECT_FIT; NONE refused
  with `SPUDRESULT_GPU_INVALID_PRESENT_SCALING`), and
  `spudgpu_get_surface_present_scalings` says which a surface takes; a
  scaling outside that is refused with
  `SPUDRESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED`. No filter and no bar color
  is promised, since neither can be chosen on every backend. The checks
  before the surface is read are written once
  (`spudgpu___internal_check_swap_chain_desc`,
  `spudgpu___internal_check_surface_present_scalings`). What each backend
  does:
  - D3D12: ONE_TO_ONE is `DXGI_SCALING_NONE`, STRETCH
    `DXGI_SCALING_STRETCH`. No ASPECT_FIT: `DXGI_SCALING_ASPECT_RATIO_STRETCH`
    acts as a stretch on an HWND swap chain (DXGI_SCALING's docs). No
    ONE_TO_ONE on a `WS_EX_LAYOUTRTL` window, where `DXGI_SCALING_NONE`
    aligns the right edges; the style is read at the query and at creation.
    Written on macOS; compiled and run on Windows 11 on 2026-10-23 (Debug,
    debug layer on, Intel Iris Xe and NVIDIA RTX A2000): both surfaces
    report one-to-one and stretch, and HelloTriangle in a 900x900 window
    over 1280x720 back buffers showed one-to-one pixel for pixel from the
    top left, cropped on the right (the strip below black), and stretch
    filling the window; aspect-fit refused. No debug layer message. The
    `WS_EX_LAYOUTRTL` case has not run.
  - Vulkan: ONE_TO_ONE always. With `VK_KHR_swapchain_maintenance1` (or
    the EXT original; the instance enables `VK_KHR_get_surface_capabilities2`
    and `VK_KHR`/`VK_EXT_surface_maintenance1` where the loader has them,
    and a device created with PRESENT enables the adapter's swapchain
    extension with `swapchainMaintenance1`), STRETCH and ASPECT_FIT (centred
    gravity) where `VkSurfacePresentScalingCapabilitiesKHR` offers them for
    every present mode the surface has, and ONE_TO_ONE as the extension's
    one-to-one with `MIN` gravity where offered. A scaled swap chain takes
    the caller's size within `[minScaledImageExtent, maxScaledImageExtent]`;
    an unscaled one keeps `currentExtent` where the surface fixes it. No
    feature bit: what it offers is per surface and per present mode, and the
    query answers that. No ASPECT_FIT on a Win32 surface (below). Type-checked
    on macOS against the Vulkan 1.4.341 headers (the Windows SDK the build
    pins), with a stub Wayland header; compiled and run on Windows 11 on
    2026-10-23 (Debug, validation on, synchronization validation too for the
    samples). The Intel Iris Xe has no swapchain_maintenance1 extension:
    one-to-one only, back buffers at the window's size (900x900 for a
    1280x720 request), nothing scaled. The NVIDIA RTX A2000 has
    `VK_KHR_swapchain_maintenance1`, and its surface offers all three
    scalings with every gravity for FIFO, MAILBOX and IMMEDIATE (scaled
    range 1x1 up). HelloTriangle there showed one-to-one as D3D12 does
    (identical capture) and stretch filling the window, both over 1280x720
    back buffers. The run found two bugs, both fixed:
    - The device's copy of the instance (`_instance`, from before adapters)
      was never filled in, so the scaling query called a null
      `vkGetPhysicalDeviceSurfaceCapabilities2KHR` and crashed on any device
      with the extension. The adapter now keeps the call (copied from the
      instance when it records the extension) and the device reads it
      there; the stale field is gone.
    - The A2000's driver offers `ASPECT_RATIO_STRETCH` with centred gravity
      on a Win32 surface and shows a stretch (its capture identical to
      stretch's), as DXGI does with `DXGI_SCALING_ASPECT_RATIO_STRETCH` on a
      window. ASPECT_FIT is not offered on a Win32 surface any more, which
      matches D3D12. Linux (Wayland's free extent, X11) has not run.
  - Metal: all three, as `contentsGravity` (`kCAGravityTopLeft`, or
    `kCAGravityBottomLeft` on a layer whose `contentsAreFlipped`, read at
    creation; `kCAGravityResize`; `kCAGravityResizeAspect`), set with
    actions off. No `magnificationFilter`. Run on macOS (Apple M5 Pro,
    Debug): `spudgpu_objects_test` makes a swap chain of each on a windowless
    `CAMetalLayer`, flipped and not, and reads the gravity back (plain,
    ASan, TSan, Metal validation), and `spudgpusamples`' HelloTriangle
    showed each in a 900x900 window over 1280x720 back buffers
    (`--scaling`, `--window`), under Metal validation with warnings fatal:
    top-left and cropped, stretched, and letterboxed.
  Every caller in the workspace names one: a caller's renderer that scales
  with draws of its own names ONE_TO_ONE, and so does every sample (their
  windows can't be resized). `spudgpu_objects_test`'s checks before the
  surface is read pass on Windows on both backends (adapter 0, the Iris Xe,
  only: the test takes the first adapter, and no real swap chain is made
  there). No test reaches the extension's path, which is how the crash
  above went unseen: only a real surface on the A2000 did. Vulkan still
  needs a Linux run, Wayland's free extent included.
- SpudFiles was reworked on 2026-10-22 (`SPUDFILES_TODO.md`): an instance
  with the caller's allocator; UTF-8 paths (Windows converts each to UTF-16
  and calls only the W functions); access, creation and share mode chosen by
  the caller; reads and writes at an offset, with no cursor; what is at a
  path from `spudfile_path_get_info`; one directory level per
  `spudfile_directory_create`. The dialogs, path-string functions and working
  directory are gone, and with them every static buffer. Every check is in
  the front end (`src/spudfiles.c`); the backends implement
  `src/files/spudfilesshared.h`: one POSIX file for Linux and Apple
  (`F_FULLFSYNC` where it exists) and Windows. Built and run on macOS,
  and on Windows 11 (MSVC, `windows-d3d12`, Debug, 2026-10-22); the Linux
  build hasn't been compiled. The platform's own error number isn't kept yet.
- SpudNet was written on 2026-10-07 and its header reworked, and every backend
  rewritten to it, on 2026-10-08: streaming HTTP transfers, TLS and proxy
  descs, error detail, a resolver, a wait set. The Apple side compiles clean
  with the `macos-metal` preset and passes `tests/spudnet_test` (run it with
  `tests/run_spudnet_test.sh build-macos-metal/Debug/spudnet_test`; it needs Python 3
  and `openssl`; 343 checks since 2026-10-22 (and an opt-in `internet` part, 22 more, run only by name: real DNS, public certificate chains good and bad, a real `wss://` echo server and a host that never answers; passing on macOS plain, ASan and TSan), with the struct-base and allocator parts and a `records` part: a listener's accept aborted from another thread, empty error records, the local IPv4 addresses and debug names read back). Linux (libcurl + BSD sockets) built with the
  `linux-vulkan` preset and passed it on 2026-10-14. Windows (WinHTTP +
  Winsock2) built with MSVC under the `windows-d3d12` preset and passed all
  261 checks on 2026-10-14, Windows 11 (`tests/run_spudnet_test.sh
  build-windows-d3d12/Debug/spudnet_test.exe` from Git Bash). The first run
  failed six. Five came from WinHTTP holding back
  `WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE` until the response
  arrives, which made `_start` wait for the response. `_start` now returns
  at `WINHTTP_CALLBACK_STATUS_REQUEST_SENT` and leaves the completion in
  flight for the next call. The sixth was the test: WinHTTP shares
  connections between every session in a process, so the test's
  `server_stat` now asks over its own TCP connection. That sharing is
  WinHTTP's own: it doesn't break the one-transfer-per-client rule, but a
  new client is not a new connection there. `SPUDNET_TODO.md` lists what each
  part assumes and what the macOS run settled. Every part of SpudNet (TCP, the
  resolver, HTTP and WebSocket) now has a caller outside this repo. On Linux, WebSocket needs libcurl 8.11+ (older ones
  return `SPUDRESULT_SPUDNET_UNSUPPORTED`; HTTP works regardless). iOS: the
  `iossimulator-metal` preset builds the tests as plain simulator executables,
  and `SPUDNET_TEST_LAUNCHER="xcrun simctl spawn booted"
  tests/run_spudnet_test.sh build-iossimulator-metal/Debug/spudnet_test` runs
  `spudnet_test` in a booted simulator against the server on the Mac's loopback.
  On 2026-10-14 every target built with no warnings and `spudnet_test` passed
  all 261 checks three runs in a row in an iPhone 17 Pro simulator (iOS 26.5),
  its `websocket` part 46 of them; `spudperf_test` and `spudinput_test` pass
  there too. A device can't run a bare executable, so on a device SpudNet has
  only been built (a caller's iOS build), not run. SpudNet's own parsers of
  network bytes (header blocks, certificate DER, UTF-8, error text, proxy
  URLs) have libFuzzer targets in `tests/fuzz/` (2026-10-28): the
  `macos-fuzz` preset (Homebrew LLVM's clang, `build-macos-fuzz`) or
  `linux-fuzz` builds them, and each also runs as a plain ctest,
  `<target>_replay`, over its seeds and every input kept in
  `tests/fuzz/regressions/<target>`, which is where a crashing input goes
  once fixed. How to run them: `tests/fuzz/README.md`. On Windows the
  `windows-fuzz` preset (LLVM's clang through its GNU-style driver,
  2026-10-29; `../claude_made_decisions.md` says why) builds them, and
  `spudnet_fuzz_http_head` has a mode for WinHTTP's own split of its raw
  header block (`spudnet_win_add_raw_headers`,
  `src/net/backends/windows/spudnetwinhttphead.h`); each target ran 10
  minutes there with nothing found.
- SpudInput was written on 2026-10-13: the header, the front end and the
  GameController backend (`src/input/backends/apple/spudinputgamecontroller.m`).
  It builds with the `macos-metal` preset and `tests/spudinput_test` passes
  with no gamepad connected (argument checks, absent-gamepad results, polls
  that find nothing, instance create and destroy). On 2026-10-14 a caller
  read a gamepad through it on macOS, held and used by a person, polling
  once a frame on the main thread: it was found when connected, both sticks
  and both triggers moved what they should in the directions and ranges the
  header states, and the left stick pressed in read down and up. Not run:
  the other buttons one by one, a press and release between two polls (the
  change handler's `buttons_went_down` with the button up again), battery,
  touch, motion, lights, labels and the CoreHaptics rumble. Known to
  be approximate on GameController: a touch exactly at a surface's middle
  reads as no touch; a motion sample's time is its arrival, not its capture;
  the motion frame and the sign of acceleration are from documentation and
  unchecked. It builds for iOS, and `spudinput_test` passes in the simulator
  (2026-10-14); no gamepad has been read there.
- SpudInput's GameInput backend (`src/input/backends/windows/spudinputgameinput.c`)
  was written on 2026-10-14, on the Windows SDK's GameInput (`GameInput.h`,
  `gameinput.lib`; no NuGet package). Devices are learnt from a device
  callback: a blocking enumeration when the instance is made, so the first
  poll lists what is connected, then a dispatcher so that every later
  callback runs inside the poll's dispatches. Each poll walks the device's
  reading history from the reading it took last, falling back to the newest
  on `GAMEINPUT_E_REFERENCE_READING_TOO_OLD`. `spudinput_test` passes on
  Windows 11 with a wired Xbox Series X|S controller (045e:0b12): found at
  the first poll, with four rumble motors and a battery state of none.
  GameInput gives no name for it, and sends no reading until about 0.7 s
  after it is first asked, then one only when something changes. Pressing
  buttons, moving sticks and rumble have not been checked by anyone holding
  it. No motion sensors, touch surfaces, lights or labels. The SDK header's
  C macros for `IGameInputDispatcher` are missing a parenthesis, so the file
  calls every method through `lpVtbl`, and the header needs `<stdbool.h>`
  before it in C.
- SpudInput's evdev backend (`src/input/backends/linux/spudinputevdev.c`) was
  written on 2026-10-14. It finds gamepads by `BTN_GAMEPAD`, learns of new
  nodes through inotify on `/dev/input` read at each poll, takes every event
  between polls, and plays `FF_RUMBLE` in 2-second plays that each poll past
  a second renews. `spudinput_test` passes on Fedora 44 with an Xbox Series
  X|S controller (xpad) connected: its name, ids, buttons, axes, analog
  triggers and rumble were found and its state at rest read. Pressing
  buttons, moving sticks and rumble have not been checked by anyone holding
  it. Battery and lights were added the same day, from sysfs under the
  node's input device and the device above it: the first `power_supply` of
  type Battery (files kept open, `pread` from 0 at each poll; `capacity`,
  else `capacity_level` as 5 steps), a multicolor LED with red, green and
  blue channels or else three single-colour LEDs, and LEDs named
  `player-1`... (player n lights `player-n` alone). They are found once,
  when the gamepad is first seen. No device here has either; they have run
  only against fake sysfs trees modelled on hid-playstation and hid-sony,
  with a scratch harness that is not committed. Writing an LED needs root
  unless a udev rule grants it, so lights usually return
  `SPUDRESULT_SPUDINPUT_ACCESS_DENIED`. Not written: motion sensors and
  touch surfaces (sibling event nodes of the same device); no gamepad is
  given them. xpad reports the Xbox X button
  (left) as `BTN_X`, which the kernel defines as `BTN_NORTH`, and Y (top) as
  `BTN_WEST`; SpudInput passes the kernel's code on, as it has no table of
  devices.
- Eight CTest targets exist: `spudcore_test` (the version as the header,
  CMake and the library each have it, and result names), `spudgpu_device_test`
  (adapters and devices: listing, adapter properties with chained limits,
  every create-device check, queues by kind, the descriptor budget, one
  device per adapter, a device made again after one is destroyed; it needs a
  GPU and no window, and exits 77, skipped, with no adapter; passes on Metal,
  plain, ASan and TSan, 2026-10-21), `spudgpu_objects_test` (the SpudGPU
  calls nothing else in the workspace made until 2026-10-22: a format by
  name, a fence signalled from the CPU and waited on, a semaphore, a buffer's
  desc and GPU address, a buffer view's and a heap's desc, a descriptor pool
  reset and allocated from again; also a GPU and no window, 77 without one;
  passes on Metal plain, ASan, TSan and under Metal validation, 2026-10-22;
  since 2026-10-23 also the present scaling checks that come before a
  surface is read, on a second device made with PRESENT, and on Metal swap
  chains on a windowless `CAMetalLayer` (`tests/spudgpu_test_metal_layer.m`),
  passing the same four ways;
  still never called anywhere: `spudgpu_create_surface_from_callback`, the
  storage-buffer and storage-image bindless calls,
  `spudgpu_cmd_bind_bindless_resources_compute` and
  `spudgpu_get_compute_pipeline_desc`), `spudmemory_test` (every argument
  check, zero pages on commit and after a decommit, a second commit keeping
  contents, a reservation larger than memory), `spudfiles_test` (every SpudFiles
  call and check in a temporary directory: each access and creation choice,
  reads and writes at an offset with a short read at the end, sizing,
  flushing, a mapping at an unaligned offset outliving its file, path info,
  replace, remove, one-level directories, a non-ASCII name, two threads
  reading one file; counting allocator), both added 2026-10-22 and passing on
  macOS plain, ASan and TSan, and on Windows 11 (MSVC, Debug, 2026-10-22),
  `spudaudio_test` (every call but `spudaudio_stream_start` and a real
  `spudaudio_set_device_sample_rate`, so nothing is heard and no device is
  changed: argument checks, both directions' devices and properties, timing
  caps, a probe of the default output's own format followed to acceptance,
  a stream created stopped, read and destroyed, the device-event callback
  set and cleared, all through a counting allocator; passes with no device;
  added 2026-10-22, macOS plain, ASan and TSan; on Windows 11 over WASAPI,
  Debug, Release and ASan, 2026-10-29),
  `spudinput_test`
  (above) and `spudperf_test` (the monotonic clocks,
  `spudperf_sleep_until_monotonic_ns` and `spudperf_sleep_until_monotonic_ms`,
  and since 2026-10-22 the CPU counter and `spudperf_get_ram_usage`),
  which needs nothing but the machine. It has run on macOS; the Windows and
  Linux halves of the nanosecond sleep, which the millisecond one waits
  through, are written and have not been compiled. `tests/` also holds two hardware tests, built behind
  `SPUDLIB_BUILD_TESTS` but deliberately not registered with ctest because each
  needs something real: `spudaudio_sine` (an audio device and someone listening;
  until 2026-10-22 its probe loop zeroed the config's base each round, so
  every probe was refused as `SPUDRESULT_STRUCT_WRONG_TYPE` and it never
  reached a stream; fixed. Run on Windows 11 over WASAPI on 2026-10-29,
  Debug, on the default output (Sony WH-1000XM5, Bluetooth; 48 kHz float
  stereo): the tone was heard clean, twice, by someone listening. Every check
  passed but one on the first run: "host time agrees with sample rate
  (within 1%)" measured 45,243.7 Hz against 48,000 (47,544.2 on the 1 s
  restart, a pass). The frames themselves kept time (3.15 s of audio in
  3.39 s); what drifted is each callback's `host_time_ns` over the first
  seconds, likely the Bluetooth path's latency settling, which the check
  counts over a short run. Not looked into further; the wired speakers have
  not been run)
  and `spudnet_test` (a local server, started by `tests/run_spudnet_test.sh`).
  SpudGPU's are `spudgpu_device_test` and `spudgpu_objects_test`, which
  draw nothing; the rest of SpudGPU is
  verified by running the `spudgpusamples` samples.
- No static/immutable sampler support. `spudgpu_sampler` (added alongside
  `SpudGPUDynamicIndexing`) only covers the dynamic, descriptor-bound case — a real
  `VkSampler` written into a descriptor set on Vulkan, a heap-slot `CreateSampler` on
  D3D12, an argument-buffer `MTLSamplerState` on Metal. D3D12's *static* samplers
  (baked into the root signature at pipeline-creation time, zero descriptor-heap cost)
  and Vulkan's *immutable* samplers (`VkDescriptorSetLayoutBinding::pImmutableSamplers`,
  set to `NULL` in `spudgpuvulkandescriptors.c`) are a real, better-fitting mechanism for the common
  case of a small fixed set of samplers that never change for the life of a pipeline —
  worth adding as a genuinely separate concept alongside `spudgpu_sampler`, not a
  replacement for it (Metal has no equivalent distinction — a regular sampler bound
  once already is the zero-cost path there). Needs reconciling two different points in
  the object hierarchy: Vulkan's immutable samplers live on the descriptor-set-layout
  binding, D3D12's static samplers live on the pipeline/root-signature.
  **Decided 2026-10-28 with the user (card S7):** immutable samplers go on the
  set-layout binding, in the core API (not an `EXT`: every backend meets the
  contract): native on Vulkan, folded into the root signature D3D12 builds
  from the layout as static samplers, bound by the Metal backend whenever the
  set is bound. The immutable sampler's desc has a struct base, so a YCbCr
  conversion can chain onto it: if ever built,
  `SPUDGPU_EXT_SAMPLER_YCBCR_CONVERSION` (Vulkan only, an optional feature
  there, with its `_NOT_SUPPORTED` result; D3D12 and Metal have none). Video
  doesn't need it: per-plane views (card S11) and conversion in the shader
  work on every backend. Built when a caller asks.
- `SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` works on Metal since
  2026-10-22. Before, Metal's `spudgpumetaldescriptors.m` wrote only the
  texture half into the argument buffer, and under SpudGPU's
  per-descriptor-set argument buffers SPIRV-Cross's sampler member for a
  combined sampler took the texture's own id, so a shader with one didn't
  compile (SPIRV-Cross allows that overlap only with Metal 3's mutable
  aliasing). Now the argument buffer layout gives the two halves distinct
  members: the texture keeps the binding's number, and the sampler sits at
  `SPUDGPU_METAL_COMBINED_SAMPLER_INDEX_BASE` (1024) past it, by a remap of
  each combined sampler at cross-compilation (`spudgpumetalshader.m`), a
  second argument in the layout's encoder and a second write
  (`spudgpumetaldescriptors.m`). A layout with a combined binding refuses a
  binding at or past 1024. Run on Metal by a caller's test of two
  combined samplers beside a separate pair, and by the same test on D3D12
  and Vulkan
  (2026-10-23, Windows 11, Intel Iris Xe and NVIDIA RTX A2000, debug and
  validation layers silent), where combined samplers needed no change;
  `spudgpusamples/Samples/HelloTexture` still uses the separate
  `SAMPLED_IMAGE` + `SAMPLER` pair it was written with.
