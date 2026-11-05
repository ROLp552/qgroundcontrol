/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "UsbUvcCamera.h"
#include "QGCLoggingCategory.h"
#include "SettingsManager.h"
#include "VideoSettings.h"

#include <QtCore/qapplicationstatic.h>
#include <QtCore/QJniEnvironment>
#include <QtCore/QJniObject>
#include <QtGui/QImage>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoFrameFormat>
#include <QtMultimedia/QVideoSink>

#include <libusb.h>
#include <libuvc/libuvc.h>

#include <algorithm>
#include <cstring>

QGC_LOGGING_CATEGORY(UsbUvcCameraLog, "qgc.videomanager.videoreceiver.usbuvc")

Q_APPLICATION_STATIC(UsbUvcCamera, _usbUvcCameraInstance);

static constexpr const char *kJniUvcCameraClassName = "org/mavlink/qgroundcontrol/QGCUvcCamera";
static constexpr const char *kJniUvcRecorderClassName = "org/mavlink/qgroundcontrol/QGCUvcRecorder";
// MediaCodecInfo.CodecCapabilities color formats
static constexpr int kColorNV12 = 21;   // COLOR_FormatYUV420SemiPlanar
static constexpr int kColorI420 = 19;   // COLOR_FormatYUV420Planar
static constexpr jint kNoDevice = -1;
static constexpr jint kWaitingForPermission = -2;
static constexpr int kWatchdogMsecs = 3000;
static constexpr int kKickDurationMsecs = 1500;
static constexpr int kMaxStreamKicks = 3;
static constexpr quint64 kMinFramesForGoodStream = 3;

// USB transfer diagnostics, fed by hooks patched into libuvc's stream.c (see CMakeLists.txt)
namespace {
    std::atomic<quint64> gXferCompleted = 0;
    std::atomic<quint64> gXferError = 0;
    std::atomic<quint64> gXferTimedOut = 0;
    std::atomic<quint64> gXferOther = 0;
    std::atomic<quint64> gIsoBadPackets = 0;
    std::atomic<quint64> gNonEmptyPackets = 0;
    std::atomic<quint64> gBytes = 0;
    std::atomic<int> gAltSetting = -1;
    std::atomic<int> gBytesPerPacket = 0;
    std::atomic<int> gPacketsPerTransfer = 0;
    std::atomic<int> gConfigBytesPerPacket = 0;
    std::atomic<int> gSubmitted = -1;
    std::atomic<int> gSubmitRet = 0;

    void resetUvcHookCounters()
    {
        gXferCompleted = 0;
        gXferError = 0;
        gXferTimedOut = 0;
        gXferOther = 0;
        gIsoBadPackets = 0;
        gNonEmptyPackets = 0;
        gBytes = 0;
        gAltSetting = -1;
        gBytesPerPacket = 0;
        gPacketsPerTransfer = 0;
        gConfigBytesPerPacket = 0;
        gSubmitted = -1;
        gSubmitRet = 0;
    }
}

extern "C" void qgc_uvc_transfer_hook(struct libusb_transfer *transfer)
{
    switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        gXferCompleted++;
        if (transfer->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
            for (int i = 0; i < transfer->num_iso_packets; i++) {
                const libusb_iso_packet_descriptor &pkt = transfer->iso_packet_desc[i];
                if (pkt.status != LIBUSB_TRANSFER_COMPLETED) {
                    gIsoBadPackets++;
                } else if (pkt.actual_length > 0) {
                    gNonEmptyPackets++;
                    gBytes += pkt.actual_length;
                }
            }
        } else if (transfer->actual_length > 0) {
            gNonEmptyPackets++;
            gBytes += transfer->actual_length;
        }
        break;
    case LIBUSB_TRANSFER_TIMED_OUT:
        gXferTimedOut++;
        break;
    case LIBUSB_TRANSFER_ERROR:
        gXferError++;
        break;
    default:
        gXferOther++;
        break;
    }
}

extern "C" void qgc_uvc_altsetting_hook(int alt, int bytesPerPacket, int packetsPerTransfer, int configBytesPerPacket)
{
    gAltSetting = alt;
    gBytesPerPacket = bytesPerPacket;
    gPacketsPerTransfer = packetsPerTransfer;
    gConfigBytesPerPacket = configBytesPerPacket;
}

extern "C" void qgc_uvc_submit_hook(int submitted, int ret)
{
    gSubmitted = submitted;
    gSubmitRet = ret;
}

UsbUvcCamera::UsbUvcCamera(QObject *parent)
    : QObject(parent)
{
    _status = tr("Not started");

    _debugTimer.setInterval(1000);
    (void) connect(&_debugTimer, &QTimer::timeout, this, &UsbUvcCamera::_updateDebugInfo);

    _watchdogTimer.setSingleShot(true);
    _watchdogTimer.setInterval(kWatchdogMsecs);
    (void) connect(&_watchdogTimer, &QTimer::timeout, this, &UsbUvcCamera::_checkStream);
}

void UsbUvcCamera::setCpuConvert(bool cpuConvert)
{
    if (cpuConvert != _cpuConvert.load()) {
        _cpuConvert = cpuConvert;
        emit cpuConvertChanged();
        _updateDebugInfo();
    }
}

void UsbUvcCamera::_updateDebugInfo()
{
    const QSize sinkSize = _sink ? _sink->videoSize() : QSize();
    _debugInfo = QStringLiteral("%1 | rx %2 incomplete %3 busy %4 failed %5 shown %6 | last %7x%8 %9 bytes | sink %10x%11 | %12 (tap to switch)")
        .arg(_status)
        .arg(_framesReceived.load()).arg(_framesIncomplete.load()).arg(_framesBusy.load())
        .arg(_framesFailed.load()).arg(_framesShown.load())
        .arg(_lastFrameWidth.load()).arg(_lastFrameHeight.load()).arg(_lastFrameBytes.load())
        .arg(sinkSize.width()).arg(sinkSize.height())
        .arg(_cpuConvert.load() ? QStringLiteral("CPU RGB") : QStringLiteral("GPU YUY2"));
    _debugInfo += QStringLiteral("\nUSB: alt %1 pkt %2 (need %3) x%4 | submitted %5 ret %6 | xfer ok %7 err %8 timeout %9 other %10 | pkts bad %11 data %12 | %13 KB")
        .arg(gAltSetting.load()).arg(gBytesPerPacket.load()).arg(gConfigBytesPerPacket.load()).arg(gPacketsPerTransfer.load())
        .arg(gSubmitted.load()).arg(gSubmitRet.load())
        .arg(gXferCompleted.load()).arg(gXferError.load()).arg(gXferTimedOut.load()).arg(gXferOther.load())
        .arg(gIsoBadPackets.load()).arg(gNonEmptyPackets.load()).arg(gBytes.load() / 1024);
    emit debugInfoChanged();
}

UsbUvcCamera::~UsbUvcCamera()
{
    // Runs at application exit: release libuvc only, the Java side goes away with the process
    if (_devh) {
        uvc_stop_streaming(_devh);
        uvc_close(_devh);
        _devh = nullptr;
    }
    if (_ctx) {
        uvc_exit(_ctx);
        _ctx = nullptr;
    }
    if (_eventThread.joinable()) {
        _stopEventThread = true;
        _eventThread.join();
    }
    if (_usbCtx) {
        libusb_exit(_usbCtx);
        _usbCtx = nullptr;
    }
}

void UsbUvcCamera::_eventLoop()
{
    while (!_stopEventThread.load()) {
        struct timeval tv = { 0, 100 * 1000 };
        (void) libusb_handle_events_timeout_completed(_usbCtx, &tv, nullptr);
    }
}

UsbUvcCamera *UsbUvcCamera::instance()
{
    return _usbUvcCameraInstance();
}

static void jniUvcDeviceChanged(JNIEnv *, jclass)
{
    // Called from a Java thread
    (void) QMetaObject::invokeMethod(UsbUvcCamera::instance(), "_deviceChanged", Qt::QueuedConnection);
}

void UsbUvcCamera::setNativeMethods()
{
    const JNINativeMethod javaMethods[] {
        {"nativeUvcDeviceChanged", "()V", reinterpret_cast<void *>(jniUvcDeviceChanged)}
    };

    QJniEnvironment jniEnv;
    (void) jniEnv.checkAndClearExceptions();

    jclass objectClass = jniEnv->FindClass(kJniUvcCameraClassName);
    if (!objectClass) {
        qCWarning(UsbUvcCameraLog) << "Couldn't find class:" << kJniUvcCameraClassName;
        (void) jniEnv.checkAndClearExceptions();
        return;
    }

    const jint val = jniEnv->RegisterNatives(objectClass, javaMethods, std::size(javaMethods));
    if (val < 0) {
        qCWarning(UsbUvcCameraLog) << "Error registering methods:" << val;
    }

    (void) jniEnv.checkAndClearExceptions();
}

void UsbUvcCamera::start(QObject *videoSink)
{
    _sink = qobject_cast<QVideoSink*>(videoSink);
    if (!_sink) {
        qCWarning(UsbUvcCameraLog) << "start: not a QVideoSink" << videoSink;
        return;
    }

    _wantStreaming = true;
    _tryOpen();
}

void UsbUvcCamera::stop()
{
    _wantStreaming = false;
    _close();
    _setStatus(tr("Stopped"));
}

void UsbUvcCamera::_deviceChanged()
{
    const bool hasDevice = QJniObject::callStaticMethod<jboolean>(kJniUvcCameraClassName, "hasDevice", "()Z");
    qCDebug(UsbUvcCameraLog) << "Device changed, has device:" << hasDevice;

    if (!hasDevice) {
        _close();
        _setStatus(tr("No USB video device connected"));
    } else if (_wantStreaming && !_devh) {
        _tryOpen();
    }
}

void UsbUvcCamera::_tryOpen()
{
    if (_devh) {
        return;
    }

    const jint fd = QJniObject::callStaticMethod<jint>(kJniUvcCameraClassName, "open", "()I");
    if (fd == kWaitingForPermission) {
        _setStatus(tr("Waiting for USB permission"));
        return;
    }
    if (fd < 0) {
        _setStatus(tr("No USB video device connected"));
        return;
    }

    if (!_usbCtx) {
        // Android apps can't enumerate /dev/bus/usb, devices are only opened from a Java file descriptor
        (void) libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
        const int usbRes = libusb_init(&_usbCtx);
        if (usbRes < 0) {
            _usbCtx = nullptr;
            _setStatus(tr("USB init failed: %1").arg(libusb_error_name(usbRes)));
            QJniObject::callStaticMethod<void>(kJniUvcCameraClassName, "close", "()V");
            return;
        }
        _stopEventThread = false;
        _eventThread = std::thread(&UsbUvcCamera::_eventLoop, this);
    }

    if (!_ctx) {
        const uvc_error_t res = uvc_init(&_ctx, _usbCtx);
        if (res < 0) {
            _ctx = nullptr;
            _setStatus(tr("USB video init failed: %1").arg(uvc_strerror(res)));
            QJniObject::callStaticMethod<void>(kJniUvcCameraClassName, "close", "()V");
            return;
        }
    }

    uvc_error_t res = uvc_wrap(fd, _ctx, &_devh);
    if (res < 0) {
        _devh = nullptr;
        _setStatus(tr("USB video open failed: %1").arg(uvc_strerror(res)));
        QJniObject::callStaticMethod<void>(kJniUvcCameraClassName, "close", "()V");
        return;
    }

    if (!_startStream(false)) {
        _close();
        return;
    }

    emit streamingChanged();
    _debugTimer.start();
    _kickCount = 0;
    _watchdogTimer.start();
}

bool UsbUvcCamera::_startStream(bool alternateFormat)
{
    // Auto prefers uncompressed YUY2 (native resolution of analog grabbers, no decoding), then MJPEG.
    // MJPEG needs much less USB bandwidth. Pick the largest frame size of the chosen format.
    // alternateFormat starts the other format, used to kick devices that don't stream on the first start.
    const uint32_t formatSetting = SettingsManager::instance()->videoSettings()->usbUvcFormat()->rawValue().toUInt();
    QList<enum uvc_vs_desc_subtype> subtypes;
    if (formatSetting == 2) {
        subtypes = {UVC_VS_FORMAT_MJPEG, UVC_VS_FORMAT_UNCOMPRESSED};
    } else {
        subtypes = {UVC_VS_FORMAT_UNCOMPRESSED, UVC_VS_FORMAT_MJPEG};
    }
    if (alternateFormat) {
        std::reverse(subtypes.begin(), subtypes.end());
    } else if (formatSetting != 0) {
        // An explicit format setting means only that format
        subtypes.removeLast();
    }

    uvc_frame_format frameFormat = UVC_FRAME_FORMAT_UNKNOWN;
    QSize frameSize;
    for (const enum uvc_vs_desc_subtype subtype : std::as_const(subtypes)) {
        for (const uvc_format_desc_t *format = uvc_get_format_descs(_devh); format; format = format->next) {
            if (format->bDescriptorSubtype != subtype) {
                continue;
            }
            if ((subtype == UVC_VS_FORMAT_UNCOMPRESSED) && (std::memcmp(format->fourccFormat, "YUY2", 4) != 0)) {
                continue;
            }
            for (const uvc_frame_desc_t *frame = format->frame_descs; frame; frame = frame->next) {
                const QSize size(frame->wWidth, frame->wHeight);
                qCDebug(UsbUvcCameraLog) << "Available:" << QByteArray(reinterpret_cast<const char*>(format->fourccFormat), 4) << size
                                         << "default fps:" << (frame->dwDefaultFrameInterval ? 10000000 / frame->dwDefaultFrameInterval : 0);
                if ((size.width() * size.height()) > (frameSize.width() * frameSize.height())) {
                    frameSize = size;
                }
            }
            if (frameSize.isValid()) {
                frameFormat = (subtype == UVC_VS_FORMAT_UNCOMPRESSED) ? UVC_FRAME_FORMAT_YUYV : UVC_FRAME_FORMAT_MJPEG;
                break;
            }
        }
        if (frameFormat != UVC_FRAME_FORMAT_UNKNOWN) {
            break;
        }
    }

    if (frameFormat == UVC_FRAME_FORMAT_UNKNOWN) {
        _setStatus(tr("USB video device doesn't offer the selected format"));
        return false;
    }

    uvc_stream_ctrl_t ctrl{};
    // fps 0: accept the device's first (usually highest) frame rate for this size
    uvc_error_t res = uvc_get_stream_ctrl_format_size(_devh, &ctrl, frameFormat, frameSize.width(), frameSize.height(), 0);
    if (res < 0) {
        _setStatus(tr("USB video format not accepted: %1").arg(uvc_strerror(res)));
        return false;
    }

    _frameSize = frameSize;
    _streamFps = ctrl.dwFrameInterval ? static_cast<int>(10000000 / ctrl.dwFrameInterval) : 25;
    _framesInFlight = 0;
    resetUvcHookCounters();
    _framesReceived = 0;
    _framesIncomplete = 0;
    _framesBusy = 0;
    _framesFailed = 0;
    _framesShown = 0;
    res = uvc_start_streaming(_devh, &ctrl, &UsbUvcCamera::_frameCallback, this, 0);
    if (res < 0) {
        _setStatus(tr("USB video start failed: %1").arg(uvc_strerror(res)));
        return false;
    }

    const QString formatName = (frameFormat == UVC_FRAME_FORMAT_YUYV) ? QStringLiteral("YUY2") : QStringLiteral("MJPEG");
    qCDebug(UsbUvcCameraLog) << "Streaming" << formatName << frameSize << (alternateFormat ? "(kick)" : "");
    _setStatus(tr("Streaming %1 %2x%3").arg(formatName).arg(frameSize.width()).arg(frameSize.height()));
    return true;
}

void UsbUvcCamera::_checkStream()
{
    // Some USB analog grabbers don't send video on the first stream start after being opened, but do
    // after the format was changed once. Do that automatically when no frames arrive.
    if (!_devh || (_framesReceived.load() >= kMinFramesForGoodStream)) {
        return;
    }

    if (_kickCount >= kMaxStreamKicks) {
        _setStatus(tr("No video from USB device"));
        return;
    }

    _kickCount++;
    qCDebug(UsbUvcCameraLog) << "No frames, restarting stream, attempt" << _kickCount;
    uvc_stop_streaming(_devh);
    (void) _startStream(true);

    const quint64 generation = _streamGeneration;
    QTimer::singleShot(kKickDurationMsecs, this, [this, generation]() {
        if (!_devh || (generation != _streamGeneration)) {
            return;
        }
        uvc_stop_streaming(_devh);
        if (_startStream(false)) {
            _watchdogTimer.start();
        }
    });
}

void UsbUvcCamera::_close()
{
    stopRecording();
    _debugTimer.stop();
    _watchdogTimer.stop();
    _streamGeneration++;
    if (_devh) {
        uvc_stop_streaming(_devh);
        uvc_close(_devh);
        _devh = nullptr;
        emit streamingChanged();
    }
    // The Java connection owns the file descriptor, release it after libusb is done with it
    QJniObject::callStaticMethod<void>(kJniUvcCameraClassName, "close", "()V");
}

void UsbUvcCamera::_setStatus(const QString &status)
{
    if (status != _status) {
        qCDebug(UsbUvcCameraLog) << "Status:" << status;
        _status = status;
        emit statusChanged();
        _updateDebugInfo();
    }
}

void UsbUvcCamera::_frameCallback(uvc_frame *frame, void *userPtr)
{
    // Called from the libuvc streaming thread
    static_cast<UsbUvcCamera*>(userPtr)->_handleFrame(frame);
}

void UsbUvcCamera::_handleFrame(uvc_frame *frame)
{
    _framesReceived++;
    _lastFrameBytes = frame->data_bytes;
    _lastFrameWidth = frame->width;
    _lastFrameHeight = frame->height;

    // Record every frame, before frames are dropped for a busy UI
    if (_recActive.load() && (frame->frame_format == UVC_FRAME_FORMAT_YUYV)) {
        _recordYuy2(frame);
    }

    // Drop frames while the UI thread is still busy with the previous ones
    if (_framesInFlight.load() > 1) {
        _framesBusy++;
        return;
    }

    QVideoFrame videoFrame;
    if (frame->frame_format == UVC_FRAME_FORMAT_YUYV) {
        const int width = static_cast<int>(frame->width);
        const int height = static_cast<int>(frame->height);
        const size_t srcStride = frame->step ? frame->step : static_cast<size_t>(width) * 2;
        if (frame->data_bytes < (srcStride * height)) {
            _framesIncomplete++;
            return;
        }
        const uchar *src = static_cast<const uchar*>(frame->data);

        if (_cpuConvert.load()) {
            // BT.601 limited range YUY2 -> RGB32
            QImage image(width, height, QImage::Format_RGB32);
            for (int y = 0; y < height; y++) {
                const uchar *line = src + (y * srcStride);
                QRgb *out = reinterpret_cast<QRgb*>(image.scanLine(y));
                for (int x = 0; x + 1 < width; x += 2) {
                    const int u = line[1] - 128;
                    const int v = line[3] - 128;
                    for (int i = 0; i < 2; i++) {
                        const int c = 298 * (line[i * 2] - 16);
                        out[x + i] = qRgb(qBound(0, (c + 409 * v + 128) >> 8, 255),
                                          qBound(0, (c - 100 * u - 208 * v + 128) >> 8, 255),
                                          qBound(0, (c + 516 * u + 128) >> 8, 255));
                    }
                    line += 4;
                }
            }
            videoFrame = QVideoFrame(image);
        } else {
            videoFrame = QVideoFrame(QVideoFrameFormat(QSize(width, height), QVideoFrameFormat::Format_YUYV));
            if (!videoFrame.map(QVideoFrame::WriteOnly)) {
                _framesFailed++;
                return;
            }
            uchar *dst = videoFrame.bits(0);
            const int dstStride = videoFrame.bytesPerLine(0);
            const size_t lineBytes = qMin(srcStride, static_cast<size_t>(dstStride));
            for (int y = 0; y < height; y++) {
                std::memcpy(dst + (y * dstStride), src + (y * srcStride), lineBytes);
            }
            videoFrame.unmap();
        }
    } else if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG) {
        QImage image;
        if (!image.loadFromData(static_cast<const uchar*>(frame->data), static_cast<int>(frame->data_bytes), "JPG")) {
            _framesFailed++;
            return;
        }
        if (_recActive.load()) {
            _recordImage(image);
        }
        videoFrame = QVideoFrame(image);
    } else {
        _framesFailed++;
        return;
    }

    _framesInFlight++;
    (void) QMetaObject::invokeMethod(this, [this, videoFrame]() {
        _framesInFlight--;
        if (_sink) {
            _sink->setVideoFrame(videoFrame);
            _framesShown++;
        }
    }, Qt::QueuedConnection);
}

bool UsbUvcCamera::startRecording(const QString &fileName)
{
    if (!_devh || !_frameSize.isValid()) {
        emit recordingError(tr("USB video is not streaming"));
        return false;
    }

    std::lock_guard<std::mutex> lock(_recMutex);
    if (_recActive.load()) {
        return true;
    }

    const int width = _frameSize.width();
    const int height = _frameSize.height();
    const int rotation = SettingsManager::instance()->videoSettings()->usbUvcRotation()->rawValue().toInt();
    const QJniObject jFileName = QJniObject::fromString(fileName);
    const jint colorFormat = QJniObject::callStaticMethod<jint>(kJniUvcRecorderClassName, "start", "(Ljava/lang/String;IIII)I",
                                                                jFileName.object<jstring>(), width, height, _streamFps, rotation);
    if ((colorFormat != kColorNV12) && (colorFormat != kColorI420)) {
        emit recordingError(tr("Could not start the H.264 encoder"));
        return false;
    }

    const int size = (width * height * 3) / 2;
    QJniEnvironment env;
    jbyteArray array = env->NewByteArray(size);
    if (!array) {
        (void) env.checkAndClearExceptions();
        QJniObject::callStaticMethod<void>(kJniUvcRecorderClassName, "stop", "()V");
        emit recordingError(tr("Out of memory"));
        return false;
    }
    _recArray = env->NewGlobalRef(array);
    env->DeleteLocalRef(array);

    _recBuffer.assign(size, 0);
    _recSize = _frameSize;
    _recColorFormat = colorFormat;
    _recFileName = fileName;
    _recActive = true;
    qCDebug(UsbUvcCameraLog) << "Recording" << _recSize << "fps" << _streamFps << "color" << colorFormat << fileName;

    emit recordingStateChanged(true, fileName);
    return true;
}

void UsbUvcCamera::stopRecording()
{
    QString fileName;
    {
        std::lock_guard<std::mutex> lock(_recMutex);
        if (!_recActive.load()) {
            return;
        }
        _recActive = false;
        QJniObject::callStaticMethod<void>(kJniUvcRecorderClassName, "stop", "()V");
        if (_recArray) {
            QJniEnvironment env;
            env->DeleteGlobalRef(static_cast<jobject>(_recArray));
            _recArray = nullptr;
        }
        fileName = _recFileName;
    }
    qCDebug(UsbUvcCameraLog) << "Recording stopped" << fileName;
    emit recordingStateChanged(false, fileName);
}

void UsbUvcCamera::_recordYuy2(const uvc_frame *frame)
{
    std::lock_guard<std::mutex> lock(_recMutex);
    const int width = static_cast<int>(frame->width);
    const int height = static_cast<int>(frame->height);
    const size_t srcStride = frame->step ? frame->step : static_cast<size_t>(width) * 2;
    if (!_recActive.load() || (QSize(width, height) != _recSize) || (frame->data_bytes < (srcStride * height))) {
        return;
    }

    // YUY2 (Y0 U Y1 V) -> NV12 (Y plane + interleaved UV) or I420 (Y, U, V planes), chroma from even rows
    uint8_t *yPlane = _recBuffer.data();
    uint8_t *uvPlane = yPlane + (width * height);
    uint8_t *vPlane = uvPlane + ((width / 2) * (height / 2));
    const bool nv12 = (_recColorFormat == kColorNV12);
    const uchar *src = static_cast<const uchar*>(frame->data);
    for (int y = 0; y < height; y++) {
        const uchar *line = src + (y * srcStride);
        uint8_t *yRow = yPlane + (y * width);
        const bool chromaRow = ((y % 2) == 0);
        for (int x = 0; (x + 1) < width; x += 2) {
            const uchar *px = line + (x * 2);
            yRow[x] = px[0];
            yRow[x + 1] = px[2];
            if (chromaRow) {
                if (nv12) {
                    uint8_t *uv = uvPlane + ((y / 2) * width) + x;
                    uv[0] = px[1];
                    uv[1] = px[3];
                } else {
                    const int ci = ((y / 2) * (width / 2)) + (x / 2);
                    uvPlane[ci] = px[1];
                    vPlane[ci] = px[3];
                }
            }
        }
    }
    _encodeRecordBuffer();
}

void UsbUvcCamera::_recordImage(const QImage &decoded)
{
    std::lock_guard<std::mutex> lock(_recMutex);
    if (!_recActive.load() || (decoded.size() != _recSize)) {
        return;
    }

    const QImage image = decoded.convertToFormat(QImage::Format_RGB32);
    const int width = image.width();
    const int height = image.height();
    uint8_t *yPlane = _recBuffer.data();
    uint8_t *uvPlane = yPlane + (width * height);
    uint8_t *vPlane = uvPlane + ((width / 2) * (height / 2));
    const bool nv12 = (_recColorFormat == kColorNV12);
    for (int y = 0; y < height; y++) {
        const QRgb *line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        uint8_t *yRow = yPlane + (y * width);
        for (int x = 0; x < width; x++) {
            const int r = qRed(line[x]);
            const int g = qGreen(line[x]);
            const int b = qBlue(line[x]);
            // BT.601 limited range
            yRow[x] = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            if (((y % 2) == 0) && ((x % 2) == 0)) {
                const uint8_t u = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                const uint8_t v = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                if (nv12) {
                    uint8_t *uv = uvPlane + ((y / 2) * width) + x;
                    uv[0] = u;
                    uv[1] = v;
                } else {
                    const int ci = ((y / 2) * (width / 2)) + (x / 2);
                    uvPlane[ci] = u;
                    vPlane[ci] = v;
                }
            }
        }
    }
    _encodeRecordBuffer();
}

void UsbUvcCamera::_encodeRecordBuffer()
{
    // Called with _recMutex held, from the libuvc capture thread
    if (!_recArray) {
        return;
    }
    QJniEnvironment env;
    const jsize size = static_cast<jsize>(_recBuffer.size());
    jbyteArray array = static_cast<jbyteArray>(_recArray);
    env->SetByteArrayRegion(array, 0, size, reinterpret_cast<const jbyte*>(_recBuffer.data()));
    (void) QJniObject::callStaticMethod<jboolean>(kJniUvcRecorderClassName, "encode", "([BI)Z", array, size);
    (void) env.checkAndClearExceptions();
}
