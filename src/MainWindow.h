#pragma once

#include "SignalingClient.h"

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>

class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScreen;
class QTabWidget;

class MainWindow final : public QMainWindow
{
public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

private:
    QWidget *buildHostPage();
    QWidget *buildViewerPage();
    void connectUsingConfig();
    void reloadScreens(QScreen *removed = nullptr);
    QScreen *selectedScreen() const;
    void refreshUi();
    void refreshRequestButton();
    void refreshCountdown();
    bool previewAllowed() const;
    void capturePreview();
    void stopPreview(const QString &message = {});

    SignalingClient client_;
    SignalingClient::Connection lastConnection_ = SignalingClient::Connection::Disconnected;
    SignalingClient::Phase lastPhase_ = SignalingClient::Phase::Idle;
    QTabWidget *tabs_ = nullptr;
    QLabel *endpoint_ = nullptr;
    QLabel *connection_ = nullptr;
    QLabel *notice_ = nullptr;
    QPushButton *connectButton_ = nullptr;

    QComboBox *screenBox_ = nullptr;
    QComboBox *ttlBox_ = nullptr;
    QPushButton *createButton_ = nullptr;
    QWidget *invitationPanel_ = nullptr;
    QLineEdit *hostCode_ = nullptr;
    QLabel *invitationClock_ = nullptr;
    QWidget *consentPanel_ = nullptr;
    QLabel *requester_ = nullptr;
    QLabel *requesterId_ = nullptr;
    QLabel *consentClock_ = nullptr;
    QPushButton *acceptButton_ = nullptr;
    QPushButton *declineButton_ = nullptr;
    QWidget *hostSessionPanel_ = nullptr;
    QLabel *hostSession_ = nullptr;
    QPushButton *hostEnd_ = nullptr;
    QPushButton *previewButton_ = nullptr;
    QLabel *preview_ = nullptr;

    QLineEdit *viewerName_ = nullptr;
    QLineEdit *viewerCode_ = nullptr;
    QPushButton *requestButton_ = nullptr;
    QLabel *viewerState_ = nullptr;
    QLabel *viewerDetails_ = nullptr;
    QLabel *viewerClock_ = nullptr;
    QPushButton *viewerEnd_ = nullptr;

    QList<QScreen *> screens_;
    QHash<QScreen *, QString> displayIds_;
    QPointer<QScreen> pinnedScreen_;
    int nextDisplayId_ = 1;
    QTimer previewTimer_;
    QTimer countdownTimer_;
#ifdef Q_OS_WIN
    bool sessionNotifications_ = false;
#endif
};
