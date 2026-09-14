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
            registry.add(PeerHello{.process = 1, .tool = 11, .host = 100, .lastFocus = 20}, 1'000);
            registry.add(PeerHello{.process = 2, .tool = 22, .host = 200, .lastFocus = 10}, 1'001);
            registry.add(PeerHello{.process = 3, .tool = 33, .host = 200, .lastFocus = 30}, 1'002);
            registry.add(PeerHello{.process = 4, .tool = 44, .host = 200, .lastFocus = 30}, 1'003);
            registry.add(PeerHello{.process = 5, .tool = 55, .host = 200, .lastFocus = 1}, 1'004);
            registry.add(PeerHello{.process = 5, .tool = 56, .host = 300, .lastFocus = 1}, 1'004);

            CHECK_FALSE(registry.select(100, 100, 1'004).has_value());
            REQUIRE(registry.select(200, 100, 1'004).has_value());
            CHECK(registry.select(200, 100, 1'004)->window == 44);

            registry.add(PeerHello{.process = 3, .tool = 33, .host = 200, .lastFocus = 40}, 1'005);
            REQUIRE(registry.select(200, 100, 1'006).has_value());
            CHECK(registry.select(200, 100, 1'006)->window == 33);
            CHECK_FALSE(registry.select(999, 100, 1'006).has_value());
        }

        TEST_CASE("registry replaces a peer hello and expires stale peers")
        {
            PeerRegistry registry;
            registry.add(PeerHello{.process = 7, .tool = 70, .host = 700, .lastFocus = 1}, 10);
            registry.expire(9);
            CHECK(registry.size() == 1);
            registry.add(PeerHello{.process = 7, .tool = 70, .host = 701, .lastFocus = 2}, 20);
            REQUIRE(registry.select(701, 0, 20).has_value());
            CHECK_FALSE(registry.select(700, 0, 20).has_value());
            CHECK(registry.size() == 1);

            registry.expire(20 + peerExpiryTicks);
            CHECK(registry.size() == 1);
            registry.expire(21 + peerExpiryTicks);
            CHECK(registry.size() == 0);
            registry.clear();
            CHECK(registry.size() == 0);
        }

        TEST_CASE("hello wire format round trips and rejects every malformed shape")
        {
            const PeerHello hello{.process = 42,
                                  .tool = static_cast<NativeWindow>(0x1234),
                                  .host = static_cast<NativeWindow>(0x5678),
                                  .lastFocus = 999};
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
                            .effect = Effect::Move};
            const auto encoded = encodePeerDrop(drop);
            CHECK(decodePeerDrop(encoded) == drop);
        }

        TEST_CASE("drop wire format rejects malformed headers and text")
        {
            const Drop drop{.paths = {L"C:\\one.txt"}, .at = {1, 2}, .effect = Effect::Copy};
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
                malformed = encodePeerDrop(Drop{.paths = {path}, .effect = Effect::Copy});
                CHECK_FALSE(decodePeerDrop(malformed).has_value());
            }

            malformed = encoded;
            malformed.insert(malformed.end() - 2, 2, std::byte{});
            overwrite(malformed, peerDropSizeOffset, static_cast<std::uint32_t>(malformed.size()));
            CHECK_FALSE(decodePeerDrop(malformed).has_value());
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
