#include "core/PeerWire.hpp"
#include "core/Peers.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <limits>

namespace burlak::core
{

    namespace
    {

        template <typename Value> void overwrite(std::vector<std::byte> &bytes, std::size_t offset, Value value)
        {
            REQUIRE(offset + sizeof(value) <= bytes.size());
            std::memcpy(bytes.data() + offset, &value, sizeof(value));
        }

        PeerReceiveContext receiveContext()
        {
            return {
                .panelsWindow = true,
                .panels = {PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {0, 0, 39, 24}},
                           PanelInfo{.visible = true, .realNames = true, .filePanel = true, .rect = {40, 0, 79, 24}}},
                .directories = {std::wstring{L"C:\\left"}, std::wstring{L"D:\\right"}},
                .geometry = CellGeometry{{100, 50}, 8, 16}};
        }

    } // namespace

    TEST_SUITE("peer protocol")
    {
        TEST_CASE("registry excludes the own host and selects focus then hello recency")
        {
            PeerRegistry registry;
            registry.begin(1000);
            CHECK(registry.add(
                PeerHello{.process = 1, .tool = 11, .host = 100, .lastFocus = 20, .echoNonce = 1000, .nonce = 101},
                PeerIdentity{.window = 11, .process = 1}));
            CHECK(registry.add(
                PeerHello{.process = 2, .tool = 22, .host = 200, .lastFocus = 10, .echoNonce = 1000, .nonce = 202},
                PeerIdentity{.window = 22, .process = 2}));
            CHECK(registry.add(
                PeerHello{.process = 3, .tool = 33, .host = 200, .lastFocus = 30, .echoNonce = 1000, .nonce = 303},
                PeerIdentity{.window = 33, .process = 3}));
            CHECK(registry.add(
                PeerHello{.process = 4, .tool = 44, .host = 200, .lastFocus = 30, .echoNonce = 1000, .nonce = 404},
                PeerIdentity{.window = 44, .process = 4}));

            CHECK_FALSE(registry.select(100, 100).has_value());
            REQUIRE(registry.select(200, 100).has_value());
            CHECK(registry.select(200, 100)->window == 44);
            CHECK(registry.select(200, 100)->nonce == 404);

            CHECK(registry.add(
                PeerHello{.process = 3, .tool = 33, .host = 200, .lastFocus = 40, .echoNonce = 1000, .nonce = 333},
                PeerIdentity{.window = 33, .process = 3}));
            CHECK(registry.add(
                PeerHello{.process = 5, .tool = 55, .host = 200, .lastFocus = 1, .echoNonce = 1000, .nonce = 505},
                PeerIdentity{.window = 55, .process = 5}));
            REQUIRE(registry.select(200, 100).has_value());
            CHECK(registry.select(200, 100)->window == 33);
            CHECK_FALSE(registry.select(999, 100).has_value());
        }

        TEST_CASE("registry accepts authenticated hellos only during discovery and stays valid until drag end")
        {
            PeerRegistry registry;
            const PeerIdentity sender{.window = 70, .process = 7};
            const PeerHello hello{.process = 7, .tool = 70, .host = 700, .lastFocus = 1, .echoNonce = 10, .nonce = 20};
            CHECK_FALSE(registry.add(hello, sender));
            registry.begin(0);
            CHECK_FALSE(registry.add(hello, sender));
            registry.begin(10);
            CHECK_FALSE(
                registry.add(PeerHello{hello.process, hello.tool, hello.host, hello.lastFocus, 11, 20}, sender));
            CHECK_FALSE(registry.add(PeerHello{hello.process, hello.tool, hello.host, hello.lastFocus, 10, 0}, sender));
            CHECK_FALSE(registry.add(hello, PeerIdentity{.window = 71, .process = 7}));
            CHECK_FALSE(registry.add(hello, PeerIdentity{.window = 70, .process = 8}));
            CHECK_FALSE(registry.add(hello, PeerIdentity{.window = 0, .process = 7}));
            CHECK_FALSE(registry.add(hello, PeerIdentity{.window = 70, .process = 0}));
            CHECK(registry.add(hello, sender));
            CHECK(registry.add(
                PeerHello{.process = 7, .tool = 70, .host = 701, .lastFocus = 2, .echoNonce = 10, .nonce = 21},
                sender));
            CHECK(registry.add(
                PeerHello{.process = 7, .tool = 72, .host = 702, .lastFocus = 3, .echoNonce = 10, .nonce = 22},
                PeerIdentity{.window = 72, .process = 7}));
            REQUIRE(registry.select(701, 0).has_value());
            CHECK_FALSE(registry.select(700, 0).has_value());
            CHECK(registry.size() == 2);

            registry.end();
            CHECK(registry.size() == 0);
            CHECK_FALSE(registry.select(701, 0).has_value());
            CHECK_FALSE(registry.add(hello, sender));
        }

        TEST_CASE("registry caps authenticated peers by replacing the oldest")
        {
            PeerRegistry registry;
            registry.begin(88);
            for (std::uint32_t process = 1; process <= peerRegistryLimit + 1; ++process) {
                const auto window = static_cast<NativeWindow>(100) + process;
                CHECK(registry.add(PeerHello{.process = process,
                                             .tool = window,
                                             .host = static_cast<NativeWindow>(200) + process,
                                             .echoNonce = 88,
                                             .nonce = 1000 + process},
                                   PeerIdentity{.window = window, .process = process}));
            }
            CHECK(registry.size() == peerRegistryLimit);
            CHECK_FALSE(registry.select(201, 0).has_value());
            CHECK(registry.select(200 + peerRegistryLimit + 1, 0).has_value());
        }

        TEST_CASE("incoming nonces are identity-bound, single-use, ended explicitly, and capped")
        {
            IncomingPeerRegistry incoming;
            const PeerAnnouncement source{
                .action = PeerAnnouncementAction::Begin, .source = {.window = 10, .process = 1}, .nonce = 100};
            CHECK_FALSE(incoming.begin(
                PeerAnnouncement{.action = PeerAnnouncementAction::End, .source = source.source, .nonce = source.nonce},
                200));
            CHECK_FALSE(incoming.begin(PeerAnnouncement{.action = PeerAnnouncementAction::Begin,
                                                        .source = {.window = 0, .process = 1},
                                                        .nonce = source.nonce},
                                       200));
            CHECK_FALSE(incoming.begin(PeerAnnouncement{.action = PeerAnnouncementAction::Begin,
                                                        .source = {.window = 10, .process = 0},
                                                        .nonce = source.nonce},
                                       200));
            CHECK_FALSE(incoming.begin(PeerAnnouncement{source.action, source.source, 0}, 200));
            CHECK_FALSE(incoming.begin(source, 0));
            CHECK(incoming.begin(source, 200));
            CHECK_FALSE(incoming
                            .accept(PeerIdentity{.window = 11, .process = 1},
                                    Drop{.paths = {}, .at = {}, .effect = Effect::None, .nonce = 200})
                            .has_value());
            CHECK_FALSE(
                incoming.accept(source.source, Drop{.paths = {}, .at = {}, .effect = Effect::None, .nonce = 201})
                    .has_value());
            const Drop accepted{.paths = {L"C:\\one.txt"}, .effect = Effect::Copy, .nonce = 200};
            CHECK(incoming.accept(source.source, accepted) ==
                  PendingPeerDrop{.drop = accepted, .sourceProcess = source.source.process});
            CHECK_FALSE(incoming.accept(source.source, accepted).has_value());

            CHECK(incoming.begin(source, 201));
            incoming.end(source);
            CHECK(incoming.size() == 1);
            incoming.end(PeerAnnouncement{
                .action = PeerAnnouncementAction::End, .source = source.source, .nonce = source.nonce + 1});
            CHECK(incoming.size() == 1);
            incoming.end(
                PeerAnnouncement{.action = PeerAnnouncementAction::End,
                                 .source = {.window = source.source.window + 1, .process = source.source.process + 1},
                                 .nonce = source.nonce});
            CHECK(incoming.size() == 1);
            incoming.end(PeerAnnouncement{
                .action = PeerAnnouncementAction::End, .source = source.source, .nonce = source.nonce});
            CHECK(incoming.size() == 0);

            for (std::uint32_t process = 1; process <= peerRegistryLimit + 1; ++process) {
                CHECK(incoming.begin(PeerAnnouncement{.action = PeerAnnouncementAction::Begin,
                                                      .source = {.window = 100 + process, .process = process},
                                                      .nonce = 1000 + process},
                                     2000 + process));
            }
            CHECK(incoming.size() == peerRegistryLimit);
            CHECK_FALSE(incoming
                            .accept(PeerIdentity{.window = 101, .process = 1},
                                    Drop{.paths = {}, .at = {}, .effect = Effect::None, .nonce = 2001})
                            .has_value());
            CHECK(
                incoming
                    .accept(PeerIdentity{.window = 100 + peerRegistryLimit + 1, .process = peerRegistryLimit + 1},
                            Drop{.paths = {}, .at = {}, .effect = Effect::None, .nonce = 2000 + peerRegistryLimit + 1})
                    .has_value());
        }

        TEST_CASE("hello wire format round trips and rejects every malformed shape")
        {
            const PeerHello hello{.process = 42,
                                  .tool = static_cast<NativeWindow>(0x1234),
                                  .host = static_cast<NativeWindow>(0x5678),
                                  .lastFocus = 999,
                                  .echoNonce = 111,
                                  .nonce = 222};
            const auto encoded = encodePeerHello(hello);
            CHECK(decodePeerHello(encoded) == hello);

            auto malformed = encoded;
            malformed.pop_back();
            CHECK_FALSE(decodePeerHello(malformed).has_value());
            malformed = encoded;
            malformed.push_back(std::byte{});
            CHECK_FALSE(decodePeerHello(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerHelloVersionOffset, peerProtocolVersion + 1U);
            CHECK_FALSE(decodePeerHello(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerHelloProcessOffset, 0U);
            CHECK_FALSE(decodePeerHello(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerHelloToolOffset, std::uint64_t{});
            CHECK_FALSE(decodePeerHello(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerHelloHostOffset, std::uint64_t{});
            CHECK_FALSE(decodePeerHello(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerHelloEchoOffset, std::uint64_t{});
            CHECK_FALSE(decodePeerHello(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerHelloNonceOffset, std::uint64_t{});
            CHECK_FALSE(decodePeerHello(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerHelloToolOffset, std::numeric_limits<std::uint64_t>::max());
            if constexpr (sizeof(NativeWindow) < sizeof(std::uint64_t)) {
                CHECK_FALSE(decodePeerHello(malformed).has_value());
            } else {
                CHECK(decodePeerHello(malformed).has_value());
            }
        }

        TEST_CASE("drop wire format round trips multiple absolute paths")
        {
            const Drop drop{.paths = {L"C:\\one.txt", L"c:\\lower.txt", L"D:/folder/two.bin",
                                      L"\\\\server\\share\\three", L"C:\\folder\\", L"C:\\\\"},
                            .at = {-12, 345},
                            .effect = Effect::Move,
                            .nonce = 333};
            const auto encoded = encodePeerDrop(drop);
            CHECK(decodePeerDrop(encoded) == drop);
        }

        TEST_CASE("drop wire format rejects malformed headers and text")
        {
            const Drop drop{.paths = {L"C:\\one.txt"}, .at = {1, 2}, .effect = Effect::Copy, .nonce = 333};
            const auto encoded = encodePeerDrop(drop);
            auto malformed = encoded;
            malformed.resize(peerDropHeaderSize - 1);
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerDropVersionOffset, peerProtocolVersion + 1U);
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerDropSizeOffset, static_cast<std::uint32_t>(malformed.size() + 2));
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerDropCountOffset, 0U);
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerDropCountOffset, 2U);
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerDropEffectOffset, static_cast<std::uint32_t>(Effect::None));
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerDropEffectOffset, static_cast<std::uint32_t>(Effect::Link));
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerDropNonceOffset, std::uint64_t{});
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerDropCountOffset, static_cast<std::uint32_t>(peerDropMaximumPaths + 1));
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            malformed.pop_back();
            overwrite(malformed, peerDropSizeOffset, static_cast<std::uint32_t>(malformed.size()));
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            malformed.resize(malformed.size() - sizeof(wchar_t));
            overwrite(malformed, peerDropSizeOffset, static_cast<std::uint32_t>(malformed.size()));
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            malformed.resize(peerDropHeaderSize);
            overwrite(malformed, peerDropSizeOffset, static_cast<std::uint32_t>(malformed.size()));
            CHECK_FALSE(decodePeerDrop(malformed).has_value());

            for (const std::wstring path :
                 {L"relative.txt", L"x", L"C:relative.txt", L"1:\\file.txt", L"{:\\file.txt", L"C:\\a\\..\\b.txt",
                  L"\\\\server", L"\\server", L"\\\\server\\", L"\\\\server\\\\file.txt", LR"(\\\bad)", L""}) {
                malformed = encodePeerDrop(Drop{.paths = {path}, .effect = Effect::Copy, .nonce = 1});
                CHECK_FALSE(decodePeerDrop(malformed).has_value());
            }

            malformed = encoded;
            malformed.insert(malformed.end() - 2, 2, std::byte{});
            overwrite(malformed, peerDropSizeOffset, static_cast<std::uint32_t>(malformed.size()));
            CHECK_FALSE(decodePeerDrop(malformed).has_value());

            malformed.assign(peerDropMaximumBytes + 1, std::byte{});
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
            malformed = encoded;
            overwrite(malformed, peerDropSizeOffset, static_cast<std::uint32_t>(peerDropMaximumBytes + 1));
            CHECK_FALSE(decodePeerDrop(malformed).has_value());

            CHECK(encodePeerDrop(Drop{.paths = {}, .at = {}, .effect = Effect::Copy, .nonce = 1}).empty());
            CHECK(encodePeerDrop(Drop{.paths = {L"C:\\x"}, .effect = Effect::Copy, .nonce = 0}).empty());
            CHECK(encodePeerDrop(Drop{.paths = std::vector<std::wstring>(peerDropMaximumPaths + 1, L"C:\\x"),
                                      .effect = Effect::Copy,
                                      .nonce = 1})
                      .empty());
            CHECK(encodePeerDrop(
                      Drop{.paths = {std::wstring(peerDropMaximumBytes, L'x')}, .effect = Effect::Copy, .nonce = 1})
                      .empty());
        }

        TEST_CASE("menu choices map only copy and move to effects")
        {
            CHECK(peerMenuEffect(PeerMenuChoice::Copy) == Effect::Copy);
            CHECK(peerMenuEffect(PeerMenuChoice::Move) == Effect::Move);
            CHECK(peerMenuEffect(PeerMenuChoice::Cancel) == Effect::None);
        }

        TEST_CASE("receive policy maps an item row to either real-names file panel")
        {
            PeerReceivePolicy policy;
            auto context = receiveContext();
            CHECK(policy.destination(context, {140, 130}) ==
                  PeerDestination{.side = PanelSide::Active, .directory = L"C:\\left"});
            CHECK(policy.destination(context, {460, 130}) ==
                  PeerDestination{.side = PanelSide::Passive, .directory = L"D:\\right"});
        }

        TEST_CASE("receive policy reports every refusal")
        {
            PeerReceivePolicy policy;
            auto context = receiveContext();
            context.panelsWindow = false;
            CHECK(policy.destination(context, {140, 130}) == std::unexpected(PeerDropRefusal::NotPanelsWindow));
            context = receiveContext();
            context.geometry.reset();
            CHECK(policy.destination(context, {140, 130}) == std::unexpected(PeerDropRefusal::GeometryUnavailable));
            context = receiveContext();
            CHECK(policy.destination(context, {100, 50}) == std::unexpected(PeerDropRefusal::NotItemRow));
            context.panels[0].reset();
            CHECK(policy.destination(context, {140, 130}) == std::unexpected(PeerDropRefusal::NotItemRow));
            context = receiveContext();
            context.panels[0]->visible = false;
            CHECK(policy.destination(context, {140, 130}) == std::unexpected(PeerDropRefusal::NotItemRow));
            context = receiveContext();
            context.panels[0]->filePanel = false;
            CHECK(policy.destination(context, {140, 130}) == std::unexpected(PeerDropRefusal::NotFilePanel));
            context = receiveContext();
            context.panels[0]->realNames = false;
            CHECK(policy.destination(context, {140, 130}) == std::unexpected(PeerDropRefusal::NoRealNames));
            context = receiveContext();
            context.directories[0].reset();
            CHECK(policy.destination(context, {140, 130}) == std::unexpected(PeerDropRefusal::DirectoryUnavailable));
            context.directories[0] = L"";
            CHECK(policy.destination(context, {140, 130}) == std::unexpected(PeerDropRefusal::DirectoryUnavailable));
        }
    }

} // namespace burlak::core
