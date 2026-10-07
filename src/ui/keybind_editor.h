#pragma once

#include "ui/keybinds.h"

namespace ui {

// draws the "edit > keybinds..." window if open. call between ImGuiLayer::newFrame() and render()
void drawKeybindEditor(KeybindRegistry& registry, bool& open);

}  // namespace ui
