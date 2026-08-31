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

/*
 * File watching infrastructure.
 * Platform-specific code in Darwin.c/Linux.c provides:
 *   watchplatinit() - initialize platform watcher
 *   watchplatadd(path) - add path to watch list
 *   watchplatremove(path) - remove path from watch list
 *   watchproc(void*) - blocking proc that sends events to cwatch
 */

void watchplatinit(void);
int watchplatadd(char *path);
void watchplatremove(char *path);
void watchproc(void*);

typedef struct Watched Watched;
struct Watched {
	char *path;
	int id;		/* platform-specific watch id */
	Watched *next;
};

static Watched *watched;
static QLock watchlk;

void
watchinit(void)
{
	if(cwatch == nil)
		return;
	watchplatinit();
	proccreate(watchproc, nil, STACK);
}

void
watchstart(Window *w, char *path)
{
	Watched *wt;
	int id;

	if(cwatch == nil)
		return;
	if(path == nil || path[0] == '\0')
		return;

	qlock(&watchlk);
	/* check if already watching */
	for(wt = watched; wt != nil; wt = wt->next) {
		if(strcmp(wt->path, path) == 0) {
			qunlock(&watchlk);
			return;
		}
	}

	id = watchplatadd(path);
	if(id < 0) {
		qunlock(&watchlk);
		return;
	}

	wt = emalloc(sizeof(Watched));
	wt->path = estrdup(path);
	wt->id = id;
	wt->next = watched;
	watched = wt;
	qunlock(&watchlk);
}

void
watchstop(Window *w)
{
	Watched *wt, **prev;
	char *path;
	Rune *r;
	int n;

	if(cwatch == nil)
		return;
	if(w == nil || w->body.file == nil)
		return;

	r = w->body.file->name;
	n = w->body.file->nname;
	if(r == nil || n == 0)
		return;

	path = runetobyte(r, n);
	if(path == nil)
		return;

	qlock(&watchlk);
	prev = &watched;
	for(wt = watched; wt != nil; wt = wt->next) {
		if(strcmp(wt->path, path) == 0) {
			watchplatremove(wt->path);
			*prev = wt->next;
			free(wt->path);
			free(wt);
			break;
		}
		prev = &wt->next;
	}
	qunlock(&watchlk);
	free(path);
}

/*
 * Check if file has actually changed by comparing SHA1.
 * Returns 1 if changed, 0 if same.
 */
static int
filechanged(File *f, char *path)
{
	int fd, n;
	DigestState *h;
	uchar out[20];
	uchar *buf;
	Dir *d;

	d = dirstat(path);
	if(d == nil)
		return 1;	/* file deleted or inaccessible */

	/* quick check: if mtime and size match stored values, might be unchanged */
	if(f->mtime == d->mtime && f->qidpath == d->qid.path && f->dev == d->dev) {
		free(d);
		return 0;
	}
	free(d);

	/* compute SHA1 to be sure */
	fd = open(path, OREAD);
	if(fd < 0)
		return 1;

	h = sha1(nil, 0, nil, nil);
	buf = emalloc(8192);
	while((n = read(fd, buf, 8192)) > 0)
		sha1(buf, n, nil, h);
	free(buf);
	close(fd);
	sha1(nil, 0, out, h);

	if(memcmp(out, f->sha1, sizeof out) == 0)
		return 0;	/* same content */

	return 1;	/* changed */
}

/*
 * Process a file change notification.
 * Called from mousethread with row locked.
 */
void
processwatch(char *path)
{
	Window *w;
	Text *t;
	File *f;
	Rune *r;
	int n, dirty;

	if(path == nil)
		return;

	/* find window by path */
	r = bytetorune(path, &n);
	if(r == nil)
		return;

	w = lookfile(r, n);
	free(r);
	if(w == nil)
		return;

	/* skip directories and scratch windows */
	if(w->isdir || w->isscratch)
		return;

	t = &w->body;
	f = t->file;

	/* check if file actually changed */
	if(!filechanged(f, path))
		return;

	/* check if buffer is dirty */
	dirty = f->mod || t->ncache;

	if(dirty) {
		/* buffer has local changes - mark as externally modified */
		w->extmod = 1;
		windrawbutton(w);
		winsettag(w);
	} else {
		/* buffer is clean - silently reload */
		winlock(w, 'W');
		get(t, t, nil, TRUE, 0, nil, 0);
		w->extmod = 0;
		winunlock(w);
	}
}
