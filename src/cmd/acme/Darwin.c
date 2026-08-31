/*
 * macOS FSEvents implementation for file watching.
 * This file must be compiled separately because CoreServices
 * conflicts with plan9port's draw.h (Point, Rect types).
 */

#include <u.h>
#include <libc.h>
#include <thread.h>

#include <CoreServices/CoreServices.h>
AUTOFRAMEWORK(CoreServices)

extern Channel *cwatch;

static FSEventStreamRef stream;
static CFMutableArrayRef paths;
static CFRunLoopRef watchloop;
static int watchready;
static int needrestart;
static QLock pathlk;

static void
fsevent_callback(
	ConstFSEventStreamRef streamRef,
	void *clientCallBackInfo,
	size_t numEvents,
	void *eventPaths,
	const FSEventStreamEventFlags eventFlags[],
	const FSEventStreamEventId eventIds[])
{
	char **pathlist = (char **)eventPaths;
	size_t i;
	char *p;

	USED(streamRef);
	USED(clientCallBackInfo);
	USED(eventIds);

	for(i = 0; i < numEvents; i++) {
		/* only care about file modifications */
		if(eventFlags[i] & (kFSEventStreamEventFlagItemModified |
		                     kFSEventStreamEventFlagItemRemoved |
		                     kFSEventStreamEventFlagItemRenamed |
		                     kFSEventStreamEventFlagItemCreated)) {
			p = strdup(pathlist[i]);
			if(p != nil && cwatch != nil)
				sendp(cwatch, p);
		}
	}
}

static void
restartstream(void)
{
	FSEventStreamContext ctx = {0, nil, nil, nil, nil};

	if(stream != nil) {
		FSEventStreamStop(stream);
		FSEventStreamInvalidate(stream);
		FSEventStreamRelease(stream);
		stream = nil;
	}

	if(CFArrayGetCount(paths) == 0)
		return;

	stream = FSEventStreamCreate(
		kCFAllocatorDefault,
		fsevent_callback,
		&ctx,
		paths,
		kFSEventStreamEventIdSinceNow,
		0.5,	/* latency in seconds */
		kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer
	);

	if(stream == nil)
		return;

	FSEventStreamScheduleWithRunLoop(stream, watchloop, kCFRunLoopDefaultMode);
	FSEventStreamStart(stream);
}

static void
checkandrestart(void)
{
	qlock(&pathlk);
	if(needrestart) {
		restartstream();
		needrestart = 0;
	}
	qunlock(&pathlk);
}

void
watchplatinit(void)
{
	paths = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
	watchready = 0;
	needrestart = 0;
}

int
watchplatadd(char *path)
{
	CFStringRef cfpath;
	char *dir;
	char *slash;

	if(path == nil)
		return -1;

	/* FSEvents watches directories, so get parent directory */
	dir = strdup(path);
	if(dir == nil)
		return -1;
	slash = strrchr(dir, '/');
	if(slash != nil && slash != dir)
		*slash = '\0';

	cfpath = CFStringCreateWithCString(kCFAllocatorDefault, dir, kCFStringEncodingUTF8);
	free(dir);
	if(cfpath == nil)
		return -1;

	qlock(&pathlk);
	/* check if already watching this directory */
	if(!CFArrayContainsValue(paths, CFRangeMake(0, CFArrayGetCount(paths)), cfpath)) {
		CFArrayAppendValue(paths, cfpath);
		if(watchready) {
			/* signal that we need to restart the stream */
			needrestart = 1;
			CFRunLoopStop(watchloop);
		}
	}
	CFRelease(cfpath);
	qunlock(&pathlk);

	return 0;
}

void
watchplatremove(char *path)
{
	/* FSEvents watches directories - we don't remove until all files in dir are unwatched */
	/* For simplicity, we don't remove paths; they'll be cleaned up on exit */
	USED(path);
}

void
watchproc(void *v)
{
	USED(v);

	threadsetname("watchproc");

	watchloop = CFRunLoopGetCurrent();

	qlock(&pathlk);
	restartstream();
	watchready = 1;
	qunlock(&pathlk);

	/* run the event loop, checking periodically for restart requests */
	for(;;) {
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
		checkandrestart();
	}
}