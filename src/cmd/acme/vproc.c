#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include <plumb.h>
#include <libsec.h>
#include <9pclient.h>
#include <complete.h>
#include "dat.h"
#include "fns.h"

enum
{
	Fd2fid,
	Fid2fd,
};

static Vpid
errorvp()
{
	Vpid vp = { nil, -1 };
	return vp;
}

static void
inrelayproc(void *v)
{
	/* args: */
		CFid *fid;
		int fd;
	/* end of args */
	void **a;
	char buf[256];
	int n;

	a = v;
	fid = a[0];
	fd = (uintptr)a[1];
	free(a);

	for(;;){
		if((n = fsread(fid, buf, sizeof buf)) <= 0)
			break;
		if(write(fd, buf, n) != n)
			break;
		/* TODO: report unexpected failures */
	}
	close(fd);
	fsclose(fid);
}

static void
inrelay(CFid *fid, int fd)
{
	void **a;

	a = emalloc(sizeof(void*)*2);
	a[0] = fid;
	a[1] = (void*)(uintptr)fd;
	proccreate(inrelayproc, a, STACK);
}

static void
outrelayproc(void *v)
{
	/* args: */
		int fd;
		CFid *fid;
		CFid *ctl;
	/* end of args */
	void **a;
	char buf[1024];
	int n;

	a = v;
	fd = (uintptr)a[0];
	fid = a[1];
	ctl = a[2];
	free(a);

	for(;;){
		n = read(fd, buf, sizeof buf);
		if(n <= 0)
			break;
		if(fswrite(fid, buf, n) != n)
			break;
	}
	fsprint(ctl, "eof");
	close(fd);
	fsclose(fid);
	fsclose(ctl);
}

static void
outrelay(int fd, CFid *fid, CFid *ctl)
{
	void **a;

	a = emalloc(sizeof(void*)*3);
	a[0] = (void*)(uintptr)fd;
	a[1] = fid;
	a[2] =ctl;
	proccreate(outrelayproc, a, STACK);
}

static void
waitproc(void *v)
{
	Vpid *vp;
	Vwaitmsg *vw;
	CFid *fid;
	char buf[128];
	int n;

	vp = v;
	snprint(buf, sizeof buf, "%d/wait", vp->id);
	fid = fsopen(vp->sess->cmd, buf, OREAD);
	if(fid == nil){
		warning(nil, "can't wait for remote process: %r\n");
		return;
	}
	if((n=fsread(fid, buf, sizeof buf-1)) < 0){
		strcpy(buf, "unknown");
		n = strlen(buf);
	}
	buf[n] = 0;

	vw = emalloc(sizeof *vw);
	vw->vp = *vp;
	vw->msg = estrdup(buf);
	sendp(cvwait, vw);
	free(vp);
}

static int
vputenv(CFid *ctl, char *env)
{
	int rv;
	char *p;

	p = getenv(env);
	if(p == nil)
		return 0;
	rv = fsprint(ctl, "env %s=%s", env, p);
	free(p);
	return rv;
}

int
vpostnote(Vpid vp, char *note)
{
	char buf[128];
	CFid *fid;
	int ret;

	if(vp.sess == nil)
		return postnote(PNGROUP, vp.id, note);

	snprint(buf, sizeof buf, "%d/ctl", vp.id);
	fid = fsopen(vp.sess->cmd, buf, OWRITE);
	if(fid == nil)
		return -1;
	ret = fsprint(fid, "note %s", note);
	fsclose(fid);

	return ret < 0 ? -1 : 0;
}

int
vpcmp(Vpid vp1, Vpid vp2)
{
	if(vp1.sess == vp2.sess)
		if(vp1.id < vp2.id)
			return -1;
		else if(vp1.id > vp2.id)
			return 1;
		else
			return 0;
	else if(vp1.sess < vp2.sess)
		return -1;
	else
		return 1;
}

Vpid
vshell(Remote *r, int fd[3], char *cmd, char *dir)
{
	Session *sess;
	CFsys *fs;
	CFid *ctl, *fid[3];
	Vpid vp, *vpp;
	int id, n;
	char buf[128];

	sess = nil;
	ctl = nil;
	fid[0] = fid[1] = fid[2] = nil;
	sess = rconnect(r);
	fs = sess->cmd;
	ctl = fsopen(fs, "new/ctl", ORDWR);
	if(ctl == nil){
		rclose(sess);
		return errorvp();
	}
	n = fsread(ctl, buf, sizeof buf-1);
	if(n <= 0)
		goto Error;
	buf[n] = 0;
	id = atoi(buf);

	if(fsprint(ctl, "cmd %s", cmd) <= 0)
		goto Error;

	/* TODO: set this up in a single ctl message to
	  * avoid the unnecessary roundtrips. These are noticeable
	  * in high-latency connections. */
	if(vputenv(ctl, "%") < 0)
		goto Error;
	if(vputenv(ctl, "samfile") < 0)
		goto Error;
	if(vputenv(ctl, "acmeaddr") < 0)
		goto Error;
	if(vputenv(ctl, "winid") < 0)
		goto Error;
	if(dir && fsprint(ctl, "dir %s", dir) <= 0)
		goto Error;

	snprint(buf, sizeof buf, "%d/stdin", id);
	fid[0] = fsopen(fs, buf, OWRITE);
	snprint(buf, sizeof buf, "%d/stdout", id);
	fid[1] = fsopen(fs, buf, OREAD);
	snprint(buf, sizeof buf, "%d/stderr", id);
	fid[2] = fsopen(fs, buf, OREAD);

	if(fid[0] == nil || fid[1] == nil || fid[2] == nil)
		goto Error;

	if(fsprint(ctl, "start") <= 0)
		goto Error;

	outrelay(fd[0], fid[0], ctl);
	inrelay(fid[1], dup(fd[1], -1));
	inrelay(fid[2], dup(fd[2], -1));
	close(fd[1]);
	close(fd[2]);

	vp.sess = sess;
	vp.id = id;
	vpp = emalloc(sizeof *vpp);
	*vpp = vp;
	proccreate(waitproc, vpp, STACK);
	return vp;

Error:
	rclose(sess);
	fsclose(ctl);
	fsclose(fid[0]);
	fsclose(fid[1]);
	fsclose(fid[2]);
	return errorvp();
}
