/*  Boolector: Satisfiability Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2015-2017 Aina Niemetz.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#ifndef BTORPROPSLS_H_INCLUDED
#define BTORPROPSLS_H_INCLUDED

#include "btorslvprop.h"
#include "btorslvsls.h"

#include "btorbv.h"
#include "btorlog.h"
#include "btormodel.h"
#include "btornode.h"
#include "btortypes.h"
#include "utils/btorhashint.h"

/*------------------------------------------------------------------------*/

#define BTOR_PROPSLS_PROB_FLIP_COND_CONST_DELTA 100

/*------------------------------------------------------------------------*/

static inline void
btor_propsls_rec_conf (Btor* btor)
{
  assert (btor);
  assert (btor_opt_get (btor, BTOR_OPT_ENGINE) == BTOR_ENGINE_PROP
          || btor_opt_get (btor, BTOR_OPT_ENGINE) == BTOR_ENGINE_SLS);

  if (btor_opt_get (btor, BTOR_OPT_ENGINE) == BTOR_ENGINE_PROP)
    BTOR_PROP_SOLVER (btor)->stats.move_prop_rec_conf += 1;
  else
    BTOR_SLS_SOLVER (btor)->stats.move_prop_rec_conf += 1;
}

static inline BtorBitVector*
btor_propsls_non_rec_conf (Btor* btor,
                           BtorBitVector* bve,
                           BtorBitVector* bvexp,
                           int32_t eidx,
                           char* op)
{
  assert (btor);
  assert (btor_opt_get (btor, BTOR_OPT_ENGINE) == BTOR_ENGINE_PROP
          || btor_opt_get (btor, BTOR_OPT_ENGINE) == BTOR_ENGINE_SLS);
  assert (bve);
  assert (bvexp);
  assert (op);

  (void) bve;
  (void) bvexp;
  (void) eidx;
  (void) op;

#ifndef NDEBUG
  char* sbve   = btor_bv_to_char (btor->mm, bve);
  char* sbvexp = btor_bv_to_char (btor->mm, bvexp);
  if (eidx)
    BTORLOG (2, "prop CONFLICT: %s := %s %s x", sbvexp, sbve, op);
  else
    BTORLOG (2, "prop CONFLICT: %s := x %s %s", sbvexp, op, sbve);
  btor_mem_freestr (btor->mm, sbve);
  btor_mem_freestr (btor->mm, sbvexp);
#endif
  if (btor_opt_get (btor, BTOR_OPT_ENGINE) == BTOR_ENGINE_PROP)
    BTOR_PROP_SOLVER (btor)->stats.move_prop_non_rec_conf += 1;
  else
    BTOR_SLS_SOLVER (btor)->stats.move_prop_non_rec_conf += 1;
  return 0;
}

/*------------------------------------------------------------------------*/

void btor_propsls_update_cone (Btor* btor,
                               BtorIntHashTable* bv_model,
                               BtorIntHashTable* roots,
                               BtorIntHashTable* score,
                               BtorIntHashTable* exps,
                               bool update_roots,
                               uint64_t* stats_updates,
                               double* time_update_cone,
                               double* time_update_cone_reset,
                               double* time_update_cone_model_gen,
                               double* time_update_cone_compute_score);

uint64_t btor_propsls_select_move_prop (Btor* btor,
                                        BtorNode* root,
                                        BtorNode** input,
                                        BtorBitVector** assignment);
void btor_propsls_compute_sls_scores (Btor* btor,
                                      BtorIntHashTable* bv_model,
                                      BtorIntHashTable* fun_model,
                                      BtorIntHashTable* score);

/*------------------------------------------------------------------------*/

#ifndef NDEBUG
BtorBitVector* inv_add_bv (Btor* btor,
                           BtorNode* add_exp,
                           BtorBitVector* bvadd,
                           BtorBitVector* bve,
                           int32_t eidx);

BtorBitVector* inv_and_bv (Btor* btor,
                           BtorNode* and_exp,
                           BtorBitVector* bvand,
                           BtorBitVector* bve,
                           int32_t eidx);

BtorBitVector* inv_eq_bv (Btor* btor,
                          BtorNode* eq_exp,
                          BtorBitVector* bveq,
                          BtorBitVector* bve,
                          int32_t eidx);

BtorBitVector* inv_ult_bv (Btor* btor,
                           BtorNode* ult_exp,
                           BtorBitVector* bvult,
                           BtorBitVector* bve,
                           int32_t eidx);

BtorBitVector* inv_sll_bv (Btor* btor,
                           BtorNode* sll_exp,
                           BtorBitVector* bvsll,
                           BtorBitVector* bve,
                           int32_t eidx);

BtorBitVector* inv_srl_bv (Btor* btor,
                           BtorNode* srl_exp,
                           BtorBitVector* bvsrl,
                           BtorBitVector* bve,
                           int32_t eidx);

BtorBitVector* inv_mul_bv (Btor* btor,
                           BtorNode* mul_exp,
                           BtorBitVector* bvmul,
                           BtorBitVector* bve,
                           int32_t eidx);

BtorBitVector* inv_udiv_bv (Btor* btor,
                            BtorNode* div_exp,
                            BtorBitVector* bvdiv,
                            BtorBitVector* bve,
                            int32_t eidx);

BtorBitVector* inv_urem_bv (Btor* btor,
                            BtorNode* urem_exp,
                            BtorBitVector* bvurem,
                            BtorBitVector* bve,
                            int32_t eidx);

BtorBitVector* inv_concat_bv (Btor* btor,
                              BtorNode* conc_exp,
                              BtorBitVector* bvconc,
                              BtorBitVector* bve,
                              int32_t eidx);

BtorBitVector* inv_slice_bv (Btor* btor,
                             BtorNode* slice_exp,
                             BtorBitVector* bvslice,
                             BtorBitVector* bve);

int32_t sat_prop_solver_aux (Btor* btor);
#endif

/*------------------------------------------------------------------------*/

#endif
