#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

	Image 	swapimage;

void
swapinit(void)
{
	swapimage.notext = 1;
}

void
putswap(Page*)
{
	panic("putswap");
}

void
dupswap(Page*)
{
	panic("dupswap");
}

int
swapcount(ulong daddr)
{
	USED(daddr);
	return 0;
}

void
kickpager(void)
{
	print("out of physical memory\n");
}

void
pagersummary(void)
{
	print("no swap\n");
}

void
setswapchan(Chan *c)
{
	cclose(c);
}
