pragma ComponentBehavior: Bound
import QtQuick
import waku
import waku.services as S

// Where a window dragged under `Tiling.dragAction: "swap"` will land: a rounded rectangle over the
// tile it takes, or on another monitor over the spot it gets at the end of that layout. While a
// tiled window is resized by mouse it outlines every tile of the workspace at the size it gets at
// the release (`Tiling.resizePreview`; the other windows move only then). One click-through
// overlay window per screen, on screen only while a drag is on. Add it once to the shell:
//
//     TilingDropIndicator {}
//
// The colours follow the waku Theme. A shell with colours of its own binds them, and the indicator
// follows every change of them (a new scheme, a wallpaper that recolours the shell):
//
//     TilingDropIndicator { color: Color.mPrimary; radius: Style.radiusM }
//
//   color         the border, and the fill at `fillOpacity` (default Theme.accent)
//   borderColor   the border alone (default `color`)
//   fillOpacity   the opacity of the fill (default 0.22)
//   borderWidth   pixels (default 3)
//   radius        the corner radius (default Theme.radius)
//   backOpacity   the opacity of the whole indicator when the window would go back to its own
//                 tile, a drop that changes nothing (default 0.5)
//   previewFillOpacity  the opacity of the fill of a resize preview's tiles (default 0.1)
//   fadeMs        the fade in and out, and the glide from tile to tile (default 120)
//
// Nothing is drawn for a drop that floats the window (the float key held).
Variants {
    id: root

    property color color: Theme.accent
    property color borderColor: root.color
    property real fillOpacity: 0.22
    property int borderWidth: 3
    property int radius: Theme.radius
    property real backOpacity: 0.5
    property real previewFillOpacity: 0.1
    property int fadeMs: 120

    model: ScreenList.screens

    delegate: PanelWindow {
        id: overlay

        required property ShellScreen modelData
        readonly property var target: S.Tiling.dropTarget
        readonly property bool shown: overlay.target.active === true && overlay.target.screen === overlay.modelData.name
        readonly property var preview: S.Tiling.resizePreview
        readonly property bool previewShown: overlay.preview.active === true && overlay.preview.screen === overlay.modelData.name
        // The preview's tiles, kept while it fades out.
        property var previewRects: []

        function followPreview() {
            if (overlay.previewShown)
                overlay.previewRects = overlay.preview.rects;
        }

        // Keeps the last rectangle while the indicator fades out.
        function follow() {
            if (!overlay.shown)
                return;
            box.x = overlay.target.x;
            box.y = overlay.target.y;
            box.width = overlay.target.width;
            box.height = overlay.target.height;
        }

        screen: modelData
        anchors.top: true
        anchors.bottom: true
        anchors.left: true
        anchors.right: true
        exclusionMode: ExclusionMode.Ignore
        layer: WlrLayer.Overlay
        keyboardFocus: WlrKeyboardFocus.None
        color: "transparent"
        mask: Region {}
        visible: overlay.shown || overlay.previewShown
        hideDelay: root.fadeMs

        onTargetChanged: overlay.follow()
        onShownChanged: overlay.follow()
        onPreviewChanged: overlay.followPreview()
        onPreviewShownChanged: overlay.followPreview()

        // The resize preview follows the mouse as it is: no glide.
        Item {
            anchors.fill: parent
            opacity: overlay.previewShown ? 1 : 0

            Behavior on opacity {
                NumberAnimation {
                    duration: root.fadeMs
                }
            }

            Repeater {
                model: overlay.previewRects

                Rectangle {
                    required property var modelData

                    x: modelData.x
                    y: modelData.y
                    width: modelData.width
                    height: modelData.height
                    radius: root.radius
                    color: Qt.rgba(root.color.r, root.color.g, root.color.b, root.color.a * root.previewFillOpacity)
                    border.color: root.borderColor
                    border.width: root.borderWidth
                }
            }
        }

        Rectangle {
            id: box

            // The glide runs only between two targets, not from where the last drag ended.
            readonly property bool glide: box.opacity > 0

            opacity: overlay.shown ? (overlay.target.kind === "back" ? root.backOpacity : 1) : 0
            radius: root.radius
            color: Qt.rgba(root.color.r, root.color.g, root.color.b, root.color.a * root.fillOpacity)
            border.color: root.borderColor
            border.width: root.borderWidth

            Behavior on opacity {
                NumberAnimation {
                    duration: root.fadeMs
                }
            }
            Behavior on x {
                enabled: box.glide
                NumberAnimation {
                    duration: root.fadeMs
                    easing.type: Easing.OutCubic
                }
            }
            Behavior on y {
                enabled: box.glide
                NumberAnimation {
                    duration: root.fadeMs
                    easing.type: Easing.OutCubic
                }
            }
            Behavior on width {
                enabled: box.glide
                NumberAnimation {
                    duration: root.fadeMs
                    easing.type: Easing.OutCubic
                }
            }
            Behavior on height {
                enabled: box.glide
                NumberAnimation {
                    duration: root.fadeMs
                    easing.type: Easing.OutCubic
                }
            }
        }
    }
}
