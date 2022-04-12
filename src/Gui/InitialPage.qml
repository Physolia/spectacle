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
 * This is the initial page for the QML UI.
 * It allows the user to set up screen capturing and export screen captures.
 * There is a mainWindow context property that points to the instance of SpectacleMainWindow.
 */
EmptyPage {
    id: root
    padding: Kirigami.Units.mediumSpacing
    // TODO redesign this UI to look nicer with the QML components we have
    contentItem: RowLayout {
        spacing: Kirigami.Units.mediumSpacing
        Image {
            fillMode: Image.PreserveAspectFit
            sourceSize.height: height
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 0
            Layout.preferredHeight: 0
            Layout.maximumHeight: parent.height
            source: SpectacleCore.screenCaptureUrl// "file:/usr/share/wallpapers/Next/contents/images/1920x1080.jpg"
        }
        GridLayout {
            Layout.maximumWidth: implicitWidth
            rowSpacing: Kirigami.Units.mediumSpacing
            columnSpacing: Kirigami.Units.mediumSpacing
            columns: 3
            QQC2.Label {
                Layout.columnSpan: 3
                text: "Capture Mode"
                font.bold: true
            }
            // Label ComboBox
            QQC2.Label {
                Layout.leftMargin: Kirigami.Units.mediumSpacing * 4
                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                text: "Area:"
            }
            QQC2.ComboBox {
                id: captureModeComboBox
                Layout.fillWidth: true
                Layout.columnSpan: 2
                model: SpectacleCore.captureModeModel
                textRole: "display"
                valueRole: "captureMode"
                // We have to set the data from Settings in onCompleted to get it at the correct time.
                // Binding to Settings properties could cause unexpected behavior when
                // there are multiple instances of Spectacle, but I have not tested that.
                Component.onCompleted: currentIndex = indexOfValue(Settings.captureMode)
                // The settings are saved to the config file when the window is destroyed.
                // This item will be destroyed before the window when the window is closed.
                Component.onDestruction: Settings.captureMode = currentValue
            }
            // Label SpinBox CheckBox
            QQC2.Label {
                Layout.leftMargin: Kirigami.Units.mediumSpacing * 4
                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                text: "Delay:"
            }
            QQC2.SpinBox {
                id: delaySpinBox
                Layout.fillWidth: true
                enabled: !captureOnClickCheckBox.checked
                Component.onCompleted: value = Settings.captureDelay
                Component.onDestruction: Settings.captureDelay = value
            }
            QQC2.CheckBox {
                id: captureOnClickCheckBox
                text: "On Click"
                enabled: Platform.supportedShutterModes === Platform.Immediate | Platform.OnClick
                QQC2.ToolTip.text: i18n("Wait for a mouse click before capturing the screenshot image")
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.visible: hovered || pressed
                Component.onCompleted: checked = Platform.supportedShutterModes & Platform.OnClick && Settings.captureOnClick
                Component.onDestruction: if (enabled) Settings.captureOnClick = checked
            }
            //----------------------
            QQC2.Label {
                Layout.topMargin: Kirigami.Units.mediumSpacing
                Layout.columnSpan: 3
                text: "Options"
                font.bold: true
            }
            QQC2.CheckBox {
                id: mousePointerCheckBox
                text: "Include mouse pointer"
                Layout.leftMargin: Kirigami.Units.mediumSpacing * 4
                Layout.columnSpan: 3
                QQC2.ToolTip.text: i18n("Show the mouse cursor in the screenshot image")
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.visible: hovered
                Component.onCompleted: checked = Settings.includePointer
                Component.onDestruction: Settings.includePointer = checked
            }
            QQC2.CheckBox {
                id: windowDecorationsCheckBox
                text: "Include window titlebar and borders"
                Layout.leftMargin: Kirigami.Units.mediumSpacing * 4
                Layout.columnSpan: 3
                QQC2.ToolTip.text: i18n("Show the window title bar, the minimize/maximize/close buttons, and the window border")
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.visible: hovered
                Component.onCompleted: checked = Settings.includeDecorations
                Component.onDestruction: Settings.includeDecorations = checked
            }
            QQC2.CheckBox {
                text: "Capture the current pop-up only"
                Layout.leftMargin: Kirigami.Units.mediumSpacing * 4
                Layout.columnSpan: 3
                QQC2.ToolTip.text: i18n("Capture only the current pop-up window (like a menu, tooltip etc).\n"
                                      + "If disabled, the pop-up is captured along with the parent window")
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.visible: hovered
                Component.onCompleted: checked = Settings.transientOnly
                Component.onDestruction: Settings.transientOnly = checked
            }
            QQC2.CheckBox {
                text: "Quit after manual Save or Copy"
                Layout.leftMargin: Kirigami.Units.mediumSpacing * 4
                Layout.columnSpan: 3
                QQC2.ToolTip.text: i18n("Quit Spectacle after manually saving or copying the image")
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.visible: hovered
                Component.onCompleted: checked = Settings.quitAfterSaveCopyExport
                Component.onDestruction: Settings.quitAfterSaveCopyExport = checked
            }
            //----------------------
            QQC2.DelayButton {
                id: screenShotButton
                Layout.columnSpan: 3
                Layout.alignment: Qt.AlignCenter
                delay: delaySpinBox.value * 1000
                icon.name: progress > 0 ? "dialog-cancel" : "spectacle"
                text: progress > 0 ? i18n("Cancel") : i18n("Take a New Screenshot")
                onCheckedChanged: if (checked) {checked = false}
                NumberAnimation on progress {
                    id: screenShotButtonAnimation
                    from: 0
                    to: 1
                    duration: delaySpinBox.value * 1000
                    onFinished: SpectacleCore.takeNewScreenshot(duration, captureModeComboBox.currentValue, mousePointerCheckBox.checked, windowDecorationsCheckBox.checked)
                }
                onClicked: if (screenShotButtonAnimation.running) {
                    screenShotButtonAnimation.stop()
                    screenShotButton.progress = 0
                } else {
                    screenShotButtonAnimation.restart()
                }
            }
        }
    }

    footer: QQC2.ToolBar {
        padding: Kirigami.Units.mediumSpacing
        contentItem: RowLayout {
            spacing: Kirigami.Units.mediumSpacing
            QQC2.ToolButton {
                id: helpMenuButton
                flat: false
                icon.name: "help-contents"
                text: i18nc("TODO", "Help")
                // helpMenu is just a QMenu instead of a SpectacleMenu, so I have to do extra work
                onPressed: if (mainWindow.helpMenu !== null) {
                    // for some reason, y has to be set to get the correct y pos, but x shouldn't be
                    mainWindow.helpMenu.pos = mapToGlobal(0, y + height)
                    mainWindow.helpMenu.show()
                }
                Connections {
                    target: mainWindow.helpMenu
                    function onAboutToShow() {
                        helpMenuButton.down = true
                    }
                    function onAboutToHide() {
                        helpMenuButton.down = Qt.binding(() => helpMenuButton.pressed)
                    }
                }
                // NOTE: only qqc2-desktop-style and qqc2-breeze-style have showMenuArrow
                Component.onCompleted: if (background.hasOwnProperty("showMenuArrow")) {
                    background.showMenuArrow = true
                }
            }
            QQC2.Button {
                // I can't figure out where the icon for the configure button in the old UI is defined.
                // Hopefully this is fine.
                icon.name: "configure"
                text: i18nc("TODO", "Configure…")
                QQC2.ToolTip.text: i18n("Change Spectacle's settings.")
                QQC2.ToolTip.visible: hovered || pressed
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: mainWindow.showPreferencesDialog()
            }
            Item {
                Layout.fillWidth: true
            }
            QQC2.Button {
                icon.name: "document-edit"
                text: i18nc("TODO", "Annotate…")
                QQC2.ToolTip.text: i18n("Add annotation to the screenshot")
                QQC2.ToolTip.visible: hovered || pressed
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            QQC2.ToolButton {
                flat: false
                icon.name: "tools"
                text: i18nc("TODO", "Tools")
                down: pressed || mainWindow.toolsMenu.visible
                onPressed: {
                    // for some reason, y has to be set to get the correct y pos, but x shouldn't be
                    mainWindow.toolsMenu.popup(mapToGlobal(0, y + height))
                }
                Component.onCompleted: if (background.hasOwnProperty("showMenuArrow")) {
                    background.showMenuArrow = true
                }
            }
            QQC2.ToolButton {
                flat: false
                icon.name: "document-share"
                text: i18nc("TODO", "Export")
                down: pressed || mainWindow.exportMenu.visible
                onPressed: {
                    // for some reason, y has to be set to get the correct y pos, but x shouldn't be
                    mainWindow.exportMenu.popup(mapToGlobal(0, y + height))
                }
                Component.onCompleted: if (background.hasOwnProperty("showMenuArrow")) {
                    background.showMenuArrow = true
                }
            }
            // TODO: these 2 should be split buttons
            QQC2.ToolButton {
                // TODO Clipboard Button
                text: i18nc("TODO", "Copy to Clipboard")
                icon.name: "edit-copy"
                flat: false
                down: pressed || mainWindow.clipboardMenu.visible
                onPressed: {
                    // for some reason, y has to be set to get the correct y pos, but x shouldn't be
                    mainWindow.clipboardMenu.popup(mapToGlobal(0, y + height))
                }
                Component.onCompleted: if (background.hasOwnProperty("showMenuArrow")) {
                    background.showMenuArrow = true
                }
            }
            QQC2.ToolButton {
                // TODO Save Button
                text: i18nc("TODO", "Save")
                icon.name: "document-save"
                flat: false
                down: pressed || mainWindow.saveMenu.visible
                onPressed: {
                    // for some reason, y has to be set to get the correct y pos, but x shouldn't be
                    mainWindow.saveMenu.popup(mapToGlobal(0, y + height))
                }
                Component.onCompleted: if (background.hasOwnProperty("showMenuArrow")) {
                    background.showMenuArrow = true
                }
            }
        }
    }
}
