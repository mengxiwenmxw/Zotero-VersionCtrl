#include "portable.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/types.h>
#endif

int portable_get_exe_dir(char *buf, size_t buflen) {
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return -1;
    char *b = strrchr(path, '\\');
    if (b) *b = '\0';
    strncpy(buf, path, buflen-1); buf[buflen-1] = '\0';
    return 0;
#else
    ssize_t l = readlink("/proc/self/exe", buf, buflen-1);
    if (l == -1) return -1;
    buf[l] = '\0';
    char *slash = strrchr(buf, '/');
    if (slash) *slash = '\0';
    return 0;
#endif
}

char *portable_mkdtemp(char *template) {
#ifdef _WIN32
    // _mktemp modifies template to a unique name; then create dir
    if (_mktemp(template) == NULL) return NULL;
    if (_mkdir(template) != 0) return NULL;
    return template;
#else
    return mkdtemp(template);
#endif
}

int portable_mkdir(const char *path, mode_t mode) {
#ifdef _WIN32
    (void)mode;
    if (_mkdir(path) != 0) return -1;
    return 0;
#else
    return mkdir(path, mode);
#endif
}

int portable_join_path(char *out, size_t outlen, const char *base, const char *name) {
    if (!out || !base || !name || outlen == 0) return -1;

    size_t blen = strlen(base);
    size_t nlen = strlen(name);

    while (blen > 0 && (base[blen - 1] == '/' || base[blen - 1] == '\\')) {
        blen--;
    }

    if (blen == 0) {
        if (nlen + 1 > outlen) return -1;
        memcpy(out, name, nlen + 1);
        return 0;
    }

    if (blen + 1 + nlen + 1 > outlen) return -1;

    memcpy(out, base, blen);
    out[blen] = '/';
    memcpy(out + blen + 1, name, nlen + 1);
    return 0;
}

int portable_replace_file(const char *src, const char *dst) {
#ifdef _WIN32
    if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return 0;
    }
    return -1;
#else
    return rename(src, dst);
#endif
}

int portable_color_enabled(void) {
    static int cached = -1;
    if (cached != -1) return cached;

    if (getenv("NO_COLOR") != NULL) {
        cached = 0;
        return cached;
    }

#ifdef _WIN32
    DWORD mode = 0;
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == NULL) {
        cached = 0;
        return cached;
    }
    if (!GetConsoleMode(handle, &mode)) {
        cached = 0;
        return cached;
    }
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
        if (!SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            cached = 0;
            return cached;
        }
    }
    cached = 1;
    return cached;
#else
    cached = isatty(STDOUT_FILENO) ? 1 : 0;
    return cached;
#endif
}
