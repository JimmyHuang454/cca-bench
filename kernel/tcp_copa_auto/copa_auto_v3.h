/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copa-Auto v3: Clean first-principles congestion control (pure 64-bit integer C).
 *
 * Implements two cleanly decoupled k (= c / mu) estimation engines plus their
 * physical two-regime fusion on top of an identical pure-Copa + PROBE_EMPTY +
 * AIMD-delta + loss-insensitive core:
 *   - Variant A (COPA_V3_PROBE_BW):   BBRv2/v3-style gentle unbiased-mean ProbeBW
 *                                     estimates mean capacity mu_hat -> rho = lambda/mu_hat
 *                                     -> Kingman inversion k = (dq - j)*(1 - rho)/rho.
 *   - Variant B (COPA_V3_DIFFUSION):  Passive multi-scale Queue-Diffusion (zero ProbeBW)
 *                                     estimates v_W = (Var(dW_dt) - 2*sigma_eta^2)/(2*dt)
 *                                     = rho*k across 10/40/160 ms scales -> rho = 1 - v_W/W
 *                                     -> k = v_W / rho.
 *   - Variant H (COPA_V3_HYBRID):     Two-regime fusion (copa_auto_v3):
 *                                     Scheme B drives steady cruise (zero periodic ProbeBW tax);
 *                                     Scheme A fires strictly on-demand (cold start, 1-RTT-gated
 *                                     idle wakeup, high-jitter under-utilization, post-j-jump),
 *                                     plus universal "cwnd shrunk without queue drop" j/R0 refresh.
 *
 * Strict design invariants:
 *   1. Default mode is 100% loss-insensitive (no loss_dq_us, no loss cwnd/delta brake).
 *   2. Zero PASTA idle-ACK counting (N_idle/N_ack removed).
 *   3. Exact closed-form target d_{q,theta}* from delta-tradeoff-v4.tex Prop. 6 & 8.
 *   4. Multi-flow fair AIMD outer loop on 1/delta (Prop. 9).
 */
#ifndef COPA_AUTO_V3_H
#define COPA_AUTO_V3_H

#include "copa_auto_math.h"

#define COPA_V3_MIN_CWND_PKTS     4U
#define COPA_V3_MAX_CWND_PKTS     1000000U
#define COPA_V3_MAX_JITTER_US     20000ULL    /* 20 ms cap on jitter floor j */
#define COPA_V3_MIN_RTT_WIN_US    10000000ULL /* 10 s sliding min-RTT window */
#define COPA_V3_EMPTY_PERIOD_US   15000000ULL /* 15 s aligned PROBE_EMPTY slot */
#define COPA_V3_DELTA_MIN_Q20     1049U       /* 0.001 * 2^20 */
#define COPA_V3_DELTA_MAX_Q20     524288U     /* 0.50  * 2^20 */
#define COPA_V3_DELTA_INIT_Q20    524288U     /* 0.50  * 2^20 */
#define COPA_V3_MAX_STEP_Q16      (2LL * COPA_LN2_Q16) /* ln(4) in Q16 */

/* Scheme A (ProbeBW) parameters */
#define COPA_V3_MU_RING_SZ        6U
#define COPA_V3_PROBE_MIN_GAP_US  1500000ULL  /* 1.5 s min periodic ProbeBW gap */
#define COPA_V3_PROBE_MAX_GAP_US  3000000ULL  /* 3.0 s max periodic ProbeBW gap */

/* Scheme B (Multi-scale Queue-Diffusion) parameters */
#define COPA_V3_DIFF_BIN_US       10000ULL    /* 10 ms wall-clock bins */
#define COPA_V3_DIFF_BIN_RING     32U
#define COPA_V3_DIFF_SCALES       3U          /* lags: ~10 ms, ~40 ms, ~160 ms */
#define COPA_V3_DIFF_WIN          16U         /* 16-sample ring per scale */

enum copa_v3_variant {
    COPA_V3_PROBE_BW  = 0, /* Scheme A: unbiased-mean ProbeBW -> c, k */
    COPA_V3_DIFFUSION = 1, /* Scheme B: passive multi-scale Queue-Diffusion -> k */
    COPA_V3_HYBRID    = 2, /* Two-regime fusion: B steady cruise + A on-demand ignition */
};

enum copa_v3_phase {
    COPA_V3_PHASE_CRUISE      = 0,
    COPA_V3_PHASE_PROBE_EMPTY = 1, /* shared: measure R0, j, empty jitter var */
    COPA_V3_PHASE_PROBE_UP    = 2, /* Scheme A only: 1-RTT 1.25x probe */
    COPA_V3_PHASE_PROBE_DRAIN = 3, /* Scheme A only: 1-RTT 0.80x drain & observe */
};

enum copa_v3_dir {
    COPA_V3_DIR_NONE = 0,
    COPA_V3_DIR_UP   = 1,
    COPA_V3_DIR_DOWN = 2,
};

/* Kathleen Nichols 3-sample windowed min/max filter */
struct copa_v3_mm_sample {
    u64 t;
    u64 v;
};

struct copa_v3_minmax {
    struct copa_v3_mm_sample s[3];
    bool init;
};

static inline u64 copa_v3_mm_get(const struct copa_v3_minmax *m)
{
    return m->init ? m->s[0].v : 0;
}

static inline void copa_v3_mm_reset(struct copa_v3_minmax *m, u64 t, u64 v)
{
    m->s[0].t = m->s[1].t = m->s[2].t = t;
    m->s[0].v = m->s[1].v = m->s[2].v = v;
    m->init = true;
}

static inline void copa_v3_mm_subwin(struct copa_v3_minmax *m, u64 win, u64 t, u64 v)
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

static inline u64 copa_v3_mm_min(struct copa_v3_minmax *m, u64 win, u64 t, u64 v)
{
    if (!m->init || v <= m->s[0].v || (t >= m->s[2].t && (t - m->s[2].t) > win)) {
        copa_v3_mm_reset(m, t, v);
        return v;
    }
    if (v <= m->s[1].v) {
        m->s[2].t = m->s[1].t = t;
        m->s[2].v = m->s[1].v = v;
    } else if (v <= m->s[2].v) {
        m->s[2].t = t;
        m->s[2].v = v;
    }
    copa_v3_mm_subwin(m, win, t, v);
    return m->s[0].v;
}

static inline u64 copa_v3_mm_max(struct copa_v3_minmax *m, u64 win, u64 t, u64 v)
{
    if (!m->init || v >= m->s[0].v || (t >= m->s[2].t && (t - m->s[2].t) > win)) {
        copa_v3_mm_reset(m, t, v);
        return v;
    }
    if (v >= m->s[1].v) {
        m->s[2].t = m->s[1].t = t;
        m->s[2].v = m->s[1].v = v;
    } else if (v >= m->s[2].v) {
        m->s[2].t = t;
        m->s[2].v = v;
    }
    copa_v3_mm_subwin(m, win, t, v);
    return m->s[0].v;
}

/* Lightweight cross-flow board to align periodic PROBE_EMPTY slots on same host */
struct copa_v3_board {
    u64 slot_req[4];
};

struct copa_v3_cfg {
    u8   variant;         /* COPA_V3_PROBE_BW (0) or COPA_V3_DIFFUSION (1) */
    bool mode_switching;  /* true: allow delay-triggered TCPCOOP competitive mode */
    s32  jitter_us;       /* < 0: auto PROBE_EMPTY; >= 0: fixed j [us] */
    u32  theta_milli;     /* 500 = 0.50 power-optimal; [10, 995] */
    u32  init_delta_q20;  /* default 0.50 (524288) */
};

struct copa_v3_diff_bin {
    u64 idx;
    u32 min_raw_dq_us;
};

struct copa_v3_state {
    struct copa_v3_cfg   cfg;
    struct copa_v3_board *board;

    /* Core Copa state */
    u64 cwnd_q10;          /* packets in Q10 */
    u32 delta_q20;         /* active delta in Q20 */
    u32 delta_run_q20;     /* outer-loop adapted delta (0 until first update) */
    u32 velocity;
    u8  direction;
    u32 same_direction;
    u64 last_rec_time_us;
    u64 last_rec_cwnd_q10;
    bool slow_start;
    u64 ss_confirm_since_us;
    u64 dec_round_start_us;
    u64 dec_round_floor_q10;

    /* RTT & queue tracking */
    struct copa_v3_minmax win_rtt_min;
    struct copa_v3_minmax recent_rtt_min; /* short ~2 s min-RTT to detect base-RTT jumps before TCPCOOP */
    struct copa_v3_minmax rtt_standing;
    struct copa_v3_minmax rtt_max;
    u64 base_rtt_us;       /* R0 [us] */
    u64 standing_us;
    u64 srtt_us;
    u64 dq_us;             /* standing_us - base_rtt_us */
    u64 last_time_us;
    u64 loaded_since_us;
    u64 last_loaded_ack_us;
    u64 last_app_limited_us;
    bool seen_app_limited;
    bool last_ack_app_limited;

    /* Competitive mode (ONLY delay-triggered TCPCOOP; never MODE_LOSS) */
    bool competitive;
    u64  last_nearly_empty_us;
    u64  last_comp_backoff_us;

    /* Estimated physical parameters (R0, j, k, rho, c, dq*) */
    bool has_jitter;
    bool first_empty_done;
    u64  jitter_us;        /* j [us] */
    u64  empty_var_us2;    /* 2 * sigma_eta^2 [us^2] measured during PROBE_EMPTY */
    s64  log_k_q16;        /* smoothed ln(k_us) in Q16 */
    bool has_k;
    u64  k_us;             /* estimated k = c / mu [us] */
    u32  rho_milli;        /* estimated utilization rho * 1000 */
    u32  c_milli;          /* estimated dimensionless burstiness c_i * 1000 (telemetry) */
    u64  dq_target_us;     /* closed-form d_{q,theta}* [us] */

    /* Bandwidth / BDP tracking (bytes/sec) */
    u64 bw_ewma_bps;
    u64 peak_bdp_q10;
    u32 last_mss;

    /* Phase state machine */
    u8   phase;
    u64  phase_start_us;
    u64  phase_deadline_us;
    u64  saved_cwnd_q10;
    u64  phase_pacing_bps;

    /* PROBE_EMPTY state */
    u64  empty_min_rtt_us;
    u64  empty_prev_cum_min_us;
    u64  empty_round_min_us;
    u64  empty_prev_round_min_us;
    u64  empty_round_start_us;
    u32  empty_flat_rounds;
    u32  empty_no_drop_rounds;
    u64  empty_idle_at_us;
    u64  empty_sample_until_us;
    u64  empty_stand_sum_us;
    u32  empty_stand_cnt;
    u64  empty_last_bin_idx;
    u32  empty_cur_bin_min_us;
    u32  empty_prev_bin_min_us;
    bool empty_has_prev_bin;
    u64  empty_diff_sum_us;
    u32  empty_diff_cnt;
    u64  last_empty_slot;
    bool has_empty_slot;
    u64  last_handover_probe_us;
    u64  base_shift_since_us;
    u64  passive_j_at_us;
    bool empty_is_aligned;

    /* Passive low-flight j/R0 learning (for app-limited / RPC workloads) */
    u64  low_win_start_us;
    u64  low_min_us;
    u32  low_samples;
    u64  low_dq_sum_us;
    u32  low_windows;
    u64  raised_min_us;
    u32  raised_windows;

    /* Steady outer-loop 4-RTT update cycle */
    u64  cycle_round_start_us;
    u32  cycle_rounds;
    bool cycle_dirty;
    u64  cycle_dq_sum_us;
    u64  cycle_raw_w_sum_us;
    u64  cycle_rate_sum_bps;
    u32  cycle_rate_cnt;
    u64  cycle_acks;
    u32  empty_q_streak;   /* consecutive cycles with W == 0 while j > 0 */

    /* Scheme A (ProbeBW) state */
    u64  mu_ring[COPA_V3_MU_RING_SZ];
    u32  mu_cnt;
    u32  mu_head;
    u64  mu_hat_bps;       /* unbiased trimmed-mean capacity [bytes/s] */
    u64  last_probebw_us;
    u64  probe_base_rate_bps;
    u64  probe_dq_before_us;
    u64  probe_obs_rate_sum;
    u32  probe_obs_rate_cnt;
    u64  probe_obs_max_rate;
    u64  probe_obs_max_dq_us;
    u32  probe_escalations;
    u32  cap_drop_rounds;
    u64  cap_drop_rate_sum;
    u32  cap_drop_rate_cnt;
    u64  cap_drop_round_start_us;

    /* Scheme B (Multi-scale Queue-Diffusion) state */
    struct copa_v3_diff_bin diff_bins[COPA_V3_DIFF_BIN_RING];
    u32  diff_bin_cnt;
    u32  diff_bin_head;
    u64  cur_bin_idx;
    u32  cur_bin_min_raw_dq_us;
    bool cur_bin_valid;
    s32  diff_dw[COPA_V3_DIFF_SCALES][COPA_V3_DIFF_WIN]; /* signed increments for drift removal */
    u32  diff_abs_cnt[COPA_V3_DIFF_SCALES];
    u32  diff_abs_head[COPA_V3_DIFF_SCALES];
    u32  cycle_diff_samples;
    u32  quiet_diff_cycles;
    u64  diff_vw_us;       /* latest v_W = rho * k [us] (telemetry) */
    u64  diff_suppress_until_us;
    u64  diff_cwnd_ref_q10;
    u64  diff_cwnd_ref_us;

    /* Hybrid (COPA_V3_HYBRID) on-demand ignition, idle safety gate & j-jump guard */
    struct copa_v3_minmax recent_cwnd_max; /* 4 s sliding max cwnd_q10 */
    u64  j_jump_round_start_us;
    u64  j_jump_dq_sum_us;
    u32  j_jump_dq_cnt;
    u32  j_jump_rounds;
    u64  app_lim_since_us;
    bool idle_wakeup_pending;
    u64  idle_wakeup_start_us;
    u64  idle_r0_check_until_us;
    u64  idle_r0_min_us;
    u32  idle_r0_samples;
    u32  quiet_empty_rtts;
    u64  quiet_empty_round_us;
    u32  low_j_streak;
    u64  low_j_sum_us;
};

static inline bool copa_v3_fixed_j(const struct copa_v3_cfg *cfg)
{
    return cfg->jitter_us >= 0;
}

static inline u64 copa_v3_fixed_j_us(const struct copa_v3_cfg *cfg)
{
    return cfg->jitter_us > 0 ? min_t(u64, (u64)cfg->jitter_us, 100000ULL) : 0ULL;
}

static inline void copa_v3_reset_velocity(struct copa_v3_state *st)
{
    st->velocity = 1;
    st->direction = COPA_V3_DIR_NONE;
    st->same_direction = 0;
    st->last_rec_time_us = 0;
}

static inline void copa_v3_reset_cycle(struct copa_v3_state *st, u64 now_us)
{
    st->cycle_round_start_us = now_us;
    st->cycle_rounds = 0;
    st->cycle_dirty = false;
    st->cycle_dq_sum_us = 0;
    st->cycle_raw_w_sum_us = 0;
    st->cycle_rate_sum_bps = 0;
    st->cycle_rate_cnt = 0;
    st->cycle_acks = 0;
    st->cycle_diff_samples = 0;
}

static inline void copa_v3_reset_diffusion(struct copa_v3_state *st)
{
    u32 s;

    st->diff_bin_cnt = 0;
    st->diff_bin_head = 0;
    st->cur_bin_valid = false;
    for (s = 0; s < COPA_V3_DIFF_SCALES; s++) {
        st->diff_abs_cnt[s] = 0;
        st->diff_abs_head[s] = 0;
    }
    st->diff_vw_us = 0;
}

static inline void copa_v3_init(struct copa_v3_state *st,
                                struct copa_v3_cfg cfg,
                                struct copa_v3_board *board,
                                u64 now_us,
                                u32 init_cwnd_pkts)
{
    u32 cwnd = max_t(u32, init_cwnd_pkts, COPA_V3_MIN_CWND_PKTS);

    __builtin_memset(st, 0, sizeof(*st));
    if (cfg.theta_milli < 10U || cfg.theta_milli > 995U)
        cfg.theta_milli = 500U;
    if (cfg.init_delta_q20 < COPA_V3_DELTA_MIN_Q20 ||
        cfg.init_delta_q20 > COPA_V3_DELTA_MAX_Q20)
        cfg.init_delta_q20 = COPA_V3_DELTA_INIT_Q20;

    st->cfg = cfg;
    st->board = board;
    st->cwnd_q10 = (u64)cwnd << 10;
    st->delta_q20 = cfg.init_delta_q20;
    st->velocity = 1;
    st->slow_start = true;
    st->last_time_us = now_us;
    st->last_nearly_empty_us = now_us;
    st->last_mss = 1448U;
    st->phase = COPA_V3_PHASE_CRUISE;
    st->rho_milli = 500U;

    if (copa_v3_fixed_j(&cfg)) {
        st->has_jitter = true;
        st->low_j_streak = 0;
        st->low_j_sum_us = 0;
        st->first_empty_done = true;
        st->jitter_us = copa_v3_fixed_j_us(&cfg);
    }
    copa_v3_reset_cycle(st, now_us);
}

/* Small insertion sort for u32 arrays (N <= 16) */
static inline void copa_v3_sort_u32(u32 *arr, u32 n)
{
    u32 i, j;

    for (i = 1; i < n; i++) {
        u32 key = arr[i];
        j = i;
        while (j > 0 && arr[j - 1] > key) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

/* Small insertion sort for u64 arrays (N <= 8) */
static inline void copa_v3_sort_u64(u64 *arr, u32 n)
{
    u32 i, j;

    for (i = 1; i < n; i++) {
        u64 key = arr[i];
        j = i;
        while (j > 0 && arr[j - 1] > key) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

/*
 * Scheme A: record one unbiased ProbeBW capacity sample C_m [bytes/s]
 * and recompute trimmed mean mu_hat_bps (NEVER BBR's max!).
 */
static inline void copa_v3_record_mu_sample(struct copa_v3_state *st, u64 rate_bps)
{
    u64 tmp[COPA_V3_MU_RING_SZ];
    u32 i, n, lo, hi;
    u64 sum = 0;

    if (rate_bps == 0)
        return;

    /* Regime-shift detection: if two consecutive samples differ by >2x from mu_hat, flush */
    if (st->mu_cnt >= 3 && st->mu_hat_bps > 0) {
        u64 prev = st->mu_ring[(st->mu_head + COPA_V3_MU_RING_SZ - 1U) % COPA_V3_MU_RING_SZ];
        bool both_high = (rate_bps * 2ULL > st->mu_hat_bps * 3ULL) &&
                         (prev * 2ULL > st->mu_hat_bps * 3ULL);
        bool both_low  = (rate_bps * 3ULL < st->mu_hat_bps * 2ULL) &&
                         (prev * 3ULL < st->mu_hat_bps * 2ULL);
        if (both_high || both_low) {
            st->mu_ring[0] = prev;
            st->mu_ring[1] = rate_bps;
            st->mu_cnt = 2;
            st->mu_head = 2;
            st->mu_hat_bps = (prev + rate_bps) / 2ULL;
            return;
        }
    }

    st->mu_ring[st->mu_head] = rate_bps;
    st->mu_head = (st->mu_head + 1U) % COPA_V3_MU_RING_SZ;
    if (st->mu_cnt < COPA_V3_MU_RING_SZ)
        st->mu_cnt++;

    n = st->mu_cnt;
    for (i = 0; i < n; i++)
        tmp[i] = st->mu_ring[i];
    copa_v3_sort_u64(tmp, n);

    if (n >= 4U) {
        lo = 1U;
        hi = n - 1U; /* drop single min and single max */
    } else {
        lo = 0U;
        hi = n;
    }
    for (i = lo; i < hi; i++)
        sum += tmp[i];
    st->mu_hat_bps = div64_u64(sum, (u64)(hi - lo));
}

/*
 * Scheme B: close one 10-ms queue-delay bin and update multi-scale |dW| rings.
 * Condition on busy queue: (W_prev > busy_floor) || (W_cur > busy_floor).
 */
static inline void copa_v3_diff_push_bin(struct copa_v3_state *st,
                                         u64 bin_idx,
                                         u32 min_raw_dq_us,
                                         u64 busy_floor_us)
{
    static const u32 lag_lo[COPA_V3_DIFF_SCALES]  = { 1U, 3U, 12U };
    static const u32 lag_hi[COPA_V3_DIFF_SCALES]  = { 2U, 6U, 20U };
    static const u32 lag_nom[COPA_V3_DIFF_SCALES] = { 1U, 4U, 16U };
    u32 cur_w = (min_raw_dq_us > (u32)st->jitter_us) ? (min_raw_dq_us - (u32)st->jitter_us) : 0U;
    u32 s, i;

    for (s = 0; s < COPA_V3_DIFF_SCALES; s++) {
        u32 best_diff = 1000U;
        bool found = false;
        u32 found_w = 0;
        u32 found_lag = lag_nom[s];

        for (i = 0; i < st->diff_bin_cnt; i++) {
            u32 pos = (st->diff_bin_head + COPA_V3_DIFF_BIN_RING - 1U - i) % COPA_V3_DIFF_BIN_RING;
            u64 p_idx = st->diff_bins[pos].idx;
            if (bin_idx <= p_idx)
                continue;
            {
                u64 lag = bin_idx - p_idx;
                if (lag > lag_hi[s])
                    break;
                if (lag >= lag_lo[s]) {
                    u32 d = (lag > lag_nom[s]) ? (u32)(lag - lag_nom[s]) : (u32)(lag_nom[s] - lag);
                    if (!found || d < best_diff) {
                        u32 pm = st->diff_bins[pos].min_raw_dq_us;
                        found_w = (pm > (u32)st->jitter_us) ? (pm - (u32)st->jitter_us) : 0U;
                        found_lag = (u32)lag;
                        best_diff = d;
                        found = true;
                        if (d == 0)
                            break;
                    }
                }
            }
        }

        if (found && (found_w > busy_floor_us || cur_w > busy_floor_us)) {
            s64 sdw = (s64)cur_w - (s64)found_w;
            /* Scale signed dW to nominal lag via sqrt(lag_nom / found_lag) */
            if (found_lag != lag_nom[s] && sdw != 0) {
                u64 scale_q10 = int_sqrt64(div64_u64((u64)lag_nom[s] << 20, (u64)found_lag));
                if (sdw > 0)
                    sdw = (s64)(((u64)sdw * scale_q10) >> 10);
                else
                    sdw = -(s64)(((u64)(-sdw) * scale_q10) >> 10);
            }
            /* Cap extreme step spikes before ring insertion (+/- 200 ms) */
            sdw = clamp_t(s64, sdw, -200000LL, 200000LL);
            st->diff_dw[s][st->diff_abs_head[s]] = (s32)sdw;
            st->diff_abs_head[s] = (st->diff_abs_head[s] + 1U) % COPA_V3_DIFF_WIN;
            if (st->diff_abs_cnt[s] < COPA_V3_DIFF_WIN)
                st->diff_abs_cnt[s]++;
            st->cycle_diff_samples++;
        }
    }

    st->diff_bins[st->diff_bin_head].idx = bin_idx;
    st->diff_bins[st->diff_bin_head].min_raw_dq_us = min_raw_dq_us;
    st->diff_bin_head = (st->diff_bin_head + 1U) % COPA_V3_DIFF_BIN_RING;
    if (st->diff_bin_cnt < COPA_V3_DIFF_BIN_RING)
        st->diff_bin_cnt++;
}

/*
 * Scheme B: compute multi-scale diffusion rate v_W [us] across dt = 10 / 40 / 160 ms.
 * Uses two fundamental physics properties of queue diffusion vs bounded noise:
 *   1. True diffusion has Var(dW_dt) growing with dt, whereas white jitter or D/D/1
 *      Copa sawtooth has Var(dW_dt) flat/bounded -> require max(Var_1, Var_2) > Var_0
 *      once multi-scale data exists, and subtract bounded baseline.
 *   2. Taking the median across valid scales rejects both high-freq noise (scale 0)
 *      and ballistic slow-fade overshoot (scale 2).
 */
static inline bool copa_v3_compute_diffusion_vw(const struct copa_v3_state *st, u64 *out_vw_us)
{
    static const u64 dt_us[COPA_V3_DIFF_SCALES] = { 10000ULL, 40000ULL, 160000ULL };
    u32 tmp[COPA_V3_DIFF_WIN];
    u64 raw_var[COPA_V3_DIFF_SCALES] = { 0, 0, 0 };
    u64 vws[COPA_V3_DIFF_SCALES] = { 0, 0, 0 };
    bool has_s[COPA_V3_DIFF_SCALES] = { false, false, false };
    u32 s, i, valid_cnt = 0;

    for (s = 0; s < COPA_V3_DIFF_SCALES; s++) {
        u32 n = st->diff_abs_cnt[s];
        u32 trim, lo, hi;
        s64 signed_sum = 0, drift;
        u64 sum = 0, mean_abs, factor_pct, var_us2, net_var;

        if (n < 4U)
            continue;
        has_s[s] = true;
        /* Subtract signed mean drift so one-sided ramps/steps do NOT look like random diffusion */
        for (i = 0; i < n; i++)
            signed_sum += (s64)st->diff_dw[s][i];
        drift = div64_s64(signed_sum, (s64)n);
        for (i = 0; i < n; i++) {
            s64 dev = (s64)st->diff_dw[s][i] - drift;
            tmp[i] = (u32)(dev >= 0 ? dev : -dev);
        }
        copa_v3_sort_u32(tmp, n);

        /* Trimmed mean of |dW| with exact half-normal variance conversion:
         *  n < 8:  no trim   -> 1 / E[|Z|]^2 = pi/2 = 1.57
         *  8..15:  trim 1    -> ~1.78
         *  16:     trim 2    -> 1.85
         */
        if (n >= 16U) {
            trim = 2U;
            factor_pct = 185ULL;
        } else if (n >= 8U) {
            trim = 1U;
            factor_pct = 178ULL;
        } else {
            trim = 0U;
            factor_pct = 157ULL;
        }
        lo = trim;
        hi = n - trim;
        for (i = lo; i < hi; i++)
            sum += tmp[i];
        mean_abs = div64_u64(sum, (u64)(hi - lo));

        /* Clamp mean_abs before squaring to prevent u64 overflow */
        if (mean_abs > 500000ULL)
            mean_abs = 500000ULL;
        var_us2 = div64_u64(factor_pct * mean_abs * mean_abs, 100ULL);
        raw_var[s] = var_us2;
        net_var = (var_us2 > st->empty_var_us2) ? (var_us2 - st->empty_var_us2) : 0ULL;
        vws[valid_cnt++] = div64_u64(net_var, 2ULL * dt_us[s]);
    }

    if (valid_cnt == 0) {
        *out_vw_us = 0;
        return false;
    }

    /* Bounded-noise rejection: if variance does NOT grow from 10 ms to 40/160 ms,
     * the fluctuation is bounded (white jitter or D/D/1 Copa sawtooth), not diffusion. */
    if (has_s[0] && (has_s[1] || has_s[2])) {
        u64 longer_var = max_t(u64, has_s[1] ? raw_var[1] : 0ULL,
                               has_s[2] ? raw_var[2] : 0ULL);
        if (longer_var * 100ULL <= raw_var[0] * 115ULL) {
            *out_vw_us = 0;
            return true;
        }
    }

    copa_v3_sort_u64(vws, valid_cnt);
    if (valid_cnt == 3U)
        *out_vw_us = vws[1]; /* median across {10, 40, 160} ms */
    else if (valid_cnt == 2U)
        *out_vw_us = vws[0]; /* conservative smaller of 2 scales */
    else
        *out_vw_us = vws[0];
    return true;
}

/* Smooth k_us in log domain (beta = 0.3, paper Eq. eq:k-ewma); tiny k_sample decays to 0 */
static inline void copa_v3_smooth_k(struct copa_v3_state *st, u64 k_sample_us)
{
    if (k_sample_us < 15ULL) {
        if (!st->has_k || st->k_us <= 20ULL) {
            st->k_us = 0;
            st->has_k = false;
            st->log_k_q16 = 0;
        } else {
            /* Gentle geometric decay toward 0 on D/D/1 links */
            st->k_us = (st->k_us * 7ULL) / 10ULL;
            if (st->k_us <= 20ULL) {
                st->k_us = 0;
                st->has_k = false;
                st->log_k_q16 = 0;
            } else {
                st->log_k_q16 = copa_ln_ratio_q16(st->k_us, 1ULL);
            }
        }
        return;
    }

    {
        u64 clamped = clamp_t(u64, k_sample_us, 15ULL, 1000000ULL);
        s64 lk = copa_ln_ratio_q16(clamped, 1ULL);
        if (!st->has_k) {
            st->log_k_q16 = lk;
            st->has_k = true;
        } else {
            st->log_k_q16 = div64_s64(7LL * st->log_k_q16 + 3LL * lk, 10LL);
        }
        /* exp(log_k_q16) returned in Q16 -> shift right 16 */
        st->k_us = copa_exp_q16(st->log_k_q16) >> COPA_Q16_SHIFT;
        if (st->k_us < 15ULL)
            st->k_us = 15ULL;
    }
}

/*
 * Pacing rate in bytes/sec:
 *   - CRUISE:      2 * cwnd_bytes / standing_rtt (standard Copa)
 *   - PROBE_EMPTY: gentle drain rate (0.70 * recent_bw)
 *   - PROBE_UP:    1.25 * probe_base_rate_bps (Scheme A)
 *   - PROBE_DRAIN: 0.80 * probe_base_rate_bps (Scheme A)
 */
static inline u64 copa_v3_pacing_rate_bps(const struct copa_v3_state *st, u32 mss)
{
    u64 stand = st->standing_us;
    u64 cwnd_bytes = (st->cwnd_q10 * (u64)mss) >> 10;

    if (st->phase != COPA_V3_PHASE_CRUISE && st->phase_pacing_bps > 0)
        return st->phase_pacing_bps;
    if (stand == 0)
        return 0;
    return div64_u64(cwnd_bytes * 2000000ULL, stand);
}

/*
 * Strict loss-insensitive handler:
 *   - RTO (persistent == true): standard TCP RTO recovery reset.
 *   - Normal packet loss (persistent == false):
 *       * Default mode (!competitive): 100% NO-OP!
 *       * Competitive mode (mode_switching && TCPCOOP): double delta (back off toward 0.5).
 */
static inline void copa_v3_on_loss(struct copa_v3_state *st, u64 now_us, bool persistent)
{
    if (persistent) {
        st->phase = COPA_V3_PHASE_CRUISE;
        st->phase_pacing_bps = 0;
        st->competitive = false;
        st->last_nearly_empty_us = now_us;
        st->cwnd_q10 = (u64)COPA_V3_MIN_CWND_PKTS << 10;
        st->slow_start = true;
        st->ss_confirm_since_us = 0;
        st->loaded_since_us = 0;
        st->peak_bdp_q10 = 0;
        copa_v3_reset_velocity(st);
        copa_v3_reset_cycle(st, now_us);
        return;
    }

    if (!st->competitive)
        return;

    if (st->last_comp_backoff_us > 0 && now_us < st->last_comp_backoff_us + st->srtt_us)
        return;
    st->delta_q20 = min_t(u32, st->delta_q20 * 2U, COPA_V3_DELTA_MAX_Q20);
    st->last_comp_backoff_us = now_us;
}

/* Passive low-flight observation for app-limited / RPC workloads */
static inline void copa_v3_observe_low(struct copa_v3_state *st,
                                       u64 now_us,
                                       u64 rtt_us,
                                       bool low_flight)
{
    u64 base = st->base_rtt_us;
    u64 span;

    if (!base)
        return;
    span = max_t(u64, base / 2ULL, 1000ULL);
    if (!low_flight || !st->low_samples || now_us > st->low_win_start_us + 2ULL * span) {
        st->low_samples = 0;
        st->low_win_start_us = now_us;
        st->low_min_us = rtt_us;
    }
    if (!low_flight)
        return;
    st->low_min_us = min_t(u64, st->low_min_us, rtt_us);
    st->low_samples++;
    if (st->low_samples < 2U || now_us < st->low_win_start_us + span)
        return;
    st->low_samples = 0;

    if (st->low_min_us > base + st->jitter_us + max_t(u64, 5000ULL, base / 4ULL)) {
        st->raised_min_us = st->raised_windows ? min_t(u64, st->raised_min_us, st->low_min_us)
                                               : st->low_min_us;
        if (++st->raised_windows >= 3U) {
            st->base_rtt_us = st->raised_min_us;
            copa_v3_mm_reset(&st->win_rtt_min, now_us, st->base_rtt_us);
            copa_v3_mm_reset(&st->recent_rtt_min, now_us, st->base_rtt_us);
            st->raised_windows = 0;
            st->k_us = 0;
            st->has_k = false;
            st->dq_target_us = 0;
            st->delta_run_q20 = 0;
            copa_v3_reset_diffusion(st);
            copa_v3_reset_cycle(st, now_us);
        }
        return;
    }
    st->raised_windows = 0;
    if (copa_v3_fixed_j(&st->cfg))
        return;
    st->low_dq_sum_us += (st->low_min_us > base) ? (st->low_min_us - base) : 0ULL;
    if (++st->low_windows == 3U) {
        u64 j = min_t(u64, st->low_dq_sum_us / 3ULL, COPA_V3_MAX_JITTER_US);
        st->jitter_us = st->has_jitter ? (3ULL * st->jitter_us + j) / 4ULL : j;
        st->has_jitter = true;
        st->low_j_streak = 0;
        st->low_j_sum_us = 0;
        st->first_empty_done = true;
        st->passive_j_at_us = now_us;
        st->low_windows = 0;
        st->low_dq_sum_us = 0;
    }
}

/* Start unified PROBE_EMPTY to measure (R0, j, empty_var_us2) */
static inline void copa_v3_start_probe_empty(struct copa_v3_state *st,
                                             u64 now_us,
                                             u32 mss,
                                             bool aligned)
{
    u64 srtt = max_t(u64, st->srtt_us, 10000ULL);
    u64 base = st->base_rtt_us ? st->base_rtt_us : srtt;
    u64 cwnd_bytes = (st->cwnd_q10 * (u64)mss) >> 10;
    u64 cwnd_rate = div64_u64(cwnd_bytes * 1000000ULL, max_t(u64, st->standing_us, base));
    u64 ref_bw = st->bw_ewma_bps > 0 ? min_t(u64, st->bw_ewma_bps, (cwnd_rate * 3ULL) / 2ULL)
                                     : cwnd_rate;
    u64 min_rate = div64_u64((u64)COPA_V3_MIN_CWND_PKTS * (u64)mss * 1000000ULL, srtt);
    u64 dur = clamp_t(u64, 6ULL * srtt, 180000ULL, 650000ULL);

    st->phase = COPA_V3_PHASE_PROBE_EMPTY;
    st->phase_start_us = now_us;
    st->phase_deadline_us = now_us + dur;
    st->saved_cwnd_q10 = st->cwnd_q10;
    st->phase_pacing_bps = max_t(u64, (ref_bw * 70ULL) / 100ULL, min_rate);

    st->empty_min_rtt_us = 0;
    st->empty_prev_cum_min_us = 0;
    st->empty_round_min_us = 0;
    st->empty_prev_round_min_us = 0;
    st->empty_round_start_us = now_us;
    st->empty_flat_rounds = 0;
    st->empty_no_drop_rounds = 0;
    st->empty_idle_at_us = 0;
    st->empty_sample_until_us = 0;
    st->empty_stand_sum_us = 0;
    st->empty_stand_cnt = 0;
    st->empty_has_prev_bin = false;
    st->empty_last_bin_idx = 0;
    st->empty_cur_bin_min_us = 0;
    st->empty_prev_bin_min_us = 0;
    st->empty_diff_sum_us = 0;
    st->empty_diff_cnt = 0;
    st->empty_is_aligned = aligned;
    st->cycle_dirty = true;
}

static inline void copa_v3_finish_probe_empty(struct copa_v3_state *st, u64 now_us)
{
    u64 base = st->base_rtt_us;
    u64 shift_thresh = max_t(u64, 5000ULL, (base * 15ULL) / 100ULL);
    bool flat_rose = (st->empty_flat_rounds >= 2U) &&
                     (st->empty_min_rtt_us > base + shift_thresh);
    bool stuck_rose = (st->empty_no_drop_rounds >= 2U) &&
                      (st->saved_cwnd_q10 <= (6ULL << 10)) &&
                      (st->empty_min_rtt_us > base + max_t(u64, 15000ULL, base / 4ULL));
    bool base_rose = flat_rose || stuck_rose;
    u64 new_base = base_rose
                       ? st->empty_min_rtt_us
                       : ((st->empty_min_rtt_us > 0 && st->empty_min_rtt_us < base)
                              ? st->empty_min_rtt_us
                              : base);

    if (base_rose) {
        st->base_rtt_us = new_base;
        copa_v3_mm_reset(&st->win_rtt_min, now_us, new_base);
        copa_v3_mm_reset(&st->recent_rtt_min, now_us, new_base);
        st->competitive = false;
        st->last_nearly_empty_us = now_us;
        st->base_shift_since_us = 0;
        st->k_us = 0;
        st->has_k = false;
        st->dq_target_us = 0;
        st->delta_run_q20 = 0;
        st->delta_q20 = st->cfg.init_delta_q20;
        copa_v3_reset_diffusion(st);
    } else if (new_base < base) {
        st->base_rtt_us = new_base;
    }

    if (!copa_v3_fixed_j(&st->cfg) && st->empty_stand_cnt >= 2U) {
        u64 old_j = st->jitter_us;
        u64 mean_stand = div64_u64(st->empty_stand_sum_us, (u64)st->empty_stand_cnt);
        u64 measured_j = (mean_stand > new_base) ? (mean_stand - new_base) : 0ULL;
        measured_j = min_t(u64, measured_j, COPA_V3_MAX_JITTER_US);

        if (!st->has_jitter || !st->first_empty_done) {
            st->jitter_us = measured_j;
        } else if (st->empty_is_aligned) {
            st->jitter_us = (st->jitter_us + measured_j) / 2ULL;
        } else {
            /* Emergency handover / j-jump probe: track step change promptly */
            st->jitter_us = (st->jitter_us + 3ULL * measured_j) / 4ULL;
        }
        st->has_jitter = true;
        st->low_j_streak = 0;
        st->low_j_sum_us = 0;

        /* Hybrid: when j jumps up significantly (e.g. Starlink reconfig / handover),
         * apply analytical unstarved delta ceiling immediately and arm 1-RTT wakeup! */
        if (st->cfg.variant == COPA_V3_HYBRID && !base_rose &&
            st->jitter_us >= 2500ULL && st->jitter_us > old_j + 1500ULL) {
            u64 bdp_pkts = max_t(u64, st->peak_bdp_q10 >> 10, st->saved_cwnd_q10 >> 10);
            if (bdp_pkts < 16ULL)
                bdp_pkts = 16ULL;
            {
                u64 sd = div64_u64((new_base + st->jitter_us) << 20, bdp_pkts * st->jitter_us);
                u32 sd_clamped = clamp_t(u32, (u32)min_t(u64, sd, (u64)COPA_V3_DELTA_MAX_Q20),
                                         15729U, COPA_V3_DELTA_MAX_Q20); /* floor 0.015 */
                if (st->delta_q20 > sd_clamped) {
                    st->delta_q20 = sd_clamped;
                    st->delta_run_q20 = sd_clamped;
                }
            }
            st->idle_wakeup_pending = true;
            st->idle_wakeup_start_us = now_us;
        }
    }
    st->first_empty_done = true;

    /* Update empty-queue raw RTT increment variance 2*sigma_eta^2 for Scheme B */
    if (st->empty_diff_cnt >= 2U) {
        u64 mean_abs = div64_u64(st->empty_diff_sum_us, (u64)st->empty_diff_cnt);
        u64 var_us2 = div64_u64(157ULL * mean_abs * mean_abs, 100ULL);
        st->empty_var_us2 = st->empty_var_us2 ? (st->empty_var_us2 + var_us2) / 2ULL : var_us2;
    }

    /* Restore cwnd smoothly so throughput rebounds in 1 RTT; reset TCPCOOP timer */
    st->cwnd_q10 = max_t(u64, st->saved_cwnd_q10, (u64)COPA_V3_MIN_CWND_PKTS << 10);
    st->phase = COPA_V3_PHASE_CRUISE;
    st->phase_pacing_bps = 0;
    st->last_nearly_empty_us = now_us;
    st->diff_suppress_until_us = now_us + max_t(u64, st->srtt_us, 20000ULL);
    copa_v3_reset_velocity(st);
    copa_v3_reset_cycle(st, now_us);
}

/* Scheme A: start 1-RTT gentle ProbeBW (1.25x) */
static inline void copa_v3_start_probe_bw(struct copa_v3_state *st, u64 now_us, u32 mss)
{
    u64 stand = max_t(u64, st->standing_us, max_t(u64, st->base_rtt_us, 10000ULL));
    u64 cwnd_bytes = (st->cwnd_q10 * (u64)mss) >> 10;
    u64 cwnd_rate = div64_u64(cwnd_bytes * 1000000ULL, stand);
    u64 base_rate = st->bw_ewma_bps > 0 ? max_t(u64, st->bw_ewma_bps, (cwnd_rate * 8ULL) / 10ULL)
                                        : cwnd_rate;
    u64 srtt = max_t(u64, st->srtt_us, 10000ULL);

    if (base_rate == 0)
        return;

    st->phase = COPA_V3_PHASE_PROBE_UP;
    st->phase_start_us = now_us;
    st->phase_deadline_us = now_us + srtt;
    st->saved_cwnd_q10 = st->cwnd_q10;
    st->probe_base_rate_bps = base_rate;
    st->probe_dq_before_us = st->dq_us;
    st->probe_obs_rate_sum = 0;
    st->probe_obs_rate_cnt = 0;
    st->probe_obs_max_rate = 0;
    st->probe_obs_max_dq_us = 0;
    st->phase_pacing_bps = (base_rate * 125ULL) / 100ULL;
    st->cwnd_q10 = clamp_t(u64, (st->saved_cwnd_q10 * 125ULL) / 100ULL + (1ULL << 10),
                           (u64)COPA_V3_MIN_CWND_PKTS << 10,
                           (u64)COPA_V3_MAX_CWND_PKTS << 10);
    st->cycle_dirty = true;
}

/*
 * Outer-loop update (every 4 steady RTTs in CRUISE):
 *   1. Estimate k_us using either Scheme A (ProbeBW) or Scheme B (Diffusion)
 *   2. Compute closed-form d_{q,theta}* from (j, k, R0', theta)
 *   3. Apply paper Prop. 9 AIMD update on 1/delta
 */
static inline void copa_v3_outer_update(struct copa_v3_state *st, u64 now_us, u32 mss)
{
    u64 base_rtt = st->base_rtt_us;
    u64 j = st->jitter_us;
    u64 cycle_dq_us, cycle_w_us, pkt_us = 0;
    u64 k_sample_us = 0;
    bool have_k_sample = false;
    u64 bdp_pkts = 0;

    if (st->cycle_acks == 0 || base_rtt == 0)
        return;

    cycle_dq_us = div64_u64(st->cycle_dq_sum_us, st->cycle_acks);
    /* Physical invariant: only lower j when loaded dq stays persistently well below j
     * across 5 consecutive cycles (~20 RTTs) — avoids downward-ratchet bias on random jitter! */
    if (!copa_v3_fixed_j(&st->cfg) && st->has_jitter && st->jitter_us > 0) {
        if (cycle_dq_us * 100ULL < st->jitter_us * 70ULL) {
            st->low_j_sum_us += cycle_dq_us;
            if (++st->low_j_streak >= 5U) {
                u64 mean_low = div64_u64(st->low_j_sum_us, (u64)st->low_j_streak);
                st->jitter_us = (st->jitter_us + mean_low) / 2ULL;
                j = st->jitter_us;
                st->low_j_streak = 0;
                st->low_j_sum_us = 0;
            }
        } else {
            st->low_j_streak = 0;
            st->low_j_sum_us = 0;
        }
    }
    cycle_w_us = (cycle_dq_us > j) ? (cycle_dq_us - j) : 0ULL;

    if (st->bw_ewma_bps > 0 && mss > 0) {
        u64 bdp_bytes = div64_u64(st->bw_ewma_bps * base_rtt, 1000000ULL);
        bdp_pkts = div64_u64(bdp_bytes, (u64)mss);
        pkt_us = div64_u64((u64)mss * 1000000ULL, st->bw_ewma_bps);
    }

    if (st->cfg.variant == COPA_V3_PROBE_BW) {
        /* ---------- Scheme A: BBR-style unbiased-mean ProbeBW -> c, k ---------- */
        if (st->mu_hat_bps > 0 && st->cycle_rate_cnt > 0) {
            u64 lambda_bps = div64_u64(st->cycle_rate_sum_bps, (u64)st->cycle_rate_cnt);
            u64 mu_pkt_us = (mss > 0) ? div64_u64((u64)mss * 1000000ULL, st->mu_hat_bps) : pkt_us;
            u64 dd1_floor = max_t(u64, 500ULL, 2ULL * mu_pkt_us);
            u64 raw_rho_m = div64_u64(lambda_bps * 1000ULL, st->mu_hat_bps);
            u32 rho_m = (u32)clamp_t(u64, raw_rho_m, 50ULL, 990ULL);
            st->rho_milli = rho_m;

            if (raw_rho_m >= 980ULL && cycle_w_us <= max_t(u64, dd1_floor * 2ULL, 2000ULL)) {
                /* Clean D/D/1 wired regime: essentially 100% full at tiny queue */
                k_sample_us = 0;
                st->c_milli = 0;
            } else if (cycle_w_us <= dd1_floor && rho_m >= 900U) {
                k_sample_us = 0;
                st->c_milli = 0;
            } else if (cycle_w_us > 0) {
                k_sample_us = div64_u64(cycle_w_us * (u64)(1000U - rho_m), (u64)rho_m);
                /* c_i = mu_i [pkts/s] * k [s] = (mu_hat_bps / mss) * k_us / 1e6 */
                if (mss > 0) {
                    u64 c_m = div64_u64(div64_u64(st->mu_hat_bps, (u64)mss) * k_sample_us, 1000ULL);
                    st->c_milli = (u32)min_t(u64, c_m, 1000000ULL);
                }
            } else {
                k_sample_us = 0;
                st->c_milli = 0;
            }
            have_k_sample = true;
        }
    } else {
        /* ---------- Scheme B / Hybrid: Passive multi-scale Queue-Diffusion -> k ---------- */
        u64 vw_us = 0;
        u64 raw_w_mean = div64_u64(st->cycle_raw_w_sum_us, st->cycle_acks);
        u64 dd1_floor = max_t(u64, 500ULL, 2ULL * pkt_us);
        bool dd1_confirmed_by_mu = false;

        if (st->cfg.variant == COPA_V3_HYBRID && st->mu_hat_bps > 0 && st->cycle_rate_cnt > 0) {
            u64 lambda_bps = div64_u64(st->cycle_rate_sum_bps, (u64)st->cycle_rate_cnt);
            if (lambda_bps * 100ULL >= st->mu_hat_bps * 96ULL &&
                cycle_w_us <= max_t(u64, dd1_floor * 2ULL, 1500ULL)) {
                dd1_confirmed_by_mu = true;
            }
        }

        if ((st->cycle_diff_samples == 0 && cycle_w_us <= dd1_floor) || dd1_confirmed_by_mu) {
            /* Quiet cycle or mu_hat-confirmed D/D/1 wired link */
            st->diff_vw_us = 0;
            k_sample_us = 0;
            st->c_milli = 0;
            have_k_sample = true;
            if (++st->quiet_diff_cycles >= 2U)
                copa_v3_reset_diffusion(st);
        } else {
            bool valid_vw = copa_v3_compute_diffusion_vw(st, &vw_us);
            st->quiet_diff_cycles = 0;
            st->diff_vw_us = vw_us;
            if (valid_vw && vw_us > 0) {
                u64 eff_w = max_t(u64, max_t(u64, cycle_w_us, raw_w_mean), 1ULL);
                u64 one_minus_rho_m = min_t(u64, div64_u64(vw_us * 1000ULL, eff_w), 400ULL);
                u32 rho_m = (u32)clamp_t(u64, 1000ULL - one_minus_rho_m, 600ULL, 960ULL);

                st->rho_milli = rho_m;
                if (cycle_w_us <= max_t(u64, dd1_floor, 1200ULL) && vw_us < 35ULL) {
                    k_sample_us = 0;
                } else {
                    k_sample_us = div64_u64(vw_us * 1000ULL, (u64)rho_m);
                }
                if (mss > 0 && st->bw_ewma_bps > 0) {
                    u64 mu_est_pkts = div64_u64(st->bw_ewma_bps * 1000ULL, (u64)mss * (u64)rho_m);
                    st->c_milli = (u32)min_t(u64, div64_u64(mu_est_pkts * k_sample_us, 1000ULL),
                                             1000000ULL);
                }
                have_k_sample = true;
            } else if (st->cfg.variant == COPA_V3_HYBRID && !st->has_k &&
                       st->mu_hat_bps > 0 && st->cycle_rate_cnt > 0 && cycle_w_us > dd1_floor) {
                /* Hybrid cold-start prior: use Scheme A's mu_hat before B fills its ring */
                u64 lambda_bps = div64_u64(st->cycle_rate_sum_bps, (u64)st->cycle_rate_cnt);
                u32 rho_m = (u32)clamp_t(u64, div64_u64(lambda_bps * 1000ULL, st->mu_hat_bps),
                                         100ULL, 980ULL);
                st->rho_milli = rho_m;
                k_sample_us = div64_u64(cycle_w_us * (u64)(1000U - rho_m), (u64)rho_m);
                have_k_sample = true;
            } else if (cycle_w_us <= dd1_floor) {
                k_sample_us = 0;
                st->c_milli = 0;
                have_k_sample = true;
            }
        }
    }

    if (have_k_sample)
        copa_v3_smooth_k(st, k_sample_us);

    /* Compute closed-form optimal target d_{q,theta}* */
    {
        u64 r0_prime = base_rtt + j;
        u64 dd1_floor = max_t(u64, 500ULL, 2ULL * pkt_us);
        u64 target_us;

        if (st->has_k && st->k_us > 0) {
            target_us = copa_target_dq_us(j, st->k_us, r0_prime, st->cfg.theta_milli);
        } else {
            target_us = j;
        }

        /* Empty-queue wakeup on high-jitter links (j >= 1 ms) when W == 0:
         * On jittery links (e.g. Starlink 375 Mbps), delta=0.5 starves rate because
         * lambda * j = 1/delta; provide a small exploration margin above j so the
         * queue wakes up and both estimators get real signal. */
        if (j >= 1000ULL && cycle_w_us <= max_t(u64, 500ULL, pkt_us)) {
            st->empty_q_streak++;
            if (target_us <= j + 1500ULL)
                target_us = j + max_t(u64, 1500ULL, j / 4ULL);
        } else {
            st->empty_q_streak = 0;
        }

        st->dq_target_us = target_us;

        /* Clean D/D/1 wired link: target is 0 (or within 2-pkt floor with j < 1 ms) -> return delta to 0.50 */
        if (target_us == 0 ||
            (j < 1000ULL && target_us <= j + dd1_floor && cycle_dq_us <= j + dd1_floor)) {
            u32 cur = st->delta_run_q20 ? st->delta_run_q20 : st->delta_q20;
            u32 next = min_t(u32, (cur * 5U) / 4U, COPA_V3_DELTA_MAX_Q20);
            st->delta_run_q20 = next;
            st->delta_q20 = next;
            return;
        }

        /* Paper Prop. 9 AIMD outer loop on 1/delta */
        if (target_us > 0) {
            u64 eff_dq = max_t(u64, cycle_dq_us, 10ULL);
            u32 cur_d = st->delta_run_q20 ? st->delta_run_q20 : st->delta_q20;
            u32 next_d;

            if (eff_dq >= target_us) {
                /* Multiplicative decrease of 1/delta (increase delta): delta * (dq / dq_target)^0.25 */
                s64 err_q16 = clamp_t(s64, copa_ln_ratio_q16(eff_dq, target_us),
                                      0LL, COPA_V3_MAX_STEP_Q16);
                u64 mult_q16 = copa_exp_q16(err_q16 >> 2); /* eta = 0.25 */
                u64 nd = ((u64)cur_d * mult_q16) >> COPA_Q16_SHIFT;
                next_d = clamp_t(u32, (u32)min_t(u64, nd, (u64)COPA_V3_DELTA_MAX_Q20),
                                 COPA_V3_DELTA_MIN_Q20, COPA_V3_DELTA_MAX_Q20);
            } else {
                /* Additive increase of 1/delta: 1/delta += a * ln(dq_target / dq), a = 10 pkts */
                s64 err_q16 = clamp_t(s64, copa_ln_ratio_q16(target_us, eff_dq),
                                      0LL, COPA_V3_MAX_STEP_Q16);
                u64 inv_q16 = div64_u64(1ULL << 36, max_t(u32, cur_d, 1U)); /* (1/delta) in Q16 */
                u64 a_pkts = 10ULL;
                /* On very high-BDP jitter-starved links, allow proportional boost to converge fast */
                if (j >= 2500ULL && st->empty_q_streak >= 2U && bdp_pkts > 60ULL)
                    a_pkts = min_t(u64, max_t(u64, bdp_pkts / 6ULL, 10ULL), 100ULL);
                inv_q16 += a_pkts * (u64)err_q16;
                next_d = (u32)div64_u64(1ULL << 36, max_t(u64, inv_q16, 1ULL));
                next_d = clamp_t(u32, next_d, COPA_V3_DELTA_MIN_Q20, COPA_V3_DELTA_MAX_Q20);
            }
            st->delta_run_q20 = next_d;
            st->delta_q20 = next_d;
        }
    }
    (void)now_us;
}

/* Optional delay-only competitive mode check (standard Copa TCPCOOP; zero loss triggers) */
static inline void copa_v3_update_mode(struct copa_v3_state *st, u64 now_us, u64 recent_min_us)
{
    u64 rtt_max_us = copa_v3_mm_get(&st->rtt_max);
    u64 base = st->base_rtt_us;
    u64 span_us = (rtt_max_us > base) ? (rtt_max_us - base) : 0ULL;
    u64 dq_us = st->dq_us;
    u64 shift_thresh = max_t(u64, 5000ULL, (base * 15ULL) / 100ULL);
    bool floor_shifted = (base > 0) &&
                         (recent_min_us > base + max_t(u64, shift_thresh, 10000ULL)) &&
                         (st->last_handover_probe_us == 0 ||
                          now_us >= st->last_handover_probe_us + 4000000ULL);
    bool nearly_empty;

    if (!st->cfg.mode_switching)
        return;

    /* Never falsely trigger TCPCOOP while verifying R0/j or when a base-RTT jump needs verification */
    if (st->phase == COPA_V3_PHASE_PROBE_EMPTY || floor_shifted) {
        st->last_nearly_empty_us = now_us;
        return;
    }

    if (st->has_jitter && st->jitter_us < 15000ULL) {
        u64 eff_dq = (dq_us > st->jitter_us) ? (dq_us - st->jitter_us) : 0ULL;
        u64 eff_span = (span_us > st->jitter_us) ? (span_us - st->jitter_us) : 0ULL;
        nearly_empty = (eff_dq <= 1000ULL) || (eff_dq <= eff_span / 2ULL + 100ULL);
    } else {
        nearly_empty = (dq_us == 0) || (dq_us <= span_us / 2ULL + 100ULL);
    }
    if (!st->competitive) {
        u64 quiet_ceil = max_t(u64, 2ULL * st->dq_target_us, st->jitter_us + 15000ULL);
        if (st->last_ack_app_limited || dq_us <= quiet_ceil)
            nearly_empty = true;
    }

    if (nearly_empty)
        st->last_nearly_empty_us = now_us;

    {
        bool want_comp = (now_us > st->last_nearly_empty_us) &&
                         ((now_us - st->last_nearly_empty_us) > 10ULL * st->srtt_us);
        if (want_comp != st->competitive) {
            st->competitive = want_comp;
            if (want_comp) {
                st->phase = COPA_V3_PHASE_CRUISE;
                st->phase_pacing_bps = 0;
                st->delta_q20 = clamp_t(u32, st->delta_q20, 20972U, COPA_V3_DELTA_MAX_Q20);
                st->last_comp_backoff_us = 0;
            } else {
                st->delta_q20 = st->delta_run_q20 ? st->delta_run_q20 : st->cfg.init_delta_q20;
            }
        }
    }
}

/*
 * Main per-ACK congestion control step.
 * Returns updated congestion window in packets.
 */
static inline u32 copa_v3_on_ack(struct copa_v3_state *st,
                                 u64 now_us,
                                 u64 rtt_sample_us,
                                 u64 srtt_us,
                                 u32 acked_pkts,
                                 u32 mss,
                                 u64 rate_bps,
                                 bool app_limited,
                                 bool low_flight)
{
    u64 prev_time = st->last_time_us;
    u64 win_rtt_min_us, recent_min_us, standing_us, dq_us, raw_dq_us;
    u64 srtt_eff;
    bool increase;

    if (now_us > st->last_time_us)
        st->last_time_us = now_us;
    if (mss > 0)
        st->last_mss = mss;
    if (rtt_sample_us == 0)
        return (u32)(st->cwnd_q10 >> 10);
    if (srtt_us == 0)
        srtt_us = rtt_sample_us;
    st->srtt_us = srtt_us;
    srtt_eff = max_t(u64, srtt_us, 10000ULL);

    /* Track base RTT R0 (immediate downward update) */
    if (st->base_rtt_us == 0 || rtt_sample_us < st->base_rtt_us)
        st->base_rtt_us = rtt_sample_us;

    /* Application-idle pause handling */
    if (prev_time > 0 && now_us > prev_time &&
        (now_us - prev_time) >= max_t(u64, 4ULL * srtt_eff, 300000ULL)) {
        u64 gap_us = now_us - prev_time;

        st->phase = COPA_V3_PHASE_CRUISE;
        st->phase_pacing_bps = 0;
        st->loaded_since_us = 0;
        st->competitive = false;
        st->last_nearly_empty_us = now_us;
        if (st->cfg.variant == COPA_V3_HYBRID) {
            /* Arm 1-RTT-gated idle wakeup (ProbeUp only if link didn't degrade!) */
            st->idle_wakeup_pending = true;
            st->idle_wakeup_start_us = 0;
            if (gap_us >= 1000000ULL) {
                /* Long idle (>= 1 s): link may have changed; flush stale B & A state */
                copa_v3_reset_diffusion(st);
                st->mu_cnt = 0;
                st->mu_head = 0;
                st->mu_hat_bps = 0;
                /* Use low-flight (empty-pipe) ACKs in the first RTT after idle to detect R0 shift */
                st->idle_r0_check_until_us = now_us + srtt_eff;
                st->idle_r0_min_us = low_flight ? rtt_sample_us : 0ULL;
                st->idle_r0_samples = low_flight ? 1U : 0U;
            }
        }
        if (st->last_ack_app_limited) {
            u64 decayed = st->peak_bdp_q10;
            if (gap_us > 1000000ULL)
                decayed = (decayed * 7ULL) / 10ULL;
            if (gap_us > 2000000ULL)
                decayed = (decayed * 7ULL) / 10ULL;
            st->peak_bdp_q10 = decayed;
            if (decayed / 2ULL > st->cwnd_q10)
                st->cwnd_q10 = min_t(u64, max_t(u64, 10ULL << 10, decayed / 2ULL),
                                     (u64)COPA_V3_MAX_CWND_PKTS << 10);
            if (decayed == 0 || st->cwnd_q10 < (decayed * 3ULL) / 4ULL) {
                st->slow_start = true;
                st->ss_confirm_since_us = 0;
            }
            copa_v3_reset_velocity(st);
        }
        copa_v3_reset_cycle(st, now_us);
    }

    /* Hybrid: complete 1-RTT empty-pipe base-RTT check after a >= 1 s idle pause (low_flight only!) */
    if (st->cfg.variant == COPA_V3_HYBRID && st->idle_r0_check_until_us > 0) {
        if (low_flight) {
            if (st->idle_r0_min_us == 0 || rtt_sample_us < st->idle_r0_min_us)
                st->idle_r0_min_us = rtt_sample_us;
            st->idle_r0_samples++;
        }
        if (now_us >= st->idle_r0_check_until_us) {
            u64 shift_thresh = max_t(u64, 5000ULL, (st->base_rtt_us * 15ULL) / 100ULL);
            if (st->idle_r0_samples >= 2U &&
                st->idle_r0_min_us > st->base_rtt_us + st->jitter_us + shift_thresh) {
                st->base_rtt_us = st->idle_r0_min_us;
                copa_v3_mm_reset(&st->win_rtt_min, now_us, st->base_rtt_us);
                copa_v3_mm_reset(&st->recent_rtt_min, now_us, st->base_rtt_us);
                st->k_us = 0;
                st->has_k = false;
                st->dq_target_us = 0;
                st->delta_run_q20 = 0;
                st->delta_q20 = st->cfg.init_delta_q20;
            }
            st->idle_r0_check_until_us = 0;
        }
    }

    /* Track loaded intervals vs app-limited intervals */
    if (srtt_us > 4ULL * rtt_sample_us ||
        (st->last_loaded_ack_us > 0 &&
         now_us > st->last_loaded_ack_us + max_t(u64, 2ULL * srtt_eff, 50000ULL))) {
        st->loaded_since_us = 0;
    }
    if (!app_limited && srtt_us <= 4ULL * rtt_sample_us) {
        if (st->cfg.variant == COPA_V3_HYBRID && st->app_lim_since_us > 0 &&
            now_us >= st->app_lim_since_us + max_t(u64, 4ULL * srtt_eff, 250000ULL)) {
            st->idle_wakeup_pending = true;
            st->idle_wakeup_start_us = now_us;
        }
        st->app_lim_since_us = 0;
        st->last_loaded_ack_us = now_us;
        if (st->loaded_since_us == 0)
            st->loaded_since_us = now_us;
    }
    st->last_ack_app_limited = app_limited;
    if (app_limited) {
        if (st->app_lim_since_us == 0)
            st->app_lim_since_us = now_us;
        st->seen_app_limited = true;
        st->last_app_limited_us = now_us;
    }

    /* Passive low-flight j/R0 learning */
    copa_v3_observe_low(st, now_us, rtt_sample_us,
                        low_flight && st->phase == COPA_V3_PHASE_CRUISE &&
                            !st->competitive && srtt_us <= 4ULL * rtt_sample_us);

    /* Update RTT filters */
    win_rtt_min_us = copa_v3_mm_min(&st->win_rtt_min, COPA_V3_MIN_RTT_WIN_US, now_us, rtt_sample_us);
    recent_min_us = copa_v3_mm_min(&st->recent_rtt_min,
                                   clamp_t(u64, 8ULL * srtt_eff, 600000ULL, 2000000ULL),
                                   now_us, rtt_sample_us);
    standing_us = copa_v3_mm_min(&st->rtt_standing, max_t(u64, srtt_us / 2ULL, 1000ULL),
                                 now_us, rtt_sample_us);
    copa_v3_mm_max(&st->rtt_max, max_t(u64, 4ULL * srtt_us, 40000ULL), now_us, rtt_sample_us);
    st->standing_us = standing_us;
    dq_us = (standing_us > st->base_rtt_us) ? (standing_us - st->base_rtt_us) : 0ULL;
    raw_dq_us = (rtt_sample_us > st->base_rtt_us) ? (rtt_sample_us - st->base_rtt_us) : 0ULL;
    st->dq_us = dq_us;

    /* Smooth delivery-rate EWMA (used for BDP / drain pacing / Scheme A) */
    if (rate_bps > 0 && (!app_limited || rate_bps > st->bw_ewma_bps)) {
        st->bw_ewma_bps = st->bw_ewma_bps ? (7ULL * st->bw_ewma_bps + rate_bps) / 8ULL : rate_bps;
        if (mss > 0) {
            u64 bdp_q10 = div64_u64(div64_u64(st->bw_ewma_bps * st->base_rtt_us, 1000000ULL) << 10,
                                    (u64)mss);
            if (bdp_q10 > st->peak_bdp_q10)
                st->peak_bdp_q10 = bdp_q10;
        }
    }

    /* Check optional delay-only competitive mode */
    copa_v3_update_mode(st, now_us, recent_min_us);

    /* ==================== PHASE: PROBE_EMPTY (shared) ==================== */
    if (st->phase == COPA_V3_PHASE_PROBE_EMPTY) {
        u64 elapsed = (now_us > st->phase_start_us) ? (now_us - st->phase_start_us) : 0ULL;
        u64 min_hold = max_t(u64, srtt_us, 25000ULL);

        if (st->empty_min_rtt_us == 0 || rtt_sample_us < st->empty_min_rtt_us)
            st->empty_min_rtt_us = rtt_sample_us;
        if (st->empty_round_min_us == 0 || rtt_sample_us < st->empty_round_min_us)
            st->empty_round_min_us = rtt_sample_us;

        if (now_us >= st->empty_round_start_us + srtt_eff) {
            if (st->empty_round_min_us > 0) {
                if (st->empty_prev_round_min_us > 0) {
                    u64 diff = (st->empty_prev_round_min_us > st->empty_round_min_us)
                                   ? (st->empty_prev_round_min_us - st->empty_round_min_us)
                                   : (st->empty_round_min_us - st->empty_prev_round_min_us);
                    u64 tol = max_t(u64, 1500ULL, st->empty_round_min_us / 25ULL);
                    if (diff <= tol)
                        st->empty_flat_rounds++;
                    else
                        st->empty_flat_rounds = 0;
                }
                if (st->empty_prev_cum_min_us > 0) {
                    u64 cum_tol = max_t(u64, 1000ULL, st->empty_min_rtt_us / 20ULL);
                    if (st->empty_prev_cum_min_us <= st->empty_min_rtt_us + cum_tol)
                        st->empty_no_drop_rounds++;
                    else
                        st->empty_no_drop_rounds = 0;
                }
                st->empty_prev_cum_min_us = st->empty_min_rtt_us;
                st->empty_prev_round_min_us = st->empty_round_min_us;
                st->empty_round_min_us = 0;
            }
            st->empty_round_start_us = now_us;
        }

        if (st->empty_idle_at_us == 0 && elapsed >= min_hold) {
            u64 tol = max_t(u64, 1000ULL, st->base_rtt_us / 20ULL);
            bool back = standing_us <= st->base_rtt_us + st->jitter_us + tol;
            bool plat = (elapsed >= 2ULL * srtt_eff) &&
                        (st->empty_flat_rounds >= 2U || st->empty_no_drop_rounds >= 2U);
            if (back || plat) {
                u64 slen = clamp_t(u64, 2ULL * srtt_us, 60000ULL, 180000ULL);
                st->empty_idle_at_us = now_us;
                st->empty_sample_until_us = min_t(u64, now_us + slen, st->phase_deadline_us);
            }
        }

        if (st->empty_idle_at_us > 0) {
            u64 bidx = div64_u64(now_us, COPA_V3_DIFF_BIN_US);
            st->empty_stand_sum_us += standing_us;
            st->empty_stand_cnt++;

            /* Measure empty-queue 10-ms raw RTT increment magnitude for 2*sigma_eta^2 */
            if (!st->empty_has_prev_bin && st->empty_last_bin_idx == 0) {
                st->empty_last_bin_idx = bidx;
                st->empty_cur_bin_min_us = (u32)min_t(u64, rtt_sample_us, 10000000ULL);
            } else if (bidx == st->empty_last_bin_idx) {
                u32 v = (u32)min_t(u64, rtt_sample_us, 10000000ULL);
                if (v < st->empty_cur_bin_min_us)
                    st->empty_cur_bin_min_us = v;
            } else {
                if (st->empty_has_prev_bin && bidx <= st->empty_last_bin_idx + 2ULL) {
                    u32 a = st->empty_cur_bin_min_us;
                    u32 b = st->empty_prev_bin_min_us;
                    st->empty_diff_sum_us += (a > b) ? (u64)(a - b) : (u64)(b - a);
                    st->empty_diff_cnt++;
                }
                st->empty_prev_bin_min_us = st->empty_cur_bin_min_us;
                st->empty_has_prev_bin = true;
                st->empty_last_bin_idx = bidx;
                st->empty_cur_bin_min_us = (u32)min_t(u64, rtt_sample_us, 10000000ULL);
            }
        }

        if (now_us >= st->phase_deadline_us ||
            (st->empty_sample_until_us > 0 && now_us >= st->empty_sample_until_us)) {
            copa_v3_finish_probe_empty(st, now_us);
        } else {
            /* Gentle ramp cwnd to 65% of saved cwnd (floor 4 pkts) */
            u64 target_q10 = max_t(u64, (st->saved_cwnd_q10 * 65ULL) / 100ULL,
                                   (u64)COPA_V3_MIN_CWND_PKTS << 10);
            u64 frac = min_t(u64, div64_u64(elapsed * 1024ULL, srtt_eff), 1024ULL);
            st->cwnd_q10 = ((st->saved_cwnd_q10 * (1024ULL - frac)) + (target_q10 * frac)) >> 10;
            st->cwnd_q10 = max_t(u64, st->cwnd_q10, (u64)COPA_V3_MIN_CWND_PKTS << 10);
            return (u32)(st->cwnd_q10 >> 10);
        }
    }

    /* ==================== PHASE: PROBE_UP (Scheme A only) ==================== */
    if (st->phase == COPA_V3_PHASE_PROBE_UP) {
        u64 elapsed = (now_us > st->phase_start_us) ? (now_us - st->phase_start_us) : 0ULL;
        u64 abort_dq = st->probe_dq_before_us + max_t(u64, 15000ULL, srtt_eff / 2ULL);

        if (dq_us > st->probe_obs_max_dq_us)
            st->probe_obs_max_dq_us = dq_us;
        /* Early abort if queue already spiked sharply during probe */
        if (now_us >= st->phase_deadline_us || (elapsed >= srtt_eff / 2ULL && dq_us > abort_dq)) {
            st->phase = COPA_V3_PHASE_PROBE_DRAIN;
            st->phase_start_us = now_us;
            st->phase_deadline_us = now_us + srtt_eff;
            st->phase_pacing_bps = max_t(u64, (st->probe_base_rate_bps * 80ULL) / 100ULL,
                                         div64_u64((u64)COPA_V3_MIN_CWND_PKTS * (u64)mss * 1000000ULL,
                                                   srtt_eff));
            st->cwnd_q10 = max_t(u64, st->saved_cwnd_q10, (u64)COPA_V3_MIN_CWND_PKTS << 10);
        }
        return (u32)(st->cwnd_q10 >> 10);
    }

    /* ==================== PHASE: PROBE_DRAIN (Scheme A only) ==================== */
    if (st->phase == COPA_V3_PHASE_PROBE_DRAIN) {
        u64 elapsed = (now_us > st->phase_start_us) ? (now_us - st->phase_start_us) : 0ULL;
        /* ACKs arriving during PROBE_DRAIN [0.15*srtt, 0.90*srtt] are the exact 1.25x
         * flight sent during PROBE_UP (the 0.80x drain packets don't return until CRUISE)! */
        if (rate_bps > 0 && !app_limited) {
            if (elapsed >= (srtt_eff * 15ULL) / 100ULL && elapsed <= (srtt_eff * 90ULL) / 100ULL) {
                st->probe_obs_rate_sum += rate_bps;
                st->probe_obs_rate_cnt++;
            }
            if (rate_bps > st->probe_obs_max_rate)
                st->probe_obs_max_rate = rate_bps;
        }
        if (dq_us > st->probe_obs_max_dq_us)
            st->probe_obs_max_dq_us = dq_us;

        if (now_us >= st->phase_deadline_us) {
            u64 obs_rate = st->probe_obs_rate_cnt > 0
                               ? div64_u64(st->probe_obs_rate_sum, (u64)st->probe_obs_rate_cnt)
                               : st->probe_obs_max_rate;
            u64 rise_thresh = st->probe_dq_before_us + max_t(u64, 4000ULL, st->jitter_us);
            bool queue_rose = (st->probe_obs_max_dq_us > rise_thresh);
            bool grew_cleanly = (!queue_rose) && (obs_rate * 100ULL >= st->probe_base_rate_bps * 114ULL);

            st->last_probebw_us = now_us;
            if (grew_cleanly && st->probe_escalations < 4U) {
                /* Under-utilized (e.g. Starlink 375M / step-up): keep gain & probe again! */
                st->probe_escalations++;
                st->cwnd_q10 = clamp_t(u64, (st->saved_cwnd_q10 * 125ULL) / 100ULL + (1ULL << 10),
                                       (u64)COPA_V3_MIN_CWND_PKTS << 10,
                                       (u64)COPA_V3_MAX_CWND_PKTS << 10);
                /* Only relax delta during escalation when jitter > 1 ms can actually starve delta=0.5 */
                if (st->jitter_us >= 1000ULL && st->delta_q20 > COPA_V3_DELTA_MIN_Q20) {
                    u32 nd = clamp_t(u32, (st->delta_q20 * 75U) / 100U,
                                     COPA_V3_DELTA_MIN_Q20, COPA_V3_DELTA_MAX_Q20);
                    st->delta_run_q20 = nd;
                    st->delta_q20 = nd;
                }
                st->bw_ewma_bps = obs_rate;
                copa_v3_start_probe_bw(st, now_us, mss);
                return (u32)(st->cwnd_q10 >> 10);
            }

            /* Pipe saturated (or escalation cap reached): record unbiased C_m sample */
            st->probe_escalations = 0;
            if (obs_rate > 0)
                copa_v3_record_mu_sample(st, obs_rate);
            st->phase = COPA_V3_PHASE_CRUISE;
            st->phase_pacing_bps = 0;
            st->cwnd_q10 = max_t(u64, st->saved_cwnd_q10, (u64)COPA_V3_MIN_CWND_PKTS << 10);
            st->diff_suppress_until_us = now_us + srtt_eff;
            st->cur_bin_valid = false;
            copa_v3_reset_velocity(st);
            copa_v3_reset_cycle(st, now_us);
        } else {
            return (u32)(st->cwnd_q10 >> 10);
        }
    }

    /* Track 4-s sliding max cwnd for Hybrid "cwnd shrunk without queue drop" j-jump guard */
    copa_v3_mm_max(&st->recent_cwnd_max, 4000000ULL, now_us, st->cwnd_q10);

    /* Verify base-RTT handover jumps (and Hybrid j-jumps) promptly before TCPCOOP locks in! */
    if (!low_flight && !st->slow_start && st->base_rtt_us > 0) {
        u64 shift_thresh = max_t(u64, 5000ULL, (st->base_rtt_us * 15ULL) / 100ULL);
        bool at_min_cwnd = st->cwnd_q10 <= (((u64)COPA_V3_MIN_CWND_PKTS + 1ULL) << 10);
        bool floor_shifted = (recent_min_us > st->base_rtt_us + max_t(u64, shift_thresh, 10000ULL)) ||
                             (at_min_cwnd && standing_us > st->base_rtt_us + st->jitter_us + shift_thresh);

        if (floor_shifted) {
            if (st->base_shift_since_us == 0)
                st->base_shift_since_us = now_us;
            if (now_us >= st->base_shift_since_us + 2ULL * srtt_eff &&
                (st->last_handover_probe_us == 0 ||
                 now_us >= st->last_handover_probe_us + 4000000ULL)) {
                st->last_handover_probe_us = now_us;
                st->base_shift_since_us = 0;
                st->competitive = false;
                copa_v3_start_probe_empty(st, now_us, mss, false);
                return (u32)(st->cwnd_q10 >> 10);
            }
        } else {
            st->base_shift_since_us = 0;
        }

        /* Hybrid: if cwnd shrank >60% into low-window territory yet round-mean dq stays
         * elevated above j, j or R0 likely jumped (e.g. Starlink reconfig) -> PROBE_EMPTY! */
        if (st->cfg.variant == COPA_V3_HYBRID && !copa_v3_fixed_j(&st->cfg) && st->first_empty_done) {
            u64 rmax_cwnd = copa_v3_mm_get(&st->recent_cwnd_max);
            bool shrunk = (rmax_cwnd >= (25ULL << 10)) &&
                          (st->cwnd_q10 * 100ULL < rmax_cwnd * 40ULL) &&
                          (st->cwnd_q10 <= (20ULL << 10));
            if (shrunk) {
                if (st->j_jump_round_start_us == 0)
                    st->j_jump_round_start_us = now_us;
                st->j_jump_dq_sum_us += dq_us;
                st->j_jump_dq_cnt++;
                if (now_us >= st->j_jump_round_start_us + srtt_eff && st->j_jump_dq_cnt > 0) {
                    u64 mean_dq = div64_u64(st->j_jump_dq_sum_us, (u64)st->j_jump_dq_cnt);
                    st->j_jump_round_start_us = now_us;
                    st->j_jump_dq_sum_us = 0;
                    st->j_jump_dq_cnt = 0;
                    if (mean_dq > st->jitter_us + max_t(u64, 4000ULL, st->jitter_us / 2ULL)) {
                        if (++st->j_jump_rounds >= 2U &&
                            (st->last_handover_probe_us == 0 ||
                             now_us >= st->last_handover_probe_us + 3000000ULL)) {
                            st->last_handover_probe_us = now_us;
                            st->j_jump_rounds = 0;
                            st->j_jump_round_start_us = 0;
                            st->competitive = false;
                            copa_v3_start_probe_empty(st, now_us, mss, false);
                            return (u32)(st->cwnd_q10 >> 10);
                        }
                    } else {
                        st->j_jump_rounds = 0;
                    }
                }
            } else {
                st->j_jump_rounds = 0;
                st->j_jump_round_start_us = 0;
                st->j_jump_dq_sum_us = 0;
                st->j_jump_dq_cnt = 0;
            }
        }
    }

    /* ==================== PHASE: CRUISE ==================== */
    if (!st->competitive) {
        /* Analytical startup anti-starvation on high-jitter links before first outer update */
        if (st->has_jitter && st->jitter_us >= 2500ULL && st->delta_run_q20 == 0 && mss > 0) {
            u64 bdp_pkts = st->bw_ewma_bps > 0
                               ? div64_u64(div64_u64(st->bw_ewma_bps * st->base_rtt_us, 1000000ULL),
                                           (u64)mss)
                               : (st->cwnd_q10 >> 10);
            if (bdp_pkts >= 10ULL) {
                u64 sd = div64_u64((st->base_rtt_us + st->jitter_us) << 20,
                                   bdp_pkts * st->jitter_us);
                u32 sd_clamped = clamp_t(u32, (u32)min_t(u64, sd, (u64)COPA_V3_DELTA_MAX_Q20),
                                         20972U, COPA_V3_DELTA_MAX_Q20);
                if (sd_clamped < st->delta_q20)
                    st->delta_q20 = sd_clamped;
            }
        } else if (st->delta_run_q20 > 0) {
            st->delta_q20 = st->delta_run_q20;
        }

        /* ---- Trigger PROBE_EMPTY? (initial or 15s aligned periodic) ---- */
        if (!app_limited && st->loaded_since_us > 0 && !st->slow_start) {
            if (now_us >= st->loaded_since_us + max_t(u64, 3ULL * srtt_us, 200000ULL)) {
                bool severe_backlog = dq_us > max_t(u64, st->base_rtt_us * 2ULL, 50000ULL);
                if (!copa_v3_fixed_j(&st->cfg) && !st->first_empty_done && !severe_backlog) {
                    copa_v3_start_probe_empty(st, now_us, mss, false);
                    return (u32)(st->cwnd_q10 >> 10);
                }
                if (st->first_empty_done && !severe_backlog) {
                    u64 slot = div64_u64(now_us, COPA_V3_EMPTY_PERIOD_US);
                    u64 phase_us = now_us - slot * COPA_V3_EMPTY_PERIOD_US;
                    bool fresh_slot = (!st->has_empty_slot || st->last_empty_slot != slot);
                    bool board_req = st->board && (st->board->slot_req[slot & 3U] == slot + 1ULL);
                    bool passive_recent = st->passive_j_at_us > 0 &&
                                          now_us < st->passive_j_at_us + 10000000ULL;
                    bool need_refresh = !copa_v3_fixed_j(&st->cfg) ||
                                        (win_rtt_min_us > st->base_rtt_us +
                                                              max_t(u64, 1500ULL, st->base_rtt_us / 40ULL));
                    if (slot > 0 && fresh_slot && phase_us < 500000ULL &&
                        (board_req || (!passive_recent && need_refresh))) {
                        st->has_empty_slot = true;
                        st->last_empty_slot = slot;
                        if (st->board && st->board->slot_req[slot & 3U] < slot + 1ULL)
                            st->board->slot_req[slot & 3U] = slot + 1ULL;
                        copa_v3_start_probe_empty(st, now_us, mss, true);
                        return (u32)(st->cwnd_q10 >> 10);
                    }
                }
            }
        }

        /* ---- Scheme A / Hybrid: capacity-drop flush & ProbeBW triggers ---- */
        if ((st->cfg.variant == COPA_V3_PROBE_BW || st->cfg.variant == COPA_V3_HYBRID) &&
            !low_flight && !st->slow_start && st->first_empty_done) {
            /* Fast capacity-drop flush: unmistakably backed-up queue + delivery < 75% mu_hat
             * for 2 RTTs (MUST NOT depend on dq_target_us, which stale mu_hat itself inflates!). */
            u64 pkt_us = (st->mu_hat_bps > 0 && mss > 0)
                             ? div64_u64((u64)mss * 1000000ULL, st->mu_hat_bps)
                             : 0ULL;
            if (st->mu_hat_bps > 0 && rate_bps > 0 &&
                dq_us > st->jitter_us + max_t(u64, 8000ULL, 2ULL * pkt_us) &&
                rate_bps * 100ULL < st->mu_hat_bps * 75ULL) {
                st->cap_drop_rate_sum += rate_bps;
                st->cap_drop_rate_cnt++;
                if (st->cap_drop_round_start_us == 0)
                    st->cap_drop_round_start_us = now_us;
                if (now_us >= st->cap_drop_round_start_us + srtt_eff) {
                    st->cap_drop_round_start_us = now_us;
                    if (++st->cap_drop_rounds >= 2U && st->cap_drop_rate_cnt > 0) {
                        u64 drop_rate = div64_u64(st->cap_drop_rate_sum, (u64)st->cap_drop_rate_cnt);
                        st->mu_ring[0] = drop_rate;
                        st->mu_cnt = 1U;
                        st->mu_head = 1U;
                        st->mu_hat_bps = drop_rate;
                        st->cap_drop_rounds = 0;
                        st->cap_drop_rate_sum = 0;
                        st->cap_drop_rate_cnt = 0;
                    }
                }
            } else {
                st->cap_drop_rounds = 0;
                st->cap_drop_rate_sum = 0;
                st->cap_drop_rate_cnt = 0;
                st->cap_drop_round_start_us = 0;
            }

            if (!app_limited) {
                if (st->cfg.variant == COPA_V3_PROBE_BW) {
                    /* Scheme A: periodic + under-util ProbeBW */
                    u64 period = clamp_t(u64, 40ULL * srtt_eff,
                                         COPA_V3_PROBE_MIN_GAP_US, COPA_V3_PROBE_MAX_GAP_US);
                    bool first_due = (st->last_probebw_us == 0) && (st->loaded_since_us > 0) &&
                                     (now_us >= st->loaded_since_us + max_t(u64, 4ULL * srtt_eff, 250000ULL));
                    bool periodic_due = (st->last_probebw_us > 0) && (now_us >= st->last_probebw_us + period);
                    bool underutil_due = (st->empty_q_streak >= 2U) &&
                                         (st->last_probebw_us == 0 ||
                                          now_us >= st->last_probebw_us + 500000ULL);
                    if (first_due || periodic_due || underutil_due) {
                        st->probe_escalations = 0;
                        copa_v3_start_probe_bw(st, now_us, mss);
                        return (u32)(st->cwnd_q10 >> 10);
                    }
                } else {
                    /* Hybrid (COPA_V3_HYBRID): ZERO periodic ProbeBW; strictly on-demand events! */
                    u64 cur_pkt_us = (st->bw_ewma_bps > 0 && mss > 0)
                                         ? div64_u64((u64)mss * 1000000ULL, st->bw_ewma_bps)
                                         : 0ULL;
                    u64 quiet_thresh = st->jitter_us + max_t(u64, 1500ULL, 2ULL * cur_pkt_us);
                    bool queue_quiet = (dq_us <= quiet_thresh);
                    bool trigger_probe = false;

                    /* Track consecutive quiet RTTs while loaded */
                    if (queue_quiet) {
                        if (st->quiet_empty_round_us == 0)
                            st->quiet_empty_round_us = now_us;
                        if (now_us >= st->quiet_empty_round_us + srtt_eff) {
                            st->quiet_empty_round_us = now_us;
                            st->quiet_empty_rtts++;
                        }
                    } else {
                        st->quiet_empty_rtts = 0;
                        st->quiet_empty_round_us = 0;
                    }

                    /* Event 1: 1-RTT-gated idle wakeup (ProbeUp ONLY if queue stayed flat!) */
                    if (st->idle_wakeup_pending) {
                        if (st->idle_wakeup_start_us == 0) {
                            st->idle_wakeup_start_us = now_us;
                        } else if (now_us >= st->idle_wakeup_start_us + srtt_eff) {
                            st->idle_wakeup_pending = false;
                            if (queue_quiet &&
                                (st->last_probebw_us == 0 ||
                                 now_us >= st->last_probebw_us + 800000ULL)) {
                                trigger_probe = true;
                            }
                        }
                    }

                    /* Event 2: Cold-start initial ignition when queue is quiet & jitter > 0 */
                    if (!trigger_probe && st->last_probebw_us == 0 && st->loaded_since_us > 0 &&
                        queue_quiet && st->jitter_us >= 1000ULL &&
                        now_us >= st->loaded_since_us + max_t(u64, 3ULL * srtt_eff, 200000ULL)) {
                        trigger_probe = true;
                    }

                    /* Event 3: Persistent under-utilization on jittery links or after drop */
                    if (!trigger_probe && st->quiet_empty_rtts >= 4U &&
                        (st->jitter_us >= 1000ULL ||
                         (st->mu_hat_bps > 0 && st->bw_ewma_bps * 100ULL < st->mu_hat_bps * 80ULL)) &&
                        (st->last_probebw_us == 0 ||
                         now_us >= st->last_probebw_us + 1500000ULL)) {
                        trigger_probe = true;
                    }

                    if (trigger_probe) {
                        st->quiet_empty_rtts = 0;
                        st->probe_escalations = 0;
                        copa_v3_start_probe_bw(st, now_us, mss);
                        return (u32)(st->cwnd_q10 >> 10);
                    }
                }
            }
        }

        /* ---- Scheme B / Hybrid: feed passive 10-ms queue-delay bins (skip sender cwnd ramps) ---- */
        if ((st->cfg.variant == COPA_V3_DIFFUSION || st->cfg.variant == COPA_V3_HYBRID) &&
            !st->slow_start) {
            if (st->diff_cwnd_ref_us == 0 || now_us >= st->diff_cwnd_ref_us + srtt_eff) {
                if (st->diff_cwnd_ref_q10 > 0) {
                    u64 hi = max_t(u64, st->cwnd_q10, st->diff_cwnd_ref_q10);
                    u64 lo = min_t(u64, st->cwnd_q10, st->diff_cwnd_ref_q10);
                    if (hi * 100ULL > lo * 125ULL) {
                        /* Rapid sender cwnd ramp/drain (>25% per RTT): suppress diffusion 1 RTT */
                        st->diff_suppress_until_us = now_us + srtt_eff;
                        st->cur_bin_valid = false;
                    }
                }
                st->diff_cwnd_ref_q10 = st->cwnd_q10;
                st->diff_cwnd_ref_us = now_us;
            }
            if (now_us >= st->diff_suppress_until_us) {
                u64 bidx = div64_u64(now_us, COPA_V3_DIFF_BIN_US);
                u32 r_dq = (u32)min_t(u64, raw_dq_us, 10000000ULL);
                u64 pkt_us = (st->bw_ewma_bps > 0 && mss > 0)
                                 ? div64_u64((u64)mss * 1000000ULL, st->bw_ewma_bps)
                                 : 0ULL;
                u64 busy_floor = max_t(u64, 1000ULL, 2ULL * pkt_us);

                if (!st->cur_bin_valid) {
                    st->cur_bin_idx = bidx;
                    st->cur_bin_min_raw_dq_us = r_dq;
                    st->cur_bin_valid = true;
                } else if (bidx == st->cur_bin_idx) {
                    if (r_dq < st->cur_bin_min_raw_dq_us)
                        st->cur_bin_min_raw_dq_us = r_dq;
                } else {
                    copa_v3_diff_push_bin(st, st->cur_bin_idx, st->cur_bin_min_raw_dq_us, busy_floor);
                    st->cur_bin_idx = bidx;
                    st->cur_bin_min_raw_dq_us = r_dq;
                }
            }
        }

        /* ---- Outer-loop 4-RTT cruise sampling (also runs if SndBuf-limited while congested!) ---- */
        if (!low_flight && !st->slow_start && st->first_empty_done &&
            (!app_limited || dq_us > max_t(u64, st->dq_target_us, st->jitter_us + 5000ULL))) {
            if (st->cycle_rounds >= 1U && !st->cycle_dirty) {
                u64 raw_w = (raw_dq_us > st->jitter_us) ? (raw_dq_us - st->jitter_us) : 0ULL;
                st->cycle_dq_sum_us += dq_us;
                st->cycle_raw_w_sum_us += raw_w;
                st->cycle_acks++;
                if (rate_bps > 0) {
                    st->cycle_rate_sum_bps += rate_bps;
                    st->cycle_rate_cnt++;
                }
            }
            if (now_us >= st->cycle_round_start_us + srtt_eff) {
                st->cycle_round_start_us = now_us;
                st->cycle_rounds++;
                if (st->cycle_rounds >= 4U) {
                    if (!st->cycle_dirty && st->cycle_acks >= 4U)
                        copa_v3_outer_update(st, now_us, mss);
                    copa_v3_reset_cycle(st, now_us);
                }
            }
        }
    } else {
        /* Competitive mode (TCPCOOP): additive 1/delta increase per RTT, capped by deep-buffer guard */
        u64 bloat_ceil = max_t(u64, 3ULL * st->base_rtt_us, 90000ULL);
        if (st->delta_q20 < 52429U && dq_us > bloat_ceil) {
            if (dq_us > (bloat_ceil * 3ULL) / 2ULL)
                st->delta_q20 = min_t(u32, (st->delta_q20 * 105U) / 100U, 52429U);
        } else {
            u64 inv_q10 = div64_u64(1ULL << 30, max_t(u32, st->delta_q20, 1U));
            u64 add_q10 = div64_u64((u64)acked_pkts << 20, max_t(u64, st->cwnd_q10, 1024ULL));
            st->delta_q20 = (u32)div64_u64(1ULL << 30, max_t(u64, inv_q10 + add_q10, 1ULL));
            if (st->delta_q20 < COPA_V3_DELTA_MIN_Q20)
                st->delta_q20 = COPA_V3_DELTA_MIN_Q20;
        }
    }

    /* ==================== Pure Copa Inner Loop ==================== */
    if (dq_us == 0) {
        increase = true;
    } else {
        u64 denom = (u64)st->delta_q20 * dq_us;
        u64 target_q10 = denom > 0 ? div64_u64(standing_us << 30, denom)
                                   : ((u64)COPA_V3_MAX_CWND_PKTS << 10);
        increase = (target_q10 >= st->cwnd_q10);
    }

    if (app_limited) {
        if (increase)
            return (u32)(st->cwnd_q10 >> 10);
        if (low_flight)
            return (u32)(st->cwnd_q10 >> 10);
        /* Do NOT let SndBuf-limited app_limited ignore genuine standing queue above dq_target! */
        {
            u64 app_ok = st->dq_target_us > 0
                             ? max_t(u64, st->dq_target_us, st->jitter_us + 2000ULL)
                             : max_t(u64, st->jitter_us + 2000ULL,
                                     min_t(u64, st->base_rtt_us / 6ULL, 10000ULL));
            if (dq_us <= app_ok)
                return (u32)(st->cwnd_q10 >> 10);
        }
    }

    /* Velocity update (once per SRTT) */
    if (!(increase && st->slow_start)) {
        if (st->last_rec_time_us == 0) {
            st->last_rec_time_us = now_us;
            st->last_rec_cwnd_q10 = st->cwnd_q10;
        } else if (now_us >= st->last_rec_time_us + srtt_us) {
            u8 dir = (st->cwnd_q10 > st->last_rec_cwnd_q10) ? COPA_V3_DIR_UP : COPA_V3_DIR_DOWN;
            if (dir != st->direction) {
                st->velocity = 1;
                st->same_direction = 0;
            } else {
                u32 thresh = 3U;
                st->same_direction++;
                if (st->dq_target_us > 0 && dir == COPA_V3_DIR_UP && dq_us * 2ULL < st->dq_target_us)
                    thresh = 1U;
                if (st->same_direction >= thresh && st->velocity < (1U << 20))
                    st->velocity <<= 1;
            }
            {
                u64 cap = ((u64)st->delta_q20 * st->cwnd_q10) >> 30;
                while (st->velocity > 1U && (u64)st->velocity > cap)
                    st->velocity >>= 1;
            }
            st->direction = dir;
            st->last_rec_time_us = now_us;
            st->last_rec_cwnd_q10 = st->cwnd_q10;
        }
    }

    if (increase) {
        st->ss_confirm_since_us = 0;
        st->dec_round_start_us = 0;
        if (st->slow_start) {
            st->cwnd_q10 += (u64)acked_pkts << 10;
        } else {
            u64 num = (u64)acked_pkts * (u64)st->velocity;
            u64 denom = max_t(u64, ((u64)st->delta_q20 * st->cwnd_q10) >> 20, 1ULL);
            st->cwnd_q10 += div64_u64(num << 20, denom);
        }
    } else {
        if (st->slow_start) {
            u64 jguard = max_t(u64, st->jitter_us + 4000ULL,
                               min_t(u64, st->base_rtt_us / 4ULL, 10000ULL));
            if (dq_us < jguard) {
                if (st->ss_confirm_since_us == 0)
                    st->ss_confirm_since_us = now_us;
                if (now_us < st->ss_confirm_since_us + max_t(u64, srtt_us, 20000ULL))
                    return (u32)(st->cwnd_q10 >> 10);
            }
            st->ss_confirm_since_us = 0;
        }
        st->slow_start = false;
        {
            u64 num = (u64)acked_pkts * (u64)st->velocity;
            u64 denom = max_t(u64, ((u64)st->delta_q20 * st->cwnd_q10) >> 20, 1ULL);
            u64 step_q10 = div64_u64(num << 20, denom);

            if (st->dec_round_start_us == 0 || now_us >= st->dec_round_start_us + srtt_eff) {
                st->dec_round_start_us = now_us;
                st->dec_round_floor_q10 = max_t(u64, st->cwnd_q10 >> 1,
                                                (u64)COPA_V3_MIN_CWND_PKTS << 10);
            }
            if (st->cwnd_q10 > step_q10)
                st->cwnd_q10 -= step_q10;
            else
                st->cwnd_q10 = (u64)COPA_V3_MIN_CWND_PKTS << 10;
            if (st->cwnd_q10 < st->dec_round_floor_q10)
                st->cwnd_q10 = st->dec_round_floor_q10;
        }
    }

    st->cwnd_q10 = clamp_t(u64, st->cwnd_q10,
                           (u64)COPA_V3_MIN_CWND_PKTS << 10,
                           (u64)COPA_V3_MAX_CWND_PKTS << 10);
    return (u32)(st->cwnd_q10 >> 10);
}

#endif /* COPA_AUTO_V3_H */
