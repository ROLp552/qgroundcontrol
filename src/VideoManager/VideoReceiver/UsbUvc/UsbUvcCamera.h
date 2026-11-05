/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSize>
#include <QtCore/QTimer>

#include <atomic>
#include <thread>

Q_DECLARE_LOGGING_CATEGORY(UsbUvcCameraLog)

class QVideoSink;
struct libusb_context;
struct uvc_context;
struct uvc_device_handle;
struct uvc_frame;

/// Reads a USB video class device (e.g. an analog video grabber) directly with libuvc on Android,
/// for devices that Android's camera API does not expose. Frames are pushed to a QVideoSink.
class UsbUvcCamera : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool     streaming   READ streaming  NOTIFY streamingChanged)
    Q_PROPERTY(QString  status      READ status     NOTIFY statusChanged)
    /// On screen diagnostics (frame counters), updated once per second while streaming
    Q_PROPERTY(QString  debugInfo   READ debugInfo  NOTIFY debugInfoChanged)
    /// Convert YUY2 to RGB on the CPU instead of letting the GPU render YUY2 frames
    Q_PROPERTY(bool     cpuConvert  READ cpuConvert WRITE setCpuConvert NOTIFY cpuConvertChanged)

public:
    explicit UsbUvcCamera(QObject *parent = nullptr);
    ~UsbUvcCamera();

    static UsbUvcCamera *instance();
    static void setNativeMethods();

    /// Start showing the USB video in the given QVideoSink (VideoOutput.videoSink from QML)
    Q_INVOKABLE void start(QObject *videoSink);
    Q_INVOKABLE void stop();

    bool streaming() const { return _devh != nullptr; }
    QString status() const { return _status; }
    QString debugInfo() const { return _debugInfo; }
    bool cpuConvert() const { return _cpuConvert.load(); }
    void setCpuConvert(bool cpuConvert);

signals:
    void streamingChanged();
    void statusChanged();
    void debugInfoChanged();
    void cpuConvertChanged();

private slots:
    void _deviceChanged();
    void _updateDebugInfo();
    void _checkStream();

private:
    void _tryOpen();
    void _close();
    bool _startStream(bool alternateFormat);
    void _setStatus(const QString &status);
    void _handleFrame(uvc_frame *frame);
    static void _frameCallback(uvc_frame *frame, void *userPtr);

    void _eventLoop();

    // We own the libusb context and its event thread: libuvc's own handler thread does not
    // restart after the last device was closed once, which leaves reopened streams without frames.
    libusb_context *_usbCtx = nullptr;
    std::thread _eventThread;
    std::atomic<bool> _stopEventThread = false;
    uvc_context *_ctx = nullptr;
    uvc_device_handle *_devh = nullptr;
    QPointer<QVideoSink> _sink;
    bool _wantStreaming = false;
    QString _status;
    QSize _frameSize;
    std::atomic<int> _framesInFlight = 0;

    std::atomic<bool> _cpuConvert = false;
    QTimer _debugTimer;
    QTimer _watchdogTimer;
    int _kickCount = 0;
    quint64 _streamGeneration = 0;
    QString _debugInfo;
    std::atomic<quint64> _framesReceived = 0;
    std::atomic<quint64> _framesIncomplete = 0;
    std::atomic<quint64> _framesBusy = 0;
    std::atomic<quint64> _framesFailed = 0;
    std::atomic<quint64> _framesShown = 0;
    std::atomic<quint64> _lastFrameBytes = 0;
    std::atomic<quint64> _lastFrameWidth = 0;
    std::atomic<quint64> _lastFrameHeight = 0;
};
