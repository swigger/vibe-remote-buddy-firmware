#include "window.h"
#include "transport.h"
#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QLocalSocket>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

Window::Window() {
  setWindowTitle("vibeled · USB 状态灯");
  resize(730, 690);
  auto root = new QWidget(this);
  setCentralWidget(root);
  auto layout = new QVBoxLayout(root);
  auto title = new QLabel(
      "<h2>vibeled</h2><p>通过 USB 控制灯光 · 关闭界面后灯效继续运行</p>");
  layout->addWidget(title);
  auto ports = new QHBoxLayout;
  device = new QComboBox;
  device->setMinimumContentsLength(32);
  ports->addWidget(device, 1);
  auto rescan = new QPushButton("刷新设备");
  ports->addWidget(rescan);
  layout->addLayout(ports);
  connect(rescan, &QPushButton::clicked, this, &Window::refresh);
  auto effects = new QGroupBox("灯效");
  auto form = new QFormLayout(effects);
  auto colorBox = [&](const QString &initial) {
    auto combo = new QComboBox;
    combo->setEditable(true);
    combo->addItems({"green", "yellow", "red", "blue", "cyan", "magenta",
                     "orange", "purple", "white", "black"});
    combo->setCurrentText(initial);
    return combo;
  };
  mainColor = colorBox("green");
  altColor = colorBox("black");
  auto addColor = [&](const QString &label, QComboBox *combo) {
    auto row = new QHBoxLayout;
    row->addWidget(combo, 1);
    auto pick = new QPushButton("选色…");
    row->addWidget(pick);
    connect(pick, &QPushButton::clicked, this, [this, combo] {
      QColor color = QColorDialog::getColor(QColor(combo->currentText()), this);
      if (color.isValid())
        combo->setCurrentText(color.name());
    });
    form->addRow(label, row);
  };
  addColor("亮相颜色", mainColor);
  addColor("灭相 / 交替颜色", altColor);
  onMs = new QLineEdit("1000");
  offMs = new QLineEdit("0");
  durationMs = new QLineEdit("0");
  form->addRow("亮相时间（毫秒）", onMs);
  form->addRow("灭相时间（0 = 常亮）", offMs);
  form->addRow("总时长（0 = 持续）", durationMs);
  level = new QComboBox;
  level->addItems({"1", "2", "3", "4", "5"});
  level->setCurrentText("3");
  form->addRow("优先级（高等级覆盖低等级）", level);
  auto actions = new QHBoxLayout;
  auto set = new QPushButton("立即设置");
  auto off = new QPushButton("全部关灯");
  actions->addWidget(set);
  actions->addWidget(off);
  form->addRow(actions);
  layout->addWidget(effects);
  auto effectArgs = [this] {
    return QStringList{mainColor->currentText(),
                       "--altcolor",
                       altColor->currentText(),
                       "--on-ms",
                       onMs->text(),
                       "--off-ms",
                       offMs->text(),
                       "--duration-ms",
                       durationMs->text(),
                       "--level",
                       level->currentText()};
  };
  connect(set, &QPushButton::clicked, this, [this, effectArgs] {
    submit(selection() + QStringList{"set"} + effectArgs());
  });
  connect(off, &QPushButton::clicked, this,
          [this] { submit(selection() + QStringList{"off"}); });
  auto named = new QHBoxLayout;
  preset = new QComboBox;
  preset->setEditable(true);
  preset->addItems(
      {"green", "yellow", "red", "blue", "slow-blue", "heartbeat"});
  named->addWidget(preset, 1);
  auto save = new QPushButton("保存并播放");
  auto play = new QPushButton("播放");
  auto remove = new QPushButton("删除");
  named->addWidget(save);
  named->addWidget(play);
  named->addWidget(remove);
  layout->addLayout(named);
  connect(save, &QPushButton::clicked, this, [this, effectArgs] {
    submit(selection() + QStringList{"save", preset->currentText()} +
           effectArgs());
  });
  connect(play, &QPushButton::clicked, this, [this] {
    submit(selection() + QStringList{"play", preset->currentText(), "--level",
                                     level->currentText()});
  });
  connect(remove, &QPushButton::clicked, this, [this] {
    submit(selection() + QStringList{"delete", preset->currentText()});
  });
  auto queries = new QHBoxLayout;
  for (auto pair : {qMakePair(QString("状态"), QString("status")),
                    qMakePair(QString("列出灯效"), QString("list"))}) {
    auto b = new QPushButton(pair.first);
    queries->addWidget(b);
    connect(b, &QPushButton::clicked, this,
            [this, pair] { submit(selection() + QStringList{pair.second}); });
  }
  auto raw = new QLineEdit;
  raw->setPlaceholderText("原始命令，例如 heartbeat --level 5");
  queries->addWidget(raw, 1);
  auto send = new QPushButton("发送");
  queries->addWidget(send);
  layout->addLayout(queries);
  auto sendRaw = [this, raw] {
    submit(selection() + QStringList{"raw", raw->text()});
  };
  connect(send, &QPushButton::clicked, this, sendRaw);
  connect(raw, &QLineEdit::returnPressed, this, sendRaw);
  auto shortcutRow = new QHBoxLayout;
  shortcutRow->addWidget(new QLabel("全局关灯快捷键"));
#ifdef Q_OS_MACOS
  QString defaultShortcut = "Ctrl+Meta+L";
#else
  QString defaultShortcut = "Ctrl+Alt+L";
#endif
  QSettings settings;
  auto shortcut = new QKeySequenceEdit(
      QKeySequence(settings.value("offShortcut", defaultShortcut).toString()));
  shortcutRow->addWidget(shortcut, 1);
  auto apply = new QPushButton("应用");
  shortcutRow->addWidget(apply);
  auto reset = new QPushButton("恢复默认");
  shortcutRow->addWidget(reset);
  layout->addLayout(shortcutRow);
  log = new QPlainTextEdit;
  log->setReadOnly(true);
  log->setMaximumBlockCount(400);
  layout->addWidget(log, 1);
  auto applyShortcut = [this, shortcut] {
    if (hotkey.set(shortcut->keySequence())) {
      QSettings().setValue("offShortcut", shortcut->keySequence().toString(
                                              QKeySequence::PortableText));
      log->appendPlainText(
          "全局关灯快捷键：" +
          shortcut->keySequence().toString(QKeySequence::NativeText));
    } else
      log->appendPlainText(
          "快捷键注册失败：组合已占用或不受支持。已有快捷键（若有）保持有效。请"
          "选择修饰键 + 字母、数字或 F1–F12。");
  };
  connect(apply, &QPushButton::clicked, this, applyShortcut);
  connect(reset, &QPushButton::clicked, this,
          [shortcut, defaultShortcut, applyShortcut] {
            shortcut->setKeySequence(QKeySequence(defaultShortcut));
            applyShortcut();
          });
  hotkey.activated = [this] { submit(selection() + QStringList{"off"}); };
  applyShortcut();
  auto menu = menuBar()->addMenu("灯光");
  auto offAction = menu->addAction("全部关灯");
  connect(offAction, &QAction::triggered, this, [this] { hotkey.activated(); });
  auto quit = menu->addAction("退出");
  connect(quit, &QAction::triggered, this, &QWidget::close);
  worker = new QObject;
  worker->moveToThread(&thread);
  connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
  thread.start();
  server.setSocketOptions(QLocalServer::UserAccessOption);
  QLocalServer::removeServer(relayName()); // main holds the GUI process lock
  if (!server.listen(relayName()))
    log->appendPlainText("命令转发不可用：" + server.errorString());
  connect(&server, &QLocalServer::newConnection, this, [this] {
    while (auto socket = server.nextPendingConnection()) {
      connect(socket, &QLocalSocket::disconnected, socket,
              &QObject::deleteLater);
      QTimer::singleShot(120000, socket,
                         [socket] { socket->disconnectFromServer(); });
      connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
        QByteArray bytes =
            socket->property("input").toByteArray() + socket->readAll();
        if (bytes.size() > 4096 || socket->property("submitted").toBool()) {
          socket->disconnectFromServer();
          return;
        }
        socket->setProperty("input", bytes);
        if (!bytes.contains('\n'))
          return;
        socket->setProperty("submitted", true);
        auto doc = QJsonDocument::fromJson(bytes.left(bytes.indexOf('\n')));
        if (!doc.isArray()) {
          socket->disconnectFromServer();
          return;
        }
        QStringList args;
        for (const auto &v : doc.array()) {
          if (!v.isString()) {
            socket->disconnectFromServer();
            return;
          }
          args.append(v.toString());
        }
        if (args.isEmpty()) {
          showNormal();
          raise();
          activateWindow();
        }
        submit(args, socket);
      });
    }
  });
  refresh();
}
Window::~Window() {
  server.close();
  thread.quit();
  thread.wait();
}
void Window::refresh() {
  QString previous = device->currentData().toString();
  if (previous.isEmpty())
    previous = QSettings().value("device").toString();
  device->clear();
  device->addItem("自动选择唯一接收器", QString());
  for (const auto &p : devices())
    device->addItem(p.portName() + " · " + p.serialNumber(),
                    p.serialNumber().isEmpty() ? p.systemLocation()
                                               : p.serialNumber());
  int index = device->findData(previous);
  if (index >= 0)
    device->setCurrentIndex(index);
}
QStringList Window::selection() const {
  QString selected = device->currentData().toString();
  QSettings().setValue("device", selected);
  return selected.isEmpty() ? QStringList{} : QStringList{"--device", selected};
}
void Window::submit(QStringList args, QLocalSocket *client) {
  QPointer<QLocalSocket> reply(client);
  QElapsedTimer queued;
  queued.start();
  QMetaObject::invokeMethod(
      worker,
      [this, args, reply, queued] {
        Command command;
        QString error;
        Result result;
        if (!parseCommand(args, command, error))
          result = {2, error};
        else if (command.kind == Command::Gui)
          result = {0, "vibeled window activated"};
        else if (queued.elapsed() >= command.timeoutMs)
          result = {1, "Command expired while waiting in the GUI queue"};
        else {
          command.timeoutMs -= int(queued.elapsed());
          result = execute(command);
        }
        QMetaObject::invokeMethod(
            this,
            [this, args, reply, result] {
              // Never display Wi-Fi passwords from compatibility invocations.
              log->appendPlainText(
                  QDateTime::currentDateTime().toString("HH:mm:ss ") +
                  (args.contains("wifi") ? "wifi (unsupported)"
                                         : args.join(' ')) +
                  "\n" + result.text);
              if (result.code == 0 && result.text.startsWith("ok builtin=")) {
                QString names = result.text.mid(11);
                names.replace(" saved=", ",");
                QString current = preset->currentText();
                preset->clear();
                preset->addItems(names.split(',', Qt::SkipEmptyParts));
                preset->setCurrentText(current);
              }
              if (reply) {
                reply->write(QJsonDocument(QJsonObject{{"code", result.code},
                                                       {"text", result.text}})
                                 .toJson(QJsonDocument::Compact) +
                             '\n');
                reply->disconnectFromServer();
              }
            },
            Qt::QueuedConnection);
      },
      Qt::QueuedConnection);
}
