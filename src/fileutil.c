#define _XOPEN_SOURCE 700
#include "fileutil.h"
#include "sha256.h"
#include "portable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#ifdef _WIN32
#include <io.h>
#define fsync _commit
#endif

int compute_sha256_hex(const char *path, char out_hex[65]) {
    return sha256_file_hex(path, out_hex);
}

int ensure_parent_dir(const char *path) {
    char *dup = strdup(path);
    if (!dup) return -1;
    char *p = strrchr(dup, '/');
    int r = 0;
    if (p) {
        *p = '\0';
        // create directories recursively
        char tmp[4096];
        tmp[0] = '\0';
        char *tok = strtok(dup, "/");
        while (tok) {
            strcat(tmp, "/");
            strcat(tmp, tok);
            if (portable_mkdir(tmp, 0755) != 0) {
                if (errno != EEXIST) { r = -1; break; }
            }
            tok = strtok(NULL, "/");
        }
    }
    free(dup);
    return r;
}

int copy_file_atomic(const char *src, const char *dst) {
    int infd = open(src, O_RDONLY);
    if (infd < 0) return -1;
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", dst);
    int tmpfd = mkstemp(tmp);
    if (tmpfd < 0) { close(infd); return -1; }

    char buf[8192];
    ssize_t r;
    while ((r = read(infd, buf, sizeof(buf))) > 0) {
        ssize_t w = write(tmpfd, buf, r);
        if (w != r) { close(infd); close(tmpfd); unlink(tmp); return -1; }
    }
    fsync(tmpfd);
    close(tmpfd);
    close(infd);
    if (rename(tmp, dst) != 0) { unlink(tmp); return -1; }
    return 0;
}

static int is_dot_or_dotdot(const char *name) {
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

int sync_from_peer(const char *peer_root, const char *local_root) {
    size_t rootlen = strlen(peer_root);
    if (peer_root[rootlen-1] == '/') rootlen--; // ignore trailing slash

    // recursive traversal stack via simple recursion
    struct stack_item { char path[4096]; };

    // Use our own recursion function
    // Define nested function via helper
    int sync_walk(const char *curpath) {
        DIR *d = opendir(curpath);
        if (!d) return -1;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (is_dot_or_dotdot(ent->d_name)) continue;
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", curpath, ent->d_name);
            struct stat st;
            if (stat(full, &st) != 0) { closedir(d); return -1; }
            if (S_ISDIR(st.st_mode)) {
                // recurse
                if (sync_walk(full) != 0) { closedir(d); return -1; }
            } else if (S_ISREG(st.st_mode)) {
                // compute relative path
                const char *rel = full + rootlen;
                if (*rel == '/') rel++;
                char dst[4096];
                snprintf(dst, sizeof(dst), "%s/%s", local_root, rel);

                // check if exists and compare sha256
                char src_hash[65] = {0}, dst_hash[65] = {0};
                if (compute_sha256_hex(full, src_hash) != 0) { closedir(d); return -1; }
                int need_copy = 0;
                if (access(dst, F_OK) != 0) {
                    need_copy = 1;
                } else {
                    if (compute_sha256_hex(dst, dst_hash) != 0) {
                        need_copy = 1;
                    } else {
                        if (strcmp(src_hash, dst_hash) != 0) need_copy = 1;
                    }
                }
                if (need_copy) {
                    // ensure parent dir exists
                    if (ensure_parent_dir(dst) != 0) { closedir(d); return -1; }
                    if (copy_file_atomic(full, dst) != 0) { closedir(d); return -1; }
                    printf("copied: %s -> %s\n", full, dst);
                }
            }
        }
        closedir(d);
        return 0;
    }

    return sync_walk(peer_root);
}
