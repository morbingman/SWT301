#ifndef __ALGORITHMS_H
#define __ALGORITHMS_H

#include "number.h"
using big::number;

/* Each Fibonacci implementation lives in its own .cpp file under impl/,
 * and is given a unique name so all five can be linked into one binary.
 */

number fibonacci_naive(number n);          // impl/naive.cpp           - O(phi^n) recursive
number fibonacci_linear(number n);         // impl/linear.cpp          - O(n) iterative
number fibonacci_matmul_simple(number n);  // impl/matmul_simple.cpp   - O(n) matrix power (repeated multiply)
number fibonacci_matmul_fastexp(number n); // impl/matmul_fastexp.cpp  - O(log n) matrix fast exponentiation
number fibonacci_field_ext(number n);      // impl/field_ext.cpp       - O(log n) fast doubling via Z[sqrt(5)]

#endif//__ALGORITHMS_H
