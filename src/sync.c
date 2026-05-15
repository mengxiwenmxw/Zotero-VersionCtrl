#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>
#include "sync.h"
#include "fileutil.h"
#include "portable.h"

static int sync_color_enabled(void) {
    return portable_color_enabled();
}

static const char *sync_cc(const char *code) {
    return sync_color_enabled() ? code : "";
}

static int resolve_peers_conf_path(char *confpath, size_t confpath_len) {
    char exe_dir[PATH_MAX];
    if (portable_get_exe_dir(exe_dir, sizeof(exe_dir)) == 0) {
        if (portable_join_path(confpath, confpath_len, exe_dir, "peers.conf") != 0) {
            fprintf(stderr, "path too long for peers.conf\n");
            return -1;
        }
    } else {
        // fallback to current working directory
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) {
            perror("getcwd");
            return -1;
        }
        if (portable_join_path(confpath, confpath_len, cwd, "peers.conf") != 0) {
            fprintf(stderr, "path too long for peers.conf\n");
            return -1;
        }
    }
    return 0;
}

static char *trim_line(char *line) {
    char *p = line;
    while (*p && (*p == ' ' || *p == '\t')) p++;
    char *end = p + strlen(p);
    while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
    return p;
}

static int read_peers_conf(const char *confpath, int (*cb)(const char *peer, void *ctx), void *ctx) {
    FILE *f = fopen(confpath, "r");
    if (!f) return -1;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim_line(line);
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;
        if (cb(p, ctx) != 0) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

struct fetch_ctx {
    char cwd[PATH_MAX];
};

static int fetch_peer_cb(const char *peer, void *opaque) {
    struct fetch_ctx *ctx = opaque;
    if (peer[0] == '\0') return 0;

    printf("%ssyncing from peer:%s %s\n", sync_cc("\033[36m"), sync_cc("\033[0m"), peer);
    if (sync_from_peer(peer, ctx->cwd) != 0) {
        fprintf(stderr, "%ssync from%s %s %sfailed%s\n", sync_cc("\033[31m"), sync_cc("\033[0m"), peer, "", sync_cc("\033[0m"));
    } else {
        printf("%ssync from%s %s %scompleted%s\n", sync_cc("\033[32m"), sync_cc("\033[0m"), peer, "", sync_cc("\033[0m"));
    }
    return 0;
}

struct check_ctx {
    int ok_count;
    int fail_count;
    int total_count;
};

static int check_peer_cb(const char *peer, void *opaque) {
    struct check_ctx *ctx = opaque;
    ctx->total_count++;

    struct stat st;
    if (stat(peer, &st) != 0) {
        fprintf(stderr, "%s[FAIL]%s %s -> %s\n", sync_cc("\033[31m"), sync_cc("\033[0m"), peer, strerror(errno));
        ctx->fail_count++;
        return 0;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s[FAIL]%s %s -> not a directory\n", sync_cc("\033[31m"), sync_cc("\033[0m"), peer);
        ctx->fail_count++;
        return 0;
    }

    DIR *d = opendir(peer);
    if (!d) {
        fprintf(stderr, "%s[FAIL]%s %s -> cannot open: %s\n", sync_cc("\033[31m"), sync_cc("\033[0m"), peer, strerror(errno));
        ctx->fail_count++;
        return 0;
    }
    closedir(d);
    printf("%s[ OK ]%s %s\n", sync_cc("\033[32m"), sync_cc("\033[0m"), peer);
    ctx->ok_count++;
    return 0;
}

// Read peers from peers.conf located next to the executable (same directory).
// For each peer path, sync files under that path into current working directory.
int sync_fetch_missing(void) {
    char confpath[PATH_MAX];
    if (resolve_peers_conf_path(confpath, sizeof(confpath)) != 0) {
        return 1;
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("getcwd");
        return 1;
    }

    struct fetch_ctx ctx;
    snprintf(ctx.cwd, sizeof(ctx.cwd), "%s", cwd);
    if (read_peers_conf(confpath, fetch_peer_cb, &ctx) != 0) {
        fprintf(stderr, "%scould not open%s %s (create one path per line)\n", sync_cc("\033[31m"), sync_cc("\033[0m"), confpath);
        return 1;
    }
    return 0;
}

int sync_check_peers(void) {
    char confpath[PATH_MAX];
    if (resolve_peers_conf_path(confpath, sizeof(confpath)) != 0) {
        return 1;
    }

    struct check_ctx ctx = {0, 0, 0};
    if (read_peers_conf(confpath, check_peer_cb, &ctx) != 0) {
        fprintf(stderr, "%scould not open%s %s (create one path per line)\n", sync_cc("\033[31m"), sync_cc("\033[0m"), confpath);
        return 1;
    }
    if (ctx.total_count == 0) {
        fprintf(stderr, "%sNo peer paths found%s in %s\n", sync_cc("\033[33m"), sync_cc("\033[0m"), confpath);
        return 1;
    }

    printf("%scheck summary:%s total=%d ok=%d fail=%d\n", sync_cc("\033[36m"), sync_cc("\033[0m"), ctx.total_count, ctx.ok_count, ctx.fail_count);
    return (ctx.fail_count == 0) ? 0 : 1;
}
