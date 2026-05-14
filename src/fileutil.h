#ifndef FILEUTIL_H
#define FILEUTIL_H

#include <sys/types.h>

int compute_sha256_hex(const char *path, char out_hex[65]);
int ensure_parent_dir(const char *path);
int copy_file_atomic(const char *src, const char *dst);
int sync_from_peer(const char *peer_root, const char *local_root);

#endif // FILEUTIL_H
