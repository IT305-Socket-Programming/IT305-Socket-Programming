#include "common.h"
#include <stdatomic.h>

#define MAX_ACTIVE_CLIENTS  256
#define MAX_SESSIONS        1024

#define LOG(fmt, ...) do { \
    time_t _t = time(NULL); \
    struct tm _tm; \
    localtime_r(&_t, &_tm); \
    char _ts[32]; \
    strftime(_ts, sizeof _ts, "%Y-%m-%d %H:%M:%S", &_tm); \
    fprintf(stderr, "[%s] [Part2_Case2_Enhanced] " fmt "\n", _ts, ##__VA_ARGS__); \
} while (0)

typedef struct {
    uint32_t file_idx;
    uint64_t offset;
    uint32_t length;
    uint8_t  status; /* 0 = UNASSIGNED, 1 = IN_FLIGHT, 2 = COMMITTED */
    int      stream_id;
} block_desc_t;

typedef struct {
    char            session_id[SESSION_ID_LEN + 1];
    char            topic[TOPIC_FIELD];
    uint8_t         manifest_hash[SHA256_DIGEST_LEN];
    uint32_t        total_files;
    uint64_t        total_bytes;
    uint32_t        total_blocks;
    block_desc_t   *blocks;
    uint8_t        *bitmask;
    size_t          bitmask_bytes;
    uint32_t        committed_blocks_count;
    int             assigned_streams;
    int             active_streams;
    int             state; /* 0 = ACTIVE, 1 = COMPLETED */
    time_t          created_at;
    time_t          last_active_at;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} enhanced_session_t;

static enhanced_session_t *g_sessions[MAX_SESSIONS];
static size_t              g_nsessions = 0;
static pthread_mutex_t     g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static char                g_sessions_dir[PATH_MAX];

static double              g_failure_prob = 0.0;
static volatile sig_atomic_t g_stop = 0;

static atomic_uint_fast64_t g_total_data_bytes = 0;
static atomic_uint_fast64_t g_injected_failures = 0;

static topic_t *g_topics = NULL;
static size_t   g_ntopics = 0;

static const struct { const char *alias; const char *canonical; } ALIASES[] = {
    { "dog", "cane" },
    { "cat", "gatto" },
    { "bird", "uccello" }
};

static void generate_session_id(char *out, size_t out_len)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t rnd[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, rnd, sizeof rnd) != (ssize_t)sizeof rnd) {
            for (size_t i = 0; i < sizeof rnd; i++) rnd[i] = (uint8_t)rand();
        }
        close(fd);
    } else {
        for (size_t i = 0; i < sizeof rnd; i++) rnd[i] = (uint8_t)rand();
    }
    for (size_t i = 0; i < 16 && (i * 2 + 2) < out_len; i++) {
        out[i * 2 + 0] = hex[(rnd[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[rnd[i] & 0x0f];
    }
    out[32] = '\0';
}

static int persist_session_locked(enhanced_session_t *s)
{
    char tmp[PATH_MAX + 128];
    char path[PATH_MAX + 128];
    snprintf(tmp, sizeof tmp, "%s/session_%s.tmp.%d", g_sessions_dir, s->session_id, getpid());
    snprintf(path, sizeof path, "%s/session_%s.meta", g_sessions_dir, s->session_id);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    uint8_t hdr[32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 4 + 8 + 4 + 4 + 4];
    memcpy(hdr, s->session_id, 32);
    memcpy(hdr + 32, s->topic, TOPIC_FIELD);
    memcpy(hdr + 32 + TOPIC_FIELD, s->manifest_hash, SHA256_DIGEST_LEN);
    put32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN, s->total_files);
    put64(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 4, s->total_bytes);
    put32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 12, s->total_blocks);
    put32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 16, s->committed_blocks_count);
    put32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 20, (uint32_t)s->state);

    if (write(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) { close(fd); unlink(tmp); return -1; }
    if (write(fd, s->bitmask, s->bitmask_bytes) != (ssize_t)s->bitmask_bytes) { close(fd); unlink(tmp); return -1; }

    fdatasync(fd);
    close(fd);
    return rename(tmp, path);
}

static const topic_t *find_topic(const char *name);

static enhanced_session_t *create_session(const topic_t *t, int streams)
{
    enhanced_session_t *s = calloc(1, sizeof(enhanced_session_t));
    if (!s) return NULL;

    generate_session_id(s->session_id, sizeof s->session_id);
    snprintf(s->topic, sizeof s->topic, "%s", t->name);
    memcpy(s->manifest_hash, t->manifest_hash, SHA256_DIGEST_LEN);
    s->total_files = (uint32_t)t->nfiles;
    s->total_bytes = t->total_bytes;
    s->assigned_streams = streams;
    s->state = 0; /* ACTIVE */
    s->created_at = time(NULL);
    s->last_active_at = time(NULL);
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);

    /* Compute total blocks across all files */
    uint32_t tb = 0;
    for (size_t i = 0; i < t->nfiles; i++) {
        uint64_t fsz = t->files[i].size;
        uint32_t b = (fsz == 0) ? 0 : (uint32_t)((fsz + CHUNK_SIZE - 1) / CHUNK_SIZE);
        tb += b;
    }
    s->total_blocks = tb;
    s->bitmask_bytes = (tb + 7) / 8;
    if (s->bitmask_bytes == 0) s->bitmask_bytes = 1;
    s->bitmask = calloc(s->bitmask_bytes, 1);

    if (tb > 0) {
        s->blocks = calloc(tb, sizeof(block_desc_t));
        uint32_t bidx = 0;
        for (size_t i = 0; i < t->nfiles; i++) {
            uint64_t fsz = t->files[i].size;
            uint64_t off = 0;
            while (off < fsz) {
                uint32_t len = (fsz - off < CHUNK_SIZE) ? (uint32_t)(fsz - off) : CHUNK_SIZE;
                s->blocks[bidx].file_idx = (uint32_t)i;
                s->blocks[bidx].offset = off;
                s->blocks[bidx].length = len;
                s->blocks[bidx].status = 0; /* UNASSIGNED */
                s->blocks[bidx].stream_id = -1;
                bidx++;
                off += len;
            }
        }
    }

    pthread_mutex_lock(&g_sessions_lock);
    if (g_nsessions < MAX_SESSIONS) {
        g_sessions[g_nsessions++] = s;
    }
    persist_session_locked(s);
    pthread_mutex_unlock(&g_sessions_lock);
    return s;
}

static enhanced_session_t *find_or_load_session(const char *session_id)
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

    uint8_t hdr[32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 4 + 8 + 4 + 4 + 4];
    if (read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) {
        close(fd);
        pthread_mutex_unlock(&g_sessions_lock);
        return NULL;
    }

    char s_topic[TOPIC_FIELD];
    memcpy(s_topic, hdr + 32, TOPIC_FIELD);
    s_topic[TOPIC_FIELD - 1] = '\0';

    const topic_t *t = find_topic(s_topic);
    if (!t) { close(fd); pthread_mutex_unlock(&g_sessions_lock); return NULL; }

    enhanced_session_t *s = calloc(1, sizeof(enhanced_session_t));
    if (!s) { close(fd); pthread_mutex_unlock(&g_sessions_lock); return NULL; }

    memcpy(s->session_id, hdr, 32);
    s->session_id[32] = '\0';
    snprintf(s->topic, sizeof s->topic, "%s", s_topic);
    memcpy(s->manifest_hash, hdr + 32 + TOPIC_FIELD, SHA256_DIGEST_LEN);
    s->total_files = get32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN);
    s->total_bytes = get64(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 4);
    s->total_blocks = get32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 12);
    s->committed_blocks_count = get32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 16);
    s->state = (int)get32(hdr + 32 + TOPIC_FIELD + SHA256_DIGEST_LEN + 20);
    s->assigned_streams = DEFAULT_STREAMS;
    s->created_at = time(NULL);
    s->last_active_at = time(NULL);
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);

    s->bitmask_bytes = (s->total_blocks + 7) / 8;
    if (s->bitmask_bytes == 0) s->bitmask_bytes = 1;
    s->bitmask = calloc(s->bitmask_bytes, 1);
    if (read(fd, s->bitmask, s->bitmask_bytes) != (ssize_t)s->bitmask_bytes) {
        /* Short read on bitmask */
    }
    close(fd);

    /* Construct block descriptors matching topic files */
    if (s->total_blocks > 0) {
        s->blocks = calloc(s->total_blocks, sizeof(block_desc_t));
        uint32_t bidx = 0;
        for (size_t i = 0; i < t->nfiles && bidx < s->total_blocks; i++) {
            uint64_t fsz = t->files[i].size;
            uint64_t off = 0;
            while (off < fsz && bidx < s->total_blocks) {
                uint32_t len = (fsz - off < CHUNK_SIZE) ? (uint32_t)(fsz - off) : CHUNK_SIZE;
                s->blocks[bidx].file_idx = (uint32_t)i;
                s->blocks[bidx].offset = off;
                s->blocks[bidx].length = len;
                /* Check bitmask for committed state */
                int is_comm = (s->bitmask[bidx / 8] >> (bidx % 8)) & 1;
                s->blocks[bidx].status = is_comm ? 2 : 0;
                s->blocks[bidx].stream_id = -1;
                bidx++;
                off += len;
            }
        }
    }

    if (g_nsessions < MAX_SESSIONS) {
        g_sessions[g_nsessions++] = s;
    }
    pthread_mutex_unlock(&g_sessions_lock);
    return s;
}

/* ============================================================================
 * Topic Scanning & Indexing
 * ============================================================================ */

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
    t->files = calloc(cap, sizeof(fentry_t));
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

/* ============================================================================
 * Connection Registry
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
    for (int i = 0; i < MAX_ACTIVE_CLIENTS; i++) {
        if (g_client_reg.fds[i] == fd) {
            g_client_reg.fds[i] = -1;
            g_client_reg.active_workers--;
            break;
        }
    }
    pthread_cond_broadcast(&g_client_reg.cond);
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

static void client_reg_wait_empty(double timeout_sec)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)timeout_sec;

    pthread_mutex_lock(&g_client_reg.lock);
    while (g_client_reg.active_workers > 0) {
        if (pthread_cond_timedwait(&g_client_reg.cond, &g_client_reg.lock, &ts) == ETIMEDOUT) {
            break;
        }
    }
    pthread_mutex_unlock(&g_client_reg.lock);
}

/* ============================================================================
 * Client Handling: Control Connections & Worker Streams
 * ============================================================================ */

static void handle_control_connection(conn_t *c, proto_hdr_t *first_h, uint8_t *first_pl, const char *peer)
{
    (void)first_h;
    char req_sess_id[SESSION_ID_LEN + 1];
    memcpy(req_sess_id, first_pl, SESSION_ID_LEN);
    req_sess_id[SESSION_ID_LEN] = '\0';

    uint16_t req_streams = get16(first_pl + SESSION_ID_LEN);
    if (req_streams < 1) req_streams = 1;
    if (req_streams > MAX_STREAMS) req_streams = MAX_STREAMS;

    uint16_t tlen = get16(first_pl + SESSION_ID_LEN + 2);
    char req_topic[TOPIC_FIELD] = {0};
    if (tlen >= sizeof req_topic) tlen = sizeof req_topic - 1;
    memcpy(req_topic, first_pl + SESSION_ID_LEN + 4, tlen);
    req_topic[tlen] = '\0';

    enhanced_session_t *session = NULL;
    const topic_t *topic = NULL;
    int is_resumed = 0;

    if (req_sess_id[0] != '\0' && req_sess_id[0] != '0') {
        session = find_or_load_session(req_sess_id);
        if (!session) {
            LOG("%s: requested session '%s' not found", peer, req_sess_id);
            send_error(c, ERR_SESSION_NOT_FOUND, "session not found");
            return;
        }
        topic = find_topic(session->topic);
        if (!topic || memcmp(session->manifest_hash, topic->manifest_hash, SHA256_DIGEST_LEN) != 0) {
            LOG("%s: topic content changed for session '%s'", peer, session->session_id);
            send_error(c, ERR_CHECKSUM_MISMATCH, "server topic content changed");
            return;
        }
        is_resumed = 1;
        LOG("%s: successfully resumed enhanced session '%s' for topic '%s'", peer, session->session_id, session->topic);
    } else {
        topic = find_topic(req_topic);
        if (!topic) {
            LOG("%s: requested topic '%s' not found", peer, req_topic);
            send_error(c, ERR_TOPIC_NOT_FOUND, "topic not found");
            return;
        }
        session = create_session(topic, (int)req_streams);
        if (!session) {
            send_error(c, ERR_INTERNAL, "failed to create session");
            return;
        }
        LOG("%s: created new enhanced session '%s' for topic '%s' (%d streams)",
            peer, session->session_id, topic->name, req_streams);
    }

    /* Send MSG_ENHANCED_SESSION_RESP */
    uint8_t s_resp[SESSION_ID_LEN + 2 + 1 + 4 + 8 + SHA256_DIGEST_LEN];
    memcpy(s_resp, session->session_id, SESSION_ID_LEN);
    put16(s_resp + SESSION_ID_LEN, (uint16_t)session->assigned_streams);
    s_resp[SESSION_ID_LEN + 2] = (uint8_t)is_resumed;
    put32(s_resp + SESSION_ID_LEN + 3, (uint32_t)topic->nfiles);
    put64(s_resp + SESSION_ID_LEN + 7, topic->total_bytes);
    memcpy(s_resp + SESSION_ID_LEN + 15, topic->manifest_hash, SHA256_DIGEST_LEN);

    if (conn_send(c, MSG_ENHANCED_SESSION_RESP, 0, ERR_OK, s_resp, sizeof s_resp, 0) != 0) return;

    /* Send Manifest (Catalog) */
    uint8_t m_start[4 + 8 + SHA256_DIGEST_LEN];
    put32(m_start + 0, (uint32_t)topic->nfiles);
    put64(m_start + 4, topic->total_bytes);
    memcpy(m_start + 12, topic->manifest_hash, SHA256_DIGEST_LEN);
    if (conn_send(c, MSG_MANIFEST_START, 0, ERR_OK, m_start, sizeof m_start, 0) != 0) return;

    for (size_t i = 0; i < topic->nfiles; i++) {
        if (g_stop) return;
        const fentry_t *fe = &topic->files[i];
        uint16_t fnlen = (uint16_t)strlen(fe->name);
        uint8_t entry[4 + 8 + SHA256_DIGEST_LEN + 2 + MAX_NAME];
        put32(entry + 0, (uint32_t)i);
        put64(entry + 4, fe->size);
        memcpy(entry + 12, fe->hash, SHA256_DIGEST_LEN);
        put16(entry + 12 + SHA256_DIGEST_LEN, fnlen);
        memcpy(entry + 14 + SHA256_DIGEST_LEN, fe->name, fnlen);
        if (conn_send(c, MSG_MANIFEST_ENTRY, 0, ERR_OK, entry, 14 + SHA256_DIGEST_LEN + fnlen, 0) != 0) return;
    }

    uint8_t m_end[8];
    put32(m_end + 0, (uint32_t)topic->nfiles);
    put32(m_end + 4, ERR_OK);
    if (conn_send(c, MSG_MANIFEST_END, 0, ERR_OK, m_end, sizeof m_end, 0) != 0) return;

    /* Control connection monitors for session completion or client disconnect */
    pthread_mutex_lock(&session->lock);
    while (!g_stop && session->committed_blocks_count < session->total_blocks) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&session->cond, &session->lock, &ts);

        uint8_t peek_buf[1];
        ssize_t pr = recv(c->fd, peek_buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (pr == 0 || (pr < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            pthread_mutex_unlock(&session->lock);
            return;
        }
    }

    if (session->committed_blocks_count >= session->total_blocks) {
        session->state = 1; /* COMPLETED */
        persist_session_locked(session);
        pthread_mutex_unlock(&session->lock);

        uint8_t done[4 + 8 + SHA256_DIGEST_LEN];
        put32(done + 0, (uint32_t)topic->nfiles);
        put64(done + 4, topic->total_bytes);
        memcpy(done + 12, topic->manifest_hash, SHA256_DIGEST_LEN);
        conn_send(c, MSG_TRANSFER_DONE, 0, ERR_OK, done, sizeof done, 0);
        LOG("%s: session '%s' completed all %u blocks (%llu bytes)",
            peer, session->session_id, session->total_blocks, (unsigned long long)session->total_bytes);
    } else {
        pthread_mutex_unlock(&session->lock);
    }
}

static void handle_worker_stream(conn_t *c, uint8_t *first_pl, const char *peer, unsigned int *seed)
{
    char sess_id[SESSION_ID_LEN + 1];
    memcpy(sess_id, first_pl, SESSION_ID_LEN);
    sess_id[SESSION_ID_LEN] = '\0';
    uint16_t stream_id = get16(first_pl + SESSION_ID_LEN);

    enhanced_session_t *session = find_or_load_session(sess_id);
    if (!session) {
        send_error(c, ERR_SESSION_NOT_FOUND, "session not found");
        return;
    }

    const topic_t *topic = find_topic(session->topic);
    if (!topic) {
        send_error(c, ERR_TOPIC_NOT_FOUND, "topic not found");
        return;
    }

    /* Send MSG_STREAM_ATTACH_ACK */
    uint8_t ack_pl[4];
    put16(ack_pl, stream_id);
    put16(ack_pl + 2, ERR_OK);
    if (conn_send(c, MSG_STREAM_ATTACH_ACK, 0, ERR_OK, ack_pl, sizeof ack_pl, 0) != 0) return;

    pthread_mutex_lock(&session->lock);
    session->active_streams++;
    pthread_mutex_unlock(&session->lock);

    uint32_t local_chunk_counter = 0;
    int current_in_flight_bidx = -1;

    for (;;) {
        if (g_stop) break;

        /* Wait for client to request work via MSG_PULL_WORK */
        proto_hdr_t ph;
        uint8_t ppl[64];
        if (conn_recv(c, &ph, ppl) != 0) break;
        if (ph.type != MSG_PULL_WORK) break;

        /* Find next unassigned block */
        pthread_mutex_lock(&session->lock);
        int chosen_bidx = -1;
        while (!g_stop && session->committed_blocks_count < session->total_blocks) {
            for (uint32_t b = 0; b < session->total_blocks; b++) {
                if (session->blocks[b].status == 0) { /* UNASSIGNED */
                    chosen_bidx = (int)b;
                    session->blocks[b].status = 1;    /* IN_FLIGHT */
                    session->blocks[b].stream_id = stream_id;
                    break;
                }
            }
            if (chosen_bidx != -1) break;

            /* All blocks either completed or in-flight by other streams */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&session->cond, &session->lock, &ts);
        }

        if (chosen_bidx == -1) {
            /* All blocks committed or stopping */
            pthread_mutex_unlock(&session->lock);
            conn_send(c, MSG_NO_MORE_WORK, 0, ERR_OK, NULL, 0, 0);
            break;
        }

        block_desc_t blk = session->blocks[chosen_bidx];
        current_in_flight_bidx = chosen_bidx;
        pthread_mutex_unlock(&session->lock);

        /* Check failure injection */
        const fentry_t *fe = &topic->files[blk.file_idx];
        if (should_inject_fault(seed, local_chunk_counter, peer, fe->name)) {
            atomic_fetch_add(&g_injected_failures, 1);
            pthread_mutex_lock(&session->lock);
            session->blocks[chosen_bidx].status = 0; /* Release back to UNASSIGNED */
            session->blocks[chosen_bidx].stream_id = -1;
            pthread_cond_broadcast(&session->cond);
            pthread_mutex_unlock(&session->lock);
            shutdown(c->fd, SHUT_RDWR);
            return;
        }

        /* Read file range from disk */
        char path[PATH_MAX + 512];
        snprintf(path, sizeof path, "%s/%s", topic->dir, fe->name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            send_error(c, ERR_FILE_NOT_FOUND, "file read failed");
            break;
        }

        uint8_t payload_buf[16 + CHUNK_SIZE];
        put32(payload_buf + 0, blk.file_idx);
        put64(payload_buf + 4, blk.offset);
        put32(payload_buf + 12, blk.length);

        ssize_t prd = pread(fd, payload_buf + 16, blk.length, (off_t)blk.offset);
        close(fd);

        if (prd != (ssize_t)blk.length) {
            send_error(c, ERR_INTERNAL, "pread failed");
            break;
        }

        /* Send MSG_DATA_CHUNK */
        uint16_t flags = (blk.offset + blk.length == fe->size) ? FLAG_EOF_FILE : 0;
        if (conn_send(c, MSG_DATA_CHUNK, flags, ERR_OK, payload_buf, 16 + blk.length, 0) != 0) {
            break;
        }

        /* Wait for MSG_CHECKPOINT_ACK */
        proto_hdr_t ack_h;
        uint8_t ack_pl[128];
        if (conn_recv(c, &ack_h, ack_pl) != 0 || ack_h.type != MSG_CHECKPOINT_ACK) {
            break;
        }

        uint32_t ack_fidx = get32(ack_pl + SESSION_ID_LEN);
        uint64_t ack_off  = get64(ack_pl + SESSION_ID_LEN + 4);
        uint32_t ack_len  = get32(ack_pl + SESSION_ID_LEN + 12);

        if (ack_fidx != blk.file_idx || ack_off != blk.offset || ack_len != blk.length) {
            send_error(c, ERR_PROTOCOL, "invalid range ACK");
            break;
        }

        /* Durably mark block COMMITTED */
        pthread_mutex_lock(&session->lock);
        session->blocks[chosen_bidx].status = 2; /* COMMITTED */
        session->bitmask[chosen_bidx / 8] |= (uint8_t)(1 << (chosen_bidx % 8));
        session->committed_blocks_count++;
        session->last_active_at = time(NULL);
        persist_session_locked(session);
        pthread_cond_broadcast(&session->cond);
        current_in_flight_bidx = -1;
        pthread_mutex_unlock(&session->lock);

        /* Confirm commit to client */
        uint8_t c_pl[16];
        put32(c_pl + 0, blk.file_idx);
        put64(c_pl + 4, blk.offset);
        put32(c_pl + 12, blk.length);
        if (conn_send(c, MSG_CHECKPOINT_COMMIT, 0, ERR_OK, c_pl, sizeof c_pl, 0) != 0) {
            break;
        }

        local_chunk_counter++;
        atomic_fetch_add(&g_total_data_bytes, blk.length);
    }

    /* Cleanup: release uncommitted in-flight block */
    pthread_mutex_lock(&session->lock);
    session->active_streams--;
    if (current_in_flight_bidx != -1 && session->blocks[current_in_flight_bidx].status == 1) {
        session->blocks[current_in_flight_bidx].status = 0; /* Release back */
        session->blocks[current_in_flight_bidx].stream_id = -1;
        pthread_cond_broadcast(&session->cond);
    }
    pthread_mutex_unlock(&session->lock);
}

static void handle_client(int fd, const char *peer, unsigned int *seed)
{
    conn_t c;
    conn_init(&c, fd);

    uint8_t pl[MAX_PAYLOAD];
    proto_hdr_t h;

    if (conn_recv(&c, &h, pl) != 0) {
        return;
    }

    if (h.type == MSG_ENHANCED_SESSION_REQ) {
        handle_control_connection(&c, &h, pl, peer);
    } else if (h.type == MSG_STREAM_ATTACH) {
        handle_worker_stream(&c, pl, peer, seed);
    } else {
        send_error(&c, ERR_BAD_REQUEST, "unexpected initial message");
    }
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
    close(j.fd);
    return NULL;
}

static void handle_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <port> <topics_root_dir> <failure_probability> [max_streams]\n", argv[0]);
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

    client_reg_init();

    if (load_topics(argv[2]) != 0 || g_ntopics == 0) {
        if (g_ntopics == 0) fprintf(stderr, "no topics found in '%s'\n", argv[2]);
        pthread_mutex_destroy(&g_client_reg.lock);
        pthread_cond_destroy(&g_client_reg.cond);
        return 1;
    }

    /* Setup session persistence directory */
    const char *sess_env = getenv("IT305_SESSION_DIR");
    if (sess_env && sess_env[0] != '\0') {
        snprintf(g_sessions_dir, sizeof g_sessions_dir, "%s", sess_env);
    } else {
        snprintf(g_sessions_dir, sizeof g_sessions_dir, "%s/.sessions", argv[2]);
    }
    mkdir_p(g_sessions_dir);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sfd, (struct sockaddr *)&sin, sizeof sin) != 0) {
        perror("bind");
        close(sfd);
        return 1;
    }

    if (listen(sfd, 128) != 0) {
        perror("listen");
        close(sfd);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof sa_pipe);
    sa_pipe.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa_pipe, NULL);

    LOG("listening on port %ld, root='%s', sessions_dir='%s', failure_prob=%.4f",
        port, argv[2], g_sessions_dir, g_failure_prob);

    while (!g_stop) {
        struct sockaddr_in cin;
        socklen_t clen = sizeof cin;
        int cfd = accept(sfd, (struct sockaddr *)&cin, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (g_stop) break;
            perror("accept");
            continue;
        }

        char peer[64];
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cin.sin_addr, ipstr, sizeof ipstr);
        snprintf(peer, sizeof peer, "%s:%u", ipstr, ntohs(cin.sin_port));

        if (client_reg_add(cfd) != 0) {
            LOG("%s: connection registry full, rejecting client", peer);
            close(cfd);
            continue;
        }

        job_t *j = malloc(sizeof(job_t));
        if (!j) {
            client_reg_del(cfd);
            close(cfd);
            continue;
        }
        j->fd = cfd;
        snprintf(j->peer, sizeof j->peer, "%s", peer);

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&tid, &attr, worker, j) != 0) {
            LOG("%s: pthread_create failed", peer);
            pthread_attr_destroy(&attr);
            client_reg_del(cfd);
            free(j);
            close(cfd);
            continue;
        }
        pthread_attr_destroy(&attr);
    }

    LOG("shutting down, waiting for active connections to finish...");
    close(sfd);
    client_reg_interrupt_all();
    client_reg_wait_empty(5.0);

    pthread_mutex_destroy(&g_client_reg.lock);
    pthread_cond_destroy(&g_client_reg.cond);

    LOG("shutdown complete (served %llu data bytes, injected %llu faults)",
        (unsigned long long)atomic_load(&g_total_data_bytes),
        (unsigned long long)atomic_load(&g_injected_failures));

    return 0;
}
