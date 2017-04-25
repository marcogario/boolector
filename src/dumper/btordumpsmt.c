/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2007-2009 Robert Daniel Brummayer.
 *  Copyright (C) 2007-2013 Armin Biere.
 *  Copyright (C) 2012-2016 Mathias Preiner.
 *  Copyright (C) 2012-2017 Aina Niemetz.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "btorexit.h"
#include "btorsort.h"
#ifndef NDEBUG
#include "btorclone.h"
#endif
#include "utils/btorhashptr.h"
#include "utils/btornodeiter.h"

#include "btordumpsmt.h"

#include <ctype.h>
#include <limits.h>

struct BtorSMTDumpContext
{
  Btor *btor;
  BtorPtrHashTable *dump;
  BtorPtrHashTable *dumped;
  BtorPtrHashTable *boolean;
  BtorPtrHashTable *stores;
  BtorPtrHashTable *idtab;
  BtorPtrHashTable *roots;
  BtorPtrHashTable *const_cache;
  FILE *file;
  int maxid;
  int pretty_print;
  int open_lets;
};

typedef struct BtorSMTDumpContext BtorSMTDumpContext;

static BtorSMTDumpContext *
new_smt_dump_context (Btor *btor, FILE *file)
{
  BtorSMTDumpContext *sdc;
  BTOR_CNEW (btor->mm, sdc);

  sdc->btor        = btor;
  sdc->dump        = btor_hashptr_table_new (btor->mm,
                                      (BtorHashPtr) btor_hash_exp_by_id,
                                      (BtorCmpPtr) btor_compare_exp_by_id);
  sdc->dumped      = btor_hashptr_table_new (btor->mm,
                                        (BtorHashPtr) btor_hash_exp_by_id,
                                        (BtorCmpPtr) btor_compare_exp_by_id);
  sdc->boolean     = btor_hashptr_table_new (btor->mm,
                                         (BtorHashPtr) btor_hash_exp_by_id,
                                         (BtorCmpPtr) btor_compare_exp_by_id);
  sdc->stores      = btor_hashptr_table_new (btor->mm,
                                        (BtorHashPtr) btor_hash_exp_by_id,
                                        (BtorCmpPtr) btor_compare_exp_by_id);
  sdc->idtab       = btor_hashptr_table_new (btor->mm,
                                       (BtorHashPtr) btor_hash_exp_by_id,
                                       (BtorCmpPtr) btor_compare_exp_by_id);
  sdc->const_cache = btor_hashptr_table_new (
      btor->mm, (BtorHashPtr) btor_bv_hash, (BtorCmpPtr) btor_bv_compare);
  /* use pointer for hashing and comparison */
  sdc->roots        = btor_hashptr_table_new (btor->mm, 0, 0);
  sdc->file         = file;
  sdc->maxid        = 1;
  sdc->pretty_print = btor_opt_get (btor, BTOR_OPT_PRETTY_PRINT);
  return sdc;
}

static void
delete_smt_dump_context (BtorSMTDumpContext *sdc)
{
  BtorPtrHashTableIterator it;

  btor_hashptr_table_delete (sdc->dump);
  btor_hashptr_table_delete (sdc->dumped);
  btor_hashptr_table_delete (sdc->boolean);
  btor_hashptr_table_delete (sdc->stores);
  btor_hashptr_table_delete (sdc->idtab);

  btor_iter_hashptr_init (&it, sdc->roots);
  while (btor_iter_hashptr_has_next (&it))
    btor_release_exp (sdc->btor, btor_iter_hashptr_next (&it));
  btor_hashptr_table_delete (sdc->roots);

  btor_iter_hashptr_init (&it, sdc->const_cache);
  while (btor_iter_hashptr_has_next (&it))
  {
    assert (it.bucket->data.as_str);
    btor_mem_freestr (sdc->btor->mm, it.bucket->data.as_str);
    btor_bv_free (sdc->btor->mm,
                  (BtorBitVector *) btor_iter_hashptr_next (&it));
  }
  btor_hashptr_table_delete (sdc->const_cache);
  BTOR_DELETE (sdc->btor->mm, sdc);
}

static void
add_root_to_smt_dump_context (BtorSMTDumpContext *sdc, BtorNode *root)
{
  if (!btor_hashptr_table_get (sdc->roots, root))
    btor_hashptr_table_add (sdc->roots, btor_copy_exp (sdc->btor, root));
}

static int
cmp_node_id (const void *p, const void *q)
{
  BtorNode *a = *(BtorNode **) p;
  BtorNode *b = *(BtorNode **) q;
  return a->id - b->id;
}

static int
smt_id (BtorSMTDumpContext *sdc, BtorNode *exp)
{
  assert (sdc);
  assert (exp);
  assert (BTOR_IS_REGULAR_NODE (exp));

  BtorPtrHashBucket *b;
  int32_t id;

  if (sdc->pretty_print)
  {
    b = btor_hashptr_table_get (sdc->idtab, exp);

    if (!b)
    {
      b              = btor_hashptr_table_add (sdc->idtab, exp);
      b->data.as_int = sdc->maxid++;
    }
    return b->data.as_int;
  }
  id = btor_exp_get_btor_id (exp);
  if (id) return id;
  return exp->id;
}

static void
dump_smt_id (BtorSMTDumpContext *sdc, BtorNode *exp)
{
  assert (sdc);
  assert (exp);

  const char *type, *sym;

  exp = BTOR_REAL_ADDR_NODE (exp);

  switch (exp->kind)
  {
    case BTOR_BV_VAR_NODE: type = "v"; goto DUMP_SYMBOL;

    case BTOR_PARAM_NODE:
      type = "p";
    DUMP_SYMBOL:
      sym = btor_get_symbol_exp (sdc->btor, exp);
      if (sym && !isdigit ((int) sym[0]))
      {
        fputs (sym, sdc->file);
        return;
      }
      break;

    case BTOR_UF_NODE: type = "uf"; goto DUMP_SYMBOL;

    case BTOR_LAMBDA_NODE: type = "f"; goto DUMP_SYMBOL;

    default: type = "$e";
  }

  fprintf (sdc->file, "%s%d", type, smt_id (sdc, exp));
}

static bool
is_boolean (BtorSMTDumpContext *sdc, BtorNode *exp)
{
  exp = BTOR_REAL_ADDR_NODE (exp);
  return btor_hashptr_table_get (sdc->boolean, exp) != 0;
}

void
btor_dumpsmt_dump_const_value (Btor *btor,
                               const BtorBitVector *bits,
                               int base,
                               FILE *file)
{
  assert (btor);
  assert (bits);
  assert (base == BTOR_OUTPUT_BASE_BIN || base == BTOR_OUTPUT_BASE_DEC
          || base == BTOR_OUTPUT_BASE_HEX);

  char *val;

  /* SMT-LIB v1.2 only supports decimal output */
  if (base == BTOR_OUTPUT_BASE_DEC)
  {
    val = btor_bv_to_dec_char (btor->mm, bits);
    fprintf (file, "(_ bv%s %d)", val, bits->width);
  }
  else if (base == BTOR_OUTPUT_BASE_HEX && bits->width % 4 == 0)
  {
    val = btor_bv_to_hex_char (btor->mm, bits);
    fprintf (file, "#x%s", val);
  }
  else
  {
    val = btor_bv_to_char (btor->mm, bits);
    fprintf (file, "#b%s", val);
  }
  btor_mem_freestr (btor->mm, val);
}

static void
dump_const_value_aux_smt (BtorSMTDumpContext *sdc, BtorBitVector *bits)
{
  assert (sdc);
  assert (bits);

  int base;
  FILE *file;
  char *val;
  BtorPtrHashBucket *b;

  base = btor_opt_get (sdc->btor, BTOR_OPT_OUTPUT_NUMBER_FORMAT);
  file = sdc->file;

  /* converting consts to decimal/hex is costly. we now always dump the value of
   * constants. in order to avoid computing the same value again we cache
   * the result of the first computation and print the cached value in
   * subsequent calls. */
  if (base == BTOR_OUTPUT_BASE_DEC)
  {
    if ((b = btor_hashptr_table_get (sdc->const_cache, bits)))
    {
      val = b->data.as_str;
      assert (val);
    }
    else
    {
      val = btor_bv_to_dec_char (sdc->btor->mm, bits);
      btor_hashptr_table_add (sdc->const_cache,
                              btor_bv_copy (sdc->btor->mm, bits))
          ->data.as_str = val;
    }
    fprintf (file, "(_ bv%s %d)", val, bits->width);
  }
  else if (base == BTOR_OUTPUT_BASE_HEX && bits->width % 4 == 0)
  {
    if ((b = btor_hashptr_table_get (sdc->const_cache, bits)))
    {
      val = b->data.as_str;
      assert (val);
    }
    else
    {
      val = btor_bv_to_hex_char (sdc->btor->mm, bits);
      btor_hashptr_table_add (sdc->const_cache,
                              btor_bv_copy (sdc->btor->mm, bits))
          ->data.as_str = val;
    }
    fprintf (file, "#x%s", val);
  }
  else
    btor_dumpsmt_dump_const_value (sdc->btor, bits, base, file);
}

void
btor_dumpsmt_dump_sort (BtorSort *sort, FILE *file)
{
  unsigned i;
  const char *fmt;

  switch (sort->kind)
  {
    case BTOR_BOOL_SORT: fputs ("Bool", file); break;

    case BTOR_BITVEC_SORT:
      fmt = "(_ BitVec %d)";
      fprintf (file, fmt, sort->bitvec.width);
      break;

    case BTOR_ARRAY_SORT:
      fmt = "(Array (_ BitVec %d) (_ BitVec %d))";
      assert (sort->array.index->kind == BTOR_BITVEC_SORT);
      assert (sort->array.element->kind == BTOR_BITVEC_SORT);
      fprintf (file,
               fmt,
               sort->array.index->bitvec.width,
               sort->array.element->bitvec.width);
      break;

    case BTOR_FUN_SORT:
      /* print domain */
      fputc ('(', file);
      if (sort->fun.domain->kind == BTOR_TUPLE_SORT)
      {
        for (i = 0; i < sort->fun.domain->tuple.num_elements; i++)
        {
          btor_dumpsmt_dump_sort (sort->fun.domain->tuple.elements[i], file);
          if (i < sort->fun.domain->tuple.num_elements - 1) fputc (' ', file);
        }
      }
      else
        btor_dumpsmt_dump_sort (sort->fun.domain, file);
      fputc (')', file);
      fputc (' ', file);

      /* print co-domain */
      btor_dumpsmt_dump_sort (sort->fun.codomain, file);
      break;

    default: assert (0);
  }
}

void
btor_dumpsmt_dump_sort_node (BtorNode *exp, FILE *file)
{
  assert (exp);
  assert (file);

  BtorSort *sort;

  exp  = BTOR_REAL_ADDR_NODE (exp);
  sort = btor_sort_get_by_id (exp->btor, btor_exp_get_sort_id (exp));
  btor_dumpsmt_dump_sort (sort, file);
}

#if 0
static void
extract_store (BtorSMTDumpContext * sdc, BtorNode * exp,
	       BtorNode ** index, BtorNode ** value, BtorNode ** array)
{
  BtorNode *ite, *eq, *apply;

  if (!btor_is_lambda_node (exp))
    return;

  if (((BtorLambdaNode *) exp)->num_params != 1)
    return;

  if (!btor_is_bv_cond_node (exp->e[1]))
    return;

  ite = exp->e[1];
  if (BTOR_IS_INVERTED_NODE (ite))
    return;

  if (!btor_is_bv_eq_node (ite->e[0]))
    return;

  /* check ite condition */
  eq = ite->e[0];
  if (BTOR_IS_INVERTED_NODE (eq))
    return;

  if (!eq->parameterized)
    return;

  /* check if branch */
  if (BTOR_REAL_ADDR_NODE (ite->e[1])->parameterized)
    return;

  /* check else branch */
  if (!BTOR_REAL_ADDR_NODE (ite->e[2])->parameterized)
    return;

  if (!btor_is_apply_node (ite->e[2]))
    return;

  apply = ite->e[2];
  if (BTOR_IS_INVERTED_NODE (apply))
    return;

  if (!btor_is_uf_array_node (apply->e[0])
      && !btor_hashptr_table_get (sdc->stores, apply->e[0]))
    return;

  if (!btor_is_param_node (apply->e[1]->e[0]))
    return;

  *index = BTOR_REAL_ADDR_NODE (eq->e[0])->parameterized ? eq->e[1] : eq->e[0];
  *value = ite->e[1];
  *array = apply->e[0];
}
#endif

#define PUSH_DUMP_NODE(                                      \
    exp, expect_bv, expect_bool, add_space, zero_ext, depth) \
  {                                                          \
    BTOR_PUSH_STACK (dump, exp);                             \
    BTOR_PUSH_STACK (expect_bv_stack, expect_bv);            \
    BTOR_PUSH_STACK (expect_bool_stack, expect_bool);        \
    BTOR_PUSH_STACK (add_space_stack, add_space);            \
    BTOR_PUSH_STACK (zero_extend_stack, zero_ext);           \
    BTOR_PUSH_STACK (depth_stack, depth);                    \
  }

static const char *g_kind2smt[BTOR_NUM_OPS_NODE] = {
    [BTOR_INVALID_NODE] = "invalid", [BTOR_BV_CONST_NODE] = "const",
    [BTOR_BV_VAR_NODE] = "var",      [BTOR_PARAM_NODE] = "param",
    [BTOR_SLICE_NODE] = "extract",   [BTOR_AND_NODE] = "bvand",
    [BTOR_FUN_EQ_NODE] = "=",        [BTOR_BV_EQ_NODE] = "=",
    [BTOR_ADD_NODE] = "bvadd",       [BTOR_MUL_NODE] = "bvmul",
    [BTOR_ULT_NODE] = "bvult",       [BTOR_SLL_NODE] = "bvshl",
    [BTOR_SRL_NODE] = "bvlshr",      [BTOR_UDIV_NODE] = "bvudiv",
    [BTOR_UREM_NODE] = "bvurem",     [BTOR_CONCAT_NODE] = "concat",
    [BTOR_APPLY_NODE] = "apply",     [BTOR_LAMBDA_NODE] = "lambda",
    [BTOR_COND_NODE] = "ite",        [BTOR_ARGS_NODE] = "args",
    [BTOR_UF_NODE] = "uf",           [BTOR_PROXY_NODE] = "proxy"};

static void
collect_and_children (BtorSMTDumpContext *sdc,
                      BtorNode *exp,
                      BtorNodePtrStack *children)
{
  assert (children);
  assert (BTOR_EMPTY_STACK (*children));
  assert (btor_is_and_node (exp));

  bool skip;
  int i, id;
  BtorNode *cur, *real_cur;
  BtorNodePtrQueue visit;
  BtorPtrHashBucket *b;
  BtorIntHashTable *cache;

  cache = btor_hashint_table_new (sdc->btor->mm);

  /* get children of multi-input and */
  BTOR_INIT_QUEUE (sdc->btor->mm, visit);
  for (i = 0; i < BTOR_REAL_ADDR_NODE (exp)->arity; i++)
    BTOR_ENQUEUE (visit, BTOR_REAL_ADDR_NODE (exp)->e[i]);
  while (!BTOR_EMPTY_QUEUE (visit))
  {
    cur      = BTOR_DEQUEUE (visit);
    real_cur = BTOR_REAL_ADDR_NODE (cur);
    id       = btor_exp_get_id (cur);

    skip = btor_hashint_table_contains (cache, id);
    if (!skip)
    {
      btor_hashint_table_add (cache, id);
      b = btor_hashptr_table_get (sdc->dump, real_cur);
    }
    else
      b = 0;

    if (!btor_is_and_node (real_cur) || (b && b->data.as_int > 1)
        || BTOR_IS_INVERTED_NODE (cur) || skip)
    {
      BTOR_PUSH_STACK (*children, cur);
      continue;
    }

    assert (!btor_hashptr_table_get (sdc->dumped, real_cur));
    btor_hashptr_table_add (sdc->dumped, real_cur);
    for (i = 0; i < real_cur->arity; i++) BTOR_ENQUEUE (visit, real_cur->e[i]);
  }
  BTOR_RELEASE_QUEUE (visit);
  btor_hashint_table_delete (cache);
}

static void
recursively_dump_exp_smt (BtorSMTDumpContext *sdc,
                          BtorNode *exp,
                          int expect_bv,
                          unsigned depth_limit)
{
  assert (sdc);
  assert (exp);
  assert (btor_hashptr_table_get (sdc->dump, BTOR_REAL_ADDR_NODE (exp)));

  unsigned depth;
  int pad, i, is_bool, add_space, zero_extend, expect_bool;
  BtorBitVector *bits;
  const char *op, *fmt;
  BtorNode *arg, *real_exp;
  BtorArgsIterator it;
  BtorNodePtrStack dump, args;
  BtorIntStack expect_bv_stack, expect_bool_stack, depth_stack;
  BtorIntStack add_space_stack, zero_extend_stack;
  BtorPtrHashTable *visited;
  BtorMemMgr *mm;

  mm      = sdc->btor->mm;
  visited = btor_hashptr_table_new (mm, 0, 0);
  BTOR_INIT_STACK (mm, args);
  BTOR_INIT_STACK (mm, dump);
  BTOR_INIT_STACK (mm, expect_bv_stack);
  BTOR_INIT_STACK (mm, expect_bool_stack);
  BTOR_INIT_STACK (mm, add_space_stack);
  BTOR_INIT_STACK (mm, zero_extend_stack);
  BTOR_INIT_STACK (mm, depth_stack);

  PUSH_DUMP_NODE (exp, expect_bv, 0, 0, 0, 0);
  while (!BTOR_EMPTY_STACK (dump))
  {
    assert (!BTOR_EMPTY_STACK (expect_bv_stack));
    assert (!BTOR_EMPTY_STACK (expect_bool_stack));
    assert (!BTOR_EMPTY_STACK (add_space_stack));
    assert (!BTOR_EMPTY_STACK (zero_extend_stack));
    assert (!BTOR_EMPTY_STACK (depth_stack));
    depth       = BTOR_POP_STACK (depth_stack);
    exp         = BTOR_POP_STACK (dump);
    expect_bv   = BTOR_POP_STACK (expect_bv_stack);
    expect_bool = BTOR_POP_STACK (expect_bool_stack);
    add_space   = BTOR_POP_STACK (add_space_stack);
    zero_extend = BTOR_POP_STACK (zero_extend_stack);
    real_exp    = BTOR_REAL_ADDR_NODE (exp);
    is_bool     = is_boolean (sdc, real_exp);
    assert (!expect_bv || !expect_bool);
    assert (!expect_bool || !expect_bv);

    /* open s-expression */
    if (!btor_hashptr_table_get (visited, real_exp))
    {
      if (add_space) fputc (' ', sdc->file);

      /* wrap node with zero_extend */
      if (zero_extend)
      {
        fmt = " ((_ zero_extend %d) ";
        fprintf (sdc->file, fmt, zero_extend);
      }

      /* always print constants */
      if (btor_is_bv_const_node (real_exp))
      {
        if (exp == sdc->btor->true_exp && !expect_bv)
          fputs ("true", sdc->file);
        else if (exp == BTOR_INVERT_NODE (sdc->btor->true_exp) && !expect_bv)
          fputs ("false", sdc->file);
        else if (BTOR_IS_INVERTED_NODE (exp))
        {
          bits = btor_bv_not (sdc->btor->mm, btor_const_get_bits (real_exp));
          dump_const_value_aux_smt (sdc, bits);
          btor_bv_free (sdc->btor->mm, bits);
        }
        else
        {
          dump_const_value_aux_smt (sdc, btor_const_get_bits (real_exp));
        }

        /* close zero extend */
        if (zero_extend) fputc (')', sdc->file);
        continue;
      }

      /* wrap non-bool node and make it bool */
      if (expect_bool && !is_bool)
      {
        fputs ("(= ", sdc->file);
        bits = btor_bv_one (sdc->btor->mm, 1);
        dump_const_value_aux_smt (sdc, bits);
        btor_bv_free (sdc->btor->mm, bits);
        fputc (' ', sdc->file);
      }

      /* wrap node with bvnot/not */
      if (BTOR_IS_INVERTED_NODE (exp))
        fputs (expect_bv || !is_bool ? "(bvnot " : "(not ", sdc->file);

      /* wrap bool node and make it a bit vector expression */
      if (is_bool && expect_bv) fputs ("(ite ", sdc->file);

      if (btor_hashptr_table_get (sdc->dumped, real_exp)
          || btor_is_lambda_node (real_exp) || btor_is_uf_node (real_exp))
      {
#ifndef NDEBUG
        BtorPtrHashBucket *b;
        b = btor_hashptr_table_get (sdc->dump, real_exp);
        assert (b);
        /* functions and variables are declared separately */
        assert (btor_is_lambda_node (real_exp) || btor_is_uf_node (real_exp)
                || btor_is_bv_var_node (real_exp)
                || btor_is_param_node (real_exp) || b->data.as_int > 1);
#endif
        dump_smt_id (sdc, exp);
        goto CLOSE_WRAPPER;
      }

      if (depth_limit && depth >= depth_limit)
      {
        fprintf (sdc->file, "%s_%d", g_kind2smt[real_exp->kind], real_exp->id);
        goto CLOSE_WRAPPER;
      }

      PUSH_DUMP_NODE (exp, expect_bv, expect_bool, 0, zero_extend, depth);
      btor_hashptr_table_add (visited, real_exp);
      op = "";
      switch (real_exp->kind)
      {
        case BTOR_SLL_NODE:
        case BTOR_SRL_NODE:
          assert (!is_bool);
          op = real_exp->kind == BTOR_SRL_NODE ? "bvlshr" : "bvshl";
          assert (btor_get_exp_width (sdc->btor, real_exp) > 1);
          pad = btor_get_exp_width (sdc->btor, real_exp)
                - btor_get_exp_width (sdc->btor, real_exp->e[1]);
          PUSH_DUMP_NODE (real_exp->e[1], 1, 0, 1, pad, depth + 1);
          PUSH_DUMP_NODE (real_exp->e[0], 1, 0, 1, 0, depth + 1);
          break;

        case BTOR_COND_NODE:
          op = "ite";
          PUSH_DUMP_NODE (real_exp->e[2], !is_bool, 0, 1, 0, depth + 1);
          PUSH_DUMP_NODE (real_exp->e[1], !is_bool, 0, 1, 0, depth + 1);
          PUSH_DUMP_NODE (real_exp->e[0], 0, 1, 1, 0, depth + 1);
          break;

        case BTOR_APPLY_NODE:
          /* we need the arguments in reversed order */
          btor_iter_args_init (&it, real_exp->e[1]);
          while (btor_iter_args_has_next (&it))
          {
            arg = btor_iter_args_next (&it);
            BTOR_PUSH_STACK (args, arg);
          }
          while (!BTOR_EMPTY_STACK (args))
          {
            arg = BTOR_POP_STACK (args);
            // TODO (ma): what about bool arguments/indices
            PUSH_DUMP_NODE (arg, 1, 0, 1, 0, depth + 1);
          }
          PUSH_DUMP_NODE (real_exp->e[0], 1, 0, 0, 0, depth + 1);
          break;

#if 0
	      case BTOR_LAMBDA_NODE:
		extract_store (sdc, exp, &index, &value, &array);
		assert (index);
		assert (value);
		assert (array);
		fputs ("(store ", sdc->file);
		DUMP_EXP_SMT (array);
		fputc (' ', sdc->file);
		DUMP_EXP_SMT (index);
		fputc (' ', sdc->file);
		DUMP_EXP_SMT (value);
		fputc (')', sdc->file);
		break;
#endif

        default:
          expect_bv = 1;
          switch (real_exp->kind)
          {
            case BTOR_FUN_EQ_NODE:
            case BTOR_BV_EQ_NODE:
              op        = "=";
              expect_bv = 1;
              break;
            case BTOR_ULT_NODE:
              op        = "bvult";
              expect_bv = 1;
              break;
            case BTOR_SLICE_NODE:
              assert (!is_bool);
              op = "(_ extract ";
              break;
            case BTOR_AND_NODE:
              op        = is_bool ? "and" : "bvand";
              expect_bv = !is_bool;
              break;
            case BTOR_ADD_NODE:
              assert (!is_bool);
              op = "bvadd";
              break;
            case BTOR_MUL_NODE:
              assert (!is_bool);
              op = "bvmul";
              break;
            case BTOR_UDIV_NODE:
              assert (!is_bool);
              op = "bvudiv";
              break;
            case BTOR_UREM_NODE:
              assert (!is_bool);
              op = "bvurem";
              break;
            case BTOR_CONCAT_NODE:
              assert (!is_bool);
              op = "concat";
              break;
            default: assert (0); op = "unknown";
          }
          if (btor_is_and_node (real_exp) && is_bool)
          {
            assert (BTOR_EMPTY_STACK (args));
            collect_and_children (sdc, exp, &args);
            assert (BTOR_COUNT_STACK (args) >= 2);
            for (i = 0; i < BTOR_COUNT_STACK (args); i++)
            {
              arg = BTOR_PEEK_STACK (args, i);
              PUSH_DUMP_NODE (arg, expect_bv, 0, 1, 0, depth + 1);
            }
            BTOR_RESET_STACK (args);
          }
          else
            for (i = real_exp->arity - 1; i >= 0; i--)
              PUSH_DUMP_NODE (real_exp->e[i], expect_bv, 0, 1, 0, depth + 1);
      }

      /* open s-expression */
      assert (op);
      fprintf (sdc->file, "(%s", op);

      if (btor_is_slice_node (real_exp))
      {
        fmt = "%d %d)";
        fprintf (sdc->file,
                 fmt,
                 btor_slice_get_upper (real_exp),
                 btor_slice_get_lower (real_exp));
      }
    }
    /* close s-expression */
    else
    {
      if (!btor_hashptr_table_get (sdc->dumped, real_exp))
        btor_hashptr_table_add (sdc->dumped, real_exp);

      /* close s-expression */
      if (real_exp->arity > 0) fputc (')', sdc->file);

    CLOSE_WRAPPER:
      /* close wrappers */

      /* wrap boolean expressions in bit vector expression */
      if (is_bool && expect_bv && !btor_is_bv_const_node (real_exp))
      {
        fputc (' ', sdc->file);
        bits = btor_bv_one (sdc->btor->mm, 1);
        dump_const_value_aux_smt (sdc, bits);
        btor_bv_free (sdc->btor->mm, bits);
        fputc (' ', sdc->file);
        bits = btor_bv_new (sdc->btor->mm, 1);
        dump_const_value_aux_smt (sdc, bits);
        btor_bv_free (sdc->btor->mm, bits);
        fputc (')', sdc->file);
      }

      /* close bvnot for non-constants */
      if (BTOR_IS_INVERTED_NODE (exp) && !btor_is_bv_const_node (real_exp))
        fputc (')', sdc->file);

      /* close bool wrapper */
      if (expect_bool && !is_boolean (sdc, real_exp)) fputc (')', sdc->file);

      /* close zero extend wrapper */
      if (zero_extend) fputc (')', sdc->file);
    }
  }
  assert (BTOR_EMPTY_STACK (expect_bv_stack));
  BTOR_RELEASE_STACK (args);
  BTOR_RELEASE_STACK (dump);
  BTOR_RELEASE_STACK (expect_bv_stack);
  BTOR_RELEASE_STACK (expect_bool_stack);
  BTOR_RELEASE_STACK (add_space_stack);
  BTOR_RELEASE_STACK (zero_extend_stack);
  BTOR_RELEASE_STACK (depth_stack);
  btor_hashptr_table_delete (visited);
}

static void
dump_let_smt (BtorSMTDumpContext *sdc, BtorNode *exp)
{
  assert (sdc);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (!btor_hashptr_table_get (sdc->dumped, exp));

  fputs ("(let (", sdc->file);
  fputc ('(', sdc->file);
  dump_smt_id (sdc, exp);  // TODO (ma): better symbol for lets?
  fputc (' ', sdc->file);
  recursively_dump_exp_smt (sdc, exp, !is_boolean (sdc, exp), 0);
  fputs ("))", sdc->file);
  sdc->open_lets++;
  assert (btor_hashptr_table_get (sdc->dumped, exp));
}

static void
dump_fun_let_smt2 (BtorSMTDumpContext *sdc, BtorNode *exp)
{
  assert (sdc);
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (!btor_hashptr_table_get (sdc->dumped, exp));

  int is_bool;

  is_bool = is_boolean (sdc, exp);
  fputs ("(define-fun ", sdc->file);
  dump_smt_id (sdc, exp);
  fputs (" () ", sdc->file);
  if (is_bool)
    fputs ("Bool", sdc->file);
  else
    btor_dumpsmt_dump_sort_node (exp, sdc->file);
  fputc (' ', sdc->file);
  recursively_dump_exp_smt (sdc, exp, !is_bool, 0);
  fputs (")\n", sdc->file);
  assert (btor_hashptr_table_get (sdc->dumped, exp));
}

static void
dump_fun_smt2 (BtorSMTDumpContext *sdc, BtorNode *fun)
{
  assert (fun);
  assert (sdc);
  assert (BTOR_IS_REGULAR_NODE (fun));
  assert (btor_is_lambda_node (fun));
  assert (!fun->parameterized);
  assert (!btor_hashptr_table_get (sdc->dumped, fun));

  int i, refs;
  BtorNode *cur, *param, *fun_body, *p;
  BtorMemMgr *mm = sdc->btor->mm;
  BtorNodePtrStack visit, shared;
  BtorNodeIterator it, iit;
  BtorPtrHashTable *mark;
  BtorPtrHashBucket *b;

  mark = btor_hashptr_table_new (mm,
                                 (BtorHashPtr) btor_hash_exp_by_id,
                                 (BtorCmpPtr) btor_compare_exp_by_id);
  BTOR_INIT_STACK (mm, visit);
  BTOR_INIT_STACK (mm, shared);

#if 0
  extract_store (sdc, fun, &index, &value, &array);

  if (index)
    {
      assert (value);
      assert (array);
      btor_hashptr_table_add (sdc->stores, fun);
      return;
    }
#endif

  /* collect shared parameterized expressions in function body */
  fun_body = btor_lambda_get_body (fun);
  BTOR_PUSH_STACK (visit, fun_body);
  while (!BTOR_EMPTY_STACK (visit))
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));

    if (btor_hashptr_table_get (mark, cur)
        || btor_hashptr_table_get (sdc->dumped, cur)
        || btor_is_lambda_node (cur))
      continue;

    b = btor_hashptr_table_get (sdc->dump, cur);
    assert (b);
    refs = b->data.as_int;

    /* args and params are handled differently */
    /* collect shared parameterized expressions in function body.
     * arguments, parameters, and constants are excluded. */
    if (!btor_is_args_node (cur)
        && !btor_is_param_node (cur)
        /* constants are always printed */
        && !btor_is_bv_const_node (cur) && cur->parameterized && refs > 1)
      BTOR_PUSH_STACK (shared, cur);

    btor_hashptr_table_add (mark, cur);
    for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (visit, cur->e[i]);
  }

  /* dump function signature */
  fputs ("(define-fun ", sdc->file);
  dump_smt_id (sdc, fun);
  fputs (" (", sdc->file);

  btor_iter_lambda_init (&it, fun);
  while (btor_iter_lambda_has_next (&it))
  {
    cur   = btor_iter_lambda_next (&it);
    param = cur->e[0];
    if (!btor_hashptr_table_get (mark, cur)) btor_hashptr_table_add (mark, cur);
    if (!btor_hashptr_table_get (mark, param))
      btor_hashptr_table_add (mark, param);
    btor_hashptr_table_add (sdc->dumped, cur);
    btor_hashptr_table_add (sdc->dumped, param);
    if (fun != cur) fputc (' ', sdc->file);
    fputc ('(', sdc->file);
    dump_smt_id (sdc, param);
    fputc (' ', sdc->file);
    btor_dumpsmt_dump_sort_node (param, sdc->file);
    fputc (')', sdc->file);
  }
  fputs (") ", sdc->file);

  if (is_boolean (sdc, fun_body))
    fputs ("Bool", sdc->file);
  else
    btor_dumpsmt_dump_sort_node (fun_body, sdc->file);
  fputc (' ', sdc->file);

  assert (sdc->open_lets == 0);

  /* dump expressions that are shared in 'fun' */
  if (shared.start)
    qsort (shared.start,
           BTOR_COUNT_STACK (shared),
           sizeof (BtorNode *),
           cmp_node_id);

  for (i = 0; i < BTOR_COUNT_STACK (shared); i++)
  {
    cur = BTOR_PEEK_STACK (shared, i);
    assert (BTOR_IS_REGULAR_NODE (cur));
    assert (cur->parameterized);
    dump_let_smt (sdc, cur);
    fputc (' ', sdc->file);
  }
  recursively_dump_exp_smt (sdc, fun_body, !is_boolean (sdc, fun_body), 0);

  /* close lets */
  for (i = 0; i < sdc->open_lets; i++) fputc (')', sdc->file);
  sdc->open_lets = 0;

  /* close define-fun */
  fputs (")\n", sdc->file);

  /* due to lambda hashing it is possible that a lambda in 'fun' is shared in
   * different functions. hence, we have to check if all lambda parents of
   * the resp. lambda have already been dumped as otherwise all expressions
   * below have to be removed from 'sdc->dumped' as they will be dumped
   * again in a different function definition. */
  btor_iter_lambda_init (&it, fun);
  while (btor_iter_lambda_has_next (&it))
  {
    cur = btor_iter_lambda_next (&it);
    btor_iter_parent_init (&iit, cur);
    while (btor_iter_parent_has_next (&iit))
    {
      p = btor_iter_parent_next (&iit);
      /* find lambda parent that needs to be dumped but has not yet been
       * dumped */
      if (btor_hashptr_table_get (sdc->dump, p)
          && !btor_hashptr_table_get (sdc->dumped, p)
          && btor_is_lambda_node (p))
      {
        BTOR_PUSH_STACK (visit, cur);
        while (!BTOR_EMPTY_STACK (visit))
        {
          cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));
          assert (BTOR_IS_REGULAR_NODE (cur));

          if (!cur->parameterized
              && (!btor_hashptr_table_get (mark, cur)
                  || !btor_hashptr_table_get (sdc->dumped, cur)))
            continue;

          if (btor_hashptr_table_get (sdc->dumped, cur))
            btor_hashptr_table_remove (sdc->dumped, cur, 0, 0);

          for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (visit, cur->e[i]);
        }
        break;
      }
    }
  }

  BTOR_RELEASE_STACK (shared);
  BTOR_RELEASE_STACK (visit);
  btor_hashptr_table_delete (mark);
}

static void
dump_declare_fun_smt (BtorSMTDumpContext *sdc, BtorNode *exp)
{
  assert (!btor_hashptr_table_get (sdc->dumped, exp));
  fputs ("(declare-fun ", sdc->file);
  dump_smt_id (sdc, exp);
  fputc (' ', sdc->file);
  if (btor_is_bv_var_node (exp)) fputs ("() ", sdc->file);
  btor_dumpsmt_dump_sort_node (exp, sdc->file);
  fputs (")\n", sdc->file);
  btor_hashptr_table_add (sdc->dumped, exp);
}

static void
dump_assert_smt2 (BtorSMTDumpContext *sdc, BtorNode *exp)
{
  assert (sdc);
  assert (exp);
  assert (btor_get_exp_width (sdc->btor, exp) == 1);

  fputs ("(assert ", sdc->file);
  if (!is_boolean (sdc, exp)) fputs ("(distinct ", sdc->file);
  recursively_dump_exp_smt (sdc, exp, 0, 0);
  if (!is_boolean (sdc, exp)) fputs (" #b0)", sdc->file);
  fputs (")\n", sdc->file);
}

static void
set_logic_smt (BtorSMTDumpContext *sdc, const char *logic)
{
  assert (sdc);

  const char *fmt;

  fmt = "(set-logic %s)\n";
  fprintf (sdc->file, fmt, logic);
}

static int
get_references (BtorSMTDumpContext *sdc, BtorNode *exp)
{
  assert (exp);

  int refs = 0;
  BtorNode *cur;
  BtorNodeIterator it;
  BtorPtrHashBucket *b;

  exp = BTOR_REAL_ADDR_NODE (exp);

  /* get reference count of roots */
  if (btor_hashptr_table_get (sdc->roots, exp)) refs++;
  if (btor_hashptr_table_get (sdc->roots, BTOR_INVERT_NODE (exp))) refs++;

  btor_iter_parent_init (&it, exp);
  while (btor_iter_parent_has_next (&it))
  {
    cur = btor_iter_parent_next (&it);
    assert (BTOR_IS_REGULAR_NODE (cur));
    b = btor_hashptr_table_get (sdc->dump, cur);
    /* argument nodes are counted differently */
    if (!b || btor_is_args_node (cur)) continue;
    refs++;
  }
  return refs;
}

static bool
has_lambda_parents_only (BtorNode *exp)
{
  BtorNode *p;
  BtorNodeIterator it;
  btor_iter_parent_init (&it, exp);
  while (btor_iter_parent_has_next (&it))
  {
    p = btor_iter_parent_next (&it);
    if (!btor_is_lambda_node (p)) return false;
  }
  return true;
}

static void
mark_boolean (BtorSMTDumpContext *sdc, BtorNodePtrStack *exps)
{
  int i, j, not_bool;
  BtorNode *cur;

  /* collect boolean terms */
  for (i = 0; i < BTOR_COUNT_STACK (*exps); i++)
  {
    cur = BTOR_PEEK_STACK (*exps, i);

    /* these nodes are boolean by definition */
    if (btor_is_bv_eq_node (cur) || btor_is_fun_eq_node (cur)
        || btor_is_ult_node (cur)
        || cur == BTOR_REAL_ADDR_NODE (sdc->btor->true_exp))
    {
      btor_hashptr_table_add (sdc->boolean, cur);
      continue;
    }
    else if (btor_is_apply_node (cur))
    {
      /* boolean function */
      if ((btor_is_lambda_node (cur->e[0])
           && is_boolean (sdc, btor_lambda_get_body (cur->e[0])))
          || (btor_is_fun_cond_node (cur->e[0]) && is_boolean (sdc, cur->e[1]))
          || (btor_is_uf_node (cur->e[0])
              && btor_sort_is_bool (
                     sdc->btor,
                     btor_sort_fun_get_codomain (
                         sdc->btor, btor_exp_get_sort_id (cur->e[0])))))
        btor_hashptr_table_add (sdc->boolean, cur);
      continue;
    }
    else if ((btor_is_and_node (cur) || btor_is_bv_cond_node (cur))
             && btor_get_exp_width (sdc->btor, cur) == 1)
    {
      not_bool = 0;
      for (j = 0; j < cur->arity; j++)
      {
        if (!is_boolean (sdc, cur->e[j]))
        {
          not_bool = 1;
          break;
        }
      }

      if (not_bool) continue;

      btor_hashptr_table_add (sdc->boolean, cur);
    }
  }
}

static void
dump_smt (BtorSMTDumpContext *sdc)
{
  assert (sdc);

  int i, j;
  BtorNode *e, *cur;
  BtorMemMgr *mm;
  BtorNodePtrStack visit, all, vars, shared, ufs;
  BtorPtrHashBucket *b;
  BtorPtrHashTableIterator it;
  BtorArgsIterator ait;

  mm = sdc->btor->mm;
  BTOR_INIT_STACK (mm, visit);
  BTOR_INIT_STACK (mm, shared);
  BTOR_INIT_STACK (mm, all);
  BTOR_INIT_STACK (mm, vars);
  BTOR_INIT_STACK (mm, ufs);

  btor_iter_hashptr_init (&it, sdc->roots);
  while (btor_iter_hashptr_has_next (&it))
  {
    cur = btor_iter_hashptr_next (&it);
    BTOR_PUSH_STACK (visit, BTOR_REAL_ADDR_NODE (cur));
  }

  /* collect constants, variables, array variables and functions */
  while (!BTOR_EMPTY_STACK (visit))
  {
    cur = BTOR_POP_STACK (visit);
    assert (BTOR_IS_REGULAR_NODE (cur));
    assert (!btor_hashptr_table_get (sdc->dumped, cur));

    if (btor_hashptr_table_get (sdc->dump, cur)) continue;

    btor_hashptr_table_add (sdc->dump, cur)->data.as_int = 0;
    BTOR_PUSH_STACK (all, cur);

    if (btor_is_bv_var_node (cur))
      BTOR_PUSH_STACK (vars, cur);
    else if (btor_is_uf_node (cur))
      BTOR_PUSH_STACK (ufs, cur);
    else if (btor_is_lambda_node (cur) && !cur->parameterized
             && !has_lambda_parents_only (cur))
      BTOR_PUSH_STACK (shared, cur);

    for (j = 0; j < cur->arity; j++)
      BTOR_PUSH_STACK (visit, BTOR_REAL_ADDR_NODE (cur->e[j]));
  }

  /* compute reference counts of expressions (required for determining shared
   * expressions)*/
  if (all.start)
    qsort (all.start, BTOR_COUNT_STACK (all), sizeof e, cmp_node_id);

  for (i = 0; i < BTOR_COUNT_STACK (all); i++)
  {
    cur = BTOR_PEEK_STACK (all, i);
    b   = btor_hashptr_table_get (sdc->dump, cur);
    assert (b);
    assert (b->data.as_int == 0);
    /* cache result for later reuse */
    b->data.as_int = get_references (sdc, cur);

    /* update references for expressions under argument nodes */
    if (btor_is_args_node (cur) && b->data.as_int > 0)
    {
      btor_iter_args_init (&ait, cur);
      while (btor_iter_args_has_next (&ait))
      {
        e = BTOR_REAL_ADDR_NODE (btor_iter_args_next (&ait));
        assert (btor_hashptr_table_get (sdc->dump, e));
        btor_hashptr_table_get (sdc->dump, e)->data.as_int += b->data.as_int;
      }
    }
  }

  /* collect globally shared expressions */
  for (i = 0; i < BTOR_COUNT_STACK (all); i++)
  {
    cur = BTOR_PEEK_STACK (all, i);
    b   = btor_hashptr_table_get (sdc->dump, cur);
    assert (b);

    if (b->data.as_int <= 1
        /* parameterized expressions are only shared within a function */
        || cur->parameterized
        || btor_is_param_node (cur)
        /* constants are always printed */
        || btor_is_bv_const_node (cur)
        /* for variables and functions the resp. symbols are always printed */
        || btor_is_bv_var_node (cur) || btor_is_lambda_node (cur)
        || btor_is_uf_node (cur)
        /* argument nodes are never printed */
        || btor_is_args_node (cur))
      continue;

    BTOR_PUSH_STACK (shared, cur);
  }

  /* collect boolean terms */
  mark_boolean (sdc, &all);

  /* begin dump */
  if (BTOR_EMPTY_STACK (ufs))
    set_logic_smt (sdc, "QF_BV");
  else
    set_logic_smt (sdc, "QF_UFBV");

  /* dump inputs */
  if (vars.start)
    qsort (vars.start, BTOR_COUNT_STACK (vars), sizeof e, cmp_node_id);

  for (i = 0; i < BTOR_COUNT_STACK (vars); i++)
  {
    cur = BTOR_PEEK_STACK (vars, i);
    dump_declare_fun_smt (sdc, cur);
  }

  if (ufs.start)
    qsort (ufs.start, BTOR_COUNT_STACK (ufs), sizeof e, cmp_node_id);

  for (i = 0; i < BTOR_COUNT_STACK (ufs); i++)
  {
    cur = BTOR_PEEK_STACK (ufs, i);
    dump_declare_fun_smt (sdc, cur);
  }

  /* dump shared expressions and functions */
  if (shared.start)
    qsort (shared.start, BTOR_COUNT_STACK (shared), sizeof e, cmp_node_id);

  for (i = 0; i < BTOR_COUNT_STACK (shared); i++)
  {
    cur = BTOR_PEEK_STACK (shared, i);
    assert (BTOR_IS_REGULAR_NODE (cur));

    if (btor_hashptr_table_get (sdc->dumped, cur)) continue;

    assert (!cur->parameterized);

    if (btor_is_lambda_node (cur))
      dump_fun_smt2 (sdc, cur);
    else
      dump_fun_let_smt2 (sdc, cur);
  }

  /* dump assertions/build root */
  btor_iter_hashptr_init (&it, sdc->roots);
  while (btor_iter_hashptr_has_next (&it))
  {
    cur = btor_iter_hashptr_next (&it);
    dump_assert_smt2 (sdc, cur);
  }
  assert (sdc->open_lets == 0);

#ifndef NDEBUG
  btor_iter_hashptr_init (&it, sdc->dump);
  while (btor_iter_hashptr_has_next (&it))
  {
    cur = btor_iter_hashptr_next (&it);
    /* constants and function applications are always dumped (hence, not in
     * mark) */
    if (btor_is_bv_const_node (cur)
        || btor_is_apply_node (cur)
        /* argument nodes are never dumped and not in mark */
        || btor_is_args_node (cur))
      continue;
    assert (btor_hashptr_table_get (sdc->dumped, cur));
  }
#endif

  BTOR_RELEASE_STACK (shared);
  BTOR_RELEASE_STACK (visit);
  BTOR_RELEASE_STACK (all);
  BTOR_RELEASE_STACK (vars);
  BTOR_RELEASE_STACK (ufs);

  fputs ("(check-sat)\n", sdc->file);
  fputs ("(exit)\n", sdc->file);
  fflush (sdc->file);
}

static void
dump_smt_aux (Btor *btor, FILE *file, BtorNode **roots, int nroots)
{
  assert (btor);
  assert (file);

  int i;
  BtorNode *tmp, *tmp_roots[nroots];
  BtorPtrHashTableIterator it;
  BtorSMTDumpContext *sdc;

  for (i = 0; i < nroots; i++) tmp_roots[i] = roots[i];

  sdc = new_smt_dump_context (btor, file);

  if (nroots)
  {
    for (i = 0; i < nroots; i++)
      add_root_to_smt_dump_context (sdc, tmp_roots[i]);
  }
  else
  {
    if (btor->inconsistent)
    {
      tmp = btor_false_exp (btor);
      add_root_to_smt_dump_context (sdc, tmp);
      btor_release_exp (btor, tmp);
    }
    else if (btor->unsynthesized_constraints->count == 0
             && btor->synthesized_constraints->count == 0)
    {
      tmp = btor_true_exp (btor);
      add_root_to_smt_dump_context (sdc, tmp);
      btor_release_exp (btor, tmp);
    }
    else
    {
      btor_iter_hashptr_init (&it, btor->unsynthesized_constraints);
      btor_iter_hashptr_queue (&it, btor->synthesized_constraints);
      while (btor_iter_hashptr_has_next (&it))
        add_root_to_smt_dump_context (sdc, btor_iter_hashptr_next (&it));
    }
  }

  dump_smt (sdc);
  delete_smt_dump_context (sdc);
}

void
btor_dumpsmt_dump (Btor *btor, FILE *file)
{
  assert (btor);
  assert (file);
  dump_smt_aux (btor, file, 0, 0);
}

void
btor_dumpsmt_dump_node (Btor *btor, FILE *file, BtorNode *exp, unsigned depth)
{
  assert (btor);

  int i;
  BtorNode *cur, *real_exp;
  BtorSMTDumpContext *sdc;
  BtorNodePtrStack visit, all;
  BtorArgsIterator ait;
  BtorPtrHashBucket *b;

  real_exp = BTOR_REAL_ADDR_NODE (exp);

  BTOR_INIT_STACK (btor->mm, all);
  BTOR_INIT_STACK (btor->mm, visit);
  sdc = new_smt_dump_context (btor, file);

  if (!exp)
  {
    fprintf (file, "null\n");
    goto CLEANUP;
  }
  else if (btor_is_args_node (real_exp) || btor_is_param_node (real_exp))
  {
    fprintf (file, "%s_%d\n", g_kind2smt[real_exp->kind], real_exp->id);
    goto CLEANUP;
  }
  else if (btor_is_bv_var_node (exp) || btor_is_uf_node (exp))
  {
    dump_declare_fun_smt (sdc, exp);
    goto CLEANUP;
  }

  BTOR_PUSH_STACK (visit, exp);
  while (!BTOR_EMPTY_STACK (visit))
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));

    if (btor_hashptr_table_get (sdc->dump, cur)) continue;

    if (btor_is_bv_var_node (cur) || btor_is_uf_node (cur)
        || (!btor_is_lambda_node (real_exp) && btor_is_param_node (cur)))
      btor_hashptr_table_add (sdc->dumped, cur);

    btor_hashptr_table_add (sdc->dump, cur);
    BTOR_PUSH_STACK (all, cur);

    for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (visit, cur->e[i]);
  }

  /* compute reference counts of expressions (required for determining shared
   * expressions)*/
  if (all.start)
    qsort (all.start, BTOR_COUNT_STACK (all), sizeof (BtorNode *), cmp_node_id);

  for (i = 0; i < BTOR_COUNT_STACK (all); i++)
  {
    cur = BTOR_PEEK_STACK (all, i);
    b   = btor_hashptr_table_get (sdc->dump, cur);
    assert (b);
    assert (b->data.as_int == 0);
    /* cache result for later reuse */
    b->data.as_int = get_references (sdc, cur);

    /* update references for expressions under argument nodes */
    if (btor_is_args_node (cur) && b->data.as_int > 0)
    {
      btor_iter_args_init (&ait, cur);
      while (btor_iter_args_has_next (&ait))
      {
        cur = BTOR_REAL_ADDR_NODE (btor_iter_args_next (&ait));
        assert (btor_hashptr_table_get (sdc->dump, cur));
        btor_hashptr_table_get (sdc->dump, cur)->data.as_int += b->data.as_int;
      }
    }
  }

  mark_boolean (sdc, &all);
  if (btor_is_lambda_node (exp))
    dump_fun_smt2 (sdc, exp);
  else
  {
    recursively_dump_exp_smt (sdc, exp, 0, depth);
    fprintf (file, "\n");
  }
CLEANUP:
  delete_smt_dump_context (sdc);
  BTOR_RELEASE_STACK (all);
  BTOR_RELEASE_STACK (visit);
}
