#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include <ns3/node.h>
#include <ns3/simple-seq-ts-header.h>
#include <algorithm>
#include <deque>
#include <list>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include "pint.h"
#include "qbb-net-device.h"
#include "switch-mmu.h" 

namespace ns3 {

class Packet;

class SwitchNode : public Node{
public:
	enum ReliabilityMode {
		RELIABILITY_AUTO = 0,
		RELIABILITY_GBN,
		RELIABILITY_SR,
		RELIABILITY_IRN,
		RELIABILITY_LR2,
		RELIABILITY_SAIL,
	};

private:
	static const uint32_t pCnt = 1025;	// Number of ports used
	static const uint32_t qCnt = 8;	// Number of queues/priorities used
	uint32_t m_ecmpSeed;
	std::unordered_map<uint32_t, std::vector<int> > m_rtTable; // map from ip address (u32) to possible ECMP port (index of dev)
	std::set<uint32_t> active_ports;	// record active ports in switch

	// monitor of PFC
	uint32_t m_bytes[pCnt][pCnt][qCnt]; // m_bytes[inDev][outDev][qidx] is the bytes from inDev enqueued for outDev at qidx
	
	uint64_t m_txBytes[pCnt]; // counter of tx bytes

	uint32_t m_lastPktSize[pCnt];
	uint64_t m_lastPktTs[pCnt]; // ns
	double m_u[pCnt];

protected:
	bool m_ecnEnabled;
	bool m_pfcEnabled;
	uint32_t m_ccMode;
	uint64_t m_maxRtt;

	uint32_t m_ackHighPrio; // set high priority for ACK/NACK

    EventId m_rtoTimerEvent;

	private:
		uint32_t m_reliabilityMode;

		// ======THEMIS (PNP)======
		bool m_themisEnabled;
		bool m_themisPnpEnabled;
		Time m_themisPnpCnpInterval;
		std::unordered_map<uint64_t, Time> m_themisPnpCache;
		uint64_t m_themisPnpSent;
		uint64_t m_themisPnpCacheHit;
		uint64_t m_themisPnpEcnCleared;
		bool IsThemisPnpDataPacket(const CustomHeader &ch) const;
		uint64_t BuildThemisFlowKey(const CustomHeader &ch) const;
		void SendThemisPnpCnp(const CustomHeader &ch, uint32_t inDev);
		void MaybeHandleThemisPnp(uint32_t ifIndex, uint32_t qIndex, uint32_t inDev, Ptr<Packet> p);

		// Sender-side SAIL flow aggregation.
		bool m_flowAggregationEnabled;
		uint32_t m_egressSwitchId1;
		uint32_t m_egressSwitchId2;
		uint32_t m_digestInterval;
		Time m_sailDigestTimeout;
		uint32_t m_sailDigestCopies;
		static const uint8_t SAIL_FLOW_TAIL_TOS = 0x80;

	struct DigestEntry {
		uint32_t sip, dip;
		uint16_t sport, dport;
		uint64_t originalPsn;
		uint64_t newPsn;
		uint32_t payloadSize;
		uint32_t flowId;
		uint32_t pg;
	};

	static const uint32_t DIGEST_ENTRY_SIZE = 40;

	struct FiveTuple {
        uint32_t sip;
        uint32_t dip;
        uint16_t sport;
        uint16_t dport;
        uint16_t pg;
        
        bool operator==(const FiveTuple& other) const {
            return sip == other.sip && dip == other.dip && 
                   sport == other.sport && dport == other.dport && pg == other.pg;
        }
        
        bool operator<(const FiveTuple& other) const {
            if (sip != other.sip) return sip < other.sip;
            if (dip != other.dip) return dip < other.dip;
            if (sport != other.sport) return sport < other.sport;
            if (dport != other.dport) return dport < other.dport;
            return pg < other.pg;
        }
    };
    
    // Maximum aggregated sequence number observed for each flow.
    std::map<FiveTuple, uint64_t> m_flowMaxAggSeq;

	/** Buffered packet stored in Depot's reorderPool or backupPool. */
	struct Lr2BufferedPacket {
		Ptr<Packet> packet;
		CustomHeader ch;
		uint32_t payloadSize;
	};

	/**
	 * Per-flow state for LR2.
	 * Used by BOTH Sentry (m_lr2SentryState) and Depot (m_lr2DepotState),
	 * though each side only uses a subset of fields.
	 * Symbol names follow LR2 paper Table II.
	 */
	struct Lr2FlowState {
		// --- Dual-register OOO tracking (Section III.B.1, Fig. 5) ---
		uint64_t expectedSeq;   // ePSN: expected in-order seq; frozen at first gap
		uint64_t expectedSeqD;  // ePSN_D: tracks upper edge of OOO window
		                        // (Sentry uses expectedSeq as ePSN_s)

		// --- Depot reorderPool: buffers OOO packets from WAN (Section III.B.1) ---
		std::map<uint64_t, Lr2BufferedPacket> reorderPool;   // seq -> buffered pkt

		// --- Depot backupPool: caches in-order packets for local retransmission (Section III.B.2) ---
		std::map<uint64_t, Lr2BufferedPacket> backupPool;    // seq -> buffered pkt
		std::deque<uint64_t> backupOrder;                     // insertion order for FIFO eviction

		// --- NACK tracking ---
		std::set<uint64_t> outstandingNackSeqs;  // seqs we have NACKed but not yet received
		Time lastNackTime;                        // rate-limit NACK generation
		uint64_t lastNackSeq;

		// --- P0: backup pool timeout retransmission (Section III.B.2, Algorithm 2) ---
		bool retState;                  // ret_state: true = in recovery mode (temporary reordering)
		uint64_t recoverSeq;            // recover_seq: target seq to reach before exiting retState
		Time lastBackupPoolRefresh;     // refreshed on each in-order arrival or ACK
		uint32_t wanInDev;              // WAN ingress port (for SendToDev routing)

		// --- P1: RNIC timeout compatibility (Section III.B.3, Algorithm 1 L4-7) ---
		uint64_t ackSeq;                // ack_seq: latest cumulative ACK from destination host

		// --- P2: Sentry SR filter ranges (Section III.C.2) ---
		// Missing byte ranges learned from Depot-NACKs. Key=startSeq, value=endSeq
		// (exclusive). Repeat packets are forwarded only if they hit one of these
		// missing ranges.
		std::map<uint64_t, uint64_t> srFilterRanges;

		Lr2FlowState()
			: expectedSeq(0),
			  expectedSeqD(0),
			  lastNackTime(Time(0)),
			  lastNackSeq(std::numeric_limits<uint64_t>::max()),
			  retState(false),
			  recoverSeq(0),
			  lastBackupPoolRefresh(Time(0)),
			  wanInDev(0),
			  ackSeq(0) {}
	};

	// --- LR2 configuration parameters (set via ns3 attributes) ---
	Time m_lr2NackInterval;            // rate-limit interval between consecutive NACKs for same flow
	uint32_t m_lr2ReorderWindowPkts;   // max OOO packets per flow in Depot reorderPool
	                                   // should be >= WAN BDP in packets for full recovery
	uint32_t m_lr2BackupWindowPkts;    // max in-order packets per flow in Depot backupPool
	Time m_lr2BackupTimeout;           // T_out: backup pool timeout for link-level retransmission.

	// --- LR2 per-flow state ---
	std::map<FiveTuple, Lr2FlowState> m_lr2SentryState;  // Sentry state (switch 0, near-sender DCI)
	std::map<FiveTuple, Lr2FlowState> m_lr2DepotState;   // Depot state (switch 1, near-receiver DCI)
	uint64_t m_lr2ReorderPoolCurPkts;
	uint64_t m_lr2ReorderPoolPeakPkts;
	uint64_t m_lr2BackupPoolCurPkts;
	uint64_t m_lr2BackupPoolPeakPkts;
	uint64_t m_sailBufferedCurPkts;
	uint64_t m_sailBufferedPeakPkts;
	uint64_t m_switchRecoveryBufferPeakPkts;
	uint64_t m_wanTxBytes;

	struct AggregatedFlowInfo {
		uint64_t nextAggregatedPsn;
		std::vector<DigestEntry> digestEntries;
		uint32_t digestCounter;		
	};

	AggregatedFlowInfo m_aggregatedFlow;

	Ptr<Packet> BuildDigestPacket();
	Ptr<Packet> BuildFakePacket(const DigestEntry &entry);
	bool AggregateFlow(Ptr<Packet> packet, CustomHeader &ch, uint32_t inDev);
	void SendSailDigestBatch(const CustomHeader &ch, uint32_t inDev);

	// Receiver-side SAIL buffering and recovery.
    struct BufferedAggregatedPacket {
        Ptr<Packet> packet;
        CustomHeader ch;
        uint64_t aggregatedPsn;
        bool isLost;
    };

	struct DigestBatch {
        uint64_t frontPsn;
		Time firstPacketArrivalTime;
		Time digestReceivedTime;
		EventId digestProcessEvent;
        std::vector<DigestEntry> entries;
        std::vector<BufferedAggregatedPacket> packets;
        bool digestReceived;
		bool digestProcessed;
		bool rawFallbackReady;                      // true => release raw buffered packets without fake/repair path
    };

	// Keyed by the digest batch start PSN.
	std::unordered_map<uint64_t, DigestBatch> m_digestBatches;
	uint64_t m_nextSailBatchToRelease;

    void BufferAggregatedPacket(Ptr<Packet> packet, CustomHeader &ch, uint64_t aggregatedPsn);
    void ProcessDigestPacket(Ptr<Packet> packet, CustomHeader &ch);
    void MarkSailBatchReady(uint64_t batchKey);
    void GenerateFakePacketsAndForward(uint64_t batchKey);
    void ReleaseRawSailBatch(uint64_t batchKey);
    void TryReleaseReadySailBatches();
    void ClearDigestBatch(uint64_t batchKey);
	const Time DIGEST_SCAN_INTERVAL = MicroSeconds(1000);
	void SendSailLossReportToHost(const DigestEntry& entry, uint32_t inDev);
	void SendLr2SwitchNack(
		const FiveTuple& flow,
		uint64_t lossPsn,
		uint16_t gapPkts,
		uint32_t inDev);
	void SendLr2HostTypeNack(const FiveTuple& flow, uint64_t lossPsn, uint32_t inDev);
	void SendLr2SyntheticAck(const FiveTuple& flow, uint64_t ackSeq, uint32_t inDev);
	void CheckRetransmissionTimeout();
	bool IsWanPort(uint32_t ifIndex) const;
	bool IsLr2Enabled() const;
	bool IsFlowAggregationActive() const;
	bool IsReliabilityDataPacket(const CustomHeader& ch) const;
	bool IsRepeatPacket(const CustomHeader& ch) const;
	bool IsSailFlowTailPacket(const CustomHeader& ch) const;
	FiveTuple BuildFlowKeyFromUdp(const CustomHeader& ch) const;
	FiveTuple BuildFlowKeyFromAck(const CustomHeader& ch) const;
	uint32_t GetPayloadSize(Ptr<Packet> packet, const CustomHeader& ch) const;
	uint16_t ComputeLr2GapPkts(uint64_t startSeq, uint64_t endSeq, uint32_t stepBytes) const;
	uint16_t DecodeLr2GapPkts(uint16_t flags) const;
	uint64_t ComputeLr2GapEndSeq(uint64_t startSeq, uint16_t gapPkts, uint32_t stepBytes) const;
	void RecordLr2SentryMissingRange(
		Lr2FlowState& state,
		uint64_t startSeq,
		uint16_t gapPkts,
		uint32_t stepBytes);
	void TrimLr2SentryMissingRanges(Lr2FlowState& state, uint64_t expectedSeq);
	bool ConsumeLr2SentryMissingSeq(
		Lr2FlowState& state,
		uint64_t seq,
		uint32_t stepBytes);
	void ResetLr2SentryState(Lr2FlowState& state, uint64_t expectedSeq);
	void ClearLr2DepotPools(Lr2FlowState& state);
	bool TryHandleLr2Sentry(
		uint32_t inDev,
		int outPort,
		Ptr<Packet> packet,
		CustomHeader& ch);
	bool TryHandleLr2DepotData(
		uint32_t inDev,
		Ptr<Packet> packet,
		CustomHeader& ch);
	bool TryHandleLr2HostNack(
		uint32_t inDev,
		int outPort,
		CustomHeader& ch);
	void TryResetLr2SentryOnHostNack(
		uint32_t inDev,
		CustomHeader& ch);
	void CheckLr2DepotTimeout();
	void Lr2DepotRetransmitFromBackup(
		Lr2FlowState& state,
		const FiveTuple& flow,
		uint32_t inDev,
		uint64_t startSeq);
	bool TryHandleLr2DepotAck(
		uint32_t inDev,
		CustomHeader& ch);
	void TryRecordLr2SentryFilter(
		uint32_t inDev,
		CustomHeader& ch);
	EventId m_lr2DepotTimerEvent;
	void RecordLr2BackupPacket(
		Lr2FlowState& state,
		uint64_t seq,
		Ptr<Packet> packet,
		const CustomHeader& ch,
		uint32_t payloadSize);
	void FlushLr2DepotBuffered(
		uint32_t inDev,
		Lr2FlowState& state,
		const FiveTuple& flow);

	int GetOutDev(Ptr<const Packet>, CustomHeader &ch);
	void SendToDev(Ptr<Packet>p, CustomHeader &ch);
	static uint32_t EcmpHash(const uint8_t* key, size_t len, uint32_t seed);
	void CheckAndSendPfc(uint32_t inDev, uint32_t qIndex);
	void CheckAndSendResume(uint32_t inDev, uint32_t qIndex);
	uint64_t GetCurrentSwitchRecoveryBufferPkts() const;
	void RefreshSwitchRecoveryBufferPeak();
	public:
	Ptr<SwitchMmu> m_mmu;
	uint32_t m_mtu;

	static TypeId GetTypeId (void);
	SwitchNode();
	void SetEcmpSeed(uint32_t seed);
	void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
	void ClearTable();
	bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch);
	void SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);

	// for approximate calc in PINT
	int logres_shift(int b, int l);
	int log2apprx(int x, int b, int m, int l); // given x of at most b bits, use most significant m bits of x, calc the result in l bits
	
	// for monitor
	uint64_t last_txBytes[pCnt]; // last sampling of the counter of tx bytes
	uint64_t last_port_qlen[pCnt]; // last sampling of the port length
	
	/**
	 * outoput format:
	 * time, sw_id, port_id, q_id, qlen, port_len
	 */
	void PrintSwitchQlen(FILE* qlen_output);
	/**
	 * outoput format:
	 * time, sw_id, port_id, txBytes
	 */
	void PrintSwitchBw(FILE* bw_output, uint32_t bw_mon_interval);
	uint64_t GetLr2ReorderPoolPeakPkts() const;
	uint64_t GetLr2BackupPoolPeakPkts() const;
	uint64_t GetSailBufferedPeakPkts() const;
	uint64_t GetSwitchRecoveryBufferPeakPkts() const;
	uint64_t GetWanTxBytes() const;
	void DumpLr2DebugState() const;

	// Timer lifecycle management.
	virtual void DoInitialize(void);
    virtual void DoDispose(void);
};

} /* namespace ns3 */

#endif /* SWITCH_NODE_H */
