/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "windowgeometry.h"

namespace KWin
{

KWIN_EFFECT_FACTORY(WindowGeometryFactory,
                    WindowGeometry,
                    "metadata.json")

} // namespace KWin

#include "main.moc"
