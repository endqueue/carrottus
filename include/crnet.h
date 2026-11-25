
/**
 * @file crnet.h
 * @brief Carrottus Net: the platform's network APIs behind one C interface. Known
 * problem: every backend has been run only over the loopback interface (on
 * macOS, the iOS simulator, Windows 11 and Fedora 44), and never on an
 * iPhone or iPad.
 *
 * The overview, and everything that holds for the whole module, is under
 * @ref crnet "Carrottus Net".
 */

#ifndef CRNET_H
#define CRNET_H

#include "crcore.h"
#include <stdbool.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/**
 * @defgroup crnet Carrottus Net
 * @brief The platform's network APIs behind one C interface. Known problem:
 * every backend has been run only against a server on the same machine - on
 * macOS, the iOS simulator, Windows 11 and Fedora 44 - and never on an iPhone
 * or iPad, so what this header says of a real network is what they were
 * written to do.
 *
 * Three parts, each a translation of what the platform already has:
 *
 * | Part             | Windows  | Linux       | Apple        |
 * |------------------|----------|-------------|--------------|
 * | TCP sockets      | Winsock2 | BSD sockets | BSD sockets  |
 * | UDP sockets      | Winsock2 | BSD sockets | BSD sockets  |
 * | HTTP client      | WinHTTP  | libcurl     | NSURLSession |
 * | WebSocket client | WinHTTP  | libcurl (*) | NSURLSession |
 * | Service discovery | DNS-SD API (dnsapi) | Avahi | dns_sd |
 *
 * (*) libcurl 8.11 or later. Against an older one, or one built without
 * WebSocket, crnet_websocket_create() returns
 * CR_RESULT_NET_UNSUPPORTED.
 *
 * On Linux all of Carrottus Net, the TCP sockets included, needs libcurl 7.84 or
 * later, built with CURL_VERSION_THREADSAFE: carrottus doesn't build against
 * an older one, and crnet_instance_create() returns
 * CR_RESULT_NET_UNSUPPORTED on one built without it
 * (@ref crnet_instances "Instance" has why).
 *
 * Not every part is on every platform: CRNET_EXT_TCP, CRNET_EXT_UDP,
 * CRNET_EXT_WEBSOCKET and CRNET_EXT_DISCOVERY say which of them this build
 * has
 * (@ref crnet_parts "Which parts this build has"). HTTP is on all of them.
 *
 * The HTTP and WebSocket clients don't sit on the TCP sockets here. TLS,
 * proxies and framing belong to the platform's client stack, and they
 * translate that stack directly.
 *
 * Pure transport, no protocol. Carrottus Net moves bytes, requests and messages
 * and nothing else. Message framing over a socket, what a status code or a
 * payload means, when to retry or reconnect, who has authority - all of
 * that is the caller's, the same way Carrottus GPU never decides what to draw.
 *
 * The same shape throughout:
 *
 * - Names are `crnet_<object>_<verb>`: crnet_tcp_socket_send(),
 *   crnet_http_client_create(), crnet_websocket_recv().
 * - Every object is an opaque handle made by its `_create` (a connected
 *   socket also by crnet_tcp_listener_accept()) and released by its
 *   `_destroy`, which takes NULL and does nothing with it. The caller owns
 *   it in between.
 * - Everything starts from a crnet_instance
 *   (@ref crnet_instances "Instance"). The `_create` calls that make an
 *   object from nothing take one; an object made from another object (an
 *   accepted socket, an HTTP transfer) is its parent's instance's. There is
 *   nothing to call once per process.
 * - Anything with more than a couple of inputs takes a desc struct.
 *   Nothing in one has a default: Carrottus Net never picks an address, a port, a
 *   backlog, a time limit or a size limit for the caller.
 * - Strings and buffers passed in are only read during the call.
 * - Every call returns at once or blocks the thread it was made on, for no
 *   longer than its time limit; Carrottus Net starts no thread
 *   (@ref crnet_threads "Threads"). A call that can wait takes a time
 *   limit (@ref crnet_time "Time limits"), and with CRNET_NO_WAIT never
 *   waits: what has to wait for the other side is begun and left to go
 *   on, and a later call carries it on. Every wait can be ended by the
 *   object's `_abort` (@ref crnet_aborting "Aborting").
 * - Failures are CR_RESULT values: the general ones, and the
 *   `CR_RESULT_CRNET_*` group in crcore.h. What the platform itself
 *   reported behind one is kept and can be read back
 *   (@ref crnet_error_detail "Error detail").
 */

/**
 * @defgroup crnet_parts Which parts this build has
 * @ingroup crnet
 * @brief Compile-time macros that say whether this build has TCP sockets, UDP
 * sockets, the WebSocket client and service discovery.
 *
 * carrottus is built for one platform at a time, and the build says which by
 * defining one of CARROTTUS_PLATFORM_WIN32, CARROTTUS_PLATFORM_LINUX and
 * CARROTTUS_PLATFORM_APPLE as 1. The last covers every Apple system, and
 * Carrottus Net has to tell one of them apart, so a build for Apple Watch also
 * defines CARROTTUS_PLATFORM_WATCHOS as 1. Anything not defined is 0.
 *
 * Each of CRNET_EXT_TCP, CRNET_EXT_UDP, CRNET_EXT_WEBSOCKET and
 * CRNET_EXT_DISCOVERY is 1 where the platform
 * lets an ordinary application use that part and 0 where it doesn't, and
 * everything belonging to a part is declared only when its macro is 1 - so
 * code that uses a part the platform doesn't have fails to compile there,
 * rather than building and then failing on the device. Code that is shared
 * between platforms tests the macro, and never a platform's name: if a
 * second platform comes to lack a part, that is one line changed here and
 * nothing to hunt for anywhere else.
 *
 * | Platform                 | TCP | UDP | WebSocket | Discovery |
 * |--------------------------|-----|-----|-----------|-----------|
 * | Windows                  | 1   | 1   | 1         | 1         |
 * | Linux                    | 1   | 1   | 1 (*)     | 1         |
 * | Android                  | 1   | 1   | 1         | 0         |
 * | Apple other than watchOS | 1   | 1   | 1         | 1         |
 * | watchOS                  | 0   | 0   | 0         | 0         |
 *
 * (*) The macro says the platform has it. Whether the libcurl carrottus was
 * built against does is found out when crnet_websocket_create() is called
 * (CR_RESULT_NET_UNSUPPORTED).
 *
 * watchOS: everything below HTTP is closed to most applications there.
 * BSD sockets and NSURLSession's WebSocket task both exist in its SDK, so
 * code that uses them compiles, and both are refused when the application
 * runs; the system only lets them through for a few kinds of application
 * (one streaming audio, chiefly). What an ordinary application has is
 * HTTP, which is what is left here. 0 is therefore a statement about what
 * the system allows rather than about what Carrottus Net has written, and it
 * shuts out the excepted kinds of application along with the rest.
 * @{
 */

#ifndef CARROTTUS_PLATFORM_WIN32
/** @brief 1 in a build for Windows, 0 otherwise. Defined by the build. */
#define CARROTTUS_PLATFORM_WIN32 0
#endif
#ifndef CARROTTUS_PLATFORM_LINUX
/** @brief 1 in a build for Linux, 0 otherwise. Defined by the build. */
#define CARROTTUS_PLATFORM_LINUX 0
#endif
#ifndef CARROTTUS_PLATFORM_APPLE
/** @brief 1 in a build for any Apple system, 0 otherwise. Defined by the build. */
#define CARROTTUS_PLATFORM_APPLE 0
#endif
#ifndef CARROTTUS_PLATFORM_WATCHOS
/**
 * @brief 1 in a build for watchOS, beside CARROTTUS_PLATFORM_APPLE, and 0
 * otherwise. Defined by the build.
 */
#define CARROTTUS_PLATFORM_WATCHOS 0
#endif

/* CARROTTUS_PLATFORM_WATCHOS comes from the build, and a watchOS build that
 * forgot it would get both parts and find out on the device. The system's
 * own macro settles it at compile time instead. */
#if CARROTTUS_PLATFORM_APPLE
#include <TargetConditionals.h>
#if TARGET_OS_WATCH && !CARROTTUS_PLATFORM_WATCHOS
#error "Carrottus Net: this is a watchOS build, and CARROTTUS_PLATFORM_WATCHOS must be defined as 1 for it."
#endif
#endif

/**
 * @def CRNET_EXT_TCP
 * @brief 1 when this build has TCP sockets and listeners, the resolver that
 * finds addresses for them, the interface listing
 * (crnet_interface_enumerate()) and crnet_address, and a wait set's calls
 * for both; 0 when it has none of them.
 */
#if CARROTTUS_PLATFORM_WIN32 || CARROTTUS_PLATFORM_LINUX || (CARROTTUS_PLATFORM_APPLE && !CARROTTUS_PLATFORM_WATCHOS)
#define CRNET_EXT_TCP 1
#else
#define CRNET_EXT_TCP 0
#endif

/**
 * @def CRNET_EXT_WEBSOCKET
 * @brief 1 when this build has the WebSocket client and a wait set's calls
 * for it; 0 when it has neither. Known problem: it goes by platform, so it
 * is 1 on Linux even when the libcurl carrottus was built against has no
 * WebSocket (older than 8.11), and that is only found out at run time, from
 * crnet_websocket_create().
 */
#if CARROTTUS_PLATFORM_WIN32 || CARROTTUS_PLATFORM_LINUX || (CARROTTUS_PLATFORM_APPLE && !CARROTTUS_PLATFORM_WATCHOS)
#define CRNET_EXT_WEBSOCKET 1
#else
#define CRNET_EXT_WEBSOCKET 0
#endif

/**
 * @def CRNET_EXT_UDP
 * @brief 1 when this build has UDP sockets and a wait set's calls for them;
 * 0 when it has neither. The same platforms as CRNET_EXT_TCP, for the same
 * reason, and its own macro so that a platform that came to allow one and
 * not the other is one line here.
 */
#if CARROTTUS_PLATFORM_WIN32 || CARROTTUS_PLATFORM_LINUX || (CARROTTUS_PLATFORM_APPLE && !CARROTTUS_PLATFORM_WATCHOS)
#define CRNET_EXT_UDP 1
#else
#define CRNET_EXT_UDP 0
#endif

/* crnet_address is declared under CRNET_EXT_TCP; UDP needs it wherever UDP
 * is. */
#if CRNET_EXT_UDP && !CRNET_EXT_TCP
#error "Carrottus Net: CRNET_EXT_UDP needs crnet_address, declared with CRNET_EXT_TCP."
#endif

/**
 * @def CRNET_EXT_DISCOVERY
 * @brief 1 when this build has service discovery (adverts, browsers,
 * resolvers) and a wait set's calls for them; 0 when it has none. 0 on
 * watchOS, which refuses it to ordinary applications, and on Android, whose
 * discovery service is Java only. An Android build is told by the system's
 * own macro, __ANDROID__ (which the NDK's compiler always defines), as a
 * watchOS build is by TargetConditionals.h, because no CARROTTUS_PLATFORM_
 * macro names Android yet; once one does, the test reads it, with
 * __ANDROID__ kept as the same guard TargetConditionals.h is for watchOS.
 */
#if !defined(__ANDROID__) && (CARROTTUS_PLATFORM_WIN32 || CARROTTUS_PLATFORM_LINUX || (CARROTTUS_PLATFORM_APPLE && !CARROTTUS_PLATFORM_WATCHOS))
#define CRNET_EXT_DISCOVERY 1
#else
#define CRNET_EXT_DISCOVERY 0
#endif

/* crnet_address is declared under CRNET_EXT_TCP. */
#if CRNET_EXT_DISCOVERY && !CRNET_EXT_TCP
#error "Carrottus Net: CRNET_EXT_DISCOVERY needs crnet_address, declared with CRNET_EXT_TCP."
#endif

/** @} */

/**
 * @defgroup crnet_threads Threads
 * @ingroup crnet
 * @brief Every thread is the caller's: Carrottus Net starts none and calls nothing
 * back.
 *
 * The library rule holds (@ref cr_threads "Threads", crcore.h): every
 * object is used by one caller at a time, from any thread and a different
 * one each time. What is particular to Carrottus Net:
 *
 * - Carrottus Net starts no thread. A call runs on the thread that made it, for
 *   as long as it takes or its time limit allows. Where work has to go on
 *   while the caller does something else, the caller brings the thread:
 *   how many there are, what they cost and when they end is not Carrottus Net's
 *   to decide. (The platform's HTTP stack has threads and queues of its
 *   own. They are the stack's, and nothing of the caller's, its allocator
 *   included, is called on them. So are Windows' DNS-SD calls, which call
 *   back on threads of their own: each callback copies into its object's
 *   ring of events under the ring's lock - held only for a fixed-size copy,
 *   at most a resolve event's 1.6 KB, nothing allocated under it - and
 *   wakes the object's wait set under the same lock, so that a set the
 *   object has been taken out of is never woken by it, and calls nothing
 *   of the caller's; the caller's `_next` takes the same lock for its copy
 *   out.) A step begun and left
 *   (@ref crnet_time "Time limits") goes on where the stack goes on by
 *   itself - WinHTTP's and NSURLSession's threads, the system's TCP
 *   connect - and otherwise in the caller's next call on the object:
 *   libcurl moves only inside a call. Either way nothing of the caller's
 *   runs anywhere but its own calls.
 * - Nothing here calls back. No function pointer is taken anywhere in this
 *   header, so no code of the caller's runs anywhere but where the caller
 *   called from.
 * - Waiting is what Carrottus Net's objects are for, so its exceptions are about
 *   waits, each marked `Thread:` on the call: every `_abort`, and
 *   crnet_wait_set_wake(), may overlap any call on their object but its
 *   destroy (@ref crnet_aborting "Aborting", @ref crnet_wait
 *   "Wait set"); a TCP socket and a WebSocket take one receiving call and
 *   one sending call at once; and a resolver's crnet_resolver_run()
 *   overlaps everything else on it, its destroy included
 *   (@ref crnet_resolvers "Resolver").
 * - Taking an object apart is always the same three steps: abort it, wait
 *   for the caller's own calls on it to return, destroy it. The one
 *   exception is the resolver, whose lookup can't be brought back and whose
 *   destroy is made not to need it.
 */

/**
 * @defgroup crnet_error_detail Error detail
 * @ingroup crnet
 * @brief What the platform reported behind a failed call. Known problems:
 * not every failure from a stack leaves a record (libcurl being unable to
 * apply a TLS option, and a WinHTTP close frame that couldn't be sent, leave
 * none); WinHTTP's own certificate complaints all come back as the one code
 * 12175 (ERROR_WINHTTP_SECURE_FAILURE), without which check failed; and the
 * `_get_error` calls take no lock, relying on the calling rule below.
 *
 * A CR_RESULT is the same on every platform, which is its use and its
 * limit: a network failure has its cause on another machine, may not happen
 * twice, and the one number that says what it was is the platform's own.
 * Each object keeps that number for its most recent failure, and its
 * `_get_error` hands it over. It is for a log or a report; what to do next is
 * decided from the CR_RESULT, which is why nothing here is portable.
 *
 * What is kept, and when:
 *
 * - Only a failure that came from the platform is recorded. A refused
 *   argument, CR_RESULT_NET_TIMED_OUT and CR_RESULT_NET_ABORTED
 *   are Carrottus Net's own doing and leave what was there alone - so does a
 *   call that succeeds. A loop polling with CRNET_NO_WAIT doesn't wear
 *   the record of a real failure away.
 * - `result` is the CR_RESULT the recorded call returned. A caller that
 *   just got a failure compares the two to know the detail is that call's
 *   and not an older one's.
 * - A TCP socket and a WebSocket keep two records, because a send and a
 *   receive may overlap on one: CRNET_ERROR_SIDE says which.
 * - `_get_error` is called under the same rule as the calls it reports on:
 *   it is a call on that side of the object, after the failed call has
 *   returned and not overlapping another call on that side. It never
 *   waits, and it takes NULL (the record comes back empty).
 *
 * A call that fails without leaving an object to ask -
 * crnet_instance_create(), crnet_interface_enumerate(),
 * crnet_tcp_listener_create(), crnet_udp_socket_create() - takes an
 * `out_error` instead, which may be
 * NULL. The other `_create` calls fail only for want of memory or over a bad
 * desc, and take none.
 * @{
 */

/** @brief Size of crnet_error::text in bytes, terminator included. */
#define CRNET_ERROR_TEXT_SIZE 256

/**
 * @brief One record of what the platform reported behind a failed call.
 * Known problem: a failure the stack gave no number for records nothing, so
 * an empty record does not by itself mean the call succeeded; compare
 * `result` with what the call returned.
 *
 * Filled by an object's `_get_error`, or through the `out_error` of a call
 * that leaves no object to ask. See @ref crnet_error_detail "Error detail".
 */
/* Frozen: filled in per call, like every error struct. It never changes; a new need gets a new type. */
typedef struct crnet_error {
	/** What the recorded call returned; CR_SUCCESS in an empty record. */
	CR_RESULT result;
	/** Which numbering `code` is in; CR_PLATFORM_ERROR_SOURCE_NONE in an empty
	 * record. */
	CR_PLATFORM_ERROR_SOURCE source;
	/** The platform's number, as it gave it. */
	int64_t code;
	/** The stack's own wording, UTF-8, null-terminated and cut to fit, where
	 * it has some that the caller couldn't get from `code`: libcurl's and
	 * NSError's. Empty for the sources the system's strerror or
	 * FormatMessage already reads. In whatever language the system is set
	 * to; not for parsing. */
	char text[CRNET_ERROR_TEXT_SIZE];
} crnet_error;

/** @brief Which of the two records a TCP socket or a WebSocket keeps. */
typedef enum CRNET_ERROR_SIDE {
	/** connect, recv, and every other call that isn't one of the sending
	 * ones. */
	CRNET_ERROR_SIDE_RECEIVING = 0,
	/** send, and the calls that end sending:
	 * crnet_tcp_socket_finish_sending(), crnet_websocket_close(). */
	CRNET_ERROR_SIDE_SENDING = 1,
} CRNET_ERROR_SIDE;

/** @} */

/**
 * @defgroup crnet_instances Instance
 * @ingroup crnet
 * @brief Where all three parts start: the platform's networking, started and
 * held by a caller. Known problems: on Linux the libcurl 7.84 floor shuts
 * out distributions that ship an older one (Ubuntu 22.04 and RHEL 9 are
 * believed to), TCP sockets included, since the instance is where libcurl
 * starts; the Linux backend holds one static lock around libcurl's start,
 * the one piece of state Carrottus Net has; and that several instances can be made
 * and destroyed independently on Windows and Linux rests on Winsock's and
 * libcurl's documented start counts, which has not been tried.
 *
 * Two of the platforms have something to start before their networking
 * can be used and to stop when it is done with: Winsock (WSAStartup,
 * WSACleanup) and libcurl (its global init and cleanup). On Apple there is
 * nothing. A crnet_instance is that start held by a caller: creating one
 * makes the platform's call, destroying it makes the matching one, and
 * every object in this header is made, directly or through its parent,
 * with an instance that outlasts it.
 *
 * - There is no call to make once per process, and no moment that has to
 *   come before the process's other threads. A part of a program that
 *   uses Carrottus Net makes an instance of its own and need not know whether
 *   any other part has one.
 * - Any number of instances may exist at once. The platform counts its
 *   starts and keeps its networking up until the last is given back;
 *   Carrottus Net itself keeps no count, and no state of its own beyond the
 *   Linux backend's lock named above.
 * - Thread: crnet_instance_create() and crnet_instance_destroy() may
 *   overlap each other and any other call in this header on other
 *   instances, although each starts or stops something of the process's
 *   (Winsock's and libcurl's start counts, the Linux backend's lock). On
 *   one instance they are like any other object's: nothing else is in it
 *   while it is destroyed.
 * - An instance is destroyed after every object made with it. One
 *   destroyed sooner is the caller's mistake and isn't caught. The one
 *   object that may still be at work is a resolver whose lookup hasn't
 *   returned: it has been destroyed as far as the caller is concerned, and
 *   it holds the platform's networking for itself until it is done
 *   (@ref crnet_resolvers "Resolver").
 * - Objects are used with others of their own instance: a wait set takes
 *   members made with the instance it was made with, and refuses any other
 *   (CR_RESULT_NET_INVALID_INSTANCE).
 * - A NULL instance given to a `_create` is
 *   CR_RESULT_NET_INVALID_INSTANCE.
 *
 * libcurl. Its global init is only safe to make while other threads are
 * running since 7.84, and only in a libcurl built for it (it says so
 * itself: CURL_VERSION_THREADSAFE). An instance that may be made at any
 * time, by a part of the program that doesn't know what the rest is doing,
 * needs exactly that, so Carrottus Net requires it rather than pass the old rule
 * on to its callers: carrottus builds against libcurl 7.84 or later only,
 * and crnet_instance_create() returns CR_RESULT_NET_UNSUPPORTED when
 * the libcurl it finds at run time lacks CURL_VERSION_THREADSAFE. Nothing
 * of Carrottus Net works on Linux without an instance, so that includes the TCP
 * sockets.
 *
 * Known problem, beside it: a Linux machine whose libcurl was built
 * without the threaded resolver or c-ares (no CURL_VERSION_ASYNCHDNS) has
 * TCP sockets and the resolver, but no HTTP or WebSocket. Such a libcurl
 * looks names up inside the calls that drive it, blocking for as long as a
 * name server takes, so crnet_http_client_create() and
 * crnet_websocket_create() refuse it rather than let a call with
 * CRNET_NO_WAIT wait.
 * @{
 */

/** @brief Opaque handle to one start of the platform's networking. */
typedef struct crnet_instance_t *crnet_instance;

/**
 * @brief What the caller decides when creating an instance. No field has a
 * default: a zeroed desc is refused, for its `base`, and one with only its
 * `base` set is refused for its `allocator`.
 */
typedef struct crnet_instance_desc {
	/** `type` is CR_STRUCT_TYPE_NET_INSTANCE_DESC; `next` is NULL (nothing
	 * is chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** Where the memory of the instance, and of every object made with it,
	 * comes from (CR_ALLOCATOR). Copied, so the struct need not outlive
	 * the call. Its functions and `user_data` must outlive the instance and
	 * every object made with it, and a resolver until crnet_resolver_run()
	 * has returned for it as well, which may be after the instance has
	 * gone. */
	const CR_ALLOCATOR *allocator;
} crnet_instance_desc;

/**
 * @brief Starts the platform's networking for this instance. Known problem:
 * on Linux it fails with CR_RESULT_NET_UNSUPPORTED on a libcurl built
 * without CURL_VERSION_THREADSAFE, which takes the TCP sockets with it
 * although they need no libcurl.
 *
 * Thread: may overlap any call on any other instance, another create
 * included (@ref crnet_instances "Instance").
 *
 * @param[in]  desc         What the caller decides.
 * @param[out] out_instance Receives the new instance, the caller's to
 *                          destroy with crnet_instance_destroy().
 * @param[out] out_error    Receives the platform's reason when it refuses.
 *                          May be NULL. See
 *                          @ref crnet_error_detail "Error detail".
 *
 * @retval CR_SUCCESS The instance was made.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_INSTANCE_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_instance` is NULL.
 * @retval CR_RESULT_NULL_ALLOCATOR `desc->allocator` is NULL.
 * @retval CR_RESULT_INVALID_ALLOCATOR `desc->allocator` lacks a function.
 * @retval CR_RESULT_OUT_OF_MEMORY The instance could not be allocated.
 * @retval CR_RESULT_NET_UNSUPPORTED Linux: the libcurl found at run
 *         time lacks CURL_VERSION_THREADSAFE. Nothing is recorded in
 *         `out_error`.
 * @retval CR_RESULT_NET_STARTUP_FAILED The platform's networking
 *         wouldn't start; `out_error` has its reason.
 */
CR_RESULT crnet_instance_create(
    const crnet_instance_desc *desc,
    crnet_instance *out_instance,
    crnet_error *out_error);

/**
 * @brief Gives the platform's networking back and frees the instance.
 *
 * Called after every object made with the instance has been destroyed; one
 * destroyed sooner is the caller's mistake and isn't caught.
 *
 * @param[in] instance The instance to destroy. NULL is taken and nothing is
 *                     done with it.
 */
void crnet_instance_destroy(crnet_instance instance);

/** @} */

/**
 * @defgroup crnet_time Time limits
 * @ingroup crnet
 * @brief One meaning for every `timeout_ms` in this header, on every
 * platform. Known problems: which clock a limit counts on across a sleep of
 * the machine is not the same everywhere and is not specified here (the
 * Windows backends' tick count goes on through one), and the Apple HTTP and
 * WebSocket waits follow the wall clock, so a change of the system clock
 * during a wait changes its length; CR_RESULT_NET_SYSTEM_TIMED_OUT and
 * CR_RESULT_NET_STACK_TIMED_OUT have not yet been seen from any
 * backend, since nothing has been run against a host that never answers.
 *
 * It is the longest the whole call may take, in milliseconds, counted by
 * Carrottus Net from the moment the call is made: for a call with several stages
 * (connecting, TLS, sending, receiving) all of them come out of the one
 * allowance. The platforms' own timers - which differ: per stage, per idle
 * stretch, whole seconds - are switched off wherever they can be, and never
 * used to keep the caller's limit. A call that runs out returns
 * CR_RESULT_NET_TIMED_OUT, and that result means this and nothing
 * else: the limit the caller gave is what ended the call.
 *
 * CRNET_NO_WAIT (0) means the call may not wait at all. It does whatever
 * can be done on the spot and returns CR_RESULT_NET_TIMED_OUT if that
 * leaves its part undone:
 *
 * - crnet_tcp_socket_recv(), crnet_websocket_recv() and
 *   crnet_http_transfer_recv() hand over what has already arrived;
 *   crnet_tcp_listener_accept(), a connection already waiting;
 *   crnet_http_transfer_receive_response(), a response that has already
 *   come.
 * - crnet_tcp_socket_send() and crnet_http_transfer_send() send as much
 *   as will be taken right now.
 * - The calls that begin a step the other side has to answer -
 *   crnet_tcp_socket_connect(), crnet_websocket_connect(),
 *   crnet_http_transfer_start() - begin it and return; and
 *   crnet_websocket_send() takes the whole message, or nothing when the
 *   one before it hasn't gone yet. See "Steps" below.
 *
 * This is what replaces a non-blocking mode: a loop that can't stall (a
 * frame loop) passes CRNET_NO_WAIT to every call, and sleeps, when it has
 * nothing to do, in crnet_wait_set_wait().
 *
 * @par Steps
 * Four calls begin something the other side has to answer before it is
 * done - a step: crnet_tcp_socket_connect() and crnet_websocket_connect()
 * open a connection, crnet_http_transfer_start() gets a request out, and
 * crnet_websocket_send() gets a message onto its connection. Each begins
 * its step whatever its limit and waits for it no longer than the limit.
 * When the limit runs out first it returns CR_RESULT_NET_TIMED_OUT and the
 * step goes on; a later call carries it on, within that call's own limit,
 * and returns how it ended:
 *
 * | Step begun by                | Carried on by                                                  |
 * |------------------------------|----------------------------------------------------------------|
 * | crnet_tcp_socket_connect()   | crnet_tcp_socket_connect_continue()                            |
 * | crnet_websocket_connect()    | crnet_websocket_connect_continue()                             |
 * | crnet_http_transfer_start()  | the next call in the order of use: _send or _receive_response |
 * | crnet_websocket_send()       | the next crnet_websocket_send(), crnet_websocket_flush()       |
 *
 * Nothing has failed when a step is left going, and nothing of the
 * caller's is held: what the step needs was copied by the call that began
 * it. A caller that no longer wants it aborts the object or destroys it,
 * which drops the step. No limit spans a step: each call has its own, and
 * a caller with a deadline for the whole keeps it.
 *
 * So CR_RESULT_NET_TIMED_OUT, from any call in this header, means the same
 * two things: the caller's limit is what ended the call, and the object is
 * as usable as it was - calling again is in order.
 *
 * CRNET_WAIT_FOREVER means Carrottus Net sets no limit. The call still ends
 * when the network does - the operating system gives up on a connection
 * that never answers, a peer closes - or when it is aborted. It never
 * returns CR_RESULT_NET_TIMED_OUT.
 *
 * @par Somebody else's timer
 * Carrottus Net's clock is not the only one running: under every call there is
 * the operating system, which stops waiting for a peer that has gone
 * silent, and under the HTTP and WebSocket calls there is the client stack
 * as well. When one of those gives up first - with time still left on the
 * caller's limit, or with no limit at all - it is not the caller's limit
 * that ran out, and the call says so with a result of its own, by which
 * part it was made on:
 *
 * - CR_RESULT_NET_SYSTEM_TIMED_OUT from a TCP socket call. The
 *   operating system gave up on the other end: a connect nobody answered (a
 *   minute or so, by the system's own settings), or an established
 *   connection whose peer stopped acknowledging.
 * - CR_RESULT_NET_STACK_TIMED_OUT from an HTTP transfer or a WebSocket
 *   call. The stack reported a time-out: its own, or the system's passed
 *   up - the stacks don't reliably say which, so both come back as this
 *   one.
 *
 * Either is a failure like any other from the network: the connection or
 * the request is over, only destroy is left, and the object's `_get_error`
 * has the platform's own code for it. What they add is a difference a
 * caller can act on. "Nobody answered" is usually worth another attempt in
 * a way that "refused" (CR_RESULT_NET_CONNECT_FAILED) or a turned-away
 * certificate is not - and neither means the caller should have waited
 * longer, which is what CR_RESULT_NET_TIMED_OUT is left to mean.
 *
 * How long somebody else's timer runs is not Carrottus Net's to set, and it is
 * not the same everywhere. Where a stack's timer can't be switched off it
 * is set as long as the stack accepts, which is the ceiling on a call made
 * with CRNET_WAIT_FOREVER:
 *
 * | Part                          | Longest a call can wait               |
 * |-------------------------------|---------------------------------------|
 * | TCP sockets                   | no ceiling; the system's own patience |
 * | HTTP, WebSocket: WinHTTP      | no ceiling; its timers switch off     |
 * | HTTP, WebSocket: libcurl      | about 25 days, while connecting only  |
 * | HTTP, WebSocket: NSURLSession | one year, for a transfer or a socket  |
 *
 * In practice the operating system has long since given up on anything
 * that would get near the last two.
 * @{
 */

/**
 * @brief A `timeout_ms` that lets the call do only what can be done on the
 * spot; it returns CR_RESULT_NET_TIMED_OUT if that is nothing.
 */
#define CRNET_NO_WAIT      0u

/**
 * @brief A `timeout_ms` that sets no limit: the call ends when the network
 * does or when it is aborted, and never returns
 * CR_RESULT_NET_TIMED_OUT.
 */
#define CRNET_WAIT_FOREVER 0xFFFFFFFFu

/** @} */

/**
 * @defgroup crnet_aborting Aborting
 * @ingroup crnet
 * @brief Ending a wait while it is in progress; the same for every object
 * that has an `_abort`.
 *
 * Those objects are crnet_resolver, crnet_tcp_socket,
 * crnet_tcp_listener, crnet_udp_socket, crnet_http_transfer,
 * crnet_websocket, crnet_service_advert, crnet_service_browser,
 * crnet_service_resolver and crnet_wait_set.
 *
 * Thread: an `_abort` may be called at any point after `_create`, from any
 * thread, overlapping any call on its object but `_destroy`. Whatever call
 * is waiting on that object returns
 * CR_RESULT_NET_ABORTED as soon as the platform lets go of it, and so
 * does every call made on it afterwards. It doesn't wear off - which is
 * what makes it safe against a call that was just about to start - so only
 * `_destroy` is left. The one thing the caller has to prevent is abort
 * racing destroy: abort first, wait for the object's other calls to come
 * back, then destroy.
 *
 * This is how a call waiting with CRNET_WAIT_FOREVER is brought back.
 */

#if CRNET_EXT_TCP

/**
 * @defgroup crnet_tcp TCP sockets
 * @ingroup crnet
 * @brief Winsock2 / BSD sockets: plain TCP over IPv4 or IPv6, no TLS. Known
 * problems: there is no keep-alive option, no local address for an outgoing
 * connection and no query for a socket's own local address; and TCP
 * differs from the WebSocket client in two ways a caller has to remember:
 * its send is partial where a WebSocket's is a whole message, and the end of
 * a connection is 0 bytes with CR_SUCCESS here and
 * CR_RESULT_NET_WS_CLOSED there.
 *
 * A translation of the platform socket API (bind/listen/accept/connect/
 * send/recv), not a networking framework. Anything that has to be private
 * or authenticated goes through the HTTP or WebSocket client further down.
 *
 * Two objects. A crnet_tcp_listener waits for incoming connections on one
 * address and port. A crnet_tcp_socket is one connection, made either by
 * crnet_tcp_listener_accept() or by crnet_tcp_socket_create() followed
 * by one crnet_tcp_socket_connect(). A socket is good for one connection:
 * after connect fails, or the connection ends, only destroy is left.
 *
 * Addresses are numeric: "192.168.1.20", "127.0.0.1", `"::1"`. The calls
 * that take one never look a name up, because the system's lookup can be
 * neither given a time limit nor interrupted, and a connect that hid one
 * couldn't keep its own. Turning a host name into addresses is its own
 * object, the crnet_resolver, and which of the answers to use is the
 * caller's to choose.
 *
 * send and recv mirror the underlying socket calls: either can transfer
 * fewer bytes than asked for. A caller that wants "exactly N bytes or
 * nothing" loops on these itself; Carrottus Net won't do it for you.
 *
 * Thread: on one socket, a receiving call (crnet_tcp_socket_recv()) may
 * overlap a sending call (crnet_tcp_socket_send(),
 * crnet_tcp_socket_finish_sending()), and each may overlap
 * crnet_tcp_socket_abort(). Two receiving calls at once, two sending
 * calls at once, or any other call on the socket during either, are the
 * caller's to prevent. A listener is one caller at a time, as every object
 * is, crnet_tcp_listener_abort() aside.
 * @{
 */

/**
 * @brief Room for any numeric address as text, with its terminator.
 *
 * The longest is an IPv6 one with an embedded IPv4 tail, 45 characters.
 */
#define CRNET_MAX_ADDRESS_STRING_LEN 46

/** @brief Which family of addresses a lookup is limited to. */
typedef enum CRNET_ADDRESS_FAMILY {
	CRNET_ADDRESS_FAMILY_ANY  = 0, /**< Whichever the name has. */
	CRNET_ADDRESS_FAMILY_IPV4 = 1, /**< IPv4 addresses only. */
	CRNET_ADDRESS_FAMILY_IPV6 = 2, /**< IPv6 addresses only. */
} CRNET_ADDRESS_FAMILY;

/**
 * @defgroup crnet_resolvers Resolver
 * @ingroup crnet_tcp
 * @brief A host name's addresses, from the operating system's resolver.
 * Known problems: a resolver that is created and destroyed without ever
 * being run is leaked, so every one must be run; and only Windows can stop a
 * lookup that is in progress - on Linux and Apple the thread in
 * crnet_resolver_run() comes back when the system's getaddrinfo does,
 * however long ago the caller stopped waiting.
 *
 * The system's lookup (getaddrinfo) blocks for as long as it takes -
 * usually milliseconds, up to its own give-up time when a name server
 * isn't answering - and on most platforms nothing can make it return
 * sooner. A time limit and an abort can therefore only be had by not being
 * the call that is stuck in it. So a lookup here is two calls, meant to
 * overlap on two of the caller's threads
 * (@ref crnet_threads "Threads"):
 *
 * - crnet_resolver_run() does the lookup. It blocks until the system has
 *   answered and keeps the answer in the resolver. It goes on a thread the
 *   caller can afford to leave in there.
 * - crnet_resolver_wait() waits for that answer, with a time limit and an
 *   abort like every other wait in this header, and copies it out. It goes
 *   on the thread that wants the addresses.
 *
 * What Carrottus Net adds is the part in between that is easy to get wrong: the
 * answer has to be somewhere both threads can reach and neither can pull
 * out from under the other, whichever finishes first. That is the
 * resolver. It stays until both of two things have happened, in either
 * order: its run has returned, and it has been destroyed. A waiter that
 * gives up destroys it and walks away while the lookup is still out; a
 * lookup that finishes first leaves the answer for the waiter. So:
 *
 * @warning Every resolver is run exactly once and destroyed exactly once.
 *
 * A resolver that turns out not to be needed - the thread for it couldn't
 * be started - is still run: abort it first and crnet_resolver_run()
 * returns at once without looking anything up. Then destroy it.
 *
 * A caller with nothing better to do than wait makes both calls on one
 * thread, run and then wait, and has an ordinary blocking lookup.
 *
 * The usual two-thread use:
 *
 * @code{.c}
 * crnet_resolver_create(instance, &desc, &resolver);
 * start_my_thread(lookup_thread, resolver);   // calls crnet_resolver_run(resolver), nothing else
 * result = crnet_resolver_wait(resolver, 2000, addresses, 8, &count);
 * crnet_resolver_destroy(resolver);         // whether or not it timed out
 * // the thread ends by itself when the system resolver lets it go
 * @endcode
 *
 * @par The instance
 * Since the lookup may still be out when the caller has destroyed the
 * resolver and gone on to destroy the instance it was made with, a resolver
 * doesn't lean on the instance's start of the platform's networking: where
 * the lookup needs one (Winsock) it takes one of its own when it is made
 * and gives it back when it is freed. Destroying the instance after the
 * resolver is therefore in order whether or not the lookup has returned.
 *
 * Thread: crnet_resolver_run() is made once and overlaps every other call
 * on the resolver, crnet_resolver_destroy() included. The others -
 * crnet_resolver_wait(), crnet_resolver_get_error() and destroy - are
 * one caller at a time among themselves, and wait and get_error have
 * returned before destroy is called. crnet_resolver_abort() may overlap
 * any call but destroy.
 * @{
 */

/** @brief Opaque handle to one lookup of one host name. */
typedef struct crnet_resolver_t *crnet_resolver;

/** @brief What crnet_resolver_create() looks up. */
typedef struct crnet_resolver_desc {
	/** `type` is CR_STRUCT_TYPE_NET_RESOLVER_DESC; `next` is NULL (nothing is
	 * chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** The name to look up, UTF-8 and null-terminated; NULL or an empty
	 * string is refused. A numeric address comes back as itself without a
	 * name server being asked. Copied; it doesn't have to outlast the
	 * create call. */
	const char *host;
	/** Limits the answer to one family, or CRNET_ADDRESS_FAMILY_ANY. */
	CRNET_ADDRESS_FAMILY family;
} crnet_resolver_desc;

/**
 * @brief Makes a resolver for one lookup; nothing is looked up yet. Known
 * problem: from here the resolver must be run once and destroyed once - one
 * destroyed without ever being run is leaked.
 *
 * @param[in]  instance     The instance the resolver is made with.
 * @param[in]  desc         The name and family to look up. Only read during
 *                          the call.
 * @param[out] out_resolver Receives the new resolver.
 *
 * @retval CR_SUCCESS The resolver was made.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_resolver` is NULL.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_RESOLVER_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `desc->host` is NULL or empty.
 * @retval CR_RESULT_OUT_OF_MEMORY The resolver could not be allocated.
 * @retval CR_RESULT_NET_STARTUP_FAILED The platform wouldn't give the
 *         resolver its own start of the networking (Winsock; see "The
 *         instance" under @ref crnet_resolvers "Resolver").
 * @retval CR_RESULT_GENERAL_FAILURE The system refused something else the
 *         resolver is made of.
 */
CR_RESULT crnet_resolver_create(
    crnet_instance instance,
    const crnet_resolver_desc *desc,
    crnet_resolver *out_resolver);

/**
 * @brief Does the lookup, on the thread that calls it, for as long as the
 * system takes. Known problem: there is no time limit to give it, and
 * outside Windows nothing brings the calling thread back before the system's
 * resolver returns.
 *
 * It takes the resolver and nothing else so that it can
 * be the whole of a thread's work, and its result is what
 * crnet_resolver_wait() would return at that moment, for a caller that
 * runs and waits on one thread. A thread that was left behind has nobody to
 * tell and ignores it.
 *
 * The resolver may already have been destroyed by the time this returns,
 * and may be destroyed while it runs. Neither is a problem, and the caller
 * of run doesn't use the handle again.
 *
 * Thread: overlaps every other call on the resolver, destroy included: it
 * is meant for a thread of the caller's that does nothing else. Once per
 * resolver.
 *
 * @param[in] resolver The resolver to run.
 *
 * @retval CR_SUCCESS The lookup finished and the name has at least one
 *         address.
 * @retval CR_RESULT_NET_INVALID_RESOLVER `resolver` is NULL, or it has
 *         already been run.
 * @retval CR_RESULT_NET_ABORTED crnet_resolver_abort() was called: at
 *         once when the abort came before the lookup had started; on
 *         Windows also when it comes during it, since Winsock's resolver is
 *         the one that can be told to stop; elsewhere only when the system
 *         lets the lookup go.
 * @retval CR_RESULT_NET_RESOLVE_FAILED The lookup finished and the name
 *         has no address.
 */
CR_RESULT crnet_resolver_run(crnet_resolver resolver);

/**
 * @brief Waits for the lookup to finish and copies its addresses out.
 *
 * The addresses are numeric text, in the order the resolver gave them
 * (which is its preference). Once the lookup has finished, every wait
 * returns the same answer at once. A wait on a resolver nobody runs just
 * uses up its time limit.
 *
 * Thread: may overlap crnet_resolver_run() and crnet_resolver_abort().
 *
 * @param[in]  resolver      The resolver to wait on.
 * @param[in]  timeout_ms    The longest to wait
 *                           (@ref crnet_time "Time limits");
 *                           CRNET_NO_WAIT only looks.
 * @param[out] out_addresses Receives up to `out_capacity` addresses, each a
 *                           null-terminated string.
 * @param[in]  out_capacity  How many addresses `out_addresses` has room for.
 * @param[out] out_count     Receives how many addresses were written; 0 on
 *                           any failure.
 *
 * @retval CR_SUCCESS The lookup finished; `out_count` addresses were
 *         written.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_addresses` or `out_count` is
 *         NULL.
 * @retval CR_RESULT_NET_INVALID_RESOLVER `resolver` is NULL.
 * @retval CR_RESULT_NET_TIMED_OUT The lookup hasn't finished; the
 *         resolver is as it was and may be waited on again, or destroyed
 *         and left.
 * @retval CR_RESULT_NET_ABORTED crnet_resolver_abort() was called.
 * @retval CR_RESULT_NET_RESOLVE_FAILED The lookup finished and the name
 *         has no address; crnet_resolver_get_error() has the resolver's
 *         reason.
 * @retval CR_RESULT_GENERAL_FAILURE The wait itself went wrong in the
 *         system.
 */
CR_RESULT crnet_resolver_wait(
    crnet_resolver resolver,
    uint32_t timeout_ms,
    char out_addresses[][CRNET_MAX_ADDRESS_STRING_LEN],
    uint32_t out_capacity,
    uint32_t *out_count);

/**
 * @brief Ends a wait on the resolver, and every wait after it. Known
 * problem: it stops the lookup itself only on Windows; elsewhere the thread
 * in crnet_resolver_run() stays there until the system's resolver returns.
 *
 * See @ref crnet_aborting "Aborting": a wait in progress, and every wait
 * after it, returns CR_RESULT_NET_ABORTED - also when the lookup had
 * already finished. What it does to the lookup itself is under
 * crnet_resolver_run().
 *
 * Thread: may overlap any call on the resolver but
 * crnet_resolver_destroy().
 *
 * @param[in] resolver The resolver to abort.
 */
void crnet_resolver_abort(crnet_resolver resolver);

/**
 * @brief Reads why the lookup failed, once it has.
 *
 * See @ref crnet_error_detail "Error detail".
 *
 * @param[in]  resolver  The resolver to ask. NULL gives an empty record.
 * @param[out] out_error Receives the record.
 */
void crnet_resolver_get_error(
    crnet_resolver resolver,
    crnet_error *out_error);

/**
 * @brief The caller's half of letting a resolver go. Known problem: the
 * other half is crnet_resolver_run() returning, so a resolver that is
 * destroyed and never run is never freed.
 *
 * Thread: may overlap crnet_resolver_run(), which is the point of it:
 * the resolver is freed when that returns. Not crnet_resolver_abort(),
 * wait or get_error.
 *
 * @param[in] resolver The resolver to destroy. NULL is taken and nothing is
 *                     done with it.
 */
void crnet_resolver_destroy(crnet_resolver resolver);

/** @} */

/**
 * @defgroup crnet_listeners Listener
 * @ingroup crnet_tcp
 * @brief The side that waits to be connected to.
 * @{
 */

/** @brief Opaque handle to a listener on one address and port. */
typedef struct crnet_tcp_listener_t *crnet_tcp_listener;

/** @brief Opaque handle to one TCP connection. */
typedef struct crnet_tcp_socket_t *crnet_tcp_socket;

/** @brief Where crnet_tcp_listener_create() listens. */
typedef struct crnet_tcp_listener_desc {
	/** `type` is CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC; `next` is NULL (nothing is
	 * chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** The local address to listen on, numeric and null-terminated:
	 * "0.0.0.0" for every IPv4 interface, `"::"` for every IPv6 one,
	 * "127.0.0.1" for this machine only, or one interface's own address. An
	 * IPv6 listener takes IPv6 connections only, on every platform;
	 * listening on both families is two listeners. NULL is refused. */
	const char *address;
	/** The port. 0 has the system pick a free one;
	 * crnet_tcp_listener_get_port() says which. */
	uint16_t port;
	/** How many connections may wait to be accepted before the system
	 * starts turning new ones away. The system may cap it. 0 is refused. */
	uint32_t backlog;
} crnet_tcp_listener_desc;

/**
 * @brief Binds and starts listening. Known problem: what Windows does with
 * two listeners that overlap on one port (below) was read from Microsoft's
 * documentation and has not been tried, and the Winsock backend sets neither
 * SO_REUSEADDR nor SO_EXCLUSIVEADDRUSE.
 *
 * When an address and port count as taken, which is the same on every
 * platform:
 *
 * - While a listener holds them, they are taken: a second listener on the
 *   same address and port is CR_RESULT_NET_ADDRESS_IN_USE, from this
 *   program or another.
 * - Once that listener is destroyed, they are free at once. The system
 *   goes on holding the connections it had accepted for a minute or two
 *   after they close, and left to itself Linux and Apple's systems would
 *   refuse the port for that long - a server that restarts would fail to
 *   come back up. Windows has no such wait, so there is none anywhere:
 *   the others are told not to keep one (SO_REUSEADDR). There is no
 *   setting for it because the strict form can't be had on Windows.
 *
 * And the one case that is not the same, and that a caller should not
 * build on: a port with one listener on every address ("0.0.0.0" or `"::"`)
 * and another on one particular address of the same family ("127.0.0.1").
 *
 * - Linux refuses the second, whichever of the two comes first: a
 *   listener on every address has the port on all of them.
 * - Windows refuses it too.
 * - Apple's systems, like the BSD they come from, allow it. They treat
 *   the two as different addresses, and a connection goes to the listener
 *   whose address fits it most closely - the particular one for that
 *   address, the general one for every other.
 *
 * So two listeners that overlap this way start on one platform and fail
 * with CR_RESULT_NET_ADDRESS_IN_USE on the others. A caller that
 * wants a port on several addresses but not all of them makes a listener
 * for each of those addresses and none for every address, which works
 * everywhere.
 *
 * @param[in]  instance     The instance the listener is made with.
 * @param[in]  desc         The address, port and backlog. Only read during
 *                          the call.
 * @param[out] out_listener Receives the new listener.
 * @param[out] out_error    Receives the system's reason for a failure, which
 *                          leaves no listener to ask. May be NULL. See
 *                          @ref crnet_error_detail "Error detail".
 *
 * @retval CR_SUCCESS The listener is listening.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_listener` is NULL.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `desc->address` is NULL or
 *         `desc->backlog` is 0.
 * @retval CR_RESULT_NET_INVALID_ADDRESS `desc->address` isn't a numeric
 *         address.
 * @retval CR_RESULT_OUT_OF_MEMORY The listener could not be allocated.
 * @retval CR_RESULT_NET_ADDRESS_IN_USE The address and port are taken.
 * @retval CR_RESULT_NET_LISTEN_FAILED The system refused to open, bind
 *         or listen on the socket for any other reason.
 * @retval CR_RESULT_GENERAL_FAILURE The system refused something else the
 *         listener is made of.
 */
CR_RESULT crnet_tcp_listener_create(
    crnet_instance instance,
    const crnet_tcp_listener_desc *desc,
    crnet_tcp_listener *out_listener,
    crnet_error *out_error);

/**
 * @brief Reads the port being listened on.
 *
 * @param[in]  listener The listener to ask.
 * @param[out] out_port Receives the port: the desc's, or the one the system
 *                      picked for 0.
 *
 * @retval CR_SUCCESS `out_port` was written.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_port` is NULL.
 * @retval CR_RESULT_NET_INVALID_TCP_LISTENER `listener` is NULL, or the
 *         system couldn't say what it is bound to.
 */
CR_RESULT crnet_tcp_listener_get_port(
    crnet_tcp_listener listener,
    uint16_t *out_port);

/**
 * @brief Waits for an incoming connection and hands back a connected socket
 * for it.
 *
 * The listener keeps listening.
 *
 * @param[in]  listener   The listener to accept from.
 * @param[in]  timeout_ms The longest to wait
 *                        (@ref crnet_time "Time limits").
 * @param[out] out_socket Receives the connected socket, the caller's to
 *                        destroy and the listener's instance's.
 *
 * @retval CR_SUCCESS A connection was accepted.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_socket` is NULL.
 * @retval CR_RESULT_NET_INVALID_TCP_LISTENER `listener` is NULL.
 * @retval CR_RESULT_NET_ABORTED crnet_tcp_listener_abort() was called.
 * @retval CR_RESULT_NET_TIMED_OUT Nobody connected in time; the
 *         listener is as it was.
 * @retval CR_RESULT_OUT_OF_MEMORY The socket could not be allocated.
 * @retval CR_RESULT_NET_ACCEPT_FAILED The system's accept failed;
 *         crnet_tcp_listener_get_error() has its reason.
 */
CR_RESULT crnet_tcp_listener_accept(
    crnet_tcp_listener listener,
    uint32_t timeout_ms,
    crnet_tcp_socket *out_socket);

/**
 * @brief Ends a wait in crnet_tcp_listener_accept(), and every one after
 * it.
 *
 * See @ref crnet_aborting "Aborting".
 *
 * Thread: may overlap any call on the listener but its destroy.
 *
 * @param[in] listener The listener to abort.
 */
void crnet_tcp_listener_abort(crnet_tcp_listener listener);

/**
 * @brief Reads what the platform reported for the most recent
 * crnet_tcp_listener_accept() that failed.
 *
 * See @ref crnet_error_detail "Error detail".
 *
 * @param[in]  listener  The listener to ask. NULL gives an empty record.
 * @param[out] out_error Receives the record.
 */
void crnet_tcp_listener_get_error(
    crnet_tcp_listener listener,
    crnet_error *out_error);

/**
 * @brief Stops listening and frees the listener.
 *
 * Connections already accepted are their own sockets and carry on.
 *
 * @param[in] listener The listener to destroy. NULL is taken and nothing is
 *                     done with it.
 */
void crnet_tcp_listener_destroy(crnet_tcp_listener listener);

/** @} */

/**
 * @defgroup crnet_sockets Socket
 * @ingroup crnet_tcp
 * @brief One connection, from either side.
 *
 * Calls made on a socket that isn't connected (before connect has
 * succeeded, while it is under way, or after it failed), other than abort,
 * destroy, get_error and crnet_tcp_socket_connect_continue(), return
 * CR_RESULT_NET_INVALID_TCP_SOCKET.
 * @{
 */

/** @brief Where crnet_tcp_socket_connect() connects to, and how long it may take. */
typedef struct crnet_tcp_socket_connect_desc {
	/** `type` is CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC; `next` is NULL (nothing is
	 * chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	const char *address; /**< Numeric and null-terminated; see crnet_resolver for names. NULL is refused. */
	uint16_t port;       /**< The port the other end is listening on. */
	/** The longest connecting may take. See
	 * @ref crnet_time "Time limits". */
	uint32_t timeout_ms;
} crnet_tcp_socket_connect_desc;

/**
 * @brief Makes a socket with no connection.
 *
 * It exists before connect so that crnet_tcp_socket_abort() has something
 * to act on while connect is still waiting.
 *
 * @param[in]  instance   The instance the socket is made with.
 * @param[out] out_socket Receives the new socket.
 *
 * @retval CR_SUCCESS The socket was made.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_socket` is NULL.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_OUT_OF_MEMORY The socket could not be allocated.
 */
CR_RESULT crnet_tcp_socket_create(
    crnet_instance instance,
    crnet_tcp_socket *out_socket);

/**
 * @brief Begins connecting to a listening address and port, and waits up
 * to `desc->timeout_ms` for the connection. Once per socket.
 *
 * The step (@ref crnet_time "Time limits", "Steps"): the system's own
 * connection attempt, begun by this call and left to go on when the limit
 * runs out. crnet_tcp_socket_connect_continue() carries it on and says how
 * it ended. Until then the socket counts as not connected: every other
 * call but abort, destroy and get_error returns
 * CR_RESULT_NET_INVALID_TCP_SOCKET, and a second connect does too.
 *
 * A system may connect at once - a loopback connect() often completes
 * inside the call - so this may return CR_SUCCESS with CRNET_NO_WAIT too.
 * A caller treats CR_SUCCESS and CR_RESULT_NET_TIMED_OUT as the two answers
 * it can get, whatever the limit.
 *
 * What is kept for the step: the socket's descriptor, already the
 * socket's, and a flag saying its connect is under way. Nothing else.
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
 * @retval CR_RESULT_NET_TIMED_OUT The connection isn't made yet; the
 *         attempt goes on. Carry it on with
 *         crnet_tcp_socket_connect_continue().
 * @retval CR_RESULT_NET_CONNECT_FAILED The other end refuses or can't
 *         be reached.
 * @retval CR_RESULT_NET_SYSTEM_TIMED_OUT Nothing answered and the
 *         operating system stopped trying before `desc->timeout_ms` was up
 *         (@ref crnet_time "Time limits").
 */
CR_RESULT crnet_tcp_socket_connect(
    crnet_tcp_socket socket,
    const crnet_tcp_socket_connect_desc *desc);

/**
 * @brief Carries on a connect that returned CR_RESULT_NET_TIMED_OUT,
 * waiting up to `timeout_ms` for its outcome.
 *
 * It is the non-blocking connect's second half - wait for the socket to
 * turn writable, then read how the attempt ended - as one call, and takes
 * only what a continuation needs: the desc was taken whole by connect.
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

/**
 * @brief Waits for the connection to take data, then sends as much of `data`
 * as it takes in one go.
 *
 * A short write, same as a raw send(): the caller sends the rest with
 * another call.
 *
 * Thread: may overlap crnet_tcp_socket_recv() and
 * crnet_tcp_socket_abort() on the same socket.
 *
 * @param[in]  socket     The connected socket to send on.
 * @param[in]  data       The bytes to send.
 * @param[in]  size       How many bytes `data` holds.
 * @param[in]  timeout_ms The longest to wait for room
 *                        (@ref crnet_time "Time limits").
 * @param[out] out_sent   Receives how many bytes were sent. May be less than
 *                        `size`, and is never 0 on success.
 *
 * @retval CR_SUCCESS `out_sent` bytes were sent.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_sent` is NULL.
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET `socket` is NULL or isn't
 *         connected.
 * @retval CR_RESULT_NET_ABORTED crnet_tcp_socket_abort() was called.
 * @retval CR_RESULT_ZERO_SIZE `data` is NULL or `size` is 0.
 * @retval CR_RESULT_NET_TIMED_OUT Nothing was sent; the connection is
 *         as it was.
 * @retval CR_RESULT_NET_SYSTEM_TIMED_OUT The operating system has given
 *         the connection up, and it is over.
 * @retval CR_RESULT_NET_SEND_FAILED The connection broke, or sending
 *         had been finished with crnet_tcp_socket_finish_sending().
 */
CR_RESULT crnet_tcp_socket_send(
    crnet_tcp_socket socket,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_sent);

/**
 * @brief Waits for data and copies up to `size` bytes of it.
 *
 * Thread: may overlap crnet_tcp_socket_send(),
 * crnet_tcp_socket_finish_sending() and crnet_tcp_socket_abort() on
 * the same socket.
 *
 * @param[in]  socket       The connected socket to receive on.
 * @param[out] data         Receives the bytes.
 * @param[in]  size         How many bytes `data` has room for.
 * @param[in]  timeout_ms   The longest to wait for data
 *                          (@ref crnet_time "Time limits").
 * @param[out] out_received Receives how many bytes were copied. May be less
 *                          than `size`. 0 with CR_SUCCESS means the peer
 *                          has finished sending and closed its side cleanly
 *                          (same as recv() returning 0).
 *
 * @retval CR_SUCCESS `out_received` bytes were copied, or the peer has
 *         finished sending (0 bytes).
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_received` is NULL.
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET `socket` is NULL or isn't
 *         connected.
 * @retval CR_RESULT_NET_ABORTED crnet_tcp_socket_abort() was called.
 * @retval CR_RESULT_ZERO_SIZE `data` is NULL or `size` is 0.
 * @retval CR_RESULT_NET_TIMED_OUT Nothing arrived in time; the
 *         connection is as it was.
 * @retval CR_RESULT_NET_SYSTEM_TIMED_OUT The operating system has given
 *         the connection up, and it is over.
 * @retval CR_RESULT_NET_RECV_FAILED The connection broke.
 */
CR_RESULT crnet_tcp_socket_recv(
    crnet_tcp_socket socket,
    void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_received);

/**
 * @brief Tells the peer this side has nothing more to send, while still
 * receiving what the peer sends.
 *
 * A half-close; the peer's recv returns 0 bytes. Returns at once. Sending
 * after it fails.
 *
 * Thread: may overlap crnet_tcp_socket_recv() and
 * crnet_tcp_socket_abort() on the same socket.
 *
 * @param[in] socket The connected socket.
 *
 * @retval CR_SUCCESS The sending side is closed.
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET `socket` is NULL or isn't
 *         connected.
 * @retval CR_RESULT_NET_ABORTED crnet_tcp_socket_abort() was called.
 * @retval CR_RESULT_NET_SEND_FAILED The system refused the half-close.
 */
CR_RESULT crnet_tcp_socket_finish_sending(crnet_tcp_socket socket);

/**
 * @brief Sets whether small writes are sent straight away (TCP_NODELAY).
 *
 * @param[in] socket   The connected socket.
 * @param[in] no_delay true sends small writes straight away; false lets the
 *                     system hold them briefly to merge with the next, which
 *                     is every system's state for a new connection.
 *
 * @retval CR_SUCCESS The option was set.
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET `socket` is NULL or isn't
 *         connected.
 * @retval CR_RESULT_NET_ABORTED crnet_tcp_socket_abort() was called.
 * @retval CR_RESULT_GENERAL_FAILURE The system refused the option.
 */
CR_RESULT crnet_tcp_socket_set_no_delay(
    crnet_tcp_socket socket,
    bool no_delay);

/**
 * @brief Reads the address and port of the other end.
 *
 * @param[in]  socket      The connected socket.
 * @param[out] out_address Receives the peer's address as numeric,
 *                         null-terminated text.
 * @param[out] out_port    Receives the peer's port.
 *
 * @retval CR_SUCCESS Both were written.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_address` or `out_port` is
 *         NULL.
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET `socket` is NULL or isn't
 *         connected.
 * @retval CR_RESULT_NET_ABORTED crnet_tcp_socket_abort() was called.
 * @retval CR_RESULT_GENERAL_FAILURE The system couldn't say who the peer
 *         is.
 */
CR_RESULT crnet_tcp_socket_get_peer_address(
    crnet_tcp_socket socket,
    char out_address[CRNET_MAX_ADDRESS_STRING_LEN],
    uint16_t *out_port);

/**
 * @brief Ends a wait on the socket, and every call after it.
 *
 * See @ref crnet_aborting "Aborting". The connection is not closed until
 * destroy.
 *
 * Thread: may overlap any call on the socket but its destroy.
 *
 * @param[in] socket The socket to abort.
 */
void crnet_tcp_socket_abort(crnet_tcp_socket socket);

/**
 * @brief Reads what the platform reported for the socket's most recent
 * failure on one side.
 *
 * See @ref crnet_error_detail "Error detail".
 *
 * Thread: a call on the side it reads: may overlap a call on the other
 * side.
 *
 * @param[in]  socket    The socket to ask. NULL gives an empty record.
 * @param[in]  side      Which of the socket's two records to read.
 * @param[out] out_error Receives the record.
 */
void crnet_tcp_socket_get_error(
    crnet_tcp_socket socket,
    CRNET_ERROR_SIDE side,
    crnet_error *out_error);

/**
 * @brief Drops the connection, whatever state it is in, and frees the
 * socket.
 *
 * @param[in] socket The socket to destroy. NULL is taken and nothing is done
 *                   with it.
 */
void crnet_tcp_socket_destroy(crnet_tcp_socket socket);

/** @} */

/**
 * @defgroup crnet_addresses Addresses
 * @ingroup crnet_tcp
 * @brief An IP address and a port as a value: what the interface listing,
 * UDP sockets and service discovery hand over and take.
 *
 * Why a value and not text: a datagram's source is read on every receive
 * and compared with the peers the caller knows, and an IPv6 link-local
 * address is only usable with its interface (its scope), which text
 * carries as a suffix ("fe80::1%en0") the platforms parse differently.
 * TCP keeps its numeric strings for now.
 *
 * Two crnet_address values Carrottus Net made for one address and port
 * are equal byte for byte: unused bytes and `reserved` are always 0. So a
 * caller may compare them with memcmp and hash their bytes.
 * @{
 */

/**
 * @brief One IPv4 or IPv6 address and a port.
 */
/* Frozen: a value type, passed in and filled in alike. It never changes; a new need gets a new type. */
typedef struct crnet_address {
	/** CRNET_ADDRESS_FAMILY_IPV4 or CRNET_ADDRESS_FAMILY_IPV6. ANY is never
	 * an address's family and is refused wherever one is taken. */
	CRNET_ADDRESS_FAMILY family;
	/** IPv6: the interface index a link-local (fe80::/10) or other scoped
	 * address is used through, as sin6_scope_id has it; 0 for a global
	 * address. IPv4: always 0. */
	uint32_t scope_id;
	/** The address in network byte order, as on the wire: IPv4 in the first
	 * 4 bytes and the other 12 zero, IPv6 in all 16. */
	uint8_t bytes[16];
	/** The port in the machine's own byte order; 0 where no port belongs
	 * (an interface's address) or where 0 asks the system for one (a UDP
	 * socket's local address). */
	uint16_t port;
	/** Always 0, so that equal addresses are equal byte for byte. Refused
	 * when not. */
	uint16_t reserved;
} crnet_address;

/**
 * @brief Makes an address from numeric text. Pure: no system state, no
 * instance, never blocks; inet_pton on every platform.
 *
 * Only a numeric address is taken, never a name ("localhost" is refused:
 * a name is the resolver's). No '%' suffix is taken either: the scope is
 * its own argument, an interface index as crnet_interface_properties has
 * it, so it means the same on every platform.
 *
 * @param[in]  text        "192.168.1.20", "::1", "fe80::1c2b:..."; UTF-8
 *                         and null-terminated. NULL is refused.
 * @param[in]  port        The port, in the machine's byte order.
 * @param[in]  scope_id    0, or for IPv6 the interface index; non-zero with
 *                         an IPv4 address is refused.
 * @param[out] out_address Receives the address. Left as it was on a
 *                         failure.
 *
 * @retval CR_SUCCESS `out_address` was written.
 * @retval CR_RESULT_NET_INVALID_ADDRESS `text` is NULL, not a numeric IPv4
 *         or IPv6 address, holds a '%', or is IPv4 with a non-zero
 *         `scope_id`.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_address` is NULL.
 */
CR_RESULT crnet_address_from_string(
    const char *text,
    uint16_t port,
    uint32_t scope_id,
    crnet_address *out_address);

/**
 * @brief Writes an address's numeric text, without its port or scope:
 * inet_ntop on every platform, so IPv6 in RFC 5952's form (the longest run
 * of zeroes as "::", lower case), and an IPv4-mapped IPv6 address as
 * "::ffff:1.2.3.4". Pure, never blocks.
 *
 * @param[in]  address  The address. NULL is refused.
 * @param[out] out_text Receives the text, null-terminated; room for
 *                      CRNET_MAX_ADDRESS_STRING_LEN bytes. Untouched on
 *                      failure.
 *
 * @retval CR_SUCCESS `out_text` was written.
 * @retval CR_RESULT_NET_INVALID_ADDRESS `address` is NULL, or its family
 *         is neither IPv4 nor IPv6, or its `reserved`, or an IPv4
 *         address's last 12 bytes or scope, aren't 0.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_text` is NULL.
 */
CR_RESULT crnet_address_to_string(
    const crnet_address *address,
    char out_text[CRNET_MAX_ADDRESS_STRING_LEN]);

/** @} */

/**
 * @defgroup crnet_interfaces Interfaces
 * @ingroup crnet_tcp
 * @brief This machine's network interfaces and the addresses each holds,
 * as the system's own table has them at one moment: getifaddrs on Apple,
 * Linux and Android, GetAdaptersAddresses on Windows. Known problems: on
 * Android 11 and later an interface with no address is missing; on Windows
 * a prefix length may be unknown (CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN).
 *
 * Nothing is left out, sorted or folded: loopback is listed, an interface
 * that is down is listed, and an address the system holds twice is listed
 * twice. Which interface to use, advertise on or show is the caller's.
 * The listing never blocks and never asks the resolver: both systems read
 * the kernel's tables.
 *
 * A listing is a snapshot. It doesn't change when the system's table
 * does, and nothing here says when that happens (no change notification);
 * a caller that wants a fresh view takes a new listing.
 *
 * Thread: a list is one caller at a time, as every object is, but for
 * its three reads, which change nothing and may overlap each other (each
 * marked Thread:), so a snapshot can be handed to jobs. Its Debug overlap
 * check opens on the destroy only (each read adds one to an atomic reader
 * count and takes it off, which the destroy checks, and writes nothing
 * else): a destroy overlapping a read is reported.
 * @{
 */

/** @brief Opaque handle to one snapshot of the interface table. */
typedef struct crnet_interface_list_t *crnet_interface_list;

/** @brief Room for an interface's name or display name, with its
 * terminator. A Windows GUID string is 38 bytes; a FriendlyName is
 * whatever the person typed, cut to fit at a UTF-8 character's boundary. */
#define CRNET_INTERFACE_NAME_CAPACITY 256

/** @brief A prefix_length the system doesn't know. Its own value, not 0,
 * since 0 is a real prefix (the default route's). */
#define CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN 0xFFFFFFFFu

/** @brief Bits of crnet_interface_properties::flags. Each is the system's
 * own: IFF_* from getifaddrs; on Windows UP from the administrative state
 * (MIB_IF_ROW2::AdminStatus), RUNNING from OperStatus, LOOPBACK from
 * IfType, POINT_TO_POINT from IfType PPP or tunnel, MULTICAST from the
 * absence of IP_ADAPTER_NO_MULTICAST, and BROADCAST from a broadcast prefix
 * in FirstPrefix. */
enum {
	CRNET_INTERFACE_FLAG_UP             = 0x01, /**< Up: IFF_UP; Windows' AdminStatus up. */
	CRNET_INTERFACE_FLAG_RUNNING        = 0x02, /**< Carrying traffic: IFF_RUNNING; Windows' OperStatus up. */
	CRNET_INTERFACE_FLAG_LOOPBACK       = 0x04, /**< This machine only. */
	CRNET_INTERFACE_FLAG_POINT_TO_POINT = 0x08, /**< A link with one peer (PPP, a tunnel, some VPNs). */
	CRNET_INTERFACE_FLAG_MULTICAST      = 0x10, /**< Takes multicast (which DNS-SD uses). */
	CRNET_INTERFACE_FLAG_BROADCAST      = 0x20, /**< Takes IPv4 broadcast: IFF_BROADCAST; on Windows a broadcast prefix. */
};

/** @brief One interface of a listing, filled by
 * crnet_interface_list_get_properties(). */
typedef struct crnet_interface_properties {
	/** `type` is CR_STRUCT_TYPE_NET_INTERFACE_PROPERTIES; `next` is NULL
	 * (nothing is chained onto this struct yet). The caller sets the type;
	 * Carrottus Net fills in the rest. See CR_STRUCT_BASE. */
	CR_STRUCT_BASE_OUT base;
	/** The system's name for it, UTF-8, null-terminated: ifa_name ("en0"),
	 * or on Windows AdapterName, the adapter's GUID string. Stable while the
	 * interface exists; what a caller keeps to find it again. */
	char name[CRNET_INTERFACE_NAME_CAPACITY];
	/** The name a person knows it by, UTF-8, null-terminated, cut to fit:
	 * Windows' FriendlyName ("Wi-Fi", "Ethernet 2"). Empty on Apple, Linux
	 * and Android, which keep none at this level; a caller shows `name`
	 * there. */
	char display_name[CRNET_INTERFACE_NAME_CAPACITY];
	/** The interface's index (if_nametoindex; Windows' IfIndex). Never 0.
	 * What a crnet_address's scope_id and the discovery descs take. */
	uint32_t index;
	/** The index IPv6 uses for it: Windows' Ipv6IfIndex, which may differ
	 * from IfIndex; elsewhere the same as `index`. 0 on Windows for an
	 * interface without IPv6. */
	uint32_t ipv6_index;
	/** CRNET_INTERFACE_FLAG_* bits. */
	uint32_t flags;
	/** The largest IP packet the link takes, in bytes, as the system
	 * reports it; 0 where it reported none. */
	uint32_t mtu;
	/** How many addresses crnet_interface_list_get_address() has for it; 0
	 * for an interface with none. */
	uint32_t address_count;
} crnet_interface_properties;

/** @brief One address an interface holds, filled by
 * crnet_interface_list_get_address(). */
typedef struct crnet_interface_address {
	/** `type` is CR_STRUCT_TYPE_NET_INTERFACE_ADDRESS; `next` is NULL
	 * (nothing is chained onto this struct yet). The caller sets the type;
	 * Carrottus Net fills in the rest. See CR_STRUCT_BASE. */
	CR_STRUCT_BASE_OUT base;
	/** The address, port 0. An IPv6 link-local address has its interface's
	 * ipv6_index as its scope_id, ready to be sent to. */
	crnet_address address;
	/** How many leading bits are the network's: 0-32 for IPv4, 0-128 for
	 * IPv6 (24 for a /24), or CRNET_INTERFACE_PREFIX_LENGTH_UNKNOWN where
	 * the system holds none (Windows' OnLinkPrefixLength 255, "an illegal
	 * value"). */
	uint32_t prefix_length;
} crnet_interface_address;

/**
 * @brief Takes a snapshot of this machine's interfaces and their
 * addresses: getifaddrs, or GetAdaptersAddresses(AF_UNSPEC,
 * GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
 * GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX) and GetIfTable2()
 * for the administrative state, copied into the list through the
 * instance's allocator and the system's own memory given back before it
 * returns. Never blocks on the network.
 *
 * @param[in]  instance  The instance the list is made with; destroyed after
 *                       it.
 * @param[out] out_list  Receives the list, the caller's to destroy with
 *                       crnet_interface_list_destroy(). NULL on failure.
 * @param[out] out_error Receives the system's reason when it refuses. May
 *                       be NULL. See @ref crnet_error_detail "Error detail".
 *                       Once card S10 is built, the same platform error
 *                       also goes to the instance's message callback, inside
 *                       this call, as every platform failure does there.
 *
 * @retval CR_SUCCESS The list was made. A machine with no interface at all
 *         gives an empty list, not a failure.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_list` is NULL.
 * @retval CR_RESULT_OUT_OF_MEMORY The list could not be allocated.
 * @retval CR_RESULT_NET_INTERFACE_ENUMERATION_FAILED getifaddrs or
 *         GetAdaptersAddresses failed; `out_error` has its reason (errno,
 *         or the Win32 error).
 */
CR_RESULT crnet_interface_enumerate(
    crnet_instance instance,
    crnet_interface_list *out_list,
    crnet_error *out_error);

/**
 * @brief Reads how many interfaces the list holds.
 *
 * Thread: a read; may overlap any other read of the same list (the three
 * reads change nothing), never its destroy.
 *
 * @param[in]  list      The list to read.
 * @param[out] out_count Receives the count; 0 for an empty list.
 *
 * @retval CR_SUCCESS `out_count` was written.
 * @retval CR_RESULT_NET_INVALID_INTERFACE_LIST `list` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_count` is NULL.
 */
CR_RESULT crnet_interface_list_get_count(
    crnet_interface_list list,
    uint32_t *out_count);

/**
 * @brief Reads one interface of the list.
 *
 * Thread: a read, as crnet_interface_list_get_count().
 *
 * @param[in]  list           The list to read.
 * @param[in]  position       Which interface, from 0 to the count less 1,
 *                            in the system's order. Not an interface index.
 * @param[out] out_properties Receives the interface; its `base.type` set by
 *                            the caller.
 *
 * @retval CR_SUCCESS `out_properties` was written.
 * @retval CR_RESULT_NET_INVALID_INTERFACE_LIST `list` is NULL.
 * @retval CR_RESULT_INDEX_OUT_OF_RANGE `position` is past the last.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_properties` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `out_properties->base.type` is not
 *         CR_STRUCT_TYPE_NET_INTERFACE_PROPERTIES.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `out_properties->base.next` is
 *         not NULL.
 */
CR_RESULT crnet_interface_list_get_properties(
    crnet_interface_list list,
    uint32_t position,
    crnet_interface_properties *out_properties);

/**
 * @brief Reads one address of one interface of the list, in the order the
 * system gave them.
 *
 * Thread: a read, as crnet_interface_list_get_count().
 *
 * @param[in]  list             The list to read.
 * @param[in]  position         Which interface, as for
 *                              crnet_interface_list_get_properties().
 * @param[in]  address_position Which of its addresses, from 0 to its
 *                              `address_count` less 1.
 * @param[out] out_address      Receives the address; its `base.type` set
 *                              by the caller.
 *
 * @retval CR_SUCCESS `out_address` was written.
 * @retval CR_RESULT_NET_INVALID_INTERFACE_LIST `list` is NULL.
 * @retval CR_RESULT_INDEX_OUT_OF_RANGE `position` or `address_position` is
 *         past the last.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_address` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `out_address->base.type` is not
 *         CR_STRUCT_TYPE_NET_INTERFACE_ADDRESS.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `out_address->base.next` is not
 *         NULL.
 */
CR_RESULT crnet_interface_list_get_address(
    crnet_interface_list list,
    uint32_t position,
    uint32_t address_position,
    crnet_interface_address *out_address);

/**
 * @brief Frees the list.
 *
 * @param[in] list The list to destroy. NULL is taken and nothing is done
 *                 with it.
 */
void crnet_interface_list_destroy(crnet_interface_list list);

/** @} */

/** @} */

#endif // CRNET_EXT_TCP

#if CRNET_EXT_UDP

/**
 * @defgroup crnet_udp UDP sockets
 * @ingroup crnet
 * @brief Winsock2 / BSD sockets: datagrams over IPv4 or IPv6, sent to an
 * address and received with their source, no connection. Known problems:
 * on iOS, iPadOS and macOS 15 and later a send to the local network before
 * the person has allowed it fails like a missing route, and can't be told
 * from one; Apple sends no datagram larger than the socket's send buffer,
 * 9216 bytes unless send_buffer_size raises it; on iOS a suspended app's
 * sockets may be reclaimed, after which only their destroy is of use;
 * EXCLUSIVE keeps the same address and port only (CRNET_UDP_PORT_SHARING).
 *
 * A translation of the platform's datagram socket (bind, sendto,
 * recvfrom), not a protocol. Nothing is ordered, acknowledged, sent again,
 * split or joined: a datagram may arrive twice, late, out of order or not
 * at all, and that is the caller's to handle. Nothing is sealed either;
 * anything private is sealed by the caller before it is sent.
 *
 * One object, a crnet_udp_socket, bound to one local address and port when
 * it is made. It sends to any address of its family and receives from any.
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
 * What is the caller's to know, per platform:
 *
 * - iOS and iPadOS 14 and later, and macOS 15 and later: Local Network
 *   privacy. The first datagram to an address on the local network brings
 *   up the system's prompt, and the app needs NSLocalNetworkUsageDescription
 *   in its Info.plist. While the person hasn't allowed it, a send to a local
 *   address fails (believed to be EHOSTUNREACH, not confirmed), reported as
 *   CR_RESULT_NET_SEND_FAILED with errno in the record: Carrottus Net can't
 *   tell that from a real "no route", and the system offers no call to ask.
 * - Apple's largest outgoing datagram is the socket's send buffer
 *   (SO_SNDBUF), whose default is net.inet.udp.maxdgram, 9216 bytes; a
 *   larger one is CR_RESULT_NET_DATAGRAM_TOO_BIG. iOS shares xnu; its
 *   default is not separately confirmed. Linux and Windows take up to
 *   65507 (IPv4) / 65527 (IPv6). A caller that stays under its path's MTU
 *   (crnet_interface_properties::mtu, less the headers) meets neither.
 * - Android: the caller's manifest needs android.permission.INTERNET, or
 *   socket() fails (EACCES, reported as CR_RESULT_NET_ACCESS_DENIED).
 *   Android 17 (API 37): an app that targets it needs the runtime
 *   permission ACCESS_LOCAL_NETWORK to send and receive unicast UDP on the
 *   local network; without it a send or receive fails, "typically" EPERM,
 *   reported as CR_RESULT_NET_ACCESS_DENIED, errno in the record. Asking for
 *   the permission is the caller's. Neither is confirmed yet.
 * - iOS and iPadOS: while an app is suspended, the system may reclaim its
 *   sockets; every later call on such a socket fails (EBADF or ENOTCONN,
 *   reported as CR_RESULT_NET_SEND_FAILED or _RECV_FAILED; in a wait set,
 *   the member is named ready, as a socket with an error is by poll(), and
 *   its next call returns that failure, errno in the record), and the
 *   caller destroys it and makes another. Which sockets to close before
 *   suspension is the caller's.
 * - Windows: the first bind of a program to a non-loopback address may
 *   bring up Windows Defender Firewall's prompt for inbound traffic; the
 *   caller's (an installer's rule, or the person's answer).
 *
 * Thread: on one socket, a receiving call (crnet_udp_socket_recv_from())
 * may overlap a sending call (crnet_udp_socket_send_to()), and each may
 * overlap crnet_udp_socket_abort(), as on a TCP socket. Two receiving or
 * two sending calls at once, or any other call during either, are the
 * caller's to prevent.
 * @{
 */

/** @brief Opaque handle to one UDP socket. */
typedef struct crnet_udp_socket_t *crnet_udp_socket;

/**
 * @brief Whether another socket may bind the same address and port, in the
 * two forms that mean the same on every platform. No value is a default:
 * NONE is refused.
 */
typedef enum CRNET_UDP_PORT_SHARING {
	/** Not chosen; refused. */
	CRNET_UDP_PORT_SHARING_NONE = 0,
	/** No other socket, of this program or another, may bind the same
	 * address and port while this one is open, and this one is refused if
	 * one already has: Windows' SO_EXCLUSIVEADDRUSE; elsewhere no reuse
	 * option set.
	 *
	 * Only the same address is kept from others on every platform. A bind
	 * of the same port on another address that overlaps this one (a
	 * specific address over this socket's wildcard, or the wildcard over
	 * this socket's specific address) differs, and is not held here:
	 * - Apple: allowed to a socket that shares (SO_REUSEADDR), of the same
	 *   user, either way; the more specific socket then takes the datagrams
	 *   sent to its address;
	 * - Windows: a later wildcard bind over this specific address is
	 *   allowed, "bound to all interfaces except the specific address";
	 * - Linux: refused unless both sockets share.
	 * A caller that needs a port to itself binds it on the wildcard of each
	 * family it uses, and still can't stop another program of the same user
	 * on Apple taking a specific address on it. */
	CRNET_UDP_PORT_SHARING_EXCLUSIVE = 1,
	/** Every socket that binds the address and port with SHARED may hold it
	 * at once: SO_REUSEADDR and SO_REUSEPORT on Apple, Linux and Android,
	 * SO_REUSEADDR on Windows. Which of them a datagram to that port is
	 * handed to differs by platform (Linux spreads them across sockets of
	 * one user; Apple gives each to the last bound; Windows doesn't say), so
	 * it is undefined here: a caller that shares a port makes no assumption
	 * about which socket receives. Linux refuses SHARED to a second socket
	 * of another user. */
	CRNET_UDP_PORT_SHARING_SHARED = 2,
} CRNET_UDP_PORT_SHARING;

/**
 * @brief A `receive_buffer_size` or `send_buffer_size` that leaves the
 * system's own size, unchanged. Its own value, not 0, so that a zeroed desc
 * has chosen nothing. Named for sockets, not UDP: a TCP desc that comes to
 * take buffer sizes takes it too.
 */
#define CRNET_SOCKET_BUFFER_SIZE_SYSTEM 0xFFFFFFFFu

/** @brief What crnet_udp_socket_create() binds, and how. */
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
	 * On Apple, also the largest datagram the socket sends: the granted size
	 * (crnet_udp_socket_get_buffer_sizes()), 9216 with the system's
	 * (net.inet.udp.maxdgram). */
	uint32_t send_buffer_size;
} crnet_udp_socket_desc;

/**
 * @brief Opens a non-blocking datagram socket and binds it: socket,
 * IPV6_V6ONLY for IPv6, the sharing options, the buffer sizes, bind, and on
 * Windows SIO_UDP_CONNRESET and SIO_UDP_NETRESET switched off.
 *
 * @param[in]  instance   The instance the socket is made with.
 * @param[in]  desc       What the caller decides. Only read during the call.
 * @param[out] out_socket Receives the socket, the caller's to destroy. NULL
 *                        on failure.
 * @param[out] out_error  Receives the system's reason for a failure, which
 *                        leaves no socket to ask. May be NULL. See
 *                        @ref crnet_error_detail "Error detail". Once card
 *                        S10 is built, the same platform error also goes to
 *                        the instance's message callback, inside this call.
 *
 * @retval CR_SUCCESS The socket is bound.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_UDP_SOCKET_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_socket` is NULL.
 * @retval CR_RESULT_NET_INVALID_ADDRESS `desc->local`'s family is neither
 *         IPv4 nor IPv6, its `reserved` isn't 0, or it is IPv4 with a scope
 *         or with bytes past the fourth.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `desc->sharing` is no value of
 *         its enum, or a buffer size is 0.
 * @retval CR_RESULT_OUT_OF_MEMORY The socket could not be allocated.
 * @retval CR_RESULT_NET_ADDRESS_IN_USE The address and port are taken
 *         (EADDRINUSE, WSAEADDRINUSE; WSAEACCES on Windows, which is what
 *         SO_EXCLUSIVEADDRUSE held by another gives).
 * @retval CR_RESULT_NET_ACCESS_DENIED The system refused this program the
 *         network (Android without INTERNET, EACCES); never on Apple.
 *         `out_error` has it.
 * @retval CR_RESULT_NET_BIND_FAILED The system refused to open, set up or
 *         bind the socket for any other reason (an address this machine
 *         doesn't hold); `out_error` has it.
 */
CR_RESULT crnet_udp_socket_create(
    crnet_instance instance,
    const crnet_udp_socket_desc *desc,
    crnet_udp_socket *out_socket,
    crnet_error *out_error);

/**
 * @brief Reads the address and port the socket is bound to (getsockname):
 * the desc's, with the port the system picked for 0.
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
 * @brief Reads the buffer sizes the system granted (getsockopt SO_RCVBUF
 * and SO_SNDBUF), which may differ from the desc's: Linux reports twice
 * what it was asked, for its own bookkeeping, capped by net.core.rmem_max
 * and wmem_max; the others cap by their own settings.
 *
 * @param[in]  socket                  The socket to ask.
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
 * @brief Sends one datagram to one address: the whole of it, or nothing.
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
 * @retval CR_RESULT_ZERO_SIZE `data` is NULL and `size` isn't 0, TCP's
 *         send's code for a missing buffer (an empty datagram, NULL and 0,
 *         is sent).
 * @retval CR_RESULT_NET_DATAGRAM_TOO_BIG Longer than the system sends in
 *         one; nothing was sent.
 * @retval CR_RESULT_NET_TIMED_OUT No room in time; nothing was sent, and
 *         the socket is as it was.
 * @retval CR_RESULT_NET_ABORTED crnet_udp_socket_abort() was called.
 * @retval CR_RESULT_NET_SEND_FAILED The system refused it for another
 *         reason (no route; Local Network privacy on Apple, believed to look
 *         the same, not confirmed: no errno is documented for it; a socket
 *         iOS has reclaimed); crnet_udp_socket_get_error() with
 *         CRNET_ERROR_SIDE_SENDING has it. The socket stays usable, but for
 *         one the system has reclaimed (iOS, above), which fails every call
 *         until destroyed.
 * @retval CR_RESULT_NET_ACCESS_DENIED The system refused this program the
 *         local network for this send (Android 17's ACCESS_LOCAL_NETWORK not
 *         granted, EPERM); never on Apple, whose refusal looks like no
 *         route. The socket stays usable once allowed; the record has the
 *         errno.
 */
CR_RESULT crnet_udp_socket_send_to(
    crnet_udp_socket socket,
    const crnet_address *to,
    const void *data,
    uint32_t size,
    uint32_t timeout_ms);

/**
 * @brief Receives one datagram and where it came from.
 *
 * With CRNET_NO_WAIT it hands over a datagram already waiting, or returns
 * CR_RESULT_NET_TIMED_OUT; with a limit it waits for one that long. After a
 * wait set names the socket, call this with CRNET_NO_WAIT until TIMED_OUT.
 *
 * The arguments are in crnet_tcp_socket_recv()'s order (the buffer, its
 * size, the limit, then the outputs). Sizes are uint32_t, not TCP's
 * uint64_t: no datagram is larger than 65,535 bytes, and a type that says
 * so is a check the caller can't miss.
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
 *                        link-local source with its scope_id. Written on
 *                        success and on TRUNCATED; untouched on any other
 *                        result.
 *
 * @retval CR_SUCCESS One whole datagram was handed over.
 * @retval CR_RESULT_NET_DATAGRAM_TRUNCATED One datagram longer than
 *         `capacity`: its first `capacity` bytes and its source were handed
 *         over and the rest is gone. Not a failure of the socket; the next
 *         call receives the next datagram.
 * @retval CR_RESULT_NET_INVALID_UDP_SOCKET `socket` is NULL.
 * @retval CR_RESULT_ZERO_SIZE `buffer` is NULL or `capacity` is 0, as TCP's
 *         receive.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_size` or `out_from` is NULL.
 * @retval CR_RESULT_NET_TIMED_OUT Nothing arrived in time; the socket is as
 *         it was.
 * @retval CR_RESULT_NET_ABORTED crnet_udp_socket_abort() was called.
 * @retval CR_RESULT_NET_RECV_FAILED The system's receive failed;
 *         crnet_udp_socket_get_error() with CRNET_ERROR_SIDE_RECEIVING has
 *         it.
 * @retval CR_RESULT_NET_ACCESS_DENIED The system refused this program the
 *         local network for this receive (Android 17's ACCESS_LOCAL_NETWORK
 *         not granted, EPERM); never on Apple. The socket stays usable once
 *         allowed; the record has the errno.
 */
CR_RESULT crnet_udp_socket_recv_from(
    crnet_udp_socket socket,
    void *buffer,
    uint32_t capacity,
    uint32_t timeout_ms,
    uint32_t *out_size,
    crnet_address *out_from);

/**
 * @brief Ends a wait in a send or a receive, and every call after it. See
 * @ref crnet_aborting "Aborting".
 *
 * Thread: may overlap any call on the socket but its destroy.
 *
 * @param[in] socket The socket to abort.
 */
void crnet_udp_socket_abort(crnet_udp_socket socket);

/**
 * @brief Reads what the platform reported for the most recent failed call
 * on one side of the socket. See @ref crnet_error_detail "Error detail".
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
 * @brief Closes the socket and frees it. Datagrams waiting in it are
 * dropped; ones already handed to the system may still go.
 *
 * @param[in] socket The socket to destroy. NULL is taken and nothing is
 *                   done with it.
 */
void crnet_udp_socket_destroy(crnet_udp_socket socket);

/** @} */

#endif // CRNET_EXT_UDP

#if CRNET_EXT_DISCOVERY

/**
 * @defgroup crnet_discovery Service discovery
 * @ingroup crnet
 * @brief DNS-SD over multicast DNS on the local network, through
 * the system's own service: dns_sd on Apple, Windows' DNS-SD API, Avahi on
 * Linux. Known problems: on Linux a create blocks in avahi-daemon's
 * D-Bus round trips, with no limit of the caller's; on iOS a suspended
 * app's discovery objects may need making again; and on Windows (seen on
 * Windows 11 build 26300, 2026-11-24):
 *
 * - a browse names no interface, and hands over every answer and goodbye
 *   the system hears, so FOUND may come several times for one instance
 *   and LOST once for each goodbye (two after one advert's destroy); and
 *   while another program on the machine registers and cancels adverts
 *   in quick succession, a departing advert's LOST may never come (none
 *   in 30 s, four runs of four; one killed was reported at once);
 * - of two adverts of one name on one machine, records differing, the
 *   second was REGISTERED in most runs and ended with NAME_CONFLICT in
 *   one: Windows' answer varies (another machine's claim untried);
 * - a resolve has no time-out of its own (none came in 60 s): a resolver
 *   of an instance that has gone stays silent, and the caller's limit is
 *   all there is; its answer gives the host's addresses on the network, at
 *   most one per family, never loopback's;
 * - a destroy waits for the system's last callback: DnsServiceRegisterCancel,
 *   DnsServiceBrowseCancel and DnsServiceResolveCancel called back inside
 *   the call, DnsServiceDeRegister within milliseconds after it.
 *
 * Three objects, each a translation of one operation of the system's:
 *
 * - A crnet_service_advert makes this machine answer for one service
 *   instance ("Living room._example._udp.local." on a port) until it is
 *   destroyed.
 * - A crnet_service_browser reports the instances of one type as they
 *   appear and go: FOUND and LOST.
 * - A crnet_service_resolver turns one found instance into its host name,
 *   port, TXT record and addresses: SERVICE and ADDRESS_ADDED.
 *
 * Each is made with its `_create`, which begins the operation and returns
 * at once; its results are events read by its `_next`, which takes a time
 * limit as every wait in this header does, and with CRNET_NO_WAIT hands
 * over what has already arrived. Each can be in a wait set, named when it
 * has an event. Nothing here calls the caller back.
 *
 * Pure translation: the service type, the name, the port, the TXT
 * entries and the interfaces are all the caller's, with no default.
 * The domain is always "local.". No name is ever changed for the caller: a
 * name that is taken ends the advert with
 * CR_RESULT_NET_SERVICE_NAME_CONFLICT. Taken means held by another
 * service: on Apple two adverts of one machine with the same name, type,
 * port and TXT record are one service to mDNSResponder, and the second is
 * REGISTERED, not a conflict; one whose records differ conflicts.
 *
 * What the caller must give the system, per platform:
 *
 * - iOS and iPadOS 14 and later: NSLocalNetworkUsageDescription and
 *   NSBonjourServices (every type browsed or advertised) in the app's
 *   Info.plist, and the person's permission, asked by the system at first
 *   use. A type missing from NSBonjourServices is
 *   CR_RESULT_NET_DISCOVERY_DENIED; while the person is being asked,
 *   each object hands over a LOCAL_NETWORK_NOT_ALLOWED event and waits.
 *   macOS 15 and later asks the same permission of apps.
 * - Linux: avahi-daemon running; otherwise every create is
 *   CR_RESULT_NET_DISCOVERY_UNAVAILABLE.
 * - Windows: the DNS-SD calls in the system's dnsapi.dll (Windows 10 or
 *   later), and mDNS (UDP 5353) let through the firewall on the network's
 *   profile.
 *
 * Thread: each object is one caller at a time; its `_abort` may overlap
 * any call on it but its destroy. While an object is in a wait set, a wait
 * on the set counts as a call on it: its `_next` may not overlap the wait.
 * @{
 */

/** @brief Room for an instance name, with its terminator: a DNS
 * label is at most 63 bytes. */
#define CRNET_SERVICE_NAME_CAPACITY 64
/** @brief Room for a service type, with its terminator:
 * "_" + at most 15 characters + "._udp" or "._tcp" (RFC 6335). */
#define CRNET_SERVICE_TYPE_CAPACITY 32
/** @brief Room for a host name, with its terminator: a DNS name is
 * at most 255 bytes. */
#define CRNET_SERVICE_HOST_CAPACITY 256
/** @brief The largest TXT record an advert takes, and the most of
 * a found one a resolve event holds, in bytes of DNS wire form.
 * RFC 6763's section 6.2 calls a TXT record over 1300 bytes "NOT
 * RECOMMENDED" (its one-packet bound, section 6.1, is about 8900), so an
 * advert keeps to it; a found one may be larger, and is then cut and said
 * so (txt_truncated). */
#define CRNET_SERVICE_TXT_CAPACITY 1300

/**
 * @brief Which interfaces an advert or a browser works on. No value
 * is a default: NONE is refused.
 */
typedef enum CRNET_DISCOVERY_SCOPE {
	/** Not chosen; refused. */
	CRNET_DISCOVERY_SCOPE_NONE = 0,
	/** Every interface the system's service uses (dns_sd's
	 * kDNSServiceInterfaceIndexAny, Avahi's AVAHI_IF_UNSPEC, Windows'
	 * InterfaceIndex 0). */
	CRNET_DISCOVERY_SCOPE_ALL_INTERFACES = 1,
	/** The one interface named by the desc's `interface_index`, an index as
	 * crnet_interface_properties has it. */
	CRNET_DISCOVERY_SCOPE_ONE_INTERFACE = 2,
} CRNET_DISCOVERY_SCOPE;

/**
 * @brief One TXT entry of an advert, as "key=value".
 */
/* Frozen: an array element the caller passes in. It never changes; a new need gets a new type. */
typedef struct crnet_service_txt_entry {
	/** Printable US-ASCII (0x20 to 0x7E) without '=', 1 byte or more,
	 * null-terminated. Keys are compared ignoring case by DNS-SD; two
	 * entries whose keys differ only in case are refused. */
	const char *key;
	/** UTF-8 without a NUL, null-terminated; may be empty (""), never NULL.
	 * The key, '=' and the value are at most 255 bytes together. */
	const char *value;
} crnet_service_txt_entry;

/* ------------------------------------------------------------------------ */
/* Advert                                                                    */
/* ------------------------------------------------------------------------ */

/** @brief Opaque handle to one advertised service instance. Its
 * event ring holds three, made at its create: an advert has at
 * most three events to hand over, LOCAL_NETWORK_NOT_ALLOWED, REGISTERED and then
 * the failure that ends it, so it needs no `event_capacity`; a repeat of
 * either (a permission withdrawn and given again in Settings) replaces the
 * unread one of its kind, so it never drops one. */
typedef struct crnet_service_advert_t *crnet_service_advert;

/** @brief What crnet_service_advert_create() advertises. */
typedef struct crnet_service_advert_desc {
	/** `type` is CR_STRUCT_TYPE_NET_SERVICE_ADVERT_DESC; `next` is NULL
	 * (nothing is chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** The instance name, the one people see ("Nathan's Mac"): UTF-8, 1 to
	 * 63 bytes, null-terminated, no ASCII control character
	 * (0x00 to 0x1F, 0x7F: RFC 6763, section 4.1.1). Copied. */
	const char *name;
	/** The service type, "_name._udp" or "_name._tcp", the name 1 to 15
	 * letters, digits and hyphens, beginning and ending with a
	 * letter or digit (RFC 6335, section 5.1), no trailing dot. Copied. */
	const char *type;
	/** The port the service is reached on, this machine's socket's; not 0.
	 * Carrottus Net doesn't check that anything listens there. */
	uint16_t port;
	/** Which interfaces to answer on. */
	CRNET_DISCOVERY_SCOPE scope;
	/** With CRNET_DISCOVERY_SCOPE_ONE_INTERFACE, the interface's index
	 * (not 0); otherwise 0. */
	uint32_t interface_index;
	/** The TXT entries, in the order they go into the record; NULL when
	 * `txt_entry_count` is 0 (an empty record). Copied. */
	const crnet_service_txt_entry *txt_entries;
	/** How many entries `txt_entries` holds. Their record in wire form (a
	 * length byte and the entry, each) is at most
	 * CRNET_SERVICE_TXT_CAPACITY bytes. */
	uint32_t txt_entry_count;
} crnet_service_advert_desc;

/** @brief What an advert's event says. */
typedef enum CRNET_SERVICE_ADVERT_EVENT_KIND {
	/** No event: never handed over, so a zeroed event reads as
	 * none. */
	CRNET_SERVICE_ADVERT_EVENT_NONE = 0,
	/** The system's service has the instance registered and answers for
	 * it; `name` is the one asked for (it is never changed). Comes once. */
	CRNET_SERVICE_ADVERT_EVENT_REGISTERED = 1,
	/** The system doesn't allow this app the local network
	 * now: dns_sd's kDNSServiceErr_PolicyDenied while the operation waits
	 * (iOS, iPadOS, macOS 15 and later), the one signal Apple gives whether
	 * the person is being asked or has said no, so a caller shows "Allow
	 * the local network in Settings" at once. Never on Windows or Linux.
	 * The advert goes on: REGISTERED follows if the person allows it. */
	CRNET_SERVICE_ADVERT_EVENT_LOCAL_NETWORK_NOT_ALLOWED = 2,
} CRNET_SERVICE_ADVERT_EVENT_KIND;

/** @brief One event of an advert, filled by
 * crnet_service_advert_next(). */
typedef struct crnet_service_advert_event {
	/** `type` is CR_STRUCT_TYPE_NET_SERVICE_ADVERT_EVENT; `next` is NULL.
	 * The caller sets the type; Carrottus Net fills in the rest. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE_OUT base;
	/** What happened. */
	CRNET_SERVICE_ADVERT_EVENT_KIND kind;
	/** The instance's name, UTF-8, null-terminated. */
	char name[CRNET_SERVICE_NAME_CAPACITY];
} crnet_service_advert_event;

/**
 * @brief Begins advertising one service instance on this machine:
 * DNSServiceRegister (kDNSServiceFlagsNoAutoRename, the host NULL, so this
 * machine's name), DnsServiceRegister (an instance from
 * DnsServiceConstructInstance), or an AvahiEntryGroup with the service,
 * committed. Returns at once; REGISTERED comes as an event.
 *
 * The host is always this machine, under the name the system's service
 * keeps for it.
 *
 * @param[in]  instance   The instance the advert is made with.
 * @param[in]  desc       What the caller decides. Only read during the
 *                        call.
 * @param[out] out_advert Receives the advert, the caller's to destroy.
 *                        NULL on failure.
 * @param[out] out_error  Receives the system's reason for a failure, which
 *                        leaves no advert to ask. May be NULL. See
 *                        @ref crnet_error_detail "Error detail". Once card
 *                        S10 is built, the same platform error also goes
 *                        to the instance's message callback, inside the
 *                        call; so for the browser's and resolver's create.
 *
 * @retval CR_SUCCESS The registration has begun.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_SERVICE_ADVERT_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_advert` is NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS The name, the type, the port,
 *         the scope and its index, or a TXT entry or the record's size
 *         breaks its rule above.
 * @retval CR_RESULT_OUT_OF_MEMORY The advert could not be allocated.
 * @retval CR_RESULT_NET_DISCOVERY_UNAVAILABLE The system's service isn't
 *         there.
 * @retval CR_RESULT_NET_DISCOVERY_DENIED The system refused this program
 *         or this type.
 * @retval CR_RESULT_NET_DISCOVERY_FAILED The system refused the
 *         registration for another reason; `out_error` has it.
 */
CR_RESULT crnet_service_advert_create(
    crnet_instance instance,
    const crnet_service_advert_desc *desc,
    crnet_service_advert *out_advert,
    crnet_error *out_error);

/**
 * @brief Hands over the advert's next event, waiting for one no
 * longer than `timeout_ms`. A failure that comes after REGISTERED (the
 * name taken later by a device that joined, the daemon restarted) is
 * returned by the next call, and the advert is then over.
 *
 * @param[in]  advert     The advert.
 * @param[in]  timeout_ms The longest to wait (@ref crnet_time "Time limits").
 * @param[out] out_event  Receives the event; its `base.type` set by the
 *                        caller.
 *
 * @retval CR_SUCCESS `out_event` was written.
 * @retval CR_RESULT_NET_INVALID_SERVICE_ADVERT `advert` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_event` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `out_event->base.type` is not
 *         CR_STRUCT_TYPE_NET_SERVICE_ADVERT_EVENT.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `out_event->base.next` is not
 *         NULL.
 * @retval CR_RESULT_NET_TIMED_OUT No event in time; the advert is as it
 *         was (an advert registered and well has no more events, so this
 *         is what every later call returns).
 * @retval CR_RESULT_NET_ABORTED crnet_service_advert_abort() was called.
 * @retval CR_RESULT_NET_SERVICE_NAME_CONFLICT The name is taken; the
 *         advert is over and no longer answers.
 * @retval CR_RESULT_NET_DISCOVERY_DENIED The system refused it after it
 *         was made (iOS's permission withdrawn).
 * @retval CR_RESULT_NET_DISCOVERY_FAILED The system's service failed it
 *         or went away; crnet_service_advert_get_error() has its code.
 */
CR_RESULT crnet_service_advert_next(
    crnet_service_advert advert,
    uint32_t timeout_ms,
    crnet_service_advert_event *out_event);

/**
 * @brief Ends a wait in crnet_service_advert_next(), and every call
 * after it. The instance stays advertised until the advert is destroyed.
 * See @ref crnet_aborting "Aborting".
 *
 * Thread: may overlap any call on the advert but its destroy.
 *
 * @param[in] advert The advert to abort.
 */
void crnet_service_advert_abort(crnet_service_advert advert);

/**
 * @brief Reads what the system reported for the advert's most recent
 * failure: a DNSServiceErrorType, a Win32 error, or an Avahi error code.
 * See @ref crnet_error_detail "Error detail".
 *
 * @param[in]  advert    The advert to ask. NULL gives an empty record.
 * @param[out] out_error Receives the record.
 */
void crnet_service_advert_get_error(
    crnet_service_advert advert,
    crnet_error *out_error);

/**
 * @brief Stops advertising and frees the advert: the system's
 * service sends its goodbye, so browsers elsewhere see LOST. Known problem:
 * on Windows it waits for DnsServiceDeRegister's callback.
 *
 * @param[in] advert The advert to destroy. NULL is taken and nothing is
 *                   done with it.
 */
void crnet_service_advert_destroy(crnet_service_advert advert);

/* ------------------------------------------------------------------------ */
/* Browser                                                                   */
/* ------------------------------------------------------------------------ */

/** @brief Opaque handle to one browse for one service type. */
typedef struct crnet_service_browser_t *crnet_service_browser;

/** @brief What crnet_service_browser_create() looks for. */
typedef struct crnet_service_browser_desc {
	/** `type` is CR_STRUCT_TYPE_NET_SERVICE_BROWSER_DESC; `next` is NULL
	 * (nothing is chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** The service type, as for an advert. Copied. */
	const char *type;
	/** Which interfaces to look on. */
	CRNET_DISCOVERY_SCOPE scope;
	/** With CRNET_DISCOVERY_SCOPE_ONE_INTERFACE, the interface's index
	 * (not 0); otherwise 0. */
	uint32_t interface_index;
	/** How many unread events the browser keeps; not 0. Allocated when the
	 * browser is made. Past it, events are dropped and EVENTS_DROPPED says
	 * so. */
	uint32_t event_capacity;
} crnet_service_browser_desc;

/** @brief What a browse event says. */
typedef enum CRNET_SERVICE_BROWSE_EVENT_KIND {
	/** No event: never handed over. */
	CRNET_SERVICE_BROWSE_EVENT_NONE = 0,
	/** An instance of the type is there, on `interface_index` (0 on
	 * Windows, which doesn't say). dns_sd: kDNSServiceFlagsAdd;
	 * Avahi: AVAHI_BROWSER_NEW; Windows: a PTR record with a TTL, each
	 * time one is heard. */
	CRNET_SERVICE_BROWSE_EVENT_FOUND = 1,
	/** One FOUND of before has gone: the same name and interface.
	 * dns_sd: a reply without kDNSServiceFlagsAdd; Avahi:
	 * AVAHI_BROWSER_REMOVE; Windows: a PTR record with a TTL of 0, each
	 * goodbye heard. */
	CRNET_SERVICE_BROWSE_EVENT_LOST = 2,
	/** Events were dropped because the ring was full: what the caller
	 * holds may be stale. `name` is empty. */
	CRNET_SERVICE_BROWSE_EVENT_EVENTS_DROPPED = 3,
	/** As the advert's: waiting for the person to allow the local
	 * network; finds follow if allowed. `name` is empty. */
	CRNET_SERVICE_BROWSE_EVENT_LOCAL_NETWORK_NOT_ALLOWED = 4,
} CRNET_SERVICE_BROWSE_EVENT_KIND;

/** @brief One event of a browser, filled by
 * crnet_service_browser_next(). */
typedef struct crnet_service_browse_event {
	/** `type` is CR_STRUCT_TYPE_NET_SERVICE_BROWSE_EVENT; `next` is NULL.
	 * The caller sets the type; Carrottus Net fills in the rest. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE_OUT base;
	/** What happened. */
	CRNET_SERVICE_BROWSE_EVENT_KIND kind;
	/** The instance's name, UTF-8, null-terminated, as another device
	 * advertised it: not to be trusted, and only a name. A name
	 * another device sends that no advert could carry (over 63 bytes, not
	 * UTF-8, a control character) is dropped, never handed over, so every
	 * name handed over is one a resolver's desc takes; the browse reader is
	 * fuzzed. */
	char name[CRNET_SERVICE_NAME_CAPACITY];
	/** The interface it was seen on: what a resolver of it takes. 0 where
	 * the system doesn't say (Windows). */
	uint32_t interface_index;
} crnet_service_browse_event;

/**
 * @brief Begins browsing for one service type: DNSServiceBrowse,
 * DnsServiceBrowse, or an AvahiServiceBrowser (AVAHI_PROTO_UNSPEC).
 * Returns at once; what is found comes as events, this machine's own
 * adverts included.
 *
 * @param[in]  instance    The instance the browser is made with.
 * @param[in]  desc        What the caller decides. Only read during the
 *                         call.
 * @param[out] out_browser Receives the browser, the caller's to destroy.
 *                         NULL on failure.
 * @param[out] out_error   Receives the system's reason for a failure. May
 *                         be NULL.
 *
 * @retval CR_SUCCESS The browse has begun.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_SERVICE_BROWSER_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_browser` is NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS The type, the scope and its
 *         index, or `event_capacity` breaks its rule above.
 * @retval CR_RESULT_OUT_OF_MEMORY The browser or its ring could not be
 *         allocated.
 * @retval CR_RESULT_NET_DISCOVERY_UNAVAILABLE The system's service isn't
 *         there.
 * @retval CR_RESULT_NET_DISCOVERY_DENIED The system refused this program
 *         or this type.
 * @retval CR_RESULT_NET_DISCOVERY_FAILED The system refused the browse for
 *         another reason; `out_error` has it.
 */
CR_RESULT crnet_service_browser_create(
    crnet_instance instance,
    const crnet_service_browser_desc *desc,
    crnet_service_browser *out_browser,
    crnet_error *out_error);

/**
 * @brief Hands over the browser's next event, oldest first, waiting
 * for one no longer than `timeout_ms`. On Apple and Linux this is where
 * the system's replies are read (DNSServiceProcessResult, Avahi's
 * dispatch), only as far as they are already waiting.
 *
 * @param[in]  browser    The browser.
 * @param[in]  timeout_ms The longest to wait (@ref crnet_time "Time limits").
 * @param[out] out_event  Receives the event; its `base.type` set by the
 *                        caller.
 *
 * @retval CR_SUCCESS `out_event` was written.
 * @retval CR_RESULT_NET_INVALID_SERVICE_BROWSER `browser` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_event` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `out_event->base.type` is not
 *         CR_STRUCT_TYPE_NET_SERVICE_BROWSE_EVENT.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `out_event->base.next` is not
 *         NULL.
 * @retval CR_RESULT_NET_TIMED_OUT No event in time; the browser is as it
 *         was.
 * @retval CR_RESULT_NET_ABORTED crnet_service_browser_abort() was called.
 * @retval CR_RESULT_NET_DISCOVERY_DENIED The system refused it after it
 *         was made.
 * @retval CR_RESULT_NET_DISCOVERY_FAILED The system's service failed the
 *         browse or went away; the browser is over.
 *         crnet_service_browser_get_error() has its code. Events read
 *         before it stand.
 */
CR_RESULT crnet_service_browser_next(
    crnet_service_browser browser,
    uint32_t timeout_ms,
    crnet_service_browse_event *out_event);

/**
 * @brief Ends a wait in crnet_service_browser_next(), and every
 * call after it. See @ref crnet_aborting "Aborting".
 *
 * Thread: may overlap any call on the browser but its destroy.
 *
 * @param[in] browser The browser to abort.
 */
void crnet_service_browser_abort(crnet_service_browser browser);

/**
 * @brief Reads what the system reported for the browser's most
 * recent failure. See @ref crnet_error_detail "Error detail".
 *
 * @param[in]  browser   The browser to ask. NULL gives an empty record.
 * @param[out] out_error Receives the record.
 */
void crnet_service_browser_get_error(
    crnet_service_browser browser,
    crnet_error *out_error);

/**
 * @brief Stops browsing and frees the browser and its unread
 * events. Known problem: on Windows it waits for DnsServiceBrowseCancel's
 * callback.
 *
 * @param[in] browser The browser to destroy. NULL is taken and nothing is
 *                    done with it.
 */
void crnet_service_browser_destroy(crnet_service_browser browser);

/* ------------------------------------------------------------------------ */
/* Resolver                                                                  */
/* ------------------------------------------------------------------------ */

/** @brief Opaque handle to one resolve of one found instance. Not
 * a crnet_resolver, which looks up a host name. */
typedef struct crnet_service_resolver_t *crnet_service_resolver;

/** @brief What crnet_service_resolver_create() resolves: an
 * instance as a FOUND event named it. */
typedef struct crnet_service_resolver_desc {
	/** `type` is CR_STRUCT_TYPE_NET_SERVICE_RESOLVER_DESC; `next` is NULL
	 * (nothing is chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** The instance's name, as the FOUND event had it. Copied. */
	const char *name;
	/** The service type, as browsed. Copied. */
	const char *type;
	/** The interface the instance was found on (the event's
	 * `interface_index`); 0 for any interface, as a Windows
	 * FOUND gives it (dns_sd's kDNSServiceInterfaceIndexAny, Avahi's
	 * AVAHI_IF_UNSPEC). */
	uint32_t interface_index;
	/** Which addresses of its host to report: IPv4, IPv6, or ANY for both. */
	CRNET_ADDRESS_FAMILY family;
	/** How many unread events the resolver keeps; not 0. */
	uint32_t event_capacity;
} crnet_service_resolver_desc;

/** @brief What a resolve event says. */
typedef enum CRNET_SERVICE_RESOLVE_EVENT_KIND {
	/** No event: never handed over. */
	CRNET_SERVICE_RESOLVE_EVENT_NONE = 0,
	/** The instance's host, port and TXT record: `host`, `port`, `txt`.
	 * At least once on success; again on dns_sd when the record changes
	 * (its resolve goes on until destroyed), never again on Avahi and
	 * Windows (theirs is one answer). */
	CRNET_SERVICE_RESOLVE_EVENT_SERVICE = 1,
	/** One address of the host: `address`, its port the service's. On Apple
	 * from DNSServiceGetAddrInfo on the host, begun by the resolver itself
	 * once SERVICE has come (dns_sd separates the two steps; Avahi and
	 * Windows give the address with the service, at most one per family). */
	CRNET_SERVICE_RESOLVE_EVENT_ADDRESS_ADDED = 2,
	/** An ADDRESS_ADDED of before is no longer the host's (dns_sd only). */
	CRNET_SERVICE_RESOLVE_EVENT_ADDRESS_REMOVED = 3,
	/** Events were dropped because the ring was full. */
	CRNET_SERVICE_RESOLVE_EVENT_EVENTS_DROPPED = 4,
	/** As the advert's: waiting for the person to allow the local
	 * network. */
	CRNET_SERVICE_RESOLVE_EVENT_LOCAL_NETWORK_NOT_ALLOWED = 5,
} CRNET_SERVICE_RESOLVE_EVENT_KIND;

/** @brief One event of a resolver, filled by
 * crnet_service_resolver_next(). Which fields are written depends on its
 * kind; the others are zero. */
typedef struct crnet_service_resolve_event {
	/** `type` is CR_STRUCT_TYPE_NET_SERVICE_RESOLVE_EVENT; `next` is NULL.
	 * The caller sets the type; Carrottus Net fills in the rest. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE_OUT base;
	/** What happened. */
	CRNET_SERVICE_RESOLVE_EVENT_KIND kind;
	/** SERVICE: the host's name, null-terminated ("Nathans-Mac.local.").
	 * Another device's claim: not to be trusted. */
	char host[CRNET_SERVICE_HOST_CAPACITY];
	/** SERVICE: the service's port. */
	uint16_t port;
	/** ADDRESS_ADDED and ADDRESS_REMOVED: the address, with the service's
	 * port and, for IPv6 link-local, the interface as its scope_id. */
	crnet_address address;
	/** SERVICE: the TXT record's size in bytes as received, which may be
	 * more than CRNET_SERVICE_TXT_CAPACITY. */
	uint32_t txt_size;
	/** SERVICE: true when `txt_size` is more than
	 * CRNET_SERVICE_TXT_CAPACITY and `txt` holds only its first bytes. */
	bool txt_truncated;
	/** SERVICE: the TXT record in DNS wire form (each entry a length byte
	 * and that many bytes), as the system received it, up to
	 * CRNET_SERVICE_TXT_CAPACITY bytes. Windows, which hands it over as
	 * keys and values, has it written back into wire form, each entry as
	 * "key=value". Parsing it is the caller's. */
	uint8_t txt[CRNET_SERVICE_TXT_CAPACITY];
} crnet_service_resolve_event;

/**
 * @brief Begins resolving one found instance:
 * DNSServiceCreateConnection, then DNSServiceResolve and, once it
 * answers, DNSServiceGetAddrInfo on its host, both with
 * kDNSServiceFlagsShareConnection on that connection (so one descriptor;
 * dns_sd.h: only a connection's own ref can be shared, and deallocating
 * it ends every operation on it; macOS tested 2026-11-21: host, port, TXT
 * and then ::1, fe80::1 with its scope and 127.0.0.1); DnsServiceResolve;
 * or an AvahiServiceResolver. Returns at once (on Linux, after avahi-
 * daemon's answers).
 *
 * @param[in]  instance     The instance the resolver is made with.
 * @param[in]  desc         What the caller decides. Only read during the
 *                          call.
 * @param[out] out_resolver Receives the resolver, the caller's to destroy.
 *                          NULL on failure.
 * @param[out] out_error    Receives the system's reason for a failure. May
 *                          be NULL.
 *
 * @retval CR_SUCCESS The resolve has begun.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_SERVICE_RESOLVER_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_resolver` is NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS The name, the type, the
 *         interface index, the family or `event_capacity` breaks its rule.
 * @retval CR_RESULT_OUT_OF_MEMORY The resolver or its ring could not be
 *         allocated.
 * @retval CR_RESULT_NET_DISCOVERY_UNAVAILABLE The system's service isn't
 *         there.
 * @retval CR_RESULT_NET_DISCOVERY_DENIED The system refused this program
 *         or this type.
 * @retval CR_RESULT_NET_DISCOVERY_FAILED The system refused the resolve
 *         for another reason; `out_error` has it.
 */
CR_RESULT crnet_service_resolver_create(
    crnet_instance instance,
    const crnet_service_resolver_desc *desc,
    crnet_service_resolver *out_resolver,
    crnet_error *out_error);

/**
 * @brief Hands over the resolver's next event, oldest first,
 * waiting for one no longer than `timeout_ms`.
 *
 * When the caller has what it needs (often the first SERVICE and one
 * ADDRESS_ADDED), it destroys the resolver: on dns_sd it otherwise goes
 * on watching.
 *
 * @param[in]  resolver   The resolver.
 * @param[in]  timeout_ms The longest to wait (@ref crnet_time "Time limits").
 * @param[out] out_event  Receives the event; its `base.type` set by the
 *                        caller.
 *
 * @retval CR_SUCCESS `out_event` was written.
 * @retval CR_RESULT_NET_INVALID_SERVICE_RESOLVER `resolver` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_event` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `out_event->base.type` is not
 *         CR_STRUCT_TYPE_NET_SERVICE_RESOLVE_EVENT.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `out_event->base.next` is not
 *         NULL.
 * @retval CR_RESULT_NET_TIMED_OUT No event in time; the resolver is as it
 *         was.
 * @retval CR_RESULT_NET_ABORTED crnet_service_resolver_abort() was called.
 * @retval CR_RESULT_NET_DISCOVERY_DENIED The system refused it after it
 *         was made.
 * @retval CR_RESULT_NET_SERVICE_NOT_ANSWERING On Avahi, its own time-out
 *         for an instance that didn't answer; the resolver is over, the
 *         system well. Never on dns_sd; none seen on Windows, whose resolve
 *         has no time-out of its own (known problems above).
 * @retval CR_RESULT_NET_DISCOVERY_FAILED The system's service failed the
 *         resolve or went away; the resolver is over.
 *         crnet_service_resolver_get_error() has its code.
 */
CR_RESULT crnet_service_resolver_next(
    crnet_service_resolver resolver,
    uint32_t timeout_ms,
    crnet_service_resolve_event *out_event);

/**
 * @brief Ends a wait in crnet_service_resolver_next(), and every
 * call after it. See @ref crnet_aborting "Aborting".
 *
 * Thread: may overlap any call on the resolver but its destroy.
 *
 * @param[in] resolver The resolver to abort.
 */
void crnet_service_resolver_abort(crnet_service_resolver resolver);

/**
 * @brief Reads what the system reported for the resolver's most
 * recent failure. See @ref crnet_error_detail "Error detail".
 *
 * @param[in]  resolver  The resolver to ask. NULL gives an empty record.
 * @param[out] out_error Receives the record.
 */
void crnet_service_resolver_get_error(
    crnet_service_resolver resolver,
    crnet_error *out_error);

/**
 * @brief Stops resolving and frees the resolver and its unread
 * events. Known problem: on Windows it waits for
 * DnsServiceResolveCancel's callback when the resolve hasn't ended.
 *
 * @param[in] resolver The resolver to destroy. NULL is taken and nothing
 *                     is done with it.
 */
void crnet_service_resolver_destroy(crnet_service_resolver resolver);

/** @} */

#endif // CRNET_EXT_DISCOVERY

/**
 * @defgroup crnet_stack HTTP and WebSocket
 * @ingroup crnet
 * @brief What the HTTP and WebSocket clients share: the platform's own client
 * stack (WinHTTP / NSURLSession / libcurl). Known problems: the stack adds
 * its own request headers and which ones differs by platform, which is a
 * choice still made for the caller (NSURLSession adds Host, Accept,
 * Accept-Language, Connection, Accept-Encoding and User-Agent to a plain
 * GET; the other two have not been looked at); and a server that ignores
 * `Accept-Encoding: identity` and compresses anyway is handed over still
 * compressed by libcurl and WinHTTP and decoded by NSURLSession.
 *
 * TLS and HTTP versions are the platform stack's. Which certificates a
 * server may present, and whether a proxy stands between here and it, are
 * the caller's to say, in a crnet_tls_desc and a crnet_proxy_desc
 * (@ref crnet_tls "TLS" and @ref crnet_proxies "Proxy"). Carrottus Net
 * carries one request or one message at a time and nothing else: no retry,
 * no redirect unless asked for, no cookies, no stored credentials, no
 * cache, no reconnect, no keep-alive pings of its own. It holds none of the
 * caller's data either: a body, in either direction, passes through in the
 * pieces the caller moves it in, and is never gathered up here. What a
 * status code or a message means is the caller's.
 *
 * The calls here that can wait are crnet_http_transfer_start(), `_send`,
 * `_receive_response` and `_recv`, and crnet_websocket_connect(), `_send`
 * and `_recv`; each takes a time limit and can be ended by the object's
 * `_abort` while it waits, as @ref crnet_time "Time limits" and
 * @ref crnet_aborting "Aborting" describe. The rest return without
 * waiting on the network. No time limit has a default.
 *
 * @par Host names
 * A URL here names its host, and the stack looks that name up itself -
 * unlike a TCP socket, which is given a numeric address and has the
 * crnet_resolver to get one. The two differ because what kept the lookup
 * out of crnet_tcp_socket_connect() doesn't hold here: the stack's lookup
 * is part of the call that needed it, comes out of that call's time limit
 * and ends with its abort. And the stack can't do without the name, which
 * goes to the server in the request and in the TLS handshake and is what
 * the server's certificate is checked against.
 *
 * What the caller doesn't get is the choice a resolver gives: where a name
 * has several addresses, or both an IPv4 and an IPv6 one, which is used is
 * the stack's to pick. A URL may give a numeric address in place of a name
 * (`"http://192.168.1.20:8080/"`, `"http://[::1]/"`), which is then the one
 * used; over TLS the server's certificate has to be for that address, or
 * the crnet_tls_desc has to say how else the server is known.
 *
 * @par Decoding and the stack's own headers
 * Two things are the stack's and can't be turned off evenly across
 * platforms, so they are the same everywhere instead: a compressed
 * response body arrives decoded, and the stack adds its own standard
 * request headers (Host, Accept-Encoding, on some platforms User-Agent and
 * Accept) where the caller set none.
 *
 * Decoding can't be switched off - NSURLSession has no way to hand over a
 * body still compressed - but compression can be declined, which comes to
 * the same thing and works on every stack. A caller that needs a response
 * body byte for byte as the server holds it sends the request header
 *
 * @code{.unparsed}
 * Accept-Encoding: identity
 * @endcode
 *
 * which tells the server to send it as it is. The stack then sends that
 * in place of its own Accept-Encoding, nothing arrives compressed, and
 * there is nothing to decode. That is what a download wants when it will
 * be resumed (a Range counts bytes as they are sent, so it only lines up
 * with what was received if the two are the same bytes), or checked
 * against a size or a hash somebody published. Everything else - an API
 * reply above all - is better left alone, smaller on the wire and decoded
 * on arrival.
 *
 * `identity` is the one value of Accept-Encoding a caller may send. Any
 * other would have the caller choosing compressions for a decoder it
 * doesn't own, with a result that differs by stack, and is refused
 * (CR_RESULT_DESC_INVALID_PARAMETERS).
 *
 * @par Results
 * A bad argument is refused before anything is sent: CR_RESULT_NULL_DESC,
 * CR_RESULT_NULL_OUTPUT_PARAMETER, CR_RESULT_DESC_INVALID_PARAMETERS (a
 * missing field, a size limit of 0, a combination the desc's own comments
 * rule out, text that isn't UTF-8) or CR_RESULT_NET_INVALID_URL (not a
 * URL, or not a scheme the call carries). Once on the network, a failure is
 * one of
 *
 * | Result                             | Meaning                                                                         |
 * |------------------------------------|---------------------------------------------------------------------------------|
 * | CR_RESULT_NET_RESOLVE_FAILED  | the host name has no address                                                    |
 * | CR_RESULT_NET_CONNECT_FAILED  | the server refused or can't be reached                                          |
 * | CR_RESULT_NET_TLS_FAILED      | handshake or certificate check failed, the crnet_tls_desc's checks included   |
 * | CR_RESULT_NET_TIMED_OUT       | the caller's time limit ran out                                                 |
 * | CR_RESULT_NET_STACK_TIMED_OUT | the stack gave up waiting first (@ref crnet_time "Time limits")               |
 *
 * or the part's own catch-all: CR_RESULT_NET_HTTP_FAILED for an HTTP
 * transfer's start and its wait for the response,
 * CR_RESULT_NET_SEND_FAILED and CR_RESULT_NET_RECV_FAILED for
 * moving a body or a WebSocket message. A connection that breaks after it
 * was made is the catch-all, not CR_RESULT_NET_CONNECT_FAILED. How
 * finely a failure is told apart is the stack's; treat the catch-alls as
 * "it didn't work" rather than as a
 * diagnosis. The diagnosis, such as the stack gave one, is the object's
 * `_get_error` (@ref crnet_error_detail "Error detail").
 * @{
 */

/**
 * @brief One request header. Known problem: the name is a generic one for
 * something only the HTTP and WebSocket clients use.
 *
 * Both strings are UTF-8 and null-terminated; neither may be NULL. A name
 * given twice goes out the way the stack sends a repeat - as two headers,
 * or as one with the values joined by ", " - so a caller that needs one
 * particular form joins the values itself.
 */
/* Frozen: an array element of the HTTP and WebSocket descs. It never changes; a new need gets a new type. */
typedef struct crnet_header {
	const char *name;  /**< The header's name, without the colon. */
	const char *value; /**< The header's value. */
} crnet_header;

/**
 * @defgroup crnet_tls TLS
 * @ingroup crnet_stack
 * @brief Which certificates a server may present. Known problems: on WinHTTP
 * a request that starts as http:// and is redirected to https:// gets
 * WinHTTP's full checks on the https hop, so a custom root or
 * `skip_host_name_check` doesn't reach it; on libcurl a root that isn't a
 * certificate is only found at the first connection, as
 * CR_RESULT_NET_TLS_FAILED, and not when the desc is handed over; on
 * Apple, pins need macOS 12, iOS 15 or watchOS 8, and Apple's own rules for
 * server certificates still apply to a self-signed one given as a root;
 * there are no client certificates, no minimum TLS version, no revocation
 * checking and no way to read back the certificate a server presented; and
 * none of it has been run against a certificate issued by a public
 * authority.
 *
 * One crnet_tls_desc says it for every https:// or wss:// connection of
 * the object it is given to: an HTTP client takes one when it is created
 * (not per transfer - one that reuses an open connection meets no
 * certificate to check), a WebSocket with its connect desc. It is checked
 * for sense there even when the URL turns out to be http:// or ws://, where
 * it then has nothing to act on.
 *
 * Three questions, answered separately:
 *
 * - `trust`: which roots a certificate chain has to end at.
 * - `skip_host_name_check`: whether the certificate has to be for the host
 *   in the URL.
 * - `pins`: whether the server's own key has to be one of a known few, on
 *   top of whatever the first two let through.
 *
 * A zeroed desc is the stack's own behaviour with nothing added: the system
 * trust store, the host name checked, no pins.
 *
 * A server with a self-signed certificate is reached one of three ways:
 * its certificate given as the one root (CRNET_TLS_TRUST_ROOTS - it is
 * its own issuer); CRNET_TLS_TRUST_ANY with a pin, which identifies the
 * server by its key alone; or CRNET_TLS_TRUST_ANY by itself, which
 * accepts whoever answers and is only for a machine the caller trusts the
 * network path to.
 *
 * A certificate these turn away is CR_RESULT_NET_TLS_FAILED from
 * crnet_websocket_connect(), or from the HTTP transfer call that was
 * waiting when the connection came up (crnet_http_transfer_start(), or a
 * later one where the stack connects late), before the request has gone
 * out. A desc that contradicts itself is
 * CR_RESULT_DESC_INVALID_PARAMETERS from the call it was handed to, as is
 * a root that isn't a certificate (on WinHTTP and NSURLSession; see the
 * known problems above for libcurl). CR_RESULT_NET_UNSUPPORTED means
 * the stack can't do what was asked - only libcurl can say that, of one
 * built with a TLS library that lacks roots from memory or pinning.
 *
 * What the stack checks apart from these (validity dates, key usage, its
 * own rules for what a server certificate must look like) stays on, except
 * under CRNET_TLS_TRUST_ANY. On Apple platforms App Transport Security
 * is a further gate in front of all of it, set in the application's
 * Info.plist and not from here.
 * @{
 */

/** @brief Which roots a server's certificate chain has to end at. */
typedef enum CRNET_TLS_TRUST {
	/** The platform's trust store, as the stack uses it unasked. */
	CRNET_TLS_TRUST_SYSTEM = 0,
	/** The desc's `roots` and nothing else: a chain that ends at a root the
	 * system trusts but the desc doesn't list is turned away. */
	CRNET_TLS_TRUST_ROOTS = 1,
	/** Either: a chain that ends at one of the desc's `roots` or at one the
	 * system trusts. */
	CRNET_TLS_TRUST_SYSTEM_AND_ROOTS = 2,
	/** Nothing about the certificate is checked - not its chain, not its
	 * dates, not its name - so the connection is encrypted to whoever
	 * answered. `pins`, when given, still apply. */
	CRNET_TLS_TRUST_ANY = 3,
} CRNET_TLS_TRUST;

/**
 * @brief One X.509 certificate in DER - the binary form, one certificate to
 * a blob.
 *
 * A PEM file is the same bytes in base64 between its BEGIN and END lines;
 * decoding that is the caller's (`openssl x509 -outform der`).
 */
/* Frozen: an array element of crnet_tls_desc. It never changes; a new need gets a new type. */
typedef struct crnet_tls_certificate {
	const void *data; /**< The certificate's DER bytes. */
	uint64_t size;    /**< How many bytes `data` holds. */
} crnet_tls_certificate;

/** @brief Size of a crnet_tls_pin in bytes: one SHA-256. */
#define CRNET_TLS_PIN_SIZE 32

/**
 * @brief The SHA-256 of a certificate's public key.
 *
 * It is the hash of the key's DER SubjectPublicKeyInfo, the form HTTP public
 * key pinning and libcurl's "sha256//" use, here as the 32 raw bytes rather
 * than their base64. It follows the key, not the certificate, so it
 * survives a renewal that keeps the key.
 *
 * @code{.sh}
 * openssl x509 -in server.pem -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256
 * @endcode
 */
/* Frozen: an array element of crnet_tls_desc. It never changes; a new need gets a new type. */
typedef struct crnet_tls_pin {
	uint8_t sha256[CRNET_TLS_PIN_SIZE]; /**< The hash, as raw bytes. */
} crnet_tls_pin;

/**
 * @brief Which certificates a server may present, for every TLS connection
 * of the object the desc is given to. Known problems: on WinHTTP `roots` and
 * `skip_host_name_check` don't reach an https hop that an http:// request
 * was redirected to; on libcurl a root that isn't a certificate is found
 * only at the first connection.
 *
 * A zeroed desc is the stack's own behaviour. See @ref crnet_tls "TLS".
 */
/* Frozen: held by value in the HTTP client and WebSocket connect descs; an addition to TLS is a struct chained onto those. It never changes; a new need gets a new type. */
typedef struct crnet_tls_desc {
	/** Which roots a certificate chain has to end at. */
	CRNET_TLS_TRUST trust;
	/** The roots for CRNET_TLS_TRUST_ROOTS and
	 * CRNET_TLS_TRUST_SYSTEM_AND_ROOTS, which need at least one. With the
	 * other two, anything but NULL and 0 is refused rather than ignored. A
	 * private CA's root goes here; so does a self-signed server certificate. */
	const crnet_tls_certificate *roots;
	/** How many certificates `roots` holds. */
	uint32_t root_count;
	/** true accepts a certificate issued for another host name than the
	 * URL's (a development certificate for "localhost" reached by address);
	 * the chain is still checked as `trust` says. CRNET_TLS_TRUST_ANY
	 * checks no name either way and is refused with false here, so that a
	 * desc never reads stricter than it is. */
	bool skip_host_name_check;
	/** With pin_count above 0, the server's own certificate - the first of
	 * its chain, not an issuer's - must have a public key matching one of
	 * these, or the connection is turned away. NULL and 0 for none. */
	const crnet_tls_pin *pins;
	/** How many pins `pins` holds. */
	uint32_t pin_count;
} crnet_tls_desc;

/** @} */

/**
 * @defgroup crnet_proxies Proxy
 * @ingroup crnet_stack
 * @brief What stands between this machine and the server. Known problems: on
 * Apple a TLS connection to `localhost` does not go through an explicit
 * proxy, so "every connection" below is not quite true for a server on the
 * same machine; an explicit proxy for https:// and wss:// has only been
 * tried on macOS; and what a proxy that wants a login looks like to the
 * caller (below) has not been observed on any stack.
 *
 * One crnet_proxy_desc says it for every connection of the object it is
 * given to, and it goes where a crnet_tls_desc goes for the same reason:
 * an HTTP client takes one when it is created (a connection the client is
 * holding open was made by one route and can't be given another), a
 * WebSocket with its connect desc. It covers plain and TLS connections
 * alike.
 *
 * A zeroed desc is the stack's own behaviour with nothing added: whatever
 * proxy the platform is set up to use, which may well be none.
 *
 * What "the platform is set up to use" means is the platform's, and not
 * the same thing on each:
 *
 * - Windows: the user's proxy settings, with automatic detection and
 *   configuration scripts where those are switched on.
 * - Apple: the proxies in the system's network settings, configuration
 *   scripts included.
 * - Linux: the http_proxy, https_proxy, all_proxy and no_proxy environment
 *   variables of this process, which is what libcurl reads. There is no
 *   system-wide setting to read, and a desktop's own (GNOME's, KDE's) is
 *   not looked at.
 *
 * What this doesn't do:
 *
 * - Log in to a proxy. Carrottus Net keeps no credentials and answers no
 *   authentication challenge, a proxy's included. One that wants a login
 *   shows as a 407 response to a plain HTTP transfer, and as a connection
 *   that fails (CR_RESULT_NET_CONNECT_FAILED, or the part's
 *   catch-all) to anything over TLS, where the proxy's refusal comes before
 *   there is a response to hand over.
 * - Name anything but an HTTP proxy. SOCKS, a proxy per scheme and a list
 *   of hosts to go around the proxy are not all to be had on every stack;
 *   a caller that needs them has them in the platform's own settings, and
 *   CRNET_PROXY_SYSTEM uses those.
 * @{
 */

/** @brief Which proxy, if any, an object's connections go through. */
typedef enum CRNET_PROXY {
	/** Whatever the platform is set up to use; see the list under
	 * @ref crnet_proxies "Proxy". */
	CRNET_PROXY_SYSTEM = 0,
	/** No proxy: every connection goes straight to the server, whatever the
	 * platform is set up to use. */
	CRNET_PROXY_NONE = 1,
	/** The one proxy the desc names in `url`, for every connection, and
	 * nothing of the platform's settings. */
	CRNET_PROXY_EXPLICIT = 2,
} CRNET_PROXY;

/**
 * @brief Which proxy stands between this machine and the server, for every
 * connection of the object the desc is given to. Known problem: with
 * CRNET_PROXY_EXPLICIT on Apple, a TLS connection to `localhost` goes
 * straight to the server and not through the proxy.
 *
 * A zeroed desc is the platform's own setting. See
 * @ref crnet_proxies "Proxy".
 */
/* Frozen: held by value in the HTTP client and WebSocket connect descs; an addition is a struct chained onto those. It never changes; a new need gets a new type. */
typedef struct crnet_proxy_desc {
	/** Where the proxy, if any, comes from. */
	CRNET_PROXY mode;
	/** For CRNET_PROXY_EXPLICIT, the proxy: "http://host:port", with the
	 * host a name or a numeric address, the port always given, and nothing
	 * after it - no path, no user name or password. It is an HTTP proxy
	 * whatever it is used to reach: an https:// or wss:// connection is
	 * tunnelled through it (CONNECT) and its TLS is still with the server,
	 * checked as the crnet_tls_desc says. With the other two modes
	 * anything but NULL is refused rather than ignored. */
	const char *url;
} crnet_proxy_desc;

/** @} */

/** @} */

/**
 * @defgroup crnet_http HTTP
 * @ingroup crnet_stack
 * @brief The HTTP client, for http:// and https://. Known problems: the rule
 * of one transfer at a time per client is enforced on libcurl and
 * NSURLSession but not on WinHTTP, which would carry several and doesn't
 * check; a server's answer that comes before the request body is all sent
 * may be lost (see crnet_http_transfer_send()); and several `Set-Cookie`
 * headers can't be read apart on any platform.
 *
 * Two objects. A crnet_http_client holds the stack's connections, so
 * that requests to one server reuse them. A crnet_http_transfer is one
 * request and its response, made on a client.
 *
 * A transfer moves its bodies the way a TCP socket moves bytes: the caller
 * sends the request body in pieces and receives the response body in
 * pieces, each call taking or giving what there is, and nothing is held in
 * between. A file goes up or comes down in a buffer's worth of memory
 * whatever its size, the caller knows how far along it is because it is
 * the one counting, and "how much is too much" is the caller's to decide by
 * no longer asking for more. A caller that wants a small body as one block
 * loops on recv into a buffer of its own; Carrottus Net won't do it for you.
 *
 * The order of use, each step once and in this order:
 *
 * | Call                                           | What it does                                                                               |
 * |------------------------------------------------|--------------------------------------------------------------------------------------------|
 * | crnet_http_transfer_create()                 | a transfer, on a client                                                                    |
 * | crnet_http_transfer_start()                  | the request line and headers                                                               |
 * | crnet_http_transfer_send()                   | the request body, until all of it is in (skipped when there is none, or none to send)      |
 * | crnet_http_transfer_receive_response()       | ends the request, waits for the status and headers                                         |
 * | crnet_http_transfer_get_status(), `_get_header`, ... | reads the status and headers                                                       |
 * | crnet_http_transfer_recv()                   | the response body, until it returns 0 bytes                                                |
 * | crnet_http_transfer_destroy()                | ends the transfer and frees it                                                             |
 *
 * A start that returns CR_RESULT_NET_TIMED_OUT has started the transfer:
 * the request is under way, and the next call in this order - send, or
 * receive_response - carries it on before its own part.
 *
 * A call made out of that order (other than abort, get_error and destroy)
 * returns CR_RESULT_NET_INVALID_HTTP_TRANSFER. A transfer is good for
 * one request: after any call on it fails, other than with
 * CR_RESULT_NET_TIMED_OUT, only destroy is left.
 *
 * Time limits belong to each call, as everywhere in this header. There is
 * none for the exchange as a whole; a caller that has one keeps it, and
 * passes each call what is left of it.
 *
 * A transfer and its client:
 *
 * - One transfer at a time per client, from its start until it is
 *   destroyed. Transfers that run side by side want a client each.
 * - Destroying a transfer whose response body was received to its end
 *   hands its connection back to the client for the next one. Destroying
 *   it any earlier drops the connection, which is also how a response
 *   nobody wants the rest of is stopped.
 * - A transfer is destroyed before its client.
 *
 * A transfer is one caller at a time; it has no sending side and receiving
 * side to share out, since a request is sent before its response is read.
 * crnet_http_transfer_abort() is the exception, as every `_abort` is.
 * @{
 */

/** @brief Opaque handle to an HTTP client: the stack's connections, kept for reuse. */
typedef struct crnet_http_client_t *crnet_http_client;

/** @brief What every connection of an HTTP client is made with. */
typedef struct crnet_http_client_desc {
	/** `type` is CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC; `next` is NULL (nothing is
	 * chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** For every https:// connection the client makes, a redirect's
	 * included. See @ref crnet_tls "TLS". */
	crnet_tls_desc tls;
	/** For every connection the client makes, a redirect's included. See
	 * @ref crnet_proxies "Proxy". */
	crnet_proxy_desc proxy;
} crnet_http_client_desc;

/**
 * @brief Makes an HTTP client. Known problem: on libcurl a root in
 * `desc->tls` that isn't a certificate is not refused here but at the first
 * connection, as CR_RESULT_NET_TLS_FAILED.
 *
 * Opens no connection by itself; the first transfer does.
 *
 * @param[in]  instance   The instance the client is made with.
 * @param[in]  desc       The TLS and proxy descs. It and everything it points
 *                        at only have to last until this returns.
 * @param[out] out_client Receives the new client.
 *
 * @retval CR_SUCCESS The client was made.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_client` is NULL.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `desc->tls` or `desc->proxy`
 *         contradicts itself, or a root isn't a certificate.
 * @retval CR_RESULT_OUT_OF_MEMORY The client could not be allocated.
 * @retval CR_RESULT_NET_UNSUPPORTED libcurl: its TLS library can't do
 *         what `desc->tls` asks; or the libcurl found at run time looks host
 *         names up synchronously (no CURL_VERSION_ASYNCHDNS), so a call with
 *         CRNET_NO_WAIT could block in the lookup. Nothing is recorded.
 * @return Another CR_RESULT where the stack refused to make its own session
 *         object.
 */
CR_RESULT crnet_http_client_create(
    crnet_instance instance,
    const crnet_http_client_desc *desc,
    crnet_http_client *out_client);

/**
 * @brief Drops the connections the client was holding open and frees it.
 *
 * Every transfer made on the client is destroyed first.
 *
 * @param[in] client The client to destroy. NULL is taken and nothing is done
 *                   with it.
 */
void crnet_http_client_destroy(crnet_http_client client);

/** @brief Opaque handle to one request and its response. */
typedef struct crnet_http_transfer_t *crnet_http_transfer;

/**
 * @brief What a request has for a body.
 *
 * It is said outright, in crnet_http_transfer_desc::body, and never
 * worked out from the method: which methods carry a body is the caller's
 * knowledge of the protocol it is speaking, not Carrottus Net's.
 *
 * The first two differ in nothing but one header. Neither sends a byte of
 * body; an empty body is announced (Content-Length: 0) and no body isn't,
 * and there are servers that turn a POST away for the lack of it.
 */
typedef enum CRNET_HTTP_BODY {
	/** No body. Carrottus Net announces none and crnet_http_transfer_send() is
	 * not called. What a GET, a HEAD or a DELETE usually is - and what a
	 * desc that leaves `body` at 0 asks for. A stack may still announce an empty body of its
	 * own accord for a method it expects one from; that is among the headers
	 * the stack adds (@ref crnet_stack "HTTP and WebSocket"), and not
	 * everywhere something that can be stopped. Known problem: NSURLSession
	 * adds a Content-Length to a POST given this; which other stacks do has
	 * not been found out. */
	CRNET_HTTP_BODY_NONE = 0,
	/** A body of exactly `body_size` bytes, announced before the first of
	 * them goes (Content-Length). `body_size` may be 0: a body that is
	 * there and empty - a POST with nothing to say - which is announced as
	 * 0 bytes on every stack, and for which send is not called either. */
	CRNET_HTTP_BODY_SIZED = 1,
	/** A body whose length isn't known when the request starts, and which
	 * ends when the caller says it has
	 * (crnet_http_transfer_receive_response()). It goes out in whatever
	 * form the stack and the HTTP version give a body of unannounced length
	 * (chunked, over HTTP/1.1) - a form some servers and proxies don't
	 * take, so a size that is known is better given. */
	CRNET_HTTP_BODY_UNSIZED = 2,
} CRNET_HTTP_BODY;

/** @brief One request, as crnet_http_transfer_start() sends it. */
typedef struct crnet_http_transfer_desc {
	/** `type` is CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC; `next` is NULL (nothing is
	 * chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** The method, sent byte for byte as given. HTTP's method names are
	 * case-sensitive, so "get" is not GET and a server may refuse it;
	 * Carrottus Net neither corrects the case nor compares without it.
	 *
	 * One method means something to Carrottus Net itself: "HEAD", spelled exactly
	 * so. Its response describes a body and has none, which every HTTP
	 * client has to know in order not to wait for one. No other method is
	 * looked at. */
	const char *method;
	const char *url; /**< Absolute, already percent-encoded. */
	/** The request's headers. Content-Length and Transfer-Encoding are not
	 * the caller's to put here: they follow from `body` and `body_size`,
	 * and either one among these headers is refused. Accept-Encoding may be
	 * here with the one value `identity` and no other; see
	 * @ref crnet_stack "HTTP and WebSocket" for what it does. */
	const crnet_header *headers;
	/** How many headers `headers` holds. */
	uint32_t header_count;
	/** What the request has for a body. See CRNET_HTTP_BODY. */
	CRNET_HTTP_BODY body;
	/** For CRNET_HTTP_BODY_SIZED, how many bytes
	 * crnet_http_transfer_send() will be given, exactly. There is no
	 * upper limit on any platform. With the other two it has nothing to say
	 * and anything but 0 is refused, rather than ignored. */
	uint64_t body_size;
	/** false hands a 3xx back as the response; true lets the stack follow
	 * it under its own rules (how many hops, which headers carry over).
	 * Following one can mean sending the request again, body and all, and a
	 * body that was passed through is no longer here to send: true is
	 * refused unless `body` is CRNET_HTTP_BODY_NONE, and a caller with a
	 * body - an empty one included - follows the redirect itself, with a
	 * new transfer. */
	bool follow_redirects;
	/** The longest crnet_http_transfer_start() may take. See
	 * @ref crnet_time "Time limits". */
	uint32_t timeout_ms;
} crnet_http_transfer_desc;

/**
 * @brief Makes a transfer on `client` with no request yet.
 *
 * It exists before start so that crnet_http_transfer_abort() has
 * something to act on while start is still waiting.
 *
 * @param[in]  client       The client the transfer is made on. It outlasts
 *                          the transfer.
 * @param[out] out_transfer Receives the new transfer.
 *
 * @retval CR_SUCCESS The transfer was made.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_transfer` is NULL.
 * @retval CR_RESULT_NET_INVALID_HTTP_CLIENT `client` is NULL.
 * @retval CR_RESULT_OUT_OF_MEMORY The transfer could not be allocated.
 */
CR_RESULT crnet_http_transfer_create(
    crnet_http_client client,
    crnet_http_transfer *out_transfer);

/**
 * @brief Starts the request: hands the request line and the headers to the
 * stack, on a connection the client already had open to that server or a
 * new one, and waits up to `desc->timeout_ms` for them to be out. Known
 * problem: starting a second transfer on a client that already has one in
 * progress is refused with CR_RESULT_NET_INVALID_HTTP_TRANSFER on
 * libcurl and NSURLSession, and is not checked on WinHTTP.
 *
 * The step (@ref crnet_time "Time limits", "Steps"): getting the request
 * out - looking the host up, connecting, TLS, sending the line and headers
 * - which the stack carries on by itself (WinHTTP, NSURLSession) or in
 * later calls (libcurl). Started whichever this returns, CR_SUCCESS or
 * CR_RESULT_NET_TIMED_OUT: the transfer moves on to the next call in the
 * order of use, which carries a start that isn't finished on before its
 * own part and reports what it failed with. NSURLSession says nothing of
 * connecting, so there this returns CR_SUCCESS at once whatever the limit.
 *
 * What is kept for the step: nothing new. The URL, method and headers are
 * copied by the stack at this call, and the transfer already keeps its
 * handles and events.
 *
 * How much of this has happened by the time start returns is the stack's:
 * some connect and send the headers here, some put it off until there is
 * a body to send or a response to wait for. So a server that can't be
 * reached, or a certificate that is turned away, is reported by start
 * where the stack connects this early and otherwise by the first later
 * call that needs the connection - with the same result either way (the
 * network failures listed under @ref crnet_stack "HTTP and WebSocket").
 * NSURLSession never reports a connect failure from start; it comes from
 * crnet_http_transfer_receive_response().
 *
 * @param[in] transfer The transfer to start. Once per transfer.
 * @param[in] desc     The request. It and everything it points at only have
 *                     to last until this returns.
 *
 * @retval CR_SUCCESS The request is out, or (NSURLSession) handed to the
 *         stack.
 * @retval CR_RESULT_NET_INVALID_HTTP_TRANSFER `transfer` is NULL, it
 *         has already been started (a start that returned
 *         CR_RESULT_NET_TIMED_OUT included), or (libcurl, NSURLSession) its
 *         client has another transfer in progress.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NET_ABORTED crnet_http_transfer_abort() was
 *         called.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS A field is missing, a header is
 *         one the caller may not set, `body_size` or `follow_redirects`
 *         doesn't go with `body`, or text isn't UTF-8.
 * @retval CR_RESULT_NET_INVALID_URL `desc->url` is not a URL, or not
 *         http:// or https://.
 * @retval CR_RESULT_NET_TIMED_OUT The transfer has started and the
 *         request isn't out yet; the next call in the order of use carries
 *         it on.
 * @retval CR_RESULT_OUT_OF_MEMORY An allocation failed.
 * @return Otherwise one of the network failures under
 *         @ref crnet_stack "HTTP and WebSocket", or
 *         CR_RESULT_NET_HTTP_FAILED.
 */
CR_RESULT crnet_http_transfer_start(
    crnet_http_transfer transfer,
    const crnet_http_transfer_desc *desc);

/**
 * @brief Waits for the stack to take request body, then hands it as much of
 * `data` as it takes in one go. Known problem: when the server has already
 * answered before the body is all sent, libcurl and NSURLSession let the
 * remaining sends through as if sent, so the caller reaches the response;
 * WinHTTP reports whatever its write fails with, and the response is lost.
 *
 * Bytes count as sent once the stack has them, which is before they have
 * reached the server; a failure to deliver them shows up in a later call.
 *
 * With CRNET_HTTP_BODY_SIZED, the bytes past `body_size` are not taken: a
 * send that would go over is cut to what is left.
 *
 * A server may answer before the body is all sent - a refusal, usually -
 * and stop reading it. What the caller gets then is not the same on every
 * stack, and is not to be relied on: the rest of the sends may be taken as
 * sent and crnet_http_transfer_receive_response() hand over the answer
 * (libcurl, NSURLSession), or a send may fail and the answer is lost
 * (WinHTTP, which reads nothing of the response until the body is
 * finished). An `Expect: 100-continue` header from the caller doesn't change
 * that on WinHTTP, which sends the body without waiting for the 100. A
 * caller that needs to hear a refusal before sending a large body asks the
 * server first, in a request of its own.
 *
 * A start that returned CR_RESULT_NET_TIMED_OUT is carried on first, out of
 * this call's limit: until the request is out, no body is taken and this
 * returns CR_RESULT_NET_TIMED_OUT (nothing taken; send again). A failure of
 * the start - the network failures under @ref crnet_stack "HTTP and
 * WebSocket", CR_RESULT_NET_HTTP_FAILED - comes from here, and ends the
 * transfer.
 *
 * @param[in]  transfer   The transfer, started and with a body to send.
 * @param[in]  data       The bytes to send.
 * @param[in]  size       How many bytes `data` holds.
 * @param[in]  timeout_ms The longest to wait for the stack to take any
 *                        (@ref crnet_time "Time limits").
 * @param[out] out_sent   Receives how many bytes were taken. May be less
 *                        than `size`, as with crnet_tcp_socket_send(), and
 *                        is never 0 on success.
 *
 * @retval CR_SUCCESS `out_sent` bytes were taken.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_sent` is NULL.
 * @retval CR_RESULT_NET_INVALID_HTTP_TRANSFER `transfer` is NULL; the
 *         call is out of the order of use; the request has
 *         CRNET_HTTP_BODY_NONE; or, with CRNET_HTTP_BODY_SIZED, nothing
 *         of `body_size` is left - which with a `body_size` of 0 is any
 *         send at all.
 * @retval CR_RESULT_NET_ABORTED crnet_http_transfer_abort() was
 *         called.
 * @retval CR_RESULT_ZERO_SIZE `data` is NULL or `size` is 0.
 * @retval CR_RESULT_NET_TIMED_OUT None of it was taken (or the request
 *         isn't out yet); the transfer is as it was and send may be tried
 *         again.
 * @return Otherwise CR_RESULT_NET_SEND_FAILED, CR_RESULT_NET_HTTP_FAILED
 *         or one of the network failures under @ref crnet_stack "HTTP and
 *         WebSocket".
 */
CR_RESULT crnet_http_transfer_send(
    crnet_http_transfer transfer,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_sent);

/**
 * @brief Says the request is complete and waits for the response's status
 * and headers.
 *
 * For a request with no body, or an empty one, it follows start directly.
 * With CRNET_HTTP_BODY_UNSIZED this call is what ends the body.
 *
 * A start that returned CR_RESULT_NET_TIMED_OUT is carried on first, out
 * of this call's limit, and its failure comes from here.
 *
 * @param[in] transfer   The transfer, started and with its body sent.
 * @param[in] timeout_ms The longest to wait for the response
 *                       (@ref crnet_time "Time limits").
 *
 * @retval CR_SUCCESS A response has begun, whatever its status - a 404 or
 *         a 500 is a success here and the caller reads the status.
 * @retval CR_RESULT_NET_INVALID_HTTP_TRANSFER `transfer` is NULL; the
 *         call is out of the order of use; or, with
 *         CRNET_HTTP_BODY_SIZED, not every one of the `body_size` bytes
 *         has been sent. A request that stops short of the length it
 *         announced can't be finished, and in that case the transfer is not
 *         ended, so the rest can still be sent.
 * @retval CR_RESULT_NET_ABORTED crnet_http_transfer_abort() was
 *         called.
 * @retval CR_RESULT_NET_TIMED_OUT The response hasn't come yet; the
 *         transfer is still waiting for it and this may be called again
 *         (the request's end was said the first time and isn't said twice).
 * @return Otherwise CR_RESULT_NET_HTTP_FAILED or one of the network
 *         failures under @ref crnet_stack "HTTP and WebSocket"; there
 *         will be no response.
 */
CR_RESULT crnet_http_transfer_receive_response(
    crnet_http_transfer transfer,
    uint32_t timeout_ms);

/**
 * @name Response status and headers
 *
 * All of it is there once crnet_http_transfer_receive_response() has
 * succeeded and until the transfer is destroyed; before that the status is
 * 0, there are no headers, and a header asked for by name is NULL.
 *
 * Each header name is there once. Where the server sent a name on several
 * lines, their values are one value here, joined with ", " in the order
 * they were sent - which is what HTTP itself says several lines of one
 * name mean, and the only form NSURLSession gives a response's headers in,
 * so it is the form on every stack.
 *
 * Set-Cookie is the one header that reading is wrong for: each of its
 * lines is a cookie of its own, and a cookie can hold a comma (the date in
 * its Expires), so the joined value can't be told apart again for sure.
 * Carrottus Net carries no cookies in either direction
 * (@ref crnet_stack "HTTP and WebSocket"), and a response's Set-Cookie is
 * here for what it is worth - whole when the server set one cookie, not to
 * be relied on when it set several.
 * @{
 */

/**
 * @brief Returns the response's status code.
 *
 * @param[in] transfer The transfer to ask.
 *
 * @return The status code as the server sent it (200, 404, ...); after a
 *         followed redirect, the final response's.
 * @retval 0 `transfer` is NULL, or no response has been received yet.
 */
uint32_t crnet_http_transfer_get_status(crnet_http_transfer transfer);

/**
 * @brief Returns the value of one response header, by name. Known problem:
 * for `Set-Cookie` sent on several lines the joined value can't be split
 * back into its cookies for sure, on any platform.
 *
 * @param[in] transfer The transfer to ask.
 * @param[in] name     The header's name, null-terminated. Names compare
 *                     without case.
 *
 * @return The header's value, a null-terminated string that belongs to the
 *         transfer and lasts until it is destroyed.
 * @retval NULL The response has no header of that name, none has been
 *         received yet, or `transfer` or `name` is NULL.
 */
const char *crnet_http_transfer_get_header(
    crnet_http_transfer transfer,
    const char *name);

/**
 * @brief Returns how many headers the response has, each name counted once.
 *
 * After a followed redirect these are the final response's headers only.
 *
 * @param[in] transfer The transfer to ask.
 *
 * @return The number of headers, for crnet_http_transfer_get_header_at().
 * @retval 0 `transfer` is NULL, or no response has been received yet.
 */
uint32_t crnet_http_transfer_get_header_count(crnet_http_transfer transfer);

/**
 * @brief Reads one response header by position.
 *
 * The headers run from 0 to count - 1, in no promised order, each name
 * once.
 *
 * @param[in]  transfer  The transfer to ask.
 * @param[in]  index     Which header, below
 *                       crnet_http_transfer_get_header_count().
 * @param[out] out_name  Receives the header's name.
 * @param[out] out_value Receives the header's value. Both strings are
 *                       null-terminated, belong to the transfer and last
 *                       until it is destroyed.
 *
 * @retval CR_SUCCESS Both were written.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_name` or `out_value` is
 *         NULL.
 * @retval CR_RESULT_INDEX_OUT_OF_RANGE `index` is past the end, or
 *         `transfer` is NULL.
 */
CR_RESULT crnet_http_transfer_get_header_at(
    crnet_http_transfer transfer,
    uint32_t index,
    const char **out_name,
    const char **out_value);

/** @} */

/**
 * @brief Waits for response body and copies up to `size` bytes of it. Known
 * problem: where a server ignores `Accept-Encoding: identity` and compresses
 * anyway, libcurl and WinHTTP hand the body over still compressed and
 * NSURLSession decodes it.
 *
 * The bytes are the body as the caller is meant to read it: decoded where
 * the server compressed it, so they need not add up to the response's
 * Content-Length header. A request that was sent with
 * `Accept-Encoding: identity` gets them as the server holds them, and then
 * they do.
 *
 * Nothing is read from the server faster than this is called: a caller
 * that stops receiving stops the download, and holds its connection, for
 * as long as it stays stopped.
 *
 * @param[in]  transfer     The transfer, with its response received.
 * @param[out] data         Receives the bytes.
 * @param[in]  size         How many bytes `data` has room for.
 * @param[in]  timeout_ms   The longest to wait for body
 *                          (@ref crnet_time "Time limits").
 * @param[out] out_received Receives how many bytes were copied. May be less
 *                          than `size`. 0 with CR_SUCCESS means the body
 *                          has ended - at once, for a response that has none
 *                          (a HEAD, a 204) - and every call after that
 *                          returns the same.
 *
 * @retval CR_SUCCESS `out_received` bytes were copied, or the body has
 *         ended (0 bytes).
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_received` is NULL.
 * @retval CR_RESULT_NET_INVALID_HTTP_TRANSFER `transfer` is NULL, or no
 *         response has been received yet.
 * @retval CR_RESULT_NET_ABORTED crnet_http_transfer_abort() was
 *         called.
 * @retval CR_RESULT_ZERO_SIZE `data` is NULL or `size` is 0, on a body that
 *         hasn't ended.
 * @retval CR_RESULT_NET_TIMED_OUT Nothing arrived in time; the transfer
 *         is as it was.
 * @return Otherwise CR_RESULT_NET_RECV_FAILED or one of the network
 *         failures under @ref crnet_stack "HTTP and WebSocket".
 */
CR_RESULT crnet_http_transfer_recv(
    crnet_http_transfer transfer,
    void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_received);

/**
 * @brief Ends a wait on the transfer, and every call after it.
 *
 * See @ref crnet_aborting "Aborting". This is how one request is
 * cancelled: its client, and the connections the client holds for other
 * transfers, are untouched. A request that was aborted may or may not have
 * reached the server.
 *
 * Thread: may overlap any call on the transfer but its destroy.
 *
 * @param[in] transfer The transfer to abort.
 */
void crnet_http_transfer_abort(crnet_http_transfer transfer);

/**
 * @brief Reads what the platform reported for the most recent call on this
 * transfer that failed.
 *
 * See @ref crnet_error_detail "Error detail".
 *
 * @param[in]  transfer  The transfer to ask. NULL gives an empty record.
 * @param[out] out_error Receives the record.
 */
void crnet_http_transfer_get_error(
    crnet_http_transfer transfer,
    crnet_error *out_error);

/**
 * @brief Ends the transfer wherever it has got to and frees it, along with
 * every string its getters returned.
 *
 * What becomes of its connection is under "A transfer and its client" in
 * @ref crnet_http "HTTP".
 *
 * @param[in] transfer The transfer to destroy. NULL is taken and nothing is
 *                     done with it.
 */
void crnet_http_transfer_destroy(crnet_http_transfer transfer);

/** @} */

#if CRNET_EXT_WEBSOCKET

/**
 * @defgroup crnet_ws WebSocket client
 * @ingroup crnet_stack
 * @brief The WebSocket client, for ws:// and wss://. Known problems: the
 * negotiated subprotocol, the opening response's headers and the peer's
 * close reason can't be read, and a ping can't be sent; bad arguments to
 * send and close return CR_RESULT_NET_SEND_FAILED where an
 * invalid-argument result is meant; a text message handed over in parts can
 * be split in the middle of a UTF-8 character; on libcurl a redirect of the
 * opening request is never followed (see
 * crnet_websocket_connect_desc::follow_redirects); and the result codes
 * say `WS_` where the functions say `websocket`.
 *
 * The order of use: create, connect once (continued with
 * crnet_websocket_connect_continue() until it has an outcome), then send
 * and recv for as long as the connection lasts, then destroy. close is the polite ending and
 * optional; destroy alone just drops the connection. A socket is good for
 * one connection: after connect fails, or the connection ends for any
 * reason, only destroy is left and a new connection is a new socket.
 *
 * Calls made on a socket that isn't connected (before connect has
 * succeeded, while it is under way, or after it failed), other than abort,
 * destroy, get_error and crnet_websocket_connect_continue(), return
 * CR_RESULT_NET_INVALID_WEBSOCKET.
 *
 * crnet_websocket_close() is not the counterpart of destroy: it is the
 * protocol's close frame, and the end of the connection as far as this side
 * is concerned, on a socket that stays the caller's to destroy.
 *
 * Thread: on one socket, a receiving call (crnet_websocket_recv()) may
 * overlap a sending call (crnet_websocket_send(), crnet_websocket_flush(),
 * crnet_websocket_close()), and each may overlap crnet_websocket_abort().
 * Two receiving calls at once, two sending calls at once, or any other
 * call on the socket during either, are the caller's to prevent.
 * crnet_websocket_connect() and crnet_websocket_connect_continue() are
 * calls on the whole socket: only abort may overlap them.
 * @{
 */

/**
 * @brief Opaque handle to one WebSocket connection.
 *
 * Separate from crnet_http_client: it shares nothing with one and needs
 * none.
 */
typedef struct crnet_websocket_t *crnet_websocket;

/**
 * @brief Which kind of message is sent or was received. Known problem: it is
 * a type enum without `_TYPE` in its name.
 */
typedef enum CRNET_WEBSOCKET_MESSAGE {
	CRNET_WEBSOCKET_MESSAGE_TEXT   = 0, /**< A text message, UTF-8. */
	CRNET_WEBSOCKET_MESSAGE_BINARY = 1, /**< A binary message. */
} CRNET_WEBSOCKET_MESSAGE;

/** @brief Where crnet_websocket_connect() connects to, and how. */
typedef struct crnet_websocket_connect_desc {
	/** `type` is CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC; `next` is NULL (nothing is
	 * chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	const char *url; /**< Absolute, already percent-encoded. */
	/** Sent with the opening request - Authorization, Sec-WebSocket-Protocol
	 * and the like. */
	const crnet_header *headers;
	/** How many headers `headers` holds. */
	uint32_t header_count;
	/** The longest opening the connection may take. See
	 * @ref crnet_time "Time limits". */
	uint32_t timeout_ms;
	/** Largest message to accept, in bytes; a bigger one ends the connection
	 * (crnet_websocket_recv() returns
	 * CR_RESULT_NET_WS_MESSAGE_TOO_BIG). 0 is refused.
	 * NSURLSession holds a whole message in memory before handing any of it
	 * over, so this is also what one message can cost there. */
	uint64_t max_message_size;
	/** What becomes of an opening request the server answers with a
	 * redirect (a 3xx) instead of accepting it.
	 *
	 * false hands the redirect back: crnet_websocket_connect() returns
	 * CR_RESULT_NET_WS_UPGRADE_REFUSED with the 3xx in its
	 * `out_http_status`, and the connection is to nowhere but `url` or not
	 * made at all. Where the server pointed is not to be had - the opening
	 * response's headers can't be read here - so a caller that would follow
	 * a redirect has to let the stack do it.
	 *
	 * true lets the stack follow it under its own rules (how many hops,
	 * which headers carry over, whether it will go from wss:// to ws://),
	 * as crnet_http_transfer_desc::follow_redirects does. The socket is
	 * then connected to wherever that led, and Carrottus Net doesn't say where:
	 * the headers above, credentials among them, may have gone to a host
	 * the caller never named, and `tls` and `proxy` apply to every hop.
	 *
	 * Known problem: libcurl refuses any answer to the opening request
	 * other than 101 before its redirect handling is reached, and keeps no
	 * record of where a 3xx pointed. On Linux, true therefore behaves as
	 * false does: a redirect is handed back as
	 * CR_RESULT_NET_WS_UPGRADE_REFUSED with the 3xx. */
	bool follow_redirects;
	/** For a wss:// connection. See @ref crnet_tls "TLS". */
	crnet_tls_desc tls;
	/** See @ref crnet_proxies "Proxy". */
	crnet_proxy_desc proxy;
} crnet_websocket_connect_desc;

/**
 * @brief Makes a socket with no connection. Known problem: on Linux, whether
 * WebSocket is there at all is only found out here, at run time
 * (CR_RESULT_NET_UNSUPPORTED from a libcurl older than 8.11 or built
 * without it), although CRNET_EXT_WEBSOCKET is 1.
 *
 * It exists before connect so that crnet_websocket_abort() has something
 * to act on while connect is still waiting.
 *
 * @param[in]  instance   The instance the socket is made with.
 * @param[out] out_socket Receives the new socket.
 *
 * @retval CR_SUCCESS The socket was made.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_socket` is NULL.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NET_UNSUPPORTED This build's stack has no WebSocket
 *         (see the table under @ref crnet "Carrottus Net"); or, libcurl,
 *         the libcurl found at run time looks host names up synchronously
 *         (no CURL_VERSION_ASYNCHDNS), so a call with CRNET_NO_WAIT could
 *         block in the lookup. Nothing is recorded.
 * @retval CR_RESULT_OUT_OF_MEMORY The socket could not be allocated.
 */
CR_RESULT crnet_websocket_create(
    crnet_instance instance,
    crnet_websocket *out_socket);

/**
 * @brief Begins opening the connection, and waits up to `desc->timeout_ms`
 * for the server to accept it. Known problem: the negotiated subprotocol
 * and the opening response's headers can't be read afterwards, so neither
 * can the target of a redirect that was handed back.
 *
 * The step (@ref crnet_time "Time limits", "Steps"): the opening request
 * and its answer - lookup, connect, TLS, the upgrade request, the 101 -
 * which the stack carries on by itself (WinHTTP, NSURLSession) or in
 * later calls (libcurl: the socket's multi handle, driven by each
 * continue). When the limit runs out first this returns
 * CR_RESULT_NET_TIMED_OUT and the opening goes on;
 * crnet_websocket_connect_continue() carries it on and says how it ended.
 * Until then the socket counts as not connected: send, recv, flush and
 * close return CR_RESULT_NET_INVALID_WEBSOCKET, and a second connect does
 * too.
 *
 * @param[in]  socket          The socket to connect. Once per socket.
 * @param[in]  desc            The URL, headers, limits and TLS and proxy
 *                             descs. It and everything it points at only
 *                             have to last until this returns.
 * @param[out] out_http_status Receives the server's status for the opening
 *                             request: 101 on success; with
 *                             CR_RESULT_NET_WS_UPGRADE_REFUSED whatever
 *                             it answered instead, a 401 or, for a desc that
 *                             doesn't follow redirects, a 3xx; or 0 where no
 *                             answer arrived, CR_RESULT_NET_TIMED_OUT
 *                             included. After a followed redirect it is
 *                             the last server's. May be NULL.
 *
 * @retval CR_SUCCESS The socket is connected.
 * @retval CR_RESULT_NET_INVALID_WEBSOCKET `socket` is NULL, or connect
 *         has already been called on it.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `desc->url` is NULL,
 *         `desc->max_message_size` is 0, `desc->headers` is NULL with a
 *         `header_count` above 0, a header is one the caller may not set,
 *         or `desc->tls` or `desc->proxy` contradicts itself.
 * @retval CR_RESULT_NET_INVALID_URL `desc->url` is not a URL, or not
 *         ws:// or wss://.
 * @retval CR_RESULT_NET_ABORTED crnet_websocket_abort() was called.
 * @retval CR_RESULT_NET_TIMED_OUT Not open yet; the opening goes on.
 *         Carry it on with crnet_websocket_connect_continue().
 * @retval CR_RESULT_NET_WS_UPGRADE_REFUSED The server answered, but not
 *         with 101; `out_http_status` has what it answered.
 * @retval CR_RESULT_NET_UNSUPPORTED libcurl: its TLS library can't do
 *         what `desc->tls` asks. Found here, before the opening is begun,
 *         so never from crnet_websocket_connect_continue().
 * @retval CR_RESULT_OUT_OF_MEMORY An allocation failed.
 * @return Otherwise one of the network failures under
 *         @ref crnet_stack "HTTP and WebSocket".
 */
CR_RESULT crnet_websocket_connect(
    crnet_websocket socket,
    const crnet_websocket_connect_desc *desc,
    uint32_t *out_http_status);

/**
 * @brief Carries on an opening that returned CR_RESULT_NET_TIMED_OUT,
 * waiting up to `timeout_ms` for its outcome.
 *
 * It takes the time limit and nothing else - the desc was taken whole by
 * connect - and returns exactly what connect would have.
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
 *                             server answered instead; 0 where no answer
 *                             has arrived, CR_RESULT_NET_TIMED_OUT
 *                             included. May be NULL.
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
 * @brief Takes one whole message to send, or none of it. Known problem: a
 * bad argument - `data` NULL with a `size` above 0, a size the stack can't
 * carry, text that isn't UTF-8 - returns CR_RESULT_NET_SEND_FAILED, the
 * same as a broken connection, and not an invalid-argument result.
 *
 * One message is in flight at a time: taken by a send and not yet all
 * handed to the connection. This call first carries on the one before it,
 * out of its limit. If that one is still in flight when the limit runs
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
 * record), and the connection is over, as a failed send's is.
 *
 * @warning A message taken and not yet gone is dropped by
 * crnet_websocket_destroy() and crnet_websocket_abort(). A caller that
 * needs it delivered before it lets go flushes first.
 *
 * Where the copy is: WinHTTP and libcurl - a buffer of the socket's own,
 * through the instance's allocator, kept until destroy (freed, on WinHTTP,
 * only after WinHTTP has reported the handle closing); NSURLSession - the
 * NSData of its message, which copies the bytes with the system's
 * allocator and goes with the message once sent, so there the socket
 * keeps nothing and each send allocates the system's memory, not the
 * instance's. On libcurl the buffer holds the part curl_ws_send hasn't
 * consumed, which libcurl requires be offered again.
 *
 * How the buffer is used, the same rules on WinHTTP and libcurl:
 * - It is sized when a message is taken that doesn't fit: never at create,
 *   and never for a socket that sends nothing. It grows to exactly the
 *   message's size, with no growth factor. A message is only taken when
 *   none is in flight, so the old block is freed before the new one is
 *   allocated: the socket never holds two.
 * - WinHTTP copies the whole message, since WinHTTP reads the buffer until
 *   it reports the send complete. libcurl first offers the caller's own
 *   bytes and copies only what wasn't consumed, so on a connection with
 *   room nothing is copied; but the buffer is grown to the whole message
 *   before the first byte is offered, so that an allocation that fails
 *   leaves nothing on the wire and the message untaken.
 * - Once a message of N bytes has been sent, the socket holds N bytes until
 *   it is destroyed: a steady stream allocates nothing, and that is a cost
 *   the caller carries. A caller that sends a rare large message and wants
 *   the memory back sends it on a socket of its own, or destroys the
 *   socket after it.
 *
 * The in-flight state and the failure of the message in flight are written
 * by the stack's completion under the socket's lock, which then notifies
 * the socket's wait set (ROOM); send, flush and close read them under the
 * same lock, so a completion that lands during one of them is seen either
 * before or after it, never half. An abort during a send or flush that is
 * waiting ends the wait, the message in flight is dropped with the
 * connection, and the call returns CR_RESULT_NET_ABORTED.
 *
 * Thread: may overlap crnet_websocket_recv() and
 * crnet_websocket_abort() on the same socket.
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
 * @retval CR_RESULT_NET_TIMED_OUT The message before is still in flight;
 *         nothing of this one was taken, and the connection is as it was.
 * @retval CR_RESULT_OUT_OF_MEMORY The socket's buffer couldn't grow to the
 *         message; nothing was taken, and the connection is as it was. The
 *         instance's allocator refused, not the platform: nothing is
 *         recorded and no message is sent.
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
 * @brief Carries the message in flight on, and waits up to `timeout_ms`
 * for it to have gone to the connection.
 *
 * libcurl moves a part-sent message only when it is called to, so a socket
 * whose last message is still in flight needs a call that moves it without
 * bringing a new one; and a caller needs to hear that a message it sent
 * has gone, or failed, without sending another. On WinHTTP and
 * NSURLSession the message goes on by itself and this only reads where it
 * has got to.
 *
 * "Gone" means handed to the connection, not acknowledged by the peer. A
 * caller that wants to know a message has gone before it goes on sends,
 * then flushes with a limit.
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
 * @brief Waits for message data and copies up to `size` bytes of it. Known
 * problem: a text message handed over in parts can be split in the middle
 * of a UTF-8 character, so a part is not always valid UTF-8 by itself.
 *
 * A message can arrive across several calls, as a short recv() does:
 * `out_message_complete` is false while more of the same message follows -
 * because `size` was too small for the rest, or because the stack hands it
 * over in parts - and true with its last bytes.
 *
 * Pings are answered by the stack and never show up here. After any failure
 * other than CR_RESULT_NET_TIMED_OUT, only destroy is left.
 *
 * Thread: may overlap crnet_websocket_send(),
 * crnet_websocket_close() and crnet_websocket_abort() on the same
 * socket.
 *
 * @param[in]  socket               The connected socket to receive on.
 * @param[out] data                 Receives the bytes.
 * @param[in]  size                 How many bytes `data` has room for.
 * @param[in]  timeout_ms           The longest to wait for data
 *                                  (@ref crnet_time "Time limits").
 * @param[out] out_received         Receives how many bytes were copied. An
 *                                  empty message is 0 bytes with
 *                                  `out_message_complete` true.
 * @param[out] out_type             Receives whether the message is text or
 *                                  binary.
 * @param[out] out_message_complete Receives true with a message's last
 *                                  bytes, false while more of it follows.
 *
 * @retval CR_SUCCESS `out_received` bytes of a message were copied.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_received`, `out_type` or
 *         `out_message_complete` is NULL.
 * @retval CR_RESULT_NET_INVALID_WEBSOCKET `socket` is NULL or isn't
 *         connected.
 * @retval CR_RESULT_ZERO_SIZE `data` is NULL or `size` is 0.
 * @retval CR_RESULT_NET_ABORTED crnet_websocket_abort() was called.
 * @retval CR_RESULT_NET_TIMED_OUT Nothing arrived in time; the
 *         connection is as it was.
 * @retval CR_RESULT_NET_WS_CLOSED A close frame ended the connection:
 *         the peer's (see crnet_websocket_get_close_code()), after
 *         everything the peer sent ahead of it has been handed over, or
 *         this side's, from the moment crnet_websocket_close() was
 *         called.
 * @retval CR_RESULT_NET_WS_MESSAGE_TOO_BIG A message was over the
 *         connect desc's `max_message_size`; the connection is over.
 * @return Otherwise CR_RESULT_NET_RECV_FAILED or another network
 *         failure: the connection broke.
 */
CR_RESULT crnet_websocket_recv(
    crnet_websocket socket,
    void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_received,
    CRNET_WEBSOCKET_MESSAGE *out_type,
    bool *out_message_complete);

/**
 * @brief Sends a close frame and ends the connection for this side there and
 * then. Known problems: a `reason` over 123 bytes returns
 * CR_RESULT_NET_SEND_FAILED and not an invalid-argument result; no
 * stack says whether the frame has gone out before a destroy that follows
 * straight after; and on libcurl a close that couldn't be sent leaves the
 * socket broken and not closed (crnet_websocket_recv() then returns
 * CR_RESULT_NET_RECV_FAILED).
 *
 * Never waits - neither for room to send nor for the peer - which is why it
 * takes no time limit.
 *
 * Once it has succeeded the socket is finished, the same on every stack:
 *
 * - crnet_websocket_recv() returns CR_RESULT_NET_WS_CLOSED, at once
 *   and from then on - a receive already waiting when the close was made
 *   included. Nothing more is handed over: not what the peer sends
 *   afterwards, not its answer to the close, and not a message that had
 *   already arrived and wasn't received yet. A caller that wants what is
 *   on its way receives it first and closes afterwards; one that wants the
 *   peer's agreement asks for it in a message of its own.
 * - crnet_websocket_send() and a second close fail.
 * - Only destroy is left.
 *
 * The protocol would let a side go on receiving after its own close frame
 * until the peer's arrives. NSURLSession can't - its close is the end of
 * its receiving - so nobody does.
 *
 * The frame is handed to the stack, not seen off: a destroy straight after
 * may drop the connection before it has gone out, and no stack says when
 * it has. A peer that must be told sends and receives that in messages.
 *
 * A close frame goes after the message in flight, never into it. With a
 * message still in flight this sends nothing and returns
 * CR_RESULT_NET_TIMED_OUT, without waiting (close takes no limit); the
 * socket is as it was - recv does not start returning
 * CR_RESULT_NET_WS_CLOSED - and the caller flushes, then closes again.
 *
 * Thread: may overlap crnet_websocket_recv() and
 * crnet_websocket_abort() on the same socket.
 *
 * @param[in] socket The connected socket to close.
 * @param[in] code   The close code (RFC 6455, e.g. 1000).
 * @param[in] reason The close reason, UTF-8, null-terminated and at most 123
 *                   bytes, or NULL for none.
 *
 * @retval CR_SUCCESS The close frame was handed to the stack.
 * @retval CR_RESULT_NET_INVALID_WEBSOCKET `socket` is NULL or isn't
 *         connected.
 * @retval CR_RESULT_NET_ABORTED crnet_websocket_abort() was called.
 * @retval CR_RESULT_NET_TIMED_OUT A message is in flight; nothing was
 *         sent.
 * @retval CR_RESULT_NET_SEND_FAILED `reason` is over 123 bytes; the message
 *         in flight failed on its way; or the connection has already ended
 *         - the peer closed it, it broke, it was closed before - and there
 *         is nothing to send a frame on.
 */
CR_RESULT crnet_websocket_close(
    crnet_websocket socket,
    uint16_t code,
    const char *reason);

/**
 * @brief Returns the peer's close code. Known problem: only the code is
 * kept; the reason the peer gave with it can't be read.
 *
 * @param[in] socket The socket to ask.
 *
 * @return The peer's close code, once crnet_websocket_recv() has returned
 *         CR_RESULT_NET_WS_CLOSED because the peer closed the
 *         connection.
 * @retval 0 The peer's close frame carried no code; the peer hasn't closed
 *         the connection; the close was this side's, since the peer's
 *         answer to it is never read; or `socket` is NULL.
 */
uint16_t crnet_websocket_get_close_code(crnet_websocket socket);

/**
 * @brief Ends a wait on the socket, and every call after it.
 *
 * See @ref crnet_aborting "Aborting". The connection is dropped without a
 * close frame.
 *
 * Thread: may overlap any call on the socket but its destroy.
 *
 * @param[in] socket The socket to abort.
 */
void crnet_websocket_abort(crnet_websocket socket);

/**
 * @brief Reads what the platform reported for the socket's most recent
 * failure on one side.
 *
 * See @ref crnet_error_detail "Error detail". A connection a close frame
 * ended (CR_RESULT_NET_WS_CLOSED) or the server declined to open
 * (CR_RESULT_NET_WS_UPGRADE_REFUSED) is the protocol working, not the
 * platform failing, and records nothing.
 *
 * Thread: a call on the side it reads: may overlap a call on the other
 * side.
 *
 * @param[in]  socket    The socket to ask. NULL gives an empty record.
 * @param[in]  side      Which of the socket's two records to read.
 * @param[out] out_error Receives the record.
 */
void crnet_websocket_get_error(
    crnet_websocket socket,
    CRNET_ERROR_SIDE side,
    crnet_error *out_error);

/**
 * @brief Drops the connection without a close frame and frees the socket.
 *
 * @param[in] socket The socket to destroy. NULL is taken and nothing is done
 *                   with it.
 */
void crnet_websocket_destroy(crnet_websocket socket);

/** @} */

#endif // CRNET_EXT_WEBSOCKET

/* A wait set is on every platform: HTTP transfers are members wherever
 * there is HTTP, and its own wake is a pipe (a loopback socket on Windows),
 * which needs no network permission. Its TCP and WebSocket calls stay
 * behind their own macros. */

/**
 * @defgroup crnet_wait Wait set
 * @ingroup crnet
 * @brief One call waiting on many objects. Known problems: it is poll()
 * and not epoll or kqueue, so each wait costs time in proportion to the
 * members - fine for hundreds, not for thousands; an object destroyed while
 * still in a set is not caught, and the set then polls a closed descriptor
 * or asks a freed WebSocket; a libcurl WebSocket is not named for a close
 * this side made; on Windows before Windows 10 version 2004, WSAPoll
 * doesn't report a TCP connect that failed, so a connecting socket is
 * named only for a connection made; a libcurl transfer may be named for
 * libcurl's own timer with nothing to hand over; not run on watchOS, where
 * the set holds HTTP transfers only.
 *
 * Every waiting call above waits on one object. A wait set is how one
 * call waits on several - a server's listener and all its connections -
 * without a thread of the caller's for each and without circling them with
 * CRNET_NO_WAIT.
 * It is the platform's poll() (WSAPoll on Windows) over the objects' own
 * sockets, and it does none of the receiving, accepting or sending itself:
 * crnet_wait_set_wait() only says which objects are worth calling, and
 * the caller then makes the ordinary calls on them with CRNET_NO_WAIT.
 *
 * TCP sockets, TCP listeners, UDP sockets, WebSockets, HTTP transfers and
 * service discovery's adverts, browsers and resolvers can be in one.
 *
 * How to use one, which is the same as for epoll or kqueue used carefully:
 *
 * - After crnet_wait_set_wait() names an object, call it with
 *   CRNET_NO_WAIT until it returns CR_RESULT_NET_TIMED_OUT, and only
 *   then wait again. A stack can hold more than it had when it woke the
 *   set - a WebSocket hands a message over in parts - and whether the
 *   leftover would be reported a second time differs between them. Draining
 *   is the one rule that is right on all of them.
 * - Being named is a reason to try, not a promise. The first try after a
 *   wake can itself return CR_RESULT_NET_TIMED_OUT (libcurl's
 *   WebSocket is always named once after it is added, since what it has
 *   already read can't be asked about), and that is not an error.
 * - An object that has ended - aborted, closed by the peer, broken - is
 *   named too. The call made on it then says which, and the caller removes
 *   it.
 * - A member whose connect is under way (a TCP socket, a WebSocket) is
 *   named, whatever bits it was added with, once the connect has an
 *   outcome. The call to make then is its `_connect_continue`.
 *
 * Whose is what:
 *
 * - The set doesn't own its objects. An object is removed from its set
 *   before it is destroyed; destroying the set first removes whatever is
 *   still in it.
 * - A set is one caller at a time in its add, remove, wait and destroy, as
 *   every object is.
 * - Thread: crnet_wait_set_abort() and crnet_wait_set_wake() may
 *   overlap any call on the set but its destroy.
 * - Thread: while an object is in a set, crnet_wait_set_wait() counts as
 *   a receiving call on it: no recv or accept on it may overlap a wait on
 *   the set. A sending call on it may overlap the wait, as it may a
 *   receive.
 * - Thread: while an HTTP transfer is in a set, a wait on the set counts
 *   as a call on the transfer: no call on the transfer may overlap it.
 * - Thread: while a socket whose connect is under way is in a set, a wait
 *   on the set counts as a call on the whole socket: its
 *   `_connect_continue` may not overlap the wait. Once the socket is
 *   connected, the wait counts as a receiving call, as above.
 * - An object goes in one set at a time, an HTTP transfer too.
 * @{
 */

/** @brief Opaque handle to a wait set. */
typedef struct crnet_wait_set_t *crnet_wait_set;

/** @brief What to be told about, and what crnet_wait_set_wait() reports. */
typedef enum CRNET_WAIT_FOR {
	/** There is something for recv, accept or receive_response, or the
	 * object has ended. */
	CRNET_WAIT_FOR_INCOMING = 1,
	/** A sending call would make progress now: crnet_tcp_socket_send() or
	 * crnet_http_transfer_send() would take bytes (a start under way
	 * counts: its next call is send), or crnet_websocket_send() would take
	 * a message (none in flight, or the one in flight can be carried on).
	 * For the stretch a caller has something it couldn't send: a TCP socket
	 * or transfer that isn't backed up, and a WebSocket with nothing in
	 * flight, have room all the time, and a set asked about room on one
	 * returns at once every time. */
	CRNET_WAIT_FOR_ROOM = 2,
} CRNET_WAIT_FOR;

/** @brief One object crnet_wait_set_wait() found worth calling. */
/* Frozen: an array element crnet_wait_set_wait fills. It never changes; a new need gets a new type. */
typedef struct crnet_wait_event {
	void *user;     /**< What the object was added with. */
	uint32_t ready; /**< CRNET_WAIT_FOR bits, from among those asked for. */
} crnet_wait_event;

/**
 * @brief Makes an empty set, for objects made with `instance`.
 *
 * @param[in]  instance  The instance the set is made with; it takes members
 *                       of that instance only.
 * @param[out] out_set   Receives the new set.
 * @param[out] out_error Receives the system's reason for a failure, which
 *                       leaves no set to ask. May be NULL. See
 *                       @ref crnet_error_detail "Error detail".
 *
 * @retval CR_SUCCESS The set was made.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_set` is NULL.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_OUT_OF_MEMORY The set could not be allocated.
 * @return Another CR_RESULT where the system refused the set its wake
 *         socket; `out_error` has the reason.
 */
CR_RESULT crnet_wait_set_create(
    crnet_instance instance,
    crnet_wait_set *out_set,
    crnet_error *out_error);

/**
 * @brief Removes whatever is still in the set and frees it. Known problem: a
 * member that was itself destroyed while still in the set is not caught.
 *
 * @param[in] set The set to destroy. NULL is taken and nothing is done with
 *                it.
 */
void crnet_wait_set_destroy(crnet_wait_set set);

#if CRNET_EXT_TCP

/**
 * @brief Adds a TCP socket to the set, connected or with its connect under
 * way. Known problem: on Windows before version 2004 a connect that failed
 * is never named (WSAPoll), only one that succeeded.
 *
 * A socket whose connect is under way is named once the connect has an
 * outcome, whatever `wait_for` says (call
 * crnet_tcp_socket_connect_continue() then); after that, as `wait_for`
 * says. While its connect is under way a wait on the set counts as a call
 * on the whole socket.
 *
 * To change `wait_for`, remove the socket and add it again.
 *
 * @param[in] set      The set to add to.
 * @param[in] socket   The socket, connected or connecting. It is removed
 *                     from the set before it is destroyed.
 * @param[in] wait_for One or both CRNET_WAIT_FOR bits.
 * @param[in] user     The caller's own, handed back in the events - a
 *                     pointer to whatever the caller keeps per connection.
 *                     Never dereferenced.
 *
 * @retval CR_SUCCESS The socket is in the set.
 * @retval CR_RESULT_NET_INVALID_WAIT_SET `set` is NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `wait_for` is 0 or has a bit
 *         that isn't a CRNET_WAIT_FOR one.
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET `socket` is NULL, connect was
 *         never called on it, or its connect failed.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `socket` was made with another
 *         instance than the set's - as for every kind of member.
 * @retval CR_RESULT_NET_ALREADY_IN_WAIT_SET `socket` is already in this
 *         set.
 * @retval CR_RESULT_OUT_OF_MEMORY The set could not grow.
 */
CR_RESULT crnet_wait_set_add_tcp_socket(
    crnet_wait_set set,
    crnet_tcp_socket socket,
    uint32_t wait_for,
    void *user);

/**
 * @brief Adds a listener to the set.
 *
 * It is reported CRNET_WAIT_FOR_INCOMING when
 * crnet_tcp_listener_accept() has a connection to hand over.
 *
 * @param[in] set      The set to add to.
 * @param[in] listener The listener. It is removed from the set before it is
 *                     destroyed.
 * @param[in] user     The caller's own, handed back in the events. Never
 *                     dereferenced.
 *
 * @retval CR_SUCCESS The listener is in the set.
 * @retval CR_RESULT_NET_INVALID_WAIT_SET `set` is NULL.
 * @retval CR_RESULT_NET_INVALID_TCP_LISTENER `listener` is NULL.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `listener` was made with
 *         another instance than the set's.
 * @retval CR_RESULT_NET_ALREADY_IN_WAIT_SET `listener` is already in
 *         this set.
 * @retval CR_RESULT_OUT_OF_MEMORY The set could not grow.
 */
CR_RESULT crnet_wait_set_add_tcp_listener(
    crnet_wait_set set,
    crnet_tcp_listener listener,
    void *user);

/**
 * @brief Removes a TCP socket from the set.
 *
 * @param[in] set    The set to remove from.
 * @param[in] socket The socket to remove. One that isn't in the set, or
 *                   NULL, is taken and nothing is done with it.
 */
void crnet_wait_set_remove_tcp_socket(crnet_wait_set set, crnet_tcp_socket socket);

/**
 * @brief Removes a listener from the set.
 *
 * @param[in] set      The set to remove from.
 * @param[in] listener The listener to remove. One that isn't in the set, or
 *                     NULL, is taken and nothing is done with it.
 */
void crnet_wait_set_remove_tcp_listener(crnet_wait_set set, crnet_tcp_listener listener);

#endif // CRNET_EXT_TCP

#if CRNET_EXT_WEBSOCKET

/**
 * @brief Adds a WebSocket to the set, connected or with its opening under
 * way. Known problem: a libcurl WebSocket is polled by its descriptor,
 * which doesn't show bytes libcurl or the TLS layer has already read and
 * says nothing when this side closes; it is named once after it is added,
 * and not for its own close.
 *
 * It is reported CRNET_WAIT_FOR_INCOMING when crnet_websocket_recv() has
 * message data or the connection has ended, and CRNET_WAIT_FOR_ROOM when
 * crnet_websocket_send() would take a message. A socket whose opening is
 * under way is named once the opening has an outcome, whatever `wait_for`
 * says (call crnet_websocket_connect_continue() then); until then a wait
 * on the set counts as a call on the whole socket. NSURLSession and
 * WinHTTP only say a message has arrived to someone who has asked for the
 * next one, so a connected socket in a set for INCOMING asks, and the
 * answer then waits in Carrottus Net for crnet_websocket_recv() as it would
 * after a recv that timed out.
 *
 * To change `wait_for`, remove the socket and add it again.
 *
 * @param[in] set      The set to add to.
 * @param[in] socket   The WebSocket, connected or connecting. It is
 *                     removed from the set before it is destroyed.
 * @param[in] wait_for One or both CRNET_WAIT_FOR bits.
 * @param[in] user     The caller's own, handed back in the events. Never
 *                     dereferenced.
 *
 * @retval CR_SUCCESS The socket is in the set.
 * @retval CR_RESULT_NET_INVALID_WAIT_SET `set` is NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `wait_for` is 0 or has a bit
 *         that isn't a CRNET_WAIT_FOR one.
 * @retval CR_RESULT_NET_INVALID_WEBSOCKET `socket` is NULL, connect was
 *         never called on it, or its connect failed.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `socket` was made with another
 *         instance than the set's.
 * @retval CR_RESULT_NET_ALREADY_IN_WAIT_SET `socket` is already in a
 *         set, this one or another.
 * @retval CR_RESULT_OUT_OF_MEMORY The set could not grow.
 */
CR_RESULT crnet_wait_set_add_websocket(
    crnet_wait_set set,
    crnet_websocket socket,
    uint32_t wait_for,
    void *user);

/**
 * @brief Removes a WebSocket from the set.
 *
 * @param[in] set    The set to remove from.
 * @param[in] socket The socket to remove. One that isn't in the set, or
 *                   NULL, is taken and nothing is done with it.
 */
void crnet_wait_set_remove_websocket(crnet_wait_set set, crnet_websocket socket);

#endif // CRNET_EXT_WEBSOCKET

#if CRNET_EXT_UDP

/**
 * @brief Adds a UDP socket to the set: its descriptor in the set's poll(),
 * as a TCP socket's is.
 *
 * It is named CRNET_WAIT_FOR_INCOMING when crnet_udp_socket_recv_from() has
 * a datagram to hand over, and CRNET_WAIT_FOR_ROOM when
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
 * @retval CR_RESULT_NET_ALREADY_IN_WAIT_SET `socket` is already in this set
 *         (as for a TCP socket: a descriptor may be in two sets' poll() at
 *         once).
 * @retval CR_RESULT_OUT_OF_MEMORY The set could not grow.
 */
CR_RESULT crnet_wait_set_add_udp_socket(
    crnet_wait_set set,
    crnet_udp_socket socket,
    uint32_t wait_for,
    void *user);

/**
 * @brief Takes a UDP socket out of the set. One not in it is ignored; so is
 * NULL for either argument.
 *
 * @param[in] set    The set.
 * @param[in] socket The socket.
 */
void crnet_wait_set_remove_udp_socket(crnet_wait_set set, crnet_udp_socket socket);

#endif // CRNET_EXT_UDP

#if CRNET_EXT_DISCOVERY

/**
 * @brief Adds an advert to the set. It is named
 * CRNET_WAIT_FOR_INCOMING when crnet_service_advert_next() has an event or
 * a failure to hand over. On Apple its descriptor is polled; on Linux,
 * Avahi's descriptors and timers; on Windows the system's callback wakes
 * the set.
 *
 * @param[in] set    The set to add to.
 * @param[in] advert The advert. It is removed from the set before it is
 *                   destroyed.
 * @param[in] user   The caller's own, handed back in the events. Never
 *                   dereferenced.
 *
 * @retval CR_SUCCESS The advert is in the set.
 * @retval CR_RESULT_NET_INVALID_WAIT_SET `set` is NULL.
 * @retval CR_RESULT_NET_INVALID_SERVICE_ADVERT `advert` is NULL.
 * @retval CR_RESULT_NET_INVALID_INSTANCE `advert` was made with another
 *         instance than the set's.
 * @retval CR_RESULT_NET_ALREADY_IN_WAIT_SET `advert` is in this set or
 *         another: unlike a socket, a discovery object is in one
 *         set at most, since on Windows the system's callback wakes the one
 *         set it was added to.
 * @retval CR_RESULT_OUT_OF_MEMORY The set could not grow.
 */
CR_RESULT crnet_wait_set_add_service_advert(
    crnet_wait_set set,
    crnet_service_advert advert,
    void *user);

/** @brief Takes an advert out of the set. One not in it is ignored;
 * so is NULL for either argument. */
void crnet_wait_set_remove_service_advert(crnet_wait_set set, crnet_service_advert advert);

/**
 * @brief Adds a browser to the set, named CRNET_WAIT_FOR_INCOMING
 * when crnet_service_browser_next() has an event or a failure. Results and
 * arguments as crnet_wait_set_add_service_advert(), with
 * CR_RESULT_NET_INVALID_SERVICE_BROWSER for a NULL `browser`.
 */
CR_RESULT crnet_wait_set_add_service_browser(
    crnet_wait_set set,
    crnet_service_browser browser,
    void *user);

/** @brief Takes a browser out of the set. One not in it is ignored;
 * so is NULL for either argument. */
void crnet_wait_set_remove_service_browser(crnet_wait_set set, crnet_service_browser browser);

/**
 * @brief Adds a resolver to the set, named CRNET_WAIT_FOR_INCOMING
 * when crnet_service_resolver_next() has an event or a failure. Results
 * and arguments as crnet_wait_set_add_service_advert(), with
 * CR_RESULT_NET_INVALID_SERVICE_RESOLVER for a NULL `resolver`.
 */
CR_RESULT crnet_wait_set_add_service_resolver(
    crnet_wait_set set,
    crnet_service_resolver resolver,
    void *user);

/** @brief Takes a resolver out of the set. One not in it is
 * ignored; so is NULL for either argument. */
void crnet_wait_set_remove_service_resolver(crnet_wait_set set, crnet_service_resolver resolver);

#endif // CRNET_EXT_DISCOVERY

/**
 * @brief Adds an HTTP transfer to the set, from its start on. Known problem:
 * on libcurl a transfer may be named for libcurl's own timer with nothing
 * to hand over, and its descriptors are asked for afresh at every wait.
 *
 * A caller that advances its transfers without waiting has to sleep
 * somewhere when none of them has anything for it, and this is where every
 * other object of this header is slept on.
 *
 * It is reported CRNET_WAIT_FOR_INCOMING when
 * crnet_http_transfer_receive_response() or crnet_http_transfer_recv()
 * would return something other than CR_RESULT_NET_TIMED_OUT (the response
 * or body has come, or the transfer has ended), and CRNET_WAIT_FOR_ROOM
 * when crnet_http_transfer_send() would take bytes.
 *
 * How each stack wakes it: WinHTTP and NSURLSession through the set's
 * wake, from their callbacks, as their WebSockets do; libcurl through the
 * client's descriptors, asked for at every wait, the wait shortened to
 * libcurl's own timer, and a piece of body libcurl has already handed over
 * counting as INCOMING without a poll.
 *
 * Thread: while the transfer is in the set, crnet_wait_set_wait() counts
 * as a call on it: no call on the transfer may overlap a wait on the set.
 *
 * @param[in] set      The set to add to.
 * @param[in] transfer The transfer, started: a start that returned
 *                     CR_RESULT_NET_TIMED_OUT has started it. It is removed
 *                     from the set before it is destroyed.
 * @param[in] wait_for One or both CRNET_WAIT_FOR bits.
 * @param[in] user     The caller's own, handed back in the events. Never
 *                     dereferenced.
 *
 * @retval CR_SUCCESS The transfer is in the set.
 * @retval CR_RESULT_NET_INVALID_WAIT_SET `set` is NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `wait_for` is 0 or has a bit
 *         that isn't a CRNET_WAIT_FOR one.
 * @retval CR_RESULT_NET_INVALID_HTTP_TRANSFER `transfer` is NULL, or
 *         hasn't been started, or a call on it has failed (only destroy is
 *         left).
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
 * @brief Removes an HTTP transfer from the set.
 *
 * After this returns, the transfer's stack makes no further wake of the
 * set.
 *
 * @param[in] set      The set to remove from.
 * @param[in] transfer The transfer to remove. One that isn't in the set, or
 *                     NULL, is taken and nothing is done with it.
 */
void crnet_wait_set_remove_http_transfer(
    crnet_wait_set set,
    crnet_http_transfer transfer);

/**
 * @brief Waits for any object in the set to be worth calling. Known problem:
 * each call is a poll() over every member, so its cost grows with the size
 * of the set.
 *
 * Objects that didn't fit in `out_events` are reported by the next call. An
 * empty set is waited on like any other and simply has nothing that could
 * be ready.
 *
 * @param[in]  set        The set to wait on.
 * @param[in]  timeout_ms The longest to wait
 *                        (@ref crnet_time "Time limits");
 *                        CRNET_NO_WAIT only looks.
 * @param[out] out_events Receives up to `capacity` events, one per object
 *                        worth calling.
 * @param[in]  capacity   How many events `out_events` has room for.
 * @param[out] out_count  Receives how many events were written.
 *
 * @retval CR_SUCCESS `out_count` events were written. An `out_count` of 0
 *         means crnet_wait_set_wake() ended the wait.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_count` or `out_events` is
 *         NULL.
 * @retval CR_RESULT_NET_INVALID_WAIT_SET `set` is NULL.
 * @retval CR_RESULT_ZERO_SIZE `capacity` is 0.
 * @retval CR_RESULT_NET_ABORTED crnet_wait_set_abort() was called.
 * @retval CR_RESULT_NET_TIMED_OUT No object was ready in time.
 * @retval CR_RESULT_OUT_OF_MEMORY The set's own working memory could not
 *         grow.
 * @retval CR_RESULT_GENERAL_FAILURE The system's poll failed;
 *         crnet_wait_set_get_error() has its reason.
 */
CR_RESULT crnet_wait_set_wait(
    crnet_wait_set set,
    uint32_t timeout_ms,
    crnet_wait_event *out_events,
    uint32_t capacity,
    uint32_t *out_count);

/**
 * @brief Ends a wait on the set, and every wait after it.
 *
 * See @ref crnet_aborting "Aborting". Ends the wait, not the objects:
 * each of those still has its own `_abort`.
 *
 * Thread: may overlap any call on the set but its destroy.
 *
 * @param[in] set The set to abort.
 */
void crnet_wait_set_abort(crnet_wait_set set);

/**
 * @brief Ends one wait and leaves the set usable, which is what an abort
 * can't do.
 *
 * Thread: at any point after `_create`, from any thread, overlapping any
 * call on the set but its destroy, as an abort may. The
 * crnet_wait_set_wait() in progress - or, when none is, the
 * next one - returns CR_SUCCESS without waiting any further, with
 * whatever members happen to be ready and an `out_count` of 0 when none
 * is. Several wakes made before a wait has returned for them may end only
 * the one wait.
 *
 * For a caller that waits on a set and also has work handed to it from
 * elsewhere (something to send, a member to add): whoever hands the work
 * over puts it wherever the two share it and then wakes the set. What a
 * wake means is the caller's; the set only stops waiting.
 *
 * @param[in] set The set to wake. NULL is taken and nothing is done with it.
 */
void crnet_wait_set_wake(crnet_wait_set set);

/**
 * @brief Reads what the platform reported for the most recent
 * crnet_wait_set_wait() that failed.
 *
 * See @ref crnet_error_detail "Error detail".
 *
 * @param[in]  set       The set to ask. NULL gives an empty record.
 * @param[out] out_error Receives the record.
 */
void crnet_wait_set_get_error(
    crnet_wait_set set,
    crnet_error *out_error);

/** @} */

#if __cplusplus
}
#endif

#endif // CRNET_H
