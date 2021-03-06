#include "cross-platform.h"
#include "lfq.h"
#include <errno.h>
#define MAXFREE 150

int inHP(struct lfq_ctx *ctx, struct lfq_node * lfn) {
	for ( int i = 0 ; i < ctx->MAXHPSIZE ; i++ ) {
		lmb();
		if (ctx->HP[i] == lfn)
			return 1;
	}
	return 0;
}

void enpool(struct lfq_ctx *ctx, struct lfq_node * lfn) {
	volatile struct lfq_node * p;
	do {
		p = ctx->fpt;
	} while(!CAS(&ctx->fpt, p, lfn));
	p->free_next = lfn;
}

void free_pool(struct lfq_ctx *ctx, bool freeall ) {
	if (!CAS(&ctx->is_freeing, 0, 1))
		return; // this pool free is not support multithreading.
	volatile struct lfq_node * p;
	
	for ( int i = 0 ; i < MAXFREE || freeall ; i++ ) {
		p = ctx->fph;
		if ( (!p->can_free) || (!p->free_next) || inHP(ctx, (struct lfq_node *)p) )
			goto exit;
		ctx->fph = p->free_next;
		free((void *)p);
	}
exit:
	ctx->is_freeing = false;
	smb();
}

void safe_free(struct lfq_ctx *ctx, struct lfq_node * lfn) {
	if (lfn->can_free && !inHP(ctx,lfn)) {
		 // free is not thread safety
		if (CAS(&ctx->is_freeing, 0, 1)) {
			free(lfn);
			ctx->is_freeing = false;
			smb();
		} else
			enpool(ctx, lfn);
	} else
		enpool(ctx, lfn);
	free_pool(ctx, false);
}

int alloc_tid(struct lfq_ctx *ctx) {
	for (int i = 0; i < ctx->MAXHPSIZE; i++) 
		if (ctx->tid_map[i] == 0) 
			if (CAS(&ctx->tid_map[i], 0, 1))
				return i;

	return -1;
}

void free_tid(struct lfq_ctx *ctx, int tid) {
	ctx->tid_map[tid]=0;
}

int lfq_init(struct lfq_ctx *ctx, int max_consume_thread) {
	struct lfq_node * tmpnode = calloc(1,sizeof(struct lfq_node));
	if (!tmpnode) 
		return -errno;
		
	struct lfq_node * free_pool_node = calloc(1,sizeof(struct lfq_node));
	if (!free_pool_node) 
		return -errno;
		
	tmpnode->can_free = free_pool_node->can_free = true;
	memset(ctx, 0, sizeof(struct lfq_ctx));
	ctx->MAXHPSIZE = max_consume_thread;
	ctx->HP = calloc(max_consume_thread,sizeof(struct lfq_node));
	ctx->tid_map = calloc(max_consume_thread,sizeof(struct lfq_node));
	ctx->head = ctx->tail=tmpnode;
	ctx->fph = ctx->fpt=free_pool_node;
	
	return 0;
}

int lfq_clean(struct lfq_ctx *ctx){
	if ( ctx->tail && ctx->head ) { // if have data in queue
		struct lfq_node *tmp;
		while ( (struct lfq_node *) ctx->head ) { // while still have node
			tmp = (struct lfq_node *) ctx->head->next;
			safe_free(ctx, (struct lfq_node *)ctx->head);
			ctx->head = tmp;
		}
		ctx->tail = 0;
	}
	if ( ctx->fph && ctx->fpt ) {
		free_pool(ctx, true);
		if ( ctx->fph != ctx->fpt )
			return -1;
		free((void *)ctx->fpt); // free the empty node
		ctx->fph=ctx->fpt=0;
	}
	if ( !ctx->fph && !ctx->fpt ) {
		free((void *)ctx->HP);
		free((void *)ctx->tid_map);
		memset(ctx,0,sizeof(struct lfq_ctx));
	} else
		return -1;
		
	return 0;
}

int lfq_enqueue(struct lfq_ctx *ctx, void * data) {
	struct lfq_node * p;
	struct lfq_node * insert_node = calloc(1,sizeof(struct lfq_node));
	if (!insert_node)
		return -errno;
	insert_node->data=data;
	mb();
	do {
		p = (struct lfq_node *) ctx->tail;
	} while(!CAS(&ctx->tail,p,insert_node));
	p->next = insert_node;
	ATOMIC_ADD( &ctx->count, 1);
	return 0;
}

void * lfq_dequeue_tid(struct lfq_ctx *ctx, int tid ) {
	void * ret=0;
	volatile struct lfq_node * p=0, * pn=0;
	int cn_runtimes = 0;
	do {
		p = ctx->head;
		ctx->HP[tid] = p;
		mb();
		if (p != ctx->head)
			continue;
		pn = p->next;
		if (pn==0 || pn != p->next){
			ctx->HP[tid] = 0;
			return 0;
		}
	} while( ! CAS(&ctx->head, p, pn) );
	mb();
	ctx->HP[tid] = 0;
	ret=pn->data;
	pn->can_free= true;
	ATOMIC_SUB( &ctx->count, 1 );
	safe_free(ctx, (struct lfq_node *)p);
	return ret;
}

void * lfq_dequeue(struct lfq_ctx *ctx ) {
	int tid = alloc_tid(ctx);
	if (tid==-1)
		return (void *)-1; // To many thread race

	void * ret = lfq_dequeue_tid(ctx, tid);
	free_tid(ctx, tid);
	return ret;
}
