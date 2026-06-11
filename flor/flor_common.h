#ifndef FLOR_COMMON_H
#define FLOR_COMMON_H

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

#define FLOR_VERSION 1
#define FLOR_MAGIC 0x464c4f52u
#define FLOR_MAX_FLOWS 4096
#define FLOR_MAX_CHUNKS 65536
#define FLOR_IMM_FLOW_BITS 12
#define FLOR_IMM_CHUNK_BITS 20
#define FLOR_ACK_RECV_DEPTH 1024
#define FLOR_DATA_RECV_DEPTH 2048
#define FLOR_ACK_SEND_DEPTH 1024
#define FLOR_MAX_POLL 32
#define FLOR_CTRL_DONE "DONE"
#define FLOR_CTRL_DONE_LEN 4

#define FLOR_ACK_MAGIC 0x41434b31u
#define FLOR_ACK_FLAG_DONE 0x1u

#define DIE(fmt, ...)                                                           \
    do {                                                                        \
        fprintf(stderr, "fatal: " fmt "\n", ##__VA_ARGS__);                  \
        exit(EXIT_FAILURE);                                                     \
    } while (0)

#define CHECK_ZERO(expr, fmt, ...)                                              \
    do {                                                                        \
        int _rc = (expr);                                                       \
        if (_rc)                                                                \
            DIE(fmt ": %s", ##__VA_ARGS__, strerror(_rc));                     \
    } while (0)

struct flor_qp_wire {
    uint16_t lid;
    uint32_t data_qpn;
    uint32_t ack_qpn;
    uint32_t data_psn;
    uint32_t ack_psn;
    uint32_t rkey;
    uint64_t addr;
    uint64_t buf_size;
    union ibv_gid gid;
};

struct flor_exchange {
    uint32_t magic;
    uint32_t version;
    uint32_t flows;
    uint32_t gid_index;
    struct flor_qp_wire flow[FLOR_MAX_FLOWS];
};

struct flor_ack {
    uint32_t magic;
    uint16_t flow_id;
    uint16_t flags;
    uint32_t msg_id;
    uint32_t base_chunk;
    uint32_t cum_ack;
    uint32_t highest_ack;
    uint64_t bitmap;
};

struct flor_context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_device_attr device_attr;
    struct ibv_port_attr port_attr;
    union ibv_gid gid;
    int ib_port;
    int gid_index;
};

struct flor_profile {
    uint64_t active_ns;
    uint64_t poll_hit;
    uint64_t poll_empty;
    uint64_t cqe_count;
    uint64_t post_send;
    uint64_t post_recv;
};

uint64_t flor_now_ns(void);
uint64_t flor_now_us(void);
uint64_t flor_realtime_us(void);
void flor_wait_until_realtime_us(uint64_t start_at_us);
double flor_timeval_sec(const struct timeval *tv);
uint32_t flor_rand_psn(void);
size_t flor_chunks_for(size_t msg_size, size_t chunk_size);
size_t flor_chunk_len(size_t msg_size, size_t chunk_size, uint32_t chunk_id);
uint32_t flor_imm(uint32_t flow_id, uint32_t chunk_id);
void flor_parse_imm(uint32_t imm, uint32_t *flow_id, uint32_t *chunk_id);
int flor_load_traffic_file(const char *path, int max_flows, size_t *sizes,
                           uint64_t *start_delay_us);
int flor_tcp_listen(const char *port);
int flor_tcp_accept(int listen_fd);
int flor_tcp_connect(const char *server, const char *port);
void flor_write_full(int fd, const void *buf, size_t len);
void flor_read_full(int fd, void *buf, size_t len);

void flor_open_device(struct flor_context *fc, const char *dev_name,
                      int ib_port, int gid_index);
struct ibv_qp *flor_create_uc_qp(struct flor_context *fc,
                                 struct ibv_cq *send_cq,
                                 struct ibv_cq *recv_cq,
                                 int max_send_wr,
                                 int max_recv_wr);
void flor_connect_uc_qp(struct flor_context *fc, struct ibv_qp *qp,
                        const struct flor_qp_wire *remote, bool data_qp,
                        uint32_t local_psn);
void flor_fill_local_wire(struct flor_context *fc, struct flor_qp_wire *wire,
                          struct ibv_qp *data_qp, struct ibv_qp *ack_qp,
                          uint32_t data_psn, uint32_t ack_psn,
                          struct ibv_mr *remote_write_mr, void *buf,
                          uint64_t buf_size);

void flor_exchange_info(int sock, const struct flor_exchange *local,
                        struct flor_exchange *remote);

#endif
