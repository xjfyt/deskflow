/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WindowsInput.h"

#include "common/Constants.h"
#include "enhanced/InputLease.h"
#include "enhanced/TlsChannel.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSharedMemory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <utility>

namespace deskflow::enhanced {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int Watchdog = 600;
constexpr ULONG_PTR InjectionTag = 0x44464558;
constexpr int EdgePadding = 12;
constexpr char InputCapability = '\x02';
std::atomic_bool consoleInterrupted = false;

BOOL WINAPI interruptConsole(DWORD reason)
{
  if (reason != CTRL_C_EVENT && reason != CTRL_BREAK_EVENT && reason != CTRL_CLOSE_EVENT)
    return FALSE;
  consoleInterrupted.store(true, std::memory_order_relaxed);
  return TRUE;
}

struct Exchange
{
  std::mutex mutex;
  InputPoll outbound;
  InputState inbound;
  Clock::time_point lastUpdate{};
  std::atomic_bool cancelled = false;
  QString error;
};

struct InputOptions
{
  PeerIdentity identity;
  QByteArray pin;
  QHostAddress address;
  quint16 port;
  bool listener;
  bool rightEdge;
};

InputPoll takePoll(Exchange &exchange)
{
  std::lock_guard lock(exchange.mutex);
  return std::exchange(exchange.outbound, {});
}

void publish(Exchange &exchange, const InputState &state, InputSide localSide)
{
  std::lock_guard lock(exchange.mutex);
  if (exchange.inbound.epoch != state.epoch || exchange.inbound.owner != state.owner)
    exchange.inbound = {state.owner, state.epoch, state.verticalPosition, {}};
  if (state.owner != localSide) {
    if (exchange.inbound.events.size() + state.events.size() > MaxInputEvents)
      throw Error(QStringLiteral("Enhanced input playback queue overflow."));
    exchange.inbound.events.append(state.events);
  }
  exchange.lastUpdate = Clock::now();
}

void runNetwork(Exchange &exchange, const InputOptions &options)
{
  QTcpSocket outgoing;
  std::unique_ptr<QTcpSocket> incoming;
  if (options.listener) {
    QTcpServer server;
    listen(server, options.address, options.port);
    QTextStream(stdout) << "Waiting for a verified enhanced input peer…" << Qt::endl;
    incoming = accept(server, QDeadlineTimer(120000), exchange.cancelled);
  } else {
    connectSocket(outgoing, options.address, options.port, QDeadlineTimer(5000), exchange.cancelled);
  }

  TlsChannel channel(
      options.listener ? *incoming : outgoing, options.identity, options.pin, exchange.cancelled, options.listener,
      QDeadlineTimer(5000)
  );
  channel.send(Message::Hello, QByteArray(1, InputCapability));
  if (channel.receive(Message::Hello, 1, 1).at(0) != InputCapability)
    throw Error(QStringLiteral("The verified peer does not support this enhanced input session."));

  InputState lease;
  while (!exchange.cancelled.load(std::memory_order_relaxed)) {
    const QDeadlineTimer deadline(Watchdog);
    if (options.listener) {
      const auto peer =
          InputPoll::decode(channel.receive(Message::InputPoll, InputHeaderSize, MaxInputPacket, deadline));
      advanceLease(lease, takePoll(exchange), peer);
      publish(exchange, lease, InputSide::Listener);
      if (lease.owner == InputSide::Connector)
        lease.events.clear();
      channel.send(Message::InputState, lease.encode(), deadline);
    } else {
      channel.send(Message::InputPoll, takePoll(exchange).encode(), deadline);
      const auto state =
          InputState::decode(channel.receive(Message::InputState, InputHeaderSize, MaxInputPacket, deadline));
      if (state.epoch < lease.epoch ||
          (state.epoch == lease.epoch && state.owner != InputSide::None && state.owner != lease.owner) ||
          (state.owner == InputSide::Connector && !state.events.isEmpty())) {
        throw Error(QStringLiteral("Invalid enhanced input lease transition."));
      }
      publish(exchange, state, InputSide::Connector);
      lease = {state.owner, state.epoch, state.verticalPosition, {}};
      QThread::msleep(8);
    }
  }
}

struct Desktop
{
  LONG left;
  LONG top;
  LONG width;
  LONG height;
};

Desktop desktop()
{
  return {
      GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
      GetSystemMetrics(SM_CYVIRTUALSCREEN)
  };
}

bool interactiveDesktop()
{
  const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!input)
    return false;

  std::array<wchar_t, 128> name{};
  std::array<wchar_t, 128> current{};
  DWORD nameLength = 0;
  DWORD currentLength = 0;
  const bool readable =
      GetUserObjectInformationW(input, UOI_NAME, name.data(), sizeof(name), &nameLength) &&
      GetUserObjectInformationW(
          GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, current.data(), sizeof(current), &currentLength
      );
  CloseDesktop(input);
  return readable && name == current;
}

bool moveCursor(const Desktop &bounds, POINT point)
{
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE;
  input.mi.dwExtraInfo = InjectionTag;
  input.mi.dx = MulDiv(std::clamp(point.x - bounds.left, 0L, bounds.width - 1), 65535, bounds.width - 1);
  input.mi.dy = MulDiv(std::clamp(point.y - bounds.top, 0L, bounds.height - 1), 65535, bounds.height - 1);
  return SendInput(1, &input, sizeof(input)) == 1;
}

bool inject(const InputEvent &event)
{
  INPUT input{};
  if (event.kind == InputKind::Key) {
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = static_cast<WORD>(event.first);
    input.ki.dwFlags = KEYEVENTF_SCANCODE | ((event.second & 2) ? KEYEVENTF_EXTENDEDKEY : 0) |
                       ((event.second & 1) ? 0 : KEYEVENTF_KEYUP);
    input.ki.dwExtraInfo = InjectionTag;
  } else {
    input.type = INPUT_MOUSE;
    input.mi.dwExtraInfo = InjectionTag;
    if (event.kind == InputKind::Wheel) {
      input.mi.dwFlags = event.first == 0 ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
      input.mi.mouseData = static_cast<DWORD>(event.first == 0 ? event.second : event.first);
    } else {
      static constexpr std::array<DWORD, 5> down{
          MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN
      };
      static constexpr std::array<DWORD, 5> up{
          MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_XUP, MOUSEEVENTF_XUP
      };
      const auto index = static_cast<size_t>(event.first - 1);
      input.mi.dwFlags = event.second ? down.at(index) : up.at(index);
      if (index >= 3)
        input.mi.mouseData = index == 3 ? XBUTTON1 : XBUTTON2;
    }
  }
  return SendInput(1, &input, sizeof(input)) == 1;
}

InputEvent mouseButton(WPARAM message, DWORD data)
{
  switch (message) {
  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
    return {InputKind::Button, 1, message == WM_LBUTTONDOWN};
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP:
    return {InputKind::Button, 2, message == WM_RBUTTONDOWN};
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
    return {InputKind::Button, 3, message == WM_MBUTTONDOWN};
  case WM_XBUTTONDOWN:
  case WM_XBUTTONUP:
    return {InputKind::Button, static_cast<qint16>(HIWORD(data) == XBUTTON1 ? 4 : 5), message == WM_XBUTTONDOWN};
  default:
    return {InputKind::Button, 0, 0};
  }
}

class WindowsInput
{
public:
  WindowsInput(Exchange &exchange, const InputOptions &options)
      : m_exchange(exchange),
        m_side(options.listener ? InputSide::Listener : InputSide::Connector),
        m_rightEdge(options.rightEdge)
  {
    s_current = this;
    for (size_t key = VK_BACK; key < m_physicalKeys.size(); ++key) {
      if (key != VK_CONTROL && key != VK_SHIFT && key != VK_MENU)
        m_physicalKeys[key] = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    }
    static constexpr std::array mouseKeys{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (size_t i = 0; i < mouseKeys.size(); ++i)
      m_physicalButtons[i] = (GetAsyncKeyState(mouseKeys[i]) & 0x8000) != 0;
  }

  ~WindowsInput()
  {
    stopLease();
    if (m_mouse)
      UnhookWindowsHookEx(m_mouse);
    if (m_keyboard)
      UnhookWindowsHookEx(m_keyboard);
    s_current = nullptr;
  }

  bool start()
  {
    const auto instance = GetModuleHandleW(nullptr);
    if (!interactiveDesktop())
      return false;
    m_keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyHook, instance, 0);
    m_mouse = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, instance, 0);
    return m_keyboard && m_mouse;
  }

  void tick()
  {
    InputState state;
    Clock::time_point lastUpdate;
    {
      std::lock_guard lock(m_exchange.mutex);
      state = m_exchange.inbound;
      m_exchange.inbound.events.clear();
      lastUpdate = m_exchange.lastUpdate;
    }
    const auto now = Clock::now();
    if (consoleInterrupted.load(std::memory_order_relaxed) || m_exchange.cancelled.load(std::memory_order_relaxed) ||
        (m_connected && now - lastUpdate > std::chrono::milliseconds(Watchdog))) {
      m_blocked = true;
      stopLease();
      QCoreApplication::quit();
      return;
    }
    if (lastUpdate == Clock::time_point{})
      return;
    if (!m_connected) {
      m_connected = true;
      QTextStream(stdout) << "Enhanced input paired. Move across the selected edge to control the peer." << Qt::endl;
    }

    if (now - m_desktopChecked > 200ms) {
      m_desktopChecked = now;
      if (!interactiveDesktop()) {
        m_blocked = true;
        stopLease();
        QCoreApplication::quit();
        return;
      }
    }

    if (state.epoch != m_epoch || state.owner != m_owner) {
      stopLease();
      m_epoch = state.epoch;
      m_owner = state.owner;
      m_blocked = false;
      if (m_owner != InputSide::None) {
        if (localInputDown() || !enter(state.verticalPosition))
          release();
      } else {
        QTextStream(stdout) << "Local input restored." << Qt::endl;
      }
    }
    if (receiving()) {
      for (const auto &event : state.events) {
        if (!apply(event)) {
          release();
          break;
        }
      }
    }
  }

private:
  static LRESULT CALLBACK keyHook(int code, WPARAM message, LPARAM value)
  {
    if (code == HC_ACTION && s_current->keyEvent(message, *reinterpret_cast<const KBDLLHOOKSTRUCT *>(value)))
      return 1;
    return CallNextHookEx(nullptr, code, message, value);
  }

  static LRESULT CALLBACK mouseHook(int code, WPARAM message, LPARAM value)
  {
    if (code == HC_ACTION && s_current->mouseEvent(message, *reinterpret_cast<const MSLLHOOKSTRUCT *>(value)))
      return 1;
    return CallNextHookEx(nullptr, code, message, value);
  }

  bool localInputDown() const
  {
    return std::ranges::any_of(m_physicalKeys, [](bool down) { return down; }) ||
           std::ranges::any_of(m_physicalButtons, [](bool down) { return down; });
  }

  bool sending() const
  {
    return m_owner == m_side && !m_blocked;
  }

  bool receiving() const
  {
    return m_owner != InputSide::None && m_owner != m_side && !m_blocked;
  }

  LONG edge(const Desktop &bounds) const
  {
    return m_rightEdge ? bounds.left + bounds.width - 1 : bounds.left;
  }

  bool enter(quint16 verticalPosition)
  {
    const auto bounds = desktop();
    if (bounds.width < 2 * EdgePadding || bounds.height < 2)
      return false;
    if (sending()) {
      POINT cursor{};
      if (!GetCursorPos(&cursor) || std::abs(cursor.x - edge(bounds)) > EdgePadding)
        return false;
      m_returnPoint = cursor;
      m_returnPoint.x += m_rightEdge ? -EdgePadding : EdgePadding;
      m_anchor = {bounds.left + bounds.width / 2, bounds.top + bounds.height / 2};
      if (!moveCursor(bounds, m_anchor))
        return false;
      m_captured = true;
      QTextStream(stdout) << "Your physical input is controlling the peer." << Qt::endl;
    } else {
      const auto y = bounds.top + MulDiv(verticalPosition, bounds.height - 1, 65535);
      if (!moveCursor(bounds, {edge(bounds) + (m_rightEdge ? -EdgePadding : EdgePadding), y}))
        return false;
      QTextStream(stdout) << "The peer is controlling this computer. Use local input to take over." << Qt::endl;
    }
    return true;
  }

  void stopLease()
  {
    for (qsizetype i = m_injected.size(); i > 0; --i) {
      auto event = m_injected.at(i - 1);
      event.second &= ~1;
      inject(event);
    }
    m_injected.clear();
    if (m_captured) {
      const auto bounds = desktop();
      if (bounds.width > 1 && bounds.height > 1)
        moveCursor(bounds, m_returnPoint);
      m_captured = false;
    }
  }

  void release()
  {
    m_blocked = true;
    stopLease();
    std::lock_guard lock(m_exchange.mutex);
    m_exchange.outbound = {m_epoch, 0, false, true, {}};
  }

  void enqueue(InputEvent event)
  {
    std::lock_guard lock(m_exchange.mutex);
    auto &poll = m_exchange.outbound;
    poll.epoch = m_epoch;
    if (event.kind == InputKind::Move && !poll.events.isEmpty() && poll.events.last().kind == InputKind::Move) {
      auto &previous = poll.events.last();
      previous.first = static_cast<qint16>(std::clamp<int>(previous.first + event.first, -8192, 8192));
      previous.second = static_cast<qint16>(std::clamp<int>(previous.second + event.second, -8192, 8192));
      return;
    }
    if (poll.events.size() >= MaxInputEvents) {
      m_exchange.cancelled.store(true, std::memory_order_relaxed);
      m_blocked = true;
      return;
    }
    poll.events.append(event);
  }

  bool apply(const InputEvent &event)
  {
    if (event.kind == InputKind::Move) {
      POINT cursor{};
      const auto bounds = desktop();
      if (!GetCursorPos(&cursor) || bounds.width < 2 || bounds.height < 2)
        return false;
      const auto x = cursor.x + event.first;
      if (m_injected.isEmpty() && event.first != 0 && (m_rightEdge ? x >= edge(bounds) : x <= edge(bounds)))
        return false;
      return moveCursor(bounds, {x, cursor.y + event.second});
    }
    if (!inject(event))
      return false;

    if (event.kind == InputKind::Key || event.kind == InputKind::Button) {
      const auto existing = std::ranges::find_if(m_injected, [&event](const InputEvent &pressed) {
        return pressed.kind == event.kind && pressed.first == event.first && (pressed.second & 2) == (event.second & 2);
      });
      if ((event.second & 1) != 0) {
        if (existing == m_injected.end())
          m_injected.append(event);
      } else if (existing != m_injected.end()) {
        m_injected.erase(existing);
      }
    }
    return true;
  }

  bool keyEvent(WPARAM message, const KBDLLHOOKSTRUCT &event)
  {
    if ((event.flags & LLKHF_INJECTED) || event.dwExtraInfo == InjectionTag || event.vkCode >= m_physicalKeys.size())
      return false;
    const auto key = static_cast<size_t>(event.vkCode);
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    m_physicalKeys[key] = down;
    if (down && key == VK_F12 && (m_physicalKeys[VK_LCONTROL] || m_physicalKeys[VK_RCONTROL]) &&
        (m_physicalKeys[VK_LMENU] || m_physicalKeys[VK_RMENU]) &&
        (m_physicalKeys[VK_LSHIFT] || m_physicalKeys[VK_RSHIFT])) {
      m_exchange.cancelled.store(true, std::memory_order_relaxed);
      m_blocked = true;
      stopLease();
      return true;
    }
    if (receiving())
      release();
    const bool swallowed = m_swallowedKeys[key];
    if (!down)
      m_swallowedKeys[key] = false;
    if (!sending())
      return swallowed;
    if (event.scanCode == 0 || event.scanCode > 255 || key == VK_PAUSE)
      return swallowed;
    m_swallowedKeys[key] = down;
    const auto flags = static_cast<qint16>(((event.flags & LLKHF_EXTENDED) ? 2 : 0) | (down ? 1 : 0));
    enqueue({InputKind::Key, static_cast<qint16>(event.scanCode), flags});
    return true;
  }

  bool mouseEvent(WPARAM message, const MSLLHOOKSTRUCT &event)
  {
    if ((event.flags & LLMHF_INJECTED) || event.dwExtraInfo == InjectionTag)
      return false;
    const auto button = mouseButton(message, event.mouseData);
    bool swallowed = false;
    if (button.first != 0) {
      const auto index = static_cast<size_t>(button.first - 1);
      m_physicalButtons[index] = button.second;
      swallowed = m_swallowedButtons[index];
      if (!button.second)
        m_swallowedButtons[index] = false;
    }
    if (receiving()) {
      release();
      return false;
    }
    if (sending()) {
      if (message == WM_MOUSEMOVE) {
        enqueue(
            {InputKind::Move, static_cast<qint16>(std::clamp(event.pt.x - m_anchor.x, -8192L, 8192L)),
             static_cast<qint16>(std::clamp(event.pt.y - m_anchor.y, -8192L, 8192L))}
        );
      } else if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        const auto delta = static_cast<qint16>(HIWORD(event.mouseData));
        if (delta != 0)
          enqueue(
              {InputKind::Wheel, message == WM_MOUSEHWHEEL ? delta : qint16{0},
               message == WM_MOUSEWHEEL ? delta : qint16{0}}
          );
      } else if (button.first != 0) {
        m_swallowedButtons[static_cast<size_t>(button.first - 1)] = button.second;
        enqueue(button);
      }
      return true;
    }
    if (m_connected && m_owner == InputSide::None && message == WM_MOUSEMOVE && !localInputDown()) {
      const auto bounds = desktop();
      const auto now = Clock::now();
      if (bounds.width > 1 && bounds.height > 1 && event.pt.x == edge(bounds) && now >= m_nextRequest) {
        const auto y = std::clamp(event.pt.y - bounds.top, 0L, bounds.height - 1);
        std::lock_guard lock(m_exchange.mutex);
        m_exchange.outbound = {m_epoch, static_cast<quint16>(MulDiv(y, 65535, bounds.height - 1)), true, false, {}};
        m_nextRequest = now + 250ms;
      }
    }
    return swallowed;
  }

  static inline WindowsInput *s_current = nullptr;

  Exchange &m_exchange;
  const InputSide m_side;
  const bool m_rightEdge;
  InputSide m_owner = InputSide::None;
  quint32 m_epoch = 0;
  HHOOK m_mouse = nullptr;
  HHOOK m_keyboard = nullptr;
  bool m_connected = false;
  bool m_blocked = false;
  bool m_captured = false;
  POINT m_anchor{};
  POINT m_returnPoint{};
  QList<InputEvent> m_injected;
  std::array<bool, 256> m_physicalKeys{};
  std::array<bool, 256> m_swallowedKeys{};
  std::array<bool, 5> m_physicalButtons{};
  std::array<bool, 5> m_swallowedButtons{};
  Clock::time_point m_nextRequest{};
  Clock::time_point m_desktopChecked{};
};

} // namespace

int windowsInputCommand(int argc, char **argv)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const QCoreApplication app(argc, argv);
  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(
      "Experimental bidirectional Windows control. Stop the classic core before starting.\n"
      "Emergency exit: Ctrl+Alt+Shift+F12.\n"
      "Usage: deskflow-core --enhanced-control <listen|connect> [options]"
  ));
  parser.addHelpOption();
  parser.addPositionalArgument(QStringLiteral("mode"), QStringLiteral("listen or connect"));
  const QCommandLineOption certificate(
      QStringLiteral("certificate"), QStringLiteral("Local enhanced PEM identity file."), QStringLiteral("path")
  );
  const QCommandLineOption peer(
      QStringLiteral("peer"), QStringLiteral("Verified SHA-256 fingerprint of the other computer."),
      QStringLiteral("fingerprint")
  );
  const QCommandLineOption address(
      QStringLiteral("address"), QStringLiteral("Explicit local bind IP or remote listener IP."), QStringLiteral("IP")
  );
  const QCommandLineOption edge(
      QStringLiteral("edge"), QStringLiteral("Local screen edge facing the other computer."),
      QStringLiteral("left|right")
  );
  const QCommandLineOption port(
      QStringLiteral("port"), QStringLiteral("Enhanced input port."), QStringLiteral("port"), QStringLiteral("24803")
  );
  parser.addOptions({certificate, peer, address, edge, port});
  auto arguments = QCoreApplication::arguments();
  arguments.removeAt(1);
  parser.process(arguments);

  try {
    const auto mode = parser.positionalArguments().value(0);
    const auto localEdge = parser.value(edge);
    const auto bindPort = parser.value(port).toUShort();
    const QHostAddress ip(parser.value(address));
    const auto pin = parseFingerprint(parser.value(peer));
    if (parser.positionalArguments().size() != 1 ||
        (mode != QStringLiteral("listen") && mode != QStringLiteral("connect")) ||
        (localEdge != QStringLiteral("right") && localEdge != QStringLiteral("left")) || bindPort == 0 || ip.isNull() ||
        pin.isEmpty() || parser.value(certificate).isEmpty()) {
      throw Error(
          QStringLiteral("Specify listen or connect, a certificate, peer fingerprint, IP address and screen edge.")
      );
    }
    const auto identity = loadIdentity(parser.value(certificate));
    if (identity.fingerprint == pin)
      throw Error(QStringLiteral("Enter the fingerprint of the other computer."));
    const InputOptions options{
        identity, pin, ip, bindPort, mode == QStringLiteral("listen"), localEdge == QStringLiteral("right")
    };

    QSharedMemory classicLock(kCoreBinName);
    if (classicLock.attach())
      classicLock.detach();
    if (!classicLock.create(1))
      throw Error(QStringLiteral("Stop the running Deskflow core before starting enhanced control."));

    Exchange exchange;
    WindowsInput input(exchange, options);
    if (!input.start())
      throw Error(QStringLiteral("Could not start Windows input hooks on the interactive desktop."));

    SetConsoleCtrlHandler(interruptConsole, TRUE);
    std::unique_ptr<QThread> network(QThread::create([&] {
      try {
        runNetwork(exchange, options);
      } catch (const std::exception &error) {
        if (!exchange.cancelled.load(std::memory_order_relaxed))
          exchange.error = QString::fromUtf8(error.what());
        exchange.cancelled.store(true, std::memory_order_relaxed);
      }
    }));
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] { input.tick(); });
    timer.start(8);
    network->start();
    QCoreApplication::exec();
    exchange.cancelled.store(true, std::memory_order_relaxed);
    network->wait();
    SetConsoleCtrlHandler(interruptConsole, FALSE);
    if (!exchange.error.isEmpty())
      throw Error(exchange.error);
    return 0;
  } catch (const std::exception &error) {
    QTextStream(stderr) << QString::fromUtf8(error.what()) << Qt::endl;
    return 1;
  }
}

} // namespace deskflow::enhanced
