#ifndef RC_COMMON_H
#define RC_COMMON_H

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <infiniband/verbs.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define RC_VERSION 1
#define RC_MAGIC 0x52434241u
#define RC_MAX_FLOWS 4096
#define RC_MAX_POLL 32
#define RC_SEND_DEPTH 128
#define RC_TCP_DONE "DONE"
#define RC_TCP_DONE_LEN 4
#define RC_DEFAULT_QP_TIMEOUT 14
#define RC_DEFAULT_RETRY_CNT 7
#define RC_DEFAULT_RNR_RETRY 7

#define DIE(fmt, ...)                                                           \
    do {                                                                        \
        fprintf(stderr, "fatal: " fmt "\n", ##__VA_ARGS__);                  \
        exit(EXIT_FAILURE);                                                     \
    } while (0)

struct rc_qp_wire {
    uint16_t lid;
    uint32_t qpn;
    uint32_t psn;
    uint32_t rkey;
    uint64_t addr;
    uint64_t buf_size;
    union ibv_gid gid;
};

struct rc_exchange {
    uint32_t magic;
    uint32_t version;
    uint32_t flows;
    uint32_t gid_index;
    struct rc_qp_wire flow[RC_MAX_FLOWS];
};

struct rc_context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_device_attr device_attr;
    struct ibv_port_attr port_attr;
    union ibv_gid gid;
    int ib_port;
    int gid_index;
    uint8_t qp_timeout;
    uint8_t retry_cnt;
    uint8_t rnr_retry;
};

struct rc_profile {
    uint64_t active_ns;
    uint64_t poll_hit;
    uint64_t poll_empty;
    uint64_t cqe_count;
    uint64_t post_send;
    uint64_t post_recv;
};

uint64_t rc_now_ns(void);
uint64_t rc_now_us(void);
uint64_t rc_realtime_us(void);
void rc_wait_until_realtime_us(uint64_t start_at_us);
double rc_timeval_sec(const struct timeval *tv);
uint32_t rc_rand_psn(void);
int rc_load_traffic_file(const char *path, int max_flows, size_t *sizes,
                         uint64_t *start_delay_us);

int rc_tcp_listen(const char *port);
int rc_tcp_accept(int listen_fd);
int rc_tcp_connect(const char *server, const char *port);
void rc_write_full(int fd, const void *buf, size_t len);
void rc_read_full(int fd, void *buf, size_t len);

void rc_open_device(struct rc_context *rc, const char *dev_name,
                    int ib_port, int gid_index);
struct ibv_qp *rc_create_qp(struct rc_context *rc, struct ibv_cq *cq,
                            int max_send_wr, int max_recv_wr);
void rc_connect_qp(struct rc_context *rc, struct ibv_qp *qp,
                   const struct rc_qp_wire *remote, uint32_t local_psn);
void rc_fill_local_wire(struct rc_context *rc, struct rc_qp_wire *wire,
                        struct ibv_qp *qp, uint32_t psn,
                        struct ibv_mr *remote_write_mr, void *buf,
                        uint64_t buf_size);
void rc_exchange_info(int sock, const struct rc_exchange *local,
                      struct rc_exchange *remote);

#endif
