#ifndef BLANK_QUICK_ORDERED_OBJECT_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_ORDERED_OBJECT_HPP

#include <cstddef>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace json_detail {
// CPU loops and explicit member initialization follow repository policy.
// NOLINTBEGIN(altera-unroll-loops,fuchsia-default-arguments-calls,readability-redundant-member-init)

// A scoped index for bulk edits. The object must not be structurally edited elsewhere
// while this index is alive. Positions remain valid when the ordered vector grows.
class ordered_object final {
  public:
    explicit ordered_object(nlohmann::ordered_json& value) : object{value.get_ref<object_type&>()} {
        names.reserve(object.get().size());
        for (const auto& [key, item] : object.get()) {
            static_cast<void>(item);
            names.emplace(key, names.size());
        }
    }
    [[nodiscard]] auto contains(std::string_view key) const -> bool {
        return names.contains(std::string{key});
    }
    [[nodiscard]] auto member(std::string_view key) -> nlohmann::ordered_json& {
        const auto [found, inserted] = names.try_emplace(std::string{key}, names.size());
        if (inserted) {
            object.get().emplace_back(found->first, nlohmann::ordered_json{});
        }
        return static_cast<object_type::Container&>(object.get()).at(found->second).second;
    }

  private:
    using object_type = nlohmann::ordered_json::object_t;
    std::reference_wrapper<object_type> object;
    std::unordered_map<std::string, std::size_t> names{};
};
// NOLINTEND(altera-unroll-loops,fuchsia-default-arguments-calls,readability-redundant-member-init)
} // namespace json_detail

#endif
