#include "drag/DropTarget.hpp"

namespace burlak::drag
{

    DropTarget::DropTarget(core::IDropPolicy &policy) : policy_{policy}
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

    HRESULT DropTarget::DragEnter(IDataObject *, DWORD, POINTL, DWORD *effect)
    {
        return apply(effect);
    }

    HRESULT DropTarget::DragOver(DWORD, POINTL, DWORD *effect)
    {
        return apply(effect);
    }

    HRESULT DropTarget::DragLeave()
    {
        return S_OK;
    }

    HRESULT DropTarget::Drop(IDataObject *, DWORD, POINTL, DWORD *effect)
    {
        return apply(effect);
    }

    HRESULT DropTarget::apply(DWORD *effect) const
    {
        if (effect == nullptr) {
            return E_INVALIDARG;
        }
        const auto chosen = policy_.effect((*effect & DROPEFFECT_MOVE) != 0, (*effect & DROPEFFECT_COPY) != 0);
        constexpr DWORD effects[]{DROPEFFECT_NONE, DROPEFFECT_COPY, DROPEFFECT_MOVE};
        *effect = effects[static_cast<std::size_t>(chosen)];
        return S_OK;
    }

} // namespace burlak::drag
