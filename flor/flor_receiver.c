#include "flor_common.h"

#include <sys/uio.h>

struct receiver_opts {
    const char *dev;
    const char *port;
    const char *traffic_file;
    int ib_port;
    int gid_index;
    int flows;
    bool flows_set;
    size_t msg_size;
    size_t chunk_size;
    uint32_t ack_batch;
    uint64_t ack_timeout_us;
};

struct receiver_flow {
    int id;
    struct ibv_cq *data_recv_cq;
    struct ibv_cq *ack_send_cq;
    struct ibv_cq *dummy_cq;
    struct ibv_qp *data_qp;
    struct ibv_qp *ack_qp;
    struct flor_profile *prof;
    uint32_t data_psn;
    uint32_t ack_psn;

    uint8_t *data_buf;
    struct ibv_mr *data_mr;
    uint8_t *dummy_recvs;
    struct ibv_mr *dummy_mr;
    struct flor_ack *ack_bufs;
    struct ibv_mr *ack_mr;

    uint8_t *received;
    uint8_t *dirty_windows;
    uint32_t *dirty_window_list;
    size_t msg_size;
    size_t chunks;
    size_t windows;
    uint32_t current_msg;
    uint32_t recv_count;
    uint32_t chunks_since_ack;
    uint32_t cum_ack;
    uint32_t highest_ack;
    uint64_t last_ack_flush_us;
    uint64_t completed_msgs;
    uint64_t sent_acks;
    bool completed_reported;
    int outstanding_acks;
    uint32_t ack_head;
    uint32_t dirty_window_count;
};

static struct receiver_flow *g_flows;
static int g_nflows;

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--dev <mlx>] [--gid-index <idx>] [--ib-port <p>] "
            "--port <tcp_port> [--flows <N>] "
            "[--msg-size <bytes>|--traffic-file <path>] "
            "--chunk-size <bytes> [--ack-batch <chunks>] "
            "[--ack-timeout-us <us>]\n",
            prog);
}

static uint64_t parse_u64(const char *s, const char *name)
{
    char *end = NULL;
    errno = 0;
    uint64_t v = strtoull(s, &end, 0);
    if (errno || !end || *end)
        DIE("invalid %s: %s", name, s);
    return v;
}

static void parse_opts(int argc, char **argv, struct receiver_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->port = "18515";
    o->ib_port = 1;
    o->gid_index = -1;
    o->flows = 1;
    o->msg_size = 1048576;
    o->chunk_size = 4096;
    o->ack_batch = 64;
    o->ack_timeout_us = 1000;

    static const struct option long_opts[] = {
        {"dev", required_argument, NULL, 'd'},
        {"gid-index", required_argument, NULL, 'g'},
        {"ib-port", required_argument, NULL, 'i'},
        {"port", required_argument, NULL, 'p'},
        {"flows", required_argument, NULL, 'f'},
        {"msg-size", required_argument, NULL, 'z'},
        {"traffic-file", required_argument, NULL, 'x'},
        {"chunk-size", required_argument, NULL, 'c'},
        {"ack-batch", required_argument, NULL, 'a'},
        {"ack-timeout-us", required_argument, NULL, 'u'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': o->dev = optarg; break;
        case 'g': o->gid_index = (int)parse_u64(optarg, "gid-index"); break;
        case 'i': o->ib_port = (int)parse_u64(optarg, "ib-port"); break;
        case 'p': o->port = optarg; break;
        case 'f':
            o->flows = (int)parse_u64(optarg, "flows");
            o->flows_set = true;
            break;
        case 'z': o->msg_size = (size_t)parse_u64(optarg, "msg-size"); break;
        case 'x': o->traffic_file = optarg; break;
        case 'c': o->chunk_size = (size_t)parse_u64(optarg, "chunk-size"); break;
        case 'a': o->ack_batch = (uint32_t)parse_u64(optarg, "ack-batch"); break;
        case 'u': o->ack_timeout_us = parse_u64(optarg, "ack-timeout-us"); break;
        default: usage(argv[0]); exit(EXIT_FAILURE);
        }
    }

    if (o->flows <= 0 || o->flows > FLOR_MAX_FLOWS ||
        (!o->traffic_file && o->msg_size == 0) || o->chunk_size == 0 ||
        o->ack_batch == 0)
        usage(argv[0]), exit(EXIT_FAILURE);
}

static void init_flow_sizes(struct receiver_opts *o, size_t *sizes)
{
    if (o->traffic_file) {
        int loaded = flor_load_traffic_file(o->traffic_file, FLOR_MAX_FLOWS,
                                            sizes, NULL);
        if (!o->flows_set) {
            o->flows = loaded;
        } else if (o->flows > loaded) {
            DIE("traffic file has %d flows, but --flows asks for %d", loaded,
                o->flows);
        }
    } else {
        for (int i = 0; i < o->flows; i++)
            sizes[i] = o->msg_size;
    }

    for (int i = 0; i < o->flows; i++) {
        if (sizes[i] == 0)
            DIE("flow %d has zero message size", i);
        if (flor_chunks_for(sizes[i], o->chunk_size) > FLOR_MAX_CHUNKS)
            DIE("flow %d has more than %d chunks; increase chunk size", i,
                FLOR_MAX_CHUNKS);
    }
}

static size_t checked_flow_count(int flows)
{
    if (flows <= 0 || flows > FLOR_MAX_FLOWS)
        DIE("invalid flow count %d", flows);
    return (size_t)(unsigned)flows;
}

static int shared_cq_size(struct flor_context *fc, int requested)
{
    // if (requested < FLOR_MAX_POLL)
    //     requested = FLOR_MAX_POLL;
    if (fc->device_attr.max_cqe > 0 && requested > fc->device_attr.max_cqe)
        requested = fc->device_attr.max_cqe;
    return requested;
}

static uint64_t wr_data_recv_id(uint32_t flow, uint32_t slot)
{
    return ((uint64_t)flow << 32) | slot;
}

static void parse_data_recv_id(uint64_t id, uint32_t *flow, uint32_t *slot)
{
    *flow = (uint32_t)(id >> 32);
    *slot = (uint32_t)id;
}

static void poll_ack_send_cq(struct receiver_flow *flows, int nflows,
                             struct ibv_cq *ack_send_cq)
{
    struct ibv_wc wc[FLOR_MAX_POLL];
    int n;
    struct flor_profile *prof = nflows > 0 ? flows[0].prof : NULL;
    uint64_t t0 = prof ? flor_now_ns() : 0;
    while ((n = ibv_poll_cq(ack_send_cq, FLOR_MAX_POLL, wc)) > 0) {
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS)
                DIE("ACK send completion error: %s",
                    ibv_wc_status_str(wc[i].status));
            uint32_t flow = (uint32_t)(wc[i].wr_id >> 32);
            if (flow >= (uint32_t)nflows)
                DIE("bad ACK send completion flow id %u", flow);
            if (flows[flow].outstanding_acks > 0)
                flows[flow].outstanding_acks--;
        }
        if (prof) {
            prof->active_ns += flor_now_ns() - t0;
            prof->poll_hit++;
            prof->cqe_count += (uint64_t)n;
            t0 = flor_now_ns();
        }
    }
    if (n < 0)
        DIE("ibv_poll_cq ACK send failed");
    if (prof)
        prof->poll_empty++;
}

static void post_data_recv(struct receiver_flow *f, uint32_t slot)
{
    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)(f->dummy_recvs + slot);
    sge.length = 1;
    sge.lkey = f->dummy_mr->lkey;

    struct ibv_recv_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_data_recv_id((uint32_t)f->id, slot);
    wr.sg_list = &sge;
    wr.num_sge = 1;

    struct ibv_recv_wr *bad = NULL;
    if (ibv_post_recv(f->data_qp, &wr, &bad))
        DIE("ibv_post_recv data failed");
    if (f->prof)
        f->prof->post_recv++;
}

static uint64_t make_ack_bitmap(const struct receiver_flow *f, uint32_t base)
{
    uint64_t bits = 0;
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t c = base + i;
        if (c >= f->chunks)
            break;
        if (f->received[c])
            bits |= 1ull << i;
    }
    return bits;
}

static void send_ack_with_bitmap(struct receiver_flow *f, uint32_t msg_id,
                                 uint32_t chunk, bool done, uint64_t bitmap)
{
    while (f->outstanding_acks >= FLOR_ACK_SEND_DEPTH)
        poll_ack_send_cq(g_flows, g_nflows, f->ack_send_cq);

    uint32_t slot = f->ack_head++ % FLOR_ACK_SEND_DEPTH;
    uint32_t base = chunk & ~63u;
    struct flor_ack *ack = &f->ack_bufs[slot];
    memset(ack, 0, sizeof(*ack));
    ack->magic = FLOR_ACK_MAGIC;
    ack->flow_id = (uint16_t)f->id;
    ack->flags = done ? FLOR_ACK_FLAG_DONE : 0;
    ack->msg_id = msg_id;
    ack->base_chunk = base;
    ack->cum_ack = f->cum_ack;
    ack->highest_ack = f->highest_ack;
    ack->bitmap = bitmap;

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)ack;
    sge.length = sizeof(*ack);
    sge.lkey = f->ack_mr->lkey;

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = ((uint64_t)f->id << 32) | slot;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    struct ibv_send_wr *bad = NULL;
    if (ibv_post_send(f->ack_qp, &wr, &bad))
        DIE("ibv_post_send ACK failed");
    f->outstanding_acks++;
    f->sent_acks++;
    if (f->prof)
        f->prof->post_send++;
}

static void mark_dirty_window(struct receiver_flow *f, uint32_t chunk)
{
    uint32_t win = chunk / 64;
    if (win < f->windows && !f->dirty_windows[win]) {
        f->dirty_windows[win] = 1;
        f->dirty_window_list[f->dirty_window_count++] = win;
    }
}

static void flush_dirty_acks(struct receiver_flow *f, uint32_t msg_id,
                             bool done)
{
    for (uint32_t i = 0; i < f->dirty_window_count; i++) {
        uint32_t win = f->dirty_window_list[i];
        uint32_t base = win * 64;

        send_ack_with_bitmap(f, msg_id, base, done,
                             make_ack_bitmap(f, base));
        f->dirty_windows[win] = 0;
    }
    f->dirty_window_count = 0;
    f->chunks_since_ack = 0;
    f->last_ack_flush_us = flor_now_us();
}

// static void reset_message(struct receiver_flow *f, uint32_t next_msg)
// {
//     memset(f->received, 0, f->chunks);
//     memset(f->dirty_windows, 0, f->windows);
//     f->current_msg = next_msg;
//     f->recv_count = 0;
//     f->chunks_since_ack = 0;
//     f->cum_ack = 0;
//     f->highest_ack = 0;
//     f->last_ack_flush_us = flor_now_us();
// }

static void handle_chunk(struct receiver_flow *f, const struct receiver_opts *o,
                         uint32_t chunk)
{
    if (chunk >= f->chunks)
        return;

    if (!f->received[chunk]) {
        f->received[chunk] = 1;
        f->recv_count++;
        f->chunks_since_ack++;
        mark_dirty_window(f, chunk);
        if (chunk + 1 > f->highest_ack)
            f->highest_ack = chunk + 1;
        while (f->cum_ack < f->chunks && f->received[f->cum_ack])
            f->cum_ack++;
    } else {
        mark_dirty_window(f, chunk);
        f->chunks_since_ack++;
    }

    bool done = f->recv_count == f->chunks;
    if (done && !f->completed_reported) {
        f->completed_msgs++;
        f->completed_reported = true;
    }

    bool window_tail = (chunk & 63u) == 63u;
    bool message_tail = chunk + 1 == f->chunks;
    if (done || window_tail || message_tail ||
        f->chunks_since_ack >= o->ack_batch)
        flush_dirty_acks(f, f->current_msg, done);
    // if (done)
    //     reset_message(f, f->current_msg + 1);
}

static int check_done_control(int sock)
{
    char buf[FLOR_CTRL_DONE_LEN];
    ssize_t n = recv(sock, buf, sizeof(buf), MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        DIE("recv control: %s", strerror(errno));
    }
    if (n == 0)
        return 1;
    if (n == FLOR_CTRL_DONE_LEN && memcmp(buf, FLOR_CTRL_DONE,
                                          FLOR_CTRL_DONE_LEN) == 0)
        return 1;
    return 0;
}

int main(int argc, char **argv)
{
    struct receiver_opts o;
    parse_opts(argc, argv, &o);
    srand48((long)time(NULL));

    size_t flow_sizes[FLOR_MAX_FLOWS];
    memset(flow_sizes, 0, sizeof(flow_sizes));
    init_flow_sizes(&o, flow_sizes);
    size_t nflows = checked_flow_count(o.flows);

    struct flor_context fc;
    flor_open_device(&fc, o.dev, o.ib_port, o.gid_index);

    struct receiver_flow *flows = calloc(nflows, sizeof(*flows));
    if (!flows)
        DIE("calloc flows failed");
    g_flows = flows;
    g_nflows = o.flows;
    struct ibv_cq *shared_data_cq =
        ibv_create_cq(fc.ctx, shared_cq_size(&fc, o.flows * 64 + 8192),
                      NULL, NULL, 0);
    struct ibv_cq *shared_ack_cq =
        ibv_create_cq(fc.ctx, shared_cq_size(&fc, o.flows * 64 + 8192),
                      NULL, NULL, 0);
    struct ibv_cq *shared_dummy_cq =
        ibv_create_cq(fc.ctx, shared_cq_size(&fc, o.flows + 1024),
                      NULL, NULL, 0);
    if (!shared_data_cq || !shared_ack_cq || !shared_dummy_cq)
        DIE("ibv_create_cq shared failed");

    for (int i = 0; i < o.flows; i++) {
        struct receiver_flow *f = &flows[i];
        size_t chunks = flor_chunks_for(flow_sizes[i], o.chunk_size);
        f->id = i;
        f->msg_size = flow_sizes[i];
        f->chunks = chunks;
        f->windows = (chunks + 63) / 64;
        f->current_msg = 0;
        f->last_ack_flush_us = flor_now_us();
        f->data_psn = flor_rand_psn();
        f->ack_psn = flor_rand_psn();
        f->data_recv_cq = shared_data_cq;
        f->ack_send_cq = shared_ack_cq;
        f->dummy_cq = shared_dummy_cq;
        f->data_qp = flor_create_uc_qp(&fc, f->dummy_cq, f->data_recv_cq,
                                       1, FLOR_DATA_RECV_DEPTH);
        f->ack_qp = flor_create_uc_qp(&fc, f->ack_send_cq, f->dummy_cq,
                                      FLOR_ACK_SEND_DEPTH, 1);

        if (posix_memalign((void **)&f->data_buf, 4096, f->msg_size))
            DIE("posix_memalign data_buf failed");
        f->data_mr = ibv_reg_mr(fc.pd, f->data_buf, f->msg_size,
                                IBV_ACCESS_LOCAL_WRITE |
                                    IBV_ACCESS_REMOTE_WRITE);
        if (!f->data_mr)
            DIE("ibv_reg_mr data_buf failed");
        f->dummy_recvs = calloc(FLOR_DATA_RECV_DEPTH, 1);
        if (!f->dummy_recvs)
            DIE("calloc dummy recvs failed");
        f->dummy_mr = ibv_reg_mr(fc.pd, f->dummy_recvs, FLOR_DATA_RECV_DEPTH,
                                 IBV_ACCESS_LOCAL_WRITE);
        if (!f->dummy_mr)
            DIE("ibv_reg_mr dummy recvs failed");
        f->ack_bufs = calloc(FLOR_ACK_SEND_DEPTH, sizeof(struct flor_ack));
        if (!f->ack_bufs)
            DIE("calloc ACK send buffers failed");
        f->ack_mr = ibv_reg_mr(fc.pd, f->ack_bufs,
                               FLOR_ACK_SEND_DEPTH * sizeof(struct flor_ack),
                               IBV_ACCESS_LOCAL_WRITE);
        if (!f->ack_mr)
            DIE("ibv_reg_mr ACK send buffers failed");
        f->received = calloc(chunks, 1);
        f->dirty_windows = calloc(f->windows, 1);
        f->dirty_window_list = calloc(f->windows, sizeof(*f->dirty_window_list));
        if (!f->received || !f->dirty_windows || !f->dirty_window_list)
            DIE("calloc bitmap failed");
        for (uint32_t s = 0; s < FLOR_DATA_RECV_DEPTH; s++)
            post_data_recv(f, s);
    }

    struct flor_exchange local;
    struct flor_exchange remote;
    memset(&local, 0, sizeof(local));
    local.magic = FLOR_MAGIC;
    local.version = FLOR_VERSION;
    local.flows = (uint32_t)o.flows;
    local.gid_index = (uint32_t)o.gid_index;
    for (int i = 0; i < o.flows; i++) {
        flor_fill_local_wire(&fc, &local.flow[i], flows[i].data_qp,
                             flows[i].ack_qp, flows[i].data_psn,
                             flows[i].ack_psn, flows[i].data_mr,
                             flows[i].data_buf, flows[i].msg_size);
    }

    int listen_fd = flor_tcp_listen(o.port);
    int sock = flor_tcp_accept(listen_fd);
    close(listen_fd);
    flor_exchange_info(sock, &local, &remote);
    if ((int)remote.flows != o.flows)
        DIE("peer flow count mismatch");

    for (int i = 0; i < o.flows; i++) {
        flor_connect_uc_qp(&fc, flows[i].data_qp, &remote.flow[i], true,
                           flows[i].data_psn);
        flor_connect_uc_qp(&fc, flows[i].ack_qp, &remote.flow[i], false,
                           flows[i].ack_psn);
    }

    uint64_t recv_chunks = 0;
    struct flor_profile prof;
    memset(&prof, 0, sizeof(prof));
    for (int i = 0; i < o.flows; i++)
        flows[i].prof = &prof;
    struct rusage ru_start, ru_end;
    getrusage(RUSAGE_SELF, &ru_start);
    uint64_t start_us = flor_now_us();
    while (!check_done_control(sock)) {
        struct ibv_wc wc[FLOR_MAX_POLL];
        while (1) {
            uint64_t t0 = flor_now_ns();
            int n = ibv_poll_cq(shared_data_cq, FLOR_MAX_POLL, wc);
            if (n <= 0) {
                if (n < 0)
                    DIE("ibv_poll_cq data recv failed");
                prof.poll_empty++;
                break;
            }
            for (int j = 0; j < n; j++) {
                if (wc[j].status != IBV_WC_SUCCESS)
                    DIE("data recv completion error: %s",
                        ibv_wc_status_str(wc[j].status));
                uint32_t flow, slot;
                parse_data_recv_id(wc[j].wr_id, &flow, &slot);
                if (flow >= (uint32_t)o.flows || slot >= FLOR_DATA_RECV_DEPTH)
                    DIE("bad data recv completion id");
                struct receiver_flow *f = &flows[flow];
                if (wc[j].wc_flags & IBV_WC_WITH_IMM) {
                    uint32_t imm_flow, chunk;
                    flor_parse_imm(ntohl(wc[j].imm_data), &imm_flow,
                                   &chunk);
                    if (imm_flow != flow)
                        DIE("immediate flow id %u does not match flow %u",
                            imm_flow, flow);
                    handle_chunk(f, &o, chunk);
                    recv_chunks++;
                }
                post_data_recv(f, slot);
            }
            prof.active_ns += flor_now_ns() - t0;
            prof.poll_hit++;
            prof.cqe_count += (uint64_t)n;
            if(n<2){
                break;
            }
        }
        poll_ack_send_cq(flows, o.flows, shared_ack_cq);
        for (int i = 0; i < o.flows; i++) {
            struct receiver_flow *f = &flows[i];
            if (f->chunks_since_ack &&
                flor_now_us() - f->last_ack_flush_us >= o.ack_timeout_us) {
                uint64_t t0 = flor_now_ns();
                flush_dirty_acks(f, f->current_msg, false);
                prof.active_ns += flor_now_ns() - t0;
            }
        }
    }

    while (1) {
        int outstanding = 0;
        for (int i = 0; i < o.flows; i++)
            outstanding += flows[i].outstanding_acks;
        if (!outstanding)
            break;
        poll_ack_send_cq(flows, o.flows, shared_ack_cq);
    }

    uint64_t end_us = flor_now_us();
    getrusage(RUSAGE_SELF, &ru_end);
    uint64_t completed = 0;
    uint64_t ack_pkts = 0;
    for (int i = 0; i < o.flows; i++) {
        completed += flows[i].completed_msgs;
        ack_pkts += flows[i].sent_acks;
    }
    double wall_s = (double)(end_us - start_us) / 1000000.0;
    double cpu_s = (flor_timeval_sec(&ru_end.ru_utime) -
                    flor_timeval_sec(&ru_start.ru_utime)) +
                   (flor_timeval_sec(&ru_end.ru_stime) -
                    flor_timeval_sec(&ru_start.ru_stime));
    double user_s = flor_timeval_sec(&ru_end.ru_utime) -
                    flor_timeval_sec(&ru_start.ru_utime);
    double sys_s = flor_timeval_sec(&ru_end.ru_stime) -
                   flor_timeval_sec(&ru_start.ru_stime);
    double cpu_percent = wall_s > 0.0 ? cpu_s / wall_s * 100.0 : 0.0;
    double active_cpu_s = (double)prof.active_ns / 1000000000.0;
    double effective_cpu_percent = wall_s > 0.0 ?
        active_cpu_s / wall_s * 100.0 : 0.0;
    printf("receiver_flows,msg_size,chunk_size,completed_msgs,recv_chunks,ack_pkts,wall_s\n");
    printf("%d,%zu,%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f\n",
           o.flows, o.traffic_file ? (size_t)0 : o.msg_size, o.chunk_size,
           completed, recv_chunks, ack_pkts, wall_s);
    printf("cpu_user_s,cpu_sys_s,cpu_percent\n");
    printf("%.6f,%.6f,%.2f\n", user_s, sys_s, cpu_percent);
    printf("effective_cpu_s,effective_cpu_percent,poll_hit,poll_empty,cqe_count,post_send,post_recv\n");
    printf("%.9f,%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
           active_cpu_s, effective_cpu_percent, prof.poll_hit,
           prof.poll_empty, prof.cqe_count, prof.post_send, prof.post_recv);

    close(sock);
    return EXIT_SUCCESS;
}
