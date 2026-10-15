/* common.c - framing, socket I/O helpers and small utilities */
#define _GNU_SOURCE
#include "common.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>

double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

double wall_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int send_all(int fd, const void *buf, size_t n, int flags)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL | flags);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int recv_all(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {            /* peer closed */
            errno = 0;
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

int conn_send_inplace(conn_t *c, uint8_t *buf, uint8_t type, uint8_t flags,
                      uint32_t len, int more)
{
    put16(buf, PROTO_MAGIC);
    buf[2] = type;
    buf[3] = flags;
    put32(buf + 4, len);
    put32(buf + 8, c->tx_seq);
    if (send_all(c->fd, buf, HDR_LEN + len, more ? MSG_MORE : 0) != 0)
        return -1;
    c->tx_seq++;
    c->tx_bytes += HDR_LEN + len;
    return 0;
}

int conn_send(conn_t *c, uint8_t type, uint8_t flags,
              const void *payload, uint32_t len, int more)
{
    uint8_t buf[CTRL_BUF];
    if (len > CTRL_BUF - HDR_LEN) { errno = EMSGSIZE; return -1; }
    if (len) memcpy(buf + HDR_LEN, payload, len);
    return conn_send_inplace(c, buf, type, flags, len, more);
}

int conn_recv(conn_t *c, msg_hdr_t *h, uint8_t *payload)
{
    uint8_t raw[HDR_LEN];
    if (recv_all(c->fd, raw, HDR_LEN) != 0) return -1;

    if (get16(raw) != PROTO_MAGIC)          { c->err = "bad magic";           return -2; }
    h->type  = raw[2];
    h->flags = raw[3];
    h->len   = get32(raw + 4);
    h->seq   = get32(raw + 8);
    if (h->type < MSG_GET_REQ || h->type > MSG_ERROR) { c->err = "unknown message type"; return -2; }
    if (h->flags & ~FLAGS_KNOWN)            { c->err = "reserved flag bits set"; return -2; }
    if (h->len > MAX_PAYLOAD)               { c->err = "payload too large";    return -2; }
    if (h->seq != c->rx_seq)                { c->err = "bad sequence number";  return -2; }

    if (h->len && recv_all(c->fd, payload, h->len) != 0) return -1;
    c->rx_seq++;
    c->rx_bytes += HDR_LEN + h->len;
    return 0;
}

int send_error(conn_t *c, uint16_t code, const char *msg)
{
    uint8_t pl[4 + 256];
    size_t n = strlen(msg);
    if (n > 256) n = 256;
    put16(pl, code);
    put16(pl + 2, (uint16_t)n);
    memcpy(pl + 4, msg, n);
    return conn_send(c, MSG_ERROR, 0, pl, (uint32_t)(4 + n), 0);
}

int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    struct stat st;
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

int safe_name(const char *name)
{
    if (name[0] == '\0') return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    return strchr(name, '/') == NULL;
}
