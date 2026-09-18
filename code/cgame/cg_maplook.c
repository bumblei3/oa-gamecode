// NeonArena per-arena look. Own TU so q3asm keeps vmMain at instruction 0 in cg_main.
#include "cg_local.h"

#ifdef NEONARENA_MOD
#include "../game/neon_maplook.h"

static void CG_SetHundredths( const char *name, float v ) {
	char col[16];
	int x;

	x = (int)( v * 100.0f + 0.5f );
	if ( x < 0 ) {
		x = 0;
	}
	Com_sprintf( col, sizeof( col ), "%i.%02i", x / 100, x % 100 );
	trap_Cvar_Set( name, col );
}

void CG_ApplyMapLook( void ) {
	const nwMapLook_t *look;
	char arena[64];

	trap_Cvar_VariableStringBuffer( "g_neonwave_arena", arena, sizeof( arena ) );
	look = NW_MapLookArena( arena, cgs.mapname );
	trap_Cvar_Set( "r_gamma", look->gamma );
	trap_Cvar_Set( "r_bloom_intensity", look->bloom_i );
	trap_Cvar_Set( "r_bloom_threshold", look->bloom_t );
	trap_Cvar_Set( "cg_neon_grid", look->grid );
	CG_SetHundredths( "cg_neon_grid_r", look->gr );
	CG_SetHundredths( "cg_neon_grid_g", look->gg );
	CG_SetHundredths( "cg_neon_grid_b", look->gb );
}

#endif
