#pragma once

#include "core/Interfaces.hpp"
#include "core/Peers.hpp"
#include "core/Policies.hpp"

#include <shobjidl.h>
#include <windows.h>

namespace burlak::drag
{

    class DragSource final : public IDropSource
    {
      public:
        DragSource(core::IReleasePolicy &policy, core::IScreen &screen, core::IExtraction &extraction,
                   core::IPeers &peers, core::PeerRegistry &registry, core::Button button, core::NativeWindow ownWindow,
                   core::NativeWindow ownHost, bool needsExtraction, std::span<const std::wstring> paths);

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void **object) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override;
        HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override;

        void completePeerHandoff();
        [[nodiscard]] core::Effect lastEffect() const;
        [[nodiscard]] bool peerHandoff() const;

      private:
        struct PendingPeerHandoff
        {
            core::Peer peer;
            core::Drop drop;
        };

        core::IReleasePolicy &policy_;
        core::IScreen &screen_;
        core::IExtraction &extraction_;
        core::IPeers &peers_;
        core::PeerRegistry &registry_;
        core::Button button_;
        core::NativeWindow ownWindow_{};
        core::NativeWindow ownHost_{};
        bool needsExtraction_{};
        std::span<const std::wstring> paths_;
        ULONG references_{1};
        core::Effect lastEffect_{core::Effect::None};
        std::optional<PendingPeerHandoff> pendingPeerHandoff_;
        bool peerHandoff_{};
    };

} // namespace burlak::drag
