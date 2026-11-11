/* Controllers that Steam hides from the game.
 *
 * Once a process has called SteamAPI_Init, the Steam client loads its overlay
 * into it, and with Steam Input enabled for a controller type the overlay
 * hides those controllers from every input API in the process: XInput reports
 * "not connected", raw input and HID enumerate nothing, and SDL lists no
 * joystick at all. A game launched from Steam is handed a virtual controller
 * in exchange. One that only initialised the API (ours, under a borrowed app
 * id) is handed nothing, so the pad that worked on the title screen goes dead
 * the moment Steam comes up. Nothing in the environment turns this off.
 *
 * The hiding is done by patching the system XInput library already loaded in
 * the process. A private copy of that library, loaded under another name, is
 * not patched and still talks to the device. Each slot that the patched
 * library calls absent and the private copy calls present is exposed to SDL
 * as a virtual game controller fed from the private copy, so the rest of the
 * input stack -- binds, glyphs, the controls panel -- sees an ordinary pad.
 *
 * XInput devices only. A DualSense or Switch pad that Steam hides is reachable
 * only through Steam's own input API. */
#include <SDL.h>

#include "../PsyX_main.h"

extern "C" void PsyX_Pad_SteamBypassTick(void);

#if defined(_WIN32) && SDL_VERSION_ATLEAST(2, 24, 0)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct ShXiGamepad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct ShXiState   { DWORD packet; ShXiGamepad pad; };
struct ShXiRumble  { WORD low, high; };

typedef DWORD (WINAPI *ShXiGetState)(DWORD, ShXiState*);
typedef DWORD (WINAPI *ShXiSetState)(DWORD, ShXiRumble*);

enum { SH_XI_SLOTS = 4, SH_XI_OK = 0 };

static ShXiGetState s_sysGet, s_ownGet;
static ShXiSetState s_ownSet;
static int          s_loaded;                 /* 0 not tried, 1 ready, -1 unavailable */
static int          s_virtIndex[SH_XI_SLOTS]; /* SDL device index + 1; 0 = none */
static SDL_Joystick* s_virtJoy[SH_XI_SLOTS];
static Uint32       s_nextScan;

extern int g_PsyX_PadFilterMode;

static int SteamBypass_Load(void)
{
	char sys[MAX_PATH], tmp[MAX_PATH], own[MAX_PATH];
	HMODULE sysDll, ownDll;
	UINT n;

	if (s_loaded != 0)
		return s_loaded > 0;
	s_loaded = -1;

	n = GetSystemDirectoryA(sys, sizeof(sys));
	if (n == 0 || n > sizeof(sys) - 20 || GetTempPathA(sizeof(tmp), tmp) == 0)
		return 0;
	SDL_strlcat(sys, "\\xinput1_4.dll", sizeof(sys));
	SDL_snprintf(own, sizeof(own), "%ssh1pc_xinput_%lu.dll", tmp, (unsigned long)GetCurrentProcessId());
	if (!CopyFileA(sys, own, FALSE))
	{
		eprintwarn("[PAD] Steam is hiding a controller and the XInput copy could not be made (%lu)\n", (unsigned long)GetLastError());
		return 0;
	}
	sysDll = GetModuleHandleA("xinput1_4.dll");
	if (sysDll == NULL)
		sysDll = LoadLibraryA("xinput1_4.dll");
	ownDll = LoadLibraryA(own);
	/* The file stays mapped; it goes with the process or at the next boot. */
	MoveFileExA(own, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
	if (sysDll == NULL || ownDll == NULL)
		return 0;
	s_sysGet = (ShXiGetState)(void*)GetProcAddress(sysDll, "XInputGetState");
	s_ownGet = (ShXiGetState)(void*)GetProcAddress(ownDll, "XInputGetState");
	s_ownSet = (ShXiSetState)(void*)GetProcAddress(ownDll, "XInputSetState");
	if (s_sysGet == NULL || s_ownGet == NULL)
		return 0;
	s_loaded = 1;
	return 1;
}

static void SDLCALL SteamBypass_Update(void* userdata)
{
	const int     slot = (int)(intptr_t)userdata;
	SDL_Joystick* js   = s_virtJoy[slot];
	ShXiState     st;
	Sint16        ly, ry;

	if (js == NULL || s_ownGet((DWORD)slot, &st) != SH_XI_OK)
		return;

	static const struct { WORD mask; int button; } map[] = {
		{ 0x1000, SDL_CONTROLLER_BUTTON_A },             { 0x2000, SDL_CONTROLLER_BUTTON_B },
		{ 0x4000, SDL_CONTROLLER_BUTTON_X },             { 0x8000, SDL_CONTROLLER_BUTTON_Y },
		{ 0x0020, SDL_CONTROLLER_BUTTON_BACK },          { 0x0010, SDL_CONTROLLER_BUTTON_START },
		{ 0x0040, SDL_CONTROLLER_BUTTON_LEFTSTICK },     { 0x0080, SDL_CONTROLLER_BUTTON_RIGHTSTICK },
		{ 0x0100, SDL_CONTROLLER_BUTTON_LEFTSHOULDER },  { 0x0200, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
		{ 0x0001, SDL_CONTROLLER_BUTTON_DPAD_UP },       { 0x0002, SDL_CONTROLLER_BUTTON_DPAD_DOWN },
		{ 0x0004, SDL_CONTROLLER_BUTTON_DPAD_LEFT },     { 0x0008, SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		SDL_JoystickSetVirtualButton(js, map[i].button, (st.pad.buttons & map[i].mask) ? 1 : 0);

	/* XInput's Y axes point up; SDL's point down. -32768 has no positive twin. */
	ly = (st.pad.ly == -32768) ? 32767 : (Sint16)-st.pad.ly;
	ry = (st.pad.ry == -32768) ? 32767 : (Sint16)-st.pad.ry;
	SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_LEFTX, st.pad.lx);
	SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_LEFTY, ly);
	SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_RIGHTX, st.pad.rx);
	SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_RIGHTY, ry);
	SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERLEFT, (Sint16)((int)st.pad.lt * 257 - 32768));
	SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, (Sint16)((int)st.pad.rt * 257 - 32768));
}

static int SDLCALL SteamBypass_Rumble(void* userdata, Uint16 low, Uint16 high)
{
	ShXiRumble r = { low, high };

	if (s_ownSet == NULL)
		return -1;
	return s_ownSet((DWORD)(intptr_t)userdata, &r) == SH_XI_OK ? 0 : -1;
}

static void SteamBypass_Detach(int slot)
{
	if (s_virtIndex[slot] == 0)
		return;
	if (s_virtJoy[slot] != NULL)
		SDL_JoystickClose(s_virtJoy[slot]);
	s_virtJoy[slot] = NULL;
	SDL_JoystickDetachVirtual(s_virtIndex[slot] - 1);
	s_virtIndex[slot] = 0;
	eprintinfo("[PAD] XInput slot %d: stand-in controller removed\n", slot);
}

static void SteamBypass_Attach(int slot)
{
	SDL_VirtualJoystickDesc desc;
	int                     index;

	SDL_zero(desc);
	desc.version    = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
	desc.type       = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
	desc.naxes      = SDL_CONTROLLER_AXIS_MAX;
	desc.nbuttons   = 15;
	desc.vendor_id  = 0x045e;
	desc.product_id = 0x028e;
	desc.name       = "Xbox Controller";
	desc.userdata   = (void*)(intptr_t)slot;
	desc.Update     = SteamBypass_Update;
	desc.Rumble     = SteamBypass_Rumble;
	index = SDL_JoystickAttachVirtualEx(&desc);
	if (index < 0)
	{
		eprintwarn("[PAD] XInput slot %d is hidden by Steam and no stand-in could be made (%s)\n", slot, SDL_GetError());
		return;
	}
	s_virtIndex[slot] = index + 1;
	/* Our own handle, for the state writes; the pad layer opens its own. */
	s_virtJoy[slot] = SDL_JoystickOpen(index);
	eprintinfo("[PAD] XInput slot %d is hidden by Steam Input; reading it directly as a stand-in controller\n", slot);
}

void PsyX_Pad_SteamBypassTick(void)
{
	const Uint32 now = SDL_GetTicks();

	if ((Sint32)(now - s_nextScan) < 0)
		return;
	s_nextScan = now + 1000;

	/* Split screen hands one pad to the second window by SDL device. A stand-in
	 * is a second handle on a physical pad, so Player 1 would keep reading the
	 * pad it gave away; while a split is running there are none. */
	if (g_PsyX_PadFilterMode != 0)
	{
		for (int slot = 0; slot < SH_XI_SLOTS; slot++)
			SteamBypass_Detach(slot);
		return;
	}

	/* Only a process Steam has loaded its overlay into can have hidden pads. */
	if (s_loaded == 0 && GetModuleHandleA("gameoverlayrenderer64.dll") == NULL &&
	    GetModuleHandleA("gameoverlayrenderer.dll") == NULL)
		return;
	if (!SteamBypass_Load())
		return;

	for (int slot = 0; slot < SH_XI_SLOTS; slot++)
	{
		ShXiState st;
		const int present = (s_ownGet((DWORD)slot, &st) == SH_XI_OK);
		const int hidden  = present && (s_sysGet((DWORD)slot, &st) != SH_XI_OK);

		if (hidden && s_virtIndex[slot] == 0)
			SteamBypass_Attach(slot);
		else if (!hidden && s_virtIndex[slot] != 0)
			SteamBypass_Detach(slot);
	}
}

#else

void PsyX_Pad_SteamBypassTick(void)
{
}

#endif
