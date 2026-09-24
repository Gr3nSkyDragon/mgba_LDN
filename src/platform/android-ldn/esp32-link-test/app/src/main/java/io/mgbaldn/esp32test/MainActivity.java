package io.mgbaldn.esp32test;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/** One screen: Run test / Stop / Copy log / Share log, and a live log. */
public class MainActivity extends Activity implements UsbLink.Logger {
    private static final String ACTION_PERMISSION = "io.mgbaldn.esp32test.USB_PERMISSION";
    private static final int MAX_LOG_CHARS = 200_000;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final StringBuilder logText = new StringBuilder();
    private final SimpleDateFormat clock = new SimpleDateFormat("HH:mm:ss.SSS", Locale.US);
    private TextView logView;
    private ScrollView scroll;
    private Button runButton;
    private UsbManager usbManager;
    private UsbLink link;
    private volatile boolean running;
    private File logFile;

    private final BroadcastReceiver permissionReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false) && device != null) {
                log("USB permission granted.");
                startSession(device);
            } else {
                log("FAIL: USB permission was refused. Tap Run test again and choose OK.");
                setRunning(false);
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        link = new UsbLink(this);
        File dir = getExternalFilesDir(null);
        logFile = new File(dir != null ? dir : getFilesDir(), "esp32-test.log");
        logFile.delete();

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (8 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        TextView help = new TextView(this);
        help.setText("Plug the ESP32-S3's USB port (the native one) into the phone, put the Switch in Wireless Club > "
                + "Direct Corner, then tap Run test.");
        root.addView(help);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        runButton = button("Run test", v -> runTest());
        row.addView(runButton, weight());
        row.addView(button("Stop", v -> NativeProbe.stop()), weight());
        row.addView(button("Copy", v -> copyLog()), weight());
        row.addView(button("Share", v -> shareLog()), weight());
        root.addView(row);

        logView = new TextView(this);
        logView.setTypeface(Typeface.MONOSPACE);
        logView.setTextSize(11);
        logView.setTextIsSelectable(true);
        scroll = new ScrollView(this);
        scroll.addView(logView);
        root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        setContentView(root);

        IntentFilter filter = new IntentFilter(ACTION_PERMISSION);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(permissionReceiver, filter);
        }
        log("ESP32 link test " + BuildConfigInfo.VERSION + " on " + Build.MANUFACTURER + " " + Build.MODEL + ", Android "
                + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + "), ABI " + Build.SUPPORTED_ABIS[0]);
        log("Log file: " + logFile.getAbsolutePath());
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(permissionReceiver);
        NativeProbe.stop();
        link.close();
        super.onDestroy();
    }

    private Button button(String text, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(text);
        b.setOnClickListener(listener);
        return b;
    }

    private LinearLayout.LayoutParams weight() {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
    }

    private void setRunning(boolean value) {
        running = value;
        ui.post(() -> runButton.setEnabled(!value));
    }

    private void runTest() {
        if (running) {
            return;
        }
        setRunning(true);
        UsbDevice device = UsbLink.find(usbManager);
        if (device == null) {
            log("FAIL: no ESP32 found on USB (Espressif 303A:1001). Devices seen: " + usbManager.getDeviceList().size());
            for (UsbDevice d : usbManager.getDeviceList().values()) {
                log("  - " + String.format("%04X:%04X", d.getVendorId(), d.getProductId()) + " " + d.getProductName());
            }
            setRunning(false);
            return;
        }
        if (usbManager.hasPermission(device)) {
            startSession(device);
            return;
        }
        log("Requesting USB permission for " + device.getDeviceName() + "...");
        int flags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
        Intent intent = new Intent(ACTION_PERMISSION).setPackage(getPackageName());
        usbManager.requestPermission(device, PendingIntent.getBroadcast(this, 0, intent, flags));
    }

    private void startSession(UsbDevice device) {
        Thread thread = new Thread(() -> {
            try {
                if (link.open(usbManager, device)) {
                    int rc = NativeProbe.run(link, 60);
                    log("=== finished, result code " + rc + (rc == 0 ? " (all good)" : ""));
                }
            } catch (Throwable t) {
                log("ERROR: " + t);
            } finally {
                link.close();
                setRunning(false);
            }
        }, "esp32-test");
        thread.start();
    }

    @Override
    public void log(String line) {
        final String stamped = clock.format(new Date()) + "  " + line + "\n";
        android.util.Log.i("esp32test", line);
        synchronized (logText) {
            logText.append(stamped);
            if (logText.length() > MAX_LOG_CHARS) {
                logText.delete(0, logText.length() - MAX_LOG_CHARS);
            }
            try (FileOutputStream out = new FileOutputStream(logFile, true)) {
                out.write(stamped.getBytes("UTF-8"));
            } catch (IOException ignored) {
                // the on-screen log still works
            }
        }
        ui.post(() -> {
            logView.append(stamped);
            scroll.post(() -> scroll.fullScroll(View.FOCUS_DOWN));
        });
    }

    private String allText() {
        synchronized (logText) {
            return logText.toString();
        }
    }

    private void copyLog() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("esp32-test log", allText()));
        Toast.makeText(this, "Log copied", Toast.LENGTH_SHORT).show();
    }

    private void shareLog() {
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_SUBJECT, "ESP32 link test log");
        send.putExtra(Intent.EXTRA_TEXT, allText());
        startActivity(Intent.createChooser(send, "Share log"));
    }
}
