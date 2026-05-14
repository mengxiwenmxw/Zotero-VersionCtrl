#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "device.h"
#include "sync.h"
#include "vcs.h"

static char *join_message(int argc, char **argv, int start_index) {
    if (start_index >= argc) return NULL;

    size_t len = 0;
    for (int i = start_index; i < argc; ++i) {
        len += strlen(argv[i]);
        if (i + 1 < argc) len++;
    }

    char *msg = malloc(len + 1);
    if (!msg) return NULL;

    size_t off = 0;
    for (int i = start_index; i < argc; ++i) {
        size_t part_len = strlen(argv[i]);
        memcpy(msg + off, argv[i], part_len);
        off += part_len;
        if (i + 1 < argc) {
            msg[off++] = ' ';
        }
    }
    msg[off] = '\0';
    return msg;
}

static void print_help(void) {
    printf("zvcs - Zotero Version Control Sync\n");
    printf("\n");
    printf("Usage:\n");
    printf("  zvcs help\n");
    printf("  zvcs init\n");
    printf("  zvcs status\n");
    printf("  zvcs check\n");
    printf("  zvcs fetch\n");
    printf("  zvcs commit [message...]\n");
    printf("  zvcs log\n");
    printf("  zvcs checkout <commit-prefix>\n");
    printf("\n");
    printf("Commands:\n");
    printf("  help        Show this help text.\n");
    printf("  init        Create the local .zvcs repository structure.\n");
    printf("  status      Show local repo status and known devices.\n");
    printf("  check       Validate peer directories listed in peers.conf.\n");
    printf("  fetch       Pull missing or changed files from peer paths.\n");
    printf("  commit      Snapshot the current tree into .zvcs/objects and store a message.\n");
    printf("  log         List local commits.\n");
    printf("  checkout    Restore files from a commit prefix.\n");
    printf("\n");
    printf("Notes:\n");
    printf("  - peers.conf is read from the executable directory first, then cwd.\n");
    printf("  - .zvcsignore supports one path prefix per line.\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    if (vcs_init_repo() != 0) {
        fprintf(stderr, "failed to initialize repository\n");
        return 1;
    }

    if (strcmp(argv[1], "help") == 0) {
        print_help();
    } else if (strcmp(argv[1], "init") == 0) {
        puts(".zvcs initialized");
    } else if (strcmp(argv[1], "status") == 0) {
        if (vcs_status() != 0) return 1;
        device_print_status();
    } else if (strcmp(argv[1], "check") == 0) {
        if (sync_check_peers() != 0) {
            fprintf(stderr, "check failed\n");
            return 1;
        }
    } else if (strcmp(argv[1], "fetch") == 0) {
        if (device_discover_peers() != 0) {
            fprintf(stderr, "peer discovery failed\n");
            return 1;
        }
        if (sync_fetch_missing() != 0) {
            fprintf(stderr, "fetch failed\n");
            return 1;
        }
    } else if (strcmp(argv[1], "commit") == 0) {
        char *msg = join_message(argc, argv, 2);
        if (argc >= 3 && !msg) {
            fprintf(stderr, "failed to build commit message\n");
            return 1;
        }
        int rc = vcs_commit(msg);
        free(msg);
        if (rc != 0) {
            fprintf(stderr, "commit failed\n");
            return 1;
        }
    } else if (strcmp(argv[1], "log") == 0) {
        if (vcs_log() != 0) return 1;
    } else if (strcmp(argv[1], "checkout") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: zvcs checkout <commit-prefix>\n"); return 1; }
        if (vcs_checkout(argv[2]) != 0) return 1;
    } else {
        print_help();
    }
    return 0;
}
