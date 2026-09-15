#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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

    struct AllowedEffects
    {
        bool copy{};
        bool move{};

        auto operator<=>(const AllowedEffects &) const = default;
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

        bool operator==(const PanelInfo &) const = default;
    };

    // FCTL_GETPANELDIRECTORY as Far reports it: the directory shown, and for a plugin panel the host file it was
    // opened from (Arclite: the archive path). Together with the plugin handle and owner they identify the panel.
    struct PanelDirectory
    {
        std::wstring name;
        std::wstring file;

        bool operator==(const PanelDirectory &) const = default;
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

        bool operator==(const HostWindow &) const = default;
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

    struct ExternalDragFacts
    {
        bool buttonDown{};
        NativeWindow pressRoot{};
        NativeWindow pointRoot{};
        NativeWindow host{};
        NativeWindow console{};
        NativeWindow tool{};
        std::optional<std::uint32_t> receiver;
        bool receiverAlive{true};
        std::uint32_t process{};
        bool ownDragActive{};

        auto operator<=>(const ExternalDragFacts &) const = default;
    };

    struct ReceiveSnapshot
    {
        bool panelsWindow{};
        std::array<std::optional<PanelInfo>, 2> panels{};
        std::array<std::optional<std::wstring>, 2> directories{};
        std::optional<HostWindow> host;
        std::optional<CellGeometry> geometry;

        bool operator==(const ReceiveSnapshot &) const = default;
    };

    struct ReceiveDestination
    {
        PanelSide side{PanelSide::Active};
        std::wstring directory;

        bool operator==(const ReceiveDestination &) const = default;
    };

    struct ReceiveDropOutcome
    {
        Effect returnedEffect{Effect::None};
        bool setPerformedNone{};

        auto operator<=>(const ReceiveDropOutcome &) const = default;
    };

    enum class DropMenuChoice : std::uint8_t
    {
        Copy,
        Move,
        Cancel
    };

    struct PluginModule
    {
        std::wstring path;
        PluginInstance instance{};

        bool operator==(const PluginModule &) const = default;
    };

    enum class Error : std::uint8_t
    {
        Unavailable,
        PanelUnavailable,
        NoRealNames,
        DirectoryUnavailable,
        NoSelection,
        ForeignCallFailed,
        ForeignCallCrashed
    };

} // namespace burlak::core
