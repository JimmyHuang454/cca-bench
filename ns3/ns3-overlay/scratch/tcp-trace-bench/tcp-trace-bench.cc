/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TCP benchmark setup, simulation and result collection. The lines parser,
 * link gates, workload applications and congestion-control setup live in
 * the adjacent files.
 */

#include "cc-config.h"
#include "line-model.h"
#include "link-gates.h"
#include "workload.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TcpCapacityTrace");

namespace tracebench
{

/** One piece of the capacity schedule. */
struct Segment
{
    double start;      ///< Start time [s]
    double rateMbps;   ///< Bottleneck capacity [Mbit/s]
    double loss;       ///< Random data loss probability, 1 = outage
    std::string event; ///< Event name written when the segment starts
    double delayMs{-1.0}; ///< Bottleneck one-way delay [ms], < 0 = unchanged
};

Ptr<PointToPointNetDevice> g_forwardDevice; ///< Router side of the bottleneck
Ptr<PointToPointNetDevice> g_reverseDevice; ///< Receiver side of the bottleneck
Ptr<RateErrorModel> g_dataLoss;             ///< Loss on data packets
Ptr<RateErrorModel> g_ackLoss;              ///< Loss on ACKs (outages only)
Ptr<HoldGate> g_dataGate;                   ///< Outage gate of the data direction
Ptr<HoldGate> g_ackGate;                    ///< Outage gate of the ACK direction
Ptr<MahimahiGate> g_mmDataGate;             ///< Trace gate of the data direction
Ptr<MahimahiGate> g_mmAckGate;              ///< Trace gate of the ACK direction
bool g_holdMode = false;                    ///< Buffer instead of drop during outages
double g_capacityMbps = 0.0;                ///< Current capacity
bool g_outage = false;                      ///< Current outage flag
Ptr<PointToPointChannel> g_bnChannel;       ///< Bottleneck channel
double g_accessDelayMs = 5.0;               ///< Access link one-way delay
double g_delayMs = 20.0;                    ///< Current bottleneck one-way delay
double g_delayTargetMs = 20.0;              ///< Bottleneck delay being slewed to
double g_delaySlewMs = 0.02;                ///< Largest delay decrease per millisecond
bool g_slewing = false;                     ///< A delay decrease is in progress

std::vector<Ptr<PacketSink>> g_sinks;           ///< One sink per flow
std::vector<uint64_t> g_lastRx;                 ///< Bytes at the last bin, per flow
std::vector<std::vector<double>> g_rttSamples;  ///< All RTT samples [ms], per flow
std::vector<std::vector<double>> g_baseRttSamples; ///< Path base RTT at each RTT sample
std::vector<std::vector<double>> g_srttSamples; ///< Smoothed RTT updates for diagnostics
std::ofstream g_rawRttLog; ///< Optional log of every valid ACK RTT sample
std::vector<double> g_binRttSum;                ///< RTT sum in the current bin, per flow
std::vector<uint32_t> g_binRttCount;            ///< RTT samples in the current bin
std::vector<double> g_binLastRttSum;            ///< Sum of RTT samples (last RTT) in the bin
std::vector<uint32_t> g_binLastRttCount;        ///< Number of RTT samples (last RTT) in the bin
std::vector<uint32_t> g_retx;                   ///< Retransmitted segments, per flow
uint32_t g_cwnd0 = 0;                           ///< Latest cwnd of flow 0
double g_binMs = 50.0;                          ///< Time-series bin
double g_warmup = 0.0;                          ///< Samples before this are not counted
double g_capacityIntegral = 0.0;                ///< Integral of capacity over the measured period [Mbit]
std::vector<uint64_t> g_rxAtWarmup;             ///< Bytes received at the end of warmup

std::ofstream g_series; ///< Time series output
std::ofstream g_events; ///< Event output
std::ofstream g_copaEst; ///< Estimates of the adaptive Copa delta (copa-auto, flow 0)

/**
 * Nearest-rank percentile.
 *
 * @param v samples (sorted in place)
 * @param p percentile in [0, 100]
 * @return percentile, 0 when empty
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
 * Set the bottleneck one-way delay.
 *
 * @param ms delay [ms]
 */
void
SetBottleneckDelay(double ms)
{
    g_delayMs = ms;
    g_bnChannel->SetAttribute("Delay", TimeValue(MicroSeconds(std::llround(ms * 1000.0))));
}

/**
 * Move the bottleneck delay towards its target by at most --delaySlew per
 * millisecond. A sudden decrease would let later packets overtake earlier
 * ones and trigger spurious fast retransmits; as long as the step is smaller
 * than the packet spacing, the order is kept.
 */
void
SlewDelay()
{
    if (g_delayMs - g_delayTargetMs <= g_delaySlewMs)
    {
        SetBottleneckDelay(g_delayTargetMs);
        g_slewing = false;
        return;
    }
    SetBottleneckDelay(g_delayMs - g_delaySlewMs);
    Simulator::Schedule(MilliSeconds(1), &SlewDelay);
}

/**
 * Apply one capacity segment to the bottleneck.
 *
 * @param seg segment
 */
void
ApplySegment(Segment seg)
{
    const bool wasOutage = g_outage;
    if (seg.delayMs >= 0)
    {
        g_delayTargetMs = seg.delayMs;
        if (seg.delayMs >= g_delayMs)
        {
            SetBottleneckDelay(seg.delayMs);
        }
        else if (!g_slewing)
        {
            g_slewing = true;
            SlewDelay();
        }
    }
    g_outage = seg.loss >= 1.0;
    g_capacityMbps = g_outage ? 0.0 : seg.rateMbps;
    if (!g_outage)
    {
        DataRate rate(static_cast<uint64_t>(seg.rateMbps * 1e6));
        g_forwardDevice->SetAttribute("DataRate", DataRateValue(rate));
        g_reverseDevice->SetAttribute("DataRate", DataRateValue(rate));
        g_dataLoss->SetRate(std::min(seg.loss, 1.0));
        if (wasOutage)
        {
            g_dataGate->EndOutage(rate);
            g_ackGate->EndOutage(rate);
        }
    }
    else if (!wasOutage)
    {
        g_dataGate->StartOutage(g_holdMode);
        g_ackGate->StartOutage(g_holdMode);
    }
    if (!seg.event.empty())
    {
        g_events << std::fixed << std::setprecision(4) << Simulator::Now().GetSeconds() << ","
                 << seg.event << "," << seg.rateMbps << std::endl;
    }
}

/** Write one time-series row and reschedule. */
void
SampleBin()
{
    const double now = Simulator::Now().GetSeconds();
    const double dt = g_binMs / 1000.0;
    if (g_mmDataGate)
    {
        g_capacityMbps = g_mmDataGate->TakeOpportunities() * 12000.0 / dt / 1e6;
    }
    if (now > g_warmup + 1e-9)
    {
        g_capacityIntegral += g_capacityMbps * dt;
    }
    g_series << std::fixed << std::setprecision(3) << now << "," << g_capacityMbps << ","
             << (g_outage ? 1 : 0);
    for (size_t i = 0; i < g_sinks.size(); i++)
    {
        uint64_t rx = g_sinks[i] ? g_sinks[i]->GetTotalRx() : 0;
        g_series << "," << (rx - g_lastRx[i]) * 8.0 / dt / 1e6;
        g_lastRx[i] = rx;
    }
    for (size_t i = 0; i < g_sinks.size(); i++)
    {
        double rtt = g_binRttCount[i] ? g_binRttSum[i] / g_binRttCount[i] : 0.0;
        g_series << "," << rtt;
        g_binRttSum[i] = 0.0;
        g_binRttCount[i] = 0;
    }
    g_series << "," << g_cwnd0;
    for (size_t i = 0; i < g_sinks.size(); i++)
    {
        g_series << ","
                 << (g_binLastRttCount[i] ? g_binLastRttSum[i] / g_binLastRttCount[i] : 0.0);
        g_binLastRttSum[i] = 0.0;
        g_binLastRttCount[i] = 0;
    }
    g_series << "," << 2 * (g_delayMs + g_accessDelayMs) << "\n";
    Simulator::Schedule(MilliSeconds(g_binMs), &SampleBin);
}

/** Snapshot the received bytes at the end of the warmup period. */
void
EndWarmup()
{
    for (size_t i = 0; i < g_sinks.size(); i++)
    {
        g_rxAtWarmup[i] = g_sinks[i] ? g_sinks[i]->GetTotalRx() : 0;
    }
}

/**
 * Accumulate an RTT sample (RTT of the last acknowledged segment) of one flow.
 *
 * @param flow flow index
 * @param oldVal previous value
 * @param newVal new RTT sample
 */
void
LastRttTrace(uint32_t flow, Time newVal)
{
    if (newVal.IsStrictlyPositive())
    {
        g_binLastRttSum[flow] += newVal.GetSeconds() * 1000.0;
        g_binLastRttCount[flow]++;
        if (Simulator::Now().GetSeconds() > g_warmup)
        {
            g_rttSamples[flow].push_back(newVal.GetSeconds() * 1000.0);
            g_baseRttSamples[flow].push_back(g_upDelay.empty() ? 2.0 * (g_delayMs + g_accessDelayMs) : BaseRttNow());
            if (g_rawRttLog.is_open())
            {
                g_rawRttLog << std::setprecision(12) << Simulator::Now().GetSeconds() << ","
                            << flow << "," << newVal.GetSeconds() * 1000.0 << "\n";
            }
        }
    }
}

/**
 * Record an RTT sample of one flow.
 *
 * @param flow flow index
 * @param oldVal previous smoothed RTT
 * @param newVal new smoothed RTT
 */
void
RttTrace(uint32_t flow, Time oldVal [[maybe_unused]], Time newVal)
{
    double ms = newVal.GetSeconds() * 1000.0;
    if (ms <= 0)
    {
        return;
    }
    g_binRttSum[flow] += ms;
    g_binRttCount[flow]++;
    if (Simulator::Now().GetSeconds() > g_warmup)
    {
        g_srttSamples[flow].push_back(ms);
    }
}

/**
 * Record the cwnd of flow 0.
 *
 * @param oldVal previous cwnd
 * @param newVal new cwnd
 */
void
CwndTrace(uint32_t oldVal [[maybe_unused]], uint32_t newVal)
{
    g_cwnd0 = newVal;
}

/**
 * Count a retransmitted segment.
 *
 * @param flow flow index
 * @param packet packet
 * @param header TCP header
 * @param local local address
 * @param peer peer address
 * @param socket socket
 */
void
RetxTrace(uint32_t flow,
          Ptr<const Packet> packet [[maybe_unused]],
          const TcpHeader& header [[maybe_unused]],
          const Address& local [[maybe_unused]],
          const Address& peer [[maybe_unused]],
          Ptr<const TcpSocketBase> socket [[maybe_unused]])
{
    if (Simulator::Now().GetSeconds() > g_warmup)
    {
        g_retx[flow]++;
    }
}

/**
 * Connect the traces of one sender once its socket exists.
 *
 * @param nodeId sender node id
 * @param flow flow index
 */
void
ConnectTraces(uint32_t nodeId, uint32_t flow)
{
    std::string path =
        "/NodeList/" + std::to_string(nodeId) + "/$ns3::TcpL4Protocol/SocketList/0/";
    Config::ConnectWithoutContext(path + "RTT", MakeBoundCallback(&RttTrace, flow));
    Config::ConnectWithoutContext(path + "RttSample", MakeBoundCallback(&LastRttTrace, flow));
    Config::ConnectWithoutContext(path + "Retransmission", MakeBoundCallback(&RetxTrace, flow));
    if (flow == 0)
    {
        Config::ConnectWithoutContext(path + "CongestionWindow", MakeCallback(&CwndTrace));
    }
}

/**
 * Split a comma separated list.
 *
 * @param s input
 * @return items
 */
std::vector<std::string>
Split(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        if (!item.empty())
        {
            out.push_back(item);
        }
    }
    return out;
}

} // namespace tracebench

using namespace tracebench;

int
main(int argc, char* argv[])
{
    std::string ccs = "bbrv2";
    std::string profile = "lines";
    std::string traceFile;
    std::string lineFile;
    std::string downTrace;
    std::string upTrace;
    std::string delayTrace;
    double defaultDelayMs = 10.0;
    double traceOffset = 0.0;
    std::string prefix = "tcp-capacity";
    bool rttSampleLog = false;
    double stopTime = 60.0;
    double capacity = 20.0; // Mbit/s
    double capMin = 5.0;
    double capMax = 40.0;
    double outageAt = 20.0;
    double outageDuration = 3.0;
    double stepFactor = 0.25;
    double stepAt = 20.0;
    double stepDuration = 15.0;
    double hoInterval = 5.0;
    double hoGapMs = 80.0;
    double hoLoss = 0.0;
    double fluctPeriod = 0.5;
    double fluctSigma = 0.3;
    double randomLoss = 0.0;
    std::string disruption = "drop";
    uint64_t holdBytes = 0;
    double eventJitter = 2.0;
    double bottleneckDelayMs = 20.0;
    double accessDelayMs = 5.0;
    uint32_t queueBytes = 500000;
    std::string aqm = "none";
    double minRtoMs = 0.0; // 0 means use the ns-3 default.
    double flowStagger = 0.0;
    uint32_t segmentSize = 0;
    uint32_t sndBufBytes = 0;
    uint32_t rcvBufBytes = 0;
    uint32_t seed = 1;
    uint32_t run = 1;
    std::string freshLoss;
    double clockGranularityMs = 0.0;
    uint32_t copaClockUs = 1;
    int32_t jitterUs = -1;
    uint32_t debugMs = 0;
    std::string workloadFile;
    double baseRttMs = 0.0;
    double tailPercentile = 95.0;
    bool evalJson = false;

    CommandLine cmd(__FILE__);
    cmd.AddValue("ccs",
                 "Comma separated algorithm of every flow, e.g. bbrv2l,cubic; copa is Copa "
                 "with delta 0.5, copa-d<delta> sets another delta",
                 ccs);
    cmd.AddValue("profile", "Capacity profile: outage|step|handover|fluctuate|file", profile);
    cmd.AddValue("traceFile",
                 "Schedule for --profile=file: 'time_s rate_mbps [loss [delay_ms]]'",
                 traceFile);
    cmd.AddValue("lineFile", "Unified CSV lines scenario file (time,cap/up_cap/down_cap,...)", lineFile);
    cmd.AddValue("downTrace", "Mahimahi trace of the data direction (mahimahi)", downTrace);
    cmd.AddValue("upTrace", "Mahimahi trace of the ACK direction, empty = unlimited", upTrace);
    cmd.AddValue("linePeriodMs", "Repeat all lines at this common period in ms; 0 disables looping", g_linePeriodMs);
    cmd.AddValue("defaultDelayMs", "Default one-way delay", defaultDelayMs);
    cmd.AddValue("delayTrace", "One-way delay per 10 ms [ms], both directions (mahimahi)", delayTrace);
    cmd.AddValue("traceOffset", "Trace position at simulation time 0 [s] (mahimahi)", traceOffset);
    cmd.AddValue("prefix", "Prefix of the output files", prefix);
    cmd.AddValue("stopTime", "Simulation duration [s]", stopTime);
    cmd.AddValue("warmup", "Initial period excluded from the metrics [s]", g_warmup);
    cmd.AddValue("binMs", "Time-series bin [ms]", g_binMs);
    cmd.AddValue("capacity", "Nominal capacity [Mbit/s] (outage, step)", capacity);
    cmd.AddValue("capMin", "Minimum capacity [Mbit/s] (handover, fluctuate)", capMin);
    cmd.AddValue("capMax", "Maximum capacity [Mbit/s] (handover, fluctuate)", capMax);
    cmd.AddValue("outageAt", "Outage start [s]", outageAt);
    cmd.AddValue("outageDuration", "Outage duration [s]", outageDuration);
    cmd.AddValue("stepFactor", "Capacity multiplier during the step", stepFactor);
    cmd.AddValue("stepAt", "Step start [s]", stepAt);
    cmd.AddValue("stepDuration", "Step duration [s]", stepDuration);
    cmd.AddValue("hoInterval", "Mean time between handovers [s]", hoInterval);
    cmd.AddValue("hoGap", "Interruption of each handover [ms]", hoGapMs);
    cmd.AddValue("hoLoss", "Extra data loss right after a handover (1 s)", hoLoss);
    cmd.AddValue("fluctPeriod", "Capacity update period of the random walk [s]", fluctPeriod);
    cmd.AddValue("fluctSigma", "Log-normal step of the random walk", fluctSigma);
    cmd.AddValue("randomLoss", "Random data loss probability outside outages", randomLoss);
    cmd.AddValue("disruption",
                 "What an outage does to packets: drop (coverage hole) | hold (buffered "
                 "handover / RLC AM)",
                 disruption);
    cmd.AddValue("holdBytes", "Buffer of the hold mode [bytes], 0 = --queueBytes", holdBytes);
    cmd.AddValue("eventJitter", "Uniform +/- jitter of event times [s]", eventJitter);
    cmd.AddValue("bottleneckDelay", "Bottleneck one-way delay [ms]", bottleneckDelayMs);
    cmd.AddValue("accessDelay", "Access link one-way delay [ms]", accessDelayMs);
    cmd.AddValue("delaySlew",
                 "Largest decrease of the bottleneck delay per millisecond [ms] (file profile)",
                 g_delaySlewMs);
    cmd.AddValue("queueBytes", "Bottleneck buffer [bytes]", queueBytes);
    cmd.AddValue("aqm", "Bottleneck queue discipline: none (FIFO) | fqcodel", aqm);
    cmd.AddValue("minRto", "TCP minimum RTO [ms]", minRtoMs);
    cmd.AddValue("flowStagger", "Start offset between consecutive flows [s]", flowStagger);
    cmd.AddValue("segmentSize", "TCP segment size [bytes]", segmentSize);
    cmd.AddValue("sndBuf", "TCP send buffer [bytes]", sndBufBytes);
    cmd.AddValue("rcvBuf", "TCP receive buffer [bytes]", rcvBufBytes);
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("run", "RNG run number", run);
    cmd.AddValue("rttSampleLog", "Write every valid ACK RTT sample to CSV", rttSampleLog);
    cmd.AddValue("freshLoss",
                 "Rate samples report only freshly lost data (TcpSocketBase::FreshLossAccounting)",
                 freshLoss);
    cmd.AddValue("clockGranularityMs",
                 "TCP ClockGranularity in ms (0 = ns-3 default)",
                 clockGranularityMs);
    cmd.AddValue("copaClockUs",
                 "Copa timestamp/RTT quantization in us (1=us, 1000=1ms slow clock)",
                 copaClockUs);
    cmd.AddValue("jitterUs", "Override Copa-Auto jitter_us (-1=auto, >=0=manual)", jitterUs);
    cmd.AddValue("debugMs", "Emit Copa-Auto internal telemetry every N ms (0=off)", debugMs);
    cmd.AddValue("workloadFile", "Optional CSV workload file (time,size[,comment])", workloadFile);
    cmd.AddValue("baseRttMs", "Optional override total base RTT in ms (0=use bottleneckDelay/accessDelay)", baseRttMs);
    cmd.AddValue("tailPercentile", "Configurable tail percentile in (0, 100] for qdelay/norm_power/RCT/FCT slowdown", tailPercentile);
    cmd.AddValue("evalJson", "Print single-line JSON compatible with mahimahi_eval.py", evalJson);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_UNLESS(std::isfinite(tailPercentile) && tailPercentile > 0.0 && tailPercentile <= 100.0,
                        "Invalid --tailPercentile (expected (0, 100]): " << tailPercentile);

    std::vector<LinePoint> downCapacityLines;
    if (profile == "lines")
    {
        NS_ABORT_MSG_IF(lineFile.empty(), "--lineFile CSV path is required for --profile=lines");
        NS_ABORT_MSG_IF(defaultDelayMs < 0 || !std::isfinite(defaultDelayMs), "Invalid default delay");
        LineScenarioData sc = ReadLineCsv(lineFile, defaultDelayMs);
        g_upCapacity = std::move(sc.upCapacity);
        downCapacityLines = std::move(sc.downCapacity);
        g_upDelay = std::move(sc.upDelay);
        g_downDelay = std::move(sc.downDelay);
        accessDelayMs = 0;
        bottleneckDelayMs = 0;
        g_warmup = 0;
    }
    if (baseRttMs > 0.0 && profile != "lines")
    {
        if (!delayTrace.empty())
        {
            accessDelayMs = 0.25;
            bottleneckDelayMs = 0.25;
        }
        else
        {
            accessDelayMs = 0.5;
            bottleneckDelayMs = std::max(baseRttMs / 2.0 - accessDelayMs, 0.5);
        }
    }

    NS_ABORT_MSG_UNLESS(disruption == "drop" || disruption == "hold",
                        "--disruption must be drop or hold");
    g_holdMode = disruption == "hold";
    if (holdBytes == 0)
    {
        holdBytes = queueBytes;
    }
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);

    const std::vector<std::string> flows = Split(ccs);
    NS_ABORT_MSG_IF(flows.empty(), "--ccs is empty");
    // "bbrv2l"/"bbrv3l" = BBRv2/v3 with the Linux loss response. It is a
    // TypeId default, so it cannot be mixed with the original model.
    bool linux = false;
    bool original = false;
    for (const auto& f : flows)
    {
        linux |= (f == "bbrv2l" || f == "bbrv3l");
        original |= (f == "bbrv2" || f == "bbrv3");
    }
    NS_ABORT_MSG_IF(linux && original, "bbrv2l/bbrv3l cannot be mixed with bbrv2/bbrv3");
    Config::SetDefault("ns3::TcpBbrV2::LinuxLossResponse", BooleanValue(linux));
    const uint32_t n = flows.size();

    // ---- Capacity schedule -------------------------------------------------
    Ptr<UniformRandomVariable> uni = CreateObject<UniformRandomVariable>();
    uni->SetStream(1000);
    Ptr<NormalRandomVariable> gauss = CreateObject<NormalRandomVariable>();
    gauss->SetStream(1001);
    auto jit = [&](double t) {
        return t + (eventJitter > 0 ? uni->GetValue(-eventJitter, eventJitter) : 0.0);
    };

    std::vector<Segment> sched;
    if (profile == "outage")
    {
        double t = jit(outageAt);
        sched.push_back({0.0, capacity, randomLoss, "start"});
        if (outageDuration > 0)
        {
            sched.push_back({t, capacity, 1.0, "outage-begin"});
            sched.push_back({t + outageDuration, capacity, randomLoss, "outage-end"});
        }
    }
    else if (profile == "step")
    {
        double t = jit(stepAt);
        sched.push_back({0.0, capacity, randomLoss, "start"});
        sched.push_back({t, capacity * stepFactor, randomLoss, "step-down"});
        sched.push_back({t + stepDuration, capacity, randomLoss, "step-up"});
    }
    else if (profile == "handover")
    {
        double cap = uni->GetValue(capMin, capMax);
        sched.push_back({0.0, cap, randomLoss, "start"});
        double t = g_warmup + uni->GetValue(0.5, 1.5) * hoInterval;
        while (t < stopTime - 1.0)
        {
            sched.push_back({t, cap, 1.0, "outage-begin"});
            cap = uni->GetValue(capMin, capMax);
            double end = t + hoGapMs / 1000.0;
            if (hoLoss > 0)
            {
                sched.push_back({end, cap, hoLoss, "cell-change"});
                sched.push_back({end + 1.0, cap, randomLoss, ""});
            }
            else
            {
                sched.push_back({end, cap, randomLoss, "cell-change"});
            }
            t += uni->GetValue(0.5, 1.5) * hoInterval;
        }
    }
    else if (profile == "fluctuate")
    {
        double cap = std::sqrt(capMin * capMax);
        sched.push_back({0.0, cap, randomLoss, "start"});
        for (double t = fluctPeriod; t < stopTime; t += fluctPeriod)
        {
            cap = std::min(capMax, std::max(capMin, cap * std::exp(fluctSigma * gauss->GetValue())));
            sched.push_back({t, cap, randomLoss, "rate-change"});
        }
    }
    else if (profile == "mahimahi" || profile == "lines")
    {
        // The devices only have to outrun the trace.
        sched.push_back({0.0, 10000.0, 0.0, "start"});
    }
    else if (profile == "file")
    {
        std::ifstream in(traceFile);
        NS_ABORT_MSG_UNLESS(in.good(), "Cannot open --traceFile " << traceFile);
        std::string line;
        bool prevOutage = false;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
            {
                continue;
            }
            std::istringstream ls(line);
            double t;
            double r;
            double l = randomLoss;
            double d = -1.0;
            if (!(ls >> t >> r))
            {
                continue;
            }
            if (ls >> l)
            {
                ls >> d;
            }
            else
            {
                l = randomLoss;
            }
            bool out = l >= 1.0 || r <= 0.0;
            std::string ev = out ? (prevOutage ? "" : "outage-begin")
                                 : (prevOutage ? "outage-end" : "rate-change");
            Segment seg{t, std::max(r, 0.001), out ? 1.0 : l, ev};
            seg.delayMs = d;
            sched.push_back(seg);
            prevOutage = out;
        }
        NS_ABORT_MSG_IF(sched.empty(), "Empty --traceFile");
        if (sched[0].delayMs >= 0)
        {
            bottleneckDelayMs = sched[0].delayMs;
        }
    }
    else
    {
        NS_FATAL_ERROR("Unknown profile '" << profile << "'");
    }

    // Leave TCP defaults untouched unless the caller explicitly overrides them.
    if (segmentSize) Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(segmentSize));
    if (sndBufBytes) Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(sndBufBytes));
    if (rcvBufBytes) Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(rcvBufBytes));
    if (clockGranularityMs > 0) Config::SetDefault("ns3::TcpSocketBase::ClockGranularity", TimeValue(MilliSeconds(clockGranularityMs)));
    if (minRtoMs > 0) Config::SetDefault("ns3::TcpSocketBase::MinRto", TimeValue(MilliSeconds(minRtoMs)));
    if (!freshLoss.empty())
    {
        NS_ABORT_MSG_IF(freshLoss != "0" && freshLoss != "1" && freshLoss != "true" && freshLoss != "false", "Invalid --freshLoss");
        Config::SetDefault("ns3::TcpSocketBase::FreshLossAccounting", BooleanValue(freshLoss == "1" || freshLoss == "true"));
    }
    TypeId::AttributeInformation tcpAttribute;
    TcpSocket::GetTypeId().LookupAttributeByName("SegmentSize", &tcpAttribute);
    segmentSize = DynamicCast<const UintegerValue>(tcpAttribute.initialValue)->Get();
    TcpSocketBase::GetTypeId().LookupAttributeByName("MinRto", &tcpAttribute);
    minRtoMs = DynamicCast<const TimeValue>(tcpAttribute.initialValue)->Get().GetSeconds() * 1000.0;

    // ---- Topology ----------------------------------------------------------
    NodeContainer senders;
    senders.Create(n);
    Ptr<Node> router = CreateObject<Node>();
    Ptr<Node> receiver = CreateObject<Node>();

    InternetStackHelper internet;
    internet.Install(senders);
    internet.Install(router);
    internet.Install(receiver);
    for (uint32_t i = 0; i < n; i++)
    {
        senders.Get(i)->GetObject<TcpL4Protocol>()->SetAttribute(
            "SocketType",
            TypeIdValue(TypeId::LookupByName(CcTypeName(flows[i]))));
    }

    PointToPointHelper access;
    access.SetDeviceAttribute(
        "DataRate",
        StringValue((profile == "lines" || profile == "mahimahi") ? "10Gbps" : "1Gbps"));
    access.SetChannelAttribute("Delay", TimeValue(MilliSeconds(accessDelayMs)));

    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate",
                                  DataRateValue(DataRate(static_cast<uint64_t>(
                                      std::max(sched[0].rateMbps, 0.001) * 1e6))));
    bottleneck.SetChannelAttribute("Delay", TimeValue(MilliSeconds(bottleneckDelayMs)));
    // Small device queue: the buffer lives in the queue disc below.
    bottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("5p"));
    NetDeviceContainer bnDevs = bottleneck.Install(router, receiver);

    TrafficControlHelper tch;
    if (aqm == "none")
    {
        tch.SetRootQueueDisc("ns3::FifoQueueDisc",
                             "MaxSize",
                             QueueSizeValue(QueueSize(QueueSizeUnit::BYTES, queueBytes)));
    }
    else if (aqm == "fqcodel")
    {
        // FQ-CoDel only supports a limit in packets.
        tch.SetRootQueueDisc(
            "ns3::FqCoDelQueueDisc",
            "MaxSize",
            QueueSizeValue(QueueSize(QueueSizeUnit::PACKETS,
                                     std::max<uint32_t>(queueBytes / (segmentSize + 52), 10))));
    }
    else
    {
        NS_FATAL_ERROR("Unknown --aqm '" << aqm << "'");
    }
    tch.Install(bnDevs.Get(0));

    Ipv4AddressHelper addr;
    for (uint32_t i = 0; i < n; i++)
    {
        NetDeviceContainer d = access.Install(senders.Get(i), router);
        std::ostringstream net;
        net << "10.1." << i << ".0";
        addr.SetBase(net.str().c_str(), "255.255.255.0");
        addr.Assign(d);
    }
    addr.SetBase("10.2.0.0", "255.255.255.0");
    Ipv4InterfaceContainer bnIfaces = addr.Assign(bnDevs);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    g_forwardDevice = DynamicCast<PointToPointNetDevice>(bnDevs.Get(0));
    g_bnChannel = DynamicCast<PointToPointChannel>(g_forwardDevice->GetChannel());
    g_accessDelayMs = accessDelayMs;
    g_delayMs = bottleneckDelayMs;
    g_delayTargetMs = bottleneckDelayMs;
    g_reverseDevice = DynamicCast<PointToPointNetDevice>(bnDevs.Get(1));
    g_dataLoss = CreateObject<RateErrorModel>();
    g_dataLoss->SetUnit(RateErrorModel::ERROR_UNIT_PACKET);
    g_dataLoss->SetRate(0.0);
    g_dataLoss->AssignStreams(2000);
    g_dataGate = CreateObject<HoldGate>();
    g_dataGate->Setup(g_reverseDevice, g_dataLoss, holdBytes);
    g_reverseDevice->SetReceiveErrorModel(g_dataGate);
    g_ackLoss = CreateObject<RateErrorModel>();
    g_ackLoss->SetUnit(RateErrorModel::ERROR_UNIT_PACKET);
    g_ackLoss->SetRate(0.0);
    g_ackLoss->AssignStreams(2001);
    g_ackGate = CreateObject<HoldGate>();
    g_ackGate->Setup(g_forwardDevice, g_ackLoss, holdBytes);
    g_forwardDevice->SetReceiveErrorModel(g_ackGate);
    if (profile == "lines")
    {
        g_mmDataGate = CreateObject<MahimahiGate>();
        g_mmDataGate->SetupLines(g_reverseDevice, g_upCapacity, queueBytes, g_upDelay);
        g_reverseDevice->SetReceiveErrorModel(g_mmDataGate);
        g_mmAckGate = CreateObject<MahimahiGate>();
        g_mmAckGate->SetupLines(g_forwardDevice, downCapacityLines, queueBytes, g_downDelay);
        g_forwardDevice->SetReceiveErrorModel(g_mmAckGate);
    }
    else if (profile == "mahimahi")
    {
        const int64_t offsetMs = static_cast<int64_t>(traceOffset * 1000);
        std::vector<int64_t> delays;
        if (!delayTrace.empty())
        {
            std::ifstream in(delayTrace);
            NS_ABORT_MSG_UNLESS(in.good(), "Cannot open trace " << delayTrace);
            for (int64_t d; in >> d;)
            {
                delays.push_back(d);
            }
            NS_ABORT_MSG_IF(delays.empty(), "Unusable trace " << delayTrace);
        }
        g_mmDataGate = CreateObject<MahimahiGate>();
        g_mmDataGate->Setup(g_reverseDevice, downTrace, queueBytes, offsetMs, delays, randomLoss);
        g_mmDataGate->SetStatsWarmupS(workloadFile.empty() ? g_warmup : 0.0);
        g_reverseDevice->SetReceiveErrorModel(g_mmDataGate);
        if (!upTrace.empty())
        {
            g_mmAckGate = CreateObject<MahimahiGate>();
            g_mmAckGate->Setup(g_forwardDevice, upTrace, queueBytes, offsetMs, delays, 0.0);
            g_forwardDevice->SetReceiveErrorModel(g_mmAckGate);
        }
    }

    // ---- Applications ------------------------------------------------------
    const uint16_t basePort = 5000;
    std::vector<double> wkArrSec;
    std::vector<uint32_t> wkSizes;
    Ptr<MortiseAppReceiver> appRx;
    const bool appMode = !workloadFile.empty();
    if (appMode)
    {
        NS_ABORT_MSG_UNLESS(LoadWorkloadFile(workloadFile, wkArrSec, wkSizes, 0.001),
                            "Failed to load --workloadFile " << workloadFile);

    }

    for (uint32_t i = 0; i < n; i++)
    {
        const uint16_t port = basePort + i;
        const double start = 0.001 + i * flowStagger;
        if (appMode && i == 0)
        {
            appRx = CreateObject<MortiseAppReceiver>();
            appRx->Setup(port, wkArrSec, wkSizes);
            receiver->AddApplication(appRx);
            appRx->SetStartTime(Seconds(0.0));
            appRx->SetStopTime(Seconds(stopTime));

            Ptr<MortiseAppSender> appTx = CreateObject<MortiseAppSender>();
            appTx->Setup(InetSocketAddress(bnIfaces.GetAddress(1), port), wkArrSec, wkSizes);
            senders.Get(i)->AddApplication(appTx);
            appTx->SetStartTime(Seconds(start));
            appTx->SetStopTime(Seconds(stopTime));
            g_sinks.push_back(nullptr);
        }
        else
        {
            PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                        InetSocketAddress(Ipv4Address::GetAny(), port));
            ApplicationContainer sinkApp = sinkHelper.Install(receiver);
            sinkApp.Start(Seconds(0));
            g_sinks.push_back(DynamicCast<PacketSink>(sinkApp.Get(0)));

            BulkSendHelper bulk("ns3::TcpSocketFactory",
                                InetSocketAddress(bnIfaces.GetAddress(1), port));
            bulk.SetAttribute("MaxBytes", UintegerValue(0));
            ApplicationContainer src = bulk.Install(senders.Get(i));
            src.Start(Seconds(start));
        }
        Simulator::Schedule(Seconds(start + 0.001),
                            &ConnectTraces,
                            senders.Get(i)->GetId(),
                            i);
        if (IsAdaptiveCopaName(flows[i]) && !evalJson && !prefix.empty() && !g_copaEst.is_open())
        {
            g_copaEst.open(prefix + "-copaest.data");
            g_copaEst << "# t j_ms k_ms rho r0_ms dq_target_ms delta\n";
        }
        Simulator::Schedule(Seconds(start) + MicroSeconds(1),
                            &ConfigureCcFlow,
                            senders.Get(i)->GetId(),
                            i,
                            flows[i],
                            jitterUs,
                            copaClockUs,
                            debugMs);
    }
    g_lastRx.assign(n, 0);
    g_rxAtWarmup.assign(n, 0);
    g_rttSamples.assign(n, {});
    g_baseRttSamples.assign(n, {});
    g_srttSamples.assign(n, {});
    g_binRttSum.assign(n, 0.0);
    g_binRttCount.assign(n, 0);
    g_binLastRttSum.assign(n, 0.0);
    g_binLastRttCount.assign(n, 0);
    g_retx.assign(n, 0);

    // ---- Outputs and schedule ---------------------------------------------
    if (!evalJson)
    {
        g_series.open(prefix + "-timeseries.csv");
        if (rttSampleLog)
        {
            g_rawRttLog.open(prefix + "-rtt-samples.csv");
            g_rawRttLog << "time_s,flow,rtt_ms\n";
        }
        g_series << "time_s,capacity_mbps,outage";
        for (uint32_t i = 0; i < n; i++)
        {
            g_series << ",tput" << i << "_mbps";
        }
        for (uint32_t i = 0; i < n; i++)
        {
            g_series << ",rtt" << i << "_ms";
        }
        g_series << ",cwnd0_bytes";
        // rtt<i>_ms is the smoothed RTT, lrtt<i>_ms the mean RTT sample of the bin.
        for (uint32_t i = 0; i < n; i++)
        {
            g_series << ",lrtt" << i << "_ms";
        }
        g_series << ",base_rtt_ms\n";
        g_events.open(prefix + "-events.csv");
        g_events << "time_s,event,capacity_mbps\n";
    }

    for (const auto& seg : sched)
    {
        if (seg.start < stopTime)
        {
            Simulator::Schedule(Seconds(seg.start), &ApplySegment, seg);
        }
    }
    Simulator::Schedule(MilliSeconds(g_binMs), &SampleBin);
    Simulator::Schedule(Seconds(g_warmup), &EndWarmup);

    if (!evalJson)
    {
        std::cout << "capacity-trace: ccs=" << ccs << " profile=" << profile << " run=" << run
                  << " queue=" << queueBytes << "B aqm=" << aqm << " baseRTT="
                  << 2 * (bottleneckDelayMs + accessDelayMs) << "ms minRto=" << minRtoMs << "ms"
                  << std::endl;
    }

    Simulator::Stop(Seconds(stopTime));
    Simulator::Run();
    if (appMode && appRx && appRx->rctsMs.size() == wkSizes.size())
    {
        stopTime = Simulator::Now().GetSeconds();
    }

    // ---- Summary -----------------------------------------------------------
    const double measured = std::max(stopTime - (appMode ? 0.0 : g_warmup), 1e-3);
    double sum = 0.0;
    double sumSq = 0.0;
    std::vector<double> tput(n);
    for (uint32_t i = 0; i < n; i++)
    {
        uint64_t rxBytes = (appMode && i == 0 && appRx)
                               ? appRx->totalRx
                               : (g_sinks[i] ? (g_sinks[i]->GetTotalRx() - g_rxAtWarmup[i]) : 0);
        tput[i] = rxBytes * 8.0 / measured / 1e6;
        sum += tput[i];
        sumSq += tput[i] * tput[i];
    }
    double avgCap = g_capacityIntegral / std::max(stopTime - g_warmup, 1e-3);
    if (profile == "lines")
    {
        avgCap = LineIntegral(g_upCapacity, stopTime * 1000.0, g_linePeriodMs) * 12.0 / (stopTime * 1000.0);
    }
    const double jain = sumSq > 0 ? sum * sum / (n * sumSq) : 0.0;

    if (evalJson)
    {
        double effBaseRttMs = (baseRttMs > 0.0) ? baseRttMs : (2.0 * (bottleneckDelayMs + accessDelayMs));
        if (profile == "lines")
        {
            const double durMs = std::max(stopTime * 1000.0, 1e-3);
            effBaseRttMs = (LineIntegral(g_upDelay, durMs, g_linePeriodMs) +
                            LineIntegral(g_downDelay, durMs, g_linePeriodMs)) /
                           durMs;
        }
        else if (profile == "file" && !g_baseRttSamples[0].empty())
        {
            double sumBase = 0.0;
            for (double base : g_baseRttSamples[0]) sumBase += base;
            effBaseRttMs = sumBase / g_baseRttSamples[0].size();
        }
        std::vector<double> qd;
        uint64_t delivPkts = 0;
        uint64_t dropPkts = 0;
        if (g_mmDataGate)
        {
            qd = g_mmDataGate->GetQDelaysMs();
            delivPkts = g_mmDataGate->GetDeliveredPkts();
            dropPkts = g_mmDataGate->GetDroppedPkts();
        }
        if (qd.empty() && profile != "lines")
        {
            const auto& rttSrc = (!g_rttSamples.empty() && !g_rttSamples[0].empty())
                                     ? g_rttSamples[0]
                                     : (!g_srttSamples.empty() ? g_srttSamples[0] : g_rttSamples[0]);
            qd.reserve(rttSrc.size());
            for (size_t i = 0; i < rttSrc.size(); ++i)
            {
                double base = (profile == "file") && i < g_baseRttSamples[0].size()
                                  ? g_baseRttSamples[0][i] : effBaseRttMs;
                qd.push_back(std::max(rttSrc[i] - base, 0.0));
            }
        }
        if (delivPkts == 0 && dropPkts == 0 && !g_retx.empty())
        {
            delivPkts = static_cast<uint64_t>(std::max(sum * 1e6 / 8.0 * measured / 1448.0, 0.0));
            dropPkts = g_retx[0];
        }
        double qdAvg = 0.0;
        for (double v : qd)
        {
            qdAvg += v;
        }
        if (!qd.empty())
        {
            qdAvg /= static_cast<double>(qd.size());
        }
        double qdP95 = Percentile(qd, 95);
        double qdTail = Percentile(qd, tailPercentile);
        double dropRate = (delivPkts + dropPkts > 0)
                              ? static_cast<double>(dropPkts) / static_cast<double>(delivPkts + dropPkts)
                              : 0.0;
        double util = (avgCap > 1e-6) ? (sum / avgCap) : 0.0;
        double estRttMs = effBaseRttMs + qdAvg;
        double estRttP95Ms = effBaseRttMs + qdP95;
        double estRttTailMs = effBaseRttMs + qdTail;
        double utilPower = (estRttMs > 1e-6) ? (util / (estRttMs / 1000.0)) : 0.0;
        double normPower = utilPower * (effBaseRttMs / 1000.0);
        double normPowerP95 = (estRttP95Ms > 1e-6) ? (util * effBaseRttMs / estRttP95Ms) : 0.0;
        double normPowerTail = (estRttTailMs > 1e-6) ? (util * effBaseRttMs / estRttTailMs) : 0.0;
        double tputPower = (estRttMs > 1e-6) ? (sum / (estRttMs / 1000.0)) : 0.0;

        std::ostringstream js;
        js << std::fixed << std::setprecision(6);
        js << "{\"duration_s\":" << measured
           << ",\"tail_percentile\":" << tailPercentile
           << ",\"capacity_mbps\":" << avgCap
           << ",\"throughput_mbps\":" << sum
           << ",\"utilization\":" << util
           << ",\"qdelay_avg_ms\":" << qdAvg
           << ",\"qdelay_p95_ms\":" << qdP95
           << ",\"qdelay_tail_ms\":" << qdTail
           << ",\"base_rtt_ms\":" << effBaseRttMs
           << ",\"est_rtt_ms\":" << estRttMs
           << ",\"util_power\":" << utilPower
           << ",\"norm_power\":" << normPower
           << ",\"norm_power_p95\":" << normPowerP95
           << ",\"norm_power_tail\":" << normPowerTail
           << ",\"tput_power\":" << tputPower
           << ",\"drop_rate\":" << dropRate;
        if (appMode && appRx)
        {
            std::vector<double> rcts = appRx->rctsMs;
            double meanRct = 0.0, p50Rct = 0.0, p95Rct = 0.0, tailRct = 0.0;
            double fctSlowdown = 0.0, fctSlowdownP95 = 0.0, fctSlowdownTail = 0.0, mortiseQoe = 0.0;
            if (!rcts.empty())
            {
                double qoeSum = 0.0, logSdSum = 0.0;
                std::vector<double> sds;
                sds.reserve(rcts.size());
                for (size_t i = 0; i < rcts.size(); i++)
                {
                    meanRct += rcts[i];
                    uint32_t sz = (i < wkSizes.size()) ? wkSizes[i] : 1448U;
                    double rctS = std::max(rcts[i] / 1000.0, 1e-6);
                    double thrMbps = (static_cast<double>(sz) * 8.0) / (rctS * 1e6);
                    qoeSum += (thrMbps * 0.5) / std::max(rcts[i], 1.0) * 100.0;
                    double requestBase = effBaseRttMs;
                    if (profile == "lines" && i < wkArrSec.size())
                    {
                        int64_t t = static_cast<int64_t>(wkArrSec[i] * 1000.0);
                        requestBase = PeriodicLineValue(g_upDelay, t) + PeriodicLineValue(g_downDelay, t);
                    }
                    else if (profile == "file" && i < wkArrSec.size())
                    {
                        requestBase = 2.0 * (bottleneckDelayMs + accessDelayMs);
                        for (const auto& seg : sched)
                        {
                            if (seg.start > wkArrSec[i]) break;
                            if (seg.delayMs >= 0) requestBase = 2.0 * (seg.delayMs + accessDelayMs);
                        }
                    }
                    double idealMs = std::max(requestBase, 1.0) +
                                     ((avgCap > 1e-6) ? (static_cast<double>(sz) * 8.0 / (avgCap * 1000.0)) : 0.0);
                    double sd = std::max(rcts[i] / std::max(idealMs, 1e-3), 1.0);
                    sds.push_back(sd);
                    logSdSum += std::log(sd);
                }
                meanRct /= static_cast<double>(rcts.size());
                mortiseQoe = qoeSum / static_cast<double>(rcts.size());
                fctSlowdown = std::exp(logSdSum / static_cast<double>(rcts.size()));
                fctSlowdownP95 = Percentile(sds, 95);
                fctSlowdownTail = Percentile(sds, tailPercentile);
                std::vector<double> sortedRcts = rcts;
                p50Rct = Percentile(sortedRcts, 50);
                p95Rct = Percentile(sortedRcts, 95);
                tailRct = Percentile(sortedRcts, tailPercentile);
            }
            uint64_t retx = g_retx.empty() ? 0 : g_retx[0];
            double retxRatio = (delivPkts + retx > 0)
                                   ? static_cast<double>(retx) / static_cast<double>(delivPkts + retx)
                                   : 0.0;
            js << ",\"requests_expected\":" << wkSizes.size()
               << ",\"requests\":" << rcts.size()
               << ",\"rct_mean_ms\":" << meanRct
               << ",\"rct_p50_ms\":" << p50Rct
               << ",\"rct_p95_ms\":" << p95Rct
               << ",\"rct_tail_ms\":" << tailRct
               << ",\"retrans_pkts\":" << retx
               << ",\"retrans_ratio\":" << retxRatio
               << ",\"fct_slowdown\":" << fctSlowdown
               << ",\"fct_slowdown_p95\":" << fctSlowdownP95
               << ",\"fct_slowdown_tail\":" << fctSlowdownTail
               << ",\"mortise_qoe\":" << mortiseQoe;
        }
        js << "}\n";
        std::cout << js.str() << std::flush;
        Simulator::Destroy();
        return 0;
    }

    if (rttSampleLog && g_mmDataGate)
    {
        std::ofstream qdLog(prefix + "-qdelay-samples.csv");
        qdLog << "time_s,qdelay_ms\n";
        const auto& qdTimes = g_mmDataGate->GetQDelayTimesS();
        const auto& qdVals = g_mmDataGate->GetQDelaysMs();
        const size_t m = std::min(qdTimes.size(), qdVals.size());
        qdLog << std::setprecision(12);
        for (size_t i = 0; i < m; ++i)
        {
            qdLog << qdTimes[i] << "," << qdVals[i] << "\n";
        }
    }

    std::ofstream json(prefix + "-metrics.json");
    json << std::fixed << std::setprecision(4);
    json << "{\n  \"rtt_stats_version\": 2, \"rtt_source\": \"ack_rtt_samples\",\n"
         << "  \"ccs\": \"" << ccs << "\", \"profile\": \"" << profile
         << "\", \"run\": " << run << ", \"queue_bytes\": " << queueBytes << ", \"aqm\": \""
         << aqm << "\", \"disruption\": \"" << disruption
         << "\", \"hold_overflow_bytes\": " << g_dataGate->GetOverflowBytes()
         << ", \"base_rtt_ms\": " << 2 * (bottleneckDelayMs + accessDelayMs)
         << ", \"min_rto_ms\": " << minRtoMs << ",\n  \"avg_capacity_mbps\": " << avgCap
         << ", \"total_throughput_mbps\": " << sum
         << ", \"utilization\": " << (avgCap > 0 ? sum / avgCap : 0.0) << ", \"jain\": " << jain
         << ",\n  \"flows\": [\n";
    for (uint32_t i = 0; i < n; i++)
    {
        std::vector<double> r = g_rttSamples[i];
        double mean = 0.0;
        for (double x : r)
        {
            mean += x;
        }
        mean = r.empty() ? 0.0 : mean / r.size();
        double srttMean = 0.0;
        for (double x : g_srttSamples[i])
        {
            srttMean += x;
        }
        srttMean = g_srttSamples[i].empty() ? 0.0 : srttMean / g_srttSamples[i].size();
        double p50 = Percentile(r, 50);
        double p95 = Percentile(r, 95);
        double p99 = Percentile(r, 99);
        json << "    {\"flow\": " << i << ", \"cc\": \"" << flows[i]
             << "\", \"throughput_mbps\": " << tput[i] << ", \"retx\": " << g_retx[i]
             << ", \"srtt_mean_ms\": " << srttMean << ", \"rtt_samples\": " << r.size()
             << ", \"rtt_mean_ms\": " << mean << ", \"rtt_p50_ms\": " << p50
             << ", \"rtt_p95_ms\": " << p95 << ", \"rtt_p99_ms\": " << p99 << "}"
             << (i + 1 < n ? "," : "") << "\n";
        std::cout << "flow " << i << " " << flows[i] << ": " << std::setprecision(3) << tput[i]
                  << " Mbps, rtt p95 " << p95 << " ms, retx " << g_retx[i] << std::endl;
    }
    std::vector<double> pooled;
    for (const auto& samples : g_rttSamples)
    {
        pooled.insert(pooled.end(), samples.begin(), samples.end());
    }
    json << "  ],\n  \"rtt_p95_ms\": " << Percentile(pooled, 95)
         << ", \"rtt_p99_ms\": " << Percentile(pooled, 99)
         << ", \"rtt_samples\": " << pooled.size() << "\n}\n";
    std::cout << "capacity " << avgCap << " Mbps, utilisation " << (avgCap > 0 ? sum / avgCap : 0)
              << ", Jain " << jain << std::endl;

    Simulator::Destroy();
    return 0;
}
