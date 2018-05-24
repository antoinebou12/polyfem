#include "ElasticProblem.hpp"
#include "State.hpp"

#include <iostream>

namespace poly_fem
{
	ElasticProblem::ElasticProblem(const std::string &name)
	: Problem(name)
	{
		boundary_ids_ = {1, 3, 5, 6};
	}

	void ElasticProblem::rhs(const std::string &formulation, const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const
	{
		val = Eigen::MatrixXd::Zero(pts.rows(), pts.cols());
	}

	void ElasticProblem::bc(const Mesh &mesh, const Eigen::MatrixXi &global_ids, const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const
	{
		val = Eigen::MatrixXd::Zero(pts.rows(), mesh.dimension());

		for(long i = 0; i < pts.rows(); ++i)
		{
			if(mesh.get_boundary_id(global_ids(i))== 1)
				val(i, 0)=-0.25;
			else if(mesh.get_boundary_id(global_ids(i))== 3)
				val(i, 0)=0.25;
			if(mesh.get_boundary_id(global_ids(i))== 5)
				val(i, 1)=-0.25;
			else if(mesh.get_boundary_id(global_ids(i))== 6)
				val(i, 1)=0.25;
		}
	}


	ElasticForceProblem::ElasticForceProblem(const std::string &name)
	: Problem(name)
	{
		boundary_ids_ = {2};
		neumann_boundary_ids_ = {4};

		force_.resize(3);
		force_.setZero();
		force_(0) = 0.1;
	}

	void ElasticForceProblem::rhs(const std::string &formulation, const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const
	{
		val = Eigen::MatrixXd::Zero(pts.rows(), pts.cols());
	}

	void ElasticForceProblem::bc(const Mesh &mesh, const Eigen::MatrixXi &global_ids, const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const
	{
		val = Eigen::MatrixXd::Zero(pts.rows(), mesh.dimension());

		for(long i = 0; i < pts.rows(); ++i)
		{
			if(mesh.get_boundary_id(global_ids(i))== 2)
				val.row(i).setZero();
		}
	}

	void ElasticForceProblem::neumann_bc(const Mesh &mesh, const Eigen::MatrixXi &global_ids, const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const
	{
		val = Eigen::MatrixXd::Zero(pts.rows(), mesh.dimension());

		for(long i = 0; i < pts.rows(); ++i)
		{
			if(mesh.get_boundary_id(global_ids(i)) == 4){
				for(int d = 0; d < val.cols(); ++d)
					val(i, d) = force_(d);
			}
		}
	}

	void ElasticForceProblem::set_parameters(const json &params)
	{
		if(params.find("boundary_ids") != params.end())
		{
			boundary_ids_.clear();
			auto j_boundary_ids = params["boundary_ids"];

			boundary_ids_.resize(j_boundary_ids.size());


			for(size_t i = 0; i < boundary_ids_.size(); ++i)
			{
				boundary_ids_[i] = j_boundary_ids[i];
			}
		}

		if(params.find("neumann_boundary_ids") != params.end())
		{
			neumann_boundary_ids_.clear();
			auto neumann_j_boundary_ids = params["neumann_boundary_ids"];

			neumann_boundary_ids_.resize(neumann_j_boundary_ids.size());


			for(size_t i = 0; i < neumann_boundary_ids_.size(); ++i)
			{
				neumann_boundary_ids_[i] = neumann_j_boundary_ids[i];
			}
		}

		if(params.find("force") != params.end())
		{
			auto ff = params["force"];
			if(ff.is_array())
			{
				for(int k = 0; k < ff.size(); ++k)
					force_(k) = ff[k];
			}
		}
	}


	ElasticProblemZeroBC::ElasticProblemZeroBC(const std::string &name)
	: Problem(name)
	{
		boundary_ids_ = {1, 2, 3, 4, 5, 6};
	}

	void ElasticProblemZeroBC::rhs(const std::string &formulation, const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const
	{
		val = Eigen::MatrixXd::Zero(pts.rows(), pts.cols());
		val.col(1).setConstant(0.5);
	}

	void ElasticProblemZeroBC::bc(const Mesh &mesh, const Eigen::MatrixXi &global_ids, const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const
	{
		val = Eigen::MatrixXd::Zero(pts.rows(), mesh.dimension());

		for(long i = 0; i < pts.rows(); ++i)
		{
			if(mesh.get_boundary_id(global_ids(i)) > 0)
				val.row(i).setZero();
		}
	}



	namespace
	{
		template<typename T>
		Eigen::Matrix<T, 2, 1> function(T x, T y)
		{
			Eigen::Matrix<T, 2, 1> res;

			res(0) = (y*y*y + x*x + x*y)/50.;
			res(1) = (3*x*x*x*x + x*y*y + x)/50.;

			return res;
		}

		template<typename T>
		Eigen::Matrix<T, 3, 1> function(T x, T y, T z)
		{
			Eigen::Matrix<T, 3, 1> res;

			res(0) = (x*y + x*x + y*y*y + 6*z)/80.;
			res(1) = (z*x - z*z*z + x*y*y + 3*x*x*x*x)/80.;
			res(2) = (x*y*z + z*z*y*y - 2*x)/80.;

			return res;
		}


		template<typename T>
		Eigen::Matrix<T, 2, 1> function_compression(T x, T y)
		{
			Eigen::Matrix<T, 2, 1> res;

			res(0) = -(y*y*y + x*x + x*y)/20.;
			res(1) = -(3*x*x*x*x + x*y*y + x)/20.;

			return res;
		}

		template<typename T>
		Eigen::Matrix<T, 3, 1> function_compression(T x, T y, T z)
		{
			Eigen::Matrix<T, 3, 1> res;

			res(0) = -(x*y + x*x + y*y*y + 6*z)/14.;
			res(1) = -(z*x - z*z*z + x*y*y + 3*x*x*x*x)/14.;
			res(2) = -(x*y*z + z*z*y*y - 2*x)/14.;

			return res;
		}


		template<typename T>
		Eigen::Matrix<T, 2, 1> function_quadratic(T x, T y)
		{
			Eigen::Matrix<T, 2, 1> res;

			res(0) = -(y*y + x*x + x*y)/50.;
			res(1) = -(3*x*x + y)/50.;

			return res;
		}

		template<typename T>
		Eigen::Matrix<T, 3, 1> function_quadratic(T x, T y, T z)
		{
			Eigen::Matrix<T, 3, 1> res;

			res(0) = -(y*y + x*x + x*y + z*y)/50.;
			res(1) = -(3*x*x + y + z*z)/50.;
			res(2) = -(x*z + y*y - 2*z)/50.;

			return res;
		}


		template<typename T>
		Eigen::Matrix<T, 2, 1> function_linear(T x, T y)
		{
			Eigen::Matrix<T, 2, 1> res;

			res(0) = -(y + x)/50.;
			res(1) = -(3*x + y)/50.;

			return res;
		}

		template<typename T>
		Eigen::Matrix<T, 3, 1> function_linear(T x, T y, T z)
		{
			Eigen::Matrix<T, 3, 1> res;

			res(0) = -(y + x + z)/50.;
			res(1) = -(3*x + y - z)/50.;
			res(2) = -(x + y - 2*z)/50.;

			return res;
		}
	}



	ElasticProblemExact::ElasticProblemExact(const std::string &name)
	: ProblemWithSolution(name)
	{ }

	VectorNd ElasticProblemExact::eval_fun(const VectorNd &pt) const
	{
		if(pt.size() == 2)
			return function(pt(0), pt(1));
		else if(pt.size() == 3)
			return function(pt(0), pt(1), pt(2));

		assert(false);
		return VectorNd(pt.size());
	}

	AutodiffGradPt ElasticProblemExact::eval_fun(const AutodiffGradPt &pt) const
	{
		if(pt.size() == 2)
			return function(pt(0), pt(1));
		else if(pt.size() == 3)
			return function(pt(0), pt(1), pt(2));

		assert(false);
		return AutodiffGradPt(pt.size());
	}

	AutodiffHessianPt ElasticProblemExact::eval_fun(const AutodiffHessianPt &pt) const
	{
		if(pt.size() == 2)
			return function(pt(0), pt(1));
		else if(pt.size() == 3)
			return function(pt(0), pt(1), pt(2));

		assert(false);
		return AutodiffHessianPt(pt.size());
	}




	CompressionElasticProblemExact::CompressionElasticProblemExact(const std::string &name)
	: ProblemWithSolution(name)
	{ }

	VectorNd CompressionElasticProblemExact::eval_fun(const VectorNd &pt) const
	{
		if(pt.size() == 2)
			return function_compression(pt(0), pt(1));
		else if(pt.size() == 3)
			return function_compression(pt(0), pt(1), pt(2));

		assert(false);
		return VectorNd(pt.size());
	}

	AutodiffGradPt CompressionElasticProblemExact::eval_fun(const AutodiffGradPt &pt) const
	{
		if(pt.size() == 2)
			return function_compression(pt(0), pt(1));
		else if(pt.size() == 3)
			return function_compression(pt(0), pt(1), pt(2));

		assert(false);
		return AutodiffGradPt(pt.size());
	}

	AutodiffHessianPt CompressionElasticProblemExact::eval_fun(const AutodiffHessianPt &pt) const
	{
		if(pt.size() == 2)
			return function_compression(pt(0), pt(1));
		else if(pt.size() == 3)
			return function_compression(pt(0), pt(1), pt(2));

		assert(false);
		return AutodiffHessianPt(pt.size());
	}






	QuadraticElasticProblemExact::QuadraticElasticProblemExact(const std::string &name)
	: ProblemWithSolution(name)
	{ }

	VectorNd QuadraticElasticProblemExact::eval_fun(const VectorNd &pt) const
	{
		if(pt.size() == 2)
			return function_quadratic(pt(0), pt(1));
		else if(pt.size() == 3)
			return function_quadratic(pt(0), pt(1), pt(2));

		assert(false);
		return VectorNd(pt.size());
	}

	AutodiffGradPt QuadraticElasticProblemExact::eval_fun(const AutodiffGradPt &pt) const
	{
		if(pt.size() == 2)
			return function_quadratic(pt(0), pt(1));
		else if(pt.size() == 3)
			return function_quadratic(pt(0), pt(1), pt(2));

		assert(false);
		return AutodiffGradPt(pt.size());
	}

	AutodiffHessianPt QuadraticElasticProblemExact::eval_fun(const AutodiffHessianPt &pt) const
	{
		if(pt.size() == 2)
			return function_quadratic(pt(0), pt(1));
		else if(pt.size() == 3)
			return function_quadratic(pt(0), pt(1), pt(2));

		assert(false);
		return AutodiffHessianPt(pt.size());
	}






	LinearElasticProblemExact::LinearElasticProblemExact(const std::string &name)
	: ProblemWithSolution(name)
	{ }

	VectorNd LinearElasticProblemExact::eval_fun(const VectorNd &pt) const
	{
		if(pt.size() == 2)
			return function_linear(pt(0), pt(1));
		else if(pt.size() == 3)
			return function_linear(pt(0), pt(1), pt(2));

		assert(false);
		return VectorNd(pt.size());
	}

	AutodiffGradPt LinearElasticProblemExact::eval_fun(const AutodiffGradPt &pt) const
	{
		if(pt.size() == 2)
			return function_linear(pt(0), pt(1));
		else if(pt.size() == 3)
			return function_linear(pt(0), pt(1), pt(2));

		assert(false);
		return AutodiffGradPt(pt.size());
	}

	AutodiffHessianPt LinearElasticProblemExact::eval_fun(const AutodiffHessianPt &pt) const
	{
		if(pt.size() == 2)
			return function_linear(pt(0), pt(1));
		else if(pt.size() == 3)
			return function_linear(pt(0), pt(1), pt(2));

		assert(false);
		return AutodiffHessianPt(pt.size());
	}

}
