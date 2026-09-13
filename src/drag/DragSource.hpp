#pragma once

#include "core/Interfaces.hpp"
#include "core/Policies.hpp"

#include <shobjidl.h>
#include <windows.h>

namespace burlak::drag
{

    class DragSource final : public IDropSource
    {
      public:
        DragSource(core::IReleasePolicy &policy, core::IScreen &screen, core::IExtraction &extraction,
                   core::Button button, core::NativeWindow ownWindow, bool needsExtraction);

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void **object) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override;
        HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override;

        [[nodiscard]] core::Effect lastEffect() const;

      private:
        core::IReleasePolicy &policy_;
        core::IScreen &screen_;
        core::IExtraction &extraction_;
        core::Button button_;
        core::NativeWindow ownWindow_{};
        bool needsExtraction_{};
        ULONG references_{1};
        core::Effect lastEffect_{core::Effect::None};
    };

} // namespace burlak::drag
