"""Exercise the actual first-boot transaction with storage failure injection."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source='\n'.join(l for l in (root/'main/factory_init.c').read_text().splitlines() if not l.startswith('#include'))
mock=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
typedef int nvs_handle_t; typedef int esp_err_t; typedef int status_code_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READWRITE 1
#define Status_OK 0
#define NVS_TransferResult_OK 0
#define N_STARTUP_LINE 2
static bool marked, committed, bad_setting, bad_read, bad_commit, abort_sound;
static unsigned sounds, writes, startup_writes, syncs;
static int opens_error;
static unsigned marker_value=2;
static unsigned char memory[16];
static struct {bool is_dirty;} settings_dirty;
static int nvs_open(const char *a,int b,int *h){*h=1;return opens_error;}
static int nvs_get_u32(int h,const char *k,uint32_t *v){*v=marked?marker_value:0;return marked?0:1;}
static void nvs_close(int h){}
static int nvs_set_u32(int h,const char *k,uint32_t v){assert(syncs&&v==2);return 0;}
static int nvs_commit(int h){if(bad_commit)return 2;marked=committed=true;marker_value=2;return 0;}
static int settings_store_setting(unsigned id,char *v){writes++;settings_dirty.is_dirty=true;return bad_setting?2:0;}
static void settings_write_startup_line(unsigned i,char *line){assert(!*line);startup_writes++;}
static void nvs_buffer_sync_physical(void){syncs++;settings_dirty.is_dirty=false;}
static bool nvsRead(uint8_t *d){memcpy(d,memory,16);if(bad_read)d[0]^=1;return true;}
static int read_buffer(uint8_t *d,unsigned a,unsigned n,bool crc){memcpy(d,memory,n);return 0;}
static struct {struct {unsigned size;int(*memcpy_from_nvs)(uint8_t*,unsigned,unsigned,bool);} nvs;} hal={{16,read_buffer}};
static bool sound(void){sounds++;return !abort_sound;}
'''
test=r'''
static void reset(void){marked=committed=bad_setting=bad_read=bad_commit=abort_sound=false;sounds=writes=startup_writes=syncs=0;opens_error=0;marker_value=2;}
int main(void){
 reset();assert(factory_initialize(sound));assert(marked&&sounds==1&&writes==89&&startup_writes==2&&syncs==1);
 assert(factory_initialize(sound));assert(sounds==1&&writes==89); // no overwrite on reboot
 reset();marked=true;marker_value=1;assert(factory_initialize(sound));assert(marker_value==2&&writes==89);
 reset();marked=true;marker_value=99;assert(!factory_initialize(sound));assert(!writes&&!sounds);
 reset();bad_setting=true;assert(!factory_initialize(sound));assert(!marked&&!syncs);
 bad_setting=false;assert(factory_initialize(sound));assert(marked&&sounds==2); // interrupted initialization retry
 reset();bad_read=true;assert(!factory_initialize(sound));assert(!marked);
 reset();bad_commit=true;assert(!factory_initialize(sound));assert(!marked);
 reset();abort_sound=true;assert(!factory_initialize(sound));assert(!marked&&!writes);
 reset();opens_error=2;assert(!factory_initialize(sound));assert(!sounds&&!writes);
 puts("PASS: first initialization, skip, interrupted retry, readback failure, marker failure, abort, storage open failure");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(mock+(root/'main/factory_settings.h').read_text()+source+test)
 subprocess.run(['cc','-std=c99',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
