//
// Copyright (c) 2019 Maxime Pinard
//
// Distributed under the MIT license
// See accompanying file LICENSE or copy at
// https://opensource.org/licenses/MIT
//
#include "solver/data/instance.hpp"
#include "solver/algorithms/greedy.hpp"
#include "solver/algorithms/rwls.hpp"
#include "solver/algorithms/memetic.hpp"
#include "solver/algorithms/crossovers/identity.hpp"
#include "solver/algorithms/crossovers/merge.hpp"
#include "solver/algorithms/crossovers/greedy_merge.hpp"
#include "solver/algorithms/crossovers/subproblem_random.hpp"
#include "solver/algorithms/crossovers/extended_subproblem_random.hpp"
#include "solver/algorithms/crossovers/subproblem_greedy.hpp"
#include "solver/algorithms/crossovers/extended_subproblem_greedy.hpp"
#include "solver/algorithms/crossovers/subproblem_rwls.hpp"
#include "solver/algorithms/crossovers/extended_subproblem_rwls.hpp"
#include "solver/algorithms/wcrossover/reset.hpp"
#include "solver/algorithms/wcrossover/keep.hpp"
#include "solver/algorithms/wcrossover/average.hpp"
#include "solver/algorithms/wcrossover/mix_random.hpp"
#include "solver/algorithms/wcrossover/add.hpp"
#include "solver/algorithms/wcrossover/difference.hpp"
#include "solver/algorithms/wcrossover/max.hpp"
#include "solver/algorithms/wcrossover/min.hpp"
#include "solver/algorithms/wcrossover/minmax.hpp"
#include "solver/algorithms/wcrossover/shuffle.hpp"
#include "solver/data/instances.hpp"
#include "common/utils/logger.hpp"
#include "common/utils/random.hpp"
#include "common/data/instance.hpp"
#include "common/data/instances.hpp"
#include "common/data/solution.hpp"
#include "git_info.hpp"

#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <vector>
#include <limits>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	constexpr bool CHECK_INSTANCES = false;

	struct program_options final
	{
		// registered instances
		std::vector<std::string> instances;

		// unknown instance
		std::string instance_type;
		std::string instance_path;
		std::string instance_name;

		// general options
		std::string output_prefix = "solver_out_";
		size_t repetitions = 1;

		// greedy options
		bool greedy = false;

		// rwls options
		bool rwls = false;
		uscp::rwls::position rwls_stop;

		// memetic options
		bool memetic = false;
		uscp::memetic::config memetic_config;
		std::string memetic_crossover;
		std::string memetic_wcrossover;
	};

	template<typename... Crossovers>
	struct crossovers
	{
	};

	template<typename... WCrossovers>
	struct wcrossovers
	{
	};

	template<typename Crossover>
	struct crossover
	{
		typedef Crossover type;
	};

	template<typename WCrossover>
	struct wcrossover
	{
		typedef WCrossover type;
	};

	template<typename Lambda, typename Crossover, typename WCrossover>
	bool foreach_crossover_wcrossover(Lambda&& lambda,
	                                  crossovers<Crossover>,
	                                  wcrossovers<WCrossover>) noexcept
	{
		return lambda(crossover<Crossover>{}, wcrossover<WCrossover>{});
	}

	template<typename Lambda,
	         typename Crossover,
	         typename WCrossover,
	         typename... WCrossovers,
	         typename = std::enable_if_t<sizeof...(WCrossovers) >= 1>>
	bool foreach_crossover_wcrossover(Lambda&& lambda,
	                                  crossovers<Crossover>,
	                                  wcrossovers<WCrossover, WCrossovers...>) noexcept
	{
		if(foreach_crossover_wcrossover(
		     std::forward<Lambda>(lambda), crossovers<Crossover>{}, wcrossovers<WCrossover>{}))
		{
			return foreach_crossover_wcrossover(
			  std::forward<Lambda>(lambda), crossovers<Crossover>{}, wcrossovers<WCrossovers...>{});
		}
		return false;
	}

	template<typename Lambda,
	         typename Crossover,
	         typename... Crossovers,
	         typename... WCrossovers,
	         typename = std::enable_if_t<sizeof...(Crossovers) >= 1>>
	bool foreach_crossover_wcrossover(Lambda&& lambda,
	                                  crossovers<Crossover, Crossovers...>,
	                                  wcrossovers<WCrossovers...>) noexcept
	{
		if(foreach_crossover_wcrossover(
		     std::forward<Lambda>(lambda), crossovers<Crossover>{}, wcrossovers<WCrossovers...>{}))
		{
			return foreach_crossover_wcrossover(std::forward<Lambda>(lambda),
			                                    crossovers<Crossovers...>{},
			                                    wcrossovers<WCrossovers...>{});
		}
		return false;
	}

	using all_crossovers = crossovers<uscp::crossover::identity,
	                                  uscp::crossover::merge,
	                                  uscp::crossover::greedy_merge,
	                                  uscp::crossover::subproblem_random,
	                                  uscp::crossover::extended_subproblem_random,
	                                  uscp::crossover::subproblem_greedy,
	                                  uscp::crossover::extended_subproblem_greedy,
	                                  uscp::crossover::subproblem_rwls,
	                                  uscp::crossover::extended_subproblem_rwls>;

	using all_wcrossovers = wcrossovers<uscp::wcrossover::reset,
	                                    uscp::wcrossover::keep,
	                                    uscp::wcrossover::average,
	                                    uscp::wcrossover::mix_random,
	                                    uscp::wcrossover::add,
	                                    uscp::wcrossover::difference,
	                                    uscp::wcrossover::max,
	                                    uscp::wcrossover::min,
	                                    uscp::wcrossover::minmax,
	                                    uscp::wcrossover::shuffle>;

	template<typename Lambda>
	bool forall_crossover_wcrossover(Lambda&& lambda) noexcept
	{
		return foreach_crossover_wcrossover(
		  std::forward<Lambda>(lambda), all_crossovers{}, all_wcrossovers{});
	}

	std::optional<nlohmann::json> process_instance(program_options& program_options,
	                                               uscp::random_engine& generator,
	                                               const uscp::problem::instance& instance_base,
	                                               bool reduce) noexcept
	{
		if(!uscp::problem::has_solution(instance_base))
		{
			LOGGER->error("Instance {} have no solution", instance_base.name);
			return {};
		}

		uscp::problem::instance instance;
		if(reduce)
		{
			instance = uscp::problem::reduce_cache(instance_base);
		}
		else
		{
			instance = instance_base;
		}

		nlohmann::json data_instance;
		data_instance["instance"] = instance_base.serialize();
		if(program_options.greedy && !program_options.rwls)
		{
			uscp::greedy::report greedy_report = uscp::greedy::solve_report(instance);
			if(reduce)
			{
				uscp::greedy::report expanded_greedy_report = uscp::greedy::expand(greedy_report);
				if(!expanded_greedy_report.solution_final.cover_all_points)
				{
					LOGGER->error("Expanded greedy solution doesn't cover all points");
					return {};
				}
				LOGGER->info("({}) Expanded greedy solution to {} subsets",
				             instance_base.name,
				             expanded_greedy_report.solution_final.selected_subsets.count());
				LOGGER->info("({}) Greedy found solution with {} subsets",
				             instance_base.name,
				             expanded_greedy_report.solution_final.selected_subsets.count());
				data_instance["greedy"] = expanded_greedy_report.serialize();
			}
			else
			{
				LOGGER->info("({}) Greedy found solution with {} subsets",
				             instance_base.name,
				             greedy_report.solution_final.selected_subsets.count());
				data_instance["greedy"] = greedy_report.serialize();
			}
		}
		if(program_options.rwls)
		{
			uscp::greedy::report greedy_report = uscp::greedy::solve_report(instance);
			if(program_options.greedy)
			{
				if(reduce)
				{
					uscp::greedy::report expanded_greedy_report =
					  uscp::greedy::expand(greedy_report);
					if(!expanded_greedy_report.solution_final.cover_all_points)
					{
						LOGGER->error("Expanded greedy solution doesn't cover all points");
						return {};
					}
					LOGGER->info("({}) Expanded greedy solution to {} subsets",
					             instance_base.name,
					             expanded_greedy_report.solution_final.selected_subsets.count());
					LOGGER->info("({}) Greedy found solution with {} subsets",
					             instance_base.name,
					             expanded_greedy_report.solution_final.selected_subsets.count());
					data_instance["greedy"] = expanded_greedy_report.serialize();
				}
				else
				{
					LOGGER->info("({}) Greedy found solution with {} subsets",
					             instance_base.name,
					             greedy_report.solution_final.selected_subsets.count());
					data_instance["greedy"] = greedy_report.serialize();
				}
			}
			std::vector<nlohmann::json> data_rwls;
			uscp::rwls::rwls rwls_manager(instance);
			rwls_manager.initialize();
			for(size_t repetition = 0; repetition < program_options.repetitions; ++repetition)
			{
				uscp::rwls::report rwls_report = rwls_manager.improve(
				  greedy_report.solution_final, generator, program_options.rwls_stop);
				if(reduce)
				{
					uscp::rwls::report expanded_rwls_report = uscp::rwls::expand(rwls_report);
					if(!expanded_rwls_report.solution_final.cover_all_points)
					{
						LOGGER->error("Expanded rwls solution doesn't cover all points");
						return {};
					}
					LOGGER->info("({}) Expanded rwls solution to {} subsets",
					             instance_base.name,
					             expanded_rwls_report.solution_final.selected_subsets.count());
					LOGGER->info("({}) RWLS improved solution from {} subsets to {} subsets",
					             instance_base.name,
					             expanded_rwls_report.solution_initial.selected_subsets.count(),
					             expanded_rwls_report.solution_final.selected_subsets.count());
					data_rwls.emplace_back(expanded_rwls_report.serialize());
				}
				else
				{
					LOGGER->info("({}) RWLS improved solution from {} subsets to {} subsets",
					             instance_base.name,
					             rwls_report.solution_initial.selected_subsets.count(),
					             rwls_report.solution_final.selected_subsets.count());
					data_rwls.emplace_back(rwls_report.serialize());
				}
			}
			data_instance["rwls"] = std::move(data_rwls);
		}
		if(program_options.memetic)
		{
			auto process_memetic = [&](auto memetic_alg) -> bool {
				std::vector<nlohmann::json> data_memetic;
				memetic_alg.initialize();
				for(size_t repetition = 0; repetition < program_options.repetitions; ++repetition)
				{
					uscp::memetic::report memetic_report =
					  memetic_alg.solve(generator, program_options.memetic_config);
					if(reduce)
					{
						uscp::memetic::report expanded_memetic_report =
						  uscp::memetic::expand(memetic_report);
						if(!expanded_memetic_report.solution_final.cover_all_points)
						{
							LOGGER->error("Expanded memetic solution doesn't cover all points");
							return false;
						}
						LOGGER->info(
						  "({}) Expanded memetic solution to {} subsets",
						  instance_base.name,
						  expanded_memetic_report.solution_final.selected_subsets.count());
						LOGGER->info(
						  "({}) Memetic found solution with {} subsets",
						  instance_base.name,
						  expanded_memetic_report.solution_final.selected_subsets.count());
						data_memetic.emplace_back(expanded_memetic_report.serialize());
					}
					else
					{
						LOGGER->info("({}) Memetic found solution with {} subsets",
						             instance_base.name,
						             memetic_report.solution_final.selected_subsets.count());
						data_memetic.emplace_back(memetic_report.serialize());
					}
				}
				data_instance["memetic"] = std::move(data_memetic);
				return true;
			};

			bool found_crossover = false;
			bool found_wcrossover = false;
			bool success = false;
			forall_crossover_wcrossover([&](auto crossover, auto wcrossover) noexcept {
				typedef typename decltype(crossover)::type crossover_type;
				typedef typename decltype(wcrossover)::type wcrossover_type;
				if(program_options.memetic_crossover == crossover_type::to_string())
				{
					found_crossover = true;
					if(program_options.memetic_wcrossover == wcrossover_type::to_string())
					{
						found_wcrossover = true;
						uscp::memetic::memetic<crossover_type, wcrossover_type> memetic_alg_(
						  instance);
						if(process_memetic(memetic_alg_))
						{
							success = true;
						}
						return false;
					}
				}
				return true;
			});
			if(!found_crossover)
			{
				LOGGER->error("No crossover operator named \"{}\" exist",
				              program_options.memetic_crossover);
				return {};
			}
			if(!found_wcrossover)
			{
				LOGGER->error("No RWLS weights crossover operator named \"{}\" exist",
				              program_options.memetic_wcrossover);
				return {};
			}
			if(!success)
			{
				return {};
			}
		}

		return data_instance;
	}

	std::optional<std::vector<nlohmann::json>> process_registered_instances(
	  program_options& program_options,
	  uscp::random_engine& generator) noexcept
	{
		std::vector<nlohmann::json> data_instances;
		for(const std::string& instance_name: program_options.instances)
		{
			const auto instance_it =
			  std::find_if(std::cbegin(uscp::problem::instances),
			               std::cend(uscp::problem::instances),
			               [&](const uscp::problem::instance_info& instance_info) {
				               return instance_info.name == instance_name;
			               });
			if(instance_it == std::cend(uscp::problem::instances))
			{
				LOGGER->error("No known instance named {} exist", instance_name);
				return {};
			}
			LOGGER->info("Current instance information: {}", *instance_it);

			uscp::problem::instance instance_base;
			if(!uscp::problem::read(*instance_it, instance_base))
			{
				LOGGER->error("Failed to read instance {}", *instance_it);
				return {};
			}
			if(!uscp::problem::has_solution(instance_base))
			{
				LOGGER->error("Instance {} have no solution", instance_base.name);
				return {};
			}

			std::optional<nlohmann::json> data_instance =
			  process_instance(program_options, generator, instance_base, instance_it->can_reduce);
			if(!data_instance)
			{
				return {};
			}
			data_instances.push_back(std::move(*data_instance));
		}

		return data_instances;
	}

	std::optional<nlohmann::json> process_unknown_instance(program_options& program_options,
	                                                       uscp::random_engine& generator) noexcept
	{
		if(program_options.instance_name.empty())
		{
			program_options.instance_name = "instance_";
			program_options.instance_name += std::to_string(generator());
			LOGGER->warn("No instance name given, generated name: {}",
			             program_options.instance_name);
			return {};
		}
		if(program_options.instance_type.empty())
		{
			LOGGER->error("No instance type given");
			return {};
		}
		auto it = std::find_if(std::cbegin(uscp::problem::readers),
		                       std::cend(uscp::problem::readers),
		                       [&](const uscp::problem::problem_reader& problem_reader) {
			                       return problem_reader.name == program_options.instance_type;
		                       });
		if(it == std::cend(uscp::problem::readers))
		{
			LOGGER->error("Invalid instance type: {}", program_options.instance_type);
			return {};
		}
		const uscp::problem::problem_reader& problem_reader = *it;

		uscp::problem::instance instance_base;
		instance_base.name = program_options.instance_name;
		if(!problem_reader.function(program_options.instance_path, instance_base))
		{
			LOGGER->error("Failed to read {} instance {} ({})",
			              program_options.instance_type,
			              program_options.instance_name,
			              program_options.instance_path);
			return {};
		}
		if(!uscp::problem::has_solution(instance_base))
		{
			LOGGER->error("Instance {} have no solution", instance_base.name);
			return {};
		}

		return process_instance(program_options, generator, instance_base, true);
	}
} // namespace

int main(int argc, char* argv[])
{
	std::ios_base::sync_with_stdio(false);
	std::setlocale(LC_ALL, "C");

	std::ostringstream instance_types_stream;
	instance_types_stream << uscp::problem::readers[0].name;
	for(size_t i = 1; i < uscp::problem::readers.size(); ++i)
	{
		instance_types_stream << '|' << uscp::problem::readers[i].name;
	}
	const std::string valid_instance_types = instance_types_stream.str();
	const std::string default_output_prefix = "solver_out_";
	const std::string default_repetitions = "1";
	const std::string default_greedy = "false";
	const std::string default_rwls = "false";
	const std::string default_rwls_steps = std::to_string(std::numeric_limits<size_t>::max());
	const std::string default_rwls_time = std::to_string(std::numeric_limits<size_t>::max());
	const std::string default_memetic = "false";
	const std::string default_memetic_cumulative_rwls_steps =
	  std::to_string(std::numeric_limits<size_t>::max());
	const std::string default_memetic_cumulative_rwls_time =
	  std::to_string(std::numeric_limits<size_t>::max());
	const std::string default_memetic_time = std::to_string(std::numeric_limits<size_t>::max());
	const std::string default_memetic_crossover = "default";
	const std::string default_memetic_wcrossover = "default";

	program_options program_options;
	try
	{
		std::ostringstream help_txt;
		help_txt << "Unicost Set Cover Problem Solver for OR-Library and STS instances\n";
		help_txt << "Build commit: " << git_info::head_sha1;
		if(git_info::is_dirty)
		{
			help_txt << " (with uncommitted changes)";
		}
		help_txt
		  << "\n"
		     "\n"
		     "This program must be launched in the folder containing the resources, it implement 3 algorithms for solving the USCP: greedy, RWLS and Memetic\n"
		     "To specify the algorithm to use and the parameters of the algorithm, see the Usage section\n"
		     "\n"
		     "To specify known instances, use --instances=<comma separated list of instances>\n"
		     "To specify an unknown instances, use --instance_type=<orlibrary|orlibrary_rail|sts|gvcp> --instance_path=<path> --instance_name=<name>\n"
		     "\n"
		     "Known instances: 4.1,4.2,4.3,4.4,4.5,4.6,4.7,4.8,4.9,4.10,5.1,5.2,5.3,5.4,5.5,5.6,5.7,5.8,5.9,5.10,6.1,6.2,6.3,6.4,6.5,A.1,A.2,A.3,A.4,A.5,B.1,B.2,B.3,B.4,B.5,C.1,C.2,C.3,C.4,C.5,D.1,D.2,D.3,D.4,D.5,E.1,E.2,E.3,E.4,E.5,NRE.1,NRE.2,NRE.3,NRE.4,NRE.5,NRF.1,NRF.2,NRF.3,NRF.4,NRF.5,NRG.1,NRG.2,NRG.3,NRG.4,NRG.5,NRH.1,NRH.2,NRH.3,NRH.4,NRH.5,CLR10,CLR11,CLR12,CLR13,CYC6,CYC7,CYC8,CYC9,CYC10,CYC11,RAIL507,RAIL516,RAIL582,RAIL2536,RAIL2586,RAIL4284,RAIL4872,STS9,STS15,STS27,STS45,STS81,STS135,STS243,STS405,STS729,STS1215,STS2187\n"
		     "\n"
		     "Implemented crossovers: identity, merge, greedy_merge, subproblem_random, extended_subproblem_random, subproblem_greedy, extended_subproblem_greedy, subproblem_rwls, extended_subproblem_rwls\n"
		     "Implemented wcrossovers: reset, keep, average, mix_random, add, difference, max, min, minmax, shuffle\n"
		     "\n"
		     "Usage examples:\n"
		     "  Solve CYC10 and CYC11 instances with RWLS and a limit of 5000 steps:\n"
		     "    ./solver --instances=CYC10,CYC11 --rwls --rwls_steps=5000\n"
		     "\n"
		     "  Solve R42, an unknown RAIL instance in ./rail_42.txt using the same format as in OR-Library, with the Memetic algorithm, the subproblem_rwls crossover, the max wcrossover and a limit of 360 seconds:\n"
		     "    ./solver --instance_type=orlibrary_rail --instance_path=./rail_42.txt --instance_name=R42 --memetic --memetic_crossover=subproblem_rwls --memetic_wcrossover=max --memetic_time=360\n";
		cxxopts::Options options("solver", help_txt.str());
		options.add_option("", cxxopts::Option("help", "Print help"));
		options.add_option("", cxxopts::Option("version", "Print version"));
		options.add_option(
		  "",
		  cxxopts::Option("i,instances",
		                  "Instances to process",
		                  cxxopts::value<std::vector<std::string>>(program_options.instances),
		                  "NAME"));
		options.add_option(
		  "",
		  cxxopts::Option("instance_type",
		                  "Type of the instance to process",
		                  cxxopts::value<std::string>(program_options.instance_type),
		                  valid_instance_types));
		options.add_option(
		  "",
		  cxxopts::Option("instance_path",
		                  "Path of the instance to process",
		                  cxxopts::value<std::string>(program_options.instance_path),
		                  "PATH"));
		options.add_option(
		  "",
		  cxxopts::Option("instance_name",
		                  "Name of the instance to process",
		                  cxxopts::value<std::string>(program_options.instance_name),
		                  "NAME"));
		options.add_option(
		  "",
		  cxxopts::Option("o,output_prefix",
		                  "Output file prefix",
		                  cxxopts::value<std::string>(program_options.output_prefix)
		                    ->default_value(default_output_prefix),
		                  "PREFIX"));
		options.add_option(
		  "",
		  cxxopts::Option(
		    "r,repetitions",
		    "Repetitions number",
		    cxxopts::value<size_t>(program_options.repetitions)->default_value(default_repetitions),
		    "N"));

		// Greedy
		options.add_option(
		  "",
		  cxxopts::Option(
		    "greedy",
		    "Solve with greedy algorithm (no repetition as it is determinist)",
		    cxxopts::value<bool>(program_options.greedy)->default_value(default_greedy)));

		// RWLS
		options.add_option(
		  "",
		  cxxopts::Option("rwls",
		                  "Improve with RWLS algorithm (start with a greedy)",
		                  cxxopts::value<bool>(program_options.rwls)->default_value(default_rwls)));
		options.add_option("",
		                   cxxopts::Option("rwls_steps",
		                                   "RWLS steps limit",
		                                   cxxopts::value<size_t>(program_options.rwls_stop.steps)
		                                     ->default_value(default_rwls_steps),
		                                   "N"));
		options.add_option("",
		                   cxxopts::Option("rwls_time",
		                                   "RWLS time (seconds) limit",
		                                   cxxopts::value<double>(program_options.rwls_stop.time)
		                                     ->default_value(default_rwls_time),
		                                   "N"));

		// Memetic
		options.add_option(
		  "",
		  cxxopts::Option(
		    "memetic",
		    "Solve with memetic algorithm",
		    cxxopts::value<bool>(program_options.memetic)->default_value(default_memetic)));
		options.add_option(
		  "",
		  cxxopts::Option(
		    "memetic_cumulative_rwls_steps",
		    "Memetic cumulative RWLS steps limit",
		    cxxopts::value<size_t>(
		      program_options.memetic_config.stopping_criterion.rwls_cumulative_position.steps)
		      ->default_value(default_memetic_cumulative_rwls_steps),
		    "N"));
		options.add_option(
		  "",
		  cxxopts::Option(
		    "memetic_cumulative_rwls_time",
		    "Memetic cumulative RWLS time (seconds) limit",
		    cxxopts::value<double>(
		      program_options.memetic_config.stopping_criterion.rwls_cumulative_position.time)
		      ->default_value(default_memetic_cumulative_rwls_time),
		    "N"));
		options.add_option("",
		                   cxxopts::Option("memetic_time",
		                                   "Memetic time limit",
		                                   cxxopts::value<double>(
		                                     program_options.memetic_config.stopping_criterion.time)
		                                     ->default_value(default_memetic_time),
		                                   "N"));
		options.add_option(
		  "",
		  cxxopts::Option("memetic_crossover",
		                  "Memetic crossover operator",
		                  cxxopts::value<std::string>(program_options.memetic_crossover)
		                    ->default_value(default_memetic_crossover),
		                  "OPERATOR"));
		options.add_option(
		  "",
		  cxxopts::Option("memetic_wcrossover",
		                  "Memetic RWLS weights crossover operator",
		                  cxxopts::value<std::string>(program_options.memetic_wcrossover)
		                    ->default_value(default_memetic_wcrossover),
		                  "OPERATOR"));
		cxxopts::ParseResult result = options.parse(argc, argv);

		if(result.count("help"))
		{
			std::cout << options.help({"", "Group"}) << std::endl;
			return EXIT_SUCCESS;
		}

		if(result.count("version"))
		{
			std::cout << "Build commit: " << git_info::head_sha1;
			if(git_info::is_dirty)
			{
				std::cout << " (with uncommitted changes)";
			}
			std::cout << std::endl;
			return EXIT_SUCCESS;
		}

		if(program_options.instances.empty() && program_options.instance_type.empty()
		   && program_options.instance_path.empty() && program_options.instance_name.empty())
		{
			std::cout << "No instances specified, nothing to do" << std::endl;
			return EXIT_SUCCESS;
		}

		if(!program_options.greedy && !program_options.rwls && !program_options.memetic)
		{
			std::cout << "No algorithm specified, nothing to do" << std::endl;
			return EXIT_SUCCESS;
		}

		if(program_options.repetitions == 0)
		{
			std::cout << "0 repetitions, nothing to do" << std::endl;
			return EXIT_SUCCESS;
		}
	}
	catch(const std::exception& e)
	{
		std::cout << "error parsing options: " << e.what() << std::endl;
		return EXIT_FAILURE;
	}
	catch(...)
	{
		std::cerr << "unknown error parsing options" << std::endl;
		return EXIT_FAILURE;
	}

	if(!init_logger())
	{
		return EXIT_FAILURE;
	}
	LOGGER->info("START");
	{
		if(git_info::is_dirty)
		{
			LOGGER->info("Commit: {} (with uncommitted changes)", git_info::head_sha1);
		}
		else
		{
			LOGGER->info("Commit: {}", git_info::head_sha1);
		}

		// check instances
		if constexpr(CHECK_INSTANCES)
		{
			uscp::problem::check_instances();
		}

		// Prepare data
		nlohmann::json data;
		data["git"]["retrieved_state"] = git_info::retrieved_state;
		data["git"]["head_sha1"] = git_info::head_sha1;
		data["git"]["is_dirty"] = git_info::is_dirty;
		std::ostringstream now_txt;
		std::time_t t = std::time(nullptr);
		now_txt << std::put_time(std::localtime(&t), "%FT%TZ");
		data["date"] = now_txt.str();

		// Process instances: generate data
		uscp::random_engine generator(std::random_device{}());
		std::optional<std::vector<nlohmann::json>> data_registered_instances =
		  process_registered_instances(program_options, generator);
		if(!data_registered_instances)
		{
			return EXIT_FAILURE;
		}
		std::vector<nlohmann::json> data_instances = std::move(*data_registered_instances);

		if(!program_options.instance_type.empty() || !program_options.instance_path.empty()
		   || !program_options.instance_name.empty())
		{
			std::optional<nlohmann::json> data_unknown_instances =
			  process_unknown_instance(program_options, generator);
			if(!data_registered_instances)
			{
				return EXIT_FAILURE;
			}
			data_instances.push_back(std::move(*data_unknown_instances));
		}
		data["instances"] = std::move(data_instances);

		// save data
		std::ostringstream file_data_stream;
		file_data_stream << program_options.output_prefix;
		file_data_stream << std::put_time(std::localtime(&t), "%Y-%m-%d-%H-%M-%S");
		file_data_stream << "_";
		file_data_stream << generator();
		file_data_stream << ".json";
		const std::filesystem::path file_data = file_data_stream.str();
		std::error_code error;
		std::filesystem::create_directories(file_data.parent_path(), error);
		if(error)
		{
			SPDLOG_LOGGER_DEBUG(
			  LOGGER, "std::filesystem::create_directories failed: {}", error.message());
		}
		std::ofstream data_stream(file_data, std::ios::out | std::ios::trunc);
		if(!data_stream)
		{
			SPDLOG_LOGGER_DEBUG(LOGGER, "std::ofstream constructor failed");
			LOGGER->error("Failed to write file {}", file_data);
			return EXIT_FAILURE;
		}
		data_stream << data.dump(4);
	}
	LOGGER->info("END");
	return EXIT_SUCCESS;
}
