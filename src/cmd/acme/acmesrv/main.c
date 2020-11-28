#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include "dat.h"
#include "fns.h"

enum
{
	STACK = 65536,
};

QLock	writelk;

void		muxin(void*);
void		muxout(void*);
int		post(char *srv);
void		fatal(char *fmt, ...);
void		makedir(char*);

int muxfds[4];

void
usage()
{
	fprint(2, "usage: acmesrv [-D] [-d] [-f] [-p exportfs] [-p cmdfs] [-n namespace]\n");
	threadexitsall("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *postname, *ns;
	Srv *exportfs, *cmdfs, *postfs;
	int p[2], i, foreground;

	debug = FALSE;
	postname = nil;
	postfs = nil;
	foreground = FALSE;
	ns = nil;

	fmtinstall('D', dirfmt);
	fmtinstall('M', dirmodefmt);

	ARGBEGIN{
	default:
		usage();
	case 'D':
		chatty9p++;
		break;
	case 'd':
		debug = TRUE;
		break;
	case 'p':
		postname = EARGF(usage());
		break;
	case 'f':
		foreground = TRUE;
		break;
	case 'n':
		ns = EARGF(usage());
		break;
	}ARGEND

	if(argc != 0)
		usage();

	if(ns != nil){
		p9putenv("NAMESPACE", ns);
		makedir(ns);
	}

	exportfs = exportfsinit("/", getuser());
	cmdfs = cmdfsinit(getuser());

	if(postname != nil){
		if(strcmp(postname, "exportfs") == 0)
			postfs = exportfs;
		else if(strcmp(postname, "cmdfs") == 0)
			postfs = cmdfs;
		else
			usage();

		postfs->foreground = foreground;
		threadpostmountsrv(postfs, postname, nil, MREPL|MCREATE);
		if(postfs->foreground)
			threadexitsall(nil);
		else
			threadexits(nil);
	}

	if(pipe(p) < 0)
		threadexitsall("pipe");
	muxfds[0] = p[0];
	exportfs->nopipe = TRUE;
	exportfs->infd = p[1];
	exportfs->outfd = p[1];
	threadpostmountsrv(exportfs, nil, nil, MREPL|MCREATE);

	if(pipe(p) < 0)
		threadexitsall("pipe");
	muxfds[1] = p[0];
	cmdfs->nopipe = TRUE;
	cmdfs->infd = p[1];
	cmdfs->outfd = p[1];
	threadpostmountsrv(cmdfs, nil, nil, MREPL|MCREATE);

	muxfds[2] = post("plumb");
	muxfds[3] = post("acme");

	/* ready to go */
	write(1, "OK", 2);

	for(i=0; i<nelem(muxfds); i++)
		proccreate(muxout, (void*)(uintptr)i, STACK);

	muxin(nil);

	threadexitsall("EOF");
}

void
muxin(void *v)
{
	USED(v);
	char buf[8192], dest;
	int n;

	for(;;){
		if(readn(0, &dest, 1) != 1)
			break;
		if(dest >= nelem(muxfds))
			threadexitsall("invalid mux");
		if((n = read9pmsg(0, buf, sizeof(buf))) < 0)
			threadexitsall("invalid 9p message");
		if(write(muxfds[(int)dest], buf, n) != n)
			break;
	}

	free(buf);
}

void
muxout(void *v)
{
	/* args: */
		int dest;
	/* end of args */
	int fd;
	char buf[8192+1];
	int n, ret;

	dest = (uintptr)v;
	fd = muxfds[dest];

	for(;;){
		if((n = read9pmsg(fd, buf+1, sizeof(buf)-1)) < 0)
			threadexitsall("invalid 9p message");
		buf[0] = dest;
		qlock(&writelk);
		ret = write(1, buf, n+1);
		qunlock(&writelk);
		if(ret != n+1)
			break;
	}

	free(buf);
}

int
post(char *srv)
{
	int p[2];

	if(pipe(p) < 0)
		fatal("can't create pipe: %r");

	if(post9pservice(p[1], srv, nil) < 0)
		fatal("post9pservice %s: %r", srv);
	close(p[1]);

	return p[0];
}

void*
erealloc(void *p, uint n)
{
	p = realloc(p, n);
	if(p == nil)
		sysfatal("realloc failed");
	setmalloctag(p, getcallerpc(&n));
	return p;
}

void
fatal(char *fmt, ...)
{
	char buf[256];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf+sizeof buf, fmt, arg);
	va_end(arg);

	fprint(2, "%s: %s\n", argv0 ? argv0 : "<prog>", buf);
	threadexitsall("fatal");
}

void
makedir(char *s)
{
	int fd;

	if(access(s, AEXIST) == 0)
		return;
	fd = create(s, OREAD, DMDIR | 0777L);
	if(fd >= 0)
		close(fd);
}