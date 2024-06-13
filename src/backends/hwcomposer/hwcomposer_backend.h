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

#include <QSemaphore>

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
    HwcomposerOutput(HwcomposerBackend *backend, hwc2_compat_display_t* display);
    ~HwcomposerOutput() override;

    RenderLoop *renderLoop() const override;
    void setDpmsMode(DpmsMode mode) override;
    void updateDpmsMode(DpmsMode mode);
    void updateEnabled(bool enable);
    bool isEnabled() const;
    void setStatesInternal();
    void notifyFrame();
    void handleVSync(int64_t timestamp);

    QVector<int32_t> regionToRects(const QRegion &region) const;
    HwcomposerWindow *createSurface();

Q_SIGNALS:
    void dpmsModeRequested(HwcomposerOutput::DpmsMode mode);
private Q_SLOTS:
    void compositing(int flags);

private:
    friend class HwcomposerBackend;
    std::unique_ptr<RenderLoop> m_renderLoop;
    QSize m_pixelSize;
    bool m_isEnabled = true;
    QSemaphore m_compositingSemaphore;
    qint64 m_vsyncPeriod;
    qint64 m_idle_time;
    qint64 m_vsync_last_timestamp;

    HwcomposerBackend *m_backend;
    hwc2_compat_display_t *m_display;
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

    //TODO: Move to outputs
    QSize size() const;
    HwcomposerWindow *createSurface();
    void enableVSync(bool enable);

    void updateOutputState(hwc2_display_t display);
    void wakeVSync(hwc2_display_t display, int64_t timestamp);

    QVector<CompositingType> supportedCompositors() const override {
        return QVector<CompositingType>{OpenGLCompositing};
    }

    bool isBacklightOff() const {
        return m_outputBlank;
    }

    hwc2_compat_device_t *hwc2_device() const {
        return m_hwc2device;
    }

    //TODO: Move to outputs
    hwc2_compat_display_t *hwc2_display() const {
        return m_hwc2_primary_display;
    }

private Q_SLOTS:
    void toggleBlankOutput();

private:
    friend HwcomposerWindow;
    Session *m_session;

    void RegisterCallbacks();

    //TODO: Change to outputs
    std::unique_ptr<HwcomposerOutput> m_output;
    //TODO: Move to outputs
    bool m_hasVsync = false;
    bool m_outputBlank = true;
    hwc2_compat_display_t *m_hwc2_primary_display = nullptr;

    hwc2_compat_device_t *m_hwc2device = nullptr;
};

class HwcomposerWindow : public HWComposerNativeWindow
{
public:
    virtual ~HwcomposerWindow();
    void present(HWComposerNativeWindowBuffer *buffer) override;

private:
    friend HwcomposerBackend;
    HwcomposerWindow(HwcomposerBackend *backend);
    HwcomposerBackend *m_backend;
    int lastPresentFence = -1;

    hwc2_compat_display_t *m_display = nullptr;
};

}
