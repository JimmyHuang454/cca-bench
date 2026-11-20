#!/usr/bin/env python3
"""
Generate an LTE fast-fading trace for ns3::TraceFadingLossModel.

The LTE module only ships EPA 3 km/h, EVA 60 km/h and ETU 3 km/h traces, whose
Doppler spread is far below high-speed rail (300 km/h at 2.12 GHz gives a
maximum Doppler shift of about 590 Hz). This script is a numpy port of
src/lte/model/fading-traces/fading_trace_generator.m:

  * 3GPP TS 36.104 Annex B.2 tap delays/powers (EPA, EVA, ETU);
  * every tap is a Rayleigh process with a Clarke/Jakes Doppler spectrum,
    generated with a sum of sinusoids (Zheng & Xiao);
  * optional Rician direct path (--k-factor, in dB) for line-of-sight dominated
    viaduct / open-track channels;
  * the frequency response is sampled at the 100 RB centres every TTI (1 ms)
    and written in dB, one line per RB, as the ns-3 model expects.

Usage (from the ns-3 root):
  python3 scratch/gen-fading-trace.py --profile EVA --speed 300
  python3 scratch/gen-fading-trace.py --profile EVA --speed 300 --k-factor 6

The default output is scratch/fading-traces/fading_trace_<PROFILE>_<speed>kmph.fad
(with a _K<k>dB suffix for Rician traces). Use it with
  --fading=scratch/fading-traces/<file>.fad
"""

import argparse
import os

import numpy as np

C = 3e8

PROFILES = {
    "EPA": (
        [0, 30e-9, 70e-9, 90e-9, 120e-9, 190e-9, 410e-9],
        [0.0, -1.0, -2.0, -3.0, -8.0, -17.2, -20.8],
    ),
    "EVA": (
        [0, 30e-9, 150e-9, 310e-9, 370e-9, 710e-9, 1090e-9, 1730e-9, 2510e-9],
        [0.0, -1.5, -1.4, -3.6, -0.6, -9.1, -7.0, -12.0, -16.9],
    ),
    "ETU": (
        [0, 50e-9, 120e-9, 200e-9, 230e-9, 500e-9, 1600e-9, 2300e-9, 5000e-9],
        [-1.0, -1.0, -1.0, 0.0, 0.0, 0.0, -3.0, -5.0, -7.0],
    ),
}


def rayleigh_sos(t, fd, rng, oscillators=32):
    """Unit-power Rayleigh process with a Jakes spectrum (Zheng & Xiao SoS)."""
    m = oscillators
    n = np.arange(1, m + 1)
    theta = rng.uniform(-np.pi, np.pi)
    alpha = (2 * np.pi * n - np.pi + theta) / (4 * m)
    phi = rng.uniform(-np.pi, np.pi, m)
    psi = rng.uniform(-np.pi, np.pi, m)
    wd = 2 * np.pi * fd
    arg = np.outer(t, wd * np.cos(alpha))
    real = np.cos(arg + phi).sum(axis=1)
    arg = np.outer(t, wd * np.sin(alpha))
    imag = np.cos(arg + psi).sum(axis=1)
    return (real + 1j * imag) / np.sqrt(m)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--profile", choices=sorted(PROFILES), default="EVA")
    parser.add_argument("--speed", type=float, default=300.0, help="km/h")
    parser.add_argument("--fc", type=float, default=2.12e9, help="carrier [Hz]")
    parser.add_argument("--duration", type=float, default=10.0, help="trace length [s]")
    parser.add_argument("--rbs", type=int, default=100, help="number of RBs")
    parser.add_argument("--k-factor", type=float, default=None, help="Rician K [dB]")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    rng = np.random.default_rng(args.seed)
    fd = args.speed / 3.6 * args.fc / C
    tti = 1e-3
    t = np.arange(int(round(args.duration / tti))) * tti

    delays, powers_db = PROFILES[args.profile]
    powers = 10 ** (np.array(powers_db) / 10)
    powers /= powers.sum()

    freqs = (np.arange(args.rbs) - (args.rbs - 1) / 2) * 180e3
    response = np.zeros((args.rbs, t.size), dtype=complex)
    for tau, power in zip(delays, powers):
        h = rayleigh_sos(t, fd, rng)
        response += np.sqrt(power) * np.outer(np.exp(-2j * np.pi * freqs * tau), h)
    if args.k_factor is not None:
        # Rician: a direct path carrying K/(K+1) of the power on top of the
        # scattered (NLOS) profile, as in the 3GPP high-speed-train open space
        # and viaduct scenarios.
        k = 10 ** (args.k_factor / 10)
        los_angle = rng.uniform(-np.pi, np.pi)
        los = np.exp(1j * (2 * np.pi * fd * np.cos(los_angle) * t + rng.uniform(-np.pi, np.pi)))
        response = np.sqrt(k / (k + 1)) * los[np.newaxis, :] + np.sqrt(1 / (k + 1)) * response

    gain_db = 10 * np.log10(np.maximum(np.abs(response) ** 2, 1e-12))

    out = args.out
    if out is None:
        suffix = f"_K{args.k_factor:g}dB" if args.k_factor is not None else ""
        out = os.path.join(
            "scratch",
            "fading-traces",
            f"fading_trace_{args.profile}_{args.speed:g}kmph{suffix}.fad",
        )
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as stream:
        for row in gain_db:
            stream.write(" ".join(f"{v:.4g}" for v in row))
            stream.write(" \n")
    print(
        f"{out}: {args.profile}, {args.speed:g} km/h, fd={fd:.1f} Hz, "
        f"mean gain {10 * np.log10(np.mean(np.abs(response) ** 2)):.2f} dB, "
        f"std {gain_db.std():.2f} dB"
    )


if __name__ == "__main__":
    main()
