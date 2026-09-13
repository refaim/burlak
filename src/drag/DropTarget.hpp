#pragma once

#include "core/Policies.hpp"

#include <oleidl.h>
#include <windows.h>

namespace burlak::drag
{

    class DropTarget final : public IDropTarget
    {
      public:
        explicit DropTarget(core::IDropPolicy &policy);

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void **object) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD keyState, POINTL point, DWORD *effect) override;
        HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD *effect) override;
        HRESULT STDMETHODCALLTYPE DragLeave() override;
        HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD keyState, POINTL point, DWORD *effect) override;

      private:
        [[nodiscard]] HRESULT apply(DWORD *effect) const;

        core::IDropPolicy &policy_;
        ULONG references_{1};
    };

} // namespace burlak::drag
