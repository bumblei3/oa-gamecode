// g_nw_perks.c - NeonWave perks (per-client cards, offers, break-window shop)
// Split out of g_neonwave.c. Perk names/caps, the per-run perk stack
// (nw_perk), the break-window offer roll and the buy path. Shared run
// state comes via g_nw_internal.h.
#include "g_local.h"
#include "g_neonwave.h"
#include "g_nw_internal.h"

#ifdef NEONARENA_MOD

// perk state (owned by this module; extern in g_nw_internal.h)
int nw_perk[ MAX_CLIENTS ][ NW_PERK_COUNT ];	// per-client stacks/charges
int nw_offer[ MAX_CLIENTS ][ 3 ];		// per-client break-window perk cards

const char *NW_PerkName( int id ) {
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

int NW_PerkCap( int id ) {
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

void NW_RollOffers( int clientID ) {
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
		va( "cp \"PERK: %s\n\"", NW_PerkName( id ) ) );
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
	trap_SendServerCommand( -1, "cp \"SECOND WIND\n\"" );
	NeonWave_PerkFx( "secondwind" );
	NW_MirrorPerks( clientID );
	return qtrue;
}

#endif // NEONARENA_MOD
