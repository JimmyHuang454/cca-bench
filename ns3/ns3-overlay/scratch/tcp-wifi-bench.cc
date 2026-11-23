/*
 * 802.11ax Wi-Fi TCP CCA benchmark (ns-3 native Wi-Fi MAC/PHY + CSMA/CA contention).
 * Supports all Copa-Auto / Mortise / ns-3 native CCAs, Bulk & Mortise .wk workloads,
 * UDP capacity probe, fast/slow clock granularity, and unified --evalJson output.
 */
#include "mortise-workload.h"

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/wifi-module.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace ns3;

namespace
{

std::vector<double> g_rttMs;
uint64_t g_retx{0};
double g_warmupS{5.0};

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
           cc == "copa_auto_j0" || cc == "copa_auto_nc_j0" || cc == "copa_auto_nc_j3" || cc == "copa_orig" ||
           cc.rfind("copa-d", 0) == 0 || cc.rfind("copa_auto_theta", 0) == 0 ||
           cc.rfind("copa_auto_nc_theta", 0) == 0;
}

std::string
CcTypeName(const std::string& name)
{
    const std::string cc = BaseCc(name);
    if (cc == "cubic") return "ns3::TcpCubic";
    if (cc == "bbr") return "ns3::TcpBbr";
    if (cc == "bbr2" || cc == "bbrv2" || cc == "bbrv2l") return "ns3::TcpBbrV2";
    if (cc == "bbr3" || cc == "bbrv3" || cc == "bbrv3l") return "ns3::TcpBbrV3";
    if (cc == "vegas") return "ns3::TcpVegas";
    if (cc == "veno") return "ns3::TcpVeno";
    if (cc == "yeah") return "ns3::TcpYeah";
    if (cc == "illinois") return "ns3::TcpIllinois";
    if (cc == "westwood") return "ns3::TcpWestwoodPlus";
    if (cc == "hybla") return "ns3::TcpHybla";
    if (cc == "reno" || cc == "newreno") return "ns3::TcpLinuxReno";
    if (cc == "mortise_copa") return "ns3::TcpMortiseCopa";
    if (cc == "copa-auto-v2" || cc == "copa_auto_v2")
    {
        return "ns3::TcpCopaAutoV2";
    }
    if (IsCopaKernelName(name)) return "ns3::TcpCopaKernel";
    TypeId tid;
    NS_ABORT_MSG_UNLESS(TypeId::LookupByNameFailSafe(cc, &tid), "Unknown CC: " << cc);
    return cc;
}

void
ConfigureCc(uint32_t nodeId, std::string name, int32_t jitterUs, uint32_t copaClockUs)
{
    const std::string cc = BaseCc(name);
    if (cc == "mortise_copa")
    {
        Config::Set("/NodeList/" + std::to_string(nodeId) +
                        "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpMortiseCopa/CopaClockUs",
                    UintegerValue(std::max<uint32_t>(copaClockUs, 1U)));
        return;
    }
    if (cc == "copa-auto-v2" || cc == "copa_auto_v2")
    {
        const std::string path = "/NodeList/" + std::to_string(nodeId) +
            "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpCopaAutoV2/";
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
    uint32_t initDeltaQ20 = 524288U; /* 0.50 * 2^20 */

    if (cc == "copa" || cc == "copa_orig")
    {
        initDeltaQ20 = 524288U;
    }
    else if (cc.rfind("copa-d", 0) == 0)
    {
        double d = std::stod(cc.substr(6));
        initDeltaQ20 = static_cast<uint32_t>(std::clamp(std::round(d * 1048576.0), 1049.0, 1048576.0));
    }
    else if (cc == "copa-auto" || cc == "copa_auto")
    {
        adaptive = true;
    }
    else if (cc == "copa_auto_nc")
    {
        adaptive = true;
        modeSwitching = false;
    }
    else if (cc == "copa_auto_j0")
    {
        adaptive = true;
        modeSwitching = !nosw;
        effJitterUs = 0;
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
}

void
RttTracer(Time /*oldVal*/, Time newVal)
{
    if (Simulator::Now().GetSeconds() >= g_warmupS && newVal.IsStrictlyPositive())
    {
        g_rttMs.push_back(newVal.GetSeconds() * 1000.0);
    }
}

void
RetxTracer(Ptr<const Packet>,
           const TcpHeader&,
           const Address&,
           const Address&,
           Ptr<const TcpSocketBase>)
{
    if (Simulator::Now().GetSeconds() >= g_warmupS)
    {
        g_retx++;
    }
}

void
ConnectSender(uint32_t nodeId)
{
    const std::string base = "/NodeList/" + std::to_string(nodeId) + "/$ns3::TcpL4Protocol/SocketList/0/";
    Config::ConnectWithoutContext(base + "RTT", MakeCallback(&RttTracer));
    Config::ConnectWithoutContext(base + "Retransmission", MakeCallback(&RetxTracer));
}

double
Percentile(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = std::min<size_t>(static_cast<size_t>(std::ceil(p / 100.0 * v.size())) - 1, v.size() - 1);
    return v[idx];
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string cc = "copa_auto_nc";
    double distanceM = 5.0;
    double wallLossDb = 0.0;
    uint32_t channelWidthMhz = 80;
    uint32_t bgStas = 0;
    double bgMbpsEach = 0.0;
    double serverDelayMs = 10.0;
    double durS = 40.0;
    double warmupS = 5.0;
    double clockGranularityMs = 1.0;
    uint32_t copaClockUs = 1;
    int32_t jitterUs = -1;
    uint32_t seed = 1;
    uint32_t run = 1;
    bool capacityProbe = false;
    double capacityMbps = 0.0;
    double baseRttMs = 0.0;
    std::string workloadFile;

    CommandLine cmd(__FILE__);
    cmd.AddValue("cc", "Congestion control algorithm", cc);
    cmd.AddValue("distanceM", "STA distance from AP in meters", distanceM);
    cmd.AddValue("wallLossDb", "Extra wall attenuation in dB", wallLossDb);
    cmd.AddValue("channelWidthMhz", "Wi-Fi channel width MHz (20/40/80)", channelWidthMhz);
    cmd.AddValue("bgStas", "Number of competing background STAs", bgStas);
    cmd.AddValue("bgMbpsEach", "UDP load per background STA in Mbps", bgMbpsEach);
    cmd.AddValue("serverDelayMs", "Wired backhaul one-way delay in ms", serverDelayMs);
    cmd.AddValue("durS", "Simulation duration in s", durS);
    cmd.AddValue("warmupS", "Warmup window in s", warmupS);
    cmd.AddValue("clockGranularityMs", "TCP ClockGranularity in ms", clockGranularityMs);
    cmd.AddValue("copaClockUs", "Copa clock quantization in us", copaClockUs);
    cmd.AddValue("jitterUs", "Override Copa-Auto jitter_us (-1=auto)", jitterUs);
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("run", "RNG run", run);
    cmd.AddValue("capacityProbe", "Run UDP capacity probe instead of TCP", capacityProbe);
    cmd.AddValue("capacityMbps", "Reference channel capacity Mbps from UDP probe", capacityMbps);
    cmd.AddValue("baseRttMs", "Reference unloaded base RTT ms", baseRttMs);
    cmd.AddValue("workloadFile", "Optional CSV workload file (time,size[,comment])", workloadFile);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);
    const bool appMode = !workloadFile.empty() && !capacityProbe;
    g_warmupS = appMode ? 0.0 : warmupS;

    std::vector<double> wkArr;
    std::vector<uint32_t> wkSizes;
    if (appMode)
    {
        NS_ABORT_MSG_UNLESS(LoadMortiseWorkload(workloadFile, wkArr, wkSizes, 0.5),
                            "Failed to load workloadFile: " << workloadFile);
        durS = std::max(durS, wkArr.back() + 20.0);
    }

    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(8 * 1024 * 1024));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(8 * 1024 * 1024));
    Config::SetDefault("ns3::TcpSocketBase::Sack", BooleanValue(true));
    Config::SetDefault("ns3::TcpSocketBase::ClockGranularity",
                       TimeValue(MilliSeconds(std::max(clockGranularityMs, 0.001))));
    Config::SetDefault("ns3::TcpSocketState::EnablePacing", BooleanValue(true));
    Config::SetDefault("ns3::TcpL4Protocol::RecoveryType", TypeIdValue(TcpPrrRecovery::GetTypeId()));
    if (!capacityProbe)
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                           TypeIdValue(TypeId::LookupByName(CcTypeName(cc))));
    }

    NodeContainer serverNode;
    serverNode.Create(1);
    NodeContainer apNode;
    apNode.Create(1);
    NodeContainer staNodes;
    staNodes.Create(1 + bgStas);

    PointToPointHelper backhaul;
    backhaul.SetDeviceAttribute("DataRate", StringValue("10Gbps"));
    backhaul.SetChannelAttribute("Delay", TimeValue(MilliSeconds(serverDelayMs)));
    NetDeviceContainer bhDevs = backhaul.Install(serverNode.Get(0), apNode.Get(0));

    YansWifiChannelHelper channel;
    channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channel.AddPropagationLoss("ns3::LogDistancePropagationLossModel",
                               "Exponent",
                               DoubleValue(3.0),
                               "ReferenceDistance",
                               DoubleValue(1.0),
                               "ReferenceLoss",
                               DoubleValue(46.6777 + wallLossDb));
    Ptr<YansWifiChannel> wifiCh = channel.Create();

    YansWifiPhyHelper phy;
    phy.SetChannel(wifiCh);
    std::string chCfg = "{0, " + std::to_string(channelWidthMhz) + ", BAND_5GHZ, 0}";
    phy.Set("ChannelSettings", StringValue(chCfg));

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211ax);
    wifi.SetRemoteStationManager("ns3::IdealWifiManager");

    WifiMacHelper mac;
    Ssid ssid = Ssid("copa-wifi-bench");
    mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid));
    NetDeviceContainer staDevs = wifi.Install(phy, mac, staNodes);
    mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
    NetDeviceContainer apDev = wifi.Install(phy, mac, apNode);

    MobilityHelper mob;
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0.0, 0.0, 2.0));              // AP
    pos->Add(Vector(distanceM, 0.0, 1.0));        // Primary STA
    for (uint32_t i = 0; i < bgStas; i++)
    {
        double angle = 2.0 * M_PI * (i + 1) / (bgStas + 1);
        pos->Add(Vector(distanceM * std::cos(angle), distanceM * std::sin(angle), 1.0));
    }
    mob.SetPositionAllocator(pos);
    mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mob.Install(apNode);
    mob.Install(staNodes);

    InternetStackHelper internet;
    internet.Install(serverNode);
    internet.Install(apNode);
    internet.Install(staNodes);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    ipv4.Assign(bhDevs);
    ipv4.SetBase("10.1.2.0", "255.255.255.0");
    ipv4.Assign(apDev);
    Ipv4InterfaceContainer staIfaces = ipv4.Assign(staDevs);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    const uint16_t port = 5000;
    Ptr<PacketSink> bulkSink;
    Ptr<MortiseAppReceiver> appRx;
    uint64_t rxAtWarmup = 0;

    if (capacityProbe)
    {
        PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sa = sink.Install(staNodes.Get(0));
        sa.Start(Seconds(0.0));
        bulkSink = DynamicCast<PacketSink>(sa.Get(0));

        OnOffHelper onoff("ns3::UdpSocketFactory", InetSocketAddress(staIfaces.GetAddress(0), port));
        onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        onoff.SetAttribute("DataRate", DataRateValue(DataRate("600Mbps")));
        onoff.SetAttribute("PacketSize", UintegerValue(1400));
        ApplicationContainer ta = onoff.Install(serverNode.Get(0));
        ta.Start(Seconds(0.2));
        ta.Stop(Seconds(durS));
    }
    else if (appMode)
    {
        appRx = CreateObject<MortiseAppReceiver>();
        appRx->Setup(port, wkArr, wkSizes);
        staNodes.Get(0)->AddApplication(appRx);
        appRx->SetStartTime(Seconds(0.0));
        appRx->SetStopTime(Seconds(durS));

        Ptr<MortiseAppSender> appTx = CreateObject<MortiseAppSender>();
        appTx->Setup(InetSocketAddress(staIfaces.GetAddress(0), port), wkArr, wkSizes);
        serverNode.Get(0)->AddApplication(appTx);
        appTx->SetStartTime(Seconds(0.2));
        appTx->SetStopTime(Seconds(durS));

        Simulator::Schedule(Seconds(0.2) + MicroSeconds(1),
                            &ConfigureCc,
                            serverNode.Get(0)->GetId(),
                            cc,
                            jitterUs,
                            copaClockUs);
        Simulator::Schedule(Seconds(0.25), &ConnectSender, serverNode.Get(0)->GetId());
    }
    else
    {
        PacketSinkHelper sink("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sa = sink.Install(staNodes.Get(0));
        sa.Start(Seconds(0.0));
        bulkSink = DynamicCast<PacketSink>(sa.Get(0));

        BulkSendHelper bulk("ns3::TcpSocketFactory", InetSocketAddress(staIfaces.GetAddress(0), port));
        bulk.SetAttribute("MaxBytes", UintegerValue(0));
        ApplicationContainer ta = bulk.Install(serverNode.Get(0));
        ta.Start(Seconds(0.2));
        ta.Stop(Seconds(durS));

        Simulator::Schedule(Seconds(0.2) + MicroSeconds(1),
                            &ConfigureCc,
                            serverNode.Get(0)->GetId(),
                            cc,
                            jitterUs,
                            copaClockUs);
        Simulator::Schedule(Seconds(0.25), &ConnectSender, serverNode.Get(0)->GetId());
    }

    // Background competing Wi-Fi STAs (CSMA/CA contention)
    for (uint32_t i = 0; i < bgStas && bgMbpsEach > 0.0; i++)
    {
        uint16_t bport = 6000 + i;
        PacketSinkHelper bs("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), bport));
        ApplicationContainer bsa = bs.Install(staNodes.Get(1 + i));
        bsa.Start(Seconds(0.0));

        OnOffHelper bo("ns3::UdpSocketFactory", InetSocketAddress(staIfaces.GetAddress(1 + i), bport));
        bo.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        bo.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        bo.SetAttribute("DataRate", DataRateValue(DataRate(static_cast<uint64_t>(bgMbpsEach * 1e6))));
        bo.SetAttribute("PacketSize", UintegerValue(1200));
        ApplicationContainer bta = bo.Install(serverNode.Get(0));
        bta.Start(Seconds(0.3 + 0.05 * i));
        bta.Stop(Seconds(durS));
    }

    if (!appMode && bulkSink)
    {
        Simulator::Schedule(Seconds(warmupS), [&]() { rxAtWarmup = bulkSink->GetTotalRx(); });
    }

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    Simulator::Stop(Seconds(durS));
    Simulator::Run();

    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());
    uint64_t txPkts = 0, rxPkts = 0, lostPkts = 0;
    for (const auto& kv : monitor->GetFlowStats())
    {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(kv.first);
        if (t.destinationPort == port)
        {
            txPkts += kv.second.txPackets;
            rxPkts += kv.second.rxPackets;
            lostPkts += kv.second.lostPackets;
        }
    }

    const double measured = std::max(durS - (appMode ? 0.0 : warmupS), 1e-3);
    uint64_t rxBytes = appMode ? (appRx ? appRx->totalRx : 0ULL)
                               : (bulkSink ? (bulkSink->GetTotalRx() - rxAtWarmup) : 0ULL);
    double thrMbps = rxBytes * 8.0 / (measured * 1e6);
    double effCap = (capacityMbps > 1e-3) ? capacityMbps : thrMbps;
    double effBaseRtt = (baseRttMs > 1e-3) ? baseRttMs : (2.0 * serverDelayMs + 2.0);
    double minRtt = effBaseRtt;
    double rttAvg = effBaseRtt;
    if (!g_rttMs.empty())
    {
        minRtt = *std::min_element(g_rttMs.begin(), g_rttMs.end());
        double s = 0.0;
        for (double x : g_rttMs) s += x;
        rttAvg = s / g_rttMs.size();
        if (baseRttMs <= 1e-3)
        {
            effBaseRtt = minRtt;
        }
    }
    double qdAvg = std::max(rttAvg - effBaseRtt, 0.0);
    double qdP95 = std::max(Percentile(g_rttMs, 95) - effBaseRtt, 0.0);
    double dropRate = (txPkts > 0) ? static_cast<double>(lostPkts) / static_cast<double>(txPkts) : 0.0;
    double util = (effCap > 1e-6) ? std::min(thrMbps / effCap, 1.5) : 0.0;
    double estRtt = effBaseRtt + qdAvg;
    double utilPower = (estRtt > 1e-6) ? (util / (estRtt / 1000.0)) : 0.0;
    double normPower = utilPower * (effBaseRtt / 1000.0);
    double tputPower = (estRtt > 1e-6) ? (thrMbps / (estRtt / 1000.0)) : 0.0;

    std::ostringstream js;
    js << std::fixed << std::setprecision(6);
    js << "{\"duration_s\":" << measured
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
       << ",\"retrans_pkts\":" << g_retx
       << ",\"retrans_ratio\":" << ((rxPkts + g_retx > 0) ? (double)g_retx / (rxPkts + g_retx) : 0.0);
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
                uint32_t sz = (i < wkSizes.size()) ? wkSizes[i] : 1448U;
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
            p50Rct = Percentile(rcts, 50);
            p95Rct = Percentile(rcts, 95);
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
