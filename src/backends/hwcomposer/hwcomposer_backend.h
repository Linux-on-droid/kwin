/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include "backends/libinput/libinputbackend.h"
#include "core/output.h"
#include "core/outputbackend.h"
#include "core/renderloop.h"

#include "dpmsinputeventfilter.h"
#include "input.h"

#include <QSemaphore>
#include <QTimer>

#include <hardware/hwcomposer.h>
#include <hwcomposer_window.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>


typedef struct hwc_display_contents_1 hwc_display_contents_1_t;
typedef struct hwc_layer_1 hwc_layer_1_t;
typedef struct hwc_composer_device_1 hwc_composer_device_1_t;

class HWComposerNativeWindowBuffer;

namespace KWin
{
class HwcomposerWindow;
class HwcomposerBackend;

class HwcomposerOutput : public Output
{
    Q_OBJECT
public:
    HwcomposerOutput(HwcomposerBackend *backend, hwc2_display_t display);
    ~HwcomposerOutput() override;

    RenderLoop *renderLoop() const override;
    void setDpmsMode(DpmsMode mode) override;
    void updateDpmsMode(DpmsMode dpmsMode);
    void updateEnabled(bool enabled);
    bool isEnabled() const;
    void resetStates();
    void notifyFrame();
    void handleVSync(int64_t timestamp);
    HwcomposerWindow *createSurface();
    void enableVSync(bool enable);
    void setPowerMode(bool enable);
    QVector<int32_t> regionToRects(const QRegion &region) const;

    hwc2_compat_display_t *hwc2_display() const
    {
        return m_display;
    }

    hwc2_display_t displayId() const
    {
        return m_displayId;
    }

private Q_SLOTS:
    void compositing(int flags);

private:
    friend HwcomposerWindow;

    hwc2_compat_display_t *m_display;
    std::unique_ptr<RenderLoop> m_renderLoop;
    QSize m_pixelSize;
    QSemaphore m_compositingSemaphore;
    qint64 m_vsyncPeriod;
    qint64 m_idle_time;
    qint64 m_vsync_last_timestamp;
    bool m_hasVsync = false;
    QTimer m_turnOffTimer;

    HwcomposerBackend *m_backend;
    hwc2_display_t m_displayId;
};

class KWIN_EXPORT HwcomposerBackend : public OutputBackend
{
    Q_OBJECT
public:
    explicit HwcomposerBackend(Session *session, QObject *parent = nullptr);
    virtual ~HwcomposerBackend();

    bool initialize() override;
    std::unique_ptr<OpenGLBackend> createOpenGLBackend() override;
    std::unique_ptr<InputBackend> createInputBackend() override;
    Outputs outputs() const override;
    void createDpmsFilter();
    void clearDpmsFilter();

    void wakeVSync(hwc2_display_t display, int64_t timestamp);
    void handleHotplug(hwc2_display_t display, bool connected, bool primaryDisplay);
    void updateOutputState(hwc2_display_t display);

    QVector<CompositingType> supportedCompositors() const override
    {
        return QVector<CompositingType>{OpenGLCompositing};
    }

    hwc2_compat_device_t *hwc2_device() const
    {
        return m_hwc2device;
    }

private:
    void RegisterCallbacks();
    void createOutput(hwc2_display_t display);

    std::map<hwc2_display_t, std::unique_ptr<HwcomposerOutput>> m_outputs;
    hwc2_compat_device_t *m_hwc2device = nullptr;
    std::unique_ptr<DpmsInputEventFilter> m_dpmsFilter;

    Session *m_session;
};

class HwcomposerWindow : public HWComposerNativeWindow
{
public:
    virtual ~HwcomposerWindow();
    void present(HWComposerNativeWindowBuffer *buffer) override;

private:
    friend HwcomposerOutput;

    HwcomposerWindow(HwcomposerOutput *output);
    HwcomposerOutput *m_output;
    int lastPresentFence = -1;
    hwc2_compat_display_t *m_display;
};
}
