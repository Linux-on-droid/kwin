/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2013, 2016 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2018 Roman Gilg <subdiff@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "touch_input.h"

#include <config-kwin.h>

#include "decorations/decoratedclient.h"
#include "input_event_spy.h"
#include "inputdevice.h"
#include "output.h"
#include "pointer_input.h"
#include "wayland/seat_interface.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"
// KDecoration
#include <KDecoration2/Decoration>
// screenlocker
#if KWIN_BUILD_SCREENLOCKER
#include <KScreenLocker/KsldApp>
#endif
// Qt
#include <QHoverEvent>
#include <QWindow>

namespace KWin
{

TouchInputRedirection::TouchInputRedirection(InputRedirection *parent)
    : InputDeviceHandler(parent)
{
}

TouchInputRedirection::~TouchInputRedirection() = default;

void TouchInputRedirection::init()
{
    Q_ASSERT(!inited());
    waylandServer()->seat()->setHasTouch(input()->hasTouch());
    connect(input(), &InputRedirection::hasTouchChanged,
            waylandServer()->seat(), &KWaylandServer::SeatInterface::setHasTouch);

    setInited(true);
    InputDeviceHandler::init();

#if KWIN_BUILD_SCREENLOCKER
    if (waylandServer()->hasScreenLockerIntegration()) {
        connect(ScreenLocker::KSldApp::self(), &ScreenLocker::KSldApp::lockStateChanged, this, [this]() {
            cancel();
            // position doesn't matter
            update();
        });
    }
#endif
    connect(workspace(), &QObject::destroyed, this, [this] {
        setInited(false);
    });
    connect(waylandServer(), &QObject::destroyed, this, [this] {
        setInited(false);
    });
}

bool TouchInputRedirection::focusUpdatesBlocked()
{
    if (m_windowUpdatedInCycle) {
        return true;
    }
    m_windowUpdatedInCycle = true;
    if (waylandServer()->seat()->isDragTouch()) {
        return true;
    }
    if (m_activeTouchPoints.count() > 1) {
        // first touch defines focus
        return true;
    }
    return false;
}

bool TouchInputRedirection::positionValid() const
{
    // we can only determine a position with at least one touch point
    return !m_activeTouchPoints.isEmpty();
}

void TouchInputRedirection::focusUpdate(Window *focusOld, Window *focusNow)
{
    // TODO: handle pointer grab aka popups

    if (focusOld && focusOld->isClient()) {
        focusOld->pointerLeaveEvent();
    }
    disconnect(m_focusGeometryConnection);
    m_focusGeometryConnection = QMetaObject::Connection();

    if (focusNow && focusNow->isClient()) {
        focusNow->pointerEnterEvent(m_lastPosition.toPoint());
    }

    auto seat = waylandServer()->seat();
    if (!focusNow || !focusNow->surface()) {
        seat->setFocusedTouchSurface(nullptr);
        return;
    }

    // TODO: invalidate pointer focus?

    // FIXME: add input transformation API to KWaylandServer::SeatInterface for touch input
    seat->setFocusedTouchSurface(focusNow->surface(), -1 * focusNow->inputTransformation().map(focusNow->pos()) + focusNow->pos());
    m_focusGeometryConnection = connect(focusNow, &Window::frameGeometryChanged, this, [this]() {
        if (!focus()) {
            return;
        }
        auto seat = waylandServer()->seat();
        if (focus()->surface() != seat->focusedTouchSurface()) {
            return;
        }
        seat->setFocusedTouchSurfacePosition(-1 * focus()->inputTransformation().map(focus()->pos()) + focus()->pos());
    });
}

void TouchInputRedirection::cleanupDecoration(Decoration::DecoratedClientImpl *old, Decoration::DecoratedClientImpl *now)
{
    Q_UNUSED(old);
    Q_UNUSED(now);

    // nothing to do
}

static QPointF mapAbsoluteToWorkspace(const QPointF &pos, const InputDevice *device)
{
    Output *output = device->output();
    if (Q_UNLIKELY(!output)) {
        output = workspace()->activeOutput();
    }

    QPointF ret;
    // TODO: Do we need to handle the flipped cases differently?
    switch (output->transform()) {
    case Output::Transform::Normal:
    case Output::Transform::Flipped:
        ret = pos;
        break;
    case Output::Transform::Rotated90:
    case Output::Transform::Flipped90:
        ret = QPointF(1.0 - pos.y(), pos.x());
        break;
    case Output::Transform::Rotated180:
    case Output::Transform::Flipped180:
        ret = QPointF(1.0 - pos.x(), 1.0 - pos.y());
        break;
    case Output::Transform::Rotated270:
    case Output::Transform::Flipped270:
        ret = QPointF(pos.y(), 1.0 - pos.x());
        break;
    default:
        Q_UNREACHABLE();
    }

    const QRectF bounds = output->geometry();
    return bounds.topLeft() + QPointF(ret.x() * bounds.width(), ret.y() * bounds.height());
}

void TouchInputRedirection::processDown(qint32 id, const QPointF &pos, quint32 time, InputDevice *device)
{
    if (!inited()) {
        return;
    }

    const QPointF workspacePos = mapAbsoluteToWorkspace(pos, device);

    m_lastPosition = workspacePos;
    m_windowUpdatedInCycle = false;
    m_activeTouchPoints.insert(id);
    if (m_activeTouchPoints.count() == 1) {
        update();
    }
    input()->setLastInputHandler(this);
    input()->processSpies(std::bind(&InputEventSpy::touchDown, std::placeholders::_1, id, workspacePos, time));
    input()->processFilters(std::bind(&InputEventFilter::touchDown, std::placeholders::_1, id, workspacePos, time));
    m_windowUpdatedInCycle = false;
}

void TouchInputRedirection::processUp(qint32 id, quint32 time, InputDevice *device)
{
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    if (!m_activeTouchPoints.remove(id)) {
        return;
    }
    input()->setLastInputHandler(this);
    m_windowUpdatedInCycle = false;
    input()->processSpies(std::bind(&InputEventSpy::touchUp, std::placeholders::_1, id, time));
    input()->processFilters(std::bind(&InputEventFilter::touchUp, std::placeholders::_1, id, time));
    m_windowUpdatedInCycle = false;
    if (m_activeTouchPoints.count() == 0) {
        update();
    }
}

void TouchInputRedirection::processMotion(qint32 id, const QPointF &pos, quint32 time, InputDevice *device)
{
    if (!inited()) {
        return;
    }
    if (!m_activeTouchPoints.contains(id)) {
        return;
    }

    const QPointF workspacePos = mapAbsoluteToWorkspace(pos, device);

    input()->setLastInputHandler(this);
    m_lastPosition = workspacePos;
    m_windowUpdatedInCycle = false;
    input()->processSpies(std::bind(&InputEventSpy::touchMotion, std::placeholders::_1, id, workspacePos, time));
    input()->processFilters(std::bind(&InputEventFilter::touchMotion, std::placeholders::_1, id, workspacePos, time));
    m_windowUpdatedInCycle = false;
}

void TouchInputRedirection::cancel()
{
    if (!inited()) {
        return;
    }
    // If the touch sequence is artificially cancelled by the compositor, touch motion and touch
    // up events will be silently ignored and won't be passed down through the event filter chain.
    // If the touch sequence is cancelled because we received a TOUCH_CANCEL event from libinput,
    // the compositor will not receive any TOUCH_MOTION or TOUCH_UP events for that slot.
    if (!m_activeTouchPoints.isEmpty()) {
        m_activeTouchPoints.clear();
        input()->processFilters(std::bind(&InputEventFilter::touchCancel, std::placeholders::_1));
    }
}

void TouchInputRedirection::frame()
{
    if (!inited() || !waylandServer()->seat()->hasTouch()) {
        return;
    }
    input()->processFilters(std::bind(&InputEventFilter::touchFrame, std::placeholders::_1));
}

}
