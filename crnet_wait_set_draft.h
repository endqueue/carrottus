/*
 * crnet_wait_set_draft.h - Carrottus Net's wait set: its instance, and the
 * memory a set uses once it is set up (crnet.h, "Wait set").
 *
 * FOR YOUR APPROVAL, version 4 (2026-11-23). Not built, not in any target.
 *   Version 1: the first, after a scratch check of today's code on the Mac
 *   (prompts/crnet_local_network_record.md, "Part 2: the scratch check").
 *   Version 2 (critique cycle 1, the library's rules): no caller named
 *   [CHANGED]; the reserve takes member counts per kind in a based desc and
 *   the library turns them into descriptors from its own backends, in place
 *   of per-platform descriptor macros a caller summed [CHANGED]; its promise
 *   limited to the set's own memory, with every member's own allocation in a
 *   wait named [CHANGED]; the Windows and Linux discovery figures owed until
 *   those backends are built [CHANGED]; get_instance's order of checks
 *   written out [CHANGED]; its Thread: exception in the group's list
 *   [CHANGED]; the room's wording; tests and version completed.
 *   Version 3 (critique cycle 2, platform truth): the table's figures at
 *   most per member, 0 after an end, Windows' UDP owed and Linux's from
 *   code not yet compiled [CHANGED]; libcurl's descriptors unbounded, not
 *   "two" [CHANGED]; the growing array the HTTP client's, not a
 *   transfer's [CHANGED]; the stacks' own allocations in a wait named
 *   [CHANGED]; one member bound for the set's growth and the reserve, a
 *   size past size_t out of memory [CHANGED]; poll()'s descriptor limit
 *   noted; the comparison with epoll, kqueue and Windows corrected; a
 *   fault in today's growth of the poll's two arrays named, fixed in 0.5.0
 *   as a fix within today's contract.
 *   Version 4 (critique cycle 3, the callers' need): two reserves on one
 *   set compose as the larger, on totals, and the set's owner reserves for
 *   everything in it [CHANGED]; the remove-and-add promise stated with its
 *   conditions [CHANGED]; libcurl's WebSocket named once at each add, said
 *   [CHANGED]; which members a reserve covers said plainly, a wait that may
 *   still run out of memory named [CHANGED]; "Why" no longer claims a
 *   path free of allocation for adds of new members [CHANGED]; each owed
 *   figure a test gate; what the callers owe themselves listed.
 * Written against include/crnet.h as built on 2026-11-23 (carrottus 0.5.0:
 * UDP sockets and service discovery among the members) and
 * src/net/crnetwaitset.c, the one file every platform's wait set is.
 *
 * Every declaration below is marked [ADDED], [CHANGED] or [REMOVED];
 * nothing unmarked changes. It compiles beside today's crnet.h: an [ADDED]
 * call or type is declared under its own name; an [ADDED] struct type,
 * which belongs in crcore.h's enum, is a macro here, of the enum's type,
 * only so the file compiles. Checked with
 *   clang -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -I include -x c
 *   clang -fsyntax-only -std=c++17 -Wall -Wextra -Wpedantic -I include -x c++
 * with -DCARROTTUS_PLATFORM_APPLE=1, -DCARROTTUS_PLATFORM_WIN32=1,
 * -DCARROTTUS_PLATFORM_LINUX=1 and bare.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * A caller may drive code of its own from one update, lending that code
 * its instance and its wait set; the code then adds and removes members as
 * it runs - waiting for ROOM only while it has something to send (crnet.h,
 * CRNET_WAIT_FOR_ROOM), and remaking sockets and resolvers as peers come
 * and go (card X47's local carrier). Its critique (the meta repo's
 * prompts/eqc_v21_critique_record.md, C4.5 and C5.10) found two things
 * Carrottus Net doesn't offer:
 *
 * 1. A set's instance. Code handed a set and an instance can't tell whether
 *    they belong together until its first add is refused with
 *    CR_RESULT_NET_INVALID_INSTANCE, which may come long after, on a
 *    connection that has nothing to do with the mistake.
 * 2. A set that doesn't allocate once it is set up. A remove and an add of
 *    a member already in use - to wait for ROOM only while a frame is
 *    backed up - should not reach an allocator on a live connection; and
 *    the set's own growth, with its failure, should come at a call the
 *    caller chooses rather than inside an add or a wait. (A new member
 *    allocates in its own create whatever the set does: a reserve can't
 *    make adding new sockets and resolvers free of allocation, only keep
 *    the set's growth and its CR_RESULT_OUT_OF_MEMORY off the add and the
 *    wait.)
 *
 * What today's code does (the scratch check, macOS, Release; four members of
 * every kind made once, then churned in and out of one set 1000 times after
 * a warm-up, the count never above a mark of 18, each round a wait):
 *
 * - The members' array never grows below its high-water mark: a remove
 *   swaps the last member into the gap (eq_array_remove_swap) and an add
 *   reserves only when full, 16 then doubling. 0 allocations.
 * - A wait's poll entries (`entries` and `infos`, two arrays of one
 *   capacity) grow lazily to the largest poll the set has seen, 16 at its
 *   first wait, then doubling. Their size is the members' descriptors, not
 *   their count: on Apple a TCP socket, listener, UDP socket and discovery
 *   object poll one each, a WebSocket and an HTTP transfer none (their
 *   stacks call back). A fresh set warmed with three WebSockets among its 18
 *   members (16 entries), then churned with its kinds swapped (up to 19),
 *   allocated twice inside the counted rounds: both arrays from 16 to 32.
 * - Beyond the set's own arrays, a member may allocate in a wait itself:
 *   libcurl's transfers and openings grow an array of their stack's
 *   descriptors (crnetcurl.c, `crnet_curl_wait_fds`: the HTTP client's, kept across
 *   its transfers, and an opening WebSocket's own; libcurl 8.8 and later)
 *   through their owner's allocator when libcurl hands over more than it
 *   held. And a wait calls the stacks themselves (libcurl's opening driven
 *   on, a WebSocket's next receive asked of NSURLSession or WinHTTP), which
 *   allocate their own memory, never the instance's.
 * - A fault under all of it: the poll's two arrays share one capacity, but
 *   their growth checked `entries` only and reserved `entries` then `infos`
 *   (crnetwaitset.c, crnet_wait_poll_push). Had the second reserve failed,
 *   the two fell out of step, the next wait skipped the check, and a
 *   Release build read `infos` past its count. Fixed in 0.5.0 within
 *   today's contract (both capacities compared, the pair grown both or
 *   neither, as carrottus/CLAUDE.md asks of two arrays sharing one
 *   capacity), so the reserve builds on it.
 *
 * So the critique's claim held "by accident" for the members, and does not
 * hold for a wait once the kinds in a set change. Nothing in the header says
 * either way.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * For the instance: what the APIs Carrottus translates do for an object that
 * keeps its parent (D3D12's ID3D12DeviceChild::GetDevice, Metal's `device`
 * property; Vulkan's objects are one device's by construction); Carrottus
 * Net's own getters (crnet_tcp_listener_get_port(),
 * crnet_udp_socket_get_local_address()), which read one fixed fact. Not
 * taken: a check call (crnet_wait_set_check_instance(set, instance)), which
 * answers one question where a getter answers it and any other.
 *
 * For the memory, three options:
 *
 * - (a) The promise stated in the header: a set allocates only when it
 *   holds more members than ever, or a wait polls more descriptors than
 *   ever. True of today's code, but a promise about history: a caller can't
 *   know from its own calls whether its next add or wait allocates, since
 *   how many descriptors a member polls is its stack's. Not taken as the
 *   contract; today's behaviour is described, as it is.
 * - (b) A capacity the caller gives, as a reserve: room made now, at a
 *   call of the caller's, for so many members of each kind, and a contract
 *   that the set's own memory doesn't grow while it stays within it. poll()
 *   and WSAPoll take the caller's array, which is what this reserves; epoll
 *   and kqueue keep registrations in the kernel, so only their output
 *   array would be the caller's to size; Windows' registered readiness
 *   (AFD_POLL over IOCP, ProcessSocketNotifications) is the same shape, and
 *   WSAEventSelect waits on 64 at most. Vulkan's descriptor
 *   pools and D3D12's heaps are sized by the caller once, the same idea.
 *   Counts per kind, not descriptors: how many descriptors a member polls is
 *   how each backend is written, which changes with a backend and isn't the
 *   caller's to know; the library reads its own figures. Taken, as a call of
 *   its own (crnet_wait_set_create() takes no desc, and a reserve may be
 *   wanted again as peers grow).
 * - (c) A call that changes a member's `wait_for` in place (epoll's
 *   EPOLL_CTL_MOD, kqueue's EV_ADD on an existing event). It spares a remove
 *   and add but not the allocation question, which is the same for one
 *   member removed and added again (it takes the room it had), and it would
 *   be one call per member kind (eight). The header's rule ("To change
 *   `wait_for`, remove the socket and add it again") stays. Not taken.
 *
 * Carrottus Net's rules held to: nothing decided for the caller (no
 * capacity is a default; a set never reserved behaves exactly as today's);
 * every allocation from the instance's allocator, and its failure
 * CR_RESULT_OUT_OF_MEMORY with the set as it was; no global state; one file
 * for every platform (crnetwaitset.c), each backend saying how many
 * descriptors its members poll through crnetshared.h; no caller named.
 *
 * ---------------------------------------------------------------------------
 * ABI and version
 * ---------------------------------------------------------------------------
 *
 * No break: carrottus 0.6.0 (from 0.5.0), each added name listed against
 * abi.txt by ./check.sh --update-abi. Added: crnet_wait_set_get_instance(),
 * crnet_wait_set_reserve(), crnet_wait_set_reserve_desc and its struct type
 * (CR_STRUCT_TYPE_NET_WAIT_SET_RESERVE_DESC, 3016). Text changes: the wait
 * set group gains "Memory" and a Thread: bullet; crnet_wait_set_add_*'s and
 * crnet_wait_set_wait()'s CR_RESULT_OUT_OF_MEMORY reworded.
 *
 * ---------------------------------------------------------------------------
 * Tests (crnet_test's part "wait", cr_noalloc_tests, the misuse harness)
 * ---------------------------------------------------------------------------
 *
 * - get_instance: a set's own instance read back; NULL set
 *   CR_RESULT_NET_INVALID_WAIT_SET with `*out_instance` NULL; NULL output
 *   CR_RESULT_NULL_OUTPUT_PARAMETER; a destroyed set refused (Debug,
 *   CR_STALE, the misuse harness's freed half); from another thread
 *   overlapping a wait (Thread:), under -tsan.
 * - reserve: NULL set, a destroyed set (Debug), NULL desc, a desc's wrong
 *   type and a chain, every count 0, a total past what a wait counts, each
 *   refused in that order; a reserve smaller than the room the set has
 *   changes nothing (CR_SUCCESS); an injected out-of-memory at each of its
 *   allocations, the set as it was and its next wait working.
 * - The contract, on a counting allocator and under cr_noalloc_tests' hook:
 *   a set reserved for the kinds the platform has, members of every kind
 *   churned in and out 1000 times within the reserve, the kinds swapped, a
 *   wait each round: 0 allocations of the set's. The same set unreserved,
 *   warmed on one mix and churned with another, allocating (the known
 *   growth, shown, so the test proves the reserve and not luck).
 * - A member removed and added again to change its `wait_for`: 0 allocations
 *   once the set has waited with it in, reserved or not.
 * - Past the reserve: an add past a kind's count allocates or, under an
 *   injected failure, is CR_RESULT_OUT_OF_MEMORY with the set as it was.
 * - Each backend's figures (crnetshared.h) held to what its members poll:
 *   a wait's entries counted over one member of each kind, connecting,
 *   open and ended.
 * - Each "owed" figure in the table a gate: the promise ships on a platform
 *   only once its row is measured there (Windows' UDP and discovery,
 *   Linux's UDP and discovery).
 * - Two reserves on one set: the room the larger of the two, on totals.
 * - A WebSocket removed and added again for ROOM a thousand times: no
 *   allocation (Apple, Windows; on libcurl one INCOMING named an add).
 * - Per platform: macOS (plain, Release, ASan, TSan); Windows 11 (Debug,
 *   Release, -asan); Fedora 44 (GCC and clang, -asan and -tsan: libcurl's
 *   own arrays outside the reserve, measured and listed).
 */

#ifndef CRNET_WAIT_SET_DRAFT_H
#define CRNET_WAIT_SET_DRAFT_H

#include "crnet.h"

#if __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* crcore.h, CR_STRUCT_TYPE [ADDED]                                         */
/* ======================================================================== */

/** [ADDED] crnet_wait_set_reserve_desc (CR_STRUCT_BASE). The next free value
 * after 0.5.0's 3015. */
#define CR_STRUCT_TYPE_NET_WAIT_SET_RESERVE_DESC ((CR_STRUCT_TYPE)3016)

/* ======================================================================== */
/* crnet.h, Wait set: text that changes [CHANGED]                           */
/* ======================================================================== */

/* [CHANGED] The group's description gains a paragraph, "Memory":
 *
 *   A set's memory is the instance's allocator's. It has room for so many
 *   members and so many of their descriptors to poll, and grows in two
 *   places only: an add past its room for members, and a wait whose
 *   members' descriptors are more than its room for them. Room once made is
 *   kept until the set is destroyed. crnet_wait_set_reserve() makes room at
 *   a call of the caller's choosing, after which the set's own memory
 *   doesn't grow while its members stay within the reserve and each polls
 *   no more than the table at crnet_wait_set_reserve() says; a set holding
 *   a member whose figure is libcurl's (a transfer, an opening WebSocket)
 *   may still grow in a wait, and fail it with CR_RESULT_OUT_OF_MEMORY.
 *
 *   A member removed and added again - to change what it waits for -
 *   takes the room it had, reserved or not, once the set has waited with it
 *   in; not a libcurl transfer or opening WebSocket, whose descriptors
 *   change from one wait to the next. On libcurl an add names a WebSocket
 *   once (what it has already read can't be asked about), so a caller that
 *   adds it again for ROOM sees one INCOMING with nothing to hand over each
 *   time.
 *
 *   Whose reserve: the set's owner, for everything that will be in it.
 *   Code handed a set by its caller reserves nothing; it says how many
 *   members of each kind it keeps there at most, and its caller adds them
 *   into its own reserve.
 *
 *   Some members allocate in a wait themselves, outside the set's memory:
 *   on Linux (libcurl 8.8 and later) an HTTP client's array of its stack's
 *   descriptors, kept across its transfers, and an opening WebSocket's own,
 *   grow when libcurl hands over more than they held. And a wait calls the
 *   stacks themselves (libcurl's opening driven on, a WebSocket's next
 *   receive asked of NSURLSession or WinHTTP), which allocate their own
 *   memory, never the instance's.
 *
 * [CHANGED] CRNET_WAIT_FOR_ROOM's "remove and add again" points at
 * "Memory".
 *
 * [CHANGED] "Whose is what" gains a bullet:
 *
 *   - Thread: crnet_wait_set_get_instance() may overlap any call on the set
 *     but its destroy.
 *
 * [CHANGED] crnet_wait_set_add_tcp_socket(), _tcp_listener(),
 * _udp_socket(), _websocket(), _http_transfer(), _service_advert(),
 * _service_browser() and _service_resolver(): "CR_RESULT_OUT_OF_MEMORY The
 * set could not grow" becomes "The set had no room for another member
 * (its room is at least its reserve and its most members so far) and
 * could not grow; it is as it was."
 *
 * [CHANGED] crnet_wait_set_wait(): "CR_RESULT_OUT_OF_MEMORY The set's own
 * working memory could not ..." becomes "The members' descriptors were
 * more than the set has room for (at least its reserve's and its largest
 * wait's so far) and its room could not grow, or a member's own array
 * couldn't; nothing was waited for, and the set is as it was."
 */

/* ======================================================================== */
/* crnet.h, Wait set [ADDED]                                                */
/* ======================================================================== */

/**
 * @brief [ADDED] Reads the instance a set was made with: the one whose
 * objects it takes (crnet_wait_set_create()).
 *
 * For code handed a set and an instance, so that a mismatch is refused where
 * it is made rather than at the first add, which may come later and
 * elsewhere.
 *
 * The checks, in order, each its own `if`: `out_instance` NULL
 * (CR_RESULT_NULL_OUTPUT_PARAMETER), then `*out_instance` set to NULL; `set`
 * NULL, then a destroyed `set` in Debug (CR_RESULT_NET_INVALID_WAIT_SET) -
 * the outputs first, as crnet_tcp_listener_get_port() and
 * crnet_wait_set_wait() take theirs, so that the output is NULL on every
 * failure.
 *
 * Thread: may overlap any call on the set but its destroy: a set's
 * instance never changes.
 *
 * @param[in]  set          The set.
 * @param[out] out_instance Receives the instance; NULL on any failure but
 *                          its own.
 *
 * @retval CR_SUCCESS `out_instance` was written.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_instance` is NULL.
 * @retval CR_RESULT_NET_INVALID_WAIT_SET `set` is NULL.
 */
CR_RESULT crnet_wait_set_get_instance(
    crnet_wait_set set,
    crnet_instance *out_instance);

/**
 * @brief [ADDED] How many members of each kind a reserve makes room for:
 * the most the caller will have in the set at once. A kind this build
 * doesn't have (CRNET_EXT_*) is 0.
 */
typedef struct crnet_wait_set_reserve_desc {
	/** `type` is CR_STRUCT_TYPE_NET_WAIT_SET_RESERVE_DESC; `next` is NULL
	 * (nothing is chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	uint32_t       tcp_sockets;
	uint32_t       tcp_listeners;
	uint32_t       udp_sockets;
	uint32_t       websockets;
	uint32_t       http_transfers;
	uint32_t       service_adverts;
	uint32_t       service_browsers;
	uint32_t       service_resolvers;
} crnet_wait_set_reserve_desc;

/**
 * @brief [ADDED] Makes room in a set, now, for the members a desc counts and
 * for a wait over all their descriptors, so that the set's own memory
 * doesn't grow while its members stay within those counts. Known problems:
 * a member whose stack keeps as many descriptors as it needs at the moment
 * (libcurl's transfers and WebSocket openings: their connections, a
 * resolve's socketpair, a pool's connections shutting down) can't be
 * bounded, and a wait past the room grows the set; a member's own memory
 * isn't the set's (crnet.h, "Wait set", "Memory"); and poll() refuses more
 * descriptors than the process may hold open (RLIMIT_NOFILE on Linux,
 * OPEN_MAX on Apple), as crnet_wait_set_wait()'s
 * CR_RESULT_GENERAL_FAILURE, whatever room was made.
 *
 * How many descriptors each kind polls is each backend's, read by the set
 * from it; the caller counts members only. Today's figures, at most per
 * member (one ended, or ready already, polls none), from the scratch check
 * and the backends' code; "owed" where the backend isn't built yet:
 *
 * | Member          | Apple | Windows | Linux                      |
 * |-----------------|-------|---------|----------------------------|
 * | TCP socket      | 1     | 1 (*)   | 1                          |
 * | TCP listener    | 1     | 1       | 1                          |
 * | UDP socket      | 1     | owed    | 1 (written, not compiled)  |
 * | WebSocket       | 0     | 0       | 1 open, libcurl's opening  |
 * | HTTP transfer   | 0     | 0       | libcurl's, unbounded       |
 * | advert, browser, resolver | 1 | owed | owed                     |
 *
 * plus the set's own wake, always one. (*) Before Windows 10 version 2004
 * WSAPoll doesn't report a refused connect (crnet.h's known problem).
 *
 * A reserve is room, not a limit: an add past a kind's count, or a wait
 * past the room, grows the set as it would have without one (and may fail
 * with CR_RESULT_OUT_OF_MEMORY there). Room is the set's in two totals,
 * members and descriptors: a reserve makes each the larger of what the set
 * has and what this desc needs, so a second reserve doesn't add to the
 * first. The set's owner reserves once, for everything that will be in the
 * set, code it lends the set to included (crnet.h, "Wait set", "Memory").
 * Room is never given back before the set is destroyed.
 *
 * @param[in] set  The set.
 * @param[in] desc The counts. Only read during the call.
 *
 * @retval CR_SUCCESS The set has room for them, from now until it is
 *         destroyed.
 * @retval CR_RESULT_NET_INVALID_WAIT_SET `set` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_WAIT_SET_RESERVE_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS Every count is 0, a kind this
 *         build doesn't have is counted, or the members (their sum) are more
 *         than the set grows to (UINT32_MAX / 2), or their descriptors and
 *         the wake are more than a wait counts (UINT32_MAX less one).
 * @retval CR_RESULT_OUT_OF_MEMORY The room could not be made, or its size
 *         passes what a size_t holds (a 32-bit build); the set is as it
 *         was.
 */
CR_RESULT crnet_wait_set_reserve(
    crnet_wait_set set,
    const crnet_wait_set_reserve_desc *desc);

/*
 * ---------------------------------------------------------------------------
 * What the callers owe themselves (not this draft's to build)
 * ---------------------------------------------------------------------------
 *
 * - Code that is lent a set states the most members of each kind it keeps
 *   there at once, and whether a remake (a socket reclaimed, a reconnect,
 *   an advert's new name) makes the new object before destroying the old,
 *   which counts against the reserve; its lender sums them into its own.
 * - Code whose wait has no result for running out of memory gains one, or
 *   holds no libcurl member in a set it reserves.
 * - With crnet_wait_set_get_instance(), code handed a set can take the set
 *   alone and read the instance from it, so that no mismatch can be made.
 *
 * ---------------------------------------------------------------------------
 * Open questions
 * ---------------------------------------------------------------------------
 *
 * 1. Counts per kind (drafted) or descriptors (version 1, with figures the
 *    caller summed)? Recommendation: per kind. A descriptor count is how a
 *    backend is written, which a caller compiled against one release would
 *    get wrong under the next; a member count is the caller's own fact.
 * 2. A reserve as a limit too (an add past it refused with a result of its
 *    own, so nothing ever allocates after setup)? Recommendation: no. A
 *    caller that wants a limit counts its own members; refusing would make a
 *    set's capacity a policy of Carrottus's, and every caller today grows
 *    freely.
 * 3. libcurl's own arrays (an HTTP client's, an opening WebSocket's):
 *    reserve them too? Recommendation: not in this draft. They belong to
 *    the client or the socket, which may be in no set, and libcurl's count
 *    is unbounded; a size given at the client's create is its own draft once
 *    the Fedora run has measured how often they grow.
 */

#if __cplusplus
}
#endif

#endif // CRNET_WAIT_SET_DRAFT_H
