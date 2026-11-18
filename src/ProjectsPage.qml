import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Item {
    id: page
    property string current: ""
    readonly property var shots: backend.project.shots || []

    function renderAll() { if (current !== "" && !backend.busy) backend.renderProject(current) }

    Shortcut { sequence: "R"; enabled: page.visible && !win.typing && shotList.currentIndex >= 0
        onActivated: backend.retryShot(page.current, page.shots[shotList.currentIndex].id, true) }
    Shortcut { sequence: "S"; enabled: page.visible && !win.typing && page.current !== ""; onActivated: backend.stitchProject(page.current) }
    Shortcut { sequence: "G"; enabled: page.visible && !win.typing && page.current !== ""; onActivated: backend.contactSheets(page.current) }

    Component.onCompleted: backend.refreshProjects()

    RowLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        ColumnLayout {
            Layout.preferredWidth: 230
            Layout.fillHeight: true
            Label { text: qsTr("Projects"); font.bold: true; font.pixelSize: 16 }
            ListView {
                id: projectList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: backend.projects
                delegate: ItemDelegate {
                    width: ListView.view.width
                    text: modelData
                    highlighted: modelData === page.current
                    onClicked: { page.current = modelData; backend.loadProject(modelData) }
                }
                Label { anchors.centerIn: parent; visible: projectList.count === 0; text: qsTr("No projects yet"); color: "#80808a" }
            }
            TextField { id: newName; Layout.fillWidth: true; placeholderText: qsTr("New project name")
                validator: RegularExpressionValidator { regularExpression: /[A-Za-z0-9][A-Za-z0-9_.-]{0,63}/ } }
            Button {
                Layout.fillWidth: true
                text: qsTr("Create")
                enabled: newName.text !== ""
                onClicked: { backend.newProject(newName.text); page.current = newName.text; newName.text = "" }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: page.current !== ""
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Label { text: backend.project.title || page.current; font.bold: true; font.pixelSize: 18 }
                Label {
                    color: "#a0a0aa"
                    text: backend.project.width ? qsTr("%1 × %2 · %3 steps · audio %4 · %5 s total")
                          .arg(backend.project.width).arg(backend.project.height).arg(backend.project.steps)
                          .arg(backend.project.audio ? qsTr("on") : qsTr("off"))
                          .arg(Number(backend.project.total_seconds || 0).toLocaleString(Qt.locale(), 'f', 1)) : (backend.project.error || "")
                }
                Item { Layout.fillWidth: true }
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                AccentButton { text: qsTr("Render all  (Space)"); enabled: !backend.busy; onClicked: page.renderAll() }
                Button { text: qsTr("Stitch  (S)"); enabled: !backend.busy; onClicked: backend.stitchProject(page.current) }
                Button { text: qsTr("Contact sheets  (G)"); enabled: !backend.busy; onClicked: backend.contactSheets(page.current) }
                Button { text: qsTr("Edit shots.json"); onClicked: backend.openPath(backend.project.shots_file) }
                Button { text: qsTr("Open folder"); onClicked: backend.openPath(backend.project.dir) }
                Button { text: qsTr("Reload"); onClicked: backend.loadProject(page.current) }
            }

            ListView {
                id: shotList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: page.shots
                currentIndex: -1
                focus: page.visible
                keyNavigationEnabled: true
                delegate: Rectangle {
                    width: ListView.view.width
                    height: col.implicitHeight + 16
                    radius: Math.min(4, theme.radius)
                    color: ListView.isCurrentItem ? "#26262e" : "#18181d"
                    MouseArea { anchors.fill: parent; onClicked: shotList.currentIndex = index }
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        ColumnLayout {
                            id: col
                            Layout.fillWidth: true
                            Label {
                                text: qsTr("Shot %1 · %2 · %3 frames · seed %4").arg(modelData.id).arg(modelData.mode)
                                      .arg(modelData.frames).arg(modelData.seed)
                                font.bold: true
                            }
                            Label { text: modelData.prompt; elide: Text.ElideRight; Layout.fillWidth: true; color: "#b8b8c0"; maximumLineCount: 2; wrapMode: Text.Wrap }
                        }
                        Label {
                            text: modelData.status === "rendered" ? qsTr("rendered") : modelData.status === "stale" ? qsTr("out of date") : qsTr("not rendered")
                            color: modelData.status === "rendered" ? "#3ecf6e" : modelData.status === "stale" ? "#ffcc66" : "#90909a"
                        }
                        Button { text: qsTr("Play"); flat: true; visible: modelData.file !== "" && backend.exists(modelData.file); onClicked: backend.openPath(modelData.file) }
                        Button { text: qsTr("Re-render"); flat: true; enabled: !backend.busy; onClicked: backend.retryShot(page.current, modelData.id, false) }
                        Button { text: qsTr("New seed  (R)"); flat: true; enabled: !backend.busy; onClicked: backend.retryShot(page.current, modelData.id, true) }
                    }
                }
            }

            Label {
                text: (backend.project.videos || []).length ? qsTr("Videos: %1").arg((backend.project.videos || []).map(v => v.split("/").pop()).join(", ")) : qsTr("No stitched video yet")
                color: "#a0a0aa"
            }
            Image {
                visible: !!backend.project.contact_grid
                source: backend.project.contact_grid ? backend.fileUrl(backend.project.contact_grid) : ""
                cache: false
                fillMode: Image.PreserveAspectFit
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 220 : 0
                MouseArea { anchors.fill: parent; onClicked: backend.openPath(backend.project.contact_grid) }
            }
        }
        Label {
            visible: page.current === ""
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            color: "#80808a"
            wrapMode: Text.Wrap
            text: qsTr("Pick or create a project. Each project is a folder with shots.json (prompt, refs, seconds, seed per shot); render, retry, stitch into video_N.mp4 with a manifest.")
        }
    }
}
