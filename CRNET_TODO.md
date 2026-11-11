# Carrottus Net TODO

`include/crnet.h` has three parts: TCP sockets (`crnet_tcp_socket_*`,
`crnet_tcp_listener_*`), an HTTP client (`crnet_http_*`) and a WebSocket
client (`crnet_websocket_*`). The HTTP and WebSocket clients were written, and
the TCP sockets rewritten, on 2026-10-07. TLS control (`crnet_tls_desc`: custom
roots, pins, accepting any certificate) and platform error detail
(`crnet_error`, each object's `_get_error`) were added the same day, after the
review below, and so was the wait set (`crnet_wait_set_*`). None of the three
has been compiled on any platform.

On 2026-10-08 the header was corrected item by item ahead of the backends, and
the backends were then rewritten to match it in one pass. `crnet.h` and the
sources agree again as far as reading them can tell: every function the header
declares has one definition per platform, and none of the names it dropped is
left in the sources. It was then compiled on macOS and run there for the first time (see "State of
the code" and "First run on macOS"). Linux and Windows were each built and
run for the first time on 2026-10-14 (see "State of the code" and "First run
on Windows").

Below is what is still wrong with it or missing from it, as of that review.

## Instances (2026-10-08): run on macOS, written elsewhere

`crnet_startup` and `crnet_shutdown` are gone. In their place is a
`crnet_instance` (`crnet_instance_create` / `_destroy`), which the `_create`
calls that make an object from nothing take as their first argument. The header
was changed first and the backends and `tests/crnet_test.c` brought to it the
same day. **macOS: built with the `macos-metal` preset with no errors or
warnings, and `tests/crnet_test` passes all 261 of its checks, three runs in a
row** (238 before; the new ones are the "instance" part and the wait set's
wake). Linux and Windows: built and passing on 2026-10-14 (see "State of the
code").

Why: a library above Carrottus Net (a caller) could not make a once-per-process call
that has to come before the process's other threads, so the rule travelled up
through every layer to the host. An instance is each caller's own.

- **What an instance is** (`crnetshared.h`): one struct for every platform,
  holding nothing of Carrottus Net's. Each platform makes it where it starts its
  networking: `crnetwindows.c` (`WSAStartup` / `WSACleanup`), `crnetcurl.c`
  (`curl_global_init` / `curl_global_cleanup`), `crneturlsession.m` (nothing).
- **NULL is `CR_RESULT_NET_INVALID_INSTANCE`** (3033, new) from
  `crnet_resolver_create`, `crnet_tcp_listener_create`,
  `crnet_tcp_socket_create`, `crnet_http_client_create`,
  `crnet_websocket_create`, `crnet_wait_set_create` and
  `crnet_instance_get_local_ipv4_addresses`. `CR_RESULT_NET_STARTUP_FAILED` (3001)
  keeps its number and name.
- **A wait set refuses another instance's objects.** Sockets, listeners and
  WebSockets keep the instance they were made with (an accepted socket its
  listener's) and the set compares. An HTTP client and a resolver keep nothing:
  neither can go in a set.
- **A resolver on Windows has a `WSAStartup` of its own**, given back when the
  resolver is freed, because its lookup may outlast the instance. So
  `crnet_resolver_create` can now fail with `STARTUP_FAILED` there. On Linux
  and Apple `getaddrinfo` needs nothing started, so the resolver takes none; the
  header's wording was narrowed to say "where the lookup needs one".
- **Linux turns away a libcurl that isn't thread-safe:**
  `crnet_instance_create` returns `CR_RESULT_NET_UNSUPPORTED` unless
  `curl_version_info` reports `CURL_VERSION_THREADSAFE`. `CMakeLists.txt` asks
  for libcurl 7.84 and `crnetcurl.c` has an `#error` below it.
- **Carrottus Net has one piece of state now:** a static lock in `crnetcurl.c`, held
  across that check and the global init. `curl_version_info` is only safe before
  the first global init when the init is thread-safe, so without the lock two
  instances made at once on a libcurl that fails the check could collide in the
  asking. It is a few lines and guards nothing on a libcurl that passes.
- **The Linux TCP sockets need no libcurl**, but the instance is where libcurl
  starts, so on Linux they depend on a thread-safe libcurl too. The header says
  so. If that ever chafes, the fix is an instance that starts libcurl only when
  the first HTTP client or WebSocket is made.
- **Left in `crnetcurl.c`:** `#if LIBCURL_VERSION_NUM >= 0x075400` and
  `>= 0x075000` branches whose other halves can no longer be built. Harmless;
  worth removing when the file is next gone through.
- **`tests/crnet_test.c`** makes one instance in `main` and has a new part,
  "instance": a second instance beside the first, one made and destroyed on
  another thread while the first is in use, NULL refused by each `_create`, a
  wait set refusing another instance's listener, and a resolver destroyed before
  its instance and run after.

To confirm on a machine:

- macOS is done (above). What that run doesn't reach is the same as before:
  everything in it is on the loopback interface.
- **Windows:** the "instance" part passes (a second instance, one made on
  another thread, a resolver run after its instance was destroyed). Still not
  tried: that `WSACleanup` from an instance being destroyed leaves another
  instance's sockets working (Winsock's count is documented), and what a
  lookup in `GetAddrInfoExW` does when its instance's cleanup was the
  second-to-last.
- **Linux:** that the libcurl in use counts its global init as its documentation
  says, which several instances rely on; and which distributions the 7.84 floor
  shuts out (Ubuntu 22.04 and RHEL 9 are believed to ship older).

## State of the code

- **macOS has run.** `tests/crnet_test` passes all 238 of its checks against
  a local server (2026-10-08, three runs in a row); see "First run on macOS"
  for what that covers and what it found. Linux and Windows have run too
  (below).
- **Apple** (NSURLSession + BSD sockets): everything compiles with the
  `macos-metal` preset as of 2026-10-08, with no errors and no warnings, also
  under `-Wall -Wextra -Wconversion`. The first build of the rewritten backends
  went through as written; the only fixes were two functions left unused in
  configurations the preset doesn't build.
- **watchOS guards:** the Apple sources also pass a syntax check with
  `CARROTTUS_PLATFORM_WATCHOS=1` defined on the Mac, which is the
  `CRNET_EXT_TCP` / `CRNET_EXT_WEBSOCKET` = 0 configuration. That shows the
  guards hang together, not that a watchOS build works.
- **Linux has run.** Built with the `linux-vulkan` preset (Fedora 44, GCC,
  libcurl 8.18.0 on OpenSSL 3.5) on 2026-10-14, and `tests/crnet_test`
  passes all 259 of its checks (`tests/run_crnet_test.sh
  build-linux-vulkan/Debug/crnet_test`). The first run failed four, from two
  defects: libcurl sends text that isn't UTF-8 as it is given, so the backend
  now refuses it with `CR_RESULT_NET_SEND_FAILED` before sending (the
  same check was added to WinHTTP, which doesn't validate either; the
  validator, `crnet_utf8_valid`, is shared); and libcurl can't follow a
  redirect of a WebSocket opening request at all (it refuses any non-101
  before its redirect handling and keeps no Location), which is now a known
  problem in `crnet.h` on `follow_redirects`, and a note in the test on
  Linux. The other two failures followed from the first: the echoed bad text
  left every later read one message behind.
- **Windows has run.** Built with MSVC under the `windows-d3d12` preset on
  Windows 11 on 2026-10-14, and `tests/crnet_test` passes all 261 of its
  checks, three runs in a row (`tests/run_crnet_test.sh
  build-windows-d3d12/Debug/crnet_test.exe`, from Git Bash). The backends
  compiled as written; only the test needed a change to build, a variable
  named `small`, which the Windows headers define as `char`. The first run
  failed six; see "First run on Windows" for both causes and what the run
  settled. (Before that, on 2026-10-08, the backends had been cross-compiled
  on the Mac with mingw-w64, whose `ws2tcpip.h` lacks `GetAddrInfoExCancel`
  and `GetAddrInfoExOverlappedResult`; `crnetwindows.c` still declares them
  for that toolchain alone.)
- **One test program, not in ctest:** `tests/crnet_test.c` with its server,
  `tests/crnet_test_server.py` (Python 3 standard library and the `openssl`
  command), started and stopped by `tests/run_crnet_test.sh
  <path to crnet_test> [part]`. The C side builds on all three. The runner
  is a shell script, which on Windows runs from Git Bash with a Windows
  Python 3 and Git's own `openssl`.
- **Callers:** every part of Carrottus Net (TCP, the resolver, the wait set, HTTP
  and WebSocket) has a caller outside this repo (2026-10-22).
- **Fuzzed (2026-10-28):** libFuzzer targets in `tests/fuzz/` over what
  Carrottus Net itself parses of bytes it doesn't control - a response's header
  block, a server certificate's DER walked to its public key, the UTF-8
  check, an error record's cut, a proxy URL - each checked against a
  plainer second reading, with the caller's allocator tracked and refusing
  part way. HTTP and WebSocket framing themselves are the stacks' and are
  not Carrottus's to fuzz. `macos-fuzz` / `linux-fuzz` presets; each target is
  also a ctest (`<target>_replay`) over its seeds and
  `tests/fuzz/regressions/`. See `tests/fuzz/README.md`. Found: looking a
  header name up walked every header, so a block of tens of thousands of
  short names (a hostile server's, ~300 KB, within libcurl's limit) took
  ~4 s on the receiving call, outside its time limit; names are now found
  through a hash table (`struct crnet_http_head`'s `slots`). No crash or
  sanitizer report in 20 minutes per target on macOS. Not fuzzed: the
  backends' glue over each stack (libcurl's close-code read, WinHTTP's
  header split), which needs the stack to drive it.


## First run on macOS (2026-10-08)

`tests/crnet_test` goes through TCP, the wait set, the resolver, HTTP
transfers, the TLS desc, the proxy desc and WebSockets against a server on the
same machine that is written in Python and shares no code with Carrottus Net. All 238
checks pass. The sections further down were written before this run and still
list their assumptions as open; for Apple, this is what the run settles.

### Confirmed on macOS

- **NSURLSession is held back by a caller that isn't receiving.** With 1 MiB of
  a 256 MiB download taken and the caller idle for two seconds, the server had
  been able to send about 6 MiB. The rest then arrived intact. This was the
  least certain part of the rewrite.
- **Uploads through the bound streams work**, sized and unsized, 3 MiB each,
  echoed back intact. A sized body of 0 is announced as `Content-Length: 0`
  for POST and DELETE alike; an unsized one goes out chunked.
- **Connection reuse:** three transfers read to their ends made no new
  connection. Destroying a transfer 1 KiB into a 256 MiB response returns at
  once and leaves the client usable.
- **Response headers** arrive joined, one value per name. `HEAD` and a 204
  give no body. Redirects follow or are handed back as the desc says.
- **`Accept-Encoding: identity`** reaches the server as the only
  `Accept-Encoding`, and the body comes as stored. Left alone, NSURLSession
  sends `gzip, deflate` and decodes.
- **TLS desc:** every trust mode, the host-name switch and pins behave as the
  header says against a self-signed certificate, for HTTP and for `wss`. A
  self-signed certificate works as its own root.
- **An oversize WebSocket message** is reported as `NSPOSIXErrorDomain` code 40
  ("Message too long"), as assumed - the open question since the first review.
- **WebSocket close** from this side ends the connection at once, drops an
  unread message, and leaves `get_close_code` at 0; the server's close code
  (4001) comes through when the server closes.
- **WebSocket redirects:** stopping one ends the task with the 3xx as its
  status, which reaches `out_http_status`; following one connects.
- **The proxy dictionary's `HTTPS*` keys**, given by their text, work on macOS:
  an `https` request goes through the proxy as a CONNECT tunnel and a pin still
  holds against the server beyond it.
- **Time limits, aborts and `CRNET_NO_WAIT`** behave as the header says on
  every object tried: waits last about their limit, an abort from another
  thread ends a wait in a fraction of a second and doesn't wear off.
- **The resolver's lifetime rule:** run after destroy, destroy after run, and
  abort-then-run all work.
- **The wait set** names the right members - a listener with a connection
  waiting, a socket with data, a peer that closed, a WebSocket with a message -
  and is quiet once they are drained.

### Found and fixed by the run

- **A server that answers before the request body is sent** left
  `crnet_http_transfer_send` waiting out its whole time limit: NSURLSession
  stops reading the body once it has the answer, so the stream never has room
  again. `send` now treats an answer that has already arrived the way it treats
  a finished task, and lets the remaining bytes through, so the caller reaches
  the response (a 413 in the test).
- **A proxy that can't be reached** came back as `CR_RESULT_NET_HTTP_FAILED`.
  NSURLSession reports it from CFNetwork's own error domain (codes 306 and 310),
  which the result mapping didn't know; it is `CR_RESULT_NET_CONNECT_FAILED`
  now, as the header describes.

### What the stack does that the header leaves open (NSURLSession)

- **Request headers it adds** to a plain GET: `Host`, `Accept: */*`,
  `Accept-Language` (the user's language), `Connection: keep-alive`,
  `Accept-Encoding: gzip, deflate`, and a `User-Agent` naming the program,
  CFNetwork and the Darwin version. This is the first row of the table the
  header should have; Windows' row is under "First run on Windows", and
  Linux is still to be looked at.
- **A POST with `CRNET_HTTP_BODY_NONE`** gets a `Content-Length` added by the
  stack. A GET with none does not.
- **Decided 2026-10-28 with the user (card S6):** `crnet.h` gets a table, a
  row per backend (NSURLSession, WinHTTP, libcurl), of the headers each stack
  adds, filled from the runs (Linux's still to look at): a choice the stack
  makes, made visible (card S2's "no hidden policy"). A header the caller sets
  is sent in place of the stack's own value of the same name on every backend.
  A header a stack adds and won't let go of (NSURLSession's `Host`, and the
  `Accept-Encoding` it may add) is said to be so, per header. Not started.
- **A connect failure** is reported from `crnet_http_transfer_receive_response`,
  never from `_start`, as expected.
- **A TLS connection to `localhost` does not go through an explicit proxy**; a
  plain one does, and so does TLS to any other host. NSURLSession's doing. The
  header says the proxy is "for every connection", which is not quite true for
  a server on the same machine on Apple.
- **A second listener on a held port** fails with errno 48; a refused connect
  is errno 61 from TCP and NSURLSession's -1004 from HTTP; a name that doesn't
  exist is the resolver's code 8.

### Not covered by the run

Every byte in the run went over the loopback interface, between two programs
on one Mac. Loopback is not a network: it is instant, loses nothing, and has
no router, DNS server, firewall or proxy in the way. What that leaves untried:

- **Real name lookup.** The run resolved `localhost`, a numeric address and a
  name reserved never to exist. No DNS server was asked: slow lookups, names
  with many addresses, and a server that doesn't answer are untried.
- **A real certificate chain.** The test server has one self-signed
  certificate. A certificate issued by a public authority through
  intermediates - what `CRNET_TLS_TRUST_SYSTEM` is for - was never met, so
  "an ordinary `https://` site with a zeroed TLS desc" has not been run.
- **A host that never answers.** On loopback a connection is accepted or
  refused at once. A host that swallows packets is what produces
  `CR_RESULT_NET_SYSTEM_TIMED_OUT` and `CR_RESULT_NET_STACK_TIMED_OUT`
  and what really tries a long time limit or `CRNET_WAIT_FOREVER`; neither
  result has been seen from any backend.
- **Slowness and loss.** Megabytes arrive in milliseconds here. A slow or lossy
  link changes the timing of everything: short sends, a response in many small
  pieces, a connection that drops part way through a transfer.
- **A real proxy.** The test's is a few dozen lines that take everything. One
  that asks for a login, inspects TLS or refuses hosts is untried, and the
  header's account of what a login demand looks like to the caller (a 407, or
  a failed connection over TLS) is unobserved.
- **The network changing.** Wi-Fi dropping, a move between networks, the
  machine sleeping in the middle of a transfer. Also time limits across a
  sleep, and a change of the system clock during a wait.
- **IPv6 beyond `::1`**, and which family a stack picks for a name that has
  both.
- **A real service.** The test server is tidy and predictable. A production
  one (Supabase, which this was built to reach) may use HTTP/2, pings,
  compression and redirects that it doesn't.
- **Load.** Many peers in a wait set, many transfers, long-lived connections.
- **Windows and Linux beyond the local test server**, the same list again.

How each could be covered:

- **Done on macOS, 2026-10-22: `crnet_test`'s `internet` part**, opt-in
  (run only by name: `tests/run_crnet_test.sh <exe> internet`), 22
  checks, passing plain and under ASan and TSan. With zeroed TLS and proxy
  descs: `example.com` looked up through a DNS server (two addresses);
  `https://example.com/` (a public authority's chain, through
  intermediates, against the system's trust store: 200 and its page);
  badssl.com's expired, self-signed, wrong-host and untrusted-root
  certificates each refused with `CR_RESULT_NET_TLS_FAILED`
  (NSURLSession's -1202 for all four); `http://github.com/` followed to
  `https://` (200); `wss://echo.websocket.org/` connected, a text message
  echoed after the server's greeting, closed with 1000; and a TCP connect
  and an HTTP GET to 10.255.255.1, which routes and never answers, each
  ending with `CR_RESULT_NET_TIMED_OUT` at the caller's 2 s (2002 and
  2001 ms). Not run on Windows or Linux.
- **The silent host, still open:** `CR_RESULT_NET_SYSTEM_TIMED_OUT` and
  `CR_RESULT_NET_STACK_TIMED_OUT` need a wait longer than the system's
  own limit (75 s for a TCP connect on macOS), so they are still unseen.
- **The real target:** the same calls against the Supabase project.
- **By hand, once a host app exists to try them in:** a proxy that wants a
  login, the network changing, sleep.

## First run on Windows (2026-10-14)

The same test, on Windows 11 against WinHTTP and Winsock2, built with MSVC.
All 261 checks pass, three runs in a row. Like the macOS run, every byte went
over loopback, so "Not covered by the run" above holds for Windows too.

### Found and fixed by the run

- **WinHTTP holds back `SENDREQUEST_COMPLETE` until the response arrives**
  for a request with no body: `REQUEST_SENT` came at once and the completion
  only when the server answered, 1.5 s later against `/slow`. `_start` waited
  for the completion, so it waited for the response, and
  `receive_response`'s time limit and abort never got the chance to act
  (five failures). `_start` now waits for `REQUEST_SENT` (`sent_event`,
  `crnet_win_http_wait_sent`) and leaves the completion in flight, which
  the "whatever is in flight comes first" wait in `_send` and
  `_receive_response` already picks up. A failure that comes before the
  request is sent (a refused connection, a refused certificate) still comes
  from `_start`.
- **WinHTTP shares connections between every session in the process.** A
  new `crnet_http_client` is a new `WinHttpOpen` session, yet across the
  whole run the server saw one HTTP connection. The test's `server_stat`
  asked through a client of its own and so never counted as a connection.
  It now asks over a TCP socket with `Connection: close` (an HTTP client
  where `CRNET_EXT_TCP` is 0). This doesn't break one transfer at a time
  per client, but on Windows a new client is not a new connection, and the
  header doesn't say that it is.

### Confirmed on Windows

- **The streaming transfer's WinHTTP assumptions**, as far as 3 MiB goes: a
  sized body announced by `Content-Length` alone with
  `WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH`, and an unsized one as
  `Transfer-Encoding: chunked` with framing written by hand, both echoed
  back intact. Nothing above 4 GB was sent. Three transfers read to their
  ends made no new connection.
- **WinHTTP is held back by a caller that isn't receiving:** with 1 MiB of a
  256 MiB download taken and the caller idle for two seconds, the server had
  been able to send 1.4 MiB. The rest then arrived intact.
- **`Accept-Encoding: identity`** reaches the server as the only
  `Accept-Encoding` with `WINHTTP_OPTION_DECOMPRESSION` off; left alone,
  WinHTTP sends `gzip, deflate` and decodes.
- **TLS desc:** every trust mode, the host-name switch and pins behave as the
  header says, for HTTP and `wss`, with roots and pins checked in the
  `SENDING_REQUEST` callback. Whether a refused request stayed off the wire
  was not checked.
- **Explicit proxy:** a plain request through it, and an `https` one tunnelled
  through it with a pin that still held against the server beyond.
- **Time limits, aborts, `CRNET_NO_WAIT`, the wait set** (`WSAPoll` over
  sockets tied to events by `WSAEventSelect`, the UDP wake socket, and
  `crnet_wait_set_wake`) and **the resolver's lifetime rule** all behave as
  on macOS. Whether the wake socket raises a firewall prompt was not seen.
- **WebSocket:** echo, a message through a small buffer, close from either
  side, close codes, redirects stopped (the 302 reaches `out_http_status`)
  and followed, a message over its limit, `wss` with a custom root.

### What the stack does that the header leaves open (WinHTTP)

- **Request headers it adds** to a plain GET: `Host`,
  `Connection: Keep-Alive`, and `Accept-Encoding: gzip, deflate` when
  decoding is on. No `User-Agent` (the session is opened with none), no
  `Accept`.
- **A POST with `CRNET_HTTP_BODY_NONE`** gets a `Content-Length` added by the
  stack. A GET with none does not. The same as NSURLSession.
- **A connect failure** is reported from `_start` (12029,
  `ERROR_WINHTTP_CANNOT_CONNECT`), where NSURLSession reports it from
  `_receive_response`. The header allows either.
- **A server that answers before the body is all sent:** `_send` fails with
  12030 (`ERROR_WINHTTP_CONNECTION_ERROR`) and the response is lost, as
  expected under "Streaming HTTP transfer", where there is also why it can't
  be got back. It first came back as
  `CR_RESULT_NET_CONNECT_FAILED`, the wrong word for a connection that
  was made. Fixed the same day: the header now says a connection that breaks
  after it was made is the part's catch-all, and 12030 there is
  `CR_RESULT_NET_SEND_FAILED`. NSURLSession's
  `NSURLErrorNetworkConnectionLost` had the same mistake and was moved to the
  catch-all with it (not built since); libcurl already did this.
- **Codes seen:** 10048 for an address in use, 10061 for a refused TCP
  connect, 11001 for a name that doesn't exist, 12175
  (`ERROR_WINHTTP_SECURE_FAILURE`) for an untrusted certificate and for a
  host-name mismatch alike, and nothing at all for an oversize WebSocket
  message (`CR_RESULT_NET_WS_MESSAGE_TOO_BIG` with no detail).

## Backends rewritten to the header (2026-10-08): written, nothing confirmed

What the header fixes of that day came to in the sources. Each item says what
was done and what it leans on that only a build and a real server can settle.

### Where things are now

- `src/net/crnethttp.c` (new) is the HTTP transfer's front end: it defines
  every `crnet_http_transfer_*`, and holds the order of use, the argument
  checks, a sized body's count, and the response's status and headers. Each
  stack implements the `crnet_http_backend_*` functions in
  `src/net/crnetshared.h` behind it and nothing else of HTTP.
- `src/crnet.c` keeps what every part shares: error records, the TLS and
  proxy desc checks, the certificate walk, and the local address query.
- The one-shot request, the response object, `crnet_http_client_cancel`,
  `crnet_http_client_get_error` and their state are gone from all three
  backends. `CR_RESULT_CRNET_CANCELLED` and
  `CR_RESULT_CRNET_HTTP_RESPONSE_TOO_BIG` are gone from `crcore.h`;
  `CR_RESULT_NET_INVALID_HTTP_TRANSFER` (3032) is new.

### Streaming HTTP transfer

- **WinHTTP.** `WinHttpSendRequest` with no body, then `WinHttpWriteData` per
  `_send` and `WinHttpReadData` per `_recv`, one operation in flight at a time
  and each waited for through the status callback. Request body is copied into
  the transfer's own buffer before it is written, so a `_send` returns as soon
  as WinHTTP has the bytes and a failure to deliver them is the next call's to
  report. `_start` waits for `REQUEST_SENT`, not the send's completion (see
  "First run on Windows"). Confirmed on Windows up to 3 MiB:
  - a sized body announced by a `Content-Length` header alone, with
    `WinHttpSendRequest`'s own length left at 0
    (`WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH`) - still not tried above 4 GB,
    which is what this is for;
  - an unsized body as a `Transfer-Encoding: chunked` header plus chunk
    framing written by hand;
  - a request handle closed after its response was read to the end leaving
    its connection for the next transfer (the pool is the process's, not the
    session's).
- **libcurl.** One easy handle per transfer on the client's multi handle. The
  read and write callbacks pause (`CURL_READFUNC_PAUSE`, `CURL_WRITEFUNC_PAUSE`)
  whenever the caller isn't in `_send` / `_recv`, and each call unpauses and
  drives the multi handle. `_start` waits for the connection through
  `CURLOPT_PREREQFUNCTION` (7.80+); an older libcurl reports a connect failure
  from the first later call. Assumed:
  - that `curl_easy_pause(CURLPAUSE_CONT)` outside a callback is enough to
    restart a paused direction on the next `curl_multi_perform`;
  - that a paused write callback is offered the same piece again, whole;
  - that `CURLOPT_UPLOAD` with a size of 0 sends `Content-Length: 0`, and with
    no size sends the body chunked;
  - that the end of a response's header block can be told from an interim
    reply or a followed redirect the way `crnet_curl_http_on_header` does it
    (status under 200, or a 3xx with a `Location` while following).
- **NSURLSession.** Upload through a pair of bound streams
  (`getBoundStreamsWithBufferSize:`), the request reading one end and `_send`
  writing the other, with the stream's events on a dispatch queue. Download by
  holding the delegate's `didReceiveData` until the caller has taken the whole
  piece. Assumed, and the least certain part of the whole rewrite:
  - that blocking `didReceiveData` really stops NSURLSession reading from the
    network, rather than letting it buffer the rest of the body out of sight;
  - that an `NSOutputStream` may be written from the caller's thread while its
    events are delivered on a dispatch queue;
  - that a `Content-Length` header set on a request whose body is a stream is
    sent as given, and one of 0 with an empty `HTTPBody` likewise;
  - that a connect failure, which `_start` can't see, always surfaces as the
    task's error in `_receive_response`.
- **A server that answers before the body is all sent** (a refusal, usually).
  libcurl and NSURLSession let the remaining `_send` calls through as if sent,
  so the caller reaches the response. WinHTTP reports whatever its write then
  fails with, and the response is lost: on Windows 11 that is 12030,
  `CR_RESULT_NET_SEND_FAILED`. Not the same. Since 2026-10-14 the
  header states it as a rule on `crnet_http_transfer_send`: such an answer
  may be lost, and a caller that needs to hear a refusal asks the server
  first, in a request of its own. (Before that, the HTTP group's known
  problems described each stack's behaviour.) Tried on 2026-10-14 and taken out again: after the failed write, calling
  `WinHttpReceiveResponse` anyway is accepted, then fails with the same
  12030, so WinHTTP gives no way to the answer once a write has broken the
  connection. Likely why: the server closes with body bytes unread, which
  resets the connection, and a reset on Windows drops what the client hadn't
  read yet, the answer with it. libcurl only has the answer because it reads
  the socket while it uploads; WinHTTP reads nothing until the body is
  finished.
  `Expect: 100-continue` was tried next, the same day. The test sends it in
  a second early-answer case, and the test server refuses in place of the
  100 there, as a real server does. WinHTTP passes the header on but doesn't
  wait for the 100: the body goes straight out and fails the same way. Having
  the backend call `WinHttpReceiveResponse` before the first write does get
  the refusal (the 413 at once), but against a server that answers
  `100 Continue` it takes in the 100 (an `INTERMEDIATE_RESPONSE` callback)
  and goes on waiting for a final status the server won't send until it has
  the body: deadlock. WinHTTP allows one operation in flight per request, so
  the body can't be written under that wait. Neither way works, and all of it
  was taken out again, leaving the header's rule above.
- **One transfer at a time per client** is enforced on libcurl (the multi
  handle is driven by one) and NSURLSession (one delegate, one state); a
  second `_start` returns `CR_RESULT_NET_INVALID_HTTP_TRANSFER`. WinHTTP
  would carry several and doesn't check.

### Response headers, request body, methods, decoding

- **One value per header name** is done once, in `crnet_head_add_header`
  (`crnethttp.c`), which joins a repeated name's values with ", ". WinHTTP
  and libcurl feed it line by line; NSURLSession's are already joined.
- **`CRNET_HTTP_BODY`** decides each stack's request shape; no backend looks
  at the method except for `HEAD`, on libcurl only (`CURLOPT_NOBODY`). To find
  out: which stacks add `Content-Length: 0` unasked to a body-less POST, so the
  header can name them.
- **`Accept-Encoding: identity`** is checked in the front end and acted on in
  `_start`: libcurl leaves `CURLOPT_ACCEPT_ENCODING` unset, WinHTTP leaves
  `WINHTTP_OPTION_DECOMPRESSION` off, NSURLSession needs nothing. See "Response
  decoding" below for what that still assumes.

### Proxy, redirects, time-outs

- **`crnet_proxy_desc`** is parsed once (`crnet_proxy_parse`, `crnet.c`)
  and applied per stack: `WinHttpOpen`'s access type, `CURLOPT_PROXY`,
  `connectionProxyDictionary`. To find out: that the `HTTPS*` proxy keys, given
  by their text, are honoured outside macOS; and what each stack returns when
  a proxy asks for a login on a TLS connection.
- **`follow_redirects` on a WebSocket** is set outright on all three
  (`WINHTTP_OPTION_REDIRECT_POLICY`, `CURLOPT_FOLLOWLOCATION`, the
  `willPerformHTTPRedirection` delegate method). To find out: that a WebSocket
  task whose redirect was stopped ends with the 3xx as its response, so the
  status reaches `out_http_status`.
- **`CR_RESULT_NET_STACK_TIMED_OUT`** is what the three result mappers
  now return for the stack's own time-out code, and
  **`CR_RESULT_NET_SYSTEM_TIMED_OUT`** what both TCP backends return for
  `ETIMEDOUT` / `WSAETIMEDOUT` from connect, send and recv. WinHTTP's WebSocket
  connect no longer borrows `ERROR_WINHTTP_TIMEOUT` to mean "the caller's time
  ran out"; it has a marker of its own (`CRNET_WIN_OWN_TIME_OUT`).
- **Still not done, and not in the header either:** which clock a time limit
  counts on across a sleep (the Windows backends' tick count goes on through
  one; Linux and Apple measure time awake), and Apple's waits, which are
  `NSCondition` wait-until-date and so follow the wall clock - now in the HTTP
  transfer as well as the WebSocket.

### TCP and the build

- **`reuse_address` is gone.** The BSD-sockets backend sets `SO_REUSEADDR`
  always; the Winsock one never did and still doesn't. No caller sets
  the field now. To confirm on a machine: that Windows refuses a
  particular-address listener on a port a wildcard listener holds (read from
  Microsoft's table, not tried), and that macOS allows it. Not done, a
  separate decision: `SO_EXCLUSIVEADDRUSE` on Windows.
- **`CRNET_EXT_TCP` / `CRNET_EXT_WEBSOCKET`.** `CMakeLists.txt` defines
  `CARROTTUS_PLATFORM_WATCHOS=1` for a watchOS build. The BSD-sockets file, the
  wait set, the WebSocket half of the NSURLSession file and
  `crnet_instance_get_local_ipv4_addresses` are each inside the matching guard, so a
  watchOS build compiles them to nothing rather than leaving them out of the
  source list. No watchOS build has been attempted.

### Outside carrottus

- A caller that wants a small response body as one block (the JSON case) has
  to loop on `crnet_http_transfer_recv` itself. That loop belongs above
  Carrottus, in a caller, and isn't written.

## Response decoding: the caller can decline it, nothing confirmed

Response bodies are still always decoded - NSURLSession can't do otherwise - but
since 2026-10-08 the header gives the caller the way round it that works on
every stack: sending `Accept-Encoding: identity`, so the server compresses
nothing. It is the only value of that header a caller may send; any other is
refused. A shared check (`crnet_headers_accept_encoding`) decides it once for
every backend. Not compiled, not run.

- **It is in the streaming transfer** since the backends were rewritten: the
  check in the front end (`crnethttp.c`), and in each backend's
  `crnet_http_backend_start` libcurl leaving `CURLOPT_ACCEPT_ENCODING` unset
  and WinHTTP leaving `WINHTTP_OPTION_DECOMPRESSION` off.
- **WinHTTP:** with the option off it adds no `Accept-Encoding` of its own and
  sends the caller's (confirmed on Windows, 2026-10-14).
- **NSURLSession:** assumed that it sends a caller's `Accept-Encoding` in place
  of its own. If it turns out to add or override, `identity` can't be
  guaranteed there and the header's claim has to be narrowed.
- **A server may ignore `identity`** and compress anyway. libcurl and WinHTTP
  would then hand the body over still compressed and NSURLSession would decode
  it. Rare, the server's fault, and not stated in the header.

## TLS control: written, nothing confirmed

`crnet_tls_desc` goes to `crnet_http_client_create` (which now takes a desc)
and into `crnet_websocket_connect_desc`. Not compiled, not run. What each
backend leans on that still needs a real server to confirm:

- **WinHTTP** has no "trust these roots" option, so roots and pins are checked in
  the `SENDING_REQUEST` status callback and a refusal closes the request handle
  from inside it. The test's TLS part passes on Windows with this, so the
  certificate is readable there and a refusal ends the request. Still
  assumed: that closing there keeps the request off the wire,
  that `WINHTTP_OPTION_SERVER_CERT_CONTEXT` fails
  with `ERROR_WINHTTP_INCORRECT_HANDLE_STATE` on a plain-http hop, and that the
  callback is never waited on by the `WinHttpSendRequest` that holds the
  WebSocket's lock.
- **WinHTTP, `SYSTEM_AND_ROOTS`:** the system half is a second chain build with
  the default engine, after WinHTTP was told to ignore unknown roots.
- **WinHTTP, http that redirects to https:** the security flags can't be set on a
  request that starts as plain http, so the https hop gets WinHTTP's full checks
  and a custom root or `skip_host_name_check` doesn't reach it.
- **libcurl** gets roots as `CURLOPT_CAINFO_BLOB` (7.77+) and the system's own
  bundle named beside it from `curl_version_info` (7.84+); a libcurl that reports
  neither `cainfo` nor `capath` returns `CR_RESULT_NET_UNSUPPORTED` for
  `SYSTEM_AND_ROOTS`. Pins are `CURLOPT_PINNEDPUBLICKEY`, which not every TLS
  library libcurl builds with has. A root that isn't a certificate is only found
  at the first connection (`CR_RESULT_NET_TLS_FAILED`), not at create as on
  the other two.
- **NSURLSession:** a refused certificate cancels the challenge and the delegate
  remembers why; assumed that the task then fails rather than retries. Apple's
  own rules for server certificates (SAN present, validity at most 825 days,
  server-auth usage) still apply to a self-signed certificate given as a root,
  and App Transport Security sits in front of everything.
- **Pins use `SecTrustCopyCertificateChain`** (macOS 12, iOS 15, watchOS 8).
- **The DER walk** that finds a certificate's public key
  (`crnet_certificate_public_key_info`) was checked against `openssl` for an
  RSA and an EC certificate, as an algorithm, in Python - not as the C.

Not there: client certificates, a minimum TLS version, revocation checking, and
any way to read back the certificate a server presented.

## Resolver: written, nothing confirmed

`crnet_resolve_host` is gone (2026-10-08). A lookup is now a `crnet_resolver`:
`crnet_resolver_run` blocks in the system resolver on a thread the caller
supplies, and `crnet_resolver_wait` waits for the answer with a time limit and
an abort on another. Carrottus Net starts no thread; the header's new "Threads"
section says so for the whole module. Implemented in both TCP backend files.
A caller uses it as meant: a thread that only runs the lookup, and a wait with a
time limit on the caller's own.

- **Run once, destroy once, in either order.** The resolver is held twice from
  creation and freed when both have happened. A resolver that is created and
  destroyed without ever being run is leaked, by design of the rule; the header
  says to abort it and run it, which returns at once.
- **Only Windows can stop a lookup** (`GetAddrInfoExW` with
  `GetAddrInfoExCancel`). On Linux and Apple the thread in `run` comes back when
  `getaddrinfo` does, however long the caller stopped waiting ago.
- **Windows, assumed:** that an overlapped `GetAddrInfoExW` signals its event
  after a cancel, so the wait for it that follows always ends; that `NS_ALL`
  matches what `getaddrinfo` consults; and that a non-ASCII host name needs no
  IDN flag.
- **`crnet_instance_get_local_ipv4_addresses` still blocks** in the same resolver with
  no time limit.

## Error detail: written, nothing confirmed

Each object keeps a `crnet_error` (the platform's own number, which numbering
it is in, and the stack's wording where it has some) for its last failure;
`crnet_startup` (now `crnet_instance_create`), `crnet_instance_get_local_ipv4_addresses` and
`crnet_tcp_listener_create` take an `out_error` instead, which changed their
signatures. Not compiled, not run.

- **Not every failure has a number behind it.** Out of memory, a refused
  argument, a response or message over its size limit, and a WebSocket close or
  refused upgrade record nothing by design. So do a few paths where the stack
  gave none: libcurl being unable to apply a TLS option
  (`CR_RESULT_NET_UNSUPPORTED`), and a WinHTTP close frame that couldn't be
  sent because the socket had already gone.
- **`text` is only filled by libcurl and NSError.** errno and Win32 codes are
  left for the caller's own `strerror` / `FormatMessage`.
- **WinHTTP's own certificate complaints** come back as 12175
  (`ERROR_WINHTTP_SECURE_FAILURE`) and no more: which of its checks failed is in
  a `SECURE_FAILURE` status callback this doesn't subscribe to.
- **The getters take no lock.** They rely on the caller reading from the thread
  whose call failed, as the header says.
- **Carrottus Files has the same gap** (`CR_RESULT_GENERAL_FAILURE` for most of
  `errno`). If it wants the same answer, `crnet_error` should move to Carrottus Core
  as one shape for both rather than be copied.

## Wait set: written, nothing confirmed

`crnet_wait_set` lets one thread wait on TCP sockets, listeners and WebSockets
together. It is one file for every platform, `src/net/crnetwaitset.c`: `poll()`
(`WSAPoll` on Windows) over the members' sockets plus a wake socket. Not
compiled, not run.

- **It is `poll()`, not `epoll`/`kqueue`.** Each wait costs time in proportion to
  the members. Fine for hundreds; a set of thousands wants the real thing behind
  the same header.
- **The Windows wake socket** is a UDP socket on 127.0.0.1 sending to itself,
  because `WSAPoll` can't wait on an event or a pipe. `WSAPoll` does report on
  a socket that is also tied to an event by `WSAEventSelect` (every TCP socket
  here is): the wait set part passes on Windows. Still assumed: that the wake
  socket raises no firewall prompt.
- **NSURLSession and WinHTTP WebSockets** have no socket to poll. Adding one
  starts a receive in the background and the stack's callback wakes the set.
  That relies on the callback and `crnet_websocket_wait_detach` taking the same
  lock, so no callback can reach a set that has been destroyed.
- **libcurl's WebSocket** is polled by its descriptor, which doesn't show bytes
  libcurl or the TLS layer has already read. The set names it once after it is
  added, and the header's "drain until `TIMED_OUT`" rule covers the rest.
- **`CRNET_WAIT_FOR_ROOM` is level-triggered** like the `poll()` under it: on a
  connection that isn't backed up, a set asked about room returns at once every
  time. The header says to ask only while there are bytes that wouldn't send.
- **`crnet_wait_set_wake` (2026-10-08)** ends one wait and leaves the set
  usable, which an abort can't: for the thread that waits on a set and is also
  handed work by other threads. It was added for a caller whose thread waits on
  every connection and has to be told when the caller queues something to send.
  It reuses the wake socket and a flag of its own beside `aborted`. A check for
  it is in `tests/crnet_test.c` ("Wait set"), and passes on macOS: a wake made
  beforehand ends the next wait, one from another thread ends a wait with no
  limit, and the set waits again afterwards. Windows' half (the `woken` flag
  beside `WSAPoll`) passes it too (2026-10-14).
- **An object destroyed while still in a set** is the caller's mistake and isn't
  caught; the set would poll a closed descriptor or ask a freed WebSocket.
- **HTTP isn't in it.** An HTTP transfer now has a `_recv` that could be waited
  on, but none of the three stacks hands over a socket for it; it would have to
  notify the set the way the NSURLSession and WinHTTP WebSockets do.

## WebSocket close: made the same everywhere, nothing confirmed

`crnet_websocket_close` used to end the connection at once on Apple and leave
it receiving until the peer answered on Windows and Linux. Since 2026-10-08 it
is the first of those on all three, in the header and in the backends: the close
frame is sent and the socket is finished for this side - `recv` returns
`CR_RESULT_NET_WS_CLOSED` at once, a message that had arrived and wasn't
received is dropped, `send` and a second close fail, and
`crnet_websocket_get_close_code` is 0 for a close this side made. Not
compiled, not run.

- **The peer's answer to our close is never read**, and neither is anything it
  sends after it. A two-way goodbye is the caller's, in messages.
- **Whether the close frame goes out** before a `destroy` that follows straight
  after is not known on any of the three.
- **WinHTTP** is told to shut down its sending and then ignored: its receive
  stays in flight until `destroy` closes the handle. Assumed harmless.
- **libcurl in a wait set:** the set polls the socket's descriptor, which says
  nothing when this side closes, so the set doesn't name a socket for its own
  close the way it does on the other two. The caller made the close and knows.
- **A close that couldn't be sent** leaves the socket broken rather than closed
  on libcurl (`recv` then returns `CR_RESULT_NET_RECV_FAILED`); on
  NSURLSession there is no telling whether it was sent.

## Inconsistent across platforms

- **Several `Set-Cookie` headers can't be read apart**, on any platform now (see
  "Response headers" under "Header only"). Nothing planned needs cookies. If
  something does, the fix is an accessor for cookies alone: `NSHTTPCookie` on
  Apple, whose parser understands NSURLSession's own joining, and the separate
  lines on the other two, read before they are joined.
- **Oversize WebSocket messages** are recognised on Apple by two signs, neither
  confirmed on a device: the receive failing with `NSPOSIXErrorDomain` /
  `EMSGSIZE` (code 40), looked for in the errors nested under the one reported
  as well, or the task's own close code being 1009 when the peer hadn't closed.
  A WebKit bug report (227030) shows that error and that close code for this
  case on iOS. Still to check with a real oversize message: that at least one
  sign appears, and that a 1009 close from the peer - which means our message
  was too big for it - is seen as the peer's (`didCloseWithCode`) before the
  failed receive is, since otherwise it would be taken for the other case.
- **WebSocket support on Linux** is still found out only at runtime
  (`CR_RESULT_NET_UNSUPPORTED` from `crnet_websocket_create`, libcurl older
  than 8.11). `CRNET_EXT_WEBSOCKET` is 1 there: it goes by platform, and the
  header can't see which libcurl carrottus was built against. Making it exact
  means CMake passing the libcurl check through as a definition.

## Still decided for the caller

Carrottus's rule is that it never decides on the caller's behalf. These do:

- The stack adds its own request headers, and which ones differs by platform.

## Looked at and left as it is

- **HTTP and WebSocket look host names up themselves, and TCP doesn't.** This
  was listed under "Still decided for the caller" and is deliberate. TCP takes
  numeric addresses because the system's lookup can't be time-limited or
  interrupted; the stacks' own lookups can, being part of the call that needs
  them. And the stacks need the name itself, for the request and for TLS. The
  one thing lost is choosing which of a name's addresses is used, and only
  libcurl has a setting that would give it back. The header says all this
  under "HTTP and WebSocket" since 2026-10-08; there is no backend work.

- **`crnet_startup` / `crnet_shutdown` were process-wide**, and were left
  so at first, on the argument that an instance handle would be ceremony over
  state that is the platform's and process-wide whatever is passed round. That
  held while Carrottus Net had one caller. It was reversed later the same day, once
  a second caller inside a library came: see "The header is ahead of the
  backends: instances" at the top. Still to confirm when Linux first runs: that
  the libcurl in use counts its global init as its documentation says, which
  several instances now rely on.

## Missing pieces

- **WebSocket:** negotiated subprotocol, the opening response's headers, the close
  reason (only the code is kept), sending a ping.
- **TCP:** keep-alive option, a local address for outgoing connections, a
  local-address query, UDP.
- **Interfaces:** `crnet_instance_get_local_ipv4_addresses` is IPv4-only, goes through
  the resolver, and doesn't follow the `crnet_<object>_<verb>` pattern. A real
  interface enumeration should replace it.
  **Decided 2026-10-28 with the user (card S9): replaced now** (no caller
  outside carrottus), by an interface listing in Carrottus Net's pattern (such as
  `crnet_interface_enumerate`): per interface its name and index, flags
  (up, loopback, point-to-point), and every address, IPv4 and IPv6, with its
  prefix length; from `getifaddrs` (Apple, Linux) and
  `GetAdaptersAddresses` (Windows), so it never blocks and never touches the
  resolver. It reports; which interface to advertise is the caller's. The
  other missing pieces wait for a caller. Not started; needs Windows and
  Linux runs.

## Naming and contract leftovers

- `send` is partial on TCP and whole-message on WebSocket.
- End of connection is 0 bytes with `CR_SUCCESS` on TCP and
  `CR_RESULT_NET_WS_CLOSED` on WebSocket.
- Bad arguments return `CR_RESULT_NET_SEND_FAILED` in several places (NULL
  data, a close reason over 123 bytes) rather than an invalid-argument result.
- Result codes say `WS_` where functions say `websocket`.
- `CRNET_WEBSOCKET_MESSAGE` is a type enum without `_TYPE` in its name.
- `crnet_header` is a generic name for something only HTTP and WebSocket use.
- A WebSocket text message handed over in parts can be split in the middle of a
  UTF-8 character.

**Decided 2026-10-28 with the user (card S5):**
- Renamed: `CR_RESULT_CRNET_WS_*` to `CR_RESULT_CRNET_WEBSOCKET_*`,
  `CRNET_WEBSOCKET_MESSAGE` to `CRNET_WEBSOCKET_MESSAGE_TYPE`,
  `crnet_header` to `crnet_http_header`. Every caller renamed in the
  same change, as card X1b's renames were.
- A bad argument (NULL data, a close reason over 123 bytes) returns an
  invalid-argument result, never `CR_RESULT_NET_SEND_FAILED`.
- Each transport keeps its own `send` (partial on TCP, whole messages on
  WebSocket) and its own end of connection (0 bytes on TCP, a result on
  WebSocket), as the protocols are; each object's header states its rule.
- A text message's parts are bytes; reassembling text, characters split
  across parts included, is the caller's, said in the header.
Not started.

## Order to do it in

0. Compiled on macOS (2026-10-08), Linux and Windows with MSVC (2026-10-14).
1. The test program passes on macOS (2026-10-08; see "First run on macOS"),
   Linux and Windows (2026-10-14; see "First run on Windows").
   Off the loopback interface: the opt-in `internet` part, a public HTTPS
   and `wss://` server and a host that never answers, passes on macOS
   (2026-10-22). Next: the same on Windows and Linux. See "Not covered by
   the run".
2. Print each object's `_get_error` from that test program wherever a call
   fails, and check the numbers are the ones the platform documents - the
   assumed `EMSGSIZE` for an oversize WebSocket message on Apple among them.
3. Run the TLS desc against a server with a private root, one with a
   self-signed certificate, and one whose pin doesn't match, on each platform.
4. ~~Compile and run on Windows and Linux.~~ Done 2026-10-14.
