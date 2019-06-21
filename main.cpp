/*
 * Copyright (C) 2017 ~ 2019 Deepin Technology Co., Ltd.
 *
 * Author:     zccrs <zccrs@live.com>
 *
 * Maintainer: zccrs <zhangjide@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "mainwindow.h"

#include <DWindowManagerHelper>
#include <DForeignWindow>
#include <DApplication>

#include <QApplication>
#include <QX11Info>
#include <QDebug>
#include <QAbstractNativeEventFilter>
#include <QTimer>
#include <QScreen>
#include <QDateTime>
#include <QDir>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QProcess>

#include <xcb/xcb.h>

DWIDGET_USE_NAMESPACE

class WindowPixmapChecker : public QObject
{
    Q_OBJECT
public:
    WindowPixmapChecker(DForeignWindow *w, qint64 start_time)
        : QObject(w)
        , window(w)
        , begin_time(start_time) {
        map_time = QDateTime::currentMSecsSinceEpoch();
        timer = new QTimer(this);
        timer->setInterval(100);
        connect(timer, &QTimer::timeout, this, &WindowPixmapChecker::updateWindowPixmap);
        QDir::current().mkpath("/tmp/test");
        updateWindowPixmap();
        timer->start();

        qDebug() << Q_FUNC_INFO << w->title() << w->winId();
    }

    ~WindowPixmapChecker() {
        qDebug() << Q_FUNC_INFO << window->title() << window->winId();
    }

    void updateWindowPixmap() {
        xcb_get_geometry_cookie_t cookie = xcb_get_geometry(QX11Info::connection(), window->winId());
        xcb_generic_error_t *error = nullptr;
        xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(QX11Info::connection(), cookie, &error);

        if (error || !reply) {
            qWarning() << "window destoyed:" << window->title() << window->winId();
            timer->stop();
            emit finished();
            if (reply) free(reply);
            return;
        } else {
            free(reply);
        }

        QImage origin = qApp->primaryScreen()->grabWindow(window->winId()).toImage();

        if (origin.isNull()) {
            return;
        }

        QImage shot = origin.scaledToWidth(20);
        qint64 current_time = QDateTime::currentMSecsSinceEpoch();

        if (last_image.isNull()) {
            last_image = shot;
            first_time = current_time;
            return;
        }

        if (shot.byteCount() == last_image.byteCount()) {
            if (memcmp(shot.constBits(), last_image.constBits(), shot.byteCount()) == 0) {
                if (current_time - last_time >= 500) {
                    qDebug() << Q_FUNC_INFO << origin.save(QString("/tmp/test/%1+%2s+%3s+%4s.png").arg(window->title()).arg((map_time - begin_time) / 1000.0).arg((first_time - begin_time) / 1000.0).arg((last_time - begin_time) / 1000.0));
                    timer->stop();
                    emit finished();
                    return;
                }
            } else {
                last_time = current_time;
            }
        } else {
            last_time = current_time;
        }

        last_image = shot;

        if (last_time - first_time > 5000 && last_time != INT64_MAX) {
            qWarning() << "window timeout:" << window->title() << window->winId() << last_time << first_time;
            timer->stop();
            emit finished();
        }
    }

    DForeignWindow *window;
    QImage last_image;
    qint64 map_time;
    qint64 first_time;
    qint64 begin_time;
    qint64 last_time = INT64_MAX;
    QTimer *timer;

signals:
    void finished();
};

class EventFilter : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.deepin.WindowTest")
    Q_CLASSINFO("D-Bus Introspection", ""
      "  <interface name=\"com.deepin.WindowTest\">\n"
      "    <method name=\"Start\">\n"
      "      <arg direction=\"in\" type=\"s\" name=\"desktop\"/>\n"
      "      <arg direction=\"in\" type=\"as\" name=\"args\"/>\n"
      "      <arg direction=\"in\" type=\"s\" name=\"windowTitle\"/>\n"
      "      <arg direction=\"out\" type=\"b\" name=\"ok\"/>\n"
      "    </method>\n"
      "  </interface>\n"
              "")
public:
    enum {
        baseEventMask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE
    };

    EventFilter()
        : QObject(nullptr)
    {
        const quint32 mask = XCB_CW_EVENT_MASK;
        const quint32 values[] = {
            // XCB_CW_EVENT_MASK
            baseEventMask
        };

        xcb_change_window_attributes(QX11Info::connection(), QX11Info::appRootWindow(), mask, values);
        windowList = DWindowManagerHelper::instance()->allWindowIdList();
        connect(DWindowManagerHelper::instance(), &DWindowManagerHelper::windowListChanged, this, &EventFilter::onWindowListChanged);
    }

    bool nativeEventFilter(const QByteArray &eventType, void *message, long *result) {
        xcb_generic_event_t *event = reinterpret_cast<xcb_generic_event_t*>(message);
        uint response_type = event->response_type & ~0x80;

        if (response_type == XCB_MAP_NOTIFY) {
            xcb_map_notify_event_t *map_event = reinterpret_cast<xcb_map_notify_event_t*>(event);

            if (map_event->event != QX11Info::appRootWindow())
                return false;

            DForeignWindow *window = DForeignWindow::fromWinId(map_event->window);

            if (window->wmClass() == "sogou-qimpanel" || window->type() == Qt::Widget)
                return false;

            qDebug() << "window map, title=" << window->title() << ", wm class=" << window->wmClass() << ",type=" << window->type() << ", window id=" << map_event->window;

            if (window->title().isEmpty() || (!listingWindowMap.contains(window->title()) && !listingWindowMap.contains("*"))) {
                window->deleteLater();
                return false;
            }

            auto checker = new WindowPixmapChecker(window, listingWindowMap.contains(window->title()) ? listingWindowMap.take(window->title()) : listingWindowMap.take("*"));
            m_checkerMap[map_event->window] = checker;

            connect(checker, &WindowPixmapChecker::finished, this, [checker, this] {
                m_checkerMap.remove(checker->window->winId());
                checker->deleteLater();
            });
        } else if (response_type == XCB_UNMAP_NOTIFY) {
            xcb_unmap_notify_event_t *map_event = reinterpret_cast<xcb_unmap_notify_event_t*>(event);

            if (map_event->event != QX11Info::appRootWindow())
                return false;

            if (WindowPixmapChecker *checker = m_checkerMap.take(map_event->window)) {
                DForeignWindow *window = checker->window;
                qDebug() << "window unmap, title=" << window->title() << ", wm class=" << window->wmClass() << ",type=" << window->type() << ", window id=" << map_event->window;
                window->deleteLater();
            }
        }

        return false;
    }

    Q_SLOT bool Start(QString desktop, const QStringList &args, const QString &windowTitle) {
        listingWindowMap.remove("*");
        qDeleteAll(m_checkerMap.values());
        m_checkerMap.clear();

        if (desktop.endsWith(".desktop")) {
            if (!desktop.startsWith("/"))
                desktop = "/usr/share/applications/" + desktop;

            QDBusInterface("com.deepin.SessionManager", "/com/deepin/StartManager", "com.deepin.StartManager").call("LaunchApp", desktop, 0u, args);

            if (QDBusConnection::sessionBus().lastError().type() != QDBusError::NoError) {
                qDebug( )<< QDBusConnection::sessionBus().lastError().message();
                QDBusConnection::sessionBus().send(QDBusMessage::createError(QDBusConnection::sessionBus().lastError()));
                return false;
            }
        } else {
            if (!QProcess::startDetached(desktop, args))
                return false;
        }

        listingWindowMap[windowTitle.isEmpty() ? "*" : windowTitle] = QDateTime::currentMSecsSinceEpoch();

        return true;
    }

    void onWindowListChanged() {
        QVector<quint32> new_window_list = DWindowManagerHelper::instance()->allWindowIdList();

        if (listingWindowMap.isEmpty())
            return;

        for (quint32 wid : new_window_list) {
            if (windowList.contains(wid))
                continue;

            // new window
            DForeignWindow *window = DForeignWindow::fromWinId(wid);

            if (window->type() == Qt::Widget) {
                window->deleteLater();
                continue;
            }

            qDebug() << "new window, title=" << window->title() << ", wm class=" << window->wmClass() << ",type=" << window->type() << ", window id=" << wid;

            auto checker = new WindowPixmapChecker(window, listingWindowMap.contains(window->title()) ? listingWindowMap.take(window->title()) : listingWindowMap.value("*"));
            m_checkerMap[wid] = checker;

            connect(checker, &WindowPixmapChecker::finished, this, [wid, this, checker] {
                m_checkerMap.remove(wid);
                checker->deleteLater();
            });
        }

        windowList = new_window_list;
    }

    QMap<QString, qint64> listingWindowMap;
    QMap<WId, WindowPixmapChecker*> m_checkerMap;
    QVector<quint32> windowList;
};

int main(int argc, char *argv[])
{
    DApplication::loadDXcbPlugin();
    QApplication a(argc, argv);

    EventFilter *filter = new EventFilter();

    QDBusConnection::sessionBus().registerService("com.deepin.WindowTest");
    QDBusConnection::sessionBus().registerObject("/", "com.deepin.WindowTest", filter, QDBusConnection::ExportAllSlots);
    a.installNativeEventFilter(filter);

    return a.exec();
}

#include "main.moc"
