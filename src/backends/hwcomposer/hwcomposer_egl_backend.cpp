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

    connect(m_backend, &HwcomposerBackend::outputAdded, this, &EglHwcomposerBackend::createEglHwcomposerOutput);
    connect(m_backend, &HwcomposerBackend::outputRemoved, this, [this](Output *output) {
        m_outputs.erase(output);
    });

    init();
}

EglHwcomposerBackend::~EglHwcomposerBackend()
{
    cleanup();
}

void EglHwcomposerBackend::cleanupSurfaces()
{
    m_outputs.clear();
}

bool EglHwcomposerBackend::createEglHwcomposerOutput(Output *output)
{
    m_outputs[output] = std::make_unique<EglHwcomposerOutput>(static_cast<HwcomposerOutput *>(output), this);
    return true;
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

    auto hwcOutputs = m_backend->outputs();

    // we only allow to start with at least one output
    if (hwcOutputs.isEmpty()) {
        return false;
    }

    for (auto *out : hwcOutputs) {
        if (!createEglHwcomposerOutput(out)) {
            return false;
        }
    }

    if (m_outputs.empty()) {
        qCCritical(KWIN_HWCOMPOSER) << "Create Window Surfaces failed";
        return false;
    }

    return makeCurrent();
}

void EglHwcomposerBackend::present(Output *output)
{
    if (!output) {
        return;
    }

    m_outputs[output]->present();
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

    return m_outputs[output].get();
}

EglHwcomposerOutput::EglHwcomposerOutput(HwcomposerOutput *output, EglHwcomposerBackend *backend)
    : m_output(output), m_backend(backend)
{
    m_nativeSurface = m_output->createSurface();
    m_surface = eglCreateWindowSurface(m_backend->eglDisplay(), m_backend->config(), (EGLNativeWindowType) static_cast<ANativeWindow *>(m_nativeSurface), nullptr);
    if (m_surface == EGL_NO_SURFACE) {
        qCCritical(KWIN_HWCOMPOSER) << "Create surface failed";
        return;
    }

    m_framebuffer = std::make_unique<GLFramebuffer>(0, m_output->pixelSize());
}

EglHwcomposerOutput::~EglHwcomposerOutput()
{
    if (m_surface != EGL_NO_SURFACE) {
        eglDestroySurface(m_backend->eglDisplay(), m_surface);
    }
}

bool EglHwcomposerOutput::makeContextCurrent() const
{
    if (eglMakeCurrent(m_backend->eglDisplay(), surface(), surface(), m_backend->context()) == EGL_FALSE) {
        qCCritical(KWIN_HWCOMPOSER) << "eglMakeCurrent failed:" << getEglErrorString();
        return false;
    }
    return true;
}

std::optional<OutputLayerBeginFrameInfo> EglHwcomposerOutput::beginFrame()
{
    if (!makeContextCurrent()) {
        return std::nullopt;
    }

    QRegion repair = infiniteRegion();
    if (m_backend->supportsBufferAge()) {
        repair = m_damageJournal.accumulate(m_bufferAge, infiniteRegion());
    }

    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(framebuffer()),
        .repaint = repair,
    };
}

void EglHwcomposerOutput::aboutToStartPainting(const QRegion &damagedRegion)
{
    if (surface() && m_bufferAge > 0 && !damagedRegion.isEmpty() && m_backend->supportsPartialUpdate()) {
        QVector<EGLint> rects = m_output->regionToRects(damagedRegion);
        const bool correct = eglSetDamageRegionKHR(m_backend->eglDisplay(), surface(), rects.data(), rects.count() / 4);
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
        QVector<EGLint> rects = m_output->regionToRects(m_currentDamage);
        const bool correct = eglSwapBuffersWithDamageKHR(m_backend->eglDisplay(), surface(), rects.data(), rects.count() / 4);
        if (!correct) {
            qCWarning(KWIN_HWCOMPOSER) << "eglSwapBuffersWithDamageKHR failed:" << getEglErrorString();
        }
    } else {
        const bool correct = eglSwapBuffers(m_backend->eglDisplay(), surface());
        if (!correct) {
            qCWarning(KWIN_HWCOMPOSER) << "eglSwapBuffers failed:" << getEglErrorString();
        }
    }

    if (m_backend->supportsBufferAge()) {
        eglQuerySurface(m_backend->eglDisplay(), surface(), EGL_BUFFER_AGE_EXT, &m_bufferAge);
    }
    m_damageJournal.add(m_currentDamage);
}

} // namespace KWin
