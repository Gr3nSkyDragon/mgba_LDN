package io.mgbaldn.gba;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;

/** The whole app: the game, a menu button, ROM import, the wireless adapter choice and the ESP32's USB link. */
public class MainActivity extends Activity implements UsbLink.Logger {
    private static final int REQUEST_ROM = 1;
    private static final int REQUEST_SAVE = 2;
    private static final int REQUEST_EXPORT = 3;
    private static final int REQUEST_BACKGROUND = 4;
    private static final String ACTION_PERMISSION = "io.mgbaldn.gba.USB_PERMISSION";
    private static final int[] DEFAULT_COLORS = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    private static final int ADAPTER_OFF = 0;
    private static final int ADAPTER_ESP32 = 1;
    private static final int ADAPTER_CABLE = 2; // the RFU Cable Wrapper over the ESP32 (Ruby/Sapphire with a cable-only link)

    private GameView gameView;
    private Emulator emulator;
    private TextView status;
    private UsbManager usbManager;
    private UsbLink usbLink;
    private SharedPreferences prefs;
    private File traceFile;
    private int adapter = ADAPTER_OFF;
    private volatile int currentFps;
    private boolean showFps = true;
    private boolean showEspStatus = true;
    private File baseDir;
    private File romDir;
    private File saveDir;
    private boolean sharedStorage;
    private String currentRom;
    private boolean controlsShown = true;
    private int controlsOpacity = 35;
    private boolean scanlines;
    private int scanlineStrength = 35;
    private int[] colors = DEFAULT_COLORS.clone();

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (ACTION_PERMISSION.equals(action)) {
                UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false) && device != null) {
                    openUsb(device);
                } else {
                    toast("USB permission was refused; the ESP32 can't be used without it.");
                }
            } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                usbLink.close();
                refreshStatus();
            } else if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
                connectUsb(false);
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        prefs = getSharedPreferences("mgba-ldn", MODE_PRIVATE);
        usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        File external = getExternalFilesDir(null);
        traceFile = new File(external != null ? external : getFilesDir(), "rfu-trace.log");

        setupFolders();

        usbLink = new UsbLink(this);
        Native.setUsbLink(usbLink);
        Native.setTrace(traceFile.getAbsolutePath());

        gameView = new GameView(this);
        emulator = new Emulator(gameView);
        emulator.setStatsListener(fps -> {
            currentFps = fps;
            refreshStatus();
        });

        FrameLayout root = new FrameLayout(this);
        root.addView(gameView, new FrameLayout.LayoutParams(-1, -1));
        TextView menu = new TextView(this);
        menu.setText("☰");
        menu.setTextSize(26);
        menu.setTextColor(0xFFFFFFFF);
        menu.setBackgroundColor(0x66000000);
        menu.setPadding(28, 8, 28, 8);
        menu.setOnClickListener(v -> showMenu());
        FrameLayout.LayoutParams menuParams = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.END);
        menuParams.setMargins(0, 6, 6, 0);
        root.addView(menu, menuParams);
        status = new TextView(this);
        status.setTextColor(0xFFFFFFFF);
        status.setBackgroundColor(0x66000000);
        status.setTextSize(12);
        status.setPadding(12, 4, 12, 4);
        root.addView(status, new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.START));
        setContentView(root);

        IntentFilter filter = new IntentFilter(ACTION_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(usbReceiver, filter);
        }

        adapter = prefs.getInt("adapter", ADAPTER_OFF);
        showFps = prefs.getBoolean("showFps", true);
        showEspStatus = prefs.getBoolean("showEspStatus", true);
        controlsShown = prefs.getBoolean("controls", true);
        controlsOpacity = prefs.getInt("opacity", 35);
        scanlines = prefs.getBoolean("scanlines", false);
        scanlineStrength = prefs.getInt("scanlineStrength", 35);
        colors = loadColors();
        gameView.setControlsVisible(controlsShown);
        gameView.setControlsOpacity(controlsOpacity);
        gameView.setScanlines(scanlines, scanlineStrength);
        gameView.setColors(colors);
        loadBackground();
        String last = prefs.getString("rom", null);
        if (last != null && new File(last).exists()) {
            startGame(last);
        }
        refreshStatus();
        askForFolderAccess();
        if (getIntent() != null && UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(getIntent().getAction())) {
            connectUsb(false);
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(intent.getAction())) {
            connectUsb(false);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        setupFolders();
        emulator.setPaused(false);
        if (adapter != ADAPTER_OFF && !usbLink.isOpen()) {
            connectUsb(false);
        }
    }

    @Override
    protected void onPause() {
        emulator.setPaused(true);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(usbReceiver);
        emulator.stop();
        usbLink.close();
        super.onDestroy();
    }

    // ---- game ----

    private void startGame(String path) {
        File save = activeSaveFor(new File(path));
        if (!emulator.load(path, save.getAbsolutePath())) {
            toast("Couldn't load that ROM.");
            return;
        }
        currentRom = path;
        prefs.edit().putString("rom", path).apply();
        applyAdapter();
        refreshStatus();
    }

    // ---- folders and saves ----
    //
    // <Internal storage>/mGBA/ROMs and /Saves when the app has all-files access; otherwise the same layout inside the
    // app's own folder (Android/data/io.mgbaldn.gba/files/mGBA). A ROM plays the save named like it (<ROM name>.sav)
    // unless another save was imported for it; then that save, and only that one, is read and written.

    private File resolveBase() {
        boolean shared;
        if (Build.VERSION.SDK_INT >= 30) {
            shared = Environment.isExternalStorageManager();
        } else {
            shared = checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
                    == android.content.pm.PackageManager.PERMISSION_GRANTED;
        }
        sharedStorage = shared;
        if (shared) {
            return new File(Environment.getExternalStorageDirectory(), "mGBA");
        }
        File external = getExternalFilesDir(null);
        return new File(external != null ? external : getFilesDir(), "mGBA");
    }

    private void setupFolders() {
        File base = resolveBase();
        File roms = new File(base, "ROMs");
        File saves = new File(base, "Saves");
        roms.mkdirs();
        saves.mkdirs();
        if (!roms.isDirectory() || !saves.isDirectory()) {
            // shared storage refused: fall back to the app's own folder
            sharedStorage = false;
            File external = getExternalFilesDir(null);
            base = new File(external != null ? external : getFilesDir(), "mGBA");
            roms = new File(base, "ROMs");
            saves = new File(base, "Saves");
            roms.mkdirs();
            saves.mkdirs();
        }
        baseDir = base;
        String previous = prefs.getString("folderBase", null);
        romDir = roms;
        saveDir = saves;
        if (!base.getAbsolutePath().equals(previous)) {
            // First run with this folder: bring over what earlier versions or the other location kept.
            File external = getExternalFilesDir(null);
            moveFiles(new File(getFilesDir(), "roms"), roms);
            moveFiles(new File(external != null ? external : getFilesDir(), "saves"), saves);
            if (previous != null) {
                moveFiles(new File(previous, "ROMs"), roms);
                moveFiles(new File(previous, "Saves"), saves);
            }
            String last = prefs.getString("rom", null);
            if (last != null && !new File(last).exists()) {
                File moved = new File(roms, new File(last).getName());
                if (moved.exists()) {
                    prefs.edit().putString("rom", moved.getAbsolutePath()).apply();
                    if (currentRom != null && last.equals(currentRom)) {
                        currentRom = moved.getAbsolutePath();
                    }
                }
            }
            prefs.edit().putString("folderBase", base.getAbsolutePath()).apply();
        }
    }

    private void moveFiles(File from, File to) {
        File[] files = from.listFiles();
        if (files == null || from.equals(to)) {
            return;
        }
        for (File file : files) {
            if (!file.isFile()) {
                continue;
            }
            File target = new File(to, file.getName());
            if (target.exists()) {
                continue;
            }
            try (InputStream in = new FileInputStream(file); OutputStream out = new FileOutputStream(target)) {
                byte[] buffer = new byte[1 << 16];
                int n;
                while ((n = in.read(buffer)) > 0) {
                    out.write(buffer, 0, n);
                }
                file.delete();
            } catch (java.io.IOException e) {
                target.delete();
            }
        }
    }

    private void askForFolderAccess() {
        if (sharedStorage || prefs.getBoolean("askedFolderAccess", false)) {
            return;
        }
        prefs.edit().putBoolean("askedFolderAccess", true).apply();
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle("Storage access")
                .setMessage("To keep your ROMs and saves in a folder you can see and copy files to (Internal storage > mGBA), "
                        + "mGBA needs access to all files.\n\nYou can skip this; the app then uses its own private folder.")
                .setPositiveButton("Grant access", (d, w) -> grantFolderAccess())
                .setNegativeButton("Skip", null).show();
    }

    private void grantFolderAccess() {
        if (Build.VERSION.SDK_INT >= 30) {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            try {
                startActivity(intent);
            } catch (RuntimeException e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] {android.Manifest.permission.WRITE_EXTERNAL_STORAGE,
                    android.Manifest.permission.READ_EXTERNAL_STORAGE}, 10);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        setupFolders();
    }

    private static String sanitize(String name) {
        return name.replaceAll("[^A-Za-z0-9._ ()-]", "_");
    }

    private File activeSaveFor(File rom) {
        String chosen = prefs.getString("save:" + rom.getName(), null);
        if (chosen != null && new File(saveDir, chosen).exists()) {
            return new File(saveDir, chosen);
        }
        return new File(saveDir, baseName(rom.getName()) + ".sav");
    }

    private void applyAdapter() {
        if (!emulator.isLoaded()) {
            return;
        }
        synchronized (emulator.lock) {
            Native.setAdapter(adapter);
        }
    }

    private void refreshStatus() {
        String text;
        if (!emulator.isLoaded()) {
            text = "No game loaded. Use the menu to open a ROM.";
        } else {
            StringBuilder parts = new StringBuilder();
            if (showFps) {
                parts.append(currentFps).append(" FPS");
            }
            if (showEspStatus && adapter != ADAPTER_OFF) {
                if (parts.length() > 0) {
                    parts.append("  |  ");
                }
                parts.append(usbLink.isOpen() ? "ESP32 connected" : "ESP32 not connected (plug it into the USB port)");
            }
            text = parts.toString();
        }
        final String shown = text;
        runOnUiThread(() -> {
            status.setText(shown);
            status.setVisibility(shown.isEmpty() ? View.GONE : View.VISIBLE);
        });
    }

    // ---- menu ----

    private void showMenu() {
        String[] items = {"Open ROM", "Import save", "Export save", "Reset", "Wireless adapter", "Connect ESP32", "Display settings",
                "Save diagnostic log", "About"};
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setItems(items, (dialog, which) -> {
            switch (which) {
                case 0:
                    openFilePicker(REQUEST_ROM, true);
                    break;
                case 1:
                    if (currentRom == null) {
                        toast("Open a game first.");
                    } else {
                        openFilePicker(REQUEST_SAVE, false);
                    }
                    break;
                case 2:
                    exportSave();
                    break;
                case 3:
                    emulator.reset();
                    break;
                case 4:
                    chooseAdapter();
                    break;
                case 5:
                    connectUsb(true);
                    break;
                case 6:
                    displaySettings();
                    break;
                case 7:
                    shareLog();
                    break;
                default:
                    about();
            }
        }).show();
    }

    // ---- display settings ----

    private static final String[] PRESET_NAMES = {"White (default)", "Black", "GBA Indigo", "GBC Teal", "GBC Berry", "GBC Dandelion",
            "GBC Kiwi", "GBC Grape"};
    private static final int[] PRESET_COLORS = {0xFFFFFFFF, 0xFF000000, 0xFF6A4FC9, 0xFF2BB5B0, 0xFFC2417D, 0xFFF2C230, 0xFF9BD34B,
            0xFF8A4FB3};

    private int[] loadColors() {
        String saved = prefs.getString("colors", null);
        int[] result = DEFAULT_COLORS.clone();
        if (saved != null) {
            String[] parts = saved.split(",");
            for (int i = 0; i < result.length && i < parts.length; ++i) {
                try {
                    result[i] = 0xFF000000 | (int) Long.parseLong(parts[i].trim());
                } catch (NumberFormatException ignored) {
                    // keep the default for this button
                }
            }
        }
        return result;
    }

    private void saveColors() {
        StringBuilder text = new StringBuilder();
        for (int i = 0; i < colors.length; ++i) {
            if (i > 0) {
                text.append(',');
            }
            text.append(colors[i] & 0xFFFFFF);
        }
        prefs.edit().putString("colors", text.toString()).apply();
        gameView.setColors(colors);
    }

    private android.widget.CheckBox checkBox(String label, boolean checked, android.widget.CompoundButton.OnCheckedChangeListener listener) {
        android.widget.CheckBox box = new android.widget.CheckBox(this);
        box.setText(label);
        box.setChecked(checked);
        box.setOnCheckedChangeListener(listener);
        return box;
    }

    private void addSlider(android.widget.LinearLayout column, String label, int min, int max, int value,
            java.util.function.IntConsumer onChange) {
        final TextView title = new TextView(this);
        title.setText(label + ": " + value + "%");
        title.setPadding(0, 24, 0, 0);
        column.addView(title);
        android.widget.SeekBar bar = new android.widget.SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(value - min);
        bar.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(android.widget.SeekBar seekBar, int progress, boolean fromUser) {
                title.setText(label + ": " + (progress + min) + "%");
                if (fromUser) {
                    onChange.accept(progress + min);
                }
            }

            @Override
            public void onStartTrackingTouch(android.widget.SeekBar seekBar) {}

            @Override
            public void onStopTrackingTouch(android.widget.SeekBar seekBar) {}
        });
        column.addView(bar);
    }

    private void displaySettings() {
        android.widget.LinearLayout column = new android.widget.LinearLayout(this);
        column.setOrientation(android.widget.LinearLayout.VERTICAL);
        column.setPadding(48, 16, 48, 0);

        column.addView(checkBox("FPS counter", showFps, (v, on) -> {
            showFps = on;
            prefs.edit().putBoolean("showFps", on).apply();
            refreshStatus();
        }));
        column.addView(checkBox("ESP32 status", showEspStatus, (v, on) -> {
            showEspStatus = on;
            prefs.edit().putBoolean("showEspStatus", on).apply();
            refreshStatus();
        }));
        column.addView(checkBox("On-screen controls", controlsShown, (v, on) -> {
            controlsShown = on;
            gameView.setControlsVisible(on);
            prefs.edit().putBoolean("controls", on).apply();
        }));
        addSlider(column, "Control opacity", 5, 100, controlsOpacity, value -> {
            controlsOpacity = value;
            gameView.setControlsOpacity(value);
            prefs.edit().putInt("opacity", value).apply();
        });
        android.widget.Button colorsButton = new android.widget.Button(this);
        colorsButton.setText("Button colors");
        colorsButton.setOnClickListener(v -> chooseColors());
        column.addView(colorsButton);
        android.widget.LinearLayout backgroundRow = new android.widget.LinearLayout(this);
        android.widget.Button pick = new android.widget.Button(this);
        pick.setText("Background photo");
        pick.setOnClickListener(v -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("image/*");
            startActivityForResult(intent, REQUEST_BACKGROUND);
        });
        android.widget.Button clear = new android.widget.Button(this);
        clear.setText("Remove");
        clear.setOnClickListener(v -> {
            new File(getFilesDir(), "background.png").delete();
            gameView.setBackgroundImage(null);
            toast("Background removed");
        });
        backgroundRow.addView(pick, new android.widget.LinearLayout.LayoutParams(0, -2, 2f));
        backgroundRow.addView(clear, new android.widget.LinearLayout.LayoutParams(0, -2, 1f));
        column.addView(backgroundRow);
        TextView hint = new TextView(this);
        hint.setText("The photo fills the area behind the buttons when the phone is held upright.");
        hint.setTextSize(12);
        column.addView(hint);
        column.addView(checkBox("Scanlines", scanlines, (v, on) -> {
            scanlines = on;
            gameView.setScanlines(on, scanlineStrength);
            prefs.edit().putBoolean("scanlines", on).apply();
        }));
        addSlider(column, "Scanline strength", 5, 100, scanlineStrength, value -> {
            scanlineStrength = value;
            gameView.setScanlines(scanlines, value);
            prefs.edit().putInt("scanlineStrength", value).apply();
        });

        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.addView(column);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle("Display settings")
                .setView(scroll).setPositiveButton("Done", null).show();
    }

    private static String hex(int color) {
        return String.format("#%06X", color & 0xFFFFFF);
    }

    private void chooseColors() {
        String[] items = new String[PRESET_NAMES.length + 1];
        System.arraycopy(PRESET_NAMES, 0, items, 0, PRESET_NAMES.length);
        items[PRESET_NAMES.length] = "Each button separately";
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle("Button colors")
                .setItems(items, (dialog, which) -> {
                    if (which < PRESET_NAMES.length) {
                        java.util.Arrays.fill(colors, PRESET_COLORS[which]);
                        saveColors();
                    } else {
                        chooseButtonColor();
                    }
                }).show();
    }

    private void chooseButtonColor() {
        String[] items = new String[GameView.COLOR_NAMES.length];
        for (int i = 0; i < items.length; ++i) {
            items[i] = GameView.COLOR_NAMES[i] + "   " + hex(colors[i]);
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle("Choose a button")
                .setItems(items, (dialog, which) -> editColor(which)).setNegativeButton("Back", null).show();
    }

    private void editColor(int index) {
        android.widget.EditText input = new android.widget.EditText(this);
        input.setText(hex(colors[index]));
        input.setSingleLine(true);
        input.setSelectAllOnFocus(true);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(GameView.COLOR_NAMES[index] + " color (hex, like #FF8800)").setView(input)
                .setPositiveButton("OK", (d, w) -> {
                    String text = input.getText().toString().trim();
                    if (text.startsWith("#")) {
                        text = text.substring(1);
                    }
                    try {
                        if (text.length() != 6) {
                            throw new NumberFormatException();
                        }
                        colors[index] = 0xFF000000 | Integer.parseInt(text, 16);
                        saveColors();
                    } catch (NumberFormatException e) {
                        toast("Use six hex digits, like FF8800");
                    }
                    chooseButtonColor();
                }).setNegativeButton("Cancel", (d, w) -> chooseButtonColor()).show();
    }

    // The background photo is downsized and kept in the app's own storage (background.png), so it survives the original
    // being moved or deleted.
    private void importBackground(Uri uri) {
        new Thread(() -> {
            try {
                BitmapFactory.Options bounds = new BitmapFactory.Options();
                bounds.inJustDecodeBounds = true;
                try (InputStream in = getContentResolver().openInputStream(uri)) {
                    BitmapFactory.decodeStream(in, null, bounds);
                }
                int sample = 1;
                while (bounds.outWidth / sample > 2200 || bounds.outHeight / sample > 2200) {
                    sample *= 2;
                }
                BitmapFactory.Options options = new BitmapFactory.Options();
                options.inSampleSize = sample;
                Bitmap image;
                try (InputStream in = getContentResolver().openInputStream(uri)) {
                    image = BitmapFactory.decodeStream(in, null, options);
                }
                if (image == null) {
                    toast("Couldn't read that picture");
                    return;
                }
                try (OutputStream out = new FileOutputStream(new File(getFilesDir(), "background.png"))) {
                    image.compress(Bitmap.CompressFormat.PNG, 100, out);
                }
                Bitmap chosen = image;
                runOnUiThread(() -> gameView.setBackgroundImage(chosen));
                toast("Background set");
            } catch (Exception e) {
                toast("Couldn't use that picture: " + e.getMessage());
            }
        }, "background").start();
    }

    private void loadBackground() {
        File file = new File(getFilesDir(), "background.png");
        if (!file.exists()) {
            return;
        }
        new Thread(() -> {
            Bitmap image = BitmapFactory.decodeFile(file.getAbsolutePath());
            if (image != null) {
                runOnUiThread(() -> gameView.setBackgroundImage(image));
            }
        }, "background-load").start();
    }

    private void chooseAdapter() {
        String[] names = {"Off", "ESP32 (GB-Link Switch LDN board, USB)", "Cable adapter (Ruby/Sapphire, ESP32)"};
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle("Wireless adapter")
                .setSingleChoiceItems(names, adapter, (dialog, which) -> {
                    adapter = which;
                    prefs.edit().putInt("adapter", adapter).apply();
                    applyAdapter();
                    if (adapter != ADAPTER_OFF) {
                        connectUsb(true);
                    }
                    refreshStatus();
                    dialog.dismiss();
                }).show();
    }

    private void about() {
        String game = "";
        if (currentRom != null) {
            game = "\n\nGame: " + new File(currentRom).getName() + "\nSave: " + activeSaveFor(new File(currentRom)).getName();
        }
        AlertDialog.Builder builder = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("mGBA LDN for Android")
                .setMessage("mGBA with Wireless Adapter support, for trading Generation 3 games with a Nintendo Switch through "
                        + "an ESP32 using GB-Link Switch LDN firmware.\n\nThe Switch must host the trade (Wireless Club > Direct Corner); do not use "
                        + "fast-forward. Not affiliated with or endorsed by mGBA.\n\nROMs and saves are kept in:\n"
                        + baseDir.getAbsolutePath() + "\n(ROMs and Saves folders)" + game)
                .setPositiveButton("OK", null);
        if (!sharedStorage) {
            builder.setNeutralButton("Storage access", (d, w) -> grantFolderAccess());
        }
        builder.show();
    }

    // ---- ROM import ----

    private void openFilePicker(int request, boolean multiple) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        if (multiple) {
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }
        startActivityForResult(intent, request);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null) {
            return;
        }
        if (requestCode == REQUEST_ROM || requestCode == REQUEST_SAVE) {
            java.util.List<Uri> uris = new java.util.ArrayList<>();
            if (data.getClipData() != null) {
                for (int i = 0; i < data.getClipData().getItemCount(); ++i) {
                    uris.add(data.getClipData().getItemAt(i).getUri());
                }
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
            importFiles(uris, requestCode == REQUEST_SAVE);
        } else if (requestCode == REQUEST_EXPORT && data.getData() != null) {
            writeSaveTo(data.getData());
        } else if (requestCode == REQUEST_BACKGROUND && data.getData() != null) {
            importBackground(data.getData());
        }
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) {
                    return cursor.getString(index);
                }
            }
        } catch (RuntimeException ignored) {
            // fall through to a generic name
        }
        return "game.gba";
    }

    private static boolean isSaveName(String name) {
        String lower = name.toLowerCase(java.util.Locale.ROOT);
        return lower.endsWith(".sav") || lower.endsWith(".srm") || lower.endsWith(".sa1") || lower.endsWith(".fla")
                || lower.endsWith(".eep");
    }

    private static String baseName(String name) {
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    // Picked files are first read completely into a temporary file and only then placed at their destination. Copying
    // straight to the destination emptied a file that was picked from the mGBA folder itself (the destination and the source
    // are then the same file: opening it for writing truncates it before a single byte has been read), and for a save the
    // "keep the old one as .bak" step moved the source away before it was read.
    private File stage(Uri from) throws java.io.IOException {
        File temp = File.createTempFile("import", ".tmp", getCacheDir());
        try (InputStream in = getContentResolver().openInputStream(from); OutputStream out = new FileOutputStream(temp)) {
            byte[] buffer = new byte[1 << 16];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
            }
        } catch (java.io.IOException | RuntimeException e) {
            temp.delete();
            throw e;
        }
        if (temp.length() == 0) {
            temp.delete();
            throw new java.io.IOException("the picked file is empty");
        }
        return temp;
    }

    private static boolean sameContent(File a, File b) throws java.io.IOException {
        if (a.length() != b.length()) {
            return false;
        }
        try (InputStream x = new FileInputStream(a); InputStream y = new FileInputStream(b)) {
            byte[] bx = new byte[1 << 16];
            byte[] by = new byte[1 << 16];
            int n;
            while ((n = x.read(bx)) > 0) {
                int got = 0;
                while (got < n) {
                    int m = y.read(by, got, n - got);
                    if (m <= 0) {
                        return false;
                    }
                    got += m;
                }
                for (int i = 0; i < n; ++i) {
                    if (bx[i] != by[i]) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    /** Puts a staged file at {@code target} (replacing what is there) and removes the staging file. */
    private static void place(File staged, File target) throws java.io.IOException {
        try {
            java.nio.file.Files.copy(staged.toPath(), target.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        } finally {
            staged.delete();
        }
    }

    // The core wants real file paths, so a ROM is copied into <mGBA>/ROMs and a save into <mGBA>/Saves under its own name.
    // A save picked together with a ROM (or on its own, for the running game) becomes THE save for that ROM: it is the only
    // file the game reads and writes until another is imported. A file that would be overwritten is kept as <name>.bak.
    private void importFiles(java.util.List<Uri> uris, boolean saveOnly) {
        toast("Importing");
        new Thread(() -> {
            try {
                java.util.List<File> roms = new java.util.ArrayList<>();
                java.util.List<File> saves = new java.util.ArrayList<>();
                java.util.List<Uri> saveUris = new java.util.ArrayList<>();
                for (Uri uri : uris) {
                    String name = sanitize(displayName(uri));
                    if (isSaveName(name)) {
                        saveUris.add(uri);
                    } else if (!saveOnly) {
                        File target = new File(romDir, name);
                        place(stage(uri), target);
                        roms.add(target);
                    }
                }
                File toLoad = roms.isEmpty() ? (currentRom != null ? new File(currentRom) : null) : roms.get(roms.size() - 1);
                if (!saveUris.isEmpty()) {
                    if (toLoad == null) {
                        toast("Pick a ROM together with its save, or open a game first.");
                        return;
                    }
                    emulator.stop(); // release the running game's save before replacing anything
                    for (Uri uri : saveUris) {
                        File target = new File(saveDir, sanitize(displayName(uri)));
                        File staged = stage(uri); // fully read first: the source may be the very file being replaced
                        if (target.exists() && !sameContent(staged, target)) {
                            // Keep what is being replaced. (Re-importing a save that is already in the folder replaces it
                            // with identical bytes, so no backup is made in that case.)
                            File backup = new File(saveDir, target.getName() + ".bak");
                            java.nio.file.Files.copy(target.toPath(), backup.toPath(),
                                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                        }
                        place(staged, target);
                        saves.add(target);
                    }
                    for (File save : saves) {
                        File owner = toLoad;
                        for (File rom : roms) {
                            if (baseName(rom.getName()).equalsIgnoreCase(baseName(save.getName()))) {
                                owner = rom;
                            }
                        }
                        prefs.edit().putString("save:" + owner.getName(), save.getName()).apply();
                    }
                    toast(saves.size() == 1 ? "Save imported" : "Saves imported");
                }
                if (toLoad != null) {
                    File loadFile = toLoad;
                    runOnUiThread(() -> startGame(loadFile.getAbsolutePath()));
                }
            } catch (Exception e) {
                toast("Import failed: " + e.getMessage());
            }
        }, "import").start();
    }

    private File currentSaveFile() {
        return currentRom == null ? null : activeSaveFor(new File(currentRom));
    }

    private void exportSave() {
        File save = currentSaveFile();
        if (save == null || !save.exists()) {
            toast("This game has no save file yet.");
            return;
        }
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/octet-stream");
        intent.putExtra(Intent.EXTRA_TITLE, save.getName());
        startActivityForResult(intent, REQUEST_EXPORT);
    }

    private void writeSaveTo(Uri uri) {
        File save = currentSaveFile();
        if (save == null) {
            return;
        }
        try (InputStream in = new FileInputStream(save); OutputStream out = getContentResolver().openOutputStream(uri)) {
            byte[] buffer = new byte[1 << 16];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
            }
            toast("Save exported");
        } catch (Exception e) {
            toast("Export failed: " + e.getMessage());
        }
    }

    // ---- ESP32 over USB ----

    private void connectUsb(boolean verbose) {
        UsbDevice device = UsbLink.find(usbManager);
        if (device == null) {
            if (verbose) {
                toast("No ESP32 found. Plug the board's native USB port into the phone.");
            }
            refreshStatus();
            return;
        }
        if (usbLink.isOpen()) {
            return;
        }
        if (usbManager.hasPermission(device)) {
            openUsb(device);
            return;
        }
        int flags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
        Intent intent = new Intent(ACTION_PERMISSION).setPackage(getPackageName());
        usbManager.requestPermission(device, PendingIntent.getBroadcast(this, 0, intent, flags));
    }

    private void openUsb(UsbDevice device) {
        if (usbLink.open(usbManager, device)) {
            toast("ESP32 connected");
        }
        refreshStatus();
    }

    @Override
    public void log(String line) {
        android.util.Log.i("mgba-ldn", line);
        toast(line);
    }

    // ---- diagnostics ----

    // The whole trace goes to the Downloads folder as a text file (a big trace can't travel inside a share intent, which
    // is why the old button appeared to do nothing); the last part is offered to the share sheet as well.
    private void shareLog() {
        if (!traceFile.exists() || traceFile.length() == 0) {
            toast("No adapter trace yet. Turn the wireless adapter on and play with it first.");
            return;
        }
        new Thread(() -> {
            String tail = "";
            try (RandomAccessFile file = new RandomAccessFile(traceFile, "r")) {
                long length = file.length();
                long keep = Math.min(length, 40_000);
                byte[] bytes = new byte[(int) keep];
                file.seek(length - keep);
                file.readFully(bytes);
                tail = new String(bytes, "UTF-8");
            } catch (Exception ignored) {
                // the file copy below is the important part
            }
            String where;
            try {
                where = saveToDownloads(traceFile, "mgba-ldn-trace-");
                File backend = new File(traceFile.getPath() + ".backend");
                if (backend.exists() && backend.length() > 0) {
                    where += " and " + saveToDownloads(backend, "mgba-ldn-trace-backend-");
                }
            } catch (Exception e) {
                where = null;
                toast("Couldn't write the log to Downloads: " + e.getMessage());
            }
            if (where != null) {
                toast("Full log saved: " + where);
            }
            final String shown = tail;
            runOnUiThread(() -> {
                try {
                    Intent send = new Intent(Intent.ACTION_SEND);
                    send.setType("text/plain");
                    send.putExtra(Intent.EXTRA_SUBJECT, "mGBA LDN adapter trace (end of log)");
                    send.putExtra(Intent.EXTRA_TEXT, shown);
                    startActivity(Intent.createChooser(send, "Share the end of the log"));
                } catch (RuntimeException e) {
                    toast("Couldn't open the share sheet: " + e.getMessage());
                }
            });
        }, "log-export").start();
    }

    private String saveToDownloads(File source, String prefix) throws java.io.IOException {
        String name = prefix + new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
                .format(new java.util.Date()) + ".txt";
        if (Build.VERSION.SDK_INT >= 29) {
            android.content.ContentValues values = new android.content.ContentValues();
            values.put(android.provider.MediaStore.Downloads.DISPLAY_NAME, name);
            values.put(android.provider.MediaStore.Downloads.MIME_TYPE, "text/plain");
            values.put(android.provider.MediaStore.Downloads.RELATIVE_PATH, android.os.Environment.DIRECTORY_DOWNLOADS);
            Uri uri = getContentResolver().insert(android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
            if (uri == null) {
                throw new java.io.IOException("the system refused to create the file");
            }
            try (InputStream in = new FileInputStream(source); OutputStream out = getContentResolver().openOutputStream(uri)) {
                byte[] buffer = new byte[1 << 16];
                int n;
                while ((n = in.read(buffer)) > 0) {
                    out.write(buffer, 0, n);
                }
            }
            return "Downloads/" + name;
        }
        File target = new File(source.getParentFile(), name);
        try (InputStream in = new FileInputStream(source); OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[1 << 16];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
            }
        }
        return target.getAbsolutePath();
    }

    private void toast(String message) {
        runOnUiThread(() -> Toast.makeText(this, message, Toast.LENGTH_SHORT).show());
    }

    // ---- physical keys ----

    private int keyBit(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_DPAD_UP: return GameView.KEY_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return GameView.KEY_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return GameView.KEY_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return GameView.KEY_RIGHT;
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_X: return GameView.KEY_A;
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_Z: return GameView.KEY_B;
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_Q: return GameView.KEY_L;
            case KeyEvent.KEYCODE_BUTTON_R1:
            case KeyEvent.KEYCODE_E: return GameView.KEY_R;
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_ENTER: return GameView.KEY_START;
            case KeyEvent.KEYCODE_BUTTON_SELECT:
            case KeyEvent.KEYCODE_DEL: return GameView.KEY_SELECT;
            default: return 0;
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        int bit = keyBit(keyCode);
        if (bit != 0) {
            gameView.setOtherKeys(gameView.otherKeys() | bit);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        int bit = keyBit(keyCode);
        if (bit != 0) {
            gameView.setOtherKeys(gameView.otherKeys() & ~bit);
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }
}
