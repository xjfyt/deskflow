/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "InputLease.h"

#include <span>

namespace deskflow::enhanced {

// The coordinator assigns these IDs after authentication. They are not accepted from input frames.
using InputPeerId = quint16;

struct InputRoute
{
  InputPeerId origin = 0;
  InputPeerId target = 0;

  bool valid() const;
  bool operator==(const InputRoute &) const = default;
};

struct RoutedInputPoll
{
  InputPeerId peer;
  InputPeerId requestedTarget;
  InputPoll poll;
};

struct RoutedInputLease
{
  InputRoute route;
  quint32 epoch = 0;
  quint16 verticalPosition = 0;
  QList<InputEvent> events;
  bool active = false;
};

// Allows one source on the virtual desktop. Remove disconnected devices from allowedRoutes before advancing.
void advanceInputRoute(
    RoutedInputLease &lease, std::span<const InputRoute> allowedRoutes, std::span<const RoutedInputPoll> polls
);

} // namespace deskflow::enhanced
