#include <Windows.h> // IWYU pragma: keep; Windows SDK headers require the public umbrella.

#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QObject>
#include <QtCore/Qt>
#include <QtCore/qlogging.h>
#include <QtCore/qtypes.h>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>

#include <bit>
#include <cstdlib>
#include <dwmapi.h>
#include <minwindef.h>
#include <uxtheme.h>
#include <windef.h>
#include <winerror.h>
#include <winnt.h>
#include <winuser.h>

namespace {
// Repository policy requires anonymous namespaces for implementation-local functions.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace)
class blank_window_events final : public QAbstractNativeEventFilter {
  public:
    explicit blank_window_events(QQuickWindow& window)
        : native_window{std::bit_cast<HWND>(window.winId())} {}

    blank_window_events(const blank_window_events&) = delete;
    blank_window_events(blank_window_events&&) = delete;
    auto operator=(const blank_window_events&) -> blank_window_events& = delete;
    auto operator=(blank_window_events&&) -> blank_window_events& = delete;
    ~blank_window_events() noexcept override = default;

    auto nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result) noexcept
        -> bool override {
        if (event_type != "windows_generic_MSG" || message == nullptr || result == nullptr) {
            return false;
        }

        const auto* const native_message = static_cast<const MSG*>(message);
        if (native_message->hwnd != native_window) {
            return false;
        }

        switch (native_message->message) {
        case WM_NCHITTEST:
            *result = HTCAPTION;
            return true;
        case WM_NCLBUTTONDBLCLK:
            *result = 0;
            return true;
        case WM_SYSCOMMAND: {
            constexpr WPARAM command_mask{0xFFF0U};
            const auto command = native_message->wParam & command_mask;
            if (command == WPARAM{SC_SIZE} || command == WPARAM{SC_MAXIMIZE}) {
                *result = 0;
                return true;
            }
            return false;
        }
        default:
            return false;
        }
    }

  private:
    HWND const native_window; // NOLINT(misc-misplaced-const): the handle itself is immutable.
};

[[nodiscard]] auto enable_mica(HWND window, bool dark) noexcept -> HRESULT {
    const BOOL dark_mode{dark ? TRUE : FALSE};
    static_cast<void>(DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark_mode,
                                            sizeof(dark_mode)));
    const auto backdrop = DWMSBT_MAINWINDOW;
    const auto backdrop_result =
        DwmSetWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    if (FAILED(backdrop_result)) {
        return backdrop_result;
    }

    const auto corners = DWMWCP_ROUND;
    static_cast<void>(
        DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners)));
    constexpr COLORREF border{DWMWA_COLOR_NONE};
    static_cast<void>(DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &border, sizeof(border)));

    constexpr MARGINS margins{
        .cxLeftWidth = -1,
        .cxRightWidth = -1,
        .cyTopHeight = -1,
        .cyBottomHeight = -1,
    };
    const auto frame_result = DwmExtendFrameIntoClientArea(window, &margins);
    if (SUCCEEDED(frame_result)) {
        return frame_result;
    }

    const auto disabled = DWMSBT_NONE;
    static_cast<void>(
        DwmSetWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE, &disabled, sizeof(disabled)));
    return frame_result;
}

auto update_backdrop(QQuickWindow& window) -> void {
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    auto* const native_window = std::bit_cast<HWND>(window.winId());
    const auto result = enable_mica(native_window, dark);
    window.setColor(SUCCEEDED(result) ? QColor{Qt::transparent}
                                      : QGuiApplication::palette().color(QPalette::Window));
    if (FAILED(result)) {
        qWarning() << "The native Mica backdrop request failed:" << result;
    }
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace)
} // namespace

auto main(int argc, char* argv[]) -> int {
    const QGuiApplication application{argc, argv, QGuiApplication::ApplicationFlags};
    QQuickWindow::setDefaultAlphaBuffer(true);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQmlApplicationEngine engine{nullptr};
    engine.loadFromModule(u"Blank", u"Main");

    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }

    auto* const window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (window == nullptr) {
        return EXIT_FAILURE;
    }
    blank_window_events window_events{*window};
    QCoreApplication::instance()->installNativeEventFilter(&window_events);
    update_backdrop(*window);
    QObject::connect(
        QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, window,
        [window](Qt::ColorScheme) -> void { update_backdrop(*window); }, Qt::AutoConnection);
    QObject::connect(
        window, &QQuickWindow::frameSwapped, window,
        [window]() -> void { update_backdrop(*window); }, Qt::SingleShotConnection);
    window->show();

    return QGuiApplication::exec();
}
