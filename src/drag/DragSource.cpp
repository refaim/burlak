#include "drag/DragSource.hpp"

#include <utility>

namespace burlak::drag
{

    DragSource::DragSource(core::IReleasePolicy &policy, core::IScreen &screen, core::IExtraction &extraction,
                           core::IPeers &peers, core::PeerRegistry &registry, core::Button button,
                           core::NativeWindow ownWindow, core::NativeWindow ownHost, bool needsExtraction,
                           std::span<const std::wstring> paths)
        : policy_{policy}, screen_{screen}, extraction_{extraction}, peers_{peers}, registry_{registry},
          button_{button}, ownWindow_{ownWindow}, ownHost_{ownHost}, needsExtraction_{needsExtraction}, paths_{paths}
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
        std::optional<core::Point> point;
        std::optional<core::Peer> peer;
        if (!escaped && released && lastEffect_ != core::Effect::None) {
            point = screen_.cursor();
            if (point) {
                const auto window = screen_.windowAt(*point);
                overOwnWindow = needsExtraction_ && window == ownWindow_;
                peer = registry_.select(window, ownHost_);
            }
        }
        const auto action = policy_.query(button_, escaped, leftDown, rightDown, lastEffect_, needsExtraction_,
                                          overOwnWindow, peer.has_value());
        if (action == core::DragAction::ExtractThenDrop) {
            extractionRan_ = extraction_.extract();
            return extractionRan_ ? DRAGDROP_S_DROP : DRAGDROP_S_CANCEL;
        }
        if (action == core::DragAction::HandToPeer) {
            if (needsExtraction_) {
                extractionRan_ = extraction_.extract();
                if (!extractionRan_) {
                    return DRAGDROP_S_CANCEL;
                }
            }
            const auto chosen = button_ == core::Button::Right
                                    ? core::peerMenuEffect(peers_.menu(ownWindow_, *point))
                                    : ((keyState & MK_SHIFT) != 0 ? core::Effect::Move : core::Effect::Copy);
            if (chosen != core::Effect::None) {
                pendingPeerHandoff_ = PendingPeerHandoff{
                    .peer = *peer,
                    .drop = {
                        .paths = {paths_.begin(), paths_.end()}, .at = *point, .effect = chosen, .nonce = peer->nonce}};
            }
            return DRAGDROP_S_CANCEL;
        }
        constexpr HRESULT results[]{S_OK, DRAGDROP_S_DROP, DRAGDROP_S_CANCEL, DRAGDROP_S_CANCEL};
        return results[static_cast<std::size_t>(action)];
    }

    HRESULT DragSource::GiveFeedback(DWORD effect)
    {
        lastEffect_ = policy_.feedback((effect & DROPEFFECT_MOVE) != 0, (effect & DROPEFFECT_COPY) != 0,
                                       !needsExtraction_ && (effect & DROPEFFECT_LINK) != 0);
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

    void DragSource::completePeerHandoff()
    {
        auto pending = std::exchange(pendingPeerHandoff_, std::nullopt);
        if (!pending) {
            return;
        }
        const auto outcome = core::peerSendOutcome(peers_.send(pending->peer, pending->drop));
        // Once a timed-out handler might have queued an extracted drop, deleting its advertised run would race
        // that receiver. Keeping it is safe; a later dead-owner sweep removes it after the ten-minute grace period.
        peerHandoff_ = outcome == core::PeerSendOutcome::Accepted ||
                       (needsExtraction_ && outcome == core::PeerSendOutcome::Indeterminate);
    }

    core::Effect DragSource::lastEffect() const
    {
        return lastEffect_;
    }

    bool DragSource::peerHandoff() const
    {
        return peerHandoff_;
    }

    bool DragSource::extractionRan() const
    {
        return extractionRan_;
    }

} // namespace burlak::drag
