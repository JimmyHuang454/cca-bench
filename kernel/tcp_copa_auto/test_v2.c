#include <assert.h>
#include <stdio.h>
#include "copa_auto_v2.h"
int main(void) {
 struct copa_v2_state v; struct copa_auto_state ca; struct copa_auto_cfg cfg = { .init_delta_q20 = COPA_DELTA_INIT_Q20 };
 copa_state_init(&ca,cfg,0,0,10); copa_v2_init(&v,16);
 v.r0_us=10000; v.count=2;
 v.rounds[0]=(struct copa_v2_round){.standing_sum=30000,.rate_sum=1000,.samples=2};
 v.rounds[1]=(struct copa_v2_round){.standing_sum=40000,.rate_sum=1200,.samples=2};
 v.rounds[2]=(struct copa_v2_round){.standing_sum=50000,.rate_sum=2000,.samples=2};
 assert(copa_v2_finish(&v,&ca)); assert(v.last_valid==2); assert(v.last_k_us==5000); assert(v.updates==1);
 assert(v.last_spread_milli == 100); assert(v.last_gain_milli >= 100 && v.last_gain_milli <= 500);
 ca.delta_q20 = 1048576; assert(copa_v2_finish(&v,&ca)); assert(ca.delta_q20 > COPA_DELTA_MAX_Q20);
 ca.delta_q20 = 500; assert(copa_v2_finish(&v,&ca)); assert(ca.delta_q20 < COPA_DELTA_MIN_Q20);
 u32 delta=ca.delta_q20; v.count=0; assert(!copa_v2_finish(&v,&ca)); assert(ca.delta_q20==delta); assert(v.last_k_us==0);
 copa_v2_init(&v,4); copa_state_init(&ca,cfg,0,0,10);
 copa_v2_on_ack(&v,&ca,1000,10000,10000,1,1000,100000,0,0); assert(v.end_us==41000);
 copa_v2_on_ack(&v,&ca,41000,20000,20000,1,1000,100000,0,0); assert(v.start_us==41000 && v.r0_us==20000 && ca.dq_us==0); assert(v.end_us==121000);
 /* Near-saturation outlier has little weight in the q-space robust fit. */
 copa_v2_init(&v,16); copa_state_init(&ca,cfg,0,0,10);
 v.r0_us=10000; v.count=3;
 v.rounds[0]=(struct copa_v2_round){.standing_sum=40000,.rate_sum=1000,.samples=2};
 v.rounds[1]=(struct copa_v2_round){.standing_sum=50000,.rate_sum=1200,.samples=2};
 v.rounds[2]=(struct copa_v2_round){.standing_sum=20020000,.rate_sum=1998,.samples=2};
 v.rounds[3]=(struct copa_v2_round){.standing_sum=40000,.rate_sum=2000,.samples=2};
 assert(copa_v2_finish(&v,&ca)); assert(v.last_valid==3); assert(v.last_k_us==10000);
 puts("v2 period isolation and fitting passed");
}
