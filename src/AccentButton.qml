import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// The main action of a page, filled with the Omarchy accent. Drawn here
// because the Material style left highlighted buttons unfilled on this Qt.
Button {
    id: control
    implicitHeight: 44
    leftPadding: 22
    rightPadding: 22
    contentItem: Label {
        text: control.text
        font.bold: true
        color: control.enabled ? theme.accentForeground : "#8a8a94"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: Math.min(8, theme.radius)
        color: !control.enabled ? "#2a2a30" : control.down ? Qt.darker(theme.accent, 1.2)
               : control.hovered ? Qt.lighter(theme.accent, 1.1) : theme.accent
    }
}
