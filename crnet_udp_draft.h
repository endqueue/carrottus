/*
 * crnet_udp_draft.h - Carrottus Net: UDP sockets. Datagrams sent to and
 * received from an address, without a thread, in Carrottus Net's wait set
 * as TCP sockets are (CRNET_TODO.md, "Missing pieces", "TCP: ... UDP").
 *
 * APPROVED by the user 2026-11-22, version 7 (SHA-256 in CRNET_TODO.md's
 * approval record). Not built,
 * not in any target.
 *   Version 1: the first.
 *   Version 2 (critique cycle 1, each library's own rules): no caller
 *   named in the text; crnet_udp_socket_recv_from()'s arguments in TCP's
 *   order [CHANGED]; a NULL `data` with a size is CR_RESULT_ZERO_SIZE, as
 *   TCP's send [CHANGED]; a socket is refused only when already in the
 *   same set, as TCP's [CHANGED]; the create's platform error also to the
 *   message callback once card S10 is built.
 *   Version 3 (critique cycle 2, platform truth): EXCLUSIVE narrowed to
 *   what every platform keeps, the same address and port, with the
 *   platforms' differences across addresses stated [CHANGED]; Apple's
 *   largest datagram is the socket's send buffer [CHANGED]; Android 17's
 *   local network permission, and a socket the system has reclaimed,
 *   stated; sources cited, Q1 to Q3 and Q5 answered.
 *   Version 4 (critique cycle 5, the callers): Q4, Q6 and Q7 answered.
 *   Version 5 (critique cycle 6, failures): CR_RESULT_NET_ACCESS_DENIED for
 *   the system refusing the network (EPERM, EACCES) [ADDED]; a reclaimed
 *   socket's results named; outputs on failure stated; tests listed.
 *   Version 6 (critique cycle 7, performance): the no-allocation round at
 *   sixteen datagrams a frame, each call's cost recorded.
 *   Version 7 (the critique's read of the whole): text later versions
 *   had left behind brought into line, every open question answered
 *   under itself, the specs agreeing (prompts/remote_drafts_critique_record.md,
 *   "Whole").
 * Written against include/crnet.h and include/crcore.h
 * as read on 2026-11-21 (carrottus version 0.4.0), against the TCP
 * backends it sits beside (src/net/backends/posix/crnetsockets.c for
 * Apple, Linux and later Android; src/net/backends/windows/crnetwindows.c)
 * and the wait set (src/net/crnetwaitset.c, src/net/crnetshared.h, "Wait
 * set"). Uses crnet_address from crnet_interface_draft.h, included below.
 *
 * Every declaration below is marked [ADDED], [CHANGED] or [REMOVED];
 * nothing unmarked changes. It compiles beside today's crnet.h, as
 * crnet_interface_draft.h does (an [ADDED] result or struct type is a
 * macro here only so the file compiles). Checked with
 *   clang -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -I include -x c
 *   clang -fsyntax-only -std=c++17 -Wall -Wextra -Wpedantic -I include -x c++
 * with -DCARROTTUS_PLATFORM_APPLE=1 and bare.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * A caller of card X46 sends one device's input to another as streams:
 * the latest state, 60 to 120 times a second, each packet repeating the
 * last few, never relayed, never retried (card X47's "streams"). Over TCP a lost packet holds up
 * every later one until it is sent again, which is the wrong trade for a
 * stick's position: the next packet already supersedes it. That is UDP,
 * and every platform has it in the same socket API the TCP sockets
 * already translate. The caller seals each datagram itself (card X48); this
 * moves bytes.
 *
 * Which backend: the one TCP uses on each platform. Apple's TCP is BSD
 * sockets (crnetsockets.c, shared with Linux), not Network.framework, so
 * UDP is too: one POSIX file for Apple, Linux and Android, Winsock2 on
 * Windows. Network.framework's NWConnection would bring its own dispatch
 * queue and callbacks, which the BSD socket and its descriptor in poll()
 * don't need.
 *
 * ---------------------------------------------------------------------------
 * The rules every platform is held to
 * ---------------------------------------------------------------------------
 *
 * Where the systems differ underneath, the header states one rule and
 * every backend keeps it:
 *
 * - One datagram per call. A send takes the whole datagram or nothing
 *   (UDP never sends part of one); a receive hands over one datagram, its
 *   source and its size.
 * - A datagram longer than the caller's buffer is reported, never
 *   silently cut: the receive returns CR_RESULT_NET_DATAGRAM_TRUNCATED
 *   with the first `capacity` bytes and the source, and the rest is gone,
 *   as the systems discard it (POSIX: recvmsg's MSG_TRUNC in msg_flags;
 *   Windows: WSAEMSGSIZE with the buffer filled). How long it was is not
 *   reported: only Linux can say (MSG_TRUNC as a recv flag), so no
 *   platform does.
 * - An unconnected UDP socket never reports another machine's ICMP
 *   errors. Linux and Apple don't on an unconnected socket (Linux's
 *   udp_err() drops a hard error on a socket not connected unless
 *   IP_RECVERR is set, net/ipv4/udp.c, read 2026-11-21, whatever udp(7)
 *   says; macOS tested 2026-11-21: a send to a closed loopback port, then
 *   a receive, is EAGAIN and the next send succeeds); Windows does,
 *   failing the next recvfrom with WSAECONNRESET after a send to a closed
 *   port, which would make one peer that went away break the receive for
 *   all. The Windows backend switches it off (SIO_UDP_CONNRESET and
 *   SIO_UDP_NETRESET set FALSE). A send to nobody is then silent on every
 *   platform, which is what UDP is.
 * - An IPv6 socket is IPv6 only (IPV6_V6ONLY set on every platform:
 *   Windows' default, not Linux's or Apple's: macOS's default is 0,
 *   tested 2026-11-21), as TCP listeners already are. Both
 *   families is two sockets.
 * - Port sharing is the caller's to say, from two that mean the same on
 *   every platform (CRNET_UDP_PORT_SHARING); nothing is set otherwise.
 * - Buffer sizes are the caller's to say, or to leave to the system by
 *   saying so; what the system granted is read back, since Linux doubles
 *   what it is asked and caps it (net.core.rmem_max).
 * - CRNET_NO_WAIT as everywhere else: a receive with nothing waiting, or
 *   a send with no room in the socket's buffer, returns
 *   CR_RESULT_NET_TIMED_OUT and the socket is as usable as it was.
 *
 * ---------------------------------------------------------------------------
 * Not here, and why
 * ---------------------------------------------------------------------------
 *
 * - Multicast and broadcast. Discovery is the system's service
 *   (crnet_discovery_draft.h), not packets of the caller's to 224.0.0.251,
 *   and nothing else needs either; iOS also needs an entitlement for
 *   multicast (com.apple.developer.networking.multicast). Each is a flag
 *   and a call when a caller asks (Q4).
 * - Connected UDP (connect() to one peer, send without an address). It
 *   saves an address per call and makes ICMP errors reportable, which the
 *   rule above switches off (Q5).
 * - Many datagrams per call (Linux's recvmmsg/sendmmsg; nothing on Apple
 *   or Windows but WSARecvMsg one at a time). At 120 a second per peer one
 *   call each is cheap (Q6).
 * - Which local address a datagram came in on (IP_PKTINFO,
 *   IPV6_RECVPKTINFO), for a socket bound to every address on a machine
 *   with several (Q7).
 * - DSCP / traffic class, the don't-fragment bit, GSO/GRO: each a
 *   per-platform translation of its own, when asked.
 *
 * ---------------------------------------------------------------------------
 * Platform notes (stated in the header as the caller's to know)
 * ---------------------------------------------------------------------------
 *
 * - iOS and iPadOS 14 and later, and macOS 15 and later: Local Network
 *   privacy. The first datagram to an address on the local network
 *   brings up the system's prompt, and the app needs
 *   NSLocalNetworkUsageDescription in its Info.plist; the caller's. While
 *   the person hasn't allowed it, a send to a local address fails
 *   (believed to be EHOSTUNREACH, reported as CR_RESULT_NET_SEND_FAILED
 *   with errno in the record). Known problem: Carrottus Net can't tell that
 *   from a real "no route", and the system offers no call to ask.
 * - [CHANGED] Apple's largest outgoing datagram is the socket's send
 *   buffer (SO_SNDBUF), whose default is net.inet.udp.maxdgram, 9216
 *   bytes (xnu's udp_sendspace, "really max datagram size", bsd/netinet/
 *   udp_usrreq.c, read 2026-11-21); a larger one is EMSGSIZE, reported as
 *   CR_RESULT_NET_DATAGRAM_TOO_BIG. Tested on macOS 2026-11-21: with the
 *   system's buffer 9216 goes and 9217 is EMSGSIZE; with send_buffer_size
 *   65535, 65507 goes and 65508 is EMSGSIZE. iOS shares xnu; its default
 *   is not separately confirmed [unconfirmed (C2.4)]. Linux and Windows take up to
 *   65507 (IPv4) / 65527 (IPv6). A caller that stays under its path's MTU
 *   (crnet_interface_properties::mtu, less the headers) meets neither.
 * - Android: the caller's manifest needs android.permission.INTERNET, or
 *   socket() fails (EACCES, reported as [CHANGED] CR_RESULT_NET_ACCESS_DENIED)
 *   [unconfirmed (C2.5)]. [CHANGED] Android 17 (API 37): an app that
 *   targets it needs the runtime permission ACCESS_LOCAL_NETWORK to send
 *   and receive unicast UDP on the local network; without it a send or
 *   receive fails, "typically" EPERM, reported as [CHANGED]
 *   CR_RESULT_NET_ACCESS_DENIED, errno in the record. Asking for the permission is
 *   the caller's (developer.android.com/privacy-and-security/
 *   local-network-permission, read 2026-11-21) [unconfirmed (C2.5): not
 *   tested here ].
 * - [CHANGED] iOS and iPadOS: while an app is suspended, the system may
 *   reclaim its sockets; every later call on such a socket fails (EBADF
 *   or ENOTCONN, reported as CR_RESULT_NET_SEND_FAILED or _RECV_FAILED;
 *   [CHANGED] in a wait set, the member is named ready, as a socket with
 *   an error is by poll(), and its next call returns that failure, errno in
 *   the record), and the caller destroys it
 *   and makes another (Apple's TN2277, "Networking and Multitasking",
 *   read 2026-11-21: "the only thing you can do is close it"). The same
 *   holds for a discovery object's connection to mDNSResponder. Which
 *   sockets to close before suspension is the caller's.
 * - Windows: the first bind of a program to a non-loopback address may
 *   bring up Windows Defender Firewall's prompt for inbound traffic; the
 *   caller's (an installer's rule, or the person's answer).
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * The platforms: BSD sockets (socket, bind with SO_REUSEADDR and
 * SO_REUSEPORT, getsockname, sendto, recvmsg and MSG_TRUNC, SO_RCVBUF and
 * SO_SNDBUF and Linux's doubling, IPV6_V6ONLY, EMSGSIZE, Apple's
 * net.inet.udp.maxdgram); Winsock2 (WSASocketW, SO_EXCLUSIVEADDRUSE and
 * SO_REUSEADDR's Windows meaning, WSARecvFrom and WSAEMSGSIZE,
 * SIO_UDP_CONNRESET, SIO_UDP_NETRESET, WSAPoll); Network.framework's
 * NWConnection and NWListener over UDP (one message per receive, the
 * endpoint as a value, isComplete for a whole datagram), looked at and not
 * taken, for the reason above.
 *
 * Industry practice: libuv's uv_udp_t (uv_udp_bind with UV_UDP_REUSEADDR
 * and UV_UDP_IPV6ONLY flags, uv_udp_try_send's all-or-nothing, the
 * UV_UDP_PARTIAL flag on a truncated receive: the same report here as a
 * result), ASIO's ip::udp::socket (receive_from with an endpoint,
 * socket_base::reuse_address, error::message_size on truncation on
 * Windows only), SDL_net 3 (SDLNet_DatagramSocket, SDLNet_SendDatagram,
 * SDLNet_ReceiveDatagram, the address as an object; it reads into a fixed
 * 64 KiB buffer, so never truncates and never asks the caller), Rust's
 * std::net::UdpSocket (recv_from truncates silently on most platforms:
 * the behaviour this draft refuses), and Valve's GameNetworkingSockets
 * (its own reliability over UDP, which is the caller's place, not this).
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * No break. Added: CRNET_EXT_UDP, crnet_udp_socket,
 * CRNET_UDP_PORT_SHARING, CRNET_SOCKET_BUFFER_SIZE_SYSTEM,
 * crnet_udp_socket_desc, crnet_udp_socket_create(),
 * crnet_udp_socket_get_local_address(),
 * crnet_udp_socket_get_buffer_sizes(), crnet_udp_socket_send_to(),
 * crnet_udp_socket_recv_from(), crnet_udp_socket_abort(),
 * crnet_udp_socket_get_error(), crnet_udp_socket_destroy(),
 * crnet_wait_set_add_udp_socket(), crnet_wait_set_remove_udp_socket(); one
 * struct type (CR_STRUCT_TYPE_NET_UDP_SOCKET_DESC 3009) and four results
 * (CR_RESULT_NET_INVALID_UDP_SOCKET 3036, CR_RESULT_NET_BIND_FAILED 3037,
 * CR_RESULT_NET_DATAGRAM_TOO_BIG 3038, CR_RESULT_NET_DATAGRAM_TRUNCATED
 * 3039), each named in cr_result_str(). Text changes, no signature: the
 * overview's table gains a row, "TCP sockets"'s known problems lose "no
 * UDP", the aborting group's list and the wait set's member list gain the
 * UDP socket, and "Error detail" names crnet_udp_socket_create() among
 * the calls with an `out_error`. Ships in the same minor version as
 * crnet_interface_draft.h, whose crnet_address it needs.
 *
 * ---------------------------------------------------------------------------
 * Tests (crnet_test, a new part "udp"; the misuse and no-allocation
 * harnesses)
 * ---------------------------------------------------------------------------
 *
 * In-process over loopback, no test server: two sockets on 127.0.0.1 and
 * two on ::1, each case seen failing first against a stub.
 * - Round trips: datagrams of 0, 1, 1200 and 9000 bytes each way, the
 *   source read back equal (memcmp) to the sender's
 *   crnet_udp_socket_get_local_address(); port 0 gets a port, and a second
 *   socket on it with EXCLUSIVE is CR_RESULT_NET_ADDRESS_IN_USE; with
 *   SHARED on both, both bind.
 * - Truncation: a 1200-byte datagram into a 100-byte buffer returns
 *   CR_RESULT_NET_DATAGRAM_TRUNCATED with the first 100 bytes and its
 *   source, and the next receive gets the next datagram, not the rest.
 * - Too big [CHANGED]: on Apple with the system's send buffer, 9216 bytes
 *   goes and 9217 is CR_RESULT_NET_DATAGRAM_TOO_BIG; with
 *   send_buffer_size 65536, 65507 goes; 70000 is TOO_BIG everywhere.
 * - Port sharing across addresses [CHANGED]: EXCLUSIVE on 0.0.0.0, then
 *   SHARED on 127.0.0.1 of the same port, and the other way, recorded per
 *   platform against the EXCLUSIVE comment (Apple binds both, Windows the
 *   wildcard over the specific, Linux neither).
 * - The ICMP rule: a send to a loopback port nobody holds, then a receive
 *   with CRNET_NO_WAIT is TIMED_OUT, not a failure, on every platform
 *   (fails on Windows without SIO_UDP_CONNRESET: seen failing first).
 * - Families: an IPv4 socket refuses an IPv6 destination
 *   (CR_RESULT_NET_INVALID_ADDRESS) and the other way; an IPv6 socket on
 *   "::" receives nothing sent to 127.0.0.1 (V6ONLY).
 * - Buffer sizes: asked 262144, read back at least that or the system's
 *   cap (Linux doubles); CRNET_SOCKET_BUFFER_SIZE_SYSTEM reads back the
 *   system's default; 0 refused.
 * - Time limits and aborts: a receive with 50 ms on an empty socket
 *   returns TIMED_OUT in 50-100 ms; one with CRNET_WAIT_FOREVER ended by
 *   crnet_udp_socket_abort() from another thread (under -tsan); every call
 *   after an abort ABORTED.
 * - Wait set: a socket added for INCOMING is named once a datagram is
 *   waiting, drained to TIMED_OUT, and not named again until the next;
 *   ROOM named at once on an idle socket; a UDP socket, a TCP socket and a
 *   listener in one set; a send from another thread overlapping a wait
 *   (allowed), under -tsan.
 * - [CHANGED] A send of NULL data with a size is CR_RESULT_ZERO_SIZE, NULL
 *   with 0 sends an empty datagram; a receive into NULL or 0 bytes is
 *   ZERO_SIZE; one socket in two wait sets, each naming it.
 * - [CHANGED] (cycle 6) BIND_FAILED for an address the machine doesn't hold
 *   (192.0.2.1); ALREADY_IN_WAIT_SET for one set twice; INVALID_INSTANCE
 *   across two instances; an injected out-of-memory at the create and at
 *   the wait set's add, nothing left; ACCESS_DENIED on the A53 without the
 *   permissions (Android, once built).
 * - Misuse: every call with zeroes and with a destroyed socket (a factory
 *   in tests/misuse/make_misuse_tests.py, CR_STALE after each null check,
 *   the socket quarantined by its instance); a desc of each wrong field.
 * - cr_noalloc_tests: the idle round sends and receives one datagram a
 *   frame on loopback with CRNET_NO_WAIT, and [CHANGED, cycle 7] sixteen a
 *   frame (eight peers at 120 Hz into a 60 Hz frame); no allocation of
 *   Carrottus's; each call's cost recorded on each platform, Windows'
 *   above all (batching, Q6, only if it passes 2% of a frame).
 * - [CHANGED] Local Network privacy and Android 17's permission can't be
 *   met by crnet_test: macOS exempts command-line tools run from Terminal
 *   or SSH, the iOS simulator has no local network privacy (TN3179, read
 *   2026-11-21), and Android isn't run until card S47; each is a device
 *   test app's case, listed in prompts/host_revamp_test.md when built.
 * - Per platform: macOS (plain, Release, ASan, TSan); the iOS simulator
 *   (loopback is not the local network, so no prompt); Windows 11 on the
 *   ThinkPad (Debug, Release, -asan; the firewall prompt does not come for
 *   loopback); Fedora 44 (GCC and clang, -asan, -tsan); Android on the
 *   A53 once Carrottus builds there (card S47), and one datagram between
 *   the A53 and the Mac over Wi-Fi by hand, and one between an iPhone test
 *   app and the Mac, which brings up iOS's prompt (a person's test, listed
 *   in prompts/host_revamp_test.md when built).
 */

#ifndef CRNET_UDP_DRAFT_H
#define CRNET_UDP_DRAFT_H

#include "crnet.h"
#include "crnet_interface_draft.h"

#if __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* crcore.h, CR_RESULT and CR_STRUCT_TYPE [ADDED]                           */
/* ======================================================================== */

/* In crcore.h these are enum values; macros here only so the draft
 * compiles. */

/** [ADDED] A UDP socket argument is NULL. */
#define CR_RESULT_NET_INVALID_UDP_SOCKET ((CR_RESULT)3036)
/** [ADDED] A UDP socket could not be opened or bound, for a reason other
 * than the address being taken (CR_RESULT_NET_ADDRESS_IN_USE). Why
 * not CR_RESULT_NET_LISTEN_FAILED: nothing listens, and a caller that
 * reads "listen" for a datagram socket is misled. */
#define CR_RESULT_NET_BIND_FAILED ((CR_RESULT)3037)
/** [ADDED] A datagram is longer than the system sends in one (EMSGSIZE,
 * WSAEMSGSIZE on a send). Nothing was sent. */
#define CR_RESULT_NET_DATAGRAM_TOO_BIG ((CR_RESULT)3038)
/** [ADDED] A datagram was longer than the buffer it was received into:
 * its first bytes and its source were handed over, the rest discarded. */
#define CR_RESULT_NET_DATAGRAM_TRUNCATED ((CR_RESULT)3039)
/** [ADDED] The system refused this program the network for this call: on
 * Android, INTERNET missing (EACCES at the create) or Android 17's
 * ACCESS_LOCAL_NETWORK not granted (EPERM at a send or receive); a caller
 * branches on it to ask for the permission, which a generic failure
 * wouldn't let it tell from a broken route. The socket stays usable once
 * allowed. Apple's Local Network refusal can't be told from no route (a
 * known problem), so it is never this there. The next free value when
 * built (3047 here, after discovery's 3040 to 3046). */
#define CR_RESULT_NET_ACCESS_DENIED ((CR_RESULT)3047)

/** [ADDED] crnet_udp_socket_desc (CR_STRUCT_BASE). */
#define CR_STRUCT_TYPE_NET_UDP_SOCKET_DESC ((CR_STRUCT_TYPE)3009)

/* ======================================================================== */
/* crnet.h, text that changes [CHANGED]                                     */
/* ======================================================================== */

/* [CHANGED] The overview's table gains a row:
 *
 *     | UDP sockets      | Winsock2 | BSD sockets | BSD sockets  |
 *
 * [CHANGED] "Which parts this build has" gains CRNET_EXT_UDP, a column in
 * its table (1 where TCP is 1, 0 on watchOS, whose system refuses BSD
 * sockets to ordinary applications as it does TCP's).
 *
 * [CHANGED] "TCP sockets", its known problems: "no query for a socket's own
 * local address and no UDP" becomes "no query for a socket's own local
 * address".
 *
 * [CHANGED] "Aborting": the objects with an `_abort` gain
 * crnet_udp_socket.
 *
 * [CHANGED] "Wait set": "TCP sockets, TCP listeners, WebSockets and HTTP
 * transfers can be in one" gains UDP sockets; the Thread: bullet on members
 * ("crnet_wait_set_wait() counts as a receiving call on it") covers them
 * as written.
 */

/* ======================================================================== */
/* crnet.h, Which parts this build has [ADDED]                              */
/* ======================================================================== */

/**
 * @def CRNET_EXT_UDP
 * @brief [ADDED] 1 when this build has UDP sockets and a wait set's calls
 * for them; 0 when it has neither. The same platforms as CRNET_EXT_TCP,
 * for the same reason, and its own macro so that a platform that came to
 * allow one and not the other is one line here.
 */
#if CARROTTUS_PLATFORM_WIN32 || CARROTTUS_PLATFORM_LINUX || (CARROTTUS_PLATFORM_APPLE && !CARROTTUS_PLATFORM_WATCHOS)
#define CRNET_EXT_UDP 1
#else
#define CRNET_EXT_UDP 0
#endif

/* crnet_address is declared under CRNET_EXT_TCP (crnet_interface_draft.h);
 * UDP needs it wherever UDP is. */
#if CRNET_EXT_UDP && !CRNET_EXT_TCP
#error "Carrottus Net: CRNET_EXT_UDP needs crnet_address, declared with CRNET_EXT_TCP."
#endif

#if CRNET_EXT_UDP

/* ======================================================================== */
/* crnet.h, UDP sockets [ADDED]                                             */
/* ======================================================================== */

/**
 * @defgroup crnet_udp UDP sockets
 * @ingroup crnet
 * @brief [ADDED] Winsock2 / BSD sockets: datagrams over IPv4 or IPv6, sent
 * to an address and received with their source, no connection. Known
 * problems: on iOS, iPadOS and macOS 15 and later a send to the local
 * network before the person has allowed it fails like a missing route,
 * and can't be told from one; Apple sends no datagram larger than the
 * socket's send buffer, 9216 bytes unless send_buffer_size raises it; on
 * iOS a suspended app's sockets may be reclaimed, after which only their
 * destroy is of use; EXCLUSIVE keeps the same address and port only
 * (CRNET_UDP_PORT_SHARING).
 *
 * A translation of the platform's datagram socket (bind, sendto,
 * recvfrom), not a protocol. Nothing is ordered, acknowledged, sent
 * again, split or joined: a datagram may arrive twice, late, out of order
 * or not at all, and that is the caller's to handle. Nothing is sealed
 * either; anything private is sealed by the caller before it is sent.
 *
 * One object, a crnet_udp_socket, bound to one local address and port
 * when it is made. It sends to any address of its family and receives
 * from any.
 *
 * The rules that hold on every platform, whatever each does underneath:
 *
 * - A send takes the whole datagram or nothing; a receive hands over one
 *   datagram. A datagram longer than the buffer is reported
 *   (CR_RESULT_NET_DATAGRAM_TRUNCATED), never cut silently.
 * - Another machine's ICMP errors are never reported: a send to a port
 *   nobody holds is silent, and no later receive fails for it.
 * - An IPv6 socket takes IPv6 only.
 *
 * Thread: on one socket, a receiving call (crnet_udp_socket_recv_from())
 * may overlap a sending call (crnet_udp_socket_send_to()), and each may
 * overlap crnet_udp_socket_abort(), as on a TCP socket. Two receiving or
 * two sending calls at once, or any other call during either, are the
 * caller's to prevent.
 * @{
 */

/** @brief [ADDED] Opaque handle to one UDP socket. */
typedef struct crnet_udp_socket_t *crnet_udp_socket;

/**
 * @brief [ADDED] Whether another socket may bind the same address and port,
 * in the two forms that mean the same on every platform. No value is a
 * default: NONE is refused.
 */
typedef enum CRNET_UDP_PORT_SHARING {
	/** Not chosen; refused. */
	CRNET_UDP_PORT_SHARING_NONE = 0,
	/** [CHANGED] No other socket, of this program or another, may bind the
	 * same address and port while this one is open, and this one is
	 * refused if one already has: Windows' SO_EXCLUSIVEADDRUSE; elsewhere
	 * no reuse option set.
	 *
	 * Only the same address is kept from others on every platform. A bind
	 * of the same port on another address that overlaps this one (a
	 * specific address over this socket's wildcard, or the wildcard over
	 * this socket's specific address) differs, and is not held here:
	 * - Apple: allowed to a socket that shares (SO_REUSEADDR), of the same
	 *   user, either way; the more specific socket then takes the
	 *   datagrams sent to its address (tested on macOS 2026-11-21:
	 *   EXCLUSIVE 0.0.0.0 then SHARED 127.0.0.1 on one port, all ten to
	 *   127.0.0.1 reached the second; xnu's in_pcbbind, read 2026-11-21);
	 * - Windows: a later wildcard bind over this specific address is
	 *   allowed, "bound to all interfaces except the specific address"
	 *   (learn.microsoft.com, "Using SO_REUSEADDR and SO_EXCLUSIVEADDRUSE",
	 *   read 2026-11-21);
	 * - Linux: refused unless both sockets share.
	 * A caller that needs a port to itself binds it on the wildcard of
	 * each family it uses, and still can't stop another program of the
	 * same user on Apple taking a specific address on it. */
	CRNET_UDP_PORT_SHARING_EXCLUSIVE = 1,
	/** Every socket that binds the address and port with SHARED may hold
	 * it at once: SO_REUSEADDR and SO_REUSEPORT on Apple, Linux and
	 * Android, SO_REUSEADDR on Windows. Which of them a datagram to that
	 * port is handed to differs by platform (Linux spreads them across
	 * sockets of one user; Apple gives each to the last bound; Windows
	 * doesn't say), so it is undefined here: a caller that shares a port
	 * makes no assumption about which socket receives. Linux refuses
	 * SHARED to a second socket of another user. */
	CRNET_UDP_PORT_SHARING_SHARED = 2,
} CRNET_UDP_PORT_SHARING;

/**
 * @brief [ADDED] A `receive_buffer_size` or `send_buffer_size` that leaves
 * the system's own size, unchanged. Its own value, not 0, so that a zeroed
 * desc has chosen nothing.
 */
#define CRNET_SOCKET_BUFFER_SIZE_SYSTEM 0xFFFFFFFFu

/** @brief [ADDED] What crnet_udp_socket_create() binds, and how. */
typedef struct crnet_udp_socket_desc {
	/** `type` is CR_STRUCT_TYPE_NET_UDP_SOCKET_DESC; `next` is NULL
	 * (nothing is chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** The local address and port to bind. Its family is the socket's.
	 * All-zero bytes is every interface of that family (0.0.0.0 or ::),
	 * 127.0.0.1 or ::1 this machine only, or one interface's own address.
	 * Port 0 has the system pick a free one;
	 * crnet_udp_socket_get_local_address() says which. A scope_id binds an
	 * IPv6 link-local address on that interface. */
	crnet_address local;
	/** Whether another socket may hold the same address and port. */
	CRNET_UDP_PORT_SHARING sharing;
	/** SO_RCVBUF in bytes, or CRNET_SOCKET_BUFFER_SIZE_SYSTEM. 0 is refused.
	 * The system may grant more or less: see
	 * crnet_udp_socket_get_buffer_sizes(). Datagrams that arrive while it is
	 * full are dropped by the system, unreported. */
	uint32_t receive_buffer_size;
	/** SO_SNDBUF in bytes, or CRNET_SOCKET_BUFFER_SIZE_SYSTEM. 0 is refused.
	 * [CHANGED] On Apple, also the largest datagram the socket sends: the
	 * granted size (crnet_udp_socket_get_buffer_sizes()), 9216 with the
	 * system's (net.inet.udp.maxdgram). */
	uint32_t send_buffer_size;
} crnet_udp_socket_desc;

/**
 * @brief [ADDED] Opens a non-blocking datagram socket and binds it: socket,
 * IPV6_V6ONLY for IPv6, the sharing options, the buffer sizes, bind, and
 * on Windows SIO_UDP_CONNRESET and SIO_UDP_NETRESET switched off.
 *
 * @param[in]  instance   The instance the socket is made with.
 * @param[in]  desc       What the caller decides. Only read during the call.
 * @param[out] out_socket Receives the socket, the caller's to destroy.
 *                        NULL on failure.
 * @param[out] out_error  Receives the system's reason for a failure, which
 *                        leaves no socket to ask. May be NULL. See
 *                        @ref crnet_error_detail "Error detail". Once card
 *                        S10 is built, the same platform error also goes
 *                        to the instance's message callback, inside this
 *                        call.
 *
 * @retval CR_SUCCESS The socket is bound.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_UDP_SOCKET_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_socket` is NULL.
 * @retval CR_RESULT_NET_INVALID_ADDRESS `desc->local`'s family is neither
 *         IPv4 nor IPv6, its `reserved` isn't 0, or it is IPv4 with a
 *         scope or with bytes past the fourth.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `desc->sharing` is no value of
 *         its enum, or a buffer size is 0.
 * @retval CR_RESULT_OUT_OF_MEMORY The socket could not be allocated.
 * @retval CR_RESULT_NET_ADDRESS_IN_USE The address and port are taken
 *         (EADDRINUSE, WSAEADDRINUSE; WSAEACCES on Windows, which is what
 *         SO_EXCLUSIVEADDRUSE held by another gives).
 * @retval CR_RESULT_NET_BIND_FAILED The system refused to open, set up or
 *         bind the socket for any other reason (an address this machine
 *         doesn't hold, Android without INTERNET); `out_error` has it.
 */
CR_RESULT crnet_udp_socket_create(
    crnet_instance instance,
    const crnet_udp_socket_desc *desc,
    crnet_udp_socket *out_socket,
    crnet_error *out_error);

/**
 * @brief [ADDED] Reads the address and port the socket is bound to
 * (getsockname): the desc's, with the port the system picked for 0.
 *
 * @param[in]  socket      The socket to ask.
 * @param[out] out_address Receives the address.
 *
 * @retval CR_SUCCESS `out_address` was written.
 * @retval CR_RESULT_NET_INVALID_UDP_SOCKET `socket` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_address` is NULL.
 */
CR_RESULT crnet_udp_socket_get_local_address(
    crnet_udp_socket socket,
    crnet_address *out_address);

/**
 * @brief [ADDED] Reads the buffer sizes the system granted (getsockopt
 * SO_RCVBUF and SO_SNDBUF), which may differ from the desc's: Linux
 * reports twice what it was asked, for its own bookkeeping, capped by
 * net.core.rmem_max and wmem_max; the others cap by their own settings.
 *
 * @param[in]  socket                The socket to ask.
 * @param[out] out_receive_buffer_size Receives SO_RCVBUF in bytes.
 * @param[out] out_send_buffer_size    Receives SO_SNDBUF in bytes.
 *
 * @retval CR_SUCCESS Both were written.
 * @retval CR_RESULT_NET_INVALID_UDP_SOCKET `socket` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER Either output is NULL.
 */
CR_RESULT crnet_udp_socket_get_buffer_sizes(
    crnet_udp_socket socket,
    uint32_t *out_receive_buffer_size,
    uint32_t *out_send_buffer_size);

/**
 * @brief [ADDED] Sends one datagram to one address: the whole of it, or
 * nothing.
 *
 * With CRNET_NO_WAIT it is sent if the socket's buffer has room now and
 * CR_RESULT_NET_TIMED_OUT returned if not; with a limit it waits for room
 * that long. Sent means handed to the system: whether it arrives is not
 * known, here or anywhere.
 *
 * @param[in] socket     The socket to send from.
 * @param[in] to         The destination, of the socket's family, port not
 *                       0. A link-local IPv6 one needs its scope_id.
 * @param[in] data       The datagram. May be NULL when `size` is 0 (an
 *                       empty datagram is a datagram).
 * @param[in] size       Its size in bytes.
 * @param[in] timeout_ms The longest to wait for room
 *                       (@ref crnet_time "Time limits").
 *
 * @retval CR_SUCCESS The datagram was handed to the system whole.
 * @retval CR_RESULT_NET_INVALID_UDP_SOCKET `socket` is NULL.
 * @retval CR_RESULT_NET_INVALID_ADDRESS `to` is NULL, not of the socket's
 *         family, port 0, or not a valid crnet_address.
 * @retval CR_RESULT_ZERO_SIZE [CHANGED] `data` is NULL and `size` isn't 0,
 *         TCP's send's code for a missing buffer (an empty datagram, NULL
 *         and 0, is sent).
 * @retval CR_RESULT_NET_DATAGRAM_TOO_BIG Longer than the system sends in
 *         one; nothing was sent.
 * @retval CR_RESULT_NET_TIMED_OUT No room in time; nothing was sent, and
 *         the socket is as it was.
 * @retval CR_RESULT_NET_ABORTED crnet_udp_socket_abort() was called.
 * @retval CR_RESULT_NET_SEND_FAILED The system refused it for another
 *         reason (no route; Local Network privacy on Apple, believed to look
 *         the same [unconfirmed (C2.6): TN3179 names no errno];
 *         Android 17's local network permission, EPERM; a socket iOS has
 *         reclaimed); crnet_udp_socket_get_error() with
 *         CRNET_ERROR_SIDE_SENDING has it. [CHANGED] The socket stays
 *         usable, but for one the system has reclaimed (iOS, above), which
 *         fails every call until destroyed.
 */
CR_RESULT crnet_udp_socket_send_to(
    crnet_udp_socket socket,
    const crnet_address *to,
    const void *data,
    uint32_t size,
    uint32_t timeout_ms);

/**
 * @brief [ADDED] Receives one datagram and where it came from.
 *
 * With CRNET_NO_WAIT it hands over a datagram already waiting, or returns
 * CR_RESULT_NET_TIMED_OUT; with a limit it waits for one that long. After
 * a wait set names the socket, call this with CRNET_NO_WAIT until
 * TIMED_OUT.
 *
 * [CHANGED] The arguments are in crnet_tcp_socket_recv()'s order (the
 * buffer, its size, the limit, then the outputs). Sizes are uint32_t, not
 * TCP's uint64_t: no datagram is larger than 65,535 bytes, and a type that
 * says so is a check the caller can't miss.
 *
 * @param[in]  socket     The socket to receive on.
 * @param[out] buffer     Receives the datagram's bytes.
 * @param[in]  capacity   The room in `buffer`; 0 is refused. A datagram
 *                        longer than this is truncated and reported.
 * @param[in]  timeout_ms The longest to wait (@ref crnet_time "Time limits").
 * @param[out] out_size   Receives how many bytes were written: the
 *                        datagram's size, or `capacity` when truncated; 0
 *                        for an empty datagram and on any failure.
 * @param[out] out_from   Receives the source address and port; an IPv6
 *                        link-local source with its scope_id. [CHANGED]
 *                        Written on success and on TRUNCATED; untouched on
 *                        any other result.
 *
 * @retval CR_SUCCESS One whole datagram was handed over.
 * @retval CR_RESULT_NET_DATAGRAM_TRUNCATED One datagram longer than
 *         `capacity`: its first `capacity` bytes and its source were
 *         handed over and the rest is gone. Not a failure of the socket;
 *         the next call receives the next datagram.
 * @retval CR_RESULT_NET_INVALID_UDP_SOCKET `socket` is NULL.
 * @retval CR_RESULT_ZERO_SIZE [CHANGED] `buffer` is NULL or `capacity` is
 *         0, as TCP's receive.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_size` or `out_from` is
 *         NULL.
 * @retval CR_RESULT_NET_TIMED_OUT Nothing arrived in time; the socket is
 *         as it was.
 * @retval CR_RESULT_NET_ABORTED crnet_udp_socket_abort() was called.
 * @retval CR_RESULT_NET_RECV_FAILED The system's receive failed;
 *         crnet_udp_socket_get_error() with CRNET_ERROR_SIDE_RECEIVING has
 *         it.
 */
CR_RESULT crnet_udp_socket_recv_from(
    crnet_udp_socket socket,
    void *buffer,
    uint32_t capacity,
    uint32_t timeout_ms,
    uint32_t *out_size,
    crnet_address *out_from);

/**
 * @brief [ADDED] Ends a wait in a send or a receive, and every call after
 * it. See @ref crnet_aborting "Aborting".
 *
 * Thread: may overlap any call on the socket but its destroy.
 *
 * @param[in] socket The socket to abort.
 */
void crnet_udp_socket_abort(crnet_udp_socket socket);

/**
 * @brief [ADDED] Reads what the platform reported for the most recent
 * failed call on one side of the socket. See
 * @ref crnet_error_detail "Error detail".
 *
 * @param[in]  socket    The socket to ask. NULL gives an empty record.
 * @param[in]  side      RECEIVING for recv_from, SENDING for send_to.
 * @param[out] out_error Receives the record.
 */
void crnet_udp_socket_get_error(
    crnet_udp_socket socket,
    CRNET_ERROR_SIDE side,
    crnet_error *out_error);

/**
 * @brief [ADDED] Closes the socket and frees it. Datagrams waiting in it
 * are dropped; ones already handed to the system may still go.
 *
 * @param[in] socket The socket to destroy. NULL is taken and nothing is
 *                   done with it.
 */
void crnet_udp_socket_destroy(crnet_udp_socket socket);

/** @} */

/* ======================================================================== */
/* crnet.h, Wait set [ADDED]                                                */
/* ======================================================================== */

/**
 * @brief [ADDED] Adds a UDP socket to the set: its descriptor in the set's
 * poll(), as a TCP socket's is.
 *
 * It is named CRNET_WAIT_FOR_INCOMING when crnet_udp_socket_recv_from()
 * has a datagram to hand over, and CRNET_WAIT_FOR_ROOM when
 * crnet_udp_socket_send_to() would take one now - which, on a socket not
 * backed up, is all the time, so ROOM is asked for only while a datagram
 * couldn't be sent. To change `wait_for`, remove the socket and add it
 * again.
 *
 * Thread: while the socket is in the set, a wait on the set counts as a
 * receiving call on it, as for a TCP socket: a send may overlap the wait.
 *
 * @param[in] set      The set to add to.
 * @param[in] socket   The socket. It is removed from the set before it is
 *                     destroyed.
 * @param[in] wait_for One or both CRNET_WAIT_FOR bits.
 * @param[in] user     The caller's own, handed back in the events. Never
 *                     dereferenced.
 *
 * @retval CR_SUCCESS The socket is in the set.
 * @retval CR_RESULT_NET_INVALID_WAIT_SET `set` is NULL.
 * @retval CR_RESULT_NET_INVALID_UDP_SOCKET `socket` is NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `wait_for` is 0 or has a bit
 *         that isn't a CRNET_WAIT_FOR one.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `socket` was made with another
 *         instance than the set's.
 * @retval CR_RESULT_NET_ALREADY_IN_WAIT_SET [CHANGED] `socket` is already in
 *         this set (as for a TCP socket: a descriptor may be in two sets'
 *         poll() at once).
 * @retval CR_RESULT_OUT_OF_MEMORY The set could not grow.
 */
CR_RESULT crnet_wait_set_add_udp_socket(
    crnet_wait_set set,
    crnet_udp_socket socket,
    uint32_t wait_for,
    void *user);

/**
 * @brief [ADDED] Takes a UDP socket out of the set. One not in it is
 * ignored; so is NULL for either argument.
 *
 * @param[in] set    The set.
 * @param[in] socket The socket.
 */
void crnet_wait_set_remove_udp_socket(crnet_wait_set set, crnet_udp_socket socket);

#endif // CRNET_EXT_UDP

/*
 * ---------------------------------------------------------------------------
 * Open questions
 * ---------------------------------------------------------------------------
 *
 * 1. Truncation as a result (drafted: CR_RESULT_NET_DATAGRAM_TRUNCATED,
 *    with the bytes handed over) or as a flag beside CR_SUCCESS (libuv's
 *    UV_UDP_PARTIAL)? Answered C2.8: the result; every platform reports
 *    it (POSIX MSG_TRUNC in msg_flags, tested on macOS 2026-11-21;
 *    Windows WSAEMSGSIZE with the buffer filled, recvfrom's page, read
 *    2026-11-21). A flag can be ignored
 *    without a trace, which is the silent cut the rule forbids; a result
 *    that isn't CR_SUCCESS can't be. Its one oddity, a result other than
 *    success that still filled the outputs, is stated on the call.
 * 2. Addresses as crnet_address (drafted) or as numeric text, as TCP
 *    takes them? Answered C2.8: crnet_address, for the reasons in
 *    crnet_interface_draft.h (a source compared every receive, the
 *    scope); its Q3 is TCP's side.
 * 3. Port sharing: two values (drafted) or the platforms' options as
 *    flags (SO_REUSEADDR, SO_REUSEPORT, SO_EXCLUSIVEADDRUSE)?
 *    Answered C2.1: two values, EXCLUSIVE narrowed to the same address
 *    and port, the one thing every platform keeps. The options mean different things on
 *    each system (SO_REUSEADDR on Windows is SO_REUSEPORT elsewhere, and
 *    lets another program take the port); flags would hand the caller
 *    three systems' rules. EXCLUSIVE and SHARED are what each can keep.
 * 4. Broadcast (SO_BROADCAST) and multicast (join, leave, TTL, loop)?
 *    Recommendation: not now: discovery is the system's, and nothing else
 *    in the plan sends to a group. A flag on the desc and two calls when a
 *    caller asks, with iOS's multicast entitlement stated as the caller's.
 *    Answered C5.42: no caller needs either.
 * 5. Connected UDP? Answered C2.8: not now. It would bring back the
 *    ICMP errors the rule switches off, per peer, which is useful only
 *    for a caller with one peer; a caller talking to several peers
 *    from one socket gains nothing.
 * 6. Batched sends and receives (recvmmsg/sendmmsg on Linux)?
 *    Recommendation: not now; a call that takes an array, emulated as a
 *    loop where the system has none, if the no-allocation harness or a
 *    profile shows the per-call cost matters.
 *    Answered C5.42: no caller needs it (a handful of datagrams a frame per peer).
 * 7. The local address a datagram arrived on (IP_PKTINFO)? A socket bound
 *    to every interface on a machine with Wi-Fi and Ethernet answers from
 *    whichever the system routes, which may not be the one the peer used.
 *    Recommendation: add it when the multi-homed case is seen in a test
 *    (an out parameter beside `out_from`, every platform has it); bind
 *    per interface meanwhile, which crnet_interface_enumerate() makes
 *    possible.
 *    Answered C5.42: not now: the caller of card X47 matches a datagram to its link by an index inside it, never by the address it came to, and answers the address it came from.
 * 8. CRNET_SOCKET_BUFFER_SIZE_SYSTEM's place: here, or also on TCP's
 *    descs, which set no buffer size today? Recommendation: define it once
 *    for every socket now (named for sockets, not UDP), and give TCP the
 *    two fields when a caller asks.
 *    Answered (whole read): as recommended.
 */

#if __cplusplus
}
#endif

#endif // CRNET_UDP_DRAFT_H
