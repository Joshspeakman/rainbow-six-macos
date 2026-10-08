/* Opt-in local test lab. Requests run on the game thread through its own Skrit
 * compiler, so objects, spells and teleports follow the normal engine paths. */
#include "w32.h"
#include "edition.h"
#include "ext.h"
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

static char lab_dir[2048], lab_result[1024];
static pthread_t lab_thread;
static int lab_thread_set, lab_busy, lab_reported;
static uint32_t lab_scratch;
static uint32_t lab_objects[512], lab_target;
static int lab_count;

static void lab_reply(Ctx *c) {
    /* Do not hide an earlier failed spawn behind a later success in a batch. */
    if(lab_reported && !strncmp(lab_result,"ERROR:",6)) { c->esp+=4; return; }
    snprintf(lab_result,sizeof lab_result,"%s",GS(ARG(0)));
    for(char *p=lab_result;*p;p++) if(*p=='\n'||*p=='\r'||*p=='\t') *p=' ';
    lab_reported=1;
    c->esp+=4; /* cdecl: only pop the return address */
}
static void lab_track(Ctx *c) {
    uint32_t goid=ARG(0);
    if(lab_count<512) lab_objects[lab_count++]=goid;
    if(ARG(1)) lab_target=goid;
    c->esp+=4;
}
static void lab_get_target(Ctx *c) { c->eax=lab_target; c->esp+=4; }
static void lab_set_target(Ctx *c) { lab_target=ARG(0); c->esp+=4; }
static void lab_get_count(Ctx *c) { c->eax=lab_count; c->esp+=4; }
static void lab_get_object(Ctx *c) { c->eax=ARG(0)<(uint32_t)lab_count?lab_objects[ARG(0)]:0; c->esp+=4; }
static void lab_clear(Ctx *c) { lab_count=0; lab_target=0; c->esp+=4; }

void w32_lab_init(const char *data) {
    const char *enabled=getenv("DS_TEST_LAB");
    if(!enabled || strcmp(enabled,"1")) return;
    snprintf(lab_dir,sizeof lab_dir,"%s/lab",data);
    if(mkdir(lab_dir,0700)<0 && errno!=EEXIST) { lab_dir[0]=0; return; }
    ext_add("?NativeLabResult@@YAXPBD@Z",lab_reply);
    ext_add("?NativeLabTrack@@YAXPBUGoid_@@_N@Z",lab_track);
    ext_add("?NativeLabTarget@@YAPBUGoid_@@XZ",lab_get_target);
    ext_add("?NativeLabSetTarget@@YAXPBUGoid_@@@Z",lab_set_target);
    ext_add("?NativeLabCount@@YAHXZ",lab_get_count);
    ext_add("?NativeLabObject@@YAPBUGoid_@@H@Z",lab_get_object);
    ext_add("?NativeLabClear@@YAXXZ",lab_clear);
    if(ext_install()<0) lab_dir[0]=0;
}

void w32_lab_thread(void) {
    if(*lab_dir) { lab_thread=pthread_self(); lab_thread_set=1; }
}

void w32_lab_poll(Ctx *c) {
    if(!lab_thread_set || lab_busy || !pthread_equal(lab_thread,pthread_self())) return;
    /* No disk traffic on every PeekMessage: check at most five times/second. */
    static uint64_t next; struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    uint64_t now=(uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec;
    if(now<next) return;
    next=now+200000000;
    if(!DS2_SKRIT_MANAGER || !rt_r32(G_MEM,DS2_SKRIT_MANAGER)) return; /* Skrit manager has not started yet. */
    char path[2300]; snprintf(path,sizeof path,"%s/request",lab_dir);
    FILE *f=fopen(path,"r"); if(!f) return;
    char kind[16], code[16000]; unsigned id;
    int valid=fscanf(f,"%u %15s\n",&id,kind)==2;
    size_t length=fread(code,1,sizeof code-1,f); code[length]=0;
    if(!feof(f)) valid=0;
    fclose(f); unlink(path);
    if(!valid) return;
    lab_busy=1; lab_reported=0; lab_result[0]=0;
    Ctx saved=*c;
    if(!lab_scratch) lab_scratch=vm_alloc(0,16384,0x3000,4);
    if(lab_scratch) {
        memcpy(GP(lab_scratch),code,length+1);
        uint32_t args[]={lab_scratch,0x80000000u};
        if(!strcmp(kind,"eval")) {
            int value=(int)w32_callback(c,ext_export("?EvalInt@Skrit@@YAHPBDH@Z"),2,args);
            snprintf(lab_result,sizeof lab_result,"%d",value); lab_reported=1;
        } else if(!strcmp(kind,"exec")) {
            w32_callback(c,ext_export("?Command@Skrit@@YAXPBD@Z"),1,args);
        }
    }
    *c=saved; rt_fpcr_sync(c->fcw);
    int ok=lab_reported && strncmp(lab_result,"ERROR:",6);
    snprintf(path,sizeof path,"%s/response.tmp",lab_dir);
    f=fopen(path,"w");
    if(f) {
        fprintf(f,"%u\t%s\t%s\n",id,ok?"ok":"error",lab_reported?lab_result:"Script failed; check the test-lab log.");
        fclose(f); char target[2300]; snprintf(target,sizeof target,"%s/response",lab_dir); rename(path,target);
    }
    fprintf(stderr,"test-lab: request %u %s: %s\n",id,ok?"ok":"error",lab_result);
    lab_busy=0;
}
