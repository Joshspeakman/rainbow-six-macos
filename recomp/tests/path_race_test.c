#include "w32.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>
#include <assert.h>
extern char w32_drive_c[1024],w32_game_overlay[1024],w32_game_layer[1024];
char w32_game_dir[1024];
static atomic_int done;static unsigned incorrect;
int w32_host_dirs(const char *,char [3][2048]);
static void *enumerate(void *unused){
 (void)unused;char dirs[3][2048];
 for(int i=0;i<20000;i++){
  assert(w32_host_dirs(".",dirs)==3);
  assert(!strcmp(dirs[0],w32_game_overlay)&&!strcmp(dirs[1],w32_game_layer)&&!strcmp(dirs[2],w32_game_dir));
 }
 done=1;return 0;
}
static void put(const char *p,int ch){FILE *f=fopen(p,"wb");assert(f);assert(fputc(ch,f)==ch);assert(!fclose(f));}
static int get(const char *p){FILE *f=fopen(p,"rb");assert(f);int ch=fgetc(f);fclose(f);return ch;}
int main(void){
 char root[]="/tmp/native-path-test-XXXXXX";assert(mkdtemp(root));
 snprintf(w32_game_dir,1024,"%s/original",root);snprintf(w32_game_overlay,1024,"%s/overlay",root);
 snprintf(w32_game_layer,1024,"%s/layer",root);snprintf(w32_drive_c,1024,"%s/drive_c",root);
 assert(!mkdir(w32_game_dir,0700)&&!mkdir(w32_game_overlay,0700)&&!mkdir(w32_game_layer,0700)&&!mkdir(w32_drive_c,0700));
 char base[2048],layer[2048],overlay[2048],path[2048];
 snprintf(base,sizeof base,"%s/settings.ini",w32_game_dir);snprintf(layer,sizeof layer,"%s/settings.ini",w32_game_layer);
 snprintf(overlay,sizeof overlay,"%s/settings.ini",w32_game_overlay);put(base,'B');put(layer,'L');
 assert(!w32_host_path("settings.ini",path,sizeof path,0)&&!strcmp(path,layer));
 assert(!w32_host_path("settings.ini",path,sizeof path,1)&&!strcmp(path,overlay)&&get(path)=='L');
 put(path,'W');assert(get(base)=='B'&&get(layer)=='L');
 assert(!w32_host_path("settings.ini",path,sizeof path,0)&&!strcmp(path,overlay));
 pthread_t t;assert(!pthread_create(&t,0,enumerate,0));unsigned total=0;
 while(!done){
  assert(!w32_host_path("settings.ini",path,sizeof path,1));
  if(strcmp(path,overlay))incorrect++;total++;
 }
 pthread_join(t,0);assert(total>0&&!incorrect);
 assert(!unlink(base)&&!unlink(layer)&&!unlink(overlay));
 assert(!rmdir(w32_game_dir)&&!rmdir(w32_game_overlay)&&!rmdir(w32_game_layer)&&!rmdir(w32_drive_c)&&!rmdir(root));
 printf("paths: overlay/layer precedence, copy-up and %u concurrent write resolutions passed\n",total);
}
