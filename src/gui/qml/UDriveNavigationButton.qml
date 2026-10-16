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
    property bool collapsed: false
    readonly property bool dark: palette.window.hslLightness < 0.5
    readonly property color textColor: dark ? "#CACCCD" : "#39443F"

    implicitHeight: control.collapsed ? 36 : detail.length ? 60 : 38
    implicitWidth: control.collapsed ? 36 : -1
    leftPadding: control.collapsed ? 0 : text.length ? 12 : 8
    rightPadding: control.collapsed ? 0 : text.length ? 12 : 8
    topPadding: control.collapsed ? 0 : 8
    bottomPadding: control.collapsed ? 0 : 8
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: detail.length ? text + ", " + detail : text
    ToolTip.visible: control.collapsed && control.hovered
    ToolTip.delay: 300
    ToolTip.text: control.detail.length ? control.text + " (" + control.detail + ")" : control.text

    background: Rectangle {
        radius: 7
        color: control.down ? (control.dark ? "#25382D" : "#C8ECD3")
            : (control.checked || control.hovered) ? (control.dark ? "#2E4436" : "#DDF5E5") : "transparent"
        border.width: control.visualFocus ? 2 : (control.checked || control.hovered || control.down) ? 1 : 0
        border.color: control.visualFocus ? "#40D979" : (control.dark ? "#346D49" : "#377848")
    }

    contentItem: RowLayout {
        spacing: control.collapsed ? 0 : control.text.length ? 12 : 0
        opacity: control.enabled ? 1 : 0.45
        Item {
            visible: control.collapsed
            Layout.fillWidth: control.collapsed
        }
        Rectangle {
            Layout.alignment: Qt.AlignCenter
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
                color: (control.checked || control.hovered || control.down) ? (control.dark ? "#4ADE80" : "#087F3C") : control.textColor
            }
            Label {
                anchors.centerIn: parent
                textFormat: Text.PlainText
                visible: control.initials.length && !control.avatarSource.toString().length
                text: control.initials.toUpperCase()
                font.bold: true
                color: "white"
            }
        }
        Item {
            visible: control.collapsed
            Layout.fillWidth: control.collapsed
        }
        ColumnLayout {
            visible: !control.collapsed && (control.text.length || control.detail.length)
            Layout.fillWidth: true
            spacing: 3
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: control.text
                color: (control.checked || control.hovered || control.down) ? (control.dark ? "#4ADE80" : "#087F3C") : control.textColor
                font.pixelSize: 13
                font.weight: Font.Bold
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
