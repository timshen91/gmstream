"""Bjontegaard rate difference, for tools/bench.py."""

import math


def bd_rate(a, b, key="psnr_y"):
    """Bjontegaard rate difference (%) of b against a, piecewise cubic in log rate."""
    def prep(pts):
        pts = sorted((p[key], math.log10(p["kbps"])) for p in pts)
        return [x for x, _ in pts], [y for _, y in pts]

    def pchip_integral(xs, ys, lo, hi, n=2000):
        # Monotone piecewise cubic (Fritsch-Carlson), integrated numerically.
        k = len(xs)
        h = [xs[i + 1] - xs[i] for i in range(k - 1)]
        dlt = [(ys[i + 1] - ys[i]) / h[i] for i in range(k - 1)]
        m = [0.0] * k
        m[0], m[-1] = dlt[0], dlt[-1]
        for i in range(1, k - 1):
            if dlt[i - 1] * dlt[i] <= 0:
                m[i] = 0
            else:
                w1, w2 = 2 * h[i] + h[i - 1], h[i] + 2 * h[i - 1]
                m[i] = (w1 + w2) / (w1 / dlt[i - 1] + w2 / dlt[i])

        def f(x):
            i = 0
            while i < k - 2 and x > xs[i + 1]:
                i += 1
            t = (x - xs[i]) / h[i]
            h00, h10, h01, h11 = 2 * t**3 - 3 * t**2 + 1, t**3 - 2 * t**2 + t, -2 * t**3 + 3 * t**2, t**3 - t**2
            return h00 * ys[i] + h10 * h[i] * m[i] + h01 * ys[i + 1] + h11 * h[i] * m[i + 1]

        s = 0
        for j in range(n):
            s += f(lo + (hi - lo) * (j + 0.5) / n)
        return s * (hi - lo) / n

    xa, ya = prep(a)
    xb, yb = prep(b)
    lo, hi = max(xa[0], xb[0]), min(xa[-1], xb[-1])
    if hi <= lo:
        return float("nan")
    ia = pchip_integral(xa, ya, lo, hi)
    ib = pchip_integral(xb, yb, lo, hi)
    return (10 ** ((ib - ia) / (hi - lo)) - 1) * 100
