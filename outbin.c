/* outbin.c	output routines for the Netwide Assembler to produce
 *		flat-form binary files
 *
 * The Netwide Assembler is copyright (C) 1996 Simon Tatham and
 * Julian Hall. All rights reserved. The software is
 * redistributable under the licence given in the file "Licence"
 * distributed in the NASM archive.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nasm.h"
#include "nasmlib.h"
#include "outform.h"

#ifdef OF_BIN

static FILE *fp;
static efunc error;

static struct Section {
    struct SAA *contents;
    long length;
    long index;
} textsect, datasect;
static long bsslen, bssindex;

static struct Reloc {
    struct Reloc *next;
    long posn;
    long bytes;
    long secref;
    long secrel;
    struct Section *target;
} *relocs, **reloctail;

static int start_point;

static void add_reloc (struct Section *s, long bytes, long secref,
		       long secrel) {
    struct Reloc *r;

    r = *reloctail = nasm_malloc(sizeof(struct Reloc));
    reloctail = &r->next;
    r->next = NULL;
    r->posn = s->length;
    r->bytes = bytes;
    r->secref = secref;
    r->secrel = secrel;
    r->target = s;
}

static void bin_init (FILE *afp, efunc errfunc, ldfunc ldef) {
    fp = afp;

    error = errfunc;
    (void) ldef;		       /* placate optimisers */

    start_point = 0;		       /* default */
    textsect.contents = saa_init(1L);
    datasect.contents = saa_init(1L);
    textsect.length = datasect.length = 0;
    textsect.index = seg_alloc();
    datasect.index = seg_alloc();
    bsslen = 0;
    bssindex = seg_alloc();
    relocs = NULL;
    reloctail = &relocs;
}

static void bin_cleanup (void) {
    struct Reloc *r;
    long datapos, dataalign, bsspos;

    datapos = (start_point + textsect.length + 3) & ~3;/* align on 4 bytes */
    dataalign = datapos - (start_point + textsect.length);

    saa_rewind (textsect.contents);
    saa_rewind (datasect.contents);

    bsspos = (datapos + datasect.length + 3) & ~3;

    for (r = relocs; r; r = r->next) {
	unsigned char *p, *q, mydata[4];
	long l;

	saa_fread (r->target->contents, r->posn, mydata, r->bytes);
	p = q = mydata;
	l = *p++;
	l += ((long)*p++) << 8;
	if (r->bytes == 4) {
	    l += ((long)*p++) << 16;
	    l += ((long)*p++) << 24;
	}

	if (r->secref == textsect.index)
	    l += start_point;
	else if (r->secref == datasect.index)
	    l += datapos;
	else if (r->secref == bssindex)
	    l += bsspos;

	if (r->secrel == textsect.index)
	    l -= start_point;
	else if (r->secrel == datasect.index)
	    l -= datapos;
	else if (r->secrel == bssindex)
	    l -= bsspos;

	if (r->bytes == 4)
	    WRITELONG(q, l);
	else
	    WRITESHORT(q, l);
	saa_fwrite (r->target->contents, r->posn, mydata, r->bytes);
    }
    saa_fpwrite (textsect.contents, fp);
    if (datasect.length > 0) {
	fwrite ("\0\0\0\0", dataalign, 1, fp);
	saa_fpwrite (datasect.contents, fp);
    }
    fclose (fp);
    saa_free (textsect.contents);
    saa_free (datasect.contents);
    while (relocs) {
	r = relocs->next;
	nasm_free (relocs);
	relocs = r;
    }
}

static void bin_out (long segto, void *data, unsigned long type,
		     long segment, long wrt) {
    unsigned char *p, mydata[4];
    struct Section *s;
    long realbytes;

    if (wrt != NO_SEG) {
	wrt = NO_SEG;		       /* continue to do _something_ */
	error (ERR_NONFATAL, "WRT not supported by binary output format");
    }

    /*
     * handle absolute-assembly (structure definitions)
     */
    if (segto == NO_SEG) {
	if ((type & OUT_TYPMASK) != OUT_RESERVE)
	    error (ERR_NONFATAL, "attempt to assemble code in [ABSOLUTE]"
		   " space");
	return;
    }

    if (segto == bssindex) {	       /* BSS */
	if ((type & OUT_TYPMASK) != OUT_RESERVE)
	    error(ERR_WARNING, "attempt to initialise memory in the"
		  " BSS section: ignored");
	s = NULL;
    } else if (segto == textsect.index) {
	s = &textsect;
    } else if (segto == datasect.index) {
	s = &datasect;
    } else {
	error(ERR_WARNING, "attempt to assemble code in"
	      " segment %d: defaulting to `.text'", segto);
	s = &textsect;
    }

    if ((type & OUT_TYPMASK) == OUT_ADDRESS) {
	if (segment != NO_SEG &&
	    segment != textsect.index &&
	    segment != datasect.index &&
	    segment != bssindex) {
	    if (segment % 2)
		error(ERR_NONFATAL, "binary output format does not support"
		      " segment base references");
	    else
		error(ERR_NONFATAL, "binary output format does not support"
		      " external references");
	    segment = NO_SEG;
	}
	if (s) {
	    if (segment != NO_SEG)
		add_reloc (s, type & OUT_SIZMASK, segment, -1L);
	    p = mydata;
	    if ((type & OUT_SIZMASK) == 4)
		WRITELONG (p, *(long *)data);
	    else
		WRITESHORT (p, *(long *)data);
	    saa_wbytes (s->contents, mydata, type & OUT_SIZMASK);
	    s->length += type & OUT_SIZMASK;
	} else
	    bsslen += type & OUT_SIZMASK;
    } else if ((type & OUT_TYPMASK) == OUT_RAWDATA) {
	type &= OUT_SIZMASK;
	p = data;
	if (s) {
	    saa_wbytes (s->contents, data, type);
	    s->length += type;
	} else
	    bsslen += type;
    } else if ((type & OUT_TYPMASK) == OUT_RESERVE) {
	if (s) {
	    error(ERR_WARNING, "uninitialised space declared in"
		  " %s section: zeroing",
		  (segto == textsect.index ? "code" : "data"));
	}
	type &= OUT_SIZMASK;
	if (s) {
	    saa_wbytes (s->contents, NULL, type);
	    s->length += type;
	} else
	    bsslen += type;
    } else if ((type & OUT_TYPMASK) == OUT_REL2ADR ||
	       (type & OUT_TYPMASK) == OUT_REL4ADR) {
	realbytes = ((type & OUT_TYPMASK) == OUT_REL4ADR ? 4 : 2);
	if (segment != NO_SEG &&
	    segment != textsect.index &&
	    segment != datasect.index &&
	    segment != bssindex) {
	    if (segment % 2)
		error(ERR_NONFATAL, "binary output format does not support"
		      " segment base references");
	    else
		error(ERR_NONFATAL, "binary output format does not support"
		      " external references");
	    segment = NO_SEG;
	}
	if (s) {
	    add_reloc (s, realbytes, segment, segto);
	    p = mydata;
	    if (realbytes == 4)
		WRITELONG (p, *(long*)data - realbytes - s->length);
	    else
		WRITESHORT (p, *(long*)data - realbytes - s->length);
	    saa_wbytes (s->contents, mydata, realbytes);
	    s->length += realbytes;
	} else
	    bsslen += realbytes;
    }
}

static void bin_deflabel (char *name, long segment, long offset,
			  int is_global) {
    if (is_global == 2) {
	error (ERR_NONFATAL, "binary output format does not support common"
	       " variables");
    }
}

static long bin_secname (char *name, int pass, int *bits) {
    /*
     * Default is 16 bits.
     */
    if (!name)
	*bits = 16;

    if (!name)
	return textsect.index;

    if (!strcmp(name, ".text"))
	return textsect.index;
    else if (!strcmp(name, ".data"))
	return datasect.index;
    else if (!strcmp(name, ".bss"))
	return bssindex;
    else
	return NO_SEG;
}

static long bin_segbase (long segment) {
    return segment;
}

static int bin_directive (char *directive, char *value, int pass) {
    int rn_error;

    if (!strcmp(directive, "org")) {
	start_point = readnum (value, &rn_error);
	if (rn_error)
	    error (ERR_NONFATAL, "argument to ORG should be numeric");
	return 1;
    } else
	return 0;
}

static void bin_filename (char *inname, char *outname, efunc error) {
    standard_extension (inname, outname, "", error);
}

struct ofmt of_bin = {
    "flat-form binary files (e.g. DOS .COM, .SYS)",
    "bin",
    bin_init,
    bin_out,
    bin_deflabel,
    bin_secname,
    bin_segbase,
    bin_directive,
    bin_filename,
    bin_cleanup
};

#endif /* OF_BIN */
