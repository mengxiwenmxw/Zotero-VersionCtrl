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

static int append_text(char **buf, size_t *len, size_t *cap, const char *text, size_t text_len) {
    if (*len + text_len + 1 > *cap) {
        size_t ncap = (*cap == 0) ? 1024 : *cap;
        while (*len + text_len + 1 > ncap) ncap *= 2;
        char *tmp = realloc(*buf, ncap);
        if (!tmp) return -1;
        *buf = tmp;
        *cap = ncap;
    }
    memcpy(*buf + *len, text, text_len);
    *len += text_len;
    (*buf)[*len] = '\0';
    return 0;
}

static void collapse_newlines(char *s) {
    if (!s) return;
    for (char *p = s; *p; ++p) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') {
        s[--len] = '\0';
    }
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

    size_t meta_len = (size_t)snprintf(NULL, 0, "timestamp: %ld\nmessage: %s\n\n", (long)now, msg);
    char *meta = malloc(meta_len + 1);
    if (!meta) {
        free_entries(entries, count);
        return -1;
    }
    snprintf(meta, meta_len + 1, "timestamp: %ld\nmessage: %s\n\n", (long)now, msg);

    size_t content_len = meta_len;
    for (size_t i = 0; i < count; ++i) {
        content_len += strlen(entries[i].sha) + 1 + strlen(entries[i].path) + 1;
    }

    char *content = malloc(content_len + 1);
    if (!content) {
        free(meta);
        free_entries(entries, count);
        return -1;
    }

    size_t off = 0;
    memcpy(content + off, meta, meta_len);
    off += meta_len;
    for (size_t i = 0; i < count; ++i) {
        int n = snprintf(content + off, content_len + 1 - off, "%s %s\n", entries[i].sha, entries[i].path);
        if (n < 0) {
            free(meta);
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
        free(meta);
        free(content);
        free_entries(entries, count);
        return -1;
    }
    fwrite(content, 1, strlen(content), tmp);
    fclose(tmp);

    char commit_sha[65];
    if (compute_sha256_hex(tmpname, commit_sha) != 0) {
        unlink(tmpname);
        free(meta);
        free(content);
        free_entries(entries, count);
        return -1;
    }
    unlink(tmpname);

    char commitfile[PATH_MAX];
    if (portable_join_path(commitfile, sizeof(commitfile), COMMIT_DIR, commit_sha) != 0) {
        free(meta);
        free(content);
        free_entries(entries, count);
        return -1;
    }
    if (access(commitfile, F_OK) != 0) {
        FILE *cf = fopen(commitfile, "wb");
        if (!cf) {
            free(meta);
            free(content);
            free_entries(entries, count);
            return -1;
        }
        fwrite(content, 1, strlen(content), cf);
        fclose(cf);
    }

    if (write_index(entries, count) != 0) {
        free(meta);
        free(content);
        free_entries(entries, count);
        return -1;
    }

    printf("Created commit %s with %zu tracked file(s)\n", commit_sha, count);

    free(meta);
    free(content);
    free_entries(entries, count);
    return 0;
}

static int compare_prefix(const char *candidate, const char *prefix) {
    return strncmp(candidate, prefix, strlen(prefix)) == 0;
}

static struct file_entry *load_index_entries(size_t *count_out) {
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return NULL;

    struct file_entry *entries = NULL;
    size_t count = 0;
    size_t cap = 0;

    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char sha[65];
        char rel[PATH_MAX];
        if (sscanf(line, "%64s %4095[^\n]", sha, rel) != 2) continue;
        if (append_entry(&entries, &count, &cap, rel, sha) != 0) {
            fclose(f);
            free_entries(entries, count);
            return NULL;
        }
    }

    fclose(f);
    *count_out = count;
    return entries;
}

static int find_entry_by_path(const struct file_entry *entries, size_t count, const char *path) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].path, path) == 0) return (int)i;
    }
    return -1;
}

static int walk_status_collect(const char *root, const char *cur, struct file_entry **entries, size_t *count, size_t *cap) {
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
            if (walk_status_collect(root, full, entries, count, cap) != 0) {
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
            continue;
        }

        if (append_entry(entries, count, cap, rel, sha) != 0) {
            fprintf(stderr, "warning: failed to record status entry for %s\n", full);
            closedir(d);
            return -1;
        }
    }

    closedir(d);
    return 0;
}

int vcs_log(void) {
    if (vcs_init_repo() != 0) return -1;

    DIR *d = opendir(COMMIT_DIR);
    if (!d) return -1;

    struct commit_record {
        char *sha;
        long ts;
        char *msg;
    };

    struct commit_record *records = NULL;
    size_t count = 0;
    size_t cap = 0;

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
        long ts = 0;
        int ts_set = 0;
        char *msg = NULL;
        size_t msg_len = 0;
        size_t msg_cap = 0;
        int in_message = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "timestamp:", 10) == 0) {
                ts = strtol(line + 10, NULL, 10);
                ts_set = 1;
            } else if (strncmp(line, "message:", 8) == 0) {
                const char *p = line + 8;
                while (*p == ' ' || *p == '\t') p++;
                size_t n = strlen(p);
                if (append_text(&msg, &msg_len, &msg_cap, p, n) != 0) {
                    free(msg);
                    fclose(f);
                    closedir(d);
                    return -1;
                }
                in_message = 1;
            } else if (in_message && (line[0] == '\n' || line[0] == '\r')) {
                break;
            } else if (in_message) {
                if (append_text(&msg, &msg_len, &msg_cap, line, strlen(line)) != 0) {
                    free(msg);
                    fclose(f);
                    closedir(d);
                    return -1;
                }
            } else if (line[0] == '\n' || line[0] == '\r') {
                break;
            }
        }
        fclose(f);

        if (!msg) {
            msg = strdup("(no message)");
            if (!msg) {
                free(records);
                closedir(d);
                return -1;
            }
        } else {
            size_t len = strlen(msg);
            while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) {
                msg[--len] = '\0';
            }
            collapse_newlines(msg);
        }

        if (count == cap) {
            size_t ncap = cap == 0 ? 16 : cap * 2;
            struct commit_record *tmp = realloc(records, ncap * sizeof(*records));
            if (!tmp) {
                free(msg);
                free(records);
                closedir(d);
                return -1;
            }
            records = tmp;
            cap = ncap;
        }

        records[count].sha = strdup(ent->d_name);
        records[count].ts = ts_set ? ts : 0;
        records[count].msg = msg;
        if (!records[count].sha) {
            free(msg);
            free(records);
            closedir(d);
            return -1;
        }
        count++;
    }

    closedir(d);

    if (count > 1) {
        for (size_t i = 0; i + 1 < count; ++i) {
            for (size_t j = i + 1; j < count; ++j) {
                if (records[i].ts < records[j].ts ||
                    (records[i].ts == records[j].ts && strcmp(records[i].sha, records[j].sha) > 0)) {
                    struct commit_record tmp = records[i];
                    records[i] = records[j];
                    records[j] = tmp;
                }
            }
        }
    }

    for (size_t i = 0; i < count; ++i) {
        printf("%s | %ld | %s\n", records[i].sha, records[i].ts, records[i].msg ? records[i].msg : "(no message)");
        free(records[i].sha);
        free(records[i].msg);
    }
    free(records);
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

    size_t index_count = 0;
    struct file_entry *index_entries = load_index_entries(&index_count);

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        free_entries(index_entries, index_count);
        return -1;
    }

    struct file_entry *work_entries = NULL;
    size_t work_count = 0;
    size_t work_cap = 0;
    if (walk_status_collect(cwd, cwd, &work_entries, &work_count, &work_cap) != 0) {
        free_entries(index_entries, index_count);
        free_entries(work_entries, work_count);
        return -1;
    }

    size_t modified = 0;
    size_t deleted = 0;
    size_t untracked = 0;

    printf("repository:\n");
    struct stat st;
    printf(" - .zvcs: %s\n", stat(ZVCS_DIR, &st) == 0 ? "present" : "missing");
    printf(" - objects: %s\n", stat(OBJ_DIR, &st) == 0 ? "present" : "missing");
    printf(" - commits: %s\n", stat(COMMIT_DIR, &st) == 0 ? "present" : "missing");
    printf(" - index: %s\n", stat(INDEX_FILE, &st) == 0 ? "present" : "missing");
    printf("\n");

    printf("tracked changes:\n");
    for (size_t i = 0; i < index_count; ++i) {
        int idx = find_entry_by_path(work_entries, work_count, index_entries[i].path);
        if (idx < 0) {
            printf(" - deleted: %s\n", index_entries[i].path);
            deleted++;
            continue;
        }
        if (strcmp(index_entries[i].sha, work_entries[idx].sha) != 0) {
            printf(" - modified: %s\n", index_entries[i].path);
            modified++;
        }
    }

    printf("untracked files:\n");
    for (size_t i = 0; i < work_count; ++i) {
        if (find_entry_by_path(index_entries, index_count, work_entries[i].path) < 0) {
            printf(" - %s\n", work_entries[i].path);
            untracked++;
        }
    }

    if (modified == 0 && deleted == 0 && untracked == 0) {
        printf("working tree clean\n");
    } else {
        printf("summary: modified=%zu deleted=%zu untracked=%zu\n", modified, deleted, untracked);
    }

    free_entries(index_entries, index_count);
    free_entries(work_entries, work_count);
    return 0;
}
