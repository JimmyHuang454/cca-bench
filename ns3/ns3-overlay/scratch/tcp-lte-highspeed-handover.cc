/*
 * Copyright (c) 2024
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TCP congestion control for a mobile user on a 4G LTE network: a passenger
 * in a fast vehicle (train or car) or a pedestrian walking or running in a
 * city.
 *
 * One UE moves along a line of eNodeBs and performs A3/RSRP handovers between
 * successive cells. Bulk TCP downloads run from remote hosts to the UE across
 * the LTE/EPC network, so the congestion control algorithm sees the capacity
 * variation and the handover interruptions of the radio link.
 *
 * Two ways to describe the route:
 *   - options: --speed, --numberOfEnbs, --enbSpacing, --trackOffset,
 *     --carLoss, --tunnelStart/--tunnelLength (straight route, constant speed)
 *   - --scenario: a 3-D line (scratch/lines/<name>.json resolved by
 *     scratch/preview-line.py --export) with a speed profile over time, cells
 *     beside the track with their own antennas, tunnels and interference;
 *     pedestrian lines also use extra losses over time (buildings, elevator
 *     cars) and dense crowds (other users in the same cell)
 *
 * The algorithm is selected with --cc=cubic|bbr|bbrv2l|bbrv3l|copa|copa-d<delta>
 * (copa is Copa with the default delta 0.5, e.g. copa-d0.1 uses delta = 0.1).
 * --rttProbe=true replaces TCP with a 64-byte UDP echo every 10 ms (RTT of the
 * unloaded path, *-rttprobe-idealrtt.data).
 * --capacityProbe=true replaces TCP with a saturating UDP flow that measures
 * the capacity of the same channel realisation. Radio, RLC, handover, core
 * network, TCP and RNG parameters are in mobile-scenario-params.h (see --help).
 *
 * Output files (base = <prefix>-<cc>, or <prefix> for the capacity probe):
 *   -throughput.data : time, total Mbps, serving cell, RSRP, Mbps of each flow
 *   -rsrp.data       : time, serving cell, RSRP, SINR
 *   -handover.data   : handover start/end events
 *   -cwnd.data       : congestion window of flow 0
 *   -rtt.data        : time, flow, smoothed RTT [ms] (one line per change)
 *   -metrics.json    : configuration and per-flow summary metrics
 *   -flowmonitor.xml : FlowMonitor statistics
 *   -trajectory.data : (--scenario) position, speed, interference, cell, SINR
 *   -line.data       : (--scenario) cells and their computed TX power
 *
 * Normally run through scratch/run-sweep.py (studies hsr-4g, hwy-4g, hsr-line,
 * hwy-line, walk-line, run-line), which sets all options.
 */

#include "mobile-scenario-params.h"
#include "mortise-workload.h"
#include "track-loss-model.h"
#include "line3d.h"

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/lte-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <memory>
#include <functional>
#include <iomanip>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TcpLteHighSpeedHandover");

namespace
{

std::ofstream g_throughputFile; //!< Interval throughput trace
std::ofstream g_rsrpFile;       //!< Periodic RSRP/SINR trace
std::ofstream g_handoverFile;   //!< Handover events
std::ofstream g_cwndFile;       //!< Sender congestion window of flow 0
std::ofstream g_rttFile;        //!< Smoothed RTT of every flow
std::ofstream g_rttSampleFile;  //!< RTT samples (last RTT) averaged per 100 ms bin
std::ofstream g_idealRttFile;   //!< RTT of the unloaded echo probe (--rttProbe)

std::vector<double> g_binRttSum;    //!< Sum of the RTT samples of the current bin, per flow
std::vector<uint32_t> g_binRttCount; //!< Number of RTT samples of the current bin, per flow
std::ofstream g_stateFile;      //!< Congestion state / in-flight of flow 0 (debug)

std::vector<Ptr<PacketSink>> g_sinks;   //!< Downlink sinks on the UE, one per flow
std::vector<uint64_t> g_lastRxBytes;    //!< Last sampled received bytes, per flow
std::vector<std::vector<double>> g_rtt; //!< RTT samples [ms], per flow
std::vector<std::vector<double>> g_srtt; //!< Smoothed RTT updates for diagnostics
std::ofstream g_rawRttLog; //!< Optional log of every valid ACK RTT sample
std::vector<uint32_t> g_retx;           //!< Retransmitted segments, per flow
Time g_lastSampleTime;                  //!< Time of the last throughput sample

double g_lastRsrp = 0.0;   //!< Last reported RSRP
double g_lastSinr = 0.0;   //!< Last reported SINR
uint16_t g_lastCellId = 0; //!< Last serving cell id
bool g_haveRsrp = false;   //!< Whether a RSRP sample is available

uint32_t g_handoverCount = 0;    //!< Number of successful handovers
uint32_t g_rlfCount = 0;         //!< Number of radio link failures
double g_samplePeriodMs = 100.0; //!< Throughput sampling period

std::string
BaseCc(const std::string& cc)
{
    return cc.substr(0, cc.find('+'));
}

bool
IsCopaKernelName(const std::string& name)
{
    const std::string cc = BaseCc(name);
    return cc == "copa" || cc == "copa-auto" || cc == "copa_auto" || cc == "copa_auto_nc" ||
           cc == "copa_auto_nc_j0" || cc == "copa_auto_nc_j3" || cc == "copa_orig" ||
           cc.rfind("copa-d", 0) == 0 || cc.rfind("copa_auto_theta", 0) == 0 ||
           cc.rfind("copa_auto_nc_theta", 0) == 0;
}

bool
IsAdaptiveCopaName(const std::string& name)
{
    const std::string cc = BaseCc(name);
    return cc == "copa-auto" || cc == "copa_auto" || cc == "copa_auto_nc" ||
           cc == "copa_auto_nc_j0" || cc == "copa_auto_nc_j3" ||
           cc.rfind("copa_auto_theta", 0) == 0 || cc.rfind("copa_auto_nc_theta", 0) == 0;
}

std::ofstream g_copaEstFile; //!< Estimates of the adaptive Copa delta (copa-auto, flow 0)

void
CopaEstimateTracer(double jitter, double ratio, double rho, double baseRtt, double dqTarget, double delta)
{
    if (g_copaEstFile.is_open())
    {
        g_copaEstFile << Simulator::Now().GetSeconds() << " " << jitter * 1e3 << " "
                      << ratio * 1e3 << " " << rho << " " << baseRtt * 1e3 << " "
                      << dqTarget * 1e3 << " " << delta << "\n";
    }
}

void
ConfigureCcFlow(uint32_t nodeId,
                uint32_t flow,
                std::string name,
                int32_t jitterUs,
                uint32_t copaClockUs)
{
    const std::string cc = BaseCc(name);
    if (cc == "mortise_copa")
    {
        const std::string path = "/NodeList/" + std::to_string(nodeId) +
                                 "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpMortiseCopa/";
        Config::Set(path + "CopaClockUs", UintegerValue(std::max<uint32_t>(copaClockUs, 1U)));
        return;
    }
    if (!IsCopaKernelName(name))
    {
        return;
    }

    const std::string path = "/NodeList/" + std::to_string(nodeId) +
                             "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpCopaKernel/";
    const bool nosw = (name.find("+nosw") != std::string::npos);

    bool adaptive = false;
    bool modeSwitching = !nosw;
    int32_t effJitterUs = jitterUs;
    uint32_t thetaMilli = 500U;
    uint32_t initDeltaQ20 = 41943U; /* 0.04 * 2^20 */

    if (cc == "copa" || cc == "copa_orig")
    {
        adaptive = false;
        modeSwitching = !nosw;
        initDeltaQ20 = 524288U;
    }
    else if (cc.rfind("copa-d", 0) == 0)
    {
        const double d = std::stod(cc.substr(6));
        NS_ABORT_MSG_UNLESS(d > 0 && d <= 1, "Copa delta must be in (0, 1]: " << cc);
        adaptive = false;
        modeSwitching = !nosw;
        initDeltaQ20 = static_cast<uint32_t>(std::clamp(std::round(d * 1048576.0), 1049.0, 1048576.0));
    }
    else if (cc == "copa-auto" || cc == "copa_auto")
    {
        adaptive = true;
        modeSwitching = !nosw;
    }
    else if (cc == "copa_auto_nc")
    {
        adaptive = true;
        modeSwitching = false;
    }
    else if (cc == "copa_auto_nc_j0")
    {
        adaptive = true;
        modeSwitching = false;
        effJitterUs = 0;
    }
    else if (cc == "copa_auto_nc_j3")
    {
        adaptive = true;
        modeSwitching = false;
        effJitterUs = 3000;
    }
    else if (cc.rfind("copa_auto_theta", 0) == 0)
    {
        double th = std::stod(cc.substr(15));
        adaptive = true;
        modeSwitching = !nosw;
        thetaMilli = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(std::round(th * 1000.0)), 10, 995));
    }
    else if (cc.rfind("copa_auto_nc_theta", 0) == 0)
    {
        double th = std::stod(cc.substr(18));
        adaptive = true;
        modeSwitching = false;
        thetaMilli = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(std::round(th * 1000.0)), 10, 995));
    }

    Config::Set(path + "Adaptive", BooleanValue(adaptive));
    Config::Set(path + "ModeSwitching", BooleanValue(modeSwitching));
    Config::Set(path + "JitterUs", IntegerValue(effJitterUs));
    Config::Set(path + "ThetaMilli", UintegerValue(thetaMilli));
    Config::Set(path + "InitDeltaQ20", UintegerValue(initDeltaQ20));
    Config::Set(path + "CopaClockUs", UintegerValue(std::max<uint32_t>(copaClockUs, 1U)));
    if (adaptive && flow == 0 && g_copaEstFile.is_open())
    {
        Config::ConnectWithoutContext(path + "Estimates", MakeCallback(&CopaEstimateTracer));
    }
}

std::string
CcTypeName(const std::string& name)
{
    const std::string cc = BaseCc(name);
    if (cc == "cubic")
    {
        return "ns3::TcpCubic";
    }
    if (cc == "bbr")
    {
        return "ns3::TcpBbr";
    }
    if (cc == "bbr2" || cc == "bbrv2" || cc == "bbrv2l")
    {
        return "ns3::TcpBbrV2";
    }
    if (cc == "bbr3" || cc == "bbrv3" || cc == "bbrv3l")
    {
        return "ns3::TcpBbrV3";
    }
    if (cc == "vegas")
    {
        return "ns3::TcpVegas";
    }
    if (cc == "veno")
    {
        return "ns3::TcpVeno";
    }
    if (cc == "yeah")
    {
        return "ns3::TcpYeah";
    }
    if (cc == "illinois")
    {
        return "ns3::TcpIllinois";
    }
    if (cc == "westwood")
    {
        return "ns3::TcpWestwoodPlus";
    }
    if (cc == "hybla")
    {
        return "ns3::TcpHybla";
    }
    if (cc == "reno" || cc == "newreno")
    {
        return "ns3::TcpLinuxReno";
    }
    if (cc == "mortise_copa")
    {
        return "ns3::TcpMortiseCopa";
    }
    if (IsCopaKernelName(name))
    {
        return "ns3::TcpCopaKernel";
    }
    TypeId tid;
    NS_ABORT_MSG_UNLESS(TypeId::LookupByNameFailSafe(cc, &tid),
                        "Unknown congestion control '" << cc << "'");
    return cc;
}

/**
 * Return the p-th percentile of a sample set (nearest rank).
 *
 * @param v samples, modified (sorted)
 * @param p percentile in [0, 100]
 * @return percentile, or 0 when empty
 */
double
Percentile(std::vector<double>& v, double p)
{
    if (v.empty())
    {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    auto idx = static_cast<size_t>(std::ceil(p / 100.0 * v.size()));
    idx = std::min(std::max<size_t>(idx, 1), v.size()) - 1;
    return v[idx];
}

/**
 * Report a handover start event.
 *
 * @param context trace context
 * @param imsi UE IMSI
 * @param cellId serving cell id before the handover
 * @param rnti UE RNTI
 * @param targetCellId target cell id
 */
void
HandoverStart(std::string context [[maybe_unused]],
              uint64_t imsi [[maybe_unused]],
              uint16_t cellId,
              uint16_t rnti [[maybe_unused]],
              uint16_t targetCellId)
{
    std::cout << std::fixed << std::setprecision(3) << Simulator::Now().GetSeconds()
              << "s  handover START  cell " << cellId << " -> " << targetCellId << std::endl;
    if (g_handoverFile.is_open())
    {
        g_handoverFile << Simulator::Now().GetSeconds() << " start " << cellId << " "
                       << targetCellId << std::endl;
    }
}

/**
 * Report a successful handover completion.
 *
 * @param context trace context
 * @param imsi UE IMSI
 * @param cellId new serving cell id
 * @param rnti UE RNTI
 */
void
HandoverEndOk(std::string context [[maybe_unused]],
              uint64_t imsi [[maybe_unused]],
              uint16_t cellId,
              uint16_t rnti [[maybe_unused]])
{
    g_handoverCount++;
    std::cout << std::fixed << std::setprecision(3) << Simulator::Now().GetSeconds()
              << "s  handover END    cell " << cellId << std::endl;
    if (g_handoverFile.is_open())
    {
        g_handoverFile << Simulator::Now().GetSeconds() << " end " << cellId << std::endl;
    }
}

/**
 * Report a radio link failure (the UE then re-establishes the connection).
 *
 * @param context trace context
 * @param imsi UE IMSI
 * @param cellId serving cell id
 * @param rnti UE RNTI
 */
void
RadioLinkFailure(std::string context [[maybe_unused]],
                 uint64_t imsi [[maybe_unused]],
                 uint16_t cellId,
                 uint16_t rnti [[maybe_unused]])
{
    g_rlfCount++;
    if (g_handoverFile.is_open())
    {
        g_handoverFile << Simulator::Now().GetSeconds() << " rlf " << cellId << std::endl;
    }
}

/**
 * Cache the latest RSRP/SINR sample reported by the UE PHY. The PHY reports
 * linear units, so convert RSRP to dBm and SINR to dB for readability.
 *
 * @param context trace context
 * @param cellId cell id
 * @param rnti UE RNTI
 * @param rsrp linear RSRP in Watts
 * @param sinr linear SINR
 * @param componentCarrierId component carrier id
 */
void
RsrpSinrTrace(std::string context [[maybe_unused]],
              uint16_t cellId,
              uint16_t rnti [[maybe_unused]],
              double rsrp,
              double sinr,
              uint8_t componentCarrierId [[maybe_unused]])
{
    g_lastRsrp = 10.0 * std::log10(rsrp) + 30.0; // W -> dBm
    g_lastSinr = 10.0 * std::log10(sinr);        // linear -> dB
    g_lastCellId = cellId;
    g_haveRsrp = true;
}

/**
 * Sample the downlink throughput of every TCP sink and log it together with
 * the serving cell and the RSRP.
 */
void
SampleThroughput()
{
    double dt = (Simulator::Now() - g_lastSampleTime).GetSeconds();
    std::vector<double> perFlow;
    double total = 0.0;
    for (size_t i = 0; i < g_sinks.size(); i++)
    {
        uint64_t rx = g_sinks[i] ? g_sinks[i]->GetTotalRx() : 0;
        double mbps = dt > 0 ? (rx - g_lastRxBytes[i]) * 8.0 / dt / 1e6 : 0.0;
        g_lastRxBytes[i] = rx;
        perFlow.push_back(mbps);
        total += mbps;
    }

    if (g_throughputFile.is_open())
    {
        g_throughputFile << Simulator::Now().GetSeconds() << " " << total << " " << g_lastCellId
                         << " " << g_lastRsrp;
        if (perFlow.size() > 1)
        {
            for (double m : perFlow)
            {
                g_throughputFile << " " << m;
            }
        }
        g_throughputFile << std::endl;
    }
    if (g_rsrpFile.is_open() && g_haveRsrp)
    {
        g_rsrpFile << Simulator::Now().GetSeconds() << " " << g_lastCellId << " " << g_lastRsrp
                   << " " << g_lastSinr << std::endl;
    }

    g_lastSampleTime = Simulator::Now();
    Simulator::Schedule(MilliSeconds(g_samplePeriodMs), &SampleThroughput);
}

/**
 * Log the sender congestion window of flow 0.
 *
 * @param oldval previous cwnd
 * @param newval new cwnd
 */
void
CwndTracer(uint32_t oldval [[maybe_unused]], uint32_t newval)
{
    if (g_cwndFile.is_open())
    {
        g_cwndFile << Simulator::Now().GetSeconds() << " " << newval << std::endl;
    }
}

/**
 * Record a smoothed RTT update.
 *
 * @param flow flow index
 * @param oldval previous RTT
 * @param newval new RTT
 */
void
RttTracer(uint32_t flow, Time oldval [[maybe_unused]], Time newval)
{
    double ms = newval.GetSeconds() * 1000.0;
    if (ms <= 0)
    {
        return;
    }
    g_srtt[flow].push_back(ms);
    if (g_rttFile.is_open())
    {
        g_rttFile << Simulator::Now().GetSeconds() << " " << flow << " " << ms << std::endl;
    }
}

/**
 * Count a retransmitted segment.
 *
 * @param flow flow index
 * @param packet retransmitted packet
 * @param header TCP header
 * @param localAddr local address
 * @param peerAddr peer address
 * @param socket sending socket
 */
void
RetxTracer(uint32_t flow,
           Ptr<const Packet> packet [[maybe_unused]],
           const TcpHeader& header [[maybe_unused]],
           const Address& localAddr [[maybe_unused]],
           const Address& peerAddr [[maybe_unused]],
           Ptr<const TcpSocketBase> socket [[maybe_unused]])
{
    g_retx[flow]++;
}

/**
 * Log the congestion state of flow 0.
 *
 * @param oldval previous state
 * @param newval new state
 */
void
CongStateTracer(TcpSocketState::TcpCongState_t oldval [[maybe_unused]],
                TcpSocketState::TcpCongState_t newval)
{
    if (g_stateFile.is_open())
    {
        g_stateFile << Simulator::Now().GetSeconds() << " state " << newval << std::endl;
    }
}

/**
 * Log the RTO of flow 0.
 *
 * @param oldval previous RTO
 * @param newval new RTO
 */
void
RtoTracer(Time oldval [[maybe_unused]], Time newval)
{
    if (g_stateFile.is_open())
    {
        g_stateFile << Simulator::Now().GetSeconds() << " rto " << newval.GetMilliSeconds()
                    << std::endl;
    }
}

/**
 * Log a uint32 sender variable of flow 0 (debug).
 *
 * @param name variable name
 * @param oldval previous value
 * @param newval new value
 */
void
U32Tracer(std::string name, uint32_t oldval [[maybe_unused]], uint32_t newval)
{
    if (g_stateFile.is_open())
    {
        g_stateFile << Simulator::Now().GetSeconds() << " " << name << " " << newval << std::endl;
    }
}

/**
 * Accumulate one RTT sample (RTT of the last acknowledged segment).
 *
 * @param flow flow index
 * @param oldval previous value
 * @param newval new RTT sample
 */
void
LastRttTracer(uint32_t flow, Time newval)
{
    if (newval.IsStrictlyPositive() && flow < g_binRttSum.size())
    {
        g_binRttSum[flow] += newval.GetSeconds() * 1000.0;
        g_binRttCount[flow]++;
        g_rtt[flow].push_back(newval.GetSeconds() * 1000.0);
        if (g_rawRttLog.is_open())
        {
            g_rawRttLog << std::setprecision(12) << Simulator::Now().GetSeconds() << ","
                        << flow << "," << newval.GetSeconds() * 1000.0 << "\n";
        }
    }
}

/**
 * Write the mean RTT sample of every flow over the last 100 ms bin.
 */
void
FlushRttBins()
{
    for (uint32_t i = 0; i < g_binRttSum.size(); i++)
    {
        if (g_binRttCount[i] > 0)
        {
            g_rttSampleFile << Simulator::Now().GetSeconds() << " " << i << " "
                            << g_binRttSum[i] / g_binRttCount[i] << "\n";
        }
        g_binRttSum[i] = 0.0;
        g_binRttCount[i] = 0;
    }
    Simulator::Schedule(MilliSeconds(100), &FlushRttBins);
}

/**
 * Send one timestamped echo request of the RTT probe and schedule the next.
 *
 * @param socket UDP socket of the remote host
 * @param dest UE address and echo port
 * @param seq sequence number
 * @param interval time between requests
 * @param stop time of the last request
 */
void
SendRttProbe(Ptr<Socket> socket, InetSocketAddress dest, uint32_t seq, Time interval, Time stop)
{
    SeqTsHeader header;
    header.SetSeq(seq);
    Ptr<Packet> p = Create<Packet>(52);
    p->AddHeader(header);
    socket->SendTo(p, 0, dest);
    if (Simulator::Now() + interval < stop)
    {
        Simulator::Schedule(interval, &SendRttProbe, socket, dest, seq + 1, interval, stop);
    }
}

/**
 * Receive an echoed probe and record its RTT.
 *
 * @param socket UDP socket of the remote host
 */
void
ReceiveRttProbe(Ptr<Socket> socket)
{
    Ptr<Packet> p;
    while ((p = socket->Recv()))
    {
        SeqTsHeader header;
        p->RemoveHeader(header);
        const double ms = (Simulator::Now() - header.GetTs()).GetSeconds() * 1000.0;
        g_idealRttFile << Simulator::Now().GetSeconds() << " " << ms << " " << header.GetSeq()
                       << "\n";
    }
}

/**
 * Connect the sender traces of one flow. Must be called after the TCP socket
 * has been created, i.e. after the application has started.
 *
 * @param nodeId sender node id
 * @param flow flow index
 */
void
ConnectSenderTraces(uint32_t nodeId, uint32_t flow)
{
    std::string path =
        "/NodeList/" + std::to_string(nodeId) + "/$ns3::TcpL4Protocol/SocketList/0/";
    if (flow == 0)
    {
        Config::ConnectWithoutContext(path + "CongestionWindow", MakeCallback(&CwndTracer));
        if (std::getenv("TCP_DEBUG_STATE"))
        {
            Config::ConnectWithoutContext(path + "CongState", MakeCallback(&CongStateTracer));
            Config::ConnectWithoutContext(path + "RTO", MakeCallback(&RtoTracer));
            Config::ConnectWithoutContext(path + "RWND",
                                          MakeBoundCallback(&U32Tracer, std::string("rwnd")));
            Config::ConnectWithoutContext(path + "BytesInFlight",
                                          MakeBoundCallback(&U32Tracer, std::string("inflight")));
            Config::ConnectWithoutContext(path + "SlowStartThreshold",
                                          MakeBoundCallback(&U32Tracer, std::string("ssthresh")));
        }
    }
    Config::ConnectWithoutContext(path + "RTT", MakeBoundCallback(&RttTracer, flow));
    Config::ConnectWithoutContext(path + "RttSample", MakeBoundCallback(&LastRttTracer, flow));
    Config::ConnectWithoutContext(path + "Retransmission", MakeBoundCallback(&RetxTracer, flow));
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string cc = "cubic";
    uint16_t numberOfEnbs = 6;
    double enbSpacing = 1000.0; // m
    double speedKmh = 300.0;    // km/h
    bool idealRrc = false;
    bool capacityProbe = false;
    bool rttProbe = false;
    double simTime = 0.0; // 0 means auto (travel past the last eNB)
    uint32_t segmentSize = 1400;
    uint32_t numFlows = 1;
    std::string competitor = "none";
    double flowStagger = 0.0;
    double bgMbps = 0.0;
    std::string prefix = "tcp-lte-highspeed";
    bool rttSampleLog = false;
    double carLoss = 0.0;           // dB
    double tunnelStart = 0.0;       // m
    double tunnelLength = 0.0;      // m, 0 = no tunnel
    double tunnelLoss = 40.0;       // dB
    double tunnelCellSpacing = 500; // m
    double enbStart = -1.0;         // x of the first eNB, <0 = one spacing
    std::string scenarioFile;       // resolved 3-D line (line3d.h)
    MobileScenarioParams params;

    double clockGranularityMs = 1.0;
    uint32_t copaClockUs = 1;
    int32_t jitterUs = -1;
    std::string workloadFile;
    bool evalJson = false;
    double capacityMbps = 0.0;
    double baseRttMs = 0.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("clockGranularityMs", "TCP ClockGranularity in ms (default 1.0)", clockGranularityMs);
    cmd.AddValue("copaClockUs", "Copa timestamp/RTT quantization in us (1=us, 1000=1ms slow clock)", copaClockUs);
    cmd.AddValue("jitterUs", "Override Copa-Auto jitter_us (-1=auto, >=0=manual)", jitterUs);
    cmd.AddValue("workloadFile", "Optional Mortise <gap_ms> <size_bytes> .wk workload", workloadFile);
    cmd.AddValue("evalJson", "Print single-line JSON compatible with eval_report.py", evalJson);
    cmd.AddValue("capacityMbps", "Reference UDP capacity probe Mbps (for utilization)", capacityMbps);
    cmd.AddValue("baseRttMs", "Reference unloaded base RTT ms (0=auto from min ACK RTT)", baseRttMs);
    cmd.AddValue("cc",
                 "Congestion control: cubic, bbr, bbrv2(l), bbrv3(l), copa (delta 0.5) or "
                 "copa-d<delta>",
                 cc);
    cmd.AddValue("numberOfEnbs", "Number of eNodeBs along the track", numberOfEnbs);
    cmd.AddValue("enbSpacing", "Distance between eNodeBs [m]", enbSpacing);
    cmd.AddValue("speed", "Train speed [km/h]", speedKmh);
    cmd.AddValue("idealRrc",
                 "Use the idealized RRC model (no handover interruption). False is realistic",
                 idealRrc);
    cmd.AddValue("capacityProbe",
                 "Measure the LTE downlink capacity with a saturating UDP flow instead of TCP",
                 capacityProbe);
    cmd.AddValue("rttProbe",
                 "Measure the RTT of the unloaded path (no TCP): a 64-byte UDP echo every "
                 "10 ms from the server to the UE and back",
                 rttProbe);
    cmd.AddValue("simTime", "Simulation duration [s], 0 for automatic", simTime);
    cmd.AddValue("segmentSize", "TCP segment size", segmentSize);
    cmd.AddValue("numFlows", "Number of concurrent flows using --cc", numFlows);
    cmd.AddValue("competitor",
                 "Add one extra flow with this algorithm (none, cubic, bbr, ...)",
                 competitor);
    cmd.AddValue("flowStagger", "Start offset between consecutive flows [s]", flowStagger);
    cmd.AddValue("bgMbps",
                 "Mean rate of a non-responsive background flow to the same UE: UDP on/off, "
                 "exponential on and off times of mean 0.5 s, 2 * bgMbps when on [Mbit/s]",
                 bgMbps);
    cmd.AddValue("prefix", "Prefix of the output files", prefix);
    cmd.AddValue("rttSampleLog", "Write every valid ACK RTT sample to CSV", rttSampleLog);
    cmd.AddValue("scenario",
                 "Resolved 3-D railway line written by scratch/preview-line.py --export "
                 "(cells, antennas, track, speed profile, tunnels, interference); "
                 "overrides the layout, speed, car and tunnel options",
                 scenarioFile);
    cmd.AddValue("enbStart",
                 "x coordinate of the first eNB [m], <0 for one spacing (legacy)",
                 enbStart);
    cmd.AddValue("carLoss", "Train car penetration loss [dB]", carLoss);
    cmd.AddValue("tunnelStart", "x coordinate of the tunnel entrance [m]", tunnelStart);
    cmd.AddValue("tunnelLength", "Tunnel length [m], 0 for no tunnel", tunnelLength);
    cmd.AddValue("tunnelLoss", "Loss across the tunnel portal/wall [dB]", tunnelLoss);
    cmd.AddValue("tunnelCellSpacing",
                 "Spacing of the in-tunnel cells (leaky feeder sections) [m]",
                 tunnelCellSpacing);
    params.AddToCommandLine(cmd);
    cmd.Parse(argc, argv);
    params.ApplyDefaults();

    NS_ABORT_MSG_IF(numFlows < 1, "numFlows must be >= 1");

    // One entry per flow: the algorithm used by its server node.
    std::vector<std::string> flowCc;
    NS_ABORT_MSG_IF(capacityProbe && rttProbe, "capacityProbe and rttProbe are exclusive");
    if (!capacityProbe && !rttProbe)
    {
        CcTypeName(cc); // validate
        flowCc.assign(numFlows, cc);
        if (competitor != "none")
        {
            CcTypeName(competitor);
            flowCc.push_back(competitor);
        }
    }
    {
        // bbrv2l/bbrv3l use the Linux-faithful BBRv2/v3 mode, a TypeId
        // default that cannot be mixed with the original bbrv2/bbrv3 model.
        auto isLinux = [](const std::string& c) {
            return c == "bbrv2l" || c == "bbrv3l";
        };
        auto isOrig = [](const std::string& c) { return c == "bbrv2" || c == "bbrv3"; };
        const bool lin = std::any_of(flowCc.begin(), flowCc.end(), isLinux);
        NS_ABORT_MSG_IF(lin && std::any_of(flowCc.begin(), flowCc.end(), isOrig),
                        "bbrv2l/bbrv3l cannot be mixed with bbrv2/bbrv3");
        Config::SetDefault("ns3::TcpBbrV2::LinuxLossResponse", BooleanValue(lin));
    }
    const uint32_t totalFlows = rttProbe ? 0 : (capacityProbe ? 1 : flowCc.size());

    // Output file base: "<prefix>-<cc>" for TCP runs, "<prefix>" for the
    // capacity probe, "<prefix>-rttprobe" for the RTT probe.
    const std::string base =
        prefix + (capacityProbe ? "" : (rttProbe ? std::string("-rttprobe") : "-" + cc));

    Line3d l3;
    const bool use3d = !scenarioFile.empty();
    if (use3d)
    {
        l3 = Line3d::Read(scenarioFile);
        NS_ABORT_MSG_UNLESS(l3.tech == "4g",
                            "tcp-lte-highspeed-handover runs 4G lines; this line is " << l3.tech);
        carLoss = 0.0;
        tunnelLength = 0.0;
        if (simTime <= 0.0)
        {
            simTime = l3.duration;
        }
    }

    double speed = speedKmh / 3.6; // m/s
    if (simTime <= 0.0)
    {
        simTime = (numberOfEnbs + 1) * enbSpacing / speed;
    }

    // TCP defaults: large buffers, SACK, PRR recovery and the selected CC.
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(1 << 21));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(1 << 21));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(segmentSize));
    Config::SetDefault("ns3::TcpSocketBase::Sack", BooleanValue(true));
    Config::SetDefault("ns3::TcpSocketBase::ClockGranularity",
                       TimeValue(MilliSeconds(std::max(clockGranularityMs, 0.001))));
    Config::SetDefault("ns3::TcpSocketState::EnablePacing", BooleanValue(true));
    Config::SetDefault("ns3::TcpL4Protocol::RecoveryType",
                       TypeIdValue(TcpPrrRecovery::GetTypeId()));
    if (!capacityProbe)
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                           TypeIdValue(TypeId::LookupByName(CcTypeName(cc))));
    }

    // LTE setup. A crowd puts many users in one cell: the default SRS
    // periodicity (40 ms) only allows about 40 connected users per cell.
    if (use3d)
    {
        uint32_t crowd = 0;
        for (const auto& c : l3.crowds)
        {
            crowd += c.n;
        }
        if (crowd > 0)
        {
            Config::SetDefault("ns3::LteEnbRrc::SrsPeriodicity",
                               UintegerValue(crowd < 70 ? 80 : (crowd < 150 ? 160 : 320)));
        }
    }
    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    lteHelper->SetEpcHelper(epcHelper);
    lteHelper->SetAttribute("UseIdealRrc", BooleanValue(idealRrc));
    params.ConfigureLte(lteHelper, epcHelper);

    Ptr<Node> pgw = epcHelper->GetPgwNode();

    // Remote hosts behind the PGW, one per flow (one for the RTT probe).
    const uint32_t numHosts = std::max<uint32_t>(totalFlows, 1);
    NodeContainer remoteHosts;
    remoteHosts.Create(numHosts);
    InternetStackHelper internet;
    internet.Install(remoteHosts);

    PointToPointHelper p2ph;
    p2ph.SetDeviceAttribute("DataRate", DataRateValue(DataRate("100Gb/s")));
    p2ph.SetDeviceAttribute("Mtu", UintegerValue(1500));
    p2ph.SetChannelAttribute("Delay", TimeValue(MilliSeconds(params.serverDelayMs)));
    Ipv4AddressHelper ipv4h;
    Ipv4StaticRoutingHelper ipv4RoutingHelper;
    for (uint32_t i = 0; i < numHosts; i++)
    {
        Ptr<Node> host = remoteHosts.Get(i);
        NetDeviceContainer devs = p2ph.Install(pgw, host);
        std::ostringstream net;
        net << "1.0." << i << ".0";
        ipv4h.SetBase(net.str().c_str(), "255.255.255.0");
        ipv4h.Assign(devs);
        Ptr<Ipv4StaticRouting> hostRouting =
            ipv4RoutingHelper.GetStaticRouting(host->GetObject<Ipv4>());
        hostRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), 1);
        if (!capacityProbe && !rttProbe)
        {
            host->GetObject<TcpL4Protocol>()->SetAttribute(
                "SocketType",
                TypeIdValue(TypeId::LookupByName(CcTypeName(flowCc[i]))));
        }
    }

    // Track: eNodeBs along the x axis (optionally offset from the track), the
    // UE runs along it.
    // Outdoor masts every enbSpacing; inside a tunnel the masts are replaced by
    // tunnel cells (leaky-feeder sections) on the track every tunnelCellSpacing.
    auto inTunnel = [&](double x) {
        return tunnelLength > 0 && x >= tunnelStart && x <= tunnelStart + tunnelLength;
    };
    std::vector<Vector> enbPos;
    for (uint16_t i = 0; i < numberOfEnbs; i++)
    {
        const double x = (enbStart < 0 ? enbSpacing : enbStart) + enbSpacing * i;
        if (!inTunnel(x))
        {
            enbPos.emplace_back(x, params.trackOffset, params.EnbZ());
        }
    }
    if (tunnelLength > 0)
    {
        const uint32_t n = std::max<uint32_t>(1, std::lround(tunnelLength / tunnelCellSpacing));
        for (uint32_t k = 0; k < n; k++)
        {
            enbPos.emplace_back(tunnelStart + tunnelLength * (k + 0.5) / n, 5.0, 10.0);
        }
    }
    std::sort(enbPos.begin(), enbPos.end(), [](const Vector& a, const Vector& b) {
        return a.x < b.x;
    });
    if (use3d)
    {
        enbPos.clear();
        for (const auto& c : l3.cells)
        {
            enbPos.push_back(c.pos);
        }
    }
    numberOfEnbs = enbPos.size();

    NodeContainer enbNodes;
    NodeContainer ueNodes;
    enbNodes.Create(numberOfEnbs);
    ueNodes.Create(1);

    Ptr<ListPositionAllocator> enbPositionAlloc = CreateObject<ListPositionAllocator>();
    for (const auto& p : enbPos)
    {
        enbPositionAlloc->Add(p);
    }
    MobilityHelper enbMobility;
    enbMobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    enbMobility.SetPositionAllocator(enbPositionAlloc);
    enbMobility.Install(enbNodes);

    MobilityHelper ueMobility;
    if (use3d)
    {
        // The train follows the waypoints of the resolved line (speed profile
        // integrated along the 3-D track).
        ueMobility.SetMobilityModel("ns3::WaypointMobilityModel");
        ueMobility.Install(ueNodes);
        Ptr<WaypointMobilityModel> wp = ueNodes.Get(0)->GetObject<WaypointMobilityModel>();
        for (const auto& w : l3.waypoints)
        {
            wp->AddWaypoint(Waypoint(Seconds(w.first), w.second));
        }
    }
    else
    {
        ueMobility.SetMobilityModel("ns3::ConstantVelocityMobilityModel");
        ueMobility.Install(ueNodes);
        ueNodes.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 0.0, params.UeZ()));
        ueNodes.Get(0)->GetObject<ConstantVelocityMobilityModel>()->SetVelocity(
            Vector(speed, 0.0, 0.0));
    }

    NetDeviceContainer enbLteDevs;
    if (use3d)
    {
        // One eNB at a time, each with its own antenna.
        for (uint32_t i = 0; i < enbNodes.GetN(); i++)
        {
            const auto& c = l3.cells[i];
            if (c.sector)
            {
                lteHelper->SetEnbAntennaModelType("ns3::SectorAntennaModel");
                lteHelper->SetEnbAntennaModelAttribute("Azimuth", DoubleValue(c.azimuthDeg));
                lteHelper->SetEnbAntennaModelAttribute("Tilt", DoubleValue(c.tiltDeg));
                lteHelper->SetEnbAntennaModelAttribute("HorizontalBeamwidth",
                                                       DoubleValue(c.beamwidthDeg));
                lteHelper->SetEnbAntennaModelAttribute("Gain", DoubleValue(c.gainDbi));
            }
            else
            {
                lteHelper->SetEnbAntennaModelType("ns3::IsotropicAntennaModel");
            }
            enbLteDevs.Add(lteHelper->InstallEnbDevice(NodeContainer(enbNodes.Get(i))));
        }
    }
    else
    {
        enbLteDevs = lteHelper->InstallEnbDevice(enbNodes);
    }
    NetDeviceContainer ueLteDevs = lteHelper->InstallUeDevice(ueNodes);

    Ptr<Line3dLossModel> line3dLoss[2];
    std::vector<double> cellTx(enbPos.size(), params.enbTxPowerDbm);
    if (use3d)
    {
        // Transmit power: given, or for a soft range the power that puts the
        // mean RSRP (RMa line of sight, no shadowing, car loss included) at
        // the edge threshold at range_m.
        // Mean pathloss of the line's environment: urban micro / macro cells
        // for --pathloss=umi*/uma* (pedestrian routes), rural macro otherwise.
        Ptr<ThreeGppPropagationLossModel> pl;
        if (params.pathloss.rfind("umi", 0) == 0)
        {
            pl = CreateObject<ThreeGppUmiStreetCanyonPropagationLossModel>();
        }
        else if (params.pathloss.rfind("uma", 0) == 0)
        {
            pl = CreateObject<ThreeGppUmaPropagationLossModel>();
        }
        else
        {
            pl = CreateObject<ThreeGppRmaPropagationLossModel>();
        }
        pl->SetAttribute("Frequency",
                         DoubleValue(LteSpectrumValueHelper::GetCarrierFrequency(params.dlEarfcn)));
        pl->SetAttribute("ShadowingEnabled", BooleanValue(false));
        pl->SetChannelConditionModel(CreateObject<AlwaysLosChannelConditionModel>());
        const double reDb = 10.0 * std::log10(12.0 * params.bandwidthRb);
        for (uint32_t i = 0; i < enbLteDevs.GetN(); i++)
        {
            const auto& c = l3.cells[i];
            double tx = c.txDbm;
            if (!c.tunnel && c.range > 0 && !c.hard)
            {
                // The 3GPP model needs the mobility models to belong to nodes.
                static NodeContainer probes;
                if (probes.GetN() == 0)
                {
                    probes.Create(2);
                    for (uint32_t n = 0; n < 2; n++)
                    {
                        probes.Get(n)->AggregateObject(
                            CreateObject<ConstantPositionMobilityModel>());
                    }
                }
                Ptr<MobilityModel> m1 = probes.Get(0)->GetObject<MobilityModel>();
                Ptr<MobilityModel> m2 = probes.Get(1)->GetObject<MobilityModel>();
                m1->SetPosition(c.pos);
                m2->SetPosition(Vector(c.pos.x + c.range, c.pos.y, c.groundZ + 1.5));
                const double loss = -pl->CalcRxPower(0.0, m1, m2);
                tx = l3.edgeRsrp + reDb + loss + l3.carLoss - c.gainDbi;
            }
            cellTx[i] = tx;
            enbLteDevs.Get(i)->GetObject<LteEnbNetDevice>()->GetPhy()->SetTxPower(tx);
        }
        int k = 0;
        for (auto ch : {lteHelper->GetDownlinkSpectrumChannel(),
                        lteHelper->GetUplinkSpectrumChannel()})
        {
            line3dLoss[k] = CreateObject<Line3dLossModel>();
            line3dLoss[k]->SetLine(&l3);
            line3dLoss[k]->SetPhone(ueNodes.Get(0)->GetObject<MobilityModel>());
            line3dLoss[k]->SetAttribute("CarLoss", DoubleValue(l3.carLoss));
            line3dLoss[k]->SetAttribute("PortalRamp", DoubleValue(l3.tunnelRamp));
            ch->AddPropagationLossModel(line3dLoss[k]);
            k++;
        }
    }

    if (carLoss > 0 || tunnelLength > 0)
    {
        for (auto ch : {lteHelper->GetDownlinkSpectrumChannel(),
                        lteHelper->GetUplinkSpectrumChannel()})
        {
            Ptr<TrackLossModel> track = CreateObject<TrackLossModel>();
            track->SetAttribute("CarLoss", DoubleValue(carLoss));
            track->SetAttribute("TunnelStart", DoubleValue(tunnelStart));
            track->SetAttribute("TunnelLength", DoubleValue(tunnelLength));
            track->SetAttribute("TunnelLoss", DoubleValue(tunnelLoss));
            ch->AddPropagationLossModel(track);
        }
    }

    internet.Install(ueNodes);
    Ipv4InterfaceContainer ueIpIfaces =
        epcHelper->AssignUeIpv4Address(NetDeviceContainer(ueLteDevs));

    // Attach the UE to the first (3-D line: closest) eNodeB and set its default route.
    if (use3d)
    {
        lteHelper->AttachToClosestEnb(ueLteDevs.Get(0), enbLteDevs);
    }
    else
    {
        lteHelper->Attach(ueLteDevs.Get(0), enbLteDevs.Get(0));
    }
    Ptr<Ipv4StaticRouting> ueStaticRouting =
        ipv4RoutingHelper.GetStaticRouting(ueNodes.Get(0)->GetObject<Ipv4>());
    ueStaticRouting->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);

    // X2 interface between the eNodeBs, needed for X2-based handover.
    lteHelper->AddX2Interface(enbNodes);

    // Dense crowds (3-D line): other users around the phone, attached to the
    // closest cell, each fed by a constant-rate UDP download while the crowd
    // is there. They share the cell with the phone (PF scheduler); their ports
    // are outside the flow range, so the metrics only count the phone.
    uint32_t crowdUsers = 0;
    if (use3d && !l3.crowds.empty())
    {
        NodeContainer crowdNodes;
        for (const auto& c : l3.crowds)
        {
            crowdNodes.Create(c.n);
        }
        crowdUsers = crowdNodes.GetN();
        Ptr<ListPositionAllocator> crowdPos = CreateObject<ListPositionAllocator>();
        for (const auto& c : l3.crowds)
        {
            // Within a few metres of the phone, on two rings.
            for (uint32_t j = 0; j < c.n; j++)
            {
                const double r = 2.0 + 2.0 * (j % 2);
                const double a = 2.0 * M_PI * j / std::max<uint32_t>(c.n, 1);
                crowdPos->Add(Vector(c.pos.x + r * std::cos(a), c.pos.y + r * std::sin(a),
                                     c.pos.z));
            }
        }
        MobilityHelper crowdMobility;
        crowdMobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
        crowdMobility.SetPositionAllocator(crowdPos);
        crowdMobility.Install(crowdNodes);
        NetDeviceContainer crowdDevs = lteHelper->InstallUeDevice(crowdNodes);
        internet.Install(crowdNodes);
        Ipv4InterfaceContainer crowdIfaces = epcHelper->AssignUeIpv4Address(crowdDevs);
        for (uint32_t j = 0; j < crowdNodes.GetN(); j++)
        {
            lteHelper->AttachToClosestEnb(crowdDevs.Get(j), enbLteDevs);
            ipv4RoutingHelper.GetStaticRouting(crowdNodes.Get(j)->GetObject<Ipv4>())
                ->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);
        }

        NodeContainer crowdHost;
        crowdHost.Create(1);
        internet.Install(crowdHost);
        NetDeviceContainer devs = p2ph.Install(pgw, crowdHost.Get(0));
        ipv4h.SetBase("1.1.0.0", "255.255.255.0");
        ipv4h.Assign(devs);
        ipv4RoutingHelper.GetStaticRouting(crowdHost.Get(0)->GetObject<Ipv4>())
            ->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), 1);

        uint32_t j = 0;
        for (const auto& c : l3.crowds)
        {
            for (uint32_t m = 0; m < c.n; m++, j++)
            {
                const uint16_t port = 20000 + j;
                PacketSinkHelper sink("ns3::UdpSocketFactory",
                                      InetSocketAddress(Ipv4Address::GetAny(), port));
                sink.Install(crowdNodes.Get(j));
                OnOffHelper onoff("ns3::UdpSocketFactory",
                                  InetSocketAddress(crowdIfaces.GetAddress(j), port));
                onoff.SetConstantRate(DataRate(c.mbps * 1e6), 1400);
                ApplicationContainer app = onoff.Install(crowdHost.Get(0));
                app.Start(Seconds(std::max(c.t0, 0.5)));
                app.Stop(Seconds(c.t1));
            }
        }
    }

    if (bgMbps > 0 && !capacityProbe && !rttProbe)
    {
        // Non-responsive background traffic to the same UE (same bearer and
        // eNB queue as the TCP flows): bursts of a constant bit rate.
        const uint16_t bgPort = 9100;
        PacketSinkHelper bgSink("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), bgPort));
        bgSink.Install(ueNodes.Get(0));
        OnOffHelper bg("ns3::UdpSocketFactory",
                       InetSocketAddress(ueIpIfaces.GetAddress(0), bgPort));
        bg.SetAttribute("OnTime", StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));
        bg.SetAttribute("OffTime", StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));
        bg.SetAttribute("DataRate", DataRateValue(DataRate(2 * bgMbps * 1e6)));
        bg.SetAttribute("PacketSize", UintegerValue(1400));
        ApplicationContainer bgApp = bg.Install(remoteHosts.Get(0));
        bgApp.Start(Seconds(1.0));
        bgApp.Stop(Seconds(simTime));
    }

    // Downlink transfers: remote host i -> UE port dlPort + i.
    const uint16_t dlPort = 1234;
    std::vector<double> wkArr;
    std::vector<uint32_t> wkSizes;
    Ptr<MortiseAppReceiver> appRx;
    const bool appMode = !workloadFile.empty() && !capacityProbe && !rttProbe;
    if (appMode)
    {
        NS_ABORT_MSG_UNLESS(LoadMortiseWorkload(workloadFile, wkArr, wkSizes, 1.0),
                            "Failed to load --workloadFile " << workloadFile);
        simTime = std::max(simTime, wkArr.back() + 20.0);
    }
    for (uint32_t i = 0; i < totalFlows; i++)
    {
        const double start = 1.0 + i * flowStagger;
        const uint16_t port = dlPort + i;
        g_lastRxBytes.push_back(0);
        g_rtt.emplace_back();
        g_srtt.emplace_back();
        g_retx.push_back(0);
        if (appMode && i == 0)
        {
            appRx = CreateObject<MortiseAppReceiver>();
            appRx->Setup(port, wkArr, wkSizes);
            ueNodes.Get(0)->AddApplication(appRx);
            appRx->SetStartTime(Seconds(0.5));
            appRx->SetStopTime(Seconds(simTime));
            g_sinks.push_back(nullptr);

            Ptr<MortiseAppSender> appTx = CreateObject<MortiseAppSender>();
            appTx->Setup(InetSocketAddress(ueIpIfaces.GetAddress(0), port), wkArr, wkSizes);
            remoteHosts.Get(i)->AddApplication(appTx);
            appTx->SetStartTime(Seconds(start));
            appTx->SetStopTime(Seconds(simTime));

            Simulator::Schedule(Seconds(start + 0.1),
                                &ConnectSenderTraces,
                                remoteHosts.Get(i)->GetId(),
                                i);
            Simulator::Schedule(Seconds(start) + MicroSeconds(1),
                                &ConfigureCcFlow,
                                remoteHosts.Get(i)->GetId(),
                                i,
                                flowCc[i],
                                jitterUs,
                                copaClockUs);
            continue;
        }
        std::string factory = capacityProbe ? "ns3::UdpSocketFactory" : "ns3::TcpSocketFactory";
        PacketSinkHelper sinkHelper(factory, InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sinkApp = sinkHelper.Install(ueNodes.Get(0));
        g_sinks.push_back(sinkApp.Get(0)->GetObject<PacketSink>());
        sinkApp.Start(Seconds(1.0));
        sinkApp.Stop(Seconds(simTime));

        ApplicationContainer sourceApp;
        if (capacityProbe)
        {
            // Saturating UDP flow: the delivered rate is the radio link capacity.
            OnOffHelper onoff(factory, InetSocketAddress(ueIpIfaces.GetAddress(0), port));
            onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
            onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
            onoff.SetAttribute("DataRate",
                               DataRateValue(DataRate(params.bandwidthRb * 1.2e6 + 5e6)));
            onoff.SetAttribute("PacketSize", UintegerValue(1400));
            sourceApp = onoff.Install(remoteHosts.Get(i));
        }
        else
        {
            BulkSendHelper bulk(factory, InetSocketAddress(ueIpIfaces.GetAddress(0), port));
            bulk.SetAttribute("MaxBytes", UintegerValue(0)); // unlimited
            sourceApp = bulk.Install(remoteHosts.Get(i));
            // The TCP socket only exists once the application has started.
            Simulator::Schedule(Seconds(start + 0.1),
                                &ConnectSenderTraces,
                                remoteHosts.Get(i)->GetId(),
                                i);
            Simulator::Schedule(Seconds(start) + MicroSeconds(1),
                                &ConfigureCcFlow,
                                remoteHosts.Get(i)->GetId(),
                                i,
                                flowCc[i],
                                jitterUs,
                                copaClockUs);
        }
        sourceApp.Start(Seconds(start));
        sourceApp.Stop(Seconds(simTime));
    }

    // Unloaded RTT: timestamped UDP echoes between the first remote host and
    // the UE, a reference for the RTT of the TCP flows (same path, empty
    // queue).
    if (rttProbe)
    {
        const uint16_t echoPort = 9;
        UdpEchoServerHelper echo(echoPort);
        ApplicationContainer echoApp = echo.Install(ueNodes.Get(0));
        echoApp.Start(Seconds(0.5));
        echoApp.Stop(Seconds(simTime));
        Ptr<Socket> probe =
            Socket::CreateSocket(remoteHosts.Get(0), UdpSocketFactory::GetTypeId());
        probe->Bind();
        probe->SetRecvCallback(MakeCallback(&ReceiveRttProbe));
        Simulator::Schedule(Seconds(1.0),
                            &SendRttProbe,
                            probe,
                            InetSocketAddress(ueIpIfaces.GetAddress(0), echoPort),
                            0,
                            MilliSeconds(10),
                            Seconds(simTime));
    }

    // 3-D line: every 100 ms update the car loss and the interference seen by
    // the phone, and log the trajectory.
    std::ofstream trajFile;
    if (use3d)
    {
        if (!evalJson)
        {
            trajFile.open(base + "-trajectory.data");
            trajFile << "# t x y z speed_kmh interference_dbm car_loss_db tunnel cell sinr_db\n";
        }
        Ptr<LteUePhy> uePhy = ueLteDevs.Get(0)->GetObject<LteUeNetDevice>()->GetPhy();
        Ptr<MobilityModel> ueMob = ueNodes.Get(0)->GetObject<MobilityModel>();
        auto poll = std::make_shared<std::function<void(double, double)>>();
        *poll = [&, uePhy, ueMob, poll](double lastIntf, double lastCar) {
            const double t = Simulator::Now().GetSeconds();
            const Vector p = ueMob->GetPosition();
            const double intf = l3.InterferenceAt(t, p);
            if (intf != lastIntf)
            {
                uePhy->SetAttribute("InterferencePower", DoubleValue(intf));
            }
            const double car = l3.CarLossAt(t);
            if (car != lastCar)
            {
                for (auto& m : line3dLoss)
                {
                    m->SetAttribute("CarLoss", DoubleValue(car));
                }
            }
            if (!evalJson)
            {
                const auto pr = l3.Project(p);
                bool tun = false;
                for (const auto& tr : l3.tunnels)
                {
                    tun = tun || (pr.first >= tr.first && pr.first <= tr.second);
                }
                const Vector v = ueMob->GetVelocity();
                trajFile << t << " " << p.x << " " << p.y << " " << p.z << " "
                         << 3.6 * std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) << " "
                         << (intf > -300 ? intf : 0.0) << " " << car << " " << tun << " "
                         << g_lastCellId << " " << g_lastSinr << "\n";
            }
            Simulator::Schedule(MilliSeconds(100), [poll, intf, car]() { (*poll)(intf, car); });
        };
        Simulator::ScheduleNow([poll]() { (*poll)(1.0, -1.0); });

        if (!evalJson)
        {
            std::ofstream lf(base + "-line.data");
            lf << "# scenario " << scenarioFile << "\n";
            for (uint32_t i = 0; i < l3.cells.size(); i++)
            {
                const auto& c = l3.cells[i];
                lf << (c.tunnel ? "tcell " : "cell ") << c.id << " " << c.pos.x << " " << c.pos.y
                   << " " << c.pos.z << " tx " << cellTx[i] << " range " << c.range << " hard "
                   << c.hard << " cellId " << i + 1 << "\n";
            }
        }
    }

    // Traces.
    if (!evalJson)
    {
        g_throughputFile.open(base + "-throughput.data", std::ios::out);
        g_rsrpFile.open(base + "-rsrp.data", std::ios::out);
        g_handoverFile.open(base + "-handover.data", std::ios::out);
        if (rttProbe)
        {
            g_idealRttFile.open(base + "-idealrtt.data", std::ios::out);
            g_idealRttFile << "# t rtt_ms seq (64-byte UDP echo every 10 ms, no TCP load)\n";
        }
        if (!capacityProbe && !rttProbe)
        {
            g_cwndFile.open(base + "-cwnd.data", std::ios::out);
            if (cc == "copa-auto")
            {
                g_copaEstFile.open(base + "-copaest.data", std::ios::out);
                g_copaEstFile << "# t j_ms k_ms rho r0_ms dq_target_ms delta\n";
            }
            g_rttFile.open(base + "-rtt.data", std::ios::out);
            if (rttSampleLog)
            {
                g_rawRttLog.open(base + "-rtt-samples.csv");
                g_rawRttLog << "time_s,flow,rtt_ms\n";
            }
            g_rttSampleFile.open(base + "-rttsample.data", std::ios::out);
            g_rttSampleFile << "# t flow mean_rtt_ms (RTT samples of the last 100 ms)\n";
            g_binRttSum.assign(totalFlows, 0.0);
            g_binRttCount.assign(totalFlows, 0);
            Simulator::Schedule(MilliSeconds(100), &FlushRttBins);
            if (std::getenv("TCP_DEBUG_STATE"))
            {
                g_stateFile.open(base + "-state.data", std::ios::out);
            }
        }
    }

    // Only the phone under test (not the users of a crowd).
    const std::string phonePath = "/NodeList/" + std::to_string(ueNodes.Get(0)->GetId());
    Config::Connect(phonePath + "/DeviceList/*/LteUeRrc/HandoverStart",
                    MakeCallback(&HandoverStart));
    Config::Connect(phonePath + "/DeviceList/*/LteUeRrc/HandoverEndOk",
                    MakeCallback(&HandoverEndOk));
    Config::Connect(phonePath + "/DeviceList/*/LteUeRrc/RadioLinkFailure",
                    MakeCallback(&RadioLinkFailure));
    Config::Connect(phonePath + "/DeviceList/*/ComponentCarrierMapUe/*/LteUePhy/"
                    "ReportCurrentCellRsrpSinr",
                    MakeCallback(&RsrpSinrTrace));

    g_lastSampleTime = Seconds(1.0);
    Simulator::Schedule(Seconds(1.0), &SampleThroughput);

    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> monitor = flowHelper.InstallAll();

    std::cout << "Mobile LTE scenario: " << cc << " x" << numFlows
              << (competitor != "none" ? " + " + competitor : "") << ", " << numberOfEnbs
              << " eNBs, " << enbSpacing << " m spacing, " << speedKmh
              << " km/h, carLoss=" << carLoss << " dB, tunnel=" << tunnelLength << " m @" << tunnelStart
              << ", idealRrc=" << idealRrc << ", duration=" << simTime << " s" << std::endl;
    std::cout << "  " << params.Describe() << std::endl;

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowHelper.GetClassifier());
    FlowMonitor::FlowStatsContainer stats = monitor->GetFlowStats();

    struct FlowResult
    {
        double throughput{0};
        double meanDelayMs{0};
        uint64_t lost{0};
        uint64_t rxPackets{0};
    };

    std::vector<FlowResult> results(totalFlows);
    for (auto& it : stats)
    {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(it.first);
        if (t.destinationPort < dlPort || t.destinationPort >= dlPort + totalFlows)
        {
            continue;
        }
        FlowResult& r = results[t.destinationPort - dlPort];
        if (!it.second.timeLastRxPacket.IsZero() && !it.second.timeFirstTxPacket.IsZero())
        {
            double span = (it.second.timeLastRxPacket - it.second.timeFirstTxPacket).GetSeconds();
            r.throughput = span > 0 ? it.second.rxBytes * 8.0 / span / 1e6 : 0.0;
        }
        r.meanDelayMs = it.second.rxPackets > 0
                            ? it.second.delaySum.GetSeconds() / it.second.rxPackets * 1000.0
                            : 0.0;
        r.lost = it.second.lostPackets;
        r.rxPackets = it.second.rxPackets;
    }

    if (evalJson)
    {
        double thrMbps = 0.0;
        uint64_t lostPkts = 0, rxPkts = 0, retxPkts = 0;
        for (uint32_t i = 0; i < totalFlows; i++)
        {
            thrMbps += results[i].throughput;
            lostPkts += results[i].lost;
            rxPkts += results[i].rxPackets;
            retxPkts += g_retx[i];
        }
        std::vector<double> rtt;
        for (const auto& s : g_rtt)
        {
            rtt.insert(rtt.end(), s.begin(), s.end());
        }
        double effCap = (capacityMbps > 1e-3) ? capacityMbps : thrMbps;
        double effBaseRtt = (baseRttMs > 1e-3) ? baseRttMs : (2.0 * params.serverDelayMs + 15.0);
        double rttAvg = effBaseRtt;
        if (!rtt.empty())
        {
            double minR = *std::min_element(rtt.begin(), rtt.end());
            double s = 0.0;
            for (double x : rtt) s += x;
            rttAvg = s / rtt.size();
            if (baseRttMs <= 1e-3)
            {
                effBaseRtt = minR;
            }
        }
        double qdAvg = std::max(rttAvg - effBaseRtt, 0.0);
        double qdP95 = std::max(Percentile(rtt, 95) - effBaseRtt, 0.0);
        double dropRate = (rxPkts + lostPkts > 0) ? (double)lostPkts / (rxPkts + lostPkts) : 0.0;
        double util = (effCap > 1e-6) ? std::min(thrMbps / effCap, 1.5) : 0.0;
        double estRtt = effBaseRtt + qdAvg;
        double utilPower = (estRtt > 1e-6) ? (util / (estRtt / 1000.0)) : 0.0;
        double normPower = utilPower * (effBaseRtt / 1000.0);
        double tputPower = (estRtt > 1e-6) ? (thrMbps / (estRtt / 1000.0)) : 0.0;

        std::ostringstream js;
        js << std::fixed << std::setprecision(6);
        js << "{\"duration_s\":" << std::max(simTime - 1.0, 1.0)
           << ",\"capacity_mbps\":" << effCap
           << ",\"throughput_mbps\":" << thrMbps
           << ",\"utilization\":" << util
           << ",\"qdelay_avg_ms\":" << qdAvg
           << ",\"qdelay_p95_ms\":" << qdP95
           << ",\"base_rtt_ms\":" << effBaseRtt
           << ",\"est_rtt_ms\":" << estRtt
           << ",\"util_power\":" << utilPower
           << ",\"norm_power\":" << normPower
           << ",\"tput_power\":" << tputPower
           << ",\"drop_rate\":" << dropRate
           << ",\"retrans_pkts\":" << retxPkts
           << ",\"retrans_ratio\":" << ((rxPkts + retxPkts > 0) ? (double)retxPkts / (rxPkts + retxPkts) : 0.0)
           << ",\"handovers\":" << g_handoverCount;
        if (appMode && appRx)
        {
            std::vector<double> rcts = appRx->rctsMs;
            double meanRct = 0.0, p50Rct = 0.0, p95Rct = 0.0, fctSlowdown = 0.0, mortiseQoe = 0.0;
            if (!rcts.empty())
            {
                double qoeSum = 0.0, logSdSum = 0.0;
                for (size_t i = 0; i < rcts.size(); i++)
                {
                    meanRct += rcts[i];
                    uint32_t sz = (i < wkSizes.size()) ? wkSizes[i] : 1400U;
                    double rctS = std::max(rcts[i] / 1000.0, 1e-6);
                    double rThr = (static_cast<double>(sz) * 8.0) / (rctS * 1e6);
                    qoeSum += (rThr * 0.5) / std::max(rcts[i], 1.0) * 100.0;
                    double idealMs = std::max(effBaseRtt, 1.0) +
                                     ((effCap > 1e-6) ? (static_cast<double>(sz) * 8.0 / (effCap * 1000.0)) : 0.0);
                    logSdSum += std::log(std::max(rcts[i] / std::max(idealMs, 1e-3), 1.0));
                }
                meanRct /= rcts.size();
                mortiseQoe = qoeSum / rcts.size();
                fctSlowdown = std::exp(logSdSum / rcts.size());
                std::vector<double> sr = rcts;
                p50Rct = Percentile(sr, 50);
                p95Rct = Percentile(sr, 95);
            }
            js << ",\"requests\":" << rcts.size()
               << ",\"rct_mean_ms\":" << meanRct
               << ",\"rct_p50_ms\":" << p50Rct
               << ",\"rct_p95_ms\":" << p95Rct
               << ",\"fct_slowdown\":" << fctSlowdown
               << ",\"mortise_qoe\":" << mortiseQoe;
        }
        js << "}\n";
        std::cout << js.str() << std::flush;
        Simulator::Destroy();
        return 0;
    }

    std::cout << "\n=== " << (capacityProbe ? "capacity probe" : (rttProbe ? "rtt probe" : cc))
              << " / mobile LTE ==="
              << std::endl;
    std::cout << std::left << std::setw(6) << "flow" << std::setw(8) << "cc" << std::setw(12)
              << "Mbps" << std::setw(12) << "delay[ms]" << std::setw(10) << "lost"
              << std::setw(10) << "retx" << std::setw(10) << "rttP95" << std::endl;

    double sum = 0.0;
    double sumSq = 0.0;
    std::ofstream json(base + "-metrics.json");
    json << std::fixed << std::setprecision(4);
    json << "{\n  \"rtt_stats_version\": 2, \"rtt_source\": \"ack_rtt_samples\",\n"
         << "  \"cc\": \"" << cc << "\", \"competitor\": \"" << competitor
         << "\", \"numFlows\": " << numFlows << ", \"speed\": " << speedKmh
         << ", \"enbSpacing\": " << enbSpacing << ", \"cells\": " << numberOfEnbs
         << ", \"carLoss\": " << carLoss << ", \"tunnelStart\": " << tunnelStart
         << ", \"tunnelLength\": " << tunnelLength
         << ", \"simTime\": " << simTime << ", \"crowdUsers\": " << crowdUsers
         << ", \"capacityProbe\": " << (capacityProbe ? "true" : "false")
         << ", \"rttProbe\": " << (rttProbe ? "true" : "false")
         << ",\n  \"params\": \"" << params.Describe() << "\""
         << ",\n  \"handovers\": " << g_handoverCount << ", \"rlf\": " << g_rlfCount << ",\n  \"flows\": [\n";
    for (uint32_t i = 0; i < totalFlows; i++)
    {
        const FlowResult& r = results[i];
        std::vector<double> rtt = g_rtt[i];
        double rttMean = 0.0;
        for (double x : rtt)
        {
            rttMean += x;
        }
        rttMean = rtt.empty() ? 0.0 : rttMean / rtt.size();
        double srttMean = 0.0;
        for (double x : g_srtt[i])
        {
            srttMean += x;
        }
        srttMean = g_srtt[i].empty() ? 0.0 : srttMean / g_srtt[i].size();
        double p50 = Percentile(rtt, 50);
        double p95 = Percentile(rtt, 95);
        double p99 = Percentile(rtt, 99);
        const std::string name = capacityProbe ? "udp" : flowCc[i];
        sum += r.throughput;
        sumSq += r.throughput * r.throughput;
        std::cout << std::left << std::setw(6) << i << std::setw(8) << name << std::setw(12)
                  << std::fixed << std::setprecision(3) << r.throughput << std::setw(12)
                  << r.meanDelayMs << std::setw(10) << r.lost << std::setw(10) << g_retx[i]
                  << std::setw(10) << p95 << std::endl;
        json << "    {\"flow\": " << i << ", \"cc\": \"" << name
             << "\", \"throughput_mbps\": " << r.throughput
             << ", \"mean_delay_ms\": " << r.meanDelayMs << ", \"lost\": " << r.lost
             << ", \"rx_packets\": " << r.rxPackets << ", \"retx\": " << g_retx[i]
             << ", \"srtt_mean_ms\": " << srttMean << ", \"rtt_samples\": " << rtt.size()
             << ", \"rtt_mean_ms\": " << rttMean << ", \"rtt_p50_ms\": " << p50
             << ", \"rtt_p95_ms\": " << p95 << ", \"rtt_p99_ms\": " << p99 << "}"
             << (i + 1 < totalFlows ? "," : "") << "\n";
    }
    double jain = sumSq > 0 ? sum * sum / (totalFlows * sumSq) : 0.0;
    std::vector<double> pooled;
    for (const auto& samples : g_rtt)
    {
        pooled.insert(pooled.end(), samples.begin(), samples.end());
    }
    json << "  ],\n  \"total_throughput_mbps\": " << sum << ", \"jain\": " << jain
         << ", \"rtt_p95_ms\": " << Percentile(pooled, 95)
         << ", \"rtt_p99_ms\": " << Percentile(pooled, 99)
         << ", \"rtt_samples\": " << pooled.size() << "\n}\n";
    json.close();

    std::cout << "total " << sum << " Mbps, Jain fairness " << jain << std::endl;
    std::cout << "successful handovers: " << g_handoverCount << ", radio link failures: "
              << g_rlfCount << std::endl;

    monitor->SerializeToXmlFile(base + "-flowmonitor.xml", true, false);

    Simulator::Destroy();
    return 0;
}
