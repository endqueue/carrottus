/*
 * spudfiles_library_draft.h - SpudFiles: opening a dynamic library, finding
 * a function it exports, closing it (card S44, raised 2026-11-09).
 *
 * DRAFT, 2026-11-09, first version. APPROVED 2026-11-09 by the user, every
 * recommendation (Q1-Q6 a, each "Decided" under its question): the
 * contract, kept as the record. Nothing of it is built. Not compilable: it repeats declarations of
 * spudcore.h and spudfiles.h to show them changed, and names the approved,
 * unbuilt spud_platform_error_draft.h (card S10). Every declaration is
 * marked [ADDED] or [CHANGED]; nothing unmarked changes. Written on the
 * ThinkPad P1 Gen 5 (Windows 11) against include/spudfiles.h and
 * include/spudcore.h as they stand after cards S39, S41 and S42 (built
 * 2026-11-06 and 07), and src/ (no LoadLibrary, dlopen, GetProcAddress or
 * dlsym anywhere in it).
 *
 * The card named the file spudlibrary_draft.h; it is
 * spudfiles_library_draft.h because Q1 recommends SpudFiles. Answered
 * otherwise, it is renamed.
 *
 * ---------------------------------------------------------------------------
 * Why now
 * ---------------------------------------------------------------------------
 *
 * SpudLib has no way to load code at run time, and nothing above it may
 * call the platform for it. Three calls, each a translation: LoadLibraryExW,
 * GetProcAddress and FreeLibrary on Windows; dlopen, dlsym and dlclose on
 * Linux and every Apple system. Every choice the platform's loader takes
 * is the caller's, with no default (Q2); what the loader says when it
 * refuses goes to the instance's message callback whole (Q6), since a
 * loader's refusal is mostly its words ("libfoo.so.2: cannot open shared
 * object file", a missing dependency's name), not a number.
 *
 * ---------------------------------------------------------------------------
 * Stated, not asked
 * ---------------------------------------------------------------------------
 *
 * - The path is UTF-8, converted as every SpudFiles path is (WTF-8 to
 *   UTF-16 on Windows). Nothing is added to it: no extension, no "lib"
 *   prefix, no search of SpudLib's own.
 * - Opening runs the library's initializers (DllMain's
 *   DLL_PROCESS_ATTACH, ELF constructors, Objective-C +load, C++ static
 *   constructors) inside the call, on the caller's thread; one that fails
 *   (DllMain returning FALSE) fails the open. Closing runs its finalizers
 *   when the platform unloads it. The platform's loader lock is held while
 *   they run; what they may call then is the platform's rule (Microsoft's
 *   DllMain rules), not SpudLib's.
 * - The platform counts opens of one library: two opens of one path give
 *   two handles over one loaded image, sharing its globals, each closed on
 *   its own. A caller that wants a second, separate image of a rebuilt
 *   library opens it under a path of its own (a copy): on every platform a
 *   path already loaded gives the image already loaded.
 * - Closing is a request: the platform may keep the image mapped (Linux
 *   keeps one that registered C++ thread_local destructors or was opened
 *   RTLD_NODELETE by another; Apple keeps one that holds Objective-C
 *   classes or Swift metadata). After spudfile_library_close() nothing
 *   found through the handle may be called, whether or not the image went;
 *   SpudLib can't tell, and nothing reports it.
 * - A function is found among the library's own exports. What a lookup
 *   finds in the library's dependencies is undefined: dlsym on a handle
 *   searches its dependencies too, GetProcAddress only the module itself,
 *   so the narrowest rule is the library's own. The name is passed as
 *   given (a C name; dlsym adds Apple's leading underscore itself; no
 *   decoration is added or removed). Windows' lookup by ordinal is not
 *   offered.
 * - Windows' error mode (SetThreadErrorMode, SEM_FAILCRITICALERRORS) is
 *   not changed: it is the calling thread's, and the caller's to set.
 * - The platform's code-signing and library-validation policy (Apple's
 *   hardened runtime, Windows' signature requirements under WDAC) is the
 *   platform's: a library it refuses is a failed open with its words.
 * - No natives accessor (the HMODULE or the dlopen handle): none asked for.
 *   A caller needing one is a card.
 *
 * ---------------------------------------------------------------------------
 * Questions for approval
 * ---------------------------------------------------------------------------
 *
 * Q1. Which module holds it?
 *     a. (Recommended, drafted) SpudFiles: spudfile_library_open, _find,
 *        _close, a handle made from a spudfile_instance. A library is
 *        opened from a path, and SpudFiles already has what that needs:
 *        the instance with the caller's allocator (the handle, the UTF-16
 *        path on Windows), the WTF-8 converter, the Debug quarantine
 *        (card X27: the instance owns files, listings and mappings, and
 *        now libraries), and the threading rule (calls overlap; the
 *        loaders are thread-safe). Its file comment's "files and paths"
 *        grows by one line.
 *     b. A module of its own (SpudLibrary, spudlibrary_*): its own
 *        instance desc, struct types and result block, for three calls,
 *        and a second instance a caller makes for them.
 *     c. SpudCore with no instance, as spudrandom_fill: then no allocator
 *        for the handle or the path's conversion, and no message callback
 *        for the loader's words (S10's out_error carries a number only).
 *     Decided 2026-11-09 with you: a.
 *
 * Q2. The loader's choices?
 *     a. (Recommended, drafted) Three fields on the desc, each with no
 *        default (the zero value refused), each refused with
 *        SPUDRESULT_SPUDFILE_LIBRARY_OPTION_NOT_SUPPORTED where the
 *        platform has no such choice, never mapped to something near it:
 *        - binding: NOW (RTLD_NOW; Windows' only behaviour, its imports
 *          bound at load) or LAZY (RTLD_LAZY; refused on Windows);
 *        - scope: LOCAL (RTLD_LOCAL; Windows' only behaviour, each
 *          module's exports its own) or GLOBAL (RTLD_GLOBAL; refused on
 *          Windows);
 *        - dependencies: PLATFORM_ORDER (Windows LoadLibraryExW with no
 *          search flag, the standard order; POSIX the loader's rules, the
 *          library's rpath and runpath first) or LIBRARY_DIRECTORY_FIRST
 *          (Windows LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
 *          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, the path fully qualified or
 *          refused with SPUDRESULT_DESC_INVALID_PARAMETERS; refused on
 *          POSIX, where the library chose that at link time with $ORIGIN
 *          or @loader_path).
 *        The desc is based, so a later choice (RTLD_NOLOAD, RTLD_NODELETE,
 *        RTLD_DEEPBIND, LOAD_LIBRARY_SEARCH_USER_DIRS) chains on.
 *     b. Each platform's flags raw (a uint32_t passed to dlopen, another
 *        to LoadLibraryExW): every choice the platform has, but the
 *        caller writes platform code and SpudLib checks nothing.
 *     c. Fixed (NOW, LOCAL, the platform's order): a default that hides
 *        three choices, which the one rule forbids.
 *     Decided 2026-11-09 with you: a.
 *
 * Q3. A path with no directory in it ("libvulkan.so.1", "d3d12.dll")?
 *     a. (Recommended, drafted) Passed as given: the platform searches
 *        for it by its own rules (dlopen's: LD_LIBRARY_PATH, the cache,
 *        the default directories; Apple's dyld rules; LoadLibraryExW's
 *        order, or with LIBRARY_DIRECTORY_FIRST the application's and
 *        system directories), which differ, as the header says. Loading a
 *        system library by name is a common, legitimate call.
 *     b. Refused (SPUDRESULT_DESC_INVALID_PARAMETERS): every path
 *        names one file, and no search can find a planted one; but a
 *        caller can't then load a system library whose directory differs
 *        between distributions.
 *     Decided 2026-11-09 with you: a.
 *
 * Q4. What a lookup returns?
 *     a. (Recommended, drafted) A function pointer type,
 *        spudfile_library_function (void (*)(void)), which the caller
 *        casts to the function's own type. C allows a function pointer to
 *        be cast to another function pointer type and back; converting a
 *        void * to one is outside ISO C (POSIX makes dlsym's work; MSVC
 *        warns). Exported data is not offered: a library exposes data
 *        through a function that returns its address.
 *     b. A void *, as dlsym: data symbols too, and the cast out of ISO C
 *        left at every caller.
 *     Decided 2026-11-09 with you: a.
 *
 * Q5. Where are the calls?
 *     a. (Recommended, drafted) On every platform SpudFiles builds for,
 *        iOS, iPadOS, tvOS and watchOS included, where dlopen exists and
 *        loads what the platform allows (a signed framework in the app's
 *        bundle); what a store's rules allow is the caller's, not
 *        SpudLib's to decide.
 *     b. Compiled out where a store forbids loading code
 *        (SPUDFILES_EXT_LIBRARY 0 there): a policy that isn't the
 *        platform's API, and a framework in the bundle could no longer be
 *        opened late.
 *     c. Present and refusing there at run time: the same policy, found
 *        later.
 *     Decided 2026-11-09 with you: a.
 *
 * Q6. How a refusal is reported?
 *     a. (Recommended, drafted) One code for a library the loader didn't
 *        load, SPUDRESULT_SPUDFILE_LIBRARY_NOT_LOADED, whatever the cause
 *        (no file there, a missing dependency, a wrong architecture, an
 *        initializer that failed, a signature refused): on POSIX dlopen
 *        says which only in words, and Windows' ERROR_MOD_NOT_FOUND names
 *        the library or any dependency alike, so no code per cause holds
 *        on every platform. The loader's words go to the instance's
 *        message callback (card S10), whole: dlerror's text, and
 *        GetLastError's number. So SpudFiles takes S10's callback with
 *        this: spudfile_instance_desc gains message_callback and
 *        message_user_data, and every SpudFiles platform failure is sent,
 *        as S10 says (built for SpudFiles first, the other modules after
 *        as S10 plans). S10's SPUD_PLATFORM_ERROR_SOURCE gains DLERROR,
 *        a dynamic loader's message with no number (code 0, the text
 *        dlerror's). A caller wanting the cause in code asks
 *        spudfile_path_get_info() first whether the file is there.
 *     b. Built before S10, the code alone: no change to the instance desc
 *        now, but a failed load says nothing of why until S10 is built,
 *        and a loader's refusal is the hardest failure to guess at.
 *     Decided 2026-11-09 with you: a. SpudFiles is the first module built
 *     with S10's callback; the rest follow S10's own plan.
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * Added: the spudfile_library handle, spudfile_library_desc (based,
 * SPUD_STRUCT_TYPE_FILES_LIBRARY_DESC), three enums, the function pointer
 * type and three calls; SPUDRESULT_SPUDFILE_INVALID_LIBRARY,
 * _LIBRARY_NOT_LOADED, _SYMBOL_NOT_FOUND and _LIBRARY_OPTION_NOT_SUPPORTED,
 * appended after 1020 (numbers given when built, in build
 * order). Under Q6a: spudfile_instance_desc gains two fields at its end
 * (unreleased, before 1.0: a listed break for every caller that fills one,
 * which zeroes them as no callback), and SPUD_PLATFORM_ERROR_SOURCE_DLERROR
 * is appended to S10's enum. spudfiles.h's file comment changes ([CHANGED]
 * below). SPUDLIB_VERSION_MINOR is raised with the build.
 * ./check.sh --update-abi lists the additions.
 *
 * ---------------------------------------------------------------------------
 * Tests (spudfiles_test, extended; every platform; two small libraries
 * built by tests/CMakeLists.txt: spudfiles_test_library_a and _b, each
 * exporting spudfiles_test_value(), returning 1 and 2, and an initializer
 * that counts)
 * ---------------------------------------------------------------------------
 *
 * - Each argument refused in order, and each enum's zero and out-of-range
 *   values; every option the platform lacks refused with OPTION_NOT_SUPPORTED
 *   (LAZY and GLOBAL on Windows, LIBRARY_DIRECTORY_FIRST on POSIX), and
 *   LIBRARY_DIRECTORY_FIRST with a relative path on Windows.
 * - A library opened, its function found and called (1), closed; the
 *   initializer run once at open; a missing name SYMBOL_NOT_FOUND.
 * - The same path opened twice: two handles, one image (a counter in the
 *   library shared), each closed on its own.
 * - Two copies of one library under two paths: two images (separate
 *   counters); a copy of b loaded beside a, each function its own.
 * - Windows: the library file replaced while its copy is loaded (the copy's
 *   path free, the original's not locked).
 * - Refusals, each NOT_LOADED, each with one message (origin PLATFORM,
 *   source DLERROR or WIN32, the text naming the file): no file there; a
 *   text file with the library's extension; a library whose dependency is
 *   missing (built for the test without it); an initializer that fails
 *   (DllMain FALSE; a constructor can't fail on POSIX, so Windows only).
 * - A path with no directory: a system library by name (kernel32.dll;
 *   libc.so.6 on Linux; /usr/lib/libSystem.B.dylib's name on Apple) opened
 *   with PLATFORM_ORDER (Q3a).
 * - A non-ASCII directory and file name; a 259-character path on Windows.
 * - Two threads opening, finding and closing at once (TSan on macOS and
 *   Linux).
 * - The misuse harness: every call with zeroes, and in Debug with a
 *   destroyed instance and a closed library (the library's factory in
 *   tests/misuse/make_misuse_tests.py; quarantined by its instance, X27).
 * - The allocator: every block freed with its size and alignment
 *   (spudtest_allocator.h); a failed allocation at each point refused with
 *   OUT_OF_MEMORY and the library not left loaded.
 * - iOS simulator: a framework embedded in the test's bundle opened (Q5a),
 *   when the simulator tests run.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * From memory, not checked on the web; read as such.
 *
 * - The platforms: dlopen, dlsym, dlclose, dlerror (POSIX; glibc's and
 *   Apple's dyld's modes: RTLD_NOW/LAZY, LOCAL/GLOBAL, NOLOAD, NODELETE,
 *   glibc's DEEPBIND); LoadLibraryExW (its LOAD_LIBRARY_SEARCH_* flags, the
 *   fully qualified path they need), GetProcAddress, FreeLibrary,
 *   GetLastError; DllMain's loader-lock rules; Apple's rule that images
 *   with Objective-C or Swift metadata are not unloaded.
 * - SDL3's SDL_LoadObject, SDL_LoadFunction (returning SDL_FunctionPointer,
 *   void (*)(void), for the same ISO C reason as Q4a) and SDL_UnloadObject:
 *   no flags offered (RTLD_NOW | RTLD_LOCAL fixed on POSIX), the error in
 *   SDL_GetError. Taken: the function pointer type. Not taken: the fixed
 *   flags (Q2c).
 * - Rust's libloading: Library::new (fixed flags) beside
 *   os::unix::Library::open(flags) and os::windows::Library::load_with_flags
 *   (each platform's raw flags, Q2b), get::<T> returning a typed symbol
 *   tied to the library's lifetime. Taken: the per-platform choices made
 *   visible. Not taken: raw flags (Q2b).
 * - .NET's NativeLibrary.Load, TryGetExport, Free (a DllImportSearchPath
 *   for the search, Q2a's dependencies field in another form).
 * - The Vulkan loader's own loading of drivers and layers (dlopen with
 *   RTLD_LAZY | RTLD_LOCAL; LoadLibraryExW with
 *   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS where a path is not absolute), and
 *   cr.h's and Godot's loading of a copied library (the reason a second
 *   image needs a path of its own).
 */

/* [CHANGED] spudfiles.h's file comment, its first paragraph. Before:
 *
 *   Opening, reading and writing at an offset, sizing, flushing, mapping and
 *   locking a file; what is at a path, replacing and removing a file, copying
 *   a file's permissions to another; making,
 *   listing, renaming and removing a directory.
 *
 * After, one line added:
 *
 *   ...listing, renaming and removing a directory; opening a dynamic
 *   library and finding a function it exports.
 *
 * Under Q6a, the rule "the platform's own number is not kept (not yet)"
 * becomes: "every failure a platform call reports goes to the instance's
 * message callback (spudcore.h, "Messages")". */

/* ---- spudcore.h ------------------------------------------------------- */

/* [ADDED] Appended to SPUDRESULT after SPUDRESULT_SPUDFILE_NOT_EMPTY;
 * numbers given when built. */
SPUDRESULT_SPUDFILE_INVALID_LIBRARY,        /**< A library argument is NULL. */
SPUDRESULT_SPUDFILE_LIBRARY_NOT_LOADED,     /**< The platform's loader didn't load the library; the instance's message callback has its words. */
SPUDRESULT_SPUDFILE_SYMBOL_NOT_FOUND,       /**< The library exports no function of that name. */
SPUDRESULT_SPUDFILE_LIBRARY_OPTION_NOT_SUPPORTED, /**< The platform's loader has no such choice. */

/* [ADDED] Registered in SPUD_STRUCT_TYPE's SpudFiles block. */
SPUD_STRUCT_TYPE_FILES_LIBRARY_DESC = 5005,

/* [ADDED, under Q6a] Appended to S10's SPUD_PLATFORM_ERROR_SOURCE. */
/** A dynamic loader's message with no number (dlerror on Linux and Apple):
 * the code is 0 and the text is the loader's. */
SPUD_PLATFORM_ERROR_SOURCE_DLERROR = 10,

/* ---- spudfiles.h ------------------------------------------------------ */

/* [CHANGED, under Q6a] spudfile_instance_desc: two fields at its end, as
 * S10's draft writes them for every instance desc. */
typedef struct spudfile_instance_desc {
	SPUD_STRUCT_BASE base;
	const SPUD_ALLOCATOR *allocator;
	/** [ADDED] Called with every message of this instance and its objects
	 * (spudcore.h, "Messages"), or NULL for none. */
	SPUD_MESSAGE_CALLBACK message_callback;
	/** [ADDED] Handed to #message_callback unchanged. */
	void *message_user_data;
} spudfile_instance_desc;

/** [ADDED] Opaque handle to an open dynamic library. */
typedef struct spudfile_library_t *spudfile_library;

/** [ADDED] When the library's references to other libraries are bound. */
typedef enum SPUDFILE_LIBRARY_BINDING {
	/** Not chosen: refused. */
	SPUDFILE_LIBRARY_BINDING_NONE = 0,
	/** All of them, before the open returns (RTLD_NOW; Windows' imports). */
	SPUDFILE_LIBRARY_BINDING_NOW = 1,
	/** Each function's when it is first called (RTLD_LAZY). Not on
	 * Windows. */
	SPUDFILE_LIBRARY_BINDING_LAZY = 2,
} SPUDFILE_LIBRARY_BINDING;

/** [ADDED] Whether the library's exports are seen by libraries opened
 * after it. */
typedef enum SPUDFILE_LIBRARY_SCOPE {
	/** Not chosen: refused. */
	SPUDFILE_LIBRARY_SCOPE_NONE = 0,
	/** Only through this handle (RTLD_LOCAL; every Windows module). */
	SPUDFILE_LIBRARY_SCOPE_LOCAL = 1,
	/** Also to libraries opened later (RTLD_GLOBAL). Not on Windows. */
	SPUDFILE_LIBRARY_SCOPE_GLOBAL = 2,
} SPUDFILE_LIBRARY_SCOPE;

/** [ADDED] Where the libraries it depends on (and, for a path with no
 * directory, the library itself) are searched for. */
typedef enum SPUDFILE_LIBRARY_DEPENDENCIES {
	/** Not chosen: refused. */
	SPUDFILE_LIBRARY_DEPENDENCIES_NONE = 0,
	/** The platform's own order: LoadLibraryExW's standard search order
	 * (no search flag); POSIX's loader rules, the library's own rpath and
	 * runpath among them. */
	SPUDFILE_LIBRARY_DEPENDENCIES_PLATFORM_ORDER = 1,
	/** The library's own directory, then the application's and the
	 * system's (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
	 * LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); the path must be fully qualified.
	 * Windows only: on POSIX the library chose this when it was linked
	 * ($ORIGIN, @loader_path). */
	SPUDFILE_LIBRARY_DEPENDENCIES_LIBRARY_DIRECTORY_FIRST = 2,
} SPUDFILE_LIBRARY_DEPENDENCIES;

/** [ADDED] What the caller decides when opening a library. No field has a
 * default. */
typedef struct spudfile_library_desc {
	/** `type` is SPUD_STRUCT_TYPE_FILES_LIBRARY_DESC; `next` is NULL
	 * (nothing is chained onto this struct yet). Set by the caller. See
	 * SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** The library: a path, or a name with no directory, which the
	 * platform searches for by its own rules (#dependencies). Nothing is
	 * added to it. */
	const char *path;
	SPUDFILE_LIBRARY_BINDING binding;           /**< When references are bound. */
	SPUDFILE_LIBRARY_SCOPE scope;               /**< Who sees its exports. */
	SPUDFILE_LIBRARY_DEPENDENCIES dependencies; /**< Where dependencies are searched for. */
	/** Name for debugging, or NULL. Copied; see spud_debug_name_set(). */
	const char *debug_name;
} spudfile_library_desc;

/**
 * @brief [ADDED] Opens a dynamic library: LoadLibraryExW, or dlopen. Runs
 * its initializers, on this thread, before returning. Two opens of one
 * path give two handles over one loaded image.
 *
 * Thread: may overlap any call on the same instance but
 * spudfile_instance_destroy().
 *
 * @param[in]  instance    The instance the handle is made with.
 * @param[in]  desc        What the caller decides.
 * @param[out] out_library Receives the library; NULL on failure.
 *
 * @retval SPUD_SUCCESS The library is loaded.
 * @retval SPUDRESULT_SPUDFILE_INVALID_INSTANCE `instance` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_FILES_LIBRARY_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_library` is NULL.
 * @retval SPUDRESULT_SPUDFILE_NULL_PATH `desc->path` is NULL or empty.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS `desc->binding`, `->scope` or
 *         `->dependencies` is NONE or not one of its values, or
 *         LIBRARY_DIRECTORY_FIRST with a path that isn't fully qualified.
 * @retval SPUDRESULT_SPUDFILE_LIBRARY_OPTION_NOT_SUPPORTED The platform's
 *         loader has no such choice: BINDING_LAZY or SCOPE_GLOBAL on
 *         Windows, DEPENDENCIES_LIBRARY_DIRECTORY_FIRST on POSIX.
 * @retval SPUDRESULT_SPUDFILE_LIBRARY_NOT_LOADED The loader refused, for
 *         any reason (no file, a dependency missing, another architecture,
 *         an initializer that failed, a signature refused); its words go to
 *         the instance's message callback.
 * @retval SPUDRESULT_OUT_OF_MEMORY The handle could not be allocated; the
 *         library is not left loaded.
 */
SPUDRESULT spudfile_library_open(
    spudfile_instance instance,
    const spudfile_library_desc *desc,
    spudfile_library *out_library);

/** [ADDED] A function a library exports, as found: cast to its own type
 * before calling it. Valid until the library is closed. */
typedef void (*spudfile_library_function)(void);

/**
 * @brief [ADDED] Finds a function the library itself exports, by name:
 * GetProcAddress, or dlsym. What it finds in the library's dependencies is
 * undefined (dlsym searches them; GetProcAddress doesn't).
 *
 * Thread: may overlap any call on the same library but
 * spudfile_library_close() and spud_debug_name_set().
 *
 * @param[in]  library      The library.
 * @param[in]  name         The exported name, as the library exports it,
 *                          null-terminated. Nothing is added or removed.
 * @param[out] out_function Receives the function; NULL on failure.
 *
 * @retval SPUD_SUCCESS `*out_function` is the function.
 * @retval SPUDRESULT_SPUDFILE_INVALID_LIBRARY `library` is NULL.
 * @retval SPUDRESULT_NULL_DESC `name` is NULL or empty.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_function` is NULL.
 * @retval SPUDRESULT_SPUDFILE_SYMBOL_NOT_FOUND The library exports no
 *         function of that name (also sent as a message, with the
 *         loader's words).
 */
SPUDRESULT spudfile_library_find(
    spudfile_library library,
    const char *name,
    spudfile_library_function *out_function);

/**
 * @brief [ADDED] Closes a library: FreeLibrary, or dlclose. The platform
 * unloads the image when its last open is closed, if it unloads it at all;
 * either way nothing found through this handle may be called after. A
 * failure the platform reports goes to the instance's message callback.
 *
 * Thread: overlaps no other call on the same library.
 *
 * @param[in] library The library to close. NULL does nothing.
 */
void spudfile_library_close(spudfile_library library);
