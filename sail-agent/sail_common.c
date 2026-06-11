#include "sail_common.h"

uint64_t sail_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t sail_now_us(void)
{
    return sail_now_ns() / 1000ull;
}

uint64_t sail_realtime_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

void sail_wait_until_realtime_us(uint64_t start_at_us)
{
    uint64_t now = sail_realtime_us();

    if (start_at_us <= now)
        DIE("--start-at-us is in the past");
    while (start_at_us - now > 1000) {
        struct timespec ts;
        uint64_t sleep_us = start_at_us - now - 500;

        ts.tv_sec = (time_t)(sleep_us / 1000000ull);
        ts.tv_nsec = (long)(sleep_us % 1000000ull) * 1000L;
        nanosleep(&ts, NULL);
        now = sail_realtime_us();
    }
    while (sail_realtime_us() < start_at_us)
        ;
}

double sail_timeval_sec(const struct timeval *tv)
{
    return (double)tv->tv_sec + (double)tv->tv_usec / 1000000.0;
}

uint32_t sail_rand_psn(void)
{
    return (uint32_t)lrand48() & 0xffffffu;
}

size_t sail_packets_for(size_t msg_size, size_t pmtu)
{
    return (msg_size + pmtu - 1) / pmtu;
}

size_t sail_packet_len(size_t msg_size, size_t pmtu, uint32_t packet_id)
{
    size_t off = (size_t)packet_id * pmtu;
    size_t left = msg_size - off;
    return left < pmtu ? left : pmtu;
}

uint32_t sail_psn_add(uint32_t psn, uint32_t delta)
{
    return (psn + delta) & 0xffffffu;
}

uint32_t sail_psn_diff(uint32_t psn, uint32_t start_psn)
{
    return (psn - start_psn) & 0xffffffu;
}

uint32_t sail_imm(uint32_t flow_id, uint32_t packet_id)
{
    return ((flow_id & ((1u << SAIL_FLOW_BITS) - 1u)) << SAIL_PACKET_BITS) |
           (packet_id & ((1u << SAIL_PACKET_BITS) - 1u));
}

void sail_parse_imm(uint32_t imm, uint32_t *flow_id, uint32_t *packet_id)
{
    *flow_id = (imm >> SAIL_PACKET_BITS) & ((1u << SAIL_FLOW_BITS) - 1u);
    *packet_id = imm & ((1u << SAIL_PACKET_BITS) - 1u);
}

static uint64_t parse_u64_field(const char *s, const char *name)
{
    char *end = NULL;
    unsigned long long v;

    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0')
        DIE("invalid %s: %s", name, s);
    return (uint64_t)v;
}

static uint64_t parse_delay_ms_to_us(const char *s)
{
    char *end = NULL;
    double v;

    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0' || v < 0.0)
        DIE("invalid start delay ms: %s", s);
    return (uint64_t)(v * 1000.0 + 0.5);
}

int sail_load_traffic_file(const char *path, int max_flows, size_t *sizes,
                           uint64_t *start_delay_us)
{
    FILE *fp = fopen(path, "r");
    char line[512];
    int max_id = -1;

    if (!fp)
        DIE("open traffic file %s: %s", path, strerror(errno));

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *id_s;
        char *size_s;
        char *delay_s;
        long id;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '#')
            continue;

        id_s = strtok(p, " \t\r\n,");
        size_s = strtok(NULL, " \t\r\n,");
        delay_s = strtok(NULL, " \t\r\n,");
        if (!id_s || !size_s)
            DIE("bad traffic line: %s", line);

        id = (long)parse_u64_field(id_s, "flow id");
        if (id < 0 || id >= max_flows)
            DIE("traffic flow id out of range: %ld", id);
        sizes[id] = (size_t)parse_u64_field(size_s, "flow size");
        start_delay_us[id] = delay_s ? parse_delay_ms_to_us(delay_s) : 0;
        if ((int)id > max_id)
            max_id = (int)id;
    }

    fclose(fp);
    return max_id + 1;
}

static int set_reuseaddr(int fd)
{
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
}

void sail_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        DIE("fcntl F_GETFL: %s", strerror(errno));
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        DIE("fcntl F_SETFL: %s", strerror(errno));
}

int sail_tcp_listen(const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp;
    int fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    rc = getaddrinfo(NULL, port, &hints, &res);
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

int sail_tcp_accept(int listen_fd)
{
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        DIE("accept: %s", strerror(errno));
    return fd;
}

int sail_tcp_connect(const char *server, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp;
    int fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(server, port, &hints, &res);
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

int sail_udp_bind_addr(const char *addr, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp;
    int fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (!addr)
        hints.ai_flags = AI_PASSIVE;
    rc = getaddrinfo(addr, port, &hints, &res);
    if (rc)
        DIE("getaddrinfo udp bind %s:%s: %s", addr ? addr : "*", port,
            gai_strerror(rc));

    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        set_reuseaddr(fd);
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        DIE("failed to bind UDP %s:%s", addr ? addr : "*", port);
    sail_set_nonblock(fd);
    return fd;
}

int sail_udp_bind(const char *port)
{
    return sail_udp_bind_addr(NULL, port);
}

int sail_udp_connect(const char *server, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp;
    int fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    rc = getaddrinfo(server, port, &hints, &res);
    if (rc)
        DIE("getaddrinfo udp connect: %s", gai_strerror(rc));

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
        DIE("failed to connect UDP %s:%s", server, port);
    return fd;
}

void sail_write_full(int fd, const void *buf, size_t len)
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

void sail_read_full(int fd, void *buf, size_t len)
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

void sail_open_device(struct sail_context *sc, const char *dev_name,
                      int ib_port, int gid_index)
{
    struct ibv_device **list;
    struct ibv_device *dev = NULL;
    int num = 0;
    int i;

    list = ibv_get_device_list(&num);
    if (!list || num == 0)
        DIE("no RDMA devices found");
    for (i = 0; i < num; i++) {
        if (!dev_name || strcmp(ibv_get_device_name(list[i]), dev_name) == 0) {
            dev = list[i];
            break;
        }
    }
    if (!dev)
        DIE("RDMA device %s not found", dev_name ? dev_name : "<first>");

    memset(sc, 0, sizeof(*sc));
    sc->ctx = ibv_open_device(dev);
    if (!sc->ctx)
        DIE("ibv_open_device failed");
    sc->pd = ibv_alloc_pd(sc->ctx);
    if (!sc->pd)
        DIE("ibv_alloc_pd failed");
    sc->ib_port = ib_port;
    sc->gid_index = gid_index;
    if (ibv_query_device(sc->ctx, &sc->device_attr))
        DIE("ibv_query_device failed");
    if (ibv_query_port(sc->ctx, ib_port, &sc->port_attr))
        DIE("ibv_query_port failed");
    if (gid_index >= 0 && ibv_query_gid(sc->ctx, ib_port, gid_index, &sc->gid))
        DIE("ibv_query_gid failed");
    ibv_free_device_list(list);
}

struct ibv_qp *sail_create_qp(struct sail_context *sc, enum ibv_qp_type type,
                              struct ibv_cq *send_cq, struct ibv_cq *recv_cq,
                              int max_send_wr, int max_recv_wr)
{
    struct ibv_qp_init_attr attr;
    struct ibv_qp_attr init;
    struct ibv_qp *qp;
    int flags;

    memset(&attr, 0, sizeof(attr));
    attr.qp_type = type;
    attr.send_cq = send_cq;
    attr.recv_cq = recv_cq;
    attr.cap.max_send_wr = max_send_wr;
    attr.cap.max_recv_wr = max_recv_wr;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    attr.cap.max_inline_data = 0;
    qp = ibv_create_qp(sc->pd, &attr);
    if (!qp)
        DIE("ibv_create_qp failed");

    memset(&init, 0, sizeof(init));
    init.qp_state = IBV_QPS_INIT;
    init.pkey_index = 0;
    init.port_num = sc->ib_port;
    init.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
            IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(qp, &init, flags))
        DIE("modify QP INIT failed");
    return qp;
}

void sail_connect_qp(struct sail_context *sc, struct ibv_qp *qp,
                     enum ibv_qp_type type, const struct sail_qp_wire *remote,
                     bool data_qp, uint32_t local_psn)
{
    struct ibv_qp_attr rtr;
    struct ibv_qp_attr rts;
    int rtr_flags;
    int rts_flags;

    memset(&rtr, 0, sizeof(rtr));
    rtr.qp_state = IBV_QPS_RTR;
    rtr.path_mtu = IBV_MTU_1024;
    rtr.dest_qp_num = data_qp ? remote->data_qpn : remote->ctrl_qpn;
    rtr.rq_psn = data_qp ? remote->data_psn : remote->ctrl_psn;
    rtr.ah_attr.dlid = remote->lid;
    rtr.ah_attr.sl = 0;
    rtr.ah_attr.src_path_bits = 0;
    rtr.ah_attr.port_num = sc->ib_port;
    if (sc->gid_index >= 0) {
        rtr.ah_attr.is_global = 1;
        rtr.ah_attr.grh.dgid = remote->gid;
        rtr.ah_attr.grh.sgid_index = sc->gid_index;
        rtr.ah_attr.grh.hop_limit = 64;
    }
    if (type == IBV_QPT_RC) {
        rtr.max_dest_rd_atomic = 1;
        rtr.min_rnr_timer = 12;
    }

    rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN;
    if (type == IBV_QPT_RC)
        rtr_flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(qp, &rtr, rtr_flags))
        DIE("modify QP RTR failed");

    memset(&rts, 0, sizeof(rts));
    rts.qp_state = IBV_QPS_RTS;
    rts.sq_psn = local_psn;
    if (type == IBV_QPT_RC) {
        rts.timeout = 14;
        rts.retry_cnt = 7;
        rts.rnr_retry = 7;
        rts.max_rd_atomic = 1;
    }
    rts_flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
    if (type == IBV_QPT_RC)
        rts_flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                     IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &rts, rts_flags))
        DIE("modify QP RTS failed");
}

void sail_fill_local_wire(struct sail_context *sc, struct sail_qp_wire *wire,
                          struct ibv_qp *data_qp, struct ibv_qp *ctrl_qp,
                          uint32_t data_psn, uint32_t ctrl_psn,
                          struct ibv_mr *remote_write_mr, void *buf,
                          uint64_t buf_size)
{
    memset(wire, 0, sizeof(*wire));
    wire->lid = sc->port_attr.lid;
    wire->data_qpn = data_qp->qp_num;
    wire->ctrl_qpn = ctrl_qp->qp_num;
    wire->data_psn = data_psn;
    wire->ctrl_psn = ctrl_psn;
    if (remote_write_mr) {
        wire->rkey = remote_write_mr->rkey;
        wire->addr = (uintptr_t)buf;
        wire->buf_size = buf_size;
    }
    wire->gid = sc->gid;
}

void sail_exchange_info(int sock, const struct sail_exchange *local,
                        struct sail_exchange *remote)
{
    sail_write_full(sock, local, sizeof(*local));
    sail_read_full(sock, remote, sizeof(*remote));
    if (remote->magic != SAIL_MAGIC || remote->version != SAIL_VERSION)
        DIE("incompatible peer exchange");
    if (remote->flows == 0 || remote->flows > SAIL_MAX_FLOWS)
        DIE("invalid peer flow count %u", remote->flows);
}
