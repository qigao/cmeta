#include "../gmssl_mac.h"
#include "test_mac.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "%s:%d failed: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while(0)

typedef struct link_packet {
  uint8_t bytes[RL_MAX_WIRE_BYTES];
  size_t size;
  unsigned destination;
} link_packet;
typedef struct pair pair;
typedef struct node {
  pair *p;
  unsigned index;
  rl_gmssl_lease lease;
  rl_session protocol;
  rl_ticket ticket;
  unsigned delivered;
  unsigned terminals;
  rl_terminal terminal_kind;
  uint64_t terminal_tag;
} node;
struct pair {
  link_packet wire[32];
  size_t count;
  node peers[2];
};

static int emit(void *user, const void *data, size_t size) {
  node *n=(node *)user;
  if (n->p->count == 32 || size > RL_MAX_WIRE_BYTES) return 0;
  link_packet *w=&n->p->wire[n->p->count++];
  memcpy(w->bytes,data,size); w->size=size; w->destination=1u-n->index;
  return 1;
}
static void on_deliver(void *user,uint32_t id,const void *bytes,size_t len) {
  node *n=(node *)user;
  (void)id;(void)bytes;(void)len;
  ++n->delivered;
}
static void on_terminal(void *user,uint64_t tag,rl_terminal result) {
  node *n=(node *)user;
  ++n->terminals;n->terminal_tag=tag;n->terminal_kind=result;
}
static rl_config new_config(unsigned index, uint32_t epoch) {
  rl_config cfg={0};
  cfg.local_id=index ? 29u : 17u;
  cfg.peer_id=index ? 17u : 29u;
  cfg.epoch=epoch;
  cfg.first_message_id=1;
  cfg.rto_ms=10;
  cfg.ack_delay_ms=0;
  cfg.deadline_ms=200;
  cfg.window=3;
  cfg.max_retries=4;
  fixture_set_directional_keys(&cfg); /* deterministic TEST keys, not deployed */
  if(epoch!=0xabc11234u) {
    cfg.tx_auth_key[0]^=(uint8_t)epoch;
    cfg.rx_auth_key[0]^=(uint8_t)epoch;
  }
  return cfg;
}
static int begin(node *n,uint64_t generation,uint32_t epoch) {
  rl_config cfg=new_config(n->index,epoch);
  rl_callbacks cb={emit,on_deliver,on_terminal,n,rl_gmssl_mac,&n->lease};
  CHECK(rl_start(&n->protocol,&cfg,cb,generation,&n->ticket)==RL_OK);
  CHECK(rl_gmssl_lease_bind(&n->lease,&n->protocol,n->ticket)==RL_GMSSL_OK);
  return 0;
}
static int setup(pair *p) {
  memset(p,0,sizeof(*p));
  for (unsigned i=0;i<2;++i) {
    p->peers[i].index=i;
    p->peers[i].p=p;
    CHECK(begin(&p->peers[i],1,0xabc11234u)==0);
  }
  return 0;
}
static link_packet take(pair *p,size_t index) {
  link_packet item=p->wire[index];
  memmove(p->wire+index,p->wire+index+1,(p->count-index-1)*sizeof(*p->wire));
  --p->count;
  return item;
}
static rl_result receive(pair *p,link_packet w,uint64_t now) {
  node *dst=&p->peers[w.destination];
  return rl_input(&dst->protocol,dst->ticket,w.bytes,w.size,now);
}
static int advance(node *n,uint64_t now,size_t budget) {
  size_t sent=0;
  CHECK(rl_step(&n->protocol,n->ticket,now,budget,&sent)==RL_OK);
  CHECK(sent<=budget);
  return (int)sent;
}
static int close_node(node *n) {
  CHECK(rl_seal(&n->protocol,n->ticket)==RL_OK);
  CHECK(rl_gmssl_lease_seal(&n->lease,n->ticket)==RL_GMSSL_OK);
  CHECK(rl_abort(&n->protocol,n->ticket)==RL_OK);
  CHECK(rl_stop(&n->protocol,n->ticket)==RL_OK);
  CHECK(rl_gmssl_lease_release(&n->lease,n->ticket)==RL_GMSSL_OK);
  for(size_t i=0;i<RL_KEY_BYTES;i++) {
    CHECK(n->protocol.config.tx_auth_key[i]==0);
    CHECK(n->protocol.config.rx_auth_key[i]==0);
  }
  return 0;
}

static int gmssl_hmac_matches_independent_provider(void) {
  pair p;CHECK(setup(&p)==0);
  node *n=&p.peers[0];
  uint8_t message[RL_WIRE_HEADER_BYTES+12]={0};
  uint8_t calculated[RL_MAC_BYTES]={0},expected[RL_MAC_BYTES]={0};
  for(size_t i=0;i<sizeof(message);i++) message[i]=(uint8_t)(i*7u);
  CHECK(rl_gmssl_mac(&n->lease,n->protocol.config.tx_auth_key,message,sizeof(message),calculated));
  CHECK(fixture_hmac_sha256(NULL,n->protocol.config.tx_auth_key,message,sizeof(message),expected));
  CHECK(memcmp(calculated,expected,RL_MAC_BYTES)==0);
  CHECK(rl_gmssl_lease_release(&n->lease,n->ticket)==RL_GMSSL_BUSY);
  CHECK(close_node(n)==0);
  CHECK(close_node(&p.peers[1])==0);
  CHECK(!rl_gmssl_mac(&n->lease,expected,message,sizeof(message),calculated));
  return 0;
}
static int authenticated_roundtrip_and_wrong_mac(void) {
  pair p;CHECK(setup(&p)==0);
  uint8_t payload[150];
  for(size_t i=0;i<sizeof(payload);i++) payload[i]=(uint8_t)(i+1u);
  CHECK(rl_send(&p.peers[0].protocol,p.peers[0].ticket,payload,sizeof(payload),83u,0)==RL_OK);
  CHECK(advance(&p.peers[0],0,4)==3);
  link_packet packet=take(&p,0);
  link_packet corrupted=packet;
  corrupted.bytes[corrupted.size-1]^=0x40;
  CHECK(receive(&p,corrupted,1)==RL_AUTH_FAILED);
  CHECK(p.peers[1].delivered==0 && p.peers[0].terminals==0);
  CHECK(receive(&p,packet,1)==RL_OK);
  while(p.count) CHECK(receive(&p,take(&p,0),1)==RL_OK);
  CHECK(p.peers[1].delivered==1 && p.peers[0].terminals==0);
  CHECK(advance(&p.peers[1],1,3)==1);
  CHECK(p.count==1);
  CHECK(receive(&p,take(&p,0),2)==RL_OK);
  CHECK(p.peers[0].terminals==1 && p.peers[0].terminal_kind==RL_TERMINAL_ACKED &&
        p.peers[0].terminal_tag==83u);
  CHECK(close_node(&p.peers[0])==0);
  CHECK(close_node(&p.peers[1])==0);
  return 0;
}

typedef struct worker_arg {
  node *n;
  bool mac_result;
  rl_gmssl_status seal_result;
} worker_arg;
static void *worker(void *context) {
  worker_arg *w=(worker_arg*)context;
  uint8_t data[RL_WIRE_HEADER_BYTES]={0};
  uint8_t out[RL_MAC_BYTES]={0};
  w->mac_result=rl_gmssl_mac(&w->n->lease,w->n->protocol.config.tx_auth_key,
                              data,sizeof(data),out);
  w->seal_result=rl_gmssl_lease_seal(&w->n->lease,w->n->ticket);
  return NULL;
}
static int cross_owner_lifecycle_and_generation_fence(void) {
  pair p;CHECK(setup(&p)==0);
  node *n=&p.peers[0];
  worker_arg arg={n,true,RL_GMSSL_OK};
  pthread_t tid;
  CHECK(pthread_create(&tid,NULL,worker,&arg)==0);
  CHECK(pthread_join(tid,NULL)==0);
  CHECK(!arg.mac_result && arg.seal_result==RL_GMSSL_WRONG_OWNER);
  rl_ticket old=n->ticket;
  CHECK(rl_gmssl_lease_bind(&n->lease,&n->protocol,old)==RL_GMSSL_BUSY);
  CHECK(rl_seal(&n->protocol,old)==RL_OK);
  CHECK(rl_gmssl_lease_seal(&n->lease,old)==RL_GMSSL_OK);
  CHECK(rl_gmssl_lease_release(&n->lease,old)==RL_GMSSL_BUSY); /* protocol still owns keys */
  CHECK(rl_stop(&n->protocol,old)==RL_OK);
  CHECK(rl_gmssl_lease_release(&n->lease,old)==RL_GMSSL_OK);
  CHECK(rl_gmssl_lease_release(&n->lease,old)==RL_GMSSL_INVALID);
  CHECK(begin(n,2,0xabc11235u)==0);
  CHECK(rl_gmssl_lease_seal(&n->lease,old)==RL_GMSSL_STALE);
  uint8_t payload[2]={0x31,0x41};
  CHECK(rl_send(&n->protocol,old,payload,sizeof(payload),50,0)==RL_STALE);
  CHECK(rl_send(&n->protocol,n->ticket,payload,sizeof(payload),51,0)==RL_OK);
  CHECK(advance(n,0,1)==1);
  link_packet frame=take(&p,0);
  CHECK(receive(&p,frame,0)==RL_PROTOCOL); /* peer still on N, wrong epoch */
  CHECK(rl_cancel(&n->protocol,n->ticket)==RL_OK);
  CHECK(n->terminals==1 && n->terminal_kind==RL_TERMINAL_CANCELLED);
  CHECK(close_node(n)==0);
  CHECK(close_node(&p.peers[1])==0);
  return 0;
}

int main(void) {
  if(gmssl_hmac_matches_independent_provider()) return 1;
  if(authenticated_roundtrip_and_wrong_mac()) return 1;
  if(cross_owner_lifecycle_and_generation_fence()) return 1;
  puts("GmSSL R2 MAC provider and scoped Generation lease: PASS");
  return 0;
}
