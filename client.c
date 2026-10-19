/*
 * client.c - Part 1: topic client
 *
 *   ./client <server_ip> <server_port> <topic> <output_dir>
 *
 * Sends GET <topic>, receives the manifest and every file, and writes them to
 * <output_dir>/<topic>/. On success prints one machine-readable line:
 *   RESULT topic=.. files=.. data_bytes=.. wire_bytes=.. start=.. end=.. secs=.. mbps=..
 */
#define _GNU_SOURCE
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

volatile sig_atomic_t g_stop = 0;

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "client: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) die("cannot resolve %s: %s", host, gai_strerror(rc));

    int fd = -1;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) die("cannot connect to %s:%s: %s", host, port, strerror(errno));

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

/* Receive one frame, require type `want`; exit on I/O error, protocol error or server ERROR. */
static void recv_expect(conn_t *c, msg_hdr_t *h, uint8_t *pl, uint8_t want)
{
    int r = conn_recv(c, h, pl);
    if (r == -1) die("connection %s", errno ? strerror(errno) : "closed by server");
    if (r == -2) die("protocol error: %s", c->err);
    if (h->type == MSG_ERROR) {
        if (h->len >= 4) {
            unsigned code = get16(pl), n = get16(pl + 2);
            if (n > h->len - 4) n = h->len - 4;
            die("server error %u: %.*s", code, (int)n, (const char *)pl + 4);
        }
        die("server error");
    }
    if (h->type != want) die("unexpected message type %u (expected %u)", h->type, want);
}

static void write_full(int fd, const uint8_t *p, size_t n, const char *path)
{
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; die("write %s: %s", path, strerror(errno)); }
        if (r == 0) die("write %s: zero bytes written", path);
        p += r;
        n -= (size_t)r;
    }
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <server_ip> <server_port> <topic> <output_dir>\n", argv[0]);
        return 2;
    }
    const char *host = argv[1], *port = argv[2], *topic = argv[3], *outdir = argv[4];
    char *end;
    long p = strtol(port, &end, 10);
    if (*port == '\0' || *end != '\0' || p < 1 || p > 65535) die("invalid port '%s'", port);
    if (strlen(topic) >= TOPIC_FIELD || !safe_name(topic)) die("invalid topic name");
    if (outdir[0] == '\0') die("output directory cannot be empty");

    char clean_outdir[PATH_MAX];
    size_t olen = strlen(outdir);
    if (olen >= sizeof clean_outdir) die("output directory path too long");
    memcpy(clean_outdir, outdir, olen + 1);
    while (olen > 1 && clean_outdir[olen - 1] == '/') {
        clean_outdir[--olen] = '\0';
    }

    double wall0 = wall_sec(), t0 = now_sec();
    int fd = connect_to(host, port);
    conn_t c;
    conn_init(&c, fd);

    uint8_t *pl = malloc(MAX_PAYLOAD);
    if (!pl) die("out of memory");
    msg_hdr_t h;

    /* GET <topic> */
    uint8_t req[TOPIC_FIELD];
    memset(req, 0, sizeof req);
    memcpy(req, topic, strlen(topic));
    if (conn_send(&c, MSG_GET_REQ, 0, req, sizeof req, 0) != 0) die("send request failed: %s", strerror(errno));

    /* manifest */
    recv_expect(&c, &h, pl, MSG_MANIFEST_START);
    if (h.len != 12) die("bad MANIFEST_START");
    uint32_t nfiles = get32(pl);
    uint64_t total = get64(pl + 4);
    if (nfiles > 5000000u) die("manifest too large (%u files)", nfiles);

    fentry_t *files = calloc(nfiles ? nfiles : 1, sizeof *files);
    if (!files) die("out of memory");
    uint64_t sum = 0;
    for (uint32_t i = 0; i < nfiles; i++) {
        recv_expect(&c, &h, pl, MSG_MANIFEST_ENTRY);
        if (h.len < 14) die("bad MANIFEST_ENTRY");
        uint16_t nl = get16(pl + 12);
        if (get32(pl) != i || nl == 0 || nl > MAX_NAME || h.len != 14u + nl) die("bad MANIFEST_ENTRY %u", i);
        files[i].size = get64(pl + 4);
        memcpy(files[i].name, pl + 14, nl);
        files[i].name[nl] = '\0';
        if (!safe_name(files[i].name)) die("unsafe file name in manifest");
        sum += files[i].size;
    }
    recv_expect(&c, &h, pl, MSG_MANIFEST_END);
    if (h.len != 4 || get32(pl) != nfiles || sum != total) die("manifest inconsistent");

    char dir[PATH_MAX];
    if ((size_t)snprintf(dir, sizeof dir, "%s/%s", clean_outdir, topic) >= sizeof dir) die("output path too long");
    if (mkdir_p(dir) != 0) die("cannot create %s: %s", dir, strerror(errno));

    /* files */
    uint64_t data_bytes = 0;
    for (uint32_t i = 0; i < nfiles; i++) {
        recv_expect(&c, &h, pl, MSG_FILE_HEADER);
        if (h.len < 22) die("bad FILE_HEADER");
        uint16_t nl = get16(pl + 20);
        if (get32(pl) != i || h.len != 22u + nl || get64(pl + 4) != 0 || get64(pl + 12) != files[i].size ||
            nl != strlen(files[i].name) || memcmp(pl + 22, files[i].name, nl) != 0)
            die("FILE_HEADER %u does not match manifest", i);

        char path[PATH_MAX];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, files[i].name) >= sizeof path) die("path too long");
        int ofd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
        if (ofd < 0) {
            if (errno == ELOOP) die("cannot create %s: symbolic link in destination path rejected", path);
            die("cannot create %s: %s", path, strerror(errno));
        }
        struct stat st;
        if (fstat(ofd, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(ofd);
            die("destination %s is not a regular file", path);
        }

        uint64_t got = 0;
        while (got < files[i].size) {
            recv_expect(&c, &h, pl, MSG_DATA_CHUNK);
            uint64_t left = files[i].size - got;
            if (h.len == 0 || h.len > left) die("bad chunk size in %s", files[i].name);
            int is_last = (h.len == left);
            if (((h.flags & FLAG_EOF_FILE) != 0) != is_last) die("bad EOF flag in %s", files[i].name);
            write_full(ofd, pl, h.len, path);
            got += h.len;
        }
        if (close(ofd) != 0) die("close %s: %s", path, strerror(errno));
        data_bytes += got;
    }

    recv_expect(&c, &h, pl, MSG_TRANSFER_DONE);
    if (h.len != 8 || get64(pl) != data_bytes || data_bytes != total) die("transfer totals do not match");

    double secs = now_sec() - t0;
    close(fd);
    printf("RESULT topic=%s files=%u data_bytes=%llu wire_bytes=%llu start=%.6f end=%.6f secs=%.6f mbps=%.3f\n",
           topic, nfiles, (unsigned long long)data_bytes,
           (unsigned long long)(c.tx_bytes + c.rx_bytes), wall0, wall0 + secs, secs,
           secs > 0 ? data_bytes * 8.0 / 1e6 / secs : 0.0);
    fprintf(stderr, "client: received %u files (%llu bytes) into %s in %.3f s\n",
            nfiles, (unsigned long long)data_bytes, dir, secs);
    free(files);
    free(pl);
    return 0;
}
