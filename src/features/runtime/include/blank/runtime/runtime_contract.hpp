#ifndef BLANK_RUNTIME_CONTRACT_HPP
#define BLANK_RUNTIME_CONTRACT_HPP

#include <blank/overrides/override_manager.hpp>
#include <blank/profiles/profile_runtime.hpp>
#include <blank/runtime_config/runtime_config.hpp>
#include <QMetaType>
#include <QString>
#include <QtTypes>
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

struct ActivationInput {
    profile_payload::minimal_profile_snapshot material;
    runtime_config::client_settings settings{};
    std::vector<overrides::override_item> patches;
    CompositionIdentity identity;
    std::vector<profile_payload::assembly_diagnostic> assemblyDiagnostics;
};

// Each source is coherent independently; this does not claim a cross-store transaction.
[[nodiscard]] ActivationInput
makeActivationInput(const profile_runtime::activation_snapshot& profile,
                    const runtime_config::settings_snapshot& settings,
                    const overrides::selection_snapshot& overrides);

enum class TransactionKind : quint8 {
    Activation,
    Mode,
    Selection,
    Tun,
};

enum class TransactionOutcome : quint8 {
    Rejected,         // No candidate write was accepted.
    Applied,          // Requested state confirmed by readback.
    RolledBack,       // Previous state reapplied and confirmed.
    Cancelled,        // Cancelled before any write was submitted.
    Uncertain,        // A submitted write or rollback could not be confirmed.
    PartiallyApplied, // Configuration confirmed; the following TUN operation failed.
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

// Local diagnostics can contain script logs and kernel output with credentials.
// Retained for explicit inspection; never forwarded to logMessage automatically.
struct ActivationDiagnostics {
    runtime_config::configuration_error configurationError{};
    std::vector<profile_payload::assembly_diagnostic> assembly;
    std::vector<runtime_config::configuration_diagnostic> configuration;
    std::vector<overrides::patch_report> patches;
    QString serviceAdmission;
    NativeCheckPolicy nativeCheckPolicy = NativeCheckPolicy::RuntimeCandidate;
    std::optional<runtime_config::kernel_check_result> nativeCheck;
};

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

struct ActivationSubmission {
    qint64 requestId = 0; // Nonzero only when accepted.
    ActivationRejection rejection = ActivationRejection::None;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return requestId != 0;
    }
};

Q_DECLARE_METATYPE(TransactionResult)

#endif
