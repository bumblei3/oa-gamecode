// NeonArena seasonal challenge system
// Weekly challenges with leaderboard and rewards.
#include "g_local.h"
#include "g_neonwave.h"

#ifdef NEONARENA_MOD

#define NW_SEASONAL_FILE		"neonwave_seasonal.dat"
#define NW_SEASONAL_LEADERBOARD	"neonwave_seasonal_leaderboard.json"
#define NW_SEASONAL_MAX_ENTRIES	10

// ---- challenge definitions ----
// Each challenge is a weekly rotation based on (tm_yday / 7) % NW_CHALLENGE_COUNT
typedef struct {
	const char *title;		// display name
	const char *desc;		// description
	int			modRequired;	// required modifier (NW_MOD_*), -1 = any
	int			mod2Required;	// second required modifier, -1 = none
	int			targetWave;		// wave to reach/clear
	qboolean	needVictory;	// qtrue = must win run, qfalse = just reach wave
	const char *reward;		// reward title
} nwChallenge_t;

static const nwChallenge_t nwChallenges[] = {
	// modifier-specific challenges
	{ "FROSTBITE",		"Clear wave 8 with FROST active",			NW_MOD_FROST,		-1,			8,	qfalse,	"ICE BREAKER"		},
	{ "BLOOD RUSH",		"Clear wave 8 with VAMPIRE active",		NW_MOD_VAMPIRE,		-1,			8,	qfalse,	"VAMPIRE LORD"		},
	{ "GLASS CANNON",	"Clear wave 8 with GLASS DRONES active",	NW_MOD_GLASS,		-1,			8,	qfalse,	"GLASS MASTER"		},
	{ "CHAOS THEORY",	"Clear wave 8 with CHAOS active",			NW_MOD_CHAOS,		-1,			8,	qfalse,	"CHAOS WALKER"		},
	{ "MIRROR MATCH",	"Clear wave 8 with MIRROR active",			NW_MOD_MIRROR,		-1,			8,	qfalse,	"REFLECTOR"			},
	{ "FROZEN CHAOS",	"Clear wave 10 with FROST + CHAOS",			NW_MOD_FROST,		NW_MOD_CHAOS, 10,	qfalse,	"STORM BRINGER"		},
	{ "OVERDRIVE",		"Clear wave 10 with FRENZY + SURGE",		NW_MOD_FRENZY,		NW_MOD_SURGE, 10,	qfalse,	"OVERCHARGED"		},
	{ "SWARM SURVIVOR",	"Clear wave 10 with SWARM active",			NW_MOD_SWARM,		-1,			10,	qfalse,	"SWARM COMMANDER"	},
	{ "SPEED DEMON",	"Win a run with TIME WARP active",			NW_MOD_TIMEWARP,	-1,			20,	qtrue,	"SPEEDRUNNER"		},
	{ "IRONMAN",		"Win a run with OVERSHIELD active",			NW_MOD_OVERSHIELD,	-1,			20,	qtrue,	"IRONCLAD"			},
	{ "LOW GRAVITY",	"Clear wave 12 with LOW GRAVITY active",	NW_MOD_LOWGRAV,		-1,			12,	qfalse,	"MOONWALKER"		},
	{ "REGEN MASTER",	"Clear wave 12 with REGEN active",			NW_MOD_REGEN,		-1,			12,	qfalse,	"IMMORTAL"			},
	{ "DOUBLE OR NOTHING", "Clear wave 12 with DOUBLE POINTS",		NW_MOD_DOUBLEPTS,	-1,			12,	qfalse,	"GAMBLER"			},
	{ "SHIELD WALL",	"Clear wave 12 with SHIELD active",			NW_MOD_SHIELD,		-1,			12,	qfalse,	"GUARDIAN"			},
	{ "MIMICRY",		"Clear wave 10 with MIMIC active",			NW_MOD_MIMIC,		-1,			10,	qfalse,	"SHAPESHIFTER"		},
	{ "AERIAL ASSAULT",	"Clear wave 10 with AERIAL ASSAULT synergy",NW_MOD_LOWGRAV,		NW_MOD_DOUBLEPTS, 10, qfalse,	"SKY RAIDER"	},
};
#define NW_CHALLENGE_COUNT (int)(sizeof(nwChallenges)/sizeof(nwChallenges[0]))

// ---- seasonal state ----
typedef struct {
	int		week;				// week number (tm_yday / 7)
	int		challengeIdx;		// current challenge index
	int		progress;			// current progress (wave reached)
	qboolean completed;			// challenge completed this week
	qboolean rewardClaimed;		// reward claimed
} nwSeasonalState_t;

static nwSeasonalState_t nw_seasonal;

// ---- leaderboard entry ----
typedef struct {
	char	name[64];
	int		week;
	int		challengeIdx;
	int		score;			// wave reached or time
	qboolean victory;
	char	date[16];		// YYYY-MM-DD
} nwLeaderboardEntry_t;

static nwLeaderboardEntry_t nw_leaderboard[NW_SEASONAL_MAX_ENTRIES];
static int nw_leaderboardCount = 0;

// ---- local prototypes ----
static int NW_WeekNumber( void );
static void NW_LoadSeasonal( void );
static void NW_SaveSeasonal( void );
static void NW_LoadLeaderboard( void );
static void NW_SaveLeaderboard( void );
static int NW_ScoreEntry( int wave, qboolean victory, int timeSec );

// ---- week number ----
static int NW_WeekNumber( void ) {
	qtime_t tm;
	trap_RealTime( &tm );
	return ( tm.tm_yday / 7 ) % NW_CHALLENGE_COUNT;
}

// ---- load/save seasonal state ----
static void NW_LoadSeasonal( void ) {
	fileHandle_t f;
	// default: no progress
	memset( &nw_seasonal, 0, sizeof( nw_seasonal ) );
	nw_seasonal.challengeIdx = -1;
	if ( trap_FS_FOpenFile( NW_SEASONAL_FILE, &f, FS_READ ) >= 0 && f ) {
		trap_FS_Read( &nw_seasonal, sizeof( nw_seasonal ), f );
		trap_FS_FCloseFile( f );
	}
	// check if week changed
	{
		int curWeek = NW_WeekNumber();
		if ( nw_seasonal.week != curWeek ) {
			// new week: rotate challenge
			nw_seasonal.week = curWeek;
			nw_seasonal.challengeIdx = curWeek;
			nw_seasonal.progress = 0;
			nw_seasonal.completed = qfalse;
			nw_seasonal.rewardClaimed = qfalse;
			NW_SaveSeasonal();
		}
	}
	G_Printf( "NeonWave: SEASONAL challenge %s (week %i, progress %i/%i, %s)\n",
		nwChallenges[nw_seasonal.challengeIdx].title,
		nw_seasonal.week, nw_seasonal.progress,
		nwChallenges[nw_seasonal.challengeIdx].targetWave,
		nw_seasonal.completed ? "COMPLETED" : "active" );
}

static void NW_SaveSeasonal( void ) {
	fileHandle_t f;
	int len = trap_FS_FOpenFile( NW_SEASONAL_FILE, &f, FS_WRITE );
	if ( len < 0 || !f ) {
		G_Printf( "NeonWave: WARNING cannot write " NW_SEASONAL_FILE "\n" );
		return;
	}
	trap_FS_Write( &nw_seasonal, sizeof( nw_seasonal ), f );
	trap_FS_FCloseFile( f );
}

// ---- load/save leaderboard ----
static void NW_LoadLeaderboard( void ) {
	fileHandle_t f;
	nw_leaderboardCount = 0;
	memset( nw_leaderboard, 0, sizeof( nw_leaderboard ) );
	if ( trap_FS_FOpenFile( NW_SEASONAL_LEADERBOARD, &f, FS_READ ) >= 0 && f ) {
		trap_FS_Read( &nw_leaderboardCount, sizeof( nw_leaderboardCount ), f );
		if ( nw_leaderboardCount > NW_SEASONAL_MAX_ENTRIES ) {
			nw_leaderboardCount = NW_SEASONAL_MAX_ENTRIES;
		}
		if ( nw_leaderboardCount > 0 ) {
			trap_FS_Read( nw_leaderboard, sizeof( nwLeaderboardEntry_t ) * nw_leaderboardCount, f );
		}
		trap_FS_FCloseFile( f );
	}
}

static void NW_SaveLeaderboard( void ) {
	fileHandle_t f;
	int len = trap_FS_FOpenFile( NW_SEASONAL_LEADERBOARD, &f, FS_WRITE );
	if ( len < 0 || !f ) {
		G_Printf( "NeonWave: WARNING cannot write " NW_SEASONAL_LEADERBOARD "\n" );
		return;
	}
	trap_FS_Write( &nw_leaderboardCount, sizeof( nw_leaderboardCount ), f );
	if ( nw_leaderboardCount > 0 ) {
		trap_FS_Write( nw_leaderboard, sizeof( nwLeaderboardEntry_t ) * nw_leaderboardCount, f );
	}
	trap_FS_FCloseFile( f );
}

// ---- score calculation ----
static int NW_ScoreEntry( int wave, qboolean victory, int timeSec ) {
	// score = wave * 1000 + (victory ? 500 : 0) - timeSec
	// higher is better
	int score = wave * 1000;
	if ( victory ) score += 500;
	score -= timeSec;
	return score;
}

// ---- public: initialize seasonal system ----
void NW_SeasonalInit( void ) {
	NW_LoadSeasonal();
	NW_LoadLeaderboard();
	// mirror to cvars for UI
	{
		const nwChallenge_t *ch = &nwChallenges[nw_seasonal.challengeIdx];
		trap_Cvar_Set( "ui_neonwave_seasonal_title", ch->title );
		trap_Cvar_Set( "ui_neonwave_seasonal_desc", ch->desc );
		trap_Cvar_Set( "ui_neonwave_seasonal_target", va( "%i", ch->targetWave ) );
		trap_Cvar_Set( "ui_neonwave_seasonal_progress", va( "%i", nw_seasonal.progress ) );
		trap_Cvar_Set( "ui_neonwave_seasonal_completed", nw_seasonal.completed ? "1" : "0" );
		trap_Cvar_Set( "ui_neonwave_seasonal_reward", ch->reward );
		trap_Cvar_Set( "ui_neonwave_seasonal_week", va( "%i", nw_seasonal.week ) );
	}
	G_Printf( "NeonWave: SEASONAL CHALLENGE %s (week %i, target %i, progress %i, %s)\n",
		nwChallenges[nw_seasonal.challengeIdx].title,
		nw_seasonal.week,
		nwChallenges[nw_seasonal.challengeIdx].targetWave,
		nw_seasonal.progress,
		nw_seasonal.completed ? "COMPLETED" : "active" );
}

// ---- public: update progress (called every wave clear) ----
void NW_SeasonalProgress( int wave ) {
	if ( nw_seasonal.completed ) return;
	if ( wave > nw_seasonal.progress ) {
		nw_seasonal.progress = wave;
	}
	NW_SaveSeasonal();
	trap_Cvar_Set( "ui_neonwave_seasonal_progress", va( "%i", nw_seasonal.progress ) );
}

// ---- public: check challenge completion (called on game over) ----
void NW_SeasonalCheck( int event ) {
	const nwChallenge_t *ch = &nwChallenges[nw_seasonal.challengeIdx];
	qboolean victory = ( event == NW_EV_VICTORY ) ? qtrue : qfalse;
	int timeSec = ( level.time - NW_GetRunStartTime() ) / 1000;

	if ( nw_seasonal.completed ) return;

	// check wave requirement
	if ( nw_seasonal.progress < ch->targetWave ) return;

	// check victory requirement
	if ( ch->needVictory && !victory ) return;

	// check modifier requirement
	if ( ch->modRequired >= 0 ) {
		if ( !( NW_GetModifiersSeen() & ( 1 << ch->modRequired ) ) ) return;
	}
	if ( ch->mod2Required >= 0 ) {
		if ( !( NW_GetModifiersSeen() & ( 1 << ch->mod2Required ) ) ) return;
	}

	// challenge completed!
	nw_seasonal.completed = qtrue;
	nw_seasonal.rewardClaimed = qtrue;
	NW_SaveSeasonal();

	trap_Cvar_Set( "ui_neonwave_seasonal_completed", "1" );
	trap_Cvar_Set( "ui_neonwave_seasonal_reward", ch->reward );

	G_Printf( "NeonWave: SEASONAL CHALLENGE COMPLETED! %s -> Reward: %s\n",
		ch->title, ch->reward );

	// add to leaderboard
	{
		nwLeaderboardEntry_t *e;
		int score = NW_ScoreEntry( nw_seasonal.progress, victory, timeSec );
		int i;
		int insert;
		qtime_t tm;
		trap_RealTime( &tm );

		// find insert position
		insert = nw_leaderboardCount;
		for ( i = 0; i < nw_leaderboardCount; i++ ) {
			if ( score > nw_leaderboard[i].score ) {
				insert = i;
				break;
			}
		}

		if ( insert < NW_SEASONAL_MAX_ENTRIES ) {
			// shift down
			for ( i = NW_SEASONAL_MAX_ENTRIES - 1; i > insert; i-- ) {
				nw_leaderboard[i] = nw_leaderboard[i-1];
			}
			e = &nw_leaderboard[insert];
			// get player name from first human client
			{
				int ci;
				for ( ci = 0; ci < MAX_CLIENTS; ci++ ) {
					gentity_t *ent = &g_entities[ci];
					if ( !ent->inuse || !ent->client ) continue;
					if ( ent->r.svFlags & SVF_BOT ) continue;
					if ( ent->client->pers.connected != CON_CONNECTED ) continue;
					Q_strncpyz( e->name, ent->client->pers.netname, sizeof(e->name) );
					break;
				}
			}
			e->week = nw_seasonal.week;
			e->challengeIdx = nw_seasonal.challengeIdx;
			e->score = score;
			e->victory = victory;
			Com_sprintf( e->date, sizeof(e->date ), "%04i-%02i-%02i",
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday );

			if ( nw_leaderboardCount < NW_SEASONAL_MAX_ENTRIES ) {
				nw_leaderboardCount++;
			}
			NW_SaveLeaderboard();

			// mirror leaderboard to cvars (top 5)
			for ( i = 0; i < 5 && i < nw_leaderboardCount; i++ ) {
				trap_Cvar_Set( va( "ui_neonwave_lb_%i_name", i ), nw_leaderboard[i].name );
				trap_Cvar_Set( va( "ui_neonwave_lb_%i_score", i ), va( "%i", nw_leaderboard[i].score ) );
				trap_Cvar_Set( va( "ui_neonwave_lb_%i_victory", i ), nw_leaderboard[i].victory ? "1" : "0" );
			}
		}
	}
}

// ---- public: get leaderboard for UI ----
int NW_SeasonalLeaderboardCount( void ) {
	return nw_leaderboardCount;
}

const char *NW_SeasonalLeaderboardName( int idx ) {
	if ( idx < 0 || idx >= nw_leaderboardCount ) return "";
	return nw_leaderboard[idx].name;
}

int NW_SeasonalLeaderboardScore( int idx ) {
	if ( idx < 0 || idx >= nw_leaderboardCount ) return 0;
	return nw_leaderboard[idx].score;
}

int NW_SeasonalLeaderboardVictory( int idx ) {
	if ( idx < 0 || idx >= nw_leaderboardCount ) return 0;
	return nw_leaderboard[idx].victory ? 1 : 0;
}

// ---- public: get current challenge info ----
const char *NW_SeasonalChallengeTitle( void ) {
	return nwChallenges[nw_seasonal.challengeIdx].title;
}

const char *NW_SeasonalChallengeDesc( void ) {
	return nwChallenges[nw_seasonal.challengeIdx].desc;
}

int NW_SeasonalChallengeTarget( void ) {
	return nwChallenges[nw_seasonal.challengeIdx].targetWave;
}

int NW_SeasonalProgressLevel( void ) {
	return nw_seasonal.progress;
}

qboolean NW_SeasonalCompleted( void ) {
	return nw_seasonal.completed;
}

const char *NW_SeasonalReward( void ) {
	return nwChallenges[nw_seasonal.challengeIdx].reward;
}

#endif // NEONARENA_MOD
