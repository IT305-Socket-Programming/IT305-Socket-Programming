#include "common.h"
#include <stdatomic.h>

typedef struct {
    char     name[MAX_NAME];
    uint64_t size;
    uint8_t  hash[SHA256_DIGEST_LEN];
} client_fentry_t;

typedef struct {
    const char      *server_ip;
    int              port;
    char             session_id[SESSION_ID_LEN + 1];
    int              stream_id;
    const char      *staging_dir;
    uint32_t         total_files;
    client_fentry_t *files;
    atomic_uint_fast64_t *transmitted_payload_bytes;
    atomic_uint_fast64_t *protocol_bytes;
    int              worker_status; /* 0 = OK, -1 = ERROR */
} worker_arg_t;

static void *stream_worker(void *arg)
{
    worker_arg_t *w = (worker_arg_t *)arg;
    w->worker_status = 0;

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        w->worker_status = -1;
        return NULL;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)w->port);
    if (inet_pton(AF_INET, w->server_ip, &sin.sin_addr) <= 0) {
        close(sfd);
        w->worker_status = -1;
        return NULL;
    }

    if (connect(sfd, (struct sockaddr *)&sin, sizeof sin) != 0) {
        close(sfd);
        w->worker_status = -1;
        return NULL;
    }

    conn_t c;
    conn_init(&c, sfd);

    /* Send MSG_STREAM_ATTACH */
    uint8_t attach_pl[SESSION_ID_LEN + 2];
    memcpy(attach_pl, w->session_id, SESSION_ID_LEN);
    put16(attach_pl + SESSION_ID_LEN, (uint16_t)w->stream_id);

    if (conn_send(&c, MSG_STREAM_ATTACH, 0, ERR_OK, attach_pl, sizeof attach_pl, 0) != 0) {
        atomic_fetch_add(w->protocol_bytes, c.tx_bytes + c.rx_bytes);
        close(sfd);
        w->worker_status = -1;
        return NULL;
    }

    proto_hdr_t h;
    uint8_t pl[MAX_PAYLOAD];
    if (conn_recv(&c, &h, pl) != 0 || h.type != MSG_STREAM_ATTACH_ACK) {
        atomic_fetch_add(w->protocol_bytes, c.tx_bytes + c.rx_bytes);
        close(sfd);
        w->worker_status = -1;
        return NULL;
    }

    /* Pull work in a loop */
    for (;;) {
        if (conn_send(&c, MSG_PULL_WORK, 0, ERR_OK, NULL, 0, 0) != 0) {
            w->worker_status = -1;
            break;
        }

        if (conn_recv(&c, &h, pl) != 0) {
            w->worker_status = -1;
            break;
        }

        if (h.type == MSG_NO_MORE_WORK) {
            /* All blocks in session have been served/committed */
            break;
        }

        if (h.type != MSG_DATA_CHUNK || h.len < 16) {
            w->worker_status = -1;
            break;
        }

        uint32_t fidx = get32(pl + 0);
        uint64_t off  = get64(pl + 4);
        uint32_t len  = get32(pl + 12);

        if (fidx >= w->total_files || len > (h.len - 16)) {
            w->worker_status = -1;
            break;
        }

        char fpath[PATH_MAX + 512];
        int path_len = snprintf(fpath, sizeof fpath, "%s/%s", w->staging_dir,
                                w->files[fidx].name);
        if (path_len < 0 || (size_t)path_len >= sizeof fpath) {
            w->worker_status = -1;
            break;
        }

        /* Open only the file being written. Keeping one descriptor per topic
         * file exceeds common per-process limits for large datasets. */
        int fd = open(fpath, O_WRONLY | O_NOFOLLOW);
        if (fd < 0) {
            w->worker_status = -1;
            break;
        }

        /* Concurrent disjoint pwrite to exact offset */
        const uint8_t *raw_chunk = pl + 16;
        size_t written = 0;
        while (written < len) {
            ssize_t pw = pwrite(fd, raw_chunk + written, len - written,
                                (off_t)(off + written));
            if (pw < 0 && errno == EINTR) continue;
            if (pw <= 0) break;
            written += (size_t)pw;
        }
        if (written != len || fdatasync(fd) != 0) {
            close(fd);
            w->worker_status = -1;
            break;
        }
        close(fd); /* Durably persisted above; release before the next block. */

        atomic_fetch_add(w->transmitted_payload_bytes, len);

        /* Send MSG_CHECKPOINT_ACK */
        uint8_t ack_pl[SESSION_ID_LEN + 4 + 8 + 4];
        memcpy(ack_pl, w->session_id, SESSION_ID_LEN);
        put32(ack_pl + SESSION_ID_LEN, fidx);
        put64(ack_pl + SESSION_ID_LEN + 4, off);
        put32(ack_pl + SESSION_ID_LEN + 12, len);

        if (conn_send(&c, MSG_CHECKPOINT_ACK, 0, ERR_OK, ack_pl, sizeof ack_pl, 0) != 0) {
            w->worker_status = -1;
            break;
        }

        /* Wait for MSG_CHECKPOINT_COMMIT */
        if (conn_recv(&c, &h, pl) != 0 || h.type != MSG_CHECKPOINT_COMMIT) {
            w->worker_status = -1;
            break;
        }
    }

    atomic_fetch_add(w->protocol_bytes, c.tx_bytes + c.rx_bytes);
    close(sfd);
    return NULL;
}

static int save_client_checkpoint(const char *chk_path, const char *session_id,
                                  const char *topic, const uint8_t *manifest_hash,
                                  uint32_t total_files, const client_fentry_t *files)
{
    char tmp[PATH_MAX + 512];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", chk_path, getpid());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    char hex[65];
    sha256_to_hex(manifest_hash, hex);

    dprintf(fd, "%s\n%s\n%s\n%u\n", session_id, topic, hex, total_files);
    for (uint32_t i = 0; i < total_files; i++) {
        char fhex[65];
        sha256_to_hex(files[i].hash, fhex);
        dprintf(fd, "%s %llu %s\n", files[i].name, (unsigned long long)files[i].size, fhex);
    }

    fdatasync(fd);
    close(fd);
    return rename(tmp, chk_path);
}

static int load_client_checkpoint(const char *chk_path, char *out_session_id,
                                  char *out_topic, uint8_t *out_manifest_hash,
                                  uint32_t *out_total_files, client_fentry_t **out_files)
{
    FILE *f = fopen(chk_path, "r");
    if (!f) return -1;

    char hex[128];
    if (fscanf(f, "%32s\n%63s\n%64s\n%u\n", out_session_id, out_topic, hex, out_total_files) != 4) {
        fclose(f);
        return -1;
    }

    for (int i = 0; i < SHA256_DIGEST_LEN; i++) {
        unsigned int byte_val;
        if (sscanf(hex + (i * 2), "%02x", &byte_val) != 1) { fclose(f); return -1; }
        out_manifest_hash[i] = (uint8_t)byte_val;
    }

    client_fentry_t *files = calloc(*out_total_files, sizeof(client_fentry_t));
    if (!files) { fclose(f); return -1; }

    for (uint32_t i = 0; i < *out_total_files; i++) {
        char fname[MAX_NAME];
        unsigned long long fsz;
        char fhex[128];
        if (fscanf(f, "%255s %llu %64s\n", fname, &fsz, fhex) != 3) {
            free(files);
            fclose(f);
            return -1;
        }
        snprintf(files[i].name, sizeof files[i].name, "%s", fname);
        files[i].size = fsz;
        for (int k = 0; k < SHA256_DIGEST_LEN; k++) {
            unsigned int bv;
            sscanf(fhex + (k * 2), "%02x", &bv);
            files[i].hash[k] = (uint8_t)bv;
        }
    }
    fclose(f);
    *out_files = files;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <server_ip> <server_port> <topic> <output_dir> [streams] [max_attempts]\n", argv[0]);
        return 2;
    }

    const char *server_ip = argv[1];
    char *end;
    long port = strtol(argv[2], &end, 10);
    if (*argv[2] == '\0' || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port '%s': must be between 1 and 65535\n", argv[2]);
        return 2;
    }

    const char *topic = argv[3];
    if (!safe_name(topic)) {
        fprintf(stderr, "invalid topic '%s': path traversal not allowed\n", topic);
        return 2;
    }

    const char *outdir = argv[4];
    if (outdir[0] == '\0') {
        fprintf(stderr, "output directory cannot be empty\n");
        return 2;
    }

    char clean_outdir[PATH_MAX];
    if (strlen(outdir) >= sizeof clean_outdir) {
        fprintf(stderr, "output directory path too long\n");
        return 2;
    }
    strcpy(clean_outdir, outdir);
    sanitize_trailing_slashes(clean_outdir);

    int streams = DEFAULT_STREAMS;
    if (argc >= 6) {
        long s = strtol(argv[5], NULL, 10);
        if (s >= 1 && s <= MAX_STREAMS) streams = (int)s;
    }

    int max_attempts = 10;
    const char *env_attempts = getenv("IT305_MAX_ATTEMPTS");
    if (env_attempts && env_attempts[0] != '\0') {
        long v = strtol(env_attempts, NULL, 10);
        if (v > 0 && v <= 1000) max_attempts = (int)v;
    }
    if (argc >= 7) {
        long v = strtol(argv[6], NULL, 10);
        if (v > 0 && v <= 1000) max_attempts = (int)v;
    }

    double t_start = now_sec();
    int attempt = 0;
    int success = 0;
    int is_non_retryable = 0;

    uint64_t useful_payload_bytes = 0;
    atomic_uint_fast64_t transmitted_payload_bytes = 0;
    atomic_uint_fast64_t total_protocol_bytes = 0;

    char staging_dir[PATH_MAX + 256];
    snprintf(staging_dir, sizeof staging_dir, "%s/.staging_%s", clean_outdir, topic);
    mkdir_p(staging_dir);

    char chk_path[PATH_MAX + 256];
    snprintf(chk_path, sizeof chk_path, "%s/.session_%s.chk", clean_outdir, topic);

    char target_topic_dir[PATH_MAX + 256];
    snprintf(target_topic_dir, sizeof target_topic_dir, "%s/%s", clean_outdir, topic);

    char saved_session_id[SESSION_ID_LEN + 1] = {0};
    char saved_topic[TOPIC_FIELD] = {0};
    uint8_t saved_manifest_hash[SHA256_DIGEST_LEN] = {0};
    uint32_t total_files = 0;
    client_fentry_t *files = NULL;

    /* Load checkpoint if exists */
    if (load_client_checkpoint(chk_path, saved_session_id, saved_topic,
                               saved_manifest_hash, &total_files, &files) == 0) {
        if (strcmp(saved_topic, topic) != 0) {
            free(files);
            files = NULL;
            saved_session_id[0] = '\0';
        }
    }

    while (attempt < max_attempts && !success && !is_non_retryable) {
        attempt++;

        int cfd = socket(AF_INET, SOCK_STREAM, 0);
        if (cfd < 0) { usleep(50000); continue; }

        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof saddr);
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
            close(cfd);
            is_non_retryable = 1;
            break;
        }

        if (connect(cfd, (struct sockaddr *)&saddr, sizeof saddr) != 0) {
            close(cfd);
            usleep(50000);
            continue;
        }

        conn_t ctrl_c;
        conn_init(&ctrl_c, cfd);

        /* Send MSG_ENHANCED_SESSION_REQ */
        uint8_t s_req[SESSION_ID_LEN + 2 + 2 + TOPIC_FIELD];
        if (saved_session_id[0] != '\0') {
            memcpy(s_req, saved_session_id, SESSION_ID_LEN);
        } else {
            memset(s_req, '0', SESSION_ID_LEN);
        }
        put16(s_req + SESSION_ID_LEN, (uint16_t)streams);
        uint16_t tlen = (uint16_t)strlen(topic);
        put16(s_req + SESSION_ID_LEN + 2, tlen);
        memcpy(s_req + SESSION_ID_LEN + 4, topic, tlen);

        uint16_t flags = (saved_session_id[0] != '\0') ? FLAG_RESUME : 0;
        if (conn_send(&ctrl_c, MSG_ENHANCED_SESSION_REQ, flags, 0, s_req, SESSION_ID_LEN + 4 + tlen, 0) != 0) {
            atomic_fetch_add(&total_protocol_bytes, ctrl_c.tx_bytes + ctrl_c.rx_bytes);
            close(cfd);
            usleep(50000);
            continue;
        }

        proto_hdr_t h;
        uint8_t pl[MAX_PAYLOAD];
        if (conn_recv(&ctrl_c, &h, pl) != 0) {
            atomic_fetch_add(&total_protocol_bytes, ctrl_c.tx_bytes + ctrl_c.rx_bytes);
            close(cfd);
            usleep(50000);
            continue;
        }

        if (h.type == MSG_ERROR) {
            uint16_t err_code = get16(pl);
            fprintf(stderr, "[attempt %d] server returned error %u\n", attempt, err_code);
            atomic_fetch_add(&total_protocol_bytes, ctrl_c.tx_bytes + ctrl_c.rx_bytes);
            close(cfd);
            if (err_code == ERR_TOPIC_NOT_FOUND || err_code == ERR_BAD_REQUEST) {
                is_non_retryable = 1;
            } else if (err_code == ERR_SESSION_NOT_FOUND || err_code == ERR_CHECKSUM_MISMATCH) {
                saved_session_id[0] = '\0';
                unlink(chk_path);
            }
            usleep(50000);
            continue;
        }

        if (h.type != MSG_ENHANCED_SESSION_RESP || h.len < SESSION_ID_LEN + 2 + 1 + 4 + 8 + SHA256_DIGEST_LEN) {
            atomic_fetch_add(&total_protocol_bytes, ctrl_c.tx_bytes + ctrl_c.rx_bytes);
            close(cfd);
            usleep(50000);
            continue;
        }

        char active_sess_id[SESSION_ID_LEN + 1];
        memcpy(active_sess_id, pl, SESSION_ID_LEN);
        active_sess_id[SESSION_ID_LEN] = '\0';
        strncpy(saved_session_id, active_sess_id, SESSION_ID_LEN);
        saved_session_id[SESSION_ID_LEN] = '\0';

        int assigned_streams = get16(pl + SESSION_ID_LEN);
        if (assigned_streams < 1) assigned_streams = 1;
        if (assigned_streams > MAX_STREAMS) assigned_streams = MAX_STREAMS;

        total_files = get32(pl + SESSION_ID_LEN + 3);
        uint8_t manifest_hash[SHA256_DIGEST_LEN];
        memcpy(manifest_hash, pl + SESSION_ID_LEN + 15, SHA256_DIGEST_LEN);

        /* Receive Manifest Catalog */
        if (conn_recv(&ctrl_c, &h, pl) != 0 || h.type != MSG_MANIFEST_START) {
            atomic_fetch_add(&total_protocol_bytes, ctrl_c.tx_bytes + ctrl_c.rx_bytes);
            close(cfd);
            usleep(50000);
            continue;
        }

        if (files) free(files);
        files = calloc(total_files, sizeof(client_fentry_t));

        int m_ok = 1;
        for (uint32_t i = 0; i < total_files; i++) {
            if (conn_recv(&ctrl_c, &h, pl) != 0 || h.type != MSG_MANIFEST_ENTRY) {
                m_ok = 0;
                break;
            }
            files[i].size = get64(pl + 4);
            memcpy(files[i].hash, pl + 12, SHA256_DIGEST_LEN);
            uint16_t fnlen = get16(pl + 12 + SHA256_DIGEST_LEN);
            memcpy(files[i].name, pl + 14 + SHA256_DIGEST_LEN, fnlen);
            files[i].name[fnlen] = '\0';
        }

        if (!m_ok || conn_recv(&ctrl_c, &h, pl) != 0 || h.type != MSG_MANIFEST_END) {
            atomic_fetch_add(&total_protocol_bytes, ctrl_c.tx_bytes + ctrl_c.rx_bytes);
            close(cfd);
            usleep(50000);
            continue;
        }

        save_client_checkpoint(chk_path, saved_session_id, topic, manifest_hash, total_files, files);

        /* Create and size files one at a time. Do not retain one descriptor
         * per file: Animals10 topics contain thousands of files. */
        int files_ready = 1;
        for (uint32_t i = 0; i < total_files; i++) {
            char fpath[PATH_MAX + 512];
            int path_len = snprintf(fpath, sizeof fpath, "%s/%s", staging_dir,
                                    files[i].name);
            if (path_len < 0 || (size_t)path_len >= sizeof fpath) {
                files_ready = 0;
                break;
            }
            int fd = open(fpath, O_WRONLY | O_CREAT | O_NOFOLLOW, 0644);
            if (fd < 0 || ftruncate(fd, (off_t)files[i].size) != 0) {
                if (fd >= 0) close(fd);
                files_ready = 0;
                break;
            }
            close(fd);
        }

        if (!files_ready) {
            atomic_fetch_add(&total_protocol_bytes, ctrl_c.tx_bytes + ctrl_c.rx_bytes);
            close(cfd);
            usleep(50000);
            continue;
        }

        /* Launch assigned_streams worker threads */
        pthread_t threads[MAX_STREAMS];
        worker_arg_t wargs[MAX_STREAMS];

        for (int k = 0; k < assigned_streams; k++) {
            wargs[k].server_ip = server_ip;
            wargs[k].port = (int)port;
            snprintf(wargs[k].session_id, sizeof wargs[k].session_id, "%s", saved_session_id);
            wargs[k].stream_id = k;
            wargs[k].staging_dir = staging_dir;
            wargs[k].total_files = total_files;
            wargs[k].files = files;
            wargs[k].transmitted_payload_bytes = &transmitted_payload_bytes;
            wargs[k].protocol_bytes = &total_protocol_bytes;
            wargs[k].worker_status = 0;

            pthread_create(&threads[k], NULL, stream_worker, &wargs[k]);
        }

        /* Wait for workers */
        int any_worker_failed = 0;
        for (int k = 0; k < assigned_streams; k++) {
            pthread_join(threads[k], NULL);
            if (wargs[k].worker_status != 0) any_worker_failed = 1;
        }

        if (!any_worker_failed) {
            struct timeval tv;
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            /* Await MSG_TRANSFER_DONE from control connection */
            if (conn_recv(&ctrl_c, &h, pl) == 0 && h.type == MSG_TRANSFER_DONE) {
            /* Verify all file SHA-256 digests */
            int verified = 1;
            uint64_t cur_useful = 0;
            for (uint32_t i = 0; i < total_files; i++) {
                char fpath[PATH_MAX + 512];
                snprintf(fpath, sizeof fpath, "%s/%s", staging_dir, files[i].name);
                uint8_t act_hash[SHA256_DIGEST_LEN];
                if (sha256_file(fpath, act_hash, NULL) != 0 ||
                    memcmp(act_hash, files[i].hash, SHA256_DIGEST_LEN) != 0) {
                    verified = 0;
                    break;
                }
                cur_useful += files[i].size;
            }

            if (verified) {
                useful_payload_bytes = cur_useful;
                rm_rf(target_topic_dir);
                rename(staging_dir, target_topic_dir);
                unlink(chk_path);
                success = 1;
                atomic_fetch_add(&total_protocol_bytes, ctrl_c.tx_bytes + ctrl_c.rx_bytes);
                close(cfd);
                break;
            }
        }
        }

        atomic_fetch_add(&total_protocol_bytes, ctrl_c.tx_bytes + ctrl_c.rx_bytes);
        close(cfd);
        usleep(50000);
    }

    if (files) free(files);

    double elapsed_sec = now_sec() - t_start;
    if (elapsed_sec < 1e-6) elapsed_sec = 1e-6;

    uint64_t tx_pl = atomic_load(&transmitted_payload_bytes);
    uint64_t retransmitted_payload_bytes = tx_pl > useful_payload_bytes ? tx_pl - useful_payload_bytes : 0;
    double mbps = (useful_payload_bytes * 8.0) / (elapsed_sec * 1e6);

    printf("RESULT topic=%s status=%s attempts=%d retries=%d streams=%d useful_bytes=%llu transmitted_bytes=%llu retransmitted_bytes=%llu protocol_bytes=%llu elapsed_sec=%.4f mbps=%.2f\n",
           topic,
           success ? "OK" : "FAIL",
           attempt,
           attempt > 0 ? attempt - 1 : 0,
           streams,
           (unsigned long long)useful_payload_bytes,
           (unsigned long long)tx_pl,
           (unsigned long long)retransmitted_payload_bytes,
           (unsigned long long)atomic_load(&total_protocol_bytes),
           elapsed_sec,
           mbps);

    return success ? 0 : 1;
}
