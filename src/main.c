#include <stdio.h>
#include <string.h>
#include "device.h"
#include "sync.h"
#include "vcs.h"

void print_help(void) {
    printf("zvcs - zotero sync tool\n");
    printf("Usage:\n");
    printf("  zvcs help             Show this help\n");
    printf("  zvcs status           Show status of devices and repo\n");
    printf("  zvcs check            Check peer directory accessibility\n");
    printf("  zvcs fetch            Fetch missing files from peers\n");
    printf("  zvcs commit           Commit current changes to local VCS\n");
    printf("  zvcs log              Show commit list\n");
    printf("  zvcs checkout <id>    Restore files from commit\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    vcs_init_repo();

    if (strcmp(argv[1], "help") == 0) {
        print_help();
    } else if (strcmp(argv[1], "status") == 0) {
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
        const char *msg = NULL;
        if (argc >= 3) msg = argv[2];
        if (vcs_commit(msg) != 0) {
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
