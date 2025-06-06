/*
 * Copyright 2018-2019 Huawei Technologies Co.,Ltd.All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <unistd.h>

#include "v1/wd_util.h"
#include "v1/wd_adapter.h"
#include "v1/wd.h"

#define LINUX_DEV_DIR	"/dev"
#define WD_UACCE_CLASS_DIR		"/sys/class/"WD_UACCE_CLASS_NAME
#define _TRY_REQUEST_TIMES		64
#define INT_MAX_SIZE			10
#define LINUX_CRTDIR_SIZE		1
#define LINUX_PRTDIR_SIZE		2
#define INSTANCE_RATIO_FOR_DEV_SCHED	4

#define GET_WEIGHT(distance, instances) (\
		((instances) & 0xffff) | (((distance) & 0xffff) << 16))
#define GET_NODE_DISTANCE(weight) (((weight) >> 16) & 0xffff)
#define GET_AVAILABLE_INSTANCES(weight) ((weight) & 0xffff)

#ifdef WITH_LOG_FILE
FILE * flog_fd = NULL;
#endif

wd_log log_out = NULL;

#define container_of(ptr, type, member) ({ \
		typeof(((type *)0)->member)(*__mptr) = (ptr); \
		(type *)((char *)__mptr - offsetof(type, member)); })

struct dev_info {
	int node_id;
	int numa_dis;
	int flags;
	int ref;
	int available_instances;
	int iommu_type;
	unsigned int weight;
	char dev_root[PATH_STR_SIZE];
	char name[WD_NAME_SIZE];
	char api[WD_NAME_SIZE];
	char algs[MAX_ATTR_STR_SIZE];
	unsigned long qfrs_offset[WD_UACCE_QFRT_MAX];
};

static int get_raw_attr(const char *dev_root, const char *attr,
							char *buf, size_t sz)
{
	char attr_file[PATH_STR_SIZE];
	char attr_path[PATH_MAX];
	char *ptrRet = NULL;
	int fd, size;

	size = snprintf(attr_file, PATH_STR_SIZE, "%s/%s",
			dev_root, attr);
	if (size <= 0) {
		WD_ERR("get %s/%s path fail!\n", dev_root, attr);
		return -WD_EINVAL;
	}

	ptrRet = realpath(attr_file, attr_path);
	if (ptrRet == NULL)
		return -WD_ENODEV;

	/*
	 * The attr_file = "/sys/class/uacce/xxx"
	 * It's the Internal Definition File Node
	 */
	fd = open(attr_path, O_RDONLY, 0);
	if (fd < 0) {
		WD_ERR("open %s fail, errno = %d!\n", attr_path, errno);
		return -WD_ENODEV;
	}
	size = read(fd, buf, sz);
	if (size <= 0) {
		WD_ERR("read nothing at %s!\n", attr_path);
		size = -WD_ENODEV;
	}

	close(fd);
	return size;
}

static int get_int_attr(struct dev_info *dinfo, const char *attr)
{
	char buf[MAX_ATTR_STR_SIZE] = {'\0'};
	int ret;

	ret = get_raw_attr(dinfo->dev_root, attr, buf, MAX_ATTR_STR_SIZE - 1);
	if (ret < 0)
		return ret;

	ret = strtol(buf, NULL, 10);
	if (errno == ERANGE) {
		WD_ERR("failed to strtol %s, out of range!\n", buf);
		return -errno;
	}

	return ret;
}

/*
 * Get string from an attr of sysfs. '\n' is used as a token of substring.
 * So '\n' could be in the middle of the string or at the last of the string.
 * Now remove the token '\n' at the end of the string to avoid confusion.
 */
static int get_str_attr(struct dev_info *dinfo, const char *attr, char *buf,
			size_t buf_sz)
{
	int ret;

	ret = get_raw_attr(dinfo->dev_root, attr, buf, buf_sz);
	if (ret < 0) {
		buf[0] = '\0';
		return ret;
	}

	if ((__u32)ret == buf_sz)
		ret = ret - 1;

	buf[ret] = '\0';
	while ((ret > 1) && (buf[ret - 1] == '\n')) {
		buf[ret - 1] = '\0';
		ret = ret - 1;
	}
	return ret;
}

static int get_ul_vec_attr(struct dev_info *dinfo, const char *attr,
			   unsigned long *vec, int vec_sz)
{
	char buf[MAX_ATTR_STR_SIZE];
	int size, i, j;
	char *begin, *end;

	size = get_raw_attr(dinfo->dev_root, attr, buf, MAX_ATTR_STR_SIZE);
	if (size < 0 || size >= MAX_ATTR_STR_SIZE) {
		for (i = 0; i < vec_sz; i++)
			vec[i] = 0;
		return size;
	}

	/*
	 * The unsigned long int max number is ULLONG_MAX 20bit
	 * char "18446744073709551615" When the value is
	 * bigger than ULLONG_MAX, It returns ULLONG_MAX
	 */
	buf[size] = '\0';
	begin = buf;
	for (i = 0; i < vec_sz; i++) {
		vec[i] = strtoul(begin, &end, 10);
		if (!end)
			break;
		begin = end;
	}

	for (j = i; j < vec_sz; j++)
		vec[j] = 0;

	return 0;
}

static bool is_alg_support(struct dev_info *dinfo, const char *alg)
{
	char *alg_save = NULL;
	char *alg_tmp;

	if (!alg)
		return false;

	alg_tmp = strtok_r(dinfo->algs, "\n", &alg_save);
	while (alg_tmp != NULL) {
		if (!strcmp(alg_tmp, alg))
			return true;

		alg_tmp = strtok_r(NULL, "\n", &alg_save);
	}

	return false;
}

static bool is_weight_more(unsigned int new, unsigned int old)
{
	unsigned int ins_new, dis_new;
	unsigned int ins_old, dis_old;

	ins_new = GET_AVAILABLE_INSTANCES(new);
	dis_new = GET_NODE_DISTANCE(new);
	ins_old = GET_AVAILABLE_INSTANCES(old);
	dis_old = GET_NODE_DISTANCE(old);

	dbg("dis_new %u, ins_new %u, dis_old %u, ins_old %u\n",
		dis_new, ins_new, dis_old, ins_old);

	if (dis_new > dis_old)
		return ins_new > ins_old * INSTANCE_RATIO_FOR_DEV_SCHED;
	else if (dis_new == dis_old)
		return ins_new > ins_old;
	else
		return ins_new * INSTANCE_RATIO_FOR_DEV_SCHED >= ins_old;
}

static void get_iommu_type(struct dev_info *dinfo)
{
	if ((unsigned int)dinfo->flags & WD_UACCE_DEV_IOMMU)
		dinfo->iommu_type = 1;
	else
		dinfo->iommu_type = 0;
}

static int get_int_attr_all(struct dev_info *dinfo)
{
	int ret;

	/* ret == 1 means device has been isolated */
	ret = get_int_attr(dinfo, "isolate");
	if (ret < 0)
		return -WD_ENODEV;
	else if (ret == 1)
		return -WD_EBUSY;

	/* ret == 0 means device has no available queues */
	ret = get_int_attr(dinfo, "available_instances");
	if (ret < 0)
		return -WD_ENODEV;
	else if (ret == 0)
		return -WD_EBUSY;

	dinfo->available_instances = ret;

	ret = get_int_attr(dinfo, "numa_distance");
	if (ret < 0)
		return ret;
	dinfo->numa_dis = ret;

	dinfo->node_id = get_int_attr(dinfo, "node_id");

	ret = get_int_attr(dinfo, "flags");
	if (ret < 0)
		return ret;
	else if ((unsigned int)ret & WD_UACCE_DEV_SVA)
		return -ENODEV;

	dinfo->flags = ret;

	return 0;
}

static int get_str_attr_all(struct dev_info *dinfo, const char *alg)
{
	int ret;

	ret = get_str_attr(dinfo, "algorithms",
			    dinfo->algs, MAX_ATTR_STR_SIZE);
	if (ret < 0)
		return ret;

	/* Add algorithm check to cut later pointless logic */
	ret = is_alg_support(dinfo, alg);
	if (!ret)
		return -EPFNOSUPPORT;

	ret = get_str_attr(dinfo, "api", dinfo->api, WD_NAME_SIZE);
	if (ret < 0)
		return ret;

	return 0;
}

static int get_ul_vec_attr_all(struct dev_info *dinfo)
{
	int ret;

	ret = get_ul_vec_attr(dinfo, "region_mmio_size",
		&dinfo->qfrs_offset[WD_UACCE_QFRT_MMIO], 1);
	if (ret < 0)
		return ret;

	ret = get_ul_vec_attr(dinfo, "region_dus_size",
		&dinfo->qfrs_offset[WD_UACCE_QFRT_DUS], 1);
	if (ret < 0)
		return ret;

	return 0;
}

static int get_dev_info(struct dev_info *dinfo, const char *alg)
{
	char buf[PATH_STR_SIZE] = {0};
	int ret;

	ret = snprintf(buf, PATH_STR_SIZE, "%s/%s",
						LINUX_DEV_DIR, dinfo->name);
	if (ret <= 0) {
		WD_ERR("snprintf err, ret %d!\n", ret);
		return -WD_EINVAL;
	}

	ret = access(buf, F_OK);
	if (ret < 0) {
		WD_ERR("failed to check file path %s, ret: %d\n", buf, ret);
		return -WD_ENODEV;
	}

	ret = get_str_attr_all(dinfo, alg);
	if (ret)
		return ret;

	ret = get_int_attr_all(dinfo);
	if (ret)
		return ret;

	ret = get_ul_vec_attr_all(dinfo);
	if (ret)
		return ret;

	get_iommu_type(dinfo);
	/*
	 * Use available_instances and numa_distance combine weight.
	 * |	2 bytes	|	2bytes	|.
	 * |numa_distance|available_instances|.
	 */
	dinfo->weight = GET_WEIGHT((__u32)dinfo->numa_dis,
		(__u32)dinfo->available_instances);

	return 0;
}

static bool copy_if_better(struct dev_info *old, struct dev_info *new,
			   unsigned int node_mask)
{
	bool find_node = false;

	dbg("try accelerator %s (inst_num=%d, node_id=%d)...\n", new->name,
	    new->available_instances, new->node_id);

	if (new->node_id >= 0 &&
		((1 << (unsigned int)new->node_id) & node_mask))
		find_node = true;

	if (old && (!old->name[0] || find_node ||
		is_weight_more(new->weight, old->weight))) {
		memcpy(old, new, sizeof(*old));
		dbg("adopted\n");
	}

	return find_node;
}

static void pre_init_dev(struct dev_info *dinfo, const char *name)
{
	int ret;

	ret = snprintf(dinfo->name, WD_NAME_SIZE, "%s", name);
	if (ret < 0) {
		WD_ERR("get file name fail!\n");
		return;
	}

	/* check the "attrs" file directory exists */
	ret = snprintf(dinfo->dev_root, PATH_STR_SIZE,
		       "%s/%s/attrs", WD_UACCE_CLASS_DIR, name);
	if (ret < 0) {
		WD_ERR("failed to copy dev attrs file path!\n");
		return;
	}

	ret = access(dinfo->dev_root, F_OK);
	if (ret < 0) {
		ret = snprintf(dinfo->dev_root, PATH_STR_SIZE,
			       "%s/%s", WD_UACCE_CLASS_DIR, name);
		if (ret < 0)
			WD_ERR("failed to copy dev file path!\n");
		return;
	}
}

static int get_denoted_dev(struct wd_capa *capa, const char *dev,
				struct dev_info *dinfop)
{
	pre_init_dev(dinfop, dev);
	if (!get_dev_info(dinfop, capa->alg))
		return 0;
	WD_ERR("%s not available, will try other devices\n", dev);
	return -WD_ENODEV;
}

static int find_available_dev(struct dev_info *dinfop,
				  struct wd_capa *capa,
				  unsigned int node_mask)
{
	struct dirent *device;
	DIR *wd_class = NULL;
	bool find_node = false;
	struct dev_info dinfo;
	char *name;
	int cnt = 0;
	int ret;

	wd_class = opendir(WD_UACCE_CLASS_DIR);
	if (!wd_class) {
		WD_ERR("WD framework is not enabled on the system, errno = %d!\n", errno);
		return -WD_ENODEV;
	}

	while (true) {
		device = readdir(wd_class);
		if (!device)
			break;
		name = device->d_name;
		if (!strncmp(name, ".", LINUX_CRTDIR_SIZE) ||
			!strncmp(name, "..", LINUX_PRTDIR_SIZE))
			continue;
		pre_init_dev(&dinfo, name);
		ret = get_dev_info(&dinfo, capa->alg);
		if (!ret) {
			cnt++;
			if (copy_if_better(dinfop, &dinfo, node_mask)) {
				find_node = true;
				break;
			}
		} else if (ret == -EPFNOSUPPORT || ret == -WD_EBUSY || ret == -WD_ENODEV) {
			continue;
		} else {
			closedir(wd_class);
			return ret;
		}
	}

	if (node_mask && !find_node)
		WD_ERR("Device not available on nodemask 0x%x!\n", node_mask);

	closedir(wd_class);
	return cnt;
}

static int find_available_res(struct wd_queue *q, struct dev_info *dinfop,
						int *num)
{
	struct wd_capa *capa = &q->capa;
	const char *dev = q->dev_path;
	int ret;

	/* As user denotes a device */
	if (dev[0] && dev[0] != '/' && !strstr(dev, "../")) {
		if (!dinfop) {
			WD_ERR("dinfop NULL!\n");
			return -WD_EINVAL;
		}

		if (q->node_mask) {
			WD_ERR("dev and node cannot be denoted together!\n");
			return -WD_EINVAL;
		}

		if (!get_denoted_dev(capa, dev, dinfop))
			goto dev_path;
	}

	ret = find_available_dev(dinfop, capa, q->node_mask);
	if (ret <= 0 && dinfop) {
		WD_ERR("get /%s path fail!\n", dinfop->name);
		return -WD_ENODEV;
	}

	if (num) {
		*num = ret;
		return 0;
	}

dev_path:
	if (!dinfop) {
		WD_ERR("dinfop NULL!\n");
		return -WD_EINVAL;
	}

	ret = snprintf(q->dev_path, PATH_STR_SIZE, "%s/%s",
						LINUX_DEV_DIR, dinfop->name);
	if (ret <= 0) {
		WD_ERR("snprintf err, ret %d!\n", ret);
		return -WD_EINVAL;
	}
	return 0;
}

static int get_queue_from_dev(struct wd_queue *q, const struct dev_info *dev)
{
	char q_path[PATH_MAX];
	char *ptrRet = NULL;
	struct q_info *qinfo;

	qinfo = q->qinfo;
	ptrRet = realpath(q->dev_path, q_path);
	if (ptrRet == NULL)
		return -WD_ENODEV;

	qinfo->fd = open(q_path, O_RDWR | O_CLOEXEC);
	if (qinfo->fd == -1) {
		WD_ERR("open %s failed, errno = %d!\n", q_path, errno);
		return -WD_ENODEV;
	}

	qinfo->hw_type = dev->api;
	qinfo->dev_flags = dev->flags;
	qinfo->iommu_type = dev->iommu_type;
	qinfo->dev_info = dev;
	qinfo->head = &qinfo->ss_list;
	__atomic_clear(&qinfo->ref, __ATOMIC_RELEASE);
	TAILQ_INIT(&qinfo->ss_list);
	memcpy(qinfo->qfrs_offset, dev->qfrs_offset,
				sizeof(qinfo->qfrs_offset));

	return 0;
}

static int wd_start_queue(struct wd_queue *q)
{
	int ret;
	struct q_info *qinfo = q->qinfo;

	ret = ioctl(qinfo->fd, WD_UACCE_CMD_START_Q);
	if (ret)
		WD_ERR("failed to start queue of %s\n", q->dev_path);
	return ret;
}

static void wd_close_queue(struct wd_queue *q)
{
	struct q_info *qinfo = q->qinfo;

	close(qinfo->fd);
}

int wd_request_queue(struct wd_queue *q)
{
	struct dev_info *dinfop;
	int try_cnt = 0;
	int ret;

	if (!q) {
		WD_ERR("input parameter q is NULL!\n");
		return -WD_EINVAL;
	}

	dinfop = calloc(1, sizeof(struct q_info) + sizeof(struct dev_info));
	if (!dinfop) {
		WD_ERR("calloc for queue info fail!\n");
		return -WD_ENOMEM;
	};
	q->qinfo = dinfop + 1;

	do {
		ret = find_available_res(q, dinfop, NULL);
		if (ret) {
			WD_ERR("cannot find available device\n");
			goto err_with_dev;
		}

		ret = get_queue_from_dev(q, (const struct dev_info *)dinfop);
		if (!ret) {
			break;
		} else {
			if (try_cnt++ > _TRY_REQUEST_TIMES) {
				WD_ERR("fail to get queue!\n");
				goto err_with_dev;
			}

			memset(dinfop, 0, sizeof(*dinfop));
		}
	} while (true);

	ret = drv_open(q);
	if (ret) {
		WD_ERR("failed to initialize queue by driver!\n");
		goto err_with_fd;
	}

	ret = wd_start_queue(q);
	if (ret)
		goto err_with_drv_openned;
	return ret;

err_with_drv_openned:
	drv_close(q);
err_with_fd:
	wd_close_queue(q);
err_with_dev:
	free(dinfop);
	q->qinfo = NULL;
	return ret;
}

void wd_release_queue(struct wd_queue *q)
{
	struct wd_ss_region_list *head;
	struct q_info *qinfo, *sqinfo;

	if (!q || !q->qinfo) {
		WD_ERR("release queue parameter error!\n");
		return;
	}
	qinfo = q->qinfo;
	if (__atomic_load_n(&qinfo->ref, __ATOMIC_RELAXED)) {
		WD_ERR("q(%s) is busy, release fail!\n", q->capa.alg);
		return;
	}
	head = qinfo->head;
	sqinfo = container_of(head, struct q_info, ss_list);
	if (sqinfo != qinfo) /* q_share */
		__atomic_sub_fetch(&sqinfo->ref, 1, __ATOMIC_RELAXED);

	if (ioctl(qinfo->fd, WD_UACCE_CMD_PUT_Q))
		WD_ERR("failed to put queue!\n");

	drv_close(q);

	/* q_reserve */
	if (qinfo->ss_size)
		drv_unmap_reserve_mem(q, qinfo->ss_va, qinfo->ss_size);

	drv_free_slice(q);

	wd_close_queue(q);
	free((void *)qinfo->dev_info);
	q->qinfo = NULL;
}

int wd_send(struct wd_queue *q, void *req)
{
	if (unlikely(!q || !req)) {
		WD_ERR("wd send input parameter null!\n");
		return -WD_EINVAL;
	}
	return wd_burst_send(q, &req, 1);
}

int wd_recv(struct wd_queue *q, void **resp)
{
	if (unlikely(!q || !resp)) {
		WD_ERR("wd recv input parameter null!\n");
		return -WD_EINVAL;
	}
	return wd_burst_recv(q, resp, 1);
}

int wd_wait(struct wd_queue *q, __u16 ms)
{
	struct q_info *qinfo;
	struct wcrypto_paras *priv;
	struct pollfd fds[1];
	int ret;

	if (unlikely(!q))
		return -WD_EINVAL;

	priv = &q->capa.priv;
	if (unlikely(!priv->is_poll))
		return -WD_EINVAL;

	qinfo = q->qinfo;
	fds[0].fd = qinfo->fd;
	fds[0].events = POLLIN;

	ret = poll(fds, 1, ms);
	if (unlikely(ret < 0)) {
		WD_ERR("failed to poll a queue!\n");
		return -WD_ENODEV;
	}

	/* return 0 for no data, 1 for new message */
	return ret;
}

int wd_recv_sync(struct wd_queue *q, void **resp, __u16 ms)
{
	int ret;

	ret = wd_wait(q, ms);
	if (likely(ret > 0)) {
		ret = wd_recv(q, resp);
		if (unlikely(!ret))
			WD_ERR("failed to recv data after poll!\n");
	}

	return ret;
}

void *wd_reserve_memory(struct wd_queue *q, size_t size)
{
	if (!q || !size) {
		WD_ERR("wd reserve memory: parameter err!\n");
		return NULL;
	}

	return drv_reserve_mem(q, size);
}

int wd_share_reserved_memory(struct wd_queue *q,
			struct wd_queue *target_q)
{
	const struct dev_info *info, *tgt_info;
	struct q_info *qinfo, *tqinfo;
	int ret;

	if (!q || !target_q || !q->qinfo || !target_q->qinfo) {
		WD_ERR("wd share reserved memory: parameter err!\n");
		return -WD_EINVAL;
	}

	qinfo = q->qinfo;
	tqinfo = target_q->qinfo;
	tgt_info = tqinfo->dev_info;
	info = qinfo->dev_info;

	/* Just share DMA memory from 'q' in NO-IOMMU mode */
	if (qinfo->iommu_type) {
		WD_ERR("IOMMU opened, not support share mem!\n");
		return -WD_EINVAL;
	}

	if (qinfo->iommu_type != tqinfo->iommu_type) {
		WD_ERR("IOMMU type mismatching as share mem!\n");
		return -WD_EINVAL;
	}
	if (info->node_id != tgt_info->node_id)
		WD_ERR("Warn: the 2 queues is not at the same node!\n");

	ret = ioctl(qinfo->fd, WD_UACCE_CMD_SHARE_SVAS, tqinfo->fd);
	if (ret) {
		WD_ERR("ioctl share dma memory fail!\n");
		return ret;
	}

	tqinfo->head = qinfo->head;
	__atomic_add_fetch(&qinfo->ref, 1, __ATOMIC_RELAXED);

	return 0;
}

int wd_get_available_dev_num(const char *algorithm)
{
	struct wd_queue q;
	int num = -1;
	int ret;

	if (!algorithm) {
		WD_ERR("algorithm is null!\n");
		return -WD_EINVAL;
	}

	memset(&q, 0, sizeof(q));
	q.capa.alg = algorithm;
	q.dev_path[0] = 0;
	ret = find_available_res(&q, NULL, &num);
	if (ret < 0)
		WD_ERR("find_available_res err, ret = %d!\n", ret);
	return num;
}

int wd_get_node_id(struct wd_queue *q)
{
	const struct dev_info *dev = NULL;
	struct q_info *qinfo = NULL;

	if (!q || !q->qinfo || !((struct q_info *)(q->qinfo))->dev_info) {
		WD_ERR("q, info or dev_info NULL!\n");
		return -WD_EINVAL;
	}

	qinfo = q->qinfo;
	dev = qinfo->dev_info;

	return dev->node_id;
}

void *wd_iova_map(struct wd_queue *q, void *va, size_t sz)
{
	struct wd_ss_region *rgn;
	struct q_info *qinfo;

	if (!q || !va) {
		WD_ERR("wd iova map: parameter err!\n");
		return NULL;
	}

	qinfo = q->qinfo;

	TAILQ_FOREACH(rgn, qinfo->head, next) {
		if (rgn->va <= va && va < rgn->va + rgn->size)
			return (void *)(uintptr_t)(rgn->pa +
				((uintptr_t)va - (uintptr_t)rgn->va));
	}

	return NULL;
}

void wd_iova_unmap(struct wd_queue *q, void *va, void *dma, size_t sz)
{
	/* For no-iommu, dma-unmap doing nothing */
}

void *wd_dma_to_va(struct wd_queue *q, void *dma)
{
	struct wd_ss_region *rgn;
	struct q_info *qinfo;
	uintptr_t va;

	if (!q || !q->qinfo || !dma) {
		WD_ERR("wd dma to va, parameter err!\n");
		return NULL;
	}

	qinfo = q->qinfo;

	TAILQ_FOREACH(rgn, qinfo->head, next) {
		if (rgn->pa <= (uintptr_t)dma &&
			(uintptr_t)dma < rgn->pa + rgn->size) {
			va = (uintptr_t)dma - rgn->pa + (uintptr_t)rgn->va;
			return (void *)va;
		}
	}

	return NULL;
}

void *wd_drv_mmap_qfr(struct wd_queue *q, enum uacce_qfrt qfrt, size_t size)
{
	struct q_info *qinfo = q->qinfo;
	size_t tmp = size;
	off_t off;

	off = qfrt * getpagesize();

	if (qfrt != WD_UACCE_QFRT_SS)
		tmp = qinfo->qfrs_offset[qfrt];

	return mmap(0, tmp, PROT_READ | PROT_WRITE,
		    MAP_SHARED, qinfo->fd, off);
}

void wd_drv_unmmap_qfr(struct wd_queue *q, void *addr,
		       enum uacce_qfrt qfrt, size_t size)
{
	struct q_info *qinfo = q->qinfo;
	int ret;

	if (!addr)
		return;

	if (qfrt != WD_UACCE_QFRT_SS)
		ret = munmap(addr, qinfo->qfrs_offset[qfrt]);
	else
		ret = munmap(addr, size);

	if (ret)
		WD_ERR("wd qfr unmap failed!\n");
}

int wd_register_log(wd_log log)
{
	if (!log) {
		WD_ERR("input log is null!\n");
		return -WD_EINVAL;
	}

	if (log_out) {
		WD_ERR("can not duplicate register!\n");
		return -WD_EINVAL;
	}

	/*
	 * No exceptions are generated during concurrency.
	 * Users are required to ensure the order of configuration
	 * operations
	 */
	log_out = log;
	dbg("log register\n");

	return WD_SUCCESS;
}

const char *wd_get_drv(struct wd_queue *q)
{
	struct q_info *qinfo;
	const struct dev_info *dev;

	if (!q || !q->qinfo)
		return NULL;

	qinfo = q->qinfo;
	dev = qinfo->dev_info;

	return (const char *)dev->api;
}
