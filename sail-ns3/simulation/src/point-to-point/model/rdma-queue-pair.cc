#include "rdma-queue-pair.h"
#include <algorithm>
#include <ns3/hash.h>
#include <ns3/ipv4-header.h>
#include <ns3/seq-ts-header.h>
#include <ns3/simulator.h>
#include <ns3/udp-header.h>
#include <ns3/uinteger.h>
#include "ns3/ppp-header.h"
#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("RdmaQueuePair");

/**************************
 * RdmaQueuePair
 *************************/
NS_OBJECT_ENSURE_REGISTERED(RdmaQueuePair);


TypeId RdmaQueuePair::GetTypeId(void) {
  static TypeId tid = 
      TypeId("ns3::RdmaQueuePair")
          .SetParent<Object>();
  return tid;
}

RdmaQueuePair::RdmaQueuePair(
    uint16_t pg,
    Ipv4Address _sip,
    Ipv4Address _dip,
    uint16_t _sport,
    uint16_t _dport) {
  startTime = Simulator::Now();
  sip = _sip;
  dip = _dip;
  sport = _sport;
  dport = _dport;
  m_src = -1;
  m_dest = -1;
  m_tag = -1;
  snd_nxt = snd_una = 0;
  m_pg = pg;
  m_ipid = 0;
  m_win = 0;
  m_baseRtt = 0;
  m_max_rate = 0;
  m_var_win = false;
  m_rate = 0;
  m_nextAvail = Time(0);
  lastPktSize = 0;
  
  loss_num = 0;
  loss_queue_head = 0;
  loss_queue_tail = 0;

  // RTO initialization - fixed 50ms timeout
  m_rto = MilliSeconds(30);
  m_rtoTimer = EventId();
  m_retx_count = 0;
  m_nack_recv_count = 0;
  m_rto_count = 0;
  m_maxSeqEverSent = 0;
  m_repeatModeActive = false;
  m_repeatUntil = 0;
  m_sailCommitAckSeq = 0;
  m_recentSailSwitchNacks.clear();
}

void RdmaQueuePair::SetSrc(uint32_t src) {
  m_src = src;
}

void RdmaQueuePair::SetDest(uint32_t dest) {
  m_dest = dest;
}

uint32_t RdmaQueuePair::GetSrc() {
  return m_src;
}

uint32_t RdmaQueuePair::GetDest() {
  return m_dest;
}

void RdmaQueuePair::SetTag(uint64_t tag) {
  m_tag = tag;
}

uint64_t RdmaQueuePair::GetTag() {
  return m_tag;
}

void RdmaQueuePair::SetWin(uint32_t win) {
  m_win = win;
  // std::cout << "set win: " << m_win << std::endl;
}

void RdmaQueuePair::SetBaseRtt(uint64_t baseRtt) {
  m_baseRtt = baseRtt;
}

void RdmaQueuePair::SetVarWin(bool v) {
  m_var_win = v;
}

/**
 * Bytes remaining to be sent for the current message.
 * Returns 0 when all data has been transmitted (snd_nxt reached message end).
 * Note: this can become > 0 again after RTO resets snd_nxt to snd_una.
 */
uint64_t RdmaQueuePair::GetBytesLeft() {
  if(m_messages.empty()){
    return 0;
  }
  uint64_t size = m_messages.front().m_size + m_messages.front().m_startSeq;
  return size >= snd_nxt ? size - snd_nxt : 0;
}

uint32_t RdmaQueuePair::GetHash(void) {
  union {
    struct {
      uint32_t sip, dip;
      uint16_t sport, dport;
    };
    char c[12];
  } buf;
  buf.sip = sip.Get();
  buf.dip = dip.Get();
  buf.sport = sport;
  buf.dport = dport;
  return Hash32(buf.c, 12);
}

/**
 * Advance the send-side cumulative acknowledgment pointer (snd_una).
 * Called from RdmaHw::ReceiveAck when an ACK arrives.
 *
 * Key invariant: snd_una only moves forward (cumulative ACK semantics).
 * After advancing snd_una, the RTO timer is cancelled here. The caller
 * (ReceiveAck) is responsible for restarting it if unACKed data remains.
 * This two-step design avoids the bug where Acknowledge() only cancelled
 * the timer when snd_una >= snd_nxt (all data ACKed), causing the timer
 * to fire spuriously when the window was always full (e.g., IRN BDP-FC).
 */
void RdmaQueuePair::Acknowledge(uint64_t ack) {
  if (ack > snd_una) {
    snd_una = ack;

    // Clean up packet send times for acknowledged packets
    auto it = m_packetSendTimes.begin();
    while (it != m_packetSendTimes.end() && it->first < ack) {
      it = m_packetSendTimes.erase(it);
    }

    // Cancel RTO timer — caller (RdmaHw::ReceiveAck) will restart if needed
    if (m_rtoTimer.IsRunning()) {
      Simulator::Cancel(m_rtoTimer);
      m_rtoTimer = EventId();
    }
  }
}

uint64_t RdmaQueuePair::GetOnTheFly() {
  return snd_nxt - snd_una;
}

/**
 * Check if the sender is window-bound (cannot send more data).
 * Returns true when the amount of in-flight data (snd_nxt - snd_una)
 * reaches the effective window size.
 * - HAS_WIN=0 → m_win=0 → always returns false (no window constraint)
 * - IRN mode → m_win = BDP-FC window (512*MTU=2MB), VarWin disabled
 * - Other modes with HAS_WIN=1 → m_win scaled by CC rate
 */
bool RdmaQueuePair::IsWinBound() {
  uint64_t w = GetWin();
  return w != 0 && GetOnTheFly() >= w;
}

/**
 * Get the effective send window in bytes.
 * If VarWin is true, the window scales with the CC rate: w = m_win * rate/maxRate.
 * For IRN, VarWin is forced false (see RdmaHw::AddQpForFlow) because the
 * BDP-FC window is a hardware constraint, not a congestion signal.
 */
uint64_t RdmaQueuePair::GetWin() {
  if (m_win == 0)
    return 0;
  uint64_t w;
  if (m_var_win) {
    w = m_win * m_rate.GetBitRate() / m_max_rate.GetBitRate();
    if (w == 0)
      w = 1; // must > 0
  } else {
    w = m_win;
  }
  return w;
}

bool RdmaQueuePair::IsFinished() {
  return m_messages.empty();
}

void RdmaQueuePair::UpdateRate() {
  m_rate = m_congestionControl->m_ccRate;
}

void RdmaQueuePair::PushMessage(
    uint64_t size,
    Callback<void> notifyAppFinish,
    Callback<void> notifyAppSent) {
  RdmaMessage msg;
  msg.m_size = size;
  if(m_messages.empty()) {
    msg.m_startSeq = snd_nxt; // there are no old messages in the queue, so set start_seq to snd_nxt
  } else {
    msg.m_startSeq = 0; // there are old messages in the queue, so modify start_seq when finish an old message
  }
  
  msg.m_notifyAppFinish = notifyAppFinish;
  msg.m_notifyAppSent = notifyAppSent;
  m_messages.push(msg);
}

void RdmaQueuePair::FinishMessage() {
  if (!m_messages.empty()) {
    RdmaMessage msg = m_messages.front();
    m_messages.pop();
    msg.m_notifyAppFinish();
    
    // snd_nxt = snd_una = 0; //TODO is it needed?
  }else{
    NS_LOG_ERROR("RdmaQueuePair::FinishMessage(): message is empty but try to finish");
  }
}

/**
 * Check if the current message (flow) is fully acknowledged.
 * True when snd_una has advanced past the message's last byte.
 * This is the sender-side completion detection: called in ReceiveAck
 * after each ACK to trigger QpCompleteMessage → flow_num_finished++.
 */
bool RdmaQueuePair::IsCurMessageFinished() {
  if (m_messages.empty()) {
    return true;
  }
  return snd_una >= m_messages.front().m_size + m_messages.front().m_startSeq;
}

bool RdmaQueuePair::IsCurMessageFinishedAt(uint64_t ackSeq) const {
  if (m_messages.empty()) {
    return true;
  }
  return ackSeq >= m_messages.front().m_size + m_messages.front().m_startSeq;
}

/*********************
 * RdmaRxQueuePair
 ********************/
NS_OBJECT_ENSURE_REGISTERED(RdmaRxQueuePair);

TypeId RdmaRxQueuePair::GetTypeId(void) {
  static TypeId tid = 
      TypeId("ns3::RdmaRxQueuePair")
      .SetParent<Object>();
  return tid;
}

RdmaRxQueuePair::RdmaRxQueuePair() {
  sip = dip = sport = dport = 0;
  m_ipid = 0;
  ReceiverNextExpectedSeq = 0;
  m_highest_expected_seq = 0;
  std::fill_n(m_receive_buffer, RECEIVE_BUFFER_SIZE, 0);
  m_receive_buffer_head = 0;
  m_receive_buffer_tail = 0;
  m_receive_buffer_num = 0;
  m_max_receive_buffer_num = 0;
  m_nackTimer = Time(0);
  m_milestone_rx = 0;
  m_lastNACK = 0;
  m_gbnSeqNakOutstanding = false;
  m_gbnSeqNakPsn = 0;
  m_sailCommitNextExpectedSeq = 0;
  m_sailSwitchNackTimeoutCount = 0;
  m_sailPendingFakePackets.clear();
  m_sailRealBufferedPackets.clear();
  m_sailOutstandingLosses.clear();
  m_sailOutstandingLossFifo.clear();
}

uint32_t RdmaRxQueuePair::GetHash(void) {
  union {
    struct {
      uint32_t sip, dip;
      uint16_t sport, dport;
    };
    char c[12];
  } buf;
  buf.sip = sip;
  buf.dip = dip;
  buf.sport = sport;
  buf.dport = dport;
  return Hash32(buf.c, 12);
}

/*********************
 * RdmaQueuePairGroup
 ********************/
NS_OBJECT_ENSURE_REGISTERED(RdmaQueuePairGroup);

TypeId RdmaQueuePairGroup::GetTypeId(void) {
  static TypeId tid = TypeId("ns3::RdmaQueuePairGroup").SetParent<Object>();
  return tid;
}

RdmaQueuePairGroup::RdmaQueuePairGroup(void) {}

uint32_t RdmaQueuePairGroup::GetN(void) {
  #ifdef NS3_MTP
  MtpInterface::explicitCriticalSection cs;
  #endif
  uint32_t size = m_qps.size();
  #ifdef NS3_MTP
	cs.ExitSection();
  #endif
  return size;
}

Ptr<RdmaQueuePair> RdmaQueuePairGroup::Get(uint32_t idx) {
  #ifdef NS3_MTP
  MtpInterface::explicitCriticalSection cs;
  #endif
  Ptr<RdmaQueuePair> qp = m_qps[idx];
  #ifdef NS3_MTP
	cs.ExitSection();
  #endif
  return qp;
}

Ptr<RdmaQueuePair> RdmaQueuePairGroup::operator[](uint32_t idx) {
  #ifdef NS3_MTP
  MtpInterface::explicitCriticalSection cs;
  #endif  
  Ptr<RdmaQueuePair> qp = m_qps[idx];
  #ifdef NS3_MTP
	cs.ExitSection();
  #endif
  return qp;
}

void RdmaQueuePairGroup::AddQp(Ptr<RdmaQueuePair> qp) {
  #ifdef NS3_MTP
  MtpInterface::explicitCriticalSection cs;
  #endif              
  m_qps.push_back(qp);
  #ifdef NS3_MTP
	cs.ExitSection();
  #endif
}

#if 0
void RdmaQueuePairGroup::AddRxQp(Ptr<RdmaRxQueuePair> rxQp){
	m_rxQps.push_back(rxQp);
}
#endif

void RdmaQueuePairGroup::RemoveFinishedQps(uint32_t &rr_last) {
  uint32_t size = m_qps.size();
  uint32_t idx = 0;
  uint32_t rm_cnt_before_idx = 0;
  auto new_end =
      std::remove_if(m_qps.begin(), m_qps.end(), [&](Ptr<RdmaQueuePair> qp) {
        idx++;
        if (qp == nullptr || qp->IsFinished()) {
          if (idx - 1 < rr_last) {
            rm_cnt_before_idx++;
          }
          return true;
        }
        return false;
      });
  rr_last -= rm_cnt_before_idx;
  m_qps.erase(new_end, m_qps.end());
}

void RdmaQueuePairGroup::Clear(void) {
  #ifdef NS3_MTP
  MtpInterface::explicitCriticalSection cs;
  #endif            
  m_qps.clear();
  #ifdef NS3_MTP
	cs.ExitSection();
  #endif
}

} // namespace ns3
