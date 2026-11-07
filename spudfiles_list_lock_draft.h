/*
 * spudfiles_list_lock_draft.h - SpudFiles: listing a directory, and an
 * advisory lock on an open file (card S39, decided 2026-11-06 with the
 * user, a; SPUDFILES_TODO.md).
 *
 * DRAFT, 2026-11-06, second version, for approval. Not built, not in any
 * target. Not compilable: it repeats spudcore.h's enums to show them
 * changed. Every declaration is marked [ADDED] or [CHANGED]. Written
 * against include/spudfiles.h and include/spudcore.h as read on
 * 2026-11-06.
 *
 * Second version, after the user's review the same day: the handle is a
 * listing (spudfile_listing), not a "directory", beside the existing
 * spudfile_directory_create() that makes a folder on disk; Windows'
 * reparse points classed by their tag, so a OneDrive placeholder is a
 * file; no UNKNOWN kind (the POSIX backend asks the file system itself);
 * a file records the lock it holds, so locking twice and unlocking none
 * answer the same on every backend; Q3 answered with WTF-8 on Windows;
 * the SQLite comparison corrected; network file systems, inheritance,
 * an empty drive root, a failed listing and hidden entries stated.
 *
 * ---------------------------------------------------------------------------
 * What it translates
 * ---------------------------------------------------------------------------
 *
 * Listing: opendir/readdir/closedir on POSIX (Linux and every Apple
 * system), FindFirstFileExW/FindNextFileW/FindClose on Windows. Locking:
 * flock on POSIX, LockFileEx/UnlockFileEx on Windows. Nothing is sorted,
 * filtered (hidden entries are listed), recursed, retried or timed out:
 * the caller's.
 *
 * ---------------------------------------------------------------------------
 * Questions for approval
 * ---------------------------------------------------------------------------
 *
 * Q1. One call into a buffer, or an iterator? The card's wording was "one
 *     call filling the caller's buffer of entries".
 *     a. (Recommended, drafted) An iterator: open, next, close - what
 *        readdir and FindNextFileW are, one entry a call into the caller's
 *        struct. No buffer to size, no count to ask for first, and no race
 *        between asking and filling. The listing is a handle because it
 *        holds the platform's own state (a DIR *, or a find HANDLE and the
 *        entry FindFirstFileExW returns at open), which the caller's memory
 *        can't hold without native types in the header, and the
 *        instance's allocator, as every object does. Not a spudfile_file:
 *        every file call would need a refusal of a directory, where a type
 *        of its own makes the mistake a compile error. Not a batch of
 *        packed entries (getdents64, getattrlistbulk,
 *        GetFileInformationByHandleEx): the platforms already fetch in
 *        batches behind readdir and FindNextFileW (FIND_FIRST_EX_LARGE_FETCH),
 *        so an entry a call costs a copy, not a system call.
 *     b. One call into the caller's buffer of entries and names, returning
 *        the count needed when it is too small: a second call to retry,
 *        whose answer may differ.
 *
 * Q2. "." and "..". POSIX's readdir gives both; FindFirstFileExW gives both
 *     except at a drive's root, where it gives neither.
 *     a. (Recommended, drafted) Never given, on any backend: they are no
 *        entries of the directory, and passing them through would make the
 *        same listing differ between platforms.
 *     b. Given as the platform gives them.
 *
 * Q3. A name that isn't UTF-8. A Linux name is any bytes but '/' and NUL;
 *     a Windows name is UTF-16 that may hold an unpaired surrogate. Every
 *     SpudFiles path is UTF-8.
 *     a. (Recommended, drafted) The entry is given, `name_is_utf8` false,
 *        with a name that opens it: on POSIX the platform's bytes as they
 *        are (POSIX takes bytes); on Windows the name as WTF-8 (UTF-8 that
 *        also encodes an unpaired surrogate as its three bytes, as Rust's
 *        OsString does on Windows), and [CHANGED] every SpudFiles path on
 *        Windows is read as WTF-8, so a path made from it opens the entry.
 *        Valid UTF-8 is valid WTF-8 and converts as before; what changes
 *        is that a path holding an encoded surrogate, answered today with
 *        SPUDRESULT_SPUDFILE_NOT_FOUND (spudfileswindows.c: "no file can
 *        have that name"), now reaches the file. The
 *        Windows backend's converter becomes SpudFiles' own (a
 *        MultiByteToWideChar with MB_ERR_INVALID_CHARS refuses
 *        surrogates); POSIX is unchanged.
 *     b. As a, but on Windows `name` empty and no path change: the entry
 *        is seen and can't be opened or shown.
 *     c. Such an entry is skipped, and counted on the listing.
 *
 * Q4. Advisory on Windows too. LockFileEx is mandatory: a locked range
 *     refuses other handles' reads and writes. flock is advisory: it
 *     refuses only other locks.
 *     a. (Recommended, drafted) The Windows backend locks one byte at
 *        offset 2^64 - 2, past any data a file can hold (Windows allows a
 *        lock past the end of a file), so the lock refuses other locks and
 *        never a read or write, as flock: the same meaning on every
 *        backend. Ours to test, not precedent: SQLite's lock bytes sit at
 *        1 GiB, a range it keeps its pages out of, not past every file's
 *        end. The tests run it on NTFS, ReFS, exFAT and an SMB share.
 *     b. The whole file, mandatory on Windows and advisory on POSIX, said
 *        on the call (Rust's File::lock does this).
 *
 * Also stated, not asked: a file holds at most one lock taken here, and
 * the file records which (one byte, touched only by lock and unlock), so
 * locking a file that holds one and unlocking one that holds none are
 * refused in the front end, the same on every backend, before any
 * platform call (flock would convert the first, LockFileEx stack it;
 * UnlockFileEx fails the second and flock doesn't). A lock is changed
 * (shared to exclusive) by unlocking and locking again, which isn't
 * atomic. A waiting lock has no limit and no cancel: a caller that wants
 * a limit asks with wait false, as often as it chooses.
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * Added: one handle, two based structs (SPUD_STRUCT_TYPE_FILES_
 * LISTING_DESC 5003, _LISTING_ENTRY 5004), two enums, a macro, six result
 * codes (1014-1019, the next free of SpudFiles'), five calls. Changed:
 * what a path may hold on Windows (Q3), a widening, no break. The entry
 * holds its name by value (1 KiB), so a listing makes no allocation per
 * entry; the caller keeps one on its stack or beside the listing.
 *
 * ---------------------------------------------------------------------------
 * Tests (spudfiles_test, extended; every backend)
 * ---------------------------------------------------------------------------
 *
 * - A folder made in the test with files, a directory, a symbolic link
 *   and (Windows) a junction, where the platform lets the test make them:
 *   every entry listed once, its kind right, "." and ".." never, a hidden
 *   entry listed; an empty folder ends at once; a non-ASCII name
 *   round-trips (listed, then opened by it); a file listed refused
 *   (NOT_A_DIRECTORY), a missing one NOT_FOUND; every argument refused in
 *   order, nothing made; next after the end keeps answering
 *   END_OF_LISTING.
 * - Names that aren't UTF-8: on Linux, a file named with an invalid byte,
 *   listed with name_is_utf8 false and opened by its bytes; on Windows, a
 *   file named with an unpaired surrogate (made through CreateFileW in the
 *   test), listed as WTF-8 and opened by it, and the path converter's
 *   cases (valid UTF-8 unchanged, an encoded surrogate pair refused as
 *   WTF-8 forbids, overlong forms refused).
 * - Windows' kinds: a OneDrive placeholder, where the machine has one,
 *   listed as a file or directory, not a link (reported as not run where
 *   it hasn't); an AF_UNIX socket as OTHER.
 * - Locks between two files opened on one path (which behave as two
 *   processes' do for flock and LockFileEx on a local file system):
 *   exclusive refuses another exclusive and a shared without waiting
 *   (LOCKED); shared allows shared; unlock lets the other take it; close
 *   releases; a locked file still read and written through the other file
 *   (Q4's advisory meaning); locking twice ALREADY_LOCKED, unlocking none
 *   NOT_LOCKED, on every backend; a waiting lock taken once the holder
 *   unlocks, on a second thread. On Windows on NTFS, ReFS, exFAT and an
 *   SMB share where the machine has them, each reported.
 * - The misuse harness: the new calls with zeroes and with a destroyed
 *   listing and file.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * Listing: POSIX readdir (d_type, DT_UNKNOWN on some file systems), Win32
 * FindFirstFileExW (FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH, the
 * reparse tag in dwReserved0), C++'s std::filesystem::directory_iterator
 * and Rust's read_dir (iterators that skip "." and ".."), Python's
 * os.scandir (the kind from the listing, an lstat where it gives none).
 * Names: Rust's OsString and WTF-8 on Windows. Locking: flock, fcntl's
 * F_SETLK (per process, released when the process closes any descriptor
 * of the file: rejected for that), LockFileEx, Rust's File::lock (flock
 * and whole-file LockFileEx), SQLite's lock bytes at 1 GiB.
 */

/* ======================================================================== */
/* spudcore.h                                                               */
/* ======================================================================== */

/* [ADDED] to SPUDRESULT, after SPUDRESULT_SPUDFILE_NOT_WRITABLE = 1013: */

	SPUDRESULT_SPUDFILE_END_OF_LISTING = 1014, /**< A listing has given every entry. An answer, not a failure. */
	SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY = 1015, /**< A file, or another kind of entry, is where a directory is wanted. */
	SPUDRESULT_SPUDFILE_LOCKED = 1016, /**< Another lock holds the file, and the call was not to wait. */
	SPUDRESULT_SPUDFILE_INVALID_LISTING = 1017, /**< A listing argument is NULL. */
	SPUDRESULT_SPUDFILE_ALREADY_LOCKED = 1018, /**< The file holds a lock taken through it already. */
	SPUDRESULT_SPUDFILE_NOT_LOCKED = 1019, /**< The file holds no lock taken through it. */

/* [ADDED] to SPUD_STRUCT_TYPE, after SPUD_STRUCT_TYPE_FILES_PATH_INFO = 5002: */

	SPUD_STRUCT_TYPE_FILES_LISTING_DESC  = 5003, /**< spudfile_listing_desc. */
	SPUD_STRUCT_TYPE_FILES_LISTING_ENTRY = 5004, /**< SPUDFILE_LISTING_ENTRY, an out struct. */

/* ======================================================================== */
/* spudfiles.h                                                              */
/* ======================================================================== */

/* [CHANGED] The header's rule on paths, Windows' half (Q3):
 *
 *     A path is UTF-8, null-terminated. On Windows it is read as WTF-8:
 *     UTF-8 that may also hold an unpaired UTF-16 surrogate as its three
 *     bytes, so every name a listing gives opens, whatever it holds. A
 *     path that isn't WTF-8 names nothing: SPUDRESULT_SPUDFILE_NOT_FOUND,
 *     as one that isn't UTF-8 does today.
 */

/* ---- Listing a directory [ADDED] ---- */

/** [ADDED] Opaque handle to an open listing of a directory's entries. */
typedef struct spudfile_listing_t *spudfile_listing;

/** [ADDED] What an entry of a directory is, as the listing says, without
 * following a link. The same kinds on every backend. */
typedef enum SPUDFILE_ENTRY_KIND {
	SPUDFILE_ENTRY_KIND_FILE      = 1, /**< A file. */
	SPUDFILE_ENTRY_KIND_DIRECTORY = 2, /**< A directory. */
	/** A symbolic link; on Windows also a junction (a mount point).
	 * Windows' other reparse points (a cloud placeholder, a deduplicated
	 * file) are the file or directory they stand for. */
	SPUDFILE_ENTRY_KIND_SYMBOLIC_LINK = 3,
	SPUDFILE_ENTRY_KIND_OTHER         = 4, /**< A device, socket, pipe or the like. */
} SPUDFILE_ENTRY_KIND;

/** [ADDED] The room an entry's name has: the longest name of any platform
 * (255 UTF-16 units on Windows and HFS+, at most 765 bytes of WTF-8; 255
 * bytes on Linux and APFS), its null and room to spare. */
#define SPUDFILE_NAME_CAPACITY 1024

/** [ADDED] What the caller decides when opening a listing. No field has a
 * default. */
typedef struct spudfile_listing_desc {
	/** `type` is SPUD_STRUCT_TYPE_FILES_LISTING_DESC; `next` is NULL. Set
	 * by the caller. See SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE base;
	/** The directory. A file, or anything but a directory, is refused with
	 * SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY. */
	const char *path;
	/** Name for debugging, or NULL. Copied; see spud_debug_name_set(). */
	const char *debug_name;
} spudfile_listing_desc;

/** [ADDED] One entry of a listing, filled by spudfile_listing_next(). */
typedef struct SPUDFILE_LISTING_ENTRY {
	/** `type` is SPUD_STRUCT_TYPE_FILES_LISTING_ENTRY; `next` is NULL. The
	 * caller sets the type; SpudFiles fills in the rest. See
	 * SPUD_STRUCT_BASE. */
	SPUD_STRUCT_BASE_OUT base;
	SPUDFILE_ENTRY_KIND kind; /**< What it is. */
	/** True when `name` is valid UTF-8 (Q3). False for a POSIX name that
	 * isn't, its bytes then given as they are, and a Windows name holding
	 * an unpaired surrogate, given as WTF-8. Either way a path made from
	 * `name` opens the entry. */
	bool name_is_utf8;
	/** Bytes in `name`, its null not counted. */
	uint32_t name_size;
	/** The name alone, no directory before it, null-terminated. */
	char name[SPUDFILE_NAME_CAPACITY];
} SPUDFILE_LISTING_ENTRY;

/**
 * @brief [ADDED] Opens a listing of a directory's entries: opendir, or
 * FindFirstFileExW (FindExInfoBasic, with a large fetch). They are then
 * given one a call by spudfile_listing_next(), in the order the platform
 * gives them, hidden ones included, "." and ".." never (Q2). Entries made
 * or removed while it is open may or may not be given, as the platform has
 * it. An empty drive root on Windows, which FindFirstFileExW answers with
 * ERROR_FILE_NOT_FOUND, is an empty listing.
 *
 * Thread: may overlap any call on the same instance but
 * spudfile_instance_destroy().
 *
 * @param[in]  instance    The instance the listing is made with.
 * @param[in]  desc        What the caller decides.
 * @param[out] out_listing Receives the listing; NULL on failure.
 *
 * @retval SPUD_SUCCESS The listing is open.
 * @retval SPUDRESULT_SPUDFILE_INVALID_INSTANCE `instance` is NULL.
 * @retval SPUDRESULT_NULL_DESC `desc` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `desc->base.type` is not
 *         SPUD_STRUCT_TYPE_FILES_LISTING_DESC.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `desc->base.next` is not NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_listing` is NULL.
 * @retval SPUDRESULT_SPUDFILE_NULL_PATH `desc->path` is NULL or empty.
 * @retval SPUDRESULT_SPUDFILE_NOT_FOUND Nothing is there, or a directory on
 *         the way to it isn't (a Windows path that isn't WTF-8 among
 *         them).
 * @retval SPUDRESULT_SPUDFILE_NOT_A_DIRECTORY Something other than a
 *         directory is there (on Windows told apart from a missing parent
 *         by the path's attributes, since FindFirstFileExW answers both
 *         alike).
 * @retval SPUDRESULT_SPUDFILE_ACCESS_DENIED The platform won't list it.
 * @retval SPUDRESULT_OUT_OF_MEMORY The listing could not be allocated.
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE The platform refused for another
 *         reason.
 */
SPUDRESULT spudfile_listing_open(
    spudfile_instance instance,
    const spudfile_listing_desc *desc,
    spudfile_listing *out_listing);

/**
 * @brief [ADDED] Gives the listing's next entry: readdir, or FindNextFileW.
 * Where readdir gives no kind (DT_UNKNOWN, on some file systems) the POSIX
 * backend asks with fstatat(AT_SYMLINK_NOFOLLOW), and skips an entry
 * removed before it could ask. On Windows a reparse point's tag decides
 * whether it is a link (IO_REPARSE_TAG_SYMLINK, _MOUNT_POINT), a socket
 * (IO_REPARSE_TAG_AF_UNIX, OTHER) or the file or directory its attributes
 * say.
 *
 * Thread: one caller at a time on a listing (it moves the platform's own
 * place in the directory); may overlap calls on other objects of the
 * instance.
 *
 * @param[in]  listing   The listing.
 * @param[out] out_entry Receives the entry. Its `base.type` is set by the
 *                       caller.
 *
 * @retval SPUD_SUCCESS `*out_entry` is the next entry.
 * @retval SPUDRESULT_SPUDFILE_END_OF_LISTING Every entry has been given;
 *         every later call says so too. `*out_entry` is unchanged.
 * @retval SPUDRESULT_SPUDFILE_INVALID_LISTING `listing` is NULL.
 * @retval SPUDRESULT_NULL_OUTPUT_PARAMETER `out_entry` is NULL.
 * @retval SPUDRESULT_STRUCT_WRONG_TYPE `out_entry->base.type` is not
 *         SPUD_STRUCT_TYPE_FILES_LISTING_ENTRY.
 * @retval SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN `out_entry->base.next` is not
 *         NULL.
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE The platform failed to read the
 *         directory (it may have gone). The listing can then only be
 *         closed: what a later next gives is undefined.
 */
SPUDRESULT spudfile_listing_next(
    spudfile_listing listing,
    SPUDFILE_LISTING_ENTRY *out_entry);

/**
 * @brief [ADDED] Closes a listing: closedir, or FindClose. NULL does
 * nothing.
 *
 * @param[in] listing The listing.
 */
void spudfile_listing_close(spudfile_listing listing);

/* ---- Locking a file [ADDED] ---- */

/** [ADDED] What a lock lets others have. */
typedef enum SPUDFILE_LOCK {
	SPUDFILE_LOCK_NONE      = 0, /**< Not a lock: refused. */
	SPUDFILE_LOCK_SHARED    = 1, /**< Others may hold shared locks too. */
	SPUDFILE_LOCK_EXCLUSIVE = 2, /**< No other lock while it is held. */
} SPUDFILE_LOCK;

/**
 * @brief [ADDED] Takes an advisory lock on an open file: flock, or
 * LockFileEx on one byte at offset 2^64 - 2, past any data a file can hold
 * (Q4), so it refuses other locks and never another file's reads or
 * writes, on every backend. Held until spudfile_file_unlock() or the
 * file's close.
 *
 * Advisory: only callers that lock are kept apart. Per open file, as
 * flock is: two files opened on one path exclude each other, in one
 * process or two. Where that fails:
 * - Over NFS, Linux emulates flock with POSIX record locks, which belong
 *   to a process: two files of one process there don't exclude each
 *   other, and other network file systems may not honour a lock at all.
 * - A child process that inherited the descriptor keeps the lock held
 *   while it has it open. SpudFiles opens every file with O_CLOEXEC, so
 *   only a descriptor the caller handed on is inherited.
 *
 * A file holds at most one lock taken through it, and records which: a
 * second is refused (ALREADY_LOCKED) without asking the platform.
 *
 * Thread: may overlap a read, write, size, flush or map on the same file,
 * and any call on other files; not spudfile_file_unlock() or
 * spudfile_file_close() on the same file. With @p wait, blocks the
 * calling thread until the lock is had, with no limit and no cancel: a
 * caller that wants a limit asks with @p wait false, as often as it
 * chooses.
 *
 * @param[in] file The file; open for reading, writing or both.
 * @param[in] lock SHARED or EXCLUSIVE.
 * @param[in] wait true: wait until it can be had. false: refuse at once.
 *
 * @retval SPUD_SUCCESS The lock is held.
 * @retval SPUDRESULT_SPUDFILE_INVALID_FILE `file` is NULL.
 * @retval SPUDRESULT_DESC_INVALID_PARAMETERS `lock` is not SHARED or
 *         EXCLUSIVE.
 * @retval SPUDRESULT_SPUDFILE_ALREADY_LOCKED The file holds a lock taken
 *         through it.
 * @retval SPUDRESULT_SPUDFILE_LOCKED Another lock holds it, and @p wait is
 *         false.
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE The platform refused for another
 *         reason (a file system that can't lock among them).
 */
SPUDRESULT spudfile_file_lock(
    spudfile_file file,
    SPUDFILE_LOCK lock,
    bool wait);

/**
 * @brief [ADDED] Lets go of a file's lock: flock with LOCK_UN, or
 * UnlockFileEx on the same byte.
 *
 * Thread: as spudfile_file_lock().
 *
 * @param[in] file The file.
 *
 * @retval SPUD_SUCCESS The lock is gone.
 * @retval SPUDRESULT_SPUDFILE_INVALID_FILE `file` is NULL.
 * @retval SPUDRESULT_SPUDFILE_NOT_LOCKED The file holds no lock taken
 *         through it.
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE The platform refused.
 */
SPUDRESULT spudfile_file_unlock(spudfile_file file);
