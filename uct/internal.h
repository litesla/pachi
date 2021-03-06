#ifndef PACHI_UCT_INTERNAL_H
#define PACHI_UCT_INTERNAL_H

/* Internal UCT structures */

#include "debug.h"
#include "move.h"
#include "ownermap.h"
#include "pattern.h"
#include "patternsp.h"
#include "patternprob.h"
#include "playout.h"
#include "stats.h"
#include "mq.h"

struct tree;
struct tree_node;
struct uct_policy;
struct uct_prior;
struct uct_dynkomi;
struct uct_pluginset;
struct joseki_dict;

/* How many games to consider at minimum before judging groups. */
#define GJ_MINGAMES	500

/* Internal engine state.引擎内部状态 */
struct uct {
	int debug_level;
	enum uct_reporting {
		UR_TEXT,
		UR_JSON,
		UR_JSON_BIG,
	} reporting;
	int reportfreq;

	int games, gamelen;
	floating_t resign_threshold, sure_win_threshold;
	double best2_ratio, bestr_ratio;
	floating_t max_maintime_ratio;
	bool pass_all_alive; /* Current value 当前值*/
	bool allow_losing_pass;
	bool territory_scoring;
	int expand_p;
	bool playout_amaf;
	bool amaf_prior;
	int playout_amaf_cutoff;
	double dumpthres;
	int force_seed;
	bool no_tbook;
	bool fast_alloc;
	size_t max_tree_size;
	size_t max_pruned_size;
	size_t pruning_threshold;
	int mercymin;
	int significant_threshold;//有效阈值

	int threads;
	enum uct_thread_model {
		TM_TREE, /* Tree parallelization w/o virtual loss.无虚拟损失的树并行化 */
		TM_TREEVL, /* Tree parallelization with virtual loss. 树与虚损失并行化。*/
	} thread_model;
	int virtual_loss;
	bool pondering_opt; /* User wants pondering 用户想要思考*/
	bool pondering; /* Actually pondering now 正在思考*/
	bool slave; /* Act as slave in distributed engine.在分布式引擎中充当从机 */
	int max_slaves; /* Optional, -1 if not set 可选，如果未设置-1*/
	int slave_index; /* 0..max_slaves-1, or -1 if not set 0..max_slaves-1或-1（如果未设置）*/
	enum stone my_color;

	int fuseki_end;
	int yose_start;

	int dynkomi_mask;
	int dynkomi_interval;
	struct uct_dynkomi *dynkomi;
	floating_t initial_extra_komi;

	floating_t val_scale;
	int val_points;
	bool val_extra;
	bool val_byavg;
	bool val_bytemp;
	floating_t val_bytemp_min;

	int random_policy_chance;
	bool local_tree;
	int tenuki_d;
	floating_t local_tree_aging;
#define LTREE_PLAYOUTS_MULTIPLIER 100
	floating_t local_tree_depth_decay;
	bool local_tree_allseq;
	bool local_tree_neival;
	enum {
		LTE_ROOT,
		LTE_EACH,
		LTE_TOTAL,
	} local_tree_eval;
	bool local_tree_rootchoose;

	struct {
		int level;
		int playouts;
	} debug_after;

	char *banner;

	struct uct_policy *policy;//规则
	struct uct_policy *random_policy;//随机规则
	struct playout_policy *playout;
	struct uct_prior *prior;
	struct uct_pluginset *plugins;
	struct joseki_dict *jdict;

	struct pattern_setup pat;
	/* Various modules (prior, policy, ...) set this if they want pattern
	 * database to be loaded. */
    /*各种模块（prior，policy，…）如果想要模式，就设置这个。
要加载的数据库。*/
	bool want_pat;

	/* Used within frame of single genmove. */
    /*在单个genmove的框架内使用*/
	struct board_ownermap ownermap;
	/* Used for coordination among slaves of the distributed engine. */
    /*用于分布式引擎的从机之间的协调*/
	int stats_hbits;
	int shared_nodes;
	int shared_levels;
	double stats_delay; /* stored in seconds 以秒为单位存储*/
	int played_own;
	int played_all; /* games played by all slaves 所有奴隶玩的游戏*/

	/* Saved dead groups, for final_status_list dead 所有奴隶玩的游戏*/
	struct move_queue dead_groups;
	int dead_groups_move;

	/* Game state - maintained by setup_state(), reset_state(). */
    /*游戏状态-由setup_state（）、reset_state（）维护*/
	struct tree *t;
};

#define UDEBUGL(n) DEBUGL_(u->debug_level, n)

bool uct_pass_is_safe(struct uct *u, struct board *b, enum stone color, bool pass_all_alive, char **msg);
void uct_prepare_move(struct uct *u, struct board *b, enum stone color);
void uct_genmove_setup(struct uct *u, struct board *b, enum stone color);
void uct_pondering_stop(struct uct *u);
void uct_get_best_moves(struct tree *t, coord_t *best_c, float *best_r, int nbest, bool winrates);

/* This is the state used for descending the tree; we use this wrapper
 * structure in order to be able to easily descend in multiple trees
 * in parallel (e.g. main tree and local tree) or compute cummulative
 * "path value" throughout the tree descent. */
/*这是用于降树的状态；我们使用这个包装结构以便能够在多个树中并行下降（例如主树和本地树），或者在整个树下降过程中计算累积的“路径值”。*/
struct uct_descent {
	/* Active tree nodes: */
    /*活动树节点：*/
	struct tree_node *node; /* Main tree. */
	struct tree_node *lnode; /* Local tree. */
	/* Value of main tree node (with all value factors, but unbiased
	 * - without exploration factor), from black's perspective. */
    /*从Black的角度看，主树节点的值（具有所有的值因子，但没有偏倚-没有探索因子）。*/
	struct move_stats value;
};


typedef struct tree_node *(*uctp_choose)(struct uct_policy *p, struct tree_node *node, struct board *b, enum stone color, coord_t exclude);
typedef floating_t (*uctp_evaluate)(struct uct_policy *p, struct tree *tree, struct uct_descent *descent, int parity);
typedef void (*uctp_descend)(struct uct_policy *p, struct tree *tree, struct uct_descent *descent, int parity, bool allow_pass);
typedef void (*uctp_winner)(struct uct_policy *p, struct tree *tree, struct uct_descent *descent);
typedef void (*uctp_prior)(struct uct_policy *p, struct tree *tree, struct tree_node *node, struct board *b, enum stone color, int parity);
typedef void (*uctp_update)(struct uct_policy *p, struct tree *tree, struct tree_node *node, enum stone node_color, enum stone player_color, struct playout_amafmap *amaf, struct board *final_board, floating_t result);
typedef void (*uctp_done)(struct uct_policy *p);

struct uct_policy {
	struct uct *uct;
	uctp_choose choose;
	uctp_winner winner;
	uctp_evaluate evaluate;
	uctp_descend descend;
	uctp_update update;
	uctp_prior prior;
	uctp_done done;
	bool wants_amaf;
	void *data;
};

#endif
