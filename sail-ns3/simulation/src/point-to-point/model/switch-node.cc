#include "ns3/ipv4.h"
#include "ns3/packet.h"
#include "ns3/ipv4-header.h"
#include "ns3/pause-header.h"
#include "ns3/flow-id-tag.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "switch-node.h"
#include "qbb-net-device.h"
#include "qbb-channel.h"
#include "ppp-header.h"
#include "ns3/int-header.h"
#include "qbb-header.h"
#include "cn-header.h"
#include "wan-opt-header.h"
#include "ns3/int-header.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <limits>
#include <cstdlib>

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("SwitchNode");
NS_OBJECT_ENSURE_REGISTERED(SwitchNode);

namespace {
void SetFlowIdTag(Ptr<Packet> packet, uint32_t flowId) {
	FlowIdTag flowIdTag;
	flowIdTag.SetFlowId(flowId);
	if (!packet->ReplacePacketTag(flowIdTag)) {
		packet->AddPacketTag(flowIdTag);
	}
}

bool Lr2DebugEnabled() {
	return std::getenv("LR2_DEBUG_FLOW") != nullptr;
}

bool Lr2DebugSnapshotEnabled() {
	return std::getenv("LR2_DEBUG_SNAPSHOT") != nullptr;
}

bool Lr2DebugPortMatch(uint16_t dport) {
	const char* raw = std::getenv("LR2_DEBUG_DPORT");
	if (raw != nullptr && raw[0] != '\0') {
		return dport == static_cast<uint16_t>(std::strtoul(raw, nullptr, 10));
	}
	return dport >= 11000 && dport <= 11015;
}

bool Lr2DebugDataPacket(const CustomHeader& ch) {
	return Lr2DebugEnabled() && ch.l3Prot == 0x11 && ch.udp.sport == 10000 &&
		Lr2DebugPortMatch(ch.udp.dport);
}

bool Lr2DebugAckPacket(const CustomHeader& ch) {
	return Lr2DebugEnabled() && (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD) &&
		ch.ack.dport == 10000 && Lr2DebugPortMatch(ch.ack.sport);
}

uint32_t Lr2DebugNodeFromIp(uint32_t ip) {
	return (ip >> 8) & 0xffff;
}
}  // namespace

TypeId SwitchNode::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SwitchNode")
    .SetParent<Node> ()
    .AddConstructor<SwitchNode> ()
	.AddAttribute("EcnEnabled",
			"Enable ECN marking.",
			BooleanValue(false),
			MakeBooleanAccessor(&SwitchNode::m_ecnEnabled),
			MakeBooleanChecker())
	.AddAttribute("PfcEnabled",
			"Enable PFC.",
			BooleanValue(true),
            MakeBooleanAccessor(&SwitchNode::m_pfcEnabled),
            MakeBooleanChecker())
	.AddAttribute("CcMode",
			"CC mode.",
			UintegerValue(0),
			MakeUintegerAccessor(&SwitchNode::m_ccMode),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("AckHighPrio",
			"Set high priority for ACK/NACK or not",
			UintegerValue(0),
			MakeUintegerAccessor(&SwitchNode::m_ackHighPrio),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("MaxRtt",
			"Max Rtt of the network",
			UintegerValue(9000),
			MakeUintegerAccessor(&SwitchNode::m_maxRtt),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("FlowAggregationEnabled",
			"Enable flow aggregation at DCI switch",
			BooleanValue(true),
			MakeBooleanAccessor(&SwitchNode::m_flowAggregationEnabled),
			MakeBooleanChecker())
	.AddAttribute("ReliabilityMode",
			"Explicit reliability mode. AUTO preserves legacy switch boolean behavior.",
			EnumValue(SwitchNode::RELIABILITY_AUTO),
			MakeEnumAccessor(&SwitchNode::m_reliabilityMode),
			MakeEnumChecker(
				SwitchNode::RELIABILITY_AUTO,
				"AUTO",
				SwitchNode::RELIABILITY_GBN,
				"GBN",
				SwitchNode::RELIABILITY_SR,
				"SR",
				SwitchNode::RELIABILITY_IRN,
				"IRN",
				SwitchNode::RELIABILITY_LR2,
				"LR2",
				SwitchNode::RELIABILITY_SAIL,
				"SAIL"))
	.AddAttribute("EgressSwitchId1",
			"Node ID of Egress Switch in Data Center 1",
			UintegerValue(0),
			MakeUintegerAccessor(&SwitchNode::m_egressSwitchId1),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("EgressSwitchId2",
			"Node ID of Egress Switch in Data Center 2",
			UintegerValue(1),
			MakeUintegerAccessor(&SwitchNode::m_egressSwitchId2),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("DigestInterval",
			"Number of aggregated packets before sending a digest packet",
			UintegerValue(32),
			MakeUintegerAccessor(&SwitchNode::m_digestInterval),
			MakeUintegerChecker<uint32_t>())
		.AddAttribute("SailDigestTimeout",
				"Timeout before a SAIL batch falls back to raw forwarding.",
				TimeValue(MicroSeconds(15000)),
				MakeTimeAccessor(&SwitchNode::m_sailDigestTimeout),
				MakeTimeChecker())
		.AddAttribute("SailDigestCopies",
				"Number of redundant SAIL digest packets sent for each batch.",
				UintegerValue(3),
				MakeUintegerAccessor(&SwitchNode::m_sailDigestCopies),
				MakeUintegerChecker<uint32_t>(1))
		.AddAttribute("MTU",
				"MTU size",
				UintegerValue(4000),
				MakeUintegerAccessor(&SwitchNode::m_mtu),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("ThemisEnabled",
				"Enable THEMIS features at external switches.",
				BooleanValue(false),
				MakeBooleanAccessor(&SwitchNode::m_themisEnabled),
				MakeBooleanChecker())
		.AddAttribute("ThemisPnpEnabled",
				"Enable THEMIS proactive notification point (PNP).",
				BooleanValue(false),
				MakeBooleanAccessor(&SwitchNode::m_themisPnpEnabled),
				MakeBooleanChecker())
		.AddAttribute("ThemisPnpCnpInterval",
				"Minimum interval between proactive CNPs for the same flow.",
				TimeValue(MicroSeconds(5)),
				MakeTimeAccessor(&SwitchNode::m_themisPnpCnpInterval),
				MakeTimeChecker())
		.AddAttribute("Lr2NackInterval",
				"Minimum interval between two LR2 switch-NACKs for the same flow/seq.",
				TimeValue(MicroSeconds(2000)),
				MakeTimeAccessor(&SwitchNode::m_lr2NackInterval),
				MakeTimeChecker())
		.AddAttribute("Lr2ReorderWindowPkts",
				"Maximum packets buffered in LR2 depot reorder pool.",
				UintegerValue(20000),
				MakeUintegerAccessor(&SwitchNode::m_lr2ReorderWindowPkts),
				MakeUintegerChecker<uint32_t>(1))
		.AddAttribute("Lr2BackupWindowPkts",
				"Maximum packets kept in LR2 depot backup pool.",
				UintegerValue(20000),
				MakeUintegerAccessor(&SwitchNode::m_lr2BackupWindowPkts),
				MakeUintegerChecker<uint32_t>(1))
		.AddAttribute("Lr2BackupTimeout",
				"Timeout for Depot backup pool retransmission. Should be > WAN RTT.",
				TimeValue(MilliSeconds(25)),
				MakeTimeAccessor(&SwitchNode::m_lr2BackupTimeout),
				MakeTimeChecker())
	  ;
  return tid;
}

SwitchNode::SwitchNode(){
	m_ecmpSeed = m_id;
	m_node_type = 1;
	m_reliabilityMode = RELIABILITY_AUTO;
	m_sailDigestTimeout = MicroSeconds(15000);
	m_sailDigestCopies = 3;
	m_mmu = CreateObject<SwitchMmu>();
	for (uint32_t i = 0; i < pCnt; i++)
		for (uint32_t j = 0; j < pCnt; j++)
			for (uint32_t k = 0; k < qCnt; k++)
				m_bytes[i][j][k] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_txBytes[i] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_lastPktSize[i] = m_lastPktTs[i] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_u[i] = 0;

	m_aggregatedFlow.nextAggregatedPsn = 0;
	m_aggregatedFlow.digestCounter = 0;
	m_themisPnpSent = 0;
	m_themisPnpCacheHit = 0;
	m_themisPnpEcnCleared = 0;
	m_lr2ReorderPoolCurPkts = 0;
	m_lr2ReorderPoolPeakPkts = 0;
	m_lr2BackupPoolCurPkts = 0;
	m_lr2BackupPoolPeakPkts = 0;
	m_sailBufferedCurPkts = 0;
	m_sailBufferedPeakPkts = 0;
	m_switchRecoveryBufferPeakPkts = 0;
	m_wanTxBytes = 0;
	m_nextSailBatchToRelease = 0;
}

uint64_t SwitchNode::GetLr2ReorderPoolPeakPkts() const {
	return m_lr2ReorderPoolPeakPkts;
}

uint64_t SwitchNode::GetLr2BackupPoolPeakPkts() const {
	return m_lr2BackupPoolPeakPkts;
}

uint64_t SwitchNode::GetSailBufferedPeakPkts() const {
	return m_sailBufferedPeakPkts;
}

uint64_t SwitchNode::GetSwitchRecoveryBufferPeakPkts() const {
	return m_switchRecoveryBufferPeakPkts;
}

uint64_t SwitchNode::GetWanTxBytes() const {
	return m_wanTxBytes;
}

uint64_t SwitchNode::GetCurrentSwitchRecoveryBufferPkts() const {
	return m_lr2ReorderPoolCurPkts + m_lr2BackupPoolCurPkts + m_sailBufferedCurPkts;
}

void SwitchNode::DumpLr2DebugState() const {
	if (!Lr2DebugSnapshotEnabled()) {
		return;
	}
	auto dumpOne = [this](const char* side, const std::map<FiveTuple, Lr2FlowState>& table) {
		for (const auto& item : table) {
			const FiveTuple& flow = item.first;
			const Lr2FlowState& state = item.second;
			if (flow.sport != 10000 || !Lr2DebugPortMatch(flow.dport)) {
				continue;
			}
			std::cout << "[LR2STATE] t=" << Simulator::Now().GetTimeStep()
				<< " node=" << GetId() << " side=" << side
				<< " flow=" << Lr2DebugNodeFromIp(flow.sip) << "->" << Lr2DebugNodeFromIp(flow.dip)
				<< " sport=" << flow.sport << " dport=" << flow.dport
				<< " exp=" << state.expectedSeq << " expD=" << state.expectedSeqD
				<< " ack=" << state.ackSeq << " ret=" << state.retState
				<< " rec=" << state.recoverSeq
				<< " reorder=" << state.reorderPool.size()
				<< " backup=" << state.backupPool.size()
				<< " outstanding=" << state.outstandingNackSeqs.size()
				<< " lastNackSeq=" << state.lastNackSeq;
			if (!state.reorderPool.empty()) {
				std::cout << " reorderFirst=" << state.reorderPool.begin()->first
					<< " reorderLast=" << state.reorderPool.rbegin()->first;
			}
			if (!state.backupPool.empty()) {
				std::cout << " backupFirst=" << state.backupPool.begin()->first
					<< " backupLast=" << state.backupPool.rbegin()->first;
			}
			std::cout << std::endl;
		}
	};
	dumpOne("depot", m_lr2DepotState);
	dumpOne("sentry", m_lr2SentryState);
}

void SwitchNode::RefreshSwitchRecoveryBufferPeak() {
	m_switchRecoveryBufferPeakPkts =
		std::max(m_switchRecoveryBufferPeakPkts, GetCurrentSwitchRecoveryBufferPkts());
}

bool SwitchNode::IsWanPort(uint32_t ifIndex) const {
	uint32_t nodeId = this->GetId();
	if (nodeId != m_egressSwitchId1 && nodeId != m_egressSwitchId2) {
		return false;
	}
	if (ifIndex >= m_devices.size()) {
		return false;
	}
	Ptr<NetDevice> localNetDevice = m_devices[ifIndex];
	Ptr<QbbNetDevice> localQbbDev = DynamicCast<QbbNetDevice>(localNetDevice);
	if (localQbbDev == nullptr) {
		return false;
	}
	Ptr<QbbChannel> channel = DynamicCast<QbbChannel>(localQbbDev->GetChannel());
	if (channel == nullptr || channel->GetNDevices() != 2) {
		return false;
	}

	uint32_t peerNodeId = std::numeric_limits<uint32_t>::max();
	for (size_t i = 0; i < channel->GetNDevices(); ++i) {
		Ptr<NetDevice> candidate = channel->GetDevice(i);
		if (candidate != localNetDevice) {
			peerNodeId = candidate->GetNode()->GetId();
			break;
		}
	}
	if (peerNodeId == std::numeric_limits<uint32_t>::max()) {
		return false;
	}

	if (nodeId == m_egressSwitchId1) {
		return peerNodeId == m_egressSwitchId2;
	}
	return peerNodeId == m_egressSwitchId1;
}

bool SwitchNode::IsLr2Enabled() const {
	return m_reliabilityMode == RELIABILITY_LR2;
}

bool SwitchNode::IsFlowAggregationActive() const {
	if (m_reliabilityMode == RELIABILITY_AUTO) {
		return m_flowAggregationEnabled;
	}
	return m_reliabilityMode == RELIABILITY_SAIL;
}

bool SwitchNode::IsReliabilityDataPacket(const CustomHeader& ch) const {
	return ch.l3Prot == 0x11 &&
		   ch.wanOptInfo.isAggregated == 0 &&
		   ch.wanOptInfo.isDigestPacket == 0 &&
		   ch.wanOptInfo.isFakePacket == 0;
}

bool SwitchNode::IsRepeatPacket(const CustomHeader& ch) const {
	return (ch.m_tos & 0x10) != 0 || ch.wanOptInfo.isRepeatPacket == 1;
}

bool SwitchNode::IsSailFlowTailPacket(const CustomHeader& ch) const {
	return (ch.m_tos & SAIL_FLOW_TAIL_TOS) != 0;
}

SwitchNode::FiveTuple SwitchNode::BuildFlowKeyFromUdp(const CustomHeader& ch) const {
	FiveTuple key;
	key.sip = ch.sip;
	key.dip = ch.dip;
	key.sport = ch.udp.sport;
	key.dport = ch.udp.dport;
	key.pg = ch.udp.pg;
	return key;
}

SwitchNode::FiveTuple SwitchNode::BuildFlowKeyFromAck(const CustomHeader& ch) const {
	FiveTuple key;
	key.sip = ch.dip;
	key.dip = ch.sip;
	key.sport = ch.ack.dport;
	key.dport = ch.ack.sport;
	key.pg = ch.ack.pg;
	return key;
}

uint32_t SwitchNode::GetPayloadSize(Ptr<Packet> packet, const CustomHeader& ch) const {
	uint32_t headerSize = ch.GetSerializedSize();
	if (packet->GetSize() < headerSize) {
		return 0;
	}
	return packet->GetSize() - headerSize;
}

uint16_t SwitchNode::ComputeLr2GapPkts(
	uint64_t startSeq,
	uint64_t endSeq,
	uint32_t stepBytes) const {
	uint32_t step = std::max<uint32_t>(stepBytes, 1);
	if (endSeq <= startSeq) {
		return 1;
	}
	uint64_t gapBytes = endSeq - startSeq;
	uint64_t gapPkts = (gapBytes + step - 1) / step;
	return static_cast<uint16_t>(std::max<uint64_t>(1, std::min<uint64_t>(gapPkts, 0x3fff)));
}

uint16_t SwitchNode::DecodeLr2GapPkts(uint16_t flags) const {
	uint16_t gapPkts = flags >> 2;
	return gapPkts == 0 ? 1 : gapPkts;
}

uint64_t SwitchNode::ComputeLr2GapEndSeq(
	uint64_t startSeq,
	uint16_t gapPkts,
	uint32_t stepBytes) const {
	uint32_t step = std::max<uint32_t>(stepBytes, 1);
	return startSeq + static_cast<uint64_t>(std::max<uint16_t>(gapPkts, 1)) * step;
}

void SwitchNode::RecordLr2SentryMissingRange(
	Lr2FlowState& state,
	uint64_t startSeq,
	uint16_t gapPkts,
	uint32_t stepBytes) {
	uint64_t newStart = startSeq;
	uint64_t newEnd = ComputeLr2GapEndSeq(startSeq, gapPkts, stepBytes);

	auto it = state.srFilterRanges.lower_bound(newStart);
	if (it != state.srFilterRanges.begin()) {
		auto prev = std::prev(it);
		if (prev->second >= newStart) {
			newStart = std::min(newStart, prev->first);
			newEnd = std::max(newEnd, prev->second);
			it = state.srFilterRanges.erase(prev);
		}
	}
	while (it != state.srFilterRanges.end() && it->first <= newEnd) {
		newStart = std::min(newStart, it->first);
		newEnd = std::max(newEnd, it->second);
		it = state.srFilterRanges.erase(it);
	}
	state.srFilterRanges.emplace(newStart, newEnd);
}

void SwitchNode::TrimLr2SentryMissingRanges(Lr2FlowState& state, uint64_t expectedSeq) {
	auto it = state.srFilterRanges.begin();
	while (it != state.srFilterRanges.end()) {
		if (it->second <= expectedSeq) {
			it = state.srFilterRanges.erase(it);
			continue;
		}
		if (it->first < expectedSeq) {
			uint64_t endSeq = it->second;
			it = state.srFilterRanges.erase(it);
			state.srFilterRanges.emplace(expectedSeq, endSeq);
		}
		break;
	}
}

bool SwitchNode::ConsumeLr2SentryMissingSeq(
	Lr2FlowState& state,
	uint64_t seq,
	uint32_t stepBytes) {
	auto it = state.srFilterRanges.upper_bound(seq);
	if (it == state.srFilterRanges.begin()) {
		return false;
	}
	--it;
	if (it->second <= seq) {
		return false;
	}

	uint32_t step = std::max<uint32_t>(stepBytes, 1);
	uint64_t pktEnd = seq + step;
	uint64_t rangeStart = it->first;
	uint64_t rangeEnd = it->second;
	state.srFilterRanges.erase(it);

	if (rangeStart < seq) {
		state.srFilterRanges.emplace(rangeStart, seq);
	}
	if (pktEnd < rangeEnd) {
		state.srFilterRanges.emplace(pktEnd, rangeEnd);
	}
	return true;
}

void SwitchNode::ResetLr2SentryState(Lr2FlowState& state, uint64_t expectedSeq) {
	state.expectedSeq = expectedSeq;
	state.expectedSeqD = expectedSeq;
	state.srFilterRanges.clear();
	state.outstandingNackSeqs.clear();
	state.lastNackTime = Time(0);
	state.lastNackSeq = std::numeric_limits<uint64_t>::max();
}

void SwitchNode::ClearLr2DepotPools(Lr2FlowState& state) {
	if (m_lr2ReorderPoolCurPkts >= state.reorderPool.size()) {
		m_lr2ReorderPoolCurPkts -= state.reorderPool.size();
	} else {
		m_lr2ReorderPoolCurPkts = 0;
	}
	if (m_lr2BackupPoolCurPkts >= state.backupPool.size()) {
		m_lr2BackupPoolCurPkts -= state.backupPool.size();
	} else {
		m_lr2BackupPoolCurPkts = 0;
	}

	state.reorderPool.clear();
	state.backupPool.clear();
	state.backupOrder.clear();
	state.outstandingNackSeqs.clear();
	state.lastNackTime = Time(0);
	state.lastNackSeq = std::numeric_limits<uint64_t>::max();
	state.retState = false;
	state.recoverSeq = 0;
}

bool SwitchNode::IsThemisPnpDataPacket(const CustomHeader &ch) const {
	return ch.l3Prot == 0x11 &&
		   ch.wanOptInfo.isDigestPacket == 0 &&
		   ch.wanOptInfo.isFakePacket == 0 &&
		   ch.wanOptInfo.isRepeatPacket == 0;
}

uint64_t SwitchNode::BuildThemisFlowKey(const CustomHeader &ch) const {
	return ((uint64_t)ch.dip << 32) | ((uint64_t)ch.udp.sport << 16) | (uint64_t)ch.udp.pg;
}

void SwitchNode::SendThemisPnpCnp(const CustomHeader &ch, uint32_t inDev) {
	CnHeader cnpHeader;
	cnpHeader.SetQindex(ch.udp.pg);
	cnpHeader.SetFlow(ch.udp.sport);
	cnpHeader.SetECNBits((uint8_t)Ipv4Header::ECN_CE);
	cnpHeader.SetQfb(1);
	cnpHeader.SetTotal(1);

	Ptr<Packet> cnpPkt = Create<Packet>(std::max(60 - 14 - 20 - (int)cnpHeader.GetSerializedSize(), 0));
	cnpPkt->AddHeader(cnpHeader);

	Ipv4Header ipHeader;
	ipHeader.SetDestination(Ipv4Address(ch.sip));
	ipHeader.SetSource(Ipv4Address(ch.dip));
	ipHeader.SetProtocol(0xFF);
	ipHeader.SetTtl(64);
	ipHeader.SetPayloadSize(cnpPkt->GetSize());
	ipHeader.SetIdentification(0);
	ipHeader.SetTos(0);
	cnpPkt->AddHeader(ipHeader);

	PppHeader ppp;
	ppp.SetProtocol(0x0021);
	cnpPkt->AddHeader(ppp);

	SetFlowIdTag(cnpPkt, inDev);

	CustomHeader cnpCh;
	cnpCh.sip = ch.dip;
	cnpCh.dip = ch.sip;
	cnpCh.l3Prot = 0xFF;
	cnpCh.cnp.qIndex = ch.udp.pg;
	cnpCh.cnp.fid = ch.udp.sport;
	cnpCh.cnp.ecnBits = (uint8_t)Ipv4Header::ECN_CE;
	cnpCh.cnp.qfb = 1;
	cnpCh.cnp.total = 1;

	SendToDev(cnpPkt, cnpCh);
}

void SwitchNode::MaybeHandleThemisPnp(uint32_t ifIndex, uint32_t qIndex, uint32_t inDev, Ptr<Packet> p) {
	if (!m_themisEnabled || !m_themisPnpEnabled || qIndex == 0 || !IsWanPort(ifIndex)) {
		return;
	}

	CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
	ch.getInt = 0;
	p->PeekHeader(ch);
	if (!IsThemisPnpDataPacket(ch)) {
		return;
	}
	if (ch.GetIpv4EcnBits() != Ipv4Header::ECN_CE) {
		return;
	}

	uint64_t flowKey = BuildThemisFlowKey(ch);
	Time now = Simulator::Now();
	auto it = m_themisPnpCache.find(flowKey);
	if (it == m_themisPnpCache.end() || (now - it->second) >= m_themisPnpCnpInterval) {
		SendThemisPnpCnp(ch, inDev);
		m_themisPnpCache[flowKey] = now;
		m_themisPnpSent++;
	} else {
		m_themisPnpCacheHit++;
	}

	PppHeader ppp;
	Ipv4Header ipHeader;
	p->RemoveHeader(ppp);
	p->RemoveHeader(ipHeader);
	ipHeader.SetEcn(Ipv4Header::ECN_NotECT);
	p->AddHeader(ipHeader);
	p->AddHeader(ppp);
	m_themisPnpEcnCleared++;
}

/**
 * Record a packet into the Depot's backupPool (LR2 Section III.B.2).
 * Called when Depot forwards data to the destination host.
 * The backupPool caches recent packets for link-level retransmission within
 * the downstream DC. Eviction is FIFO when pool exceeds m_lr2BackupWindowPkts.
 */
void SwitchNode::RecordLr2BackupPacket(
	Lr2FlowState& state,
	uint64_t seq,
	Ptr<Packet> packet,
	const CustomHeader& ch,
	uint32_t payloadSize) {
	if (state.backupPool.find(seq) == state.backupPool.end()) {
		state.backupOrder.push_back(seq);
		m_lr2BackupPoolCurPkts++;
		m_lr2BackupPoolPeakPkts =
			std::max(m_lr2BackupPoolPeakPkts, m_lr2BackupPoolCurPkts);
		RefreshSwitchRecoveryBufferPeak();
	}
	state.backupPool[seq] = {packet->Copy(), ch, payloadSize};
	while (state.backupOrder.size() > m_lr2BackupWindowPkts) {
		uint64_t oldestSeq = state.backupOrder.front();
		state.backupOrder.pop_front();
		if (state.backupPool.erase(oldestSeq) > 0 && m_lr2BackupPoolCurPkts > 0) {
			m_lr2BackupPoolCurPkts--;
		}
	}
}

/**
 * Construct and send a switch-NACK (Depot-NACK or Sentry-NACK) to the source NIC.
 * The NACK carries FLAG_SWITCH_NACK so the source NIC handles it differently
 * from a host-NACK: it starts corrective GBN from the indicated PSN without
 * updating lACK (snd_una), per LR2 Section III.D.
 */
void SwitchNode::SendLr2SwitchNack(
	const FiveTuple& flow,
	uint64_t lossPsn,
	uint16_t gapPkts,
	uint32_t inDev) {
	gapPkts = std::max<uint16_t>(gapPkts, 1);
	qbbHeader seqh;
	seqh.SetSeq(lossPsn);
	seqh.SetPG(flow.pg);
	seqh.SetSport(flow.dport);
	seqh.SetDport(flow.sport);
	seqh.SetFlags(
		static_cast<uint16_t>((gapPkts << 2) | (1 << qbbHeader::FLAG_SWITCH_NACK)));

	Ptr<Packet> nackPkt = Create<Packet>(std::max(0, 60 - 42));
	nackPkt->AddHeader(seqh);

	Ipv4Header ipHeader;
	ipHeader.SetDestination(Ipv4Address(flow.sip));
	ipHeader.SetSource(Ipv4Address(flow.dip));
	ipHeader.SetProtocol(0xFD);
	ipHeader.SetTtl(64);
	ipHeader.SetPayloadSize(nackPkt->GetSize());
	ipHeader.SetIdentification(0);
	ipHeader.SetTos(0);
	nackPkt->AddHeader(ipHeader);

	PppHeader ppp;
	ppp.SetProtocol(0x0021);
	nackPkt->AddHeader(ppp);

	SetFlowIdTag(nackPkt, inDev);

	CustomHeader nackCh;
	nackCh.sip = flow.dip;
	nackCh.dip = flow.sip;
	nackCh.l3Prot = 0xFD;
	nackCh.ack.sport = flow.dport;
	nackCh.ack.dport = flow.sport;
	nackCh.ack.seq = lossPsn;
	nackCh.ack.pg = flow.pg;
	nackCh.ack.flags =
		static_cast<uint16_t>((gapPkts << 2) | (1 << qbbHeader::FLAG_SWITCH_NACK));

	if (Lr2DebugEnabled() && flow.sport == 10000 && Lr2DebugPortMatch(flow.dport)) {
		std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
			<< " node=" << GetId() << " send_switch_nack"
			<< " flow=" << Lr2DebugNodeFromIp(flow.sip) << "->" << Lr2DebugNodeFromIp(flow.dip)
			<< " sport=" << flow.sport << " dport=" << flow.dport
			<< " loss=" << lossPsn << " gapPkts=" << gapPkts
			<< " inDev=" << inDev << std::endl;
	}
	SendToDev(nackPkt, nackCh);
}

void SwitchNode::SendLr2HostTypeNack(const FiveTuple& flow, uint64_t lossPsn, uint32_t inDev) {
	qbbHeader seqh;
	seqh.SetSeq(lossPsn);
	seqh.SetPG(flow.pg);
	seqh.SetSport(flow.dport);
	seqh.SetDport(flow.sport);

	Ptr<Packet> nackPkt = Create<Packet>(std::max(0, 60 - 42));
	nackPkt->AddHeader(seqh);

	Ipv4Header ipHeader;
	ipHeader.SetDestination(Ipv4Address(flow.sip));
	ipHeader.SetSource(Ipv4Address(flow.dip));
	ipHeader.SetProtocol(0xFD);
	ipHeader.SetTtl(64);
	ipHeader.SetPayloadSize(nackPkt->GetSize());
	ipHeader.SetIdentification(0);
	ipHeader.SetTos(0);
	nackPkt->AddHeader(ipHeader);

	PppHeader ppp;
	ppp.SetProtocol(0x0021);
	nackPkt->AddHeader(ppp);

	SetFlowIdTag(nackPkt, inDev);

	CustomHeader nackCh;
	nackCh.sip = flow.dip;
	nackCh.dip = flow.sip;
	nackCh.l3Prot = 0xFD;
	nackCh.ack.sport = flow.dport;
	nackCh.ack.dport = flow.sport;
	nackCh.ack.seq = lossPsn;
	nackCh.ack.pg = flow.pg;
	nackCh.ack.flags = 0;

	if (Lr2DebugEnabled() && flow.sport == 10000 && Lr2DebugPortMatch(flow.dport)) {
		std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
			<< " node=" << GetId() << " send_host_type_nack"
			<< " flow=" << Lr2DebugNodeFromIp(flow.sip) << "->" << Lr2DebugNodeFromIp(flow.dip)
			<< " sport=" << flow.sport << " dport=" << flow.dport
			<< " loss=" << lossPsn << " inDev=" << inDev << std::endl;
	}
	SendToDev(nackPkt, nackCh);
}

void SwitchNode::SendLr2SyntheticAck(const FiveTuple& flow, uint64_t ackSeq, uint32_t inDev) {
	qbbHeader seqh;
	seqh.SetSeq(ackSeq);
	seqh.SetPG(flow.pg);
	seqh.SetSport(flow.dport);
	seqh.SetDport(flow.sport);
	seqh.SetFlags(0);

	Ptr<Packet> ackPkt = Create<Packet>(std::max(0, 60 - 42));
	ackPkt->AddHeader(seqh);

	Ipv4Header ipHeader;
	ipHeader.SetDestination(Ipv4Address(flow.sip));
	ipHeader.SetSource(Ipv4Address(flow.dip));
	ipHeader.SetProtocol(0xFC);
	ipHeader.SetTtl(64);
	ipHeader.SetPayloadSize(ackPkt->GetSize());
	ipHeader.SetIdentification(0);
	ipHeader.SetTos(0);
	ackPkt->AddHeader(ipHeader);

	PppHeader ppp;
	ppp.SetProtocol(0x0021);
	ackPkt->AddHeader(ppp);

	SetFlowIdTag(ackPkt, inDev);

	CustomHeader ackCh;
	ackCh.sip = flow.dip;
	ackCh.dip = flow.sip;
	ackCh.l3Prot = 0xFC;
	ackCh.ack.sport = flow.dport;
	ackCh.ack.dport = flow.sport;
	ackCh.ack.seq = ackSeq;
	ackCh.ack.pg = flow.pg;
	ackCh.ack.flags = 0;

	if ((Lr2DebugEnabled() || Lr2DebugSnapshotEnabled()) &&
		flow.sport == 10000 && Lr2DebugPortMatch(flow.dport)) {
		std::cout << "[LR2FIX] t=" << Simulator::Now().GetTimeStep()
			<< " node=" << GetId() << " send_synthetic_ack"
			<< " flow=" << Lr2DebugNodeFromIp(flow.sip) << "->" << Lr2DebugNodeFromIp(flow.dip)
			<< " sport=" << flow.sport << " dport=" << flow.dport
			<< " ack=" << ackSeq << " inDev=" << inDev << std::endl;
	}
	SendToDev(ackPkt, ackCh);
}

/**
 * Flush the Depot's reorderPool: forward buffered packets to the destination
 * host in sequence order, starting from state.expectedSeq.
 * Stops when a gap is found (the next expected seq is not in the pool).
 * If stopped at a gap, sends a switch-NACK for that seq to trigger repair.
 *
 * Called after:
 * - An in-order packet arrives and fills a gap (normal flush)
 * - retState ends and ack_seq catches up to recover_seq (post-recovery flush)
 */
void SwitchNode::FlushLr2DepotBuffered(
	uint32_t inDev,
	Lr2FlowState& state,
	const FiveTuple& flow) {
	while (true) {
		auto it = state.reorderPool.find(state.expectedSeq);
		if (it == state.reorderPool.end()) {
			// Paper-consistent behavior: a flush stall inside the current
			// [ePSN, ePSN_D) window does NOT generate a fresh Depot-NACK.
			// Algorithm 1 only emits a new Depot-NACK when an arriving OOO packet
			// crosses ePSN_D (pkt.PSN > ePSN_D). Interval-internal repair-losses
			// therefore fall back to later cross-window signals or timeout-based
			// recovery, rather than an immediate stall-triggered re-NACK.
			return;
		}

		Lr2BufferedPacket buffered = it->second;
		state.reorderPool.erase(it);
		if (m_lr2ReorderPoolCurPkts > 0) {
			m_lr2ReorderPoolCurPkts--;
		}
		state.outstandingNackSeqs.erase(buffered.ch.udp.seq);

		// Per paper Algo.2: copy to backupPool when forwarding to host
		RecordLr2BackupPacket(state, buffered.ch.udp.seq, buffered.packet, buffered.ch, buffered.payloadSize);
		SetFlowIdTag(buffered.packet, inDev);
		SendToDev(buffered.packet, buffered.ch);
		state.expectedSeq += buffered.payloadSize;
		state.expectedSeqD = std::max(state.expectedSeqD, state.expectedSeq);
	}
}

/**
 * LR2 Sentry module; runs on the near-sender DCI switch (switch 0).
 * Ref: LR2 Section III.C.
 *
 * Processes data packets BEFORE they enter the WAN. Three responsibilities:
 *   1. Near-sender direct feedback (Section III.C.1):
 *      Only allow PSN == ePSN_s through; OOO packets are dropped and a
 *      Sentry-NACK is sent to the source NIC for fast retransmission.
 *   2. SR filter (Section III.C.2):
 *      Repeat (retransmission) packets are checked against the missing
 *      sequence ranges carried by Depot-NACKs. Matching seqs pass; others
 *      are dropped as redundant GBN retransmissions to save WAN bandwidth.
 *   3. RNIC timeout compatibility (Section III.C.3):
 *      When a non-repeat packet with seq < ePSN_s arrives, it indicates
 *      NIC GBN/RTO recovery. Sentry resets ePSN_s and clears the SR filter state.
 *
 * Returns true if the packet is intercepted (dropped); false if it should
 * continue to the WAN.
 */
bool SwitchNode::TryHandleLr2Sentry(
	uint32_t inDev,
	int outPort,
	Ptr<Packet> packet,
	CustomHeader& ch) {
	if (!IsLr2Enabled() || outPort < 0 || !IsWanPort(outPort) || IsWanPort(inDev) ||
		!IsReliabilityDataPacket(ch)) {
		return false;
	}

	FiveTuple flow = BuildFlowKeyFromUdp(ch);
	Lr2FlowState& state = m_lr2SentryState[flow];
	uint32_t payloadSize = std::max<uint32_t>(GetPayloadSize(packet, ch), 1);
	bool isRepeat = IsRepeatPacket(ch);
	bool lr2DebugData = Lr2DebugDataPacket(ch);
	if (lr2DebugData) {
		std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
			<< " node=" << GetId() << " sentry_data_enter"
			<< " flow=" << Lr2DebugNodeFromIp(ch.sip) << "->" << Lr2DebugNodeFromIp(ch.dip)
			<< " sport=" << ch.udp.sport << " dport=" << ch.udp.dport
			<< " seq=" << ch.udp.seq << " exp=" << state.expectedSeq
			<< " expD=" << state.expectedSeqD << " repeat=" << (int)isRepeat
			<< " ranges=" << state.srFilterRanges.size() << std::endl;
	}

	if (ch.udp.seq == state.expectedSeq) {
		if (lr2DebugData) {
			std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
				<< " node=" << GetId() << " sentry_pass_inorder"
				<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq
				<< " exp=" << state.expectedSeq << std::endl;
		}
		state.outstandingNackSeqs.erase(ch.udp.seq);
		state.expectedSeq += payloadSize;
		state.expectedSeqD = state.expectedSeq;
		TrimLr2SentryMissingRanges(state, state.expectedSeq);
		return false;
	}

	if (ch.udp.seq > state.expectedSeq) {
		Time now = Simulator::Now();
		if ((now - state.lastNackTime) >= m_lr2NackInterval ||
			state.lastNackSeq != state.expectedSeq) {
			uint16_t gapPkts =
				ComputeLr2GapPkts(state.expectedSeq, ch.udp.seq, payloadSize);
			if (lr2DebugData) {
				std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
					<< " node=" << GetId() << " sentry_gap_drop"
					<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq
					<< " loss=" << state.expectedSeq << " gapPkts=" << gapPkts << std::endl;
			}
			SendLr2SwitchNack(flow, state.expectedSeq, gapPkts, inDev);
			state.outstandingNackSeqs.insert(state.expectedSeq);
			state.lastNackTime = now;
			state.lastNackSeq = state.expectedSeq;
		}
		return true;
	}

	if (!isRepeat) {
		ResetLr2SentryState(state, ch.udp.seq + payloadSize);
		return false;
	}

	if (ConsumeLr2SentryMissingSeq(state, ch.udp.seq, payloadSize)) {
		if (lr2DebugData) {
			std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
				<< " node=" << GetId() << " sentry_pass_repeat"
				<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq << std::endl;
		}
		state.outstandingNackSeqs.erase(ch.udp.seq);
		return false;
	}
	if (lr2DebugData) {
		std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
			<< " node=" << GetId() << " sentry_drop_repeat"
			<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq << std::endl;
	}
	return true;
}

/**
 * LR2 Depot module; runs on the near-receiver DCI switch (switch 1).
 * Ref: LR2 Section III.B, Algorithm 1.
 *
 * Processes data packets arriving FROM the WAN. Implements:
 *   - OOO buffering in reorderPool with dual-register tracking (Section III.B.1)
 *   - In-order forwarding with backupPool caching (Section III.B.2)
 *   - RNIC timeout compatibility via P1 (Section III.B.3, Algo.1 L4-7)
 *   - Temporary reordering mode during backup retransmission (Fig. 4)
 *
 * Returns true if the packet is consumed by Depot (buffered or forwarded
 * internally); false if it should continue normal switch forwarding.
 */
bool SwitchNode::TryHandleLr2DepotData(
	uint32_t inDev,
	Ptr<Packet> packet,
	CustomHeader& ch) {
	if (!IsLr2Enabled() || !IsWanPort(inDev) || !IsReliabilityDataPacket(ch)) {
		return false;
	}

	FiveTuple flow = BuildFlowKeyFromUdp(ch);
	Lr2FlowState& state = m_lr2DepotState[flow];
	uint32_t payloadSize = std::max<uint32_t>(GetPayloadSize(packet, ch), 1);
	state.wanInDev = inDev;
	bool lr2DebugData = Lr2DebugDataPacket(ch);
	if (lr2DebugData) {
		std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
			<< " node=" << GetId() << " depot_data_enter"
			<< " flow=" << Lr2DebugNodeFromIp(ch.sip) << "->" << Lr2DebugNodeFromIp(ch.dip)
			<< " sport=" << ch.udp.sport << " dport=" << ch.udp.dport
			<< " seq=" << ch.udp.seq << " exp=" << state.expectedSeq
			<< " expD=" << state.expectedSeqD << " ack=" << state.ackSeq
			<< " ret=" << state.retState << " rec=" << state.recoverSeq
			<< " reorder=" << state.reorderPool.size() << " backup=" << state.backupPool.size()
			<< " repeat=" << (int)IsRepeatPacket(ch) << std::endl;
	}

	// NOTE: Per paper Algorithm 1, backupPool records happen only when
	// forwarding to host (in-order path + flush path), NOT on every WAN arrival.
	// Moved RecordLr2BackupPacket from here to the forwarding points below.

	// --- Algo.1 L4-7: P1 RNIC timeout compatibility (Section III.B.3) ---
	// In this codebase, ackSeq stores the host's cumulative ACK as the next
	// expected byte offset. Therefore:
	//   seq <  ackSeq  => already ACKed old data (RNIC timeout / host-NACK replay)
	//   seq == ackSeq  => the current missing packet, which is a valid repair
	//                     packet and must NOT trigger P1.
	// If ACKs were lost on WAN, the sender's snd_una may lag behind Depot's
	// ackSeq, and an RNIC timeout retransmission arrives with seq < ackSeq.
	// Depot then resets state to re-align with the RNIC. The reset must not
	// move ePSN behind ackSeq; bytes below ackSeq have already been accepted
	// by the destination RNIC and waiting for them again can deadlock Depot.
	// WARNING: This clears the entire reorderPool; all buffered OOO packets
	// are discarded. In high-BDP WAN this is very costly.
	if (state.ackSeq > 0 && ch.udp.seq < state.ackSeq) {
		uint64_t resetSeq =
			std::max<uint64_t>(ch.udp.seq + payloadSize, state.ackSeq);
		if (lr2DebugData) {
			std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
				<< " node=" << GetId() << " depot_p1_reset"
				<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq
				<< " oldExp=" << state.expectedSeq << " oldAck=" << state.ackSeq
				<< " resetSeq=" << resetSeq << std::endl;
		}
		state.expectedSeq = resetSeq;
		state.expectedSeqD = state.expectedSeq;
		ClearLr2DepotPools(state);
		state.lastBackupPoolRefresh = Simulator::Now();
		if (ch.udp.seq + payloadSize <= state.ackSeq) {
			// The receiver has already cumulatively ACKed this byte range.
			// If the original ACK was lost on the WAN, the source RNIC will RTO
			// and replay old data. Dropping that replay silently deadlocks the QP,
			// so Depot refreshes the source with the known cumulative ACK.
			SendLr2SyntheticAck(flow, state.ackSeq, inDev);
			return true;
		}
		if (lr2DebugData) {
			std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
				<< " node=" << GetId() << " depot_forward_inorder"
				<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq
				<< " exp=" << state.expectedSeq << " ack=" << state.ackSeq << std::endl;
		}
		RecordLr2BackupPacket(state, ch.udp.seq, packet, ch, payloadSize);
		SetFlowIdTag(packet, inDev);
		SendToDev(packet, ch);
		return true;
	}

	// Duplicate packet (seq < expectedSeq but not P1): already seen, discard.
	if (ch.udp.seq < state.expectedSeq) {
		if (lr2DebugData) {
			std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
				<< " node=" << GetId() << " depot_duplicate_drop"
				<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq
				<< " exp=" << state.expectedSeq << " ack=" << state.ackSeq << std::endl;
		}
		state.outstandingNackSeqs.erase(ch.udp.seq);
		return true;
	}

	// --- Algo.1 L8-11: In-order packet (PSN == ePSN) ---
	if (ch.udp.seq == state.expectedSeq) {
		state.outstandingNackSeqs.erase(ch.udp.seq);
		state.lastBackupPoolRefresh = Simulator::Now();

		// P0 / Fig.4 "reordering mode (temporary)": during backup pool
		// retransmission recovery, buffer even in-order packets in
		// reorderPool. Only flush when ack_seq reaches recover_seq.
		if (state.retState) {
			if (lr2DebugData) {
				std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
					<< " node=" << GetId() << " depot_ret_buffer"
					<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq
					<< " exp=" << state.expectedSeq << " ack=" << state.ackSeq
					<< " rec=" << state.recoverSeq << std::endl;
			}
			if (state.reorderPool.find(ch.udp.seq) == state.reorderPool.end()) {
				m_lr2ReorderPoolCurPkts++;
				m_lr2ReorderPoolPeakPkts =
					std::max(m_lr2ReorderPoolPeakPkts, m_lr2ReorderPoolCurPkts);
				RefreshSwitchRecoveryBufferPeak();
			}
			state.reorderPool[ch.udp.seq] = {packet->Copy(), ch, payloadSize};
			state.expectedSeq += payloadSize;
			state.expectedSeqD = std::max(state.expectedSeqD, state.expectedSeq);
			if (state.ackSeq >= state.recoverSeq) {
				state.retState = false;
				FlushLr2DepotBuffered(inDev, state, flow);
			}
			return true;
		}

		RecordLr2BackupPacket(state, ch.udp.seq, packet, ch, payloadSize);
		SetFlowIdTag(packet, inDev);
		SendToDev(packet, ch);
		state.expectedSeq += payloadSize;
		// Sync ePSN_D with ePSN when in-order
		state.expectedSeqD = std::max(state.expectedSeqD, state.expectedSeq);
		FlushLr2DepotBuffered(inDev, state, flow);
		return true;
	}

	// --- Algo.1 L12-16: OOO packet (PSN > ePSN) ---
	// Buffer in reorderPool; generate Depot-NACK using dual-register logic.
	if (state.reorderPool.find(ch.udp.seq) == state.reorderPool.end()) {
		if (state.reorderPool.size() < m_lr2ReorderWindowPkts) {
			m_lr2ReorderPoolCurPkts++;
			m_lr2ReorderPoolPeakPkts =
				std::max(m_lr2ReorderPoolPeakPkts, m_lr2ReorderPoolCurPkts);
			RefreshSwitchRecoveryBufferPeak();
			state.reorderPool[ch.udp.seq] = {packet->Copy(), ch, payloadSize};
			if (lr2DebugData) {
				std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
					<< " node=" << GetId() << " depot_buffer_ooo"
					<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq
					<< " exp=" << state.expectedSeq << " expD=" << state.expectedSeqD
					<< " reorder=" << state.reorderPool.size() << std::endl;
			}
		} else {
			Time now = Simulator::Now();
			if ((now - state.lastNackTime) >= m_lr2NackInterval ||
				state.lastNackSeq != state.expectedSeq) {
				SendLr2HostTypeNack(flow, state.expectedSeq, inDev);
				state.lastNackTime = now;
				state.lastNackSeq = state.expectedSeq;
			}
			return true;
		}
	}

	// --- Dual-register parallel NACK (Section III.B.1, Fig. 5) ---
	// ePSN_D tracks the upper edge of the OOO window independently of ePSN.
	// This allows detecting NEW gaps in the OOO region without waiting for
	// the first gap (at ePSN) to be filled, but only when an arriving packet
	// crosses ePSN_D.  The paper does not define an extra re-NACK path for
	// gap-internal repair losses within the already reported [ePSN, ePSN_D)
	// interval.
	if (ch.udp.seq == state.expectedSeqD) {
		// Algo.1 L12: packet matches ePSN_D; no new gap, advance ePSN_D.
		if (lr2DebugData) {
			std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
				<< " node=" << GetId() << " depot_advance_expD"
				<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq
				<< " oldExpD=" << state.expectedSeqD
				<< " newExpD=" << (state.expectedSeqD + payloadSize) << std::endl;
		}
		state.expectedSeqD += payloadSize;
	} else if (ch.udp.seq > state.expectedSeqD) {
		// Algo.1 L13-14: packet skipped ePSN_D; new gap at ePSN_D, NACK it.
		if (state.outstandingNackSeqs.count(state.expectedSeqD) == 0) {
			uint16_t gapPkts =
				ComputeLr2GapPkts(state.expectedSeqD, ch.udp.seq, payloadSize);
			if (lr2DebugData) {
				std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
					<< " node=" << GetId() << " depot_new_gap"
					<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq
					<< " loss=" << state.expectedSeqD << " gapPkts=" << gapPkts << std::endl;
			}
			SendLr2SwitchNack(flow, state.expectedSeqD, gapPkts, inDev);
			state.outstandingNackSeqs.insert(state.expectedSeqD);
		} else if (lr2DebugData) {
			std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
				<< " node=" << GetId() << " depot_gap_already_outstanding"
				<< " dport=" << ch.udp.dport << " seq=" << ch.udp.seq
				<< " loss=" << state.expectedSeqD << std::endl;
		}
		state.expectedSeqD = ch.udp.seq + payloadSize;
	}
	// seq < expectedSeqD: OOO packet falls inside the existing window.
	// Per Algorithm 1, this does not trigger a new Depot-NACK.
	return true;
}

/**
 * Intercept host-NACK at Depot (Section III.B.2).
 * When the destination NIC sends a NACK (due to DC-internal loss or timeout),
 * Depot tries to retransmit the requested packet from backupPool locally,
 * avoiding a costly round-trip through WAN. If the packet is not in backupPool,
 * the NACK is forwarded to the source NIC for end-to-end recovery.
 *
 * In our lossless-DC simulation, host-NACKs are rare (DC has no loss), so
 * this function mostly serves as a passthrough.
 */
bool SwitchNode::TryHandleLr2HostNack(
	uint32_t inDev,
	int outPort,
	CustomHeader& ch) {
	if (!IsLr2Enabled() || ch.l3Prot != 0xFD || outPort < 0 || !IsWanPort(outPort) ||
		IsWanPort(inDev)) {
		return false;
	}

	if (((ch.ack.flags >> qbbHeader::FLAG_SWITCH_NACK) & 1) != 0) {
		return false;
	}

	FiveTuple flow = BuildFlowKeyFromAck(ch);
	auto depotIt = m_lr2DepotState.find(flow);
	if (depotIt == m_lr2DepotState.end()) {
		return false;
	}

	Lr2FlowState& state = depotIt->second;
	state.lastBackupPoolRefresh = Simulator::Now();
	uint64_t recoverStart = std::max<uint64_t>(state.ackSeq, ch.ack.seq);
	auto backupIt = state.backupPool.find(recoverStart);
	if (backupIt == depotIt->second.backupPool.end()) {
		return false;
	}

	if (state.retState) {
		Lr2BufferedPacket buffered = backupIt->second;
		SetFlowIdTag(buffered.packet, inDev);
		SendToDev(buffered.packet, buffered.ch);
		return true;
	}
	Lr2DepotRetransmitFromBackup(state, flow, inDev, recoverStart);
	return true;
}

void SwitchNode::TryResetLr2SentryOnHostNack(uint32_t inDev, CustomHeader& ch) {
	if (!IsLr2Enabled() || ch.l3Prot != 0xFD || !IsWanPort(inDev)) {
		return;
	}
	if (((ch.ack.flags >> qbbHeader::FLAG_SWITCH_NACK) & 1) != 0) {
		return;
	}

	FiveTuple flow = BuildFlowKeyFromAck(ch);
	ResetLr2SentryState(m_lr2SentryState[flow], ch.ack.seq);
}

/**
 * Process ACK packets passing through Depot (Algo.1 L19-23).
 * ACKs flow from destination host -> Depot -> WAN -> Sentry -> source NIC.
 * Depot extracts ack_seq (cumulative ACK from host) to:
 *   1. Track acknowledged progress for P1 timeout detection
 *   2. Refresh backup pool timer (prevents premature timeout retransmission)
 *   3. End retState recovery when ack_seq reaches recover_seq
 *   4. Clean up acknowledged entries from backupPool
 *
 * Returns false: ACK is NOT intercepted, continues to source NIC.
 */
bool SwitchNode::TryHandleLr2DepotAck(uint32_t inDev, CustomHeader& ch) {
	if (!IsLr2Enabled() || ch.l3Prot != 0xFC || IsWanPort(inDev)) {
		return false;
	}

	FiveTuple flow = BuildFlowKeyFromAck(ch);
	auto it = m_lr2DepotState.find(flow);
	if (it == m_lr2DepotState.end()) return false;

	Lr2FlowState& state = it->second;
	uint64_t oldAckSeq = state.ackSeq;
	state.ackSeq = std::max<uint64_t>(state.ackSeq, ch.ack.seq);
	if (Lr2DebugAckPacket(ch) && state.ackSeq != oldAckSeq) {
		std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
			<< " node=" << GetId() << " depot_ack"
			<< " flow=" << Lr2DebugNodeFromIp(ch.dip) << "->" << Lr2DebugNodeFromIp(ch.sip)
			<< " dport=" << ch.ack.sport << " ack=" << ch.ack.seq
			<< " oldAck=" << oldAckSeq << " newAck=" << state.ackSeq
			<< " exp=" << state.expectedSeq << " expD=" << state.expectedSeqD
			<< " ret=" << state.retState << " rec=" << state.recoverSeq
			<< " reorder=" << state.reorderPool.size() << " backup=" << state.backupPool.size()
			<< std::endl;
	}
	state.lastBackupPoolRefresh = Simulator::Now();

	// End retransmission state if ack_seq caught up
	if (state.retState && state.ackSeq >= state.recoverSeq) {
		state.retState = false;
		FlushLr2DepotBuffered(state.wanInDev, state, flow);
	}

	// Trim backupPool: remove packets already acknowledged
	while (!state.backupOrder.empty() && state.backupOrder.front() < state.ackSeq) {
		uint64_t oldSeq = state.backupOrder.front();
		state.backupOrder.pop_front();
		if (state.backupPool.erase(oldSeq) > 0 && m_lr2BackupPoolCurPkts > 0) {
			m_lr2BackupPoolCurPkts--;
		}
	}

	return false;  // ACK continues to source, not intercepted
}

/**
 * Periodic timer: check all Depot flows for backup pool timeout (Section III.B.2, Algo.2).
 * If a flow has been waiting for a lost packet longer than m_lr2BackupTimeout
 * (T_out) and is not already in retransmission recovery, trigger backup pool
 * retransmission: re-send [ack_seq, ePSN) from backupPool to the host.
 *
 * Tail-loss in the downstream DC is also covered: as long as backupPool keeps
 * outstanding packets and ACK progress stalls for T_out, Depot can trigger a
 * local retransmission without waiting for the NIC's 30ms RTO.
 */
void SwitchNode::CheckLr2DepotTimeout() {
	if (!IsLr2Enabled()) return;
	Time now = Simulator::Now();

	for (auto& [flow, state] : m_lr2DepotState) {
		if (state.backupPool.empty() || state.retState) continue;

		if (state.lastBackupPoolRefresh > Time(0) &&
			(now - state.lastBackupPoolRefresh) >= m_lr2BackupTimeout) {
			if (Lr2DebugEnabled() && flow.sport == 10000 && Lr2DebugPortMatch(flow.dport)) {
				std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
					<< " node=" << GetId() << " depot_timeout"
					<< " flow=" << Lr2DebugNodeFromIp(flow.sip) << "->" << Lr2DebugNodeFromIp(flow.dip)
					<< " dport=" << flow.dport << " start=" << state.ackSeq
					<< " exp=" << state.expectedSeq << " expD=" << state.expectedSeqD
					<< " reorder=" << state.reorderPool.size() << " backup=" << state.backupPool.size()
					<< std::endl;
			}
			Lr2DepotRetransmitFromBackup(state, flow, state.wanInDev, state.ackSeq);
		}
	}

	m_lr2DepotTimerEvent = Simulator::Schedule(
		m_lr2BackupTimeout, &SwitchNode::CheckLr2DepotTimeout, this);
}

/**
 * Execute backup pool retransmission (Algo.2 timeout path).
 * Sets retState=true, records recover_seq=ePSN, rolls back ePSN to ack_seq,
 * and retransmits all packets in backupPool within [ack_seq, recover_seq).
 * During retState, all arriving packets are buffered in reorderPool (even
 * in-order ones) until ack_seq catches up to recover_seq.
 */
void SwitchNode::Lr2DepotRetransmitFromBackup(
	Lr2FlowState& state, const FiveTuple& flow, uint32_t inDev, uint64_t startSeq) {
	startSeq = std::max<uint64_t>(startSeq, state.ackSeq);
	if (Lr2DebugEnabled() && flow.sport == 10000 && Lr2DebugPortMatch(flow.dport)) {
		std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
			<< " node=" << GetId() << " depot_retx_enter"
			<< " flow=" << Lr2DebugNodeFromIp(flow.sip) << "->" << Lr2DebugNodeFromIp(flow.dip)
			<< " dport=" << flow.dport << " start=" << startSeq
			<< " exp=" << state.expectedSeq << " ack=" << state.ackSeq
			<< " backup=" << state.backupPool.size() << std::endl;
	}
	if (startSeq >= state.expectedSeq) {
		return;
	}

	auto firstIt = state.backupPool.lower_bound(startSeq);
	if (firstIt == state.backupPool.end() || firstIt->first != startSeq) {
		if (Lr2DebugEnabled() && flow.sport == 10000 && Lr2DebugPortMatch(flow.dport)) {
			std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
				<< " node=" << GetId() << " depot_retx_missing_backup"
				<< " dport=" << flow.dport << " start=" << startSeq
				<< " first=" << (firstIt == state.backupPool.end() ? 0 : firstIt->first) << std::endl;
		}
		SendLr2HostTypeNack(flow, startSeq, inDev);
		return;
	}

	state.retState = true;
	state.recoverSeq = state.expectedSeq;
	state.expectedSeq = startSeq;
	state.expectedSeqD = std::max(state.expectedSeqD, state.recoverSeq);
	state.lastBackupPoolRefresh = Simulator::Now();

	uint32_t retxCount = 0;
	for (auto it = firstIt;
		 it != state.backupPool.end() && it->first < state.recoverSeq; ++it) {
		Lr2BufferedPacket& buffered = it->second;
		Ptr<Packet> copy = buffered.packet->Copy();
		SetFlowIdTag(copy, inDev);
		SendToDev(copy, buffered.ch);
		retxCount++;
	}
	if (Lr2DebugEnabled() && flow.sport == 10000 && Lr2DebugPortMatch(flow.dport)) {
		std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
			<< " node=" << GetId() << " depot_retx_sent"
			<< " dport=" << flow.dport << " count=" << retxCount
			<< " recover=" << state.recoverSeq << " exp=" << state.expectedSeq
			<< std::endl;
	}
}

/**
 * Record Depot-NACK missing ranges into Sentry's SR filter state (Section III.C.2).
 * Called when a switch-NACK from the Depot (arriving via WAN) passes through
 * the Sentry on its way to the source NIC. The NACK carries
 * NACK(start_seq, gap_len), so Sentry records the corresponding missing range
 * and later forwards only repeat packets that fall inside one of these ranges.
 */
void SwitchNode::TryRecordLr2SentryFilter(uint32_t inDev, CustomHeader& ch) {
	if (!IsLr2Enabled() || ch.l3Prot != 0xFD || !IsWanPort(inDev)) return;
	if (((ch.ack.flags >> qbbHeader::FLAG_SWITCH_NACK) & 1) == 0) return;

	FiveTuple flow = BuildFlowKeyFromAck(ch);
	auto& state = m_lr2SentryState[flow];
	if (Lr2DebugAckPacket(ch)) {
		std::cout << "[LR2DBG] t=" << Simulator::Now().GetTimeStep()
			<< " node=" << GetId() << " sentry_record_filter"
			<< " flow=" << Lr2DebugNodeFromIp(flow.sip) << "->" << Lr2DebugNodeFromIp(flow.dip)
			<< " dport=" << flow.dport << " loss=" << ch.ack.seq
			<< " gapPkts=" << DecodeLr2GapPkts(ch.ack.flags) << std::endl;
	}
	RecordLr2SentryMissingRange(state, ch.ack.seq, DecodeLr2GapPkts(ch.ack.flags), m_mtu);
}

Ptr<Packet> SwitchNode::BuildDigestPacket() {
	uint32_t entryCount = m_aggregatedFlow.digestEntries.size();
	// Each entry stores sip(4), dip(4), sport(2), dport(2), originalPsn(8),
	// newPsn(8), payloadSize(4), flowId(4), and pg(4), for 40 bytes total.
	uint32_t digestPayloadSize = 4 + entryCount * DIGEST_ENTRY_SIZE;

    uint8_t *buffer = new uint8_t[digestPayloadSize];
    
    // Write the entry count first.
    *(uint32_t *)buffer = entryCount;
    
    // Write fixed-size digest entries.
    uint8_t *entries = buffer + 4;
    for (uint32_t i = 0; i < entryCount; i++) {
        uint32_t offset = i * DIGEST_ENTRY_SIZE;
        
        *(uint32_t *)(entries + offset) = m_aggregatedFlow.digestEntries[i].sip;
        *(uint32_t *)(entries + offset + 4) = m_aggregatedFlow.digestEntries[i].dip;
        *(uint16_t *)(entries + offset + 8) = m_aggregatedFlow.digestEntries[i].sport;
        *(uint16_t *)(entries + offset + 10) = m_aggregatedFlow.digestEntries[i].dport;
        *(uint64_t *)(entries + offset + 12) = m_aggregatedFlow.digestEntries[i].originalPsn;
        *(uint64_t *)(entries + offset + 20) = m_aggregatedFlow.digestEntries[i].newPsn;
        *(uint32_t *)(entries + offset + 28) = m_aggregatedFlow.digestEntries[i].payloadSize;
        *(uint32_t *)(entries + offset + 32) = m_aggregatedFlow.digestEntries[i].flowId;
        *(uint32_t *)(entries + offset + 36) = m_aggregatedFlow.digestEntries[i].pg;
    }
    
    Ptr<Packet> pkt = Create<Packet>(buffer, digestPayloadSize);
    delete[] buffer;

	WanOptHeader opt;
    opt.SetAggregated(true);
    opt.SetDigest(true);
    opt.SetNewPsn(0);
    opt.SetNextHeader(0xFF); // No upper-layer payload follows the digest.
    pkt->AddHeader(opt);

    // Use the first entry as the IP-header template for routing.
    Ipv4Header ip;
    ip.SetSource(Ipv4Address(m_aggregatedFlow.digestEntries.front().sip));
    ip.SetDestination(Ipv4Address(m_aggregatedFlow.digestEntries.front().dip));
    ip.SetProtocol(0xFA);
    ip.SetTtl(64);
    ip.SetPayloadSize(pkt->GetSize());
    pkt->AddHeader(ip);

    PppHeader ppp;
    ppp.SetProtocol(0x0021);
    pkt->AddHeader(ppp);

	return pkt;
}

void SwitchNode::SendSailDigestBatch(const CustomHeader &ch, uint32_t inDev) {
	if (m_aggregatedFlow.digestEntries.empty()) {
		return;
	}

	Ptr<Packet> digestPacket = BuildDigestPacket();
	CustomHeader digestCh = ch;

	digestCh.udp.seq = 0;
	digestCh.wanOptInfo.newPsn = 0;
	digestCh.wanOptInfo.isDigestPacket = 1;
	digestCh.wanOptInfo.isAggregated = 1;
	digestCh.m_payloadSize = m_mtu;

	SetFlowIdTag(digestPacket, inDev);

	for (uint32_t copy = 0; copy < m_sailDigestCopies; ++copy) {
		Ptr<Packet> digestToSend = (copy == 0) ? digestPacket : digestPacket->Copy();
		SendToDev(digestToSend, digestCh);
	}

	uint64_t batchStart = m_aggregatedFlow.digestEntries.front().newPsn;
	uint64_t batchStep =
		static_cast<uint64_t>(m_digestInterval) * static_cast<uint64_t>(m_mtu);
	uint64_t nextBatchPsn = batchStart + batchStep;
	if (m_aggregatedFlow.nextAggregatedPsn < nextBatchPsn) {
		m_aggregatedFlow.nextAggregatedPsn = nextBatchPsn;
	}

	m_aggregatedFlow.digestEntries.clear();
	m_aggregatedFlow.digestCounter = 0;
}

Ptr<Packet> SwitchNode::BuildFakePacket(const DigestEntry &entry) {
    uint32_t intHeaderSize = IntHeader::GetStaticSize();
    Ptr<Packet> pkt = Create<Packet>(entry.payloadSize + intHeaderSize);
    
    SimpleSeqTsHeader seqTs;
    seqTs.SetSeq(entry.originalPsn); 
    seqTs.SetPG((uint16_t)entry.pg);
    pkt->AddHeader(seqTs);

    UdpHeader udp;
    udp.SetSourcePort(entry.sport);
    udp.SetDestinationPort(entry.dport);
    pkt->AddHeader(udp);

    Ipv4Header ip;
    ip.SetSource(Ipv4Address(entry.sip));
    ip.SetDestination(Ipv4Address(entry.dip));
    ip.SetProtocol(0x11); // UDP
    ip.SetTtl(64);
    ip.SetTos(0x20); // SAIL fake-packet marker.
    ip.SetPayloadSize(pkt->GetSize());
    pkt->AddHeader(ip);

    PppHeader ppp;
    ppp.SetProtocol(0x0021);
    pkt->AddHeader(ppp);

    // Fake packet should carry the ingress port metadata expected by the switch.
    SetFlowIdTag(pkt, entry.flowId);

    return pkt;
}

bool SwitchNode::AggregateFlow(Ptr<Packet> packet, CustomHeader &ch, uint32_t inDev) {
    FiveTuple flowKey;
    flowKey.sip = ch.sip;
    flowKey.dip = ch.dip;
    flowKey.sport = ch.udp.sport;
    flowKey.dport = ch.udp.dport;
    flowKey.pg = ch.udp.pg;
    
    // Skip go-back-N retransmission packets that have already been aggregated.
    auto it = m_flowMaxAggSeq.find(flowKey);
    if (it != m_flowMaxAggSeq.end() && ch.udp.seq <= it->second) {
        NS_LOG_INFO("[Node 0] GoBackN Retransmission (seq=" << ch.udp.seq 
            << " <= " << it->second << "), skip aggregation");
        return false;
    }

	uint32_t payloadSize = std::max<uint32_t>(GetPayloadSize(packet, ch), 1);

    // Normal aggregation path.
    PppHeader ppp;
    packet->RemoveHeader(ppp);
    Ipv4Header ip;
    packet->RemoveHeader(ip);

    WanOptHeader opt;
    opt.SetAggregated(true);
    opt.SetNewPsn(m_aggregatedFlow.nextAggregatedPsn);
    opt.SetNextHeader(ip.GetProtocol());
    
    packet->AddHeader(opt);

    ip.SetProtocol(0xFA);
    ip.SetPayloadSize(ip.GetPayloadSize() + opt.GetSerializedSize());
    packet->AddHeader(ip);
    packet->AddHeader(ppp);

	DigestEntry entry;
    entry.sip = ch.sip;
    entry.dip = ch.dip;
    entry.sport = ch.udp.sport;
    entry.dport = ch.udp.dport;
    entry.pg = (uint32_t)ch.udp.pg;
    entry.originalPsn = ch.udp.seq;
    entry.newPsn = m_aggregatedFlow.nextAggregatedPsn;
	entry.payloadSize = payloadSize;
    entry.flowId = inDev;
    
    m_aggregatedFlow.digestEntries.push_back(entry);
    m_aggregatedFlow.nextAggregatedPsn += m_mtu;
    m_aggregatedFlow.digestCounter++;

    m_flowMaxAggSeq[flowKey] = ch.udp.seq;
    return true;
}

void SwitchNode::BufferAggregatedPacket(Ptr<Packet> packet, CustomHeader &ch, uint64_t aggregatedPsn) {
    uint64_t batchKey = ((aggregatedPsn / m_mtu) / m_digestInterval) * m_digestInterval * m_mtu;

    if (batchKey < m_nextSailBatchToRelease) {
        return;
    }

    auto batch_it = m_digestBatches.find(batchKey);
    if (batch_it == m_digestBatches.end()) {
        DigestBatch batch;
        batch.frontPsn = batchKey;
        batch.digestReceived = false;
		batch.digestProcessed = false;
		batch.rawFallbackReady = false;
		batch.digestReceivedTime = Time(0);
		batch.digestProcessEvent = EventId();
		batch.firstPacketArrivalTime = Simulator::Now();
        m_digestBatches[batchKey] = batch;
        batch_it = m_digestBatches.find(batchKey);
    }

    DigestBatch &batch = batch_it->second;
    if (batch.digestProcessed) {
        return;
    }

    BufferedAggregatedPacket buffered;
    buffered.packet = packet->Copy();
    buffered.ch = ch;
    buffered.aggregatedPsn = aggregatedPsn;
    buffered.isLost = false;

	    batch.packets.push_back(buffered);
	    m_sailBufferedCurPkts++;
	    m_sailBufferedPeakPkts =
	        std::max(m_sailBufferedPeakPkts, m_sailBufferedCurPkts);
	    RefreshSwitchRecoveryBufferPeak();

    if (!batch.digestProcessed && batch.packets.size() == m_digestInterval) {
        std::set<uint64_t> receivedPsns;
        for (const auto &pkt : batch.packets) {
            receivedPsns.insert(pkt.aggregatedPsn);
        }
        bool fullRawBatch = receivedPsns.size() == m_digestInterval;
        for (uint32_t i = 0; fullRawBatch && i < m_digestInterval; ++i) {
            uint64_t expectedPsn =
                batch.frontPsn + static_cast<uint64_t>(i) * static_cast<uint64_t>(m_mtu);
            if (receivedPsns.find(expectedPsn) == receivedPsns.end()) {
                fullRawBatch = false;
            }
        }
        if (fullRawBatch) {
            if (batch.digestProcessEvent.IsRunning()) {
                batch.digestProcessEvent.Cancel();
            }
            batch.digestProcessed = true;
            batch.rawFallbackReady = true;
            TryReleaseReadySailBatches();
            return;
        }
    }

    if (batch.digestReceived && !batch.digestProcessed) {
        MarkSailBatchReady(batchKey);
    }
}

void SwitchNode::ProcessDigestPacket(Ptr<Packet> packet, CustomHeader &ch) {
	// Packet layout: [PPP | IP | WanOpt | Digest Payload].
    PppHeader ppp;
    packet->RemoveHeader(ppp); 
    Ipv4Header ip;
    packet->RemoveHeader(ip);
    WanOptHeader opt;
    packet->RemoveHeader(opt);
    // Packet layout after stripping headers: [Digest Payload].

    uint8_t *buffer = packet->GetBuffer();
    uint32_t entryCount = *(uint32_t *)buffer;

	NS_LOG_INFO("Processing digest packet: entries=" << entryCount);

    std::vector<DigestEntry> entries;
    uint8_t *p_entries = buffer + 4;

    for (uint32_t i = 0; i < entryCount; i++) {
        uint32_t offset = i * DIGEST_ENTRY_SIZE;

        DigestEntry entry;
        entry.sip = *(uint32_t *)(p_entries + offset);
        entry.dip = *(uint32_t *)(p_entries + offset + 4);
        entry.sport = *(uint16_t *)(p_entries + offset + 8);
        entry.dport = *(uint16_t *)(p_entries + offset + 10);
        entry.originalPsn = *(uint64_t *)(p_entries + offset + 12);
        entry.newPsn = *(uint64_t *)(p_entries + offset + 20);
        entry.payloadSize = *(uint32_t *)(p_entries + offset + 28);
        entry.flowId = *(uint32_t *)(p_entries + offset + 32);
        entry.pg = *(uint32_t *)(p_entries + offset + 36);

        entries.push_back(entry);

        // NS_LOG_INFO("Digest entry " << i << ": "
        //     << entry.sip << "->" << entry.dip
        //     << ":" << entry.sport << "->" << entry.dport
        //     << " origPsn=" << entry.originalPsn
        //     << " newPsn=" << entry.newPsn
		// 	// << " payloadSize=" << entry.payloadSize
        //     // << " flowId=" << entry.flowId
        //     // << " pg=" << entry.pg
        // );
    }

	if (entries.empty()) {
        NS_LOG_WARN("Empty digest packet");
        return;
    }

    uint64_t batchKey = entries.front().newPsn;

    auto batch_it = m_digestBatches.find(batchKey);
    if (batch_it == m_digestBatches.end()) {
        if (batchKey < m_nextSailBatchToRelease) {
            return;
        }

        DigestBatch batch;
        batch.frontPsn = batchKey;
        batch.firstPacketArrivalTime = Simulator::Now();
        batch.digestReceivedTime = Simulator::Now();
        batch.digestProcessEvent = EventId();
        batch.entries = entries;
        batch.digestReceived = true;
        batch.digestProcessed = false;
        batch.rawFallbackReady = false;
        m_digestBatches[batchKey] = batch;
        MarkSailBatchReady(batchKey);
        return;
    }

    DigestBatch &batch = batch_it->second;
    if (batch.digestReceived || batch.digestProcessed) {
        return;
    }

    batch.entries = entries;
    batch.digestReceived = true;
    batch.digestProcessed = false;
    batch.rawFallbackReady = false;
    batch.digestReceivedTime = Simulator::Now();
    if (batch.digestProcessEvent.IsRunning()) {
        batch.digestProcessEvent.Cancel();
    }

    NS_LOG_INFO("Digest batch marked as received: batchKey=" << batchKey
        << " packets=" << batch.packets.size()
        << " entries=" << batch.entries.size());

    MarkSailBatchReady(batchKey);
}

void SwitchNode::MarkSailBatchReady(uint64_t batchKey) {
    auto batch_it = m_digestBatches.find(batchKey);
    if (batch_it == m_digestBatches.end()) {
        return;
    }
    DigestBatch &batch = batch_it->second;
    if (batch.digestProcessed) {
        return;
    }
    batch.digestProcessed = true;
    batch.rawFallbackReady = false;
    if (batch.digestReceived && !batch.entries.empty() &&
        batch.packets.size() == batch.entries.size()) {
        std::set<uint64_t> receivedPsns;
        for (const auto &pkt : batch.packets) {
            receivedPsns.insert(pkt.aggregatedPsn);
        }

        batch.rawFallbackReady = true;
        for (const auto &entry : batch.entries) {
            if (receivedPsns.find(entry.newPsn) == receivedPsns.end()) {
                batch.rawFallbackReady = false;
                break;
            }
        }
    }
    TryReleaseReadySailBatches();
}

void SwitchNode::GenerateFakePacketsAndForward(uint64_t batchKey) {
    auto batch_it = m_digestBatches.find(batchKey);
    if (batch_it == m_digestBatches.end()) {
        return;
    }

    DigestBatch &batch = batch_it->second;
    batch.digestProcessed = true;

    NS_LOG_INFO("Start processing SAIL digest batch");
    NS_LOG_INFO("batchKey=" << batchKey 
        << " packets=" << batch.packets.size()
        << " entries=" << batch.entries.size());

    // Map each aggregated PSN to its buffered packet.
    std::unordered_map<uint64_t, BufferedAggregatedPacket> pktMap;
    for (auto &pkt : batch.packets) {
        pktMap[pkt.aggregatedPsn] = pkt;
    }

    // Detect missing aggregated packets using the digest entries.
    std::vector<DigestEntry> lostEntries;
    
    for (uint32_t i = 0; i < batch.entries.size(); i++) {
        DigestEntry &entry = batch.entries[i];
        uint64_t newPsn = entry.newPsn;
        
        if (pktMap.find(newPsn) == pktMap.end()) {
            NS_LOG_INFO("Lost packet detected: newPsn=" << newPsn
                << " origPsn=" << entry.originalPsn
                << " flow=" << entry.sip << "->" << entry.dip
                << ":" << entry.sport << "->" << entry.dport);
            lostEntries.push_back(entry);
        }
    }

	struct PacketToSend {
        uint64_t psn;
        Ptr<Packet> packet;
        CustomHeader ch;
        bool isFakePacket;
    };

    std::vector<PacketToSend> packetsToSend;

    // Forward all buffered packets that arrived successfully.
    for (auto &pktEntry : pktMap) {
        uint64_t newPsn = pktEntry.first;
        BufferedAggregatedPacket &buffered = pktEntry.second;
        
        buffered.ch.wanOptInfo.isAggregated = 0;

		PppHeader ppp;
		buffered.packet->RemoveHeader(ppp);
		Ipv4Header ip;
		buffered.packet->RemoveHeader(ip);

		WanOptHeader opt;
		buffered.packet->RemoveHeader(opt);

		ip.SetProtocol(opt.GetNextHeader());
		ip.SetPayloadSize(ip.GetPayloadSize() - opt.GetSerializedSize()); // remove WanOptHeader bytes
		
		buffered.packet->AddHeader(ip);
		buffered.packet->AddHeader(ppp);

        PacketToSend pkt;
        pkt.psn = newPsn;
        pkt.packet = buffered.packet;
        pkt.ch = buffered.ch;
        pkt.isFakePacket = false;
        
        packetsToSend.push_back(pkt);
        
        // NS_LOG_INFO("Added normal packet: newPsn=" << newPsn
        //     << " origPsn=" << buffered.ch.udp.seq
        //     << " flow=" << buffered.ch.sip << "->" << buffered.ch.dip
        //     << ":" << buffered.ch.udp.sport << "->" << buffered.ch.udp.dport);
    }

    // Report missing entries to the receiver host, which drives SAIL
    // switch-NACK/retry recovery.
    for (auto &lostEntry : lostEntries) {
        SendSailLossReportToHost(lostEntry, lostEntry.flowId);
    }

    // Generate visible fake packets for missing entries so the receiver can
    // keep the in-order receive path moving.
    for (auto &lostEntry : lostEntries) {
        Ptr<Packet> fakePacket = BuildFakePacket(lostEntry);

        CustomHeader fakeCh;
        fakeCh.sip = lostEntry.sip;
        fakeCh.dip = lostEntry.dip;
        fakeCh.udp.sport = lostEntry.sport;
        fakeCh.udp.dport = lostEntry.dport;
        fakeCh.udp.seq = lostEntry.originalPsn;
        fakeCh.udp.pg = (uint16_t)lostEntry.pg;
        fakeCh.l3Prot = 0x11; // UDP
        fakeCh.wanOptInfo.isFakePacket = 1;
        fakeCh.wanOptInfo.isAggregated = 0;

        PacketToSend pkt;
        pkt.psn = lostEntry.newPsn;
        pkt.packet = fakePacket;
        pkt.ch = fakeCh;
        pkt.isFakePacket = true;
            
        packetsToSend.push_back(pkt);
            
        NS_LOG_INFO("Added fake packet: newPsn=" << lostEntry.newPsn
             << " origPsn=" << lostEntry.originalPsn);
    }
    
    // Preserve the aggregated-PSN order when releasing packets.
    std::sort(packetsToSend.begin(), packetsToSend.end(),
              [](const PacketToSend &a, const PacketToSend &b) {
                  return a.psn < b.psn;
              });

    for (auto &pkt : packetsToSend) {
        // NS_LOG_INFO("[Node 1] Sending packet: psn=" << pkt.psn
        //     << " isFake=" << (pkt.isFakePacket ? "true" : "false")
        //     << " origPsn=" << pkt.ch.udp.seq
        //     << " flow=" << pkt.ch.sip << "->" << pkt.ch.dip);
        
        SendToDev(pkt.packet, pkt.ch);
    }

    NS_LOG_INFO("Finished processing SAIL digest batch");

    ClearDigestBatch(batchKey);
}

void SwitchNode::ReleaseRawSailBatch(uint64_t batchKey) {
    auto batch_it = m_digestBatches.find(batchKey);
    if (batch_it == m_digestBatches.end()) {
        return;
    }

    DigestBatch &batch = batch_it->second;
    std::vector<BufferedAggregatedPacket*> packetsToSend;
    for (auto &bufferedPkt : batch.packets) {
        packetsToSend.push_back(&bufferedPkt);
    }
    std::sort(packetsToSend.begin(), packetsToSend.end(),
              [](const BufferedAggregatedPacket *a,
                 const BufferedAggregatedPacket *b) {
                  return a->aggregatedPsn < b->aggregatedPsn;
              });

    for (auto *bufferedPkt : packetsToSend) {
        PppHeader ppp;
        bufferedPkt->packet->RemoveHeader(ppp);
        Ipv4Header ip;
        bufferedPkt->packet->RemoveHeader(ip);

        WanOptHeader opt;
        bufferedPkt->packet->RemoveHeader(opt);

        ip.SetProtocol(opt.GetNextHeader());
        ip.SetPayloadSize(ip.GetPayloadSize() - opt.GetSerializedSize());
        bufferedPkt->packet->AddHeader(ip);
        bufferedPkt->packet->AddHeader(ppp);

        bufferedPkt->ch.wanOptInfo.isAggregated = 0;
        bufferedPkt->ch.wanOptInfo.isDigestPacket = 0;
        bufferedPkt->ch.wanOptInfo.isFakePacket = 0;
        bufferedPkt->ch.wanOptInfo.isRepeatPacket = 0;
        SendToDev(bufferedPkt->packet, bufferedPkt->ch);
    }
    ClearDigestBatch(batchKey);
}

void SwitchNode::TryReleaseReadySailBatches() {
    if (!IsFlowAggregationActive()) {
        return;
    }

    const uint64_t batchStep =
        static_cast<uint64_t>(m_digestInterval) * static_cast<uint64_t>(m_mtu);
    while (true) {
        auto it = m_digestBatches.find(m_nextSailBatchToRelease);
        if (it == m_digestBatches.end()) {
            return;
        }
        DigestBatch &batch = it->second;
        if (!batch.digestProcessed) {
            return;
        }

        uint64_t releaseKey = m_nextSailBatchToRelease;
        m_nextSailBatchToRelease += batchStep;
        if (batch.rawFallbackReady) {
            ReleaseRawSailBatch(releaseKey);
        } else {
            GenerateFakePacketsAndForward(releaseKey);
        }
    }
}

void SwitchNode::ClearDigestBatch(uint64_t batchKey) {
    auto batch_it = m_digestBatches.find(batchKey);
    if (batch_it != m_digestBatches.end()) {
        if (batch_it->second.digestProcessEvent.IsRunning()) {
            batch_it->second.digestProcessEvent.Cancel();
        }
        if (m_sailBufferedCurPkts >= batch_it->second.packets.size()) {
            m_sailBufferedCurPkts -= batch_it->second.packets.size();
        } else {
            m_sailBufferedCurPkts = 0;
        }
        NS_LOG_INFO("Clearing digest batch: batchKey=" << batchKey);
        m_digestBatches.erase(batch_it);
    }
}

void SwitchNode::SendSailLossReportToHost(const DigestEntry& entry, uint32_t inDev) {
    qbbHeader seqh;
    seqh.SetSeq(entry.originalPsn);
    seqh.SetPG(static_cast<uint16_t>(entry.pg));
    seqh.SetSport(entry.sport);
    seqh.SetDport(entry.dport);
    seqh.SetFlags(static_cast<uint16_t>(1 << qbbHeader::FLAG_SAIL_LOSS_REPORT));

    Ptr<Packet> reportPkt = Create<Packet>(std::max(0, 60 - 42));
    reportPkt->AddHeader(seqh);

    Ipv4Header ipHeader;
    ipHeader.SetDestination(Ipv4Address(entry.dip));
    ipHeader.SetSource(Ipv4Address(entry.sip));
    ipHeader.SetProtocol(0xFC);
    ipHeader.SetTtl(64);
    ipHeader.SetPayloadSize(reportPkt->GetSize());
    ipHeader.SetIdentification(0);
    ipHeader.SetTos(0);
    reportPkt->AddHeader(ipHeader);

    PppHeader ppp;
    ppp.SetProtocol(0x0021);
    reportPkt->AddHeader(ppp);

    SetFlowIdTag(reportPkt, inDev);

    CustomHeader reportCh;
    reportCh.sip = entry.sip;
    reportCh.dip = entry.dip;
    reportCh.l3Prot = 0xFC;
    reportCh.ack.sport = entry.sport;
    reportCh.ack.dport = entry.dport;
    reportCh.ack.seq = entry.originalPsn;
    reportCh.ack.pg = static_cast<uint16_t>(entry.pg);
    reportCh.ack.flags =
        static_cast<uint16_t>(1 << qbbHeader::FLAG_SAIL_LOSS_REPORT);

    SendToDev(reportPkt, reportCh);
}

void SwitchNode::CheckRetransmissionTimeout() {
    if (!IsFlowAggregationActive()) return;
    Time now = Simulator::Now();

    bool hasReadyFallbackBatch = false;
    for (auto it = m_digestBatches.begin(); it != m_digestBatches.end(); ) {
        DigestBatch &batch = it->second;
        
        // If the digest has not arrived in time, release buffered packets
        // through the raw fallback path.
        if (!batch.digestReceived && (now - batch.firstPacketArrivalTime > m_sailDigestTimeout)) {
            NS_LOG_WARN("[Switch] Digest Timeout for batchKey=" << it->first 
                << ". Fallback: Forwarding " << batch.packets.size() << " buffered packets raw.");
            batch.digestProcessed = true;
            batch.rawFallbackReady = true;
            hasReadyFallbackBatch = true;
            ++it;
        } else {
            ++it;
        }
    }

    if (hasReadyFallbackBatch) {
        TryReleaseReadySailBatches();
    }

    m_rtoTimerEvent = Simulator::Schedule(DIGEST_SCAN_INTERVAL, &SwitchNode::CheckRetransmissionTimeout, this);
}

int SwitchNode::GetOutDev(Ptr<const Packet> p, CustomHeader &ch){
	// look up entries
	auto entry = m_rtTable.find(ch.dip);

	// no matching entry
	if (entry == m_rtTable.end())
		return -1;

	// entry found
	auto &nexthops = entry->second;

	// pick one next hop based on hash
	union {
		uint8_t u8[4+4+2+2];
		uint32_t u32[3];
	} buf;
	buf.u32[0] = ch.sip;
	buf.u32[1] = ch.dip;
	if (ch.l3Prot == 0x6)
		buf.u32[2] = ch.tcp.sport | ((uint32_t)ch.tcp.dport << 16);
	else if (ch.l3Prot == 0x11)
		buf.u32[2] = ch.udp.sport | ((uint32_t)ch.udp.dport << 16);
	else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD)
		buf.u32[2] = ch.ack.sport | ((uint32_t)ch.ack.dport << 16);

	uint32_t idx = EcmpHash(buf.u8, 12, m_ecmpSeed) % nexthops.size();
	return nexthops[idx];
}

void SwitchNode::CheckAndSendPfc(uint32_t inDev, uint32_t qIndex){
  if (m_pfcEnabled && m_mmu->IsPfcEnabled(inDev)) {
	Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	if (m_mmu->CheckShouldPause(inDev, qIndex)){
		device->SendPfc(qIndex, 0);
		m_mmu->SetPause(inDev, qIndex);
	}
  }
}
void SwitchNode::CheckAndSendResume(uint32_t inDev, uint32_t qIndex){
  if (m_pfcEnabled && m_mmu->IsPfcEnabled(inDev)) {
	Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	if (m_mmu->CheckShouldResume(inDev, qIndex)){
		device->SendPfc(qIndex, 1);
		m_mmu->SetResume(inDev, qIndex);
	}
  }
}

void SwitchNode::SendToDev(Ptr<Packet>p, CustomHeader &ch){
	int idx = GetOutDev(p, ch);
	if (idx >= 0){
		NS_ASSERT_MSG(m_devices[idx]->IsLinkUp(), "The routing table look up should return link that is up");

		// admission control
		FlowIdTag t;
		p->PeekPacketTag(t);
		uint32_t inDev = t.GetFlowId();
        if (inDev >= pCnt) {
            std::cout << "Invalid inDev(flowId) in FlowIdTag: " << inDev
                << " l3Prot=" << (int)ch.l3Prot
                << " seq=" << ch.udp.seq
                << " ackSeq=" << ch.ack.seq
                << " sip=" << ch.sip
                << " dip=" << ch.dip
                << " sport=" << ch.udp.sport
                << " dport=" << ch.udp.dport
                << " ackSport=" << ch.ack.sport
                << " ackDport=" << ch.ack.dport
                << " isAggregated=" << (int)ch.wanOptInfo.isAggregated
                << " isDigestPacket=" << (int)ch.wanOptInfo.isDigestPacket
                << " isRepeatPacket=" << (int)ch.wanOptInfo.isRepeatPacket
                << " isFakePacket=" << (int)ch.wanOptInfo.isFakePacket
                << " nodeId=" << this->GetId()
                << std::endl;
            NS_ABORT_MSG("Invalid FlowIdTag in SwitchNode::SendToDev");
        }

		// determine the qIndex
		uint32_t qIndex;
		bool lr2SentryRepeatHighPrio =
			IsLr2Enabled() &&
			ch.l3Prot == 0x11 &&
			!IsWanPort(inDev) &&
			IsWanPort(idx) &&
			IsRepeatPacket(ch);
		if (ch.l3Prot == 0xFF || ch.l3Prot == 0xFE ||
			ch.l3Prot == 0xFD || ch.l3Prot == 0xFC ||
			lr2SentryRepeatHighPrio){  // QCN/PFC/(A)CK or LR2 repeat-to-WAN, go highest priority
			qIndex = 0;
		}else{
			qIndex = (ch.l3Prot == 0x06 ? 1 : ch.udp.pg); // if TCP, put to queue 1
		}
        if (qIndex >= qCnt) {
            std::cout << "Invalid qIndex in SwitchNode::SendToDev: " << qIndex
                << " inDev=" << inDev
                << " outDev=" << idx
                << " l3Prot=" << (int)ch.l3Prot
                << " udpPg=" << ch.udp.pg
                << " ackPg=" << ch.ack.pg
                << " seq=" << ch.udp.seq
                << " ackSeq=" << ch.ack.seq
                << " nodeId=" << this->GetId()
                << std::endl;
            NS_ABORT_MSG("Invalid qIndex in SwitchNode::SendToDev");
        }
		// std::cout << "qIndex is: " << qIndex << std::endl;
		if (qIndex != 0){ //not highest priority
			if (m_mmu->CheckIngressAdmission(inDev, qIndex, p->GetSize()) && m_mmu->CheckEgressAdmission(idx, qIndex, p->GetSize())){			// Admission control
				m_mmu->UpdateIngressAdmission(inDev, qIndex, p->GetSize());
				m_mmu->UpdateEgressAdmission(idx, qIndex, p->GetSize());
			}else{
				return; // Drop
			}
			CheckAndSendPfc(inDev, qIndex);
		}
		m_bytes[inDev][idx][qIndex] += p->GetSize();
		m_devices[idx]->SwitchSend(qIndex, p, ch);
	}else
	{
		return; // Drop
	}
}

uint32_t SwitchNode::EcmpHash(const uint8_t* key, size_t len, uint32_t seed) {
  uint32_t h = seed;
  if (len > 3) {
    const uint32_t* key_x4 = (const uint32_t*) key;
    size_t i = len >> 2;
    do {
      uint32_t k = *key_x4++;
      k *= 0xcc9e2d51;
      k = (k << 15) | (k >> 17);
      k *= 0x1b873593;
      h ^= k;
      h = (h << 13) | (h >> 19);
      h += (h << 2) + 0xe6546b64;
    } while (--i);
    key = (const uint8_t*) key_x4;
  }
  if (len & 3) {
    size_t i = len & 3;
    uint32_t k = 0;
    key = &key[i - 1];
    do {
      k <<= 8;
      k |= *key--;
    } while (--i);
    k *= 0xcc9e2d51;
    k = (k << 15) | (k >> 17);
    k *= 0x1b873593;
    h ^= k;
  }
  h ^= len;
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

void SwitchNode::SetEcmpSeed(uint32_t seed){
	m_ecmpSeed = seed;
}

void SwitchNode::AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx){
	uint32_t dip = dstAddr.Get();
	m_rtTable[dip].push_back(intf_idx);
}

void SwitchNode::ClearTable(){
	m_rtTable.clear();
}

// This function can only be called in switch mode
bool SwitchNode::SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch){
	uint32_t inDev = device->GetIfIndex();
	uint32_t nodeId = this->GetId();
	// if (nodeId == m_egressSwitchId1)
	// {
	// 	NS_LOG_INFO("Switch Node " << nodeId << " received packet at dev " << inDev
	// 		<< " from flow: " << ch.sip << "->" << ch.dip
	// 		<< ":" << ch.udp.sport << "->" << ch.udp.dport
	// 		<< " psn=" << ch.udp.seq
    //         // << " pg=" << (int)ch.udp.pg
	// 		// << " aggPsn=" << ch.wanOptInfo.newPsn
	// 		// << " isAggregated=" << (int)ch.wanOptInfo.isAggregated
    //         // << " isRepeatPacket=" << (int)ch.wanOptInfo.isRepeatPacket
    //     );
	// }

    // Read the ToS field to identify repeat packets.
    // Switch 0 and switch 1 may apply different handling.
    bool isRepeatPacket = false;
    
    Ptr<Packet> pktCopy = packet->Copy();
    PppHeader ppp;
    pktCopy->RemoveHeader(ppp);
    Ipv4Header ipHeader;
    pktCopy->PeekHeader(ipHeader);
    uint8_t tos = ipHeader.GetTos();
    
    if (tos & 0x10) {  // 0x10 = 16
        isRepeatPacket = true;
        ch.wanOptInfo.isRepeatPacket = 1;
    }

	int outPort = -1;
	if (ch.l3Prot == 0x11 || ch.l3Prot == 0xFC || ch.l3Prot == 0xFD) {
		outPort = GetOutDev(packet, ch);
	}

	TryRecordLr2SentryFilter(inDev, ch);       // P2: record Depot-NACK into SR filter ranges
	TryResetLr2SentryOnHostNack(inDev, ch);
	if (TryHandleLr2HostNack(inDev, outPort, ch)) {
		return true;
	}
	TryHandleLr2DepotAck(inDev, ch);            // P1: update ack_seq from receiver ACKs
	if (TryHandleLr2DepotData(inDev, packet, ch)) {
		return true;
	}
	if (TryHandleLr2Sentry(inDev, outPort, packet, ch)) {
		return true;
	}

	if (IsFlowAggregationActive() && ch.wanOptInfo.isAggregated == 1) {
		// NS_LOG_INFO("[Switch] Received aggregated packet at Node " << nodeId
		// 	<< ": aggPsn=" << ch.wanOptInfo.newPsn
		// 	<< " isDigestPacket=" << (int)ch.wanOptInfo.isDigestPacket);
		if (nodeId == m_egressSwitchId2) {
			if (ch.wanOptInfo.isDigestPacket == 1) {
				NS_LOG_INFO("[Node 1] Received digest packet");
				ProcessDigestPacket(packet, ch);
				return true;
			}
			else {
				BufferAggregatedPacket(packet, ch, ch.wanOptInfo.newPsn);
				return true;
			}
        }
	}

	bool aggregatedBySail = false;
	bool isSailFlowTail = IsSailFlowTailPacket(ch);
	if (IsFlowAggregationActive() && ch.wanOptInfo.isAggregated == 0) {
		if (nodeId == m_egressSwitchId1 && !isRepeatPacket) {
			if (outPort == 1) {
				aggregatedBySail = AggregateFlow(packet, ch, inDev);
                // NS_LOG_INFO("[Node 0] Packet aggregated");
			}
		}
	}

	SendToDev(packet, ch);

	if (IsFlowAggregationActive() &&
		(m_aggregatedFlow.digestCounter >= m_digestInterval ||
		 (aggregatedBySail && isSailFlowTail))) {
		SendSailDigestBatch(ch, inDev);
	}
	return true;
}

void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p){
	FlowIdTag t;
	p->PeekPacketTag(t);
	if (qIndex != 0){
		uint32_t inDev = t.GetFlowId();
		m_mmu->RemoveFromIngressAdmission(inDev, qIndex, p->GetSize());
		m_mmu->RemoveFromEgressAdmission(ifIndex, qIndex, p->GetSize());
		m_bytes[inDev][ifIndex][qIndex] -= p->GetSize();
		if (m_ecnEnabled){
			bool egressCongested = m_mmu->ShouldSendCN(ifIndex, qIndex);
			if (egressCongested){
				PppHeader ppp;
				Ipv4Header h;
				p->RemoveHeader(ppp);
					p->RemoveHeader(h);
					h.SetEcn((Ipv4Header::EcnType)0x03);
					p->AddHeader(h);
					p->AddHeader(ppp);
				}
			}
			MaybeHandleThemisPnp(ifIndex, qIndex, inDev, p);
			//CheckAndSendPfc(inDev, qIndex);
			CheckAndSendResume(inDev, qIndex);
		}
	if (1){
		uint8_t* buf = p->GetBuffer();
		if (buf[PppHeader::GetStaticSize() + 9] == 0x11){ // udp packet
			IntHeader *ih = (IntHeader*)&buf[PppHeader::GetStaticSize() + 20 + 8 + 6]; // ppp, ip, udp, SeqTs, INT
			Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
			if (m_ccMode == 3){ // HPCC
				ih->PushHop(Simulator::Now().GetTimeStep(), m_txBytes[ifIndex], dev->GetQueue()->GetNBytesTotal(), dev->GetDataRate().GetBitRate());
			}else if (m_ccMode == 10){ // HPCC-PINT
				uint64_t t = Simulator::Now().GetTimeStep();
				uint64_t dt = t - m_lastPktTs[ifIndex];
				if (dt > m_maxRtt)
					dt = m_maxRtt;
				uint64_t B = dev->GetDataRate().GetBitRate() / 8; //Bps
				uint64_t qlen = dev->GetQueue()->GetNBytesTotal();
				double newU;

				/**************************
				 * approximate calc
				 *************************/
				int b = 20, m = 16, l = 20; // see log2apprx's paremeters
				int sft = logres_shift(b,l);
				double fct = 1<<sft; // (multiplication factor corresponding to sft)
				double log_T = log2(m_maxRtt)*fct; // log2(T)*fct
				double log_B = log2(B)*fct; // log2(B)*fct
				double log_1e9 = log2(1e9)*fct; // log2(1e9)*fct
				double qterm = 0;
				double byteTerm = 0;
				double uTerm = 0;
				if ((qlen >> 8) > 0){
					int log_dt = log2apprx(dt, b, m, l); // ~log2(dt)*fct
					int log_qlen = log2apprx(qlen >> 8, b, m, l); // ~log2(qlen / 256)*fct
					qterm = pow(2, (
								log_dt + log_qlen + log_1e9 - log_B - 2*log_T
								)/fct
							) * 256;
					// 2^((log2(dt)*fct+log2(qlen/256)*fct+log2(1e9)*fct-log2(B)*fct-2*log2(T)*fct)/fct)*256 ~= dt*qlen*1e9/(B*T^2)
				}
				if (m_lastPktSize[ifIndex] > 0){
					int byte = m_lastPktSize[ifIndex];
					int log_byte = log2apprx(byte, b, m, l);
					byteTerm = pow(2, (
								log_byte + log_1e9 - log_B - log_T
								)/fct
							);
					// 2^((log2(byte)*fct+log2(1e9)*fct-log2(B)*fct-log2(T)*fct)/fct) ~= byte*1e9 / (B*T)
				}
				if (m_maxRtt > dt && m_u[ifIndex] > 0){
					int log_T_dt = log2apprx(m_maxRtt - dt, b, m, l); // ~log2(T-dt)*fct
					int log_u = log2apprx(int(round(m_u[ifIndex] * 8192)), b, m, l); // ~log2(u*512)*fct
					uTerm = pow(2, (
								log_T_dt + log_u - log_T
								)/fct
							) / 8192;
					// 2^((log2(T-dt)*fct+log2(u*512)*fct-log2(T)*fct)/fct)/512 = (T-dt)*u/T
				}
				newU = qterm+byteTerm+uTerm;

				#if 0
				/**************************
				 * accurate calc
				 *************************/
				double weight_ewma = double(dt) / m_maxRtt;
				double u;
				if (m_lastPktSize[ifIndex] == 0)
					u = 0;
				else{
					double txRate = m_lastPktSize[ifIndex] / double(dt); // B/ns
					u = (qlen / m_maxRtt + txRate) * 1e9 / B;
				}
				newU = m_u[ifIndex] * (1 - weight_ewma) + u * weight_ewma;
				printf(" %lf\n", newU);
				#endif

				/************************
				 * update PINT header
				 ***********************/
				uint16_t power = Pint::encode_u(newU);
				if (power > ih->GetPower())
					ih->SetPower(power);

				m_u[ifIndex] = newU;
			}
		}
	}
	m_txBytes[ifIndex] += p->GetSize();
	if (IsWanPort(ifIndex)) {
		m_wanTxBytes += p->GetSize();
	}
	m_lastPktSize[ifIndex] = p->GetSize();
	m_lastPktTs[ifIndex] = Simulator::Now().GetTimeStep();
}

int SwitchNode::logres_shift(int b, int l){
	static int data[] = {0,0,1,2,2,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5};
	return l - data[b];
}

int SwitchNode::log2apprx(int x, int b, int m, int l){
	int x0 = x;
	int msb = int(log2(x)) + 1;
	if (msb > m){
		x = (x >> (msb - m) << (msb - m));
		#if 0
		x += + (1 << (msb - m - 1));
		#else
		int mask = (1 << (msb-m)) - 1;
		if ((x0 & mask) > (rand() & mask))
			x += 1<<(msb-m);
		#endif
	}
	return int(log2(x) * (1<<logres_shift(b, l)));
}

// for monitor
/**
 * outoput format:
 * time, sw_id, port_id, q_id, qlen, port_len
*/
void SwitchNode::PrintSwitchQlen(FILE* qlen_output){
	uint32_t n_dev = this->GetNDevices();
	for(uint32_t i = 1; i < n_dev; ++i){
		uint64_t port_len = 0;
		for(uint32_t j=0; j < qCnt; ++j){
			port_len += m_mmu->egress_bytes[i][j];
		}
		if(port_len == last_port_qlen[i]){
			continue;
		}
		for(uint32_t j=0; j < qCnt; ++j){
			fprintf(qlen_output, "%lu, %u, %u, %u, %u, %lu\n", Simulator::Now().GetTimeStep(), m_id, i, j, m_mmu->egress_bytes[i][j], port_len);
			fflush(qlen_output);
		}
		last_port_qlen[i] = port_len;
	}		
}

/**
 * outoput format:
 * time, sw_id, port_id, bandwidth
*/
void SwitchNode::PrintSwitchBw(FILE* bw_output, uint32_t bw_mon_interval){
	uint32_t n_dev = this->GetNDevices();
	for(uint32_t i = 1; i < n_dev; ++i){
		if(last_txBytes[i] == m_txBytes[i]){
			continue;
		}
		double bw = (m_txBytes[i] - last_txBytes[i]) * 8 * 1e6 / bw_mon_interval; // bit/s
		bw = bw*1.0 / 1e9; // Gbps
		fprintf(bw_output, "%lu, %u, %u, %f\n", Simulator::Now().GetTimeStep(), m_id, i, bw);
		fflush(bw_output);
		last_txBytes[i] = m_txBytes[i];
	}	
}

void SwitchNode::DoInitialize(void) {
    Node::DoInitialize();

    if (IsFlowAggregationActive()) {
        m_rtoTimerEvent = Simulator::Schedule(DIGEST_SCAN_INTERVAL, &SwitchNode::CheckRetransmissionTimeout, this);
    }
    if (IsLr2Enabled()) {
        m_lr2DepotTimerEvent = Simulator::Schedule(m_lr2BackupTimeout, &SwitchNode::CheckLr2DepotTimeout, this);
    }
}

void SwitchNode::DoDispose(void) {
    // Cancel pending timer events before releasing switch state.
    if (m_rtoTimerEvent.IsRunning()) {
        m_rtoTimerEvent.Cancel();
    }
    if (m_lr2DepotTimerEvent.IsRunning()) {
        m_lr2DepotTimerEvent.Cancel();
    }
    
	NS_LOG_INFO("[THEMIS][PNP] node=" << this->GetId()
		<< " proactiveCnpSent=" << m_themisPnpSent
		<< " cacheHit=" << m_themisPnpCacheHit
		<< " ecnCleared=" << m_themisPnpEcnCleared);

    m_digestBatches.clear();
	m_themisPnpCache.clear();
	m_lr2SentryState.clear();
	m_lr2DepotState.clear();

    Node::DoDispose();
}
} /* namespace ns3 */
