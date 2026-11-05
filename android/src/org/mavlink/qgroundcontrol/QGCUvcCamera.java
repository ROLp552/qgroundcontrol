package org.mavlink.qgroundcontrol;

import android.app.PendingIntent;
import android.content.*;
import android.hardware.usb.*;

/**
 * Gives native code access to USB video class (UVC) devices that Android's camera API does not
 * expose, such as USB analog video grabbers. Native code reads the video with libuvc on the file
 * descriptor returned by open().
 */
public class QGCUvcCamera {
    private static final String TAG = QGCUvcCamera.class.getSimpleName();
    private static final String ACTION_USB_PERMISSION = "org.mavlink.qgroundcontrol.action.UVC_USB_PERMISSION";

    /** open() results that are not a file descriptor */
    public static final int NO_DEVICE = -1;
    public static final int WAITING_FOR_PERMISSION = -2;

    private static UsbDeviceConnection connection = null;
    private static boolean receiverRegistered = false;

    /** Called on device attach/detach and when the user answers the USB permission dialog */
    private static native void nativeUvcDeviceChanged();

    private static Context getContext() {
        final QGCActivity activity = QGCActivity.getInstance();
        return (activity != null) ? activity.getApplicationContext() : null;
    }

    private static boolean isVideoDevice(final UsbDevice device) {
        if (device.getDeviceClass() == UsbConstants.USB_CLASS_VIDEO) {
            return true;
        }
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            if (device.getInterface(i).getInterfaceClass() == UsbConstants.USB_CLASS_VIDEO) {
                return true;
            }
        }
        return false;
    }

    private static UsbDevice findDevice(final UsbManager usbManager) {
        for (final UsbDevice device : usbManager.getDeviceList().values()) {
            if (isVideoDevice(device)) {
                return device;
            }
        }
        return null;
    }

    private static final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(final Context context, final Intent intent) {
            final UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if ((device == null) || !isVideoDevice(device)) {
                return;
            }
            QGCLogger.i(TAG, "UVC device event " + intent.getAction() + " " + device.getDeviceName());
            // On detach native code stops libuvc first and then calls close()
            try {
                nativeUvcDeviceChanged();
            } catch (final Exception ex) {
                QGCLogger.e(TAG, "Exception nativeUvcDeviceChanged()", ex);
            }
        }
    };

    private static void ensureReceiver(final Context context) {
        if (receiverRegistered) {
            return;
        }
        final IntentFilter filter = new IntentFilter();
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        filter.addAction(ACTION_USB_PERMISSION);
        try {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
                context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
            } else {
                context.registerReceiver(receiver, filter);
            }
            receiverRegistered = true;
        } catch (final Exception e) {
            QGCLogger.e(TAG, "Failed to register UVC BroadcastReceiver", e);
        }
    }

    /** @return true if a USB video device is connected */
    public static boolean hasDevice() {
        final Context context = getContext();
        if (context == null) {
            return false;
        }
        ensureReceiver(context);
        final UsbManager usbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        return (usbManager != null) && (findDevice(usbManager) != null);
    }

    /**
     * Opens the first USB video device.
     * @return the file descriptor, NO_DEVICE, or WAITING_FOR_PERMISSION if the permission dialog was shown
     */
    public static int open() {
        final Context context = getContext();
        if (context == null) {
            return NO_DEVICE;
        }
        ensureReceiver(context);
        close();

        final UsbManager usbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        final UsbDevice device = (usbManager != null) ? findDevice(usbManager) : null;
        if (device == null) {
            return NO_DEVICE;
        }

        if (!usbManager.hasPermission(device)) {
            int flags = PendingIntent.FLAG_UPDATE_CURRENT;
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S) {
                flags |= PendingIntent.FLAG_MUTABLE;
            }
            final Intent intent = new Intent(ACTION_USB_PERMISSION).setPackage(context.getPackageName());
            usbManager.requestPermission(device, PendingIntent.getBroadcast(context, 1, intent, flags));
            QGCLogger.i(TAG, "Requesting permission for UVC device " + device.getDeviceName());
            return WAITING_FOR_PERMISSION;
        }

        connection = usbManager.openDevice(device);
        if (connection == null) {
            QGCLogger.w(TAG, "Failed to open UVC device " + device.getDeviceName());
            return NO_DEVICE;
        }
        QGCLogger.i(TAG, "Opened UVC device " + device.getDeviceName() + " " + device.getProductName());
        return connection.getFileDescriptor();
    }

    /** Must be called after native code has released the file descriptor */
    public static void close() {
        if (connection != null) {
            connection.close();
            connection = null;
        }
    }
}
