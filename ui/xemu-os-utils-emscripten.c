/*
 * OS-specific Helpers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "xemu-os-utils.h"
#include <emscripten.h>
#include <glib.h>

const char *xemu_get_os_info(void)
{
    static char *os_info = NULL;

    if (os_info == NULL) {
        const char *ua = emscripten_run_script_string(
            "typeof navigator !== 'undefined' ? navigator.userAgent : ''");
        os_info = g_strdup_printf("Web (%s)", (ua && *ua) ? ua : "Emscripten");
    }

    return os_info;
}
