# The develop dock has Adjustments and Masks tabs, and groups that collapse

The develop panel was one long column: the Masks group sat in the middle of it
and every group was always open. `old` had a tab widget (Adjustments | Masks |
Spots | Info) and M switched to its Masks tab. Spots and Info do not exist yet.

## Decision

**Two tabs, and the tab follows the mask mode.** The develop dock is a
`QTabWidget` (`developTabs`): Adjustments holds the `DevelopPanel`, Masks holds
its `MasksPanel`, which the panel still owns, wires and disables in the crop
mode but does not lay out (`DevelopPanel::masksPanel()`). The mask mode is the
single truth; the tab shows it:

- M, a mask tool, or choosing a mask enters the mode (`MainWindow::setMaskMode`)
  and shows Masks; Esc, M again, or entering the crop mode leaves it
  (`leaveMaskMode`) and shows Adjustments.
- Choosing the Masks tab enters the mode; choosing Adjustments leaves it,
  keeping the selection, as `leaveMaskMode` does.
- Every path ends in `showMaskMode()`, which sets the action and the tab with
  the tab widget's signals blocked, so there is no feedback loop. A failed
  entry (no editable photograph) puts the tab back.
- The dock is disabled as a whole when no photograph is editable, tabs
  included, as the mask action is.

Each tab is its own scroll area, with no focus (ADR 040); the tab bar and the
tab widget take no focus either, so a click on a tab leaves the keys with the
photograph. The dock's minimum width is the wider of the two pages plus a
scroll bar (`minimumDockWidth`), so neither scrolls sideways.

**Collapsible groups.** `CollapsibleSection` replaces the `QGroupBox` of every
group in the Adjustments tab: a flat full-width button for the title row (an
arrow, ▸ closed or ▾ open, then the title) over a body widget. The whole row is
clickable, and it takes the focus by Tab only (Space flips it), never by a
click, again for ADR 040. Whether a section is open is kept in `QSettings`
under `developSections/<id>/open`, where `<id>` is a stable identifier fixed
in code (`tone`, `toneCurve`, `whiteBalance`, `geometry`, ...), not the
translated title; the default is open. A section's minimum width does not
depend on being open, so a dock sized at start does not narrow when groups
were left closed.

**The Masks group is not collapsible.** It is the whole content of its tab,
so a title row would only hide the tab's content. It stays a `QGroupBox`.

## Consequences

- The crop mode disables a whole section, title row included, so a group
  cannot be opened or closed while cropping.
- Tests that looked for a `QGroupBox` by title look for a `CollapsibleSection`.
  Mask rows are no longer descendants of `DevelopPanel`; look in the window.
- Spots and Info tabs, when they exist, join the same tab widget; the
  tab-follows-mode rule is for Masks only.
