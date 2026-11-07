/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "AndroidVideoRecorder.h"
#include "UsbUvcCamera.h"

#include <QtCore/QJniEnvironment>
#include <QtCore/QJniObject>
#include <QtGui/QImage>
#include <QtMultimedia/QVideoFrame>

#include <cstring>

static constexpr const char *kJniRecorderClassName = "org/mavlink/qgroundcontrol/QGCUvcRecorder";
// MediaCodecInfo.CodecCapabilities color formats
static constexpr int kColorNV12 = 21;   // COLOR_FormatYUV420SemiPlanar
static constexpr int kColorI420 = 19;   // COLOR_FormatYUV420Planar

AndroidVideoRecorder::AndroidVideoRecorder(QObject *parent)
    : QObject(parent)
{
}

void AndroidVideoRecorder::start(const QString &fileName, int fps, int rotation)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _stopLocked();
    _fileName = fileName;
    _fps = (fps > 0) ? fps : 30;
    _rotation = rotation;
    _encoderStarted = false;
    _active = true;
}

void AndroidVideoRecorder::stop()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _stopLocked();
}

QString AndroidVideoRecorder::fileName() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _fileName;
}

void AndroidVideoRecorder::_stopLocked()
{
    _active = false;
    if (_encoderStarted) {
        QJniObject::callStaticMethod<void>(kJniRecorderClassName, "stop", "()V");
        _encoderStarted = false;
    }
    if (_array) {
        QJniEnvironment env;
        env->DeleteGlobalRef(static_cast<jobject>(_array));
        _array = nullptr;
    }
}

bool AndroidVideoRecorder::_prepare(const QSize &size, int frameRotation)
{
    // Called with _mutex held
    if (!_active.load()) {
        return false;
    }
    if (_encoderStarted) {
        // Frames of another size (e.g. a source format change) are skipped
        return size == _size;
    }

    const int width = size.width() & ~1;
    const int height = size.height() & ~1;
    if ((width <= 0) || (height <= 0)) {
        return false;
    }

    const int rotation = (((frameRotation + _rotation) % 360) + 360) % 360;
    const QJniObject jFileName = QJniObject::fromString(_fileName);
    const jint colorFormat = QJniObject::callStaticMethod<jint>(kJniRecorderClassName, "start", "(Ljava/lang/String;IIII)I",
                                                                jFileName.object<jstring>(), width, height, _fps, rotation);
    if ((colorFormat != kColorNV12) && (colorFormat != kColorI420)) {
        _stopLocked();
        emit errorOccurred(tr("Could not start the H.264 encoder"));
        return false;
    }

    const int bufferSize = (width * height * 3) / 2;
    QJniEnvironment env;
    jbyteArray array = env->NewByteArray(bufferSize);
    if (!array) {
        (void) env.checkAndClearExceptions();
        _encoderStarted = true;
        _stopLocked();
        emit errorOccurred(tr("Out of memory"));
        return false;
    }
    _array = env->NewGlobalRef(array);
    env->DeleteLocalRef(array);

    _buffer.assign(bufferSize, 0);
    _size = QSize(width, height);
    _colorFormat = colorFormat;
    _encoderStarted = true;
    qCDebug(UsbUvcCameraLog) << "Recording" << _size << "fps" << _fps << "color" << colorFormat << "rotation" << rotation << _fileName;
    return true;
}

void AndroidVideoRecorder::_writeYuv420(const uchar *y, int yStride, const uchar *u, const uchar *v, int uvStride, int uvPixelStride, int width, int height)
{
    // Called with _mutex held. Writes the encoder's NV12 or I420 layout from any YUV 4:2:0 source.
    uint8_t *yPlane = _buffer.data();
    uint8_t *uvPlane = yPlane + (width * height);
    uint8_t *vPlane = uvPlane + ((width / 2) * (height / 2));
    const bool nv12 = (_colorFormat == kColorNV12);

    for (int row = 0; row < height; row++) {
        std::memcpy(yPlane + (row * width), y + (row * yStride), width);
    }
    for (int row = 0; row < (height / 2); row++) {
        const uchar *uRow = u + (row * uvStride);
        const uchar *vRow = v + (row * uvStride);
        for (int col = 0; col < (width / 2); col++) {
            if (nv12) {
                uvPlane[(row * width) + (col * 2)] = uRow[col * uvPixelStride];
                uvPlane[(row * width) + (col * 2) + 1] = vRow[col * uvPixelStride];
            } else {
                uvPlane[(row * (width / 2)) + col] = uRow[col * uvPixelStride];
                vPlane[(row * (width / 2)) + col] = vRow[col * uvPixelStride];
            }
        }
    }
}

void AndroidVideoRecorder::recordYuy2(const uchar *data, size_t bytes, int width, int height, size_t stride)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if ((bytes < (stride * height)) || !_prepare(QSize(width, height), 0)) {
        return;
    }

    // YUY2 (Y0 U Y1 V) -> NV12 or I420, chroma from even rows
    const int w = _size.width();
    const int h = _size.height();
    uint8_t *yPlane = _buffer.data();
    uint8_t *uvPlane = yPlane + (w * h);
    uint8_t *vPlane = uvPlane + ((w / 2) * (h / 2));
    const bool nv12 = (_colorFormat == kColorNV12);
    for (int row = 0; row < h; row++) {
        const uchar *line = data + (row * stride);
        uint8_t *yRow = yPlane + (row * w);
        const bool chromaRow = ((row % 2) == 0);
        for (int col = 0; (col + 1) < w; col += 2) {
            const uchar *px = line + (col * 2);
            yRow[col] = px[0];
            yRow[col + 1] = px[2];
            if (chromaRow) {
                if (nv12) {
                    uint8_t *uv = uvPlane + ((row / 2) * w) + col;
                    uv[0] = px[1];
                    uv[1] = px[3];
                } else {
                    const int ci = ((row / 2) * (w / 2)) + (col / 2);
                    uvPlane[ci] = px[1];
                    vPlane[ci] = px[3];
                }
            }
        }
    }
    _encode();
}

void AndroidVideoRecorder::recordImage(const QImage &decoded)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_prepare(decoded.size(), 0)) {
        return;
    }

    const QImage image = decoded.convertToFormat(QImage::Format_RGB32);
    const int w = _size.width();
    const int h = _size.height();
    uint8_t *yPlane = _buffer.data();
    uint8_t *uvPlane = yPlane + (w * h);
    uint8_t *vPlane = uvPlane + ((w / 2) * (h / 2));
    const bool nv12 = (_colorFormat == kColorNV12);
    for (int row = 0; row < h; row++) {
        const QRgb *line = reinterpret_cast<const QRgb*>(image.constScanLine(row));
        uint8_t *yRow = yPlane + (row * w);
        for (int col = 0; col < w; col++) {
            const int r = qRed(line[col]);
            const int g = qGreen(line[col]);
            const int b = qBlue(line[col]);
            // BT.601 limited range
            yRow[col] = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            if (((row % 2) == 0) && ((col % 2) == 0)) {
                const uint8_t u = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                const uint8_t v = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                if (nv12) {
                    uint8_t *uv = uvPlane + ((row / 2) * w) + col;
                    uv[0] = u;
                    uv[1] = v;
                } else {
                    const int ci = ((row / 2) * (w / 2)) + (col / 2);
                    uvPlane[ci] = u;
                    vPlane[ci] = v;
                }
            }
        }
    }
    _encode();
}

void AndroidVideoRecorder::recordVideoFrame(const QVideoFrame &source)
{
    if (!_active.load() || !source.isValid()) {
        return;
    }

    const QVideoFrameFormat::PixelFormat pixelFormat = source.pixelFormat();
    const bool yuv420 = (pixelFormat == QVideoFrameFormat::Format_NV12) || (pixelFormat == QVideoFrameFormat::Format_NV21) ||
                        (pixelFormat == QVideoFrameFormat::Format_YUV420P) || (pixelFormat == QVideoFrameFormat::Format_YV12);
    const bool yuyv = (pixelFormat == QVideoFrameFormat::Format_YUYV);

    QVideoFrame frame(source);
    if ((yuv420 || yuyv) && frame.map(QVideoFrame::ReadOnly)) {
        const int width = frame.width();
        const int height = frame.height();
        if (yuyv) {
            const size_t stride = static_cast<size_t>(frame.bytesPerLine(0));
            recordYuy2(frame.bits(0), stride * height, width, height, stride);
        } else {
            std::lock_guard<std::mutex> lock(_mutex);
            // The camera frame's own rotation is added to the orientation hint (the view applies it too)
            if (_prepare(QSize(width, height), static_cast<int>(source.rotation()))) {
                const uchar *y = frame.bits(0);
                const int yStride = frame.bytesPerLine(0);
                const uchar *p1 = frame.bits(1);
                const int uvStride = frame.bytesPerLine(1);
                switch (pixelFormat) {
                case QVideoFrameFormat::Format_NV12:
                    _writeYuv420(y, yStride, p1, p1 + 1, uvStride, 2, _size.width(), _size.height());
                    break;
                case QVideoFrameFormat::Format_NV21:
                    _writeYuv420(y, yStride, p1 + 1, p1, uvStride, 2, _size.width(), _size.height());
                    break;
                case QVideoFrameFormat::Format_YUV420P:
                    _writeYuv420(y, yStride, p1, frame.bits(2), uvStride, 1, _size.width(), _size.height());
                    break;
                default: // Format_YV12: V plane before U
                    _writeYuv420(y, yStride, frame.bits(2), p1, uvStride, 1, _size.width(), _size.height());
                    break;
                }
                _encode();
            }
        }
        frame.unmap();
        return;
    }

    // Other formats (or GPU only frames): let Qt convert. toImage() already applies the frame rotation.
    const QImage image = source.toImage();
    if (!image.isNull()) {
        recordImage(image);
    }
}

void AndroidVideoRecorder::_encode()
{
    // Called with _mutex held, from the thread delivering frames
    if (!_array) {
        return;
    }
    QJniEnvironment env;
    const jsize size = static_cast<jsize>(_buffer.size());
    jbyteArray array = static_cast<jbyteArray>(_array);
    env->SetByteArrayRegion(array, 0, size, reinterpret_cast<const jbyte*>(_buffer.data()));
    (void) QJniObject::callStaticMethod<jboolean>(kJniRecorderClassName, "encode", "([BI)Z", array, size);
    (void) env.checkAndClearExceptions();
}
