package io.mgbaldn.esp32test;

import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;

/**
 * The Android half of the Esp32SerialOps seam: a CDC-ACM serial link to the ESP32's native USB Serial/JTAG port.
 * The native code calls read/write/present/log from its own (single) thread.
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
    boolean open(UsbManager manager, UsbDevice device) {
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
            logger.log("FAIL: the device has no CDC data interface (interfaces: " + device.getInterfaceCount() + ")");
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
            logger.log("FAIL: the CDC data interface has no bulk endpoints");
            return false;
        }
        UsbDeviceConnection conn = manager.openDevice(device);
        if (conn == null) {
            logger.log("FAIL: openDevice returned null (USB permission missing?)");
            return false;
        }
        if (comm != null && !conn.claimInterface(comm, true)) {
            logger.log("warning: could not claim the CDC control interface");
        }
        if (!conn.claimInterface(data, true)) {
            logger.log("FAIL: could not claim the CDC data interface");
            conn.close();
            return false;
        }
        int commIndex = comm != null ? comm.getId() : 0;
        // SET_LINE_CODING: 921600 baud, 1 stop bit, no parity, 8 data bits (ignored by a native USB CDC endpoint,
        // but harmless and correct for a UART bridge).
        byte[] coding = {0x00, 0x10, 0x0E, 0x00, 0x00, 0x00, 0x08};
        conn.controlTransfer(0x21, 0x20, 0, commIndex, coding, coding.length, 500);
        // SET_CONTROL_LINE_STATE: DTR on (bit 0), RTS off (bit 1) - a USB CDC device often only talks with DTR up.
        conn.controlTransfer(0x21, 0x22, 0x01, commIndex, null, 0, 500);

        connection = conn;
        controlInterface = comm;
        dataInterface = data;
        in = inEp;
        out = outEp;
        open = true;
        logger.log("USB device opened: bulk in/out endpoints " + inEp.getAddress() + "/" + outEp.getAddress());
        return true;
    }

    void close() {
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

    // ---- called from native ----

    /** Non-blocking read: bytes read (0 if none right now), or -1 if the link failed. */
    public int read(byte[] buffer) {
        UsbDeviceConnection conn = connection;
        if (!open || conn == null) {
            return -1;
        }
        int got = conn.bulkTransfer(in, buffer, buffer.length, 10);
        // A timeout with no data is reported as -1 by bulkTransfer; the device going away looks the same, so a
        // vanished device is detected through the open flag instead (cleared by close()).
        return got < 0 ? 0 : got;
    }

    public boolean write(byte[] data, int length) {
        UsbDeviceConnection conn = connection;
        if (!open || conn == null) {
            return false;
        }
        int sent = 0;
        while (sent < length) {
            byte[] chunk = data;
            int offset = sent;
            int n = length - sent;
            int got = conn.bulkTransfer(out, chunk, offset, n, 2000);
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

    public void log(String line) {
        logger.log(line);
    }
}
