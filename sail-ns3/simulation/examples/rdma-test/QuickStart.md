# Quick Start Guide

This guide will help you quickly get started with running the ns-3-alibabacloud simulation (incast).

## Prerequisites

- Git
- Python3
- NS-3 development environment

## Steps to Run

1. Clone the repository:
```bash
git clone https://github.com/aliyun/ns-3-alibabacloud
git checkout dev/qp
```

2. Configure the paths:

Open [source_dir]/ns-3-alibabacloud/simulation/examples/rdma-test/config_8to1.sh
Update all path prefixes to ensure correct paths in your environment.

3. Run incast example:
```bash
cd ./ns-3-alibabacloud/simulation
./ns3 configure -d default --enable-mtp
./ns3 run 'scratch/NormalNetwork examples/rdma-test/config_8to1.sh'
NS_LOG=SwitchNode=all ./ns3 run 'scratch/NormalNetwork examples/rdma-test/config_8to1.sh'
NS_LOG=RdmaHw=all ./ns3 run 'scratch/NormalNetwork examples/rdma-test/config_8to1.sh'
```

For deterministic reruns, pass both the ns-3 RNG parameters and the MTP thread count explicitly:

```bash
./ns3 run 'scratch/NormalNetwork examples/rdma-test/config_8to1.sh --RngSeed=1 --RngRun=6 --mtpThreads=0'
```

Notes:
- `RngSeed` controls the global master seed. Keep it fixed unless you intentionally want a different seed family.
- `RngRun` controls the ns-3 substream and should be used as the experiment seed for independent repetitions.
- `mtpThreads=0` disables MTP and avoids nondeterministic event interleavings. If you omit it, `NormalNetwork` defaults to 16 MTP threads and the same `RngRun` may still produce different results across runs.
- The current `NormalNetwork + CcMode=13` path uses ns-3 RNG for link loss. Some other code paths still use `rand()` directly, for example `QpReuseNetwork` and HPCC-PINT related logic, so `RngRun` alone does not make those paths reproducible yet.

`config_8to1.sh` now includes THEMIS-PNP toggles:
- `ns3::SwitchNode::ThemisEnabled`
- `ns3::SwitchNode::ThemisPnpEnabled`
- `ns3::SwitchNode::ThemisPnpCnpInterval`

Set them to `false` for baseline runs.

```bash
# trace
./trace_reader examples/rdma-test/outputs/mix_8to1.tr > output.txt
```

```bash
# 用gdb运行
./ns3 run 'scratch/NormalNetwork examples/rdma-test/config_8to1.sh' --gdb
(gdb) run

# 程序崩溃后会停在出错位置
Program received signal SIGSEGV, Segmentation fault.
0x00007ffff7b2c123 in ns3::SwitchNode::ProcessDigestPacket() at ../src/point-to-point/model/switch-node.cc:260

# 查看堆栈
(gdb) backtrace
#0  0x00007ffff7b2c123 in ns3::SwitchNode::ProcessDigestPacket() at switch-node.cc:260
#1  0x00007ffff7b2d456 in ns3::SwitchNode::SwitchReceiveFromDevice() at switch-node.cc:825
#2  0x00007ffff7a1b789 in ns3::QbbNetDevice::Receive() at qbb-net-device.cc:456
...

# 查看当前变量
(gdb) info locals
buffer = 0x0
entryCount = 3735928559
packet = 0x555555abcdef

# 查看具体变量值
(gdb) print buffer
$1 = (uint8_t *) 0x0

# 查看指针指向的内容
(gdb) print *packet
$2 = {m_buffer = ..., ...}

# 单步执行
(gdb) next
(gdb) step
```

4. Get results:
```bash
cd examples/rdma-test/outputs/
python3 plot_bw.py -i bw_8to1.txt
```
