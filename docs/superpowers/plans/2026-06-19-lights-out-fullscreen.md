# Lights-Out & Fullscreen Focus Modes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two composable, session-transient view modes to `MainWindow` — OS fullscreen (`F11`) and lights-out hide-all-chrome (`F12`) — with `Escape` as a restore-all panic key.

**Architecture:** The snapshot/restore mechanism described in ADR 0025 as `setChromeVisible(bool)` is extracted into a small, widget-agnostic `ChromeHider` class so it can be unit-tested with plain `QWidget`s (MainWindow itself is not unit-testable — it builds an RHI-backed `ImageViewport`, which is why only the `MainWindowStatus` helper is tested today). `MainWindow` owns one `ChromeHider` wired to its six chrome widgets, plus thin fullscreen logic that Qt provides directly. Both modes are wired to checkable View-menu actions whose `QAction` shortcuts remain live even when the menu bar is hidden.

**Tech Stack:** C++20, Qt 6 Widgets, Catch2 v3, CMake + Ninja.

## Global Constraints

- **Language:** Modern C++20. (from AGENTS.md)
- **No Hungarian / `m_` prefixes.** Plain member names; strip any `m_` from files you touch. (from AGENTS.md)
- **`const` by default; `auto` where the type is obvious.** (from AGENTS.md)
- **No new compiler warnings** in touched code (`/W4` MSVC, `-Wall -Wextra -Wpedantic` elsewhere). (from AGENTS.md)
- **Catch2 v3 tests** live in `tests/`, link `arraw_core`, and are registered in `tests/CMakeLists.txt`'s `add_executable(arraw_tests ...)` list. Each test runs as its own ctest process with `QT_QPA_PLATFORM=offscreen`.
- **Design source of truth:** `docs/adr/0025-lights-out-and-fullscreen-modes.md`. Modes must **not** persist across restarts; `closeEvent` must restore chrome before `saveState()`.

### Build & test commands (Windows, per AGENTS.md + project memory)

Run from a **Developer PowerShell for VS 2022** (so `rc.exe`/`mt.exe` and the MSVC env are on PATH; vcpkg toolchain comes from `CMakePresets.json`):

```powershell
cmake --preset default          # only needed after CMakeLists changes
cmake --build build
ctest --test-dir build --output-on-failure
```

Run a single test file's cases: `ctest --test-dir build -R ChromeHider --output-on-failure`.

---

### Task 1: `ChromeHider` — testable snapshot/restore of chrome visibility

**Files:**
- Create: `src/ChromeHider.h`
- Create: `src/ChromeHider.cpp`
- Modify: `CMakeLists.txt:171` (add `src/ChromeHider.cpp` to `CORE_SOURCES`, after `src/MainWindowStatus.cpp`)
- Test: `tests/test_ChromeHider.cpp`
- Modify: `tests/CMakeLists.txt` (add `test_ChromeHider.cpp` to the `arraw_tests` source list)

**Interfaces:**
- Consumes: nothing (leaf utility).
- Produces:
  - `class ChromeHider`
  - `explicit ChromeHider(std::vector<QWidget*> widgets)`
  - `void hide()` — snapshot each widget's `isHidden()`, then `hide()` all; no-op if already hidden.
  - `void restore()` — re-apply the snapshot via `setVisible(!wasHidden)`; no-op if not hidden.
  - `bool hidden() const`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_ChromeHider.cpp`:

```cpp
#include "ChromeHider.h"
#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QWidget>

namespace {
// QWidget construction needs a QApplication. Mirrors test_CollapsiblePane.cpp:
// each Catch test is its own ctest process. Children of a host so isHidden()
// tracks the explicit hide() flag, not the "never shown" state of a top-level.
void ensureApp() {
    static int argc = 1;
    static char arg0[] = "arraw_tests";
    static char* argv[] = {arg0, nullptr};
    if (!qApp)
        new QApplication(argc, argv);
    if (!qobject_cast<QApplication*>(qApp))
        SKIP("needs a QApplication; run isolated (ctest does this per-test)");
}
} // namespace

TEST_CASE("hide() hides every chrome widget and reports hidden", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* a = new QWidget(&host);
    auto* b = new QWidget(&host);
    ChromeHider hider({a, b});

    hider.hide();

    CHECK(hider.hidden());
    CHECK(a->isHidden());
    CHECK(b->isHidden());
}

TEST_CASE("restore() brings back widgets that were visible", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* a = new QWidget(&host);
    auto* b = new QWidget(&host);
    ChromeHider hider({a, b});
    hider.hide();

    hider.restore();

    CHECK_FALSE(hider.hidden());
    CHECK_FALSE(a->isHidden());
    CHECK_FALSE(b->isHidden());
}

TEST_CASE("restore() keeps an already-hidden widget hidden", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* shown = new QWidget(&host);
    auto* alreadyHidden = new QWidget(&host);
    alreadyHidden->hide();
    ChromeHider hider({shown, alreadyHidden});

    hider.hide();
    hider.restore();

    CHECK_FALSE(shown->isHidden());   // was visible -> restored visible
    CHECK(alreadyHidden->isHidden()); // was hidden  -> stays hidden
}

TEST_CASE("a second hide() does not clobber the snapshot", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* shown = new QWidget(&host);
    auto* alreadyHidden = new QWidget(&host);
    alreadyHidden->hide();
    ChromeHider hider({shown, alreadyHidden});

    hider.hide();
    hider.hide(); // no-op: snapshot from the first call must survive
    hider.restore();

    CHECK_FALSE(shown->isHidden());
    CHECK(alreadyHidden->isHidden());
}

TEST_CASE("restore() before any hide() is a no-op", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* a = new QWidget(&host);
    ChromeHider hider({a});

    hider.restore();

    CHECK_FALSE(hider.hidden());
    CHECK_FALSE(a->isHidden());
}

TEST_CASE("nullptr widgets are ignored", "[chrome]") {
    ensureApp();
    QWidget host;
    auto* a = new QWidget(&host);
    ChromeHider hider({a, nullptr});

    hider.hide();   // must not crash on the nullptr
    hider.restore();

    CHECK_FALSE(a->isHidden());
}
```

- [ ] **Step 2: Register the new test, then run it to verify it fails to build**

In `tests/CMakeLists.txt`, add this line inside `add_executable(arraw_tests ...)` (e.g. right after `test_CollapsiblePane.cpp`):

```cmake
    test_ChromeHider.cpp
```

Run:
```powershell
cmake --preset default
cmake --build build
```
Expected: FAIL — compile error, `ChromeHider.h` not found / `ChromeHider` undefined.

- [ ] **Step 3: Write the header**

Create `src/ChromeHider.h`:

```cpp
#pragma once
#include <vector>

class QWidget;

// Hides a fixed set of chrome widgets together and restores each to exactly the
// visibility it had before — the snapshot/restore mechanism behind the lights-out
// mode (docs/adr/0025). Deliberately widget-agnostic so it is unit-testable with
// plain QWidgets, and so it only flips raw visibility: it never drives
// CollapsiblePane's dock/strip state machine, preserving the ADR 0012 invariant.
class ChromeHider {
public:
    // Widgets to hide together. Order is irrelevant; nullptrs are ignored.
    explicit ChromeHider(std::vector<QWidget*> widgets);

    // Snapshot each widget's current hidden state, then hide them all.
    // No-op if already hidden, so a second call cannot clobber the snapshot.
    void hide();

    // Restore each widget to the visibility captured by hide(). No-op if not
    // currently hidden.
    void restore();

    bool hidden() const { return hidden_; }

private:
    std::vector<QWidget*> widgets_;
    std::vector<bool> wasHidden_; // parallel to widgets_; valid while hidden_
    bool hidden_ = false;
};
```

- [ ] **Step 4: Write the implementation**

Create `src/ChromeHider.cpp`:

```cpp
#include "ChromeHider.h"
#include <QWidget>
#include <utility>

ChromeHider::ChromeHider(std::vector<QWidget*> widgets) : widgets_(std::move(widgets)) {}

void ChromeHider::hide() {
    if (hidden_)
        return;
    wasHidden_.clear();
    wasHidden_.reserve(widgets_.size());
    for (QWidget* w : widgets_) {
        wasHidden_.push_back(w ? w->isHidden() : true);
        if (w)
            w->hide();
    }
    hidden_ = true;
}

void ChromeHider::restore() {
    if (!hidden_)
        return;
    for (size_t i = 0; i < widgets_.size(); ++i) {
        if (QWidget* w = widgets_[i])
            w->setVisible(!wasHidden_[i]);
    }
    hidden_ = false;
}
```

- [ ] **Step 5: Add the source to the core library**

In `CMakeLists.txt`, add to the `CORE_SOURCES` list (after `src/MainWindowStatus.cpp` on line 171):

```cmake
    src/ChromeHider.cpp
```

- [ ] **Step 6: Build and run the tests to verify they pass**

Run:
```powershell
cmake --preset default
cmake --build build
ctest --test-dir build -R ChromeHider --output-on-failure
```
Expected: PASS — all six `[chrome]` cases pass.

- [ ] **Step 7: Commit**

```powershell
git add src/ChromeHider.h src/ChromeHider.cpp tests/test_ChromeHider.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m @'
Add ChromeHider: snapshot/restore of chrome visibility

The testable mechanism behind lights-out mode (ADR 0025). Hides a set of
widgets together and restores each to its exact prior visibility, never
touching CollapsiblePane's state machine.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

### Task 2: Wire fullscreen + lights-out into MainWindow

**Files:**
- Modify: `src/MainWindow.h` (include, members, method declarations)
- Modify: `src/MainWindow.cpp` (store toolbar pointers, build `ChromeHider`, View actions, `keyPressEvent`, `closeEvent`)

**Interfaces:**
- Consumes: `ChromeHider` (Task 1) — `hide()`, `restore()`, `hidden()`.
- Produces (private to MainWindow): `void toggleFullScreen()`, `void toggleChrome()`, `void restoreFocusModes()`, members `fullScreenAction`, `lightsOutAction`, `toolBar_`, `adjustmentsStrip_`, `wasMaximized_`, `std::optional<ChromeHider> chromeHider`.

> **Note:** This task is verified by building and running the app, not by a unit test — `MainWindow` constructs an RHI-backed `ImageViewport` that cannot be created in the offscreen test harness (the same reason no `test_MainWindow.cpp` exists). The snapshot/restore logic it relies on is already covered by Task 1's tests.

- [ ] **Step 1: Add the include and member declarations to the header**

In `src/MainWindow.h`, add the include near the top (after line 11, `#include <optional>` is already present; add the ChromeHider include after line 8's project includes):

```cpp
#include "ChromeHider.h"
```

In the private members section, after line 154 (`std::unique_ptr<CollapsiblePane> adjustmentsPane;`), add:

```cpp
    // Focus modes (docs/adr/0025): fullscreen and lights-out hide-chrome.
    QToolBar* toolBar_ = nullptr;          // main "Tools" toolbar
    QToolBar* adjustmentsStrip_ = nullptr; // ADR 0012 reveal strip
    QAction* fullScreenAction = nullptr;   // View -> Full Screen (F11)
    QAction* lightsOutAction = nullptr;    // View -> Hide Panels (F12)
    std::optional<ChromeHider> chromeHider; // built once both toolbars exist
    bool wasMaximized_ = false;             // restore target when leaving fullscreen
```

In the private methods section, after line 143 (`void toggleClipping();`), add:

```cpp
    // Focus modes (docs/adr/0025).
    void toggleFullScreen();   // F11: OS fullscreen <-> prior windowed state
    void toggleChrome();       // F12: hide all chrome <-> restore
    void restoreFocusModes();  // Escape / closeEvent: exit both modes
```

- [ ] **Step 2: Capture the main toolbar pointer**

In `src/MainWindow.cpp`, `setupToolbar()`, change line 581 from:

```cpp
    auto* tb = new QToolBar("Tools", this);
```
to:

```cpp
    auto* tb = new QToolBar("Tools", this);
    toolBar_ = tb;
```

- [ ] **Step 3: Capture the reveal-strip pointer and build the ChromeHider**

In `src/MainWindow.cpp`, `setupDocks()`, change line 835 from:

```cpp
    auto* strip = new QToolBar("Adjustments Strip", this);
```
to:

```cpp
    auto* strip = new QToolBar("Adjustments Strip", this);
    adjustmentsStrip_ = strip;
```

Then, in the constructor (`MainWindow::MainWindow`), after the four `setup*()` calls (after line 195, `setupToolbar();`), add — at this point `menuBar()`, `statusBar()`, both docks, and both toolbars all exist:

```cpp
    // ADR 0025: the full set of chrome that lights-out (F12) hides together.
    chromeHider.emplace(std::vector<QWidget*>{
        menuBar(), toolBar_, adjustmentsStrip_, statusBar(),
        filmStripDock, adjustmentsDock});
```

- [ ] **Step 4: Add the View-menu actions**

In `src/MainWindow.cpp`, `setupMenus()`, after line 457 (`view->addSeparator();`, the one following "Reset Zoom"), add:

```cpp
    fullScreenAction = view->addAction("&Full Screen", this, &MainWindow::toggleFullScreen);
    fullScreenAction->setCheckable(true);
    fullScreenAction->setShortcut(Qt::Key_F11);

    lightsOutAction = view->addAction("&Hide Panels", this, &MainWindow::toggleChrome);
    lightsOutAction->setCheckable(true);
    lightsOutAction->setShortcut(Qt::Key_F12);

    view->addSeparator();
```

- [ ] **Step 5: Implement the three focus-mode methods**

In `src/MainWindow.cpp`, add after `toggleClipping()`'s definition (find it near the clipping helpers; place these implementations adjacent). The full implementations:

```cpp
void MainWindow::toggleFullScreen() {
    if (isFullScreen()) {
        if (wasMaximized_)
            showMaximized();
        else
            showNormal();
    } else {
        wasMaximized_ = isMaximized();
        showFullScreen();
    }
    fullScreenAction->setChecked(isFullScreen());
}

void MainWindow::toggleChrome() {
    if (!chromeHider)
        return;
    if (chromeHider->hidden())
        chromeHider->restore();
    else
        chromeHider->hide();
    lightsOutAction->setChecked(chromeHider->hidden());
}

void MainWindow::restoreFocusModes() {
    if (chromeHider && chromeHider->hidden()) {
        chromeHider->restore();
        lightsOutAction->setChecked(false);
    }
    if (isFullScreen()) {
        if (wasMaximized_)
            showMaximized();
        else
            showNormal();
        fullScreenAction->setChecked(false);
    }
}
```

- [ ] **Step 6: Handle Escape in keyPressEvent**

In `src/MainWindow.cpp`, `keyPressEvent()` (lines 400-413), add an Escape branch at the top of the `if`-chain so it takes priority and is consumed only when a focus mode is active:

```cpp
void MainWindow::keyPressEvent(QKeyEvent* e) {
    // ADR 0025: Escape is a "give me my UI back" panic key — only consumed
    // while a focus mode is active, so other Escape uses are unaffected.
    if (e->key() == Qt::Key_Escape
        && ((chromeHider && chromeHider->hidden()) || isFullScreen())) {
        restoreFocusModes();
        return;
    }

    // Culling marks (0-5, X, r/y/g/b/p) are owned by the Image menu's actions —
    // window-level shortcuts that fire whether the strip or the image has focus.
    if (e->key() == Qt::Key_Left)
        filmStrip->navigateBy(-1);
    else if (e->key() == Qt::Key_Right)
        filmStrip->navigateBy(+1);
    else if (e->key() == Qt::Key_S && e->modifiers() == Qt::NoModifier)
        proofPanel->setProofingEnabled(!proofPanel->proofingEnabled());
    else if (e->key() == Qt::Key_J && e->modifiers() == Qt::NoModifier)
        toggleClipping();
    else
        QMainWindow::keyPressEvent(e);
}
```

- [ ] **Step 7: Guard closeEvent so transient modes never persist**

In `src/MainWindow.cpp`, `closeEvent()` (lines 384-398), add a restore call before the `saveState()` block so the saved layout reflects the real arrangement, not the focus mode. After the `confirmLeavingCurrentImage()` guard (after line 388) and before `QSettings s;` (line 390), insert:

```cpp
    // ADR 0025: never persist a transient focus mode — restore real chrome and
    // window state before saveGeometry()/saveState() snapshot them.
    restoreFocusModes();
```

- [ ] **Step 8: Build and verify it compiles cleanly**

Run:
```powershell
cmake --build build
```
Expected: PASS — no errors, no new warnings.

- [ ] **Step 9: Manually verify behaviour**

Run `build\arraw.exe` (open a folder/image so chrome is fully populated) and confirm:
- `F11` toggles borderless fullscreen; pressing it again returns to the prior windowed/maximized state. The View → Full Screen checkmark tracks it.
- `F12` hides menu bar, tool bar, status bar, both docks, and the reveal strip at once; pressing it again restores them. If the Adjustments dock was collapsed-to-strip beforehand, it returns collapsed (ADR 0012 preserved).
- With either or both active, `Escape` restores everything in one press.
- Collapse the Adjustments dock (F8), enable then disable lights-out — the collapsed state survives. Normal `Escape` with no mode active does nothing unexpected (e.g. crop still cancels).
- Quit while in lights-out/fullscreen, relaunch — the app starts with normal chrome and the previously saved layout (modes did not persist).

- [ ] **Step 10: Commit**

```powershell
git add src/MainWindow.h src/MainWindow.cpp
git commit -m @'
Add lights-out (F12) and fullscreen (F11) focus modes

Wire ChromeHider into MainWindow with two checkable View actions. Escape
restores both at once; closeEvent restores chrome before saveState so the
transient modes never persist. Implements ADR 0025.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Self-Review

**Spec coverage (against ADR 0025):**
- Fullscreen F11 + restore maximized/normal → Task 2 Steps 4-5 (`toggleFullScreen`, `wasMaximized_`). ✓
- Lights-out F12 hides all six chrome elements → Task 2 Step 3 (ChromeHider widget set) + Step 4-5. ✓
- Exact-visibility restore incl. pre-collapsed dock, no CollapsiblePane perturbation → Task 1 (raw visibility snapshot) + tests. ✓
- Composable modes, four reachable combinations → independent toggles, Task 2 Step 5. ✓
- Escape restores both → Task 2 Step 6. ✓
- Shortcuts live with menu bar hidden → `QAction` window-level shortcuts, Task 2 Step 4 (noted). ✓
- No persistence; closeEvent restores before saveState → Task 2 Step 7. ✓
- CONTEXT.md unchanged (UI-only) → no task needed. ✓

**Placeholder scan:** none — every code/command step is concrete.

**Type consistency:** `ChromeHider::hide/restore/hidden` names match between Task 1 (definition) and Task 2 (use). Member names (`toolBar_`, `adjustmentsStrip_`, `chromeHider`, `fullScreenAction`, `lightsOutAction`, `wasMaximized_`) and method names (`toggleFullScreen`, `toggleChrome`, `restoreFocusModes`) are consistent across header (Step 1) and definitions (Steps 5-7).
