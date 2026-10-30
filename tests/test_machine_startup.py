"""Exercise the actual latch and sound functions against GPIO/clock/LEDC fakes."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
full_source=(root/'main/my_plugin.c').read_text()
factory_sound_source=full_source[full_source.index('static bool factory_sound ('):full_source.index('static void calibrate_on_boot')]
source=full_source
source=source[source.index('static driver_setup_ptr previous_setup;'):source.index('static bool machine_setup')]
mock=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef int gpio_num_t;
typedef void *driver_setup_ptr;
typedef void *esp_timer_handle_t;
#define STEPPERS_ENABLE_PIN 8
#define STEPPERS_STBY_PIN 7
#define X_STEP_PIN 9
#define Y_STEP_PIN 4
#define X_DIRECTION_PIN 10
#define Y_DIRECTION_PIN 2
#define X_LIMIT_PIN 38
#define Y_LIMIT_PIN 35
#define OFFLINE_TRIGGER_PIN 36
#define GPIO_MODE_INPUT_OUTPUT_OD 3
#define GPIO_MODE_INPUT_OUTPUT 2
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_ONLY 1
#define GPIO_FLOATING 0
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_3 3
#define LEDC_CHANNEL_7 7
static int levels[49], modes[49];
static uint64_t clock_us, latched_at;
static unsigned frequency, duty, latch_count;
static int gpio_get_level(int pin){return levels[pin];}
static int gpio_set_level(int pin,int value){
 if(pin==7&&value==1){
  assert(clock_us>=1000);assert(levels[8]==0&&modes[8]==3);
  assert(!levels[9]&&!levels[4]&&!levels[10]&&!levels[2]);
  latched_at=clock_us;latch_count++;
 }
 levels[pin]=value;return 0;
}
static int gpio_set_direction(int pin,int mode){modes[pin]=mode;return 0;}
static int gpio_set_pull_mode(int pin,int mode){return 0;}
static void esp_rom_delay_us(unsigned us){clock_us+=us;}
static uint64_t esp_timer_get_time(void){return clock_us;}
static int ledc_set_freq(int mode,int timer,unsigned hz){assert(timer==3);frequency=hz;return 0;}
static int ledc_set_duty(int mode,int channel,unsigned value){assert(channel==7);duty=value;return 0;}
static int ledc_update_duty(int mode,int channel){assert(channel==7);return 0;}
static int ledc_stop(int mode,int channel,int level){assert(channel==7&&!level);frequency=duty=0;return 0;}
'''
factory_mock=r'''
#define DelayMode_Dwell 0
static struct {bool abort;} sys;
static void delay_ms(unsigned ms,void *cb){clock_us+=ms*1000;}
static struct {void (*delay_ms)(unsigned,void*);} hal={delay_ms};
static bool delay_sec(float seconds,int mode){assert(seconds==3.0f);assert(frequency==2400);sound_tick(NULL);assert(frequency==2400);clock_us+=3000000;return !sys.abort;}
'''
test=r'''
static void advance(unsigned ticks){while(ticks--){clock_us+=10000;sound_tick(NULL);}}
int main(void){
 for(int i=0;i<49;i++)levels[i]=1;
 stspin220_init();assert(latch_count==1&&clock_us-latched_at>=1000);
 assert(!levels[8]&&levels[7]);
 melody_pending=true;sound_tick(NULL);assert(frequency==1047&&duty==512);
 advance(25);assert(frequency==1319);advance(25);assert(frequency==1568);
 advance(25);assert(frequency==2093);advance(25);assert(frequency==0&&melody_count==1);
 levels[36]=0;advance(2);assert(frequency==0);advance(1);
 assert(frequency==2400&&beep_count==1&&trigger_count==1);
 advance(10);assert(!frequency);advance(20);assert(beep_count==1);
 levels[36]=1;advance(1);levels[36]=0;advance(3);assert(beep_count==2);
 levels[38]=0;advance(2);assert(limit_count[0]==1&&beep_count==3);
 levels[35]=0;advance(2);assert(limit_count[1]==1&&beep_count==4);
 advance(50);assert(!frequency&&beep_count==4);
 uint64_t begin=clock_us;assert(factory_sound());assert(clock_us-begin==3020000&&!frequency&&!factory_sound_active);
 sys.abort=true;assert(!factory_sound());assert(!frequency&&!factory_sound_active);
 puts("PASS: 3s factory tone and abort cleanup; STEP/DIR low at STBY rising, setup/hold margins, 1s melody, 100ms tones, trigger/limit debounce");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'machine.c';c.write_text(mock+source+factory_mock+factory_sound_source+test)
 binary=Path(tmp)/'machine'
 subprocess.run(['cc','-std=gnu11','-Wall','-Werror','-Wno-unused-variable',str(c),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
