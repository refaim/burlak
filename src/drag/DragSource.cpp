#include "drag/DragSource.hpp"

namespace burlak::drag
{

    DragSource::DragSource(core::IReleasePolicy &policy, core::IScreen &screen, core::IExtraction &extraction,
                           core::Button button, core::NativeWindow ownWindow, bool needsExtraction)
        : policy_{policy}, screen_{screen}, extraction_{extraction}, button_{button}, ownWindow_{ownWindow},
          needsExtraction_{needsExtraction}
    {
    }

    HRESULT DragSource::QueryInterface(REFIID interfaceId, void **object)
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interfaceId != IID_IUnknown && interfaceId != IID_IDropSource) {
            return E_NOINTERFACE;
        }
        *object = static_cast<IDropSource *>(this);
        AddRef();
        return S_OK;
    }

    ULONG DragSource::AddRef()
    {
        return ++references_;
    }

    ULONG DragSource::Release()
    {
        return --references_;
    }

    HRESULT DragSource::QueryContinueDrag(BOOL escapePressed, DWORD keyState)
    {
        const bool escaped = escapePressed != FALSE;
        const bool leftDown = (keyState & MK_LBUTTON) != 0;
        const bool rightDown = (keyState & MK_RBUTTON) != 0;
        const bool released = button_ == core::Button::Left ? !leftDown : !rightDown;
        bool overOwnWindow = false;
        if (!escaped && released && lastEffect_ != core::Effect::None && needsExtraction_) {
            const auto point = screen_.cursor();
            overOwnWindow = point && screen_.windowAt(*point) == ownWindow_;
        }
        const auto action =
            policy_.query(button_, escaped, leftDown, rightDown, lastEffect_, needsExtraction_, overOwnWindow);
        if (action == core::DragAction::ExtractThenDrop) {
            return extraction_.extract() ? DRAGDROP_S_DROP : DRAGDROP_S_CANCEL;
        }
        constexpr HRESULT results[]{S_OK, DRAGDROP_S_DROP, DRAGDROP_S_CANCEL};
        return results[static_cast<std::size_t>(action)];
    }

    HRESULT DragSource::GiveFeedback(DWORD effect)
    {
        lastEffect_ = policy_.feedback((effect & DROPEFFECT_MOVE) != 0, (effect & DROPEFFECT_COPY) != 0,
                                       !needsExtraction_ && (effect & DROPEFFECT_LINK) != 0);
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

    core::Effect DragSource::lastEffect() const
    {
        return lastEffect_;
    }

} // namespace burlak::drag
