package io.mgbaldn.gba;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
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
    private volatile int scanlineStrength = 35; // percent (horizontal lines)
    private volatile boolean vScanlines;
    private volatile int vScanlineStrength = 35;
    private volatile int pixelMode = 0;
    private volatile int[] colors = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    private volatile boolean showFrameCounter;
    private volatile int frameCounterInset; // pixels kept free at the top right for the menu button
    private final Paint counterText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint counterBox = new Paint();
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
        counterText.setColor(Color.WHITE);
        counterText.setTextAlign(Paint.Align.RIGHT);
        counterText.setTextSize(12 * getResources().getDisplayMetrics().scaledDensity);
        counterBox.setColor(0x66000000);
        setFocusable(true);
    }

    // ---- colour modes ----
    //
    // GBA IPS kits (the V5 "OSD" screens) offer several colour modes and a desaturation setting; the picture is drawn through
    // a colour matrix to do the same. Each mode is a matrix on 0-255 channel values; the desaturation percentage
    // (100 = as emulated, 0 = black and white) is applied first.
    static final int MODE_ORIGINAL = 0;
    static final int MODE_MUTED = 1;
    static final int MODE_VIVID = 2;
    static final int MODE_BLACK_WHITE = 3;

    static final int MODE_DMG = 4;
    static final String[] MODE_NAMES = {"Original", "Muted", "Vivid", "Black and white", "DMG"};

    void setColorMode(int mode, int saturationPercent) {
        ColorMatrix matrix = new ColorMatrix();
        matrix.setSaturation(Math.max(0, Math.min(100, saturationPercent)) / 100f);
        ColorMatrix extra = null;
        switch (mode) {
            case MODE_MUTED: {
                // Less saturated with a little less contrast, the washed-out look of the original GBA panel.
                extra = new ColorMatrix();
                extra.setSaturation(0.72f);
                ColorMatrix contrast = new ColorMatrix(new float[] {
                    0.92f, 0, 0, 0, 10, 0, 0.92f, 0, 0, 10, 0, 0, 0.92f, 0, 10, 0, 0, 0, 1, 0});
                extra.postConcat(contrast);
                break;
            }
            case MODE_VIVID:
                extra = new ColorMatrix();
                extra.setSaturation(1.3f);
                extra.postConcat(new ColorMatrix(new float[] {
                    1.08f, 0, 0, 0, -10, 0, 1.08f, 0, 0, -10, 0, 0, 1.08f, 0, -10, 0, 0, 0, 1, 0}));
                break;
            case MODE_BLACK_WHITE:
                extra = new ColorMatrix();
                extra.setSaturation(0);
                break;
            case MODE_DMG: {
                // The black and white picture tinted onto the Game Boy green ramp, softened a little from the LCD's own 0F380F..9BBC0F (163316 darkest, 9CB33A lightest): each
                // output channel is dark + brightness * (light - dark) / 255, with the brightness taken from the input.
                final float wr = 0.299f, wg = 0.587f, wb = 0.114f;
                final float[] dark = {0x16, 0x33, 0x16};
                final float[] light = {0x9C, 0xB3, 0x3A};
                float[] m = new float[20];
                for (int i = 0; i < 3; ++i) {
                    float scale = (light[i] - dark[i]) / 255f;
                    m[i * 5] = wr * scale;
                    m[i * 5 + 1] = wg * scale;
                    m[i * 5 + 2] = wb * scale;
                    m[i * 5 + 4] = dark[i];
                }
                m[18] = 1;
                extra = new ColorMatrix(m);
                matrix.setSaturation(1); // the matrix already takes the brightness of the picture as it is
                break;
            }
            default:
                break;
        }
        if (extra != null) {
            matrix.postConcat(extra);
        }
        picture.setColorFilter(mode == MODE_ORIGINAL && saturationPercent >= 100 ? null : new ColorMatrixColorFilter(matrix));
    }

    void setFrameCounter(boolean enabled, int menuInsetPixels) {
        showFrameCounter = enabled;
        frameCounterInset = menuInsetPixels;
    }

    private void drawFrameCounter(Canvas canvas, int frame) {
        String label = "Frame " + frame;
        float pad = counterText.getTextSize() * 0.4f;
        float right = getWidth() - frameCounterInset - 12;
        float width = counterText.measureText(label);
        float top = 6;
        float bottom = top + counterText.getTextSize() + pad * 2;
        canvas.drawRect(right - width - pad * 2, top, right, bottom, counterBox);
        canvas.drawText(label, right - pad, top + pad + counterText.getTextSize() * 0.85f, counterText);
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
        // A whole number of pixels, so the overlay effects can line up exactly with the picture's game pixels.
        float left = Math.round(x);
        float top = Math.round(y);
        target.set(left, top, left + Math.round(gw), top + Math.round(gh));
    }

    // ---- drawing (called from the emulator thread) ----

    @Override
    public void onFrame(Bitmap bitmap, int width, int height, int frame) {
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
            if (scanlines || vScanlines || pixelMode != PIXEL_OFF) {
                drawOverlay(canvas, width, height);
            }
            if (controlsVisible) {
                drawControls(canvas);
            }
            if (showFrameCounter) {
                drawFrameCounter(canvas, frame);
            }
        } finally {
            holder.unlockCanvasAndPost(canvas);
        }
    }

    // Everything laid over the picture (scanlines, pixel effect) is drawn into one bitmap the size of the picture, once per
    // combination of size and settings; that bitmap is then laid over the picture each frame.
    static final int PIXEL_OFF = 0;
    static final int PIXEL_GRID = 1;
    static final int PIXEL_ROUND = 2;
    static final int PIXEL_RGB = 3;
    static final String[] PIXEL_NAMES = {"Off", "Pixel grid", "Round pixels", "RGB subpixels"};

    void setVerticalScanlines(boolean enabled, int strengthPercent) {
        vScanlines = enabled;
        vScanlineStrength = Math.max(5, Math.min(100, strengthPercent));
    }

    void setPixelMode(int mode) {
        pixelMode = mode;
    }

    // One game pixel drawn 16x16 for the pixel effects; it is scaled onto every cell of the picture.
    private Bitmap pixelTile(int mode) {
        Bitmap tile = Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(tile);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        switch (mode) {
            case PIXEL_ROUND: {
                // The cell darkened except for a rounded square in the middle, so every pixel looks like a lit dot.
                android.graphics.Path frame = new android.graphics.Path();
                frame.setFillType(android.graphics.Path.FillType.EVEN_ODD);
                frame.addRect(0, 0, 16, 16, android.graphics.Path.Direction.CW);
                frame.addRoundRect(new RectF(1, 1, 15, 15), 5, 5, android.graphics.Path.Direction.CW);
                p.setColor(0x8C000000);
                c.drawPath(frame, p);
                break;
            }
            case PIXEL_RGB: {
                // Red, green and blue stripes: each stripe is tinted with its complement, which dims the other two colours.
                p.setColor(0x66007878);
                c.drawRect(0, 0, 5.33f, 16, p);
                p.setColor(0x66780078);
                c.drawRect(5.33f, 0, 10.67f, 16, p);
                p.setColor(0x66787800);
                c.drawRect(10.67f, 0, 16, 16, p);
                break;
            }
            default:
                break;
        }
        return tile;
    }

    private void drawOverlay(Canvas canvas, int gameWidth, int gameHeight) {
        int w = Math.max(1, Math.round(target.width()));
        int h = Math.max(1, Math.round(target.height()));
        boolean horizontal = scanlines;
        boolean vertical = vScanlines;
        int hStrength = scanlineStrength;
        int vStrength = vScanlineStrength;
        int pixels = pixelMode;
        int key = ((((((w * 31 + h) * 31 + gameWidth) * 31 + gameHeight) * 31 + (horizontal ? hStrength : 0)) * 31
                + (vertical ? vStrength : 0)) * 31 + pixels);
        if (scanlineOverlay == null || key != overlayKey) {
            if (scanlineOverlay != null) {
                scanlineOverlay.recycle();
            }
            scanlineOverlay = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(scanlineOverlay);
            // The picture is scaled with nearest-neighbour sampling, so at a non-integer scale (4.5x, say) game pixels are
            // alternately 4 and 5 screen pixels wide. Every effect below is laid out on those same whole-pixel cell edges
            // (the target rectangle is a whole number of pixels, see computeTarget), so lines always fall exactly between game
            // pixels and keep the same width instead of shimmering.
            int[] ex = new int[gameWidth + 1];
            int[] ey = new int[gameHeight + 1];
            for (int i = 0; i <= gameWidth; ++i) {
                ex[i] = Math.round((float) i * w / gameWidth);
            }
            for (int i = 0; i <= gameHeight; ++i) {
                ey[i] = Math.round((float) i * h / gameHeight);
            }
            if (pixels == PIXEL_GRID) {
                // One-pixel dark lines along the right and bottom edge of every game pixel (thicker only on big screens).
                int thick = Math.max(1, Math.round(Math.min((float) w / gameWidth, (float) h / gameHeight) * 0.18f));
                Paint grid = new Paint();
                grid.setColor(0x80000000);
                for (int i = 1; i <= gameWidth; ++i) {
                    c.drawRect(ex[i] - thick, 0, ex[i], h, grid);
                }
                for (int i = 1; i <= gameHeight; ++i) {
                    c.drawRect(0, ey[i] - thick, w, ey[i], grid);
                }
            } else if (pixels != PIXEL_OFF) {
                Bitmap tile = pixelTile(pixels);
                Paint tilePaint = new Paint();
                tilePaint.setFilterBitmap(pixels == PIXEL_ROUND);
                Rect cell = new Rect();
                for (int y = 0; y < gameHeight; ++y) {
                    for (int x = 0; x < gameWidth; ++x) {
                        cell.set(ex[x], ey[y], ex[x + 1], ey[y + 1]);
                        c.drawBitmap(tile, null, cell, tilePaint);
                    }
                }
                tile.recycle();
            }
            Paint line = new Paint();
            if (horizontal) {
                line.setColor(Color.argb(Math.round(hStrength * 2.55f), 0, 0, 0));
                for (int i = 0; i < gameHeight; ++i) {
                    c.drawRect(0, ey[i] + Math.round((ey[i + 1] - ey[i]) * 0.55f), w, ey[i + 1], line);
                }
            }
            if (vertical) {
                line.setColor(Color.argb(Math.round(vStrength * 2.55f), 0, 0, 0));
                for (int i = 0; i < gameWidth; ++i) {
                    c.drawRect(ex[i] + Math.round((ex[i + 1] - ex[i]) * 0.55f), 0, ex[i + 1], h, line);
                }
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
