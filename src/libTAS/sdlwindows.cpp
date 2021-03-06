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

#include "sdlwindows.h"
#include "hook.h"
#include "logging.h"
#include "socket.h"
#include "../shared/messages.h"
#include "../shared/tasflags.h"
#include "frame.h"
//#include "libTAS.h"
#include "renderhud/RenderHUD_GL.h"
#include "renderhud/RenderHUD_SDL1.h"
#include "renderhud/RenderHUD_SDL2.h"
#include "ThreadState.h"
#include "avdumping.h"

/* 
 * Store the game window pointer
 * We assume the game never open multiple windows at a time
 */
SDL_Window* gameWindow = nullptr;

/* Has the game window pointer be sent to the program? */
bool gw_sent = 0;

/* Does the game use openGL? */
bool video_opengl = false;

/* Path of the dump file */
char* av_filename = nullptr;

/* Original function pointers */
namespace orig {
    static void(* SDL_GL_SwapWindow)(SDL_Window* window);
    static SDL_Window*(* SDL_CreateWindow)(const char*, int, int, int, int, Uint32);
    Uint32 (*SDL_GetWindowID)(SDL_Window*);
    static Uint32 (*SDL_GetWindowFlags)(SDL_Window*);
    static void (*SDL_SetWindowTitle)(void * window, const char *title);
    static void (*SDL_WM_SetCaption)(const char *title, const char *icon);
    static SDL_bool (*SDL_GetWindowWMInfo)(SDL_Window* window, SDL_SysWMinfo* info);
    static void* (*SDL_GL_CreateContext)(SDL_Window *window);
    static int (*SDL_GL_SetSwapInterval)(int interval);
    static void (*SDL_DestroyWindow)(SDL_Window*);
    static void (*SDL_SetWindowSize)(SDL_Window* window, int w, int h);

    static SDL_Renderer* (*SDL_CreateRenderer)(SDL_Window * window, int index, Uint32 flags);
    static int (*SDL_CreateWindowAndRenderer)(int, int, Uint32, SDL_Window**, SDL_Renderer**);
    static void (*SDL_RenderPresent)(SDL_Renderer * renderer);

    static SDL1::SDL_Surface *(*SDL_SetVideoMode)(int width, int height, int bpp, Uint32 flags);
    static void (*SDL_GL_SwapBuffers)(void);
    static int (*SDL_Flip)(SDL1::SDL_Surface *screen);
}

/* SDL 1.2 */
/* Override */ void SDL_GL_SwapBuffers(void)
{
    debuglog(LCF_SDL | LCF_FRAME | LCF_OGL | LCF_WINDOW, __func__, " call.");

    /* SDL 1.2 does only have one window,
     * thus it does not provide any access to window identifiers.
     * We need to pass a window id to linTAS so that it can capture inputs.
     * In our case, let's just pass a dummy value indicating that
     * we could not get access to it.
     * It will have to guess it, probably by getting the active window
     */
    if (!gw_sent) {
        Window w = 0;
        sendMessage(MSGB_WINDOW_ID);
        sendData(&w, sizeof(Window));
        gw_sent = 1;
        debuglog(LCF_SDL, "Send dummy X11 window id.");
    }

    /* Start the frame boundary and pass the function to draw */
#ifdef LIBTAS_ENABLE_HUD
    static RenderHUD_GL renderHUD;
    frameBoundary(true, [] () {orig::SDL_GL_SwapBuffers();}, renderHUD);
#else
    frameBoundary(true, [] () {orig::SDL_GL_SwapBuffers();});
#endif
}

int sendXid(void);
int sendXid(void)
{
    if (gameWindow != nullptr) {
        if (!gw_sent) {

            /* Access the X Window identifier from the SDL_Window struct */
            SDL_SysWMinfo info;
            orig::SDL_GetVersion(&info.version);
            if (orig::SDL_GetWindowWMInfo(gameWindow, &info) == SDL_FALSE) {
                debuglog(LCF_SDL | LCF_ERROR, "Could not get the X11 window identifier");
                return -1;
            }
            if (info.subsystem != SDL_SYSWM_X11) {
                debuglog(LCF_SDL | LCF_ERROR, "SDL says we are not running on X11");
                return -1;
            }
            Window xgw = info.info.x11.window;

            /* Send the X Window identifier to the program */
            sendMessage(MSGB_WINDOW_ID);
            sendData(&xgw, sizeof(Window));
            gw_sent = true;
            debuglog(LCF_SDL, "Send X11 window id: ", xgw);
        }
    }
    else {
        debuglog(LCF_SDL | LCF_ERROR, "Window pointer is empty but the game wants to draw something.");
        return -1;
    }
    return 0;
}

/* Override */ void SDL_GL_SwapWindow(SDL_Window* window)
{
    debuglog(LCF_SDL | LCF_FRAME | LCF_OGL | LCF_WINDOW, __func__, " call.");

    /* 
     * We need to pass the game window identifier to the program
     * so that it can capture inputs
     */
    if (sendXid() != 0)
        return;

    /* Start the frame boundary and pass the function to draw */
#ifdef LIBTAS_ENABLE_HUD
    static RenderHUD_GL renderHUD;
    frameBoundary(true, [&] () {orig::SDL_GL_SwapWindow(window);}, renderHUD);
#else
    frameBoundary(true, [&] () {orig::SDL_GL_SwapWindow(window);});
#endif
}

void* SDL_GL_CreateContext(SDL_Window *window)
{
    DEBUGLOGCALL(LCF_SDL | LCF_OGL | LCF_WINDOW);
    //threadState.setNative(true);
    void* context = orig::SDL_GL_CreateContext(window);
    //threadState.setNative(false);

    /* We override this function just to disable vsync,
     * except when using non deterministic timer.
     */
    if (tasflags.framerate > 0)
        orig::SDL_GL_SetSwapInterval(0);
    return context;
}

static int swapInterval = 0;

/* Override */ int SDL_GL_SetSwapInterval(int interval)
{
    debuglog(LCF_SDL | LCF_OGL | LCF_WINDOW, __func__, " call - setting to ", interval);

    /* We save the interval if the game wants it later */
    swapInterval = interval;
   
    /* When using non deterministic timer, we let the game set vsync */
    if (tasflags.framerate > 0)
        return orig::SDL_GL_SetSwapInterval(interval);
    
    return 0; // Success
}
    
/* Override */ int SDL_GL_GetSwapInterval(void)
{
    DEBUGLOGCALL(LCF_SDL | LCF_OGL | LCF_WINDOW);
    return swapInterval;
}

std::string origTitle;
std::string origIcon;

/* Override */ SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags){
    debuglog(LCF_SDL | LCF_WINDOW, __func__, " call - title: ", title, ", pos: (", x, ",", y, "), size: (", w, ",", h, "), flags: 0x", std::hex, flags, std::dec);

    origTitle = title;

    /* Disable fullscreen */
    flags &= 0xFFFFFFFF ^ SDL_WINDOW_FULLSCREEN_DESKTOP;

    /* Disable hidden windows */
    flags &= 0xFFFFFFFF ^ SDL_WINDOW_HIDDEN;

    /* Disable high DPI mode */
    flags &= 0xFFFFFFFF ^ SDL_WINDOW_ALLOW_HIGHDPI;

    threadState.setNative(true);
    gameWindow = orig::SDL_CreateWindow(title, x, y, w, h, flags); // Save the game window
    threadState.setNative(false);
    /* A new window was created. It needs to be passed to the program */
    gw_sent = false;

#ifdef LIBTAS_ENABLE_AVDUMPING
    /* Initializing the video dump */
    if (tasflags.av_dumping) {
        if (flags & SDL_WINDOW_OPENGL)
            video_opengl = true;
        else
            video_opengl = false;

        debuglog(LCF_DUMP, "Start AV dumping on file ", av_filename);
        int av = openAVDumping(gameWindow, video_opengl, av_filename, frame_counter);
        if (av != 0) {
            /* Init failed, disable AV dumping */
            tasflags.av_dumping = 0;
        }
    }
#endif

    return gameWindow;
}

/* Override */ void SDL_DestroyWindow(SDL_Window* window){
    DEBUGLOGCALL(LCF_SDL | LCF_WINDOW);
    //threadState.setNative(true);
    orig::SDL_DestroyWindow(window);
    //threadState.setNative(false);
    if (gameWindow == window)
        gameWindow = NULL;
#ifdef LIBTAS_ENABLE_AVDUMPING
    if (tasflags.av_dumping)
        closeAVDumping();
#endif
}

/* Override */ Uint32 SDL_GetWindowID(SDL_Window* window){
    DEBUGLOGCALL(LCF_SDL | LCF_WINDOW);
    return orig::SDL_GetWindowID(window);
}

/* Override */ Uint32 SDL_GetWindowFlags(SDL_Window* window){
    DEBUGLOGCALL(LCF_SDL | LCF_WINDOW);
    return orig::SDL_GetWindowFlags(window);
}

/* Override */ void SDL_SetWindowTitle(SDL_Window * window, const char *title)
{
    debuglog(LCF_SDL | LCF_WINDOW, __func__, " call with title ", title);
    if (title)
        origTitle = title;
    orig::SDL_SetWindowTitle(window, title);
}

/* Override */ void SDL_WM_SetCaption(const char *title, const char *icon)
{
    debuglog(LCF_SDL | LCF_WINDOW, __func__, " call with title ", title);
    if (title)
        origTitle = title;
    if (icon)
        origIcon = icon;
    orig::SDL_WM_SetCaption(title, icon);
}

void updateTitle(float fps, float lfps)
{
    std::ostringstream out;
    out << " (fps: " << std::fixed << std::setprecision(1) << fps;
    out << " - lfps: " << lfps << ")";
    std::string newTitle = origTitle + out.str();
    if (SDLver == 1) {
        orig::SDL_WM_SetCaption(newTitle.c_str(), origIcon.c_str());
    }
    if (SDLver == 2) {
        if (gameWindow)
            orig::SDL_SetWindowTitle(gameWindow, newTitle.c_str());
    }
}

/* Override */ int SDL_SetWindowFullscreen(SDL_Window * window, Uint32 flags)
{
    debuglog(LCF_SDL | LCF_WINDOW, __func__, " call with flags ", flags);
    return 0; // success
}

/* Override */ void SDL_SetWindowBordered(SDL_Window * window, SDL_bool bordered)
{
    debuglog(LCF_SDL | LCF_WINDOW, __func__, " call with border ", bordered);
    /* Don't do anything */
}

/* Override */ SDL_Renderer *SDL_CreateRenderer(SDL_Window * window, int index, Uint32 flags)
{
    DEBUGLOGCALL(LCF_SDL | LCF_WINDOW);
    if (flags & SDL_RENDERER_SOFTWARE)
        debuglog(LCF_SDL | LCF_WINDOW, "  flag SDL_RENDERER_SOFTWARE");
    if (flags & SDL_RENDERER_ACCELERATED)
        debuglog(LCF_SDL | LCF_WINDOW, "  flag SDL_RENDERER_ACCELERATED");
    if (flags & SDL_RENDERER_PRESENTVSYNC)
        debuglog(LCF_SDL | LCF_WINDOW, "   flag SDL_RENDERER_PRESENTVSYNC");
    if (flags & SDL_RENDERER_TARGETTEXTURE)
        debuglog(LCF_SDL | LCF_WINDOW, "   flag SDL_RENDERER_TARGETTEXTURE");
    return orig::SDL_CreateRenderer(window, index, flags);
}

/* Override */ int SDL_CreateWindowAndRenderer(int width, int height,
        Uint32 window_flags, SDL_Window **window, SDL_Renderer **renderer)
{
    DEBUGLOGCALL(LCF_SDL | LCF_WINDOW);
    debuglog(LCF_SDL | LCF_WINDOW, "  size ", width, " x ", height);

    /* Disable fullscreen */
    window_flags &= 0xFFFFFFFF ^ SDL_WINDOW_FULLSCREEN_DESKTOP;

    /* Disable hidden windows */
    window_flags &= 0xFFFFFFFF ^ SDL_WINDOW_HIDDEN;

    /* Disable high DPI mode */
    window_flags &= 0xFFFFFFFF ^ SDL_WINDOW_ALLOW_HIGHDPI;

    //threadState.setNative(true);
    int ret = orig::SDL_CreateWindowAndRenderer(width, height, window_flags, window, renderer);
    //threadState.setNative(false);
    gameWindow = *window;

    /* A new window was created. It needs to be passed to the program */
    gw_sent = false;

#ifdef LIBTAS_ENABLE_AVDUMPING
    /* Initializing the video dump */
    if (tasflags.av_dumping) {

        debuglog(LCF_DUMP, "Start AV dumping on file ", av_filename);
        int av = openAVDumping(gameWindow, false, av_filename, frame_counter);
        if (av != 0) {
            /* Init failed, disable AV dumping */
            tasflags.av_dumping = 0;
        }
    }
#endif

    return ret;
}

/* Override */ void SDL_RenderPresent(SDL_Renderer * renderer)
{
    DEBUGLOGCALL(LCF_SDL | LCF_WINDOW);

    /* 
     * We need to pass the game window identifier to the program
     * so that it can capture inputs
     */
    if (sendXid() != 0)
        return;

    /* Start the frame boundary and pass the function to draw */
#ifdef LIBTAS_ENABLE_HUD
    static RenderHUD_SDL2 renderHUD;
    renderHUD.setRenderer(renderer);
    frameBoundary(true, [&] () {orig::SDL_RenderPresent(renderer);}, renderHUD);
#else
    frameBoundary(true, [&] () {orig::SDL_RenderPresent(renderer);});
#endif
}

/* Override */ void SDL_SetWindowSize(SDL_Window* window, int w, int h)
{
    DEBUGLOGCALL(LCF_SDL | LCF_WINDOW);
    debuglog(LCF_SDL | LCF_WINDOW, "    New size: ", w, " x ", h);

    orig::SDL_SetWindowSize(window, w, h);

    /* We need to close the dumping if needed, and open a new one */
#ifdef LIBTAS_ENABLE_AVDUMPING
    if (tasflags.av_dumping) {
        debuglog(LCF_SDL | LCF_WINDOW | LCF_DUMP, "    Dumping is restarted");
        closeAVDumping();
        int av = openAVDumping(gameWindow, video_opengl, av_filename, frame_counter);
        if (av != 0) {
            /* Init failed, disable AV dumping */
            tasflags.av_dumping = 0;
        }
    }
#endif
}

/* SDL 1.2 */
/* Override */ SDL1::SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags)
{
    debuglog(LCF_SDL | LCF_WINDOW, __func__, " call with size (", width, ",", height, "), bpp ", bpp, " and flags ", std::hex, flags, std::dec);

    /* Disable fullscreen */
    flags &= (0xFFFFFFFF ^ /*SDL_FULLSCREEN*/ 0x80000000);

    /* Call real function, but do not return yet */
    //threadState.setNative(true);
    SDL1::SDL_Surface *surf = orig::SDL_SetVideoMode(width, height, bpp, flags);
    //threadState.setNative(false);

#ifdef LIBTAS_ENABLE_AVDUMPING
    /* Initializing the video dump */
    if (flags & /*SDL_OPENGL*/ 0x00000002)
        video_opengl = true;
    else
        video_opengl = false;

    if (tasflags.av_dumping) {
        int av = openAVDumping(gameWindow, video_opengl, av_filename, frame_counter);
        if (av != 0) {
            /* Init failed, disable AV dumping */
            tasflags.av_dumping = 0;
        }
    }
#endif

    return surf;
}

/* Override */ int SDL_Flip(SDL1::SDL_Surface *screen)
{
    debuglog(LCF_SDL | LCF_FRAME | LCF_WINDOW, __func__, " call.");

    /* SDL 1.2 does only have one window,
     * thus it does not provide any access to window identifiers.
     * We need to pass a window id to linTAS so that it can capture inputs.
     * In our case, let's just pass a dummy value indicating that
     * we could not get access to it.
     * It will have to guess it, probably by getting the active window
     */
    if (!gw_sent) {
        Window w = 0;
        sendMessage(MSGB_WINDOW_ID);
        sendData(&w, sizeof(Window));
        gw_sent = 1;
        debuglog(LCF_SDL, "Send dummy X11 window id.");
    }

    /* Start the frame boundary and pass the function to draw */
#ifdef LIBTAS_ENABLE_HUD
    static RenderHUD_SDL1 renderHUD;
    frameBoundary(true, [&] () {orig::SDL_Flip(screen);}, renderHUD);
#else
    frameBoundary(true, [&] () {orig::SDL_Flip(screen);});
#endif

    return 0;
}

/* Override */ SDL_GrabMode SDL_WM_GrabInput(SDL_GrabMode mode)
{
    debuglog(LCF_SDL | LCF_KEYBOARD | LCF_MOUSE | LCF_WINDOW, __func__, " call with mode ", mode);
    static SDL_GrabMode fakeGrab = SDL_GRAB_OFF;
    if (mode != SDL_GRAB_QUERY)
        fakeGrab = mode;
    return fakeGrab;
}

void link_sdlwindows(void)
{
    if (SDLver == 1) {
        LINK_NAMESPACE_SDL1(SDL_GL_SwapBuffers);
        LINK_NAMESPACE_SDL1(SDL_SetVideoMode);
        LINK_NAMESPACE_SDL1(SDL_WM_SetCaption);
        LINK_NAMESPACE_SDL1(SDL_Flip);
    }
    if (SDLver == 2) {
        LINK_NAMESPACE_SDL2(SDL_GL_SwapWindow);
        LINK_NAMESPACE_SDL2(SDL_CreateWindow);
        LINK_NAMESPACE_SDL2(SDL_DestroyWindow);
        LINK_NAMESPACE_SDL2(SDL_GetWindowID);
        LINK_NAMESPACE_SDL2(SDL_GetWindowFlags);
        LINK_NAMESPACE_SDL2(SDL_GL_SetSwapInterval);
        LINK_NAMESPACE_SDL2(SDL_GetWindowWMInfo);
        LINK_NAMESPACE_SDL2(SDL_CreateRenderer);
        LINK_NAMESPACE_SDL2(SDL_CreateWindowAndRenderer);
        LINK_NAMESPACE_SDL2(SDL_RenderPresent);
        LINK_NAMESPACE_SDL2(SDL_SetWindowSize);
        LINK_NAMESPACE_SDL2(SDL_GL_CreateContext);
        LINK_NAMESPACE_SDL2(SDL_SetWindowTitle);
    }
}

namespace orig {
    static void (*glXSwapBuffers)( Display *dpy, XID drawable );
}

void glXSwapBuffers( Display *dpy, XID drawable )
{
    LINK_NAMESPACE(glXSwapBuffers, "libGL");
    debuglog(LCF_FRAME | LCF_WINDOW, __func__, " call.");

    if (!gw_sent) {
        sendMessage(MSGB_WINDOW_ID);
        sendData(&drawable, sizeof(Window));
        gw_sent = 1;
        debuglog(LCF_SDL, "Sent X11 window id: ", drawable);
    }

    /* Start the frame boundary and pass the function to draw */
#ifdef LIBTAS_ENABLE_HUD
    static RenderHUD_GL renderHUD;
    frameBoundary(true, [&] () {orig::glXSwapBuffers(dpy, drawable);}, renderHUD);
#else
    frameBoundary(true, [&] () {orig::glXSwapBuffers(dpy, drawable);});
#endif
}

