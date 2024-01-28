/*
    SPDX-FileCopyrightText: 2016 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "input.h"
#include "kwin_export.h"

#include <QObject>

namespace KWin
{

/**
 * Describes the source types for axis events. This indicates to the client how an axis event was
 * physically generated; a client may adjust the user interface accordingly. For example, scroll events
 * from a "finger" source may be in a smooth coordinate space with kinetic scrolling whereas a "wheel"
 * source may be in discrete steps of a number of lines.
 *
 * The "continuous" axis source is a device generating events in a continuous coordinate space, but
 * using something other than a finger. One example for this source is button-based scrolling where
 * the vertical motion of a device is converted to scroll events while a button is held down.
 *
 * The "wheel tilt" axis source indicates that the actual device is a wheel but the scroll event is not
 * caused by a rotation but a (usually sideways) tilt of the wheel.
 */
enum class InputDeviceAxisSource {
    Unknown,
    Wheel,
    Finger,
    Continuous,
    WheelTilt,
};

enum class InputDeviceAxis {
    Vertical,
    Horizontal,
};

/**
 * Maps to wl_pointer.axis_relative_direction. Currently used for y axis only
 */
enum class InputDeviceAxisRelativeDirection {
    Normal,
    Inverted
};

/**
 * The InputDevice class represents an input device, e.g. a mouse, or a keyboard, etc.
 */
class KWIN_EXPORT InputDevice : public QObject
{
    Q_OBJECT

public:
    explicit InputDevice(QObject *parent = nullptr);

    virtual QString sysName() const = 0;
    virtual QString name() const = 0;

    virtual bool isEnabled() const = 0;
    virtual void setEnabled(bool enabled) = 0;

    virtual LEDs leds() const = 0;
    virtual void setLeds(LEDs leds) = 0;

    virtual bool isKeyboard() const = 0;
    virtual bool isAlphaNumericKeyboard() const = 0;
    virtual bool isPointer() const = 0;
    virtual bool isTouchpad() const = 0;
    virtual bool isTouch() const = 0;
    virtual bool isTabletTool() const = 0;
    virtual bool isTabletPad() const = 0;
    virtual bool isTabletModeSwitch() const = 0;
    virtual bool isLidSwitch() const = 0;

    virtual QString outputName() const;
    virtual void setOutputName(const QString &outputName);

Q_SIGNALS:
    void keyChanged(quint32 key, InputRedirection::KeyboardKeyState, std::chrono::microseconds time, InputDevice *device);
    void pointerButtonChanged(quint32 button, InputRedirection::PointerButtonState state, std::chrono::microseconds time, InputDevice *device);
    void pointerMotionAbsolute(const QPointF &position, std::chrono::microseconds time, InputDevice *device);
    void pointerMotion(const QPointF &delta, const QPointF &deltaNonAccelerated, std::chrono::microseconds time, InputDevice *device);
    void pointerAxisChanged(InputDeviceAxis axis, qreal delta, qint32 deltaV120,
                            InputDeviceAxisSource source,
                            InputDeviceAxisRelativeDirection relativeDirection,
                            std::chrono::microseconds time, InputDevice *device);
    void pointerFrame(InputDevice *device);
    void touchFrame(InputDevice *device);
    void touchCanceled(InputDevice *device);
    void touchDown(qint32 id, const QPointF &absolutePos, std::chrono::microseconds time, InputDevice *device);
    void touchUp(qint32 id, std::chrono::microseconds time, InputDevice *device);
    void touchMotion(qint32 id, const QPointF &absolutePos, std::chrono::microseconds time, InputDevice *device);
    void swipeGestureBegin(int fingerCount, std::chrono::microseconds time, InputDevice *device);
    void swipeGestureUpdate(const QPointF &delta, std::chrono::microseconds time, InputDevice *device);
    void swipeGestureEnd(std::chrono::microseconds time, InputDevice *device);
    void swipeGestureCancelled(std::chrono::microseconds time, InputDevice *device);
    void pinchGestureBegin(int fingerCount, std::chrono::microseconds time, InputDevice *device);
    void pinchGestureUpdate(qreal scale, qreal angleDelta, const QPointF &delta, std::chrono::microseconds time, InputDevice *device);
    void pinchGestureEnd(std::chrono::microseconds time, InputDevice *device);
    void pinchGestureCancelled(std::chrono::microseconds time, InputDevice *device);
    void holdGestureBegin(int fingerCount, std::chrono::microseconds time, InputDevice *device);
    void holdGestureEnd(std::chrono::microseconds time, InputDevice *device);
    void holdGestureCancelled(std::chrono::microseconds time, InputDevice *device);
    void switchToggledOn(std::chrono::microseconds time, InputDevice *device);
    void switchToggledOff(std::chrono::microseconds time, InputDevice *device);

    void tabletToolEvent(InputRedirection::TabletEventType type, const QPointF &pos,
                         qreal pressure, int xTilt, int yTilt, qreal rotation, bool tipDown,
                         bool tipNear, const TabletToolId &tabletToolId, std::chrono::microseconds time);
    void tabletToolButtonEvent(uint button, bool isPressed, const TabletToolId &tabletToolId, std::chrono::microseconds time);

    void tabletPadButtonEvent(uint button, bool isPressed, const TabletPadId &tabletPadId, std::chrono::microseconds time);
    void tabletPadStripEvent(int number, int position, bool isFinger, const TabletPadId &tabletPadId, std::chrono::microseconds time);
    void tabletPadRingEvent(int number, int position, bool isFinger, const TabletPadId &tabletPadId, std::chrono::microseconds time);
};

} // namespace KWin
