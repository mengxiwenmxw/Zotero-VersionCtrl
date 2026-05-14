#ifndef SHA256_H
#define SHA256_H

// Compute SHA256 hex string of a file. out_hex must be at least 65 bytes.
int sha256_file_hex(const char *path, char out_hex[65]);

#endif // SHA256_H
