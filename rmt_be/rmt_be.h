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
	struct rmt_n1_port * P;
	uint_t count;
	struct list_head Q;
};

// Configuration of the policy
struct be_config {
	uint_t max_count;
	struct list_head port_instance_L;
	struct list_head q_entry_L;
};

/* Function headers */

static struct ps_base * be_create(struct rina_component * component);
static void be_destroy(struct ps_base * bps);

void * be_q_create_p(struct rmt_ps *ps, struct rmt_n1_port * P);
int be_q_destroy_p(struct rmt_ps *ps, struct rmt_n1_port * P);
int be_enqueue_p(struct rmt_ps *ps, struct rmt_n1_port * P, struct pdu *pdu);
struct pdu * be_dequeue_p(struct rmt_ps *ps, struct rmt_n1_port * P);

static int be_p_set_param(struct ps_base * bps, const char * name, const char * value);
static int be_config_apply(struct policy_parm * param, void * data);

static int be_set_param_pv(struct be_config * data, const char * name, const char * value);

struct q_entry * get_q_entry(struct be_config * base);
int free_q_entry(struct be_config * base, struct q_entry * entry);

void free_port_instance(struct port_instance * entry);

__always_inline struct port_instance * search_port_instance(list_head * L, rmt_n1_port * P) {
	port_instance *  be_e = NULL;
	list_for_each_entry(be_e, L, L) {
		if (P == be_e->P) {
			return P; 
		}
	}
	return NULL;
};
