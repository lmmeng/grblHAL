/*
  keypad.c - An embedded CNC Controller with rs274/ngc (g-code) support

  Keypad driver code for Espressif ESP32 processor

  Part of Grbl

  Copyright (c) 2018 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "esp_log.h"

#include "driver.h"
#include "GRBL/grbl.h"

#if KEYPAD_ENABLE

#define KEYBUF_SIZE 16

static bool jogging = false, keyreleased = true;
static char keybuf_buf[16];
static jogmode_t jogMode = JogMode_Fast;
static volatile uint32_t keybuf_head = 0, keybuf_tail = 0;

void keypad_init (void)
{
	i2c_init();
}

bool keypad_setting (uint_fast16_t setting, float value, char *svalue)
{
    bool ok = false;

    switch(setting) {

        case Setting_JogStepSpeed:
        	driver_settings.jog_config.step_speed = value;
            ok = true;
            break;

        case Setting_JogSlowSpeed:
        	driver_settings.jog_config.slow_speed = value;
            ok = true;
            break;

        case Setting_JogFastSpeed:
        	driver_settings.jog_config.fast_speed = value;
            ok = true;
            break;

        case Setting_JogStepDistance:
        	driver_settings.jog_config.step_distance = value;
            ok = true;
            break;

        case Setting_JogSlowDistance:
        	driver_settings.jog_config.slow_distance = value;
            ok = true;
            break;

        case Setting_JogFastDistance:
        	driver_settings.jog_config.fast_distance = value;
            ok = true;
            break;
    }

    return ok;
}

void keypad_settings_restore (uint8_t restore_flag)
{
    if(restore_flag & SETTINGS_RESTORE_DRIVER_PARAMETERS) {
    	driver_settings.jog_config.step_speed    = 100.0f;
    	driver_settings.jog_config.slow_speed    = 600.0f;
    	driver_settings.jog_config.fast_speed    = 3000.0f;
    	driver_settings.jog_config.step_distance = 0.25f;
    	driver_settings.jog_config.slow_distance = 500.0f;
    	driver_settings.jog_config.fast_distance = 3000.0f;
    }
}

void keypad_settings_report (bool axis_settings, axis_setting_type_t setting_type, uint8_t axis_idx)
{
    if(!axis_settings) {
        report_float_setting(Setting_JogStepSpeed, driver_settings.jog_config.step_speed, 0);
        report_float_setting(Setting_JogSlowSpeed, driver_settings.jog_config.slow_speed, 0);
        report_float_setting(Setting_JogFastSpeed, driver_settings.jog_config.fast_speed, 0);
        report_float_setting(Setting_JogStepDistance, driver_settings.jog_config.step_distance, N_DECIMAL_SETTINGVALUE);
        report_float_setting(Setting_JogSlowDistance, driver_settings.jog_config.slow_distance, N_DECIMAL_SETTINGVALUE);
        report_float_setting(Setting_JogFastDistance, driver_settings.jog_config.fast_distance, N_DECIMAL_SETTINGVALUE);
    }
}

void keypad_enqueue_keycode (char c)
{
    uint32_t bptr = (keybuf_head + 1) & (KEYBUF_SIZE - 1);    // Get next head pointer

    if(bptr != keybuf_tail) {                       // If not buffer full
        keybuf_buf[keybuf_head] = c;              	// add data to buffer
        keybuf_head = bptr;                         // and update pointer
    }

//	printf("Keycode %c\n", c);
}

// Returns 0 if no keycode enqueued
static char keypad_get_keycode (void)
{
    uint32_t data = 0, bptr = keybuf_tail;

    if(bptr != keybuf_head) {
        data = keybuf_buf[bptr++];               // Get next character, increment tmp pointer
        keybuf_tail = bptr & (KEYBUF_SIZE - 1);  // and update pointer
    }

    return data;
}


// BE WARNED: this function may be dangerous to use...
static char *strrepl (char *str, int c, char *str3)
{
	char tmp[30];
	char *s = strrchr(str, c);

	while(s) {
		strcpy(tmp, str3);
		strcat(tmp, s + 1);
		strcpy(s, tmp);
		s = strrchr(str, c);
	}

	return str;
}

void keypad_process_keypress (uint_fast16_t state)
{
	bool addedGcode, jogCommand = false;
	char command[30] = "", keycode = keypad_get_keycode();

    if(keycode)
      switch(keycode) {

        case 'M':                                   // Mist override
            enqueue_accessory_override(CMD_OVERRIDE_COOLANT_MIST_TOGGLE);
            break;

        case 'C':                                   // Coolant override
            enqueue_accessory_override(CMD_OVERRIDE_COOLANT_FLOOD_TOGGLE);
            break;

        case CMD_FEED_HOLD:                         // Feed hold
        case CMD_CYCLE_START:                       // Cycle start
            protocol_process_realtime(keycode);
            break;

        case '0':
        case '1':
        case '2':                                   // Set jog mode
            jogMode = (jogmode_t)(keycode - '0');
            break;

        case 'h':                                   // "toggle" jog mode
            jogMode = jogMode == JogMode_Step ? JogMode_Fast : (jogMode == JogMode_Fast ? JogMode_Slow : JogMode_Step);
            break;

        case 'H':                                   // Home axes
            strcpy(command, "$H");
            break;

        case JOG_XR:                                // Jog X
            strcpy(command, "$J=G91X?F");
            break;

        case JOG_XL:                                // Jog -X
            strcpy(command, "$J=G91X-?F");
            break;

        case JOG_YF:                                // Jog Y
            strcpy(command, "$J=G91Y?F");
            break;

        case JOG_YB:                                // Jog -Y
            strcpy(command, "$J=G91Y-?F");
            break;

        case JOG_ZU:                                // Jog Z
            strcpy(command, "$J=G91Z?F");
            break;

        case JOG_ZD:                                // Jog -Z
            strcpy(command, "$J=G91Z-?F");
            break;

        case JOG_XRYF:                              // Jog XY
            strcpy(command, "$J=G91X?Y?F");
            break;

        case JOG_XRYB:                              // Jog X-Y
            strcpy(command, "$J=G91X?Y-?F");
            break;

        case JOG_XLYF:                              // Jog -XY
            strcpy(command, "$J=G91X-?Y?F");
            break;

        case JOG_XLYB:                              // Jog -X-Y
            strcpy(command, "$J=G91X-?Y-?F");
            break;

        case JOG_XRZU:                              // Jog XZ
            strcpy(command, "$J=G91X?Z?F");
            break;

        case JOG_XRZD:                              // Jog X-Z
            strcpy(command, "$J=G91X?Z-?F");
            break;

        case JOG_XLZU:                              // Jog -XZ
            strcpy(command, "$J=G91X-?Z?F");
            break;

        case JOG_XLZD:                              // Jog -X-Z
            strcpy(command, "$J=G91X-?Z-?F");
            break;
    }

	if(command[0] != '\0') {

		// add distance and speed to jog commands
		if((jogCommand = (command[0] == '$' && command[1] == 'J')))
			switch(jogMode) {

			case JogMode_Slow:
				strrepl(command, '?', ftoa(driver_settings.jog_config.slow_distance, 0));
				strcat(command, ftoa(driver_settings.jog_config.slow_speed, 0));
				break;

			case JogMode_Step:
				strrepl(command, '?', ftoa(driver_settings.jog_config.step_distance, 3));
				strcat(command, ftoa(driver_settings.jog_config.step_speed, 0));
				break;

			default:
				strrepl(command, '?', ftoa(driver_settings.jog_config.fast_distance, 0));
				strcat(command, ftoa(driver_settings.jog_config.fast_speed, 0));
				break;

		}

		if(!(jogCommand && keyreleased)) { // key still pressed? - do not execute jog command if released!
			addedGcode = hal.protocol_enqueue_gcode((char *)command);
			jogging = jogging || (jogCommand && addedGcode);
		}
	}
}

void keypad_keyclick_handler (bool keydown)
{
	const i2c_task_t i2c_task = {
		.action = 1,
		.params = NULL
	};

	keyreleased = !keydown;

	if(keydown) {
	    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		xQueueSendFromISR(i2cQueue, (void *)&i2c_task, &xHigherPriorityTaskWoken);
	} else if(jogging) {
		jogging = false;
		hal.protocol_process_realtime(CMD_JOG_CANCEL);
		keybuf_tail = keybuf_head = 0; // flush keycode buffer TODO: queue as well?
	}
}

#endif

