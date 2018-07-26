/*
  state_machine.c - An embedded CNC Controller with rs274/ngc (g-code) support

  Main state machine

  Part of Grbl

  Copyright (c) 2018 Terje Io
  Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
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

#include "grbl.h"

static void state_idle (uint_fast16_t new_state);
static void state_cycle (uint_fast16_t rt_exec);
static void state_await_hold (uint_fast16_t rt_exec);
static void state_noop (uint_fast16_t rt_exec);
static void state_await_motion_cancel (uint_fast16_t rt_exec);
static void state_await_resume (uint_fast16_t rt_exec);
#ifdef PARKING_ENABLE
static void state_await_waypoint_retract (uint_fast16_t rt_exec);
static void state_restore (uint_fast16_t rt_exec);
static void state_await_resumed (uint_fast16_t rt_exec);
#endif

static void (* volatile stateHandler)(uint_fast16_t rt_exec) = state_idle;

static float restore_spindle_rpm;
static planner_cond_t restore_condition;
static uint_fast16_t pending_state = STATE_IDLE;

#ifdef PARKING_ENABLE

typedef struct {
    float target[N_AXIS];
    float restore_target[N_AXIS];
    float retract_waypoint;
    bool retracting;
    bool restart_retract;
    plan_line_data_t plan_data;
} parking_data_t;

// Declare and initialize parking local variables
static parking_data_t park;

#endif

static void state_restore_conditions (planner_cond_t *condition, float rpm)
{

#ifdef PARKING_ENABLE
    if(!park.restart_retract) {
#endif

    if (gc_state.modal.spindle.on) {
        if (settings.flags.laser_mode)
        // When in laser mode, ignore spindle spin-up delay. Set to turn on laser when cycle starts.
            sys.step_control.update_spindle_rpm = On;
        else if(spindle_set_state(condition->spindle, rpm)) {
            if(hal.driver_cap.spindle_at_speed) {
                while(!hal.spindle_get_state().at_speed)
                    delay_sec(0.1f, DelayMode_SysSuspend);
            } else
                delay_sec(SAFETY_DOOR_SPINDLE_DELAY, DelayMode_SysSuspend);
        }
    }

    // Block if safety door re-opened during prior restore actions.
    if (gc_state.modal.coolant.value) {
        // NOTE: Laser mode will honor this delay. An exhaust system is often controlled by this pin.
        coolant_set_state(condition->coolant);
        delay_sec(SAFETY_DOOR_COOLANT_DELAY, DelayMode_SysSuspend);
    }

#ifdef PARKING_ENABLE
    }
#endif

}

bool initiate_hold (uint_fast16_t new_state)
{

#ifdef PARKING_ENABLE
    memset(&park.plan_data, 0, sizeof(plan_line_data_t));
    park.retract_waypoint = PARKING_PULLOUT_INCREMENT;
    park.plan_data.condition.system_motion = On;
    park.plan_data.condition.no_feed_override = On;
    park.plan_data.line_number = PARKING_MOTION_LINE_NUMBER;
#endif

    plan_block_t *block = plan_get_current_block();

    if (block == NULL) {
        restore_condition.spindle = gc_state.modal.spindle;
        restore_condition.coolant.mask = gc_state.modal.coolant.mask | hal.coolant_get_state().mask;
        restore_spindle_rpm = gc_state.spindle.rpm;
    } else {
        restore_condition = block->condition;
        restore_spindle_rpm = block->spindle.rpm;
    }
   #ifdef DISABLE_LASER_DURING_HOLD
    if (settings.flags.laser_mode)
        enqueue_accessory_ovr(CMD_SPINDLE_OVR_STOP);
   #endif

    if(sys.state & (STATE_CYCLE|STATE_JOG)) {
        st_update_plan_block_parameters();  // Notify stepper module to recompute for hold deceleration.
        sys.step_control.execute_hold = On; // Initiate suspend state with active flag.
        stateHandler = state_await_hold;
    }

    if(new_state == STATE_HOLD)
        sys.holding_state = Hold_Pending;
    else
        sys.parking_state = Parking_Retracting;

    sys.suspend = true;
    pending_state = sys.state == STATE_JOG ? new_state : STATE_IDLE;

    return sys.state == STATE_CYCLE;
}

bool state_door_reopened (void)
{
#ifdef PARKING_ENABLE
    return park.restart_retract;
#else
    return false;
#endif
}

void update_state (uint_fast16_t rt_exec)
{
    if((rt_exec & EXEC_SAFETY_DOOR) && sys.state != STATE_SAFETY_DOOR)
        set_state(STATE_SAFETY_DOOR);
    else
        stateHandler(rt_exec);
}

void set_state (uint_fast16_t new_state)
{
    if(new_state != sys.state)
      switch(new_state) {    // Set up new state and handler

        case STATE_IDLE:
            sys.suspend = false;        // Break suspend state.
            sys.step_control.flags = 0; // Restore step control to normal operation.
            sys.parking_state = Parking_DoorClosed;
            sys.holding_state = Hold_NotHolding;
            sys.state = new_state;
            stateHandler = state_idle;
            break;

        case STATE_CYCLE:
            if(sys.state == STATE_IDLE) {
                // Start cycle only if queued motions exist in planner buffer and the motion is not canceled.
                plan_block_t *block;
                if ((block = plan_get_current_block())) {
                    sys.state = new_state;
                    sys.steppers_deenergize = false;    // Cancel stepper deenergize if pending.
                    st_prep_buffer();                   // Initialize step segment buffer before beginning cycle.
                    if(block->condition.spindle.synchronized) {

                        if(hal.spindle_reset_data)
                            hal.spindle_reset_data();

                        uint32_t index = hal.spindle_get_data(SpindleData_Counters).index_count + 2;

                        while(index != hal.spindle_get_data(SpindleData_Counters).index_count); // check for abort in this loop?

                    }
                    st_wake_up();
                    stateHandler = state_cycle;
                }
            }
            break;

        case STATE_JOG:
            sys.state = new_state;
            stateHandler = state_cycle;
            break;

        case STATE_HOLD:
            if(!((sys.state & STATE_JOG) || sys.override_ctrl.feed_hold_disable)) {
                if(!initiate_hold(new_state)) {
                    sys.holding_state = Hold_Complete;
                    stateHandler = state_await_resume;
                }
                sys.state = new_state;
            }
            break;

        case STATE_SAFETY_DOOR:
            if((sys.state & (STATE_ALARM|STATE_ESTOP|STATE_SLEEP|STATE_CHECK_MODE)))
                return;
            report_feedback_message(Message_SafetyDoorAjar);
            // no break
        case STATE_SLEEP:
            sys.parking_state = Parking_Retracting;
            if(!initiate_hold(new_state)) {
                if(pending_state != new_state) {
                    sys.state = new_state;
                    state_await_hold(EXEC_CYCLE_COMPLETE); // "Simulate" a cycle stop
                }
            } else
                sys.state = new_state;
            break;

        case STATE_ALARM:
        case STATE_ESTOP:
        case STATE_HOMING:
        case STATE_CHECK_MODE:
            sys.state = new_state;
            stateHandler = state_noop;
            break;
    }
}

static void state_idle (uint_fast16_t rt_exec)
{
    if((rt_exec & EXEC_CYCLE_START))
        set_state(STATE_CYCLE);

    if(rt_exec & EXEC_FEED_HOLD)
        set_state(STATE_HOLD);
}

static void state_cycle (uint_fast16_t rt_exec)
{
    if (rt_exec & EXEC_CYCLE_COMPLETE)
        set_state(STATE_IDLE);

    if (rt_exec & EXEC_MOTION_CANCEL) {
        st_update_plan_block_parameters();  // Notify stepper module to recompute for hold deceleration.
        sys.suspend = true;
        sys.step_control.execute_hold = On; // Initiate suspend state with active flag.
        stateHandler = state_await_motion_cancel;
    }

    if ((rt_exec & EXEC_FEED_HOLD))
        set_state(STATE_HOLD);
}

static void state_await_motion_cancel (uint_fast16_t rt_exec)
{
    if (rt_exec & EXEC_CYCLE_COMPLETE) {
        if(sys.state == STATE_JOG) {
            sys.step_control.flags = 0;
            plan_reset();
            st_reset();
            gc_sync_position();
            plan_sync_position();
        }
        set_state(pending_state);
    }
}

static void state_await_hold (uint_fast16_t rt_exec)
{
    if (rt_exec & EXEC_CYCLE_COMPLETE) {

        bool handler_changed = false;

        plan_cycle_reinitialize();
        sys.step_control.flags = 0;

        switch (sys.state) {

            case STATE_TOOL_CHANGE:
                spindle_stop(); // De-energize
                hal.coolant_set_state((coolant_state_t){0}); // De-energize
                break;

            // Resume door state when parking motion has retracted and door has been closed.
            case STATE_SLEEP:
            case STATE_SAFETY_DOOR:
                // Parking manager. Handles de/re-energizing, switch state checks, and parking motions for
                // the safety door and sleep states.

                // Handles retraction motions and de-energizing.
                // Ensure any prior spindle stop override is disabled at start of safety door routine.
                sys.spindle_stop_ovr.value = 0;

              #ifndef PARKING_ENABLE

                spindle_stop(); // De-energize
                hal.coolant_set_state((coolant_state_t){0}); // De-energize
                sys.parking_state = Parking_DoorAjar;

              #else
                // Get current position and store restore location and spindle retract waypoint.
                system_convert_array_steps_to_mpos(park.target, sys_position);
                if (!park.restart_retract) {
                    memcpy(park.restore_target, park.target, sizeof(park.target));
                    park.retract_waypoint += park.restore_target[PARKING_AXIS];
                    park.retract_waypoint = min(park.retract_waypoint, PARKING_TARGET);
                }

                // Execute slow pull-out parking retract motion. Parking requires homing enabled, the
                // current location not exceeding the parking target location, and laser mode disabled.
                // NOTE: State is will remain DOOR, until the de-energizing and retract is complete.
                if (settings.flags.homing_enable && (park.target[PARKING_AXIS] < PARKING_TARGET) && !settings.flags.laser_mode && !sys.override_ctrl.parking_disable) {
                    handler_changed = true;
                    stateHandler = state_await_waypoint_retract;
                    // Retract spindle by pullout distance. Ensure retraction motion moves away from
                    // the workpiece and waypoint motion doesn't exceed the parking target location.
                    if (park.target[PARKING_AXIS] < park.retract_waypoint) {
                        park.target[PARKING_AXIS] = park.retract_waypoint;
                        park.plan_data.feed_rate = PARKING_PULLOUT_RATE;
                        park.plan_data.condition.coolant = restore_condition.coolant; // Retain coolant state
                        park.plan_data.condition.spindle = restore_condition.spindle; // Retain spindle state
                        park.plan_data.spindle.rpm = restore_spindle_rpm;
                        if(!(park.retracting = mc_parking_motion(park.target, &park.plan_data)))
                            stateHandler(EXEC_CYCLE_COMPLETE);;
                    } else
                        stateHandler(EXEC_CYCLE_COMPLETE);
                } else {
                    // Parking motion not possible. Just disable the spindle and coolant.
                    // NOTE: Laser mode does not start a parking motion to ensure the laser stops immediately.
                    spindle_stop(); // De-energize
                    hal.coolant_set_state((coolant_state_t){0});     // De-energize
                    sys.parking_state = Parking_DoorAjar;
                }
#endif
                break;

            default:
                // Feed hold manager. Controls spindle stop override states.
                // NOTE: Hold ensured as completed by condition check at the beginning of suspend routine.
                // Handles beginning of spindle stop
                if (sys.spindle_stop_ovr.initiate) {
                    sys.spindle_stop_ovr.value = 0; // Clear stop override state
                    if (gc_state.modal.spindle.on) {
                        spindle_stop(); // De-energize
                        sys.spindle_stop_ovr.enabled = On; // Set stop override state to enabled, if de-energized.
                    }
                }
                break;

        }

        if(!handler_changed) {
            sys.holding_state = Hold_Complete;
            stateHandler = state_await_resume;
        }
    }
}

static void state_await_resume (uint_fast16_t rt_exec)
{
#ifdef PARKING_ENABLE
    if(rt_exec & EXEC_CYCLE_COMPLETE) {
        if(sys.step_control.execute_sys_motion) {
            sys.step_control.execute_sys_motion = Off;
            st_parking_restore_buffer(); // Restore step segment buffer to normal run state.
        }
        sys.parking_state = Parking_DoorAjar;
    }
#endif

    if ((rt_exec & EXEC_CYCLE_START) && !(sys.state == STATE_SAFETY_DOOR && hal.system_control_get_state().safety_door_ajar)) {

        bool handler_changed = false;

        if(sys.state == STATE_HOLD && !sys.spindle_stop_ovr.value)
            sys.spindle_stop_ovr.restore_cycle = On;

        switch (sys.state) {

            case STATE_TOOL_CHANGE:
                break;

            // Resume door state when parking motion has retracted and door has been closed.
            case STATE_SLEEP:
            case STATE_SAFETY_DOOR:
              #ifdef PARKING_ENABLE

                park.restart_retract = false;
                sys.parking_state = Parking_Resuming;

                // Execute fast restore motion to the pull-out position. Parking requires homing enabled.
                // NOTE: State is will remain DOOR, until the de-energizing and retract is complete.
                if (park.retracting) {
                    handler_changed = true;
                    stateHandler = state_restore;
                    // Check to ensure the motion doesn't move below pull-out position.
                    if (park.target[PARKING_AXIS] <= PARKING_TARGET) {
                        park.target[PARKING_AXIS] = park.retract_waypoint;
                        park.plan_data.feed_rate = PARKING_RATE;
                        if(!mc_parking_motion(park.target, &park.plan_data))
                            stateHandler(EXEC_CYCLE_COMPLETE);
                    } else // tell next handler to proceed with final step immediately
                        stateHandler(EXEC_CYCLE_COMPLETE);
                }
              #else
                // Delayed Tasks: Restart spindle and coolant, delay to power-up, then resume cycle.
                // Block if safety door re-opened during prior restore actions.
                state_restore_conditions(&restore_condition, restore_spindle_rpm);
              #endif
                break;

            default:
                // Feed hold manager. Controls spindle stop override states.
                // NOTE: Hold ensured as completed by condition check at the beginning of suspend routine.
                if (sys.spindle_stop_ovr.value) {
                    // Handles restoring of spindle state
                    if (sys.spindle_stop_ovr.restore || sys.spindle_stop_ovr.restore_cycle) {
                        if (gc_state.modal.spindle.on) {
                            report_feedback_message(Message_SpindleRestore);
                            if (settings.flags.laser_mode) // When in laser mode, ignore spindle spin-up delay. Set to turn on laser when cycle starts.
                                sys.step_control.update_spindle_rpm = On;
                            else
                                spindle_set_state(restore_condition.spindle, restore_spindle_rpm);
                        }
                        sys.spindle_stop_ovr.value = 0; // Clear stop override state
                    }
                } else if (sys.step_control.update_spindle_rpm) {
                    // Handles spindle state during hold. NOTE: Spindle speed overrides may be altered during hold state.
                    // NOTE: sys.step_control.update_spindle_rpm is automatically reset upon resume in step generator.
                    spindle_set_state(restore_condition.spindle, restore_spindle_rpm);
                    sys.step_control.update_spindle_rpm = Off;
                }
                break;
        }

        // Restart cycle if there is no further processing to take place
        if(!handler_changed) {
            set_state(STATE_IDLE);
            set_state(STATE_CYCLE);
        }
    }
}

#ifdef PARKING_ENABLE

static void state_await_waypoint_retract (uint_fast16_t rt_exec)
{
    if (rt_exec & EXEC_CYCLE_COMPLETE) {

        if(sys.step_control.execute_sys_motion) {
            sys.step_control.execute_sys_motion = Off;
            st_parking_restore_buffer(); // Restore step segment buffer to normal run state.
        }

        // NOTE: Clear accessory state after retract and after an aborted restore motion.
        park.plan_data.condition.coolant.value = 0;
        park.plan_data.condition.spindle.value = 0;
        park.plan_data.spindle.rpm = 0.0f;
        spindle_stop(); // De-energize
        hal.coolant_set_state((coolant_state_t){0}); // De-energize

        stateHandler = state_await_resume;

        // Execute fast parking retract motion to parking target location.
        if (park.target[PARKING_AXIS] < PARKING_TARGET) {
            park.target[PARKING_AXIS] = PARKING_TARGET;
            park.plan_data.feed_rate = PARKING_RATE;
            if(mc_parking_motion(park.target, &park.plan_data))
                park.retracting = true;
            else
                stateHandler(EXEC_CYCLE_COMPLETE);
        } else
            stateHandler(EXEC_CYCLE_COMPLETE);
    }
}

static void restart_retract (void)
{
    report_feedback_message(Message_SafetyDoorAjar);

    stateHandler = state_await_hold;

    park.restart_retract = true;
    sys.parking_state = Parking_Retracting;

    if (sys.step_control.execute_sys_motion) {
        st_update_plan_block_parameters(); // Notify stepper module to recompute for hold deceleration.
        sys.step_control.execute_hold = true;
        sys.step_control.execute_sys_motion = On;
    } else // else NO_MOTION is active.
        stateHandler(EXEC_CYCLE_COMPLETE);

}

static void state_restore (uint_fast16_t rt_exec)
{
    if (rt_exec & EXEC_SAFETY_DOOR)
        restart_retract();

    else if (rt_exec & EXEC_CYCLE_COMPLETE) {

        if(sys.step_control.execute_sys_motion) {
            sys.step_control.execute_sys_motion = Off;
            st_parking_restore_buffer(); // Restore step segment buffer to normal run state.
        }

        stateHandler = state_await_resumed;

        // Delayed Tasks: Restart spindle and coolant, delay to power-up, then resume cycle.
        // Block if safety door re-opened during prior restore actions.
        state_restore_conditions(&restore_condition, restore_spindle_rpm);

        // Execute slow plunge motion from pull-out position to resume position.

        // Regardless if the retract parking motion was a valid/safe motion or not, the
        // restore parking motion should logically be valid, either by returning to the
        // original position through valid machine space or by not moving at all.
        park.plan_data.feed_rate = PARKING_PULLOUT_RATE;
        park.plan_data.condition.coolant = restore_condition.coolant;
        park.plan_data.condition.spindle = restore_condition.spindle;
        park.plan_data.spindle.rpm = restore_spindle_rpm;
        if(!mc_parking_motion(park.restore_target, &park.plan_data))
            stateHandler(EXEC_CYCLE_COMPLETE); // No motion, proceed to next step
    }
}

static void state_await_resumed (uint_fast16_t rt_exec)
{
    if (rt_exec & EXEC_SAFETY_DOOR)
        restart_retract();

    else if (rt_exec & EXEC_CYCLE_COMPLETE) {
        if(sys.step_control.execute_sys_motion) {
            sys.step_control.execute_sys_motion = Off;
            st_parking_restore_buffer(); // Restore step segment buffer to normal run state.
        }
        set_state(STATE_IDLE);
        set_state(STATE_CYCLE);
    }
}

#endif

static void state_noop (uint_fast16_t rt_exec)
{
    // Do nothing - state change requests are handled elsewhere or ignored.
}