/* 
 *	PearPC
 *	sysdisplay.cc - screen access functions for SDL
 *
 *	Copyright (C)      2004 Jens v.d. Heydt (mailme@vdh-webservice.de)
 *	Copyright (C)      2004 John Kelley (pearpc@kelley.ca)
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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <SDL.h>

#include "system/display.h"
#include "system/sysexcept.h"
#include "system/systhread.h"
#include "system/sysvaccel.h"
#include "system/types.h"

#include "tools/data.h"
#include "tools/debug.h"
#include "tools/snprintf.h"

//#include "io/graphic/gcard.h"
#include "configparser.h"

//#define DPRINTF(a...)
#define DPRINTF(a...) ht_printf("[Display/SDL]: " a)

#include "syssdl.h"
#include "info.h"


uint SDLSystemDisplay::bitsPerPixelToXBitmapPad(uint bitsPerPixel)
{
	if (bitsPerPixel <= 8) {
		return 8;
	} else if (bitsPerPixel <= 16) {
		return 16;
	} else {
		return 32;
	}
}

#define MASK(shift, size) (((1 << (size))-1)<<(shift))

void SDLSystemDisplay::dumpDisplayChar(const DisplayCharacteristics &chr)
{
	fprintf(stderr, "\tdimensions:          %d x %d pixels\n", chr.width, chr.height);
	fprintf(stderr, "\tpixel size in bytes: %d\n", chr.bytesPerPixel);
	fprintf(stderr, "\tpixel size in bits:  %d\n", chr.bytesPerPixel*8);
	fprintf(stderr, "\tred_mask:            %08x (%d bits)\n", MASK(chr.redShift, chr.redSize), chr.redSize);
	fprintf(stderr, "\tgreen_mask:          %08x (%d bits)\n", MASK(chr.greenShift, chr.greenSize), chr.greenSize);
	fprintf(stderr, "\tblue_mask:           %08x (%d bits)\n", MASK(chr.blueShift, chr.blueSize), chr.blueSize);
	fprintf(stderr, "\tdepth:               %d\n", chr.redSize + chr.greenSize + chr.blueSize);
}

SDLSystemDisplay::SDLSystemDisplay(const char *title, const DisplayCharacteristics &chr, int redraw_ms)
: SystemDisplay(chr, redraw_ms)
{
	mTitle = strdup(title);

	gFrameBuffer = (byte*)malloc(mClientChar.width *
		mClientChar.height * mClientChar.bytesPerPixel);
	memset(gFrameBuffer, 0, mClientChar.width *
		mClientChar.height * mClientChar.bytesPerPixel);
	damageFrameBufferAll();

	mWindow = NULL;
	mRenderer = NULL;
	mTexture = NULL;
	mFrameBufferCnv = NULL;

	sys_create_mutex(&mRedrawMutex);
}

void SDLSystemDisplay::finishMenu()
{
}

void SDLSystemDisplay::updateTitle() 
{
	String key;
	int key_toggle_mouse_grab = gKeyboard->getKeyConfig().key_toggle_mouse_grab;
	SystemKeyboard::convertKeycodeToString(key, key_toggle_mouse_grab);
	String curTitle;
	curTitle.assignFormat("%s - [%s %s mouse]", mTitle, key.contentChar(), (isMouseGrabbed() ? "disables" : "enables"));
	SDL_SetWindowTitle(mWindow, curTitle.contentChar());
}

int SDLSystemDisplay::toString(char *buf, int buflen) const
{
	return snprintf(buf, buflen, "SDL");
}

void SDLSystemDisplay::displayShow()
{
	if (!isExposed()) return;

	int firstDamagedLine, lastDamagedLine;
	// We've got problems with races here because gcard_write1/2/4
	// might set gDamageAreaFirstAddr, gDamageAreaLastAddr.
	// We can't use mutexes in gcard for speed reasons. So we'll
	// try to minimize the probability of loosing the race.
	if (gDamageAreaFirstAddr > gDamageAreaLastAddr+3) {
	        return;
	}
	int damageAreaFirstAddr = gDamageAreaFirstAddr;
	int damageAreaLastAddr = gDamageAreaLastAddr;
	healFrameBuffer();
	// end of race
	damageAreaLastAddr += 3;	// this is a hack. For speed reasons we
					// inaccurately set gDamageAreaLastAddr
					// to the first (not last) byte accessed
					// accesses are up to 4 bytes "long".
	firstDamagedLine = damageAreaFirstAddr / (mClientChar.width * mClientChar.bytesPerPixel);
	lastDamagedLine = damageAreaLastAddr / (mClientChar.width * mClientChar.bytesPerPixel);
	// Overflow may happen, because of the hack used above
	// and others, that set lastAddr = 0xfffffff0 (damageFrameBufferAll())
	if (lastDamagedLine >= mClientChar.height) {
		lastDamagedLine = mClientChar.height-1;
	}

	sys_lock_mutex(mRedrawMutex);
	if (mClientChar.bytesPerPixel == 2) {
		uint16 *sp = &((uint16 *)gFrameBuffer)[mClientChar.width * firstDamagedLine];
		uint16 *dp = &mFrameBufferCnv[mClientChar.width * firstDamagedLine];
		for (int i = mClientChar.width * (lastDamagedLine - firstDamagedLine + 1); i > 0; i--)
				*dp++ = __builtin_bswap16(*sp++);
	}
	sys_unlock_mutex(mRedrawMutex);

	SDL_UpdateTexture(mTexture, NULL, mClientChar.bytesPerPixel == 2 ? (byte *)mFrameBufferCnv : gFrameBuffer, mClientChar.bytesPerPixel * mClientChar.width);
	SDL_RenderClear(mRenderer);
	SDL_RenderCopy(mRenderer, mTexture, NULL, NULL);
	SDL_RenderPresent(mRenderer);
}

void SDLSystemDisplay::convertCharacteristicsToHost(DisplayCharacteristics &aHostChar, const DisplayCharacteristics &aClientChar)
{
	aHostChar = aClientChar;
}

bool SDLSystemDisplay::changeResolution(const DisplayCharacteristics &aCharacteristics)
{
	// We absolutely have to make sure that SDL_calls are only used
	// in the thread, that did SDL_INIT and created Surfaces etc...
	// This function behaves as a forward-function for changeResolution calls.
	// It creates an SDL_Condition and pushes a userevent (no.1) onto
	// the event queue. SDL_CondWait is used to wait for the event-thread
	// to do the actual work (in reacting on the event and calling changeResolutionREAL)
	// and finally signaling back to us, with SDL_Signal, that work is done.
	
	// AND: we have to check if the call came from another thread.
	// otherwise we would block and wait for our own thread to continue.-> endless loop
		
	mSDLChartemp = aCharacteristics;
	if (SDL_ThreadID() != mEventThreadID) { // called from a different thread than sdl eventloop
		SDL_Event ev;
		SDL_mutex *tmpmutex;
	
		//DPRINTF("Forward handler got called\n");
		ev.type = SDL_USEREVENT;
		ev.user.code = 1;
				
	
		tmpmutex = SDL_CreateMutex();
		mWaitcondition = SDL_CreateCond();
	
		SDL_LockMutex(tmpmutex);
		SDL_PushEvent(&ev);		

	 	SDL_CondWait(mWaitcondition, tmpmutex);
		//SDL_CondWait(mWaitcondition, tmpmutex, 5000);

		SDL_UnlockMutex(tmpmutex);
		SDL_DestroyMutex(tmpmutex);
		SDL_DestroyCond(mWaitcondition);
		return mChangeResRet;
	} else {
		// we can call it directly because we are in the same thread
		//ht_printf("direct call\n");
		return changeResolutionREAL(aCharacteristics);
	}

}

bool SDLSystemDisplay::changeResolutionREAL(const DisplayCharacteristics &aCharacteristics)
{
	DisplayCharacteristics chr;
	DPRINTF("changeRes got called\n");
	convertCharacteristicsToHost(chr, aCharacteristics);

	/*
	 * From the SDL documentation:
	 * "Note: The bpp parameter is the number of bits per pixel,
	 * so a bpp of 24 uses the packed representation of 3 bytes/pixel.
	 * For the more common 4 bytes/pixel mode, use a bpp of 32.
	 * Somewhat oddly, both 15 and 16 will request a 2 bytes/pixel
	 * mode, but different pixel formats."
	 *
	 * Because of their odd convention, we have to mess with 
	 * bytesPerPixel here.
	 */
	uint bitsPerPixel;
	switch (chr.bytesPerPixel) {
	case 2:
		bitsPerPixel = 15;
		break;
	case 4:
		bitsPerPixel = 32;
		break;
	default:
		ASSERT(0);
		break;
	}
		
	DPRINTF("SDL: Changing resolution to %dx%dx%d\n", aCharacteristics.width, aCharacteristics.height,bitsPerPixel);

	mSDLChar = chr;
	mClientChar = aCharacteristics;
	
	sys_lock_mutex(mRedrawMutex);

	if (mFrameBufferCnv) {
		delete[] mFrameBufferCnv;
		mFrameBufferCnv = NULL;
	}
	if (mTexture) SDL_DestroyTexture(mTexture);
	if (mRenderer) SDL_DestroyRenderer(mRenderer);
	if (mWindow) SDL_DestroyWindow(mWindow);
	mWindow = SDL_CreateWindow(APPNAME " " APPVERSION,
							   SDL_WINDOWPOS_UNDEFINED,
							   SDL_WINDOWPOS_UNDEFINED,
							   aCharacteristics.width, aCharacteristics.height,
							   mFullscreen ? SDL_WINDOW_FULLSCREEN : 0);
	if (!mWindow) {
		// FIXME: this is really bad.
		ht_printf("SDL: FATAL: can't switch mode?!\n");
		exit(1);
	}
	mRenderer = SDL_CreateRenderer(mWindow, -1, 0);
	mTexture = SDL_CreateTexture(mRenderer,
								 mClientChar.bytesPerPixel == 2 ? SDL_PIXELFORMAT_ARGB1555 : SDL_PIXELFORMAT_BGRA8888,
								 SDL_TEXTUREACCESS_STREAMING,
								 aCharacteristics.width, aCharacteristics.height);
	if (mClientChar.bytesPerPixel == 2)
		mFrameBufferCnv = new uint16[aCharacteristics.width * aCharacteristics.height];

	mFullscreenChanged = mFullscreen;
	gFrameBuffer = (byte*)realloc(gFrameBuffer, mClientChar.width *
		mClientChar.height * mClientChar.bytesPerPixel);
	damageFrameBufferAll();

	sys_unlock_mutex(mRedrawMutex);
	return true;
}

void SDLSystemDisplay::getHostCharacteristics(Container &modes)
{
}

void SDLSystemDisplay::setMouseGrab(bool enable)
{
	if (enable == isMouseGrabbed()) return;
	SystemDisplay::setMouseGrab(enable);
	SDL_SetRelativeMouseMode(enable ? SDL_TRUE : SDL_FALSE);
}

void SDLSystemDisplay::initCursor()
{
}

SystemDisplay *allocSystemDisplay(const char *title, const DisplayCharacteristics &chr, int redraw_ms)
{
	DPRINTF("Making new window %d x %d\n", chr.width, chr.height);
	return new SDLSystemDisplay(title, chr, redraw_ms);
}
