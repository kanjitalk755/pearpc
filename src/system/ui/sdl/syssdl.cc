/* 
 *	PearPC
 *	syssdl.cc
 *
 *	Copyright (C)      2004 Jens v.d. Heydt (mailme@vdh-webservice.de)
 *	Copyright (C) 1999-2002 Stefan Weyergraf
 *	Copyright (C) 1999-2004 Sebastian Biallas (sb@biallas.net)
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <SDL.h>

#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <cstring>

// for stopping the CPU
#include "cpu/cpu.h"

#include "system/sysclk.h"
#include "system/display.h"
#include "system/keyboard.h"
#include "system/mouse.h"
#include "system/systhread.h"
#include "system/systimer.h"

#include "tools/snprintf.h"

#include "syssdl.h"
#ifdef __MACOSX__
#include "cocoa.h"
#endif

static bool	gSDLVideoExposePending = false;
SDL_TimerID SDL_RedrawTimerID;

SDLSystemDisplay *sd;

static uint8 scancode_to_adb_key[256] = {
//	0    1    2    3    4    5    6    7    8    9    a    b    c    d    e    f
	0xff,0xff,0xff,0xff,0x00,0x0b,0x08,0x02,0x0e,0x03,0x05,0x04,0x22,0x26,0x28,0x25,
	0x2e,0x2d,0x1f,0x23,0x0c,0x0f,0x01,0x11,0x20,0x09,0x0d,0x07,0x10,0x06,0x12,0x13,
	0x14,0x15,0x17,0x16,0x1a,0x1c,0x19,0x1d,0x24,0x35,0x33,0x30,0x31,0x1b,0x18,0x21,
	0x1e,0x2a,0xff,0x29,0x27,0x32,0x2b,0x2f,0x2c,0x39,0x7a,0x78,0x63,0x76,0x60,0x61,
	0x62,0x64,0x65,0x6d,0x67,0x6f,0x69,0xff,0xff,0xff,0x73,0x74,0x75,0x77,0x79,0x3c,
	0x3b,0x3d,0x3e,0x47,0x4b,0x43,0x4e,0x45,0x4c,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
	0x5b,0x5c,0x52,0x41,0xff,0xff,0xff,0x51,0xff,0xff,0xff,0xff,0xff,0xff,0x7f,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0x36,0x38,0x3a,0x37,0x36,0x38,0x3a,0x37,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
};

static bool handleSDLEvent(const SDL_Event &event)
{
	static bool mouseButton[3] = {false, false, false};
	bool tmpMouseButton[3];

	SystemEvent ev;
	switch (event.type) {
	case SDL_USEREVENT:
		if (event.user.code == 1) {  // helper for changeResolution
			//ht_printf("got forward event\n");
			sd->mChangeResRet = sd->changeResolutionREAL(sd->mSDLChartemp);
			SDL_CondSignal(sd->mWaitcondition); // Signal, that condition is over.
		}
		return true;
	case SDL_KEYUP:
		ev.key.keycode = scancode_to_adb_key[event.key.keysym.scancode];
//		ht_printf("%x %x up  ", event.key.keysym.scancode, ev.key.keycode);
		if ((ev.key.keycode & 0xff) == 0xff) break;
		ev.type = sysevKey;
		ev.key.pressed = false;
		gKeyboard->handleEvent(ev);
		return true;
	case SDL_KEYDOWN:
		ev.key.keycode = scancode_to_adb_key[event.key.keysym.scancode];
//		ht_printf("%x %x %x dn  \n", event.key.keysym.sym, event.key.keysym.scancode, ev.key.keycode);
		if ((ev.key.keycode & 0xff) == 0xff) break;
		ev.type = sysevKey;
		ev.key.pressed = true;
		gKeyboard->handleEvent(ev);
		return true;
	case SDL_MOUSEBUTTONDOWN:
		ev.type = sysevMouse;
		ev.mouse.type = sme_buttonPressed;
		memcpy(tmpMouseButton, mouseButton, sizeof (tmpMouseButton));
		mouseButton[0] = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(1);
		mouseButton[1] = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(2);
		mouseButton[2] = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(3);
		ev.mouse.button1 = mouseButton[0];
		ev.mouse.button2 = mouseButton[1];
		ev.mouse.button3 = mouseButton[2];
		if (mouseButton[0] != tmpMouseButton[0]) {
			ev.mouse.dbutton = 1;
		} else if (mouseButton[1] != tmpMouseButton[1]) {
			ev.mouse.dbutton = 2;
		} else if (mouseButton[2] != tmpMouseButton[2]) {
			ev.mouse.dbutton = 3;
		} else {
			ev.mouse.dbutton = 0;
		}
		ev.mouse.x = gDisplay->mCurMouseX;
		ev.mouse.y = gDisplay->mCurMouseY;
		ev.mouse.relx = 0;
		ev.mouse.rely = 0;
		gMouse->handleEvent(ev);
		return true;
	case SDL_MOUSEBUTTONUP:
		ev.type = sysevMouse;
		ev.mouse.type = sme_buttonReleased;
		memcpy(tmpMouseButton, mouseButton, sizeof (tmpMouseButton));
		mouseButton[0] = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(1);
		mouseButton[1] = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(2);
		mouseButton[2] = SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(3);
		ev.mouse.button1 = mouseButton[0];
		ev.mouse.button2 = mouseButton[1];
		ev.mouse.button3 = mouseButton[2];
		if (mouseButton[0] != tmpMouseButton[0]) {
			ev.mouse.dbutton = 1;
		} else if (mouseButton[1] != tmpMouseButton[1]) {
			ev.mouse.dbutton = 2;
		} else if (mouseButton[2] != tmpMouseButton[2]) {
			ev.mouse.dbutton = 3;
		} else {
			ev.mouse.dbutton = 0;
		}
		ev.mouse.x = gDisplay->mCurMouseX;
		ev.mouse.y = gDisplay->mCurMouseY;
		ev.mouse.relx = 0;
		ev.mouse.rely = 0;
		gMouse->handleEvent(ev);
		return true;
	case SDL_MOUSEMOTION:
		ev.type = sysevMouse;
		ev.mouse.type = sme_motionNotify;
		ev.mouse.button1 = mouseButton[0];
		ev.mouse.button2 = mouseButton[1];
		ev.mouse.button3 = mouseButton[2];
		ev.mouse.dbutton = 0;
		ev.mouse.x = event.motion.y;
		ev.mouse.y = event.motion.x;
		ev.mouse.relx = event.motion.xrel;
		ev.mouse.rely = event.motion.yrel;
		gMouse->handleEvent(ev);
		return true;
	case SDL_QUIT:
		gDisplay->setFullscreenMode(false);
		return false;
	}
	return true;
}

static Uint32 SDL_redrawCallback(Uint32 interval, void *param)
{
	gSDLVideoExposePending = true;
	return interval;
}

void sys_gui_event() {
	if (gSDLVideoExposePending) {
		gDisplay->displayShow();
		gSDLVideoExposePending = false;
	}
	SDL_Event event;
	if (SDL_PollEvent(&event) && !handleSDLEvent(event)) {
		gDisplay->setMouseGrab(false);
		if (SDL_RedrawTimerID)
			SDL_RemoveTimer(SDL_RedrawTimerID);
		ppc_cpu_stop();
	}
}

sys_timer gSDLRedrawTimer;
static bool eventThreadAlive;

SystemDisplay *allocSystemDisplay(const char *title, const DisplayCharacteristics &chr, int redraw_ms);
SystemKeyboard *allocSystemKeyboard();
SystemMouse *allocSystemMouse();

void initUI(const char *title, const DisplayCharacteristics &aCharacteristics, int redraw_ms, const KeyboardCharacteristics &keyConfig, bool fullscreen)
{
	gDisplay = allocSystemDisplay(title, aCharacteristics, redraw_ms);
	gMouse = allocSystemMouse();
	gKeyboard = allocSystemKeyboard();
	if (!gKeyboard->setKeyConfig(keyConfig)) {
		ht_printf("no keyConfig, or is empty");
		exit(1);
	}

	gDisplay->mFullscreen = fullscreen;

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_NOPARACHUTE) < 0) {
		ht_printf("SDL: Unable to init: %s\n", SDL_GetError());
		exit(1);
	}
#ifdef __MACOSX__
	disable_SDL2_macosx_menu_bar_keyboard_shortcuts();
#endif
	atexit(SDL_Quit); // give SDl a chance to clean up before exit!
	sd = (SDLSystemDisplay*)gDisplay;

	sd->initCursor();

	sd->mEventThreadID = SDL_ThreadID();

	sd->changeResolution(sd->mClientChar);
	sd->updateTitle();
	sd->setExposed(true);

	gSDLVideoExposePending = false;
	SDL_RedrawTimerID = SDL_AddTimer(gDisplay->mRedraw_ms, SDL_redrawCallback, NULL);

	sd->setFullscreenMode(sd->mFullscreen);
}

void doneUI()
{
	if (eventThreadAlive) {
		SDL_Event event;
		event.type = SDL_QUIT;
		SDL_PushEvent(&event);
		while (eventThreadAlive) SDL_Delay(10); // FIXME: UGLY!
	}
	SDL_Quit();
}
