#ifndef PACHI_ENGINE_H
#define PACHI_ENGINE_H

#include "board.h"
#include "move.h"
#include "gtp.h"

struct move_queue;

typedef struct engine *(*engine_init_t)(char *arg, struct board *b);
typedef enum parse_code (*engine_notify_t)(struct engine *e, struct board *b, int id, char *cmd, char *args, char **reply);
typedef void (*engine_board_print_t)(struct engine *e, struct board *b, FILE *f);
typedef char *(*engine_notify_play_t)(struct engine *e, struct board *b, struct move *m, char *enginearg);
typedef char *(*engine_undo_t)(struct engine *e, struct board *b);
typedef char *(*engine_result_t)(struct engine *e, struct board *b);
typedef char *(*engine_chat_t)(struct engine *e, struct board *b, bool in_game, char *from, char *cmd);
//genmove 函数指针声明
typedef coord_t (*engine_genmove_t)(struct engine *e, struct board *b, struct time_info *ti, enum stone color, bool pass_all_alive);
typedef void  (*engine_best_moves_t)(struct engine *e, struct board *b, struct time_info *ti, enum stone color, 
				     coord_t *best_c, float *best_r, int nbest);
typedef char *(*engine_genmoves_t)(struct engine *e, struct board *b, struct time_info *ti, enum stone color,
				 char *args, bool pass_all_alive, void **stats_buf, int *stats_size);
typedef void (*engine_evaluate_t)(struct engine *e, struct board *b, struct time_info *ti, floating_t *vals, enum stone color);
typedef void (*engine_dead_group_list_t)(struct engine *e, struct board *b, struct move_queue *mq);
typedef void (*engine_stop_t)(struct engine *e);
typedef void (*engine_done_t)(struct engine *e);
typedef struct board_ownermap* (*engine_ownermap_t)(struct engine *e, struct board *b);
typedef void (*engine_livegfx_hook_t)(struct engine *e);

/* This is engine data structure. A new engine instance is spawned
 * for each new game during the program lifetime. */
/*这是引擎数据结构。生成新的引擎实例在程序生命周期内的每一个新游戏。*/
struct engine {
	char *name;
	char *comment;

	/* If set, do not reset the engine state on clear_board. */
     /*如果已设置，请勿在清除板上重置发动机状态。*/
	bool keep_on_clear;
    //这些是函数指针
	engine_notify_t          notify;
	engine_board_print_t     board_print;
	engine_notify_play_t     notify_play;
	engine_chat_t            chat;
	engine_undo_t            undo;
	engine_result_t          result;

	/* Generate a move. If pass_all_alive is true, <pass> shall be generated only
	 * if all stones on the board can be considered alive, without regard to "dead"
	 * considered stones. */
    /*生成移动。如果pass-all-live为真，则仅应生成<pass>
     * 如果董事会上所有的石头都被认为是活的，而不考虑“死的”
     * 被认为是石头。*/
	engine_genmove_t         genmove;

	/* Used by distributed engine */
    /*分布式引擎使用*/
	engine_genmoves_t        genmoves;

	/* List best moves for current position. */
    /*分布式引擎列表使用当前位置的最佳移动。*/
	engine_best_moves_t      best_moves;

	/* Evaluate feasibility of player @color playing at all free moves. Will
	 * simulate each move from b->f[i] for time @ti, then set
	 * 1-max(opponent_win_likelihood) in vals[i]. */
     /*评估玩家@color在所有自由移动中玩游戏的可行性。威尔
      * 从b->f[i]模拟每次移动的时间@ti，然后在vals[i]中设置1-max（对手获胜概率）。*/
	engine_evaluate_t        evaluate;

	/* One dead group per queued move (coord_t is (ab)used as group_t). */
    /*每个排队移动一个死组（coord_t is（ab）用作组t）。*/
	engine_dead_group_list_t dead_group_list;

	/* Pause any background thinking being done, but do not tear down
	 * any data structures yet. */
    /*暂停正在进行的任何后台思考，但不要破坏任何数据结构。*/
	engine_stop_t            stop;

	/* e->data and e will be free()d by caller afterwards. */
    /*e->data和e随后将被调用方释放（）d。*/
	engine_done_t            done;

	/* Return current ownermap, if engine supports it. */
    /*如果引擎支持，返回当前所有者地图。*/
	engine_ownermap_t        ownermap;
	
	/* GoGui hook */
    /*GoGui钩*/
	engine_livegfx_hook_t   livegfx_hook;
	
	void *data; //一个空类型指针，表示初始化的时候，记录引擎状态
};

static inline void
engine_board_print(struct engine *e, struct board *b, FILE *f)
{
	(e->board_print ? e->board_print(e, b, f) : board_print(b, f));
}

static inline void
engine_done(struct engine *e)
{
	if (e->done) e->done(e);
	if (e->data) free(e->data);
	free(e);
}

/* For engines best_move(): Add move @c with prob @r to best moves @best_c, @best_r */
static inline void
best_moves_add(coord_t c, float r, coord_t *best_c, float *best_r, int nbest)
{
	for (int i = 0; i < nbest; i++)
		if (r > best_r[i]) {
			for (int j = nbest - 1; j > i; j--) { // shift
				best_r[j] = best_r[j - 1];
				best_c[j] = best_c[j - 1];
			}
			best_r[i] = r;
			best_c[i] = c;
			break;
		}
}

static inline void
best_moves_add_full(coord_t c, float r, void *d, coord_t *best_c, float *best_r, void **best_d, int nbest)
{
	for (int i = 0; i < nbest; i++)
		if (r > best_r[i]) {
			for (int j = nbest - 1; j > i; j--) { // shift
				best_r[j] = best_r[j - 1];
				best_c[j] = best_c[j - 1];
				best_d[j] = best_d[j - 1];
			}
			best_r[i] = r;
			best_c[i] = c;
			best_d[i] = d;
			break;
		}
}


#endif
