"""Exercise actual USB TX loop with a configured device but no host reader."""
from pathlib import Path
import subprocess,tempfile
source=Path('main/usb_serial.c').read_text()
func=source[source.index('bool _usb_write (void)'):source.index('//\n// Writes a number',source.index('bool _usb_write (void)'))]
harness=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
static struct {char data[64]; char *s; size_t length;} txbuf;
static int64_t now;
static bool connected=true, proceed=true, usb_rx_seen=true;
static size_t available, written;
static int64_t esp_timer_get_time(void){return now+=1000;}
static bool usb_is_connected(void){return connected;}
static size_t tud_cdc_write_available(void){return available;}
static void usb_out_chars(const char *s,size_t n){written+=n;}
static bool callback(void){return proceed;}
static struct {bool (*stream_blocking_callback)(void);} hal={callback};
'''
main=r'''
int main(void){
 usb_rx_seen=false; txbuf.length=20; assert(!_usb_write()); assert(!txbuf.length); usb_rx_seen=true; now=0;
 txbuf.length=20; assert(!_usb_write()); assert(now>=100000 && now<110000); assert(!txbuf.length && txbuf.s==txbuf.data);
 now=0; connected=false; txbuf.length=20; assert(!_usb_write()); assert(now<100000 && !txbuf.length);
 connected=true; available=16; txbuf.length=20; assert(_usb_write()); assert(written==20 && !txbuf.length && txbuf.s==txbuf.data);
 available=0; proceed=false; now=0; txbuf.length=20; assert(!_usb_write()); assert(now<100000 && !txbuf.length);
 puts("PASS: no-reader timeout, disconnected drop, multi-chunk TX, realtime abort");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(harness+func+main)
 subprocess.run(['cc','-std=c99',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
