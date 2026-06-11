# Simplified Flor Selective Retransmission Benchmark

This directory contains a small libibverbs benchmark for evaluating a Flor-like
software selective retransmission baseline.

The implementation is intentionally limited:

- Data path: UC `RDMA_WRITE_WITH_IMM`.
- Retransmission path: UC `RDMA_WRITE_WITH_IMM` for missing chunks.
- ACK path: a separate UC QP using `IBV_WR_SEND`.
- Unit of retransmission: fixed-size chunks.
- Completion rule: the receiver counts a message as complete after all chunks
  arrive. Payload fill and validation are intentionally disabled to avoid
  CPU-bound measurements.

It does not implement Flor's dynamic chunking, congestion control, RC backup QP,
or production RPC/API layer.

## Build

Build on a Linux RDMA host with `libibverbs` installed:

```bash
make
```

## Run

Start the receiver first:

```bash
./flor_receiver \
  --dev <rdma-device> \
  --gid-index 3 \
  --port 18515 \
  --flows 4 \
  --msg-size 1048576 \
  --chunk-size 4096 \
  --ack-batch 64 \
  --ack-timeout-us 1000
```

Start the sender:

```bash
./flor_sender \
  --server <receiver-ip> \
  --dev <rdma-device> \
  --gid-index 3 \
  --port 18515 \
  --flows 4 \
  --messages 1 \
  --msg-size 1048576 \
  --chunk-size 4096 \
  --sq-depth 2048 \
  --timeout-us 100000
```

Inject first-transmission software drops to test selective retransmission:

```bash
./flor_sender \
  --server <receiver-ip> \
  --dev <rdma-device> \
  --gid-index 3 \
  --port 18515 \
  --flows 1 \
  --messages 1 \
  --msg-size 1048576 \
  --chunk-size 4096 \
  --sq-depth 2048 \
  --timeout-us 100000 \
  --drop-every 10
```

`--drop-rate <p>` is also supported. Use only one of `--drop-rate` and
`--drop-every`.

To use per-flow sizes from a traffic file:

```bash
./flor_receiver \
  --dev <rdma-device> \
  --gid-index 3 \
  --port 18515 \
  --traffic-file traffic.txt \
  --chunk-size 4096

./flor_sender \
  --server <receiver-ip> \
  --dev <rdma-device> \
  --gid-index 3 \
  --port 18515 \
  --traffic-file traffic.txt \
  --chunk-size 4096 \
  --sq-depth 2048 \
  --timeout-us 100000 \
  --fct-file fct.csv \
  --goodput-window-start-us 200000 \
  --goodput-window-end-us 1000000
```

Multiple sender processes on the same sender host can start together with:

```bash
./flor_sender ... --start-at-us 1770000000000000
```

## Output

The sender prints CSV-style aggregate metrics:

```text
flows,messages,msg_size,chunk_size,total_bytes,goodput_gbps
avg_fct_us,p50_fct_us,p95_fct_us,p99_fct_us
tx_chunks,retrans_chunks,dropped_first_chunks,ack_pkts,fast_retrans_events,timeout_events
cpu_user_s,cpu_sys_s,cpu_percent
effective_cpu_s,effective_cpu_percent,poll_hit,poll_empty,cqe_count,post_send,post_recv
```

If `--fct-file <path>` is set, the sender also writes per-flow results:

```text
flow_id,msg_size_bytes,start_time_us,end_time_us,fct_us,start_delay_us
```

If `--goodput-window-start-us` and `--goodput-window-end-us` are set, the
sender also prints windowed goodput. Flow bytes are attributed to the window
proportionally to the overlap between the flow FCT interval and the window.

The receiver prints aggregate receive-side counters:

```text
receiver_flows,msg_size,chunk_size,completed_msgs,recv_chunks,ack_pkts,wall_s
cpu_user_s,cpu_sys_s,cpu_percent
effective_cpu_s,effective_cpu_percent,poll_hit,poll_empty,cqe_count,post_send,post_recv
```

`cpu_percent` is raw busy-polling CPU utilization. `effective_cpu_percent`
counts only non-empty CQ processing and WR posting work, excluding empty polls.

## Notes

- The benchmark supports up to 4096 flows and currently sends one message per
  flow. `--messages` must be `1`.
- `--traffic-file` uses lines formatted as `ID SIZE_BYTES START_DELAY_MS`.
  Receiver and sender use the size column to allocate each flow. The sender
  also uses `START_DELAY_MS` as the per-flow release time. If `--flows` is not
  specified, the number of flows is inferred from the traffic file.
- The sender posts up to the data QP send queue depth. The depth is controlled
  by `--sq-depth` and capped by the message chunk count, the RNIC `max_qp_wr`
  limit, and the receiver's posted immediate receive depth.
- The receiver batches ACKs by default. Each ACK carries a 64-chunk bitmap, a
  cumulative ACK boundary (`cum_ack`, meaning all chunks before it arrived),
  and an observed upper boundary (`highest_ack`, meaning the highest observed
  chunk plus one). It flushes dirty windows every `--ack-batch` newly received
  chunks and always flushes at message completion. It also flushes at
  bitmap-window boundaries, message tails, and after `--ack-timeout-us`.
- The sender uses `cum_ack` to clear earlier chunks even if earlier window ACKs
  were delayed, and uses `highest_ack` to decide which still-unacked chunks are
  eligible for fast selective retransmission. It no longer infers the global
  observed frontier from a single 64-bit bitmap.
- Immediate data encodes `flow_id12:chunk_id20`; the implementation still caps
  a single flow at `FLOR_MAX_CHUNKS` chunks.
- The benchmark uses TCP only for bootstrap and termination control. ACKs are
  carried on the RDMA UC ACK QP.
