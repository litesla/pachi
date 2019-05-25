#ifndef PACHI_UCT_TREE_H
#define PACHI_UCT_TREE_H

/* Management of UCT trees. See diagram below for the node structure.
 *
 * Two allocation methods are supported for the tree nodes:
 *
 * - calloc/free: each node is allocated with one calloc.
 *   After a move, all nodes except the subtree rooted at
 *   the played move are freed one by one with free().
 *   Since this can be very slow (seen 9s and loss on time because
 *   of this) the nodes are freed in a background thread.
 *   We still reserve enough memory for the next move in case
 *   the background thread doesn't free nodes fast enough.
 *
 * - fast_alloc: a large buffer is allocated once, and each
 *   node allocation takes some of this buffer. After a move
 *   is played, no memory if freed if the buffer still has
 *   enough free space. Otherwise the subtree rooted at the
 *   played move is copied to a temporary buffer, pruning it
 *   if necessary to fit in this small buffer. We copy by
 *   preference nodes with largest number of playouts.
 *   Then the temporary buffer is copied back to the original
 *   buffer, which has now plenty of space.
 *   Once the fast_alloc mode is proven reliable, the
 *   calloc/free method will be removed. */
/*UCT树的管理。节点结构见下图。

*树节点支持两种分配方法：
*
*-calloc/free：每个节点分配一个calloc。
*移动后，除根位于的子树之外的所有节点
*使用free（）逐个释放播放的移动。
*因为这可能很慢（见9和损失时间，因为
*其中）节点在后台线程中释放。
*我们仍然保留足够的内存以备下次行动
*后台线程释放节点的速度不够快。
*
*-快速分配：一次分配一个大缓冲区，每个缓冲区
*节点分配占用了这个缓冲区的一部分。搬家后
*如果缓冲区仍然存在，则没有内存释放。
*足够的自由空间。否则，子树在
*将播放的移动复制到临时缓冲区，并将其修剪
*如有必要，安装在这个小缓冲器中。我们抄袭
*播放次数最多的首选项节点。
*然后将临时缓冲区复制回原始缓冲区
*缓冲区，现在有足够的空间。
*一旦快速分配模式被证明是可靠的，
*将删除calloc/free方法。*/

#include <stdbool.h>
#include <pthread.h>
#include "move.h"
#include "stats.h"
#include "probdist.h"

struct board;
struct uct;

/*
 *            +------+
 *            | node |
 *            +------+
 *          / <- parent
 * +------+   v- sibling +------+
 * | node | ------------ | node |
 * +------+              +------+
 *    | <- children          |
 * +------+   +------+   +------+   +------+
 * | node | - | node |   | node | - | node |
 * +------+   +------+   +------+   +------+
 */

/* TODO: Performance would benefit from a reorganization:
 * (i) Allocate all children of a node within a single block.
 * (ii) Keep all u stats together, and all amaf stats together.
 * Currently, rave_update is top source of cache misses, and
 * there is large memory overhead for having all nodes separate. */
/*TODO:重组将使业绩受益：
*（i）在单个块中分配节点的所有子节点。
*（ii）将所有U统计和所有AMAF统计放在一起。
*目前，rave_更新是缓存未命中的首要来源，并且
*将所有节点分开会有很大的内存开销。*/
struct tree_node {
	hash_t hash;
    //这一课树是一个二叉树 还有一个父节点指针 十字链表表示法，左孩子右兄弟
	struct tree_node *parent, *sibling, *children;

	/*** From here on, struct is saved/loaded from opening tbook */

	struct move_stats u;
	struct move_stats prior;
	/* XXX: Should be way for policies to add their own stats */
	struct move_stats amaf;
	/* Stats before starting playout; used for distributed engine. */
	struct move_stats pu;
	/* Criticality information; information about final board owner
	 * of the tree coordinate corresponding to the node */
	struct move_stats winner_owner; // owner == winner
	struct move_stats black_owner; // owner == black

	/* coord is usually coord_t, but this is very space-sensitive. */
#define node_coord(n) ((int) (n)->coord)
	short coord;

	unsigned short depth; // just for statistics

	/* Number of parallel descents going through this node at the moment.
	* Used for virtual loss computation. */
	signed char descents;

	/* Common Fate Graph distance from parent, but at most TREE_NODE_D_MAX+1 */
#define TREE_NODE_D_MAX 3
	unsigned char d;

#define TREE_HINT_INVALID 1 // don't go to this node, invalid move
	unsigned char hints;

	/* In case multiple threads walk the tree, is_expanded is set
	* atomically. Only the first thread setting it expands the node.
	* The node goes through 3 states:
	*   1) children == null, is_expanded == false: leaf node
	*   2) children == null, is_expanded == true: one thread currently expanding
	*   2) children != null, is_expanded == true: fully expanded node */
    /**如果多个线程遍历树，则设置为“展开”
        *原子性。只有第一个线程设置会扩展节点。
        *节点经历3种状态：
        *1）children==null，is_expanded==false:叶节点
        *2）children==null，is_expanded==true:一个线程当前正在扩展
        *2）孩子们！=null，is_expanded==true:完全展开的节点*/
	bool is_expanded;
};

struct tree_hash;

struct tree {
	struct board *board;
	struct tree_node *root;//这个才是树 其他的都是树的属性
	struct board_symmetry root_symmetry;
	enum stone root_color;

	/* Whether to use any extra komi during score counting. This is
	 * tree-specific variable since this can arbitrarily change between
	 * moves. */
	bool use_extra_komi;
	/* A single-move-valid flag that marks a tree that is potentially
	 * badly skewed and should be used with care. Currently, we never
	 * resign on untrustworthy_tree and do not reuse the tree on next
	 * move. */
	bool untrustworthy_tree;
	/* The value of applied extra komi. For DYNKOMI_LINEAR, this value
	 * is only informative, the actual value is computed per simulation
	 * based on leaf node depth. */
	floating_t extra_komi;
	/* Score in simulations, averaged over all branches, in the last
	 * search episode. */
	struct move_stats avg_score;

	/* We merge local (non-tenuki) sequences for both colors, occuring
	 * anywhere in the tree; nodes are created on-demand, special 'pass'
	 * nodes represent tenuki. Only u move_stats are used, prior and amaf
	 * is ignored. Values in root node are ignored. */
	/* The value corresponds to black-to-play as usual; i.e. if white
	 * succeeds in its replies, the values will be low. */
    /*我们合并两种颜色的局部（非Tenuki）序列，发生
        *树中的任意位置；节点按需创建，特殊的“pass”
        *节点表示Tenuki。仅使用u move_stats，prior和amaf
        *被忽略。根节点中的值将被忽略。*/
        /*该值与黑色对应，以正常播放；即，如果为白色
         * *回复成功，数值会很低。*/
	struct tree_node *ltree_black;
	/* ltree_white has white-first sequences as children. */
	struct tree_node *ltree_white;
	/* Aging factor; 2 means halve all playout values after each turn.
	 * 1 means don't age at all. */
	floating_t ltree_aging;

	/* Hash table used when working as slave for the distributed engine.
	 * Maps coordinate path to tree node. */
    /*作为分布式引擎的从机时使用的哈希表。将坐标路径映射到树节点*/
	struct tree_hash *htable;
	int hbits;

	// Statistics
	int max_depth;
	volatile size_t nodes_size; // byte size of all allocated nodes
	size_t max_tree_size; // maximum byte size for entire tree, > 0 only for fast_alloc
	size_t max_pruned_size;
	size_t pruning_threshold;
	void *nodes; // nodes buffer, only for fast_alloc
};

/* Warning: all functions below except tree_expand_node & tree_leaf_node are THREAD-UNSAFE! */
struct tree *tree_init(struct board *board, enum stone color, size_t max_tree_size,
		       size_t max_pruned_size, size_t pruning_threshold, floating_t ltree_aging, int hbits);
void tree_done(struct tree *tree);
void tree_dump(struct tree *tree, double thres);
void tree_save(struct tree *tree, struct board *b, int thres);
void tree_load(struct tree *tree, struct board *b);

struct tree_node *tree_get_node(struct tree *tree, struct tree_node *node, coord_t c, bool create);
struct tree_node *tree_garbage_collect(struct tree *tree, struct tree_node *node);
void tree_promote_node(struct tree *tree, struct tree_node **node);
bool tree_promote_at(struct tree *tree, struct board *b, coord_t c);

void tree_expand_node(struct tree *tree, struct tree_node *node, struct board *b, enum stone color, struct uct *u, int parity);
struct tree_node *tree_lnode_for_node(struct tree *tree, struct tree_node *ni, struct tree_node *lni, int tenuki_d);

static bool tree_leaf_node(struct tree_node *node);

#define tree_node_parity(tree, node) \
	((((node)->depth ^ (tree)->root->depth) & 1) ? -1 : 1)

/* Get black parity from parity within the tree. */
#define tree_parity(tree, parity) \
	(tree->root_color == S_WHITE ? (parity) : -1 * (parity))

/* Get a 0..1 value to maximize; @parity is parity within the tree. */
#define tree_node_get_value(tree, parity, value) \
	(tree_parity(tree, parity) > 0 ? value : 1 - value)

static inline bool
tree_leaf_node(struct tree_node *node)
{
	return !(node->children);
}

static inline floating_t
tree_node_criticality(const struct tree *t, const struct tree_node *node)
{
	/* cov(player_gets, player_wins) =
	 * [The argument: If 'gets' and 'wins' is uncorrelated, b_gets * b_wins
	 * is valid way to obtain winner_gets. The more correlated it is, the
	 * more distorted the result.]
	 * = winner_gets - (b_gets * b_wins + w_gets * w_wins)
	 * = winner_gets - (b_gets * b_wins + (1 - b_gets) * (1 - b_wins))
	 * = winner_gets - (b_gets * b_wins + 1 - b_gets - b_wins + b_gets * b_wins)
	 * = winner_gets - (2 * b_gets * b_wins - b_gets - b_wins + 1) */
	return node->winner_owner.value
		- (2 * node->black_owner.value * node->u.value
		   - node->black_owner.value - node->u.value + 1);
}

#endif
