/*
 *  SPDX-FileCopyrightText: 2018 Ambareesh "Amby" Balaji <ambareeshbalaji@gmail.com>
 *  SPDX-FileCopyrightText: 2022 Noah Davis <noahadvs@gmail.com>
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "SelectionEditor.h"

#include "Annotations/AnnotationDocument.h"
#include "SpectacleCore.h"
#include "Geometry.h"
#include "settings.h"
#include "spectacle_gui_debug.h"

#include <KLocalizedString>
#include <KWindowSystem>

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmapCache>
#include <QQuickWindow>
#include <QScreen>
#include <QtMath>
#include <qnamespace.h>

using G = Geometry;

class SelectionEditorSingleton
{
public:
    SelectionEditor self;
};

Q_GLOBAL_STATIC(SelectionEditorSingleton, privateSelectionEditorSelf)

static constexpr qreal s_handleRadiusMouse = 9;
static constexpr qreal s_handleRadiusTouch = 12;
static constexpr qreal s_minSpacingBetweenHandles = 20;
static constexpr qreal s_borderDragAreaSize = 10;
static constexpr qreal s_magnifierLargeStep = 15;

// SelectionEditorPrivate =====================

using MouseLocation = SelectionEditor::MouseLocation;

class SelectionEditorPrivate
{
public:
    SelectionEditorPrivate(SelectionEditor *q);

    SelectionEditor *const q;

    void updateDevicePixelRatio();
    void updateHandlePositions();

    int boundsLeft(int newTopLeftX, const bool mouse = true);
    int boundsRight(int newTopLeftX, const bool mouse = true);
    int boundsUp(int newTopLeftY, const bool mouse = true);
    int boundsDown(int newTopLeftY, const bool mouse = true);

    qreal dprRound(qreal value) const;

    void handleArrowKey(QKeyEvent *event);

    void setMouseCursor(QQuickItem *item, const QPointF &pos);
    MouseLocation mouseLocation(const QPointF &pos) const;

    const std::unique_ptr<Selection> selection;

    QPointF startPos;
    QPointF initialTopLeft;
    MouseLocation dragLocation = MouseLocation::None;
    QImage image;
    QVector<CanvasImage> screenImages;
    qreal devicePixelRatio = 1;
    qreal devicePixel = 1;
    QPointF mousePos;
    bool magnifierAllowed = false;
    bool toggleMagnifier = false;
    bool disableArrowKeys = false;
    QRectF screensRect;
    // Midpoints of handles
    QVector<QPointF> handlePositions = QVector<QPointF>{8};
    QRectF handlesRect;
    // Radius of handles is either handleRadiusMouse or handleRadiusTouch
    qreal handleRadius = s_handleRadiusMouse;
    qreal penWidth = 1;
    qreal penOffset = 0.5;
};

SelectionEditorPrivate::SelectionEditorPrivate(SelectionEditor *q)
    : q(q)
    , selection(new Selection(q))
{
}

void SelectionEditorPrivate::updateDevicePixelRatio()
{
    if (KWindowSystem::isPlatformWayland()) {
        devicePixelRatio = 1.0;
    } else {
        devicePixelRatio = qApp->devicePixelRatio();
    }

    devicePixel = G::dpx(devicePixelRatio);
    penWidth = dprRound(1.0);
    penOffset = penWidth / 2.0;
}

void SelectionEditorPrivate::updateHandlePositions()
{
    // Rectangular region
    const qreal left = selection->left();
    const qreal centerX = selection->horizontalCenter();
    const qreal right = selection->right();
    const qreal top = selection->top();
    const qreal centerY = selection->verticalCenter();
    const qreal bottom = selection->bottom();

    // rectangle too small: make handles free-floating
    qreal offset = 0;
    // rectangle too close to screen edges: move handles on that edge inside the rectangle, so they're still visible
    qreal offsetTop = 0;
    qreal offsetRight = 0;
    qreal offsetBottom = 0;
    qreal offsetLeft = 0;

    const qreal minDragHandleSpace = 4 * handleRadius + 2 * s_minSpacingBetweenHandles;
    const qreal minEdgeLength = qMin(selection->width(), selection->height());
    if (minEdgeLength < minDragHandleSpace) {
        offset = (minDragHandleSpace - minEdgeLength) / 2.0;
    } else {
        const auto translatedScreensRect = screensRect.translated(-screensRect.topLeft());

        offsetTop = top - translatedScreensRect.top() - handleRadius;
        offsetTop = (offsetTop >= 0) ? 0 : offsetTop;

        offsetRight = translatedScreensRect.right() - right - handleRadius + penWidth;
        offsetRight = (offsetRight >= 0) ? 0 : offsetRight;

        offsetBottom = translatedScreensRect.bottom() - bottom - handleRadius + penWidth;
        offsetBottom = (offsetBottom >= 0) ? 0 : offsetBottom;

        offsetLeft = left - translatedScreensRect.left() - handleRadius;
        offsetLeft = (offsetLeft >= 0) ? 0 : offsetLeft;
    }

    // top-left handle
    handlePositions[0] = QPointF{left - offset - offsetLeft, top - offset - offsetTop};
    // top-right handle
    handlePositions[1] = QPointF{right + offset + offsetRight, top - offset - offsetTop};
    // bottom-right handle
    handlePositions[2] = QPointF{right + offset + offsetRight, bottom + offset + offsetBottom};
    // bottom-left
    handlePositions[3] = QPointF{left - offset - offsetLeft, bottom + offset + offsetBottom};
    // top-center handle
    handlePositions[4] = QPointF{centerX, top - offset - offsetTop};
    // right-center handle
    handlePositions[5] = QPointF{right + offset + offsetRight, centerY};
    // bottom-center handle
    handlePositions[6] = QPointF{centerX, bottom + offset + offsetBottom};
    // left-center handle
    handlePositions[7] = QPointF{left - offset - offsetLeft, centerY};

    QPointF radiusOffset = {handleRadius, handleRadius};
    QRectF newHandlesRect = {handlePositions[0] - radiusOffset, // top left
                             handlePositions[2] + radiusOffset}; // bottom right
    if (handlesRect == newHandlesRect) {
        return;
    }
    handlesRect = newHandlesRect;
    Q_EMIT q->handlesRectChanged();
}

int SelectionEditorPrivate::boundsLeft(int newTopLeftX, const bool mouse)
{
    if (newTopLeftX < 0) {
        if (mouse) {
            // tweak startPos to prevent rectangle from getting stuck
            startPos.setX(startPos.x() + newTopLeftX * devicePixel);
        }
        newTopLeftX = 0;
    }

    return newTopLeftX;
}

int SelectionEditorPrivate::boundsRight(int newTopLeftX, const bool mouse)
{
    // the max X coordinate of the top left point
    const int realMaxX = qRound((q->width() - selection->width()) * devicePixelRatio);
    const int xOffset = newTopLeftX - realMaxX;
    if (xOffset > 0) {
        if (mouse) {
            startPos.setX(startPos.x() + xOffset * devicePixel);
        }
        newTopLeftX = realMaxX;
    }

    return newTopLeftX;
}

int SelectionEditorPrivate::boundsUp(int newTopLeftY, const bool mouse)
{
    if (newTopLeftY < 0) {
        if (mouse) {
            startPos.setY(startPos.y() + newTopLeftY * devicePixel);
        }
        newTopLeftY = 0;
    }

    return newTopLeftY;
}

int SelectionEditorPrivate::boundsDown(int newTopLeftY, const bool mouse)
{
    // the max Y coordinate of the top left point
    const int realMaxY = qRound((q->height() - selection->height()) * devicePixelRatio);
    const int yOffset = newTopLeftY - realMaxY;
    if (yOffset > 0) {
        if (mouse) {
            startPos.setY(startPos.y() + yOffset * devicePixel);
        }
        newTopLeftY = realMaxY;
    }

    return newTopLeftY;
}

qreal SelectionEditorPrivate::dprRound(qreal value) const
{
    return G::dprRound(value, devicePixelRatio);
}

void SelectionEditorPrivate::handleArrowKey(QKeyEvent *event)
{
    if (disableArrowKeys) {
        return;
    }

    const auto key = static_cast<Qt::Key>(event->key());
    const auto modifiers = event->modifiers();
    const bool modifySize = modifiers & Qt::AltModifier;
    const qreal step = modifiers & Qt::ShiftModifier ? devicePixel : dprRound(s_magnifierLargeStep);
    QRectF selectionRect = selection->rectF();

    if (key == Qt::Key_Left) {
        const int newPos = boundsLeft(qRound(selectionRect.left() * devicePixelRatio - step), false);
        if (modifySize) {
            selectionRect.setRight(devicePixel * newPos + selectionRect.width());
            selectionRect = selectionRect.normalized();
        } else {
            selectionRect.moveLeft(devicePixel * newPos);
        }
    } else if (key == Qt::Key_Right) {
        const int newPos = boundsRight(qRound(selectionRect.left() * devicePixelRatio + step), false);
        if (modifySize) {
            selectionRect.setRight(devicePixel * newPos + selectionRect.width());
        } else {
            selectionRect.moveLeft(devicePixel * newPos);
        }
    } else if (key == Qt::Key_Up) {
        const int newPos = boundsUp(qRound(selectionRect.top() * devicePixelRatio - step), false);
        if (modifySize) {
            selectionRect.setBottom(devicePixel * newPos + selectionRect.height());
            selectionRect = selectionRect.normalized();
        } else {
            selectionRect.moveTop(devicePixel * newPos);
        }
    } else if (key == Qt::Key_Down) {
        const int newPos = boundsDown(qRound(selectionRect.top() * devicePixelRatio + step), false);
        if (modifySize) {
            selectionRect.setBottom(devicePixel * newPos + selectionRect.height());
        } else {
            selectionRect.moveTop(devicePixel * newPos);
        }
    }
    selection->setRect(modifySize ? selectionRect : G::rectBounded(selectionRect, screensRect));
}

// TODO: change cursor with pointerhandlers in qml?
void SelectionEditorPrivate::setMouseCursor(QQuickItem *item, const QPointF &pos)
{
    MouseLocation mouseState = mouseLocation(pos);
    if (mouseState == MouseLocation::Outside) {
        item->setCursor(Qt::CrossCursor);
    } else if (MouseLocation::TopLeftOrBottomRight & mouseState) {
        item->setCursor(Qt::SizeFDiagCursor);
    } else if (MouseLocation::TopRightOrBottomLeft & mouseState) {
        item->setCursor(Qt::SizeBDiagCursor);
    } else if (MouseLocation::TopOrBottom & mouseState) {
        item->setCursor(Qt::SizeVerCursor);
    } else if (MouseLocation::RightOrLeft & mouseState) {
        item->setCursor(Qt::SizeHorCursor);
    } else {
        item->setCursor(Qt::OpenHandCursor);
    }
}

SelectionEditor::MouseLocation SelectionEditorPrivate::mouseLocation(const QPointF &pos) const
{
    QRectF handleRect(-handleRadius, -handleRadius, handleRadius * 2, handleRadius * 2);
    if (G::ellipseContains(handleRect.translated(handlePositions[0]), pos)) {
        return MouseLocation::TopLeft;
    }
    if (G::ellipseContains(handleRect.translated(handlePositions[1]), pos)) {
        return MouseLocation::TopRight;
    }
    if (G::ellipseContains(handleRect.translated(handlePositions[2]), pos)) {
        return MouseLocation::BottomRight;
    }
    if (G::ellipseContains(handleRect.translated(handlePositions[3]), pos)) {
        return MouseLocation::BottomLeft;
    }
    if (G::ellipseContains(handleRect.translated(handlePositions[4]), pos)) {
        return MouseLocation::Top;
    }
    if (G::ellipseContains(handleRect.translated(handlePositions[5]), pos)) {
        return MouseLocation::Right;
    }
    if (G::ellipseContains(handleRect.translated(handlePositions[6]), pos)) {
        return MouseLocation::Bottom;
    }
    if (G::ellipseContains(handleRect.translated(handlePositions[7]), pos)) {
        return MouseLocation::Left;
    }

    const auto rect = selection->normalized();
    // Rectangle can be resized when border is dragged, if it's big enough
    if (rect.width() >= 100 && rect.height() >= 100) {
        if (rect.adjusted(0, 0, 0, -rect.height() + s_borderDragAreaSize).contains(pos)) {
            return MouseLocation::Top;
        }
        if (rect.adjusted(0, rect.height() - s_borderDragAreaSize, 0, 0).contains(pos)) {
            return MouseLocation::Bottom;
        }
        if (rect.adjusted(0, 0, -rect.width() + s_borderDragAreaSize, 0).contains(pos)) {
            return MouseLocation::Left;
        }
        if (rect.adjusted(rect.width() - s_borderDragAreaSize, 0, 0, 0).contains(pos)) {
            return MouseLocation::Right;
        }
    }
    if (rect.contains(pos)) {
        return MouseLocation::Inside;
    }
    return MouseLocation::Outside;
}

// SelectionEditor =================================

SelectionEditor::SelectionEditor(QObject *parent)
    : QObject(parent)
    , d(new SelectionEditorPrivate(this))
{
    setObjectName(QStringLiteral("selectionEditor"));

    d->updateDevicePixelRatio();

    connect(d->selection.get(), &Selection::rectChanged, this, [this](){
        d->updateHandlePositions();
    });
}

SelectionEditor *SelectionEditor::instance()
{
    return &privateSelectionEditorSelf()->self;
}

Selection *SelectionEditor::selection() const
{
    return d->selection.get();
}

qreal SelectionEditor::devicePixelRatio() const
{
    return d->devicePixelRatio;
}

QRectF SelectionEditor::screensRect() const
{
    return d->screensRect;
}

int SelectionEditor::width() const
{
    return d->screensRect.width();
}

int SelectionEditor::height() const
{
    return d->screensRect.height();
}

MouseLocation SelectionEditor::dragLocation() const
{
    return d->dragLocation;
}

QRectF SelectionEditor::handlesRect() const
{
    return d->handlesRect;
}

bool SelectionEditor::magnifierAllowed() const
{
    return d->magnifierAllowed;
}

QPointF SelectionEditor::mousePosition() const
{
    return d->mousePos;
}

void SelectionEditor::setScreenImages(const QVector<CanvasImage> &screenImages)
{
    QVector<QPoint> translatedPoints;
    QRect screensRect;
    for (int i = 0; i < screenImages.length(); ++i) {
        const QPoint &screenPos = screenImages[i].rect.topLeft().toPoint();
        const QImage &image = screenImages[i].image;
        const qreal dpr = image.devicePixelRatio();

        QRect virtualScreenRect;
        if (KWindowSystem::isPlatformX11()) {
            virtualScreenRect = QRect(screenPos, image.size());
        } else {
            // `QSize / qreal` divides the int width and int height by the qreal factor,
            // then rounds the results to the nearest int.
            virtualScreenRect = QRect(screenPos, image.size() / dpr);
        }
        screensRect = screensRect.united(virtualScreenRect);

        translatedPoints.append(screenPos);
    }

    d->screenImages = screenImages;

    // compute coordinates after scaling
    for (int i = 0; i < screenImages.length(); ++i) {
        const QImage &image = screenImages[i].image;
        const QPoint &p = screenImages[i].rect.topLeft().toPoint();
        const QSize &size = image.size();
        const double dpr = image.devicePixelRatio();
        if (!qFuzzyCompare(dpr, 1.0)) {
            // must update all coordinates of next rects
            int newWidth = size.width();
            int newHeight = size.height();

            int deltaX = newWidth - (size.width());
            int deltaY = newHeight - (size.height());

            // for the next size
            for (int i2 = i; i2 < screenImages.length(); ++i2) {
                auto point = screenImages[i2].rect.topLeft();

                if (point.x() >= newWidth + p.x() - deltaX) {
                    translatedPoints[i2].setX(translatedPoints[i2].x() + deltaX);
                }
                if (point.y() >= newHeight + p.y() - deltaY) {
                    translatedPoints[i2].setY(translatedPoints[i2].y() + deltaY);
                }
            }
        }
    }

    d->image = QImage(screensRect.size(), QImage::Format_ARGB32);
    d->image.fill(Qt::black);
    QPainter painter(&d->image);
    // Don't enable SmoothPixmapTransform, we want crisp graphics.
    for (int i = 0; i < screenImages.length(); ++i) {
        // Geometry can have negative coordinates,
        // so it is necessary to subtract the upper left point,
        // because coordinates on the widget are counted from 0.
        QImage image = screenImages[i].image;
        image.setDevicePixelRatio(1);
        painter.drawImage(translatedPoints[i] - screensRect.topLeft(), image);
    }

    if (d->screensRect != screensRect) {
        d->screensRect = screensRect;
        Q_EMIT screensRectChanged();
    }

    Q_EMIT screenImagesChanged();
}

QVector<CanvasImage> SelectionEditor::screenImages() const
{
    return d->screenImages;
}

bool SelectionEditor::acceptSelection(ExportManager::Actions actions)
{
    if (d->screenImages.isEmpty()) {
        return false;
    }

    auto selectionRect = d->selection->normalized();
    if (Settings::rememberLastRectangularRegion() == Settings::Always) {
        Settings::setCropRegion(selectionRect.toAlignedRect());
    }

    if (selectionRect.isEmpty()) {
        selectionRect = d->screensRect;
    }

    auto spectacleCore = SpectacleCore::instance();
    spectacleCore->annotationDocument()->cropCanvas(selectionRect);

    if (KWindowSystem::isPlatformX11()) {
        d->image.setDevicePixelRatio(qGuiApp->devicePixelRatio());
        auto imageCropRegion = G::rectClipped({selectionRect.topLeft() * d->devicePixelRatio, //
                                               selectionRect.size() * d->devicePixelRatio}, //
                                              {d->screensRect.topLeft() * d->devicePixelRatio, //
                                               d->screensRect.size() * d->devicePixelRatio}).toRect();
        if (imageCropRegion.size() != d->image.size()) {
            Q_EMIT spectacleCore->grabDone(d->image.copy(imageCropRegion), actions);
        } else {
            Q_EMIT spectacleCore->grabDone(d->image, actions);
        }
    } else { // Wayland case
        // QGuiApplication::devicePixelRatio() is calculated by getting the highest screen DPI
        qreal maxDpr = qGuiApp->devicePixelRatio();
        auto selectionRect = d->selection->normalized().toRect();
        QSize selectionSize = selectionRect.size();
        QImage output(selectionSize * maxDpr, QImage::Format_ARGB32);
        output.fill(Qt::black);
        QPainter painter(&output);
        // Don't enable SmoothPixmapTransform, we want crisp graphics

        for (auto it = d->screenImages.constBegin(); it != d->screenImages.constEnd(); ++it) {
            const QRect &screenRect = it->rect.toRect();

            if (selectionRect.intersects(screenRect)) {
                const QPoint pos = screenRect.topLeft();
                const qreal dpr = it->image.devicePixelRatio();

                QRect intersected = screenRect.intersected(selectionRect);

                // converts to screen size & position
                QRect pixelOnScreenIntersected;
                pixelOnScreenIntersected.moveTopLeft((intersected.topLeft() - pos) * dpr);
                pixelOnScreenIntersected.setWidth(intersected.width() * dpr);
                pixelOnScreenIntersected.setHeight(intersected.height() * dpr);

                QImage screenOutput = it->image.copy(pixelOnScreenIntersected);

                // FIXME: this doesn't seem correct
                if (intersected.size() == selectionSize) {
                    // short path when single screen
                    // keep native screen resolution
                    // we need to set the pixmap dpr to be able to properly align with annotations
                    screenOutput.setDevicePixelRatio(dpr);
                    Q_EMIT spectacleCore->grabDone(screenOutput, actions);
                    return true;
                }

                // upscale the image according to max screen dpr, to keep the image not distorted
                output.setDevicePixelRatio(maxDpr);
                intersected.moveTopLeft((intersected.topLeft() - selectionRect.topLeft()) * maxDpr);
                intersected.setSize(intersected.size() * maxDpr);
                painter.drawImage(intersected, screenOutput);
            }
        }

        Q_EMIT spectacleCore->grabDone(output, actions);
    }

    return true;
}

bool SelectionEditor::eventFilter(QObject *watched, QEvent *event)
{
    auto *item = qobject_cast<QQuickItem *>(watched);

    if (!item) {
        return false;
    }

    switch (event->type()) {
    case QEvent::KeyPress:
        keyPressEvent(item, static_cast<QKeyEvent *>(event));
        break;
    case QEvent::KeyRelease:
        keyReleaseEvent(item, static_cast<QKeyEvent *>(event));
        break;
    case QEvent::HoverMove:
        hoverMoveEvent(item, static_cast<QHoverEvent *>(event));
        break;
    case QEvent::MouseButtonPress:
        mousePressEvent(item, static_cast<QMouseEvent *>(event));
        break;
    case QEvent::MouseMove:
        mouseMoveEvent(item, static_cast<QMouseEvent *>(event));
        break;
    case QEvent::MouseButtonRelease:
        mouseReleaseEvent(item, static_cast<QMouseEvent *>(event));
        break;
    case QEvent::MouseButtonDblClick:
        mouseDoubleClickEvent(item, static_cast<QMouseEvent *>(event));
        break;
    default:
        break;
    }
    return false;
}

void SelectionEditor::keyPressEvent(QQuickItem *item, QKeyEvent *event)
{
    Q_UNUSED(item);

    const auto modifiers = event->modifiers();
    const bool shiftPressed = modifiers & Qt::ShiftModifier;
    if (shiftPressed) {
        d->toggleMagnifier = true;
    }
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        acceptSelection();
        event->accept();
        break;
    case Qt::Key_Up:
    case Qt::Key_Right:
    case Qt::Key_Down:
    case Qt::Key_Left:
        d->handleArrowKey(event);
        event->accept();
        break;
    default:
        break;
    }
}

void SelectionEditor::keyReleaseEvent(QQuickItem *item, QKeyEvent *event)
{
    Q_UNUSED(item);

    if (d->toggleMagnifier && !(event->modifiers() & Qt::ShiftModifier)) {
        d->toggleMagnifier = false;
    }
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        event->accept();
        break;
    case Qt::Key_Up:
    case Qt::Key_Right:
    case Qt::Key_Down:
    case Qt::Key_Left:
        event->accept();
        break;
    default:
        break;
    }
}

void SelectionEditor::hoverMoveEvent(QQuickItem *item, QHoverEvent *event)
{
    if (!item->window() || !item->window()->screen()) {
        return;
    }
    d->mousePos = item->mapToScene(event->posF()) + G::mapFromPlatformPoint(item->window()->position(), d->devicePixelRatio);
    Q_EMIT mousePositionChanged();
    d->setMouseCursor(item, d->mousePos);
}

void SelectionEditor::mousePressEvent(QQuickItem *item, QMouseEvent *event)
{
    if (!item->window() || !item->window()->screen()) {
        return;
    }

    if (event->source() == Qt::MouseEventNotSynthesized) {
        d->handleRadius = s_handleRadiusMouse;
    } else {
        d->handleRadius = s_handleRadiusTouch;
    }

    if (event->button() & (Qt::LeftButton | Qt::RightButton)) {
        if (event->button() & Qt::RightButton) {
            d->selection->setRect({});
        }
        item->setFocus(true);
        const bool wasMagnifierAllowed = d->magnifierAllowed;
        d->mousePos = event->windowPos() + G::mapFromPlatformPoint(item->window()->position(), d->devicePixelRatio);
        Q_EMIT mousePositionChanged();
        auto newDragLocation = d->mouseLocation(d->mousePos);
        if (d->dragLocation != newDragLocation) {
            d->dragLocation = newDragLocation;
            Q_EMIT dragLocationChanged();
        }
        d->magnifierAllowed = true;
        d->disableArrowKeys = true;

        switch (d->dragLocation) {
        case MouseLocation::Outside:
            d->startPos = d->mousePos;
            break;
        case MouseLocation::Inside:
            d->startPos = d->mousePos;
            d->magnifierAllowed = false;
            d->initialTopLeft = d->selection->rectF().topLeft();
            item->setCursor(Qt::ClosedHandCursor);
            break;
        case MouseLocation::Top:
        case MouseLocation::Left:
        case MouseLocation::TopLeft:
            d->startPos = d->selection->rectF().bottomRight();
            break;
        case MouseLocation::Bottom:
        case MouseLocation::Right:
        case MouseLocation::BottomRight:
            d->startPos = d->selection->rectF().topLeft();
            break;
        case MouseLocation::TopRight:
            d->startPos = d->selection->rectF().bottomLeft();
            break;
        case MouseLocation::BottomLeft:
            d->startPos = d->selection->rectF().topRight();
            break;
        default:
            break;
        }

        if (d->magnifierAllowed != wasMagnifierAllowed) {
            Q_EMIT magnifierAllowedChanged();
        }
    }
    event->accept();
}

void SelectionEditor::mouseMoveEvent(QQuickItem *item, QMouseEvent *event)
{
    if (!item->window() || !item->window()->screen()) {
        return;
    }

    d->mousePos = event->windowPos() + G::mapFromPlatformPoint(item->window()->position(), d->devicePixelRatio);
    Q_EMIT mousePositionChanged();
    const bool wasMagnifierAllowed = d->magnifierAllowed;
    d->magnifierAllowed = true;
    switch (d->dragLocation) {
    case MouseLocation::None: {
        d->setMouseCursor(item, d->mousePos);
        d->magnifierAllowed = false;
        break;
    }
    case MouseLocation::TopLeft:
    case MouseLocation::TopRight:
    case MouseLocation::BottomRight:
    case MouseLocation::BottomLeft: {
        const bool afterX = d->mousePos.x() >= d->startPos.x();
        const bool afterY = d->mousePos.y() >= d->startPos.y();
        d->selection->setRect(afterX ? d->startPos.x() : d->mousePos.x(),
                              afterY ? d->startPos.y() : d->mousePos.y(),
                              qAbs(d->mousePos.x() - d->startPos.x()) + (afterX ? d->devicePixel : 0),
                              qAbs(d->mousePos.y() - d->startPos.y()) + (afterY ? d->devicePixel : 0));
        break;
    }
    case MouseLocation::Outside: {
        d->selection->setRect(qMin(d->mousePos.x(), d->startPos.x()),
                              qMin(d->mousePos.y(), d->startPos.y()),
                              qAbs(d->mousePos.x() - d->startPos.x()) + d->devicePixel,
                              qAbs(d->mousePos.y() - d->startPos.y()) + d->devicePixel);
        break;
    }
    case MouseLocation::Top:
    case MouseLocation::Bottom: {
        const bool afterY = d->mousePos.y() >= d->startPos.y();
        d->selection->setRect(d->selection->x(),
                              afterY ? d->startPos.y() : d->mousePos.y(),
                              d->selection->width(),
                              qAbs(d->mousePos.y() - d->startPos.y()) + (afterY ? d->devicePixel : 0));
        break;
    }
    case MouseLocation::Right:
    case MouseLocation::Left: {
        const bool afterX = d->mousePos.x() >= d->startPos.x();
        d->selection->setRect(afterX ? d->startPos.x() : d->mousePos.x(),
                              d->selection->y(),
                              qAbs(d->mousePos.x() - d->startPos.x()) + (afterX ? d->devicePixel : 0),
                              d->selection->height());
        break;
    }
    case MouseLocation::Inside: {
        d->magnifierAllowed = false;
        // We use some math here to figure out if the diff with which we
        // move the rectangle with moves it out of bounds,
        // in which case we adjust the diff to not let that happen

        QRectF newRect((d->mousePos - d->startPos + d->initialTopLeft) * d->devicePixelRatio,
                       d->selection->sizeF() * d->devicePixelRatio);

        const QRectF translatedScreensRect = d->screensRect.translated(-d->screensRect.topLeft());
        if (!translatedScreensRect.contains(newRect)) {
            // Keep the item inside the scene screen region bounding rect.
            newRect.moveTo(qMin(translatedScreensRect.right() - newRect.width(), qMax(newRect.x(), translatedScreensRect.left())) * d->devicePixel,
                           qMin(translatedScreensRect.bottom() - newRect.height(), qMax(newRect.y(), translatedScreensRect.top())) * d->devicePixel);
        }

        d->selection->setRect(G::rectBounded(newRect, d->screensRect));
        break;
    }
    default:
        break;
    }
    if (d->magnifierAllowed != wasMagnifierAllowed) {
        Q_EMIT magnifierAllowedChanged();
    }

    event->accept();
}

void SelectionEditor::mouseReleaseEvent(QQuickItem *item, QMouseEvent *event)
{
    switch (event->button()) {
    case Qt::LeftButton:
    case Qt::RightButton:
        if (d->dragLocation == MouseLocation::Outside && Settings::useReleaseToCapture()) {
            acceptSelection();
            return;
        }
        d->disableArrowKeys = false;
        if (d->dragLocation == MouseLocation::Inside) {
            item->setCursor(Qt::OpenHandCursor);
        }
        break;
    default:
        break;
    }
    event->accept();
    if (d->dragLocation != MouseLocation::None) {
        d->dragLocation = MouseLocation::None;
        Q_EMIT dragLocationChanged();
    }
    if (d->magnifierAllowed) {
        d->magnifierAllowed = false;
        Q_EMIT magnifierAllowedChanged();
    }
}

void SelectionEditor::mouseDoubleClickEvent(QQuickItem *item, QMouseEvent *event)
{
    Q_UNUSED(item)
    if (event->button() == Qt::LeftButton && d->selection->contains(d->mousePos)) {
        acceptSelection();
    }
    event->accept();
}

#include <moc_SelectionEditor.cpp>
