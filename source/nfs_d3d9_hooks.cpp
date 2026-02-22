/*
 * NFS D3D9 frontend hook bridge.
 */

#include "d3d9/d3d9_device.hpp"
#include "d3d9/d3d9_swapchain.hpp"

#if !defined(_WIN64) && (defined(GAME_MW) || defined(GAME_CARBON) || defined(GAME_UG2) || defined(GAME_UG) || defined(GAME_PS) || defined(GAME_UC))
#ifdef GAME_MW
#include "NFSMW_PreFEngHook.h"
#endif
#ifdef GAME_CARBON
#include "NFSC_PreFEngHook.h"
#endif
#ifdef GAME_UG2
#include "NFSU2_PreFEngHook.h"
#endif
#ifdef GAME_UG
#include "NFSU_PreFEngHook.h"
#endif
#ifdef GAME_PS
#include "NFSPS_PreFEngHook.h"
#endif
#ifdef GAME_UC
#include "NFSUC_PreFEngHook.h"
#endif

static void __stdcall ReShade_Hook()
{
	Direct3DSwapChain9 *const swapchain = reshade::d3d9::get_nfs_implicit_swapchain();
	if (swapchain == nullptr)
		return;

	swapchain->on_nfs_present();
}

#if defined(GAME_UC) || defined(GAME_PS)
int NFSUC_ExitPoint1 = NFSUC_EXIT1;
int NFSUC_ExitPoint2 = NFSUC_EXIT2;
int NFSUC_EntryPoint_EBX = 0;

#ifdef GAME_UC
// Keep legacy motion blur gate hook symbol for existing patch points.
bool bGlobalMotionBlur = true;
int NFSUC_MOTIONBLUR_ExitPointTrue = NFSUC_MOTIONBLUR_EXIT_TRUE;
int NFSUC_MOTIONBLUR_ExitPointFalse = NFSUC_MOTIONBLUR_EXIT_FALSE;
void __declspec(naked) MotionBlur_EntryPoint()
{
	if (!bGlobalMotionBlur)
		_asm jmp NFSUC_MOTIONBLUR_ExitPointFalse
	_asm
	{
		push 0xA
		mov ecx, 0xDF1DE0
		jmp NFSUC_MOTIONBLUR_ExitPointTrue
	}
}
#endif

void __declspec(naked) ReShade_EntryPoint()
{
	_asm mov NFSUC_EntryPoint_EBX, ebx
	ReShade_Hook();
	if (*reinterpret_cast<bool *>(NFSUC_EntryPoint_EBX + 0xA))
		_asm jmp NFSUC_ExitPoint1
	_asm jmp NFSUC_ExitPoint2
}
#else
void(__thiscall *FEManager_Render)(unsigned int dis) = reinterpret_cast<void(__thiscall *)(unsigned int)>(FEMANAGER_RENDER_ADDRESS);

void __stdcall FEManager_Render_Hook()
{
	unsigned int the_this = 0;
	_asm mov the_this, ecx

	ReShade_Hook();
	FEManager_Render(the_this);
}
#endif
#endif
