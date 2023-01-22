#include "PhysicsRemesher.hpp"

#include <polyfem/solver/NLProblem.hpp>
#include <polyfem/io/VTUWriter.hpp>

namespace polyfem::mesh
{
	template <class WMTKMesh>
	std::vector<int> PhysicsRemesher<WMTKMesh>::boundary_nodes(
		const Eigen::VectorXi &vertex_to_basis) const
	{
		std::vector<int> boundary_nodes;

		// TODO: get this from state rather than building it
		std::unordered_map<int, std::array<bool, Super::DIM>> bc_ids;
		{
			assert(state.args["boundary_conditio`ns"]["dirichlet_boundary"].is_array());
			const std::vector<json> bcs = state.args["boundary_conditions"]["dirichlet_boundary"];
			for (const json &bc : bcs)
			{
				assert(bc["dimension"].size() == this->dim());
				bc_ids[bc["id"].get<int>()] = bc["dimension"];
			}
		}

		std::vector<int> boundary_ids;
		const std::vector<Tuple> boundary_facets = this->boundary_facets(&boundary_ids);
		for (int i = 0; i < boundary_facets.size(); ++i)
		{
			const Tuple &t = boundary_facets[i];
			const auto bc = bc_ids.find(boundary_ids[i]);

			if (bc == bc_ids.end())
				continue;

			for (const size_t vid : this->facet_vids(t))
				for (int d = 0; d < this->dim(); ++d)
					if (bc->second[d])
						boundary_nodes.push_back(this->dim() * vertex_to_basis[vid] + d);
		}

		// Sort and remove the duplicate boundary_nodes.
		std::sort(boundary_nodes.begin(), boundary_nodes.end());
		auto new_end = std::unique(boundary_nodes.begin(), boundary_nodes.end());
		boundary_nodes.erase(new_end, boundary_nodes.end());

		return boundary_nodes;
	}

	template <class WMTKMesh>
	std::vector<typename PhysicsRemesher<WMTKMesh>::Tuple>
	PhysicsRemesher<WMTKMesh>::local_mesh_tuples(const VectorNd &center) const
	{
		const double rel_area = args["local_relaxation"]["local_mesh_rel_area"];
		const double n_ring = args["local_relaxation"]["local_mesh_n_ring"];
		return LocalMesh<PhysicsRemesher<WMTKMesh>>::ball_selection(
			*this, center, rel_area * this->total_volume, n_ring);
	}

	template <class WMTKMesh>
	double PhysicsRemesher<WMTKMesh>::local_mesh_energy(const VectorNd &center) const
	{
		using namespace polyfem::solver;
		using namespace polyfem::basis;

		const std::vector<Tuple> local_mesh_tuples = this->local_mesh_tuples(center);

		const bool include_global_boundary =
			state.is_contact_enabled() && std::any_of(local_mesh_tuples.begin(), local_mesh_tuples.end(), [&](const Tuple &t) {
				const size_t tid = this->element_id(t);
				for (int i = 0; i < Super::FACETS_PER_ELEMENT; ++i)
					if (this->is_boundary_facet(this->tuple_from_facet(tid, i)))
						return true;
				return false;
			});

		LocalMesh local_mesh(*this, local_mesh_tuples, include_global_boundary);

		const std::vector<ElementBases> bases = local_mesh.build_bases(state.formulation());
		const std::vector<int> boundary_nodes = local_boundary_nodes(local_mesh);
		assembler::AssemblerUtils &assembler = this->init_assembler(local_mesh.body_ids());
		SolveData solve_data;
		assembler::AssemblyValsCache ass_vals_cache;
		Eigen::SparseMatrix<double> mass;
		ipc::CollisionMesh collision_mesh;

		local_solve_data(
			local_mesh, bases, boundary_nodes, assembler, include_global_boundary,
			solve_data, ass_vals_cache, mass, collision_mesh);

		const Eigen::MatrixXd sol = utils::flatten(local_mesh.displacements());

		return solve_data.nl_problem->value(sol);
	}

	template <class WMTKMesh>
	typename PhysicsRemesher<WMTKMesh>::Operations
	PhysicsRemesher<WMTKMesh>::renew_neighbor_tuples(
		const std::string &op,
		const std::vector<Tuple> &elements) const
	{
		// POLYFEM_REMESHER_SCOPED_TIMER("Renew neighbor tuples");
		assert(elements.size() == 1);
		assert(op != "vertex_smooth");

		VectorNd center;
		if constexpr (std::is_same_v<wmtk::TriMesh, WMTKMesh>)
		{
			if (op == "edge_split")
			{
				center = vertex_attrs[elements[0].switch_vertex(*this).vid(*this)].rest_position;
			}
			else if (op == "edge_swap")
			{
				center = (vertex_attrs[elements[0].vid(*this)].rest_position
						  + vertex_attrs[elements[0].switch_vertex(*this).vid(*this)].rest_position)
						 / 2.0;
			}
			else // if (op == "edge_collapse" || op == "vertex_smooth")
			{
				center = vertex_attrs[elements[0].vid(*this)].rest_position;
			}
		}
		else
		{
			assert(op == "edge_split" || op == "edge_collapse");
			center = vertex_attrs[elements[0].vid(*this)].rest_position;
		}

		// return all edges affected by local relaxation
		std::vector<Tuple> local_mesh_tuples = this->local_mesh_tuples(center);
		this->extend_local_patch(local_mesh_tuples);

		std::vector<Tuple> edges;
		for (const Tuple &t : local_mesh_tuples)
		{
			const size_t t_id = this->element_id(t);

			for (auto j = 0; j < Super::EDGES_PER_ELEMENT; j++)
			{
				const Tuple e = WMTKMesh::tuple_from_edge(t_id, j);
				const size_t e_id = e.eid(*this);

				if (op == "edge_split" && this->edge_attr(e_id).energy_rank != Super::EdgeAttributes::EnergyRank::TOP)
					continue;
				else if (op == "edge_collapse" && this->edge_attr(e_id).energy_rank != Super::EdgeAttributes::EnergyRank::BOTTOM)
					continue;

				edges.push_back(e);
			}
		}
		wmtk::unique_edge_tuples(*this, edges);

		Operations new_ops;
		for (auto &e : edges)
			new_ops.emplace_back(op, e);

		return new_ops;
	}

	template <class WMTKMesh>
	double PhysicsRemesher<WMTKMesh>::edge_elastic_energy(const Tuple &e) const
	{
		using namespace polyfem::solver;
		using namespace polyfem::basis;

		const std::vector<Tuple> elements = this->get_incident_elements_for_edge(e);

		double volume = 0;
		for (const auto &t : elements)
			volume += this->element_volume(t);
		assert(volume > 0);

		LocalMesh local_mesh(*this, elements, /*include_global_boundary=*/false);

		const std::vector<ElementBases> bases = local_mesh.build_bases(state.formulation());
		const std::vector<int> boundary_nodes; // no boundary nodes
		assembler::AssemblerUtils &assembler = this->init_assembler(local_mesh.body_ids());
		SolveData solve_data;
		assembler::AssemblyValsCache ass_vals_cache;
		Eigen::SparseMatrix<double> mass;
		ipc::CollisionMesh collision_mesh;

		// TODO: account for contact energy
		local_solve_data(
			local_mesh, bases, boundary_nodes, assembler, false,
			solve_data, ass_vals_cache, mass, collision_mesh);

		const Eigen::MatrixXd sol = utils::flatten(local_mesh.displacements());

		return solve_data.nl_problem->value(sol) / volume; // average energy
	}

	template <class WMTKMesh>
	void PhysicsRemesher<WMTKMesh>::write_priority_queue_mesh(const std::string &path, const Tuple &e) const
	{
		constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
		constexpr double tol = 1e-14; // tolerance allowed in recomputed values

		// Save the edge energy and its position in the priority queue
		std::unordered_map<size_t, std::tuple<double, double, int>> edge_to_fields;

		// The current tuple was popped from the queue, so we need to recompute its energy
		const double current_edge_energy = edge_elastic_energy(e);
		edge_to_fields[e.eid(*this)] = std::make_tuple(current_edge_energy, 0, 0);

		// NOTE: this is not thread-safe
		auto queue = executor.serial_queue();

		// Also check that the energy is consistent with the priority queue values
		bool energies_match = true;

		for (int i = 1; !queue.empty(); ++i)
		{
			std::tuple<double, std::string, Tuple, size_t> tmp;
			bool pop_success = queue.try_pop(tmp);
			assert(pop_success);
			const auto &[energy, op, t, _] = tmp;

			// Some tuple in the queue might not be valid anymore
			if (!t.is_valid(*this))
			{
				--i; // don't count this tuple
				continue;
			}

			// assert(t.eid(*this) != e.eid(*this)); // this should have been popped

			// Check that the energy is consistent with the priority queue values
			const double recomputed_energy = edge_elastic_energy(t);
			const double diff = energy - recomputed_energy;
			if (abs(diff) >= tol)
			{
				logger().error(
					"Energy mismatch: {} vs {}; diff={:g}",
					energy, recomputed_energy, diff);
				energies_match = false;
			}

			// Check that the current edge has the highes priority
			assert(current_edge_energy - energy >= -tol); // account for numerical error

			// Save the edge energy and its position in the priority queue
			edge_to_fields[t.eid(*this)] = std::make_tuple(energy, abs(diff), i);
		}
		assert(energies_match);

		const std::vector<Tuple> edges = WMTKMesh::get_edges();

		// Create two vertices per edge to get per edge values.
		const int n_vertices = 2 * edges.size();

		std::vector<std::vector<int>> elements(edges.size(), std::vector<int>(2));
		Eigen::MatrixXd rest_positions(n_vertices, this->dim());
		Eigen::MatrixXd displacements(n_vertices, this->dim());
		Eigen::VectorXd edge_energies(n_vertices);
		Eigen::VectorXd edge_energy_diffs(n_vertices);
		Eigen::VectorXd edge_orders(n_vertices);

		for (int ei = 0; ei < edges.size(); ei++)
		{
			const std::array<size_t, 2> vids = {{
				edges[ei].vid(*this),
				edges[ei].switch_vertex(*this).vid(*this),
			}};

			double edge_energy, edge_energy_diff, edge_order;
			const auto &itr = edge_to_fields.find(edges[ei].eid(*this));
			if (itr != edge_to_fields.end())
				std::tie(edge_energy, edge_energy_diff, edge_order) = itr->second;
			else
				edge_energy = edge_energy_diff = edge_order = NaN;

			for (int vi = 0; vi < vids.size(); ++vi)
			{
				elements[ei][vi] = 2 * ei + vi;
				rest_positions.row(elements[ei][vi]) = vertex_attrs[vids[vi]].rest_position;
				displacements.row(elements[ei][vi]) = vertex_attrs[vids[vi]].displacement();
				edge_energies(elements[ei][vi]) = edge_energy;
				edge_energy_diffs(elements[ei][vi]) = edge_energy_diff;
				edge_orders(elements[ei][vi]) = edge_order;
			}
		}

		io::VTUWriter writer;
		writer.add_field("displacement", displacements);
		writer.add_field("edge_energy", edge_energies);
		writer.add_field("edge_energy_diff", edge_energy_diffs);
		writer.add_field("operation_order", edge_orders);
		writer.write_mesh(path, rest_positions, elements, /*is_simplicial=*/true);
	}

	// -------------------------------------------------------------------------
	// Template specializations

	template class PhysicsRemesher<wmtk::TriMesh>;
	template class PhysicsRemesher<wmtk::TetMesh>;

} // namespace polyfem::mesh
