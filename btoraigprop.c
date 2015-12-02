/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2015 Aina Niemetz.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "btoraigprop.h"
#include "aigprop.h"
#include "btorabort.h"
#include "btorclone.h"
#include "btorcore.h"
#include "btordbg.h"
#include "btormodel.h"
#include "btorprop.h"
#include "btorsls.h"  // for score computation
#include "utils/btorhash.h"
#include "utils/btoriter.h"

#define BTOR_AIGPROP_MAXSTEPS_CFACT 100
#define BTOR_AIGPROP_MAXSTEPS(i) \
  (BTOR_AIGPROP_MAXSTEPS_CFACT * ((i) &1u ? 1 : 1 << ((i) >> 1)))

/*------------------------------------------------------------------------*/

static void *
clone_aigprop_solver (Btor *clone, Btor *btor, BtorNodeMap *exp_map)
{
  assert (clone);
  assert (btor);
  assert (exp_map);

  BtorAIGPropSolver *slv, *res;

  if (!(slv = BTOR_AIGPROP_SOLVER (btor))) return 0;

  BTOR_NEW (clone->mm, res);
  memcpy (res, slv, sizeof (BtorAIGPropSolver));

  return res;
}

static void
delete_aigprop_solver (Btor *btor)
{
  assert (btor);

  BtorAIGPropSolver *slv;

  if (!(slv = BTOR_AIGPROP_SOLVER (btor))) return;

  BTOR_DELETE (btor->mm, slv);
}

/* Note: limits are currently unused */
static int
sat_aigprop_solver (Btor *btor, int limit0, int limit1)
{
  assert (btor);

  int j, sat_result, nmoves, max_steps;
  BtorAIGPropSolver *slv;
  BtorHashTableIterator it;
  BtorNode *root;

  (void) limit0;
  (void) limit1;

  slv = BTOR_AIGPROP_SOLVER (btor);
  assert (slv);

  nmoves = 0;

  if (btor->inconsistent) goto UNSAT;

  BTOR_MSG (btor->msg, 1, "calling SAT");

  if (btor_terminate_btor (btor))
  {
    sat_result = BTOR_UNKNOWN;
    goto DONE;
  }

  sat_result = btor_simplify (btor);
  BTOR_ABORT_BOOLECTOR (
      btor->ufs->count != 0
          || (!btor->options.beta_reduce_all.val && btor->lambdas->count != 0),
      "aigprop engine supports QF_BV only");
  btor_update_assumptions (btor);

  if (btor->inconsistent) goto UNSAT;

  if (btor_terminate_btor (btor))
  {
    sat_result = BTOR_UNKNOWN;
    goto DONE;
  }

  btor_process_unsynthesized_constraints (btor);

  if (btor->found_constraint_false)
  {
  UNSAT:
    sat_result = BTOR_UNSAT;
    goto DONE;
  }
  assert (btor->unsynthesized_constraints->count == 0);
  assert (btor_check_all_hash_tables_proxy_free_dbg (btor));
  assert (btor_check_all_hash_tables_simp_free_dbg (btor));

#ifndef NDEBUG
  btor_init_node_hash_table_iterator (&it, btor->assumptions);
  while (btor_has_next_node_hash_table_iterator (&it))
    assert (!BTOR_REAL_ADDR_NODE (btor_next_node_hash_table_iterator (&it))
                 ->simplified);
#endif

  slv->aprop = aigprop_new_aigprop (btor_get_aig_mgr_btor (btor),
                                    btor->options.seed.val);
#ifndef NBTORLOG
  slv->aprop->loglevel = btor->options.loglevel.val;
#endif

  /* collect roots AIGs */
  slv->aprop->roots =
      btor_new_ptr_hash_table (btor->mm,
                               (BtorHashPtr) btor_hash_aig_by_id,
                               (BtorCmpPtr) btor_compare_aig_by_id);
  assert (btor->unsynthesized_constraints->count == 0);
  btor_init_node_hash_table_iterator (&it, btor->synthesized_constraints);
  btor_queue_node_hash_table_iterator (&it, btor->assumptions);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    root = btor_next_node_hash_table_iterator (&it);
    assert (BTOR_REAL_ADDR_NODE (root)->av->len == 1);
    if (!btor_find_in_ptr_hash_table (slv->aprop->roots, root))
      (void) btor_insert_in_ptr_hash_table (
          slv->aprop->roots, BTOR_REAL_ADDR_NODE (root)->av->aigs[0]);
  }

  ///* Generate intial model, all inputs are initialized with false. We do
  // * not have to consider model_for_all_nodes, but let this be handled by
  // * the model generation (if enabled) after SAT has been determined. */
  // slv->api.generate_model (btor, 0, 1);

  sat_result = aigprop_sat (slv->aprop);

SAT:
  assert (sat_result == BTOR_SAT);
DONE:
  slv->stats.moves      = nmoves;
  btor->last_sat_result = sat_result;
  return sat_result;
}

static void
generate_model_aigprop_solver (Btor *btor, int model_for_all_nodes, int reset)
{
  assert (btor);

  BtorAIGPropSolver *slv;

  if (!(slv = BTOR_AIGPROP_SOLVER (btor))) return;

  if (reset)
  {
    btor_init_bv_model (btor, &btor->bv_model);
    btor_init_fun_model (btor, &btor->fun_model);
  }

  // TODO MAP BACK TO EXP LAYER
  aigprop_generate_model (BTOR_AIGPROP_SOLVER (btor)->aprop, reset);

  /* generate model for unreachable nodes */
  if (model_for_all_nodes)
    btor_generate_model (
        btor, btor->bv_model, btor->fun_model, model_for_all_nodes);
}

static void
print_stats_aigprop_solver (Btor *btor)
{
  assert (btor);
  BtorAIGPropSolver *slv;

  if (!(slv = BTOR_AIGPROP_SOLVER (btor))) return;

  BTOR_MSG (btor->msg, 1, "");
  BTOR_MSG (btor->msg, 1, "moves: %d", slv->stats.moves);
  BTOR_MSG (btor->msg, 1, "restarts: %d", slv->stats.restarts);
}

static void
print_time_stats_aigprop_solver (Btor *btor)
{
  assert (btor);
  (void) btor;
}

BtorSolver *
btor_new_aigprop_solver (Btor *btor)
{
  assert (btor);

  BtorAIGPropSolver *slv;

  BTOR_CNEW (btor->mm, slv);

  slv->kind                 = BTOR_AIGPROP_SOLVER_KIND;
  slv->api.clone            = clone_aigprop_solver;
  slv->api.delet            = delete_aigprop_solver;
  slv->api.sat              = sat_aigprop_solver;
  slv->api.generate_model   = generate_model_aigprop_solver;
  slv->api.print_stats      = print_stats_aigprop_solver;
  slv->api.print_time_stats = print_time_stats_aigprop_solver;

  BTOR_MSG (btor->msg, 1, "enabled aigprop engine");

  return (BtorSolver *) slv;
}
