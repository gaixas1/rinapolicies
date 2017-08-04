//rmt-eqta.h
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

/// Data structures

typedef struct q_entry_s {
	list_h L;
	pdu_p data;
	u32 cost; // PDU + headers cost
} q_entry;

typedef struct policer_c_t {	
	u8 next_module; //* Module towards where forward PDUs. N > 0 -> ps[N-1], else Mux
	u8 urgency_level; //* Urgency level of the ps (Only if next < 0)
	u16 cherish_th; //* Cherish thresold of the ps (Only if next < 0)
	u16 ecn_th; //* ECN thresold of the ps (Only if next < 0)
	u16 max_count; //* Max amount of PDUs admited
	u64 gain_us; //* Credits gain each us
	u64 max_credits; //* Max amount of accumulated credits
} policer_c;

typedef struct policer_d_t {
	list_h Q; // PS queue of q_entry (not part of list of ps_data_t)
	u16 count; // Amount of PDUs stored
	s64 credits; // Amount of accumulated credits
} policer_d;

typedef struct queue_t {
	u16 count; // Amount of PDUs stored
} queue;

typedef struct qos2module_t {
	list_h L;
	qos_id_t qos_id;
	u8 next_module;
	u8 def_urgency; // Level of urgency
	u16 def_cherish_th; // Level of cherish
	u16 def_ecn_th; // Level of cherish
} qos2module;

typedef struct port_instance_t {
	list_h L;
	port_p P;
	struct timespec lastT;	// "Time" of last call
	policer_d * policers; // ps modules, len == eqta_config.num_ps
	list_h * Qs; // Urgency queues in the mux, len == eqta_config.levels_urgency
	u16 mux_count; // Amount of PDUs waiting on the mux queues
	u16 count; // Amount of PDUs waiting on all port queues
	u16 max_count; // Max amount of PDUs waiting on all port queues
} port_instance;

typedef struct base_config_t {
	u8 state;
	u8 headers_weight; //* Extra weight of headers
	u8 num_policers; //*2 Number of ps in the module
	u8 levels_urgency; //*1 Levels of urgency
	u8 bytecost; // credit cost per byte
	u16 max_count; //* Max ocupation on mux
	u16 global_max_count; //* Max ocupation on port
	u16 buffer_size; //* Size of buffer of q_entries
	policer_c * policers; // Configuration of policer/shaper modules, len == num_ps
	list_h buffer; //* Buffer of q_entries
	list_h qos2modules; // List mapping QoS_id to ps index
	list_h port_instances; // List storing port instances
} base_config;

/* Function headers */

static struct ps_base * f_policy_create(struct rina_component * component);
static void f_policy_destroy(struct ps_base * bps);

void * f_rmt_q_create_policy(struct rmt_ps *ps, port_p P);
int f_rmt_q_destroy_policy(struct rmt_ps *ps, port_p P);
int f_rmt_enqueue_policy(struct rmt_ps *ps, port_p P, pdu_p pdu_i);
pdu_p f_rmt_dequeue_policy(struct rmt_ps *ps, port_p P);

static int f_set_policy_set_param(struct ps_base * bps, const char * name, const char * value);
static int f_policy_base_config_apply(struct policy_parm * param, void * data);

static int f_policy_set_param_pv(base_config * data, const char * name, const char * value);

void f_free_port_instance(base_config * conf, port_instance * port_i);
