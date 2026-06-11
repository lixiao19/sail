# SAIL RoCE Reliability Simulator

This repository contains an ns-3 based RoCE/RDMA network simulator derived from
NS-3-ALIBABACLOUD and the UNISON/MTP simulator stack. It is used to evaluate
loss recovery mechanisms for cross-DC RDMA traffic, including GBN, IRN, LR2,
and SAIL.

The current open-source version includes the single-direction SAIL
implementation only. The experimental bidirectional SAIL branch is not part of
this release.

## Repository Layout

- `simulation/`: ns-3 simulator source tree and build entry point.
- `simulation/scratch/NormalNetwork.cc`: main RDMA network simulation program.
- `simulation/examples/rdma-test/`: example topology, flow, trace, and config
  files.
- `simulation/src/point-to-point/model/`: RDMA host, switch, queue, and link
  models, including the reliability mechanisms.

Generated experiment directories and result files are intentionally excluded
from version control.

## Build

Build from the `simulation` directory:

```bash
cd simulation
./ns3 configure --enable-mtp --disable-examples
./ns3 build
```

## Run The Example Workload

`NormalNetwork` takes the config file as its first positional argument. A small
8-to-1 example is provided under `simulation/examples/rdma-test/`:

```bash
cd simulation
./ns3 run "NormalNetwork examples/rdma-test/config_8to1.sh --RngSeed=1 --RngRun=1 --mtpThreads=8"
```

The example writes FCT, PFC, queue-length, bandwidth, rate, CNP, and trace
outputs to the paths configured in `config_8to1.sh`.

If invoking the built binary directly, use the same positional config argument:

```bash
cd simulation
./build/scratch/ns3.36.1-NormalNetwork-optimized examples/rdma-test/config_8to1.sh --RngSeed=1 --RngRun=1 --mtpThreads=8
```

The exact binary name may differ if the build profile changes.

## Selecting A Reliability Mode

Set the host and switch reliability mode in the config file:

```text
ns3::RdmaHw::ReliabilityMode SAIL
ns3::SwitchNode::ReliabilityMode SAIL
```

Supported values are:

- `GBN`: host-side go-back-N recovery.
- `SR`: host-side selective recovery.
- `IRN`: IRN-style host selective recovery.
- `LR2`: switch-assisted LR2 recovery.
- `SAIL`: single-direction SAIL gateway aggregation and recovery.

For SAIL, enable gateway aggregation on the switch side:

```text
ns3::SwitchNode::FlowAggregationEnabled true
ns3::SwitchNode::EgressSwitchId1 0
ns3::SwitchNode::EgressSwitchId2 1
ns3::SwitchNode::DigestInterval 8
ns3::SwitchNode::SailDigestTimeout 15000us
ns3::SwitchNode::SailDigestCopies 3
ns3::RdmaHw::SailLossRetryInterval 50ms
```

The single-direction SAIL path aggregates packets at the sender-side gateway,
sends digest metadata over the WAN, reconstructs visible packets at the
receiver-side gateway, and lets the receiver host issue SAIL loss reports for
selective retransmission. Reverse-direction SAIL aggregation is not enabled in
this release.

## Common Parameters

Useful knobs in config files include:

```text
ERROR_RATE_PER_LINK 0.001
ns3::RdmaHw::Mtu 4096
ns3::RdmaHw::Rto 70ms
ns3::RdmaHw::CcMode 1
ns3::SwitchNode::PfcEnabled true
ns3::SwitchNode::Lr2NackInterval 2000us
ns3::SwitchNode::Lr2ReorderWindowPkts 65536
ns3::SwitchNode::Lr2BackupWindowPkts 20000
ns3::SwitchNode::Lr2BackupTimeout 25us
```

Config parsing follows the existing `simulation/scratch/common.h` format:
plain keys configure workload/input files, while `ns3::...` keys are passed to
the corresponding ns-3 attributes.

## Debugging

LR2 debug logs are disabled by default and can be enabled with environment
variables:

```bash
LR2_DEBUG_FLOW=1 LR2_DEBUG_DPORT=10000 ./ns3 run "NormalNetwork examples/rdma-test/config_8to1.sh --RngSeed=1 --RngRun=1 --mtpThreads=0"
```

Use `LR2_DEBUG_SNAPSHOT=1` to dump LR2 switch-side state snapshots when the
simulation schedules debug snapshot events.

## Acknowledgement

This codebase builds on NS-3-ALIBABACLOUD and related SimAI/UNISON simulator
components. The SAIL, LR2, and reliability-mode extensions in this branch are
research prototype code for packet loss recovery experiments.
