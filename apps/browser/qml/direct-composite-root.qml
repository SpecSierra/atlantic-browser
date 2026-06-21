import QtQuick 2.2

// Direct-composite root surface (ATLANTIC_DIRECT_COMPOSITE).
//
// In direct-composite mode the app is split across two surfaces so the chrome can
// sit ABOVE the web (lipstick won't stack a subsurface below its parent, only a
// sibling above): this empty transparent root is the bottom surface and the parent
// of the web subsurface, while the full Silica chrome runs in a child window layered
// above it. Nothing is drawn here — it only needs to map a surface.
Item {
    id: root

    // A 1x1 fully transparent pixel guarantees the scene graph produces a frame so
    // the root wl_surface maps a (transparent) buffer; an empty Item may never swap.
    Rectangle {
        width: 1
        height: 1
        color: "transparent"
    }
}
