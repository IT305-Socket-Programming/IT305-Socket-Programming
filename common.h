/*
 * common.h - shared protocol definitions and helpers (used by server and client)
 *
 * Wire format: every message = 12-byte header + payload.
 *
 *   offset 0  : magic        (u16)  0x5446
 *   offset 2  : message type (u8)
 *   offset 3  : flags        (u8)
 *   offset 4  : payload len  (u32)  <= MAX_PAYLOAD
 *   offset 8  : sequence no. (u32)  starts at 0, +1 per frame, per direction
 *
 * All integers are in network byte order (big endian).
 *
 * Part 1 conversation:
 *   C -> S : GET_REQ        topic[65]
 *   S -> C : MANIFEST_START nfiles(u32) total_bytes(u64)      (or ERROR)
 *   S -> C : MANIFEST_ENTRY index(u32) size(u64) namelen(u16) name   x nfiles
 *   S -> C : MANIFEST_END   nfiles(u32)
 *   S -> C : for each file:
 *              FILE_HEADER  index(u32) start_offset(u64) size(u64) namelen(u16) name
 *              DATA_CHUNK   raw bytes (<= CHUNK_SIZE); FLAG_EOF_FILE on the last one
 *   S -> C : TRANSFER_DONE  total_bytes(u64)
 *   ERROR  : code(u16) msglen(u16) msg      (either direction, then close)
 *
 * start_offset is always 0 in Part 1; it is already in the format so that
 * Part 2 (resume) can reuse the same messages. MSG_ACK is reserved for Part 2.
 */
#ifndef COMMON_H
#define COMMON_H

#include <signal.h>
#include <stdint.h>
#include <stddef.h>

extern volatile sig_atomic_t g_stop;

#define PROTO_MAGIC   0x5446u
#define HDR_LEN       12
#define MAX_PAYLOAD   65536
#define CHUNK_SIZE    65536
#define TOPIC_FIELD   65          /* 64 chars + NUL */
#define MAX_NAME      255
#define CTRL_BUF      1024        /* max size (header+payload) of small control frames */

enum msg_type {
    MSG_GET_REQ = 1,
    MSG_MANIFEST_START,
    MSG_MANIFEST_ENTRY,
    MSG_MANIFEST_END,
    MSG_FILE_HEADER,
    MSG_DATA_CHUNK,
    MSG_ACK,                      /* reserved for Part 2 */
    MSG_TRANSFER_DONE,
    MSG_ERROR                     /* keep last: used for range check */
};

#define FLAG_EOF_FILE  0x01u
#define FLAGS_KNOWN    (FLAG_EOF_FILE)

enum err_code {
    ERR_BAD_REQUEST    = 1,
    ERR_TOPIC_NOT_FOUND = 2,
    ERR_INTERNAL       = 3
};

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint32_t len;
    uint32_t seq;
} msg_hdr_t;

/* One entry of a topic's file list */
typedef struct {
    char     name[MAX_NAME + 1];
    uint64_t size;
} fentry_t;

/* A connection with per-direction sequence numbers and byte counters */
typedef struct {
    int         fd;
    uint32_t    tx_seq, rx_seq;
    uint64_t    tx_bytes, rx_bytes;   /* wire bytes incl. headers */
    const char *err;                  /* set on protocol error */
} conn_t;

/* ---- big-endian pack/unpack helpers ---- */
static inline void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v;
}
static inline void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v);
}
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t get64(const uint8_t *p) {
    return ((uint64_t)get32(p) << 32) | get32(p + 4);
}

/* ---- socket I/O ---- */
int  send_all(int fd, const void *buf, size_t n, int flags);   /* 0 ok, -1 error */
int  recv_all(int fd, void *buf, size_t n);                    /* 0 ok, -1 error/EOF (errno==0 on EOF) */

void conn_init(conn_t *c, int fd);
/* buf must have HDR_LEN bytes free in front of the payload (payload at buf+HDR_LEN) */
int  conn_send_inplace(conn_t *c, uint8_t *buf, uint8_t type, uint8_t flags,
                       uint32_t len, int more);
/* copies payload into a small stack buffer (len <= CTRL_BUF-HDR_LEN) */
int  conn_send(conn_t *c, uint8_t type, uint8_t flags,
               const void *payload, uint32_t len, int more);
/* returns 0 ok, -1 I/O error or EOF, -2 protocol error (see c->err) */
int  conn_recv(conn_t *c, msg_hdr_t *h, uint8_t *payload);      /* payload >= MAX_PAYLOAD bytes */

int  send_error(conn_t *c, uint16_t code, const char *msg);

/* ---- misc ---- */
double now_sec(void);             /* monotonic, for durations */
double wall_sec(void);            /* epoch seconds, for cross-machine timestamps */
int    mkdir_p(const char *path); /* 0 ok, -1 error */
int    safe_name(const char *name); /* 1 if usable as a single path component */

#endif
