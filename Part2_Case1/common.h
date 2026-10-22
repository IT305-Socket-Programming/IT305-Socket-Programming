#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile sig_atomic_t g_stop;

/* Protocol Constants */
#define PROTO_MAGIC       0x5446u /* 'T', 'F' */
#define PROTO_VERSION     0x02u   /* Part II protocol version */
#define HDR_LEN           16      /* 16-byte fixed header */
#define CHUNK_SIZE        65536   /* 64 KB chunk size */
#define MAX_PAYLOAD       65536   /* Maximum allowed payload */
#define MAX_NAME          255     /* Maximum file name length */
#define TOPIC_FIELD       64      /* Maximum topic name field */
#define SHA256_DIGEST_LEN 32      /* 256 bits = 32 bytes */

/* Protocol Message Types */
#define MSG_GET_REQ         0x01
#define MSG_MANIFEST_START  0x02
#define MSG_MANIFEST_ENTRY  0x03
#define MSG_MANIFEST_END    0x04
#define MSG_FILE_HEADER     0x05
#define MSG_DATA_CHUNK      0x06
#define MSG_TRANSFER_DONE   0x07
#define MSG_ERROR           0x0F

/* Protocol Header Flags */
#define FLAG_EOF_FILE       0x0001u
#define FLAG_RETRY          0x0002u

/* Protocol Error Codes */
#define ERR_OK                  0
#define ERR_BAD_REQUEST         1
#define ERR_TOPIC_NOT_FOUND     2
#define ERR_FILE_NOT_FOUND      3
#define ERR_INTERNAL            4
#define ERR_INJECTED_FAILURE    5
#define ERR_CHECKSUM_MISMATCH   6
#define ERR_PROTOCOL            7

/* Wire Header (16 bytes packed explicitly) */
typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint16_t status;
    uint32_t len;
    uint32_t seq;
} proto_hdr_t;

/* Connection State */
typedef struct {
    int      fd;
    uint32_t tx_seq;
    uint32_t rx_seq;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    const char *err;
} conn_t;

/* SHA-256 Context Structure */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
} sha256_ctx_t;

/* Timing Helper */
double now_sec(void);

/* SHA-256 API */
void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_LEN]);
int  sha256_file(const char *path, uint8_t digest[SHA256_DIGEST_LEN], uint64_t *out_size);
void sha256_to_hex(const uint8_t digest[SHA256_DIGEST_LEN], char hex[65]);

/* Serialization Helpers (Explicit Network Byte Order) */
void     put16(uint8_t *p, uint16_t v);
uint16_t get16(const uint8_t *p);
void     put32(uint8_t *p, uint32_t v);
uint32_t get32(const uint8_t *p);
void     put64(uint8_t *p, uint64_t v);
uint64_t get64(const uint8_t *p);

/* Socket I/O Helpers */
int send_all(int fd, const void *buf, size_t n, int flags);
int recv_all(int fd, void *buf, size_t n);

/* Protocol Connection API */
void conn_init(conn_t *c, int fd);
int  conn_send(conn_t *c, uint8_t type, uint16_t flags, uint16_t status,
               const void *payload, uint32_t len, int send_flags);
int  conn_recv(conn_t *c, proto_hdr_t *h, uint8_t *payload);

/* Filesystem and Path Safety Helpers */
int  mkdir_p(const char *path);
int  rm_rf(const char *path);
int  safe_name(const char *name);
void sanitize_trailing_slashes(char *path);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_H */
