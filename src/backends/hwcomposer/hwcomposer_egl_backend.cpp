#include "hwcomposer_egl_backend.h"
#include "hwcomposer_backend.h"
#include "hwcomposer_logging.h"
#include <kwinglplatform.h>
#include <kwinglutils.h>
#include <kwineglimagetexture.h>
#include "basiceglsurfacetexture_internal.h"
#include "basiceglsurfacetexture_wayland.h"
#include "openglbackend.h"
#include "kwineglutils_p.h"
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

    m_nativeSurface = m_backend->createSurface();
    EGLSurface surface = eglCreateWindowSurface(eglDisplay(), config(), (EGLNativeWindowType) static_cast<ANativeWindow *>(m_nativeSurface), nullptr);
    if (surface == EGL_NO_SURFACE) {
        qCCritical(KWIN_HWCOMPOSER) << "Create surface failed";
        return false;
    }
    setSurface(surface);

    m_framebuffer = std::make_unique<GLFramebuffer>(0, m_backend->size());
    return makeCurrent();
}

void EglHwcomposerBackend::present(Output *output)
{
    m_output->present();
    static_cast<HwcomposerOutput *>(output)->notifyFrame();
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
    QRegion repair = infiniteRegion();
    if (m_backend->supportsBufferAge()) {
        repair = m_damageJournal.accumulate(m_bufferAge, infiniteRegion());
    }

    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_backend->framebuffer()),
        .repaint = repair,
    };
}

void EglHwcomposerOutput::aboutToStartPainting(const QRegion &damagedRegion)
{
    if (m_backend->surface() && m_bufferAge > 0 && !damagedRegion.isEmpty() && m_backend->supportsPartialUpdate()) {
        QVector<EGLint> rects = m_hwcomposerOutput->regionToRects(damagedRegion);
        const bool correct = eglSetDamageRegionKHR(m_backend->eglDisplay(), m_backend->surface(), rects.data(), rects.count() / 4);
        if (!correct) {
            qCWarning(KWIN_HWCOMPOSER) << "eglSetDamageRegionKHR failed:" << getEglErrorString();
        }
    }
}

bool EglHwcomposerOutput::endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion)
{
    Q_UNUSED(renderedRegion)
    m_currentDamage = damagedRegion;
    return true;
}

void EglHwcomposerOutput::present()
{
    if (m_backend->supportsSwapBuffersWithDamage() && m_backend->supportsPartialUpdate()) {
        QVector<EGLint> rects = m_hwcomposerOutput->regionToRects(m_currentDamage);
        const bool correct = eglSwapBuffersWithDamageKHR(m_backend->eglDisplay(), m_backend->surface(), rects.data(), rects.count() / 4);
        if (!correct) {
            qCWarning(KWIN_HWCOMPOSER) << "eglSwapBuffersWithDamageKHR failed:" << getEglErrorString();
        }
    } else {
        const bool correct = eglSwapBuffers(m_backend->eglDisplay(), m_backend->surface());
        if (!correct) {
            qCWarning(KWIN_HWCOMPOSER) << "eglSwapBuffers failed:" << getEglErrorString();
        }
    }

    if (m_backend->supportsBufferAge()) {
        eglQuerySurface(m_backend->eglDisplay(), m_backend->surface(), EGL_BUFFER_AGE_EXT, &m_bufferAge);
    }
    m_damageJournal.add(m_currentDamage);
}

} // namespace KWin
