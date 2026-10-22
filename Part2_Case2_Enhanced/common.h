#ifndef IT305_CASE2_ENHANCED_COMMON_H
#define IT305_CASE2_ENHANCED_COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <limits.h>

#define PROTO_MAGIC          0x5446 /* "TF" in ASCII */
#define PROTO_VERSION        2
#define CHUNK_SIZE           65536  /* 64 KB block size */
#define MAX_PAYLOAD          (CHUNK_SIZE + 64)
#define TOPIC_FIELD          64
#define MAX_NAME             256
#define SHA256_DIGEST_LEN    32
#define SESSION_ID_LEN       32
#define DEFAULT_STREAMS      4
#define MAX_STREAMS          16

/* Protocol Message Types */
#define MSG_ENHANCED_SESSION_REQ   0x20
#define MSG_ENHANCED_SESSION_RESP  0x21
#define MSG_STREAM_ATTACH          0x22
#define MSG_STREAM_ATTACH_ACK      0x23
#define MSG_PULL_WORK              0x24
#define MSG_NO_MORE_WORK           0x25
#define MSG_MANIFEST_START         0x03
#define MSG_MANIFEST_ENTRY         0x04
#define MSG_MANIFEST_END           0x05
#define MSG_FILE_HEADER            0x06
#define MSG_DATA_CHUNK             0x07
#define MSG_CHECKPOINT_ACK         0x13
#define MSG_CHECKPOINT_COMMIT      0x14
#define MSG_TRANSFER_DONE          0x08
#define MSG_ERROR                  0xFF

/* Header Flags */
#define FLAG_EOF_FILE              0x0001
#define FLAG_RESUME                0x0002

/* Protocol Error Codes */
#define ERR_OK                     0
#define ERR_TOPIC_NOT_FOUND        1
#define ERR_FILE_NOT_FOUND         2
#define ERR_CHECKSUM_MISMATCH      3
#define ERR_PROTOCOL               4
#define ERR_SESSION_NOT_FOUND      5
#define ERR_BAD_REQUEST            6
#define ERR_INTERNAL               7

#pragma pack(push, 1)
typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint16_t status;
    uint32_t len;
    uint32_t seq;
} proto_hdr_t;
#pragma pack(pop)

/* Connection wrapper with byte accounting */
typedef struct {
    int         fd;
    uint32_t    tx_seq;
    uint32_t    rx_seq;
    uint64_t    tx_bytes;
    uint64_t    rx_bytes;
    const char *err;
} conn_t;

/* Topic File Catalog Structure */
typedef struct {
    char     name[MAX_NAME];
    uint64_t size;
    uint8_t  hash[SHA256_DIGEST_LEN];
} fentry_t;

typedef struct {
    char      name[TOPIC_FIELD];
    char      dir[PATH_MAX];
    size_t    nfiles;
    fentry_t *files;
    uint64_t  total_bytes;
    uint8_t   manifest_hash[SHA256_DIGEST_LEN];
} topic_t;

/* Portable self-contained SHA-256 */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
} sha256_ctx_t;

void     sha256_init(sha256_ctx_t *ctx);
void     sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
void     sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_LEN]);
int      sha256_file(const char *path, uint8_t digest[SHA256_DIGEST_LEN], uint64_t *out_size);
void     sha256_to_hex(const uint8_t digest[SHA256_DIGEST_LEN], char hex[65]);

/* Networking & Framing Primitives */
void     conn_init(conn_t *c, int fd);
int      conn_send(conn_t *c, uint8_t type, uint16_t flags, uint16_t status,
                   const void *payload, uint32_t len, int do_cork);
int      conn_recv(conn_t *c, proto_hdr_t *out_h, void *out_payload);

/* Filesystem & Utility Helpers */
int      safe_name(const char *name);
int      mkdir_p(const char *path);
int      rm_rf(const char *path);
ssize_t  write_full(int fd, const void *buf, size_t count, const char *path);
ssize_t  read_full(int fd, void *buf, size_t count);
double   now_sec(void);
void     sanitize_trailing_slashes(char *path);

static inline void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline uint16_t get16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static inline uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}
static inline uint64_t get64(const uint8_t *p) {
    return ((uint64_t)get32(p) << 32) | (uint64_t)get32(p + 4);
}

#endif /* IT305_CASE2_ENHANCED_COMMON_H */
