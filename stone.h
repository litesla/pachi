#ifndef PACHI_STONE_H
#define PACHI_STONE_H

//棋子，没有子，黑子，白字，
//为了计算方便，当我们要求棋盘大小是19*19的情况下，他会加一层小边
//就是21*21的，就是旗子边的位置
enum stone {
	S_NONE,
	S_BLACK,
	S_WHITE,
	S_OFFBOARD,
	S_MAX,
};

static char stone2char(enum stone s);
static enum stone char2stone(char s);
char *stone2str(enum stone s); /* static string */
enum stone str2stone(char *str);

static enum stone stone_other(enum stone s);


static inline char
stone2char(enum stone s)
{
	return ".XO#"[s];
}

static inline enum stone
char2stone(char s)
{
	switch (s) {
		case '.': return S_NONE;
		case 'X': return S_BLACK;
		case 'O': return S_WHITE;
		case '#': return S_OFFBOARD;
	}
	return S_NONE; // XXX
}

/* Curiously, gcc is reluctant to inline this; I have cofirmed
 * there is performance benefit. */
static inline enum stone __attribute__((always_inline))
stone_other(enum stone s)
{
	static const enum stone o[S_MAX] = { S_NONE, S_WHITE, S_BLACK, S_OFFBOARD };
	return o[s];
}

#endif
