// NeonArena wave-survival gametype logic (GT_NEONWAVE)
// Spawns escalating bot waves, tracks score + best-wave highscore.
#include "g_local.h"

#ifdef NEONARENA_MOD

#define NW_FIRST_WAVE_DELAY	5000	// ms after map start
#define NW_WAVE_BREAK		8000	// ms between waves (upgrade window)
#define NW_MAX_WAVE			20
#define NW_BOSS_WAVE		10	// from here on, each wave gets one boss drone

// Test hooks (used by CI smoke test):
//   g_neonwave_autostart 1   -> waves start without a human player (headless test)
//   g_neonwave_startwave N   -> force-start wave N (polled in NeonWave_Frame)

// CS_NEONWAVE payload: "<wave> <event> <bossHp> <bossMax> <breakMs> <pts> <best> <modifier>"
// event: 0 running, 1 cleared/break, 2 failed, 3 victory
#define NW_EV_RUNNING		0
#define NW_EV_CLEARED		1
#define NW_EV_FAILED		2
#define NW_EV_VICTORY		3

// wave modifiers (from wave 5, one per wave, boss waves excluded)
#define NW_MOD_NONE			0
#define NW_MOD_GLASS		1	// all drones die to one hit, but +2 skill aggression
#define NW_MOD_SWARM		2	// double drone count, skill capped lower
#define NW_MOD_LOWGRAV		3	// g_gravity halved for the wave
#define NW_MOD_DOUBLEPTS	4	// wave clear grants x2 upgrade points

static int nw_wave;				// current wave (1-based)
static int nw_aliveBots;
static qboolean nw_fcFired;
static qboolean nw_failFired;
static int nw_modifier = NW_MOD_NONE;
static int nw_bossType = 0;		// current boss type (NW_BOSS_*)
static int nw_runStartTime;		// run stats: level.time of first wave start
static int nw_runBestCombo;		// run stats: best streak this run (survives bot disconnects)

// test helper: is g_neonwave_autokill active? (used by fakecombo hook)
static qboolean autokillActive( void ) {
	char akBuf[8];
	trap_Cvar_VariableStringBuffer( "g_neonwave_autokill", akBuf, sizeof(akBuf) );
	return ( atoi( akBuf ) == 1 ) ? qtrue : qfalse;
}

// qtrue while a fakecombo hook is set but has not fired yet (no carrier found
// or streak not yet registered) — failrun must wait for it in combined tests
static qboolean fcPending( void ) {
	char fcBuf[8];
	trap_Cvar_VariableStringBuffer( "g_neonwave_fakecombo", fcBuf, sizeof(fcBuf) );
	if ( atoi( fcBuf ) <= 0 ) {
		return qfalse; // no fakecombo requested -> nothing pending
	}
	return nw_fcFired ? qfalse : qtrue;
}

static qboolean nw_started;
static int nw_botCounter;
static qboolean nw_inBreak;
static int nw_breakEnd;
static qboolean nw_waveHadBots;	// true once at least one bot connected this wave
static qboolean nw_over;
static int nw_event;
static qboolean nw_overVictory;		// last run ended in victory (record time only counts then)
static int nw_bossAttr = 0;		// boss attribute overlay (g_neonwave_bossattr)

// ---- daily challenge (v0.14) ----
// Deterministic per-date challenge: an FNV-1a hash over YYYY-MM-DD derives
// a boss rotation offset and modifier rotation offset, so every player gets
// the same boss/modifier sequence on the same day. Enable via
// g_neonwave_daily 1 (or force a seed with g_neonwave_dailyseed N for tests).
static qboolean nw_dailyActive;
static int nw_dailyOffset;		// modifier pool rotation 0-3
static int nw_dailyBossOffset;	// boss rotation offset 0-3

#define NW_DAILY_FNV_PRIME		16777619u
#define NW_DAILY_FNV_OFFSET		2166136261u

static unsigned int NW_DailyHash( const char *s ) {
	unsigned int h = NW_DAILY_FNV_OFFSET;
	while ( *s ) {
		h ^= (unsigned char)*s++;
		h *= NW_DAILY_FNV_PRIME;
	}
	return h;
}

static void NW_DailyInit( void ) {
	char buf[16];
	qtime_t tm;
	char dateStr[32];
	int forced;

	nw_dailyActive = qfalse;
	nw_dailyOffset = 0;
	nw_dailyBossOffset = 0;
	trap_Cvar_Set( "ui_neonwave_daily", "0" );

	trap_Cvar_VariableStringBuffer( "g_neonwave_daily", buf, sizeof(buf) );
	if ( atoi( buf ) != 1 ) {
		return;
	}
	// test hook: g_neonwave_dailyseed N forces the seed value
	trap_Cvar_VariableStringBuffer( "g_neonwave_dailyseed", buf, sizeof(buf) );
	forced = atoi( buf );
	if ( forced > 0 ) {
		G_Printf( "NeonWave: DAILY CHALLENGE seed %i (forced)\n", forced );
	} else {
		trap_RealTime( &tm );
		Com_sprintf( dateStr, sizeof( dateStr ), "%04i-%02i-%02i",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday );
		forced = (int)( NW_DailyHash( dateStr ) & 0x7fffffff );
		G_Printf( "NeonWave: DAILY CHALLENGE %s seed %i\n", dateStr, forced );
	}
	nw_dailyActive = qtrue;
	nw_dailyOffset = forced % 4;
	nw_dailyBossOffset = ( forced / 4 ) % 4;
	// mirror for the cgame HUD (DAILY badge on the wave title)
	trap_Cvar_Set( "ui_neonwave_daily", "1" );
}

// daily records use their own file so normal bests stay untouched
#define NW_DAILY_RECORDS_FILE	"neonwave_daily_records.dat"

// forward declarations (records block is defined further down)
static void NW_LoadRecords( void );
static void NW_MirrorRecordCvars( void );
static void NW_LoadDailyRecords( void );
static void NW_MirrorDailyRecordCvars( void );

void NeonWave_Reset( void ) {
	nw_wave = 0;
	nw_aliveBots = 0;
	nw_modifier = NW_MOD_NONE;
	nw_runStartTime = level.time;
	nw_runBestCombo = 0;
	nw_started = qfalse;
	nw_botCounter = 0;
	nw_inBreak = qfalse;
	nw_breakEnd = 0;
	nw_waveHadBots = qfalse;
	nw_overVictory = qfalse;
	nw_bossAttr = 0;
	NW_DailyInit();
	NW_LoadRecords();
	if ( nw_dailyActive ) {
		NW_LoadDailyRecords();
	}
	nw_over = qfalse;
	nw_event = 0;
	trap_Cvar_Set( "g_neonwave_upgradepoints", "0" );
}

qboolean NeonWave_IsBreak( void ) {
	return ( nw_inBreak && !nw_over ) ? qtrue : qfalse;
}

// test hook: mark the wave loop as started (used by "nwstartwave" console cmd)
void NeonWave_ForceStarted( void ) {
	nw_started = qtrue;
	nw_over = qfalse;
}

// Sync upgrade state to the local player's HUD.
// Packs points+levels into ps.persistant[PERS_CAPTURES] (unused in GT_NEONWAVE):
// bits 0-7 points, 8-11 hp level, 12-15 dmg level, 16-19 speed level
static void NW_SyncUpgrades( void ) {
	char ptsBuf[16];
	int pts, i, val;
	gentity_t *ent;

	trap_Cvar_VariableStringBuffer( "g_neonwave_upgradepoints", ptsBuf, sizeof(ptsBuf) );
	pts = atoi( ptsBuf );
	if ( pts < 0 ) pts = 0;
	if ( pts > 255 ) pts = 255;

	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		if ( ent->r.svFlags & SVF_BOT ) continue;
		val = pts
			| ( ( ent->client->pers.neonwaveUpHp   & 0xF ) << 8 )
			| ( ( ent->client->pers.neonwaveDmg    & 0xF ) << 12 )
			| ( ( ent->client->pers.neonwaveSpeed  & 0xF ) << 16 );
		ent->client->ps.persistant[PERS_CAPTURES] = val;
	}
}

static void NW_SpawnBot( int skill ) {
	trap_SendConsoleCommand( EXEC_APPEND,
		va("addbot sarge %i \"Drone W%d-%d\"\n", skill, nw_wave, ++nw_botCounter) );
}

// boss types: rotate per boss wave, forceable via g_neonwave_bosstype for tests
#define NW_BOSS_SNIPER	1	// railgun sniper (classic)
#define NW_BOSS_TANK	2	// slow, huge HP, chaingun-style MG spam
#define NW_BOSS_SWARM	3	// spawns mini-drones during the wave
#define NW_BOSS_GLASS	4	// glass cannon: fast, 2x HP, railgun
#define NW_BOSS_WARDEN	5	// v0.15: teleport-strikes the player's zone + brief armor phase

static int NW_PickBossType( void ) {
	char btBuf[8];
	int forced;

	// test hook: g_neonwave_bosstype N forces the type
	trap_Cvar_VariableStringBuffer( "g_neonwave_bosstype", btBuf, sizeof(btBuf) );
	forced = atoi( btBuf );
	if ( forced >= NW_BOSS_SNIPER && forced <= NW_BOSS_WARDEN ) {
		return forced;
	}
	// rotate by boss-wave count: wave 10 = sniper, 20 = tank, 30 = swarm,
	// 40 = glass cannon, ... daily challenge shifts the rotation start
	{
		int rot = ( nw_wave / NW_BOSS_WAVE - 1 ) % 4;
		if ( nw_dailyActive ) {
			rot = ( rot + nw_dailyBossOffset ) % 4;
		}
		if ( nw_wave / NW_BOSS_WAVE >= 4 ) {
			return NW_BOSS_SNIPER + rot;
		}
		return NW_BOSS_SNIPER + ( rot % 3 );
	}
}

static const char *NW_BossName( int type ) {
	switch ( type ) {
	case NW_BOSS_TANK:	return "TANK";
	case NW_BOSS_SWARM:	return "SWARM MOTHER";
	case NW_BOSS_GLASS:	return "GLASS CANNON";
	case NW_BOSS_WARDEN:	return "WARDEN";
	default:		return "SNIPER";
	}
}

static void NW_SpawnBoss( void ) {
	int type = NW_PickBossType();
	int hc = 400; // classic 4x

	if ( type == NW_BOSS_TANK ) {
		hc = 600; // 6x
	}
	if ( type == NW_BOSS_GLASS ) {
		hc = 200; // 2x — glass cannon: fragile but deadly
	}
	if ( type == NW_BOSS_WARDEN ) {
		hc = 500; // 5x — warden: tanky teleporter
	}
	nw_bossType = type;
	G_Printf( "NeonWave: boss spawned: %s (hc %i)\n", NW_BossName( type ), hc );
	trap_Cvar_Set( "g_neonwave_nextboss", "1" );
	trap_Cvar_Set( "g_neonwave_bosshc", va("%i", hc) );
	trap_SendServerCommand( -1, va( "cp \"BOSS: %s\\n\"", NW_BossName( type ) ) );
	trap_SendConsoleCommand( EXEC_APPEND,
		va("addbot sarge 5 \"BOSS W%d %s\"\n", nw_wave, NW_BossName( type )) );
}

static void NW_BossHealth( int *hp, int *maxhp ) {
	gentity_t *ent;
	int i;

	*hp = 0;
	*maxhp = 0;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( !( ent->r.svFlags & SVF_BOT ) ) continue;
		if ( ent->health <= 0 ) continue;
		if ( !ent->client->pers.neonwaveBoss ) continue;
		*hp = ent->health;
		*maxhp = ent->client->ps.stats[STAT_MAX_HEALTH];
		return;
	}
}

static int NW_Points( void ) {
	char buf[16];
	trap_Cvar_VariableStringBuffer( "g_neonwave_upgradepoints", buf, sizeof(buf) );
	return atoi( buf );
}

static int NW_Best( void ) {
	char buf[16];
	trap_Cvar_VariableStringBuffer( "g_neonwave_best", buf, sizeof(buf) );
	return atoi( buf );
}

/*
================
NW_SendStatus

"<wave> <event> <bossHp> <bossMax> <breakMs> <pts> <best>"
================
*/
// ---- run statistics (aggregated over all humans) ----
// (nw_runStartTime declared with the other statics at top)

static int NW_TestPlayerSkipBots( void ) {
	char buf[8];
	trap_Cvar_VariableStringBuffer( "g_neonwave_botasplayer", buf, sizeof(buf) );
	return atoi( buf );
}

void NeonWave_TrackRunCombo( int combo ) {
	if ( combo > nw_runBestCombo ) {
		nw_runBestCombo = combo;
	}
}

static int NW_RunKills( void ) {
	int i, kills = 0;
	int skipBots = !NW_TestPlayerSkipBots();
	gentity_t *ent;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( skipBots && ( ent->r.svFlags & SVF_BOT ) ) continue;
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		kills += ent->client->pers.nwKills;
	}
	return kills;
}

static int NW_RunBestCombo( void ) {
	int i, best = nw_runBestCombo;
	int skipBots = !NW_TestPlayerSkipBots();
	gentity_t *ent;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( skipBots && ( ent->r.svFlags & SVF_BOT ) ) continue;
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		if ( ent->client->pers.nwBestCombo > best ) best = ent->client->pers.nwBestCombo;
	}
	return best;
}

// live combo streak (highest current nwCombo among humans) for the HUD popup
static int NW_RunCurrentCombo( void ) {
	int i, cur = 0;
	gentity_t *ent;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->r.svFlags & SVF_BOT ) continue;
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		if ( ent->client->nwCombo > cur ) cur = ent->client->nwCombo;
	}
	return cur;
}

// ---- persistent records (survive map change / restart) ----
#define NW_RECORDS_FILE		"neonwave_records.dat"

typedef struct {
	int	bestWave;
	int	bestTime;	// fastest victory, seconds (0 = none)
	int	bestKills;
	int	bestCombo;
} nwRecords_t;

static nwRecords_t nw_records;

static void NW_LoadRecords( void ) {
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

static void NW_SaveRecords( void ) {
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

// ---- daily records (per-day bests, separate file, reset on day change) ----
typedef struct {
	int		dayStamp;	// yyyymmdd the records belong to
	int		bestWave;
	int		bestTime;	// fastest victory, seconds (0 = none)
	int		bestKills;
	int		bestCombo;
} nwDailyRecords_t;

static nwDailyRecords_t nw_dailyRecords;

static int NW_DailyStamp( void ) {
	qtime_t tm;
	trap_RealTime( &tm );
	return ( tm.tm_year + 1900 ) * 10000 + ( tm.tm_mon + 1 ) * 100 + tm.tm_mday;
}

static void NW_LoadDailyRecords( void ) {
	fileHandle_t f;
	nwDailyRecords_t saved;
	memset( &nw_dailyRecords, 0, sizeof( nw_dailyRecords ) );
	nw_dailyRecords.dayStamp = NW_DailyStamp();
	if ( trap_FS_FOpenFile( NW_DAILY_RECORDS_FILE, &f, FS_READ ) >= 0 && f ) {
		trap_FS_Read( &saved, sizeof( saved ), f );
		if ( saved.dayStamp == nw_dailyRecords.dayStamp ) {
			// same day: keep today's records
			nw_dailyRecords = saved;
		}
		// different day: start fresh (struct already zeroed)
		trap_FS_FCloseFile( f );
	}
	G_Printf( "NeonWave: DAILY records loaded wave=%i time=%is kills=%i combo=%i\n",
		nw_dailyRecords.bestWave, nw_dailyRecords.bestTime,
		nw_dailyRecords.bestKills, nw_dailyRecords.bestCombo );
	NW_MirrorDailyRecordCvars();
}

// mirror daily record values to cvars (cgame reads them for the daily HUD)
static void NW_MirrorDailyRecordCvars( void ) {
	trap_Cvar_Set( "g_neonwave_dailyrecwave", va("%i", nw_dailyRecords.bestWave) );
	trap_Cvar_Set( "g_neonwave_dailyrectime", va("%i", nw_dailyRecords.bestTime) );
	trap_Cvar_Set( "g_neonwave_dailyreckills", va("%i", nw_dailyRecords.bestKills) );
	trap_Cvar_Set( "g_neonwave_dailyreccombo", va("%i", nw_dailyRecords.bestCombo) );
}

static void NW_SaveDailyRecords( void ) {
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

	// day rolled over mid-session: reset and continue with a fresh set
	if ( nw_dailyRecords.dayStamp != NW_DailyStamp() ) {
		NW_LoadDailyRecords();
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
	if ( changed ) {
		NW_SaveDailyRecords();
		trap_Cvar_Set( "g_neonwave_newrecord", "1" );
	}
	NW_MirrorDailyRecordCvars();
}

static void NW_UpdateRecords( void ) {
	int kills = NW_RunKills();
	int combo = NW_RunBestCombo();
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
static void NW_MirrorRecordCvars( void ) {
	trap_Cvar_Set( "g_neonwave_recwave", va("%i", nw_records.bestWave) );
	trap_Cvar_Set( "g_neonwave_rectime", va("%i", nw_records.bestTime) );
	trap_Cvar_Set( "g_neonwave_reckills", va("%i", nw_records.bestKills) );
	trap_Cvar_Set( "g_neonwave_reccombo", va("%i", nw_records.bestCombo) );
}

static void NW_SendStatus( int event ) {
	int bossHp, bossMax, breakMs;

	nw_event = event;
	NW_BossHealth( &bossHp, &bossMax );
	breakMs = 0;
	if ( nw_inBreak && nw_breakEnd > level.time ) {
		breakMs = nw_breakEnd - level.time;
	}
	// append run stats: "<wave> <ev> <bhp> <bmax> <brk> <pts> <best> <mod> <kills> <bestcombo> <runsec> <livecombo>"
	trap_SetConfigstring( CS_NEONWAVE, va( "%i %i %i %i %i %i %i %i %i %i %i %i",
		nw_wave, event, bossHp, bossMax, breakMs, NW_Points(), NW_Best(), nw_modifier,
		NW_RunKills(), NW_RunBestCombo(), ( level.time - nw_runStartTime ) / 1000,
		NW_RunCurrentCombo() ) );
}

void NeonWave_RefreshStatus( void ) {
	NW_SendStatus( nw_event );
}

void NeonWave_DropReward( int clearedWave ) {
	gentity_t *ent;
	vec3_t origin, velocity = {0, 0, 20};
	gitem_t *mega, *armor, *ammo, *ra;
	int i;

	(void)clearedWave;
	mega  = BG_FindItem( "Mega Health" );
	armor = BG_FindItem( "Heavy Armor" );
	ammo  = BG_FindItemForWeapon( WP_LIGHTNING );
	if (!mega)  mega  = BG_FindItem( "5 Health" );
	if (!armor) armor = BG_FindItem( "Armor Shard" );
	ra = BG_FindItemForWeapon( WP_RAILGUN );

	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->r.svFlags & SVF_BOT ) continue;
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		if ( ent->health <= 0 ) continue;

		VectorCopy( ent->r.currentOrigin, origin );
		origin[2] += 24;
		if ( mega )  LaunchItem( mega,  origin, velocity );
		if ( armor ) LaunchItem( armor, origin, velocity );
		if ( ammo )  LaunchItem( ammo,  origin, velocity );
		if ( ra )    LaunchItem( ra,    origin, velocity );
	}
}

static void NeonWave_UpdateHighscore( void ) {
	int best = NW_Best();
	if ( nw_wave > best ) {
		trap_Cvar_Set( "g_neonwave_best", va("%i", nw_wave) );
		G_Printf( "NeonWave: NEW BEST wave %i (was %i)\n", nw_wave, best );
	}
}

static void NW_PickModifier( int num ) {
	static const int pool[4] = { NW_MOD_GLASS, NW_MOD_SWARM, NW_MOD_LOWGRAV, NW_MOD_DOUBLEPTS };
	int idx;
	char mbBuf[8];

	nw_modifier = NW_MOD_NONE;
	if ( num < 5 || num >= NW_BOSS_WAVE || num == NW_MAX_WAVE ) {
		return;
	}
	// test hook: g_neonwave_modifier N forces modifier 1-4
	trap_Cvar_VariableStringBuffer( "g_neonwave_modifier", mbBuf, sizeof(mbBuf) );
	if ( atoi( mbBuf ) >= NW_MOD_GLASS && atoi( mbBuf ) <= NW_MOD_DOUBLEPTS ) {
		nw_modifier = atoi( mbBuf );
		return;
	}
	// daily challenge: derive the modifier rotation offset from the date seed
	if ( nw_dailyActive ) {
		idx = ( num / 2 + num % 3 + nw_dailyOffset ) % 4;
	} else {
		idx = ( num / 2 + num % 3 ) % 4;
	}
	nw_modifier = pool[idx];
}

static const char *NW_ModifierName( int mod ) {
	switch ( mod ) {
	case NW_MOD_GLASS:		return "GLASS DRONES";
	case NW_MOD_SWARM:		return "SWARM";
	case NW_MOD_LOWGRAV:	return "LOW GRAVITY";
	case NW_MOD_DOUBLEPTS:	return "DOUBLE POINTS";
	default:				return "";
	}
}

void NeonWave_StartWave( int num ) {
	int i;
	int skill = 1 + num / 3;
	int botCount;
	char mwBuf[8];
	int maxWave;

	NW_PickModifier( num );
	if ( skill > 5 ) skill = 5;
	nw_wave = num;
	nw_botCounter = 0;
	nw_inBreak = qfalse;
	nw_waveHadBots = qfalse;

	// endless mode: past the classic max wave, keep scaling difficulty
	trap_Cvar_VariableStringBuffer( "g_neonwave_maxwave", mwBuf, sizeof(mwBuf) );
	maxWave = atoi( mwBuf );
	if ( maxWave <= 0 ) {
		maxWave = NW_MAX_WAVE;
	}
	if ( num > maxWave ) {
		// +2 bots every 3 waves past max, capped at client limit
		botCount = maxWave + 1 + ((num - maxWave) / 3) * 2;
	} else {
		botCount = num + 1;
	}

	// apply modifier side effects
	if ( nw_modifier == NW_MOD_LOWGRAV ) {
		trap_Cvar_Set( "g_gravity", "400" ); // half of default 800
	} else {
		trap_Cvar_Set( "g_gravity", "800" );
	}
	if ( nw_modifier == NW_MOD_GLASS && skill < 4 ) {
		skill += 1; // glass drones are fast/aggressive
	}
	if ( nw_modifier == NW_MOD_SWARM ) {
		botCount *= 2;
	}

	NW_SendStatus( NW_EV_RUNNING );
	if ( nw_modifier != NW_MOD_NONE ) {
		trap_SendServerCommand( -1, va( "cp \"WAVE %i: %s\\n\"", num, NW_ModifierName( nw_modifier ) ) );
	} else {
		trap_SendServerCommand( -1, va( "cp \"WAVE %i\\n\"", num ) );
	}
	G_Printf( "NeonWave: starting wave %i (%i bots, skill %i)%s%s\n", num, botCount, skill,
		num >= NW_BOSS_WAVE ? " + BOSS" : "",
		nw_modifier != NW_MOD_NONE ? va(" [%s]", NW_ModifierName( nw_modifier )) : "" );
	if ( num >= NW_BOSS_WAVE ) {
		NW_SpawnBoss();
	}
	for ( i = 0; i < botCount && i < MAX_CLIENTS - 2; i++ ) {
		NW_SpawnBot( skill );
	}
}

int NeonWave_GetWave( void ) {
	return nw_wave;
}

static void NW_GrantUpgradePoints( void ) {
	int pts = NW_Points();
	int gain = ( nw_wave >= NW_BOSS_WAVE ? 2 : 1 );
	int combo;

	if ( nw_modifier == NW_MOD_DOUBLEPTS ) {
		gain *= 2;
	}
	// boss kill bonus: +3 for taking down a boss wave
	if ( nw_wave % NW_BOSS_WAVE == 0 ) {
		gain += 3;
		G_Printf( "NeonWave: boss kill bonus +3\n" );
	}
	// combo bonus: +1 point for streaks of 5+ kills
	combo = NW_RunBestCombo();
	if ( combo >= 5 ) {
		gain += combo / 5;
		G_Printf( "NeonWave: combo bonus +%i (best streak %i)\n", combo / 5, combo );
	}
	// v0.13: mega reward — best streak of 8+ drops a Quad Damage pickup at
	// the wave-clear reward point (stacks with the standard item drop)
	if ( combo >= 8 ) {
		gitem_t *quad = BG_FindItem( "Quad Damage" );
		if ( quad ) {
			// drop at the first connected client; under botasplayer (headless
				// CI) the carrier is a bot, so don't exclude bots here. If no
				// client is alive at grant time (autokill cleared the wave),
				// fall back to a player spawn point so the reward still drops.
				gentity_t *spot = NULL;
				int j;
				for ( j = 0; j < level.maxclients && !spot; j++ ) {
					gentity_t *e = &g_entities[j];
					if ( e->inuse && e->client
							&& e->client->pers.connected == CON_CONNECTED ) {
						spot = e;
					}
				}
				if ( !spot ) {
					gentity_t *spawn = NULL;
					while ( ( spawn = G_Find( spawn, FOFS( classname ), "info_player_deathmatch" ) ) != NULL ) {
						spot = spawn;
						break;
					}
				}
			if ( spot ) {
				vec3_t qorigin, qvel = {0, 0, 20};
				VectorCopy( spot->r.currentOrigin, qorigin );
				qorigin[2] += 24;
				LaunchItem( quad, qorigin, qvel );
				G_Printf( "NeonWave: MEGA COMBO %i — Quad Damage dropped\n", combo );
			}
		}
	}
	pts += gain;
	trap_Cvar_Set( "g_neonwave_upgradepoints", va("%i", pts) );
	G_Printf( "NeonWave: upgrade point granted (%i banked)\n", pts );
}

static void NW_EnterBreak( void ) {
	nw_inBreak = qtrue;
	// restore gravity after low-grav wave; log it so headless tests can assert
	// that the modifier side effect is properly undone
	if ( nw_modifier == NW_MOD_LOWGRAV ) {
		trap_Cvar_Set( "g_gravity", "800" );
		G_Printf( "NeonWave: gravity restored to 800\\n" );
	}
	nw_breakEnd = level.time + NW_WAVE_BREAK;
	// test hook: shorten break window when g_neonwave_fastbreak is set
	{
		char fbBuf[8];
		trap_Cvar_VariableStringBuffer( "g_neonwave_fastbreak", fbBuf, sizeof(fbBuf) );
		if ( atoi( fbBuf ) == 1 ) {
			nw_breakEnd = level.time + 500;
		}
	}
	NW_GrantUpgradePoints();
	NeonWave_DropReward( nw_wave );
	NW_SendStatus( NW_EV_CLEARED );
	trap_SendServerCommand( -1, va( "cp \"WAVE %i CLEARED\nF1 HP  F2 DMG  F3 SPEED\"", nw_wave ) );
	G_Printf( "NeonWave: wave %i cleared, break %i ms\n", nw_wave, NW_WAVE_BREAK );
}

static void NW_KickBots( void ) {
	int i;
	gentity_t *ent;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( !( ent->r.svFlags & SVF_BOT ) ) continue;
		trap_DropClient( i, "eliminated" );
	}
}

static void NW_GameOver( int event, const char *why ) {
	if ( nw_over ) {
		return;
	}
	nw_over = qtrue;
	nw_inBreak = qfalse;
	nw_overVictory = ( event == NW_EV_VICTORY ) ? qtrue : qfalse;
	// restore gravity in case we ended during a low-grav wave
	trap_Cvar_Set( "g_gravity", "800" );
	NeonWave_UpdateHighscore();
	NW_UpdateRecords();
	NW_SendStatus( event );
	G_Printf( "NeonWave: RUN STATS kills=%i bestCombo=%i time=%is\n",
		NW_RunKills(), NW_RunBestCombo(), ( level.time - nw_runStartTime ) / 1000 );
	NW_KickBots();
	G_Printf( "NeonWave: %s (wave %i)\n", why, nw_wave );
	LogExit( why );
}

// ---- v0.11 boss special mechanics ----
#define NW_BOSS_SHIELD_MS	4000	// tank shield phase duration
#define NW_BOSS_SHIELD_CD	12000	// tank shield cooldown
#define NW_BOSS_RAGE_HP		0.30f	// swarm mother rage below 30% hp

static gentity_t *NW_FindBoss( void ) {
	gentity_t *ent;
	int i;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( ent->inuse && ent->client && ( ent->r.svFlags & SVF_BOT )
				&& ent->health > 0 && ent->client->pers.neonwaveBoss ) {
			return ent;
		}
	}
	return NULL;
}

static void NW_BossMechanicsFrame( int *lastMini, int bots ) {
	if ( nw_bossType == NW_BOSS_SWARM ) {
		// swarm mother: keep spawning mini-drones while the boss lives;
		// rage mode below 30% hp halves the spawn interval.
		// test hook g_neonwave_rageforce 1 forces the rage state so CI can
		// assert the ENRAGED log + faster spawns deterministically
		gentity_t *boss = NW_FindBoss();
		if ( boss ) {
			char rfBuf[8];
			int maxhp = boss->client->ps.stats[STAT_MAX_HEALTH];
			qboolean rage;
			trap_Cvar_VariableStringBuffer( "g_neonwave_rageforce", rfBuf, sizeof(rfBuf) );
			if ( atoi( rfBuf ) == 1 ) {
				boss->health = maxhp * NW_BOSS_RAGE_HP / 2; // force rage window
			}
			rage = ( maxhp > 0
				&& boss->health < maxhp * NW_BOSS_RAGE_HP ) ? qtrue : qfalse;
			if ( rage && !*lastMini ) {
				G_Printf( "NeonWave: SWARM MOTHER ENRAGED\n" );
			}
			// cap minidrones below sv_maxclients (24): wave 10 spawns 11
				// drones + 1 boss = 12 clients, so the old hardcap of 10
				// blocked every mini-drone spawn in headless CI runs
				if ( level.time > *lastMini && bots < 20 ) {
				*lastMini = level.time + ( rage ? 5000 : 10000 );
				NW_SpawnBot( 3 );
				G_Printf( "NeonWave: swarm mother spawns mini-drone%s\n",
					rage ? " (RAGE)" : "" );
			}
			return;
		}
	}

	if ( nw_bossType == NW_BOSS_TANK ) {
		// tank: periodic regeneration burst while "shielded" (visualized by
		// the boss glow pulse); implemented as self-heal to avoid touching
		// G_Damage — keeps all boss logic inside g_neonwave.c
		static int nextShield;
		gentity_t *boss = NW_FindBoss();
		if ( boss ) {
			int maxhp = boss->client->ps.stats[STAT_MAX_HEALTH];
			if ( !boss->client->pers.neonwaveBossShield && level.time > nextShield ) {
				boss->client->pers.neonwaveBossShield = 1;
				boss->client->pers.neonwaveBossShieldEnd = level.time + NW_BOSS_SHIELD_MS;
				nextShield = level.time + NW_BOSS_SHIELD_CD;
				G_Printf( "NeonWave: TANK raises SHIELD\n" );
			}
			if ( boss->client->pers.neonwaveBossShield ) {
				if ( level.time > boss->client->pers.neonwaveBossShieldEnd ) {
					boss->client->pers.neonwaveBossShield = 0;
					G_Printf( "NeonWave: TANK shield drops\n" );
				} else if ( maxhp > 0 && boss->health < maxhp ) {
					boss->health += 2; // steady regen while shielded
					if ( boss->health > maxhp ) {
						boss->health = maxhp;
					}
				}
			}
		}
	}

	if ( nw_bossType == NW_BOSS_WARDEN ) {
		// v0.15 WARDEN: periodically teleports INTO the player's zone
		// (offensive strike, opposite of the sniper's escape dash), then
		// gains a brief armor phase after arriving so the player must
		// reposition before trading damage.
		// test hook g_neonwave_wardenforce 1 forces the strike immediately
		static int nextStrike;
		char wfBuf[8];
		int forcedStrike;
		gentity_t *boss = NW_FindBoss();
		gentity_t *player = NULL;
		int i;
		trap_Cvar_VariableStringBuffer( "g_neonwave_wardenforce", wfBuf, sizeof(wfBuf) );
		forcedStrike = atoi( wfBuf );
		if ( level.time > nextStrike || forcedStrike == 1 ) {
			nextStrike = level.time + 8000;
			// nearest connected human (or bot carrier under botasplayer)
			for ( i = 0; i < level.maxclients && !player; i++ ) {
				gentity_t *e = &g_entities[i];
				if ( e->inuse && e->client
						&& e->client->pers.connected == CON_CONNECTED
						&& e->health > 0 ) {
					player = e;
				}
			}
			if ( boss && player ) {
				vec3_t org;
				VectorCopy( player->r.currentOrigin, org );
				org[0] += ( rand() % 300 ) - 150; // land close but not on top
				org[1] += ( rand() % 300 ) - 150;
				VectorCopy( org, boss->s.origin );
				VectorCopy( org, boss->client->ps.origin );
				G_Printf( "NeonWave: WARDEN strikes the player zone\n" );
				// armor phase: 3 s of damage reduction via health buffer top-up
				boss->client->pers.neonwaveBossShield = 1;
				boss->client->pers.neonwaveBossShieldEnd = level.time + 3000;
				G_Printf( "NeonWave: WARDEN raises armor\n" );
			}
		}
		// drop the armor flag when the phase ends (log once)
		if ( boss && boss->client->pers.neonwaveBossShield
				&& level.time > boss->client->pers.neonwaveBossShieldEnd ) {
			boss->client->pers.neonwaveBossShield = 0;
			G_Printf( "NeonWave: WARDEN armor drops\n" );
		}
	}

	if ( nw_bossType == NW_BOSS_SNIPER ) {
		// sniper: teleport-dash away when hit below 50% (repositioning);
		// test hook g_neonwave_bosshppct N forces the boss to spawn at
		// maxhp*N/100 so the dash path is reachable deterministically
		static int lastDash;
		gentity_t *boss = NW_FindBoss();
		if ( boss && level.time > lastDash ) {
			int maxhp = boss->client->ps.stats[STAT_MAX_HEALTH];
			lastDash = level.time + 9000;
			{
				char pctBuf[8];
				int pct;
				trap_Cvar_VariableStringBuffer( "g_neonwave_dashforce", pctBuf, sizeof(pctBuf) );
				pct = atoi( pctBuf );
				if ( pct > 0 ) {
					boss->health = maxhp * pct / 100; // force low-hp state for tests
				}
			}
			if ( maxhp > 0 && boss->health < maxhp / 2 ) {
				vec3_t org = { 0, 0, 0 };
				VectorCopy( boss->r.currentOrigin, org );
				// dash: small random offset teleport
				org[0] += ( rand() % 400 ) - 200;
				org[1] += ( rand() % 400 ) - 200;
				VectorCopy( org, boss->s.origin );
				VectorCopy( org, boss->client->ps.origin );
				G_Printf( "NeonWave: SNIPER dashes to new position\n" );
			}
		}
	}
}

void NeonWave_Frame( void ) {
	int humans, bots, i;
	gentity_t *ent;
	static int lastRefresh;

	if ( g_gametype.integer != GT_NEONWAVE ) {
		return;
	}
	if ( level.intermissiontime || level.intermissionQueued ) {
		return;
	}
	if ( nw_over ) {
		return;
	}

	humans = 0;
	bots = 0;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		if ( ent->health <= 0 ) continue;
		if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) continue;
		if ( ent->r.svFlags & SVF_BOT ) bots++;
		else humans++;
	}
	nw_aliveBots = bots;

	// test hook: g_neonwave_fakecombo N simulates a human kill streak of N
	// (tests the combo bonus + RUN STATS pipeline without real players)
	{
	char fcBuf[8];
	trap_Cvar_VariableStringBuffer( "g_neonwave_fakecombo", fcBuf, sizeof(fcBuf) );
	if ( !nw_fcFired && atoi( fcBuf ) > 0 && nw_started && nw_wave > 0 ) {
		int i2, n = atoi( fcBuf );
		gentity_t *h = NULL;
		// attribute the streak to the first connected client; with
		// g_neonwave_botasplayer 1 a bot counts as the streak carrier
		// (headless CI has no human client)
		for ( i2 = 0; i2 < level.maxclients; i2++ ) {
			ent = &g_entities[i2];
			if ( ent->inuse && ent->client
					&& ent->client->pers.connected == CON_CONNECTED
					&& ( !autokillActive() || ( ent->r.svFlags & SVF_BOT )
						|| NW_TestPlayerSkipBots() ) ) {
				h = ent;
				break;
			}
		}
		if ( !h ) {
			// no client connected yet (bots spawn frames after wave start):
			// do NOT consume the hook, retry next frame
			return;
		}
		nw_fcFired = qtrue;
		{
			int k;
			for ( k = 0; k < n; k++ ) {
				if ( k > 0 && level.time - h->client->nwLastKillTime >= 3000 ) {
					level.time -= 2000; // keep the chain alive for long streaks
				}
				h->client->nwLastKillTime = level.time;
				h->client->nwCombo++;
				h->client->pers.nwKills++;
			}
			if ( h->client->nwCombo > h->client->pers.nwBestCombo ) {
				h->client->pers.nwBestCombo = h->client->nwCombo;
			}
			if ( h->client->nwCombo > nw_runBestCombo ) {
				nw_runBestCombo = h->client->nwCombo;
			}
			G_Printf( "NeonWave: fake combo %i registered (best %i)\n",
				n, h->client->pers.nwBestCombo );
		}
	}
	}

	if ( bots > 0 ) {
		nw_waveHadBots = qtrue;
		// test hook: g_neonwave_autokill 1 -> kill all drones each frame
		// so a headless run plays through waves up to victory automatically
		{
			char akBuf[8];
			trap_Cvar_VariableStringBuffer( "g_neonwave_autokill", akBuf, sizeof(akBuf) );
			if ( atoi( akBuf ) == 1 ) {
				for ( i = 0; i < level.maxclients; i++ ) {
					ent = &g_entities[i];
					if ( !ent->inuse || !ent->client ) continue;
					if ( !( ent->r.svFlags & SVF_BOT ) ) continue;
					if ( ent->health <= 0 ) continue;
					ent->health = 0;
					ent->client->ps.stats[STAT_HEALTH] = 0;
				}
			}
		}
	}

	// test hook: g_neonwave_startwave N forces wave N (polled every frame,
	// works headless regardless of when the cvar is set)
	if ( !nw_over ) {
		char swBuf[8];
		int sw;
		trap_Cvar_VariableStringBuffer( "g_neonwave_startwave", swBuf, sizeof(swBuf) );
		sw = atoi( swBuf );
		if ( sw > 0 && sw != nw_wave ) {
			NeonWave_ForceStarted();
			nw_inBreak = qfalse;
			trap_Cvar_Set( "g_neonwave_startwave", "0" ); // consume (fire once)
			NeonWave_StartWave( sw );
			return;
		}
	}

	if ( !nw_started ) {
		qboolean autostart = qfalse;
		{
			char asBuf[8];
			trap_Cvar_VariableStringBuffer( "g_neonwave_autostart", asBuf, sizeof(asBuf) );
			autostart = ( atoi( asBuf ) != 0 ) ? qtrue : qfalse;
		}
		if ( ( humans > 0 || autostart ) && level.time > NW_FIRST_WAVE_DELAY ) {
			nw_started = qtrue;
			NeonWave_StartWave( 1 );
		}
		return;
	}

	if ( humans == 0 && !trap_Cvar_VariableValue( "g_neonwave_autostart" ) ) {
		NW_GameOver( NW_EV_FAILED, "NeonWave over" );
		return;
	}

	// test hook: g_neonwave_failrun 1 -> trigger failed game over once (tests
	// the FAILED path + RUN STATS + end screen without a real player death)
	// test hook: g_neonwave_failrun 1 -> trigger failed game over once
	{
		char frBuf[8];
		trap_Cvar_VariableStringBuffer( "g_neonwave_failrun", frBuf, sizeof(frBuf) );
		// NOTE: failFired/fcFired are declared in the shared wrapper block
		// above; when fakecombo is ALSO set, wait for the combo pipeline so
		// the combined test exercises the streak before the run ends. With
		// failrun alone (no fakecombo) fire immediately after wave start.
		if ( !nw_failFired && atoi( frBuf ) == 1 && nw_started && nw_wave > 0
				&& ( nw_fcFired || !fcPending() ) ) {
			nw_failFired = qtrue;
			NW_GameOver( NW_EV_FAILED, "NeonWave over" );
			return;
		}
	}

	if ( nw_inBreak ) {
		if ( level.time >= nw_breakEnd ) {
			nw_inBreak = qfalse;
			NeonWave_StartWave( nw_wave + 1 );
			return;
		}
		if ( level.time > lastRefresh ) {
			lastRefresh = level.time + 200;
			NW_SyncUpgrades();
			NW_SendStatus( NW_EV_CLEARED );
		}
		return;
	}

	if ( nw_waveHadBots && bots == 0 ) {
		// endless mode: g_neonwave_maxwave 0 = unlimited waves; victory only
		// when the current wave reaches the (cvar-set) final wave
		char mwBuf[8];
		int maxWave;
		trap_Cvar_VariableStringBuffer( "g_neonwave_maxwave", mwBuf, sizeof(mwBuf) );
		maxWave = atoi( mwBuf );
		if ( maxWave <= 0 ) {
			maxWave = NW_MAX_WAVE;
		}
		if ( nw_wave >= maxWave ) {
			int runSec = ( level.time - nw_runStartTime ) / 1000;
			char btBuf[16];
			int bestTime;
			// time attack: g_neonwave_besttime tracks fastest victory (seconds)
			trap_Cvar_VariableStringBuffer( "g_neonwave_besttime", btBuf, sizeof(btBuf) );
			bestTime = atoi( btBuf );
			if ( bestTime <= 0 || runSec < bestTime ) {
				trap_Cvar_Set( "g_neonwave_besttime", va("%i", runSec) );
				G_Printf( "NeonWave: NEW BEST TIME %is\n", runSec );
			}
			G_Printf( "NeonWave: victory time %is (best %is)\n", runSec,
				( bestTime > 0 && runSec >= bestTime ) ? bestTime : runSec );
			NW_GrantUpgradePoints();
			NeonWave_DropReward( nw_wave );
			NW_GameOver( NW_EV_VICTORY, "All waves cleared" );
			return;
		}
		NW_EnterBreak();
		return;
	}

	if ( bots > 0 && nw_wave >= NW_BOSS_WAVE ) {
		// v0.11 boss special mechanics
		static int lastMini;
		NW_BossMechanicsFrame( &lastMini, bots );
		if ( level.time > lastRefresh ) {
			lastRefresh = level.time + 250;
			NW_SyncUpgrades();
			NW_SendStatus( NW_EV_RUNNING );
		}
	}
}

#endif // NEONARENA_MOD
