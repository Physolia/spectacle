#pragma once

#include <QMenu>

/**
 * This class only exists to make QMenu more usable with Qt Quick
 */
class SpectacleMenu : public QMenu
{
    Q_OBJECT
    Q_PROPERTY(bool visible READ isVisible WRITE setVisible NOTIFY visibleChanged FINAL)
public:
    SpectacleMenu(const QString &title, QWidget *parent = nullptr);
    SpectacleMenu(QWidget *parent = nullptr);

    /**
     * Same as QMenu::setVisible, but it emits visibleChanged so it can be useful in QML bindings
     */
    void setVisible(bool visible) override;

    /**
     * Same as QMenu::popup(), but invokable. Takes global coordinates.
     */
    Q_INVOKABLE void popup(const QPointF &globalPos);

Q_SIGNALS:
    void visibleChanged();
};
