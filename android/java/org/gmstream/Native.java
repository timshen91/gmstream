package org.gmstream;

/** The native session (jni/session.cpp). Every call takes the handle from create. */
final class Native {
    static { System.loadLibrary("gmstream"); }

    /** Called from native threads. */
    interface Listener {
        /** The host's cursor image (RGBA, straight alpha); width 0: none. */
        void onCursor(int width, int height, int hotX, int hotY, byte[] rgba);
        /** The game wants relative motion (Mouse on Auto follows it). */
        void onRelative(boolean relative);
        void onRumble(int index, int strong, int weak, int ms);
        /** The decoded picture's size. */
        void onStreamSize(int width, int height);
        /** A one-line status (connecting, closed and why); empty: streaming. */
        void onStatus(String line);
    }

    static native long create(String filesDir, Listener listener);
    static native void destroy(long h);
    static native String authorizedLine(long h, String device);
    static native void forgetHosts(long h);
    static native void start(long h, String host, int port, int codec, int mbps, int width, int height,
                             double refreshHz, boolean pieces, int parallelism, int stripes,
                             boolean keepWiFiAwake, boolean keepGPUAwake, int pacing, boolean scanout, int displayRotation);
    static native void stop(long h);
    static native void setSurface(long h, android.view.Surface surface);
    static native String stats(long h);
    static native void key(long h, int evdev, boolean down);
    static native void button(long h, int evdev, boolean down);
    static native void motion(long h, double dx, double dy);
    static native void position(long h, float x, float y);
    static native void scroll(long h, float dxNotches, float dyNotches);
    static native void touch(long h, int id, int phase, float x, float y);
    static native void gamepad(long h, int index, int buttons, int lx, int ly, int rx, int ry, int lt, int rt);
    static native void gamepadRemove(long h, int index);
    static native void setBitrate(long h, int mbps);
    static native void requestKeyframe(long h);
}
