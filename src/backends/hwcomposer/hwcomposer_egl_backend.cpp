#include "hwcomposer_egl_backend.h"
#include "hwcomposer_backend.h"
#include "hwcomposer_logging.h"
#include <kwinglplatform.h>
#include <kwinglutils.h>
#include <kwineglimagetexture.h>
#include "basiceglsurfacetexture_internal.h"
#include "basiceglsurfacetexture_wayland.h"
#include "openglbackend.h"
#include <memory>

namespace KWin
{

EglHwcomposerBackend::EglHwcomposerBackend(HwcomposerBackend *backend)
    : AbstractEglBackend()
    , m_backend(backend)
{
    setIsDirectRendering(true);
    setSupportsNativeFence(true);
    init();
}

EglHwcomposerBackend::~EglHwcomposerBackend()
{
    cleanup();
}

bool EglHwcomposerBackend::initializeEgl()
{
    qputenv("EGL_PLATFORM", QByteArrayLiteral("hwcomposer"));
    EGLDisplay display = m_backend->sceneEglDisplay();

    if (display == EGL_NO_DISPLAY) {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (display == EGL_NO_DISPLAY) {
        return false;
    }
    setEglDisplay(display);
    return initEglAPI();
}

void EglHwcomposerBackend::init()
{
    if (!initializeEgl()) {
        setFailed("Failed to initialize egl");
        return;
    }
    if (!initRenderingContext()) {
        setFailed("Could not initialize rendering context");
        return;
    }
    initKWinGL();
    initBufferAge();
    initWayland();
}

bool EglHwcomposerBackend::initBufferConfigs()
{
    const EGLint config_attribs[] = {
        EGL_RED_SIZE,             8,
        EGL_GREEN_SIZE,           8,
        EGL_BLUE_SIZE,            8,
        EGL_ALPHA_SIZE,           8,
        EGL_RENDERABLE_TYPE,      EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,         EGL_WINDOW_BIT,
        EGL_NONE,
    };

    EGLint count;
    EGLConfig configs[1024];
    if (eglChooseConfig(eglDisplay(), config_attribs, configs, 1024, &count) == EGL_FALSE) {
        qCCritical(KWIN_HWCOMPOSER) << "choose config failed";
        return false;
    }
    if (count == 0) {
        qCCritical(KWIN_HWCOMPOSER) << "choose config did not return a config";
        return false;
    }
    setConfig(configs[0]);

    return true;
}

bool EglHwcomposerBackend::initRenderingContext()
{
    if (!initBufferConfigs()) {
        return false;
    }

    if (!createContext()) {
        return false;
    }

    const QSize size = m_backend->size();
    m_buffer = createBuffer(size);
    if (!m_buffer) {
        return false;
    }

    return makeCurrent();
}

std::shared_ptr<EglHwcomposerBuffer> EglHwcomposerBackend::createBuffer(const QSize &size)
{
    return std::make_shared<EglHwcomposerBuffer>(size, eglDisplay(), config());
}

void EglHwcomposerBackend::present(Output *output)
{
    eglSwapBuffers(eglDisplay(), m_buffer->surface());
    m_damageJournal.add(m_lastRenderedRegion);
}

std::optional<OutputLayerBeginFrameInfo> EglHwcomposerBackend::beginFrame()
{
    if (!makeCurrent()) {
        qCCritical(KWIN_HWCOMPOSER) << "Make context current failed";
        return std::nullopt;
    }

    QRegion repair = m_damageJournal.accumulate(0, infiniteRegion());
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_buffer->framebuffer()),
        .repaint = repair,
    };
}

bool EglHwcomposerBackend::endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion)
{
    Q_UNUSED(damagedRegion)
    m_lastRenderedRegion = renderedRegion;
    glFlush();
    return true;
}

std::unique_ptr<SurfaceTexture> EglHwcomposerBackend::createSurfaceTextureInternal(SurfacePixmapInternal *pixmap)
{
    return std::make_unique<BasicEGLSurfaceTextureInternal>(this, pixmap);
}

std::unique_ptr<SurfaceTexture> EglHwcomposerBackend::createSurfaceTextureWayland(SurfacePixmapWayland *pixmap)
{
    return std::make_unique<BasicEGLSurfaceTextureWayland>(this, pixmap);
}

OutputLayer* EglHwcomposerBackend::primaryLayer(Output *output)
{
    if (!output) {
        return nullptr;
    }

    if (!m_output) {
        m_output = std::make_unique<EglHwcomposerOutput>(static_cast<HwcomposerOutput*>(output), this);
    }
    return m_output.get();
}

EglHwcomposerOutput::EglHwcomposerOutput(HwcomposerOutput *output, EglHwcomposerBackend *backend)
    : m_hwcomposerOutput(output), m_backend(backend)
{
}

EglHwcomposerOutput::~EglHwcomposerOutput()
{
}

std::optional<OutputLayerBeginFrameInfo> EglHwcomposerOutput::beginFrame()
{
    QRegion repair = m_damageJournal.accumulate(0, infiniteRegion());

    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_backend->buffer()->framebuffer()),
        .repaint = repair,
    };
}

bool EglHwcomposerOutput::endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion)
{
    Q_UNUSED(damagedRegion)
    m_lastRenderedRegion = renderedRegion;
    glFlush();
    return true;
}

void EglHwcomposerOutput::present()
{
    eglSwapBuffers(m_backend->eglDisplay(), m_backend->buffer()->surface());
    m_damageJournal.add(m_lastRenderedRegion);
}

EglHwcomposerBuffer::EglHwcomposerBuffer(const QSize &size, EGLDisplay eglDisplay, EGLConfig config)
    : m_eglDisplay(eglDisplay)
{
    m_surface = eglCreatePbufferSurface(m_eglDisplay, config, nullptr);
    if (m_surface == EGL_NO_SURFACE) {
        qCCritical(KWIN_HWCOMPOSER) << "Failed to create EGL surface";
        return;
    }

    m_texture = std::make_unique<GLTexture>(GL_RGBA8, size);
    m_framebuffer = std::make_unique<GLFramebuffer>(m_texture.get());
}

EglHwcomposerBuffer::~EglHwcomposerBuffer()
{
    if (m_surface != EGL_NO_SURFACE) {
        eglDestroySurface(m_eglDisplay, m_surface);
    }
}

} // namespace KWin
