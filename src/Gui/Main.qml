/* SPDX-FileCopyrightText: 2022 Noah Davis <noahadvs@gmail.com>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import QtQuick.Controls 2.15 as QQC2
import QtQuick.Templates 2.15 as T
import org.kde.kirigami 2.19 as Kirigami
import org.kde.spectacle.private 1.0

/**
 * This is the main component for the QML UI.
 * It is instantiated inside of SpectacleMainWindow::init().
 * SpectacleMainWindow gets its minimum size from the minimumWidth
 * and minimumHeight properties of this component.
 * SpectacleMainWindow also manages the width and height of this component.
 * There is a mainWindow context property that points to the instance of SpectacleMainWindow.
 */
T.StackView {
    id: root

    // SpectacleMainWindow::init() uses these properties for the minimum window size
    property real minimumWidth: implicitWidth
    property real minimumHeight: implicitHeight

    LayoutMirroring.enabled: locale.textDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: implicitContentHeight + topPadding + bottomPadding

    hoverEnabled: false

    contentItem: currentItem

    initialItem: "qrc:/src/Gui/InitialPage.qml"

    // Using NumberAnimation instead of XAnimator because the latter wasn't always smooth enough.
    pushEnter: Transition {
        NumberAnimation {
            property: "x"
            from: (control.mirrored ? -0.5 : 0.5) * control.width
            to: 0
            duration: Kirigami.Units.longDuration
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            property: "opacity"
            from: 0.0; to: 1.0
            duration: Kirigami.Units.longDuration
            easing.type: Easing.OutCubic
        }
    }
    pushExit: Transition {
        NumberAnimation {
            property: "x"
            from: 0
            to: (control.mirrored ? -0.5 : 0.5) * -control.width
            duration: Kirigami.Units.longDuration
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            property: "opacity"
            from: 1.0; to: 0.0
            duration: Kirigami.Units.longDuration
            easing.type: Easing.OutCubic
        }
    }
    popEnter: Transition {
        NumberAnimation {
            property: "x"
            from: (control.mirrored ? -0.5 : 0.5) * -control.width
            to: 0
            duration: Kirigami.Units.longDuration
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            property: "opacity"
            from: 0.0; to: 1.0
            duration: Kirigami.Units.longDuration
            easing.type: Easing.OutCubic
        }
    }
    popExit: Transition {
        NumberAnimation {
            property: "x"
            from: 0
            to: (control.mirrored ? -0.5 : 0.5) * control.width
            duration: Kirigami.Units.longDuration
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            property: "opacity"
            from: 1.0; to: 0.0
            duration: Kirigami.Units.longDuration
            easing.type: Easing.OutCubic
        }
    }
    replaceEnter: pushEnter
    replaceExit: pushExit
}
