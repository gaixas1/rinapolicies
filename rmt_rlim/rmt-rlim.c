//rmt-rlim.c
#define RINA_PREFIX "rmt-rlim-plugin"
#define RINA_QTA_MUX_ps_NAME "rmt-rlim-ps"
#include "rmt-rlim.h"


#ifdef DEBUG
#define KALLOC(x) rkzalloc(x,GFP_ATOMIC) 
#define KFREE(x) rkfree(x) 
#else
#include "linux/slab.h"
#define KALLOC(x) kmalloc(x,GFP_ATOMIC)
#define KFREE(x) kfree(x) 
#endif

MODULE_DESCRIPTION("RMT R-LIM policy set");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sergio Leon <gaixas1@gmail.com>");

/* Main functions */

static struct ps_base * f_policy_create(struct rina_component * component){
	struct rmt * rmt; 
	struct rmt_ps * ps;
	struct rmt_config * rmt_cfg;
	base_config * conf;

	rmt = rmt_from_component(component);
	ps = (struct rmt_ps *) KALLOC(sizeof(*ps));
	if (!ps) {
		return NULL;
	}
	
	conf = (base_config *) KALLOC(sizeof(base_config));
	if (!conf) {
		LOG_ERR("Could not create conf queue");
		return NULL;
	}
		
	INIT_LIST_HEAD(&conf->buffer);
	INIT_LIST_HEAD(&conf->port_instances);
	INIT_LIST_HEAD(&conf->Q2CU);
	
	conf->max_count = 100;
	conf->buffer_size = 0;
	conf->levels_urgency = 1;
	conf->levels_cherish = 1;
	conf->headers_weight = 0;
	conf->bytecost = 1;
	conf->S = 0;
	
	conf->gain_us_u = NULL;
	conf->max_credit_u = NULL;
	conf->gain_us_c = NULL;
	conf->max_credit_c = NULL;
	conf->th_c = NULL;

	ps->base.set_policy_set_param = NULL;//f_set_policy_set_param;
	ps->dm = rmt;
	ps->priv = conf;

	rmt_cfg = rmt_config_get(rmt);
	if (rmt_cfg) {
		policy_for_each(rmt_cfg->policy_set, conf, f_policy_base_config_apply);
	} else {
		LOG_WARN("Using default conf (best-effort)");
	}
	
	if(!(conf->S & 1)) {
		conf->gain_us_u = (u64 *) KALLOC(sizeof(u64));
		conf->max_credit_u = (u64 *) KALLOC(sizeof(u64));
		if(conf->gain_us_u == NULL ||conf->max_credit_u == NULL) {
			conf->gain_us_u[0] = 1;
			conf->max_credit_u[0] = 10000;
			conf->levels_urgency = 1;
		}
		conf->S |= 1;
	}
	if(!(conf->S & 2)) {
		conf->gain_us_c = (u64 *) KALLOC(sizeof(u64));
		conf->max_credit_c = (u64 *) KALLOC(sizeof(u64));
		conf->th_c = (u16 *) KALLOC(sizeof(u16));
		if(conf->gain_us_c == NULL ||conf->max_credit_c == NULL) {
			conf->gain_us_c[0] = 1;
			conf->max_credit_c[0] = 10000;
			conf->th_c[0] = 100;
			conf->levels_cherish = 1;
		}
		conf->S |= 1;
	}
	
	conf->num_queues = conf->levels_urgency * conf->levels_cherish; 
		
	if(conf->gain_us_u == NULL || conf->max_credit_u == NULL 
		|| conf->gain_us_c == NULL || conf->max_credit_c == NULL
		|| conf->th_c == NULL ) {
		if(conf->gain_us_u){
			KFREE(conf->gain_us_u);
		}
		if(conf->max_credit_u){
			KFREE(conf->max_credit_u);
		}
		if(conf->gain_us_c){
			KFREE(conf->gain_us_c);
		}
		if(conf->max_credit_c){
			KFREE(conf->max_credit_c);
		}
		if(conf->th_c){
			KFREE(conf->th_c);
		}
			
		KFREE(conf);
		
		LOG_ERR("Failure allocating memory");
		return NULL;
	}

	ps->rmt_q_create_policy = f_rmt_q_create_policy;
	ps->rmt_q_destroy_policy = f_rmt_q_destroy_policy;
	ps->rmt_enqueue_policy = f_rmt_enqueue_policy;
	ps->rmt_dequeue_policy = f_rmt_dequeue_policy;

	LOG_INFO("Loaded R-LIM policy set and its confuration");

	return &ps->base;
}

static void f_policy_destroy(struct ps_base * bps) {
	struct rmt_ps *ps;
	base_config * conf;
	port_instance * port_i;
	q_entry * entry_i;
	
	ps = container_of(bps, struct rmt_ps, base);
	if (!bps || !ps || !ps->priv) {
		LOG_ERR("Error on rmt policy destroy. Some modules not set.");
		return;
	}
	
	conf = (base_config *) ps->priv;
	ps->priv = NULL;
	
	// Delete all remaining port instances
	while(!list_empty(&conf->port_instances)) {
		port_i = list_first_entry(&conf->port_instances, port_instance, L);
		f_free_port_instance(conf, port_i);
	}
	
	// Empty buffers
	while(!list_empty(&conf->buffer)) {
		entry_i = list_first_entry(&conf->buffer, q_entry, L);
		list_del(&entry_i->L);
		KFREE(entry_i);
		conf->buffer_size--;
	}
	
	if(conf->gain_us_u){
		KFREE(conf->gain_us_u);
	}
	if(conf->max_credit_u){
		KFREE(conf->max_credit_u);
	}
	if(conf->gain_us_c){
		KFREE(conf->gain_us_c);
	}
	if(conf->max_credit_c){
		KFREE(conf->max_credit_c);
	}
	if(conf->th_c){
		KFREE(conf->th_c);
	}
	
	KFREE(conf);
}

int f_rmt_enqueue_policy(struct rmt_ps *ps, port_p P, pdu_p pdu_i) {
	const struct pci * pci_i;
	qos_id_t qos_id;
	base_config * conf;
	
	port_instance * port_i;
	q_entry * entry_i;
	u8 next_urgency;
	u8 next_cherish;
	u16 q_id;
	qos2CU * qos2cu_i;
	
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
	
	if(port_i->count >= conf->max_count) {
		LOG_INFO("Length exceeded for Port, dropping PDU");
		pdu_destroy(pdu_i);
		return RMT_PS_ENQ_DROP;
	}
	
	next_urgency = conf->levels_urgency -1;
	next_cherish = conf->levels_cherish -1;
	list_for_each_entry(qos2cu_i, &conf->Q2CU, L) {
		if (qos_id == qos2cu_i->qos_id) {
			next_urgency = qos2cu_i->urgency;
			next_cherish = qos2cu_i->cherish;
			break;
		}
	}
	
	if(port_i->count >= conf->th_c[next_cherish]) {
		LOG_INFO("Length exceeded for Cherish level, dropping PDU");
		pdu_destroy(pdu_i);
		return RMT_PS_ENQ_DROP;
	}
	
	
	if(list_empty(&conf->buffer)) {
		entry_i = (q_entry *) KALLOC(sizeof(q_entry));
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
	entry_i->cost = (u64) pdu_len(pdu_i) + (u64) conf->headers_weight;
	entry_i->cost *= conf->bytecost;
	
	q_id = next_cherish + next_urgency * conf->levels_cherish;
	list_add_tail(&entry_i->L, &port_i->Q[q_id].q);
	port_i->count++;
	
	LOG_DBG("PDU enqueued");
	return RMT_PS_ENQ_SCHED;
}

inline void gain(u64 * credits, u64 T, u8 L, u64 * gain_us) {
	u8 i, j;
	s64 g, k;
	
	j = L-2;
	for(i = L-1; i < L; i--) {
		g = gain_us[i] * T;
		while(j < i && g > 0) {
			k = credits[j];
			if(k >= 0) {
				j++;
			} else if (k + g < 0) {
				k += g;
				g = 0;
			} else {
				g += k;
				k = 0;
				j++;
			}
		}
		if(g > 0) {
			credits[i] += g;
		}
	}
}

inline void spend(u64 * credits, u32 cost, u8 l, u8 L, u64 * max_credit) {
	u8 i;
	s64 k;
	u64 * current_cred, * current_max;
	
	for(i = l; i < L && cost > 0; i--) {
		if ( credits[i] >= cost) {
			credits[i] -= cost;
			cost = 0;
		} else {
			cost -= credits[i];
			credits[i] = 0;
		}
	}
	if(cost > 0) {
		credits[l] -= cost;
	}
	
	k = 0;
	current_cred = credits;
	current_max = max_credit;
	for(i = 0; i < L; i++) {
		*current_cred += k;
		if(*current_cred > *current_max) {
			k += *current_cred - *current_max;
			*current_cred = *current_max;
		}
		current_cred++;
		current_max++;
	}
}

pdu_p f_rmt_dequeue_policy(struct rmt_ps * ps, port_p P) {
	base_config * conf;
	port_instance * port_i;
	q_entry * entry_i;
	struct timespec t1, td;
	u64 T;
	pdu_p pdu_i;
	u8 mu, mc, lu, lc, i, j;
	queue * current_q, * sel_q;
	u32 cost;
	
	if (!ps || !P || !P->rmt_ps_queues) {
		LOG_ERR("Wrong input parameters for rmt_enqueu_scheduling_policy_rx");
		return NULL;
	}
	
	conf = ps->priv;
	port_i = P->rmt_ps_queues;
	
	lu = conf->levels_urgency;
	lc = conf->levels_cherish;
	
	//Compute ticks from last call
	getnstimeofday (&t1);
	if(timespec_compare(&port_i->lastT, &t1) < 0) {
		td = timespec_sub(t1, port_i->lastT);
		if(td.tv_sec < 2) { // MAX 2s
			T = (u64)td.tv_sec *1000000 + (u64)td.tv_nsec/1000;
		} else {
			T = 2000000;
		}
		if(T > 0){
			port_i->lastT = t1;
			gain(port_i->credits_u, T, lu, conf->gain_us_u);
			gain(port_i->credits_c, T, lc, conf->gain_us_c);
		}
	}
	
	mu = lu;
	for(i = 0; i < mu; i++) {
		if(port_i->credits_u[i] > 0) {
			mu = i; 
		}
	}
	mc = lc;
	for(i = 0; i < mc; i++) {
		if(port_i->credits_c[i] > 0) { 
			mc = i; 
		}
	}
	
	current_q = port_i->Q;
	current_q += mu*lc + mc;
	sel_q = NULL;
	for(i = mu; !sel_q && i < lu; i++) {
		for(j = mc; !sel_q && j < lc; j++) {
			if(current_q->count > 0) {
				sel_q = current_q;
			}
			current_q++;
		}
		current_q += mc;
	}
	
	if(sel_q == NULL) {
		return NULL;
	}
	
	entry_i = list_first_entry(&sel_q->q, q_entry, L);
	list_del(&entry_i->L);
	sel_q->count--;
	
	pdu_i = entry_i->data;
	cost = entry_i->cost;
	list_add(&entry_i->L, &conf->buffer);
	
	spend(port_i->credits_u, cost, sel_q->urgency, lu, conf->max_credit_u);
	spend(port_i->credits_c, cost, sel_q->cherish, lc, conf->max_credit_c);
	
	return pdu_i;
}

void * f_rmt_q_create_policy(struct rmt_ps *ps, port_p P) {
	base_config * conf;
	port_instance * port_i;
	u8 i, j;
	queue * q;
	
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
	
	port_i = (port_instance *) KALLOC(sizeof(port_instance));
	if(!port_i) {
		LOG_ERR("Memory alloc problem in rmt_create_p_policy");
		return NULL;
	}
	
	port_i->credits_u = (s64 *) KALLOC(sizeof(s64) * conf->levels_urgency);
	port_i->credits_c = (s64 *) KALLOC(sizeof(s64) * conf->levels_cherish);
	port_i->Q = (queue *) KALLOC(sizeof(queue) * conf->num_queues);
	
	if(port_i->credits_u == NULL
		|| port_i->credits_c == NULL
		|| port_i->Q == NULL) {
		if(port_i->credits_u) {
			KFREE(port_i->credits_u);
		}
		if(port_i->credits_c) {
			KFREE(port_i->credits_c);
		}
		if(port_i->Q) {
			KFREE(port_i->Q);
		}
		KFREE(port_i);
		
		LOG_ERR("Memory alloc problem in rmt_create_p_policy");
		return NULL;
	}
	
	port_i->count = 0;
	getnstimeofday (&port_i->lastT);
	
	for(i = 0 ; i < conf->levels_urgency; i++) {
		port_i->credits_u[i] = 0;
	}
	for(i = 0 ; i < conf->levels_cherish; i++) {
		port_i->credits_c[i] = 0;
	}
	for(i = 0 ; i < conf->levels_urgency; i++) {
		for(j = 0 ; j < conf->levels_cherish; j++) {
			q = &port_i->Q[i*conf->levels_cherish + j];
			q->count = 0;
			q->urgency = i;
			q->cherish = j;
			INIT_LIST_HEAD(&q->q);
		}
	}
	
	port_i->P = P;
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

/*
static int f_set_policy_set_param(struct ps_base * bps, const char * name, const char * value) {
	struct rmt_ps *ps;
	ps = container_of(bps, struct rmt_ps, base);
	return f_policy_set_param_pv((base_config *) ps->priv, name, value);
}
*/

static int f_policy_set_param_pv(base_config * conf, const char * name, const char * value) {
	
	char * v_name;
	char * v_value;
	char * p_ch;
	u8 sub_id;
	u8 v8;
	u16 v16;
	u32 v32;
	u64 v64;
	q_entry * entry_i;
	qos2CU * qos2CU_i, * qos2CU_t;
	
	
	if (!name) {
		LOG_ERR("Null parameter name");
		return -1;
	}
	if (!value) {
		LOG_ERR("Null parameter value");
		return -1;
	}
	
	v_name = (char *) name;
	v_value = (char *) value;
	
	p_ch = strchr(v_value, '.');
	sub_id = 0;
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
		case 'a':
			if(strcmp(v_name, "add_buffer") == 0) {
				while(v16 > 0) {
					entry_i = (q_entry *) KALLOC(sizeof(q_entry));
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
		case 'b':
			if(strcmp(v_name, "bytecost") == 0) {
				conf->bytecost = v8;
				return 0;
			}
			break;
		case 'm' :
			if(strcmp(v_name, "max_count") == 0) {
				conf->max_count = v16;
				return 0;
			}
			if(strcmp(v_name, "max_credit_u") == 0) {
				if(!(conf->S & 1)) {
					LOG_ERR("Urgency not set yet");
					return -1;
				}
				conf->max_credit_u[sub_id] = v64;
				return 0;
			}
			if(strcmp(v_name, "max_credit_c") == 0) {
				if(!(conf->S & 2)) {
					LOG_ERR("Cherish not set yet");
					return -1;
				}
				conf->max_credit_c[sub_id] = v64;
				return 0;
			}
			break;
		case 'h':
			if(strcmp(v_name, "header_weight") == 0) {
				conf->headers_weight = v8;
				return 0;
			}
			break;
		case 'l':
			if(strcmp(v_name, "levels_urgency") == 0) {
				if(conf->S & 1) {
					LOG_ERR("Urgency already set");
					return -1;
				}
				if(v8 == 0){
					LOG_ERR("Required at least one urgency level");
					return -1;
				}
				conf->gain_us_u = (u64 *) KALLOC(sizeof(u64) * v8);
				conf->max_credit_u = (u64 *) KALLOC(sizeof(u64) * v8);
				if(conf->gain_us_u == NULL ||conf->max_credit_u == NULL) {
					if(conf->gain_us_u) {
						KFREE(conf->gain_us_u);
						conf->gain_us_u = NULL;
					}
					if(conf->max_credit_u) {
						KFREE(conf->max_credit_u);
						conf->max_credit_u = NULL;
					}
					LOG_ERR("Failure allocating memory");
					return -1;
				}
				
				while(v8 > 0) {
					v8--;
					conf->gain_us_u[v8] = 1;
					conf->max_credit_u[v8] = 10000;
				}
				
				conf->levels_urgency = v8;
				conf->S |= 1;
				return 0;
			}
			if(strcmp(v_name, "levels_cherish") == 0) {
				if(conf->S & 2) {
					LOG_ERR("Cherish already set");
					return -1;
				}
				if(v8 == 0){
					LOG_ERR("Required at least one cherish level");
					return -1;
				}
				conf->gain_us_c = (u64 *) KALLOC(sizeof(u64) * v8);
				conf->max_credit_c = (u64 *) KALLOC(sizeof(u64) * v8);
				conf->th_c = (u16 *) KALLOC(sizeof(u16) * v8);
				if(conf->gain_us_c == NULL ||conf->max_credit_c == NULL
					||conf->th_c == NULL) {
					if(conf->gain_us_c) {
						KFREE(conf->gain_us_c);
						conf->gain_us_c = NULL;
					}
					if(conf->max_credit_c) {
						KFREE(conf->max_credit_c);
						conf->max_credit_c = NULL;
					}
					if(conf->th_c) {
						KFREE(conf->th_c);
						conf->th_c = NULL;
					}
					LOG_ERR("Failure allocating memory");
					return -1;
				}
				
				while(v8 > 0) {
					v8--;
					conf->gain_us_c[v8] = 1;
					conf->max_credit_c[v8] = 10000;
					conf->th_c[v8] = 100;
				}
				conf->levels_cherish = v8;
				conf->S |= 2;
				return 0;
			}
			break;
		case 'g':
			if(strcmp(v_name, "gain_us_u") == 0) {
				if(!(conf->S & 1)) {
					LOG_ERR("Urgency not set yet");
					return -1;
				}
				conf->gain_us_u[sub_id] = v64;
				return 0;
			}
			if(strcmp(v_name, "gain_us_c") == 0) {
				if(!(conf->S & 2)) {
					LOG_ERR("Cherish not set yet");
					return -1;
				}
				conf->gain_us_c[sub_id] = v64;
				return 0;
			}
			break;
		case 't':
			if(strcmp(v_name, "th_c") == 0) {
				if(!(conf->S & 2)) {
					LOG_ERR("Cherish not set yet");
					return -1;
				}
				conf->th_c[sub_id] = v16;
				return 0;
			}
			break;
		case 'q':
			qos2CU_i = NULL;
			list_for_each_entry(qos2CU_t, &conf->Q2CU, L) {
				if (sub_id == (u8) qos2CU_t->qos_id) {
					qos2CU_i = qos2CU_t;
				}
			}
			if(strcmp(v_name, "qos_urgency") == 0) {
				if(qos2CU_i == NULL) {
					qos2CU_i = (qos2CU *) KALLOC(sizeof(qos2CU));
					if(!qos2CU_i){						
						LOG_ERR("Failure allocating qos conf");
						return -1;
					}
					list_add(&qos2CU_i->L, &conf->Q2CU);
					qos2CU_i->qos_id = sub_id;
					qos2CU_i->urgency = v8;
					qos2CU_i->cherish = conf->levels_cherish-1;
				} else {
					qos2CU_i->urgency = v8;
				}
			}
			if(strcmp(v_name, "qos_cherish") == 0) {
				if(qos2CU_i == NULL) {
					qos2CU_i = (qos2CU *) KALLOC(sizeof(qos2CU));
					if(!qos2CU_i){						
						LOG_ERR("Failure allocating qos conf");
						return -1;
					}
					list_add(&qos2CU_i->L, &conf->Q2CU);
					qos2CU_i->qos_id = sub_id;
					qos2CU_i->urgency = conf->levels_urgency-1;
					qos2CU_i->cherish = v8;
				} else {
					qos2CU_i->cherish = v8;
				}
			}
			break;
	}
	return 1;
}

void f_free_port_instance(base_config * conf, port_instance * port_i) {
	q_entry * entry_i;
	u16 nQ;
	queue * current_q;
	
	current_q = port_i->Q;
	nQ = conf->num_queues;
	for(; nQ > 0; nQ--) {
		while(current_q->count != 0) {
			entry_i = list_first_entry(&current_q->q, q_entry, L);
			list_del(&entry_i->L);
			list_add(&entry_i->L, &conf->buffer);
			
			current_q->count--;
			conf->buffer_size++;
		}
		current_q++;
	}
	
	port_i->P->rmt_ps_queues = NULL;
	list_del(&port_i->L);
	
	if(port_i->credits_u) {
		KFREE(port_i->credits_u);
	}
	if(port_i->credits_c) {
		KFREE(port_i->credits_c);
	}
	if(port_i->Q) {
		KFREE(port_i->Q);
	}
	KFREE(port_i);
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
	LOG_INFO("RMT R-LIM policy set loaded successfully");
	return 0;
}

static void __exit mod_exit(void) {
	if (rmt_ps_unpublish(RINA_QTA_MUX_ps_NAME)) {
		LOG_ERR("Failed to unpublish policy set factory");
	} else {
		LOG_INFO("RMT R-LIM policy set unloaded successfully");
	}
}

module_init(mod_init);
module_exit(mod_exit);