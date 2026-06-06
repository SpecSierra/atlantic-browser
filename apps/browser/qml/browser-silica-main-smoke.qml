import QtQuick 2.2
import Sailfish.Silica 1.0
import Sailfish.Ambience 1.0

ApplicationWindow {
    initialPage: Component {
        Page {
            id: splash

            property string wpUrl: {
                var s = String(Ambience.source).replace("file://", "")
                var p = s.lastIndexOf("/")
                if (p < 0) return "file:///usr/share/ambience/fire/images/ambience_fire.jpg"
                var dir = s.substring(0, p)
                var name = s.substring(p + 1).replace(".ambience", "")
                return "file://" + dir + "/images/ambience_" + name + ".jpg"
            }

            // Ambience wallpaper backdrop (loads async so the splash shows instantly).
            Image {
                anchors.fill: parent
                source: splash.wpUrl
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
            }

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.55) }
                    GradientStop { position: 0.5; color: Qt.rgba(0, 0, 0, 0.4) }
                    GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.62) }
                }
            }

            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                y: Math.round(parent.height * 0.34)
                spacing: Theme.paddingLarge

                Image {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: "file:///usr/share/atlantic-browser/data/icon-launcher-browser.png"
                    width: Theme.iconSizeLauncher
                    height: Theme.iconSizeLauncher
                    sourceSize.width: Theme.iconSizeLauncher
                    sourceSize.height: Theme.iconSizeLauncher
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Atlantic"
                    color: Theme.primaryColor
                    font.pixelSize: Math.round(Theme.fontSizeExtraLarge * 1.2)
                    font.weight: Font.Light
                }
            }

            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                y: Math.round(parent.height * 0.62)
                running: true
                size: BusyIndicatorSize.Medium
            }
        }
    }
}
