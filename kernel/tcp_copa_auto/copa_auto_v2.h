/* SPDX-License-Identifier: GPL-2.0-only */
/* Experimental, independent measurement periods for Copa-Auto v2. */
#ifndef COPA_AUTO_V2_H
#define COPA_AUTO_V2_H
#include "copa_auto_core.h"

#define COPA_V2_MAX_ROUNDS 256U
struct copa_v2_round {
    u64 standing_sum, rate_sum;
    u32 samples;
    bool app_limited;
};
struct copa_v2_state {
    struct copa_v2_round rounds[COPA_V2_MAX_ROUNDS];
    u32 count, period_rtts;
    u64 start_us, end_us, round_start_us, round_len_us, r0_us;
    u64 last_k_us, last_target_us, last_mean_dq_us, last_max_rate, last_r0_us;
    u64 loss_bytes, last_loss_bytes;
    u32 last_valid, periods, updates;
    u32 last_rounds, last_app_limited, last_quality_milli, last_spread_milli;
    u32 last_error_milli, last_gain_milli, last_rho_milli;
    u64 period_samples, period_limited;
    bool started;
};

static inline void copa_v2_init(struct copa_v2_state *v, u32 period_rtts)
{
    u32 i;
    u8 *p = (u8 *)v;
    for (i = 0; i < sizeof(*v); i++) p[i] = 0;
    v->period_rtts = clamp_t(u32, period_rtts, 1U, COPA_V2_MAX_ROUNDS);
}

/* Robust weighted absolute-error fit of q = k * rho/(1-rho).
 * In q-space, downweight points near rho=1 by (1-rho)^2; the resulting
 * weighted median of per-point k has weight rho*(1-rho). All inputs are
 * from this period; no earlier k, rate or RTT enters the fit. */
static inline bool copa_v2_finish(struct copa_v2_state *v, struct copa_auto_state *ca)
{
    u64 ks[COPA_V2_MAX_ROUNDS], weights[COPA_V2_MAX_ROUNDS];
    u64 max_rate = 0, dq_sum = 0, rho_sum = 0, total_weight = 0;
    u64 target, mean, factor, delta, residual_sum = 0, q_sum = 0;
    u32 i, n = 0, qn = 0, min_rho = 1000, max_rho = 0, quality, gain;
    for (i = 0; i <= v->count && i < COPA_V2_MAX_ROUNDS; i++) {
        struct copa_v2_round *r = &v->rounds[i];
        u64 rate;
        if (r->samples < 2 || r->app_limited) continue;
        rate = div64_u64(r->rate_sum, r->samples);
        max_rate = max_t(u64, max_rate, rate);
    }
    for (i = 0; i <= v->count && i < COPA_V2_MAX_ROUNDS; i++) {
        struct copa_v2_round *r = &v->rounds[i];
        u64 rate, stand, q, k, rho, weight;
        u32 pos;
        if (r->samples < 2 || r->app_limited || !max_rate) continue;
        rate = div64_u64(r->rate_sum, r->samples);
        stand = div64_u64(r->standing_sum, r->samples);
        q = stand > v->r0_us ? stand - v->r0_us : 0;
        dq_sum += q;
        rho = div64_u64(rate * 1000ULL, max_rate);
        rho_sum += rho;
        qn++;
        if (!rate || rate >= max_rate) continue;
        min_rho = min_t(u32, min_rho, (u32)rho);
        max_rho = max_t(u32, max_rho, (u32)rho);
        weight = max_t(u64, rho * (1000ULL - rho), 1ULL);
        k = div64_u64(q * min_t(u64, div64_u64((max_rate - rate) * 1024ULL, rate), 0xffffffffULL), 1024ULL);
        pos = n++;
        while (pos && ks[pos - 1] > k) {
            ks[pos] = ks[pos - 1]; weights[pos] = weights[pos - 1]; pos--;
        }
        ks[pos] = k; weights[pos] = weight; total_weight += weight;
    }
    v->periods++;
    v->last_loss_bytes = v->loss_bytes;
    v->last_valid = n; v->last_rounds = qn;
    v->last_app_limited = v->period_samples ? (u32)div64_u64(v->period_limited * 1000ULL, v->period_samples) : 0;
    v->last_max_rate = max_rate; v->last_r0_us = v->r0_us;
    v->last_k_us = v->last_target_us = v->last_mean_dq_us = 0;
    v->last_quality_milli = v->last_spread_milli = v->last_error_milli = v->last_gain_milli = 0;
    v->last_rho_milli = qn ? (u32)div64_u64(rho_sum, qn) : 0;
    if (n < 2 || !qn || !v->r0_us || !total_weight) return false;
    {
        u64 accum = 0;
        for (i = 0; i < n; i++) {
            accum += weights[i];
            if (accum >= (total_weight + 1) / 2) break;
        }
        v->last_k_us = ks[min_t(u32, i, n - 1)];
    }
    if (!v->last_k_us) return false;
    /* Relative residual is evaluated in q-space, on the same eligible rounds. */
    for (i = 0; i <= v->count && i < COPA_V2_MAX_ROUNDS; i++) {
        struct copa_v2_round *r = &v->rounds[i];
        u64 rate, stand, q, pred;
        if (r->samples < 2 || r->app_limited) continue;
        rate = div64_u64(r->rate_sum, r->samples);
        if (!rate || rate >= max_rate) continue;
        stand = div64_u64(r->standing_sum, r->samples);
        q = stand > v->r0_us ? stand - v->r0_us : 0;
        pred = div64_u64(v->last_k_us * min_t(u64, div64_u64(rate * 1024ULL, max_rate - rate), 0xffffffffULL), 1024ULL);
        residual_sum += pred > q ? pred - q : q - pred;
        q_sum += q;
    }
    v->last_spread_milli = max_rho - min_rho;
    v->last_error_milli = (u32)min_t(u64, div64_u64(residual_sum * 1000ULL, max_t(u64, q_sum, 1ULL)), 4000ULL);
    /* Quality uses only this period. Narrow rate coverage and poor fit slow
     * the update; they never change rho, clamp delta, or reuse an old k. */
    quality = min_t(u32, n * 125U, 1000U);
    quality = (u32)div64_u64((u64)quality * min_t(u32, v->last_spread_milli * 5U, 1000U), 1000ULL);
    quality = (u32)div64_u64((u64)quality * 1000ULL, 1000ULL + v->last_error_milli);
    quality = (u32)div64_u64((u64)quality * (1000U - v->last_app_limited), 1000ULL);
    v->last_quality_milli = quality;
    gain = 100U + (400U * quality) / 1000U;
    v->last_gain_milli = gain;
    target = copa_target_dq_us(0, v->last_k_us, v->r0_us, 500);
    mean = div64_u64(dq_sum, qn);
    v->last_target_us = target; v->last_mean_dq_us = mean;
    if (!target) return false;
    {
        s64 err = copa_ln_ratio_q16(max_t(u64, mean, 1ULL), target);
        factor = copa_exp_q16(div64_s64(err * (s64)gain, 1000LL));
    }
    factor = min_t(u64, factor, (~0ULL) / max_t(u32, ca->delta_q20, 1U));
    delta = ((u64)ca->delta_q20 * factor) >> 16;
    ca->delta_q20 = (u32)clamp_t(u64, delta, 1ULL, 0xffffffffULL);
    ca->cfg.init_delta_q20 = ca->delta_q20;
    ca->rho_milli = v->last_rho_milli;
    ca->ratio_us = v->last_k_us; ca->base_rtt_us = v->r0_us;
    ca->dq_target_us = target; v->updates++;
    return true;
}

static inline u32 copa_v2_on_ack(struct copa_v2_state *v, struct copa_auto_state *ca,
                                 u64 now, u64 rtt, u64 srtt, u32 acked, u32 mss,
                                 u64 rate, bool app_limited, bool low_flight)
{
    struct copa_v2_round *r;
    u32 cwnd;
    if (!rtt) return (u32)(ca->cwnd_q10 >> 10);
    if (!srtt) srtt = rtt;
    if (v->started && now >= v->end_us) {
        copa_v2_finish(v, ca);
        v->started = false;
    }
    if (!v->started) {
        u32 i;
        for (i = 0; i < COPA_V2_MAX_ROUNDS; i++) {
            v->rounds[i].standing_sum = v->rounds[i].rate_sum = 0;
            v->rounds[i].samples = 0; v->rounds[i].app_limited = false;
        }
        v->loss_bytes = 0;
        v->period_samples = v->period_limited = 0;
        v->started = true; v->count = 0; v->start_us = now;
        v->round_start_us = now; v->round_len_us = max_t(u64, srtt, 1ULL);
        v->end_us = now + v->round_len_us * v->period_rtts;
        v->r0_us = rtt;
        /* Standing-RTT samples must also belong to this period. */
        copa_minmax_reset(&ca->rtt_standing, now, rtt);
    }
    if (rtt < v->r0_us) v->r0_us = rtt;
    if (now >= v->round_start_us + v->round_len_us && v->count + 1 < COPA_V2_MAX_ROUNDS) {
        v->count++; v->round_start_us = now; v->round_len_us = max_t(u64, srtt, 1ULL);
    }
    /* Override the v1 10-second filter with this period's running minimum. */
    copa_minmax_reset(&ca->rtt_min, now, v->r0_us);
    cwnd = copa_on_ack(ca, now, rtt, srtt, acked, mss, rate, app_limited, low_flight);
    r = &v->rounds[v->count];
    v->period_samples++;
    if (app_limited) v->period_limited++;
    if (!app_limited && rate) {
        r->standing_sum += ca->standing_us;
        r->rate_sum += rate;
        r->samples++;
    }
    return cwnd;
}
#endif
