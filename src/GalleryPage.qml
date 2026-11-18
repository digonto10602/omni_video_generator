import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtMultimedia

Item {
    id: page
    property var current: list.currentIndex >= 0 ? backend.gallery[list.currentIndex] : null

    function togglePlay() {
        if (!current) return
        player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play()
    }
    Shortcut { sequence: "Left"; enabled: page.visible && !win.typing; onActivated: player.position = Math.max(0, player.position - 5000) }
    Shortcut { sequence: "Right"; enabled: page.visible && !win.typing; onActivated: player.position = Math.min(player.duration, player.position + 5000) }
    onVisibleChanged: { if (visible) backend.refreshGallery(); else player.pause() }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        ListView {
            id: list
            Layout.preferredWidth: 330
            Layout.fillHeight: true
            clip: true
            model: backend.gallery
            focus: page.visible
            keyNavigationEnabled: true
            delegate: ItemDelegate {
                width: ListView.view.width
                highlighted: ListView.isCurrentItem
                onClicked: list.currentIndex = index
                contentItem: ColumnLayout {
                    Label { text: modelData.name; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    Label {
                        text: (modelData.kind === "video" ? qsTr("stitched video") : modelData.kind === "shot" ? qsTr("shot") : qsTr("clip"))
                              + (modelData.project ? " · " + modelData.project : "")
                              + (modelData.seed ? " · " + qsTr("seed %1").arg(modelData.seed) : "")
                        color: "#90909a"; font.pixelSize: 12
                    }
                }
            }
            Label { anchors.centerIn: parent; visible: list.count === 0; text: qsTr("Nothing generated yet"); color: "#80808a" }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "black"
                radius: theme.radius
                VideoOutput { id: videoOut; anchors.fill: parent }
                MediaPlayer {
                    id: player
                    source: page.current ? backend.fileUrl(page.current.file) : ""
                    videoOutput: videoOut
                    audioOutput: AudioOutput {}
                    loops: MediaPlayer.Infinite
                }
                MouseArea { anchors.fill: parent; onClicked: page.togglePlay() }
                Label { anchors.centerIn: parent; visible: !page.current; text: qsTr("Select a clip"); color: "#80808a" }
            }
            RowLayout {
                Layout.fillWidth: true
                Button { text: player.playbackState === MediaPlayer.PlayingState ? qsTr("Pause") : qsTr("Play"); enabled: !!page.current; onClicked: page.togglePlay() }
                Slider {
                    Layout.fillWidth: true
                    from: 0; to: Math.max(1, player.duration)
                    value: player.position
                    onMoved: player.position = value
                }
                Label { text: (player.position / 1000).toLocaleString(Qt.locale(), 'f', 1) + " / " + (player.duration / 1000).toLocaleString(Qt.locale(), 'f', 1) + " s" }
                Button { text: qsTr("Open folder"); enabled: !!page.current; onClicked: backend.openPath(page.current.file.substring(0, page.current.file.lastIndexOf("/"))) }
            }
            Label {
                visible: !!page.current && !!page.current.prompt
                text: page.current ? (page.current.prompt || "") : ""
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                maximumLineCount: 4
                elide: Text.ElideRight
                color: "#b8b8c0"
            }
        }
    }
}
