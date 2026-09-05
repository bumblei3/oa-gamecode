// StarCraft-inspired Ghost kit for NeonArena (OpenArena).
// Activate with g_neonwave_ghost 1. Commands: cloak, emp, nuke.
#include "g_local.h"

#ifdef NEONARENA_MOD

#define GH_ENERGY_MAX		100
#define GH_ENERGY_START		40
#define GH_ENERGY_KILL		15
#define GH_REGEN_MS		1000
#define GH_REGEN_AMT		3
#define GH_CLOAK_COST		40
#define GH_CLOAK_MS		5000
#define GH_EMP_COST		35
#define GH_EMP_CD		25000
#define GH_EMP_RADIUS		400
#define GH_EMP_STUN		1500
#define GH_NUKE_COST		80
#define GH_NUKE_CD		45000
#define GH_NUKE_PAINT		1500
#define GH_NUKE_INBOUND		4000
#define GH_NUKE_RADIUS		600
#define GH_NUKE_BOSS_PCT	40
#define GH_NUKE_SELF_DMG	40
#define GH_MOVE_CANCEL		24
#define GH_DETECT_RANGE		400
#define GH_SWARM_MS		4000
#define GH_FLEE_SPEED		420

static int gh_energy[MAX_CLIENTS];
static int gh_lastRegen[MAX_CLIENTS];
static int gh_empUntil[MAX_CLIENTS];
static int gh_stunUntil[MAX_CLIENTS];
static int gh_nukeCdUntil[MAX_CLIENTS];
static int gh_paintUntil[MAX_CLIENTS];
static int gh_boomAt[MAX_CLIENTS];
static int gh_lastNukeSec[MAX_CLIENTS];
static int gh_lastLaser[MAX_CLIENTS];
static vec3_t gh_nukePos[MAX_CLIENTS];
static vec3_t gh_paintOrigin[MAX_CLIENTS];
static int gh_swarmUntil;

static qboolean GH_CvarOn( void ) {
	char buf[8];
	trap_Cvar_VariableStringBuffer( "g_neonwave_ghost", buf, sizeof(buf) );
	return atoi( buf ) != 0;
}

qboolean NW_GhostActive( void ) {
	return ( g_gametype.integer == GT_NEONWAVE ) && GH_CvarOn();
}

qboolean NW_GhostSwarmActive( void ) {
	return gh_swarmUntil > level.time;
}

qboolean NW_GhostSeesInvis( gentity_t *viewer ) {
	if ( !NW_GhostActive() ) {
		return qfalse;
	}
	if ( gh_swarmUntil > level.time ) {
		return qtrue;
	}
	if ( !viewer || !viewer->client ) {
		return qfalse;
	}
	if ( viewer->client->pers.neonwaveDetector ) {
		return qtrue;
	}
	if ( viewer->client->pers.neonwaveBoss && NW_BossPhase() >= 2 ) {
		return qtrue;
	}
	return qfalse;
}

static int GH_Id( gentity_t *ent ) {
	return (int)( ent - g_entities );
}

void NW_GhostBreakCloak( gentity_t *ent ) {
	int id;
	if ( !ent || !ent->client ) {
		return;
	}
	if ( !NW_GhostActive() ) {
		return;
	}
	id = GH_Id( ent );
	if ( id < 0 || id >= MAX_CLIENTS ) {
		return;
	}
	ent->client->ps.powerups[PW_INVIS] = 0;
	if ( gh_paintUntil[id] > level.time ) {
		gh_paintUntil[id] = 0;
		trap_SendServerCommand( id, "cp \"NUKE CANCELLED\n\"" );
	}
}

void NW_GhostSpawn( gentity_t *ent ) {
	int id;
	if ( !ent || !ent->client ) {
		return;
	}
	if ( !NW_GhostActive() ) {
		return;
	}
	id = GH_Id( ent );
	gh_energy[id] = GH_ENERGY_START;
	gh_lastRegen[id] = level.time;
	gh_empUntil[id] = 0;
	gh_nukeCdUntil[id] = 0;
	gh_paintUntil[id] = 0;
	gh_boomAt[id] = 0;
	gh_lastNukeSec[id] = 0;
	ent->client->ps.powerups[PW_INVIS] = 0;
}

void NW_GhostOnKill( gentity_t *attacker ) {
	int id;
	if ( !NW_GhostActive() ) {
		return;
	}
	if ( !attacker || !attacker->client ) {
		return;
	}
	if ( attacker->r.svFlags & SVF_BOT ) {
		return;
	}
	id = GH_Id( attacker );
	gh_energy[id] += GH_ENERGY_KILL;
	if ( gh_energy[id] > GH_ENERGY_MAX ) {
		gh_energy[id] = GH_ENERGY_MAX;
	}
}

static void GH_SyncHud( gentity_t *ent ) {
	int id, cloakLeft, empCd, nukeCd;
	id = GH_Id( ent );
	cloakLeft = 0;
	if ( ent->client->ps.powerups[PW_INVIS] > level.time ) {
		cloakLeft = ent->client->ps.powerups[PW_INVIS] - level.time;
	}
	empCd = gh_empUntil[id] - level.time;
	if ( empCd < 0 ) empCd = 0;
	nukeCd = gh_nukeCdUntil[id] - level.time;
	if ( nukeCd < 0 ) nukeCd = 0;
	trap_Cvar_Set( "g_ghost_energy", va( "%i", gh_energy[id] ) );
	trap_Cvar_Set( "g_ghost_cloakms", va( "%i", cloakLeft ) );
	trap_Cvar_Set( "g_ghost_empcd", va( "%i", empCd ) );
	trap_Cvar_Set( "g_ghost_nukecd", va( "%i", nukeCd ) );
	if ( gh_paintUntil[id] > level.time ) {
		trap_Cvar_Set( "g_ghost_status", "DESIGNATING" );
	} else if ( gh_boomAt[id] > level.time ) {
		trap_Cvar_Set( "g_ghost_status", va( "NUKE %i", ( gh_boomAt[id] - level.time + 999 ) / 1000 ) );
	} else if ( gh_swarmUntil > level.time ) {
		trap_Cvar_Set( "g_ghost_status", "DETECTED" );
	} else if ( cloakLeft > 0 ) {
		trap_Cvar_Set( "g_ghost_status", "CLOAKED" );
	} else {
		trap_Cvar_Set( "g_ghost_status", "" );
	}
}

static void GH_NukeLaser( int id ) {
	gentity_t *tent;
	vec3_t sky;
	if ( level.time - gh_lastLaser[id] < 200 ) {
		return;
	}
	gh_lastLaser[id] = level.time;
	VectorCopy( gh_nukePos[id], sky );
	sky[2] += 1024;
	tent = G_TempEntity( gh_nukePos[id], EV_RAILTRAIL );
	VectorCopy( sky, tent->s.origin2 );
	tent->s.eventParm = 255;
	tent->s.clientNum = id;
}

static void GH_FleeBots( int id ) {
	int i;
	gentity_t *bot;
	vec3_t dir;
	float dist;
	for ( i = 0; i < level.maxclients; i++ ) {
		bot = &g_entities[i];
		if ( !bot->inuse || !bot->client ) continue;
		if ( !( bot->r.svFlags & SVF_BOT ) ) continue;
		if ( bot->health <= 0 ) continue;
		VectorSubtract( bot->r.currentOrigin, gh_nukePos[id], dir );
		dist = VectorLength( dir );
		if ( dist < 8.0f ) {
			dir[0] = 1;
			dir[1] = 0;
			dir[2] = 0;
			dist = 1;
		}
		if ( dist > GH_NUKE_RADIUS + 192 ) {
			continue;
		}
		VectorNormalize( dir );
		VectorScale( dir, GH_FLEE_SPEED, bot->client->ps.velocity );
		bot->client->ps.velocity[2] += 60;
	}
}

void Cmd_GhostCloak_f( gentity_t *ent ) {
	int id;
	if ( !NW_GhostActive() ) {
		trap_SendServerCommand( ent - g_entities, "print \"Ghost kit off (g_neonwave_ghost 0)\n\"" );
		return;
	}
	if ( !ent->client || ent->health <= 0 ) {
		return;
	}
	id = GH_Id( ent );
	if ( ent->client->ps.powerups[PW_INVIS] > level.time ) {
		return;
	}
	if ( gh_energy[id] < GH_CLOAK_COST ) {
		trap_SendServerCommand( id, "print \"Not enough energy for cloak\n\"" );
		return;
	}
	gh_energy[id] -= GH_CLOAK_COST;
	ent->client->ps.powerups[PW_INVIS] = level.time + GH_CLOAK_MS;
	trap_SendServerCommand( id, "cp \"CLOAKED\n\"" );
}

void Cmd_GhostEmp_f( gentity_t *ent ) {
	int id, i;
	gentity_t *bot;
	vec3_t d;
	float dist;
	if ( !NW_GhostActive() ) {
		trap_SendServerCommand( ent - g_entities, "print \"Ghost kit off (g_neonwave_ghost 0)\n\"" );
		return;
	}
	if ( !ent->client || ent->health <= 0 ) {
		return;
	}
	id = GH_Id( ent );
	if ( gh_empUntil[id] > level.time ) {
		return;
	}
	if ( gh_energy[id] < GH_EMP_COST ) {
		trap_SendServerCommand( id, "print \"Not enough energy for EMP\n\"" );
		return;
	}
	gh_energy[id] -= GH_EMP_COST;
	gh_empUntil[id] = level.time + GH_EMP_CD;
	for ( i = 0; i < level.maxclients; i++ ) {
		bot = &g_entities[i];
		if ( !bot->inuse || !bot->client ) continue;
		if ( !( bot->r.svFlags & SVF_BOT ) ) continue;
		if ( bot->health <= 0 ) continue;
		VectorSubtract( bot->r.currentOrigin, ent->r.currentOrigin, d );
		dist = VectorLength( d );
		if ( dist > GH_EMP_RADIUS ) continue;
		gh_stunUntil[i] = level.time + GH_EMP_STUN;
		VectorClear( bot->client->ps.velocity );
		bot->client->ps.pm_time = GH_EMP_STUN;
		bot->client->ps.pm_flags |= PMF_TIME_KNOCKBACK;
	}
	G_AddEvent( ent, EV_GENERAL_SOUND, G_SoundIndex( "sound/weapons/plasma/plasmx1a.wav" ) );
	trap_SendServerCommand( id, "cp \"EMP\n\"" );
}

void Cmd_GhostNuke_f( gentity_t *ent ) {
	int id;
	vec3_t forward, right, up, muzzle, end;
	trace_t tr;
	if ( !NW_GhostActive() ) {
		trap_SendServerCommand( ent - g_entities, "print \"Ghost kit off (g_neonwave_ghost 0)\n\"" );
		return;
	}
	if ( !ent->client || ent->health <= 0 ) {
		return;
	}
	id = GH_Id( ent );
	if ( gh_paintUntil[id] > level.time || gh_boomAt[id] > level.time ) {
		return;
	}
	if ( gh_nukeCdUntil[id] > level.time ) {
		return;
	}
	if ( gh_energy[id] < GH_NUKE_COST ) {
		trap_SendServerCommand( id, "print \"Not enough energy for nuke (need 80)\n\"" );
		return;
	}
	gh_energy[id] -= GH_NUKE_COST;
	AngleVectors( ent->client->ps.viewangles, forward, right, up );
	VectorCopy( ent->s.pos.trBase, muzzle );
	muzzle[2] += ent->client->ps.viewheight;
	VectorMA( muzzle, 8192, forward, end );
	trap_Trace( &tr, muzzle, NULL, NULL, end, ent->s.number, MASK_SHOT );
	VectorCopy( tr.endpos, gh_nukePos[id] );
	VectorCopy( ent->r.currentOrigin, gh_paintOrigin[id] );
	gh_paintUntil[id] = level.time + GH_NUKE_PAINT;
	gh_lastNukeSec[id] = 0;
	gh_lastLaser[id] = 0;
	trap_SendServerCommand( -1, "cp \"DESIGNATING — STAND STILL\n\"" );
}

static void GH_Detonate( gentity_t *ent, int id ) {
	gentity_t *tent;
	gentity_t *targ;
	vec3_t dir, up;
	int i, dmg;
	float dist;

	tent = G_TempEntity( gh_nukePos[id], EV_MISSILE_MISS );
	tent->s.weapon = WP_BFG;
	VectorSet( up, 0, 0, 1 );
	tent->s.eventParm = DirToByte( up );
	G_AddEvent( ent, EV_GENERAL_SOUND, G_SoundIndex( "sound/weapons/bfg/bfg_hum.wav" ) );

	for ( i = 0; i < level.num_entities; i++ ) {
		targ = &g_entities[i];
		if ( !targ->inuse || !targ->takedamage ) continue;
		if ( !targ->client ) continue;
		VectorSubtract( targ->r.currentOrigin, gh_nukePos[id], dir );
		dist = VectorLength( dir );
		if ( dist > GH_NUKE_RADIUS ) continue;
		if ( targ->r.svFlags & SVF_BOT ) {
			if ( targ->client->pers.neonwaveBoss ) {
				dmg = targ->client->pers.maxHealth * GH_NUKE_BOSS_PCT / 100;
				if ( dmg < 1 ) dmg = 1;
			} else {
				dmg = 10000;
			}
		} else {
			dmg = GH_NUKE_SELF_DMG;
		}
		G_Damage( targ, ent, ent, dir, gh_nukePos[id], dmg, DAMAGE_RADIUS, MOD_BFG );
	}
	trap_SendServerCommand( -1, "cp \"NUCLEAR STRIKE\n\"" );
	G_Printf( "Ghost: nuke detonated by %s\n", ent->client->pers.netname );
}

static void GH_DetectorThink( void ) {
	int i, h;
	gentity_t *det, *hum;
	vec3_t toH, fwd, right, up;
	float dist, cone;
	for ( i = 0; i < level.maxclients; i++ ) {
		det = &g_entities[i];
		if ( !det->inuse || !det->client ) continue;
		if ( !det->client->pers.neonwaveDetector ) continue;
		if ( det->health <= 0 ) continue;
		det->s.constantLight = 255 | ( 30 << 8 ) | ( 20 << 16 ) | ( 180 << 24 );
		AngleVectors( det->client->ps.viewangles, fwd, right, up );
		for ( h = 0; h < level.maxclients; h++ ) {
			hum = &g_entities[h];
			if ( !hum->inuse || !hum->client ) continue;
			if ( hum->r.svFlags & SVF_BOT ) continue;
			if ( hum->health <= 0 ) continue;
			if ( hum->client->ps.powerups[PW_INVIS] <= level.time ) continue;
			VectorSubtract( hum->r.currentOrigin, det->r.currentOrigin, toH );
			dist = VectorLength( toH );
			if ( dist > GH_DETECT_RANGE ) continue;
			if ( dist < 8.0f ) {
				cone = 1.0f;
			} else {
				VectorNormalize( toH );
				cone = DotProduct( fwd, toH );
			}
			if ( cone < 0.76f ) continue;
			NW_GhostBreakCloak( hum );
			gh_swarmUntil = level.time + GH_SWARM_MS;
			trap_SendServerCommand( -1, "cp \"DETECTED\n\"" );
			G_Printf( "Ghost: detector revealed client %i\n", h );
		}
	}
}

void NW_GhostFrame( void ) {
	int i, id, sec;
	gentity_t *ent;
	float moved;
	vec3_t d;
	if ( !NW_GhostActive() ) {
		return;
	}
	GH_DetectorThink();
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->r.svFlags & SVF_BOT ) {
			if ( gh_stunUntil[i] > level.time ) {
				VectorClear( ent->client->ps.velocity );
				ent->client->ps.pm_flags |= PMF_TIME_KNOCKBACK;
				if ( ent->client->ps.pm_time < 50 ) {
					ent->client->ps.pm_time = 50;
				}
			}
			continue;
		}
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		id = i;
		if ( gh_lastRegen[id] == 0 ) {
			gh_lastRegen[id] = level.time;
		}
		if ( level.time - gh_lastRegen[id] >= GH_REGEN_MS ) {
			gh_energy[id] += GH_REGEN_AMT;
			if ( gh_energy[id] > GH_ENERGY_MAX ) {
				gh_energy[id] = GH_ENERGY_MAX;
			}
			gh_lastRegen[id] = level.time;
		}
		if ( gh_paintUntil[id] > 0 ) {
			VectorSubtract( ent->r.currentOrigin, gh_paintOrigin[id], d );
			moved = VectorLength( d );
			if ( ent->health <= 0 || moved > GH_MOVE_CANCEL ) {
				gh_paintUntil[id] = 0;
				trap_SendServerCommand( id, "cp \"NUKE CANCELLED\n\"" );
			} else if ( level.time >= gh_paintUntil[id] ) {
				gh_paintUntil[id] = 0;
				gh_boomAt[id] = level.time + GH_NUKE_INBOUND;
				gh_nukeCdUntil[id] = level.time + GH_NUKE_CD;
				gh_lastNukeSec[id] = 5;
				trap_SendServerCommand( -1, "cp \"NUKE INBOUND\n\"" );
			} else {
				GH_NukeLaser( id );
			}
		}
		if ( gh_boomAt[id] > level.time ) {
			sec = ( gh_boomAt[id] - level.time + 999 ) / 1000;
			if ( sec != gh_lastNukeSec[id] && sec > 0 ) {
				gh_lastNukeSec[id] = sec;
				trap_SendServerCommand( -1, va( "cp \"NUKE %i\n\"", sec ) );
			}
			GH_NukeLaser( id );
			GH_FleeBots( id );
		} else if ( gh_boomAt[id] > 0 && level.time >= gh_boomAt[id] ) {
			gh_boomAt[id] = 0;
			GH_Detonate( ent, id );
		}
		if ( ( level.time % 200 ) < 50 ) {
			GH_SyncHud( ent );
		}
	}
}

#endif
