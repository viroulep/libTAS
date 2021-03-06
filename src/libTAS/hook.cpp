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

#include "hook.h"
#include "logging.h"
#include <string>
#include "dlhook.h"

bool link_function(void** function, const char* source, const char* library)
{
    /* Test if function is already linked */
    if (*function != nullptr)
        return true;

    /* Initialize the pointers to use real dl functions */
    dlhook_init();

    dlenter();
    /* From this function dl* call will refer to real dl functions */

    /* First try to link it from the global namespace */
    *function = dlsym(RTLD_NEXT, source);

    if (*function != nullptr) {
        dlleave();
        return true;
    }

    /* If it did not succeed, try to link using a matching library
     * loaded by the game.
     */

    if (library != nullptr) {
        safe::string libpath = find_lib(library);

        if (! libpath.empty()) {

            /* Try to link again using a matching library */
            void* handle = dlopen(libpath.c_str(), RTLD_LAZY);

            if (handle != NULL) {
                *function = dlsym(handle, source);

                if (*function != nullptr) {
                    dlleave();
                    return true;
                }
            }
        }
    }
    debuglogstdio(LCF_ERROR | LCF_HOOK, "Could not import symbol %s", source);

    *function = nullptr;
    dlleave();
    return false;
}

int SDLver = 0;

namespace orig {
    void (*SDL_GetVersion)(SDL_version* ver);
    /* SDL 1.2 specific functions */
    SDL_version * (*SDL_Linked_Version)(void);
}

int get_sdlversion(void)
{
    if (SDLver != 0)
        return 1;

    LINK_NAMESPACE_SDL2(SDL_GetVersion);
    if (orig::SDL_GetVersion == nullptr)
        LINK_NAMESPACE_SDL1(SDL_Linked_Version);

    /* Determine SDL version */
    SDL_version ver = {0, 0, 0};
    if (orig::SDL_GetVersion) {
        orig::SDL_GetVersion(&ver);
    }
    else if (orig::SDL_Linked_Version) {
        SDL_version *verp;
        verp = orig::SDL_Linked_Version();
        ver = *verp;
    }

    debuglog(LCF_SDL | LCF_HOOK, "Detected SDL ", static_cast<int>(ver.major), ".", static_cast<int>(ver.minor), ".", static_cast<int>(ver.patch));

    /* We save the version major in an extern variable because we will use it elsewhere */
    SDLver = ver.major;

    if (ver.major == 0) {
        debuglog(LCF_ERROR | LCF_SDL | LCF_HOOK, "Could not get SDL version...");
        return 0;
    }

    return 1;
}

