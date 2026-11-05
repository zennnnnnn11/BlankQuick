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

// 管道是字节流，一次读取可能只有半帧，也可能包含多帧。next 的三态分别是：
// 失败、成功但尚无完整帧、成功且取出对象；“暂时没帧”不是连接错误。
// 一旦发现坏帧或超限就保持失败，必须 clear 后才可用于新的连接，避免失去帧边界。
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
