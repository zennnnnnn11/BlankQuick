#ifndef BLANK_PROTOCOL_JSON_FRAME_HPP
#define BLANK_PROTOCOL_JSON_FRAME_HPP

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonObject>
#include <concepts>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace blank::protocol {
inline constexpr qsizetype frameLimit = qsizetype{16} * 1024 * 1024;

enum class FrameError : quint8 {
    TooLarge,
    InvalidJson,
};

template <std::default_initializable T> class FrameResult final {
  public:
    FrameResult() = default;

    explicit FrameResult(T value) : value_(std::move(value)) {}

    explicit FrameResult(FrameError error) : value_(std::nullopt), error_(error) {}

    explicit operator bool() const noexcept {
        return has_value();
    }

    // NOLINTNEXTLINE(readability-identifier-naming): optional-style compatibility query.
    [[nodiscard]] bool has_value() const noexcept {
        return value_.has_value();
    }

    [[nodiscard]] T& value() {
        if (!value_) {
            throw std::bad_optional_access{};
        }
        return *value_;
    }

    [[nodiscard]] const T& value() const {
        if (!value_) {
            throw std::bad_optional_access{};
        }
        return *value_;
    }

    [[nodiscard]] T& operator*() {
        return value();
    }

    [[nodiscard]] const T& operator*() const {
        return value();
    }

    [[nodiscard]] T* operator->() {
        return &value();
    }

    [[nodiscard]] const T* operator->() const {
        return &value();
    }

    [[nodiscard]] FrameError error() const {
        if (!error_) {
            throw std::logic_error("Successful frame result has no error.");
        }
        return *error_;
    }

  private:
    std::optional<T> value_{std::in_place};
    std::optional<FrameError> error_;
};

class JsonFrames final {
  public:
    [[nodiscard]] FrameResult<std::monostate> append(QByteArrayView bytes);
    [[nodiscard]] FrameResult<std::optional<QJsonObject>> next();
    [[nodiscard]] bool hasFrame() const;
    void clear();

  private:
    QByteArray input_;
    qsizetype offset_ = 0;
    std::optional<FrameError> failure_;
};

[[nodiscard]] FrameResult<QByteArray> encodeFrame(const QJsonObject& object);
} // namespace blank::protocol

#endif
