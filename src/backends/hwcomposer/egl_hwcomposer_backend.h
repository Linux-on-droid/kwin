/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-3.0-or-later
*/
#ifndef KWIN_EGL_HWCOMPOSER_BACKEND_H
#define KWIN_EGL_HWCOMPOSER_BACKEND_H
#include "abstract_egl_backend.h"
#include "utils/common.h"
#include <KWaylandServer/outputdevice_v2_interface.h>

#define ROTATE_EGL 0
namespace KWin
{

class HwcomposerBackend;
class HwcomposerWindow;
class HwcomposerOutput;
class EglHwcomposerBackend : public AbstractEglBackend
{
public:
    EglHwcomposerBackend(HwcomposerBackend *backend);
    virtual ~EglHwcomposerBackend();
    SurfaceTexture *createSurfaceTextureInternal(SurfacePixmapInternal *pixmap) override;
    SurfaceTexture *createSurfaceTextureWayland(SurfacePixmapWayland *pixmap) override;   
    QRegion beginFrame(AbstractOutput *output) override;
    void endFrame(AbstractOutput *output, const QRegion &renderedRegion, const QRegion &damagedRegion) override;
    void init() override;

private:
    bool initializeEgl();
    bool initRenderingContext();
    bool initBufferConfigs();
    bool makeContextCurrent();
    HwcomposerBackend *m_backend;
    HwcomposerWindow *m_nativeSurface = nullptr;
    DamageJournal m_damageJournal;
};

}

#endif
