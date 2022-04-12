#include "CaptureModeModel.h"

#include <KLocalizedString>

#include <QApplication>
#include <qnamespace.h>

CaptureModeModel::CaptureModeModel(Platform::GrabModes grabModes, QObject *parent)
    : QAbstractListModel(parent)
{
    m_roleNames[CaptureModeRole] = QByteArrayLiteral("captureMode");
    m_roleNames[Qt::DisplayRole] = QByteArrayLiteral("display");
    setGrabModes(grabModes);
}

QHash<int, QByteArray> CaptureModeModel::roleNames() const
{
    return m_roleNames;
}

QVariant CaptureModeModel::data(const QModelIndex &index, int role) const
{
    int row = index.row();
    QVariant ret;
    if (!index.isValid() || row >= m_data.size()) {
        return ret;
    }
    if (role == CaptureModeRole) {
        ret = m_data.at(row).captureMode;
    } else if (role == Qt::DisplayRole) {
        ret = m_data.at(row).label;
    }
    return ret;
}

int CaptureModeModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return m_data.size();
}

void CaptureModeModel::setGrabModes(Platform::GrabModes modes)
{
    if(m_grabModes == modes) {
        return;
    }
    m_grabModes = modes;
    m_data.clear();
    int screenCount = QApplication::screens().count();
    if (m_grabModes.testFlag(Platform::GrabMode::AllScreens)) {
        m_data.append({
            CaptureModeModel::AllScreens,
            screenCount > 1 ? i18n("All Screens") : i18n("Full Screen")
        });
    }
    if (m_grabModes.testFlag(Platform::GrabMode::AllScreensScaled) && screenCount > 1) {
        m_data.append({
            CaptureModeModel::AllScreensScaled,
            i18n("All Screens (Scaled to same size)")
        });
    }
    if (m_grabModes.testFlag(Platform::GrabMode::PerScreenImageNative)) {
        m_data.append({
            CaptureModeModel::RectangularRegion,
            i18n("Rectangular Region")
        });
    }
    if (m_grabModes.testFlag(Platform::GrabMode::CurrentScreen) && screenCount > 1) {
        m_data.append({
            CaptureModeModel::CurrentScreen,
            i18n("Current Screen")
        });
    }
    if (m_grabModes.testFlag(Platform::GrabMode::ActiveWindow)) {
        m_data.append({
            CaptureModeModel::ActiveWindow,
            i18n("Active Window")
        });
    }
    if (m_grabModes.testFlag(Platform::GrabMode::WindowUnderCursor)) {
        m_data.append({
            CaptureModeModel::WindowUnderCursor,
            i18n("Window Under Cursor")
        });
    }
    Q_EMIT countChanged();
}
