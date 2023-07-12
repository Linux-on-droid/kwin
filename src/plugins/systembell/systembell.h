/*
    SPDX-FileCopyrightText: 2023 David Redondo <kde@david-redondo.de>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "plugin.h"
#include "x11eventfilter.h"

#include <KConfigWatcher>

#include <QColor>

struct xcb_xkb_bell_notify_event_t;
namespace KWaylandServer
{
class ClientConnection;
class SurfaceInterface;
}
namespace KWin
{
class BellFilter;
class EffectWindow;

class SystemBell : public KWin::Plugin
{
    Q_OBJECT
public:
    SystemBell();
    ~SystemBell() override;
    void xkbRing(xcb_xkb_bell_notify_event_t *event);

private:
    enum class BellMode {
        SystemBell = 0x1,
        CustomSound = 0x2,
        Invert = 0x4,

        Color = 0x8,
    };
    void loadConfig(const KConfigGroup &group);
    void ringSurface(KWaylandServer::SurfaceInterface *surface);
    void ringClient(KWaylandServer::ClientConnection *client);
    void audibleBell();
    void visualBell(EffectWindow *window);

    std::unique_ptr<BellFilter> m_bellFilter;
    KConfigWatcher::Ptr m_configWatcher;
    QFlags<BellMode> m_bellModes;
    QString m_customSound;
    QColor m_color;
    int m_duration;
};
}
