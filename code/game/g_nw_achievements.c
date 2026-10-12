// g_nw_achievements.c - NeonWave achievements (per-run badges + lifetime state)
// Split out of g_neonwave.c. Per-run bitmask (nw_achievements) is evaluated
// at game over; unlocked-ever state persists to neonwave_achievements.dat.
// Shared run state comes via g_nw_internal.h.
#include "g_local.h"
#include "g_nw_internal.h"

#ifdef NEONARENA_MOD

qboolean nw_achievements[ NW_ACH_COUNT ]; // unlocked this run (run-stats JSON)
static qboolean nw_achEver[ NW_ACH_COUNT ]; // unlocked at least once (lifetime)

// Per-run badges are computed at game over and mirrored into the run-stats JSON
// (achievements[]). Unlocked-ever state is persisted to neonwave_achievements.dat
// so a player-side dashboard can show lifetime progress (X/8 unlocked).
const char *NW_AchievementName( int id ) {
	switch ( id ) {
	case NW_ACH_FIRST_VICTORY: return "FIRST VICTORY";
	case NW_ACH_SURVIVOR:      return "SURVIVOR";
	case NW_ACH_SHARPSHOOTER:  return "SHARPSHOOTER";
	case NW_ACH_STREAKER:      return "STREAKER";
	case NW_ACH_FLAWLESS:      return "FLAWLESS";
	case NW_ACH_COMBOMASTER:  return "COMBOMASTER";
	case NW_ACH_SPEEDRUNNER:  return "SPEEDRUNNER";
	case NW_ACH_HARDCORE:     return "HARDCORE";
	case NW_ACH_KILL_100:     return "BOT SLAYER";
	case NW_ACH_KILL_1000:    return "BOT ANNIHILATOR";
	case NW_ACH_WAVE_5:       return "GETTING STARTED";
	case NW_ACH_WAVE_10:      return "VETERAN";
	case NW_ACH_WAVE_30:      return "ENDURANCE";
	case NW_ACH_WAVE_50:      return "MARATHON";
	case NW_ACH_PERFECT_WAVE: return "UNTOUCHABLE";
	case NW_ACH_MULTIKILL_3:  return "TRIPLE KILL";
	case NW_ACH_MULTIKILL_5:  return "PENTAKILL";
	case NW_ACH_RAILGUN_MASTER:  return "RAILGUN MASTER";
	case NW_ACH_LIGHTNING_MASTER: return "STORM BRINGER";
	case NW_ACH_PLASMA_MASTER:    return "PLASMA SPECIALIST";
	case NW_ACH_BOSS_RUSH:    return "BOSS RUSH";
	case NW_ACH_ECHO_CHAMPION:    return "ECHO MASTER";
	case NW_ACH_OVERCLOCKED:       return "OVERCLOCKED";
	case NW_ACH_FUSION_DISCOVERER: return "FUSION SCHOLAR";
	case NW_ACH_ALL_UPGRADES:      return "MAXED OUT";
	default:                   return "";
	}
}

static qboolean nw_achEver[ NW_ACH_COUNT ]; // unlocked at least once (lifetime)

void NW_LoadAchievements( void ) {
	fileHandle_t f;
	int i, len, n;
	memset( nw_achEver, 0, sizeof( nw_achEver ) );
	len = trap_FS_FOpenFile( NW_ACHIEVEMENTS_FILE, &f, FS_READ );
	if ( len >= 0 && f ) {
		n = len;
		if ( n > (int)sizeof( nw_achEver ) ) {
			n = (int)sizeof( nw_achEver );
		}
		if ( n > 0 ) {
			trap_FS_Read( nw_achEver, n, f );
		}
		trap_FS_FCloseFile( f );
	}
	for ( i = 0; i < NW_ACH_COUNT; i++ ) {
		if ( nw_achEver[i] ) {
			G_Printf( "NeonWave: achievement already unlocked: %s\n",
				NW_AchievementName( i ) );
		}
	}
}

void NW_SaveAchievements( void ) {
	fileHandle_t f;
	int len;
	/* Headless tests earn the log line. They must not write the player profile. */
	if ( NW_Headless() ) {
		G_Printf( "NeonWave: achievement save skipped (autostart)\n" );
		return;
	}
	len = trap_FS_FOpenFile( NW_ACHIEVEMENTS_FILE, &f, FS_WRITE );
	if ( len < 0 || !f ) {
		G_Printf( "NeonWave: WARNING cannot write " NW_ACHIEVEMENTS_FILE "\n" );
		return;
	}
	trap_FS_Write( nw_achEver, sizeof( nw_achEver ), f );
	trap_FS_FCloseFile( f );
}

// Evaluate per-run achievements, merge into lifetime state, emit log markers.
// "ACHIEVEMENT <NAME>" fire every run the badge is earned (CI-assertable).
// "ACHIEVEMENT UNLOCKED <NAME>" fires only the first time ever (player feedback).
void NW_CheckAchievements( int event ) {
	int combo, deaths, victory, runSec, kills, i;
	char fkBuf[16];
	int fk;
	trap_Cvar_VariableStringBuffer( "g_neonwave_fakekills", fkBuf, sizeof(fkBuf) );
	fk = atoi( fkBuf );
	if ( fk > 0 ) {
		nw_runKills += fk;
		NW_InvalidateCache();
	}
	combo = NW_Cache()->bestCombo;
	deaths = NW_RunDeaths();
	victory = ( event == NW_EV_VICTORY ) ? 1 : 0;
	runSec = ( level.time - nw_runStartTime ) / 1000;
	kills = NW_Cache()->kills;

	// reset per-run bitmask (defensive; NeonWave_Reset already clears)
	{
		int ai;
		for ( ai = 0; ai < NW_ACH_COUNT; ai++ ) {
			nw_achievements[ai] = qfalse;
		}
	}
	if ( victory ) {
		nw_achievements[ NW_ACH_FIRST_VICTORY ] = qtrue;
	}
	if ( nw_wave >= 15 ) {
		nw_achievements[ NW_ACH_SURVIVOR ] = qtrue;
	}
	if ( combo >= 5 ) {
		nw_achievements[ NW_ACH_STREAKER ] = qtrue;
	}
	if ( combo >= 8 ) {
		nw_achievements[ NW_ACH_SHARPSHOOTER ] = qtrue;
	}
	if ( victory && deaths == 0 ) {
		nw_achievements[ NW_ACH_FLAWLESS ] = qtrue;
	}
	if ( combo >= 12 ) {
		nw_achievements[ NW_ACH_COMBOMASTER ] = qtrue;
	}
	if ( victory && runSec <= 300 ) {
		nw_achievements[ NW_ACH_SPEEDRUNNER ] = qtrue;
	}
	if ( victory && nw_hardcore ) {
		nw_achievements[ NW_ACH_HARDCORE ] = qtrue;
	}
	// v0.60: new achievements
	if ( kills >= 100 ) {
		nw_achievements[ NW_ACH_KILL_100 ] = qtrue;
	}
	if ( kills >= 1000 ) {
		nw_achievements[ NW_ACH_KILL_1000 ] = qtrue;
	}
	if ( nw_wave >= 5 ) {
		nw_achievements[ NW_ACH_WAVE_5 ] = qtrue;
	}
	if ( nw_wave >= 10 ) {
		nw_achievements[ NW_ACH_WAVE_10 ] = qtrue;
	}
	if ( nw_wave >= 30 ) {
		nw_achievements[ NW_ACH_WAVE_30 ] = qtrue;
	}
	if ( nw_wave >= 50 ) {
		nw_achievements[ NW_ACH_WAVE_50 ] = qtrue;
	}
	if ( nw_untouchableWave ) {
		nw_achievements[ NW_ACH_PERFECT_WAVE ] = qtrue;
	}
	if ( nw_multikillCount >= 3 ) {
		nw_achievements[ NW_ACH_MULTIKILL_3 ] = qtrue;
	}
	if ( nw_multikillCount >= 5 ) {
		nw_achievements[ NW_ACH_MULTIKILL_5 ] = qtrue;
	}

	for ( i = 0; i < NW_ACH_COUNT; i++ ) {
		if ( !nw_achievements[i] ) {
			continue;
		}
		G_Printf( "NeonWave: ACHIEVEMENT %s\n", NW_AchievementName( i ) );
		if ( !nw_achEver[i] ) {
			nw_achEver[i] = qtrue;
			G_Printf( "NeonWave: ACHIEVEMENT UNLOCKED %s\n", NW_AchievementName( i ) );
			// Notify clients via cvar
			trap_Cvar_Set( "ui_neonwave_achievement", NW_AchievementName( i ) );
		}
	}
	NW_SaveAchievements();
}

#endif // NEONARENA_MOD
