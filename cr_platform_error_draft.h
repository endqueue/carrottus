/*
 * cr_platform_error_draft.h - the platform's own error behind
 * CR_RESULT_API_SPECIFIC_FAILURE, delivered through the instance's message
 * callback in every module (card S10, decided 2026-11-01 with the user and
 * revised the same day: card S2's message channel, in place of a record per
 * object; the workspace's DECISIONS.md, "Decided").
 *
 * APPROVED 2026-11-01 by the user, as written here. To be built into
 * crcore.h and the module headers; where building finds this draft wrong
 * or incomplete, a card says so and this file is not edited to match.
 *
 * DRAFT, 2026-11-01. Not compilable: it names calls and
 * types crcore.h, crgpu.h, craudio.h, crfiles.h, crinput.h,
 * crmemory.h, crrandom.h and crnet.h declare. Every declaration below
 * is marked [ADDED], [CHANGED] or [REMOVED]; nothing unmarked changes.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * A CR_RESULT is the same on every platform, which is its use and its
 * limit. When the platform refuses (a VkResult, an HRESULT, an errno, an
 * OSStatus, an NSError), Carrottus returns CR_RESULT_API_SPECIFIC_FAILURE and
 * the one number that says what happened is lost: about 190 places outside
 * Carrottus Net today (Vulkan 71, D3D12 39, Metal 19, Carrottus Audio 18, Carrottus Files 17,
 * Carrottus Input 13, Carrottus Memory 7).
 *
 * Card S2 (2026-10-28) already gives crgpu_instance_desc one callback for
 * every validation message. A platform failure is one more kind of message:
 * delivered inside the failing call, on the caller's thread, naming the call
 * and the object, before the call returns. Against a record kept on each
 * object (this card's first answer):
 *   - the failure is reported where it happens, with the call that failed,
 *     never an older call's record read later;
 *   - no object carries a record for it (one small pending slot only on an
 *     object whose platform reports on a thread of its own, M3);
 *   - the platform's wording comes whole (Metal's compiler log), not cut to
 *     a fixed size;
 *   - an instance create that fails reports too, the callback being in the
 *     desc it fails on (why Vulkan chains its messenger into instance
 *     creation, and why a separate debug object made afterwards would miss
 *     it);
 *   - a `void` call's platform failure is reported, which no return code can
 *     (a present the platform refused);
 *   - no getter per handle type, and no `out_error` on calls that have an
 *     instance.
 *
 * The workspace's error policy is unchanged: a program decides from the
 * CR_RESULT. A platform failure's message is in no words of Carrottus's own:
 * the platform's number and, where the platform has some, its own wording,
 * passed through for a log or a report. (A check layer's messages, card
 * S2's, are Carrottus's words, as Vulkan's validation layers write theirs.)
 *
 * The callback is also the only place Carrottus's output goes: it never
 * writes to stdout or stderr itself (what it prints today is listed below,
 * with where each goes), and it decides no message is too routine to send.
 * Which messages to keep, show or drop is the caller's, in its handler.
 *
 * ---------------------------------------------------------------------------
 * The rules (stated once, in crcore.h's "Messages")
 * ---------------------------------------------------------------------------
 *
 *  M1. Every module with an instance takes one message callback in its
 *      instance desc, with its user data. NULL means no messages. Every
 *      object made from the instance reports to it.
 *  M2. Every failure a platform call reports is a message
 *      (CR_MESSAGE_ORIGIN_PLATFORM, severity ERROR), whatever CR_RESULT
 *      the call returns for it: CR_RESULT_API_SPECIFIC_FAILURE, an
 *      out-of-memory result, a lost device, and equally a code of its own
 *      (CR_RESULT_FILES_NOT_FOUND, from ENOENT or ENOTDIR, which the
 *      message tells apart; a refused connection; a failed lookup). For a
 *      `void` call, a failure the platform reported to it (a present that
 *      met a lost device). Carrottus judges none of them too routine to send:
 *      the caller filters on `result` in its handler. Not a failure, so
 *      nothing is sent: an answer the platform gives as an outcome
 *      (would-block, not ready, a wait's own timeout, a peer's orderly
 *      close), a time limit Carrottus keeps itself, and a refused argument or
 *      a Carrottus check, reported only by a module's own check layer where
 *      it has one (card S2's). At every validation level, off included:
 *      these failures matter most in a shipped program.
 *  M3. The callback is called inside the call that failed, on the thread
 *      that made it, before the call returns. Calls on different objects
 *      may run at the same time (the threading rule), so the callback may be
 *      called from several of the caller's threads at once; it must allow
 *      that, as Vulkan's messenger must. Carrottus never calls it from a
 *      thread of its own or a platform's. An object whose platform reports a
 *      failure on a thread of its own (a Carrottus Audio stream's callback thread;
 *      a network stack's) keeps one pending slot, written there; the
 *      caller's next call on that object takes it and sends it as a message
 *      (state Carrottus hides, so Carrottus guards it, lock-free). The slot keeps
 *      the first failure since it was last taken, the likeliest cause of the
 *      rest, and `dropped` counts the ones after it that found it full.
 *      The slot is two atomics, 12 bytes, and never a 16-byte atomic, which
 *      is lock-free only on some CPUs (x86-64 with cmpxchg16b, ARM64 with
 *      LSE) and may compile to libatomic's lock:
 *        - one 64-bit word: the CR_RESULT in the high 16 bits, the
 *          CR_PLATFORM_ERROR_SOURCE in the next 16, the code's 32 bits in
 *          the low half; 0 is empty (CR_SUCCESS, NONE). Every code a slot
 *          can hold fits 32 bits (an HRESULT, an OSStatus, an errno, a
 *          CURLcode, an NSURLErrorDomain code; Debug asserts it), and is
 *          sign-extended back to `platform_code`.
 *        - one 32-bit count of the failures that found the word full.
 *      The platform's thread compare-exchanges the word from 0 and, if it
 *      was full, adds 1 to the count. The caller's call exchanges the word
 *      with 0, then the count with 0, and sends a message if the word
 *      wasn't 0. A failure racing that take is counted in this message or
 *      sent in the next, never lost. Both atomics are 64 bits or less, so
 *      lock-free on every target. Messages a platform delivers only on its
 *      own queue (Metal's shader logs, MTLLogState) are card S2's to settle,
 *      not this card's.
 *  M4. The callback must not call the module whose call is reporting (a
 *      Carrottus GPU message's handler makes no Carrottus GPU call); another module is
 *      fine (it may write the line to a file through Carrottus Files). It returns
 *      nothing: what to do is decided from the call's CR_RESULT. It must
 *      return normally: no C++ exception, Swift or C# error, or longjmp
 *      unwinds through Carrottus (the C boundary, crossed the other way).
 *      Carrottus holds none of its own locks while calling it, so a handler
 *      that waits on another thread using Carrottus can't deadlock on them.
 *  M5. Everything the message points to is valid only during the callback;
 *      the caller copies what it keeps. Carrottus allocates nothing to send a
 *      message (the message is on the stack, the call's name a literal, the
 *      text the platform's own string while it lives).
 *  M6. A call with no instance to report to (Carrottus Memory's four, and
 *      crrandom_fill) takes `CR_PLATFORM_ERROR *out_error` instead,
 *      which may be NULL: the number alone, as those platforms give no
 *      wording that strerror or FormatMessage doesn't.
 *  M7. Messages go to whoever created the instance. A caller that is only
 *      handed an object (a library given a device) doesn't see them: its
 *      own report names the Carrottus call that failed and its CR_RESULT, and
 *      the platform's detail is the message just before it, on the same
 *      thread, in the instance creator's log. Accepted: a record on each
 *      object for such a caller would bring back the memory, the races
 *      between objects used at once, and the stale records this replaces.
 *
 * ---------------------------------------------------------------------------
 * crcore.h
 * ---------------------------------------------------------------------------
 */

/* [ADDED] Carrottus Net's CRNET_ERROR_SOURCE, moved here and renamed, every
 * value kept; VKRESULT added. */
/** @brief Which of the platform's numberings a code is in. The number means
 * nothing without it. */
typedef enum CR_PLATFORM_ERROR_SOURCE {
	/** No platform code: the message isn't a platform failure, or nothing
	 * was recorded. */
	CR_PLATFORM_ERROR_SOURCE_NONE = 0,
	/** GetLastError and WSAGetLastError values, ERROR_WINHTTP_* among them,
	 * and HRESULTs (D3D12, DXGI, WASAPI, GameInput, CryptoAPI's CERT_E_* and
	 * TRUST_E_*). FormatMessage reads all of them. */
	CR_PLATFORM_ERROR_SOURCE_WIN32 = 1,
	/** errno, from POSIX calls, ALSA's negative returns (negated) and evdev,
	 * or reported through NSPOSIXErrorDomain. */
	CR_PLATFORM_ERROR_SOURCE_ERRNO = 2,
	/** getaddrinfo's EAI_* (Linux and Apple). */
	CR_PLATFORM_ERROR_SOURCE_RESOLVER = 3,
	/** A libcurl CURLcode. */
	CR_PLATFORM_ERROR_SOURCE_CURL = 4,
	/** NSURLErrorDomain. */
	CR_PLATFORM_ERROR_SOURCE_NSURL = 5,
	/** An OSStatus: Core Audio, Security, NSOSStatusErrorDomain. */
	CR_PLATFORM_ERROR_SOURCE_OSSTATUS = 6,
	/** Carrottus Net's: the certificate matched none of the pins; the code is 0. */
	CR_PLATFORM_ERROR_SOURCE_TLS_PIN = 7,
	/** An NSError from a domain not listed here (Metal's library and
	 * pipeline errors, Game Controller's); the text starts with the
	 * domain's name. */
	CR_PLATFORM_ERROR_SOURCE_OTHER = 8,
	/** [ADDED] A VkResult. */
	CR_PLATFORM_ERROR_SOURCE_VKRESULT = 9,
} CR_PLATFORM_ERROR_SOURCE;

/* [ADDED] Who a message comes from. */
typedef enum CR_MESSAGE_ORIGIN {
	/** The platform refused a call: `platform_source` and `platform_code`
	 * say how. */
	CR_MESSAGE_ORIGIN_PLATFORM = 1,
	/** The module's own check layer (card S2's, Carrottus GPU's first). */
	CR_MESSAGE_ORIGIN_CHECK = 2,
	/** The platform's validation layer: D3D12's debug layer, Vulkan's
	 * validation layers, Metal's API validation (card S2). */
	CR_MESSAGE_ORIGIN_NATIVE_VALIDATION = 3,
	/** GPU-based validation (card S3's third level). */
	CR_MESSAGE_ORIGIN_GPU_VALIDATION = 4,
} CR_MESSAGE_ORIGIN;

/* [ADDED] Every level a platform's own layers report, so none is merged
 * into another: Vulkan's VERBOSE, INFO, WARNING and ERROR; D3D12's MESSAGE
 * (as VERBOSE), INFO, WARNING, ERROR and CORRUPTION; Metal's log levels
 * map onto the same. */
typedef enum CR_MESSAGE_SEVERITY {
	CR_MESSAGE_SEVERITY_VERBOSE = 1,
	CR_MESSAGE_SEVERITY_INFO = 2,
	CR_MESSAGE_SEVERITY_WARNING = 3,
	/** Every platform failure. */
	CR_MESSAGE_SEVERITY_ERROR = 4,
	/** D3D12's CORRUPTION: the layer judges the device's state corrupt. */
	CR_MESSAGE_SEVERITY_CORRUPTION = 5,
} CR_MESSAGE_SEVERITY;

/* [ADDED] Handed to the callback; read only during it (M5). Based, set by
 * Carrottus: a newer Carrottus may chain more onto `next`, which a caller that
 * doesn't know the type skips. CR_STRUCT_TYPE_MESSAGE registered in
 * crcore.h's block. */
typedef struct CR_MESSAGE {
	/** `type` is CR_STRUCT_TYPE_MESSAGE; `next` is NULL, or a struct a
	 * later version chains. Set by Carrottus. See CR_STRUCT_BASE. */
	CR_STRUCT_BASE base;
	/** Who the message comes from. */
	CR_MESSAGE_ORIGIN origin;
	/** How bad it is; ERROR for every platform failure. */
	CR_MESSAGE_SEVERITY severity;
	/** For a platform failure, which numbering `platform_code` is in;
	 * CR_PLATFORM_ERROR_SOURCE_NONE otherwise. */
	CR_PLATFORM_ERROR_SOURCE platform_source;
	/** For a platform failure, what the call returns for it (M2), or for a
	 * `void` call what it would have returned (a present that met a lost
	 * device: CR_RESULT_GPU_DEVICE_LOST); CR_SUCCESS for the other
	 * origins. */
	CR_RESULT result;
	/** The check's or layer's message id (card S2); 0 for a platform
	 * failure. */
	uint32_t id;
	/** For a failure sent from an object's pending slot (M3), how many more
	 * the slot overwrote since the last one sent; 0 otherwise. */
	uint32_t dropped;
	/** The platform's number, as it gave it; 0 otherwise. */
	int64_t platform_code;
	/** The call that was running, a literal ("crgpu_buffer_create"). */
	const char *call;
	/** The handle the call was made on (for a create, the handle it creates
	 * from), or NULL for an instance create. */
	const void *object;
	/** #object's debug name, or NULL. */
	const char *object_debug_name;
	/** For a create, the debug name its desc gave the object it was making
	 * (the buffer that couldn't be made), or NULL; NULL for other calls. */
	const char *subject_debug_name;
	/** The wording, UTF-8, null-terminated, whole. For a platform failure:
	 * the platform's own where it has some (an NSError's description,
	 * Metal's compiler log, libcurl's error string); else, for a code no
	 * system function reads, the code's name, a literal
	 * ("VK_ERROR_OUT_OF_DEVICE_MEMORY"), or an OSStatus's four characters
	 * when all four are printable ('fmt?'); else empty (errno and Win32
	 * codes, which strerror and FormatMessage read, and an OSStatus that is
	 * a plain number, as -50 is).
	 * For the other origins, the layer's or check's message.
	 * In the system's language; not for parsing. Never NULL. */
	const char *text;
} CR_MESSAGE;

/* [ADDED] */
/** @brief A caller's handler for messages (M1-M7).
 * @param[in] message   The message, valid only during the call.
 * @param[in] user_data The instance desc's `message_user_data`. */
typedef void (*CR_MESSAGE_CALLBACK)(const CR_MESSAGE *message, void *user_data);

/* [ADDED] For the calls with no instance (M6). */
/* Frozen: filled in per call, like every error struct. It never changes; a new need gets a new type. */
typedef struct CR_PLATFORM_ERROR {
	/** What the call returned; CR_SUCCESS if it didn't fail on the
	 * platform. */
	CR_RESULT result;
	/** Which numbering `code` is in; NONE when nothing is recorded. */
	CR_PLATFORM_ERROR_SOURCE source;
	/** The platform's number, as it gave it. */
	int64_t code;
} CR_PLATFORM_ERROR;

/*
 * ---------------------------------------------------------------------------
 * The instance descs: [CHANGED], two fields added at the end of each
 * (unreleased structs, before 1.0)
 * ---------------------------------------------------------------------------
 *
 *	/ ** Called with every message of this instance and its objects
 *	 * (crcore.h, "Messages"), or NULL for none. Required with Carrottus GPU's
 *	 * validation on (card S3's field). * /
 *	CR_MESSAGE_CALLBACK message_callback;
 *	/ ** Handed to #message_callback unchanged. * /
 *	void *message_user_data;
 *
 * crgpu_instance_desc, CRAUDIO_INSTANCE_DESC, crfiles_instance_desc,
 * CRINPUT_INSTANCE_DESC, crnet_instance_desc.
 *
 * Card S2's text changes in one detail: the callback is accepted at every
 * validation level, not only with validation on, since platform failures
 * are messages at every level (M2). "Validation on with no callback is
 * refused at creation" stays. Built before S2 and S3, this adds the callback
 * and the platform messages; S2's check layer and S3's levels then send
 * their messages through it.
 *
 * ---------------------------------------------------------------------------
 * Carrottus Memory and Carrottus Random: [CHANGED], `out_error` added last (M6)
 * ---------------------------------------------------------------------------
 *
 * crmem_reserve, crmem_commit, crmem_decommit, crmem_release and
 * crrandom_fill each gain `CR_PLATFORM_ERROR *out_error`, which may be
 * NULL; on success it is left with CR_SUCCESS and source NONE.
 *
 * ---------------------------------------------------------------------------
 * crnet.h: [CHANGED]
 * ---------------------------------------------------------------------------
 *
 * CRNET_ERROR_SOURCE moves to crcore.h as CR_PLATFORM_ERROR_SOURCE
 * (same values); crnet_error's `source` takes it. Carrottus Net keeps its
 * per-object records and `_get_error` calls as they are: a network failure
 * is routine and often asked about after the fact ("why did this socket
 * close?"), which a record serves. It also sends to its instance's
 * callback every platform failure (M2: a refused connection and a failed
 * lookup included; not would-block, a peer's orderly close, or a time limit
 * Carrottus Net keeps itself), so a log holds every module's.
 *
 * ---------------------------------------------------------------------------
 * Carrottus's own output: [REMOVED], each print moved or replaced
 * ---------------------------------------------------------------------------
 *
 * Carrottus writes nothing to stdout or stderr once this is built, in any
 * build. What prints today (src/, 2026-11-01):
 *
 * - Platform failures, which become M2 messages: Vulkan's vkQueueSubmit
 *   (two), vkCreateCommandPool, vkAllocateCommandBuffers, vkCreateSemaphore
 *   (two) and vkCreateQueryPool; D3D12's CreateCommittedResource1 and
 *   D3D12SerializeRootSignature (two); Metal's MTLCounterSampleBuffer and
 *   its three shader prints (crgpumetalshader.m).
 * - The D3D12 debug layer's printer (crgpu_d3d12_debug_message) and
 *   "Enabled D3D12 Debug Interface Layer": card S2's NATIVE_VALIDATION
 *   messages, the layer's own severity kept (CORRUPTION included).
 * - The live objects counted when a device is destroyed
 *   (crgpushared.c): a CHECK message, card S2's.
 * - A `void` recording call that records nothing and prints why: out of
 *   memory building barriers (D3D12: crgpu_command_list_pipeline_barrier,
 *   crgpu_command_list_image_barrier, crgpu_command_list_image_barrier_subresource twice,
 *   crgpu_command_list_acceleration_structure_barrier), a scaling blit
 *   (crgpu_command_list_blit_image, Metal and D3D12), an unrecognized
 *   CRGPU_IMAGE_LAYOUT (Metal's three image barriers). Not messages: a
 *   call that silently does nothing is the fault, and a message would only
 *   describe it. Each is made unable to fail (room made beforehand, as the
 *   acceleration structure build's checks do) or refused with a code:
 *   MATURITY_TODO.md, not this card.
 * - The one exception, kept and stated in crcore.h: CR_STALE's Debug
 *   line on stderr. A destroyed handle leads to no instance that can be
 *   reached safely (its memory may be the allocator's again), so there is
 *   no callback to call; Release has no check and prints nothing.
 *
 * ---------------------------------------------------------------------------
 * [REMOVED] from this draft's first version (never built)
 * ---------------------------------------------------------------------------
 *
 * The eleven `_get_error` calls (Carrottus GPU's instance, adapter, device,
 * command queue, command allocator, buffer and timestamp set; Carrottus Audio's
 * instance and stream; Carrottus Files' instance and file; Carrottus Input's instance),
 * the record on each of those objects, CR_PLATFORM_ERROR_TEXT_SIZE, and
 * `out_error` on the four instance creates.
 *
 * ---------------------------------------------------------------------------
 * ABI breaks
 * ---------------------------------------------------------------------------
 *
 * Internal: a pending slot (M3; a 64-bit and a 32-bit atomic) on Carrottus Audio's
 * stream and on Carrottus Net's objects whose stack reports on its own thread.
 *
 * Before 1.0, listed by `check.sh abi`: five instance descs gain two fields
 * at the end; CRNET_ERROR_SOURCE renamed (same values); `out_error` added
 * to Carrottus Memory's four calls and crrandom_fill. Every caller sets the new
 * fields (NULL is allowed, and means no messages) and passes NULL or a
 * record, in the same change.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * - A forced platform failure in each module reaches the callback once,
 *   inside the call, with its call name, object, result, source and code:
 *   crfiles_file_open of a path longer than the platform allows (ERRNO
 *   ENAMETOOLONG / WIN32 ERROR_FILENAME_EXCED_RANGE); a Metal shader module of bytes that aren't a
 *   .metallib (OTHER, the text naming MTLLibraryErrorDomain); a Vulkan or
 *   D3D12 buffer too large for the device (VKRESULT / WIN32, non-zero); a
 *   stream on a refused device where a test machine allows it.
 * - The message's `result` matches what the call then returns.
 * - With validation off, a platform failure is still delivered.
 * - A failure with a code of its own is sent too (M2): crfiles_path_get_info
 *   of a missing path and of a path through a file both return
 *   CR_RESULT_FILES_NOT_FOUND, and their messages carry ENOENT and
 *   ENOTDIR (POSIX; Windows' ERROR_FILE_NOT_FOUND and ERROR_PATH_NOT_FOUND);
 *   a refused TCP connect carries ECONNREFUSED / WSAECONNREFUSED.
 * - Nothing is sent for a refused argument, a fence wait that times out, a
 *   non-blocking recv with nothing to read, or a peer's orderly close.
 * - Nothing of Carrottus's reaches stdout or stderr in any of these tests,
 *   Debug or Release (both captured), but CR_STALE's line in Debug.
 * - A create's message names the object it was making
 *   (`subject_debug_name`) and the parent it made it from (`object`).
 * - A stream's failure on its callback thread is sent by the next call on
 *   the stream, on the caller's thread: the first of them, with `dropped`
 *   counting the rest (forced where a test machine allows). The slot alone,
 *   written from four threads at once while a fifth takes it: every failure
 *   either sent or counted, none lost (ThreadSanitizer clean), and its
 *   atomics lock-free (atomic_is_lock_free) on every target.
 * - A message handler that writes through another module works (M4).
 * - A NULL callback: the same failures, the same results, nothing sent.
 * - Two threads failing calls on two objects at once: both messages
 *   delivered, each with its own call (ThreadSanitizer clean).
 * - crmem_reserve of a size no platform grants (more than the address
 *   space) fills `out_error` (ERRNO ENOMEM / WIN32
 *   ERROR_NOT_ENOUGH_MEMORY): refused by the platform, not by a Carrottus
 *   check.
 * - A handler that throws is not tested (M4 forbids it); a handler that
 *   blocks on a second thread making Carrottus calls returns (no Carrottus lock
 *   held during the callback).
 * - A VkResult's message carries its name as the text; an OSStatus of four
 *   printable characters its characters, and -50 an empty text.
 * - The D3D12 debug layer's CORRUPTION and Vulkan's VERBOSE arrive as
 *   themselves, not merged into ERROR and INFO.
 * - noalloc: a failure and its message allocate nothing of Carrottus's.
 * - Carrottus Net's existing error tests, unchanged but for the enum's name, and
 *   its failures reaching the callback.
 * Run on macOS, Windows (D3D12, Vulkan) and Linux.
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 * Vulkan: VK_EXT_debug_utils' messenger, a callback with severity, type,
 *   message id, text and the objects named, chained into instance creation
 *   so instance creation itself reports; called on the application's
 *   threads, possibly at once. The model here.
 * D3D12: ID3D12InfoQueue stores messages for the application to read, and
 *   ID3D12InfoQueue1 adds a callback; a stored queue would put memory and a
 *   lock inside Carrottus, so the callback alone.
 * Metal: an NSError per failing call, which reaches the callback through
 *   the Metal backend; MTLLogState's shader logs arrive on Metal's own queue
 *   and are card S2's (M3).
 * Win32's GetLastError and POSIX's errno: the number, but thread-global,
 *   which Carrottus's no-global-state rule forbids (card S10's d).
 * SDL3: SDL_GetError() reads a per-thread string a failing call set, kept in
 *   thread-local storage allocated on a thread's first error and freed at
 *   its exit, and documented as possibly set even by a call that succeeded.
 *   Global state, outside the caller's allocator, and tied to the thread:
 *   Carrottus's threading rule lets one object's calls come from a different
 *   thread each time (a Swift actor, a C# continuation), so a failure on one
 *   thread read after resuming on another would be the wrong string.
 * SQLite's per-connection sqlite3_errcode() / sqlite3_errmsg(): a record per
 *   object, this card's first answer, kept for Carrottus Net only.
 * Rejected: a record on each object (memory on every object that can fail,
 *   text cut to a size, a getter per handle type, and a record read later
 *   may be an older call's); a generic `cr_platform_error_get(void *)` (no
 *   type checking, and an empty record that means two things); a separate
 *   debug object made after the instance (it misses instance creation).
 */
