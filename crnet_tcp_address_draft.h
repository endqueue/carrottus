/*
 * crnet_tcp_address_draft.h - Carrottus Net: TCP takes and gives
 * crnet_address, its scope with it, and the numeric strings go (card N26,
 * decided b on 2026-11-25, its shape the same day; CRNET_TODO.md, "TCP
 * addresses as `crnet_address`").
 *
 * APPROVED by the user 2026-11-25, version 6 (SHA-256 in CRNET_TODO.md,
 * N26's section), Part B with it; U1, U2 and U3 each answered a, as
 * recommended (DECISIONS.md, N34 to N37). Not built, not in any target.
 * Built on the Mac 2026-11-26 (lines beginning "Built on" are left out of the approved hash).
 * Built on Windows 2026-11-27 (lines beginning "Built on" are left out of the approved hash).
 *   Version 1: the first.
 *   Version 2 (critique cycle 1, Carrottus's own rules): a scope's
 *   refusal has its own result, CR_RESULT_NET_INVALID_SCOPE [ADDED]
 *   (T1.1); a link-local peer the system gives no scope is
 *   CR_RESULT_GENERAL_FAILURE, never a scope 0 a connect would refuse
 *   (T1.2); a scope is a bare interface index below 2^28 on every
 *   platform, Winsock's SCOPE_ID Level read as 0 (T1.3); the peer getter
 *   and the listener check in parameter order, as UDP's calls do (T1.4);
 *   multicast refused at connect and at a listener (T1.6); one rule in
 *   the function comments, the platforms' answers in this commentary
 *   (T1.7); "ipv6_index" wherever a scope is described (T1.8); Part B's
 *   refusal on crnet_resolver_create() (T1.9); results listed as crnet.h
 *   lists them, CR_SUCCESS first (T1.10); the behaviour breaks completed
 *   (T1.5).
 *   Version 3 (critique cycle 2, platform truth; its probes in the
 *   record, "Cycle 2"): another interface's index at connect fails at once
 *   or not at all, by interface (T2.1); getaddrinfo's '%' cut to 16 bits
 *   and given to a global address (T2.2); what Apple's default zone needs
 *   (T2.3); the RFC sections corrected (T2.4, T2.5); Linux's connect and
 *   bind read the scope by different tests, and the accepted socket's
 *   interface is set in tcp_v6_init_req() (T2.6, T2.7); each system's
 *   answer to multicast (T2.8); Winsock's headers and an unverified Level 0
 *   marked so (T2.9, T2.10); libuv's parse per platform (T2.11); claims
 *   not checked marked so (T2.12, T2.13, T2.15); the Windows listing's
 *   scope as its code has it (T2.14).
 *   Version 4 (critique cycle 3, the callers): the callers' section
 *   completed and corrected (T3.1 to T3.14): Rabbittus Sync's header text
 *   and its join refusing a '%', crlab's page, Pigeonous's rank, fake
 *   carrier and listener, the five locks, who is worse off.
 *   Version 5 (critique cycle 4, failures and the tests): an IPv4 address
 *   with a scope named INVALID_ADDRESS wherever INVALID_SCOPE is described
 *   (T4.1); the peer's refusal in one shared reader, tested by a new
 *   internal target (T4.2); the tests rewritten to assert what can be
 *   known and to fail when skipped on a machine that must have link-local
 *   (T4.3 to T4.6, T4.9, T4.12, T4.14); the Zone mask's order and its fuzz
 *   oracle (T4.7); the text-form tests run first against today's code
 *   (T4.8); a socket() failure named in CONNECT_FAILED and LISTEN_FAILED
 *   (T4.10); the TCP listener's WSAEACCES as ADDRESS_IN_USE, as UDP's
 *   [CHANGED] (T4.11); fe80::/10 outside fe80::/64 refused, site-local
 *   and :: with a scope named, the addresses passed on said (T4.13); the
 *   misuse factory and the resolver's capacity said (T4.15 to T4.17).
 *   Version 6 (critique cycle 5, a read of the whole): the text to go
 *   into crnet.h kept free of the draft's history, every such note moved
 *   above its declaration (T5.2, T5.3, T5.12); connect's lost NULL-address
 *   result marked (T5.1); INVALID_SCOPE and scope_id worded per call, UDP
 *   unchanged (T5.4, T5.5); fe80::/64 throughout (T5.6); a peer outside
 *   fe80::/64 refused by the reader (T5.7); Apple's privileged ports as
 *   tested (T5.8); crnet.h's "Host names", carrottus/CLAUDE.md and
 *   CRNET_TODO.md listed (T5.9); connect's two INVALID_TCP_SOCKET lines
 *   marked (T5.10); Part B's break said exactly (T5.11); "The rule"'s
 *   evidence moved to "Each platform" (T5.13). The Windows listener's
 *   WSAEACCES mapping (cycle 4's T4.11) moved out of the draft, into a
 *   change of its own: a fix within the listener's contract, not an
 *   address's.
 *
 * Written against include/crnet.h and include/crcore.h as read on
 * 2026-11-25 (carrottus version 0.5.0), the TCP backends
 * (src/net/backends/posix/crnetsockets.c for Apple, Linux and later
 * Android; src/net/backends/windows/crnetwindows.c), the shared address
 * helpers (src/net/crnetinterfaces.c, src/net/crnetshared.h,
 * "Addresses and the interface listing") and abi.txt.
 *
 * Every declaration below is marked [ADDED], [CHANGED] or [REMOVED];
 * nothing unmarked changes. It compiles beside today's crnet.h: a
 * [CHANGED] type or signature is declared under CRNET_DRAFT_CHANGED(name),
 * which appends `_as_changed` so that it doesn't collide with today's (the
 * form of crnet_resumable_draft.h); an [ADDED] result is a macro here only
 * so the file compiles (the form of crnet_udp_draft.h); text that changes
 * without a declaration is a comment marked [CHANGED]. Checked with
 *   clang -fsyntax-only -std=c11 -Wall -Wextra -Wpedantic -I include -x c
 *   clang -fsyntax-only -std=c++17 -Wall -Wextra -Wpedantic -I include -x c++
 * with -DCARROTTUS_PLATFORM_APPLE=1 (every part) and bare (CRNET_EXT_TCP 0:
 * nothing of this file but the result is declared).
 *
 * Two parts:
 * - Part A, card N26 as decided: the connect desc, the listener desc and
 *   the peer getter, and the one result they need.
 * - Part B, severable, for your choice (Q-B below): the resolver hands
 *   over crnet_address too, so that no TCP path goes through text. Part A
 *   stands without it.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * An IPv6 link-local address (fe80::/10, RFC 4291 2.4; its unicast form
 * fe80::/64, 2.5.6) is the same
 * number on every link; which link is its zone (RFC 4007 6), given to the
 * socket calls as an interface index in sin6_scope_id (RFC 3493 3.3, 3.4;
 * an implementation's convention the RFCs leave to it). The '%' text form
 * is RFC 4007 11.2. Carrottus Net's TCP takes
 * and gives addresses as numeric text, which carries a scope only as a
 * '%' suffix that each platform reads its own way, so crnet.h says "TCP
 * keeps its numeric strings for now" and crnet_address_from_string()
 * refuses a '%'. UDP, the interface listing and discovery already hand
 * over and take crnet_address, whose scope_id is sin6_scope_id. So a
 * caller can't dial over TCP a link-local address it learnt from them,
 * nor answer over UDP a TCP peer's link-local source: Pigeonous's step 3
 * found both (endqueue_server/pigeonous/PGN_TODO.md, F6).
 *
 * Today's text is worse than scopeless. Both backends parse it with
 * getaddrinfo(AI_NUMERICHOST), which on Apple and Linux takes a '%'
 * suffix of its own, by name or number, and drops one it can't read:
 * tested on macOS 27.0.1 on 2026-11-25 (the probe in
 * prompts/crnet_tcp_address_draft_record.md, "Probe"),
 * getaddrinfo("fe80::1%lo0") and ("fe80::1%1") give sin6_scope_id 1;
 * ("fe80::1%nosuch") and ("fe80::1%") give scope 0 and no error;
 * ("fe80::1%99999") gives 34463, the number cut to 16 bits; and
 * ("2001:db8::1%en0") gives a global address scope 15, which connect then
 * ignores (T2.2). Linux's glibc and Windows' parse were not checked here
 * (the Fedora and Windows tests below record them). A scope today is
 * therefore undocumented, platform-read, cut or lost silently when
 * mistyped, and given where none belongs.
 *
 * One address model for TCP, UDP and discovery: a value, compared with
 * memcmp, its scope beside it.
 *
 * ---------------------------------------------------------------------------
 * The rule every platform is held to
 * ---------------------------------------------------------------------------
 *
 * The systems disagree about a scope that is missing, wrong or where none
 * belongs (below, "Each platform"), so the header states one rule, checked
 * once before any system call (Q1), the same on every backend. Two
 * results, each for one field (Q2):
 *
 * CR_RESULT_NET_INVALID_ADDRESS, the address itself:
 * - not a valid crnet_address (crnet_address_valid(): family IPv4 or
 *   IPv6, `reserved` 0, IPv4 with no scope and nothing past its fourth
 *   byte), as UDP refuses; so an IPv4 address with a scope is
 *   INVALID_ADDRESS, not INVALID_SCOPE (T4.1);
 * - an fe80::/10 address outside fe80::/64, its bytes 2 to 7 not all zero
 *   (T4.13): RFC 4291 2.5.6 defines link-local unicast as fe80::/64 with
 *   54 zero bits, and the shared reader takes bytes 2 and 3 of any fe80::/10
 *   address as a KAME stack's embedded scope and clears them, so such an
 *   address could not come back as it went;
 * - multicast (224.0.0.0/4, ff00::/8; RFC 4291 2.7), at connect and at a
 *   listener: TCP has none on any system (T1.6), the systems refuse it
 *   differently or not at all (below, T2.8), and a scoped multicast
 *   address would otherwise fall between the scope rule and the system;
 * - IPv4-mapped IPv6 (::ffff:0:0/96; RFC 4291 2.5.5.2, RFC 3493 3.7), at
 *   connect and at a listener: an IPv6 socket here takes IPv6 only
 *   (listeners set IPV6_V6ONLY on every platform; crnet.h), and the
 *   systems' defaults disagree on a connect to one (below). An IPv4 peer
 *   is given as IPv4;
 * - at connect only, all bytes zero (0.0.0.0 or ::): Apple and Linux
 *   connect it to this machine, Windows refuses it (below). At a listener
 *   it is every interface, as today;
 * - at connect only, port 0 (no listener has it; Apple tested,
 *   EADDRNOTAVAIL in both families); at a listener 0 asks
 *   the system for one, as today and as UDP's local address.
 *
 * CR_RESULT_NET_INVALID_SCOPE [ADDED], the scope_id. A TCP address has a
 * scope exactly when it is IPv6 link-local, and the scope is a bare
 * interface index:
 * - IPv6 link-local (fe80::/64) with scope_id 0, at connect and at a
 *   listener. Carrottus Net never picks an interface, and nor may the
 *   system for it: Apple's stack can choose a default zone (below), and
 *   the systems that don't choose refuse differently (below);
 * - any other IPv6 address with scope_id not 0 (global, unique local,
 *   loopback, site-local fec0::/10, which RFC 3879 deprecated and no
 *   system here scopes for TCP, and :: at a listener, where no system can
 *   listen on every address through one interface): the systems ignore it
 *   silently (below), so a scope the caller meant is lost without a word. crnet_address
 *   already says "0 for a global address"; this makes it a check. (An IPv4
 *   address with a scope is already invalid, above.)
 * - a scope_id of 2^28 or more (T1.3), on any IPv6 address: Winsock reads
 *   sin6_scope_id as a
 *   SCOPE_ID, a 28-bit Zone and a 4-bit Level above it (SCOPE_ID in
 *   ws2def.h, the union in sockaddr_in6 in ws2ipdef.h; read in mingw-w64
 *   14.0.0's headers), so a larger value would mean another thing there
 *   than elsewhere. No system's interface index comes near it (this Mac's
 *   largest is 0x1d; Apple refuses 0x10000001 and 0x1000000f at a
 *   link-local bind and connect, EINVAL, and ignores them on ::1).
 *
 * What is passed to the system and is the system's to answer: a scope of
 * an interface that exists but isn't the address's, or of none at all
 * below 2^28 (below); a broadcast destination; an address this machine
 * doesn't hold at a listener; and addresses the rule doesn't name, which
 * the systems treat as ordinary ones (T4.13): 0.0.0.0/8 other than all
 * zero (0.1.2.3), deprecated IPv4-compatible IPv6 (::127.0.0.1, RFC 4291
 * 2.5.5.1), NAT64's 64:ff9b::/96. Each comes back as today's results
 * (CR_RESULT_NET_CONNECT_FAILED, CR_RESULT_NET_TIMED_OUT or
 * CR_RESULT_NET_SYSTEM_TIMED_OUT at connect; CR_RESULT_NET_LISTEN_FAILED
 * at a listener), the system's error in the record; the tests hold the
 * result to that set (T4.12).
 *
 * ---------------------------------------------------------------------------
 * Each platform's translation
 * ---------------------------------------------------------------------------
 *
 * One translation on both backends, the one UDP uses: the shared
 * crnet_sockaddr_from_address() (crnetinterfaces.c) writes a sockaddr_in
 * or a sockaddr_in6 with sin6_scope_id = scope_id and nothing embedded,
 * and crnet_address_from_sockaddr() reads one back (sin6_scope_id, or the
 * index a KAME stack embeds in bytes 2 and 3 where that is 0, the bytes
 * then cleared; scope 0 for every address but a link-local one).
 * [CHANGED, internal] On Windows the reader keeps the Zone alone (the low
 * 28 bits) first, and the fallbacks for a scope of 0 (the embedded bytes,
 * then the caller's interface index) apply after it (T4.7): 0x2000000F
 * reads as 15, 0x20000000 as the fallback. So a scope read back is the
 * same bare index on every platform; it reads the interface listing's and
 * UDP's sources too, which gain the same rule (no system is known to hand
 * over Level bits, so only a made sockaddr reaches it: the unit test
 * below). [ADDED, internal] crnet_tcp_peer_from_sockaddr(sockaddr,
 * length, family, out) (T4.2), written once in crnetinterfaces.c and
 * called by both backends' peer getters: the reader with interface 0,
 * then CR_RESULT_GENERAL_FAILURE for a family it can't read, a family
 * other than the socket's, a link-local peer still without a scope, or
 * an fe80::/10 peer outside fe80::/64 once an embedded scope is taken out
 * of bytes 2 and 3 (T5.7), so what it fills always passes connect's rule.
 * crnet_sockaddr_from_address() returns 0 only for a buffer too small,
 * which a sockaddr_storage never is: asserted, unreachable, as UDP
 * already relies on (T4.16). getaddrinfo leaves the TCP paths (Part B keeps it
 * in the resolver, where a name is looked up).
 *
 * The platforms' own answers to what the rule passes on, recorded here
 * and in the tests, never promised in the header (T1.7):
 * - BSD sockets, Apple (tested on macOS 27.0.1, 2026-11-25, the record's
 *   probe): a link-local address with its interface's index binds,
 *   listens and connects; getsockname, getpeername and accept give the
 *   index back in sin6_scope_id with bytes 2 and 3 zero (nothing
 *   embedded), on the listener's side and the connector's, for a listener
 *   bound to fe80::1%lo0 and one bound to ::. An index no interface has is
 *   EINVAL at bind and at connect. An index of another interface is
 *   EADDRNOTAVAIL at bind (the address isn't that interface's); at
 *   connect it depends on that interface (T2.1): EHOSTUNREACH at once
 *   (en0's address with awdl0's index) or no answer until the limit
 *   (fe80::1 with en0's index; en0's address with lo0's), the system's
 *   own ETIMEDOUT later. So a caller that tries one address on several
 *   interfaces tries them side by side, each with CRNET_NO_WAIT in a wait
 *   set, and keeps the first that connects (Pigeonous's QR, "the joiner
 *   tries it on each of its own up interfaces"). A link-local address
 *   with scope 0: EHOSTUNREACH at connect, EADDRNOTAVAIL at bind (lo0's
 *   and en0's). The stack picks a zone for scope 0 only when
 *   net.inet6.ip6.use_defaultzone is 1 and a default interface has been
 *   set (`ndp -I`; xnu's scope6.c, scope6_addr2default(); T2.3), both off
 *   on this Mac and neither the caller's to see. A scope on ::1 (en0's
 *   index, or 9999) is ignored at connect and at bind. A mapped
 *   destination connects with net.inet6.ip6.v6only 0, the default, and is
 *   EAFNOSUPPORT with IPV6_V6ONLY 1 (RFC 3493 5.3); 0.0.0.0 and :: connect
 *   to this machine. A port below 1024 is EACCES at a bind to 127.0.0.1 or
 *   ::1, while 0.0.0.0 binds (tested, cycle 5; T5.8). Multicast (T2.8):
 *   EAFNOSUPPORT at connect and at bind, IPv4 and IPv6 at every scope;
 *   255.255.255.255 at connect EAFNOSUPPORT too.
 * - BSD sockets, Linux (from the kernel's source, master, read
 *   2026-11-25: net/ipv6/tcp_ipv6.c, tcp_v6_connect() and
 *   tcp_v6_init_req(), and net/ipv6/af_inet6.c, __inet6_bind() and
 *   inet6_getname(); not run, to be confirmed on Fedora 44 by the tests
 *   below) (T2.6, T2.7): a mapped destination goes to IPv4 unless the
 *   socket is V6ONLY (then ENETUNREACH); an unspecified one is loopback
 *   ("connect() to INADDR_ANY means loopback (BSD'ism)"). Connect reads
 *   sin6_scope_id only for a link-local
 *   destination (IPV6_ADDR_LINKLOCAL), bind only for an address that needs
 *   a scope (__ipv6_addr_needs_scope_id: link-local and interface- or
 *   link-local multicast), so a global address's is ignored by both; a
 *   link-local connect or bind with no scope and no bound device is
 *   EINVAL (the source's comments: "Connect to link-local address
 *   requires an interface", "Binding to link-local address requires an
 *   interface"); a bind to an index no interface has is ENODEV; a
 *   multicast destination is ENETUNREACH at connect, and a multicast bind
 *   passes bind's address check (it is skipped for multicast), so the
 *   front end's refusal is the only one; getpeername gives the socket's
 *   bound device as a link-local peer's sin6_scope_id
 *   (ipv6_iface_scope_id()), and tcp_v6_init_req() records the SYN's
 *   interface for a link-local peer of an unbound listener (ir_iif), which
 *   the accepted socket is bound to (that last step, inet_csk_clone_lock(),
 *   not read).
 * - Winsock: sockaddr_in6's sin6_scope_id is a union with SCOPE_ID, a
 *   28-bit Zone and a 4-bit Level (ws2def.h, ws2ipdef.h; mingw-w64's, no
 *   Windows SDK on the Mac). A plain interface index is a Zone with Level
 *   0; SCOPE_LEVEL has no value 0 (ScopeLevelInterface 1 to
 *   ScopeLevelGlobal 14), so that the stack reads Level 0 as the
 *   address's own scope is expected, not verified (T2.10): the Windows
 *   test records it. An IPv6 socket is IPv6 only by default ("By default,
 *   an IPv6 socket created on Windows Vista and later only operates over
 *   the IPv6 protocol", Dual-Stack Sockets), so a mapped destination
 *   fails; connect to all-zero bytes is WSAEADDRNOTAVAIL ("The remote
 *   address is not a valid address (such as INADDR_ANY or in6addr_any)",
 *   connect's documentation). For a link-local address the zone is the IPv6
 *   interface index, crnet_interface_properties::ipv6_index (Ipv6IfIndex,
 *   iptypes.h). The interface listing gives a link-local address the
 *   system's own sin6_scope_id from GetAdaptersAddresses, or Ipv6IfIndex
 *   where the system gives none (crnetwinifaces.c; T2.14); that the two
 *   agree is for the Windows test to record. Windows' answer
 *   to a wrong scope is not known here and need not be: it is the
 *   system's, as on the others. To be recorded on Windows 11 by the tests
 *   below (an index of another interface, one of none; getpeername's raw
 *   sin6_scope_id for a link-local peer, its Level printed).
 * - Network.framework (a reference for how scope is surfaced, not a
 *   backend: Apple's TCP here is BSD sockets): an IPv6Address carries an
 *   optional NWInterface beside its bytes, the scope as an interface
 *   object rather than an index; a connection may also be pinned with
 *   NWParameters.requiredInterface.
 *
 * ---------------------------------------------------------------------------
 * The peer getter
 * ---------------------------------------------------------------------------
 *
 * crnet_tcp_socket_get_peer_address() fills one crnet_address from
 * getpeername() through crnet_address_from_sockaddr() (its interface
 * argument 0: getpeername names none): for a link-local peer its scope_id
 * as the system gave it (on Apple the index, tested; on Linux the
 * interface the connection is on), 0 for any other. A link-local peer the
 * system gives with no scope at all is CR_RESULT_GENERAL_FAILURE, "the
 * system couldn't say who the peer is" (T1.2): an address with scope 0
 * would be refused by the connect it is meant to feed. Not seen on Apple;
 * the tests watch for it elsewhere. It is never an IPv4-mapped IPv6
 * address: a listener of either family takes its own family only
 * (IPV6_V6ONLY on every platform, crnet.h), and connect refuses a mapped
 * destination, so a socket's peer is of the socket's family. (Apple
 * tested: a dual-stack listener, which Carrottus Net never makes, would
 * give an IPv4 peer as ::ffff:127.0.0.1 with scope 0.) So what it fills
 * passes the rule above, and can be handed back to
 * crnet_tcp_socket_connect_desc::to (with the port the caller means) and
 * to crnet_udp_socket_send_to() as it is.
 *
 * ---------------------------------------------------------------------------
 * Not here, and why
 * ---------------------------------------------------------------------------
 *
 * - A listener's or a connected socket's own local address as a
 *   crnet_address (crnet.h's known problem, "no query for a socket's own
 *   local address"). crnet_tcp_listener_get_port() stays: the listener's
 *   address is the caller's own desc. A getter is its own addition when a
 *   caller needs one (Q6).
 * - The same scope rule for UDP's send_to and bind, whose approved
 *   contract passes a link-local address with scope 0 and a global
 *   address with a scope to the system. One rule for both would be one
 *   address model in full; it changes an approved, built contract, so it
 *   is yours (For the user, U2).
 * - A TCP front end file. TCP's checks live in each backend (there is no
 *   crnettcp.c); the address rule is written once, in a shared check both
 *   call at the same point (Q1). Moving every TCP check into a front end,
 *   as UDP's are in crnetudp.c, is a larger change than N26.
 * - IPv4 link-local (169.254/16): IPv4 has no scope in sockaddr_in; the
 *   system routes it by its table, as today.
 * - The Windows listener's WSAEACCES at the bind (another socket's
 *   SO_EXCLUSIVEADDRUSE), LISTEN_FAILED today where UDP's create says
 *   ADDRESS_IN_USE, found by this draft's critique (T4.11): a fix within
 *   the listener's contract ("The address and port are taken"), not an
 *   address's, so a change of its own, recorded in CRNET_TODO.md.
 * - Broadcast at connect: 255.255.255.255 could be refused by its bytes,
 *   but a subnet's directed broadcast can't be told from a host without
 *   the prefix, so neither is: the system refuses both.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * The platforms: POSIX connect(), bind(), getpeername() and accept() with
 * sockaddr_in6 and sin6_scope_id (RFC 3493 3.3, 3.4, 5.3; RFC 4007 6 on
 * zone indices, 11.2 on the '%' form); Apple's BSD stack (xnu's KAME scope handling, the embedded
 * scope, net.inet6.ip6.use_defaultzone and net.inet6.ip6.v6only, tested);
 * Linux's (the source above; net.ipv6.bindv6only); Winsock's sockaddr_in6
 * and SCOPE_ID, connect() and its WSAEADDRNOTAVAIL, IPV6_V6ONLY's default
 * ("Dual-Stack Sockets for IPv6 Winsock Applications");
 * Network.framework's IPv6Address(interface:) as above (not checked
 * against Apple's documentation in this session).
 *
 * Industry practice, and what each does with a link-local address with
 * no scope:
 * - libuv: uv_tcp_connect() and uv_tcp_bind() take a struct sockaddr,
 *   passed to the system, whose error comes back for a scope of 0;
 *   uv_ip6_addr() parses a '%' suffix itself, a name through
 *   if_nametoindex on Unix and a number through atoi on Windows, an
 *   unknown name silently 0 (src/inet.c, its own comment; T2.11): the
 *   same silent loss as getaddrinfo's.
 * - Rust: std::net::SocketAddrV6 is an address, a port, a flow label and
 *   a scope_id (sin6_scope_id, its documentation says), a value as
 *   crnet_address is. That TcpStream::connect() and TcpListener::bind()
 *   pass it to the system unchanged is not checked here (T2.15).
 * - Go: net.TCPAddr is an IP, a port and a Zone ("IPv6 scoped addressing
 *   zone"), the zone an interface name; what Dial does with an empty zone
 *   not checked here.
 * - ASIO: ip::address_v6 carries scope_id() and takes one in its
 *   constructor (default 0); its endpoint's pass-through not checked.
 * None of the types refuses a missing scope; libuv, read, lets the
 * system answer, and so answers differently per platform. Carrottus Net holds one
 * answer on every platform, so it refuses: the only rule all three
 * systems can be held to without one of them choosing an interface.
 *
 * ---------------------------------------------------------------------------
 * ABI (against abi.txt; listed by ./check.sh --update-abi in the build)
 * ---------------------------------------------------------------------------
 *
 * Part A, breaks:
 * - struct crnet_tcp_listener_desc: size 32 -> 48. Removed `address`
 *   (@16, const char *) and `port` (@24, uint16_t); added `local` (@16,
 *   crnet_address, 28 bytes); `backlog` moves @28 -> @44. The same based
 *   type and CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC (3001), a new layout
 *   before 1.0 (Q5).
 * - struct crnet_tcp_socket_connect_desc: size 32 -> 48. Removed `address`
 *   and `port`; added `to` (@16, crnet_address); `timeout_ms` moves
 *   @28 -> @44. The same type, CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC
 *   (3002).
 * - fn crnet_tcp_socket_get_peer_address: CR_RESULT (crnet_tcp_socket,
 *   char *, uint16_t *) -> CR_RESULT (crnet_tcp_socket, crnet_address *).
 * Part A, added (no break): CR_RESULT_NET_INVALID_SCOPE = 3049, named in
 * cr_result_str().
 * Part B, a break:
 * - fn crnet_resolver_wait: CR_RESULT (crnet_resolver, uint32_t,
 *   char (*)[46], uint32_t, uint32_t *) -> CR_RESULT (crnet_resolver,
 *   uint32_t, crnet_address *, uint32_t, uint32_t *).
 * Unchanged: CRNET_MAX_ADDRESS_STRING_LEN (46), crnet_address_to_string()
 * and crnet_address_from_string() (Q4); every other result value and
 * struct type; crnet_address's layout.
 * Behaviour breaks not in abi.txt (listed in the commit beside it):
 * - a '%' suffix is no longer read anywhere on the TCP path; Part B: a '%'
 *   in a resolver's host is refused at crnet_resolver_create()
 *   (DESC_INVALID_PARAMETERS);
 * - refused before the system is asked: a connect to all-zero bytes, to
 *   port 0, to a multicast or an IPv4-mapped address; a listener on a
 *   multicast or a mapped address; a scope where none belongs or of 2^28
 *   or more, and a link-local address without one (each failed or was
 *   ignored by the system before);
 * - the listener's checks in parameter order (T1.4): a NULL `out_listener`
 *   now comes after the instance, desc and base checks, as UDP's create,
 *   so a call wrong in both ways gets the earlier argument's result; and
 *   a desc with a bad address and a backlog of 0 is INVALID_ADDRESS, where
 *   it was DESC_INVALID_PARAMETERS (T1.5);
 * - the peer getter: a NULL `socket` with a NULL output is now
 *   CR_RESULT_NET_INVALID_TCP_SOCKET, not NULL_OUTPUT_PARAMETER (T1.4);
 * - on Windows only, a scope read back by UDP or the interface listing
 *   loses any Level bits (none expected);
 * - an fe80::/10 address outside fe80::/64 is refused at connect and at a
 *   listener (T4.13).
 *
 * ---------------------------------------------------------------------------
 * Version (Q7)
 * ---------------------------------------------------------------------------
 *
 * 0.6.0 is the wait set's reserve's (crnet_wait_set_draft.h version 4,
 * approved 2026-11-23, not built). No version has a tag yet ("released"
 * means a vX.Y.Z tag; the workspace's CLAUDE.md, "Versions and
 * releases"), so the two share 0.6.0 if neither is tagged before both are
 * built: the macros move to 0.6.0 once, with whichever is built first;
 * the second adds to the same version, its breaks listed against abi.txt
 * in its own commit as any change's are, and the version table and "Since
 * 0.6.0" comments name both. Before 1.0 a minor version may break callers,
 * so the reserve (no break) and this (a break) may share one. If 0.6.0 is
 * tagged before this is built, this is 0.7.0. The build session checks
 * `git tag` first and says which.
 *
 * ---------------------------------------------------------------------------
 * Every caller's change, file by file (made in the same change; then the
 * five locks of carrottus, ./check.sh --update-locks, T3.11)
 * ---------------------------------------------------------------------------
 *
 * carrottus:
 * - include/crnet.h: the declarations below; "TCP sockets"'s overview
 *   ("Addresses are numeric: ..." becomes "Addresses are crnet_address
 *   values ..."); "Addresses"' "TCP keeps its numeric strings for now"
 *   removed; crnet_address's scope_id and crnet_address_from_string()'s
 *   `scope_id` worded with ipv6_index (T1.8); crnet_address's group moves
 *   from inside "TCP sockets" to stand before it, unchanged (it is defined
 *   under CRNET_EXT_TCP and used by TCP, UDP and discovery alike; text
 *   only).
 *   "Host names" (the HTTP client's): "unlike a TCP socket, which is given
 *   a numeric address" becomes "unlike a TCP socket, which is given its
 *   address as a crnet_address value" (T5.9).
 * - include/crcore.h: CR_RESULT_NET_INVALID_SCOPE; INVALID_ADDRESS's
 *   comment.
 * - CLAUDE.md ("TCP addresses are numeric; name lookup is its own
 *   object" becomes "TCP addresses are crnet_address values, a link-local
 *   one with its interface; name lookup is its own object") and
 *   CRNET_TODO.md (the N26 section: built, the runs) (T5.9).
 * - src/crcore.c: cr_result_str() names 3049.
 * - src/net/crnetshared.h and crnetinterfaces.c: [ADDED, internal]
 *   crnet_tcp_address_check(address, use), the rule above, `use` LOCAL or
 *   REMOTE, returning CR_SUCCESS, INVALID_ADDRESS or INVALID_SCOPE (Q1);
 *   [ADDED, internal] crnet_tcp_peer_from_sockaddr() (T4.2);
 *   crnet_address_from_sockaddr() keeps the Zone alone on Windows, before
 *   its fallbacks (T4.7).
 * - src/net/backends/posix/crnetsockets.c and
 *   backends/windows/crnetwindows.c: crnet_posix_parse_address() and
 *   crnet_win_parse_address() removed; the listener and connect call the
 *   shared check, then crnet_sockaddr_from_address(); the listener's and
 *   the peer getter's checks reordered (T1.4); the peer getter reads
 *   through crnet_tcp_peer_from_sockaddr(). Part B: the resolver keeps crnet_address entries
 *   (deduplicated with memcmp) in place of text, and refuses a '%' in its
 *   host.
 * - tests/crnet_test.c (every TCP desc and peer read: test_tcp,
 *   test_records, test_wait_set, test_instance_memory, test_resumable,
 *   test_internet, test_udp's TCP neighbours, test_struct_bases; Part B:
 *   test_resolver and test_internet's lookups; test_tcp's "connect to a
 *   name rather than a numeric address" goes, a name having no form in a
 *   crnet_address, and from_string("localhost")'s refusal already tested,
 *   T3.14), crcore_test (3049's name), cr_noalloc_tests.c (the idle
 *   connection), cr_misuse_tests.c (regenerated from abi.txt by
 *   tests/misuse/make_misuse_tests.py, whose crnet_tcp_listener factory
 *   sets `local` to 127.0.0.1 by crnet_address_from_string(), its family
 *   with it, T4.15; the generator's `(T){0}` arguments need no change),
 *   tests/fuzz
 *   (crnet_fuzz_text's to_string use unchanged; crnet_fuzz_ifaddr.c's
 *   oracle, `want = in6.sin6_scope_id`, masks the Zone on Windows as the
 *   reader now does, and gains seeds 0x2000000F and 0x20000000, T4.7),
 *   tests/crnet_address_rule_test.c [ADDED] (below), CMakeLists.txt (that
 *   target, built as crnet_discovery_ring_test is, with src/ on its
 *   include path; "tcp_addresses" added to the `foreach(_part interfaces
 *   udp discovery)` ctests). Unchanged:
 *   crnet_discovery_two_process.c (to_string only), the wait set.
 * rabbittus (src/sync/rbtsyncnet.cpp, include/sync/rbtsyncnet.h):
 * - the host's listeners: each listen_addresses[i] made a crnet_address
 *   with crnet_address_from_string(text, h.port, 0, &local); a failure,
 *   and CR_RESULT_NET_INVALID_SCOPE from the listener, are
 *   RBT_RESULT_INVALID_ARGUMENT as CR_RESULT_NET_INVALID_ADDRESS is today.
 * - [text, T3.2] rbtsyncnet.h's RBTSYNC_HOST_DESC::listen_addresses, "numeric,
 *   as Carrottus Net takes them", becomes "numeric, as
 *   crnet_address_from_string() reads them: no '%' suffix, and not IPv6
 *   link-local, which has no field here for its interface". A link-local
 *   listen address was RBT_RESULT_IO (the system's bind failed, LISTEN_FAILED)
 *   and is RBT_RESULT_INVALID_ARGUMENT now; a '%' suffix, which Apple's
 *   getaddrinfo took (glibc's and Windows' not checked, T3.1), is refused.
 *   Both listed in Rabbittus' commit; no caller in the workspace passes
 *   either.
 * - [T3.3] rbtsync_session_join() refuses a join host holding a '%' with
 *   RBT_RESULT_INVALID_ARGUMENT before it starts, and RBTSYNC_JOIN_DESC::host
 *   says so ("a name or a numeric address, no '%' suffix"); otherwise, with
 *   Part B, crnet_resolver_create()'s refusal would reach the session as
 *   DISCONNECTED with RBTSYNC_REASON_INTERNAL. Without Part B the same
 *   refusal keeps rbtsync's contract the same either way.
 * - connect_to_host(): Part A only: each resolved text made a
 *   crnet_address (port s.join_port, scope 0), so a link-local answer,
 *   whose scope the resolver's text already dropped, is refused at once
 *   (INVALID_SCOPE, no system call, so its message names the result
 *   alone) rather than tried; with Part B, the resolver's crnet_address
 *   entries taken as they are, the port set, scope and all. The
 *   "connecting to <address>" messages print the scope beside
 *   crnet_address_to_string()'s text ("fe80::1%15", as crlab's
 *   crlab_address_text does), so two tries of one address on two
 *   interfaces read apart (T3.4).
 * crlab (src/crlab_net.c, crlab_net.h, page/src/netpane.ts):
 * - the listener: `bind` text to crnet_address_from_string(bind, 0, 0).
 * - the TCP connect command: crnet_address_from_string(address, port,
 *   scope) as its UDP command already does (it parses a scope argument for
 *   UDP; the TCP command gains one).
 * - [T3.5] page/src/netpane.ts: the Connect TCP button sends the scope
 *   field as UDP's does, and the page's self-test and pair test append 0
 *   to their tcpConnect arguments; without it the C side's added argument
 *   fails to parse and crlab's self-test breaks.
 * - accepted peers: one crnet_address read back and printed with
 *   crlab_address_text() (address, '%' scope, port); crlab_net_connection's
 *   `peer` either becomes a crnet_address or its text grows to
 *   CRNET_MAX_ADDRESS_STRING_LEN + 11 for the scope (T3.6), the build's
 *   choice within crlab.
 * endqueue_server/pigeonous:
 * - src/pgnnet.c: listener_make() takes a family and a port and makes a
 *   zeroed `local` itself, as udp_make() does, rather than text turned
 *   into an address (T3.10); pgn_net_carrier_connect() drops
 *   is_link_local()'s refusal and the to_string round, setting d.to = *to;
 *   the accept path's `from` and pgn_net_carrier_peer_address() read the
 *   peer straight into crnet_address (the from_string(text, port, 0) round,
 *   which dropped the scope, goes), so a TCP peer's address equals its UDP
 *   source, scope included. The accept path's `from` is what answers a
 *   TCP peer's link-local source over UDP (pgnlink.c's use of out_from;
 *   the getter has no caller today, T3.9).
 * - Then N26's Pigeonous half (its PGN_TODO.md, F6): dial a QR's
 *   link-local address on each up interface with its ipv6_index, side by
 *   side; list link-local addresses in a QR again; and address_rank(),
 *   which ranks every link-local route 0 ("never", F6) so nearby links
 *   never use one, gives a scoped link-local address a rank of its own
 *   below a global IPv6 one, its value that half's to choose and record
 *   (T3.7).
 * - src/test/pgntest.c: its fake carrier keeps Carrottus Net's rule
 *   (T3.8): it refuses a link-local `to` with scope 0 (and any address
 *   Carrottus Net would refuse) and its accepted `from` keeps the scope,
 *   so the dial-on-each-interface tests can fail where the network would.
 * - tests/pgn_loopback_tests.c: the "::" listener as a zeroed IPv6
 *   crnet_address.
 * endqueue_server/client/tests (eqc_request_tests.c, eqc_test_account.cpp):
 *   each 127.0.0.1 listener as crnet_address_from_string("127.0.0.1", 0,
 *   0, &ld.local). The client's own code uses no TCP call or resolver.
 * trellis/lib/tests/trellismodel_tests.cpp (archived, card X32): four
 *   127.0.0.1 listeners, the same; trellislib's own code uses no TCP call.
 *   The workspace builds trellis against the checkout, so these are edited
 *   and trellis/lib/deps.lock moves after its model tests pass (T3.12).
 * latran and linxerworks: no Carrottus Net TCP use; their locks move
 *   after each builds and its tests pass against the new carrottus.
 * The five locks (T3.11), each moved once its repository's suites pass:
 * trellis/lib/deps.lock, latran/deps.lock, linxerworks/deps.lock,
 * endqueue_server/client/deps.lock and endqueue_server/pigeonous/deps.lock
 * (check.sh dep-locks reads all five). Rabbittus is the workspace's own
 * checkout, locked by the same files.
 * Who is worse off (T3.4): no caller in practice. Part A alone leaves
 * Rabbittus Sync as it is: a link-local answer of the resolver was
 * refused by the system and is now refused by Carrottus Net, but on an
 * Apple machine with a default zone set (use_defaultzone 1 and ndp -I),
 * where today's text could connect and Part A refuses. With Part B it
 * gains link-local answers it never had (a ".local" name answered
 * link-local), the one real gain Part B gives a caller, its only caller
 * outside carrottus being Rabbittus Sync. Pigeonous gains the link-local
 * dial and answer F6 asks for. Every other caller holds a literal.
 *
 * ---------------------------------------------------------------------------
 * Tests (crnet_test: test_tcp's cases and a new part "tcp_addresses" that
 * needs no server, a ctest with a time limit; a new internal target,
 * crnet_address_rule_test; the misuse and no-allocation harnesses)
 * ---------------------------------------------------------------------------
 *
 * First, against today's code (T4.8): none of the new tests compiles
 * against today's text API, so the build first adds a temporary text-form
 * test of every condition text can state, runs it against today's
 * backends, records what each gives, and then removes it:
 * "::ffff:127.0.0.1" (connects on Apple), "0.0.0.0" and "::" (connect to
 * this machine on Apple), port 0 (CONNECT_FAILED), "224.0.0.251" and
 * "ff02::1%1", "fe80::1" with no scope, "::1%1" and "2001:db8::1%1" (connect,
 * the scope ignored), "fe80::1%268435457" (getaddrinfo cuts it to scope 1),
 * a listener's NULL instance with a NULL output (NULL_OUTPUT_PARAMETER
 * today) and the peer getter's NULL socket with a NULL output (the same).
 * Each new test of those conditions is then seen to give the new answer.
 * Family 7, `reserved` and an IPv4 address with a scope can't be stated
 * in text; they are UDP's checks already, tested there.
 *
 * crnet_address_rule_test [ADDED] (internal, src/ on its include path, as
 * crnet_discovery_ring_test is), no network:
 * - crnet_tcp_address_check() over a table, LOCAL and REMOTE each, every
 *   row asserted (T4.13's cases): fe80::1 scope 1 passes and scope 0 is
 *   INVALID_SCOPE; febf::1 and fe80:abcd::1 INVALID_ADDRESS (outside
 *   fe80::/64); fec0::1 with a scope INVALID_SCOPE, without passes; :: with
 *   a scope INVALID_SCOPE at LOCAL; :: and 0.0.0.0 port 0 INVALID_ADDRESS
 *   at REMOTE and pass at LOCAL; ::1 with a scope INVALID_SCOPE;
 *   ::ffff:127.0.0.1 with or without a scope INVALID_ADDRESS; ff02::1,
 *   ff0e::1, ff01::1 scope 1, 224.0.0.251 and 239.255.255.255
 *   INVALID_ADDRESS; 255.255.255.255, 0.1.2.3, ::127.0.0.1 and
 *   64:ff9b::7f00:1 pass (the system's); fe80::1 scope 2^28 - 1 passes,
 *   2^28 and 0xFFFFFFFF INVALID_SCOPE; 2001:db8::1 scope 2^28
 *   INVALID_SCOPE; 127.0.0.1 scope 5 INVALID_ADDRESS; a zeroed struct
 *   INVALID_ADDRESS; port 0 with a bad scope INVALID_ADDRESS at REMOTE.
 * - crnet_tcp_peer_from_sockaddr() over made sockaddrs: fe80::1 with
 *   sin6_scope_id 0 and nothing embedded is GENERAL_FAILURE, the output
 *   zeroed; with it embedded in bytes 2 and 3, the scope; fe80::1:0:0:0:1
 *   (byte 5 set) with scope 1 GENERAL_FAILURE; an IPv4 sockaddr for an
 *   IPv6 socket GENERAL_FAILURE; a family of 0 GENERAL_FAILURE.
 * - crnet_address_from_sockaddr()'s Zone mask (T4.7): on Windows,
 *   0x2000000F reads 15 and 0x20000000 the interface fallback; elsewhere
 *   both read as given (the mask is Windows' only).
 *
 * crnet_test "tcp_addresses" (added to the RUN list and to the ctests'
 * foreach, T4.4):
 * - Link-local, asserted on one interface (T4.5): lo0's fe80::1 on Apple,
 *   where its absence is a failure, not a skip; on Linux and Windows,
 *   whose loopback has none, the first UP and RUNNING non-loopback
 *   interface with an fe80::/64 address. A listener on that address with
 *   its scope_id (the listing's, ipv6_index), a connect to it with the
 *   same, accepted. Asserted (T4.3): the client's peer equals (memcmp) the
 *   listener's `local` with the port from crnet_tcp_listener_get_port();
 *   the server's peer has that `local`'s family, bytes and scope_id and a
 *   port not 0; then a UDP socket bound on that address sends to the
 *   server's peer with the port changed to a second UDP socket's on the
 *   client's side, it arrives, and its source read back equals the
 *   server's peer but for the port (F6's second half). A listener on ::
 *   reached the same way: the same assertions, the scope from the
 *   accepted socket. The peer read back is then given to a second connect
 *   (to `local`'s port) as it is, and connects: the round trip.
 * - Every other UP and RUNNING interface with an fe80::/64 address (awdl0,
 *   llw0, the utuns and anpi on the Mac): the same connect and listener,
 *   each result asserted to be CR_SUCCESS or one of the system's listed
 *   results, the outcome printed per interface (T4.5).
 * - Where there is no eligible interface (Linux or Windows without IPv6
 *   on an up interface), a line "SKIPPED: tcp_addresses link-local (no
 *   fe80 address on an up interface)", and with CRNET_TEST_REQUIRE_LINK_LOCAL=1
 *   (set on the self-hosted runners and the machines the record names) a
 *   failure (T4.4).
 * - Each refusal, at connect and at a listener, no socket opened, by the
 *   rule's table above, on a socket that then takes one valid connect
 *   (127.0.0.1 to a live listener) and gets CR_SUCCESS: a refused desc
 *   left it usable (T4.6).
 * - The system's answers, each asserted to be in the header's set and its
 *   errno printed (T4.12): a link-local connect with an index no interface
 *   has (0x0FFFFFFE, below 2^28 and never assigned) is CONNECT_FAILED (or
 *   TIMED_OUT, then CONNECT_FAILED or SYSTEM_TIMED_OUT from continue);
 *   with another interface's index and CRNET_NO_WAIT then continue for
 *   500 ms, TIMED_OUT or CONNECT_FAILED; a listener with 0x0FFFFFFE, and
 *   with another interface's index, LISTEN_FAILED; fe80::1 at 2^28 - 1,
 *   past the check, CONNECT_FAILED or LISTEN_FAILED. On Windows the raw
 *   sin6_scope_id of the link-local peer is printed, Zone asserted equal
 *   to the interface's ipv6_index and the Level printed.
 * - The peer getter: a NULL socket is INVALID_TCP_SOCKET whatever the
 *   output; a NULL output NULL_OUTPUT_PARAMETER; on a socket not connected
 *   and on an aborted one, the output filled with 0xA5 first and every
 *   byte 0 after (T4.9); an IPv4 and an IPv6 loopback peer, scope 0.
 * - The listener's order (T4.9): a NULL instance with a NULL output is
 *   INVALID_INSTANCE, the output untouched (filled first); a good desc
 *   with a NULL output NULL_OUTPUT_PARAMETER; a bad address with a NULL
 *   output NULL_OUTPUT_PARAMETER; a bad address with backlog 0
 *   INVALID_ADDRESS (T1.5).
 * - Part B: "localhost" and "::1" resolved to crnet_address values with
 *   port 0; "fe80::1%lo0" refused at create (DESC_INVALID_PARAMETERS) on
 *   every platform; a numeric "fe80::1" resolved with scope 0 (the system
 *   has nothing to add), refused by connect with INVALID_SCOPE; "localhost"
 *   into a capacity of 1 gives 1 (T4.17). On Windows, GetAddrInfoExW's
 *   reading of "%name" and "%number" recorded first, through the
 *   temporary text-form test (T2.12).
 * - Struct bases: the existing zeroed-base and chained cases with the new
 *   descs.
 * - crcore_test: 3049 named.
 * - Misuse: regenerated from abi.txt; every call with zeroes and with a
 *   destroyed socket and listener. A gain (T4.15): the destroyed listener
 *   and peer getter reach CR_STALE for the first time (the output checked
 *   first hid it), so both stale lines are now expected.
 * - cr_noalloc_tests (T4.14): `net_round` gains
 *   crnet_tcp_socket_get_peer_address(), and one crnet_tcp_socket_connect()
 *   to the loopback listener on a socket made before the trace is traced:
 *   no allocation of Carrottus's, and none of the platform's either,
 *   where today getaddrinfo's parse allocated (the platform's, counted
 *   apart by alloctrace; recorded first against today's code).
 * - Pigeonous's test_qr_link_local_only (its PGN_TODO.md, F6), made real
 *   by N26's Pigeonous half: a QR with only a link-local address joined
 *   over each up interface on one machine.
 * - Per platform: the Mac (plain, Release, -asan, -tsan) and the iOS
 *   simulator (its interfaces are the Mac's; the part runs where lo0 is
 *   listed); Windows 11 on the ThinkPad (Debug, Release, -asan; loopback
 *   has no fe80:: address there, so the Ethernet or Wi-Fi one, and the
 *   SCOPE_ID Level read back); Fedora 44 (GCC and clang, -asan, -tsan; the
 *   kernel's EINVAL and ENODEV confirmed, the multicast bind that passes
 *   the kernel's check refused by the front end, glibc's '%' parse
 *   recorded). Windows also records GetAddrInfoExW with '%name' and
 *   '%number' (Part B's claim), and the listing's link-local scope_id
 *   against Ipv6IfIndex. The callers' suites after the locks: rabbittus'
 *   Sync tests, Pigeonous's (pgn_loopback_tests among them), the client's,
 *   trellislib's model tests, latran's and linxercore's builds, crlab
 *   built and its self-test run.
 *
 * ---------------------------------------------------------------------------
 * Open questions, each answered under itself
 * ---------------------------------------------------------------------------
 *
 * Q1. Where the rule is checked. Answer: once, in an internal
 *     crnet_tcp_address_check() beside crnet_address_valid(), returning
 *     CR_SUCCESS or the first of its two results, called by both backends
 *     at the same point, as cr_struct_check() is: the listener after the
 *     output's check and before `backlog` (field order, as UDP's create
 *     checks `local` before `sharing`), the connect after the struct base
 *     and before the abort and once-only checks, as the text's parse sits
 *     today. Inside it, one `if` per result: the address's conditions
 *     joined (each INVALID_ADDRESS), then the scope's (each INVALID_SCOPE);
 *     so two faults in one address have one answer, the address's (a
 *     mapped address with a scope, multicast with scope 0, all-zero bytes
 *     with a scope at connect, port 0 with a bad scope: each
 *     INVALID_ADDRESS).
 * Q2. One result or one per condition. Answer, drafted and yours to
 *     confirm (For the user, U3): one per field (T1.1).
 *     CR_RESULT_NET_INVALID_ADDRESS for the address (its bytes, family and
 *     port), as UDP's send_to and bind and crnet_address_from_string() use
 *     it; CR_RESULT_NET_INVALID_SCOPE [ADDED] for the scope_id, the one
 *     refusal a caller acts on at run time (a link-local address learnt
 *     without its interface: try it on each). Its comment keeps to its
 *     field, and INVALID_ADDRESS's is made specific, not wider.
 * Q3. What the peer getter gives (above). Answer: the system's scope for
 *     a link-local peer, never 0 (GENERAL_FAILURE instead); 0 for any
 *     other; never mapped; the output zeroed on any failure after its own
 *     check, as the text is emptied today.
 * Q4. Whether CRNET_MAX_ADDRESS_STRING_LEN and a text path outlive the
 *     change. Answer: the macro stays, for crnet_address_to_string() (logs)
 *     and, without Part B, the resolver's text. crnet_address_from_string()
 *     stays, for callers that hold text (a configuration, a command
 *     line). With Part B no TCP path takes or gives text.
 * Q5. The descs' struct bases. Answer: the same based types and the same
 *     CR_STRUCT_TYPE values with new layouts, allowed before 1.0 and
 *     listed above; not chained structs, since nothing is kept of the old
 *     fields and a chain for a replacement would keep them forever.
 *     check.sh struct-bases holds: base first, no #if inside, and
 *     crnet_address is a frozen value type, not a based struct, so holding
 *     it by value is allowed (crnet_udp_socket_desc already does).
 *     check.sh abi fails until abi.txt is written by --update-abi in the
 *     build, which lists the breaks above.
 * Q6. A local-address getter for a listener or a socket. Answer: not in
 *     N26 (above); crnet_tcp_listener_get_port() stays as it is.
 * Q7. The version. Answer: 0.6.0 shared with the wait set's reserve while
 *     neither is tagged; 0.7.0 if 0.6.0 is tagged first (above).
 * Q8. The field names. Answer: `local` for the listener, as UDP's desc
 *     names its bound address, and `to` for the connect, as
 *     crnet_udp_socket_send_to() names its destination. A desc of today's
 *     form then fails to compile on both fields, in C and C++, rather than
 *     compiling against a reused name.
 * Q-B. Part B, the resolver as crnet_address. Yours (For the user, U1).
 *     Recommendation: take it, in the same break. The resolver is TCP's
 *     only other text path; getaddrinfo hands it a link-local answer's
 *     scope (a name on the local link, or a numeric host with a suffix),
 *     which today's inet_ntop drops; its one caller outside carrottus is
 *     Rabbittus Sync, which turns the text straight back into an address
 *     to connect to.
 *
 * ---------------------------------------------------------------------------
 * For the user (each a card in DECISIONS.md beside the approval's)
 * ---------------------------------------------------------------------------
 *
 * U1. Part B, the resolver as crnet_address: (a) in the same break, as
 *     drafted; (b) later, its own draft; (c) never, the resolver keeps text.
 *     Recommendation: a (Q-B).
 * U2. The same scope rule for UDP's send_to and bind: (a) a card and a
 *     draft after this is built, UDP's contract changed to refuse a
 *     link-local address without a scope and a scope where none belongs,
 *     with INVALID_SCOPE; (b) UDP left as approved, the scope passed to the
 *     system. Recommendation: a, one address model in full; until then
 *     INVALID_SCOPE's and scope_id's comments say the rule is each call's.
 * U3. The scope's refusal: (a) CR_RESULT_NET_INVALID_SCOPE, as drafted;
 *     (b) CR_RESULT_NET_INVALID_ADDRESS for every address refusal, the
 *     card's first wording; (c) a missing or misplaced scope passed to the
 *     system, as UDP does. Recommendation: a; c would leave the systems'
 *     different answers (and Apple's default zone) to the caller.
 */

#ifndef CRNET_TCP_ADDRESS_DRAFT_H
#define CRNET_TCP_ADDRESS_DRAFT_H

#include "crnet.h"

#if __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* crcore.h, CR_RESULT [ADDED] and [CHANGED]                                */
/* ======================================================================== */

/** [ADDED] The scope_id doesn't fit the address, by the rule of the call
 * that takes it (crnet.h, "TCP sockets"): a link-local IPv6 address with
 * none, another IPv6 address with one, or a scope_id of 2^28 or more. An
 * IPv4 address with a scope is CR_RESULT_NET_INVALID_ADDRESS. */
#define CR_RESULT_NET_INVALID_SCOPE ((CR_RESULT)3049)

/* [CHANGED] CR_RESULT_NET_INVALID_ADDRESS = 3025, its comment only:
 *   "Not an address the call takes: not a numeric IPv4 or IPv6 address,
 *    not a valid crnet_address, or one the call's address rules refuse
 *    (crnet.h, under each call)."
 * Was "Not a numeric IPv4 or IPv6 address.", which no call taking a
 * crnet_address meant even before this change; the scope is the new
 * result's. */

#if CRNET_EXT_TCP

#define CRNET_DRAFT_CHANGED(name) name##_as_changed

/* ======================================================================== */
/* crnet.h, text that changes [CHANGED]                                     */
/* ======================================================================== */

/* [CHANGED] "TCP sockets", the overview's paragraph on addresses, now:
 *
 *   Addresses are crnet_address values (@ref crnet_addresses
 *   "Addresses"): the bytes, the port and, for an IPv6 link-local address,
 *   its interface as scope_id, that interface's ipv6_index from the
 *   interface listing (which already puts it in every link-local address
 *   it lists). Connect and a listener hold an address to one rule, the
 *   same on every platform, checked before the system is asked:
 *   - CR_RESULT_NET_INVALID_ADDRESS for an address that isn't a valid
 *     crnet_address (an IPv4 address with a scope among them), an
 *     fe80::/10 one outside link-local's fe80::/64, a multicast one, or an
 *     IPv4-mapped IPv6 one (an IPv6 socket takes IPv6 only, so an IPv4
 *     peer is given as IPv4); and at connect, all-zero bytes or port 0;
 *   - CR_RESULT_NET_INVALID_SCOPE where an IPv6 address's scope doesn't
 *     fit: it has one exactly when it is link-local, and it is below 2^28.
 *     Carrottus Net never chooses an interface for an address that has
 *     none, and doesn't pass on a scope the systems would ignore.
 *   The calls never look a name up, because the system's lookup can be
 *   neither given a time limit nor interrupted, and a connect that hid one
 *   couldn't keep its own. Turning a host name into addresses is its own
 *   object, the crnet_resolver, and which of the answers to use is the
 *   caller's to choose.
 *
 * Its known problems keep "no local address for an outgoing connection and
 * no query for a socket's own local address".
 *
 * [CHANGED] "Addresses": "TCP keeps its numeric strings for now." removed;
 * "what the interface listing, UDP sockets and service discovery hand over
 * and take" becomes "what TCP and UDP sockets, the interface listing and
 * service discovery hand over and take" (and, with Part B, the resolver).
 * The group stands before "TCP sockets" rather than inside it, text only.
 *
 * [CHANGED] crnet_address::scope_id, the field's comment
 * becoming: "IPv6: the interface a link-local (fe80::/10) address is used
 * through, as sin6_scope_id has it: the interface's ipv6_index
 * (crnet_interface_properties), which on Windows may differ from its
 * index. 0 for a global address; which other addresses may carry one is
 * each call's rule. IPv4: always 0." 
 *
 * [CHANGED] crnet_address_from_string()'s `scope_id`: "0, or for
 * IPv6 the interface's ipv6_index; non-zero with an IPv4 address is
 * refused."
 */

/* ======================================================================== */
/* crnet.h, Listener [CHANGED]                                              */
/* ======================================================================== */

/**
 * @brief [CHANGED] Where crnet_tcp_listener_create() listens: `address`
 * (text) and `port` are one crnet_address, `local`.
 */
typedef struct CRNET_DRAFT_CHANGED(crnet_tcp_listener_desc) {
	/** `type` is CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC; `next` is NULL (nothing is
	 * chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** [CHANGED] The local address and port to listen on. Its family is the
	 * listener's, and an IPv6 listener takes IPv6 connections only, on every
	 * platform; listening on both families is two listeners. All-zero bytes
	 * is every interface of the family (0.0.0.0 or ::), 127.0.0.1 or ::1 this
	 * machine only, or one interface's own address; an IPv6 link-local one
	 * (fe80::/64) with its interface's ipv6_index as scope_id, and every
	 * other address with scope_id 0; not multicast, not IPv4-mapped IPv6.
	 * Port 0 has the system pick a free one;
	 * crnet_tcp_listener_get_port() says which. Replaces `address` and
	 * `port`. */
	crnet_address local;
	/** How many connections may wait to be accepted before the system
	 * starts turning new ones away. The system may cap it. 0 is refused. */
	uint32_t backlog;
} CRNET_DRAFT_CHANGED(crnet_tcp_listener_desc);

/* Draft note: crnet_tcp_listener_create()'s brief, its known problem and
 * its two rules on when an address and port count as taken are today's,
 * unchanged; the one sentence added to them is the paragraph marked
 * [CHANGED] below. Its checks move into parameter order, `out_listener`
 * after the desc's base as crnet_udp_socket_create()'s is (was first), and
 * a NULL `desc->address` is no longer a case (there is no pointer). */
/**
 * @brief Binds and starts listening.
 *
 * [CHANGED] The address is held to the rule under "TCP sockets" before
 * anything is opened.
 *
 * @param[in]  instance     The instance the listener is made with.
 * @param[in]  desc         The address, port and backlog. Only read during
 *                          the call.
 * @param[out] out_listener Receives the new listener. Untouched by the
 *                          failures before its own check; NULL on every
 *                          one after it.
 * @param[out] out_error    Receives the system's reason for a failure, which
 *                          leaves no listener to ask. May be NULL. See
 *                          @ref crnet_error_detail "Error detail".
 *
 * @retval CR_SUCCESS The listener is listening.
 * The failures, in the order they are checked:
 * @retval CR_RESULT_NET_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_listener` is NULL.
 * @retval CR_RESULT_NET_INVALID_ADDRESS [CHANGED] `desc->local` is not a
 *         valid crnet_address (its family neither IPv4 nor IPv6, its
 *         `reserved` not 0, IPv4 with a scope or with bytes past the
 *         fourth), is fe80::/10 outside fe80::/64, is multicast, or is
 *         IPv4-mapped IPv6.
 * @retval CR_RESULT_NET_INVALID_SCOPE [ADDED] `desc->local` is IPv6
 *         link-local with scope_id 0, is any other IPv6 address (all-zero
 *         bytes included) with scope_id not 0, or has a scope_id of 2^28
 *         or more.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS [CHANGED] `desc->backlog` is 0.
 * @retval CR_RESULT_OUT_OF_MEMORY The listener could not be allocated.
 * @retval CR_RESULT_NET_ADDRESS_IN_USE The address and port are taken.
 * @retval CR_RESULT_NET_LISTEN_FAILED The system refused to open, bind
 *         or listen on the socket for any other reason: [CHANGED] among
 *         them a socket of that family it couldn't open (no IPv6, no
 *         descriptor left), an address this machine doesn't hold, a
 *         privileged port where the system has them, and a link-local
 *         address whose scope is not an interface it is on.
 * @retval CR_RESULT_GENERAL_FAILURE The system refused something else the
 *         listener is made of.
 */
CR_RESULT CRNET_DRAFT_CHANGED(crnet_tcp_listener_create)(
    crnet_instance instance,
    const CRNET_DRAFT_CHANGED(crnet_tcp_listener_desc) *desc,
    crnet_tcp_listener *out_listener,
    crnet_error *out_error);

/* ======================================================================== */
/* crnet.h, Socket [CHANGED]                                                */
/* ======================================================================== */

/**
 * @brief [CHANGED] Where crnet_tcp_socket_connect() connects to, and how
 * long it may take: `address` (text) and `port` are one crnet_address,
 * `to`.
 */
typedef struct CRNET_DRAFT_CHANGED(crnet_tcp_socket_connect_desc) {
	/** `type` is CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC; `next` is NULL (nothing is
	 * chained onto this struct yet). Set by the caller. See
	 * CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** [CHANGED] The address and port the other end is listening on; its
	 * family is the connection's. Not all-zero bytes, not multicast, not
	 * IPv4-mapped IPv6, port not 0. An IPv6 link-local one (fe80::/64) with the
	 * interface to reach it through as scope_id (that interface's
	 * ipv6_index), and every other address with scope_id 0. A name is the
	 * crnet_resolver's. Replaces `address` and `port`. */
	crnet_address to;
	/** The longest connecting may take. See
	 * @ref crnet_time "Time limits". */
	uint32_t timeout_ms;
} CRNET_DRAFT_CHANGED(crnet_tcp_socket_connect_desc);

/* Draft note: crnet_tcp_socket_connect()'s brief, its step and what is
 * kept for it are today's, unchanged; the paragraph marked [CHANGED] below
 * is added after them. Its result list loses CR_RESULT_DESC_INVALID_PARAMETERS
 * for a NULL `desc->address` [REMOVED] (there is no pointer), and lists
 * its two CR_RESULT_NET_INVALID_TCP_SOCKET cases apart, in the code's
 * order [CHANGED] (unchanged behaviour). */
/**
 * @brief Begins connecting to a listening address and port, and waits up
 * to `desc->timeout_ms` for the connection. Once per socket.
 *
 * [CHANGED] The destination is held to the rule under "TCP sockets" and
 * checked before anything is begun, so a refused desc leaves the socket
 * as it was and connect may be called again on it. A link-local destination whose scope is an interface it isn't reachable
 * through, or no interface, is the system's to answer, as an unreachable
 * address is: it may fail at once (CR_RESULT_NET_CONNECT_FAILED) or not
 * answer until the limit (CR_RESULT_NET_TIMED_OUT, then
 * CR_RESULT_NET_SYSTEM_TIMED_OUT). Trying one address on several
 * interfaces is a socket for each.
 *
 * @param[in] socket The socket to connect.
 * @param[in] desc   The address, port and time limit. Only read during the
 *                   call.
 *
 * @retval CR_SUCCESS The socket is connected.
 * The failures, in the order they are checked:
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET `socket` is NULL.
 * @retval CR_RESULT_NULL_DESC `desc` is NULL.
 * @retval CR_RESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC.
 * @retval CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval CR_RESULT_NET_INVALID_ADDRESS [CHANGED] `desc->to` is not a valid
 *         crnet_address, is fe80::/10 outside fe80::/64, is multicast, is
 *         IPv4-mapped IPv6, has all-zero bytes, or has port 0.
 * @retval CR_RESULT_NET_INVALID_SCOPE [ADDED] `desc->to` is IPv6 link-local
 *         with scope_id 0, is any other IPv6 address with scope_id not 0,
 *         or has a scope_id of 2^28 or more.
 * @retval CR_RESULT_NET_ABORTED crnet_tcp_socket_abort() was called.
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET connect has already been called
 *         on it.
 * @retval CR_RESULT_NET_TIMED_OUT The connection isn't made yet; the
 *         attempt goes on. Carry it on with
 *         crnet_tcp_socket_connect_continue().
 * @retval CR_RESULT_NET_CONNECT_FAILED The other end refuses or can't
 *         be reached, [CHANGED] or the system couldn't open a socket of the
 *         address's family (no IPv6, no descriptor left), the system's
 *         error in the record. The socket is spent either way.
 * @retval CR_RESULT_NET_SYSTEM_TIMED_OUT Nothing answered and the
 *         operating system stopped trying before `desc->timeout_ms` was up
 *         (@ref crnet_time "Time limits").
 */
CR_RESULT CRNET_DRAFT_CHANGED(crnet_tcp_socket_connect)(
    crnet_tcp_socket socket,
    const CRNET_DRAFT_CHANGED(crnet_tcp_socket_connect_desc) *desc);

/* Draft note: crnet_tcp_socket_get_peer_address() gives one crnet_address
 * in place of text and a port; its NULL output is checked after `socket`
 * (was first), in parameter order. */
/**
 * @brief [CHANGED] Reads the address and port of the other end.
 *
 * getpeername(), read as UDP reads a datagram's source: for an IPv6
 * link-local peer its interface as scope_id (its ipv6_index), as the
 * system gave it, never 0; 0 for every other. Of the socket's own family,
 * never IPv4-mapped (an IPv6 listener takes IPv6 only, and connect refuses
 * a mapped destination). What it fills passes connect's rule, so it may
 * be given to crnet_tcp_socket_connect_desc::to and to
 * crnet_udp_socket_send_to() as it is.
 *
 * @param[in]  socket      The connected socket.
 * @param[out] out_address [CHANGED] Receives the peer's address and port.
 *                         Zeroed on every failure after its own check.
 *
 * @retval CR_SUCCESS `out_address` was written.
 * The failures, in the order they are checked:
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET `socket` is NULL.
 * @retval CR_RESULT_NULL_OUTPUT_PARAMETER `out_address` is NULL.
 * @retval CR_RESULT_NET_ABORTED crnet_tcp_socket_abort() was called.
 * @retval CR_RESULT_NET_INVALID_TCP_SOCKET `socket` isn't connected.
 * @retval CR_RESULT_GENERAL_FAILURE The system couldn't say who the peer
 *         is: [CHANGED] among them a link-local peer it gave with no
 *         interface.
 */
CR_RESULT CRNET_DRAFT_CHANGED(crnet_tcp_socket_get_peer_address)(
    crnet_tcp_socket socket,
    crnet_address *out_address);

/* [REMOVED] crnet_tcp_socket_get_peer_address(socket, char
 * out_address[CRNET_MAX_ADDRESS_STRING_LEN], uint16_t *out_port): the text
 * and the separate port, replaced by the declaration above. */

/* ======================================================================== */
/* Part B, severable: crnet.h, Resolver [CHANGED]                           */
/* ======================================================================== */

/* [CHANGED, Part B] crnet_resolver_desc::host's comment gains: "No '%'
 * suffix: a scope is never text here; a link-local answer carries the
 * system's scope in crnet_resolver_wait()'s scope_id." Its layout is
 * unchanged. Draft note: a '%' is refused as crnet_address_from_string()
 * refuses one, since each system reads a suffix its own way (Apple, tested,
 * takes a name or a number, cut to 16 bits, and drops an unknown name;
 * glibc's and GetAddrInfoExW's to be recorded on Fedora and Windows).
 *
 * [CHANGED, Part B] crnet_resolver_create(), one result reworded:
 *   @retval CR_RESULT_DESC_INVALID_PARAMETERS `desc->host` is NULL, empty,
 *           or holds a '%'.
 * DESC_INVALID_PARAMETERS, not INVALID_ADDRESS: the host is a name, a
 * desc field, as its NULL and empty cases are. */

/**
 * @brief [CHANGED, Part B] Waits for the lookup to finish and copies its
 * addresses out.
 *
 * Each in the order the resolver gave them (its preference), port 0 (the
 * caller's to set: a lookup has no port), and for an IPv6 link-local
 * answer the interface the system gave as scope_id (0 where it gave
 * none, which a connect refuses). Duplicates are dropped by the whole
 * value, so one address on two interfaces is two entries. Once the lookup
 * has finished, every wait returns the same answer at once. A wait on a
 * resolver nobody runs just uses up its time limit.
 *
 * Thread: may overlap crnet_resolver_run() and crnet_resolver_abort().
 *
 * @param[in]  resolver      The resolver to wait on.
 * @param[in]  timeout_ms    The longest to wait
 *                           (@ref crnet_time "Time limits");
 *                           CRNET_NO_WAIT only looks.
 * @param[out] out_addresses [CHANGED] Receives up to `out_capacity`
 *                           addresses; any past it are dropped without a
 *                           result.
 * @param[in]  out_capacity  How many addresses `out_addresses` has room for.
 * @param[out] out_count     Receives how many addresses were written; 0 on
 *                           any failure.
 *
 * Results: today's, unchanged (CR_SUCCESS, CR_RESULT_NULL_OUTPUT_PARAMETER,
 * CR_RESULT_NET_INVALID_RESOLVER, CR_RESULT_NET_TIMED_OUT,
 * CR_RESULT_NET_ABORTED, CR_RESULT_NET_RESOLVE_FAILED,
 * CR_RESULT_GENERAL_FAILURE).
 */
CR_RESULT CRNET_DRAFT_CHANGED(crnet_resolver_wait)(
    crnet_resolver resolver,
    uint32_t timeout_ms,
    crnet_address *out_addresses,
    uint32_t out_capacity,
    uint32_t *out_count);

#undef CRNET_DRAFT_CHANGED

#endif // CRNET_EXT_TCP

#if __cplusplus
}
#endif

#endif // CRNET_TCP_ADDRESS_DRAFT_H
