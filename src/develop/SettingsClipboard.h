#pragma once

#include "develop/DevelopGroup.h"

// The in-memory, session-only Copy Settings clipboard (Milestone 8; CONTEXT.md
// "Copy Settings"). One slot, held by MainWindow as a std::optional and never
// written to the OS clipboard: the payload is an internal GlobalAdjustment, and
// the deliberate durable/portable transfer channel is a Develop Preset instead.
//
// Paste constructs a GroupChecklistDialog with `available = groups`, so the
// pasted selection is bounded by what was copied, then applies
// applyGroups(target, snapshot, dialog.selectedGroups()).
struct SettingsClipboard {
    GlobalAdjustment snapshot;
    GroupSelection groups;
};
