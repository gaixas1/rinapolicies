 //rmt-be.c
#define RINA_PREFIX "rmt-be-plugin"
#define RINA_BE_PS_NAME "rmt-be-ps"
#include "rmt-be.h"

MODULE_DESCRIPTION("RMT BE policy set");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sergio Leon <gaixas1@gmail.com>");

/// Main functions


static struct ps_base * f_policy_create(struct rina_component * component){
	
	struct rmt * rmt_i;
	struct rmt_ps * ps_i;
	struct rmt_config * rmt_cfg;
	struct base_config * conf;
	
	rmt_i = rmt_from_component(component);
	
	ps_i = rkzalloc(sizeof(struct rmt_ps), GFP_ATOMIC);
	if (!ps_i) {
		return NULL;
	}
	
	conf = kzalloc(sizeof(struct base_config), GFP_ATOMIC);
	if (!conf) {
		LOG_ERR("Could not create config queue");
		return NULL;
	}
	
	conf->max_count = 100;
	conf->ecn_th = 50;
	INIT_LIST_HEAD(&conf->port_L);
	INIT_LIST_HEAD(&conf->buffer_L);

	ps_i->base.set_policy_set_param = f_set_policy_set_param;
	ps_i->dm = rmt_i;
	ps_i->priv = conf;

	rmt_cfg = rmt_config_get(rmt_i);
	if (rmt_cfg) {
		policy_for_each(rmt_cfg->policy_set, conf, f_policy_base_config_apply);
	} else {
		LOG_WARN("Using default config (100 buffers, ecn at 50)");
	}

	ps_i->rmt_q_create_policy = f_rmt_q_create_policy;
	ps_i->rmt_q_destroy_policy = f_rmt_q_destroy_policy;
	ps_i->rmt_enqueue_policy = f_rmt_enqueue_policy;
	ps_i->rmt_dequeue_policy = f_rmt_dequeue_policy;

	LOG_INFO("Loaded BE MUX policy set and its configuration");

	return &ps_i->base;
}

static void f_policy_destroy(struct ps_base * bps) {
	
	struct rmt_ps * ps_i;
	struct base_config * conf;
	struct port_instance * port_i;
	struct q_entry * entry_i;

	if (!bps) {
		LOG_ERR("Error on rmt policy destroy. Some modules not set.");
		return;
	}

	ps_i = container_of(bps, struct rmt_ps, base);
	if (!ps_i || !ps_i->priv) {
		LOG_ERR("Error on rmt policy destroy. Some modules not set.");
		return;
	}

	conf = ps_i->priv;
	
	// Delete all remaining port instances
	while(!list_empty(&conf->port_L)) {
		port_i = list_first_entry(&conf->port_L, struct port_instance, L);
		f_free_port_instance(port_i);
	}
	
	// Empty buffers
	while(!list_empty(&conf->buffer_L)) {
		entry_i = list_first_entry(&conf->buffer_L, struct q_entry, L);
		list_del(&entry_i->L);
		rkfree(entry_i);
	}
	
	// Delete base structure
	rkfree(conf);
}


int f_rmt_enqueue_policy(struct rmt_ps *ps_i, struct rmt_n1_port * P, struct pdu *PDU) {
	struct base_config * conf;
	struct port_instance * port_i;
	struct q_entry * entry_i;
	
	if (!ps_i || !ps_i->priv || !P || !PDU) {
		LOG_ERR("Wrong input parameters for rmt_enqueu_scheduling_policy_tx");
		return RMT_PS_ENQ_ERR;
	}
	
	//Policy global config
	conf = ps_i->priv;
	
	//Search for Port instance
	port_i = P->rmt_ps_queues;
	if(!port_i) {
		LOG_ERR("Unknown rmt_port for rmt_enqueue_scheduling_policy_tx, dropping PDU");
		pdu_destroy(PDU);
		return RMT_PS_ENQ_ERR;
	}
	
	if(port_i->count >= conf->max_count) {
		LOG_INFO("Length exceeded for queue, dropping PDU");
		pdu_destroy(PDU);
		return RMT_PS_ENQ_DROP;
	}	
	
	if(list_empty(&conf->buffer_L)) {
		entry_i = rkzalloc(sizeof(struct q_entry), GFP_ATOMIC);
	} else {
		entry_i = list_first_entry(&conf->buffer_L, struct q_entry, L);
		list_del(&entry_i->L);
	}
	
	if(!entry_i) {
		LOG_ERR("Cannot allocate buffer, dropping PDU");
		pdu_destroy(PDU);
		return RMT_PS_ENQ_DROP;
	}

	entry_i->data = PDU;
	list_add_tail(&entry_i->L, &port_i->Q);

	port_i->count++;
	
	LOG_DBG("PDU enqueued");
	return RMT_PS_ENQ_SCHED;
}

struct pdu * f_rmt_dequeue_policy(struct rmt_ps * ps_i, struct rmt_n1_port * P) {
	struct base_config * conf;
	struct port_instance * port_i;
	struct pdu * PDU;
	struct q_entry * entry_i;
    struct pci * pci;
	unsigned long pci_flags;
	
	if (!ps_i || !P) {
		LOG_ERR("Wrong input parameters for rmt_enqueu_scheduling_policy_rx");
		return NULL;
	}
	
	conf = ps_i->priv;
	
	port_i = P->rmt_ps_queues;
	if(!port_i) {
		LOG_ERR("Unknown rmt_port for rmt_dequeue_scheduling_policy_rx, dropping PDU");
		return NULL;
	}
	
	PDU = NULL;
	if(!list_empty(&port_i->Q)) {
		entry_i = list_first_entry(&port_i->Q, struct q_entry, L);
		list_del(&entry_i->L);
		PDU = entry_i->data;
		list_add(&entry_i->L, &conf->buffer_L);
		port_i->count--;
	}
	if(PDU != NULL && port_i->count > conf->ecn_th) {
		pci = pdu_pci_get_rw(PDU);	
		pci_flags = pci_flags_get(pci);
		pci_flags_set(pci, pci_flags |= PDU_FLAGS_EXPLICIT_CONGESTION);
	}
	
	return PDU;
}

void * f_rmt_q_create_policy(struct rmt_ps *ps_i, struct rmt_n1_port * P) {
	struct base_config * config;
	struct port_instance * port_i;
		
	if (!ps_i || !ps_i->priv || !P) {
		LOG_ERR("Wrong input parameters for rmt_q_create_policy");
		return NULL;
	}
	
	config = ps_i->priv;
	
	if(P->rmt_ps_queues) {
		LOG_WARN("Try to create port queues for an already set port");
		return (struct port_instance *) P->rmt_ps_queues;
	}
	
	port_i = kzalloc(sizeof(struct port_instance), GFP_ATOMIC);
	if(!port_i) {
		LOG_ERR("Memory alloc problem in rmt_q_create_policy");
		return NULL;
	}
	
	port_i->P = P;
	port_i->count = 0;
	INIT_LIST_HEAD(&port_i->L);
	INIT_LIST_HEAD(&port_i->Q);
	
	P->rmt_ps_queues = port_i;
	list_add_tail(&port_i->L, &config->port_L);
	
	return port_i;
}

int f_rmt_q_destroy_policy(struct rmt_ps *ps_i, struct rmt_n1_port * P) {
	if(!P->rmt_ps_queues) {
		LOG_ERR("Unknown rmt_port for rmt_destroy_p_policy");
		return -1;
	}
	
	f_free_port_instance((struct port_instance *) P->rmt_ps_queues);
	return 0;
}


/// Helper functions

static int f_policy_base_config_apply(struct policy_parm * param, void * data) {
	struct base_config * conf;

	conf = (struct base_config *) data;
	return f_policy_set_param_pv(conf, policy_param_name(param), policy_param_value(param));
}

static int f_set_policy_set_param(struct ps_base * bps, const char * name, const char * value) {
	struct rmt_ps * ps_i;
	
	ps_i = container_of(bps, struct rmt_ps, base);
	return f_policy_set_param_pv((struct base_config *) ps_i->priv, name, value);
}


static int f_policy_set_param_pv(struct base_config * data, const char * name, const char * value) {
	int v;
		
	if (!name) {
		LOG_ERR("Null parameter name");
		return -1;
	}
	if (!value) {
		LOG_ERR("Null parameter value");
		return -1;
	}
	
	if(strcmp(name, "max_count") == 0) {
		if(kstrtoint(value, 10, &v)) {
			LOG_ERR("Error parsing max_count value \"%s\"", value);
			return -1;
		}
		
		data->max_count = v;
		LOG_INFO("Set max_count as \"%d\"", v);
		return 0;
	}
	if(strcmp(name, "ecn_th") == 0) {
		if(kstrtoint(value, 10, &v)) {
			LOG_ERR("Error parsing ecn_th value \"%s\"", value);
			return -1;
		}
		
		data->ecn_th = v;
		LOG_INFO("Set ecn_th as \"%d\"", v);
		return 0;
	}
	LOG_ERR("Unknown attribute \"%s\"", name);
	return 1;
}

void f_free_port_instance(struct port_instance * port_i) {
	struct q_entry * entry_i;
		
	port_i->P->rmt_ps_queues = NULL;
	list_del(&port_i->L);
	
	while(!list_empty(&port_i->Q)) {
		entry_i = list_first_entry(&port_i->Q, struct q_entry, L);
		list_del(&entry_i->L);
		rkfree(entry_i);
	}
		
	rkfree(port_i);
}


/// Policy init and exit

static struct ps_factory qta_factory = {
		.owner = THIS_MODULE,
		.create = f_policy_create,
		.destroy = f_policy_destroy,
};

static int __init mod_init(void) {
	strcpy(qta_factory.name, RINA_BE_PS_NAME);
	if (rmt_ps_publish(&qta_factory)) {
		LOG_ERR("Failed to publish policy set factory");
		return -1;
	}
	LOG_INFO("rmt_i BE policy set loaded successfully");
	return 0;
}

static void __exit mod_exit(void) {
	if (rmt_ps_unpublish(RINA_BE_PS_NAME)) {
		LOG_ERR("Failed to unpublish policy set factory");
	} else {
		LOG_INFO("rmt_i QTA MUX policy set unloaded successfully");
	}
}

module_init(mod_init);
module_exit(mod_exit);
