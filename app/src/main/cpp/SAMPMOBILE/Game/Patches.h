#pragma once

namespace sa {
struct CPlayerInfo;
}

extern sa::CPlayerInfo* WORLD_PLAYERS;

class Patches {
public:
	static void preApply();
	static void apply();
	static bool hasFullAudioData();

	static void allocPlayers();
	static void initBetterPoolsSize();
};
