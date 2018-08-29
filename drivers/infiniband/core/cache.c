/*
 * Copyright (c) 2004 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Intel Corporation. All rights reserved.
 * Copyright (c) 2005 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2005 Voltaire, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define pr_fmt(fmt) "ib_cache: " fmt

#include <lego/errno.h>
#include <lego/slab.h>
#include <lego/workqueue.h>
#include <lego/net.h>

#include <rdma/ib_cache.h>

#include "core_priv.h"

static inline int start_port(struct ib_device *device)
{
	return (device->node_type == RDMA_NODE_IB_SWITCH) ? 0 : 1;
}

static inline int end_port(struct ib_device *device)
{
	return (device->node_type == RDMA_NODE_IB_SWITCH) ?
		0 : device->phys_port_cnt;
}

int ib_get_cached_gid(struct ib_device *device,
		      u8                port_num,
		      int               index,
		      union ib_gid     *gid)
{
	struct ib_gid_cache *cache;
	unsigned long flags;
	int ret = 0;

	if (port_num < start_port(device) || port_num > end_port(device))
		return -EINVAL;

	spin_lock_irqsave(&device->cache.lock, flags);

	cache = device->cache.gid_cache[port_num - start_port(device)];

	if (index < 0 || index >= cache->table_len)
		ret = -EINVAL;
	else
		*gid = cache->table[index];

	spin_unlock_irqrestore(&device->cache.lock, flags);

	return ret;
}

int ib_find_cached_gid(struct ib_device *device,
		       union ib_gid	*gid,
		       u8               *port_num,
		       u16              *index)
{
	struct ib_gid_cache *cache;
	unsigned long flags;
	int p, i;
	int ret = -ENOENT;

	*port_num = -1;
	if (index)
		*index = -1;

	spin_lock_irqsave(&device->cache.lock, flags);

	for (p = 0; p <= end_port(device) - start_port(device); ++p) {
		cache = device->cache.gid_cache[p];
		for (i = 0; i < cache->table_len; ++i) {
			if (!memcmp(gid, &cache->table[i], sizeof *gid)) {
				*port_num = p + start_port(device);
				if (index)
					*index = i;
				ret = 0;
				goto found;
			}
		}
	}
found:
	spin_unlock_irqrestore(&device->cache.lock, flags);

	return ret;
}

int ib_get_cached_pkey(struct ib_device *device,
		       u8                port_num,
		       int               index,
		       u16              *pkey)
{
	struct ib_pkey_cache *cache;
	unsigned long flags;
	int ret = 0;

	if (port_num < start_port(device) || port_num > end_port(device))
		return -EINVAL;

	spin_lock_irqsave(&device->cache.lock, flags);

	cache = device->cache.pkey_cache[port_num - start_port(device)];
	if (!cache) {
		pr_info("%s(): device: %p(%s) cache: %p port %d\n",
			__func__, device, dev_name(device->dma_device),
			&device->cache, port_num);
		BUG();
	}

	if (index < 0 || index >= cache->table_len)
		ret = -EINVAL;
	else
		*pkey = cache->table[index];

	spin_unlock_irqrestore(&device->cache.lock, flags);

	return ret;
}

int ib_find_cached_pkey(struct ib_device *device,
			u8                port_num,
			u16               pkey,
			u16              *index)
{
	struct ib_pkey_cache *cache;
	unsigned long flags;
	int i;
	int ret = -ENOENT;

	if (port_num < start_port(device) || port_num > end_port(device))
		return -EINVAL;

	spin_lock_irqsave(&device->cache.lock, flags);

	cache = device->cache.pkey_cache[port_num - start_port(device)];

	*index = -1;

	for (i = 0; i < cache->table_len; ++i)
		if ((cache->table[i] & 0x7fff) == (pkey & 0x7fff)) {
			*index = i;
			ret = 0;
			break;
		}

	spin_unlock_irqrestore(&device->cache.lock, flags);

	return ret;
}

int ib_get_cached_lmc(struct ib_device *device,
		      u8                port_num,
		      u8                *lmc)
{
	unsigned long flags;
	int ret = 0;

	if (port_num < start_port(device) || port_num > end_port(device))
		return -EINVAL;

	spin_lock_irqsave(&device->cache.lock, flags);
	*lmc = device->cache.lmc_cache[port_num - start_port(device)];
	spin_unlock_irqrestore(&device->cache.lock, flags);

	return ret;
}

static void ib_cache_update(struct ib_device *device,
			    u8                port)
{
	struct ib_port_attr       *tprops = NULL;
	struct ib_pkey_cache      *pkey_cache = NULL, *old_pkey_cache;
	struct ib_gid_cache       *gid_cache = NULL, *old_gid_cache;
	int                        i;
	int                        ret;

	tprops = kmalloc(sizeof *tprops, GFP_KERNEL);
	if (!tprops)
		return;

	ret = ib_query_port(device, port, tprops);
	if (ret) {
		printk(KERN_WARNING "ib_query_port failed (%d) for %s\n",
		       ret, device->name);
		goto err;
	}

	pkey_cache = kmalloc(sizeof *pkey_cache + tprops->pkey_tbl_len *
			     sizeof *pkey_cache->table, GFP_KERNEL);
	if (!pkey_cache) {
		pr_debug("%s(): pkey %zu %d %zu\n", __func__,
			sizeof *pkey_cache, tprops->pkey_tbl_len, sizeof *pkey_cache->table);
		goto err;
	}

	pkey_cache->table_len = tprops->pkey_tbl_len;

	gid_cache = kmalloc(sizeof *gid_cache + tprops->gid_tbl_len *
			    sizeof *gid_cache->table, GFP_KERNEL);
	if (!gid_cache) {
		pr_debug("%s(): gid %zu %d %zu\n", __func__,
			sizeof *gid_cache, tprops->gid_tbl_len, sizeof *gid_cache->table);
		goto err;
	}

	gid_cache->table_len = tprops->gid_tbl_len;

	for (i = 0; i < pkey_cache->table_len; ++i) {
		ret = ib_query_pkey(device, port, i, pkey_cache->table + i);
		if (ret) {
			printk(KERN_WARNING "ib_query_pkey failed (%d) for %s (index %d)\n",
			       ret, device->name, i);
			goto err;
		}
	}

	for (i = 0; i < gid_cache->table_len; ++i) {
		ret = ib_query_gid(device, port, i, gid_cache->table + i);
		if (ret) {
			printk(KERN_WARNING "ib_query_gid failed (%d) for %s (index %d)\n",
			       ret, device->name, i);
			goto err;
		}
	}

	spin_lock_irq(&device->cache.lock);

	old_pkey_cache = device->cache.pkey_cache[port - start_port(device)];
	old_gid_cache  = device->cache.gid_cache [port - start_port(device)];

	device->cache.pkey_cache[port - start_port(device)] = pkey_cache;
	device->cache.gid_cache [port - start_port(device)] = gid_cache;

	device->cache.lmc_cache[port - start_port(device)] = tprops->lmc;

	spin_unlock_irq(&device->cache.lock);

	pr_debug("%s(): Updated port %u of dev %s. pkey_cache[%d] (%p) = %p gid_cache[%d] (%p) = %p\n",
		__func__, port, dev_name(device->dma_device),
		port - start_port(device), &(device->cache.pkey_cache[port - start_port(device)]), pkey_cache,
		port - start_port(device), &(device->cache.gid_cache [port - start_port(device)]), gid_cache);

	if (old_pkey_cache)
		kfree(old_pkey_cache);
	if (old_gid_cache)
		kfree(old_gid_cache);
	if (tprops)
		kfree(tprops);
	return;

err:
	pr_debug("%s(): Fail to update port %u of dev %s\n",
		__func__, port, dev_name(device->dma_device));
	if (old_pkey_cache)
		kfree(old_pkey_cache);
	if (old_gid_cache)
		kfree(old_gid_cache);
	if (tprops)
		kfree(tprops);
}

static void ib_cache_event(struct ib_event_handler *handler,
			   struct ib_event *event)
{
	pr_info("%s(): Got one event\n", __func__);
	if (event->event == IB_EVENT_PORT_ERR    ||
	    event->event == IB_EVENT_PORT_ACTIVE ||
	    event->event == IB_EVENT_LID_CHANGE  ||
	    event->event == IB_EVENT_PKEY_CHANGE ||
	    event->event == IB_EVENT_SM_CHANGE   ||
	    event->event == IB_EVENT_CLIENT_REREGISTER ||
	    event->event == IB_EVENT_GID_CHANGE) {
	    	/*
		 * HACK!!!
		 *
		 * We do it differently. Because at the time of porting
		 * we don't have workqueue. Instead, we just use this
		 * thread to do the dirty work:
		 */
		ib_cache_update(event->device, event->element.port_num);
	}
}

static void ib_cache_setup_one(struct ib_device *device)
{
	int p;

	pr_info("%s(): dev: %s\n", __func__, dev_name(device->dma_device));

	spin_lock_init(&device->cache.lock);

	device->cache.pkey_cache =
		kmalloc(sizeof *device->cache.pkey_cache *
			(end_port(device) - start_port(device) + 1), GFP_KERNEL);
	device->cache.gid_cache =
		kmalloc(sizeof *device->cache.gid_cache *
			(end_port(device) - start_port(device) + 1), GFP_KERNEL);

	device->cache.lmc_cache = kmalloc(sizeof *device->cache.lmc_cache *
					  (end_port(device) -
					   start_port(device) + 1),
					  GFP_KERNEL);

	if (!device->cache.pkey_cache || !device->cache.gid_cache ||
	    !device->cache.lmc_cache) {
		printk(KERN_WARNING "Couldn't allocate cache "
		       "for %s\n", device->name);
		goto err;
	}

	for (p = 0; p <= end_port(device) - start_port(device); ++p) {
		device->cache.pkey_cache[p] = NULL;
		device->cache.gid_cache [p] = NULL;
		ib_cache_update(device, p + start_port(device));
	}

	INIT_IB_EVENT_HANDLER(&device->cache.event_handler,
			      device, ib_cache_event);
	if (ib_register_event_handler(&device->cache.event_handler))
		goto err_cache;

	return;

err_cache:
	for (p = 0; p <= end_port(device) - start_port(device); ++p) {
		kfree(device->cache.pkey_cache[p]);
		kfree(device->cache.gid_cache[p]);
	}

err:
	kfree(device->cache.pkey_cache);
	kfree(device->cache.gid_cache);
	kfree(device->cache.lmc_cache);
}

static void ib_cache_cleanup_one(struct ib_device *device)
{
	int p;

	ib_unregister_event_handler(&device->cache.event_handler);

	for (p = 0; p <= end_port(device) - start_port(device); ++p) {
		kfree(device->cache.pkey_cache[p]);
		kfree(device->cache.gid_cache[p]);
	}

	kfree(device->cache.pkey_cache);
	kfree(device->cache.gid_cache);
	kfree(device->cache.lmc_cache);
}

static struct ib_client cache_client = {
	.name   = "cache",
	.add    = ib_cache_setup_one,
	.remove = ib_cache_cleanup_one
};

int __init ib_cache_setup(void)
{
	return ib_register_client(&cache_client);
}

void ib_cache_cleanup(void)
{
	ib_unregister_client(&cache_client);
}
