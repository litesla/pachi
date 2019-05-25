#ifndef PACHI_UCT_PRIOR_H
#define PACHI_UCT_PRIOR_H

#include "move.h"
#include "uct/tree.h"

struct tree;
struct tree_node;
struct uct;
struct board;
//映射
struct prior_map {
	struct board *b;
	enum stone to_play;
	int parity;
	/* [board_size2(b)] array, move_stats are the prior
	 * values to be assigned to individual moves;
	 * move_stats.value is not updated. */
    /*[Board_size2（b）]数组，move_stats是分配给单个移动的先前值；move_stats.value不更新。*/
	struct move_stats *prior;
	/* [board_size2(b)] array, whether to compute
	 * prior for the given value. */
    /*[Board_size2（b）]数组，是否先计算给定值。*/
	bool *consider;
	/* [board_size2(b)] array from cfg_distances() */
    /*来自cfg_Distances（）的[Board_Size2（b）]数组*/
	int *distances;
};

/* @value is the value, @playouts is its weight. */
static void add_prior_value(struct prior_map *map, coord_t c, floating_t value, int playouts);

void uct_prior(struct uct *u, struct tree_node *node, struct prior_map *map);

struct uct_prior;
struct uct_prior *uct_prior_init(char *arg, struct board *b, struct uct *u);
void uct_prior_done(struct uct_prior *p);


static inline void
add_prior_value(struct prior_map *map, coord_t c, floating_t value, int playouts)
{
	floating_t v = map->parity > 0 ? value : 1 - value;
	/* We don't need atomicity: */
	struct move_stats s = { .playouts = playouts, .value = v };
	stats_merge(&map->prior[c], &s);
}

#endif
