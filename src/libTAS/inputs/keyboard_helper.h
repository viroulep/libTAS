/*
    Copyright 2015-2016 Clément Gallet <clement.gallet@ens-lyon.org>

    This file is part of libTAS.

    libTAS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libTAS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libTAS.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBTAS_KEYBOARDHELPER_H_INCLUDED
#define LIBTAS_KEYBOARDHELPER_H_INCLUDED

#include "../../external/SDL.h"
#include <X11/keysym.h>

/* Helper functions to translate from a Xlib KeySym to
 * a SDL 1 SDLKey or a SDL 2 Keycode
 */
SDL_Keycode X11_TranslateKeysym(KeySym kc);
SDL1::SDLKey X11_Translate1Keysym(KeySym xsym);

/* Helper functions to translate from a SDL 1 SDLKey or
 * a SDL 2 Keycode to a SDL Scancode
 */
SDL_Scancode GetScanFromKey(SDL_Keycode);
SDL_Scancode GetScanFromKey1(SDL1::SDLKey key);

void X11_InitKeymap(void);

/* Fill the keyboard array that SDL functions are expected,
 * based on the KeySym set we get from our AllInputs struct.
 */
void xkeyboardToSDLkeyboard(KeySym Xkeyboard[], Uint8* SDLkeyboard);
void xkeyboardToSDL1keyboard(KeySym Xkeyboard[], Uint8* SDLkeyboard);

/* Fill the SDL 1 or SDL 2 full key struct based on a Xlib KeySym */
void xkeysymToSDL(SDL_Keysym *keysym, KeySym xkeysym);
void xkeysymToSDL1(SDL1::SDL_keysym *keysym, KeySym xkeysym);

#endif

