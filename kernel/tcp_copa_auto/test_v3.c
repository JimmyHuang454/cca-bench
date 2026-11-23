/*
 * Unit tests for Copa-Auto v3 (Scheme A: unbiased-mean ProbeBW,
 * Scheme B: multi-scale Queue-Diffusion).
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "copa_auto_v3.h"

static void test_loss_insensitivity(void)
{
    u8 variant;
    for (variant = 0; variant <= 2; variant++) {
        struct copa_v3_state st;
        struct copa_v3_cfg cfg = {
            .variant = variant,
            .mode_switching = true,
            .jitter_us = 2000,
            .theta_milli = 500,
            .init_delta_q20 = COPA_V3_DELTA_INIT_Q20,
        };
        copa_v3_init(&st, cfg, NULL, 1000000ULL, 20U);
        st.slow_start = false;
        st.dq_target_us = 8000ULL;
        st.delta_run_q20 = 104858U; /* 0.10 */
        st.delta_q20 = 104858U;
        st.k_us = 400ULL;
        st.has_k = true;
        st.cwnd_q10 = 80ULL << 10;

        u64 before_cwnd = st.cwnd_q10;
        u32 before_delta = st.delta_q20;
        u32 before_drun = st.delta_run_q20;
        u64 before_tgt = st.dq_target_us;
        u64 before_k = st.k_us;

        /* 50 consecutive packet losses in default mode MUST be a 100% no-op */
        for (int i = 0; i < 50; i++)
            copa_v3_on_loss(&st, 1000000ULL + (u64)i * 50000ULL, false);

        assert(st.cwnd_q10 == before_cwnd);
        assert(st.delta_q20 == before_delta);
        assert(st.delta_run_q20 == before_drun);
        assert(st.dq_target_us == before_tgt);
        assert(st.k_us == before_k);
        assert(!st.competitive);
    }
    printf("PASS test_loss_insensitivity\n");
}

static void test_v3a_unbiased_trimmed_mean(void)
{
    struct copa_v3_state st;
    struct copa_v3_cfg cfg = {
        .variant = COPA_V3_PROBE_BW,
        .mode_switching = false,
        .jitter_us = 0,
        .theta_milli = 500,
        .init_delta_q20 = COPA_V3_DELTA_INIT_Q20,
    };
    copa_v3_init(&st, cfg, NULL, 0, 20U);

    /* Fluctuating wireless link: 10..60 Mbps (mean = 35 Mbps, max = 60 Mbps) */
    u64 samples[6] = { 1000000ULL, 6000000ULL, 2000000ULL, 5000000ULL, 3000000ULL, 4000000ULL };
    for (int i = 0; i < 6; i++)
        copa_v3_record_mu_sample(&st, samples[i]);

    /* Trimmed mean drops 1M and 6M -> average(2,3,4,5M) = 3.5M (exact mean, NOT 6M peak!) */
    assert(st.mu_hat_bps == 3500000ULL);

    /* Sudden regime shift upward: two consecutive 12M samples flush old 3.5M window */
    copa_v3_record_mu_sample(&st, 12000000ULL);
    copa_v3_record_mu_sample(&st, 12000000ULL);
    assert(st.mu_hat_bps == 12000000ULL);
    printf("PASS test_v3a_unbiased_trimmed_mean\n");
}

static void test_v3b_diffusion_recovery_and_outlier_rejection(void)
{
    struct copa_v3_state st;
    struct copa_v3_cfg cfg = {
        .variant = COPA_V3_DIFFUSION,
        .mode_switching = false,
        .jitter_us = 3000,
        .theta_milli = 500,
        .init_delta_q20 = COPA_V3_DELTA_INIT_Q20,
    };
    copa_v3_init(&st, cfg, NULL, 0, 20U);

    /*
     * Synthetic queue delay W_t with known diffusion k = 500 us:
     * over dt = 10 ms = 10000 us, std(dW) = sqrt(2 * k * dt) = sqrt(10^7) = 3162 us.
     * Use Box-Muller deterministic PRNG so W_t stays well inside busy region (~30 ms mean).
     */
    unsigned int seed = 12345U;
    double w_us = 30000.0;
    for (u64 idx = 1; idx <= 80; idx++) {
        seed = seed * 1103515245U + 12345U;
        double u1 = ((seed >> 16) & 0x7fff) / 32768.0;
        if (u1 < 1e-6) u1 = 1e-6;
        seed = seed * 1103515245U + 12345U;
        double u2 = ((seed >> 16) & 0x7fff) / 32768.0;
        double z = sqrt(-2.0 * log(u1)) * cos(6.283185307 * u2);
        w_us += z * 3162.28;
        if (w_us < 10000.0) w_us = 10000.0 + fabs(w_us - 10000.0);
        if (w_us > 60000.0) w_us = 60000.0 - fabs(w_us - 60000.0);

        u32 raw_dq = (u32)(w_us + 3000.0);
        /* Inject a single huge 180 ms step spike at idx == 50 to test outlier rejection */
        if (idx == 50)
            raw_dq += 180000U;
        copa_v3_diff_push_bin(&st, idx, raw_dq, 1000ULL);
    }

    u64 vw_us = 0;
    bool ok = copa_v3_compute_diffusion_vw(&st, &vw_us);
    assert(ok);
    /* Should recover k ~ 500 us within [200, 1200] us despite the 180 ms outlier spike */
    assert(vw_us >= 200ULL && vw_us <= 1200ULL);
    printf("PASS test_v3b_diffusion_recovery_and_outlier_rejection (vw=%llu us)\n",
           (unsigned long long)vw_us);
}

static void test_dd1_wired_stays_at_delta_half(void)
{
    u8 variant;
    for (variant = 0; variant <= 2; variant++) {
        struct copa_v3_state st;
        struct copa_v3_cfg cfg = {
            .variant = variant,
            .mode_switching = true,
            .jitter_us = -1,
            .theta_milli = 500,
            .init_delta_q20 = COPA_V3_DELTA_INIT_Q20,
        };
        copa_v3_init(&st, cfg, NULL, 100000ULL, 10U);

        /* Simulate 12 s on clean 50 Mbps, 40 ms RTT D/D/1 link with realistic 1-RTT pipe lag */
        u64 t_us = 100000ULL;
        u64 link_bps = 6250000ULL; /* 50 Mbps */
        u32 mss = 1448U;
        u64 pipe[20];
        for (int k = 0; k < 20; k++) pipe[k] = 360000ULL;
        for (int step = 0; step < 6000; step++) {
            t_us += 2000ULL; /* ACK every 2 ms -> 20 steps = 40 ms = 1 RTT */
            u64 pace = copa_v3_pacing_rate_bps(&st, mss);
            u64 cwnd_pkts = st.cwnd_q10 >> 10;
            u64 cwnd_rate = (cwnd_pkts * mss * 1000000ULL) / 40000ULL;
            u64 send_now = (pace > 0 && pace < cwnd_rate) ? pace : cwnd_rate;
            u64 arr_rate = pipe[step % 20];
            pipe[step % 20] = send_now;
            u64 deliv = arr_rate < link_bps ? arr_rate : link_bps;
            u64 q_us = (arr_rate > link_bps) ? ((arr_rate - link_bps) * 20000ULL) / link_bps : 150ULL;
            if (q_us > 15000ULL) q_us = 15000ULL;
            copa_v3_on_ack(&st, t_us, 40000ULL + q_us, 40000ULL + q_us,
                           2U, mss, deliv, false, false);
        }

        assert(st.has_jitter);
        assert(st.jitter_us <= 1500ULL);
        assert(st.k_us <= 100ULL);
        /* delta should stay near 0.50 (>= 0.40) on clean D/D/1 link */
        assert(st.delta_q20 >= 419430U);
        printf("PASS test_dd1_wired variant=%u: j=%llu k=%llu dq*=%llu delta=%.3f\n",
               variant,
               (unsigned long long)st.jitter_us,
               (unsigned long long)st.k_us,
               (unsigned long long)st.dq_target_us,
               st.delta_q20 / 1048576.0);
    }
}

static void test_wireless_jitter_and_burst_adaptation(void)
{
    u8 variant;
    for (variant = 0; variant <= 2; variant++) {
        struct copa_v3_state st;
        struct copa_v3_cfg cfg = {
            .variant = variant,
            .mode_switching = false,
            .jitter_us = -1,
            .theta_milli = 500,
            .init_delta_q20 = COPA_V3_DELTA_INIT_Q20,
        };
        copa_v3_init(&st, cfg, NULL, 100000ULL, 10U);

        /* Wireless 40 Mbps mean link, 40 ms R0, 6 ms jitter floor, fading +/- 50% (1-RTT pipe) */
        u64 t_us = 100000ULL;
        u32 mss = 1448U;
        double q_bytes = 0.0;
        double pipe[23];
        for (int k = 0; k < 23; k++) pipe[k] = 320000.0;
        for (int step = 0; step < 8000; step++) {
            t_us += 2000ULL;
            double phase = (t_us / 100000.0);
            double cap_bps = 5000000.0 * (1.0 + 0.5 * sin(phase * 1.7) + 0.25 * cos(phase * 3.1));
            if (cap_bps < 1000000.0) cap_bps = 1000000.0;

            u64 pace = copa_v3_pacing_rate_bps(&st, mss);
            u64 cwnd_pkts = st.cwnd_q10 >> 10;
            u64 cwnd_rate = (cwnd_pkts * mss * 1000000ULL) / 46000ULL;
            double send_now = (double)((pace > 0 && pace < cwnd_rate) ? pace : cwnd_rate);
            if (send_now > 15000000.0) send_now = 15000000.0;
            double arr_bps = pipe[step % 23];
            pipe[step % 23] = send_now;

            q_bytes += (arr_bps - cap_bps) * 0.002;
            if (q_bytes < 0.0) q_bytes = 0.0;
            if (q_bytes > 500000.0) q_bytes = 500000.0;

            double deliv_bps = (q_bytes > 0.0) ? cap_bps : (arr_bps < cap_bps ? arr_bps : cap_bps);
            u64 q_us = (u64)(q_bytes * 1e6 / 5000000.0);
            /* Periodic 6 ms jitter floor (sawtooth between 0 and 12 ms with 6 ms standing excess) */
            u64 jit = (step % 40 == 0) ? 0ULL : 6000ULL;
            u64 rtt = 40000ULL + jit + q_us;
            copa_v3_on_ack(&st, t_us, rtt, rtt, 2U, mss, (u64)deliv_bps, false, false);
        }

        assert(st.has_jitter);
        assert(st.jitter_us >= 3000ULL && st.jitter_us <= 10000ULL);
        assert(st.k_us > 50ULL);
        assert(st.dq_target_us > st.jitter_us + 1000ULL);
        /* Must adapt delta well below 0.50 to overcome jitter + fading */
        assert(st.delta_q20 < 200000U);
        printf("PASS test_wireless variant=%u: j=%llu k=%llu rho=%u dq*=%llu delta=%.4f\n",
               variant,
               (unsigned long long)st.jitter_us,
               (unsigned long long)st.k_us,
               st.rho_milli,
               (unsigned long long)st.dq_target_us,
               st.delta_q20 / 1048576.0);
    }
}

static void test_base_rtt_handover_jump(void)
{
    u8 variant;
    for (variant = 0; variant <= 2; variant++) {
        struct copa_v3_state st;
        struct copa_v3_cfg cfg = {
            .variant = variant,
            .mode_switching = true,
            .jitter_us = -1,
            .theta_milli = 500,
            .init_delta_q20 = COPA_V3_DELTA_INIT_Q20,
        };
        copa_v3_init(&st, cfg, NULL, 100000ULL, 20U);

        u64 t_us = 100000ULL;
        u32 mss = 1448U;
        /* First 2 s at base RTT = 30 ms */
        for (int step = 0; step < 500; step++) {
            t_us += 4000ULL;
            copa_v3_on_ack(&st, t_us, 30000ULL, 30000ULL, 2U, mss, 5000000ULL, false, false);
        }
        assert(st.base_rtt_us == 30000ULL);

        /* Route change: base RTT jumps permanently to 80 ms (for 12 s) */
        for (int step = 0; step < 3000; step++) {
            t_us += 4000ULL;
            copa_v3_on_ack(&st, t_us, 80000ULL, 80000ULL, 2U, mss, 5000000ULL, false, false);
        }
        /* PROBE_EMPTY must detect the plateau and lift base_rtt_us to 80 ms */
        assert(st.base_rtt_us >= 75000ULL && st.base_rtt_us <= 85000ULL);
        printf("PASS test_base_rtt_handover variant=%u: new base_rtt=%llu us\n",
               variant, (unsigned long long)st.base_rtt_us);
    }
}

static void test_v3_hybrid_idle_wakeup_safety_gate(void)
{
    /* Case A: after 1.5 s idle, link stays good (queue stays flat) -> ProbeUp MUST fire */
    {
        struct copa_v3_state st;
        struct copa_v3_cfg cfg = {
            .variant = COPA_V3_HYBRID,
            .mode_switching = false,
            .jitter_us = 2000,
            .theta_milli = 500,
            .init_delta_q20 = COPA_V3_DELTA_INIT_Q20,
        };
        copa_v3_init(&st, cfg, NULL, 100000ULL, 20U);
        st.slow_start = false;
        st.bw_ewma_bps = 5000000ULL;
        copa_v3_on_ack(&st, 200000ULL, 40000ULL, 40000ULL, 2U, 1448U, 5000000ULL, false, false);

        /* 1.5 s idle pause */
        u64 t_us = 1700000ULL;
        bool probed = false;
        for (int i = 0; i < 40; i++) {
            t_us += 4000ULL;
            copa_v3_on_ack(&st, t_us, 40500ULL, 40500ULL, 2U, 1448U, 5000000ULL, false, false);
            if (st.phase == COPA_V3_PHASE_PROBE_UP) {
                probed = true;
                break;
            }
        }
        assert(probed);
    }

    /* Case B: after 1.5 s idle, link degraded (queue immediately jumps +20 ms) -> NO ProbeUp! */
    {
        struct copa_v3_state st;
        struct copa_v3_cfg cfg = {
            .variant = COPA_V3_HYBRID,
            .mode_switching = false,
            .jitter_us = 2000,
            .theta_milli = 500,
            .init_delta_q20 = COPA_V3_DELTA_INIT_Q20,
        };
        copa_v3_init(&st, cfg, NULL, 100000ULL, 20U);
        st.slow_start = false;
        st.bw_ewma_bps = 5000000ULL;
        copa_v3_on_ack(&st, 200000ULL, 40000ULL, 40000ULL, 2U, 1448U, 5000000ULL, false, false);

        /* 1.5 s idle pause, then immediate +20 ms queueing delay */
        u64 t_us = 1700000ULL;
        bool probed = false;
        for (int i = 0; i < 40; i++) {
            t_us += 4000ULL;
            copa_v3_on_ack(&st, t_us, 60000ULL, 60000ULL, 2U, 1448U, 1500000ULL, false, false);
            if (st.phase == COPA_V3_PHASE_PROBE_UP)
                probed = true;
        }
        assert(!probed);
        assert(!st.idle_wakeup_pending);
    }
    printf("PASS test_v3_hybrid_idle_wakeup_safety_gate\n");
}

static void test_v3_hybrid_j_jump_anti_starvation(void)
{
    struct copa_v3_state st;
    struct copa_v3_cfg cfg = {
        .variant = COPA_V3_HYBRID,
        .mode_switching = true,
        .jitter_us = -1,
        .theta_milli = 500,
        .init_delta_q20 = COPA_V3_DELTA_INIT_Q20,
    };
    copa_v3_init(&st, cfg, NULL, 100000ULL, 80U);
    st.slow_start = false;
    st.first_empty_done = true;
    st.has_jitter = true;
    st.jitter_us = 2000ULL;
    st.base_rtt_us = 20000ULL;
    st.peak_bdp_q10 = 100ULL << 10;
    st.bw_ewma_bps = 6000000ULL;
    copa_v3_mm_reset(&st.recent_cwnd_max, 100000ULL, 80ULL << 10);

    /* Simulate cwnd collapsing to 14 pkts while Starlink jitter jumped to 12 ms (RTT=20..32 ms) */
    st.cwnd_q10 = 14ULL << 10;
    u64 t_us = 200000ULL;
    bool triggered_empty = false;
    for (int i = 0; i < 200; i++) {
        t_us += 4000ULL;
        u64 rtt = (i % 12 == 0) ? 20000ULL : 32000ULL;
        copa_v3_on_ack(&st, t_us, rtt, 30000ULL, 2U, 1448U, 4000000ULL, false, false);
        if (st.phase == COPA_V3_PHASE_PROBE_EMPTY)
            triggered_empty = true;
    }
    assert(triggered_empty);
    assert(st.jitter_us >= 5000ULL);
    assert(st.delta_q20 <= 157286U); /* <= 0.15, unstarved! */
    printf("PASS test_v3_hybrid_j_jump_anti_starvation (j=%llu delta=%.4f)\n",
           (unsigned long long)st.jitter_us, st.delta_q20 / 1048576.0);
}

int main(void)
{
    test_loss_insensitivity();
    test_v3a_unbiased_trimmed_mean();
    test_v3b_diffusion_recovery_and_outlier_rejection();
    test_dd1_wired_stays_at_delta_half();
    test_wireless_jitter_and_burst_adaptation();
    test_base_rtt_handover_jump();
    test_v3_hybrid_idle_wakeup_safety_gate();
    test_v3_hybrid_j_jump_anti_starvation();
    printf("ALL v3 tests PASSED!\n");
    return 0;
}
