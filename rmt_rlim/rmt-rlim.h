//rmt-rlim.h
#include <linux/module.h>
#include <linux/list.h>
#include <linux/time.h>
#include <linux/export.h>
#include <linux/string.h>

#include "logs.h"
#include "rds/rmem.h"
#include "rmt-ps.h"
#include "policies.h"
#include "debug.h"

/*
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned long u32;
typedef signed long s32;
typedef unsigned long long u64;
typedef signed long long s64;
*/

typedef struct list_head list_h;
typedef struct pdu * pdu_p;
typedef struct rmt_n1_port * port_p;
typedef struct timespec Time_t;

/// Data structures

typedef struct q_entry_s {
	list_h L;
	
	pdu_p data;
	u32 cost;
} q_entry;

typedef struct queue_t {
	u16 count;
	u8 urgency;
	u8 cherish;
	
	list_h q;
} queue;

typedef struct qos2CU_t {
	list_h L;
	
	qos_id_t qos_id;
	u8 urgency;
	u8 cherish;
	u16 ecn;
} qos2CU;

typedef struct port_instance_t {
	list_h L;
	port_p P;
	
	u16 count;
	s64 * credits_u;
	s64 * credits_c;
	queue * Q;
	Time_t lastT;
} port_instance;

typedef struct base_config_s {
	u16 max_count;
	u16 default_ecn;
	u16 buffer_size;
	u8 levels_urgency;
	u8 levels_cherish;
	u16 num_queues;
	u16 headers_weight;
	u8 bytecost;
	u8 S;
	u64 * gain_us_u;
	u64 * max_credit_u;
	u64 * gain_us_c;
	u64 * max_credit_c;
	u16 * th_c;
	list_h buffer;
	list_h port_instances;
	list_h Q2CU;
} base_config;

/* Function headers */

static struct ps_base * f_policy_create(struct rina_component * component);
static void f_policy_destroy(struct ps_base * bps);

void * f_rmt_q_create_policy(struct rmt_ps *ps, port_p P);
int f_rmt_q_destroy_policy(struct rmt_ps *ps, port_p P);
int f_rmt_enqueue_policy(struct rmt_ps *ps, port_p P, pdu_p pdu_i);
pdu_p f_rmt_dequeue_policy(struct rmt_ps *ps, port_p P);

//static int f_set_policy_set_param(struct ps_base * bps, const char * name, const char * value);
static int f_policy_base_config_apply(struct policy_parm * param, void * data);

static int f_policy_set_param_pv(base_config * data, const char * name, const char * value);

void f_free_port_instance(base_config * conf, port_instance * port_i);
