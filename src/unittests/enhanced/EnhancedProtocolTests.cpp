/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "enhanced/FileManifest.h"
#include "enhanced/InputLease.h"
#include "enhanced/InputRouter.h"
#include "enhanced/Protocol.h"

#include <QtTest>

#include <array>

using namespace deskflow::enhanced;

class EnhancedProtocolTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void rejectsInvalidFrames()
  {
    const auto frame = encodeHeader(Message::Offer, 42, 1);
    QCOMPARE(decodeHeader(frame, 1).type, Message::Offer);
    QCOMPARE(decodeHeader(frame, 1).size, 42);
    QVERIFY_EXCEPTION_THROWN(decodeHeader(frame, 2), Error);
    QVERIFY_EXCEPTION_THROWN(decodeHeader(frame.first(4), 1), Error);
    auto badVersion = frame;
    badVersion[5] = 2;
    QVERIFY_EXCEPTION_THROWN(decodeHeader(badVersion, 1), Error);
    auto oversized = frame;
    oversized[12] = 1;
    QVERIFY_EXCEPTION_THROWN(decodeHeader(oversized, 1), Error);
  }

  void manifestPreservesUnicodeAndDirectories()
  {
    const FileManifest manifest{
        {{QStringLiteral("文件"), true, 0},
         {QStringLiteral("文件/空文件夹"), true, 0},
         {QStringLiteral("文件/表情😀.txt"), false, 42},
         {QStringLiteral("empty"), false, 0}}
    };
    const auto decoded = FileManifest::decode(manifest.encode());
    QCOMPARE(decoded.validate(), 42);
    QCOMPARE(decoded.entries.size(), 4);
    QCOMPARE(decoded.entries.at(2).path, manifest.entries.at(2).path);
  }

  void rejectsUnsafeManifest()
  {
    const FileManifest traversal{{{QStringLiteral("../outside"), false, 0}}};
    const FileManifest device{{{QStringLiteral("LPT¹.txt"), false, 0}}};
    const FileManifest duplicate{{{QStringLiteral("A"), true, 0}, {QStringLiteral("a"), false, 0}}};
    const FileManifest missingParent{{{QStringLiteral("a/b"), false, 0}}};
    const FileManifest overflow{{{QStringLiteral("large"), false, MaxTransferBytes + 1}}};
    QVERIFY_EXCEPTION_THROWN(traversal.encode(), Error);
    QVERIFY_EXCEPTION_THROWN(device.encode(), Error);
    QVERIFY_EXCEPTION_THROWN(duplicate.encode(), Error);
    QVERIFY_EXCEPTION_THROWN(missingParent.encode(), Error);
    QVERIFY_EXCEPTION_THROWN(overflow.encode(), Error);

    auto incomplete = FileManifest{{{QStringLiteral("a"), false, 1}}}.encode();
    incomplete.chop(1);
    QVERIFY_EXCEPTION_THROWN(FileManifest::decode(incomplete), Error);
  }

  void leaseAcceptsOnlyTheCurrentOriginAndEpoch()
  {
    InputState state;
    const InputPoll request{0, 100, true, false, {}};
    advanceLease(state, request, request);
    QCOMPARE(state.owner, InputSide::None);
    advanceLease(state, {}, request);
    QCOMPARE(state.owner, InputSide::Connector);
    QCOMPARE(state.epoch, 1);
    const InputPoll oldEpoch{0, 0, false, false, {{InputKind::Key, 30, 1}}};
    const InputPoll input{1, 0, false, false, {{InputKind::Key, 30, 1}}};
    const InputPoll ready{1, 0, false, false, {}, true};
    advanceLease(state, {}, input);
    QVERIFY(state.events.isEmpty());
    QVERIFY(!state.active);
    advanceLease(state, {}, ready);
    QVERIFY(!state.active);
    advanceLease(state, InputPoll::decode(ready.encode()), input);
    QVERIFY(state.active);
    QVERIFY(state.events.isEmpty());
    advanceLease(state, input, oldEpoch);
    QVERIFY(state.events.isEmpty());
    advanceLease(state, {}, InputPoll::decode(input.encode()));
    QCOMPARE(state.events.size(), 1);
    advanceLease(state, {1, 0, false, true, {}}, input);
    QCOMPARE(state.owner, InputSide::None);
    QVERIFY(!state.active);
    QVERIFY(state.events.isEmpty());
    advanceLease(state, request, {});
    QCOMPARE(state.owner, InputSide::None);
    advanceLease(state, {1, 0, true, false, {}}, {});
    QCOMPARE(state.owner, InputSide::Listener);
    QCOMPARE(state.epoch, 2);
    QVERIFY(!state.active);
  }

  void connectorRequiresValidReadyAndGrantTransitions()
  {
    const InputState initial;
    const InputPoll request{0, 100, true, false, {}};
    const InputState localGrant{InputSide::Connector, 1, 100, {}};
    validateLeaseTransition(initial, InputState::decode(localGrant.encode()), request);
    QVERIFY_EXCEPTION_THROWN(validateLeaseTransition(initial, localGrant, {}), Error);
    QVERIFY_EXCEPTION_THROWN(validateLeaseTransition(initial, {InputSide::Connector, 2, 100, {}}, request), Error);
    QVERIFY_EXCEPTION_THROWN(validateLeaseTransition(initial, {InputSide::Connector, 1, 99, {}}, request), Error);
    const InputState offer{InputSide::Listener, 1, 250, {}};
    validateLeaseTransition(initial, offer, {});
    const InputState active{InputSide::Listener, 1, 250, {}, true};
    QVERIFY_EXCEPTION_THROWN(validateLeaseTransition(offer, active, {}), Error);
    validateLeaseTransition(offer, InputState::decode(active.encode()), {1, 0, false, false, {}, true});
    QVERIFY_EXCEPTION_THROWN(validateLeaseTransition(active, offer, {}), Error);
    QVERIFY_EXCEPTION_THROWN(validateLeaseTransition(active, localGrant, {}), Error);

    const InputState packet{InputSide::Listener, 1, 250, {{InputKind::Key, 29, 1}}, true};
    validateLeaseTransition(active, InputState::decode(packet.encode()), {});
    QVERIFY_EXCEPTION_THROWN(validateLeaseTransition(offer, packet, {}), Error);
  }

  void threeDeviceRoutesOnlyAcceptAuthorizedParticipants()
  {
    constexpr InputPeerId a = 1, b = 2, c = 3;
    const std::array routes{InputRoute{a, b}, InputRoute{b, a}, InputRoute{b, c}, InputRoute{c, b}};
    RoutedInputLease lease;
    advanceInputRoute(lease, routes, std::array{RoutedInputPoll{a, c, {0, 0, true, false, {}}}});
    QVERIFY(!lease.route.valid());

    advanceInputRoute(lease, routes, std::array{RoutedInputPoll{a, b, {0, 100, true, false, {}}}});
    QCOMPARE(lease.route.origin, a);
    QCOMPARE(lease.route.target, b);
    QCOMPARE(lease.epoch, 1);
    QVERIFY(!lease.active);

    advanceInputRoute(lease, routes, std::array{RoutedInputPoll{c, 0, {1, 0, false, false, {}, true}}});
    QVERIFY(!lease.active);
    advanceInputRoute(lease, routes, std::array{RoutedInputPoll{b, 0, {1, 0, false, false, {}, true}}});
    QVERIFY(lease.active);

    const InputPoll movement{1, 0, false, false, {{InputKind::Move, 3, 5}}};
    const InputPoll release{1, 0, false, true, {}};
    const std::array samples{RoutedInputPoll{c, b, release}, RoutedInputPoll{a, b, movement}};
    advanceInputRoute(lease, routes, samples);
    QCOMPARE(lease.events.size(), 1);
    QVERIFY(lease.active);

    advanceInputRoute(lease, routes, std::array{RoutedInputPoll{b, 0, release}});
    QVERIFY(!lease.route.valid());
    QVERIFY(!lease.active);
    advanceInputRoute(lease, routes, std::array{RoutedInputPoll{b, c, {1, 300, true, false, {}}}});
    QCOMPARE(lease.route.origin, b);
    QCOMPARE(lease.route.target, c);
    QCOMPARE(lease.epoch, 2);

    const std::array withoutC{InputRoute{a, b}, InputRoute{b, a}};
    advanceInputRoute(lease, withoutC, {});
    QVERIFY(!lease.route.valid());
    QVERIFY(lease.events.isEmpty());
  }

  void simultaneousRequestsAcrossThreeDevicesDoNotAcquireTheLease()
  {
    const std::array routes{InputRoute{1, 2}, InputRoute{2, 3}};
    const InputPoll request{0, 100, true, false, {}};
    const std::array polls{RoutedInputPoll{1, 2, request}, RoutedInputPoll{2, 3, request}};
    RoutedInputLease lease;
    advanceInputRoute(lease, routes, polls);
    QVERIFY(!lease.route.valid());
    QCOMPARE(lease.epoch, 0);
  }

  void movementCoalescesWithoutReorderingPressedInputs()
  {
    QList<InputEvent> pending;
    QVERIFY(queueInputEvent(pending, {InputKind::Move, 0, 0}));
    QVERIFY(pending.isEmpty());
    QVERIFY(queueInputEvent(pending, {InputKind::Move, 8000, -8000}));
    QVERIFY(queueInputEvent(pending, {InputKind::Move, 500, -500}));
    QVERIFY(queueInputEvent(pending, {InputKind::Key, 29, 1}));
    QVERIFY(queueInputEvent(pending, {InputKind::Move, -90, 100}));
    QVERIFY(queueInputEvent(pending, {InputKind::Key, 29, 0}));
    QCOMPARE(pending.size(), 4);
    QCOMPARE(pending.at(0).first, 8192);
    QCOMPARE(pending.at(0).second, -8192);
    QCOMPARE(pending.at(1).kind, InputKind::Key);
    QCOMPARE(pending.at(1).second, 1);
    QCOMPARE(pending.at(2).kind, InputKind::Move);
    QCOMPARE(pending.at(3).second, 0);

    while (pending.size() < MaxInputEvents)
      QVERIFY(queueInputEvent(pending, {InputKind::Button, 1, 0}));
    QVERIFY(!queueInputEvent(pending, {InputKind::Key, 29, 0}));
  }
};

QTEST_GUILESS_MAIN(EnhancedProtocolTests)
#include "EnhancedProtocolTests.moc"
