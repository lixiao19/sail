#include "rc_common.h"

struct receiver_opts {
    const char *dev_name;
    const char *port;
    const char *traffic_file;
    int ib_port;
    int gid_index;
    int flows;
    size_t msg_size;
    uint8_t qp_timeout;
    uint8_t retry_cnt;
    uint8_t rnr_retry;
};

struct receiver_flow {
    int id;
    size_t msg_size;
    uint32_t psn;
    uint8_t *data_buf;
    struct ibv_mr *data_mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
};

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
            "  --port PORT                         TCP bootstrap port (default 21515)\n"
            "  --dev NAME --ib-port N --gid-index N\n"
            "  --flows N                           flow count (default 1)\n"
            "  --msg-size BYTES | --traffic-file PATH\n"
            "  --qp-timeout N --retry-cnt N --rnr-retry N\n",
            prog);
}

static void parse_opts(int argc, char **argv, struct receiver_opts *o)
{
    static const struct option long_opts[] = {
        {"port", required_argument, NULL, 'p'},
        {"dev", required_argument, NULL, 'd'},
        {"ib-port", required_argument, NULL, 'i'},
        {"gid-index", required_argument, NULL, 'g'},
        {"flows", required_argument, NULL, 'f'},
        {"msg-size", required_argument, NULL, 'z'},
        {"traffic-file", required_argument, NULL, 'x'},
        {"qp-timeout", required_argument, NULL, 'Q'},
        {"retry-cnt", required_argument, NULL, 'R'},
        {"rnr-retry", required_argument, NULL, 'N'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int ch;

    *o = (struct receiver_opts){
        .dev_name = NULL,
        .port = "21515",
        .traffic_file = NULL,
        .ib_port = 1,
        .gid_index = -1,
        .flows = 1,
        .msg_size = 1024 * 1024,
        .qp_timeout = RC_DEFAULT_QP_TIMEOUT,
        .retry_cnt = RC_DEFAULT_RETRY_CNT,
        .rnr_retry = RC_DEFAULT_RNR_RETRY,
    };
    while ((ch = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (ch) {
        case 'p': o->port = optarg; break;
        case 'd': o->dev_name = optarg; break;
        case 'i': o->ib_port = (int)parse_u64(optarg, "ib-port"); break;
        case 'g': o->gid_index = (int)parse_u64(optarg, "gid-index"); break;
        case 'f': o->flows = (int)parse_u64(optarg, "flows"); break;
        case 'z': o->msg_size = (size_t)parse_u64(optarg, "msg-size"); break;
        case 'x': o->traffic_file = optarg; break;
        case 'Q': o->qp_timeout = (uint8_t)parse_u64(optarg, "qp-timeout"); break;
        case 'R': o->retry_cnt = (uint8_t)parse_u64(optarg, "retry-cnt"); break;
        case 'N': o->rnr_retry = (uint8_t)parse_u64(optarg, "rnr-retry"); break;
        case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
        default: usage(argv[0]); exit(EXIT_FAILURE);
        }
    }
    if (o->flows <= 0 || o->flows > RC_MAX_FLOWS)
        DIE("--flows must be in [1,%d]", RC_MAX_FLOWS);
    if (o->qp_timeout > 31 || o->retry_cnt > 7 || o->rnr_retry > 7)
        DIE("invalid QP retry settings");
}

static void alloc_flow(struct rc_context *ctx, struct receiver_flow *f, int id,
                       size_t msg_size)
{
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;

    memset(f, 0, sizeof(*f));
    f->id = id;
    f->msg_size = msg_size;
    f->psn = rc_rand_psn();
    if (msg_size == 0)
        DIE("flow %d invalid message size", id);

    if (posix_memalign((void **)&f->data_buf, 4096, msg_size))
        DIE("posix_memalign receiver buffer");
    memset(f->data_buf, 0, msg_size);
    f->data_mr = ibv_reg_mr(ctx->pd, f->data_buf, msg_size, access);
    if (!f->data_mr)
        DIE("ibv_reg_mr receiver buffer");

    f->cq = ibv_create_cq(ctx->ctx, 1, NULL, NULL, 0);
    if (!f->cq)
        DIE("ibv_create_cq receiver");
    f->qp = rc_create_qp(ctx, f->cq, 1, 1);
}

static void print_stats(const struct receiver_opts *o, uint64_t bytes,
                        uint64_t wall_us, clock_t cpu_start, clock_t cpu_end)
{
    double wall_s = (double)wall_us / 1000000.0;
    double cpu_s = (double)(cpu_end - cpu_start) / (double)CLOCKS_PER_SEC;
    struct rusage ru;

    printf("role,flows,total_bytes,wall_s\n");
    printf("receiver,%d,%" PRIu64 ",%.6f\n", o->flows, bytes, wall_s);
    getrusage(RUSAGE_SELF, &ru);
    printf("cpu_user_s,cpu_sys_s,cpu_percent\n");
    printf("%.6f,%.6f,%.4f\n", rc_timeval_sec(&ru.ru_utime),
           rc_timeval_sec(&ru.ru_stime), wall_s > 0.0 ? cpu_s / wall_s * 100.0 : 0.0);
}

int main(int argc, char **argv)
{
    struct receiver_opts opts;
    struct rc_context ctx;
    struct receiver_flow *flows;
    struct rc_exchange *local;
    struct rc_exchange *remote;
    size_t *sizes;
    uint64_t *starts;
    uint64_t total_bytes = 0;
    int listen_fd;
    int sock;
    uint64_t start_us;
    uint64_t end_us;
    clock_t cpu_start;
    clock_t cpu_end;
    char done[RC_TCP_DONE_LEN];
    int i;

    parse_opts(argc, argv, &opts);
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
        total_bytes += sizes[i];
    }

    rc_open_device(&ctx, opts.dev_name, opts.ib_port, opts.gid_index);
    ctx.qp_timeout = opts.qp_timeout;
    ctx.retry_cnt = opts.retry_cnt;
    ctx.rnr_retry = opts.rnr_retry;
    flows = calloc((size_t)opts.flows, sizeof(*flows));
    local = calloc(1, sizeof(*local));
    remote = calloc(1, sizeof(*remote));
    if (!flows || !local || !remote)
        DIE("calloc receiver state");

    local->magic = RC_MAGIC;
    local->version = RC_VERSION;
    local->flows = (uint32_t)opts.flows;
    local->gid_index = (uint32_t)opts.gid_index;
    for (i = 0; i < opts.flows; i++) {
        alloc_flow(&ctx, &flows[i], i, sizes[i]);
        rc_fill_local_wire(&ctx, &local->flow[i], flows[i].qp, flows[i].psn,
                           flows[i].data_mr, flows[i].data_buf, sizes[i]);
    }

    listen_fd = rc_tcp_listen(opts.port);
    sock = rc_tcp_accept(listen_fd);
    close(listen_fd);
    rc_exchange_info(sock, local, remote);
    if (remote->flows != (uint32_t)opts.flows)
        DIE("peer flow count mismatch");
    for (i = 0; i < opts.flows; i++)
        rc_connect_qp(&ctx, flows[i].qp, &remote->flow[i], flows[i].psn);

    start_us = rc_now_us();
    cpu_start = clock();
    rc_read_full(sock, done, RC_TCP_DONE_LEN);
    if (memcmp(done, RC_TCP_DONE, RC_TCP_DONE_LEN) != 0)
        DIE("unexpected TCP trailer");
    cpu_end = clock();
    end_us = rc_now_us();

    print_stats(&opts, total_bytes, end_us - start_us, cpu_start, cpu_end);
    close(sock);
    free(sizes);
    free(starts);
    return 0;
}
