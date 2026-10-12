// g_nw_internal.h - shared internals between the NeonWave modules
// g_neonwave.c was one 3700-line file; it is being split into focused
// modules (records, achievements, modifiers, bosses, ...). This header
// carries the shared state and helpers across those modules — the C
// equivalent of the friend declarations the SDL2 prototype uses.
#ifndef G_NW_INTERNAL_H
#define G_NW_INTERNAL_H

#include "g_local.h"

// ---- shared run state (defined in g_neonwave.c) ----
extern int nw_wave;			// current wave (1-based)
extern int nw_runStartTime;		// run stats: level.time of first wave start
extern qboolean nw_dailyActive;		// daily challenge active
extern qboolean nw_overVictory;		// last run ended in victory
extern qboolean nw_hardcore;		// hardcore mode (tougher run)
extern int nw_runKills;			// autokill + headless kill counter
extern int nw_multikillCount;		// kills within the current second
extern qboolean nw_untouchableWave;	// current wave took no damage

// game-over events (NW_SendStatus / achievements read the event id)
#define NW_EV_DEATH		0
#define NW_EV_DEFEAT		1
#define NW_EV_QUIT		2
#define NW_EV_VICTORY		3

// ---- run-stat cache (defined in g_neonwave.c) ----
typedef struct nwCache_t {
    int     kills;
    int     bestCombo;
    int     currentCombo;
    int     bossHp;
    int     bossMax;
    int     points;         // first human's upgrade points
    qboolean valid;
} nwCache_t;

const nwCache_t *NW_Cache( void );

// ---- records module (g_nw_records.c) ----
#define NW_RECORDS_FILE		"neonwave_records.dat"
#define NW_DAILY_RECORDS_FILE	"neonwave_daily_records.dat"

typedef struct {
	int	bestWave;
	int	bestTime;	// fastest victory, seconds (0 = none)
	int	bestKills;
	int	bestCombo;
} nwRecords_t;

typedef struct {
	int	dayStamp;	// yyyymmdd the records belong to
	int	bestWave;
	int	bestTime;	// fastest victory, seconds (0 = none)
	int	bestKills;
	int	bestCombo;
	int	dayResult;	// 0=no run yet today, 1=no victory today, 2=victory today
	int	streak;		// consecutive daily victories (persists across days)
} nwDailyRecords_t;

void NW_LoadRecords( void );
void NW_SaveRecords( void );
void NW_LoadDailyRecords( void );
void NW_SaveDailyRecords( void );
void NW_UpdateRecords( void );
void NW_MirrorRecordCvars( void );
void NW_MirrorDailyRecordCvars( void );

// ---- achievements module (g_nw_achievements.c) ----
#define NW_ACHIEVEMENTS_FILE	"neonwave_achievements.dat"

// achievement ids (per-run badges, mirrored into run-stats JSON)
#define NW_ACH_FIRST_VICTORY	0	// cleared wave 20 (full run)
#define NW_ACH_SURVIVOR		1	// reached wave 15
#define NW_ACH_SHARPSHOOTER	2	// best combo >= 8
#define NW_ACH_STREAKER		3	// best combo >= 5
#define NW_ACH_FLAWLESS		4	// victory with 0 deaths
#define NW_ACH_COMBOMASTER	5	// best combo >= 12
#define NW_ACH_SPEEDRUNNER	6	// victory under time target (300s)
#define NW_ACH_HARDCORE		7	// victory in hardcore mode
// v0.60: 17 new achievements (total 25)
#define NW_ACH_KILL_100		8	// kill 100 bots total
#define NW_ACH_KILL_1000	9	// kill 1000 bots total
#define NW_ACH_WAVE_5		10	// survive to wave 5
#define NW_ACH_WAVE_10		11	// survive to wave 10
#define NW_ACH_WAVE_30		12	// survive to wave 30
#define NW_ACH_WAVE_50		13	// survive to wave 50
#define NW_ACH_PERFECT_WAVE	14	// clear wave without damage
#define NW_ACH_MULTIKILL_3	15	// 3 kills in 1 second
#define NW_ACH_MULTIKILL_5	16	// 5 kills in 1 second
#define NW_ACH_RAILGUN_MASTER	17	// 100 railgun kills
#define NW_ACH_LIGHTNING_MASTER	18	// 100 lightning kills
#define NW_ACH_PLASMA_MASTER	19	// 100 plasma kills
#define NW_ACH_BOSS_RUSH	20	// kill 5 bosses in a row
#define NW_ACH_ECHO_CHAMPION	21	// trigger echo-chaos 10 times (secret)
#define NW_ACH_OVERCLOCKED	22	// use 10 overclocks in one run (secret)
#define NW_ACH_FUSION_DISCOVERER	23	// trigger all fusion types (secret)
#define NW_ACH_ALL_UPGRADES	24	// max all upgrade types
#define NW_ACH_COUNT		25

extern qboolean nw_achievements[ NW_ACH_COUNT ];	// unlocked this run

const char *NW_AchievementName( int id );
void NW_LoadAchievements( void );
void NW_SaveAchievements( void );
void NW_CheckAchievements( int event );

// shared helpers from g_neonwave.c used by the achievements module
int NW_RunDeaths( void );
qboolean NW_Headless( void );
void NW_InvalidateCache( void );

#endif // G_NW_INTERNAL_H
