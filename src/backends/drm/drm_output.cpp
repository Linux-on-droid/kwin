/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_output.h"
#include "drm_backend.h"
#include "drm_connector.h"
#include "drm_crtc.h"
#include "drm_gpu.h"
#include "drm_pipeline.h"

#include "core/colortransformation.h"
#include "core/iccprofile.h"
#include "core/outputconfiguration.h"
#include "core/renderbackend.h"
#include "core/renderloop.h"
#include "core/renderloop_p.h"
#include "drm_layer.h"
#include "drm_logging.h"
// Qt
#include <QCryptographicHash>
#include <QMatrix4x4>
#include <QPainter>
// c++
#include <cerrno>
// drm
#include <drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <xf86drm.h>

namespace KWin
{

static const bool s_allowColorspaceIntel = qEnvironmentVariableIntValue("KWIN_DRM_ALLOW_INTEL_COLORSPACE") == 1;
static const bool s_disableTripleBuffering = qEnvironmentVariableIntValue("KWIN_DRM_DISABLE_TRIPLE_BUFFERING") == 1;

DrmOutput::DrmOutput(const std::shared_ptr<DrmConnector> &conn)
    : DrmAbstractOutput(conn->gpu())
    , m_pipeline(conn->pipeline())
    , m_connector(conn)
{
    m_pipeline->setOutput(this);
    m_renderLoop->setRefreshRate(m_pipeline->mode()->refreshRate());
    if (m_gpu->atomicModeSetting() && !s_disableTripleBuffering) {
        m_renderLoop->setMaxPendingFrameCount(2);
    }

    Capabilities capabilities = Capability::Dpms | Capability::IccProfile;
    State initialState;

    if (conn->overscan.isValid() || conn->underscan.isValid()) {
        capabilities |= Capability::Overscan;
        initialState.overscan = conn->overscan.isValid() ? conn->overscan.value() : conn->underscanVBorder.value();
    }
    if (conn->vrrCapable.isValid() && conn->vrrCapable.value()) {
        capabilities |= Capability::Vrr;
    }
    if (gpu()->asyncPageflipSupported()) {
        capabilities |= Capability::Tearing;
    }
    if (conn->broadcastRGB.isValid()) {
        capabilities |= Capability::RgbRange;
        initialState.rgbRange = DrmConnector::broadcastRgbToRgbRange(conn->broadcastRGB.enumValue());
    }
    if (m_connector->hdrMetadata.isValid() && m_connector->edid()->supportsPQ()) {
        capabilities |= Capability::HighDynamicRange;
    }
    if (m_connector->colorspace.isValid() && m_connector->colorspace.hasEnum(DrmConnector::Colorspace::BT2020_RGB) && m_connector->edid()->supportsBT2020()) {
        if (!m_gpu->isI915() || s_allowColorspaceIntel) {
            capabilities |= Capability::WideColorGamut;
        }
    }
    if (conn->isInternal()) {
        // TODO only set this if an orientation sensor is available?
        capabilities |= Capability::AutoRotation;
    }

    const Edid *edid = conn->edid();
    setInformation(Information{
        .name = conn->connectorName(),
        .manufacturer = edid->manufacturerString(),
        .model = conn->modelName(),
        .serialNumber = edid->serialNumber(),
        .eisaId = edid->eisaId(),
        .physicalSize = conn->physicalSize(),
        .edid = *edid,
        .subPixel = conn->subpixel(),
        .capabilities = capabilities,
        .panelOrientation = conn->panelOrientation.isValid() ? DrmConnector::toKWinTransform(conn->panelOrientation.enumValue()) : OutputTransform::Normal,
        .internal = conn->isInternal(),
        .nonDesktop = conn->isNonDesktop(),
        .mstPath = conn->mstPath(),
        .maxPeakBrightness = edid->desiredMaxLuminance(),
        .maxAverageBrightness = edid->desiredMaxFrameAverageLuminance(),
        .minBrightness = edid->desiredMinLuminance(),
    });

    initialState.modes = getModes();
    initialState.currentMode = m_pipeline->mode();
    if (!initialState.currentMode) {
        initialState.currentMode = initialState.modes.constFirst();
    }

    setState(initialState);

    m_turnOffTimer.setSingleShot(true);
    m_turnOffTimer.setInterval(dimAnimationTime());
    connect(&m_turnOffTimer, &QTimer::timeout, this, [this] {
        if (!setDrmDpmsMode(DpmsMode::Off)) {
            // in case of failure, undo aboutToTurnOff() from setDpmsMode()
            Q_EMIT wakeUp();
        }
    });
}

DrmOutput::~DrmOutput()
{
    m_pipeline->setOutput(nullptr);
}

bool DrmOutput::addLeaseObjects(QList<uint32_t> &objectList)
{
    if (!m_pipeline->crtc()) {
        qCWarning(KWIN_DRM) << "Can't lease connector: No suitable crtc available";
        return false;
    }
    qCDebug(KWIN_DRM) << "adding connector" << m_pipeline->connector()->id() << "to lease";
    objectList << m_pipeline->connector()->id();
    objectList << m_pipeline->crtc()->id();
    if (m_pipeline->crtc()->primaryPlane()) {
        objectList << m_pipeline->crtc()->primaryPlane()->id();
    }
    return true;
}

void DrmOutput::leased(DrmLease *lease)
{
    m_lease = lease;
}

void DrmOutput::leaseEnded()
{
    qCDebug(KWIN_DRM) << "ended lease for connector" << m_pipeline->connector()->id();
    m_lease = nullptr;
}

DrmLease *DrmOutput::lease() const
{
    return m_lease;
}

bool DrmOutput::updateCursorLayer()
{
    return m_pipeline->updateCursor();
}

QList<std::shared_ptr<OutputMode>> DrmOutput::getModes() const
{
    const auto drmModes = m_pipeline->connector()->modes();

    QList<std::shared_ptr<OutputMode>> ret;
    ret.reserve(drmModes.count());
    for (const auto &drmMode : drmModes) {
        ret.append(drmMode);
    }
    return ret;
}

void DrmOutput::setDpmsMode(DpmsMode mode)
{
    if (mode == DpmsMode::Off) {
        if (!m_turnOffTimer.isActive()) {
            Q_EMIT aboutToTurnOff(std::chrono::milliseconds(m_turnOffTimer.interval()));
            m_turnOffTimer.start();
        }
    } else {
        if (m_turnOffTimer.isActive() || (mode != dpmsMode() && setDrmDpmsMode(mode))) {
            Q_EMIT wakeUp();
        }
        m_turnOffTimer.stop();
    }
}

bool DrmOutput::setDrmDpmsMode(DpmsMode mode)
{
    if (!isEnabled()) {
        return false;
    }
    bool active = mode == DpmsMode::On;
    bool isActive = dpmsMode() == DpmsMode::On;
    if (active == isActive) {
        updateDpmsMode(mode);
        return true;
    }
    if (!active) {
        gpu()->waitIdle();
    }
    m_pipeline->setActive(active);
    if (DrmPipeline::commitPipelines({m_pipeline}, active ? DrmPipeline::CommitMode::TestAllowModeset : DrmPipeline::CommitMode::CommitModeset) == DrmPipeline::Error::None) {
        m_pipeline->applyPendingChanges();
        updateDpmsMode(mode);
        if (active) {
            m_renderLoop->uninhibit();
            m_renderLoop->scheduleRepaint();
            doSetChannelFactors(m_channelFactors);
        } else {
            m_renderLoop->inhibit();
        }
        return true;
    } else {
        qCWarning(KWIN_DRM) << "Setting dpms mode failed!";
        m_pipeline->revertPendingChanges();
        return false;
    }
}

DrmPlane::Transformations outputToPlaneTransform(OutputTransform transform)
{
    using PlaneTrans = DrmPlane::Transformation;

    switch (transform.kind()) {
    case OutputTransform::Normal:
        return PlaneTrans::Rotate0;
    case OutputTransform::FlipX:
        return PlaneTrans::ReflectX | PlaneTrans::Rotate0;
    case OutputTransform::Rotate90:
        return PlaneTrans::Rotate90;
    case OutputTransform::FlipX90:
        return PlaneTrans::ReflectX | PlaneTrans::Rotate90;
    case OutputTransform::Rotate180:
        return PlaneTrans::Rotate180;
    case OutputTransform::FlipX180:
        return PlaneTrans::ReflectX | PlaneTrans::Rotate180;
    case OutputTransform::Rotate270:
        return PlaneTrans::Rotate270;
    case OutputTransform::FlipX270:
        return PlaneTrans::ReflectX | PlaneTrans::Rotate270;
    default:
        Q_UNREACHABLE();
    }
}

void DrmOutput::updateModes()
{
    State next = m_state;
    next.modes = getModes();

    if (m_pipeline->crtc()) {
        const auto currentMode = m_pipeline->connector()->findMode(m_pipeline->crtc()->queryCurrentMode());
        if (currentMode != m_pipeline->mode()) {
            // DrmConnector::findCurrentMode might fail
            m_pipeline->setMode(currentMode ? currentMode : m_pipeline->connector()->modes().constFirst());
            if (m_gpu->testPendingConfiguration() == DrmPipeline::Error::None) {
                m_pipeline->applyPendingChanges();
                m_renderLoop->setRefreshRate(m_pipeline->mode()->refreshRate());
            } else {
                qCWarning(KWIN_DRM) << "Setting changed mode failed!";
                m_pipeline->revertPendingChanges();
            }
        }
    }

    next.currentMode = m_pipeline->mode();
    if (!next.currentMode) {
        next.currentMode = next.modes.constFirst();
    }

    setState(next);
}

void DrmOutput::updateDpmsMode(DpmsMode dpmsMode)
{
    State next = m_state;
    next.dpmsMode = dpmsMode;
    setState(next);
}

bool DrmOutput::present(const std::shared_ptr<OutputFrame> &frame)
{
    const bool needsModeset = gpu()->needsModeset();
    bool success;
    if (needsModeset) {
        m_pipeline->setPresentationMode(PresentationMode::VSync);
        m_pipeline->setContentType(DrmConnector::DrmContentType::Graphics);
        success = m_pipeline->maybeModeset(frame);
    } else {
        m_pipeline->setPresentationMode(frame->presentationMode());
        DrmPipeline::Error err = m_pipeline->present(frame);
        if (err != DrmPipeline::Error::None && frame->presentationMode() != PresentationMode::VSync) {
            // retry with a more basic presentation mode
            m_pipeline->setPresentationMode(PresentationMode::VSync);
            err = m_pipeline->present(frame);
        }
        success = err == DrmPipeline::Error::None;
        if (err == DrmPipeline::Error::InvalidArguments) {
            QTimer::singleShot(0, m_gpu->platform(), &DrmBackend::updateOutputs);
        }
    }
    m_renderLoop->setPresentationMode(m_pipeline->presentationMode());
    if (success) {
        Q_EMIT outputChange(frame->damage());
        return true;
    } else if (!needsModeset) {
        qCWarning(KWIN_DRM) << "Presentation failed!" << strerror(errno);
    }
    return false;
}

DrmConnector *DrmOutput::connector() const
{
    return m_connector.get();
}

DrmPipeline *DrmOutput::pipeline() const
{
    return m_pipeline;
}

bool DrmOutput::queueChanges(const std::shared_ptr<OutputChangeSet> &props)
{
    const auto mode = props->mode.value_or(currentMode()).lock();
    if (!mode) {
        return false;
    }
    const bool bt2020 = props->wideColorGamut.value_or(m_state.wideColorGamut);
    const bool hdr = props->highDynamicRange.value_or(m_state.highDynamicRange);
    m_pipeline->setMode(std::static_pointer_cast<DrmConnectorMode>(mode));
    m_pipeline->setOverscan(props->overscan.value_or(m_pipeline->overscan()));
    m_pipeline->setRgbRange(props->rgbRange.value_or(m_pipeline->rgbRange()));
    m_pipeline->setEnable(props->enabled.value_or(m_pipeline->enabled()));
    m_pipeline->setColorDescription(createColorDescription(props));
    if (bt2020 || hdr) {
        // ICC profiles don't support HDR (yet)
        m_pipeline->setIccProfile(nullptr);
    } else {
        m_pipeline->setIccProfile(props->iccProfile.value_or(m_state.iccProfile));
    }
    if (bt2020 || hdr || props->colorProfileSource.value_or(m_state.colorProfileSource) != ColorProfileSource::sRGB) {
        // remove unused gamma ramp and ctm, if present
        m_pipeline->setGammaRamp(nullptr);
        m_pipeline->setCTM(QMatrix3x3{});
    }
    return true;
}

ColorDescription DrmOutput::createColorDescription(const std::shared_ptr<OutputChangeSet> &props) const
{
    const auto colorSource = props->colorProfileSource.value_or(colorProfileSource());
    const bool hdr = props->highDynamicRange.value_or(m_state.highDynamicRange);
    const bool wcg = props->wideColorGamut.value_or(m_state.wideColorGamut);
    const auto iccProfile = props->iccProfile.value_or(m_state.iccProfile);
    if (colorSource == ColorProfileSource::ICC && !hdr && !wcg && iccProfile) {
        const double brightness = iccProfile->brightness().value_or(200);
        return ColorDescription(iccProfile->colorimetry(), NamedTransferFunction::gamma22, brightness, 0, brightness, brightness);
    }
    const bool screenSupportsHdr = m_connector->edid()->isValid() && m_connector->edid()->supportsBT2020() && m_connector->edid()->supportsPQ();
    const bool driverSupportsHdr = m_connector->colorspace.isValid() && m_connector->hdrMetadata.isValid() && (m_connector->colorspace.hasEnum(DrmConnector::Colorspace::BT2020_RGB) || m_connector->colorspace.hasEnum(DrmConnector::Colorspace::BT2020_YCC));
    const bool effectiveHdr = hdr && screenSupportsHdr && driverSupportsHdr;
    const bool effectiveWcg = wcg && screenSupportsHdr && driverSupportsHdr;
    const Colorimetry nativeColorimetry = m_information.edid.colorimetry().value_or(Colorimetry::fromName(NamedColorimetry::BT709));

    const Colorimetry colorimetry = effectiveWcg ? Colorimetry::fromName(NamedColorimetry::BT2020) : (colorSource == ColorProfileSource::EDID ? nativeColorimetry : Colorimetry::fromName(NamedColorimetry::BT709));
    const Colorimetry sdrColorimetry = effectiveWcg ? Colorimetry::fromName(NamedColorimetry::BT709).interpolateGamutTo(nativeColorimetry, props->sdrGamutWideness.value_or(m_state.sdrGamutWideness)) : Colorimetry::fromName(NamedColorimetry::BT709);
    // TODO the EDID can contain a gamma value, use that when available and colorSource == ColorProfileSource::EDID
    const NamedTransferFunction transferFunction = effectiveHdr ? NamedTransferFunction::PerceptualQuantizer : NamedTransferFunction::gamma22;
    const double minBrightness = effectiveHdr ? props->minBrightnessOverride.value_or(m_state.minBrightnessOverride).value_or(m_connector->edid()->desiredMinLuminance()) : 0;
    const double maxAverageBrightness = effectiveHdr ? props->maxAverageBrightnessOverride.value_or(m_state.maxAverageBrightnessOverride).value_or(m_connector->edid()->desiredMaxFrameAverageLuminance().value_or(m_state.sdrBrightness)) : 200;
    const double maxPeakBrightness = effectiveHdr ? props->maxPeakBrightnessOverride.value_or(m_state.maxPeakBrightnessOverride).value_or(m_connector->edid()->desiredMaxLuminance().value_or(1000)) : 200;
    const double sdrBrightness = effectiveHdr ? props->sdrBrightness.value_or(m_state.sdrBrightness) : maxPeakBrightness;
    return ColorDescription(colorimetry, transferFunction, sdrBrightness, minBrightness, maxAverageBrightness, maxPeakBrightness, sdrColorimetry);
}

void DrmOutput::applyQueuedChanges(const std::shared_ptr<OutputChangeSet> &props)
{
    if (!m_connector->isConnected()) {
        return;
    }
    Q_EMIT aboutToChange(props.get());
    m_pipeline->applyPendingChanges();

    State next = m_state;
    next.enabled = props->enabled.value_or(m_state.enabled) && m_pipeline->crtc();
    next.position = props->pos.value_or(m_state.position);
    next.scale = props->scale.value_or(m_state.scale);
    next.transform = props->transform.value_or(m_state.transform);
    next.manualTransform = props->manualTransform.value_or(m_state.manualTransform);
    next.currentMode = m_pipeline->mode();
    next.overscan = m_pipeline->overscan();
    next.rgbRange = m_pipeline->rgbRange();
    next.highDynamicRange = props->highDynamicRange.value_or(m_state.highDynamicRange);
    next.sdrBrightness = props->sdrBrightness.value_or(m_state.sdrBrightness);
    next.wideColorGamut = props->wideColorGamut.value_or(m_state.wideColorGamut);
    next.autoRotatePolicy = props->autoRotationPolicy.value_or(m_state.autoRotatePolicy);
    next.maxPeakBrightnessOverride = props->maxPeakBrightnessOverride.value_or(m_state.maxPeakBrightnessOverride);
    next.maxAverageBrightnessOverride = props->maxAverageBrightnessOverride.value_or(m_state.maxAverageBrightnessOverride);
    next.minBrightnessOverride = props->minBrightnessOverride.value_or(m_state.minBrightnessOverride);
    next.sdrGamutWideness = props->sdrGamutWideness.value_or(m_state.sdrGamutWideness);
    next.iccProfilePath = props->iccProfilePath.value_or(m_state.iccProfilePath);
    next.iccProfile = props->iccProfile.value_or(m_state.iccProfile);
    next.colorDescription = m_pipeline->colorDescription();
    next.vrrPolicy = props->vrrPolicy.value_or(m_state.vrrPolicy);
    next.colorProfileSource = props->colorProfileSource.value_or(m_state.colorProfileSource);
    next.brightness = props->brightness.value_or(m_state.brightness);
    next.desiredModeSize = props->desiredModeSize.value_or(m_state.desiredModeSize);
    next.desiredModeRefreshRate = props->desiredModeRefreshRate.value_or(m_state.desiredModeRefreshRate);
    setState(next);

    if (!isEnabled() && m_pipeline->needsModeset()) {
        m_gpu->maybeModeset(nullptr);
    }

    m_renderLoop->setRefreshRate(refreshRate());
    m_renderLoop->scheduleRepaint();

    // re-set the CTM and/or gamma lut, if necessary
    doSetChannelFactors(m_channelFactors);

    Q_EMIT changed();
}

void DrmOutput::revertQueuedChanges()
{
    m_pipeline->revertPendingChanges();
}

DrmOutputLayer *DrmOutput::primaryLayer() const
{
    return m_pipeline->primaryLayer();
}

DrmOutputLayer *DrmOutput::cursorLayer() const
{
    return m_pipeline->cursorLayer();
}

bool DrmOutput::setChannelFactors(const QVector3D &rgb)
{
    return m_channelFactors == rgb || doSetChannelFactors(rgb);
}

bool DrmOutput::doSetChannelFactors(const QVector3D &rgb)
{
    m_renderLoop->scheduleRepaint();
    m_channelFactors = rgb;
    if (m_state.wideColorGamut || m_state.highDynamicRange || m_state.colorProfileSource != ColorProfileSource::sRGB) {
        // the shader "fallback" is always active
        return true;
    }
    if (!m_pipeline->activePending()) {
        return false;
    }
    const auto inGamma22 = ColorDescription::nitsToEncoded(rgb, NamedTransferFunction::gamma22, 1);
    if (m_pipeline->hasCTM()) {
        QMatrix3x3 ctm;
        ctm(0, 0) = inGamma22.x();
        ctm(1, 1) = inGamma22.y();
        ctm(2, 2) = inGamma22.z();
        m_pipeline->setCTM(ctm);
        m_pipeline->setGammaRamp(nullptr);
        if (DrmPipeline::commitPipelines({m_pipeline}, DrmPipeline::CommitMode::Test) == DrmPipeline::Error::None) {
            m_pipeline->applyPendingChanges();
            m_channelFactorsNeedShaderFallback = false;
            return true;
        } else {
            m_pipeline->setCTM(QMatrix3x3());
            m_pipeline->applyPendingChanges();
        }
    }
    if (m_pipeline->hasGammaRamp()) {
        auto lut = ColorTransformation::createScalingTransform(inGamma22);
        if (lut) {
            m_pipeline->setGammaRamp(std::move(lut));
            if (DrmPipeline::commitPipelines({m_pipeline}, DrmPipeline::CommitMode::Test) == DrmPipeline::Error::None) {
                m_pipeline->applyPendingChanges();
                m_channelFactorsNeedShaderFallback = false;
                return true;
            } else {
                m_pipeline->setGammaRamp(nullptr);
                m_pipeline->applyPendingChanges();
            }
        }
    }
    m_channelFactorsNeedShaderFallback = m_channelFactors != QVector3D{1, 1, 1};
    return true;
}

QVector3D DrmOutput::channelFactors() const
{
    return m_channelFactors;
}

bool DrmOutput::needsColormanagement() const
{
    static bool forceColorManagement = qEnvironmentVariableIntValue("KWIN_DRM_FORCE_COLOR_MANAGEMENT") != 0;
    return forceColorManagement || m_state.wideColorGamut || m_state.highDynamicRange || m_state.colorProfileSource != ColorProfileSource::sRGB || m_channelFactorsNeedShaderFallback;
}
}

#include "moc_drm_output.cpp"
