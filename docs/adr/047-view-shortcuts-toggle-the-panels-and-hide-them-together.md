# View shortcuts toggle the panels and hide them together

`old` (its ADR 0028) had keys to hide each panel, to go full screen and to
hide everything ("lights-out"). The rewrite had only F9 for the film strip and
Ctrl+Shift+D for the debug log.

## Decision

**Keys.** F7 toggles the History dock, F8 the Develop dock, F9 the film strip,
F11 full screen and F12 Hide Panels. All are checkable items of the View menu,
in the order History, Develop, Film Strip, then Full Screen and Hide Panels,
and every one is also added to the window with `addAction`. Qt does not fire
the shortcut of an action whose only widget is a hidden menu, so without that
the keys would die when Hide Panels hides the menu bar. The same holds for every
other shortcut in the menus, so after the menus are built each shortcut-bearing menu
action (submenus included) is added to the window too. Esc is not used: it
already belongs to the white balance picker, the crop mode and the masks.

**The Develop dock is closable.** `QDockWidget` offers a toggle action only to
a closable dock, so the dock is now `DockWidgetClosable` and nothing else; it
is still neither movable nor floatable.

**Full screen** remembers on entering whether the window was maximised, taken
from the old state of the `WindowStateChange` event, so full screen entered
from outside the program (the window manager) is caught as well. Leaving goes back to maximised or normal. The action's check follows
the window's real state, so a change by the window manager keeps it right.

**Hide Panels** uses `ChromeHider`, which on hiding notes whether each widget
was hidden (the three docks, the menu bar, the status bar) and on restoring
makes each exactly as visible as it was; a dock the user had closed stays
closed. While the panels are hidden the F7, F8 and F9 actions are disabled:
they would otherwise show a panel that the restore then hides again, or make
the snapshot lie. Only F12 leaves the mode.

Tests: `tests/test_MainWindowView.cpp`.
