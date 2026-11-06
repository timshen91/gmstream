package org.gmstream;

import android.content.Context;
import android.content.SharedPreferences;

/** The client's settings (apple/Options.swift), saved in SharedPreferences. */
final class Prefs {
    String host = "";
    int port = 8444;
    int codec = 2;          // 0 H.264, 1 HEVC, 2 AV1, 4 Riptide
    int mbps = 40;
    String size = "";       // "" = the screen's, else WxH
    double refresh = 0;     // 0 = the screen's
    boolean showStats = true;
    boolean keepWiFiAwake = true;
    boolean keepGPUAwake = true;
    boolean streamFrames = true;  // AV1/HEVC in pieces
    int parallelism = 0;    // AV1 tiles / HEVC slice segments, 0 = the host's default
    int stripes = 0;        // Riptide stripes, 0 = the host's default
    int pacing = 0;         // 0 Auto (phase lock), 1 Low latency
    boolean scanout = false; // Riptide: frames written into the buffer on screen (experimental)
    boolean touchscreen = true; // fingers are a touchscreen on the host; off: a mouse

    static final String[] CODECS = {"H.264", "HEVC", "AV1", "Riptide"};
    static final int[] CODEC_IDS = {0, 1, 2, 4};

    static Prefs load(Context c) {
        SharedPreferences p = c.getSharedPreferences("gmstream", Context.MODE_PRIVATE);
        Prefs o = new Prefs();
        o.host = p.getString("host", o.host);
        o.port = p.getInt("port", o.port);
        o.codec = p.getInt("codec", o.codec);
        o.mbps = p.getInt("mbps", o.mbps);
        o.size = p.getString("size", o.size);
        o.refresh = Double.longBitsToDouble(p.getLong("refresh", Double.doubleToLongBits(o.refresh)));
        o.showStats = p.getBoolean("showStats", o.showStats);
        o.keepWiFiAwake = p.getBoolean("keepWiFiAwake", o.keepWiFiAwake);
        o.keepGPUAwake = p.getBoolean("keepGPUAwake", o.keepGPUAwake);
        o.streamFrames = p.getBoolean("streamFrames", o.streamFrames);
        o.parallelism = p.getInt("parallelism", o.parallelism);
        o.stripes = p.getInt("stripes", o.stripes);
        o.pacing = p.getInt("pacing", o.pacing);
        o.touchscreen = p.getBoolean("touchscreen", o.touchscreen);
        o.scanout = p.getBoolean("scanout", o.scanout);
        return o;
    }

    void save(Context c) {
        c.getSharedPreferences("gmstream", Context.MODE_PRIVATE).edit()
            .putString("host", host).putInt("port", port).putInt("codec", codec).putInt("mbps", mbps)
            .putString("size", size).putLong("refresh", Double.doubleToLongBits(refresh))
            .putBoolean("showStats", showStats).putBoolean("keepWiFiAwake", keepWiFiAwake)
            .putBoolean("keepGPUAwake", keepGPUAwake).putBoolean("streamFrames", streamFrames)
            .putInt("parallelism", parallelism).putInt("stripes", stripes).putInt("pacing", pacing)
            .putBoolean("touchscreen", touchscreen).putBoolean("scanout", scanout).apply();
    }

    /** What a reconnect is needed for (everything but the bitrate and the overlay). */
    String connectionKey() {
        return host + "|" + port + "|" + codec + "|" + size + "|" + refresh + "|" + keepWiFiAwake + "|" + keepGPUAwake
            + "|" + streamFrames + "|" + parallelism + "|" + stripes + "|" + pacing + "|" + scanout;
    }

    /** "WxH" or null. */
    int[] parsedSize() {
        String[] p = size.toLowerCase().split("x");
        if (p.length != 2) return null;
        try {
            int w = Integer.parseInt(p[0].trim()), h = Integer.parseInt(p[1].trim());
            return w >= 64 && h >= 64 ? new int[] {w, h} : null;
        } catch (NumberFormatException e) {
            return null;
        }
    }
}
