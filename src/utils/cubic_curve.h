/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2005 C. Boemann <cbo@boemann.dk>
    SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
    SPDX-FileCopyrightText: 2010 Cyrille Berger <cberger@cberger.net>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "kwin_export.h"

#include <QList>
#include <QPointF>
#include <QVariant>

namespace KWin
{

/**
 * Hold the data for a cubic curve.
 */
class KWIN_EXPORT CubicCurve : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QList<QPointF> points READ points CONSTANT)

public:
    /**
     * Creates a curve with two default points, one at (0,0) and another at (1,1)
     */
    CubicCurve();

    /**
     * Creates a curve from @p points
     */
    explicit CubicCurve(const QList<QPointF> &points);

    /**
     * Creates a curve that's deserialized from @p curveString.
     * This serialized string can be created by CubicCurve::toString()
     */
    explicit CubicCurve(const QString &curveString);

    /**
     * Copies an existing curve from @p curve.
     */
    CubicCurve(const CubicCurve &curve);

    ~CubicCurve() override;

    CubicCurve &operator=(const CubicCurve &curve);
    bool operator==(const CubicCurve &curve) const;

    /**
     * Calculates the value of the curve at point @p x.
     */
    Q_SCRIPTABLE qreal value(qreal x) const;

    /**
     * Returns all of the points of the curve.
     */
    Q_SCRIPTABLE const QList<QPointF> &points() const;

    /**
     * Replaces the curve's points with @p points.
     */
    Q_SCRIPTABLE void setPoints(const QList<QPointF> &points);

    /**
     * Replaces point at @p idx with @p point.
     */
    Q_SCRIPTABLE void setPoint(int idx, const QPointF &point);

    /**
     * Add a point to the curve, the list of point is always sorted.
     * @return the index of the inserted point
     */
    Q_SCRIPTABLE int addPoint(const QPointF &point);

    /**
     * Removes point @p idx from the curve.
     */
    Q_SCRIPTABLE void removePoint(int idx);

    /**
     * Serializes the points of the curve to a string.
     */
    Q_SCRIPTABLE QString toString() const;

    Q_SCRIPTABLE void fromString(const QString &string);

private:
    struct Data;
    struct Private;
    Private *const d{nullptr};
};

} // namespace KWin