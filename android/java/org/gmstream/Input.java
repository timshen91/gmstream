package org.gmstream;

import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.SparseArray;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * Game controllers (the Odin's own included) as W3C standard-mapping gamepads
 * (apple/Gamepads.swift), keyboards as Linux evdev codes, and rumble.
 */
final class Input {
    private final long h;
    private final Vibrator systemVibrator;
    private final SparseArray<Pad> pads = new SparseArray<>(); // by device id

    private static final class Pad {
        int slot;
        int buttons;
        float lx, ly, rx, ry, lt, rt, hatX, hatY;
        String last = "";
        Vibrator vibrator;
    }

    Input(long h, Vibrator systemVibrator) {
        this.h = h;
        this.systemVibrator = systemVibrator;
    }

    static boolean isPad(InputDevice d) {
        if (d == null) return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    int connected() { return pads.size(); }

    private Pad pad(InputDevice d) {
        Pad p = pads.get(d.getId());
        if (p != null) return p;
        boolean[] used = new boolean[4];
        for (int i = 0; i < pads.size(); i++) used[pads.valueAt(i).slot] = true;
        int slot = 0;
        while (slot < 3 && used[slot]) slot++;
        p = new Pad();
        p.slot = slot;
        VibratorManager vm = d.getVibratorManager();
        int[] ids = vm.getVibratorIds();
        p.vibrator = ids.length > 0 ? vm.getVibrator(ids[0]) : null;
        pads.put(d.getId(), p);
        return p;
    }

    void removed(int deviceId) {
        Pad p = pads.get(deviceId);
        if (p == null) return;
        pads.remove(deviceId);
        Native.gamepadRemove(h, p.slot);
    }

    /** After (re)connecting: every pad's state again. */
    void resend() {
        for (int i = 0; i < pads.size(); i++) {
            pads.valueAt(i).last = "";
            send(pads.valueAt(i));
        }
    }

    private static int bit(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return 0;
            case KeyEvent.KEYCODE_BUTTON_B: return 1;
            case KeyEvent.KEYCODE_BUTTON_X: return 2;
            case KeyEvent.KEYCODE_BUTTON_Y: return 3;
            case KeyEvent.KEYCODE_BUTTON_L1: return 4;
            case KeyEvent.KEYCODE_BUTTON_R1: return 5;
            case KeyEvent.KEYCODE_BUTTON_L2: return 6;
            case KeyEvent.KEYCODE_BUTTON_R2: return 7;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return 8;
            case KeyEvent.KEYCODE_BUTTON_START: return 9;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return 10;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return 11;
            case KeyEvent.KEYCODE_DPAD_UP: return 12;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 13;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 14;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 15;
            case KeyEvent.KEYCODE_BUTTON_MODE: return 16;
            default: return -1;
        }
    }

    /** A controller's button; true if it was one. */
    boolean padKey(KeyEvent e) {
        InputDevice d = e.getDevice();
        if (!isPad(d)) return false;
        int b = bit(e.getKeyCode());
        if (b < 0) return false;
        if (e.getRepeatCount() > 0) return true;
        Pad p = pad(d);
        if (e.getAction() == KeyEvent.ACTION_DOWN) p.buttons |= 1 << b; else p.buttons &= ~(1 << b);
        send(p);
        return true;
    }

    /** A controller's sticks, triggers and hat; true if it was one. */
    boolean padMotion(MotionEvent e) {
        InputDevice d = e.getDevice();
        if (!isPad(d) || (e.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
            || e.getAction() != MotionEvent.ACTION_MOVE) return false;
        Pad p = pad(d);
        p.lx = e.getAxisValue(MotionEvent.AXIS_X);
        p.ly = e.getAxisValue(MotionEvent.AXIS_Y);
        p.rx = e.getAxisValue(MotionEvent.AXIS_Z);
        p.ry = e.getAxisValue(MotionEvent.AXIS_RZ);
        p.lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
        p.rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
        p.hatX = e.getAxisValue(MotionEvent.AXIS_HAT_X);
        p.hatY = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        send(p);
        return true;
    }

    private static int axis(float v) {
        return Math.max(-32767, Math.min(32767, Math.round(v * 32767)));
    }

    private void send(Pad p) {
        int buttons = p.buttons;
        // A d-pad reported as a hat (no key events).
        if (p.hatY < -0.5f) buttons |= 1 << 12;
        if (p.hatY > 0.5f) buttons |= 1 << 13;
        if (p.hatX < -0.5f) buttons |= 1 << 14;
        if (p.hatX > 0.5f) buttons |= 1 << 15;
        int lt = Math.round(Math.max(0, Math.min(1, p.lt)) * 255), rt = Math.round(Math.max(0, Math.min(1, p.rt)) * 255);
        // Analog triggers also press their buttons, as the standard mapping has them.
        if (lt > 30) buttons |= 1 << 6;
        if (rt > 30) buttons |= 1 << 7;
        // Android's stick y points down already, as the protocol's does.
        int lx = axis(p.lx), ly = axis(p.ly), rx = axis(p.rx), ry = axis(p.ry);
        String state = buttons + "," + lx + "," + ly + "," + rx + "," + ry + "," + lt + "," + rt;
        if (state.equals(p.last)) return;
        p.last = state;
        Native.gamepad(h, p.slot, buttons, lx, ly, rx, ry, lt, rt);
    }

    /** Rumble from the host on the pad's own motor, else the device's. */
    void rumble(int index, int strong, int weak, int ms) {
        Vibrator v = systemVibrator;
        for (int i = 0; i < pads.size(); i++)
            if (pads.valueAt(i).slot == index && pads.valueAt(i).vibrator != null) v = pads.valueAt(i).vibrator;
        if (v == null || !v.hasVibrator()) return;
        int amp = Math.max(strong, weak) * 255 / 65535;
        if (ms == 0 || amp == 0) {
            v.cancel();
            return;
        }
        v.vibrate(VibrationEffect.createOneShot(ms, Math.max(1, amp)));
    }

    // ---- Keyboards: Linux evdev codes. A hardware keyboard reports its scan
    // code, which on Android is the evdev code; the rest go through this table.

    static int evdev(KeyEvent e) {
        int sc = e.getScanCode();
        if (sc > 0 && sc < 0x300) return sc;
        int k = e.getKeyCode();
        if (k >= KeyEvent.KEYCODE_A && k <= KeyEvent.KEYCODE_Z) {
            int[] row = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
            return row[k - KeyEvent.KEYCODE_A];
        }
        if (k >= KeyEvent.KEYCODE_1 && k <= KeyEvent.KEYCODE_9) return 2 + (k - KeyEvent.KEYCODE_1);
        switch (k) {
            case KeyEvent.KEYCODE_0: return 11;
            case KeyEvent.KEYCODE_ESCAPE: return 1;
            case KeyEvent.KEYCODE_DEL: return 14;
            case KeyEvent.KEYCODE_TAB: return 15;
            case KeyEvent.KEYCODE_ENTER: return 28;
            case KeyEvent.KEYCODE_SPACE: return 57;
            case KeyEvent.KEYCODE_MINUS: return 12;
            case KeyEvent.KEYCODE_EQUALS: return 13;
            case KeyEvent.KEYCODE_LEFT_BRACKET: return 26;
            case KeyEvent.KEYCODE_RIGHT_BRACKET: return 27;
            case KeyEvent.KEYCODE_SEMICOLON: return 39;
            case KeyEvent.KEYCODE_APOSTROPHE: return 40;
            case KeyEvent.KEYCODE_GRAVE: return 41;
            case KeyEvent.KEYCODE_BACKSLASH: return 43;
            case KeyEvent.KEYCODE_COMMA: return 51;
            case KeyEvent.KEYCODE_PERIOD: return 52;
            case KeyEvent.KEYCODE_SLASH: return 53;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return 42;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 54;
            case KeyEvent.KEYCODE_CTRL_LEFT: return 29;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 97;
            case KeyEvent.KEYCODE_ALT_LEFT: return 56;
            case KeyEvent.KEYCODE_ALT_RIGHT: return 100;
            case KeyEvent.KEYCODE_META_LEFT: return 125;
            case KeyEvent.KEYCODE_META_RIGHT: return 126;
            case KeyEvent.KEYCODE_DPAD_UP: return 103;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 108;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 105;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 106;
            case KeyEvent.KEYCODE_PAGE_UP: return 104;
            case KeyEvent.KEYCODE_PAGE_DOWN: return 109;
            case KeyEvent.KEYCODE_MOVE_HOME: return 102;
            case KeyEvent.KEYCODE_MOVE_END: return 107;
            case KeyEvent.KEYCODE_INSERT: return 110;
            case KeyEvent.KEYCODE_FORWARD_DEL: return 111;
            default:
                if (k >= KeyEvent.KEYCODE_F1 && k <= KeyEvent.KEYCODE_F10) return 59 + (k - KeyEvent.KEYCODE_F1);
                if (k == KeyEvent.KEYCODE_F11) return 87;
                if (k == KeyEvent.KEYCODE_F12) return 88;
                return 0;
        }
    }
}
