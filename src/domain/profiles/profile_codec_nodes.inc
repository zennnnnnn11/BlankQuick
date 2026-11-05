

// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)
using cache_key_id = std::int64_t;
using cache_scalar =
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string>;
enum class cache_token_kind : std::int8_t { scalar, key, object, array };
struct cache_token {
    // 单节点的扁平事件带：after 指向整个值之后的位置，容器无需递归扫描
    // 全部后代就能跳到下一个字段；count 用于校验数组/记录形状。
    cache_scalar value{};
    std::size_t after{};
    std::size_t count{};
    cache_token_kind kind{cache_token_kind::scalar};
};
struct cache_parent {
    std::size_t index{};
    std::bitset<cache_field_names.size()> seen{};
};
struct cache_tape {
    std::vector<cache_token> tokens{};
    std::vector<cache_parent> parents{};
};

[[nodiscard]] constexpr auto cache_name_hash(std::string_view name) noexcept -> std::uint64_t {
    constexpr std::uint64_t offset{14695981039346656037ULL};
    constexpr std::uint64_t prime{1099511628211ULL};
    auto result = offset;
    for (const auto character : name) {
        result ^= static_cast<unsigned char>(character);
        result *= prime;
    }
    return result;
}
using cache_slot_table = std::array<cache_key_id, std::bit_ceil(cache_field_names.size() * 2)>;

[[nodiscard]] consteval auto make_cache_field_slots() -> cache_slot_table {
    auto buckets = cache_slot_table{};
    buckets.fill(-1);
    auto field = cache_key_id{};
    for (const auto name : cache_field_names) {
        auto position = cache_name_hash(name) & (buckets.size() - 1);
        while (buckets.at(position) >= 0) {
            position = (position + 1) & (buckets.size() - 1);
        }
        buckets.at(position) = field++;
    }
    return buckets;
}
inline constexpr auto cache_field_slots = make_cache_field_slots();
[[nodiscard]] constexpr auto cache_key(std::string_view name) -> cache_key_id {
    auto position = cache_name_hash(name) & (cache_field_slots.size() - 1);
    for (auto probes = std::size_t{}; probes < cache_field_slots.size(); ++probes) {
        const auto field = cache_field_slots.at(position);
        if (field < 0) {
            invalid();
        }
        if (cache_field_names.at(static_cast<std::size_t>(field)) == name) {
            return field;
        }
        position = (position + 1) & (cache_field_slots.size() - 1);
    }
    invalid();
}

auto append_cache_value(cache_tape& tape, cache_token token) -> void {
    if (!tape.parents.empty()) {
        ++tape.tokens.at(tape.parents.back().index).count;
    }
    token.after = tape.tokens.size() + 1;
    tape.tokens.push_back(std::move(token));
}

template <typename Token>
    requires(std::same_as<Token, cache_token> || std::same_as<Token, const cache_token>)
[[nodiscard]] auto cache_at(std::span<Token> tokens, std::size_t index) -> Token& {
    if (index >= tokens.size()) {
        invalid();
    }
    return tokens.subspan(index).front();
}

template <std::size_t Count>
[[nodiscard]] auto cache_object_fields(std::span<const cache_token> tape, std::size_t index,
                                       const std::array<cache_key_id, Count>& keys)
    -> std::array<std::size_t, Count> {
    const auto& object = cache_at(tape, index);
    if (object.kind != cache_token_kind::object || object.count != Count) {
        invalid();
    }
    auto positions = std::array<std::size_t, Count>{};
    for (auto cursor = index + 1; cursor < object.after;) {
        const auto& key = cache_at(tape, cursor);
        if (key.kind != cache_token_kind::key) {
            invalid();
        }
        const auto found = std::ranges::find(keys, std::get<cache_key_id>(key.value));
        if (found == keys.end()) {
            invalid();
        }
        auto& position = positions.at(static_cast<std::size_t>(std::distance(keys.begin(), found)));
        if (position != 0) {
            invalid();
        }
        position = cursor + 1;
        cursor = cache_at(tape, position).after;
    }
    if (std::ranges::find(positions, std::size_t{}) != positions.end()) {
        invalid();
    }
    return positions;
}

[[nodiscard]] auto cache_integer(const cache_scalar& value) -> std::int64_t {
    if (const auto* number = std::get_if<std::int64_t>(&value)) {
        return *number;
    }
    if (const auto* number = std::get_if<std::uint64_t>(&value);
        number != nullptr && std::in_range<std::int64_t>(*number)) {
        return static_cast<std::int64_t>(*number);
    }
    invalid();
}

[[nodiscard]] auto cache_text(const cache_token& token) -> std::string_view {
    if (token.kind != cache_token_kind::scalar ||
        !std::holds_alternative<std::string>(token.value)) {
        invalid();
    }
    return std::get<std::string>(token.value);
}

template <cache_value Value>
[[nodiscard]] auto read_cache_value(std::span<cache_token> tape, std::size_t index) -> Value;

template <cache_value Value, std::size_t... Indices>
[[nodiscard]] auto read_cache_record(std::span<cache_token> tape, std::size_t index,
                                     [[maybe_unused]] std::index_sequence<Indices...> indices)
    -> Value {
    constexpr auto& members = record_traits<Value>::members;
    constexpr auto keys =
        std::to_array<cache_key_id>({cache_key(std::get<Indices>(members).first)...});
    const auto positions = cache_object_fields(tape, index, keys);
    auto result = Value{};
    ((result.*std::get<Indices>(members).second = read_cache_value<
          std::remove_cvref_t<decltype(result.*std::get<Indices>(members).second)>>(
          tape, positions.at(Indices))),
     ...);
    return result;
}

template <cache_value Value, std::size_t Index>
[[nodiscard]] auto read_cache_variant(std::span<cache_token> tape,
                                      const std::array<std::size_t, 2>& fields) -> Value {
    if constexpr (Index == std::variant_size_v<Value>) {
        invalid();
    } else {
        using alternative = std::variant_alternative_t<Index, Value>;
        if (cache_text(cache_at(tape, fields.front())) == variant_tag<alternative>::name) {
            return Value{
                std::in_place_index<Index>,
                read_cache_value<alternative>(tape, fields.back()),
            };
        }
        return read_cache_variant<Value, Index + 1>(tape, fields);
    }
}

template <std::integral Value>
[[nodiscard]] auto cache_checked_integer(const cache_scalar& value) -> Value {
    const auto number = cache_integer(value);
    if (!std::in_range<Value>(number)) {
        invalid();
    }
    return static_cast<Value>(number);
}

template <std::floating_point Value>
[[nodiscard]] auto cache_real(const cache_scalar& value) -> Value {
    const auto number = std::visit(
        []<typename Number>(const Number& input) -> Value {
            if constexpr ((std::integral<Number> && !std::same_as<Number, bool>) ||
                          std::floating_point<Number>) {
                return static_cast<Value>(input);
            } else {
                invalid();
            }
        },
        value);
    if (!std::isfinite(number)) {
        invalid();
    }
    return number;
}

template <cache_value Value>
[[nodiscard]] auto read_cache_scalar(std::span<cache_token> tape, std::size_t index) -> Value {
    const auto& token = cache_at(tape, index);
    if (token.kind != cache_token_kind::scalar) {
        invalid();
    }
    const auto& value = token.value;
    if constexpr (std::same_as<Value, std::monostate>) {
        if (!std::holds_alternative<std::monostate>(value)) {
            invalid();
        }
        return {};
    } else if constexpr (std::same_as<Value, bool>) {
        if (!std::holds_alternative<bool>(value)) {
            invalid();
        }
        return std::get<bool>(value);
    } else if constexpr (std::same_as<Value, std::byte> || std::same_as<Value, char>) {
        return static_cast<Value>(cache_checked_integer<unsigned char>(value));
    } else if constexpr (std::integral<Value>) {
        return cache_checked_integer<Value>(value);
    } else if constexpr (std::floating_point<Value>) {
        return cache_real<Value>(value);
    } else if constexpr (std::same_as<Value, std::string>) {
        static_cast<void>(cache_text(token));
        return std::move(std::get<std::string>(cache_at(tape, index).value));
    } else if constexpr (std::is_enum_v<Value>) {
        const auto found = std::ranges::find(enum_traits<Value>::values, cache_text(token),
                                             &std::pair<std::string_view, Value>::first);
        if (found == enum_traits<Value>::values.end()) {
            invalid();
        }
        return found->second;
    } else if constexpr (value_traits<Value>::kind == value_kind::duration) {
        return Value{cache_checked_integer<typename Value::rep>(value)};
    }
}

template <cache_value Value>
[[nodiscard]] auto read_cache_sequence(std::span<cache_token> tape, std::size_t index) -> Value {
    const auto& token = cache_at(tape, index);
    if (token.kind != cache_token_kind::array) {
        invalid();
    }
    auto result = Value{};
    using element = value_traits<Value>::element_type;
    auto cursor = index + 1;
    if constexpr (value_traits<Value>::kind == value_kind::vector) {
        result.reserve(token.count);
        while (cursor < token.after) {
            result.push_back(read_cache_value<element>(tape, cursor));
            cursor = cache_at(tape, cursor).after;
        }
    } else {
        if (token.count != result.size()) {
            invalid();
        }
        for (auto& item : result) {
            item = read_cache_value<element>(tape, cursor);
            cursor = cache_at(tape, cursor).after;
        }
    }
    return result;
}

[[nodiscard]] auto read_cache_timepoint(std::span<cache_token> tape, std::size_t index)
    -> std::chrono::system_clock::time_point {
    constexpr auto keys = std::to_array<cache_key_id>(
        {cache_key("ticks"), cache_key("period_num"), cache_key("period_den")});
    const auto fields = cache_object_fields(tape, index, keys);
    using period = std::chrono::system_clock::period;
    if (read_cache_value<std::int64_t>(tape, fields.at(1)) != period::num ||
        read_cache_value<std::int64_t>(tape, fields.at(2)) != period::den) {
        throw storage_failure{storage_error::unsupported_version};
    }
    return std::chrono::system_clock::time_point{std::chrono::system_clock::duration{
        read_cache_value<std::chrono::system_clock::rep>(tape, fields.front())}};
}

template <cache_value Value>
[[nodiscard]] auto read_cache_value(std::span<cache_token> tape, std::size_t index) -> Value {
    constexpr auto kind = value_traits<Value>::kind;
    const auto& token = cache_at(tape, index);
    if constexpr (kind == value_kind::optional || kind == value_kind::pointer) {
        if (token.kind == cache_token_kind::scalar &&
            std::holds_alternative<std::monostate>(token.value)) {
            return {};
        }
        using element = value_traits<Value>::element_type;
        if constexpr (kind == value_kind::pointer) {
            return std::make_unique<element>(read_cache_value<element>(tape, index));
        } else {
            return read_cache_value<element>(tape, index);
        }
    } else if constexpr (kind == value_kind::variant) {
        constexpr auto keys = std::to_array<cache_key_id>({cache_key("type"), cache_key("value")});
        return read_cache_variant<Value, 0>(tape, cache_object_fields(tape, index, keys));
    } else if constexpr (kind == value_kind::pair) {
        if (token.kind != cache_token_kind::array || token.count != 2) {
            invalid();
        }
        const auto first = index + 1;
        return {
            read_cache_value<typename Value::first_type>(tape, first),
            read_cache_value<typename Value::second_type>(tape, cache_at(tape, first).after),
        };
    } else if constexpr (kind == value_kind::vector || kind == value_kind::array) {
        return read_cache_sequence<Value>(tape, index);
    } else if constexpr (std::same_as<Value, std::chrono::system_clock::time_point>) {
        return read_cache_timepoint(tape, index);
    } else if constexpr (requires { record_traits<Value>::members; }) {
        constexpr auto count = std::tuple_size_v<decltype(record_traits<Value>::members)>;
        if constexpr (count == 0) {
            if (token.kind != cache_token_kind::object || token.count != 0) {
                invalid();
            }
            return {};
        } else {
            return read_cache_record<Value>(tape, index, std::make_index_sequence<count>{});
        }
    } else {
        return read_cache_scalar<Value>(tape, index);
    }
}

// 外层信封由 SAX 状态机直接校验；节点只暂存一份 token tape，构造完一个
// 强类型节点就清空 tape。这样不会同时持有整份通用 JSON 树和整份节点模型。
class node_cache_reader final : public nlohmann::json_sax<json> {
  public:
    explicit node_cache_reader(std::int64_t selected_limit) noexcept : limit{selected_limit} {}
    auto null() -> bool override {
        return scalar(std::monostate{});
    }
    auto boolean(bool value) -> bool override {
        return scalar(value);
    }
    auto number_integer(json::number_integer_t value) -> bool override {
        return scalar(value);
    }
    auto number_unsigned(json::number_unsigned_t value) -> bool override {
        return scalar(value);
    }
    auto number_float(json::number_float_t value, [[maybe_unused]] const json::string_t& token)
        -> bool override {
        return scalar(value);
    }
    auto string(json::string_t& value) -> bool override {
        return scalar(std::move(value));
    }
    auto binary([[maybe_unused]] json::binary_t& value) noexcept -> bool override {
        return false;
    }
    auto parse_error([[maybe_unused]] std::size_t position,
                     [[maybe_unused]] const std::string& token,
                     [[maybe_unused]] const json::exception& error) noexcept -> bool override {
        return false;
    }

    auto key(json::string_t& name) -> bool override {
        check_depth();
        if (depth == 1) {
            if (name == "version") {
                field = root_field::version;
            } else if (name == "model_schema") {
                field = root_field::schema;
            } else if (name == "nodes") {
                field = root_field::nodes;
            } else {
                field = root_field::none;
            }
            if (field == root_field::none) {
                invalid();
            }
            auto& used = seen.at(static_cast<std::size_t>(field));
            if (used) {
                invalid();
            }
            used = true;
        } else {
            require_node();
            if (tape.parents.empty()) {
                invalid();
            }
            const auto key_id = cache_key(name);
            auto& keys = tape.parents.back().seen;
            const auto position = static_cast<std::size_t>(key_id);
            if (keys.test(position)) {
                invalid();
            }
            keys.set(position);
            tape.tokens.push_back({.value = key_id, .kind = cache_token_kind::key});
        }
        return true;
    }
    auto start_object([[maybe_unused]] std::size_t elements) -> bool override {
        check_depth();
        if (depth == 0) {
            if (started) {
                invalid();
            }
            started = true;
        } else {
            start_container(cache_token_kind::object);
        }
        ++depth;
        return true;
    }
    auto end_object() -> bool override {
        if (depth == 1) {
            finished = true;
            --depth;
        } else {
            end_container();
        }
        return true;
    }
    auto start_array([[maybe_unused]] std::size_t elements) -> bool override {
        check_depth();
        if (depth == 1 && field == root_field::nodes) {
            nodes_array = true;
        } else {
            start_container(cache_token_kind::array);
        }
        ++depth;
        return true;
    }
    auto end_array() -> bool override {
        if (depth == 2 && tape.parents.empty()) {
            nodes_array = false;
            field = root_field::none;
            --depth;
        } else {
            end_container();
        }
        return true;
    }
    [[nodiscard]] auto take_nodes() -> std::vector<node_model::unified_node> {
        // 字节解析结束还不够：信封必须完整出现，版本和模型 schema 必须匹配。
        // 不支持的版本不能当作空缓存返回，否则会丢失一次应明确报告的迁移需求。
        if (!finished || !seen.at(1) || !seen.at(2) || !seen.at(3)) {
            invalid();
        }
        if (version != 1 || schema != model_schema) {
            throw storage_failure{storage_error::unsupported_version};
        }
        return std::move(nodes);
    }

  private:
    enum class root_field : std::int8_t { none, version, schema, nodes };
    static constexpr int max_depth{64};
    std::int64_t limit{};
    std::int64_t version{};
    std::string schema{};
    cache_tape tape{};
    std::vector<node_model::unified_node> nodes{};
    std::array<bool, 4> seen{};
    root_field field{root_field::none};
    int depth{};
    bool started{};
    bool finished{};
    bool nodes_array{};

    auto check_depth() const -> void {
        if (depth > max_depth) {
            throw storage_failure{storage_error::resource_limit};
        }
    }
    auto require_node() const -> void {
        if (!nodes_array || depth < 2) {
            invalid();
        }
        if (std::ssize(nodes) >= limit) {
            throw storage_failure{storage_error::resource_limit};
        }
    }
    auto complete_node() -> void {
        nodes.push_back(read_cache_value<node_model::unified_node>(tape.tokens, 0));
        tape.tokens.clear();
    }
    auto scalar(cache_scalar value) -> bool {
        check_depth();
        if (depth == 1) {
            if (field == root_field::version) {
                version = cache_integer(value);
            } else if (field == root_field::schema && std::holds_alternative<std::string>(value)) {
                schema = std::move(std::get<std::string>(value));
            } else {
                invalid();
            }
            field = root_field::none;
        } else {
            require_node();
            append_cache_value(tape, {.value = std::move(value)});
            if (depth == 2) {
                complete_node();
            }
        }
        return true;
    }
    auto start_container(cache_token_kind kind) -> void {
        require_node();
        append_cache_value(tape, {.kind = kind});
        tape.parents.push_back({.index = tape.tokens.size() - 1});
    }
    auto end_container() -> void {
        if (tape.parents.empty()) {
            invalid();
        }
        tape.tokens.at(tape.parents.back().index).after = tape.tokens.size();
        tape.parents.pop_back();
        --depth;
        if (depth == 2) {
            complete_node();
        }
    }
};

template <cache_value Value>
auto write_cache_value(std::string& output, const Value& value) -> void;

auto write_cache_literal(std::string& output, std::string_view value) -> void {
    output += '"';
    output += value;
    output += '"';
}

auto write_cache_string(std::string& output, const std::string& value) -> void {
    constexpr unsigned char first_printable{' '};
    constexpr unsigned char ascii_limit{0x80};
    const auto plain = std::ranges::all_of(value, [](char character) noexcept -> bool {
        const auto octet = static_cast<unsigned char>(character);
        return octet >= first_printable && octet < ascii_limit && character != '"' &&
               character != '\\';
    });
    if (plain) {
        write_cache_literal(output, value);
        return;
    }
    output += json(value).dump();
}

template <std::integral Value>
    requires(!std::same_as<Value, bool>)
auto write_cache_integer(std::string& output, Value value) -> void {
    auto buffer = std::array<char, std::numeric_limits<Value>::digits10 + 3>{};
    const auto encoded = std::to_chars(buffer.data(), std::to_address(buffer.end()), value);
    if (encoded.ec != std::errc{}) {
        invalid();
    }
    output.append(buffer.data(),
                  static_cast<std::size_t>(std::distance(buffer.data(), encoded.ptr)));
}

template <scalar_cache_value Value>
    requires(value_traits<Value>::kind != value_kind::optional)
auto write_cache_scalar(std::string& output, const Value& value) -> void {
    if constexpr (std::same_as<Value, std::monostate>) {
        output += "null";
    } else if constexpr (std::same_as<Value, bool>) {
        output += value ? "true" : "false";
    } else if constexpr (std::same_as<Value, std::byte> || std::same_as<Value, char>) {
        write_cache_integer(output, static_cast<unsigned char>(value));
    } else if constexpr (value_traits<Value>::kind == value_kind::duration) {
        write_cache_integer(output, value.count());
    } else {
        write_cache_integer(output, value);
    }
}

template <cache_value Value>
auto write_cache_sequence(std::string& output, const Value& value) -> void {
    output += '[';
    auto comma = false;
    for (const auto& item : value) {
        if (comma) {
            output += ',';
        }
        comma = true;
        write_cache_value(output, item);
    }
    output += ']';
}

template <cache_value Value>
auto write_cache_record(std::string& output, const Value& value) -> void {
    output += '{';
    auto comma = false;
    const auto write_field = [&output, &value, &comma](const auto& member) -> void {
        if (comma) {
            output += ',';
        }
        comma = true;
        write_cache_literal(output, member.first);
        output += ':';
        write_cache_value(output, value.*member.second);
    };
    std::apply([&write_field](const auto&... fields) -> void { (write_field(fields), ...); },
               record_traits<Value>::members);
    output += '}';
}

auto write_cache_timepoint(std::string& output, std::chrono::system_clock::time_point value)
    -> void {
    using period = std::chrono::system_clock::period;
    output += R"({"ticks":)";
    write_cache_value(output, value.time_since_epoch().count());
    output += R"(,"period_num":)";
    write_cache_value(output, period::num);
    output += R"(,"period_den":)";
    write_cache_value(output, period::den);
    output += '}';
}

template <cache_value Value>
auto write_cache_value(std::string& output, const Value& value) -> void {
    constexpr auto kind = value_traits<Value>::kind;
    if constexpr (kind == value_kind::optional || kind == value_kind::pointer) {
        if (value) {
            write_cache_value(output, *value);
        } else {
            output += "null";
        }
    } else if constexpr (std::same_as<Value, std::string>) {
        write_cache_string(output, value);
    } else if constexpr (std::floating_point<Value>) {
        if (!std::isfinite(value)) {
            invalid();
        }
        output += json(value).dump();
    } else if constexpr (scalar_cache_value<Value>) {
        write_cache_scalar(output, value);
    } else if constexpr (std::is_enum_v<Value>) {
        const auto found = std::ranges::find(enum_traits<Value>::values, value,
                                             &std::pair<std::string_view, Value>::second);
        if (found == enum_traits<Value>::values.end()) {
            invalid();
        }
        write_cache_literal(output, found->first);
    } else if constexpr (std::same_as<Value, std::chrono::system_clock::time_point>) {
        write_cache_timepoint(output, value);
    } else if constexpr (kind == value_kind::variant) {
        if (value.valueless_by_exception()) {
            invalid();
        }
        output += R"({"type":)";
        std::visit(
            [&output]<cache_value Alternative>(const Alternative& item) -> void {
                write_cache_literal(output, variant_tag<Alternative>::name);
                output += R"(,"value":)";
                write_cache_value(output, item);
            },
            value);
        output += '}';
    } else if constexpr (kind == value_kind::pair) {
        output += '[';
        write_cache_value(output, value.first);
        output += ',';
        write_cache_value(output, value.second);
        output += ']';
    } else if constexpr (kind == value_kind::vector || kind == value_kind::array) {
        write_cache_sequence(output, value);
    } else {
        write_cache_record(output, value);
    }
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)
