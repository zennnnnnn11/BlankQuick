#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QUrl>
#include <algorithm>
#include <array>
#include <blank/config/service_config.hpp>
#include <blank/runtime/config_switcher.hpp>
#include <chrono>
#include <string_view>
#include <utility>

#include "rule_readback.hpp"

namespace {
constexpr qint64 responseLimit = qint64{8} * 1024 * 1024;

[[nodiscard]] QString proxyType(QString type) {
    type = type.toLower().remove(u'-');
    if (type == QStringLiteral("ss")) {
        return QStringLiteral("Shadowsocks");
    }
    if (type == QStringLiteral("ssr")) {
        return QStringLiteral("ShadowsocksR");
    }
    if (type == QStringLiteral("select")) {
        return QStringLiteral("Selector");
    }
    constexpr auto types = std::to_array<std::string_view>({
        "Direct",      "Reject",    "RejectDrop", "Compatible", "Pass",        "PassRule",
        "Rematch",     "Dns",       "Relay",      "Selector",   "Fallback",    "URLTest",
        "LoadBalance", "Snell",     "Socks5",     "Http",       "Vmess",       "Vless",
        "Trojan",      "Hysteria",  "Hysteria2",  "WireGuard",  "Tuic",        "Ssh",
        "Mieru",       "AnyTLS",    "Sudoku",     "Masque",     "TrustTunnel", "ShadowQuic",
        "OpenVPN",     "Tailscale", "ZeroTier",   "EasyTier",   "GostRelay",
    });
    for (const auto name : types) {
        const auto runtime = QString::fromUtf8(name);
        if (runtime.toLower() == type) {
            return runtime;
        }
    }
    return {};
}
} // namespace

ConfigSwitcher::ConfigSwitcher(KernelController& kernel, const SwitchTimeouts& timeouts,
                               QObject* parent)
    : QObject(parent), kernel_(kernel), timeouts_(timeouts) {
    network_.setProxy(QNetworkProxy::NoProxy);
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        ++generation_;
        if (reply_) {
            reply_->abort();
            reply_.clear();
        }
        if (step_ == Step::Apply && submitted_) {
            diagnostics_.push_back({
                .stage = phase(),
                .error = TransactionError::Timeout,
                .rollback = rollingBack_,
            });
            complete(TransactionOutcome::Uncertain, tr("配置提交超时，结果未确认"));
            return;
        }
        fail(tr("配置事务超时"), TransactionError::Timeout);
    });
    connect(&kernel_, &KernelController::stateChanged, this, [this] {
        if (kernel_.state() != KernelController::State::Ready) {
            cancel();
        }
    });
}

bool ConfigSwitcher::busy() const noexcept {
    return step_ != Step::Idle;
}

TransactionStage ConfigSwitcher::phase() const noexcept {
    switch (step_) {
    case Step::Idle:
        return TransactionStage::Idle;
    case Step::Baseline:
        return TransactionStage::Baseline;
    case Step::Apply:
        return TransactionStage::Apply;
    case Step::General:
        return TransactionStage::General;
    case Step::Proxies:
        return TransactionStage::Proxies;
    case Step::Rules:
        return TransactionStage::Rules;
    case Step::ProxyProviders:
        return TransactionStage::ProxyProviders;
    case Step::RuleProviders:
        return TransactionStage::RuleProviders;
    case Step::Connections:
        return TransactionStage::Connections;
    case Step::Dns:
        return TransactionStage::Dns;
    case Step::Selection:
        return TransactionStage::Selection;
    case Step::FinalProxies:
        return TransactionStage::FinalProxies;
    }
    return TransactionStage::Idle;
}

bool ConfigSwitcher::rollingBack() const noexcept {
    return rollingBack_;
}

void ConfigSwitcher::start(const QJsonObject& candidate, const QString& previous,
                           const QJsonObject& selected, const QJsonObject& previousSelected,
                           bool closeConnections) {
    if (busy()) {
        return;
    }
    // 每次切换先读取运行基线，而不是假定上次提交的文本仍等于现场。
    // 模式和节点选择可能经过独立操作改变，回滚必须基于本次切换前的事实。
    ++generation_;
    candidate_ = candidate;
    closeConnections_ = closeConnections;
    previous_ = previous;
    if (previous_.isEmpty()) {
        previous_ = kernel_.initialConfiguration();
    }
    selected_ = selected;
    previousSelected_ = previousSelected;
    rollingBack_ = false;
    submitted_ = false;
    failure_.clear();
    warning_.clear();
    proxies_.clear();
    general_ = {};
    expectedSelections_ = {};
    diagnostics_.clear();
    step_ = Step::Baseline;
    deadline_.start(timeouts_.verification);
    request("/configs");
}

void ConfigSwitcher::cancel() {
    if (!busy()) {
        return;
    }
    ++generation_;
    if (reply_) {
        reply_->abort();
        reply_.clear();
    }
    diagnostics_.push_back(
        {.stage = phase(), .error = TransactionError::Cancelled, .rollback = rollingBack_});
    complete(submitted_ ? TransactionOutcome::Uncertain : TransactionOutcome::Cancelled,
             tr("内核会话结束，配置切换已取消"));
}

void ConfigSwitcher::request(const QByteArray& path, const QByteArray& method,
                             const QByteArray& body) {
    auto* reply = kernel_.sendRequest(network_, path, body, method);
    reply_ = reply;
    reply->setReadBufferSize(responseLimit + 1);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > responseLimit) {
            reply->abort();
        }
    });
    const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation] { received(reply, generation); });
    Q_EMIT progressChanged();
}

void ConfigSwitcher::apply() {
    // submitted_ 必须在发送之前置位。发送后失去连接时，我们无法证明服务
    // 没有执行；这一事实决定取消和超时应报告 Uncertain，而不是安全拒绝。
    step_ = Step::Apply;
    deadline_.start(rollingBack_ ? timeouts_.rollback : timeouts_.forward);

    submitted_ = true;
    const auto payload =
        rollingBack_ ? previous_
                     : QString::fromUtf8(QJsonDocument(candidate_).toJson(QJsonDocument::Compact));
    request("/configs?force=true", "PUT",
            QJsonDocument(QJsonObject{{QStringLiteral("payload"), payload}})
                .toJson(QJsonDocument::Compact));
}

bool ConfigSwitcher::verifyGeneral(const QJsonObject& object) const {
    // 配置切换不负责开启/关闭 TUN，所以 TUN 应保持切换前基线；端口和模式
    // 则应匹配候选配置。回滚时，这两项也改为对照运行基线。
    const auto expected = rollingBack_ ? baseline_ : candidate_;
    const auto port = object.value(QStringLiteral("mixed-port")).toInteger(-1);
    if (object.value(QStringLiteral("mode")) != expected.value(QStringLiteral("mode")) ||
        port != expected.value(QStringLiteral("mixed-port")).toInteger(-1)) {
        return false;
    }
    const auto tun = baseline_.value(QStringLiteral("tun")).toObject();
    const auto actual = object.value(QStringLiteral("tun")).toObject();
    return actual.value(QStringLiteral("enable")).toBool() ==
               tun.value(QStringLiteral("enable")).toBool() &&
           (!tun.value(QStringLiteral("enable")).toBool() ||
            actual.value(QStringLiteral("device")) == tun.value(QStringLiteral("device")));
}

bool ConfigSwitcher::verifyProxies(const QJsonObject& object) const {
    if (object.isEmpty() || object.size() > 100000) {
        return false;
    }
    for (auto entry = object.begin(); entry != object.end(); ++entry) {
        const auto value = entry.value().toObject();
        if (!entry.value().isObject() || entry.key().isEmpty() || entry.key().size() > 1024 ||
            value.value(QStringLiteral("type")).toString().isEmpty()) {
            return false;
        }
    }
    const auto expected = rollingBack_ ? previousConfig_ : candidate_;
    const auto exists = [&object](const QJsonValue& entry) {
        const auto definition = entry.toObject();
        const auto actual = object.value(definition.value(QStringLiteral("name")).toString());

        const auto type = proxyType(definition.value(QStringLiteral("type")).toString());
        return !type.isEmpty() && actual.isObject() &&
               actual.toObject().value(QStringLiteral("type")) == type;
    };
    return std::ranges::all_of(expected.value(QStringLiteral("proxies")).toArray(), exists) &&
           std::ranges::all_of(expected.value(QStringLiteral("proxy-groups")).toArray(), exists);
}

bool ConfigSwitcher::verifyProviders(const QJsonObject& object, bool rules) const {
    const auto value = object.value(QStringLiteral("providers"));
    if (!value.isObject()) {
        return false;
    }
    const auto providers = value.toObject();
    const auto configuration = rollingBack_ ? previousConfig_ : candidate_;
    const auto expected =
        configuration
            .value(rules ? QStringLiteral("rule-providers") : QStringLiteral("proxy-providers"))
            .toObject();
    for (auto entry = expected.begin(); entry != expected.end(); ++entry) {
        const auto provider = providers.value(entry.key()).toObject();
        const auto definition = entry.value().toObject();
        if (!entry.value().isObject() || provider.value(QStringLiteral("name")) != entry.key() ||
            provider.value(QStringLiteral("type")) !=
                (rules ? QStringLiteral("Rule") : QStringLiteral("Proxy")) ||
            provider.value(QStringLiteral("vehicleType")).toString().toLower() !=
                definition.value(QStringLiteral("type")).toString() ||
            (rules ? provider.value(QStringLiteral("ruleCount")).toInteger(-1) < 0 ||
                         provider.value(QStringLiteral("behavior")).toString().toLower() !=
                             definition.value(QStringLiteral("behavior")).toString()
                   : !provider.value(QStringLiteral("proxies")).isArray())) {
            return false;
        }
    }
    return true;
}

void ConfigSwitcher::received(QNetworkReply* reply, quint64 generation) {
    reply->deleteLater();
    if (generation != generation_ || reply_ != reply || !busy()) {
        return;
    }
    reply_.clear();
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto bytes = reply->readAll();
    const bool read = step_ == Step::Baseline || step_ == Step::General || step_ == Step::Proxies ||
                      step_ == Step::Rules || step_ == Step::ProxyProviders ||
                      step_ == Step::RuleProviders || step_ == Step::FinalProxies;
    if (reply->error() != QNetworkReply::NoError || status != (read ? 200 : 204) ||
        bytes.size() > responseLimit) {
        const bool rejected = step_ == Step::Apply && !rollingBack_ &&
                              (status == 400 || status == 403 || status == 409 || status == 413);
        if (rejected) {

            submitted_ = false;
        }
        diagnostics_.push_back({
            .stage = phase(),
            .error = rejected ? TransactionError::Rejected : TransactionError::Transport,
            .httpStatus = status,
            .networkError = static_cast<int>(reply->error()),
            .rollback = rollingBack_,
        });
        if (step_ == Step::Apply && (status == 0 || status == 503 || status == 504)) {
            // 连接失败时，内核或服务仍可能在执行原请求。不能并发提交
            // 回滚，否则两份配置的最终执行顺序将无法确认。
            complete(
                TransactionOutcome::Uncertain,
                tr("配置提交结果未确认（HTTP %1）；请停止并重建会话，未发起并行回滚").arg(status));
            return;
        }
        fail(tr("配置事务请求失败（HTTP %1）").arg(status), TransactionError::Transport);
        return;
    }
    const auto object = QJsonDocument::fromJson(bytes).object();
    // 一条回包只推进一个步骤。提交 -> 一般配置 -> 节点 -> 规则 -> Provider ->
    // 连接/DNS 清理 -> 恢复选择 -> 最终节点回读；全部完成才有资格发布 Applied。
    // 正向和回滚共用验证链，避免“已发回滚请求”被误报为“已恢复”。
    switch (step_) {
    case Step::Baseline: {
        baseline_ = object;
        const auto port = object.value(QStringLiteral("mixed-port")).toInteger(-1);
        const auto mode = object.value(QStringLiteral("mode")).toString();
        if (port < 1 || port > 65535 ||
            (mode != QStringLiteral("rule") && mode != QStringLiteral("global") &&
             mode != QStringLiteral("direct"))) {
            fail(tr("切换前内核配置状态无效"));
            return;
        }
        const auto candidateRules = blank::runtime_detail::expectedRules(candidate_);
        if (!candidateRules) {
            fail(tr("候选规则包含当前内核 API 无法核验的规则形式，尚未提交配置"));
            return;
        }
        expectedRules_ = *candidateRules;
        if (previous_.isEmpty()) {
            auto base = compileServiceConfig(QStringLiteral("{}"), static_cast<quint16>(port), {});
            base.value.insert(QStringLiteral("mode"), mode);
            previous_ = QString::fromUtf8(QJsonDocument(base.value).toJson(QJsonDocument::Compact));
        }
        const auto parsed = parseProfileConfig(previous_);
        if (!parsed.error.isEmpty()) {
            fail(tr("无法保存上一份配置以供回滚：%1").arg(parsed.error));
            return;
        }
        previousConfig_ = parsed.value;
        const auto rules = blank::runtime_detail::expectedRules(previousConfig_);
        if (!rules) {
            fail(tr("上一份规则无法通过当前内核 API 核验，无法保证回滚；尚未提交配置"));
            return;
        }
        previousRules_ = *rules;
        previousConfig_.insert(QStringLiteral("mode"), mode);
        previous_ =
            QString::fromUtf8(QJsonDocument(previousConfig_).toJson(QJsonDocument::Compact));
        apply();
        return;
    }
    case Step::Apply:

        deadline_.start(timeouts_.verification);
        step_ = Step::General;
        request("/configs");
        return;
    case Step::General:
        if (!verifyGeneral(object)) {
            fail(tr("内核配置回读不一致（模式、端口或 TUN 参数）"));
            return;
        }
        general_ = object;
        step_ = Step::Proxies;
        request("/proxies");
        return;
    case Step::Proxies:
        resources_ = object.value(QStringLiteral("proxies")).toObject();
        if (!verifyProxies(resources_)) {
            fail(tr("内核策略组或节点资源未就绪"));
            return;
        }
        proxies_ = bytes;
        step_ = Step::Rules;
        request("/rules");
        return;
    case Step::Rules:
        if (!blank::runtime_detail::verifyRules(rollingBack_ ? previousRules_ : expectedRules_,
                                                object)) {
            fail(tr("内核规则回读不一致（数量、顺序、类型、内容、目标或禁用状态）"));
            return;
        }
        step_ = Step::ProxyProviders;
        request("/providers/proxies");
        return;
    case Step::ProxyProviders:
    case Step::RuleProviders: {
        if (!verifyProviders(object, step_ == Step::RuleProviders)) {
            fail(tr("内核 Provider 回读不一致或响应无效"));
            return;
        }
        const auto providers = object.value(QStringLiteral("providers")).toObject();
        for (auto entry = providers.begin(); entry != providers.end(); ++entry) {
            const auto provider = entry.value().toObject();
            if (provider.value(QStringLiteral("vehicleType")) == QStringLiteral("HTTP") &&
                (step_ == Step::ProxyProviders
                     ? provider.value(QStringLiteral("proxies")).toArray().isEmpty()
                     : provider.value(QStringLiteral("ruleCount")).toInteger() == 0)) {
                warning_ = tr("；部分远程 Provider 当前为空，请检查资源页和日志");
            }
        }
        if (step_ == Step::ProxyProviders) {
            step_ = Step::RuleProviders;
            request("/providers/rules");
            return;
        }
        if (rollingBack_) {
            remaining_ = previousSelected_.keys();
            restoreNext();
            return;
        }
        step_ = closeConnections_ ? Step::Connections : Step::Dns;
        request(closeConnections_ ? QByteArray("/connections") : QByteArray("/cache/dns/flush"),
                closeConnections_ ? QByteArray("DELETE") : QByteArray("POST"));
        return;
    }
    case Step::Connections:
        step_ = Step::Dns;
        request("/cache/dns/flush", "POST");
        return;
    case Step::Dns:
        remaining_ = selected_.keys();
        restoreNext();
        return;
    case Step::Selection:
        restoreNext();
        return;
    case Step::FinalProxies:
        if (!verifyProxies(object.value(QStringLiteral("proxies")).toObject())) {
            fail(tr("恢复节点选择后资源核验失败"));
            return;
        }
        for (auto entry = expectedSelections_.constBegin(); entry != expectedSelections_.constEnd();
             ++entry) {
            if (object.value(QStringLiteral("proxies"))
                    .toObject()
                    .value(entry.key())
                    .toObject()
                    .value(QStringLiteral("now")) != entry.value()) {
                fail(tr("恢复节点选择后的回读不一致"));
                return;
            }
        }
        proxies_ = bytes;
        {
            const auto successMessage =
                closeConnections_
                    ? tr("配置已应用并回读，已请求清理旧连接和刷新 DNS 缓存")
                    : tr("热重载已应用并回读，未主动断开连接；内核重载仍可能重建 Provider 连接");
            complete(rollingBack_ ? TransactionOutcome::RolledBack : TransactionOutcome::Applied,
                     rollingBack_ ? failure_ + tr("；已重新应用上一份配置")
                                  : successMessage + warning_);
        }
        return;
    case Step::Idle:
        return;
    }
}

void ConfigSwitcher::restoreNext() {
    // 旧选择可能已被新订阅删除。只恢复仍存在、仍可选择且包含目标节点的组；
    // 被跳过的选择不算故障。实际发出的选择写入必须在最终回读中逐项核实。

    const auto selections = rollingBack_ ? previousSelected_ : selected_;
    while (!remaining_.isEmpty()) {
        const auto group = remaining_.takeFirst();
        const auto name = selections.value(group).toString();
        const auto resource = resources_.value(group).toObject();
        const auto type = resource.value(QStringLiteral("type")).toString();
        if ((type != QStringLiteral("Selector") && type != QStringLiteral("URLTest") &&
             type != QStringLiteral("Fallback")) ||
            !resource.value(QStringLiteral("all")).toArray().contains(name)) {
            continue;
        }
        step_ = Step::Selection;
        expectedSelections_.insert(group, name);
        request("/proxies/" + QUrl::toPercentEncoding(group), "PUT",
                QJsonDocument(QJsonObject{{QStringLiteral("name"), name}})
                    .toJson(QJsonDocument::Compact));
        return;
    }
    step_ = Step::FinalProxies;
    request("/proxies");
}

void ConfigSwitcher::fail(const QString& error, TransactionError code) {
    // 最多回滚一次。回滚本身失败后继续重试会掩盖真实状态，也可能不断覆盖
    // 现场；此时收敛到 Uncertain，由上层决定停止并重建会话。
    if (diagnostics_.empty() || diagnostics_.back().stage != phase() ||
        diagnostics_.back().rollback != rollingBack_) {
        diagnostics_.push_back({.stage = phase(), .error = code, .rollback = rollingBack_});
    }
    if (rollingBack_) {
        complete(TransactionOutcome::Uncertain,
                 failure_ + tr("；回滚未能确认，内核状态不确定：%1").arg(error));
        return;
    }
    if (submitted_ && kernel_.state() == KernelController::State::Ready && !previous_.isEmpty()) {
        rollingBack_ = true;
        expectedSelections_ = {};
        failure_ = error;
        deadline_.start(timeouts_.rollback);
        apply();
        return;
    }
    complete(TransactionOutcome::Rejected, error);
}

void ConfigSwitcher::complete(TransactionOutcome outcome, const QString& message) {
    if (!busy()) {
        return;
    }
    ++generation_;
    deadline_.stop();
    step_ = Step::Idle;
    Q_EMIT finished(SwitchResult{
        .outcome = outcome,
        .diagnostics = std::move(diagnostics_),
        .message = message,
        .proxies = proxies_,
        .general = general_,
    });
}
