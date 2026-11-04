#!/usr/bin/env python3
"""The inter-frame work's benchmark: quality per bit and GPU time, per change.

  bench.py quality TAG [--bin B] [-- PWRD_ARGS...]   rd/TAG.json: five clips, 60 consecutive frames,
                                                     frames 0-9 left out, 20-300 KB per frame
  bench.py bd A B                                    BD-rate of B against A: PSNR-Y, YUV-PSNR 6:1:1,
                                                     PSNR-HVS-M (2 screen heights)
  bench.py speed BIN[:ARGS] BIN[:ARGS] ... [--rounds N]
                                                     encode and decode times, the builds alternated
                                                     round by round (the GPU clock drifts), medians
  bench.py loss BIN[:ARGS] ...                       a 10% packet burst lost on frame 20 (1400-byte
                                                     packets, at three places in the frame): PSNR-Y
                                                     lost against the run without loss, 30 frames
                                                     (RT_REPORT=d: the client's loss report reaches
                                                     the encoder d frames later, which then replays
                                                     the client's decode)

A build to keep is copied to build/rtrd.TAG first, so later rebuilds do not
change what an old tag measured.
"""
import json
import os
import re
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLIPS = os.environ.get("RT_CLIPS", "clips") # the AOM CTC gaming clips (media.xiph.org/video/aomctc/test_set/b1_syn)
NAMES = ["DOTA2_1920x1080_60_8bit_420", "EuroTruckSimulator2_1920x1080p60_v2", "MINECRAFT_1080p_60_8bit",
         "STARCRAFT_1080p60", "WITCHER3_1920x1080_60_8bit_420"]
SIZES = "20000,40000,75000,150000,300000"
FRAMES, WARM = 60, 10
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bdrate import bd_rate  # noqa: E402


def quality(tag, binary, extra):
    out = {"args": extra, "frames": FRAMES, "warm": WARM}
    for n in NAMES:
        r = subprocess.run([binary, os.path.join(CLIPS, n + ".y4m"), "-n", str(FRAMES), "-warm", str(WARM),
                            "-s", SIZES] + extra, capture_output=True, text=True, check=True)
        pts = []
        for line in r.stdout.split("\n"):
            t = line.split()
            if len(t) == 7:
                pts.append({"bytes": float(t[1]), "kbps": float(t[1]) * 8 * 60 / 1000, "psnr_y": float(t[2]),
                            "psnr_u": float(t[3]), "psnr_v": float(t[4]), "psnr_w": float(t[5]), "hvs": float(t[6])})
        out[n] = pts
        print(n.split("_")[0][:10], " ".join(f"{p['bytes'] / 1000:.0f}K:{p['psnr_y']:.2f}" for p in pts), flush=True)
    os.makedirs(os.path.join(ROOT, "rd"), exist_ok=True)
    json.dump(out, open(os.path.join(ROOT, "rd", tag + ".json"), "w"), indent=1)


def bd(a, b):
    A = json.load(open(os.path.join(ROOT, "rd", a + ".json")))
    B = json.load(open(os.path.join(ROOT, "rd", b + ".json")))
    print(f"{b} against {a} (negative: fewer bits for the same quality)")
    for key, name in (("psnr_y", "PSNR-Y"), ("psnr_w", "YUV"), ("hvs", "HVS-M")):
        vals = [bd_rate(A[n], B[n], key) for n in NAMES]
        print(f"  {name:7s}", " ".join(f"{n.split('_')[0][:8]:>8s} {v:+6.1f}%" for n, v in zip(NAMES, vals)),
              f"  mean {sum(vals) / len(vals):+6.1f}%")


def speed(specs, rounds):
    clip = os.path.join(CLIPS, NAMES[4] + ".y4m")
    res = {s: {} for s in specs}
    for r in range(rounds):
        for s in (specs if r % 2 == 0 else specs[::-1]):
            b, _, args = s.partition(":")
            p = subprocess.run([b, clip, "-n", "12", "-s", "75000", "-bench", "200"] + args.split(), env=dict(os.environ, RT_NOHVS="1"),
                               capture_output=True, text=True, check=True)
            m = re.search(r"encode ([\d.]+) ms per frame.*decode GPU best ([\d.]+) ms", p.stderr)
            vals = {"encode wall": float(m.group(1)), "decode GPU": float(m.group(2))}
            for k, v in re.findall(r"^(.+?): ([\d.]+) ms per iteration", p.stderr, re.M):
                vals[k] = float(v)
            for k, v in vals.items():
                res[s].setdefault(k, []).append(v)
    keys = list(res[specs[0]])
    print(f"ms per 1080p frame at 75 KB, medians of {rounds} alternated runs of 200")
    print(f"  {'':28s}" + "".join(f"{os.path.basename(s)[:22]:>24s}" for s in specs))
    for k in keys:
        print(f"  {k:28s}" + "".join(f"{statistics.median(res[s].get(k, [0])):24.3f}" for s in specs))


def loss(specs):
    F, starts = 20, (0.05, 0.35, 0.65)
    print(f"10% of frame {F}'s packets lost; mean of bursts at {', '.join(map(str, starts))} of the frame.")
    print("  PSNR-Y the client loses against the same run without loss: on that frame (hit), summed over it")
    print("  and the 29 after (dB*fr), and on frames +1, +10 and +29")
    for s in specs:
        b, _, args = s.partition(":")
        print(f"{os.path.basename(b)} {args}")
        for n in NAMES:
            def run(lossspec):
                p = subprocess.run([b, os.path.join(CLIPS, n + ".y4m"), "-n", str(F + 30), "-s", "20000,75000"] + args.split(),
                                   env=dict(os.environ, RT_NOHVS="1", RT_LOSS=lossspec), capture_output=True, text=True, check=True)
                rows = {}
                for line in p.stderr.split("\n"):
                    t = line.split()
                    if len(t) == 6 and t[0] == "loss":
                        rows.setdefault(t[1], {})[int(t[2])] = float(t[4])  # the client's PSNR-Y
                return rows
            base = run(f"{F}:0:0")
            acc = {}
            for st in starts:
                for size, r in run(f"{F}:{st}:0.1:{os.environ.get('RT_REPORT', 0)}").items():
                    d = {f: base[size][f] - r[f] for f in r}
                    acc.setdefault(size, []).append([d[F], sum(d[f] for f in d if f >= F), d[F + 1], d[F + 10], d[F + 29]])
            line = []
            for size, vs in acc.items():
                m = [sum(x[i] for x in vs) / len(vs) for i in range(5)]
                line.append(f"{int(size) // 1000:>3}K hit {m[0]:5.2f} dB*fr {m[1]:6.2f} +1 {m[2]:5.2f} +10 {m[3]:5.2f} +29 {m[4]:5.2f}")
            print(f"  {n.split('_')[0][:10]:10s}", "   ".join(line), flush=True)


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "quality":
        extra = a[a.index("--") + 1:] if "--" in a else []
        head = a[:a.index("--")] if "--" in a else a
        quality(a[1], head[head.index("--bin") + 1] if "--bin" in head else os.path.join(ROOT, "build", "rtrd"), extra)
    elif a[0] == "bd":
        bd(a[1], a[2])
    elif a[0] == "speed":
        rounds = int(a[a.index("--rounds") + 1]) if "--rounds" in a else 5
        speed([x for x in a[1:] if not x.startswith("--") and not x.isdigit()], rounds)
    elif a[0] == "loss":
        loss(a[1:])
