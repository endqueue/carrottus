# Carrottus

One C interface over the GPU (Vulkan, D3D12, Metal), files, memory, network,
audio and input on Windows, Linux, macOS and iOS.

Carrottus is **pure mechanism, zero policy**. It translates each platform API into
one consistent C interface and stops there. It never makes a decision on the
caller's behalf, has no default that hides a choice, and holds no global state.
It lists what the platform offers (`crgpu_adapter_enumerate`, not
`crgpu_select_best_device`) and the caller decides. That includes where
memory comes from: every instance takes the caller's `CR_ALLOCATOR`.

```
The caller                    ← every decision
        ↓ C-ABI
Carrottus                       ← you are here
        ↓
Native platform               ← Vulkan / D3D12 / Metal, Win32 / Linux / macOS / iOS
```

## Example

```c
#include <crgpu.h>
#include <stdio.h>
#include <string.h>

int main(void) {
	CR_ALLOCATOR allocator = cr_allocator_system();

	crgpu_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type        = CR_STRUCT_TYPE_GPU_INSTANCE_DESC;
	instance_desc.allocator        = &allocator;
	instance_desc.application_name = "hello";
	instance_desc.engine_name      = "hello";

	crgpu_instance instance;
	if (crgpu_instance_create(&instance_desc, &instance) != CR_SUCCESS)
		return 1;

	/* Carrottus lists the adapters; choosing one is the caller's. */
	const crgpu_adapter *adapters;
	uint32_t count;
	crgpu_adapter_enumerate(instance, &adapters, &count);
	for (uint32_t i = 0; i < count; i++) {
		CRGPU_ADAPTER_PROPERTIES properties;
		memset(&properties, 0, sizeof(properties));
		properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
		if (crgpu_adapter_get_properties(adapters[i], &properties) == CR_SUCCESS)
			printf("%u: %s\n", i, properties.description);
	}

	crgpu_instance_destroy(instance);
	return 0;
}
```

From there, `crgpu_device_create` takes the adapter the caller chose.
`spudgpusamples` has complete programs: triangles, textures, mesh shaders,
indirect execution, bindless.

## Module map

Every module reports errors through the shared `CR_RESULT` enum
(`crcore.h`) rather than inventing its own convention.

| Module | Header | Prefix | Backends |
|---|---|---|---|
| Carrottus GPU | `crgpu.h` | `crgpu_` | Vulkan, D3D12, Metal |
| Carrottus Files | `crfiles.h` | `crfiles_` | Windows, Linux, macOS |
| Carrottus Memory | `crmemory.h` | `crmem_` | Windows, POSIX (Linux + Apple, one file) |
| Carrottus Net | `crnet.h` | `crnet_` | TCP sockets: Winsock2, BSD sockets (Linux + Apple, one file). HTTP + WebSocket: WinHTTP, libcurl, NSURLSession |
| Carrottus Audio | `craudio.h` | `craudio_` / `CR_RESULT_SAUD_*` | WASAPI (Windows), ALSA (Linux), CoreAudio (macOS); an argument-checking stub where no API is compiled in (iOS, watchOS) |
| Carrottus Input | `crinput.h` | `crinput_` / `CR_RESULT_CRINPUT_*` | GameController (macOS, iOS, iPadOS), evdev (Linux), GameInput (Windows). Not written: XInput (Windows); with no API built in, no instance can be made |
| Carrottus Core | `crcore.h` | `cr_` / `CR_RESULT` | platform-agnostic |
| Carrottus Perf | `crperf.h` | `crperf_` | Windows, Linux, Apple |

Carrottus GPU is the largest module by far (133 functions across 24 opaque handles).
The rest are small, focused translation shims by design — e.g. Carrottus Net moves
bytes, it doesn't do message framing or host authority; Carrottus Memory is
reserve/commit/decommit/release of whole pages, not an allocator.

## Backend status

| Backend | Language | Status |
|---|---|---|
| Vulkan | C23 | Complete; the reference backend |
| D3D12 | C++26 | Complete; SPIR-V is cross-compiled to HLSL with SPIRV-Cross |
| Metal | Objective-C | Complete; SPIR-V is cross-compiled to MSL with SPIRV-Cross |

Every backend takes shaders as SPIR-V. A build gates optional API with
`CRGPU_EXT_*` macros, and at run time each adapter reports its own
`supported_features`. Neither is assumed from the platform's name.

## Building

Requirements: CMake 3.30+, a C23 compiler (C++26 for D3D12), the Vulkan SDK
for the Vulkan backend, libcurl and ALSA on Linux.

A build has exactly one graphics backend, set by the `GRAPHICS_BACKEND` cache
variable (`Vulkan` / `D3D12` / `Metal`). The presets set it for you. Calls go
straight to that backend, with no vtable in between.

```sh
cmake --preset macos-metal          # or windows-d3d12, windows-vulkan, linux-vulkan, iossimulator-metal
cmake --build --preset macos-metal-debug
ctest --test-dir build-macos-metal -C Debug
```

On Windows inside the eqdev workspace, build from the workspace root so every
project shares one `CMAKE_MSVC_RUNTIME_LIBRARY`. The `-asan` (AddressSanitizer +
UBSan) and `-tsan` presets, on macOS and Linux, build and test under the
sanitizers. `Doxyfile` generates the API reference from the headers.

## Tests

CTest runs eight tests, none of which opens a window: `crcore_test`,
`crgpu_device_test`, `crgpu_objects_test`, `crfiles_test`,
`crmemory_test`, `craudio_test`, `crperf_test` and `crinput_test`.
The two `crgpu_` tests need a GPU and are skipped without one. Carrottus Net's test needs a
local server and runs through `tests/run_crnet_test.sh`. Most of Carrottus GPU is
verified by `spudgpusamples` instead. [`CLAUDE.md`](https://github.com/endqueue/carrottus/blob/master/CLAUDE.md), "Known gaps",
records what each covers and what has run where.

## Contributing

Read [`CLAUDE.md`](https://github.com/endqueue/carrottus/blob/master/CLAUDE.md) before making structural changes. It covers the
zero-policy rule, `CRGPU_EXT_*` gating, struct bases and the backend file
layout. Format with the repo's `.clang-format`.

## License

[Apache License 2.0](LICENSE).
