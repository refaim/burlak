#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace burlak::core
{

    struct Cell
    {
        int x{};
        int y{};

        auto operator<=>(const Cell &) const = default;
    };

    struct CellRect
    {
        int left{};
        int top{};
        int right{};
        int bottom{};

        auto operator<=>(const CellRect &) const = default;
    };

    struct Point
    {
        int x{};
        int y{};

        auto operator<=>(const Point &) const = default;
    };

    struct PixelRect
    {
        int left{};
        int top{};
        int right{};
        int bottom{};

        auto operator<=>(const PixelRect &) const = default;
    };

    enum class Button : std::uint8_t
    {
        Left,
        Right
    };

    struct Modifiers
    {
        bool shift{};
        bool control{};
        bool alt{};

        auto operator<=>(const Modifiers &) const = default;
    };

    struct MouseEvent
    {
        enum class Rewrite : std::uint8_t
        {
            Preserve,
            ButtonlessRelease,
            LeftHeldMove
        };

        Cell at{};
        bool left{};
        bool right{};
        bool moved{};
        bool wheel{};
        Modifiers mods{};
        std::uint32_t nativeButtonState{};
        std::uint32_t nativeControlState{};
        std::uint32_t nativeEventFlags{};
        Rewrite rewrite{Rewrite::Preserve};

        auto operator<=>(const MouseEvent &) const = default;
    };

    enum class Effect : std::uint8_t
    {
        None,
        Copy,
        Move,
        Link
    };

    enum class PanelSide : std::uint8_t
    {
        Active,
        Passive
    };

    struct DragStart
    {
        Button button{Button::Left};
        Cell press{};

        auto operator<=>(const DragStart &) const = default;
    };

    using PanelHandle = std::uintptr_t;
    using NativeWindow = std::uintptr_t;
    using PluginInstance = std::uintptr_t;

    using Guid = std::array<std::byte, 16>;

    struct UserData
    {
        std::uintptr_t value{};

        auto operator<=>(const UserData &) const = default;
    };

    struct PanelInfo
    {
        bool visible{};
        bool realNames{};
        bool plugin{};
        bool filePanel{};
        CellRect rect{};
        PanelHandle handle{};
        Guid owner{};
        std::size_t selectedItems{};
        std::size_t currentItem{};
        std::size_t topItem{};
    };

    struct Item
    {
        std::vector<std::byte> identity{};
        std::vector<std::byte> native{};
        std::wstring name;
        std::uint64_t size{};
        std::uintptr_t attributes{};
        bool directory{};
        bool selected{};
        UserData userData{};

        [[nodiscard]] bool operator==(const Item &other) const
        {
            return identity == other.identity && name == other.name && size == other.size &&
                   attributes == other.attributes && directory == other.directory && selected == other.selected &&
                   userData == other.userData;
        }
    };

    struct CellGeometry
    {
        Point origin{};
        int cellWidth{};
        int cellHeight{};

        constexpr bool operator==(const CellGeometry &) const = default;
    };

    struct HostWindow
    {
        NativeWindow handle{};
        PixelRect rect{};
        bool topmost{};
    };

    struct DropContext
    {
        Cell press{};
        PanelSide source{PanelSide::Active};
        std::array<std::optional<PanelInfo>, 2> panels{};
        std::optional<HostWindow> host;
        std::optional<CellGeometry> geometry;
        bool panelsWindow{};
        std::vector<std::wstring> sourcePaths;
        std::optional<std::wstring> destinationDirectory;
    };

    struct DropDecision
    {
        Effect effect{Effect::None};
        std::array<MouseEvent, 2> events{};
    };

    struct WindowPlacement
    {
        bool topmost{};
        bool demoteFirst{};

        constexpr bool operator==(const WindowPlacement &) const = default;
    };

    struct DragLoopOutcome
    {
        std::int32_t status{};
        std::uint32_t effect{};

        constexpr bool operator==(const DragLoopOutcome &) const = default;
    };

    struct ReplayOutcome
    {
        std::int32_t status{};
        std::uint32_t requested{};
        std::uint32_t written{};

        constexpr bool operator==(const ReplayOutcome &) const = default;
    };

    struct PluginModule
    {
        std::wstring path;
        PluginInstance instance{};

        bool operator==(const PluginModule &) const = default;
    };

    struct Peer
    {
        NativeWindow window{};
        NativeWindow host{};
        std::uint64_t lastFocus{};
        std::uint32_t process{};
        std::uint64_t nonce{};

        auto operator<=>(const Peer &) const = default;
    };

    struct PeerIdentity
    {
        NativeWindow window{};
        std::uint32_t process{};

        auto operator<=>(const PeerIdentity &) const = default;
    };

    enum class PeerAnnouncementAction : std::uint8_t
    {
        Begin,
        End
    };

    struct PeerAnnouncement
    {
        PeerAnnouncementAction action{PeerAnnouncementAction::Begin};
        PeerIdentity source{};
        std::uint64_t nonce{};

        auto operator<=>(const PeerAnnouncement &) const = default;
    };

    struct PeerHello
    {
        std::uint32_t process{};
        NativeWindow tool{};
        NativeWindow host{};
        std::uint64_t lastFocus{};
        std::uint64_t echoNonce{};
        std::uint64_t nonce{};

        auto operator<=>(const PeerHello &) const = default;
    };

    struct Drop
    {
        std::vector<std::wstring> paths;
        Point at{};
        Effect effect{Effect::None};
        std::uint64_t nonce{};

        auto operator<=>(const Drop &) const = default;
    };

    using PeerPayload = std::variant<PeerHello, Drop>;

    struct PeerEnvelope
    {
        PeerIdentity sender{};
        PeerPayload payload;

        auto operator<=>(const PeerEnvelope &) const = default;
    };

    struct PendingPeerDrop
    {
        Drop drop;
        std::uint32_t sourceProcess{};

        auto operator<=>(const PendingPeerDrop &) const = default;
    };

    struct AdoptedPeerPaths
    {
        std::vector<std::wstring> paths;
        std::optional<std::wstring> cleanupDirectory;

        auto operator<=>(const AdoptedPeerPaths &) const = default;
    };

    enum class Error : std::uint8_t
    {
        Unavailable,
        PanelUnavailable,
        NoRealNames,
        DirectoryUnavailable,
        NoSelection,
        ForeignCallFailed,
        ForeignCallCrashed,
        Indeterminate
    };

} // namespace burlak::core
