// Test-only read-only oracle: accept snapshots only from the real session KWin.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusError>
#include <QDBusVirtualObject>
#include <QJsonDocument>
#include <QSaveFile>
#include <cstdio>
class WindowSnapshot final : public QDBusVirtualObject {
public:
    explicit WindowSnapshot(QString path) : output(std::move(path)) {}
    QString introspect(const QString &) const override {
        return QStringLiteral("<interface name=\"org.linuxrunner.TestWindowWatch\"><method name=\"snapshot\"><arg type=\"s\" direction=\"in\"/><arg type=\"b\" direction=\"out\"/></method></interface>");
    }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
        if (message.interface() != QStringLiteral("org.linuxrunner.TestWindowWatch") || message.member() != QStringLiteral("snapshot")) return false;
        const auto owner = connection.interface()->serviceOwner(QStringLiteral("org.kde.KWin"));
        if (!owner.isValid() || owner.value() != message.service() || message.arguments().size() != 1) {
            connection.send(message.createErrorReply(QDBusError::AccessDenied, QStringLiteral("Only the real KWin session owner may publish window state"))); return true;
        }
        const QByteArray bytes = message.arguments().first().toString().toUtf8();
        QJsonParseError error; const auto json = QJsonDocument::fromJson(bytes, &error);
        bool ok = bytes.size() <= 32768 && error.error == QJsonParseError::NoError && json.isArray();
        if (ok) {
            QSaveFile file(output); file.setDirectWriteFallback(false);
            ok = file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) && file.write(bytes) == bytes.size() && file.commit();
        }
        if (!ok) std::fprintf(stderr, "KWIN_WINDOW_ORACLE: rejected invalid/unwritable snapshot\n");
        connection.send(message.createReply(QVariantList{ok})); return true;
    }
private: QString output;
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2 || !QString::fromLocal8Bit(argv[1]).startsWith(QStringLiteral("/home/mobile/Downloads/logs/kwin-windows-"))) return 2;
    auto bus = QDBusConnection::sessionBus(); WindowSnapshot object(QString::fromLocal8Bit(argv[1]));
    if (!bus.registerService(QStringLiteral("org.linuxrunner.TestWindowWatch")) || !bus.registerVirtualObject(QStringLiteral("/WindowWatch"), &object)) return 3;
    return app.exec();
}
