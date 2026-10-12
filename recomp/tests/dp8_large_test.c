#include "dp8proto.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <arpa/inet.h>
static const size_t lengths[]={0,4,73080,73081,1048576,4};
typedef struct {atomic_int ready,messages;} State;
static void connected(void *s,dp8_conn *c){(void)c;((State*)s)->ready=1;}
static void received(void *s,dp8_conn *c,uint8_t flags,const uint8_t *d,size_t n){
 (void)c;(void)flags;State *st=s;int i=st->messages;assert(i<6&&n==lengths[i]);
 for(size_t k=0;k<n;k++)assert(d[k]==(uint8_t)(k*13+i));st->messages++;
}
int main(void){
 State hs={0},cs={0};dp8_callbacks hc={&hs,0,0,connected,0,received,0},cc={&cs,0,0,0,connected,received,0};
 dp8_ep *h=dp8_open(0,0,1,0,&hc),*cli=dp8_open(0,0,0,0,&cc);assert(h&&cli);
 struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(dp8_port(h))};a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);dp8_conn *c=dp8_connect(cli,&a);
 for(int i=0;i<10000&&(!hs.ready||!cs.ready);i++)usleep(1000);assert(hs.ready&&cs.ready);
 for(int i=0;i<6;i++){size_t n=lengths[i];uint8_t *data=malloc(n+1);assert(data);for(size_t k=0;k<n;k++)data[k]=(uint8_t)(k*13+i);assert(!dp8_send(c,data,n,0));free(data);}
 for(int i=0;i<30000&&hs.messages<6;i++)usleep(1000);assert(hs.messages==6);
 dp8_close(cli);dp8_close(h);puts("DP8: empty, boundary and multi-window messages arrive intact and in order");
}
