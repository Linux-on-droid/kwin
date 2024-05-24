/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "touch.h"

#include "qwayland-server-wayland.h"

#include <QPointer>

namespace KWin
{
class ClientConnection;

class TouchInterfacePrivate : public QtWaylandServer::wl_touch
{
public:
    static TouchInterfacePrivate *get(TouchInterface *touch);
    TouchInterfacePrivate(TouchInterface *q, SeatInterface *seat);

    QList<Resource *> touchesForClient(ClientConnection *client) const;
    bool hasTouchesForClient(ClientConnection *client) const;

    TouchInterface *q;
    SeatInterface *seat;

protected:
    void touch_release(Resource *resource) override;
};

} // namespace KWin
