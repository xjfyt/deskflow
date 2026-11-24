/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "X11Input.h"

#include "enhanced/InputSession.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <memory>
#include <set>
#include <unistd.h>

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

namespace deskflow::enhanced {
namespace {

constexpr auto NoWindow = None;
#undef None

using Clock = InputExchange::Clock;
using namespace std::chrono_literals;

constexpr int EdgePadding = 12;
constexpr int TakeoverDistance = 8;
constexpr int NativeEventBudget = 384;
constexpr std::array<unsigned int, 5> XButtons{1, 3, 2, 8, 9};
volatile std::sig_atomic_t interrupted = 0;
bool protocolError = false;

void handleSignal(int)
{
  interrupted = 1;
}

int handleProtocolError(Display *, XErrorEvent *)
{
  protocolError = true;
  return 0;
}

void status(const char *message)
{
  QTextStream(stdout) << message << Qt::endl;
}

class X11DesktopAccess : public QObject
{
public:
  explicit X11DesktopAccess(QString display)
      : m_display(display.section(u'.', 0, 0)),
        m_bus(QDBusConnection::systemBus())
  {
    QObject::connect(&m_timer, &QTimer::timeout, this, [this] { refresh(); });
    m_timer.start(150);
    refresh();
  }

  bool usable() const
  {
    return Clock::now() - m_verifiedAt <= std::chrono::milliseconds(InputWatchdog);
  }

private:
  void refresh()
  {
    if (m_pending)
      return;
    auto call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1/session/auto"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll")
    );
    call << QStringLiteral("org.freedesktop.login1.Session");
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, 400), this);
    m_pending = true;
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *finished) {
      const QDBusPendingReply<QVariantMap> reply(*finished);
      finished->deleteLater();
      m_pending = false;
      m_verifiedAt = {};
      if (reply.isError())
        return;
      const auto properties = reply.value();
      const auto display = properties.value(QStringLiteral("Display")).toString();
      const auto locked = properties.value(QStringLiteral("LockedHint"));
      if (!display.isEmpty() && display.section(u'.', 0, 0) == m_display &&
          properties.value(QStringLiteral("Type")).toString() == QStringLiteral("x11") &&
          properties.value(QStringLiteral("Class")).toString() == QStringLiteral("user") &&
          properties.value(QStringLiteral("Active")).toBool() && properties.contains(QStringLiteral("Remote")) &&
          !properties.value(QStringLiteral("Remote")).toBool() && locked.isValid() && !locked.toBool()) {
        m_verifiedAt = Clock::now();
      }
    });
  }

  const QString m_display;
  const QDBusConnection m_bus;
  QTimer m_timer;
  Clock::time_point m_verifiedAt{};
  bool m_pending = false;
};

struct Point
{
  int x;
  int y;
};

Point relativeMotion(const XIRawEvent &event)
{
  Point result{};
  const auto *value = event.raw_values;
  if (!value || !event.valuators.mask || event.valuators.mask_len <= 0)
    return result;
  if (XIMaskIsSet(event.valuators.mask, 0)) {
    if (std::isfinite(*value))
      result.x = static_cast<int>(std::lround(std::clamp(*value, -8192.0, 8192.0)));
    ++value;
  }
  if (XIMaskIsSet(event.valuators.mask, 1) && std::isfinite(*value))
    result.y = static_cast<int>(std::lround(std::clamp(*value, -8192.0, 8192.0)));
  return result;
}

class X11Input
{
public:
  X11Input(
      Display *display, InputExchange &exchange, const InputSessionOptions &options, const X11DesktopAccess &access
  )
      : m_display(display),
        m_screen(DefaultScreen(display)),
        m_root(RootWindow(display, m_screen)),
        m_access(access),
        m_exchange(exchange),
        m_side(options.listener ? InputSide::Listener : InputSide::Connector),
        m_rightEdge(options.rightEdge)
  {
  }

  ~X11Input()
  {
    stopLease();
  }

  bool start()
  {
    int event = 0;
    int error = 0;
    int major = 2;
    int minor = 2; // libXi supplies the raw source device ID to XI 2.2 clients.
    int opcode = 0;
    if (XQueryExtension(m_display, "XWAYLAND", &opcode, &event, &error) ||
        !XQueryExtension(m_display, "XInputExtension", &m_xiOpcode, &event, &error) ||
        XIQueryVersion(m_display, &major, &minor) != Success || major < 2 || (major == 2 && minor < 2) ||
        !XTestQueryExtension(m_display, &event, &error, &major, &minor) || !refreshDevices()) {
      return false;
    }
    std::array<unsigned char, XIMaskLen(XI_RawMotion)> raw{};
    for (const auto type : {XI_RawMotion, XI_RawButtonPress, XI_RawButtonRelease, XI_RawKeyPress, XI_RawKeyRelease})
      XISetMask(raw.data(), type);
    std::array<unsigned char, XIMaskLen(XI_HierarchyChanged)> hierarchy{};
    XISetMask(hierarchy.data(), XI_HierarchyChanged);
    XIEventMask masks[] = {
        {XIAllMasterDevices, static_cast<int>(raw.size()), raw.data()},
        {XIAllDevices, static_cast<int>(hierarchy.size()), hierarchy.data()}
    };
    XISelectEvents(m_display, m_root, masks, 2);
    XSync(m_display, False);
    return !protocolError && refreshGeometry();
  }

  void tick()
  {
    const auto now = Clock::now();
    const auto [state, lastUpdate] = m_exchange.takeInbound();
    if (protocolError) {
      finish(QStringLiteral("The local X server rejected an enhanced input operation."));
      return;
    }
    if (m_connected && now - lastUpdate > std::chrono::milliseconds(InputWatchdog)) {
      finish(QStringLiteral("The enhanced X11 peer stopped responding."));
      return;
    }
    if (interrupted || m_exchange.cancelled.load(std::memory_order_relaxed)) {
      finish();
      return;
    }
    if (!m_access.usable()) {
      if (m_connected || now - m_startedAt >= 5s) {
        finish(QStringLiteral("The X11 session is locked, inactive, or its status could not be verified with logind."));
      }
      return;
    }
    if (now >= m_nextGeometry) {
      m_nextGeometry = now + 200ms;
      if (!refreshGeometry()) {
        finish(QStringLiteral("Could not read the local X11 screen geometry."));
        return;
      }
    }
    readEvents();
    if (m_exchange.cancelled.load(std::memory_order_relaxed))
      return;
    if (m_towardEdge) {
      m_towardEdge = false;
      requestAtEdge(now);
    }
    if (lastUpdate == Clock::time_point{})
      return;
    if (!m_connected) {
      m_connected = true;
      status("Enhanced X11 input paired. Move across the selected edge to control the other X11 computer.");
    }
    if (state.epoch != m_epoch || state.owner != m_owner) {
      stopLease();
      m_epoch = state.epoch;
      m_owner = state.owner;
      m_active = false;
      m_blocked = false;
      m_readied = false;
      m_takeoverDistance = 0;
      if (m_owner == InputSide::None)
        status("Local input restored.");
    }
    if (m_owner != InputSide::None && !m_blocked && !m_active) {
      if (localInputDown() || (m_owner == m_side && now > m_requestUntil)) {
        revoke();
      } else if (state.active) {
        m_active = true;
        if (!activate(state.verticalPosition))
          revoke();
      } else if (m_owner != m_side && !m_readied) {
        m_exchange.ready(m_epoch);
        m_readied = true;
      }
    }
    if (receiving()) {
      for (const auto &event : state.events) {
        if (!apply(event)) {
          revoke();
          break;
        }
      }
      XFlush(m_display);
    }
  }

  const QString &error() const
  {
    return m_error;
  }

private:
  bool refreshDevices()
  {
    // A core query assigns a ClientPointer to a new X connection before XIGetClientPointer.
    Window root = NoWindow;
    Window child = NoWindow;
    int rootX = 0;
    int rootY = 0;
    int windowX = 0;
    int windowY = 0;
    unsigned int mask = 0;
    XQueryPointer(m_display, m_root, &root, &child, &rootX, &rootY, &windowX, &windowY, &mask);
    int count = 0;
    using Info = std::unique_ptr<XIDeviceInfo, decltype(&XIFreeDeviceInfo)>;
    const Info devices(XIQueryDevice(m_display, XIAllDevices, &count), XIFreeDeviceInfo);
    if (!devices || !XIGetClientPointer(m_display, NoWindow, &m_pointer))
      return false;
    const auto *first = devices.get();
    const auto *last = first + count;
    const auto *master = std::find_if(first, last, [this](const XIDeviceInfo &device) {
      return device.deviceid == m_pointer && device.use == XIMasterPointer && device.enabled;
    });
    if (master == last)
      return false;
    m_keyboard = master->attachment;
    m_sources.clear();
    m_relativeSources.clear();
    bool testPointer = false;
    bool testKeyboard = false;
    for (auto *device = first; device != last; ++device) {
      const bool pointer = device->use == XISlavePointer && device->attachment == m_pointer;
      const bool keyboard = device->use == XISlaveKeyboard && device->attachment == m_keyboard;
      if ((!pointer && !keyboard) || !device->enabled)
        continue;
      if (QByteArrayView(device->name).contains(" XTEST ")) {
        testPointer = testPointer || pointer;
        testKeyboard = testKeyboard || keyboard;
      } else {
        m_sources.insert(device->deviceid);
        if (pointer && relativePointer(*device))
          m_relativeSources.insert(device->deviceid);
      }
    }
    if (!testPointer || !testKeyboard || m_relativeSources.empty())
      return false;
    m_keys.fill(false);
    return true;
  }

  static bool relativePointer(const XIDeviceInfo &device)
  {
    int relativeAxes = 0;
    for (int index = 0; index < device.num_classes; ++index) {
      if (device.classes[index]->type != XIValuatorClass)
        continue;
      const auto *axis = reinterpret_cast<const XIValuatorClassInfo *>(device.classes[index]);
      if (axis->mode == XIModeRelative && axis->number >= 0 && axis->number < 2)
        relativeAxes |= 1 << axis->number;
    }
    return relativeAxes == 3;
  }

  bool refreshGeometry()
  {
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(m_display, m_root, &attributes) || attributes.width < 2 * EdgePadding ||
        attributes.height < 2) {
      return false;
    }
    if (m_owner != InputSide::None && (m_width != attributes.width || m_height != attributes.height))
      revoke();
    m_width = attributes.width;
    m_height = attributes.height;
    return true;
  }

  bool sending() const
  {
    return m_active && !m_blocked && m_owner == m_side;
  }

  bool receiving() const
  {
    return m_active && peerOwnsLease();
  }

  bool peerOwnsLease() const
  {
    return !m_blocked && m_owner != InputSide::None && m_owner != m_side;
  }

  int edge() const
  {
    return m_rightEdge ? m_width - 1 : 0;
  }

  bool readPointer(Point &point, bool *buttons = nullptr) const
  {
    Window root = NoWindow;
    Window child = NoWindow;
    double rootX = 0;
    double rootY = 0;
    double winX = 0;
    double winY = 0;
    XIButtonState state{};
    XIModifierState modifiers{};
    XIGroupState group{};
    const auto found = XIQueryPointer(
        m_display, m_pointer, m_root, &root, &child, &rootX, &rootY, &winX, &winY, &state, &modifiers, &group
    );
    if (buttons) {
      *buttons = false;
      for (const auto button : XButtons) {
        if (button / 8 < static_cast<unsigned int>(state.mask_len) && XIMaskIsSet(state.mask, button))
          *buttons = true;
      }
    }
    if (state.mask)
      XFree(state.mask);
    if (!found || root != m_root)
      return false;
    point = {static_cast<int>(rootX), static_cast<int>(rootY)};
    return true;
  }

  bool localInputDown() const
  {
    std::array<char, 32> keys{};
    XQueryKeymap(m_display, keys.data());
    if (std::ranges::any_of(keys, [](char value) { return value != 0; }))
      return true;
    Point point{};
    bool buttons = false;
    return !readPointer(point, &buttons) || buttons;
  }

  bool grab(int device)
  {
    std::array<unsigned char, 1> emptyMask{};
    XIEventMask mask{device, static_cast<int>(emptyMask.size()), emptyMask.data()};
    return XIGrabDevice(m_display, device, m_root, CurrentTime, NoWindow, GrabModeAsync, GrabModeAsync, False, &mask) ==
           GrabSuccess;
  }

  bool activate(quint16 verticalPosition)
  {
    if (sending()) {
      Point cursor{};
      if (!readPointer(cursor) || std::abs(cursor.x - edge()) > EdgePadding || !grab(m_keyboard))
        return false;
      if (!grab(m_pointer)) {
        XIUngrabDevice(m_display, m_keyboard, CurrentTime);
        XFlush(m_display);
        return false;
      }
      m_returnPoint = {m_rightEdge ? edge() - EdgePadding : EdgePadding, cursor.y};
      m_grabbed = true;
      status("Your physical X11 input is controlling the peer.");
    } else {
      m_playback = {
          m_rightEdge ? edge() - EdgePadding : EdgePadding,
          static_cast<int>(qint64(verticalPosition) * (m_height - 1) / 65535)
      };
      if (!XTestFakeMotionEvent(m_display, m_screen, m_playback.x, m_playback.y, CurrentTime))
        return false;
      XFlush(m_display);
      status("The peer is controlling this X11 display. Use local input to take over.");
    }
    return true;
  }

  void stopLease()
  {
    for (auto event = m_injected.crbegin(); event != m_injected.crend(); ++event) {
      if (event->kind == InputKind::Key)
        XTestFakeKeyEvent(m_display, event->first, False, CurrentTime);
      else
        XTestFakeButtonEvent(m_display, XButtons.at(static_cast<size_t>(event->first - 1)), False, CurrentTime);
    }
    m_injected.clear();
    if (m_grabbed) {
      XIUngrabDevice(m_display, m_pointer, CurrentTime);
      XIUngrabDevice(m_display, m_keyboard, CurrentTime);
      if (m_access.usable()) {
        XWarpPointer(
            m_display, NoWindow, m_root, 0, 0, 0, 0, std::clamp(m_returnPoint.x, 0, m_width - 1),
            std::clamp(m_returnPoint.y, 0, m_height - 1)
        );
      }
      m_grabbed = false;
    }
    XFlush(m_display);
  }

  void revoke()
  {
    if (!m_blocked) {
      m_blocked = true;
      stopLease();
      m_exchange.release(m_epoch);
    }
  }

  void finish(const QString &error = {})
  {
    m_error = error;
    m_blocked = true;
    m_exchange.cancelled.store(true, std::memory_order_relaxed);
    stopLease();
    QCoreApplication::quit();
  }

  void readEvents()
  {
    int remaining = NativeEventBudget;
    while (remaining-- > 0 && XPending(m_display) > 0) {
      XEvent event{};
      XNextEvent(m_display, &event);
      if (event.type != GenericEvent || event.xcookie.extension != m_xiOpcode ||
          !XGetEventData(m_display, &event.xcookie)) {
        continue;
      }
      if (event.xcookie.evtype == XI_HierarchyChanged) {
        if (m_owner != InputSide::None)
          revoke();
        if (!refreshDevices())
          finish(QStringLiteral("The local X11 input devices became unavailable."));
      } else {
        const auto *raw = static_cast<XIRawEvent *>(event.xcookie.data);
        if (raw && m_sources.contains(raw->sourceid) &&
            (raw->evtype != XI_RawMotion || m_relativeSources.contains(raw->sourceid))) {
          handlePhysical(*raw);
        }
      }
      XFreeEventData(m_display, &event.xcookie);
      if (m_exchange.cancelled.load(std::memory_order_relaxed))
        return;
    }
    if (m_owner != InputSide::None && remaining < 0 && XPending(m_display) > 0)
      revoke();
  }

  void handlePhysical(const XIRawEvent &event)
  {
    const bool key = event.evtype == XI_RawKeyPress || event.evtype == XI_RawKeyRelease;
    const bool down = event.evtype == XI_RawKeyPress || event.evtype == XI_RawButtonPress;
    const auto motion = event.evtype == XI_RawMotion ? relativeMotion(event) : Point{};
    if (key) {
      if (event.detail < 8 || event.detail > 255)
        return;
      m_keys[static_cast<size_t>(event.detail)] = down;
      if (down && static_cast<unsigned int>(event.detail) == XKeysymToKeycode(m_display, XK_F12) &&
          modifierDown(XK_Control_L, XK_Control_R) && modifierDown(XK_Alt_L, XK_Alt_R) &&
          modifierDown(XK_Shift_L, XK_Shift_R)) {
        finish();
        return;
      }
    }
    if (peerOwnsLease()) {
      const auto now = Clock::now();
      if (now >= m_takeoverUntil) {
        m_takeoverUntil = now + 250ms;
        m_takeoverDistance = 0;
      }
      m_takeoverDistance += std::abs(motion.x) + std::abs(motion.y);
      if (event.evtype != XI_RawMotion || m_takeoverDistance >= TakeoverDistance)
        revoke();
      return;
    }
    if (sending()) {
      forwardPhysical(event, motion, key, down);
      return;
    }
    if (m_connected && m_owner == InputSide::None && event.evtype == XI_RawMotion &&
        (m_rightEdge ? motion.x > 0 : motion.x < 0)) {
      m_towardEdge = true;
    }
  }

  void requestAtEdge(Clock::time_point now)
  {
    Point cursor{};
    if (now < m_nextRequest || !readPointer(cursor) || cursor.x != edge() || localInputDown())
      return;
    m_exchange.request(m_epoch, static_cast<quint16>(qint64(cursor.y) * 65535 / (m_height - 1)));
    m_nextRequest = now + 250ms;
    m_requestUntil = now + std::chrono::milliseconds(InputWatchdog);
  }

  bool modifierDown(KeySym left, KeySym right) const
  {
    return m_keys.at(XKeysymToKeycode(m_display, left)) || m_keys.at(XKeysymToKeycode(m_display, right));
  }

  void forwardPhysical(const XIRawEvent &event, Point motion, bool key, bool down)
  {
    if (key) {
      m_exchange.enqueue(m_epoch, {InputKind::Key, static_cast<qint16>(event.detail), down});
      return;
    }
    if (event.evtype == XI_RawMotion) {
      m_exchange.enqueue(m_epoch, {InputKind::Move, static_cast<qint16>(motion.x), static_cast<qint16>(motion.y)});
      return;
    }
    const auto button = static_cast<unsigned int>(event.detail);
    const auto found = std::find(XButtons.begin(), XButtons.end(), button);
    if (found != XButtons.end()) {
      m_exchange.enqueue(m_epoch, {InputKind::Button, static_cast<qint16>(found - XButtons.begin() + 1), down});
    } else if (down) {
      switch (button) {
      case 4:
        m_exchange.enqueue(m_epoch, {InputKind::Wheel, 0, 120});
        break;
      case 5:
        m_exchange.enqueue(m_epoch, {InputKind::Wheel, 0, -120});
        break;
      case 6:
        m_exchange.enqueue(m_epoch, {InputKind::Wheel, -120, 0});
        break;
      case 7:
        m_exchange.enqueue(m_epoch, {InputKind::Wheel, 120, 0});
        break;
      }
    }
  }

  bool apply(const InputEvent &event)
  {
    if (event.kind == InputKind::Move) {
      const Point next{m_playback.x + event.first, m_playback.y + event.second};
      const bool crossed = m_rightEdge ? event.first > 0 && next.x >= edge() : event.first < 0 && next.x <= edge();
      if (crossed && m_injected.isEmpty())
        return false;
      m_playback = {std::clamp(next.x, 0, m_width - 1), std::clamp(next.y, 0, m_height - 1)};
      return XTestFakeMotionEvent(m_display, m_screen, m_playback.x, m_playback.y, CurrentTime);
    }
    if (event.kind == InputKind::Wheel) {
      const auto amount = event.first == 0 ? event.second : event.first;
      if (std::abs(amount) != 120)
        return false;
      const unsigned int button = event.first == 0 ? (amount > 0 ? 4 : 5) : (amount < 0 ? 6 : 7);
      return XTestFakeButtonEvent(m_display, button, True, CurrentTime) &&
             XTestFakeButtonEvent(m_display, button, False, CurrentTime);
    }
    if (event.kind == InputKind::Key && (event.first < 8 || event.second > 1))
      return false;
    const auto pressed = std::ranges::find_if(m_injected, [&event](const InputEvent &item) {
      return item.kind == event.kind && item.first == event.first;
    });
    const bool down = event.second == 1;
    const bool tracked = pressed != m_injected.end();
    if ((!down && !tracked) || (down && tracked && event.kind == InputKind::Button))
      return true;
    const auto sent =
        event.kind == InputKind::Key
            ? XTestFakeKeyEvent(m_display, event.first, down, CurrentTime)
            : XTestFakeButtonEvent(m_display, XButtons.at(static_cast<size_t>(event.first - 1)), down, CurrentTime);
    if (!sent)
      return false;
    if (down && !tracked)
      m_injected.append(event);
    else if (!down)
      m_injected.erase(pressed);
    return true;
  }

  Display *const m_display;
  const int m_screen;
  const Window m_root;
  const X11DesktopAccess &m_access;
  InputExchange &m_exchange;
  const InputSide m_side;
  const bool m_rightEdge;
  const Clock::time_point m_startedAt = Clock::now();
  InputSide m_owner = InputSide::None;
  quint32 m_epoch = 0;
  int m_pointer = 0;
  int m_keyboard = 0;
  int m_xiOpcode = 0;
  int m_width = 0;
  int m_height = 0;
  bool m_connected = false;
  bool m_active = false;
  bool m_blocked = false;
  bool m_grabbed = false;
  bool m_readied = false;
  bool m_towardEdge = false;
  int m_takeoverDistance = 0;
  Clock::time_point m_nextRequest{};
  Clock::time_point m_requestUntil{};
  Clock::time_point m_nextGeometry{};
  Clock::time_point m_takeoverUntil{};
  Point m_playback{};
  Point m_returnPoint{};
  std::set<int> m_sources;
  std::set<int> m_relativeSources;
  std::array<bool, 256> m_keys{};
  QList<InputEvent> m_injected;
  QString m_error;
};

} // namespace

int x11InputCommand(int argc, char **argv)
{
  const QCoreApplication app(argc, argv);
  try {
    const auto options = parseInputOptions(
        QCoreApplication::arguments(),
        QStringLiteral(
            "Experimental bidirectional native X11 control. Stop the classic core before starting.\n"
            "Emergency exit: Ctrl+Alt+Shift+F12.\n"
            "Usage: deskflow-core --enhanced-control <listen|connect> [options]"
        ),
        false
    );
    if (geteuid() == 0 || qEnvironmentVariable("XDG_SESSION_TYPE") == QStringLiteral("wayland") ||
        !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
      throw Error(QStringLiteral("Enhanced X11 input requires a normal local Xorg session. Wayland is not supported."));
    }
    const auto displayName = qEnvironmentVariable("DISPLAY");
    if (!displayName.startsWith(u':'))
      throw Error(QStringLiteral("Enhanced X11 input requires a local X display, such as :0."));
    const InputSessionGuard guard;
    const std::unique_ptr<Display, decltype(&XCloseDisplay)> display(XOpenDisplay(nullptr), XCloseDisplay);
    if (!display)
      throw Error(QStringLiteral("Could not open the local X display."));
    XSetErrorHandler(handleProtocolError);

    InputExchange exchange;
    const X11DesktopAccess access(displayName);
    X11Input input(display.get(), exchange, options, access);
    if (!input.start())
      throw Error(QStringLiteral("Enhanced X11 input requires XI2.2 raw devices and XTEST on a native Xorg display."));

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::unique_ptr<QThread> network(QThread::create([&] {
      runInputNetwork(exchange, options, QByteArrayView("\x02\x03", 2));
    }));
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] { input.tick(); });
    timer.start(8);
    status("Waiting for a verified enhanced X11 peer.");
    network->start();
    QCoreApplication::exec();
    exchange.cancelled.store(true, std::memory_order_relaxed);
    network->wait();
    if (!input.error().isEmpty())
      throw Error(input.error());
    if (!exchange.error.isEmpty())
      throw Error(exchange.error);
    return 0;
  } catch (const std::exception &error) {
    QTextStream(stderr) << QString::fromUtf8(error.what()) << Qt::endl;
    return 1;
  }
}

} // namespace deskflow::enhanced
