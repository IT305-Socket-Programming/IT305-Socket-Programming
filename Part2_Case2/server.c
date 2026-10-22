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
    fprintf(stdout, "[%s] [Part2_Case2] " fmt "\n", _ts, ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)

#ifndef MAX_ACTIVE_CLIENTS
#define MAX_ACTIVE_CLIENTS 1024
#endif

#define MAX_SESSIONS 512

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
static char     g_sessions_dir[PATH_MAX] = {0};

/* ============================================================================
 * Session Management & Persistent Checkpoint Storage
 * ============================================================================ */

typedef struct {
    char            session_id[SESSION_ID_LEN + 1];
    char            topic[TOPIC_FIELD];
    uint8_t         manifest_hash[SHA256_DIGEST_LEN];
    uint32_t        total_files;
    uint64_t        total_bytes;
    uint64_t       *committed_offsets; /* array of size total_files */
    int             state;              /* 0: ACTIVE, 1: COMPLETED */
    time_t          created_at;
    time_t          last_active_at;
    pthread_mutex_t lock;
} session_t;

static session_t *g_sessions[MAX_SESSIONS];
static size_t     g_nsessions = 0;
static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;

static void generate_session_id(char *out, size_t len)
{
    static atomic_uint seq = 0;
    unsigned int s = (unsigned int)(now_sec() * 1e6) ^ (uintptr_t)pthread_self() ^ atomic_fetch_add(&seq, 1);
    int fd = open("/dev/urandom", O_RDONLY);
    uint8_t rnd[16];
    if (fd >= 0) {
        if (read(fd, rnd, sizeof rnd) != (ssize_t)sizeof rnd) {
            for (size_t i = 0; i < sizeof rnd; i++) rnd[i] = (uint8_t)rand_r(&s);
        }
        close(fd);
    } else {
        for (size_t i = 0; i < sizeof rnd; i++) rnd[i] = (uint8_t)rand_r(&s);
    }

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16 && (i * 2 + 1) < len; i++) {
        out[i * 2]     = hex[(rnd[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[rnd[i] & 0x0f];
    }
    out[32] = '\0';
}

static int persist_session_locked(session_t *s)
{
    if (g_sessions_dir[0] == '\0') return -1;
    char path[PATH_MAX + 128];
    char tmp[PATH_MAX + 128];
    if (snprintf(path, sizeof path, "%s/session_%s.meta", g_sessions_dir, s->session_id) >= (int)sizeof path ||
        snprintf(tmp, sizeof tmp, "%s/session_%s.tmp", g_sessions_dir, s->session_id) >= (int)sizeof tmp) {
        return -1;
    }

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    uint8_t hdr[32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 4 + 8 + 4];
    memcpy(hdr, s->session_id, 32);
    memcpy(hdr + 32, s->topic, TOPIC_FIELD);
    memcpy(hdr + 32 + TOPIC_FIELD, s->manifest_hash, SHA256_DIGEST_LEN);
    put32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN, s->total_files);
    put64(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 4, s->total_bytes);
    put32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 12, (uint32_t)s->state);

    if (write(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) { close(fd); unlink(tmp); return -1; }

    for (uint32_t i = 0; i < s->total_files; i++) {
        uint8_t obuf[8];
        put64(obuf, s->committed_offsets[i]);
        if (write(fd, obuf, sizeof obuf) != (ssize_t)sizeof obuf) { close(fd); unlink(tmp); return -1; }
    }

    fdatasync(fd);
    close(fd);
    return rename(tmp, path);
}

static session_t *find_or_load_session(const char *session_id)
{
    pthread_mutex_lock(&g_sessions_lock);
    for (size_t i = 0; i < g_nsessions; i++) {
        if (strcmp(g_sessions[i]->session_id, session_id) == 0) {
            pthread_mutex_unlock(&g_sessions_lock);
            return g_sessions[i];
        }
    }

    /* Try loading from disk */
    char path[PATH_MAX + 128];
    if (snprintf(path, sizeof path, "%s/session_%s.meta", g_sessions_dir, session_id) >= (int)sizeof path) {
        pthread_mutex_unlock(&g_sessions_lock);
        return NULL;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        pthread_mutex_unlock(&g_sessions_lock);
        return NULL;
    }

    uint8_t hdr[32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 4 + 8 + 4];
    if (read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) { close(fd); pthread_mutex_unlock(&g_sessions_lock); return NULL; }

    uint32_t total_files = get32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN);
    uint64_t total_bytes = get64(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 4);
    uint32_t state       = get32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 12);

    session_t *s = calloc(1, sizeof(session_t));
    if (!s) { close(fd); pthread_mutex_unlock(&g_sessions_lock); return NULL; }

    memcpy(s->session_id, hdr, 32);
    s->session_id[32] = '\0';
    memcpy(s->topic, hdr + 32, TOPIC_FIELD);
    s->topic[TOPIC_FIELD - 1] = '\0';
    memcpy(s->manifest_hash, hdr + 32 + TOPIC_FIELD, SHA256_DIGEST_LEN);
    s->total_files = total_files;
    s->total_bytes = total_bytes;
    s->state = (int)state;
    s->created_at = time(NULL);
    s->last_active_at = time(NULL);
    pthread_mutex_init(&s->lock, NULL);

    s->committed_offsets = calloc(total_files, sizeof(uint64_t));
    for (uint32_t i = 0; i < total_files; i++) {
        uint8_t obuf[8];
        if (read(fd, obuf, sizeof obuf) == (ssize_t)sizeof obuf) {
            s->committed_offsets[i] = get64(obuf);
        }
    }
    close(fd);

    if (g_nsessions < MAX_SESSIONS) {
        g_sessions[g_nsessions++] = s;
    }
    pthread_mutex_unlock(&g_sessions_lock);
    return s;
}

static session_t *create_session(const topic_t *t)
{
    session_t *s = calloc(1, sizeof(session_t));
    if (!s) return NULL;

    generate_session_id(s->session_id, sizeof s->session_id);
    snprintf(s->topic, sizeof s->topic, "%s", t->name);
    memcpy(s->manifest_hash, t->manifest_hash, SHA256_DIGEST_LEN);
    s->total_files = (uint32_t)t->nfiles;
    s->total_bytes = t->total_bytes;
    s->committed_offsets = calloc(s->total_files, sizeof(uint64_t));
    s->state = 0; /* ACTIVE */
    s->created_at = time(NULL);
    s->last_active_at = time(NULL);
    pthread_mutex_init(&s->lock, NULL);

    pthread_mutex_lock(&g_sessions_lock);
    if (g_nsessions < MAX_SESSIONS) {
        g_sessions[g_nsessions++] = s;
    }
    persist_session_locked(s);
    pthread_mutex_unlock(&g_sessions_lock);
    return s;
}

/* ============================================================================
 * Connection Registry & Server Lifecycle
 * ============================================================================ */

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
    for (int i = 0; i < MAX_ACTIVE_CLIENTS; i++) g_client_reg.fds[i] = -1;
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

static atomic_ullong g_total_clients = 0;
static atomic_ullong g_total_data_bytes = 0;
static atomic_ullong g_injected_failures = 0;

/* Deterministic Alphabetical Sort */
static int compare_fentries(const void *a, const void *b)
{
    const fentry_t *fa = (const fentry_t *)a;
    const fentry_t *fb = (const fentry_t *)b;
    return strcmp(fa->name, fb->name);
}

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

        if (sha256_file(path, fe->hash, NULL) != 0) {
            LOG("failed to compute hash for %s", path);
            continue;
        }

        t->total_bytes += fe->size;
        t->nfiles++;
    }
    closedir(d);

    qsort(t->files, t->nfiles, sizeof(fentry_t), compare_fentries);

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

    session_t *session = NULL;
    const topic_t *topic = NULL;
    int is_resumed = 0;

    if (h.type == MSG_SESSION_REQ) {
        char req_sess_id[SESSION_ID_LEN + 1];
        memcpy(req_sess_id, pl, SESSION_ID_LEN);
        req_sess_id[SESSION_ID_LEN] = '\0';

        char req_topic[TOPIC_FIELD] = {0};
        uint16_t tlen = get16(pl + SESSION_ID_LEN);
        if (tlen >= sizeof req_topic) tlen = sizeof req_topic - 1;
        memcpy(req_topic, pl + SESSION_ID_LEN + 2, tlen);
        req_topic[tlen] = '\0';

        if (req_sess_id[0] != '\0' && req_sess_id[0] != '0') {
            session = find_or_load_session(req_sess_id);
            if (!session) {
                LOG("%s: requested session '%s' not found", peer, req_sess_id);
                send_error(&c, ERR_SESSION_NOT_FOUND, "session not found");
                return;
            }
            topic = find_topic(session->topic);
            if (!topic || memcmp(session->manifest_hash, topic->manifest_hash, SHA256_DIGEST_LEN) != 0) {
                LOG("%s: topic '%s' content changed on server since session created", peer, session->topic);
                send_error(&c, ERR_CHECKSUM_MISMATCH, "server topic content changed");
                return;
            }
            is_resumed = 1;
            LOG("%s: successfully resumed session '%s' for topic '%s'", peer, session->session_id, session->topic);
        } else {
            topic = find_topic(req_topic);
            if (!topic) {
                LOG("%s: requested topic '%s' not found", peer, req_topic);
                send_error(&c, ERR_TOPIC_NOT_FOUND, "topic not found");
                return;
            }
            session = create_session(topic);
            if (!session) {
                send_error(&c, ERR_INTERNAL, "failed to create session");
                return;
            }
            LOG("%s: created new session '%s' for topic '%s'", peer, session->session_id, topic->name);
        }
    } else if (h.type == MSG_GET_REQ) {
        char req_topic[TOPIC_FIELD];
        size_t tlen = h.len < sizeof req_topic - 1 ? h.len : sizeof req_topic - 1;
        memcpy(req_topic, pl, tlen);
        req_topic[tlen] = '\0';
        while (tlen > 0 && (req_topic[tlen - 1] == ' ' || req_topic[tlen - 1] == '\r' || req_topic[tlen - 1] == '\n')) {
            req_topic[--tlen] = '\0';
        }
        topic = find_topic(req_topic);
        if (!topic) {
            LOG("%s: topic '%s' not found", peer, req_topic);
            send_error(&c, ERR_TOPIC_NOT_FOUND, "topic not found");
            return;
        }
        session = create_session(topic);
        if (!session) {
            send_error(&c, ERR_INTERNAL, "failed to create session");
            return;
        }
        LOG("%s: created new session '%s' via MSG_GET_REQ for '%s'", peer, session->session_id, topic->name);
    } else {
        LOG("%s: unexpected message 0x%02x during handshake", peer, h.type);
        send_error(&c, ERR_BAD_REQUEST, "expected MSG_SESSION_REQ or MSG_GET_REQ");
        return;
    }

    /* Send MSG_SESSION_RESP */
    uint8_t s_resp[SESSION_ID_LEN + 1 + 4 + 8 + SHA256_DIGEST_LEN];
    memcpy(s_resp, session->session_id, SESSION_ID_LEN);
    s_resp[SESSION_ID_LEN] = (uint8_t)(is_resumed ? (session->state == 1 ? 2 : 1) : 0);
    put32(s_resp + SESSION_ID_LEN + 1, (uint32_t)topic->nfiles);
    put64(s_resp + SESSION_ID_LEN + 5, topic->total_bytes);
    memcpy(s_resp + SESSION_ID_LEN + 13, topic->manifest_hash, SHA256_DIGEST_LEN);
    if (conn_send(&c, MSG_SESSION_RESP, 0, ERR_OK, s_resp, sizeof s_resp, 0) != 0) return;

    /* If resumed, send MSG_RESUME_RESP containing authoritative committed checkpoints */
    if (is_resumed) {
        pthread_mutex_lock(&session->lock);
        size_t rlen = SESSION_ID_LEN + 4 + SHA256_DIGEST_LEN + 4 + (session->total_files * 8);
        uint8_t *r_buf = malloc(rlen);
        if (!r_buf) {
            pthread_mutex_unlock(&session->lock);
            send_error(&c, ERR_INTERNAL, "out of memory");
            return;
        }
        memcpy(r_buf, session->session_id, SESSION_ID_LEN);
        put32(r_buf + SESSION_ID_LEN, ERR_OK);
        memcpy(r_buf + SESSION_ID_LEN + 4, session->manifest_hash, SHA256_DIGEST_LEN);
        put32(r_buf + SESSION_ID_LEN + 4 + SHA256_DIGEST_LEN, session->total_files);
        for (uint32_t i = 0; i < session->total_files; i++) {
            put64(r_buf + SESSION_ID_LEN + 8 + SHA256_DIGEST_LEN + (i * 8), session->committed_offsets[i]);
        }
        pthread_mutex_unlock(&session->lock);

        if (conn_send(&c, MSG_RESUME_RESP, 0, ERR_OK, r_buf, (uint32_t)rlen, 0) != 0) {
            free(r_buf);
            return;
        }
        free(r_buf);
    } else {
        /* Send Manifest for new session */
        uint8_t m_start[4 + 8 + SHA256_DIGEST_LEN];
        put32(m_start + 0, (uint32_t)topic->nfiles);
        put64(m_start + 4, topic->total_bytes);
        memcpy(m_start + 12, topic->manifest_hash, SHA256_DIGEST_LEN);
        if (conn_send(&c, MSG_MANIFEST_START, 0, ERR_OK, m_start, sizeof m_start, 0) != 0) return;

        for (size_t i = 0; i < topic->nfiles; i++) {
            if (g_stop) return;
            const fentry_t *fe = &topic->files[i];
            uint16_t nlen = (uint16_t)strlen(fe->name);
            uint8_t entry[4 + 8 + SHA256_DIGEST_LEN + 2 + MAX_NAME];
            put32(entry + 0, (uint32_t)i);
            put64(entry + 4, fe->size);
            memcpy(entry + 12, fe->hash, SHA256_DIGEST_LEN);
            put16(entry + 12 + SHA256_DIGEST_LEN, nlen);
            memcpy(entry + 14 + SHA256_DIGEST_LEN, fe->name, nlen);
            if (conn_send(&c, MSG_MANIFEST_ENTRY, 0, ERR_OK, entry, 14 + SHA256_DIGEST_LEN + nlen, 0) != 0) return;
        }

        uint8_t m_end[8];
        put32(m_end + 0, (uint32_t)topic->nfiles);
        put32(m_end + 4, ERR_OK);
        if (conn_send(&c, MSG_MANIFEST_END, 0, ERR_OK, m_end, sizeof m_end, 0) != 0) return;
    }

    /* Stream remaining uncommitted ranges with per-chunk ACK confirmation */
    uint32_t chunk_counter = 0;
    uint64_t session_streamed_bytes = 0;

    for (size_t i = 0; i < topic->nfiles; i++) {
        if (g_stop) return;
        const fentry_t *fe = &topic->files[i];

        pthread_mutex_lock(&session->lock);
        uint64_t comm_off = session->committed_offsets[i];
        pthread_mutex_unlock(&session->lock);

        /* Skip already fully committed files */
        if (comm_off >= fe->size && fe->size > 0) {
            continue;
        }

        char path[PATH_MAX];
        int plen = snprintf(path, sizeof path, "%s/%s", topic->dir, fe->name);
        if (plen < 0 || (size_t)plen >= sizeof path) {
            send_error(&c, ERR_INTERNAL, "path too long");
            return;
        }

        int ffd = open(path, O_RDONLY);
        if (ffd < 0) {
            send_error(&c, ERR_FILE_NOT_FOUND, "cannot open file");
            return;
        }
        posix_fadvise(ffd, 0, 0, POSIX_FADV_SEQUENTIAL);

        /* Send MSG_FILE_HEADER with authoritative committed start offset */
        uint16_t nlen = (uint16_t)strlen(fe->name);
        uint8_t fhdr[4 + 8 + 8 + SHA256_DIGEST_LEN + 2 + MAX_NAME];
        put32(fhdr + 0, (uint32_t)i);
        put64(fhdr + 4, fe->size);
        put64(fhdr + 12, comm_off);
        memcpy(fhdr + 20, fe->hash, SHA256_DIGEST_LEN);
        put16(fhdr + 20 + SHA256_DIGEST_LEN, nlen);
        memcpy(fhdr + 22 + SHA256_DIGEST_LEN, fe->name, nlen);

        uint32_t hlen = 22 + SHA256_DIGEST_LEN + nlen;
        if (conn_send(&c, MSG_FILE_HEADER, 0, ERR_OK, fhdr, hlen, 0) != 0) {
            close(ffd);
            return;
        }

        if (comm_off > 0) {
            if (lseek(ffd, (off_t)comm_off, SEEK_SET) < 0) {
                close(ffd);
                send_error(&c, ERR_INTERNAL, "seek failed");
                return;
            }
        }

        uint64_t sent = comm_off;
        while (sent < fe->size) {
            if (g_stop) { close(ffd); return; }
            uint64_t left = fe->size - sent;
            uint32_t n = left < CHUNK_SIZE ? (uint32_t)left : CHUNK_SIZE;

            /* Check failure decision at non-empty eligible chunk boundary */
            if (should_inject_fault(seed, chunk_counter, peer, fe->name)) {
                atomic_fetch_add(&g_injected_failures, 1);
                close(ffd);
                shutdown(fd, SHUT_RDWR);
                return;
            }

            uint8_t chunk_buf[CHUNK_SIZE];
            ssize_t rd = read(ffd, chunk_buf, n);
            if (rd != (ssize_t)n) {
                close(ffd);
                send_error(&c, ERR_INTERNAL, "disk read failure");
                return;
            }

            uint16_t flags = (sent + n == fe->size) ? FLAG_EOF_FILE : 0;
            if (conn_send(&c, MSG_DATA_CHUNK, flags, ERR_OK, chunk_buf, n, 0) != 0) {
                close(ffd);
                return;
            }

            /* Wait for explicit client Checkpoint ACK before advancing committed state */
            proto_hdr_t ack_h;
            uint8_t ack_pl[128];
            if (conn_recv(&c, &ack_h, ack_pl) != 0) {
                /* Connection interrupted before ACK was received */
                close(ffd);
                return;
            }

            if (ack_h.type != MSG_CHECKPOINT_ACK) {
                close(ffd);
                send_error(&c, ERR_PROTOCOL, "expected MSG_CHECKPOINT_ACK");
                return;
            }

            uint32_t ack_file_idx = get32(ack_pl + SESSION_ID_LEN);
            uint64_t acked_off = get64(ack_pl + SESSION_ID_LEN + 4);

            if (ack_file_idx != (uint32_t)i || acked_off != sent + n) {
                close(ffd);
                send_error(&c, ERR_PROTOCOL, "invalid ACK offset");
                return;
            }

            /* Durably persist committed checkpoint on server */
            pthread_mutex_lock(&session->lock);
            session->committed_offsets[i] = acked_off;
            session->last_active_at = time(NULL);
            persist_session_locked(session);
            pthread_mutex_unlock(&session->lock);

            /* Confirm commit to client */
            uint8_t c_pl[SESSION_ID_LEN + 4 + 8];
            memcpy(c_pl, session->session_id, SESSION_ID_LEN);
            put32(c_pl + SESSION_ID_LEN, (uint32_t)i);
            put64(c_pl + SESSION_ID_LEN + 4, acked_off);
            if (conn_send(&c, MSG_CHECKPOINT_COMMIT, 0, ERR_OK, c_pl, sizeof c_pl, 0) != 0) {
                close(ffd);
                return;
            }

            sent += n;
            chunk_counter++;
            session_streamed_bytes += n;
        }
        close(ffd);
    }

    /* All files reached committed offset == size. Finalize session */
    pthread_mutex_lock(&session->lock);
    session->state = 1; /* COMPLETED */
    persist_session_locked(session);
    pthread_mutex_unlock(&session->lock);

    uint8_t done[4 + 8 + SHA256_DIGEST_LEN];
    put32(done + 0, (uint32_t)topic->nfiles);
    put64(done + 4, topic->total_bytes);
    memcpy(done + 12, topic->manifest_hash, SHA256_DIGEST_LEN);
    conn_send(&c, MSG_TRANSFER_DONE, 0, ERR_OK, done, sizeof done, 0);

    atomic_fetch_add(&g_total_clients, 1);
    atomic_fetch_add(&g_total_data_bytes, session_streamed_bytes);
    LOG("%s: session '%s' transfer completed (%llu bytes streamed in this attempt)",
        peer, session->session_id, (unsigned long long)session_streamed_bytes);
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
        fprintf(stderr, "invalid failure probability '%s': must be in [0.0, 1.0]\n", argv[3]);
        return 2;
    }
    g_failure_prob = prob;

    /* Setup session persistence directory */
    const char *sess_env = getenv("IT305_SESSION_DIR");
    if (sess_env && sess_env[0] != '\0') {
        snprintf(g_sessions_dir, sizeof g_sessions_dir, "%s", sess_env);
    } else {
        snprintf(g_sessions_dir, sizeof g_sessions_dir, "%s/.sessions", argv[2]);
    }
    mkdir_p(g_sessions_dir);

    client_reg_init();

    if (load_topics(argv[2]) != 0) {
        pthread_mutex_destroy(&g_client_reg.lock);
        pthread_cond_destroy(&g_client_reg.cond);
        return 1;
    }
    if (g_ntopics == 0) {
        fprintf(stderr, "no topics found in '%s'\n", argv[2]);
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
    if (lfd < 0) { perror("socket"); return 1; }

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

    LOG("listening on port %ld, root='%s', sessions_dir='%s', failure_prob=%.4f",
        port, argv[2], g_sessions_dir, g_failure_prob);

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
            LOG("out of memory for job %s", peer);
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
        for (size_t i = 0; i < g_ntopics; i++) free(g_topics[i].files);
        free(g_topics);
        g_topics = NULL;

        pthread_mutex_lock(&g_sessions_lock);
        for (size_t i = 0; i < g_nsessions; i++) {
            free(g_sessions[i]->committed_offsets);
            pthread_mutex_destroy(&g_sessions[i]->lock);
            free(g_sessions[i]);
        }
        pthread_mutex_unlock(&g_sessions_lock);

        pthread_mutex_destroy(&g_client_reg.lock);
        pthread_cond_destroy(&g_client_reg.cond);
    } else {
        LOG("shutdown timeout: %d active worker(s) still running; preserving memory", remaining);
    }
    return 0;
}
