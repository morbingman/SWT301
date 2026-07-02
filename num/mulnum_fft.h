#ifndef __MUL_FFT_H
#define __MUL_FFT_H

#include "number.h"

#include <algorithm>
#include <complex>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace big
{

    inline num_t<std::uint8_t> operator*(const num_t<std::uint8_t> &, const num_t<std::uint8_t> &);

    //////////////// IMPLEMENTATIONS ////////////////

    using real_t = double; // seems to be good enough
    using complex = std::complex<real_t>;
    // Not constexpr: std::complex arithmetic is only constexpr on standard
    // library implementations that have caught up to the C++20 requirement,
    // and this value is only ever used inside primitive_root(), which isn't
    // constexpr itself (it calls std::exp). Keeping this as a plain inline
    // const avoids depending on that catch-up entirely.
    inline const complex full_rot = 2 * std::numbers::pi_v<real_t> * complex(static_cast<real_t>(0), static_cast<real_t>(1));

    template<bool inv=false>
    inline complex primitive_root(size_t N)
    {
        const complex N_c(static_cast<real_t>(N), static_cast<real_t>(0));
        if constexpr(inv)
        {
            return std::exp(-full_rot / N_c);
        }
        else
        {
            return std::exp(full_rot / N_c);
        }
    }

    /* rounds x up to the next power of 2 */
    constexpr size_t pow2ceil(size_t x)
    {
        size_t y;
        do
        {
            y = x;
        }
        while (x &= x-1);
        return y << 1;
    }

    constexpr void inc_rev(size_t &x, size_t top_bit)
    {
        while (x & (top_bit >>= 1))
        {
            x ^= top_bit;
        }
        x |= top_bit;
    }

    template<typename T>
    inline std::vector<complex> bit_reverse_shuffle(const std::vector<T> &x, size_t pow2size)
    {
        std::vector<complex> out(pow2size);

        for (size_t i = 0, ri = 0; i < x.size(); ++i, inc_rev(ri, pow2size))
        {
            if constexpr (std::is_same_v<T, complex>)
            {
                out[ri] = x[i]; // already complex -- direct copy, no ambiguity
            }
            else
            {
                // Explicit real+imag avoids the single-argument constructor
                // ambiguity between std::complex's GNU _Complex-type overload
                // and its standard (double, double) overload.
                out[ri] = complex(static_cast<real_t>(x[i]), static_cast<real_t>(0));
            }
        }
        return out;
    }

    inline std::vector<std::uint64_t> from_complex(const std::vector<complex> &x)
    {
        using std::uint64_t;
        size_t top_bit = 1;
        while (top_bit < x.size())
        {
            top_bit <<= 1;
        }
        std::vector<uint64_t> out;
        out.reserve(x.size());

        for (const auto &xi : x)
        {
            out.emplace_back(static_cast<uint64_t>(std::round(xi.real())));
        }
        return out;
    }

    inline std::vector<std::uint8_t> fold(const std::vector<std::uint64_t> &x)
    {
        using std::uint8_t;
        using std::uint64_t;
        uint64_t spill = 0;
        std::vector<uint8_t> out;
        out.reserve(x.size()+8);
        for (const auto &xi : x)
        {
            uint64_t sum = xi + spill;
            out.emplace_back(static_cast<uint8_t>(sum));
            spill = sum >> 8;
        }
        while (spill)
        {
            out.emplace_back(static_cast<uint8_t>(spill));
            spill >>= 8;
        }
        return out;
    }

    enum class dft_t
    {
        normal = 0,
        inverse = 1
    };

    /* assumes x is already bit-reverse-shuffled
     *
     * [Cooley-Tukey; yoinked from Wikipedia]
     */
    /* Twiddle factors for one FFT stage (size m) are the same sequence for
     * every block in that stage -- they only depend on m and j, never on
     * the block offset k. The original code rebuilt this sequence from
     * scratch (via repeated complex multiply) inside the per-block loop,
     * so it paid for the same work x.size()/m times per stage -- roughly
     * doubling the FFT's real multiplication count on top of its proper
     * O(N log N) butterfly cost.
     *
     * Building the table once per stage (O(m) work) and reusing it across
     * all blocks in that stage removes that redundancy. Caching it further
     * across separate fft() calls (thread_local, keyed by size+direction)
     * also helps here specifically: repeated squaring in fast-doubling
     * algorithms very often reuses the same power-of-2 FFT size across
     * many consecutive multiplies, so this avoids rebuilding (and
     * recomputing std::exp for) the same table again and again.
     */
    template<bool inv>
    inline const std::vector<complex> &twiddle_table(size_t m)
    {
        thread_local std::unordered_map<size_t, std::vector<complex>> cache;

        auto it = cache.find(m);
        if (it != cache.end())
        {
            return it->second;
        }

        std::vector<complex> table(m >> 1);
        complex omega = primitive_root<inv>(m);
        complex coef(static_cast<real_t>(1), static_cast<real_t>(0));
        for (auto &c : table)
        {
            c = coef;
            coef *= omega;
        }
        return cache.emplace(m, std::move(table)).first->second;
    }

    template<dft_t dft_type=dft_t::normal>
    inline void fft(std::vector<complex> &x)
    {
        for (size_t m = 2; m <= x.size(); m <<= 1)
        {
            const std::vector<complex> &table = twiddle_table<dft_type==dft_t::normal>(m);
            size_t m2 = m >> 1;
            for (size_t k = 0; k < x.size(); k += m)
            {
                for (size_t j = 0; j < m2; ++j)
                {
                    complex t = table[j] * x[k + j + m2];
                    complex u = x[k + j];
                    x[k + j] = u + t;
                    x[k + j + m2] = u - t;
                }
            }
        }
        if constexpr(dft_type == dft_t::inverse)
        {
            for (auto &xi : x)
            {
                xi /= x.size();
            }
        }
    }

    num_t<std::uint8_t> operator*(const num_t<std::uint8_t> &lhs, const num_t<std::uint8_t> &rhs)
    {
        size_t size = pow2ceil(std::max(lhs.value.size(), rhs.value.size()) << 1);
        std::vector<complex> lc = bit_reverse_shuffle(lhs.value, size);
        std::vector<complex> rc = bit_reverse_shuffle(rhs.value, size);

        fft(lc);
        fft(rc);

        for (size_t i = 0; i < size; ++i)
        {
            lc[i] *= rc[i];
        }

        std::vector<complex> conv = bit_reverse_shuffle(lc, size);
        fft<dft_t::inverse>(conv);

        DB({ num_t z(fold(from_complex(conv))); cerr << lhs.str(true) << " * " << rhs.str(true) << " == " << z.str(true) << endl; });
        return num_t(fold(from_complex(conv)));
    }

} // namespace big


#endif//__MUL_FFT_H