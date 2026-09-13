// Shared Daily map pool. Included by g_neonwave.c and q3_ui/ui_neonstart.c.
// Keys stay stable (tests assert DAILY MAP oa_bleed for seed 12345).
// bsp[] is the loadable OpenArena map.
#ifndef NEON_DAILY_POOL_H
#define NEON_DAILY_POOL_H

#define NW_DAILY_POOL_SIZE	14
#define NW_DAILY_FNV_PRIME	16777619u
#define NW_DAILY_FNV_OFFSET	2166136261u
#define NW_DAILY_MOD_POOL	16
#define NW_DAILY_BOSS_N		13

static const char *nw_daily_key[NW_DAILY_POOL_SIZE] = {
	"oa_shine",
	"oa_minia",
	"oa_rpg3dm2",
	"oa_bleed",
	"oa_node",
	"oa_pulse",
	"oa_desert",
	"oa_vortex",
	"oa_frostbite",
	"oa_skybridge",
	"oa_underhive",
	"oa_reactor",
	"oa_overgrowth",
	"oa_thor"
};

static const char *nw_daily_bsp[NW_DAILY_POOL_SIZE] = {
	"oa_shine",
	"oa_minia",
	"oa_rpg3dm2",
	"slimefac",
	"oa_dm1",
	"oa_dm3",
	"islanddm",
	"oa_dm6",
	"oa_minia",
	"suspended",
	"am_underworks",
	"hydronex",
	"am_galmevish",
	"oa_thor"
};

/* configs/arenas stem; empty = no JSON (same as start-quake3e.sh POOL) */
static const char *nw_daily_arena[NW_DAILY_POOL_SIZE] = {
	"neon_arena",
	"",
	"catacombs",
	"bleed_chamber",
	"node_control",
	"",
	"desert_storm",
	"vortex_ring",
	"frostbite",
	"skybridge",
	"underhive",
	"reactor",
	"overgrowth",
	""
};

static unsigned int NW_DailyHash( const char *s ) {
	unsigned int h = NW_DAILY_FNV_OFFSET;
	while ( *s ) {
		h ^= (unsigned char)*s++;
		h *= NW_DAILY_FNV_PRIME;
	}
	return h;
}

static int NW_DailyPoolIndex( int forced ) {
	return ( forced / ( NW_DAILY_MOD_POOL * NW_DAILY_BOSS_N ) ) % NW_DAILY_POOL_SIZE;
}

#endif
