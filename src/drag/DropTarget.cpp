#include "drag/DropTarget.hpp"

#include "core/Policies.hpp"

namespace burlak::drag
{

    namespace
    {

        [[nodiscard]] DWORD nativeEffect(core::Effect effect)
        {
            constexpr DWORD effects[]{DROPEFFECT_NONE, DROPEFFECT_COPY, DROPEFFECT_MOVE, DROPEFFECT_LINK};
            return effects[static_cast<std::size_t>(effect)];
        }

    } // namespace

    DropTarget::DropTarget(core::IDropSession &session, core::IDropData &dropData, core::IDropMenu &menu,
                           IReceiveLifecycle &lifecycle)
        : session_{session}, dropData_{dropData}, menu_{menu}, lifecycle_{lifecycle}
    {
    }

    HRESULT DropTarget::QueryInterface(REFIID interfaceId, void **object) noexcept
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

    ULONG DropTarget::AddRef() noexcept
    {
        return ++references_;
    }

    ULONG DropTarget::Release() noexcept
    {
        return --references_;
    }

    HRESULT DropTarget::DragEnter(IDataObject *data, DWORD keyState, POINTL point, DWORD *effect) noexcept
    {
        return invoke(Entry::Enter, data, keyState, point, effect);
    }

    HRESULT DropTarget::dragEnter(IDataObject *data, DWORD keyState, POINTL point, DWORD &effect)
    {
        if (!lifecycle_.receiveMode()) {
            return applySource(keyState, point, effect, false);
        }
        if (!lifecycle_.enterReceive()) {
            // A second drag while the first still lingers in Entered (its source died after the release) must not
            // inherit the first drag's entry, file data, right button or effect mask: its DragOver and Drop are
            // answered NONE for as long as that state lasts.
            resetEntry();
            effect = DROPEFFECT_NONE;
            return S_OK;
        }
        // On entry the effect carries the source's allowed mask; every later answer stays inside it.
        allowed_ = {.copy = (effect & DROPEFFECT_COPY) != 0, .move = (effect & DROPEFFECT_MOVE) != 0};
        entered_ = true;
        return applyReceive(data, keyState, point, effect, true, false);
    }

    void DropTarget::resetEntry() noexcept
    {
        entered_ = false;
        fileData_ = false;
        rightButton_ = false;
        allowed_ = {};
    }

    HRESULT DropTarget::DragOver(DWORD keyState, POINTL point, DWORD *effect) noexcept
    {
        return invoke(Entry::Over, nullptr, keyState, point, effect);
    }

    HRESULT DropTarget::dragOver(DWORD keyState, POINTL point, DWORD &effect)
    {
        if (!lifecycle_.receiveMode()) {
            return applySource(keyState, point, effect, false);
        }
        if (!entered_) {
            effect = DROPEFFECT_NONE;
            return S_OK;
        }
        return applyReceive(nullptr, keyState, point, effect, false, false);
    }

    HRESULT DropTarget::DragLeave() noexcept
    {
        return invoke(Entry::Leave, nullptr, 0, {}, nullptr);
    }

    HRESULT DropTarget::dragLeave()
    {
        // Only one OLE drag exists per desktop, so a DragLeave in receive mode belongs to the only live drag even
        // when this target already forgot its entry (a second drag's DragEnter was refused while the first
        // lingered in Entered). It must still end that state, or the overlay would keep swallowing panel clicks
        // until the ceiling; the Entered-to-Armed exchange is a no-op in Armed and Dropping.
        if (lifecycle_.receiveMode()) {
            lifecycle_.leaveReceive();
        }
        // OLE sends DragLeave when the overlay hides under a live drag (timeout, abort); forgetting the entry
        // unconditionally keeps that drag's mask from surviving into the next armed receive.
        resetEntry();
        return S_OK;
    }

    HRESULT DropTarget::Drop(IDataObject *data, DWORD keyState, POINTL point, DWORD *effect) noexcept
    {
        return invoke(Entry::Drop, data, keyState, point, effect);
    }

    HRESULT DropTarget::invoke(Entry entry, IDataObject *data, DWORD keyState, POINTL point, DWORD *effect) noexcept
    {
        try {
            if (entry == Entry::Leave) {
                return dragLeave();
            }
            if (effect == nullptr) {
                return E_INVALIDARG;
            }
            if (entry == Entry::Enter) {
                return dragEnter(data, keyState, point, *effect);
            }
            if (entry == Entry::Over) {
                return dragOver(keyState, point, *effect);
            }
            return drop(data, keyState, point, *effect);
        } catch (const std::bad_alloc &) {
            return exceptionResult(effect, E_OUTOFMEMORY);
        } catch (...) {
            return exceptionResult(effect, E_UNEXPECTED);
        }
    }

    HRESULT DropTarget::drop(IDataObject *data, DWORD keyState, POINTL point, DWORD &effect)
    {
        if (!lifecycle_.receiveMode()) {
            return applySource(keyState, point, effect, true);
        }
        // Only the drag that entered may take the Dropping transition: a drag whose DragEnter was refused while a
        // previous one lingers in Entered must not consume that state, so entered_ is required before the lifecycle
        // is asked.
        if (!entered_ || !lifecycle_.beginReceiveDrop()) {
            resetEntry();
            effect = DROPEFFECT_NONE;
            return S_OK;
        }
        entered_ = false;
        return applyReceive(data, keyState, point, effect, false, true);
    }

    HRESULT DropTarget::applySource(DWORD keyState, POINTL point, DWORD &effect, bool dropping)
    {
        // Outside a live source drag the overlay is inert: OLE does not re-hit-test after DRAGDROP_S_DROP, so a
        // slow peer can deliver DragOver/Drop after this receive timed out or after exceptionResult returned us to
        // Idle. Answering NONE keeps such a stray call from replaying a stale same-Far copy of our own selection.
        if (!lifecycle_.sourceMode()) {
            effect = DROPEFFECT_NONE;
            return S_OK;
        }
        const core::Point at{static_cast<int>(point.x), static_cast<int>(point.y)};
        const bool shift = (keyState & MK_SHIFT) != 0;
        const auto chosen = dropping ? session_.drop(at, shift) : session_.effect(at, shift);
        effect = nativeEffect(chosen);
        return S_OK;
    }

    HRESULT DropTarget::exceptionResult(DWORD *effect, HRESULT result) noexcept
    {
        if (effect != nullptr) {
            *effect = DROPEFFECT_NONE;
        }
        resetEntry();
        if (lifecycle_.receiveMode()) {
            lifecycle_.finishReceive();
        }
        return result;
    }

    HRESULT DropTarget::applyReceive(IDataObject *data, DWORD keyState, POINTL point, DWORD &effect, bool entering,
                                     bool dropping)
    {
        if (entering) {
            fileData_ = dropData_.offersFileDrop(reinterpret_cast<std::uintptr_t>(data));
            rightButton_ = false;
        }
        rightButton_ = rightButton_ || (keyState & MK_RBUTTON) != 0;
        const core::Point at{static_cast<int>(point.x), static_cast<int>(point.y)};
        const bool shift = (keyState & MK_SHIFT) != 0;
        auto chosen = fileData_ ? session_.receiveEffect(at, shift, allowed_) : core::Effect::None;
        if (!dropping) {
            effect = nativeEffect(chosen);
            return S_OK;
        }

        // None until Burlak takes part: no CF_HDROP, or a point outside the item rows (hover already said so).
        core::ReceiveDropOutcome outcome;
        if (chosen != core::Effect::None) {
            // The hover promised an effect, so from here Burlak has taken part: a CF_HDROP that cannot be read or
            // fails the path bounds, a Cancel from the menu the user was offered, or a refused refresh must leave
            // nothing behind, and the answer is the declined outcome, not None with its console paste.
            outcome = core::declinedReceiveOutcome();
            const auto paths = dropData_.fileDropPaths(reinterpret_cast<std::uintptr_t>(data));
            if (paths && rightButton_) {
                // TrackPopupMenu pumps COM, so another source's refused DragEnter can run resetEntry() while the
                // menu is up; the mask this drop was entered with is taken before the call, not read after it.
                const auto allowed = allowed_;
                chosen = core::dropMenuEffect(menu_.choose(lifecycle_.menuOwner(), at, allowed), allowed);
            }
            if (paths && chosen != core::Effect::None && lifecycle_.refreshReceive(at)) {
                outcome = session_.receiveDrop(*paths, at, chosen);
                if (outcome.setPerformedNone) {
                    // Windows reference: Handling Shell Data Transfer Scenarios, "Handling Optimized Move Operations"
                    // (learn.microsoft.com/windows/win32/shell/datascenarios#handling-optimized-move-operations).
                    // The performed effect NONE tells the source the receiver already moved the data, so it deletes
                    // nothing; the value returned to OLE stays COPY (see core::receiveDropOutcome for why not NONE).
                    static_cast<void>(
                        dropData_.setPerformedEffect(reinterpret_cast<std::uintptr_t>(data), core::Effect::None));
                }
            }
        }
        effect = nativeEffect(outcome.returnedEffect);
        resetEntry();
        lifecycle_.finishReceive();
        return S_OK;
    }

} // namespace burlak::drag
