/*
 *  SPDX-FileCopyrightText: 2019 David Redondo <kde@david-redondo.de>
 *  SPDX-FileCopyrightText: 2015 Boudhayan Gupta <bgupta@kde.org>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "SpectacleCore.h"
#include "CaptureModeModel.h"
#include "spectacle_core_debug.h"

#include "Config.h"
#include "ShortcutActions.h"
#include "settings.h"

#include <KGlobalAccel>
#include <KIO/OpenUrlJob>
#include <KLocalizedString>
#include <KMessageBox>
#include <KNotification>
#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/plasmashell.h>
#include <KWayland/Client/registry.h>
#include <KWindowSystem>

#include <QApplication>
#include <QClipboard>
#include <QCommandLineParser>
#include <QDir>
#include <QDrag>
#include <QKeySequence>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QScopedPointer>
#include <QScreen>
#include <QTimer>

#include <memory>

SpectacleCore::SpectacleCore(QObject *parent)
    : QObject(parent)
{
}

void SpectacleCore::init()
{
    m_platform = loadPlatform();
    auto platform = m_platform.get();

    // essential connections
    connect(this, &SpectacleCore::errorMessage, this, &SpectacleCore::showErrorMessage);
    connect(platform, &Platform::newScreenshotTaken, this, &SpectacleCore::onScreenshotUpdated);
    connect(platform, &Platform::newScreensScreenshotTaken, this, &SpectacleCore::onScreenshotsUpdated);
    connect(platform, &Platform::newScreenshotFailed, this, &SpectacleCore::onScreenshotFailed);

    // set up CaptureMode model
    m_captureModeModel = std::make_unique<CaptureModeModel>(platform->supportedGrabModes(), this);
    auto captureModeModel = m_captureModeModel.get();
    connect(platform, &Platform::supportedGrabModesChanged, captureModeModel, [captureModeModel, platform](){
        captureModeModel->setGrabModes(platform->supportedGrabModes());
    });

    // set up the export manager
    auto lExportManager = ExportManager::instance();
    connect(lExportManager, &ExportManager::errorMessage, this, &SpectacleCore::showErrorMessage);
    connect(lExportManager, &ExportManager::imageSaved, this, &SpectacleCore::doCopyPath);
    connect(lExportManager, &ExportManager::forceNotify, this, &SpectacleCore::doNotify);
    connect(platform, &Platform::windowTitleChanged, lExportManager, &ExportManager::setWindowTitle);

    // Needed so the QuickEditor can go fullscreen on wayland
    if (KWindowSystem::isPlatformWayland()) {
        using namespace KWayland::Client;
        ConnectionThread *connection = ConnectionThread::fromApplication(this);
        if (!connection) {
            return;
        }
        Registry *registry = new Registry(this);
        registry->create(connection);
        connect(registry, &Registry::plasmaShellAnnounced, this, [this, registry](quint32 name, quint32 version) {
            m_waylandPlasmashell = registry->createPlasmaShell(name, version, this);
        });
        registry->setup();
        connection->roundtrip();
    }

    // set up shortcuts
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->openAction(), Qt::Key_Print);
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->fullScreenAction(), Qt::SHIFT | Qt::Key_Print);
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->activeWindowAction(), Qt::META | Qt::Key_Print);
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->windowUnderCursorAction(), Qt::META | Qt::CTRL | Qt::Key_Print);
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->regionAction(), Qt::META | Qt::SHIFT | Qt::Key_Print);
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->currentScreenAction(), QList<QKeySequence>());
}

void SpectacleCore::onActivateRequested(QStringList arguments, const QString & /*workingDirectory */)
{
    // QCommandLineParser expects the first argument to be the executable name
    // In the current version it just strips it away
    arguments.prepend(qApp->applicationFilePath());

    // We can't re-use QCommandLineParser instances, it preserves earlier parsed values
    QScopedPointer<QCommandLineParser> parser(new QCommandLineParser);
    populateCommandLineParser(parser.data());
    parser->parse(arguments);

    m_startMode = SpectacleCore::StartMode::Gui;
    m_existingLoaded = false;
    m_notify = true;
    qint64 lDelayMsec = 0;

    // are we ask to run in background or dbus mode?
    if (parser->isSet(QStringLiteral("background"))) {
        m_startMode = SpectacleCore::StartMode::Background;
    } else if (parser->isSet(QStringLiteral("dbus"))) {
        m_startMode = SpectacleCore::StartMode::DBus;
    }

    m_editExisting = parser->isSet(QStringLiteral("edit-existing"));
    if (m_editExisting) {
        QString lExistingFileName = parser->value(QStringLiteral("edit-existing"));
        if (!(lExistingFileName.isEmpty() || lExistingFileName.isNull())) {
            setScreenCaptureUrl(lExistingFileName);
            m_saveToOutput = true;
        }
    }

    auto lOnClickAvailable = m_platform->supportedShutterModes().testFlag(Platform::ShutterMode::OnClick);
    if ((!lOnClickAvailable) && (lDelayMsec < 0)) {
        lDelayMsec = 0;
    }

    // reset last region if it should not be remembered across restarts
    if (!(Settings::rememberLastRectangularRegion() == Settings::EnumRememberLastRectangularRegion::Always)) {
        Settings::setCropRegion({0, 0, 0, 0});
    }

    CaptureModeModel::CaptureMode lCaptureMode = CaptureModeModel::AllScreens;
    // extract the capture mode
    if (parser->isSet(QStringLiteral("fullscreen"))) {
        lCaptureMode = CaptureModeModel::AllScreens;
    } else if (parser->isSet(QStringLiteral("current"))) {
        lCaptureMode = CaptureModeModel::CurrentScreen;
    } else if (parser->isSet(QStringLiteral("activewindow"))) {
        lCaptureMode = CaptureModeModel::ActiveWindow;
    } else if (parser->isSet(QStringLiteral("region"))) {
        lCaptureMode = CaptureModeModel::RectangularRegion;
    } else if (parser->isSet(QStringLiteral("windowundercursor"))) {
        lCaptureMode = CaptureModeModel::TransientWithParent;
    } else if (parser->isSet(QStringLiteral("transientonly"))) {
        lCaptureMode = CaptureModeModel::WindowUnderCursor;
    } else if (m_startMode == SpectacleCore::StartMode::Gui
               && (parser->isSet(QStringLiteral("launchonly")) || Settings::launchAction() == Settings::EnumLaunchAction::DoNotTakeScreenshot)
               && !m_editExisting) {
        initGuiNoScreenshot();
        return;
    } else if (Settings::launchAction() == Settings::EnumLaunchAction::UseLastUsedCapturemode && !m_editExisting) {
        lCaptureMode = CaptureModeModel::CaptureMode(Settings::captureMode());
        if (Settings::captureOnClick()) {
            lDelayMsec = -1;
            takeNewScreenshot(lCaptureMode, lDelayMsec, Settings::includePointer(), Settings::includeDecorations());
        }
    }

    auto lExportManager = ExportManager::instance();
    lExportManager->setCaptureMode(lCaptureMode);

    switch (m_startMode) {
    case StartMode::DBus:
        m_copyImageToClipboard = Settings::clipboardGroup() == Settings::EnumClipboardGroup::PostScreenshotCopyImage;
        m_copyLocationToClipboard = Settings::clipboardGroup() == Settings::EnumClipboardGroup::PostScreenshotCopyLocation;

        qApp->setQuitOnLastWindowClosed(false);
        break;

    case StartMode::Background: {
        m_copyImageToClipboard = false;
        m_copyLocationToClipboard = false;
        m_saveToOutput = true;

        if (parser->isSet(QStringLiteral("nonotify"))) {
            m_notify = false;
        }

        if (parser->isSet(QStringLiteral("copy-image"))) {
            m_saveToOutput = false;
            m_copyImageToClipboard = true;
        } else if (parser->isSet(QStringLiteral("copy-path"))) {
            m_copyLocationToClipboard = true;
        }

        if (parser->isSet(QStringLiteral("output"))) {
            m_saveToOutput = true;
            QString lFileName = parser->value(QStringLiteral("output"));
            if (!(lFileName.isEmpty() || lFileName.isNull())) {
                setScreenCaptureUrl(lFileName);
            }
        }

        if (parser->isSet(QStringLiteral("delay"))) {
            bool lParseOk = false;
            qint64 lDelayValue = parser->value(QStringLiteral("delay")).toLongLong(&lParseOk);
            if (lParseOk) {
                lDelayMsec = lDelayValue;
            }
        }

        if (parser->isSet(QStringLiteral("onclick"))) {
            lDelayMsec = -1;
        }

        if (!m_isGuiInited) {
            static_cast<QApplication *>(qApp->instance())->setQuitOnLastWindowClosed(false);
        }

        auto lIncludePointer = false;
        auto lIncludeDecorations = true;

        if (parser->isSet(QStringLiteral("pointer"))) {
            lIncludePointer = true;
        }

        if (parser->isSet(QStringLiteral("no-decoration"))) {
            lIncludeDecorations = false;
        }

        takeNewScreenshot(lCaptureMode, lDelayMsec, lIncludePointer, lIncludeDecorations);
    } break;

    case StartMode::Gui:
        if (!m_isGuiInited) {
            initGui(lDelayMsec, Settings::includePointer(), Settings::includeDecorations());
        } else {
            using Actions = Settings::EnumPrintKeyActionRunning;
            switch (Settings::printKeyActionRunning()) {
            case Actions::TakeNewScreenshot: {
                // 0 means Immediate, -1 onClick
                int timeout = m_platform->supportedShutterModes().testFlag(Platform::ShutterMode::Immediate) ? 0 : -1;
                takeNewScreenshot(CaptureModeModel::CaptureMode(Settings::captureMode()), timeout, Settings::includePointer(), Settings::includeDecorations());
                break;
            }
            case Actions::FocusWindow:
                if (m_mainWindow->visibility() == QWindow::Hidden) {
                    m_mainWindow->show();
                }
                // NOTE: A window can be visible and minimized at the same time.
                if (m_mainWindow->windowState() & Qt::WindowMinimized) {
                    // Unminimize the window.
                    m_mainWindow->setWindowState(Qt::WindowState(m_mainWindow->windowState() & ~Qt::WindowMinimized));
                }
                m_mainWindow->requestActivate();
                break;
            case Actions::StartNewInstance:
                QProcess newInstance;
                newInstance.setProgram(QStringLiteral("spectacle"));
                newInstance.setArguments({QStringLiteral("--new-instance")});
                newInstance.startDetached();
                break;
            }
        }

        break;
    }
}

CaptureModeModel *SpectacleCore::captureModeModel() const
{
    return m_captureModeModel.get();
}

QUrl SpectacleCore::screenCaptureUrl() const
{
    return m_screenCaptureUrl;
}

void SpectacleCore::setScreenCaptureUrl(const QUrl &url)
{
    if(m_screenCaptureUrl == url) {
        return;
    }
    m_screenCaptureUrl = url;
    Q_EMIT screenCaptureUrlChanged();
}

void SpectacleCore::setScreenCaptureUrl(const QString &filePath)
{
    if (QDir::isRelativePath(filePath)) {
        setScreenCaptureUrl(QUrl::fromUserInput(QDir::current().absoluteFilePath(filePath)));
    } else {
        setScreenCaptureUrl(QUrl::fromUserInput(filePath));
    }
}

void SpectacleCore::takeNewScreenshot(int captureMode, int timeout, bool includePointer, bool includeDecorations)
{
    ExportManager::instance()->setCaptureMode(CaptureModeModel::CaptureMode(captureMode));
    auto lGrabMode = toPlatformGrabMode(CaptureModeModel::CaptureMode(captureMode));

    if (timeout < 0 || !m_platform->supportedShutterModes().testFlag(Platform::ShutterMode::Immediate)) {
        m_platform->doGrab(Platform::ShutterMode::OnClick, lGrabMode, includePointer, includeDecorations);
        return;
    }

    // when compositing is enabled, we need to give it enough time for the window
    // to disappear and all the effects are complete before we take the shot. there's
    // no way of knowing how long the disappearing effects take, but as per default
    // settings (and unless the user has set an extremely slow effect), 200
    // milliseconds is a good amount of wait time.

    auto lMsec = KWindowSystem::compositingActive() ? 200 : 50;
    QTimer::singleShot(timeout + lMsec, this, [this, lGrabMode, includePointer, includeDecorations]() {
        m_platform->doGrab(Platform::ShutterMode::Immediate, lGrabMode, includePointer, includeDecorations);
    });
}

void SpectacleCore::showErrorMessage(const QString &theErrString)
{
    qCDebug(SPECTACLE_CORE_LOG) << "ERROR: " << theErrString;

    if (m_startMode == StartMode::Gui) {
        KMessageBox::error(nullptr, theErrString);
    }
}

void SpectacleCore::onScreenshotsUpdated(const QVector<QImage> &imgs)
{
    QMap<const QScreen *, QImage> mapScreens;
    QList<QScreen *> screens = QApplication::screens();

    if (imgs.length() != screens.size()) {
        qWarning(SPECTACLE_CORE_LOG()) << "ERROR: images received from KWin do not match, expected:" << imgs.length() << "actual:" << screens.size();
        return;
    }

    // only used by CaptureModeModel::RectangularRegion
    auto it = imgs.constBegin();
    for (const QScreen *screen : screens) {
        mapScreens.insert(screen, *it);
        ++it;
    }

    //TODO FIXME
//     m_quickEditor = std::make_unique<QuickEditor>(mapScreens, m_waylandPlasmashell);
//     connect(m_quickEditor.get(), &QuickEditor::grabDone, this, &SpectacleCore::onScreenshotUpdated);
//     connect(m_quickEditor.get(), &QuickEditor::grabCancelled, this, &SpectacleCore::onScreenshotCanceled);
//     m_quickEditor->show();
}

void SpectacleCore::onScreenshotUpdated(const QPixmap &thePixmap)
{
    QPixmap existingPixmap;
    const QPixmap &pixmapUsed = (m_editExisting && !m_existingLoaded) ? existingPixmap : thePixmap;
    if (m_editExisting && !m_existingLoaded) {
        existingPixmap.load(m_screenCaptureUrl.toLocalFile());
    }

    auto lExportManager = ExportManager::instance();

    if (lExportManager->captureMode() == CaptureModeModel::RectangularRegion) {
        if (m_quickEditor) {
            //TODO FIXME
//             m_quickEditor->hide();
            m_quickEditor.reset(nullptr);
        }
    }

    lExportManager->setPixmap(pixmapUsed);
    lExportManager->updatePixmapTimestamp();

    switch (m_startMode) {
    case StartMode::Background:
    case StartMode::DBus: {
        if (m_saveToOutput || !m_copyImageToClipboard || (Settings::autoSaveImage() && !m_saveToOutput)) {
            m_saveToOutput = Settings::autoSaveImage();
            QUrl lSavePath = (m_startMode == StartMode::Background && m_screenCaptureUrl.isValid() && m_screenCaptureUrl.isLocalFile()) ? m_screenCaptureUrl : QUrl();
            lExportManager->doSave(lSavePath, m_notify);
        }

        if (m_copyImageToClipboard) {
            lExportManager->doCopyToClipboard(m_notify);
        } else if (m_copyLocationToClipboard) {
            lExportManager->doCopyLocationToClipboard(m_notify);
        }

        // if we don't have a Gui already opened, Q_EMIT allDone
        if (!m_isGuiInited) {
            // if we notify, we Q_EMIT allDone only if the user either dismissed the notification or pressed
            // the "Open" button, otherwise the app closes before it can react to it.
            if (!m_notify && m_copyImageToClipboard) {
                // Allow some time for clipboard content to transfer if '--nonotify' is used, see Bug #411263
                // TODO: Find better solution
                QTimer::singleShot(250, this, &SpectacleCore::allDone);
            } else if (!m_notify) {
                Q_EMIT allDone();
            }
        }
    } break;
    case StartMode::Gui:
        if (pixmapUsed.isNull()) {
            //TODO FIXME
            m_mainWindow->setScreenshotAndShow(pixmapUsed, false);
            m_mainWindow->setPlaceholderTextOnLaunch();
            return;
        }
        m_mainWindow->setScreenshotAndShow(pixmapUsed, m_editExisting);

        bool autoSaveImage = Settings::autoSaveImage();
        m_copyImageToClipboard = Settings::clipboardGroup() == Settings::EnumClipboardGroup::PostScreenshotCopyImage;
        m_copyLocationToClipboard = Settings::clipboardGroup() == Settings::EnumClipboardGroup::PostScreenshotCopyLocation;

        if (autoSaveImage && m_copyImageToClipboard) {
            lExportManager->doSaveAndCopy();
        } else if (autoSaveImage) {
            lExportManager->doSave();
        } else if (m_copyImageToClipboard) {
            lExportManager->doCopyToClipboard(false);
        } else if (m_copyLocationToClipboard) {
            lExportManager->doCopyLocationToClipboard(false);
        }
    }

    if (m_editExisting && !m_existingLoaded) {
        Settings::setLastSaveLocation(m_screenCaptureUrl);
        //TODO FIXME
        m_mainWindow->onImageSaved(m_screenCaptureUrl);
        m_existingLoaded = true;
    }
}

void SpectacleCore::onScreenshotCanceled()
{
    //TODO FIXME
//     m_quickEditor->hide();
    m_quickEditor.reset(nullptr);
    if (m_startMode == StartMode::Gui) {
        m_mainWindow->setScreenshotAndShow(QPixmap(), false);
    } else {
        Q_EMIT allDone();
    }
}

void SpectacleCore::onScreenshotFailed()
{
    if (ExportManager::instance()->captureMode() == CaptureModeModel::RectangularRegion && m_quickEditor) {
        //TODO FIXME
//         m_quickEditor->hide();
        m_quickEditor.reset(nullptr);
    }

    switch (m_startMode) {
    case StartMode::Background:
        showErrorMessage(i18n("Screenshot capture canceled or failed"));
        Q_EMIT allDone();
        return;
    case StartMode::DBus:
        Q_EMIT grabFailed();
        Q_EMIT allDone();
        return;
    case StartMode::Gui:
        //TODO FIXME
        m_mainWindow->onScreenshotFailed();
        m_mainWindow->setScreenshotAndShow(QPixmap(), false);
        return;
    }
}

void SpectacleCore::doNotify(const QUrl &theSavedAt)
{
    KNotification *lNotify = new KNotification(QStringLiteral("newScreenshotSaved"));

    switch (ExportManager::instance()->captureMode()) {
    case CaptureModeModel::AllScreens:
    case CaptureModeModel::AllScreensScaled:
        lNotify->setTitle(i18nc("The entire screen area was captured, heading", "Full Screen Captured"));
        break;
    case CaptureModeModel::CurrentScreen:
        lNotify->setTitle(i18nc("The current screen was captured, heading", "Current Screen Captured"));
        break;
    case CaptureModeModel::ActiveWindow:
        lNotify->setTitle(i18nc("The active window was captured, heading", "Active Window Captured"));
        break;
    case CaptureModeModel::WindowUnderCursor:
    case CaptureModeModel::TransientWithParent:
        lNotify->setTitle(i18nc("The window under the mouse was captured, heading", "Window Under Cursor Captured"));
        break;
    case CaptureModeModel::RectangularRegion:
        lNotify->setTitle(i18nc("A rectangular region was captured, heading", "Rectangular Region Captured"));
        break;
    default:
        break;
    }

    // a speaking message is prettier than a URL, special case for copy image/location to clipboard and the default pictures location
    const QString &lSavePath = theSavedAt.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash).path();

    if (m_copyImageToClipboard && theSavedAt.fileName().isEmpty()) {
        lNotify->setText(i18n("A screenshot was saved to your clipboard."));
    } else if (m_copyLocationToClipboard && !theSavedAt.fileName().isEmpty()) {
        lNotify->setText(i18n("A screenshot was saved as '%1' to '%2' and the file path of the screenshot has been saved to your clipboard.",
                              theSavedAt.fileName(),
                              lSavePath));
    } else if (lSavePath == QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)) {
        lNotify->setText(i18nc("Placeholder is filename", "A screenshot was saved as '%1' to your Pictures folder.", theSavedAt.fileName()));
    } else if (!theSavedAt.fileName().isEmpty()) {
        lNotify->setText(i18n("A screenshot was saved as '%1' to '%2'.", theSavedAt.fileName(), lSavePath));
    }

    if (!theSavedAt.isEmpty()) {
        lNotify->setUrls({theSavedAt});
        lNotify->setDefaultAction(i18nc("Open the screenshot we just saved", "Open"));
        connect(lNotify, &KNotification::defaultActivated, this, [this, theSavedAt]() {
            auto job = new KIO::OpenUrlJob(theSavedAt);
            job->start();
            QTimer::singleShot(250, this, [this] {
                if (!m_isGuiInited || Settings::quitAfterSaveCopyExport()) {
                    Q_EMIT allDone();
                }
            });
        });
        lNotify->setActions({i18n("Annotate")});
        connect(lNotify, &KNotification::action1Activated, this, [theSavedAt]() {
            QProcess newInstance;
            newInstance.setProgram(QStringLiteral("spectacle"));
            newInstance.setArguments({QStringLiteral("--new-instance"), QStringLiteral("--edit-existing"), theSavedAt.toLocalFile()});
            newInstance.startDetached();
        });
    }

    connect(lNotify, &QObject::destroyed, this, [this] {
        if (!m_isGuiInited || Settings::quitAfterSaveCopyExport()) {
            Q_EMIT allDone();
        }
    });

    lNotify->sendEvent();
}

void SpectacleCore::doCopyPath(const QUrl &savedAt)
{
    if (Settings::clipboardGroup() == Settings::EnumClipboardGroup::PostScreenshotCopyLocation) {
        qApp->clipboard()->setText(savedAt.toLocalFile());
    }
}

void SpectacleCore::populateCommandLineParser(QCommandLineParser *lCmdLineParser)
{
    lCmdLineParser->addOptions({
        {{QStringLiteral("f"), QStringLiteral("fullscreen")}, i18n("Capture the entire desktop (default)")},
        {{QStringLiteral("m"), QStringLiteral("current")}, i18n("Capture the current monitor")},
        {{QStringLiteral("a"), QStringLiteral("activewindow")}, i18n("Capture the active window")},
        {{QStringLiteral("u"), QStringLiteral("windowundercursor")}, i18n("Capture the window currently under the cursor, including parents of pop-up menus")},
        {{QStringLiteral("t"), QStringLiteral("transientonly")}, i18n("Capture the window currently under the cursor, excluding parents of pop-up menus")},
        {{QStringLiteral("r"), QStringLiteral("region")}, i18n("Capture a rectangular region of the screen")},
        {{QStringLiteral("l"), QStringLiteral("launchonly")}, i18n("Launch Spectacle without taking a screenshot")},
        {{QStringLiteral("g"), QStringLiteral("gui")}, i18n("Start in GUI mode (default)")},
        {{QStringLiteral("b"), QStringLiteral("background")}, i18n("Take a screenshot and exit without showing the GUI")},
        {{QStringLiteral("s"), QStringLiteral("dbus")}, i18n("Start in DBus-Activation mode")},
        {{QStringLiteral("n"), QStringLiteral("nonotify")}, i18n("In background mode, do not pop up a notification when the screenshot is taken")},
        {{QStringLiteral("o"), QStringLiteral("output")}, i18n("In background mode, save image to specified file"), QStringLiteral("fileName")},
        {{QStringLiteral("d"), QStringLiteral("delay")},
         i18n("In background mode, delay before taking the shot (in milliseconds)"),
         QStringLiteral("delayMsec")},
        {{QStringLiteral("c"), QStringLiteral("copy-image")}, i18n("In background mode, copy screenshot image to clipboard, unless -o is also used.")},
        {{QStringLiteral("C"), QStringLiteral("copy-path")}, i18n("In background mode, copy screenshot file path to clipboard")},
        {{QStringLiteral("w"), QStringLiteral("onclick")}, i18n("Wait for a click before taking screenshot. Invalidates delay")},
        {{QStringLiteral("i"), QStringLiteral("new-instance")}, i18n("Starts a new GUI instance of spectacle without registering to DBus")},
        {{QStringLiteral("p"), QStringLiteral("pointer")}, i18n("In background mode, include pointer in the screenshot")},
        {{QStringLiteral("e"), QStringLiteral("no-decoration")}, i18n("In background mode, exclude decorations in the screenshot")},
        {{QStringLiteral("E"), QStringLiteral("edit-existing")}, i18n("Open and edit existing screenshot file"), QStringLiteral("existingFileName")},
    });
}

void SpectacleCore::doStartDragAndDrop()
{
    auto lExportManager = ExportManager::instance();
    if (lExportManager->pixmap().isNull()) {
        return;
    }
    QUrl lTempFile = lExportManager->tempSave();
    if (!lTempFile.isValid()) {
        return;
    }

    auto lMimeData = new QMimeData;
    lMimeData->setUrls(QList<QUrl>{lTempFile});
    lMimeData->setData(QStringLiteral("application/x-kde-suggestedfilename"), QFile::encodeName(lTempFile.fileName()));

    auto lDragHandler = new QDrag(this);
    lDragHandler->setMimeData(lMimeData);
    lDragHandler->setPixmap(lExportManager->pixmap().scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    lDragHandler->exec(Qt::CopyAction);
}

// Private

Platform::GrabMode SpectacleCore::toPlatformGrabMode(CaptureModeModel::CaptureMode theCaptureMode)
{
    switch (theCaptureMode) {
    case CaptureModeModel::AllScreens:
        return Platform::GrabMode::AllScreens;
    case CaptureModeModel::CurrentScreen:
        return Platform::GrabMode::CurrentScreen;
    case CaptureModeModel::ActiveWindow:
        return Platform::GrabMode::ActiveWindow;
    case CaptureModeModel::WindowUnderCursor:
        return Platform::GrabMode::WindowUnderCursor;
    case CaptureModeModel::TransientWithParent:
        return Platform::GrabMode::TransientWithParent;
    case CaptureModeModel::RectangularRegion:
        return Platform::GrabMode::PerScreenImageNative;
    case CaptureModeModel::AllScreensScaled:
        return Platform::GrabMode::AllScreensScaled;
    default:
        return Platform::GrabMode::InvalidChoice;
    }
}

QQmlEngine *SpectacleCore::getQmlEngine()
{
    if (m_qmlEngine == nullptr) {
        m_qmlEngine = std::make_unique<QQmlEngine>(this);
        qmlRegisterSingletonInstance("org.kde.spectacle.private", 1, 0, "SpectacleCore", this);
        qmlRegisterSingletonInstance("org.kde.spectacle.private", 1, 0, "Platform", m_platform.get());
        qmlRegisterSingletonInstance("org.kde.spectacle.private", 1, 0, "Settings", Settings::self());
        qmlRegisterUncreatableType<CaptureModeModel>("org.kde.spectacle.private", 1, 0, "CaptureModeModel", QStringLiteral("Use SpectacleCore.captureModeModel"));
    }
    return m_qmlEngine.get();
}

SpectacleMainWindow *SpectacleCore::getMainWindow()
{
    if (m_mainWindow == nullptr) {
        m_mainWindow = std::make_unique<SpectacleMainWindow>(getQmlEngine());
        auto mainWindow = m_mainWindow.get();
        connect(mainWindow, &SpectacleMainWindow::newScreenshotRequest, this, &SpectacleCore::takeNewScreenshot);
        connect(mainWindow, &SpectacleMainWindow::dragAndDropRequest, this, &SpectacleCore::doStartDragAndDrop);
        connect(mainWindow, &QObject::destroyed, Settings::self(), &Settings::save);
    }
    return m_mainWindow.get();
}

void SpectacleCore::ensureGuiInitiad()
{
    if (!m_isGuiInited) {
        getMainWindow();

        //TODO add QuickEditor

        m_isGuiInited = true;
    }
}

void SpectacleCore::initGui(int theDelay, bool theIncludePointer, bool theIncludeDecorations)
{
    ensureGuiInitiad();
    takeNewScreenshot(ExportManager::instance()->captureMode(), theDelay, theIncludePointer, theIncludeDecorations);
}

void SpectacleCore::initGuiNoScreenshot()
{
    ensureGuiInitiad();
    onScreenshotUpdated(QPixmap());
}
