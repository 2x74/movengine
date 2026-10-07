#pragma once

#include "movement/zones.h"

namespace ui {

// what the zones window asked for this frame, edits and deletes are already applied to the zones themselves
struct ZonesWindowResult {
    int placeTrack = -1;     // start the corner tool on this track...
    bool placeStart = true;  // ...for its start zone (else its end zone)
    int goTrack = -1;        // switch to this track and restart onto it
    bool changed = false;    // zones were edited or deleted: save them
};

// "edit > zones...": every track (main + bonuses in use) with its start and end zone (place, delete, or adjust the bounds numerically) plus a button to add the next bonus. creatingTrack/creatingStart: the zone the corner tool is placing right now (-1 = none)
ZonesWindowResult drawZonesWindow(movement::MapZones& zones, int currentTrack, int creatingTrack,
                                  bool creatingStart, bool& open);

}  // namespace ui
