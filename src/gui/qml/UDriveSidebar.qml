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
    readonly property bool collapsed: !settingsDialog.sidebarVisible
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
        function onFocusLast() {
            if (!sidebar.collapsed)
                quitButton.forceActiveFocus(Qt.BacktabFocusReason);
            else
                personalButton.forceActiveFocus(Qt.BacktabFocusReason);
        }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 6
        opacity: widget.enabled ? 1 : 0.5
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            Layout.topMargin: 2
            Layout.bottomMargin: 4
            Item { Layout.fillWidth: true }
            T.Button {
                id: collapseButton
                objectName: "udriveCollapse"
                Layout.preferredWidth: 26
                Layout.preferredHeight: 26
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.name: sidebar.collapsed ? qsTr("Expand sidebar") : qsTr("Collapse sidebar")
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: (sidebar.collapsed ? qsTr("Expand sidebar (%1)") : qsTr("Collapse sidebar (%1)")).arg(Qt.platform.os === "osx" ? "⌘B" : "Ctrl+B")
                background: Rectangle {
                    radius: 6
                    color: collapseButton.down ? (sidebar.dark ? "#3A3A3A" : "#E2E5E8")
                        : collapseButton.hovered ? (sidebar.dark ? "#323232" : "#ECEFF1")
                        : "transparent"
                }
                contentItem: IconImage {
                    anchors.centerIn: parent
                    source: "qrc:/udrive/icons/chevron-dark.svg"
                    sourceSize: Qt.size(16, 16)
                    rotation: sidebar.collapsed ? 270 : 90
                    color: collapseButton.hovered
                        ? (sidebar.dark ? "#FFFFFF" : "#1A201C")
                        : (sidebar.dark ? "#9AA49E" : "#68776D")
                }
                Keys.onBacktabPressed: widget.parentFocusWidget.focusPrevious()
                onClicked: settingsDialog.toggleSidebar()
            }
            Item {
                visible: sidebar.collapsed
                Layout.fillWidth: sidebar.collapsed
            }
        }
        T.Button {
            id: newButton
            objectName: "udriveNew"
            Layout.preferredWidth: sidebar.collapsed ? 36 : -1
            Layout.fillWidth: !sidebar.collapsed
            Layout.preferredHeight: 38
            Layout.alignment: sidebar.collapsed ? Qt.AlignHCenter : Qt.AlignLeft
            Layout.topMargin: 4
            Layout.bottomMargin: 14
            text: settingsDialog.isWorkspacesOverview ? qsTr("+  New Workspace") : qsTr("+  New")
            hoverEnabled: true
            enabled: settingsDialog.canCreateFiles
            Accessible.name: settingsDialog.isWorkspacesOverview ? qsTr("Create a new workspace") : qsTr("Create a folder or upload a file")
            ToolTip.visible: sidebar.collapsed && hovered
            ToolTip.delay: 300
            ToolTip.text: settingsDialog.isWorkspacesOverview ? qsTr("New Workspace") : qsTr("New")
            background: Rectangle {
                radius: sidebar.collapsed ? 8 : 9
                color: !newButton.enabled ? (sidebar.dark ? "#353B37" : "#E4EAE6")
                    : newButton.down ? "#16A34A"
                    : newButton.hovered ? "#2AD564" : "#22C55E"
                border.width: newButton.visualFocus ? 2 : 0
                border.color: "#007C58"
            }
            contentItem: Label {
                text: sidebar.collapsed ? "+" : newButton.text
                font.pixelSize: sidebar.collapsed ? 20 : 14
                font.weight: Font.Bold
                color: newButton.enabled ? "#FFFFFF" : (sidebar.dark ? "#9AA49E" : "#68776D")
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            Keys.onBacktabPressed: collapseButton.forceActiveFocus(Qt.BacktabFocusReason)
            onClicked: settingsDialog.openNewFileMenu()
        }
        UDriveNavigationButton {
            id: personalButton
            objectName: "udrivePersonal"
            collapsed: sidebar.collapsed
            Layout.preferredWidth: sidebar.collapsed ? 36 : -1
            Layout.fillWidth: !sidebar.collapsed
            Layout.alignment: sidebar.collapsed ? Qt.AlignHCenter : Qt.AlignLeft
            Accessible.role: Accessible.PageTab
            checked: settingsDialog.currentPage === SettingsDialog.Files
            enabled: sidebar.hasAccount
            icon.source: "qrc:/udrive/icons/nav-folder.svg"
            text: qsTr("Personal")
            onClicked: settingsDialog.currentPage = SettingsDialog.Files
        }
        UDriveNavigationButton {
            objectName: "udriveFavorites"
            collapsed: sidebar.collapsed
            Layout.preferredWidth: sidebar.collapsed ? 36 : -1
            Layout.fillWidth: !sidebar.collapsed
            Layout.alignment: sidebar.collapsed ? Qt.AlignHCenter : Qt.AlignLeft
            Accessible.role: Accessible.PageTab
            checked: settingsDialog.currentPage === SettingsDialog.Favorites
            enabled: sidebar.hasAccount
            icon.source: "qrc:/udrive/icons/nav-star.svg"
            text: qsTr("Favorites")
            onClicked: settingsDialog.currentPage = SettingsDialog.Favorites
        }
        UDriveNavigationButton {
            objectName: "udriveShares"
            collapsed: sidebar.collapsed
            Layout.preferredWidth: sidebar.collapsed ? 36 : -1
            Layout.fillWidth: !sidebar.collapsed
            Layout.alignment: sidebar.collapsed ? Qt.AlignHCenter : Qt.AlignLeft
            Accessible.role: Accessible.PageTab
            checked: settingsDialog.currentPage === SettingsDialog.Shares
            enabled: sidebar.hasAccount
            icon.source: "qrc:/udrive/icons/nav-share.svg"
            text: qsTr("Shares")
            onClicked: settingsDialog.currentPage = SettingsDialog.Shares
        }
        UDriveNavigationButton {
            objectName: "udriveWorkspaces"
            collapsed: sidebar.collapsed
            Layout.preferredWidth: sidebar.collapsed ? 36 : -1
            Layout.fillWidth: !sidebar.collapsed
            Layout.alignment: sidebar.collapsed ? Qt.AlignHCenter : Qt.AlignLeft
            Accessible.role: Accessible.PageTab
            checked: settingsDialog.currentPage === SettingsDialog.Workspaces
            enabled: sidebar.hasAccount
            icon.source: "qrc:/udrive/icons/nav-grid.svg"
            text: qsTr("Workspaces")
            onClicked: settingsDialog.currentPage = SettingsDialog.Workspaces
        }
        UDriveNavigationButton {
            objectName: "udriveTrash"
            collapsed: sidebar.collapsed
            Layout.preferredWidth: sidebar.collapsed ? 36 : -1
            Layout.fillWidth: !sidebar.collapsed
            Layout.alignment: sidebar.collapsed ? Qt.AlignHCenter : Qt.AlignLeft
            Accessible.role: Accessible.PageTab
            checked: settingsDialog.currentPage === SettingsDialog.Trash
            enabled: sidebar.hasAccount
            icon.source: "qrc:/udrive/icons/nav-trash.svg"
            text: qsTr("Deleted files")
            onClicked: settingsDialog.currentPage = SettingsDialog.Trash
        }
        UDriveNavigationButton {
            id: addAccountButton
            collapsed: sidebar.collapsed
            Layout.preferredWidth: sidebar.collapsed ? 36 : -1
            Layout.fillWidth: !sidebar.collapsed
            Layout.alignment: sidebar.collapsed ? Qt.AlignHCenter : Qt.AlignLeft
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
            visible: !sidebar.collapsed
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
            visible: !sidebar.collapsed
            color: sidebar.dark ? "#414141" : "#E1E5E8"
        }
        RowLayout {
            Layout.alignment: sidebar.collapsed ? Qt.AlignHCenter : Qt.AlignLeft
            Layout.leftMargin: sidebar.collapsed ? 0 : 10
            Layout.rightMargin: sidebar.collapsed ? 0 : 10
            Layout.bottomMargin: sidebar.collapsed ? 4 : 0
            IconImage {
                color: sidebar.dark ? "#CACCCD" : "#39443F"
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
                source: "qrc:/udrive/icons/nav-cloud.svg"
                ToolTip.visible: sidebar.collapsed && storageMouseArea.containsMouse
                ToolTip.delay: 300
                ToolTip.text: settingsDialog.storageLabel
                MouseArea {
                    id: storageMouseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: sidebar.collapsed
                }
            }
            Label {
                visible: !sidebar.collapsed
                text: qsTr("Storage")
                color: sidebar.dark ? "#CACCCD" : "#39443F"
                font.pixelSize: 13
            }
        }
        T.ProgressBar {
            visible: !sidebar.collapsed
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
            visible: !sidebar.collapsed
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
