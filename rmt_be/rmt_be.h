//rmt_be.h
#include <linux/module.h>
#include <linux/list.h>
#include <linux/export.h>
#include <linux/string.h>

#include "logs.h"
#include "rds/rmem.h"
#include "rmt-ps.h"
#include "policies.h"
#include "debug.h"

/// Data structures

//Queue entry
struct q_entry {
	struct list_head L;
	struct pdu * data;
};

// port instance information
struct port_instance {
	struct list_head L;
	struct rmt_n1_port * P;
	
	uint_t count;
	struct list_head Q;
};

// Configuration of the policy
struct base_config {
	uint_t max_count;
	struct list_head port_L;
	struct list_head buffer_L;
};

/// Function headers

static struct ps_base * f_policy_create(struct rina_component * component);
static void f_policy_destroy(struct ps_base * bps);

void * f_rmt_q_create_policy(struct rmt_ps *ps, struct rmt_n1_port * P);
int f_rmt_q_destroy_policy(struct rmt_ps *ps, struct rmt_n1_port * P);
int f_rmt_enqueue_policy(struct rmt_ps *ps, struct rmt_n1_port * P, struct pdu * PDU);
struct pdu * f_rmt_dequeue_policy(struct rmt_ps *ps, struct rmt_n1_port * P);

static int f_set_policy_set_param(struct ps_base * bps, const char * name, const char * value);
static int f_policy_base_config_apply(struct policy_parm * param, void * data);

static int f_policy_set_param_pv(struct base_config * data, const char * name, const char * value);

void f_free_port_instance(struct port_instance * entry);
