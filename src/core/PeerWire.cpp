#include "core/PeerWire.hpp"

#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace burlak::core
{

    namespace
    {

        struct HelloHeader
        {
            std::uint64_t tool{};
            std::uint64_t host{};
            std::uint64_t lastFocus{};
            std::uint32_t version{};
            std::uint32_t process{};
        };

        struct DropHeader
        {
            std::uint32_t version{};
            std::uint32_t size{};
            std::uint32_t pathCount{};
            std::uint32_t effect{};
            std::int32_t x{};
            std::int32_t y{};
        };

        static_assert(std::is_trivially_copyable_v<HelloHeader> && sizeof(HelloHeader) == 32);
        static_assert(std::is_trivially_copyable_v<DropHeader> && sizeof(DropHeader) == peerDropHeaderSize);
        static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));

        template <typename Header, bool Exact> std::optional<Header> readHeader(std::span<const std::byte> bytes)
        {
            if constexpr (Exact) {
                if (bytes.size() != sizeof(Header)) {
                    return std::nullopt;
                }
            } else {
                if (bytes.size() < sizeof(Header)) {
                    return std::nullopt;
                }
            }
            Header header{};
            std::memcpy(&header, bytes.data(), sizeof(header));
            return header;
        }

        template <typename Header> std::vector<std::byte> headerBytes(const Header &header)
        {
            std::vector<std::byte> bytes(sizeof(header));
            std::memcpy(bytes.data(), &header, sizeof(header));
            return bytes;
        }

        [[nodiscard]] bool separator(wchar_t value)
        {
            return value == L'\\' || value == L'/';
        }

        [[nodiscard]] bool absolutePath(std::wstring_view path)
        {
            const bool drive = path.size() >= 3 &&
                               ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
                               path[1] == L':' && separator(path[2]);
            if (drive) {
                return true;
            }
            if (path.size() < 5 || !separator(path[0]) || !separator(path[1]) || separator(path[2])) {
                return false;
            }
            const auto share = path.find_first_of(L"\\/", 2);
            return share != std::wstring_view::npos && share + 1 < path.size() && !separator(path[share + 1]);
        }

        [[nodiscard]] bool safePath(std::wstring_view path)
        {
            if (!absolutePath(path)) {
                return false;
            }
            std::size_t begin{};
            while (begin < path.size()) {
                while (begin < path.size() && separator(path[begin])) {
                    ++begin;
                }
                const auto end = path.find_first_of(L"\\/", begin);
                const auto component =
                    path.substr(begin, end == std::wstring_view::npos ? path.size() - begin : end - begin);
                if (component == L"..") {
                    return false;
                }
                begin = end == std::wstring_view::npos ? path.size() : end + 1;
            }
            return true;
        }

        [[nodiscard]] bool nativeValue(std::uint64_t value)
        {
            if constexpr (sizeof(NativeWindow) < sizeof(value)) {
                return value != 0 && value <= std::numeric_limits<NativeWindow>::max();
            } else {
                return value != 0;
            }
        }

    } // namespace

    std::vector<std::byte> encodePeerHello(const PeerHello &hello)
    {
        return headerBytes(HelloHeader{.tool = static_cast<std::uint64_t>(hello.tool),
                                       .host = static_cast<std::uint64_t>(hello.host),
                                       .lastFocus = hello.lastFocus,
                                       .version = peerProtocolVersion,
                                       .process = hello.process});
    }

    std::optional<PeerHello> decodePeerHello(std::span<const std::byte> bytes)
    {
        const auto header = readHeader<HelloHeader, true>(bytes);
        if (!header || header->version != peerProtocolVersion || header->process == 0 || !nativeValue(header->tool) ||
            !nativeValue(header->host)) {
            return std::nullopt;
        }
        return PeerHello{.process = header->process,
                         .tool = static_cast<NativeWindow>(header->tool),
                         .host = static_cast<NativeWindow>(header->host),
                         .lastFocus = header->lastFocus};
    }

    std::vector<std::byte> encodePeerDrop(const Drop &drop)
    {
        std::size_t characters{};
        for (const auto &path : drop.paths) {
            characters += path.size() + 1;
        }
        const auto size = peerDropHeaderSize + characters * sizeof(wchar_t);
        auto bytes = headerBytes(DropHeader{.version = peerProtocolVersion,
                                            .size = static_cast<std::uint32_t>(size),
                                            .pathCount = static_cast<std::uint32_t>(drop.paths.size()),
                                            .effect = static_cast<std::uint32_t>(drop.effect),
                                            .x = drop.at.x,
                                            .y = drop.at.y});
        bytes.resize(size);
        auto offset = peerDropHeaderSize;
        for (const auto &path : drop.paths) {
            const auto pathBytes = path.size() * sizeof(wchar_t);
            std::memcpy(bytes.data() + offset, path.data(), pathBytes);
            offset += pathBytes + sizeof(wchar_t);
        }
        return bytes;
    }

    std::optional<Drop> decodePeerDrop(std::span<const std::byte> bytes)
    {
        const auto header = readHeader<DropHeader, false>(bytes);
        if (!header || header->version != peerProtocolVersion || header->size != bytes.size() ||
            header->pathCount == 0 ||
            (header->effect != static_cast<std::uint32_t>(Effect::Copy) &&
             header->effect != static_cast<std::uint32_t>(Effect::Move))) {
            return std::nullopt;
        }
        const auto textBytes = bytes.size() - peerDropHeaderSize;
        if (textBytes == 0 || textBytes % sizeof(wchar_t) != 0) {
            return std::nullopt;
        }
        std::wstring text(textBytes / sizeof(wchar_t), L'\0');
        std::memcpy(text.data(), bytes.data() + peerDropHeaderSize, textBytes);
        if (text.back() != L'\0') {
            return std::nullopt;
        }

        std::vector<std::wstring> paths;
        std::size_t begin{};
        while (begin < text.size()) {
            const auto end = text.find(L'\0', begin);
            if (end == begin) {
                return std::nullopt;
            }
            std::wstring path{text.substr(begin, end - begin)};
            if (!safePath(path)) {
                return std::nullopt;
            }
            paths.push_back(std::move(path));
            begin = end + 1;
        }
        if (paths.size() != header->pathCount) {
            return std::nullopt;
        }
        return Drop{
            .paths = std::move(paths), .at = {header->x, header->y}, .effect = static_cast<Effect>(header->effect)};
    }

} // namespace burlak::core
