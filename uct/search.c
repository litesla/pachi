#include <assert.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEBUG

#include "debug.h"
#include "distributed/distributed.h"
#include "move.h"
#include "random.h"
#include "timeinfo.h"
#include "uct/dynkomi.h"
#include "uct/internal.h"
#include "uct/search.h"
#include "uct/tree.h"
#include "uct/uct.h"
#include "uct/walk.h"


/* Default time settings for the UCT engine. In distributed mode, slaves are
 * unlimited by default and all control is done on the master, either in time
 * or with total number of playouts over all slaves. (It is also possible but
 * not recommended to limit only the slaves; the master then decides the move
 * when a majority of slaves have made their choice.) */
static struct time_info default_ti;
static __attribute__((constructor)) void
default_ti_init(void)
{
	time_parse(&default_ti, "10");
}

static const struct time_info unlimited_ti = {
	.period = TT_MOVE,
	.dim = TD_GAMES,
	.len = { .games = INT_MAX, .games_max = 0 },
};

/* When terminating UCT search early, the safety margin to add to the
 * remaining playout number estimate when deciding whether the result can
 * still change. */
#define PLAYOUT_DELTA_SAFEMARGIN 1000

/* Minimal number of simulations to consider early break. */
#define PLAYOUT_EARLY_BREAK_MIN 5000

/* Minimal time to consider early break (in seconds). */
#define TIME_EARLY_BREAK_MIN 1.0


/* Pachi threading structure:
 *
 * main thread
 *   |         main(), GTP communication, ...
 *   |         starts and stops the search managed by thread_manager
 *   |
 * thread_manager
 *   |         spawns and collects worker threads
 *   |
 * worker0
 * worker1
 * ...
 * workerK
 *             uct_playouts() loop, doing descend-playout until uct_halt
 *
 * Another way to look at it is by functions (lines denote thread boundaries):
 *
 * | uct_genmove()
 * | uct_search()            (uct_search_start() .. uct_search_stop())
 * | -----------------------
 * | spawn_thread_manager()
 * | -----------------------
 * | spawn_worker()
 * V uct_playouts() */

/* Set in thread manager in case the workers should stop. */
volatile sig_atomic_t uct_halt = 0;
/* ID of the thread manager. */
static pthread_t thread_manager;
bool thread_manager_running;

static pthread_mutex_t finish_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t finish_cond = PTHREAD_COND_INITIALIZER;
static volatile int finish_thread;
static pthread_mutex_t finish_serializer = PTHREAD_MUTEX_INITIALIZER;
//子线程　游戏的多次玩的次数
static void *
spawn_worker(void *ctx_)
{
	struct uct_thread_ctx *ctx = ctx_;
	/* Setup */
	fast_srandom(ctx->seed);
	/* Run */
    //若干次模拟，games表示当前线程模拟了几次	
	ctx->games = uct_playouts(ctx->u, ctx->b, ctx->color, ctx->t, ctx->ti);
	/* Finish */
	pthread_mutex_lock(&finish_serializer); //获取串行结束锁
	pthread_mutex_lock(&finish_mutex);//　获取mutex锁
	finish_thread = ctx->tid; //设置线程ｉｄ
	pthread_cond_signal(&finish_cond);//发送条件信号
	pthread_mutex_unlock(&finish_mutex);
	return ctx;
}

/* Thread manager, controlling worker threads. It must be called with
 * finish_mutex lock held, but it will unlock it itself before exiting;
 * this is necessary to be completely deadlock-free. */
/* The finish_cond can be signalled for it to stop; in that case,
 * the caller should set finish_thread = -1. */
/* After it is started, it will update mctx->t to point at some tree
 * used for the actual search, on return
 * it will set mctx->games to the number of performed simulations. */
/*线程管理器，控制工作线程。必须在保持finish_mutex锁的情况下调用它，但它会在退出前将其自身解锁；这是完全无死锁所必需的。*/
/*完成秒可以发出信号让它停止；在这种情况下，调用者应该设置完成线程=-1。*/
/*启动后，它将更新mctx->t，指向用于实际搜索的树，返回时将mctx->games设置为已执行模拟的数量。*/
//监工线程
static void *
spawn_thread_manager(void *ctx_)
{
	/* In thread_manager, we use only some of the ctx fields. */
	struct uct_thread_ctx *mctx = ctx_;
	struct uct *u = mctx->u;
	struct tree *t = mctx->t;
	fast_srandom(mctx->seed);

	int played_games = 0;
	pthread_t threads[u->threads];//线程代号
	int joined = 0;//代表由多少个线程结束了

	uct_halt = 0;//引擎是否停机

	/* Garbage collect the tree by preference when pondering. */
	if (u->pondering && t->nodes && t->nodes_size >= t->pruning_threshold) {
        //当无用的节点　过多之后　进行回收
		t->root = tree_garbage_collect(t, t->root);//垃圾回收
	}

	/* Make sure the root node is expanded. */
	{
		enum stone player_color = mctx->color;
		struct tree_node *n = t->root;
		enum stone node_color = stone_other(player_color);
		assert(node_color == t->root_color);
		
		if (tree_leaf_node(n) && !__sync_lock_test_and_set(&n->is_expanded, 1))
			tree_expand_node(t, n, mctx->b, player_color, u, 1);
	}
	
	/* Spawn threads... 开启线程*/
	for (int ti = 0; ti < u->threads; ti++) {
        //设置线程属性
		struct uct_thread_ctx *ctx = malloc2(sizeof(*ctx));
		ctx->u = u; ctx->b = mctx->b; ctx->color = mctx->color;
		mctx->t = ctx->t = t;
		ctx->tid = ti; ctx->seed = fast_random(65536) + ti;
		ctx->ti = mctx->ti;
		pthread_attr_t a;
		pthread_attr_init(&a);
		pthread_attr_setstacksize(&a, 1048576); //默认一个线程是８ｍ　这个栈空间２＾２０
		pthread_create(&threads[ti], &a, spawn_worker, ctx);//开启线程
		if (UDEBUGL(4))
			fprintf(stderr, "Spawned worker %d\n", ti);
	}

	/* ...and collect them back:　由多少个线程结束 等待多个线程结束*/
	while (joined < u->threads) {
		/* Wait for some thread to finish... */
        /*等待一些线程完成..*/
        /*把该线程唤醒，使pthread_cond_wait()通过（返回）时，该线程又自动获得该mutex。*/
        /*例程用于发信号（或唤醒）正在等待条件变量的另一个线程。它应该在锁定互斥锁后调用，并且必须解锁互斥锁才能完成pthread_cond_wait（）例程。*/
		pthread_cond_wait(&finish_cond, &finish_mutex);//当你想发送这个信号的时候，请你进行这个锁，一个一个来处理结束的字线程条件等待
		/*条件等待，信号量第一个，这个互斥锁一定是被锁上的，一定是属于这个线程的，当这个锁被释放以后，当你释放这个信号的时候，请你枪战这个锁，只有这样才能一个一个的结束线程的事情*/
        if (finish_thread < 0) { //什么情况下小于０　
			/* Stop-by-caller. Tell the workers to wrap up
			 * and unblock them from terminating. */
            /*由呼叫方停止。告诉工人们结束工作并解除他们对终止工作的封锁。*/
			uct_halt = 1;//这个是停机标志
			/* We need to make sure the workers do not complete
			 * the termination sequence before we get officially
			 * stopped - their wake and the stop wake could get
			 * coalesced. */
            /*我们需要确保工人们在我们正式停止之前没有完成终止程序——他们的唤醒和停止唤醒可以合并。*/
			pthread_mutex_unlock(&finish_serializer);//解锁串行锁
			continue;
		}
		/* ...and gather its remnants. */
		struct uct_thread_ctx *ctx;
		pthread_join(threads[finish_thread], (void **) &ctx);//等待线程结束。如果线程结束，赋值给ｃｔｘ
		played_games += ctx->games; 
		joined++;//
		free(ctx);
		if (UDEBUGL(4))
			fprintf(stderr, "Joined worker %d\n", finish_thread);
		pthread_mutex_unlock(&finish_serializer);//解锁串行结束锁
	}

    //注意所有进程结束之后，ｍｉｕｔｅｓ锁还是被锁着呢 在pthread_cond_wait 中被锁着
	pthread_mutex_unlock(&finish_mutex);
    //讲结果写入一个结构，进行返回
	mctx->games = played_games;
	return mctx;
}


/*** THREAD MANAGER end */

/*** Search infrastructure: */
/*搜索基础结构*/
//搜索的里面两个值，一个搜索的次数，另一个是
int
uct_search_games(struct uct_search_state *s)
{
	return s->ctx->t->root->u.playouts;
}

//开始搜索
void
uct_search_start(struct uct *u, struct board *b, enum stone color,
		 struct tree *t, struct time_info *ti,
		 struct uct_search_state *s)
{
	/* Set up search state. */
    /*设置搜索状态*/
	s->base_playouts = s->last_dynkomi = s->last_print = t->root->u.playouts;
	s->print_interval = u->reportfreq;
	s->fullmem = false;

	if (ti) {
		if (ti->period == TT_NULL) {
			if (u->slave) {
				*ti = unlimited_ti;
			} else {
				*ti = default_ti;
				time_start_timer(ti);
			}
		}
		time_stop_conditions(ti, b, u->fuseki_end, u->yose_start, u->max_maintime_ratio, &s->stop);
	}

	/* Fire up the tree search thread manager, which will in turn
	 * spawn the searching threads. */
    /*启动树搜索线程管理器，这将反过来生成搜索线程。*/
	assert(u->threads > 0);
	assert(!thread_manager_running);
	static struct uct_thread_ctx mctx;
    /*万能初始化方式*/
	mctx = (struct uct_thread_ctx) { .u = u, .b = b, .color = color, .t = t, .seed = fast_random(65536), .ti = ti };
	s->ctx = &mctx;//将他的地址复制过去
    //获取两个互斥所
    //第一个串行结束锁
	pthread_mutex_lock(&finish_serializer);//加锁的顺序能换吗
    //线程互斥锁
	pthread_mutex_lock(&finish_mutex);
    //开启一个监工小弟　两个线程并行执行
	pthread_create(&thread_manager, NULL, spawn_thread_manager, s->ctx);
	thread_manager_running = true;//不能进行交换
}
//停止搜索
struct uct_thread_ctx *
uct_search_stop(void)
{
    //assert的作用是先计算表达式 expression ，如果其值为假（即为0），那么它先向stderr打印一条出错信息，然后通过调用 abort 来终止程序运行
	assert(thread_manager_running);

	/* Signal thread manager to stop the workers. */
    /*通知线程管理器停止工作。*/
	pthread_mutex_lock(&finish_mutex); //获取finish_thread锁
	finish_thread = -1;//停止搜索标志
	pthread_cond_signal(&finish_cond);//发送量
	pthread_mutex_unlock(&finish_mutex);//解锁

	/* Collect the thread manager. */
    /*收集线程管理器*/
	struct uct_thread_ctx *pctx;
	thread_manager_running = false;
	pthread_join(thread_manager, (void **) &pctx);
	return pctx;
}

//进展
void
uct_search_progress(struct uct *u, struct board *b, enum stone color,
		    struct tree *t, struct time_info *ti,
		    struct uct_search_state *s, int i)
{
	struct uct_thread_ctx *ctx = s->ctx;

	/* Adjust dynkomi? */
	int di = u->dynkomi_interval * u->threads;
	if (ctx->t->use_extra_komi && u->dynkomi->permove
	    && !u->pondering && di
	    && i > s->last_dynkomi + di) {
		s->last_dynkomi += di;
		floating_t old_dynkomi = ctx->t->extra_komi;
		ctx->t->extra_komi = u->dynkomi->permove(u->dynkomi, b, ctx->t);
		if (UDEBUGL(3) && old_dynkomi != ctx->t->extra_komi)
			fprintf(stderr, "dynkomi adjusted (%f -> %f)\n",
				old_dynkomi, ctx->t->extra_komi);
	}

	/* Print progress? */
    /*打印进度*/
	if (i - s->last_print > s->print_interval) {
		s->last_print += s->print_interval; // keep the numbers tidy 保持数tidy
		uct_progress_status(u, ctx->t, color, s->last_print, NULL);
	}

	if (!s->fullmem && ctx->t->nodes_size > u->max_tree_size) {
		char *msg = "WARNING: Tree memory limit reached, stopping search.\n"
			    "Try increasing max_tree_size.\n";
		if (UDEBUGL(2))  fprintf(stderr, "%s", msg);
#ifdef _WIN32
		popup(msg);
#endif
		s->fullmem = true;
	}
}


/* Determine whether we should terminate the search early. */
/*确定是否应提前终止搜索。*/
static bool
uct_search_stop_early(struct uct *u, struct tree *t, struct board *b,
		struct time_info *ti, struct time_stop *stop,
		struct tree_node *best, struct tree_node *best2,
		int played, bool fullmem)
{
	/* If the memory is full, stop immediately. Since the tree
	 * cannot grow anymore, some non-well-expanded nodes will
	 * quickly take over with extremely high ratio since the
	 * counters are not properly simulated (just as if we use
	 * non-UCT MonteCarlo). */
	/* (XXX: A proper solution would be to prune the tree
	 * on the spot.) */
    /*如果内存已满，请立即停止。由于树不能再生长，一些未很好扩展的节点将以极高的比率迅速接管，因为计数器没有正确模拟（就像我们使用非UCT蒙特卡罗一样）。*/
    /*（一个合适的解决办法是修剪这个地方的树。）*/
	if (fullmem)
		return true;

	/* Think at least 100ms to avoid a random move. This is particularly
	 * important in distributed mode, where this function is called frequently. */
    /*至少思考100毫秒以避免随机移动。在分布式模式中，这一点尤为重要，在这种模式下，频繁调用此函数。*/
	double elapsed = 0.0;
	if (ti->dim == TD_WALLTIME) {
		elapsed = time_now() - ti->len.t.timer_start;
		if (elapsed < TREE_BUSYWAIT_INTERVAL) return false;
	}

	/* Break early if we estimate the second-best move cannot
	 * catch up in assigned time anymore. We use all our time
	 * if we are in byoyomi with single stone remaining in our
	 * period, however - it's better to pre-ponder. */
    /*如果我们估计第二个最好的动作不能在指定的时间内赶上，就提前休息。但是，如果我们在拜约米，在我们这个时代只剩下一块石头，那我们就用我们所有的时间——最好是预先考虑一下。*/
	bool time_indulgent = (!ti->len.t.main_time && ti->len.t.byoyomi_stones == 1);
	if (best2 && ti->dim == TD_WALLTIME
	    && played >= PLAYOUT_EARLY_BREAK_MIN && !time_indulgent) {
		double remaining = stop->worst.time - elapsed;
		double pps = ((double)played) / elapsed;
		double estplayouts = remaining * pps + PLAYOUT_DELTA_SAFEMARGIN;
		if (best->u.playouts > best2->u.playouts + estplayouts) {
			if (UDEBUGL(2))
				fprintf(stderr, "Early stop, result cannot change: "
					"best %d, best2 %d, estimated %f simulations to go (%d/%f=%f pps)\n",
					best->u.playouts, best2->u.playouts, estplayouts, played, elapsed, pps);
			return true;
		}
	}

	/* Early break in won situation. */
    /*早日打破胜局。*/
	if (best->u.playouts >= PLAYOUT_EARLY_BREAK_MIN
	    && (ti->dim != TD_WALLTIME || elapsed > TIME_EARLY_BREAK_MIN)
	    && tree_node_get_value(t, 1, best->u.value) >= u->sure_win_threshold) {
		return true;
	}

	return false;
}

/* Determine whether we should terminate the search later than expected. */
/*确定是否应比预期晚终止搜索。*/
static bool
uct_search_keep_looking(struct uct *u, struct tree *t, struct board *b,
		struct time_info *ti, struct time_stop *stop,
		struct tree_node *best, struct tree_node *best2,
		struct tree_node *bestr, struct tree_node *winner, int i)
{
	if (!best) {
		if (UDEBUGL(2))
			fprintf(stderr, "Did not find best move, still trying...\n");
		return true;
	}

	/* Do not waste time if we are winning. Spend up to worst time if
	 * we are unsure, but only desired time if we are sure of winning. */
	floating_t beta = 2 * (tree_node_get_value(t, 1, best->u.value) - 0.5);
	if (ti->dim == TD_WALLTIME && beta > 0) {
		double good_enough = stop->desired.time * beta + stop->worst.time * (1 - beta);
		double elapsed = time_now() - ti->len.t.timer_start;
		if (elapsed > good_enough) return false;
	}

	if (u->best2_ratio > 0) {
		/* Check best/best2 simulations ratio. If the
		 * two best moves give very similar results,
		 * keep simulating. */
		if (best2 && best2->u.playouts
		    && (double)best->u.playouts / best2->u.playouts < u->best2_ratio) {
			if (UDEBUGL(3))
				fprintf(stderr, "Best2 ratio %f < threshold %f\n",
					(double)best->u.playouts / best2->u.playouts,
					u->best2_ratio);
			return true;
		}
	}

	if (u->bestr_ratio > 0) {
		/* Check best, best_best value difference. If the best move
		 * and its best child do not give similar enough results,
		 * keep simulating. */
		if (bestr && bestr->u.playouts
		    && fabs((double)best->u.value - bestr->u.value) > u->bestr_ratio) {
			if (UDEBUGL(3))
				fprintf(stderr, "Bestr delta %f > threshold %f\n",
					fabs((double)best->u.value - bestr->u.value),
					u->bestr_ratio);
			return true;
		}
	}

	if (winner && winner != best) {
		/* Keep simulating if best explored
		 * does not have also highest value. */
		if (UDEBUGL(3))
			fprintf(stderr, "[%d] best %3s [%d] %f != winner %3s [%d] %f\n", i,
				coord2sstr(node_coord(best), t->board),
				best->u.playouts, tree_node_get_value(t, 1, best->u.value),
				coord2sstr(node_coord(winner), t->board),
				winner->u.playouts, tree_node_get_value(t, 1, winner->u.value));
		return true;
	}

	/* No reason to keep simulating, bye. */
	return false;
}

bool
uct_search_check_stop(struct uct *u, struct board *b, enum stone color,
		      struct tree *t, struct time_info *ti,
		      struct uct_search_state *s, int i)
{
	struct uct_thread_ctx *ctx = s->ctx;

	/* Never consider stopping if we played too few simulations.
	 * Maybe we risk losing on time when playing in super-extreme
	 * time pressure but the tree is going to be just too messed
	 * up otherwise - we might even play invalid suicides or pass
	 * when we mustn't. */
    /*如果我们玩的模拟太少，就不要考虑停下来。也许我们在超极端的时间压力下会有时间损失的风险，但是树会变得太乱，否则-我们甚至可能玩无效的自杀游戏，或者在不允许的情况下传球。*/
	assert(!(ti->dim == TD_GAMES && ti->len.games < GJ_MINGAMES));
	if (i < GJ_MINGAMES)
		return false;

	struct tree_node *best = NULL;
	struct tree_node *best2 = NULL; // Second-best move. 第二好动作。
	struct tree_node *bestr = NULL; // best's best child.　贝斯特最好的孩子。
	struct tree_node *winner = NULL;

	best = u->policy->choose(u->policy, ctx->t->root, b, color, resign);
	if (best) best2 = u->policy->choose(u->policy, ctx->t->root, b, color, node_coord(best));

	/* Possibly stop search early if it's no use to try on. */
    /*如果没用的话，可以提前停止搜索。然后直接跳出循环*/
	int played = u->played_all + i - s->base_playouts;
	if (best && uct_search_stop_early(u, ctx->t, b, ti, &s->stop, best, best2, played, s->fullmem))
		return true;

	/* Check against time settings. */
    /*检查时间设置。*/
	bool desired_done;
	if (ti->dim == TD_WALLTIME) {
		double elapsed = time_now() - ti->len.t.timer_start;
		if (elapsed > s->stop.worst.time) return true;
		desired_done = elapsed > s->stop.desired.time;

	} else { assert(ti->dim == TD_GAMES);
		if (i > s->stop.worst.playouts) return true;
		desired_done = i > s->stop.desired.playouts;
	}

	/* We want to stop simulating, but are willing to keep trying
	 * if we aren't completely sure about the winner yet. */
    /*我们想停止模拟，但如果我们还不能完全确定胜利者，我们愿意继续尝试。*/
	if (desired_done) {
		if (u->policy->winner && u->policy->evaluate) {
			struct uct_descent descent = { .node = ctx->t->root };
			u->policy->winner(u->policy, ctx->t, &descent);
			winner = descent.node;
		}
		if (best)
			bestr = u->policy->choose(u->policy, best, b, stone_other(color), resign);
		if (!uct_search_keep_looking(u, ctx->t, b, ti, &s->stop, best, best2, bestr, winner, i))
			return true;
	}

	/* TODO: Early break if best->variance goes under threshold
	 * and we already have enough playouts (possibly thanks to tbook
	 * or to pondering)? */
    /*托多：如果最好的话，早退---方差低于临界值，我们已经有足够的上场时间了（可能是因为tbook或思考）*/
	return false;
}


struct tree_node *
uct_search_result(struct uct *u, struct board *b, enum stone color,
		  bool pass_all_alive, int played_games, int base_playouts,
		  coord_t *best_coord)
{
	/* Choose the best move from the tree. */
    /*从树上选择最佳动作。*/
	struct tree_node *best = u->policy->choose(u->policy, u->t->root, b, color, resign);
	if (!best) {
		*best_coord = pass;
		return NULL;
	}
	*best_coord = node_coord(best);
	if (UDEBUGL(1))
		fprintf(stderr, "*** WINNER is %s (%d,%d) with score %1.4f (%d/%d:%d/%d games), extra komi %f\n",
			coord2sstr(node_coord(best), b), coord_x(node_coord(best), b), coord_y(node_coord(best), b),
			tree_node_get_value(u->t, 1, best->u.value), best->u.playouts,
			u->t->root->u.playouts, u->t->root->u.playouts - base_playouts, played_games,
			u->t->extra_komi);

	/* Do not resign if we're so short of time that evaluation of best
	 * move is completely unreliable, we might be winning actually.
	 * In this case best is almost random but still better than resign. */
    /*不要辞职，如果我们的时间太短，对最佳行动的评估完全不可靠，我们可能真的会赢。在这种情况下，贝斯特几乎是随机的，但仍然比辞职要好。*/
	if (tree_node_get_value(u->t, 1, best->u.value) < u->resign_threshold
	    && !is_pass(node_coord(best))
	    // If only simulated node has been a pass and no other node has
	    // been simulated but pass won't win, an unsimulated node has
	    // been returned; test therefore also for #simulations at root.
        //如果只有模拟节点已通过，而没有其他节点
        //已模拟，但传递不会赢，未模拟的节点
        //已返回；因此也要测试根目录下的模拟。
	    && (best->u.playouts > GJ_MINGAMES || u->t->root->u.playouts > GJ_MINGAMES * 2)
	    && !u->t->untrustworthy_tree) {
		*best_coord = resign;
		return NULL;
	}

	/* If the opponent just passed and we win counting, always
	 * pass as well. For option stones_only, we pass only when there
	 * there is nothing else to do, to show how to maximize score. */
    /*如果对手刚通过，我们就赢了，那么也要一直通过。对于选项石，我们只在没有其他事情可做时通过，以展示如何最大化得分。*/
	if (b->moves > 1 && is_pass(b->last_move.coord) && b->rules != RULES_STONES_ONLY) {
		char *msg;
		if (uct_pass_is_safe(u, b, color, pass_all_alive, &msg)) {
			if (UDEBUGL(0)) {
				float score = -1 * board_official_score(b, &u->dead_groups);
				fprintf(stderr, "<Will rather pass, looks safe enough. Final score: %s%.1f>\n",
					(score > 0 ? "B+" : "W+"), fabs(score));
			}
			*best_coord = pass;
			best = u->t->root->children; // pass is the first child
			assert(is_pass(node_coord(best)));
			return best;
		}
		if (UDEBUGL(0))	fprintf(stderr, "Refusing to pass: %s\n", msg);
	}
	
	return best;
}
