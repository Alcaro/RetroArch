/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2013-2015 - pinumbernumber
 *  Copyright (C) 2011-2017 - Daniel De Matteis
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

/* Support 360 controllers on Windows.
 * Said controllers do show under DInput but they have limitations in this mode;
 * The triggers are combined rather than seperate and it is not possible to use
 * the guide button.
 *
 * Some wrappers for other controllers also simulate xinput (as it is easier to implement)
 * so this may be useful for those also.
 **/
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include <boolean.h>
#include <retro_inline.h>
#include <compat/strl.h>
#include <dynamic/dylib.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../../config.def.h"

#include "../../tasks/tasks_internal.h"
#include "../input_driver.h"

#include "../../verbosity.h"

#ifdef HAVE_DINPUT
#include "dinput_joypad.h"
#endif

#include "xinput_joypad.h"

#ifdef HAVE_DINPUT
/* Due to 360 pads showing up under both XInput and DirectInput,
 * and since we are going to have to pass through unhandled
 * joypad numbers to DirectInput, a slightly ugly
 * hack is required here. dinput_joypad_init will fill this.
 *
 * For each pad index, the appropriate entry will be set to -1 if it is not
 * a 360 pad, or the correct XInput user number (0..3 inclusive) if it is.
 */
extern int g_xinput_pad_indexes[MAX_USERS];
extern bool g_xinput_block_pads;
#endif

#ifdef HAVE_DYNAMIC
/* For xinput1_n.dll */
static dylib_t g_xinput_dll = NULL;
#endif

/* Function pointer, to be assigned with dylib_proc */
typedef uint32_t (__stdcall *XInputGetStateEx_t)(uint32_t, XINPUT_STATE*);
static XInputGetStateEx_t g_XInputGetStateEx;

typedef uint32_t (__stdcall *XInputSetState_t)(uint32_t, XINPUT_VIBRATION*);
static XInputSetState_t g_XInputSetState;

/* Guide button may or may not be available */
static bool g_xinput_guide_button_supported = false;
static unsigned g_xinput_num_buttons        = 0;

typedef struct
{
   XINPUT_STATE xstate;
   bool         connected;
} xinput_joypad_state;

/* TODO/FIXME - static globals */
static XINPUT_VIBRATION    g_xinput_rumble_states[4];
static xinput_joypad_state g_xinput_states[4];

/* Buttons are provided by XInput as bits of a uint16.
 * Map from rarch button index (0..10) to a mask to 
 * bitwise-& the buttons against.
 * dpad is handled seperately. */
static const uint16_t button_index_to_bitmap_code[] =  {
   XINPUT_GAMEPAD_A,
   XINPUT_GAMEPAD_B,
   XINPUT_GAMEPAD_X,
   XINPUT_GAMEPAD_Y,
   XINPUT_GAMEPAD_LEFT_SHOULDER,
   XINPUT_GAMEPAD_RIGHT_SHOULDER,
   XINPUT_GAMEPAD_START,
   XINPUT_GAMEPAD_BACK,
   XINPUT_GAMEPAD_LEFT_THUMB,
   XINPUT_GAMEPAD_RIGHT_THUMB
#ifndef _XBOX
   ,
   XINPUT_GAMEPAD_GUIDE
#endif
};

#include "xinput_joypad_inl.h"

static INLINE int pad_index_to_xuser_index(unsigned pad)
{
#ifdef HAVE_DINPUT
   return g_xinput_pad_indexes[pad];
#else
   return pad < DEFAULT_MAX_PADS 
      && g_xinput_states[pad].connected ? pad : -1;
#endif
}

static const char *xinput_joypad_name(unsigned pad)
{
#ifdef HAVE_DINPUT
   /* On platforms with dinput support, we are able
    * to get a name from the device itself */
   return dinput_joypad.name(pad);
#else
   /* Generic 'XInput' instead of 'Xbox 360', because
    * there are some other non-Xbox third party PC
    * controllers */
   static const char XBOX_CONTROLLER_NAME[] = "XInput Controller";
   if (pad_index_to_xuser_index(pad) < 0)
      return NULL;

   /* On platforms without dinput support, no
    * device-specific name is available
    * > Have to use generic names instead */
   return XBOX_CONTROLLER_NAME;
#endif
}

static bool xinput_joypad_init(void *data)
{
   unsigned i, j;
   XINPUT_STATE dummy_state;

#if defined(HAVE_DYNAMIC) && !defined(__WINRT__)
   if (!g_xinput_dll)
      if (!load_xinput_dll())
         goto error;

   /* If we get here then an xinput DLL is correctly loaded.
    * First try to load ordinal 100 (XInputGetStateEx).
    */
   g_XInputGetStateEx = (XInputGetStateEx_t)dylib_proc(
         g_xinput_dll, (const char*)100);
#elif defined(__WINRT__)
   /* XInputGetStateEx is not available on WinRT */
   g_XInputGetStateEx = NULL;
#else
   g_XInputGetStateEx = (XInputGetStateEx_t)XInputGetStateEx;
#endif
   g_xinput_guide_button_supported = true;

   if (!g_XInputGetStateEx)
   {
      /* no ordinal 100. (Presumably a wrapper.) Load the ordinary
       * XInputGetState, at the cost of losing guide button support.
       */
      g_xinput_guide_button_supported = false;
#if defined(HAVE_DYNAMIC) && !defined(__WINRT__)
      g_XInputGetStateEx = (XInputGetStateEx_t)dylib_proc(
            g_xinput_dll, "XInputGetState");
#else
	  g_XInputGetStateEx = (XInputGetStateEx_t)XInputGetState;
#endif

      if (!g_XInputGetStateEx)
      {
         RARCH_ERR("[XInput]: Failed to init: DLL is invalid or corrupt.\n");
#if defined(HAVE_DYNAMIC) && !defined(__WINRT__)
         dylib_close(g_xinput_dll);
#endif
         /* DLL was loaded but did not contain the correct function. */
         goto error;
      }
      RARCH_WARN("[XInput]: No guide button support.\n");
   }

#if defined(HAVE_DYNAMIC) && !defined(__WINRT__)
   g_XInputSetState = (XInputSetState_t)dylib_proc(
         g_xinput_dll, "XInputSetState");
#else
   g_XInputSetState = (XInputSetState_t)XInputSetState;
#endif
   if (!g_XInputSetState)
   {
      RARCH_ERR("[XInput]: Failed to init: DLL is invalid or corrupt.\n");
#if defined(HAVE_DYNAMIC) && !defined(__WINRT__)
      dylib_close(g_xinput_dll);
#endif
      goto error; /* DLL was loaded but did not contain the correct function. */
   }

   /* Zero out the states. */
   for (i = 0; i < 4; ++i)
   {
      g_xinput_states[i].xstate.dwPacketNumber        = 0;
      g_xinput_states[i].xstate.Gamepad.wButtons      = 0;
      g_xinput_states[i].xstate.Gamepad.bLeftTrigger  = 0;
      g_xinput_states[i].xstate.Gamepad.bRightTrigger = 0;
      g_xinput_states[i].xstate.Gamepad.sThumbLX      = 0;
      g_xinput_states[i].xstate.Gamepad.sThumbLY      = 0;
      g_xinput_states[i].xstate.Gamepad.sThumbRX      = 0;
      g_xinput_states[i].xstate.Gamepad.sThumbRY      = 0;
      g_xinput_states[i].connected                    = 
         !(g_XInputGetStateEx(i, &dummy_state) == ERROR_DEVICE_NOT_CONNECTED);
   }

   if (  (!g_xinput_states[0].connected) &&
         (!g_xinput_states[1].connected) &&
         (!g_xinput_states[2].connected) &&
         (!g_xinput_states[3].connected))
#ifdef __WINRT__
      goto succeeded;
#else
      goto error;
#endif

#ifdef HAVE_DINPUT
   g_xinput_block_pads = true;

   /* We're going to have to be buddies with dinput if we want to be able
    * to use XInput and non-XInput controllers together. */
   if (!dinput_joypad.init(data))
   {
      g_xinput_block_pads = false;
      goto error;
   }
#endif

   for (j = 0; j < MAX_USERS; j++)
   {
      const char *name = xinput_joypad_name(j);

      if (pad_index_to_xuser_index(j) > -1)
      {
         int32_t vid          = 0;
         int32_t pid          = 0;
#ifdef HAVE_DINPUT
         int32_t dinput_index = 0;
         bool success     = dinput_joypad_get_vidpid_from_xinput_index((int32_t)pad_index_to_xuser_index(j), (int32_t*)&vid, (int32_t*)&pid,
			 (int32_t*)&dinput_index);
         /* On success, found VID/PID from dinput index */
#endif

         input_autoconfigure_connect(
               name,
               NULL,
               xinput_joypad.ident,
               j,
               vid,
               pid);
      }
   }

#ifdef __WINRT__
succeeded:
#endif
   /* non-hat button. */
   g_xinput_num_buttons = g_xinput_guide_button_supported ? 11 : 10;
   return true;

error:
   /* non-hat button. */
   g_xinput_num_buttons = g_xinput_guide_button_supported ? 11 : 10;
   return false;
}

static bool xinput_joypad_query_pad(unsigned pad)
{
   int xuser = pad_index_to_xuser_index(pad);
   if (xuser > -1)
      return g_xinput_states[xuser].connected;
#ifdef HAVE_DINPUT
   return dinput_joypad.query_pad(pad);
#else
   return false;
#endif
}

static void xinput_joypad_destroy(void)
{
   unsigned i;

   for (i = 0; i < 4; ++i)
   {
      g_xinput_states[i].xstate.dwPacketNumber        = 0;
      g_xinput_states[i].xstate.Gamepad.wButtons      = 0;
      g_xinput_states[i].xstate.Gamepad.bLeftTrigger  = 0;
      g_xinput_states[i].xstate.Gamepad.bRightTrigger = 0;
      g_xinput_states[i].xstate.Gamepad.sThumbLX      = 0;
      g_xinput_states[i].xstate.Gamepad.sThumbLY      = 0;
      g_xinput_states[i].xstate.Gamepad.sThumbRX      = 0;
      g_xinput_states[i].xstate.Gamepad.sThumbRY      = 0;
      g_xinput_states[i].connected                    = false;
   }

#if defined(HAVE_DYNAMIC) && !defined(__WINRT__)
   dylib_close(g_xinput_dll);

   g_xinput_dll        = NULL;
#endif
   g_XInputGetStateEx  = NULL;
   g_XInputSetState    = NULL;

#ifdef HAVE_DINPUT
   dinput_joypad.destroy();

   g_xinput_block_pads = false;
#endif
}


static int16_t xinput_joypad_button(unsigned port, uint16_t joykey)
{
   int xuser         = pad_index_to_xuser_index(port);
   uint16_t btn_word = 0;
#ifdef HAVE_DINPUT
   if (xuser == -1)
      return dinput_joypad.button(port, joykey);
#endif
   if (!(g_xinput_states[xuser].connected))
      return 0;
   btn_word          = g_xinput_states[xuser].xstate.Gamepad.wButtons;
   return xinput_joypad_button_state(xuser, btn_word, port, joykey);
}

static int16_t xinput_joypad_axis(unsigned port, uint32_t joyaxis)
{
   int xuser           = pad_index_to_xuser_index(port);
   XINPUT_GAMEPAD *pad = &(g_xinput_states[xuser].xstate.Gamepad);
#ifdef HAVE_DINPUT
   if (xuser == -1)
      return dinput_joypad.axis(port, joyaxis);
#endif
   if (!(g_xinput_states[xuser].connected))
      return 0;
   return xinput_joypad_axis_state(pad, port, joyaxis);
}

static int16_t xinput_joypad_state_func(
      rarch_joypad_info_t *joypad_info,
      const struct retro_keybind *binds,
      unsigned port)
{
   unsigned i;
   uint16_t btn_word;
   int16_t ret         = 0;
   uint16_t port_idx   = joypad_info->joy_idx;
   int xuser           = pad_index_to_xuser_index(port_idx);
   XINPUT_GAMEPAD *pad = &(g_xinput_states[xuser].xstate.Gamepad);
#ifdef HAVE_DINPUT
   if (xuser == -1)
      return dinput_joypad.state(joypad_info, binds, port_idx);
#endif
   if (!(g_xinput_states[xuser].connected))
      return 0;
   btn_word            = g_xinput_states[xuser].xstate.Gamepad.wButtons;

   for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
   {
      /* Auto-binds are per joypad, not per user. */
      const uint64_t joykey  = (binds[i].joykey != NO_BTN)
         ? binds[i].joykey  : joypad_info->auto_binds[i].joykey;
      const uint32_t joyaxis = (binds[i].joyaxis != AXIS_NONE)
         ? binds[i].joyaxis : joypad_info->auto_binds[i].joyaxis;
      if (
               (uint16_t)joykey != NO_BTN 
            && xinput_joypad_button_state(
               xuser, btn_word, port_idx, (uint16_t)joykey))
         ret |= ( 1 << i);
      else if (joyaxis != AXIS_NONE &&
            ((float)abs(xinput_joypad_axis_state(pad, port_idx, joyaxis)) 
             / 0x8000) > joypad_info->axis_threshold)
         ret |= (1 << i);
   }

   return ret;
}

static void xinput_joypad_poll(void)
{
   unsigned i;

   for (i = 0; i < 4; ++i)
   {
      bool new_connected = g_XInputGetStateEx(i, &(g_xinput_states[i].xstate)) != ERROR_DEVICE_NOT_CONNECTED;
      if (new_connected != g_xinput_states[i].connected)
      {
#ifndef HAVE_DINPUT
         /* Normally, dinput handles device insertion/removal for us, but
          * since dinput is not available on UWP we have to do it ourselves */
         /* Also note that on UWP, the controllers are not available on startup
          * and are instead 'plugged in' a moment later because Microsoft reasons */
         /* TODO: This may be bad for performance? */
         if (new_connected)
         {
            /* This is kinda ugly, but it's the same thing that dinput does */
            xinput_joypad_destroy();
            xinput_joypad_init(NULL);
            return;
         }
#endif
         g_xinput_states[i].connected = new_connected;
         if (!g_xinput_states[i].connected)
            input_autoconfigure_disconnect(i, xinput_joypad_name(i));
      }
   }

#ifdef HAVE_DINPUT
   dinput_joypad.poll();
#endif
}

static bool xinput_joypad_rumble(unsigned pad,
      enum retro_rumble_effect effect, uint16_t strength)
{
   int xuser = pad_index_to_xuser_index(pad);

   if (xuser == -1)
   {
#ifdef HAVE_DINPUT
      if (dinput_joypad.set_rumble)
         return dinput_joypad.set_rumble(pad, effect, strength);
#endif
      return false;
   }

   /* Consider the low frequency (left) motor the "strong" one. */
   if (effect == RETRO_RUMBLE_STRONG)
      g_xinput_rumble_states[xuser].wLeftMotorSpeed = strength;
   else if (effect == RETRO_RUMBLE_WEAK)
      g_xinput_rumble_states[xuser].wRightMotorSpeed = strength;

   if (!g_XInputSetState)
      return false;

   return (g_XInputSetState(xuser, &g_xinput_rumble_states[xuser])
      == 0);
}

input_device_driver_t xinput_joypad = {
   xinput_joypad_init,
   xinput_joypad_query_pad,
   xinput_joypad_destroy,
   xinput_joypad_button,
   xinput_joypad_state_func,
   NULL,
   xinput_joypad_axis,
   xinput_joypad_poll,
   xinput_joypad_rumble,
   xinput_joypad_name,
   "xinput",
};
