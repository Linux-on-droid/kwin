/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "fakeinputdevice.h"
#include "wayland/fakeinput_interface.h"
#include <keyboard_input.h>
#include <xkbcommon/xkbcommon.h>

namespace KWin
{
static int s_lastDeviceId = 0;

FakeInputDevice::FakeInputDevice(KWaylandServer::FakeInputDevice *device, QObject *parent)
    : InputDevice(parent)
    , m_name(QStringLiteral("Fake Input Device %1").arg(++s_lastDeviceId))
{
    connect(device, &KWaylandServer::FakeInputDevice::authenticationRequested, this, [device](const QString &application, const QString &reason) {
        Q_UNUSED(application)
        Q_UNUSED(reason)
        // TODO: make secure
        device->setAuthentication(true);
    });
    connect(device, &KWaylandServer::FakeInputDevice::pointerMotionRequested, this, [this](const QSizeF &delta) {
        // TODO: Fix time
        Q_EMIT pointerMotion(delta, delta, 0, 0, this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::pointerMotionAbsoluteRequested, this, [this](const QPointF &pos) {
        // TODO: Fix time
        Q_EMIT pointerMotionAbsolute(pos, 0, this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::pointerButtonPressRequested, this, [this](quint32 button) {
        // TODO: Fix time
        Q_EMIT pointerButtonChanged(button, InputRedirection::PointerButtonPressed, 0, this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::pointerButtonReleaseRequested, this, [this](quint32 button) {
        // TODO: Fix time
        Q_EMIT pointerButtonChanged(button, InputRedirection::PointerButtonReleased, 0, this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::pointerAxisRequested, this, [this](Qt::Orientation orientation, qreal delta) {
        // TODO: Fix time
        InputRedirection::PointerAxis axis;
        switch (orientation) {
        case Qt::Horizontal:
            axis = InputRedirection::PointerAxisHorizontal;
            break;
        case Qt::Vertical:
            axis = InputRedirection::PointerAxisVertical;
            break;
        default:
            Q_UNREACHABLE();
            break;
        }
        // TODO: Fix time
        Q_EMIT pointerAxisChanged(axis, delta, 0, InputRedirection::PointerAxisSourceUnknown, 0, this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::touchDownRequested, this, [this](qint32 id, const QPointF &pos) {
        // TODO: Fix time
        Q_EMIT touchDown(id, pos, 0, this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::touchMotionRequested, this, [this](qint32 id, const QPointF &pos) {
        // TODO: Fix time
        Q_EMIT touchMotion(id, pos, 0, this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::touchUpRequested, this, [this](qint32 id) {
        // TODO: Fix time
        Q_EMIT touchUp(id, 0, this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::touchCancelRequested, this, [this]() {
        Q_EMIT touchCanceled(this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::touchFrameRequested, this, [this]() {
        Q_EMIT touchFrame(this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::keyboardKeyCodeRequested, this, [this](quint32 button, bool pressed) {
        // TODO: Fix time
        Q_EMIT keyChanged(button, pressed ? InputRedirection::KeyboardKeyPressed : InputRedirection::KeyboardKeyReleased, 0, this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::keyboardKeySymRequested, this, [this](quint32 keysym, bool pressed) {
        // TODO: Fix time
        Q_EMIT keySymChanged(keysym, pressed ? InputRedirection::KeyboardKeyPressed : InputRedirection::KeyboardKeyReleased, mXkbKeymap.get(), 0, this);
    });
    connect(device, &KWaylandServer::FakeInputDevice::keyboardKeymap, this, [this](const QByteArray &keymap) {
        // TODO: Fix time
        auto xkb = input()->keyboard()->xkb();
        mXkbKeymap.reset(xkb_keymap_new_from_buffer(xkb->context(), keymap.constData(), keymap.size(),
                                                    XKB_KEYMAP_FORMAT_TEXT_V1,
                                                    XKB_KEYMAP_COMPILE_NO_FLAGS));
    });
}

QString FakeInputDevice::sysName() const
{
    return QString();
}

QString FakeInputDevice::name() const
{
    return m_name;
}

bool FakeInputDevice::isEnabled() const
{
    return true;
}

void FakeInputDevice::setEnabled(bool enabled)
{
    Q_UNUSED(enabled)
}

LEDs FakeInputDevice::leds() const
{
    return LEDs();
}

void FakeInputDevice::setLeds(LEDs leds)
{
    Q_UNUSED(leds)
}

bool FakeInputDevice::isKeyboard() const
{
    return true;
}

bool FakeInputDevice::isAlphaNumericKeyboard() const
{
    return true;
}

bool FakeInputDevice::isPointer() const
{
    return true;
}

bool FakeInputDevice::isTouchpad() const
{
    return false;
}

bool FakeInputDevice::isTouch() const
{
    return true;
}

bool FakeInputDevice::isTabletTool() const
{
    return false;
}

bool FakeInputDevice::isTabletPad() const
{
    return false;
}

bool FakeInputDevice::isTabletModeSwitch() const
{
    return false;
}

bool FakeInputDevice::isLidSwitch() const
{
    return false;
}

} // namespace KWin
