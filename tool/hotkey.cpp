#include "hotkey.h"
#include <QCoreApplication>
#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <Carbon/Carbon.h>
#endif

Hotkey::Hotkey() {
  QCoreApplication::instance()->installNativeEventFilter(this);
#ifdef Q_OS_MACOS
  EventTypeSpec type{kEventClassKeyboard, kEventHotKeyPressed};
  EventHandlerRef ref = nullptr;
  InstallApplicationEventHandler(
      [](EventHandlerCallRef, EventRef event, void *user) -> OSStatus {
        EventHotKeyID key{};
        if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                              nullptr, sizeof key, nullptr, &key) == noErr &&
            key.signature == 0x564c4544) {
          auto self = static_cast<Hotkey *>(user);
          if (self->activated)
            self->activated();
          return noErr;
        }
        return eventNotHandledErr;
      },
      1, &type, this, &ref);
  handler = ref;
#endif
}
Hotkey::~Hotkey() {
  QCoreApplication::instance()->removeNativeEventFilter(this);
#ifdef Q_OS_WIN
  if (id)
    UnregisterHotKey(nullptr, id);
#elif defined(Q_OS_MACOS)
  if (native)
    UnregisterEventHotKey(static_cast<EventHotKeyRef>(native));
  if (handler)
    RemoveEventHandler(static_cast<EventHandlerRef>(handler));
#endif
}
bool Hotkey::set(const QKeySequence &sequence) {
  if (sequence.count() != 1)
    return false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  int combo = sequence[0].toCombined();
#else
  int combo = sequence[0];
#endif
  int key = combo & ~int(Qt::KeyboardModifierMask);
  if (!(combo & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
    return false;
#ifdef Q_OS_WIN
  unsigned mods = MOD_NOREPEAT;
  if (combo & Qt::ControlModifier)
    mods |= MOD_CONTROL;
  if (combo & Qt::AltModifier)
    mods |= MOD_ALT;
  if (combo & Qt::MetaModifier)
    mods |= MOD_WIN;
  if (combo & Qt::ShiftModifier)
    mods |= MOD_SHIFT;
  unsigned code = (key >= Qt::Key_A && key <= Qt::Key_Z) ||
                          (key >= Qt::Key_0 && key <= Qt::Key_9)
                      ? unsigned(key)
                      : 0;
  if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
    code = VK_F1 + key - Qt::Key_F1;
  if (!code)
    return false;
  int next = id == 1 ? 2 : 1;
  if (!RegisterHotKey(nullptr, next, mods, code))
    return false;
  if (id)
    UnregisterHotKey(nullptr, id);
  id = next;
  return true;
#elif defined(Q_OS_MACOS)
  UInt32 mods = 0;
  // Qt maps Control to Command and Meta to Control on macOS by default.
  if (combo & Qt::ControlModifier)
    mods |= cmdKey;
  if (combo & Qt::MetaModifier)
    mods |= controlKey;
  if (combo & Qt::AltModifier)
    mods |= optionKey;
  if (combo & Qt::ShiftModifier)
    mods |= shiftKey;
  static const UInt32 letters[] = {
      kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F,
      kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L,
      kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R,
      kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X,
      kVK_ANSI_Y, kVK_ANSI_Z};
  static const UInt32 digits[] = {
      kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
      kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9};
  static const UInt32 functions[] = {kVK_F1, kVK_F2,  kVK_F3,  kVK_F4,
                                     kVK_F5, kVK_F6,  kVK_F7,  kVK_F8,
                                     kVK_F9, kVK_F10, kVK_F11, kVK_F12};
  UInt32 code;
  if (key >= Qt::Key_A && key <= Qt::Key_Z)
    code = letters[key - Qt::Key_A];
  else if (key >= Qt::Key_0 && key <= Qt::Key_9)
    code = digits[key - Qt::Key_0];
  else if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
    code = functions[key - Qt::Key_F1];
  else
    return false;
  EventHotKeyRef next = nullptr;
  if (!handler ||
      RegisterEventHotKey(code, mods, {0x564c4544, 1},
                          GetApplicationEventTarget(), 0, &next) != noErr)
    return false;
  if (native)
    UnregisterEventHotKey(static_cast<EventHotKeyRef>(native));
  native = next;
  return true;
#else
  return false;
#endif
}
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool Hotkey::nativeEventFilter(const QByteArray &, void *message, qintptr *) {
#else
bool Hotkey::nativeEventFilter(const QByteArray &, void *message, long *) {
#endif
#ifdef Q_OS_WIN
  auto msg = static_cast<MSG *>(message);
  if (msg->message == WM_HOTKEY && int(msg->wParam) == id) {
    if (activated)
      activated();
    return true;
  }
#else
  Q_UNUSED(message)
#endif
  return false;
}
