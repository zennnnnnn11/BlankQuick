#include <QJsonDocument>
#include <QJsonParseError>
#include <blank/protocol/json_frame.hpp>
#include <utility>

namespace blank::protocol {
FrameResult<std::monostate> JsonFrames::append(QByteArrayView bytes) {
    // 上限针对尚未消费的字节。offset_ 前的内容已经处理，追加前再压缩缓冲区，
    // 避免每读一帧都搬移剩余数据，也避免已消费内容占用下一次接收预算。
    if (failure_) {
        return FrameResult<std::monostate>{*failure_};
    }
    if (bytes.size() > frameLimit - (input_.size() - offset_)) {
        failure_ = FrameError::TooLarge;
        return FrameResult<std::monostate>{*failure_};
    }
    if (offset_ != 0) {
        input_.remove(0, offset_);
        offset_ = 0;
    }
    input_.append(bytes.data(), bytes.size());
    return {};
}

FrameResult<std::optional<QJsonObject>> JsonFrames::next() {
    if (failure_) {
        return FrameResult<std::optional<QJsonObject>>{*failure_};
    }
    const auto end = input_.indexOf('\n', offset_);
    if (end < 0) {
        return FrameResult<std::optional<QJsonObject>>{};
    }
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(input_.sliced(offset_, end - offset_), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        failure_ = FrameError::InvalidJson;
        return FrameResult<std::optional<QJsonObject>>{*failure_};
    }
    offset_ = end + 1;
    if (offset_ == input_.size()) {
        input_.clear();
        offset_ = 0;
    }
    return FrameResult<std::optional<QJsonObject>>{std::optional<QJsonObject>{document.object()}};
}

bool JsonFrames::hasFrame() const {
    return !failure_ && input_.indexOf('\n', offset_) >= 0;
}

void JsonFrames::clear() {
    input_.clear();
    offset_ = 0;
    failure_.reset();
}

FrameResult<QByteArray> encodeFrame(const QJsonObject& object) {
    auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() >= frameLimit) {
        return FrameResult<QByteArray>{FrameError::TooLarge};
    }
    bytes += '\n';
    return FrameResult<QByteArray>{std::move(bytes)};
}
} // namespace blank::protocol
