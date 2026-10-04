#pragma once
#include <QStringList>

struct Command {
  QString device;
  int timeoutMs = 15000;
  QString text;
  enum Kind { Gui, Help, Version, Scan, Light, Wifi } kind = Gui;
  bool all = false;
};
bool parseCommand(QStringList args, Command &command, QString &error);
QString usage();
struct Result {
  int code = 0;
  QString text;
};
