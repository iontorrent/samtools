/* This program demonstrates how to generate pileup from multiple BAMs
 * simutaneously, to achieve random access and to use the BED interface.
 * To compile this program separately, you may:
 *
 *   gcc -g -O2 -Wall -o bam2depth -D_MAIN_BAM2DEPTH bam2depth.c -L. -lbam -lz
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "bam.h"

typedef struct {     // auxiliary data structure
	bamFile fp;      // the file handler
	bam_iter_t iter; // NULL if a region not specified
	int min_mapQ, min_len,flag_on,flag_off; // mapQ filter; length filter;filtering flags
} aux_t;

void *bed_read(const char *fn); // read a BED or position list file
void bed_destroy(void *_h);     // destroy the BED data structure
int bed_overlap(const void *_h, const char *chr, int beg, int end); // test if chr:beg-end overlaps

// This function reads a BAM alignment from one BAM file.
static int read_bam(void *data, bam1_t *b) // read level filters better go here to avoid pileup
{
	aux_t *aux = (aux_t*)data; // data in fact is a pointer to an auxiliary structure
	int ret = aux->iter? bam_iter_read(aux->fp, aux->iter, b) : bam_read1(aux->fp, b);
	if (!(b->core.flag&BAM_FUNMAP)) {
		if (((int)b->core.qual < aux->min_mapQ)  || (b->core.flag & aux->flag_on != aux->flag_on) || (b->core.flag & aux->flag_off)) b->core.flag |= BAM_FUNMAP;
		else if (aux->min_len && bam_cigar2qlen(&b->core, bam1_cigar(b)) < aux->min_len) b->core.flag |= BAM_FUNMAP;
	}
	return ret;
}

typedef struct {
    int32_t bin;
    int32_t bin_idx;
    int32_t tid;
    int32_t bin_size;
} circos_t;

static void circos_print(circos_t *circos, bam_header_t *h)
{
  if (circos->tid < 0 || 0 == circos->bin) return;
  // NB: this could be faster with custom routines
  fputs(h->target_name[circos->tid], stdout); 
  printf("\t%d\t%d\t%f\n", 
         (circos->bin_idx * circos->bin_size),
         (circos->bin_idx + 1) * circos->bin_size - 1,
         circos->bin / (double)circos->bin_size);
}

#ifdef _MAIN_BAM2DEPTH
int main(int argc, char *argv[])
#else
int main_depth(int argc, char *argv[])
#endif
{
	int i, n, tid, beg, end, pos, *n_plp, mask, baseQ = 0, mapQ = 0, min_len = 0, use_circos = 0, max_depth = -1,flag_on = 0,flag_off = 1796;
	const bam_pileup1_t **plp;
	char *reg = 0; // specified region
	void *bed = 0; // BED data structure
	bam_header_t *h = 0; // BAM header of the 1st input
	aux_t **data;
	bam_mplp_t mplp;
        circos_t circos; circos.bin_size = 10000;

	// parse the command line
	while ((n = getopt(argc, argv, "r:b:q:Q:l:cB:m:f:F:")) >= 0) {
		switch (n) {
			case 'l': min_len = atoi(optarg); break; // minimum query length
			case 'r': reg = strdup(optarg); break;   // parsing a region requires a BAM header
			case 'b': bed = bed_read(optarg); break; // BED or position list file can be parsed now
			case 'q': baseQ = atoi(optarg); break;   // base quality threshold
			case 'Q': mapQ = atoi(optarg); break;    // mapping quality threshold
			case 'c': use_circos = 1; break; // circos output
                        case 'm': max_depth = atoi(optarg); break; // max depth
                        case 'B': circos.bin_size = atoi(optarg); break; // circos bin size
                        case 'f': flag_on = strtol(optarg,0,0); break; // flag to include reads in calculating depth
                        case 'F': flag_off = strtol(optarg,0,0); break; // flag to exclude reads in calculating depth
		}
	}
	if (optind == argc) {
		fprintf(stderr, "Usage: depth [-r reg] [-q baseQthres] [-Q mapQthres] [-l minQLen] [-b in.bed] [-c [-B binSize]] [-f include_flag] [-F exclude_flag] <in1.bam> [...]\n");
		fprintf(stderr, "Notes: \n\
\n\
By default the depth command excludes reads that are duplicates, failed platform QC, secondary mapping and unmapped reads\n\
This can be reset using the -F flag. The -f and -F flags can be used to include/exclude reads as\n\
necessary. e.g. depth -f 0x10 in.bam will generate coverage on the reverse strand. The default maximum coverage depth is \n\
set to 1,000,000. This can be changed using the -m flag. The default setting using mpileup is 8000. \n\
\n");
		return 1;
	}

	// initialize the auxiliary data structures
        if (use_circos) circos.bin = circos.bin_idx = 0; circos.tid = -1;
	n = argc - optind; // the number of BAMs on the command line
	data = calloc(n, sizeof(void*)); // data[i] for the i-th input
	beg = 0; end = 1<<30; tid = -1;  // set the default region
	for (i = 0; i < n; ++i) {
		bam_header_t *htmp;
		data[i] = calloc(1, sizeof(aux_t));
		data[i]->fp = strcmp(argv[optind+i],"-") == 0? bam_dopen(fileno(stdin),"r") :bam_open(argv[optind+i], "r"); // open BAM
		data[i]->min_mapQ = mapQ;                    // set the mapQ filter
		data[i]->min_len  = min_len;                 // set the qlen filter
		data[i]->flag_on = flag_on;                 // set the reads to include
		data[i]->flag_off = flag_off;                 // set the reads to exclude
		htmp = bam_header_read(data[i]->fp);         // read the BAM header
		if (i == 0) {
			h = htmp; // keep the header of the 1st BAM
			if (reg) bam_parse_region(h, reg, &tid, &beg, &end); // also parse the region
		} else bam_header_destroy(htmp); // if not the 1st BAM, trash the header
		if (tid >= 0) { // if a region is specified and parsed successfully
			bam_index_t *idx = bam_index_load(argv[optind+i]);  // load the index
			data[i]->iter = bam_iter_query(idx, tid, beg, end); // set the iterator
			bam_index_destroy(idx); // the index is not needed any more; phase out of the memory
		}
	}

	// the core multi-pileup loop
	mplp = bam_mplp_init(n, read_bam, (void**)data); // initialization
        if(0 < max_depth) bam_mplp_set_maxcnt(mplp, max_depth); // set the maximum depth
        else bam_mplp_set_maxcnt(mplp,1000000); // set default maximim depth to 1M instead of 8000 in bam_mplp_init
        mask = flag_off; // Default mask = (BAM_FUNMAP|BAM_FSECONDARY|BAM_FQCFAIL|BAM_FDUP)
        bam_mplp_set_mask(mplp,mask); // set mask for pileup 
	n_plp = calloc(n, sizeof(int)); // n_plp[i] is the number of covering reads from the i-th BAM
	plp = calloc(n, sizeof(void*)); // plp[i] points to the array of covering reads (internal in mplp)
	while (bam_mplp_auto(mplp, &tid, &pos, n_plp, plp) > 0) { // come to the next covered position
                int32_t cov = 0;
		if (pos < beg || pos >= end) continue; // out of range; skip
		if (bed && bed_overlap(bed, h->target_name[tid], pos, pos + 1) == 0) continue; // not in BED; skip
                if (0 == use_circos) { fputs(h->target_name[tid], stdout); printf("\t%d", pos+1); } // a customized printf() would be faster
                for (i = 0; i < n; ++i) { // base level filters have to go here
                    int j, m = 0;
                    for (j = 0; j < n_plp[i]; ++j) {
                        const bam_pileup1_t *p = plp[i] + j; // DON'T modfity plp[][] unless you really know
                        if (p->is_del || p->is_refskip) ++m; // having dels or refskips at tid:pos
                        else if (bam1_qual(p->b)[p->qpos] < baseQ) ++m; // low base quality
                    }
                    if (0 == use_circos) printf("\t%d", n_plp[i] - m); // this the depth to output
                    else cov += (n_plp[i] - m);
                }
                if (0 == use_circos) putchar('\n');
                else {
                    pos++; // make one-based
                    int32_t bin_idx = ((pos - (pos % circos.bin_size)) / circos.bin_size);
                    if (tid == circos.tid && bin_idx == circos.bin_idx) {
                        circos.bin += cov; // this is the depth to output
                    }
                    else {
                        circos_print(&circos, h); // print
                        // update
                        circos.bin = cov; // this is the depth to output
                        circos.bin_idx = bin_idx;
                        circos.tid = tid;
                    }
                }
	}
	free(n_plp); free(plp);
	bam_mplp_destroy(mplp);
        if (1 == use_circos) circos_print(&circos, h); // print

	bam_header_destroy(h);
	for (i = 0; i < n; ++i) {
		bam_close(data[i]->fp);
		if (data[i]->iter) bam_iter_destroy(data[i]->iter);
		free(data[i]);
	}
	free(data); free(reg);
	if (bed) bed_destroy(bed);
	return 0;
}
