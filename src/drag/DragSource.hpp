#pragma once

#include "core/Policies.hpp"

#include <shobjidl.h>
#include <windows.h>

namespace burlak::drag
{

    class DragSource final : public IDropSource
    {
      public:
        DragSource(core::IReleasePolicy &policy, core::Button button);

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void **object) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override;
        HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override;

        [[nodiscard]] core::Effect lastEffect() const;

      private:
        core::IReleasePolicy &policy_;
        core::Button button_;
        ULONG references_{1};
        core::Effect lastEffect_{core::Effect::None};
    };

} // namespace burlak::drag
