/*
 * Copyright (c) 2024
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Shared, command-line controllable parameters for the LTE mobility scenarios
 * (tcp-lte-highspeed-handover.cc: trains, cars and pedestrians).
 *
 * The defaults reproduce the original ("legacy") configuration of the scripts,
 * so old results stay reproducible. The recommended realistic baseline is
 * documented in scratch/README.md and used by scratch/run-sweep.py.
 */

#ifndef MOBILE_SCENARIO_PARAMS_H
#define MOBILE_SCENARIO_PARAMS_H

#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/lte-module.h"
#include "ns3/propagation-module.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ns3
{

/**
 * Radio, RLC, handover, core network, TCP and randomness parameters shared by
 * the LTE mobility scenarios.
 */
struct MobileScenarioParams
{
    uint32_t seed{1}; ///< RngSeedManager seed
    uint32_t run{1};  ///< RngSeedManager run number (independent replication)

    std::string pathloss{"friis"}; ///< friis|logdistance|cost231|rma|rma-los|umi|umi-los|uma|uma-los
    double enbHeight{-1.0};        ///< eNB antenna height [m], negative = model default
    double ueHeight{-1.0};         ///< UE antenna height [m], negative = model default
    double trackOffset{0.0};       ///< Perpendicular eNB distance from the track [m]
    std::string fading{"none"};    ///< none|epa3|eva60|etu3|etu5|etu12|eva300|rician300|<.fad path>
    double enbTxPowerDbm{46.0};    ///< eNB transmit power [dBm]
    double enbNoiseFigure{5.0};    ///< eNB noise figure [dB]
    uint32_t dlEarfcn{100};        ///< DL EARFCN (100 = 2120 MHz band 1, 1575 = 1842.5 MHz band 3)
    uint16_t bandwidthRb{25};      ///< DL/UL bandwidth in resource blocks (25 = 5 MHz)

    std::string rlcMode{"um"};    ///< um | am
    uint32_t rlcBuffer{10240};    ///< RLC transmission buffer [bytes]
    std::string scheduler{"pf"};  ///< pf | rr | pss | cqa | tdmt

    double hoHysteresis{3.0};  ///< A3 hysteresis [dB]
    double hoTttMs{256.0};     ///< A3 time-to-trigger [ms]
    double x2DelayMs{0.0};     ///< X2 link one-way delay [ms]
    double s1uDelayMs{0.0};    ///< S1-U link one-way delay [ms]
    double serverDelayMs{10.0}; ///< PGW <-> remote host one-way delay [ms]

    double minRtoMs{1000.0};  ///< TCP minimum RTO [ms] (ns-3 default 1 s, Linux 200 ms)
    uint32_t delAckCount{2};  ///< TCP delayed ACK count
    double delAckTimeoutMs{200.0}; ///< TCP delayed ACK timeout [ms] (ns-3 200, Linux 40)
    uint32_t initialCwnd{10}; ///< TCP initial congestion window [segments]
    bool freshLoss{false};    ///< Rate samples report only freshly lost data
    uint32_t dataRetries{6};  ///< TCP data retransmissions before giving up

    /**
     * Register all parameters on the command line.
     *
     * @param cmd command line
     */
    void AddToCommandLine(CommandLine& cmd)
    {
        cmd.AddValue("seed", "RNG seed", seed);
        cmd.AddValue("run", "RNG run number (use 1..N for independent replications)", run);
        cmd.AddValue("pathloss",
                     "Pathloss model: friis|logdistance|cost231|rma|rma-los|umi|umi-los|uma|uma-los",
                     pathloss);
        cmd.AddValue("enbHeight", "eNB antenna height [m], <0 for the model default", enbHeight);
        cmd.AddValue("ueHeight", "UE antenna height [m], <0 for the model default", ueHeight);
        cmd.AddValue("trackOffset", "Perpendicular distance of the eNBs from the track [m]",
                     trackOffset);
        cmd.AddValue("fading",
                     "Fast fading: none|epa3|eva60|etu3|etu5|etu12|eva300|rician300|<path to .fad "
                     "trace>",
                     fading);
        cmd.AddValue("enbTxPowerDbm", "eNB TX power [dBm]", enbTxPowerDbm);
        cmd.AddValue("enbNoiseFigure", "eNB noise figure [dB]", enbNoiseFigure);
        cmd.AddValue("earfcn", "DL EARFCN (UL = DL + 18000); 100 = band 1, 1575 = band 3", dlEarfcn);
        cmd.AddValue("bandwidth", "DL/UL bandwidth in RBs: 6|15|25|50|75|100", bandwidthRb);
        cmd.AddValue("rlcMode", "RLC mode of the data bearer: um|am", rlcMode);
        cmd.AddValue("rlcBuffer", "RLC TX buffer size [bytes]", rlcBuffer);
        cmd.AddValue("scheduler", "MAC scheduler: pf|rr|pss|cqa|tdmt", scheduler);
        cmd.AddValue("hoHysteresis", "A3 handover hysteresis [dB]", hoHysteresis);
        cmd.AddValue("hoTtt", "A3 handover time-to-trigger [ms]", hoTttMs);
        cmd.AddValue("x2Delay",
                     "X2 link one-way delay [ms]. Values > 0 can abort the ns-3 LTE model "
                     "(late X2-U forwarded data), keep 0 unless you know it is safe",
                     x2DelayMs);
        cmd.AddValue("s1uDelay", "S1-U link one-way delay [ms]", s1uDelayMs);
        cmd.AddValue("serverDelay", "PGW <-> server one-way delay [ms]", serverDelayMs);
        cmd.AddValue("minRto", "TCP minimum RTO [ms]", minRtoMs);
        cmd.AddValue("delAckCount", "TCP delayed ACK count", delAckCount);
        cmd.AddValue("delAckTimeout",
                     "TCP delayed ACK timeout [ms] (ns-3 default 200, Linux minimum 40)",
                     delAckTimeoutMs);
        cmd.AddValue("initialCwnd", "TCP initial cwnd [segments]", initialCwnd);
        cmd.AddValue("freshLoss",
                     "Rate samples report only freshly lost data "
                     "(TcpSocketBase::FreshLossAccounting)",
                     freshLoss);
        cmd.AddValue("dataRetries", "TCP data retransmissions before giving up", dataRetries);
    }

    /**
     * Apply the global defaults (RNG, RLC, TCP). Call right after parsing.
     */
    void ApplyDefaults() const
    {
        RngSeedManager::SetSeed(seed);
        RngSeedManager::SetRun(run);

        NS_ABORT_MSG_UNLESS(rlcMode == "um" || rlcMode == "am", "rlcMode must be um or am");
        Config::SetDefault("ns3::LteEnbRrc::EpsBearerToRlcMapping",
                           EnumValue(rlcMode == "am" ? LteEnbRrc::RLC_AM_ALWAYS
                                                     : LteEnbRrc::RLC_UM_ALWAYS));
        Config::SetDefault("ns3::LteRlcUm::MaxTxBufferSize", UintegerValue(rlcBuffer));
        Config::SetDefault("ns3::LteRlcAm::MaxTxBufferSize", UintegerValue(rlcBuffer));

        Config::SetDefault("ns3::LteEnbPhy::TxPower", DoubleValue(enbTxPowerDbm));
        Config::SetDefault("ns3::LteEnbPhy::NoiseFigure", DoubleValue(enbNoiseFigure));

        Config::SetDefault("ns3::TcpSocketBase::MinRto", TimeValue(MilliSeconds(minRtoMs)));
        Config::SetDefault("ns3::TcpSocket::DelAckCount", UintegerValue(delAckCount));
        Config::SetDefault("ns3::TcpSocket::DelAckTimeout",
                           TimeValue(MilliSeconds(delAckTimeoutMs)));
        Config::SetDefault("ns3::TcpSocket::InitialCwnd", UintegerValue(initialCwnd));
        Config::SetDefault("ns3::TcpSocketBase::FreshLossAccounting", BooleanValue(freshLoss));
        Config::SetDefault("ns3::TcpSocket::DataRetries", UintegerValue(dataRetries));
    }

    /**
     * Configure the LTE and EPC helpers. Must be called before installing any
     * eNB device.
     *
     * @param lte LTE helper
     * @param epc EPC helper
     */
    void ConfigureLte(Ptr<LteHelper> lte, Ptr<PointToPointEpcHelper> epc) const
    {
        lte->SetEnbDeviceAttribute("DlBandwidth", UintegerValue(bandwidthRb));
        lte->SetEnbDeviceAttribute("UlBandwidth", UintegerValue(bandwidthRb));
        lte->SetEnbDeviceAttribute("DlEarfcn", UintegerValue(dlEarfcn));
        lte->SetEnbDeviceAttribute("UlEarfcn", UintegerValue(dlEarfcn + 18000));
        lte->SetUeDeviceAttribute("DlEarfcn", UintegerValue(dlEarfcn));

        lte->SetHandoverAlgorithmType("ns3::A3RsrpHandoverAlgorithm");
        lte->SetHandoverAlgorithmAttribute("Hysteresis", DoubleValue(hoHysteresis));
        lte->SetHandoverAlgorithmAttribute("TimeToTrigger", TimeValue(MilliSeconds(hoTttMs)));

        epc->SetAttribute("X2LinkDelay", TimeValue(MilliSeconds(x2DelayMs)));
        epc->SetAttribute("S1uLinkDelay", TimeValue(MilliSeconds(s1uDelayMs)));

        lte->SetSchedulerType(SchedulerTypeName());

        if (pathloss == "friis")
        {
            lte->SetAttribute("PathlossModel", StringValue("ns3::FriisPropagationLossModel"));
        }
        else if (pathloss == "logdistance")
        {
            lte->SetAttribute("PathlossModel", StringValue("ns3::LogDistancePropagationLossModel"));
            lte->SetPathlossModelAttribute("Exponent", DoubleValue(3.5));
            lte->SetPathlossModelAttribute("ReferenceLoss", DoubleValue(39.0));
        }
        else if (pathloss == "cost231")
        {
            lte->SetAttribute("PathlossModel", StringValue("ns3::Cost231PropagationLossModel"));
            lte->SetPathlossModelAttribute("BSAntennaHeight", DoubleValue(EnbZ()));
            lte->SetPathlossModelAttribute("SSAntennaHeight", DoubleValue(UeZ()));
        }
        else if (pathloss == "rma" || pathloss == "rma-los")
        {
            lte->SetAttribute("PathlossModel",
                              StringValue("ns3::ThreeGppRmaPropagationLossModel"));
            Ptr<ChannelConditionModel> condition;
            if (pathloss == "rma-los")
            {
                condition = CreateObject<AlwaysLosChannelConditionModel>();
            }
            else
            {
                condition = CreateObject<ThreeGppRmaChannelConditionModel>();
                condition->SetAttribute("UpdatePeriod", TimeValue(Seconds(1)));
            }
            lte->SetPathlossModelAttribute("ChannelConditionModel", PointerValue(condition));
        }
        else if (pathloss == "umi" || pathloss == "umi-los" || pathloss == "uma" ||
                 pathloss == "uma-los")
        {
            // Urban street canyon micro cells (UMi) or urban macro cells (UMa),
            // 3GPP TR 38.901, for pedestrians in a city. Without -los the
            // LOS/NLOS state follows the 3GPP probability and is redrawn every
            // 20 s: about the 50 m decorrelation distance of the LOS state
            // (TR 38.901 Table 7.6.3.1-2) at walking or running speed.
            const bool umi = pathloss.rfind("umi", 0) == 0;
            lte->SetAttribute("PathlossModel",
                              StringValue(umi ? "ns3::ThreeGppUmiStreetCanyonPropagationLossModel"
                                              : "ns3::ThreeGppUmaPropagationLossModel"));
            Ptr<ChannelConditionModel> condition;
            if (pathloss.size() > 3 && pathloss.substr(3) == "-los")
            {
                condition = CreateObject<AlwaysLosChannelConditionModel>();
            }
            else
            {
                if (umi)
                {
                    condition = CreateObject<ThreeGppUmiStreetCanyonChannelConditionModel>();
                }
                else
                {
                    condition = CreateObject<ThreeGppUmaChannelConditionModel>();
                }
                condition->SetAttribute("UpdatePeriod", TimeValue(Seconds(20)));
            }
            lte->SetPathlossModelAttribute("ChannelConditionModel", PointerValue(condition));
        }
        else
        {
            NS_FATAL_ERROR("Unknown pathloss model '" << pathloss << "'");
        }

        if (fading != "none")
        {
            lte->SetFadingModel("ns3::TraceFadingLossModel");
            lte->SetFadingModelAttribute("TraceFilename", StringValue(FadingTracePath()));
            lte->SetFadingModelAttribute("TraceLength", TimeValue(Seconds(10)));
            lte->SetFadingModelAttribute("SamplesNum", UintegerValue(10000));
            lte->SetFadingModelAttribute("WindowSize", TimeValue(Seconds(0.5)));
            lte->SetFadingModelAttribute("RbNum", UintegerValue(100));
        }
    }

    /** @return eNB antenna height actually used [m] */
    double EnbZ() const
    {
        if (enbHeight >= 0)
        {
            return enbHeight;
        }
        return (pathloss == "friis" || pathloss == "logdistance") ? 0.0 : 35.0;
    }

    /** @return UE antenna height actually used [m] */
    double UeZ() const
    {
        if (ueHeight >= 0)
        {
            return ueHeight;
        }
        return (pathloss == "friis" || pathloss == "logdistance") ? 0.0 : 1.5;
    }

    /** @return ns-3 TypeId name of the selected MAC scheduler */
    std::string SchedulerTypeName() const
    {
        if (scheduler == "pf")
        {
            return "ns3::PfFfMacScheduler";
        }
        if (scheduler == "rr")
        {
            return "ns3::RrFfMacScheduler";
        }
        if (scheduler == "pss")
        {
            return "ns3::PssFfMacScheduler";
        }
        if (scheduler == "cqa")
        {
            return "ns3::CqaFfMacScheduler";
        }
        if (scheduler == "tdmt")
        {
            return "ns3::TdMtFfMacScheduler";
        }
        NS_FATAL_ERROR("Unknown scheduler '" << scheduler << "'");
        return "";
    }

    /**
     * Resolve --fading to an existing trace file. Keywords map to the traces
     * shipped with the LTE module (epa3, eva60, etu3) or generated by
     * scratch/gen-fading-trace.py (etu5 / etu12 = ETU at walking / running
     * speed, eva300, rician300 = EVA 300 km/h with a K = 6 dB direct path);
     * other values are treated as a path. Relative
     * paths are searched from the current directory and its parents, so the
     * scripts work when launched from the ns-3 root or from an output folder.
     *
     * @return path of the trace file
     */
    std::string FadingTracePath() const
    {
        std::string rel = fading;
        if (fading == "epa3")
        {
            rel = "src/lte/model/fading-traces/fading_trace_EPA_3kmph.fad";
        }
        else if (fading == "eva60")
        {
            rel = "src/lte/model/fading-traces/fading_trace_EVA_60kmph.fad";
        }
        else if (fading == "etu3")
        {
            rel = "src/lte/model/fading-traces/fading_trace_ETU_3kmph.fad";
        }
        else if (fading == "etu5")
        {
            rel = "scratch/fading-traces/fading_trace_ETU_5kmph.fad";
        }
        else if (fading == "etu12")
        {
            rel = "scratch/fading-traces/fading_trace_ETU_12kmph.fad";
        }
        else if (fading == "eva300")
        {
            rel = "scratch/fading-traces/fading_trace_EVA_300kmph.fad";
        }
        else if (fading == "rician300")
        {
            rel = "scratch/fading-traces/fading_trace_EVA_300kmph_K6dB.fad";
        }
        std::vector<std::string> tried;
        for (const std::string up : {"", "../", "../../", "../../../"})
        {
            std::string candidate = (!rel.empty() && rel[0] == '/') ? rel : up + rel;
            tried.push_back(candidate);
            if (std::ifstream(candidate).good())
            {
                return candidate;
            }
            if (!rel.empty() && rel[0] == '/')
            {
                break;
            }
        }
        std::ostringstream oss;
        for (const auto& t : tried)
        {
            oss << " " << t;
        }
        NS_FATAL_ERROR("Fading trace not found, tried:" << oss.str());
        return "";
    }

    /** @return one-line summary of the configuration */
    std::string Describe() const
    {
        std::ostringstream oss;
        oss << "seed=" << seed << " run=" << run << " pathloss=" << pathloss << " h=" << EnbZ()
            << "/" << UeZ() << "m offset=" << trackOffset << "m fading=" << fading
            << " earfcn=" << dlEarfcn << " bw=" << bandwidthRb << "RB rlc=" << rlcMode << "/" << rlcBuffer
            << "B sched=" << scheduler << " hys=" << hoHysteresis << "dB ttt=" << hoTttMs
            << "ms x2=" << x2DelayMs << "ms s1u=" << s1uDelayMs << "ms server=" << serverDelayMs
            << "ms minRto=" << minRtoMs << "ms delAck=" << delAckTimeoutMs << "ms";
        return oss.str();
    }
};

} // namespace ns3

#endif // MOBILE_SCENARIO_PARAMS_H
