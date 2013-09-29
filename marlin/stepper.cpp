/*
   stepper.c - stepper motor driver: executes motion plans using stepper motors
   Part of Grbl

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

/* The timer calculations of this module informed by the 'RepRap cartesian firmware' by Zack Smith
   and Philipp Tiefenbacher. */

#include "Marlin.h"
#include "stepper.h"
#include "planner.h"
#include "temperature.h"
#include "language.h"
#include "mbed.h"
#include "speed_lookuptable.h"
#if defined(DIGIPOTSS_PIN) && DIGIPOTSS_PIN > -1
#include <SPI.h>
#endif

Ticker stepper_timer;
void stepper_int_handler();
//===========================================================================
//=============================public variables  ============================
//===========================================================================
block_t *current_block;  // A pointer to the block currently being traced


//===========================================================================
//=============================private variables ============================
//===========================================================================
//static makes it inpossible to be called from outside of this file by extern.!

// Variables used by The Stepper Driver Interrupt
static unsigned char out_bits;        // The next stepping-bits to be output
static long counter_x,       // Counter variables for the bresenham line tracer
	    counter_y,
	    counter_z,
	    counter_e;
volatile static unsigned long step_events_completed; // The number of step events executed in the current block
#ifdef ADVANCE
static long advance_rate, advance, final_advance = 0;
static long old_advance = 0;
static long e_steps[3];
#endif
static long acceleration_time, deceleration_time;
//static unsigned long accelerate_until, decelerate_after, acceleration_rate, initial_rate, final_rate, nominal_rate;
static unsigned short acc_step_rate; // needed for deccelaration start point
static char step_loops;
static unsigned short OCR1A_nominal;
static unsigned short step_loops_nominal;

volatile long endstops_trigsteps[3]={0,0,0};
volatile long endstops_stepsTotal,endstops_stepsDone;
static volatile bool endstop_x_hit=false;
static volatile bool endstop_y_hit=false;
static volatile bool endstop_z_hit=false;
#ifdef ABORT_ON_ENDSTOP_HIT_FEATURE_ENABLED
bool abort_on_endstop_hit = false;
#endif

#if defined(X_MIN_PIN) && X_MIN_PIN > -1
static bool old_x_min_endstop=false;
static bool old_x_max_endstop=false;
static bool old_y_min_endstop=false;
static bool old_y_max_endstop=false;
static bool old_z_min_endstop=false;
static bool old_z_max_endstop=false;
#endif

static bool check_endstops = true;

volatile long count_position[NUM_AXIS] = { 0, 0, 0, 0};
volatile signed char count_direction[NUM_AXIS] = { 1, 1, 1, 1};
volatile int do_int = 0;

//===========================================================================
//=============================functions         ============================
//===========================================================================
// intRes = intIn1 * intIn2 >> 16
#define MultiU16X8toH16(intRes, charIn1, intIn2)  intRes = charIn1 * (intIn2 >> 16);

// intRes = longIn1 * longIn2 >> 24
#define MultiU24X24toH16(intRes, longIn1, longIn2) intRes = longIn1 * (longIn2 >> 24);

// Some useful constants
#define ENABLE_STEPPER_DRIVER_INTERRUPT() do_int = 1;
#define DISABLE_STEPPER_DRIVER_INTERRUPT() do_int = 0;


void checkHitEndstops()
{
	if( endstop_x_hit || endstop_y_hit || endstop_z_hit) {
		SERIAL_ECHO_START;
		SERIAL_ECHOPGM(MSG_ENDSTOPS_HIT);
		if(endstop_x_hit) {
			SERIAL_ECHOPAIR(" X:",(float)endstops_trigsteps[X_AXIS]/axis_steps_per_unit[X_AXIS]);
			LCD_MESSAGEPGM(MSG_ENDSTOPS_HIT "X");
		}
		if(endstop_y_hit) {
			SERIAL_ECHOPAIR(" Y:",(float)endstops_trigsteps[Y_AXIS]/axis_steps_per_unit[Y_AXIS]);
			LCD_MESSAGEPGM(MSG_ENDSTOPS_HIT "Y");
		}
		if(endstop_z_hit) {
			SERIAL_ECHOPAIR(" Z:",(float)endstops_trigsteps[Z_AXIS]/axis_steps_per_unit[Z_AXIS]);
			LCD_MESSAGEPGM(MSG_ENDSTOPS_HIT "Z");
		}
		SERIAL_ECHOLN("");
		endstop_x_hit=false;
		endstop_y_hit=false;
		endstop_z_hit=false;
#ifdef ABORT_ON_ENDSTOP_HIT_FEATURE_ENABLED
		if (abort_on_endstop_hit)
		{
			quickStop();
			setTargetHotend0(0);
			setTargetHotend1(0);
			setTargetHotend2(0);
		}
#endif
	}
}

void endstops_hit_on_purpose()
{
	endstop_x_hit=false;
	endstop_y_hit=false;
	endstop_z_hit=false;
}

void enable_endstops(bool check)
{
	check_endstops = check;
}

//         __________________________
//        /|                        |\     _________________         ^
//       / |                        | \   /|               |\        |
//      /  |                        |  \ / |               | \       s
//     /   |                        |   |  |               |  \      p
//    /    |                        |   |  |               |   \     e
//   +-----+------------------------+---+--+---------------+----+    e
//   |               BLOCK 1            |      BLOCK 2          |    d
//
//                           time ----->
//
//  The trapezoid is the shape the speed curve over time. It starts at block->initial_rate, accelerates
//  first block->accelerate_until step_events_completed, then keeps going at constant speed until
//  step_events_completed reaches block->decelerate_after after which it decelerates until the trapezoid generator is reset.
//  The slope of acceleration is calculated with the leib ramp alghorithm.

void st_wake_up() {
	//  TCNT1 = 0;
	ENABLE_STEPPER_DRIVER_INTERRUPT();
}

unsigned int calc_timer(unsigned int step_rate) {
	unsigned int timer;
	if(step_rate > MAX_STEP_FREQUENCY) step_rate = MAX_STEP_FREQUENCY;
	if(step_rate > 20000) { // If steprate > 20kHz >> step 4 times
		step_rate = (step_rate >> 2)&0x3fff;
		step_loops = 4;
	}
	else if(step_rate > 10000) { // If steprate > 10kHz >> step 2 times
		step_rate = (step_rate >> 1)&0x7fff;
		step_loops = 2;
	}
	else {
		step_loops = 1;
	}
	//if(step_rate < (F_CPU/500000)) step_rate = (F_CPU/500000);
	//step_rate -= (F_CPU/500000); // Correct for minimal speed
	if(step_rate >= (8*256)){ // higher step rate
		unsigned short * table_address = (unsigned short *) &speed_lookuptable_fast[(unsigned char)(step_rate>>8)][0];
		unsigned char tmp_step_rate = (step_rate & 0x00ff);
		unsigned short gain = (unsigned short)(table_address[2]);
		MultiU16X8toH16(timer, tmp_step_rate, gain);
		timer = (unsigned short)(table_address[0]) - timer;
	}
	else { // lower step rates
		unsigned short *table_address = (unsigned short*)&speed_lookuptable_slow[0][0];
		table_address += ((step_rate)>>1) & 0xfffc;
		timer = (unsigned short)(table_address[0]);
		timer -= (((unsigned short)(table_address[2]) * (unsigned char)(step_rate & 0x0007))>>3);
	}
	if(timer < 100) { timer = 100; MYSERIAL.print(MSG_STEPPER_TOO_HIGH); MYSERIAL.println(step_rate); }//(20kHz this should never happen)
	return timer;
}

// Initializes the trapezoid generator from the current block. Called whenever a new
// block begins.
void trapezoid_generator_reset() {
#ifdef ADVANCE
	advance = current_block->initial_advance;
	final_advance = current_block->final_advance;
	// Do E steps + advance steps
	e_steps[current_block->active_extruder] += ((advance >>8) - old_advance);
	old_advance = advance >>8;
#endif
	deceleration_time = 0;
	// step_rate to timer interval
	OCR1A_nominal = calc_timer(current_block->nominal_rate);
	// make a note of the number of step loops required at nominal speed
	step_loops_nominal = step_loops;
	acc_step_rate = current_block->initial_rate;
	acceleration_time = calc_timer(acc_step_rate);
	stepper_timer.detach();
	stepper_timer.attach_us(stepper_int_handler, acceleration_time);

	//    SERIAL_ECHO_START;
	//    SERIAL_ECHOPGM("advance :");
	//    SERIAL_ECHO(current_block->advance/256.0);
	//    SERIAL_ECHOPGM("advance rate :");
	//    SERIAL_ECHO(current_block->advance_rate/256.0);
	//    SERIAL_ECHOPGM("initial advance :");
	//    SERIAL_ECHO(current_block->initial_advance/256.0);
	//    SERIAL_ECHOPGM("final advance :");
	//    SERIAL_ECHOLN(current_block->final_advance/256.0);

}

void stepper_int_handler()
{
	if(!do_int)
		return;

	// If there is no current block, attempt to pop one from the buffer
	if (current_block == NULL) {
		// Anything in the buffer?
		current_block = plan_get_current_block();
		if (current_block != NULL) {
			current_block->busy = true;
			trapezoid_generator_reset();
			counter_x = -(current_block->step_event_count >> 1);
			counter_y = counter_x;
			counter_z = counter_x;
			counter_e = counter_x;
			step_events_completed = 0;

#ifdef Z_LATE_ENABLE
			if(current_block->steps_z > 0) {
				enable_z();
				stepper_timer.detach();
				stepper_timer.attach_us(stepper_int_handler, 1000); //LPC_TIM1->MR0 = 2000; //1ms wait
				return;
			}
#endif

			//      #ifdef ADVANCE
			//      e_steps[current_block->active_extruder] = 0;
			//      #endif
		}
		else {
			stepper_timer.detach();
			stepper_timer.attach_us(stepper_int_handler, 1000); //LPC_TIM1->MR0 = 2000; //1ms wait
		}
	}

	if (current_block != NULL) {
		// Set directions TO DO This should be done once during init of trapezoid. Endstops -> interrupt
		out_bits = current_block->direction_bits;

		// Set the direction bits (X_AXIS=A_AXIS and Y_AXIS=B_AXIS for COREXY)
		if((out_bits & (1<<X_AXIS))!=0){
#ifdef DUAL_X_CARRIAGE
			if (extruder_duplication_enabled){
				p_x_dir = INVERT_X_DIR;//WRITE(X_DIR_PIN, INVERT_X_DIR);
				p_x2_dir = INVERT_X_DIR; //WRITE(X2_DIR_PIN, INVERT_X_DIR);
			}
			else{
				if (current_block->active_extruder != 0)
					p_x2_dir = INVERT_X_DIR; //(X2_DIR_PIN, INVERT_X_DIR);
				else
					p_x_dir = INVERT_X_DIR; //WRITE(X_DIR_PIN, INVERT_X_DIR);
			}
#else
			p_x_dir = INVERT_X_DIR; //WRITE(X_DIR_PIN, INVERT_X_DIR);
#endif
			count_direction[X_AXIS]=-1;
		}
		else{
#ifdef DUAL_X_CARRIAGE
			if (extruder_duplication_enabled){
				p_x_dir = !INVERT_X_DIR; //WRITE(X_DIR_PIN, !INVERT_X_DIR);
				p_x2_dir = !INVERT_X_DIR;//WRITE(X2_DIR_PIN, !INVERT_X_DIR);
			}
			else{
				if (current_block->active_extruder != 0)
					p_x2_dir = !INVERT_X_DIR;//WRITE(X2_DIR_PIN, !INVERT_X_DIR);
				else
					p_x_dir = !INVERT_X_DIR; //WRITE(X_DIR_PIN, !INVERT_X_DIR);
			}
#else
			p_x_dir = !INVERT_X_DIR; //WRITE(X_DIR_PIN, !INVERT_X_DIR);
#endif
			count_direction[X_AXIS]=1;
		}
		if((out_bits & (1<<Y_AXIS))!=0){
			p_y_dir = INVERT_Y_DIR;//WRITE(Y_DIR_PIN, INVERT_Y_DIR);
			count_direction[Y_AXIS]=-1;
		}
		else{
			p_y_dir = !INVERT_Y_DIR;//WRITE(Y_DIR_PIN, INVERT_Y_DIR);
			count_direction[Y_AXIS]=1;
		}

		// Set direction en check limit switches
#ifndef COREXY
		if ((out_bits & (1<<X_AXIS)) != 0) {   // stepping along -X axis
#else
			if ((((out_bits & (1<<X_AXIS)) != 0)&&(out_bits & (1<<Y_AXIS)) != 0)) {   //-X occurs for -A and -B
#endif
				if(check_endstops)
				{
#ifdef DUAL_X_CARRIAGE
					// with 2 x-carriages, endstops are only checked in the homing direction for the active extruder
					if ((current_block->active_extruder == 0 && X_HOME_DIR == -1)
							|| (current_block->active_extruder != 0 && X2_HOME_DIR == -1))
#endif
					{
#if defined(X_MIN_PIN) && X_MIN_PIN > -1
						bool x_min_endstop=(READ(X_MIN_PIN) != X_MIN_ENDSTOP_INVERTING);
						if(x_min_endstop && old_x_min_endstop && (current_block->steps_x > 0)) {
							endstops_trigsteps[X_AXIS] = count_position[X_AXIS];
							endstop_x_hit=true;
							step_events_completed = current_block->step_event_count;
						}
						old_x_min_endstop = x_min_endstop;
#endif
					}
				}
			}
			else { // +direction
				if(check_endstops)
				{
#ifdef DUAL_X_CARRIAGE
					// with 2 x-carriages, endstops are only checked in the homing direction for the active extruder
					if ((current_block->active_extruder == 0 && X_HOME_DIR == 1)
							|| (current_block->active_extruder != 0 && X2_HOME_DIR == 1))
#endif
					{
#if defined(X_MAX_PIN) && X_MAX_PIN > -1
						bool x_max_endstop=(READ(X_MAX_PIN) != X_MAX_ENDSTOP_INVERTING);
						if(x_max_endstop && old_x_max_endstop && (current_block->steps_x > 0)){
							endstops_trigsteps[X_AXIS] = count_position[X_AXIS];
							endstop_x_hit=true;
							step_events_completed = current_block->step_event_count;
						}
						old_x_max_endstop = x_max_endstop;
#endif
					}
				}
			}

#ifndef COREXY
			if ((out_bits & (1<<Y_AXIS)) != 0) {   // -direction
#else
				if ((((out_bits & (1<<X_AXIS)) != 0)&&(out_bits & (1<<Y_AXIS)) == 0)) {   // -Y occurs for -A and +B
#endif
					if(check_endstops)
					{
#if defined(Y_MIN_PIN) && Y_MIN_PIN > -1
						bool y_min_endstop=(READ(Y_MIN_PIN) != Y_MIN_ENDSTOP_INVERTING);
						if(y_min_endstop && old_y_min_endstop && (current_block->steps_y > 0)) {
							endstops_trigsteps[Y_AXIS] = count_position[Y_AXIS];
							endstop_y_hit=true;
							step_events_completed = current_block->step_event_count;
						}
						old_y_min_endstop = y_min_endstop;
#endif
					}
				}
				else { // +direction
					if(check_endstops)
					{
#if defined(Y_MAX_PIN) && Y_MAX_PIN > -1
						bool y_max_endstop=(READ(Y_MAX_PIN) != Y_MAX_ENDSTOP_INVERTING);
						if(y_max_endstop && old_y_max_endstop && (current_block->steps_y > 0)){
							endstops_trigsteps[Y_AXIS] = count_position[Y_AXIS];
							endstop_y_hit=true;
							step_events_completed = current_block->step_event_count;
						}
						old_y_max_endstop = y_max_endstop;
#endif
					}
				}

				if ((out_bits & (1<<Z_AXIS)) != 0) {   // -direction
					p_z_dir = INVERT_Z_DIR; //WRITE(Z_DIR_PIN,INVERT_Z_DIR);

#ifdef Z_DUAL_STEPPER_DRIVERS
					p_z2_dir = INVERT_Z_DIR; //WRITE(Z2_DIR_PIN,INVERT_Z_DIR);
#endif

					count_direction[Z_AXIS]=-1;
					if(check_endstops)
					{
#if defined(Z_MIN_PIN) && Z_MIN_PIN > -1
						bool z_min_endstop=(READ(Z_MIN_PIN) != Z_MIN_ENDSTOP_INVERTING);
						if(z_min_endstop && old_z_min_endstop && (current_block->steps_z > 0)) {
							endstops_trigsteps[Z_AXIS] = count_position[Z_AXIS];
							endstop_z_hit=true;
							step_events_completed = current_block->step_event_count;
						}
						old_z_min_endstop = z_min_endstop;
#endif
					}
				}
				else { // +direction
					p_z_dir = !INVERT_Z_DIR; //WRITE(Z_DIR_PIN,!INVERT_Z_DIR);

#ifdef Z_DUAL_STEPPER_DRIVERS
					p_z2_dir = !INVERT_Z_DIR; //WRITE(Z2_DIR_PIN,!INVERT_Z_DIR);
#endif

					count_direction[Z_AXIS]=1;
					if(check_endstops)
					{
#if defined(Z_MAX_PIN) && Z_MAX_PIN > -1
						bool z_max_endstop=(READ(Z_MAX_PIN) != Z_MAX_ENDSTOP_INVERTING);
						if(z_max_endstop && old_z_max_endstop && (current_block->steps_z > 0)) {
							endstops_trigsteps[Z_AXIS] = count_position[Z_AXIS];
							endstop_z_hit=true;
							step_events_completed = current_block->step_event_count;
						}
						old_z_max_endstop = z_max_endstop;
#endif
					}
				}

#ifndef ADVANCE
				if ((out_bits & (1<<E_AXIS)) != 0) {  // -direction
					REV_E_DIR();
					count_direction[E_AXIS]=-1;
				}
				else { // +direction
					NORM_E_DIR();
					count_direction[E_AXIS]=1;
				}
#endif //!ADVANCE

				for(int8_t i=0; i < step_loops; i++) { // Take multiple steps per interrupt (For high speed moves)
#ifdef ADVANCE
					counter_e += current_block->steps_e;
					if (counter_e > 0) {
						counter_e -= current_block->step_event_count;
						if ((out_bits & (1<<E_AXIS)) != 0) { // - direction
							e_steps[current_block->active_extruder]--;
						}
						else {
							e_steps[current_block->active_extruder]++;
						}
					}
#endif //ADVANCE

					counter_x += current_block->steps_x;
					if (counter_x > 0) {
#ifdef DUAL_X_CARRIAGE
						if (extruder_duplication_enabled){
							p_x_step = !INVERT_X_STEP_PIN; //WRITE(X_STEP_PIN, !INVERT_X_STEP_PIN);
							p_x2_step = !INVERT_X_STEP_PIN; //WRITE(X2_STEP_PIN, !INVERT_X_STEP_PIN);
						}
						else {
							if (current_block->active_extruder != 0)
								p_x2_step = !INVERT_X_STEP_PIN; //WRITE(X2_STEP_PIN, !INVERT_X_STEP_PIN);
							else
								p_x_step = !INVERT_X_STEP_PIN; //WRITE(X_STEP_PIN, !INVERT_X_STEP_PIN);
						}
#else
						p_x_step = !INVERT_X_STEP_PIN; //WRITE(X_STEP_PIN, !INVERT_X_STEP_PIN);
#endif
						counter_x -= current_block->step_event_count;
						count_position[X_AXIS]+=count_direction[X_AXIS];
#ifdef DUAL_X_CARRIAGE
						if (extruder_duplication_enabled){
							p_x_step = INVERT_X_STEP_PIN; //WRITE(X_STEP_PIN, INVERT_X_STEP_PIN);
							p_x2_step = INVERT_X_STEP_PIN; //WRITE(X2_STEP_PIN, INVERT_X_STEP_PIN);
						}
						else {
							if (current_block->active_extruder != 0)
								p_x2_step = INVERT_X_STEP_PIN; //WRITE(X2_STEP_PIN, INVERT_X_STEP_PIN);
							else
								p_x_step = INVERT_X_STEP_PIN; //WRITE(X_STEP_PIN, INVERT_X_STEP_PIN);
						}
#else
						p_x_step = INVERT_X_STEP_PIN; //WRITE(X_STEP_PIN, INVERT_X_STEP_PIN);
#endif
					}

					counter_y += current_block->steps_y;
					if (counter_y > 0) {
						p_y_step = !INVERT_Y_STEP_PIN; //WRITE(Y_STEP_PIN, !INVERT_Y_STEP_PIN);
						counter_y -= current_block->step_event_count;
						count_position[Y_AXIS]+=count_direction[Y_AXIS];
						p_y_step = INVERT_Y_STEP_PIN; //WRITE(Y_STEP_PIN, INVERT_Y_STEP_PIN);
					}

					counter_z += current_block->steps_z;
					if (counter_z > 0) {
						p_z_step = !INVERT_Z_STEP_PIN; //WRITE(Z_STEP_PIN, !INVERT_Z_STEP_PIN);

#ifdef Z_DUAL_STEPPER_DRIVERS
						p_z2_step = !INVER_Z_STEP_PIN; //WRITE(Z2_STEP_PIN, !INVERT_Z_STEP_PIN);
#endif

						counter_z -= current_block->step_event_count;
						count_position[Z_AXIS]+=count_direction[Z_AXIS];
						p_z_step = INVERT_Z_STEP_PIN; //WRITE(Z_STEP_PIN, INVERT_Z_STEP_PIN);

#ifdef Z_DUAL_STEPPER_DRIVERS
						p_z2_step = INVERT_Z_STEP_PIN;//WRITE(Z2_STEP_PIN, INVERT_Z_STEP_PIN);
#endif
					}

#ifndef ADVANCE
					counter_e += current_block->steps_e;
					if (counter_e > 0) {
						p_e_step = !INVERT_E_STEP_PIN; //WRITE_E_STEP(!INVERT_E_STEP_PIN);
						counter_e -= current_block->step_event_count;
						count_position[E_AXIS]+=count_direction[E_AXIS];
						p_e_step = INVERT_E_STEP_PIN; //WRITE_E_STEP(INVERT_E_STEP_PIN);
					}
#endif //!ADVANCE
					step_events_completed += 1;
					if(step_events_completed >= current_block->step_event_count) break;
				}
				// Calculare new timer value
				unsigned short timer;
				unsigned short step_rate;
				if (step_events_completed <= (unsigned long int)current_block->accelerate_until) {

					MultiU24X24toH16(acc_step_rate, acceleration_time, current_block->acceleration_rate);
					acc_step_rate += current_block->initial_rate;

					// upper limit
					if(acc_step_rate > current_block->nominal_rate)
						acc_step_rate = current_block->nominal_rate;

					// step_rate to timer interval
					timer = calc_timer(acc_step_rate);
					stepper_timer.detach();
					stepper_timer.attach_us(stepper_int_handler, timer);
					acceleration_time += timer;
#ifdef ADVANCE
					for(int8_t i=0; i < step_loops; i++) {
						advance += advance_rate;
					}
					//if(advance > current_block->advance) advance = current_block->advance;
					// Do E steps + advance steps
					e_steps[current_block->active_extruder] += ((advance >>8) - old_advance);
					old_advance = advance >>8;

#endif
				}
				else if (step_events_completed > (unsigned long int)current_block->decelerate_after) {
					MultiU24X24toH16(step_rate, deceleration_time, current_block->acceleration_rate);

					if(step_rate > acc_step_rate) { // Check step_rate stays positive
						step_rate = current_block->final_rate;
					}
					else {
						step_rate = acc_step_rate - step_rate; // Decelerate from aceleration end point.
					}

					// lower limit
					if(step_rate < current_block->final_rate)
						step_rate = current_block->final_rate;

					// step_rate to timer interval
					timer = calc_timer(step_rate);
					stepper_timer.detach();
					stepper_timer.attach_us(stepper_int_handler, timer);
					deceleration_time += timer;
#ifdef ADVANCE
					for(int8_t i=0; i < step_loops; i++) {
						advance -= advance_rate;
					}
					if(advance < final_advance) advance = final_advance;
					// Do E steps + advance steps
					e_steps[current_block->active_extruder] += ((advance >>8) - old_advance);
					old_advance = advance >>8;
#endif //ADVANCE
				}
				else {

					stepper_timer.detach();
					stepper_timer.attach_us(stepper_int_handler, OCR1A_nominal);
					// ensure we're running at the correct step rate, even if we just came off an acceleration
					step_loops = step_loops_nominal;
				}

				// If current block is finished, reset pointer
				if (step_events_completed >= current_block->step_event_count) {
					current_block = NULL;
					plan_discard_current_block();
				}
			}
		}

#ifdef ADVANCE
		unsigned char old_OCR0A;
		// Timer interrupt for E. e_steps is set in the main routine;
		// Timer 0 is shared with millies
		ISR(TIMER0_COMPA_vect)
		{
			old_OCR0A += 52; // ~10kHz interrupt (250000 / 26 = 9615kHz)
			OCR0A = old_OCR0A;
			// Set E direction (Depends on E direction + advance)
			for(unsigned char i=0; i<4;i++) {
				if (e_steps[0] != 0) {
					p_e0_step = INVERT_E_STEP_PIN; //WRITE(E0_STEP_PIN, INVERT_E_STEP_PIN);
					if (e_steps[0] < 0) {
						p_e0_dir = INVERT_E0_DIR; //WRITE(E0_DIR_PIN, INVERT_E0_DIR);
						e_steps[0]++;
						p_e0_step = !INVERT_E_STEP_PIN; //WRITE(E0_STEP_PIN, !INVERT_E_STEP_PIN);
					}
					else if (e_steps[0] > 0) {
						p_e0_dir = !INVERT_E0_DIR; //WRITE(E0_DIR_PIN, !INVERT_E0_DIR);
						e_steps[0]--;
						p_e0_step = !INVERT_E_STEP_PIN; //WRITE(E0_STEP_PIN, !INVERT_E_STEP_PIN);
					}
				}
#if EXTRUDERS > 1
				if (e_steps[1] != 0) {
					p_e1_step = INVERT_E_STEP_PIN; //WRITE(E1_STEP_PIN, INVERT_E_STEP_PIN);
					if (e_steps[1] < 0) {
						p_e1_dir = INVERT_E1_DIR; //WRITE(E1_DIR_PIN, INVERT_E1_DIR);
						e_steps[1]++;
						p_e1_step = !INVERT_E_STEP_PIN; //WRITE(E1_STEP_PIN, !INVERT_E_STEP_PIN);
					}
					else if (e_steps[1] > 0) {
						p_e1_dir = !INVERT_E1_DIR; //WRITE(E1_DIR_PIN, !INVERT_E1_DIR);
						e_steps[1]--;
						p_e1_step = !INVERT_E_STEP_PIN; //WRITE(E1_STEP_PIN, !INVERT_E_STEP_PIN);
					}
				}
#endif
#if EXTRUDERS > 2
				if (e_steps[2] != 0) {
					p_e2_step = INVERT_E_STEP_PIN; //WRITE(E2_STEP_PIN, INVERT_E_STEP_PIN);
					if (e_steps[2] < 0) {
						p_e2_dir = INVERT_E2_DIR; //WRITE(E2_DIR_PIN, INVERT_E2_DIR);
						e_steps[2]++;
						p_e2_step = !INVER_E_STEP_PIN; //WRITE(E2_STEP_PIN, !INVERT_E_STEP_PIN);
					}
					else if (e_steps[2] > 0) {
						p_e2_dir = !INVERT_E2_DIR; //WRITE(E2_DIR_PIN, !INVERT_E2_DIR);
						e_steps[2]--;
						p_e2_step = !INVERT_E_STEP_PIN; //WRITE(E2_STEP_PIN, !INVERT_E_STEP_PIN);
					}
				}
#endif
			}
		}
#endif // ADVANCE

		void st_init()
		{
#if defined(X_ENABLE_PIN) && X_ENABLE_PIN > -1
			if(!X_ENABLE_ON) p_x_enable = 1; //WRITE(X_ENABLE_PIN,1);
#endif
#if defined(X2_ENABLE_PIN) && X2_ENABLE_PIN > -1
			if(!X_ENABLE_ON) p_x2_enable = 1; //WRITE(X2_ENABLE_PIN,1);
#endif
#if defined(Y_ENABLE_PIN) && Y_ENABLE_PIN > -1
			if(!Y_ENABLE_ON) p_y_enable = 1; //WRITE(Y_ENABLE_PIN,1);
#endif
#if defined(Z_ENABLE_PIN) && Z_ENABLE_PIN > -1
			if(!Z_ENABLE_ON) p_z_enable = 1; //WRITE(Z_ENABLE_PIN,1);

#if defined(Z_DUAL_STEPPER_DRIVERS) && defined(Z2_ENABLE_PIN) && (Z2_ENABLE_PIN > -1)
			if(!Z_ENABLE_ON) p_z2_enable = 1; //WRITE(Z2_ENABLE_PIN,1);
#endif
#endif
#if defined(E0_ENABLE_PIN) && (E0_ENABLE_PIN > -1)
			if(!E_ENABLE_ON) p_e0_enable = 1; //WRITE(E0_ENABLE_PIN,1);
#endif
#if defined(E1_ENABLE_PIN) && (E1_ENABLE_PIN > -1)
			if(!E_ENABLE_ON) p_e1_enable = 1; //WRITE(E1_ENABLE_PIN,1);
#endif
#if defined(E2_ENABLE_PIN) && (E2_ENABLE_PIN > -1)
			if(!E_ENABLE_ON) p_e2_enable = 1; //WRITE(E2_ENABLE_PIN,1);
#endif

			//endstops and pullups

#if defined(X_MIN_PIN) && X_MIN_PIN > -1
#ifdef ENDSTOPPULLUP_XMIN
			p_x_min = 1; //WRITE(X_MIN_PIN,1);
#endif
#endif

#if defined(Y_MIN_PIN) && Y_MIN_PIN > -1
#ifdef ENDSTOPPULLUP_YMIN
			p_y_min = 1 ;//WRITE(Y_MIN_PIN,1);
#endif
#endif

#if defined(Z_MIN_PIN) && Z_MIN_PIN > -1
#ifdef ENDSTOPPULLUP_ZMIN
			p_z_min = 1; //WRITE(Z_MIN_PIN,1);
#endif
#endif

#if defined(X_MAX_PIN) && X_MAX_PIN > -1
#ifdef ENDSTOPPULLUP_XMAX
			p_x_max = 1; //WRITE(X_MAX_PIN,1);
#endif
#endif

#if defined(Y_MAX_PIN) && Y_MAX_PIN > -1
#ifdef ENDSTOPPULLUP_YMAX
			p_y_max = 1; //WRITE(Y_MAX_PIN,1);
#endif
#endif

#if defined(Z_MAX_PIN) && Z_MAX_PIN > -1
#ifdef ENDSTOPPULLUP_ZMAX
			p_z_max = 1; //WRITE(Z_MAX_PIN,1);
#endif
#endif

			//Initialize Step Pins
#if defined(X_STEP_PIN) && (X_STEP_PIN > -1)
			p_x_step = INVERT_X_STEP_PIN; //WRITE(X_STEP_PIN,INVERT_X_STEP_PIN);
			disable_x();
#endif
#if defined(X2_STEP_PIN) && (X2_STEP_PIN > -1)
			p_x2_step = INVERT_X_STEP_PIN; //WRITE(X2_STEP_PIN,INVERT_X_STEP_PIN);
			disable_x();
#endif
#if defined(Y_STEP_PIN) && (Y_STEP_PIN > -1)
			p_y_step = INVERT_Y_STEP_PIN; //WRITE(Y_STEP_PIN,INVERT_Y_STEP_PIN);
			disable_y();
#endif
#if defined(Z_STEP_PIN) && (Z_STEP_PIN > -1)
			p_z_step = INVERT_Z_STEP_PIN;//WRITE(Z_STEP_PIN,INVERT_Z_STEP_PIN);
#if defined(Z_DUAL_STEPPER_DRIVERS) && defined(Z2_STEP_PIN) && (Z2_STEP_PIN > -1)
			p_z2_step = INVERT_Z_STEP; //WRITE(Z2_STEP_PIN,INVERT_Z_STEP_PIN);
#endif
			disable_z();
#endif
#if defined(E0_STEP_PIN) && (E0_STEP_PIN > -1)
			p_e0_step = INVERT_E_STEP_PIN; //WRITE(E0_STEP_PIN,INVERT_E_STEP_PIN);
			disable_e0();
#endif
#if defined(E1_STEP_PIN) && (E1_STEP_PIN > -1)
			p_e1_step = INVERT_E_STEP_PIN; //WRITE(E1_STEP_PIN,INVERT_E_STEP_PIN);
			disable_e1();
#endif
#if defined(E2_STEP_PIN) && (E2_STEP_PIN > -1)
			p_e2_step = INVERT_E_STEP_PIN;//WRITE(E2_STEP_PIN,INVERT_E_STEP_PIN);
			disable_e2();
#endif

			stepper_timer.attach_us(stepper_int_handler,2000);

			ENABLE_STEPPER_DRIVER_INTERRUPT();

#ifdef ADVANCE
			e_steps[0] = 0;
			e_steps[1] = 0;
			e_steps[2] = 0;
			TIMSK0 |= (1<<OCIE0A);
#endif //ADVANCE

			enable_endstops(true); // Start with endstops active. After homing they can be disabled
			sei();
		}


		// Block until all buffered steps are executed
		void st_synchronize()
		{
			while( blocks_queued()) {
				manage_heater();
				manage_inactivity();
			}
		}

		void st_set_position(const long &x, const long &y, const long &z, const long &e)
		{
			CRITICAL_SECTION_START;
			count_position[X_AXIS] = x;
			count_position[Y_AXIS] = y;
			count_position[Z_AXIS] = z;
			count_position[E_AXIS] = e;
			CRITICAL_SECTION_END;
		}

		void st_set_e_position(const long &e)
		{
			CRITICAL_SECTION_START;
			count_position[E_AXIS] = e;
			CRITICAL_SECTION_END;
		}

		long st_get_position(uint8_t axis)
		{
			long count_pos;
			CRITICAL_SECTION_START;
			count_pos = count_position[axis];
			CRITICAL_SECTION_END;
			return count_pos;
		}

		void finishAndDisableSteppers()
		{
			st_synchronize();
			disable_x();
			disable_y();
			disable_z();
			disable_e0();
			disable_e1();
			disable_e2();
		}

		void quickStop()
		{
			DISABLE_STEPPER_DRIVER_INTERRUPT();
			while(blocks_queued())
				plan_discard_current_block();
			current_block = NULL;
			ENABLE_STEPPER_DRIVER_INTERRUPT();
		}
