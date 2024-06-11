/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/
#ifndef KWIN_HWCOMPOSER_BACKEND_H
#define KWIN_HWCOMPOSER_BACKEND_H
#include "core/outputbackend.h"
#include "core/output.h"
#include "input.h"
#include "backends/libinput/libinputbackend.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QWaitCondition>
#include <QSemaphore>
#include <QFile>
// libhybris
#include <hardware/hwcomposer.h>
#include <hwcomposer_window.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>
#include <QBasicTimer>

// needed as hwcomposer_window.h includes EGL which on non-arm includes Xlib
#include <fixx11h.h>
#include "core/renderloop.h"
#include "wayland/output_interface.h"


typedef struct hwc_display_contents_1 hwc_display_contents_1_t;
typedef struct hwc_layer_1 hwc_layer_1_t;
typedef struct hwc_composer_device_1 hwc_composer_device_1_t;

class HWComposerNativeWindowBuffer;

namespace KWin
{

class HwcomposerWindow;
class HwcomposerBackend;
class BacklightInputEventFilter;


class HwcomposerOutput : public Output
{
    Q_OBJECT
public:
    HwcomposerOutput(HwcomposerBackend *backend, hwc2_compat_display_t* hwc2_primary_display);
    ~HwcomposerOutput() override;

    void init();

    RenderLoop *renderLoop() const override;
    bool isValid() const;
    bool hardwareTransforms() const;
    void setDpmsMode(DpmsMode mode) override;
    void updateDpmsMode(DpmsMode mode);
    void updateEnabled(bool enable);
    bool isEnabled() const;
    void setStatesInternal();
    void notifyFrame();
    void handleVSync(int64_t timestamp);
    QVector<int32_t> regionToRects(const QRegion &region) const;
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
    hwc2_compat_display_t *m_hwc2_primary_display;
};

class KWIN_EXPORT HwcomposerBackend : public OutputBackend
{
    Q_OBJECT
public:
    explicit HwcomposerBackend(Session *session, QObject *parent = nullptr);
    virtual ~HwcomposerBackend();

    Session *session() const;
    bool initialize() override;
    std::unique_ptr<OpenGLBackend> createOpenGLBackend() override;

    Outputs outputs() const override;

    QSize size() const;

    int scale() const;

    HwcomposerWindow *createSurface();

    std::unique_ptr<InputBackend> createInputBackend() override;
    void updateOutputState(hwc2_display_t display);

    void enableVSync(bool enable);
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

    hwc2_compat_display_t *hwc2_display() const {
        return m_hwc2_primary_display;
    }

Q_SIGNALS:
    void outputBlankChanged();

private Q_SLOTS:
    void toggleBlankOutput();
    void screenBrightnessChanged(int brightness) {
        m_oldScreenBrightness = brightness;
    }

private:
    friend HwcomposerWindow;   
    void setPowerMode(bool enable);
    void toggleScreenBrightness();
    Session *m_session;
    bool m_hasVsync = false;
    std::unique_ptr<BacklightInputEventFilter> m_filter;
    std::unique_ptr<HwcomposerOutput> m_output;
    bool m_outputBlank = true;    
    int m_oldScreenBrightness = 0x7f;

    void RegisterCallbacks();

    hwc2_compat_device_t *m_hwc2device = nullptr;
    hwc2_compat_display_t* m_hwc2_primary_display = nullptr;
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

    hwc2_compat_display_t *m_hwc2_primary_display = nullptr;
};

class BacklightInputEventFilter : public QObject,  public InputEventFilter
{
public:
    BacklightInputEventFilter(HwcomposerBackend *backend);
    virtual ~BacklightInputEventFilter();

    bool pointerEvent(QMouseEvent *event, quint32 nativeButton);
    bool wheelEvent(QWheelEvent *event);
    bool keyEvent(QKeyEvent *event);
    bool touchDown(qint32 id, const QPointF &pos, quint32 time);
    bool touchMotion(qint32 id, const QPointF &pos, quint32 time);
    bool touchUp(qint32 id, quint32 time);
private:
    void toggleBacklight();
    HwcomposerBackend *m_backend;
    QElapsedTimer m_doubleTapTimer;
    QVector<qint32> m_touchPoints;
    bool m_secondTap = false;
};

}

#endif
