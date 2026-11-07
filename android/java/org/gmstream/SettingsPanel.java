package org.gmstream;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * The settings, as a panel at the side of the stream, which keeps running
 * behind it. Made for a handheld: every row is a list of choices or a
 * switch, stepped with the D-pad, the hat or the left stick (left / right
 * changes, A switches or steps on, B or Back closes) or by touch (the arrows,
 * or the row). A change applies as it is made (MainActivity.applySettings):
 * nothing is typed except the host, and only a new host reconnects.
 */
final class SettingsPanel {
    interface Owner {
        /** A setting changed: save it and apply it to the stream. */
        void applySettings();
        /** The host row: ask for a host and port. */
        void editHost();
        void copyKey();
        void forgetHosts();
        void closeSettings();
    }

    /** One row: a label and a value that steps, or an action. */
    private abstract static class Row {
        final String label;
        boolean header;
        TextView value, left, right;
        View view;
        Row(String label) { this.label = label; }
        /** +1 / -1: the next or previous value; 0: A or a tap on the row. */
        abstract void step(int d);
        abstract String text();
        boolean enabled() { return true; }
        boolean stepsSideways() { return true; }
    }

    private final Context ctx;
    private final Prefs prefs;
    private final Owner owner;
    private final List<Row> rows = new ArrayList<>();
    private int selected;
    private final ScrollView scroll;
    private final LinearLayout list;
    final View view;

    private static final int[] BITRATES = {5, 10, 15, 20, 30, 40, 50, 60, 80, 100, 150, 200, 300};
    private static final String[] SIZES = {"", "1280x720", "1600x900", "1920x1080", "2560x1440", "2560x1600"};
    private static final double[] REFRESHES = {0, 60, 120};
    private static final int[] TILES = {0, 1, 2, 4, 8, 16, 32};
    private static final int[] STRIPES = {0, 2, 3, 4, 5, 6, 8, 10, 12, 16};

    SettingsPanel(Context ctx, Prefs prefs, Owner owner, String screenSize, String keyLine) {
        this.ctx = ctx;
        this.prefs = prefs;
        this.owner = owner;

        // Stream: what the host sends.
        rows.add(header("Stream"));
        rows.add(choice("Codec", Prefs.CODECS.length, () -> indexOf(Prefs.CODEC_IDS, prefs.codec),
            i -> prefs.codec = Prefs.CODEC_IDS[i], i -> Prefs.CODECS[i]));
        rows.add(choice("Bitrate", BITRATES.length, () -> nearest(BITRATES, prefs.mbps),
            i -> prefs.mbps = BITRATES[i], i -> BITRATES[i] + " Mb/s"));
        rows.add(choice("Resolution", SIZES.length, () -> indexOf(SIZES, prefs.size),
            i -> prefs.size = SIZES[i], i -> SIZES[i].isEmpty() ? "Screen " + screenSize : SIZES[i]));
        rows.add(choice("Refresh", REFRESHES.length, () -> indexOf(REFRESHES, prefs.refresh),
            i -> prefs.refresh = REFRESHES[i], i -> REFRESHES[i] == 0 ? "This screen" : (int) REFRESHES[i] + " Hz"));
        rows.add(choice("Pacing", 2, () -> prefs.pacing, i -> prefs.pacing = i,
            i -> i == 0 ? "Phase lock" : "Low latency"));
        rows.add(toggle("Scanout", () -> prefs.scanout, v -> prefs.scanout = v, null));
        rows.add(toggle("Frames in pieces", () -> prefs.streamFrames, v -> prefs.streamFrames = v, this::avOrHevc));
        rows.add(choice("Tiles / slices", TILES.length, () -> indexOf(TILES, prefs.parallelism),
            i -> prefs.parallelism = TILES[i], i -> TILES[i] == 0 ? "Host's choice" : String.valueOf(TILES[i]),
            this::avOrHevc));
        rows.add(choice("Stripes", STRIPES.length, () -> indexOf(STRIPES, prefs.stripes),
            i -> prefs.stripes = STRIPES[i], i -> STRIPES[i] == 0 ? "Host's choice" : String.valueOf(STRIPES[i]),
            () -> prefs.codec == 4));

        // This device.
        rows.add(header("This device"));
        rows.add(choice("Fingers", 2, () -> prefs.touchscreen ? 0 : 1, i -> prefs.touchscreen = i == 0,
            i -> i == 0 ? "Touchscreen" : "Mouse"));
        rows.add(toggle("Wi-Fi awake", () -> prefs.keepWiFiAwake, v -> prefs.keepWiFiAwake = v, null));
        rows.add(toggle("GPU awake", () -> prefs.keepGPUAwake, v -> prefs.keepGPUAwake = v, () -> prefs.codec == 4));
        rows.add(choice("Stats", 3, () -> prefs.statsMode, i -> prefs.statsMode = i,
            i -> i == 0 ? "Off" : i == 1 ? "Chart only" : "Full"));

        // The connection.
        rows.add(header("Connection"));
        rows.add(action("Host", () -> prefs.host.isEmpty() ? "Set…" : prefs.host + ":" + prefs.port, owner::editHost));
        rows.add(action("Device key", () -> "Copy", owner::copyKey));
        rows.add(action("Forget host keys", () -> "Forget", owner::forgetHosts));
        rows.add(action("Close", () -> "B", owner::closeSettings));

        list = new LinearLayout(ctx);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(16), dp(12), dp(16), dp(16));
        TextView title = text("gmstream", 20, Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        list.addView(title);
        TextView hint = text("D-pad or stick: move · ◀ ▶ change · A select · B close", 12, 0xff8a94a6);
        hint.setPadding(0, dp(2), 0, dp(6));
        list.addView(hint);
        for (int i = 0; i < rows.size(); i++) list.addView(build(rows.get(i), i));
        TextView key = text(keyLine, 10, 0xff6f7a8c);
        key.setPadding(0, dp(10), 0, 0);
        key.setTextIsSelectable(true);
        list.addView(key);

        scroll = new ScrollView(ctx);
        scroll.setBackgroundColor(0xf0101318);
        scroll.addView(list);
        scroll.setVerticalScrollBarEnabled(false);
        view = scroll;
        selected = next(0, 1);
        refresh();
    }

    // ---- Rows ---------------------------------------------------------------------------

    private interface IntGet { int get(); }
    private interface IntSet { void set(int v); }
    private interface Label { String of(int i); }
    private interface BoolGet { boolean get(); }
    private interface BoolSet { void set(boolean v); }
    private interface Text { String get(); }

    private Row header(String s) {
        Row r = new Row(s) {
            void step(int d) {}
            String text() { return ""; }
        };
        r.header = true;
        return r;
    }

    private Row choice(String label, int n, IntGet get, IntSet set, Label name) {
        return choice(label, n, get, set, name, null);
    }

    private Row choice(String label, int n, IntGet get, IntSet set, Label name, BoolGet applies) {
        return new Row(label) {
            void step(int d) {
                int i = Math.floorMod(Math.max(0, get.get()) + (d == 0 ? 1 : d), n);
                set.set(i);
                changed();
            }
            String text() { return name.of(Math.max(0, get.get())); }
            boolean enabled() { return applies == null || applies.get(); }
        };
    }

    private Row toggle(String label, BoolGet get, BoolSet set, BoolGet applies) {
        return new Row(label) {
            void step(int d) {
                set.set(!get.get());
                changed();
            }
            String text() { return get.get() ? "On" : "Off"; }
            boolean enabled() { return applies == null || applies.get(); }
        };
    }

    private Row action(String label, Text shown, Runnable run) {
        return new Row(label) {
            void step(int d) { if (d == 0) run.run(); }
            String text() { return shown.get(); }
            boolean stepsSideways() { return false; }
        };
    }

    private void changed() {
        owner.applySettings();
        refresh();
    }

    private static int indexOf(int[] a, int v) {
        for (int i = 0; i < a.length; i++) if (a[i] == v) return i;
        return 0;
    }

    private static int indexOf(double[] a, double v) {
        for (int i = 0; i < a.length; i++) if (Math.abs(a[i] - v) < 0.5) return i;
        return 0;
    }

    private static int indexOf(String[] a, String v) {
        for (int i = 0; i < a.length; i++) if (a[i].equalsIgnoreCase(v.trim())) return i;
        return 0;
    }

    private static int nearest(int[] a, int v) {
        int best = 0;
        for (int i = 0; i < a.length; i++) if (Math.abs(a[i] - v) < Math.abs(a[best] - v)) best = i;
        return best;
    }

    private boolean avOrHevc() { return prefs.codec == 1 || prefs.codec == 2; }

    // ---- Views --------------------------------------------------------------------------

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, ctx.getResources().getDisplayMetrics());
    }

    private TextView text(String s, float sp, int color) {
        TextView t = new TextView(ctx);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        return t;
    }

    private View build(Row r, int index) {
        if (r.header) {
            TextView h = text(r.label.toUpperCase(), 12, 0xff7fa7ff);
            h.setTypeface(Typeface.DEFAULT_BOLD);
            h.setPadding(dp(4), dp(14), 0, dp(4));
            r.view = h;
            return h;
        }
        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(10), 0, dp(4), 0);
        row.setMinimumHeight(dp(44));
        TextView label = text(r.label, 15, Color.WHITE);
        label.setSingleLine(true);
        label.setEllipsize(android.text.TextUtils.TruncateAt.END);
        row.addView(label, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        if (r.stepsSideways()) {
            r.left = arrow("◀");
            r.left.setOnClickListener(v -> { select(index); r.step(-1); });
            row.addView(r.left);
        }
        r.value = text("", 15, 0xffdde6ff);
        r.value.setGravity(Gravity.CENTER);
        r.value.setMinWidth(dp(r.stepsSideways() ? 112 : 72));
        r.value.setSingleLine(true);
        row.addView(r.value);
        if (r.stepsSideways()) {
            r.right = arrow("▶");
            r.right.setOnClickListener(v -> { select(index); r.step(1); });
            row.addView(r.right);
        }
        row.setOnClickListener(v -> { select(index); r.step(0); });
        r.view = row;
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(2);
        row.setLayoutParams(lp);
        return row;
    }

    private TextView arrow(String s) {
        TextView t = text(s, 15, 0xff9fb4ff);
        t.setPadding(dp(8), dp(10), dp(8), dp(10));
        return t;
    }

    /** Values and the highlight, after any change. */
    void refresh() {
        for (int i = 0; i < rows.size(); i++) {
            Row r = rows.get(i);
            if (r.value == null) continue;
            r.value.setText(r.text());
            boolean on = r.enabled();
            float alpha = on ? 1f : 0.4f;
            r.view.setAlpha(alpha);
            GradientDrawable bg = new GradientDrawable();
            bg.setCornerRadius(dp(8));
            bg.setColor(i == selected ? 0xff26324a : 0x00000000);
            if (i == selected) bg.setStroke(dp(1), 0xff5b7cff);
            r.view.setBackground(bg);
        }
    }

    // ---- Navigation ---------------------------------------------------------------------

    /** The next row from `from` in direction `d` that can be selected. */
    private int next(int from, int d) {
        for (int i = Math.max(0, Math.min(rows.size() - 1, from)); i >= 0 && i < rows.size(); i += d)
            if (!rows.get(i).header) return i;
        return selected;
    }

    private void select(int i) {
        selected = i;
        refresh();
        View v = rows.get(i).view;
        if (v != null) scroll.post(() -> {
            int top = v.getTop() - dp(40), bottom = v.getBottom() + dp(40);
            if (top < scroll.getScrollY()) scroll.smoothScrollTo(0, Math.max(0, top));
            else if (bottom > scroll.getScrollY() + scroll.getHeight()) scroll.smoothScrollTo(0, bottom - scroll.getHeight());
        });
    }

    private void move(int d) {
        int n = next(selected + d, d);
        if (n != selected) select(n);
    }

    private void change(int d) {
        Row r = rows.get(selected);
        if (r.enabled() && (r.stepsSideways() || d == 0)) r.step(d);
    }

    /** A key while the panel is open; true if it was ours. */
    boolean key(KeyEvent e) {
        int k = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        switch (k) {
            case KeyEvent.KEYCODE_DPAD_UP: case KeyEvent.KEYCODE_W:
                if (down) move(-1);
                return true;
            case KeyEvent.KEYCODE_DPAD_DOWN: case KeyEvent.KEYCODE_S:
                if (down) move(1);
                return true;
            case KeyEvent.KEYCODE_DPAD_LEFT: case KeyEvent.KEYCODE_A:
                if (down) change(-1);
                return true;
            case KeyEvent.KEYCODE_DPAD_RIGHT: case KeyEvent.KEYCODE_D:
                if (down) change(1);
                return true;
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_DPAD_CENTER: case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_SPACE:
                if (down && e.getRepeatCount() == 0) change(0);
                return true;
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_BACK: case KeyEvent.KEYCODE_ESCAPE:
            case KeyEvent.KEYCODE_BUTTON_START: case KeyEvent.KEYCODE_BUTTON_SELECT:
                if (!down) owner.closeSettings();
                return true;
            case KeyEvent.KEYCODE_BUTTON_L1:
                if (down) move(-4);
                return true;
            case KeyEvent.KEYCODE_BUTTON_R1:
                if (down) move(4);
                return true;
            default:
                return true; // nothing else reaches the host while the panel is open
        }
    }

    // The hat and the left stick, as the D-pad: one step per push, again
    // every 180 ms while held past the first 400 ms.
    private int heldX, heldY;
    private long heldSince, lastRepeat;

    boolean motion(MotionEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
            && (e.getSource() & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD) return false;
        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        float sx = e.getAxisValue(MotionEvent.AXIS_X), sy = e.getAxisValue(MotionEvent.AXIS_Y);
        int x = hx > 0.5f || sx > 0.6f ? 1 : hx < -0.5f || sx < -0.6f ? -1 : 0;
        int y = hy > 0.5f || sy > 0.6f ? 1 : hy < -0.5f || sy < -0.6f ? -1 : 0;
        if (Math.abs(sx) > Math.abs(sy) && hx == 0 && hy == 0) y = 0; else if (hx == 0 && hy == 0) x = 0;
        long now = e.getEventTime();
        if (x != heldX || y != heldY) {
            heldX = x;
            heldY = y;
            heldSince = lastRepeat = now;
            if (y != 0) move(y); else if (x != 0) change(x);
        } else if ((x != 0 || y != 0) && now - heldSince > 400 && now - lastRepeat > 180) {
            lastRepeat = now;
            if (y != 0) move(y); else change(x);
        }
        return true;
    }

    static FrameLayout.LayoutParams layout(int screenWidth) {
        int w = Math.max(Math.min(screenWidth, 620), screenWidth * 42 / 100);
        w = Math.min(w, screenWidth);
        return new FrameLayout.LayoutParams(w, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.END);
    }
}
