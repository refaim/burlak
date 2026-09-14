#include "core/Session.hpp"

#include "core/DragPlan.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace burlak::core
{

    namespace
    {

        void report(IFarHost &host, std::wstring line)
        {
            const std::array lines{std::move(line)};
            host.message(L"Burlak", lines);
        }

        [[nodiscard]] bool complete(ReplayOutcome outcome, std::size_t expected)
        {
            return replayOutcome(outcome).has_value() && outcome.written == expected;
        }

        [[nodiscard]] std::wstring pluginName(const PluginModule &module)
        {
            const auto separator = module.path.find_last_of(L"\\/");
            return module.path.substr(separator == std::wstring::npos ? 0 : separator + 1);
        }

        [[nodiscard]] bool extractionPanelMatches(const std::optional<PanelInfo> &panel, const ExtractionRecipe &recipe)
        {
            return panel && panel->visible && panel->plugin && !panel->realNames && panel->filePanel &&
                   panel->handle == recipe.panel && panel->owner == recipe.owner;
        }

        [[nodiscard]] std::wstring refusalText(PeerDropRefusal refusal)
        {
            constexpr std::array reasons{
                L"the current window is not the panels",    L"the console geometry is unavailable",
                L"the Far host window is unavailable",      L"the point is not on a panel item row",
                L"the destination is not a file panel",     L"the destination panel has no real directory",
                L"the destination directory is unavailable"};
            return reasons.at(static_cast<std::size_t>(refusal));
        }

        void reportPeerRefusal(IFarHost &host, std::wstring reason)
        {
            report(host, L"Burlak: drop here is not possible: " + std::move(reason));
        }

        class PeerRunCleanup final
        {
          public:
            PeerRunCleanup(IFiles &files, const std::optional<std::wstring> &directory)
                : files_{files}, directory_{directory}
            {
            }

            ~PeerRunCleanup()
            {
                if (directory_) {
                    static_cast<void>(files_.removeTree(*directory_));
                }
            }

          private:
            IFiles &files_;
            const std::optional<std::wstring> &directory_;
        };

        [[nodiscard]] bool extractRecipe(IPanels &panels, IFarHost &host, const ExtractionRecipe &recipe,
                                         std::wstring_view requestedDirectory)
        {
            const auto panel = panels.panel(PanelSide::Active);
            if (!panels.currentWindowIsPanels() || !extractionPanelMatches(panel, recipe)) {
                report(host, L"Plugin panel changed during the drag; the drop was cancelled.");
                return false;
            }
            auto items = panels.selectedItems(PanelSide::Active);
            const auto selectedCount =
                panel.transform([](const PanelInfo &current) { return current.selectedItems; }).value_or(0);
            const bool completeSnapshot = items.size() == selectedCount;
            std::erase_if(
                items, [](const Item &item) { return item.name.empty() || item.name == L"." || item.name == L".."; });
            if (!completeSnapshot || items != recipe.items) {
                report(host, L"Plugin panel changed during the drag; the drop was cancelled.");
                return false;
            }
            const auto module = host.pluginModule(recipe.owner);
            if (!module || *module != recipe.module) {
                report(host, L"Plugin panel changed during the drag; the drop was cancelled.");
                return false;
            }

            const auto destination = host.extract(recipe.panel, recipe.items, recipe.module, requestedDirectory);
            if (!destination) {
                report(host, L"Could not extract files with " + pluginName(recipe.module) + L".");
                return false;
            }
            if (*destination != requestedDirectory) {
                // OLE already advertised paths under the run directory. Following a rewritten DestPath would make
                // those paths stale, so the safe contract is to cancel instead of attempting a second relocation.
                report(host, pluginName(recipe.module) + L" extracted files to a different directory; the drop was "
                                                         L"cancelled.");
                return false;
            }
            return true;
        }

    } // namespace

    Session::Session(IPanels &panels, IFarHost &host, IScreen &screen, IInput &input, IFiles &files, IShell &shell)
        : panels_{panels}, host_{host}, screen_{screen}, input_{input}, files_{files}, shell_{shell}
    {
    }

    bool Session::begin(IDragTool &tool, DragStart start)
    {
        cleanup();
        files_.sweep();
        if (!panels_.currentWindowIsPanels()) {
            return false;
        }
        auto plan = DragPlan{panels_, host_, files_}.build();
        if (!plan) {
            return false;
        }
        const auto discard = [this, &plan] {
            if (plan->extraction) {
                static_cast<void>(files_.removeTree(plan->extraction->directory));
            }
        };
        if (!tool.start()) {
            discard();
            return false;
        }

        if (!screen_.buttonDown(start.button)) {
            discard();
            return false;
        }
        // The panel that handled the press is active by the time Far invokes the input export
        // (Far source: far/filelist.cpp, FileList::ProcessMouse).
        // Far reports the current item as its one selected item when there is no explicit selection
        // (Far source: far/filelist.cpp, FileList::GetSelCount and FileList::PluginGetSelectedPanelItem).
        DropContext context{.press = start.press,
                            .source = PanelSide::Active,
                            .panels = {panels_.panel(PanelSide::Active), panels_.panel(PanelSide::Passive)},
                            .host = screen_.hostWindow(),
                            .geometry = std::nullopt,
                            .panelsWindow = true,
                            .sourcePaths = plan->paths,
                            .destinationDirectory = panels_.directory(PanelSide::Passive)};
        if (const auto geometry = screen_.cellGeometry()) {
            context.geometry = *geometry;
        }
        // Namespace lookup is slow, so 1.2.0 checked the physical button before paying that cost and
        // again immediately before synthesizing the release.
        // The synchronous prepare message copies the complete hover and identity snapshot; the tool
        // thread reads that value only after this Far-thread call and before showAndArm.
        if (!tool.prepare(plan->paths, start.button, plan->extraction.has_value(), context)) {
            discard();
            return false;
        }
        // This Far-thread snapshot is stored before showAndArm; OLE cannot publish PendingDrop until
        // afterward. ToolWindow's prepare message separately transfers the cosmetic hover snapshot by value.
        {
            const std::lock_guard lock{pendingMutex_};
            cleanupDirectory_ =
                plan->extraction.transform([](const ExtractionRecipe &recipe) { return recipe.directory; });
            plan_ = std::move(*plan);
        }
        farContext_ = context;
        if (!context.host) {
            tool.abort();
            cleanup();
            return false;
        }
        if (!screen_.buttonDown(start.button)) {
            tool.abort();
            cleanup();
            return false;
        }

        // The fresh press over the tool window must follow this release in the serial input stream.
        input_.release(start.button);
        const bool shown = tool.showAndArm();
        if (!shown) {
            cleanup();
        }
        return shown;
    }

    void Session::prepare(DropContext context)
    {
        hoverContext_ = std::move(context);
    }

    Effect Session::effect(Point point, bool shift) const
    {
        return hoverContext_ ? dropPolicy_.effect(*hoverContext_, point, shift) : Effect::None;
    }

    Effect Session::drop(Point point, bool shift)
    {
        if (!hoverContext_) {
            return Effect::None;
        }
        const auto decision = dropPolicy_.drop(*hoverContext_, point, shift);
        if (decision.effect == Effect::None) {
            return Effect::None;
        }
        {
            const std::lock_guard lock{pendingMutex_};
            pendingDrop_ = PendingDrop{point, decision.events[1].at, decision.effect};
        }
        host_.postSynchro();
        return decision.effect;
    }

    bool Session::receivePeerDrop(PendingPeerDrop drop)
    {
        std::unique_lock lock{pendingMutex_};
        // Any same-integrity process that completes the one-drag protocol can name source paths, but it can
        // only request a copy into the real directory this Far revalidates on its own thread below.
        if (pendingPeerDrops_.size() == peerRegistryLimit) {
            return false;
        }
        pendingPeerDrops_.push_back(std::move(drop));
        lock.unlock();
        host_.postSynchro();
        return true;
    }

    bool Session::requestExtraction()
    {
        std::unique_lock lock{pendingMutex_};
        if (!plan_ || !plan_->extraction || pendingExtraction_) {
            return false;
        }
        // Only this directory value crosses from the tool thread; the panel handle, items, and module
        // remain in the Far-thread plan until ProcessSynchroEventW consumes the request.
        pendingExtraction_ = plan_->extraction->directory;
        lock.unlock();
        host_.postSynchro();
        return true;
    }

    std::optional<bool> Session::synchro()
    {
        std::optional<std::wstring> extraction;
        std::optional<PendingDrop> pending;
        std::optional<PendingPeerDrop> peerDrop;
        {
            const std::lock_guard lock{pendingMutex_};
            extraction = std::exchange(pendingExtraction_, std::nullopt);
            if (!extraction) {
                if (!pendingPeerDrops_.empty()) {
                    peerDrop = std::move(pendingPeerDrops_.front());
                    pendingPeerDrops_.pop_front();
                } else {
                    pending = std::exchange(pendingDrop_, std::nullopt);
                }
            }
        }
        if (extraction) {
            const std::lock_guard lock{pendingMutex_};
            // The tool thread can publish only the directory value from a stored extraction plan. Keeping this
            // reference under the mutex leaves the Far-owned native item buffers alive through GetFilesW.
            return plan_
                .and_then([&](const Plan &stored) {
                    return stored.extraction.transform([&](const ExtractionRecipe &recipe) {
                        return extractRecipe(panels_, host_, recipe, *extraction);
                    });
                })
                .value_or(false);
        }
        if (peerDrop) {
            // Taking ownership before validating the destination lets this Far remove an extracted run even when
            // the recorded point is refused; a failed rename leaves cleanup to the source and dead-owner sweep.
            auto adopted = files_.adoptPeerPaths(peerDrop->drop.paths, peerDrop->sourceProcess);
            const PeerRunCleanup cleanup{files_, adopted.cleanupDirectory};
            const auto host = screen_.hostWindowAt(peerDrop->drop.at);
            if (!host) {
                reportPeerRefusal(host_, refusalText(PeerDropRefusal::HostUnavailable));
                return std::nullopt;
            }
            const auto geometry = screen_.cellGeometryAt(peerDrop->drop.at);
            const PeerReceiveContext context{
                .panelsWindow = panels_.currentWindowIsPanels(),
                .panels = {panels_.panel(PanelSide::Active), panels_.panel(PanelSide::Passive)},
                .directories = {panels_.directory(PanelSide::Active), panels_.directory(PanelSide::Passive)},
                .geometry = geometry ? std::optional{*geometry} : std::nullopt};
            const auto destination = peerDropPolicy_.destination(context, peerDrop->drop.at);
            if (!destination) {
                reportPeerRefusal(host_, refusalText(destination.error()));
                return std::nullopt;
            }
            const auto copied = shell_.copy(adopted.paths, destination->directory, peerDrop->drop.effect, host->handle);
            if (!copied) {
                reportPeerRefusal(host_, L"the shell copy failed");
                return std::nullopt;
            }
            panels_.updateAndRedraw(destination->side);
            return std::nullopt;
        }
        if (!pending) {
            return std::nullopt;
        }

        const auto panelsWindow = panels_.currentWindowIsPanels();
        const std::array panels{panels_.panel(PanelSide::Active), panels_.panel(PanelSide::Passive)};
        const auto destinationDirectory = panels_.directory(PanelSide::Passive);
        const auto host = screen_.hostWindowAt(pending->point);
        const auto geometryResult = screen_.cellGeometryAt(pending->point);

        // OLE owns ordinary keyboard input during its drag loop, but a macro or timer can navigate and a console
        // resize can remap rows (Far sources: far/filelist.cpp, FileList::MoveToMouse; far/filepanels.cpp,
        // FilePanels::SwapPanels).
        if (!farContext_) {
            report(host_, L"Panels changed during the drag; the drop was cancelled.");
            return std::nullopt;
        }

        bool extractionPlan{};
        std::vector<std::wstring> originalPaths;
        {
            const std::lock_guard lock{pendingMutex_};
            extractionPlan =
                plan_.transform([](const Plan &stored) { return stored.extraction.has_value(); }).value_or(false);
            originalPaths = plan_.transform([](const Plan &stored) { return stored.paths; }).value_or(originalPaths);
        }
        std::vector<std::wstring> freshPaths;
        if (extractionPlan) {
            auto items = panels_.selectedItems(PanelSide::Active);
            const bool completeSnapshot =
                panels[0]
                    .transform([&](const PanelInfo &panel) { return items.size() == panel.selectedItems; })
                    .value_or(false);
            std::erase_if(
                items, [](const Item &item) { return item.name.empty() || item.name == L"." || item.name == L".."; });
            bool sameItems{};
            {
                const std::lock_guard lock{pendingMutex_};
                sameItems = plan_
                                .transform([&](const Plan &stored) {
                                    return stored.extraction
                                        .transform([&](const ExtractionRecipe &recipe) {
                                            return completeSnapshot && items == recipe.items;
                                        })
                                        .value_or(false);
                                })
                                .value_or(false);
            }
            if (sameItems) {
                freshPaths = std::move(originalPaths);
            }
        } else if (const auto freshPlan = DragPlan{panels_, host_, files_}.build()) {
            freshPaths = freshPlan->paths;
        }

        DropContext fresh{.press = farContext_->press,
                          .source = farContext_->source,
                          .panels = panels,
                          .host = host,
                          .geometry = std::nullopt,
                          .panelsWindow = panelsWindow,
                          .sourcePaths = std::move(freshPaths),
                          .destinationDirectory = destinationDirectory};
        if (geometryResult) {
            fresh.geometry = *geometryResult;
        }
        if (!dropPolicy_.sameIdentity(*farContext_, fresh)) {
            report(host_, L"Panels changed during the drag; the drop was cancelled.");
            return std::nullopt;
        }
        const auto decision = dropPolicy_.drop(fresh, pending->point, pending->effect == Effect::Move);
        // MoveToMouse consumes the freshly mapped row, so accepting a different cell could change the target
        // directory (Far source: far/filelist.cpp, FileList::MoveToMouse).
        if (decision.effect != pending->effect || decision.events[1].at != pending->cell) {
            report(host_, L"Panels changed during the drag; the drop was cancelled.");
            return std::nullopt;
        }

        const auto outcome = input_.replay(decision.events);
        if (complete(outcome, decision.events.size())) {
            return std::nullopt;
        }

        bool recoveryFailed = false;
        if (outcome.written == 1) {
            // A buttonless event clears Far's static DragX even at the press cell (Far source:
            // far/panel.cpp, Panel::ProcessMouseDrag).
            const std::array release{MouseEvent{.at = decision.events[0].at, .mods = decision.events[0].mods}};
            recoveryFailed = !complete(input_.replay(release), release.size());
        }
        report(host_, recoveryFailed ? L"Far could not receive the drop input or its recovery release."
                                     : L"Far could not receive the complete drop input.");
        return std::nullopt;
    }

    void Session::cleanup()
    {
        std::optional<std::wstring> directory;
        {
            const std::lock_guard lock{pendingMutex_};
            directory = std::exchange(cleanupDirectory_, std::nullopt);
        }
        if (directory) {
            // Drop targets may keep reading after OLE returns; a sharing failure is intentionally left for sweep.
            static_cast<void>(files_.removeTree(*directory));
        }
    }

    void Session::retain()
    {
        const std::lock_guard lock{pendingMutex_};
        if (cleanupDirectory_) {
            // Rewriting placeholder contents does not update their parent directory's last-write time on NTFS, so
            // retention starts the grace clock here before another begin can sweep. A failed refresh is ignored:
            // sweep may then run early, which shortens retention but is not unsafe.
            static_cast<void>(files_.touch(*cleanupDirectory_));
        }
        // A successful peer handoff transfers the run by rename. If that rename later fails, leaving the source
        // run here lets the receiving shell finish even when this drag session or Far shuts down meanwhile.
        cleanupDirectory_.reset();
    }

} // namespace burlak::core
