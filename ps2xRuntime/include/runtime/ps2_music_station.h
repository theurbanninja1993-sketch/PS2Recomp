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

// The runtime's own radio on/off switch (D-pad Down held for a couple of
// seconds). While off, the music folder is paused and the runtime shows its own
// "Radio Off" message - the game has no part in it.
void ps2MusicStationSetRadioOff(bool off);
bool ps2MusicStationRadioOff();
// GetTime() of the last on/off switch, for the overlay message.
double ps2MusicStationRadioOffSince();

std::string ps2MusicStationDirectory();
size_t ps2MusicStationTrackCount();
// Title of the current track and the time it started (GetTime()); for the
// "now playing" overlay.
std::string ps2MusicStationNowPlaying(double &startedAt);
// Whether the custom station is currently selected (in a vehicle, radio on),
// and the time (GetTime()) it became selected - for the station-logo overlay.
bool ps2MusicStationActive();
double ps2MusicStationActiveSince();
// Path of the custom station logo image (PNG next to the executable, or
// PS2X_CUSTOM_LOGO); empty when absent.
std::string ps2MusicStationLogoPath();
