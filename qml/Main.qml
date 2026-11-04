import QtQuick

Window {
    id: root

    color: "transparent"
    flags: Qt.Window | Qt.CustomizeWindowHint | Qt.ExpandedClientAreaHint
           | Qt.NoTitleBarBackgroundHint | Qt.MSWindowsFixedSizeDialogHint
    height: 600
    maximumHeight: 600
    maximumWidth: 800
    minimumHeight: 600
    minimumWidth: 800
    width: 800

    Shortcut {
        sequence: "Alt+F4"

        onActivated: root.close()
    }
}
