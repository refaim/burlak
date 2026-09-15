#pragma once

#include "core/Interfaces.hpp"

#include <oleidl.h>
#include <windows.h>

namespace burlak::drag
{

    class IReceiveLifecycle
    {
      public:
        virtual ~IReceiveLifecycle() = default;
        [[nodiscard]] virtual bool receiveMode() const noexcept = 0;
        // True only while our own OLE drag is running, so the same-Far replay path is taken for a drop onto our
        // overlay during a source drag and never for a stray Drop/DragOver that reaches a torn-down overlay in Idle.
        [[nodiscard]] virtual bool sourceMode() const noexcept = 0;
        [[nodiscard]] virtual bool enterReceive() = 0;
        virtual void leaveReceive() = 0;
        [[nodiscard]] virtual bool beginReceiveDrop() = 0;
        [[nodiscard]] virtual bool refreshReceive(core::Point point) = 0;
        // TrackPopupMenu accepts only an owner window of the calling thread, so the right-drop menu belongs to
        // the overlay itself; the host window, which another process owns, stays the owner of IFileOperation.
        [[nodiscard]] virtual core::NativeWindow menuOwner() const noexcept = 0;
        virtual void finishReceive() noexcept = 0;
    };

    class DropTarget final : public IDropTarget
    {
      public:
        DropTarget(core::IDropSession &session, core::IDropData &dropData, core::IDropMenu &menu,
                   IReceiveLifecycle &lifecycle);

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void **object) noexcept override;
        ULONG STDMETHODCALLTYPE AddRef() noexcept override;
        ULONG STDMETHODCALLTYPE Release() noexcept override;
        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD keyState, POINTL point,
                                            DWORD *effect) noexcept override;
        HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD *effect) noexcept override;
        HRESULT STDMETHODCALLTYPE DragLeave() noexcept override;
        HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD keyState, POINTL point,
                                       DWORD *effect) noexcept override;

      private:
        enum class Entry : std::uint8_t
        {
            Enter,
            Over,
            Leave,
            Drop
        };

        [[nodiscard]] HRESULT applySource(DWORD keyState, POINTL point, DWORD &effect, bool dropping);
        [[nodiscard]] HRESULT applyReceive(IDataObject *data, DWORD keyState, POINTL point, DWORD &effect,
                                           bool entering, bool dropping);
        [[nodiscard]] HRESULT dragEnter(IDataObject *data, DWORD keyState, POINTL point, DWORD &effect);
        [[nodiscard]] HRESULT dragOver(DWORD keyState, POINTL point, DWORD &effect);
        [[nodiscard]] HRESULT dragLeave();
        [[nodiscard]] HRESULT drop(IDataObject *data, DWORD keyState, POINTL point, DWORD &effect);
        [[nodiscard]] HRESULT invoke(Entry entry, IDataObject *data, DWORD keyState, POINTL point,
                                     DWORD *effect) noexcept;
        [[nodiscard]] HRESULT exceptionResult(DWORD *effect, HRESULT result) noexcept;
        void resetEntry() noexcept;

        core::IDropSession &session_;
        core::IDropData &dropData_;
        core::IDropMenu &menu_;
        IReceiveLifecycle &lifecycle_;
        ULONG references_{1};
        bool entered_{};
        bool fileData_{};
        bool rightButton_{};
        core::AllowedEffects allowed_{};
    };

} // namespace burlak::drag
