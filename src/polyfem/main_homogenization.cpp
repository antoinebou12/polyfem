#include <filesystem>

#include <CLI/CLI.hpp>

#include <highfive/H5File.hpp>
#include <highfive/H5Easy.hpp>

#include <polyfem/State.hpp>
#include <polyfem/utils/JSONUtils.hpp>
#include <polyfem/utils/Logger.hpp>

#include <polysolve/LinearSolver.hpp>

bool has_arg(const CLI::App &command_line, const std::string &value)
{
	const auto *opt = command_line.get_option_no_throw(value.size() == 1 ? ("-" + value) : ("--" + value));
	if (!opt)
		return false;

	return opt->count() > 0;
}

int main(int argc, char **argv)
{
	using namespace polyfem;

	CLI::App command_line{"polyfem"};

	// Eigen::setNbThreads(1);
	size_t max_threads = std::numeric_limits<size_t>::max();
	command_line.add_option("--max_threads", max_threads, "Maximum number of threads");

	std::string json_file = "";
	command_line.add_option("-j,--json", json_file, "Simulation JSON file")->check(CLI::ExistingFile);

	std::string hdf5_file = "";
	command_line.add_option("--hdf5", hdf5_file, "Simulation hdf5 file")->check(CLI::ExistingFile);

	std::string output_dir = "";
	command_line.add_option("-o,--output_dir", output_dir, "Directory for output files")->check(CLI::ExistingDirectory | CLI::NonexistentPath);

	bool is_quiet = false;
	command_line.add_flag("--quiet", is_quiet, "Disable cout for logging");

	bool is_strict = true;
	command_line.add_flag("-s,--strict_validation,!--ns,!--no_strict_validation", is_strict, "Disables strict validation of input JSON");

	bool fallback_solver = false;
	command_line.add_flag("--enable_overwrite_solver", fallback_solver, "If solver in json is not present, falls back to default");

	std::string log_file = "";
	command_line.add_option("--log_file", log_file, "Log to a file");

	// const std::vector<std::string> solvers = polysolve::LinearSolver::availableSolvers();
	// std::string solver;
	// command_line.add_option("--solver", solver, "Used to print the list of linear solvers available")->check(CLI::IsMember(solvers));

	const std::vector<std::pair<std::string, spdlog::level::level_enum>>
		SPDLOG_LEVEL_NAMES_TO_LEVELS = {
			{"trace", spdlog::level::trace},
			{"debug", spdlog::level::debug},
			{"info", spdlog::level::info},
			{"warning", spdlog::level::warn},
			{"error", spdlog::level::err},
			{"critical", spdlog::level::critical},
			{"off", spdlog::level::off}};
	spdlog::level::level_enum log_level = spdlog::level::debug;
	command_line.add_option("--log_level", log_level, "Log level")
		->transform(CLI::CheckedTransformer(SPDLOG_LEVEL_NAMES_TO_LEVELS, CLI::ignore_case));

	CLI11_PARSE(command_line, argc, argv);

	std::vector<std::string> names;
	std::vector<Eigen::MatrixXi> cells;
	std::vector<Eigen::MatrixXd> vertices;

	json in_args = json({});

	if (!json_file.empty())
	{
		std::ifstream file(json_file);

		if (file.is_open())
			file >> in_args;
		else
			log_and_throw_error(fmt::format("unable to open {} file", json_file));
		file.close();

		if (!in_args.contains("root_path"))
		{
			in_args["root_path"] = json_file;
		}
	}
	else
	{
		logger().error("No input file specified!");
		return command_line.exit(CLI::RequiredError("--json or --hdf5"));
	}

	if (!output_dir.empty())
	{
		std::filesystem::create_directories(output_dir);
	}

	State state(max_threads);
	state.init_logger(log_file, log_level, is_quiet);
	state.init(in_args, is_strict, output_dir, fallback_solver);
	state.load_mesh(/*non_conforming=*/false, names, cells, vertices);

	// Mesh was not loaded successfully; load_mesh() logged the error.
	if (state.mesh == nullptr)
		// Cannot proceed without a mesh.
		return EXIT_FAILURE;

	state.stats.compute_mesh_stats(*state.mesh);

	state.build_basis();

    Eigen::MatrixXd def_grad(state.mesh->dimension(), state.mesh->dimension());
    // assert(state.args["materials"]["def_grad"].is_array());
    // int i = 0, j = 0;
    // for (const auto &r : state.args["materials"]["def_grad"])
    // {
    //     assert(r.is_array());
	// 	j = 0;
	// 	for (const auto &c : r)
    //     {
	// 		def_grad(i, j) = c;
	// 		j++;
	// 	}
    //     i++;
    // }

	const int N = 50;
	for (int n = 0; n < N; n++)
	{
		def_grad << 0, 0,
		            0, -n/(2.0*N);
    	state.solve_homogenized_field(def_grad, Eigen::MatrixXd(), state.sol);
		state.out_geom.export_data(
			state,
			!state.args["time"].is_null(),
			0, 0,
			io::OutGeometryData::ExportOptions(state.args, state.mesh->is_linear(), state.problem->is_scalar(), state.solve_export_to_file),
			"step_" + std::to_string(n) + ".vtu",
			"", // nodes_path,
			"", // solution_path,
			"", // stress_path,
			"", // mises_path,
			state.is_contact_enabled(), state.solution_frames);
	}

	logger().info("total time: {}s", state.timings.total_time());

	// state.export_data();

	return EXIT_SUCCESS;
}