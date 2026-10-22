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
        fprintf(stderr, "invalid topic '%s': path traversal or control characters not allowed\n", topic);
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

    int max_attempts = 5;
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

    uint64_t transmitted_payload_bytes = 0;
    uint64_t useful_payload_bytes = 0;
    uint64_t total_protocol_bytes = 0;

    char target_topic_dir[PATH_MAX + 256];
    if (snprintf(target_topic_dir, sizeof target_topic_dir, "%s/%s", clean_outdir, topic) >= (int)sizeof target_topic_dir) {
        fprintf(stderr, "target topic path too long\n");
        return 2;
    }

    while (attempt < max_attempts && !success && !is_non_retryable) {
        attempt++;
        uint64_t attempt_payload_bytes = 0;

        char staging_dir[PATH_MAX + 256];
        if (snprintf(staging_dir, sizeof staging_dir, "%s/.staging_%s_%d_%d",
                     clean_outdir, topic, (int)getpid(), attempt) >= (int)sizeof staging_dir) {
            fprintf(stderr, "[attempt %d] staging directory path too long\n", attempt);
            break;
        }

        if (mkdir_p(staging_dir) != 0) {
            fprintf(stderr, "[attempt %d] cannot create staging directory '%s': %s\n",
                    attempt, staging_dir, strerror(errno));
            break;
        }

        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd < 0) {
            fprintf(stderr, "[attempt %d] socket creation failed: %s\n", attempt, strerror(errno));
            rm_rf(staging_dir);
            usleep(50000);
            continue;
        }

        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof saddr);
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
            fprintf(stderr, "[attempt %d] invalid server IP '%s'\n", attempt, server_ip);
            close(sfd);
            rm_rf(staging_dir);
            is_non_retryable = 1;
            break;
        }

        if (connect(sfd, (struct sockaddr *)&saddr, sizeof saddr) != 0) {
            fprintf(stderr, "[attempt %d] connection to %s:%ld failed: %s\n",
                    attempt, server_ip, port, strerror(errno));
            close(sfd);
            rm_rf(staging_dir);
            usleep(50000);
            continue;
        }

        conn_t c;
        conn_init(&c, sfd);

        /* Send MSG_GET_REQ from the beginning (offset 0) */
        uint8_t req[TOPIC_FIELD];
        memset(req, 0, sizeof req);
        snprintf((char *)req, sizeof req, "%s", topic);
        if (conn_send(&c, MSG_GET_REQ, attempt > 1 ? FLAG_RETRY : 0, 0, req, sizeof req, 0) != 0) {
            fprintf(stderr, "[attempt %d] send GET request failed\n", attempt);
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            rm_rf(staging_dir);
            usleep(50000);
            continue;
        }

        proto_hdr_t h;
        uint8_t pl[MAX_PAYLOAD];

        /* Receive MSG_MANIFEST_START */
        if (conn_recv(&c, &h, pl) != 0) {
            fprintf(stderr, "[attempt %d] failed to receive manifest start (%s)\n",
                    attempt, c.err ? c.err : "disconnected");
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            rm_rf(staging_dir);
            usleep(50000);
            continue;
        }

        if (h.type == MSG_ERROR) {
            uint16_t err_code = get16(pl);
            fprintf(stderr, "[attempt %d] server returned error code %u: %.*s\n",
                    attempt, err_code, (int)(h.len > 4 ? h.len - 4 : 0), (char *)(pl + 4));
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            rm_rf(staging_dir);
            if (err_code == ERR_TOPIC_NOT_FOUND || err_code == ERR_BAD_REQUEST) {
                is_non_retryable = 1;
            }
            break;
        }

        if (h.type != MSG_MANIFEST_START || h.len < 12 + SHA256_DIGEST_LEN) {
            fprintf(stderr, "[attempt %d] protocol violation: expected MSG_MANIFEST_START\n", attempt);
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            rm_rf(staging_dir);
            usleep(50000);
            continue;
        }

        uint32_t total_files = get32(pl);
        uint64_t total_bytes = get64(pl + 4);
        uint8_t expected_manifest_hash[SHA256_DIGEST_LEN];
        memcpy(expected_manifest_hash, pl + 12, SHA256_DIGEST_LEN);

        client_fentry_t *files = malloc(total_files * sizeof(client_fentry_t));
        if (!files && total_files > 0) {
            fprintf(stderr, "[attempt %d] memory allocation failed for %u files\n", attempt, total_files);
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            rm_rf(staging_dir);
            break;
        }

        int manifest_ok = 1;
        uint64_t sum_bytes = 0;
        for (uint32_t i = 0; i < total_files; i++) {
            if (conn_recv(&c, &h, pl) != 0 || h.type != MSG_MANIFEST_ENTRY) {
                manifest_ok = 0;
                break;
            }
            uint32_t fidx = get32(pl);
            if (fidx != i || h.len < 14 + SHA256_DIGEST_LEN) {
                manifest_ok = 0;
                break;
            }
            files[i].size = get64(pl + 4);
            memcpy(files[i].hash, pl + 12, SHA256_DIGEST_LEN);
            uint16_t nlen = get16(pl + 12 + SHA256_DIGEST_LEN);
            if (nlen > MAX_NAME || (uint32_t)(nlen + 14 + SHA256_DIGEST_LEN) != h.len) {
                manifest_ok = 0;
                break;
            }
            memcpy(files[i].name, pl + 14 + SHA256_DIGEST_LEN, nlen);
            files[i].name[nlen] = '\0';
            if (!safe_name(files[i].name)) {
                manifest_ok = 0;
                break;
            }
            sum_bytes += files[i].size;
        }

        if (!manifest_ok || sum_bytes != total_bytes) {
            fprintf(stderr, "[attempt %d] manifest entry stream corrupted or truncated\n", attempt);
            free(files);
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            rm_rf(staging_dir);
            usleep(50000);
            continue;
        }

        /* Receive MSG_MANIFEST_END */
        if (conn_recv(&c, &h, pl) != 0 || h.type != MSG_MANIFEST_END || get32(pl) != total_files) {
            fprintf(stderr, "[attempt %d] manifest end missing or inconsistent\n", attempt);
            free(files);
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            rm_rf(staging_dir);
            usleep(50000);
            continue;
        }

        /* Verify aggregate manifest hash */
        sha256_ctx_t m_ctx;
        sha256_init(&m_ctx);
        for (uint32_t i = 0; i < total_files; i++) {
            sha256_update(&m_ctx, files[i].name, strlen(files[i].name));
            sha256_update(&m_ctx, &files[i].size, sizeof(uint64_t));
            sha256_update(&m_ctx, files[i].hash, SHA256_DIGEST_LEN);
        }
        uint8_t local_manifest_hash[SHA256_DIGEST_LEN];
        sha256_final(&m_ctx, local_manifest_hash);
        if (memcmp(local_manifest_hash, expected_manifest_hash, SHA256_DIGEST_LEN) != 0) {
            fprintf(stderr, "[attempt %d] manifest integrity hash mismatch\n", attempt);
            free(files);
            total_protocol_bytes += c.tx_bytes + c.rx_bytes;
            close(sfd);
            rm_rf(staging_dir);
            usleep(50000);
            continue;
        }

        /* Receive files and chunks into attempt-specific staging directory */
        int transfer_ok = 1;
        for (uint32_t i = 0; i < total_files; i++) {
            if (conn_recv(&c, &h, pl) != 0 || h.type != MSG_FILE_HEADER) {
                transfer_ok = 0;
                break;
            }
            if (get32(pl) != i || get64(pl + 4) != files[i].size || get64(pl + 12) != 0) {
                transfer_ok = 0;
                break;
            }

            char fpath[PATH_MAX + 512];
            if (snprintf(fpath, sizeof fpath, "%s/%s", staging_dir, files[i].name) >= (int)sizeof fpath) {
                transfer_ok = 0;
                break;
            }

            int ofd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
            if (ofd < 0) {
                fprintf(stderr, "[attempt %d] cannot create file '%s': %s\n", attempt, fpath, strerror(errno));
                transfer_ok = 0;
                break;
            }

            struct stat ost;
            if (fstat(ofd, &ost) != 0 || !S_ISREG(ost.st_mode)) {
                close(ofd);
                transfer_ok = 0;
                break;
            }

            sha256_ctx_t f_ctx;
            sha256_init(&f_ctx);

            uint64_t file_got = 0;
            while (file_got < files[i].size) {
                if (conn_recv(&c, &h, pl) != 0) {
                    transfer_ok = 0;
                    break;
                }
                if (h.type != MSG_DATA_CHUNK) {
                    transfer_ok = 0;
                    break;
                }
                if (h.len > 0) {
                    write_full(ofd, pl, h.len, fpath);
                    sha256_update(&f_ctx, pl, h.len);
                    file_got += h.len;
                    attempt_payload_bytes += h.len;
                }
            }
            close(ofd);
            if (!transfer_ok) break;

            uint8_t calc_hash[SHA256_DIGEST_LEN];
            sha256_final(&f_ctx, calc_hash);
            if (memcmp(calc_hash, files[i].hash, SHA256_DIGEST_LEN) != 0) {
                fprintf(stderr, "[attempt %d] checksum verification failed for file '%s'\n", attempt, files[i].name);
                transfer_ok = 0;
                break;
            }
        }

        if (transfer_ok) {
            /* Receive MSG_TRANSFER_DONE */
            if (conn_recv(&c, &h, pl) == 0 && h.type == MSG_TRANSFER_DONE &&
                get32(pl) == total_files && get64(pl + 4) == attempt_payload_bytes &&
                memcmp(pl + 12, expected_manifest_hash, SHA256_DIGEST_LEN) == 0) {

                /* Transactional atomic publication */
                rm_rf(target_topic_dir);
                if (rename(staging_dir, target_topic_dir) == 0) {
                    success = 1;
                    useful_payload_bytes = attempt_payload_bytes;
                    transmitted_payload_bytes += attempt_payload_bytes;
                    total_protocol_bytes += (c.tx_bytes + c.rx_bytes) - attempt_payload_bytes;
                    free(files);
                    close(sfd);
                    break;
                } else {
                    fprintf(stderr, "[attempt %d] atomic publication rename failed: %s\n",
                            attempt, strerror(errno));
                }
            }
        }

        /* If we reached here without success, this attempt failed */
        transmitted_payload_bytes += attempt_payload_bytes;
        total_protocol_bytes += (c.tx_bytes + c.rx_bytes) - attempt_payload_bytes;
        free(files);
        close(sfd);
        rm_rf(staging_dir);
        usleep(50000);
    }

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
