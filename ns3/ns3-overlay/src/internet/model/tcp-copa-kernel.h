#ifndef TCP_COPA_KERNEL_H
#define TCP_COPA_KERNEL_H

#include "tcp-congestion-ops.h"
#include "ns3/traced-callback.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-statement-expression-from-macro-expansion"
#endif
#include "copa_auto_core.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace ns3
{

class TcpCopaKernel : public TcpCongestionOps
{
  public:
    static TypeId GetTypeId();

    TcpCopaKernel();
    TcpCopaKernel(const TcpCopaKernel& other);
    ~TcpCopaKernel() override = default;

    std::string GetName() const override;
    bool HasCongControl() const override;
    Ptr<TcpCongestionOps> Fork() override;
    uint32_t GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;
    void CongestionStateSet(Ptr<TcpSocketState> tcb,
                            const TcpSocketState::TcpCongState_t newState) override;
    void CongControl(Ptr<TcpSocketState> tcb,
                     const TcpRateOps::TcpRateConnection& rc,
                     const TcpRateOps::TcpRateSample& rs) override;

  private:
    void EnsureInit(Ptr<TcpSocketState> tcb);
    static uint64_t QuantizeUs(uint64_t valUs, uint32_t clockUs);

    bool m_adaptive{true};
    bool m_modeSwitching{false};
    int32_t m_jitterUs{-1};
    uint32_t m_thetaMilli{500};
    uint32_t m_initDeltaQ20{COPA_DELTA_INIT_Q20};
    uint32_t m_copaClockUs{1};
    uint32_t m_debugMs{0};
    u64 m_nextDebugUs{0};
    bool m_initialized{false};
    struct copa_auto_state m_ca{};
    TracedCallback<double, double, double, double, double, double> m_estTrace;
};

} // namespace ns3

#endif // TCP_COPA_KERNEL_H
