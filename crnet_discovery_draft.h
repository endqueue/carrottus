/*
 * crnet_discovery_draft.h - Carrottus Net: local service discovery
 * (DNS-SD over multicast DNS, "Bonjour"), through each system's own
 * service: advertising a service, browsing for one, and resolving a found
 * one to its host, port, TXT record and addresses.
 *
 * APPROVED by the user 2026-11-22, version 8 (SHA-256 in CRNET_TODO.md's
 * approval record). Not built,
 * not in any target.
 *   Version 1: the first.
 *   Version 2 (critique cycle 1, each library's own rules): no caller
 *   named in the text; each object opens the system's discovery library
 *   and finds its calls itself, so nothing is kept across objects (Q2
 *   and Q3 answered); every event kind has a 0 that is no event; the
 *   advert's ring stated; the Android test written as watchOS's; the
 *   creates' platform errors also to the message callback once card S10
 *   is built.
 *   Version 3 (critique cycle 2, platform truth): a FOUND names its
 *   interface where the system says it, 0 on Windows, and a resolver
 *   takes 0 for any interface [CHANGED]; iOS's permission while it is
 *   asked is an event of each object, PERMISSION_PENDING, and a type
 *   missing from NSBonjourServices kDNSServiceErr_NoAuth [CHANGED];
 *   Linux's creates block in D-Bus round trips to avahi-daemon, stated
 *   [CHANGED]; Windows' destroy of a registration not yet made cancels
 *   it with DnsServiceRegisterCancel [CHANGED]; RFC 6763's rules for
 *   names and the TXT bound corrected [CHANGED]; Android 17's local
 *   network permission and iOS's reclaimed connections stated; Q3's
 *   Windows versions corrected; Q5 answered. Sources cited where used.
 *   Version 4 (critique cycle 5, the callers): Q8 answered.
 *   Version 5 (critique cycle 6, failures): PERMISSION_PENDING renamed
 *   LOCAL_NETWORK_NOT_ALLOWED, Apple's one signal for asking and refused
 *   alike [CHANGED]; CR_RESULT_NET_SERVICE_NOT_ANSWERING for a resolve that
 *   timed out [ADDED]; a peer's malformed name dropped; the advert's ring's
 *   repeats folded; the wait set's rule for these objects said; tests.
 *   Version 6 (critique cycle 7, threads): Windows' callbacks' lock and
 *   ring stated and tested off Windows under TSan; Q6 answered.
 *   Version 7 (the critique's read of the whole): text later versions
 *   had left behind brought into line, every open question answered
 *   under itself, the specs agreeing (prompts/remote_drafts_critique_record.md,
 *   "Whole").
 *   Version 8 (the approval session, 2026-11-22): each object's own open
 *   of the system's library approved with two tests added: a create timed
 *   with the library loaded and not, and the last object destroyed then
 *   another created.
 * Written against include/crnet.h and include/crcore.h
 * as read on 2026-11-21 (carrottus version 0.4.0), the wait set
 * (src/net/crnetwaitset.c; src/net/crnetshared.h, "Wait set": members'
 * descriptors, timers and crnet_wait_set_notify from another thread), and
 * the macOS SDK's dns_sd.h (version 3111000005). Uses crnet_address from
 * crnet_interface_draft.h, included below. Asked for by a caller that
 * finds devices on the local network (cards X46, X47); the service types
 * ("_example._udp", say) are the caller's, never written here.
 *
 * Every declaration below is marked [ADDED], [CHANGED] or [REMOVED];
 * nothing unmarked changes. It compiles beside today's crnet.h, as its
 * siblings do (an [ADDED] result or struct type is a macro here only so
 * the file compiles). Checked with
 *   clang -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -I include -x c
 *   clang -fsyntax-only -std=c++17 -Wall -Wextra -Wpedantic -I include -x c++
 * with -DCARROTTUS_PLATFORM_APPLE=1, -DCARROTTUS_PLATFORM_WIN32=1,
 * -DCARROTTUS_PLATFORM_LINUX=1 and bare.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * A caller of card X46 has one device find another on the same network
 * without anyone typing an address. Every system already
 * runs a DNS-SD responder that does this, shared by every program on the
 * machine, answering for it while it sleeps (Apple's Bonjour Sleep Proxy),
 * and the one that the system's privacy rules govern (iOS's Local Network
 * prompt is about it). A Carrottus that sent its own multicast DNS packets
 * would be a second responder on port 5353 beside the system's, a protocol
 * implementation rather than a translation, and refused by iOS without a
 * multicast entitlement. So this translates the system's service, and
 * nothing more: what to advertise, what to look for, which of the found
 * to trust and which address to use are the caller's.
 *
 * ---------------------------------------------------------------------------
 * Which service on each platform, and why
 * ---------------------------------------------------------------------------
 *
 * | Platform         | Service                                              | How it reaches the caller's thread            |
 * |------------------|------------------------------------------------------|-----------------------------------------------|
 * | macOS, iOS,      | dns_sd (DNSServiceRegister, DNSServiceBrowse,        | each object's DNSServiceRefSockFD in the wait |
 * | iPadOS, tvOS     | DNSServiceResolve, DNSServiceGetAddrInfo), libSystem | set's poll(); DNSServiceProcessResult, which  |
 * |                  |                                                      | runs the reply's callback on the caller's     |
 * |                  |                                                      | thread, inside the caller's call              |
 * | Windows 10/11    | Windows' DNS-SD API in dnsapi (DnsServiceRegister,   | callbacks on a system thread copy into the    |
 * |                  | DnsServiceBrowse, DnsServiceResolve and their        | object's ring and wake its wait set           |
 * |                  | cancels), resolved at run time (Q3)                  | (crnet_wait_set_notify), as WinHTTP does      |
 * | Linux            | Avahi's client library (avahi-client) over D-Bus to  | Avahi's main loop is ours: an AvahiPoll whose |
 * |                  | avahi-daemon: AvahiEntryGroup, AvahiServiceBrowser,  | watches and timeouts are the wait set's       |
 * |                  | AvahiServiceResolver                                 | descriptors and timers, dispatched inside the |
 * |                  |                                                      | caller's call                                 |
 * | Android          | none in the NDK; NsdManager is Java (Q1)             | CRNET_EXT_DISCOVERY is 0 until Q1             |
 * | watchOS          | refused to ordinary applications, as sockets are     | CRNET_EXT_DISCOVERY is 0                      |
 *
 * Apple: dns_sd, not Network.framework. Network.framework's NWBrowser
 * browses and resolves, but its NWListener advertises only a listener of
 * its own (NWListener.service): it can't advertise the port of a BSD
 * socket, which is what Carrottus Net's TCP and UDP sockets are (the TCP
 * backend chose BSD sockets on Apple, crnetsockets.c, and UDP follows it).
 * And dns_sd fits Carrottus Net's rule exactly: each operation is a
 * descriptor to poll and a call that runs its callback on the thread that
 * makes it, so nothing comes from a thread of the system's and no lock or
 * ring is needed for it; Network.framework delivers on a dispatch queue.
 * dns_sd is libSystem's on every Apple platform, and the SDK on this Mac
 * marks none of the four calls deprecated. Apple's Bonjour for Windows
 * (dnssd.dll) has the same API but is not on Windows machines by default,
 * so Windows uses its own.
 *
 * Linux: Avahi, not systemd-resolved. Fedora, Ubuntu and Debian desktops
 * run avahi-daemon by default; resolved's mDNS is off by default on them,
 * and its D-Bus API resolves and registers but browses only through a
 * newer varlink interface. Avahi's compatibility dns_sd library
 * (libdns_sd from avahi-compat-libdns_sd) is not used: Avahi itself warns
 * that it is incomplete, and its own API takes our main loop.
 *
 * ---------------------------------------------------------------------------
 * The rules every platform is held to
 * ---------------------------------------------------------------------------
 *
 * - Pure translation. The caller supplies the service type, the instance
 *   name, the port, the TXT entries and the interfaces; nothing has a
 *   default. The domain is always "local." (link-local multicast DNS):
 *   wide-area DNS-SD differs on every system and is not offered (Q5).
 * - No thread, no callback. Each object's results are events, read by its
 *   `_next` call with a time limit like every wait in crnet.h, and each
 *   object can be in a wait set, which names it when it has events. The
 *   system's work goes on between the caller's calls only where the system
 *   does it by itself (Windows' threads, the daemons); dns_sd's and
 *   Avahi's replies are read inside the caller's calls.
 * - No rename. When the name an advert asked for is taken on the network,
 *   the advert ends with CR_RESULT_NET_SERVICE_NAME_CONFLICT and the
 *   caller chooses another name and makes a new advert. dns_sd would pick
 *   "Name (2)" by itself unless told not to (kDNSServiceFlagsNoAutoRename,
 *   passed); Avahi never renames (AVAHI_ENTRY_GROUP_COLLISION). Which name
 *   to fall back to is the caller's.
 * - Events are what the system reported, unfolded. The same instance may
 *   be FOUND more than once - once per interface on Apple and Linux, and
 *   on Linux once per IP protocol as well (Avahi browses IPv4 and IPv6
 *   apart) - and each FOUND is matched by one LOST, so a caller that
 *   counts per (name, interface) knows when the last copy has gone.
 *   [CHANGED] Windows' browse names no interface (its callback hands a
 *   DNS_RECORD list, which carries none: learn.microsoft.com,
 *   DNS_SERVICE_BROWSE_CALLBACK, read 2026-11-21), so there a FOUND comes
 *   once per name with `interface_index` 0, and a resolver takes 0 as
 *   "any interface" (DNS_SERVICE_RESOLVE_REQUEST's InterfaceIndex 0).
 *   macOS tested 2026-11-21: one FOUND on lo0 and one on en0 for one
 *   advert, and one LOST for each when it went.
 * - Every object keeps its events in a ring the caller sizes
 *   (`event_capacity`), allocated when it is made, so that nothing is
 *   allocated while events arrive (on Windows they arrive on the system's
 *   thread, where the caller's allocator is never called). When the ring
 *   is full the oldest unread event is kept and later ones are dropped,
 *   and the next event read is EVENTS_DROPPED: what the caller holds may
 *   be stale, and the remedy is the caller's (a new browser).
 * - Permission is reported as itself where the system says so
 *   (CR_RESULT_NET_DISCOVERY_DENIED), and a missing service as itself
 *   (CR_RESULT_NET_DISCOVERY_UNAVAILABLE): neither as a generic failure.
 * - TXT entries are "key=value", the key printable US-ASCII without '='
 *   (RFC 6763, section 6.4), the value UTF-8 without a NUL, each entry at
 *   most 255 bytes and the record at most CRNET_SERVICE_TXT_CAPACITY. That
 *   is narrower than RFC 6763 (which allows keys alone and binary values)
 *   because Windows' API takes the TXT record as two arrays of wide
 *   strings: the one form every system can register. A found TXT record
 *   is handed over as the system received it, in DNS wire form, since
 *   another device's may be anything RFC 6763 allows (Q4).
 *
 * ---------------------------------------------------------------------------
 * The caller's obligations on each platform (stated in the header)
 * ---------------------------------------------------------------------------
 *
 * - iOS and iPadOS 14 and later: the app's Info.plist has
 *   NSLocalNetworkUsageDescription (the prompt's text) and NSBonjourServices
 *   listing every type it browses or advertises ("_example._udp",
 *   "_example._tcp"). [CHANGED] Without the latter dns_sd refuses the type
 *   with kDNSServiceErr_NoAuth (-65555), CR_RESULT_NET_DISCOVERY_DENIED
 *   [unconfirmed (C2.12): Apple's forums say so, no documentation]. While
 *   the person hasn't yet allowed the local network, the operation waits,
 *   and dns_sd's callback says so with kDNSServiceErr_PolicyDenied
 *   (-65570) (TN3179, "Understanding local network privacy", read
 *   2026-11-21): that is a LOCAL_NETWORK_NOT_ALLOWED event of the object, not
 *   its end; its results follow if the person allows it. A backgrounded
 *   app asking for the first time is denied with no prompt (TN3179).
 *   macOS 15 and later has the same Local Network privacy for apps that
 *   aren't the system's.
 * - iOS and iPadOS: a suspended app's connection to mDNSResponder may be
 *   reclaimed, as its sockets may (crnet_udp_draft.h): the object's next
 *   call is CR_RESULT_NET_DISCOVERY_FAILED, and the caller makes a new
 *   one.
 * - Android 17 (API 37): ACCESS_LOCAL_NETWORK governs UDP and TCP on the
 *   local network (developer.android.com, read 2026-11-21); this part is
 *   0 on Android (Q1), and NsdManager's picker is the one path that
 *   needs no such permission, which Q1 weighs.
 * - Linux: libavahi-client installed and avahi-daemon running
 *   (CR_RESULT_NET_DISCOVERY_UNAVAILABLE otherwise, AVAHI_ERR_NO_DAEMON), and a firewall that lets mDNS in
 *   (Fedora's default zone does).
 * - Windows: Windows 10 or 11 with the DNS-SD calls in dnsapi.dll
 *   (CR_RESULT_NET_DISCOVERY_UNAVAILABLE otherwise, Q3), and Windows
 *   Defender Firewall letting mDNS (UDP 5353) in on the network's profile
 *   (a Public network blocks it by default).
 *
 * ---------------------------------------------------------------------------
 * Known problems, each to be confirmed when built
 * ---------------------------------------------------------------------------
 *
 * - Windows: which host name an advert carries. DnsServiceConstructInstance
 *   needs one; the backend passes this machine's DNS host name
 *   (GetComputerNameExW, ComputerNameDnsHostname) with ".local", and no
 *   address, believing that Windows' responder answers for its own name.
 *   Untried.
 * - Windows: whether DnsServiceBrowse reports an instance going away (a
 *   record with a TTL of 0 is believed to be its "goodbye"), and whether it
 *   reports name conflicts on a register at all. If it doesn't, LOST and
 *   CR_RESULT_NET_SERVICE_NAME_CONFLICT never come on Windows, which the
 *   header will then say.
 * - Windows: an instance name holding '.' or '\', which the full name
 *   given to DnsServiceConstructInstance has to escape, untried.
 * - Windows: destroying an advert, browser or resolver cancels it
 *   (an advert not yet REGISTERED by DnsServiceRegisterCancel on the
 *   request's DNS_SERVICE_CANCEL, one registered by DnsServiceDeRegister,
 *   whose callback comes "when the deregistration is completed";
 *   DnsServiceBrowseCancel, which brings "one further invocation of the
 *   browse callback, with status ERROR_CANCELLED"; DnsServiceResolveCancel)
 *   and waits for that callback, after which no callback can reach the
 *   object; so a destroy may block for as long as the system takes to
 *   call it (believed milliseconds; learn.microsoft.com, the three
 *   cancels' pages, read 2026-11-21). Whether DnsServiceResolveCancel
 *   always calls back is undocumented [unconfirmed (C2.10)]. Kept as a
 *   stated known problem: a destroy that returned first would leave a
 *   callback on the system's thread with no object, or the caller's
 *   allocator called there to free one.
 * - [CHANGED] Linux: every create, and an advert's commit, makes blocking
 *   D-Bus round trips to avahi-daemon: avahi_client_new() makes three
 *   (dbus_connection_send_with_reply_and_block, libdbus' default time
 *   limit), avahi_service_browser_new() and the resolver's and entry
 *   group's calls one or more each (avahi-client/client.c and browser.c,
 *   read 2026-11-21). Avahi's client has no asynchronous form of them. A
 *   local daemon answers in well under a millisecond; a daemon that hangs
 *   holds the create for libdbus' limit (25 s), with no abort. The
 *   creates' "returns at once" is therefore "returns after the daemon's
 *   answers" on Linux, the one exception to Carrottus Net's time limits,
 *   as the resolver's lookup is.
 * - Linux: each object is one avahi-client, one D-Bus connection (Q6).
 * - [CHANGED] Linux and Windows: each object opens the system's library
 *   itself when it is created (dlopen of libavahi-client.so.3 and
 *   libavahi-common.so.3; LoadLibraryExW of dnsapi.dll from the system
 *   directory, LOAD_LIBRARY_SEARCH_SYSTEM32), finds the calls it needs
 *   (dlsym, GetProcAddress) into a table in the object, and closes the
 *   library at its destroy. Both systems count opens, so the second
 *   object's open is a lookup. No handle or table outlives its object and
 *   none is kept in the instance: Carrottus holds no global state, and
 *   the instance holds none for discovery.
 * - Every platform: a resolver of a service that has gone reports nothing
 *   ever on dns_sd (it waits for the service to come back) and fails on
 *   Avahi and Windows (their resolve has a time-out of the system's own).
 *   A caller with a deadline keeps it, as for every wait here.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * The platforms: dns_sd.h (DNSServiceRegister with
 * kDNSServiceFlagsNoAutoRename, DNSServiceBrowse and kDNSServiceFlagsAdd,
 * DNSServiceResolve, DNSServiceGetAddrInfo, DNSServiceRefSockFD,
 * DNSServiceProcessResult, kDNSServiceFlagsShareConnection,
 * kDNSServiceErr_PolicyDenied, _NameConflict, _ServiceNotRunning;
 * TXTRecordRef); Network.framework (NWBrowser and its browse results'
 * added/removed/changed, NWListener.service and its nameChangeHandler,
 * NWTXTRecord), looked at and not taken for the reason above; Windows'
 * DNS-SD API (DnsServiceConstructInstance, DnsServiceRegister and
 * DNS_SERVICE_REGISTER_REQUEST, DnsServiceBrowse and
 * DNS_SERVICE_BROWSE_REQUEST, DnsServiceResolve, the three cancels,
 * DnsServiceFreeInstance, DnsRecordListFree; documented as Windows 10
 * and later); Avahi (avahi_client_new and AvahiClientState,
 * AvahiPoll's watch_new/timeout_new, avahi_entry_group_add_service_strlst
 * and _commit, AVAHI_ENTRY_GROUP_COLLISION, avahi_service_browser_new and
 * AVAHI_BROWSER_NEW/REMOVE/ALL_FOR_NOW/FAILURE,
 * avahi_service_resolver_new, avahi_string_list_serialize); Android's
 * NsdManager (registerService, discoverServices, resolveService and API
 * 34's registerServiceInfoCallback; listeners called on the system's
 * executor).
 *
 * Industry practice: Apple's NWBrowser/NWListener (events added, removed
 * and changed, a rename handler the caller may refuse); python-zeroconf
 * and Rust's mdns-sd (a responder of their own in the library, the path
 * not taken, above); Qt's QZeroConf and KDE's KDNSSD (a wrapper over
 * dns_sd, Avahi and Windows each, events by signal: the same split of
 * backends as here); Avahi's own examples (a collision handled by the
 * caller choosing avahi_alternative_service_name()); libuv and ASIO have
 * no discovery; SDL_net 3 has none.
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * No break. Added: CRNET_EXT_DISCOVERY; the capacities;
 * CRNET_DISCOVERY_SCOPE; crnet_service_txt_entry; crnet_service_advert,
 * its desc, event and calls (create, next, abort, get_error, destroy);
 * crnet_service_browser, its desc, event and calls (the same five);
 * crnet_service_resolver, its desc, event and calls (the same five; the
 * TXT record comes in the event, so there is no getter); the event kinds;
 * the wait
 * set's add and remove for each of the three. Six struct types
 * (CR_STRUCT_TYPE_NET_SERVICE_ADVERT_DESC 3010 to
 * CR_STRUCT_TYPE_NET_SERVICE_RESOLVE_EVENT 3015) and seven results
 * (CR_RESULT_NET_INVALID_SERVICE_ADVERT 3040 to
 * CR_RESULT_NET_SERVICE_NAME_CONFLICT 3046), each named in
 * cr_result_str(). Text changes, no signature: the overview's table gains
 * a row; "Which parts this build has" gains CRNET_EXT_DISCOVERY; the
 * aborting group's list and the wait set's member list gain the three
 * objects. The Linux build needs Avahi's headers; the library is opened
 * at run time (Q2).
 *
 * ---------------------------------------------------------------------------
 * Tests (crnet_test, a new part "discovery"; the misuse harness)
 * ---------------------------------------------------------------------------
 *
 * Each case seen failing first against a stub. The test advertises and
 * browses on the machine itself, over every interface, with a type of its
 * own ("_crnettest._udp") and a name with the process id in it, so two
 * runs on one network don't meet.
 * - The round: an advert REGISTERED; a browser of the type FOUND it (on
 *   at least one interface, loopback's included where the system reports
 *   one); a resolver of it gives SERVICE with this machine's host name,
 *   the port and the TXT entries back in wire form, and at least one
 *   ADDRESS_ADDED, which crnet_interface_enumerate() lists; the advert
 *   destroyed, the browser LOST it as many times as it FOUND it.
 * - Conflict: a second advert of the same name and type on the same
 *   machine ends with CR_RESULT_NET_SERVICE_NAME_CONFLICT on dns_sd and
 *   Avahi (on Windows whatever it does is recorded, then the header says
 *   it).
 * - Desc checks: types that aren't "_x._udp" or "_x._tcp" (no leading
 *   underscore, 16 characters, "_x._sctp"), names of 0 and 64 bytes, TXT
 *   entries without '=', with a key of a control character, of 256 bytes,
 *   a record over CRNET_SERVICE_TXT_CAPACITY; scope NONE; a resolver's
 *   event_capacity 0; [CHANGED] an instance name with a control
 *   character, a type of "_-x._udp" or "_x-._udp"; a resolver of
 *   interface 0 resolving on any.
 * - [CHANGED] LOCAL_NETWORK_NOT_ALLOWED and NoAuth can't be met by crnet_test
 *   (Terminal and SSH are exempt on macOS, the simulator has no local
 *   network privacy: TN3179): a device test app's cases, listed when
 *   built.
 * - [CHANGED] (cycle 6) An injected out-of-memory at each create (its
 *   ring), nothing left; ALREADY_IN_WAIT_SET and INVALID_INSTANCE; the
 *   resolver's EVENTS_DROPPED; DISCOVERY_FAILED after avahi-daemon is
 *   restarted mid-browse (skipped, 77, as the stopped-daemon test); a
 *   resolve of a vanished instance NOT_ANSWERING on Linux and Windows; a
 *   malformed name from a fake responder dropped.
 * - The ring: a browser with event_capacity 1 and twenty adverts gives
 *   EVENTS_DROPPED, then events again. [CHANGED] An advert's ring: its
 *   REGISTERED and a conflict both handed over, never dropped. A zeroed
 *   event of each kind reads NONE.
 * - [CHANGED] Two browsers and a resolver at once on Linux and Windows
 *   (each opens the library, each destroy closes it, the last leaving it
 *   closed: dladdr / GetModuleHandleExW say so); a create where the
 *   library is missing (a test build pointing the name elsewhere) gives
 *   CR_RESULT_NET_DISCOVERY_UNAVAILABLE.
 * - [ADDED, approval session] A create timed on Linux and Windows, with
 *   another object holding the library open and with none (the first
 *   load), Debug and Release, the numbers kept in CRNET_TODO.md; and the
 *   last object destroyed, then another created, twenty times in a row
 *   (libdbus unloaded and loaded again under Avahi, under -asan and
 *   -tsan, nothing reported).
 * - Time limits and aborts: next() with 50 ms and nothing to read is
 *   TIMED_OUT; one with CRNET_WAIT_FOREVER ended by the object's abort
 *   from another thread (under -tsan; on Windows with a callback arriving
 *   meanwhile).
 * - Wait set: each object named once it has an event, drained to
 *   TIMED_OUT; on Linux, a set waiting with nothing else while Avahi's
 *   D-Bus timer runs.
 * - Missing service: Linux with avahi-daemon stopped (a test that skips,
 *   77, unless run as the user who may stop it) gives
 *   CR_RESULT_NET_DISCOVERY_UNAVAILABLE from each create.
 * - Misuse: every call with zeroes and with a destroyed object (three
 *   factories in tests/misuse/make_misuse_tests.py, CR_STALE after each
 *   null check, each quarantined by its instance).
 * - Per platform: macOS (plain, Release, ASan, TSan; and a run with the
 *   Mac's Wi-Fi off, loopback only); the iOS simulator needs an app with an
 *   Info.plist for NSBonjourServices, which `simctl spawn` of a bare
 *   executable lacks, so its case is run from a small test app or skipped
 *   and listed; Windows 11 on the ThinkPad (Debug, Release, -asan) and,
 *   by hand, an iPhone test app or the Mac finding the ThinkPad's advert and the
 *   other way; Fedora 44 (GCC and clang, -asan, -tsan) and the same
 *   cross-machine check by hand; Android once Q1 is decided. The
 *   cross-machine checks are a person's, listed in
 *   prompts/host_revamp_test.md when built.
 */

#ifndef CRNET_DISCOVERY_DRAFT_H
#define CRNET_DISCOVERY_DRAFT_H

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

/** [ADDED] A service advert argument is NULL. */
#define CR_RESULT_NET_INVALID_SERVICE_ADVERT ((CR_RESULT)3040)
/** [ADDED] A service browser argument is NULL. */
#define CR_RESULT_NET_INVALID_SERVICE_BROWSER ((CR_RESULT)3041)
/** [ADDED] A service resolver argument is NULL. */
#define CR_RESULT_NET_INVALID_SERVICE_RESOLVER ((CR_RESULT)3042)
/** [ADDED] The system's discovery service isn't there: avahi-daemon not
 * running, dnsapi without the DNS-SD calls, mDNSResponder unreachable. */
#define CR_RESULT_NET_DISCOVERY_UNAVAILABLE ((CR_RESULT)3043)
/** [ADDED] The system refused this program discovery, or this type:
 * iOS's NSBonjourServices or Local Network privacy
 * (kDNSServiceErr_PolicyDenied), Avahi's D-Bus policy. */
#define CR_RESULT_NET_DISCOVERY_DENIED ((CR_RESULT)3044)
/** [ADDED] The system's discovery service failed the operation, or went
 * away during it (the daemon restarted); the object's _get_error has the
 * system's code. The object is over: only destroy is left. */
#define CR_RESULT_NET_DISCOVERY_FAILED ((CR_RESULT)3045)
/** [ADDED] The name an advert asked for is held by another service of
 * the same type on the network. The advert is over. */
#define CR_RESULT_NET_SERVICE_NAME_CONFLICT ((CR_RESULT)3046)
/** [ADDED] A resolve the system gave up on because the instance didn't
 * answer (Avahi's and Windows' own time-out): the caller retries or shows
 * "not answering", where DISCOVERY_FAILED means the service itself failed
 * and every object is made again. dns_sd never gives it (its resolve waits).
 * The next free value when built (3048 here, after UDP's ACCESS_DENIED). */
#define CR_RESULT_NET_SERVICE_NOT_ANSWERING ((CR_RESULT)3048)

/** [ADDED] crnet_service_advert_desc (CR_STRUCT_BASE). */
#define CR_STRUCT_TYPE_NET_SERVICE_ADVERT_DESC ((CR_STRUCT_TYPE)3010)
/** [ADDED] crnet_service_advert_event (CR_STRUCT_BASE_OUT). */
#define CR_STRUCT_TYPE_NET_SERVICE_ADVERT_EVENT ((CR_STRUCT_TYPE)3011)
/** [ADDED] crnet_service_browser_desc (CR_STRUCT_BASE). */
#define CR_STRUCT_TYPE_NET_SERVICE_BROWSER_DESC ((CR_STRUCT_TYPE)3012)
/** [ADDED] crnet_service_browse_event (CR_STRUCT_BASE_OUT). */
#define CR_STRUCT_TYPE_NET_SERVICE_BROWSE_EVENT ((CR_STRUCT_TYPE)3013)
/** [ADDED] crnet_service_resolver_desc (CR_STRUCT_BASE). */
#define CR_STRUCT_TYPE_NET_SERVICE_RESOLVER_DESC ((CR_STRUCT_TYPE)3014)
/** [ADDED] crnet_service_resolve_event (CR_STRUCT_BASE_OUT). */
#define CR_STRUCT_TYPE_NET_SERVICE_RESOLVE_EVENT ((CR_STRUCT_TYPE)3015)

/* ======================================================================== */
/* crnet.h, text that changes [CHANGED]                                     */
/* ======================================================================== */

/* [CHANGED] The overview's table gains a row:
 *
 *     | Service discovery | DNS-SD API (dnsapi) | Avahi | dns_sd |
 *
 * [CHANGED] "Which parts this build has" gains CRNET_EXT_DISCOVERY, a
 * column in its table (Windows 1, Linux 1, Android 0, Apple other than
 * watchOS 1, watchOS 0).
 *
 * [CHANGED] "Aborting": the objects with an `_abort` gain
 * crnet_service_advert, crnet_service_browser and crnet_service_resolver.
 *
 * [CHANGED] "Threads": the bullet on the platform's own threads gains
 * Windows' DNS-SD callbacks, which copy into the object's ring under its
 * lock and wake its set, and call nothing of the caller's. [CHANGED, cycle
 * 7] The lock: Windows doesn't say one operation's callbacks come on one
 * thread, so the ring has many writers and a lock (an SRWLOCK held only for
 * a fixed-size copy, at most a resolve event's 1.6 KB, nothing allocated
 * under it), with crnet_wait_set_notify called after it is released; the
 * reader, the caller's _next, takes the same lock for its copy out. The
 * ring's code is platform-neutral, so a test drives it from a fake system
 * thread on the Mac and Linux under -tsan (Windows has no TSan), and
 * Windows runs a stress test with the Debug overlap check.
 */

/* ======================================================================== */
/* crnet.h, Which parts this build has [ADDED]                              */
/* ======================================================================== */

/**
 * @def CRNET_EXT_DISCOVERY
 * @brief [ADDED] 1 when this build has service discovery (adverts,
 * browsers, resolvers) and a wait set's calls for them; 0 when it has
 * none. 0 on watchOS, which refuses it to ordinary applications, and on
 * Android, whose discovery service is Java only (the draft's Q1).
 * [CHANGED] An Android build is told by the system's own macro,
 * __ANDROID__ (which the NDK's compiler always defines), as a watchOS build
 * is by TargetConditionals.h, because no CARROTTUS_PLATFORM_ macro names
 * Android yet; once card S47's platform draft defines one, the test reads
 * it, with __ANDROID__ kept as the same guard TargetConditionals.h is for
 * watchOS.
 */
#if !defined(__ANDROID__) && (CARROTTUS_PLATFORM_WIN32 || CARROTTUS_PLATFORM_LINUX || (CARROTTUS_PLATFORM_APPLE && !CARROTTUS_PLATFORM_WATCHOS))
#define CRNET_EXT_DISCOVERY 1
#else
#define CRNET_EXT_DISCOVERY 0
#endif

/* crnet_address is declared under CRNET_EXT_TCP (crnet_interface_draft.h). */
#if CRNET_EXT_DISCOVERY && !CRNET_EXT_TCP
#error "Carrottus Net: CRNET_EXT_DISCOVERY needs crnet_address, declared with CRNET_EXT_TCP."
#endif

#if CRNET_EXT_DISCOVERY

/* ======================================================================== */
/* crnet.h, Service discovery [ADDED]                                       */
/* ======================================================================== */

/**
 * @defgroup crnet_discovery Service discovery
 * @ingroup crnet
 * @brief [ADDED] DNS-SD over multicast DNS on the local network, through
 * the system's own service: dns_sd on Apple, Windows' DNS-SD API, Avahi on
 * Linux. Known problems: [CHANGED] on Linux a create blocks in avahi-daemon's
 * D-Bus round trips, with no limit of the caller's; Windows' browse names
 * no interface, and its reporting of departures and name conflicts is
 * untried; a destroy on Windows waits for the system's cancel callback;
 * on iOS a suspended app's discovery objects may need making again.
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
 * CR_RESULT_NET_SERVICE_NAME_CONFLICT.
 *
 * What the caller must give the system, per platform:
 *
 * - iOS and iPadOS 14 and later: NSLocalNetworkUsageDescription and
 *   NSBonjourServices (every type browsed or advertised) in the app's
 *   Info.plist, and the person's permission, asked by the system at first
 *   use. A type missing from NSBonjourServices is
 *   CR_RESULT_NET_DISCOVERY_DENIED; while the person is being asked,
 *   each object hands over a LOCAL_NETWORK_NOT_ALLOWED event and waits [CHANGED].
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

/** @brief [ADDED] Room for an instance name, with its terminator: a DNS
 * label is at most 63 bytes. */
#define CRNET_SERVICE_NAME_CAPACITY 64
/** @brief [ADDED] Room for a service type, with its terminator:
 * "_" + at most 15 characters + "._udp" or "._tcp" (RFC 6335). */
#define CRNET_SERVICE_TYPE_CAPACITY 32
/** @brief [ADDED] Room for a host name, with its terminator: a DNS name is
 * at most 255 bytes. */
#define CRNET_SERVICE_HOST_CAPACITY 256
/** @brief [ADDED] The largest TXT record an advert takes, and the most of
 * a found one a resolve event holds, in bytes of DNS wire form. [CHANGED]
 * RFC 6763's section 6.2 calls a TXT record over 1300 bytes "NOT
 * RECOMMENDED" (its one-packet bound, section 6.1, is about 8900), so an
 * advert keeps to it; a found one may be larger, and is then cut and said
 * so (txt_truncated). */
#define CRNET_SERVICE_TXT_CAPACITY 1300

/**
 * @brief [ADDED] Which interfaces an advert or a browser works on. No value
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
 * @brief [ADDED] One TXT entry of an advert, as "key=value".
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

/** @brief [ADDED] Opaque handle to one advertised service instance. Its
 * event ring holds three, made at its create [CHANGED]: an advert has at
 * most three events to hand over, LOCAL_NETWORK_NOT_ALLOWED, REGISTERED and then
 * the failure that ends it, so it needs no `event_capacity`; a repeat of
 * either (a permission withdrawn and given again in Settings) replaces the
 * unread one of its kind, so it never drops one. */
typedef struct crnet_service_advert_t *crnet_service_advert;

/** @brief [ADDED] What crnet_service_advert_create() advertises. */
typedef struct crnet_service_advert_desc {
	/** `type` is CR_STRUCT_TYPE_NET_SERVICE_ADVERT_DESC; `next` is NULL
	 * (nothing is chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** The instance name, the one people see ("Nathan's Mac"): UTF-8, 1 to
	 * 63 bytes, null-terminated, [CHANGED] no ASCII control character
	 * (0x00 to 0x1F, 0x7F: RFC 6763, section 4.1.1). Copied. */
	const char *name;
	/** The service type, "_name._udp" or "_name._tcp", the name 1 to 15
	 * letters, digits and hyphens, [CHANGED] beginning and ending with a
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

/** @brief [ADDED] What an advert's event says. */
typedef enum CRNET_SERVICE_ADVERT_EVENT_KIND {
	/** [CHANGED] No event: never handed over, so a zeroed event reads as
	 * none. */
	CRNET_SERVICE_ADVERT_EVENT_NONE = 0,
	/** The system's service has the instance registered and answers for
	 * it; `name` is the one asked for (it is never changed). Comes once. */
	CRNET_SERVICE_ADVERT_EVENT_REGISTERED = 1,
	/** [ADDED] [CHANGED] The system doesn't allow this app the local network
	 * now: dns_sd's kDNSServiceErr_PolicyDenied while the operation waits
	 * (iOS, iPadOS, macOS 15 and later), the one signal Apple gives whether
	 * the person is being asked or has said no, so a caller shows "Allow
	 * the local network in Settings" at once. Never on Windows or Linux.
	 * The advert goes on: REGISTERED follows if the person allows it. */
	CRNET_SERVICE_ADVERT_EVENT_LOCAL_NETWORK_NOT_ALLOWED = 2,
} CRNET_SERVICE_ADVERT_EVENT_KIND;

/** @brief [ADDED] One event of an advert, filled by
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
 * @brief [ADDED] Begins advertising one service instance on this machine:
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
 * @brief [ADDED] Hands over the advert's next event, waiting for one no
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
 * @brief [ADDED] Ends a wait in crnet_service_advert_next(), and every call
 * after it. The instance stays advertised until the advert is destroyed.
 * See @ref crnet_aborting "Aborting".
 *
 * Thread: may overlap any call on the advert but its destroy.
 *
 * @param[in] advert The advert to abort.
 */
void crnet_service_advert_abort(crnet_service_advert advert);

/**
 * @brief [ADDED] Reads what the system reported for the advert's most recent
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
 * @brief [ADDED] Stops advertising and frees the advert: the system's
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

/** @brief [ADDED] Opaque handle to one browse for one service type. */
typedef struct crnet_service_browser_t *crnet_service_browser;

/** @brief [ADDED] What crnet_service_browser_create() looks for. */
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

/** @brief [ADDED] What a browse event says. */
typedef enum CRNET_SERVICE_BROWSE_EVENT_KIND {
	/** [CHANGED] No event: never handed over. */
	CRNET_SERVICE_BROWSE_EVENT_NONE = 0,
	/** An instance of the type is there, on `interface_index` (0 on
	 * Windows, which doesn't say [CHANGED]). dns_sd: kDNSServiceFlagsAdd;
	 * Avahi: AVAHI_BROWSER_NEW; Windows: a PTR record with a TTL. */
	CRNET_SERVICE_BROWSE_EVENT_FOUND = 1,
	/** One FOUND of before has gone: the same name and interface.
	 * dns_sd: a reply without kDNSServiceFlagsAdd; Avahi:
	 * AVAHI_BROWSER_REMOVE; Windows: believed a TTL of 0. */
	CRNET_SERVICE_BROWSE_EVENT_LOST = 2,
	/** Events were dropped because the ring was full: what the caller
	 * holds may be stale. `name` is empty. */
	CRNET_SERVICE_BROWSE_EVENT_EVENTS_DROPPED = 3,
	/** [ADDED] As the advert's: waiting for the person to allow the local
	 * network; finds follow if allowed. `name` is empty. */
	CRNET_SERVICE_BROWSE_EVENT_LOCAL_NETWORK_NOT_ALLOWED = 4,
} CRNET_SERVICE_BROWSE_EVENT_KIND;

/** @brief [ADDED] One event of a browser, filled by
 * crnet_service_browser_next(). */
typedef struct crnet_service_browse_event {
	/** `type` is CR_STRUCT_TYPE_NET_SERVICE_BROWSE_EVENT; `next` is NULL.
	 * The caller sets the type; Carrottus Net fills in the rest. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE_OUT base;
	/** What happened. */
	CRNET_SERVICE_BROWSE_EVENT_KIND kind;
	/** The instance's name, UTF-8, null-terminated, as another device
	 * advertised it: not to be trusted, and only a name. [CHANGED] A name
	 * another device sends that no advert could carry (over 63 bytes, not
	 * UTF-8, a control character) is dropped, never handed over, so every
	 * name handed over is one a resolver's desc takes; the browse reader is
	 * fuzzed. */
	char name[CRNET_SERVICE_NAME_CAPACITY];
	/** The interface it was seen on: what a resolver of it takes. 0 where
	 * the system doesn't say (Windows) [CHANGED]. */
	uint32_t interface_index;
} crnet_service_browse_event;

/**
 * @brief [ADDED] Begins browsing for one service type: DNSServiceBrowse,
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
 * @brief [ADDED] Hands over the browser's next event, oldest first, waiting
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
 * @brief [ADDED] Ends a wait in crnet_service_browser_next(), and every
 * call after it. See @ref crnet_aborting "Aborting".
 *
 * Thread: may overlap any call on the browser but its destroy.
 *
 * @param[in] browser The browser to abort.
 */
void crnet_service_browser_abort(crnet_service_browser browser);

/**
 * @brief [ADDED] Reads what the system reported for the browser's most
 * recent failure. See @ref crnet_error_detail "Error detail".
 *
 * @param[in]  browser   The browser to ask. NULL gives an empty record.
 * @param[out] out_error Receives the record.
 */
void crnet_service_browser_get_error(
    crnet_service_browser browser,
    crnet_error *out_error);

/**
 * @brief [ADDED] Stops browsing and frees the browser and its unread
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

/** @brief [ADDED] Opaque handle to one resolve of one found instance. Not
 * a crnet_resolver, which looks up a host name. */
typedef struct crnet_service_resolver_t *crnet_service_resolver;

/** @brief [ADDED] What crnet_service_resolver_create() resolves: an
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
	 * `interface_index`); [CHANGED] 0 for any interface, as a Windows
	 * FOUND gives it (dns_sd's kDNSServiceInterfaceIndexAny, Avahi's
	 * AVAHI_IF_UNSPEC). */
	uint32_t interface_index;
	/** Which addresses of its host to report: IPv4, IPv6, or ANY for both. */
	CRNET_ADDRESS_FAMILY family;
	/** How many unread events the resolver keeps; not 0. */
	uint32_t event_capacity;
} crnet_service_resolver_desc;

/** @brief [ADDED] What a resolve event says. */
typedef enum CRNET_SERVICE_RESOLVE_EVENT_KIND {
	/** [CHANGED] No event: never handed over. */
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
	/** [ADDED] As the advert's: waiting for the person to allow the local
	 * network. */
	CRNET_SERVICE_RESOLVE_EVENT_LOCAL_NETWORK_NOT_ALLOWED = 5,
} CRNET_SERVICE_RESOLVE_EVENT_KIND;

/** @brief [ADDED] One event of a resolver, filled by
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
 * @brief [ADDED] Begins resolving one found instance: [CHANGED]
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
 * @brief [ADDED] Hands over the resolver's next event, oldest first,
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
 * @retval CR_RESULT_NET_SERVICE_NOT_ANSWERING [ADDED] On Avahi and Windows,
 *         their own time-out for an instance that didn't answer; the
 *         resolver is over, the system well.
 * @retval CR_RESULT_NET_DISCOVERY_FAILED The system's service failed the
 *         resolve or went away; the resolver is over.
 *         crnet_service_resolver_get_error() has its code.
 */
CR_RESULT crnet_service_resolver_next(
    crnet_service_resolver resolver,
    uint32_t timeout_ms,
    crnet_service_resolve_event *out_event);

/**
 * @brief [ADDED] Ends a wait in crnet_service_resolver_next(), and every
 * call after it. See @ref crnet_aborting "Aborting".
 *
 * Thread: may overlap any call on the resolver but its destroy.
 *
 * @param[in] resolver The resolver to abort.
 */
void crnet_service_resolver_abort(crnet_service_resolver resolver);

/**
 * @brief [ADDED] Reads what the system reported for the resolver's most
 * recent failure. See @ref crnet_error_detail "Error detail".
 *
 * @param[in]  resolver  The resolver to ask. NULL gives an empty record.
 * @param[out] out_error Receives the record.
 */
void crnet_service_resolver_get_error(
    crnet_service_resolver resolver,
    crnet_error *out_error);

/**
 * @brief [ADDED] Stops resolving and frees the resolver and its unread
 * events. Known problem: on Windows it waits for
 * DnsServiceResolveCancel's callback when the resolve hasn't ended.
 *
 * @param[in] resolver The resolver to destroy. NULL is taken and nothing
 *                     is done with it.
 */
void crnet_service_resolver_destroy(crnet_service_resolver resolver);

/** @} */

/* ======================================================================== */
/* crnet.h, Wait set [ADDED]                                                */
/* ======================================================================== */

/**
 * @brief [ADDED] Adds an advert to the set. It is named
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
 *         another: [CHANGED] unlike a socket, a discovery object is in one
 *         set at most, since on Windows the system's callback wakes the one
 *         set it was added to.
 * @retval CR_RESULT_OUT_OF_MEMORY The set could not grow.
 */
CR_RESULT crnet_wait_set_add_service_advert(
    crnet_wait_set set,
    crnet_service_advert advert,
    void *user);

/** @brief [ADDED] Takes an advert out of the set. One not in it is ignored;
 * so is NULL for either argument. */
void crnet_wait_set_remove_service_advert(crnet_wait_set set, crnet_service_advert advert);

/**
 * @brief [ADDED] Adds a browser to the set, named CRNET_WAIT_FOR_INCOMING
 * when crnet_service_browser_next() has an event or a failure. Results and
 * arguments as crnet_wait_set_add_service_advert(), with
 * CR_RESULT_NET_INVALID_SERVICE_BROWSER for a NULL `browser`.
 */
CR_RESULT crnet_wait_set_add_service_browser(
    crnet_wait_set set,
    crnet_service_browser browser,
    void *user);

/** @brief [ADDED] Takes a browser out of the set. One not in it is ignored;
 * so is NULL for either argument. */
void crnet_wait_set_remove_service_browser(crnet_wait_set set, crnet_service_browser browser);

/**
 * @brief [ADDED] Adds a resolver to the set, named CRNET_WAIT_FOR_INCOMING
 * when crnet_service_resolver_next() has an event or a failure. Results
 * and arguments as crnet_wait_set_add_service_advert(), with
 * CR_RESULT_NET_INVALID_SERVICE_RESOLVER for a NULL `resolver`.
 */
CR_RESULT crnet_wait_set_add_service_resolver(
    crnet_wait_set set,
    crnet_service_resolver resolver,
    void *user);

/** @brief [ADDED] Takes a resolver out of the set. One not in it is
 * ignored; so is NULL for either argument. */
void crnet_wait_set_remove_service_resolver(crnet_wait_set set, crnet_service_resolver resolver);

#endif // CRNET_EXT_DISCOVERY

/*
 * ---------------------------------------------------------------------------
 * Open questions
 * ---------------------------------------------------------------------------
 *
 * 1. Android. The NDK has no discovery API (no dns_sd in its stable
 *    libraries); NsdManager is Java, its listeners Java interfaces called
 *    on the system's executor. Options: (a) through JNI with the JavaVM
 *    and a Context the caller hands crnet_instance_desc in a chained
 *    struct - the route card S47's Net card already names for OkHttp (its
 *    option c) - with a small Java listener class Carrottus ships as
 *    source for the caller's APK, calling back into registered natives
 *    that fill the ring and wake the set, as Windows' callbacks do;
 *    (b) CRNET_EXT_DISCOVERY stays 0 on Android, and the caller does NSD
 *    in its own Java or Kotlin and needs nothing here; (c) a multicast DNS responder of Carrottus's own
 *    over UDP: refused, a protocol implementation and a second responder.
 *    Recommendation: decide it with S47's Net card, the same way: (a) if
 *    that card takes the JavaVM (one route into Java for all of Carrottus
 *    Net), else (b). 0 until then, as drafted, which is what 02b's
 *    precedent for Input (no Java, revisited when a caller asks) gives.
 *    Answered (whole read): as recommended, with card S47's Net card (its question, not these cards'); 0 until then.
 * 2. Linux: link avahi-client at build time, or open it at run time?
 *    Answered C1.9: at run time, by each object at its create (above), with
 *    Avahi's headers at build time; CR_RESULT_NET_DISCOVERY_UNAVAILABLE
 *    where it is missing. Discovery is optional to a program in a way HTTP
 *    isn't, a minimal system (a server, a container) still runs everything
 *    else, and a per-object open keeps no state between objects.
 * 3. Windows: link dnsapi's DNS-SD calls or find them at run time
 *    (GetProcAddress at the first create)? [CHANGED] Microsoft's pages
 *    (DnsServiceRegister, _Browse, _Resolve, _ConstructInstance, the
 *    cancels, read 2026-11-21) say "Windows 10 [desktop apps only]" with
 *    no build, while the calls are reported from Windows 10 1809, and a
 *    static import of a missing export stops carrottus from loading at
 *    all. Answered C1.9:
 *    run time, by each object at its create (above),
 *    CR_RESULT_NET_DISCOVERY_UNAVAILABLE where missing; the ThinkPad's
 *    Windows 11 has them either way, and which releases do is recorded
 *    when built.
 * 4. TXT out: raw wire form (drafted) or parsed entries? Recommendation:
 *    wire form, plus, if the caller wants, a stateless walk
 *    (crnet_service_txt_next(txt, size, &offset, &key, &value)) added
 *    later: walking RFC 6763's format is the same on every platform, but it
 *    is a format, not a translation, so it stays out until asked for.
 *    Answered (whole read): as recommended.
 * 5. Wide-area DNS-SD (a domain other than "local.")? Answered C2.15: no.
 *    Card X46's caller finds devices on one network and reaches them
 *    across networks by other means (card X47); dns_sd, Avahi and Windows
 *    differ widely there.
 * 6. Linux: one avahi-client (one D-Bus connection) per object (drafted)
 *    or one per instance, shared? Recommendation: per object. Objects stay
 *    independent with no lock between them (the instance holds no state
 *    but the platform's start), and a program has a handful of them; a
 *    shared client is an internal change later if one shows a cost.
 *    Answered C7.10: per object for 0.x. Each Linux create costs its D-Bus
 *    round trips (a known problem, stated), which a caller that makes
 *    resolvers as peers appear spreads across its updates; a shared
 *    client, an object every platform has a counterpart of (dns_sd's
 *    DNSServiceCreateConnection, an AvahiClient), is its own draft when a
 *    measured frame shows the cost.
 * 7. Names: crnet_service_advert / _browser / _resolver (drafted; "advert"
 *    to keep "register" for nothing else, "resolver" beside crnet_resolver
 *    with "service" telling them apart), or dns_sd's register / browse /
 *    resolve? Recommendation: as drafted.
 *    Answered (whole read): as drafted.
 * 8. Subtypes ("_printer._sub._http._tcp"), and advertising a host other
 *    than this machine (a proxy registration)? Recommendation: neither
 *    now; each a field and a test when asked.
 *    Answered C5.42: no caller needs either.
 */

#if __cplusplus
}
#endif

#endif // CRNET_DISCOVERY_DRAFT_H
