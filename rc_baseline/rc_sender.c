#include "rc_common.h"

enum {
    RC_WR_DATA = 1,
};

struct sender_opts {
    const char *server;
    const char *dev_name;
    const char *port;
    const char *traffic_file;
    const char *fct_file;
    int ib_port;
    int gid_index;
    int flows;
    int messages;
    size_t msg_size;
    uint64_t window_start_us;
    uint64_t window_end_us;
    uint64_t start_at_us;
    uint64_t max_runtime_us;
    uint8_t qp_timeout;
    uint8_t retry_cnt;
    uint8_t rnr_retry;
};

struct sender_flow {
    int id;
    size_t msg_size;
    uint64_t start_delay_us;
    uint32_t psn;
    bool started;
    bool completed;
    uint64_t start_us;
    uint64_t end_us;
    uint8_t *send_buf;
    struct ibv_mr *send_mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    int outstanding;
};

struct sender_stats {
    uint64_t rc_writes;
    uint64_t rc_cqes;
    uint64_t bytes_delivered;
};

static struct rc_profile *g_prof;

static uint64_t wr_id_make(uint16_t flow)
{
    return ((uint64_t)RC_WR_DATA << 56) | ((uint64_t)flow << 32);
}

static uint8_t wr_id_type(uint64_t wr_id)
{
    return (uint8_t)(wr_id >> 56);
}

static uint16_t wr_id_flow(uint64_t wr_id)
{
    return (uint16_t)(wr_id >> 32);
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
            "Usage: %s --server ADDR [options]\n"
            "  --port PORT                         TCP bootstrap port (default 21515)\n"
            "  --dev NAME --ib-port N --gid-index N\n"
            "  --flows N                           flow count (default 1)\n"
            "  --messages N                        only 1 is supported (default 1)\n"
            "  --msg-size BYTES | --traffic-file PATH\n"
            "  --fct-file PATH\n"
            "  --goodput-window-start-us US --goodput-window-end-us US\n"
            "  --max-runtime-us US                 stop after this runtime; 0 disables\n"
            "  --qp-timeout N --retry-cnt N --rnr-retry N\n",
            prog);
}

static void parse_opts(int argc, char **argv, struct sender_opts *o)
{
    static const struct option long_opts[] = {
        {"server", required_argument, NULL, 's'},
        {"port", required_argument, NULL, 'p'},
        {"dev", required_argument, NULL, 'd'},
        {"ib-port", required_argument, NULL, 'i'},
        {"gid-index", required_argument, NULL, 'g'},
        {"flows", required_argument, NULL, 'f'},
        {"messages", required_argument, NULL, 'm'},
        {"msg-size", required_argument, NULL, 'z'},
        {"traffic-file", required_argument, NULL, 'x'},
        {"fct-file", required_argument, NULL, 'y'},
        {"goodput-window-start-us", required_argument, NULL, 'w'},
        {"goodput-window-end-us", required_argument, NULL, 'W'},
        {"start-at-us", required_argument, NULL, 'a'},
        {"max-runtime-us", required_argument, NULL, 'T'},
        {"qp-timeout", required_argument, NULL, 'Q'},
        {"retry-cnt", required_argument, NULL, 'R'},
        {"rnr-retry", required_argument, NULL, 'N'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int ch;

    *o = (struct sender_opts){
        .server = NULL,
        .dev_name = NULL,
        .port = "21515",
        .traffic_file = NULL,
        .fct_file = NULL,
        .ib_port = 1,
        .gid_index = -1,
        .flows = 1,
        .messages = 1,
        .msg_size = 1024 * 1024,
        .qp_timeout = RC_DEFAULT_QP_TIMEOUT,
        .retry_cnt = RC_DEFAULT_RETRY_CNT,
        .rnr_retry = RC_DEFAULT_RNR_RETRY,
    };

    while ((ch = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (ch) {
        case 's': o->server = optarg; break;
        case 'p': o->port = optarg; break;
        case 'd': o->dev_name = optarg; break;
        case 'i': o->ib_port = (int)parse_u64(optarg, "ib-port"); break;
        case 'g': o->gid_index = (int)parse_u64(optarg, "gid-index"); break;
        case 'f': o->flows = (int)parse_u64(optarg, "flows"); break;
        case 'm': o->messages = (int)parse_u64(optarg, "messages"); break;
        case 'z': o->msg_size = (size_t)parse_u64(optarg, "msg-size"); break;
        case 'x': o->traffic_file = optarg; break;
        case 'y': o->fct_file = optarg; break;
        case 'w': o->window_start_us = parse_u64(optarg, "goodput-window-start-us"); break;
        case 'W': o->window_end_us = parse_u64(optarg, "goodput-window-end-us"); break;
        case 'a': o->start_at_us = parse_u64(optarg, "start-at-us"); break;
        case 'T': o->max_runtime_us = parse_u64(optarg, "max-runtime-us"); break;
        case 'Q': o->qp_timeout = (uint8_t)parse_u64(optarg, "qp-timeout"); break;
        case 'R': o->retry_cnt = (uint8_t)parse_u64(optarg, "retry-cnt"); break;
        case 'N': o->rnr_retry = (uint8_t)parse_u64(optarg, "rnr-retry"); break;
        case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
        default: usage(argv[0]); exit(EXIT_FAILURE);
        }
    }
    if (!o->server)
        DIE("--server is required");
    if (o->flows <= 0 || o->flows > RC_MAX_FLOWS)
        DIE("--flows must be in [1,%d]", RC_MAX_FLOWS);
    if (o->messages != 1)
        DIE("only --messages 1 is supported");
    if ((o->window_start_us || o->window_end_us) &&
        o->window_end_us <= o->window_start_us)
        DIE("invalid goodput window");
    if (o->qp_timeout > 31 || o->retry_cnt > 7 || o->rnr_retry > 7)
        DIE("invalid QP retry settings");
}

static void alloc_flow(struct rc_context *ctx, struct sender_flow *f, int id,
                       size_t msg_size, uint64_t start_delay_us)
{
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;

    memset(f, 0, sizeof(*f));
    f->id = id;
    f->msg_size = msg_size;
    f->start_delay_us = start_delay_us;
    f->psn = rc_rand_psn();
    if (msg_size == 0 || msg_size > UINT32_MAX)
        DIE("flow %d invalid message size", id);

    if (posix_memalign((void **)&f->send_buf, 4096, msg_size))
        DIE("posix_memalign sender buffer");
    memset(f->send_buf, 0, msg_size);
    f->send_mr = ibv_reg_mr(ctx->pd, f->send_buf, msg_size, access);
    if (!f->send_mr)
        DIE("ibv_reg_mr sender buffer");

    f->cq = ibv_create_cq(ctx->ctx, RC_SEND_DEPTH, NULL, NULL, 0);
    if (!f->cq)
        DIE("ibv_create_cq sender");
    f->qp = rc_create_qp(ctx, f->cq, RC_SEND_DEPTH, 1);
}

static void post_rc_write(struct sender_flow *f, const struct rc_qp_wire *remote,
                          struct sender_stats *st)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)f->send_buf,
        .length = (uint32_t)f->msg_size,
        .lkey = f->send_mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = wr_id_make((uint16_t)f->id),
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad = NULL;
    uint64_t t0 = rc_now_ns();

    wr.wr.rdma.remote_addr = remote->addr;
    wr.wr.rdma.rkey = remote->rkey;
    if (ibv_post_send(f->qp, &wr, &bad))
        DIE("ibv_post_send RC write flow %d", f->id);
    if (g_prof) {
        g_prof->active_ns += rc_now_ns() - t0;
        g_prof->post_send++;
    }
    f->outstanding++;
    st->rc_writes++;
}

static void poll_cqs(struct sender_flow *flows, int nflows,
                     struct sender_stats *st)
{
    struct ibv_wc wc[RC_MAX_POLL];
    int i;

    for (i = 0; i < nflows; i++) {
        uint64_t t0 = rc_now_ns();
        int ne = ibv_poll_cq(flows[i].cq, RC_MAX_POLL, wc);
        int j;

        if (ne < 0)
            DIE("ibv_poll_cq sender");
        if (g_prof) {
            if (ne > 0) {
                g_prof->poll_hit++;
                g_prof->cqe_count += (uint64_t)ne;
            } else {
                g_prof->poll_empty++;
            }
        }
        for (j = 0; j < ne; j++) {
            uint16_t fid = wr_id_flow(wc[j].wr_id);
            struct sender_flow *f = &flows[fid];

            if (wc[j].status != IBV_WC_SUCCESS)
                DIE("sender CQE flow %u status %s", fid,
                    ibv_wc_status_str(wc[j].status));
            if (wr_id_type(wc[j].wr_id) != RC_WR_DATA)
                DIE("unexpected sender wr_id type");
            f->outstanding--;
            f->completed = true;
            f->end_us = rc_now_us();
            st->rc_cqes++;
            st->bytes_delivered += f->msg_size;
        }
        if (g_prof && ne > 0)
            g_prof->active_ns += rc_now_ns() - t0;
    }
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return (x > y) - (x < y);
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
    fprintf(fp, "flow_id,msg_size_bytes,start_time_us,end_time_us,fct_us,start_delay_us\n");
    for (i = 0; i < nflows; i++) {
        if (!flows[i].completed)
            continue;
        uint64_t s = flows[i].start_us - bench_start;
        uint64_t e = flows[i].end_us - bench_start;
        fprintf(fp, "%d,%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                flows[i].id, flows[i].msg_size, s, e, e - s,
                flows[i].start_delay_us);
    }
    fclose(fp);
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
                        const struct sender_stats *st,
                        const struct rc_profile *prof, uint64_t bench_start,
                        uint64_t wall_us, clock_t cpu_start, clock_t cpu_end,
                        bool timed_out)
{
    uint64_t *fcts = calloc((size_t)o->flows, sizeof(*fcts));
    uint64_t fct_sum = 0;
    double wall_s = (double)wall_us / 1000000.0;
    double cpu_s = (double)(cpu_end - cpu_start) / (double)CLOCKS_PER_SEC;
    double active_cpu_s = (double)prof->active_ns / 1e9;
    double window_bytes = 0.0;
    struct rusage ru;
    int i;
    int fct_count = 0;

    if (!fcts)
        DIE("calloc fcts");
    for (i = 0; i < o->flows; i++) {
        if (!flows[i].completed)
            continue;
        uint64_t s = flows[i].start_us - bench_start;
        uint64_t e = flows[i].end_us - bench_start;
        uint64_t fct = e - s;

        fcts[fct_count++] = fct;
        fct_sum += fct;
        if (o->window_end_us > o->window_start_us) {
            uint64_t os = s > o->window_start_us ? s : o->window_start_us;
            uint64_t oe = e < o->window_end_us ? e : o->window_end_us;
            if (oe > os && e > s)
                window_bytes += (double)flows[i].msg_size *
                                (double)(oe - os) / (double)(e - s);
        }
    }
    qsort(fcts, (size_t)fct_count, sizeof(*fcts), cmp_u64);

    printf("flows,messages,total_bytes,goodput_gbps\n");
    printf("%d,1,%" PRIu64 ",%.6f\n", o->flows, st->bytes_delivered,
           wall_s > 0.0 ? ((double)st->bytes_delivered * 8.0 / wall_s) / 1e9 : 0.0);
    printf("completed_flows,total_flows,timed_out,max_runtime_us\n");
    printf("%d,%d,%u,%" PRIu64 "\n", fct_count, o->flows, timed_out ? 1u : 0u,
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
    printf("rc_writes,rc_cqes\n");
    printf("%" PRIu64 ",%" PRIu64 "\n", st->rc_writes, st->rc_cqes);
    getrusage(RUSAGE_SELF, &ru);
    printf("cpu_user_s,cpu_sys_s,cpu_percent\n");
    printf("%.6f,%.6f,%.4f\n", rc_timeval_sec(&ru.ru_utime),
           rc_timeval_sec(&ru.ru_stime), wall_s > 0.0 ? cpu_s / wall_s * 100.0 : 0.0);
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
               o->window_end_us, (window_bytes * 8.0 / win_s) / 1e9);
    }
    free(fcts);
}

int main(int argc, char **argv)
{
    struct sender_opts opts;
    struct rc_context ctx;
    struct sender_flow *flows;
    struct rc_exchange *local;
    struct rc_exchange *remote;
    struct sender_stats stats = { 0 };
    struct rc_profile prof = { 0 };
    size_t *sizes;
    uint64_t *starts;
    int sock;
    int completed = 0;
    uint64_t bench_start;
    uint64_t end_us;
    clock_t cpu_start;
    clock_t cpu_end;
    int i;
    bool timed_out = false;

    parse_opts(argc, argv, &opts);
    g_prof = &prof;
    srand((unsigned int)(time(NULL) ^ getpid()));
    srand48((long)(time(NULL) ^ getpid()));

    sizes = calloc(RC_MAX_FLOWS, sizeof(*sizes));
    starts = calloc(RC_MAX_FLOWS, sizeof(*starts));
    if (!sizes || !starts)
        DIE("calloc traffic arrays");
    if (opts.traffic_file) {
        int inferred = rc_load_traffic_file(opts.traffic_file, RC_MAX_FLOWS,
                                            sizes, starts);
        if (opts.flows == 1)
            opts.flows = inferred;
    }
    for (i = 0; i < opts.flows; i++) {
        if (!sizes[i])
            sizes[i] = opts.msg_size;
    }

    rc_open_device(&ctx, opts.dev_name, opts.ib_port, opts.gid_index);
    ctx.qp_timeout = opts.qp_timeout;
    ctx.retry_cnt = opts.retry_cnt;
    ctx.rnr_retry = opts.rnr_retry;
    flows = calloc((size_t)opts.flows, sizeof(*flows));
    local = calloc(1, sizeof(*local));
    remote = calloc(1, sizeof(*remote));
    if (!flows || !local || !remote)
        DIE("calloc sender state");

    local->magic = RC_MAGIC;
    local->version = RC_VERSION;
    local->flows = (uint32_t)opts.flows;
    local->gid_index = (uint32_t)opts.gid_index;
    for (i = 0; i < opts.flows; i++) {
        alloc_flow(&ctx, &flows[i], i, sizes[i], starts[i]);
        rc_fill_local_wire(&ctx, &local->flow[i], flows[i].qp, flows[i].psn,
                           NULL, NULL, 0);
    }

    sock = rc_tcp_connect(opts.server, opts.port);
    rc_exchange_info(sock, local, remote);
    if (remote->flows != (uint32_t)opts.flows)
        DIE("peer flow count mismatch");
    for (i = 0; i < opts.flows; i++)
        rc_connect_qp(&ctx, flows[i].qp, &remote->flow[i], flows[i].psn);

    if (opts.start_at_us)
        rc_wait_until_realtime_us(opts.start_at_us);
    bench_start = rc_now_us();
    cpu_start = clock();
    while (completed < opts.flows) {
        uint64_t now = rc_now_us();

        if (opts.max_runtime_us && now - bench_start >= opts.max_runtime_us) {
            timed_out = true;
            break;
        }
        poll_cqs(flows, opts.flows, &stats);
        for (i = 0; i < opts.flows; i++) {
            struct sender_flow *f = &flows[i];

            if (!f->started && now - bench_start >= f->start_delay_us) {
                f->started = true;
                f->start_us = now;
                post_rc_write(f, &remote->flow[i], &stats);
            }
        }
        completed = count_completed(flows, opts.flows);
    }
    rc_write_full(sock, RC_TCP_DONE, RC_TCP_DONE_LEN);
    cpu_end = clock();
    end_us = rc_now_us();

    write_fct_file(opts.fct_file, flows, opts.flows, bench_start);
    print_stats(&opts, flows, &stats, &prof, bench_start,
                end_us - bench_start, cpu_start, cpu_end, timed_out);
    close(sock);
    free(sizes);
    free(starts);
    return 0;
}
