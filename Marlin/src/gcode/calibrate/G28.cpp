/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "../../inc/MarlinConfig.h"

#include "../gcode.h"

#include "../../module/endstops.h"
#include "../../module/planner.h"
#include "../../module/stepper.h" // for various

#if HAS_MULTI_HOTEND
  #include "../../module/tool_change.h"
#endif

#if HAS_LEVELING
  #include "../../feature/bedlevel/bedlevel.h"
#endif

#if ENABLED(BD_SENSOR)
  #include "../../feature/bedlevel/bdl/bdl.h"
#endif

#if ENABLED(SENSORLESS_HOMING)
  #include "../../feature/tmc_util.h"
#endif

#include "../../module/probe.h"

#if ENABLED(BLTOUCH)
  #include "../../feature/bltouch.h"
#endif

#include "../../lcd/marlinui.h"

#if ENABLED(EXTENSIBLE_UI)
  #include "../../lcd/extui/ui_api.h"
#elif ENABLED(DWIN_CREALITY_LCD)
  #include "../../lcd/e3v2/creality/dwin.h"
#elif ENABLED(DWIN_LCD_PROUI)
  #include "../../lcd/e3v2/proui/dwin.h"
#endif

#if ENABLED(LASER_FEATURE)
  #include "../../feature/spindle_laser.h"
#endif

#define DEBUG_OUT ENABLED(DEBUG_LEVELING_FEATURE)
#include "../../core/debug_out.h"

#include "../../feature/powerloss.h"
#include "../../feature/babystep.h"

#if ENABLED(QUICK_HOME)

  static void quick_home_xy() {

    // Pretend the current position is 0,0
    current_position.set(0.0, 0.0);
    sync_plan_position();

    const int x_axis_home_dir = TOOL_X_HOME_DIR(active_extruder);

    // Use a higher diagonal feedrate so axes move at homing speed
    const float minfr = _MIN(homing_feedrate(X_AXIS), homing_feedrate(Y_AXIS)),
                fr_mm_s = HYPOT(minfr, minfr);

    #if ENABLED(SENSORLESS_HOMING)
      sensorless_t stealth_states {
        NUM_AXIS_LIST(
          TERN0(X_SENSORLESS, tmc_enable_stallguard(stepperX)),
          TERN0(Y_SENSORLESS, tmc_enable_stallguard(stepperY)),
          false, false, false, false
        )
        , TERN0(X2_SENSORLESS, tmc_enable_stallguard(stepperX2))
        , TERN0(Y2_SENSORLESS, tmc_enable_stallguard(stepperY2))
      };
    #endif

    do_blocking_move_to_xy(1.5 * max_length(X_AXIS) * x_axis_home_dir, 1.5 * max_length(Y_AXIS) * Y_HOME_DIR, fr_mm_s);

    endstops.validate_homing_move();

    current_position.set(0.0, 0.0);

    #if ENABLED(SENSORLESS_HOMING) && DISABLED(ENDSTOPS_ALWAYS_ON_DEFAULT)
      TERN_(X_SENSORLESS, tmc_disable_stallguard(stepperX, stealth_states.x));
      TERN_(X2_SENSORLESS, tmc_disable_stallguard(stepperX2, stealth_states.x2));
      TERN_(Y_SENSORLESS, tmc_disable_stallguard(stepperY, stealth_states.y));
      TERN_(Y2_SENSORLESS, tmc_disable_stallguard(stepperY2, stealth_states.y2));
    #endif
  }

#endif // QUICK_HOME

#if ENABLED(Z_SAFE_HOMING)

  inline void home_z_safely() {
    DEBUG_SECTION(log_G28, "home_z_safely", DEBUGGING(LEVELING));

    // Disallow Z homing if X or Y homing is needed
    if (homing_needed_error(_BV(X_AXIS) | _BV(Y_AXIS))) return;

    sync_plan_position();

    /**
     * Move the Z probe (or just the nozzle) to the safe homing point
     * (Z is already at the right height)
     */
    constexpr xy_float_t safe_homing_xy = { Z_SAFE_HOMING_X_POINT, Z_SAFE_HOMING_Y_POINT };
    #if HAS_HOME_OFFSET
      xy_float_t okay_homing_xy = safe_homing_xy;
      okay_homing_xy -= home_offset;
    #else
      constexpr xy_float_t okay_homing_xy = safe_homing_xy;
    #endif

    destination.set(okay_homing_xy, current_position.z);

    TERN_(HOMING_Z_WITH_PROBE, destination -= probe.offset_xy);

    if (position_is_reachable(destination)) {

      if (DEBUGGING(LEVELING)) DEBUG_POS("home_z_safely", destination);

      // Free the active extruder for movement
      TERN_(DUAL_X_CARRIAGE, idex_set_parked(false));

      TERN_(SENSORLESS_HOMING, safe_delay(500)); // Short delay needed to settle

      do_blocking_move_to_xy(destination);
      homeaxis(Z_AXIS);
    }
    else {
      LCD_MESSAGE(MSG_ZPROBE_OUT);
      SERIAL_ECHO_MSG(STR_ZPROBE_OUT_SER);
    }
  }

#endif // Z_SAFE_HOMING

#if ENABLED(IMPROVE_HOMING_RELIABILITY)

  motion_state_t begin_slow_homing() {
    motion_state_t motion_state{0};
    motion_state.acceleration.set(planner.settings.max_acceleration_mm_per_s2[X_AXIS],
                                 planner.settings.max_acceleration_mm_per_s2[Y_AXIS]
                                 OPTARG(DELTA, planner.settings.max_acceleration_mm_per_s2[Z_AXIS])
                               );
    planner.settings.max_acceleration_mm_per_s2[X_AXIS] = 100;
    planner.settings.max_acceleration_mm_per_s2[Y_AXIS] = 100;
    TERN_(DELTA, planner.settings.max_acceleration_mm_per_s2[Z_AXIS] = 100);
    #if HAS_CLASSIC_JERK
      motion_state.jerk_state = planner.max_jerk;
      planner.max_jerk.set(0, 0 OPTARG(DELTA, 0));
    #endif
    planner.refresh_acceleration_rates();
    return motion_state;
  }

  void end_slow_homing(const motion_state_t &motion_state) {
    planner.settings.max_acceleration_mm_per_s2[X_AXIS] = motion_state.acceleration.x;
    planner.settings.max_acceleration_mm_per_s2[Y_AXIS] = motion_state.acceleration.y;
    TERN_(DELTA, planner.settings.max_acceleration_mm_per_s2[Z_AXIS] = motion_state.acceleration.z);
    TERN_(HAS_CLASSIC_JERK, planner.max_jerk = motion_state.jerk_state);
    planner.refresh_acceleration_rates();
  }

#endif // IMPROVE_HOMING_RELIABILITY

/**
 * G28: Home all axes according to settings
 *
 * Parameters
 *
 *  None  Home to all axes with no parameters.
 *        With QUICK_HOME enabled XY will home together, then Z.
 *
 *  L<bool>   Force leveling state ON (if possible) or OFF after homing (Requires RESTORE_LEVELING_AFTER_G28 or ENABLE_LEVELING_AFTER_G28)
 *  O         Home only if the position is not known and trusted
 *  R<linear> Raise by n mm/inches before homing
 *
 * Cartesian/SCARA parameters
 *
 *  X   Home to the X endstop
 *  Y   Home to the Y endstop
 *  Z   Home to the Z endstop
 */
void GcodeSuite::G28() {
  DEBUG_SECTION(log_G28, "G28", DEBUGGING(LEVELING));
  if (DEBUGGING(LEVELING)) log_machine_info();

  TERN_(BD_SENSOR, bdl.config_state = 0);

  /**
   * Set the laser power to false to stop the planner from processing the current power setting.
   */
  #if ENABLED(LASER_FEATURE)
    planner.laser_inline.status.isPowered = false;
  #endif

  #if ENABLED(DUAL_X_CARRIAGE)
    #if !TRONXY_UI //这里不能恢复复制的标志,否则复位后,再移动会撞头
    bool IDEX_saved_duplication_state = extruder_duplication_enabled;
    #endif
    DualXMode IDEX_saved_mode = dual_x_carriage_mode;
  #endif

  #if ENABLED(MARLIN_DEV_MODE)
    if (parser.seen_test('S')) {
      LOOP_NUM_AXES(a) set_axis_is_at_home((AxisEnum)a);
      sync_plan_position();
      SERIAL_ECHOLNPGM("Simulated Homing");
      report_current_position();
      return;
    }
  #endif

  // Home (O)nly if position is unknown
  if (!axes_should_home() && parser.seen_test('O')) {
    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("> homing not needed, skip");
    return;
  }

  #if ENABLED(FULL_REPORT_TO_HOST_FEATURE)
    const M_StateEnum old_grblstate = M_State_grbl;
    set_and_report_grblstate(M_HOMING);
  #endif

  TERN_(HAS_DWIN_E3V2_BASIC, DWIN_HomingStart());
  TERN_(EXTENSIBLE_UI, ExtUI::onHomingStart());

  planner.synchronize();          // Wait for planner moves to finish!

  SET_SOFT_ENDSTOP_LOOSE(false);  // Reset a leftover 'loose' motion state

  // Disable the leveling matrix before homing
  #if CAN_SET_LEVELING_AFTER_G28
    const bool leveling_restore_state = parser.boolval('L', TERN1(RESTORE_LEVELING_AFTER_G28, planner.leveling_active));
  #endif

  // Cancel any prior G29 session
  TERN_(PROBE_MANUALLY, g29_in_progress = false);

  // Disable leveling before homing
  TERN_(HAS_LEVELING, set_bed_leveling_enabled(false));

  // Reset to the XY plane
  TERN_(CNC_WORKSPACE_PLANES, workspace_plane = PLANE_XY);

  // Count this command as movement / activity
  reset_stepper_timeout();

  #define HAS_CURRENT_HOME(N) (defined(N##_CURRENT_HOME) && N##_CURRENT_HOME != N##_CURRENT)
  #if HAS_CURRENT_HOME(X) || HAS_CURRENT_HOME(X2) || HAS_CURRENT_HOME(Y) || HAS_CURRENT_HOME(Y2) || (ENABLED(DELTA) && HAS_CURRENT_HOME(Z)) || HAS_CURRENT_HOME(I) || HAS_CURRENT_HOME(J) || HAS_CURRENT_HOME(K) || HAS_CURRENT_HOME(U) || HAS_CURRENT_HOME(V) || HAS_CURRENT_HOME(W)
    #define HAS_HOMING_CURRENT 1
  #endif

  #if HAS_HOMING_CURRENT
    auto debug_current = [](FSTR_P const s, const int16_t a, const int16_t b) {
      DEBUG_ECHOF(s); DEBUG_ECHOLNPGM(" current: ", a, " -> ", b);
    };
    #if HAS_CURRENT_HOME(X)
      const int16_t tmc_save_current_X = stepperX.getMilliamps();
      stepperX.rms_current(X_CURRENT_HOME);
      if (DEBUGGING(LEVELING)) debug_current(F(STR_X), tmc_save_current_X, X_CURRENT_HOME);
    #endif
    #if HAS_CURRENT_HOME(X2)
      const int16_t tmc_save_current_X2 = stepperX2.getMilliamps();
      stepperX2.rms_current(X2_CURRENT_HOME);
      if (DEBUGGING(LEVELING)) debug_current(F(STR_X2), tmc_save_current_X2, X2_CURRENT_HOME);
    #endif
    #if HAS_CURRENT_HOME(Y)
      const int16_t tmc_save_current_Y = stepperY.getMilliamps();
      stepperY.rms_current(Y_CURRENT_HOME);
      if (DEBUGGING(LEVELING)) debug_current(F(STR_Y), tmc_save_current_Y, Y_CURRENT_HOME);
    #endif
    #if HAS_CURRENT_HOME(Y2)
      const int16_t tmc_save_current_Y2 = stepperY2.getMilliamps();
      stepperY2.rms_current(Y2_CURRENT_HOME);
      if (DEBUGGING(LEVELING)) debug_current(F(STR_Y2), tmc_save_current_Y2, Y2_CURRENT_HOME);
    #endif
    #if HAS_CURRENT_HOME(Z) && ENABLED(DELTA)
      const int16_t tmc_save_current_Z = stepperZ.getMilliamps();
      stepperZ.rms_current(Z_CURRENT_HOME);
      if (DEBUGGING(LEVELING)) debug_current(F(STR_Z), tmc_save_current_Z, Z_CURRENT_HOME);
    #endif
    #if HAS_CURRENT_HOME(I)
      const int16_t tmc_save_current_I = stepperI.getMilliamps();
      stepperI.rms_current(I_CURRENT_HOME);
      if (DEBUGGING(LEVELING)) debug_current(F(STR_I), tmc_save_current_I, I_CURRENT_HOME);
    #endif
    #if HAS_CURRENT_HOME(J)
      const int16_t tmc_save_current_J = stepperJ.getMilliamps();
      stepperJ.rms_current(J_CURRENT_HOME);
      if (DEBUGGING(LEVELING)) debug_current(F(STR_J), tmc_save_current_J, J_CURRENT_HOME);
    #endif
    #if HAS_CURRENT_HOME(K)
      const int16_t tmc_save_current_K = stepperK.getMilliamps();
      stepperK.rms_current(K_CURRENT_HOME);
      if (DEBUGGING(LEVELING)) debug_current(F(STR_K), tmc_save_current_K, K_CURRENT_HOME);
    #endif
    #if HAS_CURRENT_HOME(U)
      const int16_t tmc_save_current_U = stepperU.getMilliamps();
      stepperU.rms_current(U_CURRENT_HOME);
      if (DEBUGGING(LEVELING)) debug_current(F(STR_U), tmc_save_current_U, U_CURRENT_HOME);
    #endif
    #if HAS_CURRENT_HOME(V)
      const int16_t tmc_save_current_V = stepperV.getMilliamps();
      stepperV.rms_current(V_CURRENT_HOME);
      if (DEBUGGING(LEVELING)) debug_current(F(STR_V), tmc_save_current_V, V_CURRENT_HOME);
    #endif
    #if HAS_CURRENT_HOME(W)
      const int16_t tmc_save_current_W = stepperW.getMilliamps();
      stepperW.rms_current(W_CURRENT_HOME);
      if (DEBUGGING(LEVELING)) debug_current(F(STR_W), tmc_save_current_W, W_CURRENT_HOME);
    #endif
    #if SENSORLESS_STALLGUARD_DELAY
      safe_delay(SENSORLESS_STALLGUARD_DELAY); // Short delay needed to settle
    #endif
  #endif

  #if ENABLED(IMPROVE_HOMING_RELIABILITY)
    motion_state_t saved_motion_state = begin_slow_homing();
  #endif

  #if TRONXY_UI
  if(my_print_status == PRINT_RUNNING)my_print_status = PRINT_RUNNING_HOME;
  #endif

  // Always home with tool 0 active
  #if HAS_MULTI_HOTEND
    #if DISABLED(DELTA) || ENABLED(DELTA_HOME_TO_SAFE_ZONE)
      const uint8_t old_tool_index = active_extruder;
    #endif
    // PARKING_EXTRUDER homing requires different handling of movement / solenoid activation, depending on the side of homing
    #if ENABLED(PARKING_EXTRUDER)
      const bool pe_final_change_must_unpark = parking_extruder_unpark_after_homing(old_tool_index, X_HOME_DIR + 1 == old_tool_index * 2);
    #endif
    tool_change(0, true);
  #endif

  TERN_(HAS_DUPLICATION_MODE, set_duplication_enabled(false));

  remember_feedrate_scaling_off();

  endstops.enable(true); // Enable endstops for next homing move

  #if ENABLED(DELTA)

    constexpr bool doZ = true; // for NANODLP_Z_SYNC if your DLP is on a DELTA

    home_delta();

    TERN_(IMPROVE_HOMING_RELIABILITY, end_slow_homing(saved_motion_state));

  #elif ENABLED(AXEL_TPARA)

    constexpr bool doZ = true; // for NANODLP_Z_SYNC if your DLP is on a TPARA

    home_TPARA();

  #else

    #define _UNSAFE(A) (homeZ && TERN0(Z_SAFE_HOMING, axes_should_home(_BV(A##_AXIS))))

    const bool homeZ = TERN0(HAS_Z_AXIS, parser.seen_test('Z')),
               NUM_AXIS_LIST(              // Other axes should be homed before Z safe-homing
                 needX = _UNSAFE(X), needY = _UNSAFE(Y), needZ = false, // UNUSED
                 needI = _UNSAFE(I), needJ = _UNSAFE(J), needK = _UNSAFE(K),
                 needU = _UNSAFE(U), needV = _UNSAFE(V), needW = _UNSAFE(W)
               ),
               NUM_AXIS_LIST(              // Home each axis if needed or flagged
                 homeX = needX || parser.seen_test('X'),
                 homeY = needY || parser.seen_test('Y'),
                 homeZZ = homeZ,
                 homeI = needI || parser.seen_test(AXIS4_NAME), homeJ = needJ || parser.seen_test(AXIS5_NAME),
                 homeK = needK || parser.seen_test(AXIS6_NAME), homeU = needU || parser.seen_test(AXIS7_NAME),
                 homeV = needV || parser.seen_test(AXIS8_NAME), homeW = needW || parser.seen_test(AXIS9_NAME)
               ),
               home_all = NUM_AXIS_GANG(   // Home-all if all or none are flagged
                    homeX == homeX, && homeY == homeX, && homeZ == homeX,
                 && homeI == homeX, && homeJ == homeX, && homeK == homeX,
                 && homeU == homeX, && homeV == homeX, && homeW == homeX
               ),
               NUM_AXIS_LIST(
                 doX = home_all || homeX, doY = home_all || homeY, doZ = home_all || homeZ,
                 doI = home_all || homeI, doJ = home_all || homeJ, doK = home_all || homeK,
                 doU = home_all || homeU, doV = home_all || homeV, doW = home_all || homeW
               );

    #if HAS_Z_AXIS
      UNUSED(needZ); UNUSED(homeZZ);
    #else
      constexpr bool doZ = false;
    #endif
    
    #if USE_Z_LIMIT_PROBE_0 && ENABLED(HOME_Z_FIRST)
    if(doZ) {
      if((READ(Z_MAX_PIN) != Z_MAX_ENDSTOP_INVERTING
      TERN_(Z_MULTI_ENDSTOPS,|| READ(Z2_MAX_PIN) != Z2_MAX_ENDSTOP_INVERTING))
      && READ(Z_MIN_PROBE_PIN) != Z_MIN_PROBE_ENDSTOP_INVERTING) { //限位和探头都触发,这显然不合理
        #if TRONXY_UI
        popupZLimitError();
        #endif
        SERIAL_ECHOLNPGM("Homing z error for the limit");
        endstops.not_homing();
        return;
      }
      int t = 0;
      while((READ(Z_MAX_PIN) != Z_MAX_ENDSTOP_INVERTING
      TERN_(Z_MULTI_ENDSTOPS,|| READ(Z2_MAX_PIN) != Z2_MAX_ENDSTOP_INVERTING)) //限位触发,上抬
      && t++ < 5) {
        current_position.z -= 2;
        line_to_current_position(2.0);
        planner.synchronize();
      }
      if(t >= 5) { //传感器异常
        #if TRONXY_UI
        popupZLimitError();
        #endif
        SERIAL_ECHOLNPGM("Homing z error for the limit");
        endstops.not_homing();
        return;
      }
      homeaxis(Z_AXIS);
    }
    #else
    TERN_(HOME_Z_FIRST, if (doZ) homeaxis(Z_AXIS));
    #endif

    const bool seenR = parser.seenval('R');
    const float z_homing_height = seenR ? parser.value_linear_units() : Z_HOMING_HEIGHT;

    if (z_homing_height && (seenR || NUM_AXIS_GANG(doX, || doY, || TERN0(Z_SAFE_HOMING, doZ), || doI, || doJ, || doK, || doU, || doV, || doW))) {
      // Raise Z before homing any other axes and z is not already high enough (never lower z)
      if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Raise Z (before homing) by ", z_homing_height);
      do_z_clearance(z_homing_height);
      TERN_(BLTOUCH, bltouch.init());
    }

    // Diagonal move first if both are homing
    TERN_(QUICK_HOME, if (doX && doY) quick_home_xy());

    // Home Y (before X)
    if (ENABLED(HOME_Y_BEFORE_X) && (doY || TERN0(CODEPENDENT_XY_HOMING, doX)))
      homeaxis(Y_AXIS);

    // Home X
    if (doX || (doY && ENABLED(CODEPENDENT_XY_HOMING) && DISABLED(HOME_Y_BEFORE_X))) {

      #if ENABLED(DUAL_X_CARRIAGE)

        // Always home the 2nd (right) extruder first
        active_extruder = 1;
        homeaxis(X_AXIS);
        
        // Remember this extruder's position for later tool change
        inactive_extruder_x = current_position.x;

        // Home the 1st (left) extruder
        active_extruder = 0;
        homeaxis(X_AXIS);

        // Consider the active extruder to be in its "parked" position
        idex_set_parked();

      #else

        homeaxis(X_AXIS);

      #endif
    }

    #if BOTH(FOAMCUTTER_XYUV, HAS_I_AXIS)
      // Home I (after X)
      if (doI) homeaxis(I_AXIS);
    #endif

    // Home Y (after X)
    if (DISABLED(HOME_Y_BEFORE_X) && doY)
      homeaxis(Y_AXIS);

    #if BOTH(FOAMCUTTER_XYUV, HAS_J_AXIS)
      // Home J (after Y)
      if (doJ) homeaxis(J_AXIS);
    #endif

    TERN_(IMPROVE_HOMING_RELIABILITY, end_slow_homing(saved_motion_state));

    #if ENABLED(FOAMCUTTER_XYUV)
      // skip homing of unused Z axis for foamcutters
      if (doZ) set_axis_is_at_home(Z_AXIS);
    #else
      // Home Z last if homing towards the bed
      #if HAS_Z_AXIS && DISABLED(HOME_Z_FIRST)
        if (doZ) {
          #if EITHER(Z_MULTI_ENDSTOPS, Z_STEPPER_AUTO_ALIGN)
            stepper.set_all_z_lock(false);
            stepper.set_separate_multi_axis(false);
          #endif

          #if ENABLED(Z_SAFE_HOMING)
            if (TERN1(POWER_LOSS_RECOVERY, !parser.seen_test('H'))) home_z_safely(); else homeaxis(Z_AXIS);
          #else
            homeaxis(Z_AXIS);
          #endif
          probe.move_z_after_homing();
        }
      #endif
    #if USE_Z_LIMIT_PROBE_0 /*上面只是找了Z限位,现在要找probe零点*/
      #if Z_HOME_DIR > 0 //大端复位
        uint8_t home_probe_z = parser.intval('S',1);
        if(doZ && home_probe_z && !probe.deploy()) { //在断电续打的时候,不能探测Z高度
          sync_plan_position();
          float zpos = current_position.z; //记录当前高度
          #if ENABLED(Z_SAFE_HOMING)
            constexpr xy_float_t safe_homing_xy = { Z_SAFE_HOMING_X_POINT, Z_SAFE_HOMING_Y_POINT };
            #if HAS_HOME_OFFSET
              xy_float_t okay_homing_xy = safe_homing_xy;
              okay_homing_xy -= home_offset;
            #else
              constexpr xy_float_t okay_homing_xy = safe_homing_xy;
            #endif
            destination.set(okay_homing_xy, current_position.z);
            destination -= probe.offset_xy;
            if (position_is_reachable(destination)) {
              if (DEBUGGING(LEVELING)) DEBUG_POS("home_z_safely", destination);
              // Free the active extruder for movement
              TERN_(DUAL_X_CARRIAGE, idex_set_parked(false));
              TERN_(SENSORLESS_HOMING, safe_delay(500)); // Short delay needed to settle
              do_blocking_move_to_xy(destination);
            }
          #endif
          if (!probe.deploy()) { 
            probe.probe_down_to_z(-(soft_endstop.max.z * 1.5),MMM_TO_MMS(Z_PROBE_FEEDRATE_FAST));//probe探测Z直到触发probe,结束后更新current_position
            float bump = _MAX(Z_CLEARANCE_BETWEEN_PROBES, home_bump_mm(Z_AXIS));
            if (bump) {
              current_position.z += bump;
              line_to_current_position();
              bump *= 1.5;
              probe.probe_down_to_z(-bump,MMM_TO_MMS(Z_PROBE_FEEDRATE_SLOW));
            }
            probe.stow();
            PrintJobRecovery::info.zBottomPos = zpos - current_position.z;
            // 探测点停下的Z位置为0-probe-offset.z
            #if TRONXY_UI
            PrintJobRecovery::info.zBottomPos += -Probe_Offset.z;
            current_position.z = -Probe_Offset.z;
            #else
            PrintJobRecovery::info.zBottomPos += -probe.offset.z;
            current_position.z = -probe.offset.z;
            #endif
            destination[Z_AXIS] = current_position[Z_AXIS];
          }
          #if HAS_Z_AXIS && DISABLED(HOME_Z_FIRST)
          probe.move_z_after_homing();
          #endif
        }
      #else //小端复位
        if (doZ && !probe.deploy()) {
          probe.probe_down_to_z(-(soft_endstop.max.z * 1.5),MMM_TO_MMS(Z_PROBE_FEEDRATE_FAST));
          float bump = _MAX(Z_CLEARANCE_BETWEEN_PROBES, home_bump_mm(Z_AXIS));
          if (bump) {
            current_position.z += bump;
            line_to_current_position();
            bump *= 1.5;
            probe.probe_down_to_z(-bump,MMM_TO_MMS(Z_PROBE_FEEDRATE_SLOW));
          }
          probe.stow();
          #if TRONXY_UI
          current_position.z = -Probe_Offset.z;
          #else
          current_position.z = -probe.offset.z;
          #endif
          destination[Z_AXIS] = current_position[Z_AXIS];
          #if HAS_Z_AXIS && DISABLED(HOME_Z_FIRST)
          probe.move_z_after_homing();
          #endif
        }
      #endif
    #endif

      SECONDARY_AXIS_CODE(
        #if TAOLI_SERIAL
        if (doI) set_axis_is_at_home(I_AXIS),
        #else
        if (doI) homeaxis(I_AXIS),
        #endif
        if (doJ) homeaxis(J_AXIS),
        if (doK) homeaxis(K_AXIS),
        if (doU) homeaxis(U_AXIS),
        if (doV) homeaxis(V_AXIS),
        if (doW) homeaxis(W_AXIS)
      );
    #endif

    sync_plan_position();

  #endif

  /**
   * Preserve DXC mode across a G28 for IDEX printers in DXC_DUPLICATION_MODE.
   * This is important because it lets a user use the LCD Panel to set an IDEX Duplication mode, and
   * then print a standard GCode file that contains a single print that does a G28 and has no other
   * IDEX specific commands in it.
   */
  #if ENABLED(DUAL_X_CARRIAGE)

    if (TRONXY_UI || idex_is_duplicating()) {

      TERN_(IMPROVE_HOMING_RELIABILITY, saved_motion_state = begin_slow_homing());

      // Always home the 2nd (right) extruder first
      active_extruder = 1;
      homeaxis(X_AXIS);

      // Remember this extruder's position for later tool change
      inactive_extruder_x = current_position.x;

      // Home the 1st (left) extruder
      active_extruder = 0;
      homeaxis(X_AXIS);

      // Consider the active extruder to be parked
      idex_set_parked();

      dual_x_carriage_mode = IDEX_saved_mode;
      #if TRONXY_UI //这里不能恢复复制的标志,否则复位后,再移动会撞头
      stepper.set_directions();
      #else
      set_duplication_enabled(IDEX_saved_duplication_state);
      #endif

      TERN_(IMPROVE_HOMING_RELIABILITY, end_slow_homing(saved_motion_state));
    }

  #endif // DUAL_X_CARRIAGE

  endstops.not_homing();

  // Clear endstop state for polled stallGuard endstops
  TERN_(SPI_ENDSTOPS, endstops.clear_endstop_state());

  // Move to a height where we can use the full xy-area
  TERN_(DELTA_HOME_TO_SAFE_ZONE, do_blocking_move_to_z(delta_clip_start_height));

  TERN_(CAN_SET_LEVELING_AFTER_G28, if (leveling_restore_state) set_bed_leveling_enabled());

  restore_feedrate_and_scaling();

  // Restore the active tool after homing
  #if HAS_MULTI_HOTEND && (DISABLED(DELTA) || ENABLED(DELTA_HOME_TO_SAFE_ZONE))
    #if TRONXY_UI
    tool_change(old_tool_index, TERN(PARKING_EXTRUDER, !pe_final_change_must_unpark, ENABLED(DUAL_X_CARRIAGE)));
    #else
    tool_change(old_tool_index, TERN(PARKING_EXTRUDER, !pe_final_change_must_unpark, DISABLED(DUAL_X_CARRIAGE)));   // Do move if one of these
    #endif
  #endif

  #if HAS_HOMING_CURRENT
    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Restore driver current...");
    #if HAS_CURRENT_HOME(X)
      stepperX.rms_current(tmc_save_current_X);
    #endif
    #if HAS_CURRENT_HOME(X2)
      stepperX2.rms_current(tmc_save_current_X2);
    #endif
    #if HAS_CURRENT_HOME(Y)
      stepperY.rms_current(tmc_save_current_Y);
    #endif
    #if HAS_CURRENT_HOME(Y2)
      stepperY2.rms_current(tmc_save_current_Y2);
    #endif
    #if HAS_CURRENT_HOME(Z) && ENABLED(DELTA)
      stepperZ.rms_current(tmc_save_current_Z);
    #endif
    #if HAS_CURRENT_HOME(I)
      stepperI.rms_current(tmc_save_current_I);
    #endif
    #if HAS_CURRENT_HOME(J)
      stepperJ.rms_current(tmc_save_current_J);
    #endif
    #if HAS_CURRENT_HOME(K)
      stepperK.rms_current(tmc_save_current_K);
    #endif
    #if HAS_CURRENT_HOME(U)
      stepperU.rms_current(tmc_save_current_U);
    #endif
    #if HAS_CURRENT_HOME(V)
      stepperV.rms_current(tmc_save_current_V);
    #endif
    #if HAS_CURRENT_HOME(W)
      stepperW.rms_current(tmc_save_current_W);
    #endif
    #if SENSORLESS_STALLGUARD_DELAY
      safe_delay(SENSORLESS_STALLGUARD_DELAY); // Short delay needed to settle
    #endif
  #endif // HAS_HOMING_CURRENT
  
  #if !TRONXY_UI
  ui.refresh();
  #endif

  TERN_(HAS_DWIN_E3V2_BASIC, DWIN_HomingDone());
  TERN_(EXTENSIBLE_UI, ExtUI::onHomingDone());

  report_current_position();

  if (ENABLED(NANODLP_Z_SYNC) && (doZ || ENABLED(NANODLP_ALL_AXIS)))
    SERIAL_ECHOLNPGM(STR_Z_MOVE_COMP);

  TERN_(FULL_REPORT_TO_HOST_FEATURE, set_and_report_grblstate(old_grblstate));
  current_position.z += 5;
  line_to_current_position();
  planner.synchronize();
  #if TRONXY_UI
  if(my_print_status == PRINT_RUNNING_HOME) {
    my_print_status = PRINT_RUNNING;
    if(enabled_taoli)WRITE(pin_air,TAOLI_AIR_OPEN);
    #if ENABLED(BABYSTEPPING)
    int t = babystep.steps[BS_AXIS_IND(Z_AXIS)] = babystep.accum; //承继上次打印的babystep
    while(t != 0)t = babystep.steps[BS_AXIS_IND(Z_AXIS)];
    #endif
  }
  #endif

}
