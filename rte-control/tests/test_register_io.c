/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <rte/register_io.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
/* Deliberately independent golden map; do not derive the mock from the ABI. */
static const unsigned map[4][7]={
 {0x100,0x110,0x120,0x130,0x140,0x14c,0x160},
 {0x104,0x114,0x124,0x134,0x144,0x150,0x164},
 {0x108,0x118,0x128,0x138,0x158,0x154,0x168},
 {0x10c,0x11c,0x12c,0x13c,0x148,0x15c,0x16c}};
static const unsigned masks[7]={0x3ff,0xff,0xffffffff,0xffffffff,0x1ffffff,1,1};
struct mock {
 uint32_t bank[0x170/4], active[4][6];
 unsigned writes[0x170/4], stage_reads[4][6], write_count;
 unsigned fail_write, fail_read_offset;
 bool fail_high_read, block_low, fault_seen;
};
static void tick(struct mock *m) {
 for(unsigned t=0;t<4;t++) if(m->bank[map[t][6]/4]&1)
  for(unsigned j=0;j<6;j++) m->active[t][j]=m->bank[map[t][j]/4]&masks[j];
}
static int read_word(void *ctx,uint32_t off,uint32_t *v) {
 struct mock *m=ctx; assert(off<sizeof m->bank); tick(m);
 for(unsigned t=0;t<4;t++) for(unsigned j=0;j<6;j++)
  if(off==map[t][j]) m->stage_reads[t][j]++;
 if(!m->fault_seen && m->fail_read_offset==off) { m->fault_seen=true; return -EIO; }
 if(m->fail_high_read && !m->fault_seen && off>=0x160 && (m->bank[off/4]&1)) {
  m->fault_seen=true; return -EIO;
 }
 *v=m->bank[off/4]; return 0;
}
static int write_word(void *ctx,uint32_t off,uint32_t v) {
 struct mock *m=ctx; assert(off>=0x100 && off<sizeof m->bank);
 m->writes[off/4]++; m->write_count++;
 if(m->fail_write==m->write_count) {m->fault_seen=true; return -EIO;}
 if(off>=0x160 && v==0 && m->block_low) return -EIO;
 for(unsigned t=0;t<4;t++) {
  if(off==map[t][6] && v==1) for(unsigned j=0;j<6;j++) assert(m->stage_reads[t][j]>0);
  for(unsigned j=0;j<6;j++) if(off==map[t][j]) {
   assert(!(m->bank[map[t][6]/4]&1)); m->stage_reads[t][j]=0;
  }
 }
 m->bank[off/4]=v; tick(m); return 0;
}
static void init(struct mock *m) {
 memset(m,0,sizeof *m); m->bank[4/4]=1; m->bank[8/4]=2609182026U;
 m->fail_read_offset=UINT32_MAX;
 for(unsigned t=0;t<4;t++) for(unsigned j=0;j<6;j++)
  m->active[t][j]=m->bank[map[t][j]/4]=(t+j+1)&masks[j];
}
static void isolated(struct mock *m,unsigned target) {
 for(unsigned t=0;t<4;t++) if(t!=target)
  for(unsigned j=0;j<7;j++) assert(m->writes[map[t][j]/4]==0);
}
static struct rte_register_image previous(struct mock *m,unsigned t) {
 struct rte_register_image p;
 for(unsigned j=0;j<6;j++) p.words[j]=m->active[t][j];
 return p;
}
static void restored(struct mock *m,unsigned t,const struct rte_register_image *p) {
 assert(memcmp(m->active[t],p->words,sizeof p->words)==0);
 for(unsigned j=0;j<6;j++) assert(m->bank[map[t][j]/4]==p->words[j]);
 assert(m->bank[map[t][6]/4]==0); isolated(m,t);
}
int main(void) {
 struct mock m; struct rte_register_io io={&m,read_word,write_word};
 struct rte_register_image next={{23,31,0xfffffff0,0x98765432,1048576,1}},old,got;
 struct rte_error e;
 for(unsigned t=0;t<4;t++) for(unsigned j=0;j<7;j++) {
  assert(rte_target_registers[t][j].offset==map[t][j]);
  assert(rte_target_registers[t][j].mask==masks[j]);
 }
 for(unsigned t=0;t<4;t++) {
  init(&m); assert(rte_register_apply(&io,t,&next,NULL,&e)==0);
  assert(memcmp(m.active[t],next.words,sizeof next.words)==0); isolated(&m,t);
  assert(m.bank[map[t][6]/4]==0); assert(m.write_count==8);
  assert(rte_register_read_image(&io,t,&got,&e)==0);
  assert(memcmp(&next,&got,sizeof got)==0);
  for(unsigned fault=1;fault<=8;fault++) {
   init(&m); old=previous(&m,t); m.fail_write=fault;
   assert(rte_register_apply(&io,t,&next,&old,&e)!=0); restored(&m,t,&old);
   assert(strcmp(e.field,"rollback")!=0);
  }
  for(unsigned j=0;j<6;j++) {
   init(&m); old=previous(&m,t); m.fail_read_offset=map[t][j];
   assert(rte_register_apply(&io,t,&next,&old,&e)!=0); restored(&m,t,&old);
  }
  init(&m); old=previous(&m,t); m.fail_high_read=true;
  assert(rte_register_apply(&io,t,&next,&old,&e)!=0); restored(&m,t,&old);
  init(&m); m.fail_high_read=true;
  assert(rte_register_apply(&io,t,&next,NULL,&e)!=0);
  assert(strcmp(e.field,"commit_state")==0); isolated(&m,t);
  init(&m); m.fail_write=3;
  assert(rte_register_apply(&io,t,&next,NULL,&e)!=0);
  assert(strcmp(e.field,"commit_state")!=0 && strcmp(e.field,"recovery")!=0); isolated(&m,t);
  init(&m); old=previous(&m,t); m.block_low=true;
  assert(rte_register_apply(&io,t,&next,&old,&e)!=0);
  assert(strcmp(e.field,"recovery")==0); isolated(&m,t);
  for(unsigned j=0;j<6;j++) assert(m.writes[map[t][j]/4]==1); /* No restaging while LP high. */
  init(&m); m.bank[map[(t+1)%4][6]/4]=1;
  assert(rte_register_apply(&io,t,&next,NULL,&e)!=0); assert(m.write_count==0);
 }
 init(&m); m.bank[4/4]=0;
 assert(rte_register_apply(&io,0,&next,NULL,&e)!=0); assert(m.write_count==0);
 init(&m); assert(rte_register_validate_hardware(&io,RTE_EXPECTED_TIMESTAMP,&e)==0);
 m.bank[8/4]--; assert(rte_register_validate_hardware(&io,RTE_EXPECTED_TIMESTAMP,&e)!=0);
 assert(rte_register_apply(&io,4,&next,NULL,&e)!=0);
 next.words[0]=1024; assert(rte_register_apply(&io,0,&next,NULL,NULL)!=0); assert(m.write_count==0);
 init(&m); m.bank[0x110/4]=0xffffffff;
 assert(rte_register_read_image(&io,0,&got,&e)==0 && got.words[1]==255); /* Full hardware FD width is retained. */
 init(&m); m.bank[0x100/4]=0xfffffc07;
 assert(rte_register_read_image(&io,0,&got,&e)==0 && got.words[0]==7);
 puts("register IO isolation/recovery tests passed"); return 0;
}
