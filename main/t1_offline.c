#include <string.h>
#include "driver.h"
#include "grbl/hal.h"
#include "grbl/protocol.h"
#include "grbl/state_machine.h"
#include "grbl/motion_control.h"
#include "grbl/stream_file.h"
#include "driver/gpio.h"
#include "t1_offline.h"

extern const uint8_t job_start[] asm("_binary_t1_job_start");
extern const uint8_t job_end[] asm("_binary_t1_job_end");

static vfs_file_t job_handle, *job;
static size_t job_pos;
static bool opened, calibrated, queued, raw_high, stable_high, armed;
static uint32_t changed_at;
static on_execute_realtime_ptr previous_realtime;
static driver_reset_ptr previous_reset;
static enqueue_realtime_command_ptr previous_rx;

// Read directly from the embedded Flash data; no SD card or RAM copy required.
static vfs_file_t *job_open (const char *name, const char *mode)
{
    if(opened || strcmp(name, "/job.gc") || strcmp(mode, "r"))
        return NULL;
    opened = true;
    job_pos = 0;
    job_handle.size = job_end - job_start;
    return &job_handle;
}
static void job_close (vfs_file_t *file) { opened = false; }
static size_t job_read (void *buffer, size_t size, size_t count, vfs_file_t *file)
{
    if(!size) return 0;
    size_t available = (file->size - job_pos) / size;
    if(count > available) count = available;
    memcpy(buffer, job_start + job_pos, count * size);
    job_pos += count * size;
    return count;
}
static size_t job_tell (vfs_file_t *file) { return job_pos; }
static int job_seek (vfs_file_t *file, size_t pos)
{
    if(pos > file->size) return -1;
    job_pos = pos;
    return 0;
}
static bool job_eof (vfs_file_t *file) { return job_pos == file->size; }

// During a local job accept realtime controls, discard host G-Code instead of
// accidentally executing buffered host commands after the local job finishes.
static bool offline_rx (uint8_t c)
{
    previous_rx(c);
    return true;
}
static void finish_job (void)
{
    if(job) {
        vfs_file_t *file = job;
        job = NULL;
        stream_redirect_close(file);
        hal.stream.reset_read_buffer();
        hal.stream.set_enqueue_rt_handler(previous_rx);
    }
}
static status_code_t job_error (status_code_t status)
{
    finish_job();
    grbl.report.status_message(status);
    hal.stream.write("[MSG:T1 stopped: G-Code error]\r\n");
    mc_reset();
    return status;
}
static status_code_t job_finished (vfs_file_t *file, status_code_t status)
{
    if(file == job) {
        if(status != Status_OK)
            return job_error(status);
        bool ok = protocol_buffer_synchronize() && !sys.abort;
        if(ok) {
            char stop[] = "M5M9";
            ok = gc_execute_block(stop) == Status_OK;
        }
        finish_job();
        hal.stream.write(ok ? "[MSG:T1 complete]\r\n" : "[MSG:T1 aborted]\r\n");
    }
    return status;
}
static void start_job (void *data)
{
    queued = false;
    if(!calibrated || sys.abort || state_get() != STATE_IDLE || job ||
       stream_is_file() || (sys.homed.mask & 3) != 3 ||
       hal.stream.get_rx_buffer_count() != 0) {
        hal.stream.write("[MSG:T1 ignored: not idle and calibrated]\r\n");
        return;
    }
    char preamble[] = "G21G90G94M5S0";
    if(gc_execute_block(preamble) != Status_OK)
        return;
    char path[] = "/offline/job.gc";
    previous_rx = hal.stream.set_enqueue_rt_handler(offline_rx);
    if(!(job = stream_redirect_read(path, job_error, job_finished))) {
        hal.stream.set_enqueue_rt_handler(previous_rx);
        hal.stream.write("[MSG:T1 cannot open embedded G-Code]\r\n");
    } else
        hal.stream.write("[MSG:T1 started: embedded G-Code]\r\n");
}
static void poll_trigger (sys_state_t state)
{
    if(previous_realtime) previous_realtime(state);
    uint32_t now = hal.get_elapsed_ticks();
    bool high = gpio_get_level(OFFLINE_TRIGGER_PIN) != 0;
    if(high != raw_high) {
        raw_high = high;
        changed_at = now;
    }
    if((uint32_t)(now - changed_at) < 30)
        return;
    if(high) {
        stable_high = true;
        armed = calibrated && !job && !queued && state_get() == STATE_IDLE;
    } else if(stable_high) {
        stable_high = false;
        if(armed && !queued) {
            queued = task_add_immediate(start_job, NULL);
        }
        armed = false;
    }
}
static void reset_offline (void)
{
    finish_job();
    task_delete(start_job, NULL);
    queued = calibrated = armed = false;
    stable_high = raw_high = gpio_get_level(OFFLINE_TRIGGER_PIN) != 0;
    changed_at = hal.get_elapsed_ticks();
    previous_reset();
}
void t1_offline_calibrated (void) { calibrated = true; }
void t1_offline_init (void)
{
    static const vfs_t fs = {
        .fs_name = "Embedded G-Code", .fopen = job_open, .fclose = job_close,
        .fread = job_read, .ftell = job_tell, .fseek = job_seek, .feof = job_eof
    };
    vfs_mount(NULL, "/offline", &fs,
              (vfs_st_mode_t){ .directory = true, .read_only = true, .hidden = true });
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << OFFLINE_TRIGGER_PIN,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&cfg);
    static const periph_pin_t trigger_pin = {
        .function = Input_Aux4, .group = PinGroup_AuxInput,
        .pin = OFFLINE_TRIGGER_PIN, .description = "Offline trigger (active low)"
    };
    hal.periph_port.register_pin(&trigger_pin);
    stable_high = raw_high = gpio_get_level(OFFLINE_TRIGGER_PIN) != 0;
    changed_at = hal.get_elapsed_ticks();
    previous_reset = hal.driver_reset;
    hal.driver_reset = reset_offline;
    previous_realtime = grbl.on_execute_realtime;
    grbl.on_execute_realtime = poll_trigger;
}
