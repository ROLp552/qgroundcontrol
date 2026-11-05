/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick
import QtMultimedia

import QGroundControl
import QGroundControl.Controls
import QGroundControl.ScreenTools

Rectangle {
    id:                 _root
    anchors.fill:       parent
    color:              Qt.rgba(0,0,0,0.75)
    clip:               true
    visible:            _videoManager.isUvc || _isUsbUvc

    property var  _videoManager:    QGroundControl.videoManager
    property var  _videoSettings:   QGroundControl.settingsManager.videoSettings
    property var  _usbUvcCamera:    _videoManager.usbUvcCamera
    property bool _isUsbUvc:        _videoManager.isUsbUvc
    // Each source keeps its own rotation/fit (e.g. AX12 HDMI input vs a USB analog grabber)
    property int  _rotation:        _isUsbUvc ? _videoSettings.usbUvcRotation.rawValue : _videoSettings.uvcRotation.rawValue
    property int  _fit:             _isUsbUvc ? _videoSettings.usbUvcFit.rawValue : _videoSettings.uvcFit.rawValue

    // Direct USB capture: the C++ side pushes the frames into the VideoOutput's sink
    function _updateUsbUvc() {
        if (!_usbUvcCamera) {
            return
        }
        if (_isUsbUvc) {
            _usbUvcCamera.start(videoOutput.videoSink)
        } else {
            _usbUvcCamera.stop()
        }
    }

    on_IsUsbUvcChanged:     _updateUsbUvc()

    property int _usbUvcFormat: _videoSettings.usbUvcFormat.rawValue
    on_UsbUvcFormatChanged: {
        if (_usbUvcCamera && _isUsbUvc) {
            _usbUvcCamera.stop()
            _usbUvcCamera.start(videoOutput.videoSink)
        }
    }
    Component.onCompleted:  _updateUsbUvc()

    MediaDevices {
        id: mediaDevices

        function findCameraDevice(cameraId) {
            var videoInputs = mediaDevices.videoInputs
            for (var i = 0; i < videoInputs.length; i++) {
                if (videoInputs[i].description === cameraId) {
                    return videoInputs[i]
                }
            }
            return mediaDevices.defaultVideoInput
        }
    }

    CaptureSession {
        camera: Camera {
            id:             camera
            cameraDevice:   mediaDevices.findCameraDevice(_videoManager.uvcVideoSourceID)
            active:         _videoManager.isUvc
        }
        videoOutput: _isUsbUvc ? null : videoOutput
        recorder:    recorder
    }

    // Local video devices are not handled by the video receivers, so the Rec button is routed here
    MediaRecorder {
        id:         recorder
        quality:    MediaRecorder.HighQuality
        mediaFormat {
            fileFormat: MediaFormat.MPEG4
            videoCodec: MediaFormat.VideoCodec.H264
        }

        onRecorderStateChanged: _videoManager.uvcRecordingStateChanged(recorderState === MediaRecorder.RecordingState, actualLocation.toString())
        onErrorOccurred: (error, errorString) => _videoManager.uvcRecordingError(errorString)
    }

    Connections {
        target: _videoManager

        function onUvcStartRecordingRequested(fileUrl) {
            if (recorder.recorderState === MediaRecorder.StoppedState) {
                recorder.outputLocation = fileUrl
                recorder.record()
            }
        }

        function onUvcStopRecordingRequested() {
            recorder.stop()
        }
    }

    Component.onDestruction: {
        if (_usbUvcCamera && _isUsbUvc) {
            _usbUvcCamera.stop()
        }
        if (recorder.recorderState !== MediaRecorder.StoppedState) {
            recorder.stop()
            _videoManager.uvcRecordingStateChanged(false, "")
        }
    }

    // Some devices (e.g. RadioMaster AX12 HDMI input) expose the video input as a phone
    // camera with a sensor orientation, which shows the image rotated. VideoOutput applies
    // the extra rotation and keeps the aspect ratio correct for the selected fill mode.
    VideoOutput {
        id:             videoOutput
        anchors.fill:   parent
        orientation:    _rotation
        fillMode:       _fit === 1 ? VideoOutput.PreserveAspectCrop :
                        (_fit === 2 ? VideoOutput.Stretch : VideoOutput.PreserveAspectFit)
    }

    QGCLabel {
        anchors.centerIn:   parent
        text:               _usbUvcCamera ? _usbUvcCamera.status : ""
        visible:            _isUsbUvc && _usbUvcCamera && !_usbUvcCamera.streaming
        color:              "white"
        font.pointSize:     ScreenTools.largeFontPointSize
    }

    // Diagnostics for the direct USB capture (Video settings). Tap to switch GPU/CPU color conversion.
    Rectangle {
        anchors.left:       parent.left
        anchors.right:      parent.right
        anchors.top:        parent.top
        height:             debugLabel.height + ScreenTools.defaultFontPixelHeight * 0.5
        color:              Qt.rgba(0, 0, 0, 0.6)
        visible:            _isUsbUvc && _usbUvcCamera && _videoSettings.usbUvcDiagnostics.rawValue

        QGCLabel {
            id:                 debugLabel
            anchors.left:       parent.left
            anchors.right:      parent.right
            anchors.margins:    ScreenTools.defaultFontPixelWidth
            anchors.verticalCenter: parent.verticalCenter
            text:               _usbUvcCamera ? _usbUvcCamera.debugInfo : ""
            color:              "yellow"
            wrapMode:           Text.WordWrap
        }

        MouseArea {
            anchors.fill:   parent
            onClicked:      _usbUvcCamera.cpuConvert = !_usbUvcCamera.cpuConvert
        }
    }
}
