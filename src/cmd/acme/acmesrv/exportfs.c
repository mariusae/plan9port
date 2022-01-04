#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>

#include "dat.h"

struct Efid
{
	char *file;
	int open;
	int fd;
};

struct Dirread
{
	long n;
	Dir *dir;
};

typedef struct Efid Efid;
typedef struct Dirread Dirread;

char	Ename[] = "illegal name";

static void dircopy(Dir*, Dir*);
static Efid* newefid(char *file);
static int dostat(Efid *efid, Qid *qid, Dir *dir);
static char* pathprint(char *fmt, ...);
static int dodirgen(int i, Dir *d, void *v);
static void responderrstr(Req *r);

static Srv 		fs;
static char 	*fsroot;
static char 	*fsowner;

static void
fsattach(Req *r)
{
	Efid *efid;

	efid = newefid(fsroot);
	r->fid->aux = efid;
	if(dostat(efid, &r->ofcall.qid, nil) < 0){
		responderrstr(r);
		return;
	}
	r->fid->qid = r->ofcall.qid;
	respond(r, nil);
}

static void
fsstat(Req *r)
{
	if(dostat(r->fid->aux, nil, &r->d) < 0){
		responderrstr(r);
		return;
	}
	respond(r, nil);
}

static char*
fswalk1(Fid *fid, char *name, Qid *qid)
{
	Efid *efid;
	char *file, *err;

	err = nil;

	efid = fid->aux;
	file = pathprint("%s/%s", efid->file, name);
	if(strlen(file) < strlen(fsroot))
		strcpy(file, fsroot);

	free(efid->file);
	efid->file = file;
	if(dostat(efid, qid, nil) < 0) {
		err = "could not walk";
	}
	fid->qid = *qid;
	return err;
}

static void
fsopen(Req *r)
{
	int fd;
	Efid *efid;

	efid = r->fid->aux;
	if(efid->open){
		respond(r, "already open");
		return;
	}

	fd = open(efid->file, r->ifcall.mode);
	if(fd < 0){
		respond(r, "can't open file");
		return;
	}

	efid->open = TRUE;
	efid->fd = fd;
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}

static void
fscreate(Req *r)
{
	Efid *efid;
	char *file;
	int fd;

	efid = r->fid->aux;

	if(strcmp(r->ifcall.name, ".") == 0 || strcmp(r->ifcall.name, "..") == 0){
		respond(r, Ename);
		return;
	}

	file = pathprint("%s/%s", efid->file, r->ifcall.name);
	fd = create(file, r->ifcall.mode, r->ifcall.perm);
	if(fd < 0){
		responderrstr(r);
		return;
	}
	free(efid->file);
	efid->file = file;
	if(dostat(efid, &r->ofcall.qid, nil) < 0){
		responderrstr(r);
		close(fd);
		return;
	}
	r->fid->qid = r->ofcall.qid;
	efid->open = TRUE;  /* what about ORCLOSE? */
	efid->fd = fd;
	respond(r, nil);
}

static void
fsread(Req *r)
{
	Efid *efid;
	char *err;
	int n;
	Dirread *dr;

	err = nil;
	efid = r->fid->aux;

	if(r->fid->qid.type&QTDIR){
		n = seek(efid->fd, 0, 0);
		if(n != 0){
			err = "could not seek";
			goto Done;
		}
		dr = emalloc(sizeof *dr);
		dr->n = dirreadall(efid->fd, &dr->dir);
		if(dr->n < 0){
			free(dr);
			err = "could not dirread";
			goto Done;
		}
		dirread9p(r, dodirgen, dr);
	}else{
		n = pread(efid->fd, r->ofcall.data, r->ifcall.count, r->ifcall.offset);
		if(debug)
			fprint(2, "pread %d %d %d = %d", r->ofcall.data, r->ifcall.count, r->ifcall.offset, n);
		/* TODO: errstr? */
		if(n < 0)
			n = 0;
		r->ofcall.count = n;
	}
Done:
	respond(r, err);
}

static void
fswrite(Req *r)
{
	Efid *efid;
	int n;

	efid = r->fid->aux;

	if(r->fid->qid.type&QTDIR){
		respond(r, "cannot write directory");
		return;
	}

	n = pwrite(efid->fd, r->ifcall.data, r->ifcall.count, r->ifcall.offset);
	if(n < 0){
		respond(r, "cannot write");
		return;
	}
	r->ofcall.count = n;
	respond(r, nil);
}

static char*
fsclone(Fid *oldfid, Fid *newfid)
{
	Efid *oldefid, *newefid;

	oldefid = oldfid->aux;
	if(oldefid == nil)
		return nil;

	newefid = emalloc(sizeof *newefid);
	newefid->open = FALSE;
	newefid->file = estrdup(oldefid->file);
	newfid->aux = newefid;
	return nil;
}

static void
fsdestroyfid(Fid *fid)
{
	Efid *efid;

	if(debug)
		fprint(2, "destroy fid %d\n", fid->fid);

	if(fid->aux == nil)
		return;

	efid = fid->aux;
	/* TODO: what happens when you clone an open fid?
	  Is that allowed?
	*/
	if(efid->open)
		close(efid->fd);

	free(efid->file);
	free(efid);
}

Srv*
exportfsinit(char *root, char *owner)
{
	fsroot = root;
	fsowner = owner;
	fs.attach = fsattach;
	fs.walk1 = fswalk1;
	fs.stat = fsstat;
	fs.read = fsread;
	fs.write = fswrite;
	fs.open = fsopen;
	fs.create = fscreate;
	fs.clone = fsclone;
	fs.destroyfid = fsdestroyfid;
	return &fs;
}


static void
dircopy(Dir *dst, Dir *src)
{
	*dst = *src;
	dst->name = estrdup(dst->name);
	dst->uid = estrdup(dst->uid);
	dst->gid = estrdup(dst->gid);
	dst->muid = estrdup(dst->muid);
}

static int
dostat(Efid *efid, Qid *qid, Dir *dir)
{
	Dir *d;

	d = dirstat(efid->file);
	if(d == nil)
		return -1;
	if(qid != nil)
		*qid = d->qid;
	if(dir != nil){
		dircopy(dir, d);
		if(strcmp(efid->file, fsroot) == 0){
			free(dir->name);
			dir->name = estrdup("/");
		}
	}
	free(d);
	return 0;
}

static int
dodirgen(int i, Dir *d, void *v)
{
	Dirread *dr;

	dr = v;

	if(i >= dr->n){
		free(dr->dir);
		return -1;
	}
	dircopy(d, &dr->dir[i]);
	return 0;
}

static char*
pathprint(char *fmt, ...)
{
	va_list args;
	char *v;

	va_start(args, fmt);
	v = vsmprint(fmt, args);
	va_end(args);
	return cleanname(v);
}

static Efid*
newefid(char *file)
{
	Efid *efid;
	efid = emalloc(sizeof *efid);
	efid->file = estrdup(file);
	return efid;
}

static void responderrstr(Req *r)
{
	char err[ERRMAX];

	rerrstr(err, sizeof err);
	respond(r, err);
}
