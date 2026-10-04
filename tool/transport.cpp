#include "transport.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>
#include <stdexcept>

bool isBuddy(const QSerialPortInfo &p) {
  return (p.vendorIdentifier() == 0x05ac && p.productIdentifier() == 0x0220) ||
         (p.vendorIdentifier() == 0xcafe && p.productIdentifier() == 0x4016);
}
QList<QSerialPortInfo> devices(bool all) {
  QList<QSerialPortInfo> result;
  for (const auto &p : QSerialPortInfo::availablePorts()) {
#ifdef Q_OS_MACOS
    // macOS enumerates call-in and call-out names for the same CDC interface.
    if (p.systemLocation().startsWith("/dev/tty."))
      continue;
#endif
    if (all || isBuddy(p))
      result.append(p);
  }
  return result;
}
ResponseReader::ResponseReader() { rbp_rxparser_init(&parser, 0); }
void ResponseReader::feed(
    const QByteArray &data, uint32_t now,
    const std::function<void(const rbp_rx_event_ctx_t &)> &callback) {
  rbp_rx_event_ctx_t e{};
  auto handler = [](rbp_rx_event_ctx_t *event, void *user) {
    if (event->event != RBP_RX_FRAME || !event->crc_ok ||
        event->header.payload_size != event->payload_len ||
        event->header.major != RBP_MAJOR || event->header.minor != RBP_MINOR ||
        event->header.flags || event->header.connection_id)
      return;
    (*static_cast<const std::function<void(const rbp_rx_event_ctx_t &)> *>(
        user))(*event);
  };
  rbp_rx_drain(&parser, reinterpret_cast<const uint8_t *>(data.constData()),
               size_t(data.size()), now, &e, handler,
               const_cast<void *>(static_cast<const void *>(&callback)));
}

namespace {
[[noreturn]] void fail(const QString &s) {
  throw std::runtime_error(s.toUtf8().constData());
}
QString digest(const QString &s) {
  return QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256)
      .toHex()
      .left(24);
}
QString lockDirectory() {
  QString path =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
      "/vibeled-" + digest(QDir::homePath());
  QDir().mkpath(path);
  return path;
}
class Connection {
public:
  QSerialPort port;
  QElapsedTimer clock;
  int timeout;
  uint32_t session = 0, requestId = 0;
  rbp_txseq_t sequence{1};
  ResponseReader reader;
  explicit Connection(int timeoutMs) : timeout(timeoutMs) { clock.start(); }
  int remaining() const { return qMax(0, timeout - int(clock.elapsed())); }
  QJsonObject request(uint16_t opcode, const QJsonObject &object,
                      int limit = -1) {
    QElapsedTimer step;
    step.start();
    auto budget = [&] {
      return limit < 0
                 ? remaining()
                 : qMax(0, qMin(remaining(), limit - int(step.elapsed())));
    };
    QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    uint8_t encoded[RBP_MAX_ENCODED];
    rbp_header_t h{};
    h.kind = RBP_KIND_REQUEST;
    h.session_id = session;
    h.request_id = ++requestId;
    h.opcode = opcode;
    size_t n = rbp_frame_encode(
        &h, &sequence, reinterpret_cast<const uint8_t *>(body.constData()),
        size_t(body.size()), encoded);
    if (!n || port.write(reinterpret_cast<const char *>(encoded), qint64(n)) !=
                  qint64(n))
      fail("USB write failed: " + port.errorString());
    while (port.bytesToWrite())
      if (!budget() || !port.waitForBytesWritten(budget()))
        fail("USB write timeout: " + port.errorString());
    bool found = false;
    uint16_t status = 0;
    QJsonObject reply;
    while (budget() > 0 && !found) {
      if (!port.bytesAvailable() &&
          !port.waitForReadyRead(qMin(100, budget()))) {
        if (port.error() != QSerialPort::NoError &&
            port.error() != QSerialPort::TimeoutError)
          fail("USB disconnected: " + port.errorString());
        continue;
      }
      reader.feed(
          port.readAll(), uint32_t(clock.elapsed()),
          [&](const rbp_rx_event_ctx_t &e) {
            if (e.header.kind != RBP_KIND_RESPONSE ||
                e.header.request_id != requestId || e.header.opcode != opcode ||
                (session && e.header.session_id != session))
              return;
            QJsonParseError error;
            auto json = QJsonDocument::fromJson(
                QByteArray(reinterpret_cast<const char *>(e.payload),
                           e.payload_len),
                &error);
            if (error.error != QJsonParseError::NoError || !json.isObject())
              return;
            if (opcode == 0x400 && !e.header.status)
              session = e.header.session_id;
            reply = json.object();
            status = e.header.status;
            found = true;
          });
    }
    if (!found)
      fail("Device response timed out; close other Buddy management sessions "
           "and retry");
    if (status)
      fail(QString("Device rejected command (RBP status %1)").arg(status));
    return reply;
  }
  ~Connection() {
    if (session && port.isOpen()) {
      // Leave pairing/autonomous operation immediately, even after an error.
      timeout = int(clock.elapsed()) + 300;
      try {
        request(0x402, {}, 300);
      } catch (...) {
      }
    }
    port.close();
  }
};
} // namespace

Result execute(const Command &command) {
  if (command.kind == Command::Help)
    return {0, usage()};
  if (command.kind == Command::Version)
    return {0, "vibeled 1.0.0"};
  if (command.kind == Command::Wifi)
    return {2, "error wifi is unavailable: vibeled uses USB CDC; Wi-Fi "
               "provisioning is no longer required"};
  if (command.kind == Command::Scan) {
    QStringList lines;
    for (const auto &p : devices(command.all))
      lines.append(QString("%1\t%2\t%3\t%4")
                       .arg(isBuddy(p) ? "vibeled" : p.description(),
                            p.systemLocation(), p.serialNumber(),
                            p.description()));
    return {0, lines.join('\n')};
  }
  try {
    auto available = devices(true);
    QList<QSerialPortInfo> matches;
    bool alias = command.device.isEmpty() ||
                 command.device.compare("XGAI_LED", Qt::CaseInsensitive) == 0 ||
                 command.device.compare("vibeled", Qt::CaseInsensitive) == 0;
    for (const auto &p : available) {
      auto same = [&](const QString &s) {
        return s.compare(command.device, Qt::CaseInsensitive) == 0;
      };
      if ((alias && isBuddy(p)) ||
          (!alias && (same(p.portName()) || same(p.systemLocation()) ||
                      same(p.serialNumber()) || same(p.description()))))
        matches.append(p);
    }
    if (matches.isEmpty())
      return {1, "No matching USB CDC receiver. Connect the ESP32 and run "
                 "vibeled scan."};
    if (matches.size() != 1)
      return {1, "Multiple receivers match. Use --device with a port or serial "
                 "number from scan."};
    QLockFile lock(lockDirectory() + '/' + digest(matches[0].systemLocation()) +
                   ".lock");
    Connection connection(command.timeoutMs);
    // Serialized CLI and hook processes; stale locks are checked by PID, not
    // elapsed time.
    lock.setStaleLockTime(0);
    while (!lock.tryLock(0)) {
      if (lock.error() != QLockFile::LockFailedError)
        return {1, "Cannot create the device lock file"};
      if (!connection.remaining())
        return {1, "Timed out waiting for another vibeled command"};
      QThread::msleep(10);
    }
    connection.port.setPort(matches[0]);
    connection.port.setBaudRate(115200);
    connection.port.setFlowControl(QSerialPort::NoFlowControl);
    while (!connection.port.open(QIODevice::ReadWrite)) {
      if (connection.remaining() < 100)
        return {1, "Cannot open " + matches[0].portName() + ": " +
                       connection.port.errorString() +
                       ". Close the Buddy App or serial monitor."};
      QThread::msleep(50);
    }
    if (!connection.port.setDataTerminalReady(true))
      fail("Cannot assert CDC DTR: " + connection.port.errorString());
    QThread::msleep(30); // let the firmware consume the DTR transition
    connection.port.clear();
    connection.port.write(
        QByteArray(1, '\0')); // recover a partial previous record
    connection.request(0x400, {{"api", 1}});
    auto info = connection.request(0x403, {});
    if (info.value("led_api").toInt() != 1)
      fail(
          "Receiver firmware lacks LED support; install Buddy 0.13.0 or later");
    auto reply = connection.request(0x470, {{"command", command.text}});
    if (!reply.value("text").isString())
      fail("Invalid LED response from receiver");
    QString text = reply.value("text").toString();
    // Explicit close before unlocking, so a second process cannot race session
    // teardown.
    connection.request(0x402, {});
    connection.session = 0;
    connection.port.close();
    return {text.startsWith("error", Qt::CaseInsensitive) ? 2 : 0, text};
  } catch (const std::exception &error) {
    return {1, QString::fromUtf8(error.what())};
  }
}

QString relayName() { return "vibeled-" + digest(QDir::homePath()); }
bool forwardToGui(const QStringList &args, int timeoutMs, Result &result) {
  QLocalSocket socket;
  socket.connectToServer(relayName());
  if (!socket.waitForConnected(100))
    return false;
  socket.write(QJsonDocument(QJsonArray::fromStringList(args))
                   .toJson(QJsonDocument::Compact) +
               '\n');
  QElapsedTimer timer;
  timer.start();
  QByteArray buffer;
  while (timer.elapsed() < timeoutMs + qint64(1000)) {
    if (socket.bytesToWrite())
      socket.waitForBytesWritten(100);
    if (!socket.bytesAvailable())
      socket.waitForReadyRead(100);
    buffer += socket.readAll();
    if (buffer.size() > 65536)
      break;
    if (buffer.contains('\n')) {
      auto doc = QJsonDocument::fromJson(buffer.left(buffer.indexOf('\n')));
      if (!doc.isObject() || !doc.object().value("code").isDouble() ||
          !doc.object().value("text").isString())
        break;
      result = {doc.object().value("code").toInt(),
                doc.object().value("text").toString()};
      return true;
    }
    if (socket.state() == QLocalSocket::UnconnectedState)
      break;
  }
  result = {1, "GUI command response unavailable; command was not retried"};
  return true;
}
