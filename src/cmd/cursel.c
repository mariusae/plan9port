
/*
By way of Russ Cox:
	https://groups.google.com/forum/#!topic/plan9port-dev/u-Lb1Ds1DBg
Modified to support outputting lines.
*/


#include <u.h>
#include <libc.h>
#include <thread.h>
#include <9pclient.h>
#include <acme.h>

void
usage()
{
	fprint(2, "usage: cursel [-l]\n");
	threadexitsall("usage");
}

int
winreadn(Win *w, char *file, void *a, int n)
{
	int i = 0, m;
	
	while(n > 0){
		m = winread(w, file, a+i, n);
		if(m <= 0)
			return i;

		i += m;
		n -= m;
	}
	
	return i;
}

void
threadmain(int argc, char **argv)
{
	int id, lines = 0;
	uint q0, q1, i, n0, n1;
	char *buf;
	Win *w;
	
	ARGBEGIN{
	case 'l':
		lines = 1;
		break;
	default:
		usage();
	}ARGEND
	
	id = atoi(getenv("winid"));
	w = openwin(id, nil);
	if(w == nil)
		sysfatal("openwin: %r");
	winreadaddr(w, nil); // open file
	winctl(w, "addr=dot");
	q0 = winreadaddr(w, &q1);
	
	if(lines){
		buf = malloc(q1);
		if(buf==nil)
			sysfatal("malloc");
		if(winseek(w, "body", 0, 0) != 0)
			sysfatal("winseek");
		if(winreadn(w, "body", buf, q1) != q1)
			sysfatal("short winread");
		n0=n1=1;
		for(i=0;i<q1;i++){
			if(buf[i]=='\n'){
				n1++;
				if(i<q0) n0++;
			}
		}

		print("%d,%d\n", n0, n1);
	}else{
		print("#%d,#%d\n", q0, q1);
	}

	threadexitsall(nil);
}
