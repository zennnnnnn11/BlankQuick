#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>

#include <cstdlib>

auto main(int argc, char* argv[]) -> int {
    const QGuiApplication application{argc, argv, QGuiApplication::ApplicationFlags};
    QQmlApplicationEngine engine{nullptr};
    engine.loadFromModule(u"Blank", u"Main");

    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }

    return QGuiApplication::exec();
}
