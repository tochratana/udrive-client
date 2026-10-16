import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Templates as T
import QtQuick.Layouts
import org.ownCloud.gui 1.0
import org.ownCloud.libsync 1.0

Pane {
    id: sidebar
    readonly property SettingsDialog settingsDialog: ocContext
    readonly property OCQuickWidget widget: ocQuickWidget
    readonly property bool dark: palette.window.hslLightness < 0.5
    readonly property bool hasAccount: AccountManager.accounts.length > 0
    padding: 10
    Accessible.name: qsTr("UDrive navigation")
    background: Rectangle {
        radius: 16
        color: sidebar.dark ? "#272727" : "#FFFFFF"
        border.color: sidebar.dark ? "#414141" : "#E1E5E8"
    }
    Connections {
        target: widget
        function onFocusFirst() { if (newButton.enabled) newButton.forceActiveFocus(Qt.TabFocusReason); else addAccountButton.forceActiveFocus(Qt.TabFocusReason); }
        function onFocusLast() { quitButton.forceActiveFocus(Qt.BacktabFocusReason); }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 6
        opacity: widget.enabled ? 1 : 0.5
        T.Button {
            id: newButton
            objectName: "udriveNew"
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            Layout.topMargin: 32
            Layout.bottomMargin: 14
            text: qsTr("+  New")
            font.pixelSize: 14
            font.weight: Font.Bold
            hoverEnabled: true
            enabled: settingsDialog.canCreateFiles
            Accessible.name: qsTr("Create a folder or upload a file")
            background: Rectangle {
                radius: 9
                color: !newButton.enabled ? (sidebar.dark ? "#353B37" : "#E4EAE6") : newButton.down ? "#30C86A" : newButton.hovered ? "#63E493" : "#40D979"
                border.width: newButton.visualFocus ? 2 : 0
                border.color: "#007C58"
            }
            contentItem: Label {
                text: newButton.text
                font: newButton.font
                color: newButton.enabled ? "#FFFFFF" : (sidebar.dark ? "#9AA49E" : "#68776D")
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            Keys.onBacktabPressed: widget.parentFocusWidget.focusPrevious()
            onClicked: settingsDialog.openNewFileMenu()
        }
        UDriveNavigationButton {
            id: personalButton
            objectName: "udrivePersonal"
            Layout.fillWidth: true
            Accessible.role: Accessible.PageTab
            checked: settingsDialog.currentPage === SettingsDialog.Files
            enabled: sidebar.hasAccount
            icon.source: "qrc:/udrive/icons/nav-folder.svg"
            text: qsTr("Personal")
            onClicked: settingsDialog.currentPage = SettingsDialog.Files
        }
        UDriveNavigationButton {
            objectName: "udriveFavorites"
            Layout.fillWidth: true
            Accessible.role: Accessible.PageTab
            checked: settingsDialog.currentPage === SettingsDialog.Favorites
            enabled: sidebar.hasAccount
            icon.source: "qrc:/udrive/icons/nav-star.svg"
            text: qsTr("Favorites")
            onClicked: settingsDialog.currentPage = SettingsDialog.Favorites
        }
        UDriveNavigationButton {
            objectName: "udriveShares"
            Layout.fillWidth: true
            Accessible.role: Accessible.PageTab
            checked: settingsDialog.currentPage === SettingsDialog.Shares
            enabled: sidebar.hasAccount
            icon.source: "qrc:/udrive/icons/nav-share.svg"
            text: qsTr("Shares")
            onClicked: settingsDialog.currentPage = SettingsDialog.Shares
        }
        UDriveNavigationButton {
            objectName: "udriveWorkspaces"
            Layout.fillWidth: true
            Accessible.role: Accessible.PageTab
            checked: settingsDialog.currentPage === SettingsDialog.Workspaces
            enabled: sidebar.hasAccount
            icon.source: "qrc:/udrive/icons/nav-grid.svg"
            text: qsTr("Workspaces")
            onClicked: settingsDialog.currentPage = SettingsDialog.Workspaces
        }
        UDriveNavigationButton {
            objectName: "udriveTrash"
            Layout.fillWidth: true
            Accessible.role: Accessible.PageTab
            checked: settingsDialog.currentPage === SettingsDialog.Trash
            enabled: sidebar.hasAccount
            icon.source: "qrc:/udrive/icons/nav-trash.svg"
            text: qsTr("Deleted files")
            onClicked: settingsDialog.currentPage = SettingsDialog.Trash
        }
        UDriveNavigationButton {
            id: addAccountButton
            Layout.fillWidth: true
            visible: !sidebar.hasAccount
            icon.source: "qrc:/udrive/icons/nav-add.svg"
            text: qsTr("Add account")
            onClicked: settingsDialog.createNewAccount()
        }
        Item { Layout.fillHeight: true }
        // Desktop operations remain available without dominating file navigation.
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            UDriveNavigationButton {
                Layout.fillWidth: true
                icon.source: "qrc:/udrive/icons/nav-sync.svg"
                text: qsTr("Sync")
                checked: settingsDialog.currentPage === SettingsDialog.Account
                enabled: sidebar.hasAccount
                onClicked: settingsDialog.currentPage = SettingsDialog.Account
            }
            UDriveNavigationButton {
                id: quitButton
                Layout.preferredWidth: 42
                icon.source: "qrc:/udrive/icons/nav-quit.svg"
                Accessible.name: qsTr("Quit UDrive")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Quit UDrive")
                Keys.onTabPressed: widget.parentFocusWidget.focusNext()
                onClicked: Qt.quit()
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.bottomMargin: 10
            height: 1
            color: sidebar.dark ? "#414141" : "#E1E5E8"
        }
        RowLayout {
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            IconImage {
                color: sidebar.dark ? "#CACCCD" : "#39443F"
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
                source: "qrc:/udrive/icons/nav-cloud.svg"
            }
            Label { text: qsTr("Storage"); color: sidebar.dark ? "#CACCCD" : "#39443F"; font.pixelSize: 13 }
        }
        T.ProgressBar {
            Layout.fillWidth: true
            Layout.preferredHeight: 5
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            from: 0
            to: 1
            value: settingsDialog.storageFraction
            Accessible.name: qsTr("Storage usage")
            background: Rectangle { implicitHeight: 5; radius: 3; color: sidebar.dark ? "#494949" : "#E1E5E8" }
            contentItem: Item { implicitHeight: 5; Rectangle { width: parent.width * settingsDialog.storageFraction; height: parent.height; radius: 3; color: "#40D979" } }
        }
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            Layout.bottomMargin: 10
            textFormat: Text.PlainText
            text: settingsDialog.storageLabel
            color: sidebar.dark ? "#CACCCD" : "#39443F"
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }
}
