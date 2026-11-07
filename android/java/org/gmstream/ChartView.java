package org.gmstream;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Typeface;
import android.util.TypedValue;
import android.view.View;

/**
 * Latency per frame, last 10 s (apple/Latency.swift, LatencyChart): the host's
 * present -> handed to Android as a line, the display stage stacked above it
 * up to the total as a band. The scale grows in steps to fit spikes (never
 * below 20 ms); pauses in the stream (a still screen sends nothing) break the
 * line. Points come from Native.chart as (seconds ago, base ms, total ms).
 */
final class ChartView extends View {
    private static final float WINDOW = 10f, GAP = 0.25f;
    private static final float[] SCALES = {20, 40, 80, 160, 320, 640, 1280};
    private float[] pts = new float[0];
    private final Paint grid = new Paint(), label = new Paint(), band = new Paint(), line = new Paint();
    private final Path path = new Path();
    private final float density;

    ChartView(Context c) {
        super(c);
        density = c.getResources().getDisplayMetrics().density;
        grid.setColor(0x21ffffff);
        grid.setStrokeWidth(1);
        label.setColor(0xffc3c2b7);
        label.setTypeface(Typeface.MONOSPACE);
        label.setTextSize(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 9, c.getResources().getDisplayMetrics()));
        label.setAntiAlias(true);
        band.setColor(0x8ce8a33d);
        band.setStyle(Paint.Style.FILL);
        band.setAntiAlias(true);
        line.setColor(0xff3987e5);
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeWidth(1.5f * density);
        line.setStrokeJoin(Paint.Join.ROUND);
        line.setAntiAlias(true);
    }

    void set(float[] points) {
        pts = points;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas g) {
        float w = getWidth(), h = getHeight();
        int n = pts.length / 3;
        float peak = 0;
        for (int i = 0; i < n; i++) peak = Math.max(peak, pts[3 * i + 2]);
        float top = (float) Math.ceil(peak / 1000) * 1000;
        for (float s : SCALES) if (s >= peak) { top = s; break; }
        final float scale = top, labelH = label.getTextSize() + 2 * density;

        // Recessive grid: the top of the scale and its midpoint, labelled.
        for (float v : new float[] {scale, scale / 2}) {
            float y = y(v, scale, h, labelH);
            g.drawLine(0, y, w, y, grid);
        }
        g.drawText((int) scale + " ms", 2 * density, label.getTextSize(), label);
        g.drawText(String.valueOf((int) (scale / 2)), 2 * density, y(scale / 2, scale, h, labelH) - 2 * density, label);
        if (n > 0) {
            String s = String.format("max %.1f ms", peak);
            g.drawText(s, w - label.measureText(s) - 2 * density, label.getTextSize(), label);
        }

        // Unbroken runs: the display band (base to total) under the base line.
        int start = 0;
        for (int i = 1; i <= n; i++) {
            boolean end = i == n || pts[3 * (i - 1)] - pts[3 * i] > GAP; // ages fall towards now
            if (!end) continue;
            if (i - start > 1) {
                path.reset();
                path.moveTo(x(pts[3 * start], w), y(pts[3 * start + 1], scale, h, labelH));
                for (int k = start + 1; k < i; k++) path.lineTo(x(pts[3 * k], w), y(pts[3 * k + 1], scale, h, labelH));
                for (int k = i - 1; k >= start; k--) path.lineTo(x(pts[3 * k], w), y(pts[3 * k + 2], scale, h, labelH));
                path.close();
                g.drawPath(path, band);
                path.reset();
                path.moveTo(x(pts[3 * start], w), y(pts[3 * start + 1], scale, h, labelH));
                for (int k = start + 1; k < i; k++) path.lineTo(x(pts[3 * k], w), y(pts[3 * k + 1], scale, h, labelH));
                g.drawPath(path, line);
            }
            start = i;
        }
    }

    private static float x(float age, float w) {
        return (WINDOW - age) / WINDOW * w;
    }

    private static float y(float ms, float top, float h, float labelH) {
        return h - 1 - Math.min(ms, top) / top * (h - labelH);
    }
}
