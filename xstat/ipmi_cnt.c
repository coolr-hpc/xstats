#include <linux/ipmi.h>

struct ipmi_sensor_config {
	char	name[XSTAT_CNT_LEN];
	uint8_t sensor_number;
	uint8_t mul;
};

struct ipmi_sensor_ctx {
	struct ipmi_sensor_config config;
	uint8_t sensor_reading;
};

struct ipmi_sensors_ctx {
	spinlock_t lock;
	ipmi_user_t user;
	int msgid;
	int nctxs;
	struct ipmi_sensor_ctx *ctxs;
};

static int xstat_ipmi_cnt_init(const struct cpumask *mask, void *data, void **ctx);
static uint64_t xstat_ipmi_restart(void **_ctx, uint64_t last);
static int xstat_ipmi_scnprintf(char *buf, int limit, uint64_t data, void **_ctx);

#define __IPMI_CTX(sname, snum, smul) { .config = { .name = #sname, .sensor_number = snum, .mul = smul }, .sensor_reading = 0 }
#define __IPMI_CNT(ctx) { .name = "ipmi", .init = xstat_ipmi_cnt_init, .exit = NULL, .restart = xstat_ipmi_restart, .reset = NULL, .scnprintf = xstat_ipmi_scnprintf, .data = &ctx }
#ifdef XSTAT_COOLR
static struct ipmi_sensor_ctx xstat_sensor_ctxs[] = {
	__IPMI_CTX(FAN1, 65, 100),
	__IPMI_CTX(FAN2, 66, 100),
	__IPMI_CTX(FAN3, 67, 100),
	__IPMI_CTX(FAN4, 68, 100),
	__IPMI_CTX(FANA, 71, 100),
	__IPMI_CTX(FANB, 72, 100),
	__IPMI_CTX(FANC, 69, 100),
	__IPMI_CTX(FAND, 70, 100),
};
static struct ipmi_sensors_ctx xstat_group0_ctx = {
	.lock = __SPIN_LOCK_UNLOCKED(xstat_group0_ctx),
	.user = NULL,
	.msgid = 0,
	.nctxs = sizeof(xstat_sensor_ctxs) / sizeof(xstat_sensor_ctxs[0]),
	.ctxs = xstat_sensor_ctxs,
};
static struct xstat_counter xstat_ipmi_cnts[] = {
	__IPMI_CNT(xstat_group0_ctx),
};
#endif
#ifdef XSTAT_CHAMELEON
static struct ipmi_sensor_ctx cham_groupA_ctxs[] = {
	__IPMI_CTX(Fan1A, 48, 120),
	__IPMI_CTX(Fan2A, 50, 120),
	__IPMI_CTX(Fan3A, 52, 120),
	__IPMI_CTX(Fan4A, 54, 120),
	__IPMI_CTX(Fan5A, 56, 120),
	__IPMI_CTX(Fan6A, 58, 120),
	__IPMI_CTX(Fan7A, 60, 120),
};
static struct ipmi_sensor_ctx cham_groupB_ctxs[] = {
	__IPMI_CTX(Fan1B, 49, 120),
	__IPMI_CTX(Fan2B, 51, 120),
	__IPMI_CTX(Fan3B, 53, 120),
	__IPMI_CTX(Fan4B, 55, 120),
	__IPMI_CTX(Fan5B, 57, 120),
	__IPMI_CTX(Fan6B, 58, 120),
	__IPMI_CTX(Fan7B, 61, 120),
};
static struct ipmi_sensors_ctx cham_groupA_ctx = {
	.lock = __SPIN_LOCK_UNLOCKED(cham_groupA_ctx.lock),
	.user = NULL,
	.msgid = 0,
	.nctxs = sizeof(cham_groupA_ctxs) / sizeof(cham_groupA_ctxs[0]),
	.ctxs = cham_groupA_ctxs,
};
static struct ipmi_sensors_ctx cham_groupB_ctx = {
	.lock = __SPIN_LOCK_UNLOCKED(cham_groupB_ctx.lock),
	.user = NULL,
	.msgid = 0,
	.nctxs = sizeof(cham_groupB_ctxs) / sizeof(cham_groupB_ctxs[0]),
	.ctxs = cham_groupB_ctxs,
};
static struct xstat_counter xstat_ipmi_cnts[] = {
	__IPMI_CNT(cham_groupA_ctx),
	__IPMI_CNT(cham_groupB_ctx),
};
#endif

#define N_IPMI_CNTS	(sizeof(xstat_ipmi_cnts) / sizeof(xstat_ipmi_cnts[0]))
#define XSTAT_IPMI_MAGIC 0x7017beef

static struct ipmi_user_hndl xstat_ipmi_hndl;

static void xstat_register_bmc(int intf, struct device *dev) {
	int err, i;
	struct ipmi_smi_info smi_data;
	struct ipmi_sensors_ctx *ctx;

	err = ipmi_get_smi_info(intf, &smi_data);
	for (i = 0; i < N_IPMI_CNTS; i++) {
		ctx = (struct ipmi_sensors_ctx *) xstat_ipmi_cnts[i].data;
		if (!ctx->user)
			err = ipmi_create_user(intf, &xstat_ipmi_hndl, ctx, &ctx->user);
	}
}

static void xstat_ipmi_msg_handler(struct ipmi_recv_msg *msg, void *user_msg_data) {
	struct ipmi_sensors_ctx *root_ctx = (struct ipmi_sensors_ctx *) user_msg_data;
	struct ipmi_sensor_ctx *ctx;
	if (msg->user_msg_data && msg->user_msg_data >= (void *) root_ctx->ctxs
			               && msg->user_msg_data < (void *) (root_ctx->ctxs + root_ctx->nctxs)) {
		ctx = (struct ipmi_sensor_ctx *) msg->user_msg_data;
		if (msg->msg.data_len > 2) {
			ctx->sensor_reading = msg->msg.data[1];
		}
	} else {
		printk(KERN_INFO "xstat:recv invalid ipmi msg: %016llx.\n", (uint64_t) msg->user_msg_data);
	}
}

static void xstat_bmc_gone(int intf) {}

static struct ipmi_addr xstat_ipmi_address;
static struct ipmi_smi_watcher xstat_ipmi_watcher = {
	.owner = THIS_MODULE,
	.new_smi = xstat_register_bmc,
	.smi_gone = xstat_bmc_gone,
};
static struct ipmi_user_hndl xstat_ipmi_hndl = {
	.ipmi_recv_hndl = xstat_ipmi_msg_handler,
};

static int xstat_ipmi_cnt_init(const struct cpumask *mask, void *data, void **ctx) {
	*ctx = data;
	return 0;
}

static uint64_t xstat_ipmi_restart(void **_ctx, uint64_t last) {
	struct ipmi_sensors_ctx *ctx = (struct ipmi_sensors_ctx *) *_ctx;
	ipmi_user_t user;
	struct kernel_ipmi_msg msg;
	int i, err, msgid;
	struct ipmi_sensor_ctx *sctx;
	uint64_t ret = 0;
	user = ctx->user;
	msg.netfn = 0x04;
	msg.cmd = 0x2d;
	msg.data_len = 1;
	for (i = ctx->nctxs - 1; i >= 0; i--) {
		if (i != ctx->nctxs - 1) ret <<= 8;
		sctx = &ctx->ctxs[i];
		if (user) {
			msg.data = &sctx->config.sensor_number;
			spin_lock(&ctx->lock);
			msgid = ctx->msgid++;
			spin_unlock(&ctx->lock);
			err = ipmi_request_settime(user, &xstat_ipmi_address, msgid, &msg, sctx, 0 ,0, 0);
		}
		ret |= sctx->sensor_reading;
	}
	return ret;
}

static int xstat_ipmi_scnprintf(char *buf, int limit, uint64_t data, void **_ctx) {
	struct ipmi_sensors_ctx *ctx = (struct ipmi_sensors_ctx *) *_ctx;
	int val;
	int i;
	int ret;
	char *ptr = buf;
	for (i = 0; i < ctx->nctxs; i++) {
		val = data & 0xff;
		data >>= 8;
		val *= ctx->ctxs[i].config.mul;
		if (i == 0) {
			ret = scnprintf(ptr, limit, "\"%s\":%d", ctx->ctxs[i].config.name, val);
			ptr += ret;
			limit -= ret;
		} else {
			ret = scnprintf(ptr, limit, ",\"%s\":%d", ctx->ctxs[i].config.name, val);
			ptr += ret;
			limit -= ret;
		}
	}
	return ptr - buf;
}

static int xstat_ipmi_init(void) {
	xstat_ipmi_address.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
	xstat_ipmi_address.channel = IPMI_BMC_CHANNEL;
	xstat_ipmi_address.data[0] = 0;
	ipmi_smi_watcher_register(&xstat_ipmi_watcher);
	return 0;
}

static void xstat_ipmi_exit(void) {
	int i;
	ipmi_user_t user = NULL;
	struct ipmi_sensors_ctx *ctx;
	for (i = 0; i < N_IPMI_CNTS; i++) {
		ctx = (struct ipmi_sensors_ctx *) xstat_ipmi_cnts[i].data;
		spin_lock(&ctx->lock);
		user = ctx->user;
		ctx->user = 0;
		spin_unlock(&ctx->lock);
		if (user) ipmi_destroy_user(user);
	}
	ipmi_smi_watcher_unregister(&xstat_ipmi_watcher);
}


