/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2013-2015 Mathias Preiner.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "simplifier/btorelimapplies.h"
#include "btorbeta.h"
#include "btorcore.h"
#include "btordbg.h"
#include "utils/btoriter.h"
#include "utils/btorutil.h"

void
btor_eliminate_applies (Btor *btor)
{
  assert (btor);

  int num_applies, num_applies_total = 0, round;
  double start, delta;
  BtorNode *app, *fun, *subst;
  BtorNodeIterator it;
  BtorHashTableIterator h_it;
  BtorPtrHashTable *cache;

  if (btor->lambdas->count == 0) return;

  start = btor_time_stamp ();
  round = 1;
  cache = btor_new_ptr_hash_table (btor->mm,
                                   (BtorHashPtr) btor_hash_exp_pair,
                                   (BtorCmpPtr) btor_compare_exp_pair);

  /* NOTE: in some cases substitute_and_rebuild creates applies that can be
   * beta-reduced. this can happen when parameterized applies become not
   * parameterized. hence, we beta-reduce applies until fix point.
   */
  do
  {
    num_applies = 0;
    btor_init_substitutions (btor);

    /* collect function applications */
    btor_init_node_hash_table_iterator (&h_it, btor->lambdas);
    while (btor_has_next_node_hash_table_iterator (&h_it))
    {
      fun = btor_next_node_hash_table_iterator (&h_it);

      btor_init_apply_parent_iterator (&it, fun);
      while (btor_has_next_apply_parent_iterator (&it))
      {
        app = btor_next_apply_parent_iterator (&it);

        if (app->parameterized) continue;

        num_applies++;
        subst = btor_beta_reduce_full_cached (btor, app, cache);
        assert (!btor_get_ptr_hash_table (btor->substitutions, app));
        btor_insert_substitution (btor, app, subst, 0);
        btor_release_exp (btor, subst);
      }
    }

    num_applies_total += num_applies;
    BTOR_MSG (btor->msg,
              1,
              "eliminate %d applications in round %d",
              num_applies,
              round);

    btor_substitute_and_rebuild (btor, btor->substitutions);
    btor_delete_substitutions (btor);
    round++;
  } while (num_applies > 0);

#ifndef NDEBUG
  btor_init_node_hash_table_iterator (&h_it, btor->lambdas);
  while (btor_has_next_node_hash_table_iterator (&h_it))
  {
    fun = btor_next_node_hash_table_iterator (&h_it);

    btor_init_apply_parent_iterator (&it, fun);
    while (btor_has_next_apply_parent_iterator (&it))
    {
      app = btor_next_apply_parent_iterator (&it);
      assert (app->parameterized);
    }
  }
#endif

  btor_init_hash_table_iterator (&h_it, cache);
  while (btor_has_next_hash_table_iterator (&h_it))
  {
    btor_release_exp (btor, h_it.bucket->data.as_ptr);
    btor_delete_exp_pair (btor, btor_next_hash_table_iterator (&h_it));
  }
  btor_delete_ptr_hash_table (cache);

  delta = btor_time_stamp () - start;
  btor->time.elimapplies += delta;
  BTOR_MSG (btor->msg,
            1,
            "eliminated %d function applications in %.1f seconds",
            num_applies_total,
            delta);
  assert (btor_check_all_hash_tables_proxy_free_dbg (btor));
  assert (btor_check_all_hash_tables_simp_free_dbg (btor));
  assert (btor_check_unique_table_children_proxy_free_dbg (btor));
}