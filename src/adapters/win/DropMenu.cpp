#include "adapters/win/DropMenu.hpp"

#include <array>
#include <memory>
#include <type_traits>

namespace burlak::adapters::win
{

    namespace
    {

        constexpr UINT copyCommand = 1;
        constexpr UINT moveCommand = 2;
        constexpr UINT cancelCommand = 3;
        const DropMenuCalls calls{
            CreatePopupMenu,     AppendMenuW,         TrackPopupMenu,           DestroyMenu,
            SetForegroundWindow, GetForegroundWindow, GetWindowThreadProcessId, GetCurrentThreadId,
            AttachThreadInput,   PostMessageW};

        // Attaches our input queue to another thread's for the lifetime of the guard so a SetForegroundWindow the
        // system would otherwise refuse is honoured (Raymond Chen / MSDN: SetForegroundWindow succeeds for a thread
        // whose input is attached to the foreground thread's). Branch-free: a zero or self target simply makes both
        // AttachThreadInput calls no-ops.
        class AttachedInput final
        {
          public:
            AttachedInput(const DropMenuCalls &api, DWORD ours, DWORD other) : api_{api}, ours_{ours}, other_{other}
            {
                static_cast<void>(api_.attachInput(ours_, other_, TRUE));
            }
            ~AttachedInput()
            {
                static_cast<void>(api_.attachInput(ours_, other_, FALSE));
            }

          private:
            // The guard test forbids the `delete` token in src, so copy control follows the repository's convention
            // of private, undefined copy members (see ExtractionCompleter).
            AttachedInput(const AttachedInput &);
            AttachedInput &operator=(const AttachedInput &);

            const DropMenuCalls &api_;
            DWORD ours_;
            DWORD other_;
        };

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

    DropMenu::DropMenu() : calls_{calls}
    {
    }

    DropMenu::DropMenu(const DropMenuCalls &api) : calls_{api}
    {
    }

    core::DropMenuChoice DropMenu::choose(core::NativeWindow owner, core::Point point, core::AllowedEffects allowed)
    {
        UniqueMenu menu{calls_.createMenu(), MenuDestroyer{calls_.destroyMenu}};
        if (!menu) {
            return core::DropMenuChoice::Cancel;
        }
        const UINT copyFlags = MF_STRING | (allowed.copy ? 0U : MF_GRAYED);
        const UINT moveFlags = MF_STRING | (allowed.move ? 0U : MF_GRAYED);
        static_cast<void>(calls_.appendMenu(menu.get(), copyFlags, copyCommand, L"Copy here"));
        static_cast<void>(calls_.appendMenu(menu.get(), moveFlags, moveCommand, L"Move here"));
        static_cast<void>(calls_.appendMenu(menu.get(), MF_SEPARATOR, 0, nullptr));
        static_cast<void>(calls_.appendMenu(menu.get(), MF_STRING, cancelCommand, L"Cancel"));
        const auto nativeOwner = reinterpret_cast<HWND>(owner);
        // At drop time the foreground window is the drag source (another process), and our process was not the last
        // to receive input, so a bare SetForegroundWindow is refused: the popup would then never see the outside
        // click or Escape that dismisses it and the source stays blocked in DoDragDrop. Attaching our input to the
        // foreground thread lets the call through; the WM_NULL after TrackPopupMenu is the KB135788 workaround that
        // makes the first outside click dismiss the menu. We track regardless of the SetForegroundWindow result.
        const DWORD ourThread = calls_.getCurrentThreadId();
        const DWORD foregroundThread = calls_.getWindowThread(calls_.getForegroundWindow(), nullptr);
        const AttachedInput attached{calls_, ourThread, foregroundThread};
        static_cast<void>(calls_.setForegroundWindow(nativeOwner));
        const auto selected = calls_.trackMenu(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x,
                                               point.y, 0, nativeOwner, nullptr);
        static_cast<void>(calls_.postMessage(nativeOwner, WM_NULL, 0, 0));
        constexpr std::array choices{core::DropMenuChoice::Cancel, core::DropMenuChoice::Copy,
                                     core::DropMenuChoice::Move, core::DropMenuChoice::Cancel};
        const auto command = static_cast<UINT>(selected);
        return choices.at(command <= cancelCommand ? command : 0);
    }

    const DropMenuCalls &systemDropMenuCalls()
    {
        return calls;
    }

} // namespace burlak::adapters::win
