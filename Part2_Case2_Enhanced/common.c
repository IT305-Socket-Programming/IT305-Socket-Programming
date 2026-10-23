#include "common.h"

/* ============================================================================
 * Self-Contained FIPS 180-4 SHA-256 Implementation
 * ============================================================================ */

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h, m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + K256[i] + m[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i = (size_t)(ctx->count & 63);
    ctx->count += len;
    while (len > 0) {
        size_t take = (64 - i < len) ? (64 - i) : len;
        memcpy(&ctx->buffer[i], p, take);
        i += take; p += take; len -= take;
        if (i == 64) {
            sha256_transform(ctx, ctx->buffer);
            i = 0;
        }
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_LEN])
{
    size_t i = (size_t)(ctx->count & 63);
    ctx->buffer[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->buffer[i++] = 0x00;
        sha256_transform(ctx, ctx->buffer);
        i = 0;
    }
    while (i < 56) ctx->buffer[i++] = 0x00;
    uint64_t total_bits = ctx->count * 8;
    for (int k = 7; k >= 0; --k) {
        ctx->buffer[56 + (7 - k)] = (uint8_t)(total_bits >> (k * 8));
    }
    sha256_transform(ctx, ctx->buffer);
    for (int k = 0; k < 8; ++k) {
        digest[k * 4 + 0] = (uint8_t)(ctx->state[k] >> 24);
        digest[k * 4 + 1] = (uint8_t)(ctx->state[k] >> 16);
        digest[k * 4 + 2] = (uint8_t)(ctx->state[k] >> 8);
        digest[k * 4 + 3] = (uint8_t)(ctx->state[k]);
    }
}

int sha256_file(const char *path, uint8_t digest[SHA256_DIGEST_LEN], uint64_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    uint8_t buf[65536];
    uint64_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) { close(fd); return -1; }
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
        hex[i * 2 + 0] = digits[(digest[i] >> 4) & 0x0f];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[64] = '\0';
}

/* ============================================================================
 * Framing and Connection I/O Primitives
 * ============================================================================ */

void conn_init(conn_t *c, int fd)
{
    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->tx_seq = 1;
    c->rx_seq = 0;

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

ssize_t write_full(int fd, const void *buf, size_t count, const char *path)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = count;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "write_full failed for '%s': %s\n", path ? path : "fd", strerror(errno));
            return -1;
        }
        left -= (size_t)n;
        p += n;
    }
    return (ssize_t)count;
}

ssize_t read_full(int fd, void *buf, size_t count)
{
    uint8_t *p = (uint8_t *)buf;
    size_t left = count;
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return (ssize_t)(count - left); /* Short read / EOF */
        left -= (size_t)n;
        p += n;
    }
    return (ssize_t)count;
}

int conn_send(conn_t *c, uint8_t type, uint16_t flags, uint16_t status,
              const void *payload, uint32_t len, int do_cork)
{
    (void)do_cork;
    uint8_t hbuf[16];
    put16(hbuf + 0, PROTO_MAGIC);
    hbuf[2] = PROTO_VERSION;
    hbuf[3] = type;
    put16(hbuf + 4, flags);
    put16(hbuf + 6, status);
    put32(hbuf + 8, len);
    put32(hbuf + 12, c->tx_seq++);

    if (write_full(c->fd, hbuf, 16, "socket") != 16) {
        c->err = "write header failed";
        return -1;
    }
    c->tx_bytes += 16;

    if (len > 0 && payload) {
        if (write_full(c->fd, payload, len, "socket") != (ssize_t)len) {
            c->err = "write payload failed";
            return -1;
        }
        c->tx_bytes += len;
    }
    return 0;
}

int conn_recv(conn_t *c, proto_hdr_t *out_h, void *out_payload)
{
    uint8_t hbuf[16];
    ssize_t r = read_full(c->fd, hbuf, 16);
    if (r != 16) {
        c->err = (r == 0) ? "connection closed" : "short header read";
        return -1;
    }
    c->rx_bytes += 16;

    out_h->magic   = get16(hbuf + 0);
    out_h->version = hbuf[2];
    out_h->type    = hbuf[3];
    out_h->flags   = get16(hbuf + 4);
    out_h->status  = get16(hbuf + 6);
    out_h->len     = get32(hbuf + 8);
    out_h->seq     = get32(hbuf + 12);

    if (out_h->magic != PROTO_MAGIC) {
        c->err = "magic mismatch";
        return -1;
    }
    if (out_h->version != PROTO_VERSION) {
        c->err = "version mismatch";
        return -1;
    }
    if (out_h->len > MAX_PAYLOAD) {
        c->err = "payload too large";
        return -1;
    }

    if (out_h->len > 0) {
        r = read_full(c->fd, out_payload, out_h->len);
        if (r != (ssize_t)out_h->len) {
            c->err = "short payload read";
            return -1;
        }
        c->rx_bytes += out_h->len;
    }
    c->rx_seq = out_h->seq;
    return 0;
}

/* ============================================================================
 * Filesystem & Utility Primitives
 * ============================================================================ */

int safe_name(const char *name)
{
    if (!name || name[0] == '\0') return 0;
    size_t len = strlen(name);
    if (len > MAX_NAME) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (strstr(name, "..") != NULL) return 0;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char uc = (unsigned char)name[i];
        if (uc == 0 || uc == 127) return 0;
    }
    return 1;
}

int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp) return -1;
    strcpy(tmp, path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
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
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return unlink(path);

    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char sub[PATH_MAX + 512];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        rm_rf(sub);
    }
    closedir(d);
    return rmdir(path);
}

double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void sanitize_trailing_slashes(char *path)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
}
