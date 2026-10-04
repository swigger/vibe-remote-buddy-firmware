#include "cli.h"
#include "transport.h"
#include "window.h"
#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QLockFile>
#include <QStandardPaths>
#include <QTextStream>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

int main(int argc, char **argv) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
  // Command parsing/help never initializes a GUI, Bluetooth or serial port.
  if (argc > 1) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("vibeled");
    app.setApplicationName("vibeled");
    Command command;
    QString error;
    auto args = app.arguments().mid(1);
    if (!parseCommand(args, command, error)) {
      QTextStream(stderr) << error << '\n';
      return 2;
    }
    Result result;
    if (command.kind == Command::Help || command.kind == Command::Version ||
        command.kind == Command::Wifi ||
        !forwardToGui(args, command.timeoutMs, result))
      result = execute(command);
    if (!result.text.isEmpty())
      QTextStream(result.code ? stderr : stdout) << result.text << '\n';
    return result.code;
  }
#ifdef Q_OS_WIN
  // A console-subsystem executable preserves pipes/exit codes for CLI use.
  // Hide only a console owned by this GUI process (Explorer launch).
  DWORD processIds[2];
  if (GetConsoleProcessList(processIds, 2) == 1)
    FreeConsole();
#endif
  QApplication app(argc, argv);
  app.setOrganizationName("vibeled");
  app.setApplicationName("vibeled");
  QIcon icon;
  for (int size : {16, 20, 24, 32, 40, 48, 64, 128, 256, 512, 1024})
    icon.addFile(QString(":/icons/vibeled-%1.png").arg(size), QSize(size, size));
  app.setWindowIcon(icon);
  Result result;
  if (forwardToGui({}, 1000, result))
    return result.code;
  QString path =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation) + '/' +
      relayName() + "-gui.lock";
  QLockFile lock(path);
  lock.setStaleLockTime(0);
  if (!lock.tryLock(1000)) {
    if (forwardToGui({}, 1000, result))
      return result.code;
    QTextStream(stderr) << "vibeled GUI is already starting\n";
    return 1;
  }
  Window window;
  window.show();
  return app.exec();
}
