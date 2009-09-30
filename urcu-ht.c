
/*
 * TODO: keys are currently assumed <= sizeof(void *). Key target never freed.
 */

#define _LGPL_SOURCE
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>

#include <urcu.h>
#include <urcu-defer.h>
#include <arch.h>
#include <arch_atomic.h>
#include <compiler.h>
#include <urcu/jhash.h>
#include <urcu-ht.h>

/* node flags */
#define	NODE_STOLEN	(1 << 0)

struct rcu_ht_node;

struct rcu_ht_node {
	struct rcu_ht_node *next;
	void *key;
	void *data;
	unsigned int flags;
};

struct rcu_ht {
	struct rcu_ht_node **tbl;
	ht_hash_fct hash_fct;
	void (*free_fct)(void *data);	/* fct to free data */
	uint32_t keylen;
	uint32_t hashseed;
	struct ht_size {
		unsigned long add;
		unsigned long lookup;
	} size;
};

struct rcu_ht *ht_new(ht_hash_fct hash_fct, void (*free_fct)(void *data),
		      unsigned long init_size, uint32_t keylen,
		      uint32_t hashseed)
{
	struct rcu_ht *ht;

	ht = calloc(1, sizeof(struct rcu_ht));
	ht->hash_fct = hash_fct;
	ht->free_fct = free_fct;
	ht->size.add = init_size;
	ht->size.lookup = init_size;
	ht->keylen = keylen;
	ht->hashseed = hashseed;
	ht->tbl = calloc(init_size, sizeof(struct rcu_ht_node *));
	return ht;
}

void *ht_lookup(struct rcu_ht *ht, void *key)
{
	unsigned long hash;
	struct rcu_ht_node *node;
	void *ret;

	hash = ht->hash_fct(key, ht->keylen, ht->hashseed) % ht->size.lookup;

	rcu_read_lock();
	node = rcu_dereference(ht->tbl[hash]);
	for (;;) {
		if (likely(!node)) {
			ret = NULL;
			break;
		}
		if (node->key == key) {
			ret = node->data;
			break;
		}
		node = rcu_dereference(node->next);
	}
	rcu_read_unlock();

	return ret;
}

/*
 * Will re-try until either:
 * - The key is already there (-EEXIST)
 * - We successfully add the key at the head of a table bucket.
 */
int ht_add(struct rcu_ht *ht, void *key, void *data)
{
	struct rcu_ht_node *node, *old_head, *new_head;
	unsigned long hash;
	int ret = 0;

	new_head = calloc(1, sizeof(struct rcu_ht_node));
	new_head->key = key;
	new_head->data = data;
	new_head->flags = 0;
	/* here comes the fun and tricky part.
	 * Add at the beginning with a cmpxchg.
	 * Hold a read lock between the moment the first element is read
	 * and the nodes traversal (to find duplicates). This ensures
	 * the head pointer has not been reclaimed when cmpxchg is done.
	 * Always adding at the head ensures that we would have to
	 * re-try if a new item has been added concurrently. So we ensure that
	 * we never add duplicates. */
retry:
	rcu_read_lock();

	hash = ht->hash_fct(key, ht->keylen, ht->hashseed) % ht->size.add;

	old_head = node = rcu_dereference(ht->tbl[hash]);
	for (;;) {
		if (likely(!node)) {
			break;
		}
		if (node->key == key) {
			ret = -EEXIST;
			goto end;
		}
		node = rcu_dereference(node->next);
	}
	new_head->next = old_head;
	if (rcu_cmpxchg_pointer(&ht->tbl[hash], old_head, new_head) != old_head)
		goto restart;
end:
	rcu_read_unlock();

	return ret;

	/* restart loop, release and re-take the read lock to be kind to GP */
restart:
	rcu_read_unlock();
	goto retry;
}

/*
 * Restart until we successfully remove the entry, or no entry is left
 * ((void *)(unsigned long)-ENOENT).
 * Deal with concurrent stealers by doing an extra verification pass to check
 * that no element in the list are still pointing to the element stolen.
 * This could happen if two concurrent steal for consecutive objects are
 * executed. A pointer to an object being stolen could be saved by the
 * concurrent stealer for the previous object.
 * Also, given that in this precise scenario, another stealer can also want to
 * delete the doubly-referenced object; use a "stolen" flag to let only one
 * stealer delete the object.
 */
void *ht_steal(struct rcu_ht *ht, void *key)
{
	struct rcu_ht_node **prev, *node, *del_node = NULL;
	unsigned long hash;
	void *data;

retry:
	rcu_read_lock();

	hash = ht->hash_fct(key, ht->keylen, ht->hashseed) % ht->size.lookup;

	prev = &ht->tbl[hash];
	node = rcu_dereference(*prev);
	for (;;) {
		if (likely(!node)) {
			if (del_node) {
				goto end;
			} else {
				goto error;
			}
		}
		if (node->key == key) {
			break;
		}
		prev = &node->next;
		node = rcu_dereference(*prev);
	}

	if (!del_node) {
		/*
		 * Another concurrent thread stole it ? If so, let it deal with
		 * this. Assume NODE_STOLEN is the only flag. If this changes,
		 * read flags before cmpxchg.
		 */
		if (cmpxchg(&node->flags, 0, NODE_STOLEN) != 0)
			goto error;
	}

	/* Found it ! pointer to object is in "prev" */
	if (rcu_cmpxchg_pointer(prev, node, node->next) == node)
		del_node = node;
	goto restart;

end:
	/*
	 * From that point, we own node. Note that there can still be concurrent
	 * RCU readers using it. We can free it outside of read lock after a GP.
	 */
	rcu_read_unlock();

	data = del_node->data;
	call_rcu(free, del_node);
	return data;

error:
	data = (void *)(unsigned long)-ENOENT;
	rcu_read_unlock();
	return data;

	/* restart loop, release and re-take the read lock to be kind to GP */
restart:
	rcu_read_unlock();
	goto retry;
}

int ht_delete(struct rcu_ht *ht, void *key)
{
	void *data;

	data = ht_steal(ht, key);
	if (data && data != (void *)(unsigned long)-ENOENT) {
		if (ht->free_fct)
			call_rcu(ht->free_fct, data);
		return 0;
	} else {
		return -ENOENT;
	}
}

/* Delete all old elements. Allow concurrent writer accesses. */
int ht_delete_all(struct rcu_ht *ht)
{
	unsigned long i;
	struct rcu_ht_node **prev, *node, *inext;
	int cnt = 0;

	for (i = 0; i < ht->size.lookup; i++) {
		rcu_read_lock();
		prev = &ht->tbl[i];
		/*
		 * Cut the head. After that, we own the first element.
		 */
		node = rcu_xchg_pointer(prev, NULL);
		if (!node) {
			rcu_read_unlock();
			continue;
		}
		/*
		 * We manage a list shared with concurrent writers and readers.
		 * Note that a concurrent add may or may not be deleted by us,
		 * depending if it arrives before or after the head is cut.
		 * "node" points to our first node. Remove first elements
		 * iteratively.
		 */
		for (;;) {
			inext = NULL;
			prev = &node->next;
			if (prev)
				inext = rcu_xchg_pointer(prev, NULL);
			/*
			 * "node" is the first element of the list we have cut.
			 * We therefore own it, no concurrent writer may delete
			 * it. There can only be concurrent lookups. Concurrent
			 * add can only be done on a bucket head, but we've cut
			 * it already. inext is also owned by us, because we
			 * have exchanged it for "NULL". It will therefore be
			 * safe to use it after a G.P.
			 */
			rcu_read_unlock();
			if (node->data)
				call_rcu(ht->free_fct, node->data);
			call_rcu(free, node);
			cnt++;
			if (likely(!inext))
				break;
			rcu_read_lock();
			node = inext;
		}
	}
	return cnt;
}

/*
 * Should only be called when no more concurrent readers nor writers can
 * possibly access the table.
 */
int ht_destroy(struct rcu_ht *ht)
{
	int ret;

	ret = ht_delete_all(ht);
	free(ht->tbl);
	free(ht);
	return ret;
}

/*
 * Expects keys <= than pointer size to be encoded in the pointer itself.
 */
uint32_t ht_jhash(void *key, uint32_t length, uint32_t initval)
{
	uint32_t ret;
	void *vkey;

	if (length <= sizeof(void *))
		vkey = &key;
	else
		vkey = key;
	ret = jhash(vkey, length, initval);
	return ret;
}
