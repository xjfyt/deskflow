/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "InputRouter.h"

#include "Protocol.h"

#include <algorithm>
#include <limits>

namespace deskflow::enhanced {
namespace {

bool isAllowed(InputRoute route, std::span<const InputRoute> allowedRoutes)
{
  return route.valid() && std::ranges::find(allowedRoutes, route) != allowedRoutes.end();
}

void clearRoute(RoutedInputLease &lease)
{
  lease.route = {};
  lease.active = false;
}

} // namespace

bool InputRoute::valid() const
{
  return origin != 0 && target != 0 && origin != target;
}

void advanceInputRoute(
    RoutedInputLease &lease, std::span<const InputRoute> allowedRoutes, std::span<const RoutedInputPoll> polls
)
{
  lease.events.clear();
  if (lease.route.valid()) {
    if (!isAllowed(lease.route, allowedRoutes)) {
      clearRoute(lease);
      return;
    }

    const InputPoll *origin = nullptr;
    const InputPoll *target = nullptr;
    for (const auto &sample : polls) {
      if (sample.poll.epoch != lease.epoch)
        continue;
      if (sample.peer == lease.route.origin)
        origin = &sample.poll;
      else if (sample.peer == lease.route.target)
        target = &sample.poll;
    }
    if ((origin && origin->release) || (target && target->release)) {
      clearRoute(lease);
    } else if (!lease.active) {
      lease.active = target && target->ready;
    } else if (origin) {
      lease.events = origin->events;
    }
    return;
  }

  const RoutedInputPoll *candidate = nullptr;
  InputRoute route;
  for (const auto &sample : polls) {
    const InputRoute request{sample.peer, sample.requestedTarget};
    if (!sample.poll.request || sample.poll.epoch != lease.epoch || !isAllowed(request, allowedRoutes))
      continue;
    if (candidate)
      return;
    candidate = &sample;
    route = request;
  }
  if (!candidate)
    return;
  if (lease.epoch == std::numeric_limits<quint32>::max())
    throw Error(QStringLiteral("Enhanced input lease sequence exhausted."));

  ++lease.epoch;
  lease.route = route;
  lease.verticalPosition = candidate->poll.verticalPosition;
}

} // namespace deskflow::enhanced
