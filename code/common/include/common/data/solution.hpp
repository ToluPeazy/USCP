//
// Copyright (c) 2019 Maxime Pinard
//
// Distributed under the MIT license
// See accompanying file LICENSE or copy at
// https://opensource.org/licenses/MIT
//
#ifndef USCP_COMMON_SOLUTION_HPP
#define USCP_COMMON_SOLUTION_HPP

#include "common/data/instance.hpp"

#include <dynamic_bitset.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <vector>
#include <ostream>

namespace uscp
{
	struct solution_serial final
	{
		problem::instance_serial problem;
		std::vector<size_t> selected_subsets;
	};
	void to_json(nlohmann::json& j, const solution_serial& solution_serial);
	void from_json(const nlohmann::json& j, solution_serial& solution_serial);

	struct solution final
	{
		const problem::instance& problem;
		dynamic_bitset<> selected_subsets;
		dynamic_bitset<> covered_points;
		bool cover_all_points;

		explicit solution(const problem::instance& problem);
		solution(const solution&) = default;
		solution(solution&&) noexcept = default;
		solution& operator=(const solution& other);
		solution& operator=(solution&& other) noexcept;

		void compute_cover() noexcept;

		[[nodiscard]] solution_serial serialize() const noexcept;
		bool load(const solution_serial& serial) noexcept;
	};
	void to_json(nlohmann::json& j, const solution& solution);
	std::ostream& operator<<(std::ostream& os, const solution& solution);
} // namespace uscp

#endif //USCP_COMMON_SOLUTION_HPP
