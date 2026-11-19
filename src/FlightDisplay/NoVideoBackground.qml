/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick

import QGroundControl
import QGroundControl.Controls
import QGroundControl.ScreenTools

/// Background shown while there is no video: stock image, optional branding logo and a status text
Image {
    id:         noVideo
    source:     "/res/NoVideoBackground.jpg"
    fillMode:   Image.PreserveAspectCrop

    property string text:           qsTr("WAITING FOR VIDEO")
    property bool   useSmallFont:   false

    // Optional branding logo (branding/branding.qrc, not in git), centered
    Image {
        id:                 companyLogo
        anchors.centerIn:   parent
        height:             Math.min(noVideo.height * 0.7, noVideo.width * 0.9)
        width:              height
        sourceSize.height:  height
        fillMode:           Image.PreserveAspectFit
        source:             "qrc:/branding/company_logo.png"
        visible:            status === Image.Ready
    }

    Item {
        // Centered without a logo, otherwise straddling the top edge of the logo (the bottom of the
        // video area is covered by the telemetry bar)
        anchors.horizontalCenter:   parent.horizontalCenter
        anchors.verticalCenter:     companyLogo.visible ? companyLogo.top : parent.verticalCenter
        width:                      noVideoLabel.contentWidth + ScreenTools.defaultFontPixelHeight
        height:                     noVideoLabel.contentHeight + ScreenTools.defaultFontPixelHeight
        visible:                    noVideo.text !== ""

        Rectangle {
            anchors.fill:   parent
            radius:         ScreenTools.defaultFontPixelWidth / 2
            color:          "black"
            opacity:        0.5
        }

        QGCLabel {
            id:                 noVideoLabel
            text:               noVideo.text
            font.bold:          true
            color:              "white"
            font.pointSize:     noVideo.useSmallFont ? ScreenTools.smallFontPointSize : ScreenTools.largeFontPointSize
            anchors.centerIn:   parent
        }
    }
}
