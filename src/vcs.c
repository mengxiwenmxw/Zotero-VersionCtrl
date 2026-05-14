#include "vcs.h"

#include "fileutil.h"
#include "portable.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *ZVCS_DIR = ".zvcs";
static const char *OBJ_DIR = ".zvcs/objects";
static const char *COMMIT_DIR = ".zvcs/commits";
static const char *INDEX_FILE = ".zvcs/index";

struct file_entry {
    char *path;
    char sha[65];
};

static void free_entries(struct file_entry *entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; ++i) free(entries[i].path);
    free(entries);
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return portable_mkdir(path, 0755);
}

static int ensure_zvcs_dirs(void) {
    if (ensure_dir(ZVCS_DIR) != 0) return -1;
    if (ensure_dir(OBJ_DIR) != 0) return -1;
    if (ensure_dir(COMMIT_DIR) != 0) return -1;
    return 0;
}

int vcs_init_repo(void) {
    return ensure_zvcs_dirs();
}

static int path_is_ignored(const char *rel) {
    if (!rel || rel[0] == '\0') return 1;
    if (strcmp(rel, ".zvcs") == 0) return 1;
    if (strncmp(rel, ".zvcs/", 6) == 0) return 1;
    if (strcmp(rel, ".git") == 0) return 1;
    if (strncmp(rel, ".git/", 5) == 0) return 1;
    if (strcmp(rel, ".zvcsignore") == 0) return 1;
    if (strcmp(rel, "peers.conf") == 0) return 1;
    return 0;
}

static int append_entry(struct file_entry **entries, size_t *count, size_t *cap, const char *path, const char *sha) {
    if (*count == *cap) {
        size_t ncap = (*cap == 0) ? 32 : (*cap * 2);
        struct file_entry *n = realloc(*entries, ncap * sizeof(**entries));
        if (!n) return -1;
        *entries = n;
        *cap = ncap;
    }
    (*entries)[*count].path = strdup(path);
    if (!(*entries)[*count].path) return -1;
    snprintf((*entries)[*count].sha, sizeof((*entries)[*count].sha), "%s", sha);
    (*count)++;
    return 0;
}

static int walk_and_collect(const char *root, const char *cur, struct file_entry **entries, size_t *count, size_t *cap) {
    DIR *d = opendir(cur);
    if (!d) return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char full[PATH_MAX];
        if (portable_join_path(full, sizeof(full), cur, ent->d_name) != 0) {
            closedir(d);
            errno = ENAMETOOLONG;
            return -1;
        }

        struct stat st;
        if (stat(full, &st) != 0) {
            closedir(d);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            if (walk_and_collect(root, full, entries, count, cap) != 0) {
                closedir(d);
                return -1;
            }
            continue;
        }

        if (!S_ISREG(st.st_mode)) continue;

        const char *rel = full + strlen(root);
        while (*rel == '/' || *rel == '\\') rel++;
        if (path_is_ignored(rel)) continue;

        char sha[65];
        if (compute_sha256_hex(full, sha) != 0) {
            fprintf(stderr, "warning: skipping unreadable file: %s\n", full);
            closedir(d);
            continue;
        }

        char objpath[PATH_MAX];
        if (portable_join_path(objpath, sizeof(objpath), OBJ_DIR, sha) != 0) {
            closedir(d);
            errno = ENAMETOOLONG;
            return -1;
        }
        if (access(objpath, F_OK) != 0) {
            if (copy_file_atomic(full, objpath) != 0) {
                fprintf(stderr, "warning: failed to store blob for %s\n", full);
                continue;
            }
        }

        if (append_entry(entries, count, cap, rel, sha) != 0) {
            fprintf(stderr, "warning: failed to record entry for %s\n", full);
            continue;
        }
    }

    closedir(d);
    return 0;
}

static int write_index(const struct file_entry *entries, size_t count) {
    FILE *f = fopen(INDEX_FILE, "wb");
    if (!f) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (fprintf(f, "%s %s\n", entries[i].sha, entries[i].path) < 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

int vcs_commit(const char *message) {
    if (vcs_init_repo() != 0) return -1;

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return -1;

    struct file_entry *entries = NULL;
    size_t count = 0;
    size_t cap = 0;

    if (walk_and_collect(cwd, cwd, &entries, &count, &cap) != 0) {
        free_entries(entries, count);
        return -1;
    }

    if (count == 0) {
        fprintf(stderr, "no trackable files found for commit\n");
        free_entries(entries, count);
        return -1;
    }

    time_t now = time(NULL);
    const char *msg = message ? message : "(no message)";

    char meta[1024];
    snprintf(meta, sizeof(meta), "timestamp: %ld\nmessage: %s\n\n", (long)now, msg);

    size_t content_len = strlen(meta);
    for (size_t i = 0; i < count; ++i) {
        content_len += strlen(entries[i].sha) + 1 + strlen(entries[i].path) + 1;
    }

    char *content = malloc(content_len + 1);
    if (!content) {
        free_entries(entries, count);
        return -1;
    }

    size_t off = 0;
    memcpy(content + off, meta, strlen(meta));
    off += strlen(meta);
    for (size_t i = 0; i < count; ++i) {
        int n = snprintf(content + off, content_len + 1 - off, "%s %s\n", entries[i].sha, entries[i].path);
        if (n < 0) {
            free(content);
            free_entries(entries, count);
            return -1;
        }
        off += (size_t)n;
    }
    content[off] = '\0';

    char tmpname[PATH_MAX];
    snprintf(tmpname, sizeof(tmpname), ".zvcs/commit_tmp_%ld", (long)getpid());
    FILE *tmp = fopen(tmpname, "wb");
    if (!tmp) {
        free(content);
        free_entries(entries, count);
        return -1;
    }
    fwrite(content, 1, strlen(content), tmp);
    fclose(tmp);

    char commit_sha[65];
    if (compute_sha256_hex(tmpname, commit_sha) != 0) {
        unlink(tmpname);
        free(content);
        free_entries(entries, count);
        return -1;
    }
    unlink(tmpname);

    char commitfile[PATH_MAX];
    if (portable_join_path(commitfile, sizeof(commitfile), COMMIT_DIR, commit_sha) != 0) {
        free(content);
        free_entries(entries, count);
        return -1;
    }
    if (access(commitfile, F_OK) != 0) {
        FILE *cf = fopen(commitfile, "wb");
        if (!cf) {
            free(content);
            free_entries(entries, count);
            return -1;
        }
        fwrite(content, 1, strlen(content), cf);
        fclose(cf);
    }

    if (write_index(entries, count) != 0) {
        free(content);
        free_entries(entries, count);
        return -1;
    }

    printf("Created commit %s with %zu tracked file(s)\n", commit_sha, count);

    free(content);
    free_entries(entries, count);
    return 0;
}

static int compare_prefix(const char *candidate, const char *prefix) {
    return strncmp(candidate, prefix, strlen(prefix)) == 0;
}

int vcs_log(void) {
    if (vcs_init_repo() != 0) return -1;

    DIR *d = opendir(COMMIT_DIR);
    if (!d) return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[PATH_MAX];
        if (portable_join_path(path, sizeof(path), COMMIT_DIR, ent->d_name) != 0) continue;

        FILE *f = fopen(path, "r");
        if (!f) {
            printf("%s\n", ent->d_name);
            continue;
        }

        char line[1024];
        char ts[128] = "unknown";
        char msg[512] = "(no message)";
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "timestamp:", 10) == 0) {
                strncpy(ts, line + 10, sizeof(ts) - 1);
                ts[sizeof(ts) - 1] = '\0';
            } else if (strncmp(line, "message:", 8) == 0) {
                strncpy(msg, line + 8, sizeof(msg) - 1);
                msg[sizeof(msg) - 1] = '\0';
            } else if (line[0] == '\n' || line[0] == '\r') {
                break;
            }
        }
        fclose(f);

        size_t len = strlen(ts);
        while (len > 0 && (ts[0] == ' ' || ts[0] == '\t')) {
            memmove(ts, ts + 1, len);
            len--;
        }
        while (len > 0 && (ts[len - 1] == '\n' || ts[len - 1] == '\r' || ts[len - 1] == ' ')) {
            ts[--len] = '\0';
        }
        len = strlen(msg);
        while (len > 0 && (msg[0] == ' ' || msg[0] == '\t')) {
            memmove(msg, msg + 1, len);
            len--;
        }
        while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) {
            msg[--len] = '\0';
        }

        printf("%s | %s | %s\n", ent->d_name, ts, msg);
    }

    closedir(d);
    return 0;
}

int vcs_checkout(const char *commit_prefix) {
    if (vcs_init_repo() != 0) return -1;

    DIR *d = opendir(COMMIT_DIR);
    if (!d) return -1;

    char match[PATH_MAX] = {0};
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (compare_prefix(ent->d_name, commit_prefix)) {
            snprintf(match, sizeof(match), "%s", ent->d_name);
            break;
        }
    }
    closedir(d);

    if (match[0] == '\0') {
        fprintf(stderr, "commit %s not found\n", commit_prefix);
        return -1;
    }

    char commitfile[PATH_MAX];
    if (portable_join_path(commitfile, sizeof(commitfile), COMMIT_DIR, match) != 0) return -1;

    FILE *cf = fopen(commitfile, "r");
    if (!cf) return -1;

    struct file_entry *entries = NULL;
    size_t count = 0;
    size_t cap = 0;

    char line[8192];
    int in_entries = 0;
    while (fgets(line, sizeof(line), cf)) {
        if (!in_entries) {
            if (line[0] == '\n' || line[0] == '\r') in_entries = 1;
            continue;
        }

        char sha[65];
        char rel[PATH_MAX];
        if (sscanf(line, "%64s %4095[^\n]", sha, rel) != 2) continue;
        if (append_entry(&entries, &count, &cap, rel, sha) != 0) {
            fclose(cf);
            free_entries(entries, count);
            return -1;
        }
    }
    fclose(cf);

    for (size_t i = 0; i < count; ++i) {
        char blob[PATH_MAX];
        if (portable_join_path(blob, sizeof(blob), OBJ_DIR, entries[i].sha) != 0) {
            free_entries(entries, count);
            return -1;
        }

        char dst[PATH_MAX];
        if (portable_join_path(dst, sizeof(dst), ".", entries[i].path) != 0) {
            free_entries(entries, count);
            return -1;
        }

        if (ensure_parent_dir(dst) != 0) {
            free_entries(entries, count);
            return -1;
        }

        if (copy_file_atomic(blob, dst) != 0) {
            free_entries(entries, count);
            return -1;
        }

        printf("restored %s\n", entries[i].path);
    }

    free_entries(entries, count);
    return 0;
}

int vcs_status(void) {
    if (vcs_init_repo() != 0) return -1;

    struct stat st;
    printf("repository:\n");
    printf(" - .zvcs: %s\n", stat(ZVCS_DIR, &st) == 0 ? "present" : "missing");
    printf(" - objects: %s\n", stat(OBJ_DIR, &st) == 0 ? "present" : "missing");
    printf(" - commits: %s\n", stat(COMMIT_DIR, &st) == 0 ? "present" : "missing");
    printf(" - index: %s\n", stat(INDEX_FILE, &st) == 0 ? "present" : "missing");
    return 0;
}
