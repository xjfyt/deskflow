/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WindowsInput.h"

#include "enhanced/InputSession.h"

#include <QCoreApplication>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <memory>

namespace deskflow::enhanced {
namespace {

using Clock = InputExchange::Clock;
using namespace std::chrono_literals;

constexpr ULONG_PTR InjectionTag = 0x44464558;
constexpr int EdgePadding = 12;
constexpr int TakeoverDistance = 8;
constexpr auto TakeoverWindow = 250ms;
constexpr size_t MaxPressedInputs = 2 * 255 + 5;
std::atomic_bool consoleInterrupted = false;

BOOL WINAPI interruptConsole(DWORD reason)
{
  if (reason != CTRL_C_EVENT && reason != CTRL_BREAK_EVENT && reason != CTRL_CLOSE_EVENT)
    return FALSE;
  consoleInterrupted.store(true, std::memory_order_relaxed);
  return TRUE;
}

void reportState(HANDLE guiPipe, const char *state, const char *message)
{
  QTextStream output(stdout);
  if (guiPipe)
    output << "DFEX_STATE " << state << Qt::endl;
  else
    output << message << Qt::endl;
}

bool guiStopRequested(HANDLE pipe)
{
  if (!pipe)
    return false;
  DWORD available = 0;
  if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
    return true;
  if (available == 0)
    return false;
  char command = 0;
  DWORD read = 0;
  return !ReadFile(pipe, &command, 1, &read, nullptr) || read == 0 || command == 'Q';
}

struct Desktop
{
  LONG left;
  LONG top;
  LONG width;
  LONG height;

  bool operator==(const Desktop &) const = default;
};

Desktop desktop()
{
  return {
      GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
      GetSystemMetrics(SM_CYVIRTUALSCREEN)
  };
}

POINT clampToDisplay(POINT point, const RECT &display)
{
  return {std::clamp(point.x, display.left, display.right - 1), std::clamp(point.y, display.top, display.bottom - 1)};
}

bool displayAt(POINT point, RECT &display)
{
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info))
    return false;
  display = info.rcMonitor;
  return true;
}

struct EdgeDisplaySearch
{
  LONG edge;
  LONG y;
  bool right;
  bool found = false;
  POINT point{};
};

BOOL CALLBACK findDisplayAtEdge(HMONITOR, HDC, LPRECT display, LPARAM data)
{
  auto &search = *reinterpret_cast<EdgeDisplaySearch *>(data);
  if ((search.right ? display->right - 1 : display->left) != search.edge)
    return TRUE;

  const auto point = clampToDisplay({search.edge + (search.right ? -EdgePadding : EdgePadding), search.y}, *display);
  if (!search.found || std::abs(point.y - search.y) < std::abs(search.point.y - search.y)) {
    search.found = true;
    search.point = point;
  }
  return TRUE;
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
  return readable && name == current && std::wcscmp(name.data(), L"Default") == 0;
}

bool unelevatedProcess()
{
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return false;
  TOKEN_ELEVATION elevation{};
  DWORD length = 0;
  const bool allowed = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &length) &&
                       elevation.TokenIsElevated == 0;
  CloseHandle(token);
  return allowed;
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

INPUT nativeInput(const InputEvent &event)
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
  return input;
}

bool inject(const InputEvent &event)
{
  auto input = nativeInput(event);
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
  WindowsInput(InputExchange &exchange, const InputSessionOptions &options, HANDLE guiPipe)
      : m_exchange(exchange),
        m_side(options.listener ? InputSide::Listener : InputSide::Connector),
        m_rightEdge(options.rightEdge),
        m_guiPipe(guiPipe)
  {
    s_current = this;
    for (size_t key = VK_BACK; key < m_physicalKeys.size(); ++key) {
      if (key != VK_CONTROL && key != VK_SHIFT && key != VK_MENU && key != VK_PAUSE)
        m_physicalKeys[key] = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    }
    static constexpr std::array mouseKeys{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (size_t i = 0; i < mouseKeys.size(); ++i)
      m_physicalButtons[i] = (GetAsyncKeyState(mouseKeys[i]) & 0x8000) != 0;
  }

  ~WindowsInput()
  {
    stopLease();
    if (!m_injected.isEmpty())
      QTextStream(stderr) << "Windows did not accept all injected input releases on this desktop." << Qt::endl;
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
    const auto [state, lastUpdate] = m_exchange.takeInbound();
    const auto now = Clock::now();
    if (guiStopRequested(m_guiPipe) || consoleInterrupted.load(std::memory_order_relaxed) ||
        m_exchange.cancelled.load(std::memory_order_relaxed) ||
        (m_connected && now - lastUpdate > std::chrono::milliseconds(InputWatchdog))) {
      m_blocked = true;
      m_exchange.cancelled.store(true, std::memory_order_relaxed);
      stopLease();
      reportState(m_guiPipe, "local", "Local input restored.");
      QCoreApplication::quit();
      return;
    }
    if (lastUpdate == Clock::time_point{})
      return;
    if (!m_connected) {
      m_connected = true;
      reportState(m_guiPipe, "paired", "Enhanced input paired. Move across the selected edge to control the peer.");
    }

    if (now - m_desktopChecked > 200ms) {
      m_desktopChecked = now;
      if (!interactiveDesktop()) {
        m_blocked = true;
        m_exchange.cancelled.store(true, std::memory_order_relaxed);
        stopLease();
        reportState(m_guiPipe, "local", "Local input restored.");
        QCoreApplication::quit();
        return;
      }
    }

    if (state.epoch != m_epoch || state.owner != m_owner) {
      stopLease();
      m_epoch = state.epoch;
      m_owner = state.owner;
      m_blocked = false;
      m_active = false;
      m_readied = false;
      m_takeoverDistance = 0;
      if (m_owner == InputSide::None)
        reportState(m_guiPipe, "local", "Local input restored.");
    }
    if (m_owner != InputSide::None && !m_blocked && !m_active) {
      if (localInputDown() || !m_injected.isEmpty() || (m_owner == m_side && now > m_requestUntil)) {
        release();
      } else if (state.active) {
        m_active = true;
        if (!enter(state.verticalPosition))
          release();
      } else if (m_owner != m_side && !m_readied) {
        const auto bounds = desktop();
        if (bounds.width < 2 * EdgePadding || bounds.height < 2 || !interactiveDesktop()) {
          release();
        } else {
          m_exchange.ready(m_epoch);
          m_readied = true;
        }
      }
    }
    if (!receiving() && !m_injected.isEmpty())
      stopLease();
    if (m_active && !m_blocked && desktop() != m_leaseDesktop)
      release();
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
    return m_active && m_owner == m_side && !m_blocked;
  }

  bool receiving() const
  {
    return m_active && peerOwnsLease();
  }

  bool peerOwnsLease() const
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
    m_leaseDesktop = bounds;
    if (sending()) {
      POINT cursor{};
      if (!GetCursorPos(&cursor) || std::abs(cursor.x - edge(bounds)) > EdgePadding)
        return false;
      RECT display{};
      if (!displayAt(cursor, display))
        return false;
      m_returnPoint = clampToDisplay({cursor.x + (m_rightEdge ? -EdgePadding : EdgePadding), cursor.y}, display);
      m_anchor = {display.left + (display.right - display.left) / 2, display.top + (display.bottom - display.top) / 2};
      if (!moveCursor(bounds, m_anchor))
        return false;
      m_captured = true;
      reportState(m_guiPipe, "sending", "Your physical input is controlling the peer.");
    } else {
      EdgeDisplaySearch search{
          edge(bounds), bounds.top + MulDiv(verticalPosition, bounds.height - 1, 65535), m_rightEdge
      };
      if (!EnumDisplayMonitors(nullptr, nullptr, findDisplayAtEdge, reinterpret_cast<LPARAM>(&search)) ||
          !search.found || !moveCursor(bounds, search.point)) {
        return false;
      }
      m_playbackPoint = search.point;
      reportState(m_guiPipe, "receiving", "The peer is controlling this computer. Use local input to take over.");
    }
    return true;
  }

  void stopLease()
  {
    if (!m_injected.isEmpty()) {
      std::array<INPUT, MaxPressedInputs> releases;
      for (qsizetype i = 0; i < m_injected.size(); ++i) {
        auto event = m_injected.at(m_injected.size() - 1 - i);
        event.second &= ~1;
        releases[static_cast<size_t>(i)] = nativeInput(event);
      }
      const auto inserted = SendInput(static_cast<UINT>(m_injected.size()), releases.data(), sizeof(INPUT));
      m_injected.resize(m_injected.size() - inserted);
    }
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
    m_exchange.release(m_epoch);
  }

  void enqueue(InputEvent event)
  {
    if (!m_exchange.enqueue(m_epoch, event))
      m_blocked = true;
  }

  bool apply(const InputEvent &event)
  {
    if (event.kind == InputKind::Move) {
      const auto bounds = desktop();
      if (bounds.width < 2 || bounds.height < 2)
        return false;
      const POINT point{m_playbackPoint.x + event.first, m_playbackPoint.y + event.second};
      const bool crossed =
          m_rightEdge ? event.first > 0 && point.x >= edge(bounds) : event.first < 0 && point.x <= edge(bounds);
      if (m_injected.isEmpty() && crossed)
        return false;
      RECT display{};
      if (!displayAt(point, display))
        return false;
      m_playbackPoint = clampToDisplay(point, display);
      return moveCursor(bounds, m_playbackPoint);
    }
    if (event.kind == InputKind::Wheel)
      return inject(event);

    const auto existing = std::ranges::find_if(m_injected, [&event](const InputEvent &pressed) {
      return pressed.kind == event.kind && pressed.first == event.first && (pressed.second & 2) == (event.second & 2);
    });
    const bool down = (event.second & 1) != 0;
    const bool tracked = existing != m_injected.end();
    if ((!down && !tracked) || (down && tracked && event.kind == InputKind::Button))
      return true;
    if (!inject(event))
      return false;
    if (down && !tracked) {
      m_injected.append(event);
    } else if (!down) {
      m_injected.erase(existing);
    }
    return true;
  }

  bool keyEvent(WPARAM message, const KBDLLHOOKSTRUCT &event)
  {
    if ((event.flags & LLKHF_INJECTED) || event.dwExtraInfo == InjectionTag || event.vkCode >= m_physicalKeys.size())
      return false;
    const auto key = static_cast<size_t>(event.vkCode);
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (key != VK_PAUSE)
      m_physicalKeys[key] = down;
    if (down && key == VK_F12 && (m_physicalKeys[VK_LCONTROL] || m_physicalKeys[VK_RCONTROL]) &&
        (m_physicalKeys[VK_LMENU] || m_physicalKeys[VK_RMENU]) &&
        (m_physicalKeys[VK_LSHIFT] || m_physicalKeys[VK_RSHIFT])) {
      m_exchange.cancelled.store(true, std::memory_order_relaxed);
      m_blocked = true;
      m_swallowedKeys[key] = true;
      return true;
    }
    if (peerOwnsLease())
      release();
    const bool swallowed = m_swallowedKeys[key];
    if (!down)
      m_swallowedKeys[key] = false;
    if (!sending())
      return swallowed;
    if (key == VK_PAUSE)
      return true;
    m_swallowedKeys[key] = down;
    if (event.scanCode == 0 || event.scanCode > 255)
      return true;
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
    if (peerOwnsLease()) {
      if (message == WM_MOUSEMOVE && !physicalMouseTakeover(event.pt))
        return receiving();
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
      POINT previous{};
      if (bounds.width > 1 && bounds.height > 1 && event.pt.x == edge(bounds) && now >= m_nextRequest &&
          GetCursorPos(&previous) && (m_rightEdge ? previous.x < event.pt.x : previous.x > event.pt.x)) {
        const auto y = std::clamp(event.pt.y - bounds.top, 0L, bounds.height - 1);
        m_exchange.request(m_epoch, static_cast<quint16>(MulDiv(y, 65535, bounds.height - 1)));
        m_nextRequest = now + 250ms;
        m_requestUntil = now + std::chrono::milliseconds(InputWatchdog);
      }
    }
    return swallowed;
  }

  bool physicalMouseTakeover(POINT point)
  {
    POINT previous{};
    if (!GetCursorPos(&previous))
      return true;
    const auto now = Clock::now();
    if (now >= m_takeoverUntil) {
      m_takeoverUntil = now + TakeoverWindow;
      m_takeoverDistance = 0;
    }
    m_takeoverDistance += std::abs(point.x - previous.x) + std::abs(point.y - previous.y);
    return m_takeoverDistance >= TakeoverDistance;
  }

  static inline WindowsInput *s_current = nullptr;

  InputExchange &m_exchange;
  const InputSide m_side;
  const bool m_rightEdge;
  const HANDLE m_guiPipe;
  InputSide m_owner = InputSide::None;
  quint32 m_epoch = 0;
  HHOOK m_mouse = nullptr;
  HHOOK m_keyboard = nullptr;
  bool m_connected = false;
  bool m_blocked = false;
  bool m_active = false;
  bool m_readied = false;
  bool m_captured = false;
  POINT m_anchor{};
  POINT m_returnPoint{};
  POINT m_playbackPoint{};
  Desktop m_leaseDesktop{};
  QList<InputEvent> m_injected;
  std::array<bool, 256> m_physicalKeys{};
  std::array<bool, 256> m_swallowedKeys{};
  std::array<bool, 5> m_physicalButtons{};
  std::array<bool, 5> m_swallowedButtons{};
  Clock::time_point m_nextRequest{};
  Clock::time_point m_requestUntil{};
  Clock::time_point m_desktopChecked{};
  Clock::time_point m_takeoverUntil{};
  LONG m_takeoverDistance = 0;
};

} // namespace

int windowsInputCommand(int argc, char **argv)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const QCoreApplication app(argc, argv);
  try {
    const auto options = parseInputOptions(
        QCoreApplication::arguments(),
        QStringLiteral(
            "Experimental bidirectional Windows control. Stop the classic core before starting.\n"
            "Emergency exit: Ctrl+Alt+Shift+F12.\n"
            "Usage: deskflow-core --enhanced-control <listen|connect> [options]"
        ),
        true
    );
    if (!unelevatedProcess())
      throw Error(QStringLiteral("Start experimental Windows control from a standard, non-elevated desktop session."));
    HANDLE guiPipe = nullptr;
    if (options.guiSession) {
      guiPipe = GetStdHandle(STD_INPUT_HANDLE);
      if (!guiPipe || guiPipe == INVALID_HANDLE_VALUE || GetFileType(guiPipe) != FILE_TYPE_PIPE)
        throw Error(QStringLiteral("A GUI input session requires a redirected stdin pipe."));
    }
    const InputSessionGuard sessionGuard;
    InputExchange exchange;
    WindowsInput input(exchange, options, guiPipe);
    if (!input.start())
      throw Error(QStringLiteral("Could not start Windows input hooks on the interactive desktop."));

    SetConsoleCtrlHandler(interruptConsole, TRUE);
    std::unique_ptr<QThread> network(QThread::create([&] {
      runInputNetwork(exchange, options, QByteArrayView("\x02\x02", 2));
    }));
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] { input.tick(); });
    timer.start(8);
    reportState(guiPipe, "waiting", "Waiting for a verified enhanced input peer.");
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
