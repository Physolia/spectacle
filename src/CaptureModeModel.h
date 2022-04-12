#pragma once

#include "Platforms/Platform.h"

#include <QAbstractListModel>
#include <qqml.h>

class CaptureModeModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)
public:
    CaptureModeModel(Platform::GrabModes grabModes, QObject *parent = nullptr);

    enum CaptureMode {
        AllScreens = 0,
        CurrentScreen = 1,
        ActiveWindow = 2,
        WindowUnderCursor = 3,
        TransientWithParent = 4,
        RectangularRegion = 5,
        AllScreensScaled = 6 // FIXME/TODO what does this do?
    };
    Q_ENUM(CaptureMode)

    enum {
        CaptureModeRole = Qt::UserRole + 1
    };

    QHash<int, QByteArray> roleNames() const override;
    QVariant data(const QModelIndex &index, int role) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    void setGrabModes(Platform::GrabModes modes);

Q_SIGNALS:
    void countChanged();

private:
    struct Item {
        CaptureModeModel::CaptureMode captureMode;
        QString label;
    };

    QList<Item> m_data;
    QHash<int, QByteArray> m_roleNames;
    Platform::GrabModes m_grabModes;
};
