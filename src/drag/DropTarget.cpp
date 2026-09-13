#include "drag/DropTarget.hpp"

namespace burlak::drag
{

    DropTarget::DropTarget(core::IDropSession &session) : session_{session}
    {
    }

    HRESULT DropTarget::QueryInterface(REFIID interfaceId, void **object)
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interfaceId != IID_IUnknown && interfaceId != IID_IDropTarget) {
            return E_NOINTERFACE;
        }
        *object = static_cast<IDropTarget *>(this);
        AddRef();
        return S_OK;
    }

    ULONG DropTarget::AddRef()
    {
        return ++references_;
    }

    ULONG DropTarget::Release()
    {
        return --references_;
    }

    HRESULT DropTarget::DragEnter(IDataObject *, DWORD keyState, POINTL point, DWORD *effect)
    {
        return apply(keyState, point, effect, false);
    }

    HRESULT DropTarget::DragOver(DWORD keyState, POINTL point, DWORD *effect)
    {
        return apply(keyState, point, effect, false);
    }

    HRESULT DropTarget::DragLeave()
    {
        return S_OK;
    }

    HRESULT DropTarget::Drop(IDataObject *, DWORD keyState, POINTL point, DWORD *effect)
    {
        return apply(keyState, point, effect, true);
    }

    HRESULT DropTarget::apply(DWORD keyState, POINTL point, DWORD *effect, bool dropping)
    {
        if (effect == nullptr) {
            return E_INVALIDARG;
        }
        const core::Point at{static_cast<int>(point.x), static_cast<int>(point.y)};
        const bool shift = (keyState & MK_SHIFT) != 0;
        const auto chosen = dropping ? session_.drop(at, shift) : session_.effect(at, shift);
        constexpr DWORD effects[]{DROPEFFECT_NONE, DROPEFFECT_COPY, DROPEFFECT_MOVE};
        *effect = effects[static_cast<std::size_t>(chosen)];
        return S_OK;
    }

} // namespace burlak::drag
