#include "drag/DragSource.hpp"

namespace burlak::drag
{

    DragSource::DragSource(core::IReleasePolicy &policy, core::Button button) : policy_{policy}, button_{button}
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
        const auto action =
            policy_.query(button_, escapePressed != FALSE, (keyState & MK_LBUTTON) != 0, (keyState & MK_RBUTTON) != 0);
        constexpr HRESULT results[]{S_OK, DRAGDROP_S_DROP, DRAGDROP_S_CANCEL};
        return results[static_cast<std::size_t>(action)];
    }

    HRESULT DragSource::GiveFeedback(DWORD effect)
    {
        lastEffect_ = policy_.feedback((effect & DROPEFFECT_MOVE) != 0, (effect & DROPEFFECT_COPY) != 0);
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

    core::Effect DragSource::lastEffect() const
    {
        return lastEffect_;
    }

} // namespace burlak::drag
