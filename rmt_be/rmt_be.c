//rmt_be.c
#define RINA_PREFIX "rmt_be-plugin"
#define RINA_BE_PS_NAME "rmt_be-ps"
#include rmt_be.h

/* Main functions */

static struct ps_base * be_create(struct rina_component * component){
	struct rmt * rmt = rmt_from_component(component);
	struct rmt_ps * ps = rkzalloc(sizeof(*ps), GFP_ATOMIC);
	struct be_config * data;
	struct rmt_config * rmt_cfg;

	if (!ps) {
		return NULL;
	}
	
	struct be_config * base_conf = kzalloc(sizeof(struct be_config), GFP_ATOMIC);
	if (!base_conf) {
		LOG_ERR("Could not create config queue");
		return NULL;
	}
	base_conf->max_count = 100;
	INIT_LIST_HEAD(&base_conf->port_instance_L);
	INIT_LIST_HEAD(&base_conf->q_entry_L);

	ps->base.set_policy_set_param = be_p_set_param;
	ps->dm = rmt;
	ps->priv = base_conf;

	rmt_cfg = rmt_config_get(rmt);
	if (rmt_cfg) {
		policy_for_each(rmt_cfg->policy_set, data, be_config_apply);
	} else {
		LOG_WARN("Using default config (100 buffers)");
	}
	
	ps->rmt_q_create_policy = be_q_create_p;
	ps->rmt_q_destroy_policy = be_q_destroy_p;
	ps->rmt_enqueue_policy = be_enqueue_p;
	ps->rmt_dequeue_policy = be_dequeue_p;

	LOG_INFO("Loaded BE MUX policy set and its configuration");

	return &ps->base;
}

static void be_destroy(struct ps_base * bps) {
	struct rmt_ps *ps = container_of(bps, struct rmt_ps, base);
	if (!bps || !ps || !ps->priv) {
		LOG_ERR("Error on rmt policy destroy. Some modules not set.");
		return;
	}
	
	struct be_config * base_conf = ps->priv;
	
	// Delete all remaining port instances
	while(!empty_list(&base_conf->port_instance_L)) {
		struct port_instance * entry;
		entry = list_first_entry(&base_conf->q_entry_L, struct q_entry, L);
		free_port_instance(entry);
	}
	
	// Empty buffers
	while(!empty_list(&base_conf->q_entry_L)) {
		struct q_entry * entry;
		entry = list_first_entry(&base_conf->q_entry_L, struct q_entry, L);
		list_del(&entry->L);
		rkfree(entry);
	}
	
	// Delete base structure
	rkfree(base_conf);
}


int be_enqueue_p(struct rmt_ps *ps, struct rmt_n1_port * P, struct pdu *pdu) {
	if (!ps || !ps->priv || !P || !pdu) {
		LOG_ERR("Wrong input parameters for rmt_enqueu_scheduling_policy_tx");
		return RMT_ps_ENQ_ERR;
	}
	
	//Policy global config
	struct be_config * base_ps = ps->priv;
	
	//Search for Port instance
	struct port_instance * be_e = search_port_instance(&base_ps->port_instance_L, P);
	if(!be_e) {
		LOG_ERR("Unknown rmt_port for rmt_enqueu_scheduling_policy_tx, dropping PDU");
		pdu_destroy(pdu);
		return RMT_ps_ENQ_ERR;
	}
	
	if(be_e->count >=  base_ps->max_count) {
		LOG_INFO("Length exceeded for queue, dropping PDU");
		pdu_destroy(pdu);
		return RMT_PS_ENQ_DROP;
	}	
	
	struct q_entry * e = get_q_entry(base_ps);
	if(!e) {
		LOG_ERR("Cannot allocate buffer, dropping PDU");
		pdu_destroy(pdu);
		return RMT_PS_ENQ_DROP;
	}
	e->data = pdu;
	INIT_LIST_HEAD(&e->L);
	
	list_add_tail(&e->L, &be_e->Q);
	be_e->count++;
	
	LOG_DBG("PDU enqueued");
	return RMT_ps_ENQ_SCHED;
}

struct pdu * be_dequeue_p(struct rmt_ps * ps, struct rmt_n1_port * P) {
	if (!ps || !P) {
		LOG_ERR("Wrong input parameters for rmt_enqueu_scheduling_policy_rx");
		return NULL;
	}
	
	//Policy global config
	struct be_config * base_ps = ps->priv;
	
	//Search for Port instance
	struct port_instance * be_e = search_port_instance(&base_ps->port_instance_L, P);
	if(!be_e) {
		LOG_ERR("Unknown rmt_port for rmt_enqueu_scheduling_policy_rx, dropping PDU");
		pdu_destroy(pdu);
		return NULL;
	}
	
	if(!list_empty(&be_e->Q)) {
		entry = list_first_entry(&be_e->Q, struct q_entry, L);
		list_del(&entry->L);
		PDU * pdu = entry->pdu;
		free_q_entry(base_ps, entry);
		return pdu;
	}
	
	return NULL;
}

void * be_q_create_p(struct rmt_ps *ps, struct rmt_n1_port * P) {
		
	if (!ps || !ps->priv || !P) {
		LOG_ERR("Wrong input parameters for rmt_create_p_policy");
		return -1;
	}
	
	//Policy global config
	struct be_config * base_ps = ps->priv;
	
	//Port instance
	struct port_instance * be_e = search_port_instance(&base_ps->port_instance_L, P);
	if(be_e) {
		LOG_WARN("Try to create port queues for an already set port");
		return be_e;
	}
	
	be_e = kzalloc(sizeof(struct port_instance), GFP_ATOMIC);
	if(!be_e) {
		LOG_ERR("Memory alloc problem in rmt_create_p_policy");
		return -1;
	}
	
	be_e->P = P;
	be_e->count = 0;
	INIT_LIST_HEAD(&be_e->Q);
	
	P->rmt_ps_queues = (void*)be_e;
	
	return be_e;
}

int be_q_destroy_p(struct rmt_ps *ps, struct rmt_n1_port * P) {
	if (!ps || !P) {
		LOG_ERR("Wrong input parameters for rmt_destroy_p_policy");
		return -1;
	}
	
	//Search for Port instance
	struct port_instance * entry = search_port_instance(&base_ps->port_instance_L, P);
	if(!entry) {
		LOG_ERR("Unknown rmt_port for rmt_destroy_p_policy");
		return -1;
	}
	free_port_instance(entry);
	return 0;
}


/* Helper functions */

struct q_entry * get_q_entry(struct be_config * base) {
	if(!base){
		LOG_ERR("Failed to get queue entry, policy not initialized");
		return NULL;
	}
	
	struct q_entry * entry = NULL;
	
	if(list_empty(&base->q_entry_L)) {
		entry = rkzalloc(sizeof(struct q_entry), GFP_ATOMIC);
	} else {
		entry = list_first_entry(&base->q_entry_L, struct q_entry, L);
		list_del(&entry->L);
	}
	
	if (!entry) {
		LOG_ERR("Failed to allocate queue entry.");
		return NULL;
	}
	
	INIT_LIST_HEAD(&entry->L);
	entry->data = NULL;
	entry->w = 0;
	
	return entry;
}

int free_q_entry(struct be_config * base, struct q_entry * entry) {
	if(!base){
		rkfree(pos);
		LOG_ERR("Error while queue entry, policy not initialized");
		return -1;
	}
	
	///Check max buffer lenght and drop free if over it??
	
	INIT_LIST_HEAD(&entry->L);
	list_add(&entry->L, &base->q_entry_L);
	return 0;
}

static int be_config_apply(struct policy_parm * param, void * data) {
	struct be_config * tmp = (struct be_config *) data;
	return be_set_param_pv(tmp, policy_param_name(param), policy_param_value(param));
}

static int be_p_set_param(struct ps_base * bps, const char * name, const char * value) {
	struct rmt_ps *ps = container_of(bps, struct rmt_ps, base);
	return be_set_param_pv((struct be_config *) ps->priv, name, value);
}


static int be_set_param_pv(struct be_config * data, const char * name, const char * value) {
	if (!name) {
		LOG_ERR("Null parameter name");
		return -1;
	}
	if (!value) {
		LOG_ERR("Null parameter value");
		return -1;
	}
	
	
	if(strcmp(v_name, "max_count") == 0) {
		int v = 0;
		if(kstrtoint(value, 10, &v)) {
			LOG_ERR("Error parsing max_count value \"%s\"", value);
			return -1;
		}
		
		data->max_count = v;
		return 0;
	}
	LOG_ERR("Unknown attribute \"%s\"", name);
	return 1;
}

void free_port_instance(struct port_instance * entry) {
	entry->P->rmt_ps_queues = NULL;
	list_del(&entry->L);
	
	while(!empty_list(&entry->Q)) {
		struct q_entry * e;
		e = list_first_entry(&entry->Q, struct q_entry, L);
		list_del(&e->L);
		rkfree(e);
	}
		
	rkfree(entry);
}


/*
	Policy init and exit
*/
static struct ps_factory qta_factory = {
		.owner   = THIS_MODULE,
		.create  = be_create,
		.destroy = be_destroy,
};

static int __init mod_init(void) {
	strcpy(qta_factory.name, RINA_BE_PS_NAME);
	if (rmt_ps_publish(&qta_factory)) {
		LOG_ERR("Failed to publish policy set factory");
		return -1;
	}
	LOG_INFO("RMT BE policy set loaded successfully");
	return 0;
}

static void __exit mod_exit(void) {
	if (rmt_ps_unpublish(RINA_BE_PS_NAME)) {
		LOG_ERR("Failed to unpublish policy set factory");
	} else {
		LOG_INFO("RMT QTA MUX policy set unloaded successfully");
	}
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_DESCRIPTION("RMT BE policy set");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sergio Leon <gaixas1@gmail.com>");