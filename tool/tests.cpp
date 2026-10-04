#include "cli.h"
#include "led_engine.h"
#include "transport.h"
#include <QtTest>
#include <cstring>

namespace {
QByteArray saved;
bool failSave;
bool load(char *text, size_t n) {
  if (saved.isEmpty() || size_t(saved.size()) >= n)
    return false;
  std::memcpy(text, saved.constData(), size_t(saved.size() + 1));
  return true;
}
bool save(const char *text) {
  if (failSave)
    return false;
  saved = text;
  return true;
}
QString command(const char *text, uint64_t now = 0, bool allowSave = true) {
  char response[384]{};
  vibeled::execute_command(text, response, sizeof response, now, allowSave);
  return QString::fromLatin1(response);
}
QByteArray frame(uint32_t requestId = 7) {
  rbp_header_t h{};
  h.kind = RBP_KIND_RESPONSE;
  h.request_id = requestId;
  h.opcode = 0x470;
  rbp_txseq_t seq{1};
  uint8_t bytes[RBP_MAX_ENCODED];
  const QByteArray payload = "{\"text\":\"ok off\"}";
  size_t n = rbp_frame_encode(
      &h, &seq, reinterpret_cast<const uint8_t *>(payload.constData()),
      size_t(payload.size()), bytes);
  return QByteArray(reinterpret_cast<char *>(bytes), int(n));
}
} // namespace

class Tests : public QObject {
  Q_OBJECT
private slots:
  void init() {
    saved.clear();
    failSave = false;
    vibeled::init({load, save});
  }
  void legacyCli() {
    Command c;
    QString error;
    QVERIFY(parseCommand({"--device", "XGAI_LED", "--timeout", "0.25", "set",
                          "orange", "--altcolor", "blue", "--on-ms", "200",
                          "--off-ms", "800", "--duration-ms", "10000",
                          "--level", "4"},
                         c, error));
    QCOMPARE(c.device, QString("XGAI_LED"));
    QCOMPARE(c.timeoutMs, 250);
    QCOMPARE(c.text,
             QString("set orange 200 800 10000 --level 4 --altcolor blue"));
    QVERIFY(parseCommand(
        {"save", "deploy-ok", "00CC44", "--altcolor", "0033FF", "--level", "2"},
        c, error));
    QVERIFY(command(c.text.toUtf8().constData()).startsWith("ok saved"));
    QCOMPARE(vibeled::tick(0).rgb, uint32_t(0x00cc44));
    QVERIFY(parseCommand({"raw", "XGAI_LED:", "heartbeat", "--level", "5"}, c,
                         error));
    QVERIFY(command(c.text.toUtf8().constData()).startsWith("ok"));
    QCOMPARE(vibeled::tick(0).rgb, uint32_t(0x770000));
  }
  void invalidCli_data() {
    QTest::addColumn<QStringList>("args");
    QTest::newRow("overflow")
        << QStringList{"set", "red", "--on-ms", "4294967296"};
    QTest::newRow("negative") << QStringList{"set", "red", "--off-ms", "-1"};
    QTest::newRow("zero") << QStringList{"set", "red", "--on-ms", "0"};
    QTest::newRow("level") << QStringList{"play", "blue", "--level", "6"};
    QTest::newRow("duplicate")
        << QStringList{"set", "red", "--level", "3", "--level", "4"};
    QTest::newRow("name") << QStringList{"save", "too-long-preset-name", "red"};
    QTest::newRow("color") << QStringList{"set", "#12345g"};
    QTest::newRow("nan") << QStringList{"--timeout", "nan", "off"};
    QTest::newRow("extra") << QStringList{"off", "extra"};
    QTest::newRow("empty") << QStringList{"raw", ""};
  }
  void invalidCli() {
    QFETCH(QStringList, args);
    Command c;
    QString e;
    QVERIFY(!parseCommand(args, c, e));
    QVERIFY(!e.isEmpty());
  }
  void priorityAndPhases() {
    command("set green 100 100 1000 --level 1", 0);
    command("set red 50 50 250 --altcolor blue --level 5", 20);
    QCOMPARE(vibeled::tick(20).rgb, uint32_t(0xff0000));
    QCOMPARE(vibeled::tick(70).rgb, uint32_t(0xff));
    QCOMPARE(vibeled::tick(249).rgb, uint32_t(0xff0000));
    // A delayed task uses original phase, without accumulating drift.
    QCOMPARE(vibeled::tick(270).rgb, uint32_t(0x00ff00));
    QCOMPARE(vibeled::tick(350).rgb, uint32_t(0));
    QCOMPARE(vibeled::tick(400).rgb, uint32_t(0x00ff00));
    command("set black --level 1", 401);
    QCOMPARE(command("status", 401), QString("ok off"));
    command("set black 10 10 --altcolor blue --level 5", 402);
    QCOMPARE(vibeled::tick(402).rgb, uint32_t(0));
    QCOMPARE(vibeled::tick(412).rgb, uint32_t(255));
    command("off", 413);
    QCOMPARE(vibeled::tick(414).wait_ms, UINT32_MAX);
  }
  void longTiming() {
    QVERIFY(command("set red 100000 0 4294967295").startsWith("ok"));
    QVERIFY(vibeled::tick(0).wait_ms != UINT32_MAX);
    QVERIFY(command("status", 100001).contains("on=100000"));
    QCOMPARE(vibeled::tick(UINT32_MAX - 1ULL).rgb, uint32_t(0xff0000));
    QCOMPARE(vibeled::tick(UINT32_MAX).rgb, uint32_t(0));
    QVERIFY(command("set red 4294967296").startsWith("error"));
    QVERIFY(command("set +12345").startsWith("error"));
  }
  void presets() {
    QVERIFY(command("save deploy red 25 50 200 --altcolor blue --level 2")
                .startsWith("ok saved"));
    auto storage = saved;
    vibeled::init({load, save});
    QVERIFY(command("play deploy --level 5").contains("level=5"));
    QCOMPARE(vibeled::tick(25).rgb, uint32_t(0xff));
    failSave = true;
    QVERIFY(command("save deploy green").startsWith("error"));
    QCOMPARE(saved, storage);
    QVERIFY(command("play deploy").contains("color=#ff0000"));
    QVERIFY(command("delete deploy").startsWith("error"));
    QVERIFY(command("list").contains("saved=deploy"));
    failSave = false;
    for (int i = 0; i < 7; i++)
      QVERIFY(command(QString("save p%1 green").arg(i).toLatin1().constData())
                  .startsWith("ok saved"));
    QVERIFY(command("save ninth green").contains("limit"));
    QVERIFY(command("save p0 blue").startsWith("ok saved"));
    QVERIFY(command("delete deploy").startsWith("ok deleted"));
    QVERIFY(command("save ninth green").startsWith("ok saved"));
    QVERIFY(command("save busy red", 0, false).contains("busy"));
    QVERIFY(command("set red", 0, false).startsWith("ok"));
    QCOMPARE(command("getip"), QString("no ip"));
  }
  void frames() {
    ResponseReader reader;
    int count = 0;
    auto receive = [&](const rbp_rx_event_ctx_t &e) {
      count++;
      QCOMPARE(e.header.opcode, uint16_t(0x470));
    };
    QByteArray good = frame();
    for (char byte : good)
      reader.feed(QByteArray(1, byte), 0, receive);
    QCOMPARE(count, 1);
    QByteArray bad = good;
    bad[bad.size() - 3] = char(bad[bad.size() - 3] ^ 0x04);
    reader.feed(bad, 1, receive);
    QCOMPARE(count, 1);
    reader.feed(good + good, 2, receive);
    QCOMPARE(count, 3);
    reader.feed(good.left(5), 3, receive);
    reader.feed(good.mid(5), 1004, receive);
    QCOMPARE(count, 3);
    reader.feed(good, 1005, receive);
    QCOMPARE(count, 4);
  }
};
QTEST_GUILESS_MAIN(Tests)
#include "tests.moc"
