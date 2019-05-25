#ifndef PACHI_UCT_SEARCH_H
#define PACHI_UCT_SEARCH_H

/* MCTS Search infrastructure. We juggle the search threads and
 * control search duration. */

/* uct.c provides the GTP interface and engine setup. */
/* walk.c controls repeated walking of the MCTS tree within
 * the search threads. */

#include <signal.h> // sig_atomic_t

#include "debug.h"
#include "move.h"
#include "ownermap.h"
#include "playout.h"
#include "timeinfo.h"
#include "uct/internal.h"

struct tree;
struct tree_node;

/* Internal UCT structures */

/* How often to inspect the tree from the main thread to check for playout
 * stop, progress reports, etc. (in seconds) */
#define TREE_BUSYWAIT_INTERVAL 0.1 /* 100ms */


/* Thread manager state */
/*线程管理器状态*/
extern volatile sig_atomic_t uct_halt;
extern bool thread_manager_running;

/* Search thread context */
/*搜索线程上下文*/
struct uct_thread_ctx {
	int tid; //线程id
	struct uct *u;//引擎中的数据，传入字引擎
	struct board *b;//棋盘大小
	enum stone color;//执子颜色
	struct tree *t;//蒙特卡洛树
	unsigned long seed;//随机种子
	int games;//总共玩了多少次//传出参数
	struct time_info *ti;//时间限制
};


/* Progress information of the on-going MCTS search - when did we
 * last adjusted dynkomi, printed out stuff, etc. */
/*正在进行的MCTS搜索的进度信息-我们上次调整Dynkomi、打印出的内容等是什么时候？*/
struct uct_search_state {
	/* Number of games simulated for this simulation before
	 * we started the search. (We have simulated them earlier.) */
    /*在我们开始搜索之前为此模拟模拟模拟的游戏数。（我们之前已经对它们进行了模拟。）*/
	int base_playouts;
	/* Number of last dynkomi adjustment. */
    /*上次Dynkomi调整的次数。*/
	int last_dynkomi;
	/* Number of last game with progress print. */
    /*带有进度打印的上一个游戏的数目。*/
	int last_print;
	/* Number of simulations to wait before next print. */
    /*下次打印前等待的模拟数。*/
	int print_interval;
	/* Printed notification about full memory? */
    /*关于内存已满的打印通知*/
	bool fullmem;

	struct time_stop stop;
	struct uct_thread_ctx *ctx;
};

int uct_search_games(struct uct_search_state *s);

void uct_search_start(struct uct *u, struct board *b, enum stone color, struct tree *t, struct time_info *ti, struct uct_search_state *s);
struct uct_thread_ctx *uct_search_stop(void);

void uct_search_progress(struct uct *u, struct board *b, enum stone color, struct tree *t, struct time_info *ti, struct uct_search_state *s, int i);

bool uct_search_check_stop(struct uct *u, struct board *b, enum stone color, struct tree *t, struct time_info *ti, struct uct_search_state *s, int i);

struct tree_node *uct_search_result(struct uct *u, struct board *b, enum stone color, bool pass_all_alive, int played_games, int base_playouts, coord_t *best_coord);

#endif
