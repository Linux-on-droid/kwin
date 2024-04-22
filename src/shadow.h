/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2011 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "kwin_export.h"

#include <QImage>
#include <QObject>
#include <QRectF>

#include <xcb/xcb.h>

namespace KDecoration2
{
class Decoration;
class DecorationShadow;
}

namespace KWin
{

class ShadowInterface;
class Window;

/**
 * @short Class representing a Window's Shadow to be rendered by the Compositor.
 *
 * This class holds all information about the Shadow to be rendered together with the
 * window during the Compositing stage. The Shadow consists of several pixmaps and offsets.
 * For a complete description please refer to https://community.kde.org/KWin/Shadow
 *
 * To create a Shadow instance use the static factory method createShadow which will
 * create an instance for the currently used Compositing Backend. It will read the X11 Property
 * and create the Shadow and all required data (such as WindowQuads). If there is no Shadow
 * defined for the Window the factory method returns @c NULL.
 *
 * @author Martin Gräßlin <mgraesslin@kde.org>
 * @todo React on Window size changes.
 */
class KWIN_EXPORT Shadow : public QObject
{
    Q_OBJECT
public:
    explicit Shadow(Window *window);
    ~Shadow() override;

    /**
     * This method updates the Shadow when the property has been changed.
     * It is the responsibility of the owner of the Shadow to call this method
     * whenever the owner receives a PropertyNotify event.
     * This method will invoke a re-read of the Property. In case the Property has
     * been withdrawn the method returns @c false. In that case the owner should
     * delete the Shadow.
     * @returns @c true when the shadow has been updated, @c false if the property is not set anymore.
     */
    bool updateShadow();

    /**
     * Factory Method to create the shadow from the property.
     * This method takes care of creating an instance of the
     * Shadow class for the current Compositing Backend.
     *
     * If there is no shadow defined for @p window this method
     * will return @c NULL.
     * @param window The Window for which the shadow should be created
     * @return Created Shadow or @c NULL in case there is no shadow defined.
     */
    static std::unique_ptr<Shadow> createShadow(Window *window);

    Window *window() const;

    bool hasDecorationShadow() const
    {
        return m_decorationShadow != nullptr;
    }
    QImage decorationShadowImage() const;

    std::weak_ptr<KDecoration2::DecorationShadow> decorationShadow() const
    {
        return m_decorationShadow;
    }

    enum ShadowElements {
        ShadowElementTop,
        ShadowElementTopRight,
        ShadowElementRight,
        ShadowElementBottomRight,
        ShadowElementBottom,
        ShadowElementBottomLeft,
        ShadowElementLeft,
        ShadowElementTopLeft,
        ShadowElementsCount
    };
    QSizeF elementSize(ShadowElements element) const;

    QRectF rect() const
    {
        return QRectF(QPoint(0, 0), m_cachedSize);
    }
    QMarginsF offset() const
    {
        return m_offset;
    }
    inline const QImage &shadowElement(ShadowElements element) const
    {
        return m_shadowElements[element];
    }

Q_SIGNALS:
    void offsetChanged();
    void rectChanged();
    void textureChanged();

public Q_SLOTS:
    void geometryChanged();

private:
    static std::unique_ptr<Shadow> createShadowFromX11(Window *window);
    static std::unique_ptr<Shadow> createShadowFromDecoration(Window *window);
    static std::unique_ptr<Shadow> createShadowFromWayland(Window *window);
    static std::unique_ptr<Shadow> createShadowFromInternalWindow(Window *window);
    static QList<uint32_t> readX11ShadowProperty(xcb_window_t id);
    bool init(const QList<uint32_t> &data);
    bool init(KDecoration2::Decoration *decoration);
    bool init(const QPointer<ShadowInterface> &shadow);
    bool init(const QWindow *window);
    Window *m_window;
    // shadow elements
    QImage m_shadowElements[ShadowElementsCount];
    // shadow offsets
    QMarginsF m_offset;
    // caches
    QSizeF m_cachedSize;
    // Decoration based shadows
    std::shared_ptr<KDecoration2::DecorationShadow> m_decorationShadow;
};

}
