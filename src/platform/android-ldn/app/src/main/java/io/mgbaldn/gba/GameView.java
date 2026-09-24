package io.mgbaldn.gba;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/** Draws the game and the on-screen controls, and turns touches into GBA key bits. */
final class GameView extends SurfaceView implements SurfaceHolder.Callback, Emulator.FrameListener {
    // GBA key bits (mGBA's GBA_KEY_*).
    static final int KEY_A = 1, KEY_B = 1 << 1, KEY_SELECT = 1 << 2, KEY_START = 1 << 3, KEY_RIGHT = 1 << 4,
            KEY_LEFT = 1 << 5, KEY_UP = 1 << 6, KEY_DOWN = 1 << 7, KEY_R = 1 << 8, KEY_L = 1 << 9;

    /** Indices into the colour list: the D-pad first, then the buttons. */
    static final int COLOR_DPAD = 0, COLOR_A = 1, COLOR_B = 2, COLOR_L = 3, COLOR_R = 4, COLOR_START = 5, COLOR_SELECT = 6;
    static final String[] COLOR_NAMES = {"D-pad", "A", "B", "L", "R", "Start", "Select"};

    private static final class Button {
        final int key;
        final int colorIndex;
        final String label;
        final RectF area = new RectF();

        Button(int key, int colorIndex, String label) {
            this.key = key;
            this.colorIndex = colorIndex;
            this.label = label;
        }
    }

    private final Button a = new Button(KEY_A, COLOR_A, "A");
    private final Button b = new Button(KEY_B, COLOR_B, "B");
    private final Button l = new Button(KEY_L, COLOR_L, "L");
    private final Button r = new Button(KEY_R, COLOR_R, "R");
    private final Button start = new Button(KEY_START, COLOR_START, "START");
    private final Button select = new Button(KEY_SELECT, COLOR_SELECT, "SELECT");
    private final Button[] buttons = {a, b, l, r, start, select};
    private final RectF dpad = new RectF();

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint picture = new Paint();
    private final Paint overlayPaint = new Paint();
    private final Rect source = new Rect();
    private final RectF target = new RectF();
    private volatile int touchKeys;
    private volatile int otherKeys; // keyboard / gamepad
    private boolean surfaceReady;
    private volatile boolean hardwareCanvas = true;

    // Appearance settings (set from the UI thread, read by the emulator thread while drawing).
    private volatile boolean controlsVisible = true;
    private volatile int controlsOpacity = 35; // percent, of an unpressed button
    private volatile boolean scanlines;
    private volatile int scanlineStrength = 35; // percent
    private volatile int[] colors = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    private final Object decorLock = new Object();
    private Bitmap backgroundSource; // as chosen by the user
    private volatile Bitmap backgroundScaled; // cropped to the area under the game (portrait only)
    private Bitmap scanlineOverlay;
    private int overlayKey;

    GameView(Context context) {
        super(context);
        getHolder().addCallback(this);
        picture.setFilterBitmap(false);
        text.setTextAlign(Paint.Align.CENTER);
        setFocusable(true);
    }

    // ---- appearance ----

    void setControlsVisible(boolean visible) {
        controlsVisible = visible;
    }

    void setControlsOpacity(int percent) {
        controlsOpacity = Math.max(5, Math.min(100, percent));
    }

    void setScanlines(boolean enabled, int strengthPercent) {
        scanlines = enabled;
        scanlineStrength = Math.max(5, Math.min(100, strengthPercent));
    }

    void setColors(int[] value) {
        colors = value.clone();
    }

    int[] getColors() {
        return colors.clone();
    }

    /** The picture shown behind the buttons in portrait mode (null removes it). */
    void setBackgroundImage(Bitmap image) {
        synchronized (decorLock) {
            backgroundSource = image;
            rebuildBackground();
        }
    }

    private void rebuildBackground() {
        int w = getWidth();
        int h = getHeight();
        if (backgroundSource == null || w <= 0 || h <= w) {
            backgroundScaled = null;
            return;
        }
        int top = Math.round(w * 160f / 240f);
        int areaH = h - top;
        if (areaH <= 0) {
            backgroundScaled = null;
            return;
        }
        // Centre-crop the picture to fill the area exactly.
        Bitmap src = backgroundSource;
        float scale = Math.max((float) w / src.getWidth(), (float) areaH / src.getHeight());
        int cropW = Math.min(src.getWidth(), Math.round(w / scale));
        int cropH = Math.min(src.getHeight(), Math.round(areaH / scale));
        int x = (src.getWidth() - cropW) / 2;
        int y = (src.getHeight() - cropH) / 2;
        Bitmap cropped = Bitmap.createBitmap(src, x, y, cropW, cropH);
        backgroundScaled = Bitmap.createScaledBitmap(cropped, w, areaH, true);
    }

    void setOtherKeys(int keys) {
        otherKeys = keys;
        Native.setKeys(touchKeys | otherKeys);
    }

    int otherKeys() {
        return otherKeys;
    }

    // ---- layout ----

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        float unit = Math.min(w, h);
        boolean portrait = h >= w;
        float gameBottom = portrait ? w * 160f / 240f : 0;
        float area = portrait ? h - gameBottom : h;
        float top = portrait ? gameBottom : 0;
        float dpadSize = Math.min(unit * 0.5f, area * 0.55f);
        float cy = portrait ? top + area * 0.48f : h * 0.62f;
        float cx = portrait ? w * 0.24f : w * 0.14f;
        dpad.set(cx - dpadSize / 2, cy - dpadSize / 2, cx + dpadSize / 2, cy + dpadSize / 2);

        float ab = Math.min(unit * 0.2f, area * 0.24f);
        float ax = portrait ? w * 0.86f : w * 0.90f;
        float bx = portrait ? w * 0.66f : w * 0.78f;
        a.area.set(ax - ab / 2, cy - ab * 0.9f - ab / 2 + ab * 0.3f, ax + ab / 2, cy - ab * 0.9f + ab / 2 + ab * 0.3f);
        b.area.set(bx - ab / 2, cy + ab * 0.05f, bx + ab / 2, cy + ab * 1.05f);
        float shoulderW = Math.min(w * 0.3f, unit * 0.34f);
        float shoulderH = shoulderW * 0.36f;
        float shoulderY = portrait ? top + area * 0.04f : h * 0.08f;
        l.area.set(w * 0.04f, shoulderY, w * 0.04f + shoulderW, shoulderY + shoulderH);
        r.area.set(w * 0.96f - shoulderW, shoulderY, w * 0.96f, shoulderY + shoulderH);
        float smallW = Math.min(w * 0.2f, unit * 0.24f);
        float smallH = smallW * 0.32f;
        float smallY = h - smallH * 1.8f;
        select.area.set(w * 0.5f - smallW * 1.1f, smallY, w * 0.5f - smallW * 0.1f, smallY + smallH);
        start.area.set(w * 0.5f + smallW * 0.1f, smallY, w * 0.5f + smallW * 1.1f, smallY + smallH);
        text.setTextSize(Math.max(24f, unit * 0.045f));
        synchronized (decorLock) {
            rebuildBackground();
        }
    }

    private void computeTarget(int width, int height) {
        float w = getWidth();
        float h = getHeight();
        float scale = Math.min(w / width, h / height);
        float gw = width * scale;
        float gh = height * scale;
        float x = (w - gw) / 2;
        float y = h >= w ? 0 : (h - gh) / 2;
        target.set(x, y, x + gw, y + gh);
    }

    // ---- drawing (called from the emulator thread) ----

    @Override
    public void onFrame(Bitmap bitmap, int width, int height) {
        if (!surfaceReady) {
            return;
        }
        SurfaceHolder holder = getHolder();
        // A software canvas rasterises the whole (scaled-up) picture on the CPU every frame, which cost ~30% of the frame rate
        // on a wide landscape surface; a hardware canvas has the GPU scale it.
        Canvas canvas = null;
        if (hardwareCanvas) {
            try {
                canvas = holder.lockHardwareCanvas();
            } catch (RuntimeException e) {
                hardwareCanvas = false;
            }
        }
        if (canvas == null) {
            canvas = holder.lockCanvas();
        }
        if (canvas == null) {
            return;
        }
        try {
            canvas.drawColor(Color.BLACK);
            computeTarget(width, height);
            Bitmap background = backgroundScaled;
            if (background != null && getHeight() > getWidth()) {
                canvas.drawBitmap(background, 0, target.bottom, null);
            }
            source.set(0, 0, width, height);
            canvas.drawBitmap(bitmap, source, target, picture);
            if (scanlines) {
                drawScanlines(canvas, height);
            }
            if (controlsVisible) {
                drawControls(canvas);
            }
        } finally {
            holder.unlockCanvasAndPost(canvas);
        }
    }

    // Dark lines across the picture, one per game pixel row (like the scanline option of a GBA IPS screen). The lines are
    // drawn into a bitmap once per size and strength, then that bitmap is laid over the picture each frame.
    private void drawScanlines(Canvas canvas, int gameHeight) {
        int w = Math.max(1, Math.round(target.width()));
        int h = Math.max(1, Math.round(target.height()));
        int strength = scanlineStrength;
        int key = ((w * 31 + h) * 31 + gameHeight) * 31 + strength;
        if (scanlineOverlay == null || key != overlayKey) {
            if (scanlineOverlay != null) {
                scanlineOverlay.recycle();
            }
            scanlineOverlay = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(scanlineOverlay);
            Paint line = new Paint();
            line.setColor(Color.argb(Math.round(strength * 2.55f), 0, 0, 0));
            float rowHeight = (float) h / gameHeight;
            for (int i = 0; i < gameHeight; ++i) {
                float y = i * rowHeight;
                c.drawRect(0, y + rowHeight * 0.55f, w, y + rowHeight, line);
            }
            overlayKey = key;
        }
        canvas.drawBitmap(scanlineOverlay, target.left, target.top, overlayPaint);
    }

    private static boolean isLight(int color) {
        return (Color.red(color) * 299 + Color.green(color) * 587 + Color.blue(color) * 114) / 1000 > 150;
    }

    private void drawControls(Canvas canvas) {
        int keys = touchKeys;
        int[] palette = colors;
        int idle = Math.round(controlsOpacity * 2.55f);
        int pressed = Math.min(255, idle + 90);
        for (Button button : buttons) {
            boolean down = (keys & button.key) != 0;
            int base = palette[button.colorIndex];
            fill.setColor((base & 0x00FFFFFF) | ((down ? pressed : idle) << 24));
            float radius = button.area.height() / 2;
            if (button == a || button == b) {
                canvas.drawCircle(button.area.centerX(), button.area.centerY(), button.area.width() / 2, fill);
            } else {
                canvas.drawRoundRect(button.area, radius, radius, fill);
            }
            text.setColor(isLight(base) ? 0xCC000000 : 0xFFFFFFFF);
            canvas.drawText(button.label, button.area.centerX(), button.area.centerY() + text.getTextSize() * 0.35f, text);
        }
        int dpadColor = palette[COLOR_DPAD];
        float cx = dpad.centerX();
        float cy = dpad.centerY();
        float arm = dpad.width() * 0.17f;
        float half = dpad.width() / 2;
        fill.setColor((dpadColor & 0x00FFFFFF) | (idle << 24));
        canvas.drawRoundRect(cx - arm, cy - half, cx + arm, cy + half, arm / 2, arm / 2, fill);
        canvas.drawRoundRect(cx - half, cy - arm, cx + half, cy + arm, arm / 2, arm / 2, fill);
        fill.setColor((dpadColor & 0x00FFFFFF) | (pressed << 24));
        if ((keys & KEY_UP) != 0) canvas.drawRoundRect(cx - arm, cy - half, cx + arm, cy - arm * 0.2f, arm / 2, arm / 2, fill);
        if ((keys & KEY_DOWN) != 0) canvas.drawRoundRect(cx - arm, cy + arm * 0.2f, cx + arm, cy + half, arm / 2, arm / 2, fill);
        if ((keys & KEY_LEFT) != 0) canvas.drawRoundRect(cx - half, cy - arm, cx - arm * 0.2f, cy + arm, arm / 2, arm / 2, fill);
        if ((keys & KEY_RIGHT) != 0) canvas.drawRoundRect(cx + arm * 0.2f, cy - arm, cx + half, cy + arm, arm / 2, arm / 2, fill);
    }

    // ---- touch ----

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int keys = 0;
        int action = event.getActionMasked();
        if (action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_CANCEL) {
            for (int i = 0; i < event.getPointerCount(); ++i) {
                if (action == MotionEvent.ACTION_POINTER_UP && i == event.getActionIndex()) {
                    continue;
                }
                keys |= keysAt(event.getX(i), event.getY(i));
            }
        }
        touchKeys = keys;
        Native.setKeys(touchKeys | otherKeys);
        return true;
    }

    private int keysAt(float x, float y) {
        int keys = 0;
        for (Button button : buttons) {
            RectF area = button.area;
            float pad = area.height() * 0.25f;
            if (x >= area.left - pad && x <= area.right + pad && y >= area.top - pad && y <= area.bottom + pad) {
                keys |= button.key;
            }
        }
        float dx = x - dpad.centerX();
        float dy = y - dpad.centerY();
        float reach = dpad.width() * 0.75f;
        float dead = dpad.width() * 0.10f;
        if (Math.abs(dx) < reach && Math.abs(dy) < reach) {
            if (dx > dead) keys |= KEY_RIGHT;
            if (dx < -dead) keys |= KEY_LEFT;
            if (dy > dead) keys |= KEY_DOWN;
            if (dy < -dead) keys |= KEY_UP;
        }
        return keys;
    }

    // ---- surface ----

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surfaceReady = true;
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
    }
}
