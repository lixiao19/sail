# SAIL Userspace Agent

This directory contains the userspace endpoint prototype used to evaluate SAIL.
It is intentionally independent from the older `sail_agent` code.

## Model

- Data path: the sender posts one RC `RDMA_WRITE` per flow/message.
- Notification path: the switch sends UDP notifications to the receiver. Each
  notification contains the notification type, destination QP, and PSN.
- Recovery path: the receiver sends UC `NACK` messages. The sender maps the
  lost PSN to a packet offset using the original WRITE context and issues a UC
  `RDMA_WRITE_WITH_IMM` repair.
- Completion path: the receiver sends UC `DONE_ACK` after it has received the
  completion notification and all recorded losses have been repaired. The sender
  replies with UC `DONE_ACK_ACK`.
- Sender completion: a flow is complete only after the sender has observed both
  its local RC WRITE CQE and the receiver's explicit `DONE_ACK`.

The sender also provides `--simulate-notify` for local debugging. In the normal
SAIL experiment, notifications come from the switch and this option should be
left disabled.

## Build

```bash
make
```

This builds:

```text
sail_sender
sail_receiver
```

## Example

Replace the device names, GID index, and IP address with your own RDMA testbed
configuration.

Receiver:

```bash
./sail_receiver \
  --dev <receiver-rdma-device> \
  --gid-index <gid-index> \
  --port 20515 \
  --notify-port 20516 \
  --flows 1 \
  --msg-size 1048576 \
  --pmtu 1024 \
  --udp-rcvbuf 268435456 \
  --udp-burst 512 \
  --nack-timeout-us 50000 \
  --done-timeout-us 50000
```

Sender:

```bash
./sail_sender \
  --server <receiver-ip> \
  --dev <sender-rdma-device> \
  --gid-index <gid-index> \
  --port 20515 \
  --notify-port 20516 \
  --flows 1 \
  --msg-size 1048576 \
  --pmtu 1024 \
  --fct-file fct.csv
```

For local debugging without a switch-generated notification path:

```bash
./sail_sender ... --simulate-notify --drop-every 10
```

## Traffic Files

Traffic files use:

```text
flow_id size_bytes start_delay_ms
```

If `--traffic-file` is set and `--flows` is omitted, the flow count is inferred
from the largest flow ID in the file.

## Synchronized Start

Multiple sender processes on the same host can share a start timestamp:

```bash
./sail_sender ... --start-at-us <unix-epoch-usec>
```

Flow start delays in the traffic file are relative to this timestamp.

## Output

The sender reports aggregate goodput, FCT percentiles, completion counts,
retransmission/control counts, and effective CPU profiling counters. When
`--fct-file` is set, it also writes per-flow start time, end time, and FCT.

The receiver reports delivered message counts, notification counts, NACK/repair
counts, and CPU profiling counters.

## Limitations

- The prototype supports one message per flow.
- The default PMTU is 1024 bytes.
- The userspace agent is for experimental evaluation. A production deployment
  would integrate the same completion and recovery logic into an RDMA provider
  or runtime shim.
