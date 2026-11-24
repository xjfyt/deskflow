/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MacInput.h"

#include "enhanced/InputSession.h"

#include <QCoreApplication>
#include <QRandomGenerator>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <IOKit/hidsystem/IOLLEvent.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <unistd.h>

namespace deskflow::enhanced {
namespace {

using Clock = InputExchange::Clock;
using namespace std::chrono_literals;

constexpr double EdgePadding = 12;
constexpr int TakeoverDistance = 8;
constexpr std::size_t KeyCount = 128;
constexpr std::size_t ButtonCount = 5;
volatile std::sig_atomic_t interrupted = 0;

template <typename Ref> using CFRef = std::unique_ptr<std::remove_pointer_t<Ref>, decltype(&CFRelease)>;

struct Modifier
{
  CGKeyCode key;
  CGEventFlags mask;
  CGEventFlags deviceMask;
  CGEventFlags groupMask;
};

constexpr std::array modifiers{
    Modifier{kVK_Shift, kCGEventFlagMaskShift, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK | NX_DEVICELSHIFTKEYMASK},
    Modifier{
        kVK_RightShift, kCGEventFlagMaskShift, NX_DEVICERSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK | NX_DEVICELSHIFTKEYMASK
    },
    Modifier{kVK_Control, kCGEventFlagMaskControl, NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK | NX_DEVICELCTLKEYMASK},
    Modifier{
        kVK_RightControl, kCGEventFlagMaskControl, NX_DEVICERCTLKEYMASK, NX_DEVICERCTLKEYMASK | NX_DEVICELCTLKEYMASK
    },
    Modifier{kVK_Option, kCGEventFlagMaskAlternate, NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK | NX_DEVICELALTKEYMASK},
    Modifier{
        kVK_RightOption, kCGEventFlagMaskAlternate, NX_DEVICERALTKEYMASK, NX_DEVICERALTKEYMASK | NX_DEVICELALTKEYMASK
    },
    Modifier{kVK_Command, kCGEventFlagMaskCommand, NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK | NX_DEVICELCMDKEYMASK},
    Modifier{
        kVK_RightCommand, kCGEventFlagMaskCommand, NX_DEVICERCMDKEYMASK, NX_DEVICERCMDKEYMASK | NX_DEVICELCMDKEYMASK
    }
};

const Modifier *modifierFor(CGKeyCode key)
{
  const auto found = std::ranges::find(modifiers, key, &Modifier::key);
  return found == modifiers.end() ? nullptr : &*found;
}

CGEventFlags modifierFlags(const std::bitset<KeyCount> &keys)
{
  CGEventFlags flags = 0;
  for (const auto &modifier : modifiers) {
    if (keys.test(modifier.key))
      flags |= modifier.mask | modifier.deviceMask;
  }
  return flags;
}

bool supportedKey(std::int64_t key)
{
  return key >= 0 && key < static_cast<std::int64_t>(KeyCount) && key != kVK_CapsLock && key != kVK_Function;
}

bool captureAccess()
{
  return AXIsProcessTrusted() && CGPreflightListenEventAccess() && CGPreflightPostEventAccess();
}

bool activeConsole()
{
  const CFRef<CFDictionaryRef> session(CGSessionCopyCurrentDictionary(), CFRelease);
  if (!session || CFDictionaryGetValue(session.get(), kCGSessionOnConsoleKey) != kCFBooleanTrue ||
      CFDictionaryGetValue(session.get(), kCGSessionLoginDoneKey) != kCFBooleanTrue) {
    return false;
  }
  const auto user = CFDictionaryGetValue(session.get(), kCGSessionUserIDKey);
  std::int32_t userId = 0;
  return user && CFGetTypeID(user) == CFNumberGetTypeID() &&
         CFNumberGetValue(static_cast<CFNumberRef>(user), kCFNumberSInt32Type, &userId) && userId >= 0 &&
         static_cast<uid_t>(userId) == geteuid();
}

CGDirectDisplayID activeDisplay()
{
  std::array<CGDirectDisplayID, 2> displays{};
  std::uint32_t count = 0;
  if (CGGetActiveDisplayList(static_cast<std::uint32_t>(displays.size()), displays.data(), &count) != kCGErrorSuccess ||
      count != 1 || CGDisplayIsAsleep(displays.front())) {
    return kCGNullDirectDisplay;
  }
  return displays.front();
}

void handleSignal(int)
{
  interrupted = 1;
}

void status(const char *message)
{
  QTextStream(stdout) << message << Qt::endl;
}

bool readCursor(CGPoint &point)
{
  const CFRef<CGEventRef> event(CGEventCreate(nullptr), CFRelease);
  if (!event)
    return false;
  point = CGEventGetLocation(event.get());
  return std::isfinite(point.x) && std::isfinite(point.y);
}

qint16 eventDelta(CGEventRef event, CGEventField field, int limit)
{
  return static_cast<qint16>(std::clamp<std::int64_t>(CGEventGetIntegerValueField(event, field), -limit, limit));
}

class MacInput
{
public:
  MacInput(InputExchange &exchange, const InputSessionOptions &options)
      : m_exchange(exchange),
        m_side(options.listener ? InputSide::Listener : InputSide::Connector),
        m_rightEdge(options.rightEdge)
  {
  }

  ~MacInput()
  {
    stopLease();
    if (m_injectedKeys.any() || m_injectedButtons.any())
      QTextStream(stderr) << "macOS blocked one or more injected key or button releases." << Qt::endl;
    if (m_source)
      CFRunLoopRemoveSource(CFRunLoopGetCurrent(), m_source.get(), kCFRunLoopCommonModes);
    if (m_tap) {
      CGEventTapEnable(m_tap.get(), false);
      CFMachPortInvalidate(m_tap.get());
    }
  }

  void start()
  {
    if (!captureAccess()) {
      throw Error(QStringLiteral(
          "Enable Accessibility and Input Monitoring for the executable or its responsible app, then restart."
      ));
    }
    if (geteuid() == 0 || !activeConsole() || IsSecureEventInputEnabled())
      throw Error(QStringLiteral("Enhanced input requires the active local macOS desktop with Secure Input inactive."));
    m_display = activeDisplay();
    if (m_display == kCGNullDirectDisplay)
      throw Error(QStringLiteral("Experimental macOS input requires one active display."));
    m_bounds = CGDisplayBounds(m_display);
    if (m_bounds.size.width <= 2 * EdgePadding || m_bounds.size.height < 2)
      throw Error(QStringLiteral("Could not resolve the active macOS display bounds."));

    m_inject.reset(CGEventSourceCreate(kCGEventSourceStatePrivate));
    if (!m_inject)
      throw Error(QStringLiteral("Could not create a macOS input event source."));
    CGEventSourceSetUserData(m_inject.get(), m_tag);
    CGEventSourceSetLocalEventsSuppressionInterval(m_inject.get(), 0);
    CGEventSourceSetLocalEventsFilterDuringSuppressionState(
        m_inject.get(), kCGEventFilterMaskPermitAllEvents, kCGEventSuppressionStateRemoteMouseDrag
    );

    CGEventMask mask = 0;
    for (const auto event :
         {kCGEventMouseMoved, kCGEventLeftMouseDragged, kCGEventRightMouseDragged, kCGEventOtherMouseDragged,
          kCGEventLeftMouseDown, kCGEventLeftMouseUp, kCGEventRightMouseDown, kCGEventRightMouseUp,
          kCGEventOtherMouseDown, kCGEventOtherMouseUp, kCGEventScrollWheel, kCGEventKeyDown, kCGEventKeyUp,
          kCGEventFlagsChanged})
      mask |= CGEventMaskBit(event);
    m_tap.reset(CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask, tap, this));
    if (!m_tap)
      throw Error(QStringLiteral("Could not create a macOS session event tap."));
    m_source.reset(CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_tap.get(), 0));
    if (!m_source)
      throw Error(QStringLiteral("Could not make the macOS event tap runnable."));
    for (std::size_t key = 0; key < KeyCount; ++key) {
      if (supportedKey(static_cast<std::int64_t>(key))) {
        m_keys.set(key, CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, static_cast<CGKeyCode>(key)));
      }
    }
    for (std::size_t button = 0; button < ButtonCount; ++button) {
      m_buttons.set(
          button, CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, static_cast<CGMouseButton>(button))
      );
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), m_source.get(), kCFRunLoopCommonModes);
    CGEventTapEnable(m_tap.get(), true);
    if (!CGEventTapIsEnabled(m_tap.get()))
      throw Error(QStringLiteral("The macOS session event tap is disabled."));
  }

  void tick()
  {
    const auto [state, lastUpdate] = m_exchange.takeInbound();
    const auto now = Clock::now();
    if (interrupted || m_exchange.cancelled.load(std::memory_order_relaxed) ||
        (m_connected && now - lastUpdate > std::chrono::milliseconds(InputWatchdog))) {
      finish();
      return;
    }
    if (m_tapDisabled || !CGEventTapIsEnabled(m_tap.get()) || IsSecureEventInputEnabled() ||
        (m_waitingForInjection != Clock::time_point{} && now - m_waitingForInjection > 250ms)) {
      finish("macOS suspended input monitoring or event delivery.");
      return;
    }
    if (now >= m_nextDesktopCheck) {
      m_nextDesktopCheck = now + 150ms;
      if (!captureAccess() || !activeConsole() || activeDisplay() != m_display ||
          !CGRectEqualToRect(m_bounds, CGDisplayBounds(m_display))) {
        finish("The macOS desktop, display or input permissions changed.");
        return;
      }
    }
    if (lastUpdate == Clock::time_point{})
      return;
    if (!m_connected) {
      m_connected = true;
      status("Enhanced macOS input paired. Move across the selected edge to control the other Mac.");
    }
    if (state.epoch != m_epoch || state.owner != m_owner) {
      stopLease();
      m_epoch = state.epoch;
      m_owner = state.owner;
      m_blocked = false;
      m_active = false;
      m_readied = false;
      m_takeover = 0;
      if (m_owner == InputSide::None)
        status("Local input restored.");
    }
    if (m_owner != InputSide::None && !m_blocked && !m_active) {
      if (localInputHeld() || m_injectedKeys.any() || m_injectedButtons.any() ||
          (m_owner == m_side && now > m_requestUntil)) {
        revoke();
      } else if (state.active) {
        m_active = true;
        if (!activate(state.verticalPosition))
          revoke();
      } else if (peerOwnsLease() && !m_readied) {
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
    } else if (sending() && m_recenter) {
      m_recenter = false;
      if (CGWarpMouseCursorPosition(anchor()) != kCGErrorSuccess)
        revoke();
    }
    if (m_blocked)
      restoreCursor();
  }

private:
  static CGEventRef tap(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *context)
  {
    auto &input = *static_cast<MacInput *>(context);
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
      input.m_tapDisabled = true;
      return event;
    }
    if (!event)
      return event;
    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == input.m_tag) {
      input.m_waitingForInjection = {};
      return event;
    }
    if (CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) != 0 || input.m_tapDisabled ||
        input.m_exchange.cancelled.load(std::memory_order_relaxed) || IsSecureEventInputEnabled()) {
      return event;
    }
    return input.handleLocal(proxy, type, event) ? nullptr : event;
  }

  bool localInputHeld() const
  {
    return m_keys.any() || m_buttons.any();
  }

  bool sending() const
  {
    return m_active && !m_blocked && m_owner == m_side;
  }

  bool peerOwnsLease() const
  {
    return !m_blocked && m_owner != InputSide::None && m_owner != m_side;
  }

  bool receiving() const
  {
    return m_active && peerOwnsLease();
  }

  double edge() const
  {
    return m_rightEdge ? CGRectGetMaxX(m_bounds) - 1 : CGRectGetMinX(m_bounds);
  }

  CGPoint anchor() const
  {
    return CGPointMake(CGRectGetMidX(m_bounds), CGRectGetMidY(m_bounds));
  }

  bool activate(quint16 verticalPosition)
  {
    if (sending()) {
      if (!readCursor(m_returnPoint) || !CGRectContainsPoint(m_bounds, m_returnPoint) ||
          std::abs(m_returnPoint.x - edge()) >= EdgePadding || CGWarpMouseCursorPosition(anchor()) != kCGErrorSuccess) {
        return false;
      }
      m_returnPoint.x = edge() + (m_rightEdge ? -EdgePadding : EdgePadding);
      m_warped = true;
      status("Sharing this Mac's input with the paired Mac.");
    } else {
      m_position = CGPointMake(
          edge() + (m_rightEdge ? -EdgePadding : EdgePadding),
          CGRectGetMinY(m_bounds) + verticalPosition * (m_bounds.size.height - 1) / 65535.0
      );
      if (CGWarpMouseCursorPosition(m_position) != kCGErrorSuccess)
        return false;
      m_lastButtonDown.fill({});
      status("The paired Mac is controlling this Mac. Local input takes over.");
    }
    return true;
  }

  void restoreCursor()
  {
    if (m_warped && !IsSecureEventInputEnabled() && activeConsole() && activeDisplay() == m_display)
      CGWarpMouseCursorPosition(m_returnPoint);
    m_warped = false;
    m_recenter = false;
  }

  void releaseActions(CGEventTapProxy proxy)
  {
    if (m_inject && CGPreflightPostEventAccess()) {
      for (std::size_t button = 0; button < ButtonCount; ++button) {
        if (m_injectedButtons.test(button))
          postButton(button, false, proxy, false);
      }
      for (std::size_t key = 0; key < KeyCount; ++key) {
        if (m_injectedKeys.test(key))
          postKey(key, false, proxy, false);
      }
    }
  }

  void stopLease()
  {
    releaseActions(nullptr);
    restoreCursor();
    m_waitingForInjection = {};
  }

  void revoke(CGEventTapProxy proxy = nullptr)
  {
    m_blocked = true;
    m_exchange.release(m_epoch);
    if (proxy)
      releaseActions(proxy); // Releases precede the local event returned by the tap.
    else
      stopLease();
  }

  void finish(const char *message = nullptr)
  {
    m_blocked = true;
    m_exchange.cancelled.store(true, std::memory_order_relaxed);
    stopLease();
    if (message)
      status(message);
    status("Local input restored.");
    QCoreApplication::quit();
  }

  void requestAtEdge(CGEventRef event, int deltaX)
  {
    const auto time = Clock::now();
    if (!m_connected || m_owner != InputSide::None || localInputHeld() || (m_rightEdge ? deltaX <= 0 : deltaX >= 0) ||
        time < m_nextRequest) {
      return;
    }
    const auto point = CGEventGetLocation(event);
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !CGRectContainsPoint(m_bounds, point) ||
        std::abs(point.x - edge()) > 1) {
      return;
    }
    const auto position = (point.y - CGRectGetMinY(m_bounds)) / (m_bounds.size.height - 1);
    m_exchange.request(m_epoch, static_cast<quint16>(std::lround(std::clamp(position, 0.0, 1.0) * 65535)));
    m_requestUntil = time + std::chrono::milliseconds(InputWatchdog);
    m_nextRequest = time + 250ms;
  }

  bool handleLocal(CGEventTapProxy proxy, CGEventType type, CGEventRef event)
  {
    switch (type) {
    case kCGEventMouseMoved:
    case kCGEventLeftMouseDragged:
    case kCGEventRightMouseDragged:
    case kCGEventOtherMouseDragged: {
      const auto dx = eventDelta(event, kCGMouseEventDeltaX, 8192);
      const auto dy = eventDelta(event, kCGMouseEventDeltaY, 8192);
      if (sending()) {
        m_recenter = true;
        return m_exchange.enqueue(m_epoch, {InputKind::Move, dx, dy});
      }
      if (peerOwnsLease()) {
        m_takeover += std::abs(dx) + std::abs(dy);
        if (m_takeover >= TakeoverDistance)
          revoke(proxy);
      } else if (type == kCGEventMouseMoved) {
        requestAtEdge(event, dx);
      }
      return false;
    }
    case kCGEventScrollWheel: {
      if (peerOwnsLease())
        revoke(proxy);
      if (!sending())
        return false;
      const auto horizontal = eventDelta(event, kCGScrollWheelEventDeltaAxis2, 32767);
      const auto vertical = eventDelta(event, kCGScrollWheelEventDeltaAxis1, 32767);
      return (!horizontal || m_exchange.enqueue(m_epoch, {InputKind::Wheel, horizontal, 0})) &&
             (!vertical || m_exchange.enqueue(m_epoch, {InputKind::Wheel, 0, vertical}));
    }
    case kCGEventKeyDown:
    case kCGEventKeyUp:
    case kCGEventFlagsChanged:
      return localKey(proxy, type, event);
    default:
      return localButton(proxy, type, event);
    }
  }

  bool localKey(CGEventTapProxy proxy, CGEventType type, CGEventRef event)
  {
    if (peerOwnsLease())
      revoke(proxy);
    const auto nativeKey = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    if (!supportedKey(nativeKey)) {
      if (sending())
        revoke(proxy);
      return false;
    }
    const auto key = static_cast<CGKeyCode>(nativeKey);
    bool down = type == kCGEventKeyDown;
    if (type == kCGEventFlagsChanged) {
      const auto *modifier = modifierFor(key);
      if (!modifier)
        return false;
      const auto flags = CGEventGetFlags(event);
      down = flags & modifier->groupMask ? (flags & modifier->deviceMask) != 0 : (flags & modifier->mask) != 0;
    }
    m_keys.set(key, down);
    constexpr auto emergency = kCGEventFlagMaskControl | kCGEventFlagMaskAlternate | kCGEventFlagMaskShift;
    if (key == kVK_F12 && down && (modifierFlags(m_keys) & emergency) == emergency) {
      if (m_owner != InputSide::None)
        revoke(proxy);
      m_exchange.cancelled.store(true, std::memory_order_relaxed);
      return true;
    }
    const bool swallowed = m_swallowedKeys.test(key);
    if (!down)
      m_swallowedKeys.reset(key);
    if (!sending())
      return swallowed;
    if (!m_exchange.enqueue(m_epoch, {InputKind::Key, static_cast<qint16>(key + 1), down ? qint16{1} : qint16{0}}))
      return false;
    m_swallowedKeys.set(key, down);
    return true;
  }

  bool localButton(CGEventTapProxy proxy, CGEventType type, CGEventRef event)
  {
    if (peerOwnsLease())
      revoke(proxy);
    const auto nativeButton = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
    if (nativeButton < 0 || nativeButton >= static_cast<std::int64_t>(ButtonCount)) {
      if (sending())
        revoke(proxy);
      return false;
    }
    const auto button = static_cast<std::size_t>(nativeButton);
    const bool down = type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown || type == kCGEventOtherMouseDown;
    m_buttons.set(button, down);
    const bool swallowed = m_swallowedButtons.test(button);
    if (!down)
      m_swallowedButtons.reset(button);
    if (!sending())
      return swallowed;
    if (!m_exchange.enqueue(
            m_epoch, {InputKind::Button, static_cast<qint16>(button + 1), down ? qint16{1} : qint16{0}}
        ))
      return false;
    m_swallowedButtons.set(button, down);
    return true;
  }

  void post(CGEventRef event, CGEventTapProxy proxy, bool probe)
  {
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, m_tag);
    CGEventSetFlags(event, modifierFlags(m_injectedKeys));
    if (proxy) {
      CGEventTapPostEvent(proxy, event);
    } else {
      if (probe && m_waitingForInjection == Clock::time_point{})
        m_waitingForInjection = Clock::now();
      CGEventPost(kCGSessionEventTap, event);
    }
  }

  bool postKey(std::size_t key, bool down, CGEventTapProxy proxy, bool probe)
  {
    const CFRef<CGEventRef> event(
        CGEventCreateKeyboardEvent(m_inject.get(), static_cast<CGKeyCode>(key), down), CFRelease
    );
    if (!event)
      return false;
    if (modifierFor(static_cast<CGKeyCode>(key)))
      CGEventSetType(event.get(), kCGEventFlagsChanged);
    else if (down && m_injectedKeys.test(key))
      CGEventSetIntegerValueField(event.get(), kCGKeyboardEventAutorepeat, 1);
    m_injectedKeys.set(key, down);
    post(event.get(), proxy, probe);
    return true;
  }

  bool postButton(std::size_t button, bool down, CGEventTapProxy proxy, bool probe)
  {
    constexpr std::array downType{kCGEventLeftMouseDown, kCGEventRightMouseDown, kCGEventOtherMouseDown};
    constexpr std::array upType{kCGEventLeftMouseUp, kCGEventRightMouseUp, kCGEventOtherMouseUp};
    const auto index = std::min(button, std::size_t{2});
    const CFRef<CGEventRef> event(
        CGEventCreateMouseEvent(
            m_inject.get(), down ? downType[index] : upType[index], m_position, static_cast<CGMouseButton>(button)
        ),
        CFRelease
    );
    if (!event)
      return false;
    if (down) {
      const auto now = Clock::now();
      const auto previous = m_lastButtonPosition[button];
      const bool close = std::abs(previous.x - m_position.x) <= 4 && std::abs(previous.y - m_position.y) <= 4;
      m_clickCount[button] = close && now - m_lastButtonDown[button] <= 500ms ? m_clickCount[button] % 3 + 1 : 1;
      m_lastButtonDown[button] = now;
      m_lastButtonPosition[button] = m_position;
    }
    m_injectedButtons.set(button, down);
    CGEventSetIntegerValueField(event.get(), kCGMouseEventButtonNumber, static_cast<std::int64_t>(button));
    CGEventSetIntegerValueField(event.get(), kCGMouseEventClickState, m_clickCount[button]);
    post(event.get(), proxy, probe);
    return true;
  }

  bool apply(const InputEvent &event)
  {
    if (event.kind == InputKind::Key) {
      const auto key = event.first - 1;
      if (event.second > 1 || !supportedKey(key))
        return false;
      return postKey(static_cast<std::size_t>(key), event.second == 1, nullptr, true);
    }
    if (event.kind == InputKind::Button)
      return postButton(static_cast<std::size_t>(event.first - 1), event.second == 1, nullptr, true);
    if (event.kind == InputKind::Wheel) {
      const CFRef<CGEventRef> wheel(
          CGEventCreateScrollWheelEvent(
              m_inject.get(), kCGScrollEventUnitLine, 2, static_cast<std::int32_t>(event.second),
              static_cast<std::int32_t>(event.first)
          ),
          CFRelease
      );
      if (!wheel)
        return false;
      post(wheel.get(), nullptr, true);
      return true;
    }

    const auto x = m_position.x + event.first;
    if (event.first && (m_rightEdge ? x >= edge() : x <= edge()) && !m_injectedButtons.any() && !m_injectedKeys.any())
      return false;
    m_position.x = std::clamp(x, CGRectGetMinX(m_bounds), CGRectGetMaxX(m_bounds) - 1);
    m_position.y = std::clamp(m_position.y + event.second, CGRectGetMinY(m_bounds), CGRectGetMaxY(m_bounds) - 1);
    constexpr std::array dragType{kCGEventLeftMouseDragged, kCGEventRightMouseDragged, kCGEventOtherMouseDragged};
    auto type = kCGEventMouseMoved;
    std::size_t button = 0;
    for (; button < ButtonCount; ++button) {
      if (!m_injectedButtons.test(button))
        continue;
      type = dragType[std::min(button, std::size_t{2})];
      break;
    }
    const CFRef<CGEventRef> move(
        CGEventCreateMouseEvent(
            m_inject.get(), type, m_position,
            button < ButtonCount ? static_cast<CGMouseButton>(button) : kCGMouseButtonLeft
        ),
        CFRelease
    );
    if (!move)
      return false;
    if (button < ButtonCount) {
      CGEventSetIntegerValueField(move.get(), kCGMouseEventButtonNumber, static_cast<std::int64_t>(button));
      CGEventSetIntegerValueField(move.get(), kCGMouseEventClickState, m_clickCount[button]);
    }
    CGEventSetIntegerValueField(move.get(), kCGMouseEventDeltaX, event.first);
    CGEventSetIntegerValueField(move.get(), kCGMouseEventDeltaY, event.second);
    post(move.get(), nullptr, true);
    return true;
  }

  InputExchange &m_exchange;
  const InputSide m_side;
  const bool m_rightEdge;
  const std::int64_t m_tag = static_cast<std::int64_t>(QRandomGenerator::system()->generate64() >> 2) + 1;
  CFRef<CFMachPortRef> m_tap{nullptr, CFRelease};
  CFRef<CFRunLoopSourceRef> m_source{nullptr, CFRelease};
  CFRef<CGEventSourceRef> m_inject{nullptr, CFRelease};
  CGDirectDisplayID m_display = kCGNullDirectDisplay;
  CGRect m_bounds{};
  CGPoint m_position{};
  CGPoint m_returnPoint{};
  std::bitset<KeyCount> m_keys;
  std::bitset<KeyCount> m_injectedKeys;
  std::bitset<KeyCount> m_swallowedKeys;
  std::bitset<ButtonCount> m_buttons;
  std::bitset<ButtonCount> m_injectedButtons;
  std::bitset<ButtonCount> m_swallowedButtons;
  std::array<int, ButtonCount> m_clickCount{};
  std::array<CGPoint, ButtonCount> m_lastButtonPosition{};
  std::array<Clock::time_point, ButtonCount> m_lastButtonDown{};
  Clock::time_point m_waitingForInjection{};
  Clock::time_point m_nextDesktopCheck{};
  Clock::time_point m_nextRequest{};
  Clock::time_point m_requestUntil{};
  InputSide m_owner = InputSide::None;
  quint32 m_epoch = 0;
  int m_takeover = 0;
  bool m_tapDisabled = false;
  bool m_connected = false;
  bool m_blocked = false;
  bool m_active = false;
  bool m_readied = false;
  bool m_warped = false;
  bool m_recenter = false;
};

} // namespace

int macInputCommand(int argc, char **argv)
{
  const QCoreApplication app(argc, argv);
  try {
    const auto options = parseInputOptions(
        app.arguments(),
        QStringLiteral(
            "Experimental bidirectional macOS control. Stop the classic core before starting.\n"
            "Emergency exit: Ctrl+Alt+Shift+F12.\n"
            "Usage: deskflow-core --enhanced-control <listen|connect> [options]"
        ),
        false
    );
    const InputSessionGuard guard;
    InputExchange exchange;
    MacInput input(exchange, options);
    input.start();

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::unique_ptr<QThread> network(QThread::create([&] {
      runInputNetwork(exchange, options, QByteArrayView("\x02\x04", 2));
    }));
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] { input.tick(); });
    timer.start(8);
    status("Waiting for a verified enhanced macOS peer.");
    network->start();
    QCoreApplication::exec();
    exchange.cancelled.store(true, std::memory_order_relaxed);
    network->wait();
    if (!exchange.error.isEmpty())
      throw Error(exchange.error);
    return 0;
  } catch (const std::exception &error) {
    QTextStream(stderr) << QString::fromUtf8(error.what()) << Qt::endl;
    return 1;
  }
}

} // namespace deskflow::enhanced
