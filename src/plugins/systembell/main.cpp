/*
    SPDX-FileCopyrightText: 2023 David Redondo <kde@david-redono.de>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include <main.h>
#include <plugin.h>

#include "systembell.h"

namespace KWin
{
class KWIN_EXPORT SystemBellFactory : public PluginFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PluginFactory_iid FILE "metadata.json")
    Q_INTERFACES(KWin::PluginFactory)

public:
    std::unique_ptr<KWin::Plugin> create() const override
    {
        return std::make_unique<SystemBell>();
    }
};
}

#include "main.moc"
