/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef KWIN_EGL_HWCOMPOSER_BACKEND_H
#define KWIN_EGL_HWCOMPOSER_BACKEND_H

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

    GLFramebuffer* framebuffer() const { return m_framebuffer.get(); }

private:
    bool initializeEgl();
    bool initRenderingContext();
    bool initBufferConfigs();
    bool makeContextCurrent();

    HwcomposerBackend *m_backend;
    std::unique_ptr<GLFramebuffer> m_framebuffer;
    HwcomposerWindow *m_nativeSurface = nullptr;
    QRegion m_lastRenderedRegion;
    DamageJournal m_damageJournal;
    std::unique_ptr<EglHwcomposerOutput> m_output;
};

class EglHwcomposerOutput : public OutputLayer
{
public:
    EglHwcomposerOutput(HwcomposerOutput *output, EglHwcomposerBackend *backend);
    ~EglHwcomposerOutput() override;

    std::optional<OutputLayerBeginFrameInfo> beginFrame() override;
    bool endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion) override;
    void present();

private:
    HwcomposerOutput *m_hwcomposerOutput;
    EglHwcomposerBackend *m_backend;
    QRegion m_lastRenderedRegion;
    DamageJournal m_damageJournal;
};


} // namespace KWin

#endif // KWIN_EGL_HWCOMPOSER_BACKEND_H
