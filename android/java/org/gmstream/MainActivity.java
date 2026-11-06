package org.gmstream;

import android.app.Activity;
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
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.window.OnBackInvokedDispatcher;

/**
 * The stream, full screen: the video on a SurfaceView (MediaCodec or Riptide's
 * Vulkan swapchain draws into it directly), the stats over it, and a settings
 * panel (three-finger tap or the Back key, as on the iPad).
 */
public final class MainActivity extends Activity implements Native.Listener, SurfaceHolder.Callback {
    private long h;
    private Prefs prefs;
    private Input input;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private FrameLayout root;
    private SurfaceView video;
    private TextView stats;
    private ScrollView settings;
    private WifiManager.WifiLock wifiLock;
    private String status = "";
    private int streamW = 16, streamH = 9;
    private boolean streaming;
    private String startedWith = "";
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
        stats = new TextView(this);
        stats.setTypeface(Typeface.MONOSPACE);
        stats.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9);
        stats.setTextColor(Color.WHITE);
        stats.setBackgroundColor(0x99000000);
        stats.setPadding(12, 8, 12, 8);
        root.addView(stats, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START));
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
        prefs.showStats = x.getBoolean("stats", prefs.showStats);
        prefs.streamFrames = x.getBoolean("pieces", prefs.streamFrames);
        if (x.containsKey("size")) prefs.size = x.getString("size", prefs.size);
        prefs.save(this);
    }

    @Override
    protected void onNewIntent(android.content.Intent i) {
        super.onNewIntent(i);
        applyExtras(i);
        if (streaming) {
            stopStream();
            startStream();
        }
    }

    /** The display's mode with the highest refresh at its full size. */
    private void preferFastestMode(Window w) {
        Display d = getDisplay();
        Display.Mode best = d.getMode();
        for (Display.Mode m : d.getSupportedModes()) {
            if (m.getPhysicalWidth() * m.getPhysicalHeight() < best.getPhysicalWidth() * best.getPhysicalHeight()) continue;
            if (m.getRefreshRate() > best.getRefreshRate() + 0.5f
                || m.getPhysicalWidth() * m.getPhysicalHeight() > best.getPhysicalWidth() * best.getPhysicalHeight()) best = m;
        }
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

    private void startStream() {
        if (prefs.host.isEmpty() || settings != null) return;
        int[] size = prefs.parsedSize();
        if (size == null) {
            Rect b = getWindowManager().getCurrentWindowMetrics().getBounds();
            size = new int[] {Math.max(b.width(), b.height()), Math.min(b.width(), b.height())};
        }
        double hz = prefs.refresh > 0 ? prefs.refresh : getDisplay().getRefreshRate();
        Native.start(h, prefs.host.trim(), prefs.port, prefs.codec, prefs.mbps, size[0], size[1], hz,
            prefs.streamFrames, prefs.parallelism, prefs.stripes, prefs.keepWiFiAwake, prefs.keepGPUAwake, prefs.pacing);
        if (prefs.keepWiFiAwake) wifiLock.acquire();
        streaming = true;
        startedWith = prefs.connectionKey();
        input.resend();
    }

    private void stopStream() {
        if (!streaming) return;
        streaming = false;
        Native.stop(h);
        if (wifiLock.isHeld()) wifiLock.release();
    }

    private final Runnable statsTick = new Runnable() {
        @Override public void run() {
            if (h != 0) {
                String text = streaming ? Native.stats(h) : "";
                if (!status.isEmpty()) text = status + (text.isEmpty() ? "" : "\n" + text);
                if (!streaming && prefs.host.isEmpty()) text = "";
                stats.setText(text);
                stats.setVisibility((prefs.showStats || !status.isEmpty()) && !text.isEmpty() ? View.VISIBLE : View.GONE);
            }
            ui.postDelayed(this, 500);
        }
    };

    // ---- Surface -------------------------------------------------------------------

    @Override public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Surface s = holder.getSurface();
        s.setFrameRate(getDisplay().getRefreshRate(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
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
        if (settings != null) return super.dispatchKeyEvent(e);
        if (input.padKey(e)) return true;
        int k = e.getKeyCode();
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
        if (settings == null && input.padMotion(e)) return true;
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
        if (settings != null) return false;
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
    }

    // ---- Settings -------------------------------------------------------------------------

    private void toggleSettings() {
        if (settings != null) closeSettings(); else showSettings();
    }

    private TextView label(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(0xffcccccc);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        t.setPadding(0, 18, 0, 2);
        return t;
    }

    private EditText field(String value, boolean number) {
        EditText e = new EditText(this);
        e.setText(value);
        e.setTextColor(Color.WHITE);
        e.setSingleLine(true);
        if (number) e.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        return e;
    }

    private CheckBox check(String text, boolean on) {
        CheckBox c = new CheckBox(this);
        c.setText(text);
        c.setTextColor(Color.WHITE);
        c.setChecked(on);
        return c;
    }

    private Spinner spinner(String[] items, int selected) {
        Spinner s = new Spinner(this);
        ArrayAdapter<String> a = new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, items);
        s.setAdapter(a);
        s.setSelection(Math.max(0, selected));
        return s;
    }

    private static int intOf(EditText e, int fallback) {
        try { return Integer.parseInt(e.getText().toString().trim()); } catch (NumberFormatException x) { return fallback; }
    }

    private void showSettings() {
        if (settings != null) return;
        settings = new ScrollView(this);
        settings.setBackgroundColor(0xee101014);
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(48, 32, 48, 48);
        settings.addView(l);

        TextView title = new TextView(this);
        title.setText("gmstream");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        l.addView(title);

        l.addView(label("Host (name or address)"));
        EditText host = field(prefs.host, false);
        host.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        l.addView(host);
        l.addView(label("Port (plain UDP)"));
        EditText port = field(String.valueOf(prefs.port), true);
        l.addView(port);

        l.addView(label("Codec"));
        int sel = 0;
        for (int i = 0; i < Prefs.CODEC_IDS.length; i++) if (Prefs.CODEC_IDS[i] == prefs.codec) sel = i;
        Spinner codec = spinner(Prefs.CODECS, sel);
        l.addView(codec);
        l.addView(label("Bitrate (Mb/s)"));
        EditText mbps = field(String.valueOf(prefs.mbps), true);
        l.addView(mbps);
        l.addView(label("Size (WxH; empty: this screen's)"));
        EditText size = field(prefs.size, false);
        l.addView(size);
        l.addView(label("Refresh (Hz; empty: this screen's)"));
        EditText refresh = field(prefs.refresh > 0 ? String.valueOf(prefs.refresh) : "", true);
        l.addView(refresh);

        l.addView(label("Pacing"));
        Spinner pacing = spinner(new String[] {"Auto (phase lock to this screen)", "Low latency (frames as they come)"}, prefs.pacing);
        l.addView(pacing);
        CheckBox pieces = check("AV1/HEVC: take frames in pieces as the host encodes them", prefs.streamFrames);
        l.addView(pieces);
        l.addView(label("AV1/HEVC parallelism (tiles or slice segments; 0: the host's default)"));
        EditText par = field(String.valueOf(prefs.parallelism), true);
        l.addView(par);
        l.addView(label("Riptide stripes (0: the host's default)"));
        EditText stripes = field(String.valueOf(prefs.stripes), true);
        l.addView(stripes);
        CheckBox awake = check("Keep Wi-Fi awake (low-latency mode, uplink at 60 Hz)", prefs.keepWiFiAwake);
        l.addView(awake);
        CheckBox gpu = check("Keep the GPU awake for Riptide", prefs.keepGPUAwake);
        l.addView(gpu);
        CheckBox touch = check("Fingers are a touchscreen (off: a mouse)", prefs.touchscreen);
        l.addView(touch);
        CheckBox showStats = check("Show stats", prefs.showStats);
        l.addView(showStats);

        l.addView(label("This device's key: add the line to ~/.ssh/authorized_keys on the host"));
        String line = Native.authorizedLine(h, Build.MODEL.replaceAll("[^A-Za-z0-9_-]", ""));
        TextView key = new TextView(this);
        key.setText(line);
        key.setTextColor(0xff99ccff);
        key.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        key.setTextIsSelectable(true);
        l.addView(key);
        LinearLayout row = new LinearLayout(this);
        Button copy = new Button(this);
        copy.setText("Copy key line");
        copy.setOnClickListener(v -> ((ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE))
            .setPrimaryClip(ClipData.newPlainText("gmstream key", line)));
        row.addView(copy);
        Button forget = new Button(this);
        forget.setText("Forget host keys");
        forget.setOnClickListener(v -> Native.forgetHosts(h));
        row.addView(forget);
        l.addView(row);

        Button done = new Button(this);
        done.setText("Connect");
        done.setOnClickListener(v -> {
            prefs.host = host.getText().toString().trim();
            prefs.port = intOf(port, 8444);
            prefs.codec = Prefs.CODEC_IDS[codec.getSelectedItemPosition()];
            prefs.mbps = Math.max(1, intOf(mbps, prefs.mbps));
            prefs.size = size.getText().toString().trim();
            try { prefs.refresh = refresh.getText().toString().trim().isEmpty() ? 0 : Double.parseDouble(refresh.getText().toString().trim()); }
            catch (NumberFormatException x) { prefs.refresh = 0; }
            prefs.pacing = pacing.getSelectedItemPosition();
            prefs.streamFrames = pieces.isChecked();
            prefs.parallelism = Math.max(0, intOf(par, 0));
            prefs.stripes = Math.max(0, intOf(stripes, 0));
            prefs.keepWiFiAwake = awake.isChecked();
            prefs.keepGPUAwake = gpu.isChecked();
            prefs.touchscreen = touch.isChecked();
            prefs.showStats = showStats.isChecked();
            prefs.save(this);
            closeSettings();
        });
        l.addView(done);
        root.addView(settings, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        host.requestFocus();
    }

    private void closeSettings() {
        if (settings == null) return;
        root.removeView(settings);
        settings = null;
        hideSystemBars();
        root.requestFocus();
        if (prefs.host.isEmpty()) return;
        if (!streaming || !prefs.connectionKey().equals(startedWith)) {
            stopStream();
            startStream();
        } else {
            Native.setBitrate(h, prefs.mbps);
        }
    }
}
