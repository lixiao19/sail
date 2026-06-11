# RC Baseline

This directory contains a minimal RC-only RDMA WRITE benchmark. We use it as a
baseline for experiments that evaluate native RC behavior, including GBN-style
loss recovery and RNIC selective-retry configurations.

## Model

- Each flow sends one message.
- The sender posts one RC `RDMA_WRITE` for the whole message.
- Sender-side RC WRITE CQE is the message completion time used for FCT.
- The receiver does not observe per-message data completion. It allocates and
  registers memory, exchanges addr/rkey, and exits after the sender sends TCP
  `DONE`.

## Build

```bash
make
```

This builds:

```text
rc_sender
rc_receiver
```

## Example

Replace the device names, GID index, and IP address with your own RDMA testbed
configuration.

Receiver:

```bash
./rc_receiver \
  --dev <receiver-rdma-device> \
  --gid-index <gid-index> \
  --port 21515 \
  --flows 1 \
  --msg-size 65536
```

Sender:

```bash
./rc_sender \
  --server <receiver-ip> \
  --dev <sender-rdma-device> \
  --gid-index <gid-index> \
  --port 21515 \
  --flows 1 \
  --msg-size 65536 \
  --fct-file rc_fct.csv
```

## Traffic Files

Traffic files use:

```text
flow_id size_bytes start_delay_ms
```

`start_delay_ms` may be fractional, e.g., `0.5`.

If `--traffic-file` is set and `--flows` is omitted, the flow count is inferred
from the largest flow ID in the file.

## Synchronized Start

Multiple sender processes on the same host can be synchronized by passing the
same Unix epoch microsecond timestamp:

```bash
./rc_sender ... --start-at-us <unix-epoch-usec>
```

Flow start delays in the traffic file are relative to this timestamp.

## Output

The sender prints aggregate goodput, FCT percentiles, completion counts, and
CPU profiling counters. When `--fct-file` is set, it also writes per-flow start
time, end time, and FCT.

The receiver only reports basic receive-side runtime statistics because normal
RC WRITE does not generate receiver-side data completions.
