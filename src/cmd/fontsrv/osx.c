#include <u.h>

#define Point OSXPoint
#define Rect OSXRect
#define Cursor OSXCursor
#include <Carbon/Carbon.h>
#undef Rect
#undef Point
#undef Cursor
#undef offsetof
#undef nil

#include <libc.h>
#include <draw.h>
#include <memdraw.h>
#include "a.h"

extern void CGFontGetGlyphsForUnichars(CGFontRef, const UniChar[], const CGGlyph[], size_t);
extern void CGContextSetFontSmoothingStyle(CGContextRef, int);
extern int CGContextGetFontSmoothingStyle(CGContextRef);

// In these fonts, it's too hard to distinguish U+2018 and U+2019,
// so don't map the ASCII quotes there.
// See https://github.com/9fans/plan9port/issues/86
static char *skipquotemap[] = {
	"Courier",
	"Osaka",
};

enum {
	Zero = 1<<0,
	Tab = 1<<1,
	SS01 = 1<<2,
	SS02 = 1<<3,
	SS03 = 1<<4,
	SS04 = 1<<5,
	SS05 = 1<<6,
	SS06 = 1<<7,
	SS07 = 1<<8,
	SS10 = 1<<9,
	SS11 = 1<<10,
	SS12 = 1<<11,
	SS14 = 1<<12,
	SS15 = 1<<13,
	SS17 = 1<<14,

	Dquote = 1<<15,
	Lnum = 1<<16,
	Pnum = 1<<17,
	Salt = 1<<18,
	Endash = 1<<19,
};

// Store a map of font features to use.
static struct {
	char *name;
	int features;
} featuremap[] = {
	{"Vinkel", Dquote | Endash | Zero | Tab },
	{"MetaPro", Zero | Tab | Lnum | Endash },
	{"Fago", Zero | Tab | Lnum },
	{"Unit", Zero | Tab | Lnum },
	{"Gintronic", Pnum  },
	{"Operator", Zero | Tab | Lnum },
	{"Lucida", Zero },
	{"Plex", Zero | Endash },
	{"Ideal", Zero | Tab | Lnum | Endash },
	{"Whitney", Zero | Tab | Lnum |  SS11 | SS17 | Endash },
	{"Metric", Tab | Endash },
	{"Drive", Zero | Tab | Lnum |  Endash },
	{"Fira", Zero | Tab | Lnum },
	{"SF", Zero | Tab | Lnum | Endash },
	{"Syntax", Zero | Tab | Lnum | Endash },
	{"Consolas", Zero | Tab | Lnum | Endash },
	{"Calibri", Zero | Tab | Lnum | Endash },
	{"Bernino", Zero | Tab | Endash },
	{"TynineSans", Endash },
	{"Mallory", Tab | Zero | SS04 | SS06 | SS07 | Endash },
};

int
mapUnicode(char *name, int i)
{
	int j;

	if(0xd800 <= i && i < 0xe000) // surrogate pairs, will crash OS X libraries!
		return 0xfffd;
	for(j=0; j<nelem(skipquotemap); j++) {
		if(strstr(name, skipquotemap[j]))
			return i;
	}
	switch(i) {
	case '\'':
		return 0x2019;
	case '`':
		return 0x2018;
	case '"':
	for(j=0; j<nelem(featuremap); j++) {
		if(strstr(name, featuremap[j].name) && (featuremap[j].features & Dquote))
			return 0x201d;
	}
	break;
	case '-':
	for(j=0; j<nelem(featuremap); j++) {
		if(strstr(name, featuremap[j].name) && (featuremap[j].features & Endash))
			return 0x2212;
	}
	break;
	}
	return i;
}

char*
mac2c(CFStringRef s)
{
	char *p;
	int n;

	n = CFStringGetLength(s)*8;	
	p = malloc(n);
	CFStringGetCString(s, p, n, kCFStringEncodingUTF8);
	return p;
}

CFStringRef
c2mac(char *p)
{
	return CFStringCreateWithBytes(nil, (uchar*)p, strlen(p), kCFStringEncodingUTF8, false);
}

Rectangle
mac2r(CGRect r, int size, int unit)
{
	Rectangle rr;

	rr.min.x = r.origin.x*size/unit;
	rr.min.y = r.origin.y*size/unit;
	rr.max.x = (r.origin.x+r.size.width)*size/unit + 0.99999999;
	rr.max.y = (r.origin.x+r.size.width)*size/unit + 0.99999999;
	return rr;
}

void
loadfonts(void)
{
	int i, n;
	CTFontCollectionRef allc;
	CFArrayRef array;
	CFStringRef s;
	CTFontDescriptorRef f;

	allc = CTFontCollectionCreateFromAvailableFonts(0);
	array = CTFontCollectionCreateMatchingFontDescriptors(allc);
	n = CFArrayGetCount(array);
	xfont = emalloc9p(n*sizeof xfont[0]);
	for(i=0; i<n; i++) {
		f = (void*)CFArrayGetValueAtIndex(array, i);
		if(f == nil)
			continue;
		s = CTFontDescriptorCopyAttribute(f, kCTFontNameAttribute);
		xfont[nxfont].name = mac2c(s);		
		CFRelease(s);
		nxfont++;
	}
}

// Some representative text to try to discern line heights.
static char *lines[] = {
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ",
	"abcdefghijklmnopqrstuvwxyz",
	"g",
	"ÁĂÇÂÄĊÀČĀĄÅÃĥľƒ",
	"ὕαλον ϕαγεῖν δύναμαι· τοῦτο οὔ με βλάπτει.",
	"私はガラスを食べられます。それは私を傷つけません。",
	"Aš galiu valgyti stiklą ir jis manęs nežeidžia",
	"Môžem jesť sklo. Nezraní ma.",
	"camel_Snake^case.",
};

static CTFontDescriptorRef
fontfeature(CTFontDescriptorRef desc, CFStringRef feature, int value)
{
	CFNumberRef val = CFNumberCreate(CFAllocatorGetDefault(), kCFNumberIntType, &value);
	CFTypeRef keys[] = { kCTFontOpenTypeFeatureTag, kCTFontOpenTypeFeatureValue };
	CFTypeRef values[] = { feature, val };
	CFDictionaryRef dict = CFDictionaryCreate(
		CFAllocatorGetDefault(), keys, values, 2, 
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(val);
	
	CFTypeRef settingsValues[] = { dict };
	CFArrayRef featureSettings = CFArrayCreate(CFAllocatorGetDefault(), settingsValues, 1, &kCFTypeArrayCallBacks);
	CFRelease(dict);
	
	CFTypeRef descriptorKeys[] = { kCTFontFeatureSettingsAttribute };
	CFTypeRef descriptorValues[] = { featureSettings };
	CFDictionaryRef descriptorAttrs =  CFDictionaryCreate(CFAllocatorGetDefault(), descriptorKeys, 
		descriptorValues, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	
	CTFontDescriptorRef desc2 = CTFontDescriptorCreateCopyWithAttributes(desc, descriptorAttrs);
	CFRelease(descriptorAttrs);
	CFRelease(desc);
	return desc2;	
}

static CTFontDescriptorRef
fontfeatures(char *name, CTFontDescriptorRef desc) 
{
	int i;
	CTFontDescriptorRef tmp;
	// Set up OpenType Attributes
	CFAllocatorRef defaultAllocator = CFAllocatorGetDefault();

	int numberSpacing = kNumberSpacingType;
	int numberSpacingType = kMonospacedNumbersSelector;

	CFNumberRef numberSpacingId = CFNumberCreate(defaultAllocator, kCFNumberIntType, &numberSpacing);
	CFNumberRef monospacedNumbersSelector = CFNumberCreate(defaultAllocator, kCFNumberIntType, &numberSpacingType);
	tmp = desc;
	desc = CTFontDescriptorCreateCopyWithFeature(desc, numberSpacingId, monospacedNumbersSelector);
	CFRelease(tmp);
	CFRelease(numberSpacingId);
	CFRelease(monospacedNumbersSelector);

	int features = 0;
	for(i=0; i<nelem(featuremap); i++)
		if(strstr(name, featuremap[i].name)){
			features = featuremap[i].features;
			break;
		}
	if(features & Zero)
		desc = fontfeature(desc, CFSTR("zero"), 1);
	if(features & SS01)
		desc = fontfeature(desc, CFSTR("ss01"), 1);
	if(features & SS02)
		desc = fontfeature(desc, CFSTR("ss02"), 1);
	if(features & SS03)
		desc = fontfeature(desc, CFSTR("ss03"), 1);
	if(features & SS04)
		desc = fontfeature(desc, CFSTR("ss04"), 1);
	if(features & SS05)
		desc = fontfeature(desc, CFSTR("ss05"), 1);
	if(features & SS06)
		desc = fontfeature(desc, CFSTR("ss06"), 1);
	if(features & SS07)
		desc = fontfeature(desc, CFSTR("ss07"), 1);	
	if(features & SS10)
		desc = fontfeature(desc, CFSTR("ss10"), 1);
	if(features & SS11)
		desc = fontfeature(desc, CFSTR("ss11"), 1);
	if(features & SS12)
		desc = fontfeature(desc, CFSTR("ss12"), 1);
	if(features & SS14)
		desc = fontfeature(desc, CFSTR("ss14"), 1);
	if(features & SS15)
		desc = fontfeature(desc, CFSTR("ss15"), 1);
	if(features & SS17)
		desc = fontfeature(desc, CFSTR("ss17"), 1);
	if(features & Salt)
		desc = fontfeature(desc, CFSTR("salt"), 1);
	if(features & Lnum)
		desc = fontfeature(desc, CFSTR("lnum"), 1);
	if(features & Pnum)
		desc = fontfeature(desc, CFSTR("pnum"), 1);
	return desc;
}

static void
fontheight(XFont *f, int size, int *height, int *ascent)
{
	int i;
	CFStringRef s;
	CGRect bbox;
	CTFontRef font;
	CTFontDescriptorRef desc;
	CGContextRef ctxt;
	CGColorSpaceRef color;

	s = c2mac(f->name);
	desc = CTFontDescriptorCreateWithNameAndSize(s, size);
	CFRelease(s);
	if(desc == nil)
		return;
		
	desc = fontfeatures(f->name, desc);
	font = CTFontCreateWithFontDescriptor(desc, 0, nil);
	CFRelease(desc);

	if(font == nil)
		return;

	color = CGColorSpaceCreateWithName(kCGColorSpaceGenericGray);
	ctxt = CGBitmapContextCreate(nil, 1, 1, 8, 1, color, kCGImageAlphaNone);
	CGColorSpaceRelease(color);
	CGContextSetTextPosition(ctxt, 0, 0);

	for(i=0; i<nelem(lines); i++) {
		CFStringRef keys[] = { kCTFontAttributeName };
		CFTypeRef values[] = { font };
		CFStringRef str;
		CFDictionaryRef attrs;
		CFAttributedStringRef attrString;
		CGRect r;
		CTLineRef line;

 		str = c2mac(lines[i]);
 		
 		// See https://developer.apple.com/library/ios/documentation/StringsTextFonts/Conceptual/CoreText_Programming/LayoutOperations/LayoutOperations.html#//apple_ref/doc/uid/TP40005533-CH12-SW2
 		attrs = CFDictionaryCreate(kCFAllocatorDefault, (const void**)&keys,
			(const void**)&values, sizeof(keys) / sizeof(keys[0]),
			&kCFTypeDictionaryKeyCallBacks,
			&kCFTypeDictionaryValueCallBacks);
		attrString = CFAttributedStringCreate(kCFAllocatorDefault, str, attrs);
		CFRelease(str);
		CFRelease(attrs);

		line = CTLineCreateWithAttributedString(attrString);
		r = CTLineGetImageBounds(line, ctxt);
		r.size.width += r.origin.x;
		r.size.height += r.origin.y;
		CFRelease(line);
		
//	fprint(2, "%s: %g %g %g %g\n", lines[i], r.origin.x, r.origin.y, r.size.width, r.size.height);
		
		if(i == 0)
			bbox = r;
		if(bbox.origin.x > r.origin.x)
			bbox.origin.x = r.origin.x;	
		if(bbox.origin.y > r.origin.y)
			bbox.origin.y = r.origin.y;	
		if(bbox.size.width < r.size.width)
			bbox.size.width = r.size.width;
		if(bbox.size.height < r.size.height)
			bbox.size.height = r.size.height;
	}

	bbox.size.width -= bbox.origin.x;
	bbox.size.height -= bbox.origin.y;

	*height = bbox.size.height + 0.999999;
	*ascent = *height - (-bbox.origin.y + 0.999999);
		
	CGContextRelease(ctxt);
	CFRelease(font);
}

void
load(XFont *f)
{
	int i;

	if(f->loaded)
		return;
	f->loaded = 1;

	// compute height and ascent for each size on demand
	f->loadheight = fontheight;

	// enable all Unicode ranges
	for(i=0; i<nelem(f->range); i++) {
		f->range[i] = 1;
		f->nrange++;
	}
}

Memsubfont*
mksubfont(XFont *f, char *name, int lo, int hi, int size, int antialias)
{
	CFStringRef s;
	CGColorSpaceRef color;
	CGContextRef ctxt;
	CTFontRef font;
	CTFontDescriptorRef desc;
	CGRect bbox;
	Memimage *m, *mc, *m1;
	int x, y, y0;
	int i, height, ascent;
	Fontchar *fc, *fc0;
	Memsubfont *sf;
	CGFloat whitef[] = { 1.0, 1.0 };
	CGColorRef white;

	s = c2mac(name);
	desc = CTFontDescriptorCreateWithNameAndSize(s, size);
	CFRelease(s);
	if(desc == nil)
		return nil;

	desc = fontfeatures(name, desc);
	font = CTFontCreateWithFontDescriptor(desc, 0, nil);
	CFRelease(desc);

	if(font == nil)
		return nil;
	
	
	bbox = CTFontGetBoundingBox(font);
	x = (int)(bbox.size.width*2 + 0.99999999);

	fontheight(f, size, &height, &ascent);
	y = height;
	y0 = height - ascent;

	m = allocmemimage(Rect(0, 0, x*(hi+1-lo)+1, y+1), GREY8);
	if(m == nil)
		return nil;
	mc = allocmemimage(Rect(0, 0, x+1, y+1), GREY8);
	if(mc == nil){
		freememimage(m);
		return nil;
	}
	memfillcolor(m, DBlack);
	memfillcolor(mc, DBlack);
	fc = malloc((hi+2 - lo) * sizeof fc[0]);
	sf = malloc(sizeof *sf);
	if(fc == nil || sf == nil) {
		freememimage(m);
		freememimage(mc);
		free(fc);
		free(sf);
		return nil;
	}
	fc0 = fc;

	color = CGColorSpaceCreateWithName(kCGColorSpaceGenericGray);
	ctxt = CGBitmapContextCreate(byteaddr(mc, mc->r.min), Dx(mc->r), Dy(mc->r), 8,
		mc->width*sizeof(u32int), color, kCGImageAlphaNone);
	white = CGColorCreate(color, whitef);
	CGColorSpaceRelease(color);
	if(ctxt == nil) {
		freememimage(m);
		freememimage(mc);
		free(fc);
		free(sf);
		return nil;
	}

	CGContextSetAllowsAntialiasing(ctxt, antialias);
	CGContextSetAllowsFontSmoothing(ctxt, 0);
	CGContextSetTextPosition(ctxt, 0, 0);	// XXX
#if OSX_VERSION >= 101400
	CGContextSetAllowsFontSmoothing(ctxt, false);
#endif

	x = 0;
	for(i=lo; i<=hi; i++, fc++) {
		char buf[20];
		CFStringRef str;
		CFDictionaryRef attrs;
		CFAttributedStringRef attrString;
		CTLineRef line;
		CGRect r;
		CGPoint p1;
		CFStringRef keys[] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
		CFTypeRef values[] = { font, white };

		sprint(buf, "%C", (Rune)mapUnicode(name, i));
 		str = c2mac(buf);
 		
 		// See https://developer.apple.com/library/ios/documentation/StringsTextFonts/Conceptual/CoreText_Programming/LayoutOperations/LayoutOperations.html#//apple_ref/doc/uid/TP40005533-CH12-SW2
 		attrs = CFDictionaryCreate(kCFAllocatorDefault, (const void**)&keys,
			(const void**)&values, sizeof(keys) / sizeof(keys[0]),
			&kCFTypeDictionaryKeyCallBacks,
			&kCFTypeDictionaryValueCallBacks);
		attrString = CFAttributedStringCreate(kCFAllocatorDefault, str, attrs);
		CFRelease(str);
		CFRelease(attrs);

		line = CTLineCreateWithAttributedString(attrString);
		CGContextSetTextPosition(ctxt, 0, y0);
		r = CTLineGetImageBounds(line, ctxt);
		memfillcolor(mc, DBlack);
		CTLineDraw(line, ctxt);		
		CFRelease(line);

		fc->x = x;
		fc->top = 0;
		fc->bottom = Dy(m->r);

//		fprint(2, "printed %#x: %g %g\n", mapUnicode(i), p1.x, p1.y);
		p1 = CGContextGetTextPosition(ctxt);
		if(p1.x <= 0 || mapUnicode(name, i) == 0xfffd) {
			fc->width = 0;
			fc->left = 0;
			if(i == 0) {
				drawpjw(m, fc, x, (int)(bbox.size.width + 0.99999999), y, y - y0);
				x += fc->width;
			}	
			continue;
		}

		memimagedraw(m, Rect(x, 0, x + p1.x, y), mc, ZP, memopaque, ZP, S);
		fc->width = p1.x;
		fc->left = 0;
		x += p1.x;
	}
	fc->x = x;

	// round up to 32-bit boundary
	// so that in-memory data is same
	// layout as in-file data.
	if(x == 0)
		x = 1;
	if(y == 0)
		y = 1;
	if(antialias)
		x += -x & 3;
	else
		x += -x & 31;
	m1 = allocmemimage(Rect(0, 0, x, y), antialias ? GREY8 : GREY1);
	memimagedraw(m1, m1->r, m, m->r.min, memopaque, ZP, S);
	freememimage(m);
	freememimage(mc);

	sf->name = nil;
	sf->n = hi+1 - lo;
	sf->height = Dy(m1->r);
	sf->ascent = Dy(m1->r) - y0;
	sf->info = fc0;
	sf->bits = m1;

	return sf;
}
