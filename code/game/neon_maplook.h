// Per-BSP Neon look, with optional arena-stem overrides.
// Included by game, cgame (cg_maplook.c only — never cg_main.c), and q3_ui.
// overbright is latched (set before map load). bloom/gamma/grid apply live.
#ifndef NEON_MAPLOOK_H
#define NEON_MAPLOOK_H

#define NW_GRID_CYAN	0.12f, 0.55f, 0.70f
#define NW_GRID_ICE	0.40f, 0.78f, 1.00f
#define NW_GRID_IRON	0.90f, 0.22f, 0.12f
#define NW_GRID_GHOST	0.08f, 0.95f, 1.00f
#define NW_GRID_MOSS	0.18f, 0.72f, 0.28f
#define NW_GRID_BLOOD	0.85f, 0.12f, 0.18f

typedef struct {
	const char	*key;		/* bsp name, or arena stem for overrides */
	int		overbright;	/* r_mapoverbrightbits: 0 darkest, 1 default neon */
	const char	*gamma;
	const char	*bloom_i;	/* r_bloom_intensity */
	const char	*bloom_t;	/* r_bloom_threshold */
	const char	*grid;		/* cg_neon_grid overlay 0–1 */
	float		gr, gg, gb;	/* HUD grid tint */
} nwMapLook_t;

static const nwMapLook_t nw_map_look[] = {
	{ "oa_shine",		1, "1.40", "0.50", "0.60", "0.18", NW_GRID_CYAN },
	{ "oa_minia",		1, "1.40", "0.48", "0.62", "0.16", NW_GRID_CYAN },
	{ "oa_rpg3dm2",		1, "1.40", "0.50", "0.60", "0.16", NW_GRID_CYAN },
	{ "slimefac",		1, "1.45", "0.55", "0.55", "0.32", NW_GRID_BLOOD },
	{ "oa_dm1",		1, "1.42", "0.52", "0.58", "0.30", NW_GRID_CYAN },
	{ "oa_dm3",		0, "1.25", "0.40", "0.70", "0.35", NW_GRID_IRON },
	{ "islanddm",		0, "1.20", "0.35", "0.72", "0.38", NW_GRID_CYAN },
	{ "oa_dm6",		1, "1.40", "0.50", "0.60", "0.28", NW_GRID_CYAN },
	{ "suspended",		1, "1.38", "0.48", "0.62", "0.30", NW_GRID_CYAN },
	{ "am_underworks",	1, "1.45", "0.55", "0.55", "0.30", NW_GRID_CYAN },
	{ "hydronex",		1, "1.42", "0.52", "0.58", "0.28", NW_GRID_CYAN },
	{ "am_galmevish",	1, "1.40", "0.48", "0.62", "0.26", NW_GRID_MOSS },
	{ "oa_thor",		1, "1.40", "0.50", "0.60", "0.28", NW_GRID_CYAN }
};

/* Same BSP can host two arenas (oa_minia = Ironman + Frostbite). Stem wins. */
static const nwMapLook_t nw_arena_look[] = {
	{ "frostbite",		1, "1.22", "0.38", "0.72", "0.40", NW_GRID_ICE },
	{ "ironman",		0, "1.10", "0.28", "0.80", "0.20", NW_GRID_IRON },
	{ "ghost_protocol",	1, "1.45", "0.58", "0.50", "0.26", NW_GRID_GHOST },
	{ "overgrowth",		1, "1.32", "0.44", "0.66", "0.24", NW_GRID_MOSS },
	{ "bleed_chamber",	1, "1.50", "0.60", "0.48", "0.38", NW_GRID_BLOOD }
};

#define NW_MAP_LOOK_COUNT ( (int)( sizeof( nw_map_look ) / sizeof( nw_map_look[0] ) ) )
#define NW_ARENA_LOOK_COUNT ( (int)( sizeof( nw_arena_look ) / sizeof( nw_arena_look[0] ) ) )

static const nwMapLook_t *NW_MapLook( const char *map ) {
	char name[64];
	int i, n;
	const char *s;

	if ( !map || !map[0] ) {
		return &nw_map_look[0];
	}
	s = map;
	if ( s[0] == 'm' && s[1] == 'a' && s[2] == 'p' && s[3] == 's' && s[4] == '/' ) {
		s += 5;
	}
	Q_strncpyz( name, s, sizeof( name ) );
	n = (int)strlen( name );
	if ( n > 4 && name[n - 4] == '.' && ( name[n - 3] == 'b' || name[n - 3] == 'B' ) ) {
		name[n - 4] = '\0';
	}
	for ( i = 0; i < NW_MAP_LOOK_COUNT; i++ ) {
		if ( !Q_stricmp( name, nw_map_look[i].key ) ) {
			return &nw_map_look[i];
		}
	}
	return &nw_map_look[0];
}

static const nwMapLook_t *NW_MapLookArena( const char *stem, const char *map ) {
	int i;

	if ( stem && stem[0] ) {
		for ( i = 0; i < NW_ARENA_LOOK_COUNT; i++ ) {
			if ( !Q_stricmp( stem, nw_arena_look[i].key ) ) {
				return &nw_arena_look[i];
			}
		}
	}
	return NW_MapLook( map );
}

#endif
