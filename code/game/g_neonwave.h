// NeonArena wave-survival gametype — shared header
// Included by all g_neonwave*.c modules.
#ifndef G_NEONWAVE_H
#define G_NEONWAVE_H

#include "g_local.h"

#ifdef NEONARENA_MOD

// ---- constants ----
#define NW_FIRST_WAVE_DELAY     5000    // ms after map start
#define NW_WAVE_BREAK           12000   // ms between waves (perk shop)
#define NW_MAX_WAVE             20
#define NW_BOSS_WAVE            10      // from here on, each wave gets one boss drone
#define NW_BOSS_COUNT           11      // SNIPER TANK SWARM GLASS WARDEN BERSERKER TELEPORTER HEALER SHIELDER SNIPELITE DEMOLISHER

// CS_NEONWAVE payload events
#define NW_EV_RUNNING           0
#define NW_EV_CLEARED           1
#define NW_EV_FAILED            2
#define NW_EV_VICTORY           3

// wave modifiers
#define NW_MOD_NONE             0
#define NW_MOD_GLASS            1       // all drones die to one hit, but +2 skill aggression
#define NW_MOD_SWARM            2       // double drone count, skill capped lower
#define NW_MOD_LOWGRAV          3       // g_gravity halved for the wave
#define NW_MOD_DOUBLEPTS        4       // wave clear grants x2 upgrade points
#define NW_MOD_TIMEWARP         5       // player speed scaled (g_speed) for the wave
#define NW_MOD_VAMPIRE          6       // each kill heals the player a few HP (lifesteal)
#define NW_MOD_FRENZY           7       // g_quadfactor boosted -> shots hit much harder
#define NW_MOD_OVERSHIELD       8       // player granted bonus armor at wave start
#define NW_MOD_MIRROR           9       // bots' damage is partially reflected back on hit
#define NW_MOD_REGEN            10      // player regenerates HP at the start of each wave
#define NW_MOD_SURGE            11      // tougher drones but wave clear grants x3 upgrade points
#define NW_MOD_FROST            12      // slowed player (g_speed), frosty drones
#define NW_MOD_CHAOS            13      // chaotic spawns: random skill + spawn delay
#define NW_MOD_MIMIC            14      // drones copy a random upgrade value from a random human
#define NW_MOD_SHIELD           15      // temporary invulnerability at wave start
#define NW_MOD_POOL_SIZE        16

// achievements
#define NW_ACH_FIRST_VICTORY    0       // cleared wave 20 (full run)
#define NW_ACH_SURVIVOR         1       // reached wave 15
#define NW_ACH_SHARPSHOOTER     2       // best combo >= 8
#define NW_ACH_STREAKER         3       // best combo >= 5
#define NW_ACH_FLAWLESS         4       // victory with 0 deaths
#define NW_ACH_COMBOMASTER      5       // best combo >= 12
#define NW_ACH_SPEEDRUNNER      6       // victory under time target (300s)
#define NW_ACH_HARDCORE         7       // victory in hardcore mode
#define NW_ACH_COUNT            8

// boss types
#define NW_BOSS_SNIPER          1
#define NW_BOSS_TANK            2
#define NW_BOSS_SWARM           3
#define NW_BOSS_GLASS           4
#define NW_BOSS_WARDEN          5
#define NW_BOSS_BERSERKER       6
#define NW_BOSS_TELEPORTER      7
#define NW_BOSS_HEALER          8

// perk IDs
#define NW_PERK_PIERCE          1
#define NW_PERK_OVERCHARGE      4

// ---- shared state (defined in g_neonwave.c) ----
extern int nw_wave;
extern int nw_aliveBots;
extern int nw_modifier;
extern int nw_modifier2;
extern int nw_bossType;
extern int nw_bossPhase;
extern int nw_runBestCombo;
extern int nw_difficulty;
extern int nw_modifiersSeen;
extern qboolean nw_achievements[ NW_ACH_COUNT ];
extern qboolean nw_hardcore;
extern qboolean nw_dailyActive;
extern int nw_synergyIdx;
extern int nw_ptsMul;
extern int nw_over;
extern int nw_event;
extern qboolean nw_overVictory;
extern int nw_perk[ MAX_CLIENTS ][ NW_PERK_COUNT ];
extern int nw_offer[ MAX_CLIENTS ][ 3 ];

// ---- shared function prototypes ----
// wave.c
void NeonWave_Reset( void );
void NeonWave_Frame( void );
void NeonWave_OnPlayerDeath( struct gclient_s *client );
qboolean NeonWave_IsBreak( void );
void NeonWave_ForceStarted( void );

// boss.c
void NW_SpawnBoss( void );
void NW_BossFrame( void );
const char *NW_BossName( int type );

// perk.c
void NW_RollOffers( int clientID );
void NW_PickPerk( int clientID, int slot );
int NW_PerkLevel( int clientID, int perk );
const char *NW_PerkName( int id );
int NW_PerkCap( int id );

// coop.c
int NW_CountHumans( void );
int NW_CoopMockExtra( qboolean alive );
qboolean NW_CoopWaveClear( int drones );
void NW_CoopRespawnDead( void );
void NW_CoopScale( int *botCount, int *skill );

// record.c
void NW_LoadRecords( void );
void NW_SaveRecords( void );
void NW_LoadDailyRecords( void );
void NW_SaveDailyRecords( void );
void NW_CheckNewRecord( void );
void NW_WriteRunStatsJSON( void );

// modifier.c
const char *NW_ModifierName( int mod );
void NW_ApplyModifiers( void );
void NW_RestoreModifiers( void );
qboolean NW_ModActive( int mod );

// synergy.c
void NW_ApplySynergy( void );
const char *NW_SynergyName( int idx );

#endif // NEONARENA_MOD
#endif // G_NEONWAVE_H
