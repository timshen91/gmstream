package org.gmstream;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Vibrator;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import android.window.OnBackInvokedDispatcher;

/**
 * The stream, full screen: the video on a SurfaceView (MediaCodec or Riptide's
 * Vulkan swapchain draws into it directly), the stats over it, and the
 * settings at its side (SettingsPanel: three-finger tap or the Back key), the
 * stream running on behind them.
 */
public final class MainActivity extends Activity implements Native.Listener, SurfaceHolder.Callback {
    private long h;
    private Prefs prefs;
    private Input input;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private FrameLayout root;
    private SurfaceView video;
    private TextView stats;
    private LinearLayout hud;
    private ChartView chart;
    private SettingsPanel settings;
    private WifiManager.WifiLock wifiLock;
    private String status = "";
    private int streamW = 16, streamH = 9;
    private boolean streaming;
    private int startedRotation;
    private boolean relative;
    private final int[] touchIds = new int[32]; // pointer id -> protocol touch id + 1 (0: none)

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = Prefs.load(this);
        applyExtras(getIntent());
        Window w = getWindow();
        w.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        w.getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        preferFastestMode(w);

        root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        video = new SurfaceView(this);
        video.getHolder().addCallback(this);
        root.addView(video, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));
        // The stats and the latency chart, top right (Settings -> Stats:
        // Full, Chart only, Off).
        float dp = getResources().getDisplayMetrics().density;
        hud = new LinearLayout(this);
        hud.setOrientation(LinearLayout.VERTICAL);
        hud.setPadding((int) (8 * dp), (int) (4 * dp), (int) (8 * dp), (int) (6 * dp));
        android.graphics.drawable.GradientDrawable box = new android.graphics.drawable.GradientDrawable();
        box.setColor(0x99000000);
        box.setCornerRadius(4 * dp);
        hud.setBackground(box);
        stats = new TextView(this);
        stats.setTypeface(Typeface.MONOSPACE);
        stats.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9);
        stats.setTextColor(Color.WHITE);
        // Its own width (a vertical LinearLayout would stretch it to the
        // chart's and wrap the lines).
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.gravity = Gravity.END;
        hud.addView(stats, slp);
        chart = new ChartView(this);
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams((int) (240 * dp), (int) (56 * dp));
        clp.topMargin = (int) (4 * dp);
        clp.gravity = Gravity.END;
        hud.addView(chart, clp);
        FrameLayout.LayoutParams hlp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.END);
        hlp.topMargin = hlp.rightMargin = (int) (6 * dp);
        root.addView(hud, hlp);
        setContentView(root);
        hideSystemBars();

        h = Native.create(getFilesDir().getAbsolutePath(), this);
        input = new Input(h, (Vibrator) getSystemService(Context.VIBRATOR_SERVICE));
        root.setOnTouchListener((v, e) -> touch(e));
        root.setOnGenericMotionListener((v, e) -> generic(e));
        root.setOnCapturedPointerListener((v, e) -> captured(e));
        root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> layoutVideo());
        android.hardware.input.InputManager im = getSystemService(android.hardware.input.InputManager.class);
        im.registerInputDeviceListener(new android.hardware.input.InputManager.InputDeviceListener() {
            @Override public void onInputDeviceAdded(int id) {}
            @Override public void onInputDeviceRemoved(int id) { input.removed(id); }
            @Override public void onInputDeviceChanged(int id) {}
        }, ui);
        root.setFocusable(true);
        root.setFocusableInTouchMode(true);
        root.requestFocus();
        getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT,
            this::toggleSettings);
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "gmstream");
        wifiLock.setReferenceCounted(false);
        if (prefs.host.isEmpty()) showSettings();
    }

    /**
     * Settings from the launch intent, saved (for scripts:
     * am start -n org.gmstream/.MainActivity --es host H --ei port P --ei codec C --ei mbps M
     * --ei pacing 0|1 --ez stats true|false --es size WxH).
     */
    private void applyExtras(android.content.Intent i) {
        if (i == null || i.getExtras() == null) return;
        Bundle x = i.getExtras();
        // Testing from adb with the device locked: this window (only) over the
        // lock screen, and the screen on. Not saved.
        if (x.getBoolean("overLock", false)) {
            setShowWhenLocked(true);
            setTurnScreenOn(true);
        }
        if (x.containsKey("host")) prefs.host = x.getString("host", prefs.host);
        prefs.port = x.getInt("port", prefs.port);
        prefs.codec = x.getInt("codec", prefs.codec);
        prefs.mbps = x.getInt("mbps", prefs.mbps);
        prefs.pacing = x.getInt("pacing", prefs.pacing);
        prefs.parallelism = x.getInt("parallelism", prefs.parallelism);
        prefs.stripes = x.getInt("stripes", prefs.stripes);
        if (x.containsKey("stats")) prefs.statsMode = x.getBoolean("stats", true) ? 2 : 0;
        prefs.statsMode = x.getInt("statsMode", prefs.statsMode);
        prefs.scanout = x.getBoolean("scanout", prefs.scanout);
        prefs.streamFrames = x.getBoolean("pieces", prefs.streamFrames);
        if (x.containsKey("size")) prefs.size = x.getString("size", prefs.size);
        prefs.save(this);
    }

    @Override
    protected void onNewIntent(android.content.Intent i) {
        super.onNewIntent(i);
        applyExtras(i);
        if (streaming) applyToStream();
    }

    /** The refresh of the mode asked for: the display may not have switched to it yet when a stream starts. */
    private float fastestHz;

    /** The display's mode with the highest refresh at its full size. */
    private void preferFastestMode(Window w) {
        Display d = getDisplay();
        Display.Mode best = d.getMode();
        for (Display.Mode m : d.getSupportedModes()) {
            if (m.getPhysicalWidth() * m.getPhysicalHeight() < best.getPhysicalWidth() * best.getPhysicalHeight()) continue;
            if (m.getRefreshRate() > best.getRefreshRate() + 0.5f
                || m.getPhysicalWidth() * m.getPhysicalHeight() > best.getPhysicalWidth() * best.getPhysicalHeight()) best = m;
        }
        fastestHz = best.getRefreshRate();
        WindowManager.LayoutParams a = w.getAttributes();
        a.preferredDisplayModeId = best.getModeId();
        w.setAttributes(a);
    }

    private void hideSystemBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) hideSystemBars();
    }

    @Override
    protected void onResume() {
        super.onResume();
        startStream();
        ui.post(statsTick);
    }

    @Override
    protected void onPause() {
        super.onPause();
        stopStream();
        ui.removeCallbacks(statsTick);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Native.destroy(h);
        h = 0;
    }

    /** The size asked of the host: the setting, or this screen's (landscape). */
    private int[] streamSize() {
        int[] size = prefs.parsedSize();
        if (size != null) return size;
        Rect b = getWindowManager().getCurrentWindowMetrics().getBounds();
        return new int[] {Math.max(b.width(), b.height()), Math.min(b.width(), b.height())};
    }

    private double streamHz() {
        return prefs.refresh > 0 ? prefs.refresh : fastestHz > 0 ? fastestHz : getDisplay().getRefreshRate();
    }

    private void startStream() {
        if (prefs.host.isEmpty()) return;
        int[] size = streamSize();
        Native.start(h, prefs.host.trim(), prefs.port, prefs.codec, prefs.mbps, size[0], size[1], streamHz(),
            prefs.streamFrames, prefs.parallelism, prefs.stripes, prefs.keepWiFiAwake, prefs.keepGPUAwake, prefs.pacing,
            prefs.scanout, getDisplay().getRotation());
        startedRotation = getDisplay().getRotation();
        if (prefs.keepWiFiAwake) wifiLock.acquire();
        streaming = true;
        input.resend();
    }

    /** The settings as they are now, to the running stream: applied in place. */
    private void applyToStream() {
        int[] size = streamSize();
        Native.update(h, prefs.host.trim(), prefs.port, prefs.codec, prefs.mbps, size[0], size[1], streamHz(),
            prefs.streamFrames, prefs.parallelism, prefs.stripes, prefs.keepWiFiAwake, prefs.keepGPUAwake, prefs.pacing,
            prefs.scanout, getDisplay().getRotation());
        startedRotation = getDisplay().getRotation();
        if (prefs.keepWiFiAwake) wifiLock.acquire(); else if (wifiLock.isHeld()) wifiLock.release();
    }

    private void stopStream() {
        if (!streaming) return;
        streaming = false;
        Native.stop(h);
        if (wifiLock.isHeld()) wifiLock.release();
    }

    private long lastTextAt;

    // The chart ten times a second; the text once a second (its counts: fps
    // shown, drops, tears, composes are since the last poll).
    private final Runnable statsTick = new Runnable() {
        @Override public void run() {
            if (h != 0) {
                long now = android.os.SystemClock.uptimeMillis();
                boolean full = prefs.statsMode == 2, showChart = prefs.statsMode >= 1 && streaming;
                if (now - lastTextAt >= 1000) {
                    lastTextAt = now;
                    String text = streaming && full ? Native.stats(h) : "";
                    if (!status.isEmpty()) text = status + (text.isEmpty() ? "" : "\n" + text);
                    if (!streaming && prefs.host.isEmpty()) text = "";
                    stats.setText(text);
                    stats.setVisibility(text.isEmpty() ? View.GONE : View.VISIBLE);
                }
                chart.setVisibility(showChart ? View.VISIBLE : View.GONE);
                if (showChart) chart.set(Native.chart(h));
                boolean any = stats.getVisibility() == View.VISIBLE || showChart;
                hud.setVisibility(any ? View.VISIBLE : View.GONE);
            }
            ui.postDelayed(this, 100);
        }
    };

    // ---- Surface -------------------------------------------------------------------

    @Override public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Surface s = holder.getSurface();
        s.setFrameRate(fastestHz > 0 ? fastestHz : getDisplay().getRefreshRate(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
            Surface.CHANGE_FRAME_RATE_ALWAYS);
        Native.setSurface(h, s);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Native.setSurface(h, null);
    }

    /** Letterboxed to the stream's shape. */
    private void layoutVideo() {
        int rw = root.getWidth(), rh = root.getHeight();
        if (rw == 0 || rh == 0) return;
        double s = Math.min((double) rw / streamW, (double) rh / streamH);
        int w = (int) Math.round(streamW * s), hh = (int) Math.round(streamH * s);
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) video.getLayoutParams();
        if (lp.width == w && lp.height == hh) return;
        lp.width = w;
        lp.height = hh;
        video.setLayoutParams(lp);
    }

    // ---- Native.Listener (native threads) -----------------------------------------

    @Override
    public void onCursor(int width, int height, int hotX, int hotY, byte[] rgba) {
        Bitmap bmp = null;
        if (width > 0 && height > 0 && rgba.length >= width * height * 4) {
            int[] px = new int[width * height];
            for (int i = 0; i < px.length; i++)
                px[i] = (rgba[4 * i + 3] & 0xff) << 24 | (rgba[4 * i] & 0xff) << 16 | (rgba[4 * i + 1] & 0xff) << 8 | (rgba[4 * i + 2] & 0xff);
            bmp = Bitmap.createBitmap(px, width, height, Bitmap.Config.ARGB_8888);
        }
        final Bitmap b = bmp;
        ui.post(() -> {
            // The host's cursor as the system pointer: drawn by the display, no lag.
            PointerIcon icon = b != null ? PointerIcon.create(b, Math.max(0, Math.min(hotX, b.getWidth() - 1)),
                Math.max(0, Math.min(hotY, b.getHeight() - 1))) : PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL);
            root.setPointerIcon(icon);
        });
    }

    @Override
    public void onRelative(boolean on) {
        ui.post(() -> {
            relative = on;
            if (on && hasMouse()) root.requestPointerCapture(); else root.releasePointerCapture();
        });
    }

    @Override
    public void onRumble(int index, int strong, int weak, int ms) {
        ui.post(() -> input.rumble(index, strong, weak, ms));
    }

    @Override
    public void onStreamSize(int width, int height) {
        ui.post(() -> {
            streamW = width;
            streamH = height;
            layoutVideo();
        });
    }

    @Override
    public void onStatus(String line) {
        ui.post(() -> status = line);
    }

    // ---- Input ------------------------------------------------------------------------

    private static boolean hasMouse() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null && !d.isVirtual() && (d.getSources() & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE)
                return true;
        }
        return false;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int k = e.getKeyCode();
        if (k == KeyEvent.KEYCODE_VOLUME_UP || k == KeyEvent.KEYCODE_VOLUME_DOWN || k == KeyEvent.KEYCODE_VOLUME_MUTE)
            return super.dispatchKeyEvent(e);
        if (settings != null) return settings.key(e) || super.dispatchKeyEvent(e);
        if (input.padKey(e)) return true;
        if (k == KeyEvent.KEYCODE_BACK) {
            if (e.getAction() == KeyEvent.ACTION_UP) toggleSettings();
            return true;
        }
        if (k == KeyEvent.KEYCODE_VOLUME_UP || k == KeyEvent.KEYCODE_VOLUME_DOWN || k == KeyEvent.KEYCODE_VOLUME_MUTE)
            return super.dispatchKeyEvent(e);
        int code = Input.evdev(e);
        if (code == 0) return super.dispatchKeyEvent(e);
        if (e.getRepeatCount() == 0) Native.key(h, code, e.getAction() == KeyEvent.ACTION_DOWN);
        return true; // gamescope repeats keys itself
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (settings != null) return settings.motion(e) || super.dispatchGenericMotionEvent(e);
        if (input.padMotion(e)) return true;
        return super.dispatchGenericMotionEvent(e);
    }

    /** Position in the video, 0..1 from its top left. */
    private float[] inVideo(float x, float y) {
        return new float[] {(x - video.getLeft()) / Math.max(1, video.getWidth()), (y - video.getTop()) / Math.max(1, video.getHeight())};
    }

    private static int evdevButton(int android) {
        switch (android) {
            case MotionEvent.BUTTON_SECONDARY: return 0x111;
            case MotionEvent.BUTTON_TERTIARY: return 0x112;
            case MotionEvent.BUTTON_BACK: return 0x113;
            case MotionEvent.BUTTON_FORWARD: return 0x114;
            default: return 0x110;
        }
    }

    private boolean generic(MotionEvent e) {
        if (settings != null) return false;
        if (!e.isFromSource(InputDevice.SOURCE_MOUSE)) return false;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_HOVER_MOVE: {
                float[] p = inVideo(e.getX(), e.getY());
                Native.position(h, p[0], p[1]);
                return true;
            }
            case MotionEvent.ACTION_SCROLL:
                Native.scroll(h, e.getAxisValue(MotionEvent.AXIS_HSCROLL), -e.getAxisValue(MotionEvent.AXIS_VSCROLL));
                return true;
            case MotionEvent.ACTION_BUTTON_PRESS:
            case MotionEvent.ACTION_BUTTON_RELEASE: {
                float[] p = inVideo(e.getX(), e.getY());
                Native.position(h, p[0], p[1]);
                Native.button(h, evdevButton(e.getActionButton()), e.getActionMasked() == MotionEvent.ACTION_BUTTON_PRESS);
                return true;
            }
            default:
                return false;
        }
    }

    /** A captured mouse (the game wants motion): deltas and buttons. */
    private boolean captured(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_MOVE:
            case MotionEvent.ACTION_HOVER_MOVE: {
                float dx = 0, dy = 0;
                for (int i = 0; i < e.getHistorySize(); i++) {
                    dx += e.getHistoricalX(i);
                    dy += e.getHistoricalY(i);
                }
                Native.motion(h, dx + e.getX(), dy + e.getY());
                return true;
            }
            case MotionEvent.ACTION_BUTTON_PRESS:
            case MotionEvent.ACTION_BUTTON_RELEASE:
                Native.button(h, evdevButton(e.getActionButton()), e.getActionMasked() == MotionEvent.ACTION_BUTTON_PRESS);
                return true;
            case MotionEvent.ACTION_SCROLL:
                Native.scroll(h, e.getAxisValue(MotionEvent.AXIS_HSCROLL), -e.getAxisValue(MotionEvent.AXIS_VSCROLL));
                return true;
            default:
                return false;
        }
    }

    private long threeDownAt;
    private int maxPointers;

    private boolean touch(MotionEvent e) {
        if (settings != null) {
            // A tap on the stream, beside the panel, closes it.
            if (e.getActionMasked() == MotionEvent.ACTION_UP) closeSettings();
            return true;
        }
        if (e.isFromSource(InputDevice.SOURCE_MOUSE)) {
            // A mouse with a button held arrives as touches.
            if (e.getActionMasked() == MotionEvent.ACTION_MOVE) {
                float[] p = inVideo(e.getX(), e.getY());
                Native.position(h, p[0], p[1]);
            }
            return true;
        }
        int action = e.getActionMasked();
        // Three fingers at once, lifted soon: the settings (the touches are cancelled).
        if (action == MotionEvent.ACTION_DOWN) maxPointers = 1;
        if (action == MotionEvent.ACTION_POINTER_DOWN) {
            maxPointers = Math.max(maxPointers, e.getPointerCount());
            if (e.getPointerCount() == 3) threeDownAt = e.getEventTime();
        }
        if (action == MotionEvent.ACTION_UP && maxPointers == 3 && e.getEventTime() - threeDownAt < 400) {
            if (prefs.touchscreen) Native.touch(h, 0, 3, 0, 0);
            java.util.Arrays.fill(touchIds, 0);
            toggleSettings();
            return true;
        }
        if (!prefs.touchscreen) {
            // A finger is the mouse with its left button held.
            float[] p = inVideo(e.getX(0), e.getY(0));
            if (action == MotionEvent.ACTION_DOWN) {
                Native.position(h, p[0], p[1]);
                Native.button(h, 0x110, true);
            } else if (action == MotionEvent.ACTION_MOVE) {
                Native.position(h, p[0], p[1]);
            } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                Native.button(h, 0x110, false);
            }
            return true;
        }
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                int pid = e.getPointerId(i) & 31;
                int id = freeTouchId();
                touchIds[pid] = id + 1;
                float[] p = inVideo(e.getX(i), e.getY(i));
                Native.touch(h, id, 0, p[0], p[1]);
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int pid = e.getPointerId(i) & 31;
                    if (touchIds[pid] == 0) continue;
                    float[] p = inVideo(e.getX(i), e.getY(i));
                    Native.touch(h, touchIds[pid] - 1, 1, p[0], p[1]);
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = e.getActionIndex();
                int pid = e.getPointerId(i) & 31;
                if (touchIds[pid] != 0) {
                    float[] p = inVideo(e.getX(i), e.getY(i));
                    Native.touch(h, touchIds[pid] - 1, 2, p[0], p[1]);
                    touchIds[pid] = 0;
                }
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                Native.touch(h, 0, 3, 0, 0);
                java.util.Arrays.fill(touchIds, 0);
                break;
            default:
                break;
        }
        return true;
    }

    private int freeTouchId() {
        boolean[] used = new boolean[33];
        for (int t : touchIds) if (t > 0) used[t - 1] = true;
        for (int i = 0; i < 32; i++) if (!used[i]) return i;
        return 0;
    }

    @Override
    public void onConfigurationChanged(android.content.res.Configuration c) {
        super.onConfigurationChanged(c);
        root.post(this::layoutVideo);
        // Turned the other way up: the output is drawn for the panel's orientation.
        if (streaming && getDisplay().getRotation() != startedRotation) applyToStream();
    }

    // ---- Settings -------------------------------------------------------------------------

    private void toggleSettings() {
        if (settings != null) closeSettings(); else showSettings();
    }

    private final SettingsPanel.Owner panelOwner = new SettingsPanel.Owner() {
        @Override public void applySettings() {
            prefs.save(MainActivity.this);
            if (streaming) applyToStream();
            else startStream();
        }
        @Override public void editHost() { askHost(); }
        @Override public void copyKey() {
            String line = keyLine();
            ((ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE))
                .setPrimaryClip(ClipData.newPlainText("gmstream key", line));
            Toast.makeText(MainActivity.this, "Key line copied: add it to ~/.ssh/authorized_keys on the host",
                Toast.LENGTH_LONG).show();
        }
        @Override public void forgetHosts() {
            Native.forgetHosts(h);
            Toast.makeText(MainActivity.this, "Host keys forgotten", Toast.LENGTH_SHORT).show();
        }
        @Override public void closeSettings() { MainActivity.this.closeSettings(); }
    };

    private String keyLine() {
        return Native.authorizedLine(h, Build.MODEL.replaceAll("[^A-Za-z0-9_-]", ""));
    }

    private void showSettings() {
        if (settings != null) return;
        int[] screen = streamSize();
        String here = prefs.parsedSize() == null ? screen[0] + "x" + screen[1] : "";
        if (here.isEmpty()) {
            Rect b = getWindowManager().getCurrentWindowMetrics().getBounds();
            here = Math.max(b.width(), b.height()) + "x" + Math.min(b.width(), b.height());
        }
        settings = new SettingsPanel(this, prefs, panelOwner, here, "This device's key: " + keyLine());
        root.addView(settings.view, SettingsPanel.layout(root.getWidth()));
        input.releaseAll();
        if (prefs.host.isEmpty()) askHost();
    }

    private void closeSettings() {
        if (settings == null) return;
        root.removeView(settings.view);
        settings = null;
        hideSystemBars();
        root.requestFocus();
        if (!streaming) startStream();
    }

    /** Host and port: the one setting that is typed, and the one that reconnects. */
    private void askHost() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(48, 16, 48, 0);
        EditText host = new EditText(this);
        host.setHint("Host name or address");
        host.setText(prefs.host);
        host.setSingleLine(true);
        host.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        l.addView(host);
        EditText port = new EditText(this);
        port.setHint("Port (plain UDP)");
        port.setText(String.valueOf(prefs.port));
        port.setSingleLine(true);
        port.setInputType(InputType.TYPE_CLASS_NUMBER);
        l.addView(port);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Host")
            .setView(l)
            .setPositiveButton("Connect", (d, w) -> {
                String newHost = host.getText().toString().trim();
                int newPort;
                try { newPort = Integer.parseInt(port.getText().toString().trim()); } catch (NumberFormatException x) { newPort = 8444; }
                boolean changed = !newHost.equals(prefs.host) || newPort != prefs.port;
                prefs.host = newHost;
                prefs.port = newPort;
                prefs.save(this);
                if (settings != null) settings.refresh();
                if (changed && streaming) stopStream();
                if (!streaming) startStream();
            })
            .setNegativeButton("Cancel", null)
            .show();
    }
}
