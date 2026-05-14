#include <stdio.h>
#include "vcs.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include "fileutil.h"
#include "portable.h"

static const char *ZVCS_DIR = ".zvcs";
static const char *OBJ_DIR = ".zvcs/objects";
static const char *COMMIT_DIR = ".zvcs/commits";

static int ensure_zvcs_dirs(void) {
    struct stat st;
    if (stat(ZVCS_DIR, &st) != 0) {
        if (portable_mkdir(ZVCS_DIR, 0755) != 0) return -1;
    }
    if (stat(OBJ_DIR, &st) != 0) {
        if (portable_mkdir(OBJ_DIR, 0755) != 0) return -1;
    }
    if (stat(COMMIT_DIR, &st) != 0) {
        if (portable_mkdir(COMMIT_DIR, 0755) != 0) return -1;
    }
    return 0;
}

int vcs_init_repo(void) {
    if (ensure_zvcs_dirs() != 0) {
        perror("init .zvcs");
        return -1;
    }
    return 0;
}

// write blob (file) into .zvcs/objects/<sha> if not exists
static int store_blob_if_missing(const char *path, const char *sha) {
    char dst[4096];
    snprintf(dst, sizeof(dst), "%s/%s", OBJ_DIR, sha);
    if (access(dst, F_OK) == 0) return 0; // already stored
    if (ensure_parent_dir(dst) != 0) return -1;
    return copy_file_atomic(path, dst);
}

// create a commit: traverse cwd, skip .zvcs, compute sha for each file,
// store blobs, write commit file with lines: <sha> <path>
int vcs_commit(const char *message) {
    if (vcs_init_repo() != 0) return -1;
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return -1; }

    // load .zvcsignore from cwd (optional)
    char **ignore_list = NULL;
    size_t ignore_count = 0;
    {
        char ignpath[4096];
        if (portable_join_path(ignpath, sizeof(ignpath), cwd, ".zvcsignore") != 0) {
            fprintf(stderr, "path too long for .zvcsignore\n");
            return -1;
        }
        FILE *ifp = fopen(ignpath, "r");
        if (ifp) {
            char buf[1024];
            while (fgets(buf, sizeof(buf), ifp)) {
                // trim whitespace
                char *p = buf;
                while (*p && (*p == ' ' || *p == '\t')) p++;
                char *end = p + strlen(p) - 1;
                while (end >= p && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) { *end = '\0'; end--; }
                if (*p == '\0') continue;
                char *s = strdup(p);
                if (!s) break;
                char **n = realloc(ignore_list, sizeof(char*) * (ignore_count + 1));
                if (!n) { free(s); break; }
                ignore_list = n;
                ignore_list[ignore_count++] = s;
            }
            fclose(ifp);
        }
    }

    // determine executable basename to ignore it
    char exe_name[4096] = {0};
#ifndef _WIN32
    {
        char exe_path[4096];
        ssize_t l = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
        if (l != -1) { exe_path[l] = '\0'; char *b = strrchr(exe_path, '/'); if (b) strcpy(exe_name, b+1); else strcpy(exe_name, exe_path); }
    }
#endif

    // temporary buffer to build commit content for computing commit id
    char *commit_buf = NULL;
    size_t commit_buf_len = 0;

    // recursive walker
    int is_ignored(const char *rel) {
        if (!rel) return 0;
        if (strcmp(rel, "peers.conf") == 0) return 1;
        if (exe_name[0] && strcmp(rel, exe_name) == 0) return 1;
        for (size_t i = 0; i < ignore_count; ++i) {
            const char *pat = ignore_list[i];
            if (pat[0] == '\0') continue;
            // simple rules: exact match or prefix match for directories
            if (strcmp(rel, pat) == 0) return 1;
            size_t plen = strlen(pat);
            if (strncmp(rel, pat, plen) == 0) {
                // match prefix (directory or file)
                return 1;
            }
        }
        return 0;
    }

    int walk(const char *curpath) {
        DIR *d = opendir(curpath);
        if (!d) return -1;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".zvcs") == 0) continue;
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", curpath, ent->d_name);
            struct stat st;
            if (stat(full, &st) != 0) { closedir(d); return -1; }
            if (S_ISDIR(st.st_mode)) {
                if (walk(full) != 0) { closedir(d); return -1; }
            } else if (S_ISREG(st.st_mode)) {
                char sha[65];
                if (compute_sha256_hex(full, sha) != 0) { closedir(d); return -1; }
                if (store_blob_if_missing(full, sha) != 0) { closedir(d); return -1; }
                // compute relative path
                const char *rel = full + strlen(cwd);
                if (*rel == '/') rel++;
                if (is_ignored(rel)) continue;
                // append line to commit_buf
                size_t need = strlen(sha) + 1 + strlen(rel) + 1;
                char *nb = realloc(commit_buf, commit_buf_len + need + 1);
                if (!nb) { free(commit_buf); closedir(d); return -1; }
                commit_buf = nb;
                sprintf(commit_buf + commit_buf_len, "%s %s\n", sha, rel);
                commit_buf_len += need;
            }
        }
        closedir(d);
        return 0;
    }

    if (walk(cwd) != 0) { free(commit_buf); return -1; }

    // build commit content including message and timestamp
    time_t now = time(NULL);
    char meta[512];
    snprintf(meta, sizeof(meta), "timestamp: %ld\nmessage: %s\n\n", (long)now, message?message:"(no message)");

    // final content
    size_t final_len = strlen(meta) + commit_buf_len;
    char *final = malloc(final_len + 1);
    if (!final) { free(commit_buf); return -1; }
    strcpy(final, meta);
    if (commit_buf) strcat(final, commit_buf);

    // compute commit id as sha256 of final
    char commit_sha[65];
    // write final to a temporary file to compute sha via existing function
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "/tmp/zvcs_commit_%ld.tmp", (long)getpid());
    FILE *tf = fopen(tmp, "wb");
    if (!tf) { free(final); free(commit_buf); return -1; }
    fwrite(final, 1, strlen(final), tf);
    fclose(tf);
    if (compute_sha256_hex(tmp, commit_sha) != 0) { unlink(tmp); free(final); free(commit_buf); return -1; }
    unlink(tmp);

    // store commit file
    char commitfile[4096];
    snprintf(commitfile, sizeof(commitfile), "%s/%s.commit", COMMIT_DIR, commit_sha);
    FILE *cf = fopen(commitfile, "wb");
    if (!cf) { free(final); free(commit_buf); return -1; }
    fwrite(final, 1, strlen(final), cf);
    fclose(cf);

    printf("Created commit %s\n", commit_sha);

    free(final);
    free(commit_buf);
    if (ignore_list) {
        for (size_t i = 0; i < ignore_count; ++i) free(ignore_list[i]);
        free(ignore_list);
    }
    return 0;
}

int vcs_log(void) {
    if (vcs_init_repo() != 0) return -1;
    DIR *d = opendir(COMMIT_DIR);
    if (!d) { perror("open commits"); return -1; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        printf("%s\n", ent->d_name);
    }
    closedir(d);
    return 0;
}

int vcs_checkout(const char *commit_prefix) {
    if (vcs_init_repo() != 0) return -1;

    DIR *d = opendir(COMMIT_DIR);
    if (!d) { perror("open commits"); return -1; }
    struct dirent *ent;
    char match[4096] = {0};
    int found = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strncmp(ent->d_name, commit_prefix, strlen(commit_prefix)) == 0) {
            strcpy(match, ent->d_name);
            found = 1; break;
        }
    }
    closedir(d);
    if (!found) { fprintf(stderr, "commit %s not found\n", commit_prefix); return -1; }

    // open commit file
    char commitfile[4096];
    snprintf(commitfile, sizeof(commitfile), "%s/%s", COMMIT_DIR, match);
    FILE *cf = fopen(commitfile, "r");
    if (!cf) { perror("open commitfile"); return -1; }

    // skip meta lines until empty line
    char line[8192];
    while (fgets(line, sizeof(line), cf)) {
        if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) break;
    }

    // create temporary restore directory inside cwd so renames are atomic
    char tmpdir_template[4096];
    snprintf(tmpdir_template, sizeof(tmpdir_template), "%s/tmp_restore_XXXXXX", COMMIT_DIR);
    char *tmpdir = portable_mkdtemp(tmpdir_template);
    if (!tmpdir) { perror("mkdtemp"); fclose(cf); return -1; }

    // helpers: list of relative paths copied into tmpdir
    char **rel_paths = NULL;
    size_t rel_count = 0;
    // list of conflicts
    char **conflicts = NULL;
    size_t conflict_count = 0;

    // process each file: copy blob into tmpdir/<relpath>, detect conflicts
    while (fgets(line, sizeof(line), cf)) {
        // trim newline
        char *end = line + strlen(line) - 1;
        while (end >= line && (*end == '\n' || *end == '\r')) { *end = '\0'; end--; }
        if (line[0] == '\0') continue;
        char sha[65], path[4096];
        if (sscanf(line, "%64s %4095[^\n]", sha, path) < 2) continue;
        char src[4096];
        snprintf(src, sizeof(src), "%s/%s", OBJ_DIR, sha);

        // prepare temp destination path
        char tmpdst[8192];
        snprintf(tmpdst, sizeof(tmpdst), "%s/%s", tmpdir, path);
        // ensure parent dir exists under tmpdir
        if (ensure_parent_dir(tmpdst) != 0) {
            fprintf(stderr, "failed to make parent dir for %s\n", tmpdst);
            // cleanup
            fclose(cf);
            // TODO: remove tmpdir recursively
            return -1;
        }

        if (copy_file_atomic(src, tmpdst) != 0) {
            fprintf(stderr, "failed to copy blob %s to tmp %s\n", src, tmpdst);
            fclose(cf);
            return -1;
        }

        // conflict detection: if destination exists and its hash != sha -> conflict
        int is_conflict = 0;
        if (access(path, F_OK) == 0) {
            char dst_hash[65];
            if (compute_sha256_hex(path, dst_hash) == 0) {
                if (strcmp(dst_hash, sha) != 0) is_conflict = 1;
            }
        }
        if (is_conflict) {
            char *c = strdup(path);
            if (c) {
                char **n = realloc(conflicts, sizeof(char*) * (conflict_count + 1));
                if (n) { conflicts = n; conflicts[conflict_count++] = c; }
                else free(c);
            }
        }

        // record rel path for later apply
        char *rp = strdup(path);
        if (rp) {
            char **n = realloc(rel_paths, sizeof(char*) * (rel_count + 1));
            if (n) { rel_paths = n; rel_paths[rel_count++] = rp; }
            else free(rp);
        }
    }
    fclose(cf);

    if (conflict_count > 0) {
        fprintf(stderr, "Found %zu conflicts, aborting checkout:\n", conflict_count);
        for (size_t i = 0; i < conflict_count; ++i) fprintf(stderr, " - %s\n", conflicts[i]);
        // cleanup tmpdir and lists
        for (size_t i = 0; i < rel_count; ++i) {
            free(rel_paths[i]);
        }
        free(rel_paths);
        for (size_t i = 0; i < conflict_count; ++i) {
            free(conflicts[i]);
        }
        free(conflicts);
        // remove tmpdir recursively
        // simple recursive remove
        int remove_dir_recursive(const char *path) {
            DIR *d = opendir(path);
            if (!d) return -1;
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                char child[8192];
                snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
                struct stat st;
                if (stat(child, &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) remove_dir_recursive(child);
                else unlink(child);
            }
            closedir(d);
            rmdir(path);
            return 0;
        }
        remove_dir_recursive(tmpdir);
        return -1;
    }

    // apply files: move from tmpdir/<rel> to ./<rel> atomically
    for (size_t i = 0; i < rel_count; ++i) {
        char src_tmp[8192];
        char dst[8192];
        snprintf(src_tmp, sizeof(src_tmp), "%s/%s", tmpdir, rel_paths[i]);
        snprintf(dst, sizeof(dst), "%s", rel_paths[i]);
        // ensure parent dir for dst
        if (ensure_parent_dir(dst) != 0) {
            fprintf(stderr, "failed to ensure parent for %s\n", dst);
            // continue to next
            continue;
        }
        // attempt atomic rename
        if (rename(src_tmp, dst) != 0) {
            // fallback: copy atomic from src_tmp to dst
            if (copy_file_atomic(src_tmp, dst) != 0) {
                fprintf(stderr, "failed to move %s to %s\n", src_tmp, dst);
            }
        }
        printf("restored %s\n", dst);
        free(rel_paths[i]);
    }
    free(rel_paths);

    // remove tmpdir recursively
    int remove_dir_recursive(const char *path) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char child[8192];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            struct stat st;
            if (stat(child, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) remove_dir_recursive(child);
            else unlink(child);
        }
        closedir(d);
        rmdir(path);
        return 0;
    }
    remove_dir_recursive(tmpdir);

    return 0;
}
