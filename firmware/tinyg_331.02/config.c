/*
 * config.c - eeprom and compile time configuration handling 
 * Part of TinyG project
 *
 * Copyright (c) 2010 - 2012 Alden S. Hart Jr.
 *
 * TinyG is free software: you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, 
 * or (at your option) any later version.
 *
 * TinyG is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 * See the GNU General Public License for details.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * You should have received a copy of the GNU General Public License 
 * along with TinyG  If not, see <http://www.gnu.org/licenses/>.
 */
/*
 *	Config system overview
 *
 *	Config has been rewritten to support JSON objects and to be easier to extend 
 *	and modify. Each configuration value is identified by a friendly name (name).
 *	The friendly name uniquely maps to a short mnemonic string (token), 
 *	which in turn finds the index into the config arrays.
 *
 *	Config keeps the following arrays:
 * 
 *	- PROGMEM array (cfgArray) contains typed data in program memory. Each item has:
 *		- function pointer for print() method
 *		- function pointer for get() method
 *		- function pointer for set() method
 *		- target (memory location that the value is written to)
 *		- default value - for cold initialization
 *		- pointer to combined string, a comma separated list which carries:
 *			- token string
 *			- friendly name lookup string (just long enough for matching)
 *			- format string for print formatting
 *
 *	- NVM array - Contains tokens and values persisted to EEPROM (NVM)
 *		The tokens are used for data migration across firmware versions.
 *
 *	The following rules apply to friendly names:
 *	 - can be up to 24 chars and cannot contain whitespace or separators ( =  :  | , )  
 *	 - must be unique (non colliding).
 *	 - are case insensitive (and usually written as all lowercase)
 *	 - by convention axis and motor friendly names start with the axis letter 
 *		(e.g. x_feedrate) or motor designator (e.g. m1_microsteps)
 *
 *	The following rules apply to mnemonic tokens
 *	 - are 2 or 3 characters and cannot contain whitespace or separators ( =  :  | , )
 *	 - must be unique (non colliding).
 *	 - axis tokens start with the axis letter and are 3 characters including the axis letter
 *	 - motor tokens start with the motor digit and are 3 characters including the motor digit
 *	 - non-axis or non-motor tokens are 2 characters and cannot start with: xyzabcuvw0123456789
 *
 *	Adding a new value to config (or changing an existing one) involves touching the following places:
 *	 - Add a token / friendly name / formatting string to str_XXX strings (ensure unique token & name!)
 *	 - Create a new record in cfgArray[] with includes:
 *		- reference to the above string
 *		- an existing print() function or create a new one if necessary 
 *		- an existing apply() fucntion or create a new one if necessary
 *		- target pointer (a variable must exist somewhere, often in the cfg struct)
 *		- default value for the parameter
 *	 - Change CFG_VERSION in config.h to something different so it will migrate ye olde configs in NVM.
 *
 * 	The order of display is set by the order of strArray. None of the other orders 
 *	matter but are generally kept sequenced for easier reading and code maintenance.
 *
 *	Command line vs JSON operation
 *
 *	Config can be used as command line (text-based) or using JSON objects. 
 *	All functions are identical and can be accessed either way. 
 */
#include <ctype.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>			// precursor for xio.h
#include <avr/pgmspace.h>	// precursor for xio.h

#include "tinyg.h"			// config reaches into almost everything
#include "util.h"
#include "config.h"
#include "report.h"
#include "settings.h"
#include "controller.h"
#include "canonical_machine.h"
#include "gcode_parser.h"
#include "planner.h"
#include "stepper.h"
#include "system.h"
#include "xio/xio.h"
#include "xmega/xmega_eeprom.h"

//*** STATIC STUFF ***********************************************************

//static char temp[128];

typedef void (*fptrConfig)(INDEX_T i);	// required for PROGMEM access

struct cfgItem {			// structs for pgmArray
	char *string;			// pointer to names composite string
	fptrConfig print;		// print binding: aka void (*print)(INDEX_T i);
	fptrCmd get;			// GET binding aka uint8_t (*get)(const INDEX_T i, struct cmdObject *cmd[])
	fptrCmd set;			// SET binding aka uint8_t (*set)(const INDEX_T i, struct cmdObject *cmd[])
	double *target;			// target for writing config value
	double def_value;		// default value for config item
};

// generic internal functions
static uint8_t _set_nul(const INDEX_T i, cmdObj *cmd);	// noop
static uint8_t _set_ui8(const INDEX_T i, cmdObj *cmd);	// set a uint8_t value
static uint8_t _set_int(const INDEX_T i, cmdObj *cmd);	// set an integer value
static uint8_t _set_dbl(const INDEX_T i, cmdObj *cmd);	// set a double value
static uint8_t _set_dbu(const INDEX_T i, cmdObj *cmd);	// set a double with unit conversion

//static uint8_t _get_nul(const INDEX_T i, cmdObj *cmd);	// get null value type
static uint8_t _get_ui8(const INDEX_T i, cmdObj *cmd);	// get uint8_t value
static uint8_t _get_int(const INDEX_T i, cmdObj *cmd);	// get uint32_t integer value
static uint8_t _get_dbl(const INDEX_T i, cmdObj *cmd);	// get double value
static uint8_t _get_dbu(const INDEX_T i, cmdObj *cmd);	// get double with unit conversion

static uint8_t _get_ui8_value(const INDEX_T i);			// return uint8_t value
static uint8_t _get_int_value(const INDEX_T i);			// return uint32_t value
static double _get_dbl_value(const INDEX_T i);			// return double
static double _get_dbu_value(const INDEX_T i);			// return double with unit conversion

static void _print_nul(const INDEX_T i);				// print nothing
static void _print_ui8(const INDEX_T i);				// print unit8_t value w/no units
static void _print_int(const INDEX_T i);				// print integer value w/no units
static void _print_dbl(const INDEX_T i);				// print double value w/no units
static void _print_lin(const INDEX_T i);				// print linear values
static void _print_rot(const INDEX_T i);				// print rotary values

// helpers
static char *_get_format(const INDEX_T i, char *format);
static int8_t _get_axis(const INDEX_T i);
static int8_t _get_motor(const INDEX_T i);
static uint8_t _parse_config_string(char *str, struct cmdObject *c);

/*****************************************************************************
 **** PARAMETER-SPECIFIC CODE REGION *****************************************
 **** This code and data will change as you add / update config parameters ***
 *****************************************************************************/

// specialized internal functions
static uint8_t _get_unit(const INDEX_T i, cmdObj *cmd);	// get units mode as a string
static uint8_t _get_sr(const INDEX_T i, cmdObj *cmd);	// run status report
static uint8_t _set_sr(const INDEX_T i, cmdObj *cmd);	// set status report specification
static uint8_t _get_si(const INDEX_T i, cmdObj *cmd);	// get status report interval
static uint8_t _set_si(const INDEX_T i, cmdObj *cmd);	// set status report interval
static uint8_t _get_gc(const INDEX_T i, cmdObj *cmd);	// get current gcode block
static uint8_t _run_gc(const INDEX_T i, cmdObj *cmd);	// run a gcode block
static uint8_t _get_stat(const INDEX_T i, cmdObj *cmd);	// get machine state as value and string
static uint8_t _get_vel(const INDEX_T i, cmdObj *cmd);	// get current velocity
static uint8_t _get_abs(const INDEX_T i, cmdObj *cmd);	// get current machine position
static uint8_t _get_pos(const INDEX_T i, cmdObj *cmd);	// get current work position
static uint8_t _get_am(const INDEX_T i, cmdObj *cmd);	// get axis mode

static uint8_t _set_ic(const INDEX_T i, cmdObj *cmd);	// ignore CR on input
static uint8_t _set_il(const INDEX_T i, cmdObj *cmd);	// ignore LF on input
static uint8_t _set_ec(const INDEX_T i, cmdObj *cmd);	// expand CRLF on outout
static uint8_t _set_ee(const INDEX_T i, cmdObj *cmd);	// enable character echo
static uint8_t _set_ex(const INDEX_T i, cmdObj *cmd);	// enable XON/XOFF

static uint8_t _set_sa(const INDEX_T i, cmdObj *cmd);	// set motor step angle
static uint8_t _set_mi(const INDEX_T i, cmdObj *cmd);	// set microsteps
static uint8_t _set_po(const INDEX_T i, cmdObj *cmd);	// set motor polarity
static uint8_t _set_motor_steps_per_unit(const INDEX_T i, cmdObj *cmd);

static void _print_am(const INDEX_T i);					// print axis mode

static void _print_grp(const INDEX_T i);				// print a group
static uint8_t _set_grp(const INDEX_T i, cmdObj *cmd);	// set data for a group
static uint8_t _get_grp(const INDEX_T i, cmdObj *cmd);	// get data for a group
static uint8_t _get_sys(const INDEX_T i, cmdObj *cmd);	// get data for system group (special case)
static uint8_t _get_qm(const INDEX_T i, cmdObj *cmd);	// get ? group report data

//--- PROGMEM Strings -------------------------------------------------------
/* PROGMEM strings for token, friendly name match string, and print format
 *	Use accessors to get at elements in the combined strings.
 *
 *	NOTE: DO NOT USE TABS IN FORMAT STRINGS
 *	NOTE: LEAVE NO SPACE BEFORE OR AFTER FIRST COMMA (TOKEN,NAME)
 *		  LEAVE NO SPACE BEFORE SECOND COMMA (SPACE AFTER IS OK)
 */
char str_fc[] PROGMEM = "fc,config_v,[fc]  config_version  %16.2f\n";
char str_fv[] PROGMEM = "fv,firmware_v,[fv]  firmware_version%16.2f\n";
char str_fb[] PROGMEM = "fb,firmware_b,[fb]  firmware_build  %16.2f\n";

char str_line[] PROGMEM = "line,line_number,[line] line_number%17.0f\n";
char str_stat[] PROGMEM = "stat,machine_state,[stat] machine_state %14d\n";
//char str_fs[] PROGMEM = "fs,feedhold_state,[fs]  feedhold_state %1d\n";
char str_vel[] PROGMEM = "vel,velocity,[vel] velocity %23.3f %S/min\n";
char str_unit[] PROGMEM = "unit,unit,";		// current units mode as an ASCII string
char str_sr[] PROGMEM = "sr,status_r,";		// status_report
char str_si[] PROGMEM = "si,status_i,[si]  status_interval    %10.0f ms [0=off]\n";

char str_gc[] PROGMEM = "gc,gcod,[gc]";
// gcode power-on reset defaults
char str_gpl[] PROGMEM = "gpl,gcode_pl,[gpl] gcode_select_plane %10d [G17,G18,G19]\n";
char str_gun[] PROGMEM = "gun,gcode_u, [gun] gcode_units_mode   %10d [G20,G21]\n";
char str_gco[] PROGMEM = "gco,gcode_c, [gco] gcode_coord_system %10d [G54-G59]\n";
char str_gpa[] PROGMEM = "gpa,gcode_pa,[gpa] gcode_path_control %10d [G61,G61.1,G64]\n";
char str_gdi[] PROGMEM = "gdi,gcode_d, [gdi] gcode_distance_mode%10d [G90,G91]\n";
// gcode current model state
char str_gmun[] PROGMEM = "gmun,gmun,Units%10d\n";
char str_gmpl[] PROGMEM = "gmpl,gmpl,Plane%10d\n";
char str_gmco[] PROGMEM = "gmco,gmco,Coordinate system%10d\n";
char str_gmdi[] PROGMEM = "gmdi,gmdi,Distance mode%10d\n";
char str_gmfr[] PROGMEM = "gmfr,gmfr,Feed rate%10d\n";
char str_gmmm[] PROGMEM = "gmmm,gmmm,Motion mode%10d\n";

char str_ea[] PROGMEM = "ea,enable_a,[ea]  enable_acceleration%10d [0,1]\n";
char str_ja[] PROGMEM = "ja,junc,[ja]  junction_acceleration%8.0f%S\n";
char str_ml[] PROGMEM = "ml,min_l,[ml]  min_line_segment   %14.3f%S\n";
char str_ma[] PROGMEM = "ma,min_a,[ma]  min_arc_segment    %14.3f%S\n";
char str_mt[] PROGMEM = "mt,min_s,[mt]  min_segment_time   %10.0f uSec\n";

char str_ic[] PROGMEM = "ic,ignore_c,[ic]  ignore_CR (on RX)%12d [0,1]\n";
char str_il[] PROGMEM = "il,ignore_l,[il]  ignore_LF (on RX)%12d [0,1]\n";
char str_ec[] PROGMEM = "ec,enable_c,[ec]  enable_CR (on TX)%12d [0,1]\n";
char str_ee[] PROGMEM = "ee,enable_e,[ee]  enable_echo      %12d [0,1]\n";
char str_ex[] PROGMEM = "ex,enable_x,[ex]  enable_xon_xoff  %12d [0,1]\n";

// Motor strings in program memory 
char str_1ma[] PROGMEM = "1ma,m1_ma, [1ma] m1_map_to_axis%15d [0=X, 1=Y...]\n";
char str_1sa[] PROGMEM = "1sa,m1_s,  [1sa] m1_step_angle%20.3f%S\n";
char str_1tr[] PROGMEM = "1tr,m1_tr, [1tr] m1_travel_per_revolution%9.3f%S\n";
char str_1mi[] PROGMEM = "1mi,m1_mi, [1mi] m1_microsteps %15d [1,2,4,8]\n";
char str_1po[] PROGMEM = "1po,m1_pol,[1po] m1_polarity   %15d [0,1]\n";
char str_1pm[] PROGMEM = "1pm,m1_pow,[1pm] m1_power_management%10d [0,1]\n";

char str_2ma[] PROGMEM = "2ma,m2_ma, [2ma] m2_map_to_axis%15d [0=X, 1=Y...]\n";
char str_2sa[] PROGMEM = "2sa,m2_s,  [2sa] m2_step_angle%20.3f%S\n";
char str_2tr[] PROGMEM = "2tr,m2_tr, [2tr] m2_travel_per_revolution%9.3f%S\n";
char str_2mi[] PROGMEM = "2mi,m2_mi, [2mi] m2_microsteps %15d [1,2,4,8]\n";
char str_2po[] PROGMEM = "2po,m2_pol,[2po] m2_polarity   %15d [0,1]\n";
char str_2pm[] PROGMEM = "2pm,m2_pow,[2pm] m2_power_management%10d [0,1]\n";

char str_3ma[] PROGMEM = "3ma,m3_ma, [3ma] m3_map_to_axis%15d [0=X, 1=Y...]\n";
char str_3sa[] PROGMEM = "3sa,m3_s,  [3sa] m3_step_angle%20.3f%S\n";
char str_3tr[] PROGMEM = "3tr,m3_tr, [3tr] m3_travel_per_revolution%9.3f%S\n";
char str_3mi[] PROGMEM = "3mi,m3_mi, [3mi] m3_microsteps %15d [1,2,4,8]\n";
char str_3po[] PROGMEM = "3po,m3_pol,[3po] m3_polarity   %15d [0,1]\n";
char str_3pm[] PROGMEM = "3pm,m3_pow,[3pm] m3_power_management%10d [0,1]\n";

char str_4ma[] PROGMEM = "4ma,m4_ma, [4ma] m4_map_to_axis%15d [0=X, 1=Y...]\n";
char str_4sa[] PROGMEM = "4sa,m4_s,  [4sa] m4_step_angle%20.3f%S\n";
char str_4tr[] PROGMEM = "4tr,m4_tr, [4tr] m4_travel_per_revolution%9.3f%S\n";
char str_4mi[] PROGMEM = "4mi,m4_mi, [4mi] m4_microsteps %15d [1,2,4,8]\n";
char str_4po[] PROGMEM = "4po,m4_pol,[4po] m4_polarity   %15d [0,1]\n";
char str_4pm[] PROGMEM = "4pm,m4_pow,[4pm] m4_power_management%10d [0,1]\n";

// Axis strings in program memory
char str_xam[] PROGMEM = "xam,x_a,[xam] x_axis_mode%18d %S\n";
char str_xfr[] PROGMEM = "xfr,x_f,[xfr] x_feedrate_maximum%15.3f%S/min\n";
char str_xvm[] PROGMEM = "xvm,x_v,[xvm] x_velocity_maximum%15.3f%S/min\n";
char str_xtm[] PROGMEM = "xtm,x_t,[xtm] x_travel_maximum%17.3f%S\n";
char str_xjm[] PROGMEM = "xjm,x_je,[xjm] x_jerk_maximum%15.0f%S/min^3\n";
char str_xjd[] PROGMEM = "xjd,x_ju,[xjd] x_junction_deviation%14.4f%S\n";
char str_xsm[] PROGMEM = "xsm,x_s,[xsm] x_switch_mode%16d [0,1]\n";
char str_xsv[] PROGMEM = "xsv,x_s,[xsv] x_search_velocity%16.3f%S/min\n";
char str_xlv[] PROGMEM = "xlv,x_l,[xlv] x_latch_velocity%17.3f%S/min\n";
char str_xzo[] PROGMEM = "xzo,x_z,[xzo] x_zero_offset%20.3f%S\n";
char str_xabs[] PROGMEM = "xabs,x_ab,x_absolute_position%13.3f%S\n";
char str_xpos[] PROGMEM = "xpos,x_po,x_position%22.3f%S\n";

char str_yam[] PROGMEM = "yam,y_a,[yam] y_axis_mode%18d %S\n";
char str_yfr[] PROGMEM = "yfr,y_f,[yfr] y_feedrate_maximum%15.3f%S/min\n";
char str_yvm[] PROGMEM = "yvm,y_v,[yvm] y_velocity_maximum%15.3f%S/min\n";
char str_ytm[] PROGMEM = "ytm,y_t,[ytm] y_travel_maximum%17.3f%S\n";
char str_yjm[] PROGMEM = "yjm,y_je,[yjm] y_jerk_maximum%15.0f%S/min^3\n";
char str_yjd[] PROGMEM = "yjd,y_ju,[yjd] y_junction_deviation%14.4f%S\n";
char str_ysm[] PROGMEM = "ysm,y_s,[ysm] y_switch_mode%16d [0,1]\n";
char str_ysv[] PROGMEM = "ysv,y_s,[ysv] y_search_velocity%16.3f%S/min\n";
char str_ylv[] PROGMEM = "ylv,y_l,[ylv] y_latch_velocity%17.3f%S/min\n";
char str_yzo[] PROGMEM = "yzo,y_z,[yzo] y_zero_offset%20.3f%S\n";
char str_yabs[] PROGMEM = "yabs,y_ab,[yabs] y_absolute_position%13.3f%S\n";
char str_ypos[] PROGMEM = "ypos,y_po,[ypos] y_position%22.3f%S\n";

char str_zam[] PROGMEM = "zam,z_a,[zam] z_axis_mode%18d %S\n";
char str_zfr[] PROGMEM = "zfr,z_f,[zfr] z_feedrate_maximum%15.3f%S/min\n";
char str_zvm[] PROGMEM = "zvm,z_v,[zvm] z_velocity_maximum%15.3f%S/min\n";
char str_ztm[] PROGMEM = "ztm,z_t,[ztm] z_travel_maximum%17.3f%S\n";
char str_zjm[] PROGMEM = "zjm,z_je,[zjm] z_jerk_maximum%15.0f%S/min^3\n";
char str_zjd[] PROGMEM = "zjd,z_ju,[zjd] z_junction_deviation%14.4f%S\n";
char str_zsm[] PROGMEM = "zsm,z_s,[zsm] z_switch_mode%16d [0,1]\n";
char str_zsv[] PROGMEM = "zsv,z_s,[zsv] z_search_velocity%16.3f%S/min\n";
char str_zlv[] PROGMEM = "zlv,z_l,[zlv] z_latch_velocity%17.3f%S/min\n";
char str_zzo[] PROGMEM = "zzo,z_z,[zzo] z_zero_offset%20.3f%S\n";
char str_zabs[] PROGMEM = "zabs,z_ab,[zabs] z_absolute_position%13.3f%S\n";
char str_zpos[] PROGMEM = "zpos,z_po,[zpos] z_position%22.3f%S\n";

char str_aam[] PROGMEM = "aam,a_a,[aam] a_axis_mode%18d %S\n";
char str_afr[] PROGMEM = "afr,a_f,[afr] a_feedrate_maximum%15.3f%S/min\n";
char str_avm[] PROGMEM = "avm,a_v,[avm] a_velocity_maximum%15.3f%S/min\n";
char str_atm[] PROGMEM = "atm,a_t,[atm] a_travel_maximum  %15.3f%S\n";
char str_ajm[] PROGMEM = "ajm,a_je,[ajm] a_jerk_maximum%15.0f%S/min^3\n";
char str_ajd[] PROGMEM = "ajd,a_ju,[ajc] a_junction_deviation%14.4f%S\n";
char str_ara[] PROGMEM = "ara,a_r,[ara] a_radius_value%20.4f%S\n";
char str_asm[] PROGMEM = "asm,a_s,[asm] a_switch_mode%16d [0,1]\n";
char str_asv[] PROGMEM = "asv,a_s,[asv] a_search_velocity%16.3f%S/min\n";
char str_alv[] PROGMEM = "alv,a_l,[alv] a_latch_velocity%17.3f%S/min\n";
char str_azo[] PROGMEM = "azo,a_z,[azo] a_zero_offset%20.3f%S\n";
char str_aabs[] PROGMEM = "aabs,a_ab,[aabs] a_absolute_position%13.3f%S\n";
char str_apos[] PROGMEM = "apos,a_po,[apos] a_position%22.3f%S\n";

char str_bam[] PROGMEM = "bam,b_a,[bam] b_axis_mode%18d %S\n";
char str_bfr[] PROGMEM = "bfr,b_f,[bfr] b_feedrate_maximum%15.3f%S/min\n";
char str_bvm[] PROGMEM = "bvm,b_v,[bvm] b_velocity_maximum%15.3f%S/min\n";
char str_btm[] PROGMEM = "btm,b_t,[btm] b_travel_maximum%17.3f%S\n";
char str_bjm[] PROGMEM = "bjm,b_je,[bjm] b_jerk_maximum%15.0f%S/min^3\n";
char str_bjd[] PROGMEM = "bcd,b_ju,[bjd] b_junction_deviation%14.4f%S\n";
char str_bra[] PROGMEM = "bra,b_r,[bra] b_radius_value%20.4f%S\n";
char str_bsm[] PROGMEM = "bsm,b_s,[bsm] b_switch_mode%16d [0,1]\n";
char str_bsv[] PROGMEM = "bsv,b_s,[bsv] b_search_velocity%16.3f%S/min\n";
char str_blv[] PROGMEM = "blv,b_l,[blv] b_latch_velocity%17.3f%S/min\n";
char str_bzo[] PROGMEM = "bzo,b_z,[bzo] b_zero_offset%20.3f%S\n";
char str_babs[] PROGMEM = "babs,b_ab,[babs] b_absolute_position%13.3f%S\n";
char str_bpos[] PROGMEM = "bpos,b_po,[bpos] b_position%22.3f%S\n";

char str_cam[] PROGMEM = "cam,c_a,[cam] c_axis_mode%18d %S\n";
char str_cfr[] PROGMEM = "cfr,c_f,[cfr] c_feedrate_maximum%15.3f%S/min\n";
char str_cvm[] PROGMEM = "cvm,c_v,[cvm] c_velocity_maximum%15.3f%S/min\n";
char str_ctm[] PROGMEM = "ctm,c_t,[ctm] c_travel_maximum%17.3f%S\n";
char str_cjm[] PROGMEM = "cjm,c_je,[cjm] c_jerk_maximum%15.0f%S/min^3\n";
char str_cjd[] PROGMEM = "cjd,c_ju,[cjd] c_junction_deviation%14.4f%S\n";
char str_cra[] PROGMEM = "cra,c_r,[cra] c_radius_value%20.4f%S\n";
char str_csm[] PROGMEM = "csm,c_s,[csm] c_switch_mode%16d [0,1]\n";
char str_csv[] PROGMEM = "csv,c_s,[csv] c_search_velocity%16.3f%S/min\n";
char str_clv[] PROGMEM = "cls,c_l,[clv] c_latch_velocity%17.3f%S/min\n";
char str_czo[] PROGMEM = "czo,c_z,[czo] c_zero_offset%20.3f%S\n";
char str_cabs[] PROGMEM = "cabs,c_ab,[cabs] c_absolute_position%13.3f%S\n";
char str_cpos[] PROGMEM = "cpos,c_po,[cpos] c_position%22.3f%S\n";

char str_g54x[] PROGMEM = "g54x,g54_x,[g54x] g54_x_offset%18.3f%S\n";	// coordinate system offsets
char str_g54y[] PROGMEM = "g54y,g54_y,[g54y] g54_y_offset%18.3f%S\n";
char str_g54z[] PROGMEM = "g54z,g54_z,[g54z] g54_z_offset%18.3f%S\n";
char str_g54a[] PROGMEM = "g54a,g54_a,[g54a] g54_a_offset%18.3f%S\n";
char str_g54b[] PROGMEM = "g54b,g54_b,[g54b] g54_b_offset%18.3f%S\n";
char str_g54c[] PROGMEM = "g54c,g54_c,[g54c] g54_c_offset%18.3f%S\n";

char str_g55x[] PROGMEM = "g55x,g55_x,[g55x] g55_x_offset%18.3f%S\n";
char str_g55y[] PROGMEM = "g55y,g55_y,[g55y] g55_y_offset%18.3f%S\n";
char str_g55z[] PROGMEM = "g55z,g55_z,[g55z] g55_z_offset%18.3f%S\n";
char str_g55a[] PROGMEM = "g55a,g55_a,[g55a] g55_a_offset%18.3f%S\n";
char str_g55b[] PROGMEM = "g55b,g55_b,[g55b] g55_b_offset%18.3f%S\n";
char str_g55c[] PROGMEM = "g55c,g55_c,[g55c] g55_c_offset%18.3f%S\n";

char str_g56x[] PROGMEM = "g56x,g56_x,[g56x] g56_x_offset%18.3f%S\n";
char str_g56y[] PROGMEM = "g56y,g56_y,[g56y] g56_y_offset%18.3f%S\n";
char str_g56z[] PROGMEM = "g56z,g56_z,[g56z] g56_z_offset%18.3f%S\n";
char str_g56a[] PROGMEM = "g56a,g56_a,[g56a] g56_a_offset%18.3f%S\n";
char str_g56b[] PROGMEM = "g56b,g56_b,[g56b] g56_b_offset%18.3f%S\n";
char str_g56c[] PROGMEM = "g56c,g56_c,[g56c] g56_c_offset%18.3f%S\n";

char str_g57x[] PROGMEM = "g57x,g57_x,[g57x] g57_x_offset%18.3f%S\n";
char str_g57y[] PROGMEM = "g57y,g57_y,[g57y] g57_y_offset%18.3f%S\n";
char str_g57z[] PROGMEM = "g57z,g57_z,[g57z] g57_z_offset%18.3f%S\n";
char str_g57a[] PROGMEM = "g57a,g57_a,[g57a] g57_a_offset%18.3f%S\n";
char str_g57b[] PROGMEM = "g57b,g57_b,[g57b] g57_b_offset%18.3f%S\n";
char str_g57c[] PROGMEM = "g57c,g57_c,[g57c] g57_c_offset%18.3f%S\n";

char str_g58x[] PROGMEM = "g58x,g58_x,[g58x] g58_x_offset%18.3f%S\n";
char str_g58y[] PROGMEM = "g58y,g58_y,[g58y] g58_y_offset%18.3f%S\n";
char str_g58z[] PROGMEM = "g58z,g58_z,[g58z] g58_z_offset%18.3f%S\n";
char str_g58a[] PROGMEM = "g58a,g58_a,[g58a] g58_a_offset%18.3f%S\n";
char str_g58b[] PROGMEM = "g58b,g58_b,[g58b] g58_b_offset%18.3f%S\n";
char str_g58c[] PROGMEM = "g58c,g58_c,[g58c] g58_c_offset%18.3f%S\n";

char str_g59x[] PROGMEM = "g59x,g59_x,[g59x] g59_x_offset%18.3f%S\n";
char str_g59y[] PROGMEM = "g59y,g59_y,[g59y] g59_y_offset%18.3f%S\n";
char str_g59z[] PROGMEM = "g59z,g59_z,[g59z] g59_z_offset%18.3f%S\n";
char str_g59a[] PROGMEM = "g59a,g59_a,[g59a] g59_a_offset%18.3f%S\n";
char str_g59b[] PROGMEM = "g59b,g59_b,[g59b] g59_b_offset%18.3f%S\n";
char str_g59c[] PROGMEM = "g59c,g59_c,[g59c] g59_c_offset%18.3f%S\n";

  // persistence for status report vector
char str_sr00[] PROGMEM = "sr00,sr00,";
char str_sr01[] PROGMEM = "sr01,sr01,";
char str_sr02[] PROGMEM = "sr02,sr02,";
char str_sr03[] PROGMEM = "sr03,sr03,";
char str_sr04[] PROGMEM = "sr04,sr04,";
char str_sr05[] PROGMEM = "sr05,sr05,";
char str_sr06[] PROGMEM = "sr06,sr06,";
char str_sr07[] PROGMEM = "sr07,sr07,";
char str_sr08[] PROGMEM = "sr08,sr08,";
char str_sr09[] PROGMEM = "sr09,sr09,";
char str_sr10[] PROGMEM = "sr10,sr10,";
char str_sr11[] PROGMEM = "sr11,sr11,";
char str_sr12[] PROGMEM = "sr12,sr12,";
char str_sr13[] PROGMEM = "sr13,sr13,";
char str_sr14[] PROGMEM = "sr14,sr14,";
char str_sr15[] PROGMEM = "sr15,sr15,";
char str_sr16[] PROGMEM = "sr16,sr16,";
char str_sr17[] PROGMEM = "sr17,sr17,";
char str_sr18[] PROGMEM = "sr18,sr18,";
char str_sr19[] PROGMEM = "sr19,sr19,";

// group strings
char str_sys[] PROGMEM = "sys,sys,";	// system group
char str_qm[] PROGMEM = "?,qm,";		// question mark report
char str_x[] PROGMEM = "x,x,";			// axis groups
char str_y[] PROGMEM = "y,y,";
char str_z[] PROGMEM = "z,z,";
char str_a[] PROGMEM = "a,a,";
char str_b[] PROGMEM = "b,b,";
char str_c[] PROGMEM = "c,c,";
char str_1[] PROGMEM = "1,1,";			// motor groups
char str_2[] PROGMEM = "2,2,";
char str_3[] PROGMEM = "3,3,";
char str_4[] PROGMEM = "4,4,";
char str_g54[] PROGMEM = "g54,g54,";	// coordinate system offset groups
char str_g55[] PROGMEM = "g55,g55,";
char str_g56[] PROGMEM = "g56,g56,";
char str_g57[] PROGMEM = "g57,g57,";
char str_g58[] PROGMEM = "g58,g58,";
char str_g59[] PROGMEM = "g59,g59,";

/***** PROGMEM config array **************************************************
 */
struct cfgItem cfgArray[] PROGMEM = {

//	 string *, print func, get func, set func  target for get/set,    default value
	{ str_fc, _print_dbl, _get_dbl, _set_nul, (double *)&cfg.version, TINYG_CONFIG_VERSION }, // must be first
	{ str_fv, _print_dbl, _get_dbl, _set_nul, (double *)&tg.version,  TINYG_VERSION_NUMBER },
	{ str_fb, _print_dbl, _get_dbl, _set_nul, (double *)&tg.build,    TINYG_BUILD_NUMBER },

	{ str_line,_print_int, _get_int, _set_int, (double *)&cm.linenum, 0 },		// line number
	{ str_stat,_print_ui8, _get_stat,_set_nul, (double *)&cm.machine_state,0 },	// machine state
//	{ str_fs,  _print_ui8, _get_ui8, _set_nul, (double *)&cm.hold_state,0 },	// feedhold state
	{ str_vel, _print_lin, _get_vel, _set_nul, (double *)&tg.null, 0 },			// current runtime velocity
	{ str_unit,_print_nul, _get_unit,_set_nul, (double *)&gm.units_mode, 0 },	// units mode as string
	{ str_sr,  _print_nul, _get_sr,  _set_sr,  (double *)&tg.null, 0 },			// status report object
	{ str_si,  _print_dbl, _get_si,  _set_si,  (double *)&cfg.status_report_interval, STATUS_REPORT_INTERVAL_MS },

	// NOTE: The ordering within the gcode group is important for token resolution
	{ str_gc,  _print_nul, _get_gc, _run_gc,  (double *)&tg.null, 0 },	 // gcode block
	{ str_gpl, _print_ui8, _get_ui8,_set_ui8, (double *)&cfg.select_plane, GCODE_DEFAULT_PLANE },
	{ str_gun, _print_ui8, _get_ui8,_set_ui8, (double *)&cfg.units_mode,   GCODE_DEFAULT_UNITS },
	{ str_gco, _print_ui8, _get_ui8,_set_ui8, (double *)&cfg.coord_system, GCODE_DEFAULT_COORD_SYSTEM },
	{ str_gpa, _print_ui8, _get_ui8,_set_ui8, (double *)&cfg.path_control, GCODE_DEFAULT_PATH_CONTROL },
	{ str_gdi, _print_ui8, _get_ui8,_set_ui8, (double *)&cfg.distance_mode,GCODE_DEFAULT_DISTANCE_MODE },

	{ str_ea, _print_ui8, _get_ui8, _set_ui8, (double *)&cfg.enable_acceleration,  ENABLE_ACCELERATION },
	{ str_ja, _print_lin, _get_dbu, _set_dbu, (double *)&cfg.junction_acceleration,JUNCTION_ACCELERATION },
	{ str_ml, _print_lin, _get_dbu, _set_dbu, (double *)&cfg.min_segment_len,   MIN_LINE_LENGTH },
	{ str_ma, _print_lin, _get_dbu, _set_dbu, (double *)&cfg.arc_segment_len,   MM_PER_ARC_SEGMENT },
	{ str_mt, _print_lin, _get_dbl, _set_dbl, (double *)&cfg.estd_segment_usec, ESTD_SEGMENT_USEC },

	{ str_ic, _print_ui8, _get_ui8, _set_ic,  (double *)&cfg.ignore_cr,   COM_IGNORE_RX_CR },
	{ str_il, _print_ui8, _get_ui8, _set_il,  (double *)&cfg.ignore_lf,   COM_IGNORE_RX_LF },
	{ str_ec, _print_ui8, _get_ui8, _set_ec,  (double *)&cfg.enable_cr,   COM_APPEND_TX_CR },
	{ str_ee, _print_ui8, _get_ui8, _set_ee,  (double *)&cfg.enable_echo, COM_ENABLE_ECHO },
	{ str_ex, _print_ui8, _get_ui8, _set_ex,  (double *)&cfg.enable_xon,  COM_ENABLE_XON },

	{ str_1ma, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.m[MOTOR_1].motor_map,  M1_MOTOR_MAP },
	{ str_1sa, _print_rot, _get_dbl ,_set_sa, (double *)&cfg.m[MOTOR_1].step_angle, M1_STEP_ANGLE },
	{ str_1tr, _print_lin, _get_dbl ,_set_sa, (double *)&cfg.m[MOTOR_1].travel_rev, M1_TRAVEL_PER_REV },
	{ str_1mi, _print_ui8, _get_ui8, _set_mi, (double *)&cfg.m[MOTOR_1].microsteps, M1_MICROSTEPS },
	{ str_1po, _print_ui8, _get_ui8, _set_po, (double *)&cfg.m[MOTOR_1].polarity,   M1_POLARITY },
	{ str_1pm, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.m[MOTOR_1].power_mode, M1_POWER_MODE },

	{ str_2ma, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.m[MOTOR_2].motor_map,  M2_MOTOR_MAP },
	{ str_2sa, _print_rot, _get_dbl, _set_sa, (double *)&cfg.m[MOTOR_2].step_angle, M2_STEP_ANGLE },
	{ str_2tr, _print_lin, _get_dbl, _set_sa, (double *)&cfg.m[MOTOR_2].travel_rev, M2_TRAVEL_PER_REV },
	{ str_2mi, _print_ui8, _get_ui8, _set_mi, (double *)&cfg.m[MOTOR_2].microsteps, M2_MICROSTEPS },
	{ str_2po, _print_ui8, _get_ui8, _set_po, (double *)&cfg.m[MOTOR_2].polarity,   M2_POLARITY },
	{ str_2pm, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.m[MOTOR_2].power_mode, M2_POWER_MODE },

	{ str_3ma, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.m[MOTOR_3].motor_map,  M3_MOTOR_MAP },
	{ str_3sa, _print_rot, _get_dbl, _set_sa, (double *)&cfg.m[MOTOR_3].step_angle, M3_STEP_ANGLE },
	{ str_3tr, _print_lin, _get_dbl, _set_sa, (double *)&cfg.m[MOTOR_3].travel_rev, M3_TRAVEL_PER_REV },
	{ str_3mi, _print_ui8, _get_ui8, _set_mi, (double *)&cfg.m[MOTOR_3].microsteps, M3_MICROSTEPS },
	{ str_3po, _print_ui8, _get_ui8, _set_po, (double *)&cfg.m[MOTOR_3].polarity,   M3_POLARITY },
	{ str_3pm, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.m[MOTOR_3].power_mode, M3_POWER_MODE },

	{ str_4ma, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.m[MOTOR_4].motor_map,  M4_MOTOR_MAP },
	{ str_4sa, _print_rot, _get_dbl, _set_sa, (double *)&cfg.m[MOTOR_4].step_angle, M4_STEP_ANGLE },
	{ str_4tr, _print_lin, _get_dbl, _set_sa, (double *)&cfg.m[MOTOR_4].travel_rev, M4_TRAVEL_PER_REV },
	{ str_4mi, _print_ui8, _get_ui8, _set_mi, (double *)&cfg.m[MOTOR_4].microsteps, M4_MICROSTEPS },
	{ str_4po, _print_ui8, _get_ui8, _set_po, (double *)&cfg.m[MOTOR_4].polarity,   M4_POLARITY },
	{ str_4pm, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.m[MOTOR_4].power_mode, M4_POWER_MODE },

	{ str_xam, _print_am,  _get_am,  _set_ui8,(double *)&cfg.a[X].axis_mode,		X_AXIS_MODE },
	{ str_xfr, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[X].feedrate_max,		X_FEEDRATE_MAX },
	{ str_xvm, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[X].velocity_max,		X_VELOCITY_MAX },
	{ str_xtm, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[X].travel_max,		X_TRAVEL_MAX },
	{ str_xjm, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[X].jerk_max,			X_JERK_MAX },
	{ str_xjd, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[X].junction_dev,		X_JUNCTION_DEVIATION },
	{ str_xsm, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.a[X].switch_mode,		X_SWITCH_MODE },
	{ str_xsv, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[X].search_velocity,	X_SEARCH_VELOCITY },
	{ str_xlv, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[X].latch_velocity,	X_LATCH_VELOCITY },
	{ str_xzo, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[X].zero_offset,		X_ZERO_OFFSET },
	{ str_xabs,_print_lin, _get_abs, _set_nul,(double *)&tg.null, 0 },	// x absolute machine position
	{ str_xpos,_print_lin, _get_pos, _set_nul,(double *)&tg.null, 0 },	// x work position

	{ str_yam, _print_am,  _get_am,  _set_ui8,(double *)&cfg.a[Y].axis_mode,		Y_AXIS_MODE },
	{ str_yfr, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Y].feedrate_max,		Y_FEEDRATE_MAX },
	{ str_yvm, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Y].velocity_max,		Y_VELOCITY_MAX },
	{ str_ytm, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Y].travel_max,		Y_TRAVEL_MAX },
	{ str_yjm, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Y].jerk_max,			Y_JERK_MAX },
	{ str_yjd, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Y].junction_dev,		Y_JUNCTION_DEVIATION },
	{ str_ysm, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.a[Y].switch_mode,		Y_SWITCH_MODE },
	{ str_ysv, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Y].search_velocity,	Y_SEARCH_VELOCITY },
	{ str_ylv, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Y].latch_velocity,	Y_LATCH_VELOCITY },
	{ str_yzo, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Y].zero_offset,		Y_ZERO_OFFSET },
	{ str_yabs,_print_lin, _get_abs, _set_nul,(double *)&tg.null, 0 },	// y machine position
	{ str_ypos,_print_lin, _get_pos, _set_nul,(double *)&tg.null, 0 },	// y work position

	{ str_zam, _print_am,  _get_am,  _set_ui8,(double *)&cfg.a[Z].axis_mode,		Z_AXIS_MODE },
	{ str_zfr, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Z].feedrate_max, 	Z_FEEDRATE_MAX },
	{ str_zvm, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Z].velocity_max, 	Z_VELOCITY_MAX },
	{ str_ztm, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Z].travel_max,		Z_TRAVEL_MAX },
	{ str_zjm, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Z].jerk_max,			Z_JERK_MAX },
	{ str_zjd, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Z].junction_dev, 	Z_JUNCTION_DEVIATION },
	{ str_zsm, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.a[Z].switch_mode,		Z_SWITCH_MODE },
	{ str_zsv, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Z].search_velocity,	Z_SEARCH_VELOCITY },
	{ str_zlv, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Z].latch_velocity,	Z_LATCH_VELOCITY },
	{ str_zzo, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.a[Z].zero_offset,		Z_ZERO_OFFSET },
	{ str_zabs,_print_lin, _get_abs, _set_nul,(double *)&tg.null, 0 },	// z machine position
	{ str_zpos,_print_lin, _get_pos, _set_nul,(double *)&tg.null, 0 },	// z work position

	{ str_aam, _print_am,  _get_am,  _set_ui8,(double *)&cfg.a[A].axis_mode,		A_AXIS_MODE },
	{ str_afr, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[A].feedrate_max, 	A_FEEDRATE_MAX },
	{ str_avm, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[A].velocity_max, 	A_VELOCITY_MAX },
	{ str_atm, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[A].travel_max,		A_TRAVEL_MAX },
	{ str_ajm, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[A].jerk_max,			A_JERK_MAX },
	{ str_ajd, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[A].junction_dev, 	A_JUNCTION_DEVIATION },
	{ str_ara, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[A].radius,			A_RADIUS},
	{ str_asm, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.a[A].switch_mode,		A_SWITCH_MODE },
	{ str_asv, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[A].search_velocity,	A_SEARCH_VELOCITY },
	{ str_alv, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[A].latch_velocity,	A_LATCH_VELOCITY },
	{ str_azo, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[A].zero_offset,		A_ZERO_OFFSET },
	{ str_aabs,_print_rot, _get_abs, _set_nul,(double *)&tg.null, 0 },	// a machine position
	{ str_apos,_print_rot, _get_pos, _set_nul,(double *)&tg.null, 0 },	// a work position

	{ str_bam, _print_am,  _get_am,  _set_ui8,(double *)&cfg.a[B].axis_mode,		B_AXIS_MODE },
	{ str_bfr, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[B].feedrate_max, 	B_FEEDRATE_MAX },
	{ str_bvm, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[B].velocity_max, 	B_VELOCITY_MAX },
	{ str_btm, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[B].travel_max,		B_TRAVEL_MAX },
	{ str_bjm, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[B].jerk_max,			B_JERK_MAX },
	{ str_bjd, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[B].junction_dev, 	B_JUNCTION_DEVIATION },
	{ str_bra, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[B].radius,			B_RADIUS },
	{ str_bsm, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.a[B].switch_mode,		B_SWITCH_MODE },
	{ str_bsv, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[B].search_velocity,	B_SEARCH_VELOCITY },
	{ str_blv, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[B].latch_velocity,	B_LATCH_VELOCITY },
	{ str_bzo, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[B].zero_offset,		B_ZERO_OFFSET },
	{ str_babs,_print_rot, _get_abs, _set_nul,(double *)&tg.null, 0 },	// b machine position
	{ str_bpos,_print_rot, _get_pos, _set_nul,(double *)&tg.null, 0 },	// b work position

	{ str_cam, _print_am,  _get_am,  _set_ui8,(double *)&cfg.a[C].axis_mode,		C_AXIS_MODE },
	{ str_cfr, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[C].feedrate_max, 	C_FEEDRATE_MAX },
	{ str_cvm, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[C].velocity_max, 	C_VELOCITY_MAX },
	{ str_ctm, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[C].travel_max,		C_TRAVEL_MAX },
	{ str_cjm, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[C].jerk_max,			C_JERK_MAX },
	{ str_cjd, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[C].junction_dev,		C_JUNCTION_DEVIATION },
	{ str_cra, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[C].radius,			C_RADIUS },
	{ str_csm, _print_ui8, _get_ui8, _set_ui8,(double *)&cfg.a[C].switch_mode,		C_SWITCH_MODE },
	{ str_csv, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[C].search_velocity,	C_SEARCH_VELOCITY },
	{ str_clv, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[C].latch_velocity,	C_LATCH_VELOCITY },
	{ str_czo, _print_rot, _get_dbl, _set_dbl,(double *)&cfg.a[C].zero_offset,		C_ZERO_OFFSET },
	{ str_cabs,_print_rot, _get_abs, _set_nul,(double *)&tg.null, 0 },	// c machine position
	{ str_cpos,_print_rot, _get_pos, _set_nul,(double *)&tg.null, 0 },	// c work position

	// coordinate system offsets
	{ str_g54x, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G54][X], G54_X_OFFSET },
	{ str_g54y, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G54][Y], G54_Y_OFFSET },
	{ str_g54z, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G54][Z], G54_Z_OFFSET },
	{ str_g54a, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G54][A], G54_A_OFFSET },
	{ str_g54b, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G54][B], G54_B_OFFSET },
	{ str_g54c, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G54][C], G54_C_OFFSET },

	{ str_g55x, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G55][X], G55_X_OFFSET },
	{ str_g55y, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G55][Y], G55_Y_OFFSET },
	{ str_g55z, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G55][Z], G55_Z_OFFSET },
	{ str_g55a, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G55][A], G55_A_OFFSET },
	{ str_g55b, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G55][B], G55_B_OFFSET },
	{ str_g55c, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G55][C], G55_C_OFFSET },

	{ str_g56x, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G56][X], G56_X_OFFSET },
	{ str_g56y, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G56][Y], G56_Y_OFFSET },
	{ str_g56z, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G56][Z], G56_Z_OFFSET },
	{ str_g56a, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G56][A], G56_A_OFFSET },
	{ str_g56b, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G56][B], G56_B_OFFSET },
	{ str_g56c, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G56][C], G56_C_OFFSET },

	{ str_g57x, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G57][X], G57_X_OFFSET },
	{ str_g57y, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G57][Y], G57_Y_OFFSET },
	{ str_g57z, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G57][Z], G57_Z_OFFSET },
	{ str_g57a, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G57][A], G57_A_OFFSET },
	{ str_g57b, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G57][B], G57_B_OFFSET },
	{ str_g57c, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G57][C], G57_C_OFFSET },

	{ str_g58x, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G58][X], G58_X_OFFSET },
	{ str_g58y, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G58][Y], G58_Y_OFFSET },
	{ str_g58z, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G58][Z], G58_Z_OFFSET },
	{ str_g58a, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G58][A], G58_A_OFFSET },
	{ str_g58b, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G58][B], G58_B_OFFSET },
	{ str_g58c, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G58][C], G58_C_OFFSET },

	{ str_g59x, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G59][X], G59_X_OFFSET },
	{ str_g59y, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G59][Y], G59_Y_OFFSET },
	{ str_g59z, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G59][Z], G59_Z_OFFSET },
	{ str_g59a, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G59][A], G59_A_OFFSET },
	{ str_g59b, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G59][B], G59_B_OFFSET },
	{ str_g59c, _print_lin, _get_dbu, _set_dbu,(double *)&cfg.offset[G59][C], G59_C_OFFSET },

	// persistence for status report - must be in sequence
	{ str_sr00, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[0],0 },
	{ str_sr01, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[1],0 },
	{ str_sr02, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[2],0 },
	{ str_sr03, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[3],0 },
	{ str_sr04, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[4],0 },
	{ str_sr05, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[5],0 },
	{ str_sr06, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[6],0 },
	{ str_sr07, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[7],0 },
	{ str_sr08, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[8],0 },
	{ str_sr09, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[9],0 },
	{ str_sr10, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[10],0 },
	{ str_sr11, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[11],0 },
	{ str_sr12, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[12],0 },
	{ str_sr13, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[13],0 },
	{ str_sr14, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[14],0 },
	{ str_sr15, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[15],0 },
	{ str_sr16, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[16],0 },
	{ str_sr17, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[17],0 },
	{ str_sr18, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[18],0 },
	{ str_sr19, _print_nul, _get_int, _set_int,(double *)&cfg.status_report_spec[19],0 },

	// group lookups - must follow the single-valued entries for proper sub-string matching
	{ str_g54, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },	// coord offset groups
	{ str_g55, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_g56, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_g57, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_g58, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_g59, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_sys, _print_grp, _get_sys, _set_grp,(double *)&tg.null,0 },	// system group
	{ str_qm, _print_grp, _get_qm, _set_nul,(double *)&tg.null,0 },	// question mark report
	{ str_x, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },		// axis groups
	{ str_y, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_z, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_a, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_b, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_c, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_1, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },		// motor groups
	{ str_2, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_3, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 },
	{ str_4, _print_grp, _get_grp, _set_grp,(double *)&tg.null,0 }
};
#define CMD_INDEX_MAX (sizeof cfgArray / sizeof(struct cfgItem))

// hack alert. Find a better way to do this
#define CMD_COUNT_STATUS 20
#define CMD_COUNT_GROUPS 17
#define CMD_INDEX_END_SINGLES (CMD_INDEX_MAX - CMD_COUNT_STATUS - CMD_COUNT_GROUPS)
#define CMD_INDEX_START_GROUPS (CMD_INDEX_MAX - CMD_COUNT_GROUPS)
//#define NVM_STATUS_REPORT (CMD_MAX_INDEX * NVM_RECORD_LEN)	// location of status report in NVM

/**** Specialized internal functions ****************************************/
/* _get_stat() - get machine state as value and string
 * _get_vel()  - get current velocity
 * _get_abs()  - get current machine position
 * _get_pos()  - get current work position
 * _get_unit() - get units as a string for status report
 * _run_gc() - launch the gcode parser on a block of gcode
 */
static char msg_ms0[] PROGMEM = "reset";		// used for status reports
static char msg_ms1[] PROGMEM = "run";
static char msg_ms2[] PROGMEM = "stop";
static char msg_ms3[] PROGMEM = "hold";
static char msg_ms4[] PROGMEM = "resume";
static char msg_ms5[] PROGMEM = "homing";
static PGM_P msg_ms[] PROGMEM = { msg_ms0, msg_ms1, msg_ms2, msg_ms3, msg_ms4, msg_ms5 };

static uint8_t _get_stat(const INDEX_T i, cmdObj *cmd)
{
	_get_ui8(i,cmd);
//	cmd->value = (double)*((uint8_t *)pgm_read_word(&cfgArray[i].target));
	strncpy_P(cmd->string,(PGM_P)pgm_read_word(&msg_ms[(uint8_t)cmd->value]),CMD_STRING_LEN);
	cmd->value_type = VALUE_TYPE_STRING;
	return (TG_OK);
}

static uint8_t _get_vel(const INDEX_T i, cmdObj *cmd) 
{
	cmd->value = mp_get_runtime_velocity();
	if (cm_get_units_mode() == INCHES_MODE) cmd->value *= INCH_PER_MM;
	cmd->value_type = VALUE_TYPE_FLOAT;
	return (TG_OK);
}

static uint8_t _get_abs(const INDEX_T i, cmdObj *cmd) 
{
	cmd->value = cm_get_machine_runtime_position(_get_axis(i));
//	if (cm_get_units_mode() == INCHES_MODE) cmd->value *= INCH_PER_MM;
	cmd->value_type = VALUE_TYPE_FLOAT;
	return (TG_OK);
}

static uint8_t _get_pos(const INDEX_T i, cmdObj *cmd) 
{
	cmd->value = cm_get_work_runtime_position(_get_axis(i));
//	if (cm_get_units_mode() == INCHES_MODE) cmd->value *= INCH_PER_MM;
	cmd->value_type = VALUE_TYPE_FLOAT;
	return (TG_OK);
}

static uint8_t _get_gc(const INDEX_T i, cmdObj *cmd)
{
	strncpy(cmd->string, tg.in_buf, CMD_STRING_LEN);
	cmd->value_type = VALUE_TYPE_STRING;
	return (TG_OK);
}

static uint8_t _run_gc(const INDEX_T i, cmdObj *cmd)
{
	strncpy(tg.in_buf, cmd->string, INPUT_BUFFER_LEN);
	cmd->status = gc_gcode_parser(tg.in_buf);
	tg_make_json_gcode_response(cmd->status, tg.in_buf, tg.out_buf);
	return (TG_OK);
}

static char msg_un0[] PROGMEM = "inch";
static char msg_un1[] PROGMEM = "mm";
static char msg_un2[] PROGMEM = "deg";
static PGM_P msg_un[] PROGMEM = { msg_un0, msg_un1, msg_un2 };

static uint8_t _get_unit(const INDEX_T i, cmdObj *cmd)
{
	_get_ui8(i,cmd);
//	cmd->value = (double)*((uint8_t *)pgm_read_word(&cfgArray[i].target));
	strncpy_P(cmd->string,(PGM_P)pgm_read_word(&msg_un[(uint8_t)cmd->value]),CMD_STRING_LEN);
	cmd->value_type = VALUE_TYPE_STRING;
	return (TG_OK);
}

/**** STATUS REPORT FUNCTIONS ****
 * _get_sr() - run status report
 * _set_sr() - set status report specification
 * _get_si() - get status report interval
 * _set_si() - set status report interval
 *
 *	Note: _get_sr() is called during initialization and during reset when 
 *	there's actually nothing to do. So it rejects all get requests except 
 *	those where cmd->value_type == true and cmd->value == true.
 */
static uint8_t _get_sr(const INDEX_T i, cmdObj *cmd) 
{
	rpt_json_status_report();
	return (TG_OK);
}

static uint8_t _set_sr(const INDEX_T i, cmdObj *cmd) 
{
	memset(cfg.status_report_spec, 0 , sizeof(cfg.status_report_spec));
	for (uint8_t j=0; j<CMD_STATUS_REPORT_LEN; j++) {
		cmd++;
		if ((cmd->value_type == true) && (cmd->value == true)) { // see function header note
			cfg.status_report_spec[j] = cmd->index;
		}
		if (cmd->nx == NULL) break;
	}
	return (TG_OK);
}

static uint8_t _set_si(const INDEX_T i, cmdObj *cmd) 
{
	if (cmd->value < STATUS_REPORT_MIN_MS) {
		cmd->value = STATUS_REPORT_MIN_MS;
	} else if (cmd->value > STATUS_REPORT_MAX_MS) {
		cmd->value = STATUS_REPORT_MAX_MS;
	}
	// convert value to segment timing
	cfg.status_report_interval = (uint8_t)ceil(cmd->value / (ESTD_SEGMENT_USEC / 1000));
	return (TG_OK);
}

static uint8_t _get_si(const INDEX_T i, cmdObj *cmd) 
{
	_get_ui8(i,cmd);
	cmd->value *= (ESTD_SEGMENT_USEC / 1000);
	return (TG_OK);
}

/**** MOTOR FUNCTIONS ****
 * _set_sa() - set step_angle or travel_per_rev & recompute steps_per_unit
 * _set_mi() - set microsteps & recompute steps_per_unit
 * _set_po() - set polarity and update stepper structs
 * _set_motor_steps_per_unit() - update this derived value
 *		This function will need to be rethought if microstep morphing is implemented, 
 */
static uint8_t _set_sa(const INDEX_T i, cmdObj *cmd)
{ 
	_set_dbl(i, cmd);
	_set_motor_steps_per_unit(i, cmd); 
	return (TG_OK);
}

static uint8_t _set_mi(const INDEX_T i, cmdObj *cmd)
{
	_set_ui8(i, cmd);
	_set_motor_steps_per_unit(i, cmd);
	st_set_microsteps(_get_motor(i), (uint8_t)cmd->value);
	return (TG_OK);
}

static uint8_t _set_po(const INDEX_T i, cmdObj *cmd)
{ 
	_set_ui8(i, cmd);
	st_set_polarity(_get_motor(i), (uint8_t)cmd->value);
	return (TG_OK);
}

static uint8_t _set_motor_steps_per_unit(const INDEX_T i, cmdObj *cmd)
{
	uint8_t m = _get_motor(i);
	cfg.m[m].steps_per_unit = (360 / (cfg.m[m].step_angle / cfg.m[m].microsteps) / cfg.m[m].travel_rev);
	return (TG_OK);
}

/**** AXIS FUNCTIONS ****
 * _get_am()	- get axis mode w/enumeration string
 * _print_am()	- print axis mode w/enumeration string
 */
static char msg_am00[] PROGMEM = "[disabled]";
static char msg_am01[] PROGMEM = "[standard]";
static char msg_am02[] PROGMEM = "[inhibited]";
static char msg_am03[] PROGMEM = "[radius]";
static char msg_am04[] PROGMEM = "[slave X]";
static char msg_am05[] PROGMEM = "[slave Y]";
static char msg_am06[] PROGMEM = "[slave Z]";
static char msg_am07[] PROGMEM = "[slave XY]";
static char msg_am08[] PROGMEM = "[slave XZ]";
static char msg_am09[] PROGMEM = "[slave YZ]";
static char msg_am10[] PROGMEM = "[slave XYZ]";
static PGM_P msg_am[] PROGMEM = {
	msg_am00, msg_am01, msg_am02, msg_am03, msg_am04, msg_am05, 
	msg_am06, msg_am07, msg_am08, msg_am09, msg_am10
};

static uint8_t _get_am(const INDEX_T i, cmdObj *cmd)
{
	_get_ui8(i,cmd);
//	cmd->value = (double)*((uint8_t *)pgm_read_word(&cfgArray[i].target));
	strncpy_P(cmd->string,(PGM_P)pgm_read_word(&msg_am[(uint8_t)cmd->value]),CMD_STRING_LEN);
	cmd->value_type = VALUE_TYPE_INT32;
	return (TG_OK);
}

static void _print_am(const INDEX_T i)
{
	char format[CMD_FORMAT_LEN+1];
	uint8_t m = _get_ui8_value(i);
	fprintf(stderr, _get_format(i,format), m, (PGM_P)pgm_read_word(&msg_am[m]));
}

/**** SERIAL IO FUNCTIONS ****
 * _set_ic() - ignore cr on RX
 * _set_il() - ignore lf on RX
 * _set_ec() - enable CRLF on TX
 * _set_ee() - enable character echo
 * _set_ex() - enable XON/XOFF
 *
 *	The above assume USB is the std device
 */
static uint8_t _set_ic(const INDEX_T i, cmdObj *cmd) 
{
	if (NE_ZERO(cmd->value)) { 
		(void)xio_cntl(XIO_DEV_USB, XIO_IGNORECR);
	} else { 
		(void)xio_cntl(XIO_DEV_USB, XIO_NOIGNORECR);
	}
	cfg.ignore_cr = (uint8_t)cmd->value;
	return (TG_OK);
}

static uint8_t _set_il(const INDEX_T i, cmdObj *cmd) 
{
	if (NE_ZERO(cmd->value)) { 
		(void)xio_cntl(XIO_DEV_USB, XIO_IGNORELF);
	} else {
		(void)xio_cntl(XIO_DEV_USB, XIO_NOIGNORELF);
	}
	cfg.ignore_lf = (uint8_t)cmd->value;
	return (TG_OK);
}

static uint8_t _set_ec(const INDEX_T i, cmdObj *cmd) 
{
	if (NE_ZERO(cmd->value)) { 
		(void)xio_cntl(XIO_DEV_USB, XIO_CRLF);
	} else {
		(void)xio_cntl(XIO_DEV_USB, XIO_NOCRLF);
	}
	cfg.enable_cr = (uint8_t)cmd->value;
	return (TG_OK);
}

static uint8_t _set_ee(const INDEX_T i, cmdObj *cmd) 
{
	if (NE_ZERO(cmd->value)) { 
		(void)xio_cntl(XIO_DEV_USB, XIO_ECHO);
	} else {
		(void)xio_cntl(XIO_DEV_USB, XIO_NOECHO);
	}
	cfg.enable_echo = (uint8_t)cmd->value;
	return (TG_OK);
}

static uint8_t _set_ex(const INDEX_T i, cmdObj *cmd)
{
	if (NE_ZERO(cmd->value)) { 
		(void)xio_cntl(XIO_DEV_USB, XIO_XOFF);
	} else {
		(void)xio_cntl(XIO_DEV_USB, XIO_NOXOFF);
	}
	cfg.enable_xon = (uint8_t)cmd->value;
	return (TG_OK);
}

/*****************************************************************************
 *** END SETTING-SPECIFIC REGION *********************************************
 *** Code below should not require changes as parameters are added/updated ***
 *****************************************************************************/

/****************************************************************************
 * cfg_init() - called once on system init
 *
 *	Will perform one of 2 actions:
 *	(1) if NVM is set up and at current config version: use NVM data for config
 *	(2) if NVM is set up or out-of-rev: load RAM and NVM with hardwired default settings
 */

void cfg_init() 
{
	INDEX_T i = 0;
	cmdObj cmd;
	char exclusions[] = {"sr,gc"};	// don't try to SET these tokens

	cm_set_units_mode(MILLIMETER_MODE);	// must do init in MM mode

#ifdef __DISABLE_EEPROM_INIT		// cutout for debug simulation
	// Apply the hard-coded default values from settings.h and exit
	for (i=0; i<CMD_INDEX_START_GROUPS; i++) {
		if (strstr(exclusions, cmd_get_token(i, cmd.token)) != NULL) continue;
		cmd.value = (double)pgm_read_float(&cfgArray[i].def_value);
		cmd_set(i, &cmd);
	}
	rpt_init_status_report(false);	// requires special treatment (persist = false)
#else
	cfg.nvm_base_addr = NVM_BASE_ADDR;
	cfg.nvm_profile_base = cfg.nvm_base_addr;
	cfg.version = tg.build;			// use the build number as the config version
	cmd_read_NVM_value(0, &cmd);	// read the first record in NVM

	if (cmd.value == cfg.version) { // Case (1) NVM is set up and current revision. Load config from NVM
		fprintf_P(stderr,PSTR("Loading configs from EEPROM\n"));
		for (i=0; i<CMD_INDEX_START_GROUPS; i++) {
			cmd_read_NVM_value(i, &cmd);
			cmd_get_token(i, cmd.token);
			if (strstr(exclusions, cmd.token) != NULL) continue;
			cmd_set(i, &cmd);
		}
	} else {  // Case (2) NVM is out-of-rev or not set up. Use the defaults and set up NVM
		fprintf_P(stderr,PSTR("Initializing configs to default values\n"));
		for (i=0; i<CMD_INDEX_START_GROUPS; i++) {
			cmd_new_object(&cmd);
			cmd_get_token(i, cmd.token);
			if (strstr(exclusions, cmd.token) != NULL) continue;
			cmd.index = i;
			cmd.value = (double)pgm_read_float(&cfgArray[i].def_value);
			cmd_set(i, &cmd);
			if (cmd_write_NVM_value(i, &cmd) != TG_OK) {
				INFO(PSTR("Failed to update NVM in cfg_init()"));
			}
			fprintf_P(stderr,PSTR("."));
		}
		rpt_init_status_report(true);// requires special treatment (persist = true)
	}
	fprintf_P(stderr,PSTR("\n"));
#endif
}

/****************************************************************************
 * cfg_config_parser() - update a config setting from a text block
 * 
 * Use cases (execution paths handled)
 *	- $xfr=1200	single parameter set is requested
 *	- $xfr		single parameter display is requested
 *	- $x		group display is requested
 */

uint8_t cfg_config_parser(char *str)
{
	cmdObj *cmd = &cmd_array[0];			// point at first struct in the array

	ritorno(_parse_config_string(str,cmd));	// get the first object
	if ((cmd->value_type != VALUE_TYPE_PARENT) && (cmd->value_type != VALUE_TYPE_NULL)) {
		cmd_set(cmd->index, cmd);			// set single value
		cmd_write_NVM_value(cmd->index,cmd);// persist value
	}
	cmd_print(cmd->index);					// print value(s)
	return (TG_OK);
}

/****************************************************************************
 * _parse_config_string() - parse a command line
 */

static uint8_t _parse_config_string(char *str, cmdObj *cmd)
{
	char *tmp;
	char separators[] = {" =:|\t"};			// anything someone might use

	// pre-processing
	cmd_new_object(cmd);					// initialize config object
	if (*str == '$') str++;					// ignore leading $
	tmp = str;
	for (; *tmp!=NUL; tmp++) {
		*tmp = tolower(*tmp);				// convert string to lower case
		// todo: put comma tolerance in here
		// todo: insert separator for xfr1000 case in here
	}

	// field processing
	cmd->value_type = VALUE_TYPE_NULL;
	if ((tmp = strpbrk(str, separators)) == NULL) {
		strncpy(cmd->name, str, CMD_NAME_LEN);// no value part
	} else {
		*tmp = NUL;							// terminate at end of name
		strncpy(cmd->name, str, CMD_NAME_LEN);
		str = ++tmp;
		cmd->value = strtod(str, &tmp);		// tmp is the end pointer
		if (tmp != str) {
			cmd->value_type = VALUE_TYPE_FLOAT;
		}
	}
	if ((cmd->index = cmd_get_index(cmd->name)) == -1) { 
		return (TG_UNRECOGNIZED_COMMAND);
	}
	cmd_get_token(cmd->index, cmd->token);
	if (cmd->index >= CMD_INDEX_START_GROUPS) {
		cmd->value_type = VALUE_TYPE_PARENT;// indicating it's a group token
	}
	return (TG_OK);
}

/****************************************************************************/
/**** CMD FUNCTIONS *********************************************************/
/**** These are the primary external access points to cmd functions *********
 * cmd_get()   - get a value from the target - in external format
 * cmd_set()   - set a value or invoke a function
 * cmd_print() - invoke print function
 */
uint8_t cmd_get(const INDEX_T i, cmdObj *cmd)
{
	if ((i < 0) || (i >= CMD_INDEX_MAX)) { 
		cmd->status = TG_UNRECOGNIZED_COMMAND;
		return (cmd->status);
	}
	return (((fptrCmd)(pgm_read_word(&cfgArray[i].get)))(i,cmd));
}

uint8_t cmd_set(const INDEX_T i, cmdObj *cmd)
{
	if ((i < 0) || (i >= CMD_INDEX_MAX)) { 
		cmd->status = TG_UNRECOGNIZED_COMMAND;
		return (cmd->status);
	}
	return (((fptrCmd)(pgm_read_word(&cfgArray[i].set)))(i,cmd));
}

void cmd_print(INDEX_T i)
{
	if ((i < 0) || (i >= CMD_INDEX_MAX)) return;
	((fptrConfig)(pgm_read_word(&cfgArray[i].print)))(i);
}

/****************************************************************************
 * Secondary cmd functions
 * cmd_get_max_index()		- utility function to return array size				
 * cmd_get_cmd()			- like cmd_get but populates the entire cmdObj struct
 * cmd_new_object() 		- initialize a command object (that you actually passed in)
 * cmd_get_index_by_token() - get index from mnenonic token (most efficient scan)
 * cmd_get_index() 			- get index from mnenonic token or friendly name
 * cmd_get_token()			- returns token in arg string & returns pointer to string
 * cmd_get_group() 			- returns the axis prefix, motor prefix, or 'g' for general
 *
 *	cmd_get_index() and cmd_get_index_by_token() are the most expensive routines 
 *	in the whole mess. They do a linear table scan of the PROGMEM strings, and
 *	could of course be further optimized. Use cmd_get_index_by_token() if you 
 *	know your input string is a token - it's 10x faster than cmd_get_index()
 *
 *	The full string is not needed in the friendly name, just enough to match to
 *	uniqueness. This saves a fair amount of memory and time and is easier to use.
 */
INDEX_T cmd_get_max_index() { return (CMD_INDEX_MAX);}

uint8_t cmd_get_cmd(const INDEX_T i, cmdObj *cmd)
{
	cmd_new_object(cmd);
	if ((i < 0) || (i >= CMD_INDEX_MAX)) { 
		cmd->status = TG_UNRECOGNIZED_COMMAND;
		return (cmd->status);
	}
	cmd->index = i;
	cmd_get_token(i, cmd->token);
	((fptrCmd)(pgm_read_word(&cfgArray[i].get)))(i,cmd);
	return (cmd->value);
}

struct cmdObject *cmd_new_object(struct cmdObject *cmd)
{
	memset(cmd, 0, sizeof(struct cmdObject));
	cmd->value_type = VALUE_TYPE_NULL;
	return (cmd);
}

INDEX_T cmd_get_index_by_token(const char *str)
{
	char token[CMD_TOKEN_LEN+1]; // filled from cfg string; terminates w/comma, not NUL

	for (INDEX_T i=0; i<CMD_INDEX_MAX; i++) {
		strncpy_P(token,(PGM_P)pgm_read_word(&cfgArray[i]), CMD_TOKEN_LEN);
		if ((token[0] != str[0]) || (token[1] != str[1])) continue;	// first 2 chars mismatch
		if ((token[2] == ',') && (str[2] == NUL)) return (i);		// 2 char match
		if (token[2] != str[2]) continue;							// 3 char mismatch
		if ((token[3] == ',') && (str[3] == NUL)) return (i);		// 2 char match
		if (token[3] != str[3]) continue;							// 4 char mismatch
		return (i);													// 4 char match
	}
	return (-1);							// no match
}

INDEX_T cmd_get_index(const char *str)
{
	char *name;								// pointer to friendly name
	char *end;
	char token[CMD_NAMES_FIELD_LEN];

	for (INDEX_T i=0; i<CMD_INDEX_MAX; i++) {
		strncpy_P(token,(PGM_P)pgm_read_word(&cfgArray[i]), CMD_NAMES_FIELD_LEN);
		name = strstr(token,",");			// find the separating comma
		*(name++) = NUL;					// split the token and name strings
		end = strstr(name,",");				// find the terminating comma
		*end = NUL;
		if (strstr(str, token) == str) return(i);
		if (strstr(str, name) == str) return(i);
	}
	return (-1);							// no match
}

char *cmd_get_token(const INDEX_T i, char *token)
{
	if ((i < 0) || (i >= CMD_INDEX_MAX)) { 
		*token = NUL;
		return (token);
	}
	strncpy_P(token,(PGM_P)pgm_read_word(&cfgArray[i].string), CMD_TOKEN_LEN+1);
	char *ptr = strstr(token,",");			// find the first separating comma
	*ptr = NUL;								// terminate the string after the token
	return (token);
}

char cmd_get_group(const INDEX_T i)
{
	char *ptr;
	char chr;
	char groups[] = {"xyzabc1234"};

	if ((i < 0) || (i >= CMD_INDEX_MAX)) return (NUL);
	strncpy_P(&chr,(PGM_P)pgm_read_word(&cfgArray[i].string), 1);
	if ((ptr = strchr(groups, chr)) == NULL) {
		return ('g');
	}
	return (*ptr);
}

/***** Generic Internal Functions *******************************************
 * _set_nul() - set nothing (noop)
 * _set_ui8() - set value as uint8_t w/o unit conversion
 * _set_int() - set value as integer w/o unit conversion
 * _set_dbl() - set value as double w/o unit conversion
 * _set_dbu() - set value as double w/unit conversion
 *
 * _get_nul() - returns -1
 * _get_ui8() - get value as uint8_t w/o unit conversion
 * _get_int() - get value as integer w/o unit conversion
 * _get_dbl() - get value as double w/o unit conversion
 * _get_dbu() - get value as double w/unit conversion
 * _get_dbls() - get value as double with conversion to string
 *
 * _get_ui8_value() - return uint8_t value (no cmd struct involved)
 * _get_int_value() - return integer value (no cmd struct involved)
 * _get_dbl_value() - return double value (no cmd struct involved)
 * _get_dbu_value() - return double value with unit conversion
 *
 * _print_nul() - print nothing
 * _print_ui8() - print uint8_t value w/no units or unit conversion
 * _print_int() - print integer value w/no units or unit conversion
 * _print_dbl() - print double value w/no units or unit conversion
 * _print_lin() - print linear value with units and in/mm unit conversion
 * _print_rot() - print rotary value with units
 */

static uint8_t _set_nul(const INDEX_T i, cmdObj *cmd) { return (TG_OK);}

static uint8_t _set_ui8(const INDEX_T i, cmdObj *cmd)
{
	*((uint8_t *)pgm_read_word(&cfgArray[i].target)) = cmd->value;
	return (TG_OK);
}

static uint8_t _set_int(const INDEX_T i, cmdObj *cmd)
{
	*((uint32_t *)pgm_read_word(&cfgArray[i].target)) = cmd->value;
	return (TG_OK);
}

static uint8_t _set_dbl(const INDEX_T i, cmdObj *cmd)
{
	*((double *)pgm_read_word(&cfgArray[i].target)) = cmd->value;
	return (TG_OK);
}

static uint8_t _set_dbu(const INDEX_T i, cmdObj *cmd)
{
	if (cm_get_units_mode() == INCHES_MODE) {
		*((double *)pgm_read_word(&cfgArray[i].target)) = cmd->value * MM_PER_INCH;
	} else {
		*((double *)pgm_read_word(&cfgArray[i].target)) = cmd->value;
	}
	return (TG_OK);
}
/*
static uint8_t _get_nul(const INDEX_T i, cmdObj *cmd) 
{ 
	cmd->value_type = VALUE_TYPE_NULL;
	return (TG_OK);
}
*/
static uint8_t _get_ui8(const INDEX_T i, cmdObj *cmd)
{
	cmd->value = (double)*((uint8_t *)pgm_read_word(&cfgArray[i].target));
	cmd->value_type = VALUE_TYPE_INT32;
	return (TG_OK);
}

static uint8_t _get_int(const INDEX_T i, cmdObj *cmd)
{
	cmd->value = (double)*((uint32_t *)pgm_read_word(&cfgArray[i].target));
	cmd->value_type = VALUE_TYPE_INT32;
	return (TG_OK);
}

static uint8_t _get_dbl(const INDEX_T i, cmdObj *cmd)
{
	cmd->value = *((double *)pgm_read_word(&cfgArray[i].target));
	cmd->value_type = VALUE_TYPE_FLOAT;
	return (TG_OK);
}

static uint8_t _get_dbu(const INDEX_T i, cmdObj *cmd)
{
	cmd->value = *((double *)pgm_read_word(&cfgArray[i].target));
	if (cm_get_units_mode() == INCHES_MODE) {
		cmd->value *= INCH_PER_MM;
	}
	cmd->value_type = VALUE_TYPE_FLOAT;
	return (TG_OK);
}
/*
static uint8_t _get_dbls(const INDEX_T i, cmdObj *cmd)
{
	_get_dbl(i,cmd);
	sprintf(cmd->string, "%0.0f", cmd->value);
	cmd->value_type = VALUE_TYPE_STRING;
	return (TG_OK);
}
*/
static uint8_t _get_ui8_value(const INDEX_T i) 
{ 
	cmdObj cmd;
	(((fptrCmd)(pgm_read_word(&cfgArray[i].get)))(i, &cmd));
	return ((uint8_t)cmd.value);
}

static uint8_t _get_int_value(const INDEX_T i) 
{ 
	cmdObj cmd;
	(((fptrCmd)(pgm_read_word(&cfgArray[i].get)))(i, &cmd));
	return ((uint32_t)cmd.value);
}

static double _get_dbl_value(const INDEX_T i) 
{
	cmdObj cmd;
	(((fptrCmd)(pgm_read_word(&cfgArray[i].get)))(i, &cmd));
	return (cmd.value);
}

static double _get_dbu_value(const INDEX_T i) 
{
	cmdObj cmd;
	(((fptrCmd)(pgm_read_word(&cfgArray[i].get)))(i, &cmd));	// unit conversion is done by the GET dbu
	return (cmd.value);
}

static char msg_units0[] PROGMEM = " in";
static char msg_units1[] PROGMEM = " mm";
static char msg_units2[] PROGMEM = " deg";
static PGM_P msg_units[] PROGMEM = { msg_units0, msg_units1, msg_units2 };

static void _print_nul(const INDEX_T i) {}

static void _print_ui8(const INDEX_T i)
{
	char format[CMD_FORMAT_LEN+1];
	fprintf(stderr, _get_format(i,format), (uint8_t)_get_ui8_value(i));
}

static void _print_int(const INDEX_T i)
{
	char format[CMD_FORMAT_LEN+1];
	fprintf(stderr, _get_format(i,format), (uint32_t)_get_int_value(i));
}

static void _print_dbl(const INDEX_T i)
{
	char format[CMD_FORMAT_LEN+1];
	fprintf(stderr, _get_format(i,format), (double)_get_dbl_value(i));
}

static void _print_lin(const INDEX_T i)
{
	char format[CMD_FORMAT_LEN+1];
	fprintf(stderr, _get_format(i,format), _get_dbu_value(i), (PGM_P)pgm_read_word(&msg_units[cm_get_units_mode()]));
}

static void _print_rot(const INDEX_T i)
{
	char format[CMD_FORMAT_LEN+1];
	fprintf(stderr, _get_format(i,format), _get_dbl_value(i), (PGM_P)pgm_read_word(&msg_units[2]));
}


/***************************************************************************** 
 * more accessors and other functions
 * _get_format() - returns format string as above
 * _get_axis() 	 - returns the axis an index applies to or -1 if na
 * _get_motor()  - returns the axis an index applies to or -1 if na
 *
 *  NOTE: Axis and motor functions rely on the token naming conventions
 */
static char *_get_format(const INDEX_T i, char *format)
{
	char *ptr;
	char tmp[CMD_STRING_FIELD_LEN];

	strncpy_P(tmp,(PGM_P)pgm_read_word(&cfgArray[i].string), CMD_STRING_FIELD_LEN);
	ptr = strstr(tmp,",");					// find the first separating comma
	ptr = strstr(++ptr,",");				// find the second comma
	ptr++;
	while (*ptr == ' ') ptr++;				// find the first non-whitespace
	strncpy(format, ptr, CMD_FORMAT_LEN+1);
	return (format);
}

static int8_t _get_axis(const INDEX_T i)
{
	char tmp;
	char *ptr;
	char axes[] = {"xyzabc"};

	strncpy_P(&tmp,(PGM_P)pgm_read_word(&cfgArray[i].string),1);
	if ((ptr = strchr(axes, tmp)) == NULL) {
		return (-1);
	}
	return (ptr - axes);
}

static int8_t _get_motor(const INDEX_T i)
{
	char *ptr;
	char motors[] = {"1234"};
	char tmp[CMD_TOKEN_LEN+1];

	strncpy_P(tmp,(PGM_P)pgm_read_word(&cfgArray[i].string), CMD_TOKEN_LEN+1);
	if ((ptr = strchr(motors, tmp[0])) == NULL) {
		return (-1);
	}
	return (ptr - motors);
}

uint8_t cmd_persist_offset(uint8_t coord_system, uint8_t axis, double offset)
{
	char token[CMD_TOKEN_LEN+1];
	char axes[AXES+1] = { "xyzabc" };

//	token[0] = axes[axis];
	sprintf(token, "%c%2d", axes[axis], 53+coord_system);

	return (TG_OK);
}


/**** Group Operations ****
 * _print_grp()		- print axis, motor, coordinate offset or system group
 * _set_grp()		- get data for axis, motor or coordinate offset group
 * _get_grp()		- get data for axis, motor or coordinate offset group
 * _get_sys()		- get data for system group
 *
 *	Group operations work on parent/child groups where the parent object is 
 *	one of the following groups:
 *	axis group 			x,y,z,a,b,c
 *	motor group			1,2,3,4
 *	coordinate group	g54,g55,g56,g57,g58,g59
 *	system group		"sys" - a collection of otherwise unrelated system variables
 *
 *	Groups are carried as parent / child objects, e.g:
 *	{"x":{"am":1,"fr":800,....}}		set all X axis parameters
 *	{"x":""}							get all X axis parameters
 *
 *	The group prefixes are stripped from the child tokens for better alignment 
 *	with host code. I.e a group object is represented as:
 *	{"x":{"am":1,"fr":800,....}}, not:
 *	{"x":{"xam":1,"xfr":800,....}},
 *
 *	This strip makes no difference for subsequent internal operations as the 
 *	index is used and tokens are ignored once the parameter index is known.
 *	But it's useful to be able to round-trip a group back to the JSON requestor.
 */
static void _print_grp(const INDEX_T i)
{
	cmdObj *cmd = &cmd_array[0];

	cmd_get(cmd->index, cmd);				// expand cmdArray for the group or sys
	for (uint8_t i=0; i<CMD_MAX_OBJECTS; i++) {
		if ((cmd = cmd->nx) == NULL) break;
		cmd_print(cmd->index);
	}
}

static uint8_t _set_grp(const INDEX_T i, cmdObj *cmd)
{
	for (uint8_t i=0; i<CMD_MAX_OBJECTS; i++) {
		if ((cmd = cmd->nx) == NULL) break;
		cmd_set(cmd->index, cmd);
	}
	return (TG_OK);
}

static uint8_t _get_grp(const INDEX_T i, cmdObj *cmd)
{
	char *grp = cmd->name;					// group token in the parent cmd object
	INDEX_T grp_index = cmd->index;
	char token[CMD_TOKEN_LEN+1];			// token retrived from cmdArray list

	cmd->value_type = VALUE_TYPE_PARENT;	// make first obj the parent 
	for (INDEX_T i=0; i<grp_index; i++) {	// stop before you recurse
		cmd_get_token(i,token);
		if (strstr(token, grp) == token) {
			cmd_get_cmd(i,++cmd);
			strncpy(cmd->token, &cmd->token[strlen(grp)], CMD_TOKEN_LEN+1);	// strip group prefixes from token
			(cmd-1)->nx = cmd;	// set next object of previous object to this object
		}
	}
	return (TG_OK);
}

static uint8_t _get_sys(const INDEX_T i, cmdObj *cmd)
{
	char token[CMD_TOKEN_LEN+1];	// token retrived from cmdArray
	INDEX_T grp_index = cmd->index;
	char include[] = {"fv,fb,si,gpl,gun,gco,gpa,gdi,ea,ja,ml,ma,mt,ic,il,ec,ee,ex"};
	char exclude[] = {"gc"};

	cmd->value_type = VALUE_TYPE_PARENT;
	for (INDEX_T i=0; i<grp_index; i++) {
		cmd_get_token(i, token);
		if (strstr(exclude, token) != NULL) continue;
		if (strstr(include, token) != NULL) {
			cmd_get_cmd(i,++cmd);
			(cmd-1)->nx = cmd;	// set next object of previous object to this object
		}
	}
	return (TG_OK);
}

static uint8_t _get_qm(const INDEX_T i, cmdObj *cmd)
{
	char token[CMD_TOKEN_LEN+1];	// token retrived from cmdArray
	INDEX_T grp_index = cmd->index;
	char include[] = {"xpos,ypos,zpos,apos,bpos,cpos,stat"};
//	char exclude[] = {"gc"};

	cmd->value_type = VALUE_TYPE_PARENT;
	for (INDEX_T i=0; i<grp_index; i++) {
		cmd_get_token(i, token);
//		if (strstr(exclude, token) != NULL) continue;
		if (strstr(include, token) != NULL) {
			cmd_get_cmd(i,++cmd);
			(cmd-1)->nx = cmd;	// set next object of previous object to this object
		}
	}
	return (TG_OK);
}

/****************************************************************************
 * EEPROM access functions:
 * cmd_read_NVM_record()  - return token and value by index number
 * cmd_write_NVM_record() - write token/value record to NVM by index
 * cmd_read_NVM_value()   - return value (as double) by index
 * cmd_write_NVM_record() - write value to NVM by index
 */
 /*
uint8_t cmd_read_NVM_record(const INDEX_T i, cmdObj *cmd)
{
	if ((i < 0) || (i >= CMD_MAX_INDEX)) return (TG_UNRECOGNIZED_COMMAND);
	int8_t nvm_record[NVM_RECORD_LEN];
	uint16_t nvm_address = cfg.nvm_profile_base + (i * NVM_RECORD_LEN);
	(void)EEPROM_ReadBytes(nvm_address, nvm_record, NVM_RECORD_LEN);
	cmd_new_object(cmd);	// clear it and ensure token gets terminated
	strncpy(cmd->token, (char *)&nvm_record, CMD_TOKEN_LEN);
	memcpy(&cmd->value, &nvm_record[CMD_TOKEN_LEN+1], sizeof(double));
	cmd->value_type = VALUE_TYPE_FLOAT;
	return (TG_OK);
}

uint8_t cmd_write_NVM_record(const INDEX_T i, const cmdObj *cmd)
{
	if ((i < 0) || (i >= CMD_MAX_INDEX)) return (TG_UNRECOGNIZED_COMMAND);
	int8_t nvm_record[NVM_RECORD_LEN];
	uint16_t nvm_address = cfg.nvm_profile_base + (i * NVM_RECORD_LEN);
	strncpy((char *)&nvm_record, cmd->token, CMD_TOKEN_LEN);
	memcpy(&nvm_record[CMD_TOKEN_LEN+1], &cmd->value, sizeof(double));
	(void)EEPROM_WriteBytes(nvm_address, nvm_record, NVM_RECORD_LEN);
	return(TG_OK);
}
*/
uint8_t cmd_read_NVM_value(const INDEX_T i, cmdObj *cmd)
{
	if ((i < 0) || (i >= CMD_INDEX_MAX)) return (TG_UNRECOGNIZED_COMMAND);
	int8_t nvm_value[NVM_VALUE_LEN];
	uint16_t nvm_address = cfg.nvm_profile_base + (i * NVM_VALUE_LEN);
	(void)EEPROM_ReadBytes(nvm_address, nvm_value, NVM_VALUE_LEN);
	cmd_new_object(cmd);	// clear it and ensure token gets terminated
	memcpy(&cmd->value, &nvm_value, sizeof(double));
	cmd->value_type = VALUE_TYPE_FLOAT;
	return (TG_OK);
}

uint8_t cmd_write_NVM_value(const INDEX_T i, const cmdObj *cmd)
{
	if ((i < 0) || (i >= CMD_INDEX_MAX)) return (TG_UNRECOGNIZED_COMMAND);
	int8_t nvm_value[NVM_VALUE_LEN];
	uint16_t nvm_address = cfg.nvm_profile_base + (i * NVM_VALUE_LEN);
	memcpy(&nvm_value, &cmd->value, sizeof(double));
	(void)EEPROM_WriteBytes(nvm_address, nvm_value, NVM_VALUE_LEN);
	return(TG_OK);
}

/****************************************************************************
 ***** Config Diagnostics ***************************************************
 ****************************************************************************/

/*
 * cfg_dump_NVM() 		- dump current NVM profile to stderr in 8 byte lines
 * _print_NVM_record()	- print a single record
 *
 *	Requires 'label' to be a program memory string. Usage example:
 *		cfg_dump_NVM(0,10,PSTR("Initial state"));
 */
#ifdef __DEBUG

static void _dump_NVM_record(const int16_t index, const int8_t *nvm_record);

void cfg_dump_NVM(const uint16_t start_index, const uint16_t end_index, char *label)
{
	int8_t nvm_record[NVM_RECORD_LEN];
	uint16_t nvm_address;
	uint16_t i;

	fprintf_P(stderr, PSTR("\nDump NMV - %S\n"), label);
	for (i=start_index; i<end_index; i++) {
		nvm_address = cfg.nvm_profile_base + (i * NVM_RECORD_LEN);
		(void)EEPROM_ReadBytes(nvm_address, nvm_record, NVM_RECORD_LEN);
		_dump_NVM_record(i, nvm_record);
	}
}

static void _dump_NVM_record(const int16_t index, const int8_t *nvm_record)
{
	char token[CMD_TOKEN_LEN+1];
	double value;

	strncpy(token, (char *)nvm_record, CMD_TOKEN_LEN);
	memcpy(&value, &nvm_record[CMD_TOKEN_LEN+1], sizeof(double));
	fprintf_P(stderr, PSTR("Index %d - %s %1.2f [%d %d %d %d %d %d %d %d]\n"), 
							index, token, value,
							nvm_record[0], nvm_record[1], nvm_record[2], nvm_record[3],
							nvm_record[4], nvm_record[5], nvm_record[6], nvm_record[7]);
}
#endif

/****************************************************************************
 ***** Config Unit Tests ****************************************************
 ****************************************************************************/

#ifdef __UNIT_TEST_CONFIG


void cfg_unit_tests()
{
//	cmdObj cmd;

// NVM tests
/*
	strcpy(cmd.token, "fc");
	cmd.value = 329.01;

	cmd_write_NVM(0, &cmd);
	cmd.value = 0;
	cmd_read_NVM(0, &cmd);
	cmd.value = 0;
	cmd_read_NVM(0, &cmd);
	cmd.nesting_level = 0;
// 	cfg_dump_NVM(0,10,PSTR("NVM dump"));
*/

// config table tests

	INDEX_T i;
//	double val;

//	_print_configs("$", NUL);					// no filter (show all)
//	_print_configs("$", 'g');					// filter for general parameters
//	_print_configs("$", '1');					// filter for motor 1
//	_print_configs("$", 'x');					// filter for x axis

	i = cmd_get_index_by_token("xfr");

/*
	for (i=0; i<CMD_MAX_INDEX; i++) {

		cmd_get(i, &cmd);

		cmd.value = 42;
		cmd_set(i, &cmd);

		val = _get_dbl_value(i);
		cmd_get_token(i, cmd.token);

//		_get_friendly(i, string);
		_get_format(i, cmd.vstring);
		_get_axis(i);							// uncomment main function to test
		_get_motor(i);
		cmd_set(i, &cmd);
		cmd_print(i);
	}

	_parse_config_string("$1po 1", &c);			// returns a number
	_parse_config_string("XFR=1200", &c);		// returns a number
	_parse_config_string("YFR 1300", &c);		// returns a number
	_parse_config_string("zfr	1400", &c);		// returns a number
	_parse_config_string("afr", &c);			// returns a null
	_parse_config_string("Bfr   ", &c);			// returns a null
	_parse_config_string("cfr=wordy", &c);		// returns a null

//	i = cfg_get_config_index("gc");
//	i = cfg_get_config_index("gcode");
//	i = cfg_get_config_index("c_axis_mode");
//	i = cfg_get_config_index("AINT_NOBODY_HOME");
	i = cfg_get_config_index("firmware_version");
*/
}

#endif
/*
	Gcode default settings are organized by groups:

		gpl	gcode_plane_default		G17/G18/G19		plane select group
		gun	gcode units_default		G20/G21			units mode group
		gpa gcode_path_control		G61/G61.1/G64	path control mode group
		gdi	gcode_distance_mode		G90/G91			distance mode group

	Valid settings are:

		$gpl=0	use XY as default plane (G17)
		$gpl=1	use XZ as default plane (G18)
		$gpl=2	use YZ as default plane (G19)

		$gun=0	use INCHES MODE on reset (G20)
		$gun=1	use MILLIMETER MODE on reset (G21)

		$gpa=0	use EXACT STOP MODE mode on reset (G61)
		$gpa=1	use EXACT PATH MODE mode on reset (G61.1)
		$gpa=2	use CONTINUOUS MODE mode on reset (G64)

		$gdi=0	use ABSOLUTE MODE mode on reset (G90)
		$gdi=1	use INCREMENTAL MODE mode on reset (G91)

	These settings ONLY affect how the system will be set power on reset, reset button, or limit switch hit.
	They do not change the current setting . To change the current setting use 
	the corresponding GCODE command (e.g. G17, G21...)

	To GET the current default value issue the command with no value, e.g. $bug to return units mode

	JSON behavior is the same as above. Here are some example JSON requests and responses
*/
