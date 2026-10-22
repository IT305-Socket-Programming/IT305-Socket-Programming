#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <limits.h>
#include "common.h"

volatile sig_atomic_t g_stop = 0;

typedef struct {
    char     name[MAX_NAME + 1];
    uint64_t size;
    uint8_t  hash[SHA256_DIGEST_LEN];
    uint64_t committed_offset;
} client_fentry_t;

static void write_full(int fd, const void *buf, size_t n, const char *path)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "write error on %s: %s\n", path, strerror(errno));
            exit(1);
        }
        p += w;
        n -= (size_t)w;
    }
}

static int save_client_checkpoint(const char *chk_path, const char *session_id, const char *topic,
                                  const uint8_t manifest_hash[SHA256_DIGEST_LEN],
                                  uint32_t total_files, const client_fentry_t *files)
{
    char tmp_path[PATH_MAX + 64];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", chk_path);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    uint8_t hdr[SESSION_ID_LEN + TOPIC_FIELD + SHA256_DIGEST_LEN + 4];
    memcpy(hdr, session_id, SESSION_ID_LEN);
    memset(hdr + SESSION_ID_LEN, 0, TOPIC_FIELD);
    strncpy((char *)(hdr + SESSION_ID_LEN), topic, TOPIC_FIELD - 1);
    memcpy(hdr + SESSION_ID_LEN + TOPIC_FIELD, manifest_hash, SHA256_DIGEST_LEN);
    put32(hdr + SESSION_ID_LEN + TOPIC_FIELD + SHA256_DIGEST_LEN, total_files);

    if (write(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) { close(fd); unlink(tmp_path); return -1; }

    for (uint32_t i = 0; i < total_files; i++) {
        uint8_t fbuf[MAX_NAME + 8 + 8 + SHA256_DIGEST_LEN];
        memset(fbuf, 0, MAX_NAME);
        strncpy((char *)fbuf, files[i].name, MAX_NAME);
        put64(fbuf + MAX_NAME, files[i].size);
        put64(fbuf + MAX_NAME + 8, files[i].committed_offset);
        memcpy(fbuf + MAX_NAME + 16, files[i].hash, SHA256_DIGEST_LEN);
        if (write(fd, fbuf, sizeof fbuf) != (ssize_t)sizeof fbuf) { close(fd); unlink(tmp_path); return -1; }
    }

    fdatasync(fd);
    close(fd);
    return rename(tmp_path, chk_path);
}

static int load_client_checkpoint(const char *chk_path, char *session_id, char *topic,
                                  uint8_t manifest_hash[SHA256_DIGEST_LEN],
                                  uint32_t *total_files, client_fentry_t **out_files)
{
    int fd = open(chk_path, O_RDONLY);
    if (fd < 0) return -1;

    uint8_t hdr[SESSION_ID_LEN + TOPIC_FIELD + SHA256_DIGEST_LEN + 4];
    if (read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) { close(fd); return -1; }

    memcpy(session_id, hdr, SESSION_ID_LEN);
    session_id[SESSION_ID_LEN] = '\0';
    memcpy(topic, hdr + SESSION_ID_LEN, TOPIC_FIELD);
    topic[TOPIC_FIELD - 1] = '\0';
    memcpy(manifest_hash, hdr + SESSION_ID_LEN + TOPIC_FIELD, SHA256_DIGEST_LEN);
    uint32_t tf = get32(hdr + SESSION_ID_LEN + TOPIC_FIELD + SHA256_DIGEST_LEN);

    client_fentry_t *files = calloc(tf, sizeof(client_fentry_t));
    if (!files && tf > 0) { close(fd); return -1; }

    for (uint32_t i = 0; i < tf; i++) {
        uint8_t fbuf[MAX_NAME + 8 + 8 + SHA256_DIGEST_LEN];
        if (read(fd, fbuf, sizeof fbuf) != (ssize_t)sizeof fbuf) {
            free(files);
            close(fd);
            return -1;
        }
        memcpy(files[i].name, fbuf, MAX_NAME);
        files[i].name[MAX_NAME] = '\0';
        files[i].size = get64(fbuf + MAX_NAME);
        files[i].committed_offset = get64(fbuf + MAX_NAME + 8);
        memcpy(files[i].hash, fbuf + MAX_NAME + 16, SHA256_DIGEST_LEN);
    }
    close(fd);
    *total_files = tf;
    *out_files = files;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <server_ip> <server_port> <topic> <output_dir> [max_attempts]\n", argv[0]);
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
        fprintf(stderr, "invalid topic '%s': path traversal characters not allowed\n", topic);
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

    int max_attempts = 10;
    const char *env_attempts = getenv("IT305_MAX_ATTEMPTS");
    if (env_attempts && env_attempts[0] != '\0') {
        long v = strtol(env_attempts, NULL, 10);
        if (v > 0 && v <= 1000) max_attempts = (int)v;
    }
    if (argc >= 6) {
        long v = strtol(argv[5], NULL, 10);
        if (v > 0 && v <= 1000) max_attempts = (int)v;
    }

    double t_start = now_sec();
    int attempt = 0;
    int success = 0;
    int is_non_retryable = 0;

    uint64_t useful_payload_bytes = 0;
    uint64_t transmitted_payload_bytes = 0;
    uint64_t total_protocol_bytes = 0;

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

    /* Check if previous checkpoint exists */
    if (load_client_checkpoint(chk_path, saved_session_id, saved_topic,
                               saved_manifest_hash, &total_files, &files) == 0) {
        if (strcmp(saved_topic, topic) != 0) {
            /* Different topic checkpoint: discard */
            free(files);
            files = NULL;
            saved_session_id[0] = '\0';
        }
    }

    while (attempt < max_attempts && !success && !is_non_retryable) {
        attempt++;

        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd < 0) {
            usleep(50000);
            continue;
        }

        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof saddr);
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
            close(sfd);
            is_non_retryable = 1;
            break;
        }

        if (connect(sfd, (struct sockaddr *)&saddr, sizeof saddr) != 0) {
            close(sfd);
            usleep(50000);
            continue;
        }

        conn_t c;
        conn_init(&c, sfd);

        /* Send MSG_SESSION_REQ */
        uint8_t s_req[SESSION_ID_LEN + 2 + TOPIC_FIELD];
        if (saved_session_id[0] != '\0') {
            memcpy(s_req, saved_session_id, SESSION_ID_LEN);
        } else {
            memset(s_req, '0', SESSION_ID_LEN);
        }
        uint16_t tlen = (uint16_t)strlen(topic);
        put16(s_req + SESSION_ID_LEN, tlen);
        memcpy(s_req + SESSION_ID_LEN + 2, topic, tlen);

        uint16_t flags = (saved_session_id[0] != '\0') ? FLAG_RESUME : 0;
        if (conn_send(&c, MSG_SESSION_REQ, flags, 0, s_req, SESSION_ID_LEN + 2 + tlen, 0) != 0) {
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            usleep(50000);
            continue;
        }

        proto_hdr_t h;
        uint8_t pl[MAX_PAYLOAD];

        if (conn_recv(&c, &h, pl) != 0) {
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            usleep(50000);
            continue;
        }

        if (h.type == MSG_ERROR) {
            uint16_t err_code = get16(pl);
            fprintf(stderr, "[attempt %d] server returned error %u\n", attempt, err_code);
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            if (err_code == ERR_TOPIC_NOT_FOUND || err_code == ERR_BAD_REQUEST) {
                is_non_retryable = 1;
            } else if (err_code == ERR_SESSION_NOT_FOUND || err_code == ERR_CHECKSUM_MISMATCH) {
                /* Stale/corrupt session: reset session ID to restart fresh */
                saved_session_id[0] = '\0';
                unlink(chk_path);
            }
            usleep(50000);
            continue;
        }

        if (h.type != MSG_SESSION_RESP || h.len < SESSION_ID_LEN + 1 + 4 + 8 + SHA256_DIGEST_LEN) {
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            usleep(50000);
            continue;
        }

        char active_sess_id[SESSION_ID_LEN + 1];
        memcpy(active_sess_id, pl, SESSION_ID_LEN);
        active_sess_id[SESSION_ID_LEN] = '\0';
        strncpy(saved_session_id, active_sess_id, SESSION_ID_LEN);
        saved_session_id[SESSION_ID_LEN] = '\0';

        uint8_t is_resumed = pl[SESSION_ID_LEN];
        total_files = get32(pl + SESSION_ID_LEN + 1);
        uint8_t manifest_hash[SHA256_DIGEST_LEN];
        memcpy(manifest_hash, pl + SESSION_ID_LEN + 13, SHA256_DIGEST_LEN);

        if (is_resumed == 1) {
            /* Receive MSG_RESUME_RESP containing authoritative committed checkpoints */
            if (conn_recv(&c, &h, pl) != 0 || h.type != MSG_RESUME_RESP) {
                total_protocol_bytes += c.tx_bytes + c.rx_bytes;
                close(sfd);
                usleep(50000);
                continue;
            }

            uint32_t r_files = get32(pl + SESSION_ID_LEN + 4 + SHA256_DIGEST_LEN);
            if (r_files != total_files) {
                total_protocol_bytes += c.tx_bytes + c.rx_bytes;
                close(sfd);
                usleep(50000);
                continue;
            }

            if (!files) files = calloc(total_files, sizeof(client_fentry_t));

            /* Reconcile staging files with authoritative checkpoints */
            for (uint32_t i = 0; i < total_files; i++) {
                uint64_t auth_off = get64(pl + SESSION_ID_LEN + 8 + SHA256_DIGEST_LEN + (i * 8));
                files[i].committed_offset = auth_off;

                if (files[i].name[0] != '\0') {
                    char fpath[PATH_MAX + 512];
                    snprintf(fpath, sizeof fpath, "%s/%s", staging_dir, files[i].name);
                    if (truncate(fpath, (off_t)auth_off) != 0) {
                        /* truncated safely or already matched */
                    }
                }
            }
            save_client_checkpoint(chk_path, saved_session_id, topic, manifest_hash, total_files, files);
        } else {
            /* Fresh session: Receive Manifest */
            if (conn_recv(&c, &h, pl) != 0 || h.type != MSG_MANIFEST_START) {
                total_protocol_bytes += c.tx_bytes + c.rx_bytes;
                close(sfd);
                usleep(50000);
                continue;
            }

            if (files) free(files);
            files = calloc(total_files, sizeof(client_fentry_t));

            int m_ok = 1;
            for (uint32_t i = 0; i < total_files; i++) {
                if (conn_recv(&c, &h, pl) != 0 || h.type != MSG_MANIFEST_ENTRY) {
                    m_ok = 0;
                    break;
                }
                files[i].size = get64(pl + 4);
                memcpy(files[i].hash, pl + 12, SHA256_DIGEST_LEN);
                uint16_t nlen = get16(pl + 12 + SHA256_DIGEST_LEN);
                memcpy(files[i].name, pl + 14 + SHA256_DIGEST_LEN, nlen);
                files[i].name[nlen] = '\0';
                files[i].committed_offset = 0;
            }

            if (!m_ok || conn_recv(&c, &h, pl) != 0 || h.type != MSG_MANIFEST_END) {
                total_protocol_bytes += c.tx_bytes + c.rx_bytes;
                close(sfd);
                usleep(50000);
                continue;
            }
            save_client_checkpoint(chk_path, saved_session_id, topic, manifest_hash, total_files, files);
        }

        /* Receive streamed chunks with explicit Checkpoint ACKs */
        int transfer_interrupted = 0;

        for (uint32_t i = 0; i < total_files; i++) {
            /* If file was already 100% committed in earlier attempt, skip network I/O */
            if (files[i].committed_offset >= files[i].size && files[i].size > 0) {
                continue;
            }

            if (conn_recv(&c, &h, pl) != 0 || h.type != MSG_FILE_HEADER) {
                transfer_interrupted = 1;
                break;
            }

            uint32_t fidx = get32(pl);
            uint64_t start_off = get64(pl + 12);
            if (fidx != i || start_off != files[i].committed_offset) {
                transfer_interrupted = 1;
                break;
            }

            char fpath[PATH_MAX + 512];
            snprintf(fpath, sizeof fpath, "%s/%s", staging_dir, files[i].name);

            int ofd = open(fpath, O_WRONLY | O_CREAT | O_NOFOLLOW, 0644);
            if (ofd < 0) {
                transfer_interrupted = 1;
                break;
            }

            if (start_off > 0) {
                lseek(ofd, (off_t)start_off, SEEK_SET);
            }

            uint64_t current_off = start_off;
            while (current_off < files[i].size) {
                if (conn_recv(&c, &h, pl) != 0) {
                    transfer_interrupted = 1;
                    break;
                }
                if (h.type != MSG_DATA_CHUNK) {
                    transfer_interrupted = 1;
                    break;
                }

                if (h.len > 0) {
                    write_full(ofd, pl, h.len, fpath);
                    fdatasync(ofd); /* Local durability commitment */
                    transmitted_payload_bytes += h.len;

                    /* Send MSG_CHECKPOINT_ACK */
                    uint8_t ack_pl[SESSION_ID_LEN + 4 + 8];
                    memcpy(ack_pl, saved_session_id, SESSION_ID_LEN);
                    put32(ack_pl + SESSION_ID_LEN, i);
                    put64(ack_pl + SESSION_ID_LEN + 4, current_off + h.len);

                    if (conn_send(&c, MSG_CHECKPOINT_ACK, 0, ERR_OK, ack_pl, sizeof ack_pl, 0) != 0) {
                        transfer_interrupted = 1;
                        break;
                    }

                    /* Wait for Server Checkpoint Commitment */
                    proto_hdr_t com_h;
                    uint8_t com_pl[64];
                    if (conn_recv(&c, &com_h, com_pl) != 0 || com_h.type != MSG_CHECKPOINT_COMMIT) {
                        transfer_interrupted = 1;
                        break;
                    }

                    current_off += h.len;
                    files[i].committed_offset = current_off;
                    save_client_checkpoint(chk_path, saved_session_id, topic, manifest_hash, total_files, files);
                }
            }
            close(ofd);
            if (transfer_interrupted) break;
        }

        if (!transfer_interrupted) {
            /* Receive MSG_TRANSFER_DONE */
            if (conn_recv(&c, &h, pl) == 0 && h.type == MSG_TRANSFER_DONE) {
                /* Verify all file SHA-256 digests */
                int verified = 1;
                for (uint32_t i = 0; i < total_files; i++) {
                    char fpath[PATH_MAX + 512];
                    snprintf(fpath, sizeof fpath, "%s/%s", staging_dir, files[i].name);
                    uint8_t act_hash[SHA256_DIGEST_LEN];
                    if (sha256_file(fpath, act_hash, NULL) != 0 ||
                        memcmp(act_hash, files[i].hash, SHA256_DIGEST_LEN) != 0) {
                        verified = 0;
                        break;
                    }
                    useful_payload_bytes += files[i].size;
                }

                if (verified) {
                    /* Atomic publication */
                    rm_rf(target_topic_dir);
                    rename(staging_dir, target_topic_dir);
                    unlink(chk_path);
                    success = 1;
                    total_protocol_bytes += c.tx_bytes + c.rx_bytes;
                    close(sfd);
                    break;
                }
            }
        }

        total_protocol_bytes += c.tx_bytes + c.rx_bytes;
        close(sfd);
        usleep(50000);
    }

    if (files) free(files);

    double elapsed_sec = now_sec() - t_start;
    if (elapsed_sec < 1e-6) elapsed_sec = 1e-6;
    uint64_t retransmitted_payload_bytes = transmitted_payload_bytes > useful_payload_bytes ?
                                           transmitted_payload_bytes - useful_payload_bytes : 0;
    double mbps = (useful_payload_bytes * 8.0) / (elapsed_sec * 1e6);

    printf("RESULT topic=%s status=%s attempts=%d retries=%d useful_bytes=%llu transmitted_bytes=%llu retransmitted_bytes=%llu protocol_bytes=%llu elapsed_sec=%.4f mbps=%.2f\n",
           topic,
           success ? "OK" : "FAIL",
           attempt,
           attempt > 0 ? attempt - 1 : 0,
           (unsigned long long)useful_payload_bytes,
           (unsigned long long)transmitted_payload_bytes,
           (unsigned long long)retransmitted_payload_bytes,
           (unsigned long long)total_protocol_bytes,
           elapsed_sec,
           mbps);

    return success ? 0 : 1;
}
