/* This file is part of Spectacle, the KDE screenshot utility
 * SPDX-FileCopyrightText: 2019 Boudhayan Gupta <bgupta@kde.org>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

class QCommandLineParser;
#include <QObject>
#include <QQmlEngine>
#include <QQuickItem>

#include "ExportManager.h"
#include "Gui/SpectacleMainWindow.h"
#include "Platforms/PlatformLoader.h"
#include "QuickEditor/QuickEditor.h"

#include <memory>

namespace KWayland
{
namespace Client
{
class PlasmaShell;
}
}

class SpectacleCore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(CaptureModeModel *captureModeModel READ captureModeModel CONSTANT FINAL)
    Q_PROPERTY(QUrl screenCaptureUrl READ screenCaptureUrl NOTIFY screenCaptureUrlChanged FINAL)
public:
    enum class StartMode {
        Gui = 0,
        DBus = 1,
        Background = 2,
    };

    explicit SpectacleCore(QObject *parent = nullptr);
    ~SpectacleCore() override = default;
    void init();

    CaptureModeModel *captureModeModel() const;

    QUrl screenCaptureUrl() const;
    void setScreenCaptureUrl(const QUrl &url);
    // Used when setting the URL from CLI
    void setScreenCaptureUrl(const QString &filePath);

    void populateCommandLineParser(QCommandLineParser *lCmdLineParser);

    void initGuiNoScreenshot();

public Q_SLOTS:
    void takeNewScreenshot(int captureMode, int timeout, bool includePointer, bool includeDecorations);
    void showErrorMessage(const QString &theErrString);
    void onScreenshotUpdated(const QPixmap &thePixmap);
    void onScreenshotsUpdated(const QVector<QImage> &imgs);
    void onScreenshotCanceled();
    void onScreenshotFailed();
    void doStartDragAndDrop();
    void doNotify(const QUrl &theSavedAt);
    void doCopyPath(const QUrl &savedAt);

    void onActivateRequested(QStringList arguments, const QString & /*workingDirectory */);

Q_SIGNALS:
    void screenCaptureUrlChanged();

    void errorMessage(const QString &errString);
    void allDone();
    void grabFailed();

private:
    QQmlEngine *getQmlEngine();
    SpectacleMainWindow *getMainWindow();

    void ensureGuiInitiad();
    void initGui(int theDelay, bool theIncludePointer, bool theIncludeDecorations);

    Platform::GrabMode toPlatformGrabMode(CaptureModeModel::CaptureMode theCaptureMode);

    StartMode m_startMode;
    bool m_notify;
    QUrl m_screenCaptureUrl;
    PlatformPtr m_platform;
    std::unique_ptr<QQmlEngine> m_qmlEngine;
    std::unique_ptr<CaptureModeModel> m_captureModeModel;
    std::unique_ptr<SpectacleMainWindow> m_mainWindow;
//     MainWindowPtr mMainWindow = nullptr;
    std::unique_ptr<QQuickItem> m_quickEditor;
//     EditorPtr mQuickEditor;
    bool m_isGuiInited = false;
    bool m_copyImageToClipboard;
    bool m_copyLocationToClipboard;
    bool m_saveToOutput;
    bool m_editExisting;
    bool m_existingLoaded;

    KWayland::Client::PlasmaShell *m_waylandPlasmashell = nullptr;
};
