#pragma once

#include "core/Types.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace burlak::core
{

    class IPanels
    {
      public:
        [[nodiscard]] virtual std::optional<PanelInfo> panel(PanelSide side) = 0;
        [[nodiscard]] virtual std::vector<Item> selectedItems(PanelSide side) = 0;
        [[nodiscard]] virtual std::optional<std::wstring> directory(PanelSide side) = 0;
        [[nodiscard]] virtual bool currentWindowIsPanels() = 0;
        virtual void updateAndRedraw(PanelSide side) = 0;
    };

    class IFarHost
    {
      public:
        virtual void postSynchro() = 0;
        virtual void message(std::wstring_view title, std::span<const std::wstring> lines) = 0;
        [[nodiscard]] virtual std::optional<PluginModule> pluginModule(const Guid &guid) = 0;
        [[nodiscard]] virtual std::expected<void, Error> extract(PanelHandle panel, std::span<const Item> items,
                                                                 const PluginModule &module,
                                                                 std::wstring_view destination) = 0;
    };

    class IScreen
    {
      public:
        [[nodiscard]] virtual std::optional<Point> cursor() = 0;
        [[nodiscard]] virtual bool buttonDown(Button button) = 0;
        [[nodiscard]] virtual std::optional<HostWindow> hostWindow() = 0;
        [[nodiscard]] virtual std::expected<CellGeometry, Error> cellGeometry() = 0;
    };

    class IInput
    {
      public:
        virtual void release(Button button) = 0;
        virtual void press(Button button) = 0;
        [[nodiscard]] virtual ReplayOutcome replay(std::span<const MouseEvent> events) = 0;
    };

    class IShell
    {
      public:
        class DragData
        {
          public:
            virtual ~DragData() = default;
            [[nodiscard]] virtual std::uintptr_t nativeHandle() const = 0;
        };

        [[nodiscard]] virtual std::expected<std::unique_ptr<DragData>, Error> makeDataObject(
            std::span<const std::wstring> paths) = 0;
        [[nodiscard]] virtual DragLoopOutcome runDrag(NativeWindow owner, DragData &data, std::uintptr_t source) = 0;
        [[nodiscard]] virtual std::expected<void, Error> copy(std::span<const std::wstring> paths,
                                                              std::wstring_view destination, Effect effect) = 0;
    };

    class IFiles
    {
      public:
        [[nodiscard]] virtual std::expected<std::wstring, Error> runDirectory() = 0;
        [[nodiscard]] virtual std::expected<std::wstring, Error> placeholder(std::wstring_view name,
                                                                             bool directory) = 0;
        [[nodiscard]] virtual std::expected<void, Error> removeTree(std::wstring_view path) = 0;
        virtual void sweep() = 0;
    };

    class IPeers
    {
      public:
        virtual void announce() = 0;
        [[nodiscard]] virtual std::vector<Peer> peers() = 0;
        [[nodiscard]] virtual std::expected<void, Error> send(const Peer &peer, const Drop &drop) = 0;
    };

    class IDragTool
    {
      public:
        [[nodiscard]] virtual bool start() = 0;
        [[nodiscard]] virtual bool prepare(std::span<const std::wstring> paths, Button button, DropContext context) = 0;
        [[nodiscard]] virtual bool showAndArm() = 0;
        virtual void abort() = 0;
        [[nodiscard]] virtual bool active() const = 0;
        virtual void stop() = 0;
    };

    class IDropSession
    {
      public:
        virtual void prepare(DropContext context) = 0;
        [[nodiscard]] virtual Effect effect(Point point, bool shift) const = 0;
        [[nodiscard]] virtual Effect drop(Point point, bool shift) = 0;
    };

} // namespace burlak::core
