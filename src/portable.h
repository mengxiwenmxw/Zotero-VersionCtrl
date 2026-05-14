#ifndef PORTABLE_H
#define PORTABLE_H

#include <stddef.h>
#include <sys/stat.h>

// Get executable directory (not including trailing slash). buf must be PATH_MAX long.
int portable_get_exe_dir(char *buf, size_t buflen);

// mkdtemp wrapper: template should contain XXXXXX, returns pointer to buffer on success, NULL on failure
char *portable_mkdtemp(char *template);

// mkdir wrapper that works on Windows/Posix
int portable_mkdir(const char *path, mode_t mode);

// Safely join base and name into out (with '/' separator). Returns 0 on success.
int portable_join_path(char *out, size_t outlen, const char *base, const char *name);

// portable rename/unlink wrappers if needed (currently use standard ones)

#endif // PORTABLE_H
