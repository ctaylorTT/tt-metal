// SPDX-FileCopyrightText: © 2025 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <tuple>

namespace tt::tt_fabric {

// Array-based iteration helpers (for structures with array storage + get<I>() methods)
// These work with our new array-backed structures like EdmChannelWorkerInterfaceTuple

template <typename ArrayLike, typename F, size_t... Is>
constexpr void array_like_for_each_constexpr_impl(ArrayLike&& arr, F&& f, std::index_sequence<Is...>) {
    // Call get<I>() for each index with compile-time index
    (f(arr.template get<Is>(), std::integral_constant<size_t, Is>{}), ...);
}

template <size_t N, typename ArrayLike, typename F>
constexpr void array_like_for_each_constexpr(ArrayLike&& arr, F&& f) {
    array_like_for_each_constexpr_impl(
        std::forward<ArrayLike>(arr),
        std::forward<F>(f),
        std::make_index_sequence<N>{});
}

template <typename ArrayLike, typename F, size_t... Is>
constexpr void array_like_for_each_impl(ArrayLike&& arr, F&& f, std::index_sequence<Is...>) {
    // Call get<I>() for each index with runtime index
    (f(arr.template get<Is>(), Is), ...);
}

template <size_t N, typename ArrayLike, typename F>
constexpr void array_like_for_each(ArrayLike&& arr, F&& f) {
    array_like_for_each_impl(
        std::forward<ArrayLike>(arr),
        std::forward<F>(f),
        std::make_index_sequence<N>{});
}

}  // namespace tt::tt_fabric
