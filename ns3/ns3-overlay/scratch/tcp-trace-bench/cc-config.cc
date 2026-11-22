#include "cc-config.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include <fstream>
using namespace ns3;
namespace tracebench {
extern std::ofstream g_copaEst;
/**
 * Algorithm name without the Copa option suffixes "+nosw" (default mode only,
 * no switch to the competitive mode) and "+gcv" (velocity rule of the authors'
 * implementation genericCC), e.g. "copa-auto+nosw+gcv" -> "copa-auto".
 *
 * @param cc algorithm name
 * @return name without suffixes
 */
std::string
BaseCc(const std::string& cc)
{
    return cc.substr(0, cc.find('+'));
}

bool
IsCopaKernelName(const std::string& name)
{
    const std::string cc = BaseCc(name);
    return cc == "copa" || cc == "copa_nc" || cc == "copa_orig_nc" ||
           cc == "copa-auto" || cc == "copa_auto" || cc == "copa_auto_nc" ||
           cc == "copa_auto_nc_j0" || cc == "copa_auto_nc_j3" || cc == "copa_orig" ||
           cc.rfind("copa-d", 0) == 0 || cc.rfind("copa_auto_theta", 0) == 0 ||
           cc.rfind("copa_auto_nc_theta", 0) == 0;
}

bool
IsAdaptiveCopaName(const std::string& name)
{
    const std::string cc = BaseCc(name);
    return cc == "copa-auto-v2" || cc == "copa_auto_v2" || cc == "copa-auto" || cc == "copa_auto" || cc == "copa_auto_nc" ||
           cc == "copa_auto_nc_j0" || cc == "copa_auto_nc_j3" ||
           cc.rfind("copa_auto_theta", 0) == 0 || cc.rfind("copa_auto_nc_theta", 0) == 0;
}

/**
 * Record the online estimates of copa-auto.
 */
void
CopaEstimateTracer(double jitter, double ratio, double rho, double baseRtt, double dqTarget, double delta)
{
    if (g_copaEst.is_open())
    {
        g_copaEst << Simulator::Now().GetSeconds() << " " << jitter * 1e3 << " " << ratio * 1e3
                  << " " << rho << " " << baseRtt * 1e3 << " " << dqTarget * 1e3 << " " << delta
                  << "\n";
    }
}

void
ConfigureCcFlow(uint32_t nodeId,
                uint32_t flow,
                std::string name,
                int32_t jitterUs,
                uint32_t copaClockUs,
                uint32_t debugMs)
{
    const std::string cc = BaseCc(name);
    if (cc == "mortise_copa")
    {
        const std::string path = "/NodeList/" + std::to_string(nodeId) +
                                 "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpMortiseCopa/";
        Config::Set(path + "CopaClockUs", UintegerValue(std::max<uint32_t>(copaClockUs, 1U)));
        return;
    }
    if (cc == "copa-auto-v2" || cc == "copa_auto_v2")
    {
        const std::string path = "/NodeList/" + std::to_string(nodeId) +
            "/$ns3::TcpL4Protocol/SocketList/0/CongestionOps/$ns3::TcpCopaAutoV2/";
        Config::Set(path + "CopaClockUs", UintegerValue(std::max<uint32_t>(copaClockUs, 1U)));
        Config::Set(path + "DebugMs", UintegerValue(debugMs));
        if (flow == 0 && g_copaEst.is_open())
            Config::ConnectWithoutContext(path + "Estimates", MakeCallback(&CopaEstimateTracer));
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
        initDeltaQ20 = 524288U; /* 0.50 * 2^20 */
    }
    else if (cc == "copa_nc" || cc == "copa_orig_nc")
    {
        adaptive = false;
        modeSwitching = false;
        initDeltaQ20 = 524288U; /* 0.50 * 2^20 */
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
    Config::Set(path + "DebugMs", UintegerValue(debugMs));
    if (adaptive && flow == 0 && g_copaEst.is_open())
    {
        Config::ConnectWithoutContext(path + "Estimates", MakeCallback(&CopaEstimateTracer));
    }
}

/**
 * Map a short algorithm name to its ns-3 TypeId name.
 */
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
    if (cc == "copa-auto-v2" || cc == "copa_auto_v2")
    {
        return "ns3::TcpCopaAutoV2";
    }
    if (IsCopaKernelName(name))
    {
        return "ns3::TcpCopaKernel";
    }
    // Allow full TypeId names for algorithms added later.
    TypeId tid;
    NS_ABORT_MSG_UNLESS(TypeId::LookupByNameFailSafe(cc, &tid),
                        "Unknown congestion control '" << cc << "'");
    return cc;
}

}
