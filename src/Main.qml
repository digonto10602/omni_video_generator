import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1180
    height: 780
    minimumWidth: 860
    minimumHeight: 560
    visible: true
    title: "Omni Video Generator"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property bool aboutVisible: false
    property bool typing: activeFocusItem && (activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit)

    function switchTab(i) { tabs.currentIndex = i }
    function primaryAction() {
        if (tabs.currentIndex === 0) generatePage.generate()
        else if (tabs.currentIndex === 1) projectsPage.renderAll()
        else galleryPage.togglePlay()
    }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "F1"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.requestQuit() }
    Shortcut { sequence: "Ctrl+Q"; context: Qt.ApplicationShortcut; onActivated: win.requestQuit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.primaryAction() }
    Shortcut { sequence: "Ctrl+Return"; context: Qt.ApplicationShortcut; onActivated: win.primaryAction() }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.switchTab(0) }
    Shortcut { sequence: "2"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.switchTab(1) }
    Shortcut { sequence: "3"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.switchTab(2) }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.aboutVisible = !win.aboutVisible }
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.comfyRunning ? backend.stopComfy() : backend.startComfy() }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut
        onActivated: {
            if (win.helpVisible || win.aboutVisible) { win.helpVisible = false; win.aboutVisible = false }
            else if (win.activeFocusItem) win.contentItem.forceActiveFocus()
        }
    }
    Shortcut { sequence: "Ctrl+."; context: Qt.ApplicationShortcut; onActivated: backend.cancel() }

    function requestQuit() {
        if (backend.busy) quitDialog.open()
        else Qt.quit()
    }
    onClosing: (close) => { if (backend.busy) { close.accepted = false; quitDialog.open() } }

    Dialog {
        id: quitDialog
        anchors.centerIn: parent
        modal: true
        title: qsTr("A job is running")
        Label { text: qsTr("Quitting cancels it and frees the GPU. Quit anyway?") }
        standardButtons: Dialog.Yes | Dialog.No
        onAccepted: Qt.quit()
    }

    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Header: tabs, ComfyUI and GPU state.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                color: "#16161a"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 12
                    spacing: 12
                    Label { text: "Omni Video Generator"; font.pixelSize: 17; font.bold: true }
                    TabBar {
                        id: tabs
                        Layout.preferredWidth: 380
                        background: Item {}
                        TabButton { text: qsTr("Generate") }
                        TabButton { text: qsTr("Projects") }
                        TabButton { text: qsTr("Gallery") }
                    }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        width: 10; height: 10; radius: 5
                        color: backend.comfyRunning ? "#3ecf6e" : "#77777f"
                    }
                    Label {
                        text: backend.comfyText
                        color: "#c8c8d0"
                        elide: Text.ElideRight
                        Layout.maximumWidth: 360
                    }
                    Button {
                        flat: true
                        text: backend.comfyRunning ? qsTr("Stop ComfyUI") : qsTr("Start ComfyUI")
                        enabled: !backend.busy && (!backend.comfyRunning || backend.comfyOurs)
                        onClicked: backend.comfyRunning ? backend.stopComfy() : backend.startComfy()
                    }
                    Button { flat: true; text: "?"; onClicked: win.helpVisible = true }
                }
            }

            Rectangle {
                visible: backend.installProblem !== "" || backend.gpuBusy
                Layout.fillWidth: true
                Layout.preferredHeight: problemLabel.implicitHeight + 16
                color: "#3a2a10"
                Label {
                    id: problemLabel
                    anchors.fill: parent
                    anchors.margins: 8
                    anchors.leftMargin: 16
                    wrapMode: Text.Wrap
                    text: backend.installProblem !== "" ? backend.installProblem
                                                        : qsTr("GPU in use by another app; new jobs will wait for it.")
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: tabs.currentIndex
                GeneratePage { id: generatePage }
                ProjectsPage { id: projectsPage }
                GalleryPage { id: galleryPage }
            }

            // Footer: job progress, always visible.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                color: "#16161a"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 12
                    spacing: 12
                    BusyIndicator { running: backend.busy; visible: backend.busy; Layout.preferredWidth: 32; Layout.preferredHeight: 32 }
                    Label {
                        text: backend.lastError !== "" && !backend.busy ? backend.lastError
                              : backend.statusText !== "" ? backend.statusText : qsTr("Idle")
                        color: backend.lastError !== "" && !backend.busy ? "#ff8a80"
                               : backend.waitingForGpu ? "#ffcc66" : "#d8d8e0"
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    ProgressBar { value: backend.progress; visible: backend.busy; Layout.preferredWidth: 220 }
                    Label { text: backend.etaText; visible: backend.busy; color: "#a0a0aa" }
                    Button { text: qsTr("Cancel"); visible: backend.busy; onClicked: backend.cancel() }
                    Button { flat: true; text: qsTr("About"); onClicked: win.aboutVisible = true }
                }
            }
        }

        HelpOverlay { anchors.fill: parent; visible: win.helpVisible; onClose: win.helpVisible = false }

        Rectangle {
            anchors.fill: parent
            visible: win.aboutVisible
            color: "#cc000000"
            MouseArea { anchors.fill: parent; onClicked: win.aboutVisible = false }
            Pane {
                anchors.centerIn: parent
                width: Math.min(parent.width - 64, 620)
                Material.elevation: 8
                background: Rectangle { color: "#1c1c22"; radius: theme.radius }
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10
                    Label { text: "Omni Video Generator " + backend.version; font.pixelSize: 18; font.bold: true }
                    Label { text: qsTr("Build %1 · runs MiniMax H3 locally through ComfyUI; nothing is sent to any cloud service.").arg(backend.gitCommit); wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: "#ffcc66"
                        text: qsTr("Powered by MiniMax H3 · weights under the MiniMax H3 Community License: not licensed for use in the US, EU, UK or South Korea; over US$20M yearly revenue needs MiniMax's written permission.")
                    }
                    Label { text: "https://huggingface.co/MiniMaxAI/MiniMax-H3/blob/main/LICENSE"; color: "#9aa"; font.pixelSize: 12 }
                    Label { text: qsTr("Data folder: %1").arg(backend.homeDir); color: "#9aa"; font.pixelSize: 12 }
                    Button { text: qsTr("Close"); Layout.alignment: Qt.AlignRight; onClicked: win.aboutVisible = false }
                }
            }
        }
    }
}
