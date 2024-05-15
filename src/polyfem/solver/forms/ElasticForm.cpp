#include "ElasticForm.hpp"

#include <polyfem/quadrature/TriQuadrature.hpp>
#include <polyfem/quadrature/TetQuadrature.hpp>
#include <polyfem/assembler/AssemblerUtils.hpp>
#include <polyfem/io/Evaluator.hpp>
#include <polyfem/basis/ElementBases.hpp>
#include <polyfem/assembler/MatParams.hpp>
#include <polyfem/utils/Timer.hpp>
#include <polyfem/utils/MaybeParallelFor.hpp>
#include <polyfem/utils/getRSS.h>
#include <polyfem/assembler/ViscousDamping.hpp>

#include <paraviewo/ParaviewWriter.hpp>
#include <paraviewo/VTUWriter.hpp>
#include <paraviewo/HDF5VTUWriter.hpp>
#include <polyfem/utils/Rational.hpp>

#include <igl/writeMSH.h>
#include <igl/writeOBJ.h>

using namespace polyfem::assembler;
using namespace polyfem::utils;
using namespace polyfem::quadrature;

namespace polyfem::solver
{
	namespace
	{
		class LocalThreadVecStorage
		{
		public:
			Eigen::MatrixXd vec;
			assembler::ElementAssemblyValues vals;
			QuadratureVector da;

			LocalThreadVecStorage(const int size)
			{
				vec.resize(size, 1);
				vec.setZero();
			}
		};

		double dot(const Eigen::MatrixXd &A, const Eigen::MatrixXd &B) { return (A.array() * B.array()).sum(); }

		Eigen::MatrixXd refined_nodes(const int dim, const int i)
		{
			Eigen::MatrixXd A(dim + 1, dim);
			if (dim == 2)
			{
				A << 0., 0., 
					1., 0., 
					0., 1.;
				switch (i)
				{
				case 0:
					break;
				case 1:
					A.col(0).array() += 1;
					break;
				case 2:
					A.col(1).array() += 1;
					break;
				case 3:
					A.array() -= 1;
					A *= -1;
					break;
				default:
					throw std::runtime_error("Invalid node index");
				}
			}
			else
			{
				A << 0, 0, 0, 
					1, 0, 0, 
					0, 1, 0, 
					0, 0, 1;
				switch (i)
				{
				case 0:
					break;
				case 1:
					A.col(0).array() += 1;
					break;
				case 2:
					A.col(1).array() += 1;
					break;
				case 3:
					A.col(2).array() += 1;
					break;
				case 4:
				{
					Eigen::VectorXd tmp = 1 - A.col(1).array() - A.col(2).array();
					A.col(2) += A.col(0) + A.col(1);
					A.col(0) = tmp;
					break;
				}
				case 5:
				{
					Eigen::VectorXd tmp = 1. - A.col(1).array();
					A.col(2) += A.col(1);
					A.col(1) += A.col(0);
					A.col(0) = tmp;
					break;
				}
				case 6:
				{
					Eigen::VectorXd tmp = A.col(0) + A.col(1);
					A.col(1) = 1. - A.col(0).array();
					A.col(0) = tmp;
					break;
				}
				case 7:
				{
					Eigen::VectorXd tmp = 1. - A.col(0).array() - A.col(1).array();
					A.col(1) += A.col(2);
					A.col(2) = tmp;
					break;
				}
				default:
					throw std::runtime_error("Invalid node index");
				}
			}
			return A / 2;
		}

		/// @brief given the position of the vertices of a triangle, extract the subtriangle vertices based on the tree
		/// @param pts vertices of the triangle
		/// @param tree refinement hiararchy
		/// @return vertices of the subtriangles, and the refinement level of each subtriangle
		std::tuple<Eigen::MatrixXd, std::vector<int>> extract_subelement(const Eigen::MatrixXd &pts, const Tree &tree)
		{
			if (!tree.has_children())
				return {pts, std::vector<int>{0}};

			const int dim = pts.cols();
			Eigen::MatrixXd out;
			std::vector<int> levels;
			for (int i = 0; i < tree.n_children(); i++)
			{
				Eigen::MatrixXd uv;
				uv.setZero(dim+1, dim+1);
				uv.rightCols(dim) = refined_nodes(dim, i);
				if (dim == 2)
					uv.col(0) = 1. - uv.col(2).array() - uv.col(1).array();
				else
					uv.col(0) = 1. - uv.col(3).array() - uv.col(1).array() - uv.col(2).array();
				
				Eigen::MatrixXd pts_ = uv * pts;

				auto [tmp, L] = extract_subelement(pts_, tree.child(i));
				if (out.size() == 0)
					out = tmp;
				else
				{
					out.conservativeResize(out.rows() + tmp.rows(), Eigen::NoChange);
					out.bottomRows(tmp.rows()) = tmp;
				}
				for (int &i : L)
					++i;
				levels.insert(levels.end(), L.begin(), L.end());
			}
			return {out, levels};
		}
	
		quadrature::Quadrature refine_quadrature(const Tree &tree, const int dim, const int order)
		{
			Eigen::MatrixXd pts(dim + 1, dim);
			if (dim == 2)
				pts << 0., 0.,
					1., 0.,
					0., 1.;
			else
				pts << 0, 0, 0,
					1, 0, 0,
					0, 1, 0,
					0, 0, 1;
			auto [quad_points, levels] = extract_subelement(pts, tree);

			Quadrature tmp, quad;
			if (dim == 2)
			{
				TriQuadrature tri_quadrature;
				tri_quadrature.get_quadrature(order, tmp);
				tmp.points.conservativeResize(tmp.points.rows(), dim + 1);
				tmp.points.col(dim) = 1. - tmp.points.col(0).array() - tmp.points.col(1).array();
			}
			else
			{
				TetQuadrature tet_quadrature;
				tet_quadrature.get_quadrature(order, tmp);
				tmp.points.conservativeResize(tmp.points.rows(), dim + 1);
				tmp.points.col(dim) = 1. - tmp.points.col(0).array() - tmp.points.col(1).array() - tmp.points.col(2).array();
			}

			quad.points.resize(tmp.size() * levels.size(), dim);
			quad.weights.resize(tmp.size() * levels.size());

			for (int i = 0; i < levels.size(); i++)
			{
				quad.points.middleRows(i * tmp.size(), tmp.size()) = tmp.points * quad_points.middleRows(i * (dim + 1), dim + 1);
				quad.weights.segment(i * tmp.size(), tmp.size()) = tmp.weights / pow(2, dim * levels[i]);
			}
			assert (fabs(quad.weights.sum() - tmp.weights.sum()) < 1e-8);

			return quad;
		}

		Eigen::MatrixXd dense_uv_samples(const int dim, const int o)
		{
			assert(dim == 2);
			Eigen::MatrixXd uv((o+2)*(o+1)/2, dim);
			int id = 0;
			for (int i = 0; i <= o; i++)
			{
				for (int j = 0; i + j <= o; j++)
				{
					uv(id, 0) = i / double(o);
					uv(id, 1) = j / double(o);
					id++;
				}
			}
			return uv;
		}

		std::tuple<double, double> evaluate_jacobian(const basis::ElementBases &bs, const basis::ElementBases &gbs, const Eigen::MatrixXd &uv, const Eigen::VectorXd &disp)
		{
			assembler::ElementAssemblyValues vals;
			vals.compute(0, uv.cols() == 3, uv, bs, gbs);

			double min_disp_det = 1;
			double min_geo_det = 1;
			for (long p = 0; p < uv.rows(); ++p)
			{
				Eigen::MatrixXd disp_grad;
				disp_grad.setZero(uv.cols(), uv.cols());

				for (std::size_t j = 0; j < vals.basis_values.size(); ++j)
				{
					const auto &loc_val = vals.basis_values[j];

					for (int d = 0; d < uv.cols(); ++d)
					{
						for (std::size_t ii = 0; ii < loc_val.global.size(); ++ii)
						{
							disp_grad.row(d) += loc_val.global[ii].val * loc_val.grad.row(p) * disp(loc_val.global[ii].index * uv.cols() + d);
						}
					}
				}

				disp_grad = disp_grad * vals.jac_it[p] + Eigen::MatrixXd::Identity(uv.cols(), uv.cols());
				min_disp_det = std::min(min_disp_det, disp_grad.determinant());
				min_geo_det = std::min(disp_grad.determinant() / vals.jac_it[p].determinant(), min_geo_det);
			}
			return {min_geo_det, min_disp_det};
		}

		void update_quadrature(const int invalidID, const int dim, Tree &tree, const int quad_order, basis::ElementBases &bs, const basis::ElementBases &gbs, assembler::AssemblyValsCache &ass_vals_cache)
		{
			// update quadrature to capture the point with negative jacobian
			const Quadrature quad = refine_quadrature(tree, dim, quad_order);

			// capture the flipped point by refining the quadrature
			bs.set_quadrature([quad](Quadrature &quad_) {
				quad_ = quad;
			});
			logger().debug("New number of quadrature points: {}, level: {}", quad.size(), tree.depth());

			if (ass_vals_cache.is_initialized())
				ass_vals_cache.update(invalidID, dim == 3, bs, gbs);
		}
	} // namespace

	ElasticForm::ElasticForm(const int n_bases,
							 std::vector<basis::ElementBases> &bases,
							 const std::vector<basis::ElementBases> &geom_bases,
							 const assembler::Assembler &assembler,
							 assembler::AssemblyValsCache &ass_vals_cache,
							 const double t, const double dt,
							 const bool is_volume,
							 const double jacobian_threshold,
							 const ElementInversionCheck check_inversion,
							 const QuadratureRefinementScheme quad_scheme)
		: n_bases_(n_bases),
		  bases_(bases),
		  geom_bases_(geom_bases),
		  assembler_(assembler),
		  ass_vals_cache_(ass_vals_cache),
		  t_(t),
		  jacobian_threshold_(jacobian_threshold),
		  check_inversion_(check_inversion),
		  quad_scheme_(quad_scheme),
		  dt_(dt),
		  is_volume_(is_volume)
	{
		if (assembler_.is_linear())
			compute_cached_stiffness();
		// mat_cache_ = std::make_unique<utils::DenseMatrixCache>();
		mat_cache_ = std::make_unique<utils::SparseMatrixCache>();
		quadrature_hierarchy_.resize(bases_.size());

		quadrature_order_ = AssemblerUtils::quadrature_order(assembler_.name(), bases_[0].bases[0].order(), AssemblerUtils::BasisType::SIMPLEX_LAGRANGE, is_volume_ ? 3 : 2);

		logger().debug("Check inversion: {}, Quadrature refinement: {}", check_inversion_, quad_scheme_);
	
		if (check_inversion_ != "Discrete")
		{
			Eigen::VectorXd x0;
			x0.setZero(n_bases_ * (is_volume_ ? 3 : 2));
			if (!is_step_collision_free(x0, x0))
				log_and_throw_error("Initial state has inverted elements!");
				
			int basis_order = 0;
			int gbasis_order = 0;
			for (int e = 0; e < bases_.size(); e++)
			{
				if (basis_order == 0)
					basis_order = bases_[e].bases.front().order();
				else if (basis_order != bases_[e].bases.front().order())
					log_and_throw_error("Non-uniform basis order!!");
				if (gbasis_order == 0)
					gbasis_order = geom_bases_[e].bases.front().order();
				else if (gbasis_order != geom_bases_[e].bases.front().order())
					log_and_throw_error("Non-uniform gbasis order!!");
			}
		}
	}

	double ElasticForm::value_unweighted(const Eigen::VectorXd &x) const
	{
		return assembler_.assemble_energy(
			is_volume_,
			bases_, geom_bases_, ass_vals_cache_, t_, dt_, x, x_prev_);
	}

	Eigen::VectorXd ElasticForm::value_per_element_unweighted(const Eigen::VectorXd &x) const
	{
		const Eigen::VectorXd out = assembler_.assemble_energy_per_element(
			is_volume_, bases_, geom_bases_, ass_vals_cache_, t_, dt_, x, x_prev_);
		assert(abs(out.sum() - value_unweighted(x)) < std::max(1e-10 * out.sum(), 1e-10));
		return out;
	}

	void ElasticForm::first_derivative_unweighted(const Eigen::VectorXd &x, Eigen::VectorXd &gradv) const
	{
		Eigen::MatrixXd grad;
		assembler_.assemble_gradient(is_volume_, n_bases_, bases_, geom_bases_,
									 ass_vals_cache_, t_, dt_, x, x_prev_, grad);
		gradv = grad;
	}

	void ElasticForm::second_derivative_unweighted(const Eigen::VectorXd &x, StiffnessMatrix &hessian) const
	{
		POLYFEM_SCOPED_TIMER("elastic hessian");

		hessian.resize(x.size(), x.size());

		if (assembler_.is_linear())
		{
			assert(cached_stiffness_.rows() == x.size() && cached_stiffness_.cols() == x.size());
			hessian = cached_stiffness_;
		}
		else
		{
			// NOTE: mat_cache_ is marked as mutable so we can modify it here
			assembler_.assemble_hessian(
				is_volume_, n_bases_, project_to_psd_, bases_,
				geom_bases_, ass_vals_cache_, t_, dt_, x, x_prev_, *mat_cache_, hessian);
		}
	}

	void ElasticForm::finish()
	{
		for (auto &t : quadrature_hierarchy_)
			t = Tree();
	}

	double ElasticForm::max_step_size(const Eigen::VectorXd &x0, const Eigen::VectorXd &x1) const
	{
		// TODO: handle polygon and quad
		if (check_inversion_ != "Discrete")
		{
			const int dim = is_volume_ ? 3 : 2;
			double step, invalidStep;
			int invalidID;

			Tree subdivision_tree;
			{
				double transient_check_time = 0;
				{
					POLYFEM_SCOPED_TIMER("Transient Jacobian Check", transient_check_time);
					std::tie(step, invalidID, invalidStep, subdivision_tree) = maxTimeStep(dim, bases_, geom_bases_, x0, x1);
				}

				logger().log(step == 0 ? spdlog::level::warn : spdlog::level::debug, 
					"Jacobian max step size: {} at element {}, invalid step size: {}, tree depth {}, runtime {} sec", step, invalidID, invalidStep, subdivision_tree.depth(), transient_check_time);

				// if (step > 0) {
				// 	const Eigen::VectorXd xmid = x0 + (x1 - x0) * step;
				// 	const auto [isvalid, id, tree] = isValid(dim, bases_, geom_bases_, xmid);
				// 	if (!isvalid)
				// 	{
				// 		logger().error("Element {} is invalid!", id);

						// const int order = std::max(bases_[0].bases.front().order(), geom_bases_[0].bases.front().order());
						// const int n_basis_per_cell = std::max(bases_[0].bases.size(), geom_bases_[0].bases.size());
				// 		Eigen::MatrixXd cp = extract_nodes(dim, bases_, geom_bases_, xmid, order);
				// 		// std::cout << std::setprecision(20) << "flipped element\n" << cp.block(id * n_basis_per_cell, 0, n_basis_per_cell, dim) << std::endl;

						// {
						// 	std::string path = "transient_fail.hdf5";
						// 	const int n_elem = bases_.size();
						// 	std::vector<std::string> nodes_rational;
						// 	nodes_rational.resize(n_elem * n_basis_per_cell * 4 * dim);
						// 	Eigen::MatrixXd cp1 = extract_nodes(dim, bases_, geom_bases_, x0, order);
						// 	Eigen::MatrixXd cp2 = extract_nodes(dim, bases_, geom_bases_, x1, order);
						// 	for (int e = 0; e < n_elem; e++)
						// 	{
						// 		for (int i = 0; i < n_basis_per_cell; i++)
						// 		{
						// 			const int idx = i + n_basis_per_cell * e;
						// 			Eigen::Matrix<double, -1, 1, Eigen::ColMajor, 3, 1> pos = cp2.row(idx);

						// 			for (int d = 0; d < dim; d++)
						// 			{
						// 				utils::Rational num(pos(d));
						// 				nodes_rational[idx * (4 * dim) + d * 4 + 2] = num.get_numerator_str();
						// 				nodes_rational[idx * (4 * dim) + d * 4 + 3] = num.get_denominator_str();
						// 			}

						// 			pos = cp1.row(idx);

						// 			for (int d = 0; d < dim; d++)
						// 			{
						// 				utils::Rational num(pos(d));
						// 				nodes_rational[idx * (4 * dim) + d * 4 + 0] = num.get_numerator_str();
						// 				nodes_rational[idx * (4 * dim) + d * 4 + 1] = num.get_denominator_str();
						// 			}
						// 		}
						// 	}
						// 	paraviewo::HDF5MatrixWriter::write_matrix(path, dim, n_elem, n_basis_per_cell, nodes_rational);
						// 	logger().info("Save to {}", path);
						// }

				// 		{
				// 			std::string path = "static_fail.hdf5";
				// 			const int n_elem = bases_.size();
				// 			std::vector<std::string> nodes_rational;
				// 			nodes_rational.resize(n_elem * n_basis_per_cell * 2 * dim);
				// 			for (int e = 0; e < n_elem; e++)
				// 			{
				// 				for (int i = 0; i < n_basis_per_cell; i++)
				// 				{
				// 					const int idx = i + n_basis_per_cell * e;
				// 					Eigen::Matrix<double, -1, 1, Eigen::ColMajor, 3, 1> pos = cp.row(idx);

				// 					for (int d = 0; d < dim; d++)
				// 					{
				// 						utils::Rational num(pos(d));
				// 						nodes_rational[idx * (2 * dim) + d * 2 + 0] = num.get_numerator_str();
				// 						nodes_rational[idx * (2 * dim) + d * 2 + 1] = num.get_denominator_str();
				// 					}
				// 				}
				// 			}
				// 			paraviewo::HDF5MatrixWriter::write_matrix(path, dim, n_elem, n_basis_per_cell, nodes_rational);
				// 			logger().info("Save to {}", path);
				// 		}
				// 		std::terminate();
				// 	}
				// }
			}

			if (invalidID >= 0 && step < 0.5)
			{
				if (quadrature_hierarchy_[invalidID].merge(subdivision_tree))
					update_quadrature(invalidID, dim, quadrature_hierarchy_[invalidID], quadrature_order_, bases_[invalidID], geom_bases_[invalidID], ass_vals_cache_);

				// verify that new quadrature points don't make x0 invalid
				{
					Quadrature quad;
					bases_[invalidID].compute_quadrature(quad);
					const auto [geo_jac0, jac0] = evaluate_jacobian(bases_[invalidID], geom_bases_[invalidID], quad.points, x0);
					const auto [geo_jac1, jac1] = evaluate_jacobian(bases_[invalidID], geom_bases_[invalidID], quad.points, x0 + (x1 - x0) * step);
					const auto [geo_jac2, jac2] = evaluate_jacobian(bases_[invalidID], geom_bases_[invalidID], quad.points, x0 + (x1 - x0) * invalidStep);
					logger().debug("Min jacobian on quadrature points: {}, {}, {}", geo_jac0, geo_jac1, geo_jac2);
				}

				logger().debug("Peak memory: {} GB", getPeakRSS() / (1024. * 1024 * 1024));
			}

			return step;
		}
		return 1.;
	}

	bool ElasticForm::is_step_collision_free(const Eigen::VectorXd &x0, const Eigen::VectorXd &x1) const
	{		
		if (check_inversion_ == "Discrete")
			return true;

		const auto [isvalid, id, tree] = isValid(is_volume_ ? 3 : 2, bases_, geom_bases_, x1);
		return isvalid;

		// const auto [isvalid, id, tree] = isValid(is_volume_ ? 3 : 2, bases_, geom_bases_, x1);

		// return isvalid;
	}

	bool ElasticForm::is_step_valid(const Eigen::VectorXd &x0, const Eigen::VectorXd &x1) const
	{
		// check inversion on quadrature points
		Eigen::VectorXd grad;
		first_derivative(x1, grad);
		if (grad.array().isNaN().any())
			return false;

		return true;

		// Check the scalar field in the output does not contain NANs.
		// WARNING: Does not work because the energy is not evaluated at the same quadrature points.
		//          This causes small step lengths in the LS.
		// TVector x1_full;
		// reduced_to_full(x1, x1_full);
		// return state_.check_scalar_value(x1_full, true, false);
		// return true;
	}

	void ElasticForm::solution_changed(const Eigen::VectorXd &new_x)
	{
	}

	void ElasticForm::compute_cached_stiffness()
	{
		if (assembler_.is_linear() && cached_stiffness_.size() == 0)
		{
			assembler_.assemble(is_volume_, n_bases_, bases_, geom_bases_,
								ass_vals_cache_, t_, cached_stiffness_);
		}
	}

	void ElasticForm::force_material_derivative(const double t, const Eigen::MatrixXd &x, const Eigen::MatrixXd &x_prev, const Eigen::MatrixXd &adjoint, Eigen::VectorXd &term)
	{
		const int dim = is_volume_ ? 3 : 2;

		const int n_elements = int(bases_.size());

		if (assembler_.name() == "ViscousDamping")
		{
			term.setZero(2);

			auto storage = utils::create_thread_storage(LocalThreadVecStorage(term.size()));

			utils::maybe_parallel_for(n_elements, [&](int start, int end, int thread_id) {
				LocalThreadVecStorage &local_storage = utils::get_local_thread_storage(storage, thread_id);

				for (int e = start; e < end; ++e)
				{
					assembler::ElementAssemblyValues &vals = local_storage.vals;
					ass_vals_cache_.compute(e, is_volume_, bases_[e], geom_bases_[e], vals);

					const quadrature::Quadrature &quadrature = vals.quadrature;
					local_storage.da = vals.det.array() * quadrature.weights.array();

					Eigen::MatrixXd u, grad_u, prev_u, prev_grad_u, p, grad_p;
					io::Evaluator::interpolate_at_local_vals(e, dim, dim, vals, x, u, grad_u);
					io::Evaluator::interpolate_at_local_vals(e, dim, dim, vals, x_prev, prev_u, prev_grad_u);
					io::Evaluator::interpolate_at_local_vals(e, dim, dim, vals, adjoint, p, grad_p);

					for (int q = 0; q < local_storage.da.size(); ++q)
					{
						Eigen::MatrixXd grad_p_i, grad_u_i, prev_grad_u_i;
						vector2matrix(grad_p.row(q), grad_p_i);
						vector2matrix(grad_u.row(q), grad_u_i);
						vector2matrix(prev_grad_u.row(q), prev_grad_u_i);

						Eigen::MatrixXd f_prime_dpsi, f_prime_dphi;
						assembler::ViscousDamping::compute_dstress_dpsi_dphi(OptAssemblerData(t, dt_, e, quadrature.points.row(q), vals.val.row(q), grad_u_i), prev_grad_u_i, f_prime_dpsi, f_prime_dphi);

						// This needs to be a sum over material parameter basis.
						local_storage.vec(0) += -dot(f_prime_dpsi, grad_p_i) * local_storage.da(q);
						local_storage.vec(1) += -dot(f_prime_dphi, grad_p_i) * local_storage.da(q);
					}
				}
			});

			for (const LocalThreadVecStorage &local_storage : storage)
				term += local_storage.vec;
		}
		else
		{
			term.setZero(n_elements * 2, 1);

			auto storage = utils::create_thread_storage(LocalThreadVecStorage(term.size()));

			utils::maybe_parallel_for(n_elements, [&](int start, int end, int thread_id) {
				LocalThreadVecStorage &local_storage = utils::get_local_thread_storage(storage, thread_id);

				for (int e = start; e < end; ++e)
				{
					assembler::ElementAssemblyValues &vals = local_storage.vals;
					ass_vals_cache_.compute(e, is_volume_, bases_[e], geom_bases_[e], vals);

					const quadrature::Quadrature &quadrature = vals.quadrature;
					local_storage.da = vals.det.array() * quadrature.weights.array();

					Eigen::MatrixXd u, grad_u, p, grad_p;
					io::Evaluator::interpolate_at_local_vals(e, dim, dim, vals, x, u, grad_u);
					io::Evaluator::interpolate_at_local_vals(e, dim, dim, vals, adjoint, p, grad_p);

					for (int q = 0; q < local_storage.da.size(); ++q)
					{
						Eigen::MatrixXd grad_p_i, grad_u_i;
						vector2matrix(grad_p.row(q), grad_p_i);
						vector2matrix(grad_u.row(q), grad_u_i);

						Eigen::MatrixXd f_prime_dmu, f_prime_dlambda;
						assembler_.compute_dstress_dmu_dlambda(OptAssemblerData(t, dt_, e, quadrature.points.row(q), vals.val.row(q), grad_u_i), f_prime_dmu, f_prime_dlambda);

						// This needs to be a sum over material parameter basis.
						local_storage.vec(e + n_elements) += -dot(f_prime_dmu, grad_p_i) * local_storage.da(q);
						local_storage.vec(e) += -dot(f_prime_dlambda, grad_p_i) * local_storage.da(q);
					}
				}
			});

			for (const LocalThreadVecStorage &local_storage : storage)
				term += local_storage.vec;
		}
	}

	void ElasticForm::force_shape_derivative(const double t, const int n_verts, const Eigen::MatrixXd &x, const Eigen::MatrixXd &x_prev, const Eigen::MatrixXd &adjoint, Eigen::VectorXd &term)
	{
		const int dim = is_volume_ ? 3 : 2;
		const int actual_dim = (assembler_.name() == "Laplacian") ? 1 : dim;

		const int n_elements = int(bases_.size());
		term.setZero(n_verts * dim, 1);

		auto storage = utils::create_thread_storage(LocalThreadVecStorage(term.size()));

		if (assembler_.name() == "ViscousDamping")
		{
			utils::maybe_parallel_for(n_elements, [&](int start, int end, int thread_id) {
				LocalThreadVecStorage &local_storage = utils::get_local_thread_storage(storage, thread_id);

				for (int e = start; e < end; ++e)
				{
					assembler::ElementAssemblyValues &vals = local_storage.vals;
					ass_vals_cache_.compute(e, is_volume_, bases_[e], geom_bases_[e], vals);
					assembler::ElementAssemblyValues gvals;
					gvals.compute(e, is_volume_, vals.quadrature.points, geom_bases_[e], geom_bases_[e]);

					const quadrature::Quadrature &quadrature = vals.quadrature;
					local_storage.da = vals.det.array() * quadrature.weights.array();

					Eigen::MatrixXd u, grad_u, prev_u, prev_grad_u, p, grad_p;
					io::Evaluator::interpolate_at_local_vals(e, dim, dim, vals, x, u, grad_u);
					io::Evaluator::interpolate_at_local_vals(e, dim, dim, vals, x_prev, prev_u, prev_grad_u);
					io::Evaluator::interpolate_at_local_vals(e, dim, dim, vals, adjoint, p, grad_p);

					Eigen::MatrixXd grad_u_i, grad_p_i, prev_grad_u_i;
					Eigen::MatrixXd grad_v_i;
					Eigen::MatrixXd stress_tensor, f_prime_gradu_gradv;
					Eigen::MatrixXd f_prev_prime_prev_gradu_gradv;

					for (int q = 0; q < local_storage.da.size(); ++q)
					{
						vector2matrix(grad_u.row(q), grad_u_i);
						vector2matrix(grad_p.row(q), grad_p_i);
						vector2matrix(prev_grad_u.row(q), prev_grad_u_i);

						for (auto &v : gvals.basis_values)
						{
							Eigen::MatrixXd stress_grad, stress_prev_grad;
							assembler_.compute_stress_grad(OptAssemblerData(t, dt_, e, quadrature.points.row(q), vals.val.row(q), grad_u_i), prev_grad_u_i, stress_tensor, stress_grad);
							assembler_.compute_stress_prev_grad(OptAssemblerData(t, dt_, e, quadrature.points.row(q), vals.val.row(q), grad_u_i), prev_grad_u_i, stress_prev_grad);
							for (int d = 0; d < dim; d++)
							{
								grad_v_i.setZero(dim, dim);
								grad_v_i.row(d) = v.grad_t_m.row(q);

								f_prime_gradu_gradv.setZero(dim, dim);
								Eigen::MatrixXd tmp = grad_u_i * grad_v_i;
								for (int i = 0; i < f_prime_gradu_gradv.rows(); i++)
									for (int j = 0; j < f_prime_gradu_gradv.cols(); j++)
										for (int k = 0; k < tmp.rows(); k++)
											for (int l = 0; l < tmp.cols(); l++)
												f_prime_gradu_gradv(i, j) += stress_grad(i * dim + j, k * dim + l) * tmp(k, l);

								f_prev_prime_prev_gradu_gradv.setZero(dim, dim);
								tmp = prev_grad_u_i * grad_v_i;
								for (int i = 0; i < f_prev_prime_prev_gradu_gradv.rows(); i++)
									for (int j = 0; j < f_prev_prime_prev_gradu_gradv.cols(); j++)
										for (int k = 0; k < tmp.rows(); k++)
											for (int l = 0; l < tmp.cols(); l++)
												f_prev_prime_prev_gradu_gradv(i, j) += stress_prev_grad(i * dim + j, k * dim + l) * tmp(k, l);

								tmp = grad_v_i - grad_v_i.trace() * Eigen::MatrixXd::Identity(dim, dim);
								local_storage.vec(v.global[0].index * dim + d) -= dot(f_prime_gradu_gradv + f_prev_prime_prev_gradu_gradv + stress_tensor * tmp.transpose(), grad_p_i) * local_storage.da(q);
							}
						}
					}
				}
			});
		}
		else
		{
			utils::maybe_parallel_for(n_elements, [&](int start, int end, int thread_id) {
				LocalThreadVecStorage &local_storage = utils::get_local_thread_storage(storage, thread_id);

				for (int e = start; e < end; ++e)
				{
					assembler::ElementAssemblyValues &vals = local_storage.vals;
					ass_vals_cache_.compute(e, is_volume_, bases_[e], geom_bases_[e], vals);
					assembler::ElementAssemblyValues gvals;
					gvals.compute(e, is_volume_, vals.quadrature.points, geom_bases_[e], geom_bases_[e]);

					const quadrature::Quadrature &quadrature = vals.quadrature;
					local_storage.da = vals.det.array() * quadrature.weights.array();

					Eigen::MatrixXd u, grad_u, p, grad_p; //, stiffnesses;
					io::Evaluator::interpolate_at_local_vals(e, dim, actual_dim, vals, x, u, grad_u);
					io::Evaluator::interpolate_at_local_vals(e, dim, actual_dim, vals, adjoint, p, grad_p);
					// assembler_.compute_stiffness_value(formulation_, vals, quadrature.points, x, stiffnesses);

					for (int q = 0; q < local_storage.da.size(); ++q)
					{
						Eigen::MatrixXd grad_u_i, grad_p_i, stiffness_i;
						if (actual_dim == 1)
						{
							grad_u_i = grad_u.row(q);
							grad_p_i = grad_p.row(q);
						}
						else
						{
							vector2matrix(grad_u.row(q), grad_u_i);
							vector2matrix(grad_p.row(q), grad_p_i);
						}

						// stiffness_i = utils::unflatten(stiffnesses.row(q).transpose(), actual_dim * dim);

						for (auto &v : gvals.basis_values)
						{
							for (int d = 0; d < dim; d++)
							{
								Eigen::MatrixXd grad_v_i;
								grad_v_i.setZero(dim, dim);
								grad_v_i.row(d) = v.grad_t_m.row(q);

								Eigen::MatrixXd stress_tensor, f_prime_gradu_gradv;
								assembler_.compute_stress_grad_multiply_mat(OptAssemblerData(t, dt_, e, quadrature.points.row(q), vals.val.row(q), grad_u_i), grad_u_i * grad_v_i, stress_tensor, f_prime_gradu_gradv);
								// f_prime_gradu_gradv = utils::unflatten(stiffness_i * utils::flatten(grad_u_i * grad_v_i), dim);

								Eigen::MatrixXd tmp = grad_v_i - grad_v_i.trace() * Eigen::MatrixXd::Identity(dim, dim);
								local_storage.vec(v.global[0].index * dim + d) -= dot(f_prime_gradu_gradv + stress_tensor * tmp.transpose(), grad_p_i) * local_storage.da(q);
							}
						}
					}
				}
			});
		}

		for (const LocalThreadVecStorage &local_storage : storage)
			term += local_storage.vec;
	}

	void ElasticForm::get_refined_mesh(const Eigen::VectorXd &x, Eigen::MatrixXd &points, Eigen::MatrixXi &elements, const int elem) const
	{
		const int dim = is_volume_ ? 3 : 2;
		int n_elem = 0;
		for (int e = 0; e < bases_.size(); e++)
		{
			if (elem >= 0 && e != elem)
				continue;
			n_elem += quadrature_hierarchy_[e].n_leaves();
		}

		points.setZero(n_elem * (dim + 1), dim);
		int idx = 0;
		for (int e = 0; e < bases_.size(); e++)
		{
			if (elem >= 0 && e != elem)
				continue;
			
			const auto &tree = quadrature_hierarchy_[e];
			const auto &bs = bases_[e];

			Eigen::MatrixXd pts(dim + 1, dim);
			for (int i = 0; i < dim + 1; i++)
				pts.row(i) = bs.bases[i].global()[0].node + x.segment(bs.bases[i].global()[0].index * dim, dim).transpose();

			const auto [tmp, levels] = extract_subelement(pts, tree);
			points.middleRows(idx, tmp.rows()) = tmp;
			idx += tmp.rows();
		}

		elements.setZero(n_elem, dim + 1);
		for (int i = 0; i < elements.rows(); i++)
			for (int j = 0; j < elements.cols(); j++)
				elements(i, j) = i * (dim + 1) + j;
	}
} // namespace polyfem::solver
