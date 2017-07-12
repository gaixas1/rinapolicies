//rmt_be.h
#include <linux/list.h>
#include <linux/time.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/random.h>
#include "logs.h"
#include "rds/rmem.h"
#include "rmt-ps.h"
#include "policies.h"
#include "debug.h"

/* Data structures */

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

/* Function headers */

static struct ps_base * policy_create(struct rina_component * component);
static void policy_destroy(struct ps_base * bps);

void * rmt_q_create_policy(struct rmt_ps *ps, struct rmt_n1_port * P);
int rmt_q_destroy_policy(struct rmt_ps *ps, struct rmt_n1_port * P);
int rmt_enqueue_policy(struct rmt_ps *ps, struct rmt_n1_port * P, struct pdu * PDU);
struct pdu * rmt_dequeue_policy(struct rmt_ps *ps, struct rmt_n1_port * P);

static int set_policy_set_param(struct ps_base * bps, const char * name, const char * value);
static int policy_base_config_apply(struct policy_parm * param, void * data);

static int policy_set_param_pv(struct base_config * data, const char * name, const char * value);

void free_port_instance(struct port_instance * entry);
