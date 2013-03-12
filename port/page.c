#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

#define pghash(daddr)	palloc.hash[(daddr>>PGSHFT)&(PGHSIZE-1)]

struct	Palloc palloc;

static	uint	highwater;	/* TO DO */

/*
 * Split palloc.mem[i] if it's not all of the same color and we can.
 * Return the new end of the known banks.
 */
static int
splitbank(int i, int e)
{
	Pallocmem *pm;
	uintmem psz;
	uintmem npg;

	pm = &palloc.mem[i];
	pm->color  = memcolor(pm->base, &psz);
	if(pm->color < 0){
		pm->color = 0;
		if(i > 0)
			pm->color = pm[-1].color;
		return 0;
	}
	npg = psz/PGSZ;

	if(e == nelem(palloc.mem) || npg <= 1 || npg >= pm->npage)
		return 0;
	if(i+1 < e)
		memmove(pm+2, pm+1, (e-i-1)*sizeof(Pallocmem));
	pm[1].base = pm->base + npg*PGSZ;
	pm[1].npage = pm->npage - npg;
	DBG("palloc split[%d] col %d %#P %#ulld -> %ulld\n",
		i, pm->color, pm->base, pm->npage, npg);
	pm->npage = npg;
	return 1;
}

void
pageinit(void)
{
	int e, i, j;
	Page *p;
	Pallocmem *pm;
	uintmem np, pkb, kkb, kmkb, mkb;

	for(e = 0; e < nelem(palloc.mem); e++){
		if(palloc.mem[e].npage == 0)
			break;
	}

	/*
	 * Compute # of pages and split banks if not of the same color
	 * and there's room for doing so.
	 */
	np = 0;
	for(i=0; i<e; i++){
		pm = &palloc.mem[i];
		if(splitbank(i, e))
			e++;

		print("palloc[%d] col %d %#P %llud\n",
			i, pm->color, pm->base, pm->npage);

/* BUG; can't handle it all right now */
if(pm->base > 600*MiB){
	pm->npage = 0;
	continue;
}
if(pm->base+pm->npage*PGSZ > 600*MiB)
	pm->npage = pm->npage = (600*MiB-pm->base)/PGSZ;

		np += pm->npage;
	}

	palloc.pages = malloc(np*sizeof(Page));
	if(palloc.pages == nil)
		panic("pageinit");

	palloc.head = palloc.pages;
	p = palloc.head;
	for(i=0; i<e; i++){
		pm = &palloc.mem[i];
		for(j=0; j<pm->npage; j++){
			p->prev = p-1;
			p->next = p+1;
			p->pa = pm->base+j*PGSZ;
			p->color = pm->color;
			palloc.freecount++;
			p++;
		}
	}
	palloc.tail = p - 1;
	palloc.head->prev = 0;
	palloc.tail->next = 0;

	palloc.user = p - palloc.pages;

	/* user, kernel, kernel malloc area, memory */
	pkb = palloc.user*PGSZ/KiB;
	kkb = ROUNDUP((uintptr)end - KTZERO, PGSZ)/KiB;
	kmkb = ROUNDUP(sys->vmend - (uintptr)end, PGSZ)/KiB;
	mkb = sys->pmoccupied/KiB;

	/* Paging numbers */
	highwater = (palloc.user*5)/100;
	if(highwater >= 64*MB/PGSZ)
		highwater = 64*MB/PGSZ;

	print("%lldM memory: %lldK+%lldM kernel, %lldM user, %lldM lost\n",
		mkb/KiB, kkb, kmkb/KiB, pkb/KiB, (mkb-kkb-kmkb-pkb)/KiB);
}

static void
pageunchain(Page *p)
{
	if(canlock(&palloc))
		panic("pageunchain (palloc %#p)", &palloc);
	if(p->prev)
		p->prev->next = p->next;
	else
		palloc.head = p->next;
	if(p->next)
		p->next->prev = p->prev;
	else
		palloc.tail = p->prev;
	p->prev = p->next = nil;
	palloc.freecount--;
}

void
pagechaintail(Page *p)
{
	if(canlock(&palloc))
		panic("pagechaintail");
	if(palloc.tail) {
		p->prev = palloc.tail;
		palloc.tail->next = p;
	}
	else {
		palloc.head = p;
		p->prev = 0;
	}
	palloc.tail = p;
	p->next = 0;
	palloc.freecount++;
}

void
pagechainhead(Page *p)
{
	if(canlock(&palloc))
		panic("pagechainhead");
	if(palloc.head) {
		p->next = palloc.head;
		palloc.head->prev = p;
	}
	else {
		palloc.tail = p;
		p->next = 0;
	}
	palloc.head = p;
	p->prev = 0;
	palloc.freecount++;
}

Page*
newpage(int clear, Segment **s, uintptr va)
{
	Page *p;
	KMap *k;
	uchar ct;
	int i, hw, dontalloc, color;

if(up == nil)
   print("newpage called from %#p\n", getcallerpc(&clear));
	lock(&palloc);
	color = getpgcolor(va);
	hw = highwater;
	for(;;) {
		if(palloc.freecount > hw)
			break;
		if(up->kp && palloc.freecount > 0)
			break;

		unlock(&palloc);
		dontalloc = 0;
		if(s && *s) {
			qunlock(&((*s)->lk));
			*s = 0;
			dontalloc = 1;
		}
		qlock(&palloc.pwait);	/* Hold memory requesters here */

		while(waserror())	/* Ignore interrupts */
			;

		print("out of physical memory\n");
		pagereclaim(highwater/2);

		tsleep(&palloc.r, ispages, 0, 1000);

		poperror();

		qunlock(&palloc.pwait);

		/*
		 * If called from fault and we lost the segment from
		 * underneath don't waste time allocating and freeing
		 * a page. Fault will call newpage again when it has
		 * reacquired the segment locks
		 */
		if(dontalloc)
			return 0;

		lock(&palloc);
	}

	/* First try for our colour */
	for(p = palloc.head; p; p = p->next)
		if(p->color == color)
			break;

	ct = PG_NOFLUSH;
	if(p == 0) {
		p = palloc.head;
		p->color = color;
		ct = PG_NEWCOL;
	}

	pageunchain(p);

	lock(p);
	if(p->ref != 0)
		panic("newpage");

	uncachepage(p);
	p->ref++;
	p->va = va;
	p->modref = 0;
	for(i = 0; i < MACHMAX; i++)
		p->cachectl[i] = ct;
	unlock(p);
	unlock(&palloc);

	if(clear) {
		k = kmap(p);
		memset((void*)VA(k), 0, PGSZ);
		kunmap(k);
	}

	return p;
}

int
ispages(void*)
{
	return palloc.freecount >= highwater;
}

void
putpage(Page *p)
{
	lock(&palloc);
	lock(p);

	if(p->ref == 0)
		panic("putpage");

	if(--p->ref > 0) {
		unlock(p);
		unlock(&palloc);
		return;
	}

	if(p->image != nil)
		pagechaintail(p);
	else
		pagechainhead(p);

	if(palloc.r.p != 0)
		wakeup(&palloc.r);

	unlock(p);
	unlock(&palloc);
}

Page*
auxpage(void)
{
	Page *p;

	lock(&palloc);
	p = palloc.head;
	if(palloc.freecount <= highwater) {
		/* memory's tight, don't use it for file cache */
		unlock(&palloc);
		return 0;
	}
	pageunchain(p);

	lock(p);
	if(p->ref != 0)
		panic("auxpage");
	p->ref++;
	uncachepage(p);
	unlock(p);
	unlock(&palloc);

	return p;
}

static int dupretries = 15000;

int
duppage(Page *p)				/* Always call with p locked */
{
	Page *np;
	int color;
	int retries;

	retries = 0;
retry:

	if(retries++ > dupretries){
		print("duppage %d, up %#p\n", retries, up);
		dupretries += 100;
		if(dupretries > 100000)
			panic("duppage\n");
		uncachepage(p);
		return 1;
	}


	/* don't dup pages with no image */
	if(p->ref == 0 || p->image == nil || p->image->notext)
		return 0;

	/*
	 *  normal lock ordering is to call
	 *  lock(&palloc) before lock(p).
	 *  To avoid deadlock, we have to drop
	 *  our locks and try again.
	 */
	if(!canlock(&palloc)){
		unlock(p);
		if(up)
			sched();
		lock(p);
		goto retry;
	}

	/* No freelist cache when memory is relatively low */
	if(palloc.freecount < highwater) {
		unlock(&palloc);
		uncachepage(p);
		return 1;
	}

	color = getpgcolor(p->va);
	for(np = palloc.head; np; np = np->next)
		if(np->color == color)
			break;

	/* No page of the correct color */
	if(np == 0) {
		unlock(&palloc);
		uncachepage(p);
		return 1;
	}

	pageunchain(np);
	pagechaintail(np);

/*
* XXX - here's a bug? - np is on the freelist but it's not really free.
* when we unlock palloc someone else can come in, decide to
* use np, and then try to lock it.  they succeed after we've
* run copypage and cachepage and unlock(np).  then what?
* they call pageunchain before locking(np), so it's removed
* from the freelist, but still in the cache because of
* cachepage below.  if someone else looks in the cache
* before they remove it, the page will have a nonzero ref
* once they finally lock(np).
*/
	lock(np);
	unlock(&palloc);

	/* Cache the new version */
	uncachepage(np);
	np->va = p->va;
	np->daddr = p->daddr;
	copypage(p, np);
	cachepage(np, p->image);
	unlock(np);
	uncachepage(p);

	return 0;
}

void
copypage(Page *f, Page *t)
{
	KMap *ks, *kd;

	ks = kmap(f);
	kd = kmap(t);
	memmove((void*)VA(kd), (void*)VA(ks), PGSZ);
	kunmap(ks);
	kunmap(kd);
}

void
uncachepage(Page *p)			/* Always called with a locked page */
{
	Page **l, *f;

	if(p->image == 0)
		return;

	lock(&palloc.hashlock);
	l = &pghash(p->daddr);
	for(f = *l; f; f = f->hash) {
		if(f == p) {
			*l = p->hash;
			break;
		}
		l = &f->hash;
	}
	unlock(&palloc.hashlock);
	putimage(p->image);
	p->image = 0;
	p->daddr = 0;
}

void
cachepage(Page *p, Image *i)
{
	Page **l;

	/* If this ever happens it should be fixed by calling
	 * uncachepage instead of panic. I think there is a race
	 * with pio in which this can happen. Calling uncachepage is
	 * correct - I just wanted to see if we got here.
	 */
	if(p->image)
		panic("cachepage");

	incref(i);
	lock(&palloc.hashlock);
	p->image = i;
	l = &pghash(p->daddr);
	p->hash = *l;
	*l = p;
	unlock(&palloc.hashlock);
}

void
cachedel(Image *i, ulong daddr)
{
	Page *f, **l;

	lock(&palloc.hashlock);
	l = &pghash(daddr);
	for(f = *l; f; f = f->hash) {
		if(f->image == i && f->daddr == daddr) {
			lock(f);
			if(f->image == i && f->daddr == daddr){
				*l = f->hash;
				putimage(f->image);
				f->image = 0;
				f->daddr = 0;
			}
			unlock(f);
			break;
		}
		l = &f->hash;
	}
	unlock(&palloc.hashlock);
}

Page *
lookpage(Image *i, ulong daddr)
{
	Page *f;

	lock(&palloc.hashlock);
	for(f = pghash(daddr); f; f = f->hash) {
		if(f->image == i && f->daddr == daddr) {
			unlock(&palloc.hashlock);

			lock(&palloc);
			lock(f);
			if(f->image != i || f->daddr != daddr) {
				unlock(f);
				unlock(&palloc);
				return 0;
			}
			if(++f->ref == 1)
				pageunchain(f);
			unlock(&palloc);
			unlock(f);

			return f;
		}
	}
	unlock(&palloc.hashlock);

	return 0;
}

uvlong
pagereclaim(int npages)
{
	Page *p;
	uvlong ticks;

	lock(&palloc);
	ticks = fastticks(nil);

	/*
	 * All the pages with images backing them are at the
	 * end of the list (see putpage) so start there and work
	 * backward.
	 */
	for(p = palloc.tail; p && p->image && npages > 0; p = p->prev) {
		if(p->ref == 0 && canlock(p)) {
			if(p->ref == 0) {
				npages--;
				uncachepage(p);
			}
			unlock(p);
		}
	}
	ticks = fastticks(nil) - ticks;
	unlock(&palloc);

	return ticks;
}

Pte*
ptecpy(Pte *old)
{
	Pte *new;
	Page **src, **dst, *pg;

	new = ptealloc();
	dst = &new->pages[old->first-old->pages];
	new->first = dst;
	for(src = old->first; src <= old->last; src++, dst++){
		if((pg = *src) != nil){
			lock(pg);
			pg->ref++;
			unlock(pg);
			new->last = dst;
			*dst = pg;
		}
	}

	return new;
}

Pte*
ptealloc(void)
{
	Pte *new;

	new = smalloc(sizeof(Pte));
	new->first = &new->pages[PTEPERTAB];
	new->last = new->pages;
	return new;
}

void
freepte(Segment *s, Pte *p)
{
	int ref;
	void (*fn)(Page*);
	Page *pt, **pg, **ptop;

	switch(s->type&SG_TYPE) {
	case SG_PHYSICAL:
		fn = s->pseg->pgfree;
		ptop = &p->pages[PTEPERTAB];
		if(fn) {
			for(pg = p->pages; pg < ptop; pg++) {
				if(*pg == 0)
					continue;
				(*fn)(*pg);
				*pg = 0;
			}
			break;
		}
		for(pg = p->pages; pg < ptop; pg++) {
			pt = *pg;
			if(pt == 0)
				continue;
			lock(pt);
			ref = --pt->ref;
			unlock(pt);
			if(ref == 0)
				free(pt);
		}
		break;
	default:
		for(pg = p->first; pg <= p->last; pg++)
			if(*pg) {
				putpage(*pg);
				*pg = 0;
			}
	}
	free(p);
}
