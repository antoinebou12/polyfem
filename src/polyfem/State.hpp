#pragma once

#include <polyfem/Common.hpp>

#include <polyfem/basis/ElementBases.hpp>
#include <polyfem/basis/InterfaceData.hpp>

#include <polyfem/assembler/ElementAssemblyValues.hpp>
#include <polyfem/assembler/AssemblyValsCache.hpp>
#include <polyfem/assembler/RhsAssembler.hpp>
#include <polyfem/assembler/Problem.hpp>
#include <polyfem/assembler/AssemblerUtils.hpp>

#include <polyfem/mesh/Mesh.hpp>
#include <polyfem/mesh/Obstacle.hpp>
#include <polyfem/mesh/MeshNodes.hpp>
#include <polyfem/mesh/LocalBoundary.hpp>

#include <polyfem/utils/StringUtils.hpp>
#include <polyfem/utils/ElasticityUtils.hpp>
#include <polyfem/utils/Logger.hpp>
#include <polyfem/utils/IntegrableFunctional.hpp>

#include <polyfem/io/OutData.hpp>

#include <polysolve/LinearSolver.hpp>

#include <Eigen/Dense>
#include <Eigen/Sparse>

#ifdef POLYFEM_WITH_TBB
#include <tbb/global_control.h>
#endif

#include <memory>
#include <string>

#include <ipc/collision_mesh.hpp>
#include <ipc/utils/logger.hpp>
#include <ipc/friction/friction_constraint.hpp>

// Forward declaration
namespace cppoptlib
{
	template <typename ProblemType>
	class NonlinearSolver;
}

namespace polyfem
{
	namespace mesh
	{
		class Mesh2D;
		class Mesh3D;
	} // namespace mesh

	namespace solver
	{
		class NLProblem;

		class ContactForm;
		class FrictionForm;
		class BodyForm;
		class ALForm;
		class InertiaForm;
		class ElasticForm;
	} // namespace solver

	namespace time_integrator
	{
		class ImplicitTimeIntegrator;
	} // namespace time_integrator

	/// class to store time stepping data
	class SolveData
	{
	public:
		std::shared_ptr<assembler::RhsAssembler> rhs_assembler;
		std::shared_ptr<solver::NLProblem> nl_problem;

		std::shared_ptr<solver::ContactForm> contact_form;
		std::shared_ptr<solver::BodyForm> body_form;
		std::shared_ptr<solver::ALForm> al_form;
		std::shared_ptr<solver::ElasticForm> damping_form;
		std::shared_ptr<solver::FrictionForm> friction_form;
		std::shared_ptr<solver::InertiaForm> inertia_form;
		std::shared_ptr<solver::ElasticForm> elastic_form;

		std::shared_ptr<time_integrator::ImplicitTimeIntegrator> time_integrator;

		/// @brief update the barrier stiffness for the forms
		/// @param x current solution
		void updated_barrier_stiffness(const Eigen::VectorXd &x);

		/// @brief updates the dt inside the different forms
		void update_dt();
	};

	/// main class that contains the polyfem solver and all its state
	class State
	{
	public:
		//---------------------------------------------------
		//-----------------initializtion---------------------
		//---------------------------------------------------

		~State() = default;
		/// Constructor
		/// @param[in] max_threads max number of threads
		State(const unsigned int max_threads = std::numeric_limits<unsigned int>::max(), const bool skip_thread_initialization = false);

		/// initialize the polyfem solver with a json settings
		/// @param[in] args input arguments
		/// @param[in] strict_validation strict validation of input
		/// @param[in] output_dir output directory
		void init(const json &args, const bool strict_validation, const std::string &output_dir = "", const bool fallback_solver = false);

		/// initialize time settings if args contains "time"
		void init_time();

		/// main input arguments containing all defaults
		json args;
		json in_args;

		//---------------------------------------------------
		//-----------------logger----------------------------
		//---------------------------------------------------

		/// initalizing the logger
		/// @param[in] log_file is to write it to a file (use log_file="") to output to stdout
		/// @param[in] log_level 0 all message, 6 no message. 2 is info, 1 is debug
		/// @param[in] is_quit quiets the log
		void init_logger(const std::string &log_file, const spdlog::level::level_enum log_level, const bool is_quiet);

		/// initalizing the logger writes to an output stream
		/// @param[in] os output stream
		/// @param[in] log_level 0 all message, 6 no message. 2 is info, 1 is debug
		void init_logger(std::ostream &os, const spdlog::level::level_enum log_level);

		/// change log level
		/// @param[in] log_level 0 all message, 6 no message. 2 is info, 1 is debug
		void set_log_level(const spdlog::level::level_enum log_level)
		{
			spdlog::set_level(log_level);
			logger().set_level(log_level);
			ipc::logger().set_level(log_level);
			current_log_level = log_level;
		}

		int current_log_level;

		/// gets the output log as json
		/// this is *not* what gets printed but more informative
		/// information, eg it contains runtimes, errors, etc.
		std::string get_log(const Eigen::MatrixXd &sol)
		{
			std::stringstream ss;
			save_json(sol, ss);
			return ss.str();
		}

	private:
		/// initalizing the logger meant for internal usage
		void init_logger(const std::vector<spdlog::sink_ptr> &sinks, const spdlog::level::level_enum log_level);

	public:
		//---------------------------------------------------
		//-----------------assembly--------------------------
		//---------------------------------------------------

		/// assembler, it dispatches call to the different assembers based on the formulation
		assembler::AssemblerUtils assembler;
		/// current problem, it contains rhs and bc
		std::shared_ptr<assembler::Problem> problem;

		/// FE bases, the size is #elements
		std::vector<basis::ElementBases> bases;
		/// FE pressure bases for mixed elements, the size is #elements
		std::vector<basis::ElementBases> pressure_bases;
		/// Geometric mapping bases, if the elements are isoparametric, this list is empty
		std::vector<basis::ElementBases> geom_bases_;

		/// number of bases
		int n_bases;
		/// number of pressure bases
		int n_pressure_bases;
		/// number of geometric bases
		int n_geom_bases;

		// Mapping from input nodes to FE nodes
		std::vector<int> primitive_to_bases_node, primitive_to_geom_bases_node, primitive_to_pressure_bases_node;

		/// polygons, used since poly have no geom mapping
		std::map<int, Eigen::MatrixXd> polys;
		/// polyhedra, used since poly have no geom mapping
		std::map<int, std::pair<Eigen::MatrixXd, Eigen::MatrixXi>> polys_3d;

		/// vector of discretization oders, used when not all elements have the same degree, one per element
		Eigen::VectorXi disc_orders;

		/// Mapping from input nodes to FE nodes
		std::shared_ptr<polyfem::mesh::MeshNodes> mesh_nodes, geom_mesh_nodes, pressure_mesh_nodes;

		// list of dirichlet boundary geometry nodes
		std::vector<int> boundary_gnodes;
		std::vector<bool> boundary_gnodes_mask;

		/// used to store assembly values for small problems
		assembler::AssemblyValsCache ass_vals_cache;
		assembler::AssemblyValsCache mass_ass_vals_cache;
		/// used to store assembly values for pressure for small problems
		assembler::AssemblyValsCache pressure_ass_vals_cache;

		/// Stiffness matrix, it is not compute for nonlinear problems
		StiffnessMatrix stiffness;

		/// Mass matrix, it is computed only for time dependent problems
		StiffnessMatrix mass;
		/// average system mass, used for contact with IPC
		double avg_mass;

		/// System righ-hand side.
		Eigen::MatrixXd rhs;

		/// In Elasticity PDE, solve for "min W(disp_offset + u)" instead of "min W(u)"
		Eigen::MatrixXd disp_offset;

		Eigen::MatrixXd pre_sol;

		/// use average pressure for stokes problem to fix the additional dofs, true by default
		/// if false, it will fix one pressure node to zero
		bool use_avg_pressure;

		/// return the formulation (checks if the problem is scalar or not and delas with multiphisics)
		/// @return fomulation
		std::string formulation() const;

		/// check if using iso parametric bases
		/// @return if basis are isoparametric
		bool iso_parametric() const;

		/// @brief Get a constant reference to the geometry mapping bases.
		/// @return A constant reference to the geometry mapping bases.
		const std::vector<basis::ElementBases> &geom_bases() const
		{
			return iso_parametric() ? bases : geom_bases_;
		}

		std::vector<basis::ElementBases> &geom_bases()
		{
			return iso_parametric() ? bases : geom_bases_;
		}

		/// builds the bases step 2 of solve
		void build_basis();
		/// compute rhs, step 3 of solve
		void assemble_rhs();
		/// assemble matrices, step 4 of solve
		void assemble_stiffness_mat(bool assemble_mass = false);

		/// build a RhsAssembler for the problem
		std::shared_ptr<assembler::RhsAssembler> build_rhs_assembler(
			const int n_bases,
			const std::vector<basis::ElementBases> &bases,
			const assembler::AssemblyValsCache &ass_vals_cache) const;
		/// build a RhsAssembler for the problem
		std::shared_ptr<assembler::RhsAssembler> build_rhs_assembler() const
		{
			return build_rhs_assembler(n_bases, bases, mass_ass_vals_cache);
		}

		/// quadrature used for projecting boundary conditions
		/// @return the quadrature used for projecting boundary conditions
		int n_boundary_samples() const
		{
			using assembler::AssemblerUtils;
			const int n_b_samples_j = args["space"]["advanced"]["n_boundary_samples"];
			const int gdiscr_order = mesh->orders().size() <= 0 ? 1 : mesh->orders().maxCoeff();
			const int discr_order = std::max(disc_orders.maxCoeff(), gdiscr_order);

			const int n_b_samples = std::max(n_b_samples_j, AssemblerUtils::quadrature_order("Mass", discr_order, AssemblerUtils::BasisType::POLY, mesh->dimension()));

			return n_b_samples;
		}

		/// set the multimaterial, this is mean for internal usage.
		void set_materials();

	private:
		/// splits the solution in solution and pressure for mixed problems
		void sol_to_pressure(Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure);
		/// builds bases for polygons, called inside build_basis
		void build_polygonal_basis();

		//---------------------------------------------------
		//-----------------solver----------------------------
		//---------------------------------------------------

	public:
		/// solves the problems
		/// @param[out] sol solution
		/// @param[out] pressure pressure
		void solve_problem(Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure);
		void solve_homogenization(Eigen::MatrixXd &sol);
		/// solves the problem, call other methods
		/// @param[out] sol solution
		/// @param[out] pressure pressure
		void solve(Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure)
		{
			if (!mesh)
			{
				logger().error("Load the mesh first!");
				return;
			}
			stats.compute_mesh_stats(*mesh);

			build_basis();

			assemble_rhs();
			assemble_stiffness_mat();

			solve_export_to_file = false;
			solution_frames.clear();
			solve_problem(sol, pressure);
			solve_export_to_file = true;
		}

		/// timedependent stuff cached
		SolveData solve_data;
		/// initialize solver
		/// @param[out] sol solution
		/// @param[out] pressure pressure
		void init_solve(Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure);
		/// solves transient navier stokes with operator splitting
		/// @param[in] time_steps number of time steps
		/// @param[in] dt timestep size
		/// @param[out] sol solution
		/// @param[out] pressure pressure
		void solve_transient_navier_stokes_split(const int time_steps, const double dt, Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure);
		/// solves transient navier stokes with FEM
		/// @param[in] time_steps number of time steps
		/// @param[in] t0 initial times
		/// @param[in] dt timestep size
		/// @param[out] sol solution
		/// @param[out] pressure pressure
		void solve_transient_navier_stokes(const int time_steps, const double t0, const double dt, Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure);
		/// solves transient linear problem
		/// @param[in] time_steps number of time steps
		/// @param[in] t0 initial times
		/// @param[in] dt timestep size
		/// @param[out] sol solution
		/// @param[out] pressure pressure
		void solve_transient_linear(const int time_steps, const double t0, const double dt, Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure);
		/// solves transient tensor nonlinear problem
		/// @param[in] time_steps number of time steps
		/// @param[in] t0 initial times
		/// @param[in] dt timestep size
		/// @param[out] sol solution
		void solve_transient_tensor_nonlinear(const int time_steps, const double t0, const double dt, Eigen::MatrixXd &sol);
		/// initialize the nonlinear solver
		/// @param[out] sol solution
		/// @param[in] t (optional) initial time
		void init_nonlinear_tensor_solve(Eigen::MatrixXd &sol, const double t = 1.0);
		/// initialize the linear solve
		/// @param[in] t (optional) initial time
		void init_linear_solve(Eigen::MatrixXd &sol, const double t = 1.0);
		/// solves a linear problem
		/// @param[out] sol solution
		/// @param[out] pressure pressure
		void solve_linear(Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure);
		/// solves a navier stokes
		/// @param[out] sol solution
		/// @param[out] pressure pressure
		void solve_navier_stokes(Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure);
		/// solves nonlinear problems
		/// @param[out] sol solution
		/// @param[in] t (optional) time step id
		void solve_tensor_nonlinear(Eigen::MatrixXd &sol, const int t = 0);

		/// factory to create the nl solver depdending on input
		/// @return nonlinear solver (eg newton or LBFGS)
		template <typename ProblemType>
		std::shared_ptr<cppoptlib::NonlinearSolver<ProblemType>> make_nl_solver() const;

		// under periodic BC, the index map from a restricted node to the node it depends on, -1 otherwise
		Eigen::VectorXi periodic_reduce_map;
		int n_periodic_dependent_dofs;
		std::vector<bool> periodic_dimensions;
		bool has_periodic_bc() const
		{
			for (const bool &r : periodic_dimensions)
			{
				if (r)
					return true;
			}
			return false;
		}

		// add lagrangian multiplier rows for pure neumann/periodic boundary condition, returns the number of rows added
		int n_lagrange_multipliers() const;
		void apply_lagrange_multipliers(StiffnessMatrix &A) const;
		void apply_lagrange_multipliers(StiffnessMatrix &A, const Eigen::MatrixXd &coeffs) const;
		
		// compute the matrix/vector under periodic basis, if the size is larger than #periodic_basis, the extra rows are kept
		int full_to_periodic(StiffnessMatrix &A) const;
		int full_to_periodic(Eigen::MatrixXd &b, bool accumulate, bool force_dirichlet = true) const;
		void full_to_periodic(std::vector<int> &boundary_nodes_) const
		{
			if (has_periodic_bc() && !args["space"]["advanced"]["periodic_basis"])
			{
				for (int i = 0; i < boundary_nodes_.size(); i++)
					boundary_nodes_[i] = periodic_reduce_map(boundary_nodes_[i]);

				std::sort(boundary_nodes_.begin(), boundary_nodes_.end());
				auto it = std::unique(boundary_nodes_.begin(), boundary_nodes_.end());
				boundary_nodes_.resize(std::distance(boundary_nodes_.begin(), it));
			}
		}

		Eigen::MatrixXd periodic_to_full(const int ndofs, const Eigen::MatrixXd &x_periodic) const;

	private:
		/// @brief Load or compute the initial solution.
		/// @param[out] solution Output solution variable.
		void initial_solution(Eigen::MatrixXd &solution) const;
		/// @brief Load or compute the initial velocity.
		/// @param[out] solution Output velocity variable.
		void initial_velocity(Eigen::MatrixXd &velocity) const;
		/// @brief Load or compute the initial acceleration.
		/// @param[out] solution Output acceleration variable.
		void initial_acceleration(Eigen::MatrixXd &acceleration) const;

		/// @brief Solve the linear problem with the given solver and system.
		/// @param solver Linear solver.
		/// @param A Linear system matrix.
		/// @param b Right-hand side.
		/// @param compute_spectrum If true, compute the spectrum.
		/// @param[out] sol solution
		/// @param[out] pressure pressure
		void solve_linear(
			const std::unique_ptr<polysolve::LinearSolver> &solver,
			StiffnessMatrix &A,
			Eigen::VectorXd &b,
			const bool compute_spectrum,
			Eigen::MatrixXd &sol, Eigen::MatrixXd &pressure);

		//---------------------------------------------------
		//-----------------nodes flags-----------------------
		//---------------------------------------------------

	public:
		/// list of boundary nodes
		std::vector<int> boundary_nodes;
		/// list of neumann boundary nodes
		std::vector<int> pressure_boundary_nodes;
		/// mapping from elements to nodes for all mesh
		std::vector<mesh::LocalBoundary> total_local_boundary;
		/// mapping from elements to nodes for dirichlet boundary conditions
		std::vector<mesh::LocalBoundary> local_boundary;
		/// mapping from elements to nodes for neumann boundary conditions
		std::vector<mesh::LocalBoundary> local_neumann_boundary;
		/// nodes on the boundary of polygonal elements, used for harmonic bases
		std::map<int, basis::InterfaceData> poly_edge_to_data;
		/// Matrices containing the input per node dirichelt
		std::vector<Eigen::MatrixXd> input_dirichlet;
		/// per node dirichelt
		std::vector<int> dirichlet_nodes;
		std::vector<RowVectorNd> dirichlet_nodes_position;
		/// per node neumann
		std::vector<int> neumann_nodes;
		std::vector<RowVectorNd> neumann_nodes_position;

		/// Inpute nodes (including high-order) to polyfem nodes, only for isoparametric
		Eigen::VectorXi in_node_to_node;
		/// maps in vertices/edges/faces/cells to polyfem vertices/edges/faces/cells
		Eigen::VectorXi in_primitive_to_primitive;

	private:
		/// build the mapping from input nodes to polyfem nodes
		void build_node_mapping();

		//---------------------------------------------------
		//-----------------Geometry--------------------------
		//---------------------------------------------------
	public:
		/// current mesh, it can be a Mesh2D or Mesh3D
		std::unique_ptr<mesh::Mesh> mesh;
		/// Obstacles used in collisions
		mesh::Obstacle obstacle;

		/// loads the mesh from the json arguments
		/// @param[in] non_conforming creates a conforming/non conforming mesh
		/// @param[in] names keys in the hdf5
		/// @param[in] cells list of cells from hdf5
		/// @param[in] vertices list of vertices from hdf5
		void load_mesh(bool non_conforming = false,
					   const std::vector<std::string> &names = std::vector<std::string>(),
					   const std::vector<Eigen::MatrixXi> &cells = std::vector<Eigen::MatrixXi>(),
					   const std::vector<Eigen::MatrixXd> &vertices = std::vector<Eigen::MatrixXd>());

		/// loads the mesh from a geogram mesh
		/// @param[in] meshin geo mesh
		/// @param[in] boundary_marker the input of the lambda is the face barycenter, the output is the sideset id
		/// @param[in] non_conforming creates a conforming/non conforming mesh
		/// @param[in] skip_boundary_sideset skip_boundary_sideset = false it uses the lambda boundary_marker to assign the sideset
		void load_mesh(GEO::Mesh &meshin, const std::function<int(const RowVectorNd &)> &boundary_marker, bool non_conforming = false, bool skip_boundary_sideset = false);

		/// loads the mesh from V and F,
		/// @param[in] V is #vertices x dim
		/// @param[in] F is #elements x size (size = 3 for triangle mesh, size=4 for a quaud mesh if dim is 2)
		/// @param[in] non_conforming creates a conforming/non conforming mesh
		void load_mesh(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F, bool non_conforming = false)
		{
			mesh = mesh::Mesh::create(V, F, non_conforming);
			load_mesh(non_conforming);
		}

		/// set the boundary sideset from a lambda that takes the face/edge barycenter
		/// @param[in] boundary_marker function from face/edge barycenter that returns the sideset id
		void set_boundary_side_set(const std::function<int(const RowVectorNd &)> &boundary_marker)
		{
			mesh->compute_boundary_ids(boundary_marker);
		}

		/// set the boundary sideset from a lambda that takes the face/edge barycenter and a flag if the face/edge is boundary or not (used to set internal boundaries)
		/// @param[in] boundary_marker function from face/edge barycenter and a flag if the face/edge is boundary that returns the sideset id
		void set_boundary_side_set(const std::function<int(const RowVectorNd &, bool)> &boundary_marker)
		{
			mesh->compute_boundary_ids(boundary_marker);
		}

		/// set the boundary sideset from a lambda that takes the face/edge vertices and a flag if the face/edge is boundary or not (used to set internal boundaries)
		/// @param[in] boundary_marker function from face/edge vertices and a flag if the face/edge is boundary that returns the sideset id
		void set_boundary_side_set(const std::function<int(const std::vector<int> &, bool)> &boundary_marker)
		{
			mesh->compute_boundary_ids(boundary_marker);
		}

		/// Resets the mesh
		void reset_mesh();

		//---------------------------------------------------
		//-----------------IPC-------------------------------
		//---------------------------------------------------

		// boundary mesh used for collision
		/// @brief Boundary_nodes_pos contains the total number of nodes, the internal ones are zero.
		/// For high-order fem the faces are triangulated this is currently supported only for tri and tet meshes.
		Eigen::MatrixXd boundary_nodes_pos;
		/// @brief IPC collision mesh
		ipc::CollisionMesh collision_mesh;

		/// extracts the boundary mesh for collision, called in build_basis
		void build_collision_mesh(
			Eigen::MatrixXd &boundary_nodes_pos_,
			ipc::CollisionMesh &collision_mesh_, 
			const int n_bases_, 
			const std::vector<basis::ElementBases> &bases_) const;

		/// checks if vertex is obstacle
		/// @param[in] vi vertex index
		/// @return if vertex is obstalce
		bool is_obstacle_vertex(const size_t vi) const
		{
			return vi >= boundary_nodes_pos.rows() - obstacle.n_vertices();
		}

		/// @brief does the simulation has contact
		///
		/// @return true/false
		bool is_contact_enabled() const { return args["contact"]["enabled"]; }

		/// stores if input json contains dhat
		bool has_dhat = false;

		//---------------------------------------------------
		//-----------------OUTPUT----------------------------
		//---------------------------------------------------
	public:
		/// Directory for output files
		std::string output_dir;

		/// flag to decide if exporting the time dependent solution to files
		/// or save it in the solution_frames array
		bool solve_export_to_file = true;
		/// saves the frames in a vector instead of VTU
		std::vector<io::SolutionFrame> solution_frames;
		/// visualization stuff
		io::OutGeometryData out_geom;
		/// runtime statistics
		io::OutRuntimeData timings;
		/// Other statistics
		io::OutStatsData stats;

		/// saves all data on the disk according to the input params
		/// @param[in] sol solution
		/// @param[in] pressure pressure
		void export_data(const Eigen::MatrixXd &sol, const Eigen::MatrixXd &pressure);

		/// saves a timestep
		/// @param[in] time time in secs
		/// @param[in] t time index
		/// @param[in] t0 initial time
		/// @param[in] dt delta t
		/// @param[in] sol solution
		/// @param[in] pressure pressure
		void save_timestep(const double time, const int t, const double t0, const double dt, const Eigen::MatrixXd &sol, const Eigen::MatrixXd &pressure);

		/// saves a subsolve when save_solve_sequence_debug is true
		/// @param[in] i sub solve index
		/// @param[in] t time index
		/// @param[in] sol solution
		/// @param[in] pressure pressure
		void save_subsolve(const int i, const int t, const Eigen::MatrixXd &sol, const Eigen::MatrixXd &pressure);

		/// saves a timestep
		/// @param[in] time time in secs
		/// @param[in] t time index
		/// @param[in] t0 initial time
		/// @param[in] dt delta t
		void save_timestep(const double time, const int t, const double t0, const double dt);

		/// saves a subsolve when save_solve_sequence_debug is true
		/// @param[in] i sub solve index
		/// @param[in] t time index
		void save_subsolve(const int i, const int t);

		/// saves the output statistic to a stream
		/// @param[in] sol solution
		/// @param[out] out stream to write output
		void save_json(const Eigen::MatrixXd &sol, std::ostream &out);

		/// saves the output statistic to disc accoding to params
		/// @param[in] sol solution
		void save_json(const Eigen::MatrixXd &sol);

		/// @brief computes all errors
		void compute_errors(const Eigen::MatrixXd &sol);

		//-----------PATH management
		/// Get the root path for the state (e.g., args["root_path"] or ".")
		/// @return root path
		std::string root_path() const;

		/// Resolve input path relative to root_path() if the path is not absolute.
		/// @param[in] path path to resolve
		/// @param[in] only_if_exists resolve only if relative path exists
		/// @return path
		std::string resolve_input_path(const std::string &path, const bool only_if_exists = false) const;

		/// Resolve output path relative to output_dir if the path is not absolute
		/// @param[in] path path to resolve
		/// @return resolvedpath
		std::string resolve_output_path(const std::string &path) const;

#ifdef POLYFEM_WITH_TBB
		/// limits the number of used threads
		std::shared_ptr<tbb::global_control> thread_limiter;
#endif

		//---------------------------------------------------
		//-----------------differentiable--------------------
		//---------------------------------------------------
public:
		void cache_transient_adjoint_quantities(const int current_step, const Eigen::MatrixXd &sol);
		struct DiffCachedParts
		{
			StiffnessMatrix gradu_h;
			StiffnessMatrix gradu_h_next;
			Eigen::MatrixXd u;
			ipc::Constraints contact_set;
			ipc::FrictionConstraints friction_constraint_set;
			Eigen::MatrixXd p;
			Eigen::MatrixXd nu;
		};
		std::vector<DiffCachedParts> diff_cached;
		
		std::unique_ptr<polysolve::LinearSolver> lin_solver_cached; // matrix factorization of last linear solve
		Eigen::MatrixXd initial_velocity_cache; // initial velocity of last solve

		int n_linear_solves = 0;
		int n_nonlinear_solves = 0;

		bool adjoint_solved() const
		{
			return adjoint_solved_;
		}

		int ndof() const
		{
			const int actual_dim = problem->is_scalar() ? 1 : mesh->dimension();
			if (!assembler.is_mixed(formulation()))
				return actual_dim * n_bases;
			else
				return actual_dim * n_bases + n_pressure_bases;
		}
		
		int get_bdf_order() const
		{
			if (args["time"]["integrator"]["type"] == "ImplicitEuler")
				return 1;
			else if (args["time"]["integrator"]["type"] == "BDF")
				return args["time"]["integrator"]["steps"].get<int>();
			else
			{
				log_and_throw_error("Integrator type not supported for differentiability.");
				return -1;
			}
		}
		// Aux functions for setting up adjoint equations
		void compute_force_hessian(const Eigen::MatrixXd &sol, StiffnessMatrix &hessian, StiffnessMatrix &hessian_prev) const;
		// Solves the adjoint PDE for derivatives
		void solve_adjoint(const Eigen::MatrixXd &rhs);
		void solve_static_adjoint(const Eigen::VectorXd &adjoint_rhs);
		void solve_transient_adjoint(const Eigen::MatrixXd &adjoint_rhs);
		// Change geometric node positions
		void set_mesh_vertices(const Eigen::MatrixXd &vertices);
		void get_vf(Eigen::MatrixXd &vertices, Eigen::MatrixXi &faces, const bool geometric = true) const;

		// to replace the initial condition
		Eigen::MatrixXd initial_sol_update, initial_vel_update;
		// downsample grad on P2 nodes to grad on P1 nodes, only for P2 contact shape derivative
		StiffnessMatrix down_sampling_mat;
private:
		bool adjoint_solved_ = false;
		//---------------------------------------------------
		//-----------------homogenization--------------------
		//---------------------------------------------------
public:
		void solve_homogenized_field(const Eigen::MatrixXd &disp_grad, const Eigen::MatrixXd &target, Eigen::MatrixXd &sol_);
		void solve_homogenized_field_incremental(const Eigen::MatrixXd &macro_field2, Eigen::MatrixXd &macro_field1, Eigen::MatrixXd &sol_);
	};

} // namespace polyfem
