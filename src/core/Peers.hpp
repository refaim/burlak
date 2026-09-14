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

    inline constexpr std::uint64_t peerExpiryTicks = 5000;

    class PeerRegistry final
    {
      public:
        void add(const PeerHello &hello, std::uint64_t now);
        void expire(std::uint64_t now);
        void clear();
        [[nodiscard]] std::optional<Peer> select(NativeWindow host, NativeWindow ownHost, std::uint64_t now);
        [[nodiscard]] std::size_t size() const;

      private:
        struct Entry
        {
            Peer peer;
            std::uint64_t seen{};
            std::uint64_t order{};
        };

        std::vector<Entry> entries_;
        std::uint64_t nextOrder_{};
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
