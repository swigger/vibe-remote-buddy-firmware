#include "cli.h"
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <limits>

namespace {
bool name(const QString &s) {
  return QRegularExpression("^[A-Za-z0-9_-]{1,15}$").match(s).hasMatch();
}
bool color(QString &s) {
  s = s.toLower();
  static const QSet<QString> names{"black",  "white",  "red",  "green",
                                   "blue",   "yellow", "cyan", "magenta",
                                   "orange", "purple"};
  if (names.contains(s))
    return true;
  if (s.startsWith('#'))
    s.remove(0, 1);
  if (!QRegularExpression("^[0-9a-f]{6}$").match(s).hasMatch())
    return false;
  s.prepend('#');
  return true;
}
bool number(const QString &s, quint32 &value) {
  if (!QRegularExpression("^[+]?[0-9]+$").match(s).hasMatch())
    return false;
  bool ok;
  qulonglong n = s.toULongLong(&ok);
  if (!ok || n > 0xffffffffULL)
    return false;
  value = quint32(n);
  return true;
}
} // namespace

bool parseCommand(QStringList args, Command &out, QString &error) {
  out = {};
  auto fail = [&](const QString &s) {
    error = s;
    return false;
  };
  if (args.isEmpty())
    return true;
  if (args == QStringList{"--help"} || args == QStringList{"-h"} ||
      args == QStringList{"help"}) {
    out.kind = Command::Help;
    return true;
  }
  if (args == QStringList{"--version"}) {
    out.kind = Command::Version;
    return true;
  }
  while (!args.isEmpty() && args.first().startsWith("--")) {
    QString option = args.takeFirst();
    if (option != "--device" && option != "--timeout")
      return fail("Unknown option: " + option);
    if (args.isEmpty())
      return fail(option + " requires a value");
    QString value = args.takeFirst();
    if (option == "--device") {
      if (value.isEmpty())
        return fail("--device requires a port, serial number or device name");
      out.device = value;
    } else {
      bool ok;
      double seconds = value.toDouble(&ok);
      if (!ok || !std::isfinite(seconds) || seconds <= 0 ||
          seconds > double(INT_MAX) / 1000)
        return fail("--timeout must be a positive finite number of seconds "
                    "(maximum 2147483)");
      out.timeoutMs = qMax(1, int(std::ceil(seconds * 1000)));
    }
  }
  if (args.isEmpty())
    return fail("Missing command; use --help");
  QString action = args.takeFirst().toLower();
  if (action == "scan") {
    if (!args.isEmpty() && args != QStringList{"--all"})
      return fail("Usage: scan [--all]");
    out.kind = Command::Scan;
    out.all = !args.isEmpty();
    return true;
  }
  out.kind = Command::Light;
  if (action == "wifi") {
    if (args.size() != 2)
      return fail("Usage: wifi SSID PASSWORD");
    if (args[0].toUtf8().isEmpty() || args[0].toUtf8().size() > 31 ||
        args[1].toUtf8().size() > 63)
      return fail("SSID must be 1..31 bytes; password at most 63 bytes");
    out.kind = Command::Wifi;
    return true;
  }
  if (action == "raw") {
    if (args.isEmpty())
      return fail("Usage: raw COMMAND...");
    out.text = args.join(' ');
  } else if (action == "list" || action == "status" || action == "off" ||
             action == "getip") {
    if (!args.isEmpty())
      return fail("Usage: " + action);
    out.text = action;
  } else if (action == "delete") {
    if (args.size() != 1 || !name(args[0]))
      return fail("Usage: delete NAME (1..15 ASCII letters, digits, _ or -)");
    out.text = "delete " + args[0];
  } else if (action == "play") {
    if ((args.size() != 1 && args.size() != 3) || !name(args[0]))
      return fail("Usage: play NAME [--level 1..5]");
    quint32 level = 3;
    if (args.size() == 3 && (args[1] != "--level" || !number(args[2], level) ||
                             level < 1 || level > 5))
      return fail("--level must be 1..5");
    out.text = "play " + args[0] + " --level " + QString::number(level);
  } else if (action == "set" || action == "save") {
    QString prefix = action;
    if (action == "save") {
      if (args.isEmpty() || !name(args.first()))
        return fail("save requires NAME (1..15 ASCII letters, digits, _ or -)");
      prefix += ' ' + args.takeFirst();
    }
    if (args.isEmpty())
      return fail(action + " requires COLOR");
    QString mainColor = args.takeFirst(), altColor;
    if (!color(mainColor))
      return fail("Invalid color; use #RRGGBB, RRGGBB or a color name");
    quint32 timing[3] = {1000, 0, 0}, level = 3;
    QSet<QString> seen;
    while (!args.isEmpty()) {
      QString option = args.takeFirst();
      if (seen.contains(option))
        return fail("Duplicate option: " + option);
      seen.insert(option);
      if (args.isEmpty())
        return fail(option + " requires a value");
      QString value = args.takeFirst();
      if (option == "--altcolor") {
        if (!color(value))
          return fail("Invalid --altcolor");
        altColor = value;
      } else if (option == "--level") {
        if (!number(value, level) || level < 1 || level > 5)
          return fail("--level must be 1..5");
      } else if (option == "--on-ms" || option == "--off-ms" ||
                 option == "--duration-ms") {
        int index = option == "--on-ms" ? 0 : option == "--off-ms" ? 1 : 2;
        if (!number(value, timing[index]) || (index == 0 && timing[index] == 0))
          return fail(
              option +
              " must be an unsigned 32-bit integer; --on-ms must be positive");
      } else
        return fail("Unknown effect option: " + option);
    }
    out.text = prefix + ' ' + mainColor +
               QString(" %1 %2 %3 --level %4")
                   .arg(timing[0])
                   .arg(timing[1])
                   .arg(timing[2])
                   .arg(level);
    if (!altColor.isEmpty())
      out.text += " --altcolor " + altColor;
  } else
    return fail("Unknown command: " + action + "; use --help");
  if (out.text.isEmpty() || out.text.toUtf8().size() >= 256 ||
      out.text.contains(QChar(0)))
    return fail("Command must contain 1..255 UTF-8 bytes without NUL");
  return true;
}

QString usage() {
  return QStringLiteral(
      "vibeled - control Vibe Remote Buddy lights over USB CDC\n\n"
      "vibeled                                Open the Qt interface\n"
      "vibeled --help | -h | help | --version\n"
      "vibeled [--device PORT_OR_SERIAL_OR_NAME] [--timeout SEC] COMMAND\n\n"
      "  scan [--all]\n"
      "  set COLOR [--altcolor COLOR] [--on-ms N] [--off-ms N] [--duration-ms "
      "N] [--level 1..5]\n"
      "  save NAME COLOR [same options as set]\n"
      "  play NAME [--level 1..5]\n"
      "  delete NAME\n"
      "  list | status | off | getip\n"
      "  raw COMMAND...\n"
      "  wifi SSID PASSWORD   Recognized for compatibility; USB firmware does "
      "not provide Wi-Fi.\n\n"
      "Defaults: --on-ms 1000, --off-ms 0, --duration-ms 0, --level 3, "
      "--timeout 15.\n"
      "COLOR: #RRGGBB, RRGGBB, "
      "black/white/red/green/blue/yellow/cyan/magenta/orange/purple.\n"
      "NAME: 1..15 ASCII letters, digits, _ or -. scan lists USB serial "
      "ports.\n"
      "The old XGAI_LED device name selects a uniquely attached Buddy "
      "receiver.\n"
      "CLI waits for the device reply and closes the port; no wireless "
      "connection is used.\n");
}
