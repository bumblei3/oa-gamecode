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

#endif // G_NW_INTERNAL_H
