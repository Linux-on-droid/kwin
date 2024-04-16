/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "platformsupport/scenes/opengl/abstract_egl_backend.h"
#include "compositor.h"
#include "core/outputbackend.h"
#include "main.h"
#include "opengl/egl_context_attribute_builder.h"
#include "utils/common.h"
#include "wayland/drmclientbuffer.h"
#include "wayland/linux_drm_syncobj_v1.h"
#include "wayland_server.h"
// kwin libs
#include "opengl/eglimagetexture.h"
#include "opengl/eglutils_p.h"
#include "opengl/glplatform.h"
#include "opengl/glutils.h"
#include "utils/drm_format_helper.h"
// Qt
#include <QOpenGLContext>

#include <memory>

#include <drm_fourcc.h>
#include <xf86drm.h>

namespace KWin
{

static std::unique_ptr<EglContext> s_globalShareContext;

static bool isOpenGLES_helper()
{
    if (qstrcmp(qgetenv("KWIN_COMPOSE"), "O2ES") == 0) {
        return true;
    }
    return QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGLES;
}

AbstractEglBackend::AbstractEglBackend(dev_t deviceId)
    : m_deviceId(deviceId)
{
    connect(Compositor::self(), &Compositor::aboutToDestroy, this, &AbstractEglBackend::teardown);
}

AbstractEglBackend::~AbstractEglBackend()
{
}

bool AbstractEglBackend::ensureGlobalShareContext(EGLConfig config)
{
    if (!s_globalShareContext) {
        s_globalShareContext = EglContext::create(m_display, config, EGL_NO_CONTEXT);
    }
    if (s_globalShareContext) {
        kwinApp()->outputBackend()->setSceneEglGlobalShareContext(s_globalShareContext->handle());
        return true;
    } else {
        return false;
    }
}

void AbstractEglBackend::destroyGlobalShareContext()
{
    EglDisplay *const eglDisplay = kwinApp()->outputBackend()->sceneEglDisplayObject();
    if (!eglDisplay || !s_globalShareContext) {
        return;
    }
    s_globalShareContext.reset();
    kwinApp()->outputBackend()->setSceneEglGlobalShareContext(EGL_NO_CONTEXT);
}

void AbstractEglBackend::teardown()
{
    destroyGlobalShareContext();
}

void AbstractEglBackend::cleanup()
{
    for (const EGLImageKHR &image : m_importedBuffers) {
        m_display->destroyImage(image);
    }

    cleanupSurfaces();
    cleanupGL();
    m_context.reset();
}

void AbstractEglBackend::cleanupSurfaces()
{
    if (m_surface != EGL_NO_SURFACE) {
        eglDestroySurface(m_display->handle(), m_surface);
    }
}

void AbstractEglBackend::setEglDisplay(EglDisplay *display)
{
    m_display = display;
    setExtensions(m_display->extensions());
    setSupportsNativeFence(m_display->supportsNativeFence());
    setSupportsBufferAge(m_display->supportsBufferAge());
}

typedef void (*eglFuncPtr)();
static eglFuncPtr getProcAddress(const char *name)
{
    return eglGetProcAddress(name);
}

void AbstractEglBackend::initKWinGL()
{
    GLPlatform *glPlatform = GLPlatform::instance();
    glPlatform->detect(EglPlatformInterface);
    glPlatform->printResults();
    initGL(&getProcAddress);
}

void AbstractEglBackend::initWayland()
{
    if (!WaylandServer::self()) {
        return;
    }

    if (m_deviceId) {
        QString renderNode = m_display->renderNode();
        if (renderNode.isEmpty()) {
            drmDevice *device = nullptr;
            if (drmGetDeviceFromDevId(deviceId(), 0, &device) != 0) {
                qCWarning(KWIN_OPENGL) << "drmGetDeviceFromDevId() failed:" << strerror(errno);
            } else {
                if (device->available_nodes & (1 << DRM_NODE_RENDER)) {
                    renderNode = QString::fromLocal8Bit(device->nodes[DRM_NODE_RENDER]);
                } else if (device->available_nodes & (1 << DRM_NODE_PRIMARY)) {
                    qCWarning(KWIN_OPENGL) << "No render nodes have been found, falling back to primary node";
                    renderNode = QString::fromLocal8Bit(device->nodes[DRM_NODE_PRIMARY]);
                }
                drmFreeDevice(&device);
            }
        }

        if (!renderNode.isEmpty()) {
            waylandServer()->drm()->setDevice(renderNode);
        } else {
            qCWarning(KWIN_OPENGL) << "No render node have been found, not initializing wl-drm";
        }
    }

    const auto formats = m_display->allSupportedDrmFormats();
    auto filterFormats = [this, &formats](std::optional<uint32_t> bpc, bool withExternalOnlyYUV) {
        QHash<uint32_t, QList<uint64_t>> set;
        for (auto it = formats.constBegin(); it != formats.constEnd(); it++) {
            const auto info = FormatInfo::get(it.key());
            if (bpc && (!info || bpc != info->bitsPerColor)) {
                continue;
            }

            const bool externalOnlySupported = withExternalOnlyYUV && info && info->yuvConversion();
            QList<uint64_t> modifiers = externalOnlySupported ? it->allModifiers : it->nonExternalOnlyModifiers;

            if (externalOnlySupported && !modifiers.isEmpty()) {
                if (auto yuv = info->yuvConversion()) {
                    for (auto plane : std::as_const(yuv->plane)) {
                        const auto planeModifiers = formats.value(plane.format).allModifiers;
                        modifiers.erase(std::remove_if(modifiers.begin(), modifiers.end(), [&planeModifiers](uint64_t mod) {
                                            return !planeModifiers.contains(mod);
                                        }),
                                        modifiers.end());
                    }
                }
            }
            for (const auto &tranche : std::as_const(m_tranches)) {
                if (modifiers.isEmpty()) {
                    break;
                }
                const auto trancheModifiers = tranche.formatTable.value(it.key());
                for (auto trancheModifier : trancheModifiers) {
                    modifiers.removeAll(trancheModifier);
                }
            }
            if (modifiers.isEmpty()) {
                continue;
            }
            set.insert(it.key(), modifiers);
        }
        return set;
    };

    auto includeShaderConversions = [](QHash<uint32_t, QList<uint64_t>> &&formats) -> QHash<uint32_t, QList<uint64_t>> {
        for (auto format : s_drmConversions.keys()) {
            auto &modifiers = formats[format];
            if (modifiers.isEmpty()) {
                modifiers = {DRM_FORMAT_MOD_LINEAR};
            }
        }
        return formats;
    };

    if (prefer10bpc()) {
        m_tranches.append({
            .device = deviceId(),
            .flags = {},
            .formatTable = filterFormats(10, false),
        });
    }
    m_tranches.append({
        .device = deviceId(),
        .flags = {},
        .formatTable = filterFormats(8, false),
    });
    m_tranches.append({
        .device = deviceId(),
        .flags = {},
        .formatTable = includeShaderConversions(filterFormats({}, true)),
    });

    waylandServer()->setRenderBackend(this);
    LinuxDmaBufV1ClientBufferIntegration *dmabuf = waylandServer()->linuxDmabuf();
    dmabuf->setRenderBackend(this);
    dmabuf->setSupportedFormatsWithModifiers(m_tranches);
    if (auto syncObj = waylandServer()->linuxSyncObj()) {
        syncObj->setRenderBackend(this);
    }
}

void AbstractEglBackend::initClientExtensions()
{
    // Get the list of client extensions
    const char *clientExtensionsCString = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    const QByteArray clientExtensionsString = QByteArray::fromRawData(clientExtensionsCString, qstrlen(clientExtensionsCString));
    if (clientExtensionsString.isEmpty()) {
        // If eglQueryString() returned NULL, the implementation doesn't support
        // EGL_EXT_client_extensions. Expect an EGL_BAD_DISPLAY error.
        (void)eglGetError();
    }

    m_clientExtensions = clientExtensionsString.split(' ');
}

bool AbstractEglBackend::hasClientExtension(const QByteArray &ext) const
{
    return m_clientExtensions.contains(ext);
}

bool AbstractEglBackend::makeCurrent()
{
    if (QOpenGLContext *context = QOpenGLContext::currentContext()) {
        // Workaround to tell Qt that no QOpenGLContext is current
        context->doneCurrent();
    }
    return m_context->makeCurrent(m_surface);
}

void AbstractEglBackend::doneCurrent()
{
    m_context->doneCurrent();
}

bool AbstractEglBackend::isOpenGLES() const
{
    return isOpenGLES_helper();
}

bool AbstractEglBackend::createContext(EGLConfig config)
{
    if (!ensureGlobalShareContext(config)) {
        return false;
    }
    m_context = EglContext::create(m_display, config, s_globalShareContext ? s_globalShareContext->handle() : EGL_NO_CONTEXT);
    return m_context != nullptr;
}

void AbstractEglBackend::setSurface(const EGLSurface &surface)
{
    m_surface = surface;
}

QList<LinuxDmaBufV1Feedback::Tranche> AbstractEglBackend::tranches() const
{
    return m_tranches;
}

dev_t AbstractEglBackend::deviceId() const
{
    return m_deviceId;
}

bool AbstractEglBackend::prefer10bpc() const
{
    return false;
}

EGLImageKHR AbstractEglBackend::importBufferAsImage(GraphicsBuffer *buffer, int plane, int format, const QSize &size)
{
    std::pair key(buffer, plane);
    auto it = m_importedBuffers.constFind(key);
    if (Q_LIKELY(it != m_importedBuffers.constEnd())) {
        return *it;
    }

    Q_ASSERT(buffer->dmabufAttributes());
    EGLImageKHR image = importDmaBufAsImage(*buffer->dmabufAttributes(), plane, format, size);
    if (image != EGL_NO_IMAGE_KHR) {
        m_importedBuffers[key] = image;
        connect(buffer, &QObject::destroyed, this, [this, key]() {
            m_display->destroyImage(m_importedBuffers.take(key));
        });
    } else {
        qCWarning(KWIN_OPENGL) << "failed to import dmabuf" << buffer;
    }

    return image;
}

EGLImageKHR AbstractEglBackend::importBufferAsImage(GraphicsBuffer *buffer)
{
    auto key = std::pair(buffer, 0);
    auto it = m_importedBuffers.constFind(key);
    if (Q_LIKELY(it != m_importedBuffers.constEnd())) {
        return *it;
    }

    Q_ASSERT(buffer->dmabufAttributes());
    EGLImageKHR image = importDmaBufAsImage(*buffer->dmabufAttributes());
    if (image != EGL_NO_IMAGE_KHR) {
        m_importedBuffers[key] = image;
        connect(buffer, &QObject::destroyed, this, [this, key]() {
            m_display->destroyImage(m_importedBuffers.take(key));
        });
    } else {
        qCWarning(KWIN_OPENGL) << "failed to import dmabuf" << buffer;
    }

    return image;
}

EGLImageKHR AbstractEglBackend::importDmaBufAsImage(const DmaBufAttributes &dmabuf) const
{
    return m_display->importDmaBufAsImage(dmabuf);
}

EGLImageKHR AbstractEglBackend::importDmaBufAsImage(const DmaBufAttributes &dmabuf, int plane, int format, const QSize &size) const
{
    return m_display->importDmaBufAsImage(dmabuf, plane, format, size);
}

std::shared_ptr<GLTexture> AbstractEglBackend::importDmaBufAsTexture(const DmaBufAttributes &attributes) const
{
    return m_context->importDmaBufAsTexture(attributes);
}

bool AbstractEglBackend::testImportBuffer(GraphicsBuffer *buffer)
{
    return importBufferAsImage(buffer) != EGL_NO_IMAGE_KHR;
}

QHash<uint32_t, QList<uint64_t>> AbstractEglBackend::supportedFormats() const
{
    return m_display->nonExternalOnlySupportedDrmFormats();
}

EGLSurface AbstractEglBackend::surface() const
{
    return m_surface;
}

EGLConfig AbstractEglBackend::config() const
{
    return m_context->config();
}

EglDisplay *AbstractEglBackend::eglDisplayObject() const
{
    return m_display;
}

EglContext *AbstractEglBackend::contextObject()
{
    return m_context.get();
}
}

#include "moc_abstract_egl_backend.cpp"
