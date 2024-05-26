/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "renderloop.h"
#include "options.h"
#include "renderloop_p.h"
#include "scene/surfaceitem.h"
#include "utils/common.h"
#include "window.h"
#include "workspace.h"

using namespace std::chrono_literals;

namespace KWin
{

RenderLoopPrivate *RenderLoopPrivate::get(RenderLoop *loop)
{
    return loop->d.get();
}

RenderLoopPrivate::RenderLoopPrivate(RenderLoop *q, Output *output)
    : q(q)
    , output(output)
{
    compositeTimer.setSingleShot(true);
    QObject::connect(&compositeTimer, &QTimer::timeout, q, [this]() {
        dispatch();
    });
}

static std::chrono::nanoseconds estimateNextPageflip(std::chrono::nanoseconds earliestSubmitTime, std::chrono::nanoseconds lastPageflip, std::chrono::nanoseconds vblankInterval)
{
    // the last pageflip may be in the future
    const uint64_t pageflipsSince = earliestSubmitTime > lastPageflip ? (earliestSubmitTime - lastPageflip) / vblankInterval : 0;
    return lastPageflip + vblankInterval * (pageflipsSince + 1);
}

void RenderLoopPrivate::scheduleNextRepaint()
{
    if (kwinApp()->isTerminating() || compositeTimer.isActive()) {
        return;
    }
    scheduleRepaint(nextPresentationTimestamp);
}

void RenderLoopPrivate::scheduleRepaint(std::chrono::nanoseconds lastTargetTimestamp)
{
    pendingReschedule = false;
    const std::chrono::nanoseconds vblankInterval(1'000'000'000'000ull / refreshRate);
    const std::chrono::nanoseconds currentTime(std::chrono::steady_clock::now().time_since_epoch());

    // Estimate when it's a good time to perform the next compositing cycle.
    // the 1ms on top of the safety margin is required for timer and scheduler inaccuracies
    const std::chrono::nanoseconds expectedCompositingTime = std::min(renderJournal.result() + safetyMargin + 1ms, 2 * vblankInterval);

    if (presentationMode == PresentationMode::VSync) {
        // normal presentation: pageflips only happen at vblank
        if (maxPendingFrameCount == 1) {
            // keep the old behavior for backends not supporting triple buffering
            nextPresentationTimestamp = estimateNextPageflip(currentTime, lastPresentationTimestamp, vblankInterval);
        } else {
            // estimate the next pageflip that can realistically be hit
            nextPresentationTimestamp = estimateNextPageflip(std::max(lastTargetTimestamp, currentTime + expectedCompositingTime), lastPresentationTimestamp, vblankInterval);
        }
    } else if (presentationMode == PresentationMode::Async || presentationMode == PresentationMode::AdaptiveAsync) {
        // tearing: pageflips happen ASAP
        nextPresentationTimestamp = currentTime;
    } else {
        // adaptive sync: pageflips happen after one vblank interval
        // TODO read minimum refresh rate from the EDID and take it into account here
        nextPresentationTimestamp = lastPresentationTimestamp + vblankInterval;
    }

    const std::chrono::nanoseconds nextRenderTimestamp = nextPresentationTimestamp - expectedCompositingTime;
    compositeTimer.start(std::max(0ms, std::chrono::duration_cast<std::chrono::milliseconds>(nextRenderTimestamp - currentTime)));
}

void RenderLoopPrivate::delayScheduleRepaint()
{
    pendingReschedule = true;
}

void RenderLoopPrivate::notifyFrameDropped()
{
    Q_ASSERT(pendingFrameCount > 0);
    pendingFrameCount--;

    if (!inhibitCount && pendingReschedule) {
        scheduleNextRepaint();
    }
}

void RenderLoopPrivate::notifyFrameCompleted(std::chrono::nanoseconds timestamp, std::optional<std::chrono::nanoseconds> renderTime, PresentationMode mode)
{
    Q_ASSERT(pendingFrameCount > 0);
    pendingFrameCount--;

    notifyVblank(timestamp);

    if (renderTime) {
        renderJournal.add(*renderTime, timestamp);
    }
    if (compositeTimer.isActive()) {
        // reschedule to match the new timestamp and render time
        scheduleRepaint(lastPresentationTimestamp);
    }
    if (!inhibitCount && pendingReschedule) {
        scheduleNextRepaint();
    }

    Q_EMIT q->framePresented(q, timestamp, mode);
}

void RenderLoopPrivate::notifyVblank(std::chrono::nanoseconds timestamp)
{
    if (lastPresentationTimestamp <= timestamp) {
        lastPresentationTimestamp = timestamp;
    } else {
        qCDebug(KWIN_CORE,
                "Got invalid presentation timestamp: %lld (current %lld)",
                static_cast<long long>(timestamp.count()),
                static_cast<long long>(lastPresentationTimestamp.count()));
        lastPresentationTimestamp = std::chrono::steady_clock::now().time_since_epoch();
    }
}

void RenderLoopPrivate::dispatch()
{
    // On X11, we want to ignore repaints that are scheduled by windows right before
    // the Compositor starts repainting.
    pendingRepaint = true;

    Q_EMIT q->frameRequested(q);

    // The Compositor may decide to not repaint when the frameRequested() signal is
    // emitted, in which case the pending repaint flag has to be reset manually.
    pendingRepaint = false;
}

void RenderLoopPrivate::invalidate()
{
    pendingReschedule = false;
    pendingFrameCount = 0;
    compositeTimer.stop();
}

RenderLoop::RenderLoop(Output *output)
    : d(std::make_unique<RenderLoopPrivate>(this, output))
{
}

RenderLoop::~RenderLoop()
{
}

void RenderLoop::inhibit()
{
    d->inhibitCount++;

    if (d->inhibitCount == 1) {
        d->compositeTimer.stop();
    }
}

void RenderLoop::uninhibit()
{
    Q_ASSERT(d->inhibitCount > 0);
    d->inhibitCount--;

    if (d->inhibitCount == 0) {
        d->scheduleNextRepaint();
    }
}

void RenderLoop::prepareNewFrame()
{
    d->pendingFrameCount++;
}

void RenderLoop::beginPaint()
{
    d->pendingRepaint = false;
}

int RenderLoop::refreshRate() const
{
    return d->refreshRate;
}

void RenderLoop::setRefreshRate(int refreshRate)
{
    if (d->refreshRate == refreshRate) {
        return;
    }
    d->refreshRate = refreshRate;
    Q_EMIT refreshRateChanged();
}

void RenderLoop::setPresentationSafetyMargin(std::chrono::nanoseconds safetyMargin)
{
    d->safetyMargin = safetyMargin;
}

void RenderLoop::scheduleRepaint(Item *item)
{
    if (d->pendingRepaint) {
        return;
    }
    const bool vrr = d->presentationMode == PresentationMode::AdaptiveSync || d->presentationMode == PresentationMode::AdaptiveAsync;
    if (vrr && workspace()->activeWindow() && d->output) {
        Window *const activeWindow = workspace()->activeWindow();
        if (activeWindow->isOnOutput(d->output) && activeWindow->surfaceItem() && item != activeWindow->surfaceItem() && activeWindow->surfaceItem()->frameTimeEstimation() <= std::chrono::nanoseconds(1'000'000'000) / 30) {
            return;
        }
    }
    if (d->pendingFrameCount < d->maxPendingFrameCount && !d->inhibitCount) {
        d->scheduleNextRepaint();
    } else {
        d->delayScheduleRepaint();
    }
}

std::chrono::nanoseconds RenderLoop::lastPresentationTimestamp() const
{
    return d->lastPresentationTimestamp;
}

std::chrono::nanoseconds RenderLoop::nextPresentationTimestamp() const
{
    return d->nextPresentationTimestamp;
}

void RenderLoop::setPresentationMode(PresentationMode mode)
{
    if (d->presentationMode != mode) {
        qWarning() << "presentation mode switched to" << mode;
    }
    d->presentationMode = mode;
}

void RenderLoop::setMaxPendingFrameCount(uint32_t maxCount)
{
    d->maxPendingFrameCount = maxCount;
}

} // namespace KWin

#include "moc_renderloop.cpp"
