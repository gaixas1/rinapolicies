#include <linux/export.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/random.h>
#include <linux/list.h>

#define RINA_PREFIX "rmt_eqta-plugin"

#include "logs.h"
#include "rds/rmem.h"
#include "rmt-ps.h"
#include "policies.h"
#include "debug.h"

#define RINA_QTA_MUX_ps_NAME "rmt_eqta-ps"

/* Data structures */

//Configuration of ps
struct ps_config_t {
	uint_t max_count; // Max amount of PDUs admited
	uint_t gain4tick; // Credits gain each tick
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
	uint_t ps_id;
};

// Configuration of the policy
struct eqta_config {
	uint_t time2tick; // microseconds between ticks
	uint_t headers_weight; // Extra weight of headers
	uint_t num_ps; // Number of ps in the module
	uint_t levels_cherish; // Levels of cherish
	uint_t levels_urgency; // Levels of urgency
	
	struct list_head q_entry_L; // Buffer of q_entries
	struct ps_config_t * ps; // Configuration of ps modules, len == num_ps
	struct list_head Qos2ps_L; // List mapping QoS_id to ps index
};

// eqta instance information
struct eqta_instance {
	struct list_head L;
	struct rmt_n1_port * P;
	
	///ps related
	timespec lastT;	// "Time" of last call
	struct ps_data_t * ps; // ps modules, len == eqta_config.num_ps
	
	///Mux related
	uint_t mux_count; // Amount of PDUs waiting on the mux queues
	struct mux_Q_t * mux_queues; // Urgency queues in the mux, len == eqta_config.levels_urgency
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

static int eqta_set_param_pv(struct qta_mux_data * data, const char * name, const char * value);

struct q_entry * get_q_entry(struct eqta_config * base);

/* Main functions */

static struct ps_base * eqta_create(struct rina_component * component){
	struct rmt * rmt = rmt_from_component(component);
	struct rmt_ps * ps = rkzalloc(sizeof(*ps), GFP_ATOMIC);
	struct qta_mux_data * data;
	struct rmt_config * rmt_cfg;

	if (!ps)
		return NULL;

	/*
	REPLACE all inline
	data = qta_mux_data_create();
	if (!data) {
		rkfree(ps);
		return NULL;
	}

	data->config = qta_mux_config_create();
	if (!data->config) {
		rkfree(ps);
		qta_mux_data_destroy(data);
		return NULL;
	}
	*/
	
	ps->base.set_policy_set_param = eqta_p_set_param;
	ps->dm = rmt;
	ps->priv = data;

	rmt_cfg = rmt_config_get(rmt);
	if (rmt_cfg) {
		policy_for_each(rmt_cfg->policy_set, data, eqta_config_apply);
	} else {
		// TODO provide a suitable default for all the parameters.
		LOG_WARN("Missing defaults");
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
	if (bps && ps && ps->priv) {
		//qta_mux_data_destroy(ps->priv);
	}
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
	struct qta_mux_data * tmp = (struct qta_mux_data *) data;
	return eqta_set_param_pv(tmp, policy_param_name(param), policy_param_value(param));
}

static int eqta_p_set_param(struct ps_base * bps, const char * name, const char * value) {
	struct rmt_ps *ps = container_of(bps, struct rmt_ps, base);
	return eqta_set_param_pv((struct qta_mux_data *) ps->priv, name, value);
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

/*


struct q_entry {
	struct list_head next;
	struct pdu * data;
	struct q_qos * qqos;
	int w;
};

struct qta_queue {
	struct list_head list;
	struct list_head list_q_entry;
	struct list_head list_q_qos;
	uint_t key;
	struct rfifo * queue;
};

struct config_pshaper {
	uint_t rate;
	uint_t prb_max_length;
	uint_t abs_max_length;
	uint_t max_backlog;
};

struct config_q_qos {
	uint_t           abs_th;
	uint_t           th;
	uint_t           drop_prob;
	qos_id_t         qos_id;
	uint_t           key;
	uint_t           rate;
	uint_t           prb_max_length;
	uint_t           abs_max_length;
	uint_t           max_backlog;
	struct list_head list;
};

struct qta_config {
	struct list_head list_queues;
};

struct pshaper {
	struct config_pshaper config;
	uint_t length;
	uint_t backlog;
};

struct q_qos {
	uint_t           abs_th;
	uint_t           th;
	uint_t           drop_prob;
	qos_id_t         qos_id;
	struct list_head list;
	uint_t           dropped;
	uint_t           handled;
	uint_t           key;
	struct qta_queue * qta_q;
	struct pshaper   ps;
	struct robject robj;
};

struct qta_queue_set {
	struct list_head queues;
	struct list_head list;
	port_id_t        port_id;
	unsigned int     occupation;
	struct robject robj;
};

static ssize_t qta_qset_attr_show(struct robject * robj,
				  struct robj_attribute * attr,
				  char * buf)
{
	struct qta_queue_set * qset;

	qset = container_of(robj, struct qta_queue_set, robj);
	if (!qset)
		return 0;

	if (strcmp(robject_attr_name(attr), "port_id") == 0) {
		return sprintf(buf, "%u\n", qset->port_id);
	}

	if (strcmp(robject_attr_name(attr), "occupation") == 0) {
		return sprintf(buf, "%u\n", qset->occupation);
	}

	return 0;
}

static ssize_t qos_queue_attr_show(struct robject * robj,
				  struct robj_attribute * attr,
				  char * buf)
{
	struct q_qos * q;

	q = container_of(robj, struct q_qos, robj);
	if (!q)
		return 0;

	if (strcmp(robject_attr_name(attr), "urgency") == 0) {
		return sprintf(buf, "%u\n", q->key);
	}
	if (strcmp(robject_attr_name(attr), "queued_pdus") == 0) {
		return sprintf(buf, "%u\n", q->ps.length);
	}
	if (strcmp(robject_attr_name(attr), "backlog") == 0) {
		return sprintf(buf, "%u\n", q->ps.backlog);
	}
	if (strcmp(robject_attr_name(attr), "drop_pdus") == 0) {
		return sprintf(buf, "%u\n", q->dropped);
	}
	if (strcmp(robject_attr_name(attr), "total_pdus") == 0) {
		return sprintf(buf, "%u\n", q->handled);
	}
	if (strcmp(robject_attr_name(attr), "threshold") == 0) {
		return sprintf(buf, "%u\n", q->th);
	}
	if (strcmp(robject_attr_name(attr), "absolute_threshold") == 0) {
		return sprintf(buf, "%u\n", q->abs_th);
	}
	if (strcmp(robject_attr_name(attr), "drop_probability") == 0) {
		return sprintf(buf, "%u\n", q->drop_prob);
	}
	if (strcmp(robject_attr_name(attr), "qos_id") == 0) {
		return sprintf(buf, "%u\n", q->qos_id);
	}

	return 0;
}

static struct qta_queue * qta_queue_create(uint_t key)
{
	struct qta_queue * tmp = rkzalloc(sizeof(*tmp), GFP_ATOMIC);

	if (!tmp) {
		LOG_ERR("Could not allocate memory for QTA queue");
		return NULL;
	}
	tmp->key   = key;
	tmp->queue = rfifo_create();
	INIT_LIST_HEAD(&tmp->list_q_entry);
	INIT_LIST_HEAD(&tmp->list_q_qos);
	INIT_LIST_HEAD(&tmp->list);

	return tmp;
}

static void qta_queue_destroy(struct qta_queue * queue)
{
	struct q_entry *pos, *next;

	list_del(&queue->list);
	rfifo_destroy(queue->queue, (void (*)(void *)) pdu_destroy);
	list_for_each_entry_safe(pos, next, &queue->list_q_entry, next) {
		pdu_destroy(pos->data);
		list_del(&pos->next);
		rkfree(pos);
	}

	rkfree(queue);
}

static void q_qos_destroy(struct q_qos * q)
{
	if (!q)
		return;

	list_del(&q->list);
	//rfifo_destroy(q->queue, (void (*)(void *)) pdu_destroy);
	robject_del(&q->robj);
	rkfree(q);
}

static struct q_qos * q_qos_create(qos_id_t id,
				   uint_t abs_th,
				   uint_t th,
				   uint_t drop_prob,
				   uint_t key,
				   uint_t abs_max_length,
				   uint_t prb_max_length,
				   uint_t max_backlog,
				   uint_t rate,
				   struct qta_queue * qta_q,
				   struct robject * parent)
{
	struct q_qos * tmp;

	tmp = rkzalloc(sizeof(*tmp), GFP_ATOMIC);
	if (!tmp) {
		LOG_ERR("Couldn't create queue for QoS id %u", id);

		return NULL;
	}

	tmp->qos_id    = id;
	tmp->abs_th    = abs_th;
	tmp->th        = th;
	tmp->drop_prob = drop_prob;
	tmp->dropped   = 0;
	tmp->handled   = 0;
	tmp->key       = key;
	tmp->ps.config.abs_max_length = abs_max_length;
	tmp->ps.config.prb_max_length = prb_max_length;
	tmp->ps.config.max_backlog    = max_backlog;
	tmp->ps.config.rate           = rate;
	tmp->ps.backlog               = 0;
	tmp->ps.length                = 0;
	tmp->qta_q     = qta_q;
	INIT_LIST_HEAD(&tmp->list);

	if (robject_init_and_add(&tmp->robj, &qos_queue_rtype, parent, "queue-%d", id)) {
		LOG_ERR("Failed to create QTA MUX QoS Queue sysfs entry");
		q_qos_destroy(tmp);
		return NULL;
	}

	return tmp;
}

static struct config_q_qos * config_q_qos_create(qos_id_t id)
{
	struct config_q_qos * tmp = rkzalloc(sizeof(*tmp), GFP_ATOMIC);

	if (!tmp) {
		LOG_ERR("Could not create config queue %u", id);
		return NULL;
	}

	tmp->qos_id         = id;
	tmp->rate           = 0;
	tmp->abs_max_length = 0;
	tmp->prb_max_length = 0;
	tmp->max_backlog    = 0;
	INIT_LIST_HEAD(&tmp->list);

	return tmp;
}

static void config_q_qos_destroy(struct config_q_qos * q)
{
	if (!q)
		return;

	rkfree(q);
}

static struct config_q_qos * config_q_qos_find(struct qta_config * config,
					       qos_id_t            qos_id)
{
	struct config_q_qos * pos;

	list_for_each_entry(pos, &config->list_queues, list) {
		if (pos->qos_id == qos_id)
			return pos;
	}

	return NULL;
}

static struct qta_config * qta_mux_config_create(void)
{
	struct qta_config * tmp;

	tmp = rkzalloc(sizeof(*tmp), GFP_ATOMIC);
	if (!tmp) {
		LOG_ERR("Could not create config queue");
		return NULL;
	}
	INIT_LIST_HEAD(&tmp->list_queues);

	return tmp;
}

static void qta_mux_config_destroy(struct qta_config * config)
{
	struct config_q_qos *pos, *next;

	if (!config)
		return;

	list_for_each_entry_safe(pos, next, &config->list_queues, list) {
		config_q_qos_destroy(pos);
	}

	return;
}

static int qta_queue_set_destroy(struct qta_queue_set * qset)
{
	struct qta_queue *pos, *next;

	if (!qset)
		return -1;

	list_del(&qset->list);
	list_for_each_entry_safe(pos, next, &qset->queues, list) {
		qta_queue_destroy(pos);
	}
	robject_del(&qset->robj);
	rkfree(qset);

	return 0;
}

static struct qta_queue_set * qta_queue_set_create(port_id_t port_id,
						   struct robject *parent)
{
	struct qta_queue_set * tmp = rkzalloc(sizeof(*tmp), GFP_ATOMIC);

	if (!tmp)
		return NULL;

	INIT_LIST_HEAD(&tmp->queues);
	INIT_LIST_HEAD(&tmp->list);
	tmp->port_id    = port_id;
	tmp->occupation = 0;

	if (robject_init_and_add(&tmp->robj, &qta_qset_rtype, parent, "qset")) {
		LOG_ERR("Failed to create QTA MUX Queue Set sysfs entry");
		qta_queue_set_destroy(tmp);
		return NULL;
	}

	return tmp;
}

struct qta_mux_data {
	struct list_head    list_queues;
	struct qta_config * config;
};

static struct qta_mux_data * qta_mux_data_create(void)
{
	struct qta_mux_data * tmp;

	tmp = rkzalloc(sizeof(*tmp), GFP_ATOMIC);
	if (!tmp) {
		LOG_ERR("Problems creating QTA MUX data");
		return NULL;
	}

	INIT_LIST_HEAD(&tmp->list_queues);

	return tmp;
}

static void qta_mux_data_destroy(struct qta_mux_data * data)
{
	struct qta_queue_set *qset, *next;

	if (!data)
		return;

	qta_mux_config_destroy(data->config);
	list_for_each_entry_safe(qset, next, &data->list_queues, list) {
		qta_queue_set_destroy(qset);
	}

	rkfree(data);
}

static struct q_qos * q_qos_find(struct qta_queue * q, qos_id_t qos_id)
{
	struct q_qos * pos;

	list_for_each_entry(pos, &q->list_q_qos, list) {
		if (pos->qos_id == qos_id) {
			return pos;
		}
	}

	return NULL;
}

static struct q_qos * qta_q_qos_find(struct qta_queue_set * qset,
				     qos_id_t qos_id)
{
	struct qta_queue * pos;

	list_for_each_entry(pos, &qset->queues, list) {
		struct q_qos * to_ret = q_qos_find(pos, qos_id);

		if (to_ret) {
			return to_ret;
		}
	}

	return NULL;
}

static struct qta_queue * qta_queue_find_key(struct qta_queue_set * qset,
					     uint_t key)
{
	struct qta_queue * pos;

	list_for_each_entry(pos, &qset->queues, list) {
		if (pos->key == key)
			return pos;
	}

	return NULL;
}

struct qta_queue_set * queue_set_find(struct qta_mux_data * q,
				      port_id_t port_id)
{
	struct qta_queue_set * entry;

	list_for_each_entry(entry, &q->list_queues, queues) {
		if (entry->port_id == port_id) {
			return entry;
		}
	}

	return NULL;
}

static struct pdu * dequeue_entry(struct q_entry * entry,
				  struct qta_queue_set * qset)
{
	struct pdu * ret_pdu = entry->data;
	struct q_qos * qqos = entry->qqos;

	list_del(&entry->next);
	rkfree(entry);
	qset->occupation--;
	qqos->ps.length--;
	qqos->ps.backlog -= entry->w;

	return ret_pdu;

}

static struct q_entry * dequeue_qta_q_entry(struct qta_queue * q)
{
	struct q_entry * entry = NULL;

	entry = list_first_entry(&q->list_q_entry, struct q_entry, next);
	ASSERT(entry);

	return entry;
}

struct pdu * qta_rmt_dequeue_policy(struct rmt_ps	  *ps,
				    struct rmt_n1_port *n1_port)
{
	struct qta_queue *     entry;
	struct pdu *           ret_pdu;
	struct qta_queue_set * qset;
	struct q_entry *pos;

	if (!ps || !n1_port || !n1_port->rmt_ps_queues) {
		LOG_ERR("Wrong input parameters for "
				"rmt_next_scheduled_policy_tx");
		return NULL;
	}

	qset = n1_port->rmt_ps_queues;
	if (!qset)
		return NULL;

	list_for_each_entry(entry, &qset->queues, list) {
		if (!list_empty(&entry->list_q_entry)) {
			struct q_qos * qqos;
			pos = dequeue_qta_q_entry(entry);
			qqos = pos->qqos;
			ret_pdu = dequeue_entry(pos, qset);

			return ret_pdu;
		}
	}

	return NULL;
}

int qta_rmt_enqueue_policy(struct rmt_ps	  *ps,
			   struct rmt_n1_port *n1_port,
			   struct pdu	  *pdu)
{
	struct qta_queue_set * qset;
	struct q_qos *         q;
	int                    i;
	qos_id_t               qos_id;
	const struct pci * pci;
	struct q_entry * entry;
	uint_t mlength;
	uint_t mbacklog;
	uint_t w;

	if (!ps || !n1_port || !pdu) {
		LOG_ERR("Wrong input parameters for "
				"rmt_enqueu_scheduling_policy_tx");
		return RMT_ps_ENQ_ERR;
	}

	pci    = pdu_pci_get_ro(pdu);
	qos_id = pci_qos_id(pci);
	qset   = n1_port->rmt_ps_queues;
	q      = qta_q_qos_find(qset, qos_id);
	LOG_DBG("Enqueueing for QoS id %u", qos_id);

	LOG_DBG("First of all, Policer/Shaper");
	if (!q) {
		LOG_ERR("No queue for QoS id %u, dropping PDU", qos_id);
		pdu_destroy(pdu);
		return RMT_ps_ENQ_ERR;
	}

	mlength  = q->ps.config.abs_max_length;
	mbacklog = q->ps.config.max_backlog * q->ps.config.rate;
	q->ps.length++;

	if (mlength && q->ps.length > mlength) {
		LOG_INFO("Length exceeded for QoS id %u, dropping PDU", qos_id);
		pdu_destroy(pdu);
		q->ps.length--;
		q->dropped++;
		return RMT_ps_ENQ_DROP;
	}

	w = (int) pdu_len(pdu);
	q->ps.backlog += w;
	if (mbacklog && q->ps.backlog > mbacklog) {
		LOG_INFO("Backlog exceeded for QoS id %u, dropping PDU", qos_id);
		q->dropped++;
		q->ps.length--;
		q->ps.backlog -= w;
		pdu_destroy(pdu);
		return RMT_ps_ENQ_DROP;
	}

	q->handled++;
	if (q->abs_th < (qset->occupation + 1)) {
		LOG_DBG("Dropped PDU: abs_th exceeded %u", qset->occupation);
		q->dropped++;
		q->ps.length--;
		q->ps.backlog -= w;
		pdu_destroy(pdu);
		return RMT_ps_ENQ_DROP;
	}

	entry = rkzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		LOG_ERR("Failed to enqueue");
		q->dropped++;
		q->ps.length--;
		q->ps.backlog -= w;
		pdu_destroy(pdu);
		return RMT_ps_ENQ_ERR;
	}
	entry->data = pdu;
	entry->qqos = q;
	entry->w    = w;
	INIT_LIST_HEAD(&entry->next);
	list_add(&entry->next, &q->qta_q->list_q_entry);

	qset->occupation++;

	LOG_DBG("PDU enqueued");
	return RMT_ps_ENQ_SCHED;
}

static void qset_add_qta_queue(struct qta_queue *     queue,
			       struct qta_queue_set * qset)
{
	struct qta_queue * pos;

	if (list_empty(&qset->queues)) {
		list_add(&queue->list, &qset->queues);
		return;
	}

	/* Debug only * /
	list_for_each_entry(pos, &qset->queues, list) {
		struct q_qos * qqos;

		LOG_INFO("Urgency class: %u", pos->key);
		list_for_each_entry(qqos, &pos->list_q_qos, list) {
			LOG_INFO("Qos id: %u, Drop prob: %u",
					qqos->qos_id,
					qqos->drop_prob);
		}
	}

	list_for_each_entry(pos, &qset->queues, list) {
		if (pos->key > queue->key) {
			list_add_tail(&queue->list, &pos->list);
			return;
		}
	}
	list_add(&queue->list, &pos->list);
}

void * qta_rmt_q_create_policy(struct rmt_ps      *ps,
			       struct rmt_n1_port *n1_port)
{
	struct qta_mux_data *  data = ps->priv;
	struct qta_config * config;
	struct config_q_qos * pos;
	struct qta_queue_set * tmp;
	struct q_qos *        q_qos;
	struct qta_queue * queue;

	if (!ps || !n1_port || !data) {
		LOG_ERR("Wrong input parameters for "
				"rmt_scheduling_create_policy_common");
		return NULL;
	}

	/* Create the n1_port queue group * /
	/* Add it to the n1_port * /
	tmp = qta_queue_set_create(n1_port->port_id, &n1_port->robj);
	if (!tmp) {
		LOG_ERR("No scheduling policy created for port-id %d",
				n1_port->port_id);
		return NULL;
	}
	list_add(&tmp->list, &data->list_queues);
	config = data->config;

	list_for_each_entry(pos, &config->list_queues, list) {
		queue = qta_queue_find_key(tmp, pos->key);
		if (!queue) {
			LOG_INFO("Created QTA MUX queue for class %u",
				  pos->key);
			queue = qta_queue_create(pos->key);
			if (!queue) {
				qta_queue_set_destroy(tmp);
				return NULL;
			}
			qset_add_qta_queue(queue, tmp);
		}
		q_qos = q_qos_create(pos->qos_id,
				     pos->abs_th,
				     pos->th,
				     pos->drop_prob,
				     pos->key,
				     pos->abs_max_length,
				     pos->prb_max_length,
				     pos->max_backlog,
				     pos->rate,
				     queue,
				     &tmp->robj);
		if (!q_qos) {
			qta_queue_set_destroy(tmp);
			return NULL;
		}
		LOG_INFO("Added queue for QoS id %u", pos->qos_id);
		list_add(&q_qos->list, &queue->list_q_qos);
	}

	return tmp;
}

int qta_rmt_q_destroy_policy(struct rmt_ps      *ps,
			     struct rmt_n1_port *n1_port)
{
	struct qta_mux_data *  data = ps->priv;
	struct qta_queue_set * tmp = n1_port->rmt_ps_queues;

	if (!tmp || !data)
		return -1;

	qta_queue_set_destroy(tmp);
	n1_port->rmt_ps_queues = NULL;

	return 0;
}

static int qta_mux_ps_set_policy_set_param_priv(struct qta_mux_data * data,
						const char *     name,
						const char *     value)
{
	struct config_q_qos * conf_qos;
	int int_value, ret, offset, dotlen;
	qos_id_t qos_id;
	char *dot, *buf;

	if (!name) {
		LOG_ERR("Null parameter name");
		return -1;
	}

	if (!value) {
		LOG_ERR("Null parameter value");
		return -1;
	}

	dot = strchr(name, '.');
	if (!dot) {
		LOG_ERR("No info enough");
		return -1;
	}

	offset = dotlen = dot - name;
	if (name[offset] == '.')
		offset++;

	buf = rkzalloc((dotlen + 1), GFP_ATOMIC);
	if (buf) {
		memcpy(buf, name, dotlen);
		buf[dotlen] = '\0';
	}

	ret = kstrtoint(buf, 10, &qos_id);
	LOG_DBG("Arrived parameter info for QoS id %u", qos_id);
	rkfree(buf);
	conf_qos = config_q_qos_find(data->config, qos_id);
	if (!conf_qos) {
		conf_qos = config_q_qos_create(qos_id);
		if (!conf_qos)
			return -1;

		list_add(&conf_qos->list, &data->config->list_queues);
		LOG_INFO("Created queue for QoS id %u", qos_id);
	}

	if (strcmp(name + offset, "urgency-class") == 0) {
		ret = kstrtoint(value, 10, &int_value);
		if (!ret) {
			LOG_DBG("Urgency class %u", int_value);
			conf_qos->key = int_value;
			return 0;
		}
		LOG_ERR("Could not parse urgency class for QoS %u", qos_id);
		return -1;
	}

	if (strcmp(name + offset, "drop-prob") == 0) {
		ret = kstrtoint(value, 10, &int_value);
		if (!ret) {
			LOG_DBG("Drop probability %u", int_value);
			conf_qos->drop_prob = int_value;
			return 0;
		}
		LOG_ERR("Could not parse drop probability for QoS %u", qos_id);
		return -1;
	}

	if (strcmp(name + offset, "abs-th") == 0) {
		ret = kstrtoint(value, 10, &int_value);
		if (!ret) {
			LOG_DBG("Absolute threshold %u", int_value);
			conf_qos->abs_th = int_value;
			return 0;
		}
		LOG_ERR("Could not parse abs threshold for QoS %u", qos_id);
		return -1;
	}

	if (strcmp(name + offset, "th") == 0) {
		ret = kstrtoint(value, 10, &int_value);
		if (!ret) {
			LOG_DBG("Threshold %u", int_value);
			conf_qos->th = int_value;
			return 0;
		}
		LOG_ERR("Could not parse threshold for QoS %u", qos_id);
		return -1;
	}

	if (strcmp(name + offset, "rate") == 0) {
		ret = kstrtoint(value, 10, &int_value);
		if (!ret) {
			LOG_DBG("Rate %u", int_value);
			conf_qos->rate = int_value;
			return 0;
		}
		LOG_ERR("Could not parse RATE for QoS %u", qos_id);
		return -1;
	}

	if (strcmp(name + offset, "max_length") == 0) {
		ret = kstrtoint(value, 10, &int_value);
		if (!ret) {
			LOG_DBG("Max Length %u", int_value);
			conf_qos->prb_max_length = int_value;
			return 0;
		}
		LOG_ERR("Could not parse MAX LENGTH for QoS %u", qos_id);
		return -1;
	}

	if (strcmp(name + offset, "abs_max_length") == 0) {
		ret = kstrtoint(value, 10, &int_value);
		if (!ret) {
			LOG_DBG("ABS Max Length %u", int_value);
			conf_qos->abs_max_length = int_value;
			return 0;
		}
		LOG_ERR("Could not parse ABS MAX LENGTH for QoS %u", qos_id);
		return -1;
	}

	if (strcmp(name + offset, "max_backlog") == 0) {
		ret = kstrtoint(value, 10, &int_value);
		if (!ret) {
			LOG_DBG("Max Backlog %u", int_value);
			conf_qos->max_backlog = int_value;
			return 0;
		}
		LOG_ERR("Could not parse MAX BACKLOG for QoS %u", qos_id);
		return -1;
	}

	LOG_ERR("No such parameter to set");

	return -1;
}

static int rmt_config_apply(struct policy_parm * param, void * data)
{
	struct qta_mux_data * tmp;

	tmp = (struct qta_mux_data *) data;

	return qta_mux_ps_set_policy_set_param_priv(data,
			policy_param_name(param),
			policy_param_value(param));
}

static int qta_ps_set_policy_set_param(struct ps_base * bps,
				       const char *     name,
				       const char *     value)
{
	struct rmt_ps *ps = container_of(bps, struct rmt_ps, base);
	struct qta_mux_data * data = ps->priv;

	return qta_mux_ps_set_policy_set_param_priv(data, name, value);
}

static struct ps_base *
rmt_ps_qta_create(struct rina_component * component)
{
	struct rmt * rmt = rmt_from_component(component);
	struct rmt_ps * ps = rkzalloc(sizeof(*ps), GFP_ATOMIC);
	struct qta_mux_data * data;
	struct rmt_config * rmt_cfg;

	if (!ps)
		return NULL;

	data = qta_mux_data_create();
	if (!data) {
		rkfree(ps);
		return NULL;
	}

	data->config = qta_mux_config_create();
	if (!data->config) {
		rkfree(ps);
		qta_mux_data_destroy(data);
		return NULL;
	}

	ps->base.set_policy_set_param = qta_ps_set_policy_set_param;
	ps->dm = rmt;
	ps->priv = data;

	rmt_cfg = rmt_config_get(rmt);
	if (rmt_cfg) {
		policy_for_each(rmt_cfg->policy_set, data, rmt_config_apply);
	} else {
		/* TODO provide a suitable default for all the parameters. * /
		LOG_WARN("Missing defaults");
	}

	ps->rmt_dequeue_policy = qta_rmt_dequeue_policy;
	ps->rmt_enqueue_policy = qta_rmt_enqueue_policy;
	ps->rmt_q_create_policy = qta_rmt_q_create_policy;
	ps->rmt_q_destroy_policy = qta_rmt_q_destroy_policy;

	LOG_INFO("Loaded QTA MUX policy set and its configuration");

	return &ps->base;
}

static void rmt_ps_qta_destroy(struct ps_base * bps)
{
	struct rmt_ps *ps = container_of(bps, struct rmt_ps, base);

	if (bps) {
		if (ps && ps->priv)
			qta_mux_data_destroy(ps->priv);
	}
}

*/
