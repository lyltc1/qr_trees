// 
// 
// Implements iLQR (on a traditional chain) for nonlinear dynamics and cost.
//
// Arun Venkatraman (arunvenk@cs.cmu.edu)
// December 2016
//

#pragma once

#include <templated/iLQR.hh>

#include <ostream>

namespace ilqr
{

// Computes the control at timestep t at xt.
// :param alpha - Backtracking line search parameter. Setting to 1 gives regular forward pass.
template<int xdim, int udim>
inline Vector<udim> iLQRSolver<xdim,udim>::compute_control_stepsize(const Vector<xdim> &xt, 
        const int t, const double alpha) const
{
    const Matrix<udim, xdim> &Kt = Ks_[t];
    const Vector<udim> &kt = ks_[t];

    const Vector<xdim> zt = (xt - xhat_[t]);
    const Vector<udim> vt = Kt * zt + alpha*kt;

    return Vector<udim>(vt + uhat_[t]);
}

template<int xdim, int udim>
inline double iLQRSolver<xdim,udim>::forward_pass(const Vector<xdim> x_init,  
            std::vector<Vector<xdim>> &states,
            std::vector<Vector<udim>> &controls,
            const double alpha
        ) const
{
    const int T = timesteps();

    controls.resize(T);
    states.resize(T+1);

    states[0] = x_init;
    double cost_to_go = 0;
    for (int t = 0; t < T; ++t)
    {
        controls[t] = compute_control_stepsize(states[t], t, alpha); 

        const double cost = cost_(states[t], controls[t], t);
        cost_to_go += cost;

        // Roll forward the dynamics.
        const Vector<xdim> xt1 = dynamics_(states[t], controls[t]);
        states[t+1] = xt1;
    }
    const double final_cost = final_cost_(states[T]); 
    cost_to_go += final_cost;

    return cost_to_go;
}

template<int xdim, int udim>
inline void iLQRSolver<xdim,udim>::solve(const int T, 
        const Vector<xdim> &x_init, const Vector<udim> u_nominal, 
        const double mu, const int max_iters, bool verbose, 
        const double cost_convg_ratio, const double start_alpha,
        const bool warm_start, const int t_offset
        )
{
    IS_GREATER(T, 0);
    IS_GREATER_EQUAL(mu, 0);
    IS_GREATER(max_iters, 0);
    IS_GREATER(cost_convg_ratio, 0);
    IS_GREATER(start_alpha, 0);

    if (!warm_start)
    {
        Ks_ = std::vector<Matrix<udim, xdim>>(T, Matrix<udim, xdim>::Zero());
        ks_ = std::vector<Vector<udim>>(T, Vector<udim>::Zero());

        uhat_ = std::vector<Vector<udim>>(T, u_nominal);
        xhat_ = std::vector<Vector<xdim>>(T+1, Vector<xdim>::Zero());
    }
    else // if we warm start
    {
        IS_GREATER(static_cast<int>(Ks_.size()), t_offset);

        Ks_.erase(Ks_.begin(), Ks_.begin()+t_offset);
        ks_.erase(ks_.begin(), ks_.begin()+t_offset);
        uhat_.erase(uhat_.begin(), uhat_.begin()+t_offset);
        xhat_.erase(xhat_.begin(), xhat_.begin()+t_offset);

        // Confirm that the time horizon matches the size of requried
        // variables. 
        IS_EQUAL(static_cast<int>(Ks_.size()), T);
        IS_EQUAL(static_cast<int>(ks_.size()), T);
        IS_EQUAL(static_cast<int>(uhat_.size()), T);
        IS_EQUAL(static_cast<int>(xhat_.size()), T+1);
    }

    std::vector<Vector<udim>> uhat_new (T, Vector<udim>::Zero());
    std::vector<Vector<xdim>> xhat_new(T+1, Vector<xdim>::Zero());

    double old_cost = std::numeric_limits<double>::infinity();
    int iter = 0;
    for (iter = 0; iter < max_iters; ++iter)
    {
        // Line search as decribed in 
        // http://homes.cs.washington.edu/~todorov/papers/TassaIROS12.pdf
        // https://studywolf.wordpress.com/2016/02/03/the-iterative-linear-quadratic-regulator-method/
        
        // Initial step-size
        double alpha = start_alpha;

        // The step-size adaptation paramter
        constexpr double beta = 0.5; 
        static_assert(beta > 0.0 && beta < 1.0, 
            "Step-size adaptation parameter should decrease the step-size");
        
        // Before we start line search, NaN makes sure termination conditions 
        // won't be met.
        double new_cost = std::numeric_limits<double>::quiet_NaN();
        double cost_diff_ratio = std::abs((old_cost - new_cost) / new_cost);

        while(!(new_cost < old_cost || cost_diff_ratio < cost_convg_ratio))
        {
            new_cost = forward_pass(x_init, xhat_new, uhat_new, alpha);

            cost_diff_ratio = std::abs((old_cost - new_cost) / new_cost);

            // Try decreasing the step-size by beta-times. 
            alpha *= beta;
        } 

        xhat_.swap(xhat_new);
        uhat_.swap(uhat_new);

        if (verbose)
        {
            // Since we always half it at the end of the iteration, double it
            alpha = (1.0/beta)*alpha;

            PRINT("[Iter " << iter << "]: Alpha: " << alpha 
                    << ", Cost ratio: " << cost_diff_ratio 
                    << ", New Cost: " << new_cost
                    << ", Old Cost: " << old_cost);
        }

        if (cost_diff_ratio < cost_convg_ratio) 
        {
            break;
        }

        old_cost = new_cost;

        Matrix<xdim,xdim> QT; Vector<xdim> gT;
        quadratize_cost(this->final_cost_, xhat_[T], QT, gT);

        Matrix<xdim, xdim> Vt1 = QT;
        Matrix<1, xdim> Gt1 = gT.transpose();

        // Backwards pass from t=T-1 all the way through the first at t=0.
        // Storage for backing up the value function.
        Matrix<xdim, xdim> Vt;
        Matrix<1, xdim> Gt;
        for (int t = T-1; t >= 0; --t)
        {
            bellman_backup(t, mu, Vt1, Gt1, Vt, Gt, Ks_[t], ks_[t]);
            Vt1 = Vt;
            Gt1 = Gt;
        }
    }

    if (verbose)
    {
        SUCCESS("Converged after " << iter << " iterations.");
    }
}
 

template<int xdim, int udim>
inline void iLQRSolver<xdim,udim>::bellman_backup(
        const int t,
        const double mu, // Levenberg-Marquardt parameter
        const Matrix<xdim,xdim> &Vt1, 
        const Matrix<1,xdim> &Gt1, 
        Matrix<xdim,xdim> &Vt, 
        Matrix<1,xdim> &Gt, 
        Matrix<udim,xdim> &Kt, 
        Vector<udim> &kt
        )
{
    const Vector<xdim> &x = xhat_[t];
    const Vector<udim> &u = uhat_[t];

    Matrix<xdim, xdim> A; 
    Matrix<xdim, udim> B;
    linearize_dynamics(this->dynamics_, x, u, A, B);

    Matrix<xdim,xdim> Q;
    Matrix<udim,udim> R;
    Matrix<xdim,udim> P;
    Vector<xdim> g_x;
    Vector<udim> g_u;
    quadratize_cost(this->cost_, t, x, u, Q, R, P, g_x, g_u);

    // Levenberg-Marquardt parameter for damping. 
    // ie. eigenvalue inflation matrix.
    const Matrix<xdim, xdim> LM = mu * Matrix<xdim, xdim>::Identity();

    const Matrix<udim, udim> inv_term 
        = -1.0*(R + B.transpose()*(Vt1+LM)*B).inverse();

    Kt = inv_term * (P.transpose() + B.transpose()*(Vt1+LM)*A); 
    kt = inv_term * (g_u + B.transpose()*Gt1.transpose());

    const Matrix<xdim, xdim> tmp = (A + B*Kt);
    Vt = Q + 2.0*(P*Kt) 
        + Kt.transpose()*R*Kt + tmp.transpose()*Vt1*tmp;

    Gt = kt.transpose()*P.transpose() 
        + kt.transpose()*R*Kt + g_x.transpose() 
        + g_u.transpose()*Kt + kt.transpose()*B.transpose()*Vt1*tmp + Gt1*tmp;
}

template<int xdim, int udim>
inline int iLQRSolver<xdim,udim>::timesteps() const
{ 
    const size_t T = uhat_.size(); 
    // Confirm that all the required parts the same size
    IS_EQUAL(T, ks_.size());
    IS_EQUAL(T, Ks_.size());
    IS_EQUAL(T+1, xhat_.size());
    return static_cast<int>(T); 
}


} // namespace ilqr
