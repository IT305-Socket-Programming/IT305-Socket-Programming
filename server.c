/*
 * server.c - Part 1: topic-based file server (single- or multi-threaded)
 *
 *   ./server <port> <topics_root_dir> [--mode single|multi]     (default: multi)
 *
 * Every sub-directory of <topics_root_dir> is a topic; its name is the key.
 * Per-client statistics are printed to stderr when a transfer finishes.
 */
#define _GNU_SOURCE
#include "common.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Topic table: topic name -> directory (filled once at startup)       */
/* ------------------------------------------------------------------ */
typedef struct {
    char name[TOPIC_FIELD];
    char dir[PATH_MAX];
} topic_t;

static topic_t *g_topics;
static size_t   g_ntopics;

static volatile sig_atomic_t g_stop;
static atomic_ullong g_total_data_bytes;
static atomic_ullong g_total_clients;
static atomic_int    g_active;

#define LOG(...) do { fprintf(stderr, "[server] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static int cmp_topic(const void *a, const void *b)
{
    return strcmp(((const topic_t *)a)->name, ((const topic_t *)b)->name);
}

static int load_topics(const char *root)
{
    DIR *d = opendir(root);
    if (!d) { fprintf(stderr, "cannot open topics dir '%s': %s\n", root, strerror(errno)); return -1; }

    size_t cap = 16;
    g_topics = malloc(cap * sizeof *g_topics);
    if (!g_topics) { closedir(d); return -1; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        topic_t t;
        if (strlen(e->d_name) >= TOPIC_FIELD) { LOG("skipping '%s': name too long", e->d_name); continue; }
        int n = snprintf(t.dir, sizeof t.dir, "%s/%s", root, e->d_name);
        if (n < 0 || (size_t)n >= sizeof t.dir) { LOG("skipping '%s': path too long", e->d_name); continue; }

        struct stat st;
        if (stat(t.dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (access(t.dir, R_OK | X_OK) != 0) { LOG("skipping '%s': not accessible", e->d_name); continue; }

        strcpy(t.name, e->d_name);
        if (g_ntopics == cap) {
            cap *= 2;
            topic_t *nt = realloc(g_topics, cap * sizeof *g_topics);
            if (!nt) { closedir(d); return -1; }
            g_topics = nt;
        }
        g_topics[g_ntopics++] = t;
    }
    closedir(d);
    qsort(g_topics, g_ntopics, sizeof *g_topics, cmp_topic);
    return 0;
}

/* Optional convenience: English names accepted as aliases for the Italian topic folders.
 * Only used as a last resort, so exact folder names always win. */
static const struct { const char *alias, *topic; } g_aliases[] = {
    {"dog", "cane"},         {"horse", "cavallo"},   {"elephant", "elefante"},
    {"butterfly", "farfalla"}, {"chicken", "gallina"}, {"cat", "gatto"},
    {"cow", "mucca"},        {"sheep", "pecora"},    {"spider", "ragno"},
    {"squirrel", "scoiattolo"}
};

static const topic_t *find_topic(const char *name)
{
    for (size_t i = 0; i < g_ntopics; i++)
        if (strcmp(g_topics[i].name, name) == 0) return &g_topics[i];
    for (size_t i = 0; i < g_ntopics; i++)        /* forgiving fallback: case-insensitive */
        if (strcasecmp(g_topics[i].name, name) == 0) return &g_topics[i];
    for (size_t a = 0; a < sizeof g_aliases / sizeof g_aliases[0]; a++) {   /* English alias */
        if (strcasecmp(g_aliases[a].alias, name) != 0) continue;
        for (size_t i = 0; i < g_ntopics; i++)
            if (strcasecmp(g_topics[i].name, g_aliases[a].topic) == 0) return &g_topics[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* File listing                                                        */
/* ------------------------------------------------------------------ */
static int cmp_file(const void *a, const void *b)
{
    return strcmp(((const fentry_t *)a)->name, ((const fentry_t *)b)->name);
}

static int list_files(const char *dir, fentry_t **out, size_t *count)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    size_t cap = 64, n = 0;
    fentry_t *v = malloc(cap * sizeof *v);
    if (!v) { closedir(d); return -1; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char path[PATH_MAX];
        struct stat st;
        if (strlen(e->d_name) > MAX_NAME) continue;
        int len = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (len < 0 || (size_t)len >= sizeof path) continue;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;   /* regular files only */

        if (n == cap) {
            cap *= 2;
            fentry_t *nv = realloc(v, cap * sizeof *v);
            if (!nv) { free(v); closedir(d); return -1; }
            v = nv;
        }
        strcpy(v[n].name, e->d_name);
        v[n].size = (uint64_t)st.st_size;
        n++;
    }
    closedir(d);
    qsort(v, n, sizeof *v, cmp_file);
    *out = v;
    *count = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Serving one client                                                  */
/* ------------------------------------------------------------------ */
static int read_full(int fd, uint8_t *buf, size_t n)
{
    if (n > CHUNK_SIZE) return -1;                    /* never more than one chunk */
    while (n > 0) {
        ssize_t r = read(fd, buf, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;                        /* file shrank */
        buf += r;
        n -= (size_t)r;
    }
    return 0;
}

static void handle_client(int fd, const char *peer)
{
    conn_t c;
    conn_init(&c, fd);

    uint8_t *pl  = malloc(MAX_PAYLOAD);
    uint8_t *buf = malloc(HDR_LEN + CHUNK_SIZE);
    fentry_t *files = NULL;
    size_t nfiles = 0;
    int ffd = -1;
    uint64_t data_bytes = 0;
    int ok = 0;
    const char *topic_name = "-";

    if (!pl || !buf) { LOG("%s: out of memory", peer); goto out; }

    /* 1. GET request */
    msg_hdr_t h;
    int r = conn_recv(&c, &h, pl);
    if (r == -1) { LOG("%s: connection lost before request", peer); goto out; }
    if (r == -2) { LOG("%s: protocol error: %s", peer, c.err); send_error(&c, ERR_BAD_REQUEST, c.err); goto out; }
    if (h.type != MSG_GET_REQ || h.len != TOPIC_FIELD || pl[TOPIC_FIELD - 1] != '\0') {
        LOG("%s: invalid request", peer);
        send_error(&c, ERR_BAD_REQUEST, "expected GET with a 65-byte topic field");
        goto out;
    }
    double t0 = now_sec();                            /* start of "time to serve this client" */

    const topic_t *t = find_topic((const char *)pl);
    if (!t) {
        LOG("%s: unknown topic '%s'", peer, (const char *)pl);
        send_error(&c, ERR_TOPIC_NOT_FOUND, "unknown topic");
        goto out;
    }
    topic_name = t->name;

    /* 2. manifest */
    if (list_files(t->dir, &files, &nfiles) != 0) {
        LOG("%s: cannot list %s: %s", peer, t->dir, strerror(errno));
        send_error(&c, ERR_INTERNAL, "cannot read topic directory");
        goto out;
    }
    uint64_t total = 0;
    for (size_t i = 0; i < nfiles; i++) total += files[i].size;

    uint8_t ms[12];
    put32(ms, (uint32_t)nfiles);
    put64(ms + 4, total);
    if (conn_send(&c, MSG_MANIFEST_START, 0, ms, sizeof ms, 1) != 0) goto out;

    for (size_t i = 0; i < nfiles; i++) {
        uint8_t me[14 + MAX_NAME];
        uint16_t nl = (uint16_t)strlen(files[i].name);
        put32(me, (uint32_t)i);
        put64(me + 4, files[i].size);
        put16(me + 12, nl);
        memcpy(me + 14, files[i].name, nl);
        if (conn_send(&c, MSG_MANIFEST_ENTRY, 0, me, 14u + nl, 1) != 0) goto out;
    }
    uint8_t mend[4];
    put32(mend, (uint32_t)nfiles);
    if (conn_send(&c, MSG_MANIFEST_END, 0, mend, sizeof mend, 0) != 0) goto out;

    /* 3. files */
    for (size_t i = 0; i < nfiles; i++) {
        char path[PATH_MAX];
        int plen = snprintf(path, sizeof path, "%s/%s", t->dir, files[i].name);
        if (plen < 0 || (size_t)plen >= sizeof path) { LOG("%s: path too long for %s", peer, files[i].name); goto out; }
        ffd = open(path, O_RDONLY);
        if (ffd < 0) { LOG("%s: cannot open %s: %s", peer, path, strerror(errno)); goto out; }
        posix_fadvise(ffd, 0, 0, POSIX_FADV_SEQUENTIAL);

        uint8_t fh[4 + 8 + 8 + 2 + MAX_NAME];
        uint16_t nl = (uint16_t)strlen(files[i].name);
        put32(fh, (uint32_t)i);
        put64(fh + 4, 0);                              /* start offset (0 in Part 1) */
        put64(fh + 12, files[i].size);
        put16(fh + 20, nl);
        memcpy(fh + 22, files[i].name, nl);
        if (conn_send(&c, MSG_FILE_HEADER, 0, fh, 22u + nl, 1) != 0) goto out;  /* MSG_MORE: goes out with the data */

        uint64_t sent = 0;
        while (sent < files[i].size) {
            uint64_t left = files[i].size - sent;
            uint32_t n = left < CHUNK_SIZE ? (uint32_t)left : CHUNK_SIZE;
            if (read_full(ffd, buf + HDR_LEN, n) != 0) {
                LOG("%s: read error/short read on %s", peer, path);
                goto out;
            }
            uint8_t flags = (sent + n == files[i].size) ? FLAG_EOF_FILE : 0;
            if (conn_send_inplace(&c, buf, MSG_DATA_CHUNK, flags, n, 0) != 0) goto out;
            sent += n;
            data_bytes += n;
        }
        close(ffd);
        ffd = -1;
    }

    /* 4. done */
    uint8_t dn[8];
    put64(dn, data_bytes);
    if (conn_send(&c, MSG_TRANSFER_DONE, 0, dn, sizeof dn, 0) != 0) goto out;
    ok = 1;

    double secs = now_sec() - t0;
    LOG("done  peer=%s topic=%s files=%zu data_bytes=%llu wire_bytes=%llu time=%.3fs rate=%.2fMbps",
        peer, topic_name, nfiles, (unsigned long long)data_bytes,
        (unsigned long long)c.tx_bytes, secs, secs > 0 ? data_bytes * 8.0 / 1e6 / secs : 0.0);
    atomic_fetch_add(&g_total_data_bytes, data_bytes);
    atomic_fetch_add(&g_total_clients, 1);

out:
    if (!ok) LOG("aborted peer=%s topic=%s after %llu data bytes", peer, topic_name, (unsigned long long)data_bytes);
    if (ffd >= 0) close(ffd);
    free(files);
    free(buf);
    free(pl);
}

/* ------------------------------------------------------------------ */
/* Single / multi threaded accept loops                                */
/* ------------------------------------------------------------------ */
typedef struct { int fd; char peer[64]; } job_t;

static void *worker(void *arg)
{
    job_t j = *(job_t *)arg;
    free(arg);
    handle_client(j.fd, j.peer);
    close(j.fd);
    atomic_fetch_sub(&g_active, 1);
    return NULL;
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s <port> <topics_root_dir> [--mode single|multi]\n", prog);
    exit(2);
}

int main(int argc, char **argv)
{
    int multi = 1;
    if (argc != 3 && argc != 5) usage(argv[0]);
    if (argc == 5) {
        if (strcmp(argv[3], "--mode") != 0) usage(argv[0]);
        if (strcmp(argv[4], "single") == 0) multi = 0;
        else if (strcmp(argv[4], "multi") == 0) multi = 1;
        else usage(argv[0]);
    }
    char *end;
    long port = strtol(argv[1], &end, 10);
    if (*end || port < 1 || port > 65535) { fprintf(stderr, "invalid port '%s'\n", argv[1]); return 2; }

    if (load_topics(argv[2]) != 0) return 1;
    if (g_ntopics == 0) { fprintf(stderr, "no topic sub-directories found in '%s'\n", argv[2]); return 1; }
    for (size_t i = 0; i < g_ntopics; i++) LOG("topic '%s' -> %s", g_topics[i].name, g_topics[i].dir);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;                 /* no SA_RESTART: accept() must return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0) { perror("bind"); return 1; }
    if (listen(lfd, 128) != 0) { perror("listen"); return 1; }
    LOG("listening on port %ld, mode=%s", port, multi ? "multi-threaded" : "single-threaded");

    while (!g_stop) {
        struct sockaddr_in pa;
        socklen_t pl = sizeof pa;
        int cfd = accept(lfd, (struct sockaddr *)&pa, &pl);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            perror("accept");
            break;
        }
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        char peer[64], ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip);
        snprintf(peer, sizeof peer, "%s:%u", ip, ntohs(pa.sin_port));

        if (!multi) {                          /* serve sequentially */
            handle_client(cfd, peer);
            close(cfd);
        } else {                               /* one detached thread per client */
            job_t *j = malloc(sizeof *j);
            pthread_t th;
            if (!j) { close(cfd); continue; }
            j->fd = cfd;
            strcpy(j->peer, peer);
            atomic_fetch_add(&g_active, 1);
            if (pthread_create(&th, NULL, worker, j) != 0) {
                LOG("pthread_create failed for %s", peer);
                atomic_fetch_sub(&g_active, 1);
                close(cfd);
                free(j);
                continue;
            }
            pthread_detach(th);
        }
    }

    LOG("shutting down...");
    close(lfd);
    for (int i = 0; i < 100 && atomic_load(&g_active) > 0; i++) usleep(100000);   /* wait up to 10 s */
    LOG("served %llu clients, %llu data bytes", (unsigned long long)g_total_clients,
        (unsigned long long)g_total_data_bytes);
    free(g_topics);
    return 0;
}