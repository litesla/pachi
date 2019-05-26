#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "debug.h"
#include "move.h"
#include "random.h"
#include "uct/internal.h"
#include "uct/tree.h"
#include "uct/policy/generic.h"

/* This implements the basic UCB1 policy. */
/*这实现了基本的UCB1策略。*/
struct ucb1_policy {
	/* This is what the Modification of UCT with Patterns in Monte Carlo Go
	 * paper calls 'p'. Original UCB has this on 2, but this seems to
	 * produce way too wide searches; reduce this to get deeper and
	 * narrower readouts - try 0.2. */
    /*这就是用蒙特卡洛GO论文中的模式修改UCT时所说的“P”。最初的UCB在2上有这个，但这似乎产生了太宽的搜索范围；减少这个范围以获得更深更窄的读数-尝试0.2*/
	floating_t explore_p;
	/* First Play Urgency - if set to less than infinity (the MoGo paper
	 * above reports 1.0 as the best), new branches are explored only
	 * if none of the existing ones has higher urgency than fpu. */
    /*首先，发挥紧迫性-如果设置为小于无穷大（Mogo论文以上报告1.0为最好），新的分支机构只有在没有一个现有的具有更高的紧迫性比FPU。*/
	floating_t fpu;
};


void
ucb1_descend(struct uct_policy *p, struct tree *tree, struct uct_descent *descent, int parity, bool allow_pass)
{
	/* We want to count in the prior stats here after all. Otherwise,
	 * nodes with positive prior will get explored _LESS_ since the
	 * urgency will be always higher; even with normal FPU because
	 * of the explore coefficient. */
    /*毕竟，我们想在之前的统计数据中计算。否则，具有正先验的节点将得到较少的探测，因为紧急性总是更高；甚至由于探测系数的原因，使用正常的FPU。*/
	struct ucb1_policy *b = p->data;
	floating_t xpl = log(descent->node->u.playouts + descent->node->prior.playouts);

	uctd_try_node_children(tree, descent, allow_pass, parity, p->uct->tenuki_d, di, urgency) {
		struct tree_node *ni = di.node;
		int uct_playouts = ni->u.playouts + ni->prior.playouts + ni->descents;

		/* XXX: We don't take local-tree information into account. */
        /*我们不考虑本地树信息。*/

		if (uct_playouts) {
			urgency = (ni->u.playouts * tree_node_get_value(tree, parity, ni->u.value)
				   + ni->prior.playouts * tree_node_get_value(tree, parity, ni->prior.value))
				   + (parity > 0 ? 0 : ni->descents)
				  / uct_playouts;
			urgency += b->explore_p * sqrt(xpl / uct_playouts);
		} else {
			urgency = b->fpu;
		}
	} uctd_set_best_child(di, urgency);

	uctd_get_best_child(descent);
}

void
ucb1_update(struct uct_policy *p, struct tree *tree, struct tree_node *node, enum stone node_color, enum stone player_color, struct playout_amafmap *map, struct board *final_board, floating_t result)
{
	/* It is enough to iterate by a single chain; we will
	 * update all the preceding positions properly since
	 * they had to all occur in all branches, only in
	 * different order. */
    /*通过一个链迭代就足够了；我们将正确地更新前面的所有位置，因为它们都必须以不同的顺序出现在所有分支中。*/
	enum stone winner_color = result > 0.5 ? S_BLACK : S_WHITE;

	for (; node; node = node->parent) {
		stats_add_result(&node->u, result, 1);

		if (!is_pass(node_coord(node))) {
			stats_add_result(&node->winner_owner, board_at(final_board, node_coord(node)) == winner_color ? 1.0 : 0.0, 1);
			stats_add_result(&node->black_owner, board_at(final_board, node_coord(node)) == S_BLACK ? 1.0 : 0.0, 1);
		}
	}
}

void
ucb1_done(struct uct_policy *p)
{
	free(p->data);
	free(p);
}
//ｕｃｂ１策略初始化
struct uct_policy *
policy_ucb1_init(struct uct *u, char *arg)
{
	struct uct_policy *p = calloc2(1, sizeof(*p));
	struct ucb1_policy *b = calloc2(1, sizeof(*b));
	p->uct = u;
	p->data = b;
	p->done = ucb1_done;//
	p->descend = ucb1_descend;//下沉
	p->choose = uctp_generic_choose;//选择
	p->update = ucb1_update;//跟新

	b->explore_p = 0.2;
	b->fpu = 1.1; //INFINITY;

	if (arg) {
		char *optspec, *next = arg;
		while (*next) {
			optspec = next;
			next += strcspn(next, ":");
			if (*next) { *next++ = 0; } else { *next = 0; }

			char *optname = optspec;
			char *optval = strchr(optspec, '=');
			if (optval) *optval++ = 0;

			if (!strcasecmp(optname, "explore_p") && optval) {
				b->explore_p = atof(optval);
			} else if (!strcasecmp(optname, "fpu") && optval) {
				b->fpu = atof(optval);
			} else
				die("ucb1: Invalid policy argument %s or missing value\n", optname);
		}
	}

	return p;
}
