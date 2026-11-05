package org.mavlink.qgroundcontrol;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import java.nio.ByteBuffer;

/**
 * Records raw frames from the direct USB video capture (QGCUvcCamera / UsbUvcCamera) to an MP4 file
 * with the hardware H.264 encoder. Native code converts the frames to the color format returned by
 * start() and calls encode() for each frame from its capture thread.
 */
public class QGCUvcRecorder {
    private static final String TAG = QGCUvcRecorder.class.getSimpleName();
    private static final String MIME = MediaFormat.MIMETYPE_VIDEO_AVC;
    private static final long DEQUEUE_TIMEOUT_US = 10000;

    /** Color formats native code can convert to */
    public static final int COLOR_NV12 = MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar;
    public static final int COLOR_I420 = MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar;

    private static MediaCodec codec = null;
    private static MediaMuxer muxer = null;
    private static MediaCodec.BufferInfo bufferInfo = null;
    private static int trackIndex = -1;
    private static boolean muxerStarted = false;
    private static long startNs = 0;
    private static long lastPtsUs = 0;

    /**
     * @return the color format encode() expects (COLOR_NV12 or COLOR_I420), or -1 on failure
     */
    public static synchronized int start(final String path, final int width, final int height, final int fps, final int rotation) {
        stop();
        try {
            codec = MediaCodec.createEncoderByType(MIME);

            int colorFormat = -1;
            final int[] colorFormats = codec.getCodecInfo().getCapabilitiesForType(MIME).colorFormats;
            for (final int format : colorFormats) {
                if (format == COLOR_NV12) {
                    colorFormat = COLOR_NV12;
                    break;
                }
                if (format == COLOR_I420) {
                    colorFormat = COLOR_I420;
                }
            }
            if (colorFormat == -1) {
                QGCLogger.e(TAG, "H.264 encoder supports neither NV12 nor I420");
                stop();
                return -1;
            }

            final MediaFormat format = MediaFormat.createVideoFormat(MIME, width, height);
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT, colorFormat);
            format.setInteger(MediaFormat.KEY_BIT_RATE, Math.max(2000000, width * height * 8));
            format.setInteger(MediaFormat.KEY_FRAME_RATE, (fps > 0) ? fps : 25);
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            codec.start();

            muxer = new MediaMuxer(path, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            muxer.setOrientationHint(((rotation % 360) + 360) % 360);

            bufferInfo = new MediaCodec.BufferInfo();
            trackIndex = -1;
            muxerStarted = false;
            startNs = System.nanoTime();
            lastPtsUs = 0;
            QGCLogger.i(TAG, "Recording " + width + "x" + height + " fps " + fps + " color " + colorFormat + " to " + path);
            return colorFormat;
        } catch (final Exception e) {
            QGCLogger.e(TAG, "Failed to start recording", e);
            stop();
            return -1;
        }
    }

    /** Encodes one frame in the color format returned by start(). Frames are dropped if the encoder is busy. */
    public static synchronized boolean encode(final byte[] data, final int size) {
        if (codec == null) {
            return false;
        }
        try {
            final int inputIndex = codec.dequeueInputBuffer(DEQUEUE_TIMEOUT_US);
            if (inputIndex >= 0) {
                final ByteBuffer input = codec.getInputBuffer(inputIndex);
                input.clear();
                input.put(data, 0, Math.min(size, input.remaining()));
                lastPtsUs = Math.max(lastPtsUs + 1, (System.nanoTime() - startNs) / 1000);
                codec.queueInputBuffer(inputIndex, 0, input.position(), lastPtsUs, 0);
            }
            drain(false);
            return inputIndex >= 0;
        } catch (final Exception e) {
            QGCLogger.e(TAG, "Encode failed", e);
            return false;
        }
    }

    private static void drain(final boolean endOfStream) {
        int idleLoops = 0;
        while (true) {
            final int outputIndex = codec.dequeueOutputBuffer(bufferInfo, endOfStream ? DEQUEUE_TIMEOUT_US : 0);
            if (outputIndex == MediaCodec.INFO_TRY_AGAIN_LATER) {
                if (!endOfStream || (++idleLoops > 100)) {
                    break;
                }
            } else if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                if (!muxerStarted) {
                    trackIndex = muxer.addTrack(codec.getOutputFormat());
                    muxer.start();
                    muxerStarted = true;
                }
            } else if (outputIndex >= 0) {
                final ByteBuffer output = codec.getOutputBuffer(outputIndex);
                if ((bufferInfo.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                    bufferInfo.size = 0;
                }
                if ((bufferInfo.size > 0) && muxerStarted && (output != null)) {
                    output.position(bufferInfo.offset);
                    output.limit(bufferInfo.offset + bufferInfo.size);
                    muxer.writeSampleData(trackIndex, output, bufferInfo);
                }
                codec.releaseOutputBuffer(outputIndex, false);
                if ((bufferInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                    break;
                }
            }
        }
    }

    public static synchronized void stop() {
        if (codec != null) {
            try {
                final int inputIndex = codec.dequeueInputBuffer(DEQUEUE_TIMEOUT_US);
                if (inputIndex >= 0) {
                    codec.queueInputBuffer(inputIndex, 0, 0, lastPtsUs + 1, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                    drain(true);
                }
            } catch (final Exception e) {
                QGCLogger.w(TAG, "Failed to finish encoding: " + e.getMessage());
            }
            try {
                codec.stop();
            } catch (final Exception e) {
                QGCLogger.w(TAG, "codec.stop failed: " + e.getMessage());
            }
            codec.release();
            codec = null;
        }
        if (muxer != null) {
            try {
                if (muxerStarted) {
                    muxer.stop();
                }
            } catch (final Exception e) {
                QGCLogger.w(TAG, "muxer.stop failed: " + e.getMessage());
            }
            muxer.release();
            muxer = null;
            QGCLogger.i(TAG, "Recording stopped");
        }
        muxerStarted = false;
        trackIndex = -1;
    }
}
