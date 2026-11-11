/*
 * crfiles_directory_draft.h - Carrottus Files: removing an empty directory, and
 * renaming a directory (card S41, raised 2026-11-06 overnight, where a
 * caller needed both).
 *
 * DRAFT, 2026-11-06, first version. APPROVED 2026-11-07 (Q1 a, Q2 a, card
 * S41): the contract, kept as the record. Not built, not in any target. Not compilable: it repeats crcore.h's enum and crfiles.h's
 * declarations to show them changed. Every declaration is marked [ADDED]
 * or [CHANGED]. Written against include/crfiles.h as it stands after
 * card S39's listing and lock (built 2026-11-06 on Windows).
 *
 * ---------------------------------------------------------------------------
 * Why now
 * ---------------------------------------------------------------------------
 *
 * Carrottus Files makes a directory (crfiles_directory_create) and, since S39,
 * lists one, but can't remove one: crfiles_path_remove is unlink or
 * DeleteFileW and refuses a directory (NOT_A_FILE). And
 * crfiles_path_replace is a file's rename: its contract names "the file
 * that takes the target's name". A caller that writes a folder of files
 * whole and then swaps it into place (an export; any build output), or
 * cleans one up after a failure, has no way to finish. Both calls below
 * are plain translations: rmdir / RemoveDirectoryW of an empty directory,
 * and rename / MoveFileExW of a directory to a name nothing has. Removing
 * a tree is the caller's (a listing and these calls), as every platform
 * API leaves it.
 *
 * ---------------------------------------------------------------------------
 * Questions for approval
 * ---------------------------------------------------------------------------
 *
 * Q1. Rename a directory: a call of its own, or crfiles_path_replace
 *     widened?
 *     a. (Recommended, drafted) A call of its own,
 *        crfiles_directory_rename(instance, source, target, level): the
 *        target must not exist (ALREADY_EXISTS otherwise), on every
 *        backend. MoveFileExW can't rename a directory over anything
 *        (ERROR_ACCESS_DENIED for a directory target, a file target
 *        refused), and POSIX rename replaces only an empty directory, so
 *        "nothing there" is the rule all of them keep. path_replace stays
 *        a file's, its contract and checks unchanged, and the two
 *        meanings (atomic replace of a file; a move of a directory to a
 *        free name) never share one call.
 *     b. path_replace takes a directory source too, refusing an existing
 *        target only then: one call whose rule depends on what the source
 *        is.
 *
 * Q2. A directory that isn't empty.
 *     a. (Recommended, drafted) Refused with a new code,
 *        CR_RESULT_FILES_NOT_EMPTY (ENOTEMPTY or EEXIST;
 *        ERROR_DIR_NOT_EMPTY): a caller removing a tree branches on it
 *        (list, remove, try again).
 *     b. CR_RESULT_API_SPECIFIC_FAILURE: the caller can't tell it from a
 *        failure.
 *
 * Stated, not asked: removing a symbolic link to a directory (or a
 * junction) removes the link, never what it points at, on every backend
 * (rmdir of a link is ENOTDIR on POSIX: the link is unlink's, so the
 * POSIX backend unlinks an entry lstat calls a link; RemoveDirectoryW
 * removes the junction or directory link itself). A directory open by
 * another handle on Windows is CR_RESULT_FILES_IN_USE, as a file's
 * remove is.
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * Added: one result code (1020, the next free of Carrottus Files'), two calls.
 * No change to any struct or existing call.
 *
 * ---------------------------------------------------------------------------
 * Tests (crfiles_test, extended; every backend)
 * ---------------------------------------------------------------------------
 *
 * - remove: an empty directory gone; a non-empty one NOT_EMPTY and still
 *   there; a file NOT_A_DIRECTORY; nothing there NOT_FOUND; a link to a
 *   directory (and on Windows a junction) removed, its target still there
 *   with its files; each argument refused in order.
 * - rename: a directory with files to a free name, the files under the new
 *   name; a target that exists (a file, an empty directory, a full one)
 *   ALREADY_EXISTS, both as they were; a file source NOT_A_DIRECTORY;
 *   across volumes DIFFERENT_VOLUME where the machine has two; each level
 *   (NONE, DEVICE, MEDIA) as path_replace takes them.
 * - A tree written, listed and removed by the test through these calls,
 *   nothing left.
 * - The misuse harness: the two calls with zeroes and a destroyed
 *   instance.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * rmdir and rename (POSIX: rename replaces an empty directory target,
 * ENOTEMPTY or EEXIST for a full one), RemoveDirectoryW and MoveFileExW
 * (no directory target, no MOVEFILE_COPY_ALLOWED for directories across
 * volumes), C++'s std::filesystem::remove (one entry) and remove_all
 * (a tree, the library's), Rust's fs::remove_dir and fs::rename.
 */

/* [ADDED] to CR_RESULT, after CR_RESULT_FILES_NOT_LOCKED = 1019: */

	CR_RESULT_FILES_NOT_EMPTY = 1020, /**< A directory to remove holds entries. */

/**
 * @brief [ADDED] Removes an empty directory: rmdir, or RemoveDirectoryW. A
 * symbolic link to a directory, or a junction, is removed itself, never
 * what it points at.
 *
 * Thread: may overlap any call on the same instance but
 * crfiles_instance_destroy().
 *
 * @param[in] instance The instance.
 * @param[in] path     The directory.
 *
 * @retval CR_SUCCESS It is gone.
 * @retval CR_RESULT_FILES_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_FILES_NULL_PATH `path` is NULL or empty.
 * @retval CR_RESULT_FILES_NOT_FOUND Nothing is there.
 * @retval CR_RESULT_FILES_NOT_A_DIRECTORY A file, or another kind of
 *         entry, is there.
 * @retval CR_RESULT_FILES_NOT_EMPTY It holds entries.
 * @retval CR_RESULT_FILES_IN_USE Windows: another handle has it open
 *         without sharing delete.
 * @retval CR_RESULT_FILES_ACCESS_DENIED Its parent can't be written.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform refused.
 */
CR_RESULT crfiles_directory_remove(
    crfiles_instance instance,
    const char *path);

/**
 * @brief [ADDED] Gives a directory another name, on the same volume:
 * rename, or MoveFileExW (MOVEFILE_WRITE_THROUGH for DEVICE and MEDIA).
 * Nothing may be at the target, on every backend. `level` is how far the
 * rename is pushed before returning, as crfiles_path_replace() says.
 *
 * Thread: as crfiles_directory_remove().
 *
 * @param[in] instance The instance.
 * @param[in] source   The directory.
 * @param[in] target   The name it takes; nothing is there.
 * @param[in] level    How far the rename is pushed.
 *
 * @retval CR_SUCCESS `target` names the directory.
 * @retval CR_RESULT_FILES_INVALID_INSTANCE `instance` is NULL.
 * @retval CR_RESULT_FILES_NULL_PATH `source` or `target` is NULL or
 *         empty.
 * @retval CR_RESULT_DESC_INVALID_PARAMETERS `level` isn't an
 *         CRFILES_FLUSH_LEVEL.
 * @retval CR_RESULT_FILES_NOT_FOUND There is no `source`, or no parent
 *         of `target`.
 * @retval CR_RESULT_FILES_NOT_A_DIRECTORY `source` isn't a directory.
 * @retval CR_RESULT_FILES_ALREADY_EXISTS Something is at `target`.
 * @retval CR_RESULT_FILES_DIFFERENT_VOLUME They're on different
 *         volumes.
 * @retval CR_RESULT_FILES_IN_USE Windows: a handle inside it forbids
 *         the move.
 * @retval CR_RESULT_FILES_FLUSH_LEVEL_NOT_SUPPORTED See
 *         CRFILES_FLUSH_LEVEL_MEDIA.
 * @retval CR_RESULT_API_SPECIFIC_FAILURE The platform refused.
 */
CR_RESULT crfiles_directory_rename(
    crfiles_instance instance,
    const char *source,
    const char *target,
    CRFILES_FLUSH_LEVEL level);
