/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2015 Martin Gräßlin <mgraesslin@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
#include "abstractplatformcontext.h"
#include "integration.h"

namespace KWin
{

namespace QPA
{

static bool isOpenGLES()
{
    if (qstrcmp(qgetenv("KWIN_COMPOSE"), "O2ES") == 0) {
        return true;
    }
    return QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES;
}

static EGLConfig configFromGLFormat(EGLDisplay dpy, const QSurfaceFormat &format)
{
#define SIZE( __buffer__ ) format.__buffer__##BufferSize() > 0 ? format.__buffer__##BufferSize() : 0
    // not setting samples as QtQuick doesn't need it
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,         EGL_WINDOW_BIT,
        EGL_RED_SIZE,             SIZE(red),
        EGL_GREEN_SIZE,           SIZE(green),
        EGL_BLUE_SIZE,            SIZE(blue),
        EGL_ALPHA_SIZE,           SIZE(alpha),
        EGL_DEPTH_SIZE,           SIZE(depth),
        EGL_STENCIL_SIZE,         SIZE(stencil),
        EGL_RENDERABLE_TYPE,      isOpenGLES() ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_BIT,
        EGL_NONE,
    };
#undef SIZE

    EGLint count;
    EGLConfig configs[1024];
    if (eglChooseConfig(dpy, config_attribs, configs, 1, &count) == EGL_FALSE) {
        return 0;
    }
    if (count != 1) {
        return 0;
    }
    return configs[0];
}

static QSurfaceFormat formatFromConfig(EGLDisplay dpy, EGLConfig config)
{
    QSurfaceFormat format;
    EGLint value = 0;
#define HELPER(__egl__, __qt__) \
    eglGetConfigAttrib(dpy, config, EGL_##__egl__, &value); \
    format.set##__qt__(value); \
    value = 0;

#define BUFFER_HELPER(__eglColor__, __color__) \
    HELPER(__eglColor__##_SIZE, __color__##BufferSize)

    BUFFER_HELPER(RED, Red)
    BUFFER_HELPER(GREEN, Green)
    BUFFER_HELPER(BLUE, Blue)
    BUFFER_HELPER(ALPHA, Alpha)
    BUFFER_HELPER(STENCIL, Stencil)
    BUFFER_HELPER(DEPTH, Depth)
#undef BUFFER_HELPER
    HELPER(SAMPLES, Samples)
#undef HELPER
    format.setRenderableType(isOpenGLES() ? QSurfaceFormat::OpenGLES : QSurfaceFormat::OpenGL);
    format.setStereo(false);

    return format;
}

AbstractPlatformContext::AbstractPlatformContext(QOpenGLContext *context, Integration *integration, EGLDisplay display)
    : QPlatformOpenGLContext()
    , m_integration(integration)
    , m_eglDisplay(display)
    , m_config(configFromGLFormat(m_eglDisplay, context->format()))
    , m_format(formatFromConfig(m_eglDisplay, m_config))
{
}

AbstractPlatformContext::~AbstractPlatformContext()
{
    if (m_context != EGL_NO_CONTEXT) {
        eglDestroyContext(m_eglDisplay, m_context);
    }
}

void AbstractPlatformContext::doneCurrent()
{
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

QSurfaceFormat AbstractPlatformContext::format() const
{
    return m_format;
}

QFunctionPointer AbstractPlatformContext::getProcAddress(const QByteArray &procName)
{
    return eglGetProcAddress(procName.constData());
}

bool AbstractPlatformContext::isValid() const
{
    return m_context != EGL_NO_CONTEXT;
}

bool AbstractPlatformContext::bindApi()
{
    if (eglBindAPI(isOpenGLES() ? EGL_OPENGL_ES_API : EGL_OPENGL_API) == EGL_FALSE) {
        return false;
    }
    return true;
}

void AbstractPlatformContext::createContext(EGLContext shareContext)
{
    EGLContext context = EGL_NO_CONTEXT;
    if (isOpenGLES()) {
        const EGLint context_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };

        context = eglCreateContext(eglDisplay(), config(), shareContext, context_attribs);
    } else {
        const EGLint context_attribs_31_core[] = {
            EGL_CONTEXT_MAJOR_VERSION_KHR, m_format.majorVersion(),
            EGL_CONTEXT_MINOR_VERSION_KHR, m_format.minorVersion(),
            EGL_CONTEXT_FLAGS_KHR,         EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR,
            EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, m_format.profile() == QSurfaceFormat::CoreProfile ? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR : EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
            EGL_NONE
        };

        const EGLint context_attribs_legacy[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };

        const char* eglExtensionsCString = eglQueryString(eglDisplay(), EGL_EXTENSIONS);
        const QList<QByteArray> extensions = QByteArray::fromRawData(eglExtensionsCString, qstrlen(eglExtensionsCString)).split(' ');

        // Try to create a 3.1 core context
        if (m_format.majorVersion() >= 3 &&  extensions.contains(QByteArrayLiteral("EGL_KHR_create_context"))) {
            context = eglCreateContext(eglDisplay(), config(), shareContext, context_attribs_31_core);
        }

        if (context == EGL_NO_CONTEXT) {
            context = eglCreateContext(eglDisplay(), config(), shareContext, context_attribs_legacy);
        }
    }

    if (context == EGL_NO_CONTEXT) {
        return;
    }
    m_context = context;
}

}
}
