#include "movement/zones.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace movement {

namespace {

// "" for the main track (keeps old zone files loading), "bonus3_" etc.
std::string trackPrefix(int track) {
    return track == 0 ? "" : "bonus" + std::to_string(track) + "_";
}

}  // namespace

std::string trackName(int track) {
    return track == 0 ? "main" : "bonus " + std::to_string(track);
}

void loadZones(const std::string& path, MapZones& zones) {
    zones = MapZones{};
    std::ifstream file(path);
    if (!file) {
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string key;
        if (!(iss >> key)) continue;
        int track = 0;
        if (key.rfind("bonus", 0) == 0) {
            size_t underscore = key.find('_');
            if (underscore == std::string::npos) continue;
            track = std::atoi(key.substr(5, underscore - 5).c_str());
            if (track < 1 || track >= kMaxTracks) continue;
            key = key.substr(underscore + 1);
        }
        TrackZones& t = zones.tracks[track];
        if (key == "setstart") {
            StartPoint sp;
            if (iss >> sp.feetPos.x >> sp.feetPos.y >> sp.feetPos.z >> sp.yawDeg >> sp.pitchDeg) {
                sp.defined = true;
                t.startPoint = sp;
            }
            continue;
        }
        Zone zone;
        std::string rest;
        std::getline(iss, rest);
        std::istringstream values(rest);
        std::string first;
        if (std::istringstream(rest) >> first && first == "none") {
            zone.removed = true;
        } else if (!(values >> zone.min.x >> zone.min.y >> zone.min.z >> zone.max.x >> zone.max.y >> zone.max.z >>
                       zone.anchorPoint.x >> zone.anchorPoint.y >> zone.anchorPoint.z)) {
            continue;
        } else {
            zone.defined = true;
        }
        if (key == "start") {
            t.start = zone;
        } else if (key == "end") {
            t.end = zone;
        }
    }
}

bool saveZones(const std::string& path, const MapZones& zones) {
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "failed to write zones to %s\n", path.c_str());
        return false;
    }
    for (int track = 0; track < kMaxTracks; ++track) {
        const TrackZones& t = zones.tracks[track];
        std::string prefix = trackPrefix(track);
        auto write = [&](const char* which, const Zone& z) {
            if (!z.defined) {
                if (z.removed) file << prefix << which << " none\n";
                return;
            }
            file << prefix << which << ' ' << z.min.x << ' ' << z.min.y << ' ' << z.min.z << ' ' << z.max.x << ' '
                 << z.max.y << ' ' << z.max.z << ' ' << z.anchorPoint.x << ' ' << z.anchorPoint.y << ' '
                 << z.anchorPoint.z << '\n';
        };
        write("start", t.start);
        write("end", t.end);
        if (t.startPoint.defined) {
            file << prefix << "setstart " << t.startPoint.feetPos.x << ' ' << t.startPoint.feetPos.y << ' '
                 << t.startPoint.feetPos.z << ' ' << t.startPoint.yawDeg << ' ' << t.startPoint.pitchDeg << '\n';
        }
    }
    return true;
}

}  // namespace movement
