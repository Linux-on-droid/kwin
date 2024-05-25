/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "renderjournal.h"
#include "renderloop.h"

#include <QTimer>

#include <fstream>
#include <optional>

namespace KWin
{

class SurfaceItem;

class KWIN_EXPORT RenderLoopPrivate
{
public:
    static RenderLoopPrivate *get(RenderLoop *loop);
    explicit RenderLoopPrivate(RenderLoop *q, Output *output);

    void dispatch();
    void invalidate();

    void delayScheduleRepaint();
    void scheduleNextRepaint();
    void scheduleRepaint(std::chrono::nanoseconds lastTargetTimestamp);

    void notifyFrameDropped();
    void notifyFrameCompleted(std::chrono::nanoseconds timestamp, std::optional<std::chrono::nanoseconds> renderTime, PresentationMode mode, std::chrono::nanoseconds renderPrediction);
    void notifyVblank(std::chrono::nanoseconds timestamp);

    RenderLoop *const q;
    Output *const output;
    std::optional<std::fstream> m_debugOutput;
    std::chrono::nanoseconds lastPresentationTimestamp = std::chrono::nanoseconds::zero();
    std::chrono::nanoseconds nextPresentationTimestamp = std::chrono::nanoseconds::zero();
    QTimer compositeTimer;
    RenderJournal renderJournal;
    int refreshRate = 60000;
    int pendingFrameCount = 0;
    int inhibitCount = 0;
    bool pendingReschedule = false;
    bool pendingRepaint = false;
    std::chrono::nanoseconds safetyMargin{0};

    PresentationMode presentationMode = PresentationMode::VSync;
    int maxPendingFrameCount = 1;
};

} // namespace KWin
