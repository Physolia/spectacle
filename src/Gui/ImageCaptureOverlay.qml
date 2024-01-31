/* SPDX-FileCopyrightText: 2022 Noah Davis <noahadvs@gmail.com>
 * SPDX-FileCopyrightText: 2022 Marco Martin <mart@kde.org>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls as QQC
import org.kde.kirigami as Kirigami
import org.kde.spectacle.private

import "Annotations"

/**
 * This page is shown when using the rectangular region capture mode.
 *
 * - There is a `contextWindow` context property that can be used to
 * access the instance of the CaptureWindow.
 * - There is a `SelectionEditor` singleton instance that can be used to
 * access the instance of SelectionEditor instantiated by contextWindow.
 * - There is a `Selection` singleton instance that can be used to
 * access the size and position of the selected area set by SelectionEditor.
 */
MouseArea {
    // This needs to be a mousearea in orcer for the proper mouse events to be correctly filtered
    id: root
    focus: true
    acceptedButtons: Qt.LeftButton | Qt.RightButton
    hoverEnabled: true
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true
    anchors.fill: parent

    AnnotationEditor {
        id: annotations
        anchors.fill: parent
        visible: true
        enabled: contextWindow.annotating && AnnotationDocument.tool.type !== AnnotationTool.NoTool
        viewportRect: G.mapFromPlatformRect(screenToFollow.geometry, screenToFollow.devicePixelRatio)
    }

    component Overlay: Rectangle {
        color: Settings.useLightMaskColor ? "white" : "black"
        opacity: 0.5
        LayoutMirroring.enabled: false
    }
    Overlay { // top / full overlay when nothing selected
        id: topOverlay
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: selectionRectangle.visible ? selectionRectangle.top : parent.bottom
        opacity: if (Selection.empty
            && (annotations.enabled || annotations.document.undoStackDepth > 0)) {
            return 0
        } else if (Selection.empty) {
            return 0.25
        } else {
            return 0.5
        }
        Behavior on opacity {
            NumberAnimation {
                duration: Kirigami.Units.longDuration
                easing.type: Easing.OutCubic
            }
        }
    }
    Overlay { // bottom
        id: bottomOverlay
        anchors.left: parent.left
        anchors.top: selectionRectangle.visible ? selectionRectangle.bottom : undefined
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: selectionRectangle.visible && height > 0
    }
    Overlay { // left
        anchors {
            left: topOverlay.left
            top: topOverlay.bottom
            right: selectionRectangle.visible ? selectionRectangle.left : undefined
            bottom: bottomOverlay.top
        }
        visible: selectionRectangle.visible && height > 0 && width > 0
    }
    Overlay { // right
        anchors {
            left: selectionRectangle.visible ? selectionRectangle.right : undefined
            top: topOverlay.bottom
            right: topOverlay.right
            bottom: bottomOverlay.top
        }
        visible: selectionRectangle.visible && height > 0 && width > 0
    }

    Rectangle {
        id: selectionRectangle
        enabled: !annotations.enabled
        color: "transparent"
        border.color: if (enabled) {
            return palette.active.highlight
        } else if (Settings.useLightMaskColor) {
            return "black"
        } else {
            return "white"
        }
        border.width: contextWindow.dprRound(1)
        visible: !Selection.empty && G.rectIntersects(Qt.rect(x,y,width,height),
                                                      Qt.rect(0,0,parent.width, parent.height))
        x: Selection.x - border.width - annotations.viewportRect.x
        y: Selection.y - border.width - annotations.viewportRect.y
        width: Selection.width + border.width * 2
        height: Selection.height + border.width * 2

        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
    }

    Item {
        x: -annotations.viewportRect.x
        y: -annotations.viewportRect.y
        enabled: selectionRectangle.enabled
        component SelectionHandle: Handle {
            visible: enabled && selectionRectangle.visible
                && SelectionEditor.dragLocation === SelectionEditor.None
                && G.rectIntersects(Qt.rect(x,y,width,height), annotations.viewportRect)
            shapePath.fillColor: selectionRectangle.border.color
            shapePath.strokeWidth: 0
            width: Kirigami.Units.gridUnit
            height: width
        }

        SelectionHandle {
            startAngle: 90
            sweepAngle: 270
            x: SelectionEditor.handlesRect.x
            y: SelectionEditor.handlesRect.y
        }
        SelectionHandle {
            startAngle: 90
            sweepAngle: 180
            x: SelectionEditor.handlesRect.x
            y: SelectionEditor.handlesRect.y + SelectionEditor.handlesRect.height/2 - height/2
        }
        SelectionHandle {
            startAngle: 0
            sweepAngle: 270
            x: SelectionEditor.handlesRect.x
            y: SelectionEditor.handlesRect.y + SelectionEditor.handlesRect.height - height
        }
        SelectionHandle {
            startAngle: 180
            sweepAngle: 180
            x: SelectionEditor.handlesRect.x + SelectionEditor.handlesRect.width/2 - width/2
            y: SelectionEditor.handlesRect.y
        }
        SelectionHandle {
            startAngle: 0
            sweepAngle: 180
            x: SelectionEditor.handlesRect.x + SelectionEditor.handlesRect.width/2 - width/2
            y: SelectionEditor.handlesRect.y + SelectionEditor.handlesRect.height - height
        }
        SelectionHandle {
            startAngle: 270
            sweepAngle: 180
            x: SelectionEditor.handlesRect.x + SelectionEditor.handlesRect.width - width
            y: SelectionEditor.handlesRect.y + SelectionEditor.handlesRect.height/2 - height/2
        }
        SelectionHandle {
            startAngle: 180
            sweepAngle: 270
            x: SelectionEditor.handlesRect.x + SelectionEditor.handlesRect.width - width
            y: SelectionEditor.handlesRect.y
        }
        SelectionHandle {
            startAngle: 270
            sweepAngle: 270
            x: SelectionEditor.handlesRect.x + SelectionEditor.handlesRect.width - width
            y: SelectionEditor.handlesRect.y + SelectionEditor.handlesRect.height - height
        }
    }

    ShortcutsTextBox {
        anchors {
            horizontalCenter: parent.horizontalCenter
            bottom: parent.bottom
        }
        visible: opacity > 0 && Settings.showCaptureInstructions
        // Assume SelectionEditor covers all screens.
        // Use parent's coordinate system.
        opacity: root.containsMouse
            && !contains(mapFromItem(root, root.mouseX, root.mouseY))
            && !root.pressed
            && !annotations.enabled
            && !mtbDragHandler.active
            && !atbDragHandler.active
            && !G.rectIntersects(SelectionEditor.handlesRect, Qt.rect(x, y, width, height))
        Behavior on opacity {
            NumberAnimation {
                duration: Kirigami.Units.longDuration
                easing.type: Easing.OutCubic
            }
        }
    }

    Item { // separate item because it needs to be above the stuff defined above
        width: SelectionEditor.screensRect.width
        height: SelectionEditor.screensRect.height
        x: -annotations.viewportRect.x
        y: -annotations.viewportRect.y

        // Magnifier
        Loader {
            id: magnifierLoader
            readonly property point targetPoint: {
                if (SelectionEditor.magnifierLocation === SelectionEditor.FollowMouse) {
                    return SelectionEditor.mousePosition
                } else {
                    let x = -width
                    let y = -height
                    if (SelectionEditor.magnifierLocation === SelectionEditor.TopLeft
                        || SelectionEditor.magnifierLocation === SelectionEditor.Left
                        || SelectionEditor.magnifierLocation === SelectionEditor.BottomLeft) {
                        x = Selection.left
                    } else if (SelectionEditor.magnifierLocation === SelectionEditor.TopRight
                        || SelectionEditor.magnifierLocation === SelectionEditor.Right
                        || SelectionEditor.magnifierLocation === SelectionEditor.BottomRight) {
                        x = Selection.right
                    } else if (SelectionEditor.magnifierLocation === SelectionEditor.Top
                        || SelectionEditor.magnifierLocation === SelectionEditor.Bottom) {
                        if (SelectionEditor.dragLocation !== SelectionEditor.None) {
                            x = SelectionEditor.mousePosition.x
                        } else {
                            x = Selection.horizontalCenter
                        }
                    }
                    if (SelectionEditor.magnifierLocation === SelectionEditor.TopLeft
                        || SelectionEditor.magnifierLocation === SelectionEditor.Top
                        || SelectionEditor.magnifierLocation === SelectionEditor.TopRight) {
                        y = Selection.top
                    } else if (SelectionEditor.magnifierLocation === SelectionEditor.BottomLeft
                        || SelectionEditor.magnifierLocation === SelectionEditor.Bottom
                        || SelectionEditor.magnifierLocation === SelectionEditor.BottomRight) {
                        y = Selection.bottom
                    } else if (SelectionEditor.magnifierLocation === SelectionEditor.Left
                        || SelectionEditor.magnifierLocation === SelectionEditor.Right) {
                        if (SelectionEditor.dragLocation !== SelectionEditor.None) {
                            y = SelectionEditor.mousePosition.y
                        } else {
                            y = Selection.verticalCenter
                        }
                    }
                    return Qt.point(x, y)
                }
            }
            readonly property rect rect: {
                let margin = Kirigami.Units.gridUnit
                let x = targetPoint.x + margin
                let y = targetPoint.y + margin
                if (SelectionEditor.magnifierLocation !== SelectionEditor.FollowMouse) {
                    if (SelectionEditor.magnifierLocation === SelectionEditor.TopLeft
                        || SelectionEditor.magnifierLocation === SelectionEditor.Left
                        || SelectionEditor.magnifierLocation === SelectionEditor.BottomLeft) {
                        x = targetPoint.x - width - margin
                    } else if (SelectionEditor.magnifierLocation === SelectionEditor.TopRight
                        || SelectionEditor.magnifierLocation === SelectionEditor.Right
                        || SelectionEditor.magnifierLocation === SelectionEditor.BottomRight) {
                        x = targetPoint.x + margin
                    } else if (SelectionEditor.magnifierLocation === SelectionEditor.Top
                        || SelectionEditor.magnifierLocation === SelectionEditor.Bottom) {
                        x = targetPoint.x - width / 2
                    }

                    if (SelectionEditor.magnifierLocation === SelectionEditor.TopLeft
                        || SelectionEditor.magnifierLocation === SelectionEditor.Top
                        || SelectionEditor.magnifierLocation === SelectionEditor.TopRight) {
                        y = targetPoint.y - width - margin
                    } else if (SelectionEditor.magnifierLocation === SelectionEditor.BottomLeft
                        || SelectionEditor.magnifierLocation === SelectionEditor.Bottom
                        || SelectionEditor.magnifierLocation === SelectionEditor.BottomRight) {
                        y = targetPoint.y + margin
                    } else if (SelectionEditor.magnifierLocation === SelectionEditor.Left
                        || SelectionEditor.magnifierLocation === SelectionEditor.Right) {
                        y = targetPoint.y - height / 2
                    }
                }
                return G.rectBounded(dprRound(x), dprRound(y), width, height, SelectionEditor.screensRect)
            }
            x: rect.x
            y: rect.y
            z: 100
            visible: SelectionEditor.showMagnifier
                && SelectionEditor.magnifierLocation !== SelectionEditor.None
                && G.rectIntersects(rect, annotations.viewportRect)
            active: Settings.showMagnifier
            sourceComponent: Magnifier {
                viewport: annotations
                targetPoint: magnifierLoader.targetPoint
            }
        }

        // Size ToolTip
        SizeLabel {
            id: ssToolTip
            readonly property int valignment: {
                if (Selection.empty) {
                    return Qt.AlignVCenter
                }
                const margin = Kirigami.Units.mediumSpacing * 2
                const w = width + margin
                const h = height + margin
                if (SelectionEditor.handlesRect.top >= h) {
                    return Qt.AlignTop
                } else if (SelectionEditor.screensRect.height - SelectionEditor.handlesRect.bottom >= h) {
                    return Qt.AlignBottom
                } else {
                    // At the bottom of the inside of the selection rect.
                    return Qt.AlignBaseline
                }
            }
            readonly property bool normallyVisible: !Selection.empty && !(mainToolBar.visible && mainToolBar.valignment === ssToolTip.valignment)
            Binding on x {
                value: contextWindow.dprRound(Selection.horizontalCenter - ssToolTip.width / 2)
                when: ssToolTip.normallyVisible
                restoreMode: Binding.RestoreNone
            }
            Binding on y {
                value: {
                    let v = 0
                    if (ssToolTip.valignment & Qt.AlignBaseline) {
                        v = Math.min(Selection.bottom, SelectionEditor.handlesRect.bottom - Kirigami.Units.gridUnit)
                            - ssToolTip.height - Kirigami.Units.mediumSpacing * 2
                    } else if (ssToolTip.valignment & Qt.AlignTop) {
                        v = SelectionEditor.handlesRect.top
                            - ssToolTip.height - Kirigami.Units.mediumSpacing * 2
                    } else if (ssToolTip.valignment & Qt.AlignBottom) {
                        v = SelectionEditor.handlesRect.bottom + Kirigami.Units.mediumSpacing * 2
                    } else {
                        v = (root.height - ssToolTip.height) / 2 - parent.y
                    }
                    return contextWindow.dprRound(v)
                }
                when: ssToolTip.normallyVisible
                restoreMode: Binding.RestoreNone
            }
            visible: opacity > 0
            opacity: ssToolTip.normallyVisible
                && G.rectIntersects(Qt.rect(x,y,width,height), annotations.viewportRect)
            Behavior on opacity {
                NumberAnimation {
                    duration: Kirigami.Units.longDuration
                    easing.type: Easing.OutCubic
                }
            }
            size: G.rawSize(Selection.size, SelectionEditor.devicePixelRatio)
            padding: Kirigami.Units.mediumSpacing * 2
            topPadding: padding - QmlUtils.fontMetrics.descent
            bottomPadding: topPadding
            background: FloatingBackground {
                implicitWidth: Math.ceil(parent.contentWidth) + parent.leftPadding + parent.rightPadding
                implicitHeight: Math.ceil(parent.contentHeight) + parent.topPadding + parent.bottomPadding
                color: Qt.rgba(parent.palette.window.r,
                            parent.palette.window.g,
                            parent.palette.window.b, 0.85)
                border.color: Qt.rgba(parent.palette.windowText.r,
                                    parent.palette.windowText.g,
                                    parent.palette.windowText.b, 0.2)
                border.width: contextWindow.dprRound(1)
            }
        }

        Connections {
            target: Selection
            function onEmptyChanged() {
                if (!Selection.empty
                    && (mainToolBar.rememberPosition || atbLoader.rememberPosition)) {
                    mainToolBar.rememberPosition = false
                    atbLoader.rememberPosition = false
                }
            }
        }

        // Main ToolBar
        FloatingToolBar {
            id: mainToolBar
            property bool rememberPosition: false
            readonly property int valignment: {
                if (Selection.empty) {
                    return 0
                }
                if (3 * height + topPadding + Kirigami.Units.mediumSpacing
                    <= SelectionEditor.screensRect.height - SelectionEditor.handlesRect.bottom
                ) {
                    return Qt.AlignBottom
                } else if (3 * height + bottomPadding + Kirigami.Units.mediumSpacing
                    <= SelectionEditor.handlesRect.top
                ) {
                    return Qt.AlignTop
                } else {
                    // At the bottom of the inside of the selection rect.
                    return Qt.AlignBaseline
                }
            }
            readonly property bool normallyVisible: {
                let emptyHovered = (root.containsMouse || annotations.hovered) && Selection.empty
                let menuVisible = ExportMenu.visible
                menuVisible |= OptionsMenu.visible
                menuVisible |= HelpMenu.visible
                let pressed = SelectionEditor.dragLocation || annotations.anyPressed
                return (emptyHovered || !Selection.empty || menuVisible) && !pressed
            }
            Binding on x {
                value: {
                    const v = Selection.empty ?
                        (root.width - mainToolBar.width) / 2 + annotations.viewportRect.x
                        : Selection.horizontalCenter - mainToolBar.width / 2
                    return Math.max(mainToolBar.leftPadding, // min value
                           Math.min(contextWindow.dprRound(v),
                                    SelectionEditor.screensRect.width - mainToolBar.width - mainToolBar.rightPadding)) // max value
                }
                when: mainToolBar.normallyVisible && !mainToolBar.rememberPosition
                restoreMode: Binding.RestoreNone
            }
            Binding on y {
                value: {
                    let v = 0
                    // put above selection if not enough room below selection
                    if (mainToolBar.valignment & Qt.AlignTop) {
                        v = SelectionEditor.handlesRect.top
                            - mainToolBar.height - mainToolBar.bottomPadding
                    } else if (mainToolBar.valignment & Qt.AlignBottom) {
                        v = SelectionEditor.handlesRect.bottom + mainToolBar.topPadding
                    } else if (mainToolBar.valignment & Qt.AlignBaseline) {
                        v = Math.min(Selection.bottom, SelectionEditor.handlesRect.bottom - Kirigami.Units.gridUnit)
                            - mainToolBar.height - mainToolBar.bottomPadding
                    } else {
                        v = (mainToolBar.height / 2) - mainToolBar.parent.y
                    }
                    return contextWindow.dprRound(v)
                }
                when: mainToolBar.normallyVisible && !mainToolBar.rememberPosition
                restoreMode: Binding.RestoreNone
            }
            visible: opacity > 0
            opacity: normallyVisible && G.rectIntersects(Qt.rect(x,y,width,height), annotations.viewportRect)
            Behavior on opacity {
                NumberAnimation {
                    duration: Kirigami.Units.longDuration
                    easing.type: Easing.OutCubic
                }
            }
            layer.enabled: true // improves the visuals of the opacity animation
            focusPolicy: Qt.NoFocus
            contentItem: MainToolBarContents {
                id: mainToolBarContents
                focusPolicy: Qt.NoFocus
                displayMode: QQC.AbstractButton.TextBesideIcon
                showSizeLabel: mainToolBar.valignment === ssToolTip.valignment
                imageSize: G.rawSize(Selection.size, SelectionEditor.devicePixelRatio)
            }

            DragHandler { // parent is contentItem and parent is a read-only property
                id: mtbDragHandler
                enabled: Selection.empty
                target: mainToolBar
                acceptedButtons: Qt.LeftButton
                margin: mainToolBar.padding
                xAxis.minimum: annotations.viewportRect.x
                xAxis.maximum: annotations.viewportRect.x + root.width - mainToolBar.width
                yAxis.minimum: annotations.viewportRect.y
                yAxis.maximum: annotations.viewportRect.y + root.height - mainToolBar.height
                cursorShape: enabled ?
                    (active ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
                    : undefined
                onActiveChanged: if (active && !mainToolBar.rememberPosition) {
                    mainToolBar.rememberPosition = true
                }
            }
        }

        AnimatedLoader {
            id: atbLoader
            property bool rememberPosition: false
            readonly property int valignment: mainToolBar.valignment & (Qt.AlignTop | Qt.AlignBaseline) ?
                Qt.AlignTop : Qt.AlignBottom
            active: visible && mainToolBar.visible
            onActiveChanged: if (!active && rememberPosition
                && !contextWindow.annotating) {
                rememberPosition = false
            }
            state: mainToolBar.normallyVisible
                && contextWindow.annotating ? "active" : "inactive"

            Binding on x {
                value: {
                    const min = mainToolBar.x
                    const target = contextWindow.dprRound(mainToolBar.x + (mainToolBar.width - atbLoader.width) / 2)
                    const max = mainToolBar.x + mainToolBar.width - atbLoader.width
                    return Math.max(min, Math.min(target, max))
                }
                when: !atbLoader.rememberPosition
                restoreMode: Binding.RestoreNone
            }
            Binding on y {
                value: contextWindow.dprRound(atbLoader.valignment & Qt.AlignTop ?
                    mainToolBar.y - atbLoader.height - Kirigami.Units.mediumSpacing
                    : mainToolBar.y + mainToolBar.height + Kirigami.Units.mediumSpacing)
                when: !atbLoader.rememberPosition
                restoreMode: Binding.RestoreNone
            }

            DragHandler { // parented to contentItem
                id: atbDragHandler
                enabled: Selection.empty
                acceptedButtons: Qt.LeftButton
                xAxis.minimum: annotations.viewportRect.x
                xAxis.maximum: annotations.viewportRect.x + root.width - atbLoader.width
                yAxis.minimum: annotations.viewportRect.y
                yAxis.maximum: annotations.viewportRect.y + root.height - atbLoader.height
                cursorShape: enabled ?
                    (active ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
                    : undefined
                onActiveChanged: if (active && !atbLoader.rememberPosition) {
                    atbLoader.rememberPosition = true
                }
            }

            sourceComponent: FloatingToolBar {
                id: annotationsToolBar
                focusPolicy: Qt.NoFocus
                contentItem: AnnotationsToolBarContents {
                    id: annotationsContents
                    displayMode: QQC.AbstractButton.IconOnly
                    focusPolicy: Qt.NoFocus
                }

                topLeftRadius: optionsToolBar.visible
                    && optionsToolBar.x === 0
                    && atbLoader.valignment & Qt.AlignTop ? 0 : radius
                topRightRadius: optionsToolBar.visible
                    && optionsToolBar.x === width - optionsToolBar.width
                    && atbLoader.valignment & Qt.AlignTop ? 0 : radius
                bottomLeftRadius: optionsToolBar.visible
                    && optionsToolBar.x === 0
                    && atbLoader.valignment & Qt.AlignBottom ? 0 : radius
                bottomRightRadius: optionsToolBar.visible
                    && optionsToolBar.x === width - optionsToolBar.width
                    && atbLoader.valignment & Qt.AlignBottom ? 0 : radius

                // Exists purely for cosmetic reasons to make the border of
                // optionsToolBar that meets annotationsToolBar look better
                Rectangle {
                    id: borderBg
                    z: -1
                    visible: optionsToolBar.visible
                    opacity: optionsToolBar.opacity
                    parent: annotationsToolBar
                    x: optionsToolBar.x + annotationsToolBar.background.border.width
                    y: atbLoader.valignment & Qt.AlignTop ?
                        optionsToolBar.y + optionsToolBar.height : optionsToolBar.y
                    width: optionsToolBar.width - annotationsToolBar.background.border.width * 2
                    height: contextWindow.dprRound(1)
                    color: annotationsToolBar.background.color
                }

                AnimatedLoader {
                    id: optionsToolBar
                    parent: annotationsToolBar
                    x: {
                        const targetX = annotationsContents.x
                            + annotationsContents.checkedButton.x
                            + (annotationsContents.checkedButton.width - width) / 2
                        return Math.max(0, // min value
                               Math.min(contextWindow.dprRound(targetX),
                                        parent.width - width)) // max value
                    }
                    y: atbLoader.valignment & Qt.AlignTop ?
                        -optionsToolBar.height + borderBg.height
                        : optionsToolBar.height - borderBg.height
                    state: if (AnnotationDocument.tool.options !== AnnotationTool.NoOptions
                        || (AnnotationDocument.tool.type === AnnotationTool.SelectTool
                            && AnnotationDocument.selectedItem.options !== AnnotationTool.NoOptions)
                    ) {
                        return "active"
                    } else {
                        return "inactive"
                    }
                    sourceComponent: FloatingToolBar {
                        focusPolicy: Qt.NoFocus
                        contentItem: AnnotationOptionsToolBarContents {
                            displayMode: QQC.AbstractButton.IconOnly
                            focusPolicy: Qt.NoFocus
                        }
                        topLeftRadius: atbLoader.valignment & Qt.AlignBottom && x >= 0 ? 0 : radius
                        topRightRadius: atbLoader.valignment & Qt.AlignBottom && x + width <= annotationsToolBar.width ? 0 : radius
                        bottomLeftRadius: atbLoader.valignment & Qt.AlignTop && x >= 0 ? 0 : radius
                        bottomRightRadius: atbLoader.valignment & Qt.AlignTop && x + width <= annotationsToolBar.width ? 0 : radius
                    }
                }
            }
        }
    }

    // FIXME: This shortcut only exists here because spectacle interprets "Ctrl+Shift+,"
    // as "Ctrl+Shift+<" for some reason unless we use a QML Shortcut.
    Shortcut {
        sequences: [StandardKey.Preferences]
        onActivated: contextWindow.showPreferencesDialog()
    }
}
