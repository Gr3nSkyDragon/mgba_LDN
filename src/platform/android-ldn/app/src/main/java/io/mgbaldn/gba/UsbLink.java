package io.mgbaldn.gba;

import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;

/**
 * The Android half of the Esp32SerialOps seam: a CDC-ACM serial link to the ESP32's native USB Serial/JTAG port. The
 * native ESP32 backend calls read/write/present from its own I/O thread (proven on hardware with the esp32-test app).
 */
final class UsbLink {
    interface Logger {
        void log(String line);
    }

    static final int VID_ESPRESSIF = 0x303A;
    static final int PID_USB_JTAG = 0x1001;

    private final Logger logger;
    private UsbDeviceConnection connection;
    private UsbInterface controlInterface;
    private UsbInterface dataInterface;
    private UsbEndpoint in;
    private UsbEndpoint out;
    private volatile boolean open;

    // Received bytes, filled by a reader thread that sits in a blocking bulk read. Polling with a short timeout from the
    // emulator's adapter thread added up to ~10 ms to every reply from the board (about a whole game frame of extra lag on
    // each hop of the link), which the Switch side is sensitive to.
    private final java.util.ArrayDeque<byte[]> received = new java.util.ArrayDeque<>();
    private byte[] pending;
    private int pendingOffset;
    private Thread reader;

    UsbLink(Logger logger) {
        this.logger = logger;
    }

    static UsbDevice find(UsbManager manager) {
        for (UsbDevice device : manager.getDeviceList().values()) {
            if (device.getVendorId() == VID_ESPRESSIF && device.getProductId() == PID_USB_JTAG) {
                return device;
            }
        }
        return null;
    }

    /** Claims the CDC interfaces and sets 921600 8N1 with DTR asserted and RTS low, like the Windows backend. */
    synchronized boolean open(UsbManager manager, UsbDevice device) {
        close();
        UsbInterface comm = null;
        UsbInterface data = null;
        for (int i = 0; i < device.getInterfaceCount(); ++i) {
            UsbInterface intf = device.getInterface(i);
            if (intf.getInterfaceClass() == UsbConstants.USB_CLASS_COMM && comm == null) {
                comm = intf;
            } else if (intf.getInterfaceClass() == UsbConstants.USB_CLASS_CDC_DATA && data == null) {
                data = intf;
            }
        }
        if (data == null) {
            logger.log("ESP32: the device has no CDC data interface");
            return false;
        }
        UsbEndpoint inEp = null;
        UsbEndpoint outEp = null;
        for (int i = 0; i < data.getEndpointCount(); ++i) {
            UsbEndpoint ep = data.getEndpoint(i);
            if (ep.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) {
                continue;
            }
            if (ep.getDirection() == UsbConstants.USB_DIR_IN) {
                inEp = ep;
            } else {
                outEp = ep;
            }
        }
        if (inEp == null || outEp == null) {
            logger.log("ESP32: the CDC data interface has no bulk endpoints");
            return false;
        }
        UsbDeviceConnection conn = manager.openDevice(device);
        if (conn == null) {
            logger.log("ESP32: could not open the USB device (permission missing?)");
            return false;
        }
        if (comm != null && !conn.claimInterface(comm, true)) {
            logger.log("ESP32: warning, could not claim the CDC control interface");
        }
        if (!conn.claimInterface(data, true)) {
            logger.log("ESP32: could not claim the CDC data interface");
            conn.close();
            return false;
        }
        int commIndex = comm != null ? comm.getId() : 0;
        byte[] coding = {0x00, 0x10, 0x0E, 0x00, 0x00, 0x00, 0x08}; // SET_LINE_CODING 921600 8N1
        conn.controlTransfer(0x21, 0x20, 0, commIndex, coding, coding.length, 500);
        conn.controlTransfer(0x21, 0x22, 0x01, commIndex, null, 0, 500); // SET_CONTROL_LINE_STATE: DTR on, RTS off

        connection = conn;
        controlInterface = comm;
        dataInterface = data;
        in = inEp;
        out = outEp;
        open = true;
        synchronized (received) {
            received.clear();
        }
        pending = null;
        final UsbDeviceConnection readerConnection = conn;
        final UsbEndpoint readerEndpoint = inEp;
        reader = new Thread(() -> {
            byte[] buffer = new byte[512];
            while (open && connection == readerConnection) {
                int got = readerConnection.bulkTransfer(readerEndpoint, buffer, buffer.length, 100);
                if (got > 0) {
                    byte[] copy = java.util.Arrays.copyOf(buffer, got);
                    synchronized (received) {
                        received.add(copy);
                    }
                }
            }
        }, "usb-reader");
        reader.setDaemon(true);
        reader.start();
        return true;
    }

    synchronized void close() {
        open = false;
        UsbDeviceConnection conn = connection;
        connection = null;
        if (conn != null) {
            if (dataInterface != null) {
                conn.releaseInterface(dataInterface);
            }
            if (controlInterface != null) {
                conn.releaseInterface(controlInterface);
            }
            conn.close();
        }
    }

    boolean isOpen() {
        return open;
    }

    // ---- called from native ----

    /** Non-blocking read of what the reader thread has collected: bytes read, 0 if none, -1 if the link is closed. */
    public int read(byte[] buffer) {
        if (!open) {
            return -1;
        }
        int filled = 0;
        while (filled < buffer.length) {
            if (pending == null) {
                synchronized (received) {
                    pending = received.poll();
                }
                pendingOffset = 0;
                if (pending == null) {
                    break;
                }
            }
            int n = Math.min(buffer.length - filled, pending.length - pendingOffset);
            System.arraycopy(pending, pendingOffset, buffer, filled, n);
            filled += n;
            pendingOffset += n;
            if (pendingOffset >= pending.length) {
                pending = null;
            }
        }
        return filled;
    }

    public boolean write(byte[] data, int length) {
        UsbDeviceConnection conn = connection;
        if (!open || conn == null) {
            return false;
        }
        int sent = 0;
        while (sent < length) {
            int got = conn.bulkTransfer(out, data, sent, length - sent, 2000);
            if (got <= 0) {
                return false;
            }
            sent += got;
        }
        return true;
    }

    public boolean present() {
        return open;
    }
}
