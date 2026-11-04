#ifndef BLANK_QUICK_CORE_LOG_SOCKET_HPP
#define BLANK_QUICK_CORE_LOG_SOCKET_HPP

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QWebSocket>

class CoreLogSocket final : public QObject
{
    Q_OBJECT
  public:
    explicit CoreLogSocket(QObject* parent = nullptr);
    ~CoreLogSocket() override;
    CoreLogSocket(const CoreLogSocket&) = delete;
    CoreLogSocket& operator=(const CoreLogSocket&) = delete;
    CoreLogSocket(CoreLogSocket&&) = delete;
    CoreLogSocket& operator=(CoreLogSocket&&) = delete;
    void start(const QNetworkRequest& request);
    void stop();
    [[nodiscard]] bool connected() const noexcept;
  Q_SIGNALS:
    void message(const QString& text);
    void stateChanged(bool connected, const QString& status);

  private:
    void open();
    void discard();
    void retry(const QString& status);
    void checkHealth();
    QNetworkRequest request_;
    QNetworkAccessManager network_;
    QPointer<QWebSocket> socket_;
    QPointer<QNetworkReply> health_;
    QTimer retry_;
    QTimer watchdog_;
    quint64 generation_ = 0;
    int retryMs_ = 1000;
    bool running_ = false;
    bool connected_ = false;
};

#endif
