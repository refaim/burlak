#pragma once

#include "core/Types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace burlak::core
{

    inline constexpr std::uint32_t peerProtocolVersion = 2;
    inline constexpr std::size_t peerDropMaximumBytes = std::size_t{1024} * 1024U;
    inline constexpr std::size_t peerDropMaximumPaths = 4096;
    inline constexpr std::size_t peerHelloToolOffset = 0;
    inline constexpr std::size_t peerHelloHostOffset = 8;
    inline constexpr std::size_t peerHelloEchoOffset = 24;
    inline constexpr std::size_t peerHelloNonceOffset = 32;
    inline constexpr std::size_t peerHelloVersionOffset = 40;
    inline constexpr std::size_t peerHelloProcessOffset = 44;
    inline constexpr std::size_t peerDropVersionOffset = 0;
    inline constexpr std::size_t peerDropSizeOffset = 4;
    inline constexpr std::size_t peerDropCountOffset = 8;
    inline constexpr std::size_t peerDropEffectOffset = 12;
    inline constexpr std::size_t peerDropNonceOffset = 24;
    inline constexpr std::size_t peerDropHeaderSize = 32;

    [[nodiscard]] std::vector<std::byte> encodePeerHello(const PeerHello &hello);
    [[nodiscard]] std::optional<PeerHello> decodePeerHello(std::span<const std::byte> bytes);
    [[nodiscard]] std::vector<std::byte> encodePeerDrop(const Drop &drop);
    [[nodiscard]] std::optional<Drop> decodePeerDrop(std::span<const std::byte> bytes);

} // namespace burlak::core
