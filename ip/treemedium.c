#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#include "ip.h"
#include "io.h"

enum {
	IOClass		= 0,			/* All comms to/from IO */
	CPUClass		= 1,			/* cpu-cpu comms */
	PayloadSize	= 256,		/* granularity of FIFO */
	TreeDot		= 11,			/* class A network for tree */
	TreeIP		= 128,		/* tree has this in second octect */
	Hsize		= 4,			/* size of lower-level tree header */
};

/*
 *  notes from the noob:
 * on tree, this looks like an enet+ip header. The change for tree is we will compute the
 * class from the IP and put it in the first byte. The tree code can grab that and create the header.
 *
 * TO DO: this isn't really enough for production use. the tree driver must somehow
 * add delimiters to allow larger messages to flow through in 256 byte chunks.
 */
typedef struct Th Th;
struct Th {
	uchar	hdr[Hsize];	/* tree header */
	uchar	ipv4hdr[12];	/* initial part of IPv4 header */
	uchar	ipv4src[4];	/* IP source */
	uchar	ipv4dst[4];	/* IP destination */
};

typedef struct Rock Rock;
struct Rock {
	Fs*	fs;				/* file system we belong to */
	Proc*	readp;			/* reading process */
	Chan*	mchan;		/* data channel */
};

/*
 * Called by ipoput with a single block to write.
 */
static void
treebwrite(Ipifc* ifc, Block* b, int, uchar*)
{
	Th *th;
	Rock *r;
	u32int hdr, p2pdst;
	int class;

	r = ifc->arg;

	if(b->next)
		b = concatblock(b);
	b = padblock(b, ifc->medium->hsize);

	th = (Th*)b->rp;

	/* if we are going CPU to CPU, then we need to go class CPUClass. */
	class = IOClass;
	if ((th->ipv4dst[1] != 0x80) && (th->ipv4src[1] != 0x80))
		class = CPUClass;

	/*
	 * what has to happen here:
	 * If lowest octet of dest is 0xff, nothing else matters: make a TAG
	 * packet as it is some sort of non-P2P packet.
	 * Otherwise, it is a P2P, so formulate the P2P header.
	 * We should never get here unless d[0] and s[0] are 11.
	 * This step requires that we know our own IP.
	 * If each node has the correct address in its p2paddr,
	 * then it will be more efficient to use MKP2P with the target address,
	* for unicast addresses, and MKTAG to do a broadcast for broadcast IPs.
	 */
	if (th->ipv4dst[3] == 0xff) {
		hdr = MKTAG(class, 0, 0, PIH_NONE);
	} else {
		p2pdst = xyztop2p(th->ipv4dst[1], th->ipv4dst[2], th->ipv4dst[3]-1);
		hdr = MKP2P(class, p2pdst);
	}

	th->hdr[0] = hdr>>24;
	th->hdr[1] = hdr>>16;
	th->hdr[2] = hdr>>8;
	th->hdr[3] = hdr;
	r->mchan->dev->bwrite(r->mchan, b, 0);
	ifc->out++;

}

/*
 * Process to read from the device.
 */
static void
treeread(void* a)
{
	Block *b;
	Ipifc *ifc;
	Rock *r;
	char *argv[1];
	ifc = a;
	r = ifc->arg;
	r->readp = up;
	if(waserror()){
		r->readp = nil;
		pexit("hangup", 1);
	}
	for(;;){
		b = r->mchan->dev->bread(r->mchan, ifc->maxtu, 0);
		if(b == nil){
			/*
			 * get here if mchan is a pipe and other side hangs up
			 * clean up this interface & get out
			ZZZ is this a good idea?
			 */
			poperror();
			r->readp = nil;
			argv[0] = "unbind";
			if(!waserror())
				ifc->conv->p->ctl(ifc->conv, argv, 1);
			pexit("hangup", 1);
		}

		if(!canrlock(ifc)){
			freeb(b);
			continue;
		}
		if(waserror()){
			runlock(ifc);
			nexterror();
		}
		ifc->in++;
		b->rp += ifc->medium->hsize;
		if(ifc->lifc == nil)
			freeb(b);
		else
			ipiput4(r->fs, ifc, b);
		runlock(ifc);
		poperror();
	}
}

/*
 * Called to bind an Ipifc to a tree device.
 * Ifc is qlock'd.
 */
static void
treebind(Ipifc* ifc, int argc, char* argv[])
{
	Rock *r;
	Chan *mchan;

	if(argc < 2)
		error(Ebadarg);

	mchan = namec(argv[2], Aopen, ORDWR, 0);
	if(waserror()){
		cclose(mchan);
		nexterror();
	}
	/* TO DO? really need to make it non-blocking? */

	r = smalloc(sizeof(*r));
	r->mchan = mchan;
	r->fs = ifc->conv->p->f;
	ifc->arg = r;
	ifc->mbps = 1001;

	poperror();

	kproc("treeread", treeread, ifc);
}

/*
 * Called with ifc wlock'd.
 */
static void
treeunbind(Ipifc* ifc)
{
	Rock *r;

	r = ifc->arg;
	if(r->readp != nil)
		postnote(r->readp, 1, "unbind", NUser);

	/* wait for readers to die */
	while(r->readp != nil)
		tsleep(&up->sleep, return0, 0, 300);

	if(r->mchan != nil)
		cclose(r->mchan);

	free(r);
}

static Medium treemedium = {
	.name		= "tree",
	.hsize		= Hsize,
	.mintu		= PayloadSize+Hsize,	/* TO DO: better */
	.maxtu		= PayloadSize+Hsize,	/* TO DO: better */
	.maclen		= 0,
	.bind			= treebind,
	.unbind		= treeunbind,
	.bwrite		= treebwrite,
	.unbindonclose=	0,
};

void
treemediumlink(void)
{
	addipmedium(&treemedium);
}
