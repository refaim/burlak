#include "core/Peers.hpp"

#include "core/Geometry.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace burlak::core
{

    void PeerRegistry::begin(std::uint64_t nonce)
    {
        entries_.clear();
        nextOrder_ = 0;
        nonce_ = nonce == 0 ? std::nullopt : std::optional{nonce};
    }

    void PeerRegistry::end()
    {
        entries_.clear();
        nonce_.reset();
    }

    bool PeerRegistry::add(const PeerHello &hello, PeerIdentity sender)
    {
        if (!nonce_ || hello.echoNonce != *nonce_ || hello.nonce == 0 || sender.process == 0 || sender.window == 0 ||
            sender.process != hello.process || sender.window != hello.tool) {
            return false;
        }
        const auto samePeer = [&](const Entry &entry) {
            return entry.peer.process == hello.process && entry.peer.window == hello.tool;
        };
        std::erase_if(entries_, samePeer);
        if (entries_.size() == peerRegistryLimit) {
            entries_.erase(entries_.begin());
        }
        entries_.push_back(Entry{.peer = {.window = hello.tool,
                                          .host = hello.host,
                                          .lastFocus = hello.lastFocus,
                                          .process = hello.process,
                                          .nonce = hello.nonce},
                                 .order = nextOrder_++});
        return true;
    }

    std::optional<Peer> PeerRegistry::select(NativeWindow host, NativeWindow ownHost) const
    {
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

    bool IncomingPeerRegistry::begin(const PeerAnnouncement &announcement, std::uint64_t receiverNonce)
    {
        if (announcement.action != PeerAnnouncementAction::Begin || announcement.source.process == 0 ||
            announcement.source.window == 0 || announcement.nonce == 0 || receiverNonce == 0) {
            return false;
        }
        const auto sameSource = [&](const Entry &entry) { return entry.source == announcement.source; };
        std::erase_if(entries_, sameSource);
        if (entries_.size() == peerRegistryLimit) {
            entries_.erase(entries_.begin());
        }
        entries_.push_back(
            Entry{.source = announcement.source, .sourceNonce = announcement.nonce, .receiverNonce = receiverNonce});
        return true;
    }

    void IncomingPeerRegistry::end(const PeerAnnouncement &announcement)
    {
        if (announcement.action != PeerAnnouncementAction::End) {
            return;
        }
        std::erase_if(entries_, [&](const Entry &entry) {
            return entry.source == announcement.source && entry.sourceNonce == announcement.nonce;
        });
    }

    std::optional<PendingPeerDrop> IncomingPeerRegistry::accept(PeerIdentity sender, Drop drop)
    {
        const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &candidate) {
            return candidate.source == sender && candidate.receiverNonce == drop.nonce;
        });
        if (entry == entries_.end()) {
            return std::nullopt;
        }
        const auto process = entry->source.process;
        entries_.erase(entry);
        return PendingPeerDrop{.drop = std::move(drop), .sourceProcess = process};
    }

    std::size_t IncomingPeerRegistry::size() const
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
