// g_nw_modifiers.c - NeonWave wave modifiers (pool pick + synergy pairs)
// Split out of g_neonwave.c. Picks the per-wave modifier(s), publishes the
// active bitmask for the MIRROR reflect hook in g_combat.c, and evaluates
// the named synergy/anti-synergy pairs. Shared state via g_nw_internal.h.
#include "g_local.h"
#include "g_neonwave.h"
#include "g_nw_internal.h"

#ifdef NEONARENA_MOD

// modifier state (owned by this module; extern in g_nw_internal.h)
int nw_modifier = NW_MOD_NONE;
int nw_modifier2 = NW_MOD_NONE;	// second synergy modifier slot (wave >= 8)
int nw_synergyIdx = -1;		// index into nwSynergies, -1 = none
int nw_ptsMul = 1;		// wave-clear point multiplier (DOUBLEPTS/SURGE/AERIAL)
int nw_dailyOffset = 0;		// modifier pool rotation 0..(NW_MOD_POOL_SIZE-1)
int nw_modifiersSeen = 0;	// bitmask of modifiers encountered this run

static void NW_PublishModifiers( void );
static void NW_ApplySynergy( void );

typedef struct {
	int a;
	int b;
	const char *name;
	qboolean anti; // qtrue = anti-synergy (penalty), qfalse = synergy (bonus)
} nwSynergy_t;

static const nwSynergy_t nwSynergies[] = {
	{ NW_MOD_LOWGRAV, NW_MOD_DOUBLEPTS, "AERIAL ASSAULT", qfalse },
	{ NW_MOD_VAMPIRE, NW_MOD_REGEN,    "BLOOD WELL",     qfalse },
	{ NW_MOD_FRENZY,  NW_MOD_SURGE,    "OVERDRIVE",      qfalse },
	{ NW_MOD_SWARM,   NW_MOD_MIRROR,   "HIVE MIRROR",    qfalse },
	// anti-synergies: two defensive/conflicting mods weaken each other
	{ NW_MOD_OVERSHIELD, NW_MOD_VAMPIRE, "SHIELD BLEED",  qtrue  },
	{ NW_MOD_TIMEWARP,   NW_MOD_LOWGRAV, "DRIFT LOCK",    qtrue  },
};
#define NW_SYNERGY_COUNT (int)(sizeof(nwSynergies)/sizeof(nwSynergies[0]))

const char *NW_SynergyName( int idx ) {
	if ( idx < 0 || idx >= NW_SYNERGY_COUNT ) {
		return "";
	}
	return nwSynergies[idx].name;
}

// Publish the active modifier(s) to a cvar bitmask so g_combat.c (MIRROR reflect
// hook) sees MIRROR regardless of which slot it landed in. Call before every
// NW_PickModifier return path — the force/skip branches used to skip this.
static void NW_PublishModifiers( void ) {
	int nwActiveMask = 0;
	if ( nw_modifier != NW_MOD_NONE )  nwActiveMask |= ( 1 << nw_modifier );
	if ( nw_modifier2 != NW_MOD_NONE ) nwActiveMask |= ( 1 << nw_modifier2 );
	trap_Cvar_Set( "g_neonwave_modifier_active", va( "%i", nwActiveMask ) );
	if ( nwActiveMask & ( 1 << NW_MOD_MIRROR ) ) {
		G_Printf( "NeonWave: MIRROR active (mask %i, slot2=%i)\n",
			nwActiveMask, ( nw_modifier2 == NW_MOD_MIRROR ) ? 1 : 0 );
	}
}

void NW_PickModifier( int num ) {
	static const int pool[NW_MOD_POOL_SIZE] = {
		NW_MOD_GLASS, NW_MOD_SWARM, NW_MOD_LOWGRAV, NW_MOD_DOUBLEPTS,
		NW_MOD_TIMEWARP, NW_MOD_VAMPIRE, NW_MOD_FRENZY, NW_MOD_OVERSHIELD,
		NW_MOD_MIRROR, NW_MOD_REGEN, NW_MOD_SURGE, NW_MOD_FROST, NW_MOD_CHAOS,
		NW_MOD_MIMIC, NW_MOD_MIMIC, NW_MOD_SHIELD
	};
	int idx;
	int maxWave;
	char mbBuf[8];
	char mb2Buf[8];
	char mwBuf[8];

	nw_modifier = NW_MOD_NONE;
	nw_modifier2 = NW_MOD_NONE;
	nw_synergyIdx = -1;
	nw_ptsMul = 1;
	trap_Cvar_Set( "ui_neonwave_synergy", "" );
	trap_Cvar_Set( "ui_neonwave_synergyanti", "0" );
	trap_Cvar_Set( "g_neonwave_vampheal", "0" );
	trap_Cvar_Set( "g_neonwave_mirrordiv", "3" );
	trap_Cvar_VariableStringBuffer( "g_neonwave_maxwave", mwBuf, sizeof(mwBuf) );
	maxWave = atoi( mwBuf );
	if ( maxWave <= 0 ) {
		maxWave = NW_MAX_WAVE;
	}
	// v0.80: seasonal rotation — weekly offset for modifier variety
	{
		qtime_t st;
		char seasBuf[8];
		int seasonalOffset = 0;
		trap_Cvar_VariableStringBuffer( "g_neonwave_seasonal", seasBuf, sizeof(seasBuf) );
		if ( atoi( seasBuf ) == 1 ) {
			trap_RealTime( &st );
			seasonalOffset = ( st.tm_yday / 7 ) % NW_MOD_POOL_SIZE;
			G_Printf( "NeonWave: SEASONAL rotation week %i (offset %i)\n", (st.tm_yday / 7), seasonalOffset );
		}
		nw_dailyOffset = ( nw_dailyOffset + seasonalOffset ) % NW_MOD_POOL_SIZE;
	}
	// waves 5 .. max-1 (boss waves included) so a classic run sees the full pool
	if ( num < 5 || num >= maxWave ) {
		return;
	}
	// test hooks (v0.35): g_neonwave_modifier N forces slot 1, g_neonwave_modifier2 N
	// forces slot 2. Each works independently so MIRROR can be forced in either slot.
	trap_Cvar_VariableStringBuffer( "g_neonwave_modifier", mbBuf, sizeof(mbBuf) );
	if ( atoi( mbBuf ) >= NW_MOD_GLASS && atoi( mbBuf ) < NW_MOD_POOL_SIZE ) {
		nw_modifier = atoi( mbBuf );
	}
	trap_Cvar_VariableStringBuffer( "g_neonwave_modifier2", mb2Buf, sizeof( mb2Buf ) );
	if ( atoi( mb2Buf ) >= NW_MOD_GLASS && atoi( mb2Buf ) < NW_MOD_POOL_SIZE ) {
		nw_modifier2 = atoi( mb2Buf );
	}
	if ( nw_modifier != NW_MOD_NONE || nw_modifier2 != NW_MOD_NONE ) {
		NW_PublishModifiers();
		NW_ApplySynergy();
		return;
	}
	// SKIP perk: check if any connected human client has it (coop-aware)
	{
		int cid;
		gentity_t *ent;
		for ( cid = 0; cid < MAX_CLIENTS; cid++ ) {
			ent = &g_entities[cid];
			if ( !ent->inuse || !ent->client ) continue;
			if ( ent->client->pers.connected != CON_CONNECTED ) continue;
			if ( ent->r.svFlags & SVF_BOT ) continue;
			if ( nw_perk[cid][ NW_PERK_SKIP ] > 0 ) {
				nw_perk[cid][ NW_PERK_SKIP ]--;
				nw_modifier = NW_MOD_NONE;
				G_Printf( "NeonWave: SKIP modifier (client %i)\n", cid );
				NW_MirrorPerks( cid );
				NW_PublishModifiers();
				return;
			}
		}
	}
	idx = ( num - 5 + nw_dailyOffset ) % NW_MOD_POOL_SIZE;
	if ( idx < 0 ) {
		idx += NW_MOD_POOL_SIZE;
	}
	nw_modifier = pool[idx];
	// v0.35: second synergy modifier from wave 8 onward (different from slot 1)
	if ( num >= 8 ) {
		int idx2 = ( idx + 4 + nw_dailyOffset ) % NW_MOD_POOL_SIZE;
		nw_modifier2 = pool[idx2];
		if ( nw_modifier2 == nw_modifier ) {
			nw_modifier2 = pool[( idx2 + 1 ) % NW_MOD_POOL_SIZE];
		}
	}
	// publish active modifiers (bitmask cvar) for the MIRROR reflect hook in g_combat.c.
	// Covers both slots (v0.35 second modifier).
	NW_PublishModifiers();
	NW_ApplySynergy();
}

const char *NW_ModifierName( int mod ) {
	switch ( mod ) {
	case NW_MOD_GLASS:		return "GLASS DRONES";
	case NW_MOD_SWARM:		return "SWARM";
	case NW_MOD_LOWGRAV:	return "LOW GRAVITY";
	case NW_MOD_DOUBLEPTS:	return "DOUBLE POINTS";
	case NW_MOD_TIMEWARP:	return "TIME WARP";
	case NW_MOD_VAMPIRE:	return "VAMPIRE";
	case NW_MOD_FRENZY:	return "FRENZY";
	case NW_MOD_OVERSHIELD:	return "OVERSHIELD";
	case NW_MOD_MIRROR:		return "MIRROR";
	case NW_MOD_REGEN:		return "REGEN";
	case NW_MOD_SURGE:		return "SURGE";
	case NW_MOD_FROST:		return "FROST";
	case NW_MOD_CHAOS:		return "CHAOS";
	case NW_MOD_MIMIC:		return "MIMIC";
	case NW_MOD_SHIELD:		return "SHIELD";
	default:			return "";
	}
}

qboolean NW_ModActive( int mod ) {
	return ( nw_modifier == mod || nw_modifier2 == mod ) ? qtrue : qfalse;
}

static void NW_ApplySynergy( void ) {
	int i;
	nw_synergyIdx = -1;
	trap_Cvar_Set( "ui_neonwave_synergy", "" );
	trap_Cvar_Set( "ui_neonwave_synergyanti", "0" );
	if ( nw_modifier2 == NW_MOD_NONE ) {
		return; // no second modifier -> nothing to pair
	}
	for ( i = 0; i < NW_SYNERGY_COUNT; i++ ) {
		int a = nwSynergies[i].a;
		int b = nwSynergies[i].b;
		qboolean match = ( ( nw_modifier == a && nw_modifier2 == b )
			|| ( nw_modifier == b && nw_modifier2 == a ) );
		if ( match ) {
			nw_synergyIdx = i;
			trap_Cvar_Set( "ui_neonwave_synergy", nwSynergies[i].name );
			trap_Cvar_Set( "ui_neonwave_synergyanti", nwSynergies[i].anti ? "1" : "0" );
			if ( nwSynergies[i].anti ) {
				G_Printf( "NeonWave: ANTI-SYNERGY %s (%s + %s)\n",
					nwSynergies[i].name,
					NW_ModifierName( nw_modifier ),
					NW_ModifierName( nw_modifier2 ) );
			} else {
				G_Printf( "NeonWave: SYNERGY %s (%s + %s)\n",
					nwSynergies[i].name,
					NW_ModifierName( nw_modifier ),
					NW_ModifierName( nw_modifier2 ) );
			}
			return;
		}
	}
}

#endif // NEONARENA_MOD
