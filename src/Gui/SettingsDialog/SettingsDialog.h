/*
 *  SPDX-FileCopyrightText: 2015 Boudhayan Gupta <bgupta@kde.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <KConfigDialog>

class ShortcutsOptionsPage;

class SettingsDialog : public KConfigDialog
{
    Q_OBJECT

    public:

    explicit SettingsDialog(QWidget *parent = nullptr);

    private:

    bool hasChanged() override;
    bool isDefault() override;
    void updateSettings() override;
    void updateWidgets() override;
    void updateWidgetsDefault() override;

    ShortcutsOptionsPage* mShortcutsPage;
};

#endif // SETTINGSDIALOG_H
