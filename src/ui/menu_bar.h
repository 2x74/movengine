#pragma once

#include <string>
#include <vector>

namespace ui {

struct MenuBarState {
    std::vector<std::string> styleNames;
    int selectedStyle = 0;

    bool showKeybindsWindow = false;
    bool showSettingsWindow = false;
    bool showZonesWindow = false;
    bool showSoundsWindow = false;
    bool showAboutWindow = false;
    bool showJhud = true;
    // mirrors of r_showclips / r_showtriggers: the caller copies the cvars in before drawing and copies any change back out after
    bool showClips = false;
    bool showTriggers = false;
    // view > replay best run. set by the caller to whether one exists, the menu only offers it when there's something to play
    // edit > null binds. mirrors of null_ad / w_release_on_jump, copied in before the menu draws and read back after, like the show* pair
    bool nullAD = true;
    bool releaseWOnJump = true;
    bool recorrectTeleportMomentum = true;
    bool hasReplay = false;
    bool replayClicked = false;

    // set by drawMenuBar() when clicked this frame, the caller is responsible for consuming and clearing these afterward
    bool openMapClicked = false;
    bool execConfigClicked = false;
    bool openEditorClicked = false;
    bool mountCssClicked = false;
    bool loadPackClicked = false;
    bool exportPackClicked = false;
    bool exitClicked = false;

    // the "times" menu: your times on the track you're on (filled by the caller each frame). `current` rows match your style and settings now
    struct TimeRow {
        std::string label;
        std::string details;  // tooltip: the settings it was set with
        bool current = false;
        bool canApply = false;
    };
    std::string timesHeader;
    std::vector<TimeRow> times;
    int applyTimeClicked = -1;  // "use these settings" on that row, this frame
};

// all lowercase per project convention. call between ImGuiLayer::newFrame() and render()
void drawMenuBar(MenuBarState& state);

}  // namespace ui
