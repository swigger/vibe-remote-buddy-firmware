#pragma once
#include <QAbstractNativeEventFilter>
#include <QKeySequence>
#include <functional>

class Hotkey : public QAbstractNativeEventFilter {
public:
  Hotkey();
  ~Hotkey() override;
  bool set(const QKeySequence &sequence);
  std::function<void()> activated;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  bool nativeEventFilter(const QByteArray &, void *, qintptr *) override;
#else
  bool nativeEventFilter(const QByteArray &, void *, long *) override;
#endif
private:
  void *native = nullptr;
  void *handler = nullptr;
  int id = 0;
};
