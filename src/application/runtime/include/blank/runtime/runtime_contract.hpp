#ifndef BLANK_RUNTIME_CONTRACT_HPP
#define BLANK_RUNTIME_CONTRACT_HPP

#include <QtTypes>

#include <QMetaType>
#include <QString>
#include <blank/overrides/override_manager.hpp>
#include <blank/profiles/profile_runtime.hpp>
#include <blank/runtime_config/kernel_check.hpp>
#include <blank/runtime_config/runtime_config.hpp>
#include <blank/runtime_config/settings_storage.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class CompositionOrigin : quint8 {
    AdHoc,
    StoredSnapshots,
};

struct OverrideIdentity {
    std::string id;
    std::string revision;
};

// 一份配置由多个独立存储的版本共同决定。只记 profile 的 ID 不足以重现结果：
// 设置、覆写内容、绑定顺序或“保留原规则”选项变化，都可能改变最终配置。
// 此处保存来源身份，不保存可变的管理器，避免异步准备期间读到新旧混合的数据。
struct CompositionIdentity {
    CompositionOrigin origin = CompositionOrigin::AdHoc;
    profile_runtime::profile_identity profile;
    bool preserveOriginalRules = false;
    std::string settingsRevision;
    std::string bindingRevision;
    std::vector<OverrideIdentity> overrides;
};

struct ActivationIdentity {
    qint64 requestId = 0;
    CompositionIdentity composition;
};

// 激活任务的完整输入快照。material 共享只读节点池，settings 和 patches 按值持有；
// 任务提交后，用户继续编辑存储不会悄悄改变这一次激活的输入。
// identity 用于说明“这次应用的是哪一版”，不是重新读取存储的指令。
struct ActivationInput {
    profile_payload::minimal_profile_snapshot material;
    runtime_config::client_settings settings{};
    std::vector<overrides::override_item> patches;
    CompositionIdentity identity;
    std::vector<profile_payload::assembly_diagnostic> assemblyDiagnostics;
};

[[nodiscard]] [[maybe_unused]] ActivationInput
makeActivationInput(const profile_runtime::activation_snapshot& profile,
                    const runtime_config::settings_snapshot& settings,
                    const overrides::selection_snapshot& overrides);

enum class TransactionKind : quint8 {
    Activation,
    Mode,
    Selection,
    Tun,
};

// 结果表达我们已经确认的事实，而非最后一条 HTTP 请求的成败。
// RolledBack 表示旧配置也完成了回读；Uncertain 表示无法断言当前生效状态；
// PartiallyApplied 用于配置已确认应用、后续 TUN 步骤却未能确认成功的情况。
enum class TransactionOutcome : quint8 {
    Rejected,
    Applied,
    RolledBack,
    Cancelled,
    Uncertain,
    PartiallyApplied,
};

enum class TransactionStage : quint8 {
    Idle,
    Preparation,
    Generation,
    ServiceAdmission,
    NativeCheck,
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
    Tun,
};

enum class TransactionError : quint8 {
    None,
    Generation,
    ServiceAdmission,
    NativeCheck,
    Cancelled,
    Rejected,
    Transport,
    Readback,
    Timeout,
    Tun,
    Unexpected,
};

struct SwitchDiagnostic {
    TransactionStage stage = TransactionStage::Idle;
    TransactionError error = TransactionError::None;
    int httpStatus = 0;
    int networkError = 0;
    bool rollback = false;
};

enum class NativeCheckPolicy : quint8 {
    RuntimeCandidate,
    EphemeralFakeIp,
};

struct ActivationDiagnostics {
    runtime_config::configuration_error configurationError{};
    std::vector<profile_payload::assembly_diagnostic> assembly;
    std::vector<runtime_config::configuration_diagnostic> configuration;
    std::vector<overrides::patch_report> patches;
    QString serviceAdmission;
    NativeCheckPolicy nativeCheckPolicy = NativeCheckPolicy::RuntimeCandidate;
    std::optional<runtime_config::kernel_check_result> nativeCheck;
};

// 事务结束时发布的不可变记录。requestId 区分同一会话中的操作，kernelEpoch
// 区分内核会话；diagnostics 保留准备和回读链的信息，界面不必重新查询现场。
struct TransactionResult {
    qint64 requestId = 0;
    quint64 kernelEpoch = 0;
    TransactionKind kind = TransactionKind::Activation;
    TransactionOutcome outcome = TransactionOutcome::Rejected;
    TransactionStage stage = TransactionStage::Idle;
    TransactionError error = TransactionError::None;
    std::shared_ptr<const ActivationIdentity> identity;
    std::shared_ptr<const ActivationDiagnostics> diagnostics;
    std::vector<SwitchDiagnostic> switches;
    QString message;
};

enum class ActivationRejection : quint8 {
    None,
    ShuttingDown,
    NotReady,
    Uncertain,
    Busy,
    ProxyLease,
    TunActive,
};

// 接纳结果与执行结果分开：accepted() 只说明请求取得了身份并进入处理流程。
// 最终成功、回滚或不确定结果通过 transactionFinished 发布。
struct ActivationSubmission {
    qint64 requestId = 0;
    ActivationRejection rejection = ActivationRejection::None;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return requestId != 0;
    }
};

Q_DECLARE_METATYPE(TransactionResult)

#endif
