#include <utility>

#include "../num/mulnum_simple.h"
#include "../num/algorithms.h"

using num = big::num_t<std::uint32_t>;

struct M2x2
{
    num e00, e01, e10, e11;
    M2x2(num e00, num e01, num e10, num e11)
        : e00(std::move(e00))
        , e01(std::move(e01))
        , e10(std::move(e10))
        , e11(std::move(e11))
    {}
    
    M2x2 operator*(M2x2 const &o) const
    {
        return {
                e00*o.e00 + e01*o.e10,
                e00*o.e01 + e01*o.e11,
                e10*o.e00 + e11*o.e10,
                e10*o.e01 + e11*o.e11};
    }
    M2x2 &operator*=(M2x2 const &o)
    {
        return *this = *this * o;
    }
};

number fibonacci_matmul_simple(number n)
{
    M2x2 fib(num(0), num(1), num(1), num(1));
    const M2x2 step(fib);
    while (n-- > 0)
    {
        fib *= step;
    }
    return static_cast<number>(fib.e00); //static_cast<number>(fib.e00);
}
