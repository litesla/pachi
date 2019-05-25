#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEBUG

#include "debug.h"
#include "board.h"
#include "gtp.h"
#include "chat.h"
#include "move.h"
#include "mq.h"
#include "engines/josekibase.h"
#include "playout.h"
#include "playout/moggy.h"
#include "playout/light.h"
#include "tactics/util.h"
#include "timeinfo.h"
#include "uct/dynkomi.h"
#include "uct/internal.h"
#include "uct/plugins.h"
#include "uct/prior.h"
#include "uct/search.h"
#include "uct/slave.h"
#include "uct/tree.h"
#include "uct/uct.h"
#include "uct/walk.h"
#include "dcnn.h"

struct uct_policy *policy_ucb1_init(struct uct *u, char *arg);
struct uct_policy *policy_ucb1amaf_init(struct uct *u, char *arg, struct board *board);
static void uct_pondering_start(struct uct *u, struct board *b0, struct tree *t, enum stone color);

/* Maximal simulation length. */
#define MC_GAMELEN	MAX_GAMELEN


static void
setup_state(struct uct *u, struct board *b, enum stone color)
{
	u->t = tree_init(b, color, u->fast_alloc ? u->max_tree_size : 0,
			 u->max_pruned_size, u->pruning_threshold, u->local_tree_aging, u->stats_hbits);
	if (u->initial_extra_komi)
		u->t->extra_komi = u->initial_extra_komi;
	if (u->force_seed)
		fast_srandom(u->force_seed);
	if (UDEBUGL(3))
		fprintf(stderr, "Fresh board with random seed %lu\n", fast_getseed());
	if (!u->no_tbook && b->moves == 0) {
		if (color == S_BLACK) {
			tree_load(u->t, b);
		} else if (DEBUGL(0)) {
			fprintf(stderr, "Warning: First move appears to be white\n");
		}
	}
}

static void
reset_state(struct uct *u)
{
	assert(u->t);
	tree_done(u->t); u->t = NULL;
}

static void
setup_dynkomi(struct uct *u, struct board *b, enum stone to_play)
{
	if (u->t->use_extra_komi && !u->pondering && u->dynkomi->permove)
		u->t->extra_komi = u->dynkomi->permove(u->dynkomi, b, u->t);
	else if (!u->t->use_extra_komi)
		u->t->extra_komi = 0;
}

void
uct_prepare_move(struct uct *u, struct board *b, enum stone color)
{
	if (u->t) {
		/* Verify that we have sane state. */
		assert(b->es == u);
		assert(u->t && b->moves);
		if (color != stone_other(u->t->root_color))
			die("Fatal: Non-alternating play detected %d %d\n", color, u->t->root_color);
		uct_htable_reset(u->t);

	} else {
		/* We need fresh state. */
		b->es = u;
		setup_state(u, b, color);
	}

	board_ownermap_init(&u->ownermap);
	u->played_own = u->played_all = 0;
}

static void
get_dead_groups(struct uct *u, struct board *b, struct move_queue *dead, struct move_queue *unclear)
{
	enum gj_state gs_array[board_size2(b)];
	struct group_judgement gj = { .thres = 0.67, .gs = gs_array };
	board_ownermap_judge_groups(b, &u->ownermap, &gj);
	dead->moves = unclear->moves = 0;
	groups_of_status(b, &gj, GS_DEAD, dead);
	groups_of_status(b, &gj, GS_UNKNOWN, unclear);
}

/* Do we win counting, considering that given groups are dead ?
 * Assumes ownermap is well seeded. */
static bool
pass_is_safe(struct uct *u, struct board *b, enum stone color, struct move_queue *mq,
	     float ownermap_score, bool pass_all_alive, char **msg)
{
	int dame;
	floating_t score = board_official_score_and_dame(b, mq, &dame);
	if (color == S_BLACK)  score = -score;
	//fprintf(stderr, "pass_is_safe():  %d score %f   dame: %i\n", color, score, dame);

	/* Don't go to counting if position is not final.
	 * If ownermap and official score disagree position is likely not final.
	 * If too many dames also. */
	if (!pass_all_alive) {
		*msg = "score est and official score don't agree";
		if (score != ownermap_score)  return false;
		*msg = "too many dames";
		if (dame > 20)	              return false;
	}
	
	*msg = "losing on official score";
	return (score >= 0);
}

bool
uct_pass_is_safe(struct uct *u, struct board *b, enum stone color, bool pass_all_alive, char **msg)
{
	/* Make sure enough playouts are simulated to get a reasonable dead group list. */
	while (u->ownermap.playouts < GJ_MINGAMES)
		uct_playout(u, b, color, u->t);

	/* Save dead groups for final_status_list dead. */
	struct move_queue unclear;
	struct move_queue *mq = &u->dead_groups;
	u->dead_groups_move = b->moves;
	get_dead_groups(u, b, mq, &unclear);
	
	/* Unclear groups ? */
	*msg = "unclear groups";
	if (unclear.moves)  return false;
	
	if (pass_all_alive) {
		*msg = "need to remove opponent dead groups first";
		for (unsigned int i = 0; i < mq->moves; i++)
			if (board_at(b, mq->move[i]) == stone_other(color))
				return false;
		mq->moves = 0; // our dead stones are alive when pass_all_alive is true
	}
	
	if (u->allow_losing_pass) {
		*msg = "unclear point, clarify first";
		foreach_point(b) {
			if (board_at(b, c) == S_OFFBOARD)  continue;
			if (board_ownermap_judge_point(&u->ownermap, c, GJ_THRES) == PJ_UNKNOWN)
				return false;
		} foreach_point_end;
		return true;
	}

	/* Check score estimate first, official score is off if position is not final */
	*msg = "losing on score estimate";
	floating_t score = board_ownermap_score_est(b, &u->ownermap);
	if (color == S_BLACK)  score = -score;
	if (score < 0)  return false;

	return pass_is_safe(u, b, color, mq, score, pass_all_alive, msg);
}

static void
uct_board_print(struct engine *e, struct board *b, FILE *f)
{
	struct uct *u = b->es;
	board_print_ownermap(b, f, (u ? &u->ownermap : NULL));
}

static struct board_ownermap*
uct_ownermap(struct engine *e, struct board *b)
{
	struct uct *u = b->es;
	
	/* Make sure ownermap is well-seeded. */
	if (u->ownermap.playouts < GJ_MINGAMES) {
		enum stone color = stone_other(b->last_move.color);
		uct_pondering_stop(u);
		if (u->t)  reset_state(u);
		uct_prepare_move(u, b, color);	  /* don't clobber u->my_color with uct_genmove_setup() */
		
		while (u->ownermap.playouts < GJ_MINGAMES)
			uct_playout(u, b, color, u->t);
	}
	
	return &u->ownermap;
}

static char *
uct_notify_play(struct engine *e, struct board *b, struct move *m, char *enginearg)
{
	struct uct *u = e->data;
	if (!u->t) {
		/* No state, create one - this is probably game beginning
		 * and we need to load the opening tbook right now. */
		uct_prepare_move(u, b, m->color);
		assert(u->t);
	}

	/* Stop pondering, required by tree_promote_at() */
	uct_pondering_stop(u);
	if (UDEBUGL(2) && u->slave)
		tree_dump(u->t, u->dumpthres);

	if (is_resign(m->coord)) {
		/* Reset state. */
		reset_state(u);
		return NULL;
	}

	/* Promote node of the appropriate move to the tree root. */
	assert(u->t->root);
	if (u->t->untrustworthy_tree | !tree_promote_at(u->t, b, m->coord)) {
		if (UDEBUGL(3)) {
			if (u->t->untrustworthy_tree)
				fprintf(stderr, "Not promoting move node in untrustworthy tree.\n");
			else
				fprintf(stderr, "Warning: Cannot promote move node! Several play commands in row?\n");
		}
		/* Preserve dynamic komi information, though, that is important. */
		u->initial_extra_komi = u->t->extra_komi;
		reset_state(u);
		return NULL;
	}

	/* If we are a slave in a distributed engine, start pondering once
	 * we know which move we actually played. See uct_genmove() about
	 * the check for pass. */
	if (u->pondering_opt && u->slave && m->color == u->my_color && !is_pass(m->coord))
		uct_pondering_start(u, b, u->t, stone_other(m->color));

	return NULL;
}

static char *
uct_undo(struct engine *e, struct board *b)
{
	struct uct *u = e->data;

	if (!u->t) return NULL;
	uct_pondering_stop(u);
	u->initial_extra_komi = u->t->extra_komi;
	reset_state(u);
	return NULL;
}

static char *
uct_result(struct engine *e, struct board *b)
{
	struct uct *u = e->data;
	static char reply[1024];

	if (!u->t)
		return NULL;
	enum stone color = u->t->root_color;
	struct tree_node *n = u->t->root;
	snprintf(reply, 1024, "%s %s %d %.2f %.1f",
		 stone2str(color), coord2sstr(node_coord(n), b),
		 n->u.playouts, tree_node_get_value(u->t, -1, n->u.value),
		 u->t->use_extra_komi ? u->t->extra_komi : 0);
	return reply;
}

static char *
uct_chat(struct engine *e, struct board *b, bool opponent, char *from, char *cmd)
{
	struct uct *u = e->data;

	if (!u->t)
		return generic_chat(b, opponent, from, cmd, S_NONE, pass, 0, 1, u->threads, 0.0, 0.0);

	struct tree_node *n = u->t->root;
	double winrate = tree_node_get_value(u->t, -1, n->u.value);
	double extra_komi = u->t->use_extra_komi && fabs(u->t->extra_komi) >= 0.5 ? u->t->extra_komi : 0;

	return generic_chat(b, opponent, from, cmd, u->t->root_color, node_coord(n), n->u.playouts, 1,
			    u->threads, winrate, extra_komi);
}

static void
print_dead_groups(struct uct *u, struct board *b, struct move_queue *mq)
{
	fprintf(stderr, "dead groups (playing %s)\n", (u->my_color ? stone2str(u->my_color) : "???"));
	if (!mq->moves)
		fprintf(stderr, "  none\n");
	for (unsigned int i = 0; i < mq->moves; i++) {
		fprintf(stderr, "  ");
		foreach_in_group(b, mq->move[i]) {
			fprintf(stderr, "%s ", coord2sstr(c, b));
		} foreach_in_group_end;
		fprintf(stderr, "\n");
	}
}


static void
uct_dead_group_list(struct engine *e, struct board *b, struct move_queue *mq)
{
	struct uct *u = e->data;
	
	/* This means the game is probably over, no use pondering on. */
	uct_pondering_stop(u);
	
	if (u->pass_all_alive)
		return; // no dead groups

	/* Normally last genmove was a pass and we've already figured out dead groups.
	 * Don't recompute dead groups here, result could be different this time and lead to wrong list. */
	if (u->dead_groups_move == b->moves - 1) {
		memcpy(mq, &u->dead_groups, sizeof(*mq));
		print_dead_groups(u, b, mq);
		return;
	}
	
	/* Create mock state */
	if (u->t)  reset_state(u);
	// We need S_BLACK here, but don't clobber u->my_color with uct_genmove_setup() !
	uct_prepare_move(u, b, S_BLACK); 
	
	/* Make sure the ownermap is well-seeded. */
	while (u->ownermap.playouts < GJ_MINGAMES)
		uct_playout(u, b, S_BLACK, u->t);
	/* Show the ownermap: */
	if (DEBUGL(2))
		board_print_ownermap(b, stderr, &u->ownermap);

	struct move_queue unclear;
	get_dead_groups(u, b, mq, &unclear);
	print_dead_groups(u, b, mq);

	/* Clean up the mock state in case we will receive
	 * a genmove; we could get a non-alternating-move
	 * error from uct_prepare_move() in that case otherwise. */
	reset_state(u);
}

static void
uct_stop(struct engine *e)
{
	/* This is called on game over notification. However, an undo
	 * and game resume can follow, so don't panic yet and just
	 * relax and stop thinking so that we don't waste CPU. */
	struct uct *u = e->data;
	uct_pondering_stop(u);
}

static void
uct_done(struct engine *e)
{
	/* This is called on engine reset, especially when clear_board
	 * is received and new game should begin. */
	free(e->comment);

	struct uct *u = e->data;
	uct_pondering_stop(u);
	if (u->t) reset_state(u);
	if (u->dynkomi) u->dynkomi->done(u->dynkomi);

	if (u->policy) u->policy->done(u->policy);
	if (u->random_policy) u->random_policy->done(u->random_policy);
	playout_policy_done(u->playout);
	uct_prior_done(u->prior);
	joseki_done(u->jdict);
	pluginset_done(u->plugins);
}



/* Run time-limited MCTS search on foreground. */
/*前台运行时间有限的MCT搜索*/
static int
uct_search(struct uct *u, struct board *b, struct time_info *ti, enum stone color, struct tree *t, bool print_progress)
{
	struct uct_search_state s;
	uct_search_start(u, b, color, t, ti, &s); //开始搜索 他没有阻塞 他分配一个监工，监工有若干小弟
	if (UDEBUGL(2) && s.base_playouts > 0)
		fprintf(stderr, "<pre-simulated %d games>\n", s.base_playouts);

	/* The search tree is ctx->t. This is currently == . It is important
	 * to reference ctx->t directly since the
	 * thread manager will swap the tree pointer asynchronously. */
    /*搜索树是ctx->t。这是当前的==。直接引用ctx->t很重要，因为线程管理器将异步交换树指针*/
	/* Now, just periodically poll the search tree. */
	/* Note that in case of TD_GAMES, threads will not wait for
	 * the uct_search_check_stop() signalization. */
    /*现在，只需定期轮询搜索树。*/
    /*注意，在TD-U游戏中，线程不会等待
     * *UCT搜索检查停止（）信号。*/
	while (1) {
        //睡眠一个时长
		time_sleep(TREE_BUSYWAIT_INTERVAL);
		/* TREE_BUSYWAIT_INTERVAL should never be less than desired time, or the
		 * time control is broken. But if it happens to be less, we still search
		 * at least 100ms otherwise the move is completely random. */
        /*树总线等待间隔不得小于所需时间，否则时间控制将中断。但是如果它恰好是较少的，我们仍然搜索至少100毫秒，否则移动是完全随机的。*/
        //返回总模拟的总次数
		int i = uct_search_games(&s);
		/* Print notifications etc. */
        /*打印通知等*/
		uct_search_progress(u, b, color, t, ti, &s, i);
		/* Check if we should stop the search. */
        /*检查我们是否应该停止搜索。如果可以就跳出循环　这里面考虑了一种是否提前开始搜索，另一种是否是否延迟搜索*/
		if (uct_search_check_stop(u, b, color, t, ti, &s, i))
			break;
	}

	struct uct_thread_ctx *ctx = uct_search_stop();
	if (UDEBUGL(3)) tree_dump(t, u->dumpthres);
	if (UDEBUGL(2))
		fprintf(stderr, "(avg score %f/%d; dynkomi's %f/%d value %f/%d)\n",
			t->avg_score.value, t->avg_score.playouts,
			u->dynkomi->score.value, u->dynkomi->score.playouts,
			u->dynkomi->value.value, u->dynkomi->value.playouts);
	if (print_progress)
		uct_progress_status(u, t, color, ctx->games, NULL);

	if (u->debug_after.playouts > 0) {
		/* Now, start an additional run of playouts, single threaded. */
        /*现在，开始一个额外的单线程播放。*/
		struct time_info debug_ti = {
			.period = TT_MOVE,
			.dim = TD_GAMES,
		};
		debug_ti.len.games = t->root->u.playouts + u->debug_after.playouts;
		debug_ti.len.games_max = 0;

		board_print_ownermap(b, stderr, &u->ownermap);
		fprintf(stderr, "--8<-- UCT debug post-run begin (%d:%d) --8<--\n", u->debug_after.level, u->debug_after.playouts);

		int debug_level_save = debug_level;
		int u_debug_level_save = u->debug_level;
		int p_debug_level_save = u->playout->debug_level;
		debug_level = u->debug_after.level;
		u->debug_level = u->debug_after.level;
		u->playout->debug_level = u->debug_after.level;
		uct_halt = false;

		uct_playouts(u, b, color, t, &debug_ti);
		tree_dump(t, u->dumpthres);

		uct_halt = true;
		debug_level = debug_level_save;
		u->debug_level = u_debug_level_save;
		u->playout->debug_level = p_debug_level_save;

		fprintf(stderr, "--8<-- UCT debug post-run finished --8<--\n");
	}

	u->played_own += ctx->games;
	return ctx->games; //返回总模拟次数
}

/* Start pondering background with @color to play. */
/*用@color开始思考背景。*/
static void
uct_pondering_start(struct uct *u, struct board *b0, struct tree *t, enum stone color)
{
	assert(!using_dcnn(b0));
	if (UDEBUGL(1))
		fprintf(stderr, "Starting to ponder with color %s\n", stone2str(stone_other(color)));
	u->pondering = true;

	/* We need a local board copy to ponder upon. */
    /*我们需要一份本地董事会的副本来考虑。*/
	struct board *b = malloc2(sizeof(*b)); board_copy(b, b0);

	/* *b0 did not have the genmove'd move played yet. */
    /**b0尚未播放genmove'd move。*/
	struct move m = { node_coord(t->root), t->root_color };
	int res = board_play(b, &m);
	assert(res >= 0);
	setup_dynkomi(u, b, stone_other(m.color));

	/* Start MCTS manager thread "headless". */
    /*启动MCTS管理器线程“headless”。*/
	static struct uct_search_state s;
	uct_search_start(u, b, color, t, NULL, &s);
}

/* uct_search_stop() frontend for the pondering (non-genmove) mode, and
 * to stop the background search for a slave in the distributed engine. */
/*uct_search_stop（）前端用于思考（非genmove）模式，并停止后台搜索分布式引擎中的从机。*/
void
uct_pondering_stop(struct uct *u)
{
	if (!thread_manager_running)
		return;

	/* Stop the thread manager. */
    //停止线程思考
	struct uct_thread_ctx *ctx = uct_search_stop();
	if (UDEBUGL(1)) {
		if (u->pondering) fprintf(stderr, "(pondering) ");
        //写入停止思考时未收集的信息
		uct_progress_status(u, ctx->t, ctx->color, ctx->games, NULL);
	}
	if (u->pondering) {
		free(ctx->b);
		u->pondering = false;
	}
}


void
uct_genmove_setup(struct uct *u, struct board *b, enum stone color)
{
	if (b->superko_violation) {
		fprintf(stderr, "!!! WARNING: SUPERKO VIOLATION OCCURED BEFORE THIS MOVE\n");
		fprintf(stderr, "Maybe you play with situational instead of positional superko?\n");
		fprintf(stderr, "I'm going to ignore the violation, but note that I may miss\n");
		fprintf(stderr, "some moves valid under this ruleset because of this.\n");
		b->superko_violation = false;
	}

	uct_prepare_move(u, b, color);

	assert(u->t);
	u->my_color = color;

	/* How to decide whether to use dynkomi in this game? Since we use
	 * pondering, it's not simple "who-to-play" matter. Decide based on
	 * the last genmove issued. */
    /*如何决定是否在游戏中使用Dynkomi？因为我们使用思考，这不是简单的“玩谁”的问题。根据上次发布的genmove决定。*/
	u->t->use_extra_komi = !!(u->dynkomi_mask & color);
	setup_dynkomi(u, b, color);

	if (b->rules == RULES_JAPANESE)
		u->territory_scoring = true;

	/* Make pessimistic assumption about komi for Japanese rules to
	 * avoid losing by 0.5 when winning by 0.5 with Chinese rules.
	 * The rules usually give the same winner if the integer part of komi
	 * is odd so we adjust the komi only if it is even (for a board of
	 * odd size). We are not trying  to get an exact evaluation for rare
	 * cases of seki. For details see http://home.snafu.de/jasiek/parity.html */
    /*对小米的日本规则做出悲观的假设，以避免中国规则赢0.5时输0.5。如果komi的整数部分是奇数，规则通常会给出相同的优胜者，因此我们只在komi是偶数时调整它（对于奇数大小的板）。我们不想对罕见的seki病例进行准确的评估。有关详细信息，请参阅http://home.snafu.de/jasiek/parity.html*/
	if (u->territory_scoring && (((int)floor(b->komi) + board_size(b)) & 1)) {
		b->komi += (color == S_BLACK ? 1.0 : -1.0);
		if (UDEBUGL(0))
			fprintf(stderr, "Setting komi to %.1f assuming Japanese rules\n",
				b->komi);
	}
}

static void
uct_livegfx_hook(struct engine *e)
{
	struct uct *u = e->data;
	/* Hack: Override reportfreq to get decent update rates in GoGui */
    /*hack：覆盖reportfreq以在gogui获得合适的更新率*/
	u->reportfreq = MIN(u->reportfreq, 1000);
}

static struct tree_node *
genmove(struct engine *e, struct board *b, struct time_info *ti, enum stone color, bool pass_all_alive, coord_t *best_coord)
{
    //重置一个时间为0
	reset_dcnn_time();
    //获取当前系统的时间，有两个级别
	double start_time = time_now();
    //初始化引擎状态
	struct uct *u = e->data;
	u->pass_all_alive |= pass_all_alive;
	uct_pondering_stop(u);//该你思考的时候思考

	if (using_dcnn(b)) {
		// dcnn hack: reset state to make dcnn priors kick in.
		// FIXME this makes pondering useless when using dcnn ...
		if (u->t) {
			u->initial_extra_komi = u->t->extra_komi;
			reset_state(u);
		}
	}

	uct_genmove_setup(u, b, color);

    /* Start the Monte Carlo Tree Search! */
    /*开始蒙特卡洛搜索*/
	int base_playouts = u->t->root->u.playouts;
	int played_games = uct_search(u, b, ti, color, u->t, false);

	struct tree_node *best;
    //返回最好的结果
	best = uct_search_result(u, b, color, u->pass_all_alive, played_games, base_playouts, best_coord);

	if (UDEBUGL(2)) {
		double total_time = time_now() - start_time + 0.000001; /* avoid divide by zero */
		double mcts_time  = total_time - get_dcnn_time();
		fprintf(stderr, "genmove in %0.2fs (%d games/s, %d games/s/thread)\n",
			total_time, (int)(played_games/mcts_time), (int)(played_games/mcts_time/u->threads));
	}
    //写入本次模拟的信息
	uct_progress_status(u, u->t, color, played_games, best_coord);

	return best;
}

static coord_t
uct_genmove(struct engine *e, struct board *b, struct time_info *ti, enum stone color, bool pass_all_alive)
{
	struct uct *u = e->data;//引擎状态
	coord_t best_coord;
	struct tree_node *best = genmove(e, b, ti, color, pass_all_alive, &best_coord);

	if (!best) {
		/* Pass or resign. */
		if (is_pass(best_coord))
			u->initial_extra_komi = u->t->extra_komi;
		reset_state(u);
		return best_coord;
	}
	
	if (!u->t->untrustworthy_tree) {
		tree_promote_node(u->t, &best);
	} else {
		/* Throw away an untrustworthy tree. */
		/* Preserve dynamic komi information, though, that is important. */
		u->initial_extra_komi = u->t->extra_komi;
		reset_state(u);
	}

	/* After a pass, pondering is harmful for two reasons:
	 * (i) We might keep pondering even when the game is over.
	 * Of course this is the case for opponent resign as well.
	 * (ii) More importantly, the ownermap will get skewed since
	 * the UCT will start cutting off any playouts. */
    /*一次传球之后，思考是有害的，有两个原因：（i）即使比赛结束，我们也可能继续思考。当然，这也是对手辞职的原因。（ii）更重要的是，由于UCT将开始切断任何季后赛，所有人的地图将被扭曲。*/
	if (u->pondering_opt && u->t && !is_pass(node_coord(best))) {
		uct_pondering_start(u, b, u->t, stone_other(color));
	}

	return best_coord;
}

void
uct_get_best_moves(struct tree *t, coord_t *best_c, float *best_r, int nbest, bool winrates)
{
	struct tree_node* best_d[nbest];
	for (int i = 0; i < nbest; i++)  best_c[i] = pass;
	for (int i = 0; i < nbest; i++)  best_r[i] = 0;
	
	/* Find best moves */
	for (struct tree_node *n = t->root->children; n; n = n->sibling)
		best_moves_add_full(node_coord(n), n->u.playouts, n, best_c, best_r, (void**)best_d, nbest);

	if (winrates)  /* Get winrates */
		for (int i = 0; i < nbest && best_c[i] != pass; i++)
			best_r[i] = tree_node_get_value(t, 1, best_d[i]->u.value);
}

/* Kindof like uct_genmove() but find the best candidates */
static void
uct_best_moves(struct engine *e, struct board *b, struct time_info *ti, enum stone color,
	       coord_t *best_c, float *best_r, int nbest)
{
	struct uct *u = e->data;
	uct_pondering_stop(u);
	if (u->t)
		reset_state(u);	
	
	coord_t best_coord;
	genmove(e, b, ti, color, 0, &best_coord);
	uct_get_best_moves(u->t, best_c, best_r, nbest, true);

	if (u->t)	
		reset_state(u);
}

bool
uct_gentbook(struct engine *e, struct board *b, struct time_info *ti, enum stone color)
{
	struct uct *u = e->data;
	if (!u->t) uct_prepare_move(u, b, color);
	assert(u->t);

	if (ti->dim == TD_GAMES) {
		/* Don't count in games that already went into the tbook. */
		ti->len.games += u->t->root->u.playouts;
	}
	uct_search(u, b, ti, color, u->t, true);

	assert(ti->dim == TD_GAMES);
	tree_save(u->t, b, ti->len.games / 100);

	return true;
}

void
uct_dumptbook(struct engine *e, struct board *b, enum stone color)
{
	struct uct *u = e->data;
	struct tree *t = tree_init(b, color, u->fast_alloc ? u->max_tree_size : 0,
			 u->max_pruned_size, u->pruning_threshold, u->local_tree_aging, 0);
	tree_load(t, b);
	tree_dump(t, 0);
	tree_done(t);
}


floating_t
uct_evaluate_one(struct engine *e, struct board *b, struct time_info *ti, coord_t c, enum stone color)
{
	struct uct *u = e->data;

	struct board b2;
	board_copy(&b2, b);
	struct move m = { c, color };
	int res = board_play(&b2, &m);
	if (res < 0)
		return NAN;
	color = stone_other(color);

	if (u->t) reset_state(u);
	uct_prepare_move(u, &b2, color);
	assert(u->t);

	floating_t bestval;
	uct_search(u, &b2, ti, color, u->t, true);
	struct tree_node *best = u->policy->choose(u->policy, u->t->root, &b2, color, resign);
	if (!best) {
		bestval = NAN; // the opponent has no reply!
	} else {
		bestval = tree_node_get_value(u->t, 1, best->u.value);
	}

	reset_state(u); // clean our junk

	return isnan(bestval) ? NAN : 1.0f - bestval;
}

void
uct_evaluate(struct engine *e, struct board *b, struct time_info *ti, floating_t *vals, enum stone color)
{
	for (int i = 0; i < b->flen; i++) {
		if (is_pass(b->f[i]))
			vals[i] = NAN;
		else
			vals[i] = uct_evaluate_one(e, b, ti, b->f[i], color);
	}
}

static void
log_nthreads(struct uct *u)
{
	static int logged = 0;
	if (DEBUGL(0) && !logged++)  fprintf(stderr, "Threads: %i\n", u->threads);
}

static size_t
default_max_tree_size()
{
	/* Double it on 64-bit, tree takes up twice as much memory ... */
	int mult = (sizeof(void*) == 4 ? 1 : 2);

	/* Should be enough for most scenarios (up to 240k playouts ...)
	 * If you're using really long thinking times you definitely should
	 * set a higher max_tree_size. */
	return (size_t)300 * mult * 1048576;
}

//设置uct引擎状态
struct uct *
uct_state_init(char *arg, struct board *b)
{
	struct uct *u = calloc2(1, sizeof(struct uct));
	bool pat_setup = false;
    //引擎状态参数设置
	u->debug_level = debug_level;
	u->reportfreq = 1000;
	u->gamelen = MC_GAMELEN;
	u->resign_threshold = 0.2;
	u->sure_win_threshold = 0.95;
	u->mercymin = 0;
	u->significant_threshold = 50;
	u->expand_p = 8;
	u->dumpthres = 0.01;
	u->playout_amaf = true;
	u->amaf_prior = false;
	u->max_tree_size = default_max_tree_size();
	u->fast_alloc = true;
	u->pruning_threshold = 0;

	u->threads = get_nprocessors();
	u->thread_model = TM_TREEVL;
	u->virtual_loss = 1;

	u->pondering_opt = false;

	u->fuseki_end = 20; // max time at 361*20% = 72 moves (our 36th move, still 99 to play)361*20%时的最长时间=72次移动（我们的第36次移动，仍有99次移动
	u->yose_start = 40; // (100-40-25)*361/100/2 = 63 moves still to play by us then（100-40-25）*361/100/2=63个动作由我们继续玩
	u->bestr_ratio = 0.02;
	// 2.5 is clearly too much, but seems to compensate well for overly stern time allocations.2.5显然太多了，但似乎可以很好地补偿过于严厉的时间分配。
	// TODO: Further tuning and experiments with better time allocation schemes.TODO：进一步的调整和更好的时间分配方案的实验。
	u->best2_ratio = 2.5;
	// Higher values of max_maintime_ratio sometimes cause severe time trouble in tournaments 最大保持时间比率的较高值有时会在锦标赛中造成严重的时间问题。
	// It might be necessary to reduce it to 1.5 on large board, but more tuning is needed.可能有必要在大型板上将其降低到1.5，但需要进行更多的调整。
	u->max_maintime_ratio = 2.0;

	u->val_scale = 0; u->val_points = 40;
	u->dynkomi_interval = 100;
	u->dynkomi_mask = S_BLACK | S_WHITE;

	u->tenuki_d = 4;
	u->local_tree_aging = 80;
	u->local_tree_depth_decay = 1.5;
	u->local_tree_eval = LTE_ROOT;
	u->local_tree_neival = true;

	u->max_slaves = -1;
	u->slave_index = -1;
	u->stats_delay = 0.01; // 10 ms
	u->shared_levels = 1;

	u->plugins = pluginset_init(b);

	u->jdict = joseki_load(b->size);

	if (arg) {
		char *optspec, *next = arg;
		while (*next) {
			optspec = next;
			next += strcspn(next, ",");
			if (*next) { *next++ = 0; } else { *next = 0; }

			char *optname = optspec;
			char *optval = strchr(optspec, '=');
			if (optval) *optval++ = 0;

			/** Basic options */

			if (!strcasecmp(optname, "debug")) {
				if (optval)
					u->debug_level = atoi(optval);
				else
					u->debug_level++;
			} else if (!strcasecmp(optname, "reporting") && optval) {
				/* The format of output for detailed progress
				 * information (such as current best move and
				 * its value, etc.). */
                /*详细进度信息的输出格式（如当前最佳移动及其值等）。*/
				if (!strcasecmp(optval, "text")) {
					/* Plaintext traditional output. 纯文本传统输出。*/
					u->reporting = UR_TEXT;
				} else if (!strcasecmp(optval, "json")) {
					/* JSON output. Implies debug=0. JSON输出。表示debug=0。*/
					u->reporting = UR_JSON;
					u->debug_level = 0;
				} else if (!strcasecmp(optval, "jsonbig")) {
					/* JSON output, but much more detailed.
					 * Implies debug=0. JSON输出，但更详细。表示debug=0。*/
					u->reporting = UR_JSON_BIG;
					u->debug_level = 0;
				} else
					die("UCT: Invalid reporting format %s\n", optval);
			} else if (!strcasecmp(optname, "reportfreq") && optval) {
				/* The progress information line will be shown
				 * every <reportfreq> simulations. 进度信息行将每<reportfreq>次模拟显示一次。*/
				u->reportfreq = atoi(optval);
			} else if (!strcasecmp(optname, "dumpthres") && optval) {
				/* When dumping the UCT tree on output, include
				 * nodes with at least this many playouts.
				 * (A fraction of the total # of playouts at the
				 * tree root.) */
				/* Use 0 to list all nodes with at least one
				 * simulation, and -1 to list _all_ nodes. */
                /*在输出上转储UCT树时，包括至少有这么多播放的节点。（三根的总决赛分数。）
使用0列出至少有一个模拟的所有节点，-1列出所有节点。*/
				u->dumpthres = atof(optval);
			} else if (!strcasecmp(optname, "resign_threshold") && optval) {
				/* Resign when this ratio of games is lost
				 * after GJ_MINGAMES sample is taken. */
                /*当这个比例的游戏在GJ Mingames抽样后丢失时辞职。*/
				u->resign_threshold = atof(optval);
			} else if (!strcasecmp(optname, "sure_win_threshold") && optval) {
				/* Stop reading when this ratio of games is won
				 * after PLAYOUT_EARLY_BREAK_MIN sample is
				 * taken. (Prevents stupid time losses,
				 * friendly to human opponents.) */
                /*当这一比例的游戏是在游戏结束后赢得的时候，停止阅读。（防止愚蠢的时间损失，对人类对手友好。）*/
				u->sure_win_threshold = atof(optval);
			} else if (!strcasecmp(optname, "force_seed") && optval) {
				/* Set RNG seed at the tree setup. */
                /*在树设置中设置RNG SEED。*/
				u->force_seed = atoi(optval);
			} else if (!strcasecmp(optname, "no_tbook")) {
				/* Disable UCT opening tbook. */
                /*禁用UCT打开tBook。*/
				u->no_tbook = true;
			} else if (!strcasecmp(optname, "pass_all_alive")) {
				/* Whether to consider passing only after all
				 * dead groups were removed from the board;
				 * this is like all genmoves are in fact
				 * kgs-genmove_cleanup. */
                /*是否只考虑在所有死亡群体从董事会中移除后才通过；这就像所有的genmoves实际上都是KGS-genmove_清理。*/
				u->pass_all_alive = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "allow_losing_pass")) {
				/* Whether to consider passing in a clear
				 * but losing situation, to be scored as a loss
				 * for us. */
                /*是否考虑在明确但失败的情况下传球，对我们来说是一个损失。*/
				u->allow_losing_pass = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "territory_scoring")) {
				/* Use territory scoring (default is area scoring).
				 * An explicit kgs-rules command overrides this. */
                /*使用区域评分（默认为区域评分）。显式KGS规则命令重写此*/
				u->territory_scoring = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "stones_only")) {
				/* Do not count eyes. Nice to teach go to kids.
				 * http://strasbourg.jeudego.org/regle_strasbourgeoise.htm */
                /*不要数数眼睛。很高兴教给孩子们。http://strasbourg.jeudego.org/regle ou strasbourgeoise.htm*/
				b->rules = RULES_STONES_ONLY;
				u->pass_all_alive = true;
			} else if (!strcasecmp(optname, "debug_after")) {
				/* debug_after=9:1000 will make Pachi think under
				 * the normal conditions, but at the point when
				 * a move is to be chosen, the tree is dumped and
				 * another 1000 simulations are run single-threaded
				 * with debug level 9, allowing inspection of Pachi's
				 * behavior after it has thought a lot. */
                /*debug after=9:1000将使pachi在正常情况下思考，但在选择移动的时候，树被转储，另外1000个模拟使用debug level 9单线程运行，允许在经过深思熟虑后检查pachi的行为。*/
				if (optval) {
					u->debug_after.level = atoi(optval);
					char *playouts = strchr(optval, ':');
					if (playouts)
						u->debug_after.playouts = atoi(playouts+1);
					else
						u->debug_after.playouts = 1000;
				} else {
					u->debug_after.level = 9;
					u->debug_after.playouts = 1000;
				}
			} else if (!strcasecmp(optname, "banner") && optval) {
				/* Additional banner string. This must come as the
				 * last engine parameter. You can use '+' instead
				 * of ' ' if you are wrestling with kgsGtp. */
                /*字符串附加的旗帜。this as the last必备的发动机参数。你可以使用instead of +如果你是摔跤与kgsgtp*/
				if (*next) *--next = ',';
				u->banner = strdup(optval);
				for (char *b = u->banner; *b; b++) {
					if (*b == '+') *b = ' ';
				}
				break;
			} else if (!strcasecmp(optname, "plugin") && optval) {
				/* Load an external plugin; filename goes before the colon,
				 * extra arguments after the colon. */
                /*加载外部插件；文件名位于冒号之前，Xtra参数位于冒号之后。*/
				char *pluginarg = strchr(optval, ':');
				if (pluginarg)
					*pluginarg++ = 0;
				plugin_load(u->plugins, optval, pluginarg);

			/** UCT behavior and policies */
            /*UCT行为和政策*/

			} else if ((!strcasecmp(optname, "policy")
				/* Node selection policy. ucb1amaf is the
				 * default policy implementing RAVE, while
				 * ucb1 is the simple exploration/exploitation
				 * policy. Policies can take further extra
				 * options. */
                /*节点选择策略。UCB1AMAF是实现RAVE的默认策略，而UCB1是简单的勘探/开发策略。政策可以采取进一步的额外选择。*/
			            || !strcasecmp(optname, "random_policy")) && optval) {
				/* A policy to be used randomly with small
				 * chance instead of the default policy. */
				char *policyarg = strchr(optval, ':');
				struct uct_policy **p = !strcasecmp(optname, "policy") ? &u->policy : &u->random_policy;
				if (policyarg)
					*policyarg++ = 0;
				if (!strcasecmp(optval, "ucb1")) {
					*p = policy_ucb1_init(u, policyarg);
				} else if (!strcasecmp(optval, "ucb1amaf")) {
					*p = policy_ucb1amaf_init(u, policyarg, b);
				} else
					die("UCT: Invalid tree policy %s\n", optval);
			} else if (!strcasecmp(optname, "playout") && optval) {
				/* Random simulation (playout) policy.
				 * moggy is the default policy with large
				 * amount of domain-specific knowledge and
				 * heuristics. light is a simple uniformly
				 * random move selection policy. */
                /*随机模拟（播放）策略.moggy是默认策略领域特定知识和启发式方法的数量。光是一个简单的统一和移动选择政策。*/
				char *playoutarg = strchr(optval, ':');
				if (playoutarg)
					*playoutarg++ = 0;
				if (!strcasecmp(optval, "moggy")) {
					u->playout = playout_moggy_init(playoutarg, b, u->jdict);
				} else if (!strcasecmp(optval, "light")) {
					u->playout = playout_light_init(playoutarg, b);
				} else
					die("UCT: Invalid playout policy %s\n", optval);
			} else if (!strcasecmp(optname, "prior") && optval) {
				/* Node priors policy. When expanding a node,
				 * it will seed node values heuristically
				 * (most importantly, based on playout policy
				 * opinion, but also with regard to other
				 * things). See uct/prior.c for details.
				 * Use prior=eqex=0 to disable priors. */
                /*节点优先策略。当扩展一个节点时，它将以启发式方式（最重要的是，基于播放策略的意见，但也涉及到其他事情）种子节点值。详见UCT/Prior.C。使用prior=eqex=0禁用prior。*/
				u->prior = uct_prior_init(optval, b, u);
			} else if (!strcasecmp(optname, "mercy") && optval) {
				/* Minimal difference of black/white captures
				 * to stop playout - "Mercy Rule". Speeds up
				 * hopeless playouts at the expense of some
				 * accuracy. */
                /*黑白截图的最小差异，以阻止播放-“仁慈规则”。以一定的准确性加速无望的决赛。*/
				u->mercymin = atoi(optval);
			} else if (!strcasecmp(optname, "gamelen") && optval) {
				/* Maximum length of single simulation
				 * in moves. */
                /*移动中单个模拟的最大长度。*/
				u->gamelen = atoi(optval);
			} else if (!strcasecmp(optname, "expand_p") && optval) {
				/* Expand UCT nodes after it has been
				 * visited this many times. */
                /*在多次访问之后展开UCT节点。*/
				u->expand_p = atoi(optval);
			} else if (!strcasecmp(optname, "random_policy_chance") && optval) {
				/* If specified (N), with probability 1/N, random_policy policy
				 * descend is used instead of main policy descend; useful
				 * if specified policy (e.g. UCB1AMAF) can make unduly biased
				 * choices sometimes, you can fall back to e.g.
				 * random_policy=UCB1. */
                /*如果指定了（n），概率为1/n，则使用随机策略下降而不是主策略下降；如果指定的策略（例如UCB1AMAF）有时会做出过度偏向的选择，则很有用，您可以返回到随机策略=UCB1。*/
				u->random_policy_chance = atoi(optval);

			/** General AMAF behavior */
			/* (Only relevant if the policy supports AMAF.
			 * More variables can be tuned as policy
			 * parameters.) */
            /*一般AMAF行为*/
             /*（仅当政策支持AMAF时才相关。
            *更多变量可以作为策略进行调整
             *参数。）*/
			} else if (!strcasecmp(optname, "playout_amaf")) {
				/* Whether to include random playout moves in
				 * AMAF as well. (Otherwise, only tree moves
				 * are included in AMAF. Of course makes sense
				 * only in connection with an AMAF policy.) */
				/* with-without: 55.5% (+-4.1) */
                /*是否包含随机播放
                 * AAMF也是如此。（否则，只有树移动
                 * 包含在AMAF中。当然有道理
                 * 仅与AMAF政策相关。）不含：55.5%（+-4.1）*/
				if (optval && *optval == '0')
					u->playout_amaf = false;
				else
					u->playout_amaf = true;
			} else if (!strcasecmp(optname, "playout_amaf_cutoff") && optval) {
				/* Keep only first N% of playout stage AMAF
				 * information. */
                /*仅保留播放阶段AMAF信息的前n%。*/
				u->playout_amaf_cutoff = atoi(optval);
			} else if (!strcasecmp(optname, "amaf_prior") && optval) {
				/* In node policy, consider prior values
				 * part of the real result term or part
				 * of the AMAF term? */
                /*在节点策略中，考虑先验值是实际结果项的一部分还是AMAF项的一部分？*/
				u->amaf_prior = atoi(optval);

			/** Performance and memory management */
            /*性能和内存管理*/

			} else if (!strcasecmp(optname, "threads") && optval) {
				/* By default, Pachi will run with only single
				 * tree search thread! */
                /*默认情况下，pachi只运行一个树搜索线程！*/
				u->threads = atoi(optval);
			} else if (!strcasecmp(optname, "thread_model") && optval) {
				if (!strcasecmp(optval, "tree")) {
					/* Tree parallelization - all threads
					 * grind on the same tree. */
                    /*树并行化-所有线程在同一棵树上研磨。*/
					u->thread_model = TM_TREE;
					u->virtual_loss = 0;
				} else if (!strcasecmp(optval, "treevl")) {
					/* Tree parallelization, but also
					 * with virtual losses - this discou-
					 * rages most threads choosing the
					 * same tree branches to read. */
                    /*树并行化，但也有虚拟的损失-这使大多数线程无法选择要读取的同一个树分支。*/
					u->thread_model = TM_TREEVL;
				} else
					die("UCT: Invalid thread model %s\n", optval);
			} else if (!strcasecmp(optname, "virtual_loss") && optval) {
				/* Number of virtual losses added before evaluating a node. */
                /*在评估节点之前添加的虚拟损失数。*/
				u->virtual_loss = atoi(optval);
			} else if (!strcasecmp(optname, "pondering")) {
				/* Keep searching even during opponent's turn. */
                /*即使在对手转弯时也要继续搜索。*/
				u->pondering_opt = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "max_tree_size") && optval) {
				/* Maximum amount of memory [MiB] consumed by the move tree.
				 * For fast_alloc it includes the temp tree used for pruning.
				 * Default is 3072 (3 GiB). */
                /*移动树消耗的最大内存量[MIB]。对于快速分配，它包括用于修剪的临时树。默认值为3072（3 GiB）*/
				u->max_tree_size = (size_t)atoll(optval) * 1048576;  /* long is 4 bytes on windows! */
			} else if (!strcasecmp(optname, "fast_alloc")) {
				u->fast_alloc = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "pruning_threshold") && optval) {
				/* Force pruning at beginning of a move if the tree consumes
				 * more than this [MiB]. Default is 10% of max_tree_size.
				 * Increase to reduce pruning time overhead if memory is plentiful.
				 * This option is meaningful only for fast_alloc. */
                /*如果树的消耗量超过此[mib]，则在移动开始时强制修剪。默认值是max_tree_大小的10%。如果内存充足，请增加以减少修剪时间开销。此选项仅对快速分配有意义*/
				u->pruning_threshold = atol(optval) * 1048576;

			/** Time control */

			} else if (!strcasecmp(optname, "best2_ratio") && optval) {
				/* If set, prolong simulating while
				 * first_best/second_best playouts ratio
				 * is less than best2_ratio. */
                /*如果设置，则在最佳的情况下延长模拟时间，最佳子值delta大于最佳比值。*/
				u->best2_ratio = atof(optval);
			} else if (!strcasecmp(optname, "bestr_ratio") && optval) {
				/* If set, prolong simulating while
				 * best,best_best_child values delta
				 * is more than bestr_ratio. */
                /*如果设置，则在最佳的情况下延长模拟时间，最佳子值delta大于最佳比值。*/
				u->bestr_ratio = atof(optval);
			} else if (!strcasecmp(optname, "max_maintime_ratio") && optval) {
				/* If set and while not in byoyomi, prolong simulating no more than
				 * max_maintime_ratio times the normal desired thinking time. */
                /*如果设置了且不在Byoyomi中，则延长模拟时间不超过最大维护时间与正常期望思考时间的比值。*/
				u->max_maintime_ratio = atof(optval);
			} else if (!strcasecmp(optname, "fuseki_end") && optval) {
				/* At the very beginning it's not worth thinking
				 * too long because the playout evaluations are
				 * very noisy. So gradually increase the thinking
				 * time up to maximum when fuseki_end percent
				 * of the board has been played.
				 * This only applies if we are not in byoyomi. */
                /*一开始不值得考虑太长时间，因为决赛评估非常嘈杂。因此，逐渐增加思考时间，直到最长的时候，fuseki ou结束百分比的董事会已经发挥。这只适用于我们不在byoyomi。*/
				u->fuseki_end = atoi(optval);
			} else if (!strcasecmp(optname, "yose_start") && optval) {
				/* When yose_start percent of the board has been
				 * played, or if we are in byoyomi, stop spending
				 * more time and spread the remaining time
				 * uniformly.
				 * Between fuseki_end and yose_start, we spend
				 * a constant proportion of the remaining time
				 * on each move. (yose_start should actually
				 * be much earlier than when real yose start,
				 * but "yose" is a good short name to convey
				 * the idea.) */
                /*当游戏开始时，或者如果我们在Byoyomi，停止花费更多的时间，并将剩余的时间均匀分布。在Fuseki ou end和Yose ou start之间，我们将剩余时间的恒定比例花在每个动作上。（yose-start实际上应该比真正的yose-start早得多，但是“yose”是一个很好的简短名称来表达这个想法。）*/
				u->yose_start = atoi(optval);

			/** Dynamic komi */
            /*komi*/
			} else if (!strcasecmp(optname, "dynkomi") && optval) {
				/* Dynamic komi approach; there are multiple
				 * ways to adjust komi dynamically throughout
				 * play. We currently support two: */
                /*动态Komi方法；有多种方法可以在整个比赛中动态调整Komi。我们目前支持两种：*/
				char *dynkomiarg = strchr(optval, ':');
				if (dynkomiarg)
					*dynkomiarg++ = 0;
				if (!strcasecmp(optval, "none")) {
					u->dynkomi = uct_dynkomi_init_none(u, dynkomiarg, b);
				} else if (!strcasecmp(optval, "linear")) {
					/* You should set dynkomi_mask=1 or a very low
					 * handicap_value for white. */
                    /*您应该将Dynkomi_Mask设置为1或非常低残疾人价值为白色。*/
					u->dynkomi = uct_dynkomi_init_linear(u, dynkomiarg, b);
				} else if (!strcasecmp(optval, "adaptive")) {
					/* There are many more knobs to
					 * crank - see uct/dynkomi.c. */
                    /*还有很多旋钮需要转动-参见UCT/Dynkomi.c.*/
					u->dynkomi = uct_dynkomi_init_adaptive(u, dynkomiarg, b);
				} else
					die("UCT: Invalid dynkomi mode %s\n", optval);
			} else if (!strcasecmp(optname, "dynkomi_mask") && optval) {
				/* Bitmask of colors the player must be
				 * for dynkomi be applied; the default dynkomi_mask=3 allows
				 * dynkomi even in games where Pachi is white. */
                /*颜色的位掩码应用dynkomi之前玩家必须使用；默认dynkomi_mask=3允许dynkomi，即使在pachi为白色的游戏中也是如此。*/
				u->dynkomi_mask = atoi(optval);
			} else if (!strcasecmp(optname, "dynkomi_interval") && optval) {
				/* If non-zero, re-adjust dynamic komi
				 * throughout a single genmove reading,
				 * roughly every N simulations. */
				/* XXX: Does not work with tree
				 * parallelization. */
                /*If non-zero, re-adjust dynamic komi
				 * throughout a single genmove reading,
				 * roughly every N simulations. */
				/* XXX: Does not work with tree
				 * parallelization. */
                /*如果非零，重新调整动态Komithroughout一个单一的genmove读数，大致每N次模拟。不适用于树并行化。*/
				u->dynkomi_interval = atoi(optval);
			} else if (!strcasecmp(optname, "extra_komi") && optval) {
				/* Initial dynamic komi settings. This
				 * is useful for the adaptive dynkomi
				 * policy as the value to start with
				 * (this is NOT kept fixed) in case
				 * there is not enough time in the search
				 * to adjust the value properly (e.g. the
				 * game was interrupted). */
                /*初始动态Komi设置。这对于自适应Dynkomi策略很有用，因为如果搜索中没有足够的时间来正确调整值（例如游戏被中断），则该策略将作为开始的值（该值不保持固定）。*/
				u->initial_extra_komi = atof(optval);

			/** Node value result scaling */
            /*节点值结果缩放*/
			} else if (!strcasecmp(optname, "val_scale") && optval) {
				/* How much of the game result value should be
				 * influenced by win size. Zero means it isn't. */
                /*游戏结果值的多少应该受赢的大小的影响。零意味着不是。*/
				u->val_scale = atof(optval);
			} else if (!strcasecmp(optname, "val_points") && optval) {
				/* Maximum size of win to be scaled into game
				 * result value. Zero means boardsize^2. */
                /*将要缩放到游戏结果值中的最大胜利大小。零表示板大小^2。*/
				u->val_points = atoi(optval) * 2; // result values are doubled
			} else if (!strcasecmp(optname, "val_extra")) {
				/* If false, the score coefficient will be simply
				 * added to the value, instead of scaling the result
				 * coefficient because of it. */
                /*如果为false，则分数系数将简单地添加到值中，而不是因为该值而缩放结果系数。*/
				u->val_extra = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "val_byavg")) {
				/* If true, the score included in the value will
				 * be relative to average score in the current
				 * search episode inst. of jigo. */
                /*如果为真，该值中包含的分数将与jigo当前搜索集inst.中的平均分数相对应。*/
				u->val_byavg = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "val_bytemp")) {
				/* If true, the value scaling coefficient
				 * is different based on value extremity
				 * (dist. from 0.5), linear between
				 * val_bytemp_min, val_scale. */
                /*如果为真，则值缩放系数根据值极限（从0.5开始的距离）而不同，在Val_Bytemp_Min、Val_Scale之间为线性。*/
				u->val_bytemp = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "val_bytemp_min") && optval) {
				/* Minimum val_scale in case of val_bytemp. */
				u->val_bytemp_min = atof(optval);

			/** Local trees */
			/* (Purely experimental. Does not work - yet!) */

			} else if (!strcasecmp(optname, "local_tree")) {
				/* Whether to bias exploration by local tree values. */
				u->local_tree = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "tenuki_d") && optval) {
				/* Tenuki distance at which to break the local tree. */
				u->tenuki_d = atoi(optval);
				if (u->tenuki_d > TREE_NODE_D_MAX + 1)
					die("uct: tenuki_d must not be larger than TREE_NODE_D_MAX+1 %d\n", TREE_NODE_D_MAX + 1);
			} else if (!strcasecmp(optname, "local_tree_aging") && optval) {
				/* How much to reduce local tree values between moves. */
				u->local_tree_aging = atof(optval);
			} else if (!strcasecmp(optname, "local_tree_depth_decay") && optval) {
				/* With value x>0, during the descent the node
				 * contributes 1/x^depth playouts in
				 * the local tree. I.e., with x>1, nodes more
				 * distant from local situation contribute more
				 * than nodes near the root. */
				u->local_tree_depth_decay = atof(optval);
			} else if (!strcasecmp(optname, "local_tree_allseq")) {
				/* If disabled, only complete sequences are stored
				 * in the local tree. If this is on, also
				 * subsequences starting at each move are stored. */
				u->local_tree_allseq = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "local_tree_neival")) {
				/* If disabled, local node value is not
				 * computed just based on terminal status
				 * of the coordinate, but also its neighbors. */
				u->local_tree_neival = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "local_tree_eval")) {
				/* How is the value inserted in the local tree
				 * determined. */
				if (!strcasecmp(optval, "root"))
					/* All moves within a tree branch are
					 * considered wrt. their merit
					 * reaching tachtical goal of making
					 * the first move in the branch
					 * survive. */
					u->local_tree_eval = LTE_ROOT;
				else if (!strcasecmp(optval, "each"))
					/* Each move is considered wrt.
					 * its own survival. */
					u->local_tree_eval = LTE_EACH;
				else if (!strcasecmp(optval, "total"))
					/* The tactical goal is the survival
					 * of all the moves of my color and
					 * non-survival of all the opponent
					 * moves. Local values (and their
					 * inverses) are averaged. */
					u->local_tree_eval = LTE_TOTAL;
				else
					die("uct: unknown local_tree_eval %s\n", optval);
			} else if (!strcasecmp(optname, "local_tree_rootchoose")) {
				/* If disabled, only moves within the local
				 * tree branch are considered; the values
				 * of the branch roots (i.e. root children)
				 * are ignored. This may make sense together
				 * with eval!=each, we consider only moves
				 * that influence the goal, not the "rating"
				 * of the goal itself. (The real solution
				 * will be probably using criticality to pick
				 * local tree branches.) */
				u->local_tree_rootchoose = !optval || atoi(optval);

			/** Other heuristics */
			} else if (!strcasecmp(optname, "patterns")) {
				/* Load pattern database. Various modules
				 * (priors, policies etc.) may make use
				 * of this database. They will request
				 * it automatically in that case, but you
				 * can use this option to tweak the pattern
				 * parameters. */
				patterns_init(&u->pat, optval, false, true);
				u->want_pat = pat_setup = true;
			} else if (!strcasecmp(optname, "significant_threshold") && optval) {
				/* Some heuristics (XXX: none in mainline) rely
				 * on the knowledge of the last "significant"
				 * node in the descent. Such a node is
				 * considered reasonably trustworthy to carry
				 * some meaningful information in the values
				 * of the node and its children. */
				u->significant_threshold = atoi(optval);

			/** Distributed engine slaves setup */

			} else if (!strcasecmp(optname, "slave")) {
				/* Act as slave for the distributed engine. */
				u->slave = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "slave_index") && optval) {
				/* Optional index if per-slave behavior is desired.
				 * Must be given as index/max */
				u->slave_index = atoi(optval);
				char *p = strchr(optval, '/');
				if (p) u->max_slaves = atoi(++p);
			} else if (!strcasecmp(optname, "shared_nodes") && optval) {
				/* Share at most shared_nodes between master and slave at each genmoves.
				 * Must use the same value in master and slaves. */
				u->shared_nodes = atoi(optval);
			} else if (!strcasecmp(optname, "shared_levels") && optval) {
				/* Share only nodes of level <= shared_levels. */
				u->shared_levels = atoi(optval);
			} else if (!strcasecmp(optname, "stats_hbits") && optval) {
				/* Set hash table size to 2^stats_hbits for the shared stats. */
				u->stats_hbits = atoi(optval);
			} else if (!strcasecmp(optname, "stats_delay") && optval) {
				/* How long to wait in slave for initial stats to build up before
				 * replying to the genmoves command (in ms) */
				u->stats_delay = 0.001 * atof(optval);

			/** Presets */

			} else if (!strcasecmp(optname, "maximize_score")) {
				/* A combination of settings that will make
				 * Pachi try to maximize his points (instead
				 * of playing slack yose) or minimize his loss
				 * (and proceed to counting even when losing). */
				/* Please note that this preset might be
				 * somewhat weaker than normal Pachi, and the
				 * score maximization is approximate; point size
				 * of win/loss still should not be used to judge
				 * strength of Pachi or the opponent. */
				/* See README for some further notes. */
				if (!optval || atoi(optval)) {
					/* Allow scoring a lost game. */
					u->allow_losing_pass = true;
					/* Make Pachi keep his calm when losing
					 * and/or maintain winning marging. */
					/* Do not play games that are losing
					 * by too much. */
					/* XXX: komi_ratchet_age=40000 is necessary
					 * with losing_komi_ratchet, but 40000
					 * is somewhat arbitrary value. */
					char dynkomi_args[] = "losing_komi_ratchet:komi_ratchet_age=60000:no_komi_at_game_end=0:max_losing_komi=30";
					u->dynkomi = uct_dynkomi_init_adaptive(u, dynkomi_args, b);
					/* XXX: Values arbitrary so far. */
					/* XXX: Also, is bytemp sensible when
					 * combined with dynamic komi?! */
					u->val_scale = 0.01;
					u->val_bytemp = true;
					u->val_bytemp_min = 0.001;
					u->val_byavg = true;
				}

			} else
				die("uct: Invalid engine argument %s or missing value\n", optname);
		}
	}

	if (!u->policy)
		u->policy = policy_ucb1amaf_init(u, NULL, b);

	if (!!u->random_policy_chance ^ !!u->random_policy)
		die("uct: Only one of random_policy and random_policy_chance is set\n");

	if (!u->local_tree) {
		/* No ltree aging. */
		u->local_tree_aging = 1.0f;
	}

	if (u->fast_alloc) {
		if (u->pruning_threshold < u->max_tree_size / 10)
			u->pruning_threshold = u->max_tree_size / 10;
		if (u->pruning_threshold > u->max_tree_size / 2)
			u->pruning_threshold = u->max_tree_size / 2;

		/* Limit pruning temp space to 20% of memory. Beyond this we discard
		 * the nodes and recompute them at the next move if necessary. */
		u->max_pruned_size = u->max_tree_size / 5;
		u->max_tree_size -= u->max_pruned_size;
	} else {
		/* Reserve 5% memory in case the background free() are slower
		 * than the concurrent allocations. */
		u->max_tree_size -= u->max_tree_size / 20;
	}

	if (!u->prior)
		u->prior = uct_prior_init(NULL, b, u);

	if (!u->playout)
		u->playout = playout_moggy_init(NULL, b, u->jdict);
	if (!u->playout->debug_level)
		u->playout->debug_level = u->debug_level;

	if (u->want_pat && !pat_setup)
		patterns_init(&u->pat, NULL, false, true);
	dcnn_init();
	log_nthreads(u);

	if (u->slave) {
		if (!u->stats_hbits) u->stats_hbits = DEFAULT_STATS_HBITS;
		if (!u->shared_nodes) u->shared_nodes = DEFAULT_SHARED_NODES;
		assert(u->shared_levels * board_bits2(b) <= 8 * (int)sizeof(path_t));
	}

	if (!u->dynkomi)
		u->dynkomi = board_small(b) ? uct_dynkomi_init_none(u, NULL, b)
			: uct_dynkomi_init_linear(u, NULL, b);

	if (u->pondering_opt && using_dcnn(b)) {
		warning("Can't use pondering with dcnn right now, pondering turned off.\n");
		u->pondering_opt = false;
	}

	/* Some things remain uninitialized for now - the opening tbook
	 * is not loaded and the tree not set up. */
	/* This will be initialized in setup_state() at the first move
	 * received/requested. This is because right now we are not aware
	 * about any komi or handicap setup and such. */

	return u;
}

//初始化uct引擎，
struct engine *
engine_uct_init(char *arg, struct board *b)
{
    //初始化引擎状态，
	struct uct *u = uct_state_init(arg, b);
    //初始化引擎
	struct engine *e = calloc2(1, sizeof(struct engine));
	e->name = "UCT";
	e->board_print = uct_board_print;
	e->notify_play = uct_notify_play;
	e->chat = uct_chat;
	e->undo = uct_undo;
	e->result = uct_result;
	e->genmove = uct_genmove;
	e->genmoves = uct_genmoves;
	e->best_moves = uct_best_moves;
	e->evaluate = uct_evaluate;
	e->dead_group_list = uct_dead_group_list;
	e->stop = uct_stop;
	e->done = uct_done;
	e->ownermap = uct_ownermap;
	e->livegfx_hook = uct_livegfx_hook;
	e->data = u;//将cut状态赋值给data，他是动态申请的地址
	if (u->slave)
		e->notify = uct_notify;

	const char banner[] = "If you believe you have won but I am still playing, "
		"please help me understand by capturing all dead stones. "
		"Anyone can send me 'winrate' in private chat to get my assessment of the position.";
	if (!u->banner) u->banner = "";
	e->comment = malloc2(sizeof(banner) + strlen(u->banner) + 1);
	sprintf(e->comment, "%s %s", banner, u->banner);

	return e;
}
