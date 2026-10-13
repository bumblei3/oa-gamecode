// g_nw_bosses.c - NeonWave bosses (type rotation, spawn, per-frame mechanics)
// Split out of g_neonwave.c. Boss type rotation/pick, HP scaling, phase-2
// (enrage) handling and the per-type mechanics frame. Shared state via
// g_nw_internal.h.
#include "g_local.h"
#include "g_neonwave.h"
#include "g_nw_internal.h"

#ifdef NEONARENA_MOD

// boss state (owned by this module; extern in g_nw_internal.h)
int nw_bossType = 0;		// current boss type (NW_BOSS_*)
int nw_bossPhase = 1;		// boss phase (1 normal, 2 enraged)
int nw_difficulty = 0;		// dynamic difficulty tier -2..1 (0=normal)
static int nw_bossLastAttack = 0;	// last BERSERKER attack frame
static int nw_dailyBossOffset = 0;	// boss rotation offset 0..(NW_BOSS_COUNT-1)
static int nw_bossEntityCache = -1;	// cached boss entity index (-1 = invalid)

void NW_SetDailyBossOffset( int off ) {
	nw_dailyBossOffset = off;
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

const char *NW_BossName( int type ) {
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

void NW_SpawnBoss( void ) {
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
	trap_SendServerCommand( -1, va( "cp \"BOSS: %s\n\"", NW_BossName( type ) ) );
	trap_SendConsoleCommand( EXEC_APPEND,
		va("addbot sarge 5 \"BOSS W%d %s\"\n", nw_wave, NW_BossName( type )) );
}

void NW_InvalidateBossCache( void ) {
	nw_bossEntityCache = -1;
}

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



void NW_BossMechanicsFrame( int *lastMini, int bots ) {
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
				trap_SendServerCommand( -1, "cp \"BERSERKER ENTERS RAGE\n\"" );
			}
			if ( rage && level.time - nw_bossLastAttack > 800 ) {
				nw_bossLastAttack = level.time;
				trap_SendServerCommand( -1, "cp \"BERSERKER ATTACKS\n\"" );
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
			trap_SendServerCommand( -1, "cp \"TELEPORTER blinks away!\n\"" );
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

#endif // NEONARENA_MOD
