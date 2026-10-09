// n48_dumpacl.h - grant the translate daemon's user (a different, unprivileged account) LIST + SEARCH on one dump subdirectory through an extended ACL entry, without touching the mode bits (C1).
// The per-uid dump subdirectory stays 0700 for every other account; the daemon (`nobody`) can then enumerate it and read the 0644 .air files in it.
// Everything works on an OPEN FILE DESCRIPTOR of the directory (opened O_NOFOLLOW | O_DIRECTORY by the caller after its lstat checks), so a directory swapped for a symlink between the check and
// the ACL write cannot redirect it. Idempotent: an existing entry for the same user is left alone. No Foundation: host-testable (test-dumpacl.c).
#ifndef N48_DUMPACL_H
#define N48_DUMPACL_H
#include <sys/types.h>
#include <sys/acl.h>
#include <membership.h>
#include <string.h>
#include <errno.h>

// Returns 1 = entry added, 0 = already present, -1 = failure (errno set). `uid` is the account to grant.
static inline int n48da_grant_list(int dirFd, uid_t uid) {
    uuid_t want;
    if (mbr_uid_to_uuid(uid, want) != 0) { errno = ENOENT; return -1; }
    acl_t acl = acl_get_fd_np(dirFd, ACL_TYPE_EXTENDED);
    if (!acl) acl = acl_init(1);
    if (!acl) return -1;
    acl_entry_t e;
    for (int which = ACL_FIRST_ENTRY; acl_get_entry(acl, which, &e) == 0; which = ACL_NEXT_ENTRY) {
        acl_tag_t tag;
        if (acl_get_tag_type(e, &tag) != 0 || tag != ACL_EXTENDED_ALLOW) continue;
        void *q = acl_get_qualifier(e);
        const int same = q && memcmp(q, want, sizeof(uuid_t)) == 0;
        if (q) acl_free(q);
        if (same) { acl_free(acl); return 0; }
    }
    int rc = -1;
    acl_permset_t ps;
    if (acl_create_entry(&acl, &e) == 0 && acl_set_tag_type(e, ACL_EXTENDED_ALLOW) == 0 && acl_set_qualifier(e, want) == 0 && acl_get_permset(e, &ps) == 0 &&
        acl_clear_perms(ps) == 0 && acl_add_perm(ps, ACL_LIST_DIRECTORY) == 0 && acl_add_perm(ps, ACL_SEARCH) == 0 && acl_add_perm(ps, ACL_READ_ATTRIBUTES) == 0 &&
        acl_add_perm(ps, ACL_READ_EXTATTRIBUTES) == 0 && acl_add_perm(ps, ACL_READ_SECURITY) == 0 && acl_set_permset(e, ps) == 0) {
        rc = acl_set_fd_np(dirFd, acl, ACL_TYPE_EXTENDED) == 0 ? 1 : -1;
    }
    const int sv = errno;
    acl_free(acl);
    errno = sv;
    return rc;
}
#endif
