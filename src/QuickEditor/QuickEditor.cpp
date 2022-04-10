/*
 *  SPDX-FileCopyrightText: 2018 Ambareesh "Amby" Balaji <ambareeshbalaji@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "QuickEditor.h"

#include "ComparableQPoint.h"
#include "settings.h"

#include <KLocalizedString>
#include <KWayland/Client/plasmashell.h>
#include <KWayland/Client/surface.h>
#include <KWindowSystem>

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QX11Info>
#include <QtMath>

const int QuickEditor::handleRadiusMouse = 9;
const int QuickEditor::handleRadiusTouch = 12;
const qreal QuickEditor::increaseDragAreaFactor = 2.0;
const int QuickEditor::minSpacingBetweenHandles = 20;
const int QuickEditor::borderDragAreaSize = 10;

const int QuickEditor::selectionSizeThreshold = 100;

const int QuickEditor::selectionBoxPaddingX = 5;
const int QuickEditor::selectionBoxPaddingY = 4;
const int QuickEditor::selectionBoxMarginY = 5;

bool QuickEditor::bottomCaptureInstructionPrepared = false;
const int QuickEditor::bottomCaptureInstructionBoxPaddingX = 12;
const int QuickEditor::bottomCaptureInstructionBoxPaddingY = 8;
const int QuickEditor::bottomCaptureInstructionBoxPairSpacing = 6;
const int QuickEditor::bottomCaptureInstructionBoxMarginBottom = 5;
const int QuickEditor::midCaptureInstructionFontSize = 12;

const int QuickEditor::magnifierLargeStep = 15;

const int QuickEditor::magZoom = 5;
const int QuickEditor::magPixels = 16;
const int QuickEditor::magOffset = 32;

QuickEditor::QuickEditor(const QMap<const QScreen *, QImage> &images, KWayland::Client::PlasmaShell *plasmashell, QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::Popup | Qt::WindowStaysOnTopHint)
    , mMaskColor(QColor::fromRgbF(0, 0, 0, 0.5))
    , mStrokeColor(palette().highlight().color())
    , mCrossColor(QColor::fromRgbF(mStrokeColor.redF(), mStrokeColor.greenF(), mStrokeColor.blueF(), 0.7))
    , mLabelBackgroundColor(QColor::fromRgbF(palette().light().color().redF(), palette().light().color().greenF(), palette().light().color().blueF(), 0.85))
    , mLabelForegroundColor(palette().windowText().color())
    , mMidCaptureInstruction(i18n("Click and drag to draw a selection rectangle,\nor press Esc to quit"))
    , mMidCaptureInstructionFont(font())
    , mBottomCaptureInstructionFont(font())
    , mBottomCaptureInstructionGridLeftWidth(0)
    , mMouseDragState(MouseState::None)
    , mImages(images)
    , mMagnifierAllowed(false)
    , mShowMagnifier(Settings::showMagnifier())
    , mToggleMagnifier(false)
    , mReleaseToCapture(Settings::useReleaseToCapture())
    , mShowCaptureInstructions(Settings::showCaptureInstructions())
    , mDisableArrowKeys(false)
    , mbottomCaptureInstructionsLength(bottomCaptureInstructionLength)
    , mHandleRadius(handleRadiusMouse)
{
    if (Settings::useLightMaskColour()) {
        mMaskColor = QColor(255, 255, 255, 127);
    }

    setMouseTracking(true);
    setAttribute(Qt::WA_StaticContents);

    devicePixelRatio = plasmashell ? 1.0 : devicePixelRatioF();
    devicePixelRatioI = 1.0 / devicePixelRatio;

    preparePaint();
    createPixmapFromScreens();
    setGeometryToScreenPixmap(plasmashell);

    if (!(Settings::rememberLastRectangularRegion() == Settings::EnumRememberLastRectangularRegion::Never)) {
        auto savedRect = Settings::cropRegion();
        QRect cropRegion = QRect(savedRect[0], savedRect[1], savedRect[2], savedRect[3]);
        if (!cropRegion.isEmpty()) {
            mSelection = QRect(cropRegion.x() * devicePixelRatioI,
                               cropRegion.y() * devicePixelRatioI,
                               cropRegion.width() * devicePixelRatioI,
                               cropRegion.height() * devicePixelRatioI)
                             .intersected(rect());
        }
        setMouseCursor(QCursor::pos());
    } else {
        setCursor(Qt::CrossCursor);
    }

    setBottomCaptureInstructions();
    mMidCaptureInstructionFont.setPointSize(midCaptureInstructionFontSize);
    if (!bottomCaptureInstructionPrepared) {
        bottomCaptureInstructionPrepared = true;
        const auto prepare = [this](QStaticText &item) {
            item.prepare(QTransform(), mBottomCaptureInstructionFont);
            item.setPerformanceHint(QStaticText::AggressiveCaching);
        };
        for (auto &pair : mBottomCaptureInstructions) {
            prepare(pair.first);
            for (auto &item : pair.second) {
                prepare(item);
            }
        }
    }
    layoutBottomCaptureInstructions();

    update();
}

void QuickEditor::acceptSelection()
{
    if (!mSelection.isEmpty()) {
        QRect scaledCropRegion = QRect(qRound(mSelection.x() * devicePixelRatio),
                                       qRound(mSelection.y() * devicePixelRatio),
                                       qRound(mSelection.width() * devicePixelRatio),
                                       qRound(mSelection.height() * devicePixelRatio));
        Settings::setCropRegion({scaledCropRegion.x(), scaledCropRegion.y(), scaledCropRegion.width(), scaledCropRegion.height()});

        if (KWindowSystem::isPlatformX11()) {
            Q_EMIT grabDone(mPixmap.copy(scaledCropRegion));

        } else {
            // Wayland case
            qreal maxDpr = 1.0;
            for (const QScreen *screen : QGuiApplication::screens()) {
                if (screen->devicePixelRatio() > maxDpr) {
                    maxDpr = screen->devicePixelRatio();
                }
            }

            QPixmap output(mSelection.size() * maxDpr);
            QPainter painter(&output);

            for (auto it = mScreenToDpr.constBegin(); it != mScreenToDpr.constEnd(); ++it) {
                const QScreen *screen = it.key();
                const QRect &screenRect = screen->geometry();

                if (mSelection.intersects(screenRect)) {
                    const ComparableQPoint pos = screenRect.topLeft();
                    const qreal dpr = it.value();

                    QRect intersected = screenRect.intersected(mSelection);

                    // converts to screen size & position
                    QRect pixelOnScreenIntersected;
                    pixelOnScreenIntersected.moveTopLeft((intersected.topLeft() - pos) * dpr);
                    pixelOnScreenIntersected.setWidth(intersected.width() * dpr);
                    pixelOnScreenIntersected.setHeight(intersected.height() * dpr);

                    QPixmap screenOutput = QPixmap::fromImage(mImages.value(screen).copy(pixelOnScreenIntersected));

                    if (intersected.size() == mSelection.size()) {
                        // short path when single screen
                        // keep native screen resolution
                        Q_EMIT grabDone(screenOutput);
                        return;
                    }

                    // upscale the image according to max screen dpr, to keep the image not distorted
                    const auto dprI = maxDpr / dpr;
                    QBrush brush(screenOutput);
                    brush.setTransform(QTransform::fromScale(dprI, dprI));
                    intersected.moveTopLeft((intersected.topLeft() - mSelection.topLeft()) * maxDpr);
                    intersected.setSize(intersected.size() * maxDpr);
                    painter.setBrushOrigin(intersected.topLeft());
                    painter.fillRect(intersected, brush);
                }
            }

            Q_EMIT grabDone(output);
        }
    }
}

void QuickEditor::keyPressEvent(QKeyEvent *event)
{
    const auto modifiers = event->modifiers();
    const bool shiftPressed = modifiers & Qt::ShiftModifier;
    const bool altPressed = modifiers & Qt::AltModifier;
    if (shiftPressed) {
        mToggleMagnifier = true;
        update();
    }
    switch (event->key()) {
    case Qt::Key_Escape:
        Q_EMIT grabCancelled();
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        acceptSelection();
        break;
    case Qt::Key_Up: {
        if (mDisableArrowKeys) {
            update();
            break;
        }
        if (altPressed) {
            if (shiftPressed) {
                const int newBottom = mSelection.bottom() - 1;
                mSelection.setBottom((newBottom < 0) ? 0 : newBottom);
            } else {
                const qreal newScaledBottom = mSelection.bottom() * devicePixelRatio - magnifierLargeStep;
                mSelection.setBottom(qRound(devicePixelRatioI * ((newScaledBottom < 0) ? 0 : newScaledBottom)));
            }
            mSelection = mSelection.normalized();
        } else {
            if (shiftPressed) {
                const int newTop = mSelection.top() - 1;
                mSelection.moveTop((newTop < 0) ? 0 : newTop);
            } else {
                const qreal newScaledTop = mSelection.top() * devicePixelRatio - magnifierLargeStep;
                mSelection.moveTop(qRound(devicePixelRatioI * ((newScaledTop < 0) ? 0 : newScaledTop)));
            }
        }
        update();
        break;
    }
    case Qt::Key_Left: {
        if (mDisableArrowKeys) {
            update();
            break;
        }
        if (altPressed) {
            if (shiftPressed) {
                const int newRight = mSelection.right() - 1;
                mSelection.setRight((newRight < 0) ? 0 : newRight);
            } else {
                const qreal newScaledRight = mSelection.right() * devicePixelRatio - magnifierLargeStep;
                mSelection.setRight(qRound(devicePixelRatioI * ((newScaledRight < 0) ? 0 : newScaledRight)));
            }
            mSelection = mSelection.normalized();
        } else {
            if (shiftPressed) {
                const int newLeft = mSelection.left() - 1;
                mSelection.moveLeft((newLeft < 0) ? 0 : newLeft);
            } else {
                const qreal newScaledLeft = mSelection.left() * devicePixelRatio - magnifierLargeStep;
                mSelection.moveLeft(qRound(devicePixelRatioI * ((newScaledLeft < 0) ? 0 : newScaledLeft)));
            }
        }
        update();
        break;
    }
    case Qt::Key_Down: {
        if (mDisableArrowKeys) {
            update();
            break;
        }
        const int newBottom = mSelection.bottom() + 1;
        const qreal newScaledBottom = mSelection.bottom() * devicePixelRatio + magnifierLargeStep;
        const qreal scaledHeight = height() * devicePixelRatio;
        if (altPressed) {
            if (shiftPressed) {
                mSelection.setBottom(qMin(height(), newBottom));
            } else {
                mSelection.setBottom(qRound(devicePixelRatioI * qMin(scaledHeight, newScaledBottom)));
            }
            mSelection = mSelection.normalized();
        } else {
            if (shiftPressed) {
                mSelection.moveBottom(qMin(height(), newBottom));
            } else {
                mSelection.moveBottom(qRound(devicePixelRatioI * qMin(scaledHeight, newScaledBottom)));
            }
        }
        update();
        break;
    }
    case Qt::Key_Right: {
        if (mDisableArrowKeys) {
            update();
            break;
        }
        const int newRight = mSelection.right() + 1;
        const qreal newScaledRight = mSelection.right() * devicePixelRatio + magnifierLargeStep;
        const qreal scaledWidth = width() * devicePixelRatio;
        if (altPressed) {
            if (shiftPressed) {
                mSelection.setRight(qMin(width(), newRight));
            } else {
                mSelection.setRight(qRound(devicePixelRatioI * qMin(scaledWidth, newScaledRight)));
            }
            mSelection = mSelection.normalized();
        } else {
            if (shiftPressed) {
                mSelection.moveRight(qMin(width(), newRight));
            } else {
                mSelection.moveRight(qRound(devicePixelRatioI * qMin(scaledWidth, newScaledRight)));
            }
        }
        update();
        break;
    }
    default:
        break;
    }
    event->accept();
}

void QuickEditor::keyReleaseEvent(QKeyEvent *event)
{
    if (mToggleMagnifier && !(event->modifiers() & Qt::ShiftModifier)) {
        mToggleMagnifier = false;
        update();
    }
    event->accept();
}

void QuickEditor::mousePressEvent(QMouseEvent *event)
{
    if (event->source() == Qt::MouseEventNotSynthesized) {
        mHandleRadius = handleRadiusMouse;
    } else {
        mHandleRadius = handleRadiusTouch;
    }

    if (event->button() & Qt::LeftButton) {
        /* NOTE  Workaround for Bug 407843
         * If we show the selection Widget when a right click menu is open we lose focus on X.
         * When the user clicks we get the mouse back. We can only grab the keyboard if we already
         * have mouse focus. So just grab it undconditionally here.
         */
        grabKeyboard();
        mMousePos = event->pos();
        mMagnifierAllowed = true;
        mMouseDragState = mouseLocation(mMousePos);
        mDisableArrowKeys = true;
        switch (mMouseDragState) {
        case MouseState::Outside:
            mStartPos = mMousePos;
            break;
        case MouseState::Inside:
            mStartPos = mMousePos;
            mMagnifierAllowed = false;
            mInitialTopLeft = mSelection.topLeft();
            setCursor(Qt::ClosedHandCursor);
            break;
        case MouseState::Top:
        case MouseState::Left:
        case MouseState::TopLeft:
            mStartPos = mSelection.bottomRight();
            break;
        case MouseState::Bottom:
        case MouseState::Right:
        case MouseState::BottomRight:
            mStartPos = mSelection.topLeft();
            break;
        case MouseState::TopRight:
            mStartPos = mSelection.bottomLeft();
            break;
        case MouseState::BottomLeft:
            mStartPos = mSelection.topRight();
            break;
        default:
            break;
        }
    }
    if (mMagnifierAllowed) {
        update();
    }
    event->accept();
}

void QuickEditor::mouseMoveEvent(QMouseEvent *event)
{
    mMousePos = event->pos();
    mMagnifierAllowed = true;
    switch (mMouseDragState) {
    case MouseState::None: {
        setMouseCursor(mMousePos);
        mMagnifierAllowed = false;
        break;
    }
    case MouseState::TopLeft:
    case MouseState::TopRight:
    case MouseState::BottomRight:
    case MouseState::BottomLeft: {
        const bool afterX = mMousePos.x() >= mStartPos.x();
        const bool afterY = mMousePos.y() >= mStartPos.y();
        mSelection.setRect(afterX ? mStartPos.x() : mMousePos.x(),
                           afterY ? mStartPos.y() : mMousePos.y(),
                           qAbs(mMousePos.x() - mStartPos.x()) + (afterX ? devicePixelRatioI : 0),
                           qAbs(mMousePos.y() - mStartPos.y()) + (afterY ? devicePixelRatioI : 0));
        update();
        break;
    }
    case MouseState::Outside: {
        mSelection.setRect(qMin(mMousePos.x(), mStartPos.x()),
                           qMin(mMousePos.y(), mStartPos.y()),
                           qAbs(mMousePos.x() - mStartPos.x()) + devicePixelRatioI,
                           qAbs(mMousePos.y() - mStartPos.y()) + devicePixelRatioI);
        update();
        break;
    }
    case MouseState::Top:
    case MouseState::Bottom: {
        const bool afterY = mMousePos.y() >= mStartPos.y();
        mSelection.setRect(mSelection.x(),
                           afterY ? mStartPos.y() : mMousePos.y(),
                           mSelection.width(),
                           qAbs(mMousePos.y() - mStartPos.y()) + (afterY ? devicePixelRatioI : 0));
        update();
        break;
    }
    case MouseState::Right:
    case MouseState::Left: {
        const bool afterX = mMousePos.x() >= mStartPos.x();
        mSelection.setRect(afterX ? mStartPos.x() : mMousePos.x(),
                           mSelection.y(),
                           qAbs(mMousePos.x() - mStartPos.x()) + (afterX ? devicePixelRatioI : 0),
                           mSelection.height());
        update();
        break;
    }
    case MouseState::Inside: {
        mMagnifierAllowed = false;
        // We use some math here to figure out if the diff with which we
        // move the rectangle with moves it out of bounds,
        // in which case we adjust the diff to not let that happen

        // new top left point of the rectangle
        QPoint newTopLeft = ((mMousePos - mStartPos + mInitialTopLeft) * devicePixelRatio).toPoint();

        const QRect newRect(newTopLeft, mSelection.size() * devicePixelRatio);

        const QRect translatedScreensRect = mScreensRect.translated(-mScreensRect.topLeft());
        if (!translatedScreensRect.contains(newRect)) {
            // Keep the item inside the scene screen region bounding rect.
            newTopLeft.setX(qMin(translatedScreensRect.right() - newRect.width(), qMax(newTopLeft.x(), translatedScreensRect.left())));
            newTopLeft.setY(qMin(translatedScreensRect.bottom() - newRect.height(), qMax(newTopLeft.y(), translatedScreensRect.top())));
        }

        mSelection.moveTo(newTopLeft * devicePixelRatioI);
        update();
        break;
    }
    default:
        break;
    }

    event->accept();
}

void QuickEditor::mouseReleaseEvent(QMouseEvent *event)
{
    switch (event->button()) {
    case Qt::LeftButton:
        if (mMouseDragState == MouseState::Outside && mReleaseToCapture) {
            acceptSelection();
            return;
        }
        mDisableArrowKeys = false;
        if (mMouseDragState == MouseState::Inside) {
            setCursor(Qt::OpenHandCursor);
        }
        break;
    case Qt::RightButton:
        mSelection.setWidth(0);
        mSelection.setHeight(0);
        break;
    default:
        break;
    }
    event->accept();
    mMouseDragState = MouseState::None;
    update();
}

void QuickEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
    if (event->button() == Qt::LeftButton && mSelection.contains(event->pos())) {
        acceptSelection();
    }
}

QMap<ComparableQPoint, ComparableQPoint> QuickEditor::computeCoordinatesAfterScaling(const QMap<ComparableQPoint, QPair<qreal, QSize>> &outputsRect)
{
    QMap<ComparableQPoint, ComparableQPoint> translationMap;

    for (auto i = outputsRect.keyBegin(); i != outputsRect.keyEnd(); ++i) {
        translationMap.insert(*i, *i);
    }

    for (auto i = outputsRect.constBegin(); i != outputsRect.constEnd(); ++i) {
        const ComparableQPoint p = i.key();
        const QSize &size = i.value().second;
        const double dpr = i.value().first;
        if (!qFuzzyCompare(dpr, 1.0)) {
            // must update all coordinates of next rects
            int newWidth = size.width();
            int newHeight = size.height();

            int deltaX = newWidth - (size.width());
            int deltaY = newHeight - (size.height());

            // for the next size
            for (auto i2 = outputsRect.constFind(p); i2 != outputsRect.constEnd(); ++i2) {
                auto point = i2.key();
                auto finalPoint = translationMap.value(point);

                if (point.x() >= newWidth + p.x() - deltaX) {
                    finalPoint.setX(finalPoint.x() + deltaX);
                }
                if (point.y() >= newHeight + p.y() - deltaY) {
                    finalPoint.setY(finalPoint.y() + deltaY);
                }
                // update final position point with the necessary deltas
                translationMap.insert(point, finalPoint);
            }
        }
    }

    return translationMap;
}

void QuickEditor::preparePaint()
{
    for (auto i = mImages.constBegin(); i != mImages.constEnd(); ++i) {
        const QScreen *screen = i.key();
        const QImage &screenImage = i.value();

        const qreal dpr = screenImage.width() / static_cast<qreal>(screen->geometry().width());
        mScreenToDpr.insert(screen, dpr);

        QRect virtualScreenRect;
        if (KWindowSystem::isPlatformX11()) {
            virtualScreenRect = QRect(screen->geometry().topLeft(), screenImage.size());
        } else {
            virtualScreenRect = QRect(screen->geometry().topLeft(), screenImage.size() / dpr);
        }
        mScreensRect = mScreensRect.united(virtualScreenRect);
    }
}

void QuickEditor::createPixmapFromScreens()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    QMap<ComparableQPoint, QPair<qreal, QSize>> input;
    for (auto it = mImages.constBegin(); it != mImages.constEnd(); ++it) {
        const QScreen *screen = it.key();
        const QImage &screenImage = it.value();
        input.insert(screen->geometry().topLeft(), QPair<qreal, QSize>(screenImage.width() / static_cast<qreal>(screen->size().width()), screenImage.size()));
    }
    const auto pointsTranslationMap = computeCoordinatesAfterScaling(input);

    // Geometry can have negative coordinates, so it is necessary to subtract the upper left point, because coordinates on the widget are counted from 0
    mPixmap = QPixmap(mScreensRect.width(), mScreensRect.height());
    QPainter painter(&mPixmap);
    for (auto it = mImages.constBegin(); it != mImages.constEnd(); ++it) {
        painter.drawImage(pointsTranslationMap.value(it.key()->geometry().topLeft()) - mScreensRect.topLeft(), it.value());
    }
}

void QuickEditor::setGeometryToScreenPixmap(KWayland::Client::PlasmaShell *plasmashell)
{
    if (!KWindowSystem::isPlatformX11()) {
        setGeometry(mScreensRect);
    } else {
        // Even though we want the quick editor window to be placed at (0, 0) in the native
        // pixels, we cannot really specify a window position of (0, 0) if HiDPI support is on.
        //
        // The main reason for that is that Qt will scale the window position relative to the
        // upper left corner of the screen where the quick editor is on in order to perform
        // a conversion from the device-independent coordinates to the native pixels.
        //
        // Since (0, 0) in the device-independent pixels may not correspond to (0, 0) in the
        // native pixels, we use XCB API to place the quick editor window at (0, 0).

        uint16_t mask = 0;

        mask |= XCB_CONFIG_WINDOW_X;
        mask |= XCB_CONFIG_WINDOW_Y;

        const uint32_t values[] = {
            /* x */ 0,
            /* y */ 0,
        };

        xcb_configure_window(QX11Info::connection(), winId(), mask, values);
        resize(qRound(mScreensRect.width() / devicePixelRatio), qRound(mScreensRect.height() / devicePixelRatio));
    }

    // TODO This is a hack until a better interface is available
    if (plasmashell) {
        using namespace KWayland::Client;
        winId();
        auto surface = Surface::fromWindow(windowHandle());
        if (surface) {
            PlasmaShellSurface *plasmashellSurface = plasmashell->createSurface(surface, this);
            plasmashellSurface->setRole(PlasmaShellSurface::Role::Panel);
            plasmashellSurface->setPanelTakesFocus(true);
            plasmashellSurface->setPosition(geometry().topLeft());
        }
    }
}

void QuickEditor::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.eraseRect(rect());

    for (auto i = mImages.constBegin(); i != mImages.constEnd(); ++i) {
        const QImage &screenImage = i.value();
        const QScreen *screen = i.key();

        QRect rectToDraw = screen->geometry().translated(-mScreensRect.topLeft());
        const qreal dpr = screenImage.width() / static_cast<qreal>(rectToDraw.width());
        const qreal dprI = 1.0 / dpr;

        QBrush brush(screenImage);
        brush.setTransform(QTransform::fromScale(dprI, dprI));

        rectToDraw.moveTopLeft(rectToDraw.topLeft() / devicePixelRatio);
        if (KWindowSystem::isPlatformWayland()) {
            rectToDraw.setSize(rectToDraw.size() * devicePixelRatio);
        }

        painter.setBrushOrigin(rectToDraw.topLeft());
        painter.fillRect(rectToDraw, brush);
    }

    if (!mSelection.size().isEmpty() || mMouseDragState != MouseState::None) {
        const QRectF innerRect = mSelection.adjusted(1, 1, -1, -1);
        if (innerRect.width() > 0 && innerRect.height() > 0) {
            painter.setPen(mStrokeColor);
            painter.drawLine(mSelection.topLeft(), mSelection.topRight());
            painter.drawLine(mSelection.bottomRight(), mSelection.topRight());
            painter.drawLine(mSelection.bottomRight(), mSelection.bottomLeft());
            painter.drawLine(mSelection.bottomLeft(), mSelection.topLeft());
        }

        QRectF top(0, 0, width(), mSelection.top());
        QRectF right(mSelection.right(), mSelection.top(), width() - mSelection.right(), mSelection.height());
        QRectF bottom(0, mSelection.bottom() + 1, width(), height() - mSelection.bottom());
        QRectF left(0, mSelection.top(), mSelection.left(), mSelection.height());
        for (const auto &rect : {top, right, bottom, left}) {
            painter.fillRect(rect, mMaskColor);
        }

        bool dragHandlesVisible = false;
        if (mMouseDragState == MouseState::None) {
            dragHandlesVisible = true;
            drawDragHandles(painter);
        } else if (mMagnifierAllowed && (mShowMagnifier ^ mToggleMagnifier)) {
            drawMagnifier(painter);
        }
        drawSelectionSizeTooltip(painter, dragHandlesVisible);

        if (mShowCaptureInstructions) {
            drawBottomCaptureInstructions(painter);
        }
    } else {
        if (mShowCaptureInstructions) {
            drawMidCaptureInstructions(painter);
        }
    }
}

void QuickEditor::layoutBottomCaptureInstructions()
{
    int maxRightWidth = 0;
    int contentWidth = 0;
    int contentHeight = 0;
    mBottomCaptureInstructionGridLeftWidth = 0;
    for (int i = 0; i < mbottomCaptureInstructionsLength; i++) {
        const auto &item = mBottomCaptureInstructions[i];
        const auto &left = item.first;
        const auto &right = item.second;
        const auto leftSize = left.size().toSize();
        mBottomCaptureInstructionGridLeftWidth = qMax(mBottomCaptureInstructionGridLeftWidth, leftSize.width());
        for (const auto &item : right) {
            const auto rightItemSize = item.size().toSize();
            maxRightWidth = qMax(maxRightWidth, rightItemSize.width());
            contentHeight += rightItemSize.height();
        }
        contentWidth = qMax(contentWidth, mBottomCaptureInstructionGridLeftWidth + maxRightWidth + bottomCaptureInstructionBoxPairSpacing);
        contentHeight += (i != bottomCaptureInstructionLength ? bottomCaptureInstructionBoxMarginBottom : 0);
    }
    const QRect primaryGeometry = QGuiApplication::primaryScreen()->geometry().translated(-mScreensRect.topLeft());
    mBottomCaptureInstructionContentPos.setX((primaryGeometry.width() - contentWidth) / 2 + primaryGeometry.x() / devicePixelRatio);
    mBottomCaptureInstructionContentPos.setY((primaryGeometry.height() + primaryGeometry.y() / devicePixelRatio) - contentHeight - 8);
    mBottomCaptureInstructionGridLeftWidth += mBottomCaptureInstructionContentPos.x();
    mBottomCaptureInstructionBorderBox.setRect(mBottomCaptureInstructionContentPos.x() - bottomCaptureInstructionBoxPaddingX,
                                               mBottomCaptureInstructionContentPos.y() - bottomCaptureInstructionBoxPaddingY,
                                               contentWidth + bottomCaptureInstructionBoxPaddingX * 2,
                                               contentHeight + bottomCaptureInstructionBoxPaddingY * 2 - 1);
}

void QuickEditor::setBottomCaptureInstructions()
{
    if (mReleaseToCapture && mSelection.size().isEmpty()) {
        // Release to capture enabled and NO saved region available
        mbottomCaptureInstructionsLength = 3;
        mBottomCaptureInstructions[0] = {QStaticText(i18n("Take Screenshot:")),
                                         {QStaticText(i18nc("Mouse action", "Release left-click")), QStaticText(i18nc("Keyboard action", "Enter"))}};
        mBottomCaptureInstructions[1] = {
            QStaticText(i18n("Create new selection rectangle:")),
            {QStaticText(i18nc("Mouse action", "Drag outside selection rectangle")), QStaticText(i18nc("Keyboard action", "+ Shift: Magnifier"))}};
        mBottomCaptureInstructions[2] = {QStaticText(i18n("Cancel:")), {QStaticText(i18nc("Keyboard action", "Escape"))}};
    } else {
        // Default text, Release to capture option disabled
        mBottomCaptureInstructions[0] = {QStaticText(i18n("Take Screenshot:")),
                                         {QStaticText(i18nc("Mouse action", "Double-click")), QStaticText(i18nc("Keyboard action", "Enter"))}};
        mBottomCaptureInstructions[1] = {
            QStaticText(i18n("Create new selection rectangle:")),
            {QStaticText(i18nc("Mouse action", "Drag outside selection rectangle")), QStaticText(i18nc("Keyboard action", "+ Shift: Magnifier"))}};
        mBottomCaptureInstructions[2] = {QStaticText(i18n("Move selection rectangle:")),
                                         {QStaticText(i18nc("Mouse action", "Drag inside selection rectangle")),
                                          QStaticText(i18nc("Keyboard action", "Arrow keys")),
                                          QStaticText(i18nc("Keyboard action", "+ Shift: Move in 1 pixel steps"))}};
        mBottomCaptureInstructions[3] = {QStaticText(i18n("Resize selection rectangle:")),
                                         {QStaticText(i18nc("Mouse action", "Drag handles")),
                                          QStaticText(i18nc("Keyboard action", "Arrow keys + Alt")),
                                          QStaticText(i18nc("Keyboard action", "+ Shift: Resize in 1 pixel steps"))}};
        mBottomCaptureInstructions[4] = {QStaticText(i18n("Reset selection:")), {QStaticText(i18nc("Mouse action", "Right-click"))}};
        mBottomCaptureInstructions[5] = {QStaticText(i18n("Cancel:")), {QStaticText(i18nc("Keyboard action", "Escape"))}};
    }
}

void QuickEditor::drawBottomCaptureInstructions(QPainter &painter)
{
    if (mSelection.intersects(mBottomCaptureInstructionBorderBox)) {
        return;
    }

    painter.setBrush(mLabelBackgroundColor);
    painter.setPen(mLabelForegroundColor);
    painter.setFont(mBottomCaptureInstructionFont);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.drawRect(mBottomCaptureInstructionBorderBox);
    painter.setRenderHint(QPainter::Antialiasing, true);

    int topOffset = mBottomCaptureInstructionContentPos.y();
    for (int i = 0; i < mbottomCaptureInstructionsLength; i++) {
        const auto &item = mBottomCaptureInstructions[i];
        const auto &left = item.first;
        const auto &right = item.second;
        const auto leftSize = left.size().toSize();
        painter.drawStaticText(mBottomCaptureInstructionGridLeftWidth - leftSize.width(), topOffset, left);
        for (const auto &item : right) {
            const auto rightItemSize = item.size().toSize();
            painter.drawStaticText(mBottomCaptureInstructionGridLeftWidth + bottomCaptureInstructionBoxPairSpacing, topOffset, item);
            topOffset += rightItemSize.height();
        }
        if (i != bottomCaptureInstructionLength) {
            topOffset += bottomCaptureInstructionBoxMarginBottom;
        }
    }
}

void QuickEditor::drawDragHandles(QPainter &painter)
{
    // Rectangular region
    const qreal left = mSelection.x();
    const qreal centerX = left + mSelection.width() / 2.0;
    const qreal right = left + mSelection.width();
    const qreal top = mSelection.y();
    const qreal centerY = top + mSelection.height() / 2.0;
    const qreal bottom = top + mSelection.height();

    // rectangle too small: make handles free-floating
    qreal offset = 0;
    // rectangle too close to screen edges: move handles on that edge inside the rectangle, so they're still visible
    qreal offsetTop = 0;
    qreal offsetRight = 0;
    qreal offsetBottom = 0;
    qreal offsetLeft = 0;

    const qreal minDragHandleSpace = 4 * mHandleRadius + 2 * minSpacingBetweenHandles;
    const qreal minEdgeLength = qMin(mSelection.width(), mSelection.height());
    if (minEdgeLength < minDragHandleSpace) {
        offset = (minDragHandleSpace - minEdgeLength) / 2.0;
    } else {
        const QRect translatedScreensRect = mScreensRect.translated(-mScreensRect.topLeft());
        const int penWidth = painter.pen().width();

        offsetTop = top - translatedScreensRect.top() - mHandleRadius;
        offsetTop = (offsetTop >= 0) ? 0 : offsetTop;

        offsetRight = translatedScreensRect.right() - right - mHandleRadius + penWidth;
        offsetRight = (offsetRight >= 0) ? 0 : offsetRight;

        offsetBottom = translatedScreensRect.bottom() - bottom - mHandleRadius + penWidth;
        offsetBottom = (offsetBottom >= 0) ? 0 : offsetBottom;

        offsetLeft = left - translatedScreensRect.left() - mHandleRadius;
        offsetLeft = (offsetLeft >= 0) ? 0 : offsetLeft;
    }

    // top-left handle
    this->mHandlePositions[0] = QPointF{left - offset - offsetLeft, top - offset - offsetTop};
    // top-right handle
    this->mHandlePositions[1] = QPointF{right + offset + offsetRight, top - offset - offsetTop};
    // bottom-right handle
    this->mHandlePositions[2] = QPointF{right + offset + offsetRight, bottom + offset + offsetBottom};
    // bottom-left
    this->mHandlePositions[3] = QPointF{left - offset - offsetLeft, bottom + offset + offsetBottom};
    // top-center handle
    this->mHandlePositions[4] = QPointF{centerX, top - offset - offsetTop};
    // right-center handle
    this->mHandlePositions[5] = QPointF{right + offset + offsetRight, centerY};
    // bottom-center handle
    this->mHandlePositions[6] = QPointF{centerX, bottom + offset + offsetBottom};
    // left-center handle
    this->mHandlePositions[7] = QPointF{left - offset - offsetLeft, centerY};

    // start path
    QPainterPath path;

    // add handles to the path
    for (QPointF handlePosition : std::as_const(mHandlePositions)) {
        path.addEllipse(handlePosition, mHandleRadius, mHandleRadius);
    }

    // draw the path
    painter.fillPath(path, mStrokeColor);
}

void QuickEditor::drawMagnifier(QPainter &painter)
{
    const int pixels = 2 * magPixels + 1;
    int magX = static_cast<int>(mMousePos.x() * devicePixelRatio - magPixels);
    int offsetX = 0;
    if (magX < 0) {
        offsetX = magX;
        magX = 0;
    } else {
        const int maxX = mPixmap.width() - pixels;
        if (magX > maxX) {
            offsetX = magX - maxX;
            magX = maxX;
        }
    }
    int magY = static_cast<int>(mMousePos.y() * devicePixelRatio - magPixels);
    int offsetY = 0;
    if (magY < 0) {
        offsetY = magY;
        magY = 0;
    } else {
        const int maxY = mPixmap.height() - pixels;
        if (magY > maxY) {
            offsetY = magY - maxY;
            magY = maxY;
        }
    }
    QRectF magniRect(magX, magY, pixels, pixels);

    qreal drawPosX = mMousePos.x() + magOffset + pixels * magZoom / 2;
    if (drawPosX > width() - pixels * magZoom / 2) {
        drawPosX = mMousePos.x() - magOffset - pixels * magZoom / 2;
    }
    qreal drawPosY = mMousePos.y() + magOffset + pixels * magZoom / 2;
    if (drawPosY > height() - pixels * magZoom / 2) {
        drawPosY = mMousePos.y() - magOffset - pixels * magZoom / 2;
    }
    QPointF drawPos(drawPosX, drawPosY);
    QRectF crossHairTop(drawPos.x() + magZoom * (offsetX - 0.5), drawPos.y() - magZoom * (magPixels + 0.5), magZoom, magZoom * (magPixels + offsetY));
    QRectF crossHairRight(drawPos.x() + magZoom * (0.5 + offsetX), drawPos.y() + magZoom * (offsetY - 0.5), magZoom * (magPixels - offsetX), magZoom);
    QRectF crossHairBottom(drawPos.x() + magZoom * (offsetX - 0.5), drawPos.y() + magZoom * (0.5 + offsetY), magZoom, magZoom * (magPixels - offsetY));
    QRectF crossHairLeft(drawPos.x() - magZoom * (magPixels + 0.5), drawPos.y() + magZoom * (offsetY - 0.5), magZoom * (magPixels + offsetX), magZoom);
    QRectF crossHairBorder(drawPos.x() - magZoom * (magPixels + 0.5) - 1,
                           drawPos.y() - magZoom * (magPixels + 0.5) - 1,
                           pixels * magZoom + 2,
                           pixels * magZoom + 2);
    const auto frag = QPainter::PixmapFragment::create(drawPos, magniRect, magZoom, magZoom);

    painter.fillRect(crossHairBorder, mLabelForegroundColor);
    painter.drawPixmapFragments(&frag, 1, mPixmap, QPainter::OpaqueHint);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (auto &rect : {crossHairTop, crossHairRight, crossHairBottom, crossHairLeft}) {
        painter.fillRect(rect, mCrossColor);
    }
}

void QuickEditor::drawMidCaptureInstructions(QPainter &painter)
{
    painter.fillRect(rect(), mMaskColor);
    painter.setFont(mMidCaptureInstructionFont);
    QRect textSize = painter.boundingRect(QRect(), Qt::AlignCenter, mMidCaptureInstruction);
    const QRect primaryGeometry = QGuiApplication::primaryScreen()->geometry().translated(-mScreensRect.topLeft());
    QPoint pos((primaryGeometry.width() - textSize.width()) / 2 + primaryGeometry.x() / devicePixelRatio,
               (primaryGeometry.height() - textSize.height()) / 2 + primaryGeometry.y() / devicePixelRatio);

    painter.setBrush(mLabelBackgroundColor);
    QPen pen(mLabelForegroundColor);
    pen.setWidth(2);
    painter.setPen(pen);
    painter.drawRoundedRect(QRect(pos.x() - 20, pos.y() - 20, textSize.width() + 40, textSize.height() + 40), 4, 4);

    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawText(QRect(pos, textSize.size()), Qt::AlignCenter, mMidCaptureInstruction);
}

void QuickEditor::drawSelectionSizeTooltip(QPainter &painter, bool dragHandlesVisible)
{
    // Set the selection size and finds the most appropriate position:
    // - vertically centered inside the selection if the box is not covering the a large part of selection
    // - on top of the selection if the selection x position fits the box height plus some margin
    // - at the bottom otherwise
    QString selectionSizeText =
        ki18n("%1×%2").subs(qRound(mSelection.width() * devicePixelRatio)).subs(qRound(mSelection.height() * devicePixelRatio)).toString();
    const QRect selectionSizeTextRect = painter.boundingRect(QRect(), 0, selectionSizeText);

    const int selectionBoxWidth = selectionSizeTextRect.width() + selectionBoxPaddingX * 2;
    const int selectionBoxHeight = selectionSizeTextRect.height() + selectionBoxPaddingY * 2;
    const int selectionBoxX =
        qBound(0,
               static_cast<int>(mSelection.x()) + (static_cast<int>(mSelection.width()) - selectionSizeTextRect.width()) / 2 - selectionBoxPaddingX,
               width() - selectionBoxWidth);
    int selectionBoxY;
    if ((mSelection.width() >= selectionSizeThreshold) && (mSelection.height() >= selectionSizeThreshold)) {
        // show inside the box
        selectionBoxY = static_cast<int>(mSelection.y() + (mSelection.height() - selectionSizeTextRect.height()) / 2);
    } else {
        // show on top by default, above the drag Handles if they're visible
        if (dragHandlesVisible) {
            selectionBoxY = static_cast<int>(mHandlePositions[4].y() - mHandleRadius - selectionBoxHeight - selectionBoxMarginY);
            if (selectionBoxY < 0) {
                selectionBoxY = static_cast<int>(mHandlePositions[6].y() + mHandleRadius + selectionBoxMarginY);
            }
        } else {
            selectionBoxY = static_cast<int>(mSelection.y() - selectionBoxHeight - selectionBoxMarginY);
            if (selectionBoxY < 0) {
                selectionBoxY = static_cast<int>(mSelection.y() + mSelection.height() + selectionBoxMarginY);
            }
        }
    }

    // Now do the actual box, border, and text drawing
    painter.setBrush(mLabelBackgroundColor);
    painter.setPen(mLabelForegroundColor);
    const QRect selectionBoxRect(selectionBoxX, selectionBoxY, selectionBoxWidth, selectionBoxHeight);

    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.drawRect(selectionBoxRect);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.drawText(selectionBoxRect, Qt::AlignCenter, selectionSizeText);
}

void QuickEditor::setMouseCursor(const QPointF &pos)
{
    MouseState mouseState = mouseLocation(pos);
    if (mouseState == MouseState::Outside) {
        setCursor(Qt::CrossCursor);
    } else if (MouseState::TopLeftOrBottomRight & mouseState) {
        setCursor(Qt::SizeFDiagCursor);
    } else if (MouseState::TopRightOrBottomLeft & mouseState) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (MouseState::TopOrBottom & mouseState) {
        setCursor(Qt::SizeVerCursor);
    } else if (MouseState::RightOrLeft & mouseState) {
        setCursor(Qt::SizeHorCursor);
    } else {
        setCursor(Qt::OpenHandCursor);
    }
}

QuickEditor::MouseState QuickEditor::mouseLocation(const QPointF &pos)
{
    auto isPointInsideCircle = [](const QPointF &circleCenter, qreal radius, const QPointF &point) {
        return (qPow(point.x() - circleCenter.x(), 2) + qPow(point.y() - circleCenter.y(), 2) <= qPow(radius, 2)) ? true : false;
    };

    if (isPointInsideCircle(mHandlePositions[0], mHandleRadius * increaseDragAreaFactor, pos)) {
        return MouseState::TopLeft;
    }
    if (isPointInsideCircle(mHandlePositions[1], mHandleRadius * increaseDragAreaFactor, pos)) {
        return MouseState::TopRight;
    }
    if (isPointInsideCircle(mHandlePositions[2], mHandleRadius * increaseDragAreaFactor, pos)) {
        return MouseState::BottomRight;
    }
    if (isPointInsideCircle(mHandlePositions[3], mHandleRadius * increaseDragAreaFactor, pos)) {
        return MouseState::BottomLeft;
    }
    if (isPointInsideCircle(mHandlePositions[4], mHandleRadius * increaseDragAreaFactor, pos)) {
        return MouseState::Top;
    }
    if (isPointInsideCircle(mHandlePositions[5], mHandleRadius * increaseDragAreaFactor, pos)) {
        return MouseState::Right;
    }
    if (isPointInsideCircle(mHandlePositions[6], mHandleRadius * increaseDragAreaFactor, pos)) {
        return MouseState::Bottom;
    }
    if (isPointInsideCircle(mHandlePositions[7], mHandleRadius * increaseDragAreaFactor, pos)) {
        return MouseState::Left;
    }

    auto inRange = [](qreal low, qreal high, qreal value) {
        return value >= low && value <= high;
    };

    auto withinThreshold = [](qreal offset, qreal threshold) {
        return qFabs(offset) <= threshold;
    };

    // Rectangle can be resized when border is dragged, if it's big enough
    if (mSelection.width() >= 100 && mSelection.height() >= 100) {
        if (inRange(mSelection.x(), mSelection.x() + mSelection.width(), pos.x())) {
            if (withinThreshold(pos.y() - mSelection.y(), borderDragAreaSize)) {
                return MouseState::Top;
            }
            if (withinThreshold(pos.y() - mSelection.y() - mSelection.height(), borderDragAreaSize)) {
                return MouseState::Bottom;
            }
        }
        if (inRange(mSelection.y(), mSelection.y() + mSelection.height(), pos.y())) {
            if (withinThreshold(pos.x() - mSelection.x(), borderDragAreaSize)) {
                return MouseState::Left;
            }
            if (withinThreshold(pos.x() - mSelection.x() - mSelection.width(), borderDragAreaSize)) {
                return MouseState::Right;
            }
        }
    }
    if (mSelection.contains(pos.toPoint())) {
        return MouseState::Inside;
    }
    return MouseState::Outside;
}
