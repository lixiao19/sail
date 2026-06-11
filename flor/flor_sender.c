#include "flor_common.h"

struct sender_opts {
    const char *server;
    const char *dev;
    const char *port;
    const char *traffic_file;
    const char *fct_file;
    int ib_port;
    int gid_index;
    int flows;
    bool flows_set;
    int messages;
    size_t msg_size;
    size_t chunk_size;
    int sq_depth;
    uint64_t timeout_us;
    uint64_t window_start_us;
    uint64_t window_end_us;
    uint64_t start_at_us;
    uint64_t max_runtime_us;
    bool window_set;
    double drop_rate;
    uint64_t drop_every;
};

struct sender_stats {
    uint64_t tx_chunks;
    uint64_t retrans_chunks;
    uint64_t dropped_first_chunks;
    uint64_t ack_pkts;
    uint64_t fast_retrans_events;
    uint64_t timeout_events;
};

struct sender_flow {
    int id;
    struct ibv_cq *data_send_cq;
    struct ibv_cq *ack_recv_cq;
    struct ibv_cq *dummy_cq;
    struct ibv_qp *data_qp;
    struct ibv_qp *ack_qp;
    struct flor_profile *prof;
    uint32_t data_psn;
    uint32_t ack_psn;

    uint8_t *send_buf;
    struct ibv_mr *send_mr;
    struct flor_ack *ack_bufs;
    struct ibv_mr *ack_mr;

    uint8_t *acked;
    uint8_t *in_flight;
    uint8_t *ever_sent;
    uint8_t *fast_retrans;
    uint8_t *fast_retransed;
    uint64_t *sent_us;
    uint32_t *pending_chunks;
    uint32_t *pending_pos;
    size_t msg_size;
    size_t chunks;
    int data_sq_depth;
    uint32_t next_unsent_chunk;
    uint32_t pending_count;
    uint64_t start_delay_us;
    uint64_t release_us;
    uint64_t completion_us;
    uint64_t fct_us;
    uint32_t active_msg;
    uint64_t msg_start_us;
    uint32_t acked_count;
    uint32_t cumulative_acked;
    uint32_t highest_ack_seen;
    uint32_t fast_scan_upto;
    bool active;
    bool done_seen;
    int completed_msgs;
    int outstanding_writes;
};

static uint64_t g_first_attempts;

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --server <ip> [--dev <mlx>] [--gid-index <idx>] "
            "[--ib-port <p>] --port <tcp_port> [--flows <N>] "
            "[--messages <N>] [--msg-size <bytes>|--traffic-file <path>] "
            "--chunk-size <bytes> --timeout-us <us> "
            "[--sq-depth <wr>] [--fct-file <path>] "
            "[--goodput-window-start-us <us> --goodput-window-end-us <us>] "
            "[--start-at-us <unix_epoch_us>] "
            "[--max-runtime-us <us>] "
            "[--drop-rate <p>|--drop-every <N>]\n",
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

static void parse_opts(int argc, char **argv, struct sender_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->port = "18515";
    o->ib_port = 1;
    o->gid_index = -1;
    o->flows = 1;
    o->messages = 1;
    o->msg_size = 1048576;
    o->chunk_size = 4096;
    o->sq_depth = 2048;
    o->timeout_us = 100000;

    static const struct option long_opts[] = {
        {"server", required_argument, NULL, 's'},
        {"dev", required_argument, NULL, 'd'},
        {"gid-index", required_argument, NULL, 'g'},
        {"ib-port", required_argument, NULL, 'i'},
        {"port", required_argument, NULL, 'p'},
        {"flows", required_argument, NULL, 'f'},
        {"messages", required_argument, NULL, 'm'},
        {"msg-size", required_argument, NULL, 'z'},
        {"traffic-file", required_argument, NULL, 'x'},
        {"fct-file", required_argument, NULL, 'y'},
        {"chunk-size", required_argument, NULL, 'c'},
        {"sq-depth", required_argument, NULL, 'q'},
        {"timeout-us", required_argument, NULL, 't'},
        {"goodput-window-start-us", required_argument, NULL, 'w'},
        {"goodput-window-end-us", required_argument, NULL, 'W'},
        {"start-at-us", required_argument, NULL, 'a'},
        {"max-runtime-us", required_argument, NULL, 'T'},
        {"drop-rate", required_argument, NULL, 'r'},
        {"drop-every", required_argument, NULL, 'e'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': o->server = optarg; break;
        case 'd': o->dev = optarg; break;
        case 'g': o->gid_index = (int)parse_u64(optarg, "gid-index"); break;
        case 'i': o->ib_port = (int)parse_u64(optarg, "ib-port"); break;
        case 'p': o->port = optarg; break;
        case 'f':
            o->flows = (int)parse_u64(optarg, "flows");
            o->flows_set = true;
            break;
        case 'm': o->messages = (int)parse_u64(optarg, "messages"); break;
        case 'z': o->msg_size = (size_t)parse_u64(optarg, "msg-size"); break;
        case 'x': o->traffic_file = optarg; break;
        case 'y': o->fct_file = optarg; break;
        case 'c': o->chunk_size = (size_t)parse_u64(optarg, "chunk-size"); break;
        case 'q': o->sq_depth = (int)parse_u64(optarg, "sq-depth"); break;
        case 't': o->timeout_us = parse_u64(optarg, "timeout-us"); break;
        case 'w':
            o->window_start_us = parse_u64(optarg, "goodput-window-start-us");
            o->window_set = true;
            break;
        case 'W':
            o->window_end_us = parse_u64(optarg, "goodput-window-end-us");
            o->window_set = true;
            break;
        case 'a': o->start_at_us = parse_u64(optarg, "start-at-us"); break;
        case 'T': o->max_runtime_us = parse_u64(optarg, "max-runtime-us"); break;
        case 'r': o->drop_rate = strtod(optarg, NULL); break;
        case 'e': o->drop_every = parse_u64(optarg, "drop-every"); break;
        default: usage(argv[0]); exit(EXIT_FAILURE);
        }
    }

    if (!o->server || o->flows <= 0 || o->flows > FLOR_MAX_FLOWS ||
        o->messages <= 0 || (!o->traffic_file && o->msg_size == 0) ||
        o->chunk_size == 0 ||
        o->sq_depth <= 0)
        usage(argv[0]), exit(EXIT_FAILURE);
    if (o->messages != 1)
        DIE("this benchmark now supports exactly one message per flow");
    if (o->drop_rate < 0.0 || o->drop_rate > 1.0)
        DIE("drop-rate must be in [0,1]");
    if (o->drop_rate > 0.0 && o->drop_every)
        DIE("use only one of --drop-rate and --drop-every");
    if (o->window_set && o->window_end_us <= o->window_start_us)
        DIE("goodput window end must be larger than start");
}

static void init_flow_sizes(struct sender_opts *o, size_t *sizes,
                            uint64_t *start_delay_us)
{
    if (o->traffic_file) {
        int loaded = flor_load_traffic_file(o->traffic_file, FLOR_MAX_FLOWS,
                                            sizes, start_delay_us);
        if (!o->flows_set) {
            o->flows = loaded;
        } else if (o->flows > loaded) {
            DIE("traffic file has %d flows, but --flows asks for %d", loaded,
                o->flows);
        }
    } else {
        for (int i = 0; i < o->flows; i++) {
            sizes[i] = o->msg_size;
            start_delay_us[i] = 0;
        }
    }

    for (int i = 0; i < o->flows; i++) {
        if (sizes[i] == 0)
            DIE("flow %d has zero message size", i);
        if (flor_chunks_for(sizes[i], o->chunk_size) > FLOR_MAX_CHUNKS)
            DIE("flow %d has more than %d chunks; increase chunk size", i,
                FLOR_MAX_CHUNKS);
    }
}

static int shared_cq_size(struct flor_context *fc, int requested)
{
    if (requested < FLOR_MAX_POLL)
        requested = FLOR_MAX_POLL;
    if (fc->device_attr.max_cqe > 0 && requested > fc->device_attr.max_cqe)
        requested = fc->device_attr.max_cqe;
    return requested;
}

static size_t checked_flow_count(int flows)
{
    if (flows <= 0 || flows > FLOR_MAX_FLOWS)
        DIE("invalid flow count %d", flows);
    return (size_t)(unsigned)flows;
}

static uint64_t wr_data_id(uint32_t flow, uint32_t msg, uint32_t chunk)
{
    (void)msg;
    return (1ull << 63) | ((uint64_t)(flow & 0xfffu) << 20) |
           (uint64_t)(chunk & 0xfffffu);
}

static void parse_data_id(uint64_t id, uint32_t *flow, uint32_t *msg,
                          uint32_t *chunk)
{
    *flow = (uint32_t)((id >> 20) & 0xfffu);
    *msg = 0;
    *chunk = (uint32_t)(id & 0xfffffu);
}

static uint64_t wr_ack_id(uint32_t flow, uint32_t slot)
{
    return ((uint64_t)flow << 32) | slot;
}

static void parse_ack_id(uint64_t id, uint32_t *flow, uint32_t *slot)
{
    *flow = (uint32_t)(id >> 32);
    *slot = (uint32_t)id;
}

static void post_ack_recv(struct sender_flow *f, uint32_t slot)
{
    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)&f->ack_bufs[slot];
    sge.length = sizeof(struct flor_ack);
    sge.lkey = f->ack_mr->lkey;

    struct ibv_recv_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_ack_id((uint32_t)f->id, slot);
    wr.sg_list = &sge;
    wr.num_sge = 1;

    struct ibv_recv_wr *bad = NULL;
    if (ibv_post_recv(f->ack_qp, &wr, &bad))
        DIE("ibv_post_recv ACK failed");
    if (f->prof)
        f->prof->post_recv++;
}

static bool should_drop_first(const struct sender_opts *o)
{
    g_first_attempts++;
    if (o->drop_every)
        return (g_first_attempts % o->drop_every) == 0;
    if (o->drop_rate > 0.0)
        return drand48() < o->drop_rate;
    return false;
}

static void reset_pending_positions(struct sender_flow *f)
{
    for (uint32_t i = 0; i < f->chunks; i++)
        f->pending_pos[i] = UINT32_MAX;
}

static void add_pending_chunk(struct sender_flow *f, uint32_t chunk)
{
    if (chunk >= f->chunks)
        DIE("bad pending chunk %u on flow %d", chunk, f->id);
    if (f->pending_pos[chunk] != UINT32_MAX)
        return;
    f->pending_chunks[f->pending_count] = chunk;
    f->pending_pos[chunk] = f->pending_count;
    f->pending_count++;
}

static void remove_pending_chunk(struct sender_flow *f, uint32_t chunk)
{
    uint32_t pos;
    uint32_t last_chunk;

    if (chunk >= f->chunks)
        DIE("bad pending removal chunk %u on flow %d", chunk, f->id);
    pos = f->pending_pos[chunk];
    if (pos == UINT32_MAX)
        return;
    if (pos >= f->pending_count)
        DIE("bad pending position on flow %d chunk %u", f->id, chunk);
    last_chunk = f->pending_chunks[f->pending_count - 1u];
    f->pending_chunks[pos] = last_chunk;
    f->pending_pos[last_chunk] = pos;
    f->pending_count--;
    f->pending_pos[chunk] = UINT32_MAX;
}

static int post_write_chunk(struct sender_flow *f, const struct sender_opts *o,
                            const struct flor_qp_wire *remote, uint32_t chunk,
                            bool retrans, struct sender_stats *st)
{
    bool first_send = !f->ever_sent[chunk];

    if (!retrans && !f->ever_sent[chunk] && should_drop_first(o)) {
        f->ever_sent[chunk] = 1;
        add_pending_chunk(f, chunk);
        f->sent_us[chunk] = flor_now_us();
        st->dropped_first_chunks++;
        return 0;
    }

    size_t len = flor_chunk_len(f->msg_size, o->chunk_size, chunk);
    size_t off = (size_t)chunk * o->chunk_size;

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)(f->send_buf + off);
    sge.length = (uint32_t)len;
    sge.lkey = f->send_mr->lkey;

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_data_id((uint32_t)f->id, f->active_msg, chunk);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data = htonl(flor_imm((uint32_t)f->id, chunk));
    wr.wr.rdma.remote_addr = remote->addr + off;
    wr.wr.rdma.rkey = remote->rkey;

    struct ibv_send_wr *bad = NULL;
    uint64_t t0 = f->prof ? flor_now_ns() : 0;
    if (ibv_post_send(f->data_qp, &wr, &bad)) {
        return -1;
    }
    if (f->prof) {
        f->prof->active_ns += flor_now_ns() - t0;
        f->prof->post_send++;
    }

    if (first_send)
        add_pending_chunk(f, chunk);
    f->ever_sent[chunk] = 1;
    f->in_flight[chunk] = 1;
    f->sent_us[chunk] = flor_now_us();
    f->outstanding_writes++;
    if (retrans)
        st->retrans_chunks++;
    else
        st->tx_chunks++;
    return 0;
}

static void start_message(struct sender_flow *f, uint32_t msg_id,
                          uint64_t start_us)
{
    memset(f->acked, 0, f->chunks);
    memset(f->in_flight, 0, f->chunks);
    memset(f->ever_sent, 0, f->chunks);
    memset(f->fast_retrans, 0, f->chunks);
    memset(f->fast_retransed, 0, f->chunks);
    memset(f->sent_us, 0, f->chunks * sizeof(uint64_t));
    reset_pending_positions(f);
    f->active_msg = msg_id;
    f->msg_start_us = start_us;
    f->acked_count = 0;
    f->cumulative_acked = 0;
    f->highest_ack_seen = 0;
    f->fast_scan_upto = 0;
    f->next_unsent_chunk = 0;
    f->pending_count = 0;
    f->active = true;
    f->done_seen = false;
}

static void apply_ack(struct sender_flow *f, const struct flor_ack *ack,
                      struct sender_stats *st);

static void poll_data_cq(struct sender_flow *flows, int nflows,
                         struct ibv_cq *data_cq)
{
    struct ibv_wc wc[FLOR_MAX_POLL];
    struct flor_profile *prof = nflows > 0 ? flows[0].prof : NULL;

    while (true) {
        uint64_t t0 = prof ? flor_now_ns() : 0;
        int n = ibv_poll_cq(data_cq, FLOR_MAX_POLL, wc);
        if (n < 0)
            DIE("ibv_poll_cq data send failed");
        if (prof) {
            if (n > 0) {
                prof->active_ns += flor_now_ns() - t0;
                prof->poll_hit++;
                prof->cqe_count += (uint64_t)n;
            } else {
                prof->poll_empty++;
            }
        }
        if (n == 0)
            return;
        for (int j = 0; j < n; j++) {
            if (wc[j].status != IBV_WC_SUCCESS)
                DIE("data send completion error: %s",
                    ibv_wc_status_str(wc[j].status));
            uint32_t flow, msg, chunk;
            parse_data_id(wc[j].wr_id, &flow, &msg, &chunk);
            if (flow >= (uint32_t)nflows)
                DIE("bad data completion flow id %u", flow);
            struct sender_flow *f = &flows[flow];
            if (f->outstanding_writes > 0)
                f->outstanding_writes--;
            if (f->active && msg == f->active_msg && chunk < f->chunks)
                f->in_flight[chunk] = 0;
        }
    }
}

static void poll_ack_cq(struct sender_flow *flows, int nflows,
                        struct sender_stats *st, struct ibv_cq *ack_cq)
{
    struct ibv_wc wc[FLOR_MAX_POLL];
    struct flor_profile *prof = nflows > 0 ? flows[0].prof : NULL;

    while (true) {
        uint64_t t0 = prof ? flor_now_ns() : 0;
        int n = ibv_poll_cq(ack_cq, FLOR_MAX_POLL, wc);
        if (n < 0)
            DIE("ibv_poll_cq ACK recv failed");
        if (prof) {
            if (n > 0) {
                prof->active_ns += flor_now_ns() - t0;
                prof->poll_hit++;
                prof->cqe_count += (uint64_t)n;
            } else {
                prof->poll_empty++;
            }
        }
        if (n == 0)
            return;
        for (int j = 0; j < n; j++) {
            if (wc[j].status != IBV_WC_SUCCESS)
                DIE("ACK recv completion error: %s",
                    ibv_wc_status_str(wc[j].status));
            uint32_t flow, slot;
            parse_ack_id(wc[j].wr_id, &flow, &slot);
            if (flow >= (uint32_t)nflows || slot >= FLOR_ACK_RECV_DEPTH)
                DIE("bad ACK completion id");
            struct sender_flow *f = &flows[flow];
            apply_ack(f, &f->ack_bufs[slot], st);
            st->ack_pkts++;
            post_ack_recv(f, slot);
        }
    }
}

static void apply_ack(struct sender_flow *f, const struct flor_ack *ack,
                      struct sender_stats *st)
{
    if (ack->magic != FLOR_ACK_MAGIC)
        return;
    if (!f->active || ack->msg_id != f->active_msg)
        return;
    uint32_t cum_ack = ack->cum_ack;
    uint32_t highest_ack = ack->highest_ack;
    if (cum_ack > f->chunks)
        cum_ack = (uint32_t)f->chunks;
    if (highest_ack > f->chunks)
        highest_ack = (uint32_t)f->chunks;
    if (cum_ack > f->cumulative_acked) {
        for (uint32_t chunk = f->cumulative_acked; chunk < cum_ack; chunk++) {
            if (!f->acked[chunk]) {
                f->acked[chunk] = 1;
                f->acked_count++;
                remove_pending_chunk(f, chunk);
            }
        }
        f->cumulative_acked = cum_ack;
    }
    uint32_t old_highest_ack_seen = f->highest_ack_seen;
    if (highest_ack > f->highest_ack_seen)
        f->highest_ack_seen = highest_ack;

    uint32_t base = ack->base_chunk;
    for (uint32_t b = 0; b < 64; b++) {
        uint32_t chunk = base + b;
        if (chunk >= f->chunks)
            break;
        if (ack->bitmap & (1ull << b)) {
            if (!f->acked[chunk]) {
                f->acked[chunk] = 1;
                f->acked_count++;
                remove_pending_chunk(f, chunk);
            }
        }
    }

    uint32_t scan_start = f->fast_scan_upto;
    if (scan_start < old_highest_ack_seen)
        scan_start = old_highest_ack_seen;
    if (scan_start < f->cumulative_acked)
        scan_start = f->cumulative_acked;
    for (uint32_t chunk = scan_start; chunk < f->highest_ack_seen; chunk++) {
        if (f->acked[chunk] || !f->ever_sent[chunk] ||
            f->fast_retransed[chunk])
            continue;
        if (!f->fast_retrans[chunk]) {
            f->fast_retrans[chunk] = 1;
            st->fast_retrans_events++;
        }
    }
    if (f->fast_scan_upto < f->highest_ack_seen)
        f->fast_scan_upto = f->highest_ack_seen;

    if (ack->flags & FLOR_ACK_FLAG_DONE)
        f->done_seen = true;
}

static void drive_sends(struct sender_flow *f, const struct sender_opts *o,
                        const struct flor_qp_wire *remote,
                        struct sender_stats *st)
{
    if (!f->active)
        return;

    uint64_t now = flor_now_us();
    if (f->completed_msgs == 0 && now < f->release_us)
        return;

    while (f->next_unsent_chunk < f->chunks &&
           f->outstanding_writes < f->data_sq_depth) {
        uint32_t c = f->next_unsent_chunk++;

        if (post_write_chunk(f, o, remote, c, false, st)) {
            f->next_unsent_chunk = c;
            break;
        }
    }

    for (uint32_t i = 0;
         i < f->pending_count && f->outstanding_writes < f->data_sq_depth;
         i++) {
        uint32_t c = f->pending_chunks[i];

        if (f->acked[c] || f->in_flight[c] || !f->ever_sent[c])
            continue;
        if (f->fast_retrans[c]) {
            if (post_write_chunk(f, o, remote, c, true, st))
                break;
            f->fast_retrans[c] = 0;
            f->fast_retransed[c] = 1;
            continue;
        }
        if (now - f->sent_us[c] >= o->timeout_us) {
            st->timeout_events++;
            if (post_write_chunk(f, o, remote, c, true, st))
                break;
            f->fast_retrans[c] = 0;
        }
    }
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t percentile(uint64_t *v, int n, double p)
{
    if (n <= 0)
        return 0;
    int idx = (int)((p / 100.0) * (double)(n - 1) + 0.5);
    if (idx < 0)
        idx = 0;
    if (idx >= n)
        idx = n - 1;
    return v[idx];
}

static void write_fct_file(const char *path, const struct sender_flow *flows,
                           int nflows, uint64_t bench_start)
{
    FILE *fp = fopen(path, "w");
    if (!fp)
        DIE("failed to open FCT file %s: %s", path, strerror(errno));

    fprintf(fp,
            "flow_id,msg_size_bytes,start_time_us,end_time_us,fct_us,start_delay_us\n");
    for (int i = 0; i < nflows; i++) {
        const struct sender_flow *f = &flows[i];
        if (f->completed_msgs <= 0)
            continue;
        fprintf(fp, "%d,%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                i + 1, f->msg_size, f->release_us - bench_start,
                f->completion_us - bench_start, f->fct_us,
                f->start_delay_us);
    }
    if (fclose(fp))
        DIE("failed to close FCT file %s: %s", path, strerror(errno));
}

static uint64_t window_effective_bytes(const struct sender_flow *flows,
                                       int nflows, uint64_t bench_start,
                                       uint64_t win_start_us,
                                       uint64_t win_end_us)
{
    double bytes = 0.0;
    uint64_t abs_start = bench_start + win_start_us;
    uint64_t abs_end = bench_start + win_end_us;

    for (int i = 0; i < nflows; i++) {
        const struct sender_flow *f = &flows[i];
        if (f->completion_us <= f->msg_start_us)
            continue;

        uint64_t s = f->msg_start_us > abs_start ? f->msg_start_us : abs_start;
        uint64_t e = f->completion_us < abs_end ? f->completion_us : abs_end;
        if (e <= s)
            continue;

        double frac = (double)(e - s) / (double)(f->completion_us - f->msg_start_us);
        bytes += (double)f->msg_size * frac;
    }
    return (uint64_t)(bytes + 0.5);
}

int main(int argc, char **argv)
{
    struct sender_opts o;
    parse_opts(argc, argv, &o);
    srand48((long)time(NULL));

    size_t flow_sizes[FLOR_MAX_FLOWS];
    uint64_t start_delay_us[FLOR_MAX_FLOWS];
    memset(flow_sizes, 0, sizeof(flow_sizes));
    memset(start_delay_us, 0, sizeof(start_delay_us));
    init_flow_sizes(&o, flow_sizes, start_delay_us);
    size_t nflows = checked_flow_count(o.flows);

    struct flor_context fc;
    flor_open_device(&fc, o.dev, o.ib_port, o.gid_index);

    struct sender_flow *flows = calloc(nflows, sizeof(*flows));
    if (!flows)
        DIE("calloc flows failed");
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

    int max_qp_wr = fc.device_attr.max_qp_wr;
    if (max_qp_wr <= 0)
        DIE("invalid RNIC max_qp_wr %d", max_qp_wr);
    for (int i = 0; i < o.flows; i++) {
        struct sender_flow *f = &flows[i];
        size_t chunks = flor_chunks_for(flow_sizes[i], o.chunk_size);
        int data_sq_depth = o.sq_depth;
        if (data_sq_depth > max_qp_wr)
            data_sq_depth = max_qp_wr;
        if (data_sq_depth > FLOR_DATA_RECV_DEPTH)
            data_sq_depth = FLOR_DATA_RECV_DEPTH;
        if (chunks < (size_t)data_sq_depth)
            data_sq_depth = (int)chunks;

        f->id = i;
        f->msg_size = flow_sizes[i];
        f->chunks = chunks;
        f->data_sq_depth = data_sq_depth;
        f->start_delay_us = start_delay_us[i];
        f->data_psn = flor_rand_psn();
        f->ack_psn = flor_rand_psn();
        f->data_send_cq = shared_data_cq;
        f->ack_recv_cq = shared_ack_cq;
        f->dummy_cq = shared_dummy_cq;
        f->data_qp = flor_create_uc_qp(&fc, f->data_send_cq, f->dummy_cq,
                                       data_sq_depth, 1);
        f->ack_qp = flor_create_uc_qp(&fc, f->dummy_cq, f->ack_recv_cq,
                                      1, FLOR_ACK_RECV_DEPTH);

        if (posix_memalign((void **)&f->send_buf, 4096, f->msg_size))
            DIE("posix_memalign send_buf failed");
        f->send_mr = ibv_reg_mr(fc.pd, f->send_buf, f->msg_size,
                                IBV_ACCESS_LOCAL_WRITE);
        if (!f->send_mr)
            DIE("ibv_reg_mr send_buf failed");
        f->ack_bufs = calloc(FLOR_ACK_RECV_DEPTH, sizeof(struct flor_ack));
        if (!f->ack_bufs)
            DIE("calloc ACK buffers failed");
        f->ack_mr = ibv_reg_mr(fc.pd, f->ack_bufs,
                               FLOR_ACK_RECV_DEPTH * sizeof(struct flor_ack),
                               IBV_ACCESS_LOCAL_WRITE);
        if (!f->ack_mr)
            DIE("ibv_reg_mr ACK buffers failed");
        f->acked = calloc(chunks, 1);
        f->in_flight = calloc(chunks, 1);
        f->ever_sent = calloc(chunks, 1);
        f->fast_retrans = calloc(chunks, 1);
        f->fast_retransed = calloc(chunks, 1);
        f->sent_us = calloc(chunks, sizeof(uint64_t));
        f->pending_chunks = calloc(chunks, sizeof(*f->pending_chunks));
        f->pending_pos = calloc(chunks, sizeof(*f->pending_pos));
        if (!f->acked || !f->in_flight || !f->ever_sent ||
            !f->fast_retrans || !f->fast_retransed || !f->sent_us ||
            !f->pending_chunks || !f->pending_pos)
            DIE("calloc chunk state failed");
        reset_pending_positions(f);
        for (uint32_t s = 0; s < FLOR_ACK_RECV_DEPTH; s++)
            post_ack_recv(f, s);
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
                             flows[i].ack_psn, NULL, NULL, 0);
    }

    int sock = flor_tcp_connect(o.server, o.port);
    flor_exchange_info(sock, &local, &remote);
    if ((int)remote.flows != o.flows)
        DIE("peer flow count mismatch");

    for (int i = 0; i < o.flows; i++) {
        if (remote.flow[i].buf_size < flows[i].msg_size)
            DIE("receiver flow %d buffer too small", i);
        flor_connect_uc_qp(&fc, flows[i].data_qp, &remote.flow[i], true,
                           flows[i].data_psn);
        flor_connect_uc_qp(&fc, flows[i].ack_qp, &remote.flow[i], false,
                           flows[i].ack_psn);
    }

    if (o.start_at_us)
        flor_wait_until_realtime_us(o.start_at_us);
    int total_msgs = o.flows * o.messages;
    uint64_t *fcts = calloc((size_t)total_msgs, sizeof(uint64_t));
    if (!fcts)
        DIE("calloc FCT array failed");
    int fct_count = 0;
    int completed_total = 0;
    struct sender_stats st;
    memset(&st, 0, sizeof(st));
    struct flor_profile prof;
    memset(&prof, 0, sizeof(prof));
    struct rusage ru_start, ru_end;
    getrusage(RUSAGE_SELF, &ru_start);
    uint64_t bench_start = flor_now_us();
    bool timed_out = false;
    for (int i = 0; i < o.flows; i++) {
        flows[i].prof = &prof;
        flows[i].release_us = bench_start + flows[i].start_delay_us;
        start_message(&flows[i], 0, flows[i].release_us);
    }

    while (completed_total < total_msgs) {
        uint64_t loop_now = flor_now_us();
        if (o.max_runtime_us && loop_now - bench_start >= o.max_runtime_us) {
            timed_out = true;
            break;
        }
        poll_data_cq(flows, o.flows, shared_data_cq);
        poll_ack_cq(flows, o.flows, &st, shared_ack_cq);
        for (int i = 0; i < o.flows; i++) {
            struct sender_flow *f = &flows[i];

            if (f->active && (f->done_seen || f->acked_count == f->chunks)) {
                uint64_t now = flor_now_us();
                f->completion_us = now;
                f->fct_us = now - f->msg_start_us;
                fcts[fct_count++] = f->fct_us;
                f->completed_msgs++;
                completed_total++;
                f->active = false;
                if (f->completed_msgs < o.messages)
                    start_message(f, (uint32_t)f->completed_msgs, now);
            }
            drive_sends(f, &o, &remote.flow[i], &st);
        }
    }

    while (!timed_out) {
        int outstanding = 0;
        poll_data_cq(flows, o.flows, shared_data_cq);
        for (int i = 0; i < o.flows; i++) {
            outstanding += flows[i].outstanding_writes;
        }
        if (!outstanding)
            break;
    }

    uint64_t bench_end = flor_now_us();
    getrusage(RUSAGE_SELF, &ru_end);
    flor_write_full(sock, FLOR_CTRL_DONE, FLOR_CTRL_DONE_LEN);
    if (o.fct_file)
        write_fct_file(o.fct_file, flows, o.flows, bench_start);

    qsort(fcts, (size_t)fct_count, sizeof(uint64_t), cmp_u64);
    uint64_t sum = 0;
    for (int i = 0; i < fct_count; i++)
        sum += fcts[i];
    double avg = fct_count ? (double)sum / (double)fct_count : 0.0;
    double wall_s = (double)(bench_end - bench_start) / 1000000.0;
    uint64_t total_bytes = 0;
    for (int i = 0; i < o.flows; i++) {
        total_bytes += (uint64_t)flows[i].msg_size *
                       (uint64_t)flows[i].completed_msgs;
    }
    double goodput = wall_s > 0.0 ? ((double)total_bytes * 8.0 / wall_s / 1e9) : 0.0;
    uint64_t window_bytes = 0;
    double window_goodput = 0.0;
    if (o.window_set) {
        window_bytes = window_effective_bytes(flows, o.flows, bench_start,
                                              o.window_start_us,
                                              o.window_end_us);
        double window_s = (double)(o.window_end_us - o.window_start_us) /
                          1000000.0;
        window_goodput = window_s > 0.0 ?
            ((double)window_bytes * 8.0 / window_s / 1e9) : 0.0;
    }
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

    printf("flows,messages,msg_size,chunk_size,total_bytes,goodput_gbps\n");
    printf("%d,%d,%zu,%zu,%" PRIu64 ",%.6f\n", o.flows, o.messages,
           o.traffic_file ? (size_t)0 : o.msg_size, o.chunk_size, total_bytes,
           goodput);
    printf("completed_flows,total_flows,timed_out,max_runtime_us\n");
    printf("%d,%d,%u,%" PRIu64 "\n", fct_count, total_msgs,
           timed_out ? 1u : 0u, o.max_runtime_us);
    if (o.window_set) {
        printf("window_start_us,window_end_us,window_bytes,window_goodput_gbps\n");
        printf("%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f\n",
               o.window_start_us, o.window_end_us, window_bytes,
               window_goodput);
    }
    printf("avg_fct_us,p50_fct_us,p95_fct_us,p99_fct_us\n");
    printf("%.2f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", avg,
           percentile(fcts, fct_count, 50.0),
           percentile(fcts, fct_count, 95.0),
           percentile(fcts, fct_count, 99.0));
    printf("tx_chunks,retrans_chunks,dropped_first_chunks,ack_pkts,fast_retrans_events,timeout_events\n");
    printf("%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
           st.tx_chunks, st.retrans_chunks, st.dropped_first_chunks,
           st.ack_pkts, st.fast_retrans_events, st.timeout_events);
    printf("cpu_user_s,cpu_sys_s,cpu_percent\n");
    printf("%.6f,%.6f,%.2f\n", user_s, sys_s, cpu_percent);
    printf("effective_cpu_s,effective_cpu_percent,poll_hit,poll_empty,cqe_count,post_send,post_recv\n");
    printf("%.9f,%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
           active_cpu_s, effective_cpu_percent, prof.poll_hit,
           prof.poll_empty, prof.cqe_count, prof.post_send, prof.post_recv);

    close(sock);
    return 0;
}
