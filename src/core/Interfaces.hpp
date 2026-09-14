#pragma once

#include "core/Peers.hpp"
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
        [[nodiscard]] virtual std::expected<std::wstring, Error> extract(PanelHandle panel, std::span<const Item> items,
                                                                         const PluginModule &module,
                                                                         std::wstring_view destination) = 0;
    };

    class IScreen
    {
      public:
        [[nodiscard]] virtual std::optional<Point> cursor() = 0;
        [[nodiscard]] virtual bool buttonDown(Button button) = 0;
        [[nodiscard]] virtual NativeWindow windowAt(Point point) = 0;
        [[nodiscard]] virtual std::optional<HostWindow> hostWindow() = 0;
        [[nodiscard]] virtual std::optional<HostWindow> hostWindowAt(Point point) = 0;
        [[nodiscard]] virtual std::expected<CellGeometry, Error> cellGeometry() = 0;
        [[nodiscard]] virtual std::expected<CellGeometry, Error> cellGeometryAt(Point point) = 0;
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

        struct PreparedDrag
        {
            std::unique_ptr<DragData> data;
            std::size_t parsedPaths{};
        };

        [[nodiscard]] virtual std::expected<PreparedDrag, Error> makeDataObject(
            std::span<const std::wstring> paths, std::optional<Effect> preferredEffect = std::nullopt) = 0;
        [[nodiscard]] virtual DragLoopOutcome runDrag(NativeWindow owner, DragData &data, std::uintptr_t source,
                                                      bool allowLink) = 0;
        [[nodiscard]] virtual std::expected<void, Error> copy(std::span<const std::wstring> paths,
                                                              std::wstring_view destination, Effect effect,
                                                              NativeWindow owner) = 0;
    };

    class IFiles
    {
      public:
        [[nodiscard]] virtual std::expected<std::wstring, Error> runDirectory() = 0;
        [[nodiscard]] virtual std::expected<std::wstring, Error> placeholder(std::wstring_view name,
                                                                             bool directory) = 0;
        [[nodiscard]] virtual bool nameBefore(std::wstring_view left, std::wstring_view right) const = 0;
        [[nodiscard]] virtual std::expected<void, Error> removeTree(std::wstring_view path) = 0;
        [[nodiscard]] virtual std::expected<void, Error> touch(std::wstring_view path) = 0;
        [[nodiscard]] virtual AdoptedPeerPaths adoptPeerPaths(std::span<const std::wstring> paths,
                                                              std::uint32_t sourceProcess) = 0;
        virtual void sweep() = 0;
    };

    class IPeers
    {
      public:
        [[nodiscard]] virtual std::wstring_view toolWindowClass() const = 0;
        [[nodiscard]] virtual bool isAnnouncementMessage(std::uint32_t message) const = 0;
        [[nodiscard]] virtual std::optional<PeerAnnouncement> receiveAnnouncement(std::uint32_t message,
                                                                                  std::uintptr_t word,
                                                                                  std::intptr_t number) = 0;
        [[nodiscard]] virtual std::expected<std::uint64_t, Error> newNonce() const = 0;
        [[nodiscard]] virtual std::uint32_t processId() const = 0;
        [[nodiscard]] virtual NativeWindow broadcastTarget() const = 0;
        virtual void announce(NativeWindow source, NativeWindow target, std::uint64_t nonce) = 0;
        virtual void endAnnouncement(NativeWindow source, NativeWindow target, std::uint64_t nonce) = 0;
        [[nodiscard]] virtual std::expected<PeerTransportResult, Error> reply(PeerIdentity target, NativeWindow tool,
                                                                              std::uint64_t echoNonce,
                                                                              std::uint64_t nonce) = 0;
        [[nodiscard]] virtual std::optional<PeerEnvelope> receive(std::uintptr_t sender,
                                                                  std::intptr_t nativePayload) = 0;
        [[nodiscard]] virtual std::expected<PeerTransportResult, Error> send(const Peer &peer, const Drop &drop) = 0;
        [[nodiscard]] virtual PeerMenuChoice menu(NativeWindow owner, Point point) = 0;
    };

    class IDragTool
    {
      public:
        [[nodiscard]] virtual bool start() = 0;
        [[nodiscard]] virtual bool prepare(std::span<const std::wstring> paths, Button button, bool needsExtraction,
                                           DropContext context) = 0;
        [[nodiscard]] virtual bool showAndArm() = 0;
        virtual void abort() = 0;
        [[nodiscard]] virtual bool active() const = 0;
        virtual void stop() = 0;
    };

    class IExtraction
    {
      public:
        virtual ~IExtraction() = default;
        [[nodiscard]] virtual bool extract() = 0;
        virtual void retain() = 0;
        virtual void cleanup() = 0;
    };

    class IExtractionSession
    {
      public:
        virtual ~IExtractionSession() = default;
        [[nodiscard]] virtual bool requestExtraction() = 0;
        virtual void retain() = 0;
        virtual void cleanup() = 0;
    };

    class IDropSession
    {
      public:
        virtual void prepare(DropContext context) = 0;
        [[nodiscard]] virtual Effect effect(Point point, bool shift) const = 0;
        [[nodiscard]] virtual Effect drop(Point point, bool shift) = 0;
        [[nodiscard]] virtual bool receivePeerDrop(PendingPeerDrop drop) = 0;
    };

} // namespace burlak::core
