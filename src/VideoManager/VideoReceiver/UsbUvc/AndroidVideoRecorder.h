/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>

#include <atomic>
#include <mutex>
#include <vector>

class QImage;
class QVideoFrame;

/// Records raw video frames to an MP4 file with Android's hardware H.264 encoder (QGCUvcRecorder.java).
/// Used for local video sources on Android: Qt's FFmpeg based MediaRecorder can't open a video encoder
/// on some devices (e.g. RadioMaster AX12). The encoder is started on the first frame, with its size.
/// The record* functions may be called from any thread.
class AndroidVideoRecorder : public QObject
{
    Q_OBJECT

public:
    explicit AndroidVideoRecorder(QObject *parent = nullptr);

    /// @param rotation Extra clockwise rotation (degrees) stored as the file's orientation hint
    void start(const QString &fileName, int fps, int rotation);
    void stop();
    bool active() const { return _active.load(); }
    QString fileName() const;

    void recordYuy2(const uchar *data, size_t bytes, int width, int height, size_t stride);
    void recordImage(const QImage &image);
    /// Any CPU mappable YUV 4:2:0 / YUY2 frame, other formats go through QVideoFrame::toImage()
    void recordVideoFrame(const QVideoFrame &frame);

signals:
    /// Emitted from the recording thread with the recorder stopped already. Connect it queued:
    /// it is emitted while the recorder lock is held.
    void errorOccurred(const QString &errorString);

private:
    bool _prepare(const QSize &size, int frameRotation);
    void _writeYuv420(const uchar *y, int yStride, const uchar *u, const uchar *v, int uvStride, int uvPixelStride, int width, int height);
    void _encode();
    void _stopLocked();

    mutable std::mutex _mutex;
    std::atomic<bool> _active = false;
    bool _encoderStarted = false;
    QString _fileName;
    int _fps = 30;
    int _rotation = 0;
    QSize _size;
    int _colorFormat = 0;
    void *_array = nullptr;  // global ref to the Java byte[] frame buffer
    std::vector<uint8_t> _buffer;
};
