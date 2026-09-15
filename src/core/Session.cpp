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

        [[nodiscard]] bool extractRecipe(IPanels &panels, IFarHost &host, const ExtractionRecipe &recipe,
                                         std::wstring_view requestedDirectory)
        {
            // Only the panel's identity matters: the same open archive of the same plugin, still a virtual file
            // panel. GetFilesW receives the recipe's own item records, so Far's current cursor and selection, which
            // move whenever the user drags across the panel, are deliberately not compared. The handle alone is a
            // heap pointer the plugin can reuse for an archive opened during the drag, so the location (inner
            // directory and host archive) is compared as well.
            const auto panel = panels.panel(PanelSide::Active);
            if (!panels.currentWindowIsPanels() || !extractionPanelMatches(panel, recipe) ||
                panels.pluginDirectory(PanelSide::Active) != recipe.location) {
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
        if (tool.active()) {
            return false;
        }
        cleanup();
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
            // A replay queued by the previous drag is obsolete once a new gesture starts: the same Far-thread call
            // that begins this drag would otherwise consume it against the context just built from the new press.
            pendingDrop_.reset();
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

    void Session::endSource()
    {
        // The tool window calls this when its OLE drag ends. Dropping the hover context here is enough to disarm a
        // stray same-Far drop: effect() and drop() consult only hoverContext_, so without it drop() returns None and
        // no pendingDrop_ can be posted. The Far-thread drop/identity snapshot (farContext_, plan_, pendingDrop_) is
        // left untouched because a same-Far release may still have its replay synchro in flight; it is read only when
        // a pendingDrop_ exists, which now cannot be created afresh, and the next begin rebuilds it.
        hoverContext_.reset();
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

    bool Session::requestReceiveSnapshot(Point point)
    {
        bool accepted{};
        {
            const std::lock_guard lock{pendingMutex_};
            accepted = !pendingReceivePoint_;
            if (accepted) {
                pendingReceivePoint_ = point;
            }
        }
        if (accepted) {
            host_.postSynchro();
        }
        return accepted;
    }

    std::optional<ReceiveSnapshot> Session::takeReceiveSnapshot()
    {
        const std::lock_guard lock{pendingMutex_};
        return std::exchange(completedReceiveSnapshot_, std::nullopt);
    }

    bool Session::requestReceiveRefresh(Point point)
    {
        bool accepted{};
        {
            const std::lock_guard lock{pendingMutex_};
            accepted = receiveSnapshot_.has_value() && !pendingReceiveRefresh_;
            if (accepted) {
                // The request carries the identity Drop is acting on, so Far's thread answers for that drop even
                // if the receive slot is replaced or cleared before synchro runs.
                pendingReceiveRefresh_ = PendingReceiveRefresh{point, *receiveSnapshot_};
            }
        }
        if (accepted) {
            host_.postSynchro();
        }
        return accepted;
    }

    std::optional<bool> Session::takeReceiveRefresh()
    {
        const std::lock_guard lock{pendingMutex_};
        return std::exchange(completedReceiveRefresh_, std::nullopt);
    }

    void Session::prepareReceive(ReceiveSnapshot snapshot)
    {
        const std::lock_guard lock{pendingMutex_};
        receiveSnapshot_ = std::move(snapshot);
    }

    void Session::cancelReceive()
    {
        const std::lock_guard lock{pendingMutex_};
        receiveSnapshot_.reset();
        pendingReceiveRefresh_.reset();
        completedReceiveRefresh_.reset();
    }

    Effect Session::receiveEffect(Point point, bool shift, AllowedEffects allowed) const
    {
        const std::lock_guard lock{pendingMutex_};
        return receiveSnapshot_ ? receivePolicy_.effect(*receiveSnapshot_, point, shift, allowed) : Effect::None;
    }

    NativeWindow Session::receiveOwner() const
    {
        const std::lock_guard lock{pendingMutex_};
        return receiveSnapshot_.and_then([](const ReceiveSnapshot &snapshot) { return snapshot.host; })
            .transform([](const HostWindow &host) { return host.handle; })
            .value_or(0);
    }

    ReceiveDropOutcome Session::receiveDrop(std::span<const std::wstring> paths, Point point, Effect effect)
    {
        std::optional<ReceiveSnapshot> snapshot;
        {
            const std::lock_guard lock{pendingMutex_};
            snapshot = receiveSnapshot_;
        }
        const auto destination = snapshot ? receivePolicy_.destination(*snapshot, point) : std::nullopt;
        const bool validEffect = effect == Effect::Copy || effect == Effect::Move;
        if (!destination || !validEffect || paths.empty()) {
            return {};
        }
        const auto owner = snapshot->host.transform([](const HostWindow &host) { return host.handle; }).value_or(0);
        // A run under %TEMP%\Burlak is intentionally just another source. IFileOperation can rename its contents
        // on the same volume; the source-side retention and timer sweep handle whatever remains.
        const bool completed = shell_.copy(paths, destination->directory, effect, owner).has_value();
        if (completed) {
            {
                const std::lock_guard lock{pendingMutex_};
                pendingRedraw_ = destination->side;
            }
            host_.postSynchro();
        }
        return receiveDropOutcome(effect, completed);
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
        std::optional<Point> receivePoint;
        std::optional<PendingReceiveRefresh> receiveRefresh;
        std::optional<PanelSide> redraw;
        {
            const std::lock_guard lock{pendingMutex_};
            extraction = std::exchange(pendingExtraction_, std::nullopt);
            if (!extraction) {
                receivePoint = std::exchange(pendingReceivePoint_, std::nullopt);
                if (!receivePoint) {
                    receiveRefresh = std::exchange(pendingReceiveRefresh_, std::nullopt);
                }
                if (!receivePoint && !receiveRefresh) {
                    redraw = std::exchange(pendingRedraw_, std::nullopt);
                }
                if (!receivePoint && !receiveRefresh && !redraw) {
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
        if (receivePoint) {
            const auto geometry = screen_.cellGeometryAt(*receivePoint);
            ReceiveSnapshot snapshot{
                .panelsWindow = panels_.currentWindowIsPanels(),
                .panels = {panels_.panel(PanelSide::Active), panels_.panel(PanelSide::Passive)},
                .directories = {panels_.directory(PanelSide::Active), panels_.directory(PanelSide::Passive)},
                .host = screen_.hostWindowAt(*receivePoint),
                .geometry = geometry ? std::optional{*geometry} : std::nullopt};
            const std::lock_guard lock{pendingMutex_};
            completedReceiveSnapshot_ = std::move(snapshot);
            return std::nullopt;
        }
        if (receiveRefresh) {
            const auto geometry = screen_.cellGeometryAt(receiveRefresh->point);
            const ReceiveSnapshot fresh{
                .panelsWindow = panels_.currentWindowIsPanels(),
                .panels = {panels_.panel(PanelSide::Active), panels_.panel(PanelSide::Passive)},
                .directories = {panels_.directory(PanelSide::Active), panels_.directory(PanelSide::Passive)},
                .host = screen_.hostWindowAt(receiveRefresh->point),
                .geometry = geometry ? std::optional{*geometry} : std::nullopt};
            const bool matches = receivePolicy_.sameIdentity(receiveRefresh->before, fresh, receiveRefresh->point);
            {
                const std::lock_guard lock{pendingMutex_};
                completedReceiveRefresh_ = matches;
            }
            if (!matches) {
                report(host_, L"Panel changed during the drop; the drop was cancelled.");
            }
            return std::nullopt;
        }
        if (redraw) {
            panels_.updateAndRedraw(*redraw);
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
        cleanupDirectory_.reset();
    }

} // namespace burlak::core
