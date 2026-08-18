#include "psx/libpad.h"
#include "psx/libetc.h"

#include "../PsyX_main.h"
#include "PsyX_pad.h"
#include "PsyX/PsyX_public.h"

#include <string.h>

extern "C"
{
extern int g_padCommEnable;
}

typedef struct
{
	Sint32				deviceId;	// linked device Id
	SDL_GameController* gc;

	u_char*				padData;
	bool				switchingAnalog;
	u_short				hystWord[2]; /* PC: per-mapping Schmitt-trigger latch (analog->digital anti-chatter) */
} PsyXController;

int						g_cfg_controllerToSlotMapping[MAX_CONTROLLERS] = { -1, -1 };

/* PC port: movement source for the controller. 0 = analog stick only,
 * 1 = d-pad only (digital), 2 = both (default). Set from config in main_pc.c.
 * Drives whether the emulated pad sits in analog (0x73) or digital (0x41) mode. */
int						g_cfg_controllerMovement = 2;
int						g_cfg_disableDpadMovement = 0; /* 1 = controller D-pad no longer drives movement (freed for action binds); keyboard arrows unaffected */

PsyXController			g_controllers[MAX_CONTROLLERS];

/* PSX PadSetAct semantics: the game registers a LIVE actuator buffer once
 * and the pad driver transmits its current bytes to the controller every
 * vsync; the game then just mutates the bytes in place (Silent Hill's
 * vibration engine repacks them per frame in func_8009E718). A fire-once
 * PadSetAct loses every later value change, so register here and
 * retransmit from PsyX_Pad_InternalPadUpdates. */
static unsigned char*	g_actBufTable[MAX_CONTROLLERS];
static int				g_actBufLen[MAX_CONTROLLERS];
const u_char*			g_sdlKeyboardState = NULL;

u_short PsyX_Pad_UpdateKeyboardInput();
void	PsyX_Pad_UpdateGameControllerInput(PsyXController* controller, LPPADRAW pad);

/* Touch controls live in the port (pc_touch.c), which knows about game state;
 * declared rather than included so PsyCross keeps no pc_port include path. */
extern "C" void Pc_Touch_Update(void);
extern "C" void Pc_Touch_NoteOtherInput(int padAttached, int keyWord);
extern "C" int  Pc_Touch_Active(void);
extern "C" void Pc_Touch_GetPad(unsigned short* word,
                                unsigned char* rightX, unsigned char* rightY,
                                unsigned char* leftX,  unsigned char* leftY);

// Initializes SDL controllers
int PsyX_Pad_InitSystem()
{
	// do not init second time!
	if (g_sdlKeyboardState != NULL)
		return 1;

	// DualSense needs the hidapi backend. Both hints already default to "1" on
	// every SDL2 that ships a PS5 driver (>= 2.0.14), so this is an explicit pin
	// rather than a fix — but it MUST stay above the SDL_InitSubSystem below,
	// because SDL latches joystick hints when the subsystem starts. An
	// SDL_JOYSTICK_HIDAPI=0 environment variable still wins (NORMAL priority),
	// which is what leaves Steam Input's overrides working.
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");

	memset(g_controllers, 0, sizeof(g_controllers));

	// init keyboard state
	g_sdlKeyboardState = SDL_GetKeyboardState(NULL);

	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0)
	{
		eprinterr("Failed to initialise SDL GameController subsystem!\n");
		return 0;
	}

	// Add more controllers from custom file
	SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");

	/* A pad that never raises SDL_CONTROLLERDEVICEADDED leaves controller->gc
	 * NULL, so UpdateGameControllerInput writes "all released" every poll and
	 * the pad is silently driven by the key path instead — which is where the
	 * Android shared-scancode collision bites. Record what SDL actually sees so
	 * that state is visible in the log rather than inferred from its absence. */
	{
		int n = SDL_NumJoysticks();
		int i;

		eprintinfo("[PAD] SDL_NumJoysticks=%d\n", n);

		for (i = 0; i < n; i++)
		{
			const char* nm = SDL_JoystickNameForIndex(i);
			eprintinfo("[PAD]   %d '%s' isGameController=%d\n",
				i, nm ? nm : "?", SDL_IsGameController(i) ? 1 : 0);
		}
	}

	return 1;
}

// Prints controller list into console
void PsyX_Pad_Debug_ListControllers()
{
	int numJoysticks = SDL_NumJoysticks();
	int numHaptics = SDL_NumHaptics();

	if (numJoysticks)
	{
		eprintf("SDL GameController list:\n");

		for (int i = 0; i < numJoysticks; i++)
		{
			if (SDL_IsGameController(i))
			{
				eprintinfo("  %d '%s'\n", i, SDL_GameControllerNameForIndex(i));
			}
		}
	}
	else
		eprintwarn("No SDL GameControllers found!\n");

	if (numHaptics)
	{
		eprintf("SDL haptic list:\n");

		for (int i = 0; i < numHaptics; i++)
		{
			eprintinfo("  %d '%s'\n", i, SDL_HapticName(i));
		}
	}
	else
		eprintwarn("No SDL haptics found!\n");
}


/* One-shot detailed dump of a controller, for building a proper SDL mapping (or
 * a softmod patch) for hardware whose stock profile is incomplete. Prints the
 * GUID, the raw button/axis/hat counts and the mapping string SDL is using, so
 * the gap between "buttons the device has" and "buttons the profile names" is
 * visible in one place. */
static void PsyX_Pad_DumpDeviceDetail(SDL_GameController* gc)
{
	SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);
	char          guid[64];
	char*         map;
	int           i;

	if (js == NULL)
		return;

	SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guid, sizeof(guid));

	eprintinfo("[PADINFO] name='%s' guid=%s%s",
		SDL_JoystickName(js) ? SDL_JoystickName(js) : "?", guid, "\n");
	eprintinfo("[PADINFO] raw buttons=%d axes=%d hats=%d balls=%d%s",
		SDL_JoystickNumButtons(js), SDL_JoystickNumAxes(js),
		SDL_JoystickNumHats(js), SDL_JoystickNumBalls(js), "\n");
	eprintinfo("[PADINFO] vendor=0x%04X product=0x%04X version=0x%04X%s",
		SDL_JoystickGetVendor(js), SDL_JoystickGetProduct(js),
		SDL_JoystickGetProductVersion(js), "\n");

	map = SDL_GameControllerMapping(gc);
	if (map != NULL)
	{
		eprintinfo("[PADINFO] sdl mapping: %s%s", map, "\n");
		SDL_free(map);
	}

	/* Which controller name each raw button currently answers to, so an unnamed
	 * one shows up as a hole in the list rather than being invisible. */
	for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
	{
		SDL_GameControllerButtonBind b =
			SDL_GameControllerGetBindForButton(gc, (SDL_GameControllerButton)i);

		if (b.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON)
			eprintinfo("[PADINFO] '%s' <- raw button %d%s",
				SDL_GameControllerGetStringForButton((SDL_GameControllerButton)i),
				b.value.button, "\n");
		else if (b.bindType == SDL_CONTROLLER_BINDTYPE_HAT)
			eprintinfo("[PADINFO] '%s' <- hat %d mask %d%s",
				SDL_GameControllerGetStringForButton((SDL_GameControllerButton)i),
				b.value.hat.hat, b.value.hat.hat_mask, "\n");
	}
}

// Opens specific system controller and assigns to specified slot
void PsyX_Pad_OpenController(Sint32 deviceId, int slot)
{
	PsyXController* controller = &g_controllers[slot];

	if (controller->gc)
	{
		return;
	}

	controller->gc = SDL_GameControllerOpen(deviceId);
	controller->switchingAnalog = false;

	if (controller->gc)
	{
		// assign device id automatically
		if (controller->deviceId == -1)
			controller->deviceId = deviceId;

		PsyX_Pad_DumpDeviceDetail(controller->gc);
	}
}

// Closes controller in specific slot
void PsyX_Pad_CloseController(int slot)
{
	PsyXController* controller = &g_controllers[slot];
	SDL_GameControllerClose(controller->gc);

	controller->gc = NULL;
}

// Called from LIBPAD
void PsyX_Pad_InitPad(int slot, u_char* padData)
{
	PsyXController* controller = &g_controllers[slot];

	controller->padData = padData;
	controller->deviceId = g_cfg_controllerToSlotMapping[slot];

	if (padData)
	{
		LPPADRAW pad = (LPPADRAW)padData;
		
		bool wasConnected = (pad->id == 0x41 || pad->id == 0x73);

		if(!wasConnected)
			pad->id = slot == 0 ? 0x41 : 0xFF;	// since keyboard is a main controller - it's always on

		// only reset buttons
		pad->buttons[0] = 0xFF;
		pad->buttons[1] = 0xFF;
		pad->analog[0] = 128;
		pad->analog[1] = 128;
		pad->analog[2] = 128;
		pad->analog[3] = 128;
	}
}

// called from Psy-X SDL events
void PsyX_Pad_Event_ControllerAdded(Sint32 deviceId)
{
	int i;
	PsyXController* controller;

	// reinitialize haptics (why we still here?)
	SDL_QuitSubSystem(SDL_INIT_HAPTIC);			// FIXME: this will crash if you already have haptics
	SDL_InitSubSystem(SDL_INIT_HAPTIC);

	PsyX_Pad_Debug_ListControllers();

	// find mapping and open
	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		controller = &g_controllers[i];

		if (controller->deviceId == -1 || controller->deviceId == deviceId)
		{
			PsyX_Pad_OpenController(deviceId, i);
			break;
		}
	}
}

// called from Psy-X SDL events
void PsyX_Pad_Event_ControllerRemoved(Sint32 deviceId)
{
	int i;
	PsyXController* controller;

	PsyX_Pad_Debug_ListControllers();

	// find mapping and close
	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		controller = &g_controllers[i];

		if (controller->deviceId == deviceId)
		{
			PsyX_Pad_CloseController(i);
		}
	}
}

void PsyX_Pad_InternalPadUpdates()
{
	PsyXController* controller;
	LPPADRAW pad;
	u_short kbInputs;

	if (g_padCommEnable == 0)
		return;

	kbInputs = PsyX_Pad_UpdateKeyboardInput();

	/* Let the touch layer stand aside for real hardware. Both facts are needed:
	 * an opened GameController covers the normal case, and the key word covers
	 * Android pads that SDL never enumerates as controllers at all. */
	{
		int attached = 0;
		int i;

		for (i = 0; i < MAX_CONTROLLERS; i++)
		{
			if (g_controllers[i].gc && SDL_GameControllerGetAttached(g_controllers[i].gc))
			{
				attached = 1;
				break;
			}
		}

		Pc_Touch_NoteOtherInput(attached, (int)kbInputs);
	}

	/* Rebuild the virtual touch pad here rather than from the frame loop: this
	 * is the one place guaranteed to run exactly once per pad read, so the
	 * gesture state a read sees can never be a frame stale or a frame early. */
	Pc_Touch_Update();

	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		controller = &g_controllers[i];

		if (controller->padData)
		{
			pad = (LPPADRAW)controller->padData;

			PsyX_Pad_UpdateGameControllerInput(controller, pad);

			// Retransmit the registered actuator buffer (PSX pad driver
			// behavior) so in-place value changes by the game reach SDL.
			if (g_actBufTable[i] && g_actBufLen[i] > 0 && controller->gc)
				PsyX_Pad_Vibrate(0, i, g_actBufTable[i], g_actBufLen[i]);

			// PC port: analog mode is config-driven (controller_movement) rather
			// than the original Select+Start manual toggle. analog/both -> 0x73
			// (left stick active), dpad -> 0x41 (digital, stick ignored). Only
			// when a real controller is attached; keyboard stays digital below.
			if (controller->gc && SDL_GameControllerGetAttached(controller->gc))
			{
				pad->id = (g_cfg_controllerMovement == 1) ? 0x41 : 0x73;
			}

			/* Touch presents itself as an ordinary analog pad on port 1, so
			 * nothing downstream of libpad needs to know a finger is involved.
			 * It runs AFTER the controller read (which zeroes everything when
			 * no gamepad is open) and BEFORE the keyboard merge, so a physical
			 * key can still add presses on top. id 0x73 is what makes the game
			 * read the sticks at all -- 0x41 is digital-only. */
			if (i == 0 && Pc_Touch_Active())
			{
				unsigned short tw = 0xFFFF;
				unsigned char  rx = 128, ry = 128, lx = 128, ly = 128;

				Pc_Touch_GetPad(&tw, &rx, &ry, &lx, &ly);

				*(u_short*)pad->buttons &= tw;
				pad->analog[0] = rx;
				pad->analog[1] = ry;
				pad->analog[2] = lx;
				pad->analog[3] = ly;
				pad->id        = 0x73;
				pad->status    = 0;
			}

			// Update keyboard for PAD
			if ((g_activeKeyboardControllers & (1 << i)) && kbInputs != 0xffff)
			{
				pad->status = 0;	// PadStateStable?

				if (pad->id != 0x41)
				{
					if(pad->id != 0x73)
						eprintf("Port %d ANALOG: OFF\n", i + 1);

					pad->id = 0x41; // force disable analog
				}

				*(u_short*)pad->buttons &= kbInputs;
			}
		}
	}

	/* Fold every other controller into player 1.
	 *
	 * Controllers are opened first-come into slots, so whichever device SDL
	 * enumerates first becomes player 1 -- and that is not necessarily the one
	 * a human is holding. On an arcade cabinet a 'Logitech USB Receiver' dongle
	 * claimed slot 0 while the actual panel landed in slot 1, so the panel was
	 * driving a second player this game does not have and almost nothing
	 * responded.
	 *
	 * Silent Hill is single-player, so merging is strictly a gain: any pad on
	 * the machine drives Harry, including a cabinet's second-player controls.
	 * Purely additive -- each slot still gets its own state above, so a port
	 * that wants pad 2 can still read it. Active-low, so AND means "pressed on
	 * either". */
	{
		LPPADRAW p0 = (LPPADRAW)g_controllers[0].padData;
		int      i;

		if (p0 != NULL)
		{
			for (i = 1; i < MAX_CONTROLLERS; i++)
			{
				LPPADRAW pn = (LPPADRAW)g_controllers[i].padData;

				if (pn == NULL || g_controllers[i].gc == NULL ||
				    !SDL_GameControllerGetAttached(g_controllers[i].gc))
					continue;

				*(u_short*)p0->buttons &= *(u_short*)pn->buttons;

				/* Sticks can't be OR'd, so the first deflected one wins;
				 * a centred pad never fights one being pushed. */
				{
					const int n0 = (abs((int)p0->analog[0] - 128) + abs((int)p0->analog[1] - 128) +
					                abs((int)p0->analog[2] - 128) + abs((int)p0->analog[3] - 128));
					const int nn = (abs((int)pn->analog[0] - 128) + abs((int)pn->analog[1] - 128) +
					                abs((int)pn->analog[2] - 128) + abs((int)pn->analog[3] - 128));

					if (nn > n0)
					{
						p0->analog[0] = pn->analog[0];
						p0->analog[1] = pn->analog[1];
						p0->analog[2] = pn->analog[2];
						p0->analog[3] = pn->analog[3];
						p0->id        = pn->id;
					}
				}

				p0->status = 0;
			}
		}
	}
}


int GetControllerButtonState(SDL_GameController* cont, int buttonOrAxis); /* defined below */

extern "C" int PsyX_Pad_SkipButtonHeld(void)
{
	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (!gc)
			continue;

		/* Route the FMV/blocking-loop "skip" through the configured Action/Start
		 * binds (primary + alternate) instead of hardcoded A/Start, so a rebound
		 * controller still skips. */
		if (GetControllerButtonState(gc, g_cfg_controllerMapping.gc_cross)  > 16384 ||
		    GetControllerButtonState(gc, g_cfg_controllerMapping2.gc_cross) > 16384 ||
		    GetControllerButtonState(gc, g_cfg_controllerMapping.gc_start)  > 16384)
			return 1;
	}

	return 0;
}

int GetControllerButtonState(SDL_GameController* cont, int buttonOrAxis)
{
	/* Raw joystick button: bypasses the controller profile entirely, which is
	 * the only way to reach a button the profile does not name. */
	if (buttonOrAxis & CONTROLLER_MAP_FLAG_RAWBTN)
	{
		SDL_Joystick* js = SDL_GameControllerGetJoystick(cont);
		const int     raw = buttonOrAxis & 0x0FFF;

		if (js == NULL || raw >= SDL_JoystickNumButtons(js))
			return 0;

		return SDL_JoystickGetButton(js, raw) * 32767;
	}

	if (buttonOrAxis & CONTROLLER_MAP_FLAG_AXIS)
	{
		int value = SDL_GameControllerGetAxis(cont, (SDL_GameControllerAxis)(buttonOrAxis & ~(CONTROLLER_MAP_FLAG_AXIS | CONTROLLER_MAP_FLAG_INVERSE)));

		if (abs(value) > 500 && (buttonOrAxis & CONTROLLER_MAP_FLAG_INVERSE))
			value *= -1;

		return value;
	}

	return SDL_GameControllerGetButton(cont, (SDL_GameControllerButton)buttonOrAxis) * 32767;
}

/* PC port: is an SDL game-controller button held on ANY attached physical
 * controller? Read straight from SDL, NOT the keyboard-merged PSX pad word, so a
 * keyboard key mapped to the same PSX button cannot trigger a controller-only
 * action (e.g. the Change-Camera pad bind). sdlGameControllerButton < 0 = unbound. */
extern "C" int PsyX_RawControllerButtonHeld(int sdlGameControllerButton)
{
	int i;
	if (sdlGameControllerButton < 0)
		return 0;
	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (gc && SDL_GameControllerGetAttached(gc) &&
		    SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)sdlGameControllerButton))
			return 1;
	}
	return 0;
}

/* PC port: as above, but accepts a bind encoded by PsyX_LookupGameControllerMapping
 * — i.e. a digital button OR an axis (CONTROLLER_MAP_FLAG_AXIS), which is how
 * "lefttrigger"/"righttrigger" are represented. The PSX-button binds always went
 * through that encoding (pad_cross defaults to righttrigger), but the port's own
 * action binds resolved with SDL_GameControllerGetButtonFromString, which knows
 * only digital buttons and returns INVALID for a trigger — so binding an action to
 * L2/R2 silently did nothing. Digitised with the same >16384 half-scale threshold
 * the pad word uses elsewhere. */
extern "C" int PsyX_RawControllerBindHeld(int buttonOrAxis)
{
	int i;
	if (buttonOrAxis < 0)
		return 0;
	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		SDL_GameController* gc = g_controllers[i].gc;
		if (gc && SDL_GameControllerGetAttached(gc) &&
		    abs(GetControllerButtonState(gc, buttonOrAxis)) > 16384)
			return 1;
	}
	return 0;
}

/* PC port: Schmitt-trigger digitization. An analog input (trigger/stick) mapped to a
   button presses only above HIGH and releases only below LOW, so a value wavering near a
   single 50% threshold can't chatter the digital bit -- that chatter double-fired the gun
   on analog triggers. Digital buttons report 0/32767 and clear both thresholds (unaffected).
   prevWord is the previous frame's post-hysteresis word for this mapping. */
static inline bool PadBtnPressed(SDL_GameController* cont, int getter, u_short prevWord, u_short bit)
{
	int v = GetControllerButtonState(cont, getter);
	bool was = (prevWord & bit) == 0; /* active-low: clear bit = was pressed */
	return was ? (v > 8000) : (v > 24000);
}

/* Build the active-low 16-bit PSX button word from one controller mapping (with hysteresis). */
static u_short PsyX_Pad_BuildPadWord(SDL_GameController* cont, const PsyXControllerMapping& mapping, u_short prevWord)
{
	u_short ret = 0xFFFF;
	if (PadBtnPressed(cont, mapping.gc_square,     prevWord, 0x8000)) ret &= ~0x8000; //Square
	if (PadBtnPressed(cont, mapping.gc_circle,     prevWord, 0x2000)) ret &= ~0x2000; //Circle
	if (PadBtnPressed(cont, mapping.gc_triangle,   prevWord, 0x1000)) ret &= ~0x1000; //Triangle
	if (PadBtnPressed(cont, mapping.gc_cross,      prevWord, 0x4000)) ret &= ~0x4000; //Cross
	if (PadBtnPressed(cont, mapping.gc_l1,         prevWord, 0x400))  ret &= ~0x400;  //L1
	if (PadBtnPressed(cont, mapping.gc_r1,         prevWord, 0x800))  ret &= ~0x800;  //R1
	if (PadBtnPressed(cont, mapping.gc_l2,         prevWord, 0x100))  ret &= ~0x100;  //L2
	if (PadBtnPressed(cont, mapping.gc_r2,         prevWord, 0x200))  ret &= ~0x200;  //R2
	if (PadBtnPressed(cont, mapping.gc_dpad_up,    prevWord, 0x10))   ret &= ~0x10;   //UP
	if (PadBtnPressed(cont, mapping.gc_dpad_down,  prevWord, 0x40))   ret &= ~0x40;   //DOWN
	if (PadBtnPressed(cont, mapping.gc_dpad_left,  prevWord, 0x80))   ret &= ~0x80;   //LEFT
	if (PadBtnPressed(cont, mapping.gc_dpad_right, prevWord, 0x20))   ret &= ~0x20;   //RIGHT
	if (PadBtnPressed(cont, mapping.gc_l3,         prevWord, 0x2))    ret &= ~0x2;    //L3
	if (PadBtnPressed(cont, mapping.gc_r3,         prevWord, 0x4))    ret &= ~0x4;    //R3
	if (PadBtnPressed(cont, mapping.gc_select,     prevWord, 0x1))    ret &= ~0x1;    //SELECT
	if (PadBtnPressed(cont, mapping.gc_start,      prevWord, 0x8))    ret &= ~0x8;    //START
	return ret;
}

void PsyX_Pad_UpdateGameControllerInput(PsyXController* controller, LPPADRAW pad)
{
	SDL_GameController* cont = controller->gc;
	short leftX, leftY, rightX, rightY;
	u_short ret;

	if (!cont)
	{
		pad->analog[0] = 127;
		pad->analog[1] = 127;
		pad->analog[2] = 127;
		pad->analog[3] = 127;

		*(u_short*)pad->buttons = 0xFFFF;
		return;
	}

	/* Primary binds AND the secondary (second-button-per-action) binds: active-low,
	 * so an action reads pressed if EITHER mapping clears its bit. Analog sticks come
	 * from the primary mapping's axes only. */
	/* Report every controller button this device can produce, once each.
	 *
	 * The keyboard probe found nothing on an arcade cabinet because its panel is
	 * a game controller, not a keyboard -- and which SDL buttons a panel exposes
	 * is not guessable: a 6-button stick has no triggers and no Back, which is
	 * exactly why Aim (R2) and Inventory (Select) did nothing there while the
	 * face buttons worked. Press each button once and this prints the map. */
	{
		static unsigned char s_btnSeen[SDL_CONTROLLER_BUTTON_MAX];
		static unsigned char s_axSeen[SDL_CONTROLLER_AXIS_MAX];
		int b;

		for (b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
		{
			if (!s_btnSeen[b] && SDL_GameControllerGetButton(cont, (SDL_GameControllerButton)b))
			{
				s_btnSeen[b] = 1;
				eprintinfo("[BTN] button '%s' on '%s'\n",
					SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b),
					SDL_GameControllerName(cont) ? SDL_GameControllerName(cont) : "?");
			}
		}

		/* RAW joystick buttons as well. SDL_GameController only exposes buttons
		 * its mapping covers, so a panel with more buttons than the profile
		 * describes has some that are simply invisible above -- which is exactly
		 * what two dead buttons on a 6-button cabinet look like. The raw device
		 * sees every one, and the index printed here is what a binding needs. */
		{
			SDL_Joystick* js = SDL_GameControllerGetJoystick(cont);

			if (js != NULL)
			{
				static unsigned char s_rawSeen[32];
				int nb = SDL_JoystickNumButtons(js);
				int r;

				if (nb > 32) nb = 32;
				for (r = 0; r < nb; r++)
				{
					if (!s_rawSeen[r] && SDL_JoystickGetButton(js, r))
					{
						s_rawSeen[r] = 1;
						eprintinfo("[BTN] RAW joystick button %d on '%s'\n",
							r, SDL_JoystickName(js) ? SDL_JoystickName(js) : "?");
					}
				}
			}
		}

		/* Triggers and sticks are axes, not buttons; a panel that reports its
		 * shoulder buttons as axes would otherwise look like it had none. */
		for (b = 0; b < SDL_CONTROLLER_AXIS_MAX; b++)
		{
			if (!s_axSeen[b] && abs(SDL_GameControllerGetAxis(cont, (SDL_GameControllerAxis)b)) > 16384)
			{
				s_axSeen[b] = 1;
				eprintinfo("[BTN] axis '%s' on '%s'\n",
					SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)b),
					SDL_GameControllerName(cont) ? SDL_GameControllerName(cont) : "?");
			}
		}
	}

	u_short w1 = PsyX_Pad_BuildPadWord(cont, g_cfg_controllerMapping,  controller->hystWord[0]);
	u_short w2 = PsyX_Pad_BuildPadWord(cont, g_cfg_controllerMapping2, controller->hystWord[1]);
	controller->hystWord[0] = w1;
	controller->hystWord[1] = w2;
	ret = w1 & w2;

	/* "Disable D-pad for movement": un-press the controller D-pad bits (active-low,
	 * so OR them back to 1) so the D-pad no longer drives walk/turn. Keyboard arrows
	 * use a separate word (unaffected), and actions bound to the D-pad read the raw
	 * controller via PsyX_RawControllerButtonHeld, so binding still works. Bits:
	 * UP 0x10, DOWN 0x40, LEFT 0x80, RIGHT 0x20. */
	if (g_cfg_disableDpadMovement)
		ret |= 0x10 | 0x40 | 0x80 | 0x20;

	leftX = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_left_x);
	leftY = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_left_y);

	rightX = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_right_x);
	rightY = GetControllerButtonState(cont, g_cfg_controllerMapping.gc_axis_right_y);

	*(u_short*)pad->buttons = ret;

	// map to range
	pad->analog[0] = (rightX / 256) + 128;
	pad->analog[1] = (rightY / 256) + 128;
	pad->analog[2] = (leftX / 256) + 128;
	pad->analog[3] = (leftY / 256) + 128;
}

static u_short PsyX_Pad_BuildKbWord(const PsyXKeyboardMapping& mapping)
{
	u_short ret = 0xFFFF;

	if (g_sdlKeyboardState[mapping.kc_square])     ret &= ~0x8000;//Square
	if (g_sdlKeyboardState[mapping.kc_circle])     ret &= ~0x2000;//Circle
	if (g_sdlKeyboardState[mapping.kc_triangle])   ret &= ~0x1000;//Triangle
	if (g_sdlKeyboardState[mapping.kc_cross])      ret &= ~0x4000;//Cross
	if (g_sdlKeyboardState[mapping.kc_l1])         ret &= ~0x400; //L1
	if (g_sdlKeyboardState[mapping.kc_l2])         ret &= ~0x100; //L2
	if (g_sdlKeyboardState[mapping.kc_l3])         ret &= ~0x2;   //L3
	if (g_sdlKeyboardState[mapping.kc_r1])         ret &= ~0x800; //R1
	if (g_sdlKeyboardState[mapping.kc_r2])         ret &= ~0x200; //R2
	if (g_sdlKeyboardState[mapping.kc_r3])         ret &= ~0x4;   //R3
	if (g_sdlKeyboardState[mapping.kc_dpad_up])    ret &= ~0x10;  //UP
	if (g_sdlKeyboardState[mapping.kc_dpad_down])  ret &= ~0x40;  //DOWN
	if (g_sdlKeyboardState[mapping.kc_dpad_left])  ret &= ~0x80;  //LEFT
	if (g_sdlKeyboardState[mapping.kc_dpad_right]) ret &= ~0x20;  //RIGHT
	if (g_sdlKeyboardState[mapping.kc_select])     ret &= ~0x1;   //SELECT
	if (g_sdlKeyboardState[mapping.kc_start])      ret &= ~0x8;   //START

	return ret;
}

/* Mouse buttons -> PSX button word (active-low). Each pressed SDL mouse button
 * 1..5 clears whatever PSX bits the config bound it to (g_cfg_mouseButtonMask). */
static u_short PsyX_Pad_BuildMouseWord()
{
	extern int g_PsyX_WheelUpFrames, g_PsyX_WheelDownFrames;
	u_short ret = 0xFFFF;
	Uint32  mb  = SDL_GetMouseState(NULL, NULL);
	int     b;

	for (b = 1; b <= 5; b++)
	{
		if ((mb & SDL_BUTTON(b)) && g_cfg_mouseButtonMask[b])
			ret &= ~g_cfg_mouseButtonMask[b];
	}

	/* Mouse wheel up/down occupy mask slots 6/7 (see Pc_ParseMouseName). The
	 * latch is set on the scroll event and decayed once per frame in
	 * PsyX_EndScene — read it here (don't consume), so a wheel bound to a PSX
	 * button AND to the graphics keys both see the same notch. */
	if (g_PsyX_WheelUpFrames   > 0 && g_cfg_mouseButtonMask[6]) ret &= ~g_cfg_mouseButtonMask[6];
	if (g_PsyX_WheelDownFrames > 0 && g_cfg_mouseButtonMask[7]) ret &= ~g_cfg_mouseButtonMask[7];
	return ret;
}

u_short PsyX_Pad_UpdateKeyboardInput()
{
	u_short ret;

	//Not initialised yet
	if (g_sdlKeyboardState == NULL)
		return 0xFFFF;

	SDL_PumpEvents();

	/* Report every distinct key this machine can produce, once each.
	 *
	 * An arcade cabinet's controls are an encoder emitting scancodes nobody can
	 * guess from here, and "most buttons do nothing" cannot be fixed without
	 * knowing WHICH ones they are -- the default map (C/V/Z/X/Enter/arrows) is
	 * a PC keyboard layout that a cabinet has no reason to match. Each scancode
	 * logs a single line the first time it is seen, so pressing every button
	 * once produces a complete map of the hardware and nothing after that. */
	{
		static unsigned char s_keySeen[SDL_NUM_SCANCODES];
		static int           s_keyCount = 0;
		int                  sc;

		for (sc = 0; sc < SDL_NUM_SCANCODES && s_keyCount < 64; sc++)
		{
			if (g_sdlKeyboardState[sc] && !s_keySeen[sc])
			{
				const char* nm = SDL_GetScancodeName((SDL_Scancode)sc);

				s_keySeen[sc] = 1;
				s_keyCount++;
				eprintinfo("[KEY] scancode %d = '%s'\n", sc, (nm && nm[0]) ? nm : "(unnamed)");
			}
		}
	}

	ret = PsyX_Pad_BuildKbWord(g_cfg_keyboardMapping);

	/* Secondary key binds + mouse buttons (gated by allow_mouse_secondary,
	 * forced on in TPS). Active-low: a button is pressed if clear in ANY
	 * source, so the layers combine with AND. */
	if (g_cfg_allowMouseSecondary)
	{
		ret &= PsyX_Pad_BuildKbWord(g_cfg_keyboardMapping2);
		ret &= PsyX_Pad_BuildMouseWord();
	}

#if defined(__ANDROID__)
	/* Android hands one physical gamepad button to SDL through several input
	 * devices at once (a pad enumerates a main node plus a "Consumer Control"
	 * node), and SDL folds them all into ONE global scancode array with no
	 * device identity. The devices' UP/DOWN events interleave, so one node's UP
	 * clears the shared bit while the button is still physically held. Measured
	 * on a GameSir X5: press, 32ms, release, 16ms, press again — which the game
	 * reads as two rising edges and hands to two different screens (Start on the
	 * title also picking New Game; a press that skips the intro FMV also
	 * confirming its way into the difficulty select).
	 *
	 * Hold a release briefly: one reversed this fast was never a real release.
	 * Only the release edge is affected, so presses stay instant, and the window
	 * is well under the ~120ms a deliberate double-tap takes.
	 *
	 * This is a repair for the shared-scancode collision, not for the pad. The
	 * clean route is opening the device as an SDL_GameController so each node
	 * keeps its own state — see the [PAD] enumeration logged at init. */
	{
		#define KB_RELEASE_HOLD_MS 40

		static Uint32 s_lastPressedMs[16];
		Uint32        now = SDL_GetTicks();
		int           bit;

		for (bit = 0; bit < 16; bit++)
		{
			u_short mask = (u_short)(1 << bit);

			if (!(ret & mask))
			{
				s_lastPressedMs[bit] = now;
			}
			else if (s_lastPressedMs[bit] != 0 &&
			         (now - s_lastPressedMs[bit]) < KB_RELEASE_HOLD_MS)
			{
				ret &= ~mask;
			}
		}
	}
#endif

	return ret;
}

int PsyX_Pad_GetStatus(int mtap, int slot)
{
	PsyXController* controller;

	if (slot == 0)
		return 1;	// keyboard always here

	controller = &g_controllers[slot];

	if (controller->gc && SDL_GameControllerGetAttached(controller->gc))
		return 1;

	return 0;
}

void PsyX_Pad_Vibrate(int mtap, int slot, unsigned char* table, int len)
{
	PsyXController* controller = &g_controllers[slot];

	if (len == 0)
		return;

	Uint16 freq_high	= table[0] * 255;
	Uint16 freq_low		= len > 1 ? table[1] * 255 : 0;

	// apply minimal shake
	if(freq_low != 0 && freq_low < 4096)
		freq_low = 4096;

	if (freq_high != 0 && freq_high < 4096)
		freq_high = 4096;

	SDL_GameControllerRumble(controller->gc, freq_low, freq_high, 200);
}

void PsyX_Pad_SetActBuffer(int slot, unsigned char* table, int len)
{
	if (slot < 0 || slot >= MAX_CONTROLLERS)
		return;

	g_actBufTable[slot] = table;
	g_actBufLen[slot]   = (table != NULL) ? len : 0;

	if (g_actBufLen[slot] == 0)
	{
		PsyXController* controller = &g_controllers[slot];
		if (controller->gc)
			SDL_GameControllerRumble(controller->gc, 0, 0, 0);
	}
}
