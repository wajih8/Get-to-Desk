#include "SignalingClient.h"

#include <QChar>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>

namespace {
constexpr int ReplyTimeoutMs = 8000;
constexpr qint64 MaxGrantMs = 2 * 60 * 60 * 1000;

bool serverId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9a-f]{32}\\z"));
    return pattern.match(id).hasMatch();
}

bool displayId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    return pattern.match(id).hasMatch();
}

QDateTime expiry(const QJsonObject &event)
{
    const auto value = event.value(QStringLiteral("expires_at")).toString();
    return value.endsWith(QLatin1Char('Z'))
        ? QDateTime::fromString(value, Qt::ISODateWithMs) : QDateTime{};
}

QString errorNotice(const QString &code)
{
    if (code == QStringLiteral("invitation_not_found")) return QStringLiteral("Invitation code not found. Check the code or ask for a new one.");
    if (code == QStringLiteral("invitation_expired")) return QStringLiteral("The invitation expired. Ask the host for a new one.");
    if (code == QStringLiteral("invitation_used")) return QStringLiteral("The invitation has already been used. Ask for a new one.");
    if (code == QStringLiteral("self_request")) return QStringLiteral("Open a second app instance to use this invitation.");
    if (code == QStringLiteral("request_not_found")) return QStringLiteral("The access request is closed. Create a new invitation.");
    if (code == QStringLiteral("session_not_found")) return QStringLiteral("The session is already closed.");
    if (code == QStringLiteral("forbidden")) return QStringLiteral("This connection is not authorized for that action.");
    if (code == QStringLiteral("invalid_message")) return QStringLiteral("The server rejected the request as invalid.");
    if (code == QStringLiteral("rate_limited")) return QStringLiteral("Too many attempts. Try again in a minute.");
    if (code == QStringLiteral("capacity_reached")) return QStringLiteral("The server is at capacity. Try again shortly.");
    return QStringLiteral("The signaling server rejected the request.");
}

QString closureNotice(const QString &reason)
{
    if (reason == QStringLiteral("declined")) return QStringLiteral("The host declined access.");
    if (reason == QStringLiteral("invitation_expired")) return QStringLiteral("The invitation expired while awaiting approval.");
    if (reason == QStringLiteral("host_disconnected")) return QStringLiteral("The host disconnected.");
    if (reason == QStringLiteral("viewer_disconnected")) return QStringLiteral("The viewer disconnected.");
    if (reason == QStringLiteral("ended_by_host")) return QStringLiteral("The host ended the session.");
    if (reason == QStringLiteral("ended_by_viewer")) return QStringLiteral("The viewer ended the session.");
    if (reason == QStringLiteral("session_expired")) return QStringLiteral("The session grant expired.");
    return QStringLiteral("The server closed the connection request or session.");
}
}

SignalingClient::SignalingClient(QObject *parent) : QObject(parent)
{
    socket_.setMaxAllowedIncomingMessageSize(4096);
    replyTimer_.setSingleShot(true);
    grantTimer_.setSingleShot(true);
    grantTimer_.setTimerType(Qt::PreciseTimer);

    connect(&socket_, &QWebSocket::textMessageReceived, this, &SignalingClient::receive);
    connect(&socket_, &QWebSocket::binaryMessageReceived, this, &SignalingClient::protocolError);
    connect(&socket_, &QWebSocket::disconnected, this, [this] {
        if (connection_ != Connection::Disconnected) {
            disconnectFromServer(QStringLiteral("The signaling connection closed. Reconnect for a new invitation."));
        }
    });
    connect(&socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (connection_ != Connection::Disconnected) {
            disconnectFromServer(QStringLiteral("Could not use the signaling connection. %1").arg(socket_.errorString()));
        }
    });
    connect(&replyTimer_, &QTimer::timeout, this, [this] {
        disconnectFromServer(QStringLiteral("The signaling server timed out. Reconnect to try again."));
    });
    connect(&grantTimer_, &QTimer::timeout, this, &SignalingClient::endSession);
}

bool SignalingClient::validInvitationCode(const QString &code)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{32}\\z"));
    return pattern.match(code).hasMatch();
}

bool SignalingClient::validViewerName(const QString &name)
{
    const auto characters = name.toUcs4();
    if (characters.isEmpty() || characters.size() > 64 || name != name.trimmed()) {
        return false;
    }
    return std::all_of(characters.cbegin(), characters.cend(), [](char32_t point) {
        return point == U' ' || QChar::isLetter(point) || QChar::isMark(point) || QChar::isNumber(point)
            || QChar::isPunct(point) || QChar::isSymbol(point);
    });
}

void SignalingClient::connectTo(const QUrl &url)
{
    if (connection_ != Connection::Disconnected) return;
    connection_ = Connection::Connecting;
    replyTimer_.start(ReplyTimeoutMs);
    update(QStringLiteral("Connecting to the signaling server…"));
    socket_.open(url);
}

void SignalingClient::disconnectFromServer(const QString &notice)
{
    connection_ = Connection::Disconnected;
    peerId_.clear();
    resetPairing();
    socket_.abort();
    update(notice);
}

void SignalingClient::createInvitation(const QString &display, int ttlSeconds)
{
    if (connection_ != Connection::Ready || pairing_.phase != Phase::Idle
        || !displayId(display) || ttlSeconds < 1 || ttlSeconds > 300) return;
    pairing_.role = Role::Host;
    pairing_.displayId = display;
    send(QStringLiteral("create_invitation"), Phase::CreatingInvitation,
         {{QStringLiteral("display_id"), display}, {QStringLiteral("ttl_seconds"), ttlSeconds}},
         QStringLiteral("Creating an invitation…"));
}

void SignalingClient::requestAccess(const QString &code, const QString &viewerName)
{
    if (connection_ != Connection::Ready || pairing_.phase != Phase::Idle
        || !validInvitationCode(code) || !validViewerName(viewerName)) return;
    pairing_.role = Role::Viewer;
    send(QStringLiteral("request_access"), Phase::RequestingAccess,
         {{QStringLiteral("invitation_code"), code}, {QStringLiteral("viewer_name"), viewerName}},
         QStringLiteral("Sending the access request…"));
}

void SignalingClient::acceptAccess()
{
    if (pairing_.phase != Phase::HostApproval) return;
    send(QStringLiteral("accept_access"), Phase::Accepting,
         {{QStringLiteral("access_request_id"), pairing_.accessId},
          {QStringLiteral("permissions"), QJsonArray{QStringLiteral("view")}}},
         QStringLiteral("Approving view access…"));
}

void SignalingClient::declineAccess()
{
    if (pairing_.phase != Phase::HostApproval) return;
    send(QStringLiteral("decline_access"), Phase::Declining,
         {{QStringLiteral("access_request_id"), pairing_.accessId}},
         QStringLiteral("Declining the request…"));
}

void SignalingClient::endSession()
{
    if (pairing_.phase != Phase::Active) return;
    grantTimer_.stop();
    pairing_.permissions.clear();
    send(QStringLiteral("end_session"), Phase::Ending,
         {{QStringLiteral("session_id"), pairing_.sessionId}}, QStringLiteral("Ending the session…"));
}

void SignalingClient::send(const QString &type, Phase next, QJsonObject fields, const QString &notice)
{
    requestId_ = QUuid::createUuid().toString(QUuid::Id128);
    fields.insert(QStringLiteral("v"), 1);
    fields.insert(QStringLiteral("type"), type);
    fields.insert(QStringLiteral("request_id"), requestId_);
    pairing_.phase = next;
    replyTimer_.start(ReplyTimeoutMs);
    update(notice);
    const auto text = QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact));
    if (socket_.sendTextMessage(text) <= 0 && connection_ != Connection::Disconnected) {
        disconnectFromServer(QStringLiteral("The signaling connection could not send the request."));
    }
}

void SignalingClient::update(const QString &notice)
{
    notice_ = notice;
    emit changed();
}

void SignalingClient::resetPairing()
{
    completeCommand();
    grantTimer_.stop();
    pairing_ = {};
}

void SignalingClient::completeCommand()
{
    requestId_.clear();
    replyTimer_.stop();
}

bool SignalingClient::isReply(const QJsonObject &event) const
{
    return !requestId_.isEmpty() && event.value(QStringLiteral("request_id")).toString() == requestId_;
}

void SignalingClient::protocolError()
{
    if (connection_ != Connection::Disconnected) {
        disconnectFromServer(QStringLiteral("The server sent an incompatible signaling message."));
    }
}

void SignalingClient::receive(const QString &text)
{
    if (connection_ == Connection::Disconnected) return;
    const auto document = QJsonDocument::fromJson(text.toUtf8());
    const auto event = document.object();
    const auto type = event.value(QStringLiteral("type")).toString();
    if (!document.isObject() || event.value(QStringLiteral("v")).toDouble() != 1 || type.isEmpty()) {
        protocolError();
        return;
    }

    if (connection_ == Connection::Connecting) {
        const auto id = event.value(QStringLiteral("peer_id")).toString();
        if (type != QStringLiteral("ready") || !serverId(id) || event.contains(QStringLiteral("request_id"))) {
            protocolError();
            return;
        }
        peerId_ = id;
        completeCommand();
        connection_ = Connection::Ready;
        update(QStringLiteral("Connected to the signaling server."));
        return;
    }

    if (type == QStringLiteral("invitation_created")) {
        invitationCreated(event);
    } else if (type == QStringLiteral("access_requested")) {
        accessRequested(event);
    } else if (type == QStringLiteral("access_pending")) {
        accessPending(event);
    } else if (type == QStringLiteral("access_closed")) {
        accessClosed(event);
    } else if (type == QStringLiteral("session_started")) {
        sessionStarted(event);
    } else if (type == QStringLiteral("session_ended")) {
        sessionEnded(event);
    } else if (type == QStringLiteral("invitation_expired")) {
        if (pairing_.phase == Phase::InvitationOpen
            && event.value(QStringLiteral("invitation_id")).toString() == pairing_.invitationId) {
            resetPairing();
            update(QStringLiteral("The invitation expired. Create a new one to continue."));
        }
    } else if (type == QStringLiteral("error")) {
        commandError(event);
    } else {
        protocolError();
    }
}

void SignalingClient::invitationCreated(const QJsonObject &event)
{
    if (pairing_.phase != Phase::CreatingInvitation || !isReply(event)) return;
    const auto id = event.value(QStringLiteral("invitation_id")).toString();
    const auto code = event.value(QStringLiteral("invitation_code")).toString();
    const auto deadline = expiry(event);
    if (!serverId(id) || !validInvitationCode(code) || !deadline.isValid()
        || event.value(QStringLiteral("display_id")).toString() != pairing_.displayId) {
        protocolError();
        return;
    }
    completeCommand();
    pairing_.phase = Phase::InvitationOpen;
    pairing_.invitationId = id;
    pairing_.invitationCode = code;
    pairing_.expiresAt = deadline;
    update(QStringLiteral("Share the invitation code with the intended viewer."));
}

void SignalingClient::accessRequested(const QJsonObject &event)
{
    if (pairing_.phase != Phase::InvitationOpen
        || event.value(QStringLiteral("invitation_id")).toString() != pairing_.invitationId) return;
    const auto id = event.value(QStringLiteral("access_request_id")).toString();
    const auto viewer = event.value(QStringLiteral("viewer_peer_id")).toString();
    const auto name = event.value(QStringLiteral("viewer_name")).toString();
    if (!serverId(id) || !serverId(viewer) || viewer == peerId_ || !validViewerName(name)
        || event.value(QStringLiteral("display_id")).toString() != pairing_.displayId
        || expiry(event) != pairing_.expiresAt || event.contains(QStringLiteral("request_id"))) {
        protocolError();
        return;
    }
    pairing_.phase = Phase::HostApproval;
    pairing_.accessId = id;
    pairing_.hostPeerId = peerId_;
    pairing_.viewerPeerId = viewer;
    pairing_.viewerName = name;
    pairing_.invitationCode.clear();
    update(QStringLiteral("An access request is waiting for your decision."));
}

void SignalingClient::accessPending(const QJsonObject &event)
{
    if (pairing_.phase != Phase::RequestingAccess || !isReply(event)) return;
    const auto id = event.value(QStringLiteral("access_request_id")).toString();
    const auto invitation = event.value(QStringLiteral("invitation_id")).toString();
    const auto host = event.value(QStringLiteral("host_peer_id")).toString();
    const auto deadline = expiry(event);
    if (!serverId(id) || !serverId(invitation) || !serverId(host) || host == peerId_ || !deadline.isValid()) {
        protocolError();
        return;
    }
    completeCommand();
    pairing_.phase = Phase::WaitingForHost;
    pairing_.accessId = id;
    pairing_.invitationId = invitation;
    pairing_.hostPeerId = host;
    pairing_.viewerPeerId = peerId_;
    pairing_.expiresAt = deadline;
    update(QStringLiteral("Waiting for approval on the host PC."));
}

void SignalingClient::accessClosed(const QJsonObject &event)
{
    const auto phase = pairing_.phase;
    if (phase != Phase::WaitingForHost && phase != Phase::HostApproval
        && phase != Phase::Accepting && phase != Phase::Declining) return;
    if (event.value(QStringLiteral("access_request_id")).toString() != pairing_.accessId
        || event.value(QStringLiteral("invitation_id")).toString() != pairing_.invitationId
        || (event.contains(QStringLiteral("request_id")) && !isReply(event))) return;
    const auto reason = event.value(QStringLiteral("reason")).toString();
    resetPairing();
    update(closureNotice(reason));
}

void SignalingClient::sessionStarted(const QJsonObject &event)
{
    const bool host = pairing_.phase == Phase::Accepting && isReply(event);
    const bool viewer = pairing_.phase == Phase::WaitingForHost && !event.contains(QStringLiteral("request_id"));
    if ((!host && !viewer) || event.value(QStringLiteral("access_request_id")).toString() != pairing_.accessId) return;

    const auto id = event.value(QStringLiteral("session_id")).toString();
    const auto display = event.value(QStringLiteral("display_id")).toString();
    const auto deadline = expiry(event);
    const auto fields = event.value(QStringLiteral("permissions")).toArray();
    QStringList permissions;
    for (const auto field : fields) {
        const auto value = field.toString();
        if ((value != QStringLiteral("view") && value != QStringLiteral("keyboard")
            && value != QStringLiteral("pointer")) || permissions.contains(value)) {
            protocolError();
            return;
        }
        permissions.append(value);
    }
    if (!serverId(id) || !displayId(display) || !deadline.isValid() || !permissions.contains(QStringLiteral("view"))
        || event.value(QStringLiteral("host_peer_id")).toString() != pairing_.hostPeerId
        || event.value(QStringLiteral("viewer_peer_id")).toString() != pairing_.viewerPeerId
        || (host && (display != pairing_.displayId || permissions != QStringList{QStringLiteral("view")}))) {
        protocolError();
        return;
    }
    completeCommand();
    pairing_.phase = Phase::Active;
    pairing_.displayId = display;
    pairing_.sessionId = id;
    pairing_.permissions = permissions;
    pairing_.expiresAt = deadline;
    const qint64 remaining = std::min(QDateTime::currentDateTimeUtc().msecsTo(deadline), MaxGrantMs);
    if (remaining <= 0) {
        endSession();
        return;
    }
    grantTimer_.start(static_cast<int>(remaining));
    update(QStringLiteral("The host approved the session."));
}

void SignalingClient::sessionEnded(const QJsonObject &event)
{
    if ((pairing_.phase != Phase::Active && pairing_.phase != Phase::Ending)
        || event.value(QStringLiteral("session_id")).toString() != pairing_.sessionId
        || (event.contains(QStringLiteral("request_id")) && !isReply(event))) return;
    const auto reason = event.value(QStringLiteral("reason")).toString();
    resetPairing();
    update(closureNotice(reason));
}

void SignalingClient::commandError(const QJsonObject &event)
{
    if (!isReply(event)) return;
    const auto code = event.value(QStringLiteral("code")).toString();
    if (pairing_.phase == Phase::Ending && code != QStringLiteral("session_not_found")) {
        disconnectFromServer(errorNotice(code));
        return;
    }
    if ((pairing_.phase == Phase::Accepting || pairing_.phase == Phase::Declining)
        && (code == QStringLiteral("capacity_reached") || code == QStringLiteral("invalid_message"))) {
        completeCommand();
        pairing_.phase = Phase::HostApproval;
    } else {
        resetPairing();
    }
    update(errorNotice(code));
}
