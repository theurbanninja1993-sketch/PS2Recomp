#pragma once

// Custom radio station: the player's own music from a "music" folder next
// to the executable (MP3 / OGG / FLAC / WAV, subfolders included), like
// the GTA III / Vice City PC "MP3 Player". The game override decides when it
// is heard (VCS: the unused 10th station slot while in a vehicle).

#include <cstdint>
#include <string>

// The station name shown in the "now playing" overlay (and the settings).
const char *ps2MusicStationName();

using Ps2MusicStationQuery = bool (*)(const uint8_t *rdram);
void ps2MusicStationSetQuery(Ps2MusicStationQuery query);
bool ps2MusicStationSupported(); // a game registered a query

// Window thread, once per frame (after audio is initialised).
void ps2MusicStationUpdate(const uint8_t *rdram);
void ps2MusicStationNextTrack();
void ps2MusicStationRescan();

std::string ps2MusicStationDirectory();
size_t ps2MusicStationTrackCount();
// Title of the current track and the time it started (GetTime()); for the
// "now playing" overlay.
std::string ps2MusicStationNowPlaying(double &startedAt);
