#include <execinfo.h>
#include <stdio.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include "common.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif
#ifdef NS3_MPI
#include <mpi.h>
#include "ns3/mpi-interface.h"
#endif

using namespace std;
using namespace ns3;

extern uint32_t node_num, switch_num, link_num, trace_num, nvswitch_num,
    gpus_per_server;

extern std::unordered_map<uint32_t, unordered_map<uint32_t, uint16_t>>
    portNumber;

extern std::ifstream flowf;
extern FlowInput flow_input;

uint32_t flow_num_finished = 0;
uint32_t stop_flow_num_finished = 0;
bool simulation_stop_requested = false;

struct CollectivePlanEntry {
  uint32_t group;
  uint32_t phase;
  uint32_t rank;
  uint32_t src;
  uint32_t dst;
  uint32_t dport;
  uint64_t size;
};

uint32_t collective_group_count = 0;
uint32_t collective_rank_count = 0;
uint32_t collective_phase_count = 0;
double collective_start_time = 1.0;
uint32_t collective_groups_done = 0;
std::vector<std::vector<std::vector<CollectivePlanEntry>>> collective_plan;
std::vector<uint32_t> collective_current_phase;
std::vector<uint32_t> collective_pending;
std::vector<uint64_t> collective_phase_max_finish_ns;
std::vector<bool> collective_group_done;

void PrintSwitchStatsSnapshot() {
  for (uint32_t i = 0; i < n.GetN(); ++i) {
    if (n.Get(i)->GetNodeType() != 1) {
      continue;
    }
    Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n.Get(i));
    if (sw == nullptr) {
      continue;
    }
    uint64_t lr2ReorderPeak = sw->GetLr2ReorderPoolPeakPkts();
    uint64_t lr2BackupPeak = sw->GetLr2BackupPoolPeakPkts();
    uint64_t sailBufferPeak = sw->GetSailBufferedPeakPkts();
    uint64_t switchRecoveryPeak = sw->GetSwitchRecoveryBufferPeakPkts();
    uint64_t wanTxBytes = sw->GetWanTxBytes();
    if (lr2ReorderPeak == 0 && lr2BackupPeak == 0 && sailBufferPeak == 0 &&
        wanTxBytes == 0) {
      continue;
    }
    std::cout << "at " << Simulator::Now().GetNanoSeconds()
              << "ns, switch stats, node: " << sw->GetId()
              << " lr2_reorder_peak_pkts: " << lr2ReorderPeak
              << " lr2_backup_peak_pkts: " << lr2BackupPeak
              << " sail_buffer_peak_pkts: " << sailBufferPeak
              << " switch_recovery_buf_peak_pkts: " << switchRecoveryPeak
              << " wan_tx_bytes: " << wanTxBytes << std::endl;
    sw->DumpLr2DebugState();
  }
}

void DumpSwitchStatsPeriodically() {
  PrintSwitchStatsSnapshot();
  Simulator::Schedule(MilliSeconds(100), &DumpSwitchStatsPeriodically);
}

void ReadFlowInput() {
  if (flow_input.idx < flow_num) {
    flowf >> flow_input.src >> flow_input.dst >> flow_input.pg >>
        flow_input.dport >> flow_input.maxPacketCount >> flow_input.start_time;
    NS_ASSERT(
        n.Get(flow_input.src)->GetNodeType() == 0 &&
        n.Get(flow_input.dst)->GetNodeType() == 0);
  }
}
void ScheduleFlowInputs() {
  while (flow_input.idx < flow_num &&
         Seconds(flow_input.start_time) == Simulator::Now()) {
    uint32_t port =
        portNumber[flow_input.src][flow_input.dst]++; // get a new port number
    RdmaClientHelper clientHelper(
        (uint16_t)flow_input.pg,
        serverAddress[flow_input.src],
        serverAddress[flow_input.dst],
        port,
        flow_input.dport,
        flow_input.maxPacketCount,
        has_win ? (global_t == 1
                       ? maxBdp
                       : pairBdp[n.Get(flow_input.src)][n.Get(flow_input.dst)])
                : 0,
        global_t == 1 ? maxRtt : pairRtt[flow_input.src][flow_input.dst],
        nullptr,
        nullptr,
        1,
        flow_input.src,
        flow_input.dst);
    ApplicationContainer appCon = clientHelper.Install(n.Get(flow_input.src));
    appCon.Start(Time(0));
    std::cout << "Register flow " << flow_input.idx << " src: " << flow_input.src
              << " dst: " << flow_input.dst << " pg: " << flow_input.pg
              << " sport: " << port
              << " dport: " << flow_input.dport
              << " maxPacketCount: " << flow_input.maxPacketCount // Stored as bytes in this trace format.
              << " start_time: " << flow_input.start_time << std::endl;

    // get the next flow input
    flow_input.idx++;
    ReadFlowInput();
  }

  // schedule the next time to run this function
  if (flow_input.idx < flow_num) {
    Simulator::Schedule(
        Seconds(flow_input.start_time) - Simulator::Now(), ScheduleFlowInputs);
  } else { // no more flows, close the file
    flowf.close();
  }
}

void LoadCollectivePlan() {
  NS_ASSERT_MSG(collective_mode == "allreduce", "LoadCollectivePlan called outside collective mode");
  std::ifstream plan(collective_plan_file.c_str());
  NS_ABORT_MSG_IF(!plan.is_open(), "Failed to open COLLECTIVE_PLAN_FILE: " << collective_plan_file);

  plan >> collective_group_count >> collective_rank_count >> collective_phase_count >> collective_start_time;
  NS_ABORT_MSG_IF(collective_group_count == 0 || collective_rank_count == 0 || collective_phase_count == 0,
                  "Invalid collective plan header");

  collective_plan.assign(
      collective_group_count,
      std::vector<std::vector<CollectivePlanEntry>>(collective_phase_count));

  CollectivePlanEntry entry;
  uint64_t entry_count = 0;
  while (plan >> entry.group >> entry.phase >> entry.rank >> entry.src >>
             entry.dst >> entry.dport >> entry.size) {
    NS_ABORT_MSG_IF(entry.group >= collective_group_count, "Invalid collective group");
    NS_ABORT_MSG_IF(entry.phase >= collective_phase_count, "Invalid collective phase");
    NS_ABORT_MSG_IF(entry.rank >= collective_rank_count, "Invalid collective rank");
    NS_ABORT_MSG_IF(n.Get(entry.src)->GetNodeType() != 0 || n.Get(entry.dst)->GetNodeType() != 0,
                    "Collective endpoints must be host nodes");
    collective_plan[entry.group][entry.phase].push_back(entry);
    entry_count++;
  }

  uint64_t expected = (uint64_t)collective_group_count * collective_phase_count * collective_rank_count;
  NS_ABORT_MSG_IF(entry_count != expected,
                  "Collective plan has " << entry_count << " entries, expected " << expected);
  for (uint32_t group = 0; group < collective_group_count; group++) {
    for (uint32_t phase = 0; phase < collective_phase_count; phase++) {
      NS_ABORT_MSG_IF(collective_plan[group][phase].size() != collective_rank_count,
                      "Collective group/phase entry count mismatch");
    }
  }

  collective_current_phase.assign(collective_group_count, 0);
  collective_pending.assign(collective_group_count, 0);
  collective_phase_max_finish_ns.assign(collective_group_count, 0);
  collective_group_done.assign(collective_group_count, false);
  collective_groups_done = 0;

  std::cout << "Loaded AllReduce collective plan groups: " << collective_group_count
            << " ranks: " << collective_rank_count
            << " phases: " << collective_phase_count
            << " entries: " << entry_count
            << " start_time: " << collective_start_time << std::endl;
}

void StartCollectiveMessage(const CollectivePlanEntry &entry) {
  uint32_t port = portNumber[entry.src][entry.dst]++;
  RdmaClientHelper clientHelper(
      3,
      serverAddress[entry.src],
      serverAddress[entry.dst],
      port,
      entry.dport,
      entry.size,
      has_win ? (global_t == 1
                     ? maxBdp
                     : pairBdp[n.Get(entry.src)][n.Get(entry.dst)])
              : 0,
      global_t == 1 ? maxRtt : pairRtt[entry.src][entry.dst],
      nullptr,
      nullptr,
      1,
      entry.src,
      entry.dst);
  ApplicationContainer appCon = clientHelper.Install(n.Get(entry.src));
  appCon.Start(Time(0));
  std::cout << "at " << Simulator::Now().GetNanoSeconds()
            << "ns, collective start, group: " << entry.group
            << " phase: " << entry.phase
            << " rank: " << entry.rank
            << " src: " << entry.src
            << " dst: " << entry.dst
            << " sport: " << port
            << " dport: " << entry.dport
            << " size: " << entry.size << std::endl;
}

void StartCollectivePhase(uint32_t group) {
  NS_ABORT_MSG_IF(group >= collective_group_count, "Invalid collective group start");
  if (collective_group_done[group]) {
    return;
  }
  uint32_t phase = collective_current_phase[group];
  NS_ABORT_MSG_IF(phase >= collective_phase_count, "Collective phase out of range");
  const auto &entries = collective_plan[group][phase];
  collective_pending[group] = entries.size();
  collective_phase_max_finish_ns[group] = 0;
  std::cout << "at " << Simulator::Now().GetNanoSeconds()
            << "ns, collective phase start, group: " << group
            << " phase: " << phase
            << " pending: " << collective_pending[group] << std::endl;
  for (const auto &entry : entries) {
    StartCollectiveMessage(entry);
  }
}

void CompleteCollectivePhase(uint32_t group, uint32_t phase, uint64_t phase_finish_ns) {
#ifdef NS3_MTP
  MtpInterface::explicitCriticalSection cs;
#endif
  bool request_stop = false;
  NS_ABORT_MSG_IF(group >= collective_group_count, "Invalid collective group completion");
  NS_ABORT_MSG_IF(phase != collective_current_phase[group], "Collective phase completion out of order");
  NS_ABORT_MSG_IF(collective_pending[group] != 0, "Collective phase completed with pending messages");

  std::cout << "at " << phase_finish_ns
            << "ns, collective phase complete, group: " << group
            << " phase: " << phase << std::endl;
  collective_current_phase[group]++;
  if (collective_current_phase[group] < collective_phase_count) {
    Simulator::Schedule(NanoSeconds(0), &StartCollectivePhase, group);
#ifdef NS3_MTP
    cs.ExitSection();
#endif
    return;
  }

  collective_group_done[group] = true;
  collective_groups_done++;
  std::cout << "at " << phase_finish_ns
            << "ns, collective group complete, group: " << group << std::endl;
  if (collective_groups_done == collective_group_count && !simulation_stop_requested) {
    simulation_stop_requested = true;
    request_stop = true;
  }
#ifdef NS3_MTP
  cs.ExitSection();
#endif
  if (request_stop) {
    PrintSwitchStatsSnapshot();
    cancel_monitor();
    Simulator::Stop(Seconds(collective_stop_delay_ms / 1000.0));
  }
}

void HandleCollectiveMessageComplete(Ptr<RdmaQueuePair> q) {
#ifdef NS3_MTP
  MtpInterface::explicitCriticalSection cs;
#endif
  uint32_t dport = q->dport;
  NS_ABORT_MSG_IF(dport < 10000, "Collective message has non-collective dport");
  uint32_t encoded = dport - 10000;
  uint32_t group = encoded / 1000;
  uint32_t rem = encoded % 1000;
  uint32_t phase = rem / 32;
  uint32_t rank = rem % 32;
  NS_ABORT_MSG_IF(group >= collective_group_count || phase >= collective_phase_count ||
                      rank >= collective_rank_count,
                  "Collective dport decode out of range");
  NS_ABORT_MSG_IF(collective_group_done[group], "Collective message completed after group done");
  NS_ABORT_MSG_IF(phase != collective_current_phase[group], "Collective message completed for a non-current phase");
  NS_ABORT_MSG_IF(collective_pending[group] == 0, "Collective pending count underflow");

  uint64_t now_ns = Simulator::Now().GetNanoSeconds();
  collective_phase_max_finish_ns[group] =
      std::max<uint64_t>(collective_phase_max_finish_ns[group], now_ns);
  collective_pending[group]--;
  if (collective_pending[group] != 0) {
#ifdef NS3_MTP
    cs.ExitSection();
#endif
    return;
  }

  uint64_t phase_finish_ns = collective_phase_max_finish_ns[group];
  uint64_t delay_ns = phase_finish_ns > now_ns ? phase_finish_ns - now_ns : 0;
  Simulator::Schedule(NanoSeconds(delay_ns), &CompleteCollectivePhase, group, phase, phase_finish_ns);
#ifdef NS3_MTP
  cs.ExitSection();
#endif
}

// Helper function to delete RxQp after delay
void delayed_delete_rxqp(Ptr<RdmaDriver> rdma, uint32_t sip, uint16_t pg, uint16_t sport) {
  rdma->m_rdma->DeleteRxQp(sip, pg, sport);
}

void print_qp_resource_stats(Ptr<RdmaQueuePair> q) {
  uint32_t sid = ip_to_node_id(q->sip), did = ip_to_node_id(q->dip);
  uint32_t peakRxBufPkts = 0;
  uint64_t peakRxBufBytes = 0;
  uint64_t sailSwitchNackTimeouts = 0;

  Ptr<Node> dstNode = n.Get(did);
  Ptr<RdmaDriver> rdma = dstNode->GetObject<RdmaDriver>();
  Ptr<RdmaRxQueuePair> rxQp =
      rdma->m_rdma->GetRxQp(q->dip.Get(), q->sip.Get(), q->dport, q->sport, q->m_pg, false);
  if (rxQp != nullptr) {
    peakRxBufPkts = rxQp->m_max_receive_buffer_num;
    peakRxBufBytes = (uint64_t)peakRxBufPkts * rdma->m_rdma->m_mtu;
    sailSwitchNackTimeouts = rxQp->m_sailSwitchNackTimeoutCount;
  }

  std::cout << "at " << Simulator::Now().GetNanoSeconds()
            << "ns, qp stats, src: " << sid
            << " did: " << did
            << " port: " << q->sport
            << " retx: " << q->m_retx_count
            << " nack_recv: " << q->m_nack_recv_count
            << " rto: " << q->m_rto_count
            << " switch_nack_timeout: " << sailSwitchNackTimeouts
            << " peak_rx_buf_pkts: " << peakRxBufPkts
            << " peak_rx_buf_bytes: " << peakRxBufBytes
            << " endpoint_recovery_buf_pkts: " << peakRxBufPkts
            << " endpoint_recovery_buf_bytes: " << peakRxBufBytes << std::endl;
}


void qp_finish_normal(FILE* fout, Ptr<RdmaQueuePair> q) {
  uint32_t sid = ip_to_node_id(q->sip), did = ip_to_node_id(q->dip);
  #ifdef NS3_MTP
  MtpInterface::explicitCriticalSection cs;
  #endif
  Ptr<Node> dstNode = n.Get(did);
  Ptr<RdmaDriver> rdma = dstNode->GetObject<RdmaDriver>();
  
  // Delay RxQp deletion by 50ms to allow late-arriving packets to be processed
  // This prevents NACK storms caused by delayed packets arriving after flow completion
  Simulator::Schedule(MilliSeconds(30), &delayed_delete_rxqp, rdma, q->sip.Get(), q->m_pg, q->sport);
  
  std::cout << "at "<< Simulator::Now().GetNanoSeconds()<<"ns, qp finish, src: " << sid << " did: " << did
            << " port: " << q->sport << std::endl;
  #ifdef NS3_MTP
  cs.ExitSection();
  #endif
  print_qp_resource_stats(q);
}

void send_finish_normal(FILE* fout, Ptr<RdmaQueuePair> q) {
  // Currently do nothing
  // uint32_t sid = ip_to_node_id(q->sip), did = ip_to_node_id(q->dip);
}

void message_finish_normal(FILE* fout, Ptr<RdmaQueuePair> q, uint64_t msgSize){
  uint32_t sid = ip_to_node_id(q->sip), did = ip_to_node_id(q->dip);
  uint64_t base_rtt = pairRtt[sid][did], b = pairBw[sid][did];
  uint32_t packet_payload_size =
      get_config_value_ns3<uint64_t>("ns3::RdmaHw::Mtu");
  uint64_t size = msgSize;
  uint32_t total_bytes = size +
      ((size - 1) / packet_payload_size + 1) *
          (CustomHeader::GetStaticWholeHeaderSize() -
           IntHeader::GetStaticSize()); // translate to the minimum bytes
                                        // required (with header but no INT)
  uint64_t standalone_fct = base_rtt + total_bytes * 8000000000lu / b;
  fprintf(
      fout,
      "%08x %08x %u %u %lu %lu %lu %lu\n",
      q->sip.Get(),
      q->dip.Get(),
      q->sport,
      q->dport,
      size,
      q->startTime.GetTimeStep(),
      (Simulator::Now() - q->startTime).GetTimeStep(),
      standalone_fct);
  fflush(fout);

  // std::cout << "at "<< Simulator::Now().GetNanoSeconds()<<"ns, message finish, src: " << sid << " did: " << did
  //           << " port: " << q->sport << " total bytes: " << size<< std::endl;
  // Ptr<Node> dstNode = n.Get(did);
  // Ptr<RdmaDriver> rdma = dstNode->GetObject<RdmaDriver>();
  // rdma->m_rdma->DeleteRxQp(q->sip.Get(), q->m_pg, q->sport);
  if (collective_mode == "allreduce") {
    flow_num_finished++;
    HandleCollectiveMessageComplete(q);
    return;
  }
  flow_num_finished++;
  bool should_stop = false;
  if (stop_on_foreground_only != 0) {
    if (q->dport == stop_flow_dport) {
      stop_flow_num_finished++;
      uint32_t target_stop_count = stop_flow_count == 0 ? flow_num : stop_flow_count;
      should_stop = stop_flow_num_finished >= target_stop_count;
    }
  } else {
    should_stop = flow_num_finished == flow_num;
  }
  if(should_stop){
    cancel_monitor();
    if (!simulation_stop_requested) {
      simulation_stop_requested = true;
      Simulator::Stop(MilliSeconds(35));
    }
  }
}

int main(int argc, char* argv[]) {
#ifdef NS3_MPI
  ns3::MpiInterface::Enable(&argc, &argv);
// GlobalValue::Bind ("SimulatorImplementationType",
//                    StringValue ("ns3::DistributedSimulatorImpl"));
#endif

  // MPI_Init(&argc, &argv);
  float comm_scale = 1;
  uint32_t mtp_threads = 16;

  CommandLine cmd;
  cmd.AddValue("commscale", "Communication Scale", comm_scale);
#ifdef NS3_MTP
  cmd.AddValue(
      "mtpThreads",
      "MTP thread count. Set to 0 to disable MTP for deterministic single-thread runs.",
      mtp_threads);
#endif
  cmd.Parse(argc, argv);

#ifdef NS3_MTP
  if (mtp_threads > 0) {
    MtpInterface::Enable(mtp_threads);
  }
#endif

  clock_t begint, endt;
  begint = clock();

  if (!ReadConf(argc, argv))
    return -1;
  SetConfig();
  SetupNetwork(qp_finish_normal, send_finish_normal, message_finish_normal);

  //
  // Now, do the actual simulation.
  //
  std::cout << "Running Simulation.\n";
  fflush(stdout);
  NS_LOG_INFO("Run Simulation.");

  std::cout << "Flow num: " << flow_num << std::endl;
  if (collective_mode == "allreduce") {
    LoadCollectivePlan();
    for (uint32_t group = 0; group < collective_group_count; group++) {
      Simulator::Schedule(Seconds(collective_start_time), &StartCollectivePhase, group);
    }
  } else {
    NS_ABORT_MSG_IF(collective_mode != "none", "Unknown COLLECTIVE_MODE: " << collective_mode);
    flow_input.idx = 0;
    if (flow_num > 0) {
      ReadFlowInput();
      Simulator::Schedule(
          Seconds(flow_input.start_time) - Simulator::Now(), ScheduleFlowInputs);
    }
  }

  Simulator::Schedule(MilliSeconds(100), &DumpSwitchStatsPeriodically);

  Simulator::Stop(Seconds(simulator_stop_time));
  Simulator::Run();
  PrintSwitchStatsSnapshot();
  // Simulator::Stop(TimeStep (0x7fffffffffffffffLL));
  Simulator::Destroy();

  endt = clock();
  std::cout << (double)(endt - begint) / CLOCKS_PER_SEC << "\n";
  return 0;
}
