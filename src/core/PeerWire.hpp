#pragma once

#include "core/Types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace burlak::core
{

    inline constexpr std::uint32_t peerProtocolVersion = 1;
    inline constexpr std::size_t peerHelloToolOffset = 0;
    inline constexpr std::size_t peerHelloHostOffset = 8;
    inline constexpr std::size_t peerHelloVersionOffset = 24;
    inline constexpr std::size_t peerHelloProcessOffset = 28;
    inline constexpr std::size_t peerDropVersionOffset = 0;
    inline constexpr std::size_t peerDropSizeOffset = 4;
    inline constexpr std::size_t peerDropCountOffset = 8;
    inline constexpr std::size_t peerDropEffectOffset = 12;
    inline constexpr std::size_t peerDropHeaderSize = 24;

    [[nodiscard]] std::vector<std::byte> encodePeerHello(const PeerHello &hello);
    [[nodiscard]] std::optional<PeerHello> decodePeerHello(std::span<const std::byte> bytes);
    [[nodiscard]] std::vector<std::byte> encodePeerDrop(const Drop &drop);
    [[nodiscard]] std::optional<Drop> decodePeerDrop(std::span<const std::byte> bytes);

} // namespace burlak::core
