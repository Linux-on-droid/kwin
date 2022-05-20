/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2021 David Edmundson <davidedmundson@kde.org>
    SPDX-FileCopyrightText: 2021 David Redondo <kde@david-redondo.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef XWLDROPHANDLER_H
#define XWLDROPHANDLER_H

#include "wayland/datadevice_interface.h"

#include <xcb/xcb.h>

namespace KWin
{
namespace Xwl
{

class Xvisit;

class XwlDropHandler : public KWaylandServer::AbstractDropHandler
{
    Q_OBJECT
public:
    XwlDropHandler();

    void updateDragTarget(KWaylandServer::SurfaceInterface *surface, quint32 serial) override;

private:
    void drop() override;
    Xvisit *m_xvisit = nullptr;
};
}
}
#endif // XWLDROPHANDLER_H
