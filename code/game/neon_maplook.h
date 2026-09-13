// Per-BSP Neon look. Included by game, cgame, and q3_ui.
// overbright is latched (set before map load). bloom/gamma/grid apply live.
#ifndef NEON_MAPLOOK_H
#define NEON_MAPLOOK_H

typedef struct {
	const char	*bsp;
	int		overbright;	/* r_mapoverbrightbits: 0 darkest, 1 default neon */
	const char	*gamma;
	const char	*bloom_i;	/* r_bloom_intensity */
	const char	*bloom_t;	/* r_bloom_threshold */
	const char	*grid;		/* cg_neon_grid overlay 0–1 */
} nwMapLook_t;

static const nwMapLook_t nw_map_look[] = {
	{ "oa_shine",		1, "1.40", "0.50", "0.60", "0.18" },
	{ "oa_minia",		1, "1.40", "0.48", "0.62", "0.16" },
	{ "oa_rpg3dm2",		1, "1.40", "0.50", "0.60", "0.16" },
	{ "slimefac",		1, "1.45", "0.55", "0.55", "0.32" },
	{ "oa_dm1",		1, "1.42", "0.52", "0.58", "0.30" },
	{ "oa_dm3",		0, "1.25", "0.40", "0.70", "0.35" },
	{ "islanddm",		0, "1.20", "0.35", "0.72", "0.38" },
	{ "oa_dm6",		1, "1.40", "0.50", "0.60", "0.28" },
	{ "suspended",		1, "1.38", "0.48", "0.62", "0.30" },
	{ "am_underworks",	1, "1.45", "0.55", "0.55", "0.30" },
	{ "hydronex",		1, "1.42", "0.52", "0.58", "0.28" },
	{ "am_galmevish",	1, "1.40", "0.48", "0.62", "0.26" },
	{ "oa_thor",		1, "1.40", "0.50", "0.60", "0.28" }
};

#define NW_MAP_LOOK_COUNT ( (int)( sizeof( nw_map_look ) / sizeof( nw_map_look[0] ) ) )

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
		if ( !Q_stricmp( name, nw_map_look[i].bsp ) ) {
			return &nw_map_look[i];
		}
	}
	return &nw_map_look[0];
}

#endif
