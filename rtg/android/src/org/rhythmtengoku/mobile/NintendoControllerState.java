package org.rhythmtengoku.mobile;

import java.util.ArrayList;
import java.util.LinkedHashMap;

/** Touchable's "rhythm" layout: the right half is A, the left half follows the
 *  current scene, and B, -, +, Y, X sit in the letterbox bars. Game-specific
 *  scene hints arrive from the native side (touch_hint.c). */
final class NintendoControllerState {
    // Keep these bits identical to rtg/host/dm_input.h (nn::hid::NpadButton).
    static final long A = 1L, B = 1L << 1, X = 1L << 2, Y = 1L << 3;
    static final long PLUS = 1L << 10, MINUS = 1L << 11, L = 1L << 6, R = 1L << 7, ZL = 1L << 8, ZR = 1L << 9;
    static final long LEFT = 1L << 12, UP = 1L << 13, RIGHT = 1L << 14, DOWN = 1L << 15;

    static final int KIND_DIRECTION = 0, KIND_BUTTON = 1, KIND_A_REGION = 2, KIND_LEFT_REGION = 3;
    // Packed native hint layout values.
    static final int LAYOUT_DPAD = 0, LAYOUT_ONE = 1, LAYOUT_TWO = 2, LAYOUT_MENU = 3;
    static final int SHOW_DPAD = 1, SHOW_X = 2, SHOW_Y = 4;
    static final int DIR_LEFT = 1, DIR_UP = 2, DIR_RIGHT = 4, DIR_DOWN = 8;

    static final class Control {
        final int id, kind;
        final String label, description;
        final long mask;
        float x, y, size;
        boolean visible, faded;
        Control(int id, int kind, String label, String description, long mask) {
            this.id = id; this.kind = kind; this.label = label; this.description = description; this.mask = mask;
        }
        boolean contains(float px, float py) {
            float radius = size / 2;
            return visible && (px - x) * (px - x) + (py - y) * (py - y) <= radius * radius;
        }
    }
    private static final class Pointer {
        final Control origin;
        final int kind;
        float x, y;
        Pointer(int kind, Control origin, float x, float y) { this.kind = kind; this.origin = origin; this.x = x; this.y = y; }
    }

    private final LinkedHashMap<Integer, Pointer> pointers = new LinkedHashMap<>();
    private final ArrayList<Control> controls = new ArrayList<>();
    private long accessibilityButtons, buttons;
    private int hintLayout = LAYOUT_MENU, hintFirst, hintSecond, hintExtra, lastDownRegion = -1;
    private float contentLeft, contentTop, contentRight, contentBottom;
    private float leftX, leftY, leftRight, leftBottom;
    private float aLeft, aTop, aRight, aBottom;
    private boolean verticalBars;

    /** dp geometry; the top utility strip never emits rhythm inputs. */
    void layout(float w, float h, float safeL, float safeT, float safeR, float safeB) {
        clear(); controls.clear();
        if (w <= 0 || h <= 0) return;
        int[] fit = GameDisplayMetrics.fitGame(Math.round(w), Math.round(h));
        float cw = fit[0], ch = fit[1];
        contentLeft = (w - cw) / 2; contentRight = contentLeft + cw;
        float gameTop = (h - ch) / 2, gameBottom = gameTop + ch;
        verticalBars = gameTop >= 44 && h - gameBottom >= 44;
        float contentL = verticalBars ? contentLeft : 0, contentR = verticalBars ? contentRight : w;
        float bottom = verticalBars ? gameBottom : h;
        contentBottom = bottom;
        contentTop = Math.max(safeT, 0) + 60;
        float half = (contentL + contentR) / 2;
        aLeft = half + 4; aTop = contentTop; aRight = contentR - 4; aBottom = bottom - 6;
        leftX = contentL + 4; leftY = contentTop; leftRight = half - 4; leftBottom = bottom - 6;
        float controlBottom = verticalBars ? gameBottom : h - Math.max(safeB + 18, 36);
        float left = Math.max(safeL + 12, 28), right = w - Math.max(safeR + 12, 28);
        float cx = left + 74, cy = controlBottom - 74, d = 54;
        /* The letterbox width already computed by fitGame is the same edge the
         * native present path draws; do not solve 16:9 a second time. */
        float bar = contentLeft;
        float lx = bar >= 52 ? bar / 2 : left + 30;
        float rx = bar >= 52 ? w - bar / 2 : right - 30;
        float by = Math.max(safeT + 30, 48);
        float[] xs = {cx - d, cx, cx + d, cx, lx, rx, rx, lx, lx};
        float[] ys = {cy, cy - d, cy, cy + d, by, by + 56, by, by + 56, by + 112};
        if (verticalBars) {
            float topY = Math.max(safeT + 22, 22);
            topY = Math.min(topY, gameTop / 2);
            float pair = 50;
            xs[4] = lx; ys[4] = topY;                 // B
            xs[7] = lx + pair; ys[7] = topY;          // Y
            xs[8] = lx + 2 * pair; ys[8] = topY;      // X
            xs[5] = rx - pair; ys[5] = topY;          // -
            xs[6] = rx; ys[6] = topY;                 // +
        }
        add(KIND_DIRECTION, "←", "方向键，左", LEFT, xs[0], ys[0], 50);
        add(KIND_DIRECTION, "↑", "方向键，上", UP, xs[1], ys[1], 50);
        add(KIND_DIRECTION, "→", "方向键，右", RIGHT, xs[2], ys[2], 50);
        add(KIND_DIRECTION, "↓", "方向键，下", DOWN, xs[3], ys[3], 50);
        add(KIND_BUTTON, "B", "B，返回或辅助操作；按下或按住，抬手松开", B, xs[4], ys[4], 44);
        add(KIND_BUTTON, "−", "减号，跳过练习", MINUS, xs[5], ys[5], 44);
        add(KIND_BUTTON, "+", "加号，游戏暂停菜单", PLUS, xs[6], ys[6], 44);
        add(KIND_BUTTON, "Y", "Y，按住查看示范，松开继续练习", Y, xs[7], ys[7], 44);
        add(KIND_BUTTON, "X", "X，模式辅助操作", X, xs[8], ys[8], 44);
        applyHint();
    }
    private void add(int kind, String label, String description, long mask, float x, float y, float size) {
        Control c = new Control(controls.size(), kind, label, description, mask);
        c.x = x; c.y = y; c.size = size; controls.add(c);
    }
    ArrayList<Control> controls() { return controls; }
    void setHint(int layout, int first, int second, int extra) {
        hintLayout = layout; hintFirst = first; hintSecond = second; hintExtra = extra;
        // A held finger must not keep a button of the previous scene.
        pointers.entrySet().removeIf(e -> e.getValue().kind == KIND_LEFT_REGION);
        applyHint();
    }
    int hintLayout() { return hintLayout; }
    int hintFirst() { return hintFirst; } int hintSecond() { return hintSecond; }
    static String labelFor(int npadBit) {
        switch (npadBit) {
            case 0: return "A"; case 1: return "B"; case 2: return "X"; case 3: return "Y";
            case 12: return "←"; case 13: return "↑"; case 14: return "→"; case 15: return "↓";
        }
        return "";
    }
    private void applyHint() {
        boolean menu = hintLayout == LAYOUT_MENU;
        boolean level = !menu;
        boolean dpad = menu || hintLayout == LAYOUT_DPAD || (hintExtra & SHOW_DPAD) != 0;
        int required = hintLayout == LAYOUT_DPAD ? hintFirst : 0;
        for (Control c : controls) {
            if (c.kind == KIND_DIRECTION) {
                c.visible = dpad;
                c.faded = level && hintLayout == LAYOUT_DPAD && required != 0 && (required & directionBit(c.mask)) == 0;
            } else if (c.mask == B || c.mask == MINUS || c.mask == PLUS) {
                c.visible = true;
            } else if (c.mask == Y) {
                c.visible = level && (hintExtra & SHOW_Y) != 0;
            } else if (c.mask == X) {
                c.visible = level && (hintExtra & SHOW_X) != 0;
            }
        }
        // Release any held control that the new scene hid.
        for (Integer id : new ArrayList<>(pointers.keySet())) {
            Pointer p = pointers.get(id);
            if (p != null && p.origin != null && !p.origin.visible) pointers.remove(id);
        }
        recompute();
    }
    private static int directionBit(long mask) {
        return mask == LEFT ? DIR_LEFT : mask == UP ? DIR_UP : mask == RIGHT ? DIR_RIGHT : DIR_DOWN;
    }
    Control control(int id) { return id >= 0 && id < controls.size() ? controls.get(id) : null; }
    Control button(long mask) { for (Control c : controls) if (c.mask == mask) return c; return null; }
    float contentLeft() { return contentLeft; } float contentTop() { return contentTop; }
    float contentRight() { return contentRight; } float contentBottom() { return contentBottom; }
    float leftX() { return leftX; } float leftY() { return leftY; } float leftRight() { return leftRight; } float leftBottom() { return leftBottom; }
    float aLeft() { return aLeft; } float aTop() { return aTop; } float aRight() { return aRight; } float aBottom() { return aBottom; }
    boolean verticalBars() { return verticalBars; }
    boolean leftVisible() { return hintLayout != LAYOUT_MENU; }
    long buttons() { return buttons; }
    boolean held(Control c) { return (buttons & c.mask) != 0; }

    private boolean inA(float x, float y) { return x >= aLeft && x <= aRight && y >= aTop && y <= aBottom; }
    private boolean inLeft(float x, float y) { return hintLayout != LAYOUT_MENU && x >= leftX && x <= leftRight && y >= leftY && y <= leftBottom; }
    private long leftMask(float x, float y) {
        if (hintLayout == LAYOUT_ONE) return bit(hintFirst);
        if (hintLayout == LAYOUT_TWO) return bit(x < (leftX + leftRight) / 2 ? hintFirst : hintSecond);
        if (hintLayout == LAYOUT_DPAD) {
            float dx = x - (leftX + leftRight) / 2, dy = y - (leftY + leftBottom) / 2;
            if (dx * dx + dy * dy < 20 * 20) return 0;
            return Math.abs(dx) > Math.abs(dy) ? (dx < 0 ? LEFT : RIGHT) : (dy < 0 ? UP : DOWN);
        }
        return 0;
    }
    private static long bit(int number) { return number >= 0 && number < 64 ? 1L << number : 0; }

    void down(int pointerId, float x, float y) {
        lastDownRegion = -1;
        if (pointers.containsKey(pointerId) || pointers.size() >= 16) return;
        for (Control c : controls) if (c.contains(x, y)) {
            pointers.put(pointerId, new Pointer(c.kind, c, x, y)); recompute(); return;
        }
        if (inA(x, y)) lastDownRegion = KIND_A_REGION;
        else if (inLeft(x, y)) lastDownRegion = KIND_LEFT_REGION;
        else return;
        pointers.put(pointerId, new Pointer(lastDownRegion, null, x, y));
        recompute();
    }
    /** Region that the last down() landed in, or -1 for a button/blank area. */
    int lastDownRegion() { return lastDownRegion; }
    void move(int pointerId, float x, float y) {
        Pointer p = pointers.get(pointerId); if (p == null) return;
        p.x = x; p.y = y;
        // A keeps its DOWN owner through drift; buttons release when the
        // finger leaves and press again when it returns.
        recompute();
    }
    void up(int pointerId) { pointers.remove(pointerId); recompute(); }
    boolean owns(int pointerId) { return pointers.containsKey(pointerId); }
    /** Whether a finger in the adaptive left half currently holds a button. */
    boolean leftHeld() {
        for (Pointer p : pointers.values())
            if (p.kind == KIND_LEFT_REGION && leftMask(p.x, p.y) != 0) return true;
        return false;
    }
    void accessibilityButton(int id, boolean down) {
        Control c = control(id); if (c == null) return;
        if (down) accessibilityButtons |= c.mask; else accessibilityButtons &= ~c.mask;
        recompute();
    }
    void clear() { pointers.clear(); accessibilityButtons = buttons = 0; }
    private void recompute() {
        buttons = accessibilityButtons;
        for (Pointer p : pointers.values()) {
            if (p.origin != null) {
                if (p.origin.visible && p.origin.contains(p.x, p.y)) buttons |= p.origin.mask;
            } else if (p.kind == KIND_A_REGION) {
                buttons |= A;
            } else if (p.kind == KIND_LEFT_REGION) {
                buttons |= leftMask(p.x, p.y);
            }
        }
    }
}