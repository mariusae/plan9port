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


Vfd
vopen(char *file, int omode)
{
	Remote *r;
	Session *sess;
	Vfd v;

	v.which = Verr;

	if((r = remote(file)) != nil){
		sess = rconnect(r);
		if(sess == nil)
			goto Done;
		/* TODO: strip prefix */
		v.fid = fsopen(sess->fs, file, omode);
		v.sess = sess;
		if(v.fid != nil)
			v.which = Vremote;
	}else{
		v.fd = open(file, omode);
		if(v.fd >= 0)
			v.which = Vlocal;
	}

Done:
	return v;
}

Dir*
vdirstat(char *file)
{
	Remote *r;
	Session *sess;
	Dir *d;

	if((r = remote(file)) == nil)
		return dirstat(file);
	if((sess = rconnect(r)) == nil)
		return nil;
	d = fsdirstat(sess->fs, file);
	rclose(sess);
	return d;
}

Vfd
vcreate(char *file, int omode, ulong perm)
{
	Remote *r;
	Session *sess;
	Vfd v;

	v.which = Verr;

	if((r = remote(file)) == nil){
		v.fd = create(file, omode, perm);
		if(v.fd >= 0)
			v.which = Vlocal;
		return v;
	}
	if((sess = rconnect(r)) != nil){
		v.fid = fscreate(sess->fs, file, omode, perm);
		if(v.fid != nil){
			v.which = Vremote;
			v.sess = sess;
		}
	}
	return v;
}


Dir*
vdirfstat(Vfd fd)
{
	switch(fd.which){
	default:
		return nil;
	case Vlocal:
		return dirfstat(fd.fd);
	case Vremote:
		return fsdirfstat(fd.fid);
	}
}

long
vdirread(Vfd fd, Dir **d)
{
	switch(fd.which){
	default:
		return -1;
	case Vlocal:
		return dirread(fd.fd, d);
	case Vremote:
		return fsdirread(fd.fid, d);
	}
}

long
vdirreadall(Vfd fd, Dir **d)
{
	switch(fd.which){
	default:
		return -1;
	case Vlocal:
		return dirreadall(fd.fd, d);
	case Vremote:
		return fsdirreadall(fd.fid, d);
	}
}

/* TODO: pass by pointer is a hack here. */
int
vclose(Vfd *fd)
{
	switch(fd->which){
	case Vlocal:
		fd->which = Vclosed;
		return close(fd->fd);
	case Vremote:
		fd->which = Vclosed;
		fsclose(fd->fid);
		rclose(fd->sess);
		fd->sess = nil;
		return 0;
	}
	return -1;
}

long
vread(Vfd fd, void *buf, long nbytes)
{
	switch(fd.which){
	default:
		return -1;
	case Vlocal:
		return read(fd.fd, buf, nbytes);
	case Vremote:
		return fsread(fd.fid, buf, nbytes);
	}

}

long
vwrite(Vfd fd, void *buf, long n)
{
	switch(fd.which){
	default:
		return -1;
	case Vlocal:
		return write(fd.fd, buf, n);
	case Vremote:
		return fswrite(fd.fid, buf, n);
	}
}

int
vaccess(char *file, int mode)
{
	Remote *r;
	Session *sess;
	int rv;

	if((r = remote(file)) == nil)
		return access(file, mode);
	if((sess = rconnect(r)) == nil)
		return -1;

	rv = fsaccess(sess->fs, file, mode);
	rclose(sess);
	return rv;
}