#pragma once

#include "adapters/win/Focus.hpp"
#include "core/Interfaces.hpp"

#include <bcrypt.h>
#include <windows.h>

#include <array>

namespace burlak::adapters::win
{

    struct PeerCalls
    {
        decltype(&RegisterWindowMessageW) registerMessage;
        decltype(&PostMessageW) postMessage;
        decltype(&SendMessageTimeoutW) sendMessageTimeout;
        decltype(&GetCurrentProcessId) getProcessId;
        decltype(&GetClassNameW) getClassName;
        decltype(&GetWindowThreadProcessId) getWindowProcess;
        decltype(&GetLastError) getLastError;
        decltype(&BCryptGenRandom) random;
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

        [[nodiscard]] std::wstring_view toolWindowClass() const override;
        [[nodiscard]] bool isAnnouncementMessage(std::uint32_t message) const override;
        [[nodiscard]] std::optional<core::PeerAnnouncement> receiveAnnouncement(std::uint32_t message,
                                                                                std::uintptr_t word,
                                                                                std::intptr_t number) override;
        [[nodiscard]] std::expected<std::uint64_t, core::Error> newNonce() const override;
        [[nodiscard]] std::uint32_t processId() const override;
        [[nodiscard]] core::NativeWindow broadcastTarget() const override;
        void announce(core::NativeWindow source, core::NativeWindow target, std::uint64_t nonce) override;
        void endAnnouncement(core::NativeWindow source, core::NativeWindow target, std::uint64_t nonce) override;
        [[nodiscard]] std::expected<core::PeerTransportResult, core::Error> reply(core::PeerIdentity target,
                                                                                  core::NativeWindow tool,
                                                                                  std::uint64_t echoNonce,
                                                                                  std::uint64_t nonce) override;
        [[nodiscard]] std::optional<core::PeerEnvelope> receive(std::uintptr_t sender,
                                                                std::intptr_t nativePayload) override;
        [[nodiscard]] std::expected<core::PeerTransportResult, core::Error> send(const core::Peer &peer,
                                                                                 const core::Drop &drop) override;
        [[nodiscard]] core::PeerMenuChoice menu(core::NativeWindow owner, core::Point point) override;

      private:
        [[nodiscard]] core::NativeWindow hostWindow() const;
        [[nodiscard]] std::optional<core::PeerIdentity> toolIdentity(core::NativeWindow window) const;
        [[nodiscard]] bool expectedWindow(core::NativeWindow window, std::uint32_t process,
                                          std::wstring_view expectedClass) const;
        void postAnnouncement(core::PeerAnnouncementAction action, core::NativeWindow source, core::NativeWindow target,
                              std::uint64_t nonce);
        [[nodiscard]] std::expected<core::PeerTransportResult, core::Error> sendBytes(
            core::PeerIdentity target, std::wstring_view expectedClass, std::uintptr_t kind,
            std::span<const std::byte> bytes) const;

        struct AnnouncementHalf
        {
            core::PeerIdentity source{};
            std::uint32_t low{};
        };

        Focus &focus_;
        const PeerCalls &calls_;
        std::array<std::uint32_t, 4> announcements_{};
        std::array<std::optional<AnnouncementHalf>, 2> pendingAnnouncements_{};
        core::NativeWindow tool_{};
    };

    [[nodiscard]] const PeerCalls &systemPeerCalls();
    [[nodiscard]] std::uintptr_t peerHelloDataKind();
    [[nodiscard]] std::uintptr_t peerDropDataKind();
    [[nodiscard]] std::uint32_t peerSendTimeoutMilliseconds();

} // namespace burlak::adapters::win
