/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Core state machine for tcp_copa_auto (pure 64-bit integer arithmetic).
 * Mirrors quinn-proto/src/congestion/copa/{mod.rs,auto.rs,drain.rs} including
 * standing-queue-corrected k estimation, left/right-censoring damping,
 * rate-based DrainController, and RCT optimizations.
 */
#ifndef COPA_AUTO_CORE_H
#define COPA_AUTO_CORE_H

#include "copa_auto_math.h"

#define COPA_MIN_CWND_PKTS     4U
#define COPA_MAX_CWND_PKTS     1000000U
#define COPA_MAX_JITTER_US     20000ULL   /* 20 ms */
#define COPA_IDLE_MARGIN_US    1000ULL    /* 1 ms */
#define COPA_MIN_RTT_WIN_US    10000000ULL /* 10 s */
#define COPA_PROBE_PERIOD_US   20000000ULL /* 20 s */
#define COPA_PROBE_DUR_MIN_US  300000ULL   /* 300 ms */
#define COPA_PROBE_DUR_MAX_US  2000000ULL  /* 2 s */
#define COPA_DRAIN_SKIP_US     10000000ULL /* 10 s */
#define COPA_DELTA_MIN_Q20     1049U       /* 0.001 * 2^20 */
#define COPA_DELTA_MAX_Q20     524288U     /* 0.5   * 2^20 */
#define COPA_DELTA_INIT_Q20    104858U     /* 0.1   * 2^20 */
#define COPA_MAX_STEP_Q16      (2LL * COPA_LN2_Q16) /* ln(4) in Q16 */
#define COPA_FAR_RATIO_Q16     26573LL     /* ln(1.5) in Q16 */

enum copa_dir {
    COPA_DIR_NONE = 0,
    COPA_DIR_UP   = 1,
    COPA_DIR_DOWN = 2,
};

enum copa_drain_phase {
    COPA_PHASE_NORMAL     = 0,
    COPA_PHASE_DRAINING   = 1,
    COPA_PHASE_RECOVERING = 2,
};

enum copa_drain_kind {
    COPA_DRAIN_REFRESH_RTT = 0,
    COPA_DRAIN_MEASURE_J   = 1,
};

/* Kathleen Nichols 3-sample windowed min/max filter (time in us) */
struct copa_minmax_sample {
    u64 t;
    u64 v;
};

struct copa_minmax {
    struct copa_minmax_sample s[3];
    bool init;
};

static inline u64 copa_minmax_get(const struct copa_minmax *m)
{
    return m->init ? m->s[0].v : 0;
}

static inline void copa_minmax_reset(struct copa_minmax *m, u64 t, u64 v)
{
    m->s[0].t = m->s[1].t = m->s[2].t = t;
    m->s[0].v = m->s[1].v = m->s[2].v = v;
    m->init = true;
}

static inline void copa_minmax_subwin_update(struct copa_minmax *m, u64 win, u64 t, u64 v)
{
    u64 dt = (t >= m->s[0].t) ? (t - m->s[0].t) : 0;

    if (dt > win) {
        m->s[0] = m->s[1];
        m->s[1] = m->s[2];
        m->s[2].t = t;
        m->s[2].v = v;
        if (t >= m->s[0].t && (t - m->s[0].t) > win) {
            m->s[0] = m->s[1];
            m->s[1] = m->s[2];
            m->s[2].t = t;
            m->s[2].v = v;
        }
    } else if (m->s[1].t == m->s[0].t && dt > win / 4) {
        m->s[2].t = m->s[1].t = t;
        m->s[2].v = m->s[1].v = v;
    } else if (m->s[2].t == m->s[1].t && dt > win / 2) {
        m->s[2].t = t;
        m->s[2].v = v;
    }
}

static inline u64 copa_minmax_running_min(struct copa_minmax *m, u64 win, u64 t, u64 v)
{
    if (!m->init || v <= m->s[0].v || (t >= m->s[2].t && (t - m->s[2].t) > win)) {
        copa_minmax_reset(m, t, v);
        return v;
    }
    if (v <= m->s[1].v) {
        m->s[2].t = m->s[1].t = t;
        m->s[2].v = m->s[1].v = v;
    } else if (v <= m->s[2].v) {
        m->s[2].t = t;
        m->s[2].v = v;
    }
    copa_minmax_subwin_update(m, win, t, v);
    return m->s[0].v;
}

static inline u64 copa_minmax_running_max(struct copa_minmax *m, u64 win, u64 t, u64 v)
{
    if (!m->init || v >= m->s[0].v || (t >= m->s[2].t && (t - m->s[2].t) > win)) {
        copa_minmax_reset(m, t, v);
        return v;
    }
    if (v >= m->s[1].v) {
        m->s[2].t = m->s[1].t = t;
        m->s[2].v = m->s[1].v = v;
    } else if (v >= m->s[2].v) {
        m->s[2].t = t;
        m->s[2].v = v;
    }
    copa_minmax_subwin_update(m, win, t, v);
    return m->s[0].v;
}

/* Shared cross-flow probe/drain coordination board (4-slot ring) */
struct copa_probe_board {
    u64 requests[4];
    u64 drain_requests[4];
    u64 length_slot[4];
    u64 length_us[4];
};

struct copa_auto_cfg {
    bool adaptive;
    bool mode_switching;
    s32  jitter_us;        /* < 0: auto-probe j; >= 0: manual fixed j [us] (no MEASURE_J probe) */
    u32  theta_milli;      /* 500 = 0.50; 980 = 0.98 */
    u32  init_delta_q20;   /* 0.1 for copa_auto, 0.5 for copa_orig */
};

static inline bool copa_fixed_jitter(const struct copa_auto_cfg *cfg)
{
    return cfg->jitter_us >= 0;
}

static inline u64 copa_fixed_jitter_us(const struct copa_auto_cfg *cfg)
{
    return cfg->jitter_us > 0 ? min_t(u64, (u64)cfg->jitter_us, 100000ULL) : 0ULL;
}

struct copa_auto_state {
    struct copa_auto_cfg cfg;
    struct copa_probe_board *board;

    /* RTT filters */
    struct copa_minmax rtt_min;
    struct copa_minmax rtt_standing;
    struct copa_minmax rtt_max;
    u64 standing_us;
    u64 srtt_us;
    u64 dq_us;

    /* Copa cwnd/velocity/mode state */
    u64 cwnd_q10;          /* cwnd in packets * 1024 for fractional AIMD precision */
    u32 delta_q20;         /* active delta in Q20 */
    u32 velocity;
    u8  direction;
    u32 same_direction;
    u64 last_rec_time_us;
    u64 last_rec_cwnd_q10;
    bool slow_start;
    bool competitive;
    u64 last_nearly_empty_us;
    u64 last_delta_dec_us;
    u64 last_time_us;
    bool last_ack_app_limited;
    bool seen_app_limited;
    u64 last_app_limited_us;
    u64 ss_confirm_since_us;
    u32 startup_delta_q20;
    u64 peak_bdp_pkts_q10;
    u64 last_bdp_pkts;
    u64 loss_dq_us;
    u64 last_loss_us;
    u64 last_loss_relax_us;

    /* 2-bucket bandwidth estimator (bytes/sec) */
    u64 bw_buckets[2];
    u64 bw_updated_us[2];
    u64 bw_bucket_start_us;
    u8  bw_active;

    /* DrainController state */
    u8  drain_phase;
    u8  drain_kind;
    bool drain_has_slot;
    u64 drain_slot;
    u64 evaluated_slot;
    bool has_evaluated_slot;
    u64 drain_start_us;
    u64 drain_deadline_us;
    u64 drain_idle_at_us;
    u64 drain_measure_until_us;
    u64 drain_saved_cwnd_q10;
    u64 drain_saved_rate_bps;
    u64 drain_soft_cwnd_q10;
    u64 drain_rate_bps;
    u64 drain_rtt_min_start_us;
    u64 drain_trusted_start_us;
    bool drain_trusted_verified;
    bool drain_hold_full;
    u64 drain_min_rtt_us;
    u64 drain_round_start_us;
    u64 drain_prev_min_rtt_us;
    u32 drain_flat_rounds;
    bool drain_congested;
    u64 drain_probe_dq_sum_us;
    u64 drain_probe_acks;
    u64 trusted_min_us;
    u64 valid_after_us;
    u64 loaded_since_us;
    u64 last_loaded_ack_us;
    u64 passive_j_at_us;
    u64 low_window_us, low_min_us;
    u32 low_samples, low_windows, raised_windows;
    u64 raised_min_us, low_dq_sum_us;
    u64 last_app_idle_us;
    u64 drain_backoff_slots;
    u64 next_drain_slot;
    u64 recover_until_us;
    u64 recover_rate_cap_bps;

    /* AutoDelta estimator state */
    bool has_jitter;
    u64 jitter_us;
    u64 ratio_us;          /* k = c / mu [us] */
    u64 pending_k_us;
    u32 rho_milli;         /* [50, 980] */
    u64 base_rtt_us;
    u64 dq_target_us;
    u32 delta_run_q20;     /* 0 = unset */

    u64 last_ack_us;
    bool in_app_idle;
    u64 round_start_us;
    u64 round_dq_sum_us;
    u64 round_acks;
    u64 round_idle;
    u64 round_max_gap_us;
    u64 round_min_rtt_us;
    u64 round_deliv_bytes;
    bool round_app_limited;

    u64 rounds;
    u64 seg_dq_sum_us;
    u64 seg_stand_dq_sum_us;
    u64 seg_acks;
    u64 seg_idle;
    u64 seg_max_gap_us;
    u64 seg_min_rtt_us;
    u64 seg_deliv_bytes;
    u64 seg_dur_us;
    u64 seg_bw_est_bps;
    u32 seg_rounds;
    bool seg_probe;
    bool seg_app_limited;
    bool post_idle;

    u32 floor_shift_streak;
    bool handover_drain_pending;
    bool first_probe_done;
    bool aligned_probe_seen;
    u32 settle_halves;

    u64 decided_slot;
    bool has_decided_slot;
    u64 probing_slot;
    bool has_probing_slot;
    u64 slot_duration_us;
    u32 fallback_level;
    u32 agreeing;
    bool stable;
    bool reset_next;
    u64 rtt_min_at_probe_us;
    u64 probe_hi_us;
};

static inline void copa_reset_velocity(struct copa_auto_state *ca)
{
    ca->velocity = 1;
    ca->direction = COPA_DIR_NONE;
    ca->same_direction = 0;
    ca->last_rec_time_us = 0;
    ca->last_rec_cwnd_q10 = 0;
}

static inline void copa_reset_segment(struct copa_auto_state *ca)
{
    ca->seg_dq_sum_us = 0;
    ca->seg_stand_dq_sum_us = 0;
    ca->seg_acks = 0;
    ca->seg_idle = 0;
    ca->seg_max_gap_us = 0;
    ca->seg_min_rtt_us = 0;
    ca->seg_deliv_bytes = 0;
    ca->seg_dur_us = 0;
    ca->seg_bw_est_bps = 0;
    ca->seg_rounds = 0;
    ca->seg_probe = false;
    ca->seg_app_limited = false;
}

static inline void copa_state_init(struct copa_auto_state *ca,
                                   struct copa_auto_cfg cfg,
                                   struct copa_probe_board *board,
                                   u64 now_us,
                                   u32 init_cwnd_pkts)
{
    u32 i;
    u8 *p = (u8 *)ca;
    for (i = 0; i < sizeof(*ca); i++)
        p[i] = 0;

    ca->cfg = cfg;
    ca->board = board;
    ca->cwnd_q10 = (u64)max_t(u32, init_cwnd_pkts, COPA_MIN_CWND_PKTS) << 10;
    ca->delta_q20 = cfg.init_delta_q20;
    ca->velocity = 1;
    ca->slow_start = true;
    ca->last_nearly_empty_us = now_us;
    ca->last_time_us = now_us;
    ca->drain_backoff_slots = 1;
    if (cfg.adaptive && copa_fixed_jitter(&cfg)) {
        ca->has_jitter = true;
        ca->jitter_us = copa_fixed_jitter_us(&cfg);
        ca->first_probe_done = true;
    }
}

static inline u64 copa_bw_estimate(const struct copa_auto_state *ca, u64 now_us, u64 srtt_us)
{
    u64 max_age = max_t(u64, 8ULL * srtt_us, 1000000ULL);
    u64 best = 0;
    int i;
    for (i = 0; i < 2; i++) {
        if (ca->bw_updated_us[i] > 0 && now_us >= ca->bw_updated_us[i] &&
            (now_us - ca->bw_updated_us[i]) <= max_age) {
            if (ca->bw_buckets[i] > best)
                best = ca->bw_buckets[i];
        }
    }
    return best;
}

static inline void copa_bw_update(struct copa_auto_state *ca,
                                  u64 now_us,
                                  u64 srtt_us,
                                  u64 rate_bps,
                                  bool app_limited)
{
    u64 min_win = max_t(u64, srtt_us, 5000ULL);
    u64 cur_est;

    if (ca->drain_phase != COPA_PHASE_NORMAL || rate_bps == 0)
        return;

    if (ca->bw_bucket_start_us == 0)
        ca->bw_bucket_start_us = now_us;
    if (!app_limited && now_us >= ca->bw_bucket_start_us &&
        (now_us - ca->bw_bucket_start_us) >= 2ULL * min_win) {
        ca->bw_active ^= 1;
        ca->bw_buckets[ca->bw_active] = 0;
        ca->bw_updated_us[ca->bw_active] = 0;
        ca->bw_bucket_start_us = now_us;
    }

    cur_est = copa_bw_estimate(ca, now_us, srtt_us);
    if ((!app_limited || (cur_est > 0 && rate_bps > cur_est)) &&
        rate_bps >= ca->bw_buckets[ca->bw_active]) {
        ca->bw_buckets[ca->bw_active] = rate_bps;
        ca->bw_updated_us[ca->bw_active] = now_us;
    }
}

static inline u64 copa_pacing_rate_bps(const struct copa_auto_state *ca, u32 mss, u64 now_us)
{
    u64 rate;

    if (ca->drain_phase == COPA_PHASE_DRAINING && ca->drain_rate_bps > 0)
        return ca->drain_rate_bps;

    if (ca->standing_us == 0)
        return 0;

    /* 2 * cwnd_bytes / RTT_standing = 2 * (cwnd_q10 * mss / 1024) * 1e6 / standing_us */
    rate = div64_u64(((ca->cwnd_q10 * (u64)mss) >> 10) * 2000000ULL, ca->standing_us);
    if (ca->drain_phase == COPA_PHASE_RECOVERING && now_us < ca->recover_until_us &&
        ca->recover_rate_cap_bps > 0 && ca->recover_rate_cap_bps < rate) {
        rate = ca->recover_rate_cap_bps;
    }
    return rate;
}

static inline u64 copa_wanted_probe_dur(u64 srtt_us)
{
    return clamp_t(u64, 16ULL * srtt_us, COPA_PROBE_DUR_MIN_US, COPA_PROBE_DUR_MAX_US);
}

static inline u64 copa_fallback_every(u32 level)
{
    u32 shift = min_t(u32, level, 4U);
    /* 120 s = 6 slots, doubles up to 600 s = 30 slots */
    return min_t(u64, 6ULL << shift, 30ULL);
}

static inline bool copa_rtt_min_changed(u64 a_us, u64 b_us)
{
    u64 diff = (a_us > b_us) ? (a_us - b_us) : (b_us - a_us);
    u64 hi = max_t(u64, a_us, b_us);
    u64 thresh = max_t(u64, 5000ULL, hi / 5);
    return diff > thresh;
}

static inline void copa_unsettle(struct copa_auto_state *ca)
{
    ca->stable = false;
    ca->agreeing = 0;
    ca->reset_next = true;
    ca->fallback_level = 0;
}

static inline void copa_record_aligned_probe(struct copa_auto_state *ca, u64 probe_dq_us, u64 rtt_min_us)
{
    bool fresh = !ca->aligned_probe_seen || ca->reset_next;
    u64 old = ca->has_jitter ? ca->jitter_us : probe_dq_us;
    u64 j = (!fresh && ca->has_jitter) ? min_t(u64, old, probe_dq_us) : probe_dq_us;
    u64 tol_old = max_t(u64, 500ULL, old / 10);
    u64 tol_hi = max_t(u64, 500ULL, ca->probe_hi_us / 10);
    bool was_stable = ca->stable;

    if (ca->has_jitter && !fresh && ca->stable &&
        probe_dq_us > max_t(u64, (ca->probe_hi_us * 3) / 2, ca->probe_hi_us + tol_hi)) {
        copa_unsettle(ca);
    } else if (ca->has_jitter && !fresh && old >= j && (old - j) <= tol_old) {
        ca->agreeing++;
    } else {
        ca->agreeing = 0;
    }

    if (fresh) {
        ca->reset_next = false;
        ca->probe_hi_us = probe_dq_us;
    } else if (!ca->reset_next) {
        ca->probe_hi_us = max_t(u64, ca->probe_hi_us, probe_dq_us);
    }

    ca->stable = !ca->reset_next && (ca->agreeing + 1 >= 3);
    if (was_stable && ca->stable && ca->fallback_level < 4)
        ca->fallback_level++;

    ca->rtt_min_at_probe_us = rtt_min_us;
    ca->jitter_us = min_t(u64, j, COPA_MAX_JITTER_US);
    ca->has_jitter = true;
    ca->aligned_probe_seen = true;
}

/* Aggregate independent low-flight standing minima. A single RTT can seed
 * the floor, but cannot identify the mean excess above it. */
static inline void copa_observe_low(struct copa_auto_state *ca, u64 now,
                                    u64 rtt, bool low_flight)
{
    u64 base = ca->trusted_min_us;
    u64 span = max_t(u64, base / 2, 1000);
    if (!low_flight || !ca->low_samples || now - ca->low_window_us > 2 * span) {
        ca->low_samples = 0;
        ca->low_window_us = now;
        ca->low_min_us = rtt;
    }
    if (!low_flight)
        return;
    ca->low_min_us = min_t(u64, ca->low_min_us, rtt);
    ca->low_samples++;
    if (ca->low_samples < 2 || now - ca->low_window_us < span)
        return;
    ca->low_samples = 0;
    if (ca->low_min_us > base + ca->jitter_us + max_t(u64, 5000, base / 4)) {
        ca->raised_min_us = ca->raised_windows ? min_t(u64, ca->raised_min_us, ca->low_min_us) : ca->low_min_us;
        if (++ca->raised_windows >= 3) {
            bool fixed_j = copa_fixed_jitter(&ca->cfg);
            ca->trusted_min_us = ca->raised_min_us;
            copa_minmax_reset(&ca->rtt_min, now, ca->trusted_min_us);
            ca->raised_windows = 0;
            ca->has_jitter = fixed_j;
            ca->first_probe_done = fixed_j;
            ca->jitter_us = fixed_j ? copa_fixed_jitter_us(&ca->cfg) : 0;
            ca->pending_k_us = ca->ratio_us = ca->delta_run_q20 = 0;
            ca->stable = ca->aligned_probe_seen = false;
            ca->low_windows = ca->low_dq_sum_us = 0;
            copa_reset_segment(ca);
        }
        return;
    }
    ca->raised_windows = 0;
    if (copa_fixed_jitter(&ca->cfg))
        return;
    ca->low_dq_sum_us += ca->low_min_us > base ? ca->low_min_us - base : 0;
    if (++ca->low_windows == 3) {
        u64 j = min_t(u64, ca->low_dq_sum_us / 3, COPA_MAX_JITTER_US);
        ca->jitter_us = ca->has_jitter ? (3 * ca->jitter_us + j) / 4 : j;
        ca->has_jitter = ca->first_probe_done = true;
        ca->passive_j_at_us = now;
        ca->low_windows = ca->low_dq_sum_us = 0;
    }
}

static inline void copa_update_delta(struct copa_auto_state *ca,
                                     u64 seg_dq_us,
                                     u64 rtt_min_us,
                                     u64 bdp_pkts)
{
    u64 j = ca->jitter_us;
    u32 rho_milli;
    u64 pkt_time_us = (bdp_pkts > 1) ? div64_u64(rtt_min_us, bdp_pkts) : 0;
    u64 seg_stand = 0, round_stand = 0, raw_q, max_stand, stand_dq, eff_dq, floor_dq, queueing;
    u64 k_us, ratio_us, base_rtt_us, dq_target_us;
    s64 err_q16, step_q16;
    u32 base_delta_q20, next_delta_q20;

    if (ca->seg_acks == 0)
        return;

    /* rho in [50, 980] milli */
    rho_milli = (u32)clamp_t(u64,
                             1000ULL - div64_u64(1000ULL * ca->seg_idle, ca->seg_acks),
                             50ULL, 980ULL);

    if (ca->seg_min_rtt_us > rtt_min_us + j)
        seg_stand = ca->seg_min_rtt_us - rtt_min_us - j;
    if (j <= 2ULL * pkt_time_us)
        round_stand = div64_u64(ca->seg_stand_dq_sum_us, ca->seg_acks);

    raw_q = (seg_dq_us > j) ? (seg_dq_us - j) : 0;
    max_stand = (raw_q > (3ULL * pkt_time_us) / 2) ? (raw_q - (3ULL * pkt_time_us) / 2) : 0;
    if (max_stand > 8ULL * pkt_time_us)
        max_stand = 8ULL * pkt_time_us;
    stand_dq = min_t(u64, max_t(u64, seg_stand, round_stand), max_stand);
    eff_dq = (raw_q > stand_dq) ? (raw_q - stand_dq) : 0;

    /* In manual fixed-jitter mode, if delay-based idle detection saturates (>= 950)
     * without a large standing queue, fall back to rate-based utilization. */
    if (copa_fixed_jitter(&ca->cfg) && rho_milli >= 950 &&
        ca->seg_dur_us > 0 && ca->seg_bw_est_bps > 0 &&
        stand_dq * 10 < 3 * max_t(u64, raw_q, 10ULL)) {
        u64 seg_rate = div64_u64(ca->seg_deliv_bytes * 1000000ULL, ca->seg_dur_us);
        u32 rho_bw = (u32)clamp_t(u64, div64_u64(seg_rate * 1000ULL, ca->seg_bw_est_bps),
                                  50ULL, 980ULL);
        if (rho_bw < rho_milli)
            rho_milli = rho_bw;
    }

    if (bdp_pkts > 1 && j > 0 && rho_milli < 950 && seg_dq_us <= j + (j * 15) / 100) {
        floor_dq = max_t(u64, div64_u64(15ULL * j * (1000ULL - rho_milli), 100000ULL), 10ULL);
    } else {
        floor_dq = 10ULL; /* 10 us = 1e-5 s */
    }
    queueing = max_t(u64, eff_dq, floor_dq);

    /* k = queueing * (1 - rho) / rho [us] */
    k_us = max_t(u64, div64_u64(queueing * (1000ULL - rho_milli), rho_milli), 1ULL);

    {
        bool update_ratio = true;
        if (ca->ratio_us == 0) {
            if (rho_milli == 50 || rho_milli == 980) {
                /* Conservative prior so delta still adapts instead of freezing */
                ratio_us = max_t(u64, rtt_min_us / 20, 500ULL);
                update_ratio = false;
            } else if (!ca->pending_k_us) {
                ca->pending_k_us = k_us;
                ratio_us = k_us;
                update_ratio = false;
            } else {
                ratio_us = (ca->pending_k_us + k_us) / 2;
                ca->pending_k_us = 0;
            }
        } else if (rho_milli == 50 || rho_milli == 980) {
            ratio_us = ca->ratio_us;
            update_ratio = false;
        } else {
            /* weight in milli: default 300 (0.30); left-censored 75; right-censored 150 */
            u32 w_milli = 300;
            if (bdp_pkts > 1 && j > 1000ULL && seg_dq_us <= j && k_us < ca->ratio_us) {
                w_milli = 75;
            } else if (bdp_pkts > 1 && ca->seg_idle == 0 &&
                       stand_dq * 10 < 3 * max_t(u64, raw_q, 10ULL) &&
                       k_us < ca->ratio_us) {
                w_milli = 150;
            }
            /* log-domain EWMA: ratio_new = ratio * exp(w * ln(k / ratio)) */
            {
                s64 dln_q16 = copa_ln_ratio_q16(k_us, ca->ratio_us);
                s64 scaled = div64_s64(dln_q16 * (s64)w_milli, 1000LL);
                u64 factor_q16 = copa_exp_q16(scaled);
                ratio_us = max_t(u64, (ca->ratio_us * factor_q16) >> COPA_Q16_SHIFT, 1ULL);
            }
        }

        base_rtt_us = rtt_min_us + j;
        dq_target_us = copa_target_dq_us(j, ratio_us, base_rtt_us, ca->cfg.theta_milli);
        if (bdp_pkts > 1 && ca->cfg.theta_milli > 500)
            dq_target_us = min_t(u64, dq_target_us, j + (3ULL * base_rtt_us) / 2);

        /* Manual-jitter unreachable floor guard for high-jitter wireless links */
        if (copa_fixed_jitter(&ca->cfg) && rho_milli < 850 && ca->seg_min_rtt_us > rtt_min_us) {
            u64 seg_min_dq = ca->seg_min_rtt_us - rtt_min_us;
            dq_target_us = max_t(u64, dq_target_us, min_t(u64, seg_min_dq, 15000ULL));
        }

        /* Buffer-aware ceiling from observed non-RTO loss-edge queueing delay */
        if (ca->loss_dq_us > 0) {
            u64 buf_cap = max_t(u64, (ca->loss_dq_us * 3ULL) / 4ULL, 2000ULL);
            dq_target_us = min_t(u64, dq_target_us, buf_cap);
        }

        base_delta_q20 = ca->delta_run_q20 ? ca->delta_run_q20 : ca->delta_q20;
        err_q16 = copa_ln_ratio_q16(max_t(u64, seg_dq_us, 10ULL), max_t(u64, dq_target_us, 10ULL));
        err_q16 = clamp_t(s64, err_q16, -COPA_MAX_STEP_Q16, COPA_MAX_STEP_Q16);

        if (err_q16 < 0) {
            /* Additive increase of 1/delta (AIMD on 1/delta) */
            u64 far_frac_milli = 0;
            u64 slope_milli = 10000ULL; /* fair_increase = 10.0 */
            u64 inv_delta_q10;
            u64 add_q10;

            if (bdp_pkts > 30)
                far_frac_milli = min_t(u64, div64_u64((bdp_pkts - 30) * 1000ULL, 170ULL), 1000ULL);
            if (-err_q16 > COPA_FAR_RATIO_Q16)
                slope_milli = 10000ULL + far_frac_milli * 20ULL; /* up to 30.0 */

            /* 1/base_delta in Q10 = (1 << 30) / base_delta_q20 */
            inv_delta_q10 = div64_u64(1ULL << 30, base_delta_q20);
            /* -slope * err in Q10 = (slope_milli * (-err_q16)) / (1000 * 64) */
            add_q10 = div64_u64(slope_milli * (u64)(-err_q16), 64000ULL);
            inv_delta_q10 += add_q10;
            next_delta_q20 = (u32)div64_u64(1ULL << 30, max_t(u64, inv_delta_q10, 1ULL));
        } else {
            /* Multiplicative increase of delta (gain = 0.25) */
            step_q16 = err_q16 >> 2;
            next_delta_q20 = (u32)(((u64)base_delta_q20 * copa_exp_q16(step_q16)) >> COPA_Q16_SHIFT);
        }

        ca->rho_milli = rho_milli;
        if (update_ratio)
            ca->ratio_us = ratio_us;
        ca->base_rtt_us = base_rtt_us;
        ca->dq_target_us = dq_target_us;
        ca->delta_run_q20 = clamp_t(u32, next_delta_q20, COPA_DELTA_MIN_Q20, COPA_DELTA_MAX_Q20);
    }
}

static inline void copa_apply_segment(struct copa_auto_state *ca,
                                      u64 seg_dq_us,
                                      u64 rtt_min_us,
                                      u64 bdp_pkts)
{
    u64 floor_shift = (ca->seg_min_rtt_us > rtt_min_us) ? (ca->seg_min_rtt_us - rtt_min_us) : 0;
    u64 shift_thresh = max_t(u64, 5000ULL, (rtt_min_us * 15) / 100);
    bool shifted = (ca->delta_run_q20 > 0) &&
                   (floor_shift > ca->jitter_us + shift_thresh) &&
                   (seg_dq_us > 2ULL * max_t(u64, ca->dq_target_us, 5000ULL)) &&
                   (seg_dq_us < 2ULL * floor_shift);
    if (shifted) {
        ca->floor_shift_streak++;
        if (ca->floor_shift_streak >= 1)
            ca->handover_drain_pending = true;
    } else {
        ca->floor_shift_streak = 0;
        copa_update_delta(ca, seg_dq_us, rtt_min_us, bdp_pkts);
    }
}

static inline void copa_finish_drain(struct copa_auto_state *ca,
                                     u64 now_us,
                                     u64 srtt_us,
                                     u64 rtt_min_us,
                                     u32 mss)
{
    bool plateau = (ca->drain_flat_rounds >= 2) && !ca->drain_congested &&
                   (ca->drain_kind == COPA_DRAIN_REFRESH_RTT) &&
                   (ca->drain_min_rtt_us > 0);
    u64 shift_thresh = max_t(u64, 5000ULL, (rtt_min_us * 15) / 100);
    bool base_rose = plateau && ca->drain_min_rtt_us > rtt_min_us &&
                     (ca->drain_min_rtt_us - rtt_min_us) > shift_thresh;
    u64 new_base = base_rose ? ca->drain_min_rtt_us
                             : (ca->drain_min_rtt_us > 0 ? min_t(u64, rtt_min_us, ca->drain_min_rtt_us) : rtt_min_us);

    if (base_rose) {
        copa_minmax_reset(&ca->rtt_min, now_us, new_base);
        ca->floor_shift_streak = 0;
        ca->handover_drain_pending = false;
        if (copa_fixed_jitter(&ca->cfg)) {
            ca->has_jitter = true;
            ca->jitter_us = copa_fixed_jitter_us(&ca->cfg);
            ca->first_probe_done = true;
        }
    }
    ca->trusted_min_us = new_base;

    if (ca->drain_kind == COPA_DRAIN_MEASURE_J) {
        if (ca->drain_probe_acks > 0) {
            u64 mean_dq = div64_u64(ca->drain_probe_dq_sum_us, ca->drain_probe_acks);
            if (ca->drain_flat_rounds >= 2 && !ca->drain_congested &&
                ca->drain_min_rtt_us > rtt_min_us &&
                (ca->drain_min_rtt_us - rtt_min_us) > shift_thresh) {
                u64 shift = ca->drain_min_rtt_us - rtt_min_us;
                mean_dq = (mean_dq > shift) ? (mean_dq - shift) : 0;
            }
            if (!ca->first_probe_done) {
                ca->first_probe_done = true;
                if (!ca->has_jitter) {
                    ca->jitter_us = min_t(u64, mean_dq, COPA_MAX_JITTER_US);
                    ca->has_jitter = true;
                }
            }
            if (ca->drain_has_slot)
                copa_record_aligned_probe(ca, mean_dq, new_base);
        }
        ca->rounds -= ca->rounds % 8;
        copa_reset_segment(ca);
        ca->post_idle = false;
        if (ca->drain_has_slot) {
            ca->decided_slot = ca->drain_slot;
            ca->has_decided_slot = true;
            if (ca->has_probing_slot && ca->probing_slot == ca->drain_slot)
                ca->has_probing_slot = false;
        }
        ca->loaded_since_us = now_us;
        ca->drain_backoff_slots = 1;
    } else {
        u64 imp_tol = max_t(u64, 1000ULL, ca->drain_rtt_min_start_us / 50);
        if (ca->drain_rtt_min_start_us >= new_base + imp_tol)
            ca->drain_backoff_slots = 1;
        else
            ca->drain_backoff_slots = min_t(u64, ca->drain_backoff_slots * 2, 8ULL);
        if (ca->drain_has_slot)
            ca->next_drain_slot = ca->drain_slot + ca->drain_backoff_slots;
    }

    if (!ca->drain_congested && !copa_rtt_min_changed(ca->drain_rtt_min_start_us, new_base)) {
        u64 target_q10 = ca->drain_saved_cwnd_q10;
        u64 bw = copa_bw_estimate(ca, now_us, srtt_us);
        if (bw > 0 && mss > 0) {
            u64 bdp_q10 = div64_u64(bw * new_base, 1000000ULL * mss) << 10;
            if (target_q10 < bdp_q10)
                target_q10 = bdp_q10;
        }
        if (target_q10 > ca->cwnd_q10) {
            ca->cwnd_q10 = clamp_t(u64, target_q10,
                                   (u64)COPA_MIN_CWND_PKTS << 10,
                                   (u64)COPA_MAX_CWND_PKTS << 10);
            copa_reset_velocity(ca);
        }
    }

    if (ca->drain_saved_rate_bps > 0 && srtt_us > 0 &&
        srtt_us <= 4 * max_t(u64, ca->standing_us, 1000)) {
        ca->drain_phase = COPA_PHASE_RECOVERING;
        ca->recover_until_us = now_us + srtt_us;
        ca->recover_rate_cap_bps = ca->drain_saved_rate_bps;
    } else {
        ca->drain_phase = COPA_PHASE_NORMAL;
    }
}

static inline void copa_start_drain(struct copa_auto_state *ca,
                                    u8 kind,
                                    bool has_slot,
                                    u64 slot,
                                    u64 now_us,
                                    u64 srtt_us,
                                    u64 rtt_min_us,
                                    u64 standing_us,
                                    u64 dq_us,
                                    u32 mss,
                                    u64 max_dur_us,
                                    bool hold_full)
{
    u64 trusted = (ca->trusted_min_us > 0) ? min_t(u64, ca->trusted_min_us, rtt_min_us) : rtt_min_us;
    u64 stand = max_t(u64, standing_us, max_t(u64, trusted, 1000ULL));
    u64 cwnd_bytes = (ca->cwnd_q10 * mss) >> 10;
    u64 cwnd_rate = div64_u64(cwnd_bytes * 1000000ULL, stand);
    u64 bw = copa_bw_estimate(ca, now_us, srtt_us);
    u64 min_bytes = (u64)COPA_MIN_CWND_PKTS * mss;
    u64 eff_bw = bw > 0 ? min_t(u64, bw, (cwnd_rate * 3) / 2) : cwnd_rate;
    u64 min_rate = div64_u64(min_bytes * 1000000ULL, max_t(u64, srtt_us, 10000ULL));
    u64 gain_pct = (kind == COPA_DRAIN_MEASURE_J) ? 78ULL : (dq_us > 20000ULL ? 75ULL : 85ULL);
    u64 bdp_bytes, soft_q10, dl;

    if (eff_bw < div64_u64(min_bytes * 1000000ULL, stand))
        eff_bw = div64_u64(min_bytes * 1000000ULL, stand);

    ca->drain_saved_rate_bps = copa_pacing_rate_bps(ca, mss, now_us);
    ca->drain_saved_cwnd_q10 = ca->cwnd_q10;
    ca->drain_rate_bps = max_t(u64, (eff_bw * gain_pct) / 100ULL, min_rate);
    bdp_bytes = div64_u64(eff_bw * trusted, 1000000ULL);
    soft_q10 = div64_u64((bdp_bytes * 9) / 10, max_t(u32, mss, 1U)) << 10;
    ca->drain_soft_cwnd_q10 = min_t(u64, max_t(u64, soft_q10, (u64)COPA_MIN_CWND_PKTS << 10), ca->cwnd_q10);

    if (kind == COPA_DRAIN_REFRESH_RTT) {
        dl = max_t(u64, max_t(u64, max_dur_us, 150000ULL), max_t(u64, 4ULL * srtt_us, 2ULL * dq_us + srtt_us));
    } else {
        dl = max_t(u64, max_dur_us, 4ULL * srtt_us);
    }

    ca->drain_phase = COPA_PHASE_DRAINING;
    ca->drain_kind = kind;
    ca->drain_has_slot = has_slot;
    ca->drain_slot = slot;
    ca->drain_start_us = now_us;
    ca->drain_deadline_us = now_us + min_t(u64, dl, COPA_PROBE_DUR_MAX_US);
    ca->drain_idle_at_us = 0;
    ca->drain_measure_until_us = 0;
    ca->drain_rtt_min_start_us = rtt_min_us;
    ca->drain_trusted_start_us = trusted;
    ca->drain_trusted_verified = (ca->trusted_min_us > 0);
    ca->drain_hold_full = hold_full;
    ca->drain_min_rtt_us = 0;
    ca->drain_round_start_us = now_us;
    ca->drain_prev_min_rtt_us = 0;
    ca->drain_flat_rounds = 0;
    ca->drain_congested = false;
    ca->drain_probe_dq_sum_us = 0;
    ca->drain_probe_acks = 0;
}

static inline void copa_on_loss(struct copa_auto_state *ca, u64 now_us, bool persistent)
{
    if (ca->drain_phase == COPA_PHASE_DRAINING && (persistent || ca->cfg.mode_switching))
        ca->drain_congested = true;

    if (persistent) {
        ca->valid_after_us = now_us;
        ca->loaded_since_us = 0;
        ca->low_samples = 0;
        ca->seg_probe = true;
        ca->drain_phase = COPA_PHASE_NORMAL;
        ca->cwnd_q10 = (u64)COPA_MIN_CWND_PKTS << 10;
        ca->slow_start = true;
        ca->ss_confirm_since_us = 0;
        ca->peak_bdp_pkts_q10 = 0;
        copa_reset_velocity(ca);
        return;
    }

    if (!ca->competitive) {
        if (!ca->cfg.adaptive)
            return;
        if (ca->last_loss_us > 0 && now_us < ca->last_loss_us + max_t(u64, ca->srtt_us, 10000ULL))
            return;
        /* Gentle shallow-buffer loss brake in default (non-competitive) mode:
         * exit slow start immediately, pull cwnd back mildly, learn loss-edge dq,
         * and softly nudge delta upward so shallow buffers don't cascade-drop. */
        ca->slow_start = false;
        ca->ss_confirm_since_us = 0;
        {
            u64 target_q10 = (ca->cwnd_q10 * 85ULL) / 100ULL;
            if (ca->last_bdp_pkts > 0) {
                u64 bdp_cap = max_t(u64, (ca->last_bdp_pkts * 11ULL) / 10ULL, COPA_MIN_CWND_PKTS) << 10;
                if (target_q10 > bdp_cap)
                    target_q10 = bdp_cap;
            }
            ca->cwnd_q10 = max_t(u64, target_q10, (u64)COPA_MIN_CWND_PKTS << 10);
            copa_reset_velocity(ca);
        }
        {
            u64 obs_dq = max_t(u64, ca->dq_us, 2000ULL);
            ca->loss_dq_us = ca->loss_dq_us ? (3ULL * ca->loss_dq_us + obs_dq) / 4ULL : obs_dq;
            ca->last_loss_relax_us = now_us;
        }
        {
            u32 cur_d = ca->delta_run_q20 ? ca->delta_run_q20 : ca->delta_q20;
            ca->delta_run_q20 = clamp_t(u32, (cur_d * 5U) / 4U, COPA_DELTA_MIN_Q20, COPA_DELTA_MAX_Q20);
        }
        ca->last_loss_us = now_us;
        return;
    }
    if (ca->last_delta_dec_us > 0 && now_us < ca->last_delta_dec_us + ca->srtt_us)
        return;
    {
        u32 ceil = ca->cfg.adaptive ? COPA_DELTA_MAX_Q20 : ca->cfg.init_delta_q20;
        ca->delta_q20 = min_t(u32, ca->delta_q20 * 2, ceil);
        ca->last_delta_dec_us = now_us;
    }
}

static inline void copa_update_mode(struct copa_auto_state *ca, u64 now_us, u64 dq_us, u64 rtt_min_us)
{
    u64 rtt_max_us = copa_minmax_get(&ca->rtt_max);
    u64 span_us = (rtt_max_us > rtt_min_us) ? (rtt_max_us - rtt_min_us) : 0;
    bool nearly_empty;
    bool comp;

    if (!ca->cfg.mode_switching)
        return;

    if (ca->cfg.adaptive) {
        if (ca->has_jitter && ca->jitter_us < 15000ULL) {
            u64 eff_dq = (dq_us > ca->jitter_us) ? (dq_us - ca->jitter_us) : 0;
            u64 eff_span = (span_us > ca->jitter_us) ? (span_us - ca->jitter_us) : 0;
            nearly_empty = (eff_dq <= COPA_IDLE_MARGIN_US) || (eff_dq * 10 < eff_span);
        } else {
            nearly_empty = (dq_us == 0) || (dq_us * 10 < span_us);
        }
        if (!ca->competitive &&
            (ca->last_ack_app_limited ||
             (ca->dq_target_us > 0 && dq_us * 100 <= 115ULL * ca->dq_target_us))) {
            nearly_empty = true;
        }
    } else {
        nearly_empty = (dq_us == 0) || (dq_us * 10 < span_us);
    }

    if (nearly_empty)
        ca->last_nearly_empty_us = now_us;

    comp = (now_us > ca->last_nearly_empty_us) &&
           ((now_us - ca->last_nearly_empty_us) > 5ULL * ca->srtt_us);
    if (comp != ca->competitive) {
        u32 ceil = ca->cfg.adaptive ? COPA_DELTA_MAX_Q20 : ca->cfg.init_delta_q20;
        ca->competitive = comp;
        if (comp && ca->cfg.adaptive && ca->peak_bdp_pkts_q10 > (150ULL << 10)) {
            /* High-BDP clamp >= 0.02 */
            ca->delta_q20 = clamp_t(u32, ca->delta_q20, 20972U, ceil);
        } else {
            ca->delta_q20 = ceil;
        }
        ca->last_delta_dec_us = 0;
    }
}

/*
 * Main per-ACK congestion control step.
 * Returns updated congestion window in packets.
 */
static inline u32 copa_on_ack(struct copa_auto_state *ca,
                              u64 now_us,
                              u64 rtt_sample_us,
                              u64 srtt_us,
                              u32 acked_pkts,
                              u32 mss,
                              u64 rate_bps,
                              bool app_limited, bool low_flight)
{
    u64 prev_time = ca->last_time_us;
    u64 idle_thresh = max_t(u64, 4ULL * max_t(u64, srtt_us, 10000ULL), 300000ULL);
    u64 min_rtt_win, rtt_min_us, standing_us, dq_us, bw, bdp_pkts;
    bool recent_app_limited, holding, excluded, increase;

    if (now_us > ca->last_time_us)
        ca->last_time_us = now_us;
    if (srtt_us == 0)
        srtt_us = rtt_sample_us;
    ca->srtt_us = srtt_us;

    if (ca->cfg.adaptive) {
        u64 base = ca->trusted_min_us;
        if (base && now_us - prev_time >= max_t(u64, 300000, 8 * base)) {
            /* A small RTT after an application pause is already fresh. */
            ca->valid_after_us = rtt_sample_us > 4 * base ? now_us : prev_time;
            ca->drain_phase = COPA_PHASE_NORMAL;
            ca->loaded_since_us = 0;
            ca->low_samples = 0;
            ca->seg_probe = true;
        }
        if (!rtt_sample_us || (ca->valid_after_us &&
            rtt_sample_us > now_us - ca->valid_after_us))
            return (u32)(ca->cwnd_q10 >> 10);
        if (!base || rtt_sample_us < base)
            ca->trusted_min_us = rtt_sample_us;
        /* A write boundary can mark individual ACKs app-limited even while
         * a bulk transfer keeps the bottleneck busy across many RTTs. */
        if (srtt_us > 4 * rtt_sample_us ||
            (ca->last_loaded_ack_us && now_us - ca->last_loaded_ack_us >
             max_t(u64, 2 * ca->trusted_min_us, 50000))) {
            ca->loaded_since_us = 0;
        }
        if (!app_limited && srtt_us <= 4 * rtt_sample_us) {
            ca->last_loaded_ack_us = now_us;
            if (!ca->loaded_since_us)
                ca->loaded_since_us = now_us;
        }
        copa_observe_low(ca, now_us, rtt_sample_us, low_flight &&
                         ca->drain_phase == COPA_PHASE_NORMAL && !ca->competitive &&
                         srtt_us <= 4 * rtt_sample_us);
    }

    /* Application-idle restart (gentle idle-decayed BDP resume) */
    if (ca->cfg.adaptive && !ca->slow_start && !ca->competitive &&
        ca->last_ack_app_limited && now_us >= prev_time &&
        (now_us - prev_time) >= idle_thresh) {
        u64 gap_us = now_us - prev_time;
        u64 decayed = ca->peak_bdp_pkts_q10;
        u64 floor_q10;
        bool recent_loss = ca->last_loss_us > 0 && now_us < ca->last_loss_us + 2000000ULL;

        if (gap_us > 1000000ULL)
            decayed = (decayed * 7ULL) / 10ULL;
        if (gap_us > 2000000ULL)
            decayed = (decayed * 7ULL) / 10ULL;
        ca->peak_bdp_pkts_q10 = decayed;
        ca->drain_phase = COPA_PHASE_NORMAL;
        floor_q10 = max_t(u64, 10ULL << 10, decayed / 2ULL);
        if (floor_q10 > ca->cwnd_q10)
            ca->cwnd_q10 = min_t(u64, floor_q10, (u64)COPA_MAX_CWND_PKTS << 10);
        if (!recent_loss && (decayed == 0 || ca->cwnd_q10 < (decayed * 3ULL) / 4ULL)) {
            ca->slow_start = true;
            ca->ss_confirm_since_us = 0;
        }
        copa_reset_velocity(ca);
        ca->last_nearly_empty_us = now_us;
    }

    ca->last_ack_app_limited = app_limited;
    if (app_limited) {
        ca->seen_app_limited = true;
        ca->last_app_limited_us = now_us;
    }

    /* Poll drain / recovery expiry */
    if (ca->drain_phase == COPA_PHASE_RECOVERING && now_us >= ca->recover_until_us) {
        ca->drain_phase = COPA_PHASE_NORMAL;
    } else if (ca->drain_phase == COPA_PHASE_DRAINING && now_us >= ca->drain_deadline_us) {
        u64 rm = ca->trusted_min_us;
        copa_finish_drain(ca, now_us, srtt_us, rm ? rm : ca->drain_rtt_min_start_us, mss);
    }

    recent_app_limited = ca->seen_app_limited && now_us >= ca->last_app_limited_us &&
                         (now_us - ca->last_app_limited_us) <= COPA_MIN_RTT_WIN_US;
    min_rtt_win = (ca->cfg.adaptive && recent_app_limited)
                      ? clamp_t(u64, 32ULL * srtt_us, 1000000ULL, COPA_MIN_RTT_WIN_US)
                      : COPA_MIN_RTT_WIN_US;

    rtt_min_us = copa_minmax_running_min(&ca->rtt_min, min_rtt_win, now_us, rtt_sample_us);
    if (ca->cfg.adaptive)
        rtt_min_us = ca->trusted_min_us;
    standing_us = copa_minmax_running_min(&ca->rtt_standing, max_t(u64, srtt_us / 2, 1000ULL),
                                          now_us, rtt_sample_us);
    copa_minmax_running_max(&ca->rtt_max, max_t(u64, 4ULL * srtt_us, 1000ULL),
                            now_us, rtt_sample_us);

    if (rtt_min_us == 0 || standing_us == 0)
        return (u32)(ca->cwnd_q10 >> 10);

    ca->standing_us = standing_us;
    dq_us = (standing_us > rtt_min_us) ? (standing_us - rtt_min_us) : 0;
    ca->dq_us = dq_us;

    copa_bw_update(ca, now_us, srtt_us, rate_bps, app_limited);
    bw = copa_bw_estimate(ca, now_us, srtt_us);
    bdp_pkts = (bw > 0 && mss > 0) ? div64_u64(bw * rtt_min_us, 1000000ULL * mss) : 0;
    if (bdp_pkts > 0)
        ca->last_bdp_pkts = bdp_pkts;
    if (!app_limited && bdp_pkts > 0) {
        u64 bdp_q10 = bdp_pkts << 10;
        ca->peak_bdp_pkts_q10 = max_t(u64, (ca->peak_bdp_pkts_q10 * 85) / 100, bdp_q10);
    }

    /* Slowly relax shallow-buffer loss ceiling after sustained loss-free operation */
    if (ca->loss_dq_us > 0 && now_us >= ca->last_loss_relax_us + 5000000ULL) {
        ca->loss_dq_us = (ca->loss_dq_us * 11ULL) / 10ULL;
        ca->last_loss_relax_us = now_us;
        if (ca->last_loss_us > 0 && now_us >= ca->last_loss_us + 15000000ULL &&
            ca->loss_dq_us > 2ULL * rtt_min_us) {
            ca->loss_dq_us = 0;
        }
    }

    holding = (ca->drain_phase == COPA_PHASE_DRAINING) ||
              (!ca->competitive && ca->cfg.adaptive && (ca->slow_start || ca->delta_run_q20 == 0));
    if (holding)
        ca->last_nearly_empty_us = now_us;
    else
        copa_update_mode(ca, now_us, dq_us, rtt_min_us);

    if (ca->cfg.adaptive) {
        if (ca->trusted_min_us > 0) {
            ca->trusted_min_us = min_t(u64, ca->trusted_min_us, rtt_sample_us);
            if (app_limited && dq_us <= max_t(u64, 2000ULL, ca->trusted_min_us / 20))
                ca->last_app_idle_us = now_us;
        }
        excluded = (ca->drain_phase != COPA_PHASE_NORMAL) || ca->competitive;

        /* AutoDelta round & segment accounting */
        if (ca->round_start_us == 0)
            ca->round_start_us = now_us;
        if (app_limited) {
            ca->in_app_idle = true;
            ca->round_app_limited = true;
        } else {
            if (!ca->in_app_idle && ca->last_ack_us > 0 && now_us > ca->last_ack_us) {
                u64 gap = now_us - ca->last_ack_us;
                if (gap > ca->round_max_gap_us)
                    ca->round_max_gap_us = gap;
            }
            ca->in_app_idle = false;
        }
        ca->last_ack_us = now_us;
        if (excluded || srtt_us > 4 * rtt_sample_us)
            ca->seg_probe = true;

        /* Board coordination for aligned 20 s slots */
        if (ca->rtt_min_at_probe_us > 0 && ca->stable &&
            copa_rtt_min_changed(ca->rtt_min_at_probe_us, rtt_min_us)) {
            copa_unsettle(ca);
        }
        if (!ca->slow_start && ca->board && !copa_fixed_jitter(&ca->cfg)) {
            u64 slot = div64_u64(now_us, COPA_PROBE_PERIOD_US);
            u64 phase = now_us - slot * COPA_PROBE_PERIOD_US;
            u64 own = copa_wanted_probe_dur(srtt_us);
            if (ca->first_probe_done && phase >= COPA_PROBE_DUR_MAX_US) {
                u32 idx = (u32)((slot + 1) & 3);
                bool want = !ca->stable || (((slot + 1) % copa_fallback_every(ca->fallback_level)) == 0);
                if (want && ca->board->requests[idx] < slot + 2)
                    ca->board->requests[idx] = slot + 2;
                if (ca->board->length_slot[idx] != slot + 2) {
                    ca->board->length_slot[idx] = slot + 2;
                    ca->board->length_us[idx] = own;
                } else if (own > ca->board->length_us[idx]) {
                    ca->board->length_us[idx] = own;
                }
            }
            if ((!ca->has_decided_slot || ca->decided_slot != slot) && phase < COPA_PROBE_DUR_MAX_US) {
                u32 idx = (u32)(slot & 3);
                bool want = !ca->first_probe_done || !ca->stable ||
                            ((slot % copa_fallback_every(ca->fallback_level)) == 0);
                u64 len = own;
                ca->decided_slot = slot;
                ca->has_decided_slot = true;
                if (want && ca->board->requests[idx] < slot + 1)
                    ca->board->requests[idx] = slot + 1;
                if (ca->board->length_slot[idx] == slot + 1 && ca->board->length_us[idx] > len)
                    len = ca->board->length_us[idx];
                ca->slot_duration_us = len;
                if (ca->board->requests[idx] == slot + 1 || (slot % 30 == 0)) {
                    ca->probing_slot = slot;
                    ca->has_probing_slot = true;
                }
            }
        }

        ca->round_dq_sum_us += dq_us;
        ca->round_acks++;
        ca->round_deliv_bytes += (u64)acked_pkts * (u64)mss;
        if (ca->round_min_rtt_us == 0 || rtt_sample_us < ca->round_min_rtt_us)
            ca->round_min_rtt_us = rtt_sample_us;
        if (ca->has_jitter &&
            rtt_sample_us <= rtt_min_us + ca->jitter_us + COPA_IDLE_MARGIN_US) {
            ca->round_idle++;
        }

        if (now_us >= ca->round_start_us + srtt_us && ca->round_acks >= 2) {
            if (!ca->slow_start) {
                if (ca->round_app_limited) {
                    if (ca->settle_halves == 0 && !ca->competitive && !ca->seg_probe &&
                        ca->seg_rounds >= 3 && ca->seg_acks >= 6 &&
                        ca->seg_max_gap_us < max_t(u64, srtt_us, 50000ULL) &&
                        ca->has_jitter) {
                        u64 seg_dq = div64_u64(ca->seg_dq_sum_us, ca->seg_acks);
                        copa_apply_segment(ca, seg_dq, rtt_min_us, bdp_pkts);
                    }
                    copa_reset_segment(ca);
                    ca->rounds -= ca->rounds % 8;
                    ca->post_idle = true;
                } else {
                    u64 settle_target = ca->post_idle ? 1ULL : 3ULL;
                    if ((ca->rounds % 8) >= settle_target) {
                        if (ca->round_idle == 0 && ca->has_jitter && ca->round_min_rtt_us > rtt_min_us) {
                            u64 r_min_dq = ca->round_min_rtt_us - rtt_min_us;
                            u64 r_mean_dq = div64_u64(ca->round_dq_sum_us, ca->round_acks);
                            u64 r_stand = (r_min_dq > ca->jitter_us) ? (r_min_dq - ca->jitter_us) : 0;
                            u64 r_cap = (r_mean_dq > ca->jitter_us) ? (r_mean_dq - ca->jitter_us) : 0;
                            if (r_stand > r_cap)
                                r_stand = r_cap;
                            ca->seg_stand_dq_sum_us += r_stand * ca->round_acks;
                        }
                        ca->seg_dq_sum_us += ca->round_dq_sum_us;
                        ca->seg_acks += ca->round_acks;
                        ca->seg_idle += ca->round_idle;
                        ca->seg_deliv_bytes += ca->round_deliv_bytes;
                        ca->seg_dur_us += (now_us > ca->round_start_us) ? (now_us - ca->round_start_us) : srtt_us;
                        if (bw > ca->seg_bw_est_bps)
                            ca->seg_bw_est_bps = bw;
                        if (ca->round_max_gap_us > ca->seg_max_gap_us)
                            ca->seg_max_gap_us = ca->round_max_gap_us;
                        if (ca->round_min_rtt_us > 0 &&
                            (ca->seg_min_rtt_us == 0 || ca->round_min_rtt_us < ca->seg_min_rtt_us)) {
                            ca->seg_min_rtt_us = ca->round_min_rtt_us;
                        }
                        ca->seg_rounds++;
                    }
                    ca->rounds++;
                    if ((ca->rounds % 8) != 0) {
                        /* Mid-period capacity surge early flush */
                        if (ca->settle_halves == 0 && !ca->competitive && !ca->seg_probe &&
                            ca->seg_rounds >= 3 && ca->seg_acks >= 6 &&
                            ca->seg_max_gap_us < max_t(u64, srtt_us, 50000ULL) &&
                            ca->dq_target_us > 0 && ca->has_jitter) {
                            u64 seg_dq = div64_u64(ca->seg_dq_sum_us, ca->seg_acks);
                            if (seg_dq * 100 < 35ULL * ca->dq_target_us) {
                                copa_apply_segment(ca, seg_dq, rtt_min_us, bdp_pkts);
                                copa_reset_segment(ca);
                                ca->rounds -= ca->rounds % 8;
                                ca->post_idle = true;
                            }
                        }
                    } else {
                        bool settling_after = (ca->settle_halves > 0);
                        if (settling_after)
                            ca->settle_halves--;
                        if (ca->seg_acks > 0 && !settling_after && !ca->competitive &&
                            !ca->seg_probe && !ca->seg_app_limited &&
                            ca->seg_max_gap_us < max_t(u64, srtt_us, 50000ULL) &&
                            ca->has_jitter) {
                            u64 seg_dq = div64_u64(ca->seg_dq_sum_us, ca->seg_acks);
                            copa_apply_segment(ca, seg_dq, rtt_min_us, bdp_pkts);
                        }
                        copa_reset_segment(ca);
                        ca->post_idle = false;
                    }
                }
            }
            ca->round_start_us = now_us;
            ca->round_dq_sum_us = 0;
            ca->round_acks = 0;
            ca->round_idle = 0;
            ca->round_max_gap_us = 0;
            ca->round_min_rtt_us = 0;
            ca->round_deliv_bytes = 0;
            ca->round_app_limited = false;
        }
    }

    if (!ca->competitive) {
        u32 fb = (ca->slow_start || ca->startup_delta_q20 == 0) ? ca->cfg.init_delta_q20 : ca->startup_delta_q20;
        ca->delta_q20 = (ca->cfg.adaptive && ca->delta_run_q20 > 0) ? ca->delta_run_q20 : fb;

        /* Drive DrainController */
        if (ca->cfg.adaptive) {
            if (ca->drain_phase == COPA_PHASE_DRAINING) {
                u64 elapsed = (now_us > ca->drain_start_us) ? (now_us - ca->drain_start_us) : 0;
                u64 min_hold = max_t(u64, srtt_us, 20000ULL);
                bool sampling, done;

                if (ca->drain_min_rtt_us == 0 || rtt_sample_us < ca->drain_min_rtt_us)
                    ca->drain_min_rtt_us = rtt_sample_us;
                if (now_us >= ca->drain_round_start_us + max_t(u64, srtt_us, 10000ULL)) {
                    if (ca->drain_prev_min_rtt_us > 0) {
                        u64 tol = max_t(u64, 1000ULL, ca->drain_min_rtt_us / 50);
                        if (ca->drain_prev_min_rtt_us <= ca->drain_min_rtt_us + tol)
                            ca->drain_flat_rounds++;
                        else
                            ca->drain_flat_rounds = 0;
                    }
                    ca->drain_prev_min_rtt_us = ca->drain_min_rtt_us;
                    ca->drain_round_start_us = now_us;
                }
                if (ca->drain_idle_at_us == 0 && elapsed >= min_hold) {
                    u64 j = ca->has_jitter ? ca->jitter_us : 0;
                    u64 btol = max_t(u64, 1000ULL, ca->drain_trusted_start_us / 20);
                    bool back = ca->drain_trusted_verified &&
                                standing_us <= ca->drain_trusted_start_us + j + btol;
                    bool plat = elapsed >= 2ULL * srtt_us && ca->drain_flat_rounds >= 2;
                    if (back || plat) {
                        ca->drain_idle_at_us = now_us;
                        if (ca->drain_kind == COPA_DRAIN_MEASURE_J && !ca->drain_hold_full) {
                            u64 slen = ca->drain_has_slot
                                           ? clamp_t(u64, 5ULL * srtt_us, 100000ULL, 600000ULL)
                                           : clamp_t(u64, 2ULL * srtt_us, 60000ULL, 200000ULL);
                            ca->drain_measure_until_us = min_t(u64, now_us + slen, ca->drain_deadline_us);
                        }
                    }
                }
                sampling = (ca->drain_kind == COPA_DRAIN_MEASURE_J) &&
                           ca->drain_idle_at_us > 0 && !ca->drain_congested;
                if (sampling) {
                    ca->drain_probe_dq_sum_us += dq_us;
                    ca->drain_probe_acks++;
                }
                done = (ca->drain_kind == COPA_DRAIN_REFRESH_RTT)
                           ? (ca->drain_idle_at_us > 0 || now_us >= ca->drain_deadline_us)
                           : (now_us >= ca->drain_deadline_us ||
                              (ca->drain_measure_until_us > 0 && now_us >= ca->drain_measure_until_us));
                if (done) {
                    copa_finish_drain(ca, now_us, srtt_us, rtt_min_us, mss);
                } else {
                    /* Smooth 1-SRTT cwnd ramp */
                    u64 ramp = max_t(u64, srtt_us, 10000ULL);
                    u64 frac = min_t(u64, div64_u64(elapsed * 1024ULL, ramp), 1024ULL);
                    ca->cwnd_q10 = ((ca->drain_saved_cwnd_q10 * (1024ULL - frac)) +
                                    (ca->drain_soft_cwnd_q10 * frac)) >> 10;
                    ca->cwnd_q10 = max_t(u64, ca->cwnd_q10, (u64)COPA_MIN_CWND_PKTS << 10);
                    return (u32)(ca->cwnd_q10 >> 10);
                }
            } else if (ca->drain_phase == COPA_PHASE_NORMAL && !app_limited && ca->loaded_since_us &&
                       now_us - ca->loaded_since_us >= max_t(u64, 3 * srtt_us, 200000) &&
                       (!ca->passive_j_at_us || now_us - ca->passive_j_at_us >= COPA_DRAIN_SKIP_US)) {
                if (ca->handover_drain_pending) {
                    ca->handover_drain_pending = false;
                    copa_start_drain(ca, COPA_DRAIN_REFRESH_RTT, false, 0,
                                     now_us, srtt_us, rtt_min_us, standing_us, dq_us,
                                     mss, max_t(u64, 3ULL * srtt_us, 120000ULL), false);
                    return (u32)(ca->cwnd_q10 >> 10);
                }
                if (!copa_fixed_jitter(&ca->cfg) && !ca->slow_start && !ca->first_probe_done) {
                    u64 max_dur = clamp_t(u64, 4ULL * srtt_us, 120000ULL, 350000ULL);
                    copa_start_drain(ca, COPA_DRAIN_MEASURE_J, false, 0,
                                     now_us, srtt_us, rtt_min_us, standing_us, dq_us,
                                     mss, max_dur, false);
                    return (u32)(ca->cwnd_q10 >> 10);
                }
                if (!copa_fixed_jitter(&ca->cfg) && !ca->slow_start && ca->first_probe_done) {
                    u64 slot = div64_u64(now_us, COPA_PROBE_PERIOD_US);
                    u64 phase = now_us - slot * COPA_PROBE_PERIOD_US;
                    if ((!ca->has_evaluated_slot || ca->evaluated_slot != slot) &&
                        ca->has_probing_slot && ca->probing_slot == slot &&
                        phase < ca->slot_duration_us) {
                        bool hold_full = ca->slot_duration_us > copa_wanted_probe_dur(srtt_us) + 1000ULL;
                        ca->evaluated_slot = slot;
                        ca->has_evaluated_slot = true;
                        copa_start_drain(ca, COPA_DRAIN_MEASURE_J, true, slot,
                                         now_us, srtt_us, rtt_min_us, standing_us, dq_us,
                                         mss, ca->slot_duration_us, hold_full);
                        return (u32)(ca->cwnd_q10 >> 10);
                    }
                    if ((!ca->has_evaluated_slot || ca->evaluated_slot != slot) &&
                        ca->has_decided_slot && ca->decided_slot == slot &&
                        phase < ca->slot_duration_us &&
                        (!ca->has_probing_slot || ca->probing_slot != slot)) {
                        bool board_req = ca->board && (ca->board->drain_requests[slot & 3] == slot + 1);
                        bool skip_idle = ca->last_app_idle_us > 0 &&
                                         now_us < ca->last_app_idle_us + COPA_DRAIN_SKIP_US;
                        bool drifted = (ca->trusted_min_us == 0) ||
                                       (rtt_min_us > ca->trusted_min_us + max_t(u64, 1000ULL, ca->trusted_min_us / 50));
                        ca->evaluated_slot = slot;
                        ca->has_evaluated_slot = true;
                        if (board_req || (slot >= ca->next_drain_slot && !skip_idle && drifted)) {
                            if (ca->board && ca->board->drain_requests[slot & 3] < slot + 1)
                                ca->board->drain_requests[slot & 3] = slot + 1;
                            copa_start_drain(ca, COPA_DRAIN_REFRESH_RTT, true, slot,
                                             now_us, srtt_us, rtt_min_us, standing_us, dq_us,
                                             mss, 50000ULL, false);
                            return (u32)(ca->cwnd_q10 >> 10);
                        }
                    }
                }
            }
        }
    } else {
        /* Competitive mode: abort drain; AIMD on 1/delta with deep-buffer cap */
        u64 bloat_ceil = max_t(u64, 3ULL * rtt_min_us, 90000ULL);
        ca->drain_phase = COPA_PHASE_NORMAL;
        /* 0.05 in Q20 = 52429 */
        if (ca->cfg.adaptive && ca->delta_q20 < 52429U && dq_us > bloat_ceil) {
            if (dq_us > (bloat_ceil * 3) / 2)
                ca->delta_q20 = min_t(u32, (ca->delta_q20 * 105U) / 100U, 52429U);
        } else {
            u64 inv_q10 = div64_u64(1ULL << 30, max_t(u32, ca->delta_q20, 1U));
            u64 add_q10 = div64_u64((u64)acked_pkts << 20, max_t(u64, ca->cwnd_q10, 1024ULL));
            ca->delta_q20 = (u32)div64_u64(1ULL << 30, max_t(u64, inv_q10 + add_q10, 1ULL));
            if (ca->delta_q20 < COPA_DELTA_MIN_Q20)
                ca->delta_q20 = COPA_DELTA_MIN_Q20;
        }
    }

    /* Target rate vs current rate */
    if (dq_us == 0) {
        increase = true;
    } else {
        /* target = 1 / (delta * dq) [pkts/s] = (1<<20)*1e6 / (delta_q20 * dq_us)
         * current = cwnd_pkts / eff_standing = (cwnd_q10 / 1024) * 1e6 / eff_standing_us
         * target >= current  <=>  eff_standing_us * (1<<30) >= cwnd_q10 * delta_q20 * dq_us
         */
        u64 eff_standing_us = standing_us;
        if (!ca->competitive && ca->cfg.adaptive &&
            (recent_app_limited || dq_us > 200000ULL)) {
            u64 max_q = max_t(u64, max_t(u64, (3ULL * ca->dq_target_us) / 2, rtt_min_us / 2), 20000ULL);
            eff_standing_us = min_t(u64, standing_us, rtt_min_us + max_q);
        }
        /* Compare target_pkts_q10 = (eff_standing_us << 30) / (delta_q20 * dq_us) >= cwnd_q10 */
        {
            u64 denom = (u64)ca->delta_q20 * dq_us;
            u64 target_q10 = denom > 0 ? div64_u64(eff_standing_us << 30, denom) : ((u64)COPA_MAX_CWND_PKTS << 10);
            increase = (target_q10 >= ca->cwnd_q10);
        }
    }

    if (app_limited) {
        if (increase)
            return (u32)(ca->cwnd_q10 >> 10);
        if (ca->cfg.adaptive) {
            u64 max_tail = max_t(u64, max_t(u64, 2ULL * ca->dq_target_us, rtt_min_us / 2), 30000ULL);
            if (dq_us <= max_tail)
                return (u32)(ca->cwnd_q10 >> 10);
        }
    }

    /* Velocity update (Paper rule, once per SRTT) */
    if (!(increase && ca->slow_start)) {
        if (ca->last_rec_time_us == 0) {
            ca->last_rec_time_us = now_us;
            ca->last_rec_cwnd_q10 = ca->cwnd_q10;
        } else if (now_us >= ca->last_rec_time_us + srtt_us) {
            u8 dir = (ca->cwnd_q10 > ca->last_rec_cwnd_q10) ? COPA_DIR_UP : COPA_DIR_DOWN;
            if (dir != ca->direction) {
                ca->velocity = 1;
                ca->same_direction = 0;
            } else {
                u32 thresh = 3;
                ca->same_direction++;
                if (ca->cfg.adaptive && ca->dq_target_us > 0) {
                    if ((dir == COPA_DIR_UP && dq_us * 2 < ca->dq_target_us) ||
                        (dir == COPA_DIR_DOWN && ca->seen_app_limited &&
                         dq_us > max_t(u64, (5ULL * ca->dq_target_us) / 2, 50000ULL))) {
                        thresh = 1;
                    }
                }
                if (ca->same_direction >= thresh && ca->velocity < (1U << 20))
                    ca->velocity <<= 1;
            }
            /* Cap velocity <= delta * cwnd */
            {
                u64 cap = ((u64)ca->delta_q20 * ca->cwnd_q10) >> 30;
                while (ca->velocity > 1 && (u64)ca->velocity > cap)
                    ca->velocity >>= 1;
            }
            ca->direction = dir;
            ca->last_rec_time_us = now_us;
            ca->last_rec_cwnd_q10 = ca->cwnd_q10;
        }
    }

    if (increase) {
        ca->ss_confirm_since_us = 0;
        if (ca->slow_start) {
            ca->cwnd_q10 += (u64)acked_pkts << 10;
        } else {
            /* step_q10 = acked * v / (delta * cwnd) * 1024 */
            u64 num = (u64)acked_pkts * (u64)ca->velocity;
            u64 denom = max_t(u64, ((u64)ca->delta_q20 * ca->cwnd_q10) >> 20, 1ULL);
            u64 step_q10 = div64_u64(num << 20, denom);
            ca->cwnd_q10 += step_q10;
        }
    } else {
        if (ca->slow_start && ca->cfg.adaptive) {
            u64 jguard = max_t(u64, ca->jitter_us + 4000ULL, min_t(u64, rtt_min_us / 4, 10000ULL));
            if (dq_us < jguard) {
                if (ca->ss_confirm_since_us == 0)
                    ca->ss_confirm_since_us = now_us;
                if (now_us < ca->ss_confirm_since_us + max_t(u64, srtt_us, 20000ULL))
                    return (u32)(ca->cwnd_q10 >> 10);
            }
            ca->ss_confirm_since_us = 0;
            if (ca->startup_delta_q20 == 0 && bdp_pkts > 60) {
                u32 sd = (u32)div64_u64(6ULL << 20, bdp_pkts);
                ca->startup_delta_q20 = clamp_t(u32, sd, 5243U, ca->cfg.init_delta_q20);
            }
        }
        ca->slow_start = false;
        {
            u64 num = (u64)acked_pkts * (u64)ca->velocity;
            u64 denom = max_t(u64, ((u64)ca->delta_q20 * ca->cwnd_q10) >> 20, 1ULL);
            u64 step_q10 = div64_u64(num << 20, denom);
            if (ca->cwnd_q10 > step_q10)
                ca->cwnd_q10 -= step_q10;
            else
                ca->cwnd_q10 = (u64)COPA_MIN_CWND_PKTS << 10;
        }
    }

    ca->cwnd_q10 = clamp_t(u64, ca->cwnd_q10,
                           (u64)COPA_MIN_CWND_PKTS << 10,
                           (u64)COPA_MAX_CWND_PKTS << 10);
    return (u32)(ca->cwnd_q10 >> 10);
}

#endif /* COPA_AUTO_CORE_H */
