package io.mgbaldn.gba;

import android.graphics.Bitmap;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Owns the game thread: runs frames, plays their audio (which also paces emulation) and hands pictures to the view. */
final class Emulator {
    interface FrameListener {
        void onFrame(Bitmap bitmap, int width, int height);
    }

    interface StatsListener {
        void onStats(int fps);
    }

    static final int VIDEO_W = 256;
    static final int VIDEO_H = 224;

    /** Held while calling into Native (the game thread holds it for each frame). */
    final Object lock = new Object();

    private final ByteBuffer video = ByteBuffer.allocateDirect(VIDEO_W * VIDEO_H * 4).order(ByteOrder.nativeOrder());
    private final Bitmap bitmap = Bitmap.createBitmap(VIDEO_W, VIDEO_H, Bitmap.Config.ARGB_8888);
    private final FrameListener listener;
    private volatile StatsListener stats;
    private Thread thread;
    private volatile boolean running;
    private volatile boolean paused;
    private volatile boolean loaded;

    Emulator(FrameListener listener) {
        this.listener = listener;
        bitmap.setHasAlpha(false);
    }

    void setStatsListener(StatsListener value) {
        stats = value;
    }

    boolean isLoaded() {
        return loaded;
    }

    boolean load(String path, String savePath) {
        stop();
        boolean ok;
        synchronized (lock) {
            ok = Native.load(path, savePath, video);
        }
        loaded = ok;
        if (ok) {
            start();
        }
        return ok;
    }

    void reset() {
        synchronized (lock) {
            if (loaded) {
                Native.reset();
            }
        }
    }

    void setPaused(boolean value) {
        paused = value;
    }

    private void start() {
        running = true;
        paused = false;
        thread = new Thread(this::loop, "emulator");
        thread.start();
    }

    void stop() {
        running = false;
        Thread t = thread;
        thread = null;
        if (t != null) {
            try {
                t.join(3000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }
        synchronized (lock) {
            if (loaded) {
                Native.unload();
                loaded = false;
            }
        }
    }

    private AudioTrack newTrack(int rate) {
        int minimum = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate).setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                .setBufferSizeInBytes(Math.max(minimum, rate / 60 * 4 * 4))
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();
        track.play();
        return track;
    }

    private void loop() {
        int rate;
        synchronized (lock) {
            rate = Native.sampleRate();
        }
        AudioTrack track = newTrack(rate);
        short[] audio = new short[8192];
        boolean wasPaused = false;
        long windowStart = System.nanoTime();
        int windowFrames = 0;
        while (running) {
            if (paused) {
                if (!wasPaused) {
                    track.pause();
                    track.flush();
                    wasPaused = true;
                }
                sleep(40);
                continue;
            }
            if (wasPaused) {
                track.play();
                wasPaused = false;
            }
            int frames;
            int width;
            int height;
            synchronized (lock) {
                if (!loaded) {
                    break;
                }
                frames = Native.runFrame(audio);
                width = Native.width();
                height = Native.height();
            }
            video.rewind();
            bitmap.copyPixelsFromBuffer(video);
            listener.onFrame(bitmap, width, height);
            ++windowFrames;
            long now = System.nanoTime();
            if (now - windowStart >= 1_000_000_000L) {
                int fps = (int) Math.round(windowFrames * 1e9 / (now - windowStart));
                windowStart = now;
                windowFrames = 0;
                StatsListener listener = stats;
                if (listener != null) {
                    listener.onStats(fps);
                }
                synchronized (lock) {
                    if (loaded) {
                        Native.traceNote("wall=" + System.currentTimeMillis() % 1000000 + "ms fps=" + fps);
                    }
                }
            }
            if (frames > 0) {
                track.write(audio, 0, frames * 2); // blocks: this is the emulation clock
            } else {
                sleep(16);
            }
        }
        track.stop();
        track.release();
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
    }
}
