/*
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#pragma once

#include "kwin_export.h"

#include <QObject>
#include <QPointF>
#include <QSizeF>

struct wl_resource;

namespace KWaylandServer
{
class Display;
class FakeInputDevice;
class FakeInputDevicePrivate;
class FakeInputInterfacePrivate;

/**
 * @brief Represents the Global for org_kde_kwin_fake_input interface.
 *
 * The fake input interface allows clients to send fake input events to the
 * Wayland server. For the actual events it creates a FakeInputDevice. Whenever
 * the FakeInputInterface creates a device the signal deviceCreated gets emitted.
 *
 * Accepting fake input events is a security risk. The server should make a
 * dedicated decision about whether it wants to accept fake input events from a
 * device. Because of that by default no events are forwarded to the server. The
 * device needs to request authentication and the server must explicitly authenticate
 * the device. The recommendation is that the server only accepts input for in some
 * way trusted clients.
 *
 * @see FakeInputDevice
 */
class KWIN_EXPORT FakeInputInterface : public QObject
{
    Q_OBJECT

public:
    explicit FakeInputInterface(Display *display, QObject *parent = nullptr);
    ~FakeInputInterface() override;

Q_SIGNALS:
    /**
     * Signal emitted whenever a client bound the fake input @p device.
     * @param device The created FakeInputDevice
     */
    void deviceCreated(KWaylandServer::FakeInputDevice *device);

private:
    QScopedPointer<FakeInputInterfacePrivate> d;
};

/**
 * @brief Represents the Resource for a org_kde_kwin_fake_input interface.
 *
 * @see FakeInputInterface
 */
class KWIN_EXPORT FakeInputDevice : public QObject
{
    Q_OBJECT
public:
    ~FakeInputDevice() override;
    /**
     * @returns the native wl_resource.
     */
    wl_resource *resource();

    /**
     * Authenticate this device to send events. If @p authenticated is @c true events are
     * accepted, for @c false events are no longer accepted.
     *
     * @param authenticated Whether the FakeInputDevice should be considered authenticated
     */
    void setAuthentication(bool authenticated);
    /**
     * @returns whether the FakeInputDevice is authenticated and allowed to send events, default is @c false.
     */
    bool isAuthenticated() const;

Q_SIGNALS:
    /**
     * Request for authentication.
     *
     * The server might use the provided information to make a decision on whether the
     * FakeInputDevice should get authenticated. It is recommended to not trust the data
     * and to combine it with information from ClientConnection.
     *
     * @param application A textual description of the application
     * @param reason A textual description of the reason why the application wants to send fake input events
     */
    void authenticationRequested(const QString &application, const QString &reason);
    /**
     * Request a pointer motion by @p delta.
     */
    void pointerMotionRequested(const QSizeF &delta);
    /**
     * Request an absolute pointer motion to @p pos.
     */
    void pointerMotionAbsoluteRequested(const QPointF &pos);
    /**
     * Requests a pointer button pressed for @p button.
     */
    void pointerButtonPressRequested(quint32 button);
    /**
     * Requests a pointer button release for @p button.
     */
    void pointerButtonReleaseRequested(quint32 button);
    /**
     * Requests a pointer axis for the given @p orientation by @p delta.
     */
    void pointerAxisRequested(Qt::Orientation orientation, qreal delta);
    /**
     * Requests a touch down at @p pos and identified by @p id.
     */
    void touchDownRequested(quint32 id, const QPointF &pos);
    /**
     * Requests a touch motion by @p pos and identified by @p id.
     */
    void touchMotionRequested(quint32 id, const QPointF &pos);
    /**
     * Requests a touch up identified by @p id.
     */
    void touchUpRequested(quint32 id);
    /**
     * Requests a touch cancel event.
     */
    void touchCancelRequested();
    /**
     * Requests a touch frame event.
     */
    void touchFrameRequested();
    /**
     * Requests a keyboard key code (i.e. KEY_, see linux/input.h) pressed for @p keycode.
     */
    void keyboardKeyCodeRequested(quint32 keycode, bool pressed);

    /**
     * Requests the keymap to use when a keysym is received
     */
    void keyboardKeymap(const QByteArray &keymap);

    /**
     * Requests a keyboard keysym (i.e. XKB) pressed for @p keysym.
     */
    void keyboardKeySymRequested(quint32 keysym, bool pressed);

private:
    friend class FakeInputInterfacePrivate;
    FakeInputDevice(FakeInputInterface *parent, wl_resource *resource);
    QScopedPointer<FakeInputDevicePrivate> d;
};

}

Q_DECLARE_METATYPE(KWaylandServer::FakeInputDevice *)
