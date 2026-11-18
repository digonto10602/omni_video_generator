import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Item {
    id: page
    property string mode: "t2v"
    property var refs: []
    readonly property int frames: backend.framesFor(secondsBox.realValue)
    readonly property bool overCap: widthBox.value * heightBox.value > backend.maxPixels
    readonly property bool refsOk: mode === "t2v" ? refs.length === 0 : mode === "i2v" ? refs.length === 1 : refs.length <= 9

    function generate() {
        if (backend.busy || prompt.text.trim() === "" || !refsOk) return
        backend.generate({
            mode: page.mode, prompt: prompt.text, refs: page.refs,
            width: widthBox.value, height: heightBox.value,
            seconds: secondsBox.realValue, steps: stepsBox.value,
            seed: randomSeed.checked ? -1 : Number(seedField.text), audio: audioSwitch.checked
        })
    }
    function addRefs(paths) {
        let list = page.mode === "i2v" ? [] : page.refs.slice()
        for (const p of paths) if (list.indexOf(p) < 0) list.push(p)
        if (page.mode === "i2v") list = list.slice(0, 1)
        page.refs = list.slice(0, 9)
    }

    Connections {
        target: backend
        function onImagesPicked(paths) { if (paths.length) page.addRefs(paths) }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: 14
            anchors.margins: 20

            Item { Layout.preferredHeight: 4 }

            RowLayout {
                Layout.leftMargin: 20; Layout.rightMargin: 20
                spacing: 8
                Repeater {
                    model: [["t2v", qsTr("Text to video")], ["i2v", qsTr("Image to video")], ["r2v", qsTr("Reference to video")]]
                    Button {
                        text: modelData[1]
                        checkable: true
                        checked: page.mode === modelData[0]
                        highlighted: checked
                        onClicked: { page.mode = modelData[0]; if (page.mode === "t2v") page.refs = []; else if (page.mode === "i2v") page.refs = page.refs.slice(0, 1) }
                    }
                }
                Item { Layout.fillWidth: true }
            }

            Label {
                Layout.leftMargin: 20; Layout.rightMargin: 20
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#a0a0aa"
                text: page.mode === "t2v" ? qsTr("Describe the shot, motion and sound in one block. Uses the fl2va model.")
                    : page.mode === "i2v" ? qsTr("The image becomes the first frame; refer to it as <Picture 1>. Uses the fl2va model.")
                    : qsTr("Up to 9 reference images; name them in the prompt as <Picture 1>, <Picture 2>... Uses the ref2va model.")
            }

            TextArea {
                id: prompt
                Layout.leftMargin: 20; Layout.rightMargin: 20
                Layout.fillWidth: true
                Layout.preferredHeight: 150
                wrapMode: TextEdit.Wrap
                placeholderText: qsTr("Prompt: scene, camera, motion, and audio (dialogue, effects, music)")
            }
            Label {
                Layout.leftMargin: 20
                color: "#80808a"
                font.pixelSize: 12
                text: qsTr("No negative prompt: H3's graph uses a guider without CFG, so there is nothing to feed one into.")
            }

            // Reference / first-frame images.
            ColumnLayout {
                visible: page.mode !== "t2v"
                Layout.leftMargin: 20; Layout.rightMargin: 20
                Layout.fillWidth: true
                RowLayout {
                    Label { text: page.mode === "i2v" ? qsTr("First frame") : qsTr("Reference images (%1/9)").arg(page.refs.length); font.bold: true }
                    Button { text: qsTr("Add image…"); onClicked: backend.pickImages(page.mode === "r2v") }
                    Label { text: qsTr("or drop files here"); color: "#80808a" }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 130
                    color: drop.containsDrag ? "#24242c" : "#18181d"
                    radius: theme.radius
                    border.color: page.refsOk ? "#2a2a33" : "#a05050"
                    DropArea {
                        id: drop
                        anchors.fill: parent
                        onDropped: (d) => { page.addRefs(d.urls.map(u => decodeURIComponent(u.toString().replace("file://", "")))) }
                    }
                    ListView {
                        anchors.fill: parent
                        anchors.margins: 8
                        orientation: ListView.Horizontal
                        spacing: 8
                        model: page.refs
                        delegate: Item {
                            width: 120; height: 114
                            Image {
                                anchors.fill: parent
                                anchors.bottomMargin: 16
                                source: backend.fileUrl(modelData)
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                            }
                            Label {
                                anchors.bottom: parent.bottom
                                text: page.mode === "i2v" ? qsTr("first frame") : "<Picture " + (index + 1) + ">"
                                font.pixelSize: 11
                                color: theme.accent
                            }
                            ToolButton {
                                anchors.right: parent.right
                                text: "✕"
                                onClicked: { const l = page.refs.slice(); l.splice(index, 1); page.refs = l }
                            }
                        }
                    }
                }
            }

            GridLayout {
                Layout.leftMargin: 20; Layout.rightMargin: 20
                columns: 4
                columnSpacing: 16
                rowSpacing: 6

                Label { text: qsTr("Size preset") }
                ComboBox {
                    id: preset
                    Layout.preferredWidth: 280
                    model: backend.presets
                    textRole: "label"
                    onActivated: (i) => { widthBox.value = backend.presets[i].width; heightBox.value = backend.presets[i].height }
                }
                Label { text: qsTr("Width × height") }
                RowLayout {
                    SpinBox { id: widthBox; from: 256; to: 2048; stepSize: 32; value: 608; editable: true
                        onValueModified: value = backend.snapSize(value) }
                    Label { text: "×" }
                    SpinBox { id: heightBox; from: 256; to: 2048; stepSize: 32; value: 352; editable: true
                        onValueModified: value = backend.snapSize(value) }
                }

                Label { text: qsTr("Seconds") }
                RowLayout {
                    SpinBox {
                        id: secondsBox
                        from: 1; to: 60; value: 6; stepSize: 1
                        property real realValue: value / 2
                        textFromValue: (v) => (v / 2).toLocaleString(Qt.locale(), 'f', 1)
                        valueFromText: (t) => Math.round(Number.fromLocaleString(Qt.locale(), t) * 2)
                        editable: true
                    }
                    Label {
                        color: "#a0a0aa"
                        text: qsTr("→ %1 frames = %2 s at 24 fps").arg(page.frames)
                              .arg(backend.secondsFor(page.frames).toLocaleString(Qt.locale(), 'f', 2))
                    }
                }
                Label { text: qsTr("Steps") }
                SpinBox { id: stepsBox; from: 1; to: 60; value: 20; editable: true }

                Label { text: qsTr("Seed") }
                RowLayout {
                    CheckBox { id: randomSeed; text: qsTr("Random"); checked: true }
                    TextField {
                        id: seedField
                        enabled: !randomSeed.checked
                        text: "1001"
                        validator: RegularExpressionValidator { regularExpression: /[0-9]{1,15}/ }
                        Layout.preferredWidth: 140
                    }
                }
                Label { text: qsTr("Audio") }
                Switch { id: audioSwitch; checked: true; text: checked ? qsTr("generate sound") : qsTr("silent") }
            }

            Label {
                visible: page.overCap
                Layout.leftMargin: 20
                color: "#ffcc66"
                text: qsTr("Above H3's native canvas (768 × 1344 pixels): untested on this card and much slower.")
            }
            Label {
                visible: !page.refsOk
                Layout.leftMargin: 20
                color: "#ff8a80"
                text: page.mode === "i2v" ? qsTr("Image to video needs exactly one image.") : qsTr("At most 9 reference images.")
            }

            RowLayout {
                Layout.leftMargin: 20; Layout.rightMargin: 20
                Button {
                    text: backend.busy ? qsTr("Working…") : qsTr("Generate  (Space)")
                    highlighted: true
                    enabled: !backend.busy && prompt.text.trim() !== "" && page.refsOk
                    onClicked: page.generate()
                }
                Button {
                    visible: backend.lastOutput !== "" && !backend.busy
                    text: qsTr("Open result")
                    onClicked: backend.openPath(backend.lastOutput)
                }
                Item { Layout.fillWidth: true }
            }

            Label {
                Layout.leftMargin: 20; Layout.rightMargin: 20
                Layout.fillWidth: true
                font.pixelSize: 12
                color: "#8a8a94"
                text: qsTr("On this 8 GB card a 608 × 352, 3 s clip at 20 steps takes about 2 minutes; 1344 × 768, 5 s takes about 27 minutes.")
                wrapMode: Text.Wrap
            }

            TextArea {
                Layout.leftMargin: 20; Layout.rightMargin: 20
                Layout.fillWidth: true
                Layout.preferredHeight: 140
                readOnly: true
                text: backend.log
                font.family: "monospace"
                font.pixelSize: 11
                wrapMode: TextEdit.Wrap
                onTextChanged: cursorPosition = length
            }
            Item { Layout.preferredHeight: 12 }
        }
    }
}
