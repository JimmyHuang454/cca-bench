/*
 * ns-3 CI benchmark runner for Copa-Auto and baseline TCP CCAs.
 * Shares the exact C state machine from kernel/tcp_copa_auto/copa_auto_core.h.
 *
 * Supports:
 *   - CCs: copa_auto, copa_auto_nc, copa_auto_nc_j0, copa_auto_nc_j3,
 *          copa_auto_theta<val>, copa_orig, mortise_copa,
 *          bbr, bbr2, bbr3, cubic, vegas, reno
 *   - Workloads: bulk (saturated) and app (Mortise <gap_ms>\t<size_bytes> .wk)
 *   - Traces: Mahimahi downlink delivery-ms traces (--downTrace) + optional
 *             LeoCC 10ms one-way delay traces (--delayTrace)
 *   - Output: single JSON summary (--outJson) compatible with mahimahi_eval.py
 */
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-statement-expression-from-macro-expansion"
#endif
#include "copa_auto_core.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "mortise_copa_ns3.h"

namespace ns3
{

static struct copa_probe_board g_ns3_copa_board = {};

class TcpCopaKernel : public TcpCongestionOps
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("ns3::TcpCopaKernel")
                .SetParent<TcpCongestionOps>()
                .SetGroupName("Internet")
                .AddConstructor<TcpCopaKernel>()
                .AddAttribute("Adaptive",
                              "Enable AutoDelta adaptive delta",
                              BooleanValue(true),
                              MakeBooleanAccessor(&TcpCopaKernel::m_adaptive),
                              MakeBooleanChecker())
                .AddAttribute("ModeSwitching",
                              "Enable Copa competitive mode switching",
                              BooleanValue(false),
                              MakeBooleanAccessor(&TcpCopaKernel::m_modeSwitching),
                              MakeBooleanChecker())
                .AddAttribute("JitterUs",
                              "Fixed jitter in us (-1 = auto probe, >= 0 = manual fixed j)",
                              IntegerValue(-1),
                              MakeIntegerAccessor(&TcpCopaKernel::m_jitterUs),
                              MakeIntegerChecker<int32_t>())
                .AddAttribute("ThetaMilli",
                              "Throughput weight theta*1000 (500 = 0.50)",
                              UintegerValue(500),
                              MakeUintegerAccessor(&TcpCopaKernel::m_thetaMilli),
                              MakeUintegerChecker<uint32_t>())
                .AddAttribute("InitDeltaQ20",
                              "Initial delta in Q20",
                              UintegerValue(COPA_DELTA_INIT_Q20),
                              MakeUintegerAccessor(&TcpCopaKernel::m_initDeltaQ20),
                              MakeUintegerChecker<uint32_t>());
        return tid;
    }

    TcpCopaKernel() = default;
    TcpCopaKernel(const TcpCopaKernel& o)
        : TcpCongestionOps(o),
          m_adaptive(o.m_adaptive),
          m_modeSwitching(o.m_modeSwitching),
          m_jitterUs(o.m_jitterUs),
          m_thetaMilli(o.m_thetaMilli),
          m_initDeltaQ20(o.m_initDeltaQ20),
          m_initialized(false)
    {
    }
    ~TcpCopaKernel() override = default;

    std::string GetName() const override
    {
        return "TcpCopaKernel";
    }

    bool HasCongControl() const override
    {
        return true;
    }

    Ptr<TcpCongestionOps> Fork() override
    {
        return CopyObject<TcpCopaKernel>(this);
    }

    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t /*bytesInFlight*/) override
    {
        return std::max<uint32_t>(tcb->m_cWnd.Get(), 2U * tcb->m_segmentSize);
    }

    void EnsureInit(Ptr<TcpSocketState> tcb)
    {
        if (m_initialized)
            return;
        uint32_t mss = std::max<uint32_t>(tcb->m_segmentSize, 536U);
        uint32_t initPkts = std::max<uint32_t>(tcb->m_cWnd.Get() / mss, 10U);
        struct copa_auto_cfg cfg = {};
        cfg.adaptive = m_adaptive;
        cfg.mode_switching = m_modeSwitching;
        cfg.jitter_us = m_jitterUs;
        cfg.theta_milli = m_thetaMilli;
        cfg.init_delta_q20 = m_initDeltaQ20;
        copa_state_init(&m_ca, cfg, &g_ns3_copa_board,
                        (u64)Simulator::Now().GetMicroSeconds(), initPkts);
        tcb->m_pacing = true;
        m_initialized = true;
    }

    void CongestionStateSet(Ptr<TcpSocketState> tcb,
                            const TcpSocketState::TcpCongState_t newState) override
    {
        EnsureInit(tcb);
        if (newState == TcpSocketState::CA_LOSS || newState == TcpSocketState::CA_RECOVERY)
        {
            u64 now_us = (u64)Simulator::Now().GetMicroSeconds();
            bool persistent = (newState == TcpSocketState::CA_LOSS);
            copa_on_loss(&m_ca, now_us, persistent);
            if (persistent || (!m_ca.competitive && m_ca.cfg.adaptive))
            {
                uint32_t mss = std::max<uint32_t>(tcb->m_segmentSize, 536U);
                u32 cwnd_pkts = std::max<u32>((u32)(m_ca.cwnd_q10 >> 10), COPA_MIN_CWND_PKTS);
                tcb->m_cWnd = cwnd_pkts * mss;
            }
        }
    }

    void CongControl(Ptr<TcpSocketState> tcb,
                     const TcpRateOps::TcpRateConnection& /*rc*/,
                     const TcpRateOps::TcpRateSample& rs) override
    {
        EnsureInit(tcb);
        u64 now_us = (u64)Simulator::Now().GetMicroSeconds();
        uint32_t mss = std::max<uint32_t>(tcb->m_segmentSize, 536U);

        Time lrtt = tcb->m_lastRtt.Get();
        Time srtt = tcb->m_srtt.Get();
        u64 srtt_us = (srtt.IsStrictlyPositive() && srtt != Time::Max())
                          ? (u64)srtt.GetMicroSeconds()
                          : 0ULL;
        u64 rtt_sample_us = lrtt.IsStrictlyPositive()
                                ? (u64)lrtt.GetMicroSeconds()
                                : (m_ca.cfg.adaptive ? 0ULL : srtt_us);
        if (!rtt_sample_us && !m_ca.cfg.adaptive)
            return;
        if (srtt_us == 0)
            srtt_us = rtt_sample_us;

        u32 acked_pkts = rs.m_ackedSacked > 0 ? std::max<u32>(rs.m_ackedSacked / mss, 1U) : 1U;
        u64 rate_bps = rs.m_deliveryRate.GetBitRate() / 8ULL;
        bool app_limited = rs.m_isAppLimited;
        bool low_flight = rs.m_isAppLimited && (rs.m_priorInFlight <= 4U * mss);

        if (rs.m_bytesLoss > 0)
            copa_on_loss(&m_ca, now_us, false);

        u32 new_cwnd = copa_on_ack(&m_ca, now_us, rtt_sample_us, srtt_us,
                                   acked_pkts, mss, rate_bps, app_limited, low_flight);
        new_cwnd = std::max<u32>(new_cwnd, COPA_MIN_CWND_PKTS);
        tcb->m_cWnd = new_cwnd * mss;

        u64 pacing_bps = copa_pacing_rate_bps(&m_ca, mss, now_us);
        if (pacing_bps > 0)
            tcb->m_pacingRate = DataRate(pacing_bps * 8ULL);
    }

  private:
    bool m_adaptive{true};
    bool m_modeSwitching{false};
    int32_t m_jitterUs{-1};
    uint32_t m_thetaMilli{500};
    uint32_t m_initDeltaQ20{COPA_DELTA_INIT_Q20};
    bool m_initialized{false};
    struct copa_auto_state m_ca{};
};

NS_OBJECT_ENSURE_REGISTERED(TcpCopaKernel);

struct QTracker
{
    double warmupS{5.0};
    std::queue<Time> enqTimes;
    std::vector<double> qdelaysMs;
    uint64_t deliveredBytes{0};
    uint64_t deliveredPkts{0};
    uint64_t droppedPkts{0};

    void OnEnqueue(Ptr<const Packet>)
    {
        enqTimes.push(Simulator::Now());
    }

    void OnDequeue(Ptr<const Packet> p)
    {
        if (enqTimes.empty())
            return;
        Time enq = enqTimes.front();
        enqTimes.pop();
        if (Simulator::Now().GetSeconds() >= warmupS)
        {
            qdelaysMs.push_back((Simulator::Now() - enq).GetSeconds() * 1000.0);
            deliveredBytes += p->GetSize();
            deliveredPkts++;
        }
    }

    void OnDrop(Ptr<const Packet>)
    {
        if (Simulator::Now().GetSeconds() >= warmupS)
            droppedPkts++;
    }
};

class AppWkServer : public Application
{
  public:
    AppWkServer(Address peer, std::vector<double> arrSec, std::vector<uint32_t> sizes)
        : m_peer(peer), m_arrSec(std::move(arrSec)), m_sizes(std::move(sizes))
    {
    }

    void StartApplication() override
    {
        m_sock = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        m_sock->SetSendCallback(MakeCallback(&AppWkServer::HandleSend, this));
        m_sock->Connect(m_peer);
        for (size_t i = 0; i < m_arrSec.size(); i++)
            Simulator::Schedule(Seconds(m_arrSec[i]), &AppWkServer::OnRequest, this, i);
    }

    void StopApplication() override
    {
        if (m_sock)
            m_sock->Close();
    }

  private:
    void OnRequest(size_t idx)
    {
        m_pendingBytes += m_sizes[idx];
        TrySend();
    }

    void HandleSend(Ptr<Socket>, uint32_t)
    {
        TrySend();
    }

    void TrySend()
    {
        while (m_pendingBytes > 0 && m_sock)
        {
            uint32_t avail = m_sock->GetTxAvailable();
            if (avail == 0)
                break;
            uint32_t chunk = std::min<uint32_t>(m_pendingBytes, std::min<uint32_t>(avail, 14480U));
            int sent = m_sock->Send(Create<Packet>(chunk));
            if (sent <= 0)
                break;
            m_pendingBytes -= (uint32_t)sent;
        }
    }

    Address m_peer;
    std::vector<double> m_arrSec;
    std::vector<uint32_t> m_sizes;
    Ptr<Socket> m_sock;
    uint32_t m_pendingBytes{0};
};

class AppWkClient : public Application
{
  public:
    AppWkClient(uint16_t port, std::vector<double> arrSec, std::vector<uint32_t> sizes)
        : m_port(port), m_arrSec(std::move(arrSec)), m_sizes(std::move(sizes))
    {
        m_doneSec.assign(m_sizes.size(), -1.0);
        uint64_t cum = 0;
        for (uint32_t s : m_sizes)
        {
            cum += s;
            m_cumBytes.push_back(cum);
        }
    }

    void StartApplication() override
    {
        m_listen = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        m_listen->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
        m_listen->Listen();
        m_listen->SetAcceptCallback(MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
                                    MakeCallback(&AppWkClient::HandleAccept, this));
    }

    void StopApplication() override
    {
        if (m_conn)
            m_conn->Close();
        if (m_listen)
            m_listen->Close();
    }

    const std::vector<double>& DoneSec() const
    {
        return m_doneSec;
    }

  private:
    void HandleAccept(Ptr<Socket> s, const Address&)
    {
        m_conn = s;
        m_conn->SetRecvCallback(MakeCallback(&AppWkClient::HandleRead, this));
    }

    void HandleRead(Ptr<Socket> s)
    {
        Ptr<Packet> p;
        while ((p = s->Recv()))
        {
            if (p->GetSize() == 0)
                break;
            m_totalRecv += p->GetSize();
            double nowS = Simulator::Now().GetSeconds();
            while (m_nextReq < m_cumBytes.size() && m_totalRecv >= m_cumBytes[m_nextReq])
            {
                m_doneSec[m_nextReq] = nowS;
                m_nextReq++;
            }
        }
    }

    uint16_t m_port;
    std::vector<double> m_arrSec;
    std::vector<uint32_t> m_sizes;
    std::vector<uint64_t> m_cumBytes;
    std::vector<double> m_doneSec;
    Ptr<Socket> m_listen;
    Ptr<Socket> m_conn;
    uint64_t m_totalRecv{0};
    size_t m_nextReq{0};
};

static double MortisePenalty(double r)
{
    if (r < 0.05) return 0.0;
    if (r < 0.10) return 4.0 * r - 0.2;
    if (r < 0.40) return r + 0.1;
    return 0.5;
}

static double PercentileSorted(const std::vector<double>& v, double p)
{
    if (v.empty())
        return 0.0;
    double k = (double)(v.size() - 1) * p;
    size_t f = (size_t)std::floor(k);
    size_t c = (size_t)std::ceil(k);
    if (f == c)
        return v[f];
    return v[f] + (k - f) * (v[c] - v[f]);
}

static std::vector<double> LoadMahimahiRatesMbps(const std::string& path, double binMs, double durS)
{
    std::ifstream f(path);
    std::vector<uint64_t> msList;
    uint64_t v;
    while (f >> v)
        msList.push_back(v);
    if (msList.empty())
        return {10.0};
    uint64_t periodMs = msList.back() + 1;
    size_t nBins = (size_t)std::ceil((durS * 1000.0) / binMs);
    std::vector<uint64_t> counts(nBins, 0);
    uint64_t off = 0;
    while (off < (uint64_t)(durS * 1000.0))
    {
        for (uint64_t t : msList)
        {
            uint64_t absMs = off + t;
            size_t b = (size_t)(absMs / binMs);
            if (b < nBins)
                counts[b]++;
        }
        off += periodMs;
    }
    std::vector<double> rates(nBins);
    for (size_t i = 0; i < nBins; i++)
    {
        double mbps = (double)counts[i] * 1500.0 * 8.0 / (binMs * 1000.0);
        rates[i] = std::max(mbps, 0.05); /* 50 kbps floor preserves real LTE scheduling stalls */
    }
    return rates;
}

static std::vector<double> LoadDelayTraceMs(const std::string& path)
{
    std::ifstream f(path);
    std::vector<double> vals;
    double v;
    while (f >> v)
        vals.push_back(std::max(v, 1.0));
    return vals;
}

static bool LoadWorkloadFile(const std::string& path,
                             std::vector<double>& arrSec,
                             std::vector<uint32_t>& sizes)
{
    std::ifstream f(path);
    if (!f.is_open())
        return false;
    std::string line;
    double tSec = 0.2; /* small offset after TCP connect */
    while (std::getline(f, line))
    {
        size_t pos = line.find_first_not_of(" \t\r\n");
        if (pos == std::string::npos || line[pos] == '#')
            continue;
        std::istringstream iss(line);
        double gapMs = 0.0;
        uint64_t sz = 0;
        if (!(iss >> gapMs >> sz) || sz == 0)
            continue;
        tSec += gapMs / 1000.0;
        arrSec.push_back(tSec);
        sizes.push_back((uint32_t)std::min<uint64_t>(sz, 100000000ULL));
    }
    return !arrSec.empty();
}

class FifoPointToPointChannel : public PointToPointChannel
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::FifoPointToPointChannel")
                                .SetParent<PointToPointChannel>()
                                .SetGroupName("PointToPoint")
                                .AddConstructor<FifoPointToPointChannel>();
        return tid;
    }

    bool TransmitStart(Ptr<const Packet> p, Ptr<PointToPointNetDevice> src, Time txTime) override
    {
        uint32_t wire = (src == GetSource(0)) ? 0 : 1;
        Time now = Simulator::Now();
        Time arrive = now + txTime + GetDelay();
        if (arrive < m_lastArrival[wire])
        {
            arrive = m_lastArrival[wire] + NanoSeconds(1);
        }
        m_lastArrival[wire] = arrive;
        Time relDelay = arrive - now;
        Simulator::ScheduleWithContext(GetDestination(wire)->GetNode()->GetId(),
                                       relDelay,
                                       &PointToPointNetDevice::Receive,
                                       GetDestination(wire),
                                       p->Copy());
        return true;
    }

  private:
    Time m_lastArrival[2] = {Seconds(0), Seconds(0)};
};

NS_OBJECT_ENSURE_REGISTERED(FifoPointToPointChannel);

static void SetBottleDelay(Ptr<PointToPointChannel> ch, double oneWayMs)
{
    ch->SetAttribute("Delay", TimeValue(MilliSeconds(std::max(oneWayMs, 0.5))));
}

int main(int argc, char* argv[])
{
    std::string cc = "copa_auto_nc";
    std::string mode = "bulk";       /* bulk | app */
    std::string downTrace = "";
    std::string upTrace = "";
    std::string delayTrace = "";
    std::string workloadFile = "";
    std::string outJson = "";
    double rateMbps = 48.0;
    double baseRttMs = 20.0;
    uint32_t queueBytes = 2000000;
    double durS = 60.0;
    double warmupS = 5.0;
    double binMs = 5.0;
    double clockGranularityMs = 1.0;
    int32_t jitterUs = -1;

    CommandLine cmd(__FILE__);
    cmd.AddValue("cc", "CCA name", cc);
    cmd.AddValue("mode", "bulk|app", mode);
    cmd.AddValue("downTrace", "Mahimahi downlink trace file", downTrace);
    cmd.AddValue("upTrace", "Optional Mahimahi uplink trace file", upTrace);
    cmd.AddValue("delayTrace", "Optional LeoCC 10ms one-way delay trace", delayTrace);
    cmd.AddValue("workloadFile", "Mortise <gap_ms> <size_bytes> .wk file", workloadFile);
    cmd.AddValue("outJson", "Optional output JSON path", outJson);
    cmd.AddValue("rateMbps", "Fallback constant rate Mbps when downTrace empty", rateMbps);
    cmd.AddValue("baseRttMs", "Base propagation RTT ms", baseRttMs);
    cmd.AddValue("queueBytes", "Bottleneck queue bytes", queueBytes);
    cmd.AddValue("durS", "Simulation duration s", durS);
    cmd.AddValue("warmupS", "Bulk warmup window s", warmupS);
    cmd.AddValue("binMs", "Mahimahi downlink trace rate-bin clock in ms (default 5.0)", binMs);
    cmd.AddValue("clockGranularityMs", "ns-3 TcpSocketBase::ClockGranularity in ms (default 1.0)", clockGranularityMs);
    cmd.AddValue("jitterUs", "Override jitter_us (-1=auto, >=0=manual)", jitterUs);
    cmd.Parse(argc, argv);

    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(16 * 1024 * 1024));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(16 * 1024 * 1024));
    Config::SetDefault("ns3::TcpSocket::InitialCwnd", UintegerValue(10));
    Config::SetDefault("ns3::TcpSocketBase::Sack", BooleanValue(true));
    Config::SetDefault("ns3::TcpSocketBase::ClockGranularity",
                       TimeValue(MilliSeconds(std::max(clockGranularityMs, 0.001))));
    Config::SetDefault("ns3::TcpL4Protocol::RecoveryType", TypeIdValue(TcpPrrRecovery::GetTypeId()));

    if (cc == "bbr")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpBbr::GetTypeId()));
        Config::SetDefault("ns3::TcpSocketState::EnablePacing", BooleanValue(true));
    }
    else if (cc == "bbr2")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpBbrV2::GetTypeId()));
        Config::SetDefault("ns3::TcpSocketState::EnablePacing", BooleanValue(true));
    }
    else if (cc == "bbr3")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpBbrV3::GetTypeId()));
        Config::SetDefault("ns3::TcpSocketState::EnablePacing", BooleanValue(true));
    }
    else if (cc == "cubic")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpCubic::GetTypeId()));
    }
    else if (cc == "vegas")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpVegas::GetTypeId()));
    }
    else if (cc == "veno")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpVeno::GetTypeId()));
    }
    else if (cc == "yeah")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpYeah::GetTypeId()));
    }
    else if (cc == "illinois")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpIllinois::GetTypeId()));
    }
    else if (cc == "westwood")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpWestwoodPlus::GetTypeId()));
    }
    else if (cc == "hybla")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpHybla::GetTypeId()));
    }
    else if (cc == "reno")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpLinuxReno::GetTypeId()));
    }
    else if (cc == "mortise_copa")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpMortiseCopa::GetTypeId()));
        Config::SetDefault("ns3::TcpSocketState::EnablePacing", BooleanValue(true));
    }
    else
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpCopaKernel::GetTypeId()));
        Config::SetDefault("ns3::TcpSocketState::EnablePacing", BooleanValue(true));
        if (cc == "copa_orig")
        {
            Config::SetDefault("ns3::TcpCopaKernel::Adaptive", BooleanValue(false));
            Config::SetDefault("ns3::TcpCopaKernel::ModeSwitching", BooleanValue(true));
            Config::SetDefault("ns3::TcpCopaKernel::InitDeltaQ20", UintegerValue(COPA_DELTA_MAX_Q20));
        }
        else if (cc == "copa_auto")
        {
            Config::SetDefault("ns3::TcpCopaKernel::Adaptive", BooleanValue(true));
            Config::SetDefault("ns3::TcpCopaKernel::ModeSwitching", BooleanValue(true));
            Config::SetDefault("ns3::TcpCopaKernel::JitterUs", IntegerValue(jitterUs));
        }
        else if (cc == "copa_auto_nc")
        {
            Config::SetDefault("ns3::TcpCopaKernel::Adaptive", BooleanValue(true));
            Config::SetDefault("ns3::TcpCopaKernel::ModeSwitching", BooleanValue(false));
            Config::SetDefault("ns3::TcpCopaKernel::JitterUs", IntegerValue(jitterUs));
        }
        else if (cc == "copa_auto_nc_j0")
        {
            Config::SetDefault("ns3::TcpCopaKernel::Adaptive", BooleanValue(true));
            Config::SetDefault("ns3::TcpCopaKernel::ModeSwitching", BooleanValue(false));
            Config::SetDefault("ns3::TcpCopaKernel::JitterUs", IntegerValue(0));
        }
        else if (cc == "copa_auto_nc_j3")
        {
            Config::SetDefault("ns3::TcpCopaKernel::Adaptive", BooleanValue(true));
            Config::SetDefault("ns3::TcpCopaKernel::ModeSwitching", BooleanValue(false));
            Config::SetDefault("ns3::TcpCopaKernel::JitterUs", IntegerValue(3000));
        }
        else if (cc.rfind("copa_auto_theta", 0) == 0)
        {
            double th = std::stod(cc.substr(15));
            uint32_t thMilli = (uint32_t)std::clamp<int>((int)std::round(th * 1000.0), 10, 995);
            Config::SetDefault("ns3::TcpCopaKernel::Adaptive", BooleanValue(true));
            Config::SetDefault("ns3::TcpCopaKernel::ModeSwitching", BooleanValue(true));
            Config::SetDefault("ns3::TcpCopaKernel::ThetaMilli", UintegerValue(thMilli));
            Config::SetDefault("ns3::TcpCopaKernel::JitterUs", IntegerValue(jitterUs));
        }
        else if (cc.rfind("copa_auto_nc_theta", 0) == 0)
        {
            double th = std::stod(cc.substr(18));
            uint32_t thMilli = (uint32_t)std::clamp<int>((int)std::round(th * 1000.0), 10, 995);
            Config::SetDefault("ns3::TcpCopaKernel::Adaptive", BooleanValue(true));
            Config::SetDefault("ns3::TcpCopaKernel::ModeSwitching", BooleanValue(false));
            Config::SetDefault("ns3::TcpCopaKernel::ThetaMilli", UintegerValue(thMilli));
            Config::SetDefault("ns3::TcpCopaKernel::JitterUs", IntegerValue(jitterUs));
        }
    }

    double accessOneWayMs = 0.5;
    double bottleOneWayMs = std::max(baseRttMs / 2.0 - accessOneWayMs, 0.5);

    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper access;
    access.SetDeviceAttribute("DataRate", StringValue("4Gbps"));
    access.SetChannelAttribute("Delay", TimeValue(MilliSeconds(accessOneWayMs)));
    access.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("4000p"));
    NetDeviceContainer d01 = access.Install(nodes.Get(0), nodes.Get(1));

    Ptr<PointToPointNetDevice> bDev = CreateObject<PointToPointNetDevice>();
    bDev->SetAddress(Mac48Address::Allocate());
    bDev->SetDataRate(DataRate((uint64_t)(rateMbps * 1e6)));
    Ptr<DropTailQueue<Packet>> bQueueObj = CreateObject<DropTailQueue<Packet>>();
    bQueueObj->SetMaxSize(QueueSize(std::to_string(queueBytes) + "B"));
    bDev->SetQueue(bQueueObj);
    nodes.Get(1)->AddDevice(bDev);

    Ptr<PointToPointNetDevice> uDev = CreateObject<PointToPointNetDevice>();
    uDev->SetAddress(Mac48Address::Allocate());
    uDev->SetDataRate(DataRate((uint64_t)(rateMbps * 1e6)));
    Ptr<DropTailQueue<Packet>> uQueueObj = CreateObject<DropTailQueue<Packet>>();
    uQueueObj->SetMaxSize(QueueSize(std::to_string(queueBytes) + "B"));
    uDev->SetQueue(uQueueObj);
    nodes.Get(2)->AddDevice(uDev);

    Ptr<FifoPointToPointChannel> bCh = CreateObject<FifoPointToPointChannel>();
    bCh->SetAttribute("Delay", TimeValue(MilliSeconds(bottleOneWayMs)));
    bDev->Attach(bCh);
    uDev->Attach(bCh);

    NetDeviceContainer d12;
    d12.Add(bDev);
    d12.Add(uDev);

    InternetStackHelper stack;
    stack.Install(nodes);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    ipv4.Assign(d01);
    ipv4.SetBase("10.1.2.0", "255.255.255.0");
    Ipv4InterfaceContainer i12 = ipv4.Assign(d12);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ptr<Queue<Packet>> bQueue = bDev->GetQueue();

    QTracker qt;
    qt.warmupS = (mode == "bulk") ? warmupS : 0.0;
    bQueue->TraceConnectWithoutContext("Enqueue", MakeCallback(&QTracker::OnEnqueue, &qt));
    bQueue->TraceConnectWithoutContext("Dequeue", MakeCallback(&QTracker::OnDequeue, &qt));
    bQueue->TraceConnectWithoutContext("Drop", MakeCallback(&QTracker::OnDrop, &qt));

    std::vector<double> arrSec;
    std::vector<uint32_t> sizes;
    double simEndS = durS;
    if (mode != "bulk")
    {
        if (!workloadFile.empty() && LoadWorkloadFile(workloadFile, arrSec, sizes))
        {
            simEndS = std::max(durS, arrSec.back() + 25.0);
        }
        else
        {
            uint32_t szPat[5] = {65000, 260000, 130000, 1200000, 460000};
            for (int i = 0; i < 55; i++)
            {
                arrSec.push_back(0.5 + i * 0.95 + ((i * 37) % 15) * 0.02);
                sizes.push_back(szPat[i % 5]);
            }
            simEndS = std::max(durS, arrSec.back() + 15.0);
        }
    }

    double meanCapMbps = rateMbps;
    if (!downTrace.empty())
    {
        double effBinMs = std::max(binMs, 1.0);
        auto rates = LoadMahimahiRatesMbps(downTrace, effBinMs, simEndS + 2.0);
        double sum = 0.0;
        int cnt = 0;
        bDev->SetDataRate(DataRate((uint64_t)(rates[0] * 1e6)));
        for (size_t i = 0; i < rates.size(); i++)
        {
            double tS = i * effBinMs / 1000.0;
            if (tS >= qt.warmupS && tS < simEndS)
            {
                sum += rates[i];
                cnt++;
            }
            if (i > 0 && std::fabs(rates[i] - rates[i - 1]) > 0.05)
            {
                Simulator::Schedule(Seconds(tS), &PointToPointNetDevice::SetDataRate, bDev,
                                    DataRate((uint64_t)(rates[i] * 1e6)));
            }
        }
        meanCapMbps = sum / std::max(cnt, 1);
    }

    if (!upTrace.empty())
    {
        double binMs = 10.0;
        auto upRates = LoadMahimahiRatesMbps(upTrace, binMs, simEndS + 2.0);
        uDev->SetDataRate(DataRate((uint64_t)(upRates[0] * 1e6)));
        for (size_t i = 1; i < upRates.size(); i++)
        {
            if (std::fabs(upRates[i] - upRates[i - 1]) > 0.05)
            {
                Simulator::Schedule(Seconds(i * binMs / 1000.0), &PointToPointNetDevice::SetDataRate,
                                    uDev, DataRate((uint64_t)(upRates[i] * 1e6)));
            }
        }
    }

    if (!delayTrace.empty())
    {
        auto delays = LoadDelayTraceMs(delayTrace);
        if (!delays.empty())
        {
            SetBottleDelay(bCh, std::max(delays[0] - accessOneWayMs, 0.5));
            size_t totalSteps = (size_t)std::ceil(simEndS * 100.0); /* 10ms per line */
            double prev = delays[0];
            for (size_t i = 1; i < totalSteps; i++)
            {
                double cur = delays[i % delays.size()];
                if (std::fabs(cur - prev) >= 0.5)
                {
                    Simulator::Schedule(Seconds(i * 0.01), &SetBottleDelay, bCh,
                                        std::max(cur - accessOneWayMs, 0.5));
                    prev = cur;
                }
            }
        }
    }

    uint16_t port = 5000;
    Ptr<AppWkClient> appClient;
    if (mode == "bulk")
    {
        PacketSinkHelper sink("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sinkApp = sink.Install(nodes.Get(2));
        sinkApp.Start(Seconds(0.0));
        sinkApp.Stop(Seconds(simEndS));

        BulkSendHelper src("ns3::TcpSocketFactory", InetSocketAddress(i12.GetAddress(1), port));
        src.SetAttribute("MaxBytes", UintegerValue(0));
        ApplicationContainer srcApp = src.Install(nodes.Get(0));
        srcApp.Start(Seconds(0.0));
        srcApp.Stop(Seconds(simEndS));
    }
    else
    {
        appClient = CreateObject<AppWkClient>(port, arrSec, sizes);
        nodes.Get(2)->AddApplication(appClient);
        appClient->SetStartTime(Seconds(0.0));
        appClient->SetStopTime(Seconds(simEndS));

        Ptr<AppWkServer> appServer =
            CreateObject<AppWkServer>(InetSocketAddress(i12.GetAddress(1), port), arrSec, sizes);
        nodes.Get(0)->AddApplication(appServer);
        appServer->SetStartTime(Seconds(0.05));
        appServer->SetStopTime(Seconds(simEndS));
    }

    Simulator::Stop(Seconds(simEndS));
    Simulator::Run();

    double avgQd = 0.0, p95Qd = 0.0;
    if (!qt.qdelaysMs.empty())
    {
        for (double d : qt.qdelaysMs)
            avgQd += d;
        avgQd /= qt.qdelaysMs.size();
        std::sort(qt.qdelaysMs.begin(), qt.qdelaysMs.end());
        p95Qd = PercentileSorted(qt.qdelaysMs, 0.95);
    }
    double drop = (qt.deliveredPkts + qt.droppedPkts) > 0
                      ? (double)qt.droppedPkts / (double)(qt.deliveredPkts + qt.droppedPkts)
                      : 0.0;
    double measDurS = std::max(simEndS - qt.warmupS, 0.001);
    double thrMbps = (double)qt.deliveredBytes * 8.0 / 1e6 / measDurS;
    double util = meanCapMbps > 0.0 ? thrMbps / meanCapMbps : 0.0;
    double estRttMs = baseRttMs + avgQd;
    double utilPower = estRttMs > 0.0 ? util / (estRttMs / 1000.0) : 0.0;
    double normPower = utilPower * (baseRttMs / 1000.0);
    double tputPower = estRttMs > 0.0 ? thrMbps / (estRttMs / 1000.0) : 0.0;

    size_t reqDone = 0;
    double meanRct = 0.0, p50Rct = 0.0, p95Rct = 0.0, fctSlowdown = 0.0, mortiseQoe = 0.0;
    if (mode != "bulk")
    {
        const auto& done = appClient->DoneSec();
        std::vector<double> rctsMs;
        double qoeSum = 0.0, rctSum = 0.0, logSdSum = 0.0;
        double penalty = MortisePenalty(drop);
        double capBytesPerMs = meanCapMbps * 1e6 / 8.0 / 1000.0;
        for (size_t i = 0; i < done.size(); i++)
        {
            if (done[i] > arrSec[i])
            {
                double rctMs = std::max((done[i] - arrSec[i]) * 1000.0, 1.0);
                double reqMBps = (sizes[i] / 1e6) / (rctMs / 1000.0);
                double idealMs = baseRttMs + (capBytesPerMs > 0.0 ? (sizes[i] / capBytesPerMs) : 0.0);
                double sd = std::max(rctMs / idealMs, 1.0);
                rctsMs.push_back(rctMs);
                rctSum += rctMs;
                logSdSum += std::log(sd);
                qoeSum += reqMBps * (1.0 - penalty);
            }
        }
        reqDone = rctsMs.size();
        if (!rctsMs.empty())
        {
            meanRct = rctSum / rctsMs.size();
            std::sort(rctsMs.begin(), rctsMs.end());
            p50Rct = PercentileSorted(rctsMs, 0.50);
            p95Rct = PercentileSorted(rctsMs, 0.95);
            fctSlowdown = std::exp(logSdSum / rctsMs.size());
            mortiseQoe = qoeSum / rctsMs.size();
        }
    }

    std::ostringstream js;
    js << std::fixed << std::setprecision(6) << "{"
       << "\"duration_s\":" << measDurS
       << ",\"capacity_mbps\":" << meanCapMbps
       << ",\"throughput_mbps\":" << thrMbps
       << ",\"utilization\":" << util
       << ",\"qdelay_avg_ms\":" << avgQd
       << ",\"qdelay_p95_ms\":" << p95Qd
       << ",\"base_rtt_ms\":" << baseRttMs
       << ",\"est_rtt_ms\":" << estRttMs
       << ",\"util_power\":" << utilPower
       << ",\"norm_power\":" << normPower
       << ",\"tput_power\":" << tputPower
       << ",\"drop_rate\":" << drop;
    if (mode != "bulk" && reqDone > 0)
    {
        js << ",\"requests\":" << reqDone
           << ",\"rct_mean_ms\":" << meanRct
           << ",\"rct_p50_ms\":" << p50Rct
           << ",\"rct_p95_ms\":" << p95Rct
           << ",\"retrans_pkts\":" << qt.droppedPkts
           << ",\"retrans_ratio\":" << drop
           << ",\"fct_slowdown\":" << fctSlowdown
           << ",\"mortise_qoe\":" << mortiseQoe;
    }
    js << "}\n";

    if (!outJson.empty())
    {
        std::ofstream of(outJson);
        of << js.str();
    }
    else
    {
        std::cout << js.str();
    }

    Simulator::Destroy();
    return 0;
}

} // namespace ns3

int main(int argc, char* argv[])
{
    return ns3::main(argc, argv);
}
