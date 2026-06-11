#include "flor_common.h"

uint64_t flor_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t flor_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

uint64_t flor_realtime_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

void flor_wait_until_realtime_us(uint64_t start_at_us)
{
    uint64_t now = flor_realtime_us();

    if (start_at_us <= now)
        DIE("--start-at-us is in the past");
    while (start_at_us - now > 1000) {
        struct timespec ts;
        uint64_t sleep_us = start_at_us - now - 500;

        ts.tv_sec = (time_t)(sleep_us / 1000000ull);
        ts.tv_nsec = (long)(sleep_us % 1000000ull) * 1000L;
        nanosleep(&ts, NULL);
        now = flor_realtime_us();
    }
    while (flor_realtime_us() < start_at_us)
        ;
}

double flor_timeval_sec(const struct timeval *tv)
{
    return (double)tv->tv_sec + (double)tv->tv_usec / 1000000.0;
}

uint32_t flor_rand_psn(void)
{
    return (uint32_t)lrand48() & 0xffffffu;
}

size_t flor_chunks_for(size_t msg_size, size_t chunk_size)
{
    return (msg_size + chunk_size - 1) / chunk_size;
}

size_t flor_chunk_len(size_t msg_size, size_t chunk_size, uint32_t chunk_id)
{
    size_t off = (size_t)chunk_id * chunk_size;
    size_t left = msg_size - off;
    return left < chunk_size ? left : chunk_size;
}

uint32_t flor_imm(uint32_t flow_id, uint32_t chunk_id)
{
    return ((flow_id & 0xfffu) << FLOR_IMM_CHUNK_BITS) |
           (chunk_id & 0xfffffu);
}

void flor_parse_imm(uint32_t imm, uint32_t *flow_id, uint32_t *chunk_id)
{
    *flow_id = (imm >> FLOR_IMM_CHUNK_BITS) & 0xfffu;
    *chunk_id = imm & 0xfffffu;
}

int flor_load_traffic_file(const char *path, int max_flows, size_t *sizes,
                           uint64_t *start_delay_us)
{
    FILE *fp = fopen(path, "r");
    size_t *raw_sizes;
    uint64_t *raw_delays;
    int min_id = max_flows + 1;
    int max_id = -1;
    int base;
    int flows;
    if (!fp)
        DIE("failed to open traffic file %s: %s", path, strerror(errno));

    raw_sizes = calloc((size_t)max_flows + 1, sizeof(*raw_sizes));
    raw_delays = calloc((size_t)max_flows + 1, sizeof(*raw_delays));
    if (!raw_sizes || !raw_delays)
        DIE("calloc traffic arrays failed");

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '#')
            continue;

        unsigned id = 0;
        unsigned long long bytes = 0;
        double delay_ms = 0.0;
        int fields = sscanf(p, "%u %llu %lf", &id, &bytes, &delay_ms);
        if (fields < 2)
            DIE("bad traffic line: %s", line);
        if (id > (unsigned)max_flows)
            DIE("traffic flow id %u out of range 0..%d or 1..%d", id,
                max_flows - 1, max_flows);
        if (bytes == 0 || bytes > (unsigned long long)SIZE_MAX)
            DIE("invalid traffic size for flow %u", id);

        if (raw_sizes[id] != 0)
            DIE("duplicate traffic flow id %u", id);
        raw_sizes[id] = (size_t)bytes;
        raw_delays[id] = fields >= 3 ? (uint64_t)(delay_ms * 1000.0 + 0.5) : 0;
        if ((int)id < min_id)
            min_id = (int)id;
        if ((int)id > max_id)
            max_id = (int)id;
    }
    fclose(fp);

    if (max_id < 0)
        DIE("traffic file %s has no flows", path);
    base = min_id == 0 ? 0 : 1;
    flows = max_id - base + 1;
    if (flows <= 0 || flows > max_flows)
        DIE("traffic file %s has invalid flow id range", path);

    for (int id = base; id <= max_id; id++) {
        int idx = id - base;
        if (raw_sizes[id] == 0)
            DIE("traffic file has missing flow id %d", id);
        sizes[idx] = raw_sizes[id];
        if (start_delay_us)
            start_delay_us[idx] = raw_delays[id];
    }
    free(raw_sizes);
    free(raw_delays);
    return flows;
}

static int set_reuseaddr(int fd)
{
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
}

int flor_tcp_listen(const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp = NULL;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int rc = getaddrinfo(NULL, port, &hints, &res);
    if (rc)
        DIE("getaddrinfo listen: %s", gai_strerror(rc));

    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        set_reuseaddr(fd);
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0 &&
            listen(fd, 1) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        DIE("failed to listen on TCP port %s", port);
    return fd;
}

int flor_tcp_accept(int listen_fd)
{
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        DIE("accept: %s", strerror(errno));
    return fd;
}

int flor_tcp_connect(const char *server, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp = NULL;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(server, port, &hints, &res);
    if (rc)
        DIE("getaddrinfo connect: %s", gai_strerror(rc));

    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        DIE("failed to connect to %s:%s", server, port);
    return fd;
}

void flor_write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            DIE("write: %s", strerror(errno));
        }
        if (n == 0)
            DIE("short write");
        p += n;
        len -= (size_t)n;
    }
}

void flor_read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            DIE("read: %s", strerror(errno));
        }
        if (n == 0)
            DIE("peer closed TCP connection");
        p += n;
        len -= (size_t)n;
    }
}

void flor_open_device(struct flor_context *fc, const char *dev_name,
                      int ib_port, int gid_index)
{
    int num = 0;
    struct ibv_device **list = ibv_get_device_list(&num);
    if (!list || num == 0)
        DIE("no RDMA devices found");

    struct ibv_device *dev = NULL;
    for (int i = 0; i < num; i++) {
        if (!dev_name || strcmp(ibv_get_device_name(list[i]), dev_name) == 0) {
            dev = list[i];
            break;
        }
    }
    if (!dev)
        DIE("RDMA device %s not found", dev_name ? dev_name : "<first>");

    memset(fc, 0, sizeof(*fc));
    fc->ctx = ibv_open_device(dev);
    if (!fc->ctx)
        DIE("ibv_open_device failed");
    fc->pd = ibv_alloc_pd(fc->ctx);
    if (!fc->pd)
        DIE("ibv_alloc_pd failed");
    fc->ib_port = ib_port;
    fc->gid_index = gid_index;

    if (ibv_query_device(fc->ctx, &fc->device_attr))
        DIE("ibv_query_device failed");
    if (ibv_query_port(fc->ctx, ib_port, &fc->port_attr))
        DIE("ibv_query_port failed");
    if (gid_index >= 0 && ibv_query_gid(fc->ctx, ib_port, gid_index, &fc->gid))
        DIE("ibv_query_gid failed");

    ibv_free_device_list(list);
}

struct ibv_qp *flor_create_uc_qp(struct flor_context *fc,
                                 struct ibv_cq *send_cq,
                                 struct ibv_cq *recv_cq,
                                 int max_send_wr,
                                 int max_recv_wr)
{
    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_type = IBV_QPT_UC;
    attr.send_cq = send_cq;
    attr.recv_cq = recv_cq;
    attr.cap.max_send_wr = max_send_wr;
    attr.cap.max_recv_wr = max_recv_wr;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    attr.cap.max_inline_data = 0;

    struct ibv_qp *qp = ibv_create_qp(fc->pd, &attr);
    if (!qp)
        DIE("ibv_create_qp failed (send_wr=%d recv_wr=%d errno=%d: %s)",
            max_send_wr, max_recv_wr, errno, strerror(errno));

    struct ibv_qp_attr init;
    memset(&init, 0, sizeof(init));
    init.qp_state = IBV_QPS_INIT;
    init.pkey_index = 0;
    init.port_num = fc->ib_port;
    init.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(qp, &init, flags))
        DIE("modify QP INIT failed");
    return qp;
}

void flor_connect_uc_qp(struct flor_context *fc, struct ibv_qp *qp,
                        const struct flor_qp_wire *remote, bool data_qp,
                        uint32_t local_psn)
{
    struct ibv_qp_attr rtr;
    memset(&rtr, 0, sizeof(rtr));
    rtr.qp_state = IBV_QPS_RTR;
    rtr.path_mtu = IBV_MTU_1024;
    rtr.dest_qp_num = data_qp ? remote->data_qpn : remote->ack_qpn;
    rtr.rq_psn = data_qp ? remote->data_psn : remote->ack_psn;
    rtr.ah_attr.dlid = remote->lid;
    rtr.ah_attr.sl = 0;
    rtr.ah_attr.src_path_bits = 0;
    rtr.ah_attr.port_num = fc->ib_port;
    if (fc->gid_index >= 0) {
        rtr.ah_attr.is_global = 1;
        rtr.ah_attr.grh.dgid = remote->gid;
        rtr.ah_attr.grh.sgid_index = fc->gid_index;
        rtr.ah_attr.grh.hop_limit = 64;
    }

    int rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                    IBV_QP_DEST_QPN | IBV_QP_RQ_PSN;
    if (ibv_modify_qp(qp, &rtr, rtr_flags))
        DIE("modify QP RTR failed");

    struct ibv_qp_attr rts;
    memset(&rts, 0, sizeof(rts));
    rts.qp_state = IBV_QPS_RTS;
    rts.sq_psn = local_psn;
    int rts_flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
    if (ibv_modify_qp(qp, &rts, rts_flags))
        DIE("modify QP RTS failed");
}

void flor_fill_local_wire(struct flor_context *fc, struct flor_qp_wire *wire,
                          struct ibv_qp *data_qp, struct ibv_qp *ack_qp,
                          uint32_t data_psn, uint32_t ack_psn,
                          struct ibv_mr *remote_write_mr, void *buf,
                          uint64_t buf_size)
{
    memset(wire, 0, sizeof(*wire));
    wire->lid = fc->port_attr.lid;
    wire->data_qpn = data_qp->qp_num;
    wire->ack_qpn = ack_qp->qp_num;
    wire->data_psn = data_psn;
    wire->ack_psn = ack_psn;
    if (remote_write_mr) {
        wire->rkey = remote_write_mr->rkey;
        wire->addr = (uintptr_t)buf;
        wire->buf_size = buf_size;
    }
    wire->gid = fc->gid;
}

void flor_exchange_info(int sock, const struct flor_exchange *local,
                        struct flor_exchange *remote)
{
    flor_write_full(sock, local, sizeof(*local));
    flor_read_full(sock, remote, sizeof(*remote));
    if (remote->magic != FLOR_MAGIC || remote->version != FLOR_VERSION)
        DIE("incompatible peer exchange");
    if (remote->flows == 0 || remote->flows > FLOR_MAX_FLOWS)
        DIE("invalid peer flow count %u", remote->flows);
}
