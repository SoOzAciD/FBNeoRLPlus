/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2013 - Hans-Kristian Arntzen
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "menu_context.h"
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

static const menu_ctx_driver_t *menu_ctx_drivers[] = {
#if defined(HAVE_RMENU)
   &menu_ctx_rmenu,
#endif
#if defined(HAVE_RMENU_XUI)
   &menu_ctx_rmenu_xui,
#endif
#if defined(HAVE_RGUI)
   &menu_ctx_rgui,
#endif
   NULL // zero length array is not valid
};

const menu_ctx_driver_t *menu_ctx_find_driver(const char *ident)
{
   for (unsigned i = 0; menu_ctx_drivers[i]; i++)
   {
      if (strcmp(menu_ctx_drivers[i]->ident, ident) == 0)
         return menu_ctx_drivers[i];
   }

   return NULL;
}

bool menu_ctx_init_first(const menu_ctx_driver_t **driver, rgui_handle_t **handle)
{
   if (!menu_ctx_drivers[0])
      return false;

   for (unsigned i = 0; menu_ctx_drivers[i]; i++)
   {
      void *h = menu_ctx_drivers[i]->init();
      if (h)
      {
         *driver = menu_ctx_drivers[i];
         *handle = h;
         return true;
      }
   }

   return false;
}

