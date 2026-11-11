/* Controllers that Steam keeps from the game.
 *
 * Once a process has called SteamAPI_Init, the Steam client loads its overlay
 * into it. The online build runs under an app id that Steam treats as a native
 * Steam Input title, so for every controller type with Steam Input enabled the
 * overlay hides the pad from XInput, raw input, HID and therefore SDL, and
 * expects the game to read it through ISteamInput instead. A pad that worked
 * on the title screen goes dead the moment Steam comes up.
 *
 * Two sources bring those pads back, both as SDL virtual game controllers so
 * the rest of the input stack -- binds, glyphs, the controls panel -- sees an
 * ordinary pad:
 *
 *   fed slots     the game reads ISteamInput (pc_port/src/net/
 *                 sh_net_steam_input.c) and feeds each controller's state
 *                 here. Every controller type Steam knows, with rumble.
 *   XInput slots  the fallback while Steam has no layout of ours bound (a
 *                 player who once picked another layout for this app id keeps
 *                 it until they change it in Steam). Steam hides XInput pads
 *                 by patching the system XInput library in the process; a
 *                 private copy loaded under another name is not patched and
 *                 still reads the device. Xbox-type pads only.
 *
 * Without Steam in the process none of this runs and SDL owns every pad. */
#include <SDL.h>

#include "../PsyX_main.h"

extern "C" {
void PsyX_Pad_SteamBypassTick(void);
void PsyX_Pad_SteamBypassHold(int hold);
void PsyX_Pad_StandInFeed(int slot, int present, unsigned buttons, const short* axes, const char* name, unsigned vid, unsigned pid);
void PsyX_Pad_StandInSetRumble(int (*fn)(int slot, unsigned short low, unsigned short high));
int  PsyX_Pad_StandInRumble(SDL_GameController* gc, unsigned short low, unsigned short high);
}

#if defined(_WIN32) && SDL_VERSION_ATLEAST(2, 24, 0)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct ShXiGamepad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct ShXiState   { DWORD packet; ShXiGamepad pad; };
struct ShXiRumble  { WORD low, high; };

typedef DWORD (WINAPI *ShXiGetState)(DWORD, ShXiState*);
typedef DWORD (WINAPI *ShXiSetState)(DWORD, ShXiRumble*);

/* Stand-ins 0..3 are XInput slots read through the private copy; 4..7 are fed
 * by the game. */
enum { SH_XI_SLOTS = 4, SH_FED_SLOTS = 4, SH_STANDINS = SH_XI_SLOTS + SH_FED_SLOTS, SH_XI_OK = 0 };

struct StandIn
{
	int            index; /* SDL device index + 1; 0 = none */
	SDL_Joystick*  joy;
	SDL_JoystickID instance;
};

static StandIn      s_standIn[SH_STANDINS];
static ShXiGetState s_sysGet, s_ownGet;
static ShXiSetState s_ownSet;
static int          s_loaded; /* 0 not tried, 1 ready, -1 unavailable */
static int          s_hold;
static Uint32       s_nextScan;
static int        (*s_fedRumble)(int slot, unsigned short low, unsigned short high);

extern int g_PsyX_PadFilterMode;

static void StandIn_Remove(int s, const char* why)
{
	if (s_standIn[s].index == 0)
		return;
	if (s_standIn[s].joy != NULL)
		SDL_JoystickClose(s_standIn[s].joy);
	SDL_JoystickDetachVirtual(s_standIn[s].index - 1);
	s_standIn[s].index    = 0;
	s_standIn[s].joy      = NULL;
	s_standIn[s].instance = -1;
	eprintinfo("[PAD] stand-in controller %d removed (%s)\n", s, why);
}

static int StandIn_Add(int s, const char* name, unsigned vid, unsigned pid)
{
	SDL_VirtualJoystickDesc desc;
	int                     index;

	SDL_zero(desc);
	desc.version    = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
	desc.type       = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
	desc.naxes      = SDL_CONTROLLER_AXIS_MAX;
	desc.nbuttons   = 15;
	desc.vendor_id  = (Uint16)vid;
	desc.product_id = (Uint16)pid;
	desc.name       = name;
	index = SDL_JoystickAttachVirtualEx(&desc);
	if (index < 0)
	{
		eprintwarn("[PAD] no stand-in controller could be made for '%s' (%s)\n", name, SDL_GetError());
		return 0;
	}
	s_standIn[s].index = index + 1;
	/* Our own handle, for the state writes; the pad layer opens its own. */
	s_standIn[s].joy      = SDL_JoystickOpen(index);
	s_standIn[s].instance = s_standIn[s].joy ? SDL_JoystickInstanceID(s_standIn[s].joy) : -1;
	return 1;
}

static void StandIn_Write(int s, unsigned buttons, const short* axes)
{
	SDL_Joystick* js = s_standIn[s].joy;

	if (js == NULL)
		return;
	for (int b = 0; b < 15; b++)
		SDL_JoystickSetVirtualButton(js, b, (buttons >> b) & 1);
	for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
		SDL_JoystickSetVirtualAxis(js, a, axes[a]);
}

/* ------------------------------------------------------------------ */
/* Fed by the game                                                     */
/* ------------------------------------------------------------------ */

void PsyX_Pad_StandInFeed(int slot, int present, unsigned buttons, const short* axes, const char* name, unsigned vid, unsigned pid)
{
	const int s = SH_XI_SLOTS + slot;

	if (slot < 0 || slot >= SH_FED_SLOTS)
		return;
	/* Split screen hands a pad to the second window by SDL device; a stand-in
	 * is a second handle on a physical pad, so there are none while a split
	 * is running and Player 1 cannot keep reading the pad it gave away. */
	if (!present || g_PsyX_PadFilterMode != 0)
	{
		StandIn_Remove(s, present ? "split screen" : "gone from Steam Input");
		return;
	}
	if (s_standIn[s].index == 0)
	{
		if (!StandIn_Add(s, name, vid, pid))
			return;
		eprintinfo("[PAD] Steam Input controller %d '%s' is read through Steam as a stand-in controller\n", slot, name);
	}
	StandIn_Write(s, buttons, axes);
}

void PsyX_Pad_StandInSetRumble(int (*fn)(int slot, unsigned short low, unsigned short high))
{
	s_fedRumble = fn;
}

/* SDL's own rumble call fails on a virtual controller through sdl2-compat, so
 * the pad layer asks here first. 1 = this controller is a stand-in and the
 * request went to its real device. */
int PsyX_Pad_StandInRumble(SDL_GameController* gc, unsigned short low, unsigned short high)
{
	SDL_Joystick*  js = gc ? SDL_GameControllerGetJoystick(gc) : NULL;
	SDL_JoystickID id = js ? SDL_JoystickInstanceID(js) : -1;

	if (id < 0)
		return 0;
	for (int s = 0; s < SH_STANDINS; s++)
	{
		if (s_standIn[s].index == 0 || s_standIn[s].instance != id)
			continue;
		if (s < SH_XI_SLOTS)
		{
			ShXiRumble r = { low, high };
			if (s_ownSet != NULL)
				s_ownSet((DWORD)s, &r);
		}
		else if (s_fedRumble != NULL)
			s_fedRumble(s - SH_XI_SLOTS, low, high);
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* XInput fallback                                                     */
/* ------------------------------------------------------------------ */

void PsyX_Pad_SteamBypassHold(int hold)
{
	s_hold = hold;
}

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

static void SteamBypass_Read(int slot)
{
	static const struct { WORD mask; int button; } map[] = {
		{ 0x1000, SDL_CONTROLLER_BUTTON_A },             { 0x2000, SDL_CONTROLLER_BUTTON_B },
		{ 0x4000, SDL_CONTROLLER_BUTTON_X },             { 0x8000, SDL_CONTROLLER_BUTTON_Y },
		{ 0x0020, SDL_CONTROLLER_BUTTON_BACK },          { 0x0010, SDL_CONTROLLER_BUTTON_START },
		{ 0x0040, SDL_CONTROLLER_BUTTON_LEFTSTICK },     { 0x0080, SDL_CONTROLLER_BUTTON_RIGHTSTICK },
		{ 0x0100, SDL_CONTROLLER_BUTTON_LEFTSHOULDER },  { 0x0200, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
		{ 0x0001, SDL_CONTROLLER_BUTTON_DPAD_UP },       { 0x0002, SDL_CONTROLLER_BUTTON_DPAD_DOWN },
		{ 0x0004, SDL_CONTROLLER_BUTTON_DPAD_LEFT },     { 0x0008, SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
	};
	ShXiState st;
	unsigned  buttons = 0;
	short     axes[SDL_CONTROLLER_AXIS_MAX];

	if (s_ownGet((DWORD)slot, &st) != SH_XI_OK)
		return;
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (st.pad.buttons & map[i].mask)
			buttons |= 1u << map[i].button;
	/* XInput's Y axes point up; SDL's point down. -32768 has no positive twin. */
	axes[SDL_CONTROLLER_AXIS_LEFTX]        = st.pad.lx;
	axes[SDL_CONTROLLER_AXIS_LEFTY]        = (st.pad.ly == -32768) ? 32767 : (short)-st.pad.ly;
	axes[SDL_CONTROLLER_AXIS_RIGHTX]       = st.pad.rx;
	axes[SDL_CONTROLLER_AXIS_RIGHTY]       = (st.pad.ry == -32768) ? 32767 : (short)-st.pad.ry;
	axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT]  = (short)((int)st.pad.lt * 257 - 32768);
	axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = (short)((int)st.pad.rt * 257 - 32768);
	StandIn_Write(slot, buttons, axes);
}

void PsyX_Pad_SteamBypassTick(void)
{
	const Uint32 now  = SDL_GetTicks();
	const int    scan = (Sint32)(now - s_nextScan) >= 0;

	if (scan)
	{
		s_nextScan = now + 1000;

		/* Off while Steam Input feeds the pads itself, and during a split
		 * (see PsyX_Pad_StandInFeed). */
		if (s_hold || g_PsyX_PadFilterMode != 0)
		{
			for (int slot = 0; slot < SH_XI_SLOTS; slot++)
				StandIn_Remove(slot, s_hold ? "Steam Input has it" : "split screen");
		}
		/* Only a process Steam has loaded its overlay into can have hidden pads. */
		else if ((s_loaded != 0 || GetModuleHandleA("gameoverlayrenderer64.dll") != NULL ||
		          GetModuleHandleA("gameoverlayrenderer.dll") != NULL) && SteamBypass_Load())
		{
			for (int slot = 0; slot < SH_XI_SLOTS; slot++)
			{
				ShXiState st;
				const int present = (s_ownGet((DWORD)slot, &st) == SH_XI_OK);
				const int hidden  = present && (s_sysGet((DWORD)slot, &st) != SH_XI_OK);

				if (hidden && s_standIn[slot].index == 0)
				{
					if (StandIn_Add(slot, "Xbox Controller", 0x045e, 0x028e))
						eprintinfo("[PAD] XInput slot %d is hidden by Steam with no layout of ours bound; read directly as a stand-in controller\n", slot);
				}
				else if (!hidden)
					StandIn_Remove(slot, "no longer hidden");
			}
		}
	}

	for (int slot = 0; slot < SH_XI_SLOTS; slot++)
		if (s_standIn[slot].index != 0)
			SteamBypass_Read(slot);
}

#else

void PsyX_Pad_SteamBypassTick(void) {}
void PsyX_Pad_SteamBypassHold(int hold) { (void)hold; }
void PsyX_Pad_StandInFeed(int slot, int present, unsigned buttons, const short* axes, const char* name, unsigned vid, unsigned pid)
{
	(void)slot; (void)present; (void)buttons; (void)axes; (void)name; (void)vid; (void)pid;
}
void PsyX_Pad_StandInSetRumble(int (*fn)(int slot, unsigned short low, unsigned short high)) { (void)fn; }
int  PsyX_Pad_StandInRumble(SDL_GameController* gc, unsigned short low, unsigned short high)
{
	(void)gc; (void)low; (void)high;
	return 0;
}

#endif
