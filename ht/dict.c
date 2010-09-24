#include "dict.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define DICT_MAX_LOAD	0.75
#define DICT_REHASH_BATCH_SIZE	20

/* Hash function by Daniel J. Bernstein */
static unsigned long
djb_hash(char *str, size_t sz) {

	unsigned long hash = 5381;
	char *p;

	for(p = str; p < str+sz; ++p) {
		hash = ((hash << 5) + hash) + (unsigned char)(*p); /* hash * 33 + c */
	}

	return hash;
}

static long
ht_next_prime(long k) {
	long primes[] = {53, 97, 193, 389, 769, 1543, 3079, 6151, 12289,
		24593, 49157, 98317, 196613, 393241, 786433, 1572869,
		3145739, 6291469, 12582917, 25165843, 50331653,
		100663319, 201326611, 402653189, 805306457, 1610612741,
		3221225473, 4294967291};

	int i;
	long n = sizeof(primes)/sizeof(*primes);
	for(i = n-1; i != -1; --i) {
		if(k >= primes[i]) {
			break;
		}
	}

	i++;
	return primes[i];
}

/* create a new HT */
static struct ht *
ht_new(long sz) {
	struct ht *ht = calloc(sizeof(struct ht), 1);

	sz = ht_next_prime(sz);

	ht->sz = sz;

	ht->slots = calloc(sizeof(struct bucket), sz);
	if(ht->slots == NULL) {
		fprintf(stderr, "failed to allocate %ld bytes.\n",
				sz * (long)sizeof(struct bucket));
		abort();
	}

	return ht;
}


static void
bucket_free(struct bucket *b) {
	if(b->collision_prev) {
		b->collision_prev->collision_next = b->collision_next;
		if(b->collision_next) {
			b->collision_next->collision_prev = b->collision_prev;
		}

		free(b);
	}
}


/* delete a HT */
static void
ht_free(struct ht *ht, void (*key_free)(void*)) {
	struct bucket *b, *tmp;

	/* delete all items from the HT */
	for(b = ht->first; b; ) {
		tmp = b->next;
		key_free(b->k);
		bucket_free(b);
		b = tmp;
	}

	/* delete the container */
	free(ht->slots);
	free(ht);
}


/* inset into a HT */
static struct bucket *
ht_insert(struct ht *ht, unsigned long h, char *k, size_t k_sz, char *v, size_t v_sz) {

	struct bucket *b, *head, *tmp;

	tmp = head = b = &ht->slots[h % ht->sz];

	/* check for re-assignment */
	while(tmp && tmp->k) {
		if(tmp->k_sz == k_sz && memcmp(k, tmp->k, k_sz) == 0) {

			/* keep track of used size */
			ht->total_key_len += k_sz - tmp->k_sz;
			ht->total_val_len += v_sz - tmp->v_sz;

			tmp->v = v; /* hit! replace */
			tmp->v_sz = v_sz;
			return NULL;
		}
		tmp = tmp->collision_next;
	}

	/* found nothing, add to head. */
	if(head->k != NULL) { /* add to existing list */
		b = calloc(sizeof(struct bucket), 1);
		b->collision_next = head->collision_next;
		head->collision_next = b;
		b->collision_prev = head;
	}
	b->k = k;
	b->k_sz = k_sz;
	b->v = v;
	b->v_sz = v_sz;

	/* keep track of used size */
	ht->count++;
	ht->total_key_len += k_sz;
	ht->total_val_len += v_sz;

	return b;
}

/* lookup a key in a HT */
static struct bucket *
ht_get(struct ht *ht, unsigned long h, char *k, size_t k_sz) {

	struct bucket *b = &ht->slots[h % ht->sz];

	while(b) {
		if(b->k && k_sz == b->k_sz && memcmp(k, b->k, k_sz) == 0) { /* found! */
			return b;
		}
		b = b->collision_next;
	}
	return NULL;
}


/* record a bucket as used */
static void
ht_record_used_bucket(struct ht *ht, struct bucket *b) {

	if(ht->first) {
		ht->first->prev = b;
	}
	b->next = ht->first;
	ht->first = b;
}


static char*
key_dup_default(const char *k, size_t k_sz) {
	(void)k_sz;
	return (char*)k;
}
static void
key_free_default(void *k) {
	(void)k;
	return;
}


/* create a new dictionary */
struct dict *
dict_new(long sz) {
	
	struct dict *d = calloc(sizeof(struct dict), 1);
	d->ht = ht_new(sz); /* a single pre-defined HT */

	/* default helper functions */
	d->key_hash = djb_hash;
	d->key_dup = key_dup_default;
	d->key_free = key_free_default;

	return d;
}

/* delete a dictionary */
void
dict_free(struct dict *d) {

	/* free both hash tables */
	ht_free(d->ht, d->key_free);
	if(d->ht_old) ht_free(d->ht_old, d->key_free);

	free(d);
}

/* transfer K items from the old hash table to the new one. */
static void
dict_rehash(struct dict *d) {
	
	long k = DICT_REHASH_BATCH_SIZE;
	struct bucket *b, *next;

	if(d->ht_old == NULL) {
		return;
	}

	/* transfer old elements to the new HT. */

	for(b = d->ht_old->first; b && k--;) {

		struct bucket *b_new;
		unsigned long h = d->key_hash(b->k, b->k_sz);

		if((b_new = ht_insert(d->ht, h, b->k, b->k_sz, b->v, b->v_sz))) {
			/* new used slot, add to list. */
			ht_record_used_bucket(d->ht, b_new);
		}

		next = b->next;

		/* re-attach b's neighbours together and free b. */
		bucket_free(b);
		b = next;
	}

	if((d->ht_old->first = b)) {
		return;
	}

	ht_free(d->ht_old, d->key_free);
	d->ht_old = NULL;
}


/* add an item into the dictionary */
void
dict_set(struct dict *d, char *k, size_t k_sz, char *v, size_t v_sz) {

	struct bucket *b;
	char *k_dup = k;
	unsigned long h = d->key_hash(k, k_sz);

	/* possibly duplicate key */
	k_dup = d->key_dup(k, k_sz);

	/* check for important load and resize if need be. */
	if((float)d->ht->count / (float)d->ht->sz > DICT_MAX_LOAD) {
		/* expand and replace HT */
		d->ht_old = d->ht;
		d->ht = ht_new(d->ht->sz + 1); /* will select next prime */
	}

	if((b = ht_insert(d->ht, h, k_dup, k_sz, v, v_sz))) {
		ht_record_used_bucket(d->ht, b);
	}
	
	dict_rehash(d);
}

char*
dict_get(struct dict *d, char *k, size_t k_sz, size_t *v_sz) {

	struct bucket *b;
	unsigned long h = d->key_hash(k, k_sz);

	if((b = ht_get(d->ht, h, k, k_sz))) {
		if(v_sz) {
			*v_sz = b->v_sz;
		}
		return b->v;
	} else if(d->ht_old && (b = ht_get(d->ht_old, h, k, k_sz))) {
		if(v_sz) {
			*v_sz = b->v_sz;
		}
		return b->v;
	}

	return NULL;
}

int
dict_remove(struct dict *d, char *k, size_t sz) {

	struct bucket *b = NULL;
	unsigned long h = d->key_hash(k, sz);
	struct ht *ht;

	if((b = ht_get(d->ht, h, k, sz))) {
		ht = d->ht;
	} else if(d->ht_old && (b = ht_get(d->ht_old, h, k, sz))) {
		ht = d->ht_old;
	} else {
		return -1;
	}

	/* re-attach elements */
	if(b->prev) {
		b->prev->next = b->next;
	}
	if(b->next) {
		b->next->prev = b->prev;
	}

	ht->total_key_len -= b->k_sz;
	ht->total_val_len -= b->v_sz;
	ht->count--;

	/* free duplicated key */
	d->key_free(b->k);

	/* remove bucket */
	bucket_free(b);

	return 0;
}

/* for each item, call a callback function */
void
dict_foreach(struct dict *d, foreach_cb fun, void *data) {

	int i;
	struct bucket *heads[2];

	heads[0] = d->ht->first;
	if(d->ht_old) {
		heads[1] = d->ht_old->first;
	} else {
		heads[1] = NULL;
	}

	/* call on each HT */
	for(i = 0; i < 2; ++i) {
		struct bucket *b;
		for(b = heads[i]; b; b = b->next) {
			fun(b->k, b->k_sz, b->v, b->v_sz, data);
		}
	}
}

long
dict_count(struct dict *d) {
	
	long l = d->ht->count;
	if(d->ht_old) {
		l += d->ht_old->count;
	}
	printf("total count: %ld\n", l);
	return l;
}

