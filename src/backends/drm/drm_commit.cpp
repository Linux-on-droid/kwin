/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2023 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_commit.h"
#include "core/renderbackend.h"
#include "drm_blob.h"
#include "drm_buffer.h"
#include "drm_connector.h"
#include "drm_crtc.h"
#include "drm_gpu.h"
#include "drm_object.h"
#include "drm_property.h"

#include <QApplication>
#include <QThread>

namespace KWin
{

DrmCommit::DrmCommit(DrmGpu *gpu)
    : m_gpu(gpu)
{
}

DrmCommit::~DrmCommit()
{
    Q_ASSERT(QThread::currentThread() == QApplication::instance()->thread());
}

DrmGpu *DrmCommit::gpu() const
{
    return m_gpu;
}

DrmAtomicCommit::DrmAtomicCommit(const QList<DrmPipeline *> &pipelines)
    : DrmCommit(pipelines.front()->gpu())
    , m_pipelines(pipelines)
{
}

void DrmAtomicCommit::addProperty(const DrmProperty &prop, uint64_t value)
{
    prop.checkValueInRange(value);
    props.push_back(&prop);
    m_properties[prop.drmObject()->id()][prop.propId()] = value;
}

void DrmAtomicCommit::addBlob(const DrmProperty &prop, const std::shared_ptr<DrmBlob> &blob)
{
    addProperty(prop, blob ? blob->blobId() : 0);
    m_blobs[&prop] = blob;
}

void DrmAtomicCommit::addBuffer(DrmPlane *plane, const std::shared_ptr<DrmFramebuffer> &buffer, const std::shared_ptr<OutputFrame> &frame)
{
    addProperty(plane->fbId, buffer ? buffer->framebufferId() : 0);
    m_buffers[plane] = buffer;
    m_frames[plane] = frame;
    // atomic commits with IN_FENCE_FD fail with NVidia
    // and also with tearing (as of kernel 6.8, should be fixed later)
    if (plane->inFenceFd.isValid() && !plane->gpu()->isNVidia() && !m_tearing) {
        addProperty(plane->inFenceFd, buffer ? buffer->syncFd().get() : -1);
    }
    m_planes.emplace(plane);
}

void DrmAtomicCommit::setVrr(DrmCrtc *crtc, bool vrr)
{
    addProperty(crtc->vrrEnabled, vrr ? 1 : 0);
    m_vrr = vrr;
}

void DrmAtomicCommit::setTearing(bool enable)
{
    m_tearing = enable;
}

void DrmAtomicCommit::setPresentationMode(PresentationMode mode)
{
    m_mode = mode;
}

bool DrmAtomicCommit::test(DrmAtomicCommit *currentState)
{
    return doCommit(DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_NONBLOCK | (m_tearing ? DRM_MODE_PAGE_FLIP_ASYNC : 0), currentState);
}

bool DrmAtomicCommit::testAllowModeset()
{
    return doCommit(DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET);
}

bool DrmAtomicCommit::commit(DrmAtomicCommit *currentState)
{
    return doCommit(DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT | (m_tearing ? DRM_MODE_PAGE_FLIP_ASYNC : 0), currentState);
}

bool DrmAtomicCommit::commitModeset()
{
    m_modeset = true;
    return doCommit(DRM_MODE_ATOMIC_ALLOW_MODESET);
}

bool DrmAtomicCommit::doCommit(uint32_t flags, DrmAtomicCommit *currentState)
{
    std::vector<uint32_t> objects;
    std::vector<uint32_t> propertyCounts;
    std::vector<uint32_t> propertyIds;
    std::vector<uint64_t> values;
    objects.reserve(m_properties.size());
    propertyCounts.reserve(m_properties.size());
    int removed = 0;
    int added = 0;
    for (const auto &[object, properties] : m_properties) {
        size_t i = 0;
        for (const auto &[property, value] : properties) {
            // The kernel rejects async commits that contain anything else than FB_ID of the primary plane,
            // even if the property values aren't actually modified.
            // To make async commits still possible, filter out properties that haven't changed on our side
            if (m_tearing && currentState) {
                const auto objIt = currentState->m_properties.find(object);
                if (objIt != currentState->m_properties.end()) {
                    const auto propIt = objIt->second.find(property);
                    if (propIt != objIt->second.end()) {
                        const uint64_t previousValue = propIt->second;
                        if (previousValue == value) {
                            removed++;
                            const auto it = std::find_if(props.begin(), props.end(), [property](const DrmProperty *prop) {
                                return prop->propId() == property;
                            });
                            if (it != props.end()) {
                                qWarning() << "removing prop" << (*it)->name() << "->" << value;
                            }
                            continue;
                        }
                    }
                }
            }
            propertyIds.push_back(property);
            values.push_back(value);
            i++;
            if (m_tearing) {
                const auto it = std::find_if(props.begin(), props.end(), [property](const DrmProperty *prop) {
                    return prop->propId() == property;
                });
                if (it != props.end()) {
                    qWarning() << "adding prop" << (*it)->name() << "->" << value;
                }
            }
            added++;
        }
        if (i > 0) {
            objects.push_back(object);
            propertyCounts.push_back(i);
        }
    }
    drm_mode_atomic commitData{
        .flags = flags,
        .count_objs = uint32_t(objects.size()),
        .objs_ptr = reinterpret_cast<uint64_t>(objects.data()),
        .count_props_ptr = reinterpret_cast<uint64_t>(propertyCounts.data()),
        .props_ptr = reinterpret_cast<uint64_t>(propertyIds.data()),
        .prop_values_ptr = reinterpret_cast<uint64_t>(values.data()),
        .reserved = 0,
        .user_data = reinterpret_cast<uint64_t>(this),
    };
    bool ret = drmIoctl(m_gpu->fd(), DRM_IOCTL_MODE_ATOMIC, &commitData) == 0;
    if (m_tearing) {
        qWarning() << "removed:" << removed << "added:" << added;
    }
    return ret;
}

void DrmAtomicCommit::pageFlipped(std::chrono::nanoseconds timestamp)
{
    Q_ASSERT(QThread::currentThread() == QApplication::instance()->thread());
    for (const auto &[plane, buffer] : m_buffers) {
        plane->setCurrentBuffer(buffer);
    }
    for (const auto &[plane, frame] : m_frames) {
        if (frame) {
            frame->presented(timestamp, m_mode);
        }
    }
    m_frames.clear();
    for (const auto pipeline : std::as_const(m_pipelines)) {
        pipeline->pageFlipped(timestamp);
    }
}

bool DrmAtomicCommit::areBuffersReadable() const
{
    return std::ranges::all_of(m_buffers, [](const auto &pair) {
        const auto &[plane, buffer] = pair;
        return !buffer || buffer->isReadable();
    });
}

void DrmAtomicCommit::setDeadline(std::chrono::steady_clock::time_point deadline)
{
    for (const auto &[plane, buffer] : m_buffers) {
        if (buffer) {
            buffer->setDeadline(deadline);
        }
    }
}

std::optional<bool> DrmAtomicCommit::isVrr() const
{
    return m_vrr;
}

const std::unordered_set<DrmPlane *> &DrmAtomicCommit::modifiedPlanes() const
{
    return m_planes;
}

void DrmAtomicCommit::merge(DrmAtomicCommit *onTop)
{
    for (const auto &[obj, properties] : onTop->m_properties) {
        auto &ownProperties = m_properties[obj];
        for (const auto &[prop, value] : properties) {
            ownProperties[prop] = value;
        }
    }
    for (const auto &[plane, buffer] : onTop->m_buffers) {
        m_buffers[plane] = buffer;
        m_frames[plane] = onTop->m_frames[plane];
        m_planes.emplace(plane);
    }
    for (const auto &[prop, blob] : onTop->m_blobs) {
        m_blobs[prop] = blob;
    }
    if (onTop->m_vrr) {
        m_vrr = onTop->m_vrr;
    }
    m_cursorOnly &= onTop->isCursorOnly();
    m_tearing &= onTop->m_tearing;
}

void DrmAtomicCommit::setCursorOnly(bool cursor)
{
    m_cursorOnly = cursor;
}

bool DrmAtomicCommit::isCursorOnly() const
{
    return m_cursorOnly;
}

bool DrmAtomicCommit::tearing() const
{
    return m_tearing;
}

DrmLegacyCommit::DrmLegacyCommit(DrmPipeline *pipeline, const std::shared_ptr<DrmFramebuffer> &buffer, const std::shared_ptr<OutputFrame> &frame)
    : DrmCommit(pipeline->gpu())
    , m_pipeline(pipeline)
    , m_buffer(buffer)
    , m_frame(frame)
{
}

bool DrmLegacyCommit::doModeset(DrmConnector *connector, DrmConnectorMode *mode)
{
    uint32_t connectorId = connector->id();
    if (drmModeSetCrtc(gpu()->fd(), m_pipeline->crtc()->id(), m_buffer->framebufferId(), 0, 0, &connectorId, 1, mode->nativeMode()) == 0) {
        m_pipeline->crtc()->setCurrent(m_buffer);
        return true;
    } else {
        return false;
    }
}

bool DrmLegacyCommit::doPageflip(PresentationMode mode)
{
    m_mode = mode;
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
    if (mode == PresentationMode::Async || mode == PresentationMode::AdaptiveAsync) {
        flags |= DRM_MODE_PAGE_FLIP_ASYNC;
    }
    return drmModePageFlip(gpu()->fd(), m_pipeline->crtc()->id(), m_buffer->framebufferId(), flags, this) == 0;
}

void DrmLegacyCommit::pageFlipped(std::chrono::nanoseconds timestamp)
{
    Q_ASSERT(QThread::currentThread() == QApplication::instance()->thread());
    m_pipeline->crtc()->setCurrent(m_buffer);
    if (m_frame) {
        m_frame->presented(timestamp, m_mode);
        m_frame.reset();
    }
    m_pipeline->pageFlipped(timestamp);
}
}
