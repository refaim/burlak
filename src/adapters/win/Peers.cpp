#include "adapters/win/Peers.hpp"

#include "core/PeerWire.hpp"
#include "core/Policies.hpp"

#include <memory>
#include <type_traits>

namespace burlak::adapters::win
{

    namespace
    {

        constexpr wchar_t announcementName[] = L"Burlak.PeerAnnouncement.v1";
        constexpr std::uintptr_t helloKind = 0x484C4F31U;
        constexpr std::uintptr_t dropKind = 0x44525031U;
        constexpr UINT copyCommand = 1;
        constexpr UINT moveCommand = 2;
        constexpr UINT cancelCommand = 3;

        const PeerCalls calls{RegisterWindowMessageW, ChangeWindowMessageFilterEx,
                              PostMessageW,           SendMessageW,
                              GetCurrentProcessId,    GetTickCount64,
                              GetConsoleWindow,       IsWindowVisible,
                              GetWindowRect,          GetWindow,
                              CreatePopupMenu,        AppendMenuW,
                              TrackPopupMenu,         DestroyMenu,
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

    Peers::Peers(Focus &focus, const PeerCalls &api)
        : focus_{focus}, calls_{api}, announcement_{calls_.registerMessage(announcementName)}
    {
    }

    std::uint32_t Peers::announcementMessage() const
    {
        return announcement_;
    }

    std::uint32_t Peers::processId() const
    {
        return calls_.getProcessId();
    }

    std::uint64_t Peers::now() const
    {
        return calls_.getTickCount();
    }

    core::NativeWindow Peers::broadcastTarget() const
    {
        return reinterpret_cast<core::NativeWindow>(HWND_BROADCAST);
    }

    bool Peers::allowMessages(core::NativeWindow tool)
    {
        tool_ = tool;
        CHANGEFILTERSTRUCT result{.cbSize = sizeof(result), .ExtStatus = 0};
        const auto window = reinterpret_cast<HWND>(tool);
        const bool announcementAllowed =
            announcement_ != 0 && calls_.changeFilter(window, announcement_, MSGFLT_ALLOW, &result) != FALSE;
        const bool copyAllowed = calls_.changeFilter(window, WM_COPYDATA, MSGFLT_ALLOW, &result) != FALSE;
        return announcementAllowed && copyAllowed;
    }

    void Peers::announce(core::NativeWindow source, core::NativeWindow target)
    {
        static_cast<void>(calls_.postMessage(reinterpret_cast<HWND>(target), announcement_, processId(),
                                             static_cast<LPARAM>(source)));
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

    bool Peers::reply(core::NativeWindow target, core::NativeWindow tool)
    {
        const auto host = hostWindow();
        if (host == 0) {
            return false;
        }
        const auto bytes =
            core::encodePeerHello({.process = processId(), .tool = tool, .host = host, .lastFocus = focus_.last()});
        const auto previous = tool_;
        tool_ = tool;
        const bool sent = sendBytes(target, helloKind, bytes);
        tool_ = previous;
        return sent;
    }

    std::optional<core::PeerPayload> Peers::receive(std::intptr_t nativePayload)
    {
        if (nativePayload == 0) {
            return std::nullopt;
        }
        const auto &copy = *reinterpret_cast<const COPYDATASTRUCT *>(nativePayload);
        if (copy.lpData == nullptr || copy.cbData == 0) {
            return std::nullopt;
        }
        const auto bytes = std::span{static_cast<const std::byte *>(copy.lpData), copy.cbData};
        if (copy.dwData == helloKind) {
            return core::decodePeerHello(bytes).transform(
                [](core::PeerHello hello) { return core::PeerPayload{hello}; });
        }
        if (copy.dwData == dropKind) {
            return core::decodePeerDrop(bytes).transform(
                [](core::Drop drop) { return core::PeerPayload{std::move(drop)}; });
        }
        return std::nullopt;
    }

    bool Peers::sendBytes(core::NativeWindow target, std::uintptr_t kind, std::span<const std::byte> bytes) const
    {
        COPYDATASTRUCT copy{.dwData = kind,
                            .cbData = static_cast<DWORD>(bytes.size()),
                            .lpData = const_cast<std::byte *>(bytes.data())};
        return target != 0 && calls_.sendMessage(reinterpret_cast<HWND>(target), WM_COPYDATA,
                                                 static_cast<WPARAM>(tool_), reinterpret_cast<LPARAM>(&copy)) != 0;
    }

    std::expected<void, core::Error> Peers::send(const core::Peer &peer, const core::Drop &drop)
    {
        const auto bytes = core::encodePeerDrop(drop);
        return core::expectedOutcome(sendBytes(peer.window, dropKind, bytes), core::Error::Unavailable);
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

} // namespace burlak::adapters::win
