/* uinput-test — create a virtual keyboard, type "hello", hold it open 60s */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
static int ui;
static void ev(int t,int c,int v){struct input_event e;memset(&e,0,sizeof e);
 e.type=t;e.code=c;e.value=v;write(ui,&e,sizeof e);}
static void tap(int k){ev(EV_KEY,k,1);ev(EV_SYN,SYN_REPORT,0);usleep(60000);
 ev(EV_KEY,k,0);ev(EV_SYN,SYN_REPORT,0);usleep(140000);}
int main(void){
 struct uinput_user_dev ud; int i;
 int keys[]={KEY_H,KEY_E,KEY_L,KEY_O,KEY_LEFT,KEY_RIGHT,KEY_UP,KEY_DOWN,KEY_ENTER,KEY_SPACE};
 ui=open("/dev/input/uinput",O_WRONLY);
 if(ui<0){perror("uinput");return 1;}
 ioctl(ui,UI_SET_EVBIT,EV_KEY); ioctl(ui,UI_SET_EVBIT,EV_SYN); ioctl(ui,UI_SET_EVBIT,EV_REP);
 for(i=0;i<10;i++) ioctl(ui,UI_SET_KEYBIT,keys[i]);
 memset(&ud,0,sizeof ud);
 snprintf(ud.name,UINPUT_MAX_NAME_SIZE,"padkeys Virtual Keyboard");
 ud.id.bustype=BUS_VIRTUAL; ud.id.vendor=0x1209; ud.id.product=0x0AD0; ud.id.version=1;
 if(write(ui,&ud,sizeof ud)!=sizeof ud){perror("write");return 1;}
 if(ioctl(ui,UI_DEV_CREATE)<0){perror("UI_DEV_CREATE");return 1;}
 printf("virtual keyboard created\n"); fflush(stdout);
 sleep(5);
 for(i=0;i<40;i++){
   printf("typing hello (%d)...\n",i); fflush(stdout);
   tap(KEY_H);tap(KEY_E);tap(KEY_L);tap(KEY_L);tap(KEY_O);tap(KEY_SPACE);
   sleep(8);
 }
 ioctl(ui,UI_DEV_DESTROY); return 0;
}
