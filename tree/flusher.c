/*
 * Copyright (c) 2012-2015 The nessDB Project Developers. All rights reserved.
 * Code is licensed with BSD.
 *
 */

#include "u.h"
#include "c.h"
#include "t.h"

// 遍历nmb，将里面的数据刷到child中
void _flush_buffer_to_child(struct node *child, struct nmb *buf)
{
	struct mb_iter iter;

	mb_iter_init(&iter, buf->pma);
	while (mb_iter_next(&iter)) {
		/* TODO(BohuTANG): check msn */
		struct nmb_values nvalues;

		nmb_get_values(&iter, &nvalues);

		struct bt_cmd cmd = {
			.msn = nvalues.msn,
			.type = nvalues.type,
			.key = &nvalues.key,
			.val = &nvalues.val,
			.xidpair = nvalues.xidpair
		};
		child->i->put(child, &cmd);
	}
}

void _flush_some_child(struct buftree *t, struct node *parent);

/*
 * PROCESS:
 *	- check child reactivity
 *	- if FISSIBLE: split child
 *	- if FLUSHBLE: flush buffer from child
 * ENTER:
 *	- parent is already locked
 *	- child is already locked
 * EXIT:
 *	- parent is unlocked
 *	- no nodes are locked
 */
void _child_maybe_reactivity(struct buftree *t, struct node *parent, struct node *child)
{
	enum node_state state = get_node_state(child);

	switch (state) {
	case STABLE:
		cache_unpin(t->cf, child->cpair);
		cache_unpin(t->cf, parent->cpair);
		break;
	case FISSIBLE:
		node_split_child(t, parent, child);
		cache_unpin(t->cf, child->cpair);
		cache_unpin(t->cf, parent->cpair);
		break;
	case FLUSHBLE:
		cache_unpin(t->cf, parent->cpair);
		_flush_some_child(t, child);
		break;
	}
}

/*
 * PROCESS:
 *	- pick a heaviest child of parent
 *	- flush from parent to child
 *	- maybe split/flush child recursively
 * ENTER:
 *	- parent is already locked
 * EXIT:
 *	- parent is unlocked
 *	- no nodes are locked
 */
void _flush_some_child(struct buftree *t, struct node *parent)
{
	int childnum;
	enum node_state state;
	struct node *child;
	struct partition *part;
	struct nmb *buffer;

	childnum = parent->i->find_heaviest(parent);
	nassert(childnum < parent->n_children);
	part = &parent->parts[childnum];
	buffer = part->msgbuf;
	if (cache_get_and_pin(t->cf, part->child_nid, (void**)&child, L_WRITE) != NESS_OK) {
		__ERROR("cache get node error, nid [%" PRIu64 "]", part->child_nid);
		return;
	}

	state = get_node_state(child);
	if (state == STABLE) {
		node_set_dirty(parent);
		part->msgbuf = nmb_new(t->hdr->opts);
		_flush_buffer_to_child(child, buffer);
		nmb_free(buffer);
	}

	_child_maybe_reactivity(t, parent, child);
}

/*
 * EFFECT:
 *	- do flush in a background thread
 * PROCESS:
 *	- if buf is NULL, we will do _flush_some_child
 *	- if buf is NOT NULL, we will do _flush_buffer_to_child
 * ENTER:
 *	- fe->node is already locked
 * EXIT:
 *	- nodes are unlocked
 */
static void _flush_node_func(void *fe)
{
	enum node_state state;
	struct flusher_extra *extra = (struct flusher_extra*)fe;
	struct buftree *t = extra->tree;
	struct node *n = extra->node;
	struct nmb *buf = extra->buffer;

	node_set_dirty(n);
	if (buf) {
    // 如果buf不为空
    // 那么就将buf刷到子节点中
		_flush_buffer_to_child(n, buf);
		nmb_free(buf);

		/* check the child node */
    // 检查节点状态状态
		state = get_node_state(n);
		if (state == FLUSHBLE)
      // 递归刷新子节点
			_flush_some_child(t, n);
		else
			cache_unpin(t->cf, n->cpair);
	} else {
		/* we want flush some buffer from n */
    // 递归刷新子节点
		_flush_some_child(t, n);
	}

	xfree(extra);
}

/*
 * add work to background thread (non-block)
 */
static void _place_node_and_buffer_on_background(struct buftree *t, struct node *node, struct nmb *buffer)
{
	struct flusher_extra *extra = xmalloc(sizeof(*extra));

	extra->tree = t;
	extra->node = node;
	extra->buffer = buffer;

	kibbutz_enq(t->cf->cache->c_kibbutz, _flush_node_func, extra);
}

/*
 * EFFECT:
 *	- flush in background thread
 * ENTER:
 *	- parent is already locked
 * EXIT:
 *	- nodes are all unlocked
 */
void buftree_flush_node_on_background(struct buftree *t, struct node *parent)
{
	int childnum;
	enum node_state state;
	struct node *child;
	struct partition *part;

	nassert(parent->height > 0);
  // 首先找到size最大的子节点
	childnum = parent->i->find_heaviest(parent);
	part = &parent->parts[childnum];

	/* pin the child */
  // 锁住这个子节点，同时根据child_nid拿到child节点指针返回
	if (cache_get_and_pin(t->cf, part->child_nid, (void**)&child, L_WRITE) != NESS_OK) {
		__ERROR("cache get node error, nid [%" PRIu64 "]", part->child_nid);
		return;
	}

	state = get_node_state(child);
	if (state == STABLE) {
    // 如果当前是稳定状态，那么将这个child的buffer取下来
		/* detach buffer from parent */
		struct nmb *buf = part->msgbuf;
    // 设置父节点是脏节点
		node_set_dirty(parent);
    // 重新分配一个新的buffer给子节点
		part->msgbuf = nmb_new(t->hdr->opts);

		/* flush it in background thread */
    // 放到后台线程中刷到磁盘里
		_place_node_and_buffer_on_background(t, child, buf);
		cache_unpin(t->cf, parent->cpair);
	} else {
		/* the child is reactive, we deal it in main thread */
		_child_maybe_reactivity(t, parent, child);
	}
}
