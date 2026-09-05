// StarCraft-inspired Ghost kit for NeonArena (OpenArena).
// Activate with g_neonwave_ghost 1. Commands: cloak, emp, lockdown, nuke.
#include "g_local.h"

#ifdef NEONARENA_MOD

#define GH_ENERGY_MAX		100
#define GH_ENERGY_START		55
#define GH_ENERGY_KILL		15
#define GH_REGEN_MS		1000
#define GH_REGEN_AMT		3
#define GH_CLOAK_COST		25
#define GH_CLOAK_DRAIN		8
#define GH_AMBUSH_MS		2000
#define GH_EMP_COST		35
#define GH_EMP_CD		25000
#define GH_EMP_RADIUS		400
#define GH_EMP_STUN		1500
#define GH_EMP_SPEED		1600
#define GH_LOCK_COST		50
#define GH_LOCK_CD		20000
#define GH_LOCK_MS		4000
#define GH_LOCK_SPEED		900
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
#define GH_WARN_MS		800
#define GH_HUD_NONE		0
#define GH_HUD_CLOAKED		1
#define GH_HUD_AMBUSH		2
#define GH_HUD_SCAN		3
#define GH_HUD_DETECTED		4
#define GH_HUD_DESIGNATING	5
#define GH_HUD_NUKE		6
#define GH_LOCK_LIGHT		( 40 | ( 230 << 8 ) | ( 255 << 16 ) | ( 200 << 24 ) )
#define GH_DET_LIGHT		( 255 | ( 30 << 8 ) | ( 20 << 16 ) | ( 180 << 24 ) )

static int gh_energy[MAX_CLIENTS];
static int gh_lastRegen[MAX_CLIENTS];
static int gh_empUntil[MAX_CLIENTS];
static int gh_stunUntil[MAX_CLIENTS];
static int gh_lockUntil[MAX_CLIENTS];
static int gh_lockCdUntil[MAX_CLIENTS];
static int gh_lockFlying[MAX_CLIENTS];
static int gh_ambushUntil[MAX_CLIENTS];
static int gh_nukeCdUntil[MAX_CLIENTS];
static int gh_paintUntil[MAX_CLIENTS];
static int gh_boomAt[MAX_CLIENTS];
static int gh_lastNukeSec[MAX_CLIENTS];
static int gh_lastLaser[MAX_CLIENTS];
static vec3_t gh_nukePos[MAX_CLIENTS];
static vec3_t gh_paintOrigin[MAX_CLIENTS];
static int gh_swarmUntil;
static int gh_coneStart[MAX_CLIENTS];
static int gh_inCone[MAX_CLIENTS];
static int gh_lastWarn[MAX_CLIENTS];
static int gh_turretWarn;

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

static void GH_Sound( gentity_t *ent, char *path ) {
	if ( !ent ) {
		return;
	}
	G_AddEvent( ent, EV_GENERAL_SOUND, G_SoundIndex( path ) );
}

static qboolean GH_Cloaked( gentity_t *ent ) {
	return ent->client && ent->client->ps.powerups[PW_INVIS] > level.time;
}

qboolean NW_GhostLocked( gentity_t *ent ) {
	int id;
	if ( !NW_GhostActive() || !ent || !ent->client ) {
		return qfalse;
	}
	id = GH_Id( ent );
	if ( id < 0 || id >= MAX_CLIENTS ) {
		return qfalse;
	}
	return gh_lockUntil[id] > level.time;
}

qboolean NW_GhostAmbush( gentity_t *ent ) {
	int id;
	if ( !NW_GhostActive() || !ent || !ent->client ) {
		return qfalse;
	}
	id = GH_Id( ent );
	if ( id < 0 || id >= MAX_CLIENTS ) {
		return qfalse;
	}
	if ( gh_ambushUntil[id] <= level.time ) {
		return qfalse;
	}
	gh_ambushUntil[id] = 0;
	trap_SendServerCommand( id, "cp \"AMBUSH\n\"" );
	GH_Sound( ent, "sound/feedback/hit.wav" );
	return qtrue;
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
	if ( GH_Cloaked( ent ) ) {
		gh_ambushUntil[id] = level.time + GH_AMBUSH_MS;
		GH_Sound( ent, "sound/items/wearoff.wav" );
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
	gh_lockUntil[id] = 0;
	gh_lockCdUntil[id] = 0;
	gh_lockFlying[id] = 0;
	gh_ambushUntil[id] = 0;
	gh_nukeCdUntil[id] = 0;
	gh_paintUntil[id] = 0;
	gh_boomAt[id] = 0;
	gh_lastNukeSec[id] = 0;
	gh_coneStart[id] = 0;
	gh_inCone[id] = 0;
	ent->client->ps.powerups[PW_INVIS] = 0;
	ent->client->ps.stats[STAT_GHOST_ENERGY] = GH_ENERGY_START;
	ent->client->ps.stats[STAT_GHOST_CDS] = 0;
	ent->client->ps.stats[STAT_GHOST_ST] = 0;
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

static int GH_SecLeft( int until ) {
	int ms;
	ms = until - level.time;
	if ( ms <= 0 ) {
		return 0;
	}
	ms = ( ms + 999 ) / 1000;
	if ( ms > 255 ) {
		ms = 255;
	}
	return ms;
}

static void GH_SyncHud( gentity_t *ent ) {
	int id, cloakLeft, empCd, nukeCd, lockCd, empS, lockS, nukeS, cloakS;
	int st, nukeSec;
	id = GH_Id( ent );
	cloakLeft = 0;
	if ( GH_Cloaked( ent ) ) {
		cloakLeft = gh_energy[id] * 1000 / GH_CLOAK_DRAIN;
		if ( cloakLeft < 1 ) {
			cloakLeft = 1;
		}
	}
	empCd = gh_empUntil[id] - level.time;
	if ( empCd < 0 ) empCd = 0;
	nukeCd = gh_nukeCdUntil[id] - level.time;
	if ( nukeCd < 0 ) nukeCd = 0;
	lockCd = gh_lockCdUntil[id] - level.time;
	if ( lockCd < 0 ) lockCd = 0;
	empS = GH_SecLeft( gh_empUntil[id] );
	lockS = GH_SecLeft( gh_lockCdUntil[id] );
	nukeS = GH_SecLeft( gh_nukeCdUntil[id] );
	cloakS = ( cloakLeft + 999 ) / 1000;
	if ( cloakS > 255 ) cloakS = 255;
	st = GH_HUD_NONE;
	nukeSec = 0;
	if ( gh_paintUntil[id] > level.time ) {
		st = GH_HUD_DESIGNATING;
		trap_Cvar_Set( "g_ghost_status", "DESIGNATING" );
	} else if ( gh_boomAt[id] > level.time ) {
		nukeSec = GH_SecLeft( gh_boomAt[id] );
		st = GH_HUD_NUKE;
		trap_Cvar_Set( "g_ghost_status", va( "NUKE %i", nukeSec ) );
	} else if ( gh_swarmUntil > level.time ) {
		st = GH_HUD_DETECTED;
		trap_Cvar_Set( "g_ghost_status", "DETECTED" );
	} else if ( gh_inCone[id] ) {
		st = GH_HUD_SCAN;
		trap_Cvar_Set( "g_ghost_status", "SCANNING" );
	} else if ( cloakLeft > 0 ) {
		st = GH_HUD_CLOAKED;
		trap_Cvar_Set( "g_ghost_status", "CLOAKED" );
	} else if ( gh_ambushUntil[id] > level.time ) {
		st = GH_HUD_AMBUSH;
		trap_Cvar_Set( "g_ghost_status", "AMBUSH" );
	} else {
		trap_Cvar_Set( "g_ghost_status", "" );
	}
	ent->client->ps.stats[STAT_GHOST_ENERGY] = gh_energy[id];
	ent->client->ps.stats[STAT_GHOST_CDS] = empS | ( lockS << 8 ) | ( nukeS << 16 ) | ( cloakS << 24 );
	ent->client->ps.stats[STAT_GHOST_ST] = st | ( nukeSec << 8 );
	trap_Cvar_Set( "g_ghost_energy", va( "%i", gh_energy[id] ) );
	trap_Cvar_Set( "g_ghost_cloakms", va( "%i", cloakLeft ) );
	trap_Cvar_Set( "g_ghost_empcd", va( "%i", empCd ) );
	trap_Cvar_Set( "g_ghost_nukecd", va( "%i", nukeCd ) );
	trap_Cvar_Set( "g_ghost_lockcd", va( "%i", lockCd ) );
}

static void GH_LockBeam( gentity_t *bot ) {
	gentity_t *tent;
	vec3_t top;
	VectorCopy( bot->r.currentOrigin, top );
	top[2] += 96;
	tent = G_TempEntity( bot->r.currentOrigin, EV_RAILTRAIL );
	VectorCopy( top, tent->s.origin2 );
	tent->s.eventParm = 255;
	tent->s.clientNum = bot->s.number;
}

static void GH_WarnLaser( gentity_t *det, gentity_t *hum, int *lastWarn ) {
	gentity_t *tent;
	if ( !lastWarn ) {
		return;
	}
	if ( level.time - *lastWarn < 200 ) {
		return;
	}
	*lastWarn = level.time;
	tent = G_TempEntity( hum->r.currentOrigin, EV_RAILTRAIL );
	VectorCopy( det->r.currentOrigin, tent->s.origin2 );
	tent->s.eventParm = 255;
	tent->s.clientNum = det->s.number;
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
		if ( gh_lockUntil[i] > level.time ) continue;
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

static void GH_StunBot( gentity_t *bot, int ms ) {
	int id;
	id = GH_Id( bot );
	if ( id < 0 || id >= MAX_CLIENTS ) {
		return;
	}
	gh_stunUntil[id] = level.time + ms;
	VectorClear( bot->client->ps.velocity );
	bot->client->ps.pm_time = ms;
	bot->client->ps.pm_flags |= PMF_TIME_KNOCKBACK;
}

void NW_GhostEmpBurst( vec3_t origin, gentity_t *owner ) {
	int i;
	gentity_t *bot;
	gentity_t *tent;
	vec3_t d, up;
	float dist;

	tent = G_TempEntity( origin, EV_MISSILE_MISS );
	tent->s.weapon = WP_PLASMAGUN;
	VectorSet( up, 0, 0, 1 );
	tent->s.eventParm = DirToByte( up );
	if ( owner ) {
		G_AddEvent( owner, EV_GENERAL_SOUND, G_SoundIndex( "sound/weapons/plasma/plasmx1a.wav" ) );
	}

	for ( i = 0; i < level.maxclients; i++ ) {
		bot = &g_entities[i];
		if ( !bot->inuse || !bot->client ) continue;
		if ( !( bot->r.svFlags & SVF_BOT ) ) continue;
		if ( bot->health <= 0 ) continue;
		VectorSubtract( bot->r.currentOrigin, origin, d );
		dist = VectorLength( d );
		if ( dist > GH_EMP_RADIUS ) continue;
		bot->client->ps.stats[STAT_ARMOR] = 0;
		GH_StunBot( bot, GH_EMP_STUN );
	}
}

static void GH_EmpThink( gentity_t *ent ) {
	NW_GhostEmpBurst( ent->r.currentOrigin, ent->parent );
	ent->classname = "ghost_emp_x";
	G_FreeEntity( ent );
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
	if ( GH_Cloaked( ent ) ) {
		NW_GhostBreakCloak( ent );
		trap_SendServerCommand( id, "cp \"DECLOAKED\n\"" );
		return;
	}
	if ( gh_energy[id] < GH_CLOAK_COST ) {
		trap_SendServerCommand( id, "print \"Not enough energy for cloak\n\"" );
		return;
	}
	gh_energy[id] -= GH_CLOAK_COST;
	gh_lastRegen[id] = level.time;
	ent->client->ps.powerups[PW_INVIS] = level.time + 3600000;
	trap_SendServerCommand( id, "cp \"CLOAKED\n\"" );
	GH_Sound( ent, "sound/items/protect3.wav" );
}

void Cmd_GhostEmp_f( gentity_t *ent ) {
	int id;
	vec3_t forward, right, up, muzzle;
	gentity_t *bolt;
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
	NW_GhostBreakCloak( ent );
	AngleVectors( ent->client->ps.viewangles, forward, right, up );
	CalcMuzzlePoint( ent, forward, right, up, muzzle );
	bolt = fire_plasma( ent, muzzle, forward );
	bolt->classname = "ghost_emp";
	bolt->damage = 0;
	bolt->splashDamage = 0;
	bolt->splashRadius = 0;
	bolt->think = GH_EmpThink;
	bolt->nextthink = level.time + 3000;
	VectorScale( forward, GH_EMP_SPEED, bolt->s.pos.trDelta );
	SnapVector( bolt->s.pos.trDelta );
	trap_SendServerCommand( id, "cp \"EMP\n\"" );
	GH_Sound( ent, "sound/weapons/plasma/hyprbf1a.wav" );
}

static void GH_LockRefund( gentity_t *owner ) {
	int id;
	if ( !owner || !owner->client ) {
		return;
	}
	id = GH_Id( owner );
	if ( id < 0 || id >= MAX_CLIENTS ) {
		return;
	}
	if ( !gh_lockFlying[id] ) {
		return;
	}
	gh_lockFlying[id] = 0;
	gh_energy[id] += GH_LOCK_COST;
	if ( gh_energy[id] > GH_ENERGY_MAX ) {
		gh_energy[id] = GH_ENERGY_MAX;
	}
	trap_SendServerCommand( id, "print \"Lockdown missed\n\"" );
}

static qboolean GH_IsTurret( gentity_t *ent ) {
	return ent && ent->inuse && ent->classname
		&& !Q_stricmp( ent->classname, "ghost_detector_turret" );
}

static void GH_LockTarget( gentity_t *owner, gentity_t *targ ) {
	int oid, tid;
	oid = GH_Id( owner );
	gh_lockFlying[oid] = 0;
	gh_lockCdUntil[oid] = level.time + GH_LOCK_CD;
	targ->s.constantLight = GH_LOCK_LIGHT;
	GH_LockBeam( targ );
	trap_SendServerCommand( -1, "cp \"LOCKED\n\"" );
	GH_Sound( owner, "sound/weapons/lightning/lg_hit.wav" );
	if ( GH_IsTurret( targ ) ) {
		targ->timestamp = level.time + GH_LOCK_MS;
		G_Printf( "Ghost: lockdown on Detector Turret\n" );
		return;
	}
	tid = GH_Id( targ );
	gh_lockUntil[tid] = level.time + GH_LOCK_MS;
	VectorClear( targ->client->ps.velocity );
	targ->client->ps.pm_time = GH_LOCK_MS;
	targ->client->ps.pm_flags |= PMF_TIME_KNOCKBACK;
	targ->client->ps.weaponTime = GH_LOCK_MS;
	G_Printf( "Ghost: lockdown on %s\n", targ->client->pers.netname );
}

static void GH_LockThink( gentity_t *ent ) {
	GH_LockRefund( ent->parent );
	ent->classname = "ghost_lock_x";
	G_FreeEntity( ent );
}

void NW_GhostLockImpact( gentity_t *bolt, gentity_t *other ) {
	gentity_t *owner;
	qboolean mech;
	owner = bolt->parent;
	mech = qfalse;
	if ( GH_IsTurret( other ) && other->health > 0 ) {
		mech = qtrue;
	} else if ( other && other->inuse && other->client && other->health > 0
			&& ( other->r.svFlags & SVF_BOT )
			&& ( other->client->pers.neonwaveBoss || other->client->pers.neonwaveDetector ) ) {
		mech = qtrue;
	}
	if ( mech && owner && owner->client ) {
		GH_LockTarget( owner, other );
	} else {
		GH_LockRefund( owner );
	}
}

void Cmd_GhostLockdown_f( gentity_t *ent ) {
	int id;
	vec3_t forward, right, up, muzzle;
	gentity_t *bolt;
	if ( !NW_GhostActive() ) {
		trap_SendServerCommand( ent - g_entities, "print \"Ghost kit off (g_neonwave_ghost 0)\n\"" );
		return;
	}
	if ( !ent->client || ent->health <= 0 ) {
		return;
	}
	id = GH_Id( ent );
	if ( gh_lockCdUntil[id] > level.time ) {
		return;
	}
	if ( gh_lockFlying[id] ) {
		return;
	}
	if ( gh_energy[id] < GH_LOCK_COST ) {
		trap_SendServerCommand( id, "print \"Not enough energy for lockdown (need 50)\n\"" );
		return;
	}
	gh_energy[id] -= GH_LOCK_COST;
	gh_lockFlying[id] = 1;
	NW_GhostBreakCloak( ent );
	AngleVectors( ent->client->ps.viewangles, forward, right, up );
	CalcMuzzlePoint( ent, forward, right, up, muzzle );
	bolt = fire_rocket( ent, muzzle, forward );
	bolt->classname = "ghost_lock";
	bolt->damage = 0;
	bolt->splashDamage = 0;
	bolt->splashRadius = 0;
	bolt->think = GH_LockThink;
	bolt->nextthink = level.time + 2500;
	VectorScale( forward, GH_LOCK_SPEED, bolt->s.pos.trDelta );
	SnapVector( bolt->s.pos.trDelta );
	trap_SendServerCommand( id, "cp \"LOCKDOWN\n\"" );
	GH_Sound( ent, "sound/weapons/rocket/rocklf1a.wav" );
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
	GH_Sound( ent, "sound/weapons/bfg/bfg_fire.wav" );
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

static void GH_ScanCloak( gentity_t *det, vec3_t fwd, int *lastWarn ) {
	int h;
	gentity_t *hum;
	vec3_t toH;
	float dist, cone;

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
		if ( !gh_inCone[h] && gh_coneStart[h] == 0 ) {
			gh_coneStart[h] = level.time;
			trap_SendServerCommand( h, "cp \"SCANNING\n\"" );
		}
		gh_inCone[h] = 1;
		GH_WarnLaser( det, hum, lastWarn );
		if ( level.time - gh_coneStart[h] >= GH_WARN_MS ) {
			NW_GhostBreakCloak( hum );
			gh_swarmUntil = level.time + GH_SWARM_MS;
			gh_inCone[h] = 0;
			gh_coneStart[h] = 0;
			trap_SendServerCommand( -1, "cp \"DETECTED\n\"" );
			G_Printf( "Ghost: detector revealed client %i\n", h );
		}
	}
}

static void GH_TurretDie( gentity_t *self, gentity_t *inflictor, gentity_t *attacker, int damage, int meansOfDeath ) {
	G_Printf( "Ghost: detector turret destroyed\n" );
	self->takedamage = qfalse;
	self->r.contents = 0;
	self->s.constantLight = 0;
	self->think = G_FreeEntity;
	self->nextthink = level.time + 50;
	self->classname = "ghost_detector_turret_x";
}

static void GH_TurretPickOrigin( vec3_t out ) {
	gentity_t *spot, *hum, *best;
	float bestMin, nearest, d;
	vec3_t delta;
	int h;

	best = NULL;
	bestMin = -1.0f;
	spot = NULL;
	while ( ( spot = G_Find( spot, FOFS( classname ), "info_player_deathmatch" ) ) != NULL ) {
		nearest = 999999.0f;
		for ( h = 0; h < level.maxclients; h++ ) {
			hum = &g_entities[h];
			if ( !hum->inuse || !hum->client ) continue;
			if ( hum->r.svFlags & SVF_BOT ) continue;
			if ( hum->health <= 0 ) continue;
			VectorSubtract( spot->s.origin, hum->r.currentOrigin, delta );
			d = VectorLength( delta );
			if ( d < nearest ) {
				nearest = d;
			}
		}
		if ( nearest > bestMin ) {
			bestMin = nearest;
			best = spot;
		}
	}
	if ( best ) {
		VectorCopy( best->s.origin, out );
		out[2] += 8.0f;
	} else {
		VectorSet( out, 0, 0, 48 );
	}
}

void NW_GhostSpawnTurret( int wave ) {
	gentity_t *t, *old;
	vec3_t org;

	if ( !NW_GhostActive() || wave < 8 ) {
		return;
	}
	old = NULL;
	while ( ( old = G_Find( old, FOFS( classname ), "ghost_detector_turret" ) ) != NULL ) {
		old->classname = "ghost_detector_turret_x";
		old->think = G_FreeEntity;
		old->nextthink = level.time + 1;
		old->r.contents = 0;
		trap_UnlinkEntity( old );
	}
	GH_TurretPickOrigin( org );
	t = G_Spawn();
	t->classname = "ghost_detector_turret";
	t->s.eType = ET_GENERAL;
	t->s.modelindex = G_ModelIndex( "models/weapons2/rocketl/rocketl.md3" );
	t->s.constantLight = GH_DET_LIGHT;
	VectorCopy( org, t->s.origin );
	VectorCopy( org, t->r.currentOrigin );
	VectorCopy( org, t->s.pos.trBase );
	t->s.pos.trType = TR_STATIONARY;
	VectorSet( t->r.mins, -16, -16, 0 );
	VectorSet( t->r.maxs, 16, 16, 40 );
	t->r.contents = CONTENTS_BODY;
	t->clipmask = MASK_SOLID;
	t->takedamage = qtrue;
	t->health = 120;
	t->die = GH_TurretDie;
	t->timestamp = 0;
	trap_LinkEntity( t );
	G_Printf( "NeonWave: DETECTOR turret (wave %i)\n", wave );
}

static void GH_DetectorThink( void ) {
	int i, h;
	gentity_t *det;
	vec3_t fwd, right, up;

	for ( h = 0; h < level.maxclients; h++ ) {
		gh_inCone[h] = 0;
	}
	for ( i = 0; i < level.maxclients; i++ ) {
		det = &g_entities[i];
		if ( !det->inuse || !det->client ) continue;
		if ( !det->client->pers.neonwaveDetector ) continue;
		if ( det->health <= 0 ) continue;
		if ( gh_lockUntil[i] > level.time ) continue;
		det->s.constantLight = GH_DET_LIGHT;
		AngleVectors( det->client->ps.viewangles, fwd, right, up );
		GH_ScanCloak( det, fwd, &gh_lastWarn[i] );
	}
	for ( i = MAX_CLIENTS; i < level.num_entities; i++ ) {
		det = &g_entities[i];
		if ( !GH_IsTurret( det ) ) continue;
		if ( det->health <= 0 ) continue;
		if ( det->timestamp > level.time ) {
			det->s.constantLight = GH_LOCK_LIGHT;
			if ( ( level.time % 200 ) < 50 ) {
				GH_LockBeam( det );
			}
			continue;
		}
		det->s.angles[YAW] += 2.0f;
		if ( det->s.angles[YAW] > 360.0f ) {
			det->s.angles[YAW] -= 360.0f;
		}
		VectorCopy( det->s.angles, det->s.apos.trBase );
		det->s.constantLight = GH_DET_LIGHT;
		AngleVectors( det->s.angles, fwd, right, up );
		GH_ScanCloak( det, fwd, &gh_turretWarn );
	}
	for ( h = 0; h < level.maxclients; h++ ) {
		if ( !gh_inCone[h] ) {
			gh_coneStart[h] = 0;
		}
	}
}

void NW_GhostFrame( void ) {
	int i, id, sec;
	gentity_t *ent;
	float moved;
	vec3_t d;
	qboolean cloaked;
	if ( !NW_GhostActive() ) {
		return;
	}
	GH_DetectorThink();
	for ( i = 0; i < level.maxclients; i++ ) {
		ent = &g_entities[i];
		if ( !ent->inuse || !ent->client ) continue;
		if ( ent->r.svFlags & SVF_BOT ) {
			if ( gh_stunUntil[i] > level.time || gh_lockUntil[i] > level.time ) {
				VectorClear( ent->client->ps.velocity );
				ent->client->ps.pm_flags |= PMF_TIME_KNOCKBACK;
				if ( ent->client->ps.pm_time < 50 ) {
					ent->client->ps.pm_time = 50;
				}
				if ( gh_lockUntil[i] > level.time ) {
					ent->client->ps.weaponTime = 80;
					ent->s.constantLight = GH_LOCK_LIGHT;
					if ( ( level.time % 200 ) < 50 ) {
						GH_LockBeam( ent );
					}
				}
			}
			continue;
		}
		if ( ent->client->pers.connected != CON_CONNECTED ) continue;
		id = i;
		if ( gh_lastRegen[id] == 0 ) {
			gh_lastRegen[id] = level.time;
		}
		cloaked = GH_Cloaked( ent );
		if ( level.time - gh_lastRegen[id] >= GH_REGEN_MS ) {
			if ( cloaked ) {
				gh_energy[id] -= GH_CLOAK_DRAIN;
				if ( gh_energy[id] <= 0 ) {
					gh_energy[id] = 0;
					NW_GhostBreakCloak( ent );
					cloaked = qfalse;
					trap_SendServerCommand( id, "cp \"DECLOAKED\n\"" );
				}
			} else {
				gh_energy[id] += GH_REGEN_AMT;
				if ( gh_energy[id] > GH_ENERGY_MAX ) {
					gh_energy[id] = GH_ENERGY_MAX;
				}
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
		GH_SyncHud( ent );
	}
}

#endif
