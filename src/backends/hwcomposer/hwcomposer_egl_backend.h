/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include "abstract_egl_backend.h"
#include "core/outputlayer.h"
#include "utils/common.h"
#include "utils/damagejournal.h"
#include <memory>
#include <optional>

#include <KWayland/Client/buffer.h>

namespace KWin
{

class HwcomposerBackend;
class HwcomposerOutput;
class HwcomposerWindow;
class EglHwcomposerOutput; // Forward declaration

class EglHwcomposerBackend : public AbstractEglBackend
{
public:
    EglHwcomposerBackend(HwcomposerBackend *backend);
    ~EglHwcomposerBackend() override;

    std::unique_ptr<SurfaceTexture> createSurfaceTextureInternal(SurfacePixmapInternal *pixmap) override;
    std::unique_ptr<SurfaceTexture> createSurfaceTextureWayland(SurfacePixmapWayland *pixmap) override;
    OutputLayer *primaryLayer(Output *output) override;
    void init() override;
    void present(Output *output) override;

private:
    bool initializeEgl();
    bool initBufferConfigs();
    bool initRenderingContext();
    bool createEglHwcomposerOutput(Output *output);
    void cleanupSurfaces() override;

    HwcomposerBackend *m_backend;
    std::map<Output *, std::unique_ptr<EglHwcomposerOutput>> m_outputs;
};

class EglHwcomposerOutput : public OutputLayer
{
public:
    EglHwcomposerOutput(HwcomposerOutput *output, EglHwcomposerBackend *backend);
    ~EglHwcomposerOutput() override;

    std::optional<OutputLayerBeginFrameInfo> beginFrame() override;
    void aboutToStartPainting(const QRegion &damagedRegion) override;
    bool endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion) override;
    void present();

    EGLSurface surface() const
    {
        return m_surface;
    }
    GLFramebuffer *framebuffer() const
    {
        return m_framebuffer.get();
    }

private:
    bool makeContextCurrent() const;

    HwcomposerWindow *m_nativeSurface = nullptr;
    EGLSurface m_surface = EGL_NO_SURFACE;
    std::unique_ptr<GLFramebuffer> m_framebuffer;
    QRegion m_currentDamage;
    DamageJournal m_damageJournal;
    int m_bufferAge = 0;

    HwcomposerOutput *m_output;
    EglHwcomposerBackend *m_backend;
};

} // namespace KWin
