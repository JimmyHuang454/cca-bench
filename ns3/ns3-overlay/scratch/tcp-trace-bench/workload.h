#pragma once
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
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
            }
            if (m_idx == m_sizes.size())
            {
                Simulator::Stop();
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
ParseWorkloadCsvRow(const std::string& rawLine, std::vector<std::string>& fields)
{
    fields.clear();
    std::string line = rawLine;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t i = 0;
    const size_t n = line.size();
    while (true)
    {
        std::string field;
        if (i < n && line[i] == '"')
        {
            ++i;
            bool closed = false;
            while (i < n)
            {
                if (line[i] == '"')
                {
                    if (i + 1 < n && line[i + 1] == '"')
                    {
                        field.push_back('"');
                        i += 2;
                    }
                    else
                    {
                        ++i;
                        closed = true;
                        break;
                    }
                }
                else
                {
                    field.push_back(line[i++]);
                }
            }
            if (!closed) return false;
            while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i < n && line[i] != ',') return false;
        }
        else
        {
            while (i < n && line[i] != ',')
            {
                if (line[i] == '"') return false;
                field.push_back(line[i++]);
            }
        }
        fields.push_back(std::move(field));
        if (i == n) break;
        ++i;
    }
    return true;
}

inline bool
ParseWorkloadTimeMs(const std::string& raw, double& outMs)
{
    const auto first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    const auto last = raw.find_last_not_of(" \t\r\n");
    const std::string s = raw.substr(first, last - first + 1);
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string numStr = s;
    double mult = 1.0;
    if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "ms") == 0)
    {
        numStr = s.substr(0, s.size() - 2);
        mult = 1.0;
    }
    else if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "us") == 0)
    {
        numStr = s.substr(0, s.size() - 2);
        mult = 1e-3;
    }
    else if (lower.size() > 1 && lower.back() == 's')
    {
        numStr = s.substr(0, s.size() - 1);
        mult = 1000.0;
    }
    if (numStr.empty() || std::isspace(static_cast<unsigned char>(numStr.back()))) return false;
    std::istringstream iss(numStr);
    double val = 0.0;
    if (!(iss >> val) || !iss.eof() || !std::isfinite(val) || val < 0.0) return false;
    const double ms = val * mult;
    if (!std::isfinite(ms)) return false;
    outMs = ms;
    return true;
}

inline bool
ParseWorkloadSizeBytes(const std::string& raw, uint64_t& outBytes)
{
    const auto first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
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
    if (numStr.empty() || std::isspace(static_cast<unsigned char>(numStr.back()))) return false;
    if (!hasUnit)
    {
        if (numStr.find_first_not_of("0123456789") != std::string::npos) return false;
        std::istringstream iss(numStr);
        int64_t val = 0;
        if (!(iss >> val) || !iss.eof() || val <= 0 || val > 100000000) return false;
        outBytes = static_cast<uint64_t>(val);
        return true;
    }
    std::istringstream iss(numStr);
    double val = 0.0;
    if (!(iss >> val) || !iss.eof() || !std::isfinite(val) || val <= 0.0) return false;
    const double scaled = std::round(val * mult);
    if (!std::isfinite(scaled) || scaled < 1.0 || scaled > 100000000.0) return false;
    outBytes = static_cast<uint64_t>(scaled);
    return true;
}

inline bool
LoadWorkloadFile(const std::string& path,
                 std::vector<double>& arrSec,
                 std::vector<uint32_t>& sizes,
                 double startOffsetS)
{
    std::ifstream f(path);
    if (!f) return false;
    static const std::set<std::string> kAllowed = {"time", "size", "comment"};
    std::map<std::string, size_t> cols;
    size_t ncols = 0, tIdx = 0, sIdx = 0;
    double prevMs = -1.0;
    std::string raw;
    std::vector<std::string> fields;
    unsigned lineNo = 0;
    while (std::getline(f, raw))
    {
        ++lineNo;
        if (lineNo == 1 && raw.size() >= 3 &&
            static_cast<unsigned char>(raw[0]) == 0xEF &&
            static_cast<unsigned char>(raw[1]) == 0xBB &&
            static_cast<unsigned char>(raw[2]) == 0xBF)
        {
            raw.erase(0, 3);
        }
        const auto first = raw.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || raw[first] == '#') continue;
        if (!ParseWorkloadCsvRow(raw, fields)) return false;
        if (cols.empty())
        {
            ncols = fields.size();
            for (size_t i = 0; i < ncols; ++i)
            {
                const auto f0 = fields[i].find_first_not_of(" \t\r\n");
                if (f0 == std::string::npos) return false;
                const auto f1 = fields[i].find_last_not_of(" \t\r\n");
                std::string name = fields[i].substr(f0, f1 - f0 + 1);
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!kAllowed.count(name) || cols.count(name)) return false;
                cols[name] = i;
            }
            if (!cols.count("time") || !cols.count("size")) return false;
            tIdx = cols["time"];
            sIdx = cols["size"];
            continue;
        }
        if (fields.size() > ncols) return false;
        if (fields.size() < ncols) fields.resize(ncols);
        double absMs = 0.0;
        uint64_t sz = 0;
        if (!ParseWorkloadTimeMs(fields[tIdx], absMs) ||
            (prevMs >= 0.0 && absMs < prevMs) ||
            !ParseWorkloadSizeBytes(fields[sIdx], sz) || sz == 0)
        {
            return false;
        }
        prevMs = absMs;
        arrSec.push_back(startOffsetS + absMs / 1000.0);
        sizes.push_back(static_cast<uint32_t>(sz));
    }
    return !cols.empty() && !arrSec.empty();
}

}
