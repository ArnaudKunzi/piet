// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "hdfe/dof.h"

#include <algorithm>
#include <numeric>
#include <unordered_set>

namespace cecon {

namespace {

struct UnionFind {
    std::vector<int> parent;
    explicit UnionFind(int n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(int a, int b) { parent[find(a)] = find(b); }
};

// Connected components of the bipartite graph linking FE1 levels to FE2 levels.
int connected_components(const Factor& f1, const Factor& f2) {
    const int K1 = f1.n_levels, K2 = f2.n_levels;
    UnionFind uf(K1 + K2);
    const int64_t n = static_cast<int64_t>(f1.codes.size());
    for (int64_t i = 0; i < n; ++i) uf.unite(f1.codes[i], K1 + f2.codes[i]);
    std::unordered_set<int> roots;
    roots.reserve(K1 + K2);
    for (int v = 0; v < K1 + K2; ++v) roots.insert(uf.find(v));
    return static_cast<int>(roots.size());
}

}  // namespace

AbsorbedDof absorbed_dof(const std::vector<Factor>& fe, const std::vector<char>& nested) {
    const size_t G = fe.size();
    std::vector<int> M(G, 0);
    std::vector<size_t> free_fe;  // effects not nested in a cluster, in order
    AbsorbedDof out;
    for (size_t g = 0; g < G; ++g) {
        if (g < nested.size() && nested[g]) {
            M[g] = fe[g].n_levels;
            out.df_a_nested += M[g];
        } else {
            free_fe.push_back(g);
        }
    }
    if (free_fe.size() > 1 || (!free_fe.empty() && out.df_a_nested > 0))
        for (size_t i = (out.df_a_nested > 0 ? 0 : 1); i < free_fe.size(); ++i) M[free_fe[i]] = 1;
    for (size_t i = 0; i < free_fe.size(); ++i)
        for (size_t j = i + 1; j < free_fe.size(); ++j) {
            const int c = connected_components(fe[free_fe[i]], fe[free_fe[j]]);
            M[free_fe[j]] = std::max(M[free_fe[j]], c);
        }
    for (size_t g = 0; g < G; ++g) out.df_a += fe[g].n_levels - M[g];
    return out;
}

DofInfo compute_dof(int64_t n, int k_vars, const std::vector<Factor>& fe) {
    DofInfo d;
    d.n = n;
    d.k_vars = k_vars;
    for (const Factor& f : fe) d.fe_levels.push_back(f.n_levels);

    int sum_levels = 0;
    for (int L : d.fe_levels) sum_levels += L;

    // 1 and 2 effects are exact; from the third on reghdfe's pairwise components
    // only bound the redundancy (its "?" rows), and so does this.
    d.k_fe = absorbed_dof(fe, {}).df_a;
    d.fe_exact = fe.size() <= 2;

    d.fe_redundant = sum_levels - d.k_fe;
    d.dof_resid = n - k_vars - d.k_fe;
    return d;
}

}  // namespace cecon
