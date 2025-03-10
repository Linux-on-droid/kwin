/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "hwcomposer_backend.h"
#include "hwcomposer_egl_backend.h"
#include "hwcomposer_logging.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QtConcurrent>

#include <sync/sync.h>

#include "core/renderloop_p.h"


namespace KWin {

HwcomposerBackend::HwcomposerBackend(Session *session, QObject *parent)
    : OutputBackend(parent)
    , m_session(session)
{
}

HwcomposerBackend::~HwcomposerBackend()
{
    if (sceneEglDisplay() != EGL_NO_DISPLAY) {
        eglTerminate(sceneEglDisplay());
    }
}

typedef struct : public HWC2EventListener
{
    HwcomposerBackend *backend = nullptr;
} HwcProcs_v20;

void hwc2_callback_vsync(HWC2EventListener *listener, int32_t sequenceId,
                         hwc2_display_t display, int64_t timestamp)
{
    static_cast<const HwcProcs_v20 *>(listener)->backend->wakeVSync(display, timestamp);
}

void hwc2_callback_hotplug(HWC2EventListener *listener, int32_t sequenceId,
                           hwc2_display_t display, bool connected,
                           bool primaryDisplay)
{
    hwc2_compat_device_on_hotplug(static_cast<const HwcProcs_v20 *>(listener)->backend->hwc2_device(), display, connected);

    static_cast<const HwcProcs_v20 *>(listener)->backend->handleHotplug(display, connected, primaryDisplay);
}

void hwc2_callback_refresh(HWC2EventListener *listener, int32_t sequenceId,
                           hwc2_display_t display)
{
    static_cast<const HwcProcs_v20 *>(listener)->backend->updateOutputState(display);
}

void HwcomposerBackend::RegisterCallbacks()
{
    static int composerSequenceId = 0;

    HwcProcs_v20 *procs = new HwcProcs_v20();
    procs->on_vsync_received = hwc2_callback_vsync;
    procs->on_hotplug_received = hwc2_callback_hotplug;
    procs->on_refresh_received = hwc2_callback_refresh;
    procs->backend = this;

    hwc2_compat_device_register_callback(m_hwc2device, procs, composerSequenceId++);
}

void HwcomposerBackend::createOutput(hwc2_display_t display)
{
    m_outputs[display] = std::make_unique<HwcomposerOutput>(this, display);
    m_outputs[display]->setPowerMode(true);

    Q_EMIT outputAdded(m_outputs[display].get());
}

bool HwcomposerBackend::initialize()
{
    m_hwc2device = hwc2_compat_device_new(false);

    RegisterCallbacks();
    for (int i = 0; i < 5 * 1000; ++i) {
        // Wait at most 5s for hotplug events
        if (hwc2_compat_device_get_display_by_id(m_hwc2device, 0))
        break;
        usleep(1000);
    }

    return true;
}

std::unique_ptr<OpenGLBackend> HwcomposerBackend::createOpenGLBackend()
{
    return std::make_unique<EglHwcomposerBackend>(this);
}

std::unique_ptr<InputBackend>HwcomposerBackend::createInputBackend()
{
    return std::make_unique<LibinputBackend>(m_session);
}

Outputs HwcomposerBackend::outputs() const
{
    QVector<HwcomposerOutput *> outputs;

    for (const auto &out : m_outputs)
        outputs.push_back(out.second.get());
    return outputs;
}

void HwcomposerBackend::createDpmsFilter()
{
    if (m_dpmsFilter) {
        // already another output is off
        return;
    }
    m_dpmsFilter = std::make_unique<DpmsInputEventFilter>();
    input()->prependInputEventFilter(m_dpmsFilter.get());
}

void HwcomposerBackend::clearDpmsFilter()
{
    m_dpmsFilter.reset();
}

void HwcomposerBackend::wakeVSync(hwc2_display_t display, int64_t timestamp)
{
    if (m_outputs.find(display) != m_outputs.end())
        m_outputs[display]->handleVSync(timestamp);
}

void HwcomposerBackend::handleHotplug(hwc2_display_t display, bool connected, bool primaryDisplay)
{
    qCInfo(KWIN_HWCOMPOSER) << "Hotplug Display: " << display << ", connected: " << connected << ", isPrimary: " << primaryDisplay;
    if (connected) {
        if (m_outputs.find(display) == m_outputs.end()) {
            createOutput(display);
            m_outputs[display]->updateEnabled(true);
        }
    } else {
        if (m_outputs.find(display) != m_outputs.end()) {
            m_outputs[display]->updateEnabled(false);
            Q_EMIT outputRemoved(m_outputs[display].get());
            m_outputs.erase(display);
        }
    }
    Q_EMIT outputsQueried();
}

void HwcomposerBackend::updateOutputState(hwc2_display_t display)
{
    m_outputs[display]->resetStates();
    Q_EMIT outputsQueried();
}

HwcomposerOutput::HwcomposerOutput(HwcomposerBackend *backend, hwc2_display_t display)
    : Output(backend)
    , m_renderLoop(std::make_unique<RenderLoop>())
    , m_backend(backend)
    , m_displayId(display)
{
    m_display = hwc2_compat_device_get_display_by_id(backend->hwc2_device(), m_displayId);

    HWC2DisplayConfig *config = hwc2_compat_display_get_active_config(m_display);
    Q_ASSERT(config);

    int32_t width = config->width;
    int32_t height = config->height;
    int32_t dpiX = config->dpiX;
    int32_t dpiY = config->dpiY;
    QSize pixelSize(width, height);
    if (pixelSize.isEmpty()) {
        return;
    }
    QSizeF physicalSize = pixelSize / 3.8;
    if (dpiX != 0 && dpiY != 0) {
        static const qreal factor = 25.4;
        physicalSize = QSizeF(qreal(pixelSize.width() * 1000) / qreal(dpiX) * factor,
                              qreal(pixelSize.height() * 1000) / qreal(dpiY) * factor);
    }
    QString debugDpi = qgetenv("KWIN_DEBUG_DPI");
    if (!debugDpi.isEmpty() && debugDpi.toFloat() != 0) {
        physicalSize = pixelSize / debugDpi.toFloat();
    }

    // Set output information
    // Since Hwcomposer does not provide an EDID structure, we use placeholders for EDID information
    setInformation(Information{
        .name = QStringLiteral("hwcomposer-%1").arg(m_displayId),
        .manufacturer = QStringLiteral("Android"),
        .model = QStringLiteral("Android"),
        .serialNumber = QString(),
        .eisaId = QString(),
        .physicalSize = physicalSize.toSize(),
        .edid = QByteArray(),
        .subPixel = SubPixel::Unknown,
        .capabilities = Capability::Dpms,
        .panelOrientation = KWin::Output::Transform::Normal,
        .internal = false,
        .nonDesktop = false,
    });

    m_turnOffTimer.setSingleShot(true);
    m_turnOffTimer.setInterval(dimAnimationTime());
    connect(&m_turnOffTimer, &QTimer::timeout, this, [this] {
        updateDpmsMode(DpmsMode::Off);
    });

    resetStates();
}

HwcomposerOutput::~HwcomposerOutput()
{
    if (m_display != NULL) {
        free(m_display);
    }
}

RenderLoop *HwcomposerOutput::renderLoop() const
{
    return m_renderLoop.get();
}

void HwcomposerOutput::setDpmsMode(DpmsMode mode)
{
    if (mode == DpmsMode::Off) {
        if (!m_turnOffTimer.isActive()) {
            Q_EMIT aboutToTurnOff(std::chrono::milliseconds(m_turnOffTimer.interval()));
            m_turnOffTimer.start();
        }
        m_backend->createDpmsFilter();
    } else {
        m_turnOffTimer.stop();
        m_backend->clearDpmsFilter();

        if (mode != dpmsMode()) {
            updateDpmsMode(mode);
            Q_EMIT wakeUp();
        }
    }
}

void HwcomposerOutput::updateDpmsMode(DpmsMode dpmsMode)
{
    setPowerMode(dpmsMode == DpmsMode::On);

    State next = m_state;
    next.dpmsMode = dpmsMode;
    setState(next);
}

void HwcomposerOutput::updateEnabled(bool enabled)
{
    State next = m_state;
    next.enabled = enabled;
    setState(next);
}

void HwcomposerOutput::resetStates()
{
    // Retrieve and set display configuration attributes
    HWC2DisplayConfig *config = hwc2_compat_display_get_active_config(m_display);
    Q_ASSERT(config);

    int32_t width = config->width;
    int32_t height = config->height;
    int32_t dpiX = config->dpiX;
    int32_t dpiY = config->dpiY;
    m_vsyncPeriod = (config->vsyncPeriod == 0) ? 16666667 : config->vsyncPeriod;
    m_idle_time = 2 * 1000000;

    // Override with debug environment variables if they exist
    QString debugWidth = qgetenv("KWIN_DEBUG_WIDTH");
    if (!debugWidth.isEmpty()) {
        width = debugWidth.toInt();
    }
    QString debugHeight = qgetenv("KWIN_DEBUG_HEIGHT");
    if (!debugHeight.isEmpty()) {
        height = debugHeight.toInt();
    }
    QSize pixelSize(width, height);

    if (pixelSize.isEmpty()) {
        return;
    }

    // Calculate physical size
    QSizeF physicalSize = pixelSize / 3.8;
    if (dpiX != 0 && dpiY != 0) {
        static const qreal factor = 25.4;
        physicalSize = QSizeF(qreal(pixelSize.width() * 1000) / qreal(dpiX) * factor,
                              qreal(pixelSize.height() * 1000) / qreal(dpiY) * factor);
    }

    QString debugDpi = qgetenv("KWIN_DEBUG_DPI");
    if (!debugDpi.isEmpty() && debugDpi.toFloat() != 0) {
        physicalSize = pixelSize / debugDpi.toFloat();
    }

    float scale = 1.0;
    if (dpiX != 0 && dpiY != 0) {
        float dpi = (dpiX + dpiY) / 2.0;
        if (dpi > 160) {
            scale = dpi / 160.0;
        }
    } else {
        scale = std::min(pixelSize.width() / 96.0, pixelSize.height() / 96.0);
    }

    m_renderLoop->setRefreshRate(10E11 / m_vsyncPeriod);

    QList<std::shared_ptr<OutputMode>> modes;
    OutputMode::Flags modeFlags = OutputMode::Flag::Preferred;
    std::shared_ptr<OutputMode> mode = std::make_shared<OutputMode>(pixelSize, m_renderLoop->refreshRate(), modeFlags);
    modes << mode;
    State initialState;
    initialState.modes = modes;
    initialState.currentMode = modes.constFirst();
    initialState.scale = scale;

    setState(initialState);
    Q_EMIT m_backend->outputsQueried();
}

void HwcomposerOutput::notifyFrame()
{
    int flags = 1;
    if (m_compositingSemaphore.available() > 0) {
        flags = 0;
    }
    QMetaObject::invokeMethod(this, "compositing", Qt::QueuedConnection, Q_ARG(int, flags));
}

void HwcomposerOutput::handleVSync(int64_t timestamp)
{
    m_vsync_last_timestamp = timestamp;
}

HwcomposerWindow *HwcomposerOutput::createSurface()
{
    return new HwcomposerWindow(this);
}

void HwcomposerOutput::enableVSync(bool enable)
{
    if (m_hasVsync == enable) {
        return;
    }
    hwc2_compat_display_set_vsync_enabled(m_display, enable ? HWC2_VSYNC_ENABLE : HWC2_VSYNC_DISABLE);
    m_hasVsync = enable;
}

void HwcomposerOutput::setPowerMode(bool enable)
{
    enableVSync(enable);
    hwc2_compat_display_set_power_mode(m_display, enable ? HWC2_POWER_MODE_ON : HWC2_POWER_MODE_OFF);
}

QVector<int32_t> HwcomposerOutput::regionToRects(const QRegion &region) const
{
    const int height = pixelSize().height();
    const QMatrix4x4 matrix = Output::logicalToNativeMatrix(rect(), scale(), transform());
    QVector<EGLint> rects;
    rects.reserve(region.rectCount() * 4);
    for (const QRect &_rect : region) {
        const QRect rect = matrix.mapRect(_rect);
        rects << rect.left();
        rects << height - (rect.y() + rect.height());
        rects << rect.width();
        rects << rect.height();
    }
    return rects;
}

void HwcomposerOutput::compositing(int flags)
{
    m_compositingSemaphore.release();
    if (flags > 0) {
        RenderLoopPrivate *renderLoopPrivate = RenderLoopPrivate::get(m_renderLoop.get());
        if (renderLoopPrivate->pendingFrameCount > 0) {
            qint64 now = std::chrono::steady_clock::now().time_since_epoch().count();
            qint64 next_vsync = m_vsync_last_timestamp + m_vsyncPeriod;

            if ((next_vsync - now) <= m_idle_time) {
                // Too close! Sad
                std::chrono::nanoseconds ntimestamp(next_vsync + m_vsyncPeriod - m_idle_time);
                renderLoopPrivate->notifyFrameCompleted(ntimestamp);
            } else {
                // We can go ahead
                std::chrono::nanoseconds ntimestamp(next_vsync - m_idle_time);
                renderLoopPrivate->notifyFrameCompleted(ntimestamp);
            }
        }
    }
    m_compositingSemaphore.acquire();
}

HwcomposerWindow::HwcomposerWindow(HwcomposerOutput *output)
    : HWComposerNativeWindow(output->pixelSize().width(), output->pixelSize().height(), HAL_PIXEL_FORMAT_RGBA_8888)
    , m_output(output)
{
    m_display = m_output->hwc2_display();
    hwc2_compat_layer_t *layer = hwc2_compat_display_create_layer(m_display);
    hwc2_compat_layer_set_composition_type(layer, HWC2_COMPOSITION_CLIENT);
    hwc2_compat_layer_set_blend_mode(layer, HWC2_BLEND_MODE_NONE);

    hwc2_compat_layer_set_source_crop(layer, 0.0f, 0.0f, m_output->pixelSize().width(), m_output->pixelSize().height());
    hwc2_compat_layer_set_display_frame(layer, 0, 0, m_output->pixelSize().width(), m_output->pixelSize().height());
    hwc2_compat_layer_set_visible_region(layer, 0, 0, m_output->pixelSize().width(), m_output->pixelSize().height());
}

HwcomposerWindow::~HwcomposerWindow()
{
    if (lastPresentFence != -1) {
        close(lastPresentFence);
    }
}

void HwcomposerWindow::present(HWComposerNativeWindowBuffer *buffer)
{
    uint32_t numTypes = 0;
    uint32_t numRequests = 0;
    int displayId = m_output->displayId();
    hwc2_error_t error = HWC2_ERROR_NONE;

    int acquireFenceFd = HWCNativeBufferGetFence(buffer);
    int syncBeforeSet = 1;

    if (syncBeforeSet && acquireFenceFd >= 0) {
        sync_wait(acquireFenceFd, -1);
        close(acquireFenceFd);
        acquireFenceFd = -1;
    }

    error = hwc2_compat_display_validate(m_display, &numTypes, &numRequests);
    if (error != HWC2_ERROR_NONE && error != HWC2_ERROR_HAS_CHANGES) {
        qDebug("prepare: validate failed for display %d: %d", displayId, error);
        return;
    }

    if (numTypes || numRequests) {
        qDebug("prepare: validate required changes for display %d: %d", displayId, error);
        return;
    }

    error = hwc2_compat_display_accept_changes(m_display);
    if (error != HWC2_ERROR_NONE) {
        qDebug("prepare: acceptChanges failed: %d", error);
        return;
    }

    hwc2_compat_display_set_client_target(m_display, /* slot */ 0, buffer,
                                          acquireFenceFd,
                                          HAL_DATASPACE_UNKNOWN);

    int presentFence = -1;
    hwc2_compat_display_present(m_display, &presentFence);

    if (lastPresentFence != -1) {
        sync_wait(lastPresentFence, -1);
        close(lastPresentFence);
    }

    lastPresentFence = presentFence != -1 ? dup(presentFence) : -1;

    HWCNativeBufferSetFence(buffer, presentFence);
}

}  // namespace KWin
