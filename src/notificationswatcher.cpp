#include "notificationswatcher.h"

#include "lipsticknotification.h"
#include "notification.h"
#include "desktopfilesortmodel.h"

#include "sailfishapp.h"
#include <QGuiApplication>
#include <QtQml>

#include <QTimer>

#include <QtDBus/private/qdbusmessage_p.h>

#include <QDebug>

extern "C" {
static DBusHandlerResult
qDBusSignalFilter(DBusConnection *connection, DBusMessage *message, void *data)
{
    Q_UNUSED(data);
    Q_UNUSED(connection);
    NotificationsWatcher *d = static_cast<NotificationsWatcher *>(data);

    return d->handleRawMessage(message) ?
        DBUS_HANDLER_RESULT_HANDLED :
        DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
}

NotificationsWatcher::NotificationsWatcher(QObject *parent) :
    QDBusVirtualObject(parent)
{
    notifIface = new QDBusInterface("org.freedesktop.Notifications",
                                    "/org/freedesktop/Notifications",
                                    "org.freedesktop.Notifications",
                                    QDBusConnection::sessionBus(), this);
    dconf = new MDConfAgent("/apps/android-notifications/", this);
    dconf->watchKey("dummyId");
    dconf->watchKey("mode");
    dconf->watchKey("whitelist");
    dconf->watchKey("blacklist");
    dconf->watchKey("defaultCategory");
    dconf->watchKey("category_silent");
    dconf->watchKey("category_chat");
    dconf->watchKey("category_sms");
    dconf->watchKey("category_email");
    dconf->watchKey("category_android_notify");
    dconf->setValue("dummyId", 0);

    view = NULL;
}

void NotificationsWatcher::start()
{
    bool ready = QDBusConnection::sessionBus().registerService("org.coderus.androidnotifications");

    if (!ready) {
        qWarning() << "Service already registered, exiting...";
        QGuiApplication::quit();
        return;
    }
    else {
        qDebug() << "Service registered successfully!";

        DBusError error;
        dbus_error_init (&error);

        DBusConnection *connection = static_cast<DBusConnection *>(QDBusConnection::sessionBus().internalPointer());

        dbus_bus_add_match(connection,
                           "eavesdrop='true',type='method_return'",
                           &error);
        if (dbus_error_is_set (&error)) {
            qWarning() << "Failed to set filter match rule:" << error.message;
            dbus_error_free (&error);
        }

        dbus_connection_add_filter(connection, qDBusSignalFilter, this, 0);

        qDBusRegisterMetaType<QVariantHash>();
        qDBusRegisterMetaType<LipstickNotification>();
        qDBusRegisterMetaType<NotificationList>();

        QDBusConnection::sessionBus().registerVirtualObject("/", this, QDBusConnection::SubPath);

        QDBusInterface busInterface("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus");
        busInterface.call(QDBus::NoBlock, "AddMatch", "eavesdrop='true',type='method_call',interface='org.freedesktop.Notifications',member='Notify'");

        qmlRegisterType<DesktopFileSortModel>("org.coderus.androidnotifications", 1, 0, "DesktopFileSortModel");
    }
}

QString NotificationsWatcher::introspect(const QString &path) const
{
    if (path == "/") {
        return "<node name=\"org\"/>";
    }
    else if (path == "/org") {
        return "<node name=\"coderus\"/>";
    }
    else if (path == "/org/coderus") {
        return "<node name=\"androidnotifications\"/>";
    }
    else if (path == "/org/coderus/androidnotifications") {
        return "<interface name=\"org.coderus.androidnotifications\">\
                    <method name=\"showUI\">\
                        <arg direction=\"in\" type=\"as\" name=\"params\"/>\
                        <annotation name=\"org.freedesktop.DBus.Method.NoReply\" value=\"true\"/>\
                    </method>\
                </interface>";
    }

    return "";
}

bool NotificationsWatcher::handleMessage(const QDBusMessage &message, const QDBusConnection &connection)
{
    const QVariantList dbusArguments = message.arguments();
    const QString member = message.member();
    const QString interface = message.interface();

    if (interface == "org.freedesktop.Notifications" && member == "Notify") {
        if (handleNotify(dbusArguments)) {
            QDBusError error;
            DBusMessage *msg = QDBusMessagePrivate::toDBusMessage(message, QDBusConnection::UnixFileDescriptorPassing, &error);
            int serial = dbus_message_get_serial(msg);
            pendingSerials.append(serial);
        }

        return true;
    }
    else if (interface == "org.coderus.androidnotifications" && member == "showUI") {
        QDBusMessage reply = message.createReply();
        connection.call(reply, QDBus::NoBlock);

        showUI();

        return true;
    }

    return false;
}

bool NotificationsWatcher::handleRawMessage(DBusMessage *msg)
{
    int type = dbus_message_get_type(msg);
    if (type == QDBusMessage::ReplyMessage) {
        int serial = dbus_message_get_reply_serial(msg);

        if (pendingSerials.contains(serial)) {
            DBusMessageIter iter;
            dbus_message_iter_init(msg, &iter);
            if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32) {
                uint value;
                dbus_message_iter_get_basic(&iter, &value);
                QTimer *timer = new QTimer(this);
                timer->setSingleShot(true);
                signalTimers.insert(timer, value);
                QObject::connect(timer, SIGNAL(timeout()), this, SLOT(timerTimeout()));
                timer->start(1);
            }
            pendingSerials.removeAll(serial);
        }
    }

    return false;
}

void NotificationsWatcher::handleNotification(uint id)
{
    QDBusReply<NotificationList> reply = notifIface->call("GetNotifications", "AndroidNotification");
    if (reply.isValid()) {
        NotificationList notifications = reply.value();
        foreach (LipstickNotification *lipsticknotification, notifications.notifications()) {
            if (lipsticknotification->replacesId() == id) {
                QString package = lipsticknotification->icon().split("/").last();
                package.chop(14); // waiting for proper solution

                if (dconf->value("mode", 0) == 0) { // whitelist
                    QStringList whitelist = dconf->value("whitelist").toStringList();
                    if (!whitelist.contains(package)) {
                        return;
                    }
                }
                else { // blacklist
                    QStringList blacklist = dconf->value("blacklist").toStringList();
                    if (blacklist.contains(package)) {
                        return;
                    }
                }

                QString category = dconf->value("defaultCategory", "android_notify").toString();

                if (dconf->value("category_silent", QStringList()).toStringList().contains(package)) {
                    category = "";
                }
                else if (dconf->value("category_chat", QStringList()).toStringList().contains(package)) {
                    category = "chat";
                }
                else if (dconf->value("category_sms", QStringList()).toStringList().contains(package)) {
                    category = "sms";
                }
                else if (dconf->value("category_email", QStringList()).toStringList().contains(package)) {
                    category = "email";
                }
                else if (dconf->value("category_android_notify", QStringList()).toStringList().contains(package)) {
                    category = "android_notify";
                }

                if (!category.isEmpty()) {
                    qDebug() << "Faking sound with" << category;
                    Notification dummy;
                    if (dconf->value("dummyId").toUInt() > 0) {
                        dummy.setReplacesId(dconf->value("dummyId").toUInt());
                        dummy.close();
                    }
                    dummy.setHintValue(LipstickNotification::HINT_FEEDBACK, category);
                    dummy.publish();
                    dconf->setValue("dummyId", dummy.replacesId());
                }

                qDebug() << "Replacing notification" << lipsticknotification->replacesId();
                Notification notification;
                notification.setAppName("AndroidNotification");
                notification.setSummary(lipsticknotification->summary());
                notification.setBody(lipsticknotification->body());
                if (lipsticknotification->hints().contains(LipstickNotification::HINT_PREVIEW_SUMMARY)) {
                    notification.setPreviewSummary(lipsticknotification->hints().value(LipstickNotification::HINT_PREVIEW_SUMMARY).toString());
                }
                else {
                    notification.setPreviewSummary(lipsticknotification->summary());
                }
                if (lipsticknotification->hints().contains(LipstickNotification::HINT_PREVIEW_BODY)) {
                    notification.setPreviewBody(lipsticknotification->hints().value(LipstickNotification::HINT_PREVIEW_BODY).toString());
                }
                else {
                    notification.setPreviewBody(lipsticknotification->body());
                }
                if (lipsticknotification->hints().contains(LipstickNotification::HINT_PREVIEW_ICON)) {
                    notification.setHintValue(LipstickNotification::HINT_PREVIEW_ICON, lipsticknotification->hints().value(LipstickNotification::HINT_PREVIEW_ICON).toString());
                }
                else {
                    notification.setHintValue(LipstickNotification::HINT_PREVIEW_ICON, lipsticknotification->appIcon());
                }
                if (lipsticknotification->hints().contains(LipstickNotification::HINT_ICON)) {
                    notification.setHintValue(LipstickNotification::HINT_ICON, lipsticknotification->hints().value(LipstickNotification::HINT_PREVIEW_ICON).toString());
                }
                else {
                    notification.setHintValue(LipstickNotification::HINT_ICON, lipsticknotification->appIcon());
                }
                notification.setHintValue(LipstickNotification::HINT_PRIORITY, QString("100"));
                if (!category.isEmpty()) {
                    notification.setHintValue(LipstickNotification::HINT_FEEDBACK, category);
                }
                notification.setReplacesId(lipsticknotification->replacesId());
                notification.publish();
                break;
            }
        }
    }
    else {
        qWarning() << reply.error().message();
    }
}

void NotificationsWatcher::showUI()
{
    if (!view) {
        qDebug() << "Construct view";
        view = SailfishApp::createView();
        QObject::connect(view, SIGNAL(destroyed()), this, SLOT(onViewDestroyed()));
        QObject::connect(view, SIGNAL(closing(QQuickCloseEvent*)), this, SLOT(onViewClosing(QQuickCloseEvent*)));
        view->setTitle("Android notifications");
        view->setSource(SailfishApp::pathTo("qml/main.qml"));
        view->showFullScreen();
    }
    else if (view->windowState() == Qt::WindowNoState) {
        qDebug() << "Create view";
        view->create();
        view->showFullScreen();
    }
    else {
        qDebug() << "Show view";
        view->raise();
        view->requestActivate();
    }
}

QVariant NotificationsWatcher::parse(const QDBusArgument &argument)
{
    switch (argument.currentType()) {
    case QDBusArgument::BasicType: {
        QVariant v = argument.asVariant();
        if (v.userType() == qMetaTypeId<QDBusObjectPath>())
            return v.value<QDBusObjectPath>().path();
        else if (v.userType() == qMetaTypeId<QDBusSignature>())
            return v.value<QDBusSignature>().signature();
        else
            return v;
    }
    case QDBusArgument::VariantType: {
        QVariant v = argument.asVariant().value<QDBusVariant>().variant();
        if (v.userType() == qMetaTypeId<QDBusArgument>())
            return parse(v.value<QDBusArgument>());
        else
            return v;
    }
    case QDBusArgument::ArrayType: {
        QVariantList list;
        argument.beginArray();
        while (!argument.atEnd())
            list.append(parse(argument));
        argument.endArray();
        return list;
    }
    case QDBusArgument::StructureType: {
        QVariantList list;
        argument.beginStructure();
        while (!argument.atEnd())
            list.append(parse(argument));
        argument.endStructure();
        return QVariant::fromValue(list);
    }
    case QDBusArgument::MapType: {
        QVariantMap map;
        argument.beginMap();
        while (!argument.atEnd()) {
            argument.beginMapEntry();
            QVariant key = parse(argument);
            QVariant value = parse(argument);
            map.insert(key.toString(), value);
            argument.endMapEntry();
        }
        argument.endMap();
        return map;
    }
    default:
        return QVariant();
        break;
    }
}

bool NotificationsWatcher::handleNotify(const QVariantList &arguments)
{
    QString appName = arguments.value(0).toString();
    QString appIcon = arguments.value(2).toString();
    QString summary = arguments.value(3).toString();
    QString body = arguments.value(4).toString();
    QVariant arg6 = arguments.value(6);
    QVariantMap hints;
    if (arg6.userType() == qMetaTypeId<QDBusArgument>()) {
        hints = parse(arg6.value<QDBusArgument>()).toMap();
    }
    else
        hints = arg6.value<QDBusVariant>().variant().toMap();

    if (appName == "AndroidNotification"
            && !body.isEmpty()
            && !summary.isEmpty()
            && !appIcon.isEmpty()
            && !hints.contains(LipstickNotification::HINT_PRIORITY)
            ) {
        return true;
    }

    return false;
}

void NotificationsWatcher::onViewDestroyed()
{
    qDebug() << "Window destroyed";
    view = NULL;
}

void NotificationsWatcher::onViewClosing(QQuickCloseEvent *)
{
    qDebug() << "Window closed";
    delete view;
}

void NotificationsWatcher::timerTimeout()
{
    if (signalTimers.contains(sender())) {
        uint id = signalTimers.value(sender());
        signalTimers.remove(sender());
        delete sender();

        handleNotification(id);
    }
}
