//rmt_eqta.h
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

//Configuration of ps
struct ps_config_t {
	uint_t max_count; // Max amount of PDUs admited
	uint_t gain_us; // Credits gain each us
	uint_t max_credits; // Max amount of accumulated credits
	
	uint_t next_module; // Module towards where forward PDUs. N >= 0 -> ps[N], else Mux
	uint_t cherish_level; // Cherish level of the ps (Only if next < 0)
	uint_t urgency_level; // Urgency level of the ps (Only if next < 0)
};

//Queue entry
struct q_entry {
	struct list_head L;
	
	struct pdu * data;
	uint_t weight;
};

//Data structure of ps
struct ps_data_t {
	struct list_head Q; // PS queue of q_entry (not part of listo of ps_data_t)
	uint_t count; // Amount of PDUs stored
	uint_t credits; // Amount of accumulated credits
};

//Data structure of Mux queue
struct mux_Q_t {
	uint_t count; // Amount of PDUs stored
}

//Mapping QoS id to ps index
struct qos2ps_t {
	struct list_head L;
	
	qos_id_t qos_id;
	int_t PS_id;
};

// eqta instance information
struct eqta_instance {
	struct rmt_n1_port * P;
	
	///ps related
	struct timespec lastT;	// "Time" of last call
	struct ps_data_t * ps; // ps modules, len == eqta_config.num_ps
	
	///Mux related
	uint_t mux_count; // Amount of PDUs waiting on the mux queues
	struct list_head * Qs; // Urgency queues in the mux, len == eqta_config.levels_urgency
};

// Configuration of the policy
struct eqta_config {
	uint_t headers_weight; // Extra weight of headers
	uint_t num_ps; // Number of ps in the module
	uint_t levels_cherish; // Levels of cherish
	uint_t * cherish_thresholds; // cherish_thresholds, len = levels_cherish
	uint_t levels_urgency; // Levels of urgency
	uint_t max_count; // Max ocupation on port
	struct list_head q_entry_L; // Buffer of q_entries
	uint_t q_entry_buffer_size; // Size of buffer of q_entries
	
	struct ps_config_t * ps; // Configuration of ps modules, len == num_ps
	struct list_head Qos2ps_L; // List mapping QoS_id to ps index
	
	struct list_head eqta_instance_L; // List storing port instances
};

/* Function headers */

static struct ps_base * eqta_create(struct rina_component * component);
static void eqta_destroy(struct ps_base * bps);

void * eqta_q_create_p(struct rmt_ps *ps, struct rmt_n1_port * P);
int eqta_q_destroy_p(struct rmt_ps *ps, struct rmt_n1_port * P);
int eqta_enqueue_p(struct rmt_ps *ps, struct rmt_n1_port * P, struct pdu *pdu);
struct pdu * eqta_dequeue_p(struct rmt_ps *ps, struct rmt_n1_port * P);

static int eqta_p_set_param(struct ps_base * bps, const char * name, const char * value);
static int eqta_config_apply(struct policy_parm * param, void * data);

static int eqta_set_param_pv(struct eqta_config * data, const char * name, const char * value);

struct q_entry * get_q_entry(struct eqta_config * base);
int free_q_entry(struct eqta_config * base, struct q_entry * entry);

void free_port_instance(struct eqta_instance * entry);

/* Inline functions */
__always_inline int_t search_PS_id(list_head * L, const qos_id_t qos_id) {
	int_t PS_id = -1;
    struct qos2ps_t * qos2ps_e;
	list_for_each_entry(qos2ps_e, L, L) {
		if (qos_id == qos2ps_e->qos_id) {
			PS_id = qos2ps_e->PS_id;
		}
	}
	return PS_id;
};

__always_inline struct eqta_instance * search_eqta_instance(list_head * L, rmt_n1_port * P) {
	eqta_instance *  eqta_e = NULL;
	list_for_each_entry(eqta_e, L, L) {
		if (P == eqta_e->P) {
			return P; 
		}
	}
	return NULL;
};
