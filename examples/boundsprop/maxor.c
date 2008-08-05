#include "maxor.h"
#include "../../boolector.h"
#include "../../btorutil.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* maxOR algorithm from hacker's delight, page 60 */

BtorExp *
btor_maxor (Btor *btor,
            BtorExp *a_in,
            BtorExp *b_in,
            BtorExp *c_in,
            BtorExp *d_in,
            BtorExp *m_in,
            int num_bits)
{
  BtorExp *temp_1, *temp_2, *m, *zero;
  BtorExp *tmp, *a, *b, *c, *d, *m_minus_1, *b_minus_m;
  BtorExp *d_minus_m, *one_log_bits, *b_and_d;
  BtorExp *b_and_d_and_m, *temp_1_ugte_a, *temp_2_ugte_c;
  BtorExp *b_and_d_and_m_ne_zero, *cond_1, *cond_2, *result;
  BtorExp *and_break, *cond_3, *cond_4, *_break;
  int i;

  assert (btor != NULL);
  assert (a_in != NULL);
  assert (b_in != NULL);
  assert (c_in != NULL);
  assert (d_in != NULL);
  assert (m_in != NULL);
  assert (num_bits > 0);
  assert (btor_is_power_of_2_util (num_bits));

  a = boolector_copy (btor, a_in);
  b = boolector_copy (btor, b_in);
  c = boolector_copy (btor, c_in);
  d = boolector_copy (btor, d_in);
  m = boolector_copy (btor, m_in);

  one_log_bits = boolector_one (btor, btor_log_2_util (num_bits));
  zero         = boolector_zero (btor, num_bits);

  /* as soon _break becomes 1, we do not change the values
   * of b and d anymore */
  _break = boolector_false (btor);

  for (i = 0; i < num_bits; i++)
  {
    b_and_d               = boolector_and (btor, b, d);
    b_and_d_and_m         = boolector_and (btor, b_and_d, m);
    b_and_d_and_m_ne_zero = boolector_ne (btor, b_and_d_and_m, zero);

    m_minus_1 = boolector_dec (btor, m);

    b_minus_m     = boolector_sub (btor, b, m);
    temp_1        = boolector_or (btor, b_minus_m, m_minus_1);
    temp_1_ugte_a = boolector_ugte (btor, temp_1, a);

    d_minus_m     = boolector_sub (btor, d, m);
    temp_2        = boolector_or (btor, d_minus_m, m_minus_1);
    temp_2_ugte_c = boolector_ugte (btor, temp_2, c);

    /* update b */
    cond_1 = boolector_cond (btor, temp_1_ugte_a, temp_1, b);
    cond_2 = boolector_cond (btor, b_and_d_and_m_ne_zero, cond_1, b);
    tmp    = boolector_cond (btor, _break, b, cond_2);
    boolector_release (btor, b);
    b = tmp;

    /* update _break */
    and_break = boolector_and (btor, b_and_d_and_m_ne_zero, temp_1_ugte_a);
    tmp       = boolector_or (btor, _break, and_break);
    boolector_release (btor, _break);
    _break = tmp;
    boolector_release (btor, and_break);

    /* update d */
    cond_3 = boolector_cond (btor, temp_2_ugte_c, temp_2, d);
    cond_4 = boolector_cond (btor, b_and_d_and_m_ne_zero, cond_3, d);
    tmp    = boolector_cond (btor, _break, d, cond_4);
    boolector_release (btor, d);
    d = tmp;

    /* update _break */
    and_break = boolector_and (btor, b_and_d_and_m_ne_zero, temp_2_ugte_c);
    tmp       = boolector_or (btor, _break, and_break);
    boolector_release (btor, _break);
    _break = tmp;
    boolector_release (btor, and_break);

    /* update m */
    tmp = boolector_srl (btor, m, one_log_bits);
    boolector_release (btor, m);
    m = tmp;

    boolector_release (btor, b_and_d);
    boolector_release (btor, b_and_d_and_m);
    boolector_release (btor, b_and_d_and_m_ne_zero);
    boolector_release (btor, cond_1);
    boolector_release (btor, cond_2);
    boolector_release (btor, cond_3);
    boolector_release (btor, cond_4);
    boolector_release (btor, m_minus_1);
    boolector_release (btor, b_minus_m);
    boolector_release (btor, d_minus_m);
    boolector_release (btor, temp_1);
    boolector_release (btor, temp_2);
    boolector_release (btor, temp_1_ugte_a);
    boolector_release (btor, temp_2_ugte_c);
  }

  result = boolector_or (btor, b, d);

  boolector_release (btor, _break);
  boolector_release (btor, a);
  boolector_release (btor, b);
  boolector_release (btor, c);
  boolector_release (btor, d);
  boolector_release (btor, m);
  boolector_release (btor, zero);
  boolector_release (btor, one_log_bits);

  return result;
}