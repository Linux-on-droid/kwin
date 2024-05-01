/*
    SPDX-FileCopyrightText: 2014 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2018 David Edmundson <davidedmundson@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "display.h"
#include "clientconnection.h"
#include "display_p.h"
#include "linuxdmabufv1clientbuffer_p.h"
#include "output.h"
#include "shmclientbuffer_p.h"
#include "utils/common.h"
#include "utils/systemservice.h"
#include "utils/wl-socket.h"

#include <poll.h>
#include <string.h>
#include <sys/socket.h>

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QDebug>
#include <QRect>

namespace KWin
{
DisplayPrivate *DisplayPrivate::get(Display *display)
{
    return display->d.get();
}

DisplayPrivate::DisplayPrivate(Display *q)
    : q(q)
{
}

void DisplayPrivate::registerSocketName(const QString &socketName)
{
    socketNames.append(socketName);
    KWinSystemService::FDStore::instance()->storeMetadata("waylandSocketNames", socketNames);
    Q_EMIT q->socketNamesChanged();
}

Display::Display(QObject *parent)
    : QObject(parent)
    , d(new DisplayPrivate(this))
{
    d->display = wl_display_create();
    d->loop = wl_display_get_event_loop(d->display);
}

Display::~Display()
{
    wl_display_destroy_clients(d->display);
    wl_display_destroy(d->display);
}

bool Display::addSocketFileDescriptor(int fileDescriptor, const QString &name)
{
    // Clean this up
    if (wl_display_add_socket_fd(d->display, fileDescriptor)) {
        qCWarning(KWIN_CORE, "Failed to add %d fd to display", fileDescriptor);
        return false;
    }
    if (!name.isEmpty()) {
        d->registerSocketName(name);
    }
    return true;
}

bool Display::addSocketName(const QString &name)
{
    // Dave, this is now a crap method name
    // we're also no longer using the socket name

    int lastFd = KWinSystemService::FDStore::instance()->storedFd("waylandSocket");
    if (lastFd >= 0) {
        qDebug() << "restoring FD";
        addSocketFileDescriptor(lastFd);

        // Dave we're mixing up cardinality here
        d->socketNames = KWinSystemService::FDStore::instance()->metaData("waylandSocketNames").toStringList();

        // Dave how will we do kwin_wayland --replace?

        // Dave, if we pushed env vars somewhere we could drop teh
        // concept of socketNames entirely
    } else {
        qDebug() << "making new socket";
        // Dave this leaks, probably best to adjust that original code
        // Also need to keep the fd-lock?
        auto socket = wl_socket_create();
        if (!socket) {
            qCWarning(KWIN_CORE) << "Failed to create a new socket";
            return false;
        }
        int fd = wl_socket_get_fd(socket);

        qDebug() << "new socket " << wl_socket_get_display_name(socket);
        KWinSystemService::FDStore::instance()->store("waylandSocket", fd);

        addSocketFileDescriptor(fd, wl_socket_get_display_name(socket));
    }
    return true;
}

QStringList Display::socketNames() const
{
    return d->socketNames;
}

bool Display::start()
{
    if (d->running) {
        return true;
    }

    const int fileDescriptor = wl_event_loop_get_fd(d->loop);
    if (fileDescriptor == -1) {
        qCWarning(KWIN_CORE) << "Did not get the file descriptor for the event loop";
        return false;
    }

    d->socketNotifier = new QSocketNotifier(fileDescriptor, QSocketNotifier::Read, this);
    connect(d->socketNotifier, &QSocketNotifier::activated, this, &Display::dispatchEvents);

    QAbstractEventDispatcher *dispatcher = QCoreApplication::eventDispatcher();
    connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, &Display::flush);

    d->running = true;
    Q_EMIT runningChanged(true);

    return true;
}

void Display::dispatchEvents()
{
    if (wl_event_loop_dispatch(d->loop, 0) != 0) {
        qCWarning(KWIN_CORE) << "Error on dispatching Wayland event loop";
    }
}

void Display::flush()
{
    wl_display_flush_clients(d->display);
}

void Display::createShm()
{
    Q_ASSERT(d->display);
    new ShmClientBufferIntegration(this);
}

quint32 Display::nextSerial()
{
    return wl_display_next_serial(d->display);
}

quint32 Display::serial()
{
    return wl_display_get_serial(d->display);
}

bool Display::isRunning() const
{
    return d->running;
}

Display::operator wl_display *()
{
    return d->display;
}

Display::operator wl_display *() const
{
    return d->display;
}

QList<OutputInterface *> Display::outputs() const
{
    return d->outputs;
}

QList<OutputDeviceV2Interface *> Display::outputDevices() const
{
    return d->outputdevicesV2;
}

QList<OutputInterface *> Display::outputsIntersecting(const QRect &rect) const
{
    QList<OutputInterface *> outputs;
    for (auto *output : std::as_const(d->outputs)) {
        if (output->handle()->geometry().intersects(rect)) {
            outputs << output;
        }
    }
    return outputs;
}

OutputInterface *Display::largestIntersectingOutput(const QRect &rect) const
{
    OutputInterface *returnOutput = nullptr;
    uint64_t biggestArea = 0;
    for (auto *output : std::as_const(d->outputs)) {
        const QRect intersect = output->handle()->geometry().intersected(rect);
        const uint64_t area = intersect.width() * intersect.height();
        if (area > biggestArea) {
            biggestArea = area;
            returnOutput = output;
        }
    }
    return returnOutput;
}

QList<SeatInterface *> Display::seats() const
{
    return d->seats;
}

ClientConnection *Display::getConnection(wl_client *client)
{
    // TODO: Use wl_client_set_user_data() when we start requiring libwayland-server that has it, and remove client lists here and in ClientConnection.
    Q_ASSERT(client);
    auto it = std::find_if(d->clients.constBegin(), d->clients.constEnd(), [client](ClientConnection *c) {
        return c->client() == client;
    });
    if (it != d->clients.constEnd()) {
        return *it;
    }
    // no ConnectionData yet, create it
    auto c = new ClientConnection(client, this);
    d->clients << c;
    connect(c, &ClientConnection::disconnected, this, [this](ClientConnection *c) {
        Q_EMIT clientDisconnected(c);
    });
    connect(c, &ClientConnection::destroyed, this, [this, c]() {
        d->clients.removeOne(c);
    });
    Q_EMIT clientConnected(c);
    return c;
}

ClientConnection *Display::createClient(int fd)
{
    Q_ASSERT(fd != -1);
    Q_ASSERT(d->display);
    wl_client *c = wl_client_create(d->display, fd);
    if (!c) {
        return nullptr;
    }
    return getConnection(c);
}

GraphicsBuffer *Display::bufferForResource(wl_resource *resource)
{
    if (auto buffer = LinuxDmaBufV1ClientBuffer::get(resource)) {
        return buffer;
    } else if (auto buffer = ShmClientBuffer::get(resource)) {
        return buffer;
    } else {
        return nullptr;
    }
}

SecurityContext::SecurityContext(Display *display, FileDescriptor &&listenFd, FileDescriptor &&closeFd, const QString &appId)
    : QObject(display)
    , m_display(display)
    , m_listenFd(std::move(listenFd))
    , m_closeFd(std::move(closeFd))
    , m_appId(appId)
{
    qCDebug(KWIN_CORE) << "Adding listen fd for" << appId;

    auto closeSocketWatcher = new QSocketNotifier(m_closeFd.get(), QSocketNotifier::Read, this);
    connect(closeSocketWatcher, &QSocketNotifier::activated, this, &SecurityContext::onCloseFdActivated);

    if (m_closeFd.isClosed()) {
        deleteLater();
        return;
    }

    auto listenFdListener = new QSocketNotifier(m_listenFd.get(), QSocketNotifier::Read, this);
    connect(listenFdListener, &QSocketNotifier::activated, this, &SecurityContext::onListenFdActivated);
}

SecurityContext::~SecurityContext()
{
    qCDebug(KWIN_CORE) << "Removing listen fd for " << m_appId;
}

void SecurityContext::onListenFdActivated(QSocketDescriptor socketDescriptor)
{
    int clientFd = accept4(socketDescriptor, nullptr, nullptr, SOCK_CLOEXEC);
    if (clientFd < 0) {
        qCWarning(KWIN_CORE) << "Failed to accept client from security listen FD";
        return;
    }
    auto client = m_display->createClient(clientFd);
    client->setSecurityContextAppId(m_appId);
}

void SecurityContext::onCloseFdActivated()
{
    if (m_closeFd.isClosed()) {
        deleteLater();
    }
}

} // namespace KWin

#include "moc_display.cpp"
