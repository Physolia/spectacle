/*
 *  SPDX-FileCopyrightText: 2019 David Redondo <kde@david-redondo.de>
 *  SPDX-FileCopyrightText: 2015 Boudhayan Gupta <bgupta@kde.org>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "SpectacleCore.h"
#include "Gui/Annotations/AnnotationViewport.h"
#include "Gui/CaptureWindow.h"
#include "Gui/Selection.h"
#include "Gui/SelectionEditor.h"
#include "Gui/SpectacleImageProvider.h"
#include "Gui/SpectacleWindow.h"
#include "ShortcutActions.h"
// generated
#include "Config.h"
#include "spectacle_core_debug.h"

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
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QDrag>
#include <QKeySequence>
#include <QMimeData>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScopedPointer>
#include <QScreen>
#include <QTimer>
#include <QtMath>
#include <utility>

SpectacleCore *SpectacleCore::s_self = nullptr;

SpectacleCore::SpectacleCore(QObject *parent)
    : QObject(parent)
{
    s_self = this;
    m_annotationSyncTimer = std::make_unique<QTimer>(new QTimer(this));
    m_annotationSyncTimer->setInterval(400);
    m_annotationSyncTimer->setSingleShot(true);

    m_delayAnimation = std::make_unique<QVariantAnimation>(this);
    m_delayAnimation->setStartValue(0.0);
    m_delayAnimation->setEndValue(1.0);
    m_delayAnimation->setDuration(1);
    m_delayAnimation->setCurrentTime(0);
    auto delayAnimation = m_delayAnimation.get();
    // We need to reset this on start in case a previous instance
    // didn't reset these before it closed or crashed.
    unityLauncherUpdate({
        {QStringLiteral("progress-visible"), false},
        {QStringLiteral("progress"), 0}
    });
    using State = QVariantAnimation::State;
    auto onStateChanged = [this](State newState, State oldState) {
        Q_UNUSED(oldState)
        if (newState == State::Running) {
            unityLauncherUpdate({{QStringLiteral("progress-visible"), true}});
        } else if (newState == State::Stopped) {
            unityLauncherUpdate({{QStringLiteral("progress-visible"), false}});
            m_delayAnimation->setCurrentTime(0);
        }
    };
    auto onValueChanged = [this](const QVariant &value) {
        Q_EMIT captureTimeRemainingChanged();
        Q_EMIT captureProgressChanged();
        unityLauncherUpdate({{QStringLiteral("progress"), value.toReal()}});
        const auto windows = SpectacleWindow::instances();
        if (m_delayAnimation->state() != State::Stopped && !windows.isEmpty()) {
            if (captureTimeRemaining() <= 500 && windows.constFirst()->isVisible()) {
                SpectacleWindow::setVisibilityForAll(QWindow::Hidden);
            }
            SpectacleWindow::setTitleForAll(SpectacleWindow::Timer);
        }
    };
    auto onFinished = [this]() {
        m_platform->doGrab(Platform::ShutterMode::Immediate, m_tempGrabMode,
                           m_tempIncludePointer, m_tempIncludeDecorations);
        setVideoMode(false);
    };
    QObject::connect(delayAnimation, &QVariantAnimation::stateChanged,
                     this, onStateChanged, Qt::QueuedConnection);
    QObject::connect(delayAnimation, &QVariantAnimation::valueChanged,
                     this, onValueChanged, Qt::QueuedConnection);
    QObject::connect(delayAnimation, &QVariantAnimation::finished,
                     this, onFinished, Qt::QueuedConnection);
}

SpectacleCore::~SpectacleCore() noexcept
{
    s_self = nullptr;
    m_waylandPlasmashell = nullptr;
}

void SpectacleCore::init()
{
    m_platform = loadPlatform();
    m_videoPlatform = loadVideoPlatform();
    auto platform = m_platform.get();
    m_annotationDocument = std::make_unique<AnnotationDocument>(new AnnotationDocument(this));

    // essential connections
    connect(this, &SpectacleCore::errorMessage, this, &SpectacleCore::showErrorMessage);
    connect(this, &SpectacleCore::grabDone, this, [this](const QPixmap &pixmap){
        // only clear images because we're transitioning from rectangle capture to image view.
        m_annotationDocument->clearImages();
        if (m_startMode != StartMode::Gui) {
            SpectacleWindow::setVisibilityForAll(QWindow::Hidden);
        }
        onScreenshotUpdated(pixmap);
    });

    connect(platform, &Platform::newScreenshotTaken, this, [this](const QPixmap &pixmap){
        m_annotationDocument->clear();
        onScreenshotUpdated(pixmap);
    });
    connect(platform, &Platform::newScreensScreenshotTaken, this, [this](const QVector<ScreenImage> &screenImages) {
        SelectionEditor::instance()->setScreenImages(screenImages);
        m_annotationDocument->clear();
        for (const auto &img : SelectionEditor::instance()->screenImages()) {
            QImage image(img.image);
            if (KWindowSystem::isPlatformWayland()) {
                image.setDevicePixelRatio(qreal(image.width()) / img.screen->geometry().width());
            } else {
                image.setDevicePixelRatio(qApp->devicePixelRatio());
            }
            m_annotationDocument->addImage(image, img.screen->geometry().topLeft());
        }

        auto remember = Settings::rememberLastRectangularRegion();
        if (remember == Settings::Never) {
            SelectionEditor::instance()->selection()->setRect({});
        } else if (remember == Settings::Always) {
            SelectionEditor::instance()->selection()->setRect(Settings::cropRegion());
        }

        initCaptureWindows(CaptureWindow::Image);
        SpectacleWindow::setVisibilityForAll(QWindow::FullScreen);
    });
    connect(platform, &Platform::newScreenshotFailed, this, &SpectacleCore::onScreenshotFailed);

    // set up the export manager
    auto exportManager = ExportManager::instance();
    connect(exportManager, &ExportManager::errorMessage, this, &SpectacleCore::showErrorMessage);
    connect(exportManager, &ExportManager::forceNotify, this, &SpectacleCore::doNotify);
    connect(platform, &Platform::windowTitleChanged, exportManager, &ExportManager::setWindowTitle);
    connect(m_annotationDocument.get(), &AnnotationDocument::repaintNeeded, m_annotationSyncTimer.get(), qOverload<>(&QTimer::start));
    connect(m_annotationSyncTimer.get(), &QTimer::timeout, this, &SpectacleCore::syncExportPixmap);

    connect(exportManager, &ExportManager::imageSaved, this, [this](const QUrl &savedAt){
        if (Settings::clipboardGroup() == Settings::EnumClipboardGroup::PostScreenshotCopyLocation) {
            qApp->clipboard()->setText(savedAt.toLocalFile());
        }
        SpectacleWindow::setTitleForAll(SpectacleWindow::Saved, savedAt.fileName());
        if (m_viewerWindow) {
            m_viewerWindow->showSavedScreenshotMessage(savedAt);
        }
    });
    connect(exportManager, &ExportManager::imageCopied, this, [this](){
        if (m_viewerWindow) {
            m_viewerWindow->showCopiedMessage();
        }
    });
    connect(exportManager, &ExportManager::imageLocationCopied, this, [this](const QUrl &savedAt){
        SpectacleWindow::setTitleForAll(SpectacleWindow::Saved, savedAt.fileName());
        if (m_viewerWindow) {
            m_viewerWindow->showSavedAndLocationCopiedMessage(savedAt);
        }
    });
    connect(exportManager, &ExportManager::imageSavedAndCopied, this, [this](const QUrl &savedAt){
        SpectacleWindow::setTitleForAll(SpectacleWindow::Saved, savedAt.fileName());
        if (m_viewerWindow) {
            m_viewerWindow->showSavedAndCopiedMessage(savedAt);
        }
    });

    if (KWindowSystem::isPlatformWayland()) {
        using namespace KWayland::Client;
        ConnectionThread *connection = ConnectionThread::fromApplication(this);
        if (connection) {
            Registry *registry = new Registry(this);
            registry->create(connection);
            connect(registry, &Registry::plasmaShellAnnounced, this, [this, registry](quint32 name, quint32 version) {
                m_waylandPlasmashell = registry->createPlasmaShell(name, version, this);
            });
            registry->setup();
            connection->roundtrip();
        }
    }

    // set up shortcuts
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->openAction(), Qt::Key_Print);
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->fullScreenAction(), Qt::SHIFT | Qt::Key_Print);
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->activeWindowAction(), Qt::META | Qt::Key_Print);
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->windowUnderCursorAction(), Qt::META | Qt::CTRL | Qt::Key_Print);
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->regionAction(), Qt::META | Qt::SHIFT | Qt::Key_Print);
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->currentScreenAction(), QList<QKeySequence>());
    KGlobalAccel::self()->setGlobalShortcut(ShortcutActions::self()->openWithoutScreenshotAction(), QList<QKeySequence>());

    // set up CaptureMode model
    m_captureModeModel = std::make_unique<CaptureModeModel>(platform->supportedGrabModes(), this);
    m_recordingModeModel = std::make_unique<RecordingModeModel>(m_videoPlatform->supportedRecordingModes(), this);
    auto captureModeModel = m_captureModeModel.get();
    connect(platform, &Platform::supportedGrabModesChanged, captureModeModel, [this](){
        m_captureModeModel->setGrabModes(m_platform->supportedGrabModes());
    });

    connect(qApp, &QApplication::screenRemoved, this, [this](QScreen *screen) {
        for (auto it = m_captureWindows.begin(); it != m_captureWindows.end(); ++it) {
            if (it->get()->screen() == screen) {
                auto pointer = it->release();
                m_captureWindows.erase(it);
                pointer->hide();
                pointer->deleteLater();
            }
        }
    });

    connect(m_videoPlatform.get(), &VideoPlatform::recordingChanged, this, &SpectacleCore::recordingChanged);
    connect(m_videoPlatform.get(), &VideoPlatform::recordingSaved, this, [this](const QString &path) {
        const QUrl url = QUrl::fromUserInput(path, {}, QUrl::AssumeLocalFile);
        m_viewerWindow->showSavedVideoMessage(url);
        setCurrentVideo(url);
    });
}

SpectacleCore *SpectacleCore::instance()
{
    return s_self;
}

Platform *SpectacleCore::platform() const
{
    return m_platform.get();
}

KWayland::Client::PlasmaShell *SpectacleCore::plasmaShellInterfaceWrapper() const
{
    return m_waylandPlasmashell;
}

CaptureModeModel *SpectacleCore::captureModeModel() const
{
    return m_captureModeModel.get();
}

RecordingModeModel *SpectacleCore::recordingModeModel() const
{
    return m_recordingModeModel.get();
}

AnnotationDocument *SpectacleCore::annotationDocument() const
{
    return m_annotationDocument.get();
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

int SpectacleCore::captureTimeRemaining() const
{
    int totalDuration = m_delayAnimation->totalDuration();
    int currentTime = m_delayAnimation->currentTime();
    return currentTime > totalDuration || m_delayAnimation->state() == QVariantAnimation::Stopped ?
        0 : totalDuration - currentTime;
}

qreal SpectacleCore::captureProgress() const
{
    // using currentValue() sometimes gives 1.0 when we don't want it.
    return m_delayAnimation->state() == QVariantAnimation::Stopped ?
        0 : m_delayAnimation->currentValue().toReal();
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

    m_startMode = StartMode::Gui;
    m_existingLoaded = false;
    m_notify = true;
    qint64 delayMsec = 0;

    // are we ask to run in background or dbus mode?
    if (parser->isSet(QStringLiteral("background"))) {
        m_startMode = StartMode::Background;
    } else if (parser->isSet(QStringLiteral("dbus"))) {
        m_startMode = StartMode::DBus;
    }

    m_editExisting = parser->isSet(QStringLiteral("edit-existing"));
    if (m_editExisting) {
        QString existingFileName = parser->value(QStringLiteral("edit-existing"));
        if (!(existingFileName.isEmpty() || existingFileName.isNull())) {
            setScreenCaptureUrl(existingFileName);
            m_saveToOutput = true;
        }
    }

    auto onClickAvailable = m_platform->supportedShutterModes().testFlag(Platform::ShutterMode::OnClick);
    if ((!onClickAvailable) && (delayMsec < 0)) {
        delayMsec = 0;
    }

    // reset last region if it should not be remembered across restarts
    if (!(Settings::rememberLastRectangularRegion() == Settings::EnumRememberLastRectangularRegion::Always)) {
        Settings::setCropRegion({0, 0, 0, 0});
    }

    CaptureModeModel::CaptureMode captureMode = CaptureModeModel::AllScreens;
    // extract the capture mode
    if (parser->isSet(QStringLiteral("fullscreen"))) {
        captureMode = CaptureModeModel::AllScreens;
    } else if (parser->isSet(QStringLiteral("current"))) {
        captureMode = CaptureModeModel::CurrentScreen;
    } else if (parser->isSet(QStringLiteral("activewindow"))) {
        captureMode = CaptureModeModel::ActiveWindow;
    } else if (parser->isSet(QStringLiteral("region"))) {
        captureMode = CaptureModeModel::RectangularRegion;
    } else if (parser->isSet(QStringLiteral("windowundercursor"))) {
        captureMode = CaptureModeModel::TransientWithParent;
    } else if (parser->isSet(QStringLiteral("transientonly"))) {
        captureMode = CaptureModeModel::WindowUnderCursor;
    } else if (m_startMode == StartMode::Gui
               && (parser->isSet(QStringLiteral("launchonly")) || Settings::launchAction() == Settings::EnumLaunchAction::DoNotTakeScreenshot)
               && !m_editExisting) {
        initViewerWindow(ViewerWindow::Dialog);
        m_viewerWindow->setVisible(true);
        return;
    } else if (Settings::launchAction() == Settings::EnumLaunchAction::UseLastUsedCapturemode && !m_editExisting) {
        captureMode = CaptureModeModel::CaptureMode(Settings::captureMode());
        if (Settings::captureOnClick()) {
            delayMsec = -1;
            takeNewScreenshot(captureMode, delayMsec);
        }
    }

    auto exportManager = ExportManager::instance();
    exportManager->setCaptureMode(captureMode);

    switch (m_startMode) {
    case StartMode::DBus:
        // if both mCopyImageToClipboard and mSaveToOutput are false, image will only be copied to clipboard
        m_copyImageToClipboard = Settings::clipboardGroup() == Settings::EnumClipboardGroup::PostScreenshotCopyImage;
        m_copyLocationToClipboard = Settings::clipboardGroup() == Settings::EnumClipboardGroup::PostScreenshotCopyLocation;
        m_saveToOutput = Settings::autoSaveImage();

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
                delayMsec = lDelayValue;
            }
        }

        if (parser->isSet(QStringLiteral("onclick"))) {
            delayMsec = -1;
        }

        if (isGuiNull()) {
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

        takeNewScreenshot(captureMode, delayMsec, lIncludePointer, lIncludeDecorations);
    } break;

    case StartMode::Gui:
        if (isGuiNull()) {
            takeNewScreenshot(captureMode, delayMsec);
        } else {
            using Actions = Settings::EnumPrintKeyActionRunning;
            switch (Settings::printKeyActionRunning()) {
            case Actions::TakeNewScreenshot: {
                // 0 means Immediate, -1 onClick
                int timeout = m_platform->supportedShutterModes().testFlag(Platform::ShutterMode::Immediate) ? 0 : -1;
                takeNewScreenshot(Settings::captureMode(), timeout);
                break;
            }
            case Actions::FocusWindow: {
                bool isCaptureWindow = !m_captureWindows.empty();
                SpectacleWindow *window = nullptr;
                if (isCaptureWindow) {
                    window = m_captureWindows.front().get();
                } else {
                    window = m_viewerWindow.get();
                }
                if (isCaptureWindow) {
                    SpectacleWindow::setVisibilityForAll(QWindow::FullScreen);
                } else {
                    // Unminimize the window.
                    window->unminimize();
                }
                window->requestActivate();
                break;
            }
            case Actions::StartNewInstance: {
                QProcess newInstance;
                newInstance.setProgram(QCoreApplication::applicationFilePath());
                newInstance.setArguments({QStringLiteral("--new-instance")});
                newInstance.startDetached();
                break;
            }
            }
        }

        break;
    }
}

void SpectacleCore::takeNewScreenshot(int captureMode, int timeout, bool includePointer, bool includeDecorations, bool transientOnly)
{
    m_delayAnimation->stop();

    // TODO: Improve API for transientOnly or make it obsolete.
    if (!transientOnly
        && m_platform->supportedGrabModes() & Platform::TransientWithParent
        && captureMode == CaptureModeModel::WindowUnderCursor) {
        captureMode = CaptureModeModel::TransientWithParent;
    }

    ExportManager::instance()->setCaptureMode(CaptureModeModel::CaptureMode(captureMode));
    m_tempGrabMode = toPlatformGrabMode(CaptureModeModel::CaptureMode(captureMode));
    m_tempIncludePointer = includePointer;
    m_tempIncludeDecorations = includeDecorations;

    if (timeout < 0 || !m_platform->supportedShutterModes().testFlag(Platform::ShutterMode::Immediate)) {
        SpectacleWindow::setVisibilityForAll(QWindow::Hidden);
        m_platform->doGrab(Platform::ShutterMode::OnClick, m_tempGrabMode, m_tempIncludePointer, m_tempIncludeDecorations);
        return;
    }

    const bool noDelay = timeout == 0;

    // when compositing is enabled, we need to give it enough time for the window
    // to disappear and all the effects are complete before we take the shot. there's
    // no way of knowing how long the disappearing effects take, but as per default
    // settings (and unless the user has set an extremely slow effect), 200
    // milliseconds is a good amount of wait time.
    timeout = qMax(timeout, KWindowSystem::compositingActive() ? 200 : 50);

    if (noDelay) {
        SpectacleWindow::setVisibilityForAll(QWindow::Hidden);
        QTimer::singleShot(timeout, this, [this]() {
            m_platform->doGrab(Platform::ShutterMode::Immediate, m_tempGrabMode, m_tempIncludePointer, m_tempIncludeDecorations);
        });
        return;
    }

    m_delayAnimation->setDuration(timeout);
    m_delayAnimation->start();

    SpectacleWindow::setVisibilityForAll(QWindow::Minimized);
}

void SpectacleCore::cancelScreenshot()
{
    if (m_startMode != StartMode::Gui) {
        Q_EMIT allDone();
        return;
    }

    int currentTime = m_delayAnimation->currentTime();
    m_delayAnimation->stop();
    if (currentTime > 0) {
        SpectacleWindow::setTitleForAll(SpectacleWindow::Previous);
    }
}

void SpectacleCore::showErrorMessage(const QString &theErrString)
{
    qCDebug(SPECTACLE_CORE_LOG) << "ERROR: " << theErrString;

    if (m_startMode == StartMode::Gui) {
        KMessageBox::error(nullptr, theErrString);
    }
}

void SpectacleCore::onScreenshotUpdated(const QPixmap &thePixmap)
{
    QPixmap existingPixmap;
    const QPixmap &pixmapUsed = (m_editExisting && !m_existingLoaded) ? existingPixmap : thePixmap;
    if (m_editExisting && !m_existingLoaded) {
        existingPixmap.load(m_screenCaptureUrl.toLocalFile());
    }

    auto exportManager = ExportManager::instance();
    exportManager->setPixmap(pixmapUsed);
    m_annotationDocument->addImage(pixmapUsed.toImage(), QPointF(0, 0));
    exportManager->updatePixmapTimestamp();

    switch (m_startMode) {
    case StartMode::Background:
    case StartMode::DBus: {
        syncExportPixmap();
        if (m_saveToOutput || !m_copyImageToClipboard
            || (Settings::autoSaveImage() && !m_saveToOutput)) {
            m_saveToOutput = Settings::autoSaveImage();
            QUrl lSavePath = (m_startMode == StartMode::Background && m_screenCaptureUrl.isValid() && m_screenCaptureUrl.isLocalFile()) ? m_screenCaptureUrl : QUrl();
            exportManager->doSave(lSavePath, m_notify);
        }

        if (m_copyImageToClipboard) {
            exportManager->doCopyToClipboard(m_notify);
        } else if (m_copyLocationToClipboard) {
            exportManager->doCopyLocationToClipboard(m_notify);
        }

        // if we don't have a Gui already opened, Q_EMIT allDone
        if (isGuiNull()) {
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
            initViewerWindow(ViewerWindow::Dialog);
            m_viewerWindow->setVisible(true);
            return;
        }
        if (!m_editExisting) {
            setScreenCaptureUrl(QUrl(QStringLiteral("image://spectacle/%1").arg(pixmapUsed.cacheKey())));
        }
        initViewerWindow(ViewerWindow::Image);
        m_viewerWindow->setVisible(true);
        auto titlePreset = !pixmapUsed.isNull() ? SpectacleWindow::Unsaved : SpectacleWindow::Saved;
        SpectacleWindow::setTitleForAll(titlePreset);

        m_saveToOutput = Settings::autoSaveImage();
        m_copyImageToClipboard = Settings::clipboardGroup() == Settings::EnumClipboardGroup::PostScreenshotCopyImage;
        m_copyLocationToClipboard = Settings::clipboardGroup() == Settings::EnumClipboardGroup::PostScreenshotCopyLocation;

        if (m_saveToOutput && m_copyImageToClipboard) {
            syncExportPixmap();
            exportManager->doSaveAndCopy();
        } else if (m_saveToOutput) {
            exportManager->doSave();
        } else if (m_copyImageToClipboard) {
            syncExportPixmap();
            exportManager->doCopyToClipboard(false);
        } else if (m_copyLocationToClipboard) {
            exportManager->doCopyLocationToClipboard(false);
        }
    }

    if (m_editExisting && !m_existingLoaded) {
        Settings::setLastSaveLocation(m_screenCaptureUrl);
        m_existingLoaded = true;
    }
}

void SpectacleCore::onScreenshotFailed()
{
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
        if (!m_viewerWindow) {
            initViewerWindow(ViewerWindow::Dialog);
        }
        m_viewerWindow->showScreenshotFailedMessage();
        return;
    }
}

void SpectacleCore::doNotify(const QUrl &theSavedAt)
{
    KNotification *lNotify = new KNotification(QStringLiteral("newScreenshotSaved"));

    int index = captureModeModel()->indexOfCaptureMode(ExportManager::instance()->captureMode());
    auto captureModeLabel = captureModeModel()->data(captureModeModel()->index(index),
                                                     Qt::DisplayRole);
    lNotify->setTitle(captureModeLabel.toString());

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
                if (isGuiNull() || Settings::quitAfterSaveCopyExport()) {
                    Q_EMIT allDone();
                }
            });
        });
        lNotify->setActions({i18n("Annotate")});
        connect(lNotify, &KNotification::action1Activated, this, [theSavedAt]() {
            QProcess newInstance;
            newInstance.setProgram(QCoreApplication::applicationFilePath());
            newInstance.setArguments({QStringLiteral("--new-instance"), QStringLiteral("--edit-existing"), theSavedAt.toLocalFile()});
            newInstance.startDetached();
        });
    }

    connect(lNotify, &QObject::destroyed, this, [this] {
        QTimer::singleShot(250, this, [this] {
            if (isGuiNull() || Settings::quitAfterSaveCopyExport()) {
                Q_EMIT allDone();
            }
        });
    });

    lNotify->sendEvent();
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

bool SpectacleCore::isGuiNull() const
{
    return m_captureWindows.empty() && m_viewerWindow == nullptr;
}

void SpectacleCore::initGuiNoScreenshot()
{
    // in some cases like the openWithoutScreenshot DBus method, the start mode is DBus, but we need to show a GUI
    // so we should switch the mode appropriately
    m_startMode = SpectacleCore::StartMode::Gui;
    initViewerWindow(ViewerWindow::Dialog);
    m_viewerWindow->setVisible(true);
}

void SpectacleCore::syncExportPixmap()
{
    qreal maxDpr = 0.0;
    for (auto &img : m_annotationDocument->baseImages()) {
        maxDpr = qMax(maxDpr, img.devicePixelRatio());
    }
    QRectF imageRect(QPointF(0, 0), m_annotationDocument->canvasSize());
    const auto &image = m_annotationDocument->renderToImage(imageRect, maxDpr);
    ExportManager::instance()->setPixmap(QPixmap::fromImage(image));
}

QQmlEngine *SpectacleCore::getQmlEngine()
{
    if (m_engine == nullptr) {
        m_engine = std::make_unique<QQmlEngine>(this);
        m_engine->addImageProvider(QStringLiteral("spectacle"),
                                   new SpectacleImageProvider(QQmlImageProviderBase::Pixmap));
        m_engine->rootContext()->setContextObject(new KLocalizedContext(m_engine.get()));

        qmlRegisterSingletonInstance(QML_URI_PRIVATE, 1, 0, "SpectacleCore", this);
        qmlRegisterSingletonInstance(QML_URI_PRIVATE, 1, 0, "Platform", m_platform.get());
        qmlRegisterSingletonInstance(QML_URI_PRIVATE, 1, 0, "Settings", Settings::self());
        qmlRegisterSingletonInstance(QML_URI_PRIVATE, 1, 0, "CaptureModeModel", m_captureModeModel.get());
        qmlRegisterSingletonInstance(QML_URI_PRIVATE, 1, 0, "SelectionEditor", SelectionEditor::instance());
        qmlRegisterSingletonInstance(QML_URI_PRIVATE, 1, 0, "Selection", SelectionEditor::instance()->selection());

        qmlRegisterSingletonInstance(QML_URI_PRIVATE, 1, 0, "AnnotationDocument", m_annotationDocument.get());
        qmlRegisterUncreatableType<AnnotationTool>(QML_URI_PRIVATE, 1, 0, "AnnotationTool",
                                                   QStringLiteral("Use AnnotationDocument.tool"));
        qmlRegisterUncreatableType<SelectedActionWrapper>(QML_URI_PRIVATE, 1, 0, "SelectedAction",
                                                          QStringLiteral("Use AnnotationDocument.selectedAction"));
        qmlRegisterType<AnnotationViewport>(QML_URI_PRIVATE, 1, 0, "AnnotationViewport");
    }
    return m_engine.get();
}

void SpectacleCore::initCaptureWindows(CaptureWindow::Mode mode)
{
    deleteWindows();

    // Allow the window to be transparent. Used for video recording UI.
    // It has to be set before creating the window.
    QQuickWindow::setDefaultAlphaBuffer(true);

    auto engine = getQmlEngine();
    for (auto *screen : qApp->screens()) {
        m_captureWindows.emplace_back(std::make_unique<CaptureWindow>(mode, screen, engine));
    }
}

void SpectacleCore::initViewerWindow(ViewerWindow::Mode mode)
{
    deleteWindows();

    // Transparency isn't needed for this window.
    QQuickWindow::setDefaultAlphaBuffer(false);

    m_viewerWindow = std::make_unique<ViewerWindow>(mode, getQmlEngine());
}

void SpectacleCore::deleteWindows()
{
    if (auto pointer = m_viewerWindow.release()) {
        pointer->hide();
        pointer->deleteLater();
    } else {
        for (auto it = m_captureWindows.begin(); it != m_captureWindows.end(); ++it) {
            auto pointer = it->release();
            m_captureWindows.erase(it);
            pointer->hide();
            pointer->deleteLater();
        }
    }
}

void SpectacleCore::unityLauncherUpdate(const QVariantMap &properties) const
{
    QDBusMessage message = QDBusMessage::createSignal(QStringLiteral("/org/kde/Spectacle"),
                                                      QStringLiteral("com.canonical.Unity.LauncherEntry"),
                                                      QStringLiteral("Update"));
    message.setArguments({QApplication::desktopFileName(), properties});
    QDBusConnection::sessionBus().send(message);
}

void SpectacleCore::startRecordingScreen(QScreen *screen, bool withPointer)
{
    Q_ASSERT(!m_videoPlatform->isRecording());
    const QString output = ExportManager::instance()->suggestedVideoFilename(m_videoPlatform->extension());
    m_videoPlatform->startRecording(output, VideoPlatform::Screen, screen, withPointer);
    setVideoMode(true);
}

void SpectacleCore::startRecordingRegion(const QRect &region, bool withPointer)
{
    Q_ASSERT(!m_videoPlatform->isRecording());
    const QString output = ExportManager::instance()->suggestedVideoFilename(m_videoPlatform->extension());
    m_videoPlatform->startRecording(output, VideoPlatform::Region, region, withPointer);
    setVideoMode(true);
}

void SpectacleCore::startRecordingWindow(const QString &uuid, bool withPointer)
{
    Q_ASSERT(!m_videoPlatform->isRecording());
    const QString output = ExportManager::instance()->suggestedVideoFilename(m_videoPlatform->extension());
    m_videoPlatform->startRecording(output, VideoPlatform::Window, uuid, withPointer);
    setVideoMode(true);
}

void SpectacleCore::finishRecording()
{
    Q_ASSERT(m_videoPlatform->isRecording());
    m_videoPlatform->finishRecording();
}

bool SpectacleCore::isRecording() const
{
    return m_videoPlatform->isRecording();
}

bool SpectacleCore::recordingSupported() const
{
    return m_videoPlatform->supportedRecordingModes() != 0;
}

bool SpectacleCore::videoMode() const
{
    return m_videoMode;
}

void SpectacleCore::setVideoMode(bool videoMode)
{
    if (videoMode == m_videoMode) {
        return;
    }
    m_videoMode = videoMode;
    Q_EMIT videoModeChanged(videoMode);
}

QUrl SpectacleCore::currentVideo() const
{
    return m_currentVideo;
}

void SpectacleCore::setCurrentVideo(const QUrl &currentVideo)
{
    if (currentVideo == m_currentVideo) {
        return;
    }
    m_currentVideo = currentVideo;
    Q_EMIT currentVideoChanged(currentVideo);
}
