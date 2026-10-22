#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <limits.h>

double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ============================================================================
 * SHA-256 Implementation (FIPS 180-4 / RFC 6234)
 * ============================================================================ */

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffeu, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    for (int i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->count = 0;
}

void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t buf_idx = (size_t)(ctx->count & 0x3f);
    ctx->count += len;

    if (buf_idx > 0) {
        size_t left = 64 - buf_idx;
        if (len < left) {
            memcpy(ctx->buffer + buf_idx, p, len);
            return;
        }
        memcpy(ctx->buffer + buf_idx, p, left);
        sha256_transform(ctx, ctx->buffer);
        p += left;
        len -= left;
    }

    while (len >= 64) {
        sha256_transform(ctx, p);
        p += 64;
        len -= 64;
    }

    if (len > 0) {
        memcpy(ctx->buffer, p, len);
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_LEN])
{
    uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);

    uint8_t zero = 0;
    while ((ctx->count & 0x3f) != 56) {
        sha256_update(ctx, &zero, 1);
    }

    uint64_t bits = (ctx->count - 1) * 8;
    uint8_t len_bytes[8];
    for (int i = 7; i >= 0; i--) {
        len_bytes[i] = (uint8_t)(bits & 0xff);
        bits >>= 8;
    }
    sha256_update(ctx, len_bytes, 8);

    for (int i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)((ctx->state[i] >> 24) & 0xff);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xff);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xff);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xff);
    }
}

int sha256_file(const char *path, uint8_t digest[SHA256_DIGEST_LEN], uint64_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }

    sha256_ctx_t ctx;
    sha256_init(&ctx);

    uint8_t buf[CHUNK_SIZE];
    uint64_t total = 0;
    while (1) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        sha256_update(&ctx, buf, (size_t)n);
        total += (size_t)n;
    }

    close(fd);
    sha256_final(&ctx, digest);
    if (out_size) *out_size = total;
    return 0;
}

void sha256_to_hex(const uint8_t digest[SHA256_DIGEST_LEN], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_DIGEST_LEN; i++) {
        hex[i * 2]     = digits[(digest[i] >> 4) & 0x0f];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[64] = '\0';
}

/* ============================================================================
 * Serialization Helpers (Explicit Network Byte Order)
 * ============================================================================ */

void put16(uint8_t *p, uint16_t v)
{
    uint16_t nv = htons(v);
    memcpy(p, &nv, sizeof nv);
}

uint16_t get16(const uint8_t *p)
{
    uint16_t nv;
    memcpy(&nv, p, sizeof nv);
    return ntohs(nv);
}

void put32(uint8_t *p, uint32_t v)
{
    uint32_t nv = htonl(v);
    memcpy(p, &nv, sizeof nv);
}

uint32_t get32(const uint8_t *p)
{
    uint32_t nv;
    memcpy(&nv, p, sizeof nv);
    return ntohl(nv);
}

void put64(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)((v >> 56) & 0xff);
    p[1] = (uint8_t)((v >> 48) & 0xff);
    p[2] = (uint8_t)((v >> 40) & 0xff);
    p[3] = (uint8_t)((v >> 32) & 0xff);
    p[4] = (uint8_t)((v >> 24) & 0xff);
    p[5] = (uint8_t)((v >> 16) & 0xff);
    p[6] = (uint8_t)((v >> 8) & 0xff);
    p[7] = (uint8_t)(v & 0xff);
}

uint64_t get64(const uint8_t *p)
{
    return ((uint64_t)p[0] << 56) |
           ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) |
           ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) |
           ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  |
           ((uint64_t)p[7]);
}

/* ============================================================================
 * Socket I/O and Protocol Framing
 * ============================================================================ */

int send_all(int fd, const void *buf, size_t n, int flags)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL | flags);
        if (r < 0) {
            if (errno == EINTR) {
                if (g_stop) return -1;
                continue;
            }
            return -1;
        }
        if (r == 0) {
            errno = ECONNRESET;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int recv_all(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0) {
            if (errno == EINTR) {
                if (g_stop) return -1;
                continue;
            }
            return -1;
        }
        if (r == 0) {
            errno = 0; /* orderly peer disconnect */
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

void conn_init(conn_t *c, int fd)
{
    memset(c, 0, sizeof *c);
    c->fd = fd;
}

int conn_send(conn_t *c, uint8_t type, uint16_t flags, uint16_t status,
              const void *payload, uint32_t len, int send_flags)
{
    uint8_t hdr[HDR_LEN];
    put16(hdr + 0,  PROTO_MAGIC);
    hdr[2] = PROTO_VERSION;
    hdr[3] = type;
    put16(hdr + 4,  flags);
    put16(hdr + 6,  status);
    put32(hdr + 8,  len);
    put32(hdr + 12, c->tx_seq);

    if (send_all(c->fd, hdr, HDR_LEN, (len > 0 ? MSG_MORE : 0) | send_flags) != 0) {
        return -1;
    }
    if (len > 0 && send_all(c->fd, payload, len, send_flags) != 0) {
        return -1;
    }

    c->tx_seq++;
    c->tx_bytes += HDR_LEN + len;
    return 0;
}

int conn_recv(conn_t *c, proto_hdr_t *h, uint8_t *payload)
{
    uint8_t hdr[HDR_LEN];
    if (recv_all(c->fd, hdr, HDR_LEN) != 0) return -1;

    h->magic   = get16(hdr + 0);
    h->version = hdr[2];
    h->type    = hdr[3];
    h->flags   = get16(hdr + 4);
    h->status  = get16(hdr + 6);
    h->len     = get32(hdr + 8);
    h->seq     = get32(hdr + 12);

    if (h->magic != PROTO_MAGIC) {
        c->err = "bad magic";
        return -2;
    }
    if (h->version != PROTO_VERSION) {
        c->err = "unsupported protocol version";
        return -2;
    }
    if (h->len > MAX_PAYLOAD) {
        c->err = "payload too large";
        return -2;
    }
    if (h->seq != c->rx_seq) {
        c->err = "sequence number mismatch";
        return -2;
    }

    if (h->len > 0) {
        if (!payload) {
            c->err = "null payload buffer";
            return -2;
        }
        if (recv_all(c->fd, payload, h->len) != 0) return -1;
    }

    c->rx_seq++;
    c->rx_bytes += HDR_LEN + h->len;
    return 0;
}

/* ============================================================================
 * Filesystem and Safety Helpers
 * ============================================================================ */

void sanitize_trailing_slashes(char *path)
{
    if (!path) return;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
}

int mkdir_p(const char *path)
{
    if (!path || path[0] == '\0') return -1;
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            if (*(p - 1) == '/') continue;
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int rm_rf(const char *path)
{
    if (!path || path[0] == '\0') return -1;
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }

    DIR *d = opendir(path);
    if (!d) return -1;

    struct dirent *e;
    int ret = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char sub[PATH_MAX];
        int n = snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (n < 0 || (size_t)n >= sizeof sub) {
            ret = -1;
            break;
        }
        if (rm_rf(sub) != 0) {
            ret = -1;
            break;
        }
    }
    closedir(d);
    if (ret != 0) return -1;
    return rmdir(path);
}

int safe_name(const char *name)
{
    if (!name || name[0] == '\0') return 0;
    size_t len = strlen(name);
    if (len > MAX_NAME) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;

    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char uc = (unsigned char)name[i];
        if (uc < 32 || uc == 127) return 0;
    }
    return 1;
}
