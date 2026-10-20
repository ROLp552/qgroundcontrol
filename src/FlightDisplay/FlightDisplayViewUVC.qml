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

Rectangle {
    id:                 _root
    anchors.fill:       parent
    color:              Qt.rgba(0,0,0,0.75)
    clip:               true
    visible:            _videoManager.isUvc

    property var _videoManager:     QGroundControl.videoManager
    property var _videoSettings:    QGroundControl.settingsManager.videoSettings
    property int _rotation:         _videoSettings.uvcRotation.rawValue
    property int _fit:              _videoSettings.uvcFit.rawValue

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
        videoOutput: videoOutput
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
}
