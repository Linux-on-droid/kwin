/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2007 Rivo Laks <rivolaks@hot.ee>
    SPDX-FileCopyrightText: 2008 Lucas Murray <lmurray@undefinedfire.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "invert.h"

#include "libkwineffects/kwinglplatform.h"
#include "libkwineffects/kwinglutils.h"
#include <KGlobalAccel>
#include <KLocalizedString>
#include <QAction>
#include <QFile>
#include <QStandardPaths>

#include <QMatrix4x4>

Q_LOGGING_CATEGORY(KWIN_INVERT, "kwin_effect_invert", QtWarningMsg)

static void ensureResources()
{
    // Must initialize resources manually because the effect is a static lib.
    Q_INIT_RESOURCE(invert);
}

namespace KWin
{

InvertEffect::InvertEffect()
    : m_inited(false)
    , m_valid(true)
    , m_shader(nullptr)
    , m_allWindows(false)
{
    QAction *a = new QAction(this);
    a->setObjectName(QStringLiteral("Invert"));
    a->setText(i18n("Toggle Invert Effect"));
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << (Qt::CTRL | Qt::META | Qt::Key_I));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << (Qt::CTRL | Qt::META | Qt::Key_I));
    connect(a, &QAction::triggered, this, &InvertEffect::toggleScreenInversion);

    QAction *b = new QAction(this);
    b->setObjectName(QStringLiteral("InvertWindow"));
    b->setText(i18n("Toggle Invert Effect on Window"));
    KGlobalAccel::self()->setDefaultShortcut(b, QList<QKeySequence>() << (Qt::CTRL | Qt::META | Qt::Key_U));
    KGlobalAccel::self()->setShortcut(b, QList<QKeySequence>() << (Qt::CTRL | Qt::META | Qt::Key_U));
    connect(b, &QAction::triggered, this, [this] {
        InvertEffect::toggleWindow(effects->activeWindow());
    });

    QAction *c = new QAction(this);
    c->setObjectName(QStringLiteral("Invert Screen Colors"));
    c->setText(i18n("Invert Screen Colors"));
    KGlobalAccel::self()->setDefaultShortcut(c, QList<QKeySequence>());
    KGlobalAccel::self()->setShortcut(c, QList<QKeySequence>());
    connect(c, &QAction::triggered, this, &InvertEffect::toggleScreenInversion);

    connect(effects, &EffectsHandler::windowAdded, this, &InvertEffect::slotWindowAdded);
    connect(effects, &EffectsHandler::windowClosed, this, &InvertEffect::slotWindowClosed);
}

InvertEffect::~InvertEffect() = default;

bool InvertEffect::supported()
{
    return effects->compositingType() == OpenGLCompositing;
}

bool InvertEffect::isInvertable(EffectWindow *window) const
{
    return m_allWindows != m_windows.contains(window);
}

void InvertEffect::invert(EffectWindow *window)
{
    if (m_valid && !m_inited) {
        m_valid = loadData();
    }

    redirect(window);
    setShader(window, m_shader.get());
}

void InvertEffect::uninvert(EffectWindow *window)
{
    unredirect(window);
}

bool InvertEffect::loadData()
{
    ensureResources();
    m_inited = true;

    m_shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture, QString(), QStringLiteral(":/effects/invert/shaders/invert.frag"));
    if (!m_shader->isValid()) {
        qCCritical(KWIN_INVERT) << "The shader failed to load!";
        return false;
    }

    return true;
}

void InvertEffect::slotWindowAdded(KWin::EffectWindow *w)
{
    if (isInvertable(w)) {
        invert(w);
    }
}

void InvertEffect::slotWindowClosed(EffectWindow *w)
{
    m_windows.removeOne(w);
}

void InvertEffect::toggleScreenInversion()
{
    m_allWindows = !m_allWindows;

    const auto windows = effects->stackingOrder();
    for (EffectWindow *window : windows) {
        if (isInvertable(window)) {
            invert(window);
        } else {
            uninvert(window);
        }
    }

    effects->addRepaintFull();
}

void InvertEffect::toggleWindow(KWin::EffectWindow *window)
{
    if (!window) {
        return;
    }
    if (!m_windows.contains(window)) {
        m_windows.append(window);
    } else {
        m_windows.removeOne(window);
    }
    if (isInvertable(window)) {
        invert(window);
    } else {
        uninvert(window);
    }
    window->addRepaintFull();
}

bool InvertEffect::isActive() const
{
    return m_valid && (m_allWindows || !m_windows.isEmpty());
}

bool InvertEffect::provides(Feature f)
{
    return f == ScreenInversion;
}

bool KWin::InvertEffect::perform(Feature feature, const QVariantList &arguments)
{
    if (feature != ScreenInversion) {
        return false;
    }
    for (const auto &arg : arguments) {
        if (auto window = arg.value<KWin::EffectWindow *>()) {
            toggleWindow(window);
        }
    }
    return true;
}

} // namespace
