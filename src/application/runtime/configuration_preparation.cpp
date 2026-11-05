#include <QJsonDocument>
#include <blank/config/service_config.hpp>
#include <blank/runtime/configuration_preparation.hpp>
#include <blank/runtime_config/configuration_document.hpp>
#include <blank/scripting/override_script.hpp>
#include <exception>
#include <iterator>
#include <new>
#include <utility>

namespace {
struct NativeCheckInput {
    QByteArray bytes;
    NativeCheckPolicy policy = NativeCheckPolicy::RuntimeCandidate;
};

NativeCheckInput nativeCheckInput(const QJsonObject& candidate, const QByteArray& bytes) {
    // -t 也可能触碰 fake-IP 持久化数据库。正在运行的内核拥有这份数据库，
    // 预检只关闭临时副本的持久化；真正提交的 bytes 始终保留用户设置。
    const auto dns = candidate.value(QStringLiteral("dns")).toObject();
    auto profile = candidate.value(QStringLiteral("profile")).toObject();
    const auto persistence = profile.value(QStringLiteral("store-fake-ip"));
    if (dns.value(QStringLiteral("enhanced-mode")) != QStringLiteral("fake-ip") ||
        !persistence.isBool() || !persistence.toBool()) {
        return {.bytes = bytes};
    }

    profile.insert(QStringLiteral("store-fake-ip"), false);
    auto transient = candidate;
    transient.insert(QStringLiteral("profile"), profile);
    return {
        .bytes = QJsonDocument(transient).toJson(QJsonDocument::Compact),
        .policy = NativeCheckPolicy::EphemeralFakeIp,
    };
}

// The coordinator chooses the executor. Both executors return the same data
// result, so each next patch sees precisely the previous patch's output.
overrides::dom::document_result applyPatches(json_detail::document&& candidate,
                                             std::span<const overrides::override_item> patches,
                                             const overrides::engine_options& options) {
    auto result = overrides::dom::apply_document(std::move(candidate), {}, options);
    if (result.error != overrides::override_error::none) {
        return result;
    }
    if (std::ssize(patches) > options.max_patches) {
        result.error = overrides::override_error::resource_limit;
        result.stage = overrides::dom::failure_stage::input;
        result.value = {};
        return result;
    }
    for (const auto& patch : patches) {
        auto next = patch.ext == "js" ? overrides::script::evaluate(result.value, patch, options)
                                      : overrides::dom::apply_document(
                                            std::move(result.value), std::span{&patch, 1}, options);
        result.reports.insert(result.reports.end(), std::make_move_iterator(next.reports.begin()),
                              std::make_move_iterator(next.reports.end()));
        result.value = std::move(next.value);
        result.error = next.error;
        result.stage = next.stage;
        if (result.error != overrides::override_error::none) {
            return result;
        }
    }
    return result;
}
} // namespace

runtime_config::configuration_result
composeActivation(const ActivationInput& input, const runtime_config::runtime_context& context,
                  const runtime_config::generation_limits& limits) noexcept try {
    // 草稿允许暂时不完整，后面的覆写可能修复它。只有整条链完成后才做最终
    // 校验，避免逐个覆写都被要求独立生成一份可运行配置。
    auto draft = runtime_config::make_draft(input.material, input.settings, context, limits);
    if (draft.status.error != runtime_config::configuration_error::none) {
        return std::move(draft.status);
    }
    return runtime_config::finalize_configuration(
        applyPatches(std::move(draft.value), input.patches, limits.patches), input.settings,
        context, limits);
} catch (const std::bad_alloc&) {
    return {.error = runtime_config::configuration_error::allocation_failure};
} catch (const std::exception&) {
    return {.error = runtime_config::configuration_error::unexpected_failure};
} catch (...) {
    return {.error = runtime_config::configuration_error::unexpected_failure};
}

ActivationCheck prepareActivation(ActivationInput input,
                                  const runtime_config::runtime_context& context,
                                  const KernelOptions& options, const std::stop_token& stop) try {
    ActivationCheck result;
    result.diagnostics.assembly = std::move(input.assemblyDiagnostics);
    if (stop.stop_requested()) {
        result.code = TransactionError::Cancelled;
        result.error = QStringLiteral("Activation canceled.");
        return result;
    }
    auto generated = composeActivation(input, context);
    result.diagnostics.configurationError = generated.error;
    result.diagnostics.configuration = std::move(generated.diagnostics);
    result.diagnostics.patches = std::move(generated.patch_reports);
    if (generated.error != runtime_config::configuration_error::none) {
        result.code = TransactionError::Generation;
        result.error = QStringLiteral("Configuration generation rejected the candidate.");
        return result;
    }
    // 三道检查各管一件事：生成器检查配置语义，服务准入检查受控执行约束，
    // 内核 -t 检查当前二进制是否接受配置。任一道失败都不向切换器交付 payload。
    result.stage = TransactionStage::ServiceAdmission;
    result.code = TransactionError::ServiceAdmission;
    if (generated.effective.tun_enabled && !options.serviceManaged) {
        result.error = QStringLiteral("TUN capture requires an installed service session.");
        return result;
    }
    if (generated.config_json.size() > std::size_t{4} * 1024 * 1024) {
        result.error = QStringLiteral("The service configuration limit is 4 MiB.");
        return result;
    }
    const auto payload = QString::fromUtf8(generated.config_json.data(),
                                           static_cast<qsizetype>(generated.config_json.size()));
    const auto admitted = compileServiceConfig(
        payload, 7890, {}, false, QString::fromStdWString(context.data_directory.native()));
    if (!admitted.error.isEmpty()) {
        result.diagnostics.serviceAdmission = admitted.error;
        result.error = QStringLiteral("Service admission rejected the candidate.");
        return result;
    }
    const auto bytes = QJsonDocument(admitted.value).toJson(QJsonDocument::Compact);
    const auto checkInput = nativeCheckInput(admitted.value, bytes);
    result.diagnostics.nativeCheckPolicy = checkInput.policy;
    result.stage = TransactionStage::NativeCheck;

    result.diagnostics.nativeCheck = runtime_config::check_with_kernel(
        std::string_view(checkInput.bytes.constData(),
                         static_cast<std::size_t>(checkInput.bytes.size())),
        {.executable = options.executable.toStdWString(), .data_directory = context.data_directory},
        stop);
    const auto error = result.diagnostics.nativeCheck->error;
    if (error != runtime_config::kernel_check_error::none) {
        result.code = error == runtime_config::kernel_check_error::cancelled
                          ? TransactionError::Cancelled
                          : TransactionError::NativeCheck;
        result.error = QStringLiteral("Native configuration check failed or was canceled.");
        return result;
    }
    result.payload = QString::fromUtf8(bytes);
    result.enableTun = generated.effective.tun_enabled;
    result.code = TransactionError::None;
    return result;
} catch (const std::exception&) {
    return {
        .payload = {},
        .error = QStringLiteral("Configuration preparation failed."),
        .code = TransactionError::Unexpected,
    };
}
