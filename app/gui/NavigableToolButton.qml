import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

ToolButton {
    property string iconSource

    activeFocusOnTab: true

    icon.source: iconSource

    // Omnuv: **sized to the bar, never to this button's own background.**
    // Upstream bound `icon.width: background.width`, which under Material was
    // harmless because Material's ToolButton has a fixed implicit size. Under
    // FluentWinUI3 the button is sized from its content — `implicitWidth:
    // Math.max(implicitBackgroundWidth …, implicitContentWidth + padding)` —
    // and its content is an IconLabel sized from the icon. So icon = background
    // = button = icon + padding: a cycle that grows by the padding on every
    // pass, asks qsvg to rasterise the glyph at thousands of pixels, is refused
    // at the 256 MB image limit, and pegs the UI thread at ~130 % of a core
    // while the window stays white. Measured on the rig on 15 September, and it
    // ran on regardless of whether the bar was laid out or visible, because a
    // geometry binding evaluates either way.
    //
    // The bar's height is a fixed number (`main.qml`, 0 or 60) and depends on
    // nothing here, so it is the one size that cannot feed back.
    icon.width: parent ? Math.max(0, parent.height - topPadding - bottomPadding) : 0
    icon.height: parent ? Math.max(0, parent.height - topPadding - bottomPadding) : 0

    // The instrument that would have named this in seconds rather than in an
    // afternoon: the rasteriser's own complaint arrives after ~100 s and names
    // a file, not a size. Silent while healthy.
    onIconChanged: {
        if (icon.width > 512 || icon.height > 512) {
            console.warn("NavigableToolButton: icon runaway " + icon.width + "x" + icon.height
                         + " in a " + width + "x" + height + " button")
        }
    }

    // This determines the size of the Material highlight. We increase it
    // from the default because we use larger than normal icons for TV readability.
    Layout.preferredHeight: parent.height

    Keys.onReturnPressed: {
        clicked()
    }

    Keys.onEnterPressed: {
        clicked()
    }

    Keys.onRightPressed: {
        nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
    }

    Keys.onLeftPressed: {
        nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
    }
}
