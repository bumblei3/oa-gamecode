// NeonArena start menu — Play / Daily / Ghost / Arena without the console.
#include "ui_local.h"

#ifdef NEONARENA_MOD

#include "../game/neon_daily_pool.h"
#include "../game/neon_maplook.h"

#define ID_PLAY			10
#define ID_DAILY		11
#define ID_GHOST		12
#define ID_ARENA		13
#define ID_SETUP		14
#define ID_EXIT			15
#define ID_GHOST0		20
#define ID_GHOST1		21
#define ID_GHOST2		22
#define ID_GHOSTBACK		23
#define ID_ARENA_SPIN		30
#define ID_ARENA_GO		31
#define ID_ARENABACK		32

#define NEON_MENU_SPACING	34

typedef struct {
	const char	*title;
	const char	*stem;
	const char	*map;
	const char	*hint;
	int		ghost;
	int		hardcore;
	int		maxwave;
	int		startwave;
	int		modifier;
	const char	*hp;
	const char	*dmg;
	const char	*spd;
	const char	*cnt;
	const char	*grav;
	const char	*emax;	/* NULL = default 100 */
	const char	*regen;	/* NULL = default 4 */
} neonArenaDef_t;

static const neonArenaDef_t neonArenas[] = {
	{ "NEON ARENA",		"neon_arena",		"oa_shine",		"Rail + lightning. Home map.",		0, 0, 20, 0, 0,  "1",    "1",    "1",    "1",    "1",   NULL, NULL },
	{ "BOSS RUSH",		"boss_rush",		"oa_dm3",		"A boss every wave.",			0, 0, 15, 10, 0, "1",    "1",    "1",    "1",    "1",   NULL, NULL },
	{ "GHOST PROTOCOL",	"ghost_protocol",	"oa_shine",		"Ghost kit. Extra energy.",		1, 0, 20, 0, 0,  "1",    "1",    "1",    "1",    "1",   "150", "6" },
	{ "IRONMAN",		"ironman",		"oa_minia",		"Hardcore. One life. Harsh look.",	0, 1, 20, 0, 0,  "1",    "1",    "1",    "1",    "1",   NULL, NULL },
	{ "CATACOMBS",		"catacombs",		"oa_rpg3dm2",		"Tight halls. Low HP, high damage.",	0, 0, 18, 0, 0,  "0.85", "1.15", "1",    "1",    "1",   NULL, NULL },
	{ "BLEED CHAMBER",	"bleed_chamber",	"slimefac",		"Dark. Visibility is the fight.",	0, 0, 20, 0, 0,  "1",    "1",    "1",    "1",    "1",   NULL, NULL },
	{ "NODE CONTROL",	"node_control",		"oa_dm1",		"Vertical. Extra drones.",		0, 0, 22, 0, 0,  "1",    "1",    "1",    "1.1",  "1",   NULL, NULL },
	{ "DESERT STORM",	"desert_storm",		"islanddm",		"Open ground. Long range.",		0, 0, 20, 0, 0,  "1",    "1",    "1",    "1",    "1",   NULL, NULL },
	{ "VORTEX RING",	"vortex_ring",		"oa_dm6",		"LOWGRAV. Movement is everything.",	0, 0, 20, 0, 3,  "1",    "1",    "1",    "1",    "0.7", NULL, NULL },
	{ "FROSTBITE",		"frostbite",		"oa_minia",		"FROST. Ice look. Slow drones.",	0, 0, 20, 0, 12, "1",    "1",    "0.85", "1",    "1",   NULL, NULL },
	{ "SKYBRIDGE",		"skybridge",		"suspended",		"Platforms. Fall damage.",		0, 0, 20, 0, 0,  "1",    "1",    "1",    "1",    "0.8", NULL, NULL },
	{ "UNDERHIVE",		"underhive",		"am_underworks",	"Corridors. Lightning range.",		0, 0, 20, 0, 0,  "1",    "1",    "1",    "1",    "1",   NULL, NULL },
	{ "REACTOR",		"reactor",		"hydronex",		"Industrial core. Tight lanes.",	0, 0, 20, 0, 0,  "1",    "1",    "1",    "1",    "1",   NULL, NULL },
	{ "OVERGROWTH",		"overgrowth",		"am_galmevish",		"Ghost kit. Moss look.",		1, 0, 20, 0, 0,  "1",    "1",    "1",    "1",    "1",   NULL, NULL }
};

#define NEON_ARENA_COUNT ( (int)( sizeof( neonArenas ) / sizeof( neonArenas[0] ) ) )

static const char *neonArenaTitles[] = {
	"NEON ARENA",
	"BOSS RUSH",
	"GHOST PROTOCOL",
	"IRONMAN",
	"CATACOMBS",
	"BLEED CHAMBER",
	"NODE CONTROL",
	"DESERT STORM",
	"VORTEX RING",
	"FROSTBITE",
	"SKYBRIDGE",
	"UNDERHIVE",
	"REACTOR",
	"OVERGROWTH",
	NULL
};

typedef struct {
	menuframework_s	menu;
	menutext_s	banner;
	menutext_s	play;
	menutext_s	daily;
	menutext_s	ghost;
	menutext_s	arena;
	menutext_s	setup;
	menutext_s	exit;
} neonStartMenu_t;

typedef struct {
	menuframework_s	menu;
	menutext_s	banner;
	menutext_s	infil;
	menutext_s	sab;
	menutext_s	spec;
	menutext_s	back;
} neonGhostMenu_t;

typedef struct {
	menuframework_s	menu;
	menutext_s	banner;
	menulist_s	pick;
	menutext_s	go;
	menutext_s	back;
} neonArenaMenu_t;

static neonStartMenu_t	s_neon;
static neonGhostMenu_t	s_ghost;
static neonArenaMenu_t	s_arena;

static void UI_NeonGhostMenu( void );
static void UI_NeonArenaMenu( void );

static void Neon_ResetModeCvars( void ) {
	trap_Cvar_Set( "g_gametype", "14" );
	trap_Cvar_Set( "g_neonwave_daily", "0" );
	trap_Cvar_Set( "g_neonwave_ghost", "0" );
	trap_Cvar_Set( "g_neonwave_hardcore", "0" );
	trap_Cvar_Set( "g_neonwave_startwave", "0" );
	trap_Cvar_Set( "g_neonwave_maxwave", "20" );
	trap_Cvar_Set( "g_neonwave_modifier", "0" );
	trap_Cvar_Set( "g_neonwave_bosstype", "0" );
	trap_Cvar_Set( "g_neonwave_drone_hp_scale", "1" );
	trap_Cvar_Set( "g_neonwave_drone_damage_scale", "1" );
	trap_Cvar_Set( "g_neonwave_drone_speed_scale", "1" );
	trap_Cvar_Set( "g_neonwave_drone_count_scale", "1" );
	trap_Cvar_Set( "g_neonwave_gravity_scale", "1" );
	trap_Cvar_Set( "g_neonwave_arena", "" );
	trap_Cvar_Set( "g_ghost_energy_max", "100" );
	trap_Cvar_Set( "g_ghost_regen_amt", "4" );
	trap_Cvar_Set( "g_ghost_loadout", "0" );
}

static const neonArenaDef_t *Neon_ArenaByStem( const char *stem ) {
	int i;
	if ( !stem || !stem[0] ) {
		return NULL;
	}
	for ( i = 0; i < NEON_ARENA_COUNT; i++ ) {
		if ( !Q_stricmp( neonArenas[i].stem, stem ) ) {
			return &neonArenas[i];
		}
	}
	return NULL;
}

static void Neon_ApplyArena( const neonArenaDef_t *a, int daily ) {
	char buf[16];
	Neon_ResetModeCvars();
	if ( daily ) {
		trap_Cvar_Set( "g_neonwave_daily", "1" );
	}
	if ( !a ) {
		return;
	}
	trap_Cvar_Set( "g_neonwave_ghost", a->ghost ? "1" : "0" );
	trap_Cvar_Set( "g_neonwave_hardcore", a->hardcore ? "1" : "0" );
	Com_sprintf( buf, sizeof( buf ), "%i", a->maxwave );
	trap_Cvar_Set( "g_neonwave_maxwave", buf );
	if ( !daily ) {
		Com_sprintf( buf, sizeof( buf ), "%i", a->startwave );
		trap_Cvar_Set( "g_neonwave_startwave", buf );
		Com_sprintf( buf, sizeof( buf ), "%i", a->modifier );
		trap_Cvar_Set( "g_neonwave_modifier", buf );
	}
	trap_Cvar_Set( "g_neonwave_drone_hp_scale", a->hp );
	trap_Cvar_Set( "g_neonwave_drone_damage_scale", a->dmg );
	trap_Cvar_Set( "g_neonwave_drone_speed_scale", a->spd );
	trap_Cvar_Set( "g_neonwave_drone_count_scale", a->cnt );
	trap_Cvar_Set( "g_neonwave_gravity_scale", a->grav );
	if ( a->stem && a->stem[0] ) {
		trap_Cvar_Set( "g_neonwave_arena", a->stem );
	}
	if ( a->emax && a->emax[0] ) {
		trap_Cvar_Set( "g_ghost_energy_max", a->emax );
	}
	if ( a->regen && a->regen[0] ) {
		trap_Cvar_Set( "g_ghost_regen_amt", a->regen );
	}
}

static void Neon_ApplyMapLook( const char *map, const char *stem ) {
	const nwMapLook_t *look;
	char buf[8];
	look = NW_MapLookArena( stem, map );
	Com_sprintf( buf, sizeof( buf ), "%i", look->overbright );
	trap_Cvar_Set( "r_mapoverbrightbits", buf );
	trap_Cvar_Set( "r_gamma", look->gamma );
	trap_Cvar_Set( "r_bloom_intensity", look->bloom_i );
	trap_Cvar_Set( "r_bloom_threshold", look->bloom_t );
	trap_Cvar_Set( "cg_neon_grid", look->grid );
}

static void Neon_LaunchMap( const char *map, int ghost, const char *stem ) {
	if ( stem && stem[0] ) {
		trap_Cvar_Set( "g_neonwave_arena", stem );
	} else {
		trap_Cvar_Set( "g_neonwave_arena", "" );
	}
	Neon_ApplyMapLook( map, stem );
	trap_Cmd_ExecuteText( EXEC_APPEND, va( "wait ; map %s\n", map ) );
	if ( ghost ) {
		trap_Cmd_ExecuteText( EXEC_APPEND, "wait ; exec ghost-binds.cfg\n" );
	}
}

static int Neon_LastLoadout( void ) {
	char buf[8];
	int lo;
	trap_Cvar_VariableStringBuffer( "g_ghost_loadout", buf, sizeof( buf ) );
	lo = atoi( buf );
	if ( lo < 0 || lo > 2 ) {
		return 0;
	}
	return lo;
}

static void Neon_LaunchGhost( int loadout, const char *tag ) {
	char buf[8];
	Neon_ResetModeCvars();
	trap_Cvar_Set( "g_neonwave_ghost", "1" );
	Com_sprintf( buf, sizeof( buf ), "%i", loadout );
	trap_Cvar_Set( "g_ghost_loadout", buf );
	trap_Print( va( "NeonArena: start %s ghost loadout %i\n", tag, loadout ) );
	Neon_LaunchMap( "oa_shine", 1, "" );
}

static void Neon_StartPlay( void ) {
	Neon_LaunchGhost( Neon_LastLoadout(), "PLAY" );
}

static void Neon_StartDaily( void ) {
	qtime_t tm;
	char dateStr[32];
	int forced;
	int mi;
	const neonArenaDef_t *a;

	trap_RealTime( &tm );
	Com_sprintf( dateStr, sizeof( dateStr ), "%04i-%02i-%02i",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday );
	forced = (int)( NW_DailyHash( dateStr ) & 0x7fffffff );
	mi = NW_DailyPoolIndex( forced );
	a = Neon_ArenaByStem( nw_daily_arena[mi] );
	Neon_ApplyArena( a, 1 );
	trap_Cvar_Set( "ui_neonwave_dailymap", nw_daily_key[mi] );
	trap_Cvar_Set( "ui_neonwave_dailybsp", nw_daily_bsp[mi] );
	trap_Print( va( "NeonArena: start DAILY %s bsp %s\n", nw_daily_key[mi], nw_daily_bsp[mi] ) );
	Neon_LaunchMap( nw_daily_bsp[mi], a ? a->ghost : 0, nw_daily_arena[mi] );
}

static void Neon_StartGhost( int loadout ) {
	Neon_LaunchGhost( loadout, "GHOST" );
}

static void Neon_StartArena( int idx ) {
	const neonArenaDef_t *a;
	if ( idx < 0 || idx >= NEON_ARENA_COUNT ) {
		idx = 0;
	}
	a = &neonArenas[idx];
	Neon_ApplyArena( a, 0 );
	trap_Print( va( "NeonArena: start ARENA %s map %s\n", a->title, a->map ) );
	Neon_LaunchMap( a->map, a->ghost, a->stem );
}

static void NeonStart_Event( void *ptr, int event ) {
	if ( event != QM_ACTIVATED ) {
		return;
	}
	switch ( ( (menucommon_s *)ptr )->id ) {
	case ID_PLAY:
		Neon_StartPlay();
		break;
	case ID_DAILY:
		Neon_StartDaily();
		break;
	case ID_GHOST:
		UI_NeonGhostMenu();
		break;
	case ID_ARENA:
		UI_NeonArenaMenu();
		break;
	case ID_SETUP:
		UI_SetupMenu();
		break;
	case ID_EXIT:
		UI_CreditMenu();
		break;
	}
}

static void NeonGhost_Event( void *ptr, int event ) {
	if ( event != QM_ACTIVATED ) {
		return;
	}
	switch ( ( (menucommon_s *)ptr )->id ) {
	case ID_GHOST0:
		Neon_StartGhost( 0 );
		break;
	case ID_GHOST1:
		Neon_StartGhost( 1 );
		break;
	case ID_GHOST2:
		Neon_StartGhost( 2 );
		break;
	case ID_GHOSTBACK:
		UI_PopMenu();
		break;
	}
}

static void NeonArena_Event( void *ptr, int event ) {
	if ( event != QM_ACTIVATED ) {
		return;
	}
	switch ( ( (menucommon_s *)ptr )->id ) {
	case ID_ARENA_GO:
		Neon_StartArena( s_arena.pick.curvalue );
		break;
	case ID_ARENABACK:
		UI_PopMenu();
		break;
	}
}

static void Neon_InitPText( menutext_s *t, int y, int id, const char *label, void (*cb)(void *, int) ) {
	t->generic.type		= MTYPE_PTEXT;
	t->generic.flags	= QMF_CENTER_JUSTIFY | QMF_PULSEIFFOCUS;
	t->generic.x		= 320;
	t->generic.y		= y;
	t->generic.id		= id;
	t->generic.callback	= cb;
	t->string		= (char *)label;
	t->color		= color_red;
	t->style		= UI_CENTER | UI_DROPSHADOW;
}

static void NeonStart_Draw( void ) {
	int lo;
	int forced;
	int mi;
	qtime_t tm;
	char dateStr[32];
	const char *kit;
	const neonArenaDef_t *a;

	Menu_Draw( &s_neon.menu );

	lo = Neon_LastLoadout();
	if ( lo == 1 ) {
		kit = "PLAY  SABOTEUR   J cloak  H emp  K lock  L kit";
	} else if ( lo == 2 ) {
		kit = "PLAY  SPECTRE   H emp  K lock  N nuke  M scan";
	} else {
		kit = "PLAY  INFILTRATOR   J cloak  H emp  K lock  L kit";
	}
	UI_DrawString( 320, 418, kit, UI_CENTER | UI_SMALLFONT, color_white );

	trap_RealTime( &tm );
	Com_sprintf( dateStr, sizeof( dateStr ), "%04i-%02i-%02i",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday );
	forced = (int)( NW_DailyHash( dateStr ) & 0x7fffffff );
	mi = NW_DailyPoolIndex( forced );
	if ( nw_daily_arena[mi] && nw_daily_arena[mi][0] ) {
		a = Neon_ArenaByStem( nw_daily_arena[mi] );
		if ( a ) {
			UI_DrawString( 320, 434, va( "TODAY  %s", a->title ), UI_CENTER | UI_SMALLFONT, color_white );
		} else {
			UI_DrawString( 320, 434, va( "TODAY  %s", nw_daily_key[mi] ), UI_CENTER | UI_SMALLFONT, color_white );
		}
	} else {
		UI_DrawString( 320, 434, va( "TODAY  %s", nw_daily_key[mi] ), UI_CENTER | UI_SMALLFONT, color_white );
	}
}

static void NeonGhost_Draw( void ) {
	menucommon_s *item;
	const char *hint;

	Menu_Draw( &s_ghost.menu );
	hint = "INFILTRATOR cloak+emp  SABOTEUR cheap CC  SPECTRE nuke";
	item = Menu_ItemAtCursor( &s_ghost.menu );
	if ( item ) {
		if ( item->id == ID_GHOST0 ) {
			hint = "Cloak, EMP, Lockdown";
		} else if ( item->id == ID_GHOST1 ) {
			hint = "Cheaper EMP + Lockdown";
		} else if ( item->id == ID_GHOST2 ) {
			hint = "Nuke + Multiscan. No cloak.";
		}
	}
	UI_DrawString( 320, 400, hint, UI_CENTER | UI_SMALLFONT, color_white );
}

static void NeonArena_Draw( void ) {
	int idx;
	const neonArenaDef_t *a;

	Menu_Draw( &s_arena.menu );
	idx = s_arena.pick.curvalue;
	if ( idx >= 0 && idx < NEON_ARENA_COUNT ) {
		a = &neonArenas[idx];
		if ( a->hint && a->hint[0] ) {
			UI_DrawString( 320, 236, a->hint, UI_CENTER | UI_SMALLFONT, color_white );
		}
	}
}

void UI_NeonStartMenu( void ) {
	int y;
	int style;

	style = UI_CENTER | UI_DROPSHADOW;
	memset( &s_neon, 0, sizeof( s_neon ) );

	s_neon.menu.fullscreen	= qtrue;
	s_neon.menu.wrapAround	= qtrue;
	s_neon.menu.showlogo	= qtrue;
	s_neon.menu.draw	= NeonStart_Draw;

	s_neon.banner.generic.type	= MTYPE_BTEXT;
	s_neon.banner.generic.flags	= QMF_CENTER_JUSTIFY;
	s_neon.banner.generic.x		= 320;
	s_neon.banner.generic.y		= 80;
	s_neon.banner.string		= "NEON ARENA";
	s_neon.banner.color		= color_white;
	s_neon.banner.style		= style;

	y = 160;
	Neon_InitPText( &s_neon.play,  y, ID_PLAY,  "PLAY",  NeonStart_Event ); y += NEON_MENU_SPACING;
	Neon_InitPText( &s_neon.daily, y, ID_DAILY, "DAILY", NeonStart_Event ); y += NEON_MENU_SPACING;
	Neon_InitPText( &s_neon.ghost, y, ID_GHOST, "GHOST", NeonStart_Event ); y += NEON_MENU_SPACING;
	Neon_InitPText( &s_neon.arena, y, ID_ARENA, "ARENA", NeonStart_Event ); y += NEON_MENU_SPACING;
	Neon_InitPText( &s_neon.setup, y, ID_SETUP, "SETUP", NeonStart_Event ); y += NEON_MENU_SPACING;
	Neon_InitPText( &s_neon.exit,  y, ID_EXIT,  "EXIT",  NeonStart_Event );

	Menu_AddItem( &s_neon.menu, &s_neon.banner );
	Menu_AddItem( &s_neon.menu, &s_neon.play );
	Menu_AddItem( &s_neon.menu, &s_neon.daily );
	Menu_AddItem( &s_neon.menu, &s_neon.ghost );
	Menu_AddItem( &s_neon.menu, &s_neon.arena );
	Menu_AddItem( &s_neon.menu, &s_neon.setup );
	Menu_AddItem( &s_neon.menu, &s_neon.exit );

	trap_Key_SetCatcher( KEYCATCH_UI );
	uis.menusp = 0;
	UI_PushMenu( &s_neon.menu );
}

static void UI_NeonGhostMenu( void ) {
	int y;
	int style;

	style = UI_CENTER | UI_DROPSHADOW;
	memset( &s_ghost, 0, sizeof( s_ghost ) );
	s_ghost.menu.fullscreen	= qtrue;
	s_ghost.menu.wrapAround	= qtrue;
	s_ghost.menu.draw	= NeonGhost_Draw;

	s_ghost.banner.generic.type	= MTYPE_BTEXT;
	s_ghost.banner.generic.flags	= QMF_CENTER_JUSTIFY;
	s_ghost.banner.generic.x	= 320;
	s_ghost.banner.generic.y	= 80;
	s_ghost.banner.string		= "GHOST KIT";
	s_ghost.banner.color		= color_white;
	s_ghost.banner.style		= style;

	y = 180;
	Neon_InitPText( &s_ghost.infil, y, ID_GHOST0, "INFILTRATOR", NeonGhost_Event ); y += NEON_MENU_SPACING;
	Neon_InitPText( &s_ghost.sab,   y, ID_GHOST1, "SABOTEUR",    NeonGhost_Event ); y += NEON_MENU_SPACING;
	Neon_InitPText( &s_ghost.spec,  y, ID_GHOST2, "SPECTRE",     NeonGhost_Event ); y += NEON_MENU_SPACING;
	Neon_InitPText( &s_ghost.back,  y, ID_GHOSTBACK, "BACK",     NeonGhost_Event );

	Menu_AddItem( &s_ghost.menu, &s_ghost.banner );
	Menu_AddItem( &s_ghost.menu, &s_ghost.infil );
	Menu_AddItem( &s_ghost.menu, &s_ghost.sab );
	Menu_AddItem( &s_ghost.menu, &s_ghost.spec );
	Menu_AddItem( &s_ghost.menu, &s_ghost.back );

	UI_PushMenu( &s_ghost.menu );
}

static void UI_NeonArenaMenu( void ) {
	int y;
	int style;

	style = UI_CENTER | UI_DROPSHADOW;
	memset( &s_arena, 0, sizeof( s_arena ) );
	s_arena.menu.fullscreen	= qtrue;
	s_arena.menu.wrapAround	= qtrue;
	s_arena.menu.draw	= NeonArena_Draw;

	s_arena.banner.generic.type	= MTYPE_BTEXT;
	s_arena.banner.generic.flags	= QMF_CENTER_JUSTIFY;
	s_arena.banner.generic.x	= 320;
	s_arena.banner.generic.y	= 80;
	s_arena.banner.string		= "ARENA";
	s_arena.banner.color		= color_white;
	s_arena.banner.style		= style;

	s_arena.pick.generic.type	= MTYPE_SPINCONTROL;
	s_arena.pick.generic.flags	= QMF_PULSEIFFOCUS | QMF_CENTER_JUSTIFY | QMF_SMALLFONT;
	s_arena.pick.generic.id		= ID_ARENA_SPIN;
	s_arena.pick.generic.x		= 320;
	s_arena.pick.generic.y		= 200;
	s_arena.pick.itemnames		= neonArenaTitles;

	y = 280;
	Neon_InitPText( &s_arena.go,   y, ID_ARENA_GO,  "FIGHT", NeonArena_Event ); y += NEON_MENU_SPACING;
	Neon_InitPText( &s_arena.back, y, ID_ARENABACK, "BACK",  NeonArena_Event );

	Menu_AddItem( &s_arena.menu, &s_arena.banner );
	Menu_AddItem( &s_arena.menu, &s_arena.pick );
	Menu_AddItem( &s_arena.menu, &s_arena.go );
	Menu_AddItem( &s_arena.menu, &s_arena.back );

	UI_PushMenu( &s_arena.menu );
}

#else

void UI_NeonStartMenu( void ) {
	UI_SPLevelMenu();
}

#endif
