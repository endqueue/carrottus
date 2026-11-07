# SpudFiles TODO

Written on 2026-10-20, from item 4 of `MATURITY_TODO.md`, which now points
here. **Done on 2026-10-22:** the proposal below was agreed, with the four
decisions as recommended, and built. What follows "The proposal to start
from" is the record of what was built and what is left. **Renamed on
2026-10-27** (card X1b): the prefix `sfs_` below is now `spudfile_`
(`SPUDFILE_`, `SPUDRESULT_SPUDFILE_*`); the record says the new names.

SpudFiles was written before spudlib's current rules, and breaks several of
them. Converting its structs to struct bases as they are would polish an API
that has to change anyway, so the struct-base step is part of this rework, not
a step of its own.

Begin the session with a proposal for the new `spudfiles.h`, and write code
only once it is agreed.

**Before it:** `MATURITY_TODO.md`'s item 3 is settled for what this needs:
the new instance desc takes a `SPUD_ALLOCATOR`, with no default, and every
result has a name from `spudresult_str`. The new `spudfiles.h` is written to
those rules.

## What is wrong now

- **Writable static data.** The path functions (`spudfile_get_path_components`,
  `spudfile_simplify_path`, `spudfile_split_path`, `spudfile_get_relative_path`,
  `spudfile_file_dialog_multi_path`) and `spudfile_get_working_directory`, the dialogs
  and `run_dialog_helper` return pointers into `static` buffers: not
  reentrant, not thread-safe, and each call overwrites the last answer. These
  are fifteen of spudlib's `mutable-globals` baseline entries.
  `spudfile_set_working_directory` changes the whole process's state.
- **A default chosen for the caller.** `spudfile_default_file_dialog_attributes`
  picks dialog flags on the caller's behalf.
- **GUI.** spudlib owns no window, and GUI belongs to the caller, but
  SpudFiles shows modal dialogs (`spudfile_file_dialog`, `spudfile_open_folder_dialog`);
  the macOS one runs `osascript` and ignores `SPUDFILE_FILE_DIALOG_FLAG_OVERWRITE_PROMPT`.
- **No platform API translated.** The path-string functions (`spudfile_is_slash`,
  `spudfile_correct_slashes`, `spudfile_is_absolute`, `spudfile_split_path`,
  `spudfile_build_path_from_components`, `spudfile_simplify_path`,
  `spudfile_get_relative_path`) are string handling that decides what a path
  means, not a translation of any platform call.
- **Old shapes.** `spudfile_file_open` and `spudfile_file_dialog` take their structs by
  value; results come back as `bool` (`spudfile_file_exists` can't tell "no file"
  from a failure) or `const char *` instead of `SPUDRESULT`. The header's
  `// FileOpenAttribs`-style comments name an older API. `SPUDFILE_FIND_FILE_DATA`
  is used by no function. `include/spudfiles.h:181` ends in a stray `\`
  that splices it onto the next declaration, and
  `spudfile_get_working_directory()` has empty parentheses rather than `(void)`.
  `spudfile_file_read` returns `SPUD_SUCCESS` at the end of the file with fewer
  bytes read than asked and no count, so a short read can't be seen.
- **Windows paths in the code page.** `spudfile_file_open` calls `CreateFileA`
  (`src/files/backends/windows/spudfileswindows.c:70`), so a path outside the
  system code page fails there, while `spudfile_file_replace` already converts to
  UTF-16 for `MoveFileExW`.
- **A share mode chosen for the caller.** The same call passes
  `FILE_SHARE_READ` always.

## What the callers use

As of 2026-10-20, read from the workspace with
`grep -rnoE "\bsfs_[a-z_]+\("` over its other repositories (three files):
`spudfile_file_open`, `_read`, `_write`, `_set_pos`, `_get_size`, `_flush`,
`_replace`, `_remove`, `_map`, `_release` and `spudfile_file_exists`.
Nothing calls the path functions, the working directory calls,
`spudfile_create_directory`, `spudfile_directory_exists` or the dialogs. Run the grep
again before deciding what to delete.

## The proposal to start from

- Delete the dialogs, `spudfile_default_file_dialog_attributes`,
  `SPUDFILE_FILE_DIALOG_ATTRIBUTES` and its flags, and `SPUDFILE_FIND_FILE_DATA`.
- Delete the path-string functions. A caller that needs them owns them.
- Delete `spudfile_set_working_directory` and `spudfile_get_working_directory`, or keep
  only a plain translation that writes into a caller's buffer.
- Every remaining call returns `SPUDRESULT`. A call that reports a string or
  a yes/no answer writes it into the caller's buffer or output parameter
  (`out_buf`, `capacity`, `out_size`, as SpudNet's getters do). A buffer too
  small gets a result of its own and the size it needs, as Vulkan's
  two-call idiom does with `VK_INCOMPLETE`.
- Paths are UTF-8, said once in the header; the Windows backend converts
  every one to UTF-16 and calls only the `W` functions.
- `spudfile_file_exists` becomes a stat-like call (`stat`,
  `GetFileAttributesExW`): `SPUDRESULT_NOT_FOUND`-style for no file, else
  the kind, size and modification time.
- Consider reads and writes at an offset (`pread`/`pwrite`, `ReadFile` with
  an offset), which the platforms have natively and which share no cursor
  between threads, beside or instead of `_set_pos`/`_get_pos`.
- Candidates, only if a caller needs them: listing a directory (an iterator
  over `opendir`/`FindFirstFileW`) and the platform's known folders
  (`SHGetKnownFolderPath`, `NSSearchPathForDirectories`); both translate a
  platform call, unlike the path-string functions.
- `spudfile_file_open` takes a `const SPUDFILE_FILE_OPEN_ATTRIBUTES *` that starts with
  a `SPUD_STRUCT_BASE` (`SPUD_STRUCT_TYPE_FILES_OPEN_DESC`, 5000; perhaps
  renamed to a `_desc`, as the other modules' are), refused with
  `SPUDRESULT_NULL_DESC` when NULL. It is the one struct likely to grow
  (exclusive create, share modes, access hints). Its share mode is the
  caller's, with no default.
- Its checks are written once, in a validate function in `src/spudfiles.c`
  that each backend calls first, as `spudaudio_validate_stream_desc` is for
  SpudAudio, and not three times in the backends. (SpudFiles has one backend
  per platform, so it needs none of SpudInput's dispatch.)
- A test, `tests/spudfiles_test.c`, registered with ctest: the argument
  checks and refusals, and open, write, read, map, flush, replace and remove
  in a temporary directory. Written 2026-10-22 against today's API (passing
  on macOS); the rework moves it to the new calls.
- `CLAUDE.md`: the SpudFiles macOS-backend note under "Known gaps" (the
  `osascript` dialogs) removed with the dialogs.
- `STRUCT_BASE_TODO.md`: its "spudfiles.h" section updated to what was done;
  then its step 6 (that file's classification rewritten as a record, and
  `CLAUDE.md`'s note shortened).
- `check.baseline`: remove the three `spudfiles.h` `struct-bases` entries
  and every SpudFiles `mutable-globals` entry.

## The design, as agreed and built (2026-10-22)

Written against the callers of the day (re-grepped 2026-10-22): one that
reads and writes a document's archive at offsets and replaces it by
rename, one that reads a SPIR-V file whole, and one demo; two folders
outside `projects.conf` call the old API too and are left as they are.
This is `include/spudfiles.h` as built, in short:

```c
/* ---- Instance: the caller's allocator, as every other module. ---- */
typedef struct spudfile_instance_t *spudfile_instance;
typedef struct spudfile_instance_desc {
	SPUD_STRUCT_BASE base;            /* SPUD_STRUCT_TYPE_FILES_INSTANCE_DESC */
	const SPUD_ALLOCATOR *allocator;  /* required; copied */
} spudfile_instance_desc;
SPUDRESULT spudfile_instance_create(const spudfile_instance_desc *desc, spudfile_instance *out_instance);
void spudfile_instance_destroy(spudfile_instance instance);   /* every file and mapping first */

/* ---- Opening. Every path is UTF-8; Windows converts to UTF-16 and calls
 * only W functions. ---- */
typedef enum SPUDFILE_ACCESS SPUD_ENUM_BASE {
	SPUDFILE_ACCESS_NONE = 0,               /* refused: a zeroed desc hasn't chosen */
	SPUDFILE_ACCESS_READ = 1, SPUDFILE_ACCESS_WRITE = 2, SPUDFILE_ACCESS_READ_WRITE = 3,
} SPUDFILE_ACCESS;
typedef enum SPUDFILE_CREATE SPUD_ENUM_BASE {   /* CreateFileW's dispositions / O_CREAT, O_EXCL, O_TRUNC */
	SPUDFILE_CREATE_NONE = 0,               /* refused */
	SPUDFILE_CREATE_OPEN_EXISTING = 1,      /* SPUDRESULT_SPUDFILE_NOT_FOUND if there's none */
	SPUDFILE_CREATE_NEW = 2,                /* SPUDRESULT_SPUDFILE_ALREADY_EXISTS if there's one */
	SPUDFILE_CREATE_OPEN_OR_CREATE = 3,
	SPUDFILE_CREATE_TRUNCATE = 4,           /* create, or empty the one there */
} SPUDFILE_CREATE;
typedef uint32_t SPUDFILE_SHARE;            /* what others may do while it's open */
enum { SPUDFILE_SHARE_NONE = 0, SPUDFILE_SHARE_READ = 1, SPUDFILE_SHARE_WRITE = 2, SPUDFILE_SHARE_DELETE = 4 };
/* Windows enforces it (FILE_SHARE_*); POSIX has no share modes and enforces
 * nothing. The rule on every backend: a caller may be refused what another
 * holder's share mode forbids, and must not rely on being refused. */

typedef struct spudfile_open_desc {
	SPUD_STRUCT_BASE base;             /* SPUD_STRUCT_TYPE_FILES_OPEN_DESC (5000) */
	const char *path;
	SPUDFILE_ACCESS access;
	SPUDFILE_CREATE create;
	SPUDFILE_SHARE share;
	const char *debug_name;            /* copied, or NULL */
} spudfile_open_desc;
SPUDRESULT spudfile_file_open(spudfile_instance instance, const spudfile_open_desc *desc, spudfile_file *out_file);
void spudfile_file_close(spudfile_file file);    /* NULL does nothing */

/* ---- I/O at an offset: pread/pwrite, ReadFile/WriteFile with an
 * OVERLAPPED offset. No cursor, so two threads can read one file. ---- */
SPUDRESULT spudfile_file_read_at(spudfile_file file, uint64_t offset, void *buffer, uint64_t size,
                            uint64_t *out_read);   /* fewer than asked only at the end */
SPUDRESULT spudfile_file_write_at(spudfile_file file, uint64_t offset, const void *data, uint64_t size);
SPUDRESULT spudfile_file_get_size(spudfile_file file, uint64_t *out_size);
SPUDRESULT spudfile_file_set_size(spudfile_file file, uint64_t size);   /* ftruncate / SetFileInformationByHandle */
SPUDRESULT spudfile_file_flush(spudfile_file file, SPUDFILE_FLUSH_LEVEL level);   /* as today */

/* ---- Mapping: as today, released with a void call. ---- */
SPUDRESULT spudfile_file_map(spudfile_file file, uint64_t offset, uint64_t size, spudfile_mapping *out_mapping);
const void *spudfile_mapping_get_data(spudfile_mapping mapping);
uint64_t spudfile_mapping_get_size(spudfile_mapping mapping);
void spudfile_mapping_release(spudfile_mapping mapping);

/* ---- Paths: one call each, translated. ---- */
typedef enum SPUDFILE_PATH_KIND SPUD_ENUM_BASE {
	SPUDFILE_PATH_KIND_FILE = 1, SPUDFILE_PATH_KIND_DIRECTORY = 2, SPUDFILE_PATH_KIND_OTHER = 3,
} SPUDFILE_PATH_KIND;
typedef struct SPUDFILE_PATH_INFO {
	SPUD_STRUCT_BASE_OUT base;         /* SPUD_STRUCT_TYPE_FILES_PATH_INFO */
	SPUDFILE_PATH_KIND kind;
	uint64_t size;                     /* bytes; 0 for a directory */
	int64_t modified_ns;               /* since the Unix epoch */
} SPUDFILE_PATH_INFO;
SPUDRESULT spudfile_path_get_info(spudfile_instance instance, const char *path, SPUDFILE_PATH_INFO *out_info);
                                       /* SPUDRESULT_SPUDFILE_NOT_FOUND: nothing there */
SPUDRESULT spudfile_path_replace(spudfile_instance instance, const char *source, const char *target,
                            SPUDFILE_FLUSH_LEVEL level);   /* as today's spudfile_file_replace */
SPUDRESULT spudfile_path_remove(spudfile_instance instance, const char *path);   /* a file; as today */
SPUDRESULT spudfile_directory_create(spudfile_instance instance, const char *path);
                                       /* one level: SPUDRESULT_SPUDFILE_NOT_FOUND without its parent,
                                        * SPUDRESULT_SPUDFILE_ALREADY_EXISTS if it's there */
```

New results, in SpudFiles' block: `SPUDRESULT_SPUDFILE_INVALID_INSTANCE`,
`SPUDRESULT_SPUDFILE_NOT_FOUND`, `SPUDRESULT_SPUDFILE_ALREADY_EXISTS`,
`SPUDRESULT_SPUDFILE_ACCESS_DENIED`, `SPUDRESULT_SPUDFILE_NOT_A_FILE`. Today's
`SPUDRESULT_GENERAL_FAILURE` for a failed `open`/`read`/`write` becomes the
specific one, or `SPUDRESULT_API_SPECIFIC_FAILURE`, with the platform's
number kept for a `_get_error` as SpudNet does.

Gone: the dialogs and their attributes and flags, `SPUDFILE_FIND_FILE_DATA`, the
path-string functions, the working directory calls, the six access modes
(`SPUDFILE_EFILE_ACCESS_MODE_*`; append was used by no caller), the cursor
(`_get_pos`/`_set_pos`) and `spudfile_file_exists`/`spudfile_directory_exists` (both
`spudfile_path_get_info`).

Every caller in the workspace moved in the same change: each makes a
SpudFiles instance with the system heap for the length of its call, reads
at an offset where it seeked and read, and a save's temporary file is
created with `SPUDFILE_CREATE_NEW` (the old OVERWRITE could clobber a stray
file of the same name). One caller's loader closed no file it opened; it
does now.

The four decisions, as answered on 2026-10-22: offsets only, no cursor;
one directory level per `spudfile_directory_create`; the working directory
calls deleted; the share mode a required field of the open desc.

Beyond the proposal: `SPUDRESULT_SPUDFILE_NOT_WRITABLE` (1013), the twin of
`_NOT_READABLE`, for a write, a size or a flush on a file opened only to
read; `SPUDRESULT_SPUDFILE_INVALID_MAPPING` (1003) is retired, since releasing a
mapping can't fail. Files and directories get the platform's default
permissions with the umask (0666 and 0777 before it), where the old code
chose 0644 and 0755.

Run on macOS (2026-10-22): `tests/spudfiles_test` (every call and check, a
non-ASCII name, two threads reading one file, a counting allocator) and
spudlib's other seven ctests, plain and under ASan and TSan; the callers'
tests over it. Run on Windows 11 (2026-10-22): built with MSVC under the
`windows-d3d12` preset (Debug) with no warnings, `spudfiles_test` passing
every check (the non-ASCII name and the two threads included) and
spudlib's other seven ctests with it. The POSIX backend's Linux build
hasn't been compiled.

Left:

- The platform's own error number for a failure (as SpudNet's `_get_error`
  keeps it): not built. Every failure is one of the results, and the rest
  `SPUDRESULT_API_SPECIFIC_FAILURE`.
- Directory listing and the platform's known folders: only when a caller
  needs them.

## Listing a directory, and an advisory lock (card S39, decided 2026-11-06 with you, a)

Two plain translations, each with a caller now (a cook cache that lists
its folder and locks it against a second process; a tool that walks a
project folder):

- **A listing**: `spudfile_listing_open` / `_next` / `_close`, an
  iterator over `readdir` / `FindFirstFileExW`, one entry a call (its kind
  and its name, held in the entry), in the order the platform gives, "."
  and ".." never. No sorting, no recursion, no filtering: the caller's.
  (The card said one call into a buffer; the draft's Q1 says why not.)
- **An advisory lock on an open file**: `spudfile_file_lock` /
  `spudfile_file_unlock`, `flock` / `LockFileEx` on one byte past any data,
  shared or exclusive, wait or fail at once, released by its own call or
  the file's close; the file records the lock it holds.
- **Windows paths read as WTF-8**, so a listed name holding an unpaired
  surrogate opens.

**Draft approved 2026-11-06 by you** (`spudfiles_list_lock_draft.h`, its
second version, Q1-Q4 all a); it is the contract, and stays as the record.
**Built 2026-11-06 overnight on Windows 11** (ThinkPad P1 Gen 5, MSVC
14.51; the first step of that night's prompt): the six
codes (1014-1019) and two struct types (5003, 5004) in `spudcore.h`, the
five calls in `spudfiles.h`, every check in the front end
(`src/spudfiles.c`: arguments in order, the end said again without asking
the platform, the lock a file holds recorded there, so locking twice and
unlocking none answer the same on every backend, and a close letting go
of a held lock first), the Windows backend (`FindFirstFileExW` with
`FindExInfoBasic` and a large fetch, a reparse point by its tag,
`LockFileEx` on the byte at 2^64 - 2, and SpudFiles' own WTF-8 converter
for every path in place of `MultiByteToWideChar`, a lone surrogate's
three bytes taken, a pair of them refused), and the POSIX backend
(`opendir`/`readdir`, `fstatat` with `AT_SYMLINK_NOFOLLOW` for
`DT_UNKNOWN`, `flock`; written, not compiled: the Mac and Linux compile
and run it next). `spudfiles_test` gained the draft's tests, seen failing
first against stubs (48 failures), then passing: every argument refused in
order, an empty folder, files, a folder, a hidden file, a non-ASCII name
round-tripped, a junction listed as a link (symbolic links aren't made
here without developer mode: reported, not listed), a name with an
unpaired surrogate listed as WTF-8 and opened by it, the converter's
refusals, and the locks between two files on one path (exclusive against
both kinds, shared with shared, advisory reads and writes, unlock, close,
a waiting lock on a second thread had only after the holder lets go), on
NTFS; `SPUDFILES_TEST_LOCK_DIRS` runs the locks on more file systems
(none other on this machine). `windows-d3d12` and `-vulkan`, Debug and
Release, every ctest (15) passing; `abi.txt` additions only; the misuse
harness regenerated with a listing factory. Its first caller locks its
cache folder with it on every platform and lists the folder for orphaned
entries.

## Removing and renaming a directory (card S41, raised 2026-11-06)

Found overnight, a caller writing a folder whole and swapping it into
place: no call removes a directory or
renames one (`spudfile_path_remove` is a file's; `spudfile_path_replace`
is a file's rename). Drafted for approval, not built:
`spudfiles_directory_draft.h` (`spudfile_directory_remove`,
`spudfile_directory_rename`, `SPUDRESULT_SPUDFILE_NOT_EMPTY`). Beside it,
card S42: a copied file can't keep its executable bit on POSIX.

## Breaks

Every caller of `spudfile_file_open` and `spudfile_file_exists` (the grep above finds
them).
Change them in the same change, and build and run each one's tests. Windows
and Linux backends change too: build them (`windows-d3d12`, `linux-vulkan`)
before calling it done.

## Done when

The steps every item of `MATURITY_TODO.md` ends with:

- build on macOS and run the tests, plain and under the `-asan` preset;
- build every caller the change breaks, in the same change, and run its tests;
- `./check.sh` from the workspace, with the entries above taken out of
  `check.baseline`;
- after the spudlib commit is pushed, `./check.sh --update-locks`, and commit
  the locks with the repositories that use them;
- each changed repository's `.git/COMMIT_DRAFT` rewritten to describe its
  change.
