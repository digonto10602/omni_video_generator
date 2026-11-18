import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Rectangle {
    id: root
    signal close()
    color: "#d0000000"
    MouseArea { anchors.fill: parent; onClicked: root.close() }

    readonly property var keys: [
        ["?  /  F1", qsTr("Show or hide this help")],
        ["1 2 3", qsTr("Generate, Projects, Gallery")],
        ["Space  /  Ctrl+Enter", qsTr("Generate · render project · play or pause")],
        ["Ctrl+.", qsTr("Cancel the running job")],
        ["C", qsTr("Start or stop ComfyUI")],
        ["R", qsTr("Projects: retry the selected shot with a new seed")],
        ["S", qsTr("Projects: stitch into video_N.mp4")],
        ["G", qsTr("Projects: contact sheets")],
        ["↑ ↓", qsTr("Move through shots or gallery items")],
        ["← →", qsTr("Gallery: seek 5 seconds")],
        ["A", qsTr("About and licence")],
        ["Esc", qsTr("Close overlays, leave a text field")],
        ["Q  /  Ctrl+Q", qsTr("Quit (asks while a job runs)")]
    ]

    Pane {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 560)
        background: Rectangle { color: "#1c1c22"; radius: theme.radius }
        ColumnLayout {
            anchors.fill: parent
            spacing: 6
            Label { text: qsTr("Keyboard"); font.pixelSize: 18; font.bold: true; Layout.bottomMargin: 6 }
            Repeater {
                model: root.keys
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: modelData[0]; color: theme.accent; font.family: "monospace"; Layout.preferredWidth: 190 }
                    Label { text: modelData[1]; Layout.fillWidth: true; wrapMode: Text.Wrap }
                }
            }
        }
    }
}
