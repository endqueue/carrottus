# Maturity TODO

Written on 2026-10-20. Four pieces of work that bring the parts of carrottus
written before its current rules up to them, and give it what C APIs that
stay stable for decades have and it lacks. Each is meant for a session of
its own, in the order below. Items 1 and 2 are done (2026-10-20); 3 has three parts done (version,
result names, the allocator, Carrottus Files included since item 4) and Carrottus Memory's
arena out, with the policy still to do, and 4 is done (2026-10-22). Item 3 was added the same day, from a comparison with Vulkan,
Win32, POSIX, SQLite, zlib, libcurl and Lua; it goes before Carrottus Files so
that the new `crfiles.h` is written to its rules.

What "done" means here is measured by the workspace's `check.sh`: when items
1, 2 and 4 are finished, `check.baseline` has no `mutable-globals|carrottus` or
`struct-bases|carrottus` entries left, and nothing under `bindings/`. Item 3
adds to `crcore.h` and removes no entry.

Every item ends the same way:

- build on macOS and run the tests, plain and under the `-asan` preset;
- build every caller the change breaks, in the same change, and run its tests;
- `./check.sh` from the workspace, with the entries this item removes taken out
  of `check.baseline`;
- after the carrottus commit is pushed, `./check.sh --update-locks`, and commit
  the locks with the repositories that use them;
- each changed repository's `.git/COMMIT_DRAFT` rewritten to describe its
  change.

## 1. Remove `bindings/` (done 2026-10-20)

**What:** delete `bindings/rust/` and `bindings/swift/`, and the "Rust
bindings" section of `CLAUDE.md` and the Rust and Swift bullets of
`README.md`.

**Why:**

- **Nothing calls them.** carrottus's callers include its C headers
  directly, and nothing in the workspace outside carrottus names either
  binding. The workspace's own rules (`../CLAUDE.md`) say which code may
  call carrottus, and none of it is Rust or Swift.
- **A binding names its caller's language.** carrottus never names or assumes
  its caller. Its C headers are its interface, and every language with C
  interop can use them as they are.
- **Each is a second build of carrottus.** `carrottus-sys`'s `build.rs` compiles
  five of the Vulkan backend's files with the `cc` crate, and the Swift package
  compiles symlinks to `crcore.c` and two Metal files. Neither goes through
  `CMakeLists.txt`, so neither gets its backend selection, definitions or
  warnings. Each is a partial carrottus that nothing tests.
- **They have drifted.** `bindings.rs` declares the Carrottus GPU descs without
  `base`, so its layouts have been wrong since 2026-10-19. Neither binding
  has been changed since 2026-05-20.

If a caller in another language ever needs carrottus, what serves it is a
separate repository of its own: a `-sys` crate whose bindgen pass reads the
real headers at build time, over a carrottus built by CMake, or a Swift module
map over the headers. Neither lives here.

**Breaks:** nothing.

**Done:** `bindings/` deleted, with `CLAUDE.md`'s "Rust bindings" section,
`README.md`'s "Bindings" section and the Rust and Swift build folders in
`.gitignore`. `STRUCT_BASE_TODO.md`'s Carrottus GPU record still mentions the
bindings as they were then.

## Done outside the plan: Carrottus Input's dispatch (2026-10-20)

Not one of the items, but it changes the files item 2 touches. Any number of
native input APIs may now be in one build (GameInput and XInput on Windows,
once XInput is written), and the caller's `native_api` chooses: the front end
has one `switch` per backend verb on it, the backends are part of
`src/crinput.c`'s translation unit (included there, never compiled alone),
and each verb is `crinput_<api>_<verb>`, forced inline into its case with
`CRINPUT_BACKEND_FUNCTION`. `CLAUDE.md`'s Carrottus Input paragraph and
`src/input/crinputshared.h` have the rule.

Built and tested on macOS only (Debug, Release, `-asan`, the Xcode
generator; every caller in the workspace against it). **Still to do:** build
`windows-d3d12` (the GameInput backend, and the `/w14714 /we4714` flags) and
`linux-vulkan` (evdev), and run `crinput_test` on each.

## 2. Convert Carrottus Input to struct bases (done 2026-10-20)

Done as `STRUCT_BASE_TODO.md` classifies it; that file's "Carrottus Input,
converted" is the record of what changed and what was run. The nine
`crinput.h` entries are out of `check.baseline`. **Still to do:** run
`crinput_test` once with a gamepad connected, which is when
`crinput_gamepad_get_properties`' three struct-base refusals run.

**Proposed alongside it, as its own change:** a query of whether a
native API built in will start on this machine (GameInput's runtime may be
missing), such as `crinput_native_api_is_available(api)`, so the caller can
choose before `crinput_instance_create` fails with
`CR_RESULT_API_SPECIFIC_FAILURE`. **Decided 2026-10-28 with the user
(card S8): add it.** It reports whether the platform will start that API
on this machine (GameInput's runtime present or not), as adapter listing
reports what is there, and never picks a fallback: which API to use is the
caller's. Its own change; needs a Windows run. Not started.

## 3. What every module shares: version, result names, allocator, policy

Begin with a proposal for the `crcore.h` additions and the policy text,
and write code only once it is agreed.

Long-lived C APIs have these and carrottus does not:

- **A version, at compile time and at run time: done (2026-10-20).**
  `ZLIB_VERNUM` and `zlibVersion()`, `SQLITE_VERSION_NUMBER` and
  `sqlite3_libversion_number()`, `LIBCURL_VERSION_NUM` and
  `curl_version_info()`, `VK_HEADER_VERSION` and
  `vkEnumerateInstanceVersion`. `crcore.h` has `CARROTTUS_VERSION_MAJOR`/
  `_MINOR`/`_PATCH` (0.1.0), `CARROTTUS_VERSION`, Vulkan's packing
  (`CARROTTUS_MAKE_VERSION`, 10/10/12 bits, no variant) and
  `cr_get_version()`; `CMakeLists.txt` reads the version from the header.
  One version for the library, no version string, and no caller refused for
  its version. The header's "Version" group has what major, minor and patch
  promise from 1.0.0, which belongs with the stability policy below. Still
  open: tags (none yet), and a `check.sh` check that a `vX.Y.Z` tag matches
  the header, once there are tags.
- **A name for each result: done (2026-10-20).** Vulkan's `string_VkResult`
  (`vk_enum_string_helper.h`) and `curl_easy_strerror`. carrottus already had
  it as `cr_result_str()` (`crcore.h`), which keeps its name; it was
  missing 11 results (Carrottus GPU's six heap and placement results, Carrottus Files'
  1003-1007), which read as "CR_RESULT_UNKNOWN". Every result has a case (157 as of 2026-10-22),
  and the switch has no `default`, so clang's `-Wswitch` (on by default)
  and GCC's (in `-Wall`) report a result added without one. MSVC's C4062
  is off by default, so a Windows-only change is not caught.
- **The caller's allocator: done (2026-10-21; Carrottus Files 2026-10-22).**
  `VkAllocationCallbacks`, `SQLITE_CONFIG_MALLOC`, `lua_Alloc`, zlib's
  `zalloc`/`zfree`. `crcore.h`'s "The caller's allocator" group has
  `CR_ALLOCATOR` (frozen: `user_data`, `allocate(size, alignment)`,
  `free(memory, size, alignment)`, sized and aligned both ways, so an arena
  or pool needs no header per block), the contract, and
  `cr_allocator_system()`, the system heap by name. There is no default:
  a create call refuses a NULL allocator (`CR_RESULT_NULL_ALLOCATOR`, 15)
  and one missing a function (`CR_RESULT_INVALID_ALLOCATOR`, 16).
  - Carrottus GPU, Carrottus Audio, Carrottus Net and Carrottus Input each take it as a required
    `allocator` field of a based instance desc (`crgpu_instance_desc`,
    `CRAUDIO_INSTANCE_DESC` and `crnet_instance_desc` are new;
    `crgpu_instance_create`'s four loose parameters moved into its desc).
    The first checks of each create are written once per module.
  - Every handle starts with a `cr_object_head` (`src/crallocator.h`):
    its debug name and its own copy of the allocator, so that a handle
    freed after its instance (a Carrottus Net resolver; perhaps WASAPI's device
    watch) still has it. `cr_debug_name_set` copies through it.
  - Every allocation of every backend goes through it, freed with its own
    size and alignment. `tests/crtest_allocator.h` checks that in
    `crinput_test` and `crnet_test`.
  - Left: the D3D12 backend's
    `std::vector`/`std::string`; memory the platforms and drivers allocate
    themselves (Carrottus GPU passes Vulkan no `VkAllocationCallbacks`).
  - Which threads call it is in each module's header: a Carrottus Audio stream's
    callback thread never does, its device watch does.
  - Carrottus Memory's arena is out of Carrottus: done (2026-10-22). It decided
    alignment, growth and zeroing, and its alignment macro, destroy
    (`free` on mapped memory) and create (no check of the reservation; a
    zero commit size divided by zero later) were broken. Nothing but
    a caller's log line used it, so it went rather than moving to another
    repository. Carrottus Memory is `crmem_get_page_size`, `crmem_reserve`,
    `crmem_commit`, `crmem_decommit` and `crmem_release`, each returning a
    `CR_RESULT`, with a POSIX backend for Linux and Apple
    (`src/memory/backends/posix/`) beside Windows'.
    `tests/crmemory_test` passes on macOS, plain and under ASan and
    TSan, and on Windows 11 (MSVC, Debug, 2026-10-22), and the
    POSIX one has not been compiled on Linux.
    **Decided 2026-10-27 with you (card X1b, line 2):** the shape is kept,
    with two changes. The prefixes are spelled out: Carrottus Memory's `smem_` is
    `crmem_` (`CRMEM_`, `CR_RESULT_MEM_*`) and Carrottus Files' `sfs_` is
    `crfiles_` (`CRFILES_`, `CR_RESULT_CRFILES_*`), every caller
    renamed in the same change (every caller in the workspace). And
    `crmem_get_reserve_granularity()` reports where a reservation starts
    (Windows' 64 KiB `dwAllocationGranularity`; the page size on POSIX), as
    Unreal's `GetVirtualSizeAlignment` does, so a caller can size small
    reservations without stranding address space; nothing is rounded for
    it. Run on macOS (2026-10-27); the Windows backend's new call is
    written, not compiled.
- **A stated stability and threading policy.** `CLAUDE.md` says a released
  layout never changes, but nothing says what "released" is: there are no
  tags or versions, and carrottus is STATIC only, pinned by commit in each
  caller's `deps.lock`, so in practice only source compatibility holds. Write
  down what is promised and from which version (as SQLite and Vulkan do), and
  per call what may be called from which thread at once (Vulkan's "externally
  synchronized"); today only about 15 places in the headers say.
  **Decided 2026-10-27 with you (card X2):** per-library semantic versions
  (`vX.Y.Z` tags, checked against `CARROTTUS_VERSION_*` from 1.0); before 1.0
  a minor version may break, each break listed against the ABI snapshot;
  from 1.0 released layouts are frozen and grow by chained structs, enums
  and results append-only. carrottus stays static-only until a shared build
  is needed, which keeps the next bullet waiting. Workspace releases are
  dated tags on eqdev, never on carrottus. The rules are in the root
  `CLAUDE.md`, "Versions and releases". **The threading half, decided
  2026-10-27 (card X3):** every object is used by one caller at a time
  (serialization, not thread identity), stated once in each header; the
  exceptions carry a `Thread:` line; Debug builds detect overlapping calls
  on one object. Carrottus creates no threads but what a platform imposes (an
  audio callback; the network stacks' own). Root `CLAUDE.md`, "Threading".
- **The platform's own error behind `CR_RESULT_API_SPECIFIC_FAILURE`.
  Decided 2026-11-01 with the user (card S10), revised the same day:**
  card S2's message channel, not a record per object. Every module with an
  instance takes one `CR_MESSAGE_CALLBACK` and its user data in its
  instance desc (NULL for none; required with Carrottus GPU's validation on);
  every failure a platform call reports is a message, whatever
  `CR_RESULT` it maps to (ENOENT and ENOTDIR both behind `NOT_FOUND` are
  told apart; the caller filters on the result, Carrottus judges nothing too
  routine; not would-block, a wait's timeout, an orderly close or a time
  limit Carrottus keeps), at every validation level, sent
  inside the failing call on the caller's thread before it returns, with
  the call's name, the object and its debug name, the `CR_RESULT`, the
  platform's numbering (`CR_PLATFORM_ERROR_SOURCE`, Carrottus Net's enum moved
  to `crcore.h`, `VKRESULT` added) and code, and the platform's own
  wording whole where it has some. The callback may be called from several
  of the caller's threads at once, never from Carrottus's, must not call the
  module whose call is reporting back, and reads the message only during
  the call; nothing is allocated. An object whose platform reports on its
  own thread (a Carrottus Audio stream) keeps one lock-free pending slot (a 64-bit and a 32-bit atomic, never a
  16-byte one), holding
  the first failure, sent by the next call on it with a count of the rest.
  Severities are every platform layer's (VERBOSE to D3D12's CORRUPTION),
  none merged. The callback is Carrottus's only output: its ~30 prints go
  (platform failures to messages, the D3D12 debug layer's printer and the
  live-object report to S2's), all but `CR_STALE`'s Debug line, which
  has no instance to reach. A create's message
  also names the object it was making. Messages go to the instance's
  creator only; a caller handed just an object reports the Carrottus call
  and its code, the detail being the message before it in the creator's
  log (accepted). Carrottus Memory's four calls and `crrandom_fill`, with no
  instance, take `CR_PLATFORM_ERROR *out_error` (result, source, code).
  Carrottus Net keeps its per-object records and also sends its failures. S2's
  text changes in one detail: the callback is accepted with validation off.
  Rejected on the way: a record and a `_get_error` on every object that can
  fail (the first answer), and one generic getter over `void *`. Draft
  `cr_platform_error_draft.h`, approved 2026-11-01. Then a message at
  every platform failure (the ~190 behind `API_SPECIFIC_FAILURE` and those
  mapped to codes of their own), Carrottus's prints removed, the tests it
  lists, and runs
  on macOS, Windows and Linux. Not built.
- **`void` recording calls that silently record nothing** (found reading
  card S10's draft, 2026-11-01). Each prints why and returns: out of memory
  building barriers (D3D12's `crgpu_command_list_pipeline_barrier`,
  `_image_barrier`, `_image_barrier_subresource` twice,
  `_acceleration_structure_barrier`), a scaling blit
  (`crgpu_command_list_blit_image`, Metal and D3D12), an unrecognized
  `CRGPU_IMAGE_LAYOUT` (Metal's three image barriers). Each is made
  unable to fail (room made beforehand, as the acceleration structure
  build's checks do; an enum checked where the caller can be refused) or
  refused with a code; the print goes. A change to public calls: a draft
  first. Not started.
- **Before any shared build of carrottus** (not needed while it is static
  only): an export macro, default-hidden symbols, a `SOVERSION`, and symbol
  versioning or a `.def` file.

**Breaks:** nothing, if the allocator is optional and chained or added with
the struct bases.

## 4. Rework Carrottus Files: done (2026-10-22)

The proposal was agreed and built; `CRFILES_TODO.md` has the record.

The largest of the four, and the one to design before writing: begin the
session with a proposal for the new `crfiles.h`, and write code only once it
is agreed. The plan is in `CRFILES_TODO.md`: what is wrong now, what the
callers use, the proposal to start from, and what it breaks.
