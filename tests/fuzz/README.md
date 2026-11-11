# Carrottus Net fuzz targets

libFuzzer targets over Carrottus Net's own parsers of bytes it doesn't control.
HTTP and WebSocket framing themselves (status lines, chunked bodies,
WebSocket frame headers, masking, fragmentation, close frames) are parsed by
the platform's stack - NSURLSession, libcurl, WinHTTP - not by Carrottus, so
what Carrottus parses, and what is fuzzed here, is what the stacks hand on:

| Target | Function (file) | Bytes from |
|---|---|---|
| `crnet_fuzz_http_head` | `crnet_head_add_header_line`, `crnet_head_add_header`, `crnet_head_get_header` (`src/net/crnethttp.c`); on Windows also `crnet_win_add_raw_headers` (`src/net/backends/windows/crnetwinhttp.c`) | the server: a response's header block, a line at a time (libcurl, WinHTTP) or as pairs (NSURLSession), and on Windows as WinHTTP hands over the whole block, UTF-16 |
| `crnet_fuzz_certificate` | `crnet_certificate_public_key_info` (`src/crnet.c`) | the server: its certificate's DER, walked to the key a pin is checked against (WinHTTP, NSURLSession) |
| `crnet_fuzz_text` | `crnet_utf8_valid`, `crnet_error_record`, `crnet_ascii_equal_no_case`, `crnet_url_has_scheme`, `crnet_headers_accept_encoding` | the caller or the stack: text checked before a WebSocket send, a stack's message cut to fit a record |
| `crnet_fuzz_proxy` | `crnet_proxy_parse` (`src/crnet.c`) | the caller: a proxy URL |

Each checks, besides AddressSanitizer's and UndefinedBehaviorSanitizer's
own findings, that nothing leaks or is freed wrongly through the caller's
allocator (with allocations refused part way, for the out-of-memory paths),
and that the answer matches a second, plainer reading of the same bytes. The
input layout of each is at the top of its file.

## Running

Clang with libFuzzer is needed: Apple's clang has none, Homebrew's LLVM
(`brew install llvm`) has, and so does clang on Linux. The presets build the
whole library with `-fsanitize=fuzzer-no-link,address,undefined`, Debug only,
in a binary dir of their own:

```sh
cmake --preset macos-fuzz            # or linux-fuzz
cmake --build --preset macos-fuzz-debug
mkdir -p /tmp/fuzz-head && cp tests/fuzz/corpus/crnet_fuzz_http_head/* /tmp/fuzz-head/
build-macos-fuzz/Debug/crnet_fuzz_http_head -max_total_time=1200 -max_len=4096 /tmp/fuzz-head
```

On Windows (2026-10-29), from a Visual Studio developer prompt with LLVM's
`bin` on `PATH` (LLVM 23.1.4 ran it):

```sh
cmake --preset windows-fuzz
cmake --build --preset windows-fuzz-relwithdebinfo
PATH="/c/Program Files/LLVM/lib/clang/23/lib/windows:$PATH"   build-windows-fuzz/RelWithDebInfo/crnet_fuzz_http_head -max_total_time=600 -max_len=4096 <scratch corpus>
```

It is LLVM's clang through its GNU-style driver, RelWithDebInfo at `-O1`
with asserts on, on the static release CRT, which clang's AddressSanitizer
and LLVM's prebuilt libFuzzer need on Windows, and without indirect-call
and comparison tracing, which break C++ exceptions there
(`../../../claude_made_decisions.md`, 2026-10-29). The runtime DLL beside
the clang version on the `PATH` above must be found to run a target.

Fuzz into a scratch copy of the corpus, not `corpus/` itself, which holds
only a few small, valid seeds. On macOS, LeakSanitizer reports a leak in
libFuzzer's own RSS thread when a run ends; pass
`LSAN_OPTIONS=suppressions=<file>` with `leak:fuzzer::StartRssThread` in it.

## When it finds something

1. Minimize the input: `<target> -minimize_crash=1 -runs=100000 crash-<hash>`.
2. Fix the cause in `src/`.
3. Keep the minimized input as `regressions/<target>/<what-it-was>`.

Every build with `CARROTTUS_BUILD_TESTS` (the default) builds each target a
second time with `crfuzz_replay.c`'s plain `main` and registers it with
ctest as `<target>_replay`, fed every file in `corpus/<target>` and
`regressions/<target>`. That runs on every platform and compiler, libFuzzer
or not.
