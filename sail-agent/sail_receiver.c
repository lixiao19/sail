#include "sail_common.h"

enum {
    SAIL_WR_CTRL_SEND = 2,
    SAIL_WR_CTRL_RECV = 3,
};

#define SAIL_UDP_RECV_BATCH_MAX 512

struct receiver_opts {
    const char *dev_name;
    const char *port;
    const char *notify_port;
    const char *notify_bind_ip;
    const char *traffic_file;
    int ib_port;
    int gid_index;
    int flows;
    size_t msg_size;
    size_t pmtu;
    uint64_t nack_timeout_us;
    uint64_t done_timeout_us;
    int udp_rcvbuf;
    int udp_burst;
    bool enable_fast_nack;
};

struct receiver_stats {
    uint64_t loss_notifs;
    uint64_t complete_notifs;
    uint64_t duplicate_complete_notifs;
    uint64_t unmatched_notifs;
    uint64_t nacks;
    uint64_t repair_writes;
    uint64_t done_acks;
    uint64_t done_ack_acks;
    uint64_t messages_delivered;
    uint64_t bytes_delivered;
    uint64_t fast_nack_resends;
    uint64_t timeout_nack_resends;
    uint64_t deferred_nacks;
};

struct notify_flow_map_entry {
    uint32_t dqp;
    int flow_idx;
    bool used;
};

struct receiver_flow {
    int id;
    size_t msg_size;
    size_t packets;
    uint32_t data_psn;
    uint32_t ctrl_psn;
    uint32_t data_qpn;
    uint32_t start_psn;
    uint32_t received_loss_count;
    uint32_t repaired_count;
    uint32_t unrepaired_loss_count;
    uint32_t done_seq;
    bool complete_seen;
    bool done_sent;
    bool done_acked;
    bool delivered_counted;
    uint64_t last_done_us;

    uint8_t *data_buf;
    struct ibv_mr *data_mr;
    struct sail_ctrl_msg *ctrl_recv_bufs;
    struct sail_ctrl_msg *ctrl_send_bufs;
    struct ibv_mr *ctrl_recv_mr;
    struct ibv_mr *ctrl_send_mr;
    uint8_t *lost;
    uint8_t *repaired;
    uint8_t *fast_nack_sent;
    uint64_t *last_nack_us;
    uint32_t *loss_list;
    uint32_t loss_count;
    uint32_t loss_cap;

    struct ibv_cq *ctrl_cq;
    struct ibv_qp *data_qp;
    struct ibv_qp *ctrl_qp;
    int outstanding_ctrl;
    uint32_t ctrl_send_head;
};

static struct sail_profile *g_prof;

static uint64_t wr_id_make(uint8_t type, uint16_t flow, uint16_t slot)
{
    return ((uint64_t)type << 56) | ((uint64_t)flow << 32) | slot;
}

static uint8_t wr_id_type(uint64_t wr_id)
{
    return (uint8_t)(wr_id >> 56);
}

static uint16_t wr_id_flow(uint64_t wr_id)
{
    return (uint16_t)(wr_id >> 32);
}

static uint16_t wr_id_slot(uint64_t wr_id)
{
    return (uint16_t)wr_id;
}

static uint64_t parse_u64(const char *s, const char *name)
{
    char *end = NULL;
    unsigned long long v;

    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0')
        DIE("invalid %s: %s", name, s);
    return (uint64_t)v;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --port PORT                         TCP bootstrap port (default 20515)\n"
            "  --notify-port PORT                  UDP notification port (default 20516)\n"
            "  --notify-bind-ip IP                 bind UDP notifications to this local IP\n"
            "  --dev NAME --ib-port N --gid-index N\n"
            "  --flows N                           flow count (default 1)\n"
            "  --msg-size BYTES | --traffic-file PATH\n"
            "  --pmtu BYTES                        simulated packet size (default 1024)\n"
            "  --nack-timeout-us US                NACK resend timeout (default 50000)\n"
            "  --done-timeout-us US                DONE_ACK resend timeout (default 50000)\n"
            "  --udp-rcvbuf BYTES                  UDP socket receive buffer (default 67108864)\n"
            "  --udp-burst N                       UDP notifications handled per loop (default 256)\n"
            "  --enable-fast-nack                  resend earlier NACKs when later repairs arrive\n",
            prog);
}

static void parse_opts(int argc, char **argv, struct receiver_opts *o)
{
    static const struct option long_opts[] = {
        {"port", required_argument, NULL, 'p'},
        {"notify-port", required_argument, NULL, 'n'},
        {"notify-bind-ip", required_argument, NULL, 'a'},
        {"dev", required_argument, NULL, 'd'},
        {"ib-port", required_argument, NULL, 'i'},
        {"gid-index", required_argument, NULL, 'g'},
        {"flows", required_argument, NULL, 'f'},
        {"msg-size", required_argument, NULL, 'z'},
        {"traffic-file", required_argument, NULL, 'x'},
        {"pmtu", required_argument, NULL, 'u'},
        {"nack-timeout-us", required_argument, NULL, 't'},
        {"done-timeout-us", required_argument, NULL, 'T'},
        {"udp-rcvbuf", required_argument, NULL, 'R'},
        {"udp-burst", required_argument, NULL, 'B'},
        {"enable-fast-nack", no_argument, NULL, 'F'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int ch;

    *o = (struct receiver_opts){
        .dev_name = NULL,
        .port = "20515",
        .notify_port = "20516",
        .notify_bind_ip = NULL,
        .traffic_file = NULL,
        .ib_port = 1,
        .gid_index = -1,
        .flows = 1,
        .msg_size = 1024 * 1024,
        .pmtu = SAIL_DEFAULT_PMTU,
        .nack_timeout_us = 50000,
        .done_timeout_us = 50000,
        .udp_rcvbuf = 64 * 1024 * 1024,
        .udp_burst = 256,
        .enable_fast_nack = false,
    };
    while ((ch = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (ch) {
        case 'p': o->port = optarg; break;
        case 'n': o->notify_port = optarg; break;
        case 'a': o->notify_bind_ip = optarg; break;
        case 'd': o->dev_name = optarg; break;
        case 'i': o->ib_port = (int)parse_u64(optarg, "ib-port"); break;
        case 'g': o->gid_index = (int)parse_u64(optarg, "gid-index"); break;
        case 'f': o->flows = (int)parse_u64(optarg, "flows"); break;
        case 'z': o->msg_size = (size_t)parse_u64(optarg, "msg-size"); break;
        case 'x': o->traffic_file = optarg; break;
        case 'u': o->pmtu = (size_t)parse_u64(optarg, "pmtu"); break;
        case 't': o->nack_timeout_us = parse_u64(optarg, "nack-timeout-us"); break;
        case 'T': o->done_timeout_us = parse_u64(optarg, "done-timeout-us"); break;
        case 'R': o->udp_rcvbuf = (int)parse_u64(optarg, "udp-rcvbuf"); break;
        case 'B': o->udp_burst = (int)parse_u64(optarg, "udp-burst"); break;
        case 'F': o->enable_fast_nack = true; break;
        case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
        default: usage(argv[0]); exit(EXIT_FAILURE);
        }
    }
    if (o->flows <= 0 || o->flows > SAIL_MAX_FLOWS)
        DIE("--flows must be in [1,%d]", SAIL_MAX_FLOWS);
    if (o->pmtu == 0 || o->pmtu > SAIL_DEFAULT_PMTU)
        DIE("--pmtu must be in [1,%u]", SAIL_DEFAULT_PMTU);
    if (o->nack_timeout_us == 0 || o->done_timeout_us == 0)
        DIE("timeouts must be positive");
    if (o->udp_rcvbuf <= 0 || o->udp_burst <= 0)
        DIE("UDP receive buffer and burst must be positive");
}

static int shared_cq_size(struct sail_context *ctx, int requested)
{
    if (requested < SAIL_MAX_POLL)
        requested = SAIL_MAX_POLL;
    if (ctx->device_attr.max_cqe > 0 && requested > ctx->device_attr.max_cqe)
        requested = ctx->device_attr.max_cqe;
    return requested;
}

static void prof_post_send(void)
{
    if (g_prof)
        g_prof->post_send++;
}

static void prof_post_recv(void)
{
    if (g_prof)
        g_prof->post_recv++;
}

static void post_ctrl_recv(struct receiver_flow *f, uint16_t slot)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)&f->ctrl_recv_bufs[slot],
        .length = sizeof(f->ctrl_recv_bufs[slot]),
        .lkey = f->ctrl_recv_mr->lkey,
    };
    struct ibv_recv_wr wr = {
        .wr_id = wr_id_make(SAIL_WR_CTRL_RECV, (uint16_t)f->id, slot),
        .sg_list = &sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad = NULL;
    // uint64_t t0 = sail_now_ns();

    if (ibv_post_recv(f->ctrl_qp, &wr, &bad))
        DIE("ibv_post_recv receiver ctrl flow %d", f->id);
    // if (g_prof)
    //     g_prof->active_ns += sail_now_ns() - t0;
    prof_post_recv();
}

static bool post_ctrl_send(struct receiver_flow *f, enum sail_ctrl_type type,
                           uint32_t psn, struct receiver_stats *st)
{
    uint16_t slot;
    struct sail_ctrl_msg *msg;
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad = NULL;

    if (f->outstanding_ctrl >= SAIL_CTRL_SEND_DEPTH)
        return false;
    slot = (uint16_t)(f->ctrl_send_head++ % SAIL_CTRL_SEND_DEPTH);
    msg = &f->ctrl_send_bufs[slot];
    *msg = (struct sail_ctrl_msg){
        .magic = SAIL_MSG_MAGIC,
        .type = (uint16_t)type,
        .flow_id = (uint16_t)f->id,
        .msg_id = 0,
        .psn = psn,
        .done_seq = f->done_seq,
    };
    sge = (struct ibv_sge){
        .addr = (uintptr_t)msg,
        .length = sizeof(*msg),
        .lkey = f->ctrl_send_mr->lkey,
    };
    wr = (struct ibv_send_wr){
        .wr_id = wr_id_make(SAIL_WR_CTRL_SEND, (uint16_t)f->id, slot),
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    if (ibv_post_send(f->ctrl_qp, &wr, &bad))
        DIE("ibv_post_send receiver ctrl flow %d", f->id);
    // if (g_prof)
    //     g_prof->active_ns += sail_now_ns() - t0;
    prof_post_send();
    f->outstanding_ctrl++;
    if (type == SAIL_CTRL_NACK)
        st->nacks++;
    else if (type == SAIL_CTRL_DONE_ACK)
        st->done_acks++;
    return true;
}

static bool send_nack(struct receiver_flow *f, uint32_t packet_id,
                      struct receiver_stats *st)
{
    uint32_t psn = sail_psn_add(f->start_psn, packet_id);

    if (!post_ctrl_send(f, SAIL_CTRL_NACK, psn, st))
        return false;
    f->last_nack_us[packet_id] = sail_now_us();
    return true;
}

static void append_loss(struct receiver_flow *f, uint32_t packet_id)
{
    if (f->loss_count == f->loss_cap) {
        uint32_t new_cap = f->loss_cap ? f->loss_cap * 2u : 16u;
        uint32_t *new_list;

        if (new_cap < f->loss_cap || new_cap > f->packets)
            new_cap = (uint32_t)f->packets;
        new_list = realloc(f->loss_list, (size_t)new_cap * sizeof(*new_list));
        if (!new_list)
            DIE("realloc loss list flow %d", f->id);
        f->loss_list = new_list;
        f->loss_cap = new_cap;
    }
    f->loss_list[f->loss_count++] = packet_id;
}

static uint32_t find_loss_index(const struct receiver_flow *f, uint32_t packet_id)
{
    uint32_t i;

    for (i = 0; i < f->loss_count; i++) {
        if (f->loss_list[i] == packet_id)
            return i;
    }
    return UINT32_MAX;
}

static void remove_loss_index(struct receiver_flow *f, uint32_t idx)
{
    if (idx >= f->loss_count)
        DIE("bad loss list index on flow %d", f->id);
    if (idx + 1u < f->loss_count) {
        memmove(&f->loss_list[idx], &f->loss_list[idx + 1u],
                (size_t)(f->loss_count - idx - 1u) * sizeof(*f->loss_list));
    }
    f->loss_count--;
}

static void maybe_send_done(struct receiver_flow *f,
                            const struct receiver_opts *o,
                            struct receiver_stats *st)
{
    uint64_t now;

    if (!f->complete_seen || f->unrepaired_loss_count != 0 || f->done_acked)
        return;

    now = sail_now_us();
    if (f->done_sent && now - f->last_done_us < o->done_timeout_us)
        return;
    if (!f->done_sent) {
        f->done_seq++;
        f->done_sent = true;
        if (!f->delivered_counted) {
            st->messages_delivered++;
            st->bytes_delivered += f->msg_size;
            f->delivered_counted = true;
        }
    }
    if (post_ctrl_send(f, SAIL_CTRL_DONE_ACK, 0, st))
        f->last_done_us = now;
}

static uint32_t hash_dqp(uint32_t dqp)
{
    dqp ^= dqp >> 16;
    dqp *= 0x7feb352du;
    dqp ^= dqp >> 15;
    return dqp;
}

static size_t next_pow2_size(size_t v)
{
    size_t p = 1;

    while (p < v)
        p <<= 1;
    return p;
}

static struct notify_flow_map_entry *build_notify_map(struct receiver_flow *flows,
                                                      int nflows,
                                                      size_t *map_mask)
{
    size_t map_size = next_pow2_size((size_t)nflows * 4u);
    struct notify_flow_map_entry *map = calloc(map_size, sizeof(*map));
    int i;

    if (!map)
        DIE("calloc notify map");
    for (i = 0; i < nflows; i++) {
        uint32_t dqp = flows[i].data_qpn & 0xffffffu;
        size_t idx = hash_dqp(dqp) & (map_size - 1u);

        while (map[idx].used)
            idx = (idx + 1u) & (map_size - 1u);
        map[idx].used = true;
        map[idx].dqp = dqp;
        map[idx].flow_idx = i;
    }
    *map_mask = map_size - 1u;
    return map;
}

static struct receiver_flow *find_notify_flow(struct receiver_flow *flows,
                                              const struct notify_flow_map_entry *map,
                                              size_t map_mask, uint32_t dqp,
                                              uint32_t psn,
                                              uint32_t *packet_id)
{
    size_t idx = hash_dqp(dqp) & map_mask;

    while (map[idx].used) {
        if (map[idx].dqp == (dqp & 0xffffffu)) {
            struct receiver_flow *f = &flows[map[idx].flow_idx];
            uint32_t pid = sail_psn_diff(psn, f->start_psn);

            if (pid < f->packets) {
                *packet_id = pid;
                return f;
            }
            return NULL;
        }
        idx = (idx + 1u) & map_mask;
    }
    return NULL;
}

static void handle_loss_notify(struct receiver_flow *f, uint32_t packet_id,
                               struct receiver_stats *st)
{
    if (packet_id >= f->packets)
        DIE("loss PSN outside message on flow %d", f->id);
    if (!f->lost[packet_id]) {
        f->lost[packet_id] = 1;
        append_loss(f, packet_id);
        f->received_loss_count++;
        f->unrepaired_loss_count++;
        if (!send_nack(f, packet_id, st))
            st->deferred_nacks++;
    }
    st->loss_notifs++;
}

static void handle_complete_notify(struct receiver_flow *f, uint32_t packet_id,
                                   const struct receiver_opts *o,
                                   struct receiver_stats *st)
{
    if (packet_id != f->packets - 1)
        DIE("completion PSN is not the last packet on flow %d", f->id);
    if (f->complete_seen)
        st->duplicate_complete_notifs++;
    f->complete_seen = true;
    st->complete_notifs++;
    maybe_send_done(f, o, st);
}

static void handle_udp_notification(struct receiver_flow *flows,
                                    const struct notify_flow_map_entry *map,
                                    size_t map_mask,
                                    const struct receiver_opts *o,
                                    struct receiver_stats *st,
                                    const struct sail_notify_msg *msg)
{
    struct receiver_flow *f;
    uint32_t dqp;
    uint32_t psn;
    uint32_t packet_id;

    if (ntohl(msg->magic) != SAIL_NOTIFY_MAGIC)
        DIE("bad UDP notification");
    dqp = ntohl(msg->dqp) & 0xffffffu;
    psn = ntohl(msg->psn) & 0xffffffu;
    f = find_notify_flow(flows, map, map_mask, dqp, psn, &packet_id);
    if (!f) {
        st->unmatched_notifs++;
        return;
    }
    if (msg->type == SAIL_NOTIFY_LOSS)
        handle_loss_notify(f, packet_id, st);
    else if (msg->type == SAIL_NOTIFY_COMPLETE)
        handle_complete_notify(f, packet_id, o, st);
    else
        DIE("unknown UDP notification type %u", msg->type);
}

static void poll_udp_notifications(int udp_fd, struct receiver_flow *flows,
                                   const struct notify_flow_map_entry *map,
                                   size_t map_mask,
                                   const struct receiver_opts *o,
                                   struct receiver_stats *st)
{
    int handled = 0;
    struct sail_notify_msg msgs[SAIL_UDP_RECV_BATCH_MAX];
    struct iovec iov[SAIL_UDP_RECV_BATCH_MAX];
    struct mmsghdr mhdr[SAIL_UDP_RECV_BATCH_MAX];
    int i;

    memset(mhdr, 0, sizeof(mhdr));
    for (i = 0; i < SAIL_UDP_RECV_BATCH_MAX; i++) {
        iov[i].iov_base = &msgs[i];
        iov[i].iov_len = sizeof(msgs[i]);
        mhdr[i].msg_hdr.msg_iov = &iov[i];
        mhdr[i].msg_hdr.msg_iovlen = 1;
    }

    while (handled < o->udp_burst) {
        int want = o->udp_burst - handled;
        int n;
        int j;
        uint64_t t0 = sail_now_ns();

        if (want > SAIL_UDP_RECV_BATCH_MAX)
            want = SAIL_UDP_RECV_BATCH_MAX;
        n = recvmmsg(udp_fd, mhdr, (unsigned int)want, MSG_DONTWAIT, NULL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            DIE("recvmmsg UDP notify: %s", strerror(errno));
        }
        if (n == 0)
            return;
        for (j = 0; j < n; j++) {
            if (mhdr[j].msg_len != sizeof(msgs[j]))
                DIE("short UDP notification");
            handle_udp_notification(flows, map, map_mask, o, st, &msgs[j]);
        }
        handled += n;
        if (g_prof) {
            g_prof->active_ns += sail_now_ns() - t0;
        }
    }
}

static void configure_udp_rcvbuf(int udp_fd, int requested)
{
    int actual = 0;
    socklen_t len = sizeof(actual);

    if (setsockopt(udp_fd, SOL_SOCKET, SO_RCVBUF, &requested,
                   sizeof(requested)) != 0) {
        fprintf(stderr, "warning: failed to set UDP SO_RCVBUF=%d: %s\n",
                requested, strerror(errno));
    }
    if (getsockopt(udp_fd, SOL_SOCKET, SO_RCVBUF, &actual, &len) == 0) {
        fprintf(stderr, "udp_rcvbuf_requested=%d udp_rcvbuf_actual=%d\n",
                requested, actual);
    }
}

static void handle_repair_imm(struct receiver_flow *f, uint32_t imm,
                              const struct receiver_opts *o,
                              struct receiver_stats *st)
{
    uint32_t flow_id;
    uint32_t packet_id;
    uint32_t i;
    uint32_t loss_idx;

    (void)o;
    sail_parse_imm(imm, &flow_id, &packet_id);
    if (flow_id != (uint32_t)f->id || packet_id >= f->packets)
        DIE("bad repair immediate on flow %d", f->id);
    if (!f->lost[packet_id])
        DIE("repair for unknown loss on flow %d packet %u", f->id, packet_id);
    if (!f->repaired[packet_id]) {
        loss_idx = find_loss_index(f, packet_id);
        if (loss_idx == UINT32_MAX)
            DIE("repair for non-pending loss on flow %d packet %u", f->id,
                packet_id);
        if (o->enable_fast_nack) {
            for (i = 0; i < loss_idx; i++) {
                uint32_t pending = f->loss_list[i];

                if (f->fast_nack_sent[pending])
                    continue;
                if (send_nack(f, pending, st))
                    st->fast_nack_resends++;
                else
                    st->deferred_nacks++;
                f->fast_nack_sent[pending] = 1;
            }
        }
        f->repaired[packet_id] = 1;
        f->repaired_count++;
        if (f->unrepaired_loss_count == 0)
            DIE("repair underflow on flow %d packet %u", f->id, packet_id);
        f->unrepaired_loss_count--;
        remove_loss_index(f, loss_idx);
    }
    st->repair_writes++;
}

static void poll_ctrl_cq(struct receiver_flow *flows, int nflows,
                         const struct receiver_opts *o,
                         struct receiver_stats *st, struct ibv_cq *ctrl_cq)
{
    struct ibv_wc wc[SAIL_MAX_POLL];

    while (true) {
        uint64_t t0 = sail_now_ns();
        int ne = ibv_poll_cq(ctrl_cq, SAIL_MAX_POLL, wc);
        int j;

        if (ne < 0)
            DIE("ibv_poll_cq receiver ctrl");
        if (g_prof) {
            if (ne > 0) {
                g_prof->poll_hit++;
                g_prof->cqe_count += (uint64_t)ne;
            } else {
                g_prof->poll_empty++;
            }
        }
        if (ne == 0)
            return;
        for (j = 0; j < ne; j++) {
            uint16_t fid = wr_id_flow(wc[j].wr_id);
            uint16_t slot = wr_id_slot(wc[j].wr_id);
            struct receiver_flow *f;
            uint8_t type = wr_id_type(wc[j].wr_id);

            if (fid >= (uint16_t)nflows)
                DIE("receiver ctrl CQE bad flow %u", fid);
            f = &flows[fid];
            if (wc[j].status != IBV_WC_SUCCESS)
                DIE("receiver ctrl CQE flow %u status %s", fid,
                    ibv_wc_status_str(wc[j].status));
            if (type == SAIL_WR_CTRL_SEND) {
                f->outstanding_ctrl--;
            } else if (type == SAIL_WR_CTRL_RECV) {
                if (wc[j].opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
                    if (!(wc[j].wc_flags & IBV_WC_WITH_IMM))
                        DIE("repair CQE missing immediate");
                    handle_repair_imm(f, ntohl(wc[j].imm_data), o, st);
                    maybe_send_done(f, o, st);
                } else if (wc[j].opcode == IBV_WC_RECV) {
                    struct sail_ctrl_msg *msg = &f->ctrl_recv_bufs[slot];

                    if (msg->magic != SAIL_MSG_MAGIC)
                        DIE("bad receiver ctrl magic");
                    if (msg->type == SAIL_CTRL_DONE_ACK_ACK) {
                        if (msg->done_seq == f->done_seq)
                            f->done_acked = true;
                        st->done_ack_acks++;
                    } else {
                        DIE("unexpected receiver ctrl type %u", msg->type);
                    }
                } else {
                    DIE("unexpected receiver ctrl opcode %d", wc[j].opcode);
                }
                post_ctrl_recv(f, slot);
            } else {
                DIE("unexpected receiver wr_id type");
            }
        }
        if (g_prof)
            g_prof->active_ns += sail_now_ns() - t0;
    }
}

static void process_timeouts(struct receiver_flow *flows, int nflows,
                             const struct receiver_opts *o,
                             struct receiver_stats *st)
{
    uint64_t now = sail_now_us();
    int i;

    for (i = 0; i < nflows; i++) {
        uint32_t j;
        struct receiver_flow *f = &flows[i];

        for (j = 0; j < f->loss_count; j++) {
            uint32_t p = f->loss_list[j];

            if (now - f->last_nack_us[p] >= o->nack_timeout_us) {
                if (send_nack(f, p, st))
                    st->timeout_nack_resends++;
                else
                    st->deferred_nacks++;
            }
        }
        maybe_send_done(f, o, st);
    }
}

static bool control_done(int sock)
{
    char buf[SAIL_TCP_DONE_LEN];
    ssize_t n = recv(sock, buf, SAIL_TCP_DONE_LEN, MSG_DONTWAIT | MSG_PEEK);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return false;
        DIE("recv TCP DONE: %s", strerror(errno));
    }
    if (n == 0 || n < SAIL_TCP_DONE_LEN)
        return false;
    if (memcmp(buf, SAIL_TCP_DONE, SAIL_TCP_DONE_LEN) != 0)
        DIE("unexpected TCP trailer");
    sail_read_full(sock, buf, SAIL_TCP_DONE_LEN);
    return true;
}

static int outstanding_ctrl(const struct receiver_flow *flows, int nflows)
{
    int i;
    int out = 0;

    for (i = 0; i < nflows; i++)
        out += flows[i].outstanding_ctrl;
    return out;
}

static void alloc_flow(struct sail_context *ctx, struct receiver_flow *f,
                       int id, size_t msg_size, const struct receiver_opts *o,
                       struct ibv_cq *ctrl_cq)
{
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;

    memset(f, 0, sizeof(*f));
    f->id = id;
    f->msg_size = msg_size;
    f->packets = sail_packets_for(msg_size, o->pmtu);
    f->data_psn = sail_rand_psn();
    f->ctrl_psn = sail_rand_psn();
    if (f->packets == 0 || f->packets > SAIL_MAX_PACKETS)
        DIE("flow %d packet count out of range", id);

    if (posix_memalign((void **)&f->data_buf, 4096, msg_size))
        DIE("posix_memalign receiver data");
    memset(f->data_buf, 0, msg_size);
    f->data_mr = ibv_reg_mr(ctx->pd, f->data_buf, msg_size, access);
    if (!f->data_mr)
        DIE("ibv_reg_mr receiver data");

    f->ctrl_recv_bufs = calloc(SAIL_CTRL_RECV_DEPTH, sizeof(*f->ctrl_recv_bufs));
    f->ctrl_send_bufs = calloc(SAIL_CTRL_SEND_DEPTH, sizeof(*f->ctrl_send_bufs));
    f->lost = calloc(f->packets, 1);
    f->repaired = calloc(f->packets, 1);
    f->fast_nack_sent = calloc(f->packets, 1);
    f->last_nack_us = calloc(f->packets, sizeof(*f->last_nack_us));
    if (!f->ctrl_recv_bufs || !f->ctrl_send_bufs || !f->lost ||
        !f->repaired || !f->fast_nack_sent || !f->last_nack_us)
        DIE("calloc receiver flow buffers");

    f->ctrl_recv_mr = ibv_reg_mr(ctx->pd, f->ctrl_recv_bufs,
                                 SAIL_CTRL_RECV_DEPTH * sizeof(*f->ctrl_recv_bufs),
                                 access);
    f->ctrl_send_mr = ibv_reg_mr(ctx->pd, f->ctrl_send_bufs,
                                 SAIL_CTRL_SEND_DEPTH * sizeof(*f->ctrl_send_bufs),
                                 access);
    if (!f->ctrl_recv_mr || !f->ctrl_send_mr)
        DIE("ibv_reg_mr receiver control");

    f->ctrl_cq = ctrl_cq;
    f->data_qp = sail_create_qp(ctx, IBV_QPT_RC, f->ctrl_cq, f->ctrl_cq, 1, 1);
    f->ctrl_qp = sail_create_qp(ctx, IBV_QPT_UC, f->ctrl_cq, f->ctrl_cq,
                                SAIL_CTRL_SEND_DEPTH + 64,
                                SAIL_CTRL_RECV_DEPTH);
    f->data_qpn = f->data_qp->qp_num & 0xffffffu;
}

static void post_initial_recvs(struct receiver_flow *flows, int nflows)
{
    int i;
    uint16_t slot;

    for (i = 0; i < nflows; i++) {
        for (slot = 0; slot < SAIL_CTRL_RECV_DEPTH; slot++)
            post_ctrl_recv(&flows[i], slot);
    }
}

static void print_stats(const struct receiver_opts *o,
                        const struct receiver_stats *st,
                        const struct sail_profile *prof,
                        uint64_t wall_us, clock_t cpu_start, clock_t cpu_end,
                        const struct rusage *ru_start,
                        const struct rusage *ru_end)
{
    double wall_s = (double)wall_us / 1000000.0;
    double cpu_s = (double)(cpu_end - cpu_start) / (double)CLOCKS_PER_SEC;
    double active_cpu_s = (double)prof->active_ns / 1e9;
    double user_s = sail_timeval_sec(&ru_end->ru_utime) -
                    sail_timeval_sec(&ru_start->ru_utime);
    double sys_s = sail_timeval_sec(&ru_end->ru_stime) -
                   sail_timeval_sec(&ru_start->ru_stime);

    printf("role,flows,pmtu,total_bytes,goodput_gbps\n");
    printf("receiver,%d,%zu,%" PRIu64 ",%.6f\n", o->flows, o->pmtu,
           st->bytes_delivered,
           wall_s > 0.0 ? ((double)st->bytes_delivered * 8.0 / wall_s) / 1e9 : 0.0);
    printf("loss_notifs,complete_notifs,nacks,repair_writes,done_acks,done_ack_acks\n");
    printf("%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 "\n",
           st->loss_notifs, st->complete_notifs, st->nacks,
           st->repair_writes, st->done_acks, st->done_ack_acks);
    printf("duplicate_complete_notifs\n");
    printf("%" PRIu64 "\n", st->duplicate_complete_notifs);
    printf("unmatched_notifs\n");
    printf("%" PRIu64 "\n", st->unmatched_notifs);
    printf("messages_delivered,fast_nack_resends,timeout_nack_resends,deferred_nacks\n");
    printf("%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
           st->messages_delivered, st->fast_nack_resends,
           st->timeout_nack_resends, st->deferred_nacks);
    printf("cpu_user_s,cpu_sys_s,cpu_percent\n");
    printf("%.6f,%.6f,%.4f\n", user_s, sys_s,
           wall_s > 0.0 ? cpu_s / wall_s * 100.0 : 0.0);
    printf("effective_cpu_s,effective_cpu_percent,poll_hit,poll_empty,cqe_count,post_send,post_recv\n");
    printf("%.9f,%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 "\n",
           active_cpu_s, wall_s > 0.0 ? active_cpu_s / wall_s * 100.0 : 0.0,
           prof->poll_hit, prof->poll_empty, prof->cqe_count,
           prof->post_send, prof->post_recv);
}

static void print_incomplete_flows(const struct receiver_flow *flows, int nflows)
{
    int i;
    int printed = 0;

    fprintf(stderr,
            "receiver_incomplete_flows: flow,msg_size,packets,complete_seen,"
            "received_loss,repaired,unrepaired,done_sent,done_acked\n");
    for (i = 0; i < nflows; i++) {
        const struct receiver_flow *f = &flows[i];

        if (f->done_acked)
            continue;
        fprintf(stderr, "%d,%zu,%zu,%u,%u,%u,%u,%u,%u\n",
                f->id, f->msg_size, f->packets, f->complete_seen ? 1u : 0u,
                f->received_loss_count, f->repaired_count,
                f->unrepaired_loss_count, f->done_sent ? 1u : 0u,
                f->done_acked ? 1u : 0u);
        printed++;
        if (printed >= 64)
            break;
    }
    if (printed == 0)
        fprintf(stderr, "receiver_incomplete_flows: none\n");
}

int main(int argc, char **argv)
{
    struct receiver_opts opts;
    struct sail_context ctx;
    struct receiver_flow *flows;
    struct sail_exchange *local;
    struct sail_exchange *remote;
    struct notify_flow_map_entry *notify_map;
    size_t notify_map_mask;
    struct ibv_cq *shared_ctrl_cq;
    struct receiver_stats stats = { 0 };
    struct sail_profile prof = { 0 };
    size_t *sizes;
    uint64_t *starts;
    int listen_fd;
    int sock;
    int udp_fd;
    bool tcp_done = false;
    uint64_t start_us;
    uint64_t end_us;
    clock_t cpu_start;
    clock_t cpu_end;
    struct rusage ru_start;
    struct rusage ru_end;
    int i;

    parse_opts(argc, argv, &opts);
    g_prof = &prof;
    srand((unsigned int)(time(NULL) ^ getpid()));
    srand48((long)(time(NULL) ^ getpid()));

    sizes = calloc(SAIL_MAX_FLOWS, sizeof(*sizes));
    starts = calloc(SAIL_MAX_FLOWS, sizeof(*starts));
    if (!sizes || !starts)
        DIE("calloc traffic arrays");
    if (opts.traffic_file) {
        int inferred = sail_load_traffic_file(opts.traffic_file, SAIL_MAX_FLOWS,
                                              sizes, starts);
        if (opts.flows == 1)
            opts.flows = inferred;
    }
    for (i = 0; i < opts.flows; i++) {
        if (!sizes[i])
            sizes[i] = opts.msg_size;
        if (sail_packets_for(sizes[i], opts.pmtu) > SAIL_MAX_PACKETS)
            DIE("flow %d too many packets", i);
    }

    sail_open_device(&ctx, opts.dev_name, opts.ib_port, opts.gid_index);
    flows = calloc((size_t)opts.flows, sizeof(*flows));
    local = calloc(1, sizeof(*local));
    remote = calloc(1, sizeof(*remote));
    if (!flows || !local || !remote)
        DIE("calloc receiver state");
    shared_ctrl_cq = ibv_create_cq(ctx.ctx,
                                   shared_cq_size(&ctx,
                                                  opts.flows * 64 + 8192),
                                   NULL, NULL, 0);
    if (!shared_ctrl_cq)
        DIE("ibv_create_cq receiver shared");

    local->magic = SAIL_MAGIC;
    local->version = SAIL_VERSION;
    local->flows = (uint32_t)opts.flows;
    local->gid_index = (uint32_t)opts.gid_index;
    local->udp_port = (uint32_t)parse_u64(opts.notify_port, "notify-port");
    for (i = 0; i < opts.flows; i++) {
        alloc_flow(&ctx, &flows[i], i, sizes[i], &opts, shared_ctrl_cq);
        sail_fill_local_wire(&ctx, &local->flow[i], flows[i].data_qp,
                             flows[i].ctrl_qp, flows[i].data_psn,
                             flows[i].ctrl_psn, flows[i].data_mr,
                             flows[i].data_buf, sizes[i]);
    }
    post_initial_recvs(flows, opts.flows);

    udp_fd = sail_udp_bind_addr(opts.notify_bind_ip, opts.notify_port);
    configure_udp_rcvbuf(udp_fd, opts.udp_rcvbuf);
    listen_fd = sail_tcp_listen(opts.port);
    sock = sail_tcp_accept(listen_fd);
    close(listen_fd);
    sail_exchange_info(sock, local, remote);
    sail_set_nonblock(sock);
    if (remote->flows != (uint32_t)opts.flows)
        DIE("peer flow count mismatch");

    for (i = 0; i < opts.flows; i++) {
        flows[i].start_psn = remote->flow[i].data_psn & 0xffffffu;
        sail_connect_qp(&ctx, flows[i].data_qp, IBV_QPT_RC, &remote->flow[i],
                        true, flows[i].data_psn);
        sail_connect_qp(&ctx, flows[i].ctrl_qp, IBV_QPT_UC, &remote->flow[i],
                        false, flows[i].ctrl_psn);
    }
    notify_map = build_notify_map(flows, opts.flows, &notify_map_mask);

    memset(&prof, 0, sizeof(prof));
    start_us = sail_now_us();
    getrusage(RUSAGE_SELF, &ru_start);
    cpu_start = clock();
    while (!tcp_done || outstanding_ctrl(flows, opts.flows) != 0) {
        poll_ctrl_cq(flows, opts.flows, &opts, &stats, shared_ctrl_cq);
        if (!tcp_done) {
            poll_udp_notifications(udp_fd, flows, notify_map, notify_map_mask,
                                   &opts, &stats);
            process_timeouts(flows, opts.flows, &opts, &stats);
            tcp_done = control_done(sock);
        }
    }
    cpu_end = clock();
    getrusage(RUSAGE_SELF, &ru_end);
    end_us = sail_now_us();
    print_stats(&opts, &stats, &prof, end_us - start_us, cpu_start, cpu_end,
                &ru_start, &ru_end);
    print_incomplete_flows(flows, opts.flows);
    close(sock);
    close(udp_fd);
    free(sizes);
    free(starts);
    return 0;
}
