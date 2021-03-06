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

#ifndef LIBTAS_XKEYBOARD_H_INCL
#define LIBTAS_XKEYBOARD_H_INCL

#include "../global.h"
#include <X11/X.h>
#include <X11/Xlib.h>

OVERRIDE int XQueryKeymap( Display*, char [32]);
OVERRIDE Bool XQueryPointer(
        Display*    /* display */,
        Window      /* w */,
        Window*     /* root_return */,
        Window*     /* child_return */,
        int*        /* root_x_return */,
        int*        /* root_y_return */,
        int*        /* win_x_return */,
        int*        /* win_y_return */,
        unsigned int*  /* mask_return */
        );

#endif

