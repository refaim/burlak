#pragma once

#include "core/Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace burlak::core
{

    inline constexpr std::size_t peerRegistryLimit = 64;

    class PeerRegistry final
    {
      public:
        void begin(std::uint64_t nonce);
        void end();
        [[nodiscard]] bool add(const PeerHello &hello, PeerIdentity sender);
        [[nodiscard]] std::optional<Peer> select(NativeWindow host, NativeWindow ownHost) const;
        [[nodiscard]] std::size_t size() const;

      private:
        struct Entry
        {
            Peer peer;
            std::uint64_t order{};
        };

        std::vector<Entry> entries_;
        std::optional<std::uint64_t> nonce_;
        std::uint64_t nextOrder_{};
    };

    class IncomingPeerRegistry final
    {
      public:
        [[nodiscard]] bool begin(const PeerAnnouncement &announcement, std::uint64_t receiverNonce);
        void end(const PeerAnnouncement &announcement);
        [[nodiscard]] std::optional<PendingPeerDrop> accept(PeerIdentity sender, Drop drop);
        [[nodiscard]] std::size_t size() const;

      private:
        struct Entry
        {
            PeerIdentity source{};
            std::uint64_t sourceNonce{};
            std::uint64_t receiverNonce{};
        };

        std::vector<Entry> entries_;
    };

    enum class PeerMenuChoice : std::uint8_t
    {
        Cancel,
        Copy,
        Move
    };

    [[nodiscard]] Effect peerMenuEffect(PeerMenuChoice choice);

    enum class PeerDropRefusal : std::uint8_t
    {
        NotPanelsWindow,
        GeometryUnavailable,
        HostUnavailable,
        NotItemRow,
        NotFilePanel,
        NoRealNames,
        DirectoryUnavailable
    };

    struct PeerReceiveContext
    {
        bool panelsWindow{};
        std::array<std::optional<PanelInfo>, 2> panels{};
        std::array<std::optional<std::wstring>, 2> directories{};
        std::optional<CellGeometry> geometry;
    };

    struct PeerDestination
    {
        PanelSide side{PanelSide::Active};
        std::wstring directory;

        auto operator<=>(const PeerDestination &) const = default;
    };

    using PeerDestinationResult = std::expected<PeerDestination, PeerDropRefusal>;

    class PeerReceivePolicy final
    {
      public:
        [[nodiscard]] PeerDestinationResult destination(const PeerReceiveContext &context, Point point) const;
    };

} // namespace burlak::core
