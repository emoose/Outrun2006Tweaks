#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

#include "d3d9.h"

class DisableDPIScaling : public Hook
{
public:
	std::string_view description() override
	{
		return "DisableDPIScaling";
	}

	bool validate() override
	{
		return Settings::DisableDPIScaling;
	}

	bool apply() override
	{
		SetProcessDPIAware();
		return true;
	}

	static DisableDPIScaling instance;
};
DisableDPIScaling DisableDPIScaling::instance;

class LensFlarePathFix : public Hook
{
	const static int LoadLensFlareOffset_StringAddr = 0x1A29F8;

public:
	std::string_view description() override
	{
		return "LensFlarePathFix";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		// Game code tries to load lens_flare_offset.bin from the wrong path
		// Code tries loading from common/, but game files have it inside media/
		// We'll just patch code to load from media/ instead
		// (only patch it if file actually exists inside media/ though, some fix guides already told people to move it to common/)

		std::string NewPath = "\\media\\lens_flare_offset.bin";
		if (std::filesystem::exists("." + NewPath))
		{
			auto* patch_addr = Module::exe_ptr<char>(LoadLensFlareOffset_StringAddr);

			DWORD dwProtect;
			VirtualProtect((void*)patch_addr, NewPath.length(), PAGE_EXECUTE_READWRITE, &dwProtect);
			strcpy(patch_addr, NewPath.c_str());
			VirtualProtect((void*)patch_addr, NewPath.length(), dwProtect, &dwProtect);
		}

		return true;
	}

	static LensFlarePathFix instance;
};
LensFlarePathFix LensFlarePathFix::instance;

class ScreenEdgeCullFix : public Hook
{
	const static int CalcBall3D2D_Addr = 0x49E70;

	// Hook CalcBall3D2D to rescale screen-ratio positions back to 4:3 positions that game code expects
	// (fixes objects like cones being culled out before they reach edge of the screen)
	inline static SafetyHookInline dest_orig = {};
	static float __cdecl destination(float a1, Sphere* a2, Sphere* a3)
	{
		float ret = dest_orig.ccall<float>(a1, a2, a3);

		constexpr float ratio_4_3 = 4.f / 3.f;

		float ratio_screen = *Game::screen_width / *Game::screen_height;

		a3->f0 = (a3->f0 / ratio_screen) * ratio_4_3;
		a3->f1 = (a3->f1 * ratio_screen) / ratio_4_3;
		return ret;
	}

public:
	std::string_view description() override
	{
		return "ScreenEdgeCullFix";
	}

	bool validate() override
	{
		return Settings::ScreenEdgeCullFix;
	}

	bool apply() override
	{
		dest_orig = safetyhook::create_inline(Module::exe_ptr(CalcBall3D2D_Addr), destination);
		return !!dest_orig;
	}

	static ScreenEdgeCullFix instance;
};
ScreenEdgeCullFix ScreenEdgeCullFix::instance;

class DisableStageCulling : public Hook
{
	const static int CalcCulling_PatchAddr = 0x501F;

public:
	std::string_view description() override
	{
		return "DisableStageCulling";
	}

	bool validate() override
	{
		return Settings::DisableStageCulling;
	}

	bool apply() override
	{
		// Patch "if (CheckCulling(...))" -> no-op
		Memory::VP::Patch(Module::exe_ptr(CalcCulling_PatchAddr), { 0x90, 0x90 });
		return true;
	}

	static DisableStageCulling instance;
};
DisableStageCulling DisableStageCulling::instance;

class DisableVehicleLODs : public Hook
{
	const static int DispOthcar_PatchAddr = 0xAE4E9;
public:
	std::string_view description() override
	{
		return "DisableVehicleLODs";
	}

	bool validate() override
	{
		return Settings::DisableVehicleLODs;
	}

	bool apply() override
	{
		// Patch "eax = car.LodNumber" -> "eax = 0"
		Memory::VP::Patch(Module::exe_ptr(DispOthcar_PatchAddr), { 0x90, 0x31, 0xC0 });

		return true;
	}

	static DisableVehicleLODs instance;
};
DisableVehicleLODs DisableVehicleLODs::instance;

class TransparencySupersampling : public Hook
{
	const static int DeviceInitHookAddr = 0xEC2F;
	const static int DeviceResetHookAddr = 0x17A20;

	inline static SafetyHookMid dest_hook = {};
	inline static SafetyHookMid deviceReset_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		auto* device = Game::D3DDevice();
		device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
		device->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, TRUE);
		device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		device->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);

		// NVIDIA transparency supersampling
		device->SetRenderState(D3DRS_ADAPTIVETESS_Y, D3DFORMAT(MAKEFOURCC('S', 'S', 'A', 'A')));
	}

public:
	std::string_view description() override
	{
		return "TransparencySupersampling";
	}

	bool validate() override
	{
		return Settings::TransparencySupersampling;
	}

	bool apply() override
	{
		dest_hook = safetyhook::create_mid(Module::exe_ptr(DeviceInitHookAddr), destination);
		deviceReset_hook = safetyhook::create_mid(Module::exe_ptr(DeviceResetHookAddr), destination);
		return !!dest_hook;
	}

	static TransparencySupersampling instance;
};
TransparencySupersampling TransparencySupersampling::instance;

class WindowedHideMouseCursor : public Hook
{
	const static int WndProc_Addr = 0x17F90;

	inline static SafetyHookInline dest_orig = {};
	static LRESULT __stdcall destination(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msg == WM_SETFOCUS || (msg == WM_ACTIVATE && lParam != WA_INACTIVE))
		{
			ShowCursor(false);
		}
		else if (msg == WM_ERASEBKGND) // erase window to white during device reset
		{
			RECT rect;
			GetClientRect(hwnd, &rect);
			HBRUSH brush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
			FillRect((HDC)wParam, &rect, brush);
			DeleteObject(brush);
			return 1;
		}
		// Other message handling
		return dest_orig.stdcall<LRESULT>(hwnd, msg, wParam, lParam);
	}

public:
	std::string_view description() override
	{
		return "WindowedHideMouseCursor";
	}

	bool validate() override
	{
		return Settings::WindowedHideMouseCursor;
	}

	bool apply() override
	{
		dest_orig = safetyhook::create_inline(Module::exe_ptr(WndProc_Addr), destination);
		return !!dest_orig;
	}

	static WindowedHideMouseCursor instance;
};
WindowedHideMouseCursor WindowedHideMouseCursor::instance;

class WindowedBorderless : public Hook
{
	const static int WinMain_BorderlessWindow_WndStyleExAddr = 0x18175;
	const static int WinMain_BorderlessWindow_WndStyleAddr = 0x1817A;
	const static int Win32_Init_DisableWindowResize_Addr1 = 0xE9E3;
	const static int Win32_Init_DisableWindowResize_Addr2 = 0xEA30;
	const static int Win32_Init_SetWindowPos_Addr = 0xEAA7;

	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		SetWindowPos(HWND(ctx.ebp), 0, 
			Settings::WindowPositionX, Settings::WindowPositionY, 
			*Game::screen_width, *Game::screen_height,
			0x40);
	}

public:
	std::string_view description() override
	{
		return "WindowedBorderless";
	}

	bool validate() override
	{
		return Settings::WindowedBorderless;
	}

	bool apply() override
	{
		auto* patch_addr = Module::exe_ptr(WinMain_BorderlessWindow_WndStyleExAddr);
		Memory::VP::Patch(patch_addr, uint32_t(0));

		patch_addr = Module::exe_ptr(WinMain_BorderlessWindow_WndStyleAddr);
		Memory::VP::Patch(patch_addr, uint32_t(WS_POPUP));

		patch_addr = Module::exe_ptr(Win32_Init_DisableWindowResize_Addr1);
		Memory::VP::Nop(patch_addr, 6);

		patch_addr = Module::exe_ptr(Win32_Init_DisableWindowResize_Addr2);
		Memory::VP::Nop(patch_addr, 6);

		// replace original SetWindowPos call
		Memory::VP::Nop(Module::exe_ptr(Win32_Init_SetWindowPos_Addr), 0x23);
		dest_hook = safetyhook::create_mid(Module::exe_ptr(Win32_Init_SetWindowPos_Addr), destination);

		return true;
	}

	static WindowedBorderless instance;
};
WindowedBorderless WindowedBorderless::instance;

class AnisotropicFiltering : public Hook
{
	const static int ChangeTexAttribute_HookAddr1 = 0x9AD8;
	const static int ChangeTexAttribute_HookAddr2 = 0x8960;

	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		int Sampler = ctx.ebp;

		Game::D3DDevice()->SetSamplerState(Sampler, D3DSAMP_MAXANISOTROPY, 16);
	}

	inline static SafetyHookMid dest_hook2 = {};
	static void destination2(safetyhook::Context& ctx)
	{
		int Sampler = *(int*)(ctx.esp + 0xC);

		if (ctx.edi == D3DSAMP_MINFILTER || ctx.edi == D3DSAMP_MAGFILTER)
		{
			ctx.esi = D3DTEXF_ANISOTROPIC;

			Game::D3DDevice()->SetSamplerState(Sampler, D3DSAMP_MAXANISOTROPY, 16);
		}
	}

public:
	std::string_view description() override
	{
		return "AnisotropicFiltering";
	}

	bool validate() override
	{
		return Settings::AnisotropicFiltering > 0;
	}

	bool apply() override
	{
		dest_hook = safetyhook::create_mid(Module::exe_ptr(ChangeTexAttribute_HookAddr1), destination);
		dest_hook2 = safetyhook::create_mid(Module::exe_ptr(ChangeTexAttribute_HookAddr2), destination2);

		return true;
	}

	static AnisotropicFiltering instance;
};
AnisotropicFiltering AnisotropicFiltering::instance;

class D3DPresentParamOverrides : public Hook
{
	const static int D3DInit_HookAddr = 0xEB66;

	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		Game::D3DPresentParams->PresentationInterval = Settings::VSync;
		if (!Settings::VSync)
			Game::D3DPresentParams->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	}

public:
	std::string_view description() override
	{
		return "D3DPresentParamOverrides";
	}

	bool validate() override
	{
		return Settings::VSync != 1;
	}

	bool apply() override
	{
		dest_hook = safetyhook::create_mid(Module::exe_ptr(D3DInit_HookAddr), destination);

		return true;
	}

	static D3DPresentParamOverrides instance;
};
D3DPresentParamOverrides D3DPresentParamOverrides::instance;
