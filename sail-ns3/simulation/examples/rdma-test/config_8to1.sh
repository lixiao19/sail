# INPUT FILE PATH
TOPOLOGY_FILE examples/rdma-test/ft-ft-topo.txt
FLOW_FILE examples/rdma-test/flows/8to1.txt

# OUTPUT FILE PATH
FCT_OUTPUT_FILE examples/rdma-test/outputs/fct.txt
PFC_OUTPUT_FILE examples/rdma-test/outputs/pfc.txt

# MONITOR SETTINGS
QLEN_MON_FILE examples/rdma-test/outputs/qlen.txt
BW_MON_FILE examples/rdma-test/outputs/bw.txt
RATE_MON_FILE examples/rdma-test/outputs/rate.txt
CNP_MON_FILE examples/rdma-test/outputs/cnp.txt
MON_START 2000001
MON_END 5000000
QP_MON_INTERVAL 100
QLEN_MON_INTERVAL 100
BW_MON_INTERVAL 1000

# TRACE SETTINGS (see more in ns3-interface/analysis)
ENABLE_TRACE 1
TRACE_FILE examples/rdma-test/trace_config.txt
TRACE_OUTPUT_FILE examples/rdma-test/outputs/mix.tr

# VAR SETTINGS
# SIMULATOR_STOP_TIME 40000000000000.00
SIMULATOR_STOP_TIME 10.00
ERROR_RATE_PER_LINK 0.0000

HAS_WIN 0
GLOBAL_T 0

INT_MULTI 1
PINT_LOG_BASE 1.05

ACK_HIGH_PRIO 0

LINK_DOWN 0 0 0

KMAX_MAP 7 25000000000 8000 50000000000 16000 100000000000 32000 200000000000 24000 400000000000 64000 800000000000 48000 1600000000000 48000
KMIN_MAP 7 25000000000 2000 50000000000 4000 100000000000 8000 200000000000 6000 400000000000 16000 800000000000 12000 1600000000000 12000
PMAX_MAP 7 25000000000 0.2 50000000000 0.2 100000000000 0.2 200000000000 0.8 400000000000 0.2 800000000000 0.2 1600000000000 0.2

# Receiver-side LR2 flush/local-retransmission can create an 800G->100G burst
# at the last hop (ToR uplink ingress -> host-facing egress). In this MMU
# model, the hot 800G ingress cannot consume the whole shared pool; with
# dynamic thresholding it effectively gets about half of the usable shared
# buffer before headroom/drop logic kicks in. We therefore hardcode 512MiB so
# even worst-case reorderPool-sized LR2 bursts are not limited by switch MMU.
BUFFER_SIZE 512

# NS3 SETTINGS
ns3::QbbNetDevice::QcnEnabled true
ns3::QbbNetDevice::DynamicThreshold true

ns3::SwitchNode::PfcEnabled false
ns3::SwitchNode::FlowAggregationEnabled false
ns3::SwitchNode::ReliabilityMode LR2
ns3::SwitchNode::EgressSwitchId1 0
ns3::SwitchNode::EgressSwitchId2 1
ns3::SwitchNode::DigestInterval 32
ns3::SwitchNode::ThemisEnabled false
ns3::SwitchNode::ThemisPnpEnabled false
ns3::SwitchNode::ThemisPnpCnpInterval 5us
ns3::SwitchNode::Lr2NackInterval 2000us
ns3::SwitchNode::Lr2ReorderWindowPkts 65536
ns3::SwitchNode::Lr2BackupWindowPkts 20000

ns3::RdmaHw::SREnabled true
ns3::RdmaHw::ReliabilityMode LR2
ns3::RdmaHw::IrnBufferSlots 65536

ns3::RdmaHw::Mtu 4000
ns3::RdmaHw::CcMode 13
ns3::RdmaHw::RateAI 5Mb/s
ns3::RdmaHw::RateHAI 50Mb/s
ns3::RdmaHw::MinRate 100Mb/s
ns3::RdmaHw::L2ChunkSize 4000
ns3::RdmaHw::L2AckInterval 1
ns3::RdmaHw::L2BackToZero false
ns3::RdmaHw::VarWin true
ns3::RdmaHw::RateBound true
ns3::RdmaHw::NicCoalesceMethod PER_QP
ns3::RdmaHw::NACKGenerationInterval 2000.0

ns3::MellanoxDcqcn::AlphaResumInterval 1.0
ns3::MellanoxDcqcn::RateDecreaseInterval 4.0
ns3::MellanoxDcqcn::ClampTargetRate false
ns3::MellanoxDcqcn::RPTimer 900.0
ns3::MellanoxDcqcn::EwmaGain 0.00390625
ns3::MellanoxDcqcn::FastRecoveryTimes 1
ns3::Dctcp::DctcpRateAI 1000Mb/s
ns3::Hpcc::FastReact true
ns3::Hpcc::TargetUtil 0.95
ns3::Hpcc::MiThresh 0
ns3::Hpcc::MultiRate false
ns3::Hpcc::SampleFeedback false
ns3::HpccPint::PintProb 1.0
ns3::RealDcqcn::EwmaGain 0.00390625
ns3::RealDcqcn::F 1
ns3::RealDcqcn::RateUpdateDelay 300us
ns3::RealDcqcn::AlphaUpdateDelay 2.56us
ns3::RealDcqcn::BytesThreshold 524240
ns3::RealDcqcn::Clamp false
