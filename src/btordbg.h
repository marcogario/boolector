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

#ifndef BTORDBG_H_INCLUDED
#define BTORDBG_H_INCLUDED

/*------------------------------------------------------------------------*/
#ifndef NDEBUG
/*------------------------------------------------------------------------*/

#include "btorcore.h"

/*------------------------------------------------------------------------*/
/* core                                                                   */
/*------------------------------------------------------------------------*/

bool btor_dbg_check_lambdas_static_rho_proxy_free (const Btor* btor);

bool btor_dbg_check_unique_table_children_proxy_free (const Btor* btor);

bool btor_dbg_check_hash_table_proxy_free (BtorPtrHashTable* table);

bool btor_dbg_check_all_hash_tables_proxy_free (const Btor* btor);

bool btor_dbg_check_hash_table_simp_free (BtorPtrHashTable* table);

bool btor_dbg_check_all_hash_tables_simp_free (const Btor* btor);

bool btor_dbg_check_constraints_not_const (const Btor* btor);

bool btor_dbg_check_assumptions_simp_free (const Btor* btor);

/*------------------------------------------------------------------------*/
/* exp                                                                    */
/*------------------------------------------------------------------------*/

bool btor_precond_slice_exp_dbg (Btor* btor,
                                 const BtorNode* exp,
                                 uint32_t upper,
                                 uint32_t lower);

bool btor_precond_ext_exp_dbg (Btor* btor, const BtorNode* exp);

bool btor_precond_regular_unary_bv_exp_dbg (Btor* btor, const BtorNode* exp);

bool btor_precond_regular_binary_bv_exp_dbg (Btor* btor,
                                             const BtorNode* e0,
                                             const BtorNode* e1);

bool btor_precond_eq_exp_dbg (Btor* btor,
                              const BtorNode* e0,
                              const BtorNode* e1);

bool btor_precond_shift_exp_dbg (Btor* btor,
                                 const BtorNode* e0,
                                 const BtorNode* e1);

bool btor_precond_concat_exp_dbg (Btor* btor,
                                  const BtorNode* e0,
                                  const BtorNode* e1);

bool btor_precond_read_exp_dbg (Btor* btor,
                                const BtorNode* e_array,
                                const BtorNode* e_index);

bool btor_precond_write_exp_dbg (Btor* btor,
                                 const BtorNode* e_array,
                                 const BtorNode* e_index,
                                 const BtorNode* e_value);

bool btor_precond_cond_exp_dbg (Btor* btor,
                                const BtorNode* e_cond,
                                const BtorNode* e_if,
                                const BtorNode* e_else);

bool btor_precond_apply_exp_dbg (Btor* btor,
                                 const BtorNode* fun,
                                 const BtorNode* args);

/*------------------------------------------------------------------------*/
#endif
/*------------------------------------------------------------------------*/

#endif
