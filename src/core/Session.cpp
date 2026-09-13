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

    } // namespace

    Session::Session(IPanels &panels, IFarHost &host, IScreen &screen, IInput &input, IFiles &files)
        : panels_{panels}, host_{host}, screen_{screen}, input_{input}, files_{files}
    {
    }

    bool Session::begin(IDragTool &tool, DragStart start)
    {
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
        {
            const std::lock_guard lock{pendingMutex_};
            extraction = std::exchange(pendingExtraction_, std::nullopt);
            pending = std::exchange(pendingDrop_, std::nullopt);
        }
        if (extraction) {
            ExtractionRecipe recipe;
            {
                const std::lock_guard lock{pendingMutex_};
                // A pending extraction is published only after begin stored this plugin plan; the fallback keeps
                // invariant loss inside the export firewall instead of dereferencing an empty optional.
                recipe =
                    plan_.and_then([](const Plan &stored) { return stored.extraction; }).value_or(ExtractionRecipe{});
            }
            const bool succeeded = host_.extract(recipe.panel, recipe.items, recipe.module, *extraction).has_value();
            if (!succeeded) {
                const auto separator = recipe.module.path.find_last_of(L"\\/");
                const auto name = recipe.module.path.substr(separator == std::wstring::npos ? 0 : separator + 1);
                report(host_, L"Could not extract files with " + name + L".");
            }
            return succeeded;
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

        Plan originalPlan;
        {
            const std::lock_guard lock{pendingMutex_};
            originalPlan = *plan_;
        }
        std::vector<std::wstring> freshPaths;
        if (originalPlan.extraction) {
            auto items = panels_.selectedItems(PanelSide::Active);
            std::erase_if(
                items, [](const Item &item) { return item.name.empty() || item.name == L"." || item.name == L".."; });
            if (items == originalPlan.extraction->items) {
                freshPaths = originalPlan.paths;
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

} // namespace burlak::core
