/*
    SPDX-FileCopyrightText: 2023 David Redondo <kde@david-redondo.de>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "kwin_export.h"

#include <QObject>

#include <memory>

namespace KWaylandServer
{
class ClientConnection;
class Display;
class SurfaceInterface;
class SystemBellV1InterfacePrivate;
class KWIN_EXPORT SystemBellV1Interface : public QObject
{
    Q_OBJECT
public:
    SystemBellV1Interface(Display *display, QObject *parent = nullptr);
    ~SystemBellV1Interface();
Q_SIGNALS:
    void ring(ClientConnection *client);
    void ringSurface(SurfaceInterface *surface);

private:
    std::unique_ptr<SystemBellV1InterfacePrivate> d;
};
}
