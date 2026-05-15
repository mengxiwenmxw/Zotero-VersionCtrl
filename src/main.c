#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <stdbool.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include "device.h"
#include "sync.h"
#include "portable.h"
#include "vcs.h"

static int color_enabled(void) {
    return portable_color_enabled();
}

static const char *cc(const char *code) {
    return color_enabled() ? code : "";
}

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

static const char *pick_default_editor(void) {
    const char *editor = getenv("VISUAL");
    if (editor && editor[0] != '\0') return editor;

    editor = getenv("EDITOR");
    if (editor && editor[0] != '\0') return editor;

#ifdef _WIN32
    return "notepad";
#else
    return "vi";
#endif
}

static int launch_editor(const char *path) {
    const char *editor = pick_default_editor();
    char cmd[PATH_MAX * 2 + 64];

    if (snprintf(cmd, sizeof(cmd), "%s \"%s\"", editor, path) >= (int)sizeof(cmd)) {
        return -1;
    }

    int rc = system(cmd);
    if (rc == -1) return -1;
#ifndef _WIN32
    if (!WIFEXITED(rc) || WEXITSTATUS(rc) != 0) return -1;
#else
    if (rc != 0) return -1;
#endif
    return 0;
}

struct message_line {
    char *text;
    size_t len;
    int blank;
};

static void free_message_lines(struct message_line *lines, size_t count) {
    if (!lines) return;
    for (size_t i = 0; i < count; ++i) free(lines[i].text);
    free(lines);
}

static char *load_editor_message(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[nread] = '\0';

    struct message_line *lines = NULL;
    size_t count = 0;
    size_t cap = 0;

    char *cursor = buf;
    while (*cursor != '\0') {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor += strlen(cursor);
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#') continue;

        if (count == cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            struct message_line *tmp = realloc(lines, ncap * sizeof(*lines));
            if (!tmp) {
                free(lines);
                free(buf);
                return NULL;
            }
            lines = tmp;
            cap = ncap;
        }

        lines[count].text = strdup(line);
        if (!lines[count].text) {
            free_message_lines(lines, count);
            free(buf);
            return NULL;
        }
        lines[count].len = len;
        lines[count].blank = (len == 0);
        count++;
    }

    free(buf);

    size_t start = 0;
    while (start < count && lines[start].blank) start++;
    size_t end = count;
    while (end > start && lines[end - 1].blank) end--;
    if (start >= end) {
        free_message_lines(lines, count);
        return NULL;
    }

    size_t total = 0;
    for (size_t i = start; i < end; ++i) {
        total += lines[i].len;
        if (i + 1 < end) total++;
    }

    char *msg = malloc(total + 1);
    if (!msg) {
        free_message_lines(lines, count);
        return NULL;
    }

    size_t off = 0;
    for (size_t i = start; i < end; ++i) {
        memcpy(msg + off, lines[i].text, lines[i].len);
        off += lines[i].len;
        if (i + 1 < end) msg[off++] = '\n';
    }
    msg[off] = '\0';

    free_message_lines(lines, count);
    return msg;
}

static char *prompt_commit_message(void) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), ".zvcs/COMMIT_EDITMSG.%ld.%ld", (long)getpid(), (long)time(NULL));

    FILE *f = fopen(tmp, "wb");
    if (!f) return NULL;
    fputs("# Enter the commit message below. Lines starting with '#' will be ignored.\n", f);
    fputs("# Save and close the editor to continue.\n", f);
    fputs("\n", f);
    fclose(f);

    if (launch_editor(tmp) != 0) {
        unlink(tmp);
        return NULL;
    }

    char *msg = load_editor_message(tmp);
    unlink(tmp);
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
    printf("  zvcs commit [-m message]\n");
    printf("  zvcs log\n");
    printf("  zvcs checkout <commit-prefix>\n");
    printf("\n");
    printf("Commands:\n");
    printf("  %shelp%s        Show this help text.\n", cc("\033[36m"), cc("\033[0m"));
    printf("  %sinit%s        Create the local .zvcs repository structure.\n", cc("\033[36m"), cc("\033[0m"));
    printf("  %sstatus%s      Show local repo status and known devices.\n", cc("\033[36m"), cc("\033[0m"));
    printf("  %scheck%s       Validate peer directories listed in peers.conf.\n", cc("\033[36m"), cc("\033[0m"));
    printf("  %sfetch%s       Pull missing or changed files from peer paths.\n", cc("\033[36m"), cc("\033[0m"));
    printf("  %scommit%s      Snapshot the current tree into .zvcs/objects and store a message.\n", cc("\033[36m"), cc("\033[0m"));
    printf("  %slog%s         List local commits.\n", cc("\033[36m"), cc("\033[0m"));
    printf("  %scheckout%s    Restore files from a commit prefix.\n", cc("\033[36m"), cc("\033[0m"));
    printf("\n");
    printf("Notes:\n");
    printf("  - commit without -m opens the default editor for the message.\n");
    printf("  - peers.conf is read from the executable directory first, then cwd.\n");
    printf("  - .zvcsignore supports one path prefix per line.\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    if (vcs_init_repo() != 0) {
        fprintf(stderr, "%sfailed to initialize repository%s\n", cc("\033[31m"), cc("\033[0m"));
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
            fprintf(stderr, "%scheck failed%s\n", cc("\033[31m"), cc("\033[0m"));
            return 1;
        }
    } else if (strcmp(argv[1], "fetch") == 0) {
        if (device_discover_peers() != 0) {
            fprintf(stderr, "%speer discovery failed%s\n", cc("\033[31m"), cc("\033[0m"));
            return 1;
        }
        if (sync_fetch_missing() != 0) {
            fprintf(stderr, "%sfetch failed%s\n", cc("\033[31m"), cc("\033[0m"));
            return 1;
        }
    } else if (strcmp(argv[1], "commit") == 0) {
        char *msg = NULL;
        if (argc >= 3 && (strcmp(argv[2], "-m") == 0 || strcmp(argv[2], "--message") == 0)) {
            if (argc < 4) {
                fprintf(stderr, "%susage:%s zvcs commit -m <message>\n", cc("\033[33m"), cc("\033[0m"));
                return 1;
            }
            msg = join_message(argc, argv, 3);
            if (!msg) {
                fprintf(stderr, "%sfailed to build commit message%s\n", cc("\033[31m"), cc("\033[0m"));
                return 1;
            }
        } else if (argc >= 3 && argv[2][0] == '-') {
            fprintf(stderr, "%susage:%s zvcs commit [-m <message>]\n", cc("\033[33m"), cc("\033[0m"));
            return 1;
        } else if (argc >= 3) {
            msg = join_message(argc, argv, 2);
            if (!msg) {
                fprintf(stderr, "%sfailed to build commit message%s\n", cc("\033[31m"), cc("\033[0m"));
                return 1;
            }
        } else {
            msg = prompt_commit_message();
            if (!msg) {
                fprintf(stderr, "%sfailed to read commit message from editor%s\n", cc("\033[31m"), cc("\033[0m"));
                return 1;
            }
        }
        int rc = vcs_commit(msg);
        free(msg);
        if (rc != 0) {
            fprintf(stderr, "%scommit failed%s\n", cc("\033[31m"), cc("\033[0m"));
            return 1;
        }
    } else if (strcmp(argv[1], "log") == 0) {
        if (vcs_log() != 0) return 1;
    } else if (strcmp(argv[1], "checkout") == 0) {
        if (argc < 3) { fprintf(stderr, "%susage:%s zvcs checkout <commit-prefix>\n", cc("\033[33m"), cc("\033[0m")); return 1; }
        if (vcs_checkout(argv[2]) != 0) return 1;
    } else {
        print_help();
    }
    return 0;
}
