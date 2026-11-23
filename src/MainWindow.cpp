#include "MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wtsapi32.h>
#endif

namespace {
using Connection = SignalingClient::Connection;
using Role = SignalingClient::Role;
using Phase = SignalingClient::Phase;

QLabel *label(const QString &text = {}, const char *name = "")
{
    auto *result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setObjectName(QString::fromLatin1(name));
    return result;
}

QPushButton *button(const QString &text, const char *name, bool primary = false)
{
    auto *result = new QPushButton(text);
    result->setObjectName(QString::fromLatin1(name));
    result->setProperty("primary", primary);
    return result;
}

QVBoxLayout *card(QHBoxLayout *row, int stretch)
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("card"));
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);
    row->addWidget(frame, stretch);
    return layout;
}

QString configPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config.json"));
}

QString remainingText(const QDateTime &deadline)
{
    if (!deadline.isValid()) return {};
    const auto seconds = (std::max<qint64>(0, QDateTime::currentDateTimeUtc().msecsTo(deadline)) + 999) / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

const char *Style = R"QSS(
    QMainWindow, QTabWidget::pane { background: #f3f6fa; }
    QWidget { color: #16263b; font-size: 13px; }
    QFrame#card { background: #ffffff; border: 1px solid #d3dfeb; border-radius: 12px; }
    QLabel#title { font-size: 24px; font-weight: 700; }
    QLabel#sectionTitle, QLabel#viewerState { font-size: 17px; font-weight: 700; }
    QLabel#endpoint, QLabel#muted { color: #405775; }
    QLabel#notice { color: #193f56; background: #e7f0f5; border-radius: 8px; padding: 10px 12px; }
    QLabel#previewCanvas {
        color: #d5e3ef; background: #16263b; border-radius: 8px; padding: 4px;
    }
    QTabWidget::pane { border: 0; }
    QTabBar::tab {
        color: #405775; padding: 9px 26px; border-bottom: 2px solid transparent;
    }
    QTabBar::tab:selected { color: #0b615b; font-weight: 700; border-bottom-color: #0b7167; }
    QTabBar::tab:disabled { color: #7c8da2; }
    QPushButton, QLineEdit, QComboBox {
        background: #ffffff; border: 1px solid #adbfce; border-radius: 6px; min-height: 24px; padding: 5px 9px;
    }
    QPushButton { font-weight: 600; }
    QPushButton:hover { background: #edf4f8; }
    QPushButton[primary="true"] { background: #0b6d63; color: #ffffff; border-color: #0b6d63; }
    QPushButton[primary="true"]:hover { background: #095a52; }
    QPushButton:disabled, QLineEdit:disabled, QComboBox:disabled {
        background: #edf1f5; color: #65798f; border-color: #d3dfeb;
    }
    QPushButton:focus, QLineEdit:focus, QComboBox:focus { border: 2px solid #177fd1; }
)QSS";
}

MainWindow::MainWindow()
{
    setWindowTitle(QStringLiteral("Remote Desktop Client"));
    setMinimumSize(740, 530);
    resize(1020, 630);
    setStyleSheet(QString::fromLatin1(Style));

    auto *root = new QWidget(this);
    auto *outer = new QVBoxLayout(root);
    outer->setContentsMargins(20, 18, 20, 16);
    outer->setSpacing(12);
    auto *header = new QHBoxLayout;
    auto *heading = new QVBoxLayout;
    heading->setSpacing(3);
    heading->addWidget(label(QStringLiteral("Remote Desktop"), "title"));
    endpoint_ = label(QStringLiteral("Server URL from config.json"), "endpoint");
    endpoint_->setToolTip(QDir::toNativeSeparators(configPath()));
    heading->addWidget(endpoint_);
    header->addLayout(heading, 1);
    connection_ = label();
    connection_->setObjectName(QStringLiteral("connectionState"));
    connection_->setWordWrap(false);
    connectButton_ = button(QStringLiteral("&Connect"), "connectButton");
    header->addWidget(connection_);
    header->addWidget(connectButton_);
    outer->addLayout(header);

    tabs_ = new QTabWidget;
    tabs_->setObjectName(QStringLiteral("roleTabs"));
    tabs_->addTab(buildHostPage(), QStringLiteral("&Host"));
    tabs_->addTab(buildViewerPage(), QStringLiteral("&Viewer"));
    outer->addWidget(tabs_, 1);
    notice_ = label({}, "notice");
    outer->addWidget(notice_);
    setCentralWidget(root);

    connect(&client_, &SignalingClient::changed, this, &MainWindow::refreshUi);
    connect(connectButton_, &QPushButton::clicked, this, [this] {
        if (client_.connection() == Connection::Disconnected) connectUsingConfig();
        else client_.disconnectFromServer();
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index != 0) stopPreview();
        refreshUi();
    });
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { reloadScreens(); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
        if (screen == selectedScreen() || screen == pinnedScreen_.data()) {
            stopPreview(QStringLiteral("The selected display was disconnected."));
        }
        if (screen == pinnedScreen_.data() && client_.pairing().role == Role::Host) {
            client_.disconnectFromServer(QStringLiteral("The shared display was disconnected. Reconnect to create a new invitation."));
        }
        displayIds_.remove(screen);
        reloadScreens(screen);
    });

    connect(&previewTimer_, &QTimer::timeout, this, &MainWindow::capturePreview);
    connect(&countdownTimer_, &QTimer::timeout, this, &MainWindow::refreshCountdown);
    countdownTimer_.start(250);
    reloadScreens();
#ifdef Q_OS_WIN
    sessionNotifications_ = WTSRegisterSessionNotification(reinterpret_cast<HWND>(winId()), NOTIFY_FOR_THIS_SESSION) != FALSE;
#endif
    QTimer::singleShot(0, this, &MainWindow::connectUsingConfig);
}

MainWindow::~MainWindow()
{
#ifdef Q_OS_WIN
    if (sessionNotifications_) WTSUnRegisterSessionNotification(reinterpret_cast<HWND>(winId()));
#endif
}

QWidget *MainWindow::buildHostPage()
{
    auto *page = new QWidget;
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(0, 8, 0, 0);
    row->setSpacing(12);
    auto *control = card(row, 4);
    control->addWidget(label(QStringLiteral("Share this PC"), "sectionTitle"));

    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    screenBox_ = new QComboBox;
    screenBox_->setObjectName(QStringLiteral("displayBox"));
    ttlBox_ = new QComboBox;
    ttlBox_->setObjectName(QStringLiteral("ttlBox"));
    ttlBox_->addItem(QStringLiteral("5 minutes"), 300);
    ttlBox_->addItem(QStringLiteral("1 minute"), 60);
    ttlBox_->addItem(QStringLiteral("15 seconds"), 15);
    form->addRow(QStringLiteral("&Display"), screenBox_);
    form->addRow(QStringLiteral("Expires &in"), ttlBox_);
    control->addLayout(form);
    createButton_ = button(QStringLiteral("Create i&nvitation"), "createInvitation", true);
    control->addWidget(createButton_);

    invitationPanel_ = new QWidget;
    auto *invitation = new QVBoxLayout(invitationPanel_);
    invitation->setContentsMargins(0, 4, 0, 0);
    auto *codeLabel = label(QStringLiteral("Invitation code"));
    hostCode_ = new QLineEdit;
    hostCode_->setObjectName(QStringLiteral("hostCode"));
    hostCode_->setAccessibleName(QStringLiteral("Invitation code"));
    hostCode_->setReadOnly(true);
    hostCode_->setLayoutDirection(Qt::LeftToRight);
    auto *copyCode = button(QStringLiteral("Cop&y code"), "copyInvitation");
    invitationClock_ = label({}, "muted");
    invitation->addWidget(codeLabel);
    invitation->addWidget(hostCode_);
    invitation->addWidget(copyCode);
    invitation->addWidget(invitationClock_);
    control->addWidget(invitationPanel_);

    consentPanel_ = new QWidget;
    auto *consent = new QVBoxLayout(consentPanel_);
    consent->setContentsMargins(0, 4, 0, 0);
    requester_ = label({}, "requesterName");
    requesterId_ = label({}, "muted");
    requesterId_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    consentClock_ = label({}, "muted");
    auto *response = new QHBoxLayout;
    acceptButton_ = button(QStringLiteral("&Allow view"), "acceptAccess", true);
    declineButton_ = button(QStringLiteral("Dec&line"), "declineAccess");
    response->addWidget(acceptButton_);
    response->addWidget(declineButton_);
    consent->addWidget(requester_);
    consent->addWidget(requesterId_);
    consent->addWidget(consentClock_);
    consent->addLayout(response);
    control->addWidget(consentPanel_);

    hostSessionPanel_ = new QWidget;
    auto *session = new QVBoxLayout(hostSessionPanel_);
    session->setContentsMargins(0, 4, 0, 0);
    hostSession_ = label({}, "hostSession");
    hostSession_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    hostEnd_ = button(QStringLiteral("&End session"), "hostEnd");
    session->addWidget(hostSession_);
    session->addWidget(hostEnd_);
    control->addWidget(hostSessionPanel_);
    control->addStretch();

    auto *screenPreview = card(row, 7);
    auto *previewHeader = new QHBoxLayout;
    previewHeader->addWidget(label(QStringLiteral("Local screen preview"), "sectionTitle"), 1);
    previewButton_ = button(QStringLiteral("Start &preview"), "previewButton");
    previewHeader->addWidget(previewButton_);
    screenPreview->addLayout(previewHeader);
    preview_ = label(QStringLiteral("Preview stopped"), "previewCanvas");
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(200, 160);
    preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    screenPreview->addWidget(preview_, 1);
    screenPreview->addWidget(label(QStringLiteral("Visible on this PC only."), "muted"));

    connect(screenBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        stopPreview();
        refreshUi();
    });
    connect(previewButton_, &QPushButton::clicked, this, [this] {
        if (previewTimer_.isActive()) {
            stopPreview();
        } else if (previewAllowed()) {
            previewButton_->setText(QStringLiteral("Stop &preview"));
            previewTimer_.start(250);
            capturePreview();
        }
    });
    connect(createButton_, &QPushButton::clicked, this, [this] {
        pinnedScreen_ = selectedScreen();
        if (pinnedScreen_) {
            client_.createInvitation(displayIds_.value(pinnedScreen_.data()), ttlBox_->currentData().toInt());
        }
    });
    connect(copyCode, &QPushButton::clicked, this, [this] {
        if (client_.pairing().phase == Phase::InvitationOpen) {
            QGuiApplication::clipboard()->setText(client_.pairing().invitationCode);
            notice_->setText(QStringLiteral("Invitation copied."));
        }
    });
    connect(acceptButton_, &QPushButton::clicked, &client_, &SignalingClient::acceptAccess);
    connect(declineButton_, &QPushButton::clicked, &client_, &SignalingClient::declineAccess);
    connect(hostEnd_, &QPushButton::clicked, &client_, &SignalingClient::endSession);
    return page;
}

QWidget *MainWindow::buildViewerPage()
{
    auto *page = new QWidget;
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(0, 8, 0, 0);
    row->setSpacing(12);
    auto *control = card(row, 4);
    control->addWidget(label(QStringLiteral("Connect to a PC"), "sectionTitle"));
    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);

    viewerName_ = new QLineEdit(QStringLiteral("Viewer PC"));
    viewerName_->setObjectName(QStringLiteral("viewerName"));
    viewerName_->setMaxLength(128);
    viewerCode_ = new QLineEdit;
    viewerCode_->setObjectName(QStringLiteral("viewerCode"));
    viewerCode_->setPlaceholderText(QStringLiteral("Paste the 32-character code"));
    viewerCode_->setLayoutDirection(Qt::LeftToRight);
    viewerCode_->setMaxLength(128);
    form->addRow(QStringLiteral("Your &name"), viewerName_);
    form->addRow(QStringLiteral("Invitation &code"), viewerCode_);
    control->addLayout(form);
    requestButton_ = button(QStringLiteral("&Request access"), "requestAccess", true);
    control->addWidget(requestButton_);
    control->addStretch();

    auto *session = card(row, 7);
    viewerState_ = label({}, "viewerState");
    viewerDetails_ = label({}, "viewerDetails");
    viewerDetails_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    viewerClock_ = label({}, "muted");
    viewerEnd_ = button(QStringLiteral("&End session"), "viewerEnd");
    session->addWidget(viewerState_);
    session->addWidget(viewerDetails_);
    session->addWidget(viewerClock_);
    session->addWidget(viewerEnd_);
    session->addStretch();

    connect(viewerName_, &QLineEdit::textChanged, this, &MainWindow::refreshRequestButton);
    connect(viewerCode_, &QLineEdit::textChanged, this, &MainWindow::refreshRequestButton);
    connect(viewerCode_, &QLineEdit::returnPressed, this, [this] { requestButton_->click(); });
    connect(requestButton_, &QPushButton::clicked, this, [this] {
        client_.requestAccess(viewerCode_->text().trimmed(), viewerName_->text().trimmed());
    });
    connect(viewerEnd_, &QPushButton::clicked, &client_, &SignalingClient::endSession);
    return page;
}

void MainWindow::connectUsingConfig()
{
    if (client_.connection() != Connection::Disconnected) return;
    QFile file(configPath());
    if (!file.open(QIODevice::ReadOnly)) {
        endpoint_->setText(QStringLiteral("Missing config.json"));
        client_.disconnectFromServer(QStringLiteral("Open or copy config.json beside the executable, then click Connect."));
        return;
    }
    const auto json = QJsonDocument::fromJson(file.readAll());
    const auto address = json.object().value(QStringLiteral("server_url")).toString().trimmed();
    const QUrl url(address, QUrl::StrictMode);
    if (!json.isObject() || !url.isValid() || url.host().isEmpty() || url.hasFragment()
        || !url.userInfo().isEmpty() || (url.scheme() != QStringLiteral("ws") && url.scheme() != QStringLiteral("wss"))) {
        endpoint_->setText(QStringLiteral("Check config.json"));
        client_.disconnectFromServer(QStringLiteral("Set server_url to a complete WebSocket URL, such as ws://127.0.0.1:8765/ws."));
        return;
    }
    endpoint_->setText(url.toDisplayString(QUrl::RemoveQuery));
    client_.connectTo(url);
}

void MainWindow::reloadScreens(QScreen *removed)
{
    auto *previous = selectedScreen();
    screens_ = QGuiApplication::screens();
    screens_.removeAll(removed);
    const QSignalBlocker blocker(screenBox_);
    screenBox_->clear();
    int selected = -1;
    for (auto *screen : screens_) {
        if (!displayIds_.contains(screen)) {
            displayIds_.insert(screen, QStringLiteral("display-%1").arg(nextDisplayId_++));
            connect(screen, &QScreen::geometryChanged, this, [this] { reloadScreens(); });
        }
        const QString name = screen == QGuiApplication::primaryScreen()
            ? QStringLiteral("Primary display") : QStringLiteral("Display %1").arg(screenBox_->count() + 1);
        screenBox_->addItem(QStringLiteral("%1 (%2 × %3)").arg(name).arg(screen->size().width()).arg(screen->size().height()),
                            displayIds_.value(screen));
        if (screen == previous || (!previous && screen == QGuiApplication::primaryScreen())) {
            selected = screenBox_->count() - 1;
        }
    }
    if (selected >= 0) screenBox_->setCurrentIndex(selected);
    refreshUi();
}

QScreen *MainWindow::selectedScreen() const
{
    const int index = screenBox_->currentIndex();
    return index >= 0 && index < screens_.size() ? screens_.at(index) : nullptr;
}

void MainWindow::refreshUi()
{
    const auto connection = client_.connection();
    const auto &pairing = client_.pairing();
    const auto phase = pairing.phase;
    const bool online = connection == Connection::Ready;
    const bool idle = phase == Phase::Idle;
    const bool host = pairing.role == Role::Host;
    const bool viewer = pairing.role == Role::Viewer;
    const bool active = phase == Phase::Active;
    const bool ending = phase == Phase::Ending;

    if ((connection == Connection::Disconnected && lastConnection_ != connection)
        || (lastPhase_ == Phase::Active && !active) || !previewAllowed()) stopPreview();
    lastConnection_ = connection;
    lastPhase_ = phase;
    if (idle) pinnedScreen_.clear();
    connection_->setText(online ? QStringLiteral("Connected") : connection == Connection::Connecting
                         ? QStringLiteral("Connecting…") : QStringLiteral("Offline"));
    connection_->setStyleSheet(online ? QStringLiteral("color: #0b615b; font-weight: 700;")
                                       : QStringLiteral("color: #405775;"));
    connectButton_->setText(connection == Connection::Disconnected ? QStringLiteral("&Connect") : QStringLiteral("&Disconnect"));
    notice_->setText(client_.notice());

    tabs_->setTabEnabled(0, !viewer);
    tabs_->setTabEnabled(1, !host);
    screenBox_->setEnabled(idle && !screens_.isEmpty());
    ttlBox_->setEnabled(idle);
    createButton_->setEnabled(online && idle && selectedScreen() != nullptr);
    previewButton_->setEnabled(previewAllowed());
    invitationPanel_->setVisible(phase == Phase::InvitationOpen);
    hostCode_->setText(pairing.invitationCode);

    const bool consent = phase == Phase::HostApproval || phase == Phase::Accepting || phase == Phase::Declining;
    consentPanel_->setVisible(consent);
    requester_->setText(consent ? QStringLiteral("%1 requests view access (unverified name).").arg(pairing.viewerName) : QString());
    requesterId_->setText(consent ? QStringLiteral("Viewer connection %1").arg(pairing.viewerPeerId) : QString());
    acceptButton_->setEnabled(phase == Phase::HostApproval && !pinnedScreen_.isNull());
    declineButton_->setEnabled(phase == Phase::HostApproval);
    hostSessionPanel_->setVisible(host && (active || ending));
    hostSession_->setText(active && host ? QStringLiteral("View-only session with %1\nSession %2").arg(pairing.viewerName, pairing.sessionId)
                                         : QStringLiteral("Session ending…"));
    hostEnd_->setEnabled(active);

    viewerName_->setEnabled(idle);
    viewerCode_->setEnabled(idle);
    if (phase == Phase::WaitingForHost && !viewerCode_->text().isEmpty()) viewerCode_->clear();
    refreshRequestButton();
    viewerEnd_->setVisible(viewer && (active || ending));
    viewerEnd_->setEnabled(active);
    if (viewer && active) {
        viewerState_->setText(QStringLiteral("Session approved"));
        viewerDetails_->setText(QStringLiteral("Display %1\nGranted permissions %2\nSession %3\nScreen streaming is a later milestone.")
                                .arg(pairing.displayId, pairing.permissions.join(QStringLiteral(", ")), pairing.sessionId));
    } else if (phase == Phase::WaitingForHost) {
        viewerState_->setText(QStringLiteral("Waiting for the host"));
        viewerDetails_->setText(QStringLiteral("Host connection %1").arg(pairing.hostPeerId));
    } else if (phase == Phase::RequestingAccess || (viewer && ending)) {
        viewerState_->setText(ending ? QStringLiteral("Session ending…") : QStringLiteral("Sending request…"));
        viewerDetails_->clear();
    } else {
        viewerState_->setText(QStringLiteral("No active session"));
        viewerDetails_->clear();
    }
    refreshCountdown();
}

void MainWindow::refreshRequestButton()
{
    requestButton_->setEnabled(client_.connection() == Connection::Ready && client_.pairing().phase == Phase::Idle
        && SignalingClient::validInvitationCode(viewerCode_->text().trimmed())
        && SignalingClient::validViewerName(viewerName_->text().trimmed()));
}

void MainWindow::refreshCountdown()
{
    const auto &pairing = client_.pairing();
    const auto time = remainingText(pairing.expiresAt);
    const auto text = time.isEmpty() ? QString() : QStringLiteral("Expires in %1").arg(time);
    invitationClock_->setText(text);
    consentClock_->setText(text);
    viewerClock_->setVisible(pairing.role == Role::Viewer
                            && (pairing.phase == Phase::WaitingForHost || pairing.phase == Phase::Active));
    viewerClock_->setText(text);
}

bool MainWindow::previewAllowed() const
{
    const auto &pairing = client_.pairing();
    return tabs_->currentIndex() == 0 && ((pairing.phase == Phase::Idle && selectedScreen())
        || (pairing.phase == Phase::Active && pairing.role == Role::Host && !pinnedScreen_.isNull()));
}

void MainWindow::capturePreview()
{
    if (!previewAllowed()) {
        stopPreview();
        return;
    }
    auto *screen = client_.pairing().phase == Phase::Active ? pinnedScreen_.data() : selectedScreen();
    const auto frame = screen->grabWindow(0);
    if (frame.isNull()) {
        stopPreview(QStringLiteral("This display could not be captured. Try starting the preview again."));
        return;
    }
    const qreal scale = preview_->devicePixelRatioF();
    auto thumbnail = frame.scaled(preview_->contentsRect().size() * scale, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    thumbnail.setDevicePixelRatio(scale);
    preview_->setPixmap(thumbnail);
}

void MainWindow::stopPreview(const QString &message)
{
    if (!previewTimer_.isActive() && message.isEmpty()) return;
    previewTimer_.stop();
    preview_->setText(message.isEmpty() ? QStringLiteral("Preview stopped") : message);
    previewButton_->setText(QStringLiteral("Start &preview"));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    stopPreview();
    client_.disconnectFromServer();
    QMainWindow::closeEvent(event);
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    const auto *event = static_cast<const MSG *>(message);
    const bool sessionUnavailable = event->message == WM_WTSSESSION_CHANGE
        && (event->wParam == WTS_SESSION_LOCK || event->wParam == WTS_SESSION_LOGOFF
            || event->wParam == WTS_CONSOLE_DISCONNECT || event->wParam == WTS_REMOTE_DISCONNECT);
    const bool suspended = event->message == WM_POWERBROADCAST && event->wParam == PBT_APMSUSPEND;
    if (sessionUnavailable || suspended) {
        stopPreview(QStringLiteral("Preview stopped when the desktop became unavailable."));
        client_.disconnectFromServer(QStringLiteral("The desktop became unavailable. Reconnect to start a new session."));
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif
