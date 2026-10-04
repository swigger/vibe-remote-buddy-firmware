#pragma once
#include "cli.h"
#include "rbp/frame.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <functional>

bool isBuddy(const QSerialPortInfo &port);
QList<QSerialPortInfo> devices(bool all = false);
Result execute(const Command &command);
QString relayName();
// Returns false only when no GUI accepted the connection. Never retries an
// accepted command.
bool forwardToGui(const QStringList &args, int timeoutMs, Result &result);

class ResponseReader {
public:
  ResponseReader();
  void feed(const QByteArray &data, uint32_t now,
            const std::function<void(const rbp_rx_event_ctx_t &)> &callback);

private:
  rbp_rxparser_t parser{};
};
