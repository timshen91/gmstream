#!/usr/bin/env python3
"""Reads a GM_DEBUG_TRACE file (Sources/Trace.swift) and reports network
stalls: stretches with no datagram at all (audio alone is one every 5 ms),
what arrived after them, audio losses and one-way delay around them, and the
frames on screen whose latency spiked.

    tools/trace.py trace.bin [--stall MS] [--spike MS]
"""
import argparse, struct, statistics as st
from collections import Counter

REC = struct.Struct('<QIBBHIHHQ')
AUDIO, VIDEO, CONTROL, PYRO, PRESENTED, CLOCK = 1, 2, 3, 4, 0xFE, 0xFF

ap = argparse.ArgumentParser()
ap.add_argument('file')
ap.add_argument('--stall', type=float, default=12, help='arrival gap counted as a stall (ms)')
ap.add_argument('--spike', type=float, default=None, help='total latency counted as a spike (ms; default p50 + 8)')
ap.add_argument('--events', type=int, default=40, help='stalls/spikes to list')
a = ap.parse_args()

data = open(a.file, 'rb').read()
recs = [REC.unpack_from(data, i) for i in range(0, len(data) - REC.size + 1, REC.size)]
if not recs: raise SystemExit('empty trace')
dgrams = sorted((r for r in recs if r[2] < 0xF0), key=lambda r: r[0])
pres = [r for r in recs if r[2] == PRESENTED]
clocks = [r for r in recs if r[2] == CLOCK]
t0 = dgrams[0][0]
ms = lambda ns: ns / 1e6
at = lambda t: ms(t - t0) / 1000  # seconds into the trace

# Clock: the offset from the fastest round trip (host = local + off).
off = None
if clocks:
    c = min(clocks, key=lambda r: r[1])
    off = struct.unpack('<q', struct.pack('<Q', c[8]))[0]
    rtts = sorted(ms(r[1]) for r in clocks)
    print(f'clock: {len(clocks)} pings, rtt min {rtts[0]:.2f} p50 {rtts[len(rtts)//2]:.2f} max {rtts[-1]:.2f} ms')

span = ms(dgrams[-1][0] - t0) / 1000
kinds = Counter(r[2] for r in dgrams)
print(f'trace: {span:.1f} s, {len(dgrams)} datagrams {dict(kinds)}, {len(pres)} frames presented')

# App-side read lag: the network thread late to read (not the network).
lags = sorted(ms(r[1]) for r in dgrams)
print(f'read lag: p50 {lags[len(lags)//2]:.3f} p99 {lags[int(len(lags)*.99)]:.3f} max {lags[-1]:.2f} ms')

# Audio: sequence holes (lost) and one-way delay (relative to its minimum).
audio = [r for r in dgrams if r[2] == AUDIO]
lost = []
owd = {}
if audio:
    seqs = sorted(set(r[5] for r in audio))
    for x, y in zip(seqs, seqs[1:]):
        if y - x > 1: lost.append((x + 1, y - x - 1))
    if off is not None:
        raw = [(r[0], r[0] - (r[8] - off)) for r in audio]
        base = min(d for _, d in raw)
        owd = {t: ms(d - base) for t, d in raw}
        v = sorted(owd.values())
        print(f'audio: {len(audio)} pkts, lost {sum(n for _, n in lost)} in {len(lost)} holes; '
              f'one-way delay above min: p50 {v[len(v)//2]:.2f} p90 {v[int(len(v)*.9)]:.2f} p99 {v[int(len(v)*.99)]:.2f} max {v[-1]:.2f} ms')
    else:
        print(f'audio: {len(audio)} pkts, lost {sum(n for _, n in lost)} in {len(lost)} holes')

# Video frames: incomplete (a packet never came).
frames = {}
for r in dgrams:
    if r[2] in (VIDEO, PYRO):
        f = frames.setdefault(r[5], [r[7], 0, r[0], r[0]])
        f[1] += 1; f[3] = r[0]
incomplete = sorted((fid, f) for fid, f in frames.items() if f[1] < f[0])
print(f'video: {len(frames)} frames, {len(incomplete)} missing packets')

# Stalls: gaps between consecutive datagrams of any kind.
stalls = []
for i in range(1, len(dgrams)):
    g = dgrams[i][0] - dgrams[i - 1][0]
    if ms(g) >= a.stall:
        # The burst: datagrams within 2 ms after the stall ended, one after another.
        j = i
        while j + 1 < len(dgrams) and dgrams[j + 1][0] - dgrams[j][0] < 300_000: j += 1
        burst = dgrams[i:j + 1]
        na = sum(1 for r in burst if r[2] == AUDIO)
        stalls.append((dgrams[i - 1][0], g, len(burst), na, ms(burst[-1][0] - burst[0][0])))
print(f'stalls (no datagram for >= {a.stall} ms): {len(stalls)}'
      + (f', {len(stalls)/span:.2f}/s' if span else ''))
if len(stalls) > 2:
    iv = [ms(b[0] - x[0]) for x, b in zip(stalls, stalls[1:])]
    print(f'  interval between stalls: min {min(iv):.0f} p50 {st.median(iv):.0f} max {max(iv):.0f} ms')
    durs = sorted(ms(s[1]) for s in stalls)
    print(f'  duration: p50 {durs[len(durs)//2]:.1f} max {durs[-1]:.1f} ms')

# Latency on screen.
tot = [(r[0], ms(r[0] - r[8])) for r in pres if r[8]]
spikes = []
if tot:
    v = sorted(x for _, x in tot)
    p50 = v[len(v) // 2]
    thr = a.spike if a.spike is not None else p50 + 8
    spikes = [(t, x) for t, x in tot if x >= thr]
    print(f'total latency: p50 {p50:.1f} p90 {v[int(len(v)*.9)]:.1f} p99 {v[int(len(v)*.99)]:.1f} max {v[-1]:.1f} ms; '
          f'{len(spikes)} frames >= {thr:.1f} ms')
    covered = sum(1 for t, _ in spikes if any(s[0] <= t <= s[0] + s[1] + 60_000_000 for s in stalls))
    if spikes: print(f'  spikes within 60 ms after a stall: {covered}/{len(spikes)}')

def audio_owd_near(t):
    xs = [d for ta, d in owd.items() if t - 50_000_000 <= ta <= t + 100_000_000]
    return max(xs) if xs else None

print(f'\nstalls (t s, gap ms, burst pkts/audio, burst span ms, audio owd max ±):')
for t, g, nb, na, bs in sorted(stalls, key=lambda s: -s[1])[:a.events]:
    o = audio_owd_near(t)
    print(f'  {at(t):9.3f}  gap {ms(g):6.1f}  burst {nb:4d}/{na:2d} in {bs:5.2f}' + (f'  owd {o:6.1f}' if o is not None else ''))
if lost:
    print('audio holes (t s, first seq, n):')
    seq_t = {r[5]: r[0] for r in audio}
    for s, n in lost[:a.events]:
        print(f'  {at(seq_t.get(s + n, t0)):9.3f}  {s}  {n}')
if incomplete:
    print('incomplete frames (t s, id, got/count):')
    for fid, f in incomplete[:a.events]:
        print(f'  {at(f[2]):9.3f}  {fid}  {f[1]}/{f[0]}')
