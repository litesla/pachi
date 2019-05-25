#include <stdio.h>

#include "random.h"


/* Simple Park-Miller for floating point; LCG as used in glibc and other places */
/*用于浮点的简单Park Miller；用于glibc和其他地方的lcg*/


/********************************************************************************************/
#ifdef _WIN32

/* Use TlsGetValue() / TlsSetValue() for thread-local storage,
 * mingw-w64's __thread is painfully slow. */

static int tls_index = -1;
//在主函数之前运行
static void __attribute__((constructor))
init_fast_random()
{
	tls_index = TlsAlloc();
	fast_srandom(29264);
}

void
fast_srandom(unsigned long seed_)
{
	TlsSetValue(tls_index, (void*)(intptr_t)seed_);
}

unsigned long
fast_getseed(void)
{
	return (unsigned long)(intptr_t)TlsGetValue(tls_index);
}

uint16_t
fast_random(unsigned int max)
{
	unsigned long pmseed = fast_getseed();
	pmseed = ((pmseed * 1103515245) + 12345) & 0x7fffffff;
	fast_srandom(pmseed);
	return ((pmseed & 0xffff) * max) >> 16;
}


#else


/********************************************************************************************/
#ifndef NO_THREAD_LOCAL
//线程中的随机变量，为了完成线程的独立性
//随机的初始值 在计算过程中是多线程的，为了还原现场
//每个线程有一个独立的随机种子
static __thread unsigned long pmseed = 29264;

void
fast_srandom(unsigned long seed_)
{
	pmseed = seed_;
}

unsigned long
fast_getseed(void)
{
	return pmseed;
}
//他要生成一个不大于max的随机数 列成一个公式(x×max)/(2^16) < max 
uint16_t
fast_random(unsigned int max)
{
    //寻找的一个尽可能长的循环节，每个状态只出现一次，在这个序列环中不会出现分叉
    //运用加法的情况是乘法无法跳出0状态
	pmseed = ((pmseed * 1103515245) + 12345) & 0x7fffffff;
	return ((pmseed & 0xffff) * max) >> 16;
}

float
fast_frandom(void)
{
	/* Construct (1,2) IEEE floating_t from our random integer */
	/* http://rgba.org/articles/sfrand/sfrand.htm */
	union { unsigned long ul; floating_t f; } p;
	p.ul = (((pmseed *= 16807) & 0x007fffff) - 1) | 0x3f800000;
	return p.f - 1.0f;
}

#else


/********************************************************************************************/

/* Thread local storage not supported through __thread,
 * use pthread_getspecific() instead. */

#include <pthread.h>

static pthread_key_t seed_key;

static void __attribute__((constructor))
random_init(void)
{
	pthread_key_create(&seed_key, NULL);
	fast_srandom(29264UL);
}

void
fast_srandom(unsigned long seed_)
{
	pthread_setspecific(seed_key, (void *)seed_);
}

unsigned long
fast_getseed(void)
{
	return (unsigned long)pthread_getspecific(seed_key);
}

uint16_t
fast_random(unsigned int max)
{
	unsigned long pmseed = (unsigned long)pthread_getspecific(seed_key);
	pmseed = ((pmseed * 1103515245) + 12345) & 0x7fffffff;
	pthread_setspecific(seed_key, (void *)pmseed);
	return ((pmseed & 0xffff) * max) >> 16;
}

float
fast_frandom(void)
{
	/* Construct (1,2) IEEE floating_t from our random integer */
	/* http://rgba.org/articles/sfrand/sfrand.htm */
	unsigned long pmseed = (unsigned long)pthread_getspecific(seed_key);
	pmseed *= 16807;
	union { unsigned long ul; floating_t f; } p;
	p.ul = ((pmseed & 0x007fffff) - 1) | 0x3f800000;
	pthread_setspecific(seed_key, (void *)pmseed);
	return p.f - 1.0f;
}

#endif
#endif
