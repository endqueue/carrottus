# Carrottus Files TODO

Written on 2026-10-20, from item 4 of `MATURITY_TODO.md`, which now points
here. **Done on 2026-10-22:** the proposal below was agreed, with the four
decisions as recommended, and built. What follows "The proposal to start
from" is the record of what was built and what is left. **Renamed on
2026-10-27** (card X1b): the prefix `sfs_` below is now `crfiles_`
(`CRFILES_`, `CR_RESULT_CRFILES_*`); the record says the new names.

Carrottus Files was written before carrottus's current rules, and breaks several of
them. Converting its structs to struct bases as they are would polish an API
that has to change anyway, so the struct-base step is part of this rework, not
a step of its own.

Begin the session with a proposal for the new `crfiles.h`, and write code
only once it is agreed.

**Before it:** `MATURITY_TODO.md`'s item 3 is settled for what this needs:
the new instance desc takes a `CR_ALLOCATOR`, with no default, and every
result has a name from `cr_result_str`. The new `crfiles.h` is written to
those rules.

## What is wrong now

- **Writable static data.** The path functions (`crfiles_get_path_components`,
  `crfiles_simplify_path`, `crfiles_split_path`, `crfiles_get_relative_path`,
  `crfiles_file_dialog_multi_path`) and `crfiles_get_working_directory`, the dialogs
  and `run_dialog_helper` return pointers into `static` buffers: not
  reentrant, not thread-safe, and each call overwrites the last answer. These
  are fifteen of carrottus's `mutable-globals` baseline entries.
  `crfiles_set_working_directory` changes the whole process's state.
- **A default chosen for the caller.** `crfiles_default_file_dialog_attributes`
  picks dialog flags on the caller's behalf.
- **GUI.** carrottus owns no window, and GUI belongs to the caller, but
  Carrottus Files shows modal dialogs (`crfiles_file_dialog`, `crfiles_open_folder_dialog`);
  the macOS one runs `osascript` and ignores `CRFILES_FILE_DIALOG_FLAG_OVERWRITE_PROMPT`.
- **No platform API translated.** The path-string functions (`crfiles_is_slash`,
  `crfiles_correct_slashes`, `crfiles_is_absolute`, `crfiles_split_path`,
  `crfiles_build_path_from_components`, `crfiles_simplify_path`,
  `crfiles_get_relative_path`) are string handling that decides what a path
  means, not a translation of any platform call.
- **Old shapes.** `crfiles_file_open` and `crfiles_file_dialog` take their structs by
  value; results come back as `bool` (`crfiles_file_exists` can't tell "no file"
  from a failure) or `const char *` instead of `CR_RESULT`. The header's
  `// FileOpenAttribs`-style comments name an older API. `CRFILES_FIND_FILE_DATA`
  is used by no function. `include/crfiles.h:181` ends in a stray `\`
  that splices it onto the next declaration, and
  `crfiles_get_working_directory()` has empty parentheses rather than `(void)`.
  `crfiles_file_read` returns `CR_SUCCESS` at the end of the file with fewer
  bytes read than asked and no count, so a short read can't be seen.
- **Windows paths in the code page.** `crfiles_file_open` calls `CreateFileA`
  (`src/files/backends/windows/crfileswindows.c:70`), so a path outside the
  system code page fails there, while `crfiles_file_replace` already converts to
  UTF-16 for `MoveFileExW`.
- **A share mode chosen for the caller.** The same call passes
  `FILE_SHARE_READ` always.

## What the callers use

As of 2026-10-20, read from the workspace with
`grep -rnoE "\bsfs_[a-z_]+\("` over its other repositories (three files):
`crfiles_file_open`, `_read`, `_write`, `_set_pos`, `_get_size`, `_flush`,
`_replace`, `_remove`, `_map`, `_release` and `crfiles_file_exists`.
Nothing calls the path functions, the working directory calls,
`crfiles_create_directory`, `crfiles_directory_exists` or the dialogs. Run the grep
again before deciding what to delete.

## The proposal to start from

- Delete the dialogs, `crfiles_default_file_dialog_attributes`,
  `CRFILES_FILE_DIALOG_ATTRIBUTES` and its flags, and `CRFILES_FIND_FILE_DATA`.
- Delete the path-string functions. A caller that needs them owns them.
- Delete `crfiles_set_working_directory` and `crfiles_get_working_directory`, or keep
  only a plain translation that writes into a caller's buffer.
- Every remaining call returns `CR_RESULT`. A call that reports a string or
  a yes/no answer writes it into the caller's buffer or output parameter
  (`out_buf`, `capacity`, `out_size`, as Carrottus Net's getters do). A buffer too
  small gets a result of its own and the size it needs, as Vulkan's
  two-call idiom does with `VK_INCOMPLETE`.
- Paths are UTF-8, said once in the header; the Windows backend converts
  every one to UTF-16 and calls only the `W` functions.
- `crfiles_file_exists` becomes a stat-like call (`stat`,
  `GetFileAttributesExW`): `CR_RESULT_NOT_FOUND`-style for no file, else
  the kind, size and modification time.
- Consider reads and writes at an offset (`pread`/`pwrite`, `ReadFile` with
  an offset), which the platforms have natively and which share no cursor
  between threads, beside or instead of `_set_pos`/`_get_pos`.
- Candidates, only if a caller needs them: listing a directory (an iterator
  over `opendir`/`FindFirstFileW`) and the platform's known folders
  (`SHGetKnownFolderPath`, `NSSearchPathForDirectories`); both translate a
  platform call, unlike the path-string functions.
- `crfiles_file_open` takes a `const CRFILES_FILE_OPEN_ATTRIBUTES *` that starts with
  a `CR_STRUCT_BASE` (`CR_STRUCT_TYPE_FILES_OPEN_DESC`, 5000; perhaps
  renamed to a `_desc`, as the other modules' are), refused with
  `CR_RESULT_NULL_DESC` when NULL. It is the one struct likely to grow
  (exclusive create, share modes, access hints). Its share mode is the
  caller's, with no default.
- Its checks are written once, in a validate function in `src/crfiles.c`
  that each backend calls first, as `craudio_validate_stream_desc` is for
  Carrottus Audio, and not three times in the backends. (Carrottus Files has one backend
  per platform, so it needs none of Carrottus Input's dispatch.)
- A test, `tests/crfiles_test.c`, registered with ctest: the argument
  checks and refusals, and open, write, read, map, flush, replace and remove
  in a temporary directory. Written 2026-10-22 against today's API (passing
  on macOS); the rework moves it to the new calls.
- `CLAUDE.md`: the Carrottus Files macOS-backend note under "Known gaps" (the
  `osascript` dialogs) removed with the dialogs.
- `STRUCT_BASE_TODO.md`: its "crfiles.h" section updated to what was done;
  then its step 6 (that file's classification rewritten as a record, and
  `CLAUDE.md`'s note shortened).
- `check.baseline`: remove the three `crfiles.h` `struct-bases` entries
  and every Carrottus Files `mutable-globals` entry.

## The design, as agreed and built (2026-10-22)

Written against the callers of the day (re-grepped 2026-10-22): one that
reads and writes a document's archive at offsets and replaces it by
rename, one that reads a SPIR-V file whole, and one demo; two folders
outside `projects.conf` call the old API too and are left as they are.
This is `include/crfiles.h` as built, in short:

```c
/* ---- Instance: the caller's allocator, as every other module. ---- */
typedef struct crfiles_instance_t *crfiles_instance;
typedef struct crfiles_instance_desc {
	CR_STRUCT_BASE base;            /* CR_STRUCT_TYPE_FILES_INSTANCE_DESC */
	const CR_ALLOCATOR *allocator;  /* required; copied */
} crfiles_instance_desc;
CR_RESULT crfiles_instance_create(const crfiles_instance_desc *desc, crfiles_instance *out_instance);
void crfiles_instance_destroy(crfiles_instance instance);   /* every file and mapping first */

/* ---- Opening. Every path is UTF-8; Windows converts to UTF-16 and calls
 * only W functions. ---- */
typedef enum CRFILES_ACCESS CR_ENUM_BASE {
	CRFILES_ACCESS_NONE = 0,               /* refused: a zeroed desc hasn't chosen */
	CRFILES_ACCESS_READ = 1, CRFILES_ACCESS_WRITE = 2, CRFILES_ACCESS_READ_WRITE = 3,
} CRFILES_ACCESS;
typedef enum CRFILES_CREATE CR_ENUM_BASE {   /* CreateFileW's dispositions / O_CREAT, O_EXCL, O_TRUNC */
	CRFILES_CREATE_NONE = 0,               /* refused */
	CRFILES_CREATE_OPEN_EXISTING = 1,      /* CR_RESULT_FILES_NOT_FOUND if there's none */
	CRFILES_CREATE_NEW = 2,                /* CR_RESULT_FILES_ALREADY_EXISTS if there's one */
	CRFILES_CREATE_OPEN_OR_CREATE = 3,
	CRFILES_CREATE_TRUNCATE = 4,           /* create, or empty the one there */
} CRFILES_CREATE;
typedef uint32_t CRFILES_SHARE;            /* what others may do while it's open */
enum { CRFILES_SHARE_NONE = 0, CRFILES_SHARE_READ = 1, CRFILES_SHARE_WRITE = 2, CRFILES_SHARE_DELETE = 4 };
/* Windows enforces it (FILE_SHARE_*); POSIX has no share modes and enforces
 * nothing. The rule on every backend: a caller may be refused what another
 * holder's share mode forbids, and must not rely on being refused. */

typedef struct crfiles_open_desc {
	CR_STRUCT_BASE base;             /* CR_STRUCT_TYPE_FILES_OPEN_DESC (5000) */
	const char *path;
	CRFILES_ACCESS access;
	CRFILES_CREATE create;
	CRFILES_SHARE share;
	const char *debug_name;            /* copied, or NULL */
} crfiles_open_desc;
CR_RESULT crfiles_file_open(crfiles_instance instance, const crfiles_open_desc *desc, crfiles_file *out_file);
void crfiles_file_close(crfiles_file file);    /* NULL does nothing */

/* ---- I/O at an offset: pread/pwrite, ReadFile/WriteFile with an
 * OVERLAPPED offset. No cursor, so two threads can read one file. ---- */
CR_RESULT crfiles_file_read_at(crfiles_file file, uint64_t offset, void *buffer, uint64_t size,
                            uint64_t *out_read);   /* fewer than asked only at the end */
CR_RESULT crfiles_file_write_at(crfiles_file file, uint64_t offset, const void *data, uint64_t size);
CR_RESULT crfiles_file_get_size(crfiles_file file, uint64_t *out_size);
CR_RESULT crfiles_file_set_size(crfiles_file file, uint64_t size);   /* ftruncate / SetFileInformationByHandle */
CR_RESULT crfiles_file_flush(crfiles_file file, CRFILES_FLUSH_LEVEL level);   /* as today */

/* ---- Mapping: as today, released with a void call. ---- */
CR_RESULT crfiles_file_map(crfiles_file file, uint64_t offset, uint64_t size, crfiles_mapping *out_mapping);
const void *crfiles_mapping_get_data(crfiles_mapping mapping);
uint64_t crfiles_mapping_get_size(crfiles_mapping mapping);
void crfiles_mapping_release(crfiles_mapping mapping);

/* ---- Paths: one call each, translated. ---- */
typedef enum CRFILES_PATH_KIND CR_ENUM_BASE {
	CRFILES_PATH_KIND_FILE = 1, CRFILES_PATH_KIND_DIRECTORY = 2, CRFILES_PATH_KIND_OTHER = 3,
} CRFILES_PATH_KIND;
typedef struct CRFILES_PATH_INFO {
	CR_STRUCT_BASE_OUT base;         /* CR_STRUCT_TYPE_FILES_PATH_INFO */
	CRFILES_PATH_KIND kind;
	uint64_t size;                     /* bytes; 0 for a directory */
	int64_t modified_ns;               /* since the Unix epoch */
} CRFILES_PATH_INFO;
CR_RESULT crfiles_path_get_info(crfiles_instance instance, const char *path, CRFILES_PATH_INFO *out_info);
                                       /* CR_RESULT_FILES_NOT_FOUND: nothing there */
CR_RESULT crfiles_path_replace(crfiles_instance instance, const char *source, const char *target,
                            CRFILES_FLUSH_LEVEL level);   /* as today's crfiles_file_replace */
CR_RESULT crfiles_path_remove(crfiles_instance instance, const char *path);   /* a file; as today */
CR_RESULT crfiles_directory_create(crfiles_instance instance, const char *path);
                                       /* one level: CR_RESULT_FILES_NOT_FOUND without its parent,
                                        * CR_RESULT_FILES_ALREADY_EXISTS if it's there */
```

New results, in Carrottus Files' block: `CR_RESULT_FILES_INVALID_INSTANCE`,
`CR_RESULT_FILES_NOT_FOUND`, `CR_RESULT_FILES_ALREADY_EXISTS`,
`CR_RESULT_FILES_ACCESS_DENIED`, `CR_RESULT_FILES_NOT_A_FILE`. Today's
`CR_RESULT_GENERAL_FAILURE` for a failed `open`/`read`/`write` becomes the
specific one, or `CR_RESULT_API_SPECIFIC_FAILURE`, with the platform's
number kept for a `_get_error` as Carrottus Net does.

Gone: the dialogs and their attributes and flags, `CRFILES_FIND_FILE_DATA`, the
path-string functions, the working directory calls, the six access modes
(`CRFILES_EFILE_ACCESS_MODE_*`; append was used by no caller), the cursor
(`_get_pos`/`_set_pos`) and `crfiles_file_exists`/`crfiles_directory_exists` (both
`crfiles_path_get_info`).

Every caller in the workspace moved in the same change: each makes a
Carrottus Files instance with the system heap for the length of its call, reads
at an offset where it seeked and read, and a save's temporary file is
created with `CRFILES_CREATE_NEW` (the old OVERWRITE could clobber a stray
file of the same name). One caller's loader closed no file it opened; it
does now.

The four decisions, as answered on 2026-10-22: offsets only, no cursor;
one directory level per `crfiles_directory_create`; the working directory
calls deleted; the share mode a required field of the open desc.

Beyond the proposal: `CR_RESULT_FILES_NOT_WRITABLE` (1013), the twin of
`_NOT_READABLE`, for a write, a size or a flush on a file opened only to
read; `CR_RESULT_FILES_INVALID_MAPPING` (1003) is retired, since releasing a
mapping can't fail. Files and directories get the platform's default
permissions with the umask (0666 and 0777 before it), where the old code
chose 0644 and 0755.

Run on macOS (2026-10-22): `tests/crfiles_test` (every call and check, a
non-ASCII name, two threads reading one file, a counting allocator) and
carrottus's other seven ctests, plain and under ASan and TSan; the callers'
tests over it. Run on Windows 11 (2026-10-22): built with MSVC under the
`windows-d3d12` preset (Debug) with no warnings, `crfiles_test` passing
every check (the non-ASCII name and the two threads included) and
carrottus's other seven ctests with it. The POSIX backend's Linux build
hasn't been compiled.

Left:

- The platform's own error number for a failure: **built 2026-11-09** as
  card S10's message callback, Carrottus Files the first module to take it (see
  "Opening a dynamic library" below). Every failure is still one of the
  results, and the platform's number goes to the instance's callback.
- Directory listing and the platform's known folders: only when a caller
  needs them.

## Listing a directory, and an advisory lock (card S39, decided 2026-11-06 with you, a)

Two plain translations, each with a caller now (a cook cache that lists
its folder and locks it against a second process; a tool that walks a
project folder):

- **A listing**: `crfiles_listing_open` / `_next` / `_close`, an
  iterator over `readdir` / `FindFirstFileExW`, one entry a call (its kind
  and its name, held in the entry), in the order the platform gives, "."
  and ".." never. No sorting, no recursion, no filtering: the caller's.
  (The card said one call into a buffer; the draft's Q1 says why not.)
- **An advisory lock on an open file**: `crfiles_file_lock` /
  `crfiles_file_unlock`, `flock` / `LockFileEx` on one byte past any data,
  shared or exclusive, wait or fail at once, released by its own call or
  the file's close; the file records the lock it holds.
- **Windows paths read as WTF-8**, so a listed name holding an unpaired
  surrogate opens.

**Draft approved 2026-11-06 by you** (`crfiles_list_lock_draft.h`, its
second version, Q1-Q4 all a); it is the contract, and stays as the record.
**Built 2026-11-06 overnight on Windows 11** (ThinkPad P1 Gen 5, MSVC
14.51; the first step of that night's prompt): the six
codes (1014-1019) and two struct types (5003, 5004) in `crcore.h`, the
five calls in `crfiles.h`, every check in the front end
(`src/crfiles.c`: arguments in order, the end said again without asking
the platform, the lock a file holds recorded there, so locking twice and
unlocking none answer the same on every backend, and a close letting go
of a held lock first), the Windows backend (`FindFirstFileExW` with
`FindExInfoBasic` and a large fetch, a reparse point by its tag,
`LockFileEx` on the byte at 2^64 - 2, and Carrottus Files' own WTF-8 converter
for every path in place of `MultiByteToWideChar`, a lone surrogate's
three bytes taken, a pair of them refused), and the POSIX backend
(`opendir`/`readdir`, `fstatat` with `AT_SYMLINK_NOFOLLOW` for
`DT_UNKNOWN`, `flock`; written, not compiled: the Mac and Linux compile
and run it next). `crfiles_test` gained the draft's tests, seen failing
first against stubs (48 failures), then passing: every argument refused in
order, an empty folder, files, a folder, a hidden file, a non-ASCII name
round-tripped, a junction listed as a link (symbolic links aren't made
here without developer mode: reported, not listed), a name with an
unpaired surrogate listed as WTF-8 and opened by it, the converter's
refusals, and the locks between two files on one path (exclusive against
both kinds, shared with shared, advisory reads and writes, unlock, close,
a waiting lock on a second thread had only after the holder lets go), on
NTFS; `CRFILES_TEST_LOCK_DIRS` runs the locks on more file systems
(none other on this machine). `windows-d3d12` and `-vulkan`, Debug and
Release, every ctest (15) passing; `abi.txt` additions only; the misuse
harness regenerated with a listing factory. Its first caller locks its
cache folder with it on every platform and lists the folder for orphaned
entries.

## Removing and renaming a directory (card S41, decided 2026-11-07 with you, the draft as written)

Found 2026-11-06 overnight, a caller writing a folder whole and swapping it
into place: no call removes a directory or renames one
(`crfiles_path_remove` is a file's; `crfiles_path_replace` is a file's
rename).

**Draft approved 2026-11-07 by you** (`crfiles_directory_draft.h`, its
first version, Q1 a and Q2 a); it is the contract, and stays as the record:
- `crfiles_directory_remove`: an empty directory, `rmdir` /
  `RemoveDirectoryW`; a link or junction to a directory removed itself,
  never what it points at.
- `crfiles_directory_rename`: a call of its own (Q1 a), to a name where
  nothing is, on every backend (`rename` / `MoveFileExW`, the flush level
  as `crfiles_path_replace` takes it); `path_replace` stays a file's.
- `CR_RESULT_FILES_NOT_EMPTY` (1020) for a directory that holds
  entries (Q2 a), so a caller removing a tree can branch on it.

**Built 2026-11-07 on Windows 11** (ThinkPad P1 Gen 5, MSVC 14.51):
`CR_RESULT_FILES_NOT_EMPTY` in `crcore.h` and `cr_result_str`, the
two calls in `crfiles.h`, their checks in the front end
(`src/crfiles.c`: arguments in order, the flush level as
`crfiles_path_replace` checks it), the Windows backend
(`GetFileAttributesW` for what is there, links not followed, then
`RemoveDirectoryW`, `ERROR_DIR_NOT_EMPTY` as NOT_EMPTY; `MoveFileExW` with
no replace and no copy, `MOVEFILE_WRITE_THROUGH` for DEVICE and MEDIA,
access denied told apart by what is at the target), and the POSIX backend
(`lstat`, a link to a directory `unlink`ed, `rmdir`, ENOTEMPTY or EEXIST as
NOT_EMPTY; the rename `renameat2` with `RENAME_NOREPLACE` on Linux and
`renamex_np` with `RENAME_EXCL` on Apple, so nothing at the target holds
with no window, a checked `rename` only on a file system without them;
written, not compiled). `crfiles_test` gained the draft's tests, seen
failing first against stubs (55 failures), then passing: each argument
refused in order; an empty directory removed, one holding a file or a
directory NOT_EMPTY and kept, a file NOT_A_DIRECTORY, nothing NOT_FOUND; a
junction removed itself, its directory and file kept (symbolic links not
made here, as for S39); a directory held open without sharing delete
IN_USE; a directory with a file renamed to a free name and back at each
level; a file, an empty directory and a full one at the target each
ALREADY_EXISTS, both kept; a file source NOT_A_DIRECTORY; no parent for
the target NOT_FOUND; a file inside held open IN_USE, nothing moved; and a
tree written as `.tree.partial`, renamed into place and removed through a
listing and the two calls, nothing left. Across volumes runs only with
`CRFILES_TEST_OTHER_VOLUME_DIR` (one volume here: not run).
`windows-d3d12` and `-vulkan`, Debug and Release, and both `-asan`
presets: every ctest (15) passing; `cr_misuse_tests` 10 of 10 on each
backend in Debug (249 calls with a destroyed handle); `abi.txt` additions
only. Its first caller writes its export as a partial folder and swaps it
in on these calls.

## A copied file's mode (card S42, decided 2026-11-07 with you, b)

Found 2026-11-06 overnight, read from the code: a file Carrottus Files creates
takes the platform's default mode (0666 less the umask), and no call sets
one, so a program copied through Carrottus Files on Linux or macOS isn't
executable. **Decided: b.** A call that copies a file's mode bits from
another, `crfiles_path_copy_mode(instance, source, target)`. It carries
what the source already is and decides nothing. Rejected: a "make it a program" choice in the open desc (a, a
decision the caller would make per file, refused on one platform), and
leaving it to the developer's `chmod +x` (c).

**Draft approved 2026-11-07 by you** (`crfiles_mode_draft.h`, its first
version, Q1 a and Q2 a); it is the contract, and stays as the record:
- POSIX: the nine permission bits (`stat`, then `chmod`; `st_mode & 0777`),
  never the set-user-ID, set-group-ID and sticky bits, whose meaning
  depends on an owner a copy doesn't carry (Q1 a).
- Windows: the read-only attribute (`GetFileAttributesW`, then
  `SetFileAttributesW`, the target's other attributes kept), so a source
  that can't be written gives a target that can't be written on every
  backend (Q2 a; the card had said Windows does nothing, before the draft
  found that Windows keeps this one permission).
- Links followed for both paths, the umask no part of it, any kind of
  entry, nothing flushed; no owner, ACL or extended attribute copied.

**Built 2026-11-07 on Windows 11** (the same machine and day as S41):
`crfiles_path_copy_mode` in `crfiles.h`, its file comment's rule on
new files' permissions pointing to it; its checks in the front end; the
Windows backend (each path opened for its attributes alone, a link
followed, `FILE_BASIC_INFO` read from both and written to the target with
only `FILE_ATTRIBUTE_READONLY` taken from the source, its times left as
they are, `ERROR_WRITE_PROTECT` the platform's failure); the POSIX backend
(`stat`, then `chmod` with `st_mode & 0777`, `EROFS` the platform's
failure; written, not compiled). `crfiles_test` gained the draft's
tests, seen failing first against a stub (15 failures), then passing on
Windows: each argument refused in order, nothing at either path NOT_FOUND
with the target unchanged, the read-only attribute carried both ways with
the target's hidden attribute kept, a directory as either path, a
junction as the target changing its directory and not itself. The POSIX
tests (written, not compiled): each of four modes carried under a umask
of 077, the set-ID bits never carried, a directory either way, symbolic
links followed both ways with the links unchanged, and `/` onto itself
refused when not root. Its first caller gives each Linux and macOS
export's executable its template executable's permissions. Its caller's Linux and macOS exports, and the
ctest that runs an exported game there, wait on it.

## Opening a dynamic library (card S44, decided 2026-11-09 with you, the draft as written)

Raised 2026-11-09: a caller loads a rebuilt shared library at run time and
may reach the platform only through Carrottus, which had no loader.
**Draft approved 2026-11-09 by you** (`crfiles_library_draft.h`, its
first version, Q1-Q6 a); it is the contract, and stays as the record:
- In Carrottus Files (Q1): `crfiles_library_open`, `_find` and `_close`, a
  handle made from the instance and quarantined by it in Debug (X27).
  `LoadLibraryExW`, `GetProcAddress` and `FreeLibrary`; `dlopen`, `dlsym`
  and `dlclose`.
- The loader's choices on the desc, none with a default (Q2): binding
  (`NOW`, `LAZY`), scope (`LOCAL`, `GLOBAL`) and dependency search
  (`PLATFORM_ORDER`, `LIBRARY_DIRECTORY_FIRST`, the last needing a fully
  qualified path), each refused with
  `CR_RESULT_FILES_LIBRARY_OPTION_NOT_SUPPORTED` where the platform
  has no such choice (`LAZY` and `GLOBAL` on Windows,
  `LIBRARY_DIRECTORY_FIRST` on POSIX).
- A bare name goes to the platform's own search (Q3).
- A lookup returns `crfiles_library_function`, `void (*)(void)`; no data
  symbols; a function the library itself exports, what is found in its
  dependencies undefined (Q4).
- On every platform, iOS included (Q5).
- One code for a refused load, `CR_RESULT_FILES_LIBRARY_NOT_LOADED`;
  the loader's words through card S10's message callback, which Carrottus Files
  takes first: `crfiles_instance_desc` gains `message_callback` and
  `message_user_data`, every Carrottus Files platform failure is sent, and S10's
  sources gain `DLERROR` (Q6). A listed break for every caller that fills
  the instance desc.

**Built 2026-11-09 on Windows 11** (the workspace's prompt for it,
parts 2 and 3; ThinkPad P1 Gen 5, MSVC 14.51), with card S10's half it
needs:
- `crcore.h`: S10's types as its draft writes them (`CR_PLATFORM_ERROR_SOURCE`
  moved from Carrottus Net, every value kept, `VKRESULT` 9 and `DLERROR` 10 added;
  `CR_MESSAGE_ORIGIN`, `CR_MESSAGE_SEVERITY`, `CR_MESSAGE` based as
  `CR_STRUCT_TYPE_MESSAGE` 1, `CR_MESSAGE_CALLBACK`, `CR_PLATFORM_ERROR`)
  and a "Messages" section with M1-M7. Carrottus Net's `crnet_error` takes the
  moved enum; its records stay.
- Carrottus Files' half of S10: `crfiles_instance_desc` gains `message_callback`
  and `message_user_data`; every platform failure, on both backends, is
  sent inside the failing call, naming the call and the object
  (`src/files/crfilesshared.h`'s `crfiles_report`, made per call by the
  front end). The other modules keep their descs: S10 builds them later.
- The loader: `crfiles_library_open`, `_find`, `_close`, results 1021-1024,
  `CR_STRUCT_TYPE_FILES_LIBRARY_DESC` 5005, quarantined by its instance in
  Debug, its factory in the misuse generator. Every check in the front end.
- Tests in `crfiles_test`; four test libraries from one source
  (`tests/crfiles_test_library.c`), built by the root `CMakeLists.txt`,
  where carrottus's tests are (there is no `tests/CMakeLists.txt`).
- Found: Windows' loader takes 250 characters where the file calls take
  259 (ERROR_FILENAME_EXCED_RANGE from 251, both search modes); nothing is
  added to the path, so the test loads 250 and sees 259 refused.
- `CARROTTUS_VERSION_MINOR` 2.
- Run: windows-d3d12 and windows-vulkan, Debug and Release, 15 ctests each passing, no warning; windows-d3d12-asan and windows-vulkan-asan (MSVC AddressSanitizer, Debug), 15 each passing; cr_misuse_tests ten times per backend in Debug, each run 280 calls with zeroes and 253 with a destroyed handle, 0 failures; cr_noalloc_tests passing in every configuration; crnet_test 343 of 343 (D3D12, Debug).

**Run on the Mac 2026-11-09** (Apple M5 Pro, macOS 27, Apple clang 21;
the workspace's prompt for the Mac, part 1): the POSIX half (dlopen,
dlsym, dlclose with dlerror's text, and errno's messages for every file
call) compiled at once with no warning, under `-Wall -Wextra -Wpedantic`
too, and needed no change. `macos-metal` Debug and Release, `-asan` and
`-tsan`: `crfiles_test` passing in all four; every other ctest passing
but `crgpu_objects_test`'s X27 quarantine case in the Debug three (the
Metal backend still frees a destroyed handle at once: X27's Metal half,
not built). `cr_misuse_tests` ten times in Debug (269 calls with zeroes,
234 with a destroyed handle, no failure); `cr_noalloc_tests` none of
ours. `iossimulator-metal` Debug and Release in an iPhone 17 Pro
simulator (iOS 26.5): `crfiles_test` and the other tests that need no
GPU or server passing. Added or changed in the tests:
- `LAZY`, `GLOBAL` and both together accepted on POSIX and the library
  called through each.
- Access denied by a directory made `0000` (open and path info, each
  `ACCESS_DENIED` with `EACCES`).
- APFS refuses a name that isn't UTF-8 (`EILSEQ`): S39's listing test of
  such a name keeps Linux's case and, on Apple, sees the create refused and
  nothing listed.
- Q5a: on iOS a framework (`crfiles_test_framework`, value 5) built into a
  `Frameworks` folder beside the test, which stands in for an app bundle,
  opened by the path of its binary. The simulator doesn't enforce code
  signing on dlopen, so this shows the framework's layout and the path, not
  a device's library validation; a device can't run a bare executable.
- Under ThreadSanitizer on Apple the two-thread case holds one more open of
  each library while the threads run: TSan's own `dlopen` lists the loaded
  images and crashed (about 1 run in 8) when another thread's `dlclose`
  unmapped one meanwhile. Reproduced with a program of two threads calling
  dlopen and dlclose alone, no Carrottus in it; never without TSan (40 of 40).
Found and fixed: card X27's quarantine push read the list's head with a
plain load beside another thread's compare-and-swap, a data race in C11
(TSan, two threads closing libraries); the read is atomic now on clang and
GCC, and MSVC's code is unchanged (its volatile read is atomic).

Left: Linux (the POSIX half on glibc, `-tsan` there, `libc.so.6` by name).

## Breaks

Every caller of `crfiles_file_open` and `crfiles_file_exists` (the grep above finds
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
- after the carrottus commit is pushed, `./check.sh --update-locks`, and commit
  the locks with the repositories that use them;
- each changed repository's `.git/COMMIT_DRAFT` rewritten to describe its
  change.
