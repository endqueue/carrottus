# Struct bases TODO

Planned on 2026-10-18. Steps 1 and 2 of "Order of work" are done (the
`crcore.h` pieces, and the three ABI bugs), and step 3 is done for
Carrottus GPU, Carrottus Audio and Carrottus Net (2026-10-19, "Carrottus GPU, converted" and the
sections after it), for Carrottus Input (2026-10-20, "Carrottus Input, converted") and
for Carrottus Files (2026-10-22, with its rework). Every module is converted. Every extensible struct in
carrottus's public headers starts with a `CR_STRUCT_BASE`: a type tag and a
pointer to a chain of further structs. A struct's layout is frozen once it is
released. A new field never goes into a released struct; it comes as a new
struct that the caller chains on. This is Vulkan's `sType`/`pNext` pattern
(`VkBaseInStructure`), in the nested form that `webgpu.h` uses.

## Why

- **Binary compatibility.** A caller built against one version of these headers
  keeps working against a later carrottus, and a caller built against a later
  version gets a refusal from an older carrottus, never a misread struct. This
  matters once carrottus ships as a binary built separately from its caller.
  While carrottus and its caller are built together from source, nothing can
  break this way.
- **Layouts never move.** Arrays of structs, structs held by value and
  structs mirrored in other languages all keep one layout forever. A trailing
  `struct_size` field, the other common scheme (Win32's `cbSize`), breaks all
  three, and it can't tell a wrong struct from a short one.
- **A wrong struct is caught.** `base.type` says what the struct is, so a
  pointer to the wrong desc is refused by its type.
- **Additions are independent.** Two unrelated additions to one desc are two
  chained structs, used separately or together. They are not one ever-growing
  tail that has to be filled in order.

## Design

In `include/crcore.h`:

```c
/* Which struct a based struct is. Numbered in blocks per module, as
 * CR_RESULT is, and never renumbered. 0 is never valid, so a struct left
 * zeroed is refused rather than misread. */
typedef enum CR_STRUCT_TYPE {
	CR_STRUCT_TYPE_UNKNOWN            = 0,
	/* Carrottus GPU: 1000+ */
	CR_STRUCT_TYPE_GPU_IMAGE_DESC     = 1000,
	/* ... */
	/* Carrottus Audio: 2000+ */
	/* Carrottus Net: 3000+ */
	/* Carrottus Input: 4000+ */
	/* Carrottus Files: 5000+ */
	CR_STRUCT_TYPE_MAX_ENUM           = 0x7FFFFFFF
} CR_STRUCT_TYPE;

/* First member of every extensible struct the caller fills in. */
typedef struct CR_STRUCT_BASE {
	CR_STRUCT_TYPE type;               /* this struct's own type */
	const struct CR_STRUCT_BASE *next; /* the next struct in the chain, or NULL */
} CR_STRUCT_BASE;

/* First member of every extensible struct carrottus fills in for the caller
 * (properties, capabilities, requirements). The caller builds the chain and
 * sets each type; carrottus writes the fields. */
typedef struct CR_STRUCT_BASE_OUT {
	CR_STRUCT_TYPE type;
	struct CR_STRUCT_BASE_OUT *next;
} CR_STRUCT_BASE_OUT;
```

A desc, and an addition chained onto it:

```c
typedef struct crgpu_image_desc {
	CR_STRUCT_BASE base;   /* type = CR_STRUCT_TYPE_GPU_IMAGE_DESC */
	CRGPU_IMAGE_USAGE usage;
	/* ... */
	const char *debug_name;  /* always present, NULL for none */
} crgpu_image_desc;

/* An example of a later addition, not a planned one. */
typedef struct crgpu_image_multisample_desc {
	CR_STRUCT_BASE base;   /* type = CR_STRUCT_TYPE_GPU_IMAGE_MULTISAMPLE_DESC */
	uint32_t sample_count;
} crgpu_image_multisample_desc;
```

The caller's side:

```c
crgpu_image_multisample_desc ms = {
	.base = { CR_STRUCT_TYPE_GPU_IMAGE_MULTISAMPLE_DESC, NULL },
	.sample_count = 4,
};
crgpu_image_desc desc = {
	.base = { CR_STRUCT_TYPE_GPU_IMAGE_DESC, &ms.base },
	/* ... */
};
```

carrottus's side: check the root's type, then walk the chain:

```c
if (desc->base.type != CR_STRUCT_TYPE_GPU_IMAGE_DESC)
	return CR_RESULT_STRUCT_WRONG_TYPE;
const crgpu_image_multisample_desc *ms = NULL;
for (const CR_STRUCT_BASE *s = desc->base.next; s; s = s->next) {
	switch (s->type) {
	case CR_STRUCT_TYPE_GPU_IMAGE_MULTISAMPLE_DESC:
		if (ms)
			return CR_RESULT_STRUCT_DUPLICATE_IN_CHAIN;
		ms = (const crgpu_image_multisample_desc *)s;
		break;
	default:
		return CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN;
	}
}
```

## Rules

1. **Every public struct is based or frozen.**
   - **Based:** its first member is `CR_STRUCT_BASE base;` or
     `CR_STRUCT_BASE_OUT base;`.
   - **Frozen:** the line above its `typedef` is a `/* Frozen: <why> */`
     comment. A frozen struct never changes; a new need gets a new type.
2. **Every layout is fixed once released.** That holds for based structs too:
   no field is added, removed, reordered or resized. Additions are chained
   structs.
3. **The layout is the same in every build configuration.** No field sits
   inside `#if _DEBUG` or any other build switch.
4. **A based struct is never held by value inside another struct.** Its own
   chain would be a second chain, which nothing walks. Either it is frozen,
   or it is a chained struct on the outer struct's chain, or the outer
   struct points to it.
5. **The member is always named `base`, and the cast is always through it.**
   C guarantees that a pointer to a struct converts to a pointer to its first
   member and back (C11 6.7.2.1p15), so walking the chain is defined
   behavior. Vulkan's flat `sType`/`pNext` fields would instead rely on
   casting between unrelated structs that share their first fields. A flat
   `type` field would also clash with the existing `type` fields
   (`crgpu_image_desc::type`, `CRFILES_FILE_DIALOG_ATTRIBUTES::type`).
6. **Anything unknown in a chain is refused, never skipped.** Skipping it
   would quietly decide for the caller that the addition didn't matter.
   Vulkan leaves this to its validation layers; carrottus refuses at every
   call, in every build. A type that appears twice in one chain is refused
   too.
7. **There are no defaults.** Leaving a chained struct out means that
   addition isn't used. The chained struct's fields follow the same
   no-default rule as every desc.
8. **Type numbers are permanent.** They are assigned in their module's block
   and never reused, and a struct that is retired keeps its number.

New result codes in `crcore.h`, in the generic block after
`CR_RESULT_NULL_OBJECT` (11):

| Code | When |
|---|---|
| `CR_RESULT_STRUCT_WRONG_TYPE` (12) | A struct's `base.type` is not the type the call takes (including 0) |
| `CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN` (13) | A chained struct's type is not one the call accepts in that chain, or not known to this build |
| `CR_RESULT_STRUCT_DUPLICATE_IN_CHAIN` (14) | One type appears twice in a chain |

## Which structs get which

This classification is proposed. Confirm each struct as it is converted. The
rule of thumb is: based if it may plausibly grow; frozen if it is a value, a
fixed fact of a platform or GPU format, an element of a hot per-frame array,
or held by value elsewhere. Line numbers are as of 2026-10-18.

### crgpu.h (converted 2026-10-19, as listed here)

The list changed with the adapters-and-devices change (2026-10-21,
`CRGPU_DEVICE_TODO.md`): `CRGPU_DEVICE_PROPERTIES` and the three
`*_capabilities` structs went, and the instance and device descs, the
adapter properties and the two limits structs came. Below is the list as
of 2026-10-22: 22 caller-filled, 4 Carrottus GPU-filled.

- **`CR_STRUCT_BASE` (caller fills):**
  - `crgpu_instance_desc`
  - `crgpu_device_desc`
  - `crgpu_submit_desc`
  - `crgpu_command_allocator_desc`
  - `crgpu_buffer_desc`
  - `crgpu_buffer_view_desc`
  - `crgpu_image_desc`
  - `crgpu_image_view_desc`
  - `crgpu_heap_desc`
  - `crgpu_swap_chain_desc`
  - `crgpu_descriptor_set_layout_desc`
  - `crgpu_descriptor_pool_desc`
  - `crgpu_descriptor_set_desc`
  - `crgpu_sampler_desc`
  - `crgpu_write_descriptor_set`
  - `crgpu_shader_module_desc`
  - `crgpu_graphics_pipeline_desc`
  - `crgpu_compute_pipeline_desc`
  - `crgpu_rendering_begin_desc`
  - `crgpu_bundle_inheritance_desc`
  - `crgpu_buffer_barrier`
  - `crgpu_image_barrier`
- **`CR_STRUCT_BASE_OUT` (carrottus fills):**
  - `CRGPU_ADAPTER_PROPERTIES`
  - `crgpu_placement_requirements`
  - `CRGPU_BINDLESS_LIMITS`
  - `CRGPU_MESH_SHADING_LIMITS`
- **Frozen:**
  - **Array element, held by value in `CRGPU_ADAPTER_PROPERTIES`:**
    `CRGPU_QUEUE_POOL`.
  - **Values:** `CRGPU_DEPTH_STENCIL_VALUE` and `CRGPU_CLEAR_VALUE`
    (both held by value), `CRGPU_VIEWPORT`, `CRGPU_SCISSOR_RECT`.
  - **Held by value, or array elements:**
    `crgpu_image_view_desc_subresource_range`,
    `crgpu_descriptor_binding_desc`, `crgpu_descriptor_pool_size`,
    `crgpu_descriptor_buffer_info`, `crgpu_descriptor_image_info`,
    `crgpu_vertex_attribute_desc`, `crgpu_vertex_binding_desc`,
    `crgpu_push_constant_range_desc`, `crgpu_blend_attachment_desc`,
    `crgpu_color_attachment_desc` (an array in
    `crgpu_rendering_begin_desc`), `crgpu_depth_attachment_desc`.
  - **Copy regions:** `crgpu_image_buffer_copy_desc` and
    `crgpu_image_blit_desc`, as Vulkan's `VkBufferImageCopy` and
    `VkImageBlit` are.
  - **Read by the GPU, so the layout is the GPU's and can never carry a
    base:** `crgpu_draw_indirect_args`, `crgpu_draw_indexed_indirect_args`.

### crgpu_*_natives.h

All frozen: `CRGPU_D3D12_BUFFER_VIEW`, `CRGPU_D3D12_IMAGE_VIEW` and
`CRGPU_MTL_BUFFER_VIEW` are facts of the native API they expose.

### craudio.h (converted 2026-10-19, as listed here)

- **`CR_STRUCT_BASE`:** `CRAUDIO_STREAM_DESC`.
- **`CR_STRUCT_BASE_OUT`:** `CRAUDIO_DEVICE_PROPERTIES`,
  `CRAUDIO_TIMING_CAPS`, `CRAUDIO_STREAM_CONFIG`.
- **Frozen:** `CRAUDIO_FORMAT` (a value, held by value);
  `CRAUDIO_THREAD_DESC` (held by value in `CRAUDIO_STREAM_DESC`);
  `CRAUDIO_CALLBACK_INFO` (handed to the callback every period on the audio
  thread); `CRAUDIO_STREAM_STATUS` (polled).

### crnet.h (converted 2026-10-19, as listed here)

- **`CR_STRUCT_BASE`:** `crnet_resolver_desc`,
  `crnet_tcp_listener_desc`, `crnet_tcp_socket_connect_desc`,
  `crnet_http_client_desc`, `crnet_http_transfer_desc`,
  `crnet_websocket_connect_desc`.
- **Frozen, held by value:** `crnet_tls_desc` and `crnet_proxy_desc`,
  in `crnet_http_client_desc` and `crnet_websocket_connect_desc`. See
  "Decisions", 2.
- **Frozen:** `crnet_error` (filled per call, like every error struct);
  `crnet_header`, `crnet_tls_certificate`, `crnet_tls_pin` (array
  elements); `crnet_wait_event` (an array carrottus fills on each wait).

### crinput.h (converted 2026-10-20, as listed here)

- **`CR_STRUCT_BASE`:** `CRINPUT_INSTANCE_DESC`.
- **`CR_STRUCT_BASE_OUT`:** `CRINPUT_GAMEPAD_PROPERTIES`.
- **Frozen:** `CRINPUT_GAMEPAD_STATE` and `CRINPUT_GAMEPAD_MOTION_SAMPLE`
  (polled every frame); `CRINPUT_GAMEPAD_TOUCH_STATE` and
  `CRINPUT_GAMEPAD_TOUCH_CONTACT` (held by value);
  `CRINPUT_GAMEPAD_TOUCH_SURFACE`, `CRINPUT_GAMEPAD_RUMBLE`,
  `CRINPUT_GAMEPAD_BATTERY`.
- Rumble and state stay frozen on purpose (confirmed 2026-10-20). Rumble has
  grown elsewhere (XInput's two motors, GameInput's four), and the answer
  there was a new struct and call, as here: a chained motor struct left out
  would need a meaning, unchanged or stopped, which is a default. A new
  button needs no new struct: bits 17-31 of `CRINPUT_GAMEPAD_BUTTONS` are
  free, and a new reading is a new getter.

### crfiles.h

Converted 2026-10-22, with the rework (`CRFILES_TODO.md`); the structs
planned here went with the old API.

- **`CR_STRUCT_BASE`:** `crfiles_instance_desc` (5001), `crfiles_open_desc` (5000).
- **`CR_STRUCT_BASE_OUT`:** `CRFILES_PATH_INFO` (5002).

## ABI bugs (fixed 2026-10-18)

Each was wrong whether or not the struct bases go in.

1. **Struct layouts differed between Debug and Release.** `const char
   *debug_name` sat inside `#if _DEBUG` in nine Carrottus GPU descs
   (`crgpu_buffer_desc`, `_image_desc`, `_heap_desc`,
   `_descriptor_set_layout_desc`, `_descriptor_pool_desc`, `_sampler_desc`,
   `_shader_module_desc`, `_shader_pipeline_desc`, `_compute_pipeline_desc`),
   so a caller and a carrottus built in different configurations read every
   field after it at a different offset. The field is now always present.
   Names are kept in every build: each handle's `_debug_name` member and
   every copy and free of it are unconditional, in all three backends and
   every module (212 `#if _DEBUG` blocks removed, 20 split so that only
   the placement count and the open-list checks stay debug-only).
   `CLAUDE.md`'s rule is now "A debug name is copied ... in every build".
   What stays debug-only is platform debug tooling, which touches no layout:
   Vulkan's `vkSetDebugUtilsObjectNameEXT` calls and D3D12's info-queue
   callback.
2. **Functions existed only in Debug builds.** `cr_debug_name_set`,
   `cr_debug_name_get` and their macros were declared only under
   `#ifdef _DEBUG`. Now always.
3. **Outputs lived inside input descs.** `gpu_address_location` is gone
   from `crgpu_buffer_desc` and `crgpu_image_desc` (an image never had
   one: it was always 0). A buffer's address comes from the new
   `crgpu_buffer_get_gpu_address(buffer, &address)`, in all three
   backends.

Run on macOS (Metal), Debug and Release: carrottus's tests, Carrottus Net's 261
checks plain and under ASan+UBSan, and every caller in the workspace
built against it with its tests passing. The Vulkan and D3D12 backends were not
compiled (no headers on this Mac); their changes remove preprocessor lines
or move the address, and Release builds now carry what Debug builds always
did. Windows and Linux builds are due.

## Decisions (were open questions)

Recommended and confirmed on 2026-10-18.

1. **Getters that return a desc.** `crgpu_buffer_get_desc`,
   `_get_buffer_view_desc`, `_get_image_desc`, `_get_image_view_desc`,
   `_get_heap_desc`, `_get_swap_chain_desc` and `_get_compute_pipeline_desc`
   write an input struct back out, whose `const` chain can't be filled.
   - The getter writes the whole root struct, `base` included: `base.type`
     is the struct's own type and `base.next` is NULL. The caller sets
     nothing first. This is D3D12's `ID3D12Resource::GetDesc`: a fixed
     struct back, and a new getter for anything added later.
   - Chained additions the object was made with don't come back through
     it. One comes back through a getter of its own, written when a caller
     needs it, never before.
   - `debug_name` in the returned desc is the handle's own copy. It is
     valid until the object is destroyed or renamed, and NULL in a build
     that keeps no names. The getter's comment says so. The caller copies
     it if it needs it longer.
   - `gpu_address_location` is no longer in the returned desc (ABI bug 3,
     fixed).
2. **TLS and proxy settings: stay held by value, frozen.** Making them
   chained structs would make leaving one out mean
   `CRNET_TLS_TRUST_SYSTEM` or `CRNET_PROXY_SYSTEM`. That is a default
   that hides a choice, which rule 7 and Carrottus's first rule forbid.
   - Held by value, every desc states its trust and proxy mode in fields
     that are always there.
   - Zero is the named enumerator `_SYSTEM`, a value the caller chose,
     not an absence that carrottus fills in.
   - Anything TLS or proxies gain later (a client certificate, a minimum
     TLS version) is a new struct chained onto the HTTP client or
     WebSocket connect desc. `crnet_tls_desc` itself never grows.
3. **Static initializers: no.**
   - C's designated initializers already say it in one line:
     `.base = { CR_STRUCT_TYPE_GPU_IMAGE_DESC, NULL }`.
   - A macro would be a second spelling of the same thing.
   - Swift doesn't import macros like these, and C# never sees C macros,
     so the bindings can't share them.
   - A macro is one step from carrying a default. `webgpu.h`'s
     `WGPU_*_INIT` macros fill in default values, which is the policy
     carrottus refuses.
   - Vulkan has none.

## Carrottus GPU, converted (2026-10-19)

- `crcore.h`: `CR_STRUCT_TYPE_GPU_*` 1000-1024, one per based struct,
  in header order.
- `crgpu.h`: the 20 caller-filled structs above have a
  `CR_STRUCT_BASE base` first, the 5 Carrottus GPU-filled ones a
  `CR_STRUCT_BASE_OUT base`, and every other struct and union (the
  natives headers' too) a `/* Frozen: ... */` line above its `typedef`.
  Each function that takes a based struct documents the two results for it;
  the void ones (`crgpu_command_list_pipeline_barrier`, `_update_descriptor_sets`,
  `_cmd_begin_rendering`, `_begin_bundle_command_list`) say they record
  nothing for a wrong base, as for their other invalid arguments.
- `src/crstructbase.h` (new): `cr_struct_check`, `cr_struct_out_check`,
  and the bases a struct handed back has. Every backend calls them, right
  after the null check of the struct's parameter, so the results and their
  order are the same on all three (111 checks across Vulkan, D3D12 and
  Metal; an array, a barrier or descriptor write, is checked element by
  element).
- Getters: `crgpu_get_*_desc` set the returned desc's whole base
  (Decision 1); the device-properties and capability getters that copy or
  clear the whole struct put the caller's base back after.
- carrottus's own calls into its API (the `_synced` submit, the clear and
  depth helpers that begin rendering) set the base like any caller.

Run on macOS (Metal), 2026-10-19: carrottus Debug and Release with its tests,
Carrottus Net's 261 checks, and the ASan+UBSan preset; every caller in the
workspace built against it with its tests passing, the GPU tests under
Metal validation, and every Metal sample for 120 frames under Metal API
and shader validation, exiting cleanly. The same day, on Windows, the
Vulkan and D3D12 backends built and a caller's GPU tests passed on both
(reported). Not yet run: Linux (Vulkan), and the samples, and the callers
that build only for Windows and Linux, on either. The Rust bindings and the Swift
package were already out of step with the C API and are not brought to it
(the Swift package did not build before either); their device-properties
and buffer-desc uses set the base.

## Carrottus Audio, converted (2026-10-19)

- `crcore.h`: `CR_STRUCT_TYPE_AUDIO_*` 2000-2003.
- `craudio.h`: `CRAUDIO_STREAM_DESC` has a `CR_STRUCT_BASE base`;
  `CRAUDIO_DEVICE_PROPERTIES`, `_TIMING_CAPS` and `_STREAM_CONFIG` a
  `CR_STRUCT_BASE_OUT base`; `CRAUDIO_FORMAT`, `_THREAD_DESC`,
  `_CALLBACK_INFO` and `_STREAM_STATUS` are marked frozen. The functions
  that take them document the two results.
- The stream desc is checked once, in `craudio_validate_stream_desc`
  (`src/craudio.c`), which every backend's probe and create call. Each
  output struct is checked in every backend (CoreAudio, WASAPI, ALSA, the
  stub) right after its null check, before Carrottus Audio zeroes it as it
  always has; its base is put back after anything that zeroes or copies the
  whole struct, so a refused probe leaves the caller's base too.

Run on macOS, 2026-10-19: carrottus Debug, Release and ASan+UBSan with its
tests; a throwaway CoreAudio program (properties, timing caps, probe,
create without starting, get config; a zeroed, a mistyped and a chained
base each refused with the right result) passed; every caller in the
workspace builds and passes its tests. WASAPI and ALSA are written, not
compiled.

## Carrottus Net, converted (2026-10-19)

- `crcore.h`: `CR_STRUCT_TYPE_NET_*` 3000-3005.
- `crnet.h`: the six descs above start with a `CR_STRUCT_BASE base`;
  `crnet_error`, `_header`, `_tls_certificate`, `_tls_pin`, `_tls_desc`,
  `_proxy_desc` and `_wait_event` are marked frozen. Each of the six
  functions that take a desc documents the two results. Carrottus Net has no
  struct it fills that may grow, so no `CR_STRUCT_BASE_OUT`.
- Every backend checks the desc right after its null check: the BSD
  sockets and Winsock files (resolver, listener, connect), NSURLSession,
  libcurl and WinHTTP (HTTP client, WebSocket connect), and the shared
  HTTP front end, `src/net/crnethttp.c` (transfer start). libcurl's
  WebSocket stub for curl before 8.11 refuses every call before it reads
  the desc, as before.
- `tests/crnet_test.c` sets every desc's base after its `memset`, and has
  a new part, "Struct bases" (`bases`): each of the six calls refused for a
  zeroed base, another desc's type and a chained struct, before any
  connection is made.

Run on macOS, 2026-10-19: carrottus Debug and Release with its ctest suites;
`tests/run_crnet_test.sh` passes all 279 checks (261 before, 18 new)
plain, under ASan+UBSan and under TSan; every caller in the workspace
builds and passes its tests. Winsock, libcurl and WinHTTP are written,
not compiled.

## Carrottus Input, converted (2026-10-20)

- `crcore.h`: `CR_STRUCT_TYPE_INPUT_INSTANCE_DESC` = 4000 and
  `CR_STRUCT_TYPE_INPUT_GAMEPAD_PROPERTIES` = 4001.
- `crinput.h`: `CRINPUT_INSTANCE_DESC` has a `CR_STRUCT_BASE base`,
  `CRINPUT_GAMEPAD_PROPERTIES` a `CR_STRUCT_BASE_OUT base`, and the
  other seven are marked frozen. Both functions document the two results.
- Both checks are in the front end, `src/crinput.c`, once: every argument
  check is there and the backends only fill its records, so no backend
  changed. `crinput_instance_create` checks right after `if (!desc)` and
  keeps its copy of the desc with `next` cleared;
  `crinput_gamepad_get_properties` checks right after its null check
  (so after the gamepad's), and puts the base back after copying the record.
- `tests/crinput_test.c` sets every base, and refuses a zeroed base,
  another struct's type and a chained struct for both calls. Properties
  come after the gamepad in the order of checks, so their refusals run only
  with a gamepad connected; the test says when they are skipped.

Run on macOS, 2026-10-20, with no gamepad connected: carrottus Debug,
Release and ASan+UBSan with its ctest suites; every caller in the workspace
builds and passes its tests; `check.sh struct-bases` has no Carrottus Input entry
left. Not run: the properties refusals (no gamepad here), and the GameInput
and evdev builds, which needed no change but have not been compiled.

## Order of work

1. **Done.** Write the `crcore.h` pieces and the three result codes, with
   `cr_result_str` names. `CR_STRUCT_TYPE` has no struct types yet;
   each is added with its struct.
2. **Done.** Fix the three ABI bugs first, in their own change. Each is a fix on its
   own and makes the conversion smaller.
3. **Done: Carrottus GPU, Carrottus Audio, Carrottus Net, Carrottus Input and Carrottus Files.** Convert one module at a time, header and every backend together:
   Carrottus GPU, then Carrottus Audio, Carrottus Net, Carrottus Input, Carrottus Files. Bring
   `spudgpusamples`, the tests and every caller along in the same change.
   This is a source break for every caller.
4. After each module:
   - build on macOS and run the tests, plain and under the `-asan` preset;
   - run Carrottus Net's test (`tests/run_crnet_test.sh`) after Carrottus Net;
   - flag Windows and Linux runs as needed for the Carrottus GPU change. It touches
     every backend, so it warrants them.
5. **Done** (2026-10-20). Add the rule to the workspace's `check.sh` as a `struct-bases` check,
   covering rules 1, 3 and 4: every public `typedef struct` is based or
   marked frozen, nothing in a struct body sits under a build switch, and no
   based struct is held by value.
6. Rewrite this file's "Which structs get which" as a record of what was
   done, then shorten `CLAUDE.md`'s note to the rules and a pointer here.
