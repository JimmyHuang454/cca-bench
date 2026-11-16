/*
 * Copyright (c) 2024
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * BBRv3-style congestion control, see tcp-bbr-v3.h for details.
 */

#include "tcp-bbr-v3.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpBbrV3");
NS_OBJECT_ENSURE_REGISTERED(TcpBbrV3);

TypeId
TcpBbrV3::GetTypeId()
{
    static TypeId tid = TypeId("ns3::TcpBbrV3")
                            .SetParent<TcpBbrV2>()
                            .AddConstructor<TcpBbrV3>()
                            .SetGroupName("Internet");
    return tid;
}

TcpBbrV3::TcpBbrV3()
    : TcpBbrV2()
{
    NS_LOG_FUNCTION(this);
    SetBbrVersion(3);
}

TcpBbrV3::TcpBbrV3(const TcpBbrV3& sock)
    : TcpBbrV2(sock)
{
    NS_LOG_FUNCTION(this);
    SetBbrVersion(3);
}

TcpBbrV3::~TcpBbrV3()
{
    NS_LOG_FUNCTION(this);
}

std::string
TcpBbrV3::GetName() const
{
    return "TcpBbrV3";
}

Ptr<TcpCongestionOps>
TcpBbrV3::Fork()
{
    return CopyObject<TcpBbrV3>(this);
}

} // namespace ns3
