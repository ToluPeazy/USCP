//
// Copyright (c) 2019 Maxime Pinard
//
// Distributed under the MIT license
// See accompanying file LICENSE or copy at
// https://opensource.org/licenses/MIT
//
#include "solver/algorithms/rwls.hpp"
#include "solver/data/solution.hpp"
#include "common/utils/logger.hpp"
#include "common/utils/timer.hpp"

#include <cassert>
#include <algorithm>
#include <utility>

#define ensure(expr)                                                        \
	do                                                                      \
	{                                                                       \
		if(!(expr))                                                         \
		{                                                                   \
			LOGGER->error("[{}:{}] failed: {}", __FILE__, __LINE__, #expr); \
			abort();                                                        \
		}                                                                   \
	} while(false)

#define NDEBUG_SCORE
#if !defined(NDEBUG) && !defined(NDEBUG_SCORE)
#	define assert_score(expr) assert(expr)
#else
#	define assert_score(expr) static_cast<void>(0)
#endif

uscp::rwls::report::report(const problem::instance& problem) noexcept
  : solution_initial(problem), solution_final(problem), found_at()
{
}

uscp::rwls::report_serial uscp::rwls::report::serialize() const noexcept
{
	report_serial serial;
	assert(solution_initial.problem.name == solution_final.problem.name);
	serial.solution_initial = solution_initial.serialize();
	serial.solution_final = solution_final.serialize();
	serial.steps = found_at.steps;
	serial.time = found_at.time;
	return serial;
}

bool uscp::rwls::report::load(const uscp::rwls::report_serial& serial) noexcept
{
	if(!solution_initial.load(serial.solution_initial))
	{
		LOGGER->warn("Failed to load initial solution");
		return false;
	}
	if(!solution_final.load(serial.solution_final))
	{
		LOGGER->warn("Failed to load final solution");
		return false;
	}
	found_at.steps = serial.steps;
	found_at.time = serial.time;

	return true;
}

uscp::rwls::rwls::rwls(const uscp::problem::instance& problem) noexcept
  : m_problem(problem), m_subsets_neighbors(), m_subsets_covering_points(), m_initialized(false)
{
	m_subsets_neighbors.resize(m_problem.subsets_number);
#ifdef USCP_RWLS_LOW_MEMORY_FOOTPRINT
	for(dynamic_bitset<>& subset_neighbors: m_subsets_neighbors)
	{
		subset_neighbors.resize(m_problem.subsets_number);
	}
#endif

	m_subsets_covering_points.resize(m_problem.points_number);
	for(dynamic_bitset<>& subsets_covering_point: m_subsets_covering_points)
	{
		subsets_covering_point.resize(m_problem.subsets_number);
	}
}

void uscp::rwls::rwls::initialize() noexcept
{
	generate_subsets_neighbors();
	generate_subsets_covering_points();
	m_initialized = true;
}

uscp::rwls::position uscp::rwls::rwls::improve(uscp::solution& solution,
                                               uscp::random_engine& generator,
                                               uscp::rwls::position stopping_criterion) noexcept
{
	if(!m_initialized)
	{
		initialize();
	}

	LOGGER->info("({}) Start optimising by RWLS solution with {} subsets",
	             solution.problem.name,
	             solution.selected_subsets.count());
	timer timer;
	resolution_data data(solution, generator);
	init(data);
	SPDLOG_LOGGER_DEBUG(LOGGER, "({}) RWLS inited in {}s", m_problem.name, timer.elapsed());

	timer.reset();
	size_t step = 0;
	position found_at;
	found_at.steps = 0;
	found_at.time = 0;
	while(step < stopping_criterion.steps && timer.elapsed() < stopping_criterion.time)
	{
		while(data.uncovered_points.none())
		{
			data.current_solution.compute_cover();
			assert(data.current_solution.cover_all_points);
			if(!data.current_solution.cover_all_points)
			{
				LOGGER->error("New best solution doesn't cover all points");
				abort();
			}

			data.best_solution = data.current_solution;
			found_at.steps = step;
			found_at.time = timer.elapsed();
			SPDLOG_LOGGER_DEBUG(LOGGER,
			                    "({}) RWLS new best solution with {} subsets at step {} in {}s",
			                    m_problem.name,
			                    data.best_solution.selected_subsets.count(),
			                    step,
			                    timer.elapsed());

			const size_t selected_subset = select_subset_to_remove_no_timestamp(data);
			remove_subset(data, selected_subset);
		}

		// remove subset
		const size_t subset_to_remove = select_subset_to_remove(data);
		remove_subset(data, subset_to_remove);
		data.subsets_information[subset_to_remove].timestamp = static_cast<int>(step);

		// add subset
		const size_t selected_point = select_uncovered_point(data);
		const size_t subset_to_add = select_subset_to_add(data, selected_point);
		add_subset(data, subset_to_add);

		data.subsets_information[subset_to_add].timestamp = static_cast<int>(step);
		make_tabu(data, subset_to_add);

		// update points weights
		data.uncovered_points.iterate_bits_on([&](size_t uncovered_points_bit_on) noexcept {
			assert(data.points_information[uncovered_points_bit_on].subsets_covering_in_solution
			       == 0);

			++data.points_information[uncovered_points_bit_on].weight;

			// update subsets score depending on this point weight
			// subset that can cover the point if added to solution
			m_subsets_covering_points[uncovered_points_bit_on].iterate_bits_on([&](
			  size_t subsets_covering_bit_on) noexcept {
				++data.subsets_information[subsets_covering_bit_on].score;
			});
		});
#if !defined(NDEBUG) && !defined(NDEBUG_SCORE)
		for(size_t i = 0; i < m_problem.subsets_number; ++i)
		{
			assert(data.subsets_information[i].score == compute_subset_score(data, i));
		}
#endif

		++step;
	}

	LOGGER->info("({}) Optimised RWLS solution to {} subsets in {} steps {}s",
	             m_problem.name,
	             data.best_solution.selected_subsets.count(),
	             step,
	             timer.elapsed());

	return found_at;
}

void uscp::rwls::rwls::generate_subsets_neighbors() noexcept
{
	LOGGER->info("({}) start building subsets RWLS neighbors", m_problem.name);
	timer timer;
	dynamic_bitset<> tmp; // to minimize memory allocations
#pragma omp parallel for default(none) \
  shared(m_problem, m_subsets_neighbors) private(tmp) if(m_problem.subsets_number > 8)
	for(size_t i_current_subset = 0; i_current_subset < m_problem.subsets_number;
	    ++i_current_subset)
	{
		for(size_t i_other_subset = i_current_subset + 1; i_other_subset < m_problem.subsets_number;
		    ++i_other_subset)
		{
			tmp = m_problem.subsets_points[i_current_subset];
			tmp &= m_problem.subsets_points[i_other_subset];
			if(tmp.any())
			{
#pragma omp critical
				{
#ifdef USCP_RWLS_LOW_MEMORY_FOOTPRINT
					m_subsets_neighbors[i_current_subset].set(i_other_subset);
					m_subsets_neighbors[i_other_subset].set(i_current_subset);
#else
					m_subsets_neighbors[i_current_subset].push_back(i_other_subset);
					m_subsets_neighbors[i_other_subset].push_back(i_current_subset);
#endif
				}
			}
		}
	}
	LOGGER->info("({}) Built subsets neighbors in {}s", m_problem.name, timer.elapsed());
}

void uscp::rwls::rwls::generate_subsets_covering_points() noexcept
{
	// no parallel: writing (event different elements) in bitsets is not thread safe
	for(size_t i = 0; i < m_problem.subsets_number; ++i)
	{
		m_problem.subsets_points[i].iterate_bits_on([&](size_t bit_on) noexcept {
			m_subsets_covering_points[bit_on].set(i);
		});
	}
}

uscp::rwls::rwls::resolution_data::resolution_data(solution& solution,
                                                   random_engine& generator_) noexcept
  : generator(generator_)
  , best_solution(solution)
  , current_solution(solution)
  , uncovered_points(solution.problem.points_number)
  , points_information()
  , subsets_information()
  , tabu_subsets()
{
	points_information.resize(solution.problem.points_number);
	subsets_information.resize(solution.problem.subsets_number);
}

int uscp::rwls::rwls::compute_subset_score(const resolution_data& data,
                                           size_t subset_number) noexcept
{
	assert(subset_number < m_problem.subsets_number);

	int subset_score = 0;
	if(data.current_solution.selected_subsets[subset_number])
	{
		// if in solution, gain score for points covered only by the subset
		m_problem.subsets_points[subset_number].iterate_bits_on([&](size_t bit_on) noexcept {
			if(data.points_information[bit_on].subsets_covering_in_solution == 1)
			{
				assert(!data.uncovered_points[bit_on]);
				subset_score -= data.points_information[bit_on].weight;
			}
		});
		assert(subset_score <= 0);
	}
	else
	{
		// if out of solution, gain score for uncovered points it can cover
		m_problem.subsets_points[subset_number].iterate_bits_on([&](size_t bit_on) noexcept {
			if(data.points_information[bit_on].subsets_covering_in_solution == 0)
			{
				assert(data.uncovered_points[bit_on]);
				subset_score += data.points_information[bit_on].weight;
			}
			else
			{
				assert(!data.uncovered_points[bit_on]);
			}
		});
		assert(subset_score >= 0);
	}

	return subset_score;
}

void uscp::rwls::rwls::init(uscp::rwls::rwls::resolution_data& data) noexcept
{
	// points information
	dynamic_bitset<> tmp;
#pragma omp parallel for default(none) shared(data) private(tmp)
	for(size_t i = 0; i < m_problem.points_number; ++i)
	{
		tmp = m_subsets_covering_points[i];
		tmp &= data.current_solution.selected_subsets;
		data.points_information[i].subsets_covering_in_solution = tmp.count();
	}

	// subset scores
#pragma omp parallel for default(none) shared(data)
	for(size_t i = 0; i < m_problem.subsets_number; ++i)
	{
		data.subsets_information[i].score = compute_subset_score(data, i);
		assert(data.current_solution.selected_subsets[i] ? data.subsets_information[i].score <= 0
		                                                 : data.subsets_information[i].score >= 0);
	}
}

void uscp::rwls::rwls::add_subset(uscp::rwls::rwls::resolution_data& data,
                                  size_t subset_number) noexcept
{
	assert(subset_number < m_problem.subsets_number);
	assert(!data.current_solution.selected_subsets[subset_number]);
	assert(data.subsets_information[subset_number].score >= 0);

	// update points information
	dynamic_bitset<> points_newly_covered(m_problem.points_number);
	dynamic_bitset<> point_now_covered_twice(m_problem.points_number);
	m_problem.subsets_points[subset_number].iterate_bits_on([&](size_t bit_on) noexcept {
		assert(
		  data.points_information[bit_on].subsets_covering_in_solution
		  == (m_subsets_covering_points[bit_on] & data.current_solution.selected_subsets).count());
		++data.points_information[bit_on].subsets_covering_in_solution;
		if(data.points_information[bit_on].subsets_covering_in_solution == 1)
		{
			points_newly_covered.set(bit_on);
		}
		else if(data.points_information[bit_on].subsets_covering_in_solution == 2)
		{
			point_now_covered_twice.set(bit_on);
		}
	});

	// add subset to solution
	data.current_solution.selected_subsets.set(subset_number);
	data.uncovered_points -= m_problem.subsets_points[subset_number];

	// update score
	data.subsets_information[subset_number].score = -data.subsets_information[subset_number].score;
	assert_score(data.subsets_information[subset_number].score
	             == compute_subset_score(data, subset_number));

	// update neighbors
	dynamic_bitset<> tmp; // to minimize memory allocations
#ifdef USCP_RWLS_LOW_MEMORY_FOOTPRINT
	m_subsets_neighbors[subset_number].iterate_bits_on([&](size_t i_neighbor) {
#else
#	pragma omp parallel for default(none) \
	  shared(data, subset_number, point_now_covered_twice, points_newly_covered) private(tmp)
	for(size_t i = 0; i < m_subsets_neighbors[subset_number].size(); ++i)
	{
		const size_t i_neighbor = m_subsets_neighbors[subset_number][i];
#endif
		data.subsets_information[i_neighbor].canAddToSolution = true;
		if(data.current_solution.selected_subsets[i_neighbor])
		{
			// lost score because it is no longer the only one to cover these points
			tmp = point_now_covered_twice;
			tmp &= m_problem.subsets_points[i_neighbor];
			tmp.iterate_bits_on([&](size_t bit_on) noexcept {
				data.subsets_information[i_neighbor].score +=
				  data.points_information[bit_on].weight;
			});
		}
		else
		{
			// lost score because these points are now covered in the solution
			tmp = points_newly_covered;
			tmp &= m_problem.subsets_points[i_neighbor];
			tmp.iterate_bits_on([&](size_t bit_on) noexcept {
				data.subsets_information[i_neighbor].score -=
				  data.points_information[bit_on].weight;
			});
		}
		assert_score(data.subsets_information[i_neighbor].score
		             == compute_subset_score(data, i_neighbor));
	}
#ifdef USCP_RWLS_LOW_MEMORY_FOOTPRINT
	);
#endif
}

void uscp::rwls::rwls::remove_subset(uscp::rwls::rwls::resolution_data& data,
                                     size_t subset_number) noexcept
{
	assert(subset_number < m_problem.subsets_number);
	assert(data.current_solution.selected_subsets[subset_number]);
	assert(data.subsets_information[subset_number].score <= 0);

	// update points information
	dynamic_bitset<> points_newly_uncovered(m_problem.points_number);
	dynamic_bitset<> point_now_covered_once(m_problem.points_number);
	m_problem.subsets_points[subset_number].iterate_bits_on([&](size_t bit_on) noexcept {
		assert(data.points_information[bit_on].subsets_covering_in_solution > 0);
		assert(
		  data.points_information[bit_on].subsets_covering_in_solution
		  == (m_subsets_covering_points[bit_on] & data.current_solution.selected_subsets).count());
		--data.points_information[bit_on].subsets_covering_in_solution;
		if(data.points_information[bit_on].subsets_covering_in_solution == 0)
		{
			points_newly_uncovered.set(bit_on);
		}
		else if(data.points_information[bit_on].subsets_covering_in_solution == 1)
		{
			point_now_covered_once.set(bit_on);
		}
	});

	// remove subset from solution
	data.current_solution.selected_subsets.reset(subset_number);
	assert((data.uncovered_points & points_newly_uncovered).none());
	data.uncovered_points |= points_newly_uncovered;

	// update score
	data.subsets_information[subset_number].score = -data.subsets_information[subset_number].score;
	assert_score(data.subsets_information[subset_number].score
	             == compute_subset_score(data, subset_number));

	data.subsets_information[subset_number].canAddToSolution = false;

	// update neighbors
	dynamic_bitset<> tmp; // to minimize memory allocations
#ifdef USCP_RWLS_LOW_MEMORY_FOOTPRINT
	m_subsets_neighbors[subset_number].iterate_bits_on([&](size_t i_neighbor) {
#else
#	pragma omp parallel for default(none) \
	  shared(data, subset_number, points_newly_uncovered, point_now_covered_once) private(tmp)
	for(size_t i = 0; i < m_subsets_neighbors[subset_number].size(); ++i)
	{
		const size_t i_neighbor = m_subsets_neighbors[subset_number][i];
#endif
		data.subsets_information[i_neighbor].canAddToSolution = true;
		if(data.current_solution.selected_subsets[i_neighbor])
		{
			// gain score because it is no the only one to cover these points
			tmp = point_now_covered_once;
			tmp &= m_problem.subsets_points[i_neighbor];
			tmp.iterate_bits_on([&](size_t bit_on) noexcept {
				data.subsets_information[i_neighbor].score -=
				  data.points_information[bit_on].weight;
			});
		}
		else
		{
			// gain score because these points are now uncovered in the solution
			tmp = points_newly_uncovered;
			tmp &= m_problem.subsets_points[i_neighbor];
			tmp.iterate_bits_on([&](size_t bit_on) noexcept {
				data.subsets_information[i_neighbor].score +=
				  data.points_information[bit_on].weight;
			});
		}

		assert_score(data.subsets_information[i_neighbor].score
		             == compute_subset_score(data, i_neighbor));
	}
#ifdef USCP_RWLS_LOW_MEMORY_FOOTPRINT
	);
#endif
}

void uscp::rwls::rwls::make_tabu(uscp::rwls::rwls::resolution_data& data,
                                 size_t subset_number) noexcept
{
	assert(subset_number < m_problem.subsets_number);
	data.tabu_subsets.push_back(subset_number);
	if(data.tabu_subsets.size() > TABU_LIST_LENGTH)
	{
		data.tabu_subsets.pop_front();
	}
}

bool uscp::rwls::rwls::is_tabu(const uscp::rwls::rwls::resolution_data& data,
                               size_t subset_number) noexcept
{
	assert(subset_number < m_problem.subsets_number);
	return std::find(std::cbegin(data.tabu_subsets), std::cend(data.tabu_subsets), subset_number)
	       != std::cend(data.tabu_subsets);
}

size_t uscp::rwls::rwls::select_subset_to_remove_no_timestamp(
  const uscp::rwls::rwls::resolution_data& data) noexcept
{
	assert(data.current_solution.selected_subsets.any());
	size_t selected_subset = data.current_solution.selected_subsets.find_first();
	int best_score = data.subsets_information[selected_subset].score;
	data.current_solution.selected_subsets.iterate_bits_on([&](size_t bit_on) noexcept {
		if(data.subsets_information[bit_on].score > best_score)
		{
			best_score = data.subsets_information[bit_on].score;
			selected_subset = bit_on;
		}
	});
	ensure(data.current_solution.selected_subsets.test(selected_subset));
	return selected_subset;
}

size_t uscp::rwls::rwls::select_subset_to_remove(
  const uscp::rwls::rwls::resolution_data& data) noexcept
{
	assert(data.current_solution.selected_subsets.any());
	size_t remove_subset = data.current_solution.selected_subsets.find_first();
	std::pair<int, int> best_score_minus_timestamp(
	  data.subsets_information[remove_subset].score,
	  -data.subsets_information[remove_subset].timestamp);
	data.current_solution.selected_subsets.iterate_bits_on([&](size_t bit_on) noexcept {
		const std::pair<int, int> current_score_timestamp(
		  data.subsets_information[bit_on].score, -data.subsets_information[bit_on].timestamp);
		if(current_score_timestamp > best_score_minus_timestamp && !is_tabu(data, bit_on))
		{
			best_score_minus_timestamp = current_score_timestamp;
			remove_subset = bit_on;
		}
	});
	ensure(data.current_solution.selected_subsets.test(remove_subset));
	return remove_subset;
}

size_t uscp::rwls::rwls::select_subset_to_add(const uscp::rwls::rwls::resolution_data& data,
                                              size_t point_to_cover) noexcept
{
	assert(point_to_cover < m_problem.points_number);
	assert(data.uncovered_points.test(point_to_cover));

	const dynamic_bitset<> subsets_covering_not_selected =
	  m_subsets_covering_points[point_to_cover] & ~data.current_solution.selected_subsets;
	assert(subsets_covering_not_selected.any());
	if(subsets_covering_not_selected.none())
	{
		LOGGER->error("No subset not selected cover this point, problem not preprocessed?");
		abort();
	}
	size_t add_subset = subsets_covering_not_selected.find_first();
	bool add_subset_is_tabu = is_tabu(data, add_subset);
	std::pair<int, int> best_score_minus_timestamp(data.subsets_information[add_subset].score,
	                                               -data.subsets_information[add_subset].timestamp);
	subsets_covering_not_selected.iterate_bits_on([&](size_t bit_on) noexcept {
		if(!data.subsets_information[bit_on].canAddToSolution)
		{
			return;
		}
		const std::pair<int, int> current_score_minus_timestamp(
		  data.subsets_information[bit_on].score, -data.subsets_information[bit_on].timestamp);
		if(add_subset_is_tabu)
		{
			best_score_minus_timestamp = current_score_minus_timestamp;
			add_subset = bit_on;
			add_subset_is_tabu = is_tabu(data, add_subset);
			return;
		}
		if(current_score_minus_timestamp > best_score_minus_timestamp && !is_tabu(data, bit_on))
		{
			best_score_minus_timestamp = current_score_minus_timestamp;
			add_subset = bit_on;
		}
	});

	if(is_tabu(data, add_subset))
	{
		LOGGER->warn("Selected subset is tabu");
	}
	ensure(!data.current_solution.selected_subsets.test(add_subset));
	return add_subset;
}

size_t uscp::rwls::rwls::select_uncovered_point(uscp::rwls::rwls::resolution_data& data) noexcept
{
	assert(data.uncovered_points.count() > 0);
	size_t selected_point = 0;
	std::uniform_int_distribution<size_t> uncovered_point_dist(0, data.uncovered_points.count());
	const size_t selected_point_number = uncovered_point_dist(data.generator);
	size_t current_point_number = 0;
	data.uncovered_points.iterate_bits_on([&](size_t bit_on) noexcept {
		if(++current_point_number >= selected_point_number)
		{
			selected_point = bit_on;
			return false;
		}
		return true;
	});
	ensure(data.uncovered_points.test(selected_point));
	return selected_point;
}

uscp::solution uscp::rwls::improve(const uscp::solution& solution_initial,
                                   random_engine& generator,
                                   position stopping_criterion)
{
	rwls rwls(solution_initial.problem);
	rwls.initialize();
	solution solution_final(solution_initial);
	rwls.improve(solution_final, generator, stopping_criterion);
	return solution_final;
}

uscp::rwls::report uscp::rwls::improve_report(const uscp::solution& solution_initial,
                                              uscp::random_engine& generator,
                                              position stopping_criterion)
{
	rwls rwls(solution_initial.problem);
	rwls.initialize();
	report report(solution_initial.problem);
	report.solution_initial = solution_initial;
	report.solution_final = solution_initial;
	report.found_at = rwls.improve(report.solution_final, generator, stopping_criterion);
	return report;
}

uscp::rwls::report uscp::rwls::expand(const uscp::rwls::report& reduced_report) noexcept
{
	if(!reduced_report.solution_final.problem.reduction.has_value())
	{
		LOGGER->error("Tried to expand report of non-reduced instance");
		return reduced_report;
	}
	report expanded_report(*reduced_report.solution_final.problem.reduction->parent_instance);
	expanded_report.solution_initial = expand(reduced_report.solution_initial);
	expanded_report.solution_final = expand(reduced_report.solution_final);
	expanded_report.found_at = reduced_report.found_at;
	return expanded_report;
}
