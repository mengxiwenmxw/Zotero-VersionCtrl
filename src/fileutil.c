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
#include <limits.h>

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
#ifdef _WIN32
    char *q = strrchr(dup, '\\');
    if (!p || (q && q > p)) p = q;
#endif
    int r = 0;
    if (p) {
        *p = '\0';
        char tmp[PATH_MAX];
        tmp[0] = '\0';
        char *saveptr = NULL;
        char *tok = strtok_r(dup, "/\\", &saveptr);
        while (tok) {
            size_t used = strlen(tmp);
            size_t need = used + strlen(tok) + 2;
            if (need > sizeof(tmp)) { r = -1; break; }
            if (used == 0) {
                snprintf(tmp, sizeof(tmp), "%s", tok);
            } else {
                strcat(tmp, "/");
                strcat(tmp, tok);
            }
            if (portable_mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                r = -1;
                break;
            }
            tok = strtok_r(NULL, "/\\", &saveptr);
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
        ssize_t written = 0;
        while (written < r) {
            ssize_t w = write(tmpfd, buf + written, (size_t)(r - written));
            if (w < 0) { close(infd); close(tmpfd); unlink(tmp); return -1; }
            written += w;
        }
    }
    if (r < 0) { close(infd); close(tmpfd); unlink(tmp); return -1; }
    fsync(tmpfd);
    close(tmpfd);
    close(infd);
    if (rename(tmp, dst) != 0) {
        // On Windows, rename() does not overwrite an existing file.
        // Remove the destination and retry so sqlite and other tracked files can update.
        if (remove(dst) != 0 && errno != ENOENT) {
            unlink(tmp);
            return -1;
        }
        if (rename(tmp, dst) != 0) {
            unlink(tmp);
            return -1;
        }
    }
    return 0;
}

static int is_dot_or_dotdot(const char *name) {
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

struct sync_ctx {
    const char *local_root;
    size_t rootlen;
};

static int should_skip_sync_entry(const char *rel, const char *name) {
    if (!rel || !name) return 1;

    if (strcmp(name, ".zvcs") == 0 || strcmp(name, ".zvcsignore") == 0) return 1;
    if (strcmp(name, "peers.conf") == 0) return 1;
    if (strcmp(name, "zvcs") == 0 || strcmp(name, "zvcs.exe") == 0) return 1;
    if (strcmp(name, "zotero.sqlite-wal") == 0) return 1;
    if (strncmp(rel, ".zvcs/", 6) == 0 || strcmp(rel, ".zvcs") == 0) return 1;
    if (strcmp(rel, ".zvcsignore") == 0 || strcmp(rel, "peers.conf") == 0) return 1;
    if (strcmp(rel, "zotero.sqlite-wal") == 0) return 1;
    return 0;
}

static int sync_walk(const struct sync_ctx *ctx, const char *curpath) {
    DIR *d = opendir(curpath);
    if (!d) return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (is_dot_or_dotdot(ent->d_name)) continue;

        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", curpath, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) {
            closedir(d);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            if (sync_walk(ctx, full) != 0) {
                closedir(d);
                return -1;
            }
            continue;
        }

        if (!S_ISREG(st.st_mode)) continue;

        const char *rel = full + ctx->rootlen;
        while (*rel == '/' || *rel == '\\') rel++;
        if (should_skip_sync_entry(rel, ent->d_name)) continue;

        char dst[4096];
        snprintf(dst, sizeof(dst), "%s/%s", ctx->local_root, rel);

        char src_hash[65] = {0}, dst_hash[65] = {0};
        if (compute_sha256_hex(full, src_hash) != 0) {
            closedir(d);
            return -1;
        }

        int need_copy = 0;
        if (access(dst, F_OK) != 0) {
            need_copy = 1;
        } else if (compute_sha256_hex(dst, dst_hash) != 0) {
            need_copy = 1;
        } else if (strcmp(src_hash, dst_hash) != 0) {
            need_copy = 1;
        }

        if (need_copy) {
            if (ensure_parent_dir(dst) != 0) {
                closedir(d);
                return -1;
            }
            if (copy_file_atomic(full, dst) != 0) {
                closedir(d);
                return -1;
            }
            printf("copied: %s -> %s\n", full, dst);
        }
    }

    closedir(d);
    return 0;
}

int sync_from_peer(const char *peer_root, const char *local_root) {
    size_t rootlen = strlen(peer_root);
    while (rootlen > 0 && (peer_root[rootlen - 1] == '/' || peer_root[rootlen - 1] == '\\')) {
        rootlen--;
    }

    struct sync_ctx ctx = {
        .local_root = local_root,
        .rootlen = rootlen,
    };

    return sync_walk(&ctx, peer_root);
}
