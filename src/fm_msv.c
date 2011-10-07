/* MSV algorithm; generic (non-SIMD) version.
 * 
 * Contents:
 *   1. MSV implementation.
 *   2. Benchmark driver.
 *   3. Unit tests.
 *   4. Test driver.
 *   5. Example.
 *   6. Copyright and license information.
 * 
 * SRE, Fri Aug 15 10:38:21 2008 [Janelia]
 * SVN $Id: generic_msv.c 3582 2011-06-26 20:09:21Z wheelert $
 */

#include "p7_config.h"

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_gumbel.h"


#include "hmmer.h"

/*****************************************************************
 * 1. MSV implementation.
 *****************************************************************/
/* hit_sorter(): qsort's pawn, below */
static int
hit_sorter(const void *a, const void *b)
{
    FM_DIAG *d1 = (FM_DIAG*)a;
    FM_DIAG *d2 = (FM_DIAG*)b;

    //overlapping diagonals will share the same value of (n - k), so sort by that ...
    if      ( (d1->n - d1->k) > (d2->n - d2->k)) return 1;
    else if ( (d1->n - d1->k) < (d2->n - d2->k)) return -1;
    else {
      // ... and if both on same diagonal, order from top to bottom on y axis
      if  (d1->k > d2->k)    return  1;
      else                   return -1;
    }

}

int
mergeSeeds(FM_DIAGLIST *seeds) {
  int i;
  int j = 0;
  FM_DIAG *diags = seeds->diags;
  FM_DIAG next = diags[0];

  int tmp;

  int curr_n       = next.n;
  int curr_k       = next.k;
  int curr_len     = next.length;
  int curr_end     = curr_n + curr_len - 1;
  int curr_diagval = next.n - next.k;

  for( i=1; i<seeds->count; i++) {

    next = diags[i];
    //overlapping diagonals will share the same value of (n + length - k)
    if (next.n - next.k == curr_diagval) {
      //overlapping diags; extend, if appropriate
      tmp = next.n + next.length - 1;
      if (tmp > curr_end) {
        curr_end = tmp;
        curr_len = curr_end - curr_n + 1;
      }
    } else {
      //doesn't overlap current diagonal, so store current...
      diags[j].n      = curr_n;
      diags[j].k      = curr_k;
      diags[j].length = curr_end - curr_n + 1;

      // ... then start up a new one
      curr_n   = next.n;
      curr_k   = next.k;
      curr_len = next.length;
      curr_end = curr_n + curr_len - 1;
      curr_diagval = next.n - next.k;

      j++;
    }
  }
  // store final entry
  diags[j].n      = curr_n;
  diags[j].k      = curr_k;
  diags[j].length = curr_end - curr_n + 1;


  seeds->count = j+1;
  return eslOK;

}

uint32_t
FM_backtrackSeed(const FM_DATA *fmf, FM_CFG *fm_cfg, int i, FM_DIAG *seed) {
  int j = i;
  int len = 0;
  int c;

  while ( j != fmf->term_loc && (j & fm_cfg->maskSA)) { //go until we hit a position in the full SA that was sampled during FM index construction
    c = getChar( fm_cfg->meta->alph_type, j, fmf->BWT);
    j = fm_getOccCount (fmf, fm_cfg, j-1, c);
    j += abs(fmf->C[c]);
    len++;
  }

  return len + (j==fmf->term_loc ? 0 : fmf->SA[ j >> fm_cfg->shiftSA ]) ; // len is how many backward steps we had to take to find a sampled SA position

}

int
FM_getPassingDiags(const FM_DATA *fmf, FM_CFG *fm_cfg,
            int k, float sc, int depth, int fm_direction,
            int model_direction, int complementarity,
            FM_INTERVAL *interval,
            FM_DIAGLIST *seeds
            )
{

  int i;
  FM_DIAG *seed;

  //iterate over the forward interval, for each entry backtrack until hitting a sampled suffix array entry
  for (i = interval->lower;  i<= interval->upper; i++) {
    seed = fm_newSeed(seeds);
//    seed->fm_direction    = fm_direction;
//    seed->model_direction = model_direction;
//    seed->complementarity = complementarity;
    seed->k      = k;
    seed->length = depth;
    seed->n      = FM_backtrackSeed(fmf, fm_cfg, i, seed);

    /*  In both fm_forward and fm_backward cases, seed->n corresponds to the start
     * of the seed in terms of the target sequence, in a forward direction. But the meaning of
     * seed->k differs: in fm_backward, k represents the model position aligned to n, while
     * in fm_forward, k holds the model position aligned to the last character of the seed.
     * So ... shift k over in the forward case.  Doing this will mean
     */
    if(fm_direction == fm_forward) // reverse the orientation of the seed
      seed->k -= depth - 1;

    //fprintf(stderr, "            %ld (%s)\n", (long)(seed->n), fm_direction == fm_forward ? "forward" : "reverse");

  }

  return eslOK;
}


int
FM_Recurse( int depth, int M, int fm_direction,
            const FM_DATA *fmf, const FM_DATA *fmb,
            FM_CFG *fm_cfg, const FM_HMMDATA *fm_hmmdata,
            int first, int last, float sc_threshFM,
            FM_DP_PAIR *dp_pairs,
            FM_INTERVAL *interval_1, FM_INTERVAL *interval_2,
            FM_DIAGLIST *seeds,
            char *seq
          )
{


  float sc, next_score;

  //int max_k;
  int c, i, k;

  FM_INTERVAL interval_1_new, interval_2_new;

  for (c=0; c< fm_cfg->meta->alph_size; c++) {//acgt
    int dppos = last;

    seq[depth-1] = fm_cfg->meta->alph[c];
    seq[depth] = '\0';


    for (i=first; i<=last; i++) { // for each surviving diagonal from the previous round

        if (dp_pairs[i].model_direction == fm_forward)
          k = dp_pairs[i].pos + 1;
        else  //fm_backward
          k = dp_pairs[i].pos - 1;

        if (dp_pairs[i].complementarity == fm_complement) {
          next_score = fm_hmmdata->scores[k][ fm_getComplement(c,fm_cfg->meta->alph_type) ];
        } else
          next_score = fm_hmmdata->scores[k][c];

        sc = dp_pairs[i].score + next_score;


        if ( sc >= sc_threshFM ) { // this is a seed I want to extend

          interval_1_new.lower = interval_1->lower;
          interval_1_new.upper = interval_1->upper;

          //fprintf(stderr, "%-18s : (%.2f)\n", seq, sc);

          if (fm_direction == fm_forward) {
            //searching for forward matches on the FM-index
            interval_2_new.lower = interval_2->lower;
            interval_2_new.upper = interval_2->upper;
            if ( interval_1_new.lower >= 0 && interval_1_new.lower <= interval_1_new.upper  )  //no use extending a non-existent string
              updateIntervalForward( fmb, fm_cfg, c, &interval_1_new, &interval_2_new);

            if ( interval_1_new.lower >= 0 && interval_1_new.lower <= interval_1_new.upper  )  //no use extending a non-existent string
              FM_getPassingDiags(fmf, fm_cfg, k, sc, depth, fm_forward,
                                 dp_pairs[i].model_direction, dp_pairs[i].complementarity,
                                 &interval_2_new, seeds);

          } else {
            //searching for reverse matches on the FM-index
            if ( interval_1_new.lower >= 0 && interval_1_new.lower <= interval_1_new.upper  )  //no use extending a non-existent string
              updateIntervalReverse( fmf, fm_cfg, c, &interval_1_new);

            if ( interval_1_new.lower >= 0 && interval_1_new.lower <= interval_1_new.upper  )  //no use extending a non-existent string
              FM_getPassingDiags(fmf, fm_cfg, k, sc, depth, fm_backward,
                                 dp_pairs[i].model_direction, dp_pairs[i].complementarity,
                                 &interval_1_new, seeds);

          }



        } else if (  sc <= 0                                                                                        //some other path in the string enumeration tree will do the job
            || depth == fm_cfg->max_depth                                                                            //can't extend anymore, 'cause we've reached the pruning length
            || (depth == dp_pairs[i].max_score_len + fm_cfg->neg_len_limit)                                        //too many consecutive positions with a negative total score contribution (sort of like Xdrop)
            || (sc/depth < fm_cfg->score_ratio_req)                                                                //score density is too low
            || (dp_pairs[i].max_consec_pos < fm_cfg->consec_pos_req  &&                                            //a seed is expected to have at least one run of positive-scoring matches at least length consec_pos_req;  if it hasn't,  (see Tue Nov 23 09:39:54 EST 2010)
                   ( (depth >= fm_cfg->max_depth/2 &&  sc/depth < sc_threshFM/fm_cfg->max_depth)                              // if we're close to the end of the sequence, abort -- if that end does have sufficiently long all-positive run, I'll find it on the reverse sweep
                   || depth == fm_cfg->max_depth-fm_cfg->consec_pos_req+1 )                                                   // if we're at least half way across the sequence, and score density is too low, abort -- if the density on the other side is high enough, I'll find it on the reverse sweep
               )
            || (dp_pairs[i].model_direction == fm_forward  &&
                   ( (k == M)                                                                                                          //can't extend anymore, 'cause we're at the end of the model, going forward
                  || (depth > (fm_cfg->max_depth - 10) &&  sc + fm_hmmdata->opt_ext_fwd[k][fm_cfg->max_depth-depth-1] < sc_threshFM)   //can't hit threshold, even with best possible forward extension up to length ssv_req
                  ))
            || (dp_pairs[i].model_direction == fm_backward &&
                   ( (k == 1)                                                                                                          //can't extend anymore, 'cause we're at the beginning of the model, going backwards
                  || (depth > (fm_cfg->max_depth - 10) &&  sc + fm_hmmdata->opt_ext_rev[k][fm_cfg->max_depth-depth-1] < sc_threshFM )  //can't hit threshold, even with best possible extension up to length ssv_req
                  ))
         )
        {

          //do nothing - it's been pruned

        } else { // it's possible to extend this diagonal and reach the threshold score

            dppos++;
            dp_pairs[dppos].pos = k;
            dp_pairs[dppos].score = sc;
            dp_pairs[dppos].model_direction   = dp_pairs[i].model_direction;
            dp_pairs[dppos].complementarity   = dp_pairs[i].complementarity;

            if (sc > dp_pairs[i].max_score) {
              dp_pairs[dppos].max_score = sc;
              dp_pairs[dppos].max_score_len = depth;
            } else {
              dp_pairs[dppos].max_score = dp_pairs[i].max_score;
              dp_pairs[dppos].max_score_len = dp_pairs[i].max_score_len;
            }

            dp_pairs[dppos].consec_pos =  (next_score > 0 ? dp_pairs[i].consec_pos + 1 : 0);
            dp_pairs[dppos].max_consec_pos = ESL_MAX( dp_pairs[dppos].consec_pos, dp_pairs[i].max_consec_pos);

        }
    }



    if ( dppos > last ){  // at least one diagonal that might reach threshold score, but hasn't yet, so extend

      interval_1_new.lower = interval_1->lower;
      interval_1_new.upper = interval_1->upper;

      if (fm_direction == fm_forward) {
        interval_2_new.lower = interval_2->lower;
        interval_2_new.upper = interval_2->upper;
        if ( interval_1_new.lower >= 0 && interval_1_new.lower <= interval_1_new.upper  )  //no use extending a non-existent string
          updateIntervalForward( fmb, fm_cfg, c, &interval_1_new, &interval_2_new);

        if (  interval_1_new.lower < 0 || interval_1_new.lower > interval_1_new.upper )  //that string doesn't exist in fwd index
            continue;

        FM_Recurse(depth+1, M, fm_direction,
                  fmf, fmb, fm_cfg, fm_hmmdata,
                  last+1, dppos, sc_threshFM, dp_pairs,
                  &interval_1_new, &interval_2_new,
                  seeds,
                  seq
                  );


      } else {

        if ( interval_1_new.lower >= 0 && interval_1_new.lower <= interval_1_new.upper  )  //no use extending a non-existent string
          updateIntervalReverse( fmf, fm_cfg, c, &interval_1_new);

        if (  interval_1_new.lower < 0 || interval_1_new.lower > interval_1_new.upper )  //that string doesn't exist in reverse index
            continue;

        FM_Recurse(depth+1, M, fm_direction,
                  fmf, fmb, fm_cfg, fm_hmmdata,
                  last+1, dppos, sc_threshFM, dp_pairs,
                  &interval_1_new, NULL,
                  seeds,
                  seq
                  );

      }
    } else {
      //printf ("internal stop: %d\n", depth);
    }

  }

  return eslOK;
}


int FM_getSeeds (const P7_OPROFILE *gm, P7_GMX *gx, float sc_threshFM,
                FM_DIAGLIST *seeds,
                const FM_DATA *fmf, const FM_DATA *fmb,
                FM_CFG *fm_cfg, const FM_HMMDATA *fm_hmmdata )
{
  FM_INTERVAL interval_f1, interval_f2, interval_bk;
  //ESL_DSQ c;
  int i, k;
  int status;
  float sc;
  char         *seq;

  FM_DP_PAIR *dp_pairs_fwd;
  FM_DP_PAIR *dp_pairs_rev;

  ESL_ALLOC(dp_pairs_fwd, gm->M * fm_cfg->max_depth * sizeof(FM_DP_PAIR)); // guaranteed to be enough to hold all diagonals
  ESL_ALLOC(dp_pairs_rev, gm->M * fm_cfg->max_depth * sizeof(FM_DP_PAIR));

  ESL_ALLOC(seq, 50*sizeof(char));


  for (i=0; i<fm_cfg->meta->alph_size; i++) {//skip '$'
    int fwd_cnt=0;
    int rev_cnt=0;

    interval_f1.lower = interval_f2.lower = interval_bk.lower = fmf->C[i];
    interval_f1.upper = interval_f2.upper = interval_bk.upper = abs(fmf->C[i+1])-1;

    if (interval_f1.lower<0 ) //none of that character found
      continue;


    //c = i - (i <= gm->abc->K ? 1 : 0);  // shift to the easel alphabet
    seq[0] = fm_cfg->meta->alph[i];//gm->abc->sym[i];
    seq[1] = '\0';

    // Fill in a DP column for the character c, (compressed so that only positive-scoring entries are kept)
    // There will be 4 DP columns for each character, (1) fwd-std, (2) fwd-complement, (3) rev-std, (4) rev-complement
    for (k = 1; k <= gm->M; k++) // there's no need to bother keeping an entry starting at the last position (gm->M)
    {

      /*
      sc = fm_hmmdata->scores[k][i];
      if (sc>0) { // we'll extend any positive-scoring diagonal
        if (k < gm->M-2) { // don't bother starting a forward diagonal so close to the end of the model
          //Forward pass on the FM-index
          dp_pairs_fwd[fwd_cnt].pos =             k;
          dp_pairs_fwd[fwd_cnt].score =           sc;
          dp_pairs_fwd[fwd_cnt].max_score =       sc;
          dp_pairs_fwd[fwd_cnt].max_score_len =   1;
          dp_pairs_fwd[fwd_cnt].consec_pos =      1;
          dp_pairs_fwd[fwd_cnt].max_consec_pos =  1;
          dp_pairs_fwd[fwd_cnt].complementarity = fm_nocomplement;
          dp_pairs_fwd[fwd_cnt].model_direction = fm_forward;
          fwd_cnt++;
        }
        if (k > 3) { // don't bother starting a reverse diagonal so close to the start of the model
          dp_pairs_rev[rev_cnt].pos =             k;
          dp_pairs_rev[rev_cnt].score =           sc;
          dp_pairs_rev[rev_cnt].max_score =       sc;
          dp_pairs_rev[rev_cnt].max_score_len =   1;
          dp_pairs_rev[rev_cnt].consec_pos =      1;
          dp_pairs_rev[rev_cnt].max_consec_pos =  1;
          dp_pairs_rev[rev_cnt].complementarity = fm_nocomplement;
          dp_pairs_rev[rev_cnt].model_direction = fm_backward;
          rev_cnt++;
        }
      }
      */


      sc = fm_hmmdata->scores[k][fm_getComplement(i, fm_cfg->meta->alph_type)];
      if (sc>0) { // we'll extend any positive-scoring diagonal

        //forward on the FM, reverse on the model
        if (k > 3) { // don't bother starting a reverse diagonal so close to the start of the model
          dp_pairs_fwd[fwd_cnt].pos =             k;
          dp_pairs_fwd[fwd_cnt].score =           sc;
          dp_pairs_fwd[fwd_cnt].max_score =       sc;
          dp_pairs_fwd[fwd_cnt].max_score_len =   1;
          dp_pairs_fwd[fwd_cnt].consec_pos =      1;
          dp_pairs_fwd[fwd_cnt].max_consec_pos =  1;
          dp_pairs_fwd[fwd_cnt].complementarity = fm_complement;
          dp_pairs_fwd[fwd_cnt].model_direction = fm_backward;
          fwd_cnt++;
        }


        //reverse on the FM, forward on the model
        if (k < gm->M-2) { // don't bother starting a forward diagonal so close to the end of the model
          dp_pairs_rev[rev_cnt].pos =             k;
          dp_pairs_rev[rev_cnt].score =           sc;
          dp_pairs_rev[rev_cnt].max_score =       sc;
          dp_pairs_rev[rev_cnt].max_score_len =   1;
          dp_pairs_rev[rev_cnt].consec_pos =      1;
          dp_pairs_rev[rev_cnt].max_consec_pos =  1;
          dp_pairs_rev[rev_cnt].complementarity = fm_complement;
          dp_pairs_rev[rev_cnt].model_direction = fm_forward;
          rev_cnt++;
        }
      }


    }

    FM_Recurse ( 2, gm->M, fm_forward,
                fmf, fmb, fm_cfg, fm_hmmdata,
                 0, fwd_cnt-1, sc_threshFM, dp_pairs_fwd,
                 &interval_f1, &interval_f2,
                 seeds
                 , seq
            );


    FM_Recurse ( 2, gm->M, fm_backward,
                fmf, fmb, fm_cfg, fm_hmmdata,
                 0, rev_cnt-1, sc_threshFM, dp_pairs_rev,
                 &interval_bk, NULL,
                 seeds
                 , seq
            );

  }

  //now sort, first by direction, then N (position on database sequence), then K (model position)
  qsort(seeds->diags, seeds->count, sizeof(FM_DIAG), hit_sorter);

  //merge duplicates
  mergeSeeds(seeds);

  return eslOK;

ERROR:
  ESL_EXCEPTION(eslEMEM, "Error allocating memory for hit list\n");

}

/* Function:  p7_FM_MSV()
 * Synopsis:  Finds windows with MSV scores above given threshold
 *
 * Details:   Uses FM-index to find high-scoring diagonals (seeds), then extends those
 *            seeds to maximal scoring diagonals (no gaps). Diagonals in close
 *            proximity (within a small window) are stitched together, and windows
 *            meeting the MSV scoring threshold (usually score s.t. p=0.02) are
 *            captured, and passed on to the Viterbi and Forward stages of the
 *            pipeline.
 *
 *
 * Args:      dsq     - digital target sequence, 1..L
 *            L       - length of dsq in residues
 *            gm      - profile (can be in any mode)
 *            gx      - DP matrix
 *            nu      - configuration: expected number of hits (use 2.0 as a default)
 *            bg      - the background model, required for translating a P-value threshold into a score threshold
 *            P       - p-value below which a region is captured as being above threshold
 *            starts  - RETURN: array of start positions for windows surrounding above-threshold areas
 *            ends    - RETURN: array of end positions for windows surrounding above-threshold areas
 *            hit_cnt - RETURN: count of entries in the above two arrays
 *
 *
 * Note:      Not worried about speed here. Based on p7_GMSV
 *
 * Returns:   <eslOK> on success.
 *
 * Throws:    <eslEINVAL> if <ox> allocation is too small.
 */
int
p7_FM_MSV( P7_OPROFILE *om, P7_GMX *gx, float nu, P7_BG *bg, double P, int **starts, int** ends, int *hit_cnt,
         const FM_DATA *fmf, const FM_DATA *fmb, FM_CFG *fm_cfg, const FM_HMMDATA *fm_hmmdata)
{

  float P_fm = 0.5;
  float nullsc, sc_thresh, sc_threshFM;
  float invP, invP_FM;

  int i;
//  float      **dp    = gx->dp;
//  float       *xmx   = gx->xmx;
  float      tloop = logf((float) om->max_length / (float) (om->max_length+3));
  float      tloop_total = tloop * om->max_length;
  float      tmove = logf(     3.0f / (float) (om->max_length+3));
  float      tbmk  = logf(     2.0f / ((float) om->M * (float) (om->M+1)));
//  float    tej   = logf((nu - 1.0f) / nu);
  float      tec   = logf(1.0f / nu);
//  int          i,k;
  int          status;
  int hit_arr_size = 50; // arbitrary size - it, and the hit lists it relates to, will be increased as necessary

  FM_DIAGLIST seeds;

  fm_initSeeds(&seeds);

  /*
   * Computing the score required to let P meet the F1 prob threshold
   * In original code, converting from an MSV score S (the score getting
   * to state C) to a probability goes like this:
   *  S = XMX(L,p7G_C)
   *  usc = S + tmove + tloop_total
   *  P = f ( (usc - nullsc) / eslCONST_LOG2 , mu, lambda)
   * and we're computing the threshold score S, so reverse it:
   *  (usc - nullsc) /  eslCONST_LOG2 = inv_f( P, mu, lambda)
   *  usc = nullsc + eslCONST_LOG2 * inv_f( P, mu, lambda)
   *  S = usc - tmove - tloop_total
   *
   *  Here, I compute threshold with length model based on max_length.  Usually, the
   *  length of a window returned by this scan will be 2*max_length-1 or longer.  Doesn't
   *  really matter - in any case, both the bg and om models will change with roughly
   *  1 bit for each doubling of the length model, so they offset.
   */
  p7_bg_SetLength(bg, om->max_length);
  p7_oprofile_ReconfigMSVLength(om, om->max_length);
  p7_bg_NullOne  (bg, NULL, om->max_length, &nullsc);

  invP = esl_gumbel_invsurv(P, om->evparam[p7_MMU],  om->evparam[p7_MLAMBDA]);
  sc_thresh =   nullsc  + (invP * eslCONST_LOG2) - tmove - tloop_total;

  invP_FM = esl_gumbel_invsurv(P_fm, om->evparam[p7_MMU],  om->evparam[p7_MLAMBDA]);
  sc_threshFM =   nullsc  + (invP_FM * eslCONST_LOG2) - tmove - tloop_total - tmove - tbmk - tec;


  /* stuff used to keep track of hits (regions passing the p-threshold)*/
  ESL_ALLOC(*starts, hit_arr_size * sizeof(int));
  ESL_ALLOC(*ends, hit_arr_size * sizeof(int));
  (*starts)[0] = (*ends)[0] = -1;
  *hit_cnt = 0;

  //get diagonals that score above sc_threshFM
  FM_getSeeds(om, gx, sc_threshFM, &seeds, fmf, fmb, fm_cfg, fm_hmmdata);


  //now extend those diagonals to find ones scoring above sc_thresh (



  int L;  //shouldn't be here
  /*
   * merge overlapping windows, compressing list in place.
   */
  if ( *hit_cnt > 0 ) {
    int merged = 0;
    do {
      merged = 0;

      int new_hit_cnt = 0;
      for (i=1; i<*hit_cnt; i++) {
        if ((*starts)[i] <= (*ends)[new_hit_cnt]) {
          //merge windows
          merged = 1;
          if ( (*ends)[i] > (*ends)[new_hit_cnt] )
            (*ends)[new_hit_cnt] = (*ends)[i];

        } else {
          //new window
          new_hit_cnt++;
          (*starts)[new_hit_cnt] = (*starts)[i];
          (*ends)[new_hit_cnt] = (*ends)[i];
        }
      }
      *hit_cnt = new_hit_cnt + 1;
    } while (merged > 0);

    if ((*starts)[0] <  1)
      (*starts)[0] =  1;

    if ((*ends)[*hit_cnt - 1] >  L)
      (*ends)[*hit_cnt - 1] =  L;

  }
  return eslEOF;

ERROR:
  ESL_EXCEPTION(eslEMEM, "Error allocating memory for hit list\n");

}
/*------------------ end, p7_FM_MSV() ------------------------*/





/*****************************************************************
 * 2. Benchmark driver.
 *****************************************************************/
#ifdef p7GENERIC_MSV_BENCHMARK
/*
   gcc -g -O2      -o generic_msv_benchmark -I. -L. -I../easel -L../easel -Dp7GENERIC_MSV_BENCHMARK generic_msv.c -lhmmer -leasel -lm
   icc -O3 -static -o generic_msv_benchmark -I. -L. -I../easel -L../easel -Dp7GENERIC_MSV_BENCHMARK generic_msv.c -lhmmer -leasel -lm
   ./benchmark-generic-msv <hmmfile>
 */
/* As of Fri Dec 28 14:48:39 2007
 *    Viterbi  = 61.8 Mc/s
 *    Forward  =  8.6 Mc/s
 *   Backward  =  7.1 Mc/s
 *       GMSV  = 55.9 Mc/s
 * (gcc -g -O2, 3.2GHz Xeon, N=50K, L=400, M=72 RRM_1 model)
 */
#include "p7_config.h"

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_getopts.h"
#include "esl_random.h"
#include "esl_randomseq.h"
#include "esl_stopwatch.h"

#include "hmmer.h"

static ESL_OPTIONS options[] = {
  /* name           type      default  env  range toggles reqs incomp  help                                       docgroup*/
  { "-h",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "show brief help on version and usage",           0 },
  { "-s",        eslARG_INT,     "42", NULL, NULL,  NULL,  NULL, NULL, "set random number seed to <n>",                  0 },
  { "-L",        eslARG_INT,    "400", NULL, "n>0", NULL,  NULL, NULL, "length of random target seqs",                   0 },
  { "-N",        eslARG_INT,  "20000", NULL, "n>0", NULL,  NULL, NULL, "number of random target seqs",                   0 },
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options] <hmmfile>";
static char banner[] = "benchmark driver for generic MSV";

int 
main(int argc, char **argv)
{
  ESL_GETOPTS    *go      = p7_CreateDefaultApp(options, 1, argc, argv, banner, usage);
  char           *hmmfile = esl_opt_GetArg(go, 1);
  ESL_STOPWATCH  *w       = esl_stopwatch_Create();
  ESL_RANDOMNESS *r       = esl_randomness_CreateFast(esl_opt_GetInteger(go, "-s"));
  ESL_ALPHABET   *abc     = NULL;
  P7_HMMFILE     *hfp     = NULL;
  P7_HMM         *hmm     = NULL;
  P7_BG          *bg      = NULL;
  P7_PROFILE     *gm      = NULL;
  P7_GMX         *gx      = NULL;
  int             L       = esl_opt_GetInteger(go, "-L");
  int             N       = esl_opt_GetInteger(go, "-N");
  ESL_DSQ        *dsq     = malloc(sizeof(ESL_DSQ) * (L+2));
  int             i;
  float           sc;
  double          base_time, bench_time, Mcs;

  if (p7_hmmfile_OpenE(hmmfile, NULL, &hfp, NULL) != eslOK) p7_Fail("Failed to open HMM file %s", hmmfile);
  if (p7_hmmfile_Read(hfp, &abc, &hmm)            != eslOK) p7_Fail("Failed to read HMM");

  bg = p7_bg_Create(abc);
  p7_bg_SetLength(bg, L);
  gm = p7_profile_Create(hmm->M, abc);
  p7_ProfileConfig(hmm, bg, gm, L, p7_UNILOCAL);
  gx = p7_gmx_Create(gm->M, L);

  /* Baseline time. */
  esl_stopwatch_Start(w);
  for (i = 0; i < N; i++) esl_rsq_xfIID(r, bg->f, abc->K, L, dsq);
  esl_stopwatch_Stop(w);
  base_time = w->user;

  /* Benchmark time. */
  esl_stopwatch_Start(w);
  for (i = 0; i < N; i++)
    {
      esl_rsq_xfIID(r, bg->f, abc->K, L, dsq);
      p7_GMSV      (dsq, L, gm, gx, 2.0, &sc);
    }
  esl_stopwatch_Stop(w);
  bench_time = w->user - base_time;
  Mcs        = (double) N * (double) L * (double) gm->M * 1e-6 / (double) bench_time;
  esl_stopwatch_Display(stdout, w, "# CPU time: ");
  printf("# M    = %d\n",   gm->M);
  printf("# %.1f Mc/s\n", Mcs);

  free(dsq);
  p7_gmx_Destroy(gx);
  p7_profile_Destroy(gm);
  p7_bg_Destroy(bg);
  p7_hmm_Destroy(hmm);
  p7_hmmfile_Close(hfp);
  esl_alphabet_Destroy(abc);
  esl_stopwatch_Destroy(w);
  esl_randomness_Destroy(r);
  esl_getopts_Destroy(go);
  return 0;
}
#endif /*p7GENERIC_MSV_BENCHMARK*/
/*----------------- end, benchmark ------------------------------*/

/*****************************************************************
 * 3. Unit tests
 *****************************************************************/
#ifdef p7GENERIC_MSV_TESTDRIVE
#include "esl_getopts.h"
#include "esl_random.h"
#include "esl_randomseq.h"
#include "esl_vectorops.h"
/* The MSV score can be validated against Viterbi (provided we trust
 * Viterbi), by creating a multihit local profile in which:
 *   1. All t_MM scores = 0
 *   2. All other core transitions = -inf
 *   3. All t_BMk entries uniformly log 2/(M(M+1))
 */
static void
utest_msv(ESL_GETOPTS *go, ESL_RANDOMNESS *r, ESL_ALPHABET *abc, P7_BG *bg, P7_PROFILE *gm, int nseq, int L)
{
  P7_PROFILE *g2 = NULL;
  ESL_DSQ   *dsq = NULL;
  P7_GMX    *gx  = NULL;
  float     sc1, sc2;
  int       k, idx;

  if ((dsq    = malloc(sizeof(ESL_DSQ) *(L+2))) == NULL)  esl_fatal("malloc failed");
  if ((gx     = p7_gmx_Create(gm->M, L))        == NULL)  esl_fatal("matrix creation failed");
  if ((g2     = p7_profile_Clone(gm))           == NULL)  esl_fatal("profile clone failed");

  /* Make g2's scores appropriate for simulating the MSV algorithm in Viterbi */
  esl_vec_FSet(g2->tsc, p7P_NTRANS * g2->M, -eslINFINITY);
  for (k = 1; k <  g2->M; k++) p7P_TSC(g2, k, p7P_MM) = 0.0f;
  for (k = 0; k <  g2->M; k++) p7P_TSC(g2, k, p7P_BM) = log(2.0f / ((float) g2->M * (float) (g2->M+1)));

  for (idx = 0; idx < nseq; idx++)
    {
      if (esl_rsq_xfIID(r, bg->f, abc->K, L, dsq) != eslOK) esl_fatal("seq generation failed");

      if (p7_GMSV    (dsq, L, gm, gx, 2.0, &sc1)       != eslOK) esl_fatal("MSV failed");
      if (p7_GViterbi(dsq, L, g2, gx,      &sc2)       != eslOK) esl_fatal("viterbi failed");
      if (fabs(sc1-sc2) > 0.0001) esl_fatal("MSV score not equal to Viterbi score");
    }

  p7_gmx_Destroy(gx);
  p7_profile_Destroy(g2);
  free(dsq);
  return;
}
#endif /*p7GENERIC_MSV_TESTDRIVE*/
/*----------------- end, unit tests -----------------------------*/


/*****************************************************************
 * 4. Test driver.
 *****************************************************************/
/* gcc -g -Wall -Dp7GENERIC_MSV_TESTDRIVE -I. -I../easel -L. -L../easel -o generic_msv_utest generic_msv.c -lhmmer -leasel -lm
 */
#ifdef p7GENERIC_MSV_TESTDRIVE
#include "easel.h"
#include "esl_getopts.h"
#include "esl_msa.h"

#include "p7_config.h"
#include "hmmer.h"

static ESL_OPTIONS options[] = {
  /* name           type      default  env  range toggles reqs incomp  help                                       docgroup*/
  { "-h",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "show brief help on version and usage",           0 },
  { "-s",        eslARG_INT,     "42", NULL, NULL,  NULL,  NULL, NULL, "set random number seed to <n>",                  0 },
  { "-v",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "be verbose",                                     0 },
  { "--vv",      eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "be very verbose",                                0 },
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options]";
static char banner[] = "unit test driver for the generic Msv implementation";

int
main(int argc, char **argv)
{
  ESL_GETOPTS    *go   = p7_CreateDefaultApp(options, 0, argc, argv, banner, usage);
  ESL_RANDOMNESS *r    = esl_randomness_CreateFast(esl_opt_GetInteger(go, "-s"));
  ESL_ALPHABET   *abc  = NULL;
  P7_HMM         *hmm  = NULL;
  P7_PROFILE     *gm   = NULL;
  P7_BG          *bg   = NULL;
  int             M    = 100;
  int             L    = 200;
  int             nseq = 20;
  char            errbuf[eslERRBUFSIZE];

  if ((abc = esl_alphabet_Create(eslAMINO))         == NULL)  esl_fatal("failed to create alphabet");
  if (p7_hmm_Sample(r, M, abc, &hmm)                != eslOK) esl_fatal("failed to sample an HMM");
  if ((bg = p7_bg_Create(abc))                      == NULL)  esl_fatal("failed to create null model");
  if ((gm = p7_profile_Create(hmm->M, abc))         == NULL)  esl_fatal("failed to create profile");
  if (p7_ProfileConfig(hmm, bg, gm, L, p7_LOCAL)    != eslOK) esl_fatal("failed to config profile");
  if (p7_hmm_Validate    (hmm, errbuf, 0.0001)      != eslOK) esl_fatal("whoops, HMM is bad!: %s", errbuf);
  if (p7_profile_Validate(gm,  errbuf, 0.0001)      != eslOK) esl_fatal("whoops, profile is bad!: %s", errbuf);

  utest_msv(go, r, abc, bg, gm, nseq, L);

  p7_profile_Destroy(gm);
  p7_bg_Destroy(bg);
  p7_hmm_Destroy(hmm);
  esl_alphabet_Destroy(abc);
  esl_randomness_Destroy(r);
  esl_getopts_Destroy(go);
  return 0;
}
#endif /*p7GENERIC_MSV_TESTDRIVE*/
/*-------------------- end, test driver -------------------------*/


/*****************************************************************
 * 5. Example
 *****************************************************************/
#ifdef p7GENERIC_MSV_EXAMPLE
/* 
   gcc -g -O2 -Dp7GENERIC_MSV_EXAMPLE -I. -I../easel -L. -L../easel -o generic_msv_example generic_msv.c -lhmmer -leasel -lm
 */
#include "p7_config.h"

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_getopts.h"
#include "esl_gumbel.h"
#include "esl_sq.h"
#include "esl_sqio.h"

#include "hmmer.h"

static ESL_OPTIONS options[] = {
  /* name           type      default  env  range toggles reqs incomp  help                                       docgroup*/
  { "-h",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "show brief help on version and usage",             0 },
  { "--nu",      eslARG_REAL,   "2.0", NULL, NULL,  NULL,  NULL, NULL, "set nu param to <x>: expected # MSV diagonals",    0 },
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options] <hmmfile> <seqfile>";
static char banner[] = "example of generic MSV algorithm";


int 
main(int argc, char **argv)
{
  ESL_GETOPTS    *go      = p7_CreateDefaultApp(options, 2, argc, argv, banner, usage);
  char           *hmmfile = esl_opt_GetArg(go, 1);
  char           *seqfile = esl_opt_GetArg(go, 2);
  float           nu      = esl_opt_GetReal(go, "--nu");
  ESL_ALPHABET   *abc     = NULL;
  P7_HMMFILE     *hfp     = NULL;
  P7_HMM         *hmm     = NULL;
  P7_BG          *bg      = NULL;
  P7_PROFILE     *gm      = NULL;
  P7_GMX         *fwd     = NULL;
  ESL_SQ         *sq      = NULL;
  ESL_SQFILE     *sqfp    = NULL;
  P7_TRACE       *tr      = NULL;
  int             format  = eslSQFILE_UNKNOWN;
  float           sc, nullsc, seqscore, lnP;
  int             status;

  /* Read in one HMM */
  if (p7_hmmfile_OpenE(hmmfile, NULL, &hfp, NULL) != eslOK) p7_Fail("Failed to open HMM file %s", hmmfile);
  if (p7_hmmfile_Read(hfp, &abc, &hmm)            != eslOK) p7_Fail("Failed to read HMM");
  p7_hmmfile_Close(hfp);
 
  /* Open sequence file */
  sq     = esl_sq_CreateDigital(abc);
  status = esl_sqfile_Open(seqfile, format, NULL, &sqfp);
  if      (status == eslENOTFOUND) p7_Fail("No such file.");
  else if (status == eslEFORMAT)   p7_Fail("Format unrecognized.");
  else if (status == eslEINVAL)    p7_Fail("Can't autodetect stdin or .gz.");
  else if (status != eslOK)        p7_Fail("Open failed, code %d.", status);

  /* Configure a profile from the HMM */
  bg = p7_bg_Create(abc);
  gm = p7_profile_Create(hmm->M, abc);
  p7_ProfileConfig(hmm, bg, gm, sq->n, p7_LOCAL);

  /* Allocate matrix */
  fwd = p7_gmx_Create(gm->M, sq->n);

  while ((status = esl_sqio_Read(sqfp, sq)) == eslOK)
    {
      p7_ReconfigLength(gm,  sq->n);
      p7_bg_SetLength(bg,    sq->n);
      p7_gmx_GrowTo(fwd, gm->M, sq->n); 

      /* Run MSV */
      p7_GMSV(sq->dsq, sq->n, gm, fwd, nu, &sc);

      /* Calculate bit score and P-value using standard null1 model*/
      p7_bg_NullOne  (bg, sq->dsq, sq->n, &nullsc);
      seqscore = (sc - nullsc) / eslCONST_LOG2;
      lnP      =  esl_gumbel_logsurv(seqscore,  gm->evparam[p7_MMU],  gm->evparam[p7_MLAMBDA]);

      /* output suitable for direct use in profmark benchmark postprocessors:
       * <Pvalue> <bitscore> <target name> <query name>
       */
      printf("%g\t%.2f\t%s\t%s\n", exp(lnP), seqscore, sq->name, hmm->name);

      esl_sq_Reuse(sq);
    }
  if      (status == eslEFORMAT) esl_fatal("Parse failed (sequence file %s):\n%s\n", sqfp->filename, esl_sqfile_GetErrorBuf(sqfp));
  else if (status != eslEOF)     esl_fatal("Unexpected error %d reading sequence file %s", status, sqfp->filename);

  /* Cleanup */
  esl_sqfile_Close(sqfp); 
  esl_sq_Destroy(sq);
  p7_trace_Destroy(tr);
  p7_gmx_Destroy(fwd);
  p7_profile_Destroy(gm);
  p7_bg_Destroy(bg);
  p7_hmm_Destroy(hmm);
  esl_alphabet_Destroy(abc);
  esl_getopts_Destroy(go);
  return 0;
}
#endif /*p7GENERIC_MSV_EXAMPLE*/
/*-------------------- end, example -----------------------------*/

/*****************************************************************
 * @LICENSE@
 *****************************************************************/
