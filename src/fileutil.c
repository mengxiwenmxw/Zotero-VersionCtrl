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
#ifndef O_BINARY
#define O_BINARY _O_BINARY
#endif
#endif
#ifndef O_BINARY
#define O_BINARY 0
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
        int absolute = (dup[0] == '/');
        char *saveptr = NULL;
        char *tok = strtok_r(dup, "/\\", &saveptr);
        while (tok) {
            size_t used = strlen(tmp);
            size_t need = used + strlen(tok) + 2;
            if (need > sizeof(tmp)) { r = -1; break; }
            if (used == 0) {
                if (absolute) {
                    snprintf(tmp, sizeof(tmp), "/%s", tok);
                } else {
                    snprintf(tmp, sizeof(tmp), "%s", tok);
                }
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

static int copy_stream_to_fd(int infd, int outfd) {
    char buf[8192];
    ssize_t r;
    while ((r = read(infd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < r) {
            ssize_t w = write(outfd, buf + written, (size_t)(r - written));
            if (w < 0) return -1;
            written += w;
        }
    }
    return r < 0 ? -1 : 0;
}

static int copy_file_to_temp(const char *src, const char *dst_tmp) {
    int infd = open(src, O_RDONLY | O_BINARY);
    if (infd < 0) {
        fprintf(stderr, "copy_file_atomic: open failed for %s: %s\n", src, strerror(errno));
        return -1;
    }

    int outfd = open(dst_tmp, O_WRONLY | O_TRUNC | O_BINARY);
    if (outfd < 0) {
        fprintf(stderr, "copy_file_atomic: open temp failed for %s: %s\n", dst_tmp, strerror(errno));
        close(infd);
        return -1;
    }

    int rc = copy_stream_to_fd(infd, outfd);
    if (rc != 0) {
        fprintf(stderr, "copy_file_atomic: copy failed for %s -> %s: %s\n", src, dst_tmp, strerror(errno));
    }

    if (fsync(outfd) != 0) {
        fprintf(stderr, "copy_file_atomic: fsync failed for %s: %s\n", dst_tmp, strerror(errno));
        rc = -1;
    }

    close(outfd);
    close(infd);
    return rc;
}

int copy_file_atomic(const char *src, const char *dst) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", dst) >= (int)sizeof(tmp)) {
        fprintf(stderr, "copy_file_atomic: path too long for %s\n", dst);
        return -1;
    }

    if (ensure_parent_dir(dst) != 0) {
        fprintf(stderr, "copy_file_atomic: failed to create parent dir for %s\n", dst);
        return -1;
    }

    int tmpfd = mkstemp(tmp);
    if (tmpfd < 0) {
        fprintf(stderr, "copy_file_atomic: mkstemp failed for %s: %s\n", dst, strerror(errno));
        return -1;
    }
    close(tmpfd);

    int rc = copy_file_to_temp(src, tmp);
    if (rc != 0) {
        unlink(tmp);
        return -1;
    }

    if (portable_replace_file(tmp, dst) != 0) {
        fprintf(stderr, "copy_file_atomic: replace failed for %s -> %s: %s\n", tmp, dst, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int is_dot_or_dotdot(const char *name) {
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

static int has_suffix(const char *s, const char *suffix) {
    size_t slen = strlen(s);
    size_t sufflen = strlen(suffix);
    return slen >= sufflen && strcmp(s + slen - sufflen, suffix) == 0;
}

static int is_sqlite_primary(const char *rel) {
    return has_suffix(rel, ".sqlite") || has_suffix(rel, ".db") || strcmp(rel, "zotero.sqlite") == 0;
}

static int is_sqlite_sidecar(const char *rel) {
    return has_suffix(rel, "-wal") || has_suffix(rel, "-shm") || has_suffix(rel, "-journal");
}

static int is_regular_file_path(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int path_needs_copy(const char *src, const char *dst) {
    struct stat sst;
    struct stat dstst;
    if (stat(src, &sst) != 0 || !S_ISREG(sst.st_mode)) return 0;
    if (stat(dst, &dstst) != 0 || !S_ISREG(dstst.st_mode)) return 1;
    if (sst.st_size != dstst.st_size) return 1;

    char src_hash[65];
    char dst_hash[65];
    if (compute_sha256_hex(src, src_hash) != 0) return 1;
    if (compute_sha256_hex(dst, dst_hash) != 0) return 1;
    return strcmp(src_hash, dst_hash) != 0;
}

static int sqlite_group_needs_copy(const char *primary_src, const char *primary_dst, const char *rel, const char *local_root) {
    char sidecar_src[PATH_MAX];
    char sidecar_dst[PATH_MAX];
    static const char *suffixes[] = {"-wal", "-shm", "-journal"};

    if (path_needs_copy(primary_src, primary_dst)) {
        return 1;
    }

    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        if (snprintf(sidecar_src, sizeof(sidecar_src), "%s%s", primary_src, suffixes[i]) >= (int)sizeof(sidecar_src)) {
            return 1;
        }
        if (!is_regular_file_path(sidecar_src)) continue;
        if (portable_join_path(sidecar_dst, sizeof(sidecar_dst), local_root, rel) != 0) {
            return 1;
        }
        size_t dst_len = strlen(sidecar_dst);
        size_t suff_len = strlen(suffixes[i]);
        if (dst_len + suff_len + 1 > sizeof(sidecar_dst)) {
            return 1;
        }
        memcpy(sidecar_dst + dst_len, suffixes[i], suff_len + 1);
        if (path_needs_copy(sidecar_src, sidecar_dst)) {
            return 1;
        }
    }

    return 0;
}

static int should_skip_sync_entry(const char *rel, const char *name) {
    if (!rel || !name) return 1;
    if (strcmp(name, ".zvcs") == 0 || strcmp(name, ".zvcsignore") == 0) return 1;
    if (strcmp(name, "peers.conf") == 0) return 1;
    if (strcmp(name, "zvcs") == 0 || strcmp(name, "zvcs.exe") == 0) return 1;
    if (strncmp(rel, ".zvcs/", 6) == 0 || strcmp(rel, ".zvcs") == 0) return 1;
    if (strcmp(rel, ".zvcsignore") == 0 || strcmp(rel, "peers.conf") == 0) return 1;
    return 0;
}

struct sync_item {
    char src[PATH_MAX];
    char dst[PATH_MAX];
    char rel[PATH_MAX];
    int sqlite_group;
    int sqlite_primary;
};

struct sync_plan {
    struct sync_item *items;
    size_t count;
    size_t cap;
};

static void free_sync_plan(struct sync_plan *plan) {
    free(plan->items);
    plan->items = NULL;
    plan->count = 0;
    plan->cap = 0;
}

static int append_sync_item(struct sync_plan *plan, const char *src, const char *dst, const char *rel, int sqlite_group, int sqlite_primary) {
    if (plan->count == plan->cap) {
        size_t ncap = plan->cap == 0 ? 32 : plan->cap * 2;
        struct sync_item *tmp = realloc(plan->items, ncap * sizeof(*tmp));
        if (!tmp) return -1;
        plan->items = tmp;
        plan->cap = ncap;
    }
    snprintf(plan->items[plan->count].src, sizeof(plan->items[plan->count].src), "%s", src);
    snprintf(plan->items[plan->count].dst, sizeof(plan->items[plan->count].dst), "%s", dst);
    snprintf(plan->items[plan->count].rel, sizeof(plan->items[plan->count].rel), "%s", rel);
    plan->items[plan->count].sqlite_group = sqlite_group;
    plan->items[plan->count].sqlite_primary = sqlite_primary;
    plan->count++;
    return 0;
}

static int schedule_sqlite_sidecars(struct sync_plan *plan, const char *local_root, const char *src, const char *rel, int sqlite_group) {
    static const char *suffixes[] = {"-wal", "-shm", "-journal"};
    char sidecar_rel[PATH_MAX];
    char sidecar_src[PATH_MAX];
    char sidecar_dst[PATH_MAX];

    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        if (snprintf(sidecar_rel, sizeof(sidecar_rel), "%s%s", rel, suffixes[i]) >= (int)sizeof(sidecar_rel)) {
            return -1;
        }
        if (snprintf(sidecar_src, sizeof(sidecar_src), "%s%s", src, suffixes[i]) >= (int)sizeof(sidecar_src)) {
            return -1;
        }
        if (access(sidecar_src, F_OK) != 0) continue;
        if (portable_join_path(sidecar_dst, sizeof(sidecar_dst), local_root, sidecar_rel) != 0) {
            return -1;
        }
        if (append_sync_item(plan, sidecar_src, sidecar_dst, sidecar_rel, sqlite_group, 0) != 0) {
            return -1;
        }
    }
    return 0;
}

static int collect_sync_plan(const char *curpath, const char *local_root, size_t rootlen, struct sync_plan *plan) {
    DIR *d = opendir(curpath);
    if (!d) {
        if (errno == ENOENT) {
            fprintf(stderr, "sync: directory disappeared, skipping %s\n", curpath);
            return 0;
        }
        fprintf(stderr, "sync: opendir failed for %s: %s\n", curpath, strerror(errno));
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (is_dot_or_dotdot(ent->d_name)) continue;

        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", curpath, ent->d_name) >= (int)sizeof(full)) {
            fprintf(stderr, "sync: path too long under %s\n", curpath);
            closedir(d);
            return -1;
        }

        struct stat st;
        if (stat(full, &st) != 0) {
            if (errno == ENOENT) {
                fprintf(stderr, "sync: source disappeared, skipping %s\n", full);
                continue;
            }
            fprintf(stderr, "sync: stat failed for %s: %s\n", full, strerror(errno));
            closedir(d);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            if (collect_sync_plan(full, local_root, rootlen, plan) != 0) {
                fprintf(stderr, "sync: descending into %s failed\n", full);
                closedir(d);
                return -1;
            }
            continue;
        }

        if (!S_ISREG(st.st_mode)) continue;

        const char *rel = full + rootlen;
        while (*rel == '/' || *rel == '\\') rel++;
        if (should_skip_sync_entry(rel, ent->d_name)) continue;

        char dst[PATH_MAX];
        if (portable_join_path(dst, sizeof(dst), local_root, rel) != 0) {
            fprintf(stderr, "sync: destination path too long for %s\n", rel);
            closedir(d);
            return -1;
        }

        if (is_sqlite_sidecar(rel)) {
            continue;
        }

        int sqlite_group = is_sqlite_primary(rel);
        if (!sqlite_group && !path_needs_copy(full, dst)) {
            continue;
        }
        if (sqlite_group && !sqlite_group_needs_copy(full, dst, rel, local_root)) {
            continue;
        }

        if (append_sync_item(plan, full, dst, rel, sqlite_group, sqlite_group) != 0) {
            fprintf(stderr, "sync: failed to schedule %s\n", full);
            closedir(d);
            return -1;
        }

        if (sqlite_group) {
            if (schedule_sqlite_sidecars(plan, local_root, full, rel, sqlite_group) != 0) {
                fprintf(stderr, "sync: failed to schedule sqlite companions for %s\n", full);
                closedir(d);
                return -1;
            }
        }
    }

    closedir(d);
    return 0;
}

struct sqlite_txn {
    char primary_src[PATH_MAX];
    char primary_dst[PATH_MAX];
    char primary_tmp[PATH_MAX];
    char primary_rel[PATH_MAX];
    char sidecar_src[3][PATH_MAX];
    char sidecar_dst[3][PATH_MAX];
    char sidecar_tmp[3][PATH_MAX];
    char sidecar_rel[3][PATH_MAX];
    int sidecar_count;
};

static void cleanup_sqlite_txn(struct sqlite_txn *txn) {
    for (int i = 0; i < txn->sidecar_count; ++i) {
        if (txn->sidecar_tmp[i][0] != '\0') unlink(txn->sidecar_tmp[i]);
    }
    if (txn->primary_tmp[0] != '\0') unlink(txn->primary_tmp);
}

static int finalize_sqlite_txn(struct sqlite_txn *txn) {
    if (txn->primary_tmp[0] != '\0') {
        if (portable_replace_file(txn->primary_tmp, txn->primary_dst) != 0) {
            fprintf(stderr, "sync: replace failed for %s -> %s: %s\n", txn->primary_tmp, txn->primary_dst, strerror(errno));
            return -1;
        }
        txn->primary_tmp[0] = '\0';
    }
    for (int i = 0; i < txn->sidecar_count; ++i) {
        if (txn->sidecar_tmp[i][0] != '\0') {
            if (portable_replace_file(txn->sidecar_tmp[i], txn->sidecar_dst[i]) != 0) {
                fprintf(stderr, "sync: replace failed for %s -> %s: %s\n", txn->sidecar_tmp[i], txn->sidecar_dst[i], strerror(errno));
                return -1;
            }
            txn->sidecar_tmp[i][0] = '\0';
        }
    }
    return 0;
}

static int copy_sqlite_group(const struct sync_plan *plan, size_t start, size_t *advanced, char **failed_name, size_t *sqlite_copied) {
    struct sqlite_txn txn;
    memset(&txn, 0, sizeof(txn));

    const struct sync_item *primary = &plan->items[start];
    snprintf(txn.primary_src, sizeof(txn.primary_src), "%s", primary->src);
    snprintf(txn.primary_dst, sizeof(txn.primary_dst), "%s", primary->dst);
    snprintf(txn.primary_rel, sizeof(txn.primary_rel), "%s", primary->rel);

    char tmpbuf[PATH_MAX];
    if (snprintf(tmpbuf, sizeof(tmpbuf), "%s.sqlite_tmp.XXXXXX", primary->dst) >= (int)sizeof(tmpbuf)) {
        return -1;
    }

    if (ensure_parent_dir(primary->dst) != 0) {
        fprintf(stderr, "sync: failed to create parent dir for %s\n", primary->dst);
        return -1;
    }

    int fd = mkstemp(tmpbuf);
    if (fd < 0) {
        fprintf(stderr, "sync: mkstemp failed for sqlite primary %s: %s\n", primary->dst, strerror(errno));
        return -1;
    }
    close(fd);
    snprintf(txn.primary_tmp, sizeof(txn.primary_tmp), "%s", tmpbuf);

    if (copy_file_to_temp(primary->src, txn.primary_tmp) != 0) {
        *failed_name = strdup(primary->rel);
        cleanup_sqlite_txn(&txn);
        return -1;
    }

    size_t idx = start + 1;
    while (idx < plan->count && plan->items[idx].sqlite_group) {
        if (!plan->items[idx].sqlite_primary) {
            int side_i = txn.sidecar_count;
            if (side_i >= 3) {
                fprintf(stderr, "sync: too many sqlite sidecars for %s\n", primary->rel);
                cleanup_sqlite_txn(&txn);
                *failed_name = strdup(plan->items[idx].rel);
                return -1;
            }
            snprintf(txn.sidecar_dst[side_i], sizeof(txn.sidecar_dst[side_i]), "%s", plan->items[idx].dst);
            snprintf(txn.sidecar_rel[side_i], sizeof(txn.sidecar_rel[side_i]), "%s", plan->items[idx].rel);
            if (snprintf(tmpbuf, sizeof(tmpbuf), "%s.sqlite_tmp.XXXXXX", plan->items[idx].dst) >= (int)sizeof(tmpbuf)) {
                cleanup_sqlite_txn(&txn);
                *failed_name = strdup(plan->items[idx].rel);
                return -1;
            }
            if (ensure_parent_dir(plan->items[idx].dst) != 0) {
                cleanup_sqlite_txn(&txn);
                *failed_name = strdup(plan->items[idx].rel);
                return -1;
            }
            fd = mkstemp(tmpbuf);
            if (fd < 0) {
                fprintf(stderr, "sync: mkstemp failed for sqlite sidecar %s: %s\n", plan->items[idx].dst, strerror(errno));
                cleanup_sqlite_txn(&txn);
                *failed_name = strdup(plan->items[idx].rel);
                return -1;
            }
            close(fd);
            snprintf(txn.sidecar_tmp[side_i], sizeof(txn.sidecar_tmp[side_i]), "%s", tmpbuf);
            snprintf(txn.sidecar_src[side_i], sizeof(txn.sidecar_src[side_i]), "%s", plan->items[idx].src);
            if (copy_file_to_temp(plan->items[idx].src, txn.sidecar_tmp[side_i]) != 0) {
                *failed_name = strdup(plan->items[idx].rel);
                cleanup_sqlite_txn(&txn);
                return -1;
            }
            txn.sidecar_count++;
        }
        idx++;
        if (idx < plan->count && plan->items[idx].sqlite_primary) break;
    }

    if (finalize_sqlite_txn(&txn) != 0) {
        cleanup_sqlite_txn(&txn);
        *failed_name = strdup(primary->rel);
        return -1;
    }

    *advanced = idx - start;
    if (sqlite_copied) *sqlite_copied += *advanced;
    return 0;
}

static void append_failed(char ***failed, size_t *failed_count, size_t *failed_cap, const char *name) {
    if (*failed_count == *failed_cap) {
        size_t ncap = *failed_cap == 0 ? 8 : *failed_cap * 2;
        char **tmp = realloc(*failed, ncap * sizeof(*tmp));
        if (!tmp) return;
        *failed = tmp;
        *failed_cap = ncap;
    }
    (*failed)[*failed_count] = strdup(name);
    if ((*failed)[*failed_count]) (*failed_count)++;
}

static int copy_sync_plan(const struct sync_plan *plan, size_t *copied_out, size_t *failed_out, size_t *sqlite_out, size_t *regular_out) {
    char **failed = NULL;
    size_t failed_count = 0;
    size_t failed_cap = 0;
    size_t copied = 0;
    size_t sqlite_copied = 0;
    size_t regular_copied = 0;

    for (size_t i = 0; i < plan->count; ) {
        if (plan->items[i].sqlite_group && plan->items[i].sqlite_primary) {
            size_t advanced = 0;
            char *failed_name = NULL;
            if (copy_sqlite_group(plan, i, &advanced, &failed_name, &sqlite_copied) == 0) {
                copied += advanced;
                i += advanced;
                continue;
            }
            if (failed_name) {
                append_failed(&failed, &failed_count, &failed_cap, failed_name);
                free(failed_name);
            } else {
                append_failed(&failed, &failed_count, &failed_cap, plan->items[i].rel);
            }
            i += advanced > 0 ? advanced : 1;
            continue;
        }

        if (copy_file_atomic(plan->items[i].src, plan->items[i].dst) == 0) {
            copied++;
            regular_copied++;
        } else {
            append_failed(&failed, &failed_count, &failed_cap, plan->items[i].rel);
        }
        i++;
    }

    printf("sync summary: copied %zu/%zu file(s)\n", copied, plan->count);
    printf("sync classification: sqlite=%zu regular=%zu\n", sqlite_copied, regular_copied);
    if (failed_count > 0) {
        printf("failed files:\n");
        for (size_t i = 0; i < failed_count; ++i) {
            if (failed[i]) {
                printf(" - %s\n", failed[i]);
                free(failed[i]);
            }
        }
    }
    free(failed);

    if (copied_out) *copied_out = copied;
    if (failed_out) *failed_out = failed_count;
    if (sqlite_out) *sqlite_out = sqlite_copied;
    if (regular_out) *regular_out = regular_copied;
    return 0;
}

int sync_from_peer(const char *peer_root, const char *local_root) {
    size_t rootlen = strlen(peer_root);
    while (rootlen > 0 && (peer_root[rootlen - 1] == '/' || peer_root[rootlen - 1] == '\\')) {
        rootlen--;
    }

    struct sync_plan plan = {0};
    int rc = collect_sync_plan(peer_root, local_root, rootlen, &plan);
    if (rc != 0) {
        free_sync_plan(&plan);
        return -1;
    }

    size_t copied = 0;
    size_t failed = 0;
    size_t sqlite_copied = 0;
    size_t regular_copied = 0;
    rc = copy_sync_plan(&plan, &copied, &failed, &sqlite_copied, &regular_copied);
    free_sync_plan(&plan);
    if (rc != 0) return -1;
    return failed == 0 ? 0 : -1;
}
