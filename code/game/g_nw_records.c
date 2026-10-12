// g_nw_records.c - NeonWave persistent records (global + daily)
// Split out of g_neonwave.c (3700 lines). Records state, load/save and
// the record-update logic; shared run state comes via g_nw_internal.h.
#include "g_local.h"
#include "g_nw_internal.h"

#ifdef NEONARENA_MOD

static nwRecords_t nw_records;
static nwDailyRecords_t nw_dailyRecords;

static int NW_DailyStamp( void ) {
	qtime_t tm;
	trap_RealTime( &tm );
	return ( tm.tm_year + 1900 ) * 10000 + ( tm.tm_mon + 1 ) * 100 + tm.tm_mday;
}

void NW_LoadRecords( void ) {
	fileHandle_t f;
	memset( &nw_records, 0, sizeof( nw_records ) );
	if ( trap_FS_FOpenFile( NW_RECORDS_FILE, &f, FS_READ ) >= 0 && f ) {
		trap_FS_Read( &nw_records, sizeof( nw_records ), f );
		trap_FS_FCloseFile( f );
	}
	G_Printf( "NeonWave: records loaded wave=%i time=%is kills=%i combo=%i\n",
		nw_records.bestWave, nw_records.bestTime,
		nw_records.bestKills, nw_records.bestCombo );
	NW_MirrorRecordCvars();
}

void NW_SaveRecords( void ) {
	fileHandle_t f;
	int len = trap_FS_FOpenFile( NW_RECORDS_FILE, &f, FS_WRITE );
	if ( len < 0 || !f ) {
		G_Printf( "NeonWave: WARNING cannot write " NW_RECORDS_FILE "\n" );
		return;
	}
	trap_FS_Write( &nw_records, sizeof( nw_records ), f );
	trap_FS_FCloseFile( f );
	G_Printf( "NeonWave: RECORDS SAVED wave=%i time=%is kills=%i combo=%i\n",
		nw_records.bestWave, nw_records.bestTime,
		nw_records.bestKills, nw_records.bestCombo );
}

void NW_LoadDailyRecords( void ) {
	fileHandle_t f;
	nwDailyRecords_t saved;
	memset( &nw_dailyRecords, 0, sizeof( nw_dailyRecords ) );
	nw_dailyRecords.dayStamp = NW_DailyStamp();
	if ( trap_FS_FOpenFile( NW_DAILY_RECORDS_FILE, &f, FS_READ ) >= 0 && f ) {
		trap_FS_Read( &saved, sizeof( saved ), f );
		if ( saved.dayStamp == nw_dailyRecords.dayStamp ) {
			// same day: keep today's records (including today's streak/dayResult)
			nw_dailyRecords = saved;
		} else {
			// different day: carry streak forward if yesterday was a victory
			nw_dailyRecords.streak = ( saved.dayResult == 2 ) ? saved.streak : 0;
			// dayResult stays 0 (today hasn't been played yet)
		}
		trap_FS_FCloseFile( f );
	}
	G_Printf( "NeonWave: DAILY records loaded wave=%i time=%is kills=%i combo=%i\n",
		nw_dailyRecords.bestWave, nw_dailyRecords.bestTime,
		nw_dailyRecords.bestKills, nw_dailyRecords.bestCombo );
	NW_MirrorDailyRecordCvars();
}

// mirror daily record values to cvars (cgame reads them for the daily HUD)
void NW_MirrorDailyRecordCvars( void ) {
	trap_Cvar_Set( "g_neonwave_dailyrecwave", va( "%i", nw_dailyRecords.bestWave ) );
	trap_Cvar_Set( "g_neonwave_dailyrectime", va( "%i", nw_dailyRecords.bestTime ) );
	trap_Cvar_Set( "g_neonwave_dailyreckills", va( "%i", nw_dailyRecords.bestKills ) );
	trap_Cvar_Set( "g_neonwave_dailyreccombo", va( "%i", nw_dailyRecords.bestCombo ) );
	trap_Cvar_Set( "g_neonwave_dailystreak", va( "%i", nw_dailyRecords.streak ) );
}

void NW_SaveDailyRecords( void ) {
	fileHandle_t f;
	int len = trap_FS_FOpenFile( NW_DAILY_RECORDS_FILE, &f, FS_WRITE );
	if ( len < 0 || !f ) {
		G_Printf( "NeonWave: WARNING cannot write " NW_DAILY_RECORDS_FILE "\n" );
		return;
	}
	nw_dailyRecords.dayStamp = NW_DailyStamp();
	trap_FS_Write( &nw_dailyRecords, sizeof( nw_dailyRecords ), f );
	trap_FS_FCloseFile( f );
	G_Printf( "NeonWave: DAILY RECORDS SAVED wave=%i time=%is kills=%i combo=%i\n",
		nw_dailyRecords.bestWave, nw_dailyRecords.bestTime,
		nw_dailyRecords.bestKills, nw_dailyRecords.bestCombo );
}

static void NW_UpdateDailyRecords( int kills, int combo, int runSec ) {
	qboolean changed = qfalse;
	qboolean firstRunToday = qfalse;

	// day rolled over mid-session: reload (carries streak forward if yesterday was a victory)
	if ( nw_dailyRecords.dayStamp != NW_DailyStamp() ) {
		NW_LoadDailyRecords();
	}
	// first run of the day: determine streak outcome
	if ( nw_dailyRecords.dayResult == 0 ) {
		firstRunToday = qtrue;
		if ( nw_overVictory ) {
			nw_dailyRecords.dayResult = 2;
			nw_dailyRecords.streak++;
		} else {
			nw_dailyRecords.dayResult = 1;
			nw_dailyRecords.streak = 0;
		}
	}

	trap_Cvar_Set( "g_neonwave_newrecord", "0" );
	if ( nw_wave > nw_dailyRecords.bestWave ) {
		nw_dailyRecords.bestWave = nw_wave;
		changed = qtrue;
		G_Printf( "NeonWave: NEW DAILY RECORD WAVE %i\n", nw_wave );
	}
	if ( nw_overVictory && ( nw_dailyRecords.bestTime <= 0 || runSec < nw_dailyRecords.bestTime ) ) {
		nw_dailyRecords.bestTime = runSec;
		changed = qtrue;
		G_Printf( "NeonWave: NEW DAILY RECORD TIME %is\n", runSec );
	}
	if ( kills > nw_dailyRecords.bestKills ) {
		nw_dailyRecords.bestKills = kills;
		changed = qtrue;
		G_Printf( "NeonWave: NEW DAILY RECORD KILLS %i\n", kills );
	}
	if ( combo > nw_dailyRecords.bestCombo ) {
		nw_dailyRecords.bestCombo = combo;
		changed = qtrue;
		G_Printf( "NeonWave: NEW DAILY RECORD COMBO %ix\n", combo );
	}
	// always persist current daily state (records + streak + dayResult)
	NW_SaveDailyRecords();
	trap_Cvar_Set( "g_neonwave_newrecord", changed ? "1" : "0" );
	NW_MirrorDailyRecordCvars();
}

void NW_UpdateRecords( void ) {
	// Use the cached single-pass scan (also used by NW_SendStatus)
	const nwCache_t *cache = NW_Cache();
	int kills = cache->kills;
	int combo = cache->bestCombo;
	int runSec = ( level.time - nw_runStartTime ) / 1000;
	qboolean changed = qfalse;

	// daily challenge: update the per-day record set instead of the global one
	if ( nw_dailyActive ) {
		NW_UpdateDailyRecords( kills, combo, runSec );
	}

	trap_Cvar_Set( "g_neonwave_newrecord", "0" ); // reset per run end
	if ( nw_wave > nw_records.bestWave ) {
		nw_records.bestWave = nw_wave;
		changed = qtrue;
		G_Printf( "NeonWave: NEW RECORD WAVE %i\n", nw_wave );
	}
	if ( nw_overVictory && ( nw_records.bestTime <= 0 || runSec < nw_records.bestTime ) ) {
		nw_records.bestTime = runSec;
		changed = qtrue;
		G_Printf( "NeonWave: NEW RECORD TIME %is\n", runSec );
	}
	if ( kills > nw_records.bestKills ) {
		nw_records.bestKills = kills;
		changed = qtrue;
		G_Printf( "NeonWave: NEW RECORD KILLS %i\n", kills );
	}
	if ( combo > nw_records.bestCombo ) {
		nw_records.bestCombo = combo;
		changed = qtrue;
		G_Printf( "NeonWave: NEW RECORD COMBO %ix\n", combo );
	}
	if ( changed ) {
		NW_SaveRecords();
		trap_Cvar_Set( "g_neonwave_newrecord", "1" ); // cgame: NEW RECORD banner
	}
	NW_MirrorRecordCvars();
}

// mirror current record values to cvars (called on load and on record update)
void NW_MirrorRecordCvars( void ) {
	trap_Cvar_Set( "g_neonwave_recwave", va("%i", nw_records.bestWave) );
	trap_Cvar_Set( "g_neonwave_rectime", va("%i", nw_records.bestTime) );
	trap_Cvar_Set( "g_neonwave_reckills", va("%i", nw_records.bestKills) );
	trap_Cvar_Set( "g_neonwave_reccombo", va("%i", nw_records.bestCombo) );
}

#endif // NEONARENA_MOD
