#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include "dat.h"
#include "fns.h"

/*

	BUG: There are too many roundtrips to execute simple commands.

*/

enum
{
	Qdir,
	Qnew,
	Qcmd,

	QCctl,
	QCstdin,
	QCstdout,
	QCstderr,
	QCwait,
};

enum
{
	Ctlerr,
	Ctlenv,
	Ctlcmd,
	Ctldir,

	Ctlstart,
	Ctleof,
	Ctlnote,
	Ctldel,
};

enum
{
	Ioread,
	Iowrite,
	Iowait,
};

typedef struct Cmd Cmd;
typedef struct Env Env;
typedef struct Dirtab Dirtab;
typedef struct Xfid Xfid;
typedef struct Io Io;

struct Dirtab
{
	char	*name;
	uchar	type;
	uint	qid;
	uint	perm;
};

struct Xfid
{
	Cmd *c;
	Dirtab dir;
};

struct Cmd
{
	int ref; /* protected by cmdlk */

	int id;
	int pid;

	char *body;
	char *dir;

	Env *env;

	/* stdio pipes: */
	int stdin[2];
	int stdout[2];
	int stderr[2];

	Channel *waitc;

	Cmd *next;
};

struct Env
{
	char *name;
	char *value;
	Env *next;
};

struct Io
{
	Req *r;
	Channel *waitc;
	int op;
	int fd;
};

static char *fsowner;
static Channel *cwait;

static QLock cmdlk;
static Cmd *cmdlist;
static int cmdnum;

static Dirtab dirtab[] = {
	{".",			QTDIR,	Qdir,		0500|DMDIR},
	{"new",		QTDIR,	Qnew,	0500|DMDIR},
};

/* write commands into ctl, e.g., "start" */
/* ctl shows status */

static Dirtab dirtabc[] = {
	{".",			QTDIR,		Qdir,			0500|DMDIR},
	{"ctl",		QTAPPEND,	QCctl,		0600},
	{"stdin",		QTAPPEND,	QCstdin,		0200|DMAPPEND},
	{"stdout",		QTFILE,		QCstdout,		0400},
	{"stderr",		QTFILE,		QCstderr,		0400},
	{"wait",		QTFILE,		QCwait,		0400},
};

static void		dostat(Xfid*, Dir*, Qid*);
static Dirtab* 	finddir(Dirtab*, int, char*);
static int 		dodirgen(int i, Dir *d, void *v);
static void		printstr(Req*, char*, ...);
static int		writestr(Req*, char**);
static char*	cmdctl(Cmd*, int, char*);
static void		waitproc(void*);

static void		io(Req*, int, int);
static void		waitio(Req*, Channel*);
static void		xioproc(void*);

static Cmd*	newcmd();
static Cmd* 	findcmd(int which);
static Cmd* 	findcmdpid(int pid);
static void		decrcmd(Cmd*);

static void
fsattach(Req *r)
{
	Xfid *xfid;

	xfid = emalloc(sizeof *xfid);
	xfid->dir = dirtab[0];
	r->fid->aux = xfid;
	dostat(xfid, nil, &r->fid->qid);
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}

static void
fsstat(Req *r)
{
	dostat(r->fid->aux, &r->d, nil);
	respond(r, nil);
}

static void
fsopen(Req *r)
{
	respond(r, nil);
}

static void
fsread(Req *r)
{
	Xfid *xfid;

	xfid = r->fid->aux;
	switch(xfid->dir.qid){
	case Qdir:
		dirread9p(r, dodirgen, xfid);
		break;
	case QCctl:
		printstr(r, "%d %d", xfid->c->id, xfid->c->pid);
		break;
	case QCstdout:
		io(r, xfid->c->stdout[0], Ioread);
		return;
	case QCstderr:
		io(r, xfid->c->stderr[0], Ioread);
		return;
	case QCwait:
		waitio(r, xfid->c->waitc);
		return;
	default:
		respond(r, "bug");
		return;
	}
	respond(r, nil);
}

static void
fswrite(Req *r)
{
	Xfid *xfid;
	char *p, *q, *err;
	int ctl;

	xfid = r->fid->aux;
	p = nil;
	err = nil;

	switch(xfid->dir.qid){
	case QCctl:
		writestr(r, &p);
		ctl = Ctlerr;
		if(strcmp(p, "start") == 0)
			ctl = Ctlstart;
		else if(strcmp(p, "eof") == 0)
			ctl = Ctleof;
		else if(strcmp(p, "del") == 0)
			ctl = Ctldel;
		else{
			q = strchr(p, ' ');
			if(q == nil)
				goto Ctldone;
			*q++ = 0;
			if(strcmp(p, "env") == 0)
				ctl = Ctlenv;
			else if(strcmp(p, "dir") == 0)
				ctl = Ctldir;
			else if(strcmp(p, "cmd") == 0)
				ctl = Ctlcmd;
			else if(strcmp(p, "note") == 0)
				ctl = Ctlnote;
		}

Ctldone:
		if(ctl == Ctlerr){
			free(p);
			respond(r, "bad command");
			return;
		}
		err = cmdctl(xfid->c, ctl, q);
		free(p);
		break;
	case QCstdin:
		io(r, xfid->c->stdin[0], Iowrite);
		return;
	}
	respond(r, err);
}


static char*
fswalk1(Fid *fid, char *name, Qid *qid)
{
	Xfid *xfid;
	Dirtab *dir;
	char *q;
	int i;
	Cmd *c;

	xfid = fid->aux;
	dir = nil;

	if(strcmp(name, "..") == 0){
		decrcmd(xfid->c);
		xfid->c = nil;
		dir = &dirtab[0];
	}else if(xfid->c)
		dir = finddir(dirtabc, nelem(dirtabc), name);
	else if((dir = finddir(dirtab, nelem(dirtab), name)) != nil){
		if(dir->qid != Qnew)
			goto Found;
		c = newcmd();
		xfid->c = c;
		dir = &dirtabc[0];
	}else{
		i = strtoul(name, &q, 10);
		if(q == name)
			goto Notfound;
		xfid->c = findcmd(i);
		if(xfid->c == nil)
			goto Notfound;
		dir = &dirtabc[0];
	}
	if(dir == nil)
		goto Notfound;
Found:
	xfid->dir = *dir;
	dostat(xfid, nil, qid);
	return nil;

Notfound:
	return "no such file";
}

static char*
fsclone(Fid *oldfid, Fid *newfid)
{
	Xfid *oldxfid, *newxfid;

	oldxfid = oldfid->aux;
	newxfid = emalloc(sizeof *newxfid);
	*newxfid = *oldxfid;
	if(newxfid->c){
		qlock(&cmdlk);
		newxfid->c->ref++;
		qunlock(&cmdlk);
	}
	newfid->aux = newxfid;
	return nil;
}

static void
fsdestroyfid(Fid *fid)
{
	Xfid *xfid;

	xfid = fid->aux;
	if(xfid == nil)
		return;

	decrcmd(xfid->c);
	free(xfid);
}

static void
dostat(Xfid *xfid, Dir *d, Qid *q)
{
	Qid qid;

	qid.path = xfid->dir.qid;
	if(xfid->c)
		qid.path |= xfid->c->id << 8;
	qid.type = xfid->dir.type;
	qid.vers = 0;

	if(d){
		d->name = estrdup(xfid->dir.name);
		d->muid = estrdup("muid");
		d->mode = xfid->dir.perm;
		d->uid = estrdup(fsowner);
		d->gid = estrdup(fsowner);
		d->length = 0;
		d->qid = qid;
	}
	if(q)
		*q = qid;
}

static Dirtab*
finddir(Dirtab *tab, int n, char *name)
{
	int i;
	for(i=0; i<n; i++)
	if(strcmp(tab[i].name, name) == 0)
		return &tab[i];
	return nil;
}

static int
dodirgen(int i, Dir *d, void *v)
{
	Xfid *xfid, dxfid;

	xfid = v;

	if(xfid->c){
		if(i >= nelem(dirtabc))
			return -1;
		dxfid.c = xfid->c;
		dxfid.dir = dirtabc[i];
	}else{
		/* TODO: enumerate commands */
		if(i >= nelem(dirtab))
			return -1;
		dxfid.c = nil;
		dxfid.dir = dirtab[i];
	}
	dostat(&dxfid, d, nil);
	return 0;
}

static void
printstr(Req *r, char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	r->ofcall.count = vsnprint(r->ofcall.data, r->ifcall.count, fmt, arg);
	va_end(arg);

	memmove(
		r->ofcall.data,
		r->ofcall.data + r->ifcall.offset,
		r->ofcall.count - r->ifcall.offset);
	r->ofcall.count -= r->ifcall.offset;
}

static int
writestr(Req *r, char **p)
{
	int n;
	if(*p)
		n = strlen(*p);
	else
		n = 0;

	*p = erealloc(*p, n+r->ifcall.count+1);
	memmove(*p+n, r->ifcall.data, r->ifcall.count);
	(*p)[n+r->ifcall.count] = 0;
	r->ofcall.count = r->ifcall.count;
	return r->ifcall.count;
}

static void
xioproc(void *v)
{
	Io *io;
	Waitmsg *w;
	char err[ERRMAX];

	io = v;
	switch(io->op){
	case Ioread:
		io->r->ofcall.count =
			read(io->fd, io->r->ofcall.data, io->r->ifcall.count);
		break;
	case Iowrite:
		io->r->ofcall.count =
			write(io->fd, io->r->ifcall.data, io->r->ifcall.count);
		break;
	case Iowait:
		w = recvp(io->waitc);
		readstr(io->r, w->msg);
		sendp(io->waitc, w);
		break;
	}
	if(io->r->ofcall.count < 0){
		io->r->ofcall.count = 0;
		rerrstr(err, sizeof(err));
		respond(io->r, err);
	}else
		respond(io->r, nil);

	free(io);
}

static void
io(Req *r, int fd, int op)
{
	Io *io;
	io = emalloc(sizeof *io);
	io->r = r;
	io->fd = fd;
	io->op = op;
	proccreate(xioproc, io, 8192);
}

static void
waitio(Req *r, Channel *c)
{
	Io *io;
	io = emalloc(sizeof *io);
	io->r = r;
	io->op = Iowait;
	io->waitc = c;
	proccreate(xioproc, io, 8192);
}

static char*
cmdctl(Cmd *c, int ctl, char *arg)
{
	char *rcarg[4], *q;
	int cfd[3];
	Waitmsg *w;
	Env *env;

	switch(ctl){
	case Ctlenv:
		q = strchr(arg, '=');
		if(q == nil)
			return "invalid env";
		*q++ = 0;
		for(env=c->env; env && strcmp(env->name, arg)!=0; env=env->next);
		if(env){
			free(env->value);
			env->value = estrdup(q);
		}else{
			env = emalloc(sizeof *env);
			env->name = estrdup(arg);
			env->value = estrdup(q);
			env->next = c->env;
			c->env = env;
		}
		break;
	case Ctlcmd:
		if(c->body)
			free(c->body);
		c->body = estrdup(arg);
		break;
	case Ctldir:
		if(c->dir)
			free(c->dir);
		c->dir = estrdup(arg);
		break;
	case Ctlstart:
		rcarg[0] = "rc";
		rcarg[1] = "-c";
		rcarg[2] = c->body;
		rcarg[3] = nil;

		cfd[0] = c->stdin[1];
		cfd[1] = c->stdout[1];
		cfd[2] = c->stderr[1];
		for(env=c->env; env; env=env->next)
			putenv(env->name, env->value);
		rfork(RFFDG|RFNOTEG);
		c->pid = threadspawnd(cfd, rcarg[0], rcarg, c->dir);
		if(c->pid < 0){
			w = emalloc(sizeof *w);
			w->pid = -1;
			w->time[0] = w->time[1] = w->time[2] = time(nil);
			w->msg = "failed to start";
			close(cfd[0]);
			close(cfd[1]);
			close(cfd[2]);
			sendp(c->waitc, w);
		}
		break;
	case Ctleof:
		close(c->stdin[0]);
		break;
	case Ctlnote:
		if(postnote(PNGROUP, c->pid, arg) < 0)
			return "could not kill process";
		break;
	case Ctldel:
		decrcmd(c);
		break;
	}
	return nil;
}

static void
waitproc(void *v)
{
	Waitmsg *w;
	Cmd *c;

	threadsetname("waitproc");

	USED(v);

	for(;;){
		w = recvp(cwait);
		c = findcmdpid(w->pid);
		if(c == nil)
			continue;
		sendp(c->waitc, w);
		decrcmd(c);
	}

}

static Cmd*
newcmd()
{
	Cmd *c;

	c = emalloc(sizeof *c);
	c->ref = 2; /* one for wait, one for the caller */
	c->id = ++cmdnum;
	c->waitc = chancreate(sizeof(Waitmsg*), 1);
	c->pid = -1;
	if(pipe(c->stdin) < 0 || pipe(c->stdout) < 0 || pipe(c->stderr) < 0)
		sysfatal("can't create pipe");
	qlock(&cmdlk);
	c->next = cmdlist;
	cmdlist = c;
	qunlock(&cmdlk);
	return c;
}

static Cmd*
findcmd(int id)
{
	Cmd *c;
	if(id == 0)
		return nil;
	qlock(&cmdlk);
	for(c=cmdlist; c; c=c->next)
		if(c->id == id)
			break;
	if(c != nil)
		c->ref++;
	qunlock(&cmdlk);
	return c;
}

static Cmd*
findcmdpid(int pid)
{
	Cmd *c;
	qlock(&cmdlk);
	for(c=cmdlist; c; c=c->next)
		if(c->pid == pid)
			break;
	if(c != nil)
		c->ref++;
	qunlock(&cmdlk);
	return c;
}

static void
decrcmd(Cmd *c)
{
	Cmd **p;
	Env *e;

	if(c == nil)
		return;
	qlock(&cmdlk);
	if(--c->ref > 0){
		qunlock(&cmdlk);
		return;
	}
	for(p = &cmdlist; *p; p=&(*p)->next)
		if(*p == c)
			break;
	if(*p)
		*p = (*p)->next;
	qunlock(&cmdlk);

	free(c->body);
	free(c->dir);
	while(c->env){
		e = c->env->next;
		free(c->env);
		c->env = e;
	}
	close(c->stdin[0]);
	close(c->stdout[0]);
	close(c->stderr[0]);
	chanfree(c->waitc);
	free(c);
}


static void
fsstart()
{
	cwait = threadwaitchan();
	proccreate(waitproc, cwait, 8192);
}

static Srv fs = {
	.attach = fsattach,
	.stat = fsstat,
	.open = fsopen,
	.read = fsread,
	.write = fswrite,
	.walk1 = fswalk1,
	.clone = fsclone,
	.destroyfid = fsdestroyfid,
	.start = fsstart,
};

Srv*
cmdfsinit(char *owner)
{
	fsowner = owner;
	return &fs;
}
