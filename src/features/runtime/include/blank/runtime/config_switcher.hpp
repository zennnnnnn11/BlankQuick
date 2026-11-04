#ifndef BLANK_QUICK_CONFIG_SWITCHER_HPP
#define BLANK_QUICK_CONFIG_SWITCHER_HPP

#include <blank/runtime/runtime_contract.hpp>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <blank/runtime/kernel_controller.hpp>
#include <blank/shared/timeouts.hpp>
#include <chrono>

struct SwitchTimeouts {
    std::chrono::milliseconds forward{timeout_budget::config_submission};
    std::chrono::milliseconds rollback{timeout_budget::config_submission};
    std::chrono::milliseconds verification{timeout_budget::verification};
};

struct SwitchResult {
    TransactionOutcome outcome = TransactionOutcome::Rejected;
    std::vector<SwitchDiagnostic> diagnostics;
    QString message;
    QByteArray proxies;
    QJsonObject general;
};

class ConfigSwitcher final : public QObject {
    Q_OBJECT
  public:
    explicit ConfigSwitcher(KernelController& kernel, SwitchTimeouts timeouts = SwitchTimeouts(),
                            QObject* parent = nullptr);
    void start(const QJsonObject& candidate, const QString& previous, const QJsonObject& selected,
               const QJsonObject& previousSelected, bool closeConnections = true);
    void cancel();
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] TransactionStage phase() const noexcept;
    [[nodiscard]] bool rollingBack() const noexcept;
  Q_SIGNALS:
    void finished(const SwitchResult& result);
    void progressChanged();

  private:
    enum class Step : quint8 {
        Idle,
        Baseline,
        Apply,
        General,
        Proxies,
        Rules,
        ProxyProviders,
        RuleProviders,
        Connections,
        Dns,
        Selection,
        FinalProxies,
    };
    void request(const QByteArray& path, const QByteArray& method = "GET",
                 const QByteArray& body = {});
    void received(QNetworkReply* reply, quint64 generation);
    void apply();
    void fail(const QString& error, TransactionError code = TransactionError::Readback);
    void restoreNext();
    void complete(TransactionOutcome outcome, const QString& message);
    [[nodiscard]] bool verifyGeneral(const QJsonObject& object) const;
    [[nodiscard]] bool verifyProxies(const QJsonObject& object) const;
    [[nodiscard]] static bool verifyRules(const QJsonObject& object);

    KernelController& kernel_;
    SwitchTimeouts timeouts_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QTimer deadline_;
    Step step_ = Step::Idle;
    quint64 generation_ = 0;
    bool rollingBack_ = false;
    bool submitted_ = false;
    bool closeConnections_ = true;
    QString previous_;
    QString failure_;
    QString warning_;
    QJsonObject candidate_;
    QJsonObject previousConfig_;
    QJsonObject baseline_;
    QJsonObject general_;
    QJsonObject selected_;
    QJsonObject previousSelected_;
    QJsonObject resources_;
    QJsonObject expectedSelections_;
    QStringList remaining_;
    QByteArray proxies_;
    std::vector<SwitchDiagnostic> diagnostics_;
};

#endif
