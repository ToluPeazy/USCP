//
// Copyright (c) 2019 Maxime Pinard
//
// Distributed under the MIT license
// See accompanying file LICENSE or copy at
// https://opensource.org/licenses/MIT
//
#include "solver/algorithms/greedy.hpp"
#include "common/utils/timer.hpp"
#include "common/data/instance.hpp"
#include "solver/data/solution.hpp"

#include <dynamic_bitset.hpp>

#include <utility>
#include <functional>

#if defined(__GNUC__)
#	define COND_LIKELY(expr) __builtin_expect(!!(expr), 1)
#	define COND_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#else
#	define COND_LIKELY(expr) (!!(expr))
#	define COND_UNLIKELY(expr) (!!(expr))
#endif

#define ensure(expr)                                                        \
	do                                                                      \
	{                                                                       \
		if(COND_UNLIKELY(!(expr)))                                          \
		{                                                                   \
			LOGGER->error("[{}:{}] failed: {}", __FILE__, __LINE__, #expr); \
			abort();                                                        \
		}                                                                   \
	} while(false)

namespace
{
	template<typename is_greater_t, bool restricted>
	[[nodiscard, gnu::hot]] uscp::greedy::report solve_report_impl(
	  const uscp::problem::instance& problem,
	  [[maybe_unused]] const dynamic_bitset<>& authorized_subsets,
	  std::shared_ptr<spdlog::logger> logger) noexcept
	{
		const is_greater_t is_greater;
		if constexpr(restricted)
		{
			assert(authorized_subsets.size() == problem.subsets_number);
		}
		SPDLOG_LOGGER_DEBUG(logger, "({}) Start building greedy solution", problem.name);
		const timer timer;

		uscp::greedy::report report(problem);
		while(!report.solution_final.cover_all_points)
		{
			size_t max_subset_number =
			  report.solution_final.selected_subsets.size(); //invalid initial value
			dynamic_bitset<> covered_points_with_max_subset(problem.points_number);
			size_t covered_points_number_with_max_subset =
			  report.solution_final.covered_points.count();
			dynamic_bitset<> new_covered_points;
			for(size_t i = 0; i < problem.subsets_number; ++i)
			{
				if constexpr(restricted)
				{
					if(!authorized_subsets.test(i))
					{
						continue;
					}
				}
				if(report.solution_final.selected_subsets[i])
				{
					// already selected
					continue;
				}

				new_covered_points = report.solution_final.covered_points;
				new_covered_points |= problem.subsets_points[i];
				const size_t new_covered_points_number = new_covered_points.count();
				if(is_greater(new_covered_points_number, covered_points_number_with_max_subset))
				{
					max_subset_number = i;
					covered_points_with_max_subset = new_covered_points;
					covered_points_number_with_max_subset = new_covered_points_number;
				}
			}

			// all subset already included or no subset add covered points
			if(max_subset_number == report.solution_final.selected_subsets.size())
			{
				logger->error("The problem has no solution");
				abort();
			}

			// update solution (faster)
			report.solution_final.selected_subsets[max_subset_number].set();
			report.solution_final.covered_points = covered_points_with_max_subset;
			report.solution_final.cover_all_points = report.solution_final.covered_points.all();

			// update solution (slower)
			//solution.compute_cover();
			//solution.compute_cost();
		}

		report.time = timer.elapsed();
		report.solution_final.compute_cover();
		ensure(report.solution_final.cover_all_points);
		SPDLOG_LOGGER_DEBUG(logger,
		                    "({}) Built greedy solution with {} subsets in {}s",
		                    problem.name,
		                    report.solution_final.selected_subsets.count(),
		                    report.time);

		return report;
	}

	template<bool restricted>
	[[nodiscard, gnu::hot]] uscp::greedy::report random_solve_report_impl(
	  uscp::random_engine& generator,
	  const uscp::problem::instance& problem,
	  [[maybe_unused]] const dynamic_bitset<>& authorized_subsets,
	  std::shared_ptr<spdlog::logger> logger) noexcept
	{
		if constexpr(restricted)
		{
			assert(authorized_subsets.size() == problem.subsets_number);
		}
		SPDLOG_LOGGER_DEBUG(logger, "({}) Start building random greedy solution", problem.name);
		const timer timer;

		uscp::greedy::report report(problem);
		size_t equal_counter = 0;
		std::uniform_real_distribution<> dist(0.0, 1.0);
		while(!report.solution_final.cover_all_points)
		{
			size_t max_subset_number =
			  report.solution_final.selected_subsets.size(); //invalid initial value
			dynamic_bitset<> covered_points_with_max_subset(problem.points_number);
			size_t covered_points_number_with_max_subset =
			  report.solution_final.covered_points.count();
			dynamic_bitset<> new_covered_points;
			for(size_t i = 0; i < problem.subsets_number; ++i)
			{
				if constexpr(restricted)
				{
					if(!authorized_subsets.test(i))
					{
						continue;
					}
				}
				if(report.solution_final.selected_subsets[i])
				{
					// already selected
					continue;
				}

				new_covered_points = report.solution_final.covered_points;
				new_covered_points |= problem.subsets_points[i];
				const size_t new_covered_points_number = new_covered_points.count();
				if(new_covered_points_number > covered_points_number_with_max_subset)
				{
					max_subset_number = i;
					covered_points_with_max_subset = new_covered_points;
					covered_points_number_with_max_subset = new_covered_points_number;
					equal_counter = 1;
				}
				else if(new_covered_points_number == covered_points_number_with_max_subset)
				{
					++equal_counter;
					if(dist(generator) < (1.0 / equal_counter))
					{
						max_subset_number = i;
						covered_points_with_max_subset = new_covered_points;
					}
				}
			}

			// all subset already included or no subset add covered points
			if(max_subset_number == report.solution_final.selected_subsets.size())
			{
				logger->error("The problem has no solution");
				abort();
			}

			// update solution (faster)
			report.solution_final.selected_subsets[max_subset_number].set();
			report.solution_final.covered_points = covered_points_with_max_subset;
			report.solution_final.cover_all_points = report.solution_final.covered_points.all();

			// update solution (slower)
			//solution.compute_cover();
			//solution.compute_cost();
		}

		report.time = timer.elapsed();
		report.solution_final.compute_cover();
		ensure(report.solution_final.cover_all_points);
		SPDLOG_LOGGER_DEBUG(logger,
		                    "({}) Built random greedy solution with {} subsets in {}s",
		                    problem.name,
		                    report.solution_final.selected_subsets.count(),
		                    report.time);

		return report;
	}
} // namespace

uscp::greedy::report::report(const uscp::problem::instance& problem) noexcept
  : solution_final(problem), time()
{
}

uscp::greedy::report_serial uscp::greedy::report::serialize() const noexcept
{
	report_serial serial;
	serial.solution_final = solution_final.serialize();
	serial.time = time;
	return serial;
}

bool uscp::greedy::report::load(const uscp::greedy::report_serial& serial) noexcept
{
	if(!solution_final.load(serial.solution_final))
	{
		LOGGER->warn("Failed to load solution");
		return false;
	}
	time = serial.time;

	return true;
}

uscp::solution uscp::greedy::solve(const uscp::problem::instance& problem,
                                   std::shared_ptr<spdlog::logger> logger) noexcept
{
	return solve_report(problem, std::move(logger)).solution_final;
}

uscp::greedy::report uscp::greedy::solve_report(const uscp::problem::instance& problem,
                                                std::shared_ptr<spdlog::logger> logger) noexcept
{
	return solve_report_impl<std::greater<>, false>(problem, dynamic_bitset<>{}, std::move(logger));
}

uscp::solution uscp::greedy::rsolve(const uscp::problem::instance& problem,
                                    std::shared_ptr<spdlog::logger> logger) noexcept
{
	return rsolve_report(problem, std::move(logger)).solution_final;
}

uscp::greedy::report uscp::greedy::rsolve_report(const uscp::problem::instance& problem,
                                                 std::shared_ptr<spdlog::logger> logger) noexcept
{
	return solve_report_impl<std::greater_equal<>, false>(
	  problem, dynamic_bitset<>{}, std::move(logger));
}

uscp::solution uscp::greedy::random_solve(uscp::random_engine& generator,
                                          const uscp::problem::instance& problem,
                                          std::shared_ptr<spdlog::logger> logger) noexcept
{
	return random_solve_report(generator, problem, std::move(logger)).solution_final;
}

uscp::greedy::report uscp::greedy::random_solve_report(
  uscp::random_engine& generator,
  const uscp::problem::instance& problem,
  std::shared_ptr<spdlog::logger> logger) noexcept
{
	return random_solve_report_impl<false>(
	  generator, problem, dynamic_bitset<>{}, std::move(logger));
}

uscp::solution uscp::greedy::restricted_solve(const uscp::problem::instance& problem,
                                              const dynamic_bitset<>& authorized_subsets,
                                              std::shared_ptr<spdlog::logger> logger) noexcept
{
	return restricted_solve_report(problem, authorized_subsets, std::move(logger)).solution_final;
}

uscp::greedy::report uscp::greedy::restricted_solve_report(
  const uscp::problem::instance& problem,
  const dynamic_bitset<>& authorized_subsets,
  std::shared_ptr<spdlog::logger> logger) noexcept
{
	return solve_report_impl<std::greater<>, true>(problem, authorized_subsets, std::move(logger));
}

uscp::solution uscp::greedy::restricted_rsolve(const uscp::problem::instance& problem,
                                               const dynamic_bitset<>& authorized_subsets,
                                               std::shared_ptr<spdlog::logger> logger) noexcept
{
	return restricted_rsolve_report(problem, authorized_subsets, std::move(logger)).solution_final;
}

uscp::greedy::report uscp::greedy::restricted_rsolve_report(
  const uscp::problem::instance& problem,
  const dynamic_bitset<>& authorized_subsets,
  std::shared_ptr<spdlog::logger> logger) noexcept
{
	return solve_report_impl<std::greater_equal<>, true>(
	  problem, authorized_subsets, std::move(logger));
}

uscp::greedy::report uscp::greedy::expand(const uscp::greedy::report& reduced_report) noexcept
{
	if(!reduced_report.solution_final.problem.reduction.has_value())
	{
		LOGGER->error("Tried to expand report of non-reduced instance");
		return reduced_report;
	}
	report expanded_report(*reduced_report.solution_final.problem.reduction->parent_instance);
	expanded_report.solution_final = expand(reduced_report.solution_final);
	expanded_report.time = reduced_report.time;
	return expanded_report;
}

uscp::solution uscp::greedy::restricted_random_solve(
  uscp::random_engine& generator,
  const uscp::problem::instance& problem,
  const dynamic_bitset<>& authorized_subsets,
  std::shared_ptr<spdlog::logger> logger) noexcept
{
	return restricted_random_solve_report(generator, problem, authorized_subsets, std::move(logger))
	  .solution_final;
}

uscp::greedy::report uscp::greedy::restricted_random_solve_report(
  uscp::random_engine& generator,
  const uscp::problem::instance& problem,
  const dynamic_bitset<>& authorized_subsets,
  std::shared_ptr<spdlog::logger> logger) noexcept
{
	return random_solve_report_impl<true>(
	  generator, problem, authorized_subsets, std::move(logger));
}
