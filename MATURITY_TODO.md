# Maturity TODO

Written on 2026-10-20. Four pieces of work that bring the parts of spudlib
written before its current rules up to them, and give it what C APIs that
stay stable for decades have and it lacks. Each is meant for a session of
its own, in the order below. Items 1 and 2 are done (2026-10-20); 3 has three parts done (version,
result names, the allocator, SpudFiles included since item 4) and SpudMemory's
arena out, with the policy still to do, and 4 is done (2026-10-22). Item 3 was added the same day, from a comparison with Vulkan,
Win32, POSIX, SQLite, zlib, libcurl and Lua; it goes before SpudFiles so
that the new `spudfiles.h` is written to its rules.

What "done" means here is measured by the workspace's `check.sh`: when items
1, 2 and 4 are finished, `check.baseline` has no `mutable-globals|spudlib` or
`struct-bases|spudlib` entries left, and nothing under `bindings/`. Item 3
adds to `spudcore.h` and removes no entry.

Every item ends the same way:

- build on macOS and run the tests, plain and under the `-asan` preset;
- build every caller the change breaks, in the same change, and run its tests;
- `./check.sh` from the workspace, with the entries this item removes taken out
  of `check.baseline`;
- after the spudlib commit is pushed, `./check.sh --update-locks`, and commit
  the locks with the repositories that use them;
- each changed repository's `.git/COMMIT_DRAFT` rewritten to describe its
  change.

## 1. Remove `bindings/` (done 2026-10-20)

**What:** delete `bindings/rust/` and `bindings/swift/`, and the "Rust
bindings" section of `CLAUDE.md` and the Rust and Swift bullets of
`README.md`.

**Why:**

- **Nothing calls them.** spudlib's callers include its C headers
  directly, and nothing in the workspace outside spudlib names either
  binding. The workspace's own rules (`../CLAUDE.md`) say which code may
  call spudlib, and none of it is Rust or Swift.
- **A binding names its caller's language.** spudlib never names or assumes
  its caller. Its C headers are its interface, and every language with C
  interop can use them as they are.
- **Each is a second build of spudlib.** `spudlib-sys`'s `build.rs` compiles
  five of the Vulkan backend's files with the `cc` crate, and the Swift package
  compiles symlinks to `spudcore.c` and two Metal files. Neither goes through
  `CMakeLists.txt`, so neither gets its backend selection, definitions or
  warnings. Each is a partial spudlib that nothing tests.
- **They have drifted.** `bindings.rs` declares the SpudGPU descs without
  `base`, so its layouts have been wrong since 2026-10-19. Neither binding
  has been changed since 2026-05-20.

If a caller in another language ever needs spudlib, what serves it is a
separate repository of its own: a `-sys` crate whose bindgen pass reads the
real headers at build time, over a spudlib built by CMake, or a Swift module
map over the headers. Neither lives here.

**Breaks:** nothing.

**Done:** `bindings/` deleted, with `CLAUDE.md`'s "Rust bindings" section,
`README.md`'s "Bindings" section and the Rust and Swift build folders in
`.gitignore`. `STRUCT_BASE_TODO.md`'s SpudGPU record still mentions the
bindings as they were then.

## Done outside the plan: SpudInput's dispatch (2026-10-20)

Not one of the items, but it changes the files item 2 touches. Any number of
native input APIs may now be in one build (GameInput and XInput on Windows,
once XInput is written), and the caller's `native_api` chooses: the front end
has one `switch` per backend verb on it, the backends are part of
`src/spudinput.c`'s translation unit (included there, never compiled alone),
and each verb is `spudinput_<api>_<verb>`, forced inline into its case with
`SPUDINPUT_BACKEND_FUNCTION`. `CLAUDE.md`'s SpudInput paragraph and
`src/input/spudinputshared.h` have the rule.

Built and tested on macOS only (Debug, Release, `-asan`, the Xcode
generator; every caller in the workspace against it). **Still to do:** build
`windows-d3d12` (the GameInput backend, and the `/w14714 /we4714` flags) and
`linux-vulkan` (evdev), and run `spudinput_test` on each.

## 2. Convert SpudInput to struct bases (done 2026-10-20)

Done as `STRUCT_BASE_TODO.md` classifies it; that file's "SpudInput,
converted" is the record of what changed and what was run. The nine
`spudinput.h` entries are out of `check.baseline`. **Still to do:** run
`spudinput_test` once with a gamepad connected, which is when
`spudinput_gamepad_get_properties`' three struct-base refusals run.

**Proposed alongside it, as its own change:** a query of whether a
native API built in will start on this machine (GameInput's runtime may be
missing), such as `spudinput_native_api_is_available(api)`, so the caller can
choose before `spudinput_instance_create` fails with
`SPUDRESULT_API_SPECIFIC_FAILURE`. **Decided 2026-10-28 with the user
(card S8): add it.** It reports whether the platform will start that API
on this machine (GameInput's runtime present or not), as adapter listing
reports what is there, and never picks a fallback: which API to use is the
caller's. Its own change; needs a Windows run. Not started.

## 3. What every module shares: version, result names, allocator, policy

Begin with a proposal for the `spudcore.h` additions and the policy text,
and write code only once it is agreed.

Long-lived C APIs have these and spudlib does not:

- **A version, at compile time and at run time: done (2026-10-20).**
  `ZLIB_VERNUM` and `zlibVersion()`, `SQLITE_VERSION_NUMBER` and
  `sqlite3_libversion_number()`, `LIBCURL_VERSION_NUM` and
  `curl_version_info()`, `VK_HEADER_VERSION` and
  `vkEnumerateInstanceVersion`. `spudcore.h` has `SPUDLIB_VERSION_MAJOR`/
  `_MINOR`/`_PATCH` (0.1.0), `SPUDLIB_VERSION`, Vulkan's packing
  (`SPUDLIB_MAKE_VERSION`, 10/10/12 bits, no variant) and
  `spud_get_version()`; `CMakeLists.txt` reads the version from the header.
  One version for the library, no version string, and no caller refused for
  its version. The header's "Version" group has what major, minor and patch
  promise from 1.0.0, which belongs with the stability policy below. Still
  open: tags (none yet), and a `check.sh` check that a `vX.Y.Z` tag matches
  the header, once there are tags.
- **A name for each result: done (2026-10-20).** Vulkan's `string_VkResult`
  (`vk_enum_string_helper.h`) and `curl_easy_strerror`. spudlib already had
  it as `spudresult_str()` (`spudcore.h`), which keeps its name; it was
  missing 11 results (SpudGPU's six heap and placement results, SpudFiles'
  1003-1007), which read as "SPUDRESULT_UNKNOWN". Every result has a case (157 as of 2026-10-22),
  and the switch has no `default`, so clang's `-Wswitch` (on by default)
  and GCC's (in `-Wall`) report a result added without one. MSVC's C4062
  is off by default, so a Windows-only change is not caught.
- **The caller's allocator: done (2026-10-21; SpudFiles 2026-10-22).**
  `VkAllocationCallbacks`, `SQLITE_CONFIG_MALLOC`, `lua_Alloc`, zlib's
  `zalloc`/`zfree`. `spudcore.h`'s "The caller's allocator" group has
  `SPUD_ALLOCATOR` (frozen: `user_data`, `allocate(size, alignment)`,
  `free(memory, size, alignment)`, sized and aligned both ways, so an arena
  or pool needs no header per block), the contract, and
  `spud_allocator_system()`, the system heap by name. There is no default:
  a create call refuses a NULL allocator (`SPUDRESULT_NULL_ALLOCATOR`, 15)
  and one missing a function (`SPUDRESULT_INVALID_ALLOCATOR`, 16).
  - SpudGPU, SpudAudio, SpudNet and SpudInput each take it as a required
    `allocator` field of a based instance desc (`spudgpu_instance_desc`,
    `SPUDAUDIO_INSTANCE_DESC` and `spudnet_instance_desc` are new;
    `spudgpu_create_instance`'s four loose parameters moved into its desc).
    The first checks of each create are written once per module.
  - Every handle starts with a `spud_object_head` (`src/spudallocator.h`):
    its debug name and its own copy of the allocator, so that a handle
    freed after its instance (a SpudNet resolver; perhaps WASAPI's device
    watch) still has it. `spud_debug_name_set` copies through it.
  - Every allocation of every backend goes through it, freed with its own
    size and alignment. `tests/spudtest_allocator.h` checks that in
    `spudinput_test` and `spudnet_test`.
  - Left: the D3D12 backend's
    `std::vector`/`std::string`; memory the platforms and drivers allocate
    themselves (SpudGPU passes Vulkan no `VkAllocationCallbacks`).
  - Which threads call it is in each module's header: a SpudAudio stream's
    callback thread never does, its device watch does.
  - SpudMemory's arena is out of SpudLib: done (2026-10-22). It decided
    alignment, growth and zeroing, and its alignment macro, destroy
    (`free` on mapped memory) and create (no check of the reservation; a
    zero commit size divided by zero later) were broken. Nothing but
    a caller's log line used it, so it went rather than moving to another
    repository. SpudMemory is `spudmem_get_page_size`, `spudmem_reserve`,
    `spudmem_commit`, `spudmem_decommit` and `spudmem_release`, each returning a
    `SPUDRESULT`, with a POSIX backend for Linux and Apple
    (`src/memory/backends/posix/`) beside Windows'.
    `tests/spudmemory_test` passes on macOS, plain and under ASan and
    TSan, and on Windows 11 (MSVC, Debug, 2026-10-22), and the
    POSIX one has not been compiled on Linux.
    **Decided 2026-10-27 with you (card X1b, line 2):** the shape is kept,
    with two changes. The prefixes are spelled out: SpudMemory's `smem_` is
    `spudmem_` (`SPUDMEM_`, `SPUDRESULT_SPUDMEM_*`) and SpudFiles' `sfs_` is
    `spudfile_` (`SPUDFILE_`, `SPUDRESULT_SPUDFILE_*`), every caller
    renamed in the same change (every caller in the workspace). And
    `spudmem_get_reserve_granularity()` reports where a reservation starts
    (Windows' 64 KiB `dwAllocationGranularity`; the page size on POSIX), as
    Unreal's `GetVirtualSizeAlignment` does, so a caller can size small
    reservations without stranding address space; nothing is rounded for
    it. Run on macOS (2026-10-27); the Windows backend's new call is
    written, not compiled.
- **A stated stability and threading policy.** `CLAUDE.md` says a released
  layout never changes, but nothing says what "released" is: there are no
  tags or versions, and spudlib is STATIC only, pinned by commit in each
  caller's `deps.lock`, so in practice only source compatibility holds. Write
  down what is promised and from which version (as SQLite and Vulkan do), and
  per call what may be called from which thread at once (Vulkan's "externally
  synchronized"); today only about 15 places in the headers say.
  **Decided 2026-10-27 with you (card X2):** per-library semantic versions
  (`vX.Y.Z` tags, checked against `SPUDLIB_VERSION_*` from 1.0); before 1.0
  a minor version may break, each break listed against the ABI snapshot;
  from 1.0 released layouts are frozen and grow by chained structs, enums
  and results append-only. spudlib stays static-only until a shared build
  is needed, which keeps the next bullet waiting. Workspace releases are
  dated tags on eqdev, never on spudlib. The rules are in the root
  `CLAUDE.md`, "Versions and releases". **The threading half, decided
  2026-10-27 (card X3):** every object is used by one caller at a time
  (serialization, not thread identity), stated once in each header; the
  exceptions carry a `Thread:` line; Debug builds detect overlapping calls
  on one object. SpudLib creates no threads but what a platform imposes (an
  audio callback; the network stacks' own). Root `CLAUDE.md`, "Threading".
- **The platform's own error behind `SPUDRESULT_API_SPECIFIC_FAILURE`.
  Decided 2026-11-01 with the user (card S10), revised the same day:**
  card S2's message channel, not a record per object. Every module with an
  instance takes one `SPUD_MESSAGE_CALLBACK` and its user data in its
  instance desc (NULL for none; required with SpudGPU's validation on);
  every failure a platform call reports is a message, whatever
  `SPUDRESULT` it maps to (ENOENT and ENOTDIR both behind `NOT_FOUND` are
  told apart; the caller filters on the result, SpudLib judges nothing too
  routine; not would-block, a wait's timeout, an orderly close or a time
  limit SpudLib keeps), at every validation level, sent
  inside the failing call on the caller's thread before it returns, with
  the call's name, the object and its debug name, the `SPUDRESULT`, the
  platform's numbering (`SPUD_PLATFORM_ERROR_SOURCE`, SpudNet's enum moved
  to `spudcore.h`, `VKRESULT` added) and code, and the platform's own
  wording whole where it has some. The callback may be called from several
  of the caller's threads at once, never from SpudLib's, must not call the
  module whose call is reporting back, and reads the message only during
  the call; nothing is allocated. An object whose platform reports on its
  own thread (a SpudAudio stream) keeps one lock-free pending slot (a 64-bit and a 32-bit atomic, never a
  16-byte one), holding
  the first failure, sent by the next call on it with a count of the rest.
  Severities are every platform layer's (VERBOSE to D3D12's CORRUPTION),
  none merged. The callback is SpudLib's only output: its ~30 prints go
  (platform failures to messages, the D3D12 debug layer's printer and the
  live-object report to S2's), all but `SPUD_STALE`'s Debug line, which
  has no instance to reach. A create's message
  also names the object it was making. Messages go to the instance's
  creator only; a caller handed just an object reports the SpudLib call
  and its code, the detail being the message before it in the creator's
  log (accepted). SpudMemory's four calls and `spudrandom_fill`, with no
  instance, take `SPUD_PLATFORM_ERROR *out_error` (result, source, code).
  SpudNet keeps its per-object records and also sends its failures. S2's
  text changes in one detail: the callback is accepted with validation off.
  Rejected on the way: a record and a `_get_error` on every object that can
  fail (the first answer), and one generic getter over `void *`. Draft
  `spud_platform_error_draft.h`, approved 2026-11-01. Then a message at
  every platform failure (the ~190 behind `API_SPECIFIC_FAILURE` and those
  mapped to codes of their own), SpudLib's prints removed, the tests it
  lists, and runs
  on macOS, Windows and Linux. Not built.
- **`void` recording calls that silently record nothing** (found reading
  card S10's draft, 2026-11-01). Each prints why and returns: out of memory
  building barriers (D3D12's `spudgpu_cmd_pipeline_barrier`,
  `_image_barrier`, `_image_barrier_subresource` twice,
  `_acceleration_structure_barrier`), a scaling blit
  (`spudgpu_cmd_blit_image`, Metal and D3D12), an unrecognized
  `SPUDGPU_IMAGE_LAYOUT` (Metal's three image barriers). Each is made
  unable to fail (room made beforehand, as the acceleration structure
  build's checks do; an enum checked where the caller can be refused) or
  refused with a code; the print goes. A change to public calls: a draft
  first. Not started.
- **Before any shared build of spudlib** (not needed while it is static
  only): an export macro, default-hidden symbols, a `SOVERSION`, and symbol
  versioning or a `.def` file.

**Breaks:** nothing, if the allocator is optional and chained or added with
the struct bases.

## 4. Rework SpudFiles: done (2026-10-22)

The proposal was agreed and built; `SPUDFILES_TODO.md` has the record.

The largest of the four, and the one to design before writing: begin the
session with a proposal for the new `spudfiles.h`, and write code only once it
is agreed. The plan is in `SPUDFILES_TODO.md`: what is wrong now, what the
callers use, the proposal to start from, and what it breaks.
