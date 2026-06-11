#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef SAIL_COMMON_H
#define SAIL_COMMON_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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

#define SAIL_VERSION 2
#define SAIL_MAGIC 0x5341494cu
#define SAIL_MSG_MAGIC 0x534d5332u
#define SAIL_NOTIFY_MAGIC 0x534e5432u
#define SAIL_MAX_FLOWS 4096
#define SAIL_FLOW_BITS 12
#define SAIL_PACKET_BITS 20
#define SAIL_MAX_PACKETS (1u << SAIL_PACKET_BITS)
#define SAIL_DEFAULT_PMTU 1024u
#define SAIL_CTRL_RECV_DEPTH 1024
#define SAIL_CTRL_SEND_DEPTH 1024
#define SAIL_DATA_RECV_DEPTH 64
#define SAIL_DATA_SEND_DEPTH 128
#define SAIL_MAX_POLL 32
#define SAIL_TCP_DONE "DONE"
#define SAIL_TCP_DONE_LEN 4

#define DIE(fmt, ...)                                                           \
    do {                                                                        \
        fprintf(stderr, "fatal: " fmt "\n", ##__VA_ARGS__);                  \
        exit(EXIT_FAILURE);                                                     \
    } while (0)

enum sail_ctrl_type {
    SAIL_CTRL_NACK = 1,
    SAIL_CTRL_DONE_ACK = 2,
    SAIL_CTRL_DONE_ACK_ACK = 3,
};

enum sail_notify_type {
    SAIL_NOTIFY_LOSS = 1,
    SAIL_NOTIFY_COMPLETE = 2,
};

struct sail_qp_wire {
    uint16_t lid;
    uint32_t data_qpn;
    uint32_t ctrl_qpn;
    uint32_t data_psn;
    uint32_t ctrl_psn;
    uint32_t rkey;
    uint64_t addr;
    uint64_t buf_size;
    union ibv_gid gid;
};

struct sail_exchange {
    uint32_t magic;
    uint32_t version;
    uint32_t flows;
    uint32_t gid_index;
    uint32_t udp_port;
    uint32_t reserved;
    struct sail_qp_wire flow[SAIL_MAX_FLOWS];
};

struct sail_ctrl_msg {
    uint32_t magic;
    uint16_t type;
    uint16_t flow_id;
    uint32_t msg_id;
    uint32_t psn;
    uint32_t done_seq;
    uint32_t reserved;
};

struct sail_notify_msg {
    uint32_t magic;
    uint8_t type;
    uint8_t reserved[3];
    uint32_t dqp;
    uint32_t psn;
};

struct sail_context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_device_attr device_attr;
    struct ibv_port_attr port_attr;
    union ibv_gid gid;
    int ib_port;
    int gid_index;
};

struct sail_profile {
    uint64_t active_ns;
    uint64_t poll_hit;
    uint64_t poll_empty;
    uint64_t cqe_count;
    uint64_t post_send;
    uint64_t post_recv;
};

uint64_t sail_now_ns(void);
uint64_t sail_now_us(void);
uint64_t sail_realtime_us(void);
void sail_wait_until_realtime_us(uint64_t start_at_us);
double sail_timeval_sec(const struct timeval *tv);
uint32_t sail_rand_psn(void);
size_t sail_packets_for(size_t msg_size, size_t pmtu);
size_t sail_packet_len(size_t msg_size, size_t pmtu, uint32_t packet_id);
uint32_t sail_psn_add(uint32_t psn, uint32_t delta);
uint32_t sail_psn_diff(uint32_t psn, uint32_t start_psn);
uint32_t sail_imm(uint32_t flow_id, uint32_t packet_id);
void sail_parse_imm(uint32_t imm, uint32_t *flow_id, uint32_t *packet_id);

int sail_load_traffic_file(const char *path, int max_flows, size_t *sizes,
                           uint64_t *start_delay_us);
int sail_tcp_listen(const char *port);
int sail_tcp_accept(int listen_fd);
int sail_tcp_connect(const char *server, const char *port);
void sail_write_full(int fd, const void *buf, size_t len);
void sail_read_full(int fd, void *buf, size_t len);
void sail_set_nonblock(int fd);
int sail_udp_bind(const char *port);
int sail_udp_bind_addr(const char *addr, const char *port);
int sail_udp_connect(const char *server, const char *port);

void sail_open_device(struct sail_context *sc, const char *dev_name,
                      int ib_port, int gid_index);
struct ibv_qp *sail_create_qp(struct sail_context *sc, enum ibv_qp_type type,
                              struct ibv_cq *send_cq, struct ibv_cq *recv_cq,
                              int max_send_wr, int max_recv_wr);
void sail_connect_qp(struct sail_context *sc, struct ibv_qp *qp,
                     enum ibv_qp_type type, const struct sail_qp_wire *remote,
                     bool data_qp, uint32_t local_psn);
void sail_fill_local_wire(struct sail_context *sc, struct sail_qp_wire *wire,
                          struct ibv_qp *data_qp, struct ibv_qp *ctrl_qp,
                          uint32_t data_psn, uint32_t ctrl_psn,
                          struct ibv_mr *remote_write_mr, void *buf,
                          uint64_t buf_size);
void sail_exchange_info(int sock, const struct sail_exchange *local,
                        struct sail_exchange *remote);

#endif
