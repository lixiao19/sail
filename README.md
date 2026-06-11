# SAIL Prototype Code

This repository contains the research prototype for SAIL, a switch-assisted
loss recovery design for long-haul RDMA.

The code is organized into five components:

```text
open_source_release/
  sail-agent/
          End-host SAIL agent used by the experiments.
  sail-p4/
          Tofino 1 P4 data-plane prototype.
  sail-ns3/
          ns-3 based RoCE/RDMA simulator used for large-scale evaluation.
  rc_baseline/
          RC-only RDMA WRITE baseline used for GBN/RNIC-SR experiments.
  flor/   Simplified FLOR-style selective retransmission baseline.
```

## Components

### `sail-agent/`

The SAIL end-host agent is a userspace C prototype built on RDMA Verbs. The
sender uses RC RDMA WRITEs for data transfer. The receiver consumes
switch-generated UDP loss and completion notifications, sends UC NACKs, receives
UC repair WRITEs, and returns explicit completion acknowledgments.

```bash
cd sail-agent
make
```

### `sail-p4/`

The P4 program implements the SAIL data-plane logic on Tofino 1, including
macro-flow sequence assignment, delayed metadata piggybacking, loss detection,
dummy packet construction, compact UDP notifications, and ordered release
through a loopback-based recirculation path.

```bash
cd sail-p4
make
```

The P4 control-plane helper configures front-panel ports, loopback ports,
multicast groups, and mirror sessions used by the prototype.

### `sail-ns3/`

`sail-ns3/` contains the ns-3 based RoCE/RDMA simulator used for larger-scale
experiments. It includes SAIL, GBN, SR/IRN-style host recovery, and LR2-style
switch-assisted recovery modes, plus analysis scripts for FCT, bandwidth,
queue-length, rate, and CNP traces.

```bash
cd sail-ns3/simulation
./ns3 configure --enable-mtp --disable-examples
./ns3 build
```

### `flor/`

`flor/` contains a lightweight FLOR-inspired baseline for experiments. It uses
UC `WRITE_WITH_IMM` for chunk transmission and retransmission, plus a UC SEND
path for ACKs.

### `rc_baseline/`

`rc_baseline/` contains a minimal RC-only RDMA WRITE benchmark. The sender uses
local RC WRITE CQEs as flow completion times, while the receiver only exposes
registered memory and exits after TCP synchronization. We use this benchmark for
native GBN and RNIC-SR experiments.

## Traffic Files

The benchmark binaries accept traffic files with one flow per line:

```text
flow_id size_bytes start_delay_ms
```

`start_delay_ms` may be fractional. Flow IDs are positive integers.

## Notes

- This is a research prototype, not production networking software.
- The endpoint benchmarks assume Linux, libibverbs, and Mellanox/NVIDIA RDMA
  devices.
- The P4 program targets Tofino 1 and requires a compatible Barefoot/Intel SDE.
- The bundle intentionally excludes local traces, generated logs, private
  hostnames, credentials, and machine-specific orchestration scripts.
- Device names, IP addresses, port numbers, and Tofino front-panel ports in the
  README files are examples. Replace them with your own testbed configuration.
