/*
  Apery, a USI shogi playing engine derived from Stockfish, a UCI chess playing engine.
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad
  Copyright (C) 2011-2017 Hiraoka Takuya

  Apery is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Apery is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef APERY_LEARNER_HPP
#define APERY_LEARNER_HPP

#include "position.hpp"
#include "thread.hpp"
#include "evaluate.hpp"

#if defined LEARN

#if 0
#define PRINT_PV(x) x
#else
#define PRINT_PV(x)
#endif

// EvaluatorGradient のメモリ使用量を三角配列を用いて抑えた代わりに、double にして精度を高めたもの。
struct TriangularEvaluatorGradient {
    TriangularArray<std::array<double, 2>, int, fe_end, fe_end> kpp_grad[SquareNum];
    std::array<double, 2> kkp_grad[SquareNum][SquareNum][fe_end];

    void incParam(const Position& pos, const std::array<double, 2>& dinc) {
        const Square sq_bk = pos.kingSquare(Black);
        const Square sq_wk = pos.kingSquare(White);
        const int* list0 = pos.cplist0();
        const int* list1 = pos.cplist1();
        const std::array<double, 2> f = {{dinc[0] / FVScale, dinc[1] / FVScale}};

        for (int i = 0; i < pos.nlist(); ++i) {
            const int k0 = list0[i];
            const int k1 = list1[i];
            for (int j = 0; j < i; ++j) {
                const int l0 = list0[j];
                const int l1 = list1[j];
                kpp_grad[sq_bk         ].at(k0, l0) += f;
                kpp_grad[inverse(sq_wk)].at(k1, l1)[0] -= f[0];
                kpp_grad[inverse(sq_wk)].at(k1, l1)[1] += f[1];
            }
            kkp_grad[sq_bk][sq_wk][k0] += f;
        }
    }

    void clear() { memset(this, 0, sizeof(*this)); } // double 型とかだと規格的に 0 は保証されなかった気がするが実用上問題ないだろう。
};

// float, double 型の atomic 加算。T は float, double を想定。
template <typename T>
inline T atomicAdd(std::atomic<T> &x, const T diff) {
    T old = x.load(std::memory_order_consume);
    T desired = old + diff;
    while (!x.compare_exchange_weak(old, desired, std::memory_order_release, std::memory_order_consume))
        desired = old + diff;
    return desired;
}
// float, double 型の atomic 減算
template <typename T>
inline T atomicSub(std::atomic<T> &x, const T diff) { return atomicAdd(x, -diff); }

TriangularEvaluatorGradient& operator += (TriangularEvaluatorGradient& lhs, TriangularEvaluatorGradient& rhs) {
#if defined _OPENMP
#pragma omp parallel
#endif
#ifdef _OPENMP
#pragma omp for
#endif
    for (int sq = SQ11; sq < SquareNum; ++sq)
        for (auto lit = std::begin(lhs.kpp_grad[sq]), rit = std::begin(rhs.kpp_grad[sq]); lit != std::end(lhs.kpp_grad[sq]); ++lit, ++rit)
            *lit += *rit;
    for (auto lit = &(***std::begin(lhs.kkp_grad)), rit = &(***std::begin(rhs.kkp_grad)); lit != &(***std::end(lhs.kkp_grad)); ++lit, ++rit)
        *lit += *rit;

    return lhs;
}

// kpp_grad, kkp_grad の値を低次元の要素に与える。
inline void lowerDimension(EvaluatorBase<std::array<std::atomic<double>, 2>>& base, const TriangularEvaluatorGradient& grad)
{
#define FOO(indices, oneArray, sum)                                     \
    for (auto index : indices) {                                        \
        if (index == std::numeric_limits<ptrdiff_t>::max()) break;      \
        if (0 <= index) {                                               \
            atomicAdd((*oneArray( index))[0], sum[0]);                  \
            atomicAdd((*oneArray( index))[1], sum[1]);                  \
        }                                                               \
        else {                                                          \
            atomicSub((*oneArray(-index))[0], sum[0]);                  \
            atomicAdd((*oneArray(-index))[1], sum[1]);                  \
        }                                                               \
    }

#if defined _OPENMP
#pragma omp parallel
#endif

    // KPP
    {
#ifdef _OPENMP
#pragma omp for
#endif
        for (int ksq = SQ11; ksq < SquareNum; ++ksq) {
            ptrdiff_t indices[KPPIndicesMax];
            for (EvalIndex i = (EvalIndex)0; i < fe_end; ++i) {
                for (EvalIndex j = (EvalIndex)0; j <= i; ++j) { // 三角配列なので、i までで良い。
                    base.kppIndices(indices, static_cast<Square>(ksq), i, j);
                    FOO(indices, base.oneArrayKPP, grad.kpp_grad[ksq].at(i, j));
                }
            }
        }
    }
    // KKP
    {
#ifdef _OPENMP
#pragma omp for
#endif
        for (int ksq0 = SQ11; ksq0 < SquareNum; ++ksq0) {
            ptrdiff_t indices[KKPIndicesMax];
            for (Square ksq1 = SQ11; ksq1 < SquareNum; ++ksq1) {
                for (EvalIndex i = (EvalIndex)0; i < fe_end; ++i) {
                    base.kkpIndices(indices, static_cast<Square>(ksq0), ksq1, i);
                    FOO(indices, base.oneArrayKKP, grad.kkp_grad[ksq0][ksq1][i]);
                }
            }
        }
    }
#undef FOO
}

inline void printEvalTable(const Square ksq, const int p0, const int p1_base, const bool isTurn) {
    for (Rank r = Rank1; r < RankNum; ++r) {
        for (File f = File9; File1 <= f; --f) {
            const Square sq = makeSquare(f, r);
            printf("%5d", Evaluator::KPP[ksq][p0][p1_base + sq][isTurn]);
        }
        printf("\n");
    }
    printf("\n");
    fflush(stdout);
}

#endif

#endif // #ifndef APERY_LEARNER_HPP
