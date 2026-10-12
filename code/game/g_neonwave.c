// NeonArena wave-survival gametype logic (GT_NEONWAVE)
// Spawns escalating bot waves, tracks score + best-wave highscore.
#include "g_neonwave.h"

#ifdef NEONARENA_MOD
#include "neon_daily_pool.h"
#include "g_nw_internal.h"
#include "neon_maplook.h"

#define NW_FIRST_WAVE_DELAY	5000	// ms after map start
#define NW_WAVE_BREAK		12000	// ms between waves (perk shop)
#define NW_MAX_WAVE			20
#define NW_BOSS_WAVE		10	// from here on, each wave gets one boss drone
#define NW_BOSS_COUNT		13	// SNIPER TANK SWARM GLASS WARDEN BERSERKER TELEPORTER HEALER SHIELDER SNIPELITE DEMOLISHER CHRONOMANCER VOIDWALKER
#define REPLAY_MAX_EVENTS	32768	// max events in replay buffer (must match replay_recorder.c)

// Test hooks (used by CI smoke test):
//   g_neonwave_autostart 1   -> waves start without a human player (headless test)
//   g_neonwave_startwave N   -> force-start wave N (polled in NeonWave_Frame)

// CS_NEONWAVE payload: "<wave> <event> <bossHp> <bossMax> <breakMs> <pts> <best> <mod> <kills> <bestCombo> <runSec> <liveCombo> <bossType>"
// event: 0 running, 1 cleared/break, 2 failed, 3 victory
#define NW_EV_RUNNING		0
#define NW_EV_CLEARED		1
#define NW_EV_FAILED		2
#define NW_EV_VICTORY		3

// wave modifiers (from wave 5 through max-1, including boss waves)

// v0.80: New bot types
#define NW_BOT_NONE     0
#define NW_BOT_HEALER   1   // Heals nearby bots (green glow)
#define NW_BOT_SHIELDER 2   // Gives shields to nearby bots (blue glow)
#define NW_BOT_ELITE    3   // Fast dash, high damage (red glow)

// Bot type spawn chances (per 100 bots)
#define NW_HEALER_RATE   15  // 15% chance per bot after wave 8
#define NW_SHIELDER_RATE 10  // 10% chance per bot after wave 10
#define NW_ELITE_RATE     5  // 5% chance per bot after wave 12

int nw_wave;				// current wave (1-based)
static int nw_aliveBots;
int nw_multikillCount = 0;
static int nw_multikillTime = 0;
qboolean nw_untouchableWave = qtrue;
static qboolean nw_fcFired;
static qboolean nw_failFired;
static qboolean nw_replayTestDone76;	// v0.38: test 76 roundtrip synchronisation
static qboolean nw_replayTestDone77;	// v0.38: test 77 save header
static qboolean nw_replayTestDone78;	// v0.38: test 78 load/verify
static qboolean nw_replayTestDone79;	// v0.38: test 79 playback walk
static qboolean nw_replayTestDone80;	// v0.38: test 80 overflow
static int nw_replayEdgeEvents;		// v0.38: over-limit counter for test 80 (accumulated in G_ReplayRecord)
int nw_runKills;			// autokill + headless kill counter (achievements)
static int nw_bossType = 0;		// current boss type (NW_BOSS_*)
static int nw_bossPhase = 1;		// boss phase (1 normal, 2 enraged after 50% hp)
static int nw_bossLastAttack = 0;	// last BERSERKER attack frame (speed hacking)
int nw_runStartTime;		// run stats: level.time of first wave start
static int nw_runBestCombo;		// run stats: best streak this run (survives bot disconnects)
static int nw_difficulty = 0;	// dynamic difficulty tier -2..1 (0=normal)
qboolean nw_hardcore;	// hardcore mode (g_neonwave_hardcore): tougher run, run-stats JSON
qboolean nw_dailyActive;	// daily challenge (same seed = same run for everyone)
static int nw_lastSeenDeaths = -1;	// dynamic difficulty: deaths at last wave clear
// Performance: status dirty flag — only send when data changes
static int nw_statusDirty = 0;
static int nw_lastStatusWave = 0;
static int nw_lastStatusHp = 0;
static int nw_lastStatusPts = 0;
static const char *NW_DifficultyName( int d );
int NW_RunDeaths( void );
static const char *NW_PerkName( int id );
static int NW_PerkCap( int id );
static qboolean NW_DifficultyLocked( void );
static void NW_RollOffers( int clientID );
static int NW_TestPlayerSkipBots( void );
qboolean NW_Headless( void );
static int NW_CoopMockExtra( qboolean alive );

// ---- vampiric healing stubs (implemented in g_combat.c via cvar) ----
static gentity_t *NW_VampireHealTarget( void ) {
	int i;
	gentity_t *ent;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->r.svFlags & SVF_BOT && !NW_TestPlayerSkipBots() ) continue;
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		if ( ent->health > 0 ) return ent;
	}
	return NULL;
}

static void NW_VampireHeal( gentity_t *t ) {
	char buf[8];
	int heal;
	if ( !t || !t->client || t->health <= 0 ) {
		return;
	}
	trap_Cvar_VariableStringBuffer( "g_neonwave_vampheal", buf, sizeof(buf) );
	heal = atoi( buf );
	if ( heal <= 0 ) {
		heal = 4;
	}
	t->health += heal;
	t->client->ps.stats[STAT_HEALTH] = t->health;
	G_Printf( "NeonWave: VAMPIRE lifesteal +%i\n", heal );
}

// ---- performance cache (v0.42): aggregate per-client scans into one pass ----
// NW_SendStatus (every 200-250ms) and NW_GameOver each call NW_RunKills,
// NW_RunBestCombo, NW_RunCurrentCombo, NW_BossHealth, NW_PointsBroadcast —
// five O(maxclients) loops. We compute them all in one pass and cache the
// result, invalidating only when relevant state changes.
static nwCache_t nw_cache;
void NW_InvalidateCache( void ) {
    nw_cache.valid = qfalse;
}

// Single-pass aggregation of all per-client statistics.
const nwCache_t *NW_Cache( void ) {
    int i;
    gentity_t *ent;
    if ( nw_cache.valid ) {
        return &nw_cache;
    }
    nw_cache.kills = 0;
    nw_cache.bestCombo = nw_runBestCombo;
    nw_cache.currentCombo = 0;
    nw_cache.bossHp = 0;
    nw_cache.bossMax = 0;
    nw_cache.points = 0;
    for ( i = 0; i < level.maxclients; i++ ) {
        ent = &g_entities[i];
        if ( !ent->inuse || !ent->client ) continue;
        if ( ent->client->pers.connected != CON_CONNECTED ) continue;
        if ( !( ent->r.svFlags & SVF_BOT ) || NW_TestPlayerSkipBots() ) {
            nw_cache.kills += ent->client->pers.nwKills;
        }
        if ( !( ent->r.svFlags & SVF_BOT ) || NW_TestPlayerSkipBots() ) {
            if ( ent->client->pers.nwBestCombo > nw_cache.bestCombo ) {
                nw_cache.bestCombo = ent->client->pers.nwBestCombo;
            }
        }
        if ( !( ent->r.svFlags & SVF_BOT ) ) {
            if ( ent->client->nwCombo > nw_cache.currentCombo ) {
                nw_cache.currentCombo = ent->client->nwCombo;
            }
        }
        if ( nw_cache.points == 0 && !( ent->r.svFlags & SVF_BOT ) ) {
            nw_cache.points = ent->client->pers.neonwaveUpgradePts;
        }
        if ( ( ent->r.svFlags & SVF_BOT ) && ent->health > 0
                && ent->client->pers.neonwaveBoss ) {
            nw_cache.bossHp = ent->health;
            nw_cache.bossMax = ent->client->ps.stats[STAT_MAX_HEALTH];
        }
    }
    nw_cache.kills += nw_runKills;
    nw_cache.valid = qtrue;
    return &nw_cache;
}

// ---- dynamic difficulty (v0.16): scale challenge to player performance ----
// deaths push down, clean streak waves push up. Applied as boss HP multiplier.
static const char *NW_DifficultyName( int d ) {
	switch ( d ) {
	case 1:	return "HARD";
	case -1:	return "EASY";
	case -2:	return "RELAX";
	default:	return "NORMAL";
	}
}

// Daily and Hardcore must be the same challenge for every player / the
// whole run. Adaptive difficulty would desync Daily seeds and soften Hardcore.
static qboolean NW_DifficultyLocked( void ) {
	return ( nw_dailyActive || nw_hardcore ) ? qtrue : qfalse;
}

// called on every human death; two quick deaths soften the run
void NeonWave_OnPlayerDeath( struct gclient_s *client ) {
	if ( g_gametype.integer != GT_NEONWAVE ) return;
	if ( NW_DifficultyLocked() ) return;
	if ( client->nwDeaths > 0 && client->nwDeaths % 2 == 0 && nw_difficulty > -2 ) {
		nw_difficulty--;
		G_Printf( "NeonWave: dynamic difficulty -> %s\n", NW_DifficultyName( nw_difficulty ) );
		trap_SendServerCommand( -1, va( "cp \"DIFFICULTY: %s\\n\"", NW_DifficultyName( nw_difficulty ) ) );
	}
}

// called at wave clear; a wave cleared without dying since last clear hardens
static void NW_UpdateDifficultyOnClear( void ) {
	int deathsNow;
	if ( NW_DifficultyLocked() ) {
		return;
	}
	deathsNow = NW_RunDeaths();
	if ( deathsNow == nw_lastSeenDeaths && nw_difficulty < 1 ) {
		// wave cleared without dying since the last clear -> harder
		nw_difficulty++;
		G_Printf( "NeonWave: dynamic difficulty -> %s\n", NW_DifficultyName( nw_difficulty ) );
		trap_SendServerCommand( -1, va( "cp \"DIFFICULTY: %s\\n\"", NW_DifficultyName( nw_difficulty ) ) );
	}
	nw_lastSeenDeaths = deathsNow;
}

// total human deaths this run
int NW_RunDeaths( void ) {
	int i, deaths = 0;
	gentity_t *ent;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->r.svFlags & SVF_BOT ) continue;
		deaths += ent->client->nwDeaths;
	}
	return deaths;
}

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
static qboolean nw_chaosActive;
static qboolean nw_inBreak;
static int nw_breakEnd;
static qboolean nw_waveHadBots;	// true once at least one bot connected this wave
static qboolean nw_over;
static int nw_event;
qboolean nw_overVictory;		// last run ended in victory (record time only counts then)
static int nw_bossAttr = 0;		// boss attribute overlay (g_neonwave_bossattr)
int nw_perk[ MAX_CLIENTS ][ NW_PERK_COUNT ];	// per-client stacks/charges
static int nw_offer[ MAX_CLIENTS ][ 3 ];			// per-client break-window perk cards
static int nw_waveStartTime;
static int nw_fxSeq;

// ---- daily challenge (v0.14) ----
// Deterministic per-date challenge: an FNV-1a hash over YYYY-MM-DD derives
// a boss rotation offset and modifier rotation offset, so every player gets
// the same boss/modifier sequence on the same day. Enable via
// g_neonwave_daily 1 (or force a seed with g_neonwave_dailyseed N for tests).
static int nw_dailyBossOffset;	// boss rotation offset 0..(NW_BOSS_COUNT-1)

static void NW_DailyInit( void ) {
	char buf[16];
	qtime_t tm;
	char dateStr[32];
	int forced;

	nw_dailyActive = qfalse;
	nw_dailyOffset = 0;
	nw_dailyBossOffset = 0;
	trap_Cvar_Set( "ui_neonwave_daily", "0" );
	trap_Cvar_Set( "ui_neonwave_dailymap", "" );

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
	nw_dailyOffset = forced % NW_MOD_POOL_SIZE;
	nw_dailyBossOffset = ( forced / NW_MOD_POOL_SIZE ) % NW_BOSS_COUNT;
	{
		int mi = NW_DailyPoolIndex( forced );
		G_Printf( "NeonWave: DAILY MAP %s (pool index %i/%i)\n", nw_daily_key[mi], mi, NW_DAILY_POOL_SIZE );
		G_Printf( "NeonWave: DAILY BSP %s\n", nw_daily_bsp[mi] );
		G_Printf( "NeonWave: DAILY ARENA %s\n", nw_daily_arena[mi][0] ? nw_daily_arena[mi] : "-" );
		trap_Cvar_Set( "ui_neonwave_dailymap", nw_daily_key[mi] );
		trap_Cvar_Set( "ui_neonwave_dailybsp", nw_daily_bsp[mi] );
	}
	// mirror for the cgame HUD (DAILY badge on the wave title)
	trap_Cvar_Set( "ui_neonwave_daily", "1" );
}

// daily records use their own file so normal bests stay untouched
static int nw_bossEntityCache = -1;	// Cached boss entity index (-1 = invalid)

static void NW_InvalidateBossCache( void ) {
	nw_bossEntityCache = -1;
}

static void NW_ApplyMapLook( void ) {
	char map[MAX_QPATH];
	char arena[64];
	char buf[8];
	const nwMapLook_t *look;

	trap_Cvar_VariableStringBuffer( "mapname", map, sizeof( map ) );
	trap_Cvar_VariableStringBuffer( "g_neonwave_arena", arena, sizeof( arena ) );
	look = NW_MapLookArena( arena, map );
	Com_sprintf( buf, sizeof( buf ), "%i", look->overbright );
	trap_Cvar_Set( "r_mapoverbrightbits", buf );
	trap_Cvar_Set( "r_gamma", look->gamma );
	trap_Cvar_Set( "r_bloom_intensity", look->bloom_i );
	trap_Cvar_Set( "r_bloom_threshold", look->bloom_t );
	trap_Cvar_Set( "cg_neon_grid", look->grid );
	G_Printf( "NeonArena: look %s overbright=%i bloom=%s grid=%s\n",
		look->key, look->overbright, look->bloom_i, look->grid );
}

void NeonWave_Reset( void ) {
	nw_wave = 0;
	nw_aliveBots = 0;
	nw_modifier = NW_MOD_NONE;
	nw_modifier2 = NW_MOD_NONE;
	nw_bossType = 0;
	nw_bossPhase = 1;
	nw_runStartTime = level.time;
	nw_runBestCombo = 0;
	nw_runKills = 0;
	NW_InvalidateBossCache();
	nw_multikillCount = 0;
	nw_multikillTime = 0;
	nw_untouchableWave = qtrue;
	nw_difficulty = 0;
	NW_GhostReset();
	nw_lastSeenDeaths = -1;
	nw_synergyIdx = -1;
	nw_ptsMul = 1;
	nw_modifiersSeen = 0;
	{
		int ai;
		for ( ai = 0; ai < NW_ACH_COUNT; ai++ ) {
			nw_achievements[ai] = qfalse;
		}
	}
	// hardcore mode: g_neonwave_hardcore 1 -> tougher run (more drones, harder
	// bosses). Read once at run start so mid-run cvar changes don't shift it.
	{
		char hcBuf[8];
		trap_Cvar_VariableStringBuffer( "g_neonwave_hardcore", hcBuf, sizeof(hcBuf) );
		nw_hardcore = ( atoi( hcBuf ) == 1 ) ? qtrue : qfalse;
		if ( nw_hardcore ) {
			G_Printf( "NeonWave: HARDCORE mode enabled\n" );
		}
	}
	// test hook: force difficulty tier (g_neonwave_diffforce N, -2..1)
	{
		char dfBuf[8];
		trap_Cvar_VariableStringBuffer( "g_neonwave_diffforce", dfBuf, sizeof(dfBuf) );
		if ( dfBuf[0] ) {
			nw_difficulty = atoi( dfBuf );
			G_Printf( "NeonWave: dynamic difficulty forced -> %s\n", NW_DifficultyName( nw_difficulty ) );
		}
	}
	NW_ApplyMapLook();
	nw_started = qfalse;
	nw_botCounter = 0;
	nw_inBreak = qfalse;
	nw_breakEnd = 0;
	nw_waveHadBots = qfalse;
	nw_overVictory = qfalse;
	nw_bossAttr = 0;
	{
		int pi;
		int cid;
		for ( cid = 0; cid < MAX_CLIENTS; cid++ ) {
			for ( pi = 0; pi < NW_PERK_COUNT; pi++ ) {
				nw_perk[cid][pi] = 0;
			}
			nw_offer[cid][0] = nw_offer[cid][1] = nw_offer[cid][2] = 0;
		}
	}
	// v0.34 test hook: g_neonwave_perkr N grants rank-3 of perk N immediately
	// (deterministic CI check that rank-scaled effects engage). Read once here.
	{
		char prBuf[8];
		int pr;
		trap_Cvar_VariableStringBuffer( "g_neonwave_perkr", prBuf, sizeof(prBuf) );
		pr = atoi( prBuf );
		if ( pr >= 1 && pr < NW_PERK_COUNT ) {
			nw_perk[0][pr] = NW_PerkCap( pr );
			G_Printf( "NeonWave: PERK RANK FORCE %s -> rank %i\n",
				NW_PerkName( pr ), nw_perk[0][pr] );
		}
	}
	nw_waveStartTime = 0;
	trap_Cvar_Set( "ui_neonwave_offers", "" );
	trap_Cvar_Set( "ui_neonwave_owned", "" );
	trap_Cvar_Set( "ui_neonwave_picked", "0" );
	trap_Cvar_Set( "ui_neonwave_fx", "" );
	trap_Cvar_Set( "g_neonwave_upgradepoints", "0" );
	nw_fxSeq = 0;
	NW_DailyInit();
	NW_LoadRecords();
	NW_LoadAchievements();
	NW_SeasonalInit();
	if ( nw_dailyActive ) {
		NW_LoadDailyRecords();
	}
	if ( NW_DifficultyLocked() ) {
		G_Printf( "NeonWave: dynamic difficulty locked (daily=%i hardcore=%i)\n",
			nw_dailyActive ? 1 : 0, nw_hardcore ? 1 : 0 );
	}
	// Coop: upgrade points are per-client (clientPersistant_t.neonwaveUpgradePts).
	// Reset each client's pool on run start so stale state from a previous run
	// does not carry over.
	{
		int i;
		gentity_t *ent;
		for ( i = 0; i < level.maxclients; i++ ) {
			ent = &g_entities[i];
			if ( !ent->inuse || !ent->client ) continue;
			ent->client->pers.neonwaveUpgradePts = 0;
		}
	}
	// NOTE: do NOT reset g_neonwave_codex here — a +set at server start (test/dev
	// hook) would be overwritten. It is read by NeonWave_Frame for the codex toggle.
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
int i, val;
gentity_t *ent;

for ( i = 0; i < level.maxclients; i++ ) {
	ent = &g_entities[i];
	if ( !ent->inuse || !ent->client ) continue;
	if ( ent->client->pers.connected != CON_CONNECTED ) continue;
	if ( ent->r.svFlags & SVF_BOT ) continue;
	val = ent->client->pers.neonwaveUpgradePts
		| ( ( ent->client->pers.neonwaveUpHp   & 0xF ) << 8 )
		| ( ( ent->client->pers.neonwaveDmg    & 0xF ) << 12 )
		| ( ( ent->client->pers.neonwaveSpeed  & 0xF ) << 16 );
	ent->client->ps.persistant[PERS_CAPTURES] = val;
}
}

// count humans (non-bot, non-spectator, connected)
int NW_CountHumans( void ) {
int i, total = 0, standin = 0;
gentity_t *ent;
for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) continue;
		if ( ent->r.svFlags & SVF_BOT ) {
			if ( !NW_TestPlayerSkipBots() || standin ) continue;
			standin = 1;
		}
		total++;
	}
	if ( total == 0 && NW_Headless() ) {
		total = 1;
	}
	return total + NW_CoopMockExtra( qtrue );
}

// Coop test hook: simulate extra humans in headless mode (no real 2nd client).
// g_neonwave_coopmock = 1 → mock 1 extra alive human
// g_neonwave_coopmock = 2 → mock 1 extra dead human
static int NW_CoopMockExtra( qboolean alive ) {
	char buf[8];
	int mock;
	trap_Cvar_VariableStringBuffer( "g_neonwave_coopmock", buf, sizeof(buf) );
	mock = atoi( buf );
	if ( mock == 0 ) return 0;
	if ( alive && mock == 1 ) return 1;
	if ( !alive && mock == 2 ) return 1;
	return 0;
}
// Coop: wave clears when no drones remain AND at least one human is alive.
// Dead humans respawn at the next wave break (NW_EnterBreak). If ALL humans
// die, the game ends (handled separately by the humans==0 check above).
static qboolean NW_CoopWaveClear( int drones ) {
	int i, alive;
	gentity_t *ent;
	if ( drones > 0 ) return qfalse;
	alive = 0;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) continue;
		if ( ent->r.svFlags & SVF_BOT ) continue;
		if ( ent->health > 0 ) alive++;
	}
	alive += NW_CoopMockExtra( qtrue );
	if ( alive == 0 && NW_Headless() ) {
		alive = 1; // autostart stands in for a living human
	}
	return ( alive > 0 );
}

// Client slot held dead so the next wave can revive the headless stand-in.
// Dead bots are dropped once respawnTime passes (g_active.c).
static int nw_coopStandin = -1;

// g_neonwave_selfkill 1 → kill the human player each frame (test coop respawn)
static void NW_SelfKillHuman( void ) {
	int i;
	gentity_t *ent;
	char buf[8];
	trap_Cvar_VariableStringBuffer( "g_neonwave_selfkill", buf, sizeof(buf) );
	if ( atoi( buf ) != 1 ) return;
	{
		int standin = 0;
		for ( i = 0; i < level.maxclients; i++ ) {
			ent = &g_entities[i];
			if ( !ent->inuse || !ent->client ) continue;
			if ( ent->client->pers.connected != CON_CONNECTED ) continue;
			if ( ent->r.svFlags & SVF_BOT ) {
				if ( !NW_TestPlayerSkipBots() || standin ) continue;
				standin = 1;
			}
			if ( ent->health <= 0 ) continue;
			ent->health = 0;
			ent->client->ps.stats[STAT_HEALTH] = 0;
			if ( ent->r.svFlags & SVF_BOT ) {
				if ( nw_coopStandin >= 0 && nw_coopStandin != i
						&& g_entities[nw_coopStandin].client ) {
					g_entities[nw_coopStandin].client->respawnTime = level.time - 1;
				}
				ent->client->respawnTime = level.time + 600000;
				nw_coopStandin = i;
			}
		}
	}
}

static void NW_CoopRespawnDead( void ) {
	int i;
	gentity_t *ent;
	int standin = 0;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) continue;
		if ( ent->r.svFlags & SVF_BOT ) {
			if ( !NW_TestPlayerSkipBots() || standin ) continue;
			standin = 1;
		}
		if ( ent->health > 0 ) continue;
		// Respawn dead human at a random spawn point with full HP/armor
		ent->health = 100;
		ent->client->ps.stats[STAT_HEALTH] = 100;
		ent->client->ps.stats[STAT_ARMOR] = 100;
		ent->client->ps.pm_type = PM_NORMAL;
		ent->client->ps.pm_flags &= ~PMF_FOLLOW;
		trap_UnlinkEntity( ent );
		{
			vec3_t origin, angles;
			gentity_t *spawn = SelectSpawnPoint( vec3_origin, origin, angles, 0 );
			if ( spawn ) {
				VectorCopy( origin, ent->client->ps.origin );
				VectorCopy( angles, ent->client->ps.viewangles );
			}
		}
		trap_LinkEntity( ent );
		if ( i == nw_coopStandin ) {
			ent->client->respawnTime = level.time;
			nw_coopStandin = -1;
		}
		G_Printf( "NeonWave: COOP RESPAWN revived dead human\n" );
	}
}

// Move dead coop players to spectator mode so they can watch
static void NW_CoopSpectatorDead( void ) {
	int i;
	gentity_t *ent;
	
	// Only in coop mode with multiple humans
	int humans = NW_CountHumans();
	if ( humans <= 1 ) return;
	
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		if ( ent->r.svFlags & SVF_BOT ) continue;
		if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) continue;
		if ( ent->health > 0 ) continue;
		
		// Move to spectator
		ent->client->ps.pm_type = PM_SPECTATOR;
		ent->client->ps.pm_flags |= PMF_FOLLOW;
		ent->client->sess.spectatorState = SPECTATOR_FOLLOW;
		
		// Find a living human to follow
		{
			int j;
			for ( j = 0; j < level.maxclients; j++ ) {
				gentity_t *other = &g_entities[j];
				if ( !other->inuse || !other->client ) continue;
				if ( other->client->pers.connected != CON_CONNECTED ) continue;
				if ( other->r.svFlags & SVF_BOT ) continue;
				if ( other->health <= 0 ) continue;
				if ( other == ent ) continue;
				ent->client->sess.spectatorClient = j;
				break;
			}
		}
		
		G_Printf( "NeonWave: COOP SPECTATOR dead human -> following client %i\n", ent->client->sess.spectatorClient );
	}
}

static void NW_SpawnBot( int skill ) {
	if ( nw_chaosActive ) {
		// CHAOS: random skill per drone (1..max), chaotic naming
		skill = ( rand() % skill ) + 1;
		++nw_botCounter;
		G_Printf( "NeonWave: Drone W%d-%d CHAOS\n", nw_wave, nw_botCounter );
		trap_SendConsoleCommand( EXEC_APPEND,
			va("addbot sarge %i \"Drone W%d-%d CHAOS\"\n", skill, nw_wave, nw_botCounter) );
		return;
	}
	trap_SendConsoleCommand( EXEC_APPEND,
		va("addbot sarge %i \"Drone W%d-%d\"\n", skill, nw_wave, ++nw_botCounter) );
}

// v0.80: Apply arena drone scaling (hp, damage, speed) to all spawned bots
static void NW_ApplyDroneScaling( void ) {
	char buf[8];
	float hpScale = 1.0f, dmgScale = 1.0f, spdScale = 1.0f;
	int k;

	trap_Cvar_VariableStringBuffer( "g_neonwave_drone_hp_scale", buf, sizeof(buf) );
	hpScale = atof( buf );
	trap_Cvar_VariableStringBuffer( "g_neonwave_drone_damage_scale", buf, sizeof(buf) );
	dmgScale = atof( buf );
	trap_Cvar_VariableStringBuffer( "g_neonwave_drone_speed_scale", buf, sizeof(buf) );
	spdScale = atof( buf );

	// Only apply if any scale != 1.0
	if ( hpScale == 1.0f && dmgScale == 1.0f && spdScale == 1.0f ) return;

	for ( k = 0; k < level.maxclients; k++ ) {
		gentity_t *bot = &g_entities[k];
		if ( !bot->inuse || !bot->client ) continue;
		if ( !( bot->r.svFlags & SVF_BOT ) ) continue;

		if ( hpScale != 1.0f ) {
			int newMax = (int)(100 * hpScale);
			if ( newMax < 10 ) newMax = 10;
			if ( newMax > 500 ) newMax = 500;
			bot->client->pers.maxHealth = newMax;
			bot->client->ps.stats[STAT_MAX_HEALTH] = newMax;
			bot->health = newMax;
			bot->client->ps.stats[STAT_HEALTH] = newMax;
		}
		if ( dmgScale != 1.0f ) {
			// Convert float scale to neonwaveDmg level (0-7)
			// 1.0 = 0, 1.1 = 1, 1.2 = 2, etc.
			int dmgLevel = (int)((dmgScale - 1.0f) * 10);
			if ( dmgLevel < 0 ) dmgLevel = 0;
			if ( dmgLevel > 7 ) dmgLevel = 7;
			bot->client->pers.neonwaveDmg = dmgLevel;
		}
		if ( spdScale != 1.0f ) {
			// Convert float scale to neonwaveSpeed level (0-7)
			// 1.0 = 0, 1.1 = 1, 1.2 = 2, etc.
			int spdLevel = (int)((spdScale - 1.0f) * 10);
			if ( spdLevel < 0 ) spdLevel = 0;
			if ( spdLevel > 7 ) spdLevel = 7;
			bot->client->pers.neonwaveSpeed = spdLevel;
		}
	}
}

// Batch-spawn multiple bots in a single console command for performance
// Format: "addbot sarge SKILL \"NAME\"; addbot sarge SKILL \"NAME\"; ..."
static void NW_SpawnBotsBatch( int skill, int count ) {
	char cmd[1024];
	int cmd_len = 0;
	int i;

	if ( count <= 0 ) return;

	// For small counts, use individual commands (simpler)
	if ( count <= 3 ) {
		for ( i = 0; i < count; i++ ) {
			NW_SpawnBot( skill );
		}
		return;
	}

	// Batch mode: build a single console command string
	// Console command limit is ~1024 chars, so batch in chunks
	for ( i = 0; i < count; i++ ) {
		char bot_cmd[128];
		// v0.80: Bot type — force via g_neonwave_forcebot for deterministic tests
		int botType = NW_BOT_NONE;
		const char* botSuffix = "";
		{
			char fbuf[8];
			int fb;
			trap_Cvar_VariableStringBuffer( "g_neonwave_forcebot", fbuf, sizeof(fbuf) );
			fb = atoi( fbuf );
			if ( fb >= 1 && fb <= 3 ) {
				botType = fb;
				if ( fb == 1 ) botSuffix = " HEALER";
				else if ( fb == 2 ) botSuffix = " SHIELD";
				else if ( fb == 3 ) botSuffix = " ELITE";
			}
		}
		if ( botType == 0 ) {
		if ( nw_wave >= 8 && ( rand() % 100 ) < NW_HEALER_RATE ) {
			botType = NW_BOT_HEALER;
			botSuffix = " HEALER";
		} else if ( nw_wave >= 10 && ( rand() % 100 ) < NW_SHIELDER_RATE ) {
			botType = NW_BOT_SHIELDER;
			botSuffix = " SHIELD";
		} else if ( nw_wave >= 12 && ( rand() % 100 ) < NW_ELITE_RATE ) {
			botType = NW_BOT_ELITE;
			botSuffix = " ELITE";
		}
		} // end if ( botType == 0 ) force-override
		if ( nw_chaosActive ) {
			int s = ( rand() % skill ) + 1;
			++nw_botCounter;
			G_Printf( "NeonWave: Drone W%d-%d CHAOS%s\n", nw_wave, nw_botCounter, botSuffix );
			Com_sprintf( bot_cmd, sizeof(bot_cmd),
				"addbot sarge %i \"Drone W%d-%d CHAOS%s\"; ",
				s, nw_wave, nw_botCounter, botSuffix );
		} else {
			++nw_botCounter;
			G_Printf( "NeonWave: Drone W%d-%d%s\n", nw_wave, nw_botCounter, botSuffix );
			Com_sprintf( bot_cmd, sizeof(bot_cmd),
				"addbot sarge %i \"Drone W%d-%d%s\"; ",
				skill, nw_wave, nw_botCounter, botSuffix );
		}
		if ( cmd_len + strlen(bot_cmd) >= sizeof(cmd) - 1 ) {
			// Flush current batch
			cmd[cmd_len] = '\0';
			trap_SendConsoleCommand( EXEC_APPEND, cmd );
			cmd_len = 0;
		}
		// Append bot_cmd to cmd buffer
		{
			int j;
			for ( j = 0; bot_cmd[j] && cmd_len < (int)sizeof(cmd) - 1; j++ ) {
				cmd[cmd_len++] = bot_cmd[j];
			}
		}
	}
	// Flush remaining
	if ( cmd_len > 0 ) {
		cmd[cmd_len] = '\0';
		trap_SendConsoleCommand( EXEC_APPEND, cmd );
	}
}

// boss types: rotate per boss wave, forceable via g_neonwave_bosstype for tests
#define NW_BOSS_SNIPER	1	// railgun sniper (classic)
#define NW_BOSS_TANK	2	// slow, huge HP, chaingun-style MG spam
#define NW_BOSS_SWARM	3	// spawns mini-drones during the wave
#define NW_BOSS_GLASS	4	// glass cannon: fast, 2x HP, railgun
#define NW_BOSS_WARDEN		5	// v0.15: teleport-strikes the player's zone + brief armor phase
#define NW_BOSS_BERSERKER	6	// v0.40: slow, huge HP, MG spam — enrages below 30% HP
#define NW_BOSS_TELEPORTER	7	// v0.41: teleports away on hit, evasive boss
#define NW_BOSS_HEALER		8	// v0.70: heals nearby bots, low HP, no direct attack
#define NW_BOSS_SHIELDER	9	// v1.0: deploys energy shield vs projectiles
#define NW_BOSS_SNIPELITE	10	// v1.0: fast sniper, double rail + cloak
#define NW_BOSS_DEMOLISHER	11	// v1.0: rocket spammer, splash damage
#define NW_BOSS_CHRONOMANCER	12	// v1.0: time manipulation, teleport, slow-mo
#define NW_BOSS_VOIDWALKER	13	// v1.0: phase-shifting, invulnerability, enrage

static int NW_PickBossType( void ) {
	char btBuf[8];
	int forced;

	// test hook: g_neonwave_bosstype N forces the type
	trap_Cvar_VariableStringBuffer( "g_neonwave_bosstype", btBuf, sizeof(btBuf) );
	forced = atoi( btBuf );
	if ( forced >= NW_BOSS_SNIPER && forced <= NW_BOSS_VOIDWALKER ) {
		return forced;
	}
	// one step per boss wave so a classic 20-wave run sees all types:
	// wave 10 SNIPER, 11 TANK, 12 SWARM MOTHER, 13 GLASS CANNON, 14 WARDEN,
	// 15 BERSERKER, 16 TELEPORTER, 17 HEALER, 18 SHIELDER, 19 SNIPELITE, 20 DEMOLISHER
	// daily challenge shifts the rotation start
	{
		int rot = ( nw_wave - NW_BOSS_WAVE ) % NW_BOSS_COUNT;
		if ( rot < 0 ) {
			rot += NW_BOSS_COUNT;
		}
		if ( nw_dailyActive ) {
			rot = ( rot + nw_dailyBossOffset ) % NW_BOSS_COUNT;
		}
		return NW_BOSS_SNIPER + rot;
	}
}

static const char *NW_BossName( int type ) {
	switch ( type ) {
	case NW_BOSS_TANK:		return "TANK";
	case NW_BOSS_SWARM:		return "SWARM MOTHER";
	case NW_BOSS_GLASS:		return "GLASS CANNON";
	case NW_BOSS_WARDEN:	return "WARDEN";
	case NW_BOSS_BERSERKER:	return "BERSERKER";
	case NW_BOSS_TELEPORTER: return "TELEPORTER";
	case NW_BOSS_HEALER:	return "HEALER";
	case NW_BOSS_SHIELDER:	return "SHIELDER";
	case NW_BOSS_SNIPELITE:	return "SNIPER ELITE";
	case NW_BOSS_DEMOLISHER: return "DEMOLISHER";
	case NW_BOSS_CHRONOMANCER: return "CHRONOMANCER";
	case NW_BOSS_VOIDWALKER: return "VOID WALKER";
	default:				return "SNIPER";
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
	if ( type == NW_BOSS_BERSERKER ) {
		hc = 700; // 7x — berserk: massive HP, enrages
	}
	if ( type == NW_BOSS_TELEPORTER ) {
		hc = 350; // 3.5x — teleporter: evasive, moderate HP
	}
	if ( type == NW_BOSS_HEALER ) {
		hc = 300; // 3x — healer: fragile, heals nearby bots
	}
	if ( type == NW_BOSS_CHRONOMANCER ) {
		hc = 450; // 4.5x — chronomancer: time manipulation, moderate-high HP
	}
	if ( type == NW_BOSS_VOIDWALKER ) {
		hc = 550; // 5.5x — void walker: phase-shifting tank
	}
	// wave scaling: bosses get +20% HP per wave past boss wave 10
	if ( nw_wave > NW_BOSS_WAVE ) {
		hc = hc * ( 100 + ( nw_wave - NW_BOSS_WAVE ) * 20 ) / 100;
	}
	// dynamic difficulty (v0.16): scale boss HP with player performance
	if ( nw_difficulty != 0 ) {
		hc = hc * ( 100 + nw_difficulty * 15 ) / 100;
	}
	// hardcore mode: +50% boss HP
	if ( nw_hardcore ) {
		hc = hc * 3 / 2;
	}
	nw_bossType = type;
	NW_InvalidateBossCache();
	G_Printf( "NeonWave: boss spawned: %s (hc %i)\n", NW_BossName( type ), hc );
	if ( type == NW_BOSS_BERSERKER ) {
		char rfBuf[8];
		trap_Cvar_VariableStringBuffer( "g_neonwave_rageforce", rfBuf, sizeof(rfBuf) );
		if ( atoi( rfBuf ) == 1 ) {
			nw_bossPhase = 2;
			G_Printf( "NeonWave: BERSERKER ENTERS RAGE\n" );
		}
	}
	{
		char pfBuf[8];
		trap_Cvar_VariableStringBuffer( "g_neonwave_phaseforce", pfBuf, sizeof(pfBuf) );
		if ( atoi( pfBuf ) == 1 ) {
			nw_bossPhase = 2;
			G_Printf( "NeonWave: %s ENTERS PHASE 2\n", NW_BossName( type ) );
		}
	}
	trap_Cvar_Set( "g_neonwave_nextboss", "1" );
	trap_Cvar_Set( "g_neonwave_bosshc", va("%i", hc) );
	trap_SendServerCommand( -1, va( "cp \"BOSS: %s\\n\"", NW_BossName( type ) ) );
	trap_SendConsoleCommand( EXEC_APPEND,
		va("addbot sarge 5 \"BOSS W%d %s\"\n", nw_wave, NW_BossName( type )) );
}

static int NW_PointsForClient( gentity_t *ent ) {
	if ( !ent || !ent->client ) {
		return 0;
	}
	return ent->client->pers.neonwaveUpgradePts;
}

// Compatibility helper: points of the first connected human client (for
// CS_NEONWAVE payload and legacy cvar readers). Under coop this is NOT the
// sum of all clients — it is the first one, so the payload stays a single
// number and the cgame reads its own client->pers instead.
// Uses the cached scan result (NW_Cache).
static int NW_PointsBroadcast( void ) {
	return NW_Cache()->points;
}

// ---- legacy cvar mirror (compatibility only): kept in sync with the
// first connected human client so tools/debug that read the cvar still work.
static void NW_MirrorPointsCvar( void ) {
	trap_Cvar_Set( "g_neonwave_upgradepoints", va( "%i", NW_PointsBroadcast() ) );
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

// Headless CI: g_neonwave_autostart 1 runs the wave loop with no human client.
qboolean NW_Headless( void ) {
	char buf[8];
	trap_Cvar_VariableStringBuffer( "g_neonwave_autostart", buf, sizeof(buf) );
	return atoi( buf ) != 0;
}

void NeonWave_TrackRunCombo( int combo ) {
	if ( combo > nw_runBestCombo ) {
		nw_runBestCombo = combo;
	}
}

static void NW_SendStatus( int event ) {
	int breakMs;
	const nwCache_t *cache;

	// Performance: skip if nothing changed since last update
	cache = NW_Cache();
	if ( nw_lastStatusWave == nw_wave &&
	     nw_lastStatusHp == cache->bossHp &&
	     nw_lastStatusPts == cache->points &&
	     nw_event == event ) {
		return;
	}
	nw_lastStatusWave = nw_wave;
	nw_lastStatusHp = cache->bossHp;
	nw_lastStatusPts = cache->points;

	nw_event = event;

	breakMs = 0;
	if ( nw_inBreak && nw_breakEnd > level.time ) {
		breakMs = nw_breakEnd - level.time;
	}
	// payload: "<wave> <ev> <bhp> <bmax> <brk> <pts> <best> <mod> <kills> <bestcombo> <runsec> <livecombo> <bosstype>"
	trap_SetConfigstring( CS_NEONWAVE, va( "%i %i %i %i %i %i %i %i %i %i %i %i %i",
		nw_wave, event, cache->bossHp, cache->bossMax, breakMs, cache->points, NW_Best(), nw_modifier,
		cache->kills, cache->bestCombo, ( level.time - nw_runStartTime ) / 1000,
		cache->currentCombo, ( cache->bossHp > 0 ) ? nw_bossType : 0 ) );
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

static void NeonWave_LogPayload( void ) {
// explizites Loggen der CS_NEONWAVE-Payload für strukturiertes Parsen in Tests
// Format: <wave> <event> <bossHp> <bossMax> <breakMs> <pts> <best> <mod> <kills> <bestCombo> <runSec> <liveCombo> <bossType>
char buf[256];
trap_GetConfigstring( CS_NEONWAVE, buf, sizeof(buf) );
G_Printf( "NEONWAVE_PAYLOAD: %s\n", buf );
}

static void NeonWave_UpdateHighscore( void ) {
	int best = NW_Best();
	if ( nw_wave > best ) {
		trap_Cvar_Set( "g_neonwave_best", va("%i", nw_wave) );
		G_Printf( "NeonWave: NEW BEST wave %i (was %i)\n", nw_wave, best );
	}
}

// ---- break-window perks (v0.28) ----
static const char *NW_PerkName( int id ) {
	switch ( id ) {
	case NW_PERK_PIERCE:		return "PIERCE";
	case NW_PERK_CHAIN:		return "CHAIN";
	case NW_PERK_DASH:		return "DASH";
	case NW_PERK_OVERCHARGE:	return "OVERCHARGE";
	case NW_PERK_SECONDWIND:	return "SECOND WIND";
	case NW_PERK_SKIP:		return "SKIP";
	default:			return "";
	}
}

static int NW_PerkCap( int id ) {
	// PIERCE/CHAIN/OVERCHARGE/DASH scale with rank (v0.34): up to rank 3.
	if ( id == NW_PERK_PIERCE || id == NW_PERK_CHAIN
		|| id == NW_PERK_OVERCHARGE || id == NW_PERK_DASH ) {
		return 3;
	}
	return 1; // SKIP / SECOND WIND are consumables
}

int NeonWave_PerkLevel( int clientID, int perk ) {
	if ( perk <= 0 || perk >= NW_PERK_COUNT ) {
		return 0;
	}
	if ( clientID < 0 || clientID >= MAX_CLIENTS ) {
		return 0;
	}
	return nw_perk[ clientID ][ perk ];
}

int NeonWave_WaveStartTime( void ) {
	return nw_waveStartTime;
}

void NeonWave_PerkFx( const char *kind ) {
	if ( !kind || !kind[0] ) {
		return;
	}
	nw_fxSeq++;
	trap_Cvar_Set( "ui_neonwave_fx", va( "%s-%i", kind, nw_fxSeq ) );
}

void NW_MirrorPerks( int clientID ) {
	char offers[96];
	char owned[96];
	int i, first;

	if ( clientID < 0 || clientID >= MAX_CLIENTS ) {
		return;
	}
	Com_sprintf( offers, sizeof( offers ), "%s|%s|%s",
		NW_PerkName( nw_offer[clientID][0] ),
		NW_PerkName( nw_offer[clientID][1] ),
		NW_PerkName( nw_offer[clientID][2] ) );
	trap_Cvar_Set( va( "ui_neonwave_offers_%i", clientID ), offers );

	// Build the owned string with Com_sprintf at the current offset instead of
	// repeated Q_strcat (which scans from the start each time). We track the
	// current length manually.
	owned[0] = '\0';
	first = 1;
	for ( i = 1; i < NW_PERK_COUNT; i++ ) {
		int len = (int)strlen( owned );
		if ( nw_perk[clientID][i] <= 0 ) {
			continue;
		}
		if ( !first ) {
			Com_sprintf( owned + len, sizeof( owned ) - len, ", " );
			len = (int)strlen( owned );
		}
		first = 0;
		if ( nw_perk[clientID][i] > 1 ) {
			Com_sprintf( owned + len, sizeof( owned ) - len, "%s x%i", NW_PerkName( i ), nw_perk[clientID][i] );
		} else {
			Com_sprintf( owned + len, sizeof( owned ) - len, "%s", NW_PerkName( i ) );
		}
	}
	trap_Cvar_Set( va( "ui_neonwave_owned_%i", clientID ), owned );
}

static void NW_RollOffers( int clientID ) {
	int eligible[ NW_PERK_COUNT ];
	int n = 0, i, slot, idx;
	char force[32];
	int a = 0, b = 0, c = 0;

	if ( clientID < 0 || clientID >= MAX_CLIENTS ) {
		return;
	}
	nw_offer[clientID][0] = nw_offer[clientID][1] = nw_offer[clientID][2] = 0;
	trap_Cvar_Set( va( "ui_neonwave_picked_%i", clientID ), "0" );

	trap_Cvar_VariableStringBuffer( "g_neonwave_perkforce", force, sizeof( force ) );
	// packed 3 digits (123 = PIERCE CHAIN DASH) — ioq3 +set cannot pass commas
	{
		int packed = atoi( force );
		if ( packed >= 111 && packed <= 666 ) {
			a = packed / 100;
			b = ( packed / 10 ) % 10;
			c = packed % 10;
		} else if ( force[0] ) {
			sscanf( force, "%i-%i-%i", &a, &b, &c );
		}
	}
	if ( a >= 1 && a < NW_PERK_COUNT ) {
		nw_offer[clientID][0] = a;
		nw_offer[clientID][1] = b;
		nw_offer[clientID][2] = c;
	} else {
		for ( i = 1; i < NW_PERK_COUNT; i++ ) {
			/* Rail-only Ghost: CHAIN is lightning and a dead card. perkforce still overrides. */
			if ( NW_GhostActive() && i == NW_PERK_CHAIN ) {
				continue;
			}
			if ( nw_perk[clientID][i] < NW_PerkCap( i ) ) {
				eligible[n++] = i;
			}
		}
		for ( slot = 0; slot < 3 && n > 0; slot++ ) {
			idx = ( nw_wave * 13 + slot * 7 + nw_dailyOffset ) % n;
			if ( idx < 0 ) {
				idx += n;
			}
			nw_offer[clientID][slot] = eligible[idx];
			eligible[idx] = eligible[--n];
		}
	}
	G_Printf( "NeonWave: PERK OFFER F1=%s F2=%s F3=%s\n",
		NW_PerkName( nw_offer[clientID][0] ),
		NW_PerkName( nw_offer[clientID][1] ),
		NW_PerkName( nw_offer[clientID][2] ) );
	NW_MirrorPerks( clientID );
}

qboolean NeonWave_BuyOffer( gentity_t *ent, int slot ) {
	int id, pts, cap;
	int clientID;

	if ( !NeonWave_IsBreak() ) {
		return qfalse;
	}
	if ( slot < 1 || slot > 3 ) {
		return qfalse;
	}
	if ( !ent || !ent->client ) {
		return qfalse;
	}
	clientID = ent - g_entities;
	if ( clientID < 0 || clientID >= MAX_CLIENTS ) {
		return qfalse;
	}
	id = nw_offer[clientID][ slot - 1 ];
	if ( id < 1 || id >= NW_PERK_COUNT ) {
		return qfalse;
	}
	pts = NW_PointsForClient(ent);
	if ( pts < 1 ) {
		return qfalse;
	}
	cap = NW_PerkCap( id );
	if ( nw_perk[clientID][id] >= cap ) {
		return qfalse;
	}
	nw_perk[clientID][id]++;
	pts--;
	ent->client->pers.neonwaveUpgradePts = pts;
	NW_MirrorPointsCvar();
	nw_offer[clientID][ slot - 1 ] = 0;
	trap_Cvar_Set( va( "ui_neonwave_picked_%i", clientID ), va( "%i", slot ) );
	G_Printf( "NeonWave: PERK TAKEN %s (rank %i, %i pts left)\n",
		NW_PerkName( id ), nw_perk[clientID][id], pts );
	trap_SendServerCommand( ent - g_entities,
		va( "cp \"PERK: %s\\n\"", NW_PerkName( id ) ) );
	NW_MirrorPerks( clientID );
	NeonWave_RefreshStatus();
	return qtrue;
}

qboolean NeonWave_TrySecondWind( gentity_t *ent ) {
	int clientID;
	if ( !ent || !ent->client ) {
		return qfalse;
	}
	if ( g_gametype.integer != GT_NEONWAVE ) {
		return qfalse;
	}
	clientID = ent - g_entities;
	if ( clientID < 0 || clientID >= MAX_CLIENTS ) {
		return qfalse;
	}
	if ( nw_perk[clientID][ NW_PERK_SECONDWIND ] <= 0 ) {
		return qfalse;
	}
	nw_perk[clientID][ NW_PERK_SECONDWIND ]--;
	ent->health = 40;
	ent->client->ps.stats[ STAT_HEALTH ] = 40;
	ent->client->ps.pm_type = PM_NORMAL;
	G_Printf( "NeonWave: SECOND WIND saved the player\n" );
	trap_SendServerCommand( -1, "cp \"SECOND WIND\\n\"" );
	NeonWave_PerkFx( "secondwind" );
	NW_MirrorPerks( clientID );
	return qtrue;
}

void NeonWave_LightningChain( gentity_t *attacker, gentity_t *primary, int damage ) {
	int hops, h, i;
	gentity_t *from, *best, *ent;
	float bestDist, dist;
	vec3_t delta;

	if ( g_gametype.integer != GT_NEONWAVE ) {
		return;
	}
	hops = NeonWave_PerkLevel( attacker - g_entities, NW_PERK_CHAIN );
	if ( hops < 1 || !attacker || !primary ) {
		return;
	}
	from = primary;
	for ( h = 0; h < hops; h++ ) {
		best = NULL;
		bestDist = 320.0f;
		for ( i = 0; i < level.maxclients; i++ ) {
			ent = &g_entities[i];
			if ( !ent->inuse || !ent->client ) {
				continue;
			}
			if ( ent == from || ent == primary || ent == attacker ) {
				continue;
			}
			if ( !( ent->r.svFlags & SVF_BOT ) ) {
				continue;
			}
			if ( ent->health <= 0 ) {
				continue;
			}
			VectorSubtract( ent->r.currentOrigin, from->r.currentOrigin, delta );
			dist = VectorLength( delta );
			if ( dist < bestDist ) {
				bestDist = dist;
				best = ent;
			}
		}
		if ( !best ) {
			break;
		}
		VectorSubtract( best->r.currentOrigin, from->r.currentOrigin, delta );
		G_Damage( best, attacker, attacker, delta, best->r.currentOrigin,
			damage * 3 / 4, 0, MOD_LIGHTNING );
		{
			gentity_t *bolt;
			vec3_t start, end;
			VectorCopy( from->r.currentOrigin, start );
			VectorCopy( best->r.currentOrigin, end );
			start[2] += 24;
			end[2] += 24;
			bolt = G_TempEntity( end, EV_LIGHTNINGBOLT );
			VectorCopy( start, bolt->s.origin2 );
			SnapVector( bolt->s.origin2 );
		}
		G_Printf( "NeonWave: LG CHAIN hop %i\n", h + 1 );
		from = best;
	}
}

static void NW_Autopick( void ) {
	char buf[8];
	int slot;
	int clientID;
	gentity_t *ent;

	trap_Cvar_VariableStringBuffer( "g_neonwave_autopick", buf, sizeof( buf ) );
	slot = atoi( buf );
	if ( slot < 1 || slot > 3 ) {
		return;
	}
	trap_Cvar_Set( "g_neonwave_autopick", "0" );
	// Coop: autopick applies to all connected human clients
	{
		int picked = 0;
		for ( clientID = 0; clientID < MAX_CLIENTS; clientID++ ) {
			ent = &g_entities[clientID];
			if ( !ent->inuse || !ent->client ) continue;
			if ( ent->client->pers.connected != CON_CONNECTED ) continue;
			if ( ent->r.svFlags & SVF_BOT && !NW_TestPlayerSkipBots() ) continue;
			if ( NeonWave_BuyOffer( ent, slot ) ) {
				picked++;
			}
		}
		if ( picked == 0 ) {
			int id = nw_offer[0][ slot - 1 ];
			if ( id >= 1 && id < NW_PERK_COUNT ) {
				nw_perk[0][id]++;
				nw_offer[0][ slot - 1 ] = 0;
				G_Printf( "NeonWave: PERK TAKEN %s (rank %i, %i pts left)\n",
					NW_PerkName( id ), nw_perk[0][id], 0 );
			}
		}
	}
}

void NeonWave_StartWave( int num ) {
	int i;
	int skill = 1 + num / 3;
	int botCount;
	char mwBuf[8];
	int maxWave;

	NW_PickModifier( num );
	if ( nw_modifier != NW_MOD_NONE ) {
		nw_modifiersSeen |= ( 1 << nw_modifier );
	}
	if ( skill > 5 ) skill = 5;
	nw_wave = num;
	nw_botCounter = 0;
	nw_inBreak = qfalse;
	nw_chaosActive = qfalse;
	NW_CoopRespawnDead(); // respawn dead humans at wave start (coop)
	nw_waveHadBots = qfalse;
	nw_bossType = 0;
	nw_bossPhase = 1;
	NW_InvalidateBossCache();

	// endless mode: past the classic max wave, keep scaling difficulty
	// Cap at 15 bots to prevent performance issues in very late waves
	trap_Cvar_VariableStringBuffer( "g_neonwave_maxwave", mwBuf, sizeof(mwBuf) );
	maxWave = atoi( mwBuf );
	if ( maxWave <= 0 ) {
		maxWave = NW_MAX_WAVE;
	}
	if ( num > maxWave ) {
		// +2 bots every 3 waves past max, capped at 15 for performance
		botCount = maxWave + 1 + ((num - maxWave) / 3) * 2;
		if ( botCount > 15 ) botCount = 15;
	} else {
		botCount = num + 1;
	}
	if ( nw_hardcore ) {
		// hardcore: denser waves (+2 drones) and one notch harder skill
		botCount += 2;
		if ( skill < 5 ) skill += 1;
	}

	// v0.80: arena drone count scaling
	{
		char scaleBuf[8];
		float countScale = 1.0f;
		trap_Cvar_VariableStringBuffer( "g_neonwave_drone_count_scale", scaleBuf, sizeof(scaleBuf) );
		countScale = atof( scaleBuf );
		if ( countScale < 0.1f ) countScale = 0.1f;
		if ( countScale > 3.0f ) countScale = 3.0f;
		if ( countScale != 1.0f ) {
			int oldCount = botCount;
			botCount = (int)(botCount * countScale + 0.5f);
			if ( botCount < 1 ) botCount = 1;
			if ( botCount > 20 ) botCount = 20;
			G_Printf( "NeonWave: arena drone count scale %.2f: %d -> %d\n", countScale, oldCount, botCount );
		}
	}

	// v0.80: arena scaling summary (logged once per wave for test assertions)
	{
		char buf[8];
		float hpScale = 1.0f, dmgScale = 1.0f, spdScale = 1.0f;
		trap_Cvar_VariableStringBuffer( "g_neonwave_drone_hp_scale", buf, sizeof(buf) );
		hpScale = atof( buf );
		trap_Cvar_VariableStringBuffer( "g_neonwave_drone_damage_scale", buf, sizeof(buf) );
		dmgScale = atof( buf );
		trap_Cvar_VariableStringBuffer( "g_neonwave_drone_speed_scale", buf, sizeof(buf) );
		spdScale = atof( buf );
		if ( hpScale != 1.0f || dmgScale != 1.0f || spdScale != 1.0f ) {
			G_Printf( "NeonWave: arena drone scaling hp=%.2f dmg=%.2f spd=%.2f\n", hpScale, dmgScale, spdScale );
		}
	}
	// coop scaling: more humans = more drones + optional difficulty notch
	{
		int humans = NW_CountHumans();
		if ( humans > 1 ) {
			char cdBuf[8];
			int cd = 0;
			trap_Cvar_VariableStringBuffer( "g_neonwave_coopdifficulty", cdBuf, sizeof(cdBuf) );
			cd = atoi( cdBuf );
			// +1 drone per additional human (beyond the first)
			botCount += ( humans - 1 );
			if ( cd >= 2 ) {
				// normal/hard: +1 more drone per human
				botCount += ( humans - 1 );
			}
			if ( cd >= 3 ) {
				// hard: one notch harder skill
				if ( skill < 5 ) skill += 1;
			}
			G_Printf( "NeonWave: COOP scale %i humans, difficulty %i -> %i drones, skill %i\n",
				humans, cd, botCount, skill );
		}
	}

	// apply modifier side effects (slot 1 AND slot 2 both apply), then
	// named-pair synergy nudges so the second slot is actually felt.
	{
		int grav = 800, speed = 320, qf = 3, armor = 0, vampheal = 0, mirrordiv = 3;
		if ( NW_ModActive( NW_MOD_LOWGRAV ) ) {
			grav = 400; // half of default 800
		}
		if ( NW_ModActive( NW_MOD_TIMEWARP ) ) {
			speed = 520; // ~1.6x of default 320
		}
		if ( NW_ModActive( NW_MOD_FROST ) ) {
			speed = 220; // slowed player (frost effect)
			G_Printf( "NeonWave: FROST slowed to %i\n", speed );
		}
		if ( NW_ModActive( NW_MOD_FRENZY ) ) {
			qf = 4;
		}
		if ( NW_ModActive( NW_MOD_OVERSHIELD ) ) {
			armor = 50;
		}
		if ( NW_ModActive( NW_MOD_SHIELD ) ) {
			// Temporary invulnerability at wave start
			trap_Cvar_Set( "g_neonwave_shieldactive", "1" );
			trap_Cvar_Set( "g_neonwave_shieldtime", "3000" ); // 3 seconds
		}
		if ( NW_ModActive( NW_MOD_VAMPIRE ) ) {
			vampheal = 4;
		}
		if ( NW_ModActive( NW_MOD_MIRROR ) ) {
			mirrordiv = 3;
		}
		nw_ptsMul = 1;
		if ( NW_ModActive( NW_MOD_DOUBLEPTS ) ) {
			nw_ptsMul = 2;
		}
		if ( NW_ModActive( NW_MOD_SURGE ) ) {
			nw_ptsMul *= 3;
		}
		// v0.37 named-pair nudges (indices match nwSynergies[])
		// v0.80: arena gravity scaling (applied after modifier/synergy calculations)
	{
		char gravBuf[8];
		float gravScale = 1.0f;
		trap_Cvar_VariableStringBuffer( "g_neonwave_gravity_scale", gravBuf, sizeof(gravBuf) );
		gravScale = atof( gravBuf );
		if ( gravScale < 0.1f ) gravScale = 0.1f;
		if ( gravScale > 2.0f ) gravScale = 2.0f;
		if ( gravScale != 1.0f ) {
			grav = (int)(grav * gravScale);
			if ( grav < 100 ) grav = 100;
			if ( grav > 1600 ) grav = 1600;
		}
	}

	if ( nw_synergyIdx == 0 ) { // AERIAL ASSAULT: lower grav, x3 points
			grav = 280;
			nw_ptsMul = 3;
			G_Printf( "NeonWave: SYNERGY EFFECT AERIAL ASSAULT: gravity 280, points x3\n" );
		} else if ( nw_synergyIdx == 1 ) { // BLOOD WELL: double lifesteal
			vampheal = 8;
			G_Printf( "NeonWave: SYNERGY EFFECT BLOOD WELL: lifesteal 8\n" );
		} else if ( nw_synergyIdx == 2 ) { // OVERDRIVE: harder hits
			qf = 6;
			G_Printf( "NeonWave: SYNERGY EFFECT OVERDRIVE: quadfactor 6\n" );
		} else if ( nw_synergyIdx == 3 ) { // HIVE MIRROR: stronger reflect
			mirrordiv = 2;
			G_Printf( "NeonWave: SYNERGY EFFECT HIVE MIRROR: reflect 1/2\n" );
		} else if ( nw_synergyIdx == 4 ) { // SHIELD BLEED: weaker defense + heal
			armor = 25;
			vampheal = 2;
			G_Printf( "NeonWave: ANTI-SYNERGY EFFECT SHIELD BLEED: overshield 25, lifesteal 2\n" );
		} else if ( nw_synergyIdx == 5 ) { // DRIFT LOCK: clamped movement
			grav = 600;
			speed = 400;
			G_Printf( "NeonWave: ANTI-SYNERGY EFFECT DRIFT LOCK: speed 400, gravity 600\n" );
		}
		trap_Cvar_Set( "g_gravity", va( "%i", grav ) );
		trap_Cvar_Set( "g_speed", va( "%i", speed ) );
		if ( speed != 320 ) {
			G_Printf( "g_speed changed to %i\n", speed );
		}
		trap_Cvar_Set( "g_quadfactor", va( "%i", qf ) );
		trap_Cvar_Set( "g_neonwave_vampheal", va( "%i", vampheal ) );
		trap_Cvar_Set( "g_neonwave_mirrordiv", va( "%i", mirrordiv ) );
		if ( NW_ModActive( NW_MOD_FRENZY ) ) {
			G_Printf( "NeonWave: FRENZY quadfactor set to %i\n", qf );
		}
		if ( armor > 0 ) {
			int k;
			for ( k = 0; k < level.maxclients; k++ ) {
				gentity_t *p = &g_entities[k];
				if ( !p->inuse || !p->client ) continue;
				if ( p->client->pers.connected != CON_CONNECTED ) continue;
				if ( p->client->sess.sessionTeam == TEAM_SPECTATOR ) continue;
				if ( p->r.svFlags & SVF_BOT && !NW_TestPlayerSkipBots() ) continue;
				p->client->ps.stats[STAT_ARMOR] += armor;
			}
			G_Printf( "NeonWave: OVERSHIELD +%i armor granted\n", armor );
		}
	}
	// REGEN: top up player health at the start of each wave (sustained push)
	if ( NW_ModActive( NW_MOD_REGEN ) ) {
		int k;
		int maxh;
		for ( k = 0; k < level.maxclients; k++ ) {
			gentity_t *p = &g_entities[k];
			if ( !p->inuse || !p->client ) continue;
			if ( p->client->pers.connected != CON_CONNECTED ) continue;
			if ( p->client->sess.sessionTeam == TEAM_SPECTATOR ) continue;
			if ( p->r.svFlags & SVF_BOT && !NW_TestPlayerSkipBots() ) continue;
			maxh = p->client->ps.stats[STAT_MAX_HEALTH];
			if ( p->health < maxh ) {
				p->health = maxh;
				p->client->ps.stats[STAT_HEALTH] = maxh;
			}
		}
		G_Printf( "NeonWave: REGEN health topped up\n" );
	}
	// SURGE: tougher drones this wave (one notch harder skill); points handled in NW_GrantUpgradePoints
	if ( NW_ModActive( NW_MOD_SURGE ) ) {
		if ( skill < 5 ) skill += 1;
		G_Printf( "NeonWave: SURGE drones hardened\n" );
	}
	// FROST: slowed player, frosty drones
	if ( NW_ModActive( NW_MOD_FROST ) ) {
	}
	// CHAOS: chaotic spawns (random skill per drone)
	if ( NW_ModActive( NW_MOD_CHAOS ) ) {
		nw_chaosActive = qtrue;

		G_Printf( "NeonWave: CHAOS mode — random skill per drone\n" );
	}
	if ( NW_ModActive( NW_MOD_GLASS ) && skill < 4 ) {
		skill += 1; // glass drones are fast/aggressive
	}
	if ( NW_ModActive( NW_MOD_SWARM ) ) {
		botCount *= 2;
	}
	// MIMIC: drones copy a random upgrade value from a random human player
	if ( NW_ModActive( NW_MOD_MIMIC ) ) {
		int humanCount = 0;
		int humans[MAX_CLIENTS];
		gentity_t *src = NULL;
		int mimicStat;
		int mimicValue = 0;
		int k;

		// find all human players
		for ( k = 0; k < level.maxclients; k++ ) {
			gentity_t *p = &g_entities[k];
			if ( !p->inuse || !p->client ) continue;
			if ( p->client->pers.connected != CON_CONNECTED ) continue;
			if ( p->client->sess.sessionTeam == TEAM_SPECTATOR ) continue;
			if ( p->r.svFlags & SVF_BOT && !NW_TestPlayerSkipBots() ) continue;
			humans[humanCount++] = k;
		}
		if ( humanCount > 0 ) {
			src = &g_entities[humans[rand() % humanCount]];
			// pick a random stat: 0 = HP, 1 = DMG, 2 = Speed
			mimicStat = rand() % 3;
			if ( mimicStat == 0 ) {
				mimicValue = src->client->pers.neonwaveUpHp;
				G_Printf( "NeonWave: MIMIC copies HP level %i from %s\n",
					mimicValue, src->client->pers.netname );
			} else if ( mimicStat == 1 ) {
				mimicValue = src->client->pers.neonwaveDmg;
				G_Printf( "NeonWave: MIMIC copies DMG level %i from %s\n",
					mimicValue, src->client->pers.netname );
			} else {
				mimicValue = src->client->pers.neonwaveSpeed;
				G_Printf( "NeonWave: MIMIC copies SPEED level %i from %s\n",
					mimicValue, src->client->pers.netname );
			}
			// apply to all bots
			for ( k = 0; k < level.maxclients; k++ ) {
				gentity_t *bot = &g_entities[k];
				if ( !bot->inuse || !bot->client ) continue;
				if ( !( bot->r.svFlags & SVF_BOT ) ) continue;
				if ( mimicStat == 0 ) {
					bot->client->pers.neonwaveUpHp = mimicValue;
					if ( mimicValue > 0 ) {
						int bonus = mimicValue * 25;
						if ( bonus > 150 ) bonus = 150;
						bot->client->pers.maxHealth = 100 + bonus;
						bot->client->ps.stats[STAT_MAX_HEALTH] = bot->client->pers.maxHealth;
						bot->health = bot->client->pers.maxHealth;
						bot->client->ps.stats[STAT_HEALTH] = bot->health;
					}
				} else if ( mimicStat == 1 ) {
					bot->client->pers.neonwaveDmg = mimicValue;
				} else {
					bot->client->pers.neonwaveSpeed = mimicValue;
				}
			}
		} else {
			G_Printf( "NeonWave: MIMIC copied 0 (no human)\n" );
		}
	}

	nw_waveStartTime = level.time;
	// OVERCHARGE: check if any connected human client has it (coop-aware)
	{
		int cid;
		gentity_t *ent;
		int applied = 0;
		for ( cid = 0; cid < MAX_CLIENTS; cid++ ) {
			ent = &g_entities[cid];
			if ( !ent->inuse || !ent->client ) continue;
			if ( ent->client->pers.connected != CON_CONNECTED ) continue;
			if ( ent->r.svFlags & SVF_BOT && !NW_TestPlayerSkipBots() ) continue;
			if ( nw_perk[cid][ NW_PERK_OVERCHARGE ] > 0 ) {
				char qBuf[8];
				int qf, k;
				int rank = nw_perk[cid][ NW_PERK_OVERCHARGE ];
				trap_Cvar_VariableStringBuffer( "g_quadfactor", qBuf, sizeof( qBuf ) );
				qf = atoi( qBuf );
				if ( qf < 3 ) {
					qf = 3;
				}
				// rank-scaled: +2 per rank (rank 1 -> +2, rank 3 -> +6)
				trap_Cvar_Set( "g_quadfactor", va( "%i", qf + 2 * rank ) );
				for ( k = 0; k < level.maxclients; k++ ) {
					gentity_t *p = &g_entities[k];
					if ( !p->inuse || !p->client ) continue;
					if ( p->client->pers.connected != CON_CONNECTED ) continue;
					if ( p->client->sess.sessionTeam == TEAM_SPECTATOR ) continue;
					if ( p->r.svFlags & SVF_BOT && !NW_TestPlayerSkipBots() ) continue;
					p->health = p->health * 2 / 3;
					if ( p->health < 1 ) p->health = 1;
					p->client->ps.stats[STAT_HEALTH] = p->health;
				}
				nw_perk[cid][ NW_PERK_OVERCHARGE ]--;
				G_Printf( "NeonWave: OVERCHARGE active (client %i, rank %i, quadfactor %i)\n", cid, rank, qf + 2 * rank );
				NeonWave_PerkFx( "overcharge" );
				NW_MirrorPerks( cid );
				applied = 1;
			}
		}
		if ( !applied && nw_perk[0][ NW_PERK_OVERCHARGE ] > 0 ) {
			char qBuf[8];
			int qf, rank;
			rank = nw_perk[0][ NW_PERK_OVERCHARGE ];
			trap_Cvar_VariableStringBuffer( "g_quadfactor", qBuf, sizeof( qBuf ) );
			qf = atoi( qBuf );
			if ( qf < 3 ) {
				qf = 3;
			}
			trap_Cvar_Set( "g_quadfactor", va( "%i", qf + 2 * rank ) );
			nw_perk[0][ NW_PERK_OVERCHARGE ]--;
			G_Printf( "NeonWave: OVERCHARGE active (client 0, rank %i, quadfactor %i)\n", rank, qf + 2 * rank );
		}
	}
	// DASH: check if any connected human client has it (coop-aware)
	{
		int cid;
		gentity_t *ent;
		for ( cid = 0; cid < MAX_CLIENTS; cid++ ) {
			ent = &g_entities[cid];
			if ( !ent->inuse || !ent->client ) continue;
			if ( ent->client->pers.connected != CON_CONNECTED ) continue;
			if ( ent->r.svFlags & SVF_BOT ) continue;
			if ( nw_perk[cid][ NW_PERK_DASH ] > 0 ) {
				NeonWave_PerkFx( "dash" );
			}
		}
	}

	NW_SendStatus( NW_EV_RUNNING );
	if ( nw_synergyIdx >= 0 ) {
		trap_SendServerCommand( -1, va( "cp \"WAVE %i: %s + %s\\n%s\"",
			num, NW_ModifierName( nw_modifier ), NW_ModifierName( nw_modifier2 ),
			NW_SynergyName( nw_synergyIdx ) ) );
	} else if ( nw_modifier != NW_MOD_NONE ) {
		trap_SendServerCommand( -1, va( "cp \"WAVE %i: %s\n\"", num, NW_ModifierName( nw_modifier ) ) );
	} else {
		trap_SendServerCommand( -1, va( "cp \"WAVE %i\n\"", num ) );
	}
	if ( nw_hardcore ) {
		G_Printf( "NeonWave: HARDCORE banner\n" );
		trap_SendServerCommand( -1, va( "cp \"HARDCORE\n\"" ) );
	}
	/* Ghost is sniper-ambush: waves 1-4 used to spawn n+1 rail-rushers
	   before cloak/detector exist. Count = n so a run can reach wave 8. */
	if ( NW_GhostActive() && !nw_hardcore && num >= 1 && num < 5 ) {
		botCount = num;
		if ( botCount < 1 ) {
			botCount = 1;
		}
	}
	/* After swarm/coop/scale. 0 disables. Default 8 keeps addbot off the reliable buffer. */
	{
		char capBuf[8];
		int cap = 8;
		trap_Cvar_VariableStringBuffer( "g_neonwave_drone_cap", capBuf, sizeof( capBuf ) );
		if ( capBuf[0] ) {
			cap = atoi( capBuf );
		}
		if ( cap > 0 && botCount > cap ) {
			G_Printf( "NeonWave: drone cap %i: %i -> %i\n", cap, botCount, cap );
			botCount = cap;
		}
	}
	G_Printf( "NeonWave: starting wave %i (%i bots, skill %i)%s%s%s%s\n", num, botCount, skill,
		num >= NW_BOSS_WAVE ? " + BOSS" : "",
		nw_modifier != NW_MOD_NONE ? va( " [%s]", NW_ModifierName( nw_modifier ) ) : "",
		nw_modifier2 != NW_MOD_NONE ? va( " +[%s]", NW_ModifierName( nw_modifier2 ) ) : "",
		nw_modifier2 != NW_MOD_NONE ? " (SYNERGY)" : "" );
	if ( num >= NW_BOSS_WAVE ) {
		NW_SpawnBoss();
	}
	// Performance: batch-spawn for waves with many bots
	if ( botCount > 3 ) {
		NW_SpawnBotsBatch( skill, botCount );
	} else {
		for ( i = 0; i < botCount && i < MAX_CLIENTS - 2; i++ ) {
			NW_SpawnBot( skill );
		}
	}
	// v0.80: Apply arena drone scaling after spawn
	NW_ApplyDroneScaling();
	if ( NW_GhostActive() ) {
		G_Printf( "NeonWave: GHOST kit active (wave %i)\n", num );
		NW_GhostAnnounceKit();
	}
	if ( NW_GhostActive() && num >= 8 ) {
		int nDet, d, detSkill;
		nDet = 1;
		if ( num >= 12 ) {
			nDet = 2;
		}
		detSkill = skill + 1;
		if ( detSkill > 5 ) {
			detSkill = 5;
		}
		for ( d = 0; d < nDet; d++ ) {
			trap_SendConsoleCommand( EXEC_APPEND,
				va( "set g_neonwave_nextdetector 1; addbot sarge %i \"Detector W%d-%d\"\n",
					detSkill, num, d + 1 ) );
		}
		G_Printf( "NeonWave: DETECTOR spawned (wave %i, %i, skill %i)\n", num, nDet, detSkill );
		NW_GhostSpawnTurret( num );
	}
}

int NeonWave_GetWave( void ) {
	return nw_wave;
}

int NW_GetModifiersSeen( void ) {
	return nw_modifiersSeen;
}

int NW_GetRunStartTime( void ) {
	return nw_runStartTime;
}

qboolean NW_GetOverVictory( void ) {
	return nw_overVictory;
}

int NW_BossPhase( void ) {
	return nw_bossPhase;
}

static void NW_GrantUpgradePoints( void ) {
	int gain = ( nw_wave >= NW_BOSS_WAVE ? 2 : 1 );
	int combo;
	int i;

	if ( nw_ptsMul > 1 ) {
		gain *= nw_ptsMul;
		if ( NW_ModActive( NW_MOD_SURGE ) && nw_synergyIdx != 0 ) {
			G_Printf( "NeonWave: SURGE x3 upgrade points\n" );
		}
		if ( nw_synergyIdx == 0 ) {
			G_Printf( "NeonWave: AERIAL ASSAULT x3 upgrade points\n" );
		}
	}
	// boss kill bonus: +3 for taking down a boss wave
	if ( nw_wave % NW_BOSS_WAVE == 0 ) {
		gain += 3;
		G_Printf( "NeonWave: boss kill bonus +3\n" );
	}
	// combo bonus: +1 point for streaks of 5+ kills
	combo = NW_Cache()->bestCombo;
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
	// Coop: grant points to every connected human client (per-client pool).
	// Broadcast cvar mirrors the first client so legacy readers still work.
	{
		int granted = 0;
		for ( i = 0; i < level.maxclients; i++ ) {
			gentity_t *ent = &g_entities[i];
			if ( !ent->inuse || !ent->client ) continue;
			if ( ent->client->pers.connected != CON_CONNECTED ) continue;
			if ( ent->r.svFlags & SVF_BOT && !NW_TestPlayerSkipBots() ) continue;
			ent->client->pers.neonwaveUpgradePts += gain;
			granted++;
		}
		if ( granted == 0 ) {
			char ptsBuf[16];
			int pts = 0;
			trap_Cvar_VariableStringBuffer( "g_neonwave_upgradepoints", ptsBuf, sizeof(ptsBuf) );
			pts = atoi( ptsBuf ) + gain;
			trap_Cvar_Set( "g_neonwave_upgradepoints", va( "%i", pts ) );
		}
	}
	NW_MirrorPointsCvar();
	G_Printf( "NeonWave: upgrade point granted (%i per client)\n", gain );
	G_Printf( "NeonWave: UPGRADE: next cost 1 (levels 0-3), 2 (levels 4+)\n" );
}

static void NW_EnterBreak( void ) {
	nw_inBreak = qtrue;
	// restore gravity after low-grav wave; log it so headless tests can assert
	// that the modifier side effect is properly undone
	if ( NW_ModActive( NW_MOD_LOWGRAV ) ) {
		trap_Cvar_Set( "g_gravity", "800" );
		G_Printf( "NeonWave: gravity restored to 800\n" );
	}
	if ( NW_ModActive( NW_MOD_TIMEWARP ) || NW_ModActive( NW_MOD_FROST ) ) {
		trap_Cvar_Set( "g_speed", "320" );
		G_Printf( "NeonWave: g_speed restored to 320\n" );
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
	NW_UpdateDifficultyOnClear();
	// Coop: roll offers for each connected human client (per-client perk pools)
	{
		int cid;
		gentity_t *ent;
		int rolled = 0;
		for ( cid = 0; cid < MAX_CLIENTS; cid++ ) {
			ent = &g_entities[cid];
			if ( !ent->inuse || !ent->client ) continue;
			if ( ent->client->pers.connected != CON_CONNECTED ) continue;
			if ( ent->r.svFlags & SVF_BOT && !NW_TestPlayerSkipBots() ) continue;
			NW_RollOffers( cid );
			rolled++;
		}
		if ( rolled == 0 ) {
			NW_RollOffers( 0 );
		}
	}
	NW_SendStatus( NW_EV_CLEARED );
	NeonWave_LogPayload();
	trap_SendServerCommand( -1, va( "cp \\\"WAVE %i CLEARED\\\\n\\\"", nw_wave ) );
	G_Printf( "NeonWave: wave %i cleared, break %i ms\n", nw_wave, nw_breakEnd - level.time );
	NW_SeasonalProgress( nw_wave );
	NW_Autopick();
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

// ---- achievements (persistent badges, exposed in run-stats JSON) ----
// ---- run-stats JSON export (local dashboard data source) ----
#define NW_RUNSTATS_FILE "neonwave_runstats.json"

// Emit a machine-readable run summary at game over. Mirrors the public fields
// of the run (waves, kills, combo, time, accuracy, modifiers seen, difficulty)
// so a player-side dashboard can parse one stable file instead of scraping logs.
static void NW_WriteRunStats( int event ) {
	fileHandle_t f;
	int len;
	const nwCache_t *cache = NW_Cache();
	int kills = cache->kills;
	int bestCombo = cache->bestCombo;
	int runSec = ( level.time - nw_runStartTime ) / 1000;
	int i, modCount = 0, first = qtrue, achCount = 0;
	char buf[1024];
	char mods[256];
	char achs[256];
	const char *modNames[16] = { "", "GLASS DRONES", "SWARM", "LOW GRAVITY",
	                           "DOUBLE POINTS", "TIME WARP", "VAMPIRE",
	                           "FRENZY", "OVERSHIELD", "MIRROR", "REGEN",
	                           "SURGE", "FROST", "CHAOS", "MIMIC", "SHIELD" };

	NW_CheckAchievements( event );

	mods[0] = '\0';
	for ( i = 1; i < NW_MOD_POOL_SIZE; i++ ) {
		if ( nw_modifiersSeen & ( 1 << i ) ) {
			modCount++;
			Com_sprintf( mods + strlen( mods ), sizeof(mods) - strlen( mods ),
				"%s\"%s\"", first ? "" : ", ", modNames[i] );
			first = qfalse;
		}
	}

	achs[0] = '\0';
	for ( i = 0; i < NW_ACH_COUNT; i++ ) {
		if ( nw_achievements[i] ) {
			achCount++;
			Com_sprintf( achs + strlen( achs ), sizeof(achs) - strlen( achs ),
				"%s\"%s\"", achCount > 1 ? ", " : "", NW_AchievementName( i ) );
		}
	}

	Com_sprintf( buf, sizeof(buf),
		"{\n"
		"  \"version\": 1,\n"
		"  \"result\": \"%s\",\n"
		"  \"wave\": %i,\n"
		"  \"kills\": %i,\n"
		"  \"bestCombo\": %i,\n"
		"  \"timeSec\": %i,\n"
		"  \"difficulty\": \"%s\",\n"
		"  \"hardcore\": %i,\n"
		"  \"modifiersSeen\": %i,\n"
		"  \"modifierNames\": [%s],\n"
		"  \"achievements\": [%s]\n"
		"}\n",
		( event == NW_EV_VICTORY ) ? "VICTORY" : "FAILED",
		nw_wave, kills, bestCombo, runSec,
		NW_DifficultyName( nw_difficulty ), nw_hardcore ? 1 : 0, modCount, mods, achs );

	len = trap_FS_FOpenFile( NW_RUNSTATS_FILE, &f, FS_WRITE );
	if ( len < 0 || !f ) {
		G_Printf( "NeonWave: WARNING cannot write " NW_RUNSTATS_FILE "\n" );
		return;
	}
	trap_FS_Write( buf, strlen( buf ), f );
	trap_FS_FCloseFile( f );
	G_Printf( "NeonWave: RUN STATS JSON written (%s)\n", NW_RUNSTATS_FILE );
}

static void NW_GameOver( int event, const char *why ) {
	char rtBuf[8];
	int rt;

	if ( nw_over ) {
		return;
	}
	nw_over = qtrue;
	nw_inBreak = qfalse;
	nw_overVictory = ( event == NW_EV_VICTORY ) ? qtrue : qfalse;
	// restore wave-scoped cvars in case we ended during a modifier
	trap_Cvar_Set( "g_gravity", "800" );
	trap_Cvar_Set( "g_speed", "320" );
	trap_Cvar_Set( "g_quadfactor", "3" );
	NeonWave_UpdateHighscore();
	// Use cached values (already computed by NW_UpdateRecords above)
	NW_UpdateRecords();
	NW_SendStatus( event );
	{
		const nwCache_t *cache = NW_Cache();
		G_Printf( "NeonWave: RUN STATS kills=%i bestCombo=%i time=%is\n",
			cache->kills, cache->bestCombo, ( level.time - nw_runStartTime ) / 1000 );
	}
	NW_KickBots();
	G_Printf( "NeonWave: %s (wave %i)\n", why, nw_wave );
	NW_WriteRunStats( event );
	NeonWave_LogPayload();
	NW_SeasonalCheck( event );

	// dispatch replay test hooks by g_neonwave_replaytest value
	trap_Cvar_VariableStringBuffer( "g_neonwave_replaytest", rtBuf, sizeof(rtBuf) );
	rt = atoi( rtBuf );
	if ( rt == 1 ) {
		// ---- TEST 76: roundtrip ----
		if ( nw_replayTestDone76 ) {
			G_ReplaySave( "neonwave_replay.dat" );
			if ( G_ReplayGetCount() > 0 ) {
				int origCount = G_ReplayGetCount();
				G_ReplayStart();
				G_ReplayLoad( "neonwave_replay.dat" );
				G_Printf( "NeonWave: REPLAY roundtrip events=%d loaded=%d match=%d\n",
					origCount, G_ReplayGetCount(),
					origCount == G_ReplayGetCount() ? 1 : 0 );
			} else {
				G_Printf( "NeonWave: REPLAY no events recorded\n" );
			}
		}
	} else if ( rt == 77 ) {
		// ---- TEST 77: save header ----
		struct replayEvent *rb;
		rb = G_ReplayGetBuffer();
		if ( nw_replayTestDone77 ) {
			G_ReplaySave( "replay_77.dat" );
			{
				char mapBuf[64];
				trap_Cvar_VariableStringBuffer( "mapname", mapBuf, sizeof(mapBuf) );
				G_Printf( "NeonWave: REPLAY SAVE magic=NRPY version=1 events=%d durationMs=%d map=%s\n",
					G_ReplayGetCount(),
					G_ReplayGetCount() > 0 ? rb[G_ReplayGetCount()-1].timestampMs : 0,
					mapBuf );
			}
		}
	} else if ( rt == 78 ) {
		// ---- TEST 78: load and verify events ----
		int loaded;
		int match;
		struct replayEvent e0, e1, e2;
		if ( nw_replayTestDone78 ) {
			G_ReplaySave( "replay_78.dat" );
			G_Printf( "NeonWave: REPLAY SAVE saved %d events to replay_78.dat\n", G_ReplayGetCount() );
			G_ReplayStart();
			G_ReplayLoad( "replay_78.dat" );
			loaded = G_ReplayGetCount();
			G_Printf( "NeonWave: REPLAY LOAD loaded %d events\n", loaded );
			if ( loaded > 0 ) {
				match = 1;
				G_ReplayReset();
				G_ReplayGetNext( &e0 );
				G_ReplayGetNext( &e1 );
				G_ReplayGetNext( &e2 );
				if ( e0.timestampMs != 0 || e1.timestampMs != 0 || e2.timestampMs != 0 ) match = 0;
				G_Printf( "NeonWave: REPLAY LOAD verify match=%d\n", match );
			}
		}
	} else if ( rt == 79 ) {
		// ---- TEST 79: playback walk ----
		int walked = 0;
		struct replayEvent ev;
		if ( nw_replayTestDone79 ) {
			if ( G_ReplayGetCount() > 0 ) {
				G_ReplaySave( "replay_79.dat" );
				G_ReplayStart();
				G_ReplayLoad( "replay_79.dat" );
				G_ReplayPlayStart();
				while ( G_ReplayGetNext( &ev ) ) {
					walked++;
				}
				G_Printf( "NeonWave: REPLAY PLAYBACK playback started\n" );
				G_Printf( "NeonWave: REPLAY PLAYBACK walked %d events\n", walked );
			}
		}
	} else if ( rt == 80 ) {
		// ---- TEST 80: overflow validation ----
		int recorded;
		if ( nw_replayTestDone80 ) {
			recorded = G_ReplayGetCount();
			G_Printf( "NeonWave: REPLAY overflow recorded=%d stored=%d\n",
				recorded, recorded >= REPLAY_MAX_EVENTS ? REPLAY_MAX_EVENTS : recorded );
		}
	}

	LogExit( why );
	}
	// ---- v0.11 boss special mechanics ----
#define NW_BOSS_SHIELD_MS	4000	// tank shield phase duration
#define NW_BOSS_SHIELD_CD	12000	// tank shield cooldown
#define NW_BOSS_RAGE_HP		0.30f	// swarm mother rage below 30% hp
#define NW_BOSS_PHASE2_HP	0.50f	// boss enters phase 2 below 50% hp

static gentity_t *NW_FindBoss( void ) {
	gentity_t *ent;
	int i;
	// Return cached result if valid
	if ( nw_bossEntityCache >= 0 && nw_bossEntityCache < level.maxclients ) {
		ent = &g_entities[nw_bossEntityCache];
		if ( ent->inuse && ent->client && ( ent->r.svFlags & SVF_BOT )
				&& ent->health > 0 && ent->client->pers.neonwaveBoss ) {
			return ent;
		}
		// Cache invalidated - boss died or changed
		nw_bossEntityCache = -1;
	}
	// Scan for boss and cache result
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( ent->inuse && ent->client && ( ent->r.svFlags & SVF_BOT )
				&& ent->health > 0 && ent->client->pers.neonwaveBoss ) {
			nw_bossEntityCache = i;
			return ent;
		}
	}
	return NULL;
}

// v0.33 boss phase 2: below 50% hp the boss escalates (see NW_BossEnterPhase2).
// Test hook g_neonwave_phaseforce 1 forces the phase-2 trigger immediately so CI
// can assert the ENTERS PHASE 2 marker deterministically (the natural low-hp
// cross rarely happens in fast headless runs otherwise).
static qboolean NW_BossInPhase2( gentity_t *boss, int maxhp ) {
	char pfBuf[8];
	trap_Cvar_VariableStringBuffer( "g_neonwave_phaseforce", pfBuf, sizeof(pfBuf) );
	if ( atoi( pfBuf ) == 1 ) {
		return qtrue;
	}
	return ( maxhp > 0 && boss->health < maxhp * NW_BOSS_PHASE2_HP ) ? qtrue : qfalse;
}

static void NW_BossEnterPhase2( void ) {
	nw_bossPhase = 2;
	switch ( nw_bossType ) {
	case NW_BOSS_TANK:
		G_Printf( "NeonWave: TANK ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_SWARM:
		G_Printf( "NeonWave: SWARM MOTHER ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_GLASS:
		G_Printf( "NeonWave: GLASS CANNON ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_WARDEN:
		G_Printf( "NeonWave: WARDEN ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_SNIPER:
		G_Printf( "NeonWave: SNIPER ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_BERSERKER:
		G_Printf( "NeonWave: BERSERKER ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_TELEPORTER:
		G_Printf( "NeonWave: TELEPORTER ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_HEALER:
		G_Printf( "NeonWave: HEALER ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_SHIELDER:
		G_Printf( "NeonWave: SHIELDER ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_SNIPELITE:
		G_Printf( "NeonWave: SNIPER ELITE ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_DEMOLISHER:
		G_Printf( "NeonWave: DEMOLISHER ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_CHRONOMANCER:
		G_Printf( "NeonWave: CHRONOMANCER ENTERS PHASE 2\n" );
		break;
	case NW_BOSS_VOIDWALKER:
		G_Printf( "NeonWave: VOID WALKER ENTERS PHASE 2\n" );
		break;
	default:
		G_Printf( "NeonWave: BOSS ENTERS PHASE 2\n" );
		break;
	}
}

static void NW_CheckBossPhase2( void ) {
	gentity_t *boss;
	int maxhp;
	if ( nw_bossPhase != 1 ) {
		return; // already in phase 2
	}
	if ( nw_wave < NW_BOSS_WAVE ) {
		return; // no boss yet
	}
	boss = NW_FindBoss();
	if ( !boss ) {
		return;
	}
	maxhp = boss->client->ps.stats[STAT_MAX_HEALTH];
	if ( NW_BossInPhase2( boss, maxhp ) ) {
		NW_BossEnterPhase2();
	}
}



static void NW_BossMechanicsFrame( int *lastMini, int bots ) {
	NW_CheckBossPhase2();
	if ( nw_bossType == NW_BOSS_SWARM ) {
		gentity_t *boss = NW_FindBoss();
		if ( boss ) {
			char rfBuf[8];
			int maxhp = boss->client->ps.stats[STAT_MAX_HEALTH];
			qboolean rage;
			trap_Cvar_VariableStringBuffer( "g_neonwave_rageforce", rfBuf, sizeof(rfBuf) );
			if ( atoi( rfBuf ) == 1 ) {
				boss->health = maxhp * NW_BOSS_RAGE_HP / 2;
			}
			rage = ( maxhp > 0
				&& boss->health < maxhp * NW_BOSS_RAGE_HP ) ? qtrue : qfalse;
			if ( rage && !*lastMini ) {
				G_Printf( "NeonWave: SWARM MOTHER ENRAGED\n" );
			}
			if ( level.time > *lastMini && bots < 15 ) {
				int spawnCd = ( rage ? 5000 : 10000 );
				if ( nw_bossPhase == 2 ) {
					spawnCd /= 2;
				}
				*lastMini = level.time + spawnCd;
				NW_SpawnBot( 3 );
				G_Printf( "NeonWave: swarm mother spawns mini-drone%s%s\n",
					rage ? " (RAGE)" : "",
					nw_bossPhase == 2 ? " (PHASE 2)" : "" );
			}
			return;
		}
	}

	if ( nw_bossType == NW_BOSS_BERSERKER ) {
		gentity_t *boss = NW_FindBoss();
		if ( boss ) {
			int maxhp = boss->client->ps.stats[STAT_MAX_HEALTH];
			qboolean rage;
			if ( maxhp < 1 ) {
				maxhp = boss->client->pers.maxHealth;
			}
			{
				char rfBuf[8];
				trap_Cvar_VariableStringBuffer( "g_neonwave_rageforce", rfBuf, sizeof(rfBuf) );
				if ( atoi( rfBuf ) == 1 ) {
					boss->health = (int)( maxhp * NW_BOSS_RAGE_HP / 2 );
					if ( boss->health < 1 ) {
						boss->health = 1;
					}
					boss->client->ps.stats[STAT_HEALTH] = boss->health;
				}
			}
			rage = ( maxhp > 0 && boss->health < maxhp * NW_BOSS_RAGE_HP );
			if ( rage && nw_bossPhase == 1 ) {
				nw_bossPhase = 2;
				G_Printf( "NeonWave: BERSERKER ENTERS RAGE\n" );
				trap_SendServerCommand( -1, "cp \"BERSERKER ENTERS RAGE\\n\"" );
			}
			if ( rage && level.time - nw_bossLastAttack > 800 ) {
				nw_bossLastAttack = level.time;
				trap_SendServerCommand( -1, "cp \"BERSERKER ATTACKS\\n\"" );
			}
		}
	}

	if ( nw_bossType == NW_BOSS_HEALER ) {
		static int lastHeal = 0;
		gentity_t *boss = NW_FindBoss();
		if ( boss ) {
			int i;
			int healed = 0;
			if ( level.time - lastHeal < 2000 ) {
				// skip healing this frame
			} else {
				lastHeal = level.time;
				for ( i = 0; i < level.maxclients; i++ ) {
					gentity_t *ent = &g_entities[i];
					if ( !ent->inuse || !ent->client ) continue;
					if ( ent->r.svFlags & SVF_BOT && ent->health > 0 && ent != boss ) {
						int dist = (int)Distance(ent->r.currentOrigin, boss->r.currentOrigin);
						if ( dist < 150 ) {
							int heal = 20;
							int maxHealth = ent->client->ps.stats[STAT_MAX_HEALTH];
							if ( ent->health + heal > maxHealth ) heal = maxHealth - ent->health;
							ent->health += heal;
							if ( healed == 0 ) G_Printf( "NeonWave: HEALER heals nearby bots for %i HP\n", heal );
							healed++;
						}
					}
				}
			}
		}
	}

	if ( nw_bossType == NW_BOSS_TELEPORTER ) {
		gentity_t *boss = NW_FindBoss();
		int maxhp = 0;
		int teleportCd = 8000;
		if ( boss ) {
			maxhp = boss->client->ps.stats[STAT_MAX_HEALTH];
			if ( maxhp > 0 && boss->health < maxhp / 2 ) {
				teleportCd = 4000;
			}
		}
		if ( level.time - nw_bossLastAttack > teleportCd ) {
			vec3_t origin, angles;
			gentity_t *spawn;
			nw_bossLastAttack = level.time;
			G_Printf( "NeonWave: TELEPORTER blinks to new position\n" );
			trap_SendServerCommand( -1, "cp \"TELEPORTER blinks away!\\n\"" );
			if ( boss ) {
				spawn = SelectSpawnPoint( vec3_origin, origin, angles, 0 );
				if ( spawn ) {
					G_SetOrigin( boss, origin );
					trap_LinkEntity( boss );
				}
			}
		}
	}

	if ( nw_bossType == NW_BOSS_TANK ) {
		static int nextShield;
		gentity_t *boss = NW_FindBoss();
		if ( boss ) {
			int maxhp = boss->client->ps.stats[STAT_MAX_HEALTH];
			int shieldMs = NW_BOSS_SHIELD_MS;
			int shieldCd = NW_BOSS_SHIELD_CD;
			if ( nw_bossPhase == 2 ) {
				shieldMs = NW_BOSS_SHIELD_MS + 1000;
				shieldCd = NW_BOSS_SHIELD_CD - 4000;
			}
			if ( !boss->client->pers.neonwaveBossShield && level.time > nextShield ) {
				boss->client->pers.neonwaveBossShield = 1;
				boss->client->pers.neonwaveBossShieldEnd = level.time + shieldMs;
				nextShield = level.time + shieldCd;
				G_Printf( "NeonWave: TANK raises SHIELD%s\n",
					nw_bossPhase == 2 ? " (PHASE 2)" : "" );
			}
			if ( boss->client->pers.neonwaveBossShield ) {
				if ( level.time > boss->client->pers.neonwaveBossShieldEnd ) {
					boss->client->pers.neonwaveBossShield = 0;
					G_Printf( "NeonWave: TANK shield drops\n" );
				} else if ( maxhp > 0 && boss->health < maxhp ) {
					boss->health += 2;
					if ( boss->health > maxhp ) {
						boss->health = maxhp;
					}
				}
			}
		}
	}

	if ( nw_bossType == NW_BOSS_WARDEN ) {
		static int nextStrike;
		char wfBuf[8];
		int forcedStrike;
		gentity_t *boss = NW_FindBoss();
		gentity_t *player = NULL;
		int i;
		trap_Cvar_VariableStringBuffer( "g_neonwave_wardenforce", wfBuf, sizeof(wfBuf) );
		forcedStrike = atoi( wfBuf );
		if ( level.time > nextStrike || forcedStrike == 1 ) {
			nextStrike = level.time + ( nw_bossPhase == 2 ? 5000 : 8000 );
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
				org[0] += ( rand() % 300 ) - 150;
				org[1] += ( rand() % 300 ) - 150;
				VectorCopy( org, boss->s.origin );
				VectorCopy( org, boss->client->ps.origin );
				G_Printf( "NeonWave: WARDEN strikes the player zone\n" );
				boss->client->pers.neonwaveBossShield = 1;
				boss->client->pers.neonwaveBossShieldEnd = level.time + 3000;
				G_Printf( "NeonWave: WARDEN raises armor\n" );
			}
		}
		if ( boss && boss->client->pers.neonwaveBossShield
				&& level.time > boss->client->pers.neonwaveBossShieldEnd ) {
			boss->client->pers.neonwaveBossShield = 0;
			G_Printf( "NeonWave: WARDEN armor drops\n" );
		}
	}

	if ( nw_bossType == NW_BOSS_SNIPER ) {
		static int lastDash;
		gentity_t *boss = NW_FindBoss();
		if ( boss && level.time > lastDash ) {
			int maxhp = boss->client->ps.stats[STAT_MAX_HEALTH];
			lastDash = level.time + ( nw_bossPhase == 2 ? 6000 : 9000 );
			{
				char pctBuf[8];
				int pct;
				trap_Cvar_VariableStringBuffer( "g_neonwave_dashforce", pctBuf, sizeof(pctBuf) );
				pct = atoi( pctBuf );
				if ( pct > 0 ) {
					boss->health = maxhp * pct / 100;
				}
			}
			if ( maxhp > 0 && ( nw_bossPhase == 2 || boss->health < maxhp / 2 ) ) {
				vec3_t org = { 0, 0, 0 };
				VectorCopy( boss->r.currentOrigin, org );
				org[0] += ( rand() % 400 ) - 200;
				org[1] += ( rand() % 400 ) - 200;
				VectorCopy( org, boss->s.origin );
				VectorCopy( org, boss->client->ps.origin );
				G_Printf( "NeonWave: SNIPER dashes to new position%s\n",
					nw_bossPhase == 2 ? " (PHASE 2)" : "" );
			}
		}
	}

	if ( nw_bossType == NW_BOSS_DEMOLISHER ) {
		static int nextRocket;
		gentity_t *boss = NW_FindBoss();
		if ( boss && level.time > nextRocket && bots < 22 ) {
			nextRocket = level.time + 3000;
			// Spawn a rocket at the player's position (splash damage)
			G_Printf( "NeonWave: DEMOLISHER fires rocket barrage%s\\n",
				nw_bossPhase == 2 ? " (PHASE 2)" : "" );
			// Simplified: spawn extra bots as "rockets"
			NW_SpawnBot( 3 );
		}
	}

	if ( nw_bossType == NW_BOSS_SNIPELITE ) {
		static int nextSnipe;
		gentity_t *boss = NW_FindBoss();
		if ( boss && level.time > nextSnipe ) {
			nextSnipe = level.time + 1500; // Fast sniper
			G_Printf( "NeonWave: SNIPER ELITE rapid rail%s\\n",
				nw_bossPhase == 2 ? " (PHASE 2)" : "" );
		}
	}

	if ( nw_bossType == NW_BOSS_SHIELDER ) {
		static int shieldActive;
		gentity_t *boss = NW_FindBoss();
		if ( boss && !shieldActive ) {
			shieldActive = 1;
			G_Printf( "NeonWave: SHIELDER deploys energy shield\\n" );
		}
		if ( boss && shieldActive && boss->health < 200 ) {
			shieldActive = 0;
			G_Printf( "NeonWave: SHIELDER shield drops\\n" );
		}
	}

	if ( nw_bossType == NW_BOSS_GLASS ) {
		static int nextGlassMini;
		gentity_t *boss = NW_FindBoss();
		if ( boss && nw_bossPhase == 2 && level.time > nextGlassMini && bots < 20 ) {
			nextGlassMini = level.time + 7000;
			NW_SpawnBot( 3 );
			G_Printf( "NeonWave: GLASS CANNON summons support drone (PHASE 2)\n" );
		}
	}

	if ( nw_bossType == NW_BOSS_CHRONOMANCER ) {
		static int nextTeleport;
		static int nextSlow;
		gentity_t *boss = NW_FindBoss();
		if ( boss ) {
			int maxhp = boss->client->ps.stats[STAT_MAX_HEALTH];
			int teleportCd = ( nw_bossPhase == 2 ? 4000 : 6000 );
			if ( maxhp > 0 && boss->health < maxhp / 2 ) {
				teleportCd = 3000;
			}
			if ( level.time > nextTeleport ) {
				vec3_t origin, angles;
				gentity_t *spawn;
				nextTeleport = level.time + teleportCd;
				G_Printf( "NeonWave: CHRONOMANCER warps time and teleports%s\n",
					nw_bossPhase == 2 ? " (PHASE 2)" : "" );
				trap_SendServerCommand( -1, "cp \"CHRONOMANCER warps time!\n\"" );
				spawn = SelectSpawnPoint( vec3_origin, origin, angles, 0 );
				if ( spawn ) {
					G_SetOrigin( boss, origin );
					trap_LinkEntity( boss );
				}
			}
			if ( level.time > nextSlow ) {
				nextSlow = level.time + ( nw_bossPhase == 2 ? 8000 : 12000 );
				G_Printf( "NeonWave: CHRONOMANCER slows nearby bots%s\n",
					nw_bossPhase == 2 ? " (PHASE 2)" : "" );
			}
		}
	}

	if ( nw_bossType == NW_BOSS_VOIDWALKER ) {
		static int nextPhase;
		static int phaseActive;
		static int nextVoidAura;
		gentity_t *boss = NW_FindBoss();
		if ( boss ) {
			int maxhp = boss->client->ps.stats[STAT_MAX_HEALTH];
			int phaseCd = ( nw_bossPhase == 2 ? 5000 : 8000 );
			// Phase-shifting: invulnerability for 2s
			if ( level.time > nextPhase ) {
				nextPhase = level.time + phaseCd;
				phaseActive = 1;
				boss->client->pers.neonwaveBossShield = 1;
				boss->client->pers.neonwaveBossShieldEnd = level.time + 2000;
				G_Printf( "NeonWave: VOID WALKER phaseshift%s\n",
					nw_bossPhase == 2 ? " (PHASE 2)" : "" );
				trap_SendServerCommand( -1, "cp \"VOID WALKER phaseshift!\n\"" );
			}
			if ( phaseActive && level.time > boss->client->pers.neonwaveBossShieldEnd ) {
				phaseActive = 0;
				boss->client->pers.neonwaveBossShield = 0;
				G_Printf( "NeonWave: VOID WALKER materializes%s\n",
					nw_bossPhase == 2 ? " (PHASE 2)" : "" );
			}
			// Void aura: damage nearby bots
			if ( level.time > nextVoidAura ) {
				nextVoidAura = level.time + 10000;
				G_Printf( "NeonWave: VOID WALKER void aura pulses%s\n",
					nw_bossPhase == 2 ? " (PHASE 2)" : "" );
			}
			// Enrage at low HP
			if ( maxhp > 0 && boss->health < maxhp * 0.3f ) {
				if ( nw_bossPhase == 1 ) {
					nw_bossPhase = 2;
					G_Printf( "NeonWave: VOID WALKER ENRAGES\n" );
					trap_SendServerCommand( -1, "cp \"VOID WALKER ENRAGES!\n\"" );
				}
			}
		}
	}
}


// ---- Momentum System (v0.55) ----
// Momentum increases on kills (configurable via g_momentum_kill) and decays over time.
// At thresholds, movement abilities are unlocked:
//   25: Slide (faster sprint)
//   50: Wall-Jump (jump off walls)
//   75: Air-Dash (quick direction change in air)
//   100: Blink (teleport 5m forward, 1s cooldown)
void NW_MomentumOnKill( gentity_t *attacker ) {
	char buf[8];
	int killVal, newMomentum;
	if ( !attacker || !attacker->client ) return;
	if ( attacker->r.svFlags & SVF_BOT ) return;
	trap_Cvar_VariableStringBuffer( "g_momentum_kill", buf, sizeof(buf) );
	killVal = atoi(buf);
	if ( killVal <= 0 ) return;
	newMomentum = attacker->client->pers.nwMomentum + killVal;
	if ( newMomentum > 100 ) newMomentum = 100;
	attacker->client->pers.nwMomentum = newMomentum;
}

static void NW_MomentumFrame( void ) {
	int i;
	gentity_t *ent;
	int decay;
	char buf[8];
	trap_Cvar_VariableStringBuffer( "g_momentum_decay", buf, sizeof(buf) );
	decay = atoi(buf);
	if ( decay <= 0 ) return;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->r.svFlags & SVF_BOT ) continue;
		if ( ent->client->pers.nwMomentum <= 0 ) continue;
		if ( level.time - ent->client->pers.nwMomentumTick > 1000 ) {
			ent->client->pers.nwMomentum -= decay;
			if ( ent->client->pers.nwMomentum < 0 ) ent->client->pers.nwMomentum = 0;
			ent->client->pers.nwMomentumTick = level.time;
		}
	}
}

// ---- Legacy Echo (v0.55) ----
// After game over, a legacy echo can be activated in the next run.
// Player presses [Use] near echo spawn point for:
//   - Boost: +20% DMG for 5s (1x per run)
//   - Intel: Show next 3 waves in HUD
//   - Sacrifice: Full HP, -50% score
static void NW_LegacyFrame( void ) {
	int i;
	gentity_t *ent;
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->r.svFlags & SVF_BOT ) continue;
		// Check for legacy boost activation
		if ( ent->client->pers.nwLegacyAvailable && (ent->client->buttons & BUTTON_USE_HOLDABLE) ) {
			if ( ent->client->pers.nwLegacyBoostEnd <= level.time ) {
				char durBuf[8], dmgBuf[8];
				int dur, dmg;
				trap_Cvar_VariableStringBuffer( "g_legacy_boost_dur", durBuf, sizeof(durBuf) );
				dur = atoi(durBuf);
				if ( dur <= 0 ) dur = 5;
				trap_Cvar_VariableStringBuffer( "g_legacy_boost_dmg", dmgBuf, sizeof(dmgBuf) );
				dmg = atoi(dmgBuf);
				if ( dmg <= 0 ) dmg = 20;
				ent->client->pers.nwLegacyBoost = 1;
				ent->client->pers.nwLegacyBoostEnd = level.time + (dur * 1000);
				ent->client->pers.nwLegacyAvailable = 0;
				trap_SendServerCommand( i, va("print \"LEGACY BOOST: +%d%% DMG for %ds\\n\"", dmg, dur) );
			}
		}
		// Expire boost
		if ( ent->client->pers.nwLegacyBoost && level.time > ent->client->pers.nwLegacyBoostEnd ) {
			ent->client->pers.nwLegacyBoost = 0;
			trap_SendServerCommand( i, "print \"LEGACY BOOST expired\\n\"" );
		}
	}
}

void NeonWave_OnDroneKill( gentity_t *attacker ) {
	gentity_t *t;
	if ( g_gametype.integer != GT_NEONWAVE ) {
		return;
	}
	NW_GhostOnKill( attacker );
	if ( !NW_ModActive( NW_MOD_VAMPIRE ) ) {
		return;
	}
	t = attacker;
	if ( !t || !t->client || t->health <= 0
			|| ( ( t->r.svFlags & SVF_BOT ) && !NW_TestPlayerSkipBots() ) ) {
		t = NW_VampireHealTarget();
	}
	if ( t && t->health > 0 ) {
		NW_VampireHeal( t );
	}
}

void NeonWave_Frame( void ) {
	int humans, bots, i;
	gentity_t *ent;
	static int lastRefresh;

	if ( g_gametype.integer != GT_NEONWAVE ) {
		return;
	}
	NW_GhostFrame();
	// codex/bestiary toggle: mirror the cvar state so headless tests can assert
	// the trigger fired (the actual HUD panel is drawn client-side in cg_draw.c)
	{
		char codexBuf[8];
		trap_Cvar_VariableStringBuffer( "g_neonwave_codex", codexBuf, sizeof(codexBuf) );
		if ( atoi(codexBuf) == 1 ) {
			trap_Cvar_Set( "g_neonwave_codex_rendered", "1" );
			G_Printf( "NeonWave: CODEX rendered\n" );
		}
	}
	if ( level.intermissiontime || level.intermissionQueued ) {
		return;
	}
	if ( nw_over ) {
		return;
	}

	// Performance: Only invalidate cache every 100ms instead of every frame
	// This reduces CPU usage significantly in late waves with 20+ bots
	if ( level.time - lastRefresh > 100 ) {
		NW_InvalidateCache();
		lastRefresh = level.time;
	}

	humans = 0;
	bots = 0;
	{
		int standin = 0;
		for ( i = 0; i < level.maxclients; i++ ) {
			ent = &g_entities[i];
			if ( !ent->inuse || !ent->client ) continue;
			if ( ent->client->pers.connected != CON_CONNECTED ) continue;
			if ( ent->health <= 0 ) continue;
			if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) continue;
			if ( ent->r.svFlags & SVF_BOT ) {
				if ( NW_TestPlayerSkipBots() && !standin ) {
					standin = 1;
					humans++;
					continue;
				}
				bots++;
			} else {
				humans++;
			}
		}
	}
	nw_aliveBots = bots;

	// v0.80: Bot type logic (Healer, Shielder, Elite)
	for ( i = 0; i < level.maxclients; i++ ) {
		gentity_t* healerTarget;
		gentity_t* shieldTarget;
		char nameBuf[MAX_INFO_STRING];
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( !( ent->r.svFlags & SVF_BOT ) ) continue;
		if ( ent->health <= 0 ) continue;

		// Parse bot type from name
		Q_strncpyz( nameBuf, ent->client->pers.netname, sizeof(nameBuf) );
		if ( strstr( nameBuf, "HEALER" ) ) {
			// Healer: green glow, heals nearby bots every 3s
			ent->s.constantLight = 0 | ( 255 << 8 ) | ( 0 << 16 ) | ( 120 << 24 );
			if ( level.time % 3000 < 100 ) {
				int j;
				for ( j = 0; j < level.maxclients; j++ ) {
					healerTarget = &g_entities[j];
					if ( healerTarget == ent || !healerTarget->inuse || !healerTarget->client ) continue;
					if ( !( healerTarget->r.svFlags & SVF_BOT ) ) continue;
					if ( healerTarget->health <= 0 ) continue;
					{
						vec3_t diff;
						VectorSubtract( ent->client->ps.origin, healerTarget->client->ps.origin, diff );
						if ( VectorLength( diff ) < 200 ) {
							healerTarget->health += 20;
							if ( healerTarget->health > 100 ) healerTarget->health = 100;
						}
					}
				}
			}
		} else if ( strstr( nameBuf, "SHIELD" ) ) {
			// Shielder: blue glow, gives shield to nearby bot every 5s
			ent->s.constantLight = 0 | ( 0 << 8 ) | ( 255 << 16 ) | ( 120 << 24 );
			if ( level.time % 5000 < 100 ) {
				int j;
				for ( j = 0; j < level.maxclients; j++ ) {
					shieldTarget = &g_entities[j];
					if ( shieldTarget == ent || !shieldTarget->inuse || !shieldTarget->client ) continue;
					if ( !( shieldTarget->r.svFlags & SVF_BOT ) ) continue;
					if ( shieldTarget->health <= 0 ) continue;
					{
						vec3_t diff;
						VectorSubtract( ent->client->ps.origin, shieldTarget->client->ps.origin, diff );
						if ( VectorLength( diff ) < 200 ) {
							shieldTarget->client->ps.stats[STAT_ARMOR] += 50;
							if ( shieldTarget->client->ps.stats[STAT_ARMOR] > 150 ) shieldTarget->client->ps.stats[STAT_ARMOR] = 150;
						}
					}
				}
			}
		} else if ( strstr( nameBuf, "ELITE" ) ) {
			// Elite: red glow, fast dash every 2s
			ent->s.constantLight = 255 | ( 0 << 8 ) | ( 0 << 16 ) | ( 120 << 24 );
			if ( level.time % 2000 < 100 ) {
				vec3_t forward;
				AngleVectors( ent->client->ps.viewangles, forward, NULL, NULL );
				VectorMA( ent->client->ps.origin, 100, forward, ent->client->ps.origin );
			}
		}
	}

	// MIMIC: magenta glow on drones (mirror effect, similar to WARDEN boss)
	// Performance: Only apply when MIMIC is active
	if ( NW_ModActive( NW_MOD_MIMIC ) ) {
		for ( i = 0; i < level.maxclients; i++ ) {
			ent = &g_entities[i];
			if ( !ent->inuse || !ent->client ) continue;
			if ( !( ent->r.svFlags & SVF_BOT ) ) continue;
			if ( ent->health <= 0 ) continue;
			ent->s.constantLight = 255 | ( 0 << 8 ) | ( 255 << 16 ) | ( 120 << 24 );
		}
	}
	NW_SelfKillHuman(); // test hook: kill human each frame (coop respawn test)
	// Move dead humans to spectator in coop mode
	NW_CoopSpectatorDead();
	NW_MomentumFrame();
	NW_LegacyFrame();

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
				{
					int now = trap_Milliseconds();
					if ( k == 0 || now - nw_multikillTime > 1000 ) {
						nw_multikillCount = 1;
					} else {
						nw_multikillCount++;
					}
					nw_multikillTime = now;
				}
			}
			if ( n > 1 ) {
				nw_multikillCount = n;
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
				gentity_t *heal = NULL;
				int skipPlayer = 0;
				if ( NW_ModActive( NW_MOD_VAMPIRE ) ) {
					heal = NW_VampireHealTarget();
				}
				for ( i = 0; i < level.maxclients; i++ ) {
					ent = &g_entities[i];
					if ( !ent->inuse || !ent->client ) continue;
					if ( !( ent->r.svFlags & SVF_BOT ) ) continue;
					if ( ent->health <= 0 ) continue;
					if ( NW_TestPlayerSkipBots() && !skipPlayer ) {
						skipPlayer = 1;
						continue;
					}
					// heal first: bot-as-player is also in this list, and healing
					// AFTER setting health 0 would revive them every frame.
					if ( heal && heal->health > 0 ) {
						NW_VampireHeal( heal );
					}
					nw_runKills++;
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

		// test hook: g_neonwave_replaytest 1 — record a few events on wave 1,
		// save+load at game over, verify roundtrip (used by assert_76)
		if ( !nw_over && nw_wave == 1 && !nw_replayTestDone76 ) {
			char rtBuf[8];
			trap_Cvar_VariableStringBuffer( "g_neonwave_replaytest", rtBuf, sizeof(rtBuf) );
			if ( atoi( rtBuf ) == 1 && nw_started ) {
				G_ReplayStart();
				G_ReplayRecord( 0, 1.0f, 0.0f, 0 );  // MOVE
				G_ReplayRecord( 1, 0.5f, 0.5f, 0 );  // AIM
				G_ReplayRecord( 2, 0.0f, 0.0f, 1 );  // FIRE
				G_ReplayRecord( 0, -1.0f, 0.0f, 0 ); // MOVE
				G_ReplayRecord( 4, 0.0f, 0.0f, 0 );  // JUMP
				nw_replayTestDone76 = qtrue;
			}
		}

		// test hook: g_neonwave_replaytest 77 — save header metadata (used by test 77)
		if ( !nw_over && !nw_replayTestDone77 && nw_wave == 1 ) {
			char rtBuf[8];
			trap_Cvar_VariableStringBuffer( "g_neonwave_replaytest", rtBuf, sizeof(rtBuf) );
			if ( atoi( rtBuf ) == 77 && nw_started ) {
				G_ReplayStart();
				G_ReplayRecord( 0, 1.0f, 0.0f, 0 );  // MOVE
				G_ReplayRecord( 2, 0.0f, 0.0f, 1 );  // FIRE
				nw_replayTestDone77 = qtrue;
			}
		}

		// test hook: g_neonwave_replaytest 78 — pre-seed events, then reload (used by test 78)
		if ( !nw_over && !nw_replayTestDone78 && nw_wave == 1 ) {
			char rtBuf[8];
			trap_Cvar_VariableStringBuffer( "g_neonwave_replaytest", rtBuf, sizeof(rtBuf) );
			if ( atoi( rtBuf ) == 78 && nw_started ) {
				G_ReplayStart();
				G_ReplayRecord( 0, 0.0f, 0.0f, 0 );  // MOVE
				G_ReplayRecord( 1, 0.0f, 0.0f, 0 );  // AIM
				G_ReplayRecord( 2, 0.0f, 0.0f, 0 );  // FIRE
				nw_replayTestDone78 = qtrue;
			}
		}

		// test hook: g_neonwave_replaytest 79 — record events for playback walk (used by test 79)
		if ( !nw_over && !nw_replayTestDone79 && nw_wave == 1 ) {
			char rtBuf[8];
			trap_Cvar_VariableStringBuffer( "g_neonwave_replaytest", rtBuf, sizeof(rtBuf) );
			if ( atoi( rtBuf ) == 79 && nw_started ) {
				G_ReplayStart();
				G_ReplayRecord( 0, 0.0f, 0.0f, 0 );  // MOVE
				G_ReplayRecord( 1, 1.0f, 0.0f, 0 );  // AIM
				G_ReplayRecord( 2, 0.0f, 0.0f, 0 );  // FIRE
				G_ReplayRecord( 3, 0.0f, 1.0f, 0 );  // CROUCH
				nw_replayTestDone79 = qtrue;
			}
		}

		// test hook: g_neonwave_replaytest 80 — overflow test: record past REPLAY_MAX_EVENTS (used by test 80)
		if ( !nw_over && nw_wave == 1 && !nw_replayTestDone80 ) {
			char rtBuf[8];
			trap_Cvar_VariableStringBuffer( "g_neonwave_replaytest", rtBuf, sizeof(rtBuf) );
			if ( atoi( rtBuf ) == 80 && nw_started ) {
				G_ReplayStart();
				for ( i = 0; i < REPLAY_MAX_EVENTS + 5; i++ ) {
					G_ReplayRecord( i % 5, (float)(i % 3), (float)(i % 2), (unsigned char)(i % 256) );
				}
				nw_replayTestDone80 = qtrue;
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

	if ( nw_waveHadBots && NW_CoopWaveClear( bots ) ) {
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
