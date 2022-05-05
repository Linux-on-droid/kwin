/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2022 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "drm_buffer.h"

#include "drm_gpu.h"
#include "logging.h"

// system
#include <sys/mman.h>
// c++
#include <cerrno>
// drm
#include <drm_fourcc.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace KWin
{

DrmGpuBuffer::DrmGpuBuffer(DrmGpu *gpu, QSize size, uint32_t format, uint64_t modifier, const std::array<uint32_t, 4> &handles, const std::array<uint32_t, 4> &strides, const std::array<uint32_t, 4> &offsets, uint32_t planeCount)
    : m_gpu(gpu)
    , m_size(size)
    , m_format(format)
    , m_modifier(modifier)
    , m_handles(handles)
    , m_strides(strides)
    , m_offsets(offsets)
    , m_planeCount(planeCount)
{
}

DrmGpuBuffer::~DrmGpuBuffer()
{
    for (uint32_t i = 0; i < m_planeCount; i++) {
        if (m_fds[i] != -1) {
            close(m_fds[i]);
        }
    }
}

DrmGpu *DrmGpuBuffer::gpu() const
{
    return m_gpu;
}

uint32_t DrmGpuBuffer::format() const
{
    return m_format;
}

uint64_t DrmGpuBuffer::modifier() const
{
    return m_modifier;
}

QSize DrmGpuBuffer::size() const
{
    return m_size;
}

std::array<int, 4> DrmGpuBuffer::fds()
{
    if (m_fds[0] == -1) {
        createFds();
    }
    return m_fds;
}

std::array<uint32_t, 4> DrmGpuBuffer::handles() const
{
    return m_handles;
}

std::array<uint32_t, 4> DrmGpuBuffer::strides() const
{
    return m_strides;
}

std::array<uint32_t, 4> DrmGpuBuffer::offsets() const
{
    return m_offsets;
}

uint32_t DrmGpuBuffer::planeCount() const
{
    return m_planeCount;
}

void DrmGpuBuffer::createFds()
{
}

DrmFramebuffer::DrmFramebuffer(const std::shared_ptr<DrmGpuBuffer> &buffer, uint32_t fbId)
    : m_buffer(buffer)
    , m_framebufferId(fbId)
{
}

DrmFramebuffer::~DrmFramebuffer()
{
    drmModeRmFB(m_buffer->gpu()->fd(), m_framebufferId);
}

uint32_t DrmFramebuffer::framebufferId() const
{
    return m_framebufferId;
}

DrmGpuBuffer *DrmFramebuffer::buffer() const
{
    return m_buffer.get();
}

std::shared_ptr<DrmFramebuffer> DrmFramebuffer::createFramebuffer(const std::shared_ptr<DrmGpuBuffer> &buffer)
{
    const auto size = buffer->size();
    const auto handles = buffer->handles();
    const auto strides = buffer->strides();
    const auto offsets = buffer->offsets();

    uint32_t framebufferId = 0;
    int ret = drmModeAddFB2(buffer->gpu()->fd(), size.width(), size.height(), buffer->format(), handles.data(), strides.data(), offsets.data(), &framebufferId, 0);
    if (ret == EOPNOTSUPP && handles.size() == 1) {
        ret = drmModeAddFB(buffer->gpu()->fd(), size.width(), size.height(), 24, 32, strides[0], handles[0], &framebufferId);
    }
    if (ret == 0) {
        return std::make_shared<DrmFramebuffer>(buffer, framebufferId);
    } else {
        qCWarning(KWIN_DRM) << "Could not create drm framebuffer!" << strerror(errno);
        return nullptr;
    }
}
}
