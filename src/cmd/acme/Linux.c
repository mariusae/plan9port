/*
 * Linux inotify implementation for file watching.
 */

#include <u.h>
#include <libc.h>
#include <thread.h>

#include <sys/inotify.h>
#include <limits.h>
#include <errno.h>

extern Channel *cwatch;

typedef struct WatchEntry WatchEntry;
struct WatchEntry {
	int wd;		/* inotify watch descriptor */
	char *path;	/* full path */
	WatchEntry *next;
};

static int inotifyfd = -1;
static WatchEntry *entries;
static QLock entrylk;

void
watchplatinit(void)
{
	inotifyfd = inotify_init1(IN_CLOEXEC);
	if(inotifyfd < 0)
		fprint(2, "acme: inotify_init failed: %r\n");
}

int
watchplatadd(char *path)
{
	WatchEntry *e;
	int wd;

	if(inotifyfd < 0 || path == nil)
		return -1;

	wd = inotify_add_watch(inotifyfd, path,
		IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
	if(wd < 0)
		return -1;

	qlock(&entrylk);
	e = malloc(sizeof(WatchEntry));
	if(e == nil) {
		qunlock(&entrylk);
		return -1;
	}
	e->wd = wd;
	e->path = strdup(path);
	e->next = entries;
	entries = e;
	qunlock(&entrylk);

	return wd;
}

void
watchplatremove(char *path)
{
	WatchEntry *e, **prev;

	if(inotifyfd < 0)
		return;

	qlock(&entrylk);
	prev = &entries;
	for(e = entries; e != nil; e = e->next) {
		if(strcmp(e->path, path) == 0) {
			inotify_rm_watch(inotifyfd, e->wd);
			*prev = e->next;
			free(e->path);
			free(e);
			break;
		}
		prev = &e->next;
	}
	qunlock(&entrylk);
}

static char*
findpath(int wd)
{
	WatchEntry *e;
	char *path = nil;

	qlock(&entrylk);
	for(e = entries; e != nil; e = e->next) {
		if(e->wd == wd) {
			path = strdup(e->path);
			break;
		}
	}
	qunlock(&entrylk);
	return path;
}

void
watchproc(void *v)
{
	char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
	struct inotify_event *ev;
	ssize_t n;
	char *p, *path;

	USED(v);

	threadsetname("watchproc");

	if(inotifyfd < 0)
		return;

	for(;;) {
		n = read(inotifyfd, buf, sizeof buf);
		if(n <= 0) {
			if(n < 0 && errno == EINTR)
				continue;
			break;
		}

		for(p = buf; p < buf + n; ) {
			ev = (struct inotify_event *)p;

			if(ev->mask & (IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB)) {
				path = findpath(ev->wd);
				if(path != nil && cwatch != nil)
					sendp(cwatch, path);
			}

			p += sizeof(struct inotify_event) + ev->len;
		}
	}
}
