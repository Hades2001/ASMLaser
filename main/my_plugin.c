#include <math.h>
#include <stdio.h>

#include "driver.h"
#include "grbl/hal.h"
#include "grbl/protocol.h"
#include "grbl/state_machine.h"
#include "grbl/ioports.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "my_plugin.h"
#include "t1_offline.h"
#include "factory_init.h"

#define T0_AXES (X_AXIS_BIT | Y_AXIS_BIT)
#if T0_MICROSTEPS != 256
#error "MODE1/MODE2 are wired high: this board initializes STSPIN220 to 256 microsteps"
#endif

static driver_setup_ptr previous_setup;
static esp_timer_handle_t sound_timer;
static bool beep_pending, melody_pending;
static bool factory_sound_active;
static uint32_t melody_count, beep_count, trigger_count, limit_count[2];

void my_plugin_beep (void)
{
    __atomic_store_n(&beep_pending, true, __ATOMIC_RELAXED);
}

static void stspin220_init (void)
{
    // Datasheet Rev 5 table 8: MODE1=MODE2=1, STCK=DIR=0 => 1/256.
    // Hold EN low throughout the latch sequence. EN is also an open-drain
    // FAULT output from the chip, so the MCU must not drive it push-pull high.
    gpio_set_level(STEPPERS_ENABLE_PIN, 0);
    gpio_set_direction(STEPPERS_ENABLE_PIN, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(STEPPERS_ENABLE_PIN, GPIO_PULLUP_ONLY);
    gpio_set_level(STEPPERS_STBY_PIN, 0);
    gpio_set_direction(STEPPERS_STBY_PIN, GPIO_MODE_INPUT_OUTPUT);

    const gpio_num_t pins[] = {X_STEP_PIN, Y_STEP_PIN, X_DIRECTION_PIN, Y_DIRECTION_PIN};
    for(unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_set_level(pins[i], 0);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(pins[i], GPIO_FLOATING);
    }
    esp_rom_delay_us(1000); // MODE setup >= 1us; enter standby first.
    gpio_set_level(STEPPERS_STBY_PIN, 1);
    esp_rom_delay_us(1000); // MODE hold >= 100us after the STBY rising edge.
    // Keep STBY high: pulling it low again resets the translator and disables
    // the power stages. driver_setup() takes over STEP/DIR and their RMT mapping.
}

static void tone (uint32_t hz)
{
    if(hz) {
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3, hz);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 512);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7);
    } else
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 0);
}

// A dedicated esp_timer task services sound and switch edges even inside the
// core's homing loop. No LEDC API is called from GPIO/stepper interrupts.
static void sound_tick (void *arg)
{
    if(__atomic_load_n(&factory_sound_active, __ATOMIC_ACQUIRE))
        return;
    static uint8_t low_samples[3];
    static bool pressed[3];
    static uint32_t started;
    static unsigned phase = 4;
    static bool melody, short_tone;
    const gpio_num_t inputs[] = {X_LIMIT_PIN, Y_LIMIT_PIN, OFFLINE_TRIGGER_PIN};
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    for(unsigned i = 0; i < 3; i++) {
        uint8_t threshold = i == 2 ? 3 : 2;
        if(gpio_get_level(inputs[i])) {
            low_samples[i] = 0;
            pressed[i] = false;
        } else if(low_samples[i] < threshold && ++low_samples[i] == threshold && !pressed[i]) {
            pressed[i] = true;
            if(i < 2)
                __atomic_fetch_add(&limit_count[i], 1, __ATOMIC_RELAXED);
            else
                __atomic_fetch_add(&trigger_count, 1, __ATOMIC_RELAXED);
            my_plugin_beep();
        }
    }
    if(__atomic_exchange_n(&melody_pending, false, __ATOMIC_RELAXED)) {
        melody = true;
        short_tone = false;
        phase = 4;
        started = now;
        __atomic_fetch_add(&melody_count, 1, __ATOMIC_RELAXED);
    }
    if(melody) {
        const uint16_t notes[] = {1047, 1319, 1568, 2093};
        unsigned next = (now - started) / 250;
        if(next >= 4) {
            tone(0);
            melody = false;
        } else if(next != phase) {
            tone(notes[next]);
            phase = next;
        }
    } else if(__atomic_exchange_n(&beep_pending, false, __ATOMIC_RELAXED)) {
        tone(2400);
        short_tone = true;
        started = now;
        __atomic_fetch_add(&beep_count, 1, __ATOMIC_RELAXED);
    } else if(short_tone && (uint32_t)(now - started) >= 100) {
        tone(0);
        short_tone = false;
    }
}

static bool machine_setup (settings_t *settings)
{
    bool ok = previous_setup(settings);
    // The generic driver initializes all outputs push-pull. Restore EN/FAULT
    // open-drain without changing the enable level selected by the driver.
    gpio_set_direction(STEPPERS_ENABLE_PIN, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(STEPPERS_ENABLE_PIN, GPIO_PULLUP_ONLY);
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_3, .freq_hz = 2400, .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_channel_config_t channel = {
        .gpio_num = BUZZER_PIN, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_7, .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_3, .duty = 0, .hpoint = 0
    };
    esp_timer_create_args_t args = { .callback = sound_tick, .name = "machine_sound" };
    return ok && ledc_timer_config(&timer) == ESP_OK &&
           ledc_channel_config(&channel) == ESP_OK &&
           esp_timer_create(&args, &sound_timer) == ESP_OK &&
           esp_timer_start_periodic(sound_timer, 10000) == ESP_OK;
}

static status_code_t machine_status (sys_state_t state, char *args)
{
    char text[220];
    snprintf(text, sizeof(text),
             "[T1:ConfiguredMicrosteps=256|STBY=%d|Trigger=%d|XLimit=%d|YLimit=%d|Melodies=%lu|Beeps=%lu|LimitX=%lu|LimitY=%lu|Triggers=%lu]\r\n",
             gpio_get_level(STEPPERS_STBY_PIN), gpio_get_level(OFFLINE_TRIGGER_PIN),
             gpio_get_level(X_LIMIT_PIN), gpio_get_level(Y_LIMIT_PIN),
             (unsigned long)__atomic_load_n(&melody_count, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&beep_count, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&limit_count[0], __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&limit_count[1], __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&trigger_count, __ATOMIC_RELAXED));
    hal.stream.write(text);
    return Status_OK;
}

static bool fan_ready;
static on_spindle_select_ptr previous_spindle_select;
static spindle_set_state_ptr laser_set_state;

#ifdef AUXOUTPUT3_PIN
static bool claim_fan (xbar_t *pin, uint8_t port, void *data)
{
    if(pin->pin != AUXOUTPUT3_PIN)
        return false;

    // Enumeration supplies the physical AUX number expected by claim().
    // claim() may replace port with 255 on failure; never use it as a loop index.
    fan_ready = ioport_claim(Port_Digital, Port_Output, &port, "Laser fan") != NULL;
    if(fan_ready) {
        gpio_set_level(AUXOUTPUT3_PIN, 0);
        gpio_set_direction(AUXOUTPUT3_PIN, GPIO_MODE_OUTPUT);
    }
    return true;
}
#endif

static void fan_set (bool on)
{
#ifdef AUXOUTPUT3_PIN
    if(fan_ready)
        gpio_set_level(AUXOUTPUT3_PIN, on ? 1 : 0);
#endif
}

static void laser_with_fan (spindle_ptrs_t *spindle, spindle_state_t state, float rpm)
{
    // Start the fan before enabling laser output, including M3/M4 S0.
    if(state.on && fan_ready)
        fan_set(true);
    else if(state.on) {
        // The laser cannot run without its configured cooling output.
        state.value = 0;
        rpm = 0.0f;
    }

    laser_set_state(spindle, state, rpm);

    // Stop the laser first. This path also handles reset/alarm spindle shutdown.
    if(!state.on)
        fan_set(false);
}

static bool select_spindle (spindle_ptrs_t *spindle)
{
    if(previous_spindle_select && !previous_spindle_select(spindle))
        return false;

    if(spindle->type == SpindleType_PWM && spindle->set_state != laser_with_fan) {
        laser_set_state = spindle->set_state;
        spindle->set_state = laser_with_fan;
    }

    return true;
}

static bool calibration_settings_valid (void)
{
    if(T0_MICROSTEPS <= 0 || !fan_ready)
        return false;

    if(!settings.homing.flags.enabled || !settings.homing.flags.init_lock ||
       settings.homing.flags.force_set_origin || settings.homing.flags.manual ||
       settings.homing.dir_mask.mask != 0 || sys.homing.mask != T0_AXES ||
       !settings.limits.flags.hard_enabled ||
       (settings.limits.invert.mask & T0_AXES) != T0_AXES ||
       (settings.limits.disable_pullup.mask & T0_AXES) ||
       (settings.limits.soft_enabled.mask & T0_AXES) ||
       settings.mode != Mode_Laser)
        return false;

    for(uint_fast8_t axis = X_AXIS; axis <= Y_AXIS; axis++) {
        float steps = 20.0f * T0_MICROSTEPS / 3.0f;
        if(!isfinite(settings.axis[axis].steps_per_mm) ||
           fabsf(settings.axis[axis].steps_per_mm - steps) > 0.001f ||
           fabsf(settings.axis[axis].max_travel + 42.0f) > 0.001f ||
           fabsf(settings.axis[axis].homing_seek_rate - 1500.0f) > 0.001f ||
           fabsf(settings.axis[axis].homing_feed_rate - 1500.0f) > 0.001f)
            return false;
    }

    // $H runs stored startup lines; require an empty list before auto-calibration.
    char line[sizeof(stored_line_t)];
    for(uint_fast8_t idx = 0; idx < N_STARTUP_LINE; idx++)
        if(!settings_read_startup_line(idx, line) || line[0])
            return false;

    return true;
}

static bool factory_sound (void)
{
    // Stop the periodic callback before taking ownership of its LEDC channel.
    __atomic_store_n(&factory_sound_active, true, __ATOMIC_RELEASE);
    hal.delay_ms(20, NULL);
    tone(2400);
    bool completed = delay_sec(3.0f, DelayMode_Dwell);
    tone(0);
    __atomic_store_n(&factory_sound_active, false, __ATOMIC_RELEASE);
    return completed && !sys.abort;
}

static void calibrate_on_boot (void *data)
{
    if(!factory_initialize(factory_sound)) {
        system_raise_alarm(Alarm_HomingRequired);
        hal.stream.write("[MSG:Factory initialization failed; restart to retry]\r\n");
        return;
    }
    // Keep startup diagnostics visible on the UART console without a USB host.
    printf("T0 boot: STBY=%d EN=%d enable_invert=%u state=%u\n",
           gpio_get_level(STEPPERS_STBY_PIN), gpio_get_level(STEPPERS_ENABLE_PIN),
           settings.steppers.enable_invert.mask, (unsigned)state_get());
    __atomic_store_n(&melody_pending, true, __ATOMIC_RELAXED);
    // Startup melody runs concurrently with the one-second fan check.
    // Exercise only the fan at startup; laser output remains off.
    if(fan_ready) {
        fan_set(true);
        hal.stream.write("[MSG:T0 fan check on]\r\n");
        bool completed = delay_sec(1.0f, DelayMode_Dwell);
        printf("T0 fan finished: completed=%u abort=%u\n", completed, sys.abort);
        fan_set(false);
        hal.stream.write("[MSG:T0 fan check off]\r\n");
        if(!completed || sys.abort)
            return;
    }

    if(!calibration_settings_valid()) {
        printf("T0 settings rejected: fan=%u homing=%u init_lock=%u force_origin=%u manual=%u dir=%u axes=%u hard=%u invert=%u no_pullup=%u soft=%u mode=%u\n",
               fan_ready, settings.homing.flags.enabled, settings.homing.flags.init_lock,
               settings.homing.flags.force_set_origin, settings.homing.flags.manual,
               settings.homing.dir_mask.mask, sys.homing.mask,
               settings.limits.flags.hard_enabled, settings.limits.invert.mask,
               settings.limits.disable_pullup.mask, settings.limits.soft_enabled.mask, settings.mode);
        for(unsigned axis = X_AXIS; axis <= Y_AXIS; axis++)
            printf("T0 axis %u: steps=%.6f travel=%.3f seek=%.3f feed=%.3f\n", axis,
                   settings.axis[axis].steps_per_mm, settings.axis[axis].max_travel,
                   settings.axis[axis].homing_seek_rate, settings.axis[axis].homing_feed_rate);
        if(state_get() == STATE_IDLE)
            system_raise_alarm(Alarm_HomingRequired);
        hal.stream.write("[MSG:T0 blocked: check fan GPIO, microsteps, XY settings and empty $N lines]\r\n");
        return;
    }

    // Invoke the normal homing command, preserving its control-input checks,
    // switch debounce, pull-off, failure alarms and realtime abort handling.
    char home[] = "$H";
    hal.stream.write("[MSG:T0 homing XY]\r\n");
    printf("T0 homing start: STBY=%d EN=%d XLimit=%d YLimit=%d\n",
           gpio_get_level(STEPPERS_STBY_PIN), gpio_get_level(STEPPERS_ENABLE_PIN),
           gpio_get_level(X_LIMIT_PIN), gpio_get_level(Y_LIMIT_PIN));
    status_code_t home_status = system_execute_line(home);
    printf("T0 homing end: status=%u abort=%u state=%u homed=%u STBY=%d EN=%d\n",
           home_status, sys.abort, (unsigned)state_get(), sys.homed.mask,
           gpio_get_level(STEPPERS_STBY_PIN), gpio_get_level(STEPPERS_ENABLE_PIN));
    if(home_status != Status_OK || sys.abort ||
       state_get() != STATE_IDLE || (sys.homed.mask & T0_AXES) != T0_AXES) {
        hal.stream.write("[MSG:T0 homing failed; origin unchanged]\r\n");
        return;
    }

    // With positive-end switches and ordinary grbl machine coordinates the
    // switches are at MPos 0, and the opposite workspace corner is MPos -42.
    // The homing pull-off is already accounted for by the core (usually -1).
    char modal[] = "G21G90G94M5";
    char move[] = "G53G1X-42Y-42F1500";
    char origin[] = "G92X0Y0";

    if(gc_execute_block(modal) != Status_OK ||
       gc_execute_block(move) != Status_OK ||
       !protocol_buffer_synchronize() || sys.abort || state_get() != STATE_IDLE ||
       sys.position[X_AXIS] != lroundf(-42.0f * settings.axis[X_AXIS].steps_per_mm) ||
       sys.position[Y_AXIS] != lroundf(-42.0f * settings.axis[Y_AXIS].steps_per_mm)) {
        hal.stream.write("[MSG:T0 return failed; origin unchanged]\r\n");
        return;
    }

    if(gc_execute_block(origin) == Status_OK) {
        t1_offline_calibrated();
        hal.stream.write("[MSG:T0 ready: XY work origin set]\r\n");
        printf("T0 ready: XY work origin set\n");
    }
    else
        hal.stream.write("[MSG:T0 failed to set G92 origin]\r\n");
}

void my_plugin_init (void)
{
    stspin220_init();
    previous_setup = hal.driver_setup;
    hal.driver_setup = machine_setup;
    gpio_set_level(BUZZER_PIN, 0);
    gpio_set_direction(BUZZER_PIN, GPIO_MODE_OUTPUT);

#ifdef AUXOUTPUT3_PIN
    ioports_enumerate(Port_Digital, Port_Output,
                     (pin_cap_t){ .claimable = On }, claim_fan, NULL);
#endif

    previous_spindle_select = grbl.on_spindle_select;
    grbl.on_spindle_select = select_spindle;
    static const sys_command_t commands[] = {
        {"T1", machine_status, { .noargs = On, .allow_blocking = On }, { .str = "machine input and sound diagnostics" }}
    };
    static sys_commands_t command_group = { .n_commands = 1, .commands = commands };
    system_register_commands(&command_group);
    static const periph_pin_t stby = {
        .function = Output_Aux5, .group = PinGroup_AuxOutput,
        .pin = STEPPERS_STBY_PIN, .mode = { .mask = PINMODE_OUTPUT }, .description = "STSPIN220 STBY"
    };
    static const periph_pin_t buzzer = {
        .function = Output_Aux6, .group = PinGroup_AuxOutput,
        .pin = BUZZER_PIN, .mode = { .mask = PINMODE_OUTPUT }, .description = "Passive buzzer"
    };
    hal.periph_port.register_pin(&stby);
    hal.periph_port.register_pin(&buzzer);
    t1_offline_init();
    task_run_on_startup(calibrate_on_boot, NULL);
}
