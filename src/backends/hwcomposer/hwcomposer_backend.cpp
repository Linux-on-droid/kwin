/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "hwcomposer_backend.h"

#include "composite.h"
#include "hwcomposer_egl_backend.h"
#include "hwcomposer_logging.h"
#include "main.h"
#include "scene/workspacescene.h"
#include "backends/libinput/libinputbackend.h"
//#include "screens_hwcomposer.h"
#include "wayland_server.h"
// KWayland
#include "wayland/output_interface.h"
#include "wayland/seat_interface.h"
// KDE
#include <KConfigGroup>
// Qt
#include <QDBusConnection>
#include <QKeyEvent>
// hybris/android
#include <android-config.h>
#include <hardware/hardware.h>
#include <hardware/lights.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>
// linux
#include <linux/input.h>
#include <sync/sync.h>

#include <QDBusError>
#include <QtConcurrent>
#include <QDBusMessage>
#include "core/renderloop_p.h"
#include "composite.h"
// based on test_hwcomposer.c from libhybris project (Apache 2 licensed)

#include "core/output.h"
#include "core/outputconfiguration.h"

using namespace KWaylandServer;

namespace KWin {

BacklightInputEventFilter::BacklightInputEventFilter(HwcomposerBackend *backend)
    : InputEventFilter()
    , m_backend(backend)
{
}

BacklightInputEventFilter::~BacklightInputEventFilter() = default;

bool BacklightInputEventFilter::pointerEvent(QMouseEvent *event, quint32 nativeButton)
{
    Q_UNUSED(event)
    Q_UNUSED(nativeButton)
    if (!m_backend->isBacklightOff()) {
        return false;
    }
    toggleBacklight();
    return true;
}

bool BacklightInputEventFilter::wheelEvent(QWheelEvent *event)
{
    Q_UNUSED(event)
    if (!m_backend->isBacklightOff()) {
        return false;
    }
    toggleBacklight();
    return true;
}

bool BacklightInputEventFilter::keyEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_PowerOff && event->type() == QEvent::KeyRelease) {
        toggleBacklight();
    }
    return true;
}

bool BacklightInputEventFilter::touchDown(qint32 id, const QPointF &pos, quint32 time)
{
    Q_UNUSED(pos)
    Q_UNUSED(time)
    if (!m_backend->isBacklightOff()) {
        return false;
    }
    if (m_touchPoints.isEmpty()) {
        if (!m_doubleTapTimer.isValid()) {
            // this is the first tap
            m_doubleTapTimer.start();
        } else {
            if (m_doubleTapTimer.elapsed() < qApp->doubleClickInterval()) {
                m_secondTap = true;
            } else {
                // took too long. Let's consider it a new click
                m_doubleTapTimer.restart();
            }
        }
    } else {
        // not a double tap
        m_doubleTapTimer.invalidate();
        m_secondTap = false;
    }
    m_touchPoints << id;
    return true;
}

bool BacklightInputEventFilter::touchUp(qint32 id, quint32 time)
{
    Q_UNUSED(time)
    m_touchPoints.removeAll(id);
    if (!m_backend->isBacklightOff()) {
        return false;
    }
    if (m_touchPoints.isEmpty() && m_doubleTapTimer.isValid() && m_secondTap) {
        if (m_doubleTapTimer.elapsed() < qApp->doubleClickInterval()) {
            toggleBacklight();
        }
        m_doubleTapTimer.invalidate();
        m_secondTap = false;
    }
    return true;
}

bool BacklightInputEventFilter::touchMotion(qint32 id, const QPointF &pos, quint32 time)
{
    Q_UNUSED(id)
    Q_UNUSED(pos)
    Q_UNUSED(time)
    return m_backend->isBacklightOff();
}

void BacklightInputEventFilter::toggleBacklight()
{
    // queued to not modify the list of event filters while filtering
    QMetaObject::invokeMethod(m_backend, "toggleBlankOutput", Qt::QueuedConnection);
}

HwcomposerBackend::HwcomposerBackend(Session *session, QObject *parent)
    : OutputBackend(parent)
    , m_session(session)
{
}

Session *HwcomposerBackend::session() const
{
    return m_session;
}

HwcomposerBackend::~HwcomposerBackend()
{
    if (sceneEglDisplay() != EGL_NO_DISPLAY) {
        eglTerminate(sceneEglDisplay());
    }
}

void HwcomposerBackend::toggleBlankOutput()
{
    if (!m_hwc2device) {
        return;
    }
    m_outputBlank = !m_outputBlank;
    enableVSync(!m_outputBlank);

    hwc2_compat_display_set_power_mode(m_hwc2_primary_display, m_outputBlank ? HWC2_POWER_MODE_OFF : HWC2_POWER_MODE_ON);

    // enable/disable compositor repainting when blanked
    if (m_output != nullptr) m_output.get()->updateEnabled(!m_outputBlank);
    if (Compositor *compositor = Compositor::self()) {
        if (!m_outputBlank) {
            compositor->scene()->addRepaintFull();
        }
    }
    if (m_outputBlank){
        m_filter.reset(new BacklightInputEventFilter(this));
        input()->prependInputEventFilter(m_filter.get());
    } else m_filter.reset();
    Q_EMIT outputBlankChanged();
}

typedef struct : public HWC2EventListener
{
    HwcomposerBackend *backend = nullptr;
} HwcProcs_v20;

void hwc2_callback_vsync(HWC2EventListener *listener, int32_t sequenceId,
                         hwc2_display_t display, int64_t timestamp)
{
    static_cast<const HwcProcs_v20 *>(listener)->backend->wakeVSync();
}

void hwc2_callback_hotplug(HWC2EventListener *listener, int32_t sequenceId,
                           hwc2_display_t display, bool connected,
                           bool primaryDisplay)
{
    hwc2_compat_device_on_hotplug(static_cast<const HwcProcs_v20 *>(listener)->backend->hwc2_device(), display, connected);
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

bool HwcomposerBackend::initialize()
{
    m_hwc2device = hwc2_compat_device_new(false);

    RegisterCallbacks();
    for (int i = 0; i < 5 * 1000; ++i) {
        // Wait at most 5s for hotplug events
        if ((m_hwc2_primary_display =
                hwc2_compat_device_get_display_by_id(m_hwc2device, 0)))
        break;
        usleep(1000);
    }

    //move to HwcomposerOutput + signal
    toggleBlankOutput();

    // get display configuration
    m_output.reset(new HwcomposerOutput(this, m_hwc2_primary_display));
    if (!m_output->isValid()) {
        return false;
    }

    if (m_output->refreshRate() != 0) {
        m_vsyncInterval = 1000000/m_output->refreshRate();
    }

    m_output->updateDpmsMode(HwcomposerOutput::DpmsMode::On);

    Q_EMIT outputAdded(m_output.get());
    m_output.get()->updateEnabled(true);

    Q_EMIT outputsQueried();

    return true;
}

void HwcomposerBackend::updateOutputState(hwc2_display_t display) {
    if (m_output != nullptr) {
        m_output.get()->setStatesInternal();
        Q_EMIT outputsQueried();
    }
}

std::unique_ptr<InputBackend>HwcomposerBackend::createInputBackend()
{
    return std::make_unique<LibinputBackend>(m_session);
}

QSize HwcomposerBackend::size() const
{
    if (m_output) {
        return m_output->pixelSize();
    }
    return QSize();
}

int HwcomposerBackend::scale() const
{
    if (m_output) {
        return m_output->scale();
    }
    return 1;
}

void HwcomposerBackend::enableVSync(bool enable)
{
    if (m_hasVsync == enable) {
        return;
    }
    hwc2_compat_display_set_vsync_enabled(m_hwc2_primary_display, enable ? HWC2_VSYNC_ENABLE : HWC2_VSYNC_DISABLE);
    m_hasVsync = enable;
}

HwcomposerWindow *HwcomposerBackend::createSurface()
{
    return new HwcomposerWindow(this);
}

Outputs HwcomposerBackend::outputs() const
{
    if (m_output != nullptr) {
        return QVector<HwcomposerOutput *>({m_output.get()});
    }
    return {};
}

std::unique_ptr<OpenGLBackend> HwcomposerBackend::createOpenGLBackend()
{
    return std::make_unique<EglHwcomposerBackend>(this);
}

void HwcomposerBackend::waitVSync()
{
    if (!m_hasVsync) {
        return;
    }
    m_vsyncMutex.lock();
    m_vsyncWaitCondition.wait(&m_vsyncMutex, m_vsyncInterval);
    m_vsyncMutex.unlock();
}

void HwcomposerBackend::compositing(int flags)
{
    m_compositingSemaphore.release();
    if(flags > 0){
        RenderLoopPrivate *renderLoopPrivate = RenderLoopPrivate::get(m_output->renderLoop());
        if(renderLoopPrivate->pendingFrameCount > 0){
            renderLoopPrivate->notifyFrameCompleted(std::chrono::steady_clock::now().time_since_epoch());
        }
    }
    m_compositingSemaphore.acquire();
}

void HwcomposerBackend::wakeVSync()
{
    int flags = 1;
    if (m_compositingSemaphore.available() > 0) {
        flags = 0;
    }
    QMetaObject::invokeMethod(this, "compositing", Qt::QueuedConnection, Q_ARG(int, flags));
    m_vsyncMutex.lock();
    m_vsyncWaitCondition.wakeAll();
    m_vsyncMutex.unlock();
}

HwcomposerWindow::HwcomposerWindow(HwcomposerBackend *backend)
    : HWComposerNativeWindow( backend->size().width(),  backend->size().height(), HAL_PIXEL_FORMAT_RGBA_8888), m_backend(backend)
{
    m_hwc2_primary_display = m_backend->hwc2_display();
    hwc2_compat_layer_t *layer = hwc2_compat_display_create_layer(m_hwc2_primary_display);
    hwc2_compat_layer_set_composition_type(layer, HWC2_COMPOSITION_CLIENT);
    hwc2_compat_layer_set_blend_mode(layer, HWC2_BLEND_MODE_NONE);

    hwc2_compat_layer_set_source_crop(layer, 0.0f, 0.0f, m_backend->size().width(), m_backend->size().height());
    hwc2_compat_layer_set_display_frame(layer, 0, 0, m_backend->size().width(), m_backend->size().height());
    hwc2_compat_layer_set_visible_region(layer, 0, 0, m_backend->size().width(), m_backend->size().height());
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
    int displayId = 0;
    hwc2_error_t error = HWC2_ERROR_NONE;

    int acquireFenceFd = HWCNativeBufferGetFence(buffer);
    int syncBeforeSet = 1;

    if (syncBeforeSet && acquireFenceFd >= 0) {
        sync_wait(acquireFenceFd, -1);
        close(acquireFenceFd);
        acquireFenceFd = -1;
    }

    hwc2_compat_display_set_power_mode(m_hwc2_primary_display, HWC2_POWER_MODE_ON);
    error = hwc2_compat_display_validate(m_hwc2_primary_display, &numTypes, &numRequests);
    if (error != HWC2_ERROR_NONE && error != HWC2_ERROR_HAS_CHANGES) {
        qDebug("prepare: validate failed for display %d: %d", displayId, error);
        return;
    }

    if (numTypes || numRequests) {
        qDebug("prepare: validate required changes for display %d: %d",displayId, error);
        return;
    }

    error = hwc2_compat_display_accept_changes(m_hwc2_primary_display);
    if (error != HWC2_ERROR_NONE) {
        qDebug("prepare: acceptChanges failed: %d", error);
        return;
    }

    hwc2_compat_display_set_client_target(m_hwc2_primary_display, /* slot */ 0, buffer,
                                            acquireFenceFd,
                                            HAL_DATASPACE_UNKNOWN);

    int presentFence = -1;
    hwc2_compat_display_present(m_hwc2_primary_display, &presentFence);


    if (lastPresentFence != -1) {
        sync_wait(lastPresentFence, -1);
        close(lastPresentFence);
    }

    lastPresentFence = presentFence != -1 ? dup(presentFence) : -1;

    HWCNativeBufferSetFence(buffer, presentFence);
}

bool HwcomposerOutput::hardwareTransforms() const
{
    return false;
}

HwcomposerOutput::HwcomposerOutput(HwcomposerBackend *backend, hwc2_compat_display_t *hwc2_primary_display)
    : Output(backend)
    , m_renderLoop(std::make_unique<RenderLoop>())
    , m_hwc2_primary_display(hwc2_primary_display)
    , m_backend(backend)
{

    HWC2DisplayConfig *config = hwc2_compat_display_get_active_config(m_hwc2_primary_display);
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
        .name = QStringLiteral("hwcomposer"),
        .manufacturer = QStringLiteral("Android"),
        .model = QStringLiteral("Lindroid"),
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

    setStatesInternal();
}


HwcomposerOutput::~HwcomposerOutput()
{
    if (m_hwc2_primary_display != NULL) {
        free(m_hwc2_primary_display);
    }
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

void HwcomposerOutput::setStatesInternal()
{
    // Retrieve and set display configuration attributes
    HWC2DisplayConfig *config = hwc2_compat_display_get_active_config(m_hwc2_primary_display);
    Q_ASSERT(config);

    int32_t width = config->width;
    int32_t height = config->height;
    int32_t dpiX = config->dpiX;
    int32_t dpiY = config->dpiY;
    int32_t vsyncPeriod = (width == 2072) ? 20000000 : config->vsyncPeriod;

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

    QList<std::shared_ptr<OutputMode>> modes;
    OutputMode::Flags modeFlags = OutputMode::Flag::Preferred;
    std::shared_ptr<OutputMode> mode = std::make_shared<OutputMode>(pixelSize, (vsyncPeriod == 0) ? 60000 : 10E11 / vsyncPeriod, modeFlags);
    modes << mode;
    State initialState;
    initialState.modes = modes;
    initialState.currentMode = modes.constFirst();
    initialState.scale = scale;

    setState(initialState);
}

void HwcomposerOutput::updateEnabled(bool enable)
{
    m_isEnabled = enable;
}

bool HwcomposerOutput::isEnabled() const
{
    return m_isEnabled;
}

RenderLoop *HwcomposerOutput::renderLoop() const
{
    return m_renderLoop.get();
}

bool HwcomposerOutput::isValid() const
{
    return isEnabled();
}

void HwcomposerOutput::setDpmsMode(DpmsMode mode)
{

}

void HwcomposerOutput::updateDpmsMode(DpmsMode mode)
{
    Q_EMIT dpmsModeRequested(mode);
}

}  // namespace KWin
