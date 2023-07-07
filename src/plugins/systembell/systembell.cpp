/*
    SPDX-FileCopyrightText: 2023 David Redondo <kde@david-redondo.de>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "systembell.h"

#include "effects.h"
#include "wayland/surface_interface.h"
#include "wayland/systembell_v1_interface.h"
#include "wayland_server.h"

#include <KNotification>

using namespace Qt::StringLiterals;

namespace KWin
{
SystemBell::SystemBell()
    : m_configWatcher(KConfigWatcher::create(KSharedConfig::openConfig(u"kaccessrc"_qs)))
{
    if (waylandServer()) {
        auto systemBell = new KWaylandServer::SystemBellV1Interface(waylandServer()->display(), this);
        connect(systemBell, &KWaylandServer::SystemBellV1Interface::ringSurface, this, &SystemBell::ringSurface);
        connect(systemBell, &KWaylandServer::SystemBellV1Interface::ring, this, &SystemBell::ringClient);
    }
    connect(m_configWatcher.get(), &KConfigWatcher::configChanged, this, [this](const KConfigGroup &group) {
        if (group.name() == "Bell"_L1) {
            loadConfig(group);
        }
    });
    loadConfig(m_configWatcher->config()->group("Bell"));
}

void SystemBell::audibleBell()
{
    if (m_bellModes & BellMode::SystemBell) {
        KNotification::beep();
    }
    if (m_bellModes & BellMode::CustomSound) {
        // TODO play the sound
    }
}

void SystemBell::visualBell(EffectWindow *window)
{
    if (m_bellModes & BellMode::Invert) {
        if (auto effect = static_cast<EffectsHandlerImpl *>(effects)->provides(Effect::ScreenInversion)) {
            effect->perform(Effect::ScreenInversion, {QVariant::fromValue(window)});
            QTimer::singleShot(m_duration, window, [window, effect] {
                effect->perform(Effect::ScreenInversion, {QVariant::fromValue(window)});
            });
        }
    } else if (m_bellModes & BellMode::Color) {
        // TODO effect that makes it a color
    }
}

void SystemBell::ringSurface(KWaylandServer::SurfaceInterface *surface)
{
    audibleBell();
    visualBell(effects->findWindow(surface));
}

void SystemBell::ringClient(KWaylandServer::ClientConnection *client)
{
    audibleBell();
    const auto windows = KWin::waylandServer()->windows();
    for (const auto window : windows) {
        if (window->surface()->client() == client) {
            visualBell(window->effectWindow());
        }
    }
}

void SystemBell::loadConfig(const KConfigGroup &group)
{
    m_bellModes = {};
    m_customSound = QString();
    m_color = QColor();

    m_bellModes.setFlag(BellMode::SystemBell, group.readEntry("SystemBell", true));

    if (group.readEntry("ArtsBell", false)) {
        m_bellModes |= BellMode::CustomSound;
        m_customSound = group.readEntry("ArtsBellFile");
    }

    if (group.readEntry("VisibleBell", false)) {
        if (group.readEntry("VisibleBellInvert", false)) {
            m_bellModes |= BellMode::Invert;
        } else {
            m_bellModes |= BellMode::Color;
            m_color = group.readEntry("VisibleBellColor", QColor(Qt::red));
        }
        m_duration = group.readEntry("VisibleBellPause", 500);
    }
}

}
#include "moc_systembell.cpp"
