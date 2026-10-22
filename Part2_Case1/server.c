#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <limits.h>
#include "common.h"

#define LOG(fmt, ...) do { \
    time_t _t = time(NULL); \
    struct tm _tm; \
    localtime_r(&_t, &_tm); \
    char _ts[32]; \
    strftime(_ts, sizeof _ts, "%Y-%m-%d %H:%M:%S", &_tm); \
    fprintf(stdout, "[%s] [Part2_Case1] " fmt "\n", _ts, ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)

#ifndef MAX_ACTIVE_CLIENTS
#define MAX_ACTIVE_CLIENTS 1024
#endif

/* English to Italian topic alias mapping (Animals10 dataset) */
static const struct { const char *alias; const char *canonical; } ALIASES[] = {
    { "dog",        "cane"       },
    { "horse",      "cavallo"    },
    { "elephant",   "elefante"   },
    { "butterfly",  "farfalla"   },
    { "chicken",    "gallina"    },
    { "cat",        "gatto"      },
    { "cow",        "mucca"      },
    { "sheep",      "pecora"     },
    { "spider",     "ragno"      },
    { "squirrel",   "scoiattolo" },
};

typedef struct {
    char     name[MAX_NAME + 1];
    uint64_t size;
    uint8_t  hash[SHA256_DIGEST_LEN];
} fentry_t;

typedef struct {
    char     name[TOPIC_FIELD];
    char     dir[PATH_MAX];
    size_t   nfiles;
    fentry_t *files;
    uint64_t total_bytes;
    uint8_t  manifest_hash[SHA256_DIGEST_LEN];
} topic_t;

static topic_t *g_topics = NULL;
static size_t   g_ntopics = 0;
volatile sig_atomic_t g_stop = 0;
static double   g_failure_prob = 0.0;

/* Active Client Connection Registry */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             active_workers;
    int             fds[MAX_ACTIVE_CLIENTS];
} client_registry_t;

static client_registry_t g_client_reg;

static void client_reg_init(void)
{
    pthread_mutex_init(&g_client_reg.lock, NULL);
    pthread_cond_init(&g_client_reg.cond, NULL);
    g_client_reg.active_workers = 0;
    for (int i = 0; i < MAX_ACTIVE_CLIENTS; i++) {
        g_client_reg.fds[i] = -1;
    }
}

static int client_reg_add(int fd)
{
    pthread_mutex_lock(&g_client_reg.lock);
    for (int i = 0; i < MAX_ACTIVE_CLIENTS; i++) {
        if (g_client_reg.fds[i] == -1) {
            g_client_reg.fds[i] = fd;
            g_client_reg.active_workers++;
            pthread_mutex_unlock(&g_client_reg.lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_client_reg.lock);
    return -1;
}

static void client_reg_del(int fd)
{
    pthread_mutex_lock(&g_client_reg.lock);
    int found = 0;
    for (int i = 0; i < MAX_ACTIVE_CLIENTS; i++) {
        if (g_client_reg.fds[i] == fd) {
            g_client_reg.fds[i] = -1;
            close(fd);
            found = 1;
            break;
        }
    }
    if (found) {
        g_client_reg.active_workers--;
        if (g_client_reg.active_workers == 0) {
            pthread_cond_broadcast(&g_client_reg.cond);
        }
    } else {
        close(fd);
    }
    pthread_mutex_unlock(&g_client_reg.lock);
}

static void client_reg_interrupt_all(void)
{
    pthread_mutex_lock(&g_client_reg.lock);
    for (int i = 0; i < MAX_ACTIVE_CLIENTS; i++) {
        if (g_client_reg.fds[i] != -1) {
            shutdown(g_client_reg.fds[i], SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&g_client_reg.lock);
}

static int client_reg_wait_empty(int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&g_client_reg.lock);
    while (g_client_reg.active_workers > 0) {
        int rc = pthread_cond_timedwait(&g_client_reg.cond, &g_client_reg.lock, &ts);
        if (rc == ETIMEDOUT) break;
    }
    int remaining = g_client_reg.active_workers;
    pthread_mutex_unlock(&g_client_reg.lock);
    return remaining;
}

/* Global Statistics Counters */
static atomic_ullong g_total_clients = 0;
static atomic_ullong g_total_data_bytes = 0;
static atomic_ullong g_injected_failures = 0;

/* Helper to compare filenames for deterministic sorting */
static int compare_fentries(const void *a, const void *b)
{
    const fentry_t *fa = (const fentry_t *)a;
    const fentry_t *fb = (const fentry_t *)b;
    return strcmp(fa->name, fb->name);
}

/* Scan and hash all regular files in a topic directory */
static int scan_topic_files(topic_t *t)
{
    DIR *d = opendir(t->dir);
    if (!d) return -1;

    size_t cap = 16;
    t->files = malloc(cap * sizeof(fentry_t));
    if (!t->files) { closedir(d); return -1; }
    t->nfiles = 0;
    t->total_bytes = 0;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!safe_name(e->d_name)) continue;

        char path[PATH_MAX];
        int n = snprintf(path, sizeof path, "%s/%s", t->dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof path) continue;

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || access(path, R_OK) != 0) continue;

        if (t->nfiles == cap) {
            cap *= 2;
            fentry_t *tmp = realloc(t->files, cap * sizeof(fentry_t));
            if (!tmp) { free(t->files); t->files = NULL; closedir(d); return -1; }
            t->files = tmp;
        }

        fentry_t *fe = &t->files[t->nfiles];
        snprintf(fe->name, sizeof fe->name, "%s", e->d_name);
        fe->size = (uint64_t)st.st_size;

        /* Calculate file SHA-256 */
        if (sha256_file(path, fe->hash, NULL) != 0) {
            LOG("failed to compute hash for %s", path);
            continue;
        }

        t->total_bytes += fe->size;
        t->nfiles++;
    }
    closedir(d);

    /* Deterministic alphabetical sort */
    qsort(t->files, t->nfiles, sizeof(fentry_t), compare_fentries);

    /* Calculate aggregate manifest hash */
    sha256_ctx_t m_ctx;
    sha256_init(&m_ctx);
    for (size_t i = 0; i < t->nfiles; i++) {
        sha256_update(&m_ctx, t->files[i].name, strlen(t->files[i].name));
        sha256_update(&m_ctx, &t->files[i].size, sizeof(uint64_t));
        sha256_update(&m_ctx, t->files[i].hash, SHA256_DIGEST_LEN);
    }
    sha256_final(&m_ctx, t->manifest_hash);

    char hex[65];
    sha256_to_hex(t->manifest_hash, hex);
    LOG("loaded topic '%s': %zu files, %llu bytes, manifest_sha256=%s",
        t->name, t->nfiles, (unsigned long long)t->total_bytes, hex);
    return 0;
}

static int load_topics(const char *root)
{
    DIR *d = opendir(root);
    if (!d) { perror("opendir topics root"); return -1; }

    char clean_root[PATH_MAX];
    size_t root_len = strlen(root);
    if (root_len >= sizeof clean_root) { closedir(d); return -1; }
    strcpy(clean_root, root);
    sanitize_trailing_slashes(clean_root);

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!safe_name(e->d_name)) continue;

        topic_t t;
        memset(&t, 0, sizeof t);
        if (strlen(e->d_name) >= TOPIC_FIELD) continue;
        int n = snprintf(t.dir, sizeof t.dir, "%s/%s", clean_root, e->d_name);
        if (n < 0 || (size_t)n >= sizeof t.dir) continue;

        struct stat st;
        if (stat(t.dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        snprintf(t.name, sizeof t.name, "%s", e->d_name);
        if (scan_topic_files(&t) != 0) continue;

        topic_t *tmp = realloc(g_topics, (g_ntopics + 1) * sizeof(topic_t));
        if (!tmp) { closedir(d); return -1; }
        g_topics = tmp;
        g_topics[g_ntopics++] = t;
    }
    closedir(d);
    return 0;
}

static const topic_t *find_topic(const char *name)
{
    for (size_t i = 0; i < g_ntopics; i++) {
        if (strcmp(g_topics[i].name, name) == 0) return &g_topics[i];
    }
    for (size_t i = 0; i < g_ntopics; i++) {
        if (strcasecmp(g_topics[i].name, name) == 0) return &g_topics[i];
    }
    for (size_t a = 0; a < sizeof ALIASES / sizeof ALIASES[0]; a++) {
        if (strcasecmp(ALIASES[a].alias, name) == 0) {
            for (size_t i = 0; i < g_ntopics; i++) {
                if (strcasecmp(g_topics[i].name, ALIASES[a].canonical) == 0) {
                    return &g_topics[i];
                }
            }
        }
    }
    return NULL;
}

static void send_error(conn_t *c, uint16_t code, const char *msg)
{
    uint8_t pl[2 + 256];
    put16(pl, code);
    uint16_t mlen = (uint16_t)strlen(msg);
    if (mlen > 254) mlen = 254;
    put16(pl + 2, mlen);
    memcpy(pl + 4, msg, mlen);
    conn_send(c, MSG_ERROR, 0, code, pl, 4 + mlen, 0);
}

static int should_inject_fault(unsigned int *seed, uint32_t chunk_index, const char *peer, const char *file_name)
{
    /* Test-only deterministic control hooks (disabled by default) */
    const char *fail_after_env = getenv("IT305_TEST_FAIL_AFTER_CHUNKS");
    if (fail_after_env && fail_after_env[0] != '\0') {
        long target = strtol(fail_after_env, NULL, 10);
        if (target >= 0 && (long)chunk_index >= target) {
            LOG("%s: [DETERMINISTIC FAULT] injected on chunk %u (IT305_TEST_FAIL_AFTER_CHUNKS=%ld) on '%s'",
                peer, chunk_index, target, file_name);
            return 1;
        }
    }

    static atomic_int s_deterministic_faults_done = 0;
    const char *fail_on_env = getenv("IT305_TEST_FAIL_ON_CHUNK");
    if (fail_on_env && fail_on_env[0] != '\0') {
        int max_faults = 1;
        const char *max_f_env = getenv("IT305_TEST_FAIL_COUNT");
        if (max_f_env && max_f_env[0] != '\0') {
            max_faults = atoi(max_f_env);
        }
        long target = strtol(fail_on_env, NULL, 10);
        if (target > 0 && (long)chunk_index == target && atomic_load(&s_deterministic_faults_done) < max_faults) {
            atomic_fetch_add(&s_deterministic_faults_done, 1);
            LOG("%s: [DETERMINISTIC FAULT] injected on chunk %u (IT305_TEST_FAIL_ON_CHUNK=%ld, count=%d) on '%s'",
                peer, chunk_index, target, max_faults, file_name);
            return 1;
        }
    }

    /* Production probabilistic Bernoulli failure injection */
    if (g_failure_prob <= 0.0) return 0;
    if (g_failure_prob >= 1.0) {
        LOG("%s: [BERNOULLI FAULT] injected (p=1.00) on chunk %u on '%s'", peer, chunk_index, file_name);
        return 1;
    }

    double roll = (double)rand_r(seed) / (double)RAND_MAX;
    if (roll < g_failure_prob) {
        LOG("%s: [BERNOULLI FAULT] injected (p=%.4f, roll=%.4f) on chunk %u on '%s'",
            peer, g_failure_prob, roll, chunk_index, file_name);
        return 1;
    }
    return 0;
}

static void handle_client(int fd, const char *peer, unsigned int *seed)
{
    conn_t c;
    conn_init(&c, fd);

    uint8_t pl[MAX_PAYLOAD];
    proto_hdr_t h;

    if (conn_recv(&c, &h, pl) != 0) {
        LOG("%s: handshake recv failed (%s)", peer, c.err ? c.err : "disconnect");
        return;
    }
    if (h.type != MSG_GET_REQ) {
        LOG("%s: unexpected message type 0x%02x, expected MSG_GET_REQ", peer, h.type);
        send_error(&c, ERR_BAD_REQUEST, "expected MSG_GET_REQ");
        return;
    }

    char topic[TOPIC_FIELD];
    size_t tlen = h.len < sizeof topic - 1 ? h.len : sizeof topic - 1;
    memcpy(topic, pl, tlen);
    topic[tlen] = '\0';
    while (tlen > 0 && (topic[tlen - 1] == ' ' || topic[tlen - 1] == '\r' || topic[tlen - 1] == '\n')) {
        topic[--tlen] = '\0';
    }

    LOG("%s: requested topic '%s'", peer, topic);
    const topic_t *t = find_topic(topic);
    if (!t) {
        LOG("%s: topic '%s' not found", peer, topic);
        send_error(&c, ERR_TOPIC_NOT_FOUND, "topic not found");
        return;
    }

    /* 1. Send MSG_MANIFEST_START */
    uint8_t m_start[4 + 8 + SHA256_DIGEST_LEN];
    put32(m_start + 0, (uint32_t)t->nfiles);
    put64(m_start + 4, t->total_bytes);
    memcpy(m_start + 12, t->manifest_hash, SHA256_DIGEST_LEN);
    if (conn_send(&c, MSG_MANIFEST_START, 0, ERR_OK, m_start, sizeof m_start, 0) != 0) return;

    /* 2. Stream MSG_MANIFEST_ENTRY frames */
    for (size_t i = 0; i < t->nfiles; i++) {
        if (g_stop) return;
        const fentry_t *fe = &t->files[i];
        uint16_t nlen = (uint16_t)strlen(fe->name);
        uint8_t entry[4 + 8 + SHA256_DIGEST_LEN + 2 + MAX_NAME];
        put32(entry + 0, (uint32_t)i);
        put64(entry + 4, fe->size);
        memcpy(entry + 12, fe->hash, SHA256_DIGEST_LEN);
        put16(entry + 12 + SHA256_DIGEST_LEN, nlen);
        memcpy(entry + 14 + SHA256_DIGEST_LEN, fe->name, nlen);

        uint32_t elen = 14 + SHA256_DIGEST_LEN + nlen;
        if (conn_send(&c, MSG_MANIFEST_ENTRY, 0, ERR_OK, entry, elen, 0) != 0) return;
    }

    /* 3. Send MSG_MANIFEST_END */
    uint8_t m_end[4 + 4];
    put32(m_end + 0, (uint32_t)t->nfiles);
    put32(m_end + 4, ERR_OK);
    if (conn_send(&c, MSG_MANIFEST_END, 0, ERR_OK, m_end, sizeof m_end, 0) != 0) return;

    /* 4. Stream files and eligible non-empty chunks */
    uint32_t total_chunks_streamed = 0;
    uint64_t client_transferred_bytes = 0;

    for (size_t i = 0; i < t->nfiles; i++) {
        if (g_stop) return;
        const fentry_t *fe = &t->files[i];
        char path[PATH_MAX];
        int plen = snprintf(path, sizeof path, "%s/%s", t->dir, fe->name);
        if (plen < 0 || (size_t)plen >= sizeof path) {
            send_error(&c, ERR_INTERNAL, "file path too long");
            return;
        }

        int ffd = open(path, O_RDONLY);
        if (ffd < 0) {
            LOG("%s: cannot open %s: %s", peer, path, strerror(errno));
            send_error(&c, ERR_FILE_NOT_FOUND, "cannot open file");
            return;
        }
        posix_fadvise(ffd, 0, 0, POSIX_FADV_SEQUENTIAL);

        /* Verify file size has not changed since scan */
        struct stat st;
        if (fstat(ffd, &st) != 0 || (uint64_t)st.st_size != fe->size) {
            close(ffd);
            LOG("%s: file size mismatch on %s (expected %llu, got %llu)",
                peer, path, (unsigned long long)fe->size, (unsigned long long)st.st_size);
            send_error(&c, ERR_INTERNAL, "file modified during transfer");
            return;
        }

        /* Send MSG_FILE_HEADER */
        uint16_t nlen = (uint16_t)strlen(fe->name);
        uint8_t fhdr[4 + 8 + 8 + SHA256_DIGEST_LEN + 2 + MAX_NAME];
        put32(fhdr + 0, (uint32_t)i);
        put64(fhdr + 4, fe->size);
        put64(fhdr + 12, 0); /* Start offset 0 in Case 1 */
        memcpy(fhdr + 20, fe->hash, SHA256_DIGEST_LEN);
        put16(fhdr + 20 + SHA256_DIGEST_LEN, nlen);
        memcpy(fhdr + 22 + SHA256_DIGEST_LEN, fe->name, nlen);

        uint32_t hlen = 22 + SHA256_DIGEST_LEN + nlen;
        if (conn_send(&c, MSG_FILE_HEADER, 0, ERR_OK, fhdr, hlen, 0) != 0) {
            close(ffd);
            return;
        }

        /* Stream Chunks and verify SHA-256 on the fly */
        sha256_ctx_t serve_sha;
        sha256_init(&serve_sha);

        uint64_t sent = 0;
        while (sent < fe->size) {
            if (g_stop) { close(ffd); return; }
            uint64_t left = fe->size - sent;
            uint32_t n = left < CHUNK_SIZE ? (uint32_t)left : CHUNK_SIZE;

            /* Failure decision happens at eligible non-empty chunk boundary */
            if (should_inject_fault(seed, total_chunks_streamed, peer, fe->name)) {
                atomic_fetch_add(&g_injected_failures, 1);
                close(ffd);
                /* Terminate abruptly without sending MSG_TRANSFER_DONE or completion */
                shutdown(fd, SHUT_RDWR);
                return;
            }

            uint8_t chunk_buf[CHUNK_SIZE];
            ssize_t rd = read(ffd, chunk_buf, n);
            if (rd != (ssize_t)n) {
                LOG("%s: read failure on %s", peer, path);
                close(ffd);
                send_error(&c, ERR_INTERNAL, "disk read error");
                return;
            }

            sha256_update(&serve_sha, chunk_buf, n);

            uint16_t flags = (sent + n == fe->size) ? FLAG_EOF_FILE : 0;
            if (conn_send(&c, MSG_DATA_CHUNK, flags, ERR_OK, chunk_buf, n, 0) != 0) {
                close(ffd);
                return;
            }

            sent += n;
            total_chunks_streamed++;
            client_transferred_bytes += n;
        }
        close(ffd);

        uint8_t actual_served_hash[SHA256_DIGEST_LEN];
        sha256_final(&serve_sha, actual_served_hash);
        if (memcmp(actual_served_hash, fe->hash, SHA256_DIGEST_LEN) != 0) {
            LOG("%s: file content mutated during streaming on %s", peer, path);
            send_error(&c, ERR_INTERNAL, "file content modified during streaming");
            return;
        }
    }

    /* 5. Send MSG_TRANSFER_DONE */
    uint8_t done[4 + 8 + SHA256_DIGEST_LEN];
    put32(done + 0, (uint32_t)t->nfiles);
    put64(done + 4, client_transferred_bytes);
    memcpy(done + 12, t->manifest_hash, SHA256_DIGEST_LEN);
    if (conn_send(&c, MSG_TRANSFER_DONE, 0, ERR_OK, done, sizeof done, 0) != 0) return;

    atomic_fetch_add(&g_total_clients, 1);
    atomic_fetch_add(&g_total_data_bytes, client_transferred_bytes);
    LOG("%s: successfully completed transfer of topic '%s' (%llu bytes in %u chunks)",
        peer, t->name, (unsigned long long)client_transferred_bytes, total_chunks_streamed);
}

typedef struct {
    int  fd;
    char peer[64];
} job_t;

static void *worker(void *arg)
{
    job_t j = *(job_t *)arg;
    free(arg);

    unsigned int seed = (unsigned int)(now_sec() * 1e6) ^ (uintptr_t)pthread_self() ^ (unsigned int)j.fd;
    handle_client(j.fd, j.peer, &seed);
    client_reg_del(j.fd);
    return NULL;
}

static void handle_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <port> <topics_root_dir> <failure_probability>\n", argv[0]);
        return 2;
    }

    char *end;
    long port = strtol(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port '%s': must be between 1 and 65535\n", argv[1]);
        return 2;
    }

    double prob = strtod(argv[3], &end);
    if (argv[3] == end || *end != '\0' || isnan(prob) || isinf(prob) || prob < 0.0 || prob > 1.0) {
        fprintf(stderr, "invalid failure probability '%s': must be a finite float in [0.0, 1.0]\n", argv[3]);
        return 2;
    }
    g_failure_prob = prob;

    client_reg_init();

    if (load_topics(argv[2]) != 0) {
        pthread_mutex_destroy(&g_client_reg.lock);
        pthread_cond_destroy(&g_client_reg.cond);
        return 1;
    }
    if (g_ntopics == 0) {
        fprintf(stderr, "no topic directories found in '%s'\n", argv[2]);
        pthread_mutex_destroy(&g_client_reg.lock);
        pthread_cond_destroy(&g_client_reg.cond);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("socket");
        return 1;
    }

    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("bind");
        close(lfd);
        return 1;
    }
    if (listen(lfd, 512) != 0) {
        perror("listen");
        close(lfd);
        return 1;
    }

    LOG("listening on port %ld, root='%s', failure_prob=%.4f", port, argv[2], g_failure_prob);

    while (!g_stop) {
        struct sockaddr_in pa;
        socklen_t plen = sizeof pa;
        int cfd = accept(lfd, (struct sockaddr *)&pa, &plen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (g_stop) break;
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip);
        char peer[64];
        snprintf(peer, sizeof peer, "%s:%u", ip, ntohs(pa.sin_port));

        if (client_reg_add(cfd) != 0) {
            LOG("client registry full (%d active clients); rejecting connection from %s",
                MAX_ACTIVE_CLIENTS, peer);
            close(cfd);
            continue;
        }

        job_t *j = malloc(sizeof *j);
        if (!j) {
            LOG("out of memory creating job for %s", peer);
            client_reg_del(cfd);
            continue;
        }
        j->fd = cfd;
        snprintf(j->peer, sizeof j->peer, "%s", peer);

        pthread_t th;
        if (pthread_create(&th, NULL, worker, j) != 0) {
            LOG("pthread_create failed for %s", peer);
            client_reg_del(cfd);
            free(j);
            continue;
        }
        pthread_detach(th);
    }

    LOG("shutting down server...");
    close(lfd);
    client_reg_interrupt_all();
    int remaining = client_reg_wait_empty(5000);
    LOG("server served %llu clients, %llu data bytes, %llu injected failures",
        (unsigned long long)g_total_clients,
        (unsigned long long)g_total_data_bytes,
        (unsigned long long)g_injected_failures);

    if (remaining == 0) {
        LOG("Server stopped cleanly");
        for (size_t i = 0; i < g_ntopics; i++) {
            free(g_topics[i].files);
        }
        free(g_topics);
        g_topics = NULL;
        pthread_mutex_destroy(&g_client_reg.lock);
        pthread_cond_destroy(&g_client_reg.cond);
    } else {
        LOG("shutdown timeout: %d active worker(s) still running; preserving registry and topic memory to prevent use-after-free",
            remaining);
    }
    return 0;
}
