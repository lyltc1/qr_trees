#include <lqr/lqr_tree.hh>

#include <utils/debug_utils.hh>

#include <iterator>
#include <numeric>
#include <unordered_map>

namespace
{
    // Double precision equality checking epsilon.
    constexpr double EPS = 1e-5;

} // namespace

namespace lqr
{

LQRTree::LQRTree(int state_dim, int control_dim)
    : state_dim_(state_dim),
      control_dim_(control_dim),
      ZERO_VALUE_MATRIX_(Eigen::MatrixXd::Zero(state_dim, state_dim))
{
}

std::shared_ptr<PlanNode> LQRTree::make_plan_node(
                                const Eigen::MatrixXd &A,
                                const Eigen::MatrixXd &B,
                                const Eigen::MatrixXd &Q,
                                const Eigen::MatrixXd &R,
                                const double probability)
{
    std::shared_ptr<lqr::PlanNode> plan_node 
        = std::make_shared<lqr::PlanNode>(state_dim_, control_dim_, A, B, Q, R, probability);

    // Update the linearization and quadraticization of 
    // the dynamics and cost respectively. (does nothing...)
    plan_node->update_dynamics();
    plan_node->update_cost();

    return plan_node;
}

TreeNodePtr LQRTree::add_root(
        const Eigen::MatrixXd &A,
        const Eigen::MatrixXd &B,
        const Eigen::MatrixXd &Q,
        const Eigen::MatrixXd &R
        )
{
    return add_root(make_plan_node(A, B, Q, R, 1.0)); 
}

TreeNodePtr LQRTree::add_root(const std::shared_ptr<PlanNode> &plan_node)
{
    tree_ = data::Tree<PlanNode>(plan_node);
    return tree_.root();
}

std::vector<TreeNodePtr> LQRTree::add_nodes(const std::vector<std::shared_ptr<PlanNode>> &plan_nodes, 
        TreeNodePtr &parent)
{
    // Confirm the probabilities in the plan nodes sum to 1.
    const double probability_sum = 
        std::accumulate(plan_nodes.begin(), plan_nodes.end(), 0.0,
            [](const double a, const std::shared_ptr<PlanNode> &node) 
            {
                return a + node->probability_;
            }
            );
    IS_ALMOST_EQUAL(probability_sum, 1.0, EPS); // Throw error if sum is not close to 1.0

    // Create tree nodes from the plan nodes and add them to the tree.
    std::vector<TreeNodePtr> children;
    children.reserve(plan_nodes.size());
    for (const auto &plan_node : plan_nodes)
    {
        children.emplace_back(tree_.add_child(parent, plan_node));
    }
    return children;
}

TreeNodePtr LQRTree::root()
{
    return tree_.root();
}

void LQRTree::forward_pass(const Eigen::VectorXd &x0)
{
    // Process from the end of the list, but start at the beginning.
    std::list<std::pair<TreeNodePtr, Eigen::MatrixXd>> to_process;
    // First x linearization is just from the root, not from rolling out dynamics.
    Eigen::VectorXd xt = x0;
    to_process.emplace_front(tree_.root(), xt);
    while (!to_process.empty())
    {
        auto &process_pair = to_process.back();
        const Eigen::MatrixXd xt1 = forward_node(process_pair.first->item(), 
                process_pair.second); 
        for (auto child : process_pair.first->children())
        {
            to_process.emplace_front(child, xt1);
        }

        to_process.pop_back();
    }
}

Eigen::MatrixXd LQRTree::forward_node(std::shared_ptr<PlanNode> node, 
        const Eigen::MatrixXd &xt)
{
    const Eigen::VectorXd ut = node->K_ * xt;

    // Set the new linearization point at the new xt for the node.
    node->set_x(xt);
    node->set_u(ut);
    node->update_dynamics(); 
    node->update_cost();

    // Go to the next state.
    Eigen::VectorXd xt1 = node->dynamics_.A * xt + node->dynamics_.B *ut;
    return xt1;
}



void LQRTree::bellman_tree_backup()
{
   // Special case to compute the control policy and value matrices for the leaf nodes.
   control_and_value_for_leaves();

   // Start at all the leaf nodes (currently assume they are all at the same depth) and work our
   // way up the tree until we get the root node (single node at depth = 0).
   auto all_children = tree_.leaf_nodes();
   while (!(all_children.size() == 1 && all_children.front()->depth() == 0))
   {
       // For all the children, back up their value function to their parents. 
       // To work up the tree, the parents become the new children.
       all_children = backup_to_parents(all_children);
   } 
}

void LQRTree::control_and_value_for_leaves()
{
   auto leaf_nodes = tree_.leaf_nodes();

   // Confirm all leaves are at the same depth in the tree. This isn't necessary for the
   // general algorithm, but we need it for the current implementation.
   const int FIRST_DEPTH = leaf_nodes.size() > 0 ? leaf_nodes.front()->depth() : -1;
   for (auto &leaf: leaf_nodes)
   {
       IS_EQUAL(leaf->depth(), FIRST_DEPTH);
       std::shared_ptr<PlanNode> node = leaf->item();
       // Compute the leaf node's control policy K, k using a Zero Value matrix for the future.
       compute_control_policy(node, ZERO_VALUE_MATRIX_);
       node->V_ = compute_value_matrix(node, ZERO_VALUE_MATRIX_);
   }
}

std::list<TreeNodePtr> LQRTree::backup_to_parents(const std::list<TreeNodePtr> &all_children)
{
   // Hash the leaves by their parent so we can process all the children for a parent.    
   std::unordered_map<TreeNodePtr, std::list<TreeNodePtr>> parent_map;

   // Confirm all children are at the same depth in the tree. This isn't necessary for the
   // general algorithm, but we need it for the current implementation.
   const int FIRST_DEPTH = all_children.size() > 0 ? all_children.front()->depth() : -1;
   for (auto &child : all_children)
   {
       IS_EQUAL(child->depth(), FIRST_DEPTH);
       parent_map[child->parent()].push_back(child);
   }

   std::list<TreeNodePtr> parents;
   for (auto &parent_children_pair : parent_map)
   {
       // Compute the weighted \tilde{V}_{t+1} = \sum_k p_k V_{T+1}^{(k)} matrix by computing the
       // probability-weighted average 
       Eigen::MatrixXd Vtilde = ZERO_VALUE_MATRIX_;
       auto &children = parent_children_pair.second;
       for (auto &child : children)
       {
           const std::shared_ptr<PlanNode> &child_node = child->item();
           Vtilde += child_node->probability_ * child_node->V_;
       }

       std::shared_ptr<PlanNode> parent_node = parent_children_pair.first->item();
       // Compute the parent node's control policy K, k using Vtilde.
       compute_control_policy(parent_node, Vtilde);
       // Compute parent's Vt from this the Vtilde (from T+1) and the control policy K,k computed above.
       parent_node->V_ = compute_value_matrix(parent_node, Vtilde);

       parents.push_back(parent_children_pair.first);
   }

   return parents;
}

Eigen::MatrixXd LQRTree::compute_value_matrix(const std::shared_ptr<PlanNode> &node, 
                                               const Eigen::MatrixXd &Vt1)
{
    // Extract dynamics terms.
    const Eigen::MatrixXd &A = node->dynamics_.A;
    const Eigen::MatrixXd &B = node->dynamics_.B;
    // Extract cost terms.
    const Eigen::MatrixXd &Q = node->cost_.Q;
    const Eigen::MatrixXd &R = node->cost_.R;
    // Extract control policy terms.
    const Eigen::MatrixXd &K = node->K_;

    const auto tmp = (A + B*K);
    const Eigen::MatrixXd Vt = Q + K.transpose()*R*K + tmp.transpose()*Vt1*tmp;

    return Vt;
}

void LQRTree::compute_control_policy(std::shared_ptr<PlanNode> &node, const Eigen::MatrixXd &Vt1)
{
    const Eigen::MatrixXd &A = node->dynamics_.A;
    const Eigen::MatrixXd &B = node->dynamics_.B;
    // Extract cost terms.
    const Eigen::MatrixXd &R = node->cost_.R;

    node->check_sizes();

    const Eigen::MatrixXd inv_cntrl_term = (R + B.transpose()*Vt1*B).inverse();

    node->K_ = inv_cntrl_term * (B.transpose()*Vt1*A);
    node->K_ *= -1.0;
}

} // namespace lqr
