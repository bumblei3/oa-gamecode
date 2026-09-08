// NeonArena wave-survival gametype — shared header
// Included by all g_neonwave*.c modules.
#ifndef G_NEONWAVE_H
#define G_NEONWAVE_H

#include "g_local.h"

#ifdef NEONARENA_MOD

// ---- constants ----
#ifndef NW_FIRST_WAVE_DELAY
#define NW_FIRST_WAVE_DELAY     5000    // ms after map start
#endif
#ifndef NW_WAVE_BREAK
#define NW_WAVE_BREAK           12000   // ms between waves (perk shop)
#endif
#ifndef NW_MAX_WAVE
#define NW_MAX_WAVE             20
#endif
#ifndef NW_BOSS_WAVE
#define NW_BOSS_WAVE            10      // from here on, each wave gets one boss drone
#endif
#ifndef NW_BOSS_COUNT
#define NW_BOSS_COUNT           13      // SNIPER TANK SWARM GLASS WARDEN BERSERKER TELEPORTER HEALER SHIELDER SNIPELITE DEMOLISHER CHRONOMANCER VOIDWALKER
#endif

// ---- constants ----
#ifndef NW_FIRST_WAVE_DELAY
#define NW_FIRST_WAVE_DELAY     5000    // ms after map start
#endif
#ifndef NW_WAVE_BREAK
#define NW_WAVE_BREAK           12000   // ms between waves (perk shop)
#endif
#ifndef NW_MAX_WAVE
#define NW_MAX_WAVE             20
#endif
#ifndef NW_BOSS_WAVE
#define NW_BOSS_WAVE            10      // from here on, each wave gets one boss drone
#endif
#ifndef NW_BOSS_COUNT
#define NW_BOSS_COUNT           13      // SNIPER TANK SWARM GLASS WARDEN BERSERKER TELEPORTER HEALER SHIELDER SNIPELITE DEMOLISHER CHRONOMANCER VOIDWALKER
#endif

// CS_NEONWAVE payload events
#ifndef NW_EV_RUNNING
#define NW_EV_RUNNING           0
#endif
#ifndef NW_EV_CLEARED
#define NW_EV_CLEARED           1
#endif
#ifndef NW_EV_FAILED
#define NW_EV_FAILED            2
#endif
#ifndef NW_EV_VICTORY
#define NW_EV_VICTORY           3
#endif

// wave modifiers
#ifndef NW_MOD_NONE
#define NW_MOD_NONE             0
#endif
#ifndef NW_MOD_GLASS
#define NW_MOD_GLASS            1       // all drones die to one hit, but +2 skill aggression
#endif
#ifndef NW_MOD_SWARM
#define NW_MOD_SWARM            2       // double drone count, skill capped lower
#endif
#ifndef NW_MOD_LOWGRAV
#define NW_MOD_LOWGRAV          3       // g_gravity halved for the wave
#endif
#ifndef NW_MOD_DOUBLEPTS
#define NW_MOD_DOUBLEPTS        4       // wave clear grants x2 upgrade points
#endif
#ifndef NW_MOD_TIMEWARP
#define NW_MOD_TIMEWARP         5       // player speed scaled (g_speed) for the wave
#endif
#ifndef NW_MOD_VAMPIRE
#define NW_MOD_VAMPIRE          6       // each kill heals the player a few HP (lifesteal)
#endif
#ifndef NW_MOD_FRENZY
#define NW_MOD_FRENZY           7       // g_quadfactor boosted -> shots hit much harder
#endif
#ifndef NW_MOD_OVERSHIELD
#define NW_MOD_OVERSHIELD       8       // player granted bonus armor at wave start
#endif
#ifndef NW_MOD_MIRROR
#define NW_MOD_MIRROR           9       // bots' damage is partially reflected back on hit
#endif
#ifndef NW_MOD_REGEN
#define NW_MOD_REGEN            10      // player regenerates HP at the start of each wave
#endif
#ifndef NW_MOD_SURGE
#define NW_MOD_SURGE            11      // tougher drones but wave clear grants x3 upgrade points
#endif
#ifndef NW_MOD_FROST
#define NW_MOD_FROST            12      // slowed player (g_speed), frosty drones
#endif
#ifndef NW_MOD_CHAOS
#define NW_MOD_CHAOS            13      // chaotic spawns: random skill + spawn delay
#endif
#ifndef NW_MOD_MIMIC
#define NW_MOD_MIMIC            14      // drones copy a random upgrade value from a random human
#endif
#ifndef NW_MOD_SHIELD
#define NW_MOD_SHIELD           15      // temporary invulnerability at wave start
#endif
#ifndef NW_MOD_POOL_SIZE
#define NW_MOD_POOL_SIZE        16
#endif

// achievements
#ifndef NW_ACH_FIRST_VICTORY
#define NW_ACH_FIRST_VICTORY    0       // cleared wave 20 (full run)
#endif
#ifndef NW_ACH_SURVIVOR
#define NW_ACH_SURVIVOR         1       // reached wave 15
#endif
#ifndef NW_ACH_SHARPSHOOTER
#define NW_ACH_SHARPSHOOTER     2       // best combo >= 8
#endif
#ifndef NW_ACH_STREAKER
#define NW_ACH_STREAKER         3       // best combo >= 5
#endif
#ifndef NW_ACH_FLAWLESS
#define NW_ACH_FLAWLESS         4       // victory with 0 deaths
#endif
#ifndef NW_ACH_COMBOMASTER
#define NW_ACH_COMBOMASTER      5       // best combo >= 12
#endif
#ifndef NW_ACH_SPEEDRUNNER
#define NW_ACH_SPEEDRUNNER      6       // victory under time target (300s)
#endif
#ifndef NW_ACH_HARDCORE
#define NW_ACH_HARDCORE         7       // victory in hardcore mode
#endif
#ifndef NW_ACH_COUNT
#define NW_ACH_COUNT            25
#endif

// boss types
#ifndef NW_BOSS_SNIPER
#define NW_BOSS_SNIPER          1
#endif
#ifndef NW_BOSS_TANK
#define NW_BOSS_TANK            2
#endif
#ifndef NW_BOSS_SWARM
#define NW_BOSS_SWARM           3
#endif
#ifndef NW_BOSS_GLASS
#define NW_BOSS_GLASS           4
#endif
#ifndef NW_BOSS_WARDEN
#define NW_BOSS_WARDEN          5
#endif
#ifndef NW_BOSS_BERSERKER
#define NW_BOSS_BERSERKER       6
#endif
#ifndef NW_BOSS_TELEPORTER
#define NW_BOSS_TELEPORTER      7
#endif
#ifndef NW_BOSS_HEALER
#define NW_BOSS_HEALER          8
#endif

// perk IDs
#ifndef NW_PERK_PIERCE
#define NW_PERK_PIERCE          1
#endif
#ifndef NW_PERK_OVERCHARGE
#define NW_PERK_OVERCHARGE      4
#endif

// ---- shared state (defined in g_neonwave.c) ----
// Use getter functions instead of extern variables to avoid static/non-static conflicts.
int NW_GetWave( void );
int NW_GetModifiersSeen( void );
int NW_GetRunStartTime( void );
qboolean NW_GetOverVictory( void );

// ---- shared function prototypes ----
// wave.c
void NeonWave_Reset( void );
void NeonWave_Frame( void );
void NeonWave_OnPlayerDeath( struct gclient_s *client );
qboolean NeonWave_IsBreak( void );
void NeonWave_ForceStarted( void );
int NeonWave_GetWave( void );

// boss.c
int NW_BossPhase( void );

// seasonal.c
void NW_SeasonalInit( void );
void NW_SeasonalProgress( int wave );
void NW_SeasonalCheck( int event );
int NW_SeasonalLeaderboardCount( void );
const char *NW_SeasonalLeaderboardName( int idx );
int NW_SeasonalLeaderboardScore( int idx );
int NW_SeasonalLeaderboardVictory( int idx );
const char *NW_SeasonalChallengeTitle( void );
const char *NW_SeasonalChallengeDesc( void );
int NW_SeasonalChallengeTarget( void );
int NW_SeasonalProgressLevel( void );
qboolean NW_SeasonalCompleted( void );
const char *NW_SeasonalReward( void );

#endif // NEONARENA_MOD
#endif // G_NEONWAVE_H
