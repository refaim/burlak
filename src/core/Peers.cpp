#include "core/Peers.hpp"

#include "core/Geometry.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace burlak::core
{

    void PeerRegistry::add(const PeerHello &hello, std::uint64_t now)
    {
        const auto samePeer = [&](const Entry &entry) {
            return entry.peer.process == hello.process && entry.peer.window == hello.tool;
        };
        std::erase_if(entries_, samePeer);
        entries_.push_back(Entry{
            .peer = {.window = hello.tool, .host = hello.host, .lastFocus = hello.lastFocus, .process = hello.process},
            .seen = now,
            .order = nextOrder_++});
    }

    void PeerRegistry::expire(std::uint64_t now)
    {
        std::erase_if(entries_,
                      [now](const Entry &entry) { return now >= entry.seen && now - entry.seen > peerExpiryTicks; });
    }

    void PeerRegistry::clear()
    {
        entries_.clear();
    }

    std::optional<Peer> PeerRegistry::select(NativeWindow host, NativeWindow ownHost, std::uint64_t now)
    {
        expire(now);
        if (host == ownHost) {
            return std::nullopt;
        }
        std::optional<std::size_t> selected;
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            const auto &entry = entries_[index];
            if (entry.peer.host != host) {
                continue;
            }
            // A host that never delivers FOCUS_EVENT leaves zero here, so any peer with a recorded focus wins;
            // equal ticks (including all-zero hosts) fall back to the most recently received hello.
            const auto priority = std::pair{entry.peer.lastFocus, entry.order};
            if (!selected || priority > std::pair{entries_[*selected].peer.lastFocus, entries_[*selected].order}) {
                selected = index;
            }
        }
        return selected ? std::optional{entries_[*selected].peer} : std::nullopt;
    }

    std::size_t PeerRegistry::size() const
    {
        return entries_.size();
    }

    Effect peerMenuEffect(PeerMenuChoice choice)
    {
        constexpr std::array effects{Effect::None, Effect::Copy, Effect::Move};
        return effects.at(static_cast<std::size_t>(choice));
    }

    PeerDestinationResult PeerReceivePolicy::destination(const PeerReceiveContext &context, Point point) const
    {
        if (!context.panelsWindow) {
            return std::unexpected(PeerDropRefusal::NotPanelsWindow);
        }
        if (!context.geometry) {
            return std::unexpected(PeerDropRefusal::GeometryUnavailable);
        }
        const auto cell = toCell(point, *context.geometry);
        std::optional<std::size_t> selected;
        for (std::size_t index = 0; index < context.panels.size(); ++index) {
            if (context.panels[index] && isItemCell(*context.panels[index], cell)) {
                selected = index;
                break;
            }
        }
        if (!selected) {
            return std::unexpected(PeerDropRefusal::NotItemRow);
        }
        const auto &panel = *context.panels[*selected];
        if (!panel.filePanel) {
            return std::unexpected(PeerDropRefusal::NotFilePanel);
        }
        if (!panel.realNames) {
            return std::unexpected(PeerDropRefusal::NoRealNames);
        }
        const auto &directory = context.directories[*selected];
        if (!directory || directory->empty()) {
            return std::unexpected(PeerDropRefusal::DirectoryUnavailable);
        }
        constexpr std::array sides{PanelSide::Active, PanelSide::Passive};
        return PeerDestination{.side = sides[*selected], .directory = *directory};
    }

} // namespace burlak::core
