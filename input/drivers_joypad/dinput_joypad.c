/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
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

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <windowsx.h>

#include <dinput.h>
#include <mmsystem.h>

#include <boolean.h>
#include <compat/strl.h>
#include <string/stdstring.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../../tasks/tasks_internal.h"
#include "../input_keymaps.h"
#include "../../retroarch.h"
#include "../../verbosity.h"
#include "dinput_joypad.h"

/* TODO/FIXME - static globals */
static struct dinput_joypad_data g_pads[MAX_USERS];
static unsigned g_joypad_cnt;

/* forward declarations */
void dinput_destroy_context(void);
bool dinput_init_context(void);

extern LPDIRECTINPUT8 g_dinput_ctx;

#include "dinput_joypad_inl.h"

#ifdef HAVE_XINPUT
extern bool g_xinput_block_pads;
extern int g_xinput_pad_indexes[MAX_USERS];
static unsigned g_last_xinput_pad_idx;

static const GUID common_xinput_guids[] = {
   {MAKELONG(0x28DE, 0x11FF),0x0000,0x0000,{0x00,0x00,0x50,0x49,0x44,0x56,0x49,0x44}}, /* Valve streaming pad */
   {MAKELONG(0x045E, 0x02A1),0x0000,0x0000,{0x00,0x00,0x50,0x49,0x44,0x56,0x49,0x44}}, /* Wired 360 pad */
   {MAKELONG(0x045E, 0x028E),0x0000,0x0000,{0x00,0x00,0x50,0x49,0x44,0x56,0x49,0x44}}  /* wireless 360 pad */
};

bool dinput_joypad_get_vidpid_from_xinput_index(
      int32_t index, int32_t *vid,
      int32_t *pid, int32_t *dinput_index)
{
   int i;

   for (i = 0; i < ARRAY_SIZE(g_xinput_pad_indexes); i++)
   {
      /* Found XInput pad? */
      if (index == g_xinput_pad_indexes[i])
      {
         if (vid)
            *vid = g_pads[i].vid;

         if (pid)
            *pid = g_pads[i].pid;

         if (dinput_index)
            *dinput_index = i;

         return true;
      }
   }

   return false;
}

/* Based on SDL2's implementation. */
static bool guid_is_xinput_device(const GUID* product_guid)
{
   unsigned i, num_raw_devs     = 0;
   PRAWINPUTDEVICELIST raw_devs = NULL;

   /* Check for well known XInput device GUIDs,
    * thereby removing the need for the IG_ check.
    * This lets us skip RAWINPUT for popular devices.
    *
    * Also, we need to do this for the Valve Streaming Gamepad
    * because it's virtualized and doesn't show up in the device list.  */

   for (i = 0; i < ARRAY_SIZE(common_xinput_guids); ++i)
   {
      if (string_is_equal_fast(product_guid,
               &common_xinput_guids[i], sizeof(GUID)))
         return true;
   }

   /* Go through RAWINPUT (WinXP and later) to find HID devices. */
   if (!raw_devs)
   {
      if ((GetRawInputDeviceList(NULL, &num_raw_devs,
                  sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) || (!num_raw_devs))
         return false;

      raw_devs = (PRAWINPUTDEVICELIST)
         malloc(sizeof(RAWINPUTDEVICELIST) * num_raw_devs);
      if (!raw_devs)
         return false;

      if (GetRawInputDeviceList(raw_devs, &num_raw_devs,
               sizeof(RAWINPUTDEVICELIST)) == (UINT)-1)
      {
         free(raw_devs);
         raw_devs = NULL;
         return false;
      }
   }

   for (i = 0; i < num_raw_devs; i++)
   {
      RID_DEVICE_INFO rdi;
      char *dev_name  = NULL;
      UINT rdi_size   = sizeof(rdi);
      UINT name_size  = 0;

      rdi.cbSize      = rdi_size;

      /* 
       * Step 1 -
       * Check if device type is HID
       * Step 2 -
       * Query size of name
       * Step 3 -
       * Allocate string holding ID of device
       * Step 4 -
       * query ID of device
       * Step 5 -
       * Check if the device ID contains "IG_".
       * If it does, then it's an XInput device
       * This information can not be found from DirectInput 
       */
      if (
               (raw_devs[i].dwType == RIM_TYPEHID)                    /* 1 */
            && (GetRawInputDeviceInfoA(raw_devs[i].hDevice,
                RIDI_DEVICEINFO, &rdi, &rdi_size) != ((UINT)-1))
            && (MAKELONG(rdi.hid.dwVendorId, rdi.hid.dwProductId)
             == ((LONG)product_guid->Data1))
            && (GetRawInputDeviceInfoA(raw_devs[i].hDevice,
                RIDI_DEVICENAME, NULL, &name_size) != ((UINT)-1))     /* 2 */
            && ((dev_name = (char*)malloc(name_size)) != NULL)        /* 3 */
            && (GetRawInputDeviceInfoA(raw_devs[i].hDevice,
                RIDI_DEVICENAME, dev_name, &name_size) != ((UINT)-1)) /* 4 */
            && (strstr(dev_name, "IG_"))                              /* 5 */
         )
      {
         free(dev_name);
         free(raw_devs);
         raw_devs = NULL;
         return true;
      }

      if (dev_name)
         free(dev_name);
   }

   free(raw_devs);
   raw_devs = NULL;
   return false;
}
#endif

static BOOL CALLBACK enum_joypad_cb(const DIDEVICEINSTANCE *inst, void *p)
{
#ifdef HAVE_XINPUT
   bool is_xinput_pad;
#endif
   LPDIRECTINPUTDEVICE8 *pad = NULL;
   if (g_joypad_cnt == MAX_USERS)
      return DIENUM_STOP;

   pad = &g_pads[g_joypad_cnt].joypad;

#ifdef __cplusplus
   if (FAILED(IDirectInput8_CreateDevice(
               g_dinput_ctx, inst->guidInstance, pad, NULL)))
#else
   if (FAILED(IDirectInput8_CreateDevice(
               g_dinput_ctx, &inst->guidInstance, pad, NULL)))
#endif
      return DIENUM_CONTINUE;

   g_pads[g_joypad_cnt].joy_name          = strdup((const char*)inst->tszProductName);
   g_pads[g_joypad_cnt].joy_friendly_name = strdup((const char*)inst->tszInstanceName);

   /* there may be more useful info in the GUID,
    * so leave this here for a while */
#if 0
   printf("Guid = {%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}\n",
   inst->guidProduct.Data1,
   inst->guidProduct.Data2,
   inst->guidProduct.Data3,
   inst->guidProduct.Data4[0],
   inst->guidProduct.Data4[1],
   inst->guidProduct.Data4[2],
   inst->guidProduct.Data4[3],
   inst->guidProduct.Data4[4],
   inst->guidProduct.Data4[5],
   inst->guidProduct.Data4[6],
   inst->guidProduct.Data4[7]);
#endif

   g_pads[g_joypad_cnt].vid = inst->guidProduct.Data1 % 0x10000;
   g_pads[g_joypad_cnt].pid = inst->guidProduct.Data1 / 0x10000;

#ifdef HAVE_XINPUT
   is_xinput_pad = g_xinput_block_pads
      && guid_is_xinput_device(&inst->guidProduct);

   if (is_xinput_pad)
   {
      if (g_last_xinput_pad_idx < 4)
         g_xinput_pad_indexes[g_joypad_cnt] = g_last_xinput_pad_idx++;
      goto enum_iteration_done;
   }
#endif

   /* Set data format to simple joystick */
   IDirectInputDevice8_SetDataFormat(*pad, &c_dfDIJoystick2);
   IDirectInputDevice8_SetCooperativeLevel(*pad,
         (HWND)video_driver_window_get(),
         DISCL_EXCLUSIVE | DISCL_BACKGROUND);

   IDirectInputDevice8_EnumObjects(*pad, enum_axes_cb,
         *pad, DIDFT_ABSAXIS);

   dinput_create_rumble_effects(&g_pads[g_joypad_cnt]);

#ifdef HAVE_XINPUT
   if (!is_xinput_pad)
#endif
   {
      input_autoconfigure_connect(
            g_pads[g_joypad_cnt].joy_name,
            g_pads[g_joypad_cnt].joy_friendly_name,
            dinput_joypad.ident,
            g_joypad_cnt,
            g_pads[g_joypad_cnt].vid,
            g_pads[g_joypad_cnt].pid);
   }

#ifdef HAVE_XINPUT
enum_iteration_done:
#endif
   g_joypad_cnt++;
   return DIENUM_CONTINUE;
}

static bool dinput_joypad_init(void *data)
{
   unsigned i;

   if (!dinput_init_context())
      return false;

#ifdef HAVE_XINPUT
   g_last_xinput_pad_idx = 0;
#endif

   for (i = 0; i < MAX_USERS; ++i)
   {
#ifdef HAVE_XINPUT
      g_xinput_pad_indexes[i]     = -1;
#endif
      g_pads[i].joy_name          = NULL;
      g_pads[i].joy_friendly_name = NULL;
   }

   IDirectInput8_EnumDevices(g_dinput_ctx, DI8DEVCLASS_GAMECTRL,
         enum_joypad_cb, NULL, DIEDFL_ATTACHEDONLY);
   return true;
}

static void dinput_joypad_poll(void)
{
   unsigned i;
   for (i = 0; i < MAX_USERS; i++)
   {
      unsigned j;
      HRESULT ret;
      struct dinput_joypad_data *pad  = &g_pads[i];
#ifdef HAVE_XINPUT
      bool                    polled  = g_xinput_pad_indexes[i] < 0;
      if (!polled)
         continue;
#endif
      if (!pad || !pad->joypad)
         continue;

      pad->joy_state.lX               = 0;
      pad->joy_state.lY               = 0;
      pad->joy_state.lRx              = 0;
      pad->joy_state.lRy              = 0;
      pad->joy_state.lRz              = 0;
      pad->joy_state.rglSlider[0]     = 0;
      pad->joy_state.rglSlider[1]     = 0;
      pad->joy_state.rgdwPOV[0]       = 0;
      pad->joy_state.rgdwPOV[1]       = 0;
      pad->joy_state.rgdwPOV[2]       = 0;
      pad->joy_state.rgdwPOV[3]       = 0;
      for (j = 0; j < 128; j++)
         pad->joy_state.rgbButtons[j] = 0;

      pad->joy_state.lVX              = 0;
      pad->joy_state.lVY              = 0;
      pad->joy_state.lVZ              = 0;
      pad->joy_state.lVRx             = 0;
      pad->joy_state.lVRy             = 0;
      pad->joy_state.lVRz             = 0;
      pad->joy_state.rglVSlider[0]    = 0;
      pad->joy_state.rglVSlider[1]    = 0;
      pad->joy_state.lAX              = 0;
      pad->joy_state.lAY              = 0;
      pad->joy_state.lAZ              = 0;
      pad->joy_state.lARx             = 0;
      pad->joy_state.lARy             = 0;
      pad->joy_state.lARz             = 0;
      pad->joy_state.rglASlider[0]    = 0;
      pad->joy_state.rglASlider[1]    = 0;
      pad->joy_state.lFX              = 0;
      pad->joy_state.lFY              = 0;
      pad->joy_state.lFZ              = 0;
      pad->joy_state.lFRx             = 0;
      pad->joy_state.lFRy             = 0;
      pad->joy_state.lFRz             = 0;
      pad->joy_state.rglFSlider[0]    = 0;
      pad->joy_state.rglFSlider[1]    = 0;

      /* If this fails, something *really* bad must have happened. */
      if (FAILED(IDirectInputDevice8_Poll(pad->joypad)))
         if (
                  FAILED(IDirectInputDevice8_Acquire(pad->joypad))
               || FAILED(IDirectInputDevice8_Poll(pad->joypad))
            )
            continue;

      ret = IDirectInputDevice8_GetDeviceState(pad->joypad,
            sizeof(DIJOYSTATE2), &pad->joy_state);

      if (ret == DIERR_INPUTLOST || ret == DIERR_NOTACQUIRED)
         input_autoconfigure_disconnect(i, g_pads[i].joy_friendly_name);
   }
}

input_device_driver_t dinput_joypad = {
   dinput_joypad_init,
   dinput_joypad_query_pad,
   dinput_joypad_destroy,
   dinput_joypad_button,
   dinput_joypad_state,
   NULL,
   dinput_joypad_axis,
   dinput_joypad_poll,
   dinput_joypad_set_rumble,
   dinput_joypad_name,
   "dinput",
};
