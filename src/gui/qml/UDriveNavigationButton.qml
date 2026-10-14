import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import QtQuick.Templates as T

T.Button {
    id: control
    property string initials: ""
    property string detail: ""
    property url avatarSource: ""
    readonly property bool dark: palette.window.hslLightness < 0.5
    readonly property color textColor: dark ? "#CACCCD" : "#39443F"

    implicitHeight: detail.length ? 60 : 38
    leftPadding: text.length ? 12 : 8
    rightPadding: text.length ? 12 : 8
    topPadding: 8
    bottomPadding: 8
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: detail.length ? text + ", " + detail : text

    background: Rectangle {
        radius: 7
        color: control.checked ? (control.dark ? "#2D4233" : "#DDF5E5")
            : control.down ? (control.dark ? "#353535" : "#E1E7E3")
            : control.hovered ? (control.dark ? "#353535" : "#EAEDEE") : "transparent"
        border.width: control.visualFocus ? 2 : control.checked ? 1 : 0
        border.color: control.visualFocus ? "#40D979" : "#377848"
    }

    contentItem: RowLayout {
        spacing: control.text.length ? 12 : 0
        opacity: control.enabled ? 1 : 0.45
        Rectangle {
            Layout.preferredWidth: 24
            Layout.preferredHeight: 24
            radius: 10
            color: control.initials.length ? "#007C58" : "transparent"
            Image {
                anchors.fill: parent
                visible: control.initials.length
                fillMode: Image.PreserveAspectFit
                source: control.avatarSource
                sourceSize: Qt.size(30, 30)
                cache: false
            }
            IconImage {
                anchors.fill: parent
                anchors.margins: 2
                visible: !control.initials.length
                source: control.icon.source
                sourceSize: Qt.size(22, 22)
                color: control.checked ? (control.dark ? "#49DA78" : "#087F3C") : control.textColor
            }
            Label {
                anchors.centerIn: parent
                textFormat: Text.PlainText
                visible: control.initials.length && !control.avatarSource.toString().length
                text: control.initials.toUpper()
                font.bold: true
                color: "white"
            }
        }
        ColumnLayout {
            visible: control.text.length || control.detail.length
            Layout.fillWidth: true
            spacing: 3
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: control.text
                color: control.checked ? (control.dark ? "#49DA78" : "#087F3C") : control.textColor
                font.pixelSize: 13
                font.weight: control.checked ? Font.DemiBold : Font.Normal
                elide: Text.ElideRight
            }
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                visible: control.detail.length
                text: control.detail
                color: control.dark ? "#B2CFC3" : "#517366"
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }
    }
}
