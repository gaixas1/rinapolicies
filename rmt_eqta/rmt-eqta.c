//rmt-eqta.c
#define RINA_PREFIX "rmt-eqta-plugin"
#define RINA_QTA_MUX_ps_NAME "rmt-eqta-ps"
#include "rmt-eqta.h"


MODULE_DESCRIPTION("RMT QTA MUX policy set");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sergio Leon <gaixas1@gmail.com>");

/* Main functions */

static struct ps_base * f_policy_create(struct rina_component * component){
	struct rmt * rmt; 
	struct rmt_ps * ps;
	struct rmt_config * rmt_cfg;
	base_config * conf;

	rmt = rmt_from_component(component);
	ps = rkzalloc(sizeof(*ps), GFP_ATOMIC);
	if (!ps) {
		return NULL;
	}
	
	conf = kzalloc(sizeof(base_config), GFP_ATOMIC);
	if (!conf) {
		LOG_ERR("Could not create conf queue");
		return NULL;
	}
		
	INIT_LIST_HEAD(&conf->buffer);
	INIT_LIST_HEAD(&conf->qos2modules);
	INIT_LIST_HEAD(&conf->port_instances);
	
	conf->state = 0;
	conf->headers_weight = 0;
	conf->num_policers = 0;
	conf->levels_urgency = 1;
	conf->bytecost = 1;
	conf->max_count = 100;
	conf->global_max_count = 100;
	conf->buffer_size = 0;
	conf->policers = NULL;

	ps->base.set_policy_set_param = f_set_policy_set_param;
	ps->dm = rmt;
	ps->priv = conf;

	rmt_cfg = rmt_config_get(rmt);
	if (rmt_cfg) {
		policy_for_each(rmt_cfg->policy_set, conf, f_policy_base_config_apply);
	} else {
		LOG_WARN("Using default conf (best-effort)");
	}
	
	conf->state |= 1;
	
	if(conf->num_policers > 0 && !conf->policers) {
		rkfree(conf);
		return NULL;
	}

	ps->rmt_q_create_policy = f_rmt_q_create_policy;
	ps->rmt_q_destroy_policy = f_rmt_q_destroy_policy;
	ps->rmt_enqueue_policy = f_rmt_enqueue_policy;
	ps->rmt_dequeue_policy = f_rmt_dequeue_policy;

	LOG_INFO("Loaded QTA MUX policy set and its confuration");

	return &ps->base;
}

static void f_policy_destroy(struct ps_base * bps) {
	struct rmt_ps *ps;
	base_config * conf;
	port_instance * port_i;
	q_entry * entry_i;
	qos2module * qos2module_i;
	
	ps = container_of(bps, struct rmt_ps, base);
	if (!bps || !ps || !ps->priv) {
		LOG_ERR("Error on rmt policy destroy. Some modules not set.");
		return;
	}
	
	conf = (base_config *) ps->priv;
	
	// Delete all remaining port instances
	while(!list_empty(&conf->port_instances)) {
		port_i = list_first_entry(&conf->port_instances, port_instance, L);
		f_free_port_instance(conf, port_i);
	}
	
	// Empty buffers
	while(!list_empty(&conf->buffer)) {
		entry_i = list_first_entry(&conf->buffer, q_entry, L);
		list_del(&entry_i->L);
		rkfree(entry_i);
		conf->buffer_size--;
	}
	
	//Remove qos 2 module mapping
	while(!list_empty(&conf->qos2modules)) {
		qos2module_i = list_first_entry(&conf->qos2modules, qos2module, L);
		list_del(&qos2module_i->L);
		rkfree(qos2module_i);
	}
	
	// Delete base structures
	if(conf->policers) {
		rkfree(conf->policers);
	}
	rkfree(conf);
}

int f_rmt_enqueue_policy(struct rmt_ps *ps, port_p P, pdu_p pdu_i) {
	const struct pci * pci_i;
	qos_id_t qos_id;
	base_config * conf;
	u8 next_module, def_urgency;
	u16 def_cherish_th;
	qos2module * qos2module_i;
	port_instance * port_i;
	policer_d * psh_d;
	policer_c * psh_c;
	q_entry * entry_i;
	
	if (!ps || !ps->priv || !P || !pdu_i) {
		LOG_ERR("Wrong input parameters for rmt_enqueu_scheduling_policy_tx");
		return RMT_PS_ENQ_ERR;
	}
	
	//Get QoS_id
	pci_i = pdu_pci_get_ro(pdu_i);
	qos_id = pci_qos_id(pci_i);
	
	//Policy global conf
	conf = ps->priv;
	
	//Search for Port instance
	port_i = P->rmt_ps_queues;
	if(!port_i) {
		LOG_ERR("Unknown rmt_port for rmt_enqueue_scheduling_policy_tx, dropping PDU");
		pdu_destroy(pdu_i);
		return RMT_PS_ENQ_ERR;
	}
	
	if(port_i->count >= conf->global_max_count) {
		LOG_INFO("Length exceeded for Port, dropping PDU");
		pdu_destroy(pdu_i);
		return RMT_PS_ENQ_DROP;
	}
	
	//Search next module id for the qos_id, default = 0 (MUX)
	next_module = 0;
	def_cherish_th = 100;
	def_urgency = conf->levels_urgency-1;
	list_for_each_entry(qos2module_i, &conf->qos2modules, L) {
		if (qos_id == qos2module_i->qos_id) {
			next_module = qos2module_i->next_module;
			def_cherish_th = qos2module_i->def_cherish_th;
			def_urgency = qos2module_i->def_urgency;
		}
	}
	
	if(next_module == 0 || next_module > conf->num_policers) {
		//To MUX
		next_module = 0;
		if(port_i->mux_count >= def_cherish_th) {
			LOG_INFO("Length exceeded for Mux, dropping PDU");
			pdu_destroy(pdu_i);
			return RMT_PS_ENQ_DROP;
		}
	} else {
		//To PS
		psh_c = conf->policers + next_module - 1;
		psh_d = port_i->policers + next_module - 1;
		if(psh_d->count >=  psh_c->max_count) {
			LOG_INFO("Length exceeded for policer/shaper id %u, dropping PDU", next_module);
			pdu_destroy(pdu_i);
			return RMT_PS_ENQ_DROP;
		}
	}	
	
	if(list_empty(&conf->buffer)) {
		entry_i = rkzalloc(sizeof(q_entry), GFP_ATOMIC);
	} else {
		entry_i = list_first_entry(&conf->buffer, q_entry, L);
		list_del(&entry_i->L);
		conf->buffer_size--;
	}
	
	if(!entry_i) {
		LOG_ERR("Cannot allocate buffer, dropping PDU");
		pdu_destroy(pdu_i);
		return RMT_PS_ENQ_DROP;
	}
	
	entry_i->data = pdu_i;
	entry_i->cost = (u64) pdu_len(pdu_i) + conf->headers_weight;
	entry_i->cost *= conf->bytecost;
	
	if(next_module == 0) {
		//Insert PDU into MUX queue
		list_add_tail(&entry_i->L, &port_i->Qs[def_urgency]);
		port_i->mux_count++;
	} else {
		//Insert PDU into PS queue
		list_add_tail(&entry_i->L, &psh_d->Q);
		psh_d->count++;
	}
	
	port_i->count++;

	LOG_DBG("PDU enqueued");
	return RMT_PS_ENQ_SCHED;
}

pdu_p f_rmt_dequeue_policy(struct rmt_ps * ps, port_p P) {
	base_config * conf;
	port_instance * port_i;
	q_entry * entry_i;
	u8 num_policers, headers_weight, i, dst_max_count, mux_urgency;
	struct timespec t1, td;
	int T;
	policer_c * psh_c, * psh_n_c;
	policer_d * psh_d, * psh_n_d;
	pdu_p pdu_i;
    struct pci * pci;
	unsigned long pci_flags;
	
	if (!ps || !P) {
		LOG_ERR("Wrong input parameters for rmt_enqueu_scheduling_policy_rx");
		return NULL;
	}
	
	//Policy global conf
	conf = ps->priv;
	
	port_i = P->rmt_ps_queues;
	if(!port_i) {
		LOG_ERR("Unknown rmt_port for rmt_enqueue_scheduling_policy_rx, dropping PDU");
		return NULL;
	}
	
	num_policers = conf->num_policers;
	headers_weight = conf->headers_weight;
	
	//Compute ticks from last call
	getnstimeofday (&t1);
	T = 0;
	if(timespec_compare(&port_i->lastT, &t1) < 0) {
		td = timespec_sub(t1, port_i->lastT);
		if(td.tv_sec < 2) { // MAX 2s
			T = td.tv_sec *1000000 + td.tv_nsec/1000;
		} else {
			T = 2000000;
		}
		port_i->lastT = t1;
	}
	
	
	for(i = 0; i < num_policers; i++) {
		psh_c = conf->policers + i;
		psh_d = port_i->policers + i;
		psh_d->credits += T * psh_c->gain_us;
		
		if(psh_c->next_module == 0) {
			//To MUX
			dst_max_count = psh_c->cherish_th;
			mux_urgency = psh_c->urgency_level;
			
			while(!list_empty(&psh_d->Q) && psh_d->credits > 0) {
				entry_i = list_first_entry(&psh_d->Q, q_entry, L);
				list_del(&entry_i->L);
				psh_d->credits -= entry_i->cost;
				psh_d->count--;
				if(port_i->mux_count >= psh_c->cherish_th) {
					LOG_INFO("Length exceeded for MUX queue for cherish_th %u, dropping PDU", psh_c->cherish_th);
					pdu_destroy(entry_i->data);
					list_add(&entry_i->L, &conf->buffer);
					conf->buffer_size++;
					port_i->count--;
				} else {
					list_add_tail(&entry_i->L, port_i->Qs+mux_urgency);
					port_i->mux_count++;
					if(port_i->mux_count > psh_c->ecn_th) {
						pci = pdu_pci_get_rw(entry_i->data);	
						pci_flags = pci_flags_get(pci);
						pci_flags_set(pci, pci_flags |= PDU_FLAGS_EXPLICIT_CONGESTION);
					}
				}
			}
		} else {
			//To another PS
			psh_n_c = conf->policers + psh_c->next_module - 1;
			psh_n_d = port_i->policers + psh_c->next_module - 1;
			dst_max_count = psh_n_c->max_count;
				
			while(!list_empty(&psh_d->Q) && psh_d->credits > 0) {
				entry_i = list_first_entry(&psh_d->Q, q_entry, L);
				list_del(&entry_i->L);
				psh_d->credits -= entry_i->cost;
				psh_d->count--;
				if(psh_n_d->count >= dst_max_count) {
					LOG_INFO("Length exceeded for dst PS (id %u), dropping PDU", psh_c->next_module);
					pdu_destroy(entry_i->data);
					list_add(&entry_i->L, &conf->buffer);
					conf->buffer_size++;
					port_i->count--;
				} else {
					list_add_tail(&entry_i->L, &psh_n_d->Q);
					psh_n_d->count++;
				}
			}
		}
	}
	
	//Get next from MUX
	mux_urgency = conf->levels_urgency;
	for(i = 0; i < mux_urgency; i++) {
		if(!list_empty(port_i->Qs+i)) {
			entry_i = list_first_entry(port_i->Qs+i, q_entry, L);
			list_del(&entry_i->L);
			pdu_i = entry_i->data;
			list_add(&entry_i->L, &conf->buffer);
			conf->buffer_size++;
			port_i->count--;
			port_i->mux_count--;
			return pdu_i;
		}
	}
	
	return NULL;
}

void * f_rmt_q_create_policy(struct rmt_ps *ps, port_p P) {
	base_config * conf;
	port_instance * port_i;
	u8 i;
	
	if (!ps || !ps->priv || !P) {
		LOG_ERR("Wrong input parameters for rmt_create_p_policy");
		return NULL;
	}
	//Policy global conf
	conf = ps->priv;
	
	//Port instance
	if(P->rmt_ps_queues) {
		LOG_WARN("Try to create port queues for an already set port");
		return (port_instance *) P->rmt_ps_queues;
	}
	
	port_i = kzalloc(sizeof(port_instance), GFP_ATOMIC);
	if(!port_i) {
		LOG_ERR("Memory alloc problem in rmt_create_p_policy");
		return NULL;
	}
	
	port_i->P = P;
	port_i->mux_count = 0;
	port_i->count = 0;
	getnstimeofday (&port_i->lastT);
	port_i->policers = kzalloc(sizeof(policer_d) * conf->num_policers, GFP_ATOMIC);
	port_i->Qs = kzalloc(sizeof(list_h) * conf->levels_urgency, GFP_ATOMIC);
	
	if(!port_i->policers || !port_i->Qs) {
		LOG_ERR("Memory alloc problem in rmt_create_p_policy");
		if(port_i->policers) {
			kzfree(port_i->policers);
		}
		if(port_i->Qs) {
			kzfree(port_i->Qs);
		}
		kzfree(port_i);
		return NULL;
	}
	 
	for(i = 0 ; i < conf->num_policers; i++) {
		port_i->policers[i].count = 0;
		port_i->policers[i].credits = 0;
		INIT_LIST_HEAD(&port_i->policers[i].Q);
	}
	
	for(i = 0 ; i < conf->levels_urgency; i++) {
		INIT_LIST_HEAD(port_i->Qs+i);
	}
	
	P->rmt_ps_queues = (void*)port_i;
	return port_i;
}

int f_rmt_q_destroy_policy(struct rmt_ps *ps, port_p P) {
	if (!ps || !ps->priv || !P) {
		LOG_ERR("Wrong input parameters for rmt_destroy_p_policy");
		return -1;
	}
	
	if(!P->rmt_ps_queues) {
		LOG_ERR("Unknown rmt_port for rmt_destroy_p_policy");
		return -1;
	}
	f_free_port_instance((base_config*) ps->priv, (port_instance *) P->rmt_ps_queues);
	return 0;
}


/* Helper functions */

static int f_policy_base_config_apply(struct policy_parm * param, void * data) {
	return f_policy_set_param_pv((base_config *) data, policy_param_name(param), policy_param_value(param));
}

static int f_set_policy_set_param(struct ps_base * bps, const char * name, const char * value) {
	struct rmt_ps *ps;
	ps = container_of(bps, struct rmt_ps, base);
	return f_policy_set_param_pv((base_config *) ps->priv, name, value);
}


static int f_policy_set_param_pv(base_config * conf, const char * name, const char * value) {
	
	char * v_name = (char *) name;
	char * v_value = (char *) value;
	char * p_ch;
	u8 sub_id;
	u8 v8;
	u16 v16;
	u32 v32;
	u64 v64;
	q_entry * entry_i;
	policer_c * policer_i;
	qos2module * qos2module_i, * qos2module_t;
	
	if (!name) {
		LOG_ERR("Null parameter name");
		return -1;
	}
	if (!value) {
		LOG_ERR("Null parameter value");
		return -1;
	}
	
	p_ch = strchr(v_value, '.');
	sub_id = -1;
	if(p_ch) {
		*p_ch = '\0';
		if(kstrtou8(v_value, 10, &sub_id) ||  kstrtou64(p_ch+1, 10, &v64) ) {
			*p_ch = '.';
			LOG_ERR("Error while parsing parameter %s with value %s", name, value);
			return -1;
		}
		*p_ch = '.';
	} else if(kstrtou64(value, 10, &v64) ) {
		LOG_ERR("Error while parsing parameter %s with value %s", name, value);
		return -1;
	}
	v32 = (u32) v64;
	v16 = (u16) v64;
	v8 = (u8) v64;
		
	switch(v_name[0]) {
		case 'b':
			if(strcmp(v_name, "bytecost") == 0) {
				conf->bytecost = v8;
				return 0;
			}
			break;
		case 'h':
			if(strcmp(v_name, "header_weight") == 0) {
				conf->headers_weight = v8;
				return 0;
			}
			break;
		case 'i':
			if(strcmp(v_name, "init_buffer") == 0) {
				while(v16 > 0) {
					entry_i = rkzalloc(sizeof(q_entry), GFP_ATOMIC);
					if(!entry_i){						
						LOG_ERR("Failure pre-allocating buffers");
					} else {
						INIT_LIST_HEAD(&entry_i->L);
						list_add(&entry_i->L, &conf->buffer);
						conf->buffer_size++;
					}
					v16--;
				}
				return 0;
			}
			break;
		case 'l':
			if(strcmp(v_name, "levels_urgency") == 0) {
				if(conf->state & 1) {
					LOG_ERR("Cannot re-confure the number of urgency queues after start-up");
					return -1;
				}
				if(v8 == 0){
					LOG_ERR("Required at least one urgency queue");
					return -1;
				}
				conf->levels_urgency = v8;
				return 0;
			}
			break;
		case 'm':
			if(strcmp(v_name, "max_mux_count") == 0) {
				conf->max_count = v16;
				return 0;
			} else if(strcmp(v_name, "max_global_count") == 0) {
				conf->global_max_count = v16;
				return 0;
			}
			break;
		case 'n':
			if(strcmp(v_name, "num_policers") == 0) {
				if(conf->state & 2) {
					LOG_ERR("Cannot re-confure the number of policers");
					return -1;
				}
				
				conf->policers = rkzalloc(sizeof(policer_c) * v8, GFP_ATOMIC);
				if(!conf->policers) {
					LOG_ERR("Failure allocating policer/shapers");
					return -1;
				}
				conf->num_policers = v8;
				while(v8 >0){
					v8--;
					conf->policers[v8].max_count = 100;
					conf->policers[v8].gain_us = 100;
					conf->policers[v8].max_credits = 100000;
					conf->policers[v8].next_module = 0;
					conf->policers[v8].cherish_th = 10;
					conf->policers[v8].ecn_th = 5;
					conf->policers[v8].urgency_level =  conf->levels_urgency-1;
				}
				conf->state |= 2;
				
				return 0;
			}
			break;
		case 'p':
			if(v_name[1] == 's' && v_name[2] == '_') {
				if(sub_id == 0 || sub_id > conf->num_policers){
					LOG_ERR("Invalid policer id %u", sub_id);
					return -1;
				}
				v_name += 3;
				policer_i = conf->policers + sub_id -1;
				
				if(strcmp(v_name, "max_count") == 0) {
					policer_i->max_count = v16;
					return 0;
				} else if(strcmp(v_name, "gain_us") == 0) {
					policer_i->gain_us = v64;
					return 0;
				} else if(strcmp(v_name, "max_credit") == 0) {
					policer_i->max_credits = v64;
					return 0;
				} else if(strcmp(v_name, "next_module") == 0) {
					if(v8 >= conf->num_policers) {
						LOG_ERR("Invalid next P/S id %d", v8);
					}
					policer_i->next_module = v8;
					return 0;
				} else if(strcmp(v_name, "cherish_th") == 0) {
					policer_i->cherish_th = v8;
					return 0;
				} else if(strcmp(v_name, "ecn_th") == 0) {
					policer_i->ecn_th = v8;
					return 0;
				} else if(strcmp(v_name, "urgency") == 0) {
					if(v8 >= conf->levels_urgency) {
						LOG_ERR("Invalid urgency level %u at P/S %u", v8, sub_id);
						return -1;
					}
					policer_i->max_count = v8;
					return 0;
				}
			}
			break;
		case 'q':
			if(v_name[1] == 'o' && v_name[2] == 's' && v_name[3] == '_') {
				if(sub_id == 0 || sub_id > conf->num_policers){
					LOG_ERR("Invalid policer id %u", sub_id);
					return -1;
				}
				v_name += 4;
				qos2module_i = NULL;
				list_for_each_entry(qos2module_t, &conf->qos2modules, L) {
					if (sub_id == (u8) qos2module_t->qos_id) {
						qos2module_i = qos2module_t;
					}
				}
				if(!qos2module_i) {
					qos2module_i = rkzalloc(sizeof(qos2module), GFP_ATOMIC);
					if(!qos2module_i){						
						LOG_ERR("Failure allocating qos conf");
						return -1;
					} else {
						INIT_LIST_HEAD(&qos2module_i->L);
						list_add(&qos2module_i->L, &conf->qos2modules);
						qos2module_i->qos_id = (qos_id_t) sub_id;
						qos2module_i->next_module = 0;
						qos2module_i->def_cherish_th = 0;
						qos2module_i->def_urgency = conf->levels_urgency-1;
					}
				}
				if(strcmp(v_name, "next") == 0) {
					if(v8 >= conf->num_policers) {
						LOG_ERR("Invalid next P/S id %d", v8);
					}
					qos2module_i->next_module = v8;
				} else if(strcmp(v_name, "urgency") == 0) {
					if(v8 >= conf->levels_urgency) {
						LOG_ERR("Invalid urgency level  %u at P/S %u", v8, sub_id);
						return -1;
					}
					qos2module_i->def_urgency = v8;
				} else if(strcmp(v_name, "cherish_th") == 0) {
					qos2module_i->def_cherish_th = v16;
				}
			}
			break;
	}
	return 1;
}

void f_free_port_instance(base_config * conf, port_instance * port_i) {
	q_entry * entry_i;
	u8 i;
	
	port_i->P->rmt_ps_queues = NULL;
	list_del(&port_i->L);
	
	for(i = 0; i < conf->num_policers; i++) {
		while(!list_empty(&port_i->policers[i].Q)) {
			entry_i = list_first_entry(&port_i->policers[i].Q, q_entry, L);
			list_del(&entry_i->L);
			list_add(&entry_i->L, &conf->buffer);
			conf->buffer_size++;
		}
	}
	rkfree(port_i->policers);
	
	for(i = 0; i < conf->levels_urgency; i++) {
		while(!list_empty(port_i->Qs + i)) {
			entry_i = list_first_entry(port_i->Qs + i, q_entry, L);
			list_del(&entry_i->L);
			list_add(&entry_i->L, &conf->buffer);
			conf->buffer_size++;
		}
	}
	rkfree(port_i->Qs);
		
	rkfree(port_i);
}


/*
	Policy init and exit
*/
static struct ps_factory qta_factory = {
		.owner   = THIS_MODULE,
		.create  = f_policy_create,
		.destroy = f_policy_destroy,
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
