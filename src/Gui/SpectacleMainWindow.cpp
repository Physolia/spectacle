/* This file is part of Spectacle, the KDE screenshot utility
 * SPDX-FileCopyrightText: 2015 Boudhayan Gupta <bgupta@kde.org>
 * SPDX-FileCopyrightText: 2019 David Redondo <kde@david-redondo.de>
 * SPDX-FileCopyrightText: 2022 Noah Davis <noahadvs@gmail.com>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "SpectacleMainWindow.h"

#include "CaptureModeModel.h"
#include "SettingsDialog/GeneralOptionsPage.h"
#include "SettingsDialog/SaveOptionsPage.h"
#include "SettingsDialog/SettingsDialog.h"
#include "SettingsDialog/ShortcutsOptionsPage.h"
#include "settings.h"

#include <KAboutData>
#include <KConfigGroup>
// #include <KGuiItem>
#include <KHelpMenu>
// #include <KIO/JobUiDelegate>
#include <KIO/OpenFileManagerWindowJob>
#include <KIO/OpenUrlJob>
#include <KLocalizedContext>
#include <KLocalizedString>
#include <KStandardAction>
#include <KWindowSystem>

#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDesktopServices>
#include <QApplication>
#include <QPalette>
#include <QPrintDialog>
#include <QPrinter>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <kcoreconfigskeleton.h>
#include <kio/jobuidelegate.h>
#include <memory>
#include <QTimer>
#include <qqml.h>
#include <qstringliteral.h>
#include <QVariantAnimation>
#include <QtMath>

#ifdef XCB_FOUND
#include <QX11Info>
#include <xcb/xcb.h>
#endif

SpectacleMainWindow::SpectacleMainWindow(QQmlEngine *engine)
    // Unlike the other QQuickView contructors,
    // this one requires a parent window argument to be explicitly set.
    // However, a parent window is not actually needed for it to work correctly, so it can be nullptr.
    : QQuickView(engine, nullptr)
    , m_placeholderParent(new QWidget)
    , m_toolsMenu(new SpectacleMenu)
    , m_exportMenu(new ExportMenu)
    , m_clipboardMenu(new SpectacleMenu)
    , m_saveMenu(new SpectacleMenu)
{
    // before we do anything, we need to set a window property
    // that skips the close/hide window animation on kwin. this
    // fixes a ghost image of the spectacle window that appears
    // on subsequent screenshots taken with the take new screenshot
    // button
    //
    // credits for this goes to Thomas Lübking <thomas.luebking@gmail.com>

#ifdef XCB_FOUND
    if (KWindowSystem::isPlatformX11()) {

        // do the xcb shenanigans
        xcb_connection_t *xcbConn = QX11Info::connection();
        const QByteArray effectName = QByteArrayLiteral("_KDE_NET_WM_SKIP_CLOSE_ANIMATION");

        xcb_intern_atom_cookie_t atomCookie = xcb_intern_atom_unchecked(xcbConn, false, effectName.length(), effectName.constData());
        QScopedPointer<xcb_intern_atom_reply_t, QScopedPointerPodDeleter> atom(xcb_intern_atom_reply(xcbConn, atomCookie, nullptr));
        if (!atom.isNull()) {
            uint32_t value = 1;
            xcb_change_property(xcbConn, XCB_PROP_MODE_REPLACE, winId(), atom->atom, XCB_ATOM_CARDINAL, 32, 1, &value);
        }
    }
#endif

    QMetaObject::invokeMethod(this, &SpectacleMainWindow::init, Qt::QueuedConnection);
}

void SpectacleMainWindow::init()
{
    // set m_placeholderParent geometry
    m_placeholderParent->setGeometry(this->geometry());
    connect(this, &QWindow::xChanged, m_placeholderParent.get(), [this](){
        m_placeholderParent->setGeometry(this->geometry());
    });
    connect(this, &QWindow::yChanged, m_placeholderParent.get(), [this](){
        m_placeholderParent->setGeometry(this->geometry());
    });
    connect(this, &QWindow::widthChanged, m_placeholderParent.get(), [this](){
        m_placeholderParent->setGeometry(this->geometry());
    });
    connect(this, &QWindow::heightChanged, m_placeholderParent.get(), [this](){
        m_placeholderParent->setGeometry(this->geometry());
    });

    // change window title on save and on autosave
    connect(ExportManager::instance(), &ExportManager::imageSaved, this, &SpectacleMainWindow::onImageSaved);
    connect(ExportManager::instance(), &ExportManager::imageCopied, this, &SpectacleMainWindow::onImageCopied);
    connect(ExportManager::instance(), &ExportManager::imageLocationCopied, this, &SpectacleMainWindow::onImageSavedAndLocationCopied);
    connect(ExportManager::instance(), &ExportManager::imageSavedAndCopied, this, &SpectacleMainWindow::onImageSavedAndCopied);

    //BEGIN Menu setup
    // the help menu
    KHelpMenu *helpMenu = new KHelpMenu(nullptr, KAboutData::applicationData(), true);
    m_helpMenu = helpMenu->menu();

    // the tools menu
    m_toolsMenu->addAction(QIcon::fromTheme(QStringLiteral("document-open-folder")),
                          i18n("Open Default Screenshots Folder"),
                          this,
                          &SpectacleMainWindow::openScreenshotsFolder);
    m_toolsMenu->addAction(KStandardAction::print(this, &SpectacleMainWindow::showPrintDialog, this));
    // TODO: Remove this when recording functionality is added
    m_screenRecorderToolsMenu = m_toolsMenu->addMenu(i18n("Record Screen"));
    m_screenRecorderToolsMenu->setIcon(QIcon::fromTheme(QStringLiteral("media-record")));
    connect(m_screenRecorderToolsMenu, &QMenu::aboutToShow, this, [this]() {
        KMoreToolsMenuFactory *moreToolsMenuFactory = new KMoreToolsMenuFactory(QStringLiteral("spectacle/screenrecorder-tools"));
        moreToolsMenuFactory->setParentWidget(m_toolsMenu.get());
        m_screenRecorderToolsMenuFactory.reset(moreToolsMenuFactory);
        m_screenRecorderToolsMenu->clear();
        m_screenRecorderToolsMenuFactory->fillMenuFromGroupingNames(m_screenRecorderToolsMenu, {QStringLiteral("screenrecorder")});
    });

    // the save menu
    QAction *m_saveAsAction = KStandardAction::saveAs(this, &SpectacleMainWindow::saveAs, this);
    QAction *m_saveAction = KStandardAction::save(this, &SpectacleMainWindow::save, this);
    m_saveMenu->addAction(m_saveAsAction);
    m_saveMenu->addAction(m_saveAction);
    setDefaultSaveAction();

    // the clipboard menu
    QAction *m_clipboardImageAction = KStandardAction::copy(this, &SpectacleMainWindow::copyImage, this);
    m_clipboardImageAction->setText(i18n("Copy Image to Clipboard"));
    QAction *m_clipboardLocationAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Location to Clipboard"), this);
    connect(m_clipboardLocationAction, &QAction::triggered, this, &SpectacleMainWindow::copyLocation);
    m_clipboardMenu->addAction(m_clipboardImageAction);
    m_clipboardMenu->addAction(m_clipboardLocationAction);
    setDefaultCopyAction();
    //END Menu setup

    //BEGIN QML setup
    // Expose this SpectacleMainWindow to QML in a convenient way
    QQmlContext *rootContext = this->engine()->rootContext();
    rootContext->setContextProperty(QStringLiteral("mainWindow"), this);

    // Set up i18n for QML
    rootContext->setContextObject(new KLocalizedContext(this->engine()));

    // Set up the window color handling so we don't need an extra Rectangle
    this->setColor(qApp->palette().color(QPalette::Window));
    connect(qApp, &QApplication::paletteChanged, this, [this](){
        setColor(qApp->palette().color(QPalette::Window));
    });

    // Resize the QML item we will generate to fit the window
    this->setResizeMode(QQuickView::SizeRootObjectToView);

    // Set up Main.qml
    QUrl mainQmlUrl(QStringLiteral("qrc:/src/Gui/Main.qml"));
    QVariantMap initialProperties = {
        {QStringLiteral("parent"), QVariant::fromValue(this->contentItem())}
    };
    // The item can expect to have a parent and a window before it is completed.
    // If you don't set the parent in initialProperties first,
    // the parent and window for the item will be set after the item is completed.
    this->setInitialProperties(initialProperties);
    this->setSource(mainQmlUrl);
    //END QML setup

    // QQuickView::rootObject() is a pointer to the QQuickItem created from Main.qml
    QQuickItem *mainItem = this->rootObject();

    // Set up window size handling based on the main item
    updateMinimumWidth();
    updateMinimumHeight();
    connect(mainItem, SIGNAL(minimumWidthChanged()), this, SLOT(updateMinimumWidth()));
    connect(mainItem, SIGNAL(minimumHeightChanged()), this, SLOT(updateMinimumHeight()));
    resize(qRound(mainItem->implicitWidth()), qRound(mainItem->implicitHeight()));

    // the KSGWidget
/*
    //TODO FIXME
    connect(mKSWidget, &KSWidget::newScreenshotRequest, this, &SpectacleMainWindow::captureScreenshot);
    connect(mKSWidget, &KSWidget::dragInitiated, this, &SpectacleMainWindow::dragAndDropRequest);

    // the Button Bar

    mDialogButtonBox->setStandardButtons(QDialogButtonBox::Help);
    mDialogButtonBox->button(QDialogButtonBox::Help)->setAutoDefault(false);

    mConfigureButton->setDefaultAction(KStandardAction::preferences(this, SLOT(showPreferencesDialog()), this));
    mConfigureButton->setText(i18n("Configure..."));
    mConfigureButton->setToolTip(i18n("Change Spectacle's settings."));
    mConfigureButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mDialogButtonBox->addButton(mConfigureButton, QDialogButtonBox::ResetRole);

#ifdef KIMAGEANNOTATOR_FOUND
    mAnnotateButton->setText(i18n("Annotate"));
    mAnnotateButton->setToolTip(i18n("Add annotation to the screenshot"));
    mAnnotateButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mAnnotateButton->setIcon(QIcon::fromTheme(QStringLiteral("document-edit")));
    connect(mAnnotateButton, &QToolButton::clicked, this, [this] {
        if (mAnnotatorActive) {
            mKSWidget->hideAnnotator();
            mAnnotateButton->setText(i18n("Annotate"));
        } else {
            mKSWidget->showAnnotator();
            mAnnotateButton->setText(i18n("Annotation Done"));
        }

        mToolsButton->setEnabled(mAnnotatorActive);
        mSendToButton->setEnabled(mAnnotatorActive);
        mClipboardButton->setEnabled(mAnnotatorActive);
        mSaveButton->setEnabled(mAnnotatorActive);

        mAnnotatorActive = !mAnnotatorActive;
    });
    mDialogButtonBox->addButton(mAnnotateButton, QDialogButtonBox::ActionRole);
#endif
*/

/*
    // message widget
    connect(mMessageWidget, &KMessageWidget::linkActivated, this, [](const QString &str) {
        QDesktopServices::openUrl(QUrl(str));
    });

    // layouts

    mDivider->setFrameShape(QFrame::HLine);
    mDivider->setLineWidth(2);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(mKSWidget);
    layout->addWidget(mMessageWidget);
    layout->addWidget(mDivider);
    layout->addWidget(mDialogButtonBox);
    mMessageWidget->hide();

    // populate our send-to actions
    mSendToButton->setMenu(m_exportMenu);
    connect(m_exportMenu, &ExportMenu::imageShared, this, &SpectacleMainWindow::showImageSharedFeedback);

    // Allow Ctrl+Q to quit the app
    QAction *actionQuit = KStandardAction::quit(qApp, &QApplication::quit, this);
    actionQuit->setShortcut(QKeySequence::Quit);
    addAction(actionQuit);

    // message: open containing folder
    mOpenContaining = new QAction(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Open Containing Folder"), mMessageWidget);
    connect(mOpenContaining, &QAction::triggered, [=] {
        QUrl imageUrl;
        if (ExportManager::instance()->isImageSavedNotInTemp()) {
            imageUrl = Settings::lastSaveLocation();
        } else {
            imageUrl = ExportManager::instance()->tempSave();
        }

        KIO::highlightInFileManager({imageUrl});
    });

    mHideMessageWidgetTimer = new QTimer(this);
    //     connect(mHideMessageWidgetTimer, &QTimer::timeout,
    //             mMessageWidget, &KMessageWidget::animatedHide);
    mHideMessageWidgetTimer->setInterval(10000);
    // done with the init
*/
}

QMenu *SpectacleMainWindow::helpMenu() const
{
    return m_helpMenu;
}

SpectacleMenu *SpectacleMainWindow::toolsMenu() const
{
    return m_toolsMenu.get();
}

SpectacleMenu *SpectacleMainWindow::exportMenu() const
{
    return m_exportMenu.get();
}

SpectacleMenu *SpectacleMainWindow::clipboardMenu() const
{
    return m_clipboardMenu.get();
}

SpectacleMenu *SpectacleMainWindow::saveMenu() const
{
    return m_saveMenu.get();
}

void SpectacleMainWindow::updateMinimumWidth()
{
    QQuickItem *mainItem = this->rootObject();
    if (!mainItem) {
        return;
    }
    qreal itemMinimumWidth = mainItem->property("minimumWidth").toReal();
    this->setMinimumWidth(qRound(itemMinimumWidth));
}

void SpectacleMainWindow::updateMinimumHeight()
{
    QQuickItem *mainItem = this->rootObject();
    if (!mainItem) {
        return;
    }
    qreal itemMinimumHeight = mainItem->property("minimumHeight").toReal();
    this->setMinimumHeight(qRound(itemMinimumHeight));
}

void SpectacleMainWindow::setDefaultSaveAction()
{
    switch (Settings::lastUsedSaveMode()) {
    case Settings::SaveAs:
        //TODO FIXME
//         mSaveButton->setDefaultAction(m_saveAsAction);
//         mSaveButton->setText(i18n("Save As..."));
        break;
    case Settings::Save:
//         mSaveButton->setDefaultAction(m_saveAction);
        break;
    }
//     mSaveButton->setEnabled(m_pixmapExists);
}

void SpectacleMainWindow::setDefaultCopyAction()
{
    switch (Settings::lastUsedCopyMode()) {
    case Settings::CopyImage:
        //TODO FIXME
//         mClipboardButton->setText(i18n("Copy Image to Clipboard"));
//         mClipboardButton->setDefaultAction(m_clipboardImageAction);
        break;
    case Settings::CopyLocation:
//         mClipboardButton->setText(i18n("Copy Location to Clipboard"));
//         mClipboardButton->setDefaultAction(m_clipboardLocationAction);
        break;
    }
//     mClipboardButton->setEnabled(m_pixmapExists);
}

void SpectacleMainWindow::captureScreenshot(CaptureModeModel::CaptureMode theCaptureMode, int theTimeout, bool theIncludePointer, bool theIncludeDecorations)
{
    if (theTimeout < 0) { // OnClick is checked (always the case on Wayland)
        hide();
        Q_EMIT newScreenshotRequest(theCaptureMode, theTimeout, theIncludePointer, theIncludeDecorations);
        return;
    }

    showMinimized();
    // TODO FIXME
//     mMessageWidget->hide();
    QTimer *timer = new QTimer;
    timer->setSingleShot(true);
    timer->setInterval(theTimeout);
    auto unityUpdate = [](const QVariantMap &properties) {
        QDBusMessage message =
            QDBusMessage::createSignal(QStringLiteral("/org/kde/Spectacle"), QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"));
        message.setArguments({QApplication::desktopFileName(), properties});
        QDBusConnection::sessionBus().send(message);
    };
    auto delayAnimation = new QVariantAnimation(timer);
    delayAnimation->setStartValue(0.0);
    delayAnimation->setEndValue(1.0);
    delayAnimation->setDuration(timer->interval());
    connect(delayAnimation, &QVariantAnimation::valueChanged, this, [=] {
        const double progress = delayAnimation->currentValue().toDouble();
        const double timeoutInSeconds = theTimeout / 1000.0;
//         mKSWidget->setProgress(progress);
        unityUpdate({{QStringLiteral("progress"), progress}});
        setTitle(i18ncp("@title:window", "%1 second", "%1 seconds", qMin(int(timeoutInSeconds), qCeil((1 - progress) * timeoutInSeconds))));
    });
    connect(timer, &QTimer::timeout, this, [=] {
        this->hide();
        timer->deleteLater();
//         mKSWidget->setProgress(0);
        unityUpdate({{QStringLiteral("progress-visible"), false}});
        Q_EMIT newScreenshotRequest(theCaptureMode, 0, theIncludePointer, theIncludeDecorations);
    });

//     connect(mKSWidget, &KSWidget::screenshotCanceled, timer, [=] {
//         timer->stop();
//         timer->deleteLater();
//         restoreWindowTitle();
//         unityUpdate({{QStringLiteral("progress-visible"), false}});
//     });

    unityUpdate({{QStringLiteral("progress-visible"), true}, {QStringLiteral("progress"), 0}});
    timer->start();
    delayAnimation->start();
}

void SpectacleMainWindow::setScreenshotAndShow(const QPixmap &pixmap, bool showAnnotator)
{
    m_pixmapExists = !pixmap.isNull();
    if (m_pixmapExists) {
        //TODO FIXME
//         mKSWidget->setScreenshotPixmap(pixmap);
//         m_exportMenu->imageUpdated();
        setUnsaved(true, i18nc("@title:window Unsaved Screenshot", "Unsaved*"));
    } else {
        restoreWindowTitle();
    }

#ifdef KIMAGEANNOTATOR_FOUND
//     mAnnotateButton->setEnabled(m_pixmapExists);
#endif
//     mSendToButton->setEnabled(m_pixmapExists);
//     mClipboardButton->setEnabled(m_pixmapExists);
//     mSaveButton->setEnabled(m_pixmapExists);

//     mKSWidget->setButtonState(KSWidget::State::TakeNewScreenshot);
    show();
    requestActivate();
    /* NOTE windowWidth only produces the right result if it is called after the window is visible.
     * Because of this the call is not moved into the if above */
    if (m_pixmapExists) {
//         resize(QSize(windowWidth(pixmap), DEFAULT_WINDOW_HEIGHT));
    }
    if (showAnnotator) {
#ifdef KIMAGEANNOTATOR_FOUND
//         mAnnotateButton->click();
#endif
    }
}

void SpectacleMainWindow::showPrintDialog()
{
    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog printDialog(&printer);
    if (printDialog.exec() == QDialog::Accepted) {
        ExportManager::instance()->doPrint(&printer);
        return;
    }
}

void SpectacleMainWindow::openScreenshotsFolder()
{
    auto job = new KIO::OpenUrlJob(Settings::defaultSaveLocation());
    //TODO FIXME
//     job->setUiDelegate(new KIO::JobUiDelegate(KIO::JobUiDelegate::AutoHandlingEnabled, this));
    job->start();
}

void SpectacleMainWindow::quitExternally()
{
    qApp->setQuitOnLastWindowClosed(false);
    hide();
}

bool SpectacleMainWindow::isUnsaved() const
{
    return m_unsaved;
}

void SpectacleMainWindow::setUnsaved(bool unsaved, const QString &title)
{
    if (m_unsaved == unsaved) {
        return;
    }
    m_unsaved = unsaved;
    setTitle(title);
}

/* TODO FIXME
void SpectacleMainWindow::showInlineMessage(const QString &message,
                                     const KMessageWidget::MessageType messageType,
                                     const MessageDuration messageDuration,
                                     const QList<QAction *> &actions)
{
    const auto messageWidgetActions = mMessageWidget->actions();
    for (QAction *action : messageWidgetActions) {
        mMessageWidget->removeAction(action);
    }
    for (QAction *action : actions) {
        mMessageWidget->addAction(action);
    }
    mMessageWidget->setText(message);
    mMessageWidget->setMessageType(messageType);

    switch (messageType) {
    case KMessageWidget::Error:
        mMessageWidget->setIcon(QIcon::fromTheme(QStringLiteral("dialog-error")));
        break;
    case KMessageWidget::Warning:
        mMessageWidget->setIcon(QIcon::fromTheme(QStringLiteral("dialog-warning")));
        break;
    case KMessageWidget::Positive:
        mMessageWidget->setIcon(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")));
        break;
    case KMessageWidget::Information:
        mMessageWidget->setIcon(QIcon::fromTheme(QStringLiteral("dialog-information")));
        break;
    }

    mHideMessageWidgetTimer->stop();
    mMessageWidget->animatedShow();
    if (messageDuration == MessageDuration::AutoHide) {
        mHideMessageWidgetTimer->start();
    }
}
*/

void SpectacleMainWindow::showImageSharedFeedback(bool error, const QString &message)
{
    if (error == 1) {
        // error == 1 means the user cancelled the sharing
        return;
    }

    if (error) {
        //TODO FIXME
//         showInlineMessage(i18n("There was a problem sharing the image: %1", message), KMessageWidget::Error);
    } else {
        if (message.isEmpty()) {
//             showInlineMessage(i18n("Image shared"), KMessageWidget::Positive);
        } else {
//             showInlineMessage(i18n("The shared image link (<a href=\"%1\">%1</a>) has been copied to the clipboard.", message),
//                               KMessageWidget::Positive,
//                               MessageDuration::Persistent);
            QApplication::clipboard()->setText(message);
        }
    }
}

void SpectacleMainWindow::copyLocation()
{
    Settings::setLastUsedCopyMode(Settings::CopyLocation);
    setDefaultCopyAction();

    const bool quitChecked = Settings::quitAfterSaveCopyExport();
    ExportManager::instance()->doCopyLocationToClipboard();
    if (quitChecked) {
        quitExternally();
    }
}

void SpectacleMainWindow::copyImage()
{
    Settings::setLastUsedCopyMode(Settings::CopyImage);
    setDefaultCopyAction();

    const bool quitChecked = Settings::quitAfterSaveCopyExport();
    ExportManager::instance()->doCopyToClipboard();
    if (quitChecked) {
        quitExternally();
    }
}

void SpectacleMainWindow::onImageCopied()
{
    //TODO FIXME
//     showInlineMessage(i18n("The screenshot has been copied to the clipboard."), KMessageWidget::Information);
}

void SpectacleMainWindow::onImageSavedAndLocationCopied(const QUrl &location)
{
    //TODO FIXME
//     showInlineMessage(
//         i18n("The screenshot has been saved as <a href=\"%1\">%2</a> and its location has been copied to clipboard", location.toString(), location.fileName()),
//         KMessageWidget::Positive,
//         MessageDuration::AutoHide,
//         {mOpenContaining});
}

void SpectacleMainWindow::onScreenshotFailed()
{
    //TODO FIXME
//     showInlineMessage(i18n("Could not take a screenshot. Please report this bug here: <a href=\"https://bugs.kde.org/enter_bug.cgi?product=Spectacle\">create "
//                            "a spectacle bug</a>"),
//                       KMessageWidget::Warning);
#ifdef KIMAGEANNOTATOR_FOUND
//     mAnnotateButton->setEnabled(false);
#endif
}

void SpectacleMainWindow::setPlaceholderTextOnLaunch()
{
    QString placeholderText(i18n("Ready to take a screenshot"));
    //TODO FIXME
//     mKSWidget->showPlaceholderText(placeholderText);
    setTitle(placeholderText);
}

void SpectacleMainWindow::showPreferencesDialog()
{
    //TODO FIXME
    if (KConfigDialog::showDialog(QStringLiteral("settings"))) {
        return;
    }
    (new SettingsDialog(m_placeholderParent.get()))->show();
}

void SpectacleMainWindow::onImageSaved(const QUrl &location)
{
    setUnsaved(false, location.fileName());
    //TODO FIXME
//     showInlineMessage(i18n("The screenshot was saved as <a href=\"%1\">%2</a>", location.toString(), location.fileName()),
//                       KMessageWidget::Positive,
//                       MessageDuration::AutoHide,
//                       {mOpenContaining});
}

void SpectacleMainWindow::onImageSavedAndCopied(const QUrl &location)
{
    setUnsaved(false, location.fileName());
    //TODO FIXME
//     showInlineMessage(i18n("The screenshot was copied to the clipboard and saved as <a href=\"%1\">%2</a>", location.toString(), location.fileName()),
//                       KMessageWidget::Positive,
//                       MessageDuration::AutoHide,
//                       {mOpenContaining});
}

void SpectacleMainWindow::save()
{
    Settings::setLastUsedSaveMode(Settings::Save);
    setDefaultSaveAction();

    const bool quitChecked = Settings::quitAfterSaveCopyExport();
    ExportManager::instance()->doSave(QUrl(), /* notify */ quitChecked);
    if (quitChecked) {
        quitExternally();
    }
}

void SpectacleMainWindow::saveAs()
{
    Settings::setLastUsedSaveMode(Settings::SaveAs);
    setDefaultSaveAction();

    const bool quitChecked = Settings::quitAfterSaveCopyExport();
    if (ExportManager::instance()->doSaveAs(/* notify */ quitChecked) && quitChecked) {
        quitExternally();
    }
}

void SpectacleMainWindow::restoreWindowTitle()
{
    if (isUnsaved()) {
        setTitle(i18nc("@title:window Unsaved Screenshot", "Unsaved[*]"));
    } else {
        // if a screenshot is not visible inside of mKSWidget, it means we have launched spectacle
        // with the last mode set to 'rectangular region' and canceled the screenshot
        //TODO FIXME
//         if (mKSWidget->isScreenshotSet()) {
//             setTitle(Settings::lastSaveLocation().fileName());
//         } else {
            setPlaceholderTextOnLaunch();
//         }
#ifdef KIMAGEANNOTATOR_FOUND
//         mAnnotateButton->setEnabled(mKSWidget->isScreenshotSet());
#endif
    }
}
