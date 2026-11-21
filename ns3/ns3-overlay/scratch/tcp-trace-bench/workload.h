#pragma once
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include <algorithm>
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
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::istringstream iss(line);
        double gapMs = 0.0;
        uint64_t sz = 0;
        if (!(iss >> gapMs >> sz) || sz == 0)
        {
            continue;
        }
        tSec += gapMs / 1000.0;
        arrSec.push_back(tSec);
        sizes.push_back(static_cast<uint32_t>(std::min<uint64_t>(sz, 100000000ULL)));
    }
    return !arrSec.empty();
}

}
