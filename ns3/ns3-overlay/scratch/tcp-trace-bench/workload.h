#pragma once
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>
using namespace ns3;
namespace tracebench {
class MortiseAppSender : public Application
{
  public:
    void Setup(Address peer, std::vector<double> arrSec, std::vector<uint32_t> sizes)
    {
        m_peer = peer;
        m_arrSec = std::move(arrSec);
        m_sizes = std::move(sizes);
    }

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        m_socket->SetSendCallback(MakeCallback(&MortiseAppSender::OnSendSpace, this));
        m_socket->Connect(m_peer);
        for (size_t i = 0; i < m_arrSec.size(); i++)
        {
            Simulator::Schedule(Seconds(m_arrSec[i] - 0.001), &MortiseAppSender::QueueRequest, this, m_sizes[i]);
        }
    }

    void StopApplication() override
    {
        if (m_socket)
        {
            m_socket->Close();
        }
    }

    void QueueRequest(uint32_t bytes)
    {
        m_pending += bytes;
        Pump();
    }

    void OnSendSpace(Ptr<Socket>, uint32_t)
    {
        Pump();
    }

    void Pump()
    {
        if (!m_socket)
        {
            return;
        }
        while (m_pending > 0)
        {
            uint32_t avail = m_socket->GetTxAvailable();
            if (avail == 0)
            {
                break;
            }
            uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(m_pending, std::min<uint32_t>(avail, 65536U)));
            int sent = m_socket->Send(Create<Packet>(chunk));
            if (sent <= 0)
            {
                break;
            }
            m_pending -= static_cast<uint64_t>(sent);
        }
    }

    Address m_peer;
    Ptr<Socket> m_socket;
    std::vector<double> m_arrSec;
    std::vector<uint32_t> m_sizes;
    uint64_t m_pending{0};
};

class MortiseAppReceiver : public Application
{
  public:
    std::vector<double> rctsMs;
    uint64_t totalRx{0};

    void Setup(uint16_t port, std::vector<double> arrSec, std::vector<uint32_t> sizes)
    {
        m_port = port;
        m_arrSec = std::move(arrSec);
        m_sizes = std::move(sizes);
    }

  private:
    void StartApplication() override
    {
        m_listen = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        m_listen->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
        m_listen->Listen();
        m_listen->SetAcceptCallback(MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
                                    MakeCallback(&MortiseAppReceiver::OnAccept, this));
    }

    void OnAccept(Ptr<Socket> s, const Address&)
    {
        m_conn = s;
        m_conn->SetRecvCallback(MakeCallback(&MortiseAppReceiver::OnRead, this));
    }

    void OnRead(Ptr<Socket> s)
    {
        Ptr<Packet> p;
        while ((p = s->Recv()) && p->GetSize() > 0)
        {
            totalRx += p->GetSize();
            m_accum += p->GetSize();
            double nowS = Simulator::Now().GetSeconds();
            while (m_idx < m_sizes.size() && m_accum >= m_sizes[m_idx])
            {
                m_accum -= m_sizes[m_idx];
                double rctMs = std::max((nowS - m_arrSec[m_idx]) * 1000.0, 0.1);
                rctsMs.push_back(rctMs);
                m_idx++;
                if (m_idx == m_sizes.size())
                {
                    Simulator::Stop();
                    return;
                }
            }
        }
    }

    uint16_t m_port{5000};
    Ptr<Socket> m_listen;
    Ptr<Socket> m_conn;
    std::vector<double> m_arrSec;
    std::vector<uint32_t> m_sizes;
    uint64_t m_accum{0};
    size_t m_idx{0};
};

inline bool
ParseWorkloadSizeBytes(const std::string& raw, uint64_t& outBytes)
{
    const auto first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return false;
    }
    const auto last = raw.find_last_not_of(" \t\r\n");
    const std::string s = raw.substr(first, last - first + 1);
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string numStr = s;
    double mult = 1.0;
    bool hasUnit = false;
    if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "mb") == 0)
    {
        numStr = s.substr(0, s.size() - 2);
        mult = 1024.0 * 1024.0;
        hasUnit = true;
    }
    else if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "kb") == 0)
    {
        numStr = s.substr(0, s.size() - 2);
        mult = 1024.0;
        hasUnit = true;
    }
    else if (lower.size() > 1 && lower.back() == 'm')
    {
        numStr = s.substr(0, s.size() - 1);
        mult = 1024.0 * 1024.0;
        hasUnit = true;
    }
    else if (lower.size() > 1 && lower.back() == 'k')
    {
        numStr = s.substr(0, s.size() - 1);
        mult = 1024.0;
        hasUnit = true;
    }
    else if (lower.size() > 1 && lower.back() == 'b')
    {
        numStr = s.substr(0, s.size() - 1);
        mult = 1.0;
        hasUnit = true;
    }

    if (numStr.empty() || std::isspace(static_cast<unsigned char>(numStr.back())))
    {
        return false;
    }
    if (!hasUnit)
    {
        if (numStr.find_first_not_of("0123456789") != std::string::npos)
        {
            return false;
        }
        std::istringstream iss(numStr);
        int64_t val = 0;
        if (!(iss >> val) || !iss.eof() || val <= 0 || val > 100000000)
        {
            return false;
        }
        outBytes = static_cast<uint64_t>(val);
        return true;
    }
    std::istringstream iss(numStr);
    double val = 0.0;
    if (!(iss >> val) || !iss.eof() || !std::isfinite(val) || val <= 0.0)
    {
        return false;
    }
    const double scaled = std::round(val * mult);
    if (!std::isfinite(scaled) || scaled < 1.0 || scaled > 100000000.0)
    {
        return false;
    }
    outBytes = static_cast<uint64_t>(scaled);
    return true;
}

bool
LoadWorkloadFile(const std::string& path,
                 std::vector<double>& arrSec,
                 std::vector<uint32_t>& sizes,
                 double startOffsetS)
{
    std::ifstream f(path);
    if (!f)
    {
        return false;
    }
    double tSec = startOffsetS;
    double prevAbsMs = -1.0;
    int mode = 0; // 0=unset, 1=csv (<t_ms>,<size>), 2=legacy (<gap_ms> <size_bytes>)
    std::string line;
    while (std::getline(f, line))
    {
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#')
        {
            continue;
        }
        const auto last = line.find_last_not_of(" \t\r\n");
        const std::string trimmed = line.substr(first, last - first + 1);
        const auto commaPos = trimmed.find(',');
        const int lineMode = (commaPos != std::string::npos) ? 1 : 2;
        if (mode == 0)
        {
            mode = lineMode;
        }
        else if (mode != lineMode)
        {
            return false;
        }
        if (mode == 1)
        {
            if (trimmed.find(',', commaPos + 1) != std::string::npos)
            {
                return false;
            }
            const std::string tRaw = trimmed.substr(0, commaPos);
            const auto tFirst = tRaw.find_first_not_of(" \t\r\n");
            if (tFirst == std::string::npos)
            {
                return false;
            }
            const auto tLast = tRaw.find_last_not_of(" \t\r\n");
            std::istringstream ts(tRaw.substr(tFirst, tLast - tFirst + 1));
            double absMs = 0.0;
            uint64_t sz = 0;
            ts >> absMs;
            if (ts.fail() || !ts.eof() || !std::isfinite(absMs) || absMs < 0.0 ||
                (prevAbsMs >= 0.0 && absMs < prevAbsMs) ||
                !ParseWorkloadSizeBytes(trimmed.substr(commaPos + 1), sz) || sz == 0)
            {
                return false;
            }
            prevAbsMs = absMs;
            arrSec.push_back(startOffsetS + absMs / 1000.0);
            sizes.push_back(static_cast<uint32_t>(std::min<uint64_t>(sz, 100000000ULL)));
        }
        else
        {
            std::istringstream iss(trimmed);
            double gapMs = 0.0;
            std::string szStr;
            std::string extra;
            uint64_t sz = 0;
            if (!(iss >> gapMs >> szStr) || (iss >> extra) ||
                !std::isfinite(gapMs) || gapMs < 0.0 ||
                !ParseWorkloadSizeBytes(szStr, sz) || sz == 0)
            {
                continue;
            }
            tSec += gapMs / 1000.0;
            arrSec.push_back(tSec);
            sizes.push_back(static_cast<uint32_t>(std::min<uint64_t>(sz, 100000000ULL)));
        }
    }
    return !arrSec.empty();
}

}
