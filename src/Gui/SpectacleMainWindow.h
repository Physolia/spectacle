/* This file is part of Spectacle, the KDE screenshot utility
 * SPDX-FileCopyrightText: 2015 Boudhayan Gupta <bgupta@kde.org>
 * SPDX-FileCopyrightText: 2022 Noah Davis <noahadvs@gmail.com>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "CaptureModeModel.h"
#include "Config.h"
#include "ExportMenu.h"
#include "Platforms/Platform.h"

#include <KNS3/KMoreToolsMenuFactory>
#include <KSharedConfig>

#include <QQuickView>

#include <memory>

/**
 * The main window of Spectacle's UI.
 * Adapted from KSMainWindow, a QDialog subclass from the old Qt Widgets UI.
 */
class SpectacleMainWindow : public QQuickView
{
    Q_OBJECT
    Q_PROPERTY(QMenu *helpMenu READ helpMenu CONSTANT FINAL)
    Q_PROPERTY(SpectacleMenu *toolsMenu READ toolsMenu CONSTANT FINAL)
    Q_PROPERTY(SpectacleMenu *exportMenu READ exportMenu CONSTANT FINAL)
    Q_PROPERTY(SpectacleMenu *clipboardMenu READ clipboardMenu CONSTANT FINAL)
    Q_PROPERTY(SpectacleMenu *saveMenu READ saveMenu CONSTANT FINAL)

public:
    explicit SpectacleMainWindow(QQmlEngine *engine);
    ~SpectacleMainWindow() override = default;

    enum class MessageDuration { AutoHide, Persistent };

    QMenu *helpMenu() const;
    SpectacleMenu *toolsMenu() const;
    SpectacleMenu *exportMenu() const;
    SpectacleMenu *clipboardMenu() const;
    SpectacleMenu *saveMenu() const;

public Q_SLOTS:

    void setScreenshotAndShow(const QPixmap &pixmap, bool showAnnotator);
    void onImageSaved(const QUrl &location);
    void onImageSavedAndCopied(const QUrl &location);
    void onScreenshotFailed();
    void setPlaceholderTextOnLaunch();
    void showPrintDialog();
    void openScreenshotsFolder();
    void showPreferencesDialog();

Q_SIGNALS:

    void newScreenshotRequest(int theCaptureMode, int theTimeout, bool theIncludePointer, bool theIncludeDecorations);
    void dragAndDropRequest();
    void screenshotUrlChanged();

private Q_SLOTS:

    void init();
    void captureScreenshot(CaptureModeModel::CaptureMode theCaptureMode, int theTimeout, bool theIncludePointer, bool theIncludeDecorations);
    void showImageSharedFeedback(bool error, const QString &message);
    void onImageCopied();
    void onImageSavedAndLocationCopied(const QUrl &location);
    void setDefaultSaveAction();
    void setDefaultCopyAction();
    void save();
    void saveAs();
    void copyImage();
    void copyLocation();
    void restoreWindowTitle();
    void updateMinimumWidth();
    void updateMinimumHeight();

private:
    enum class QuitBehavior { QuitImmediately, QuitExternally };
    void quitExternally();

    // Replaces QWidget::isWindowModified()
    bool isUnsaved() const;
    // Replaces QWidget::setWindowModified();
    void setUnsaved(bool unsaved, const QString &title);

//     void showInlineMessage(const QString &message,
//                            const KMessageWidget::MessageType messageType,
//                            const MessageDuration messageDuration = MessageDuration::AutoHide,
//                            const QList<QAction *> &actions = {});
//     int windowWidth(const QPixmap &pixmap) const;

    bool m_pixmapExists = false;
    bool m_unsaved = false;
    QUrl m_screenshotUrl;

    // exists for automatically positioning QDialogs
    const std::unique_ptr<QWidget> m_placeholderParent;

    QPointer<QMenu> m_helpMenu;
    const std::unique_ptr<SpectacleMenu> m_toolsMenu;

    // TODO: Remove this when recording functionality is added
    QPointer<QMenu> m_screenRecorderToolsMenu;
    std::unique_ptr<KMoreToolsMenuFactory> m_screenRecorderToolsMenuFactory;

    const std::unique_ptr<ExportMenu> m_exportMenu;
    const std::unique_ptr<SpectacleMenu> m_clipboardMenu;
    const std::unique_ptr<SpectacleMenu> m_saveMenu;
};
