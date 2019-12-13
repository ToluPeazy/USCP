//
// Copyright (c) 2019 Maxime Pinard
//
// Distributed under the MIT license
// See accompanying file LICENSE or copy at
// https://opensource.org/licenses/MIT
//
#ifndef USCP_GREEDY_HPP
#define USCP_GREEDY_HPP

#include "common/data/solution.hpp"
#include "common/algorithms/greedy.hpp"

#include <nlohmann/json.hpp>

namespace uscp::greedy
{
	struct report final
	{
		solution solution_final;
		double time;

		explicit report(const problem::instance& problem) noexcept;
		report(const report&) = default;
		report(report&&) noexcept = default;
		report& operator=(const report& other) = default;
		report& operator=(report&& other) noexcept = default;

		[[nodiscard]] report_serial serialize() const noexcept;
		bool load(const report_serial& serial) noexcept;
	};

	[[nodiscard, gnu::hot]] solution solve(const problem::instance& problem) noexcept;

	[[nodiscard, gnu::hot]] report solve_report(const problem::instance& problem) noexcept;

	[[nodiscard, gnu::hot]] solution restricted_solve(
	  const problem::instance& problem,
	  const dynamic_bitset<>& authorized_subsets) noexcept;

	[[nodiscard, gnu::hot]] report restricted_solve_report(
	  const problem::instance& problem,
	  const dynamic_bitset<>& authorized_subsets) noexcept;

	[[nodiscard]] report expand(const report& reduced_report) noexcept;
} // namespace uscp::greedy

#endif //USCP_GREEDY_HPP
