/*
  driver.h - An embedded CNC Controller with rs274/ngc (g-code) support

  Driver code for ESP32

  Part of Grbl

  Copyright (c) 2018 Terje Io
  Copyright (c) 2011-2015 Sungeun K. Jeon
  Copyright (c) 2009-2011 Simen Svale Skogsrud

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

#ifndef __DRIVER_H__
#define __DRIVER_H__

#include "soc/rtc.h"
#include "driver/gpio.h"
#include "driver/timer.h"
#include "driver/ledc.h"
#include "driver/rmt.h"
#include "driver/i2c.h"

#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "GRBL/grbl.h"

#include "keypad.h"

// Configuration
// Set value to 1 to enable, 0 to disable

#define CNC_BOOSTERPACK  0 // do not change!
#define PWM_RAMPED       0 // Ramped spindle PWM.
#define PROBE_ENABLE     1 // Probe input
#define PROBE_ISR        0 // Catch probe state change by interrupt TODO: needs verification!
#define KEYPAD_ENABLE    0 // I2C keypad for jogging etc. NOTE: not yet ready
#define WIFI_ENABLE      0 // Streaming over WiFi.
#define BLUETOOTH_ENABLE 0 // Streaming over Bluetooth.
#define SDCARD_ENABLE    0 // Run jobs from SD card.
#define IOEXPAND_ENABLE  0 // I2C IO expander for some output signals.
#define EEPROM_ENABLE    0 // I2C EEPROM (24LC16) support.

// End configuration

typedef struct {
	wifi_settings_t wifi;
	bluetooth_settings_t bluetooth;
	jog_config_t jog_config;
} driver_settings_t;

typedef struct {
	uint8_t action;
	void *params;
} i2c_task_t;

extern driver_settings_t driver_settings;

#if CNC_BOOSTERPACK

#define IOEXPAND 0

#if SDCARD_ENABLE

// Pin mapping when using SPI mode.
// With this mapping, SD card can be used both in SPI and 1-line SD mode.
// Note that a pull-up on CS line is required in SD mode.
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

#endif // SDCARD_ENABLE

// timer definitions
#define STEP_TIMER_GROUP TIMER_GROUP_0
#define STEP_TIMER_INDEX TIMER_0

// Define step pulse output pins.
#define X_STEP_PIN      GPIO_NUM_26
#define Y_STEP_PIN      GPIO_NUM_27
#define Z_STEP_PIN      GPIO_NUM_14
#define STEP_MASK     	(1ULL << X_STEP_PIN|1ULL << Y_STEP_PIN|1ULL << Z_STEP_PIN) // All step bits

// Define step direction output pins. NOTE: All direction pins must be on the same port.
#define X_DIRECTION_PIN     GPIO_NUM_2
#define Y_DIRECTION_PIN     GPIO_NUM_15
#define Z_DIRECTION_PIN     GPIO_NUM_12
#define DIRECTION_MASK    	(1ULL << X_DIRECTION_PIN|1ULL << Y_DIRECTION_PIN|1ULL << Z_DIRECTION_PIN) // All direction bits

// Define stepper driver enable/disable output pin(s).
#define STEPPERS_DISABLE_PIN    IOEXPAND
#define STEPPERS_DISABLE_MASK	(1ULL << STEPPERS_DISABLE_PIN)

// Define homing/hard limit switch input pins and limit interrupt vectors.
#define X_LIMIT_PIN     GPIO_NUM_4
#define Y_LIMIT_PIN     GPIO_NUM_16
#define Z_LIMIT_PIN     GPIO_NUM_32
#define LIMIT_MASK    	(1ULL << X_LIMIT_PIN|1ULL << Y_LIMIT_PIN|1ULL << Z_LIMIT_PIN) // All limit bits

// Define spindle enable and spindle direction output pins.
#define SPINDLE_ENABLE_PIN      IOEXPAND
#define SPINDLE_DIRECTION_PIN   IOEXPAND
#define SPINDLE_MASK 			(1ULL << SPINDLE_ENABLE_PIN|1ULL << SPINDLE_DIRECTION_PIN)
#define SPINDLEPWMPIN     		GPIO_NUM_17

// Define flood and mist coolant enable output pins.

#define COOLANT_FLOOD_PIN   IOEXPAND
#define COOLANT_MIST_PIN    IOEXPAND
#define COOLANT_MASK 		(1UL << COOLANT_FLOOD_PIN|1ULL << COOLANT_MIST_PIN)

// Define user-control CONTROLs (cycle start, reset, feed hold) input pins.
#define RESET_PIN           GPIO_NUM_35
#define FEED_HOLD_PIN       GPIO_NUM_39
#define CYCLE_START_PIN     GPIO_NUM_36
#define SAFETY_DOOR_PIN     GPIO_NUM_34
#define CONTROL_MASK      	(1UL << RESET_PIN|1UL << FEED_HOLD_PIN|1UL << CYCLE_START_PIN|1UL << SAFETY_DOOR_PIN)

// Define probe switch input pin.
#define PROBE_PIN       GPIO_NUM_13

#if KEYPAD_ENABLE
#define KEYPAD_STROBE_PIN	GPIO_NUM_33
#endif

#if IOEXPAND_ENABLE || KEYPAD_ENABLE || EEPROM_ENABLE
// Define I2C port/pins
#define I2C_PORT  I2C_NUM_1
#define I2C_SDA   GPIO_NUM_21
#define I2C_SCL   GPIO_NUM_22
#define I2C_CLOCK 100000
#endif

#if IOEXPAND_ENABLE
typedef union {
	uint8_t mask;
	struct {
		uint8_t stepper_enable_z :1,
				stepper_enable_y :1,
				mist_on          :1,
				flood_on         :1,
				reserved         :1,
			    spindle_dir      :1,
				stepper_enable_x :1,
				spindle_on		 :1;
	};
} ioexpand_t;
#endif

#else

#if SDCARD_ENABLE

// Pin mapping when using SPI mode.
// With this mapping, SD card can be used both in SPI and 1-line SD mode.
// Note that a pull-up on CS line is required in SD mode.
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

#endif // SDCARD_ENABLE

// timer definitions
#define STEP_TIMER_GROUP TIMER_GROUP_0
#define STEP_TIMER_INDEX TIMER_0

// Define step pulse output pins.
#define X_STEP_PIN      GPIO_NUM_12
#define Y_STEP_PIN      GPIO_NUM_14
#define Z_STEP_PIN      GPIO_NUM_27
#define STEP_MASK     	(1ULL << X_STEP_PIN|1ULL << Y_STEP_PIN|1ULL << Z_STEP_PIN) // All step bits

// Define step direction output pins. NOTE: All direction pins must be on the same port.
#define X_DIRECTION_PIN     GPIO_NUM_26
#define Y_DIRECTION_PIN     GPIO_NUM_25
#define Z_DIRECTION_PIN     GPIO_NUM_33
#define DIRECTION_MASK    	(1ULL << X_DIRECTION_PIN|1ULL << Y_DIRECTION_PIN|1ULL << Z_DIRECTION_PIN) // All direction bits

// Define stepper driver enable/disable output pin(s).
#define STEPPERS_DISABLE_PIN    GPIO_NUM_13
#define STEPPERS_DISABLE_MASK	(1ULL << STEPPERS_DISABLE_PIN)

// Define homing/hard limit switch input pins and limit interrupt vectors.
#define X_LIMIT_PIN     GPIO_NUM_2
#define Y_LIMIT_PIN     GPIO_NUM_4
#define Z_LIMIT_PIN     GPIO_NUM_15
#define LIMIT_MASK    	(1ULL << X_LIMIT_PIN|1ULL << Y_LIMIT_PIN|1ULL << Z_LIMIT_PIN) // All limit bits

// Define spindle enable and spindle direction output pins.
#define SPINDLE_ENABLE_PIN      GPIO_NUM_18
#define SPINDLE_DIRECTION_PIN   GPIO_NUM_5
#define SPINDLE_MASK 			(1ULL << SPINDLE_ENABLE_PIN|1ULL << SPINDLE_DIRECTION_PIN)
#define SPINDLEPWMPIN     		GPIO_NUM_17

// Define flood and mist coolant enable output pins.

#define COOLANT_FLOOD_PIN   GPIO_NUM_16
#define COOLANT_MIST_PIN    GPIO_NUM_21
#define COOLANT_MASK 		(1UL << COOLANT_FLOOD_PIN|1ULL << COOLANT_MIST_PIN)

// Define user-control CONTROLs (cycle start, reset, feed hold) input pins.
#define RESET_PIN           GPIO_NUM_34
#define FEED_HOLD_PIN       GPIO_NUM_36
#define CYCLE_START_PIN     GPIO_NUM_39
#define SAFETY_DOOR_PIN     GPIO_NUM_35
#define CONTROL_MASK      	(1UL << RESET_PIN|1UL << FEED_HOLD_PIN|1UL << CYCLE_START_PIN|1UL << SAFETY_DOOR_PIN)

// Define probe switch input pin.
#if PROBE_ENABLE
#define PROBE_PIN       GPIO_NUM_32
#else
#define PROBE_PIN       0xFF
#endif

#if KEYPAD_ENABLE
#error No free pins for keypad!
#endif

#if IOEXPAND_ENABLE || KEYPAD_ENABLE || EEPROM_ENABLE
// Define I2C port/pins
#define I2C_PORT  I2C_NUM_1
#define I2C_SDA   GPIO_NUM_21
#define I2C_SCL   GPIO_NUM_22
#define I2C_CLOCK 100000
#endif

#if IOEXPAND_ENABLE
typedef union {
	uint8_t mask;
	struct {
		uint8_t spindle_on       :1,
				spindle_dir      :1,
				mist_on          :1,
				flood_on         :1,
				stepper_enable_z :1,
				stepper_enable_x :1,
				stepper_enable_y :1,
				reserved		 :1;
	};
} ioexpand_t;
#endif

#endif

#ifdef I2C_PORT
extern QueueHandle_t i2cQueue;
extern SemaphoreHandle_t i2cBusy;
void i2c_init (void);
#endif

void selectStream (stream_setting_t stream);

#endif // __DRIVER_H__
