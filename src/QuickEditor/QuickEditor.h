/*
 *  SPDX-FileCopyrightText: 2018 Ambareesh "Amby" Balaji <ambareeshbalaji@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef QUICKEDITOR_H
#define QUICKEDITOR_H

#include <QMap>
#include <QStaticText>
#include <QWidget>

class ComparableQPoint;
class QKeyEvent;
class QMouseEvent;
class QPainter;

namespace KWayland::Client
{
class PlasmaShell;
}

class QuickEditor : public QWidget
{
    Q_OBJECT

public:
    QuickEditor(const QMap<const QScreen *, QImage> &images, KWayland::Client::PlasmaShell *plasmashell, QWidget *parent = nullptr);

Q_SIGNALS:
    void grabDone(const QPixmap &thePixmap);
    void grabCancelled();

private:
    enum MouseState : short {
        None =          0b000000,
        Inside =        0b000001,
        Outside =       0b000010,
        TopLeft =       0b000101,
        Top =           0b010001,
        TopRight =      0b001001,
        Right =         0b100001,
        BottomRight =   0b000110,
        Bottom =        0b010010,
        BottomLeft =    0b001010,
        Left =          0b100010,
        TopLeftOrBottomRight = TopLeft & BottomRight, // 100
        TopRightOrBottomLeft = TopRight & BottomLeft, // 1000
        TopOrBottom = Top & Bottom, // 10000
        RightOrLeft = Right & Left, // 100000
    };

    void acceptSelection();
    int boundsLeft(int newTopLeftX, const bool mouse = true);
    int boundsRight(int newTopLeftX, const bool mouse = true);
    int boundsUp(int newTopLeftY, const bool mouse = true);
    int boundsDown(int newTopLeftY, const bool mouse = true);
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *) override;
    void drawBottomHelpText(QPainter &painter);
    void drawDragHandles(QPainter &painter);
    void drawMagnifier(QPainter &painter);
    void drawMidHelpText(QPainter &painter);
    void preparePaint();
    void createPixmapFromScreens();
    void setGeometryToScreenPixmap(KWayland::Client::PlasmaShell *plasmashell);
    void drawSelectionSizeTooltip(QPainter &painter, bool dragHandlesVisible);
    void setBottomHelpText();
    void layoutBottomHelpText();
    void setMouseCursor(const QPointF &pos);
    MouseState mouseLocation(const QPointF &pos);

    static QMap<ComparableQPoint, ComparableQPoint> computeCoordinatesAfterScaling(const QMap<ComparableQPoint, QPair<qreal, QSize>> &outputsRect);

    static const int handleRadiusMouse;
    static const int handleRadiusTouch;
    static const qreal increaseDragAreaFactor;
    static const int minSpacingBetweenHandles;
    static const int borderDragAreaSize;

    static const int selectionSizeThreshold;

    static const int selectionBoxPaddingX;
    static const int selectionBoxPaddingY;
    static const int selectionBoxMarginY;

    static const int bottomHelpMaxLength = 6;
    static bool bottomHelpTextPrepared;
    static const int bottomHelpBoxPaddingX;
    static const int bottomHelpBoxPaddingY;
    static const int bottomHelpBoxPairSpacing;
    static const int bottomHelpBoxMarginBottom;
    static const int midHelpTextFontSize;

    static const int magnifierLargeStep;

    static const int magZoom;
    static const int magPixels;
    static const int magOffset;

    QColor mMaskColor;
    QColor mStrokeColor;
    QColor mCrossColor;
    QColor mLabelBackgroundColor;
    QColor mLabelForegroundColor;
    QRect mSelection;
    QPointF mStartPos;
    QPointF mInitialTopLeft;
    QString mMidHelpText;
    QFont mMidHelpTextFont;
    std::pair<QStaticText, std::vector<QStaticText>> mBottomHelpText[bottomHelpMaxLength];
    QFont mBottomHelpTextFont;
    QRect mBottomHelpBorderBox;
    QPoint mBottomHelpContentPos;
    int mBottomHelpGridLeftWidth;
    MouseState mMouseDragState;
    QMap<const QScreen *, QImage> mImages;
    QMap<const QScreen *, qreal> mScreenToDpr;
    QPixmap mPixmap;
    qreal devicePixelRatio;
    qreal devicePixelRatioI;
    QPointF mMousePos;
    bool mMagnifierAllowed;
    bool mShowMagnifier;
    bool mToggleMagnifier;
    bool mReleaseToCapture;
    bool mDisableArrowKeys;
    int mbottomHelpLength;
    QRect mScreensRect;

    // Midpoints of handles
    QVector<QPointF> mHandlePositions = QVector<QPointF>{8};
    // Radius of handles is either handleRadiusMouse or handleRadiusTouch
    int mHandleRadius;
};

#endif // QUICKEDITOR_H
