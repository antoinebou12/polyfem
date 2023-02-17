#pragma once

#include <polyfem/utils/Logger.hpp>

#include <Eigen/Core>

namespace polyfem::solver
{
	/** This parameterize a function f : x -> y
	 * and provides the chain rule with respect to previous gradients
	 */
	class Parametrization
	{
	public:
		virtual ~Parametrization() {}

		virtual Eigen::VectorXd inverse_eval(const Eigen::VectorXd &y) const
		{
			log_and_throw_error("Not supported");
			return Eigen::VectorXd();
		}

		virtual int size(const int x_size) const = 0; // just for verification
		virtual Eigen::VectorXd eval(const Eigen::VectorXd &x) const = 0;
		virtual Eigen::VectorXd apply_jacobian(const Eigen::VectorXd &grad_full, const Eigen::VectorXd &x) const = 0;

		virtual Eigen::VectorXi get_state_variable_indexing() const { return Eigen::VectorXi(); }
	};

	class CompositeParametrization : public Parametrization
	{
	public:
		CompositeParametrization() {}
		CompositeParametrization(const std::vector<std::shared_ptr<Parametrization>> &parametrizations) : parametrizations_(parametrizations) {}
		virtual ~CompositeParametrization() {}

		int size(const int x_size) const override
		{
			int cur_size = x_size;
			for (const auto &p : parametrizations_)
				cur_size = p->size(cur_size);

			return cur_size;
		}

		Eigen::VectorXd inverse_eval(const Eigen::VectorXd &y) const override
		{
			if (parametrizations_.empty())
				return y;

			Eigen::VectorXd x = y;
			for (int i = parametrizations_.size() - 1; i >= 0; i--)
			{
				x = parametrizations_[i]->inverse_eval(x);
			}

			return x;
		}

		Eigen::VectorXd eval(const Eigen::VectorXd &x) const override
		{
			if (parametrizations_.empty())
				return x;

			Eigen::VectorXd y = x;
			for (const auto &p : parametrizations_)
			{
				y = p->eval(y);
			}

			return y;
		}
		Eigen::VectorXd apply_jacobian(const Eigen::VectorXd &grad_full, const Eigen::VectorXd &x) const override
		{
			if (parametrizations_.empty())
				return grad_full;

			std::vector<Eigen::VectorXd> ys;
			auto y = x;
			for (const auto &p : parametrizations_)
			{
				ys.emplace_back(y);
				y = p->eval(y);
			}

			Eigen::VectorXd gradv = grad_full;
			for (int i = parametrizations_.size() - 1; i >= 0; --i)
			{
				gradv = parametrizations_[i]->apply_jacobian(gradv, ys[i]);
			}

			return gradv;
		}

		Eigen::VectorXi get_state_variable_indexing() const override
		{
			if (parametrizations_.size() == 0)
				return Eigen::VectorXi();
			else
				return parametrizations_.back()->get_state_variable_indexing();
		}

	private:
		std::vector<std::shared_ptr<Parametrization>> parametrizations_;
	};
} // namespace polyfem::solver
