#ifndef PACHI_UCT_POLICY_GENERIC_H
#define PACHI_UCT_POLICY_GENERIC_H

/* Some default policy routines and templates. */

#include "board.h"
#include "stone.h"
#include "uct/internal.h"

struct board;
struct tree_node;

struct tree_node *uctp_generic_choose(struct uct_policy *p, struct tree_node *node, struct board *b, enum stone color, coord_t exclude);
void uctp_generic_winner(struct uct_policy *p, struct tree *tree, struct uct_descent *descent);


/* Some generic stitching for tree descent. */

#if 0
#define uctd_debug(fmt...) fprintf(stderr, fmt);
#else
#define uctd_debug(fmt...)
#endif

#define uctd_try_node_children(tree, descent, allow_pass, parity, tenuki_d, di, urgency) \
	/* Information abound best children. 信息丰富，最好的孩子*/ \
	/* XXX: We assume board <=25x25.我们假设电路板<=25x25 */ \
	struct uct_descent dbest[BOARD_MAX_MOVES + 1] = { { .node = descent->node->children, .lnode = NULL } }; int dbests = 1; \
	floating_t best_urgency = -9999; \
	/* Descent children iterator. 后代迭代器。*/ \
	struct uct_descent dci = { .node = descent->node->children, .lnode = descent->lnode ? descent->lnode->children : NULL }; \
	\
    /*一直便利兄弟(sibling)节点找*/\
	for (; dci.node; dci.node = dci.node->sibling) { \
		floating_t urgency; \
		/* Do not consider passing early.不要考虑及早通过。 */ \
		if (unlikely((!allow_pass && is_pass(node_coord(dci.node))) || (dci.node->hints & TREE_HINT_INVALID))) \
			continue; \
		/* Position dci.lnode to point at or right after the local\
		 * node corresponding to dci.node.将dci.lnode定位到与dci.node对应的本地节点处或其后面。 */ \
		while (dci.lnode && node_coord(dci.lnode) < node_coord(dci.node)) \
			dci.lnode = dci.lnode->sibling; \
		/* Set up descent-further iterator. This is the public-accessible
		 * one, and usually is similar to dci. However, in case of local\
		 * trees, we may keep next-candidate pointer in dci while storing\
		 * actual-specimen in di.设置下降进一步迭代器。这是一个公众可访问的，通常类似于DCI。但是，在本地树的情况下，我们可以将下一个候选指针保存在DCI中，同时将实际样本存储在DI中。 */ \
		struct uct_descent di = dci; \
		if (dci.lnode) { \
			/* Set lnode to local tree node corresponding
			 * to node (dci.lnode, pass-lnode or NULL).将lnode设置为与节点对应的本地树节点（dci.lnode、pass lnode或null）。 */ \
			di.lnode = tree_lnode_for_node(tree, dci.node, dci.lnode, tenuki_d); \
		}

		/* ...your urgency computation code goes here...您的紧急计算代码显示在这里。 */

#define uctd_set_best_child(di, urgency) \
		uctd_debug("(%s) %f\n", coord2sstr(node_coord(di.node), tree->board), urgency); \
		if (urgency - best_urgency > __FLT_EPSILON__) { /* urgency > best_urgency */ \
			uctd_debug("new best\n"); \
			best_urgency = urgency; dbests = 0; \
		} \
		if (urgency - best_urgency > -__FLT_EPSILON__) { /* urgency >= best_urgency */ \
			uctd_debug("another best\n"); \
			/* We want to always choose something else than a pass \
			 * in case of a tie. pass causes degenerative behaviour.\ 我们总是想选择其他的东西，而不是一个通行证，以防平局。通过导致退化行为*/ \
			if (dbests == 1 && is_pass(node_coord(dbest[0].node))) { \
				dbests--; \
			} \
			struct uct_descent db = di; \
			/* Make sure lnode information is meaningful.确保代码信息有意义。 */ \
			if (db.lnode && is_pass(node_coord(db.lnode))) \
				db.lnode = NULL; \
			dbest[dbests++] = db; \
		} \
	}

#define uctd_get_best_child(descent) *(descent) = dbest[fast_random(dbests)];


#endif
