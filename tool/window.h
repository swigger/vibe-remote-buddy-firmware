#pragma once
#include "hotkey.h"
#include <QLocalServer>
#include <QMainWindow>
#include <QPointer>
#include <QThread>
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QLocalSocket;

class Window : public QMainWindow {
  Q_OBJECT
public:
  Window();
  ~Window() override;

private:
  QComboBox *device, *mainColor, *altColor, *level, *preset;
  QLineEdit *onMs, *offMs, *durationMs;
  QPlainTextEdit *log;
  QLocalServer server;
  QThread thread;
  QObject *worker;
  Hotkey hotkey;
  void refresh();
  void submit(QStringList args, QLocalSocket *client = nullptr);
  QStringList selection() const;
};
