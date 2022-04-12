#include "SpectacleMenu.h"

SpectacleMenu::SpectacleMenu(const QString &title, QWidget *parent)
    : QMenu(title, parent)
{}

SpectacleMenu::SpectacleMenu(QWidget *parent)
    : QMenu(parent)
{}

void SpectacleMenu::setVisible(bool visible)
{
    bool oldVisible = isVisible();
    if (oldVisible == visible) {
        return;
    }
    QMenu::setVisible(visible);
    Q_EMIT visibleChanged();
}

void SpectacleMenu::popup(const QPointF &globalPos)
{
    QMenu::popup(globalPos.toPoint());
}
