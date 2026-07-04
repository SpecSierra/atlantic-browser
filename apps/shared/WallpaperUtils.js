.pragma library

/*
 * Resolve the current ambience wallpaper to its full-size image. For system
 * ambiences Ambience.source is a .ambience descriptor path (e.g.
 * .../fire/fire.ambience) and the displayable wallpaper lives next to it under
 * images/ambience_<name>.jpg. For user-created ambiences (made from a photo),
 * Ambience.source is the wallpaper image itself
 * (~/.local/share/ambienced/wallpapers/<hash>.jpg) — use it as-is. Shared by every
 * surface that paints the ambience backdrop (Background, StartPage, PopUpMenu,
 * the launch splash) so the resolution logic lives in one place.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

function ambienceImageUrl(ambienceSource) {
    var s = String(ambienceSource).replace("file://", "");
    if (s.length > 0 && s.indexOf(".ambience", s.length - 9) < 0)
        return "file://" + s;
    var p = s.lastIndexOf("/");
    if (p < 0) return "file:///usr/share/ambience/fire/images/ambience_fire.jpg";
    var dir = s.substring(0, p);
    var name = s.substring(p + 1).replace(".ambience", "");
    return "file://" + dir + "/images/ambience_" + name + ".jpg";
}
