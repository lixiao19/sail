#include "rdma-hw.h"
#include <algorithm>
#include <ns3/ipv4-header.h>
#include <ns3/simple-seq-ts-header.h>
#include <ns3/simulator.h>
#include <ns3/udp-header.h>
#include <iostream>
#include "cn-header.h"
#include "ns3/boolean.h"
#include "ns3/data-rate.h"
#include "ns3/double.h"
#include "ns3/pointer.h"
#include "ns3/ppp-header.h"
#include "ns3/uinteger.h"
#include "ppp-header.h"
#include "qbb-header.h"
#include "rdma-congestion-ops.h"
#include "rdma-queue-pair.h"
#include <limits>
#include <cstdlib>
#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("RdmaHw");
NS_OBJECT_ENSURE_REGISTERED(RdmaHw);

namespace {
bool Lr2HwDebugEnabled() {
  return std::getenv("LR2_DEBUG_FLOW") != nullptr;
}

bool Lr2HwDebugPortMatch(uint16_t dport) {
  const char* raw = std::getenv("LR2_DEBUG_DPORT");
  if (raw != nullptr && raw[0] != '\0') {
    return dport == static_cast<uint16_t>(std::strtoul(raw, nullptr, 10));
  }
  return dport >= 11000 && dport <= 11015;
}
}

TypeId RdmaHw::GetTypeId(void) {
  static TypeId tid =
      TypeId("ns3::RdmaHw")
          .SetParent<Object>()
          .AddAttribute(
              "MinRate",
              "Minimum rate of a throttled flow",
              DataRateValue(DataRate("100Mb/s")),
              MakeDataRateAccessor(&RdmaHw::m_minRate),
              MakeDataRateChecker())
          .AddAttribute(
              "Mtu",
              "Mtu.",
              UintegerValue(1000),
              MakeUintegerAccessor(&RdmaHw::m_mtu),
              MakeUintegerChecker<uint32_t>())
          .AddAttribute(
              "CcMode",
              "which mode of DCQCN is running",
              UintegerValue(0),
              MakeUintegerAccessor(&RdmaHw::m_cc_mode),
              MakeUintegerChecker<uint32_t>())
          .AddAttribute(
              "NACKGenerationInterval",
              "The NACK Generation interval",
              DoubleValue(500.0),
              MakeDoubleAccessor(&RdmaHw::m_nack_interval),
              MakeDoubleChecker<double>())
          .AddAttribute(
              "Rto",
              "Requester retransmission timeout for unacknowledged data.",
              TimeValue(MilliSeconds(30)),
              MakeTimeAccessor(&RdmaHw::m_rto),
              MakeTimeChecker())
          .AddAttribute(
              "SailLossRetryInterval",
              "Receiver-side SAIL switch-NACK retry timeout.",
              TimeValue(MilliSeconds(25)),
              MakeTimeAccessor(&RdmaHw::m_sailLossRetryInterval),
              MakeTimeChecker())
          .AddAttribute(
              "L2ChunkSize",
              "Layer 2 chunk size. Disable chunk mode if equals to 0.",
              UintegerValue(0),
              MakeUintegerAccessor(&RdmaHw::m_chunk),
              MakeUintegerChecker<uint32_t>())
          .AddAttribute(
              "L2AckInterval",
              "Layer 2 Ack intervals. Disable ack if equals to 0.",
              UintegerValue(0),
              MakeUintegerAccessor(&RdmaHw::m_ack_interval),
              MakeUintegerChecker<uint32_t>())
          .AddAttribute(
              "L2BackToZero",
              "Layer 2 go back to zero transmission.",
              BooleanValue(false),
              MakeBooleanAccessor(&RdmaHw::m_backto0),
              MakeBooleanChecker())
          .AddAttribute(
              "RateAI",
              "Rate increment unit in AI period",
              DataRateValue(DataRate("5Mb/s")),
              MakeDataRateAccessor(&RdmaHw::m_rai),
              MakeDataRateChecker())
          .AddAttribute(
              "RateHAI",
              "Rate increment unit in hyperactive AI period",
              DataRateValue(DataRate("50Mb/s")),
              MakeDataRateAccessor(&RdmaHw::m_rhai),
              MakeDataRateChecker())
          .AddAttribute(
              "VarWin",
              "Use variable window size or not",
              BooleanValue(false),
              MakeBooleanAccessor(&RdmaHw::m_var_win),
              MakeBooleanChecker())
          .AddAttribute(
              "RateBound",
              "Bound packet sending by rate, for test only",
              BooleanValue(true),
              MakeBooleanAccessor(&RdmaHw::m_rateBound),
              MakeBooleanChecker())
          .AddAttribute(
              "GPUsPerServer",
              "the number of gpus in a server, used for routing",
              UintegerValue(1),
              MakeUintegerAccessor(&RdmaHw::m_gpus_per_server),
              MakeUintegerChecker<uint32_t>())
          .AddAttribute(
              "TotalPauseTimes",
              "The number of pause times to simulate PFC pause due to PCIe",
              UintegerValue(0),
              MakeUintegerAccessor(&RdmaHw::m_total_pause_times),
              MakeUintegerChecker<uint64_t>())
          .AddAttribute(
              "NVLS_enable",
              "NVLS enable info",
              UintegerValue(0),
              MakeUintegerAccessor(&RdmaHw::nvls_enable),
              MakeUintegerChecker<uint32_t>())
          .AddAttribute(
              "NicCoalesceMethod",
              "The Nic's coalesce method",
              EnumValue(NicCoalesceMethod::PER_QP),
              MakeEnumAccessor(&RdmaHw::m_nic_coalesce_method),
              MakeEnumChecker(
                  NicCoalesceMethod::PER_QP,
                  "PER_QP",
                  NicCoalesceMethod::PER_IP,
                  "PER_IP"))
          .AddAttribute(
              "CnpInterval",
              "The Cnp interval",
              TimeValue(MicroSeconds(4)),
              MakeTimeAccessor(&RdmaHw::m_cnp_interval),
              MakeTimeChecker())
          .AddAttribute(
              "SREnabled",
              "Enable Selective Repeat",
              BooleanValue(false),
              MakeBooleanAccessor(&RdmaHw::m_sr_enabled),
              MakeBooleanChecker())
          .AddAttribute(
              "ReliabilityMode",
              "Explicit reliability mode. AUTO preserves legacy boolean behavior.",
              EnumValue(RdmaHw::RELIABILITY_AUTO),
              MakeEnumAccessor(&RdmaHw::m_reliability_mode),
              MakeEnumChecker(
                  RdmaHw::RELIABILITY_AUTO,
                  "AUTO",
                  RdmaHw::RELIABILITY_GBN,
                  "GBN",
                  RdmaHw::RELIABILITY_SR,
                  "SR",
                  RdmaHw::RELIABILITY_IRN,
                  "IRN",
                  RdmaHw::RELIABILITY_LR2,
                  "LR2",
                  RdmaHw::RELIABILITY_SAIL,
                  "SAIL"))
          .AddAttribute(
              "IrnBufferSlots",
              "Number of selective-repeat bitmap slots in IRN mode (CX7 NIC = 512).",
              UintegerValue(512),
              MakeUintegerAccessor(&RdmaHw::m_irn_buffer_slots),
              MakeUintegerChecker<uint32_t>(1, RECEIVE_BUFFER_SIZE))
          .AddAttribute(
              "BdpFcEnabled",
              "Enable an explicit BDP-FC send window outside IRN mode.",
              BooleanValue(false),
              MakeBooleanAccessor(&RdmaHw::m_bdp_fc_enabled),
              MakeBooleanChecker())
          .AddAttribute(
              "BdpFcWindowSlots",
              "BDP-FC send window in MTU-sized slots when BdpFcEnabled is true.",
              UintegerValue(512),
              MakeUintegerAccessor(&RdmaHw::m_bdp_fc_window_slots),
              MakeUintegerChecker<uint32_t>(1, RECEIVE_BUFFER_SIZE));
  return tid;
}

RdmaHw::RdmaHw()
    : m_rto(MilliSeconds(30)),
      m_sailLossRetryInterval(MilliSeconds(25)) {}

void RdmaHw::enable_nvls() {
  nvls_enable = 1;
}

void RdmaHw::disable_nvls() {
  nvls_enable = 0;
}

void RdmaHw::add_nvswitch(uint32_t nvswitch_id) {
  nvswitch_set.insert(nvswitch_id);
}

void RdmaHw::SetNode(Ptr<Node> node) {
  m_node = node;
}
void RdmaHw::Setup(
    QpCompleteCallback cb,
    SendCompleteCallback send_cb,
    MessageCompleteCallback message_cb) {
  tx_bytes.resize(m_nic.size());
  last_tx_bytes.resize(m_nic.size());
  m_nic_peripTable.resize(m_nic.size());
  for (uint32_t i = 0; i < m_nic.size(); i++) {
    tx_bytes[i] = 0;
    last_tx_bytes[i] = 0;
    Ptr<QbbNetDevice> dev = m_nic[i].dev;
    if (dev == NULL)
      continue;
    // share data with NIC
    dev->m_rdmaEQ->m_qpGrp = m_nic[i].qpGrp;
    // setup callback
    dev->m_rdmaReceiveCb = MakeCallback(&RdmaHw::Receive, this);
    dev->m_rdmaSentCb = MakeCallback(&RdmaHw::SendPacketComplete, this);
    dev->m_rdmaLinkDownCb = MakeCallback(&RdmaHw::SetLinkDown, this);
    dev->m_rdmaPktSent = MakeCallback(&RdmaHw::PktSent, this);
    dev->m_rdmaUpdateTxBytes = MakeCallback(&RdmaHw::UpdateTxBytes, this);
    // config NIC
    dev->m_rdmaEQ->m_rdmaGetNxtPkt = MakeCallback(&RdmaHw::GetNxtPacket, this);
  }
  // setup qp complete callback
  m_qpCompleteCallback = cb;
  m_sendCompleteCallback = send_cb;
  m_messageCompleteCallback = message_cb;
}

/**
 * Resolve the effective reliability mode for this NIC.
 * If the user set an explicit mode (GBN/SR/IRN/LR2/SAIL), use it directly.
 * AUTO (default) falls back to legacy boolean: SREnabled=true -> SR, else GBN.
 */
RdmaHw::ReliabilityMode RdmaHw::ResolveReliabilityMode() const {
  if (m_reliability_mode != RELIABILITY_AUTO) {
    return static_cast<ReliabilityMode>(m_reliability_mode);
  }
  return m_sr_enabled ? RELIABILITY_SR : RELIABILITY_GBN;
}

/**
 * Whether the receiver NIC uses selective-repeat (SR) buffering.
 * SR/IRN use the NIC SR receive path (bitmap-based OOO buffering + per-gap
 * NACK). GBN/LR2/SAIL do NOT. LR2's NIC side behaves as GBN; SAIL uses a
 * host-side software bookkeeping path with fake-packet-visible ACK semantics.
 */
bool RdmaHw::UseSelectiveRepeat() const {
  ReliabilityMode mode = ResolveReliabilityMode();
  return mode == RELIABILITY_SR || mode == RELIABILITY_IRN;
}

/** True if this NIC operates in IRN mode (SR + BDP-FC window). */
bool RdmaHw::IsIrnMode() const {
  return ResolveReliabilityMode() == RELIABILITY_IRN;
}

bool RdmaHw::UseBdpFcWindow() const {
  return IsIrnMode() || m_bdp_fc_enabled;
}

/** True if this NIC operates in LR2 mode (GBN at NIC, SR offloaded to switch). */
bool RdmaHw::IsLr2Mode() const {
  return ResolveReliabilityMode() == RELIABILITY_LR2;
}

/** True if this NIC operates in SAIL mode. */
bool RdmaHw::IsSailMode() const {
  return ResolveReliabilityMode() == RELIABILITY_SAIL;
}

/**
 * Number of SR bitmap slots for the receiver.
 * - IRN: limited by NIC hardware bitmap (default 512 = CX7 NIC).
 *   BDP-FC window = slots * MTU = 512 * 4000 = 2MB.
 * - SR: use full RECEIVE_BUFFER_SIZE (65536 slots = 256MB/QP),
 *   effectively unlimited; this is a simulation idealization.
 */
uint32_t RdmaHw::GetSelectiveBufferSlots() const {
  if (IsIrnMode()) {
    return std::max<uint32_t>(
        1, std::min<uint32_t>(m_irn_buffer_slots, RECEIVE_BUFFER_SIZE));
  }
  return RECEIVE_BUFFER_SIZE;
}

uint64_t RdmaHw::GetGbnSequenceNakSeq(uint64_t expected) const {
  if (!m_backto0 || m_chunk == 0) {
    return expected;
  }
  return expected / m_chunk * m_chunk;
}

bool RdmaHw::TryEnterGbnSequenceNakState(Ptr<RdmaRxQueuePair> rxQp) {
  if (rxQp->m_gbnSeqNakOutstanding) {
    return false;
  }

  uint64_t nackSeq = GetGbnSequenceNakSeq(rxQp->ReceiverNextExpectedSeq);
  rxQp->m_gbnSeqNakOutstanding = true;
  rxQp->m_gbnSeqNakPsn = nackSeq;
  rxQp->ReceiverNextExpectedSeq = nackSeq;
  return true;
}

void RdmaHw::ClearGbnSequenceNakState(
    Ptr<RdmaRxQueuePair> rxQp,
    uint64_t oldExpected) {
  if (!rxQp->m_gbnSeqNakOutstanding) {
    return;
  }
  if (rxQp->m_gbnSeqNakPsn != oldExpected) {
    return;
  }

  rxQp->m_gbnSeqNakOutstanding = false;
  rxQp->m_gbnSeqNakPsn = 0;
}

bool RdmaHw::AdvanceSailCommit(
    Ptr<RdmaRxQueuePair> rxQp,
    uint64_t* commitSeqOut) {
  uint64_t oldCommit = rxQp->m_sailCommitNextExpectedSeq;
  while (true) {
    auto fakeIt =
        rxQp->m_sailPendingFakePackets.find(rxQp->m_sailCommitNextExpectedSeq);
    if (fakeIt != rxQp->m_sailPendingFakePackets.end()) {
      break;
    }

    auto realIt =
        rxQp->m_sailRealBufferedPackets.find(rxQp->m_sailCommitNextExpectedSeq);
    if (realIt == rxQp->m_sailRealBufferedPackets.end()) {
      break;
    }

    rxQp->m_sailCommitNextExpectedSeq += realIt->second;
    rxQp->m_sailRealBufferedPackets.erase(realIt);
  }

  if (commitSeqOut != nullptr) {
    *commitSeqOut = rxQp->m_sailCommitNextExpectedSeq;
  }
  return rxQp->m_sailCommitNextExpectedSeq > oldCommit;
}

void RdmaHw::InsertSailOutstandingLoss(Ptr<RdmaRxQueuePair> rxQp, uint64_t seq) {
  auto it = rxQp->m_sailOutstandingLosses.find(seq);
  if (it == rxQp->m_sailOutstandingLosses.end()) {
    return;
  }
  if (it->second.inFifo) {
    return;
  }

  rxQp->m_sailOutstandingLossFifo.push_back(seq);
  auto tailIt = rxQp->m_sailOutstandingLossFifo.end();
  --tailIt;
  it->second.fifoIt = tailIt;
  it->second.inFifo = true;
}

void RdmaHw::EraseSailOutstandingLoss(Ptr<RdmaRxQueuePair> rxQp, uint64_t seq) {
  auto it = rxQp->m_sailOutstandingLosses.find(seq);
  if (it == rxQp->m_sailOutstandingLosses.end()) {
    return;
  }

  if (it->second.retryEvent.IsRunning()) {
    it->second.retryEvent.Cancel();
  }
  if (it->second.inFifo) {
    rxQp->m_sailOutstandingLossFifo.erase(it->second.fifoIt);
  }
  rxQp->m_sailOutstandingLosses.erase(it);
}

void RdmaHw::ReissueSailOutstandingLoss(Ptr<RdmaRxQueuePair> rxQp, uint64_t seq) {
  auto it = rxQp->m_sailOutstandingLosses.find(seq);
  if (it == rxQp->m_sailOutstandingLosses.end()) {
    return;
  }

  SendSailSwitchNack(rxQp, seq);
  if (it->second.retryEvent.IsRunning()) {
    it->second.retryEvent.Cancel();
  }
  it->second.retryCount++;
  if (it->second.retryCount >= kSailMaxLossRetries) {
    EraseSailOutstandingLoss(rxQp, seq);
    return;
  }

  if (it->second.inFifo) {
    rxQp->m_sailOutstandingLossFifo.splice(
        rxQp->m_sailOutstandingLossFifo.end(),
        rxQp->m_sailOutstandingLossFifo,
        it->second.fifoIt);
  } else {
    InsertSailOutstandingLoss(rxQp, seq);
    it = rxQp->m_sailOutstandingLosses.find(seq);
  }

  it->second.retryEvent = Simulator::Schedule(
      m_sailLossRetryInterval,
      &RdmaHw::HandleSailLossRetry,
      this,
      rxQp->dip,
      rxQp->sip,
      rxQp->dport,
      rxQp->sport,
      rxQp->m_ecn_source.qIndex,
      seq);
}

bool RdmaHw::HandleSailOutstandingLossArrival(
    Ptr<RdmaRxQueuePair> rxQp,
    uint64_t seq) {
  auto stateIt = rxQp->m_sailOutstandingLosses.find(seq);
  if (stateIt == rxQp->m_sailOutstandingLosses.end() || !stateIt->second.inFifo) {
    return false;
  }

  std::vector<uint64_t> prefixSeqs;
  for (auto it = rxQp->m_sailOutstandingLossFifo.begin();
       it != stateIt->second.fifoIt;
       ++it) {
    prefixSeqs.push_back(*it);
  }

  for (uint64_t pendingSeq : prefixSeqs) {
    ReissueSailOutstandingLoss(rxQp, pendingSeq);
  }

  EraseSailOutstandingLoss(rxQp, seq);
  return true;
}

void RdmaHw::ClearResolvedSailLosses(
    Ptr<RdmaRxQueuePair> rxQp,
    uint64_t commitSeq) {
  std::vector<uint64_t> resolvedSeqs;
  auto it = rxQp->m_sailOutstandingLosses.begin();
  while (it != rxQp->m_sailOutstandingLosses.end() && it->first < commitSeq) {
    resolvedSeqs.push_back(it->first);
    ++it;
  }
  for (uint64_t seq : resolvedSeqs) {
    EraseSailOutstandingLoss(rxQp, seq);
  }
}

void RdmaHw::SendSailSwitchNackOnce(Ptr<RdmaRxQueuePair> rxQp, uint64_t seq) {
  qbbHeader seqh;
  seqh.SetSeq(seq);
  seqh.SetPG(rxQp->m_ecn_source.qIndex);
  seqh.SetSport(rxQp->sport);
  seqh.SetDport(rxQp->dport);
  seqh.SetFlags(static_cast<uint16_t>(1 << qbbHeader::FLAG_SWITCH_NACK));

  Ptr<Packet> newp = Create<Packet>(
      std::max(60 - 14 - 20 - static_cast<int>(seqh.GetSerializedSize()), 0));
  newp->AddHeader(seqh);

  Ipv4Header head;
  head.SetDestination(Ipv4Address(rxQp->dip));
  head.SetSource(Ipv4Address(rxQp->sip));
  head.SetProtocol(0xFD);
  head.SetTtl(64);
  head.SetPayloadSize(newp->GetSize());
  head.SetIdentification(rxQp->m_ipid++);
  newp->AddHeader(head);
  AddHeader(newp, 0x800);

  uint32_t nic_idx = GetNicIdxOfRxQp(rxQp);
  m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(newp);
  m_nic[nic_idx].dev->TriggerTransmit();
}

void RdmaHw::SendSailSwitchNack(Ptr<RdmaRxQueuePair> rxQp, uint64_t seq) {
  if (!IsSailMode()) {
    SendSailSwitchNackOnce(rxQp, seq);
    return;
  }

  SendSailSwitchNackOnce(rxQp, seq);
  for (uint32_t copy = 1; copy < m_sailSwitchNackCopies; ++copy) {
    Simulator::Schedule(
        m_sailSwitchNackRedundancyGap * copy,
        &RdmaHw::SendSailSwitchNackOnce,
        this,
        rxQp,
        seq);
  }
}

bool RdmaHw::ShouldSuppressSailSwitchNack(Ptr<RdmaQueuePair> qp, uint64_t seq) {
  Time now = Simulator::Now();
  for (auto it = qp->m_recentSailSwitchNacks.begin();
       it != qp->m_recentSailSwitchNacks.end();) {
    if (now - it->second > m_sailSwitchNackSuppressInterval) {
      it = qp->m_recentSailSwitchNacks.erase(it);
    } else {
      ++it;
    }
  }

  auto existing = qp->m_recentSailSwitchNacks.find(seq);
  if (existing != qp->m_recentSailSwitchNacks.end()) {
    existing->second = now;
    return true;
  }
  qp->m_recentSailSwitchNacks[seq] = now;
  return false;
}

bool RdmaHw::HasOutstandingSailCommit(Ptr<RdmaQueuePair> qp) const {
  if (!IsSailMode() || qp == nullptr || qp->m_messages.empty()) {
    return false;
  }

  const RdmaQueuePair::RdmaMessage& msg = qp->m_messages.front();
  uint64_t messageEnd = msg.m_startSeq + msg.m_size;
  return qp->snd_una >= messageEnd && qp->m_sailCommitAckSeq < messageEnd;
}

void RdmaHw::HandleSailLossRetry(
    uint32_t senderIp,
    uint32_t receiverIp,
    uint16_t senderPort,
    uint16_t receiverPort,
    uint16_t pg,
    uint64_t seq) {
  if (!IsSailMode()) {
    return;
  }

  Ptr<RdmaRxQueuePair> rxQp =
      GetRxQp(receiverIp, senderIp, receiverPort, senderPort, pg, false);
  if (rxQp == nullptr) {
    return;
  }

  auto it = rxQp->m_sailOutstandingLosses.find(seq);
  if (it == rxQp->m_sailOutstandingLosses.end()) {
    return;
  }
  if (seq < rxQp->m_sailCommitNextExpectedSeq) {
    EraseSailOutstandingLoss(rxQp, seq);
    return;
  }

  rxQp->m_sailSwitchNackTimeoutCount++;
  uint64_t fifoHead =
      rxQp->m_sailOutstandingLossFifo.empty()
          ? std::numeric_limits<uint64_t>::max()
          : rxQp->m_sailOutstandingLossFifo.front();
  NS_LOG_INFO("[SAIL timeout] flow=" << rxQp->sip << "->" << rxQp->dip
              << " sport=" << rxQp->sport
              << " dport=" << rxQp->dport
              << " seq=" << seq
              << " fifo_head=" << fifoHead
              << " fifo_size=" << rxQp->m_sailOutstandingLossFifo.size());

  ReissueSailOutstandingLoss(rxQp, seq);
}

int RdmaHw::ReceiveSailLossReport(Ptr<Packet> p, CustomHeader& ch) {
  (void)p;
  if (!IsSailMode()) {
    return 0;
  }

  Ptr<RdmaRxQueuePair> rxQp =
      GetRxQp(ch.dip, ch.sip, ch.ack.dport, ch.ack.sport, ch.ack.pg, true);
  uint64_t seq = ch.ack.seq;
  if (seq < rxQp->m_sailCommitNextExpectedSeq) {
    return 0;
  }
  if (rxQp->m_sailOutstandingLosses.count(seq) != 0) {
    return 0;
  }

  RdmaRxQueuePair::SailLossState lossState;
  rxQp->m_sailOutstandingLosses.emplace(seq, lossState);
  InsertSailOutstandingLoss(rxQp, seq);
  SendSailSwitchNack(rxQp, seq);
  auto inserted = rxQp->m_sailOutstandingLosses.find(seq);
  inserted->second.retryEvent = Simulator::Schedule(
      m_sailLossRetryInterval,
      &RdmaHw::HandleSailLossRetry,
      this,
      ch.sip,
      ch.dip,
      ch.ack.sport,
      ch.ack.dport,
      ch.ack.pg,
      seq);
  return 0;
}

int RdmaHw::HandleSailReceive(
    Ptr<Packet> p,
    CustomHeader& ch,
    Ptr<RdmaRxQueuePair> rxQp,
    uint32_t payloadSize,
    bool isFakePacket) {
  uint64_t seq = ch.udp.seq;
  uint64_t oldExpected = rxQp->ReceiverNextExpectedSeq;
  bool sendVisibleAck = false;
  bool sendCommitAck = false;
  uint64_t visibleAckSeq = rxQp->ReceiverNextExpectedSeq;
  uint64_t commitAckSeq = rxQp->m_sailCommitNextExpectedSeq;

  if (isFakePacket) {
    if (seq == rxQp->ReceiverNextExpectedSeq) {
      rxQp->m_sailPendingFakePackets[seq] = payloadSize;
      rxQp->ReceiverNextExpectedSeq += payloadSize;
      sendVisibleAck = true;
      visibleAckSeq = rxQp->ReceiverNextExpectedSeq;
    } else if (seq <= rxQp->ReceiverNextExpectedSeq) {
      sendVisibleAck = true;
      visibleAckSeq = rxQp->ReceiverNextExpectedSeq;
    }

    if (sendVisibleAck) {
      SendAck(
          rxQp,
          ch,
          1,
          visibleAckSeq,
          static_cast<uint16_t>(1 << qbbHeader::FLAG_SAIL_VISIBLE_ACK));
    }
    return 0;
  }

  if (seq < oldExpected) {
    sendVisibleAck = true;
    visibleAckSeq = rxQp->ReceiverNextExpectedSeq;

    HandleSailOutstandingLossArrival(rxQp, seq);

    if (seq >= rxQp->m_sailCommitNextExpectedSeq &&
        rxQp->m_sailRealBufferedPackets.count(seq) == 0) {
      rxQp->m_sailRealBufferedPackets[seq] = payloadSize;
    } else if (seq < rxQp->m_sailCommitNextExpectedSeq) {
      sendCommitAck = true;
      commitAckSeq = rxQp->m_sailCommitNextExpectedSeq;
    }
    rxQp->m_sailPendingFakePackets.erase(seq);
    bool advancedCommit = AdvanceSailCommit(rxQp, &commitAckSeq);
    sendCommitAck = sendCommitAck || advancedCommit;
    if (advancedCommit) {
      ClearResolvedSailLosses(rxQp, commitAckSeq);
    }
  } else if (seq == oldExpected) {
    rxQp->ReceiverNextExpectedSeq += payloadSize;
    ClearGbnSequenceNakState(rxQp, oldExpected);
    sendVisibleAck = true;
    visibleAckSeq = rxQp->ReceiverNextExpectedSeq;

    HandleSailOutstandingLossArrival(rxQp, seq);

    if (rxQp->m_sailRealBufferedPackets.count(seq) == 0) {
      rxQp->m_sailRealBufferedPackets[seq] = payloadSize;
    }
    sendCommitAck = AdvanceSailCommit(rxQp, &commitAckSeq);
    if (sendCommitAck) {
      ClearResolvedSailLosses(rxQp, commitAckSeq);
    }
  } else {
    if (TryEnterGbnSequenceNakState(rxQp)) {
      SendAck(rxQp, ch, 2, rxQp->ReceiverNextExpectedSeq);
    }
    return 0;
  }

  if (sendVisibleAck && sendCommitAck && visibleAckSeq == commitAckSeq) {
    SendAck(
        rxQp,
        ch,
        1,
        visibleAckSeq,
        static_cast<uint16_t>(
            (1 << qbbHeader::FLAG_SAIL_VISIBLE_ACK) |
            (1 << qbbHeader::FLAG_SAIL_COMMIT_ACK)));
    return 0;
  }
  if (sendVisibleAck) {
    SendAck(
        rxQp,
        ch,
        1,
        visibleAckSeq,
        static_cast<uint16_t>(1 << qbbHeader::FLAG_SAIL_VISIBLE_ACK));
  }
  if (sendCommitAck) {
    SendAck(
        rxQp,
        ch,
        1,
        commitAckSeq,
        static_cast<uint16_t>(1 << qbbHeader::FLAG_SAIL_COMMIT_ACK));
  }
  return 0;
}

void RdmaHw::RestartRtoTimer(Ptr<RdmaQueuePair> qp) {
  if (qp->m_rtoTimer.IsRunning()) {
    Simulator::Cancel(qp->m_rtoTimer);
  }
  if (qp->snd_una < qp->snd_nxt || HasOutstandingSailCommit(qp)) {
    qp->m_rtoTimer =
        Simulator::Schedule(qp->m_rto, &RdmaHw::HandleRtoTimeout, this, qp);
  }
}

/**
 * Selective retransmission: resend exactly ONE packet at the given sequence.
 * Used by SR/IRN host-NACKs and SAIL switch-NACKs.
 * Unlike GBN's RecoverQueue (which resets snd_nxt to snd_una and resends
 * everything), this only enqueues a single repeat packet into the high-
 * priority queue.
 *
 * LR2 does NOT use this function on its main switch-NACK path; it uses
 * StartLr2SwitchRecovery() to enter repeat-mode corrective GBN.
 */
void RdmaHw::RetransmitFromSeq(Ptr<RdmaQueuePair> qp, uint64_t seq) {
  if (seq >= qp->snd_nxt) {
    return;  // nothing sent at this seq yet
  }

  uint32_t payloadSize = m_mtu;
  if (!qp->m_messages.empty()) {
    uint64_t messageEnd =
        qp->m_messages.front().m_startSeq + qp->m_messages.front().m_size;
    if (seq >= messageEnd) {
      return;
    }
    payloadSize = std::min<uint64_t>(payloadSize, messageEnd - seq);
  }

  qp->m_retx_count++;
  Ptr<Packet> repeatPacket = GenRepeatPacket(qp, payloadSize, seq);
  uint32_t nic_idx = GetNicIdxOfQp(qp);
  m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(repeatPacket);
  m_nic[nic_idx].dev->TriggerTransmit();

  RestartRtoTimer(qp);
}

void RdmaHw::StartLr2SwitchRecovery(Ptr<RdmaQueuePair> qp, uint64_t seq) {
  if (Lr2HwDebugEnabled() && qp->sport == 10000 && Lr2HwDebugPortMatch(qp->dport)) {
    std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
              << " host=" << qp->m_src << " src_switch_recovery_enter"
              << " dst=" << qp->m_dest << " sport=" << qp->sport << " dport=" << qp->dport
              << " nackSeq=" << seq << " snd_una=" << qp->snd_una
              << " snd_nxt=" << qp->snd_nxt << " maxSent=" << qp->m_maxSeqEverSent
              << " repeat=" << qp->m_repeatModeActive << " repeatUntil=" << qp->m_repeatUntil
              << std::endl;
  }
  uint64_t replayUpper = std::max<uint64_t>(qp->m_maxSeqEverSent, qp->snd_nxt);
  if (replayUpper == 0) {
    return;
  }

  uint64_t target = std::max<uint64_t>(qp->snd_una, seq);
  if (target >= replayUpper) {
    return;
  }

  qp->snd_nxt = std::min<uint64_t>(qp->snd_nxt, target);
  qp->m_repeatModeActive = true;
  qp->m_repeatUntil = std::max<uint64_t>(qp->m_repeatUntil, replayUpper);
  if (Lr2HwDebugEnabled() && qp->sport == 10000 && Lr2HwDebugPortMatch(qp->dport)) {
    std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
              << " host=" << qp->m_src << " src_switch_recovery_set"
              << " dport=" << qp->dport << " target=" << target
              << " replayUpper=" << replayUpper << " snd_nxt=" << qp->snd_nxt
              << " repeatUntil=" << qp->m_repeatUntil << std::endl;
  }
  RestartRtoTimer(qp);

  uint32_t nic_idx = GetNicIdxOfQp(qp);
  m_nic[nic_idx].dev->TriggerTransmit();
}

uint32_t ip_to_node_id(Ipv4Address ip) {
  return (ip.Get() >> 8) & 0xffff;
}
uint32_t RdmaHw::GetNicIdxOfQp(Ptr<RdmaQueuePair> qp) {
  uint32_t src = qp->m_src;
  uint32_t dst = qp->m_dest;
  if (src / m_gpus_per_server == dst / m_gpus_per_server ||
      m_rtTable_nxthop_nvswitch.count(qp->dip.Get()) !=
          0) { // src and dst are in the same server, communicate through
               // nvswitch
    auto& v = m_rtTable_nxthop_nvswitch[qp->dip.Get()];
    if (v.size() > 0) {
      return v[qp->GetHash() % v.size()];
    } else {
      NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
    }
  } else { // src and dst don't in the same server, communicate through swicth
    auto& v = m_rtTable[qp->dip.Get()];
    if (v.size() > 0) {
      return v[qp->GetHash() % v.size()];
    } else {
      NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
    }
  }
}
uint64_t RdmaHw::GetQpKey(uint32_t dip, uint16_t sport, uint16_t pg) {
  return ((uint64_t)dip << 32) | ((uint64_t)sport << 16) | (uint64_t)pg;
}
Ptr<RdmaQueuePair> RdmaHw::GetQp(uint32_t dip, uint16_t sport, uint16_t pg) {
  uint64_t key = GetQpKey(dip, sport, pg);
  auto it = m_qpMap.find(key);
  if (it != m_qpMap.end())
    return it->second;
  return NULL;
}
Ptr<RdmaQueuePair> RdmaHw::AddQueuePair(
    uint32_t src,
    uint32_t dest,
    uint64_t tag,
    uint64_t size,
    uint16_t pg,
    Ipv4Address sip,
    Ipv4Address dip,
    uint16_t sport,
    uint16_t dport,
    uint32_t win,
    uint64_t baseRtt,
    Callback<void> notifyAppFinish,
    Callback<void> notifyAppSent) {
  // create qp
  Ptr<RdmaQueuePair> qp =
      CreateObject<RdmaQueuePair>(pg, sip, dip, sport, dport);
  qp->SetSrc(src);
  qp->SetDest(dest);
  qp->SetTag(tag);
  qp->SetWin(win);
  qp->SetBaseRtt(baseRtt);
  qp->SetVarWin(m_var_win);
  qp->m_rto = m_rto;

  // IRN BDP-FC (paper Section 3.2): bound in-flight packets to receiver bitmap capacity.
  // The BDP-FC window is a hardware constraint (NIC bitmap size), not a
  // congestion signal.  VarWin would scale this fixed window by m_rate/m_max_rate,
  // which can shrink it below 1 MTU when CC reduces the rate, causing a stall.
  if (UseBdpFcWindow() && qp->m_win == 0) {
    uint32_t slots = IsIrnMode() ? m_irn_buffer_slots : m_bdp_fc_window_slots;
    uint32_t bdpfc_win = slots * m_mtu;
    qp->SetWin(bdpfc_win);
    qp->SetVarWin(false);
  }
  
  if(size != 0){
    qp->PushMessage(size, notifyAppFinish, notifyAppSent);
  }
  // add qp
  uint32_t nic_idx = GetNicIdxOfQp(qp);

  // std::cout << "src is: " << src << ", dst is: " << dest <<  ", nic_idx: " <<
  // nic_idx << ", and the m_nic size is: " << m_nic.size() << std::endl; Assign
  // the qp to specific qbbnetdevice
  m_nic[nic_idx].qpGrp->AddQp(qp);
  uint64_t key = GetQpKey(dip.Get(), sport, pg);
  m_qpMap[key] = qp;
  qp_cnp[key] = 0;
  last_qp_cnp[key] = 0;
  last_qp_rate[key] = 0;

  // set init variables
  DataRate m_bps = m_nic[nic_idx].dev->GetDataRate();
  qp->m_rate = m_bps;
  qp->m_max_rate = m_bps;
  
  Ptr<Packet> mtuPacket = GenDataPacket(qp, m_mtu);
  qp->m_tokenBucket.Init(
      mtuPacket->GetSize(),
      Simulator::Now(),
      std::max(mtuPacket->GetSize(), m_nic[nic_idx].dev->m_rdmaEQ->m_token_per_round) * 2);
  
  TypeId congTypeId;
  switch (m_cc_mode) {
    case 1:
      congTypeId = MellanoxDcqcn::GetTypeId();
      break;
    case 3:
      congTypeId = Hpcc::GetTypeId();
      break;
    case 7:
      congTypeId = Timely::GetTypeId();
      break;
    case 8:
      congTypeId = Dctcp::GetTypeId();
      break;
    case 10:
      congTypeId = HpccPint::GetTypeId();
      break;
    case 11:
      congTypeId = Swift::GetTypeId();
      break;
    case 12:
      congTypeId = RealDcqcn::GetTypeId();
      break;
    case 13:
      congTypeId = HpccEcn::GetTypeId();
      break;
    default:
      NS_FATAL_ERROR(
          "cc_mode" << m_cc_mode << " can not match any CongestionTypeId");
      break;
  }
  bool createAlgo = false;
  if (m_nic_coalesce_method == NicCoalesceMethod::PER_QP) {
    ObjectFactory congestionAlgorithmFactory;
    congestionAlgorithmFactory.SetTypeId(congTypeId);
    Ptr<RdmaCongestionOps> algo =
        congestionAlgorithmFactory.Create<RdmaCongestionOps>();
    qp->m_congestionControl = algo;
    createAlgo = true;
  } else if (m_nic_coalesce_method == NicCoalesceMethod::PER_IP) {
    auto& mp = m_nic_peripTable[nic_idx];
    if (mp.find(dip.Get()) == mp.end()) {
      // Create new tableEntry
      mp[dip.Get()] = CreateObject<NicPerIpTableEntry>();

      ObjectFactory congestionAlgorithmFactory;
      congestionAlgorithmFactory.SetTypeId(congTypeId);
      Ptr<RdmaCongestionOps> algo =
          congestionAlgorithmFactory.Create<RdmaCongestionOps>();
      mp[dip.Get()]->m_congestionControl = algo;
      createAlgo = true;
    }
    qp->m_congestionControl = mp[dip.Get()]->m_congestionControl;
    mp[dip.Get()]->qpNum++;
  }
  if(createAlgo){
    qp->m_congestionControl->SetHw(this, m_nic[nic_idx].dev);
    qp->m_congestionControl->LazyInit(qp, m_bps);
    if(!m_cc_configs.empty()){
      for(auto& cc_config : m_cc_configs){
        qp->m_congestionControl->SetAttribute(cc_config.first, *(cc_config.second));
      }
      m_cc_configs.clear();
    }
  }

  // NVLS settings
  if (nvls_enable == 1)
    qp->nvls_enable = 1;
  else
    qp->nvls_enable = 0;
  // Notify Nic
  m_nic[nic_idx].dev->NewQp(qp);

  return qp;
}

void RdmaHw::DeleteQueuePair(Ptr<RdmaQueuePair> qp) {
  // remove qp from the m_qpMap
  uint64_t key = GetQpKey(qp->dip.Get(), qp->sport, qp->m_pg);
  m_qpMap.erase(key);
  qp_cnp.erase(key);
  last_qp_cnp.erase(key);
  last_qp_rate.erase(key);
}

Ptr<RdmaRxQueuePair> RdmaHw::GetRxQp(
    uint32_t sip,
    uint32_t dip,
    uint16_t sport,
    uint16_t dport,
    uint16_t pg,
    bool create) {
  uint64_t key = ((uint64_t)dip << 32) | ((uint64_t)pg << 16) | (uint64_t)dport;
  #ifdef NS3_MTP
  MtpInterface::explicitCriticalSection cs;
  #endif
  auto it = m_rxQpMap.find(key);
  if (it != m_rxQpMap.end()){
	#ifdef NS3_MTP
	cs.ExitSection();
	#endif
	return it->second;
  }
  if (create) {
    // printf("Create new RxQp for sip: %u, dip: %u, sport: %u, dport: %u, pg: %u\n", sip, dip, sport, dport, pg);
    // create new rx qp
    Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
    // init the qp
    q->sip = sip;
    q->dip = dip;
    q->sport = sport;
    q->dport = dport;
    q->m_ecn_source.qIndex = pg;
    // store in map
    m_rxQpMap[key] = q;
	#ifdef NS3_MTP
	cs.ExitSection();
	#endif
    return q;
  }
  #ifdef NS3_MTP
  cs.ExitSection();
  #endif
  return NULL;
}

uint32_t RdmaHw::GetNicIdxOfRxQp(Ptr<RdmaRxQueuePair> q) {
  if (m_rtTable.count(q->dip) != 0) {
    auto& v = m_rtTable[q->dip];
    if (v.size() > 0)
      return v[q->GetHash() % v.size()];
    else
      NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
  } else if (m_rtTable_nxthop_nvswitch.count(q->dip) != 0) {
    auto& v = m_rtTable_nxthop_nvswitch[q->dip];
    if (v.size() > 0)
      return v[q->GetHash() % v.size()];
    else
      NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
  } else {
    NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
  }
  NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
}
void RdmaHw::DeleteRxQp(uint32_t dip, uint16_t pg, uint16_t dport) {
  uint64_t key = ((uint64_t)dip << 32) | ((uint64_t)pg << 16) | (uint64_t)dport;
  auto it = m_rxQpMap.find(key);
  if (it != m_rxQpMap.end()) {
    for (auto& loss : it->second->m_sailOutstandingLosses) {
      if (loss.second.retryEvent.IsRunning()) {
        loss.second.retryEvent.Cancel();
      }
    }
  }
  m_rxQpMap.erase(key);
}

int RdmaHw::SendPacketComplete(Ptr<Packet> p, CustomHeader& ch) {
  uint16_t qIndex = ch.udp.pg;
  uint16_t port = ch.udp.sport;
  // uint8_t cnp = (ch.flags >> qbbHeader::FLAG_CNP) & 1;
  // int i;
  uint64_t key = GetQpKey(ch.dip, port, qIndex);
  Ptr<RdmaQueuePair> qp = m_qpMap.find(key) == m_qpMap.end() ? nullptr : m_qpMap[key];
  if (qp == NULL) {
    return 0;
  }
  // uint32_t nic_idx = GetNicIdxOfQp(qp);
  // Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;
  SendComplete(qp);
  return 0;
}

void RdmaHw::SendComplete(Ptr<RdmaQueuePair> qp) {
  NS_ASSERT(!m_sendCompleteCallback.IsNull());

  m_sendCompleteCallback(qp);
}

int RdmaHw::ReceiveUdp(Ptr<Packet> p, CustomHeader& ch) {
  uint8_t ecnbits = ch.GetIpv4EcnBits();

  bool isFakePacket = (ch.m_tos & 0x20) != 0; // SAIL fake-packet marker.
  uint32_t payload_size = p->GetSize() - ch.GetSerializedSize();
  // Find or create the corresponding receiver queue pair.
  Ptr<RdmaRxQueuePair> rxQp =
      GetRxQp(ch.dip, ch.sip, ch.udp.dport, ch.udp.sport, ch.udp.pg, true);
  if (ecnbits != 0) {
    rxQp->m_ecn_source.ecnbits |= ecnbits;
    rxQp->m_ecn_source.qfb++;
  }
  rxQp->m_ecn_source.total++;
  rxQp->m_milestone_rx = m_ack_interval;

  if (IsSailMode()) {
    return HandleSailReceive(p, ch, rxQp, payload_size, isFakePacket);
  }

  if (isFakePacket) {
    return 0;
  }

  // x determines ACK type: 0=no ACK, 1=ACK, 2=NACK
  int x = 0;
  uint16_t nackFlags = 0;
  if (!UseSelectiveRepeat()) {
    // GBN / LR2-NIC path: simple in-order check, discard OOO packets
    x = ReceiverCheckSeq(ch.udp.seq, rxQp, payload_size);
  } else {
    // ============================================================
    // SR receive path (used by SR-only / IRN / SAIL)
    //
    // Maintains a circular bitmap of size `bufferSlots` to track
    // which OOO packets have been received. Three cases:
    //   (a) seq == expected: in-order arrival, advance state
    //   (b) seq >  expected: OOO, buffer in bitmap, NACK gaps
    //   (c) seq <  expected: duplicate, reply ACK (lost-ACK recovery)
    // ============================================================
    uint32_t bufferSlots = GetSelectiveBufferSlots();

    // --- Case (a): In-order packet ---
    if (ch.udp.seq == rxQp->ReceiverNextExpectedSeq) {
      if (rxQp->m_receive_buffer_num == 0) {
        // No OOO packets buffered; simplest case, just advance.
        rxQp->ReceiverNextExpectedSeq += payload_size;
        rxQp->m_highest_expected_seq = rxQp->ReceiverNextExpectedSeq;
        x = 1;
      } else if (rxQp->m_receive_buffer_num == 1) {
        // Exactly one OOO packet is buffered; this in-order packet fills
        // the only gap, so we can advance past all buffered data at once.
        rxQp->m_receive_buffer_num = 0;
        for (uint64_t i = 0;
             i < (rxQp->m_highest_expected_seq - rxQp->ReceiverNextExpectedSeq) /
                     m_mtu;
             i += 1) {
          rxQp->m_receive_buffer[rxQp->m_receive_buffer_head] = 0;
          rxQp->m_receive_buffer_head =
              (rxQp->m_receive_buffer_head + 1) % bufferSlots;
        }
        rxQp->ReceiverNextExpectedSeq = rxQp->m_highest_expected_seq;
        x = 1;
      } else {
        // Multiple OOO packets buffered. Fill this gap slot and scan
        // forward to find the next gap (first 0 in the bitmap).
        rxQp->m_receive_buffer_num -= 1;
        rxQp->m_receive_buffer[rxQp->m_receive_buffer_head] = 0;
        rxQp->m_receive_buffer_head =
            (rxQp->m_receive_buffer_head + 1) % bufferSlots;
        for (uint64_t i = 1;
             i < (rxQp->m_highest_expected_seq - rxQp->ReceiverNextExpectedSeq) /
                     m_mtu;
             i += 1) {
          if (rxQp->m_receive_buffer[rxQp->m_receive_buffer_head] == 0) {
            rxQp->ReceiverNextExpectedSeq += i * m_mtu;
            break;
          }
          rxQp->m_receive_buffer[rxQp->m_receive_buffer_head] = 0;
          rxQp->m_receive_buffer_head =
              (rxQp->m_receive_buffer_head + 1) % bufferSlots;
        }
        // Still have gaps; NACK the new first gap.
        rxQp->m_nackTimer = Simulator::Now() + MicroSeconds(m_nack_interval);
        rxQp->m_lastNACK = rxQp->ReceiverNextExpectedSeq;
        x = 2;
      }

    // --- Case (b): OOO packet (seq > expected) ---
    // Rate-limited NACK for the first gap (ReceiverNextExpectedSeq).
    } else if (ch.udp.seq > rxQp->ReceiverNextExpectedSeq) {
      if (Simulator::Now() >= rxQp->m_nackTimer ||
          rxQp->m_lastNACK != rxQp->ReceiverNextExpectedSeq) {
        rxQp->m_nackTimer = Simulator::Now() + MicroSeconds(m_nack_interval);
        rxQp->m_lastNACK = rxQp->ReceiverNextExpectedSeq;
        x = 2;  // NACK
      }

      uint64_t gapSlots = (ch.udp.seq - rxQp->ReceiverNextExpectedSeq) / m_mtu;
      if (gapSlots >= bufferSlots) {
        // IRN BDP-FC overflow: packet beyond bitmap capacity. Drop it and
        // NACK expectedSeq. (In IRN mode bufferSlots=512; in SR mode this
        // should never happen since bufferSlots=65536 >> any practical BDP.)
        // BDP-FC prevents this; if it happens, drop packet and NACK expectedSeq
        if (x != 2) {
          rxQp->m_nackTimer = Simulator::Now() + MicroSeconds(m_nack_interval);
          rxQp->m_lastNACK = rxQp->ReceiverNextExpectedSeq;
          x = 2;
        }
      } else {
        // Within bitmap range; record this OOO packet in the bitmap.
        int pkt_pos =
            (gapSlots + rxQp->m_receive_buffer_head) % bufferSlots;
        if (ch.udp.seq >= rxQp->m_highest_expected_seq) {
          // New high-water mark: this packet is beyond any previously seen.
          // All positions between old highest and this seq are new gaps.
          // Immediately NACK each new gap; this is the "parallel gap
          // detection" that allows SR to recover all losses within one RTT.
          rxQp->m_receive_buffer_num +=
              (ch.udp.seq - rxQp->m_highest_expected_seq) / m_mtu;
          int loss_num = (ch.udp.seq - rxQp->m_highest_expected_seq) / m_mtu;
          for (int i = 0; i < loss_num; i += 1) {
            SendAck(rxQp, ch, 2, rxQp->m_highest_expected_seq + i * m_mtu);
          }
          rxQp->m_highest_expected_seq = ch.udp.seq + payload_size;
          rxQp->m_receive_buffer[pkt_pos] = 1;
        } else {
          // Retransmission filling a known gap; mark bitmap slot as received.
          if (rxQp->m_receive_buffer[pkt_pos] == 0) {
            rxQp->m_receive_buffer_num -= 1;
            rxQp->m_receive_buffer[pkt_pos] = 1;
          }
        }
      }

    // --- Case (c): Duplicate packet (seq < expected) ---
    } else {
      // Duplicate packet (seq < ReceiverNextExpectedSeq).
      // Must reply with ACK carrying current ReceiverNextExpectedSeq so that
      // the sender can advance snd_una.  Without this, a lost ACK causes the
      // sender to RTO-retransmit old data that the receiver silently drops,
      // leading to an infinite RTO loop.
      x = 1;
    }
  }

  // if (isFakePacket) {
  //     if (x == 1) {
  //         NS_LOG_INFO("[Host Recv] Fake Packet Accepted (ACK). State is synced.");
  //     } else if (x == 2 || x == 4) {
  //         NS_LOG_INFO("[Host Recv] Fake Packet Rejected (NACK generated)!"
  //             << " Reason Code=" << x
  //             << " (2=Gap Detected, 4=Out of order?)");
  //     } else if (x == 3) {
  //         NS_LOG_INFO("[Host Recv] Fake Packet Duplicate.");
  //     } else if (x == 5) {
  //         NS_LOG_INFO("[Host Recv] Fake Packet Buffered (Delayed ACK).");
  //     }

  //     std::cout << "Fake Packet " << ch.udp.seq << " x: " << x << " next expected seq: " << rxQp->ReceiverNextExpectedSeq << std::endl;
  // }
  if (UseSelectiveRepeat()) {
    rxQp->m_max_receive_buffer_num =
        std::max(rxQp->m_max_receive_buffer_num, rxQp->m_receive_buffer_num);
    if (x == 2) {
      SendAck(rxQp, ch, 2, rxQp->ReceiverNextExpectedSeq, nackFlags);
    } else {
      SendAck(rxQp, ch, 1, rxQp->ReceiverNextExpectedSeq);
    }
  } else if (x == 1 || x == 2 || x == 3) {
    // GBN mode: generate ACK (x=1), NACK (x=2), or duplicate-ACK (x=3).
    SendAck(rxQp, ch, (x == 3) ? 1 : x, rxQp->ReceiverNextExpectedSeq);
  }

  return 0;
}

int RdmaHw::SendAck(
    Ptr<RdmaRxQueuePair> rxQp,
    CustomHeader& ch,
    int x,
    uint64_t seq,
    uint16_t extraFlags){
  qbbHeader seqh;
  seqh.SetSeq(seq);
  seqh.SetPG(ch.udp.pg);
  seqh.SetSport(ch.udp.dport);
  seqh.SetDport(ch.udp.sport);
  seqh.SetIntHeader(ch.udp.ih);
  seqh.SetFlags(extraFlags);
  if (rxQp->m_ecn_source.ecnbits) {
    if (m_cc_mode == 1) {
      seqh.SetCnp();
    } else {
      uint64_t key = 0;
      if (m_nic_coalesce_method == NicCoalesceMethod::PER_IP) {
        key = GetNicIdxOfRxQp(rxQp); // each nic has a timer
      } else if (m_nic_coalesce_method == NicCoalesceMethod::PER_QP) {
        key = GetQpKey(
            ch.sip, ch.udp.sport, ch.udp.pg); // each rxqp has a timer
      }
      if (m_cnpTable.count(key) == 0) {
        // Create new tableEntry
        m_cnpTable[key] = Simulator::Now() - m_cnp_interval;
      }
      if (Simulator::Now() - m_cnpTable[key] >= m_cnp_interval) {
        seqh.SetCnp();
        m_cnpTable[key] = Simulator::Now();
      }
    }
  }
  rxQp->m_ecn_source.ecnbits = 0;
  rxQp->m_ecn_source.qfb = 0;
  rxQp->m_ecn_source.total = 0;

  Ptr<Packet> newp = Create<Packet>(
      std::max(60 - 14 - 20 - (int)seqh.GetSerializedSize(), 0));
  newp->AddHeader(seqh);

  Ipv4Header head; // Prepare IPv4 header
  head.SetDestination(Ipv4Address(ch.sip));
  head.SetSource(Ipv4Address(ch.dip));
  head.SetProtocol(x == 1 ? 0xFC : 0xFD); // ack=0xFC nack=0xFD
  head.SetTtl(64);
  head.SetPayloadSize(newp->GetSize());
  head.SetIdentification(rxQp->m_ipid++);
  // GPU receives the packet and generate ACK with NVLS tag
  if (ch.m_tos == 4)
    head.SetTos(4);

  newp->AddHeader(head);
  AddHeader(newp, 0x800); // Attach PPP header
  uint32_t dip = ch.dip;
  uint32_t did = (dip >> 8) & 0xffff;
  // send
  uint32_t nic_idx = GetNicIdxOfRxQp(rxQp);
  m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(newp);
  // Packet delivered to the target NVSwitch.
  if (did == m_node->GetId() && m_node->GetNodeType() == 2 && ch.m_tos == 4)
    m_nic[nic_idx].dev->SwitchAsHostSend();
  else
    m_nic[nic_idx].dev->TriggerTransmit();

  return 0;
}

int RdmaHw::ReceiveCnp(Ptr<Packet> p, CustomHeader& ch) {
  // QCN on NIC
  // This is a Congestion signal
  // Then, extract data from the congestion packet.
  // We assume, without verify, the packet is destinated to me
  uint32_t qIndex = ch.cnp.qIndex;
  if (qIndex == 1) { // DCTCP
    return 0;
  }
  uint16_t udpport = ch.cnp.fid; // corresponds to the sport
  // get qp
  Ptr<RdmaQueuePair> qp = GetQp(ch.sip, udpport, qIndex);
  if (qp == NULL) {
    std::cout << "ERROR: QCN NIC cannot find the flow\n";
    return 0;
  }
  // get nic
  uint32_t nic_idx = GetNicIdxOfQp(qp);
  Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;

  if (qp->m_rate == 0) // lazy initialization
  {
    qp->m_congestionControl->LazyInit(qp, dev->GetDataRate());
  }

  uint64_t key = GetQpKey(qp->dip.Get(), qp->sport, qp->m_pg);
  qp_cnp[key]++; // keep CNP counter consistent with ACK-CNP path
  qp->m_congestionControl->HandleCnp(qp);
  dev->TriggerTransmit();
  return 0;
}

int RdmaHw::ReceiveAck(Ptr<Packet> p, CustomHeader& ch) {
  uint16_t qIndex = ch.ack.pg;
  uint16_t port = ch.ack.dport;
  uint64_t seq = ch.ack.seq;
  uint8_t cnp = (ch.ack.flags >> qbbHeader::FLAG_CNP) & 1;
  bool switchNack = (ch.ack.flags >> qbbHeader::FLAG_SWITCH_NACK) & 1;
  bool sailVisibleAck = (ch.ack.flags >> qbbHeader::FLAG_SAIL_VISIBLE_ACK) & 1;
  bool sailCommitAck = (ch.ack.flags >> qbbHeader::FLAG_SAIL_COMMIT_ACK) & 1;

  Ptr<RdmaQueuePair> qp = GetQp(ch.sip, port, qIndex);
  if (qp == NULL || qp->m_messages.empty()) {
    return 0;
  }

  uint32_t nic_idx = GetNicIdxOfQp(qp);
  Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;

  if (IsSailMode() && ch.l3Prot == 0xFC && (sailVisibleAck || sailCommitAck)) {
    uint64_t oldSndUna = qp->snd_una;
    uint64_t oldSailCommitAckSeq = qp->m_sailCommitAckSeq;

    if (sailVisibleAck) {
      if (!m_backto0) {
        qp->Acknowledge(seq);
      } else {
        uint64_t goback_seq = seq / m_chunk * m_chunk;
        qp->Acknowledge(goback_seq);
      }
    }

    if (sailCommitAck) {
      qp->m_sailCommitAckSeq = std::max<uint64_t>(qp->m_sailCommitAckSeq, seq);
      if (qp->IsCurMessageFinishedAt(qp->m_sailCommitAckSeq)) {
        QpCompleteMessage(qp);
        return 0;
      }
    }

    if (cnp) {
      uint64_t key = GetQpKey(qp->dip.Get(), qp->sport, qp->m_pg);
      qp_cnp[key]++;
    }

    if (cnp || (sailVisibleAck && sailCommitAck)) {
      qp->m_congestionControl->HandleAck(qp, p, ch);
    }

    if (qp->snd_una > oldSndUna ||
        qp->m_sailCommitAckSeq > oldSailCommitAckSeq ||
        !qp->m_rtoTimer.IsRunning()) {
      RestartRtoTimer(qp);
    }

    uint32_t dip = ch.dip;
    uint32_t did = (dip >> 8) & 0xffff;
    if (did == m_node->GetId() && m_node->GetNodeType() == 2)
      dev->SwitchAsHostSend();
    else
      dev->TriggerTransmit();
    return 0;
  }

  if (m_ack_interval == 0)
    std::cout << "ERROR: shouldn't receive ack\n";
  else {
    if (ch.l3Prot == 0xFC) // ACK
    {
      if (Lr2HwDebugEnabled() && IsLr2Mode() && qp->sport == 10000 && Lr2HwDebugPortMatch(qp->dport)) {
        std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
                  << " host=" << qp->m_src << " src_ack"
                  << " dst=" << qp->m_dest << " dport=" << qp->dport
                  << " ack=" << seq << " snd_una=" << qp->snd_una
                  << " snd_nxt=" << qp->snd_nxt << std::endl;
      }
      if (!m_backto0) {
        qp->Acknowledge(seq);
      } else {
        uint64_t goback_seq = seq / m_chunk * m_chunk;
        qp->Acknowledge(goback_seq);
      }
      // Restart RTO timer if there are still unACKed packets
      if (qp->snd_una < qp->snd_nxt && !qp->m_rtoTimer.IsRunning()) {
        qp->m_rtoTimer =
            Simulator::Schedule(qp->m_rto, &RdmaHw::HandleRtoTimeout, this, qp);
      }
      if (qp->IsCurMessageFinished()) {
        QpCompleteMessage(qp);
        // if (qp->IsFinished()) {
        //   QpComplete(qp);
        // }
        return 0;
      }
    }
  }
  if (ch.l3Prot == 0xFD) // NACK
  {
    qp->m_nack_recv_count++;
    if (switchNack) {
      if (Lr2HwDebugEnabled() && IsLr2Mode() && qp->sport == 10000 && Lr2HwDebugPortMatch(qp->dport)) {
        std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
                  << " host=" << qp->m_src << " src_switch_nack_recv"
                  << " dst=" << qp->m_dest << " dport=" << qp->dport
                  << " seq=" << seq << " snd_una=" << qp->snd_una
                  << " snd_nxt=" << qp->snd_nxt << " maxSent=" << qp->m_maxSeqEverSent
                  << std::endl;
      }
      if (IsSailMode() && ShouldSuppressSailSwitchNack(qp, seq)) {
        return 0;
      }
      if (IsLr2Mode()) {
        StartLr2SwitchRecovery(qp, seq);
      } else {
        RetransmitFromSeq(qp, seq);
      }
    } else if (UseSelectiveRepeat()) {
      // Host-NACK in SR mode: also selective retransmit (same as switch-NACK)
      RetransmitFromSeq(qp, seq);
    } else {
      if (seq >= qp->snd_una) {
        RecoverQueue(qp, seq, true);
      }
    }
  }

  // handle cnp
  if (cnp) {
    uint64_t key = GetQpKey(qp->dip.Get(), qp->sport, qp->m_pg);
    qp_cnp[key]++; // update for the number of cnp this qp has received
  }

  qp->m_congestionControl->HandleAck(qp, p, ch);

  uint32_t dip = ch.dip;
  uint32_t did = (dip >> 8) & 0xffff;
  // ACK may advance the on-the-fly window, allowing more packets to send
  if (did == m_node->GetId() && m_node->GetNodeType() == 2)
    dev->SwitchAsHostSend();
  else
    dev->TriggerTransmit();
  // std:://cout << "ack triggere transmitted\n";
  return 0;
}

int RdmaHw::Receive(Ptr<Packet> p, CustomHeader& ch) {
  if (ch.l3Prot == 0x11) { // UDP
    ReceiveUdp(p, ch);
  } else if (ch.l3Prot == 0xFF) { // CNP
    ReceiveCnp(p, ch);
  } else if (
      ch.l3Prot == 0xFC &&
      ((ch.ack.flags >> qbbHeader::FLAG_SAIL_LOSS_REPORT) & 1)) {
    ReceiveSailLossReport(p, ch);
  } else if (ch.l3Prot == 0xFD) { // NACK
    ReceiveAck(p, ch);
  } else if (ch.l3Prot == 0xFC) { // ACK
    ReceiveAck(p, ch);
  }
  return 0;
}

int RdmaHw::ReceiverCheckSeq(
    uint64_t seq,
    Ptr<RdmaRxQueuePair> q,
    uint32_t size) {
  uint64_t expected = q->ReceiverNextExpectedSeq;
  if (seq == expected) {
    q->ReceiverNextExpectedSeq = expected + size;
    ClearGbnSequenceNakState(q, expected);
    if (q->ReceiverNextExpectedSeq >= q->m_milestone_rx) {
      q->m_milestone_rx += m_ack_interval;
      return 1; // Generate ACK
    } else if (q->ReceiverNextExpectedSeq % m_chunk == 0) {
      return 1;
    } else {
      return 5;
    }
  } else if (seq > expected) {
    // RC-style GBN: keep only one outstanding sequence NACK for the current
    // gap. Later OOO packets are dropped silently until the missing seq
    // arrives or requester RTO fires.
    if (TryEnterGbnSequenceNakState(q)) {
      return 2;
    }
    return 4;
  } else {
    // Duplicate.
    return 3;
  }
}
void RdmaHw::AddHeader(Ptr<Packet> p, uint16_t protocolNumber) {
  PppHeader ppp;
  ppp.SetProtocol(EtherToPpp(protocolNumber));
  p->AddHeader(ppp);
}
uint16_t RdmaHw::EtherToPpp(uint16_t proto) {
  switch (proto) {
    case 0x0800:
      return 0x0021; // IPv4
    case 0x86DD:
      return 0x0057; // IPv6
    default:
      NS_ASSERT_MSG(false, "PPP Protocol number not defined!");
  }
  return 0;
}

/**
 * GBN recovery: reset snd_nxt to snd_una so the TX path resends everything.
 * Called on host-NACK in GBN/LR2 mode. Unlike RetransmitFromSeq (which sends
 * one packet), this causes the normal DequeueAndTransmit loop to re-send all
 * data from the last acknowledged point.
 */
void RdmaHw::RecoverQueue(Ptr<RdmaQueuePair> qp) {
  RecoverQueue(qp, qp->snd_una, false);
}

void RdmaHw::RecoverQueue(Ptr<RdmaQueuePair> qp, uint64_t seq, bool updateAck) {
  uint64_t replayUpper = std::max<uint64_t>(qp->m_maxSeqEverSent, qp->snd_nxt);
  uint64_t target = std::max<uint64_t>(qp->snd_una, seq);
  if (replayUpper > 0) {
    target = std::min<uint64_t>(target, replayUpper);
  }

  if (updateAck) {
    qp->Acknowledge(target);
  }

  NS_LOG_INFO("GoBackN, qp src:"<<qp->m_src<<" dst:"<<qp->m_dest<<" port:"<<qp->sport<<" dport:"<<qp->dport<<" seq:"<<target);
  qp->snd_nxt = target;
  qp->m_repeatModeActive = false;
  qp->m_repeatUntil = 0;
  RestartRtoTimer(qp);

  uint32_t nic_idx = GetNicIdxOfQp(qp);
  m_nic[nic_idx].dev->TriggerTransmit();
}

void RdmaHw::QpComplete(Ptr<RdmaQueuePair> qp) {
  NS_ASSERT(!m_qpCompleteCallback.IsNull());
  

  uint32_t nic_idx = GetNicIdxOfQp(qp);
  #ifdef NS3_MTP
          MtpInterface::explicitCriticalSection cs;
  #endif
          m_nic[nic_idx].dev->m_rdmaEQ->RemoveFinishedQps();
  #ifdef NS3_MTP
          cs.ExitSection();
  #endif
  
  qp->m_congestionControl->QpComplete(qp);
  if(m_nic_coalesce_method == NicCoalesceMethod::PER_QP)
  {
    qp->m_congestionControl->AllComplete();
  }
  else if(m_nic_coalesce_method == NicCoalesceMethod::PER_IP)
  {
    m_nic_peripTable[nic_idx][qp->dip.Get()]->qpNum--;
    if(m_nic_peripTable[nic_idx][qp->dip.Get()]->qpNum == 0)
    {
      // No qp exists
      // std::cout<<"No qp exists, so delete the ip "<<qp->dip.Get()<<" from nic "<<nic_idx<<std::endl;
      m_nic_peripTable[nic_idx][qp->dip.Get()]->m_congestionControl->AllComplete();
      m_nic_peripTable[nic_idx].erase(qp->dip.Get());
    }
    
  }

  // This callback will log info
  // It may also delete the rxQp on the receiver
  m_qpCompleteCallback(qp);

  qp->m_congestionControl = nullptr;

  // delete the qp
  DeleteQueuePair(qp);
}

void RdmaHw::QpCompleteMessage(Ptr<RdmaQueuePair> qp) {
  if (qp == nullptr || qp->m_messages.empty()) {
    return;
  }
  if (qp->m_rtoTimer.IsRunning()) {
    Simulator::Cancel(qp->m_rtoTimer);
    qp->m_rtoTimer = EventId();
  }

  // qp->m_messages.front().m_notifyAppFinish();
  // callback
  RdmaQueuePair::RdmaMessage msg = qp->m_messages.front();
  qp->FinishMessage();
  m_messageCompleteCallback(qp, msg.m_size);
  if(!qp->m_messages.empty()){
    // Have more messages to send
    // std::cout<<"at "<<Simulator::Now().GetTimeStep()<<"ns, qp src:"<<qp->m_src<<" dst:"<<qp->m_dest<<" port:"<<qp->sport<<" dport:"<<qp->dport<<" has more messages to send\n";
    qp->m_messages.front().m_startSeq = qp->snd_nxt;
    uint32_t nic_idx = GetNicIdxOfQp(qp);
    m_nic[nic_idx].dev->TriggerTransmit();
  }
}

void RdmaHw::SetLinkDown(Ptr<QbbNetDevice> dev) {
  printf("RdmaHw: node:%u a link down\n", m_node->GetId());
}

void RdmaHw::AddTableEntry(
    Ipv4Address& dstAddr,
    uint32_t intf_idx,
    bool is_nvswitch) {
  uint32_t dip = dstAddr.Get();
  if (is_nvswitch == false)
    m_rtTable[dip].push_back(intf_idx);
  else {
    m_rtTable_nxthop_nvswitch[dip].push_back(intf_idx);
  }
}

void RdmaHw::ClearTable() {
  m_rtTable.clear();
  m_rtTable_nxthop_nvswitch.clear();
}

void RdmaHw::RedistributeQp() {
  // clear old qpGrp
  for (uint32_t i = 0; i < m_nic.size(); i++) {
    if (m_nic[i].dev == NULL)
      continue;
    m_nic[i].qpGrp->Clear();
  }

  // redistribute qp
  for (auto& it : m_qpMap) {
    Ptr<RdmaQueuePair> qp = it.second;
    uint32_t nic_idx = GetNicIdxOfQp(qp);
    m_nic[nic_idx].qpGrp->AddQp(qp);
    // Notify Nic
    m_nic[nic_idx].dev->ReassignedQp(qp);
  }
}

Ptr<Packet> RdmaHw::GetNxtPacket(Ptr<RdmaQueuePair> qp) {
  Ptr<Packet> p;
  // if(m_sr_enabled && qp->loss_num > 0){
  //   uint64_t payload_size = m_mtu;
  //   p = GenRepeatPacket(qp, payload_size, qp->loss_queue[qp->loss_queue_head]);
  //   qp->loss_queue_head = (qp->loss_queue_head + 1) % SR_QUEUE_LEN_MAX;
  //   qp->loss_num--;
  // } else{
  uint64_t payload_size = qp->GetBytesLeft();
  if ((uint64_t)m_mtu < payload_size)
    payload_size = m_mtu;
  uint64_t seq = qp->snd_nxt;
  if (seq < qp->m_maxSeqEverSent) {
    qp->m_retx_count++;
  }
  if (Lr2HwDebugEnabled() && IsLr2Mode() && qp->sport == 10000 && Lr2HwDebugPortMatch(qp->dport) && seq < qp->m_maxSeqEverSent) {
    std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
              << " host=" << qp->m_src << " src_send_repeat"
              << " dst=" << qp->m_dest << " dport=" << qp->dport
              << " seq=" << seq << " snd_una=" << qp->snd_una
              << " snd_nxt=" << qp->snd_nxt << " repeat=" << qp->m_repeatModeActive
              << " repeatUntil=" << qp->m_repeatUntil << std::endl;
  }
  p = GenDataPacket(qp, payload_size);
  // update state
  qp->snd_nxt += payload_size;
  qp->m_maxSeqEverSent = std::max<uint64_t>(qp->m_maxSeqEverSent, qp->snd_nxt);
  if (qp->m_repeatModeActive && qp->snd_nxt >= qp->m_repeatUntil) {
    qp->m_repeatModeActive = false;
    qp->m_repeatUntil = 0;
  }
  // }


  qp->m_ipid++;

  // return
  return p;
}

Ptr<Packet> RdmaHw::GenRepeatPacket(Ptr<RdmaQueuePair> qp, uint32_t pkt_size, uint32_t seq) {
  Ptr<Packet> p = Create<Packet>(pkt_size);
  // add SimpleSeqTsHeader
  SimpleSeqTsHeader seqTs;
  seqTs.SetSeq(seq);
  seqTs.SetPG(qp->m_pg);
  p->AddHeader(seqTs);
  // add udp header
  UdpHeader udpHeader;
  udpHeader.SetDestinationPort(qp->dport);
  udpHeader.SetSourcePort(qp->sport);
  p->AddHeader(udpHeader);
  // add ipv4 header
  Ipv4Header ipHeader;
  ipHeader.SetSource(qp->sip);
  ipHeader.SetDestination(qp->dip);
  ipHeader.SetProtocol(0x11);
  ipHeader.SetPayloadSize(p->GetSize());
  ipHeader.SetTtl(64);
  // nvls <-> ToS, ToS = 1 -> NVLS enable
  uint8_t tosValue = 0;
  if (qp->nvls_enable == 1)
    tosValue |= 4;
  tosValue |= 16;
  ipHeader.SetTos(tosValue);
  ipHeader.SetIdentification(qp->m_ipid);
  p->AddHeader(ipHeader);
  // add ppp header
  PppHeader ppp;
  ppp.SetProtocol(
      0x0021); // EtherToPpp(0x800), see point-to-point-net-device.cc
  p->AddHeader(ppp);
  return p;
}

Ptr<Packet> RdmaHw::GenDataPacket(Ptr<RdmaQueuePair> qp, uint32_t pkt_size) {
  Ptr<Packet> p = Create<Packet>(pkt_size);
  // add SimpleSeqTsHeader
  SimpleSeqTsHeader seqTs;
  seqTs.SetSeq(qp->snd_nxt);
  seqTs.SetPG(qp->m_pg);
  p->AddHeader(seqTs);
  // add udp header
  UdpHeader udpHeader;
  udpHeader.SetDestinationPort(qp->dport);
  udpHeader.SetSourcePort(qp->sport);
  p->AddHeader(udpHeader);
  // add ipv4 header
  Ipv4Header ipHeader;
  ipHeader.SetSource(qp->sip);
  ipHeader.SetDestination(qp->dip);
  ipHeader.SetProtocol(0x11);
  ipHeader.SetPayloadSize(p->GetSize());
  ipHeader.SetTtl(64);
  uint8_t tosValue = 0;
  if (qp->nvls_enable == 1) {
    tosValue |= 4;
  }
  if (qp->m_repeatModeActive && qp->snd_nxt < qp->m_repeatUntil) {
    tosValue |= 16;
  }
  if (IsSailMode() && !qp->m_messages.empty()) {
    const RdmaQueuePair::RdmaMessage& msg = qp->m_messages.front();
    uint64_t messageEnd = msg.m_startSeq + msg.m_size;
    if (qp->snd_nxt + pkt_size >= messageEnd) {
      tosValue |= 0x80;
    }
  }
  ipHeader.SetTos(tosValue);
  ipHeader.SetIdentification(qp->m_ipid);
  p->AddHeader(ipHeader);
  // add ppp header
  PppHeader ppp;
  ppp.SetProtocol(
      0x0021); // EtherToPpp(0x800), see point-to-point-net-device.cc
  p->AddHeader(ppp);
  return p;
}

void RdmaHw::PktSent(
    Ptr<RdmaQueuePair> qp,
    Ptr<Packet> pkt,
    Time interframeGap) {
  qp->lastPktSize = pkt->GetSize();
  qp->m_congestionControl->PktSent(qp, pkt);
  // DataRate old = qp->m_rate;
  qp->m_rate = qp->m_congestionControl->m_ccRate;
  // if(old != qp->m_rate)
  //   std::cout<<"At "<<Simulator::Now().GetTimeStep()<<" qp "<<qp->m_src<<" "<<qp->m_dest<<" "<<qp->sport<<" "<<qp->dport<<" rate is "<<qp->m_rate.GetBitRate()<<" old rate is "<<old.GetBitRate()<<std::endl;
  UpdateNextAvail(qp, interframeGap, pkt->GetSize());
  
  // Record packet send time for RTO
  uint64_t seq = qp->snd_nxt - pkt->GetSize();
  qp->m_packetSendTimes[seq] = Simulator::Now();
  
  // Start RTO timer if not running
  if (!qp->m_rtoTimer.IsRunning() && qp->snd_nxt > qp->snd_una) {
    qp->m_rtoTimer = Simulator::Schedule(qp->m_rto, &RdmaHw::HandleRtoTimeout, this, qp);
  }
}

void RdmaHw::UpdateNextAvail(
    Ptr<RdmaQueuePair> qp,
    Time interframeGap,
    uint32_t pkt_size) {
  Time sendingTime;
  if (m_rateBound)
    sendingTime = interframeGap + qp->m_rate.CalculateBytesTxTime(pkt_size);
  else
    sendingTime = interframeGap + qp->m_max_rate.CalculateBytesTxTime(pkt_size);
  qp->m_nextAvail = Simulator::Now() + sendingTime;
}

void RdmaHw::ChangeRate(Ptr<RdmaQueuePair> qp, DataRate new_rate) {
#if 1
  Time sendingTime = qp->m_rate.CalculateBytesTxTime(qp->lastPktSize);
  Time new_sendintTime = new_rate.CalculateBytesTxTime(qp->lastPktSize);
  qp->m_nextAvail = qp->m_nextAvail + new_sendintTime - sendingTime;
  // update nic's next avail event
  uint32_t nic_idx = GetNicIdxOfQp(qp);
  m_nic[nic_idx].dev->UpdateNextAvail(qp->m_nextAvail);
#endif

  // change to new rate
  qp->m_rate = new_rate;
}

/**
 * RTO timeout handler for GBN-style full retransmission.
 * Resets snd_nxt to snd_una so the normal TX path resends all unACKed data.
 * This is the fallback recovery for all modes:
 * - GBN/LR2: primary recovery mechanism (host-NACK also triggers RecoverQueue)
 * - SR/IRN/SAIL: last resort when NACKs fail (e.g., tail-loss where no OOO
 *   packets trigger NACK detection)
 *
 * In LR2, this RTO triggers the P1 path on the Depot switch: the retransmitted
 * packets have seq < Depot's expectedSeq, causing Depot to clear its reorderPool
 * and re-align with the NIC. See switch-node.cc TryHandleLr2DepotData P1 section.
 */
void RdmaHw::HandleRtoTimeout(Ptr<RdmaQueuePair> qp) {
  if (qp->snd_una < qp->snd_nxt) {
    qp->m_rto_count++;
    if (Lr2HwDebugEnabled() && IsLr2Mode() && qp->sport == 10000 && Lr2HwDebugPortMatch(qp->dport)) {
      std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
                << " host=" << qp->m_src << " src_rto"
                << " dst=" << qp->m_dest << " dport=" << qp->dport
                << " snd_una=" << qp->snd_una << " snd_nxt=" << qp->snd_nxt
                << " maxSent=" << qp->m_maxSeqEverSent << std::endl;
    }
    // GBN-style recovery: reset snd_nxt so normal TX path resends all data
    qp->snd_nxt = qp->snd_una;
    uint32_t nic_idx = GetNicIdxOfQp(qp);
    m_nic[nic_idx].dev->TriggerTransmit();

    // Restart RTO timer
    qp->m_rtoTimer = Simulator::Schedule(qp->m_rto, &RdmaHw::HandleRtoTimeout, this, qp);
    return;
  }

  if (HasOutstandingSailCommit(qp)) {
    qp->m_rto_count++;
    RetransmitFromSeq(qp, qp->m_sailCommitAckSeq);
  }
}
/**
 * when nic send a packet, update the bytes it has sent
 */
void RdmaHw::UpdateTxBytes(uint32_t port_id, uint64_t bytes) {
  tx_bytes[port_id] += bytes;
}
/**
 * output format:
 * time, host_id, port_id, bandwidth
 */
void RdmaHw::PrintHostBW(FILE* bw_output, uint32_t bw_mon_interval) {
  for (int i = 0; i < m_nic.size(); ++i) {
    if (tx_bytes[i] == last_tx_bytes[i]) {
      continue;
    }
    double bw =
        (tx_bytes[i] - last_tx_bytes[i]) * 8 * 1e6 / (bw_mon_interval); // bit/s
    bw = bw * 1.0 / 1e9; // Gbps
    fprintf(
        bw_output,
        "%lu, %u, %u, %f\n",
        Simulator::Now().GetTimeStep(),
        m_node->GetId(),
        i,
        bw);
    fflush(bw_output);
    last_tx_bytes[i] = tx_bytes[i];
  }
}
/**
 * output format:
 * time, src, dst, sport, dport, size, rate
 */
void RdmaHw::PrintQPRate(FILE* rate_output) {
  std::unordered_map<uint64_t, Ptr<RdmaQueuePair>>::iterator it =
      m_qpMap.begin();
  for (; it != m_qpMap.end(); it++) {
    Ptr<RdmaQueuePair> qp = it->second;
    uint64_t key = it->first;
    // if (qp->m_rate.GetBitRate() == last_qp_rate[key]) {
    //   continue;
    // }
    fprintf(
        rate_output,
        "%lu, %u, %u, %u, %u, %lu, %lu\n",
        Simulator::Now().GetTimeStep(),
        qp->m_src,
        qp->m_dest,
        qp->sport,
        qp->dport,
        qp->snd_nxt - qp->m_messages.front().m_startSeq,
        qp->m_rate.GetBitRate());
    fflush(rate_output);
    last_qp_rate[key] = qp->m_rate.GetBitRate();
  }
}
/**
 * output format:
 * time, src, dst, sport, dport, size, cnp_number
 */
void RdmaHw::PrintQPCnpNumber(FILE* cnp_output) {
  std::unordered_map<uint64_t, Ptr<RdmaQueuePair>>::iterator it =
      m_qpMap.begin();
  for (; it != m_qpMap.end(); it++) {
    Ptr<RdmaQueuePair> qp = it->second;
    uint64_t key = it->first;
    if (qp_cnp[key] != last_qp_cnp[key]) {
      fprintf(
          cnp_output,
          "%lu, %u, %u, %u, %u, %u, %u\n",
          Simulator::Now().GetTimeStep(),
          qp->m_src,
          qp->m_dest,
          qp->sport,
          qp->dport,
          qp->m_messages.front().m_size,
          qp_cnp[key]);
      fflush(cnp_output);
      last_qp_cnp[key] = qp_cnp[key];
    }
  }
}
// void RdmaHw::SetPintSmplThresh(double p){
//        pint_smpl_thresh = (uint32_t)(65536 * p);
// }
} // namespace ns3
