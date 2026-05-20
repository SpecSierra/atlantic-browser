import QtQuick 2.2
import Sailfish.Silica 1.0

ApplicationWindow {
    initialPage: Component {
        Page {
            Column {
                anchors.centerIn: parent
                spacing: Theme.paddingLarge

                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    running: true
                    size: BusyIndicatorSize.Large
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Launching Atlantic"
                }
            }
        }
    }
}
