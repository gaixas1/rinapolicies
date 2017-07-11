//rmt_eqta.c
#define RINA_PREFIX "rmt_eqta-plugin"
#define RINA_QTA_MUX_ps_NAME "rmt_eqta-ps"
#include rmt_eqta.h

/* Main functions */

static struct ps_base * eqta_create(struct rina_component * component){
	struct rmt * rmt = rmt_from_component(component);
	struct rmt_ps * ps = rkzalloc(sizeof(*ps), GFP_ATOMIC);
	struct eqta_config * data;
	struct rmt_config * rmt_cfg;

	if (!ps) {
		return NULL;
	}
	
	struct eqta_config * base_conf = kzalloc(sizeof(struct eqta_config), GFP_ATOMIC);
	if (!base_conf) {
		LOG_ERR("Could not create config queue");
		return NULL;
	}
	INIT_LIST_HEAD(&base_conf->q_entry_L);
	base_conf->q_entry_buffer_size = 0;
		
	INIT_LIST_HEAD(&base_conf->Qos2ps_L);
	INIT_LIST_HEAD(&base_conf->eqta_instance_L);

	ps->base.set_policy_set_param = eqta_p_set_param;
	ps->dm = rmt;
	ps->priv = base_conf;

	rmt_cfg = rmt_config_get(rmt);
	if (rmt_cfg) {
		policy_for_each(rmt_cfg->policy_set, data, eqta_config_apply);
	} else {
		base_conf->headers_weight = 0;
		base_conf->num_ps = 0;
		base_conf->levels_cherish = 1;
		base_conf->levels_urgency = 1;
		base_conf->max_count = 100;
		base_conf->cherish_thresholds = kzalloc(sizeof(uint_t), GFP_ATOMIC);
		if(base_conf->cherish_thresholds =! NULL) {
			base_conf->cherish_thresholds[0] = 100;
		}
		base_conf->ps = NULL;
		LOG_WARN("Using default config (best-effort)");
	}
	
	if(!base_conf->cherish_thresholds || ( base_conf->num_ps > 0 && !base_conf->ps)) {
		LOG_ERR("Could not configure queues");
		if(base_conf->cherish_thresholds) {
			rkfree(base_conf->cherish_thresholds);
		}
		if(base_conf->ps) {
			rkfree(base_conf->ps);
		}
		rkfree(base_conf);
		return NULL;
	}

	ps->rmt_q_create_policy = eqta_q_create_p;
	ps->rmt_q_destroy_policy = eqta_q_destroy_p;
	ps->rmt_enqueue_policy = eqta_enqueue_p;
	ps->rmt_dequeue_policy = eqta_dequeue_p;

	LOG_INFO("Loaded QTA MUX policy set and its configuration");

	return &ps->base;
}

static void eqta_destroy(struct ps_base * bps) {
	struct rmt_ps *ps = container_of(bps, struct rmt_ps, base);
	if (!bps || !ps || !ps->priv) {
		LOG_ERR("Error on rmt policy destroy. Some modules not set.");
		return;
	}
	
	struct eqta_config * base_conf = ps->priv;
	
	// Delete all remaining port instances
	while(!empty_list(&base_conf->eqta_instance_L)) {
		struct eqta_instance * entry;
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
	
	//Remove qos 2 PS mapping
	while(!empty_list(&base_conf->Qos2ps_L)) {
		struct qos2ps_t * entry;
		entry = list_first_entry(&base_conf->Qos2ps_L, struct qos2ps_t, L);
		list_del(&entry->L);
		rkfree(entry);
	}
	
	// Delete base structures
	if(base_conf->cherish_thresholds) {
		rkfree(base_conf->cherish_thresholds);
	}
	if(base_conf->ps) {
		rkfree(base_conf->ps);
	}
	rkfree(base_conf);
}


int eqta_enqueue_p(struct rmt_ps *ps, struct rmt_n1_port * P, struct pdu *pdu) {
	if (!ps || !ps->priv || !P || !pdu) {
		LOG_ERR("Wrong input parameters for rmt_enqueu_scheduling_policy_tx");
		return RMT_ps_ENQ_ERR;
	}
	
	//Get QoS_id
	const struct pci * pci = pdu_pci_get_ro(pdu);
	qos_id_t qos_id = pci_qos_id(pci);
	
	//Policy global config
	struct eqta_config * qta_ps = ps->priv;
	
	//Search PS_id for the qos_id, default = 0
	int_t PS_id = search_PS_id(&qta_ps->Qos2ps_L, qos_id);
	
	//Search for Port instance
	struct eqta_instance * eqta_e = search_eqta_instance(&qta_ps->eqta_instance_L, P);
	if(!eqta_e) {
		LOG_ERR("Unknown rmt_port for rmt_enqueu_scheduling_policy_tx, dropping PDU");
		pdu_destroy(pdu);
		return RMT_ps_ENQ_ERR;
	}
	
	if(PS_id < 0) {
		//To MUX
		if(eqta_e->mux_count >=  qta_ps->cherish_thresholds[qta_ps->levels_cherish-1]) {
			LOG_INFO("Length exceeded for Mux, dropping PDU");
			pdu_destroy(pdu);
			///Record dropped stats??
			return RMT_PS_ENQ_DROP;
		}
	} else {
		//To PS
		if(eqta_e->ps[PS_id].count >=  qta_ps->ps[PS_id].max_count) {
			LOG_INFO("Length exceeded for PS id %u, dropping PDU", PS_id);
			pdu_destroy(pdu);
			///Record dropped stats??
			return RMT_PS_ENQ_DROP;
		}
	}	
	
	///Check max occupation??
	if(qset->occupation >= qta_ps->max_count) {
		LOG_INFO("Length exceeded for port occupation, dropping PDU");
		pdu_destroy(pdu);
		///Record dropped stats??
		return RMT_PS_ENQ_DROP;
	}
	
	struct q_entry * e = get_q_entry(qta_ps);
	if(!e) {
		LOG_ERR("Cannot allocate buffer, dropping PDU");
		pdu_destroy(pdu);
		return RMT_PS_ENQ_DROP;
	}
	e->data = pdu;
	e->weight = (uint_t) pdu_len(pdu);
	INIT_LIST_HEAD(&e->L);
	
	if(PS_id < 0) {
		//Insert PDU into MUX queue
		list_add_tail(&e->L, &eqta_e->Qs[qta_ps->levels_urgency-1]);
		eqta_e->mux_count++;
	} else {
		//Insert PDU into PS queue
		list_add_tail(&e->L, &eqta_e->ps[PS_id].Q);
		PS_i->count++;
	}
		

	LOG_DBG("PDU enqueued");
	return RMT_ps_ENQ_SCHED;
}

struct pdu * eqta_dequeue_p(struct rmt_ps * ps, struct rmt_n1_port * P) {
	if (!ps || !P) {
		LOG_ERR("Wrong input parameters for rmt_enqueu_scheduling_policy_rx");
		return NULL;
	}
	
	//Policy global config
	struct eqta_config * qta_ps = ps->priv;
	
	//Search for Port instance
	struct eqta_instance * eqta_e = search_eqta_instance(&qta_ps->eqta_instance_L, P);
	if(!eqta_e) {
		LOG_ERR("Unknown rmt_port for rmt_enqueu_scheduling_policy_rx, dropping PDU");
		pdu_destroy(pdu);
		return NULL;
	}
	
	uint_t num_ps = qta_ps->num_ps;
	uint_t headers_weight = qta_ps->headers_weight;
	
	//Compute ticks from last call
	struct timespec t1;
	getnstimeofday (&t1);
	uint_t T = 0; // temp
	if(timespec_compare(&eqta_e->lastT, &t1) < 0) {
		struct timespec td = timespec_sub(tl, eqta_e->lastT);
		if(td.tv_sec < 2) { // MAX 2s
			T = td.tv_sec *1000000 + td.tv_nsec/1000;
		} else {
			T = 2000000;
		}
		eqta_e->lastT = t1;
	}
	struct q_entry * entry = NULL;
	
	//For each PS
	// - Give credits for T ticks
	// - Move all PDUs within credits
	// - Check credits <= MAX
	for(uint_t i = 0; i < num_ps; i++) {
		struct ps_config_t * ps_conf = qta_ps->ps + i;
		struct ps_data_t * ps_i = eqta_e->ps + i;
		ps_i->credits += T * ps_conf->gain4tick;
		
		if(ps_conf->next_module < 0) {
			//To MUX
			uint_t cherish_max_count = qta_ps->cherish_thresholds[ps_conf->cherish_level];
			uint_t mux_urgency = ps_conf->urgency_level;
			while(!list_empty(&ps_i->Q) && ps_i->credits > 0) {
				entry = list_first_entry(&ps_i->Q, struct q_entry, L);
				list_del(&entry->L);
				ps_i->credits -= (entry->weight + headers_weight);
				ps_i->count--;
				
				if(eqta_e->mux_count >= cherish_max_count) {
					LOG_INFO("Length exceeded for MUX queue for cherish %u, dropping PDU", ps_conf->cherish_level);
					pdu_destroy(entry->pdu);
					free_q_entry(base_conf, entry);
					qset->occupation--;
				} else {
					INIT_LIST_HEAD(&entry->L);
					list_add_tail(&entry->L, eqta_e->Qs+mux_urgency);
					eqta_e->mux_count++;
				}
			}
		} else {
			//To another PS
			struct ps_data_t * dst_PS_i = eqta_e->ps + ps_conf->next_module;
			uint_t dst_max_count = qta_ps->ps[ps_conf->next_module].max_count;
				
			while(!list_empty(&ps_i->Q) && ps_i->credits > 0) {
				entry = list_first_entry(&ps_i->Q, struct q_entry, L);
				list_del(&entry->L);
				ps_i->credits -= (entry->weight + headers_weight);
				ps_i->count--;
				
				if(dst_PS_i->count >= dst_max_count) {
					LOG_INFO("Length exceeded for dst PS (id %u), dropping PDU", ps_conf->next_module);
					pdu_destroy(entry->pdu);
					free_q_entry(base_conf, entry);
					qset->occupation--;
				} else {
					INIT_LIST_HEAD(&entry->L);
					list_add_tail(&entry->L, &dst_PS_i->Q);
					dst_PS_i->count++;
				}
			}
		}
	}
	
	//Get next from MUX
	uint_t levels_urgency = base_conf->levels_urgency;
	for(uint_t i = 0; i < levels_urgency; i++) {
		if(!list_empty(eqta_e->Qs+i)) {
			entry = list_first_entry(eqta_e->Qs+i, struct q_entry, L);
			list_del(&entry->L);
			PDU * pdu = entry->pdu;
			free_q_entry(base_conf, entry);
			return pdu;
		}
	}
	
	return NULL;
}

void * eqta_q_create_p(struct rmt_ps *ps, struct rmt_n1_port * P) {
		
	if (!ps || !ps->priv || !P) {
		LOG_ERR("Wrong input parameters for rmt_create_p_policy");
		return -1;
	}
	//Policy global config
	struct eqta_config * qta_ps = ps->priv;
	
	//Port instance
	struct eqta_instance * eqta_e = search_eqta_instance(&qta_ps->eqta_instance_L, P);
	if(eqta_e) {
		LOG_WARN("Try to create port queues for an already set port");
		return eqta_e;
	}
	
	eqta_e = kzalloc(sizeof(struct eqta_instance), GFP_ATOMIC);
	if(!eqta_e) {
		LOG_ERR("Memory alloc problem in rmt_create_p_policy");
		return -1;
	}
	
	eqta_e->P = P;
	eqta_e->mux_count = 0;
	getnstimeofday (&eqta_e->lastT);
	eqta_e->ps = kzalloc(sizeof(struct ps_data_t) * qta_ps->num_ps, GFP_ATOMIC);
	eqta_e->Qs = kzalloc(sizeof(struct list_head) * qta_ps->levels_urgency, GFP_ATOMIC);
	
	if(!eqta_e->ps || !eqta_e->Qs) {
		LOG_ERR("Memory alloc problem in rmt_create_p_policy");
		if(eqta_e->ps) {
			kzfree(eqta_e->ps);
		}
		if(eqta_e->Qs) {
			kzfree(eqta_e->Qs);
		}
		kzfree(eqta_e);
		return -1;
	}
	 
	for(int i = 0 ; i < qta_ps->num_ps; i++) {
		eqta_e->ps[i].count = 0;
		eqta_e->ps[i].credits = 0;
		INIT_LIST_HEAD(&eqta_e->ps[i].Q);
	}
	
	for(int i = 0 ; i < qta_ps->levels_urgency; i++) {
		INIT_LIST_HEAD(eqta_e->Qs+i);
	}
	
	entry->P->rmt_ps_queues = (void*)eqta_e;
	return eqta_e;
}

int eqta_q_destroy_p(struct rmt_ps *ps, struct rmt_n1_port * P) {
	if (!ps || !P) {
		LOG_ERR("Wrong input parameters for rmt_destroy_p_policy");
		return -1;
	}
	
	//Search for Port instance
	struct eqta_instance * entry = search_eqta_instance(&qta_ps->eqta_instance_L, P);
	if(!entry) {
		LOG_ERR("Unknown rmt_port for rmt_destroy_p_policy");
		return -1;
	}
	free_port_instance(entry);
	return 0;
}


/* Helper functions */

struct q_entry * get_q_entry(struct eqta_config * base) {
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

int free_q_entry(struct eqta_config * base, struct q_entry * entry) {
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

static int eqta_config_apply(struct policy_parm * param, void * data) {
	struct eqta_config * tmp = (struct eqta_config *) data;
	return eqta_set_param_pv(tmp, policy_param_name(param), policy_param_value(param));
}

static int eqta_p_set_param(struct ps_base * bps, const char * name, const char * value) {
	struct rmt_ps *ps = container_of(bps, struct rmt_ps, base);
	return eqta_set_param_pv((struct eqta_config *) ps->priv, name, value);
}


static int eqta_set_param_pv(struct eqta_config * data, const char * name, const char * value) {
	if (!name) {
		LOG_ERR("Null parameter name");
		return -1;
	}
	if (!value) {
		LOG_ERR("Null parameter value");
		return -1;
	}
	
	char * v_name = name;
	char * v_value = value;
	int v_id, v;
	//getValue(&v_id, &v, v_value);
	
	switch(v_name[0]) {
		case 'c':
			if(strcmp(v_name, "ch_th") == 0) {
				if(v_id < 0 || v_id >= data->levels_cherish){
					LOG_ERR("Invalid cherish value %d", v_id);
					return -1;
				}
				if(v < 1){
					LOG_ERR("Invalid cherish threshold %d for value %d", v, v_id);
					return -1;
				}
				data->cherish_thresholds[v_id] = v;
				return 0;
			}
			break;
		case 'h':
			if(strcmp(v_name, "header_weight") == 0) {
				if(v < 0){
					LOG_ERR("Invalid header weight %d", v);
					return -1;
				}
				data->headers_weight = v;
				return 0;
			}
			break;
		case 'i':
			if(strcmp(v_name, "init_buffer") == 0) {
				if(v < 0){
					LOG_ERR("Invalid buffer size %d", v);
					return -1;
				}
				struct q_entry * entry = NULL;
				for(int i = 0; i < v; i++) {
					entry = rkzalloc(sizeof(struct q_entry), GFP_ATOMIC);
					if(!entry){						
						LOG_ERR("Failure pre-allocating buffers");
						return -1;
					}
					INIT_LIST_HEAD(&entry->L);
					list_add(&entry->L, &data->q_entry_L);
					data->q_entry_buffer_size++;
				}
				return 0;
			}
			break;
		case 'l':
			if(strcmp(v_name, "levels_cherish") == 0) {
				if(v < 0){
					LOG_ERR("Invalid amount of cherish levels %d", v);
					return -1;
				}
				uint_t * t = data->cherish_thresholds;
				data->cherish_thresholds = rkzalloc(sizeof(uint_t) * v, GFP_ATOMIC);
				for(int i = 0; i < v; i++) {
					data->cherish_thresholds[i] = 1;
				}
				if(t) {
					for(int i = 0; i < v && i < data->levels_cherish; i++) {
						data->cherish_thresholds[i] = t[i];
					}
					rkfree(e);
				}
				data->levels_cherish = v;
				return 0;
			} else if(strcmp(v_name, "levels_urgency") == 0) {
				if(v < 0){
					LOG_ERR("Invalid amount of urgency levels %d", v);
					return -1;
				}
				data->levels_urgency = v;
				return 0;
			}
			break;
		case 'm':
			if(strcmp(v_name, "max_count") == 0) {
				if(v < 0){
					LOG_ERR("Invalid max queue size %d", v);
					return -1;
				}
				data->max_count = v;
				return 0;
			}
			break;
		case 'n':
			if(strcmp(v_name, "num_ps") == 0) {
				if(v < 0){
					LOG_ERR("Invalid amount of policer/shapers, %d", v);
					return -1;
				}
				struct ps_config_t * t = data->ps;
				data->ps = rkzalloc(sizeof(struct ps_config_t) * v, GFP_ATOMIC);
				if(!data->ps) {
					LOG_ERR("Failure allocating policer/shapers");
					return -1;
				}
				
				for(int i = 0; i < v; i++) {
					data->ps[i].max_count = 100;
					data->ps[i].gain_us = 100;
					data->ps[i].max_credits = 100000;
					data->ps[i].next_module = -1;
					data->ps[i].cherish_level = data->levels_cherish-1;
					data->ps[i].max_count =  data->levels_urgency-1;
				}
				
				if(data->ps) {
					for(int i = 0; i < v && i < data->num_ps; i++) {
						data->ps[i].max_count = t[i].max_count;
						data->ps[i].gain_us = t[i].gain_us;
						data->ps[i].max_credits = t[i].max_credits;
						data->ps[i].next_module = t[i].next_module;
						data->ps[i].cherish_level = t[i].cherish_level;
						data->ps[i].urgency_level = t[i].urgency_level;
					}
					rkfree(e);
				}
				
				data->num_ps = v;
				
				return 0;
			}
			break;
		case 'p':
			if(v_name[1] == 's' && v_name[2] == '_') {
				v_name += 3;
				
				if(v_id < 0 || v_id >= data->num_ps){
					LOG_ERR("Invalid ps_id %d", v_id);
					return -1;
				}
				
				
				if(strcmp(v_name, "max_count") == 0) {
					if(v < 1) {
						LOG_ERR("Invalid max P/S queue size %d", v);
						return -1;
					}
					
					data->ps[v_id].max_count = v;
					return 0;
				} else if(strcmp(v_name, "gain_us") == 0) {
					if(v < 1) {
						LOG_ERR("Invalid P/S gain per us %d", v);
						return -1;
					}
					
					data->ps[v_id].gain_us = v;
					return 0;
				} else if(strcmp(v_name, "max_credit") == 0) {
					if(v < 1) {
						LOG_ERR("Invalid max ammunt of credits at P/S %d", v);
						return -1;
					}
					
					data->ps[v_id].max_credits = v;
					return 0;
				} else if(strcmp(v_name, "next_module") == 0) {
					if(v < 0){
						v = -1;
					}
					if(v >= data->num_ps) {
						LOG_ERR("Invalid next P/S id %d", v);
					}
					
					data->ps[v_id].next_module = v;
					return 0;
				} else if(strcmp(v_name, "cherish") == 0) {
					if(v < 0 || v >= data->levels_cherish) {
						LOG_ERR("Invalid cherish level %d at P/S %d", v, v_id);
						return -1;
					}
					
					data->ps[v_id].cherish_level = v;
					return 0;
				} else if(strcmp(v_name, "urgency") == 0) {
					if(v < 0 || v >= data->levels_urgency) {
						LOG_ERR("Invalid urgency level %d at P/S %d", v, v_id);
						return -1;
					}
					
					data->ps[v_id].urgency_level = v;
					return 0;
				}
			}
			break;
		case 'q':
			if(strcmp(v_name, "qos2ps") == 0) {
				if(v_id < 0){
					LOG_ERR("Invalid qos_id %d", v_id);
					return -1;
				}
				if(v < 0){
					v = -1;
				}
				struct qos2ps_t * e = NULL,  * t;
				list_for_each_entry(t, &data->Qos2ps_L, L) {
					if (t.qos_id == v_id) {
						e = t;
					}
				}
				if(!e) {
					e = rkzalloc(sizeof(struct qos2ps_t), GFP_ATOMIC);
					if(!e) {
						LOG_ERR("Failure allocating map QoS to policer/shaper");
						return -1;
					}
					e->qos_id = v_id;
					INIT_LIST_HEAD(&e->L);
					list_add_tail(&e->L, &data->Qos2ps_L);
				}
				e->PS_id = v;
			}
			break;
	}
	return 1;
}

void free_port_instance(struct eqta_instance * entry) {
	entry->P->rmt_ps_queues = NULL;
	list_del(&entry->L);
	
	for(int i = 0; i < base_conf->num_ps; i++) {
		while(!empty_list(&entry->ps[i])) {
			struct q_entry * e;
			e = list_first_entry(&entry->ps[i], struct q_entry, L);
			list_del(&e->L);
			rkfree(e);
		}
	}
	rkfree(entry->ps);
	
	for(int i = 0; i < base_conf->levels_urgency; i++) {
		while(!empty_list(&entry->Qs[i])) {
			struct q_entry * e;
			e = list_first_entry(&entry->Qs[i], struct q_entry, L);
			list_del(&e->L);
			rkfree(e);
		}
	}
	rkfree(entry->Qs);
		
	rkfree(entry);
}


/*
	Policy init and exit
*/
static struct ps_factory qta_factory = {
		.owner   = THIS_MODULE,
		.create  = eqta_create,
		.destroy = eqta_destroy,
};

static int __init mod_init(void) {
	strcpy(qta_factory.name, RINA_QTA_MUX_ps_NAME);
	if (rmt_ps_publish(&qta_factory)) {
		LOG_ERR("Failed to publish policy set factory");
		return -1;
	}
	LOG_INFO("RMT QTA MUX policy set loaded successfully");
	return 0;
}

static void __exit mod_exit(void) {
	if (rmt_ps_unpublish(RINA_QTA_MUX_ps_NAME)) {
		LOG_ERR("Failed to unpublish policy set factory");
	} else {
		LOG_INFO("RMT QTA MUX policy set unloaded successfully");
	}
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_DESCRIPTION("RMT QTA MUX policy set");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sergio Leon <gaixas1@gmail.com>");

/*
	*_attr_show
*/
/*
RINA_SYSFS_Ops(qta_qset);
RINA_ATTRS(qta_qset, port_id, occupation);
RINA_KTYPE(qta_qset);

RINA_SYSFS_Ops(qos_queue);
RINA_ATTRS(qos_queue, queued_pdus, drop_pdus, total_pdus, urgency,
		threshold, absolute_threshold,
		drop_probability, qos_id);
RINA_KTYPE(qos_queue);
*/
