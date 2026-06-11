#include "sail_common.h"

enum {
    SAIL_WR_DATA = 1,
    SAIL_WR_CTRL_SEND = 2,
    SAIL_WR_CTRL_RECV = 3,
    SAIL_WR_REPAIR = 4,
};

struct sender_opts {
    const char *server;
    const char *dev_name;
    const char *port;
    const char *notify_port;
    const char *traffic_file;
    const char *fct_file;
    int ib_port;
    int gid_index;
    int flows;
    int messages;
    size_t msg_size;
    size_t pmtu;
    double drop_rate;
    uint64_t drop_every;
    uint64_t window_start_us;
    uint64_t window_end_us;
    uint64_t start_at_us;
    uint64_t max_runtime_us;
    bool simulate_notify;
};

struct sender_stats {
    uint64_t rc_writes;
    uint64_t loss_notifs;
    uint64_t complete_notifs;
    uint64_t nacks;
    uint64_t repair_writes;
    uint64_t done_acks;
    uint64_t done_ack_acks;
    uint64_t bytes_delivered;
};

struct sender_flow {
    int id;
    size_t msg_size;
    size_t packets;
    uint64_t start_delay_us;
    uint32_t data_psn;
    uint32_t ctrl_psn;
    uint32_t done_seq;
    bool started;
    bool data_done;
    bool done_ack;
    bool completed;
    uint64_t start_us;
    uint64_t end_us;
    uint64_t data_done_us;
    uint64_t done_ack_us;

    uint8_t *send_buf;
    struct ibv_mr *send_mr;
    struct sail_ctrl_msg *ctrl_recv_bufs;
    struct sail_ctrl_msg *ctrl_send_bufs;
    struct ibv_mr *ctrl_recv_mr;
    struct ibv_mr *ctrl_send_mr;
    struct ibv_cq *data_cq;
    struct ibv_cq *ctrl_cq;
    struct ibv_qp *data_qp;
    struct ibv_qp *ctrl_qp;
    int outstanding_data;
    int outstanding_ctrl;
    int outstanding_repair;
    uint32_t ctrl_send_head;
};

static uint64_t g_drop_attempts;
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

static double parse_double(const char *s, const char *name)
{
    char *end = NULL;
    double v;

    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0')
        DIE("invalid %s: %s", name, s);
    return v;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --server ADDR [options]\n"
            "  --port PORT                         TCP bootstrap port (default 20515)\n"
            "  --notify-port PORT                  UDP notification port (default 20516)\n"
            "  --dev NAME --ib-port N --gid-index N\n"
            "  --flows N                           flow count (default 1)\n"
            "  --messages N                        only 1 is supported (default 1)\n"
            "  --msg-size BYTES | --traffic-file PATH\n"
            "  --pmtu BYTES                        simulated packet size (default 1024)\n"
            "  --fct-file PATH\n"
            "  --goodput-window-start-us US --goodput-window-end-us US\n"
            "  --start-at-us UNIX_EPOCH_US\n"
            "  --max-runtime-us US                 stop after this runtime; 0 disables\n"
            "  --simulate-notify                   generate UDP loss/complete notifications from sender\n"
            "  --drop-rate P | --drop-every N\n",
            prog);
}

static void parse_opts(int argc, char **argv, struct sender_opts *o)
{
    static const struct option long_opts[] = {
        {"server", required_argument, NULL, 's'},
        {"port", required_argument, NULL, 'p'},
        {"notify-port", required_argument, NULL, 'n'},
        {"dev", required_argument, NULL, 'd'},
        {"ib-port", required_argument, NULL, 'i'},
        {"gid-index", required_argument, NULL, 'g'},
        {"flows", required_argument, NULL, 'f'},
        {"messages", required_argument, NULL, 'm'},
        {"msg-size", required_argument, NULL, 'z'},
        {"traffic-file", required_argument, NULL, 'x'},
        {"pmtu", required_argument, NULL, 'u'},
        {"fct-file", required_argument, NULL, 'y'},
        {"goodput-window-start-us", required_argument, NULL, 'w'},
        {"goodput-window-end-us", required_argument, NULL, 'W'},
        {"start-at-us", required_argument, NULL, 'a'},
        {"max-runtime-us", required_argument, NULL, 'T'},
        {"simulate-notify", no_argument, NULL, 'N'},
        {"drop-rate", required_argument, NULL, 'r'},
        {"drop-every", required_argument, NULL, 'e'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int ch;

    *o = (struct sender_opts){
        .server = NULL,
        .dev_name = NULL,
        .port = "20515",
        .notify_port = "20516",
        .traffic_file = NULL,
        .fct_file = NULL,
        .ib_port = 1,
        .gid_index = -1,
        .flows = 1,
        .messages = 1,
        .msg_size = 1024 * 1024,
        .pmtu = SAIL_DEFAULT_PMTU,
        .drop_rate = 0.0,
        .drop_every = 0,
        .simulate_notify = false,
    };
    while ((ch = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (ch) {
        case 's': o->server = optarg; break;
        case 'p': o->port = optarg; break;
        case 'n': o->notify_port = optarg; break;
        case 'd': o->dev_name = optarg; break;
        case 'i': o->ib_port = (int)parse_u64(optarg, "ib-port"); break;
        case 'g': o->gid_index = (int)parse_u64(optarg, "gid-index"); break;
        case 'f': o->flows = (int)parse_u64(optarg, "flows"); break;
        case 'm': o->messages = (int)parse_u64(optarg, "messages"); break;
        case 'z': o->msg_size = (size_t)parse_u64(optarg, "msg-size"); break;
        case 'x': o->traffic_file = optarg; break;
        case 'u': o->pmtu = (size_t)parse_u64(optarg, "pmtu"); break;
        case 'y': o->fct_file = optarg; break;
        case 'w': o->window_start_us = parse_u64(optarg, "goodput-window-start-us"); break;
        case 'W': o->window_end_us = parse_u64(optarg, "goodput-window-end-us"); break;
        case 'a': o->start_at_us = parse_u64(optarg, "start-at-us"); break;
        case 'T': o->max_runtime_us = parse_u64(optarg, "max-runtime-us"); break;
        case 'N': o->simulate_notify = true; break;
        case 'r': o->drop_rate = parse_double(optarg, "drop-rate"); break;
        case 'e': o->drop_every = parse_u64(optarg, "drop-every"); break;
        case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
        default: usage(argv[0]); exit(EXIT_FAILURE);
        }
    }
    if (!o->server)
        DIE("--server is required");
    if (o->flows <= 0 || o->flows > SAIL_MAX_FLOWS)
        DIE("--flows must be in [1,%d]", SAIL_MAX_FLOWS);
    if (o->messages != 1)
        DIE("only --messages 1 is supported");
    if (o->pmtu == 0 || o->pmtu > SAIL_DEFAULT_PMTU)
        DIE("--pmtu must be in [1,%u]", SAIL_DEFAULT_PMTU);
    if (o->drop_rate < 0.0 || o->drop_rate > 1.0)
        DIE("--drop-rate must be in [0,1]");
    if (o->drop_rate > 0.0 && o->drop_every != 0)
        DIE("use only one of --drop-rate and --drop-every");
    if ((o->window_start_us || o->window_end_us) &&
        o->window_end_us <= o->window_start_us)
        DIE("invalid goodput window");
}

static bool should_mark_lost(const struct sender_opts *o)
{
    uint64_t attempt = ++g_drop_attempts;
    double sample;

    if (o->drop_every != 0 && attempt % o->drop_every == 0)
        return true;
    if (o->drop_rate <= 0.0)
        return false;
    sample = (double)rand() / (double)RAND_MAX;
    return sample < o->drop_rate;
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

static void post_ctrl_recv(struct sender_flow *f, uint16_t slot)
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
        DIE("ibv_post_recv sender ctrl flow %d", f->id);
    // if (g_prof)
    //     g_prof->active_ns += sail_now_ns() - t0;
    prof_post_recv();
}

static void post_ctrl_send(struct sender_flow *f, enum sail_ctrl_type type,
                           uint32_t msg_id, uint32_t psn, uint32_t done_seq,
                           struct sender_stats *st)
{
    uint16_t slot = (uint16_t)(f->ctrl_send_head++ % SAIL_CTRL_SEND_DEPTH);
    struct sail_ctrl_msg *msg = &f->ctrl_send_bufs[slot];
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad = NULL;
    // uint64_t t0;

    if (f->outstanding_ctrl >= SAIL_CTRL_SEND_DEPTH)
        DIE("sender ctrl SQ exhausted on flow %d", f->id);
    *msg = (struct sail_ctrl_msg){
        .magic = SAIL_MSG_MAGIC,
        .type = (uint16_t)type,
        .flow_id = (uint16_t)f->id,
        .msg_id = msg_id,
        .psn = psn,
        .done_seq = done_seq,
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
    // t0 = sail_now_ns();
    if (ibv_post_send(f->ctrl_qp, &wr, &bad))
        DIE("ibv_post_send sender ctrl flow %d", f->id);
    // if (g_prof)
    //     g_prof->active_ns += sail_now_ns() - t0;
    prof_post_send();
    f->outstanding_ctrl++;
    if (type == SAIL_CTRL_DONE_ACK_ACK)
        st->done_ack_acks++;
}

static void post_repair_write(struct sender_flow *f,
                              const struct sail_qp_wire *remote,
                              const struct sender_opts *o,
                              uint32_t psn, struct sender_stats *st)
{
    uint32_t packet_id = sail_psn_diff(psn, f->data_psn);
    size_t off;
    size_t len;
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad = NULL;
    // uint64_t t0;

    if (packet_id >= f->packets)
        DIE("NACK PSN outside message on flow %d", f->id);
    off = (size_t)packet_id * o->pmtu;
    len = sail_packet_len(f->msg_size, o->pmtu, packet_id);
    sge = (struct ibv_sge){
        .addr = (uintptr_t)(f->send_buf + off),
        .length = (uint32_t)len,
        .lkey = f->send_mr->lkey,
    };
    wr = (struct ibv_send_wr){
        .wr_id = wr_id_make(SAIL_WR_REPAIR, (uint16_t)f->id,
                            (uint16_t)(packet_id & 0xffffu)),
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
        .send_flags = IBV_SEND_SIGNALED,
        .imm_data = htonl(sail_imm((uint32_t)f->id, packet_id)),
    };
    wr.wr.rdma.remote_addr = remote->addr + off;
    wr.wr.rdma.rkey = remote->rkey;
    // t0 = sail_now_ns();
    if (ibv_post_send(f->ctrl_qp, &wr, &bad))
        DIE("ibv_post_send repair flow %d psn %u", f->id, psn);
    // if (g_prof)
    //     g_prof->active_ns += sail_now_ns() - t0;
    prof_post_send();
    f->outstanding_repair++;
    st->repair_writes++;
}

static void post_data_write(struct sender_flow *f, const struct sail_qp_wire *remote,
                            struct sender_stats *st)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)f->send_buf,
        .length = (uint32_t)f->msg_size,
        .lkey = f->send_mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = wr_id_make(SAIL_WR_DATA, (uint16_t)f->id, 0),
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad = NULL;
    uint64_t t0;

    wr.wr.rdma.remote_addr = remote->addr;
    wr.wr.rdma.rkey = remote->rkey;
    t0 = sail_now_ns();
    if (ibv_post_send(f->data_qp, &wr, &bad))
        DIE("ibv_post_send RC write flow %d", f->id);
    if (g_prof)
        g_prof->active_ns += sail_now_ns() - t0;
    prof_post_send();
    f->outstanding_data++;
    st->rc_writes++;
}

static void send_udp_notification(int udp_fd, const struct sail_notify_msg *msg)
{
    ssize_t n = send(udp_fd, msg, sizeof(*msg), 0);
    if (n != (ssize_t)sizeof(*msg))
        DIE("send UDP notification: %s", n < 0 ? strerror(errno) : "short send");
}

static void send_loss_and_complete(struct sender_flow *f, int udp_fd,
                                   const struct sail_qp_wire *remote,
                                   const struct sender_opts *o,
                                   struct sender_stats *st)
{
    uint32_t packet_id;
    uint32_t dqp = remote->data_qpn & 0xffffffu;

    for (packet_id = 0; packet_id < f->packets; packet_id++) {
        if (!should_mark_lost(o))
            continue;
        struct sail_notify_msg msg = {
            .magic = htonl(SAIL_NOTIFY_MAGIC),
            .type = SAIL_NOTIFY_LOSS,
            .dqp = htonl(dqp),
            .psn = htonl(sail_psn_add(f->data_psn, packet_id)),
        };
        send_udp_notification(udp_fd, &msg);
        st->loss_notifs++;
    }

    {
        struct sail_notify_msg msg = {
            .magic = htonl(SAIL_NOTIFY_MAGIC),
            .type = SAIL_NOTIFY_COMPLETE,
            .dqp = htonl(dqp),
            .psn = htonl(sail_psn_add(f->data_psn, (uint32_t)f->packets - 1)),
        };
        send_udp_notification(udp_fd, &msg);
        st->complete_notifs++;
    }
}

static void alloc_flow(struct sail_context *ctx, struct sender_flow *f,
                       int id, size_t msg_size, uint64_t start_delay_us,
                       const struct sender_opts *o,
                       struct ibv_cq *data_cq, struct ibv_cq *ctrl_cq)
{
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;

    memset(f, 0, sizeof(*f));
    f->id = id;
    f->msg_size = msg_size;
    f->packets = sail_packets_for(msg_size, o->pmtu);
    f->start_delay_us = start_delay_us;
    f->data_psn = sail_rand_psn();
    f->ctrl_psn = sail_rand_psn();
    if (f->packets == 0 || f->packets > SAIL_MAX_PACKETS)
        DIE("flow %d packet count out of range", id);
    if (msg_size > UINT32_MAX)
        DIE("flow %d message too large for one verbs SGE", id);

    if (posix_memalign((void **)&f->send_buf, 4096, msg_size))
        DIE("posix_memalign sender buffer");
    memset(f->send_buf, 0, msg_size);
    f->send_mr = ibv_reg_mr(ctx->pd, f->send_buf, msg_size, access);
    if (!f->send_mr)
        DIE("ibv_reg_mr sender data");

    f->ctrl_recv_bufs = calloc(SAIL_CTRL_RECV_DEPTH, sizeof(*f->ctrl_recv_bufs));
    f->ctrl_send_bufs = calloc(SAIL_CTRL_SEND_DEPTH, sizeof(*f->ctrl_send_bufs));
    if (!f->ctrl_recv_bufs || !f->ctrl_send_bufs)
        DIE("calloc sender ctrl buffers");
    f->ctrl_recv_mr = ibv_reg_mr(ctx->pd, f->ctrl_recv_bufs,
                                 SAIL_CTRL_RECV_DEPTH * sizeof(*f->ctrl_recv_bufs),
                                 access);
    f->ctrl_send_mr = ibv_reg_mr(ctx->pd, f->ctrl_send_bufs,
                                 SAIL_CTRL_SEND_DEPTH * sizeof(*f->ctrl_send_bufs),
                                 access);
    if (!f->ctrl_recv_mr || !f->ctrl_send_mr)
        DIE("ibv_reg_mr sender ctrl");

    f->data_cq = data_cq;
    f->ctrl_cq = ctrl_cq;
    f->data_qp = sail_create_qp(ctx, IBV_QPT_RC, f->data_cq, f->data_cq,
                                SAIL_DATA_SEND_DEPTH, 1);
    f->ctrl_qp = sail_create_qp(ctx, IBV_QPT_UC, f->ctrl_cq, f->ctrl_cq,
                                SAIL_CTRL_SEND_DEPTH + 64,
                                SAIL_CTRL_RECV_DEPTH);
}

static void post_initial_recvs(struct sender_flow *flows, int nflows)
{
    int i;
    uint16_t slot;

    for (i = 0; i < nflows; i++)
        for (slot = 0; slot < SAIL_CTRL_RECV_DEPTH; slot++)
            post_ctrl_recv(&flows[i], slot);
}

static void poll_data_cq(struct sender_flow *flows, int nflows,
                         struct ibv_cq *data_cq)
{
    struct ibv_wc wc[SAIL_MAX_POLL];

    while (true) {
        uint64_t t0 = sail_now_ns();
        int ne = ibv_poll_cq(data_cq, SAIL_MAX_POLL, wc);
        int j;

        if (ne < 0)
            DIE("ibv_poll_cq sender data");
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

            if (fid >= (uint16_t)nflows)
                DIE("sender data CQE bad flow %u", fid);
            if (wc[j].status != IBV_WC_SUCCESS)
                DIE("sender data CQE flow %u status %s", fid,
                    ibv_wc_status_str(wc[j].status));
            if (wr_id_type(wc[j].wr_id) == SAIL_WR_DATA) {
                flows[fid].outstanding_data--;
                flows[fid].data_done = true;
                flows[fid].data_done_us = sail_now_us();
            }
        }
        if (g_prof)
            g_prof->active_ns += sail_now_ns() - t0;
    }
}

static void poll_ctrl_cq(struct sender_flow *flows, int nflows,
                         const struct sail_qp_wire *remote,
                         const struct sender_opts *o,
                         struct sender_stats *st, struct ibv_cq *ctrl_cq)
{
    struct ibv_wc wc[SAIL_MAX_POLL];

    while (true) {
        uint64_t t0 = sail_now_ns();
        int ne = ibv_poll_cq(ctrl_cq, SAIL_MAX_POLL, wc);
        int j;

        if (ne < 0)
            DIE("ibv_poll_cq sender ctrl");
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
            struct sender_flow *f;
            uint8_t type = wr_id_type(wc[j].wr_id);

            if (fid >= (uint16_t)nflows)
                DIE("sender ctrl CQE bad flow %u", fid);
            f = &flows[fid];
            if (wc[j].status != IBV_WC_SUCCESS)
                DIE("sender ctrl CQE flow %u status %s", fid,
                    ibv_wc_status_str(wc[j].status));
            if (type == SAIL_WR_CTRL_SEND) {
                f->outstanding_ctrl--;
            } else if (type == SAIL_WR_REPAIR) {
                f->outstanding_repair--;
            } else if (type == SAIL_WR_CTRL_RECV) {
                struct sail_ctrl_msg *msg = &f->ctrl_recv_bufs[slot];

                if (msg->magic != SAIL_MSG_MAGIC)
                    DIE("bad sender ctrl magic");
                if (msg->type == SAIL_CTRL_NACK) {
                    st->nacks++;
                    post_repair_write(f, &remote[fid], o, msg->psn, st);
                } else if (msg->type == SAIL_CTRL_DONE_ACK) {
                    st->done_acks++;
                    f->done_ack = true;
                    f->done_ack_us = sail_now_us();
                    f->done_seq = msg->done_seq;
                    post_ctrl_send(f, SAIL_CTRL_DONE_ACK_ACK, msg->msg_id,
                                   0, msg->done_seq, st);
                } else {
                    DIE("unexpected sender ctrl type %u", msg->type);
                }
                post_ctrl_recv(f, slot);
            } else {
                DIE("unexpected sender wr_id type");
            }
        }
        if (g_prof)
            g_prof->active_ns += sail_now_ns() - t0;
    }
}

static void write_fct_file(const char *path, const struct sender_flow *flows,
                           int nflows, uint64_t bench_start)
{
    FILE *fp;
    int i;

    if (!path)
        return;
    fp = fopen(path, "w");
    if (!fp)
        DIE("open fct file %s: %s", path, strerror(errno));
    fprintf(fp, "flow_id,msg_size_bytes,start_time_us,end_time_us,fct_us,"
            "start_delay_us,data_done_time_us,done_ack_time_us,"
            "data_done_gap_us,done_ack_gap_us\n");
    for (i = 0; i < nflows; i++) {
        if (!flows[i].completed)
            continue;
        uint64_t s = flows[i].start_us - bench_start;
        uint64_t e = flows[i].end_us - bench_start;
        uint64_t data_done = flows[i].data_done_us ?
                             flows[i].data_done_us - bench_start : 0;
        uint64_t done_ack = flows[i].done_ack_us ?
                            flows[i].done_ack_us - bench_start : 0;
        fprintf(fp, "%d,%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                flows[i].id, flows[i].msg_size, s, e, e - s,
                flows[i].start_delay_us, data_done, done_ack,
                data_done ? data_done - s : 0, done_ack ? done_ack - s : 0);
    }
    fclose(fp);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int count_completed(const struct sender_flow *flows, int nflows)
{
    int i;
    int completed = 0;

    for (i = 0; i < nflows; i++) {
        if (flows[i].completed)
            completed++;
    }
    return completed;
}

static void print_stats(const struct sender_opts *o, const struct sender_flow *flows,
                        int nflows, const struct sender_stats *st,
                        const struct sail_profile *prof,
                        uint64_t bench_start, uint64_t wall_us,
                        clock_t cpu_start, clock_t cpu_end,
                        const struct rusage *ru_start,
                        const struct rusage *ru_end,
                        bool timed_out)
{
    uint64_t *fcts = calloc((size_t)nflows, sizeof(*fcts));
    uint64_t total_bytes = 0;
    uint64_t fct_sum = 0;
    double wall_s = (double)wall_us / 1000000.0;
    double cpu_s = (double)(cpu_end - cpu_start) / (double)CLOCKS_PER_SEC;
    double active_cpu_s = (double)prof->active_ns / 1e9;
    double window_goodput = 0.0;
    double user_s = sail_timeval_sec(&ru_end->ru_utime) -
                    sail_timeval_sec(&ru_start->ru_utime);
    double sys_s = sail_timeval_sec(&ru_end->ru_stime) -
                   sail_timeval_sec(&ru_start->ru_stime);
    int i;
    int fct_count = 0;

    if (!fcts)
        DIE("calloc fcts");
    for (i = 0; i < nflows; i++) {
        if (!flows[i].completed)
            continue;
        uint64_t s = flows[i].start_us - bench_start;
        uint64_t e = flows[i].end_us - bench_start;
        uint64_t fct = e - s;
        fcts[fct_count++] = fct;
        fct_sum += fct;
        total_bytes += flows[i].msg_size;
        if (o->window_end_us > o->window_start_us) {
            uint64_t os = s > o->window_start_us ? s : o->window_start_us;
            uint64_t oe = e < o->window_end_us ? e : o->window_end_us;
            if (oe > os && e > s) {
                double frac = (double)(oe - os) / (double)(e - s);
                window_goodput += (double)flows[i].msg_size * frac;
            }
        }
    }
    qsort(fcts, (size_t)fct_count, sizeof(*fcts), cmp_u64);

    printf("flows,messages,pmtu,total_bytes,goodput_gbps\n");
    printf("%d,1,%zu,%" PRIu64 ",%.6f\n", nflows, o->pmtu, total_bytes,
           wall_s > 0.0 ? ((double)total_bytes * 8.0 / wall_s) / 1e9 : 0.0);
    printf("completed_flows,total_flows,timed_out,max_runtime_us\n");
    printf("%d,%d,%u,%" PRIu64 "\n", fct_count, nflows, timed_out ? 1u : 0u,
           o->max_runtime_us);
    printf("avg_fct_us,p50_fct_us,p95_fct_us,p99_fct_us\n");
    if (fct_count) {
        printf("%.2f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
               (double)fct_sum / (double)fct_count, fcts[fct_count / 2],
               fcts[(int)((double)(fct_count - 1) * 0.95)],
               fcts[(int)((double)(fct_count - 1) * 0.99)]);
    } else {
        printf("0.00,0,0,0\n");
    }
    printf("rc_writes,loss_notifs,complete_notifs,nacks,repair_writes,done_acks,done_ack_acks\n");
    printf("%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",%" PRIu64 "\n",
           st->rc_writes, st->loss_notifs, st->complete_notifs, st->nacks,
           st->repair_writes, st->done_acks, st->done_ack_acks);
    printf("cpu_user_s,cpu_sys_s,cpu_percent\n");
    printf("%.6f,%.6f,%.4f\n", user_s, sys_s,
           wall_s > 0.0 ? cpu_s / wall_s * 100.0 : 0.0);
    printf("effective_cpu_s,effective_cpu_percent,poll_hit,poll_empty,cqe_count,post_send,post_recv\n");
    printf("%.9f,%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 "\n",
           active_cpu_s, wall_s > 0.0 ? active_cpu_s / wall_s * 100.0 : 0.0,
           prof->poll_hit, prof->poll_empty, prof->cqe_count,
           prof->post_send, prof->post_recv);
    if (o->window_end_us > o->window_start_us) {
        double win_s = (double)(o->window_end_us - o->window_start_us) / 1000000.0;
        printf("window_start_us,window_end_us,window_goodput_gbps\n");
        printf("%" PRIu64 ",%" PRIu64 ",%.6f\n", o->window_start_us,
               o->window_end_us, (window_goodput * 8.0 / win_s) / 1e9);
    }
    free(fcts);
}

static void print_incomplete_flows(const struct sender_flow *flows, int nflows)
{
    int i;
    int printed = 0;

    fprintf(stderr,
            "sender_incomplete_flows: flow,msg_size,packets,started,"
            "data_done,done_ack,outstanding_data,outstanding_ctrl,"
            "outstanding_repair,start_delay_us\n");
    for (i = 0; i < nflows; i++) {
        const struct sender_flow *f = &flows[i];

        if (f->completed)
            continue;
        fprintf(stderr, "%d,%zu,%zu,%u,%u,%u,%d,%d,%d,%" PRIu64 "\n",
                f->id, f->msg_size, f->packets, f->started ? 1u : 0u,
                f->data_done ? 1u : 0u, f->done_ack ? 1u : 0u,
                f->outstanding_data, f->outstanding_ctrl,
                f->outstanding_repair, f->start_delay_us);
        printed++;
        if (printed >= 64)
            break;
    }
    if (printed == 0)
        fprintf(stderr, "sender_incomplete_flows: none\n");
}

static int outstanding(const struct sender_flow *flows, int nflows)
{
    int i;
    int out = 0;

    for (i = 0; i < nflows; i++)
        out += flows[i].outstanding_data + flows[i].outstanding_ctrl +
               flows[i].outstanding_repair;
    return out;
}

int main(int argc, char **argv)
{
    struct sender_opts opts;
    struct sail_context ctx;
    struct sender_flow *flows;
    struct sail_exchange *local;
    struct sail_exchange *remote;
    struct ibv_cq *shared_data_cq;
    struct ibv_cq *shared_ctrl_cq;
    struct sender_stats stats = { 0 };
    struct sail_profile prof = { 0 };
    size_t *sizes;
    uint64_t *starts;
    int sock;
    int udp_fd;
    int completed = 0;
    int i;
    uint64_t bench_start;
    uint64_t end_us;
    clock_t cpu_start;
    clock_t cpu_end;
    struct rusage ru_start;
    struct rusage ru_end;
    bool timed_out = false;

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
        DIE("calloc sender state");
    shared_data_cq = ibv_create_cq(ctx.ctx,
                                   shared_cq_size(&ctx, opts.flows * 2 + 1024),
                                   NULL, NULL, 0);
    shared_ctrl_cq = ibv_create_cq(ctx.ctx,
                                   shared_cq_size(&ctx,
                                                  opts.flows * 64 + 8192),
                                   NULL, NULL, 0);
    if (!shared_data_cq || !shared_ctrl_cq)
        DIE("ibv_create_cq sender shared");

    local->magic = SAIL_MAGIC;
    local->version = SAIL_VERSION;
    local->flows = (uint32_t)opts.flows;
    local->gid_index = (uint32_t)opts.gid_index;
    for (i = 0; i < opts.flows; i++) {
        alloc_flow(&ctx, &flows[i], i, sizes[i], starts[i], &opts,
                   shared_data_cq, shared_ctrl_cq);
        sail_fill_local_wire(&ctx, &local->flow[i], flows[i].data_qp,
                             flows[i].ctrl_qp, flows[i].data_psn,
                             flows[i].ctrl_psn, NULL, NULL, 0);
    }
    post_initial_recvs(flows, opts.flows);

    sock = sail_tcp_connect(opts.server, opts.port);
    sail_exchange_info(sock, local, remote);
    if (remote->flows != (uint32_t)opts.flows)
        DIE("peer flow count mismatch");
    udp_fd = opts.simulate_notify ? sail_udp_connect(opts.server,
                                                     opts.notify_port) : -1;

    for (i = 0; i < opts.flows; i++) {
        sail_connect_qp(&ctx, flows[i].data_qp, IBV_QPT_RC, &remote->flow[i],
                        true, flows[i].data_psn);
        sail_connect_qp(&ctx, flows[i].ctrl_qp, IBV_QPT_UC, &remote->flow[i],
                        false, flows[i].ctrl_psn);
    }

    if (opts.start_at_us)
        sail_wait_until_realtime_us(opts.start_at_us);
    memset(&prof, 0, sizeof(prof));
    bench_start = sail_now_us();
    getrusage(RUSAGE_SELF, &ru_start);
    cpu_start = clock();
    while (completed < opts.flows) {
        uint64_t now = sail_now_us();

        if (opts.max_runtime_us && now - bench_start >= opts.max_runtime_us) {
            timed_out = true;
            break;
        }
        poll_data_cq(flows, opts.flows, shared_data_cq);
        poll_ctrl_cq(flows, opts.flows, remote->flow, &opts, &stats,
                     shared_ctrl_cq);
        for (i = 0; i < opts.flows; i++) {
            struct sender_flow *f = &flows[i];

            if (!f->started && now - bench_start >= f->start_delay_us) {
                f->started = true;
                f->start_us = now;
                post_data_write(f, &remote->flow[i], &stats);
                if (opts.simulate_notify) {
                    send_loss_and_complete(f, udp_fd, &remote->flow[i], &opts,
                                           &stats);
                }
            }
            if (f->started && !f->completed && f->data_done && f->done_ack) {
                f->completed = true;
                f->end_us = sail_now_us();
                stats.bytes_delivered += f->msg_size;
            }
        }
        completed = count_completed(flows, opts.flows);
    }
    while (!timed_out && outstanding(flows, opts.flows) != 0) {
        poll_data_cq(flows, opts.flows, shared_data_cq);
        poll_ctrl_cq(flows, opts.flows, remote->flow, &opts, &stats,
                     shared_ctrl_cq);
    }
    sail_write_full(sock, SAIL_TCP_DONE, SAIL_TCP_DONE_LEN);
    cpu_end = clock();
    getrusage(RUSAGE_SELF, &ru_end);
    end_us = sail_now_us();

    write_fct_file(opts.fct_file, flows, opts.flows, bench_start);
    print_stats(&opts, flows, opts.flows, &stats, &prof, bench_start,
                end_us - bench_start, cpu_start, cpu_end, &ru_start, &ru_end,
                timed_out);
    print_incomplete_flows(flows, opts.flows);
    if (udp_fd >= 0)
        close(udp_fd);
    close(sock);
    free(sizes);
    free(starts);
    return 0;
}
