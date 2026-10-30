"""Host regression tests compile the actual offline module with a fake HAL.
Run: python3 tests/test_t1_offline.py (requires a C compiler).
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/t1_offline.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
source = source.replace('extern const uint8_t job_start[] asm("_binary_t1_job_start");',
                        'static const uint8_t job_start[] = "G21\\nM5\\nM2\\n";')
source = source.replace('extern const uint8_t job_end[] asm("_binary_t1_job_end");',
                        'static const uint8_t *job_end = job_start + sizeof(job_start) - 1;')
mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
typedef int status_code_t;
typedef int sys_state_t;
typedef void (*on_execute_realtime_ptr)(sys_state_t);
typedef void (*driver_reset_ptr)(void);
typedef bool (*enqueue_realtime_command_ptr)(uint8_t);
typedef struct {size_t size;} vfs_file_t;
typedef struct {bool directory,read_only,hidden;} vfs_st_mode_t;
typedef struct {
const char *fs_name;
vfs_file_t *(*fopen)(const char*,const char*);
void (*fclose)(vfs_file_t*);
size_t (*fread)(void*,size_t,size_t,vfs_file_t*);
size_t (*ftell)(vfs_file_t*);
int (*fseek)(vfs_file_t*,size_t);
bool (*feof)(vfs_file_t*);
} vfs_t;
typedef struct {int function,group,pin;const char *description;} periph_pin_t;
typedef struct {uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type;} gpio_config_t;
#define OFFLINE_TRIGGER_PIN 36
#define GPIO_MODE_INPUT 0
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define Input_Aux4 4
#define PinGroup_AuxInput 1
#define On true
#define STATE_IDLE 0
#define STATE_CYCLE 1
#define Status_OK 0
struct {bool abort;struct{int mask;} homed;} sys;
static unsigned now, rx_calls, resets, reports, starts;
static int level=1, state, rx_count;
static bool redirected;
static void (*pending)(void*);
static const vfs_t *mounted;
static enqueue_realtime_command_ptr rx;
static void noop(void){}
static void realtime(int s){}
static void write_text(const char *s){}
static status_code_t report(status_code_t s){reports++;return s;}
static bool original_rx(uint8_t c){rx_calls++;return c=='?';}
static enqueue_realtime_command_ptr set_rx(enqueue_realtime_command_ptr fn){enqueue_realtime_command_ptr old=rx;rx=fn;return old;}
static unsigned ticks(void){return now;}
static unsigned available(void){return rx_count;}
static void reg_pin(const periph_pin_t *p){}
struct {on_execute_realtime_ptr on_execute_realtime;struct{status_code_t(*status_message)(status_code_t);}report;} grbl;
struct {
struct {void(*write)(const char*);void(*reset_read_buffer)(void);enqueue_realtime_command_ptr(*set_enqueue_rt_handler)(enqueue_realtime_command_ptr);unsigned(*get_rx_buffer_count)(void);} stream;
unsigned(*get_elapsed_ticks)(void);
driver_reset_ptr driver_reset;
struct{void(*register_pin)(const periph_pin_t*);}periph_port;
} hal;
static int gpio_get_level(int pin){return level;}
static void gpio_config(gpio_config_t *p){assert(p->pull_up_en==0&&p->pull_down_en==0);}
static int state_get(void){return state;}
static bool stream_is_file(void){return redirected;}
static bool vfs_mount(void *p,const char *path,const vfs_t *fs,vfs_st_mode_t mode){mounted=fs;return true;}
static vfs_file_t *stream_redirect_read(char *path,status_code_t(*err)(status_code_t),status_code_t(*end)(vfs_file_t*,status_code_t)){
vfs_file_t *f=mounted->fopen("/job.gc","r"); redirected=f!=NULL;if(f)starts++;return f;
}
static void stream_redirect_close(vfs_file_t *f){mounted->fclose(f);redirected=false;}
static bool protocol_buffer_synchronize(void){return !sys.abort;}
static status_code_t gc_execute_block(char *s){return 0;}
static void mc_reset(void){resets++;sys.abort=true;}
static bool task_add_immediate(void(*fn)(void*),void *arg){assert(!pending);pending=fn;return true;}
static void task_delete(void(*fn)(void*),void *arg){if(pending==fn)pending=NULL;}
'''
test = r'''
static void tick(unsigned dt){now+=dt;poll_trigger(state);if(pending){void(*fn)(void*)=pending;pending=NULL;fn(NULL);}}
static void press(void){level=0;tick(1);tick(30);}
static void release(void){level=1;tick(1);tick(30);}
int main(void){
rx=original_rx;hal.stream.write=write_text;hal.stream.reset_read_buffer=noop;
hal.stream.set_enqueue_rt_handler=set_rx;hal.stream.get_rx_buffer_count=available;
hal.get_elapsed_ticks=ticks;hal.driver_reset=noop;hal.periph_port.register_pin=reg_pin;
grbl.on_execute_realtime=realtime;grbl.report.status_message=report;
sys.homed.mask=3;t1_offline_init();
press();assert(starts==0); // cannot run before calibration
release();t1_offline_calibrated();tick(30);
level=0;tick(1);tick(15);level=1;tick(1);tick(30);assert(starts==0); // reject bounce
press();assert(starts==1&&job);tick(1000);assert(starts==1); // held button
assert(rx('G')&&rx('?')&&rx_calls==2); // host data discarded, realtime forwarded
char data[32]={0};assert(job_read(data,1,sizeof(data),job)==sizeof(job_start)-1);
assert(!strcmp(data,(const char*)job_start));assert(job_eof(job));
assert(job_seek(job,job->size+1)==-1);assert(job_read(data,0,10,job)==0);
release();press();assert(starts==1); // press during job is consumed
job_finished(job,Status_OK);assert(!job&&!redirected&&rx==original_rx);
tick(100);assert(starts==1); // no deferred replay of busy press
release();press();assert(starts==2);assert(job_pos==0); // repeat opens from start
reset_offline();assert(!job&&!calibrated&&rx==original_rx&&!opened);
release();press();assert(starts==2); // reset invalidates calibration
release();t1_offline_calibrated();tick(30);press();assert(starts==3);
job_error(20);assert(!job&&resets==1&&reports==1&&rx==original_rx);
puts("PASS: debounce, calibration gate, held/busy trigger, embedded stream, EOF, reset and error cleanup");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'offline.c'
    c.write_text(mock + '\n' + source + '\n' + test)
    binary = Path(tmp) / 'offline'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
