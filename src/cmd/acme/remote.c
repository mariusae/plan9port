#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include <bio.h>
#include <plumb.h>
#include <libsec.h>
#include <9pclient.h>
#include "dat.h"
#include "fns.h"

typedef 	struct Emsg	Emsg;

static void	readerproc(void *v);
static void	writerproc(void *v);
static void runwriter(Session *sess, char *name, Channel *c, int fd);
static void rundemuxer(Session *sess, char *name, int fd, int nc, Channel **c);
static void runmuxer(Session *sess, char *name, int fd, Channel *c, int dest);
static void	watchdogproc(void*);
static void sendcommandproc(void *v);

static void* 	srecvp(Session *s, Channel *c);
static int		ssendp(Session *s, Channel *c, void *p);

static int		dial9p(char *srv);

static Emsg*	eget();
static void		eput(Emsg*);

/* can be overriden by acme -s */
char* racmename = "acmesrv";

static int debug = 0;
static int dfd = -1;

static char *Psrv[Pmax] = {
	"exportfs",
	"cmdfs",
	"plumb",
	"acme"
};

enum
{
	Msize = 8192,
};

struct Emsg
{
	char port;
	int n;
	char buf[Msize];
	Emsg *free;
};

static QLock elk;
static Emsg *elist;

Remote*
remote(char *path)
{
	Remote *r;
	int i;
	int len;

	len = strlen(path);

	/* This is not quite right; we should make sure the path is exactly the prefix, or
	  else contains / after. */
	for(r=remotes; r; r=r->next)
	for(i=0; i < r->nprefix; i++)
		if(strlen(r->prefix[i]) <= len && memcmp(r->prefix[i], path, strlen(r->prefix[i])) == 0)
			return r;
	return nil;
}

Session*
rconnect(Remote *r)
{
	char *av[8];
	int ac, i, srvfd, ret, remotefd;
	int sfd[3], p[2];
	char buf[2];
	char *name;
	Command *c;
	Session *sess;

	if(debug && dfd < 0)
		dfd = create("/tmp/acme.remote.debug", OWRITE, 0664);

	qlock(&r->lk);
	if((sess = r->sess) != nil){
		sendul(sess->refc, 1);
		qunlock(&r->lk);
		return sess;
	}

	if(debug)
		fprint(dfd, "acme: connect: %s\n", r->machine);

	warning(nil, "remote: connecting %s\n", r->machine);
	ac = 0;
	av[ac++] = "ssh";
	av[ac++] = r->machine;
	av[ac++] = racmename;
	av[ac++] = "-n";
	av[ac++] = "/tmp/ns.acmesrv";
/*	av[ac++] = "-D";*/
	av[ac++] = nil;

	if(debug){
		int i;
		fprint(dfd, "exec");
		for(i = 0; av[i]; i++)
			fprint(dfd, " %s", av[i]);
		fprint(dfd, "\n");
	}

	if(pipe(sfd) < 0){
		warning(nil, "remote: %s: can't create pipe: %r\n", r->machine);
		goto Error;
	}
	rfork(RFFDG|RFNOTEG);
	remotefd = sfd[0];
	sfd[0] = dup(sfd[1], -1);
/*	sfd[2] = dup(2, -1);*/
	/* Have to use this otherwise we're attached to the same
	  process group as acme itself. */
	sfd[2] = dup(erroutfd, -1);

	/* TODO: dial local services first, then use this to determine
	  of them should be part of the session. */

	if((ret = threadspawn(sfd, av[0], av)) < 0){
		warning(nil, "remote: %s: can't create remote proc\n", r->machine);
		goto Error;
	}

	/* Wait until we reach the remote, then initialize session control. */
	if(read(remotefd, &buf[0], 1) != 1){
		warning(nil, "remote: %s: EOF\n", r->machine);
		goto Error;
	}
	for(;;){
		if(read(remotefd, &buf[1], 1) != 1){
			warning(nil, "remote: %s: EOF\n", r->machine);
			goto Error;
		}
		/* TODO: print out bytes before "OK". Buffer these and put them in a warning. */
		if(strncmp(buf, "OK", 2) == 0)
			break;
		buf[0] = buf[1];
	}

	/* Now we're ready to establish a session and monitor it. */
	sess = emalloc(sizeof *sess);
	sess->r = r;
	sess->remotepid = ret;
	sess->remotefd = remotefd;
	sess->errorc = chancreate(sizeof(char*), 0);
	sess->stopc = chancreate(sizeof(int), 0);
	sess->remotec = chancreate(sizeof(Emsg*), 1);
	sess->refc = chancreate(sizeof(int), 0);
	for(i=0; i<nelem(sess->localfd); i++)
		sess->localfd[i] = -1;

	/* Register the command so that it shows up in the top
	  and also so that it can be killed. */
	c = emalloc(sizeof *c);
	c->vp.sess = nil;
	c->vp.id = sess->remotepid;
	name = smprint("%s:remote ", r->machine);
	c->name = bytetorune(name, &c->nname);
	free(name);
	c->text = estrdup("");
	c->sess = sess;

	/* HACK ALERT: we send the command asynchronously to avoid
	 * a deadlock between the sending the command and displaying
	 * errors in the output. This is because we may hold the row.lk
	 * while connecting. Fix this.
	 */
	/* 	sendp(ccommand, c); */
	threadcreate(sendcommandproc, c, STACK*2);

	/* Monitor the session and provide proper teardown. */
	threadcreate(watchdogproc, sess, STACK*2);
	sendul(sess->refc, 1); /* ssh proc */
	for(i=0; i<nelem(sess->localc); i++){
		sess->localc[i] = chancreate(sizeof(Emsg*), 1);
		switch(i){
		default:
			/* TODO: just start a proc that returns errors */
			if((srvfd = dial9p(Psrv[i])) < 0){
				serror(sess, "could not connect service %s", Psrv[i]);
				sess = nil; /* so it doesn't get freed */
				goto Error;
			}
			sess->localfd[i] = srvfd;
			break;
		case Pexportfs:
		case Pcmdfs:
			if(pipe(p) < 0)
				error("can't create pipe");
			srvfd = p[0];
			sess->localfd[i] = p[1];
			break;
		}
		runwriter(sess, smprint("mux->%s", Psrv[i]), sess->localc[i], srvfd);
		runmuxer(sess, smprint("%s->mux", Psrv[i]), srvfd, sess->remotec, i);
	}

	rundemuxer(sess, estrdup("remote->mux"), sess->remotefd, nelem(sess->localc), sess->localc);
	runwriter(sess, estrdup("mux->remote"), sess->remotec, sess->remotefd);

	/* Setup services on top of the session.
	 * Note: there's a race between potential session errors from the setup
	 * code above, and setting up here. We should have some sort of lock
	 * that gates teardown (and also protects sess->fs here.)
	 */
	sess->fs = fsmount(sess->localfd[Pexportfs], nil);
	if(sess->fs == nil){
		sendul(sess->refc, 1); /* hack to make teardown work */
		serror(sess, "could not connect exportfs");
		sess = nil;
		goto Error;
	}
	sess->cmd = fsmount(sess->localfd[Pcmdfs], nil);
	if(sess->cmd == nil){
		sendul(sess->refc, 1); /* hack to make teardown work */
		serror(sess, "could not connect cmdfs");
		sess = nil;
		goto Error;
	}

	warning(nil, "remote: %s: connected \n", r->machine);


	r->sess = sess;
	sendul(sess->refc, 1); /* returned session */
	qunlock(&r->lk);
	return sess;

Error:
	qunlock(&r->lk);
	free(sess);
	return nil;
}

void
rclose(Session *sess)
{
	if(sess == nil)
		return;
	sendul(sess->refc, -1);
}

void
serror(Session *s, char *fmt, ...)
{
	va_list arg;
	char *msg;

	va_start(arg, fmt);
	msg = vsmprint(fmt, arg);
	if(msg == nil)
		error("malloc");
	sendp(s->errorc, msg);
	va_end(arg);
}

void
readerproc(void *v)
{
	/* args: */
		Session *sess;
		char *name;
		int fd;
		int wrap;
		int nc;
		Channel **c;
	/* end of args */
	void **a;
	Emsg *e;
	char port;

	a = v;
	sess = a[0];
	name = a[1];
	fd = (uintptr)a[2];
	wrap = (uintptr)a[3];
	nc = (uintptr)a[4];
	c = (Channel**)&a[5];

	for(;;){
		e = eget();
		if(wrap >= 0){
			port = 0;
			e->port = wrap;
		}else if(readn(fd, &port, 1) != 1){
			break;
		}
		if((e->n = read9pmsg(fd, e->buf, sizeof(e->buf))) <= 0)
			break;
		if(debug && wrap >= 0)
			fprint(dfd, "%s: read n:%d port:%d\n", name, e->n, e->port);
		else if(debug)
			fprint(dfd, "%s: read n:%d port:%d\n", name, e->n, port);
		if(port >= nc){
			eput(e);
			warning(nil, "remote: invalid destination\n");
		}else if(ssendp(sess, c[(int)port], e) != 0)
			break;
	}
	serror(sess, "%s: read error: %r", name);
	eput(e);
	free(a);
	free(name);
}


static void
rundemuxer(Session *sess, char *name, int fd, int nc, Channel **c)
{
	void **a;

	a = emalloc(sizeof(void*)*5+sizeof(Channel*)*nc);
	a[0] = sess;
	a[1] = name;
	a[2] = (void*)(uintptr)fd;
	a[3] = (void*)(uintptr)-1;
	a[4] = (void*)(uintptr)nc;
	memcpy(&a[5], c, sizeof(Channel*)*nc);

	sendul(sess->refc, 1);

	proccreate(readerproc, a, STACK*2);
}

static void
runmuxer(Session *sess, char *name, int fd, Channel *c, int port)
{
	void **a;

	a = emalloc(sizeof(void*)*6);
	a[0] = sess;
	a[1] = name;
	a[2] = (void*)(uintptr)fd;
	a[3] = (void*)(uintptr)port;
	a[4] = (void*)(uintptr)1;
	a[5] = c;

	sendul(sess->refc, 1);

	proccreate(readerproc, a, STACK*2);
}

void
writerproc(void *v)
{
	/* args: */
		Session *sess;
		char *name;
		int fd;
		Channel *c;
	/* end of args */
	void **a;
	Emsg *e;

	a = v;
	sess = a[0];
	name = a[1];
	fd = (uintptr)a[2];
	c = a[3];
	free(a);

	for(;;){
		if((e = srecvp(sess, c)) == nil)
			break;
		if(debug)
			fprint(dfd, "%s: write n:%d port:%d\n", name, e->n, e->port);
		if(e->port >= 0 && write(fd, &e->port, 1) != 1)
			break;
		if(write(fd, e->buf, e->n) != e->n)
			break;
		eput(e);
	}
	serror(sess, "%s: write error: %r", name);
	eput(e);
	free(name);
}

static void
runwriter(Session *sess, char *name, Channel *c, int fd)
{
	void **a;

	a = emalloc(sizeof(void*)*4);
	a[0] = sess;
	a[1] = name;
	a[2] = (void*)(uintptr)fd;
	a[3] = c;

	sendul(sess->refc, 1);
	proccreate(writerproc, a, STACK*2);
}

static void
watchdogproc(void *v)
{
	Session *s;
	char  *msg, err[ERRMAX];
	enum { Wref, Werror, Wstop, N };
	int ref, x, stopping, i;
	Alt alts[N+1];

	s = v;
	stopping = FALSE;
	ref = recvul(s->refc);

	while(ref){
		alts[Wref].c = s->refc;
		alts[Wref].v = &x;
		alts[Wref].op = CHANRCV;
		alts[Werror].c = s->errorc;
		alts[Werror].v = &msg;
		alts[Werror].op = CHANRCV;
		alts[Wstop].op = stopping ? CHANSND : CHANEND;
		alts[Wstop].v = &stopping;
		alts[Wstop].c = s->stopc;
		alts[N].op = CHANEND;

		switch(alt(alts)){
		case Wref:
			ref += x;
			break;
		case Werror:
			ref--;
			if(stopping){
				free(msg);
				break;
			}
			qlock(&s->r->lk);
			if(s->r->sess == s)
				s->r->sess = nil;
			qunlock(&s->r->lk);
			if(msg == nil){
				warning(nil, "remote: %s: remoting process died\n", s->r->machine);
			}else{
				warning(nil, "remote: %s: %s\n", s->r->machine, msg);
				free(msg);
				if(postnote(PNGROUP, s->remotepid, "kill") < 0){
					rerrstr(err, sizeof err);
					if(strcmp(err, "No such process") != 0)
						warning(nil, "remote: %s: could not kill remoting process: %r\n", s->r->machine);
				}
			}
			for(i=0; i<nelem(s->localfd); i++)
				if(s->localfd[i] >= 0)
					close(s->localfd[i]);
			stopping = TRUE;
			break;
		}
	}
	if(s->fs != nil)
		fsunmount(s->fs);
	for(i=0; i<nelem(s->localc); i++)
		chanfree(s->localc[i]);
	chanfree(s->remotec);
	chanfree(s->errorc);
	chanfree(s->refc);
	chanfree(s->stopc);
	free(s);
}

static void
sendcommandproc(void *v)
{
	sendp(ccommand, v);
}

static void*
srecvp(Session *s, Channel *c)
{
	Alt alts[3];
	void *v;

	v = nil;

	alts[0].c = c;
	alts[0].v = &v;
	alts[0].op = CHANRCV;
	alts[1].c = s->stopc;
	alts[1].v = nil;
	alts[1].op = CHANRCV;
	alts[2].op = CHANEND;
	alt(alts);
	return v;
}

static int
ssendp(Session *s, Channel *c, void *p)
{

	Alt alts[3];

	alts[0].c = c;
	alts[0].v = &p;
	alts[0].op = CHANSND;
	alts[1].c = s->stopc;
	alts[1].v = nil;
	alts[1].op = CHANRCV;
	alts[2].op = CHANEND;
	return alt(alts);
}

static int
dial9p(char *srv)
{
	char *addr;
	int fd;

	addr = smprint("unix!%s/%s", getns(), srv);
	if(debug)
		fprint(dfd, "dial9p: %s\n", addr);

	fd = dial(addr, 0, 0, 0);
	if(fd < 0){
		if(debug)
			fprint(dfd, "dial9p: %s error: %r\n", addr);
		return -1;
	}
/*	fcntl(fd, F_SETFL, FD_CLOEXEC);*/
	return fd;
}


static Emsg*
eget()
{
	Emsg *e;
	e = nil;
	qlock(&elk);
	if(elist){
		e = elist;
		elist = e->free;
		e->free = nil;
	}
	qunlock(&elk);
	if(e == nil)
		e = emalloc(sizeof *e);
	e->n = 0;
	e->port = -1;
	return e;
}

static void
eput(Emsg *e)
{
	if(e == nil)
		return;
	qlock(&elk);
	e->free = elist;
	elist = e;
	qunlock(&elk);
}
