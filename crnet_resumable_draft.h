/*
 * crnet_resumable_draft.h - Carrottus Net: resumable starts. A connect, an
 * HTTP request's start and a WebSocket send that may not wait begin their
 * step and come back, and later calls carry the step on, so that a caller
 * that makes no thread can advance all its network work from its own
 * non-blocking update call (CRNET_TODO.md, "Resumable starts").
 *
 * APPROVED by the user 2026-11-13, tenth version (SHA-256 in CRNET_TODO.md's
 * approval record), U1 answered a, as drafted. Its ten
 * questions decided with the user on 2026-11-13, every one as recommended
 * (CRNET_TODO.md); the seven passes after them (versions 4-10) change no
 * answer, and what they found that would is under "For the user" there
 * (C9.4). Not built,
 * not in any target. Written against include/crnet.h and
 * include/crcore.h as read on 2026-11-13 (carrottus 3bc4e44, version
 * 0.2.0), and against each backend as it is there: src/net/crnethttp.c,
 * src/net/crnetwaitset.c, src/net/backends/windows/crnetwinhttp.c and
 * crnetwindows.c, src/net/backends/apple/crneturlsession.m,
 * src/net/backends/linux/crnetcurl.c, src/net/backends/posix/crnetsockets.c.
 *
 * Every declaration below is marked [ADDED], [CHANGED] or [REMOVED];
 * nothing unmarked changes. It compiles beside today's crnet.h: an
 * [ADDED] call is declared under its own name; a [CHANGED] call whose
 * signature stays is declared again as it is (a C redeclaration), with its
 * new contract above it; the one [CHANGED] signature is declared under
 * CRNET_DRAFT_CHANGED(name), which appends `_as_changed` so that it
 * doesn't collide with today's. Checked with
 *   clang -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -I include -x c
 *   clang -fsyntax-only -std=c++17 -Wall -Wextra -Wpedantic -I include -x c++
 * both bare (every platform macro 0: the HTTP half only) and with
 * -DCARROTTUS_PLATFORM_APPLE=1 (every part).
 *
 * Names are today's (CR_RESULT_NET_WS_*, CRNET_WEBSOCKET_MESSAGE,
 * crnet_header). Card S5's renames, decided 2026-10-28 and not started,
 * apply to whatever this adds when they are made.
 *
 * ---------------------------------------------------------------------------
 * Versions
 * ---------------------------------------------------------------------------
 *
 * 1. 2026-11-13. The decided shape (the client TODO's question 1, a,
 *    decided with the user 2026-11-13): CRNET_NO_WAIT on a connect, a
 *    start or a WebSocket send begins the step and returns
 *    CR_RESULT_NET_TIMED_OUT, the next call carries it on; a send takes the
 *    whole message or nothing. HTTP transfers join a wait set.
 * 2. 2026-11-13. First critique pass, platform correctness, backend by
 *    backend against the code and the platforms' documentation: findings
 *    P1-P11 below.
 * 3. 2026-11-13. Second critique pass, misuse and threading: findings
 *    T1-T14 below.
 * 4. 2026-11-13. Third critique pass, the decided answers (Q1-Q10, all
 *    a): findings C3.1-C3.6 (CRNET_TODO.md, "Critique").
 * 5. 2026-11-13. Fourth critique pass, each backend again against its
 *    documentation and the sources: findings C4.1-C4.10.
 * 6. 2026-11-13. Fifth critique pass, Carrottus's rules (no hidden
 *    policy, no global state, the caller's allocator, card S10, Debug
 *    checks, naming): findings C5.1-C5.7.
 * 7. 2026-11-13. Sixth critique pass, memory and data: findings
 *    C6.1-C6.8.
 * 8. 2026-11-13. Seventh critique pass, concurrency and abort: findings
 *    C7.1-C7.9.
 * 9. 2026-11-13. Eighth critique pass, testability and the plan:
 *    findings C8.1-C8.8.
 * 10. 2026-11-13. Ninth critique pass, a final read (contradictions,
 *    stale references, the ABI list, wording): findings C9.1-C9.5. This
 *    version.
 *
 * ---------------------------------------------------------------------------
 * Why a draft
 * ---------------------------------------------------------------------------
 *
 * Card X21: every new or changed public item in carrottus is designed in a
 * draft header and approved before it is built. This changes what
 * CRNET_NO_WAIT and CR_RESULT_NET_TIMED_OUT mean on four calls (the two
 * connects, start, a WebSocket send) and on close, changes what an HTTP
 * send and receive_response report, adds five calls and changes one
 * signature (C9.1).
 *
 * The need. Today, with CRNET_NO_WAIT, crnet_tcp_socket_connect(),
 * crnet_http_transfer_start(), crnet_websocket_connect() and
 * crnet_websocket_send() do nothing and return CR_RESULT_NET_TIMED_OUT
 * (crnet.h, "Time limits": they "can't finish without the other side
 * answering"). They only work by waiting, so a caller that may start no
 * thread (the workspace's threading rule: libraries create no threads;
 * network I/O is non-blocking, advanced from the owner's update call) can't
 * open a connection, start a request or send a WebSocket message without
 * stalling its update. And an HTTP transfer can't be in a wait set, so such
 * a caller can't sleep until a transfer has something for it either.
 *
 * Every stack is asynchronous underneath, and none needs a thread of
 * Carrottus's own to go on with a step once it is begun:
 *
 * - WinHTTP runs in its asynchronous mode already (WINHTTP_FLAG_ASYNC):
 *   every step is begun by a call that returns at once and finished on
 *   WinHTTP's own threads, which report through the status callback; the
 *   backends' "waiting" is a WaitForMultipleObjects on the event the
 *   callback sets.
 * - NSURLSession runs every task on its own queue: a task resumed goes on
 *   by itself, and sendMessage:completionHandler: copies the message and
 *   calls back when it is gone. The backends' waiting is an NSCondition or
 *   a dispatch semaphore.
 * - libcurl's multi interface (already what drives every transfer and
 *   every WebSocket opening here) does one non-blocking pass per
 *   curl_multi_perform; curl_ws_send sends what fits and says how much.
 *   The backends' waiting is curl_multi_poll and poll().
 * - BSD sockets and Winsock: a non-blocking connect() returns EINPROGRESS
 *   / WSAEWOULDBLOCK, the socket turns writable when it has an outcome,
 *   and SO_ERROR says which. The backends already connect this way and
 *   wait with poll() or WSAEventSelect's event.
 *
 * So the change is to stop throwing the step away when the wait gives up,
 * and to let a later call pick it up: a translation of what each stack
 * already is, no decision made for the caller. What Carrottus keeps to do
 * that is listed per call ("What is kept"), all of it on the object the
 * step belongs to and through the instance's allocator, or the stack's own.
 *
 * ---------------------------------------------------------------------------
 * The rule, stated once (crnet.h, "Time limits" [CHANGED])
 * ---------------------------------------------------------------------------
 *
 * A step is something the other side has to answer before it is done:
 * opening a TCP connection, opening a WebSocket, getting an HTTP request
 * out, getting a WebSocket message onto its connection. A call that begins
 * a step begins it whatever its time limit, CRNET_NO_WAIT included, and
 * waits for it no longer than the limit. When the limit runs out first the
 * call returns CR_RESULT_NET_TIMED_OUT and the step goes on: on the
 * stack's own threads, in the system, or, for libcurl, the next time a
 * call on the object drives it. Nothing has failed and nothing is lost.
 * A later call - which one is said on each - carries the step on, again
 * within its own limit, and reports how it ended. A caller that no longer
 * wants it aborts or destroys the object (Q1).
 *
 * CR_RESULT_NET_TIMED_OUT keeps its one meaning - the caller's limit, and
 * nothing else, is what ended the call - and gains a second half that now
 * holds for every call in this header: the object is as usable as it was,
 * and calling again is always in order. Two calls lose an exception to that:
 * a WebSocket send that runs out no longer ends the connection, and an HTTP
 * start that runs out no longer ends the transfer.
 *
 * With CRNET_NO_WAIT a call still never waits: it begins what it can
 * begin on the spot, which for these calls is always the step itself, and
 * returns.
 *
 * ---------------------------------------------------------------------------
 * Questions for the user
 * ---------------------------------------------------------------------------
 *
 * [CHANGED] (C3.1) Decided with the user 2026-11-13, every one a, as
 * recommended (CRNET_TODO.md, "Decided with you"). Each question stays as
 * it was put, as the record of what was weighed.
 *
 * Q1. Which time-outs leave the step going.
 *     a. (Recommended, drafted) Every CR_RESULT_NET_TIMED_OUT from these
 *        calls, CRNET_NO_WAIT or a longer limit alike. One meaning across
 *        the header: "not yet; call again", as recv, accept and
 *        receive_response already have it. It removes the two places where
 *        running out of time ends an object today (a WebSocket send leaves
 *        part of a message on the wire; an HTTP start may have half sent
 *        its request), which existed only because the call threw its step
 *        away when it stopped waiting. A caller with an overall deadline
 *        keeps it and aborts.
 *     b. Only CRNET_NO_WAIT begins and leaves a step; a longer limit that
 *        runs out ends the object as today. Two meanings of one result,
 *        told apart by the argument the caller passed.
 *
 * Q2. How a connect is carried on (a TCP socket's, a WebSocket's).
 *     a. (Recommended, drafted) [ADDED] crnet_tcp_socket_connect_continue()
 *        and crnet_websocket_connect_continue(): the object and a time
 *        limit (and the WebSocket's status out), nothing else. Explicit
 *        about what is read: the desc was taken whole by the first call.
 *        The other calls on a connecting object stay refused as on an
 *        unconnected one (INVALID_TCP_SOCKET, INVALID_WEBSOCKET), so where
 *        the connect's outcome is reported is one call, as today.
 *     b. The same connect called again with a desc: its base checked,
 *        only its `timeout_ms` read, the rest required to be as the first
 *        time and not compared. A desc that is partly ignored, and a caller
 *        that keeps one alive or rebuilds it only to pass a time limit.
 *     c. The socket counts as connecting; recv and send carry the connect
 *        on, return TIMED_OUT until it is open and report its failure;
 *        the opening status comes from a new getter. Every recv and send
 *        result list grows by the connect's failures, and both sides of a
 *        socket would race to finish one step.
 *
 * Q3. Where an HTTP start is carried on.
 *     a. (Recommended, drafted) A start that returns TIMED_OUT has
 *        started the transfer: it is under way and the request may not yet
 *        be out. The next call in the order of use (send, or
 *        receive_response) carries it on before doing its own part, within
 *        its own limit, and reports its failure; start itself isn't called
 *        again (it returns INVALID_HTTP_TRANSFER, as for any started
 *        transfer). Start has no output to hand back, and crnet.h already
 *        lets a later call report what start would ("some connect and send
 *        the headers here, some put it off"); this makes that the whole
 *        story. NSURLSession never says when it has connected, so there
 *        start returns CR_SUCCESS at once (P1), and the caller's code is the
 *        same either way: start, then the next call.
 *     b. start never returns TIMED_OUT: CR_SUCCESS once the request is
 *        handed to the stack, after waiting up to its limit only so that an
 *        early failure comes from start. The desc's `timeout_ms` then says
 *        only when a failure is reported, which no caller can use.
 *     c. As Q2 a: an [ADDED] crnet_http_transfer_start_continue(). A
 *        call that does what the next call in the order of use does anyway.
 *
 * Q4. What a WebSocket send takes, and who holds it until it has gone.
 *     a. (Recommended, drafted) One message in flight per socket. A send
 *        first carries on the message before it; if that one is still in
 *        flight when the limit runs out, the new one is not taken
 *        (TIMED_OUT, nothing of it taken: send it again). Otherwise the
 *        new message is taken whole - copied into a buffer of the socket's
 *        own, through the instance's allocator (WinHTTP, libcurl), or into
 *        the stack's own message (NSURLSession's NSData, which copies) -
 *        and send returns CR_SUCCESS without waiting for it to go out; a
 *        limit above 0 is only ever spent on the message before. "Taken"
 *        is what CR_SUCCESS meant already ("handed to the connection, not
 *        acknowledged by the peer"); it is now the copy, not the wire. The
 *        caller's bytes are read only during the call, as everywhere in
 *        crnet.h. The buffer grows to the largest message sent and is
 *        kept, so a steady stream allocates nothing (cr_noalloc_tests).
 *     b. As a, with no copy: the caller's buffer is lent until the message
 *        has gone (the next send taken, or crnet_websocket_flush()
 *        returning CR_SUCCESS). No copy, and the first lifetime in crnet.h
 *        that outlasts a call - which a caller gets wrong silently.
 *     c. A queue of messages in flight. How many and how much memory is a
 *        policy, and WinHTTP takes one send at a time (P3), so the queue
 *        would be Carrottus's; the caller can keep one of its own.
 *
 * Q5. Seeing a message off.
 *     a. (Recommended, drafted) [ADDED] crnet_websocket_flush(socket,
 *        timeout_ms): carries the message in flight on, CR_SUCCESS once
 *        none is in flight, TIMED_OUT while one is, the failure of one that
 *        failed. Needed on libcurl, which moves a part-sent frame only
 *        when curl_ws_send is called again (P4): without it the last
 *        message of a burst would sit until the caller sent another. On
 *        WinHTTP and NSURLSession, which go on by themselves, it only
 *        reports. It is also how a caller that wants today's "sent" waits
 *        for it: send, then flush with a limit.
 *     b. No flush: the next send or close carries it on. A last message on
 *        libcurl then never goes, and nothing tells a caller its message
 *        failed until it sends another.
 *
 * Q6. HTTP transfers in a wait set.
 *     a. (Recommended, drafted) [ADDED]
 *        crnet_wait_set_add_http_transfer() and _remove_http_transfer(),
 *        with CRNET_WAIT_FOR_INCOMING (receive_response or recv has
 *        something, or the transfer has ended) and CRNET_WAIT_FOR_ROOM
 *        (send would take bytes; a start under way counts, its next call
 *        being send). A caller that sleeps between events needs it: without
 *        it, a transfer is only found ready by circling it with
 *        CRNET_NO_WAIT on a timer of the caller's, which is either late or
 *        busy. Every stack can wake a set (P7): WinHTTP and NSURLSession
 *        through the set's notify, as their WebSockets do; libcurl through
 *        its descriptors and timer, read from the client's multi handle at
 *        each wait.
 *     b. Not added: a caller with transfers polls them with CRNET_NO_WAIT
 *        on a timer of its own, as today.
 *
 * Q7. A wait set where there are no TCP sockets or WebSockets (watchOS).
 *     a. (Recommended, drafted) A wait set on every platform: with Q6 it
 *        has a member there, and it needs no network permission (its wake
 *        is a pipe, polled with poll(); neither is a network socket). Its
 *        TCP and WebSocket calls stay behind their macros. Not tried on
 *        watchOS: the first run there says (P11).
 *     b. The wait set stays behind CRNET_EXT_TCP || CRNET_EXT_WEBSOCKET;
 *        a watchOS caller circles its transfers with CRNET_NO_WAIT.
 *
 * Q8. TCP connect too.
 *     a. (Recommended, drafted) Yes: crnet_tcp_socket_connect() is the
 *        fourth call "Time limits" lists as doing nothing with
 *        CRNET_NO_WAIT, and its non-blocking connect is the simplest of
 *        all (the backends already make one, then wait). Leaving it would
 *        keep one exception in the rule above.
 *     b. Only the three calls the need named; TCP connect keeps "does
 *        nothing".
 *
 * Q9. libcurl that looks names up synchronously.
 *     a. (Recommended, drafted) crnet_http_client_create() and
 *        crnet_websocket_create() return CR_RESULT_NET_UNSUPPORTED on a
 *        libcurl without CURL_VERSION_ASYNCHDNS (P6), so that the promise
 *        "CRNET_NO_WAIT never waits" holds wherever a client or socket can
 *        be made. Such a libcurl resolves inside curl_multi_perform,
 *        blocking for as long as a name server takes. Every libcurl the
 *        major distributions ship has the threaded resolver or c-ares (to
 *        check on Fedora, Debian and Ubuntu at build); the WebSocket
 *        precedent (found out at create, CR_RESULT_NET_UNSUPPORTED) is
 *        followed. TCP and the resolver are untouched: they don't use
 *        libcurl. (C5.3) Each create asks curl_version_info itself; the
 *        answer is kept nowhere, so no state of Carrottus's own is added.
 *     b. A known problem on the HTTP and WebSocket groups: on such a
 *        libcurl a CRNET_NO_WAIT call may block in the lookup. The caller
 *        can't tell which libcurl it has.
 *
 * Q10. Asking a wait set about a WebSocket's room.
 *     a. (Recommended, drafted) [CHANGED] crnet_wait_set_add_websocket()
 *        gains `wait_for`, as crnet_wait_set_add_tcp_socket() has it:
 *        INCOMING as today, ROOM for "a send would take a message now"
 *        (none in flight, or the one in flight can be carried on). A
 *        listed break: every caller passes CRNET_WAIT_FOR_INCOMING where it
 *        passed nothing.
 *     b. [ADDED] a second call beside it with `wait_for`, the old one
 *        kept: two calls for one thing.
 *     c. No `wait_for`: a WebSocket with a message in flight is named ROOM
 *        unasked. A bit in `ready` that wasn't asked for, which the header
 *        promises never happens.
 *
 * ---------------------------------------------------------------------------
 * Findings, first critique pass (version 2): platform correctness
 * ---------------------------------------------------------------------------
 *
 * P1. NSURLSession: the HTTP backend never waits in start (it resumes the
 *     task and returns; NSURLSession reports no connection event), so
 *     there start returns CR_SUCCESS whatever the limit. Version 1 promised
 *     TIMED_OUT from a CRNET_NO_WAIT start on every stack; version 2 lets
 *     start return either and says the caller treats both as "started"
 *     (Q3 a).
 * P2. WinHTTP: opening a WebSocket is two asynchronous steps
 *     (WinHttpSendRequest, then WinHttpReceiveResponse) and a synchronous
 *     WinHttpWebSocketCompleteUpgrade. Version 1 had each continuing call
 *     issue the next step, so an opening stood still between a caller's
 *     calls. Version 2: the status callback issues WinHttpReceiveResponse
 *     on SENDREQUEST_COMPLETE (WinHTTP allows calls on the request handle
 *     from its callback), so the opening runs to the response on WinHTTP's
 *     threads; the continuing call reads the status and completes the
 *     upgrade, keeping the handle's lifetime in the caller's calls. The
 *     request handle's close by the TLS check or an abort is already
 *     guarded by `request_closing` / the lock.
 * P3. WinHTTP takes one WebSocket send at a time, and its buffer must stay
 *     valid until WRITE_COMPLETE (WinHttpWebSocketSend's documentation;
 *     today's send waits for that event, INFINITE after an abort, because
 *     the buffer is the caller's). Version 1 said "taken into the stack";
 *     version 2 makes "taken" the copy in a buffer of the socket's own,
 *     freed only after WinHTTP has reported the handle closing (Q4 a).
 *     WinHTTP's length is a DWORD: messages over 4 GiB - 1 stay refused,
 *     as today.
 * P4. libcurl moves a part-sent frame only when curl_ws_send is called
 *     again with the rest (today's crnet_curl_send_frame loops on the
 *     offset for that reason); nothing drives it between calls. Version 1
 *     had no call that did: a last message could sit half sent forever.
 *     Version 2 adds crnet_websocket_flush() (Q5) and has a wait set ask
 *     the descriptor for POLLOUT while a frame is part sent (Q10).
 * P5. libcurl refuses a new frame while one is unfinished, and two
 *     callers of curl_ws_send today besides send: recv (its echo of the
 *     peer's close frame, and the close frame it sends for a message over
 *     the limit) and close. Version 2: close with a message in flight
 *     returns TIMED_OUT and sends nothing (stated for every stack, T5);
 *     recv's own close frames first finish the frame in flight if that
 *     needs no waiting, and are otherwise left unsent - the connection is
 *     ending either way, and recv's results don't change. For the build
 *     to check: whether libcurl's own automatic pong is held back behind
 *     a part-sent data frame or refused (the test sends a ping during
 *     one).
 * P6. libcurl built without an asynchronous resolver (no threaded
 *     resolver, no c-ares; CURL_VERSION_ASYNCHDNS clear) looks names up
 *     inside curl_multi_perform, which then blocks for as long as the name
 *     server takes - a CRNET_NO_WAIT call included. Version 1 didn't know;
 *     Q9 asks. The threaded resolver's thread is libcurl's, as
 *     NSURLSession's and WinHTTP's are theirs (crnet.h, "Threads").
 * P7. libcurl in a wait set: a transfer's descriptors are its multi
 *     handle's and change from one curl_multi_perform to the next, and
 *     libcurl has timers of its own (happy eyeballs, its resolver's) that
 *     need a curl_multi_perform when they fire. Version 2: the set asks the
 *     member for its descriptors at every wait (curl_multi_waitfds from
 *     libcurl 8.8, curl_multi_fdset's sets turned into poll entries
 *     before), and shortens its poll() to curl_multi_timeout's answer; a
 *     wait that ends for libcurl's timer names the transfer, which is a
 *     reason to try, as the header already says of every naming.
 * P8. libcurl pauses a transfer whose response bytes the caller hasn't
 *     taken (the `pending` piece and `recv_paused`): its descriptor may stay
 *     readable while libcurl won't read. Version 2: a transfer with a
 *     pending piece is named INCOMING from the piece, without a poll entry,
 *     as `report_once` does for a libcurl WebSocket.
 * P9. Winsock: WSAPoll before Windows 10 version 2004 doesn't report a
 *     non-blocking connect that failed (no POLLERR/POLLHUP; Microsoft's
 *     documentation of WSAPoll). A TCP socket connecting in a wait set on
 *     an older Windows is named for a connection made, never for one
 *     refused. A known problem on crnet_wait_set_add_tcp_socket(); the
 *     socket's own connect_continue always reports it.
 * P10. NSURLSession: the message in flight is Foundation's copy (NSData,
 *     from the system's allocator), not the instance's allocator, as
 *     today's send already does: memory the platform allocates stays the
 *     platform's (carrottus/CLAUDE.md). Stated on send.
 * P11. watchOS: no WebSocket, no TCP; a wait set there (Q7 a) holds HTTP
 *     transfers only, woken by NSURLSession through the set's pipe.
 *     pipe() and poll() are believed open to every application there (they
 *     are not network sockets); not run on watchOS, said on the group.
 *
 * ---------------------------------------------------------------------------
 * Findings, second critique pass (version 3): misuse and threading
 * ---------------------------------------------------------------------------
 *
 * T1. Sides. crnet_tcp_socket_connect_continue() and
 *     crnet_websocket_connect_continue() are calls on the whole object, as
 *     connect is: nothing else on it may overlap them but abort (the other
 *     calls refuse a socket that isn't open, so there is nothing to
 *     overlap). crnet_websocket_flush() is a sending call: it may overlap a
 *     receiving call and abort, never send or close. Stated on each.
 * T2. A continue on a socket whose connect was never called, or has already
 *     returned an outcome (success or failure), returns
 *     CR_RESULT_NET_INVALID_TCP_SOCKET / _INVALID_WEBSOCKET, the result
 *     the header already gives for "connect has already been called". A
 *     second connect on a connecting socket gets the same.
 * T3. Abort with a step under way: the step is ended, its continuing call
 *     (or the next call) returns CR_RESULT_NET_ABORTED, and only destroy
 *     is left, as for every abort. WinHTTP's copy of a message in flight is
 *     freed only after WinHTTP reports the WebSocket handle closing, which
 *     destroy already waits for (`handles_closed_event`).
 * T4. Destroy with a step under way drops it: a connection half open, a
 *     request part sent, a message taken and not yet gone. Stated on send:
 *     a taken message is not delivered by destroy; flush first.
 * T5. Close with a message in flight returns CR_RESULT_NET_TIMED_OUT and
 *     sends nothing: the close frame can't go ahead of the message's last
 *     bytes on any stack (libcurl refuses it, P5; WinHTTP's
 *     WinHttpWebSocketSend refuses a send while "a close or send is
 *     pending", ERROR_INVALID_OPERATION, and says nothing of a shutdown
 *     behind a pending send, C4.4), and close takes no time limit, so it never
 *     waits for the message. The socket is not closed: recv doesn't start
 *     returning CR_RESULT_NET_WS_CLOSED. The caller flushes, then closes.
 * T6. Errors. A step's failure is recorded on the side of the call that
 *     returns it: a connect's on the receiving side (as today), a message's
 *     on the sending side by the send, flush or close that finds it.
 *     CR_RESULT_NET_TIMED_OUT records nothing, as today, so a loop that
 *     carries a step on with CRNET_NO_WAIT doesn't wear a real failure's
 *     record away. When card S10 reaches Carrottus Net, a failure the
 *     platform reported on its own thread goes to the instance's message
 *     callback from the object's pending slot, inside the call that returns
 *     it (crcore.h, M3): never from WinHTTP's, NSURLSession's or libcurl's
 *     threads.
 * T7. A failed in-flight message ends the connection (as a failed send
 *     does today): the send, flush or close that finds it returns
 *     CR_RESULT_NET_SEND_FAILED and only destroy is left on the sending
 *     side; recv reports the broken connection as it does today.
 * T8. Each continuing call's limit counts from that call; no limit spans
 *     the step. A caller's overall deadline is its own, as "Time limits"
 *     already says of a whole HTTP exchange.
 * T9. Wait sets. An HTTP transfer goes in one set at a time (as a
 *     WebSocket does) and is removed before it is destroyed. While it is
 *     in a set, a wait on the set counts as a call on the transfer, which
 *     has no sides: no call on the transfer overlaps a wait on its set.
 *     Its stack's notifications reach the set through the same attach and
 *     detach protocol as a WebSocket's (crnetshared.h), so a detach is
 *     never followed by a notify.
 * T10. A connecting object in a wait set: crnet_wait_set_add_tcp_socket()
 *     and crnet_wait_set_add_websocket() take a socket whose connect is
 *     under way (today: connected only). It is named, whatever bits it was
 *     added with, once its connect has an outcome; the caller then calls
 *     `_connect_continue`. A socket whose connect hasn't been called is
 *     still refused.
 * T11. Debug overlap checks (the workspace's threading rule) open each new
 *     call on its object and side, and a test makes the forbidden overlaps
 *     (two flushes, a flush and a send, a continue and a send) in Debug.
 * T12. A text message that isn't UTF-8 is refused before anything is
 *     taken (as today, CR_RESULT_NET_SEND_FAILED until S5), and doesn't
 *     touch the message in flight.
 * T13. Allocation: the WinHTTP and libcurl message buffers grow to the
 *     largest message and are kept until destroy, freed with their
 *     capacity; a socket that sends nothing allocates nothing for it. The
 *     cr_noalloc_tests idle round gains a message a frame, sent with
 *     CRNET_NO_WAIT and flushed.
 * T14. The front end's early return (src/net/crnethttp.c: "with no
 *     waiting allowed nothing is done") goes, and crnetshared.h's
 *     crnet_http_backend_start loses "its time limit is not
 *     CRNET_NO_WAIT"; a backend's start then returns TIMED_OUT with the
 *     request under way, and the front end puts the transfer in
 *     CRNET_HTTP_PHASE_SENDING either way (crnet_http_settle no longer ends
 *     it for a time-out). A backend's send and receive_response first
 *     finish a start under way.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * The platforms: WinHTTP's asynchronous mode (WinHttpSendRequest,
 * WinHttpReceiveResponse, WinHttpWebSocketSend and their completions,
 * calls from inside the status callback, one outstanding send per
 * WebSocket); NSURLSession (tasks running on the session's delegate queue,
 * URLSessionWebSocketTask's sendMessage:completionHandler:, no connection
 * event for a data task); libcurl's multi interface (curl_multi_perform,
 * curl_multi_poll, curl_multi_waitfds, curl_multi_fdset,
 * curl_multi_timeout, CONNECT_ONLY 2 through the multi handle, curl_ws_send
 * partial sends, CURL_VERSION_ASYNCHDNS); BSD sockets' and Winsock's
 * non-blocking connect (EINPROGRESS, WSAEWOULDBLOCK, SO_ERROR, WSAPoll's
 * connect-failure gap before Windows 10 2004).
 *
 * Industry practice: non-blocking sockets and connect() with poll, epoll
 * and kqueue (a step begun, finished when the descriptor says so, the
 * outcome read on the next call); libcurl's own multi model (one
 * perform call drives everything, the caller sleeps in a poll over
 * libcurl's descriptors and timer); Rust's futures (poll returns Pending
 * and the next poll carries the work on; a waker says when to poll, as a
 * wait set names an object); Go's net/http over its netpoller; Python's
 * asyncio transports (write() buffers whole and a drain() waits for it -
 * Q4 a and Q5 a are that shape). Browsers' WebSocket.send() takes the whole
 * message into a buffer and reports bufferedAmount; Q4 a keeps one message
 * instead of an unbounded buffer, leaving the queue to the caller.
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * Breaks, before 1.0, each listed against abi.txt (./check.sh --update-abi)
 * and in the commit:
 * - crnet_wait_set_add_websocket() gains `uint32_t wait_for` before `user`
 *   (Q10 a). Every caller passes CRNET_WAIT_FOR_INCOMING.
 * - Behaviour, no signature: crnet_tcp_socket_connect(),
 *   crnet_http_transfer_start(), crnet_websocket_connect() and
 *   crnet_websocket_send() with CRNET_NO_WAIT begin their step where they
 *   did nothing; a time-out from any of them leaves the object usable
 *   where it ended it (Q1 a); crnet_websocket_send() returns once the
 *   message is taken, not once it is on the connection (Q4 a);
 *   crnet_websocket_close() returns CR_RESULT_NET_TIMED_OUT with a
 *   message in flight (T5). A caller that destroyed the object after
 *   TIMED_OUT behaves as before; one that sent and destroyed at once now
 *   flushes in between (T4).
 * - crnet_wait_set and its calls are declared on every platform (Q7 a;
 *   an addition on watchOS).
 * - crnet_http_client_create() and crnet_websocket_create() (Q9 a)
 *   may return CR_RESULT_NET_UNSUPPORTED on Linux for a reason they didn't.
 * - [ADDED] (C9.2) Behaviour, no signature, missing from the list before:
 *   crnet_http_transfer_send() and _receive_response() carry a start on
 *   and return its failures (CR_RESULT_NET_HTTP_FAILED and the network
 *   failures from send, which listed only SEND_FAILED's); send returns
 *   CR_RESULT_NET_TIMED_OUT while the request isn't out;
 *   crnet_wait_set_add_tcp_socket() and _add_websocket() take a socket
 *   whose connect is under way; CRNET_WAIT_FOR_ROOM's meaning widens to
 *   HTTP transfers and WebSockets (no value changes);
 *   crnet_websocket_connect() lists CR_RESULT_OUT_OF_MEMORY (documented,
 *   not new); CR_RESULT_NET_TIMED_OUT's comment.
 * Added: crnet_tcp_socket_connect_continue(),
 * crnet_websocket_connect_continue(), crnet_websocket_flush(),
 * crnet_wait_set_add_http_transfer(), crnet_wait_set_remove_http_transfer().
 * No new result code, struct, struct type or enum value. carrottus goes to
 * 0.3.0 (CARROTTUS_VERSION_MINOR 3): a minor version may break before 1.0.
 *
 * ---------------------------------------------------------------------------
 * Tests (crnet_test, extended; every backend; and the harnesses)
 * ---------------------------------------------------------------------------
 *
 * Each new case seen failing against today's code first. A new part,
 * "resumable", plus changes to "tcp", "http", "websocket" and "wait":
 * - The no-thread loop, per object: a TCP connect, an HTTP GET to /slow, a
 *   POST with a sized body, an UNSIZED upload, a WebSocket opening, and a
 *   burst of WebSocket messages, each driven to the end by calls with
 *   CRNET_NO_WAIT only, the thread sleeping between them in
 *   crnet_wait_set_wait() and never otherwise: every step finishes, the
 *   echo matches, and the thread never sits in a Carrottus call longer than
 *   250 ms while the server holds every answer back for 1 s, so a call that
 *   waited would be caught whatever else the machine is doing; the
 *   longest call is printed (C8.2). [CHANGED] (C8.1) Where a stack may
 *   finish a step inside the call (a loopback connect, NSURLSession's
 *   start), the test takes CR_SUCCESS and CR_RESULT_NET_TIMED_OUT both and
 *   goes on; the steps it must see left going are held by the server
 *   (below). A TCP connect left going can't be forced on loopback on
 *   every system (Linux drops a SYN to a full backlog, Windows refuses
 *   it), so that case is in the `internet` part, against 192.0.2.1
 *   (TEST-NET-1, which never answers); on loopback only its outcomes.
 * - [ADDED] (C8.3) The test server gains: `/slow-upgrade?ms=`, which holds
 *   the WebSocket 101 back; WebSocket text messages `pause:ms` (stop
 *   reading for ms, so a large message stays in flight) and
 *   `pause-and-ping:ms` (the same, with a ping sent meanwhile); and
 *   `/slow-read?ms=`, which reads a request body slowly, so that an HTTP
 *   transfer's ROOM can be seen come and go.
 * - A connect that is refused, and a wss:// opening whose certificate is
 *   turned away, each reported by `_connect_continue` with today's result
 *   and record; a WebSocket upgrade refused (/refuse) with its 401 in
 *   `out_http_status`; an HTTP start to a closed port reported by the next
 *   call; a TCP connect to a host that never answers (the `internet` part)
 *   still TIMED_OUT after several continues and then aborted.
 * - A longer limit that runs out leaves the object usable (Q1): a start
 *   with 1 ms to /slow, then receive_response gets the response; a send
 *   with 1 ms behind a large message, then the same message sent again
 *   once flush succeeds.
 * - WebSocket send: a 64 MiB message sent with CRNET_NO_WAIT is taken
 *   (CR_SUCCESS) and the next send is TIMED_OUT until flush succeeds;
 *   the server echoes both whole and in order; a message sent and the
 *   socket destroyed at once may be lost, flushed first it arrives;
 *   close with a message in flight is TIMED_OUT and recv still works;
 *   the caller's buffer overwritten right after send doesn't change what
 *   arrives (the copy); a ping from the server during a part-sent frame
 *   is answered (P5, libcurl).
 * - Aborts: from another thread, during each step under way; the
 *   continuing call returns ABORTED; on WinHTTP the socket is destroyed
 *   at once after an abort with a message in flight, under ASan.
 * - Wait set: an HTTP transfer named INCOMING for its response and its
 *   body, ROOM for its request body; a connecting TCP socket and
 *   WebSocket named once their connect has an outcome, refused and
 *   accepted alike; a WebSocket asked ROOM named once its message in
 *   flight has gone; on libcurl, a transfer whose timer fires named (a
 *   reason to try).
 * - Misuse, in the misuse harness and crnet_test: every new call with
 *   zeroes and with a destroyed handle; continue without a connect, after
 *   an outcome, twice; a transfer in two sets; the overlap checks (T11).
 *   (C5.5) Every new call checks its handle with CR_STALE right after its
 *   null check, the same checks in the same order on every backend; the
 *   misuse generator gains a started-transfer factory for
 *   crnet_wait_set_add_http_transfer().
 * - [ADDED] (C8.5) The overlap checks of T11 need the workspace's Debug
 *   overlap check (card X3), which Carrottus Net doesn't have yet: they
 *   are built with it, and until then T11's forbidden overlaps are tested
 *   as far as TSan sees them. (C8.7) The two-thread cases, each run under
 *   -tsan: an abort from another thread during each step under way; a
 *   recv while another thread flushes; a wait on a set while another
 *   thread sends on a member.
 * - (C8.8) The `resumable` part also runs in the iOS simulator
 *   (CRNET_TEST_LAUNCHER), as the rest of crnet_test does.
 * - cr_noalloc_tests: the idle round sends one WebSocket message a frame
 *   with CRNET_NO_WAIT and flushes it, no allocation of Carrottus's after
 *   the first (T13).
 * - The libcurl create calls on a libcurl without ASYNCHDNS (Q9): not
 *   reachable on the test machines; checked by reading, and by a build
 *   against a libcurl configured with --disable-threaded-resolver where
 *   one is at hand.
 */

#ifndef CRNET_RESUMABLE_DRAFT_H
#define CRNET_RESUMABLE_DRAFT_H

#include "crnet.h"

#if __cplusplus
extern "C" {
#endif

/* The draft's way of naming a [CHANGED] signature beside today's, so the
 * file compiles: crnet_wait_set_add_websocket becomes
 * crnet_wait_set_add_websocket_as_changed here, and keeps its own name in
 * crnet.h. Not part of the proposal. */
#define CRNET_DRAFT_CHANGED(name) name##_as_changed

/* ======================================================================== */
/* crnet.h, the overview                                                    */
/* ======================================================================== */

/* [CHANGED] The bullet on calls, in the module's overview:
 *
 *     - Every call returns at once or blocks the thread it was made on, for
 *       no longer than its time limit; Carrottus Net starts no thread
 *       (@ref crnet_threads "Threads"). A call that can wait takes a time
 *       limit (@ref crnet_time "Time limits"), and with CRNET_NO_WAIT
 *       never waits: what has to wait for the other side is begun and
 *       left to go on, and a later call carries it on. Every wait can be
 *       ended by the object's `_abort` (@ref crnet_aborting "Aborting").
 *
 * (Today: "and has no non-blocking mode".)
 *
 * [CHANGED] "Threads", its first bullet gains:
 *
 *     A step begun and left (@ref crnet_time "Time limits") goes on
 *     where the stack goes on by itself - WinHTTP's and NSURLSession's
 *     threads, the system's TCP connect - and otherwise in the caller's
 *     next call on the object: libcurl moves only inside a call. Either
 *     way nothing of the caller's runs anywhere but its own calls.
 */

/* ======================================================================== */
/* crnet.h, "Time limits" [CHANGED]                                         */
/* ======================================================================== */

/* The paragraphs on CRNET_NO_WAIT become:
 *
 *     CRNET_NO_WAIT (0) means the call may not wait at all. It does
 *     whatever can be done on the spot and returns CR_RESULT_NET_TIMED_OUT
 *     if that leaves its part undone:
 *
 *     - crnet_tcp_socket_recv(), crnet_websocket_recv() and
 *       crnet_http_transfer_recv() hand over what has already arrived;
 *       crnet_tcp_listener_accept(), a connection already waiting;
 *       crnet_http_transfer_receive_response(), a response that has
 *       already come.
 *     - crnet_tcp_socket_send() and crnet_http_transfer_send() send as
 *       much as will be taken right now.
 *     - The calls that begin a step the other side has to answer -
 *       crnet_tcp_socket_connect(), crnet_websocket_connect(),
 *       crnet_http_transfer_start() - begin it and return; and
 *       crnet_websocket_send() takes the whole message, or nothing when the
 *       one before it hasn't gone yet. See "Steps" below.
 *
 *     This is what replaces a non-blocking mode: a loop that can't stall
 *     (a frame loop) passes CRNET_NO_WAIT to every call, and sleeps, when
 *     it has nothing to do, in crnet_wait_set_wait().
 *
 * And a new paragraph, "Steps":
 *
 *     Four calls begin something the other side has to answer before it
 *     is done - a step: crnet_tcp_socket_connect() and
 *     crnet_websocket_connect() open a connection,
 *     crnet_http_transfer_start() gets a request out, and
 *     crnet_websocket_send() gets a message onto its connection. Each
 *     begins its step whatever its limit and waits for it no longer than
 *     the limit. When the limit runs out first it returns
 *     CR_RESULT_NET_TIMED_OUT and the step goes on; a later call carries
 *     it on, within that call's own limit, and returns how it ended:
 *
 *     | Step begun by                | Carried on by                                              |
 *     |------------------------------|------------------------------------------------------------|
 *     | crnet_tcp_socket_connect()   | crnet_tcp_socket_connect_continue()                        |
 *     | crnet_websocket_connect()    | crnet_websocket_connect_continue()                         |
 *     | crnet_http_transfer_start()  | the next call in the order of use: _send or _receive_response |
 *     | crnet_websocket_send()       | the next crnet_websocket_send(), crnet_websocket_flush()   |
 *
 *     Nothing has failed when a step is left going, and nothing of the
 *     caller's is held: what the step needs was copied by the call that
 *     began it. A caller that no longer wants it aborts the object or
 *     destroys it, which drops the step. No limit spans a step: each call
 *     has its own, and a caller with a deadline for the whole keeps it.
 *
 *     So CR_RESULT_NET_TIMED_OUT, from any call in this header, means the
 *     same two things: the caller's limit is what ended the call, and the
 *     object is as usable as it was - calling again is in order.
 *
 * And the paragraph on CR_RESULT_NET_TIMED_OUT under "Somebody else's
 * timer" is unchanged.
 */

/* ======================================================================== */
/* crnet.h, TCP sockets                                                     */
/* ======================================================================== */

#if CRNET_EXT_TCP

/**
 * [CHANGED] (Q1, Q2, Q8) Begins connecting to a listening address and
 * port, and waits up to `desc->timeout_ms` for the connection. Once per
 * socket.
 *
 * Why it changes: with CRNET_NO_WAIT it did nothing, and a limit that ran
 * out ended the socket, so a caller that may not wait could never connect.
 * The system's non-blocking connect() goes on without anyone waiting.
 *
 * The step: the system's own connection attempt, begun by this call and
 * left to go on when the limit runs out. crnet_tcp_socket_connect_continue()
 * carries it on and says how it ended. Until then the socket counts as not
 * connected: every other call but abort, destroy and get_error returns
 * CR_RESULT_NET_INVALID_TCP_SOCKET, and a second connect does too.
 *
 * What is kept: the socket's descriptor or Winsock SOCKET, already the
 * socket's, and a flag saying its connect is under way. Nothing else.
 *
 * [ADDED] (C4.10) A system may connect at once - a loopback connect()
 * often completes inside the call - so this may return CR_SUCCESS with
 * CRNET_NO_WAIT too. A caller treats CR_SUCCESS and CR_RESULT_NET_TIMED_OUT
 * as the two answers it can get, whatever the limit.
 *
 * @param[in] socket The socket to connect.
 * @param[in] desc   The address, port and time limit. Only read during the
 *                   call.
 *
 * @retval CR_SUCCESS The socket is connected.
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET `socket` is NULL, or connect
 *         has already been called on it.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `desc->address` is NULL.
 * @retval CR_RESULT_NET_INVALID_ADDRESS `desc->address` isn't a numeric
 *         address.
 * @retval CR_RESULT_NET_ABORTED crnet_tcp_socket_abort() was called.
 * @retval CR_RESULT_NET_TIMED_OUT [CHANGED] The connection isn't made
 *         yet; the attempt goes on. Carry it on with
 *         crnet_tcp_socket_connect_continue(). (Today: the socket was
 *         spent, or, with CRNET_NO_WAIT, nothing was done.)
 * @retval CR_RESULT_NET_CONNECT_FAILED The other end refuses or can't
 *         be reached.
 * @retval CR_RESULT_NET_SYSTEM_TIMED_OUT Nothing answered and the
 *         operating system stopped trying (@ref crnet_time "Time limits").
 */
CR_RESULT crnet_tcp_socket_connect(
    crnet_tcp_socket socket,
    const crnet_tcp_socket_connect_desc *desc);

/**
 * [ADDED] (Q2 a, Q8 a) Carries on a connect that returned
 * CR_RESULT_NET_TIMED_OUT, waiting up to `timeout_ms` for its outcome.
 *
 * Why it earns its place: the connect's outcome has to be read somewhere,
 * and the desc was taken whole by connect; this takes only what a
 * continuation needs, the time limit. It is the non-blocking connect's
 * second half - poll for writable, then SO_ERROR - as one call.
 *
 * Thread: a call on the whole socket, as connect is: only
 * crnet_tcp_socket_abort() may overlap it.
 *
 * @param[in] socket     The socket whose connect is under way.
 * @param[in] timeout_ms The longest to wait (@ref crnet_time "Time
 *                       limits"); CRNET_NO_WAIT only looks.
 *
 * @retval CR_SUCCESS The socket is connected.
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET `socket` is NULL; connect was
 *         never called on it; or connect, or an earlier continue, has
 *         already returned the outcome.
 * @retval CR_RESULT_NET_ABORTED crnet_tcp_socket_abort() was called.
 * @retval CR_RESULT_NET_TIMED_OUT Still not made; the attempt goes on and
 *         this may be called again.
 * @retval CR_RESULT_NET_CONNECT_FAILED The other end refused or couldn't be
 *         reached; crnet_tcp_socket_get_error() (receiving side) has the
 *         system's reason. Only destroy is left.
 * @retval CR_RESULT_NET_SYSTEM_TIMED_OUT Nothing answered and the
 *         operating system stopped trying. Only destroy is left.
 */
CR_RESULT crnet_tcp_socket_connect_continue(
    crnet_tcp_socket socket,
    uint32_t timeout_ms);

#endif // CRNET_EXT_TCP

/* ======================================================================== */
/* crnet.h, HTTP                                                            */
/* ======================================================================== */

/* [CHANGED] The HTTP group's known problems lose "and an HTTP transfer
 * can't go in a wait set" (Q6 a).
 *
 * [CHANGED] Its order of use gains one sentence under the table:
 *
 *     A start that returns CR_RESULT_NET_TIMED_OUT has started the
 *     transfer: the request is under way, and the next call in this order
 *     - send, or receive_response - carries it on before its own part.
 *
 * [CHANGED] "A transfer is good for one request: after any call on it
 * fails - other than by running out of time where that call's own comment
 * says it may be tried again - only destroy is left" becomes "after any
 * call on it fails, other than with CR_RESULT_NET_TIMED_OUT, only destroy
 * is left." (Every call's time-out may now be tried again, Q1 a.)
 */

/**
 * [CHANGED] (Q1, Q3) Starts the request: hands the request line and the
 * headers to the stack, on a connection the client already had open to
 * that server or a new one, and waits up to `desc->timeout_ms` for them to
 * be out. Known problem: starting a second transfer on a client that
 * already has one in progress is refused with
 * CR_RESULT_NET_INVALID_HTTP_TRANSFER on libcurl and NSURLSession, and is
 * not checked on WinHTTP.
 *
 * Why it changes: with CRNET_NO_WAIT it did nothing, and a limit that ran
 * out ended the transfer, so a caller that may not wait could never send a
 * request.
 *
 * The step: getting the request out - looking the host up, connecting,
 * TLS, sending the line and headers - which the stack does on its own
 * threads (WinHTTP, NSURLSession) or in later calls (libcurl). Started
 * whichever this returns, CR_SUCCESS or CR_RESULT_NET_TIMED_OUT: the
 * transfer moves on to the next call in the order of use, which carries a
 * start that isn't finished on before its own part and reports what it
 * failed with. How far the step has got when this returns is the stack's:
 * NSURLSession says nothing of connecting, so there this returns
 * CR_SUCCESS at once whatever the limit, and a server that can't be
 * reached is reported by crnet_http_transfer_receive_response(), as today.
 *
 * What is kept: nothing new. The URL, method and headers are copied by the
 * stack at this call (WinHttpOpenRequest, WinHttpAddRequestHeaders;
 * curl_easy_setopt's string copies and the slist the transfer already
 * owns; NSMutableURLRequest), and the transfer already keeps its handles
 * and events.
 *
 * @param[in] transfer The transfer to start. Once per transfer.
 * @param[in] desc     The request. It and everything it points at only have
 *                     to last until this returns.
 *
 * @retval CR_SUCCESS The request is out, or (NSURLSession) handed to the
 *         stack.
 * @retval CR_RESULT_NET_INVALID_HTTP_TRANSFER `transfer` is NULL, it
 *         has already been started (a start that returned TIMED_OUT
 *         included), or (libcurl, NSURLSession) its client has another
 *         transfer in progress.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NET_ABORTED crnet_http_transfer_abort() was called.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS A field is missing, a header is
 *         one the caller may not set, `body_size` or `follow_redirects`
 *         doesn't go with `body`, or text isn't UTF-8.
 * @retval CR_RESULT_NET_INVALID_URL `desc->url` is not a URL, or not
 *         http:// or https://.
 * @retval CR_RESULT_NET_TIMED_OUT [CHANGED] The transfer has started and
 *         the request isn't out yet; the next call in the order of use
 *         carries it on. (Today: the transfer was ended, or, with
 *         CRNET_NO_WAIT, nothing was done.)
 * @retval CR_RESULT_OUT_OF_MEMORY An allocation failed.
 * @return Otherwise one of the network failures under
 *         @ref crnet_stack "HTTP and WebSocket", or
 *         CR_RESULT_NET_HTTP_FAILED.
 */
CR_RESULT crnet_http_transfer_start(
    crnet_http_transfer transfer,
    const crnet_http_transfer_desc *desc);

/**
 * [CHANGED] (Q3 a) Waits for the stack to take request body, then hands it
 * as much of `data` as it takes in one go. Unchanged but for one paragraph,
 * added:
 *
 * A start that returned CR_RESULT_NET_TIMED_OUT is carried on first, out
 * of this call's limit: until the request is out, no body is taken and
 * this returns CR_RESULT_NET_TIMED_OUT (nothing taken; send again). A
 * failure of the start - the network failures under
 * @ref crnet_stack "HTTP and WebSocket", CR_RESULT_NET_HTTP_FAILED -
 * comes from here, and ends the transfer.
 */
CR_RESULT crnet_http_transfer_send(
    crnet_http_transfer transfer,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_sent);

/**
 * [CHANGED] (Q3 a) Says the request is complete and waits for the
 * response's status and headers. Unchanged but for one paragraph, added:
 *
 * A start that returned CR_RESULT_NET_TIMED_OUT is carried on first, out
 * of this call's limit, and its failure comes from here (as NSURLSession's
 * already does today).
 */
CR_RESULT crnet_http_transfer_receive_response(
    crnet_http_transfer transfer,
    uint32_t timeout_ms);

/* ======================================================================== */
/* crnet.h, WebSocket client                                                */
/* ======================================================================== */

#if CRNET_EXT_WEBSOCKET

/* [CHANGED] The group's order of use becomes: "create, connect once
 * (continued with crnet_websocket_connect_continue() until it has an
 * outcome), then send and recv for as long as the connection lasts, then
 * destroy." And its Thread paragraph names crnet_websocket_flush() among
 * the sending calls, and crnet_websocket_connect_continue() among the calls
 * nothing overlaps but abort.
 */

/**
 * [CHANGED] (Q1, Q2) Begins opening the connection, and waits up to
 * `desc->timeout_ms` for the server to accept it. Known problem: the
 * negotiated subprotocol and the opening response's headers can't be read
 * afterwards, so neither can the target of a redirect that was handed back.
 *
 * Why it changes: with CRNET_NO_WAIT it did nothing, and a limit that ran
 * out ended the socket, so a caller that may not wait could never open
 * one.
 *
 * The step: the opening request and its answer - lookup, connect, TLS,
 * the upgrade request, the 101 - which the stack does on its own threads
 * (WinHTTP: the status callback issues WinHttpReceiveResponse once the
 * request is out; NSURLSession: the task) or in later calls (libcurl: the
 * socket's multi handle, driven by each continue). When the limit runs out
 * first this returns CR_RESULT_NET_TIMED_OUT and the opening goes on;
 * crnet_websocket_connect_continue() carries it on and says how it ended.
 * Until then the socket counts as not connected: send, recv and close
 * return CR_RESULT_NET_INVALID_WEBSOCKET, and a second connect does too.
 *
 * What is kept, beyond what the socket keeps today: libcurl - the easy
 * handle stays on the socket's multi handle between calls (it already
 * holds the headers' slist and TLS blobs), as CURLOPT_CONNECT_ONLY's
 * documentation requires of a connect-only handle for as long as its
 * connection is used (C4.2); WinHTTP - the request handle,
 * the stage the opening has reached and its error, under the socket's lock
 * (the URL's wide strings are copied by WinHttpOpenRequest and freed here);
 * NSURLSession - the task and the state's `open_state`, which a time-out no
 * longer sets to "spent".
 *
 * [ADDED] (C7.3) On WinHTTP the status callback issues
 * WinHttpReceiveResponse on a WinHTTP thread while the caller may be
 * aborting: it holds the socket's lock and checks that the request handle
 * hasn't been closed, as the certificate check in the same callback
 * already does, and a refusal is recorded as the opening's error, its
 * event set and the socket's wait set notified. (C7.2) On NSURLSession every
 * completion block, the opening's and each message's, captures the
 * socket's retained state object, never the socket itself, so a
 * completion that arrives after destroy writes into memory that is still
 * there and is let go with the last of them.
 *
 * @param[in]  socket          The socket to connect. Once per socket.
 * @param[in]  desc            As today. It and everything it points at only
 *                             have to last until this returns.
 * @param[out] out_http_status As today; 0 with CR_RESULT_NET_TIMED_OUT.
 *                             May be NULL.
 *
 * @retval CR_SUCCESS The socket is connected.
 * @retval CR_RESULT_NET_INVALID_WEBSOCKET `socket` is NULL, or connect
 *         has already been called on it.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS As today.
 * @retval CR_RESULT_NET_INVALID_URL `desc->url` is not a URL, or not
 *         ws:// or wss://.
 * @retval CR_RESULT_NET_ABORTED crnet_websocket_abort() was called.
 * @retval CR_RESULT_NET_TIMED_OUT [CHANGED] Not open yet; the opening goes
 *         on. Carry it on with crnet_websocket_connect_continue(). (Today:
 *         the socket was spent, or, with CRNET_NO_WAIT, nothing was done.)
 * @retval CR_RESULT_NET_WS_UPGRADE_REFUSED The server answered, but not
 *         with 101; `out_http_status` has what it answered.
 * @retval CR_RESULT_NET_UNSUPPORTED libcurl: its TLS library can't do
 *         what `desc->tls` asks. Found here, before the opening is begun,
 *         so never from crnet_websocket_connect_continue().
 * @retval CR_RESULT_OUT_OF_MEMORY [ADDED] (C3.3) An allocation failed
 *         (today unlisted, though WinHTTP and libcurl can return it); the
 *         same result connect_continue lists.
 * @return Otherwise one of the network failures under
 *         @ref crnet_stack "HTTP and WebSocket".
 */
CR_RESULT crnet_websocket_connect(
    crnet_websocket socket,
    const crnet_websocket_connect_desc *desc,
    uint32_t *out_http_status);

/**
 * [ADDED] (Q2 a) Carries on an opening that returned
 * CR_RESULT_NET_TIMED_OUT, waiting up to `timeout_ms` for its outcome.
 *
 * Why it earns its place: an opening has an outcome and a status to hand
 * back, and the desc was taken whole by connect; this takes the time limit
 * and nothing else, and returns exactly what connect would have.
 *
 * On libcurl this is where the opening moves: a caller that stops calling
 * it leaves the opening standing (and the server may give up on it). On
 * WinHTTP and NSURLSession the opening goes on by itself and this reads
 * where it has got to, and on WinHTTP completes the upgrade.
 *
 * Thread: a call on the whole socket, as connect is: only
 * crnet_websocket_abort() may overlap it.
 *
 * @param[in]  socket          The socket whose opening is under way.
 * @param[in]  timeout_ms      The longest to wait (@ref crnet_time "Time
 *                             limits"); CRNET_NO_WAIT only looks (and on
 *                             libcurl makes the one pass that costs no
 *                             waiting).
 * @param[out] out_http_status As connect's: 101 on success; with
 *                             CR_RESULT_NET_WS_UPGRADE_REFUSED what the
 *                             server answered instead; 0 where no answer has
 *                             arrived, CR_RESULT_NET_TIMED_OUT included. May
 *                             be NULL.
 *
 * @retval CR_SUCCESS The socket is connected.
 * @retval CR_RESULT_NET_INVALID_WEBSOCKET `socket` is NULL; connect was
 *         never called on it; or connect, or an earlier continue, has
 *         already returned the outcome.
 * @retval CR_RESULT_NET_ABORTED crnet_websocket_abort() was called.
 * @retval CR_RESULT_NET_TIMED_OUT Not open yet; the opening goes on and
 *         this may be called again.
 * @retval CR_RESULT_NET_WS_UPGRADE_REFUSED The server answered, but not
 *         with 101. Only destroy is left.
 * @retval CR_RESULT_OUT_OF_MEMORY An allocation failed. Only destroy is
 *         left.
 * @return Otherwise one of the network failures under
 *         @ref crnet_stack "HTTP and WebSocket", recorded on the receiving
 *         side. Only destroy is left.
 */
CR_RESULT crnet_websocket_connect_continue(
    crnet_websocket socket,
    uint32_t timeout_ms,
    uint32_t *out_http_status);

/**
 * [CHANGED] (Q1, Q4) Takes one whole message to send, or none of it. Known
 * problem: a bad argument - `data` NULL with a `size` above 0, a size the
 * stack can't carry, text that isn't UTF-8 - returns
 * CR_RESULT_NET_SEND_FAILED, the same as a broken connection, and not an
 * invalid-argument result (card S5 changes it).
 *
 * Why it changes: with CRNET_NO_WAIT it did nothing, and a limit that ran
 * out ended the connection, so a caller that may not wait could never
 * send, and one with a limit lost the connection to a slow peer.
 *
 * One message is in flight at a time: taken by a send and not yet all
 * handed to the connection. This call first carries on the one before
 * it, out of its limit. If that one is still in flight when the limit runs
 * out, this message is not taken: CR_RESULT_NET_TIMED_OUT, the connection
 * as it was, and the caller sends the same message again later (when a
 * wait set names the socket for CRNET_WAIT_FOR_ROOM, or on its next round).
 * Otherwise this message is taken whole and the call returns CR_SUCCESS at
 * once, without waiting for it to go out: its bytes have been copied, and
 * `data` is the caller's again. A limit is only ever spent on the message
 * before.
 *
 * Taken means copied, not sent and not acknowledged: the message goes on
 * to the connection behind this call, and crnet_websocket_flush() says when
 * it has gone. A message that fails on its way is reported by the next
 * send, flush or close (CR_RESULT_NET_SEND_FAILED, the sending side's
 * record), and the connection is over, as a failed send's is today.
 *
 * @warning A message taken and not yet gone is dropped by
 * crnet_websocket_destroy() and crnet_websocket_abort(). A caller that
 * needs it delivered before it lets go flushes first.
 *
 * Where the copy is: WinHTTP and libcurl - a buffer of the socket's own,
 * through the instance's allocator, grown to the largest message sent and
 * kept until destroy (freed, on WinHTTP, only after WinHTTP has reported the
 * handle closing); NSURLSession - the NSData of its message, which copies
 * the bytes with the system's allocator, as today. On libcurl the buffer
 * holds the part curl_ws_send hasn't consumed, which its documentation
 * requires be offered again - "buffer and buflen must be updated on every
 * following invocation to only point to the remaining piece", and after
 * CURLE_AGAIN "retry with the same data and length" (C4.1).
 *
 * [ADDED] (C6.1-C6.5) How the buffer is used, the same rules on WinHTTP
 * and libcurl:
 * - It is sized when a message is taken that doesn't fit: never at create,
 *   and never for a socket that sends nothing. It grows to exactly the
 *   message's size, no growth factor (a size of Carrottus's choosing would
 *   be a policy). A message is only taken when none is in flight, so the
 *   old block is freed before the new one is allocated: the socket never
 *   holds two. Bytes, alignment 1, freed with the capacity it was made
 *   with (CR_ALLOCATOR's contract).
 * - WinHTTP copies the whole message, since WinHTTP reads the buffer until
 *   WRITE_COMPLETE. libcurl first offers the caller's own bytes to
 *   curl_ws_send and copies only what it didn't consume, so on a
 *   connection with room nothing is copied; but the buffer is grown to the
 *   whole message before the first byte is offered, so that an allocation
 *   that fails leaves nothing on the wire and the message untaken.
 * - A 64 MiB message costs 64 MiB of the socket's (WinHTTP; libcurl at
 *   most), kept as C5.1 says; NSURLSession's copy is Foundation's and goes
 *   with its message once sent, so there the socket keeps nothing and
 *   each send allocates the system's memory, not the instance's.
 * - What else a socket holds for this: the buffer's pointer, capacity,
 *   size and offset, the message's type, an in-flight flag and the
 *   failure of a message in flight: tens of bytes in the socket's own
 *   block, allocated with it. A TCP socket adds one flag; a transfer adds
 *   the set it is in.
 *
 * [ADDED] (C5.1) What the buffer keeps is said here because it is a cost
 * the caller carries: once a message of N bytes has been sent, the socket
 * holds N bytes until it is destroyed. A caller that sends a rare large
 * message and wants the memory back sends it on a socket of its own, or
 * destroys the socket after it. (Whether it should be kept at all is
 * asked under "For the user" in CRNET_TODO.md, U1.)
 *
 * [ADDED] (C7.4-C7.6) The in-flight flag and the failure of the message in
 * flight are written by the stack's completion (WinHTTP's WRITE_COMPLETE
 * or REQUEST_ERROR, NSURLSession's completion handler) under the socket's
 * lock or condition - today's WinHTTP callback writes `send_error` without
 * it - which then notifies the socket's wait set (ROOM). send, flush and
 * close read them under the same lock, so a completion that lands during
 * one of them is seen either before or after it, never half. An abort
 * during a send or flush that is waiting ends the wait (WinHTTP's abort
 * event, NSURLSession's cancel, libcurl's eventfd), the message in flight
 * is dropped with the connection, and the call returns
 * CR_RESULT_NET_ABORTED.
 *
 * Thread: may overlap crnet_websocket_recv() and crnet_websocket_abort()
 * on the same socket.
 *
 * @param[in] socket     The connected socket to send on.
 * @param[in] type       Text or binary. A text message has to be valid
 *                       UTF-8.
 * @param[in] data       The message's bytes, only read during the call. May
 *                       be NULL when `size` is 0.
 * @param[in] size       How many bytes `data` holds; 0 sends an empty
 *                       message. At most 4 GiB - 1 on WinHTTP.
 * @param[in] timeout_ms The longest to wait for the message before this one
 *                       to go (@ref crnet_time "Time limits");
 *                       CRNET_NO_WAIT only looks.
 *
 * @retval CR_SUCCESS The message is taken, whole.
 * @retval CR_RESULT_NET_INVALID_WEBSOCKET `socket` is NULL or isn't
 *         connected.
 * @retval CR_RESULT_NET_ABORTED crnet_websocket_abort() was called.
 * @retval CR_RESULT_NET_TIMED_OUT [CHANGED] The message before is still
 *         in flight; nothing of this one was taken, and the connection is
 *         as it was. (Today: the connection was ended, or, with
 *         CRNET_NO_WAIT, nothing was sent.)
 * @retval CR_RESULT_OUT_OF_MEMORY The socket's buffer couldn't grow to the
 *         message; nothing was taken, and the connection is as it was.
 *         The instance's allocator refused, not the platform: nothing is
 *         recorded and no message is sent (card S10, M2) (C5.2).
 * @retval CR_RESULT_NET_SEND_FAILED A bad argument (see above), nothing
 *         taken; or the message before failed on its way, or the connection
 *         has ended, been closed or broken, and only destroy is left on the
 *         sending side.
 */
CR_RESULT crnet_websocket_send(
    crnet_websocket socket,
    CRNET_WEBSOCKET_MESSAGE type,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms);

/**
 * [ADDED] (Q5 a) Carries the message in flight on, and waits up to
 * `timeout_ms` for it to have gone to the connection.
 *
 * Why it earns its place: libcurl moves a part-sent message only when it is
 * called to, so a socket whose last message is still in flight needs a
 * call that moves it without bringing a new one; and a caller needs to
 * hear that a message it sent has gone, or failed, without sending
 * another. On WinHTTP and NSURLSession the message goes on by itself and
 * this only reads where it has got to.
 *
 * "Gone" is what CR_SUCCESS from today's send means: handed to the
 * connection, not acknowledged by the peer.
 *
 * Thread: a sending call: may overlap crnet_websocket_recv() and
 * crnet_websocket_abort() on the same socket; not send, close or another
 * flush.
 *
 * @param[in] socket     The connected socket.
 * @param[in] timeout_ms The longest to wait (@ref crnet_time "Time
 *                       limits"); CRNET_NO_WAIT only looks (and on libcurl
 *                       sends what fits right now).
 *
 * @retval CR_SUCCESS No message is in flight: the last one taken has gone,
 *         or none was taken.
 * @retval CR_RESULT_NET_INVALID_WEBSOCKET `socket` is NULL or isn't
 *         connected.
 * @retval CR_RESULT_NET_ABORTED crnet_websocket_abort() was called.
 * @retval CR_RESULT_NET_TIMED_OUT A message is still in flight; it goes on
 *         (on libcurl, when this or send is called again).
 * @retval CR_RESULT_NET_SEND_FAILED The message in flight failed on its
 *         way, or the connection has ended; the sending side's record has
 *         the platform's reason. Only destroy is left on the sending side.
 */
CR_RESULT crnet_websocket_flush(
    crnet_websocket socket,
    uint32_t timeout_ms);

/**
 * [CHANGED] (T5) Sends a close frame and ends the connection for this side
 * there and then. Unchanged but for one result and one paragraph:
 *
 * A close frame goes after the message in flight, never into it. With a
 * message still in flight this sends nothing and returns
 * CR_RESULT_NET_TIMED_OUT, without waiting (close takes no limit); the
 * socket is as it was - recv does not start returning
 * CR_RESULT_NET_WS_CLOSED - and the caller flushes, then closes again.
 *
 * @retval CR_RESULT_NET_TIMED_OUT [ADDED] A message is in flight; nothing
 *         was sent.
 */
CR_RESULT crnet_websocket_close(
    crnet_websocket socket,
    uint16_t code,
    const char *reason);

#endif // CRNET_EXT_WEBSOCKET

/* ======================================================================== */
/* crnet.h, Wait set                                                        */
/* ======================================================================== */

/* [CHANGED] (Q7 a) The wait set is declared on every platform:
 *
 *     #if CRNET_EXT_TCP || CRNET_EXT_WEBSOCKET   (removed)
 *
 * with its TCP and WebSocket calls still behind their own macros. This
 * draft keeps today's gate around what it declares, so that it compiles
 * beside today's header, where crnet_wait_set is declared only under it.
 *
 * [CHANGED] The group's brief: "TCP sockets, TCP listeners, WebSockets and
 * HTTP transfers can be in one." Its known problems lose "an HTTP transfer
 * can't be a member", and gain: "on Windows before Windows 10 version
 * 2004, WSAPoll doesn't report a TCP connect that failed, so a connecting
 * socket is named only for a connection made (P9); a libcurl transfer may
 * be named for libcurl's own timer with nothing to hand over; not run on
 * watchOS, where the set holds HTTP transfers only."
 *
 * [CHANGED] Its "How to use one" gains:
 *
 *     - A member whose connect is under way (a TCP socket, a WebSocket) is
 *       named, whatever bits it was added with, once the connect has an
 *       outcome. The call to make then is its `_connect_continue`.
 *
 * [CHANGED] Its "Whose is what" gains:
 *
 *     - Thread: while an HTTP transfer is in a set, a wait on the set
 *       counts as a call on the transfer: no call on the transfer may
 *       overlap it.
 *     - [ADDED] (C7.1) Thread: while a socket whose connect is under way
 *       is in a set, a wait on the set counts as a call on the whole
 *       socket: its `_connect_continue` may not overlap the wait (on
 *       libcurl both read and drive the socket's multi handle). Once the
 *       socket is connected, the wait counts as a receiving call, as
 *       today.
 *     - An object goes in one set at a time (as today), an HTTP transfer
 *       too.
 */

#if CRNET_EXT_TCP || CRNET_EXT_WEBSOCKET

/* [CHANGED] (Q6 a, Q10 a) CRNET_WAIT_FOR's two values say more; no value is
 * added:
 *
 *     CRNET_WAIT_FOR_INCOMING = 1:
 *         There is something for recv, accept or receive_response, or the
 *         object has ended.
 *     CRNET_WAIT_FOR_ROOM = 2:
 *         A sending call would make progress now: crnet_tcp_socket_send()
 *         or crnet_http_transfer_send() would take bytes (a start under way
 *         counts: its next call is send), or crnet_websocket_send() would
 *         take a message (none in flight, or the one in flight can be
 *         carried on). For the stretch a caller has something it couldn't
 *         send: a TCP socket or transfer that isn't backed up, and a
 *         WebSocket with nothing in flight, have room all the time, and a set
 *         asked about room on one returns at once every time.
 */

#if CRNET_EXT_TCP

/**
 * [CHANGED] (T10) Adds a TCP socket to the set, connected or with its
 * connect under way. Unchanged but for:
 *
 * A socket whose connect is under way is named once the connect has an
 * outcome, whatever `wait_for` says (call crnet_tcp_socket_connect_continue()
 * then); after that, as `wait_for` says. While its connect is under
 * way a wait on the set counts as a call on the whole socket (C7.1).
 * Known problem: on Windows before
 * version 2004 a connect that failed is never named (WSAPoll), only one
 * that succeeded.
 *
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET [CHANGED] `socket` is NULL,
 *         connect was never called on it, or its connect failed.
 */
CR_RESULT crnet_wait_set_add_tcp_socket(
    crnet_wait_set set,
    crnet_tcp_socket socket,
    uint32_t wait_for,
    void *user);

#endif // CRNET_EXT_TCP

#if CRNET_EXT_WEBSOCKET

/**
 * [CHANGED] (Q10 a, T10) Adds a WebSocket to the set, connected or with its
 * opening under way. Known problem: a libcurl WebSocket is polled by its
 * descriptor, which doesn't show bytes libcurl or the TLS layer has already
 * read and says nothing when this side closes; it is named once after it
 * is added, and not for its own close.
 *
 * Why it changes: a caller whose message is in flight needs to sleep until
 * it can carry it on (P4), and a caller whose connect is under way until
 * the connect has an outcome; and `wait_for` is how
 * crnet_wait_set_add_tcp_socket() already asks.
 *
 * It is reported CRNET_WAIT_FOR_INCOMING when crnet_websocket_recv() has
 * message data or the connection has ended, and CRNET_WAIT_FOR_ROOM when
 * crnet_websocket_send() would take a message. A socket whose opening is
 * under way is named once the opening has an outcome, whatever `wait_for`
 * says (call crnet_websocket_connect_continue() then); until then a wait
 * on the set counts as a call on the whole socket (C7.1). NSURLSession and
 * WinHTTP only say a message has arrived to someone who has asked for the
 * next one, so adding a connected socket for INCOMING asks, and the answer
 * then waits in Carrottus Net for crnet_websocket_recv() as it would after
 * a recv that timed out.
 *
 * To change `wait_for`, remove the socket and add it again.
 *
 * @param[in] set      The set to add to.
 * @param[in] socket   The WebSocket, connected or connecting. It is removed
 *                     from the set before it is destroyed.
 * @param[in] wait_for [ADDED] One or both CRNET_WAIT_FOR bits.
 * @param[in] user     The caller's own, handed back in the events. Never
 *                     dereferenced.
 *
 * @retval CR_SUCCESS The socket is in the set.
 * @retval CR_RESULT_NET_INVALID_WAIT_SET `set` is NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS [ADDED] `wait_for` is 0 or has
 *         a bit that isn't a CRNET_WAIT_FOR one.
 * @retval CR_RESULT_NET_INVALID_WEBSOCKET `socket` is NULL, connect was
 *         never called on it, or its connect failed.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `socket` was made with another
 *         instance than the set's.
 * @retval CR_RESULT_NET_ALREADY_IN_WAIT_SET `socket` is already in a
 *         set, this one or another.
 * @retval CR_RESULT_OUT_OF_MEMORY The set could not grow.
 */
CR_RESULT CRNET_DRAFT_CHANGED(crnet_wait_set_add_websocket)(
    crnet_wait_set set,
    crnet_websocket socket,
    uint32_t wait_for,
    void *user);

#endif // CRNET_EXT_WEBSOCKET

/**
 * [ADDED] (Q6 a) Adds an HTTP transfer to the set, from its start on.
 * Known problem: on libcurl a transfer may be named for libcurl's own timer
 * with nothing to hand over, and its descriptors are asked for afresh at
 * every wait.
 *
 * Why it earns its place: a caller that advances its transfers without
 * waiting has to sleep somewhere when none of them has anything for it,
 * and the wait set is where every other object of this header is slept on.
 * Without it the caller circles its transfers with CRNET_NO_WAIT on a timer
 * of its own, late or busy.
 *
 * It is reported CRNET_WAIT_FOR_INCOMING when
 * crnet_http_transfer_receive_response() or crnet_http_transfer_recv()
 * would return something other than CR_RESULT_NET_TIMED_OUT (the
 * response or body has come, or the transfer has ended), and
 * CRNET_WAIT_FOR_ROOM when crnet_http_transfer_send() would take bytes.
 * A transfer that isn't started yet is refused: there is nothing it could
 * be named for.
 *
 * How each stack wakes it: WinHTTP and NSURLSession through the set's
 * notify, from their callbacks (as their WebSockets do); libcurl through
 * the client's multi handle's descriptors (curl_multi_waitfds, from libcurl
 * 8.8; curl_multi_fdset before), the set's wait shortened to
 * curl_multi_timeout, and a piece of body libcurl has already handed over
 * counting as INCOMING without a poll. (C4.3) curl_multi_waitfds' count
 * may grow between asking it and filling the array (CURLM_OUT_OF_MEMORY):
 * the set asks again with its array grown, through its allocator; and its
 * documentation pairs it with no timer, so the set asks
 * curl_multi_timeout itself.
 *
 * Thread: while the transfer is in the set, crnet_wait_set_wait() counts
 * as a call on it: no call on the transfer may overlap a wait on the set.
 *
 * @param[in] set      The set to add to.
 * @param[in] transfer The transfer, started: a start that returned
 *                     CR_RESULT_NET_TIMED_OUT has started it (C3.4). It
 *                     is removed from the set
 *                     before it is destroyed.
 * @param[in] wait_for One or both CRNET_WAIT_FOR bits.
 * @param[in] user     The caller's own, handed back in the events. Never
 *                     dereferenced.
 *
 * Checks in the order crnet_wait_set_add_tcp_socket() makes them (set,
 * wait_for, the member, its instance, already in a set), one `if` for each
 * result (C5.7).
 *
 * @retval CR_SUCCESS The transfer is in the set.
 * @retval CR_RESULT_NET_INVALID_WAIT_SET `set` is NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `wait_for` is 0 or has a bit
 *         that isn't a CRNET_WAIT_FOR one.
 * @retval CR_RESULT_NET_INVALID_HTTP_TRANSFER `transfer` is NULL, or
 *         hasn't been started, or a call on it has failed (only destroy
 *         is left), as a WebSocket whose connect failed is refused.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `transfer` was made on a client
 *         of another instance than the set's.
 * @retval CR_RESULT_NET_ALREADY_IN_WAIT_SET `transfer` is already in a
 *         set, this one or another.
 * @retval CR_RESULT_OUT_OF_MEMORY The set could not grow.
 */
CR_RESULT crnet_wait_set_add_http_transfer(
    crnet_wait_set set,
    crnet_http_transfer transfer,
    uint32_t wait_for,
    void *user);

/**
 * [ADDED] (Q6 a) Removes an HTTP transfer from the set.
 *
 * Why it earns its place: every kind of member has its remove, and a
 * transfer leaves its set before it is destroyed.
 *
 * After this returns, the transfer's stack makes no further notify on the
 * set.
 *
 * @param[in] set      The set to remove from.
 * @param[in] transfer The transfer to remove. One that isn't in the set, or
 *                     NULL, is taken and nothing is done with it.
 */
void crnet_wait_set_remove_http_transfer(
    crnet_wait_set set,
    crnet_http_transfer transfer);

#endif // CRNET_EXT_TCP || CRNET_EXT_WEBSOCKET

/* ======================================================================== */
/* crnet.h, HTTP and WebSocket clients' creation (Q9 a) [CHANGED]           */
/* ======================================================================== */

/* crnet_http_client_create() and crnet_websocket_create() each gain one
 * result, Linux only:
 *
 *     @retval CR_RESULT_NET_UNSUPPORTED libcurl: the libcurl found at run
 *             time looks host names up synchronously (no
 *             CURL_VERSION_ASYNCHDNS), so a call with CRNET_NO_WAIT could
 *             block in the lookup. Nothing is recorded.
 *
 * Known problem, added to the Instance group beside the THREADSAFE one: a
 * Linux machine whose libcurl was built without the threaded resolver or
 * c-ares has TCP sockets and the resolver, but no HTTP or WebSocket. No
 * signature changes, so nothing is declared here.
 */

/* ======================================================================== */
/* crcore.h                                                                 */
/* ======================================================================== */

/* [CHANGED] CARROTTUS_VERSION_MINOR 2 -> 3 (the breaks under "ABI").
 *
 * [CHANGED] CR_RESULT_NET_TIMED_OUT's one-line comment:
 *     "The call's own time limit ran out, and nothing else; the object is
 *      as usable as it was."
 *
 * No result code, struct type or other value is added.
 */

/* ======================================================================== */
/* Nothing [REMOVED].                                                       */
/* ======================================================================== */

#if __cplusplus
}
#endif

#endif // CRNET_RESUMABLE_DRAFT_H
