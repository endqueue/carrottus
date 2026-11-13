# Carrottus

Hardware Abstraction Layer (HAL). This is a from-scratch
translation layer over GPU, file system, memory, network, and audio primitives — a general HAL,
not a GPU library with utilities bolted on. See `../CLAUDE.md` (the `eqdev` workspace
root) for how this repo fits into the workspace; this file covers
conventions specific to working inside `carrottus` itself. `MATURITY_TODO.md`
is the plan for bringing the parts written before these rules up to them
(a version, result names, the caller's allocator (done) and a stability
policy for every module; a rework of Carrottus Files (done 2026-10-22); `bindings/`
is removed and Carrottus Input's struct bases are in).

## The one rule everything else follows

**Carrottus is pure mechanism, no hidden policy.** It translates a platform/GPU API into a
consistent C interface and stops there. Every choice is visible and the caller's
(worded so 2026-10-28, card S2): where a behaviour must exist at all, such as
where validation messages go, the caller supplies it. It never makes a decision on
the caller's behalf, never has a default that hides a choice, and holds no global state.

- Expose enumeration/query functions so the caller can inspect and
  decide — `crgpu_adapter_enumerate`, not `crgpu_select_best_device`.
- No "convenience" fallback that picks something for the caller. If a change feels
  like it wants one, that decision belongs to the caller, not here.
- Everything flows through explicit handles the caller owns and passes in — no
  library-owned globals.
- If a bug turns out to be Carrottus silently defaulting instead of translating a
  caller-supplied parameter (e.g. `crgpu_image_view_create` once hardcoded
  `VK_IMAGE_ASPECT_COLOR_BIT` regardless of what the caller asked for), that's a
  design-principle violation, not just a bug — fix it by passing the real parameter
  through, not by adding a special case.
- When a fix spans Carrottus and its caller, make the correct structural change on
  both sides rather than working around a layering gap from one side only.
- **Carrottus doesn't know who calls it.** Nothing here - code, comments, headers,
  docs - names a caller or assumes which one it is. Write "the caller".

## Module map

Every module returns the shared `CR_RESULT` enum (`crcore.h`, 165 values with
`CR_SUCCESS`, as of 2026-10-23) rather than inventing its own error convention.

| Module | Header | Prefix | Backends |
|---|---|---|---|
| Carrottus GPU | `crgpu.h` | `crgpu_` | Vulkan, D3D12, Metal |
| Carrottus Files | `crfiles.h` | `crfiles_` | Windows, Linux, macOS |
| Carrottus Memory | `crmemory.h` | `crmem_` | Windows, POSIX (Linux + Apple, one file) |
| Carrottus Net | `crnet.h` | `crnet_` | TCP sockets: Winsock2, BSD sockets (Linux + Apple, one file). HTTP + WebSocket: WinHTTP, libcurl, NSURLSession |
| Carrottus Audio | `craudio.h` | `craudio_` / `CR_RESULT_SAUD_*` | WASAPI (Windows), ALSA (Linux), CoreAudio (macOS), RemoteIO (iOS and iPadOS: `craudioremoteio.c` and the session half `craudiosession.m`, since 2026-11-02); an argument-checking stub where no API is compiled in (watchOS) |
| Carrottus Input | `crinput.h` | `crinput_` / `CR_RESULT_CRINPUT_*` | GameController (macOS, iOS, iPadOS), evdev (Linux), GameInput (Windows). Not written: XInput (Windows); with no API built in, no instance can be made |
| Carrottus Core | `crcore.h` | `cr_` / `CR_RESULT` | platform-agnostic |
| Carrottus Perf | `crperf.h` | `crperf_` | Windows, Linux, Apple |
| Carrottus Random | `crrandom.h` | `crrandom_` | `BCryptGenRandom` (Windows), `getrandom` (Linux), `getentropy` (Apple). The operating system's secure random bytes, one call, no state: Carrottus has no generator of its own. Added 2026-10-31; macOS compiled and run (`crrandom_test`). Run on Windows the same day (Windows 11, `windows-d3d12` and `windows-vulkan`, Debug, Release and `-asan`): its first compile there failed, `bcrypt.h` included before the `windows.h` it needs; fixed, `crrandom_test` passes. Linux written, not compiled |

Carrottus GPU dominates the codebase (133 functions across 24 opaque handles) and is where
most work happens. The others are small, focused translation shims — don't let their
API surface grow beyond what they are: Carrottus Net is "move bytes," not a networking
framework (message framing, snapshot/delta semantics, host authority are all
the caller's problem). All three parts share one shape, set out at the
top of `crnet.h` - `crnet_<object>_<verb>` names, `_create`/`_destroy`, desc
structs with no defaults, one meaning for `timeout_ms` (`CRNET_NO_WAIT` stands in
for a non-blocking mode), `_abort` from any thread, and `CR_RESULT_CRNET_*`
results - so a new call follows it rather than the platform API it wraps. TCP
addresses are numeric; name lookup is its own object (`crnet_resolver`) because
the system's lookup can't be time-limited or interrupted: `crnet_resolver_run`
blocks in it on a thread the caller supplies, `crnet_resolver_wait` waits for
the answer on another. Carrottus Net itself starts no thread, anywhere. The HTTP and WebSocket clients follow the same
rule one level up: they translate the platform's own client stack (TLS, proxies and
framing are the stack's) and carry one request or one message, with every call
blocking on the caller's thread. HTTP is a streaming transfer
(`crnet_http_transfer`: start, send the body in pieces, receive the response,
recv its body in pieces) - nothing of the caller's is buffered in Carrottus Net, and
there is no one-shot request to add back. Each backend keeps the caller's time limit itself,
as the whole call's allowance, with the stack's own timers switched off - don't hand
a limit to WinHTTP/NSURLSession/libcurl, they each mean something different by it -
and each blocking call can be ended from another thread
(`crnet_http_transfer_abort`, `crnet_websocket_abort`). Which server
certificates are accepted is the caller's, in a `crnet_tls_desc` (trust mode,
DER roots, host-name switch, SHA-256 public-key pins) given per HTTP client and
per WebSocket connect; a zeroed one is the stack's own check. A failure's
`CR_RESULT` is portable and the platform's own number behind it is not, so
each object keeps the latter in a `crnet_error` for its `_get_error` - for
logs, never for deciding what to do - and a backend records it at the point it
returns the failure (`crnet_error_record`). Waiting on several objects at
once is a `crnet_wait_set` (`src/net/crnetwaitset.c`, one `poll()`-based file
for every platform): it only says which objects are worth calling, and the
caller still makes the ordinary calls with `CRNET_NO_WAIT`. No retry, no
reconnect, no cookies, no keep-alive pings, and nothing that knows what a status
code or a message means. What is the same on every stack is written once: the
HTTP transfer's order of use, checks and header storage in
`src/net/crnethttp.c`, with each stack behind the `crnet_http_backend_*`
functions of `src/net/crnetshared.h`; a new rule about a transfer goes in the
front end, not in three backends. Parts a platform doesn't allow are compiled
out by `CRNET_EXT_TCP` / `CRNET_EXT_WEBSOCKET` (both 0 on watchOS) - test
those, never a platform's name. Carrottus Memory is
the platform's virtual memory a page at a time (reserve, commit, decommit,
release), not an allocator: the arena it had decided alignment, growth and
zeroing for its caller and went on 2026-10-22 (`MATURITY_TODO.md`, item 3).
Nothing is rounded - a size or address that isn't a page multiple is
refused - and a committed page reads zero on every backend, after a decommit
too (POSIX decommits by mapping fresh pages over the range, since Apple's
`MADV_DONTNEED` keeps contents). The checks are in the front end,
`src/crmemory.c`, and each backend only implements
`src/memory/crmemoryshared.h`.
Carrottus Audio is "move PCM frames to and from an endpoint," not an audio engine: no
mixer, decoder, resampler, voices, spatialisation or volume policy (those are
the caller's). It never picks a device - the OS defaults
are reported as a property - and never silently converts a format: a stream the
endpoint can't take fails with `CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED` unless the
caller set `CRAUDIO_STREAM_FLAG_ALLOW_OS_CONVERSION`. Which native APIs are built
in is decided in `CMakeLists.txt` and published as PUBLIC
`CRAUDIO_PLATFORM_*` / `CRAUDIO_COMPILE_*` definitions, so a caller tests those
at compile time; the table is at the top of `craudio.h`.
Carrottus Input is "what devices are connected and what state they are in," read with
no window: gamepads only so far. Nothing that arrives as a window's event (keys,
the pointer, typed text) belongs in it, since Carrottus owns no window. It never
picks a device or numbers players, applies no dead zone or curve, lists only
what the platform itself presents as a gamepad (no table of other devices'
layouts), and starts no thread: the caller polls. A gamepad is named by a
`crinput_gamepad_id` value, not a handle, so that one unplugged is told apart
from one still there. What a gamepad has beyond buttons and axes (rumble motors,
battery, motion sensors, touch surfaces, lights, labels) is a per-device feature
bit, and asking for one it lacks is `CR_RESULT_INPUT_FEATURE_NOT_PRESENT`,
never a silent no-op. Outputs are set and held, with no durations or patterns;
motion is every sample between two polls, raw, in one stated frame, with no
orientation worked out; labels are only the platform's own words, and no table
of devices or button art lives here. The public API is one front end,
`src/crinput.c`: every argument check and the table of gamepads and ids are
there, once, and a backend only finds devices and reads them into the front
end's records through the verbs listed in `src/input/crinputshared.h`. A
new rule about a call goes in the front end, not in each backend. Any number
of native APIs may be built into one build (GameInput and XInput on Windows,
once XInput is written), and the caller's `native_api` chooses between them,
so the front end dispatches on it with one `switch` per verb, never a
function-pointer table. The backends are part of the front end's translation
unit: `src/crinput.c` includes each one built in (CMake compiles none of
them alone, and the front end is compiled as Objective-C where GameController
is in), and each defines its verbs as `crinput_<api>_<verb>` with
`CRINPUT_BACKEND_FUNCTION`, static and forced inline into its case. A
dispatched call is then a compare and a branch, with no call. A function the
compiler can't inline fails the build (always_inline on clang and GCC, C4714
as an error on MSVC), except that MSVC inlines nothing at `/Ob0`, its Debug
default. On 2026-10-20 the macOS Debug and Release objects held no
`crinput_gamecontroller_*` function, only the blocks inside them; the
GameInput and evdev builds were not run.

## Carrottus GPU conventions

- **Opaque handles only.** Every resource type (`crgpu_instance`, `crgpu_device`,
  `crgpu_buffer`, `crgpu_command_list`, ...) is a forward-declared pointer with no
  caller-visible fields. Backend-private struct layout lives in an internal header per
  backend (`crgpuvulkan.h`, `crgpud3d12.hpp`) — never in the public `crgpu.h`.
- **Dynamic rendering, not render-pass objects, as the default path.** The renderpass
  concept was removed in favor of `crgpu_command_list_begin_rendering`/`cmd_end_rendering`
  (commit `e90f921`). Don't reintroduce a `VkRenderPass`/`VkFramebuffer`-style object
  into the public API as the default — D3D12's backend already emulates this begin/end
  shape on top of its native render-target-view binding, not the other way around.
  **Designed, not built:** no `CRGPU_LEGACY_*` macro, `crgpu_framebuffer` or
  `crgpu_command_list_begin_rendering_legacy` exists in the code yet; the rest of this
  bullet is the plan for when one is needed.
  If/when Carrottus targets hardware that lacks Vulkan 1.3/`VK_KHR_dynamic_rendering`
  (older/low-end Android, Wear OS smartwatches), the classic `VkRenderPass`/
  `VkFramebuffer` fallback this implies would be reached only through `CRGPU_LEGACY_*`
  (see below) — `crgpu_command_list_begin_rendering`/`crgpu_rendering_begin_desc` keep an
  identical calling convention on both paths, but the fallback itself would be exposed as a
  small opaque `crgpu_framebuffer` handle (`CRGPU_LEGACY_FRAMEBUFFER`-gated,
  paired with `crgpu_command_list_begin_rendering_legacy`) rather than hidden entirely
  inside the Vulkan backend. That object needs a real owner for its lifetime
  (creation, caching, invalidation on resize) — a private Carrottus-side cache keyed on
  attachment sets would be exactly the hidden global state/hidden decision the
  zero-policy rule above forbids. The owner is the caller: it holds raw image views
  on modern hardware or a `crgpu_framebuffer` it created and caches on legacy
  hardware, and makes the same begin/end call either way.
- **`CRGPU_EXT_*` gates a capability gap between backends; `CRGPU_LEGACY_*`
  (designed, not built) gates one extra caller-owned object for constrained hardware
  within a backend.**
  Both are compile-time macros in `crgpu.h`, computed once and gated on everywhere
  else — never scatter a raw `#if !CRGPU_COMPILE_<BACKEND>` check across call
  sites; if a second backend later lacks the same thing, that should be a one-line
  edit to the macro's own definition, not an audit of every use site. They answer
  different questions and must not be conflated:
  - `CRGPU_EXT_<NAME>` gates a capability that some `GRAPHICS_BACKEND` choices
    don't implement at all (bindless/descriptor indexing is the reference example —
    Metal needs a `MTLHeap`-backed allocator it doesn't have yet). Define it `1` only
    when a backend that implements it is the one compiled in
    (`CRGPU_COMPILE_VULKAN_API || CRGPU_COMPILE_D3D12_API`-style), `0` otherwise,
    and wrap the entire public function/type group in `#if CRGPU_EXT_<NAME>` so
    calling it against a build that lacks it is a compile/link error, not a silent
    no-op or a runtime NULL surprise discovered on the wrong platform. Pair every
    `CRGPU_EXT_<NAME>` with a `CR_RESULT_GPU_EXT_<NAME>_NOT_SUPPORTED` in
    `crcore.h`, returned when the backend compiles the extension in but the
    specific device/driver still doesn't support it — a distinct, later-discovered
    case from the macro itself being `0`. Two exceptions:
    `CR_RESULT_GPU_EXT_DEPTH_BOUNDS_TEST_NOT_SUPPORTED` exists and is returned
    nowhere, since a pipeline's `depth_bounds_test_enable` is masked on a device
    without the feature (the pipeline behaves as if it is false, as `crgpu.h`
    states); and `CRGPU_EXT_BUNDLES` has no paired result, since every GPU of
    a backend that implements bundles records them.
    `CRGPU_EXT_DESCRIPTOR_SETS`/`CRGPU_EXT_SUBPASS_MERGING` (see the OpenGL note
    below) are the next likely candidates whenever their real API surface gets
    built. An `EXT` has to stay a narrow, self-contained island (a handful of
    types/functions) — if gating a capability would mean wrapping most of
    `crgpu_command_list_*` or another load-bearing chunk of the header, that's a sign the
    backend needs its own `GRAPHICS_BACKEND` entry instead of an `EXT` flag draped
    over nearly everything.
  - **An `EXT` is named for its capability, never for a platform or an API**
    (card S15, decided 2026-10-31 with the user): `CRGPU_EXT_BUNDLES`, not
    `D3D12_BUNDLES`, even where one backend is the only one that has it
    today. Another backend that gains the capability turns the macro to 1,
    with no rename and no caller change. So S12's Apple ray tracing sibling
    is `CRGPU_EXT_RAY_FUNCTION_TABLES`, and tile shading and framebuffer
    fetch, if built (card S16), `CRGPU_EXT_TILE_SHADING` and
    `CRGPU_EXT_FRAMEBUFFER_FETCH`.
  - `CRGPU_EXT_MESH_SHADING` (`CrGPUMeshShaders` sample) is a second, distinct
    flavor of the same pattern: the macro itself is `1` on **every** backend — mesh
    shading has no structural per-backend gap the way bindless does — so the compile
    flag alone doesn't do the interesting work here. What still needs the full
    `EXT` treatment is *runtime* hardware/driver support, which genuinely varies even
    though the backend code exists on all three: Vulkan probes
    `VK_EXT_mesh_shader`'s presence and `VkPhysicalDeviceMeshShaderFeaturesEXT`
    when the instance lists its adapters (`crgpuvulkan___read_adapter_facts`,
    `crgpuvulkancontext.c`, the first optional device extension this backend
    has ever had to enumerate rather than assume) and enables it at device
    creation only for a device created with the feature, D3D12
    checks `D3D12_FEATURE_DATA_D3D12_OPTIONS7::MeshShaderTier`
    (`crgpud3d12context.cpp`), and only Metal's is unconditionally true (Metal 3
    mesh shading is guaranteed on every Apple Silicon target this backend already
    requires). `CRGPU_FEATURE_MESH_SHADING` in an adapter's
    `supported_features`, and in the device's features when the device was
    created with it, surface that per-GPU fact to the caller, and
    `CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED` refuses a mesh pipeline on a
    device created without it — don't assume `CRGPU_EXT_MESH_SHADING == 1`
    means a given device can actually draw with it.
  - `CRGPU_EXT_RAY_QUERY` (card S12, built 2026-10-31 from the approved
    `crgpu_raytracing_draft.h`, R1-R8) is the mesh shading flavour again:
    1 on all three backends, with `CRGPU_FEATURE_RAY_QUERY` per adapter
    (Vulkan `VK_KHR_acceleration_structure`, `VK_KHR_ray_query` and
    `VK_KHR_deferred_host_operations` listed, `accelerationStructure` and
    `rayQuery` on, all enabled only for a device created with the bit;
    D3D12 `OPTIONS5::RaytracingTier` 1.1 or later; Metal
    `supportsRaytracing` and `supportsRaytracingFromRender` on Apple7 or
    later, macOS 14 / iOS 17 or later) and
    `CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED` from every call of the
    section on a device created without it. Acceleration structures are a
    handle (`crgpu_acceleration_structure`, committed or placed in a heap
    of `CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES`), sized by
    `crgpu_device_get_acceleration_structure_build_sizes()`, built and updated
    many to a call (`crgpu_command_list_build_acceleration_structures()`, nothing
    recorded on a refusal), copied, compacted (the size written into the
    caller's buffer) and ordered by a barrier call of their own. Carrottus GPU
    keeps no scratch, pool or cache and sets no build flag. Instances are in
    the device's own layout (card S14): a query
    (`crgpu_device_get_acceleration_structure_instance_layout()`: 64 bytes row
    major on Vulkan and D3D12, Metal's 72-byte indirect descriptor) and a
    stateless CPU write of many at once; a GPU writer is the caller's, one
    per layout kind. Metal makes resident the bottom levels a top level's
    last build named (R5) wherever it is built or its set bound. What every
    backend checks the same way is in `src/gpu/crgpushared.c`; each
    backend's translation is its own `*raytracing` file. Ray tracing
    *pipelines* (`CRGPU_EXT_RAY_TRACING_PIPELINE`,
    `CRGPU_EXT_RAY_FUNCTION_TABLES`) are not built (card S12). Settled
    while building (cards S23-S26, 2026-10-31): the heap class and the
    descriptor type are refused with the extension's result on a device
    without the feature; `placement_alignment` is 256 on Vulkan and 64 KiB
    on D3D12, and on Metal Metal's own (1024 on an Apple M5 Pro), and a
    placed structure the API would make larger than that rounding is
    refused; a SCRATCH to SCRATCH barrier orders two builds sharing a
    scratch range (D3D12 a UAV barrier between any two states that are both
    its `UNORDERED_ACCESS`); a build, copy or size write is refused on a
    copy list or bundle (`CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE`,
    368) and inside a rendering pass (`..._INSIDE_RENDERING`, 369), and
    leaves the compute bindings undefined until bound again. An update is
    taken only of exactly its source's last layout, kept on the handle as
    values (an array grown at a bottom level's first update-allowing build
    or copy), and a build call's checks make all the room it needs, so its
    recording can't fail.
  - Since 2026-10-21 (`CRGPU_DEVICE_TODO.md`): an `EXT` macro says what the
    backend in this build implements and nothing about any GPU, and every
    one's comment says so. Where support also varies by GPU it pairs with a
    `CRGPU_FEATURE_<NAME>` bit, and its `CR_RESULT_GPU_EXT_<NAME>_NOT_SUPPORTED`
    means "the device wasn't created with the feature" (no depth bounds result
    is returned, and bundles have no bit or result; see above). The bits are always
    defined, never inside an `#if`; a bit whose macro is 0 is never in any
    adapter's supported features. Bindless is `CRGPU_EXT_BINDLESS` /
    `CRGPU_FEATURE_BINDLESS` / `CR_RESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED`
    (it was `..._BINDLESS_DESCRIPTOR_INDEXING`).
  - `CRGPU_LEGACY_<NAME>` gates a fallback for hardware that some devices *within*
    a single backend's target range lack, even though every *currently* targeted
    device of that backend has it (dynamic rendering on old/low-end Vulkan hardware
    is the motivating case). This is a runtime device fact, not a `GRAPHICS_BACKEND`
    choice — the same compiled Vulkan backend has to run on both old and new
    hardware — so `CRGPU_LEGACY_<NAME>` must gate an opt-in build configuration for
    targeting that constrained hardware profile (e.g. a dedicated CMake option),
    never `GRAPHICS_BACKEND` itself. Which path a given device actually needs is
    surfaced to the caller as a feature bit of the adapter (as
    `CRGPU_FEATURE_BINDLESS` is), never resolved silently inside
    Carrottus — the caller decides which path to take per-device; Carrottus only reports
    the fact and does the mechanical object construction for whichever path is
    chosen. `CRGPU_LEGACY_<NAME>` may grow the public surface by exactly one
    narrow, opaque resource type when the fallback needs a persistent object with
    real lifetime (`CRGPU_LEGACY_FRAMEBUFFER` above) — that's still fundamentally
    different from `EXT`: the caller isn't adapting its behavior, it's just holding
    the handle Carrottus needs to do the identical job on older hardware.
- **OpenGL/OpenGL ES, if ever needed, is a fourth `GRAPHICS_BACKEND`
  (`CRGPU_COMPILE_OPENGL_API`), not reached through `CRGPU_LEGACY_*`.** Its gap
  from Vulkan/D3D12/Metal isn't one narrow fallback object — no true deferred
  command-list recording, no descriptor-set object, different synchronization
  primitives — which is a structural, broad difference, not a hardware-SKU fact
  within an existing backend. Express it the same way Metal's missing bindless is
  expressed today: a pile of `CRGPU_EXT_<NAME>` macros going to `0` for it
  (`CRGPU_EXT_DESCRIPTOR_SETS` is the clearest candidate), computed alongside
  Vulkan/D3D12/Metal as a genuine fourth backend choice — never squeezed under
  `CRGPU_LEGACY_*`'s opt-in constrained-hardware path, which exists for one small
  caller-owned object, not a structurally different API.
- **Backend file split mirrors the header, not guessed.** Each backend (Vulkan, D3D12,
  Metal) uses the same twelve-file-per-concern layout: `context`, `buffer`, `heap`,
  `image`, `shader`, `swapchain`, `descriptors`, `command`, `renderpass`, `native`,
  `sync`, `query` (timestamp sets, added 2026-10-15). If `crgpu.h` grows a new
  concern, split a new file for it in every backend rather than dumping new
  functions into an existing one. What every backend checks the same way is
  written once in `src/gpu/crgpushared.c` (`crgpushared.h`), as Carrottus Net and
  Carrottus Input keep their front ends: the pipeline's colour-attachment and depth-bias
  checks are there.
- **A fence is a counter that only goes up, on every backend.** A submission
  signals it to a value the caller chooses
  (`crgpu_submit_desc::signal_fence_value`), `crgpu_fence_wait` waits
  for a value per fence, and there is no reset: a fence is reused by
  signaling it higher. That is `ID3D12Fence` and `MTLSharedEvent` as they
  are, and a timeline `VkSemaphore` on Vulkan - not a `VkFence`, which is one
  bit that has to be reset by hand and would make the same call behave
  differently per backend. Carrottus GPU keeps no counter of its own for a caller's
  fence; don't add one. The swap chain's own per-frame fences are internal to
  each backend (plain `VkFence`s on Vulkan, since acquire can't take a
  timeline semaphore) and are not `crgpu_fence`s: a submission reaches them
  through `crgpu_submit_desc::swap_chain`, which is also all that
  `crgpu_command_queue_submit_lists_synced` is. Written 2026-10-10, compiled
  for Metal only. `crgpu_semaphore` is still binary on Vulkan and a hidden
  counter on D3D12 and Metal.
- **One list of an allocator is open at a time, on every backend.** D3D12
  refuses a second (`COMMAND_ALLOCATOR_CONTENTION`, and the list stays
  closed); Vulkan and Metal would allow it, so `crgpu.h` states D3D12's
  rule for all three. A debug build keeps the open list in the allocator's
  `_open_list` and asserts in begin (bundle begin too) and in
  `crgpu_command_allocator_reset`; end and destroy clear it. Vulkan's list
  holds its allocator by value, so it keeps `_debug_allocator` for this.
  Added 2026-10-16; run on D3D12 and on Vulkan on Windows, and on Metal
  (`rbtrend_gpu_tests`, Debug), never fired.
- **A buffer's memory is one `CRGPU_MEMORY_KIND`, a contract and not a
  set of Vulkan property bits.** `DEVICE`, `UPLOAD`, `READBACK` and
  `DEVICE_MAPPABLE` each say what the CPU and the GPU may do with the buffer
  and which usages it may have (the table is on the enum in `crgpu.h`);
  each backend picks whatever memory keeps that contract, and the checks
  that enforce it are the same in all three. `CRGPU_MEMORY_KIND_NONE` is
  refused: a zeroed desc has not chosen. Don't add a kind, or a flag beside
  the kind, that only one API can tell apart (coherent, cached, write
  combined): the caller always calls `crgpu_buffer_flush` after a mapped
  write and `crgpu_buffer_invalidate` before a mapped read, and a backend
  that has nothing to do for one does the checks and returns. An image has
  no kind at all: it is in the device's memory and never mapped.
  `DEVICE_MAPPABLE` is the one kind a device may lack: it needs a device
  created with `CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY` and is refused with
  `CR_RESULT_GPU_MEMORY_KIND_NOT_SUPPORTED` otherwise; it has no `CRGPU_EXT_*`
  macro because every backend implements it. Written 2026-10-10, compiled
  for Metal only; D3D12's custom heap for it has never run.
- **A buffer or image desc carries no flag only one API acts on.**
  `CRGPU_HEAP_FLAGS` (D3D12's heap flags under another name) and
  `CRGPU_RESOURCE_FLAGS` (one D3D12 resource flag) were removed on
  2026-10-10: Vulkan read neither, Metal ignored both, and no caller set a
  bit. Don't bring a bit back onto the desc. Sharing a resource across
  processes or APIs, if it is ever needed, is an `CRGPU_EXT_*` group with
  its export and import calls, which all three APIs can back. Zeroing is
  settled by rule instead of by flag: a new buffer's or image's contents are
  undefined until written, on every backend, so the D3D12 backend always
  passes `D3D12_HEAP_FLAG_CREATE_NOT_ZEROED`.
- **A heap is one device allocation; Carrottus GPU places, the caller decides where.**
  `crgpu_heap_create` makes a heap of one `CRGPU_MEMORY_KIND` and one
  `CRGPU_HEAP_RESOURCE_CLASS` (buffers, images, attachment images);
  `crgpu_get_buffer/image_placement_requirements` ask the heap for a
  resource's size and alignment; `crgpu_buffer_create_placed/image` make an
  ordinary handle at the caller's offset, destroyed with the ordinary destroy
  call. Carrottus GPU keeps no record of what is placed where and checks no
  overlap - which heap, which offset, when to make or drop a heap are the
  caller's. Settled on 2026-10-11, don't reopen without a reason:
  - One class per heap on every backend, D3D12 tier 2 included (D3D12 tier 1
    made the rule; Vulkan never has buffers and images in one allocation).
  - The requirement queries take the heap, not the device: Vulkan folds
    `nonCoherentAtomSize` into a mappable heap's answer there, so a flush of
    one placed buffer never reaches the next.
  - Vulkan chooses a heap's memory type when it is made, from probe
    resources (`crgpuvulkanheap.c` explains which), and maps a mappable
    heap once for its life: Vulkan allows one mapping of a `VkDeviceMemory`.
  - Metal heaps are `MTLHeapTypePlacement` and hazard **tracked**, because
    this backend's barriers do nothing and rely on tracking. Metal may then
    order work on unrelated resources of one heap. Untracked heaps wait for
    real Metal barriers.
  - An image in an attachment-images heap is first written whole (a pass
    that clears it, depth and stencil both, or copies into all of it):
    D3D12's rule for placed render targets, without a discard command.
  - A debug build counts live placements per heap (atomically) and
    `crgpu_heap_destroy` asserts the count is 0.
  - D3D12 places every texture at 64 KiB alignment. Asking for 4 KiB is a
    debug layer error for a texture too big for it, and telling them apart
    needs each format's tile shape; not done.
  Written 2026-10-11. Vulkan and D3D12 type-checked on Linux against the
  Khronos and DirectX headers, and the Vulkan heap, buffer and image files
  run against a mock Vulkan; the Metal files parsed with clang against a
  stub Metal header only. Nothing has run on a GPU.
- **Native escape hatches are interop-only.** `crgpu_vulkan_natives.h` /
  `_d3d12_natives.h` / `_metal_natives.h` unwrap a handle back to its raw native type
  (`VkInstance`, `ID3D12Device14`, ...) for third-party libraries Carrottus doesn't wrap
  itself (ImGui, RenderDoc). Carrottus has no visibility into what the caller does with
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
    wait/signal across queues, which is all `crgpu_semaphore`'s public API
    ever asks for). Neither uses `MTLFence` — it can't be waited on from the
    CPU or across command queues, and belongs in `crgpu_command_list_pipeline_barrier`'s
    implementation instead, not in fence/semaphore.

## Code conventions

- **The version is written once, in `crcore.h`** (`CARROTTUS_VERSION_MAJOR`/
  `_MINOR`/`_PATCH`, since 2026-10-20, at 0.1.0). `CMakeLists.txt` reads
  those three lines for `project(VERSION)`, and `cr_get_version()` returns
  what the library was built with. One version for the whole library;
  what each part promises is the table in the header's "Version" group.
  Nothing is promised before 1.0.0, and "released" means a `vX.Y.Z` tag.
  A chained struct's comment says "Since X.Y.Z". carrottus refuses no caller
  for its version: comparing is the caller's.

- **Every struct that may grow starts with a `base`** (`CR_STRUCT_BASE`,
  or `CR_STRUCT_BASE_OUT` for one Carrottus fills in; `crcore.h`). Every
  other public struct has a `/* Frozen: ... */` line above its `typedef`.
  A released layout never changes: a new field is a new struct chained on.
  A call checks a based argument with `cr_struct_check` /
  `cr_struct_out_check` (`src/crstructbase.h`) right after its null
  check, in every backend. Carrottus GPU, Carrottus Audio, Carrottus Net and Carrottus Input are converted; the rest
  of the plan, and the rules, are in `STRUCT_BASE_TODO.md`. The workspace's
  `check.sh struct-bases` checks the headers' side: based or frozen, base
  first, no `#if` in a struct, no based struct held by value, and each type
  registered in `CR_STRUCT_TYPE` as the right kind and used in `src/`.

- **Argument checks are one `if` per distinct return.** They sit at the top of
  the function, in parameter order, before any allocation or platform call, and
  each returns the `CR_RESULT` that names exactly what was wrong:

  ```c
  if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
  if (!desc) return CR_RESULT_NULL_DESC;
  ```

  Conditions that would return different values are never joined. A joined check
  can only return one code, so the caller can't tell which argument failed, and
  Carrottus's job is to report the fact, not a summary of it.
- **Conditions may be joined with `||` when every one of them leads to the same
  return** - the same `CR_RESULT`, or the same `bool`, null or plain `return`
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
  	printf("crgpu: DXC compile failed (%ls): %s\n", profile,
  	    (const char *)errors->GetBufferPointer());
  else
  	printf("crgpu: DXC compile failed (%ls): hr=0x%08lx\n", profile, (unsigned long)hr);
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
  // crgpu_command_queue_submit_lists_synced, identical in Vulkan, D3D12 and Metal.
  if (!queue) return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
  if (!cmd_lists) return CR_RESULT_GPU_INVALID_COMMAND_LIST;
  if (cmd_list_count == 0) return CR_RESULT_ZERO_SIZE;
  if (!swap_chain) return CR_RESULT_GPU_INVALID_SWAP_CHAIN;
  ```

  This covers every kind of return, not only `CR_RESULT`: where one backend
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

  `crgpu_command_list_push_compute_constants` is the example. Vulkan keeps one push
  constant state for a command list, D3D12 keeps one for graphics and one for
  compute, and Metal has none and resends the block to an encoder. The header
  says none of that. It says:

  ```c
  /* Two rules hold on every backend, whatever the graphics API underneath
   * keeps or shares:
   *
   * - Push after crgpu_command_list_bind_compute_pipeline() and before the
   *   crgpu_command_list_dispatch() that reads the data. What a dispatch reads from a
   *   byte that has not been pushed since the compute pipeline was last bound
   *   is undefined.
   * - Graphics and compute pipelines do not have a push constant block each:
   *   after this call, what a draw reads from the block is undefined until it
   *   is pushed again with crgpu_command_list_push_graphics_constants(), and the other way
   *   round.
   */
  ```

  This is the preferred way to handle such a case. A caller that follows the
  rule gets the same result on every backend; one that leans on what a single
  API preserves is outside the contract. Reach for an `CRGPU_EXT_*` macro or
  a capability query only when a backend can't do the thing at all, not when
  the APIs merely keep different state. The fence model (a counter on every
  backend) is the same idea applied to an object.
- What may differ is what only one platform can produce: a failed platform call
  (`CR_RESULT_API_SPECIFIC_FAILURE`), or a feature a backend doesn't have,
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
  and tightly packed signatures) is made by `crgpu_device_create`.
- **All of Carrottus's own memory comes from the caller's allocator** (`CR_ALLOCATOR`,
  `crcore.h`, since 2026-10-21). A module's instance desc takes it, the
  instance keeps a copy, and every object made from the instance keeps a copy of
  its parent's in its head, so it frees through its own copy and no destroy call
  takes an allocator. Never `malloc`, `calloc`, `realloc`, `free` or `strdup`
  Carrottus's own memory, and no allocating `new` or `delete` in the C++ backends.
  The helpers are in `src/crallocator.h`: `CR_NEW`, `CR_NEW_ZEROED`,
  `CR_DELETE`, `CR_NEW_ARRAY`, `CR_NEW_ARRAY_ZEROED`, `CR_DELETE_ARRAY`,
  `CR_RESIZE_ARRAY`, `cr_string_copy`/`cr_string_free`, and
  `cr_allocate`/`cr_free` for an untyped blob. An array that grows is an
  `EQ_ARRAY` from eqstd (below), on the object's `CR_ALLOCATOR` handed
  over by `cr_eq_allocator`. The workspace's
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
  a public API), a PRIVATE dependency of `carrottus`: no public header includes
  it. The object that owns the array keeps an `EQ_ALLOCATOR` made by
  `cr_eq_allocator(&head.allocator)` (`src/crallocator.h`) beside it, since
  the array keeps a pointer to it; every eqstd call that grows returns an
  `EQ_RESULT`, and anything but `EQ_SUCCESS` becomes `CR_RESULT_OUT_OF_MEMORY`
  (or the caller's false), the array unchanged. eqstd keeps
  `CR_ALLOCATOR`'s contract (never 0 bytes, each block freed with its size
  and alignment; an array's alignment is `EQ_ARRAY_ALIGNMENT` of the
  element's size, a multiple of its alignment). Where a growth pattern
  matters (a table sized from the capacity, a count that must fit a
  `uint32_t`), the capacity is chosen here and given to `eq_array_reserve`,
  which allocates exactly that; otherwise `eq_array_push` grows by doubling
  from 4. Converted: Carrottus Net's response headers (`src/net/crnethttp.c`)
  and the wait set's members and poll entries (`src/net/crnetwaitset.c`).
  Converted 2026-10-29 and run on Windows: WASAPI's endpoints, GameInput's
  devices, and the D3D12 backend's indirect signatures and heap free ranges
  (the device keeps their `EQ_ALLOCATOR`); the D3D12 backend's
  `std::vector`s too, as `eq::array` on the device's allocator (each image's
  subresource states, the bindless free stacks, the barrier lists, the
  adapter list), so none of them allocates outside the caller's allocator or
  throws `std::bad_alloc` into a C caller any more. SPIRV-Cross's own
  `std::vector` and `std::string` stay: its API takes and returns them.
  Still `CR_RESIZE_ARRAY`, to convert when it can be compiled and tested
  again: evdev's gamepads. A size of 0 is asked for and freed as 1 by the helpers, so a
  count that may be 0 needs no case of its own. `tests/crtest_allocator.h`
  checks all of this in a test.
- **A destroyed handle is refused in Debug** (card X16, 2026-10-30;
  `src/crstale.h`). A Debug build's `cr_object_head` holds a tag:
  `CR_BIRTH` sets it where a create fills the head, `CR_KILL` writes
  it dead at every destroy and owner release, before the allocator is
  copied out, and every public call checks each handle right after its
  null check, its own `if`, refusing as the null check does
  (`if (CR_STALE(x, "call", "x")) return ...;`, one line printed), the
  same checks in the same order in every backend. Release has no tag.
  A new handle type gets a factory in
  `tests/misuse/make_misuse_tests.py`. **Since card X27 (2026-11-08) a
  Debug destroy keeps the handle's block** in its owner's quarantine (a
  list linked through the dead blocks themselves, no allocation, lock-free
  push) until the owner is destroyed, which frees each block through the
  same allocator with its size and alignment; so the tag stays dead and no
  new object is made in that memory. The owner is named at the create,
  `CR_BIRTH_IN(obj, &owner->quarantine)`, and the destroy frees with
  `CR_RETIRE` where it used `CR_DELETE` (`src/crallocator.h`; the
  owner per kind is in `src/crstale.h`): a device for what is made on
  it, the instance for devices, surfaces, files, listings, mappings and
  Carrottus Net's objects. Freed at once as before: roots, what an owner
  releases in its own destroy, a resolver, and a Carrottus Audio stream (the
  header doesn't say a stream goes before its instance). Release
  unchanged. Built and run on Windows (D3D12, Vulkan, WinHTTP, Winsock,
  files); the POSIX sockets, libcurl and URLSession halves are written,
  not compiled; the audio backends still free at once. Metal's half
  built and run on the Mac on 2026-11-11 (Apple M5 Pro): before it,
  `crgpu_objects_test`'s quarantine case failed in every Debug build
  there, a new buffer made in the dead one's block. Still
  best effort for a handle with no owner: the misuse harness keeps its
  own block-keeping allocator for those.
- **Every handle starts with a `cr_object_head`** (`src/crallocator.h`): its
  debug name, then its copy of the allocator. A handle is freed by copying the
  allocator out of its head first, since the head is inside the memory being
  freed:

  ```c
  	const CR_ALLOCATOR allocator = buffer->head.allocator;
  	cr_string_free(&allocator, buffer->head.debug_name);
  	CR_DELETE(&allocator, crgpu_buffer_vulkan, buffer);
  ```
- **Memory the platform allocates stays the platform's.** What `realpath`,
  CoreFoundation, COM, libcurl, ALSA's `snd_*_malloc` or `getaddrinfo` give is
  freed the way the platform says; Objective-C and COM reference counts are left
  alone. So is memory a graphics driver allocates: Carrottus GPU passes Vulkan no
  `VkAllocationCallbacks`, and D3D12 and Metal take none.
- **A C++ type with a constructor:** `CR_NEW`, check the pointer, then
  construct the object in place in that memory. `CR_NEW`, not
  `CR_NEW_ZEROED`, because the constructor is what initialises it. The
  in-place form needs `<new>`.

  ```cpp
  crgpu_buffer_d3d12 *object = CR_NEW(&device->head.allocator, crgpu_buffer_d3d12);
  if (!object) return CR_RESULT_OUT_OF_MEMORY;
  object = new (object) crgpu_buffer_d3d12();
  ```

  It is destroyed the same two steps in reverse: the destructor called explicitly,
  then `CR_DELETE`, with the allocator copied out first.
- **A type with no constructor:** `CR_NEW_ZEROED`, check the pointer, and that
  is all. The zeroed memory is its initial state, and nothing is constructed. It
  is destroyed with `CR_DELETE` alone.
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
  `CR_RESULT_OUT_OF_MEMORY`, where an allocating `new` would throw, and no
  exception may cross Carrottus's C boundary.
- Objects with no destroy call of their own are released by their owner: the
  adapters by `crgpu_instance_destroy`; a device's command queues and its
  bindless state by `crgpu_device_destroy`;
  descriptor sets by their pool, on reset and on destroy; a swap chain's back
  buffer images and views by the swap chain.
- What is left: `std::vector` and `std::string` (function locals in
  `crgpud3d12context.cpp`, `crgpud3d12command.cpp`, `crgpud3d12renderpass.cpp`
  and `crgpud3d12shader.cpp`, the bindless free stacks, and an image's
  `_subresource_states` in `crgpud3d12.hpp`) still allocate
  through the standard allocator, which reports failure by throwing. They
  become `eq::array` / `eq::string` on the device's allocator, and the
  `CR_RESIZE_ARRAY` arrays listed above `EQ_ARRAY`s, when those files are
  next built on their platforms (Windows, Linux); then `CR_RESIZE_ARRAY`
  goes.
- **A debug name is copied, never kept as the caller's pointer, in every
  build.** `debug_name` fields and the name member of a handle exist in
  Debug and Release alike (since 2026-10-18; before, only under
  `#if _DEBUG`, which made a desc's layout differ between the two). A
  build switch never changes a public struct's layout or which functions
  exist. Every handle that carries a name has it in its head
  (`head.debug_name`), and that member is either null or a copy Carrottus made
  through the handle's allocator. The caller may free or reuse its own string
  as soon as the call that took it returns. Storing the pointer
  (`pResult->head.debug_name = desc->debug_name;`) ties the object to memory
  Carrottus does not own.
- **`cr_debug_name_set` (`crcore.c`) is the one place a name is copied.**
  It frees the name the object had, stores a copy of the new one made through
  the head's allocator, and returns `CR_RESULT_OUT_OF_MEMORY` if the copy
  fails. A create call whose desc has a `debug_name` calls it, after the head's
  allocator is set; so does a caller renaming an object later.

  ```c
  	if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
  		crgpu_buffer_destroy(pResult);
  		return CR_RESULT_OUT_OF_MEMORY;
  	}
  	pResult->_desc.debug_name = pResult->head.debug_name;
  ```
- A null `debug_name` stays null: there is nothing to copy and it is not an
  error.
- The copy is the last step of the create call, after every native object
  exists, so a failed copy releases the finished object through its own destroy
  call and returns `CR_RESULT_OUT_OF_MEMORY`.
- A handle that also keeps the caller's desc (`_desc = *desc`) points
  `_desc.debug_name` at the handle's copy. The struct copy alone still carries
  the caller's pointer, and a `crgpu_get_*_desc` call would hand it back.
- **Because the setter frees the old name, a handle's name must be null from
  the moment the handle exists.** Allocate with `CR_NEW_ZEROED`, or copy from
  a zeroed struct, or value-initialise (`new (p) T()`); a handle from a bare
  `CR_NEW` sets it to null itself.
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
crgpu.h says it does. Every sample in `crgpusamples` but mesh shaders
ran there too. On 2026-11-04 on the ThinkPad P1 (Fedora 44, Mesa 26.0.3:
ANV, NVK on the RTX A2000, llvmpipe) the presets ran under `-asan` and
`-tsan` with GCC 16.2.1 and again with clang 22.1.8, 15 ctests each and
`crnet_test` 341/341, `cr_misuse_tests` ten times under `-asan`, and
callers' GPU tests and the samples under both on every adapter: no report
in Carrottus. craudio_test gained a TSan suppression for ALSA's own ring
buffer under PipeWire, and a second name for the plugin's lock entry
(`tests/sanitizers/linux-tsan.supp`; `../DECISIONS.md` card S37). That covers the calls that caller makes. A call nothing above
Carrottus uses is only as verified as its sample in `crgpusamples`. Vulkan
on Windows has run `rabbittustests`' `rbtrend_gpu_tests` (2026-10-16, Intel Iris
Xe and NVIDIA RTX A2000, validation layer clean), which found that the
Vulkan backend passed a descriptor write's image layout and a descriptor set
layout binding's stage flags to Vulkan untranslated; both are Carrottus GPU enum
values and now go through `cr_layout_to_vk` and
`crgpuvulkan___shader_stages`. Metal ran the same tests on macOS the
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
at every `crgpu_command_list_bind_compute_pipeline`, which validation refuses as
redundant; it is set only when it differs from the encoder's now
(`_encoder_compute_pipeline_state`), as the render encoder's state already
was. The same day `rbtrend_gpu_tests` ran on Vulkan under synchronization
validation (`VK_LAYER_VALIDATE_SYNC=1`, both devices), which found 15
hazards per device in the Vulkan backend's image barriers: `layout_to_access`
gave colour and depth attachments write access only (a pass loading its
target read it unordered after the transition), and `layout_to_stage` gave
depth the early fragment tests only (depth is also written at the late
ones). Attachments are read and written now, depth at both stages, and
`crgpu_command_list_pipeline_barrier`'s RENDER_TARGET and DEPTH_WRITE states read
and write too; rerun, no message on either device. Also the same day:
the D3D12 debug layer's messages reached only an attached debugger, so a
plain Debug run showed none. A Debug build now registers an
`ID3D12InfoQueue1` message callback per device
(`crgpu_d3d12_debug_message`, `crgpud3d12context.cpp`) that prints
corruptions, errors and warnings to stdout as `D3D12 <SEVERITY>: [ id ]
text`, as Vulkan's validation layer does with no messenger; nothing is
registered when the layer is off or the runtime predates
`ID3D12InfoQueue1`. Run without a debugger on both devices: silent on the
tests as they are, and a mismatched clear value put in for the test
printed its 6 warnings on each. On 2026-10-22 every `crgpusamples` sample
ran on D3D12 (both devices and WARP, GPU-based validation too) and Vulkan
(both devices, under synchronization and GPU-assisted validation as well),
which found two Vulkan bugs: an image barrier out of `UNDEFINED` or
`PRESENT_SRC` (`CRGPU_RESOURCE_STATE_PRESENT`/`_COMMON`) started at
`TOP_OF_PIPE`/`BOTTOM_OF_PIPE`, not chained to the acquire semaphore's wait,
and now starts at its destination stage; and `drawIndirectFirstInstance` was
never enabled, so an indirect draw's non-zero `first_instance` was outside
Vulkan's rules. That is `CRGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE` now
(every D3D12 and Metal adapter; the Metal line was compiled on macOS on
2026-10-22, where carrottus's tests pass over it, plain and under ASan and
TSan, though no test there reads the bit).
Rerun, every sample clean in every mode. The same day a caller's fragment
shader that read only locations 1 and 2 of its vertex shader's outputs
failed to link on D3D12 (debug layer error 660), which matches the stages
by register in order, while Vulkan and Metal match by location and took it;
`crgpu_graphics_pipeline_create`'s comment now states the rule that holds
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
made on a device without `CRGPU_FEATURE_MESH_SHADING` (its SPIR-V needs
`VK_EXT_mesh_shader` enabled even as a module), so
`crgpu_shader_module_create` now refuses one with
`CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED`, on every backend, after
the alignment check; and a vertex shader's `gl_Layer` in SPIR-V before 1.5
(glslc's default) is the `ShaderViewportIndexLayerEXT` capability, which
needs `VK_EXT_shader_viewport_index_layer` and not only
`shaderOutputLayer`. A Vulkan adapter has `CRGPU_FEATURE_LAYERED_RENDERING`
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
presets (MSVC's AddressSanitizer, Debug); `crnet_test` passed all 343
checks, Debug, Release and ASan. `crgpu_device_test` and
`crgpu_objects_test` ran on adapter 0 only (the Iris Xe; the tests take
the first adapter), debug layer and validation layer silent. A caller's GPU
tests ran on all three D3D12 adapters (debug layer, and GPU-based validation
once) and both Vulkan devices (validation, synchronization and GPU-assisted
validation): silent but GPU-based validation's `[942]` on the Iris Xe,
which `../claude_made_decisions.md` (2026-10-29) takes as the validator's
(a buffer barrier from COMMON after an ExecuteCommandLists, where D3D12
decays buffers to COMMON; clean on the A2000 and WARP). Every
`crgpusamples` sample ran on the three D3D12 adapters (debug layer, and
GPU-based validation) and both Vulkan devices (all three validation modes),
clean; `CrGPUMeshShaders` exits 1 on the Iris Xe, which has no mesh
shading. The run found one thing here: on Windows `cr_system_allocate`
handed a size no rounding can hold to `_aligned_malloc` and relied on the
CRT to refuse it, which AddressSanitizer turns into an abort; the refusal
is now Carrottus's, on every platform (`crcore_test` under ASan is its
test). The D3D12 backend's arrays then moved to eqstd (below) and all of
the above ran again, as clean.

On 2026-10-30 the same machine ran 2026-10-30's work: both presets
built clean, Debug and Release; every ctest passed on both backends (14
since that day: `cr_misuse_tests`, every public function with zeroes and,
in Debug, with each handle destroyed, and `cr_noalloc_tests`, which holds
Carrottus GPU's submit and present loop, Carrottus Audio's callback, Carrottus Net's idle
connection and Carrottus Input's poll to no allocation of Carrottus's own), on both
adapters, Vulkan under validation; `crnet_test` 343 of 343. Found and
fixed that day: D3D12 never defined `crgpu_surface_create_from_callback`
or the two swap chain semaphore getters, so a caller naming one didn't link
there (they now refuse, as the header says); a recording call and a
submission allocated their converted arrays every time (now kept on the
command list or queue: one allocation a submit on D3D12, six on Vulkan,
before); Vulkan's swap chain desc reported the caller's image count, not
its own (card S13, decided 2026-11-12: the caller's count, refused outside
the surface's range, with a query; `CRGPU_DEVICE_TODO.md`, not built). The
75 Carrottus GPU calls no test made are made by `crgpu_objects_test` since that
day (a drawn grid read back, every draw kind, placed resources, compute, a
swap chain on a hidden window, the surface callback on Vulkan), on Windows
only; Metal, ALSA, CoreAudio, curl, POSIX, evdev, GameController and
URLSession files touched by X16 were written there, not compiled.

On 2026-10-30 the Mac (Apple M5 Pro, macOS 27) compiled and ran them:
`macos-metal` Debug and Release, `-asan` and `-tsan`, every ctest passing
in all four (14), `crnet_test` 343 of 343 plain, Release and ASan,
`cr_misuse_tests` ten runs in Debug (206 calls with a destroyed handle,
no failure), and `crgpu_objects_test` and `cr_noalloc_tests` under
Metal API and shader validation with warnings fatal. Found and fixed:
- `crgpu_objects_test`'s mesh shader drew nothing on Metal: Metal's
  `setDepthTestMinBound:maxBound:` is encoder state with no pipeline
  switch, so the bounds set for the depth-bounded draw before it clipped
  the mesh pipeline, created without the test. `crgpu.h` stated the
  difference per backend ("on Metal this call alone turns the test on");
  it now states one rule (set the range after binding a pipeline with
  `depth_bounds_test_enable`; a bind leaves it undefined; a pipeline
  without the test is never tested), and Metal keeps it: a bind of a
  pipeline without the test sets the encoder's bounds to [0, 1], and
  `crgpu_command_list_set_depth_bounds` sets them only while a pipeline with it
  is bound (`_encoder_depth_bounds`). Vulkan and D3D12 kept it already.
- Metal's validation, with warnings fatal, refused the same vertex buffer
  set again between two draws, and a descriptor set's argument buffer set
  again for a second pipeline in a pass (the compute side was fixed so on
  2026-10-24): the render encoder now keeps each vertex buffer slot's
  buffer and offset (`setVertexBufferOffset:` where only the offset
  changes) and each stage's argument buffer per set, and sets only what
  changes.
- `crgpu_command_list_begin_rendering` made a new `MTLRenderPassDescriptor` (and
  its attachment descriptors) every pass: 5 of Metal's 10 allocations of
  an offscreen frame. The list keeps one (`_pass_desc_mtl`), every field
  set or cleared at each begin and its textures cleared once the encoder
  is made: 6 a frame now, the command buffer's and encoders' own.
- The iOS Carrottus Audio stub (`craudiostub.c`) didn't compile: X16's
  `CR_STALE` without `crallocator.h`.
- `tests/alloctrace.h` counted nothing in Release on macOS: clang takes
  `malloc` to read no global of the program's, so the store of
  `malloc_logger` before the counted code looked dead and was dropped.
  Both stores are volatile now; a planted malloc and calloc count 2 of 2
  in Debug and Release. Its table holds 16,384 stacks (1,024 before),
  since Metal's debug layer fills 1,024 in a frame and what has no room
  is counted as ours.
- `cr_noalloc_tests` asked Core Audio for `CRAUDIO_THREAD_PRIORITY_NORMAL`,
  which it refuses, so its Carrottus Audio scenario was skipped; it asks for
  `PLATFORM` there, as the other tests do, and the callback runs.
`cr_noalloc_tests` then: none of ours in Carrottus GPU's offscreen frame (6 a
frame of Metal's: command buffer, encoder, residency), its present loop
(about 24 a frame of Metal's and Core Animation's, most under
`nextDrawable`), Carrottus Audio's Core Audio callback (none at all), Carrottus Net's
idle round (none) and Carrottus Input's poll (2 of GameController's,
`[GCController controllers]`).

On 2026-10-31 Carrottus GPU took each API's own shader format and stopped
compiling (`crgpu_shaderformat_draft.h`; cards A9, S17-S22): run on
Windows 11 on the Iris Xe and the RTX A2000 over D3D12 and Vulkan (and WARP
by a caller's tests), Debug, Release and `-asan`, 15 ctests passing,
`cr_misuse_tests` ten times on each backend, `cr_noalloc_tests` none of
ours, `crgpu_objects_test` on both GPUs with the debug layer and
validation silent; clip space +Y up (Vulkan by negative viewport heights),
read back by the objects test and seen in every `crgpusamples` sample;
`shaderDrawParameters` in Vulkan's base set. The Metal backend's module
path (a `.metallib`, the entry point by name and function type) is written,
not compiled.

On 2026-10-31 `CRGPU_EXT_RAY_QUERY` was built (card S12, step 1; the
approved `crgpu_raytracing_draft.h`) and run on Windows 11 (ThinkPad P1
Gen 5): `windows-d3d12` and `windows-vulkan`, Debug and Release, 15
ctests each, and both `-asan` presets (VsDevCmd), 15 each; the NVIDIA RTX
A2000 builds, refits, compacts, clones and traces in
`crgpu_objects_test` (`--adapter 1`) on both backends, every check
passing, under the D3D12 debug layer and GPU-based validation and under
Vulkan validation, synchronization validation and GPU-assisted
validation, all silent (GPU-AV's notices of its own settings aside); the
Intel Iris Xe has no `CRGPU_FEATURE_RAY_QUERY` on either API and every
call is refused on it, and on every adapter's first device; WARP (D3D12)
reports raytracing tier 1.1, and the whole test passes on it (its update
scratch is 0 bytes). `cr_noalloc_tests` holds the refit-and-trace frame
to no allocation of ours on the A2000 (both backends) and WARP;
`cr_misuse_tests` ten runs a backend (269 calls with zeroes, 242 with a
destroyed handle, no failure). Metal's file (`crgpumetalraytracing.m`)
and the Metal halves of the context, heap and descriptor changes are
written, not compiled. Linux not built. ThreadSanitizer has not seen it.

On 2026-11-01 the Mac (Apple M5 Pro, macOS 27.0.1, Xcode 27.2, Slang
2026.18.3-2) compiled and ran both: the native-format path (a `.metallib`,
the entry point by name) and `CRGPU_EXT_RAY_QUERY` compiled at once, with
no warning. `macos-metal` Debug and Release under Metal API and shader
validation with warnings fatal, and `-asan` and `-tsan`: 15 ctests each,
passing; `cr_misuse_tests` ten times (258 calls with zeroes, 217 with a
destroyed handle, no failure); `crgpu_objects_test` whole, ray queries
included (`CRGPU_DEVICE_TODO.md`, "Hardware ray tracing", has what);
both geometry and tessellation EXTs 0 and no adapter with either bit.
`iossimulator-metal` built and its tests run in an iPhone 17 Pro simulator
(iOS 26.5), all passing; Carrottus GPU's skip there, since the simulator's GPU
has no Metal 3 family. Changed:
- Metal's ray tracing file: `placement_alignment` from Metal's answer (1024;
  S24), the 72-byte instance asserted field by field, residency one
  `useResources:count:usage:` per encoder from a list of Metal objects kept
  on the top level, the build descriptors kept on the command list.
- The render encoder keeps its viewports and scissors and sets them only
  when they differ (validation refused a second equal one).
- Push constants go only to the stages whose function reads them, from the
  pipeline's reflection (validation refused one no argument takes).
- `tools/crgpu_msl` builds for the build machine in a cross-compiling
  build too (ExternalProject, `tools/crgpu_msl/CMakeLists.txt`), every
  shader step runs `${CRGPU_MSL_EXECUTABLE}` and names the build's own
  SDK and deployment target, and the tool tells SPIRV-Cross that iOS GPUs
  have base vertex and instance.
- Carrottus Random on Apple is `SecRandomCopyBytes` (the iOS SDKs declare no
  `getentropy`); `cr_noalloc_tests` links as C++ for its demangler.
`cr_noalloc_tests`: none of ours, the refit-and-trace frame included (41
a frame of Metal's own there). Not changed and recorded as a known gap
since 2026-10-17, then card S27: Slang keeps only the bindings an entry
point reads, and the argument buffer the backend writes from a set's
layout doesn't match a module's that leaves bindings out. Decided
2026-11-01 (a): the build pads, Metal refuses a mismatch; draft
`crgpu_shaderbindings_draft.h`, approved the same day, built the same
evening on the Mac (Known gaps below, `CRGPU_DEVICE_TODO.md`).

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
  on real hardware (Apple M5 Pro) via `crgpusamples/Samples/HelloTriangle`; the
  natives-header asymmetries above were worked out ahead of the implementation and
  held. Buffer/image copy (`crgpu_command_list_copy_buffer`/`_copy_buffer_to_image`/
  `_copy_image_to_buffer`, `crgpu_image_get_buffer_copy_size`) is also real now,
  via a new `_active_blit_encoder` on the command list following the same
  end-any-other-active-encoder-first pattern as `_active_compute_encoder` — verified
  end-to-end via `crgpusamples/Samples/HelloTexture`'s texture upload, which
  silently rendered solid black until this landed (these were unimplemented
  placeholders before, per the file comment in `crgpumetalrenderpass.m`).
  `crgpu_command_list_blit_image` implements only a blit that is a copy (same size,
  orientation and format); a scaling, flipping or converting one is reported and
  records nothing. A pipeline's `cull_mode`, `front_face_ccw` and `wireframe` are applied since
  2026-10-13 (set on the encoder at `crgpu_command_list_bind_graphics_pipeline`); before that Metal
  dropped all three and drew every triangle filled and unculled, which no caller had
  noticed because none culled. Back-face culling with counter-clockwise fronts is
  verified by a caller's read-back test; `CRGPU_CULL_MODE_FRONT`, clockwise fronts
  and wireframe are written and not run. A command list's command buffer is taken
  inside an autorelease pool of the backend's own, so a caller that is plain C with
  no run loop works; before, such a caller hung at its 65th command list.

## Build system

- **eqstd** (`EQ_ARRAY` and the rest; see "An array that grows") is linked
  PRIVATE. `CMakeLists.txt`'s `cr_add_workspace_or_fetch` adds the
  workspace's `../eqstd` checkout, edits included, when it is there (in its
  own binary dir, and only if no `eqstd` target exists yet), and otherwise
  fetches `endqueue/eqstd` at the commit pinned there (a hash, never a
  branch), so a clone of carrottus alone still builds. eqstd's own tests are
  built only when it is the top-level project. Moving the pin: change the
  hash in the call, with the code that needs the newer eqstd.
- **The shader step is `cmake/cr_shaders.cmake`, written once for every
  caller** (card X18, built 2026-11-01): `cr_add_shader()` makes SPIR-V,
  Slang's reflection, DXIL on Windows, and Metal source and a .metallib
  where a .metallib is made (elsewhere Metal source only under
  `CR_SHADERS_CHECK_METAL`). `CR_SHADERS_CMAKE` names it for a
  caller's build, which wraps it and never copies it. `tools/crgpu_msl` is a program of its own for the
  build machine (ExternalProject, or `CRGPU_MSL_EXECUTABLE`), never a
  target here and never in the library. Anything the file decides is read
  inside the function, not from a variable it set: it is read once, in the
  first directory that includes it.
- `GRAPHICS_BACKEND` is a single-choice CMake cache variable (`Vulkan` / `D3D12` /
  `Metal`) — there is no multi-backend build. The source list is assembled per-platform
  and per-backend *before* `add_library`, so only one backend's translation units ever
  compile in.
- `CMAKE_MSVC_RUNTIME_LIBRARY` is **not** set here — it must already be force-set by
  the parent `eqdev` root `CMakeLists.txt` before `add_subdirectory(carrottus)` runs (see
  `../CLAUDE.md`). If you're building `carrottus` standalone rather than through the
  workspace root, set it yourself before configuring, matching whatever
  it will be linked with uses, or Debug builds will fail to link once combined.
- SDL3 integration is header-only glue (`crgpu_sdl3.h`) — one inline
  `crgpu_create_surface_from_sdl3` per backend (wraps `SDL_Vulkan_CreateSurface` on
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

- **Threads in Carrottus GPU (2026-10-21): Carrottus locks what it hides; the
  caller synchronizes the handles it holds.** State the caller can't see is
  Carrottus GPU's to guard, on every backend: the bindless slot table, an
  adapter's one-device claim (an atomic exchange), a device's descriptor
  budget, D3D12's shared descriptor heaps and indirect signatures. Handles
  the caller holds are the caller's: a queue,
  like a command list, allocator or swap chain, is used by one thread at a
  time, on every backend, and Carrottus GPU takes no lock for it (`crgpu.h`,
  `crgpu_command_queue_submit_lists`). A lock there would be a decision made for
  the caller, and a hidden stall: a present or wait-idle holding it blocks
  every other thread's submit. Done so far:
  - Every backend: submit, `crgpu_command_queue_submit` (which the synced submit
    goes through), wait-idle and present bracket their use of the queue with
    a debug-build counter (`crgpuvulkan___queue_enter`,
    `crgpu_d3d12___queue_enter`, `crgpumetal___queue_enter`) that
    asserts when two threads are in one queue at once; nothing in release.
    D3D12's `crgpu_command_queue_submit` queues its waits, lists and signals by
    separate calls and bumps semaphore counts, so it was never safe from two
    threads at once either. Metal also uses its queue on its own when a
    swap chain is made and when a list takes its command buffer; Metal's
    queue allows that from any thread, and it has no Vulkan or D3D12
    counterpart.
  - Since 2026-10-21 nothing is made later: the instance lists its adapters
    when it is created and never changes the list, so every adapter call is
    lock-free; a device's queues and bindless table are made by
    `crgpu_device_create`, before the device is handed out, and read with
    no lock after (the old `_devices_lock`, `_bindless_create_mutex` and D3D12's
    atomic `_bindless` went with the lazy state they guarded). One device per
    adapter is an atomic exchange on the adapter.
  - Bindless registering and unregistering hold the bindless state's lock
    (Vulkan's `cr_mutex` across the `vkUpdateDescriptorSets` too, since the
    set is externally synchronized; D3D12's around its slot stacks, with the
    `Create*View` writes outside it, as D3D12 allows).
  - A threaded test on a fake driver (before 2026-10-21) found the old lazy
    races (8 logical devices, a slot handed out twice, TSan reports) and none
    after the fix, TSan, ASan and UBSan clean; the debug assert fired on
    unsynchronized submits and not on synchronized ones.
  Not done: `crgpu.h` states the queue rule, and nothing yet about bindless
  or the other calls.
- An indirect draw's `stride` is a multiple of 4 and at least the size of
  its args struct, on every backend (Vulkan's rule); otherwise nothing is
  recorded. A larger stride skips the caller's data between entries. Until
  2026-10-16 D3D12 ignored `stride`: its one cached command signature fixed
  `ByteStride` at the args size. It now keeps a signature per stride and
  kind on the device: the two tightly packed ones made with the device (so
  the common case takes no lock, since 2026-10-21), any other in
  `_indirect_signatures`, mutex-guarded, made at first use; all released with
  the device. `rabbittustests`' `test_draw_indirect_stride` draws with a
  32-byte stride; it fails on the old D3D12 code and passes on all three
  devices here. The same test found that D3D12's `crgpu_buffer_view_create`
  refused a buffer that was not a vertex, index or uniform buffer (Vulkan and
  Metal make a view of any buffer); it now keeps the range for any usage.
- The D3D12 backend lists adapters of feature level 12_0 and up (it
  required 12_2, from its first commit, for no reason any code had; changed
  2026-10-16). 12_0 guarantees resource binding tier 2, which descriptor
  sets of more than 128 SRVs need. Everything past it is a feature bit,
  read per adapter at instance creation through a probe device released
  before the next adapter's (since 2026-10-21; `CRGPU_DEVICE_TODO.md`):
  mesh shading (OPTIONS7), the depth bounds test (OPTIONS2), and bindless,
  which needs resource binding tier 3. WARP is listed, as
  `CRGPU_ADAPTER_TYPE_SOFTWARE`. Run on an Iris Xe (12_1, tier 3) and an
  RTX A2000 before the adapter change, and every `crgpusamples` sample on
  both and on WARP after it (2026-10-22); the tier-2 path has not run on any
  device.
- D3D12's shader-visible descriptors are in two heaps per device
  (`crgpu_device_d3d12::_csv_heap`, `_sampler_heap`), and every pool and
  the bindless state take a range of them (`crgpud3d12descriptors.cpp`).
  Before 2026-10-16 each pool had heaps of its own, so binding sets of two
  pools, or bindless and a pool, for one draw left a table in a heap no
  longer set. Since 2026-10-21 the heaps are made by `crgpu_device_create`,
  sized by the caller's descriptor budget (`crgpu_device_desc::max_descriptors`
  and `::max_samplers`, at most 1,000,000 and 2048), and a budget of 0 makes
  no heap. On every backend a pool past what is left of the budget is
  refused with `CR_RESULT_GPU_OUT_OF_DESCRIPTORS` (Vulkan and Metal keep a
  count, D3D12 the heaps, whose free ranges may be fragmented, which the
  header allows everywhere). Run on D3D12 (NVIDIA RTX A2000) by the samples
  and `rbtrend_gpu_tests` before the budget, debug layer clean, and by every
  sample after it (2026-10-22, both devices and WARP).
- A D3D12 `TEXTURE2D` view has no array slice, so until 2026-10-16 every
  `CRGPU_IMAGE_VIEW_TYPE_2D` view on D3D12 saw layer 0 whatever its
  `base_array_layer` (render target, depth, shader resource and storage
  views alike). A 2D view of any other layer is now made as a 2D array view
  of that one layer (`crgpu_d3d12_view_type`). Found by `rabbittustests`'
  `test_generate_mips`, whose layer 1 rendered into and sampled layer 0;
  it passes on both devices here, debug layer clean. A shader that
  declares a `Texture2D` and is handed such a view (a 2D view of a layer
  past the first) gets a `TEXTURE2DARRAY` view of one slice, which D3D12
  leaves undefined; the drivers here read it right. A caller that samples
  one layer of an array texture declares an array texture and passes a
  one-layer 2D array view, as a caller's downsample shader does since the
  same day; then the view and the shader agree on every backend. A run
  of `rbtrend_gpu_tests` with GPU-based validation on (a temporary
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
  (`crgpu_d3d12_storage_cubes_as_arrays`), whose third coordinate is
  the face as the cube's was. An `imageSize()` of a cube takes the width
  and height of the array's three (not run). `rabbittustests`'
  `test_cube_storage_views` writes every face of two cubes through a
  `CUBE` and a `CUBE_ARRAY` storage view and reads all twelve layers back;
  against the old views it wrote layer 0 twice and nothing else. It passes
  on D3D12 (both devices, debug layer clean), on Vulkan, and on Metal (M5
  Pro, validation clean with warnings fatal). Shader resource
  views of cubes were right already: a `CUBE` view starts at layer 0 by the
  header's rule, and `CUBE_ARRAY` takes its first face.
- Until 2026-10-16 the Vulkan device never enabled `imageCubeArray`, so a
  `CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY` view, and any shader that declares a
  cube array, were outside Vulkan's rules on every device. It is enabled
  for a device created with `CRGPU_FEATURE_CUBE_ARRAY_VIEWS` (every D3D12
  and Metal adapter has it), and `crgpu_image_view_create` refuses a
  `CUBE_ARRAY` view on a device without it, on every backend. Run on Vulkan by `test_cube_storage_views`
  on both devices here, validation layer clean, and on Metal, where the
  property is true and both cubes ran (validation clean).
- Since 2026-10-16 the Vulkan device can enable `multiDrawIndirect` (since
  2026-10-21 only with `CRGPU_FEATURE_MULTI_DRAW_INDIRECT`), and
  `CRGPU_ADAPTER_PROPERTIES::max_draw_indirect_count` reports what one
  indirect call takes on a device with it (1 without it, on every backend;
  UINT32_MAX on D3D12 and Metal). A larger count records nothing, on every
  backend.

- The debug name rule (see "A debug name is copied") was
  applied to every module on 2026-10-09. A device's queues are the device's,
  made and freed with it, names and all (Vulkan's graphics queue had no
  release path, a new one on each call, until 2026-10-21). Descriptor sets had none either until 2026-10-21; a Vulkan pool now keeps its sets in a list
  and frees them on reset and destroy, as D3D12's does.
- Cross-backend return values (see "A function returns the same value for the
  same condition in every backend") were aligned across Carrottus GPU and Carrottus Audio on
  2026-10-09; Carrottus Files, Carrottus Memory and Carrottus Net already matched. What still
  differs in Carrottus GPU, each a gap in one backend and not a choice:
  - Metal has no `crgpu_get_shader_pipeline_desc`, bundle or bindless calls;
    D3D12 has no `crgpu_surface_create_from_callback` or swap chain
    semaphore getters.
  - D3D12 returns `CR_RESULT_GPU_INVALID_IMAGE_TYPE` for an image desc
    whose `type` is not an image type (from building the resource desc, so
    after every other check, and after `CR_RESULT_GPU_HEAP_MISMATCH` for a
    placed image); Vulkan and Metal don't check it.
  - Metal: a set's argument buffer is laid out from every binding of its
    layout, and a shader's argument buffer struct from the bindings it
    declares (every declared one since 2026-10-17, used or not). A shader
    that declares only some of a set layout's bindings, and reads one past
    a binding it leaves out, reads the wrong argument. Every shader that
    reads a set declares the set's whole layout (a shared include does it).
    Since Slang (2026-10-31) a module keeps only the bindings its entry
    point reads, so the rule broke four of a caller's shaders. **Decided
    2026-11-01 with the user (card S27, a):** the rule goes into
    `crgpu.h` for every backend, the shader step pads each set from
    Slang's reflection (`crgpu_msl` on SPIRV-Cross's C++ API,
    `pad_argument_buffer_resources`), and Metal's pipeline creation refuses
    a module that differs (`CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH`). Draft
    `crgpu_shaderbindings_draft.h`, approved 2026-11-01; built the same
    day on the Mac, with the one shader step (`cmake/cr_shaders.cmake`,
    card X18), the padding `tools/crgpu_msl`'s own from Slang's
    reflection (SPIRV-Cross's can't make this layout), Metal's check of
    what it can see (each set's ids and their Metal kinds and counts);
    Windows and Linux not built (`CRGPU_DEVICE_TODO.md`).
  - D3D12's `crgpu_command_list_begin_rendering` returned early without a colour
    attachment until 2026-10-17, so a depth-only pass rendered on Vulkan
    and Metal only. It now begins a pass of a depth attachment alone;
    written on macOS, then run on D3D12 on 2026-10-17 by `rabbittustests`'
    `test_lit_mesh_shadow` (Intel Iris Xe and NVIDIA RTX A2000, debug
    layer clean). A caller's host built and ran over it on D3D12 and
    Vulkan the same day (reported), but that caller renders no depth-only
    pass, so that run doesn't exercise it.
  - Metal's `crgpu_swap_chain_create` rejects `buffer_count != 1` and
    `CRGPU_PRESENT_MODE_MAILBOX`. Geometry and tessellation are behind
    `CRGPU_EXT_GEOMETRY_SHADER` and `CRGPU_EXT_TESSELLATION` since
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
  `crgpu_swap_chain_desc::present_scaling` names one
  `CRGPU_PRESENT_SCALING` (ONE_TO_ONE, STRETCH, ASPECT_FIT; NONE refused
  with `CR_RESULT_GPU_INVALID_PRESENT_SCALING`), and
  `crgpu_surface_get_present_scalings` says which a surface takes; a
  scaling outside that is refused with
  `CR_RESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED`. No filter and no bar color
  is promised, since neither can be chosen on every backend. The checks
  before the surface is read are written once
  (`crgpu___internal_check_swap_chain_desc`,
  `crgpu___internal_check_surface_present_scalings`). What each backend
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
    Debug): `crgpu_objects_test` makes a swap chain of each on a windowless
    `CAMetalLayer`, flipped and not, and reads the gravity back (plain,
    ASan, TSan, Metal validation), and `crgpusamples`' HelloTriangle
    showed each in a 900x900 window over 1280x720 back buffers
    (`--scaling`, `--window`), under Metal validation with warnings fatal:
    top-left and cropped, stretched, and letterboxed.
  Every caller in the workspace names one: a caller's renderer that scales
  with draws of its own names ONE_TO_ONE, and so does every sample (their
  windows can't be resized). `crgpu_objects_test`'s checks before the
  surface is read pass on Windows on both backends (adapter 0, the Iris Xe,
  only: the test takes the first adapter, and no real swap chain is made
  there). No test reaches the extension's path, which is how the crash
  above went unseen: only a real surface on the A2000 did. Vulkan still
  needs a Linux run, Wayland's free extent included.
- Carrottus Files was reworked on 2026-10-22 (`CRFILES_TODO.md`): an instance
  with the caller's allocator; UTF-8 paths (Windows converts each to UTF-16
  and calls only the W functions); access, creation and share mode chosen by
  the caller; reads and writes at an offset, with no cursor; what is at a
  path from `crfiles_path_get_info`; one directory level per
  `crfiles_directory_create`. The dialogs, path-string functions and working
  directory are gone, and with them every static buffer. Every check is in
  the front end (`src/crfiles.c`); the backends implement
  `src/files/crfilesshared.h`: one POSIX file for Linux and Apple
  (`F_FULLFSYNC` where it exists) and Windows. Built and run on macOS,
  and on Windows 11 (MSVC, `windows-d3d12`, Debug, 2026-10-22); the Linux
  build hasn't been compiled. The platform's own error number goes to the
  instance's message callback since 2026-11-09 (card S10's rules M1-M7 in
  `crcore.h`, Carrottus Files the first module built with them), and a
  dynamic library is opened, searched and closed through Carrottus Files (card
  S44, `crfiles_library_*`; `CRFILES_TODO.md`), built and run on
  Windows, and on the Mac and the iOS simulator on 2026-11-09 (the POSIX
  halves of both; Linux not yet compiled).
  Listing a directory and an advisory lock on an open file (card S39,
  `crfiles_list_lock_draft.h`) were built on 2026-11-06 on Windows 11:
  `crfiles_listing_open`/`_next`/`_close` (no "." or "..", a reparse point
  by its tag, the end said again by the front end) and
  `crfiles_file_lock`/`_unlock` (`LockFileEx` on the byte at 2^64 - 2,
  `flock` on POSIX; the file records its lock, so twice and none answer the
  same everywhere), and every Windows path read as WTF-8 by Carrottus Files' own
  converter, so a listed name with an unpaired surrogate opens. Run on
  NTFS only (no other file system on the machine); symbolic links not made
  there (no developer mode), a junction listed in their place. The POSIX
  backend's half is written, not compiled. Removing an empty directory
  and renaming one to a free name (card S41, `crfiles_directory_draft.h`:
  `crfiles_directory_remove`, `crfiles_directory_rename`,
  `CR_RESULT_FILES_NOT_EMPTY`) were built on 2026-11-07 on Windows 11,
  the same way (`CRFILES_TODO.md` has the run); POSIX's half, on
  `renameat2`/`renamex_np` so the target is refused by the rename itself,
  written, not compiled. Copying a file's permissions to another (card
  S42, `crfiles_mode_draft.h`: `crfiles_path_copy_mode`, POSIX's nine
  bits or Windows' read-only attribute, links followed) was built the same
  day on Windows; its POSIX half written, not compiled.
- Carrottus Net was written on 2026-10-07 and its header reworked, and every backend
  rewritten to it, on 2026-10-08: streaming HTTP transfers, TLS and proxy
  descs, error detail, a resolver, a wait set. The Apple side compiles clean
  with the `macos-metal` preset and passes `tests/crnet_test` (run it with
  `tests/run_crnet_test.sh build-macos-metal/Debug/crnet_test`; it needs Python 3
  and `openssl`; 343 checks since 2026-10-22 (and an opt-in `internet` part, 22 more, run only by name: real DNS, public certificate chains good and bad, a real `wss://` echo server and a host that never answers; passing on macOS plain, ASan and TSan), with the struct-base and allocator parts and a `records` part: a listener's accept aborted from another thread, empty error records, the local IPv4 addresses and debug names read back). Linux (libcurl + BSD sockets) built with the
  `linux-vulkan` preset and passed it on 2026-10-14. Windows (WinHTTP +
  Winsock2) built with MSVC under the `windows-d3d12` preset and passed all
  261 checks on 2026-10-14, Windows 11 (`tests/run_crnet_test.sh
  build-windows-d3d12/Debug/crnet_test.exe` from Git Bash). The first run
  failed six. Five came from WinHTTP holding back
  `WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE` until the response
  arrives, which made `_start` wait for the response. `_start` now returns
  at `WINHTTP_CALLBACK_STATUS_REQUEST_SENT` and leaves the completion in
  flight for the next call. The sixth was the test: WinHTTP shares
  connections between every session in a process, so the test's
  `server_stat` now asks over its own TCP connection. That sharing is
  WinHTTP's own: it doesn't break the one-transfer-per-client rule, but a
  new client is not a new connection there. `CRNET_TODO.md` lists what each
  part assumes and what the macOS run settled. Every part of Carrottus Net (TCP, the
  resolver, HTTP and WebSocket) now has a caller outside this repo. On Linux, WebSocket needs libcurl 8.11+ (older ones
  return `CR_RESULT_NET_UNSUPPORTED`; HTTP works regardless). iOS: the
  `iossimulator-metal` preset builds the tests as plain simulator executables,
  and `CRNET_TEST_LAUNCHER="xcrun simctl spawn booted"
  tests/run_crnet_test.sh build-iossimulator-metal/Debug/crnet_test` runs
  `crnet_test` in a booted simulator against the server on the Mac's loopback.
  On 2026-10-14 every target built with no warnings and `crnet_test` passed
  all 261 checks three runs in a row in an iPhone 17 Pro simulator (iOS 26.5),
  its `websocket` part 46 of them; `crperf_test` and `crinput_test` pass
  there too. A device can't run a bare executable, so on a device Carrottus Net has
  only been built (a caller's iOS build), not run. Carrottus Net's own parsers of
  network bytes (header blocks, certificate DER, UTF-8, error text, proxy
  URLs) have libFuzzer targets in `tests/fuzz/` (2026-10-28): the
  `macos-fuzz` preset (Homebrew LLVM's clang, `build-macos-fuzz`) or
  `linux-fuzz` builds them, and each also runs as a plain ctest,
  `<target>_replay`, over its seeds and every input kept in
  `tests/fuzz/regressions/<target>`, which is where a crashing input goes
  once fixed. How to run them: `tests/fuzz/README.md`. On Windows the
  `windows-fuzz` preset (LLVM's clang through its GNU-style driver,
  2026-10-29; `../claude_made_decisions.md` says why) builds them, and
  `crnet_fuzz_http_head` has a mode for WinHTTP's own split of its raw
  header block (`crnet_win_add_raw_headers`,
  `src/net/backends/windows/crnetwinhttphead.h`); each target ran 10
  minutes there with nothing found.
- Carrottus Input was written on 2026-10-13: the header, the front end and the
  GameController backend (`src/input/backends/apple/crinputgamecontroller.m`).
  It builds with the `macos-metal` preset and `tests/crinput_test` passes
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
  unchecked. It builds for iOS, and `crinput_test` passes in the simulator
  (2026-10-14); no gamepad has been read there.
- Carrottus Input's GameInput backend (`src/input/backends/windows/crinputgameinput.c`)
  was written on 2026-10-14, on the Windows SDK's GameInput (`GameInput.h`,
  `gameinput.lib`; no NuGet package). Devices are learnt from a device
  callback: a blocking enumeration when the instance is made, so the first
  poll lists what is connected, then a dispatcher so that every later
  callback runs inside the poll's dispatches. Each poll walks the device's
  reading history from the reading it took last, falling back to the newest
  on `GAMEINPUT_E_REFERENCE_READING_TOO_OLD`. `crinput_test` passes on
  Windows 11 with a wired Xbox Series X|S controller (045e:0b12): found at
  the first poll, with four rumble motors and a battery state of none.
  GameInput gives no name for it, and sends no reading until about 0.7 s
  after it is first asked, then one only when something changes. Pressing
  buttons, moving sticks and rumble have not been checked by anyone holding
  it. No motion sensors, touch surfaces, lights or labels. The SDK header's
  C macros for `IGameInputDispatcher` are missing a parenthesis, so the file
  calls every method through `lpVtbl`, and the header needs `<stdbool.h>`
  before it in C.
- Carrottus Input's evdev backend (`src/input/backends/linux/crinputevdev.c`) was
  written on 2026-10-14. It finds gamepads by `BTN_GAMEPAD`, learns of new
  nodes through inotify on `/dev/input` read at each poll, takes every event
  between polls, and plays `FF_RUMBLE` in 2-second plays that each poll past
  a second renews. `crinput_test` passes on Fedora 44 with an Xbox Series
  X|S controller (xpad) connected: its name, ids, buttons, axes, analog
  triggers and rumble were found and its state at rest read. Held and
  pressed on 2026-11-12 (Fedora 44, xpad, over USB), read by a checker over
  this module's calls: all 15 buttons went down as their bits (the d-pad
  from xpad's hat, Menu and the Xbox button included), each stick reached
  -1 and +1 on both axes and each trigger 0 to 1, and both grip motors
  ran at full and stopped, felt by the user (`CR_SUCCESS` on each call; no
  trigger motors, which xpad doesn't drive). Labels came back empty, the
  controller not reporting `CRINPUT_GAMEPAD_FEATURE_LABELS`. Battery and lights were added the same day, from sysfs under the
  node's input device and the device above it: the first `power_supply` of
  type Battery (files kept open, `pread` from 0 at each poll; `capacity`,
  else `capacity_level` as 5 steps), a multicolor LED with red, green and
  blue channels or else three single-colour LEDs, and LEDs named
  `player-1`... (player n lights `player-n` alone). They are found once,
  when the gamepad is first seen. No device here has either; they have run
  only against fake sysfs trees modelled on hid-playstation and hid-sony,
  with a scratch harness that is not committed. Writing an LED needs root
  unless a udev rule grants it, so lights usually return
  `CR_RESULT_INPUT_ACCESS_DENIED`. Not written: motion sensors and
  touch surfaces (sibling event nodes of the same device); no gamepad is
  given them. xpad reports the Xbox X button
  (left) as `BTN_X`, which the kernel defines as `BTN_NORTH`, and Y (top) as
  `BTN_WEST`; Carrottus Input passes the kernel's code on, as it has no table of
  devices.
- Eight CTest targets exist: `crcore_test` (the version as the header,
  CMake and the library each have it, and result names), `crgpu_device_test`
  (adapters and devices: listing, adapter properties with chained limits,
  every create-device check, queues by kind, the descriptor budget, one
  device per adapter, a device made again after one is destroyed; it needs a
  GPU and no window, and exits 77, skipped, with no adapter; passes on Metal,
  plain, ASan and TSan, 2026-10-21), `crgpu_objects_test` (the Carrottus GPU
  calls nothing else in the workspace made until 2026-10-22: a format by
  name, a fence signalled from the CPU and waited on, a semaphore, a buffer's
  desc and GPU address, a buffer view's and a heap's desc, a descriptor pool
  reset and allocated from again; also a GPU and no window, 77 without one;
  passes on Metal plain, ASan, TSan and under Metal validation, 2026-10-22;
  since 2026-10-23 also the present scaling checks that come before a
  surface is read, on a second device made with PRESENT, and on Metal swap
  chains on a windowless `CAMetalLayer` (`tests/crgpu_test_metal_layer.m`),
  passing the same four ways;
  still never called anywhere: `crgpu_surface_create_from_callback`, the
  storage-buffer and storage-image bindless calls,
  `crgpu_command_list_bind_compute_bindless_resources` and
  `crgpu_compute_pipeline_get_desc`), `crmemory_test` (every argument
  check, zero pages on commit and after a decommit, a second commit keeping
  contents, a reservation larger than memory), `crfiles_test` (every Carrottus Files
  call and check in a temporary directory: each access and creation choice,
  reads and writes at an offset with a short read at the end, sizing,
  flushing, a mapping at an unaligned offset outliving its file, path info,
  replace, remove, one-level directories, a non-ASCII name, two threads
  reading one file; counting allocator), both added 2026-10-22 and passing on
  macOS plain, ASan and TSan, and on Windows 11 (MSVC, Debug, 2026-10-22),
  `craudio_test` (every call but `craudio_stream_start` and a real
  `craudio_device_set_sample_rate`, so nothing is heard and no device is
  changed: argument checks, both directions' devices and properties, timing
  caps, a probe of the default output's own format followed to acceptance,
  a stream created stopped, read and destroyed, the device-event callback
  set and cleared, all through a counting allocator; passes with no device;
  added 2026-10-22, macOS plain, ASan and TSan; on Windows 11 over WASAPI,
  Debug, Release and ASan, 2026-10-29; since 2026-11-02 also a silent
  stream started, its callbacks checked from inside, stopped and started
  again. On Windows on 2026-11-03 that version first failed to build
  (MSVC's C compiler gives `<stdatomic.h>` only from C11 and behind
  `/experimental:c11atomics`, now set on the test as on the Vulkan
  backend; and `nanosleep`, now Carrottus Perf's monotonic wait) and then found
  WASAPI calling the stream callback while the state was still STOPPED:
  the prefill before the device starts ran ahead of `RUNNING`, which the
  header defines as the state while the callback is called. WASAPI and
  ALSA (the same prefill order) now store `RUNNING` first and put
  STOPPED back if the start fails, as CoreAudio and RemoteIO already
  did. WASAPI run (ThinkPad P1, Realtek speakers, `windows-d3d12` and
  `-vulkan`, Debug and Release); ALSA written, not compiled),
  `crinput_test`
  (above) and `crperf_test` (the monotonic clocks,
  `crperf_sleep_until_monotonic_ns` and `crperf_sleep_until_monotonic_ms`,
  and since 2026-10-22 the CPU counter and `crperf_get_ram_usage`),
  which needs nothing but the machine. It has run on macOS; the Windows and
  Linux halves of the nanosecond sleep, which the millisecond one waits
  through, are written and have not been compiled. `tests/` also holds two hardware tests, built behind
  `CARROTTUS_BUILD_TESTS` but deliberately not registered with ctest because each
  needs something real: `craudio_sine` (an audio device and someone listening;
  until 2026-10-22 its probe loop zeroed the config's base each round, so
  every probe was refused as `CR_RESULT_STRUCT_WRONG_TYPE` and it never
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
  and `crnet_test` (a local server, started by `tests/run_crnet_test.sh`).
  Carrottus GPU's are `crgpu_device_test` and `crgpu_objects_test`, which
  draw nothing; the rest of Carrottus GPU is
  verified by running the `crgpusamples` samples.
- No static/immutable sampler support. `crgpu_sampler` (added alongside
  `CrGPUDynamicIndexing`) only covers the dynamic, descriptor-bound case — a real
  `VkSampler` written into a descriptor set on Vulkan, a heap-slot `CreateSampler` on
  D3D12, an argument-buffer `MTLSamplerState` on Metal. D3D12's *static* samplers
  (baked into the root signature at pipeline-creation time, zero descriptor-heap cost)
  and Vulkan's *immutable* samplers (`VkDescriptorSetLayoutBinding::pImmutableSamplers`,
  set to `NULL` in `crgpuvulkandescriptors.c`) are a real, better-fitting mechanism for the common
  case of a small fixed set of samplers that never change for the life of a pipeline —
  worth adding as a genuinely separate concept alongside `crgpu_sampler`, not a
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
  `CRGPU_EXT_SAMPLER_YCBCR_CONVERSION` (Vulkan only, an optional feature
  there, with its `_NOT_SUPPORTED` result; D3D12 and Metal have none). Video
  doesn't need it: per-plane views (card S11) and conversion in the shader
  work on every backend. Built when a caller asks.
- `CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` works on Metal since
  2026-10-22. Before, Metal's `crgpumetaldescriptors.m` wrote only the
  texture half into the argument buffer, and under Carrottus GPU's
  per-descriptor-set argument buffers SPIRV-Cross's sampler member for a
  combined sampler took the texture's own id, so a shader with one didn't
  compile (SPIRV-Cross allows that overlap only with Metal 3's mutable
  aliasing). Now the argument buffer layout gives the two halves distinct
  members: the texture keeps the binding's number, and the sampler sits at
  `CRGPU_METAL_COMBINED_SAMPLER_INDEX_BASE` (1024) past it, by a remap of
  each combined sampler at cross-compilation (`crgpumetalshader.m`), a
  second argument in the layout's encoder and a second write
  (`crgpumetaldescriptors.m`). A layout with a combined binding refuses a
  binding at or past 1024. Run on Metal by a caller's test of two
  combined samplers beside a separate pair, and by the same test on D3D12
  and Vulkan
  (2026-10-23, Windows 11, Intel Iris Xe and NVIDIA RTX A2000, debug and
  validation layers silent), where combined samplers needed no change;
  `crgpusamples/Samples/HelloTexture` still uses the separate
  `SAMPLED_IMAGE` + `SAMPLER` pair it was written with.
