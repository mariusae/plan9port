/*
 * Broker mode for sam -B: discover/dispatch/spawn sam instances via tmux.
 *
 * When $TMUX is not set, execs into sam -dfa <files>.
 * When $TMUX is set, finds unix sockets for sam instances in the current
 * tmux window and either sends B commands to an existing instance,
 * shows a menu for multiple instances, or starts a new one via split-window.
 */

#include <u.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#include "sam.h"

/* Undo plan9port overrides so we can use BSD socket calls */
#undef listen
#undef accept

int Bflag;
int Nflag;
char *broker_sockpath;  /* -t sockpath for direct connect */
char *broker_argv0;     /* original argv[0] for re-exec */

/*
 * Resolve a file argument (possibly with :line:col suffix) to an absolute path.
 * Returns a malloc'd string.
 */
static char *
resolve_arg(char *arg)
{
	char *colon1, *colon2, *filepart, *suffix;
	char resolved[4096];
	char result[4096];

	/* Find :line or :line:col suffix */
	suffix = nil;
	filepart = strdup(arg);

	/* Try stripping :col, then :line */
	colon1 = strrchr(filepart, ':');
	if(colon1 && colon1 != filepart && colon1[1] != '\0'){
		char *end;
		strtol(colon1 + 1, &end, 10);
		if(*end == '\0'){
			*colon1 = '\0';
			colon2 = strrchr(filepart, ':');
			if(colon2 && colon2 != filepart && colon2[1] != '\0'){
				char *end2;
				strtol(colon2 + 1, &end2, 10);
				if(*end2 == '\0'){
					/* file:line:col */
					*colon2 = '\0';
					suffix = colon2;  /* remember where we cut */
				} else {
					/* file:line only */
					suffix = colon1;
				}
			} else {
				suffix = colon1;
			}
		} else {
			/* no numeric suffix */
			suffix = nil;
			free(filepart);
			filepart = strdup(arg);
		}
	}

	/* Resolve the file portion to absolute path */
	if(filepart[0] == '/'){
		snprint(resolved, sizeof resolved, "%s", filepart);
	} else {
		char cwd[2048];
		if(getwd(cwd, sizeof cwd) == nil)
			cwd[0] = '\0';
		snprint(resolved, sizeof resolved, "%s/%s", cwd, filepart);
	}

	/* Reattach suffix */
	if(suffix){
		/* Rebuild: suffix points into filepart at the NUL we inserted */
		/* We need to reconstruct from the original arg */
		int filelen = strlen(filepart);
		char *orig_suffix = arg + filelen;
		snprint(result, sizeof result, "%s%s", resolved, orig_suffix);
	} else {
		snprint(result, sizeof result, "%s", resolved);
	}

	free(filepart);
	return strdup(result);
}

/*
 * Resolve the path to our own sam binary.
 * Uses broker_argv0 if it contains a slash (relative or absolute path),
 * otherwise falls back to $PLAN9/bin/sam.
 */
static char *
sam_binary(void)
{
	char cwd[2048];
	char resolved[4096];

	if(broker_argv0 && strchr(broker_argv0, '/')){
		if(broker_argv0[0] == '/')
			return strdup(broker_argv0);
		if(getwd(cwd, sizeof cwd))
			snprint(resolved, sizeof resolved, "%s/%s", cwd, broker_argv0);
		else
			snprint(resolved, sizeof resolved, "%s", broker_argv0);
		return strdup(resolved);
	}
	return smprint("%s/bin/sam", get9root());
}

/*
 * Get the tmux session:window identifier for the current pane.
 */
static int
get_tmux_window(char *buf, int bufsize)
{
	FILE *fp;
	int n;

	fp = popen("tmux display-message -p '#S:#I'", "r");
	if(fp == nil)
		return -1;
	n = fread(buf, 1, bufsize - 1, fp);
	pclose(fp);
	if(n <= 0)
		return -1;
	/* strip trailing newline */
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
		n--;
	buf[n] = '\0';
	return 0;
}

static int try_connect(char *path);

/*
 * Extract the pid from a socket path.
 * Socket name format: session:window.pid.sock
 */
static int
sock_pid(char *path)
{
	char *base, *dot, *dot2;

	base = strrchr(path, '/');
	if(base)
		base++;
	else
		base = path;
	dot = strchr(base, '.');
	if(dot == nil)
		return -1;
	dot2 = strrchr(base, '.');
	if(dot2 == dot)
		return -1;
	return atoi(dot + 1);
}

/*
 * Query a sam instance for its current filename via the socket.
 * Connects, sends "?\n", reads response. Returns malloc'd string or nil.
 */
static char *
query_curfile(char *sockpath)
{
	int fd, n;
	char buf[1024];
	fd_set fds;
	struct timeval tv;

	fd = try_connect(sockpath);
	if(fd < 0)
		return nil;
	write(fd, "?\n", 2);

	/* Wait up to 500ms for response */
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	tv.tv_sec = 0;
	tv.tv_usec = 500000;
	if(select(fd + 1, &fds, nil, nil, &tv) <= 0){
		close(fd);
		return nil;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if(n <= 0)
		return nil;
	/* strip trailing newline */
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
		n--;
	buf[n] = '\0';
	return strdup(buf);
}

/*
 * Map a pid to its tmux pane index in the current window.
 * Returns pane index (0-based) or -1 if not found.
 * Uses tmux list-panes to find the pane whose shell is an ancestor of pid.
 */
static int
pid_to_pane(int pid)
{
	FILE *fp;
	char line[256];
	int pane_idx, pane_pid;

	fp = popen("tmux list-panes -F '#{pane_index} #{pane_pid}'", "r");
	if(fp == nil)
		return -1;
	while(fgets(line, sizeof line, fp)){
		if(sscanf(line, "%d %d", &pane_idx, &pane_pid) == 2){
			/* Check if our target pid is a descendant of this pane's shell.
			 * Simple approach: check if pane_pid is an ancestor of pid
			 * by walking /proc or just checking direct parent. On macOS
			 * we use ps to check. For simplicity, try matching pane_pid
			 * as parent of pid by running ps. */
			char cmd[128];
			FILE *ps;
			int ppid, cur;

			/* Walk up the process tree from pid to see if we hit pane_pid */
			cur = pid;
			while(cur > 1 && cur != pane_pid){
				snprint(cmd, sizeof cmd, "ps -o ppid= -p %d", cur);
				ps = popen(cmd, "r");
				if(ps == nil)
					break;
				if(fscanf(ps, "%d", &ppid) != 1){
					pclose(ps);
					break;
				}
				pclose(ps);
				cur = ppid;
			}
			if(cur == pane_pid){
				pclose(fp);
				return pane_idx;
			}
		}
	}
	pclose(fp);
	return -1;
}

/*
 * Build the socket directory path: /tmp/sam-tmux-$USER/
 */
static void
sock_dir(char *buf, int bufsize)
{
	snprint(buf, bufsize, "/tmp/sam-tmux-%s", getuser());
}

/*
 * Try to connect to a socket. Returns fd on success, -1 on failure.
 * On ECONNREFUSED, unlinks the stale socket.
 */
static int
try_connect(char *path)
{
	int fd;
	struct sockaddr_un addr;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if(fd < 0)
		return -1;

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprint(addr.sun_path, sizeof addr.sun_path, "%s", path);

	if(connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0){
		close(fd);
		if(errno == ECONNREFUSED)
			unlink(path);
		return -1;
	}
	return fd;
}

/*
 * Send B commands for each file to an open socket fd, then close it.
 */
static void
send_files(int fd, int argc, char **argv)
{
	int i;
	char *resolved;
	char line[4096];

	for(i = 0; i < argc; i++){
		resolved = resolve_arg(argv[i]);
		snprint(line, sizeof line, "B %s\n", resolved);
		write(fd, line, strlen(line));
		free(resolved);
	}
	close(fd);
}

/*
 * Build a sam -dfa command string with resolved file arguments.
 */
static char *
build_sam_cmd(int argc, char **argv)
{
	static char cmd[8192];
	int n, i;
	char *resolved;
	char *sam;

	sam = sam_binary();
	n = snprint(cmd, sizeof cmd, "%s -dfa", sam);
	free(sam);

	for(i = 0; i < argc; i++){
		resolved = resolve_arg(argv[i]);
		n += snprint(cmd + n, sizeof(cmd) - n, " '%s'", resolved);
		free(resolved);
	}
	return cmd;
}

void
broker_main(int argc, char **argv)
{
	char *tmux;
	char winid[128];
	char dir[256];
	char prefix[256];
	char path[512];
	DIR *dp;
	struct dirent *de;
	int plen;

	/* Collected live sockets */
	char *socks[64];
	int nsocks = 0;
	int fd, i;

	tmux = getenv("TMUX");

	/* Direct-connect mode: -t sockpath */
	if(broker_sockpath){
		fd = try_connect(broker_sockpath);
		if(fd < 0){
			fprint(2, "sam -B: cannot connect to %s: %r\n", broker_sockpath);
			exits("connect");
		}
		send_files(fd, argc, argv);
		exits(nil);
	}

	/* No tmux: just exec sam -dfa files */
	if(tmux == nil || tmux[0] == '\0'){
		char *sam;
		char **nargv;
		int nargc;

		sam = sam_binary();
		nargc = argc + 4;
		nargv = malloc(sizeof(char *) * (nargc + 1));
		nargv[0] = sam;
		nargv[1] = "-d";
		nargv[2] = "-f";
		nargv[3] = "-a";
		for(i = 0; i < argc; i++)
			nargv[4 + i] = argv[i];
		nargv[4 + argc] = nil;
		execvp(sam, nargv);
		fprint(2, "sam -B: exec %s: %r\n", sam);
		exits("exec");
	}

	/* Get current tmux window ID */
	if(get_tmux_window(winid, sizeof winid) < 0){
		fprint(2, "sam -B: cannot determine tmux window\n");
		exits("tmux");
	}

	/* -N: always start a new instance in a split */
	if(Nflag){
		char *cmd = build_sam_cmd(argc, argv);
		execlp("tmux", "tmux", "split-window", "-v", cmd, nil);
		fprint(2, "sam -N: exec tmux: %r\n");
		exits("exec");
	}

	/* Build socket directory and prefix */
	sock_dir(dir, sizeof dir);
	snprint(prefix, sizeof prefix, "%s.", winid);
	plen = strlen(prefix);

	/* Scan for matching sockets */
	dp = opendir(dir);
	if(dp != nil){
		while((de = readdir(dp)) != nil){
			if(strncmp(de->d_name, prefix, plen) != 0)
				continue;
			/* Must end in .sock */
			if(strlen(de->d_name) < 6 ||
			   strcmp(de->d_name + strlen(de->d_name) - 5, ".sock") != 0)
				continue;
			snprint(path, sizeof path, "%s/%s", dir, de->d_name);
			fd = try_connect(path);
			if(fd >= 0){
				close(fd);
				if(nsocks < 64)
					socks[nsocks++] = strdup(path);
			}
			/* stale sockets already cleaned by try_connect */
		}
		closedir(dp);
	}

	if(nsocks == 0){
		/* No existing sam: start one in a split */
		char *cmd = build_sam_cmd(argc, argv);
		execlp("tmux", "tmux", "split-window", "-v", cmd, nil);
		fprint(2, "sam -B: exec tmux: %r\n");
		exits("exec");
	}

	if(nsocks == 1){
		/* One existing sam: send files directly */
		fd = try_connect(socks[0]);
		if(fd < 0){
			fprint(2, "sam -B: cannot connect to %s: %r\n", socks[0]);
			exits("connect");
		}
		send_files(fd, argc, argv);
		for(i = 0; i < nsocks; i++)
			free(socks[i]);
		exits(nil);
	}

	/* Multiple sams: show tmux menu */
	{
		char menucmd[8192];
		int n;
		char *sam;
		char filelist[4096];
		int fn;
		char *resolved;

		sam = sam_binary();

		/* Build file arguments string */
		fn = 0;
		for(i = 0; i < argc; i++){
			resolved = resolve_arg(argv[i]);
			fn += snprint(filelist + fn, sizeof(filelist) - fn, " '%s'", resolved);
			free(resolved);
		}

		n = snprint(menucmd, sizeof menucmd, "tmux display-menu -T 'Send to sam'");
		for(i = 0; i < nsocks; i++){
			char label[256];
			char *curname;
			char *basename;
			int pid, pane;

			pid = sock_pid(socks[i]);
			pane = pid > 0 ? pid_to_pane(pid) : -1;
			curname = query_curfile(socks[i]);

			/* Use just the basename of the current file */
			if(curname){
				basename = strrchr(curname, '/');
				if(basename)
					basename++;
				else
					basename = curname;
			} else {
				basename = nil;
			}

			if(pane >= 0 && basename)
				snprint(label, sizeof label, "pane %d: %s", pane, basename);
			else if(pane >= 0)
				snprint(label, sizeof label, "pane %d", pane);
			else if(basename)
				snprint(label, sizeof label, "sam: %s", basename);
			else
				snprint(label, sizeof label, "sam (pid %d)", pid);

			n += snprint(menucmd + n, sizeof(menucmd) - n,
				" '%s' '' 'run-shell \"%s -B -t %s%s\"'",
				label, sam, socks[i], filelist);

			if(curname)
				free(curname);
		}

		free(sam);
		for(i = 0; i < nsocks; i++)
			free(socks[i]);

		system(menucmd);
		exits(nil);
	}
}
