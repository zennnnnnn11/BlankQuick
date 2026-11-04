#ifndef BLANK_QUICK_SERVICE_PIPE_HPP
#define BLANK_QUICK_SERVICE_PIPE_HPP

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <QWinEventNotifier>
#include <blank/ipc/service_security.hpp>
#include <blank/protocol/json_frame.hpp>

class SecurePipe final : public QObject
{
    Q_OBJECT
  public:
    explicit SecurePipe(QObject* parent = nullptr);
    ~SecurePipe() override;
    SecurePipe(const SecurePipe&) = delete;
    SecurePipe& operator=(const SecurePipe&) = delete;
    SecurePipe(SecurePipe&&) = delete;
    SecurePipe& operator=(SecurePipe&&) = delete;
    void listen();
    void resume();
    void send(const QJsonObject& frame);
    void sendEvent(const QJsonObject& frame);
    void disconnectPeer();
    [[nodiscard]] const PipeIdentity& identity() const noexcept;
  Q_SIGNALS:
    void message(const QJsonObject& frame);
    void closed();
    void failed(const QString& error);

  private:
    void accepted();
    void read();
    void refreshSecurity();
    blank::windows::Handle anchor_;
    blank::windows::Handle event_;
    OVERLAPPED connection_{};
    std::unique_ptr<QWinEventNotifier> acceptEvent_;
    std::unique_ptr<QWinEventNotifier> ownerEvent_;
    QLocalSocket peer_;
    PipeIdentity identity_;
    QStringList allowed_;
    blank::protocol::JsonFrames input_;
    QTimer authentication_;
    QTimer sessions_;
    bool waiting_ = false;
};

#endif
