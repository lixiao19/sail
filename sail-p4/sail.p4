#include <core.p4>
#include <tna.p4>

typedef bit<48> mac_addr_t;
typedef bit<32> ipv4_addr_t;

const bit<16> ETHERTYPE_IPV4 = 0x0800;
const bit<8> IP_PROTOCOL_UDP = 17;
const bit<16> ROCE_V2 = 4791;
const bit<16> ROCE_V2_WAN = 4790;
const bit<16> SAIL_NOTIFY_PORT = 20516;

const bit<8> RC_WRITE_FIRST = 6;
const bit<8> RC_WRITE_MIDDLE = 7;
const bit<8> RC_WRITE_LAST = 8;
const bit<8> RC_WRITE_ONLY = 10;
const bit<8> RC_CNP = 129;

const bit<32> SAIL_NOTIFY_MAGIC = 0x534e5432;
const bit<32> SAIL_NOTIFY_SEED_MAGIC = 0x534e5344;
const bit<32> SAIL_QUEUE_MAGIC = 0x53415155;
const bit<8> SAIL_NOTIFY_LOSS = 1;
const bit<8> SAIL_NOTIFY_COMPLETE = 2;
const bit<8> SAIL_QUEUE_PKT = 1;
const bit<8> SAIL_QUEUE_DUMMY = 2;
const bit<8> SAIL_QUEUE_WAIT_HEAD = 0;
const bit<8> SAIL_QUEUE_HEAD_READY = 1;
const bit<8> SAIL_QUEUE_HEAD_ADVANCE = 2;
const bit<2> RELEASE_QUEUE = 1;
const bit<2> RELEASE_RECEIVER = 2;
const bit<4> PATH_OTHER = 0;
const bit<4> PATH_NOTIFY = 1;
const bit<4> PATH_BUILD_DUMMY = 2;
const bit<4> PATH_QUEUE = 3;
const bit<4> PATH_SENDER = 4;
const bit<4> PATH_RDCI = 5;
const bit<4> PATH_RDCI_BYPASS = 6;
const bit<4> PATH_RECEIVER = 7;
const bit<4> PATH_RETURN_WAN = 8;

const bit<3> NOTIFY_MIRROR_TYPE = 1;
const MirrorId_t NOTIFY_MIRROR_SID = 66;
const bit<16> DUMMY_MC_GID = 130;
const bit<16> DUMMY_MC_RID = 21;

#define STEP 8
#define MSH_BYTES 42
#define MSH_BYTES_SUB_IP 22
#define QUEUE_SHIM_BYTES 16
#define QUEUE_BUDGET 1024

#define PORT_SENDER 184
#define PORT_RECEIVER 52
#define PORT_WAN_TO_RECEIVER 20
#define PORT_WAN_TO_SENDER 4
#define PORT_NOTIFY_LOOP 128
#define PORT_QUEUE_LOOP 136

header ethernet_h {
    mac_addr_t dst_mac;
    mac_addr_t src_mac;
    bit<16> ether_type;
}

header ipv4_h {
    bit<4> version;
    bit<4> ihl;
    bit<6> dscp;
    bit<2> ecn;
    bit<16> total_len;
    bit<16> identification;
    bit<3> flags;
    bit<13> frag_offset;
    bit<8> ttl;
    bit<8> protocol;
    bit<16> checksum;
    ipv4_addr_t src_ip;
    ipv4_addr_t dst_ip;
}

header udp_h {
    bit<16> src_port;
    bit<16> dst_port;
    bit<16> length;
    bit<16> checksum;
}

header ib_bth_h {
    bit<8> opcode;
    bit<8> flags;
    bit<16> partition_key;
    bit<8> reserved0;
    bit<24> destination_qp;
    bit<1> ack_request;
    bit<7> reserved1;
    bit<24> packet_seqnum;
}

header ib_reth_h {
    bit<32> virtual_address_hi;
    bit<32> virtual_address_lo;
    bit<32> rkey;
    bit<32> dma_length;
}

header sail_msh_h {
    bit<32> seq;
    bit<1> meta_valid;
    bit<1> meta_reth_valid;
    bit<6> padding;
}

header sail_meta_h {
    ipv4_addr_t meta_src_ip;
    ipv4_addr_t meta_dst_ip;
    bit<16> meta_src_port;
    bit<16> meta_dst_port;
    bit<24> meta_dqpn;
    bit<24> meta_psn;
    bit<8> meta_opcode;
    bit<16> meta_ip_len;
    bit<32> meta_virtual_address_hi;
    bit<32> meta_virtual_address_lo;
    bit<32> meta_rkey;
    bit<32> meta_dma_length;
}

header sail_queue_h {
    bit<32> magic;
    bit<8> queue_type;
    bit<8> padding;
    bit<16> budget;
    bit<32> msn;
    bit<32> queue_tag;
}

header sail_notify_seed_h {
    bit<32> magic;
    bit<8> type;
    bit<24> dqp;
    bit<24> psn;
    mac_addr_t dst_mac;
    mac_addr_t src_mac;
    ipv4_addr_t src_ip;
    ipv4_addr_t dst_ip;
}

header sail_notify_h {
    bit<32> magic;
    bit<8> type;
    bit<24> reserved;
    bit<32> dqp;
    bit<32> psn;
}

struct headers_t {
    sail_notify_seed_h notify_seed;
    ethernet_h ethernet;
    ipv4_h ipv4;
    udp_h udp;
    sail_queue_h queue;
    ib_bth_h infiniband;
    sail_msh_h msh;
    sail_meta_h meta;
    ib_reth_h reth;
    sail_notify_h notify;
}

struct metadata_t {
    bit<4> ingress_path;
    bit<32> notify_seed_magic;
    MirrorId_t notify_mirror_sid;

    bit<32> old_seen_seq;
    bit<32> expected_old_seq;
    bit<32> release_msn;
    bit<1> seq_next_needed;
    bit<2> seq_next_result;
    bit<2> release_path;
    bit<1> queue_forced_release;
    bit<32> queue_is_head;
    bit<1> complete_notify_candidate;
    bit<1> notify_needed;
    bit<8> notify_type;
    bit<24> notify_dqp;
    bit<24> notify_psn;
    mac_addr_t notify_dst_mac;
    mac_addr_t notify_src_mac;
    ipv4_addr_t notify_src_ip;
    ipv4_addr_t notify_dst_ip;
}

struct egress_metadata_t {
    bit<1> sender_process;
    bit<32> cache_dqpn_opcode;
    bit<32> cache_ports;
    bit<32> cache_reth_va_hi;
    bit<32> cache_reth_va_lo;
    bit<32> cache_reth_rkey;
    bit<32> cache_reth_dma_length;

    bit<32> old_dqpn_opcode;
    ipv4_addr_t old_src_ip;
    ipv4_addr_t old_dst_ip;
    bit<32> old_ports;
    bit<32> old_psn;
    bit<16> old_ip_len;
    bit<32> old_reth_va_hi;
    bit<32> old_reth_va_lo;
    bit<32> old_reth_rkey;
    bit<32> old_reth_dma_length;
}

parser SwitchIngressParser(
        packet_in pkt,
        out headers_t hdr,
        out metadata_t ig_md,
        out ingress_intrinsic_metadata_t ig_intr_md) {
    state start {
        pkt.extract(ig_intr_md);
        transition parse_port_metadata;
    }

    state parse_port_metadata {
        pkt.advance(PORT_METADATA_SIZE);
        transition select(ig_intr_md.ingress_port) {
            PORT_NOTIFY_LOOP: parse_notify_seed;
            default: parse_ethernet;
        }
    }

    state parse_notify_seed {
        pkt.extract(hdr.notify_seed);
        transition accept;
    }

    state parse_ethernet {
        pkt.extract(hdr.ethernet);
        transition select(hdr.ethernet.ether_type) {
            ETHERTYPE_IPV4: parse_ipv4;
            default: accept;
        }
    }

    state parse_ipv4 {
        pkt.extract(hdr.ipv4);
        transition select(hdr.ipv4.protocol) {
            IP_PROTOCOL_UDP: parse_udp;
            default: accept;
        }
    }

    state parse_udp {
        pkt.extract(hdr.udp);
        transition select(ig_intr_md.ingress_port, hdr.udp.dst_port) {
            (PORT_QUEUE_LOOP, ROCE_V2): parse_queue_or_bth;
            (PORT_QUEUE_LOOP, ROCE_V2_WAN): parse_queue_or_bth;
            (_, ROCE_V2): parse_bth;
            (_, ROCE_V2_WAN): parse_bth;
            default: accept;
        }
    }

    state parse_queue_or_bth {
        bit<32> prefix = pkt.lookahead<bit<32>>();
        transition select(prefix) {
            SAIL_QUEUE_MAGIC: parse_queue;
            default: parse_bth;
        }
    }

    state parse_queue {
        pkt.extract(hdr.queue);
        transition parse_bth;
    }

    state parse_bth {
        pkt.extract(hdr.infiniband);
        transition select(hdr.infiniband.reserved0) {
            1: parse_msh;
            default: parse_optional_reth;
        }
    }

    state parse_msh {
        pkt.extract(hdr.msh);
        transition parse_sail_meta;
    }

    state parse_sail_meta {
        pkt.extract(hdr.meta);
        transition parse_optional_reth;
    }

    state parse_optional_reth {
        transition select(hdr.infiniband.opcode) {
            RC_WRITE_FIRST: parse_reth;
            RC_WRITE_ONLY: parse_reth;
            default: accept;
        }
    }

    state parse_reth {
        pkt.extract(hdr.reth);
        transition accept;
    }
}

control SwitchIngressDeparser(
        packet_out pkt,
        inout headers_t hdr,
        in metadata_t ig_md,
        in ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md) {
    Mirror() mirror;
    Checksum() ipv4_checksum;

    apply {
        if (ig_dprsr_md.mirror_type == NOTIFY_MIRROR_TYPE) {
            mirror.emit<sail_notify_seed_h>(
                ig_md.notify_mirror_sid,
                {
                    ig_md.notify_seed_magic,
                    ig_md.notify_type,
                    ig_md.notify_dqp,
                    ig_md.notify_psn,
                    ig_md.notify_dst_mac,
                    ig_md.notify_src_mac,
                    ig_md.notify_src_ip,
                    ig_md.notify_dst_ip
                });
        }

        if (hdr.ipv4.isValid()) {
            hdr.ipv4.checksum = ipv4_checksum.update({
                hdr.ipv4.version,
                hdr.ipv4.ihl,
                hdr.ipv4.dscp,
                hdr.ipv4.ecn,
                hdr.ipv4.total_len,
                hdr.ipv4.identification,
                hdr.ipv4.flags,
                hdr.ipv4.frag_offset,
                hdr.ipv4.ttl,
                hdr.ipv4.protocol,
                hdr.ipv4.src_ip,
                hdr.ipv4.dst_ip
            }, false);
        }
        pkt.emit(hdr);
    }
}

control SwitchIngress(
        inout headers_t hdr,
        inout metadata_t ig_md,
        in ingress_intrinsic_metadata_t ig_intr_md,
        in ingress_intrinsic_metadata_from_parser_t ig_prsr_md,
        inout ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md,
        inout ingress_intrinsic_metadata_for_tm_t ig_tm_md) {

    Register<bit<32>, bit<8>>(STEP, 0x7fffffff) recv_seen_seq_reg;
    RegisterAction<bit<32>, bit<8>, bit<32>>(recv_seen_seq_reg) update_seen_seq = {
        void apply(inout bit<32> val, out bit<32> old) {
            old = val;
            val = hdr.msh.seq;
        }
    };

    Register<bit<32>, bit<8>>(1, 0) seq_next_reg;
    RegisterAction<bit<32>, bit<8>, bit<2>>(seq_next_reg)
            process_seq_next = {
        void apply(inout bit<32> val, out bit<2> result) {
            result = 0;
            if (ig_md.queue_forced_release == 1) {
                val = ig_md.release_msn + 1;
                result = 3;
            } else if (ig_md.release_msn == val) {
                val = val + 1;
                result = 1;
            }
        }
    };

    Register<bit<32>, bit<8>>(1, 0) pkt_tail_tag_reg;
    RegisterAction<bit<32>, bit<8>, bit<32>>(pkt_tail_tag_reg)
            alloc_pkt_queue_tag = {
        void apply(inout bit<32> val, out bit<32> tag) {
            tag = val;
            val = val + 1;
        }
    };

    Register<bit<32>, bit<8>>(1, 0) dummy_tail_tag_reg;
    RegisterAction<bit<32>, bit<8>, bit<32>>(dummy_tail_tag_reg)
            alloc_dummy_queue_tag = {
        void apply(inout bit<32> val, out bit<32> tag) {
            tag = val;
            val = val + 1;
        }
    };

    Register<bit<32>, bit<8>>(1, 0) pkt_head_tag_reg;
    RegisterAction<bit<32>, bit<8>, bit<32>>(pkt_head_tag_reg)
            check_pkt_queue_head = {
        void apply(inout bit<32> val, out bit<32> is_head) {
            is_head = 0;
            if (hdr.queue.queue_tag == val) {
                is_head = 1;
            }
        }
    };
    RegisterAction<bit<32>, bit<8>, bit<32>>(pkt_head_tag_reg)
            advance_pkt_queue_head = {
        void apply(inout bit<32> val, out bit<32> advanced) {
            val = val + 1;
            advanced = 1;
        }
    };

    Register<bit<32>, bit<8>>(1, 0) dummy_head_tag_reg;
    RegisterAction<bit<32>, bit<8>, bit<32>>(dummy_head_tag_reg)
            check_dummy_queue_head = {
        void apply(inout bit<32> val, out bit<32> is_head) {
            is_head = 0;
            if (hdr.queue.queue_tag == val) {
                is_head = 1;
            }
        }
    };
    RegisterAction<bit<32>, bit<8>, bit<32>>(dummy_head_tag_reg)
            advance_dummy_queue_head = {
        void apply(inout bit<32> val, out bit<32> advanced) {
            val = val + 1;
            advanced = 1;
        }
    };

    action count_msn() {}
    action count_loss() {}
    action count_dummy() {}
    action count_pkt_enqueue() {}
    action count_dummy_enqueue() {}
    action count_fast_release() {}
    action count_recir_release() {}
    action count_degrade() {}
    action count_timeout() {}
    action count_loss_notify() {}
    action count_complete_notify() {}

    action set_ingress_path(bit<4> path) {
        ig_md.ingress_path = path;
    }

    table ingress_path_classifier {
        key = {
            ig_intr_md.ingress_port: exact;
            hdr.notify_seed.isValid(): exact;
            hdr.queue.isValid(): exact;
            hdr.msh.isValid(): exact;
        }
        actions = {
            set_ingress_path;
        }
        const entries = {
            (PORT_NOTIFY_LOOP, true, false, false): set_ingress_path(PATH_NOTIFY);
            (PORT_QUEUE_LOOP, false, false, true): set_ingress_path(PATH_BUILD_DUMMY);
            (PORT_QUEUE_LOOP, false, true, true): set_ingress_path(PATH_QUEUE);
            (PORT_QUEUE_LOOP, false, true, false): set_ingress_path(PATH_QUEUE);
            (PORT_SENDER, false, false, false): set_ingress_path(PATH_SENDER);
            (PORT_WAN_TO_SENDER, false, false, true): set_ingress_path(PATH_RDCI);
            (PORT_WAN_TO_SENDER, false, false, false): set_ingress_path(PATH_RDCI_BYPASS);
            (PORT_RECEIVER, false, false, false): set_ingress_path(PATH_RECEIVER);
            (PORT_WAN_TO_RECEIVER, false, false, false): set_ingress_path(PATH_RETURN_WAN);
        }
        const default_action = set_ingress_path(PATH_OTHER);
        size = 9;
    }

    action set_egress(PortId_t port) {
        ig_tm_md.ucast_egress_port = port;
    }

    table ingress_simple_forward {
        key = {
            ig_md.ingress_path: exact;
        }
        actions = {
            set_egress;
            NoAction;
        }
        const entries = {
            PATH_SENDER: set_egress(PORT_WAN_TO_RECEIVER);
            PATH_RDCI_BYPASS: set_egress(PORT_RECEIVER);
            PATH_RECEIVER: set_egress(PORT_WAN_TO_SENDER);
            PATH_RETURN_WAN: set_egress(PORT_SENDER);
        }
        const default_action = NoAction();
        size = 4;
    }

    action drop_packet() {
        ig_dprsr_md.drop_ctl = 1;
    }

    table cnp_drop_dispatch {
        key = {
            hdr.infiniband.isValid(): exact;
            hdr.infiniband.opcode: exact;
        }
        actions = {
            drop_packet;
            NoAction;
        }
        const entries = {
            (true, RC_CNP): drop_packet();
        }
        const default_action = NoAction();
        size = 1;
    }

    action receiver_update_history() {
        bit<8> seen_idx = (bit<8>) hdr.msh.seq[2:0];
        ig_md.old_seen_seq = update_seen_seq.execute(seen_idx);
    }

    action receiver_prepare_expected() {
        ig_md.expected_old_seq = hdr.msh.seq - STEP;
    }

    table receiver_history {
        actions = {
            receiver_update_history;
        }
        const default_action = receiver_update_history();
        size = 1;
    }

    action prepare_receiver_release() {
        ig_md.release_msn = hdr.msh.seq;
        ig_md.seq_next_needed = 1;
        ig_md.release_path = RELEASE_RECEIVER;
    }

    table receiver_prepare_release {
        actions = {
            prepare_receiver_release;
        }
        const default_action = prepare_receiver_release();
        size = 1;
    }

    action run_seq_next_process() {
        ig_md.seq_next_result = process_seq_next.execute(0);
    }

    table seq_next_process {
        actions = {
            run_seq_next_process;
        }
        const default_action = run_seq_next_process();
        size = 1;
    }

    table seq_next_maybe_process {
        key = {
            ig_md.seq_next_needed: exact;
        }
        actions = {
            run_seq_next_process;
            NoAction;
        }
        const entries = {
            1: run_seq_next_process();
        }
        const default_action = NoAction();
        size = 1;
    }

    action prepare_queue_release() {
        ig_md.release_msn = hdr.queue.msn;
        ig_md.seq_next_needed = 1;
        ig_md.release_path = RELEASE_QUEUE;
    }

    table queue_prepare_release {
        actions = {
            prepare_queue_release;
        }
        const default_action = prepare_queue_release();
        size = 1;
    }

    action check_pkt_head() {
        ig_md.queue_is_head = check_pkt_queue_head.execute(0);
    }

    table pkt_queue_head_check {
        key = {
            hdr.queue.queue_type: exact;
        }
        actions = {
            check_pkt_head;
            NoAction;
        }
        const entries = {
            SAIL_QUEUE_PKT: check_pkt_head();
        }
        const default_action = NoAction();
        size = 1;
    }

    action check_dummy_head() {
        ig_md.queue_is_head = check_dummy_queue_head.execute(0);
    }

    table dummy_queue_head_check {
        key = {
            hdr.queue.queue_type: exact;
        }
        actions = {
            check_dummy_head;
            NoAction;
        }
        const entries = {
            SAIL_QUEUE_DUMMY: check_dummy_head();
        }
        const default_action = NoAction();
        size = 1;
    }

    action advance_pkt_head() {
        ig_md.queue_is_head = advance_pkt_queue_head.execute(0);
    }

    table pkt_queue_head_advance {
        key = {
            hdr.queue.queue_type: exact;
        }
        actions = {
            advance_pkt_head;
            NoAction;
        }
        const entries = {
            SAIL_QUEUE_PKT: advance_pkt_head();
        }
        const default_action = NoAction();
        size = 1;
    }

    action advance_dummy_head() {
        ig_md.queue_is_head = advance_dummy_queue_head.execute(0);
    }

    table dummy_queue_head_advance {
        key = {
            hdr.queue.queue_type: exact;
        }
        actions = {
            advance_dummy_head;
            NoAction;
        }
        const entries = {
            SAIL_QUEUE_DUMMY: advance_dummy_head();
        }
        const default_action = NoAction();
        size = 1;
    }

    action request_dummy_clone() {
        ig_tm_md.mcast_grp_a = DUMMY_MC_GID;
    }

    action prepare_notify(bit<8> notify_type) {
        ig_md.notify_needed = 1;
        ig_md.notify_type = notify_type;
        ig_md.notify_dqp = hdr.infiniband.destination_qp;
        ig_md.notify_psn = hdr.infiniband.packet_seqnum;
        ig_md.notify_dst_mac = hdr.ethernet.dst_mac;
        ig_md.notify_src_mac = hdr.ethernet.src_mac;
        ig_md.notify_src_ip = hdr.ipv4.src_ip;
        ig_md.notify_dst_ip = hdr.ipv4.dst_ip;
    }

    action emit_notify() {
        ig_dprsr_md.mirror_type = NOTIFY_MIRROR_TYPE;
        ig_md.notify_seed_magic = SAIL_NOTIFY_SEED_MAGIC;
        ig_md.notify_mirror_sid = NOTIFY_MIRROR_SID;
    }

    action prepare_complete_notify() {
        ig_md.notify_needed = 1;
        ig_md.notify_type = SAIL_NOTIFY_COMPLETE;
        ig_md.notify_dqp = hdr.infiniband.destination_qp;
        ig_md.notify_psn = hdr.infiniband.packet_seqnum;
        ig_md.notify_dst_mac = hdr.ethernet.dst_mac;
        ig_md.notify_src_mac = hdr.ethernet.src_mac;
        ig_md.notify_src_ip = hdr.ipv4.src_ip;
        ig_md.notify_dst_ip = hdr.ipv4.dst_ip;
        count_complete_notify();
    }

    table complete_notify_dispatch {
        key = {
            ig_md.complete_notify_candidate: exact;
            hdr.infiniband.isValid(): exact;
            hdr.infiniband.opcode: exact;
        }
        actions = {
            prepare_complete_notify;
            NoAction;
        }
        const entries = {
            (1, true, RC_WRITE_LAST): prepare_complete_notify();
            (1, true, RC_WRITE_ONLY): prepare_complete_notify();
        }
        const default_action = NoAction();
        size = 2;
    }

    table notify_emit_dispatch {
        key = {
            ig_md.notify_needed: exact;
        }
        actions = {
            emit_notify;
            NoAction;
        }
        const entries = {
            1: emit_notify();
        }
        const default_action = NoAction();
        size = 1;
    }

    action enqueue_packet() {
        hdr.queue.setValid();
        hdr.queue.magic = SAIL_QUEUE_MAGIC;
        hdr.queue.queue_type = SAIL_QUEUE_PKT;
        hdr.queue.padding = SAIL_QUEUE_WAIT_HEAD;
        hdr.queue.budget = QUEUE_BUDGET;
        hdr.queue.msn = hdr.msh.seq;
        hdr.queue.queue_tag = alloc_pkt_queue_tag.execute(0);
        hdr.ipv4.total_len = hdr.ipv4.total_len + QUEUE_SHIM_BYTES;
        hdr.udp.length = hdr.udp.length + QUEUE_SHIM_BYTES;
        ig_tm_md.ucast_egress_port = PORT_QUEUE_LOOP;
    }

    action enqueue_dummy() {
        hdr.queue.setValid();
        hdr.queue.magic = SAIL_QUEUE_MAGIC;
        hdr.queue.queue_type = SAIL_QUEUE_DUMMY;
        hdr.queue.padding = SAIL_QUEUE_WAIT_HEAD;
        hdr.queue.budget = QUEUE_BUDGET;
        hdr.queue.msn = hdr.msh.seq;
        hdr.queue.queue_tag = alloc_dummy_queue_tag.execute(0);
        hdr.ipv4.total_len = hdr.ipv4.total_len + QUEUE_SHIM_BYTES;
        hdr.udp.length = hdr.udp.length + QUEUE_SHIM_BYTES;
        ig_tm_md.ucast_egress_port = PORT_QUEUE_LOOP;
    }

    action build_dummy() {
        hdr.ipv4.src_ip = hdr.meta.meta_src_ip;
        hdr.ipv4.dst_ip = hdr.meta.meta_dst_ip;
        hdr.udp.src_port = hdr.meta.meta_src_port;
        hdr.udp.dst_port = hdr.meta.meta_dst_port;
        hdr.infiniband.opcode = hdr.meta.meta_opcode;
        hdr.infiniband.reserved0 = 1;
        hdr.infiniband.destination_qp = hdr.meta.meta_dqpn;
        hdr.infiniband.packet_seqnum = hdr.meta.meta_psn;
        hdr.ipv4.total_len = hdr.meta.meta_ip_len + MSH_BYTES;
        hdr.udp.length = hdr.meta.meta_ip_len + MSH_BYTES_SUB_IP;
        hdr.msh.seq = hdr.msh.seq - STEP;
        hdr.msh.meta_valid = 0;
    }

    action clear_dummy_reth() {
        hdr.reth.setInvalid();
    }

    action restore_dummy_reth() {
        hdr.reth.setValid();
        hdr.reth.virtual_address_hi = hdr.meta.meta_virtual_address_hi;
        hdr.reth.virtual_address_lo = hdr.meta.meta_virtual_address_lo;
        hdr.reth.rkey = hdr.meta.meta_rkey;
        hdr.reth.dma_length = hdr.meta.meta_dma_length;
    }

    action build_compact_notify() {
        hdr.ethernet.setValid();
        hdr.ipv4.setValid();
        hdr.udp.setValid();
        hdr.notify.setValid();
        hdr.ethernet.dst_mac = hdr.notify_seed.dst_mac;
        hdr.ethernet.src_mac = hdr.notify_seed.src_mac;
        hdr.ethernet.ether_type = ETHERTYPE_IPV4;
        hdr.ipv4.version = 4;
        hdr.ipv4.ihl = 5;
        hdr.ipv4.dscp = 0;
        hdr.ipv4.ecn = 0;
        hdr.ipv4.total_len = 44;
        hdr.ipv4.identification = 0;
        hdr.ipv4.flags = 0;
        hdr.ipv4.frag_offset = 0;
        hdr.ipv4.ttl = 64;
        hdr.ipv4.protocol = IP_PROTOCOL_UDP;
        hdr.ipv4.src_ip = hdr.notify_seed.src_ip;
        hdr.ipv4.dst_ip = hdr.notify_seed.dst_ip;
        hdr.udp.src_port = SAIL_NOTIFY_PORT;
        hdr.udp.dst_port = SAIL_NOTIFY_PORT;
        hdr.udp.length = 24;
        hdr.udp.checksum = 0;
        hdr.notify.magic = SAIL_NOTIFY_MAGIC;
        hdr.notify.type = hdr.notify_seed.type;
        hdr.notify.reserved = 0;
        hdr.notify.dqp = (bit<32>) hdr.notify_seed.dqp;
        hdr.notify.psn = (bit<32>) hdr.notify_seed.psn;
        hdr.notify_seed.setInvalid();
        ig_tm_md.ucast_egress_port = PORT_RECEIVER;
    }

    action queue_degrade_and_advance() {
        count_degrade();
        hdr.queue.padding = SAIL_QUEUE_HEAD_ADVANCE;
        ig_tm_md.ucast_egress_port = PORT_QUEUE_LOOP;
    }

    action queue_release_advance() {
        hdr.queue.padding = SAIL_QUEUE_HEAD_ADVANCE;
        ig_tm_md.ucast_egress_port = PORT_QUEUE_LOOP;
    }

    action receiver_fast_release() {
        ig_md.complete_notify_candidate = 1;
        ig_tm_md.ucast_egress_port = PORT_RECEIVER;
        count_fast_release();
    }

    table release_success_dispatch {
        key = {
            ig_md.seq_next_needed: exact;
            ig_md.seq_next_result: exact;
            ig_md.release_path: exact;
        }
        actions = {
            queue_degrade_and_advance;
            queue_release_advance;
            receiver_fast_release;
            NoAction;
        }
        const entries = {
            (1, 3, RELEASE_QUEUE): queue_degrade_and_advance();
            (1, 1, RELEASE_QUEUE): queue_release_advance();
            (1, 3, RELEASE_RECEIVER): receiver_fast_release();
            (1, 1, RELEASE_RECEIVER): receiver_fast_release();
        }
        const default_action = NoAction();
        size = 4;
    }

    action reset_ingress_metadata() {
        ig_md.ingress_path = PATH_OTHER;
        ig_md.old_seen_seq = 0;
        ig_md.expected_old_seq = 0;
        ig_md.release_msn = 0;
        ig_md.seq_next_needed = 0;
        ig_md.seq_next_result = 0;
        ig_md.release_path = 0;
        ig_md.queue_forced_release = 0;
        ig_md.queue_is_head = 0;
        ig_md.complete_notify_candidate = 0;
        ig_md.notify_needed = 0;
        ig_md.notify_type = 0;
        ig_md.notify_dqp = 0;
        ig_md.notify_psn = 0;
        ig_md.notify_dst_mac = 0;
        ig_md.notify_src_mac = 0;
        ig_md.notify_src_ip = 0;
        ig_md.notify_dst_ip = 0;
        ig_md.notify_seed_magic = 0;
        ig_md.notify_mirror_sid = (MirrorId_t)0;

        ig_dprsr_md.mirror_type = 0;
        ig_dprsr_md.drop_ctl = 0;
        ig_tm_md.mcast_grp_a = 0;
    }

    apply {
        reset_ingress_metadata();

        ingress_path_classifier.apply();
        ingress_simple_forward.apply();

        if (ig_md.ingress_path == PATH_NOTIFY) {
            build_compact_notify();
            exit;
        } else if (ig_md.ingress_path == PATH_BUILD_DUMMY) {
            if (hdr.msh.meta_reth_valid == 1) {
                restore_dummy_reth();
            } else {
                clear_dummy_reth();
            }
            build_dummy();
            prepare_notify(SAIL_NOTIFY_LOSS);
            count_loss_notify();
            count_dummy();
            enqueue_dummy();
            count_dummy_enqueue();
        } else if (ig_md.ingress_path == PATH_QUEUE) {
            ig_tm_md.ucast_egress_port = PORT_QUEUE_LOOP;
            if (hdr.queue.padding == SAIL_QUEUE_WAIT_HEAD) {
                pkt_queue_head_check.apply();
                dummy_queue_head_check.apply();
                if (ig_md.queue_is_head == 1) {
                    hdr.queue.padding = SAIL_QUEUE_HEAD_READY;
                } else if (hdr.queue.budget > 0) {
                    hdr.queue.budget = hdr.queue.budget - 1;
                }
            } else if (hdr.queue.padding == SAIL_QUEUE_HEAD_READY) {
                queue_prepare_release.apply();
                if (hdr.queue.budget == 0) {
                    ig_md.queue_forced_release = 1;
                    count_timeout();
                }
            } else if (hdr.queue.padding == SAIL_QUEUE_HEAD_ADVANCE) {
                pkt_queue_head_advance.apply();
                dummy_queue_head_advance.apply();
                if (ig_md.queue_is_head == 1) {
                    ig_md.complete_notify_candidate = 1;
                    ig_tm_md.ucast_egress_port = PORT_RECEIVER;
                    count_recir_release();
                } else {
                    hdr.queue.padding = SAIL_QUEUE_WAIT_HEAD;
                }
            }
        } else if (ig_md.ingress_path == PATH_RDCI) {
            receiver_history.apply();
            if (hdr.msh.seq >= STEP) {
                receiver_prepare_expected();
                if (hdr.msh.meta_valid == 1) {
                    if (ig_md.old_seen_seq != ig_md.expected_old_seq) {
                        request_dummy_clone();
                        count_loss();
                    }
                }
            }
            receiver_prepare_release.apply();
        }

        cnp_drop_dispatch.apply();
        seq_next_maybe_process.apply();
        release_success_dispatch.apply();
        if (ig_md.seq_next_needed == 1 &&
            ig_md.seq_next_result == 0 &&
            ig_md.release_path == RELEASE_QUEUE &&
            hdr.queue.budget > 0) {
                hdr.queue.budget = hdr.queue.budget - 1;
        } else if (ig_md.seq_next_needed == 1 &&
                   ig_md.seq_next_result == 0 &&
                   ig_md.release_path == RELEASE_RECEIVER) {
                enqueue_packet();
                count_pkt_enqueue();
        }

        complete_notify_dispatch.apply();
        notify_emit_dispatch.apply();
    }
}

parser SwitchEgressParser(
        packet_in pkt,
        out headers_t hdr,
        out egress_metadata_t eg_md,
        out egress_intrinsic_metadata_t eg_intr_md) {
    state start {
        pkt.extract(eg_intr_md);
        transition select(eg_intr_md.egress_port) {
            PORT_NOTIFY_LOOP: parse_notify_seed;
            default: parse_ethernet;
        }
    }

    state parse_notify_seed {
        pkt.extract(hdr.notify_seed);
        transition accept;
    }

    state parse_ethernet {
        pkt.extract(hdr.ethernet);
        transition select(hdr.ethernet.ether_type) {
            ETHERTYPE_IPV4: parse_ipv4;
            default: accept;
        }
    }

    state parse_ipv4 {
        pkt.extract(hdr.ipv4);
        transition select(hdr.ipv4.protocol) {
            IP_PROTOCOL_UDP: parse_udp;
            default: accept;
        }
    }

    state parse_udp {
        pkt.extract(hdr.udp);
        transition select(hdr.udp.dst_port) {
            ROCE_V2: parse_queue_or_bth;
            ROCE_V2_WAN: parse_queue_or_bth;
            default: accept;
        }
    }

    state parse_queue_or_bth {
        bit<32> prefix = pkt.lookahead<bit<32>>();
        transition select(prefix) {
            SAIL_QUEUE_MAGIC: parse_queue;
            default: parse_bth;
        }
    }

    state parse_queue {
        pkt.extract(hdr.queue);
        transition parse_bth;
    }

    state parse_bth {
        pkt.extract(hdr.infiniband);
        transition select(hdr.infiniband.reserved0) {
            1: parse_msh;
            default: parse_optional_reth;
        }
    }

    state parse_msh {
        pkt.extract(hdr.msh);
        transition parse_sail_meta;
    }

    state parse_sail_meta {
        pkt.extract(hdr.meta);
        transition parse_optional_reth;
    }

    state parse_optional_reth {
        transition select(hdr.infiniband.opcode) {
            RC_WRITE_FIRST: parse_reth;
            RC_WRITE_ONLY: parse_reth;
            default: accept;
        }
    }

    state parse_reth {
        pkt.extract(hdr.reth);
        transition accept;
    }
}

control SwitchEgress(
        inout headers_t hdr,
        inout egress_metadata_t eg_md,
        in egress_intrinsic_metadata_t eg_intr_md,
        in egress_intrinsic_metadata_from_parser_t eg_prsr_md,
        inout egress_intrinsic_metadata_for_deparser_t eg_dprsr_md,
        inout egress_intrinsic_metadata_for_output_port_t eg_oport_md) {

    #define EGRESS_CACHE_REGISTER(name, type, field) \
        Register<type, bit<8>>(STEP) name##_cache_reg; \
        RegisterAction<type, bit<8>, type>(name##_cache_reg) name##_cache_swap = { \
            void apply(inout type val, out type old) { \
                old = val; \
                val = (type) (field); \
            } \
        }

    EGRESS_CACHE_REGISTER(dqpn_opcode, bit<32>, eg_md.cache_dqpn_opcode);
    EGRESS_CACHE_REGISTER(src_ip, bit<32>, hdr.ipv4.src_ip);
    EGRESS_CACHE_REGISTER(dst_ip, bit<32>, hdr.ipv4.dst_ip);
    EGRESS_CACHE_REGISTER(ports, bit<32>, eg_md.cache_ports);
    EGRESS_CACHE_REGISTER(psn, bit<32>, hdr.infiniband.packet_seqnum);
    EGRESS_CACHE_REGISTER(ip_len, bit<16>, hdr.ipv4.total_len);
    EGRESS_CACHE_REGISTER(reth_va_hi, bit<32>, eg_md.cache_reth_va_hi);
    EGRESS_CACHE_REGISTER(reth_va_lo, bit<32>, eg_md.cache_reth_va_lo);
    EGRESS_CACHE_REGISTER(reth_rkey, bit<32>, eg_md.cache_reth_rkey);
    EGRESS_CACHE_REGISTER(reth_dma_length, bit<32>, eg_md.cache_reth_dma_length);

    Register<bit<32>, bit<8>>(1, 0) sender_msn_reg;
    RegisterAction<bit<32>, bit<8>, bit<32>>(sender_msn_reg)
            sender_next_msn = {
        void apply(inout bit<32> val, out bit<32> seq) {
            seq = val;
            val = val + 1;
        }
    };

    action sender_allocate_msn() {
        hdr.msh.setValid();
        hdr.meta.setValid();
        hdr.infiniband.reserved0 = 1;
        hdr.msh.seq = sender_next_msn.execute(0);
    }

    table sender_allocate_msn_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_allocate_msn;
            NoAction;
        }
        const entries = {
            1: sender_allocate_msn();
        }
        const default_action = NoAction();
        size = 1;
    }

    action mark_sender_process() {
        eg_md.sender_process = 1;
    }

    table sender_process_classifier {
        key = {
            eg_intr_md.egress_port: exact;
            hdr.infiniband.isValid(): exact;
            hdr.msh.isValid(): exact;
            hdr.infiniband.opcode: exact;
        }
        actions = {
            mark_sender_process;
            NoAction;
        }
        const entries = {
            (PORT_WAN_TO_RECEIVER, true, false, RC_WRITE_FIRST): mark_sender_process();
            (PORT_WAN_TO_RECEIVER, true, false, RC_WRITE_MIDDLE): mark_sender_process();
            (PORT_WAN_TO_RECEIVER, true, false, RC_WRITE_LAST): mark_sender_process();
            (PORT_WAN_TO_RECEIVER, true, false, RC_WRITE_ONLY): mark_sender_process();
        }
        const default_action = NoAction();
        size = 4;
    }

    action sender_prepare_cache() {
        eg_md.cache_dqpn_opcode =
            hdr.infiniband.destination_qp ++ hdr.infiniband.opcode;
        eg_md.cache_ports = hdr.udp.src_port ++ hdr.udp.dst_port;
        eg_md.cache_reth_va_hi = 0;
        eg_md.cache_reth_va_lo = 0;
        eg_md.cache_reth_rkey = 0;
        eg_md.cache_reth_dma_length = 0;
    }

    action sender_prepare_reth_cache() {
        eg_md.cache_reth_va_hi = hdr.reth.virtual_address_hi;
        eg_md.cache_reth_va_lo = hdr.reth.virtual_address_lo;
        eg_md.cache_reth_rkey = hdr.reth.rkey;
        eg_md.cache_reth_dma_length = hdr.reth.dma_length;
    }

    table sender_prepare_cache_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_prepare_cache;
            NoAction;
        }
        const entries = {
            1: sender_prepare_cache();
        }
        const default_action = NoAction();
        size = 1;
    }

    table sender_prepare_reth_cache_tbl {
        key = {
            eg_md.sender_process: exact;
            hdr.reth.isValid(): exact;
        }
        actions = {
            sender_prepare_reth_cache;
            NoAction;
        }
        const entries = {
            (1, true): sender_prepare_reth_cache();
        }
        const default_action = NoAction();
        size = 1;
    }

    action sender_swap_dqpn_opcode() {
        bit<8> idx = (bit<8>) hdr.msh.seq[2:0];
        eg_md.old_dqpn_opcode = dqpn_opcode_cache_swap.execute(idx);
    }

    table sender_dqpn_opcode_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_swap_dqpn_opcode;
            NoAction;
        }
        const entries = {
            1: sender_swap_dqpn_opcode();
        }
        const default_action = NoAction();
        size = 1;
    }

    action sender_swap_src_ip() {
        bit<8> idx = (bit<8>) hdr.msh.seq[2:0];
        eg_md.old_src_ip = src_ip_cache_swap.execute(idx);
    }

    table sender_src_ip_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_swap_src_ip;
            NoAction;
        }
        const entries = {
            1: sender_swap_src_ip();
        }
        const default_action = NoAction();
        size = 1;
    }

    action sender_swap_dst_ip() {
        bit<8> idx = (bit<8>) hdr.msh.seq[2:0];
        eg_md.old_dst_ip = dst_ip_cache_swap.execute(idx);
    }

    table sender_dst_ip_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_swap_dst_ip;
            NoAction;
        }
        const entries = {
            1: sender_swap_dst_ip();
        }
        const default_action = NoAction();
        size = 1;
    }

    action sender_swap_ports() {
        bit<8> idx = (bit<8>) hdr.msh.seq[2:0];
        eg_md.old_ports = ports_cache_swap.execute(idx);
    }

    table sender_ports_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_swap_ports;
            NoAction;
        }
        const entries = {
            1: sender_swap_ports();
        }
        const default_action = NoAction();
        size = 1;
    }

    action sender_swap_psn() {
        bit<8> idx = (bit<8>) hdr.msh.seq[2:0];
        eg_md.old_psn = psn_cache_swap.execute(idx);
    }

    table sender_psn_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_swap_psn;
            NoAction;
        }
        const entries = {
            1: sender_swap_psn();
        }
        const default_action = NoAction();
        size = 1;
    }

    action sender_swap_length() {
        bit<8> idx = (bit<8>) hdr.msh.seq[2:0];
        eg_md.old_ip_len = ip_len_cache_swap.execute(idx);
    }

    table sender_length_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_swap_length;
            NoAction;
        }
        const entries = {
            1: sender_swap_length();
        }
        const default_action = NoAction();
        size = 1;
    }

    action sender_swap_reth_va_hi() {
        bit<8> idx = (bit<8>) hdr.msh.seq[2:0];
        eg_md.old_reth_va_hi = reth_va_hi_cache_swap.execute(idx);
    }

    table sender_reth_va_hi_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_swap_reth_va_hi;
            NoAction;
        }
        const entries = {
            1: sender_swap_reth_va_hi();
        }
        const default_action = NoAction();
        size = 1;
    }

    action sender_swap_reth_va_lo() {
        bit<8> idx = (bit<8>) hdr.msh.seq[2:0];
        eg_md.old_reth_va_lo = reth_va_lo_cache_swap.execute(idx);
    }

    table sender_reth_va_lo_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_swap_reth_va_lo;
            NoAction;
        }
        const entries = {
            1: sender_swap_reth_va_lo();
        }
        const default_action = NoAction();
        size = 1;
    }

    action sender_swap_reth_rkey() {
        bit<8> idx = (bit<8>) hdr.msh.seq[2:0];
        eg_md.old_reth_rkey = reth_rkey_cache_swap.execute(idx);
    }

    table sender_reth_rkey_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_swap_reth_rkey;
            NoAction;
        }
        const entries = {
            1: sender_swap_reth_rkey();
        }
        const default_action = NoAction();
        size = 1;
    }

    action sender_swap_reth_dma_length() {
        bit<8> idx = (bit<8>) hdr.msh.seq[2:0];
        eg_md.old_reth_dma_length = reth_dma_length_cache_swap.execute(idx);
    }

    table sender_reth_dma_length_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_swap_reth_dma_length;
            NoAction;
        }
        const entries = {
            1: sender_swap_reth_dma_length();
        }
        const default_action = NoAction();
        size = 1;
    }

    action sender_fill_meta_from_cache() {
        hdr.meta.meta_dqpn = eg_md.old_dqpn_opcode[31:8];
        hdr.meta.meta_opcode = eg_md.old_dqpn_opcode[7:0];
        hdr.meta.meta_src_ip = eg_md.old_src_ip;
        hdr.meta.meta_dst_ip = eg_md.old_dst_ip;
        hdr.meta.meta_src_port = eg_md.old_ports[31:16];
        hdr.meta.meta_dst_port = eg_md.old_ports[15:0];
        hdr.meta.meta_psn = eg_md.old_psn[23:0];
        hdr.meta.meta_ip_len = eg_md.old_ip_len;
        hdr.meta.meta_virtual_address_hi = eg_md.old_reth_va_hi;
        hdr.meta.meta_virtual_address_lo = eg_md.old_reth_va_lo;
        hdr.meta.meta_rkey = eg_md.old_reth_rkey;
        hdr.meta.meta_dma_length = eg_md.old_reth_dma_length;
        hdr.msh.meta_valid = 0;
    }

    action sender_extend_packet() {
        hdr.ipv4.total_len = hdr.ipv4.total_len + MSH_BYTES;
        hdr.udp.length = hdr.udp.length + MSH_BYTES;
    }

    action sender_mark_first_or_only() {
        hdr.msh.meta_valid = 1;
        hdr.msh.meta_reth_valid = 1;
    }

    action sender_mark_middle_or_last() {
        hdr.msh.meta_valid = 1;
        hdr.msh.meta_reth_valid = 0;
    }

    action sender_clear_msh_metadata() {
        hdr.msh.meta_valid = 0;
        hdr.msh.meta_reth_valid = 0;
    }

    table sender_fill_meta_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_fill_meta_from_cache;
            NoAction;
        }
        const entries = {
            1: sender_fill_meta_from_cache();
        }
        const default_action = NoAction();
        size = 1;
    }

    table sender_extend_packet_tbl {
        key = {
            eg_md.sender_process: exact;
        }
        actions = {
            sender_extend_packet;
            NoAction;
        }
        const entries = {
            1: sender_extend_packet();
        }
        const default_action = NoAction();
        size = 1;
    }

    table sender_mark_meta_tbl {
        key = {
            eg_md.sender_process: exact;
            hdr.meta.meta_opcode: exact;
        }
        actions = {
            sender_mark_first_or_only;
            sender_mark_middle_or_last;
            sender_clear_msh_metadata;
            NoAction;
        }
        const entries = {
            (1, RC_WRITE_FIRST): sender_mark_first_or_only();
            (1, RC_WRITE_ONLY): sender_mark_first_or_only();
            (1, RC_WRITE_MIDDLE): sender_mark_middle_or_last();
            (1, RC_WRITE_LAST): sender_mark_middle_or_last();
        }
        const default_action = NoAction();
        size = 4;
    }

    action rewrite_udp_dst(bit<16> dst_port) {
        hdr.udp.dst_port = dst_port;
    }

    table rewrite_udp_dst_tbl {
        key = {
            eg_intr_md.egress_port: exact;
            hdr.udp.isValid(): exact;
            hdr.udp.dst_port: exact;
        }
        actions = {
            rewrite_udp_dst;
            NoAction;
        }
        const entries = {
            (PORT_WAN_TO_RECEIVER, true, ROCE_V2): rewrite_udp_dst(ROCE_V2_WAN);
            (PORT_WAN_TO_SENDER, true, ROCE_V2): rewrite_udp_dst(ROCE_V2_WAN);
            (PORT_RECEIVER, true, ROCE_V2_WAN): rewrite_udp_dst(ROCE_V2);
            (PORT_SENDER, true, ROCE_V2_WAN): rewrite_udp_dst(ROCE_V2);
        }
        const default_action = NoAction();
        size = 4;
    }

    action strip_queue_shim() {
        hdr.queue.setInvalid();
        hdr.ipv4.total_len = hdr.ipv4.total_len - QUEUE_SHIM_BYTES;
        hdr.udp.length = hdr.udp.length - QUEUE_SHIM_BYTES;
    }

    table strip_queue_loop_tbl {
        key = {
            eg_intr_md.egress_port: exact;
            eg_intr_md.egress_rid: exact;
            hdr.queue.isValid(): exact;
        }
        actions = {
            strip_queue_shim;
            NoAction;
        }
        const entries = {
            (PORT_QUEUE_LOOP, DUMMY_MC_RID, true): strip_queue_shim();
        }
        const default_action = NoAction();
        size = 1;
    }

    table strip_receiver_queue_tbl {
        key = {
            eg_intr_md.egress_port: exact;
            hdr.queue.isValid(): exact;
        }
        actions = {
            strip_queue_shim;
            NoAction;
        }
        const entries = {
            (PORT_RECEIVER, true): strip_queue_shim();
        }
        const default_action = NoAction();
        size = 1;
    }

    action strip_msh() {
        hdr.msh.setInvalid();
        hdr.meta.setInvalid();
        hdr.infiniband.reserved0 = 0;
        hdr.ipv4.total_len = hdr.ipv4.total_len - MSH_BYTES;
        hdr.udp.length = hdr.udp.length - MSH_BYTES;
    }

    table strip_receiver_msh_tbl {
        key = {
            eg_intr_md.egress_port: exact;
            hdr.msh.isValid(): exact;
        }
        actions = {
            strip_msh;
            NoAction;
        }
        const entries = {
            (PORT_RECEIVER, true): strip_msh();
        }
        const default_action = NoAction();
        size = 1;
    }

    action reset_egress_metadata() {
        eg_md.sender_process = 0;
        eg_md.cache_dqpn_opcode = 0;
        eg_md.cache_ports = 0;
        eg_md.cache_reth_va_hi = 0;
        eg_md.cache_reth_va_lo = 0;
        eg_md.cache_reth_rkey = 0;
        eg_md.cache_reth_dma_length = 0;

        eg_md.old_dqpn_opcode = 0;
        eg_md.old_src_ip = 0;
        eg_md.old_dst_ip = 0;
        eg_md.old_ports = 0;
        eg_md.old_psn = 0;
        eg_md.old_ip_len = 0;
        eg_md.old_reth_va_hi = 0;
        eg_md.old_reth_va_lo = 0;
        eg_md.old_reth_rkey = 0;
        eg_md.old_reth_dma_length = 0;
    }

    apply {
        reset_egress_metadata();

        sender_process_classifier.apply();
        sender_allocate_msn_tbl.apply();
        sender_prepare_cache_tbl.apply();
        sender_prepare_reth_cache_tbl.apply();
        sender_dqpn_opcode_tbl.apply();
        sender_src_ip_tbl.apply();
        sender_dst_ip_tbl.apply();
        sender_ports_tbl.apply();
        sender_psn_tbl.apply();
        sender_length_tbl.apply();
        sender_reth_va_hi_tbl.apply();
        sender_reth_va_lo_tbl.apply();
        sender_reth_rkey_tbl.apply();
        sender_reth_dma_length_tbl.apply();
        sender_fill_meta_tbl.apply();
        sender_extend_packet_tbl.apply();
        sender_mark_meta_tbl.apply();
        rewrite_udp_dst_tbl.apply();
        strip_queue_loop_tbl.apply();
        strip_receiver_queue_tbl.apply();
        strip_receiver_msh_tbl.apply();
    }
}

control SwitchEgressDeparser(
        packet_out pkt,
        inout headers_t hdr,
        in egress_metadata_t eg_md,
        in egress_intrinsic_metadata_for_deparser_t eg_dprsr_md) {
    Checksum() ipv4_checksum;

    apply {
        if (hdr.ipv4.isValid()) {
            hdr.ipv4.checksum = ipv4_checksum.update({
                hdr.ipv4.version,
                hdr.ipv4.ihl,
                hdr.ipv4.dscp,
                hdr.ipv4.ecn,
                hdr.ipv4.total_len,
                hdr.ipv4.identification,
                hdr.ipv4.flags,
                hdr.ipv4.frag_offset,
                hdr.ipv4.ttl,
                hdr.ipv4.protocol,
                hdr.ipv4.src_ip,
                hdr.ipv4.dst_ip
            }, false);
        }
        pkt.emit(hdr);
    }
}

Pipeline(SwitchIngressParser(),
         SwitchIngress(),
         SwitchIngressDeparser(),
         SwitchEgressParser(),
         SwitchEgress(),
         SwitchEgressDeparser()) pipe;

Switch(pipe) main;
