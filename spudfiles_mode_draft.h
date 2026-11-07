/*
 * spudfiles_mode_draft.h - SpudFiles: copying one file's permissions onto
 * another (card S42, decided 2026-11-07 with the user, b).
 *
 * DRAFT, 2026-11-07, first version. APPROVED 2026-11-07 (Q1 a, Q2 a, card
 * S42): the contract, kept as the record. Not built, not in any target. Not compilable: it repeats spudfiles.h's declarations to show
 * them changed. Every declaration is marked [ADDED] or [CHANGED]. Written
 * against include/spudfiles.h as it stands after card S39 (built
 * 2026-11-06), beside the approved spudfiles_directory_draft.h (card S41),
 * which it doesn't touch.
 *
 * ---------------------------------------------------------------------------
 * Why now
 * ---------------------------------------------------------------------------
 *
 * A file SpudFiles creates takes the platform's default permissions (0666
 * less the umask on POSIX, spudfiles.h's rules), and no call changes them.
 * A caller that copies a program through SpudFiles (reading one file and
 * writing another) gets a copy that, on Linux and every Apple system, can't
 * be run. The card's answer: one call that copies the permissions a file
 * already has onto another, so the copy is what its source is and SpudFiles
 * decides nothing about which files are programs. A plain translation: stat
 * and chmod on POSIX.
 *
 * ---------------------------------------------------------------------------
 * Questions for approval
 * ---------------------------------------------------------------------------
 *
 * Q1. Which POSIX bits are copied?
 *     a. (Recommended, drafted) The nine permission bits, read, write and
 *        run for the owner, the group and others (st_mode & 0777). The
 *        set-user-ID, set-group-ID and sticky bits (07000) are not: what
 *        they grant depends on the file's owner and group, which a copy
 *        doesn't carry (chown is the superuser's), so carried onto a file
 *        of another owner they would grant something else than they did.
 *        Linux also clears set-group-ID silently in chmod for a caller
 *        outside the file's group, so "copied" couldn't be promised. That
 *        is cp's own split (cp without -p never carries them either).
 *     b. All twelve (st_mode & 07777), as `cp -p` and Python's
 *        shutil.copymode do, with set-group-ID sometimes dropped by the
 *        kernel without a failure.
 *
 * Q2. What is copied on Windows?
 *     The card said "a no-op where the platform has no mode". Writing this
 *     draft found that Windows does keep one per-file permission of the same
 *     kind: the read-only attribute (FILE_ATTRIBUTE_READONLY), which is what
 *     a POSIX file with no write bit is. Python's os.chmod and
 *     shutil.copymode and Rust's fs::set_permissions carry it on Windows,
 *     and only it.
 *     a. (Recommended, drafted) The read-only attribute: the target is made
 *        read-only, or not, as the source is (SetFileAttributesW, every
 *        other attribute of the target kept). Then one observable rule holds
 *        on every backend: a source that can't be written gives a target
 *        that can't be written, and one that can, a target that can; POSIX
 *        adds who may run it. Access control lists are not copied on any
 *        backend (nor POSIX ACLs, extended attributes, or Apple's quarantine
 *        flag and signatures).
 *     b. Nothing on Windows, as the card said: the call checks its
 *        arguments and that both paths name something, and returns. A
 *        read-only source then gives a writable copy on Windows only.
 *
 * Stated, not asked:
 * - Symbolic links are followed, for the source (stat) and the target
 *   (chmod): the permissions are those of what each leads to, as
 *   spudfile_path_get_info() reports. Linux can't change a link's own
 *   permissions, so "follow" is the rule every backend can keep.
 * - The umask plays no part: chmod and SetFileAttributesW set what they
 *   are given. The bits copied are the source's as they are.
 * - Any kind of entry may be either path (a directory's permissions are
 *   copied the same way). The two needn't be the same kind.
 * - Nothing is flushed: like spudfile_path_get_info(), no flush level.
 *   A crash soon after can undo it; the caller that needs it lasting
 *   flushes the file, or replaces into its place, after.
 * - The source and the target may be on different volumes.
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * Added: one call. No result code (every answer is an existing one), no
 * struct, no change to any existing call. spudfiles.h's file comment
 * changes one rule's words ([CHANGED] below).
 *
 * ---------------------------------------------------------------------------
 * Tests (spudfiles_test, extended; every backend)
 * ---------------------------------------------------------------------------
 *
 * - Each argument refused in order; a missing source and a missing target
 *   each NOT_FOUND, the target unchanged.
 * - POSIX: a 0755 source onto a 0644 target gives 0755, a 0444 one 0444,
 *   a 0600 one 0600, with the process's umask set to 077 for the test
 *   (put back after) to show it plays no part; a source with set-user-ID
 *   and set-group-ID (made by the test on its own file) gives the target
 *   its nine bits only (Q1 a); a symbolic link as the source and as the
 *   target, what each leads to read and changed, the links themselves
 *   unchanged (lstat); a directory as either path; a target owned by
 *   another user, where the machine has one, ACCESS_DENIED (EPERM).
 * - Windows: a read-only source makes a writable target read-only, and a
 *   writable source a read-only target writable again (Q2 a), the target's
 *   hidden attribute kept through both; a junction as the target, its
 *   directory changed; a directory as either path.
 * - The misuse harness: the call with zeroes and a destroyed instance.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * stat/chmod and fchmodat (AT_SYMLINK_NOFOLLOW is EOPNOTSUPP on Linux);
 * cp and cp -p (GNU coreutils: the 07000 bits only with -p); Python's
 * shutil.copymode (S_IMODE, 07777; on Windows only the read-only flag
 * through os.chmod) and Rust's fs::set_permissions (all mode bits on Unix,
 * readonly on Windows); C++'s std::filesystem::permissions (perms::mask,
 * 07777, and on Windows the read-only attribute by owner_write);
 * GetFileAttributesW and SetFileAttributesW.
 */

/* [CHANGED] spudfiles.h's file comment, "Rules that hold on every backend",
 * the permissions rule. Before:
 *
 * - New files and directories get the platform's default permissions, with
 *   the process's umask applied on POSIX (0666 and 0777 before it).
 *
 * After:
 *
 * - New files and directories get the platform's default permissions, with
 *   the process's umask applied on POSIX (0666 and 0777 before it);
 *   spudfile_path_copy_mode() gives one the permissions of another.
 */

/**
 * @brief [ADDED] Gives `target` the permissions `source` has: on Linux and
 * Apple, its nine permission bits (stat, then chmod; st_mode & 0777); on
 * Windows, its read-only attribute (GetFileAttributesW, then
 * SetFileAttributesW, the target's other attributes kept). Symbolic links
 * are followed for both. The set-user-ID, set-group-ID and sticky bits,
 * the owner, access control lists and extended attributes are not copied.
 * The umask plays no part. Nothing is flushed.
 *
 * Thread: may overlap any call on the same instance but
 * spudfile_instance_destroy(); two calls changing one target at once leave
 * it as either one left it.
 *
 * @param[in] instance The instance.
 * @param[in] source   What the permissions are read from.
 * @param[in] target   What they are given to.
 *
 * @retval SPUD_SUCCESS `target` has `source`'s permissions.
 * @retval SPUDRESULT_SPUDFILE_INVALID_INSTANCE `instance` is NULL.
 * @retval SPUDRESULT_SPUDFILE_NULL_PATH `source` or `target` is NULL or
 *         empty.
 * @retval SPUDRESULT_SPUDFILE_NOT_FOUND Nothing is at `source`, or at
 *         `target` (the target unchanged either way).
 * @retval SPUDRESULT_SPUDFILE_ACCESS_DENIED A directory on the way can't be
 *         read, or the platform won't let the caller change `target`'s
 *         permissions (POSIX: not its owner).
 * @retval SPUDRESULT_API_SPECIFIC_FAILURE The platform refused (a
 *         read-only file system among them).
 */
SPUDRESULT spudfile_path_copy_mode(
    spudfile_instance instance,
    const char *source,
    const char *target);
