/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "hwcomposer_backend.h"

#include "composite.h"
#include "egl_hwcomposer_backend.h"
#include "logging.h"
#include "main.h"
#include "scene.h"
#include "session.h"

//#include "screens_hwcomposer.h"
#include "wayland_server.h"
// KWayland
#include <KWaylandServer/output_interface.h>
#include <KWaylandServer/seat_interface.h>
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
#include "renderloop_p.h"
#include "composite.h"
// based on test_hwcomposer.c from libhybris project (Apache 2 licensed)

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

HwcomposerBackend::HwcomposerBackend(QObject *parent)
    : Platform(parent)
    , m_session(Session::create(this))
{
    setPerScreenRenderingEnabled(true);
    supportsOutputChanges();

    if (!QDBusConnection::sessionBus().connect(QStringLiteral("org.kde.Solid.PowerManagement"),
                                              QStringLiteral("/org/kde/Solid/PowerManagement/Actions/BrightnessControl"),
                                              QStringLiteral("org.kde.Solid.PowerManagement.Actions.BrightnessControl"),
                                              QStringLiteral("brightnessChanged"), this,
                                              SLOT(screenBrightnessChanged(int)))) {
        qCWarning(KWIN_HWCOMPOSER) << "Failed to connect to brightness control";
    }
}

HwcomposerBackend::~HwcomposerBackend()
{
    if (sceneEglDisplay() != EGL_NO_DISPLAY) {
        eglTerminate(sceneEglDisplay());
    }
}

Session *HwcomposerBackend::session() const
{
    return m_session;
}

void HwcomposerBackend::toggleBlankOutput()
{
    if (!m_hwc2device) {
        return;
    }
    m_outputBlank = !m_outputBlank;
    toggleScreenBrightness();
    enableVSync(!m_outputBlank);

    hwc2_compat_display_set_power_mode(m_hwc2_primary_display, m_outputBlank ? HWC2_POWER_MODE_OFF : HWC2_POWER_MODE_ON);

    // enable/disable compositor repainting when blanked
    if (!m_output.isNull()) m_output.data()->setEnabled(!m_outputBlank);
    if (Compositor *compositor = Compositor::self()) {
        if (!m_outputBlank) {
            compositor->scene()->addRepaintFull();
        }
    }
    if (m_outputBlank){
        m_filter.reset(new BacklightInputEventFilter(this));
        input()->prependInputEventFilter(m_filter.data());
    } else m_filter.reset();
    Q_EMIT outputBlankChanged();
}

void HwcomposerBackend::toggleScreenBrightness()
{
    if (!m_lights) {
        return;
    }
    const int brightness = m_outputBlank ? 0 : m_oldScreenBrightness;
    struct light_state_t state;
    state.flashMode = LIGHT_FLASH_NONE;
    state.brightnessMode = BRIGHTNESS_MODE_USER;

    state.color = (int)((0xffU << 24) | (brightness << 16) |
                        (brightness << 8) | brightness);
    m_lights->set_light(m_lights, &state);
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
    hw_module_t *hwcModule = nullptr;
    if (hw_get_module(HWC_HARDWARE_MODULE_ID, (const hw_module_t **)&hwcModule) != 0) {
        qCWarning(KWIN_HWCOMPOSER) << "Failed to get hwcomposer module";
        return false;
    }
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
    initLights();
    toggleBlankOutput();

    // get display configuration
    m_output.reset(new HwcomposerOutput(this, m_hwc2_primary_display));
    if (!m_output->isValid()) {
        return false;
    }

    if (m_output->refreshRate() != 0) {
        m_vsyncInterval = 1000000/m_output->refreshRate();
    }

    m_output->setDpmsMode(HwcomposerOutput::DpmsMode::On);

    if (m_lights) {
        auto updateDpms = [this] {
            if (m_output) {
                m_output->setDpmsModeInternal(m_outputBlank ? HwcomposerOutput::DpmsMode::Off : HwcomposerOutput::DpmsMode::On);
            }
        };
        connect(this, &HwcomposerBackend::outputBlankChanged, this, updateDpms);

        connect(m_output.data(), &HwcomposerOutput::dpmsModeRequested, this,
            [this] (HwcomposerOutput::DpmsMode mode) {
                if (mode == HwcomposerOutput::DpmsMode::On) {
                    if (m_outputBlank) {
                        toggleBlankOutput();
                    } else Q_EMIT outputBlankChanged();
                } else {
                    if (!m_outputBlank) {
                        toggleBlankOutput();
                    } else Q_EMIT outputBlankChanged();
                }
            }
        );
    }

    Q_EMIT outputAdded(m_output.data());
    Q_EMIT outputEnabled(m_output.data());

    setReady(true);
    Q_EMIT screensQueried();

    return true;
}

void HwcomposerBackend::initLights()
{
    hw_module_t *lightsModule = nullptr;
    if (hw_get_module(LIGHTS_HARDWARE_MODULE_ID, (const hw_module_t **)&lightsModule) != 0) {
        qCWarning(KWIN_HWCOMPOSER) << "Failed to get lights module";
        return;
    }
    light_device_t *lightsDevice = nullptr;
    if (lightsModule->methods->open(lightsModule, LIGHT_ID_BACKLIGHT, (hw_device_t **)&lightsDevice) != 0) {
        qCWarning(KWIN_HWCOMPOSER) << "Failed to create lights device";
        return;
    }
    m_lights = lightsDevice;
}

InputBackend *HwcomposerBackend::createInputBackend()
{
    return new LibinputBackend(this);
}

QSize HwcomposerBackend::size() const
{
    if (m_output) {
        return m_output->pixelSize();
    }
    return QSize();
}

QSize HwcomposerBackend::screenSize() const
{
    if (m_output) {
        return m_output->pixelSize() / m_output->scale();
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
    if (!m_output.isNull()) {
        return QVector<HwcomposerOutput *>({m_output.data()});
    }
    return {};
}

Outputs HwcomposerBackend::enabledOutputs() const
{
    return outputs();
}

void HwcomposerBackend::updateOutputsEnabled()
{}

bool HwcomposerBackend::updateOutputs()
{
    updateOutputsEnabled();
    Q_EMIT screensQueried();

    return true;
}
OpenGLBackend *HwcomposerBackend::createOpenGLBackend()
{
    return new EglHwcomposerBackend(this);
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

HwcomposerWindow::HwcomposerWindow(HwcomposerBackend *backend) //! [dba debug: 2021-06-18]
    : HWComposerNativeWindow( backend->size().width(),  backend->size().height(), HAL_PIXEL_FORMAT_RGBA_8888), m_backend(backend)
{
    setBufferCount(3);
    m_hwc2_primary_display = m_backend->hwc2_display();
    hwc2_compat_layer_t *layer = hwc2_compat_display_create_layer(m_hwc2_primary_display);
    hwc2_compat_layer_set_composition_type(layer, HWC2_COMPOSITION_CLIENT);
    hwc2_compat_layer_set_blend_mode(layer, HWC2_BLEND_MODE_NONE);
    hwc2_compat_layer_set_transform(layer, HWC_TRANSFORM_ROT_90);

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
    : AbstractWaylandOutput(), m_renderLoop(new RenderLoop(this)), m_hwc2_primary_display(hwc2_primary_display), m_backend(backend)
{
    int32_t attr_values[5];
    HWC2DisplayConfig *config = hwc2_compat_display_get_active_config(hwc2_primary_display);
    Q_ASSERT(config);
    attr_values[0] = config->width;
    attr_values[1] = config->height;
    attr_values[2] = config->dpiX;
    attr_values[3] = config->dpiY;
    attr_values[4] = config->vsyncPeriod;

    if (attr_values[0] == 2072){
        attr_values[4] = 20000000;
    }else{
        attr_values[4] = config->vsyncPeriod;
    }

    QString debugWidth = qgetenv("KWIN_DEBUG_WIDTH");
    if (!debugWidth.isEmpty()) {
        attr_values[0] = debugWidth.toInt();
    }
    QString debugHeight = qgetenv("KWIN_DEBUG_HEIGHT");
    if (!debugHeight.isEmpty()) {
        attr_values[1] = debugHeight.toInt();
    }
    QSize pixelSize(attr_values[0], attr_values[1]);

    if (pixelSize.isEmpty()) {
        return;
    }

    QSizeF physicalSize = pixelSize / 3.8;
    if (attr_values[2] != 0 && attr_values[3] != 0) {
        static const qreal factor = 25.4;
        physicalSize = QSizeF(qreal(pixelSize.width() * 1000) / qreal(attr_values[2]) * factor,
                              qreal(pixelSize.height() * 1000) / qreal(attr_values[3]) * factor);
    }

    QString debugDpi = qgetenv("KWIN_DEBUG_DPI");
    if (!debugDpi.isEmpty()) {
        if (debugDpi.toFloat() != 0) {
            physicalSize = pixelSize / debugDpi.toFloat();
        }
    }

    // read in mode information
    QVector<Mode> modes;
    {
        ModeFlags deviceflags = 0;
        deviceflags |= ModeFlag::Current;
        deviceflags |= ModeFlag::Preferred;

        Mode mode;
        mode.id = 0;
        mode.size = QSize(attr_values[0], attr_values[1]);
        mode.flags = deviceflags;
        mode.refreshRate = (attr_values[4] == 0) ? 60000 : 10E11 / attr_values[4];
        modes << mode;
    }
    initialize(QString(), QString(), QString(), QString(), physicalSize.toSize(), modes, {});
    setInternal(true);
    setCapabilityInternal(HwcomposerOutput::Capability::Dpms);


    const auto outputGroup = kwinApp()->config()->group("HWComposerOutputs").group("0");
    setCurrentModeInternal(pixelSize, modes[0].refreshRate);

    const qreal dpi = modeSize().height() / (physicalSize.height() / 25.4);
    KConfig _cfgfonts(QStringLiteral("kcmfonts"));
    KConfigGroup cfgfonts(&_cfgfonts, "General");
    qDebug() << Q_FUNC_INFO << "set default xft dpi: modeSize:" << modeSize() << "physicalSize:" << physicalSize << "dpi:" << dpi;
    cfgfonts.writeEntry("defaultXftDpi", 192);
    setScale(1.0);

    QString debugScale = qgetenv("KWIN_DEBUG_SCALE");
    if (!debugScale.isEmpty()) {
        setScale(outputGroup.readEntry("Scale", debugScale.toFloat()));
    }
}

HwcomposerOutput::~HwcomposerOutput()
{
    if (m_hwc2_primary_display != NULL) {
        free(m_hwc2_primary_display);
    }
}

void HwcomposerOutput::setEnabled(bool enable) 
{
    m_isEnabled = enable;
}

bool HwcomposerOutput::isEnabled() const
{
    return m_isEnabled;
}

RenderLoop *HwcomposerOutput::renderLoop() const
{
    return m_renderLoop;
}

bool HwcomposerOutput::isValid() const
{
    return isEnabled();
}

void HwcomposerOutput::setDpmsMode(DpmsMode mode)
{
    Q_EMIT dpmsModeRequested(mode);
}

}  // namespace KWin
