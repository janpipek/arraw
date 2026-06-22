# Tab-Driven Mask and Spot Tools Implementation Plan

**Goal:** Make the Masks and Spots adjustment tabs control their corresponding
on-image overlays and editing modes, move the `M`/`Q` shortcuts to tab selection,
and remove the Masks and Spots buttons from the main toolbar.

**Architecture:** `MainWindow` remains the GUI wiring module. A small pure
`toolForAdjustmentTab()` function defines the transition from the selected tab
and current viewport tool to the next viewport tool. `MainWindow` applies that
transition on tab changes and when an image becomes available. Window-scoped
shortcut actions select the tabs without appearing in the toolbar. Mask and Spot
develop effects remain rendered at all times; only their overlays, handles, and
on-image interaction mode follow the selected tab.

**Tech Stack:** C++20, Qt 6 Widgets, Catch2 v3, CMake, Ninja.

## Behaviour Contract

- Selecting Masks activates `ImageViewport::ActiveTool::LocalMask`.
- Selecting Spots activates `ImageViewport::ActiveTool::SpotTool`.
- Leaving Masks or Spots deactivates that tool and hides its overlay.
- Selecting a non-tool tab does not disturb Crop, Straighten, or White Balance.
- `M` selects Masks and `Q` selects Spots; both are idempotent and reactivate the
  matching tool when their tab is already selected.
- Tab-driven tools remain inactive while no image is loaded or a load is running.
- Adding a Mask needs no special signal: its button lives on the Masks tab, which
  already owns tool activation.

## Task 1: Remove the superseded add-mask activation attempt

**Files:**

- Modify: `src/LocalAdjustmentPanel.h`
- Modify: `src/LocalAdjustmentPanel.cpp`
- Modify: `src/MainWindow.cpp`
- Modify: `tests/test_LocalAdjustmentPanel.cpp`

- [x] Remove the uncommitted `LocalAdjustmentPanel::maskAdded` signal, emission,
  connection, and signal-count assertion.
- [x] Keep add/select/commit behavior otherwise unchanged.

## Task 2: Specify tab-to-tool transitions test-first

**Files:**

- Create: `src/AdjustmentTabTool.h`
- Create: `src/AdjustmentTabTool.cpp`
- Create: `tests/test_AdjustmentTabTool.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [x] Add one failing Catch2 test for Masks selecting `LocalMask`.
- [x] Implement the minimal pure mapping.
- [x] Add cases for Spots, leaving either overlay tab, preserving unrelated
  modal tools, and the disabled/no-image state.
- [x] Keep the interface value-based and independent of Qt widgets so the
  behavior is testable without constructing the RHI-backed `MainWindow`.

## Task 3: Make adjustment tabs drive viewport tools

**Files:**

- Modify: `src/MainWindow.h`
- Modify: `src/MainWindow.cpp`

- [x] Store both `masksTabIndex` and `spotsTabIndex`.
- [x] Connect `QTabWidget::currentChanged` to one synchronization method.
- [x] Apply `toolForAdjustmentTab()` and call `ImageViewport::setActiveTool()`
  only when the desired tool differs.
- [x] Re-run synchronization when tools become enabled after image load.
- [x] Deactivate Mask/Spot modes while loading or when no image is available.
- [x] Remove the old viewport-to-Masks-tab synchronization so control flows in
  one direction: adjustment tab to viewport tool.

## Task 4: Move `M` and `Q` off the toolbar

**Files:**

- Modify: `src/MainWindow.h`
- Modify: `src/MainWindow.cpp`

- [x] Remove the Masks and Spots toolbar actions from `setupToolbar()` and the
  modal `QActionGroup` dispatch.
- [x] Remove their checked-state handling from `syncToolActions()`.
- [x] Add window-scoped shortcut actions that are not inserted into a toolbar.
- [x] Make `M` select Masks and `Q` select Spots. Explicitly synchronize after
  selection so pressing a shortcut on the already-current tab is idempotent.
- [x] Keep shortcut enablement aligned with the existing image/loading state.

## Task 5: Documentation and verification

**Files:**

- Modify: `README.md`
- Modify: `docs/keybindings.md`

- [x] Describe `M` and `Q` as adjustment-tab shortcuts rather than toolbar tools.
- [x] Run `just format-check` (blocked only by pre-existing unrelated formatting drift).
- [x] Build `arraw_tests` and run the focused tab-tool tests.
- [x] Run `ctest --test-dir build --output-on-failure`.
- [ ] Manually smoke-test tab clicks and `M`/`Q` with a loaded image, confirming
  overlays disappear when leaving their tabs and no Mask/Spot toolbar buttons
  remain.
- [x] Review the diff against this plan and `docs/code_guidelines.md`, then commit.
