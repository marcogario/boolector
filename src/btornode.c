/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2007-2009 Robert Daniel Brummayer.
 *  Copyright (C) 2007-2015 Armin Biere.
 *  Copyright (C) 2012-2017 Aina Niemetz.
 *  Copyright (C) 2012-2017 Mathias Preiner.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "btornode.h"

#include "btorabort.h"
#include "btoraig.h"
#include "btoraigvec.h"
#include "btorbeta.h"
#include "btorlog.h"
#include "btorrewrite.h"
#include "utils/btorhashint.h"
#include "utils/btorhashptr.h"
#include "utils/btornodeiter.h"
#include "utils/btorutil.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*------------------------------------------------------------------------*/

#define BTOR_UNIQUE_TABLE_LIMIT 30

#define BTOR_FULL_UNIQUE_TABLE(table)   \
  ((table).num_elements >= (table).size \
   && btor_util_log_2 ((table).size) < BTOR_UNIQUE_TABLE_LIMIT)

/*------------------------------------------------------------------------*/

const char *const g_btor_op2str[BTOR_NUM_OPS_NODE] = {
    [BTOR_INVALID_NODE] = "invalid", [BTOR_BV_CONST_NODE] = "const",
    [BTOR_BV_VAR_NODE] = "var",      [BTOR_PARAM_NODE] = "param",
    [BTOR_SLICE_NODE] = "slice",     [BTOR_AND_NODE] = "and",
    [BTOR_BV_EQ_NODE] = "beq",       [BTOR_FUN_EQ_NODE] = "feq",
    [BTOR_ADD_NODE] = "add",         [BTOR_MUL_NODE] = "mul",
    [BTOR_ULT_NODE] = "ult",         [BTOR_SLL_NODE] = "sll",
    [BTOR_SRL_NODE] = "srl",         [BTOR_UDIV_NODE] = "udiv",
    [BTOR_UREM_NODE] = "urem",       [BTOR_CONCAT_NODE] = "concat",
    [BTOR_APPLY_NODE] = "apply",     [BTOR_LAMBDA_NODE] = "lambda",
    [BTOR_COND_NODE] = "cond",       [BTOR_ARGS_NODE] = "args",
    [BTOR_UF_NODE] = "uf",           [BTOR_PROXY_NODE] = "proxy",
};

/*------------------------------------------------------------------------*/

static unsigned hash_primes[] = {333444569u, 76891121u, 456790003u};

#define NPRIMES ((uint32_t) (sizeof hash_primes / sizeof *hash_primes))

/*------------------------------------------------------------------------*/

/* do not move these two functions to the header (circular dependency) */

bool
btor_is_bv_cond_node (const BtorNode *exp)
{
  return btor_is_cond_node (exp)
         && btor_sort_is_bitvec (BTOR_REAL_ADDR_NODE (exp)->btor,
                                 btor_exp_get_sort_id (exp));
}

bool
btor_is_fun_cond_node (const BtorNode *exp)
{
  return btor_is_cond_node (exp)
         && btor_sort_is_fun (BTOR_REAL_ADDR_NODE (exp)->btor,
                              btor_exp_get_sort_id (exp));
}

/*------------------------------------------------------------------------*/

#ifndef NDEBUG
static bool
is_valid_kind (BtorNodeKind kind)
{
  return BTOR_INVALID_NODE <= kind && kind < BTOR_NUM_OPS_NODE;
}
#endif

static void
set_kind (Btor *btor, BtorNode *exp, BtorNodeKind kind)
{
  assert (is_valid_kind (kind));
  assert (is_valid_kind (exp->kind));

  assert (!BTOR_INVALID_NODE);

  if (exp->kind)
  {
    assert (btor->ops[exp->kind].cur > 0);
    btor->ops[exp->kind].cur--;
  }

  if (kind)
  {
    btor->ops[kind].cur++;
    assert (btor->ops[kind].cur > 0);
    if (btor->ops[kind].cur > btor->ops[kind].max)
      btor->ops[kind].max = btor->ops[kind].cur;
  }

  exp->kind = kind;
}

/*------------------------------------------------------------------------*/

static void
inc_exp_ref_counter (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);

  BtorNode *real_exp;

  (void) btor;
  real_exp = BTOR_REAL_ADDR_NODE (exp);
  BTOR_ABORT (real_exp->refs == INT_MAX, "Node reference counter overflow");
  real_exp->refs++;
}

void
btor_inc_exp_ext_ref_counter (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);

  BtorNode *real_exp = BTOR_REAL_ADDR_NODE (exp);
  BTOR_ABORT (real_exp->ext_refs == INT_MAX, "Node reference counter overflow");
  real_exp->ext_refs += 1;
  btor->external_refs += 1;
}

void
btor_dec_exp_ext_ref_counter (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);

  BTOR_REAL_ADDR_NODE (exp)->ext_refs -= 1;
  btor->external_refs -= 1;
}

BtorNode *
btor_copy_exp (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  inc_exp_ref_counter (btor, exp);
  return exp;
}

/*------------------------------------------------------------------------*/

static inline uint32_t
hash_slice_exp (BtorNode *e, uint32_t upper, uint32_t lower)
{
  uint32_t hash;
  assert (upper >= lower);
  hash = hash_primes[0] * (uint32_t) BTOR_REAL_ADDR_NODE (e)->id;
  hash += hash_primes[1] * (uint32_t) upper;
  hash += hash_primes[2] * (uint32_t) lower;
  return hash;
}

static inline uint32_t
hash_bv_exp (Btor *btor, BtorNodeKind kind, uint32_t arity, BtorNode *e[])
{
  uint32_t hash = 0;
  uint32_t i;
#ifndef NDEBUG
  if (btor_opt_get (btor, BTOR_OPT_SORT_EXP) > 0
      && btor_is_binary_commutative_node_kind (kind))
    assert (arity == 2), assert (BTOR_REAL_ADDR_NODE (e[0])->id
                                 <= BTOR_REAL_ADDR_NODE (e[1])->id);
#else
  (void) btor;
  (void) kind;
#endif
  assert (arity <= NPRIMES);
  for (i = 0; i < arity; i++)
    hash += hash_primes[i] * (uint32_t) BTOR_REAL_ADDR_NODE (e[i])->id;
  return hash;
}

static uint32_t
hash_lambda_exp (Btor *btor,
                 BtorNode *param,
                 BtorNode *body,
                 BtorIntHashTable *params)
{
  assert (btor);
  assert (param);
  assert (body);
  assert (BTOR_IS_REGULAR_NODE (param));
  assert (btor_is_param_node (param));

  uint32_t i, hash = 0;
  BtorNode *cur, *real_cur;
  BtorNodePtrStack visit;
  BtorIntHashTable *marked;

  marked = btor_hashint_table_new (btor->mm);
  BTOR_INIT_STACK (btor->mm, visit);
  BTOR_PUSH_STACK (visit, (BtorNode *) body);

  while (!BTOR_EMPTY_STACK (visit))
  {
    cur      = BTOR_POP_STACK (visit);
    real_cur = BTOR_REAL_ADDR_NODE (cur);

    if (btor_hashint_table_contains (marked, real_cur->id)) continue;

    if (!real_cur->parameterized)
    {
      hash += btor_exp_get_id (cur);
      continue;
    }

    /* paramterized lambda already hashed, we can use already computed hash
     * value instead of recomputing it */
    if (btor_is_lambda_node (real_cur))
    {
      hash += btor_hashptr_table_get (btor->lambdas, real_cur)->data.as_int;
      hash += real_cur->kind;
      hash += real_cur->e[0]->kind;
      continue;
    }
    else if (btor_is_param_node (real_cur) && real_cur != param && params)
      btor_hashint_table_add (params, real_cur->id);

    btor_hashint_table_add (marked, real_cur->id);
    hash += BTOR_IS_INVERTED_NODE (cur) ? -real_cur->kind : real_cur->kind;
    for (i = 0; i < real_cur->arity; i++)
      BTOR_PUSH_STACK (visit, real_cur->e[i]);
  }
  BTOR_RELEASE_STACK (visit);
  btor_hashint_table_delete (marked);
  return hash;
}

/* Computes hash value of expresssion by children ids */
static uint32_t
compute_hash_exp (Btor *btor, BtorNode *exp, uint32_t table_size)
{
  assert (exp);
  assert (table_size > 0);
  assert (btor_util_is_power_of_2 (table_size));
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (!btor_is_bv_var_node (exp));
  assert (!btor_is_uf_node (exp));

  uint32_t hash = 0;

  if (btor_is_bv_const_node (exp))
    hash = btor_bv_hash (btor_const_get_bits (exp));
  /* hash for lambdas is computed once during creation. afterwards, we always
   * have to use the saved hash value since hashing of lambdas requires all
   * parameterized nodes and their inputs (cf. hash_lambda_exp), which may
   * change at some point. */
  else if (btor_is_lambda_node (exp))
    hash = btor_hashptr_table_get (exp->btor->lambdas, (BtorNode *) exp)
               ->data.as_int;
  else if (exp->kind == BTOR_SLICE_NODE)
    hash = hash_slice_exp (
        exp->e[0], btor_slice_get_upper (exp), btor_slice_get_lower (exp));
  else
    hash = hash_bv_exp (btor, exp->kind, exp->arity, exp->e);
  hash &= table_size - 1;
  return hash;
}

/*------------------------------------------------------------------------*/

static void
setup_node_and_add_to_id_table (Btor *btor, void *ptr)
{
  assert (btor);
  assert (ptr);

  BtorNode *exp;
  uint32_t id;

  exp = (BtorNode *) ptr;
  assert (!BTOR_IS_INVERTED_NODE (exp));
  assert (!exp->id);

  exp->refs = 1;
  exp->btor = btor;
  btor->stats.expressions++;
  id = BTOR_COUNT_STACK (btor->nodes_id_table);
  BTOR_ABORT (id == INT_MAX, "expression id overflow");
  exp->id = id;
  BTOR_PUSH_STACK (btor->nodes_id_table, exp);
  assert (BTOR_COUNT_STACK (btor->nodes_id_table) == exp->id + 1);
  assert (BTOR_PEEK_STACK (btor->nodes_id_table, exp->id) == exp);
  btor->stats.node_bytes_alloc += exp->bytes;

  if (btor_is_apply_node (exp)) exp->apply_below = 1;
}

/* Enlarges unique table and rehashes expressions. */
static void
enlarge_nodes_unique_table (Btor *btor)
{
  assert (btor);

  BtorMemMgr *mm;
  uint32_t size, new_size, i;
  uint32_t hash;
  BtorNode *cur, *temp, **new_chains;

  mm       = btor->mm;
  size     = btor->nodes_unique_table.size;
  new_size = size ? 2 * size : 1;
  BTOR_CNEWN (mm, new_chains, new_size);
  for (i = 0; i < size; i++)
  {
    cur = btor->nodes_unique_table.chains[i];
    while (cur)
    {
      assert (BTOR_IS_REGULAR_NODE (cur));
      assert (!btor_is_bv_var_node (cur));
      assert (!btor_is_uf_node (cur));
      temp             = cur->next;
      hash             = compute_hash_exp (btor, cur, new_size);
      cur->next        = new_chains[hash];
      new_chains[hash] = cur;
      cur              = temp;
    }
  }
  BTOR_DELETEN (mm, btor->nodes_unique_table.chains, size);
  btor->nodes_unique_table.size   = new_size;
  btor->nodes_unique_table.chains = new_chains;
}

static void
remove_from_nodes_unique_table_exp (Btor *btor, BtorNode *exp)
{
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));

  uint32_t hash;
  BtorNode *cur, *prev;

  if (!exp->unique) return;

  assert (btor);
  assert (btor->nodes_unique_table.num_elements > 0);

  hash = compute_hash_exp (btor, exp, btor->nodes_unique_table.size);
  prev = 0;
  cur  = btor->nodes_unique_table.chains[hash];

  while (cur != exp)
  {
    assert (cur);
    assert (BTOR_IS_REGULAR_NODE (cur));
    prev = cur;
    cur  = cur->next;
  }
  assert (cur);
  if (!prev)
    btor->nodes_unique_table.chains[hash] = cur->next;
  else
    prev->next = cur->next;

  btor->nodes_unique_table.num_elements--;

  exp->unique = 0; /* NOTE: this is not debugging code ! */
  exp->next   = 0;
}

static void
remove_from_hash_tables (Btor *btor, BtorNode *exp, bool keep_symbol)
{
  assert (btor);
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (!btor_is_invalid_node (exp));

  BtorHashTableData data;

  switch (exp->kind)
  {
    case BTOR_BV_VAR_NODE:
      btor_hashptr_table_remove (btor->bv_vars, exp, 0, 0);
      break;
    case BTOR_LAMBDA_NODE:
      btor_hashptr_table_remove (btor->lambdas, exp, 0, 0);
      break;
    case BTOR_UF_NODE: btor_hashptr_table_remove (btor->ufs, exp, 0, 0); break;
    case BTOR_FUN_EQ_NODE:
      btor_hashptr_table_remove (btor->feqs, exp, 0, 0);
      break;
    default: break;
  }

  if (!keep_symbol && btor_hashptr_table_get (btor->node2symbol, exp))
  {
    btor_hashptr_table_remove (btor->node2symbol, exp, 0, &data);
    if (data.as_str[0] != 0)
    {
      btor_hashptr_table_remove (btor->symbols, data.as_str, 0, 0);
      btor_mem_freestr (btor->mm, data.as_str);
    }
  }

  if (btor_hashptr_table_get (btor->parameterized, exp))
  {
    btor_hashptr_table_remove (btor->parameterized, exp, 0, &data);
    assert (data.as_ptr);
    btor_hashint_table_delete (data.as_ptr);
  }
}

/*------------------------------------------------------------------------*/

/* Connects child to its parent and updates list of parent pointers.
 * Expressions are inserted at the beginning of the regular parent list
 */
static void
connect_child_exp (Btor *btor, BtorNode *parent, BtorNode *child, uint32_t pos)
{
  assert (btor);
  assert (parent);
  assert (BTOR_IS_REGULAR_NODE (parent));
  assert (btor == parent->btor);
  assert (child);
  assert (btor == BTOR_REAL_ADDR_NODE (child)->btor);
  assert (pos <= 2);
  assert (btor_simplify_exp (btor, child) == child);
  assert (!btor_is_args_node (child) || btor_is_args_node (parent)
          || btor_is_apply_node (parent));

  (void) btor;
  uint32_t tag;
  bool insert_beginning = 1;
  BtorNode *real_child, *first_parent, *last_parent, *tagged_parent;

  /* set specific flags */

  /* set parent parameterized if child is parameterized */
  if (!btor_is_lambda_node (parent)
      && BTOR_REAL_ADDR_NODE (child)->parameterized)
    parent->parameterized = 1;

  // TODO (ma): why don't we bind params here?

  if (btor_is_fun_cond_node (parent) && BTOR_REAL_ADDR_NODE (child)->is_array)
    parent->is_array = 1;

  if (BTOR_REAL_ADDR_NODE (child)->lambda_below) parent->lambda_below = 1;

  if (BTOR_REAL_ADDR_NODE (child)->apply_below) parent->apply_below = 1;

  BTOR_REAL_ADDR_NODE (child)->parents++;
  inc_exp_ref_counter (btor, child);

  /* update parent lists */

  if (btor_is_apply_node (parent)) insert_beginning = false;

  real_child     = BTOR_REAL_ADDR_NODE (child);
  parent->e[pos] = child;
  tagged_parent  = BTOR_TAG_NODE (parent, pos);

  assert (!parent->prev_parent[pos]);
  assert (!parent->next_parent[pos]);

  /* no parent so far? */
  if (!real_child->first_parent)
  {
    assert (!real_child->last_parent);
    real_child->first_parent = tagged_parent;
    real_child->last_parent  = tagged_parent;
  }
  /* add parent at the beginning of the list */
  else if (insert_beginning)
  {
    first_parent = real_child->first_parent;
    assert (first_parent);
    parent->next_parent[pos] = first_parent;
    tag                      = btor_exp_get_tag (first_parent);
    BTOR_REAL_ADDR_NODE (first_parent)->prev_parent[tag] = tagged_parent;
    real_child->first_parent                             = tagged_parent;
  }
  /* add parent at the end of the list */
  else
  {
    last_parent = real_child->last_parent;
    assert (last_parent);
    parent->prev_parent[pos] = last_parent;
    tag                      = btor_exp_get_tag (last_parent);
    BTOR_REAL_ADDR_NODE (last_parent)->next_parent[tag] = tagged_parent;
    real_child->last_parent                             = tagged_parent;
  }
}

/* Disconnects a child from its parent and updates its parent list */
static void
disconnect_child_exp (Btor *btor, BtorNode *parent, uint32_t pos)
{
  assert (btor);
  assert (parent);
  assert (BTOR_IS_REGULAR_NODE (parent));
  assert (btor == parent->btor);
  assert (!btor_is_bv_const_node (parent));
  assert (!btor_is_bv_var_node (parent));
  assert (!btor_is_uf_node (parent));
  assert (pos <= 2);

  (void) btor;
  BtorNode *first_parent, *last_parent;
  BtorNode *real_child, *tagged_parent;

  tagged_parent = BTOR_TAG_NODE (parent, pos);
  real_child    = BTOR_REAL_ADDR_NODE (parent->e[pos]);
  real_child->parents--;
  first_parent = real_child->first_parent;
  last_parent  = real_child->last_parent;
  assert (first_parent);
  assert (last_parent);

  /* if a parameter is disconnected from a lambda we have to reset
   * 'lambda_exp' of the parameter in order to keep a valid state */
  if (btor_is_lambda_node (parent)
      && pos == 0
      /* if parent gets rebuilt via substitute_and_rebuild, it might
       * result in a new lambda term, where the param is already reused.
       * if this is the case param is already bound by a different lambda
       * and we are not allowed to reset param->lambda_exp to 0. */
      && btor_param_get_binding_lambda (parent->e[0]) == parent)
    btor_param_set_binding_lambda (parent->e[0], 0);

  /* only one parent? */
  if (first_parent == tagged_parent && first_parent == last_parent)
  {
    assert (!parent->next_parent[pos]);
    assert (!parent->prev_parent[pos]);
    real_child->first_parent = 0;
    real_child->last_parent  = 0;
  }
  /* is parent first parent in the list? */
  else if (first_parent == tagged_parent)
  {
    assert (parent->next_parent[pos]);
    assert (!parent->prev_parent[pos]);
    real_child->first_parent                    = parent->next_parent[pos];
    BTOR_PREV_PARENT (real_child->first_parent) = 0;
  }
  /* is parent last parent in the list? */
  else if (last_parent == tagged_parent)
  {
    assert (!parent->next_parent[pos]);
    assert (parent->prev_parent[pos]);
    real_child->last_parent                    = parent->prev_parent[pos];
    BTOR_NEXT_PARENT (real_child->last_parent) = 0;
  }
  /* detach parent from list */
  else
  {
    assert (parent->next_parent[pos]);
    assert (parent->prev_parent[pos]);
    BTOR_PREV_PARENT (parent->next_parent[pos]) = parent->prev_parent[pos];
    BTOR_NEXT_PARENT (parent->prev_parent[pos]) = parent->next_parent[pos];
  }
  parent->next_parent[pos] = 0;
  parent->prev_parent[pos] = 0;
  parent->e[pos]           = 0;
}

/* Disconnect children of expression in parent list and if applicable from
 * unique table.  Do not touch local data, nor any reference counts.  The
 * kind of the expression becomes 'BTOR_DISCONNECTED_NODE' in debugging mode.
 *
 * Actually we have the sequence
 *
 *   UNIQUE -> !UNIQUE -> ERASED -> DISCONNECTED -> INVALID
 *
 * after a unique or non uninque expression is allocated until it is
 * deallocated.  We also have loop back from DISCONNECTED to !UNIQUE
 * if an expression is rewritten and reused as PROXY.
 */
static void
disconnect_children_exp (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (!btor_is_invalid_node (exp));
  assert (!exp->unique);
  assert (exp->erased);
  assert (!exp->disconnected);

  uint32_t i;

  for (i = 0; i < exp->arity; i++) disconnect_child_exp (btor, exp, i);
  exp->disconnected = 1;
}

/*------------------------------------------------------------------------*/

/* Delete local data of expression.
 *
 * Virtual reads and simplified expressions have to be handled by the
 * calling function, e.g. 'btor_release_exp', to avoid recursion.
 *
 * We use this function to update operator stats
 */
static void
erase_local_data_exp (Btor *btor, BtorNode *exp, bool free_sort)
{
  assert (btor);
  assert (exp);

  assert (BTOR_IS_REGULAR_NODE (exp));

  assert (!exp->unique);
  assert (!exp->erased);
  assert (!exp->disconnected);
  assert (!btor_is_invalid_node (exp));

  BtorMemMgr *mm;
  BtorPtrHashTable *static_rho;
  BtorPtrHashTableIterator it;

  mm = btor->mm;
  //  BTORLOG ("%s: %s", __FUNCTION__, btor_util_node2string (exp));

  switch (exp->kind)
  {
    case BTOR_BV_CONST_NODE:
      btor_bv_free (mm, btor_const_get_bits (exp));
      if (btor_const_get_invbits (exp))
        btor_bv_free (mm, btor_const_get_invbits (exp));
      btor_const_set_bits (exp, 0);
      btor_const_set_invbits (exp, 0);
      break;
    case BTOR_LAMBDA_NODE:
      static_rho = btor_lambda_get_static_rho (exp);
      if (static_rho)
      {
        btor_iter_hashptr_init (&it, static_rho);
        while (btor_iter_hashptr_has_next (&it))
        {
          btor_release_exp (btor, it.bucket->data.as_ptr);
          btor_release_exp (btor, btor_iter_hashptr_next (&it));
        }
        btor_hashptr_table_delete (static_rho);
        ((BtorLambdaNode *) exp)->static_rho = 0;
      }
      /* fall through intended */
    case BTOR_UF_NODE:
      if (exp->rho)
      {
        btor_hashptr_table_delete (exp->rho);
        exp->rho = 0;
      }
      break;
    case BTOR_COND_NODE:
      if (btor_is_fun_cond_node (exp) && exp->rho)
      {
        btor_hashptr_table_delete (exp->rho);
        exp->rho = 0;
      }
      break;
    default: break;
  }

  if (free_sort)
  {
    assert (btor_exp_get_sort_id (exp));
    btor_sort_release (btor, btor_exp_get_sort_id (exp));
    btor_exp_set_sort_id (exp, 0);
  }

  if (exp->av)
  {
    btor_aigvec_release_delete (btor->avmgr, exp->av);
    exp->av = 0;
  }
  exp->erased = 1;
}

static void
really_deallocate_exp (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (btor == exp->btor);
  assert (!exp->unique);
  assert (exp->disconnected);
  assert (exp->erased);
  assert (exp->id);
  assert (BTOR_PEEK_STACK (btor->nodes_id_table, exp->id) == exp);
  BTOR_POKE_STACK (btor->nodes_id_table, exp->id, 0);

  BtorMemMgr *mm;

  mm = btor->mm;

  set_kind (btor, exp, BTOR_INVALID_NODE);

  if (btor_is_bv_const_node (exp))
  {
    btor_bv_free (btor->mm, btor_const_get_bits (exp));
    if (btor_const_get_invbits (exp))
      btor_bv_free (btor->mm, btor_const_get_invbits (exp));
  }
  btor_mem_free (mm, exp, exp->bytes);
}

static void
recursively_release_exp (Btor *btor, BtorNode *root)
{
  assert (btor);
  assert (root);
  assert (BTOR_IS_REGULAR_NODE (root));
  assert (root->refs == 1);

  BtorNodePtrStack stack;
  BtorMemMgr *mm;
  BtorNode *cur;
  uint32_t i;

  mm = btor->mm;

  BTOR_INIT_STACK (mm, stack);
  cur = root;
  goto RECURSIVELY_RELEASE_NODE_ENTER_WITHOUT_POP;

  do
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (stack));

    if (cur->refs > 1)
      cur->refs--;
    else
    {
    RECURSIVELY_RELEASE_NODE_ENTER_WITHOUT_POP:
      assert (cur->refs == 1);
      assert (!cur->ext_refs || cur->ext_refs == 1);
      assert (cur->parents == 0);

      for (i = 1; i <= cur->arity; i++)
        BTOR_PUSH_STACK (stack, cur->e[cur->arity - i]);

      if (cur->simplified)
      {
        BTOR_PUSH_STACK (stack, cur->simplified);
        cur->simplified = 0;
      }

      remove_from_nodes_unique_table_exp (btor, cur);
      erase_local_data_exp (btor, cur, true);

      /* It is safe to access the children here, since they are pushed
       * on the stack and will be released later if necessary.
       */
      remove_from_hash_tables (btor, cur, 0);
      disconnect_children_exp (btor, cur);
      really_deallocate_exp (btor, cur);
    }
  } while (!BTOR_EMPTY_STACK (stack));
  BTOR_RELEASE_STACK (stack);
}

void
btor_release_exp (Btor *btor, BtorNode *root)
{
  assert (btor);
  assert (root);
  assert (btor == BTOR_REAL_ADDR_NODE (root)->btor);

  root = BTOR_REAL_ADDR_NODE (root);

  assert (root->refs > 0);

  if (root->refs > 1)
    root->refs--;
  else
    recursively_release_exp (btor, root);
}

/*------------------------------------------------------------------------*/

void
btor_set_to_proxy_exp (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (btor == exp->btor);
  assert (exp->simplified);

  uint32_t i;
  BtorNode *e[3];

  remove_from_nodes_unique_table_exp (btor, exp);
  /* also updates op stats */
  erase_local_data_exp (btor, exp, false);
  assert (exp->arity <= 3);
  BTOR_CLR (e);
  for (i = 0; i < exp->arity; i++) e[i] = exp->e[i];
  remove_from_hash_tables (btor, exp, 1);
  disconnect_children_exp (btor, exp);

  for (i = 0; i < exp->arity; i++) btor_release_exp (btor, e[i]);

  set_kind (btor, exp, BTOR_PROXY_NODE);

  exp->disconnected  = 0;
  exp->erased        = 0;
  exp->arity         = 0;
  exp->parameterized = 0;
}

/*------------------------------------------------------------------------*/

void
btor_exp_set_btor_id (Btor *btor, BtorNode *exp, int32_t id)
{
  assert (btor);
  assert (exp);
  assert (id);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  assert (btor_is_bv_var_node (exp) || btor_is_uf_array_node (exp));

  (void) btor;
  BtorNode *real_exp;
  BtorPtrHashBucket *b;

  real_exp = BTOR_REAL_ADDR_NODE (exp);
  b        = btor_hashptr_table_get (btor->inputs, real_exp);
  assert (b);
  b->data.as_int = id;
}

int32_t
btor_exp_get_btor_id (BtorNode *exp)
{
  assert (exp);

  int32_t id = 0;
  Btor *btor;
  BtorNode *real_exp;
  BtorPtrHashBucket *b;

  real_exp = BTOR_REAL_ADDR_NODE (exp);
  btor     = real_exp->btor;

  if ((b = btor_hashptr_table_get (btor->inputs, real_exp)))
    id = b->data.as_int;
  if (BTOR_IS_INVERTED_NODE (exp)) return -id;
  return id;
}

BtorNode *
btor_match_node_by_id (Btor *btor, int32_t id)
{
  assert (btor);
  assert (id > 0);
  if (id >= BTOR_COUNT_STACK (btor->nodes_id_table)) return 0;
  return btor_copy_exp (btor, BTOR_PEEK_STACK (btor->nodes_id_table, id));
}

BtorNode *
btor_get_node_by_id (Btor *btor, int32_t id)
{
  assert (btor);
  bool is_inverted = id < 0;
  id               = abs (id);
  if (id >= BTOR_COUNT_STACK (btor->nodes_id_table)) return 0;
  return BTOR_COND_INVERT_NODE (is_inverted,
                                BTOR_PEEK_STACK (btor->nodes_id_table, id));
}

/*------------------------------------------------------------------------*/

char *
btor_get_symbol_exp (Btor *btor, const BtorNode *exp)
{
  /* do not pointer-chase! */
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  BtorPtrHashBucket *b;

  b = btor_hashptr_table_get (btor->node2symbol, BTOR_REAL_ADDR_NODE (exp));
  if (b) return b->data.as_str;
  return 0;
}

void
btor_set_symbol_exp (Btor *btor, BtorNode *exp, const char *symbol)
{
  /* do not pointer-chase! */
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  assert (symbol);
  assert (!btor_hashptr_table_get (btor->symbols, (char *) symbol));

  BtorPtrHashBucket *b;
  char *sym;

  exp = BTOR_REAL_ADDR_NODE (exp);
  sym = btor_mem_strdup (btor->mm, symbol);
  btor_hashptr_table_add (btor->symbols, sym)->data.as_ptr = exp;
  b = btor_hashptr_table_get (btor->node2symbol, exp);

  if (b)
  {
    btor_hashptr_table_remove (btor->symbols, b->data.as_str, 0, 0);
    btor_mem_freestr (btor->mm, b->data.as_str);
  }
  else
    b = btor_hashptr_table_add (btor->node2symbol, exp);

  b->data.as_str = sym;
}

BtorNode *
btor_get_node_by_symbol (Btor *btor, const char *sym)
{
  assert (btor);
  assert (sym);
  BtorPtrHashBucket *b;
  b = btor_hashptr_table_get (btor->symbols, (char *) sym);
  if (!b) return 0;
  return b->data.as_ptr;
}

BtorNode *
btor_match_node_by_symbol (Btor *btor, const char *sym)
{
  assert (btor);
  assert (sym);
  return btor_copy_exp (btor, btor_get_node_by_symbol (btor, sym));
}

/*------------------------------------------------------------------------*/

BtorNode *
btor_match_node (Btor *btor, BtorNode *exp)
{
  assert (btor);
  assert (exp);

  uint32_t id;
  BtorNode *res;

  id = BTOR_REAL_ADDR_NODE (exp)->id;
  assert (id > 0);
  if (id >= BTOR_COUNT_STACK (btor->nodes_id_table)) return 0;
  res = btor_copy_exp (btor, BTOR_PEEK_STACK (btor->nodes_id_table, id));
  return BTOR_IS_INVERTED_NODE (exp) ? BTOR_INVERT_NODE (res) : res;
}

/*------------------------------------------------------------------------*/

/* Compares expressions by id */
int32_t
btor_compare_exp_by_id (const BtorNode *exp0, const BtorNode *exp1)
{
  assert (exp0);
  assert (exp1);

  uint32_t id0, id1;

  id0 = btor_exp_get_id (exp0);
  id1 = btor_exp_get_id (exp1);
  if (id0 < id1) return -1;
  if (id0 > id1) return 1;
  return 0;
}

int32_t
btor_compare_exp_by_id_qsort_desc (const void *p, const void *q)
{
  BtorNode *a = BTOR_REAL_ADDR_NODE (*(BtorNode **) p);
  BtorNode *b = BTOR_REAL_ADDR_NODE (*(BtorNode **) q);
  return b->id - a->id;
}

int32_t
btor_compare_exp_by_id_qsort_asc (const void *p, const void *q)
{
  BtorNode *a = BTOR_REAL_ADDR_NODE (*(BtorNode **) p);
  BtorNode *b = BTOR_REAL_ADDR_NODE (*(BtorNode **) q);
  return a->id - b->id;
}

/* Computes hash value of expression by id */
uint32_t
btor_hash_exp_by_id (const BtorNode *exp)
{
  assert (exp);
  return (uint32_t) btor_exp_get_id (exp) * 7334147u;
}

/*------------------------------------------------------------------------*/

uint32_t
btor_get_exp_width (Btor *btor, const BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (!btor_is_fun_node (exp));
  assert (!btor_is_args_node (exp));
  return btor_sort_bitvec_get_width (btor, btor_exp_get_sort_id (exp));
}

uint32_t
btor_get_fun_exp_width (Btor *btor, const BtorNode *exp)
{
  assert (btor);
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));

  assert (btor_sort_is_fun (btor, btor_exp_get_sort_id (exp)));
  return btor_sort_bitvec_get_width (
      btor, btor_sort_fun_get_codomain (btor, btor_exp_get_sort_id (exp)));
}

uint32_t
btor_get_index_exp_width (Btor *btor, const BtorNode *e_array)
{
  assert (btor);
  assert (e_array);
  assert (btor == BTOR_REAL_ADDR_NODE (e_array)->btor);

  assert (btor_sort_is_array (btor, btor_exp_get_sort_id (e_array))
          || btor_sort_is_fun (btor, btor_exp_get_sort_id (e_array)));
  return btor_sort_bitvec_get_width (
      btor, btor_sort_array_get_index (btor, btor_exp_get_sort_id (e_array)));
}

/*------------------------------------------------------------------------*/

BtorBitVector *
btor_const_get_bits (BtorNode *exp)
{
  assert (exp);
  assert (btor_is_bv_const_node (exp));
  return ((BtorBVConstNode *) BTOR_REAL_ADDR_NODE (exp))->bits;
}

BtorBitVector *
btor_const_get_invbits (BtorNode *exp)
{
  assert (exp);
  assert (btor_is_bv_const_node (exp));
  return ((BtorBVConstNode *) BTOR_REAL_ADDR_NODE (exp))->invbits;
}

void
btor_const_set_bits (BtorNode *exp, BtorBitVector *bits)
{
  assert (exp);
  assert (btor_is_bv_const_node (exp));
  ((BtorBVConstNode *) BTOR_REAL_ADDR_NODE (exp))->bits = bits;
}

void
btor_const_set_invbits (BtorNode *exp, BtorBitVector *bits)
{
  assert (exp);
  assert (btor_is_bv_const_node (exp));
  ((BtorBVConstNode *) BTOR_REAL_ADDR_NODE (exp))->invbits = bits;
}

/*------------------------------------------------------------------------*/

uint32_t
btor_get_fun_arity (Btor *btor, BtorNode *exp)
{
  (void) btor;
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  exp = btor_simplify_exp (btor, exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (btor_sort_is_fun (btor, btor_exp_get_sort_id (exp)));
  return btor_sort_fun_get_arity (btor, btor_exp_get_sort_id (exp));
}

uint32_t
btor_get_args_arity (Btor *btor, BtorNode *exp)
{
  (void) btor;
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);
  exp = btor_simplify_exp (btor, exp);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (btor_is_args_node (exp));
  return btor_sort_tuple_get_arity (btor, btor_exp_get_sort_id (exp));
}

/*------------------------------------------------------------------------*/

BtorNode *
btor_lambda_get_body (BtorNode *lambda)
{
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  return ((BtorLambdaNode *) lambda)->body;
}

void
btor_lambda_set_body (BtorNode *lambda, BtorNode *body)
{
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  ((BtorLambdaNode *) lambda)->body = body;
}

BtorPtrHashTable *
btor_lambda_get_static_rho (BtorNode *lambda)
{
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  return ((BtorLambdaNode *) lambda)->static_rho;
}

void
btor_lambda_set_static_rho (BtorNode *lambda, BtorPtrHashTable *static_rho)
{
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  ((BtorLambdaNode *) lambda)->static_rho = static_rho;
}

BtorPtrHashTable *
btor_lambda_copy_static_rho (Btor *btor, BtorNode *lambda)
{
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  assert (btor_lambda_get_static_rho (lambda));

  BtorNode *data, *key;
  BtorPtrHashTableIterator it;
  BtorPtrHashTable *static_rho;

  btor_iter_hashptr_init (&it, btor_lambda_get_static_rho (lambda));
  static_rho = btor_hashptr_table_new (btor->mm,
                                       (BtorHashPtr) btor_hash_exp_by_id,
                                       (BtorCmpPtr) btor_compare_exp_by_id);
  while (btor_iter_hashptr_has_next (&it))
  {
    data = btor_copy_exp (btor, it.bucket->data.as_ptr);
    key  = btor_copy_exp (btor, btor_iter_hashptr_next (&it));
    btor_hashptr_table_add (static_rho, key)->data.as_ptr = data;
  }
  return static_rho;
}

void
btor_lambda_delete_static_rho (Btor *btor, BtorNode *lambda)
{
  BtorPtrHashTable *static_rho;
  BtorPtrHashTableIterator it;

  static_rho = btor_lambda_get_static_rho (lambda);
  if (!static_rho) return;

  btor_iter_hashptr_init (&it, static_rho);
  while (btor_iter_hashptr_has_next (&it))
  {
    btor_release_exp (btor, it.bucket->data.as_ptr);
    btor_release_exp (btor, btor_iter_hashptr_next (&it));
  }
  btor_hashptr_table_delete (static_rho);
  btor_lambda_set_static_rho (lambda, 0);
}

/*------------------------------------------------------------------------*/

uint32_t
btor_slice_get_upper (BtorNode *slice)
{
  assert (btor_is_slice_node (slice));
  return ((BtorSliceNode *) BTOR_REAL_ADDR_NODE (slice))->upper;
}

uint32_t
btor_slice_get_lower (BtorNode *slice)
{
  assert (btor_is_slice_node (slice));
  return ((BtorSliceNode *) BTOR_REAL_ADDR_NODE (slice))->lower;
}

/*------------------------------------------------------------------------*/

BtorNode *
btor_param_get_binding_lambda (BtorNode *param)
{
  assert (btor_is_param_node (param));
  return ((BtorParamNode *) BTOR_REAL_ADDR_NODE (param))->lambda_exp;
}

void
btor_param_set_binding_lambda (BtorNode *param, BtorNode *lambda)
{
  assert (btor_is_param_node (param));
  assert (!lambda || btor_is_lambda_node (lambda));
  ((BtorParamNode *) BTOR_REAL_ADDR_NODE (param))->lambda_exp = lambda;
}

bool
btor_param_is_bound (BtorNode *param)
{
  assert (btor_is_param_node (param));
  return btor_param_get_binding_lambda (param) != 0;
}

BtorNode *
btor_param_get_assigned_exp (BtorNode *param)
{
  assert (btor_is_param_node (param));
  return ((BtorParamNode *) BTOR_REAL_ADDR_NODE (param))->assigned_exp;
}

BtorNode *
btor_param_set_assigned_exp (BtorNode *param, BtorNode *exp)
{
  assert (btor_is_param_node (param));
  assert (!exp || btor_exp_get_sort_id (param) == btor_exp_get_sort_id (exp));
  return ((BtorParamNode *) BTOR_REAL_ADDR_NODE (param))->assigned_exp = exp;
}

/*------------------------------------------------------------------------*/

static bool
is_sorted_bv_exp (Btor *btor, BtorNodeKind kind, BtorNode *e[])
{
  if (!btor_opt_get (btor, BTOR_OPT_SORT_EXP)) return 1;
  if (!btor_is_binary_commutative_node_kind (kind)) return 1;
  if (e[0] == e[1]) return 1;
  if (BTOR_INVERT_NODE (e[0]) == e[1] && BTOR_IS_INVERTED_NODE (e[1])) return 1;
  return BTOR_REAL_ADDR_NODE (e[0])->id <= BTOR_REAL_ADDR_NODE (e[1])->id;
}

static void
sort_bv_exp (Btor *btor, BtorNodeKind kind, BtorNode *e[])
{
  if (!is_sorted_bv_exp (btor, kind, e)) BTOR_SWAP (BtorNode *, e[0], e[1]);
}

/*------------------------------------------------------------------------*/

/* Search for constant expression in hash table. Returns 0 if not found. */
static BtorNode **
find_const_exp (Btor *btor, BtorBitVector *bits)
{
  assert (btor);
  assert (bits);

  BtorNode *cur, **result;
  uint32_t hash;

  hash = btor_bv_hash (bits);
  hash &= btor->nodes_unique_table.size - 1;
  result = btor->nodes_unique_table.chains + hash;
  cur    = *result;
  while (cur)
  {
    assert (BTOR_IS_REGULAR_NODE (cur));
    if (btor_is_bv_const_node (cur)
        && btor_get_exp_width (btor, cur) == bits->width
        && !btor_bv_compare (btor_const_get_bits (cur), bits))
      break;
    else
    {
      result = &cur->next;
      cur    = *result;
    }
  }
  return result;
}

/* Search for slice expression in hash table. Returns 0 if not found. */
static BtorNode **
find_slice_exp (Btor *btor, BtorNode *e0, uint32_t upper, uint32_t lower)
{
  assert (btor);
  assert (e0);
  assert (upper >= lower);

  BtorNode *cur, **result;
  uint32_t hash;

  hash = hash_slice_exp (e0, upper, lower);
  hash &= btor->nodes_unique_table.size - 1;
  result = btor->nodes_unique_table.chains + hash;
  cur    = *result;
  while (cur)
  {
    assert (BTOR_IS_REGULAR_NODE (cur));
    if (cur->kind == BTOR_SLICE_NODE && cur->e[0] == e0
        && btor_slice_get_upper (cur) == upper
        && btor_slice_get_lower (cur) == lower)
      break;
    else
    {
      result = &cur->next;
      cur    = *result;
    }
  }
  return result;
}

static BtorNode **
find_bv_exp (Btor *btor, BtorNodeKind kind, BtorNode *e[], uint32_t arity)
{
  bool equal;
  uint32_t i;
  uint32_t hash;
  BtorNode *cur, **result;

  assert (kind != BTOR_SLICE_NODE);
  assert (kind != BTOR_BV_CONST_NODE);

  sort_bv_exp (btor, kind, e);
  hash = hash_bv_exp (btor, kind, arity, e);
  hash &= btor->nodes_unique_table.size - 1;

  result = btor->nodes_unique_table.chains + hash;
  cur    = *result;
  while (cur)
  {
    assert (BTOR_IS_REGULAR_NODE (cur));
    if (cur->kind == kind && cur->arity == arity)
    {
      equal = true;
      /* special case for bv eq; (= (bvnot a) b) == (= a (bvnot b)) */
      if (kind == BTOR_BV_EQ_NODE && cur->e[0] == BTOR_INVERT_NODE (e[0])
          && cur->e[1] == BTOR_INVERT_NODE (e[1]))
        break;
      for (i = 0; i < arity && equal; i++)
        if (cur->e[i] != e[i]) equal = false;
      if (equal) break;
#ifndef NDEBUG
      if (btor_opt_get (btor, BTOR_OPT_SORT_EXP) > 0
          && btor_is_binary_commutative_node_kind (kind))
        assert (arity == 2),
            assert (e[0] == e[1] || BTOR_INVERT_NODE (e[0]) == e[1]
                    || !(cur->e[0] == e[1] && cur->e[1] == e[0]));
#endif
    }
    result = &(cur->next);
    cur    = *result;
  }
  return result;
}

static int32_t compare_lambda_exp (Btor *, BtorNode *, BtorNode *, BtorNode *);

static BtorNode **
find_lambda_exp (Btor *btor,
                 BtorNode *param,
                 BtorNode *body,
                 uint32_t *lambda_hash,
                 BtorIntHashTable *params,
                 bool compare_lambdas)
{
  assert (btor);
  assert (param);
  assert (body);
  assert (BTOR_IS_REGULAR_NODE (param));
  assert (btor_is_param_node (param));

  BtorNode *cur, **result;
  uint32_t hash;

  hash = hash_lambda_exp (btor, param, body, params);
  if (lambda_hash) *lambda_hash = hash;
  hash &= btor->nodes_unique_table.size - 1;
  result = btor->nodes_unique_table.chains + hash;
  cur    = *result;
  while (cur)
  {
    assert (BTOR_IS_REGULAR_NODE (cur));
    if (cur->kind == BTOR_LAMBDA_NODE
        && ((param == cur->e[0] && body == cur->e[1])
            || ((!cur->parameterized && compare_lambdas
                 && compare_lambda_exp (btor, param, body, cur)))))
      break;
    else
    {
      result = &cur->next;
      cur    = *result;
    }
  }
  assert (!*result || btor_is_lambda_node (*result));
  return result;
}

static int32_t
compare_lambda_exp (Btor *btor,
                    BtorNode *param,
                    BtorNode *body,
                    BtorNode *lambda)
{
  assert (btor);
  assert (param);
  assert (body);
  assert (BTOR_IS_REGULAR_NODE (param));
  assert (btor_is_param_node (param));
  assert (BTOR_IS_REGULAR_NODE (lambda));
  assert (btor_is_lambda_node (lambda));
  assert (!lambda->parameterized);

  uint32_t i;
  int32_t equal = 0;
  BtorMemMgr *mm;
  BtorNode *cur, *real_cur, **result, *subst_param, **e, *l0, *l1;
  BtorPtrHashTable *cache, *param_map;
  BtorPtrHashBucket *b, *bb;
  BtorNodePtrStack stack, args;
  BtorNodeIterator it, iit;

  mm          = btor->mm;
  subst_param = lambda->e[0];

  if (btor_exp_get_sort_id (subst_param) != btor_exp_get_sort_id (param)
      || btor_exp_get_sort_id (body) != btor_exp_get_sort_id (lambda->e[1]))
    return 0;

  cache = btor_hashptr_table_new (mm, 0, 0);

  /* create param map */
  param_map = btor_hashptr_table_new (mm, 0, 0);
  btor_hashptr_table_add (param_map, param)->data.as_ptr = subst_param;

  if (btor_is_lambda_node (body) && btor_is_lambda_node (lambda->e[1]))
  {
    btor_iter_lambda_init (&it, body);
    btor_iter_lambda_init (&iit, lambda->e[1]);
    while (btor_iter_lambda_has_next (&it))
    {
      if (!btor_iter_lambda_has_next (&iit)) goto NOT_EQUAL;

      l0 = btor_iter_lambda_next (&it);
      l1 = btor_iter_lambda_next (&iit);

      if (btor_exp_get_sort_id (l0) != btor_exp_get_sort_id (l1))
        goto NOT_EQUAL;

      param       = l0->e[0];
      subst_param = l1->e[0];
      assert (BTOR_IS_REGULAR_NODE (param));
      assert (BTOR_IS_REGULAR_NODE (subst_param));
      assert (btor_is_param_node (param));
      assert (btor_is_param_node (subst_param));

      if (btor_exp_get_sort_id (param) != btor_exp_get_sort_id (subst_param))
        goto NOT_EQUAL;

      btor_hashptr_table_add (param_map, param)->data.as_ptr = subst_param;
    }
  }
  else if (btor_is_lambda_node (body) || btor_is_lambda_node (lambda->e[1]))
    goto NOT_EQUAL;

  BTOR_INIT_STACK (mm, args);
  BTOR_INIT_STACK (mm, stack);
  BTOR_PUSH_STACK (stack, body);
  while (!BTOR_EMPTY_STACK (stack))
  {
    cur      = BTOR_POP_STACK (stack);
    real_cur = BTOR_REAL_ADDR_NODE (cur);

    if (!real_cur->parameterized)
    {
      BTOR_PUSH_STACK (args, cur);
      continue;
    }

    b = btor_hashptr_table_get (cache, real_cur);

    if (!b)
    {
      b = btor_hashptr_table_add (cache, real_cur);
      BTOR_PUSH_STACK (stack, cur);
      for (i = 1; i <= real_cur->arity; i++)
        BTOR_PUSH_STACK (stack, real_cur->e[real_cur->arity - i]);
    }
    else if (!b->data.as_ptr)
    {
      assert (BTOR_COUNT_STACK (args) >= real_cur->arity);
      args.top -= real_cur->arity;
      e = args.top;

      if (btor_is_slice_node (real_cur))
      {
        result = find_slice_exp (btor,
                                 e[0],
                                 btor_slice_get_upper (real_cur),
                                 btor_slice_get_lower (real_cur));
      }
      else if (btor_is_lambda_node (real_cur))
      {
        result = find_lambda_exp (btor, e[0], e[1], 0, 0, false);
      }
      else if (btor_is_param_node (real_cur))
      {
        if ((bb = btor_hashptr_table_get (param_map, real_cur)))
          result = (BtorNode **) &bb->data.as_ptr;
        else
          result = &real_cur;
      }
      else
      {
        assert (!btor_is_lambda_node (real_cur));
        result = find_bv_exp (btor, real_cur->kind, e, real_cur->arity);
      }

      if (!*result)
      {
        BTOR_RESET_STACK (args);
        break;
      }

      BTOR_PUSH_STACK (args, BTOR_COND_INVERT_NODE (cur, *result));
      b->data.as_ptr = *result;
    }
    else
    {
      assert (b->data.as_ptr);
      BTOR_PUSH_STACK (args, BTOR_COND_INVERT_NODE (cur, b->data.as_ptr));
    }
  }
  assert (BTOR_COUNT_STACK (args) <= 1);

  if (!BTOR_EMPTY_STACK (args)) equal = BTOR_TOP_STACK (args) == lambda->e[1];

  BTOR_RELEASE_STACK (stack);
  BTOR_RELEASE_STACK (args);
NOT_EQUAL:
  btor_hashptr_table_delete (cache);
  btor_hashptr_table_delete (param_map);
  return equal;
}

static BtorNode **
find_exp (Btor *btor,
          BtorNodeKind kind,
          BtorNode *e[],
          uint32_t arity,
          uint32_t *lambda_hash,
          BtorIntHashTable *params)
{
  assert (btor);
  assert (arity > 0);
  assert (e);

#ifndef NDEBUG
  uint32_t i;
  for (i = 0; i < arity; i++) assert (e[i]);
#endif

  if (kind == BTOR_LAMBDA_NODE)
    return find_lambda_exp (btor, e[0], e[1], lambda_hash, params, true);
  else if (lambda_hash)
    *lambda_hash = 0;

  return find_bv_exp (btor, kind, e, arity);
}

/*------------------------------------------------------------------------*/

static BtorNode *
new_const_exp_node (Btor *btor, BtorBitVector *bits)
{
  assert (btor);
  assert (bits);

  BtorBVConstNode *exp;

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_BV_CONST_NODE);
  exp->bytes = sizeof *exp;
  btor_exp_set_sort_id ((BtorNode *) exp, btor_sort_bitvec (btor, bits->width));
  setup_node_and_add_to_id_table (btor, exp);
  btor_const_set_bits ((BtorNode *) exp, btor_bv_copy (btor->mm, bits));
  btor_const_set_invbits ((BtorNode *) exp, btor_bv_not (btor->mm, bits));
  return (BtorNode *) exp;
}

static BtorNode *
new_slice_exp_node (Btor *btor, BtorNode *e0, uint32_t upper, uint32_t lower)
{
  assert (btor);
  assert (e0);
  assert (btor == BTOR_REAL_ADDR_NODE (e0)->btor);
  assert (upper < btor_get_exp_width (btor, e0));
  assert (upper >= lower);

  BtorSliceNode *exp = 0;

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_SLICE_NODE);
  exp->bytes = sizeof *exp;
  exp->arity = 1;
  exp->upper = upper;
  exp->lower = lower;
  btor_exp_set_sort_id ((BtorNode *) exp,
                        btor_sort_bitvec (btor, upper - lower + 1));
  setup_node_and_add_to_id_table (btor, exp);
  connect_child_exp (btor, (BtorNode *) exp, e0, 0);
  return (BtorNode *) exp;
}

static BtorNode *
new_lambda_exp_node (Btor *btor, BtorNode *e_param, BtorNode *e_exp)
{
  assert (btor);
  assert (e_param);
  assert (BTOR_IS_REGULAR_NODE (e_param));
  assert (btor_is_param_node (e_param));
  assert (!btor_param_is_bound (e_param));
  assert (e_exp);
  assert (btor == e_param->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_exp)->btor);

  BtorSortId s, domain, codomain;
  BtorSortIdStack param_sorts;
  BtorLambdaNode *lambda_exp;
  BtorTupleSortIterator it;
  BtorPtrHashBucket *b;
  BtorIntHashTable *params;

  BTOR_INIT_STACK (btor->mm, param_sorts);

  BTOR_CNEW (btor->mm, lambda_exp);
  set_kind (btor, (BtorNode *) lambda_exp, BTOR_LAMBDA_NODE);
  lambda_exp->bytes        = sizeof *lambda_exp;
  lambda_exp->arity        = 2;
  lambda_exp->lambda_below = 1;
  setup_node_and_add_to_id_table (btor, (BtorNode *) lambda_exp);
  connect_child_exp (btor, (BtorNode *) lambda_exp, e_param, 0);
  connect_child_exp (btor, (BtorNode *) lambda_exp, e_exp, 1);

  BTOR_PUSH_STACK (param_sorts, btor_exp_get_sort_id (e_param));
  /* curried lambdas (functions) */
  if (btor_is_lambda_node (e_exp))
  {
    btor_lambda_set_body (
        (BtorNode *) lambda_exp,
        btor_simplify_exp (btor, btor_lambda_get_body (e_exp)));
    btor_iter_tuple_sort_init (
        &it,
        btor,
        btor_sort_fun_get_domain (btor, btor_exp_get_sort_id (e_exp)));
    while (btor_iter_tuple_sort_has_next (&it))
    {
      s = btor_iter_tuple_sort_next (&it);
      BTOR_PUSH_STACK (param_sorts, s);
    }

    if ((b = btor_hashptr_table_get (btor->parameterized, e_exp)))
    {
      params = b->data.as_ptr;
      btor_hashint_table_remove (params, e_param->id);
      btor_hashptr_table_remove (btor->parameterized, e_exp, 0, 0);
      if (params->count > 0)
      {
        btor_hashptr_table_add (btor->parameterized, lambda_exp)->data.as_ptr =
            params;
        lambda_exp->parameterized = 1;
      }
      else
        btor_hashint_table_delete (params);
    }
  }
  else
    btor_lambda_set_body ((BtorNode *) lambda_exp, e_exp);

  domain =
      btor_sort_tuple (btor, param_sorts.start, BTOR_COUNT_STACK (param_sorts));
  codomain = btor_exp_get_sort_id (lambda_exp->body);
  btor_exp_set_sort_id ((BtorNode *) lambda_exp,
                        btor_sort_fun (btor, domain, codomain));

  btor_sort_release (btor, domain);
  BTOR_RELEASE_STACK (param_sorts);

  assert (!BTOR_REAL_ADDR_NODE (lambda_exp->body)->simplified);
  assert (!btor_is_lambda_node (lambda_exp->body));
  assert (!btor_hashptr_table_get (btor->lambdas, lambda_exp));
  (void) btor_hashptr_table_add (btor->lambdas, lambda_exp);
  /* set lambda expression of parameter */
  btor_param_set_binding_lambda (e_param, (BtorNode *) lambda_exp);
  return (BtorNode *) lambda_exp;
}

static BtorNode *
new_args_exp_node (Btor *btor, uint32_t arity, BtorNode *e[])
{
  assert (btor);
  assert (arity > 0);
  assert (arity <= 3);
  assert (e);

  uint32_t i;
  BtorArgsNode *exp;
  BtorSortIdStack sorts;
  BtorTupleSortIterator it;
#ifndef NDEBUG
  for (i = 0; i < arity; i++) assert (e[i]);
#endif

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_ARGS_NODE);
  exp->bytes = sizeof (*exp);
  exp->arity = arity;
  setup_node_and_add_to_id_table (btor, exp);

  for (i = 0; i < arity; i++)
    connect_child_exp (btor, (BtorNode *) exp, e[i], i);

  /* create tuple sort for argument node */
  BTOR_INIT_STACK (btor->mm, sorts);
  for (i = 0; i < arity; i++)
  {
    if (btor_is_args_node (e[i]))
    {
      assert (i == 2);
      assert (BTOR_IS_REGULAR_NODE (e[i]));
      btor_iter_tuple_sort_init (&it, btor, btor_exp_get_sort_id (e[i]));
      while (btor_iter_tuple_sort_has_next (&it))
        BTOR_PUSH_STACK (sorts, btor_iter_tuple_sort_next (&it));
    }
    else
      BTOR_PUSH_STACK (sorts, btor_exp_get_sort_id (e[i]));
  }
  btor_exp_set_sort_id (
      (BtorNode *) exp,
      btor_sort_tuple (btor, sorts.start, BTOR_COUNT_STACK (sorts)));
  BTOR_RELEASE_STACK (sorts);
  return (BtorNode *) exp;
}

static BtorNode *
new_node (Btor *btor, BtorNodeKind kind, uint32_t arity, BtorNode *e[])
{
  assert (btor);
  assert (arity > 0);
  assert (arity <= 3);
  assert (btor_is_binary_node_kind (kind) || btor_is_ternary_node_kind (kind));
  assert (e);

#ifndef NDEBUG
  if (btor_opt_get (btor, BTOR_OPT_SORT_EXP) > 0
      && btor_is_binary_commutative_node_kind (kind))
    assert (arity == 2), assert (BTOR_REAL_ADDR_NODE (e[0])->id
                                 <= BTOR_REAL_ADDR_NODE (e[1])->id);
#endif

  uint32_t i;
  BtorBVNode *exp;
  BtorSortId sort;

#ifdef NDEBUG
  for (i = 0; i < arity; i++)
  {
    assert (e[i]);
    assert (btor == BTOR_REAL_ADDR_NODE (e[i])->btor);
  }
#endif

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, kind);
  exp->bytes = sizeof (*exp);
  exp->arity = arity;
  setup_node_and_add_to_id_table (btor, exp);

  switch (kind)
  {
    case BTOR_COND_NODE:
      sort = btor_sort_copy (btor, btor_exp_get_sort_id (e[1]));
      break;

    case BTOR_CONCAT_NODE:
      sort = btor_sort_bitvec (
          btor,
          btor_get_exp_width (btor, e[0]) + btor_get_exp_width (btor, e[1]));
      break;

    case BTOR_FUN_EQ_NODE:
    case BTOR_BV_EQ_NODE:
    case BTOR_ULT_NODE: sort = btor_sort_bool (btor); break;

    case BTOR_APPLY_NODE:
      sort = btor_sort_copy (
          btor, btor_sort_fun_get_codomain (btor, btor_exp_get_sort_id (e[0])));
      break;

    default:
      assert (kind == BTOR_AND_NODE || kind == BTOR_ADD_NODE
              || kind == BTOR_MUL_NODE || kind == BTOR_SLL_NODE
              || kind == BTOR_SRL_NODE || kind == BTOR_UDIV_NODE
              || kind == BTOR_UREM_NODE);
      sort = btor_sort_copy (btor, btor_exp_get_sort_id (e[0]));
  }

  btor_exp_set_sort_id ((BtorNode *) exp, sort);

  for (i = 0; i < arity; i++)
    connect_child_exp (btor, (BtorNode *) exp, e[i], i);

  if (kind == BTOR_FUN_EQ_NODE)
  {
    assert (!btor_hashptr_table_get (btor->feqs, exp));
    btor_hashptr_table_add (btor->feqs, exp)->data.as_int = 0;
  }

  return (BtorNode *) exp;
}

/*------------------------------------------------------------------------*/

static BtorNode *
create_exp (Btor *btor, BtorNodeKind kind, uint32_t arity, BtorNode *e[])
{
  assert (btor);
  assert (kind);
  assert (arity > 0);
  assert (arity <= 3);
  assert (e);

  uint32_t i;
  uint32_t lambda_hash;
  BtorNode **lookup, *simp_e[3];
  BtorIntHashTable *params = 0;

  for (i = 0; i < arity; i++)
  {
    assert (BTOR_REAL_ADDR_NODE (e[i])->btor == btor);
    simp_e[i] = btor_simplify_exp (btor, e[i]);
  }

  /* collect params only for function bodies */
  if (kind == BTOR_LAMBDA_NODE && !btor_is_lambda_node (e[1]))
    params = btor_hashint_table_new (btor->mm);

  lookup = find_exp (btor, kind, simp_e, arity, &lambda_hash, params);
  if (!*lookup)
  {
    if (BTOR_FULL_UNIQUE_TABLE (btor->nodes_unique_table))
    {
      enlarge_nodes_unique_table (btor);
      lookup = find_exp (btor, kind, simp_e, arity, &lambda_hash, 0);
    }

    switch (kind)
    {
      case BTOR_LAMBDA_NODE:
        assert (arity == 2);
        *lookup = new_lambda_exp_node (btor, simp_e[0], simp_e[1]);
        btor_hashptr_table_get (btor->lambdas, *lookup)->data.as_int =
            lambda_hash;
        if (params)
        {
          if (params->count > 0)
          {
            btor_hashptr_table_add (btor->parameterized, *lookup)->data.as_ptr =
                params;
            (*lookup)->parameterized = 1;
          }
          else
            btor_hashint_table_delete (params);
        }
        break;
      case BTOR_ARGS_NODE:
        *lookup = new_args_exp_node (btor, arity, simp_e);
        break;
      default: *lookup = new_node (btor, kind, arity, simp_e);
    }
    assert (btor->nodes_unique_table.num_elements < INT_MAX);
    btor->nodes_unique_table.num_elements++;
    (*lookup)->unique = 1;
  }
  else
  {
    inc_exp_ref_counter (btor, *lookup);
    if (params) btor_hashint_table_delete (params);
  }
  assert (BTOR_IS_REGULAR_NODE (*lookup));
  return *lookup;
}

/*------------------------------------------------------------------------*/

BtorNode *
btor_node_create_const (Btor *btor, const BtorBitVector *bits)
{
  assert (btor);
  assert (bits);

  bool inv;
  BtorBitVector *lookupbits;
  BtorNode **lookup;

  /* normalize constants, constants are always even */
  if (btor_bv_get_bit (bits, 0))
  {
    lookupbits = btor_bv_not (btor->mm, bits);
    inv        = true;
  }
  else
  {
    lookupbits = btor_bv_copy (btor->mm, bits);
    inv        = false;
  }

  lookup = find_const_exp (btor, lookupbits);
  if (!*lookup)
  {
    if (BTOR_FULL_UNIQUE_TABLE (btor->nodes_unique_table))
    {
      enlarge_nodes_unique_table (btor);
      lookup = find_const_exp (btor, lookupbits);
    }
    *lookup = new_const_exp_node (btor, lookupbits);
    assert (btor->nodes_unique_table.num_elements < INT_MAX);
    btor->nodes_unique_table.num_elements += 1;
    (*lookup)->unique = 1;
  }
  else
    inc_exp_ref_counter (btor, *lookup);

  assert (BTOR_IS_REGULAR_NODE (*lookup));

  btor_bv_free (btor->mm, lookupbits);

  if (inv) return BTOR_INVERT_NODE (*lookup);
  return *lookup;
}

BtorNode *
btor_node_create_var (Btor *btor, BtorSortId sort, const char *symbol)
{
  assert (btor);
  assert (sort);
  assert (btor_sort_is_bitvec (btor, sort));
  assert (!symbol || !btor_hashptr_table_get (btor->symbols, (char *) symbol));

  BtorBVVarNode *exp;

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_BV_VAR_NODE);
  exp->bytes = sizeof *exp;
  setup_node_and_add_to_id_table (btor, exp);
  btor_exp_set_sort_id ((BtorNode *) exp, btor_sort_copy (btor, sort));
  (void) btor_hashptr_table_add (btor->bv_vars, exp);
  if (symbol) btor_set_symbol_exp (btor, (BtorNode *) exp, symbol);
  return (BtorNode *) exp;
}

BtorNode *
btor_node_create_uf (Btor *btor, BtorSortId sort, const char *symbol)
{
  assert (btor);
  assert (sort);
  assert (!symbol || !btor_hashptr_table_get (btor->symbols, (char *) symbol));

  BtorUFNode *exp;

  assert (btor_sort_is_fun (btor, sort));
  assert (btor_sort_is_bitvec (btor, btor_sort_fun_get_codomain (btor, sort))
          || btor_sort_is_bool (btor, btor_sort_fun_get_codomain (btor, sort)));

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_UF_NODE);
  exp->bytes = sizeof (*exp);
  btor_exp_set_sort_id ((BtorNode *) exp, btor_sort_copy (btor, sort));
  setup_node_and_add_to_id_table (btor, exp);
  (void) btor_hashptr_table_add (btor->ufs, exp);
  if (symbol) btor_set_symbol_exp (btor, (BtorNode *) exp, symbol);
  return (BtorNode *) exp;
}

BtorNode *
btor_node_create_param (Btor *btor, BtorSortId sort, const char *symbol)
{
  assert (btor);
  assert (sort);
  assert (btor_sort_is_bitvec (btor, sort));
  assert (!symbol || !btor_hashptr_table_get (btor->symbols, (char *) symbol));

  BtorParamNode *exp;

  BTOR_CNEW (btor->mm, exp);
  set_kind (btor, (BtorNode *) exp, BTOR_PARAM_NODE);
  exp->bytes         = sizeof *exp;
  exp->parameterized = 1;
  btor_exp_set_sort_id ((BtorNode *) exp, btor_sort_copy (btor, sort));
  setup_node_and_add_to_id_table (btor, exp);
  if (symbol) btor_set_symbol_exp (btor, (BtorNode *) exp, symbol);
  return (BtorNode *) exp;
}

static BtorNode *
unary_exp_slice_exp (Btor *btor, BtorNode *exp, uint32_t upper, uint32_t lower)
{
  assert (btor);
  assert (exp);
  assert (btor == BTOR_REAL_ADDR_NODE (exp)->btor);

  bool inv;
  BtorNode **lookup;

  exp = btor_simplify_exp (btor, exp);

  assert (!btor_is_fun_node (exp));
  assert (upper >= lower);
  assert (upper < btor_get_exp_width (btor, exp));

  if (btor_opt_get (btor, BTOR_OPT_REWRITE_LEVEL) > 0
      && BTOR_IS_INVERTED_NODE (exp))
  {
    inv = true;
    exp = BTOR_INVERT_NODE (exp);
  }
  else
    inv = false;

  lookup = find_slice_exp (btor, exp, upper, lower);
  if (!*lookup)
  {
    if (BTOR_FULL_UNIQUE_TABLE (btor->nodes_unique_table))
    {
      enlarge_nodes_unique_table (btor);
      lookup = find_slice_exp (btor, exp, upper, lower);
    }
    *lookup = new_slice_exp_node (btor, exp, upper, lower);
    assert (btor->nodes_unique_table.num_elements < INT_MAX);
    btor->nodes_unique_table.num_elements++;
    (*lookup)->unique = 1;
  }
  else
    inc_exp_ref_counter (btor, *lookup);
  assert (BTOR_IS_REGULAR_NODE (*lookup));
  if (inv) return BTOR_INVERT_NODE (*lookup);
  return *lookup;
}

BtorNode *
btor_slice_exp_node (Btor *btor, BtorNode *exp, uint32_t upper, uint32_t lower)
{
  exp = btor_simplify_exp (btor, exp);
  assert (btor_precond_slice_exp_dbg (btor, exp, upper, lower));
  return unary_exp_slice_exp (btor, exp, upper, lower);
}

BtorNode *
btor_and_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_AND_NODE, 2, e);
}

BtorNode *
btor_eq_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  BtorNodeKind kind;

  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_eq_exp_dbg (btor, e[0], e[1]));
  if (btor_is_fun_node (e[0]))
    kind = BTOR_FUN_EQ_NODE;
  else
    kind = BTOR_BV_EQ_NODE;
  return create_exp (btor, kind, 2, e);
}

BtorNode *
btor_add_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_ADD_NODE, 2, e);
}

BtorNode *
btor_mul_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_MUL_NODE, 2, e);
}

BtorNode *
btor_ult_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_ULT_NODE, 2, e);
}

BtorNode *
btor_sll_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_shift_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_SLL_NODE, 2, e);
}

BtorNode *
btor_srl_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_shift_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_SRL_NODE, 2, e);
}

BtorNode *
btor_udiv_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_UDIV_NODE, 2, e);
}

BtorNode *
btor_urem_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_regular_binary_bv_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_UREM_NODE, 2, e);
}

BtorNode *
btor_concat_exp_node (Btor *btor, BtorNode *e0, BtorNode *e1)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e0);
  e[1] = btor_simplify_exp (btor, e1);
  assert (btor_precond_concat_exp_dbg (btor, e[0], e[1]));
  return create_exp (btor, BTOR_CONCAT_NODE, 2, e);
}

BtorNode *
btor_cond_exp_node (Btor *btor,
                    BtorNode *e_cond,
                    BtorNode *e_if,
                    BtorNode *e_else)
{
  uint32_t i, arity;
  BtorNode *e[3], *cond, *lambda;
  BtorNodePtrStack params;
  BtorSort *sort;
  e[0] = btor_simplify_exp (btor, e_cond);
  e[1] = btor_simplify_exp (btor, e_if);
  e[2] = btor_simplify_exp (btor, e_else);
  assert (btor_precond_cond_exp_dbg (btor, e[0], e[1], e[2]));

  /* represent parameterized function conditionals (with parameterized
   * functions) as parameterized function
   * -> gets beta reduced in btor_apply_exp_node */
  if (btor_is_fun_node (e[1]) && (e[1]->parameterized || e[2]->parameterized))
  {
    BTOR_INIT_STACK (btor->mm, params);
    assert (btor_sort_is_fun (btor, btor_exp_get_sort_id (e[1])));
    arity = btor_get_fun_arity (btor, e[1]);
    sort  = btor_sort_get_by_id (btor, btor_exp_get_sort_id (e[1]));
    assert (sort->fun.domain->kind == BTOR_TUPLE_SORT);
    assert (sort->fun.domain->tuple.num_elements == arity);
    for (i = 0; i < arity; i++)
      BTOR_PUSH_STACK (
          params,
          btor_param_exp (btor, sort->fun.domain->tuple.elements[i]->id, 0));
    e[1]   = btor_apply_exps (btor, params.start, arity, e[1]);
    e[2]   = btor_apply_exps (btor, params.start, arity, e[2]);
    cond   = create_exp (btor, BTOR_COND_NODE, 3, e);
    lambda = btor_fun_exp (btor, params.start, arity, cond);
    while (!BTOR_EMPTY_STACK (params))
      btor_release_exp (btor, BTOR_POP_STACK (params));
    btor_release_exp (btor, e[1]);
    btor_release_exp (btor, e[2]);
    btor_release_exp (btor, cond);
    BTOR_RELEASE_STACK (params);
    return lambda;
  }
  return create_exp (btor, BTOR_COND_NODE, 3, e);
}

#if 0
BtorNode *
btor_bv_cond_exp_node (Btor * btor, BtorNode * e_cond, BtorNode * e_if,
		       BtorNode * e_else)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e_cond)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_if)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_else)->btor);

  if (btor_opt_get (btor, BTOR_OPT_REWRITE_LEVEL) > 0)
    return btor_rewrite_ternary_exp (btor, BTOR_BCOND_NODE, e_cond, e_if, e_else);

  return btor_cond_exp_node (btor, e_cond, e_if, e_else);
}

// TODO: arbitrary conditionals on functions
BtorNode *
btor_array_cond_exp_node (Btor * btor, BtorNode * e_cond, BtorNode * e_if,
			  BtorNode * e_else)
{
  assert (btor == BTOR_REAL_ADDR_NODE (e_cond)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_if)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (e_else)->btor);

  BtorNode *cond, *param, *lambda, *app_if, *app_else;

  e_cond = btor_simplify_exp (btor, e_cond);
  e_if = btor_simplify_exp (btor, e_if);
  e_else = btor_simplify_exp (btor, e_else);

  assert (BTOR_IS_REGULAR_NODE (e_if));
  assert (btor_is_fun_node (e_if));
  assert (BTOR_IS_REGULAR_NODE (e_else));
  assert (btor_is_fun_node (e_else));

  param = btor_param_exp (btor, btor_exp_get_sort_id (e_if), 0);
  app_if = btor_apply_exps (btor, &param, 1, e_if); 
  app_else = btor_apply_exps (btor, &param, 1, e_else);
  cond = btor_bv_cond_exp_node (btor, e_cond, app_if, app_else); 
  lambda = btor_lambda_exp (btor, param, cond); 
  lambda->is_array = 1;

  btor_release_exp (btor, param);
  btor_release_exp (btor, app_if);
  btor_release_exp (btor, app_else);
  btor_release_exp (btor, cond);
  
  return lambda;
}
#endif

/* more than 4 children are not possible as we only have 2 bit for storing
 * the position in the parent pointers */
#define ARGS_MAX_NUM_CHILDREN 3

BtorNode *
btor_node_create_args (Btor *btor, BtorNode *args[], uint32_t argc)
{
  assert (btor);
  assert (argc > 0);
  assert (args);

  int64_t i, cur_argc, cnt_args, rem_free, num_args;
  BtorNode *e[ARGS_MAX_NUM_CHILDREN];
  BtorNode *result = 0, *last = 0;

  /* arguments fit in one args node */
  if (argc <= ARGS_MAX_NUM_CHILDREN)
  {
    num_args = 1;
    rem_free = ARGS_MAX_NUM_CHILDREN - argc;
    cur_argc = argc;
  }
  /* arguments have to be split into several args nodes.
   * compute number of required args nodes */
  else
  {
    rem_free = argc % (ARGS_MAX_NUM_CHILDREN - 1);
    num_args = argc / (ARGS_MAX_NUM_CHILDREN - 1);
    /* we can store at most 1 more element into 'num_args' nodes
     * without needing an additional args node */
    if (rem_free > 1) num_args += 1;

    assert (num_args > 1);
    /* compute number of arguments in last args node */
    cur_argc = argc - (num_args - 1) * (ARGS_MAX_NUM_CHILDREN - 1);
  }
  cnt_args = cur_argc - 1;

  /* split up args in 'num_args' of args nodes */
  for (i = argc - 1; i >= 0; i--)
  {
    assert (cnt_args >= 0);
    assert (cnt_args <= ARGS_MAX_NUM_CHILDREN);
    assert (!btor_is_fun_node (args[i]));
    assert (btor == BTOR_REAL_ADDR_NODE (args[i])->btor);
    e[cnt_args] = btor_simplify_exp (btor, args[i]);
    cnt_args -= 1;

    assert (i > 0 || cnt_args < 0);
    if (cnt_args < 0)
    {
      result = create_exp (btor, BTOR_ARGS_NODE, cur_argc, e);

      /* init for next iteration */
      cur_argc    = ARGS_MAX_NUM_CHILDREN;
      cnt_args    = cur_argc - 1;
      e[cnt_args] = result;
      cnt_args -= 1;

      if (last) btor_release_exp (btor, last);

      last = result;
    }
  }

  assert (result);
  return result;
}

BtorNode *
btor_apply_exp_node (Btor *btor, BtorNode *fun, BtorNode *args)
{
  assert (btor);
  assert (fun);
  assert (args);
  assert (btor == BTOR_REAL_ADDR_NODE (fun)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (args)->btor);
  assert (btor_precond_apply_exp_dbg (btor, fun, args));

  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, fun);
  e[1] = btor_simplify_exp (btor, args);

  assert (BTOR_IS_REGULAR_NODE (e[0]));
  assert (BTOR_IS_REGULAR_NODE (e[1]));
  assert (btor_is_fun_node (e[0]));
  assert (btor_is_args_node (e[1]));

  /* eliminate nested functions */
  if (btor_is_lambda_node (e[0]) && e[0]->parameterized)
  {
    btor_beta_assign_args (btor, e[0], args);
    BtorNode *result = btor_beta_reduce_bounded (btor, e[0], 1);
    btor_beta_unassign_params (btor, e[0]);
    return result;
  }
  assert (!btor_is_fun_cond_node (e[0])
          || (!e[0]->e[1]->parameterized && !e[0]->e[2]->parameterized));
  return create_exp (btor, BTOR_APPLY_NODE, 2, e);
}

BtorNode *
btor_lambda_exp_node (Btor *btor, BtorNode *e_param, BtorNode *e_exp)
{
  BtorNode *e[2];
  e[0] = btor_simplify_exp (btor, e_param);
  e[1] = btor_simplify_exp (btor, e_exp);
  return create_exp (btor, BTOR_LAMBDA_NODE, 2, e);
}

/*========================================================================*/

BtorNodePair *
btor_new_exp_pair (Btor *btor, BtorNode *exp1, BtorNode *exp2)
{
  assert (btor);
  assert (exp1);
  assert (exp2);
  assert (btor == BTOR_REAL_ADDR_NODE (exp1)->btor);
  assert (btor == BTOR_REAL_ADDR_NODE (exp2)->btor);

  uint32_t id1, id2;
  BtorNodePair *result;

  BTOR_NEW (btor->mm, result);
  id1 = btor_exp_get_id (exp1);
  id2 = btor_exp_get_id (exp2);
  if (id2 < id1)
  {
    result->exp1 = btor_copy_exp (btor, exp2);
    result->exp2 = btor_copy_exp (btor, exp1);
  }
  else
  {
    result->exp1 = btor_copy_exp (btor, exp1);
    result->exp2 = btor_copy_exp (btor, exp2);
  }
  return result;
}

void
btor_delete_exp_pair (Btor *btor, BtorNodePair *pair)
{
  assert (btor);
  assert (pair);
  btor_release_exp (btor, pair->exp1);
  btor_release_exp (btor, pair->exp2);
  BTOR_DELETE (btor->mm, pair);
}

uint32_t
btor_hash_exp_pair (const BtorNodePair *pair)
{
  uint32_t result;
  assert (pair);
  result = (uint32_t) BTOR_REAL_ADDR_NODE (pair->exp1)->id;
  result += (uint32_t) BTOR_REAL_ADDR_NODE (pair->exp2)->id;
  result *= 7334147u;
  return result;
}

int32_t
btor_compare_exp_pair (const BtorNodePair *pair1, const BtorNodePair *pair2)
{
  assert (pair1);
  assert (pair2);

  int32_t result;

  result = btor_exp_get_id (pair1->exp1);
  result -= btor_exp_get_id (pair2->exp1);
  if (result != 0) return result;
  result = btor_exp_get_id (pair1->exp2);
  result -= btor_exp_get_id (pair2->exp2);
  return result;
}
