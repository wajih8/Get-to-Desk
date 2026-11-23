#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

class SignalingClient final : public QObject
{
    Q_OBJECT

public:
    enum class Connection { Disconnected, Connecting, Ready };
    enum class Role { None, Host, Viewer };
    enum class Phase {
        Idle, CreatingInvitation, InvitationOpen, RequestingAccess, WaitingForHost,
        HostApproval, Accepting, Declining, Active, Ending
    };

    struct Pairing {
        Role role = Role::None;
        Phase phase = Phase::Idle;
        QString displayId;
        QString invitationId;
        QString invitationCode;
        QString accessId;
        QString hostPeerId;
        QString viewerPeerId;
        QString viewerName;
        QString sessionId;
        QStringList permissions;
        QDateTime expiresAt;
    };

    explicit SignalingClient(QObject *parent = nullptr);

    Connection connection() const { return connection_; }
    const Pairing &pairing() const { return pairing_; }
    const QString &notice() const { return notice_; }

    void connectTo(const QUrl &url);
    void disconnectFromServer(const QString &notice = QStringLiteral("Disconnected."));
    void createInvitation(const QString &displayId, int ttlSeconds);
    void requestAccess(const QString &code, const QString &viewerName);
    void acceptAccess();
    void declineAccess();
    void endSession();

    static bool validInvitationCode(const QString &code);
    static bool validViewerName(const QString &name);

signals:
    void changed();

private:
    void receive(const QString &text);
    void send(const QString &type, Phase next, QJsonObject fields, const QString &notice);
    void update(const QString &notice);
    void resetPairing();
    void completeCommand();
    bool isReply(const QJsonObject &event) const;
    void protocolError();

    void invitationCreated(const QJsonObject &event);
    void accessRequested(const QJsonObject &event);
    void accessPending(const QJsonObject &event);
    void accessClosed(const QJsonObject &event);
    void sessionStarted(const QJsonObject &event);
    void sessionEnded(const QJsonObject &event);
    void commandError(const QJsonObject &event);

    QWebSocket socket_;
    QTimer replyTimer_;
    QTimer grantTimer_;
    Connection connection_ = Connection::Disconnected;
    Pairing pairing_;
    QString peerId_;
    QString requestId_;
    QString notice_ = QStringLiteral("Connect to the signaling server to start pairing.");
};
