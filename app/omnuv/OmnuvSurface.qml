import QtQuick

import Omnuv 1.0

// **A popup's background, drawn by us** (3 October 2026). Material draws its
// dialogs', menus' and dropdowns' backgrounds as a shadowed layer, and the
// software renderer, which the Linux loop and a machine without working
// graphics drivers use, draws no layer effects: the background vanished and
// the items floated over the page. The card's fill, radius and line, as the
// web's popups; the system's colours under high contrast.
Rectangle {
    color: Theme.highContrast ? palette.base : Theme.fillCard
    radius: Theme.radiusOverlay
    border.color: Theme.strokeCard
}
