#pragma once
// NFS UG2 - ReShade Pre FENg Hook
#ifndef OLD_NFS
#define OLD_NFS
#endif
#ifndef HAS_FOG_CTRL
#define HAS_FOG_CTRL
#endif
#define FEMANAGER_RENDER_HOOKADDR1 0x005CC1FB
#define FEMANAGER_RENDER_HOOKADDR2 0x005CC1FB
#define FEMANAGER_RENDER_ADDRESS 0x005378C0
#define NFS_D3D9_DEVICE_ADDRESS 0x00870974
#define DRAW_FENG_BOOL_ADDR 0x007F91C4

#define PLAYERBYINDEX_ADDR 0x008900B4
#define CAR_RESETTOPOS_ADDR 0x005FBD60
#define SAVEHOTPOS_ADDR 0x008900CC
#define LOADHOTPOS_ADDR 0x008900CD

#define EXITGAMEFLAG_ADDR 0x00864F4C
#define GAMEFLOWMGR_ADDR 0x00865480
#define GAMEFLOWMGR_STATUS_ADDR 0x008654A4
#define GAMEFLOWMGR_UNLOADFE_ADDR 0x00578320
#define GAMEFLOWMGR_UNLOADTRACK_ADDR 0x005819C0
#define GAMEFLOWMGR_LOADREGION_ADDR 0x00581020
#define SKIPFE_ADDR 0x00864FAC
#define SKIPFETRACKNUM_ADDR 0x0089E7A0
#define SKIPFETRACKNUM_ADDR2 0x007FAA18
#define DEFAULT_TRACK_NUM 4000
#define STARTSKIPFERACE_ADDR 0x0053FEB0
#define ONLINENABLED_ADDR 0x00865084

#define SKIPFE_NUMAICARS_ADDR 0x00864FB0
#define SKIPFE_TRACKDIRECTION_ADDR 0x00864FB4
//#define SKIPFE_BEACOP_ADDR 0x0073466C
#define SKIPFE_TRAFFICDENSITY_ADDR 0x00864FBC
//#define SKIPFE_TRAFFICONCOMING_ADDR 0x006F1D78
#define SKIPFE_DRAGRACE_ADDR 0x00864FC0
#define SKIPFE_DRIFTRACE_ADDR 0x00864FC4
#define SKIPFE_P2P_ADDR 0x00864FD4
#define SKIPFE_NUMPLAYERCARS_ADDR 0x007FAA1C
#define SKIPFE_NUMLAPS_ADDR 0x007FAA20
#define SKIPFE_RACETYPE_ADDR 0x007FAA28
#define SKIPFE_DIFFICULTY_ADDR 0x007FAA30
#define SKIPFE_DRIFTRACETEAMED_ADDR 0x00864FC8
#define SKIPFE_BURNOUTRACE_ADDR 0x00864FCC
#define SKIPFE_SHORTTRACK_ADDR 0x00864FD0
#define SKIPFE_ROLLINGSTART_SPEED 0x00864FE0 // float

#define SKIPFE_DEFAULTPLAYER1CARTYPE_ADDR 0x007FAA48
#define SKIPFE_DEFAULTPLAYER2CARTYPE_ADDR 0x007FAA4C
#define SKIPFE_DEFAULTPLAYER1SKININDEX_ADDR 0x007FAA54
#define SKIPFE_DEFAULTPLAYER2SKININDEX_ADDR 0x007FAA58
#define SKIPFE_PLAYERCARUPGRADEALL_ADDR 0x007FAAC0
#define SKIPFE_FORCEPLAYER1STARTPOS_ADDR 0x00864FE4
#define SKIPFE_FORCEPLAYER2STARTPOS_ADDR 0x00864FE8

#define SKIPFE_FORCEALLAICARSTOBETHISTYPE_ADDR 0x007FAA78
#define SKIPFE_FORCEAICAR1TOBETHISTYPE_ADDR 0x007FAA7C
#define SKIPFE_FORCEAICAR2TOBETHISTYPE_ADDR 0x007FAA80
#define SKIPFE_FORCEAICAR3TOBETHISTYPE_ADDR 0x007FAA84
#define SKIPFE_FORCEAICAR4TOBETHISTYPE_ADDR 0x007FAA88
#define SKIPFE_FORCEAICAR5TOBETHISTYPE_ADDR 0x007FAA8C
#define SKIPFE_FORCEALLAICARSTOPERFRATING_ADDR 0x007FAA90
#define SKIPFE_FORCEAICAR1TOPERFRATING_ADDR 0x007FAA94
#define SKIPFE_FORCEAICAR2TOPERFRATING_ADDR 0x007FAA98
#define SKIPFE_FORCEAICAR3TOPERFRATING_ADDR 0x007FAA9C
#define SKIPFE_FORCEAICAR4TOPERFRATING_ADDR 0x007FAAA0
#define SKIPFE_FORCEAICAR5TOPERFRATING_ADDR 0x007FAAA4

#define UNLOCKALLTHINGS_ADDR 0x00838464

// NFSU2 specific values
#define SHUTUPRACHEL_ADDR 0x00864FA8
#define UNLIMITEDCASHOLAZ_ADDR 0x00838440
// Precipitation stuff
#define PRECIPITATION_ENABLE_ADDR 0x007FAA50
#define PRECIPITATION_DEBUG_ADDR 0x008A1D38
#define PRECIPITATION_RENDER_ADDR 0x00803A60
#define PRECIPITATION_PERCENT_ADDR 0x00803968
#define PRECIP_RAINX_ADDR 0x0080396C
#define PRECIP_RAINY_ADDR 0x00803970
#define PRECIP_RAINZ_ADDR 0x00803974
#define PRECIP_RAINZCONSTANT_ADDR 0x00803978
#define PRECIP_SNOWX_ADDR 0x0080397C
#define PRECIP_SNOWY_ADDR 0x00803980
#define PRECIP_SNOWZ_ADDR 0x00803984
#define PRECIP_SNOWZCONSTANT_ADDR 0x00803988
#define PRECIP_SLEETX_ADDR 0x0080398C
#define PRECIP_SLEETY_ADDR 0x00803990
#define PRECIP_SLEETZ_ADDR 0x00803994
#define PRECIP_SLEETZCONSTANT_ADDR 0x00803998
#define PRECIP_HAILX_ADDR 0x0080399C
#define PRECIP_HAILY_ADDR 0x008039A0
#define PRECIP_HAILZ_ADDR 0x008039A4
#define PRECIP_HAILZCONSTANT_ADDR 0x008039A8
#define PRECIP_BOUNDX_ADDR 0x008039AC
#define PRECIP_BOUNDY_ADDR 0x008039B0
#define PRECIP_BOUNDZ_ADDR 0x008039B4
#define PRECIP_AHEADX_ADDR 0x008039B8
#define PRECIP_AHEADY_ADDR 0x008A1D3C
#define PRECIP_AHEADZ_ADDR 0x008A1D40
#define PRECIP_RAINWINDEFF_ADDR 0x008039CC
#define PRECIP_SNOWWINDEFF_ADDR 0x008039D0
#define PRECIP_SLEETWINDEFF_ADDR 0x008039D4
#define PRECIP_HAILWINDEFF_ADDR 0x008039D8
#define PRECIP_RAINRADIUSX_ADDR 0x008039DC
#define PRECIP_RAINRADIUSY_ADDR 0x008039E0
#define PRECIP_RAINRADIUSZ_ADDR 0x008039E4
#define PRECIP_SNOWRADIUSX_ADDR 0x008039E8
#define PRECIP_SNOWRADIUSY_ADDR 0x008039EC
#define PRECIP_SNOWRADIUSZ_ADDR 0x008039F0
#define PRECIP_SLEETRADIUSX_ADDR 0x008039F4
#define PRECIP_SLEETRADIUSY_ADDR 0x008039F8
#define PRECIP_SLEETRADIUSZ_ADDR 0x008039FC
#define PRECIP_HAILRADIUSX_ADDR 0x00803A00
#define PRECIP_HAILRADIUSY_ADDR 0x00803A04
#define PRECIP_HAILRADIUSZ_ADDR 0x00803A08
#define PRECIP_WEATHERCHANGE_ADDR 0x00803A10
#define PRECIP_DRIVEFACTOR_ADDR 0x00803A14
#define PRECIP_SNOWPERCENT_ADDR 0x00803A64
#define PRECIP_RAINPERCENT_ADDR 0x00803A68
#define PRECIP_FOGPERCENT_ADDR 0x008A1D44
#define PRECIP_CHANCE100_ADDR 0x008A1D48
#define PRECIP_RAININTHEHEADLIGHTS_ADDR 0x00803A70
#define PRECIP_WINDANG_ADDR 0x008A1D50
#define PRECIP_SWAYMAX_ADDR 0x00803A78
#define PRECIP_MAXWINDEFF_ADDR 0x00803A80
#define PRECIP_PREVAILINGMULT_ADDR 0x00803A80
#define PRECIP_ONSCREEN_DRIPSPEED_ADDR 0x00803AC0
#define PRECIP_ONSCREEN_SPEEDMOD_ADDR 0x00803AC4
#define PRECIP_ONSCREEN_OVERRIDE_ADDR 0x008A1D58
#define PRECIP_BASEDAMPNESS_ADDR 0x00803AC8
#define PRECIP_UBERDAMPNESS_ADDR 0x00803ACC
#define FOG_CTRLOVERRIDE_ADDR 0x008A1D70
#define DEFAULT_RAIN_TYPE 5

#define EGETVIEW_ADDR 0x0048B1E0
#define RAININIT_ADDR 0x00613050
#define SETRAIN_HOOK_ADDR 0x005FF695

#define DOSCREENPRINTF_ADDR 0x008650A0
#define DEBUGCAMERASENABLED_ADDR 0x00865098

#define BUILDVERSIONCLNAME_ADDR 0x007FA734
#define BUILDVERSIONMACHINE_ADDR 0x007FA730
#define BUILDVERSIONCLNUMBER_ADDR 0x007FA738
//#define BUILDVERSIONDATE_ADDR 0x008F8698
//#define BUILDVERSIONPLAT_ADDR 0x008A87A4
//#define BUILDVERSIONNAME_ADDR 0x008A8EAC
//#define BUILDVERSIONOPTNAME_ADDR 0x008A8EA8

#define DRAWCARS_ADDR 0x008026C8
#define DRAWCARSREFLECTIONS_ADDR 0x008026CC
#define DRAWLIGHTFLARES_ADDR 0x007F3D40
#define DRAWFANCYCARSHADOW_ADDR 0x008026B8
#define FANCYCARSHADOWEDGEMULT_ADDR 0x008026BC
#define WHEELPIVOTTRANSLATIONAMOUNT_ADDR 0x008026C0
#define WHEELSTANDARDWIDTH_ADDR 0x008026C4

#define SHOWALLCARSINFE_ADDR 0x00864FF4
#define FENG_PINSTANCE_ADDR 0x008384C4
//#define FENG_PUSHER_ADDR 0x00555D00
#define FENG_PUSHPACKAGE_ADDR 0x00555E80
#define FENG_POPPACKAGE_ADDR 0x005379A0
#define FENG_SWITCHPACKAGE_ADDR 0x00537980

#define BASEFOG_FALLOFF_ADDR 0x00800E18
#define BASEFOG_FALLOFFX_ADDR 0x00800E1C
#define BASEFOG_FALLOFFY_ADDR 0x00800E20
#define BASEWEATHER_FOG_ADDR 0x00800E24
#define BASEWEATHER_FOG_START_ADDR 0x00800E28
#define BASEWEATHER_FOG_COLOUR_R_ADDR 0x00800E34
#define BASEWEATHER_FOG_COLOUR_G_ADDR 0x00800E30
#define BASEWEATHER_FOG_COLOUR_B_ADDR 0x00800E2C

void __stdcall FEManager_Render_Hook();
void __stdcall SetRainBase_Custom();