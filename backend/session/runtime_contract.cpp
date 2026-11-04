#include "runtime_contract.hpp"

#include <stdexcept>

ActivationInput makeActivationInput(const profile_runtime::activation_snapshot& profile,
                                    const runtime_config::settings_snapshot& settings,
                                    const overrides::selection_snapshot& overrides)
{
    if (profile.identity.id.empty() || profile.identity.revision.empty() ||
        profile.identity.generation.empty() || overrides.binding.profile_id != profile.identity.id)
    {
        throw std::invalid_argument("Profile and override selection identities differ.");
    }
    ActivationInput input{
        .material = profile.profile,
        .settings = settings.settings,
        .patches = {},
        .identity =
            {
                .origin = CompositionOrigin::StoredSnapshots,
                .profile = profile.identity,
                .preserveOriginalRules = profile.requested_preserve_original_rules,
                .settingsRevision = settings.revision,
                .bindingRevision = overrides.binding.revision,
                .overrides = {},
            },
        .assemblyDiagnostics = profile.diagnostics,
    };
    input.patches.reserve(overrides.items.size());
    input.identity.overrides.reserve(overrides.items.size());
    for (const auto& item : overrides.items)
    {
        input.patches.push_back(item.item);
        input.identity.overrides.push_back({.id = item.item.id, .revision = item.revision});
    }
    return input;
}
