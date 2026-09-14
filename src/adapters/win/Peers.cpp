#include "adapters/win/Peers.hpp"

#include "core/PeerWire.hpp"
#include "core/Policies.hpp"

#include <algorithm>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace burlak::adapters::win
{

    namespace
    {

        constexpr std::array announcementNames{
            L"Burlak.PeerAnnouncement.Begin.Low.v2", L"Burlak.PeerAnnouncement.Begin.High.v2",
            L"Burlak.PeerAnnouncement.End.Low.v2", L"Burlak.PeerAnnouncement.End.High.v2"};
        constexpr wchar_t toolClass[] = L"BurlakToolWindow";
        constexpr std::uintptr_t helloKind = 0x484C4F32U;
        constexpr std::uintptr_t dropKind = 0x44525032U;
        constexpr std::uint32_t sendTimeoutMilliseconds = 250;
        constexpr UINT copyCommand = 1;
        constexpr UINT moveCommand = 2;
        constexpr UINT cancelCommand = 3;

        const PeerCalls calls{RegisterWindowMessageW,
                              PostMessageW,
                              SendMessageTimeoutW,
                              GetCurrentProcessId,
                              GetClassNameW,
                              GetWindowThreadProcessId,
                              BCryptGenRandom,
                              GetConsoleWindow,
                              IsWindowVisible,
                              GetWindowRect,
                              GetWindow,
                              CreatePopupMenu,
                              AppendMenuW,
                              TrackPopupMenu,
                              DestroyMenu,
                              SetForegroundWindow};

        struct MenuDestroyer
        {
            decltype(&DestroyMenu) destroy;

            void operator()(HMENU menu) const noexcept
            {
                static_cast<void>(destroy(menu));
            }
        };

        using MenuObject = std::remove_pointer_t<HMENU>;
        using UniqueMenu = std::unique_ptr<MenuObject, MenuDestroyer>;

    } // namespace

    Peers::Peers(Focus &focus) : Peers{focus, calls}
    {
    }

    Peers::Peers(Focus &focus, const PeerCalls &api) : focus_{focus}, calls_{api}
    {
        for (std::size_t index = 0; index < announcements_.size(); ++index) {
            announcements_[index] = calls_.registerMessage(announcementNames[index]);
        }
    }

    std::wstring_view Peers::toolWindowClass() const
    {
        return toolClass;
    }

    bool Peers::isAnnouncementMessage(std::uint32_t message) const
    {
        return message != 0 && std::ranges::find(announcements_, message) != announcements_.end();
    }

    std::optional<core::PeerIdentity> Peers::toolIdentity(core::NativeWindow window) const
    {
        if (window == 0) {
            return std::nullopt;
        }
        const auto native = reinterpret_cast<HWND>(window);
        std::array<wchar_t, 64> name{};
        const auto length = calls_.getClassName(native, name.data(), static_cast<int>(name.size()));
        DWORD process{};
        if (length <= 0 || std::wstring_view{name.data(), static_cast<std::size_t>(length)} != toolClass ||
            calls_.getWindowProcess(native, &process) == 0 || process == 0) {
            return std::nullopt;
        }
        return core::PeerIdentity{.window = window, .process = process};
    }

    bool Peers::expectedWindow(core::NativeWindow window, std::uint32_t process, std::wstring_view expectedClass) const
    {
        if (window == 0 || process == 0) {
            return false;
        }
        const auto native = reinterpret_cast<HWND>(window);
        std::array<wchar_t, 64> name{};
        const auto length = calls_.getClassName(native, name.data(), static_cast<int>(name.size()));
        DWORD actualProcess{};
        return length > 0 && std::wstring_view{name.data(), static_cast<std::size_t>(length)} == expectedClass &&
               calls_.getWindowProcess(native, &actualProcess) != 0 && actualProcess == process;
    }

    std::optional<core::PeerAnnouncement> Peers::receiveAnnouncement(std::uint32_t message, std::uintptr_t word,
                                                                     std::intptr_t number)
    {
        const auto found = std::ranges::find(announcements_, message);
        if (message == 0 || found == announcements_.end()) {
            return std::nullopt;
        }
        const auto identity = toolIdentity(static_cast<core::NativeWindow>(word));
        if (!identity) {
            return std::nullopt;
        }
        const auto messageIndex = static_cast<std::size_t>(found - announcements_.begin());
        const auto actionIndex = messageIndex / 2;
        if (messageIndex % 2 == 0) {
            pendingAnnouncements_[actionIndex] =
                AnnouncementHalf{.source = *identity, .low = static_cast<std::uint32_t>(number)};
            return std::nullopt;
        }
        auto low = std::exchange(pendingAnnouncements_[actionIndex], std::nullopt);
        if (!low || low->source != *identity) {
            return std::nullopt;
        }
        const auto nonce = static_cast<std::uint64_t>(low->low) |
                           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(number)) << 32U);
        if (nonce == 0) {
            return std::nullopt;
        }
        constexpr std::array actions{core::PeerAnnouncementAction::Begin, core::PeerAnnouncementAction::End};
        return core::PeerAnnouncement{.action = actions[actionIndex], .source = *identity, .nonce = nonce};
    }

    std::expected<std::uint64_t, core::Error> Peers::newNonce() const
    {
        std::uint64_t nonce{};
        const auto status =
            calls_.random(nullptr, reinterpret_cast<PUCHAR>(&nonce), sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0 || nonce == 0) {
            return std::unexpected(core::Error::Unavailable);
        }
        return nonce;
    }

    std::uint32_t Peers::processId() const
    {
        return calls_.getProcessId();
    }

    core::NativeWindow Peers::broadcastTarget() const
    {
        return reinterpret_cast<core::NativeWindow>(HWND_BROADCAST);
    }

    void Peers::postAnnouncement(core::PeerAnnouncementAction action, core::NativeWindow source,
                                 core::NativeWindow target, std::uint64_t nonce)
    {
        tool_ = source;
        const auto begin = action == core::PeerAnnouncementAction::Begin ? 0U : 2U;
        if (announcements_[begin] == 0 || announcements_[begin + 1] == 0 || source == 0 || target == 0 || nonce == 0) {
            return;
        }
        const auto nativeTarget = reinterpret_cast<HWND>(target);
        static_cast<void>(calls_.postMessage(nativeTarget, announcements_[begin], static_cast<WPARAM>(source),
                                             static_cast<LPARAM>(static_cast<std::uint32_t>(nonce))));
        static_cast<void>(calls_.postMessage(nativeTarget, announcements_[begin + 1], static_cast<WPARAM>(source),
                                             static_cast<LPARAM>(static_cast<std::uint32_t>(nonce >> 32U))));
    }

    void Peers::announce(core::NativeWindow source, core::NativeWindow target, std::uint64_t nonce)
    {
        postAnnouncement(core::PeerAnnouncementAction::Begin, source, target, nonce);
    }

    void Peers::endAnnouncement(core::NativeWindow source, core::NativeWindow target, std::uint64_t nonce)
    {
        postAnnouncement(core::PeerAnnouncementAction::End, source, target, nonce);
    }

    core::NativeWindow Peers::hostWindow() const
    {
        const auto console = calls_.getConsoleWindow();
        RECT rect{};
        if (console != nullptr && calls_.isWindowVisible(console) != FALSE && calls_.getWindowRect(console, &rect) &&
            rect.right > rect.left && rect.bottom > rect.top) {
            return reinterpret_cast<core::NativeWindow>(console);
        }
        return reinterpret_cast<core::NativeWindow>(console == nullptr ? nullptr : calls_.getWindow(console, GW_OWNER));
    }

    bool Peers::reply(core::PeerIdentity target, core::NativeWindow tool, std::uint64_t echoNonce, std::uint64_t nonce)
    {
        const auto host = hostWindow();
        if (host == 0) {
            return false;
        }
        const auto bytes = core::encodePeerHello({.process = processId(),
                                                  .tool = tool,
                                                  .host = host,
                                                  .lastFocus = focus_.last(),
                                                  .echoNonce = echoNonce,
                                                  .nonce = nonce});
        const auto previous = tool_;
        tool_ = tool;
        const bool sent = sendBytes(target, toolClass, helloKind, bytes).has_value();
        tool_ = previous;
        return sent;
    }

    std::optional<core::PeerEnvelope> Peers::receive(std::uintptr_t sender, std::intptr_t nativePayload)
    {
        const auto identity = toolIdentity(static_cast<core::NativeWindow>(sender));
        if (!identity || nativePayload == 0) {
            return std::nullopt;
        }
        const auto &copy = *reinterpret_cast<const COPYDATASTRUCT *>(nativePayload);
        if (copy.lpData == nullptr || copy.cbData == 0) {
            return std::nullopt;
        }
        const auto bytes = std::span{static_cast<const std::byte *>(copy.lpData), copy.cbData};
        if (copy.dwData == helloKind) {
            return core::decodePeerHello(bytes).transform(
                [&](core::PeerHello hello) { return core::PeerEnvelope{*identity, core::PeerPayload{hello}}; });
        }
        if (copy.dwData == dropKind) {
            return core::decodePeerDrop(bytes).transform(
                [&](core::Drop drop) { return core::PeerEnvelope{*identity, core::PeerPayload{std::move(drop)}}; });
        }
        return std::nullopt;
    }

    std::expected<void, core::Error> Peers::sendBytes(core::PeerIdentity target, std::wstring_view expectedClass,
                                                      std::uintptr_t kind, std::span<const std::byte> bytes) const
    {
        if (bytes.empty() || !expectedWindow(target.window, target.process, expectedClass)) {
            return std::unexpected(core::Error::Unavailable);
        }
        COPYDATASTRUCT copy{.dwData = kind,
                            .cbData = static_cast<DWORD>(bytes.size()),
                            .lpData = const_cast<std::byte *>(bytes.data())};
        DWORD_PTR result{};
        const auto sent = calls_.sendMessageTimeout(reinterpret_cast<HWND>(target.window), WM_COPYDATA,
                                                    static_cast<WPARAM>(tool_), reinterpret_cast<LPARAM>(&copy),
                                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, sendTimeoutMilliseconds, &result);
        if (sent == 0) {
            return std::unexpected(core::Error::Indeterminate);
        }
        return core::expectedOutcome(result != 0, core::Error::Unavailable);
    }

    std::expected<void, core::Error> Peers::send(const core::Peer &peer, const core::Drop &drop)
    {
        if (peer.nonce == 0 || drop.nonce != peer.nonce) {
            return std::unexpected(core::Error::Unavailable);
        }
        const auto bytes = core::encodePeerDrop(drop);
        return sendBytes({.window = peer.window, .process = peer.process}, toolClass, dropKind, bytes);
    }

    core::PeerMenuChoice Peers::menu(core::NativeWindow owner, core::Point point)
    {
        UniqueMenu menu{calls_.createMenu(), MenuDestroyer{calls_.destroyMenu}};
        if (!menu || calls_.appendMenu(menu.get(), MF_STRING, copyCommand, L"Copy here") == FALSE ||
            calls_.appendMenu(menu.get(), MF_STRING, moveCommand, L"Move here") == FALSE ||
            calls_.appendMenu(menu.get(), MF_SEPARATOR, 0, nullptr) == FALSE ||
            calls_.appendMenu(menu.get(), MF_STRING, cancelCommand, L"Cancel") == FALSE) {
            return core::PeerMenuChoice::Cancel;
        }
        const auto window = reinterpret_cast<HWND>(owner);
        static_cast<void>(calls_.setForegroundWindow(window));
        const auto command = calls_.trackMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x,
                                              point.y, 0, window, nullptr);
        if (command == copyCommand) {
            return core::PeerMenuChoice::Copy;
        }
        return command == moveCommand ? core::PeerMenuChoice::Move : core::PeerMenuChoice::Cancel;
    }

    const PeerCalls &systemPeerCalls()
    {
        return calls;
    }

    std::uintptr_t peerHelloDataKind()
    {
        return helloKind;
    }

    std::uintptr_t peerDropDataKind()
    {
        return dropKind;
    }

    std::uint32_t peerSendTimeoutMilliseconds()
    {
        return sendTimeoutMilliseconds;
    }

} // namespace burlak::adapters::win
