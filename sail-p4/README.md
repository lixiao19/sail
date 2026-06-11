# SAIL Tofino P4 Prototype

This directory contains the Tofino 1 data-plane prototype for SAIL.

## Data-Plane Functions

The program implements the following SAIL functions:

- S-DCI macro-flow sequence assignment.
- Delayed metadata piggybacking over the long-haul path.
- R-DCI loss detection using a bounded receive-history window.
- Dummy packet construction for recoverable lost packets.
- Ordered packet release using logical queues over a loopback path.
- Compact UDP loss and completion notifications for the endpoint agent.

## Topology Constants

The prototype is written for a 1x1 test topology. The default logical roles are:

```text
sender-facing port
receiver-facing port
long-haul sender-to-receiver port
long-haul receiver-to-sender port
notification loopback port
queue/dummy loopback port
```

The concrete device-port constants are defined near the top of `sail.p4`:

```p4
#define PORT_SENDER ...
#define PORT_RECEIVER ...
#define PORT_WAN_TO_RECEIVER ...
#define PORT_WAN_TO_SENDER ...
#define PORT_NOTIFY_LOOP ...
#define PORT_QUEUE_LOOP ...
```

Update these constants and the corresponding control-plane port list in
`test_control/headers.h` before deploying on a different switch or topology.

## Pipeline Placement

The S-DCI logic runs mainly in egress, immediately before packets leave for the
long-haul link. This placement makes the assigned macro-flow sequence number
match the actual long-haul packet order.

The R-DCI logic runs mainly in ingress before packets are released to the
receiver RNIC. It detects missing M-SNs, triggers dummy construction, generates
notifications, and enforces ordered release.

## Queue Emulation

Tofino 1 does not expose flexible P4-accessible queue management primitives.
SAIL therefore emulates two logical queues with a loopback-based recirculation
path:

- `Q_pkt` for normal packets blocked behind a loss.
- `Q_dummy` for generated dummy packets.

Each queued packet carries a small queue shim with its logical queue type, queue
tag, M-SN, and remaining recirculation budget. Queue head tags are maintained in
registers. Only a queue-head packet may check whether its M-SN equals
`Seq_next`.

## Dummy Packets

When metadata for a missing packet is available, the switch clones the metadata
carrier to the queue/dummy loopback path and rewrites the clone into a dummy
packet. The dummy restores the lost packet's five-tuple, destination QP, PSN,
opcode, logical packet length, and RETH fields when the lost packet is a WRITE
FIRST or WRITE ONLY packet.

The prototype rewrites logical IP/UDP lengths but does not recompute RoCE ICRC
or physically truncate the cloned payload.

## Notifications

LOSS and COMPLETE notifications are generated with truncated ingress mirror
copies. After the mirror copy enters the notification loopback port, the data
plane rewrites it into a compact UDP packet carrying:

```c
struct sail_notify {
    uint32_t magic;
    uint8_t  type;
    uint8_t  reserved[3];
    uint32_t dqp;
    uint32_t psn;
};
```

## Build

Set `SDE` and `SDE_INSTALL` for your Tofino SDE installation, then run:

```bash
make
```

This builds the P4 program and the small control-plane helper `sail_ctrl`.

## Control Plane

`test_control/ctrl.c` initializes the switch daemon and configures:

- front-panel ports,
- MAC-near loopback ports,
- the dummy multicast group,
- the notification mirror session.

It does not install dynamic forwarding rules; the prototype uses P4 constant
entries for dispatch and forwarding decisions.

## Limitations

- The prototype targets RC WRITE traffic.
- The metadata history window is fixed by `STEP`.
- Recirculation-budget expiration is used as the fallback path.
- The program is a research prototype and should be reviewed carefully before
  use outside an isolated testbed.
