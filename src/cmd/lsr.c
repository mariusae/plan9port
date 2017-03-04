/* Courtesy rsc: https://swtch.com/lsr.c  */

#include <u.h>
#include <libc.h>
#include <bio.h>

Biobuf bout;
int printdirs;
int prunixtime;

typedef struct Path Path;
struct Path
{
	Path *up;
	Dir *d;
};
#pragma varargck type "P" Path*

static void
_pathfmt(Fmt *fmt, Path *p)
{
	if(p->up && strcmp(p->d->name, ".") != 0){
		_pathfmt(fmt, p->up);
		fmtstrcpy(fmt, "/");
	}
	fmtstrcpy(fmt, p->d->name);
}

int
pathfmt(Fmt *fmt)
{
	Path *p;

	p = va_arg(fmt->args, Path*);
	_pathfmt(fmt, p);
	return 0;
}

int
dirpcmp(const void *va, const void *vb)
{
	Dir *a, *b;

	a = (Dir*)va;
	b = (Dir*)vb;
	return strcmp(a->name, b->name);
}

void
prname(Path *p)
{
	if(p->d->mode&DMDIR){
		if(!printdirs)
			return;
		Bprint(&bout, "%P/", p);
	}else
		Bprint(&bout, "%P", p);
	if(prunixtime)
		Bprint(&bout, " %luo %lud %llud", p->d->mode, p->d->mtime, p->d->length);
	Bprint(&bout, "\n");
}

void
pr(Path *p)
{
	char *s;
	Dir *d;
	Path np;
	int i, fd, n;

	prname(p);
	if(p->d->mode&DMDIR){
		s = smprint("%P", p);
		if(s == nil)
			sysfatal("smprint: %r");
		if((fd = open(s, OREAD)) < 0){
			fprint(2, "cannot open %s: %r\n", s);
			free(s);
			return;
		}
		free(s);
		n = dirreadall(fd, &d);
		close(fd);
		if(n < 0){
			fprint(2, "cannot read %P: %r\n", p);
			return;
		}
		qsort(d, n, sizeof(d[0]), dirpcmp);
		for(i=0; i<n; i++){
			np.d = &d[i];
			np.up = p;
			pr(&np);
		}
		free(d);
	}
}

void
pr1(char *s)
{
	Path p;
	Dir *d;

	if((d = dirstat(s)) == nil){
		fprint(2, "stat %s: %r\n", s);
		return;
	}
	p.d = d;
	d->name = s;
	p.up = nil;
	pr(&p);
	free(d);
}

void
main(int argc, char **argv)
{
	int i;

	ARGBEGIN{
	case 't':
		prunixtime = 1;
		break;
	case 'd':
		printdirs = 1;
		break;
	}ARGEND

	Binit(&bout, 1, OWRITE);

	fmtinstall('P', pathfmt);

	if(argc)
		for(i=0; i<argc; i++)
			pr1(argv[i]);
	else
		pr1(".");
	Bterm(&bout);
}
