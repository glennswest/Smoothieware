/*

    Comprehensive Delta Strategy by 626Pilot
    This code requires a Z-probe. You can use your own, or get mine here: http://www.thingiverse.com/626Pilot/designs

    This strategy implements functionality found in the following separate strategies:
        * DeltaCalibrationStrategy (calibrate DR & endstops on delta printers) - my version is faster :)
        * ThreePointStrategy (adjust surface normal) - this is calibrated by simulated annealing (see below)
        * ZGridStrategy (depth-map print surface & adjust effector's Z according to that)

    This strategy ADDS the following functionality, which the other strategies lack:
        * Probe calibration
        * Parallel simulated annealing, a "weak AI" method of calibrating delta printers (all 14 variables, SIMULTANEOUSLY!!!)
        * Surface normal (like ThreePointStrategy) AND depth-mapped interpolation (like ZGridStrategy) at the same time
        * You don't have to pick whether you want one feature or another; you can use everything you need
        * Method Prefixes: All printed output gets prepended with a method prefix, so you ALWAYS know which method printed anything
        *                  ~ Call push_prefix("XX") to specify the current method's two-character prefix
        *                  ~ Call _printf("words", ...) to get "[XX] words" (variadic, so you can use it like normal printf)
        *                  ~ Call pop_prefix() before you return from a method to restore the last one's prefix
        *                  ~ The prefix stack is managed, so you can never push or pop beyond the defined prefix stack size
        *                  ~ Saves over 10KB over manually putting "[XX] " at the beginning of every printed string

    The code was originally written for delta printers, but the probe calibration, surface normal, and grid-based Z correction are
    useful on other types as well. Some further improvements are needed, but I think I will eventually take "Delta" out of the
    name.

    G-codes:	G29	Probe Calibration
                G31	Heuristic Calibration (parallel simulated annealing)
                G32	Iterative Calibration (only calibrates endstops & delta radius)
                M667	Virtual shimming and depth correction params/enable/disable

    Files:	/sd/dm_surface_transform (contains depth map for use with depth map Z correction)

    The recommended way to use this on a Delta printer is:
    G29 (calibrate your probe)
    G32 (iterative calibration - gets endstops/delta radius correct - K to keep, but don't use that if you want to run G31 afterwards)
    G31 O P Q R S (simulated annealing - corrects for errors in X, Y, and Z - it may help to run this multiple times)
    G31 A (depth mapping - corrects errors in Z, but not X or Y - benefits from simulated annealing, though)

    The recommended way to use this on a Cartesian/CoreXY/SCARA etc. printer (of which the author has tested none, FYI):
    G29 (calibrate your probe)
    G31 A (depth mapping)

    To Do
    -------------------------
    * Migrate about 1KB of arrays from static class members to malloc'd on AHB0:
      * surface_transform.depth (done)
      * best_probe_calibration
      * test_point (200 bytes)
      * test_axis (300 bytes)
      * depth_map (200 bytes?)
      * active_point (100 bytes)
    * Audit arrays created during probe calibration & simulated annealing to see if any are candidates for AHB0 migration.
    * Increase probing grid size to 7x7, rather than 5x5. (Needs above AHB0 migration to be done first, crashes otherwise)
    * Elaborate probing grid to be able to support non-square grids (for the sake of people with rectangular build areas).
    * Add "leaning tower" support
      * Add {X, Y, Z(?)} coords for top and bottom of tower & use in FK and IK in robot/arm_solutions/LinearDeltaSolution.cpp/.h
      * Add letter codes to LinearDeltaSolution.cpp for saving/loading the values from config-override
      * Add simulated annealing section for tower lean
    * Make G31 B the specific command for heuristic calibration, and have it select O P Q R S (all annealing types) by default.
      * Make the annealer do a test probe rather than printing out the simulated depths.
      * If G31 B is run without args, use the last probed depths.
    * We are using both three-dimensional (Cartesian) and one-dimensional (depths type, .abs and .rel) arrays. Cartesians are
      necessary for IK/FK, but maybe we can make a type with X, Y, absolute Z, and relative Z, and be done with the multiple types.
      (Except for the depth map, which has to be kept in RAM all the time.)
      Such arrays can be "fat" while in use because they will live on the stack, and not take up any space when the calibration
      routines are not running.

*/


#include "ComprehensiveDeltaStrategy.h"
#include "Kernel.h"
#include "Config.h"
#include "Robot.h"
#include "StreamOutputPool.h"
#include "Gcode.h"
#include "checksumm.h"
#include "ConfigValue.h"
#include "PublicDataRequest.h"
#include "EndstopsPublicAccess.h"
#include "PublicData.h"
#include "Conveyor.h"
#include "ZProbe.h"
#include "BaseSolution.h"
#include "SerialMessage.h"
#include "Vector3.h"
#include "Planner.h"
#include "utils.h"

#include <time.h>
#include <tuple>
#include <algorithm>
#include <string>

// probe_radius is "deprecated" in favor of just radius, but it shouldn't be.
// Using just "radius" sounds like the printer radius, but probing can't always be done that far out.
#define probe_radius_checksum CHECKSUM("probe_radius")

#define probe_smoothing_checksum      CHECKSUM("probe_smoothing")
#define probe_acceleration_checksum   CHECKSUM("probe_acceleration")
#define probe_priming_checksum        CHECKSUM("probe_priming")
#define probe_offset_x_checksum       CHECKSUM("probe_offset_x")
#define probe_offset_y_checksum       CHECKSUM("probe_offset_y")
#define probe_offset_z_checksum       CHECKSUM("probe_offset_z")

// Array subscripts: Cartesian axes
#define X 0
#define Y 1
#define Z 2

// Array subscripts: Towers and their counter-clockwise neighbors
#define XY 0
#define YZ 1
#define ZX 2


// This prints to ALL streams. If you have second_usb_serial_enable turned on, you better connect a terminal to it!
// Otherwise, eventually the serial buffers will get full and the printer may crash the effector into the build surface.

// Print "[PF] words", where PF is two characters sent to push_prefix()
#define _printf prefix_printf

// Print "words", no prefix
#define __printf THEKERNEL->streams->printf

// printf() variant that can inject a prefix, and knows how to talk to the serial thingy
// Despite the extra space it takes, we still save a few KB from not having to store the same five characters ('[XX] ')
// at the beginning of a bunch of lines.
int __attribute__ ((noinline)) ComprehensiveDeltaStrategy::prefix_printf(const char* format, ...) {

    asm("");	// Discourage G++ from deciding to inline me anyway

    int len = strlen(format) * 2;
    char buf[len];

    va_list vl;
    va_start(vl, format);
    vsnprintf(buf, len, format, vl);
    va_end(vl);

    THEKERNEL->streams->printf("[%s] %s", method_prefix[method_prefix_idx], buf);
    
    return 1;

}

// This serves in place of a constructor; it will be called whenever the config is reloaded
// (which you can do over a serial console, by the way)
bool ComprehensiveDeltaStrategy::handleConfig() {

    // Init method prefixes
    method_prefix_idx = -1;
    push_prefix("");

    // Set probe_from_height to a value that find_bed_center_height() will know means it needs to be initialized
    probe_from_height = -1;

    // Set the dirty flag, so we know we have to calibrate the endstops and delta radius
    geom_dirty = true;

    // Turn off Z compensation (we don't want that interfering with our readings)
    surface_transform.depth = nullptr;
    surface_transform.depth_enabled = false;
    surface_transform.have_depth_map = false;

    // Zero out the surface normal
    set_virtual_shimming(0, 0, 0);
    set_adjust_function(true);

    // Zero out depth_map
    zero_depth_maps();

    // Turn off all calibration types
    clear_calibration_types();

    // TODO: Read this from config_override via M-code
    surface_shape = PSS_CIRCLE;

    // Initialize the best probe calibration stats (we'll use sigma==-1 to check whether initialized)
    best_probe_calibration.sigma = -1;
    best_probe_calibration.range = -1;
    best_probe_calibration.accel = -1;
    best_probe_calibration.debounce_count = -1;
    best_probe_calibration.decelerate = false;
    best_probe_calibration.eccentricity = true ;
    best_probe_calibration.smoothing = -1;
    best_probe_calibration.fast = -1;
    best_probe_calibration.slow = -1;
    
    // Probe radius
    float r = THEKERNEL->config->value(leveling_strategy_checksum, comprehensive_delta_strategy_checksum, probe_radius_checksum)->by_default(-1)->as_number();
    if(r == -1) {
        // Deprecated config syntax
        r =  THEKERNEL->config->value(zprobe_checksum, probe_radius_checksum)->by_default(100.0F)->as_number();
    }
    this->probe_radius = r;

    // Initialize bilinear interpolation array scaler (requires probe_radius)
    bili.cartesian_to_array_scaler = (DM_GRID_DIMENSION - 1) / (probe_radius * 2);

    // Initialize test points (requires probe_radius)
    init_test_points();

    // Probe smoothing: If your probe is super jittery, we can probe multiple times per request and average the results
    int p = THEKERNEL->config->value(comprehensive_delta_strategy_checksum, probe_smoothing_checksum)->by_default(1)->as_number();
    if(p <  1) p = 1;
    if(p > 10) p = 10;
    this->probe_smoothing = p;

    // Probe priming: Run the probe a specified # of times before the "real" probing (good for printers that demonstrate a Z settling issue)
    p = THEKERNEL->config->value(comprehensive_delta_strategy_checksum, probe_priming_checksum)->by_default(0)->as_number();
    if(p <  0) p = 0;
    if(p > 10) p = 10;
    this->probe_priming = p;

    // Probe acceleration
    probe_acceleration = THEKERNEL->config->value(comprehensive_delta_strategy_checksum, probe_acceleration_checksum)->by_default(200)->as_number();

    // Effector coordinates when probe is at bed center, at the exact height where it triggers.
    // To determine this:
    // - Heat the extruder
    // - Jog it down to the print surface, so it leaves a little dot
    // - Deploy the probe and move it until its trigger is touching the dot
    // - Jog the probe up enough to remove the dot, and then do so
    // - Jog the probe back down again until it triggers (use tiny moves to get it as accurate as possible)
    // - Record the position in config as probe_offset_x/y/z
    this->probe_offset_x = THEKERNEL->config->value(comprehensive_delta_strategy_checksum, probe_offset_x_checksum)->by_default(0)->as_number();
    this->probe_offset_y = THEKERNEL->config->value(comprehensive_delta_strategy_checksum, probe_offset_y_checksum)->by_default(0)->as_number();
    this->probe_offset_z = THEKERNEL->config->value(comprehensive_delta_strategy_checksum, probe_offset_z_checksum)->by_default(0)->as_number();

    return true;

}


// Init & clear memory on AHB0 for the bed-leveling depth map
// Thanks to ZGridStrategy.cpp, where I figured out how to use AHB0.
bool ComprehensiveDeltaStrategy::initDepthMapRAM() {

    // Init space on AHB0 for storing the bed leveling lerp grid
    if(surface_transform.depth != nullptr) {
        AHB0.dealloc(surface_transform.depth);
    }

    surface_transform.depth = (float *)AHB0.alloc(DM_GRID_ELEMENTS * sizeof(float));

    if(surface_transform.depth == nullptr) {
        _printf("ERROR: Couldn't allocate RAM for depth map.\n");
        return false;
    }

    // Zero out surface transform depths
    for(int i=0; i<DM_GRID_ELEMENTS; i++) {
        surface_transform.depth[i] = 0;
    }

    return true;

}


// Destructor
ComprehensiveDeltaStrategy::~ComprehensiveDeltaStrategy() {
    if(surface_transform.depth != nullptr) {
        AHB0.dealloc(surface_transform.depth);
    }
}


// Process incoming G- and M-codes
bool ComprehensiveDeltaStrategy::handleGcode(Gcode *gcode) {

    if( gcode->has_g) {
        // G code processing
        if(gcode->g == 29) { // Test the Z-probe for repeatability
            THEKERNEL->conveyor->wait_for_empty_queue();
            measure_probe_repeatability(gcode);
            return true;
        }

        if(gcode->g == 31) { // Depth mapping & heuristic delta calibration
            return handle_depth_mapping_calibration(gcode);
        }

        if(gcode->g == 32) { // Auto calibration for delta, Z bed mapping for cartesian
            bool keep = false;
            if(gcode->has_letter('K')) {
                keep = gcode->get_value('K');
            }
            THEKERNEL->conveyor->wait_for_empty_queue();
            iterative_calibration(keep);
            return true;
        }

    } else if(gcode->has_m) {
    
        char letters[] = "ABCDEFTUVLR";

        switch(gcode->m) {

            // If the geometry is modified externally, we set the dirty flag (but not for Z - that requires no recalibration)
            case 665:
                for(unsigned int i=0; i<strlen(letters); i++) {
                    if(gcode->has_letter(letters[i])) {
                        geom_dirty = true;
                    }
                }
                break;
            
            // Set geom dirty on trim change as well
            case 666:
                geom_dirty = true;
                break;
            
            // Surface equation for virtual shimming, depth map correction, and master enable
            case 667:
                handle_shimming_and_depth_correction(gcode);
                break;

            // Save depth map (CSV)
            case 500:
            case 503:
                // We use gcode->stream->printf instead of _printf because the dispatcher temporarily replaces the serial
                // stream printer with a file stream printer when M500/503 is sent.
                // A=X, B=Y, C=Z, D=Shimming enabled (1 or 0), E=Depth map correction enabled (1 or 0), Z=Master enable (1 or 0)
                // Master enable has to be on for either shimming or depth map correction to actually work.
                // Their individual flags only control whether they're available or not.
                gcode->stream->printf(
                    ";ABC=Shimming data; D=Shimming; E=Depth map; Z=Master enable\nM667 A%1.4f B%1.4f C%1.4f D%d E%d Z%d\n",
                    surface_transform.tri_points[X][Z], surface_transform.tri_points[Y][Z], surface_transform.tri_points[Z][Z],
                    (int)surface_transform.plane_enabled, (int)surface_transform.depth_enabled, (int)surface_transform.active);
                break;

        } // switch(gcode->m)
        
    }

    return false;

}


// Handlers for G-code commands too elaborate (read: stack-heavy) to cleanly fit in handleGcode()
// This fixes config-override file corruption when doing M500. :)

// G31
bool ComprehensiveDeltaStrategy::handle_depth_mapping_calibration(Gcode *gcode) {

    THEKERNEL->conveyor->wait_for_empty_queue();

    int x, y;//, dm_pos;

    if(gcode->has_letter('A')) {

        // It took me a really, really long (and frustrating) time to figure this out
        if(probe_offset_x != 0 || probe_offset_y != 0) {
            _printf("Depth correction doesn't work with X or Y probe offsets.\n");
            return false;
        }
    
        push_prefix("DC");
        newline();
        _printf("Probing bed for depth correction...\n");

        // Disable depth correction (obviously)
        surface_transform.depth_enabled = false;

        // Allocate some RAM for the depth map
        if(!initDepthMapRAM()) {
            _printf("Couldn't allocate RAM for the depth map.");
            return false;
        }

        // Build depth map
        zero_depth_maps();
        float cartesian[DM_GRID_ELEMENTS][3];
        if(!depth_map_print_surface(cartesian, RESULTS_FORMATTED, true)) {
            _printf("Couldn't build depth map - aborting!\n");
            pop_prefix();
            return false;
        }


        // Copy depth map to surface_transform.depth[], which contains depths only
        for(int i=0; i<DM_GRID_ELEMENTS; i++) {
            surface_transform.depth[i] = cartesian[i][Z];
        }

        // Propagate values outward from circle to edge, in case they go outside probe_radius
        if(surface_shape == PSS_CIRCLE) {
            for(y=0; y<DM_GRID_DIMENSION; y++) {
                for(x=0; x <= (DM_GRID_DIMENSION-1) / 2; x++) {

                    int dm_pos_right = (y * DM_GRID_DIMENSION) + ((DM_GRID_DIMENSION - 1) / 2) + x;
                    int dm_pos_left  = (y * DM_GRID_DIMENSION) + ((DM_GRID_DIMENSION - 1) / 2) - x;

                    // Propagate right
                    if(active_point[dm_pos_right] == TP_INACTIVE) {
                        surface_transform.depth[dm_pos_right] = surface_transform.depth[dm_pos_right - 1];
                    }

                    // Propagate left
                    if(active_point[dm_pos_left] == TP_INACTIVE) {
                        surface_transform.depth[dm_pos_left] = surface_transform.depth[dm_pos_left + 1];
                    }

                }
            }
        }

        // Enable depth correction
        surface_transform.depth_enabled = true;
        set_adjust_function(true);

        // Save to a file.
        // I tried saving this with G-codes, but I guess you can't stuff that much data.
        // The config-overrides file was corrupted when I tried! I found mention of a
        // file corruption bug elsewhere in the firmware, so I guess it's a known issue.
        // I could have just written everything as binary data, but I wanted people to
        // be able to populate the file with numbers from a regular $10 depth gauge in
        // case they don't have a Z-probe.
        FILE *fp = fopen("/sd/dm_surface_transform", "w");
        if(fp != NULL) {
            fprintf(fp, "; Depth Map Surface Transform\n");
            for(y=0; y<DM_GRID_DIMENSION; y++) {
                fprintf(fp, "; Line %d of %d\n", y + 1, DM_GRID_DIMENSION);
                for(x=0; x<DM_GRID_DIMENSION; x++) {
                    fprintf(fp, "%1.5f\n", surface_transform.depth[(y * DM_GRID_DIMENSION) + x]);
                }
            }

            // This is probably important to do
            fclose(fp);

            _printf("Surface transform saved to SD card. Type M500 to auto-enable.\n");
        
        } else {

            _printf("Couldn't save surface transform to SD card!\n");

        }
        
        zprobe->home();
        pop_prefix();

    } else if(gcode->has_letter('Z')) {

        // We are only here to map the surface - no calibration
        newline();
        push_prefix("DM");
        _printf("Current kinematics:\n");
        print_kinematics();
        newline();
        float dummy[DM_GRID_ELEMENTS][3];
        if(!depth_map_print_surface(dummy, RESULTS_FORMATTED, false)) {
            _printf("Couldn't depth-map the surface.\n");
        }
        pop_prefix();
        zprobe->home();

    } else {

        // Do a heuristic calibration (or simulation)
        clear_calibration_types();
        int annealing_tries = 50;
        float max_temp = 0.35;
        float binsearch_width = 0.1;
        float overrun_divisor = 2;
        bool simulate_only = false;
        bool keep_settings = false;
        bool zero_all_offsets = false;

        // Keep settings?
        if(gcode->has_letter('K')) {
            keep_settings = true;
        }

        // Simulate-only
        if(gcode->has_letter('L')) {
            simulate_only = true;
        }

        // Endstops
        if(gcode->has_letter('O')) {
            caltype.endstop.active = true;
            caltype.endstop.annealing_temp_mul = gcode->get_value('O');
        }
        
        // Delta radius, including individual tower offsets
        if(gcode->has_letter('P')) {
            caltype.delta_radius.active = true;
            caltype.delta_radius.annealing_temp_mul = gcode->get_value('P');
        }

        // Arm length, including individual arm length offsets
        if(gcode->has_letter('Q')) {
            caltype.arm_length.active = true;
            caltype.arm_length.annealing_temp_mul = gcode->get_value('Q');
        }

        // Tower angle offsets
        if(gcode->has_letter('R')) {
            caltype.tower_angle.active = true;
            caltype.tower_angle.annealing_temp_mul = gcode->get_value('R');
        }
        
        // Surface plane virtual shimming
        if(gcode->has_letter('S')) {
            caltype.virtual_shimming.active = true;
            caltype.virtual_shimming.annealing_temp_mul = gcode->get_value('S');
        }

        // Annealing tries
        // Generally, more iterations require lower temps
        if(gcode->has_letter('T')) {
            annealing_tries = gcode->get_value('T');
        }
        
        // Max temperature (tradeoff between "too cold to get there" and "so hot that it boils" - you want "just right")
        if(gcode->has_letter('U')) {
            max_temp = gcode->get_value('U');
        }
        
        // Binary search width (tradeoff between speed and accuracy - I recommend 0.1)
        if(gcode->has_letter('V')) {
            binsearch_width = gcode->get_value('V');
        }

        // Overrun divisor (what a random move is divided by if it overshoots the ideal value)
        // No, it isn't a good idea to use <=1.
        if(gcode->has_letter('W')) {
            overrun_divisor = gcode->get_value('W');
        }

        // Zero all offset values
        if(gcode->has_letter('Y')) {
            zero_all_offsets = true;
        }

        push_prefix("HC");
        if(gcode->get_num_args() > 0) {

            // Make sure at least one caltype is turned on
            if(
                !caltype.endstop.active &&
                !caltype.delta_radius.active &&
                !caltype.arm_length.active &&
                !caltype.tower_angle.active &&
                !caltype.virtual_shimming.active
            ){
                _printf("No calibration types selected - activating endstops & delta radius.\n");
                caltype.endstop.active = true;
                caltype.delta_radius.active = true;
            }

            heuristic_calibration(annealing_tries, max_temp, binsearch_width, simulate_only, keep_settings, zero_all_offsets, overrun_divisor);
        
        } else {
        
            flush();
            _printf("G31 usage: (* = you can supply an annealing multiplier)\n");
            _printf("Z: Probe and display depth map - no calibration\n");
            _printf("A: Set up depth map for auto leveling (corrects Z only - run AFTER annealing)\n");
            _printf("\n");
            _printf("Simulated annealing (corrects X, Y and Z - run G32 first):\n");
            _printf("K: Keep last settings\n");
            _printf("L: Simulate only (don't probe)\n");
            _printf("O: Endstops *\n");
            _printf("P: Delta radius *\n");
            _printf("Q: Arm length *\n");
            _printf("R: Tower angle offsets *\n");
            _printf("S: Surface plane virtual shimming *\n");
            _printf("t: Annealing: Iterations (50)\n");		// Repetier Host eats lines starting with T >:(
            _printf("U: Annealing: Max t_emp (0.35)\n");	// Repetier Host eats all lines containing "temp" >8(
            _printf("V: Annealing: Binary search width (0.1)\n");
            _printf("W: Annealing: Overrun divisor (2)\n");
            _printf("Y: Zero all individual radius, angle, and arm length offsets\n");
            flush();
        } //if(gcode->get_num_args() > 0)
        pop_prefix();
    
    } // !gcode->has_letter('M')

    return true;

}

// M667
bool ComprehensiveDeltaStrategy::handle_shimming_and_depth_correction(Gcode *gcode) {

    push_prefix("DM");

    // Triangle points for shimming surface normal
    if(gcode->has_letter('A')) surface_transform.tri_points[X][Z] = gcode->get_value('A');
    if(gcode->has_letter('B')) surface_transform.tri_points[Y][Z] = gcode->get_value('B');
    if(gcode->has_letter('C')) surface_transform.tri_points[Z][Z] = gcode->get_value('C');

    // Shimming
    if(gcode->has_letter('D')) surface_transform.plane_enabled = gcode->get_value('D');
    if(surface_transform.plane_enabled) {
        set_virtual_shimming(surface_transform.tri_points[X][Z], surface_transform.tri_points[Y][Z], surface_transform.tri_points[Z][Z]);
        set_adjust_function(true);
    }

    // Depth map
    if(gcode->has_letter('E')) {

        if(probe_offset_x == 0 && probe_offset_y == 0) {

            if(surface_transform.have_depth_map) {

                // Depth map already loaded 
                surface_transform.depth_enabled = gcode->get_value('E');

            } else {

                // ST not initialized - try to load it
                
                // First, allocate memory for depth map
                if(!initDepthMapRAM()) {
                    _printf("Couldn't allocate RAM for the depth map.");
                    return false;
                }
                
                FILE *fp = fopen("/sd/dm_surface_transform", "r");
                if(fp != NULL) {

                    char buf[48];
                    int i = 0;
                    
                    while(fgets(buf, sizeof buf, fp) != NULL) {

                        // Chop trailing newline
                        char *pos;
                        if((pos=strchr(buf, '\n')) != NULL) {
                            *pos = '\0';
                        }

                        // Skip comment lines
                        if(buf[0] == ';') continue;

                        // Add float value to the transform                            
                        if(i < DM_GRID_ELEMENTS) {

                            float fval = atof(buf);

                            if(fval > -5 && fval < 5) {
                                surface_transform.depth[i] = strtof(buf, NULL);
                                //_printf("buffer='%s' - Surface transform element %2d set to %1.3f.\n", buf, i, surface_transform.depth[i]);
                                i++;
                            } else {
                                _printf("Surface transform element %2d is out of range (%1.3f) - aborting.\n", i, surface_transform.depth[i]);
                                fclose(fp);
                                surface_transform.depth_enabled = false;
                                return false;
                            }

                        }
                    } // while

                    // Goodbye, cool file full of useful numbers
                    fclose(fp);

                    // Sanity check
                    if(i != DM_GRID_ELEMENTS) {
                        _printf("ERROR: Expected %d elements, but got %d - aborting.\n", DM_GRID_ELEMENTS, i);
                        surface_transform.have_depth_map = false;
                        surface_transform.depth_enabled = false;
                    } else {
                        surface_transform.depth_enabled = gcode->get_value('E');
                        if(surface_transform.depth_enabled == true) {
                            surface_transform.depth_enabled = true;
                            set_adjust_function(true);
                        } else {
                            surface_transform.depth_enabled = false;
                        }
                    }

                } else {

                    _printf("Depth correction not initialized.\n");

                } // if(fp != NULL)

            } // if(surface_transform.have_depth_map)

        } else {

            // FIXME:
            // For now, silently fail to enable.
            // This is because whatever we spew here risks hanging the firmware on startup,
            // because it will fill a serial buffer that never gets flushed.
            // The same warning is printed above if you do G31 A with probe offsets enabled,
            // so users are somewhat likely to see it.
            
            //_printf("Depth correction doesn't work with X or Y probe offsets.\n");

        } // if(probe offsets are 0)
      
    } // if(gcode->has_letter('E')

    // Global enable/disable
    if(gcode->has_letter('Z')) {
        bool enable = gcode->get_value('Z');
        if(enable) {
            if(surface_transform.depth_enabled || surface_transform.plane_enabled) {
                set_adjust_function(true);
            } else {
                _printf("Can't enable surface transform - no data.\n");
            }
        } else {
            set_adjust_function(false);
        }
    }

    //_printf("Surface transform: Depth map=%s; Surface plane=%s; Active=%s\n", surface_transform.depth_enabled ? _STR_ENABLED_ : _STR_DISABLED_, surface_transform.plane_enabled ? _STR_ENABLED_ : _STR_DISABLED_, surface_transform.active ? _STR_ENABLED_ : _STR_DISABLED_);
    pop_prefix();

    return true;

}


// Main heuristic calibration routine
// This expects caltype.*.active to be set true/false beforehand
bool ComprehensiveDeltaStrategy::heuristic_calibration(int annealing_tries, float max_temp, float binsearch_width, bool simulate_only, bool keep_settings, bool zero_all_offsets, float overrun_divisor) {


/*
        Simulated annealing notes
        
        - Works by trying to take the system from a high-energy state to the lowest-energy state
        - Slowly reduces the "temperature" of the system
            - Temperature affects how "bad" a possibility can be and still be tested
        - Acceptance probability function
            - P(e, e', T)
                -  e: existing state
                - e': candidate test state
                -  T: global temperature
            - Generally, but not always, we want e' < e
                - If e' > e, it's "hotter" and less desirable
                - However, hotter may be necessary to escape a local optimum
                - How much hotter e' can be than e is bound by T
            - P(e, e', T) must ALWAYS be positive
                - If it's not, we may get stuck around a local optimum and never "escape" to find the global optimum
        
        - Pseudocode
            - state = state[0]					// OK
            - energy = energy(state)				// OK
            - kMax = max iterations				// OK
            - eMax = maximum acceptable energy			// OK
            - while(k < kMax && energy > eMax) {		// OK
            -	temp = temperature(k / kMax)			// OK
            -	stateNew = randomNeighbor(s)			// Pick some random other state w/ variables anywhere in range
            - 	energyNew = energy(stateNew)			// OK
            -	if(P(energy, energyNew, temp) > frand(0, 1)) {	// Simulate energy of new state, compare to temperature
            -		state = stateNew;			// OK
            -		energy = energyNew;			// OK
            - 	}						// OK
            -	k++						// OK
            - }							// OK
*/

    // LED twiddling
    bool LED_state = false;

    // Banner
    push_prefix("HC");
    print_task_with_warning("Heuristic calibration");

    // Sanity check regular variables
    annealing_tries = clamp(annealing_tries, 10, 1000);
    max_temp = clamp(max_temp, 0, 2);
    binsearch_width = clamp(binsearch_width, 0, 0.5);
    overrun_divisor = clamp(overrun_divisor, 0.5, 15);

    // Ensure parallel annealing temp multipliers aren't zero
    if(caltype.endstop.annealing_temp_mul == 0) caltype.endstop.annealing_temp_mul = 1;
    if(caltype.delta_radius.annealing_temp_mul == 0) caltype.delta_radius.annealing_temp_mul = 1;
    if(caltype.arm_length.annealing_temp_mul == 0) caltype.arm_length.annealing_temp_mul = 1;
    if(caltype.tower_angle.annealing_temp_mul == 0) caltype.tower_angle.annealing_temp_mul = 1;
    if(caltype.virtual_shimming.annealing_temp_mul == 0) caltype.virtual_shimming.annealing_temp_mul = 1;
    
    // Ensure parallel annealing temp multipliers aren't crazy
    caltype.endstop.annealing_temp_mul = clamp(caltype.endstop.annealing_temp_mul, 0, 50);
    caltype.delta_radius.annealing_temp_mul = clamp(caltype.delta_radius.annealing_temp_mul, 0, 50);
    caltype.arm_length.annealing_temp_mul = clamp(caltype.arm_length.annealing_temp_mul, 0, 50);
    caltype.tower_angle.annealing_temp_mul = clamp(caltype.tower_angle.annealing_temp_mul, 0, 50);
    caltype.virtual_shimming.annealing_temp_mul = clamp(caltype.virtual_shimming.annealing_temp_mul, 0, 50);

    // Zero offsets, if requested
    if(zero_all_offsets) {
        set_virtual_shimming(0, 0, 0);
        set_trim(0, 0, 0);
        set_tower_radius_offsets(0, 0, 0);
        set_tower_angle_offsets(0, 0, 0);
        get_kinematics(base_set);
        get_kinematics(cur_set);
    }

    // Is it live, or is it Memorex?
    char _sim[] = "Simulation (L)";
    char _probe[] = "Probe";
    _printf("            Data source: %s\n", simulate_only ? _sim : _probe);

    // Display values used, along with the G-codes used to set them
    _printf("           Active tests: ");
    display_calibration_types(true, false);
    _printf("         Inactive tests: ");
    display_calibration_types(false, true);

    _printf(" Keep last settings (K): %s\n", keep_settings ? _STR_TRUE_ : _STR_FALSE_);
    _printf("    Annealing tries (T): %d\n", annealing_tries);
    _printf("           Max temp (U): %1.3f\n", max_temp);
    _printf("Binary search width (V): %1.3f\n", binsearch_width);
    _printf("    Overrun divisor (W): %1.3f\n", overrun_divisor);
    _printf("   Zero all offsets (Y): %s\n", zero_all_offsets ? _STR_TRUE_ : _STR_FALSE_);
    newline();

    // Make sure the depth maps are blank
    zero_depth_maps();


    // *******************************************************************
    // * Run a simulated annealing to get the printer config most likely *
    // * to produce what the real printer is doing                       *
    // *******************************************************************

    srand(time(NULL));

    // Depth correction has to be off, or none of this stuff will work
    surface_transform.depth_enabled = false;
    
    // Deal with virtual shimming
    if(caltype.virtual_shimming.active) {
        surface_transform.plane_enabled = true;
    } else {
        surface_transform.plane_enabled = false;
    }

    // We need to save the kinematic settings for later
    if(!simulate_only || !base_set.initialized) {
        _printf("Baseline kinematics updated.\n");
        get_kinematics(base_set);
    }

    // Make sure cur_set is initialized
    if(!cur_set.initialized) {
        get_kinematics(cur_set);
    }

    // If we aren't keeping the kinematic settings, copy the base settings into the current settings
    // If not simulating, we need to stay with the last kinematics because they may have changed
    // (whereas, in simulation, they never change)
    if(keep_settings || !simulate_only) {
        _printf("Keeping existing kinematics.\n");
        get_kinematics(cur_set);
    } else {
        _printf("Restoring baseline kinematics.\n");
        base_set.copy_to(cur_set);
        set_kinematics(cur_set);
    }


    // Tests (min, max, value|TEST_INIT_MIDRANGE))
    // Main tests:
    TestConfig test_endstop[3] { {-5, 0}, {-5, 0}, {-5, 0} };
    TestConfig test_delta_radius(cur_set.delta_radius - 5, cur_set.delta_radius + 5);
    TestConfig test_arm_length(cur_set.arm_length - 5, cur_set.arm_length + 5);
    TestConfig test_tower_angle[3] { {-3, 3}, {-3, 3}, {-3, 3} };
    TestConfig test_virtual_shimming[3] { {-3, 3}, {-3, 3}, {-3, 3} };

    // Offsets that tie into the main tests:
    TestConfig test_delta_radius_offset[3] { {-3, 3}, {-3, 3}, {-3, 3} };
//    TestConfig test_arm_length_offset[3] { {-3, 3}, {-3, 3}, {-3, 3} };

    // Set up for outer loop
    int outer_try, outer_tries = 1;		// How many full iterations (probe print surface and run annealing for each test variable)
    int annealing_try;				// Current annealing iteration
    float tempFraction, temp;
    float best_value;				// Binary search returns this

    // Set up target tolerance
    float target = 0.005;			// Target deviation for individual element in simulated annealing only
    float global_target = 0.010;		// Target Z-deviation for all points on print surface

    // If simulating, we only need to run the numbers once per session. Otherwise, they have to be redone every time.
    static bool need_to_simulate_IK = true;

    // Copy kinematic settings from temp_set to cur_set after simulating IK?
    bool restore_from_temp_set;

    // Other vars
    float cur_cartesian[DM_GRID_ELEMENTS][3];
    int j, k;
    int try_mod_5;				// Will be set to annealing_try % 5
    float lowest;				// For finding the lowest absolute value of three variables

    // Keep track of energy so that we can bail if the annealing stalls
    #define LAST_ENERGY_N 6
    float last_energy[LAST_ENERGY_N];
    unsigned last_energy_count = 0;
    for(j=0; j<LAST_ENERGY_N; j++) {
        last_energy[j] = 0;
    }

    
    // ************************************
    // * Simulated Annealing - Outer Loop *
    // ************************************

    for(outer_try = 0; outer_try < outer_tries; outer_try++) {

        // Clear flag that tells the IK simulator to restore kinematics to temp_set after the simulation runs
        restore_from_temp_set = false;


        if(simulate_only) {

            // Doing it for pretend: Generate some test values
            zero_depth_maps();

            if(!keep_settings) {

                _printf("Perturbing simulated printer parameters.\n");

                // Save existing kinematics
                restore_from_temp_set = true;
                get_kinematics(temp_set);

                // Perturb the parameters
                if(caltype.endstop.active) {
                    set_trim(-1.834, -1.779, 0.000);
                }

                if(caltype.delta_radius.active) {
                    set_delta_radius(131.25);
                    set_tower_radius_offsets(-1, 0, 2);
                } else {
                    set_tower_radius_offsets(0, 0, 0);
                }

                if(caltype.arm_length.active) {
                    set_arm_length(269.75);
                }

                if(caltype.tower_angle.active) {
                    set_tower_angle_offsets(1, 0, -1.5);
                } else {
                    set_tower_angle_offsets(0, 0, 0);
                }
                
                if(caltype.virtual_shimming.active) {
                    set_virtual_shimming(0.0, 0.0, -1.0);
                } else {
                    set_virtual_shimming(0, 0, 0);
                }

                // Save the perturbed kinematics
                get_kinematics(cur_set);

                // Trigger regen of carriage positions
                need_to_simulate_IK = true;

                _printf("After hosing the variables, the settings are now:\n");
                print_kinematics();

            } // !keep_settings

        } else { // !simulate_only

            // Doing it for real: Get values from probe
            // depth_map[] will contain measured depths relative to center

            if(!keep_settings) {

                _printf("Depth-mapping the print surface...\n");
                print_kinematics();
                if(!depth_map_print_surface(cur_cartesian, RESULTS_FORMATTED, false)) {
                    _printf("Couldn't depth-map the surface.\n");
                    zprobe->home();
                    pop_prefix();
                    return false;
                }

            } else {

                _printf("Keeping old depth map.\n");

            }

        } // simulate_only


        // ***************************************************************
        // * Figure out the actuator positions,                          *
        // * given a printer that ~perfectly~ matches the current config *
        // ***************************************************************

        // Generated test positions => cur_cartesian, generated axis positions => test_axis[] (class member)
        if(need_to_simulate_IK || !simulate_only) {
            _printf("Generating carriage positions for a printer with this configuration.\n");

            simulate_IK(cur_cartesian, cur_set.trim);
            if(restore_from_temp_set) {
                temp_set.copy_to(cur_set);
                set_kinematics(cur_set);
            }
            need_to_simulate_IK = false;
        }

        newline();
        _printf("Starting test configuration: Arm Length=%1.3f, Delta Radius=%1.3f\n", cur_set.arm_length, cur_set.delta_radius);



        // Get energy of initial state
        float energy = calc_energy(cur_cartesian);
        newline();
        _printf("***** Simulated annealing pass %d of %d in progress *****\n", outer_try + 1, outer_tries);
        _printf("Existing calibration has energy %1.3f\n \n", energy);
        _printf("Reticulating splines...\n");


        // ************************************
        // * Simulated Annealing - Inner Loop *
        // ************************************

        for(annealing_try=0; annealing_try<annealing_tries; annealing_try++) {


            // Twiddle an LED so the user knows we aren't dead
            // From main.c: "led0 init doe, led1 mainloop running, led2 idle loop running, led3 sdcard ok"
            // Therefore, LED 1 seems like the one to strobe. Normally, it's constantly dark when this method is running.
            if(THEKERNEL->use_leds) {
                leds[1] = LED_state;
                LED_state = !LED_state;
            }

            // Set the annealing temperature
            tempFraction = (float)annealing_try / (float)annealing_tries;
            temp = max_temp - (tempFraction * max_temp);
            if(temp < 0.01) {
                temp = 0.01;
            }
            
            try_mod_5 = annealing_try % 5;

            // ****************
            // * Delta Radius *
            // ****************

            if(caltype.delta_radius.active) {

                // Find the best tower (delta) radius offsets
                for(k=0; k<3; k++) {
                    best_value = find_optimal_config(&ComprehensiveDeltaStrategy::set_tower_radius_offsets, cur_set.tower_radius, k, test_delta_radius_offset[k].range_min, test_delta_radius_offset[k].range_max, binsearch_width, cur_cartesian, target);
                    move_randomly_towards(cur_set.tower_radius[k], best_value, temp * caltype.delta_radius.annealing_temp_mul, target, overrun_divisor);
                }

                // Find the tower radius with the lowest absolute value
                lowest = 999;
                for(k=0; k<3; k++) {
                    if(fabs(cur_set.tower_radius[k]) < lowest) {
                        lowest = cur_set.tower_radius[k];
                    }
                }

                // Steal that value from the individual radius settings and give it to the global radius setting
                for(k=0; k<3; k++) {
                    cur_set.tower_radius[k] -= lowest;
                }
                cur_set.delta_radius += lowest;

                // Tell the robot what the new delta radius & offsets are
                set_delta_radius(cur_set.delta_radius, false);
                set_tower_radius_offsets(cur_set.tower_radius[X], cur_set.tower_radius[Y], cur_set.tower_radius[Z], false);

            } // caltype.delta_radius.active


            // **************
            // * Arm Length *
            // **************

            if(caltype.arm_length.active) {

              	best_value = find_optimal_config(&ComprehensiveDeltaStrategy::set_arm_length, test_arm_length.range_min, test_arm_length.range_max, binsearch_width, cur_cartesian, target);
              	move_randomly_towards(cur_set.arm_length, best_value, temp * caltype.arm_length.annealing_temp_mul, target, overrun_divisor);
              	set_arm_length(cur_set.arm_length, false);
            }


            // ************
            // * Endstops *
            // ************

            if(caltype.endstop.active) {

                for(k=0; k<3; k++) {
                    best_value = find_optimal_config(&ComprehensiveDeltaStrategy::set_test_trim, cur_set.trim, k, test_endstop[k].range_min, test_endstop[k].range_max, binsearch_width, cur_cartesian, target);
                    move_randomly_towards(cur_set.trim[k], best_value, temp * caltype.endstop.annealing_temp_mul, target, overrun_divisor);
                }

                // Set trim
                set_trim(cur_set.trim[X], cur_set.trim[Y], cur_set.trim[Z]);

            }


            // ****************
            // * Tower angles *
            // ****************

            if(caltype.tower_angle.active) {

                for(k=0; k<3; k++) {
                    best_value = find_optimal_config(&ComprehensiveDeltaStrategy::set_tower_angle_offsets, cur_set.tower_angle, k, test_tower_angle[k].range_min, test_tower_angle[k].range_max, binsearch_width, cur_cartesian, target);
                    move_randomly_towards(cur_set.tower_angle[k], best_value, temp * caltype.endstop.annealing_temp_mul, target, overrun_divisor);
                }
                set_tower_angle_offsets(cur_set.tower_angle[X], cur_set.tower_angle[Y], cur_set.tower_angle[Z], false);

            }


            // ********************
            // * Virtual Shimming *
            // ********************

            if(caltype.virtual_shimming.active) {
            
                for(k=0; k<3; k++) {
                    best_value = find_optimal_config(&ComprehensiveDeltaStrategy::set_test_virtual_shimming, cur_set.virtual_shimming, k, test_virtual_shimming[k].range_min, test_virtual_shimming[k].range_max, binsearch_width, cur_cartesian, target);
                    move_randomly_towards(cur_set.virtual_shimming[k], best_value, temp * caltype.virtual_shimming.annealing_temp_mul, target, overrun_divisor);
                }
                set_virtual_shimming(cur_set.virtual_shimming[X], cur_set.virtual_shimming[Y], cur_set.virtual_shimming[Z], false);

            }

            // Tell the robot to recalculate the kinematics
            post_adjust_kinematics();


            // *****************************
            // * Re-center all test ranges *
            // *****************************

            test_delta_radius.reset_min_max();
            test_arm_length.reset_min_max();
            for(k=0; k<3; k++) {
                test_endstop[k].reset_min_max();
                test_delta_radius_offset[k].reset_min_max();
                test_tower_angle[k].reset_min_max();
                test_virtual_shimming[k].reset_min_max();
            }
            

            // ****************
            // * Housekeeping *
            // ****************

            if(try_mod_5 == 0) {
                float tempE = simulate_FK_and_get_energy(test_axis, cur_set.trim, cur_cartesian);
                _printf("Try %d of %d, energy=%1.3f (want <= %1.3f)\n", annealing_try, annealing_tries, tempE, global_target);

                // *****************************************************
                // * Keep track of last energy, and abort if it stalls *
                // *****************************************************

                // Shift the last_energy array right by one entry
                for(j = LAST_ENERGY_N - 1; j > 0; j--) {
                    last_energy[j] = last_energy[j - 1];
                }

                // Store the new entry
                last_energy[0] = tempE;

                // The count tells us whether the array is full, and therefore whether it's useful for running statistics
                if(++last_energy_count >= LAST_ENERGY_N) {

                    last_energy_count = LAST_ENERGY_N - 1;

                    // Calc stats
                    float mu, sigma, min, max;
                    calc_statistics(last_energy, LAST_ENERGY_N, mu, sigma, min, max);
                    
                    if(sigma < 0.01) {
                        _printf("Annealing has stalled - aborting.\n");
                        break;
                    }
                    
                    /*
                    // For debugging
                    _printf("Last energy counts: ");
                    for(j=0; j<LAST_ENERGY_N; j++) {
                        _printf("%1.3f ", last_energy[j]);
                    }
                    _printf("sigma=%1.3f\n", sigma);
                    */

                }
                
                // Abort if within the global target
                if(tempE <= global_target) {
                    _printf("Annealing : Within target\n");
                    break;
                }
            } // try_mod_5 == 0

            flush();


        } // annealing_try
        
        float endE = simulate_FK_and_get_energy(test_axis, cur_set.trim, cur_cartesian);
        newline();
        _printf("End of annealing pass (energy=%1.3f)\n", endE);
        
        if(endE <= global_target) {
            _printf("/!\\ SUCCESS /!\\\n");
            break;
        }


        _printf(" \n");

    } // outer_try


    // Print the results
    _printf("Heuristic calibration complete (energy=%1.3f). Final settings:\n", simulate_FK_and_get_energy(test_axis, cur_set.trim, cur_cartesian));

    // Normalize trim (this prevents downward creep)
    auto mm = std::minmax({ cur_set.trim[X], cur_set.trim[Y], cur_set.trim[Z] });
    cur_set.trim[X] -= mm.second;
    cur_set.trim[Y] -= mm.second;
    cur_set.trim[Z] -= mm.second;
    set_trim(cur_set.trim[X], cur_set.trim[Y], cur_set.trim[Z]);

    print_kinematics();
    
    newline();
    _printf("Final SIMULATED depths:\n");
    print_depths(cur_cartesian);

    newline();
    _printf("You can run this command again to see if it gets better, or type M500 to save.\n");

    pop_prefix();
    zprobe->home();
    
    return true;

}


// Find the most optimal configuration for a test function (e.g. set_delta_radius())
// (float version)
float ComprehensiveDeltaStrategy::find_optimal_config(bool (ComprehensiveDeltaStrategy::*test_function)(float, bool), float min, float max, float binsearch_width, float cartesian[DM_GRID_ELEMENTS][3], float target) {

    float energy_min, energy_max;
    
    // Find the direction of the most optimal configuration using a binary search
    for(int j=0; j<250; j++) {

        // Test energy at min & max

        ((this)->*test_function)(min, true);
        energy_min = simulate_FK_and_get_energy(test_axis, cur_set.trim, cartesian);

        ((this)->*test_function)(max, true);
        energy_max = simulate_FK_and_get_energy(test_axis, cur_set.trim, cartesian);

        // Who won?
        if(max - min <= target) {
            break;
        }
        if(energy_min < energy_max) {
            max -= ((max - min) * binsearch_width);
        }
        if(energy_min > energy_max) {
            min += ((max - min) * binsearch_width);
        }
    
    }

    return (min + max) / 2.0f;

} 


// Find the most optimal configuration for a test function (e.g. set_delta_radius())
// (float[3] version) 
float ComprehensiveDeltaStrategy::find_optimal_config(bool (ComprehensiveDeltaStrategy::*test_function)(float, float, float, bool), float values[3], int value_idx, float min, float max, float binsearch_width, float cartesian[DM_GRID_ELEMENTS][3], float target) {

    int j;
    float energy_min, energy_max;
    float save_val = values[value_idx];

    // Find the direction of the most optimal configuration using a binary search
    for(j=0; j<250; j++) {

        // Test energy at min & max
        values[value_idx] = min;
        ((this)->*test_function)(values[X], values[Y], values[Z], true);
        energy_min = simulate_FK_and_get_energy(test_axis, cur_set.trim, cartesian);

        values[value_idx] = max;
        ((this)->*test_function)(values[X], values[Y], values[Z], true);
        energy_max = simulate_FK_and_get_energy(test_axis, cur_set.trim, cartesian);

        // Who won?
        if(max - min <= target) {
            break;
        }
        if(energy_min < energy_max) {
            max -= ((max - min) * binsearch_width);
        }
        if(energy_min > energy_max) {
            min += ((max - min) * binsearch_width);
        }
    
    }

    values[value_idx] = save_val;
    return (min + max) / 2.0f;

} 


// find_optimal_config() requires a test function that takes three floats and returns a bool
bool ComprehensiveDeltaStrategy::set_test_trim(float x, float y, float z, bool dummy) {
    cur_set.trim[X] = x;
    cur_set.trim[Y] = y;
    cur_set.trim[Z] = z;
    return true;
}


bool ComprehensiveDeltaStrategy::set_test_virtual_shimming(float x, float y, float z, bool dummy) {
    cur_set.virtual_shimming[X] = x;
    cur_set.virtual_shimming[Y] = y;
    cur_set.virtual_shimming[Z] = z;
    set_virtual_shimming(x, y, z);
    return true;
}


// Move a random distance in the direction we just figured out in find_optimal_config()
void ComprehensiveDeltaStrategy::move_randomly_towards(float &value, float best, float temp, float target, float overrun_divisor) {

    float step = ( ((float)rand() / RAND_MAX) * temp ) + 0.001;

    if(best > value + target) {
        if(value + step > best) {
            step /= overrun_divisor;
        }
        value += step;
    }
    if(best < value - target) {
        if(value - step < best) {
            step /= overrun_divisor;
        }
        value -= step;
    }

}


// Simulate inverse (cartesian->actuator) kinematics
// cartesian[] will contain the generated test points
// test_axis[] (class member) will contain the generated axis positions
void ComprehensiveDeltaStrategy::simulate_IK(float cartesian[DM_GRID_ELEMENTS][3], float trim[3]) {

    float pos[3];

    for(int j = 0; j < DM_GRID_ELEMENTS; j++) {
    
        cartesian[j][X] = test_point[j][X];
        cartesian[j][Y] = test_point[j][Y];

        if(active_point[j] == TP_ACTIVE) {

            // Current cartesian coordinates of the depth map
            cartesian[j][Z] = depth_map[j].rel;
        
            pos[X] = cartesian[j][X];
            pos[Y] = cartesian[j][Y];
            pos[Z] = cartesian[j][Z];
            
            // Adjust Cartesian positions for surface transform plane (virtual shimming)
            if(surface_transform.plane_enabled) {
                pos[Z] += ((-surface_transform.normal[X] * pos[X]) - (surface_transform.normal[Y] * pos[Y]) - surface_transform.d) / surface_transform.normal[Z];
            }
            
            // Query the robot: Where do the axes have to be for the effector to be at these coordinates?
            THEKERNEL->robot->arm_solution->cartesian_to_actuator(pos, test_axis[j]);
        
            // Adjust axis positions to simulate the effects of trim
            test_axis[j][X] += trim[X];
            test_axis[j][Y] += trim[Y];
            test_axis[j][Z] += trim[Z];
        
        } else {
        
            cartesian[j][Z] = 0;
            test_axis[j][X] = 0;
            test_axis[j][Y] = 0;
            test_axis[j][Z] = 0;
        
        }
        
    } // for

}


// Simulate forward (actuator->cartesian) kinematics (returns the "energy" of the end result)
// The resulting cartesian coordinates are stored in cartesian[][]
float ComprehensiveDeltaStrategy::simulate_FK_and_get_energy(float axis_position[DM_GRID_ELEMENTS][3], float trim[3], float cartesian[DM_GRID_ELEMENTS][3]) {

    float trimmed[3];

    for(int j = 0; j < DM_GRID_ELEMENTS; j++) {

        if(active_point[j] == TP_ACTIVE) {
            trimmed[X] = axis_position[j][X] - trim[X];
            trimmed[Y] = axis_position[j][Y] - trim[Y];
            trimmed[Z] = axis_position[j][Z] - trim[Z];

            THEKERNEL->robot->arm_solution->actuator_to_cartesian(trimmed, cartesian[j]);

            // Adjust Cartesian positions for surface transform plane (virtual shimming)
            if(surface_transform.plane_enabled) {
                cartesian[j][Z] -= ((-surface_transform.normal[X] * cartesian[j][X]) - (surface_transform.normal[Y] * cartesian[j][Y]) - surface_transform.d) / surface_transform.normal[Z];
            }
        }
    }

    return calc_energy(cartesian);

}















// Find test_point[] array index of point closest to coordinates, taking the print surface shape into account
int ComprehensiveDeltaStrategy::find_nearest_test_point(float pos[2]) {

//_printf("FNTP for {%1.3f, %1.3f}\n", pos[X], pos[Y]);

    float dist;
    float lowest = 999;
    int lowest_idx = 0;
    float tp[2];

    for(int i=0; i<DM_GRID_ELEMENTS; i++) {

        tp[X] = test_point[i][X];
        tp[Y] = test_point[i][Y];
        dist = distance2D(pos, tp);
//_printf("Testing point %d: pos={%1.2f, %1.2f} dist=%1.2f, lowest=%1.2f: ", i, tp[X], tp[Y], dist, lowest);

        // FIXME: Are there any conditions where we'd want TP_ACTIVE_NEIGHBOR? Probably not...
        if(active_point[i] == TP_ACTIVE || active_point[i] == TP_CENTER) {
            if(dist < lowest) {
//__printf("Winner so far\n");
                lowest = dist;
                lowest_idx = i;
            } else {
//__printf("Not the winner\n");
            }
        } else {
//__printf("Point not active\n");
        }

    }

    return lowest_idx;

}


// Initialize test points to be used with G31 operations
void ComprehensiveDeltaStrategy::init_test_points() {

    // Initialize "test points" (grid)
    // -----------------------------------------------------
    // The grid is (2 * probe_radius) x (2 * probe_radius)
    float x, y;
    int n = 0;
    float point_spacing = (probe_radius * 2) / (DM_GRID_DIMENSION - 1);
    for(y = probe_radius; y >= -probe_radius; y-= point_spacing) {
        for(x = -probe_radius; x <= probe_radius; x += point_spacing) {
            test_point[n][X] = x;
            test_point[n][Y] = y;
            n++;
        }
    }
    
    // The method find_nearest_test_point() will only work once the above code is run.

    // Determine active points
    // -----------------------------------------------------
    float origin[2] = { 0, 0 };
//    int x, y, dm_pos;
    int dm_pos;
    float neighboring_probe_radius = probe_radius + (probe_radius / ((DM_GRID_DIMENSION - 1) / 2));

    //_printf("Probe radius: %1.3f - Neighboring probe radius: %1.3f\n", probe_radius, neighboring_probe_radius);

    // Determine active/inactive points based on print surface shape
    for(y=0; y<DM_GRID_DIMENSION; y++) {
        for(x=0; x<DM_GRID_DIMENSION; x++) {

            // Determine index of this grid position in the depth map array
            dm_pos = (y * DM_GRID_DIMENSION) + x;
    
            switch(surface_shape) {

                // Circle print shape requires determining which points are within probe_radius,
                // and which are their immediate neighbors outside probe_radius
                case PSS_CIRCLE:
                
                    if(distance2D(origin, test_point[dm_pos]) <= probe_radius) {

                        // Within probe radius, and NOT origin: Active
                        active_point[dm_pos] = TP_ACTIVE;

                    } else {

                        // We have to be super picky about what we make a neighbor
                        if(
                          distance2D(origin, test_point[dm_pos]) <= neighboring_probe_radius &&
                          y != 0 &&				// Not on the Y axis
                          y != (DM_GRID_DIMENSION - 1) &&	// Not on the top row
                          y != -(DM_GRID_DIMENSION - 1)) {	// Not on the bottom row

                            // Neighbor
                            active_point[dm_pos] = TP_ACTIVE_NEIGHBOR;

                        } else {

                            // Neither active or neighbor
                            active_point[dm_pos] = TP_INACTIVE;
                        }

                    }
                    break;
                
                // Square print shape is easy: everything is active!
                case PSS_SQUARE:
                    active_point[dm_pos] = TP_ACTIVE;
                    break;

            } // switch
        } // for x
    } // for y

    // Mark the origin point
    active_point[find_nearest_test_point(origin)] = TP_CENTER;

    /*
    // For testing
    for(y=0; y<DM_GRID_DIMENSION; y++) {
        for(x=0; x<DM_GRID_DIMENSION; x++) {
            dm_pos = (y * DM_GRID_DIMENSION) + x;

            __printf("%02d: ", dm_pos);
            switch(active_point[dm_pos]) {
                case TP_CENTER:		 __printf("center   "); break;
                case TP_ACTIVE:          __printf("active   "); break;
                case TP_ACTIVE_NEIGHBOR: __printf("neighbor "); break;
                case TP_INACTIVE:        __printf("inactive "); break;
            }

        }
        newline();
    }
    */


    // Initialize "tower points" (points nearest to a tower)
    // -----------------------------------------------------
    // Towers are 60 degrees off centerline.
    // So, the quadrants look like this:
    // Q2: -xDeg, +yDeg   Q1: +xDeg, +yDeg
    // Q3: -xDeg, -yDeg   Q4: +xDeg, -yDeg
    float xDeg = 0.866025f;
    float yDeg = 0.5;
    float pos[2];

    // Find center
    pos[X] = 0;
    pos[Y] = 0;
    tower_point_idx[TP_CTR] = find_nearest_test_point(pos);

    // Find X tower
    pos[X] = -xDeg * probe_radius;
    pos[Y] = -yDeg * probe_radius;
    tower_point_idx[TP_X] = find_nearest_test_point(pos);

    // Find Y tower
    pos[X] =  xDeg * probe_radius;
    pos[Y] = -yDeg * probe_radius;
    tower_point_idx[TP_Y] = find_nearest_test_point(pos);
    
    // Find Z tower
    pos[X] = 0;
    pos[Y] = probe_radius;
    tower_point_idx[TP_Z] = find_nearest_test_point(pos);

    surface_transform.tri_points[X][X] = test_point[tower_point_idx[TP_X]][X];
    surface_transform.tri_points[X][Y] = test_point[tower_point_idx[TP_X]][Y];
    surface_transform.tri_points[X][Z] = 0;

    surface_transform.tri_points[Y][X] = test_point[tower_point_idx[TP_Y]][X];
    surface_transform.tri_points[Y][Y] = test_point[tower_point_idx[TP_Y]][Y];
    surface_transform.tri_points[Y][Z] = 0;

    surface_transform.tri_points[Z][X] = test_point[tower_point_idx[TP_Z]][X];
    surface_transform.tri_points[Z][Y] = test_point[tower_point_idx[TP_Z]][Y];
    surface_transform.tri_points[Z][Z] = 0;

}


// Set the adjust function. This tells the kernel how to adjust Z for any point.
// I used ThreePointStrategy.cpp as an example.
void ComprehensiveDeltaStrategy::set_adjust_function(bool on) {

    surface_transform.active = on;

    if(on) {
//        _printf("[ST] Depth correction enabled.\n");
        THEKERNEL->robot->compensationTransform = [this](float target[3]) { target[Z] += this->get_adjust_z(target[X], target[Y]); };
    } else {
//        _printf("[ST] Depth correction disabled.\n");
        THEKERNEL->robot->compensationTransform = nullptr;
    }

}


// Figure out how far up or down we need to move the effector to conform to the print surface shape.
// There are two methods here, which can be used in tandem or separately.
// First, we can adjust Z by rotating the virtual plane, a la ThreePointStrategy.cpp.
// Second, we can bilinearly interpolate our coordinates relative to a depth map to approximate the
// correct depth.
//
// Because this is called hundreds of times per second, it has to run FAST. Therefore, stack variables
// are not used (except for whatever temporary ones the compiler uses to do math). Every variable that
// can be set aside beforehand, has been.
float ComprehensiveDeltaStrategy::get_adjust_z(float targetX, float targetY) {

    st_z_offset = 0;

    // Adjust Z according to the rotation of the plane of the print surface
    if(surface_transform.plane_enabled && surface_transform.active) {

        st_z_offset = ((-surface_transform.normal[X] * targetX) - (surface_transform.normal[Y] * targetY) - surface_transform.d) / surface_transform.normal[Z];

    }

    // Adjust Z according to depth map
    if(surface_transform.depth_enabled && surface_transform.active) {


        // Based on code retrieved from:
        // http://stackoverflow.com/questions/8808996/bilinear-interpolation-to-enlarge-bitmap-images

        // Determine which quad the point is in
        // Thx Liviu:
        // http://stackoverflow.com/questions/16592870/map-the-point-x-y-in-cartesian-coordinate-into-2d-array-elements
        // ----------------------------------------------------------------------
        // The print surface is in Cartesian.
        // Our array is in single-quadrant (origin at 0,0; X grows right and Y grows down).
        // Translate surface coordinates to array coordinates by adding the difference between coordinate systems.

        // Constrain data and calculate array positions & bounding box
        // ----------------------------------------------------------------------
        // Constrain tested points to probe radius

//static int count = 0;
//count = 0;
//if(count % 100 == 0 && zdebug) {
//    _printf("targetX=%1.3f targetY=%1.3f\n", targetX, targetY);
//    _printf("probe_radius=%1.3f scaler=%1.3f\n", probe_radius, bili.cartesian_to_array_scaler);
//
//}

        targetX = clamp(targetX, -probe_radius, probe_radius);
        targetY = clamp(targetY, -probe_radius, probe_radius);

        // Calculate (floating-point) array position
        bili.array_x = (targetX - -probe_radius) * bili.cartesian_to_array_scaler;
        bili.array_y = (-targetY - -probe_radius) * bili.cartesian_to_array_scaler;	// Y inverted since it starts high and ends low

        // Calculate bounding box
        bili.x1 = floor(bili.array_x);
        bili.y1 = floor(bili.array_y);
        bili.x2 = bili.x1 + 1;
        bili.y2 = bili.y1 + 1;
//if(count % 100 == 0 && zdebug) {
//    _printf("array_x=%1.3f array_y=%1.3f\n", bili.array_x, bili.array_y);
//    _printf("Bounding box: {%1.1f, %1.1f} to {%1.1f, %1.1f}\n", bili.x1, bili.y1, bili.x2, bili.y2);
//}


        // Calculate surface transform array indices for bounding box corners
        // ----------------------------------------------------------------------
        //  x1 ____________ x2  
        // y1 | Q11    Q21
        //    | 
        //    |
        // y2 | Q12    Q22
        bili.st_Q11 = (bili.y1 * DM_GRID_DIMENSION) + bili.x1;
        bili.st_Q12 = (bili.y2 * DM_GRID_DIMENSION) + bili.x1;
        bili.st_Q21 = (bili.y1 * DM_GRID_DIMENSION) + bili.x2;
        bili.st_Q22 = (bili.y2 * DM_GRID_DIMENSION) + bili.x2;

        // Retrieve heights from the quad's points
        // ----------------------------------------------------------------------
        bili.Q11 = surface_transform.depth[bili.st_Q11];
        bili.Q12 = surface_transform.depth[bili.st_Q12];
        bili.Q21 = surface_transform.depth[bili.st_Q21];
        bili.Q22 = surface_transform.depth[bili.st_Q22];

        // Set up the first terms
        // ----------------------------------------------------------------------
        bili.divisor = (bili.x2 - bili.x1) * (bili.y2 - bili.y1);
        bili.first_term[0] = bili.Q11 / bili.divisor;
        bili.first_term[1] = bili.Q21 / bili.divisor;
        bili.first_term[2] = bili.Q12 / bili.divisor;
        bili.first_term[3] = bili.Q22 / bili.divisor;

        // Set up the second and third terms
        // ----------------------------------------------------------------------
        bili.x2_minus_x = bili.x2 - bili.array_x;
        bili.x_minus_x1 = bili.array_x - bili.x1;
        bili.y2_minus_y = bili.y2 - bili.array_y;
        bili.y_minus_y1 = bili.array_y - bili.y1;

//if(count % 100 == 0 && zdebug) {
//    _printf("Indices for array entries for this BB: Q11=%d Q21=%d Q12=%d Q22=%d\n", bili.st_Q11, bili.st_Q21, bili.st_Q12, bili.st_Q22);
//    _printf("Heights: {%1.2f, %1.2f, %1.2f, %1.2f}\n", bili.Q11, bili.Q12, bili.Q21, bili.Q22);
//    _printf("First terms: %1.2f, %1.2f, %1.2f, %1.2f\n", bili.first_term[0], bili.first_term[1], bili.first_term[2], bili.first_term[3]);
//    _printf("Second & third terms: x2_minus_x=%1.2f; x_minus_x1=%1.2f; y2_minus_y=%1.2f; y_minus_y1=%1.2f\n", bili.x2_minus_x, bili.x_minus_x1, bili.y2_minus_y, bili.y_minus_y1);
//}

        // Interpolate    
        // ----------------------------------------------------------------------
        bili.result =
            bili.first_term[0] * bili.x2_minus_x * bili.y2_minus_y +
            bili.first_term[1] * bili.x_minus_x1 * bili.y2_minus_y +
            bili.first_term[2] * bili.x2_minus_x * bili.y_minus_y1 +
            bili.first_term[3] * bili.x_minus_x1 * bili.y_minus_y1;

        st_z_offset += bili.result;

//if(count % 100 == 0 && zdebug) {
//    _printf("st_z_offset = %1.3f\n", st_z_offset);
//}
//
//count++;

    }

    /*
    static int count = 0;
    if(count % 1000 == 0) {
        __printf("returning offset %1.3f\n", st_z_offset);
    }
    count++;
    */
    
    return st_z_offset;

}



// Measure probe tolerance (repeatability)
// Things that may have an impact on repeatability:
// - How tightly the probe is printed and/or built
// - Controller cooling, especially the stepper drivers
// - Noise from other wiring in the chassis
// - feedrate
// - debounce_count
// - probe_smoothing
bool ComprehensiveDeltaStrategy::measure_probe_repeatability(Gcode *gcode) {

    // Statistical variables
    int i;
    int steps;
    int nSamples = 10;
    float mu = 0;	// Mean
    float sigma = 0;	// Standard deviation
    float dev = 0;	// Sample deviation

    push_prefix("PR");

    // Options
    float want_acceleration = probe_acceleration;
    bool do_eccentricity_test = false;

    // Process G-code params, if any
    if(gcode != nullptr) {
        if(gcode->has_letter('A')) {
            want_acceleration = gcode->get_value('A');
            if(want_acceleration < 1 || want_acceleration > 1000) {
                want_acceleration = probe_acceleration;
            }
        }
        if(gcode->has_letter('B')) {
            int db = (int)gcode->get_value('B');
            if(db <    0) db = 0;
            if(db > 2000) db = 2000;
            zprobe->setDebounceCount(db);
        }
        if(gcode->has_letter('D')) {
            // This will be cast to a bool
            zprobe->setDecelerateOnTrigger((bool)gcode->get_value('D'));
        }
        if(gcode->has_letter('E')) {
            do_eccentricity_test = true;
        }
        if(gcode->has_letter('P')) {
            probe_smoothing = (unsigned int)gcode->get_value('P');
            if(probe_smoothing <  0) probe_smoothing = 0;
            if(probe_smoothing > 10) probe_smoothing = 10;
        }
        if(gcode->has_letter('Q')) {
            probe_priming = (unsigned int)gcode->get_value('Q');
            // If your probe takes more than 20 hits to settle, you should figure out why :(
            if(probe_priming <  0) probe_priming = 0;
            if(probe_priming > 20) probe_priming = 20;
        }
        if(gcode->has_letter('U')) {
            // ZProbe sanity-checks this already
            zprobe->setFastFeedrate(gcode->get_value('U'));
        }
        if(gcode->has_letter('V')) {
            // ZProbe sanity-checks this already
            zprobe->setSlowFeedrate(gcode->get_value('V'));
        }
        if(gcode->has_letter('S')) {
            nSamples = (int)gcode->get_value('S');
            if(nSamples > 30) {
                _printf("Too many samples!\n");
                pop_prefix();
                return false;
            }
        }
    }

    float sample[nSamples];
    if(probe_smoothing < 1) probe_smoothing = 1;
    if(probe_smoothing > 10) probe_smoothing = 10;

    // Print settings
    _printf("   Repeatability test: %d samples (S)\n", nSamples);
    _printf("     Acceleration (A): %1.1f\n", want_acceleration = 0 ? THEKERNEL->planner->get_acceleration() : want_acceleration);
    _printf("   Debounce count (B): %d\n", zprobe->getDebounceCount());
    _printf(" Smooth decel (D0|D1): %s\n", zprobe->getDecelerateOnTrigger() ? _STR_TRUE_ : _STR_FALSE_);
    _printf("Eccentricity test (E): %s\n", do_eccentricity_test ? _STR_ON_ : _STR_OFF_);
    _printf("  Probe smoothing (P): %d\n", probe_smoothing);
    _printf("    Probe priming (Q): %d\n", probe_priming);
    _printf("            Feedrates: Fast (U) = %1.3f, Slow (V) = %1.3f\n", zprobe->getFastFeedrate(), zprobe->getSlowFeedrate());
    _printf("1 step = %1.5f mm.\n", zprobe->zsteps_to_mm(1.0f));
 
    // Move into position, after safely determining the true bed height
    prepare_to_probe();

    // Prime the probe (run it a number of times to get it to "settle")
    if(!prime_probe()) {
        pop_prefix();
        return false;
    }

    float xDeg = 0.866025f;
    float yDeg = 0.5f;
    float radius = 10;// probe_radius;

    // Move the probe around to see if we can throw it off (e.g.: if it's loose, the printer has "delta arm blues", etc.)
    for(i=0; i<nSamples; i++) {

        if(do_eccentricity_test) {

            // Move towards X
            zprobe->coordinated_move(-xDeg * radius, -yDeg * radius, NAN, zprobe->getFastFeedrate(), false);
            zprobe->coordinated_move(0, 0, NAN, zprobe->getFastFeedrate(), false);
                
            // Move towards Y
            zprobe->coordinated_move(xDeg * radius, -yDeg * radius, NAN, zprobe->getFastFeedrate(), false);
            zprobe->coordinated_move(0, 0, NAN, zprobe->getFastFeedrate(), false);
                
            // Move towards Z
            zprobe->coordinated_move(0, radius, NAN, zprobe->getFastFeedrate(), false);
            zprobe->coordinated_move(0, 0, NAN, zprobe->getFastFeedrate(), false);

        }

        // Probe at center
        if(do_probe_at(steps, 0, 0)) {
            sample[i] = steps;
            _printf("Test %2d of %2d: Measured %d steps (%1.3f mm)\n", i + 1, nSamples, steps, zprobe->zsteps_to_mm(steps));
            if(steps > 50000) {
                _printf("Discarding result and trying again. Check probe_height.\n");
                i--;
            } else {
                mu += (float)steps;
            }
        } else {
            _printf("do_probe_at() returned false. Check probe_height.\n");
            pop_prefix();
            return false;
        }
    }
            
    // Mean
    mu /= nSamples;
            
    // Range and standard deviation
    int min=9999, max=0;
    for(i=0; i<nSamples; i++) {
        dev += powf((float)sample[i] - mu, 2);
        if(sample[i] < min) min = sample[i];
        if(sample[i] > max) max = sample[i];
    }
    sigma = sqrtf(dev/nSamples);

    // I dare anyone to tell me this should be an interquartile mean...
    float rep = zprobe->zsteps_to_mm(max - min);

    // Print stats
    _printf("Stats:\n");
    _printf("  range: %d steps (%1.4f mm)\n", max - min, zprobe->zsteps_to_mm(max - min));
    _printf("     mu: %1.3f steps (%1.3f mm)\n", mu, zprobe->zsteps_to_mm(mu));
    _printf("  sigma: %1.3f steps (%1.3f mm)\n", sigma, zprobe->zsteps_to_mm(sigma));
    _printf("Repeatability: %1.4f (add a little to be sure)\n", rep);

    if(best_probe_calibration.sigma == -1 || sigma < best_probe_calibration.sigma) {

        _printf("This is your best score so far!\n");
        best_probe_calibration.sigma = sigma;
        best_probe_calibration.range = max - min;
        best_probe_calibration.accel = want_acceleration;
        best_probe_calibration.debounce_count = zprobe->getDebounceCount();
        best_probe_calibration.decelerate = zprobe->getDecelerateOnTrigger();
        best_probe_calibration.eccentricity = do_eccentricity_test;
        best_probe_calibration.smoothing = probe_smoothing;
        best_probe_calibration.priming = probe_priming;
        best_probe_calibration.fast = zprobe->getFastFeedrate();
        best_probe_calibration.slow = zprobe->getSlowFeedrate();

    } else {

        _printf(
            "Best score so far: [sigma=%1.3f, range=%d] => accel=%f, debounce=%d, decelerate=%s, eccentricity=%s, smoothing=%d, priming=%d, fastFR=%1.3f, slowFR=%1.3f\n",
            best_probe_calibration.sigma,
            best_probe_calibration.range,
            best_probe_calibration.accel,
            best_probe_calibration.debounce_count,
            best_probe_calibration.decelerate ? _STR_TRUE_ : _STR_FALSE_,
            best_probe_calibration.eccentricity ? _STR_ON_ : _STR_OFF_,
            best_probe_calibration.smoothing,
            best_probe_calibration.priming,
            best_probe_calibration.fast,
            best_probe_calibration.slow
        );

    }

    // Print evaluation
    _printf("This score is ");
    if(rep < 0.015) {
        __printf("very good!");
    } else if(rep <= 0.03) {
        __printf("average.");
    } else if(rep <= 0.04) {
        __printf("borderline.");
    } else {
        __printf("HORRIBLE.");
    }
    newline();
    newline();

    pop_prefix();
    return true;

}



// Depth-map the print surface
// Initially useful for diagnostics, but the data may be useful for doing live height corrections
// Depths are stored in depth_map (class member)
bool ComprehensiveDeltaStrategy::depth_map_print_surface(float cartesian[DM_GRID_ELEMENTS][3], _cds_dmps_result display_results, bool extrapolate_neighbors) {

/*

    Probe-to-edge strategy
    
    PROBLEM:	With a 5x5 (or even 7x7) grid, selecting sample points based on whether they're within PROBE_RADIUS
                results in a diamond-shaped probing area that omits a lot of the periphery. This is no good!
    
    SOLUTION:	We have allocated memory for points outside the circle, so we can use them - we just can't probe them
                at their coordinates because they lie outside probe_radius. However, we CAN probe as close to it as
                possible and use that to interpolate the right value for it:

                * = test point (inside or outside probe_radius)
                / = edge of probe_radius

                Out  A     In
                *    /     *

                1: Sample at point A
                2: Calculate slope between In and A
                3: Project depth at Out based on that slope and its distance from In
                4: Set that depth as Out's depth
                
                Therefore, when the probe returns to point A in the future (with depth correction enabled), it will
                be at the same depth it determined before. We don't have to do anything to tell the interpolation
                routine, get_adjust_z(), because it already measures those points.

                Loading and saving will work in exactly the same way, as well.
    
                We should store the touch points for TP_X and TP_Y separately so that we can use them for the
                iterative calibration routine, and for adjusting the plane surface normal.

*/

    push_prefix("DM");

    int origin_steps;	// Steps from probe_height to bed surface at bed center
    int steps; 		// Steps from probe_height to bed surface at one of the test points

    float center[2] = {0, 0};
    int center_point = find_nearest_test_point(center);

    // Measure depth from probe_from_height at bed center

    prepare_to_probe();

    if(!prime_probe()) {
        _printf("Couldn't prime probe.\n");
        pop_prefix();
        return false;
    }

    if(do_probe_at(origin_steps, 0, 0)) {
        depth_map[center_point].rel = 0;
        depth_map[center_point].abs = zprobe->zsteps_to_mm(origin_steps);
        if(display_results != RESULTS_NONE) {
            _printf("Depth to bed surface at center: %d steps (%1.3f mm)\n", origin_steps, depth_map[TP_CTR].abs);
        }
    } else {
        _printf("Couldn't measure depth to origin.\n");
        pop_prefix();
        return false;
    }

//origin_steps = 700;

    // Measure depth from probe_height at all test points
    float best = 999;
    float worst = 0;

    // FIRST PASS: Depth-map all active points
    for(int i=0; i<DM_GRID_ELEMENTS; i++) {

        // If active_points_only, we only probe the points figured out in init_test_points(); else we probe them all
        // We don't probe TP_CTR because we already did above, in order to be able to store relative depths
        if(active_point[i] == TP_ACTIVE) {

            // Run the probe
            if(!do_probe_at(steps, test_point[i][X], test_point[i][Y])) {
                _printf("do_probe_at() returned false.\n");
                pop_prefix();
                return false;
            }
//steps = 600;

            // Store result in depth_map
            depth_map[i].rel = zprobe->zsteps_to_mm(origin_steps - steps);
            depth_map[i].abs = zprobe->zsteps_to_mm(steps);

            // ...And in cartesian[]
            // FIXME: I think there is a redundancy here... need to see how both arrays are used by callers.
            cartesian[i][X] = test_point[i][X];
            cartesian[i][Y] = test_point[i][Y];
            cartesian[i][Z] = depth_map[i].rel;

            // Do some statistics (sign doesn't matter, only magnitude)
            if(fabs(depth_map[i].rel) < fabs(best)) {
                best = fabs(depth_map[i].rel);
            }

            if(fabs(depth_map[i].rel) > fabs(worst)) {
                worst = fabs(depth_map[i].rel);
            }

            if(display_results == RESULTS_UNFORMATTED) {

                // We're going to plainly print the results, one by one, with no special formatting
                _printf("Depth: %1.3fmm (%1.3fmm absolute)\n", depth_map[i].rel, depth_map[i].abs);

            }

            flush();

        }
    }
    
    // SECOND PASS: Probe neighboring-active points and interpolate.
    // We're doing two loops because it would have been a hassle to make one loop do everything.
    // The points are probed in array order, and the active-neigbhor points on the left can't be computed
    // until their within-radius neighbors' heights are known.
    if(extrapolate_neighbors) {

        for(int i=0; i<DM_GRID_ELEMENTS; i++) {

            if(active_point[i] == TP_ACTIVE_NEIGHBOR) {

                float coords[2];
                int active_idx;

                // X is the coordinate at print_radius.
                // Equation - complete the squares: x^2 + y^2 = probe_radius^2 - solve for x.
                // ...
                // x^2 = probe_radius^2 - y^2
                // x = sqrt(probe_radius^2 - y^2)
                coords[X] = sqrt( (probe_radius * probe_radius) - (test_point[i][Y] * test_point[i][Y]) );

                // Necessary to flip coords in Q2/3 because the sqrt(... code above only produces positive results.
                // Technically, the equation produces "two" answers because by definition, there are TWO X coords
                // for any given Y - one on the left side of the circle, and the other on the right side.
                if(test_point[i][X] > 0) {
                    active_idx = i - 1;	// Neighboring point is to the left
                } else {
                    active_idx = i + 1;	// Neighboring point is to the right
                    coords[X] = -coords[X];
                }
                
                // Y coordinate is the same whether active or active-neighbor
                coords[Y] = test_point[i][Y];

                // Run the probe
                if(!do_probe_at(steps, coords[X], coords[Y])) {
                    _printf("do_probe_at() returned false.\n");
                    pop_prefix();
                    return false;
                }
                // steps = 500;

                // To extrapolate, we need the depths of the active-neighbor, and its associated active point
                struct point_type {
                    float x;
                    float y;
                    _cds_depths_t z;
                };

                // Extrapolate depth at test_point[i] based on the slope between the depths of the active test point & probed point
                point_type active { test_point[active_idx][X], test_point[active_idx][Y], { depth_map[active_idx].abs, depth_map[active_idx].rel } };
                point_type probed { coords[X], coords[Y], { zprobe->zsteps_to_mm(steps), zprobe->zsteps_to_mm(origin_steps - steps) } };
                point_type extrap { test_point[i][X], test_point[i][Y], { 0, 0 } };

                //_printf("neighbor=%d, active=%d\n", i, active_idx);
                //_printf("active = {%1.3f, %1.3f, %1.3f | %1.3f}\n", active.x, active.y, active.z.abs, active.z.rel);
                //_printf("probed = {%1.3f, %1.3f, %1.3f | %1.3f}\n", probed.x, probed.y, probed.z.abs, probed.z.rel);
                //_printf("...\n");

                _cds_depths_t rise { probed.z.abs - active.z.abs, probed.z.rel - active.z.rel };
                float dist_active_to_extrap = sqrt(pow(extrap.x - active.x, 2));
                float dist_active_to_probed = sqrt(pow(probed.x - active.x, 2));
                float dist_mul = dist_active_to_extrap / dist_active_to_probed; // This will be 1.something

                //_printf("rise = %1.3f | %1.3f\n", rise.abs, rise.rel);
                //_printf("dist active to extrap = %1.3f\n", dist_active_to_extrap);
                //_printf("dist active to probed = %1.3f\n", dist_active_to_probed);
                //_printf("dist mul = %1.3f\n", dist_mul);

                extrap.z.abs = active.z.abs + (rise.abs * dist_mul);
                extrap.z.rel = zprobe->zsteps_to_mm(origin_steps) - extrap.z.abs;

                //_printf("extrap = {%1.3f, %1.3f, %1.3f | %1.3f}\n", extrap.x, extrap.y, extrap.z.abs, extrap.z.rel);
                //newline();

                // Store result in depth_map
                depth_map[i].rel = extrap.z.rel;
                depth_map[i].abs = extrap.z.abs;

                // ...And in cartesian[]
                // FIXME: I think there is a redundancy here... need to see how both arrays are used by callers.
                cartesian[i][X] = test_point[i][X];
                cartesian[i][Y] = test_point[i][Y];
                cartesian[i][Z] = depth_map[i].rel;


            } // if TP_ACTIVE_NEIGHBOR

        } // for i

    } else {

        for(int i=0; i<DM_GRID_ELEMENTS; i++) {

            if(active_point[i] == TP_ACTIVE_NEIGHBOR) {

                depth_map[i].abs = 0;
                depth_map[i].rel = 0;
                cartesian[i][X] = test_point[i][X];
                cartesian[i][Y] = test_point[i][Y];
                cartesian[i][Z] = 0;

            }

        }

    } // if(extrapolate_neighbors)

    // Show the results (pretty)
    if(display_results == RESULTS_FORMATTED) {
        print_depths(depth_map);
    }

    pop_prefix();
    return true;

}


// Perform a GeneB-style calibration on the endstops and delta radius.
// Unlike GeneB's method, this converges both at the same time and should produce a slightly better calibration.
// It's a good idea to run this before the heuristic calibration, so it has a good starting point.
bool ComprehensiveDeltaStrategy::iterative_calibration(bool keep_settings) {

    push_prefix("IC");
    print_task_with_warning("Iterative calibration");

    zero_depth_maps();
    set_adjust_function(false);		// Surface plane can confound this method
    
    if(keep_settings) {
        _printf("Keeping kinematics.\n");
    } else {
        _printf("Resetting kinematics.\n");
        set_trim(0, 0, 0);
        set_tower_radius_offsets(0, 0, 0);
        set_tower_angle_offsets(0, 0, 0);
        set_tower_arm_offsets(0, 0, 0);
        set_virtual_shimming(0, 0, 0);
    }

    _printf("Current kinematics:\n");
    print_kinematics();

    // Init test points specific to this routine (we don't use the grid)
    // -----------------------------------------------------------------
    // Towers are 60 degrees off centerline.
    // So, the quadrants look like this:
    // Q2: -xDeg, +yDeg   Q1: +xDeg, +yDeg
    // Q3: -xDeg, -yDeg   Q4: +xDeg, -yDeg
    float xDeg = 0.866025f;
    float yDeg = 0.5;
    float tower[3][2];	// [tower][xy]

    // X tower
    tower[X][X] = -xDeg * probe_radius;
    tower[X][Y] = -yDeg * probe_radius;

    // Y tower
    tower[Y][X] =  xDeg * probe_radius;
    tower[Y][Y] = -yDeg * probe_radius;
    
    // Z tower
    tower[Z][X] = 0;
    tower[Z][Y] = probe_radius;

    // Different calibration types can be turned on and off
    // For now we only do endstops and delta radius, but other types can be added as well
    caltype.endstop.active = true;
    caltype.delta_radius.active = true;

    // This is the target accuracy. 30 microns is pretty good.
    float target = 0.03;

    // Steps from probe height to trigger
    int steps;

    // Indexed by TP_CTR|X|Y|Z
    float depth[4];

    // Main loop
    for(int outer_i = 0; outer_i < 20; outer_i++) {

        // Banner preceded by line break for easy visual parsing
        newline();
        _printf("Iteration %d (max %d)\n", outer_i + 1, 20);
    
        // Determine center height
        prepare_to_probe();
        if(!prime_probe()) {
            pop_prefix();
            return false;
        }
        if(do_probe_at(steps, 0, 0)) {
            depth[TP_CTR] = zprobe->zsteps_to_mm(steps);
        } else {
            pop_prefix();
            return false;
        }
        
        // Determine depth near each tower
        if(!do_probe_at(steps, tower[X][X], tower[X][Y])) {
            pop_prefix();
            return false;
        }
        depth[TP_X] = zprobe->zsteps_to_mm(steps);

        if(!do_probe_at(steps, tower[Y][X], tower[Y][Y])) {
            pop_prefix();
            return false;
        }
        depth[TP_Y] = zprobe->zsteps_to_mm(steps);

        if(!do_probe_at(steps, tower[Z][X], tower[Z][Y])) {
            pop_prefix();
            return false;
        }
        depth[TP_Z] = zprobe->zsteps_to_mm(steps);

        // Deviation for towers
        // These are measured for all calibration types
        auto tower_minmax = std::minmax({ depth[TP_CTR], depth[TP_X], depth[TP_Y], depth[TP_Z] });
        float tower_deviation = tower_minmax.second - tower_minmax.first;

        // Do we calibrate the endstops?
        if(caltype.endstop.active) {

            // ****************
            // *** ENDSTOPS ***
            // ****************
            
            push_prefix("ES");
            
            // Deviation and trimscale
            static float last_deviation;
            static float trimscale;
                
            // Do we need to reset the variables?
            if(caltype.endstop.needs_reset) {
                last_deviation = 999;
                trimscale = 1.3F;
                caltype.endstop.needs_reset = false;
            }

            _printf("Endstops: Difference => %1.3f (want %1.3f)", tower_deviation, target);

            // Deviation within tolerance?
            if(fabs(tower_deviation) <= target) {
                
                // Yep
                newline();
                _printf("Endstops are within tolerance.\n");
                caltype.endstop.in_tolerance = true;
                    
            } else {
                
                // Nope
                __printf(", out of tolerance by %1.3f.\n", tower_deviation - target);
                caltype.endstop.in_tolerance = false;
                    
                // Get trim
                float trim[3];
                if(!get_trim(trim[X], trim[Y], trim[Z])) {
                    _printf("Couldn't query trim.\n");
                    pop_prefix();
                    return false;
                }
                    
                // Sanity-check the trim
                if(trim[X] > 0) trim[X] = 0;
                if(trim[Y] > 0) trim[Y] = 0;
                if(trim[Z] > 0) trim[Z] = 0;
                
                if(trim[X] < -5 || trim[Y] < -5 || trim[Z] < -5) {
                    _printf("Trim: {%1.3f, %1.3f, %1.3f}\n", trim[X], trim[Y], trim[Z]);
                    _printf("Values less than -5 suggest that something is horribly wrong.\n");
                    pop_prefix();
                    return false;
                }
                    
                // If things stayed the same or got worse, we reduce the trimscale
                if((tower_deviation >= last_deviation) && (trimscale * 0.95 >= 0.9)) {  
                    trimscale *= 0.9;
                    _printf("/!\\ Deviation same or worse vs. last time - reducing trim scale to %1.3f\n", trimscale);
                }
                last_deviation = tower_deviation;
                    
                // Set all towers' trims
                trim[X] += (tower_minmax.first - depth[TP_X]) * trimscale;
                trim[Y] += (tower_minmax.first - depth[TP_Y]) * trimscale;
                trim[Z] += (tower_minmax.first - depth[TP_Z]) * trimscale;
                    
                // Correct the downward creep issue by normalizing the trim offsets
                auto mm = std::minmax({trim[X], trim[Y], trim[Z]});
                trim[X] -= mm.second;
                trim[Y] -= mm.second;
                trim[Z] -= mm.second;
                _printf("Setting endstops to {%1.3f, %1.3f, %1.3f}.\n", trim[X], trim[Y], trim[Z]);
                    
                set_trim(trim[X], trim[Y], trim[Z]);
                    
            }
            
            pop_prefix();

        } // caltype.endstop.active
        
        
        if(caltype.delta_radius.active) {

            // ********************                
            // *** DELTA RADIUS ***
            // ********************
            
            push_prefix("DR");
            
            float dr_factor = 2.0;
                
            // Retrieve delta radius or die trying
            float delta_radius;
            if(!get_delta_radius(delta_radius)) {
                _printf("Couldn't query delta_radius.\n");
                pop_prefix();
                return false;
            }

            // Examine differences between tower depths and use this to adjust delta_radius
            float avg = (depth[TP_X] + depth[TP_Y] + depth[TP_Z]) / 3.0;
            float deviation = depth[TP_CTR] - avg;
            _printf("Delta Radius - Depths: Center=%1.3f, Tower average=%1.3f => Difference: %1.3f (want %1.3f)\n", depth[TP_CTR], avg, deviation, target);
            _printf("Delta radius is ");

            // Deviation within tolerance?
            if(fabs(deviation) <= target) {

                // Yep
                __printf("within tolerance.\n");
                caltype.delta_radius.in_tolerance = true;

            } else {

                // Nope
                __printf("out of tolerance by %1.3f.\n", deviation - target);
                caltype.delta_radius.in_tolerance = false;
                    
                _printf("Changing delta radius from %1.3f to ", delta_radius);
                delta_radius += (deviation * dr_factor);
                __printf("%1.3f\n", delta_radius);
                set_delta_radius(delta_radius);

            }

            pop_prefix();

        } // caltype.delta_radius.active


        // Done with ALL tasks?
        // Right now this only does the endstops & delta radius, but more can be added later.
        if(caltype.endstop.in_tolerance && caltype.delta_radius.in_tolerance) {
            newline();
            print_kinematics();
            newline();
            _printf("All done! Save settings with M500.\n");
            pop_prefix();
            zprobe->home();
            return true;
        }


    } // for outer_i

    _printf("Maximum tries exceeded. If this is good enough, type M500 to save.\n");
    pop_prefix();
    return true;

}


// Prepare to probe
void ComprehensiveDeltaStrategy::prepare_to_probe() {

    // Determine bed_height, probe_from_height, and probe_height_to_trigger
    if(probe_from_height == -1) {
        find_bed_center_height();
    }

    // Home the machine
    zprobe->home();

    // Do a relative move to an depth of probe_height
    zprobe->coordinated_move(NAN, NAN, -probe_from_height, zprobe->getFastFeedrate(), true);

}


// Enforce clean geometry
bool ComprehensiveDeltaStrategy::require_clean_geometry() {

    if(geom_dirty) {
        __printf("[EC] Geometry has been changed - recalibrating.\n");
        if(!iterative_calibration(false)) return false;
        if(!find_bed_center_height(true)) return false;		// Reset probe_from_height, since the endstop trim may have been changed
        geom_dirty = false;
    }

    return true;

}


// Prime the probe, if set
bool ComprehensiveDeltaStrategy::prime_probe() {

    if(probe_priming) {
        int i, steps;
        __printf("[PR] Priming probe %d times.\n", probe_priming);
        for(i=0; i<probe_priming; i++) {
            if(!do_probe_at(steps, 0, 0)) {
                return false;
            }
        }
    }
    return true;

}


// Probe the center of the bed to determine its height in steps, taking probe offsets into account.
// Refreshes the following variables, AND SHOULD BE CALLED BEFORE READING THEM:
//	bed_height
//	probe_from_height
// 	mm_probe_height_to_trigger
bool ComprehensiveDeltaStrategy::find_bed_center_height(bool reset_all) {

    push_prefix("BH");

    // Step counter
    int steps;

    // Start from the top
    zprobe->home();

    // Did they ask for a complete reset? (This means we have to re-find bed center height)
    if(reset_all) {
        probe_from_height = -1;
    }
 
    // If we haven't determined the probe-from height yet, do so now
    // We'll remember it until the machine is reset
    if(probe_from_height == -1) {

        // Fast the first time
        _printf("Determining the probe-from height.\n");
        zprobe->run_probe(steps, true);
        
        // Probe from height = total measured height - height required for the probe not to drag
        probe_from_height = zprobe->zsteps_to_mm(steps) - zprobe->getProbeHeight();
        zprobe->home();

    } else {
        _printf("Probe-from height = %1.3f\n", probe_from_height);
    }

    // Move to probe_from_height (relative move!)
    zprobe->coordinated_move(NAN, NAN, -probe_from_height, zprobe->getFastFeedrate(), true);
    
    // Prime the probe - this measurement is one of the most important!
    if(!prime_probe()) {
        pop_prefix();
        return false;
    }
    
    // Move to probing offset
    // We do these as two seperate steps because the top of a delta's build envelope is domed,
    // and we want to avoid the possibility of asking the effector to move somewhere it can't
    zprobe->coordinated_move(probe_offset_x, probe_offset_y, NAN, zprobe->getFastFeedrate(), false);

    // Now, slowly probe the depth
    save_acceleration();
    set_acceleration(probe_acceleration);
    if(!zprobe->run_probe(steps, false)) {
        restore_acceleration();
        pop_prefix();
        return false;
    }
    restore_acceleration();
    mm_probe_height_to_trigger = zprobe->zsteps_to_mm(steps);

    // Set final bed height
    bed_height = probe_from_height + mm_probe_height_to_trigger + probe_offset_z;

    // Tell the machine about the new height
    // FIXME: Endstops.cpp might have a more direct method for doing this - if so, that should be used instead!
    // -- Construct command
    char cmd[18];       // Should be enough for "M665 Z1000.12345"
    snprintf(cmd, 17, "M665 Z%1.5f", bed_height);
    
    // -- Send command
    struct SerialMessage message;
    message.message = cmd;
    message.stream = &(StreamOutput::NullStream);
    THEKERNEL->call_event(ON_CONSOLE_LINE_RECEIVED, &message);
    THEKERNEL->conveyor->wait_for_empty_queue();

    _printf("Bed height set to %1.3f\n", bed_height);

    pop_prefix();
    return true;

}


// Do a probe at a specified (X, Y) location, taking probe offset into account
bool ComprehensiveDeltaStrategy::do_probe_at(int &steps, float x, float y, bool skip_smoothing) {

    // Move to location, corrected for probe offset (if any)
    zprobe->coordinated_move(x + probe_offset_x, y + probe_offset_y, NAN, zprobe->getFastFeedrate(), false);

    // Run the number of tests specified in probe_smoothing
    steps = 0;
    int smoothing, result;
    if(skip_smoothing) {
        smoothing = 1;
    } else {
        smoothing = probe_smoothing;
    }

    save_acceleration();
    set_acceleration(probe_acceleration);

    for(int i=0; i < smoothing; i++) {
        // Run the probe
        if(!zprobe->run_probe(result)) {
            if(i != 0) steps /= i;
            __printf("[DP] do_probe_at(steps, %1.3f, %1.3f) - run_probe() returned false, s=%d.\n", x + probe_offset_x, y + probe_offset_y, steps);
            restore_acceleration();
            return false;
        }

        // Return probe to original Z
        if(zprobe->getDecelerateOnTrigger()) {
            zprobe->return_probe(zprobe->getStepsAtDecelEnd());
        } else {
            zprobe->return_probe(result);
        }

        // Add to accumulator
        steps += result;

    }

    restore_acceleration();
    
    // Average
    steps /= smoothing;

    // Sanity check
    if(steps < 100) {
        __printf("[DP] do_probe_at(): steps=%d - this is much too small - is probe_height high enough?\n", steps);
        return false;
    } else {
        return true;
    }
}


// The printer has to have its position refreshed when the kinematics change. Otherwise, it will jerk violently the
// next time it moves, because its last milestone (location) was calculated using the previous kinematics.
void ComprehensiveDeltaStrategy::post_adjust_kinematics() {

    float pos[3];
    THEKERNEL->robot->get_axis_position(pos);
    THEKERNEL->robot->reset_axis_position(pos[0], pos[1], pos[2]);

}


// This is the version you want to use if you're fiddling with the endstops. Note that endstop
// offset values are NEGATIVE (steps down).
void ComprehensiveDeltaStrategy::post_adjust_kinematics(float offset[3]) {

    float pos[3];
    THEKERNEL->robot->get_axis_position(pos);
    THEKERNEL->robot->reset_axis_position(pos[0] + offset[0], pos[1] + offset[1], pos[2] + offset[2]);
    geom_dirty = true;

}


// Following are getters/setters for global accelration (not Z-specific)
void ComprehensiveDeltaStrategy::save_acceleration() {
    saved_acceleration = THEKERNEL->planner->get_acceleration();
}

void ComprehensiveDeltaStrategy::restore_acceleration() {
    set_acceleration(saved_acceleration);
}

void ComprehensiveDeltaStrategy::set_acceleration(float a) {

    char cmd[20];       // Should be enough for "M204 S1234.45678"
    snprintf(cmd, 19, "M204 S%1.5f", a);
    // -- Send command
    struct SerialMessage message;
    message.message = cmd;
    message.stream = &(StreamOutput::NullStream);
    THEKERNEL->call_event(ON_CONSOLE_LINE_RECEIVED, &message);
    THEKERNEL->conveyor->wait_for_empty_queue();

}


// Following are getters/setters for endstops
bool ComprehensiveDeltaStrategy::set_trim(float x, float y, float z) {

    float t[3] {x, y, z};
    bool ok = PublicData::set_value( endstops_checksum, trim_checksum, t);

    if (ok) {
//        __printf("[ES] Set trim to: X=%f Y=%f Z=%f\n", x, y, z);
    } else {
        __printf("[ES] Unable to set trim. Are endstops enabled?\n");
    }

    return ok;
}

bool ComprehensiveDeltaStrategy::get_trim(float &x, float &y, float &z) {

    void *returned_data;
    bool ok = PublicData::get_value( endstops_checksum, trim_checksum, &returned_data );

    if (ok) {
        float *trim = static_cast<float *>(returned_data);
        x = trim[0];
        y = trim[1];
        z = trim[2];
        return true;
    }
    return false;
}


// Following are getters/setters for delta geometry variables

// Arm length
bool ComprehensiveDeltaStrategy::set_arm_length(float arm_length, bool update) {

    options['L'] = arm_length;
    if(THEKERNEL->robot->arm_solution->set_optional(options)) {
        if(update) {
            post_adjust_kinematics();
        }
        return true;
    } else {
        return false;
    }

}

bool ComprehensiveDeltaStrategy::get_arm_length(float &arm_length) {

    if(THEKERNEL->robot->arm_solution->get_optional(options)) {
        arm_length = options['L'];
        return true;
    } else {
        return false;
    }

}


// Delta radius
bool ComprehensiveDeltaStrategy::set_delta_radius(float delta_radius, bool update) {

    options['R'] = delta_radius;
    if(THEKERNEL->robot->arm_solution->set_optional(options)) {
        if(update) {
            post_adjust_kinematics();
        }
        return true;
    } else {
        return false;
    }

}

bool ComprehensiveDeltaStrategy::get_delta_radius(float &delta_radius) {

    if(THEKERNEL->robot->arm_solution->get_optional(options)) {
        delta_radius = options['R'];
        return true;
    } else {
        return false;
    }

}


// Tower radius offsets
bool ComprehensiveDeltaStrategy::set_tower_radius_offsets(float x, float y, float z, bool update) {

    options['A'] = x;
    options['B'] = y;
    options['C'] = z;
    if(THEKERNEL->robot->arm_solution->set_optional(options)) {
        if(update) {
            post_adjust_kinematics();
        }
        return true;
    } else {
        return false;
    }

}

bool ComprehensiveDeltaStrategy::get_tower_radius_offsets(float &x, float &y, float &z) {

    if(THEKERNEL->robot->arm_solution->get_optional(options)) {
        x = options['A'];
        y = options['B'];
        z = options['C'];
        return true;
    } else {
        return false;
    }

}


// Tower angle offsets
bool ComprehensiveDeltaStrategy::set_tower_angle_offsets(float x, float y, float z, bool update) {

    options['D'] = x;
    options['E'] = y;
    options['F'] = z;
    if(THEKERNEL->robot->arm_solution->set_optional(options)) {
        if(update) {
            post_adjust_kinematics();
        }
        return true;
    } else {
        return false;
    }

}

bool ComprehensiveDeltaStrategy::get_tower_angle_offsets(float &x, float &y, float &z) {

    if(THEKERNEL->robot->arm_solution->get_optional(options)) {
        x = options['D'];
        y = options['E'];
        z = options['F'];
        return true;
    } else {
        return false;
    }

}


// Arm length offsets
bool ComprehensiveDeltaStrategy::set_tower_arm_offsets(float x, float y, float z, bool update) {

    options['T'] = x;
    options['U'] = y;
    options['V'] = z;
    if(THEKERNEL->robot->arm_solution->set_optional(options)) {
        if(update) {
            post_adjust_kinematics();
        }
        return true;
    } else {
        return false;
    }

}

bool ComprehensiveDeltaStrategy::get_tower_arm_offsets(float &x, float &y, float &z) {

    if(THEKERNEL->robot->arm_solution->get_optional(options)) {
        x = options['T'];
        y = options['U'];
        z = options['V'];
        return true;
    } else {
        return false;
    }

}


// Virtual Shimming (thx Plane3d.cpp)
bool ComprehensiveDeltaStrategy::set_virtual_shimming(float x, float y, float z, bool update) {

//_printf("SVS: {%1.3f, %1.3f, %1.3f}\n", x, y, z);

    // Z depths are in millimeters relative to surface, negative=lower
    surface_transform.tri_points[X][Z] = x;
    surface_transform.tri_points[Y][Z] = y;
    surface_transform.tri_points[Z][Z] = z;



    if(x == 0 && y == 0 && z == 0) {

        // This gets its own special case because Vector3.cpp is incapable of handling null vectors.
        // It will literally calculate that the cross product of {0, 0, 0} and {0, 0, 0} is {nan, nan, nan}.
        surface_transform.normal.set(0, 0, 1);
        surface_transform.d = 0;

    } else {

        Vector3 v1, v2, v3;
        v1.set(surface_transform.tri_points[X][X], surface_transform.tri_points[X][Y], surface_transform.tri_points[X][Z]);
        v2.set(surface_transform.tri_points[Y][X], surface_transform.tri_points[Y][Y], surface_transform.tri_points[Y][Z]);
        v3.set(surface_transform.tri_points[Z][X], surface_transform.tri_points[Z][Y], surface_transform.tri_points[Z][Z]);

//        _printf("Vector 1: {%1.3f, %1.3f, %1.3f}\n", v1[X], v1[Y], v1[Z]);
//        _printf("Vector 2: {%1.3f, %1.3f, %1.3f}\n", v2[X], v2[Y], v2[Z]);
//        _printf("Vector 3: {%1.3f, %1.3f, %1.3f}\n", v3[X], v3[Y], v3[Z]);

        Vector3 ab = v1.sub(v2);
        Vector3 ac = v1.sub(v3);
        Vector3 cross_product = ab.cross(ac);

        surface_transform.normal = cross_product.unit();

//        _printf("ab: {%1.3f, %1.3f, %1.3f}\n", ab[X], ab[Y], ab[Z]);
//        _printf("ac: {%1.3f, %1.3f, %1.3f}\n", ac[X], ac[Y], ac[Z]);
//        _printf("cross product: {%1.3f, %1.3f, %1.3f}\n", cross_product[X], cross_product[Y], cross_product[Z]);
//        _printf("normal: {%1.3f, %1.3f, %1.3f}\n", surface_transform.normal[X], surface_transform.normal[Y], surface_transform.normal[Z]);

        Vector3 dv = surface_transform.normal.mul(v1);
//        _printf("dv: {%1.3f, %1.3f, %1.3f}\n", dv[X], dv[Y], dv[Z]);

        surface_transform.d = -dv[0] - dv[1] - dv[2];
//        _printf("d: %1.3f\n", surface_transform.d);

        surface_transform.plane_enabled = true;
        set_adjust_function(true);

    }

//    _printf("normal: {%1.3f, %1.3f, %1.3f}\n", surface_transform.normal[X], surface_transform.normal[Y], surface_transform.normal[Z]);
//    _printf("d: %1.3f\n", surface_transform.d);

    surface_transform.have_normal = true;
    return true;
    
}

bool ComprehensiveDeltaStrategy::get_virtual_shimming(float &x, float &y, float &z) {
    if(surface_transform.plane_enabled) {
        x = surface_transform.tri_points[X][Z];
        y = surface_transform.tri_points[Y][Z];
        z = surface_transform.tri_points[Z][Z];
    } else {
        x = y = z = 0;
    }
    return true;
}


// Getter/setter for ALL kinematics
bool ComprehensiveDeltaStrategy::set_kinematics(KinematicSettings settings, bool update) {

    if(settings.initialized) {

        set_delta_radius(settings.delta_radius);
        set_arm_length(settings.arm_length);
        set_trim(settings.trim[X], settings.trim[Y], settings.trim[Z]);
        set_tower_radius_offsets(settings.tower_radius[X], settings.tower_radius[Y], settings.tower_radius[Z]);
        set_tower_angle_offsets(settings.tower_angle[X], settings.tower_angle[Y], settings.tower_angle[Z]);
        set_tower_arm_offsets(settings.tower_arm[X], settings.tower_arm[Y], settings.tower_arm[Z]);
        set_virtual_shimming(settings.virtual_shimming[X], settings.virtual_shimming[Y], settings.virtual_shimming[Z]);

        if(update) {
            post_adjust_kinematics();
        }

        return true;

    } else {

        __printf("[SK] Tried to set kinematics to uninitialized settings!\n");
        return false;

    }
}

bool ComprehensiveDeltaStrategy::get_kinematics(KinematicSettings &settings) {

    get_delta_radius(settings.delta_radius);
    get_arm_length(settings.arm_length);
    get_trim(settings.trim[X], settings.trim[Y], settings.trim[Z]);
    get_tower_radius_offsets(settings.tower_radius[X], settings.tower_radius[Y], settings.tower_radius[Z]);
    get_tower_angle_offsets(settings.tower_angle[X], settings.tower_angle[Y], settings.tower_angle[Z]);
    get_tower_arm_offsets(settings.tower_arm[X], settings.tower_arm[Y], settings.tower_arm[Z]);
    get_virtual_shimming(settings.virtual_shimming[X], settings.virtual_shimming[Y], settings.virtual_shimming[Z]);
    settings.initialized = true;
    return true;

}


// Print currently set kinematics
void ComprehensiveDeltaStrategy::print_kinematics() {

    KinematicSettings settings;
    get_kinematics(settings);
    print_kinematics(settings);

}

void ComprehensiveDeltaStrategy::print_kinematics(KinematicSettings settings) {

    push_prefix("PK");
    _printf("          Arm length: %1.3f\n", settings.arm_length);
    _printf("        Delta radius: %1.3f\n", settings.delta_radius);
    _printf("     Endstop offsets: {%1.3f, %1.3f, %1.3f}\n", settings.trim[X], settings.trim[Y], settings.trim[Z]);
    _printf("Radius offsets (ABC): {%1.3f, %1.3f, %1.3f}\n", settings.tower_radius[X], settings.tower_radius[Y], settings.tower_radius[Z]);
    _printf(" Angle offsets (DEF): {%1.3f, %1.3f, %1.3f}\n", settings.tower_angle[X], settings.tower_angle[Y], settings.tower_angle[Z]);
    _printf("    Virtual shimming: {%1.3f, %1.3f, %1.3f}, vector={%1.3f, %1.3f, %1.3f}, d=%1.3f, %s\n", settings.virtual_shimming[X], settings.virtual_shimming[Y], settings.virtual_shimming[Z], surface_transform.normal[X], surface_transform.normal[Y], surface_transform.normal[Z], surface_transform.d, (surface_transform.plane_enabled && surface_transform.active) ? _STR_ENABLED_ : _STR_DISABLED_);
    _printf("Depth (Z) correction: %s\n", (surface_transform.depth_enabled && surface_transform.active) ? _STR_ENABLED_ : _STR_DISABLED_);
    pop_prefix();

}


// Print measured or simulated depths
void ComprehensiveDeltaStrategy::print_depths(float depths[DM_GRID_ELEMENTS][3]) {

    _cds_depths_t _depths[DM_GRID_ELEMENTS];
    
    for(int i=0; i<DM_GRID_ELEMENTS; i++) {
        _depths[i].abs = 0;
        _depths[i].rel = depths[i][Z];
    }
    
    print_depths(_depths);

}

void ComprehensiveDeltaStrategy::print_depths(_cds_depths_t depths[DM_GRID_ELEMENTS]) {

    float rel_depths[DM_GRID_ELEMENTS];
    float best = 999, worst = 0;
    float mu, sigma, min, max;

    // Print header
    __printf("[PD] ");

    int i;

    // Print all depths
    int col = 0;
    for(i=0; i<DM_GRID_ELEMENTS; i++) {

        // Statistics calc requires a one-dimensional array
        rel_depths[i] = depths[i].rel;

        // Do some statistics (sign doesn't matter, only magnitude)
        if(fabs(depths[i].rel) < fabs(best)) {
            best = fabs(depths[i].rel);
        }

        if(fabs(depths[i].rel) > fabs(worst)) {
            worst = fabs(depths[i].rel);
        }

        // Print entry (or a blank space, if the test point is turned off)
        switch(active_point[i]) {
            case TP_CENTER:
            case TP_ACTIVE:
                __printf(" %6.3f ", depths[i].rel);
                break;
            case TP_ACTIVE_NEIGHBOR:
                __printf("[%6.3f]", depths[i].rel);
                break;
            case TP_INACTIVE:
                 __printf("        ");
                 break;
        }

        // Space or new line?
        if(col++ < DM_GRID_DIMENSION - 1) {
            __printf("   ");
        } else if(i < DM_GRID_ELEMENTS - 1) {
            col = 0;
            __printf("\n[PD]\n[PD] ");
        }
        
        flush();

    }

    // Calculate and print statistics.
    // The difference between "best/worst" and "min/max" is that best and worst are indifferent to sign.
    calc_statistics(rel_depths, DM_GRID_ELEMENTS, mu, sigma, min, max);
    __printf("\n[PD] Best=%1.3f, worst=%1.3f, min=%1.3f, max=%1.3f, mu=%1.3f, sigma=%1.3f, energy=%1.3f\n", best, worst, min, max, mu, sigma, calc_energy(depths));
    flush();

}


// Distance between two points in 2-space
float ComprehensiveDeltaStrategy::distance2D(float first[2], float second[2]) {
    return sqrt(pow(second[X] - first[X], 2) + pow(second[Y] - first[Y], 2));
}


// Distance between two points in 3-space
float ComprehensiveDeltaStrategy::distance3D(float first[3], float second[3]) {
    return sqrt(
        pow(second[X] - first[X], 2) +
        pow(second[Y] - first[Y], 2) +
        pow(second[Z] - first[Z], 2)
        );
}


// Rotate a point around another point in 2-space.
// Adapted from http://stackoverflow.com/questions/2259476/rotating-a-point-about-another-point-2d
void ComprehensiveDeltaStrategy::rotate2D(float (&point)[2], float reference[2], float angle) {

    float s = sin(angle * 3.141595 / 180.0);
    float c = cos(angle * 3.141595 / 180.0);
    
    point[X] -= reference[X];
    point[Y] -= reference[Y];

    float xNew = point[X] * c - point[Y] * s;
    float yNew = point[X] * s + point[Y] * c;

    point[X] = xNew + reference[X];
    point[Y] = yNew + reference[Y];

}


// Zero out depth_map.
void ComprehensiveDeltaStrategy::zero_depth_maps() {

    for(int i=0; i < DM_GRID_ELEMENTS; i++) {
        depth_map[i].abs = 0;
        depth_map[i].rel = 0;
    }

}


// Copy a depth map to another depth map
void ComprehensiveDeltaStrategy::copy_depth_map(_cds_depths_t source[], _cds_depths_t dest[]) {

    for(int i=0; i < DM_GRID_ELEMENTS; i++) {
        dest[i].abs = source[i].abs;
        dest[i].rel = source[i].rel;
    }

}


// Turn off all calibration types
void ComprehensiveDeltaStrategy::clear_calibration_types() {

    caltype.endstop.active = false;
    caltype.delta_radius.active = false;
    caltype.arm_length.active = false;
    caltype.tower_angle.active = false;
    caltype.virtual_shimming.active = false;

}


// Display active/inactive calibration types.
// The args are either-or - they shouldn't both be true.
void ComprehensiveDeltaStrategy::display_calibration_types(bool active, bool inactive) {

    char ES[] = "Endstops (O)";
    char DR[] = "Delta Radius (P)";
    char AL[] = "Arm Length (Q)";
    char TAO[] = "Tower Angle Offset (R)";
    char VS[] = "Virtual Shimming (S)";
    char format[] = "[%s, mul=%1.2f] ";
    int nShown = 0;

    // Display active caltypes
    if(active) {

        if(caltype.endstop.active) {
            __printf(format, ES, caltype.endstop.annealing_temp_mul);
            nShown++;
        }

        if(caltype.delta_radius.active) {
            __printf(format, DR, caltype.delta_radius.annealing_temp_mul);
            nShown++;
        }

        if(caltype.arm_length.active) {
            __printf(format, AL, caltype.arm_length.annealing_temp_mul);
            nShown++;
        }

        if(caltype.tower_angle.active) {
            __printf(format, TAO, caltype.tower_angle.annealing_temp_mul);
            nShown++;
        }
        
        if(caltype.virtual_shimming.active) {
            __printf(format, VS, caltype.virtual_shimming.annealing_temp_mul);
            nShown++;
        }

    } // active    

    // Display inactive caltypes
    if(inactive) {

        if(!caltype.endstop.active) {
            __printf(format, ES, caltype.endstop.annealing_temp_mul);
            nShown++;
        }

        if(!caltype.delta_radius.active) {
            __printf(format, DR, caltype.delta_radius.annealing_temp_mul);
            nShown++;
        }

        if(!caltype.arm_length.active) {
            __printf(format, AL, caltype.arm_length.annealing_temp_mul);
            nShown++;
        }

        if(!caltype.tower_angle.active) {
            __printf(format, TAO, caltype.tower_angle.annealing_temp_mul);
            nShown++;
        }

        if(!caltype.virtual_shimming.active) {
            __printf(format, VS, caltype.virtual_shimming.annealing_temp_mul);
            nShown++;
        }

    } // inactive

    // Print a nice placeholder if no caltypes were active/inactive
    if(nShown == 0) {
        __printf("(none)");
    }

    __printf("\n");

}


// Calculate mean (mu), standard deviation (sigma), min, and max values for an array of arbitrary length
void ComprehensiveDeltaStrategy::calc_statistics(float values[], int n_values, float &mu, float &sigma, float &min, float &max) {

    // Init
    int stats;
    float dev;
    min =  999;
    max = -999;

    // Mu, min, and max
    mu = 0;
    for(stats = 0; stats < n_values; stats++) {
        mu += values[stats];
        if(values[stats] > max) { max = values[stats]; }
        if(values[stats] < min) { min = values[stats]; }
    }
    mu /= n_values;

    // Sigma
    dev = 0;
    for(stats=0; stats < n_values; stats++) {
        dev += powf((float)values[stats] - mu, 2);
    }
    sigma = sqrtf(dev/n_values);

}


// Calculate the "energy" of an array of depths
float ComprehensiveDeltaStrategy::calc_energy(_cds_depths_t points[DM_GRID_ELEMENTS]) {

    float cartesian[DM_GRID_ELEMENTS][3];
    for(int i=0; i<DM_GRID_ELEMENTS; i++) {
        cartesian[i][X] = test_point[i][X];
        cartesian[i][Y] = test_point[i][Y];
        cartesian[i][Z] = points[i].rel;
    }
    
    return calc_energy(cartesian);

}

float ComprehensiveDeltaStrategy::calc_energy(float cartesian[DM_GRID_ELEMENTS][3]) {

    float mu = 0;
    int i = 0;

    for(int stats = 0; stats < DM_GRID_ELEMENTS; stats++) {
        if(active_point[stats] == TP_ACTIVE) {
            mu += fabs(cartesian[stats][Z]);
            i++;
        }
    }
        
    return mu / i;

}


// Calculate the midpoint of a 2-D line.
// first[] and second[] are floats. Resulting midpoint stored in dest[].
void ComprehensiveDeltaStrategy::midpoint(float first[2], float second[2], float (&dest)[2]) {

    dest[0] = (first[0] + second[0]) / 2;
    dest[1] = (first[1] + second[1]) / 2;

}


// Make sure n is between lower and upper
float ComprehensiveDeltaStrategy::clamp(float n, float lower, float upper) {
    return std::max(lower, std::min(n, upper));
}


// Print some spaces
void ComprehensiveDeltaStrategy::str_pad_left(unsigned char spaces) {
    for(unsigned char i = 0; i < spaces; i++) {
        __printf(" ");
    }
}


// Print a banner indicating what we're working on, and what a terrible idea it would be to touch the printer
// in any way (except for the reset button)
void ComprehensiveDeltaStrategy::print_task_with_warning(const std::string& str) {
    char str_[str.length() + 1];	// Plus 1 to accommodate a \0... str.length() doesn't account for that!
    std::strcpy(str_, str.c_str());
    newline();
    _printf("%s in progress. Press Reset to abort.\n", str_);
    _printf("/!\\ PROBE CRASH DANGER /!\\ Don't press buttons, send commands, or access the SD card.\n \n");
}


// Allow the kernel to flush the serial buffer, and perform whatever other maintenance tasks are needed
// Note: It would be a good idea to avoid doing anything to the kernel that would hang it ON_IDLE.
void ComprehensiveDeltaStrategy::flush() {
    THEKERNEL->call_event(ON_IDLE);
}

void ComprehensiveDeltaStrategy::newline() {
    THEKERNEL->streams->printf(" \n");
}


// Method Prefixes
// Rather than _printf("[xx] thing"), where "[xx] " is repeated in dozens to hundreds of _printf() statements,
// we automate the prefix, the idea being to save RAM.
void ComprehensiveDeltaStrategy::print_method_prefix() {
    if(method_prefix[method_prefix_idx][0] != 0) {
        THEKERNEL->streams->printf("[%s] ", method_prefix[method_prefix_idx]);
    }
}

void ComprehensiveDeltaStrategy::push_prefix(const std::string& mp) {
    if(method_prefix_idx + 1 < MP_MAX_PREFIXES) {
        strncpy(method_prefix[++method_prefix_idx], mp.c_str(), 3);
    } else {
        THEKERNEL->streams->printf("Prefix: Max prefixes exceeded (%d)\n", method_prefix_idx);
    }
}

void ComprehensiveDeltaStrategy::pop_prefix() {
    if(method_prefix_idx > 0) {
        method_prefix_idx--;
    } else {
        THEKERNEL->streams->printf("Prefix: Tried to pop one too many times\n");
    }
}











// Dead code waiting to be flushed down the turlet below this line - cut here:
// ---8<-----------8<-----------8<-----------8<-----------8<------------8<----


/*
                // Find the best arm length offsets
//                for(k=0; k<3; k++) {
k = annealing_try % 3;
                    best_value = find_optimal_config(&ComprehensiveDeltaStrategy::set_tower_arm_offsets, cur_set.tower_arm, k, test_arm_length_offset[k].range_min, test_arm_length_offset[k].range_max, binsearch_width, cur_cartesian, target);
                    move_randomly_towards(cur_set.tower_arm[k], best_value, temp * caltype.arm_length.annealing_temp_mul, target, overrun_divisor);
//                }

                // Find the arm length offset with the lowest absolute value
                lowest = 999;
                for(k=0; k<3; k++) {
                    if(fabs(cur_set.tower_arm[k]) < lowest) {
                        lowest = cur_set.tower_arm[k];
                    }
                }

                // Steal that value from the individual arm length settings and give it to the global arm length setting
                for(k=0; k<3; k++) {
                    cur_set.tower_arm[k] -= lowest;
                }
                cur_set.arm_length += lowest;

                // Tell the robot what the new arm length & offsets are            
                set_arm_length(cur_set.arm_length, false);
                set_tower_arm_offsets(cur_set.tower_arm[X], cur_set.tower_arm[Y], cur_set.tower_arm[Z], false);
/**/


/*
    // Towers are 60 degrees off centerline.
    // So, the quadrants look like this:
    // Q2: -xDeg, +yDeg   Q1: +xDeg, +yDeg
    // Q3: -xDeg, -yDeg   Q4: +xDeg, -yDeg
    float xDeg = 0.866025f;
    float yDeg = 0.5;

    // Points at towers (this is simple quadrant stuff)
    test_point[TP_X][X] = -xDeg * probe_radius;
    test_point[TP_X][Y] = -yDeg * probe_radius;
    test_point[TP_Y][X] =  xDeg * probe_radius;
    test_point[TP_Y][Y] = -yDeg * probe_radius;
    test_point[TP_Z][X] =                    0;
    test_point[TP_Z][Y] =         probe_radius;

    // Points opposite towers
    // Merely a sign-flipped version of above, so the points are mirrored about the origin
    test_point[TP_OPP_X][X] =  xDeg * probe_radius;
    test_point[TP_OPP_X][Y] =  yDeg * probe_radius;
    test_point[TP_OPP_Y][X] = -xDeg * probe_radius;
    test_point[TP_OPP_Y][Y] =  yDeg * probe_radius;
    test_point[TP_OPP_Z][X] =                    0;
    test_point[TP_OPP_Z][Y] =        -probe_radius;

    // Midpoints between towers
    midpoint(test_point[TP_X], test_point[TP_Y], test_point[TP_MID_XY]);
    midpoint(test_point[TP_Y], test_point[TP_Z], test_point[TP_MID_YZ]);
    midpoint(test_point[TP_Z], test_point[TP_X], test_point[TP_MID_ZX]);

    // Opposite midpoints between towers
    // These happen to be halfway between {0, 0} and the points opposite the X/Y/Z towers
    test_point[TP_OPP_MID_XY][X] = test_point[TP_MID_XY][X];
    test_point[TP_OPP_MID_XY][Y] = -test_point[TP_MID_XY][Y];
    test_point[TP_OPP_MID_ZX][X] = test_point[TP_OPP_X][X] / 2;
    test_point[TP_OPP_MID_ZX][Y] = -test_point[TP_OPP_X][Y] / 2;
    test_point[TP_OPP_MID_YZ][X] = test_point[TP_OPP_Y][X] / 2;
    test_point[TP_OPP_MID_YZ][Y] = -test_point[TP_OPP_Y][Y] / 2;
*/




// Copy depth_map to last_depth_map & zero all of depth_map
/*
void ComprehensiveDeltaStrategy::save_depth_map() {

    for(int i = 0; i < CDS_DEPTH_MAP_N_POINTS; i++) {
        last_depth_map[i].rel = depth_map[i].rel;
        last_depth_map[i].abs = depth_map[i].abs;
    }

}
*/





/* Probe the depth of points near each tower, and at the halfway points between each tower:

        1
        /\
     2 /__\ 6
      /\  /\
     /__\/__\
    3   4    5

   This pattern defines the points of a triforce, hence the name.
*/
/*
bool ComprehensiveDeltaStrategy::probe_triforce(float (&depth)[6], float &score_avg, float &score_ISM, float &PHTT) {

    // Init test points
    int triforce[6] = { TP_Z, TP_MID_ZX, TP_X, TP_MID_XY, TP_Y, TP_MID_YZ };

    int s;				// # of steps (passed by reference to probe_delta_tower, which sets it)
    int i;
    score_avg = 0;			// Score starts at 0 (perfect) - the further away it gets, the worse off we are!
    score_ISM = 0;

    // Need to get bed height in current tower angle configuration (the following method automatically refreshes mm_PHTT)
    // We're passing the current value of PHTT back by reference in case the caller cares, e.g. if they want a baseline.
    require_clean_geometry();
    prepare_to_probe();
    if(!prime_probe()) return false;
    PHTT = mm_probe_height_to_trigger;

    // This is for storing the probe results in terms of score (deviation from center height).
    // This is different from the "scores" we return, which is the average and intersextile mean of the contents of scores[].
    float score[6];

    for(i=0; i<6; i++) {
        // Probe triforce
        _printf("[PT] Probing point %d at <%1.3f, %1.3f>.\n", i, test_point[triforce[i]][X], test_point[triforce[i]][Y]);

        // Move into position and probe the depth
        // depth[i] is probed and calculated in exactly the same way that mm_probe_height_to_trigger is
        // This means that we can compare probe results from this and mm_PHTT on equal terms
        if(!do_probe_at(s, test_point[triforce[i]][X], test_point[triforce[i]][Y])) {
            return false;
        }
        depth[i] = zprobe->zsteps_to_mm(s);
        score[i] = fabs(depth[i] - mm_probe_height_to_trigger);
    }
    
    // Do some statistics
    auto mm = std::minmax({score});
    for(i=0; i<6; i++) {
    
        // Average
        score_avg += score[i];

        // Intersextile mean (ignore lowest and highest values, keep the remaining four)
        // Works similar to an interquartile mean, but more specific to our problem domain (we always have exactly 6 samples)
        // Context: http://en.wikipedia.org/wiki/Interquartile_mean
        if(score[i] != *mm.first && score[i] != *mm.second) {
            score_ISM += score[i];
        }
    }
    score_avg /= 6;
    score_ISM /= 4;

    _printf("[TQ] Probe height to trigger at bed center (PHTT) - this is the target depth: %1.3f\n", mm_probe_height_to_trigger);
    _printf("[TQ]        Current depths: {%1.3f, %1.3f, %1.3f, %1.3f, %1.3f, %1.3f}\n", depth[0], depth[1], depth[2], depth[3], depth[4], depth[5]);
    _printf("[TQ]   Delta(depth - PHTT): {%1.3f, %1.3f, %1.3f, %1.3f, %1.3f, %1.3f}\n", fabs(depth[0] - mm_probe_height_to_trigger), fabs(depth[1] - mm_probe_height_to_trigger), fabs(depth[2] - mm_probe_height_to_trigger), fabs(depth[3] - mm_probe_height_to_trigger), fabs(depth[4] - mm_probe_height_to_trigger), fabs(depth[5] - mm_probe_height_to_trigger));
    _printf("[TQ]  Score (lower=better): avg=%1.3f, ISM=%1.3f\n", score_avg, score_ISM);

    return true;

}
*/

/*
// Depth-map an imaginary line, and points perpendicular, from a tower to its opposite point
// (across the print surface), in a given number of segments
bool ComprehensiveDeltaStrategy::depth_map_segmented_line(float first[2], float second[2], unsigned char segments) {

    // Calculate vector and length
    Vector3 vec(second[X] - first[X], second[Y] - first[Y], 0);
    Vector3 vec_norm = vec.unit();
    float dist = distance2D(first, second);
    float seg_dist = dist / (float)segments;
//    _printf("Endpoints: <%1.3f, %1.3f> to <%1.3f, %1.3f>\n", first[X], first[Y], second[X], second[Y]);
//    _printf("   Vector: <%1.3f, %1.3f>; Norm: <%1.3f, %1.3f>\n", vec[0], vec[1], vec_norm[0], vec_norm[1]);
//    _printf("     Dist: %1.3f, segment dist: %1.3f\n", dist, seg_dist);


    // Measure depth from probe_height at bed center
    int steps;
    int origin_steps = 0;

    require_clean_geometry();
    prepare_to_probe();
    
    if(!prime_probe()) return false;

    if(do_probe_at(origin_steps, 0, 0)) {
        _printf("[SL] Steps from probe_from_height to bed surface at center: %d\n", origin_steps);
    } else {
        _printf("[SL] do_probe_at() returned false.\n");
        return false;
    }

    float arm_length;
    float arm_radius;
    float armX, armY, armZ;

    get_arm_length(arm_length);
    get_delta_radius(arm_radius);
    get_tower_arm_offsets(armX, armY, armZ);
//    _printf("Segments: %d\n", segments);
//    _printf("Basic - Arm length: %1.3f  Radius: %1.3f\n", arm_length, arm_radius);
//    _printf("Arm offsets: <%1.3f, %1.3f, %1.3f>\n", armX, armY, armZ);
//    _printf("Origin Z steps: %d\n", origin_steps);

    int base_depths[segments + 1][3];

    for(int i=0; i <= segments; i++) {
        //void ComprehensiveDeltaStrategy::rotate2D(float (&point)[2], float reference[2], float angle)
        float tp[2] = { first[X] + (vec_norm[X] * seg_dist * i), first[Y] + (vec_norm[Y] * seg_dist * i) };
        float tp_pos_rot[2] = { first[X] + (vec_norm[X] * seg_dist * (i + 1)), first[Y] + (vec_norm[Y] * seg_dist * (i + 1)) };
        float tp_neg_rot[2] = { first[X] + (vec_norm[X] * seg_dist * (i + 1)), first[Y] + (vec_norm[Y] * seg_dist * (i + 1)) };
        rotate2D(tp_pos_rot, tp, 90);
        rotate2D(tp_neg_rot, tp, -90);


        _printf(
            "Segment %d endpoint at <%1.3f, %1.3f> has projection <%1.3f, %1.3f> and perpendiculars <%1.3f, %1.3f> and <%1.3f, %1.3f>\n",
            i, tp[X], tp[Y],
            first[X] + (vec_norm[X] * seg_dist * (i + 1)), first[Y] + (vec_norm[Y] * seg_dist * (i + 1)),
            tp_pos_rot[X], tp_pos_rot[Y], tp_neg_rot[X], tp_neg_rot[Y]);

            
        do_probe_at(steps, tp_pos_rot[X], tp_pos_rot[Y]);
        base_depths[i][0] = steps;
        do_probe_at(steps, tp[X], tp[Y]);
        base_depths[i][1] = steps;
        do_probe_at(steps, tp_neg_rot[X], tp_neg_rot[Y]);
        base_depths[i][2] = steps;
        
        _printf("Segment %d endpoint at <%1.3f, %1.3f> - depths: pos=%1.3f, center=%1.3f, neg=%1.3f\n", i, tp[X], tp[Y], zprobe->zsteps_to_mm(origin_steps - base_depths[i][0]), zprobe->zsteps_to_mm(origin_steps - base_depths[i][1]), zprobe->zsteps_to_mm(origin_steps - base_depths[i][2]));
    }

    return true;   

}
*/

/*
                    // Find the direction of the most optimal configuration with a binary search
                    for(j=0; j<sampling_tries; j++) {
                
                        // Test energy at min & max
                        cur_set.trim[k] = t_endstop[k].min;
                        energy_min = simulate_FK_and_get_energy(test_axis, cur_set.trim, cur_cartesian);
                        
                        cur_set.trim[k] = t_endstop[k].max;
                        energy_max = simulate_FK_and_get_energy(test_axis, cur_set.trim, cur_cartesian);

                        // Who won?
                        if(t_endstop[k].max - t_endstop[k].min <= target) {
                            break;
                        }
                        if(energy_min < energy_max) {
                            t_endstop[k].max -= ((t_endstop[k].max - t_endstop[k].min) * binsearch_width);
                        } else {
                            t_endstop[k].min += ((t_endstop[k].max - t_endstop[k].min) * binsearch_width);
                        }

                    }

                    t_endstop[k].best = (t_endstop[k].min + t_endstop[k].max) / 2.0f;
                    step = ( ((float)rand() / RAND_MAX) * temp ) + 0.001;

    //_printf("[HC] Tower %d, try %3d: best=%1.3f step=%1.3f ", k, annealing_try, t_endstop[k].best, step);
                    if(t_endstop[k].best > t_endstop[k].val + target) {
                        if(t_endstop[k].val + step > t_endstop[k].best) {
                            step /= 2;
                        }
                        t_endstop[k].val += step;
                    }
                    if(t_endstop[k].best < t_endstop[k].val - target) {
                        if(t_endstop[k].val - step < t_endstop[k].best) {
                            step /= 2;
                        }
                        t_endstop[k].val -= step;
                    }
    //_printf("val=%1.3f\n", t_endstop[k].val);
*/
