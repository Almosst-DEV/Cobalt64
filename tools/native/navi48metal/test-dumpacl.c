// test-dumpacl.c: n48_dumpacl.h on a real temp directory under $N48_TEST_TMP (else $TMPDIR, else /private/tmp); removed with rmdir only. cc -O1 -Wall -Wextra -Werror -o t test-dumpacl.c && N48_TEST_TMP=<scratch> ./t
// Checks: the ACL entry for `nobody` is added (list/search), a second call is a no-op (0), the mode bits stay 0700, a bad fd fails.
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "n48_dumpacl.h"
static int fails, runs;
#define CHECK(name, cond) do { int ok_ = (cond) ? 1 : 0; runs++; if (!ok_) { fails++; printf("FAIL: %s\n", name); } } while (0)
int main(void) {
    char d[1100]; { const char *b = getenv("N48_TEST_TMP"); if (!b || !*b) b = getenv("TMPDIR"); if (!b || !*b) b = "/private/tmp"; snprintf(d, sizeof d, "%s/n48acl.XXXXXX", b); }
    if (!mkdtemp(d)) { perror("mkdtemp"); return 2; }
    chmod(d, 0700);
    int fd = open(d, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    CHECK("opened the temp directory", fd >= 0);
    CHECK("first grant adds an entry (1)", n48da_grant_list(fd, (uid_t)4294967294u) == 1);
    CHECK("second grant is a no-op (0)", n48da_grant_list(fd, (uid_t)4294967294u) == 0);
    struct stat st; fstat(fd, &st);
    CHECK("the mode bits are still 0700", (st.st_mode & 07777) == 0700);
    acl_t a = acl_get_fd_np(fd, ACL_TYPE_EXTENDED); int n = 0; acl_entry_t e;
    for (int w = ACL_FIRST_ENTRY; a && acl_get_entry(a, w, &e) == 0; w = ACL_NEXT_ENTRY) n++;
    CHECK("exactly one ACL entry", n == 1);
    if (a) { char *txt = acl_to_text(a, NULL); CHECK("entry is user:nobody allow read,execute(=list,search),readattr..., no write/delete", txt && strstr(txt, "nobody") && strstr(txt, ":allow:") && strstr(txt, "read,execute") && !strstr(txt, "write") && !strstr(txt, "delete")); if (txt) { printf("acl: %s", txt); acl_free(txt); } acl_free(a); }
    CHECK("a second user gets a second entry", n48da_grant_list(fd, getuid()) == 1);
    close(fd);
    CHECK("a closed fd fails", n48da_grant_list(fd, (uid_t)4294967294u) == -1);
    rmdir(d);   /* ACL'd dir: rmdir works for the owner */
    printf("test-dumpacl: %d checks, %d failed\n", runs, fails);
    return fails ? 1 : 0;
}
