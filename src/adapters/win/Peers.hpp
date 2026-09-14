#pragma once

#include "adapters/win/Focus.hpp"
#include "core/Interfaces.hpp"

#include <windows.h>

namespace burlak::adapters::win
{

    struct PeerCalls
    {
        decltype(&RegisterWindowMessageW) registerMessage;
        decltype(&ChangeWindowMessageFilterEx) changeFilter;
        decltype(&PostMessageW) postMessage;
        decltype(&SendMessageW) sendMessage;
        decltype(&GetCurrentProcessId) getProcessId;
        decltype(&GetTickCount64) getTickCount;
        decltype(&GetConsoleWindow) getConsoleWindow;
        decltype(&IsWindowVisible) isWindowVisible;
        decltype(&GetWindowRect) getWindowRect;
        decltype(&GetWindow) getWindow;
        decltype(&CreatePopupMenu) createMenu;
        decltype(&AppendMenuW) appendMenu;
        decltype(&TrackPopupMenu) trackMenu;
        decltype(&DestroyMenu) destroyMenu;
        decltype(&SetForegroundWindow) setForegroundWindow;
    };

    class Peers final : public core::IPeers
    {
      public:
        explicit Peers(Focus &focus);
        Peers(Focus &focus, const PeerCalls &calls);

        [[nodiscard]] std::uint32_t announcementMessage() const override;
        [[nodiscard]] std::uint32_t processId() const override;
        [[nodiscard]] std::uint64_t now() const override;
        [[nodiscard]] core::NativeWindow broadcastTarget() const override;
        [[nodiscard]] bool allowMessages(core::NativeWindow tool) override;
        void announce(core::NativeWindow source, core::NativeWindow target) override;
        [[nodiscard]] bool reply(core::NativeWindow target, core::NativeWindow tool) override;
        [[nodiscard]] std::optional<core::PeerPayload> receive(std::intptr_t nativePayload) override;
        [[nodiscard]] std::expected<void, core::Error> send(const core::Peer &peer, const core::Drop &drop) override;
        [[nodiscard]] core::PeerMenuChoice menu(core::NativeWindow owner, core::Point point) override;

      private:
        [[nodiscard]] core::NativeWindow hostWindow() const;
        [[nodiscard]] bool sendBytes(core::NativeWindow target, std::uintptr_t kind,
                                     std::span<const std::byte> bytes) const;

        Focus &focus_;
        const PeerCalls &calls_;
        std::uint32_t announcement_{};
        core::NativeWindow tool_{};
    };

    [[nodiscard]] const PeerCalls &systemPeerCalls();
    [[nodiscard]] std::uintptr_t peerHelloDataKind();
    [[nodiscard]] std::uintptr_t peerDropDataKind();

} // namespace burlak::adapters::win
