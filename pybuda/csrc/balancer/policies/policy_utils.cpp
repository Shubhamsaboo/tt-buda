// SPDX-FileCopyrightText: © 2024 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0
#include "balancer/policies/policy_utils.hpp"

#include <experimental/filesystem>
#include <fstream>

#include "balancer/legalizer/legalizer.hpp"
#include "passes/fork_join.hpp"
#include "placer/dram.hpp"
#include "placer/interactive_placer.hpp"
#include "placer/lower_to_placer.hpp"
#include "scheduler/scheduler.hpp"
#include "shared_utils/placement_printer.hpp"
#include "shared_utils/pretty_table.hpp"

using Graph = tt::graphlib::Graph;
using Node = tt::graphlib::Node;
using NodeType = tt::graphlib::NodeType;
using Edge = tt::graphlib::Edge;
using DataFormat = tt::DataFormat;

namespace tt::balancer
{

OpModelMap to_op_model_map(OpModels const &selected_op_models)
{
    OpModelMap op_model_map;
    for (auto const &[node, op_model] : selected_op_models)
    {
        op_model_map.insert({node->name(), op_model});
    }
    return op_model_map;
}

placer::PlacerSolution run_placer(
    Graph const *graph, const BalancerConfig &config, OpModelMap const &selected_op_models)
{
    std::unordered_map<std::string, tt::placer::GridShape> op_to_grid_shape;
    std::unordered_map<std::string, tt::placer::GridShape> input_queue_to_grid_shape;
    for (auto [node_name, op_model] : selected_op_models)
    {
        Node *node = graph->get_node_by_name(node_name);
        switch (node->node_type())
        {
            case NodeType::kInput:
            {
                input_queue_to_grid_shape.insert(
                    {node_name,
                     tt::placer::GridShape(
                         (std::uint32_t)op_model.grid_shape.r, (std::uint32_t)op_model.grid_shape.c)});
                break;
            }
            case NodeType::kBudaOp:
            {
                op_to_grid_shape.insert(
                    {node_name,
                     tt::placer::GridShape(
                         (std::uint32_t)op_model.grid_shape.r, (std::uint32_t)op_model.grid_shape.c)});
                break;
            }
            default: break;
        }
    }

    scheduler::Schedule scheduled_ops = run_scheduler(config.scheduler_config, graph);

    placer::PlacerConfig placer_config = {
        .chip_ids = config.chip_ids,
        .chip_placement_policy = config.chip_placement_policy,
        .device_config = config.device_config,
        .device_grid =
            placer::GridShape((uint32_t)config.device_config.grid_size.r, (uint32_t)config.device_config.grid_size.c),
        .contains_recompute = graph->contains_recompute_nodes(),
        .output_queues_on_host = config.output_queues_on_host,
        .strategy = placer::PlacementStrategy::LeftToRight,
        .op_to_grid_shape = op_to_grid_shape,
        .input_queue_to_grid_shape = input_queue_to_grid_shape,
        .op_to_epoch_type = placer::lowering::get_op_to_epoch_type_mapping(graph, scheduled_ops),
        .op_to_grad_op = placer::lowering::get_op_to_grad_op_mapping(graph, scheduled_ops),
        .op_to_recompute_op = placer::lowering::get_op_to_recompute_mapping(graph, scheduled_ops),
        .ops_tagged_for_chip_id_break = placer::lowering::tag_ops_for_chip_break(
            config.device_config.arch_name,
            config.op_names_to_chip_break,
            scheduled_ops,
            graph,
            config.use_interactive_placer),
        .ops_tagged_for_epoch_break = placer::lowering::tag_ops_for_epoch_break(
            config.device_config.arch_name,
            config.op_names_to_epoch_break,
            config.op_names_to_chip_break,
            scheduled_ops,
            graph,
            config.use_interactive_placer),
        .ops_tagged_for_temporal_epoch_break = placer::lowering::tag_ops_for_temporal_epoch_break(
            graph, scheduled_ops, config.op_name_to_placer_overrides),
        .fwd_to_bwd_nodes = placer::lowering::get_fwd_to_bwd_nodes(graph),
        .fwd_to_opt_nodes = placer::lowering::get_fwd_to_opt_nodes(graph, scheduled_ops),
        .output_ops = placer::lowering::get_output_nodes(graph),
        .op_to_chip_id_assignment = config.op_to_chip_id_assignment,
        .op_to_overrides = config.op_name_to_placer_overrides,
        .enable_auto_transposing_placement = config.enable_auto_transposing_placement,
    };

    // NB: We can avoid introducing both core-graph-lib and autograd modules in as dependencies
    // if we move the lowering code (relevant dependencies on both packages) here. Alternatively
    // only have lowering.hpp/cpp files depend on core-graph-lib/autograd
    placer::PlacerSolution solution = placer::placer(placer_config, scheduled_ops);

    // Visualize placement
    if (env_as<bool>("PYBUDA_BALANCER_PLACER_DATA"))
    {
        const std::string placement_dir_path = "bp_data";
        std::experimental::filesystem::create_directory(placement_dir_path);
        std::string file_name = placement_dir_path + "/" + (graph->name().empty() ? "noname" : graph->name()) + "_" +
                                policy_to_string(config.policy_type) + ".txt";
        std::ofstream of(file_name);
        dump_balancer_placer_data(
            graph, config.chip_ids, solution, selected_op_models, of, config.device_config.arch_name);
    }

    return solution;
}

std::vector<uint> get_num_epochs_per_node_epoch_type(Graph const *graph, tt::placer::PlacerSolution placer_solution)
{
    (void)graph;
    constexpr int NUM_EPOCH_TYPES = 3;
    constexpr std::array<NodeEpochType, NUM_EPOCH_TYPES> epoch_types = {
        NodeEpochType::Forward, NodeEpochType::Backward, NodeEpochType::Optimizer};

    std::vector<uint> num_epochs_per_node_type(NUM_EPOCH_TYPES, 0);
    std::unordered_map<uint, std::vector<std::string>> epoch_to_op_names;

    for (uint i = 0; i < placer_solution.num_epochs; i++)
    {
        epoch_to_op_names.emplace(i, std::vector<std::string>());
    }

    for (auto kvp : placer_solution.name_to_op_placement)
    {
        epoch_to_op_names.at(kvp.second.epoch_id()).push_back(kvp.first);
    }

    for (int i = 0; i < NUM_EPOCH_TYPES; ++i)
    {
        num_epochs_per_node_type[i] = placer_solution.num_temporal_epochs(epoch_types[i]);
    }

    // Pop opt and bwd if not training mode

    while (num_epochs_per_node_type.back() == 0)
    {
        num_epochs_per_node_type.pop_back();
    }

    return num_epochs_per_node_type;
}

void dump_balancer_placer_data(
    Graph const *graph,
    std::vector<std::uint32_t> chip_ids,
    tt::placer::PlacerSolution const &placer_solution,
    OpModelMap const &op_model_map,
    std::ostream &of,
    const std::string &arch_name)
{
    if (not env_as<bool>("PYBUDA_BALANCER_PLACER_DATA"))
        return;

    // Create some supporting structures
    std::unordered_map<std::string, int> op_name_to_id_map;
    for (std::pair<const std::string, tt::placer::OpPlacement> kvp : placer_solution.name_to_op_placement)
    {
        op_name_to_id_map.emplace(kvp.first, graph->get_node_by_name(kvp.first)->id());
    }

    std::vector<std::pair<std::string, int>> sorted_op_id_name_pairs;
    std::transform(
        op_name_to_id_map.begin(),
        op_name_to_id_map.end(),
        std::back_inserter(sorted_op_id_name_pairs),
        [](const std::pair<const std::string, int> &kvp) { return kvp; });

    std::sort(
        sorted_op_id_name_pairs.begin(),
        sorted_op_id_name_pairs.end(),
        [](const auto &lhs, const auto &rhs) { return lhs.second < rhs.second; });

    // Create mapping of op id to new set of ids that are in [0, N)
    std::unordered_map<int, int> original_id_to_visualized_id;
    int new_id = 0;
    for (std::pair<std::string, int> kvp : sorted_op_id_name_pairs)
    {
        original_id_to_visualized_id.emplace(kvp.second, new_id);
        new_id++;
    }

    // Placer doesn't have access to graph and PlacerSolution is NodeEpochType-agnostic, so printer will be called here
    // Whether we're training or not, should be read from compiler config, but hack it for now
    uint node_epoch_types_count = graph->contains_bwd_nodes() ? 3 : 1;
    std::vector<uint> epochs_per_epoch_type = get_num_epochs_per_node_epoch_type(graph, placer_solution);

    tt::utils::PlacementPrinter::DeviceType dev_type = (arch_name == "grayskull")
                                                           ? tt::utils::PlacementPrinter::DeviceType::Grayskull
                                                           : tt::utils::PlacementPrinter::DeviceType::Wormhole;

    std::uint32_t max_chip_id = 0;
    for (std::uint32_t chip_id : chip_ids)
    {
        max_chip_id = std::max(max_chip_id, chip_id);
    }

    tt::utils::PlacementPrinter printer(dev_type, node_epoch_types_count, epochs_per_epoch_type, max_chip_id + 1);

    for (auto &kvp : placer_solution.name_to_op_placement)
    {
        std::string name = kvp.first;
        tt::placer::OpPlacement opPlacement = kvp.second;

        auto coords = opPlacement.placed_cores;

        printer.fillRectangle(
            placer_solution.temporal_epoch_id(name),
            opPlacement.chip_id,
            coords.start.row,
            coords.start.col,
            coords.end.row,
            coords.end.col,
            original_id_to_visualized_id.at(op_name_to_id_map[name])  // prints id for visualization
        );
    }

    of << printer.generatePlacementString();

    // Print op data
    tt::utils::PrettyTable table;
    table.add_row(
        {"Visual id",
         "Op id",
         "Op name",
         "Op type",
         "Grid (RxC)",
         "Cores",
         "Cycles",
         "mblock (t)",
         "ublock (u_kt)",
         "Data fmt",
         "Math fdlty",
         "L1 mem (kb)"});

    for (auto &kvp : sorted_op_id_name_pairs)
    {
        const std::string op_name = kvp.first;
        const int op_id = kvp.second;

        // Since op type is of format "BudaOp::matmul", we remove the prefix
        std::string op_type = graph->node_by_id(op_id)->get_type();
        TT_ASSERT(op_type.substr(0, 8) == "BudaOp::", "Op not a buda op!");
        op_type = op_type.substr(8);

        std::string placed_core_shapes;
        int placed_cores_volume = 0;
        tt::placer::CoordRange coord_range = placer_solution.name_to_op_placement.at(op_name).placed_cores;
        placed_core_shapes += " " + std::to_string(coord_range.size_r()) + "x" + std::to_string(coord_range.size_c());
        placed_cores_volume += coord_range.size_r() * coord_range.size_c();

        const OpModel &op_model = op_model_map.at(op_name);

        std::string execution_cycles = std::to_string(op_model.get_execution_cycles(arch_name));
        std::string memory_used_kb = round_float(op_model.get_l1_memory_usage() / 1024.f, 2);
        std::string mblock = std::to_string(op_model.block_shape().mblock_m) + "x" +
                             std::to_string(op_model.block_shape().mblock_n) + " " +
                             std::to_string(op_model.block_shape().t);
        std::string ublock =
            std::to_string(op_model.block_shape().ublock.rt) + "x" + std::to_string(op_model.block_shape().ublock.ct);
        std::string data_format = ((std::stringstream &)(std::stringstream() << op_model.data_format)).str();
        std::string math_fidelity = ((std::stringstream &)(std::stringstream() << op_model.math_fidelity())).str();

        table.add_row({
            std::to_string(original_id_to_visualized_id.at(op_id)),
            std::to_string(op_id),
            op_name,
            op_type,
            placed_core_shapes,
            std::to_string(placed_cores_volume),
            execution_cycles,
            mblock,
            ublock,
            data_format,
            math_fidelity,
            memory_used_kb,
        });
    }

    of << table.generate_table_string(tt::utils::PrettyTable::Format::Pretty) << std::endl;

    int epoch_id = 0;
    int total_cost = 0;
    std::vector<EpochCost> epoch_costs = calculate_epoch_costs(placer_solution, op_model_map, arch_name);
    fmt::print(of, "Epoch costs:\n");
    for (EpochCost epoch_cost : epoch_costs)
    {
        fmt::print(of, "  {}: {} cycles\n", epoch_id++, epoch_cost.setup_cycles + epoch_cost.runtime_cycles);
        total_cost += epoch_cost.setup_cycles + epoch_cost.runtime_cycles;
    }
    fmt::print(of, "  Total: {} cycles\n", total_cost);

    // TODO: print graph of ops to file stream
    // Consider graphviz:
    // -
    // https://stackoverflow.com/questions/9181183/how-to-print-a-boost-graph-in-graphviz-with-one-of-the-properties-displayed
    // - https://stackoverflow.com/questions/33301493/network-graph-visualisation
}

std::vector<EpochCost> calculate_epoch_costs(
    placer::PlacerSolution const &placer_solution, OpModelMap const &selected_op_models, std::string const &arch_name)
{
    std::vector<EpochCost> epoch_costs;
    epoch_costs.resize(placer_solution.num_epochs);
    for (auto const &[node, placement] : placer_solution.name_to_op_placement)
    {
        OpModel const &op_model = selected_op_models.at(node);
        epoch_costs[placement.epoch_id()].runtime_cycles =
            std::max(epoch_costs[placement.epoch_id()].runtime_cycles, op_model.get_execution_cycles(arch_name));
    }
    return epoch_costs;
}

void epoch_or_chip_break_remove_processed_nodes(
    const Graph *graph,
    std::vector<tt::scheduler::Schedule> &op_names_to_epoch_or_chip_break,
    const std::unordered_set<const tt::graphlib::Node *> &processed_nodes)
{
    if (processed_nodes.empty())
    {
        return;
    }

    auto it = op_names_to_epoch_or_chip_break.begin();
    while (it != op_names_to_epoch_or_chip_break.end())
    {
        auto &op_names = *it;
        auto op_names_it = op_names.begin();
        bool delete_op_names = false;
        while (op_names_it != op_names.end())
        {
            auto &op_name = *op_names_it;
            auto node = graph->get_node_by_name(op_name);
            if (processed_nodes.find(node) != processed_nodes.end())
            {
                delete_op_names = true;
                break;
            }
            else
            {
                ++op_names_it;
            }
        }

        if (delete_op_names)
        {
            it = op_names_to_epoch_or_chip_break.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::pair<scheduler::Schedule, std::unordered_set<string>> policy_run_scheduler(
    graphlib::Graph const *graph,
    BalancerConfig const &config,
    const std::unordered_set<const tt::graphlib::Node *> &processed_nodes,
    const tt::scheduler::Schedule &processed_schedule,
    std::vector<tt::scheduler::Schedule> &op_names_to_epoch_break)
{
    std::vector<tt::scheduler::Schedule> op_names_to_chip_break;
    const auto [scheduled_ops, epoch_break_ops, chip_break_ops] = policy_run_scheduler(
        graph, config, processed_nodes, processed_schedule, op_names_to_epoch_break, op_names_to_chip_break);
    return make_pair(std::move(scheduled_ops), std::move(epoch_break_ops));
}

std::tuple<scheduler::Schedule, std::unordered_set<string>, std::unordered_set<string>> policy_run_scheduler(
    graphlib::Graph const *graph,
    BalancerConfig const &config,
    const std::unordered_set<const tt::graphlib::Node *> &processed_nodes,
    const tt::scheduler::Schedule &processed_schedule,
    std::vector<tt::scheduler::Schedule> &op_names_to_epoch_break,
    std::vector<tt::scheduler::Schedule> &op_names_to_chip_break)
{
    scheduler::SchedulerConfig scheduler_config = config.scheduler_config;
    if (processed_nodes.size() > 0)
    {
        TT_ASSERT(processed_nodes.size() == processed_schedule.size());
        scheduler_config.ignored_nodes = &processed_nodes;
        scheduler_config.scheduler_constraints.push_back(processed_schedule);
    }

    scheduler::Schedule scheduled_ops = run_scheduler(scheduler_config, graph);

    epoch_or_chip_break_remove_processed_nodes(graph, op_names_to_epoch_break, processed_nodes);
    epoch_or_chip_break_remove_processed_nodes(graph, op_names_to_chip_break, processed_nodes);
    std::unordered_set<string> epoch_break_ops = placer::lowering::tag_ops_for_epoch_break(
        config.device_config.arch_name,
        op_names_to_epoch_break,
        op_names_to_chip_break,
        scheduled_ops,
        graph,
        config.use_interactive_placer);
    std::unordered_set<string> chip_break_ops = placer::lowering::tag_ops_for_chip_break(
        config.device_config.arch_name, op_names_to_chip_break, scheduled_ops, graph, config.use_interactive_placer);

    return make_tuple(std::move(scheduled_ops), std::move(epoch_break_ops), std::move(chip_break_ops));
}

// Cuts OPs in current epoch from rest of the graph.
//
void cut_graph_solver_epoch(
    const graphlib::Graph *graph, placer::InteractivePlacer &placer, legalizer::GraphSolver &graph_solver)
{
    // Only cut edges from ops that have been placed already
    balancer::CutEdges const &already_cut_edges = graph_solver.get_cut_edges();
    std::vector<std::string> const &current_epoch_ops = placer.current_epoch_ops();
    std::vector<graphlib::Edge> edges_to_cut;
    for (auto const &op_name : current_epoch_ops)
    {
        for (auto const &edge : graph->user_data_edges(graph->get_node_by_name(op_name)))
        {
            auto *user = graph->node_by_id(edge.consumer_node_id);
            if (user->node_type() != graphlib::NodeType::kBudaOp)
                continue;

            if (already_cut_edges.find(edge) != already_cut_edges.end())
                continue;

            if (std::find(current_epoch_ops.begin(), current_epoch_ops.end(), user->name()) != current_epoch_ops.end())
                continue;

            edges_to_cut.push_back(edge);
        }
    }

    if (edges_to_cut.size() > 0)
    {
        graph_solver.cut(edges_to_cut, true /*epoch_cut*/);
    }
}

// Validate that all ops in scheduled_ops have been placed in placer_solution.
//
void validate_solution(const scheduler::Schedule &scheduled_ops, const placer::PlacerSolution &placer_solution)
{
    if (placer_solution.name_to_op_placement.size() < scheduled_ops.size())
    {
        log_error(LogBalancer, "Some ops haven't been placed:");
        for (std::size_t i = 0; i < scheduled_ops.size(); i++)
        {
            if (placer_solution.name_to_op_placement.count(scheduled_ops[i]) == 0)
            {
                log_error(LogBalancer, "  - {}", scheduled_ops[i]);
            }
        }
        TT_THROW("Failed to place all ops.");
    }
}

// Merge buffering queues and ops for total current epoch nodes.
// Most balancer policies will track and work with op nodes only
// but for setting proper traversal contexts we need other nodes as well.
//
std::unordered_set<const tt::graphlib::Node *> calculate_current_epoch_nodes(
    const Graph *graph, const std::unordered_set<const tt::graphlib::Node *> &current_epoch_ops)
{
    std::unordered_set<const tt::graphlib::Node *> current_epoch_nodes(current_epoch_ops);

    for (const Node *op_node : current_epoch_ops)
    {
        for (Node *node : graph->data_operands(op_node))
        {
            if (node->node_type() == NodeType::kQueue and current_epoch_ops.count(graph->data_operands(node)[0]) > 0)
            {
                TT_ASSERT(node->as<graphlib::QueueNode>()->is_buffering());
                current_epoch_nodes.insert(node);
            }
        }
    }

    return current_epoch_nodes;
}

// Invoke SET of selected op_model on graphsolver instance for given node.
//
void set_op_model_for_node(
    legalizer::GraphSolver &graph_solver,
    const graphlib::Node *node,
    const OpModel &selected_op_model,
    std::string const &arch_name)
{
    graph_solver.set(node, selected_op_model);
    log_debug(
        LogBalancer,
        "Selected grid for node {} is {}, {}, {}, cycles {}",
        node->name(),
        selected_op_model.grid_shape,
        selected_op_model.t_stream_factor,
        selected_op_model.output_buffers[0].block_shape.ublock,
        selected_op_model.get_execution_cycles(arch_name));
}

void set_op_model_for_node_ribbon(
    legalizer::GraphSolver &graph_solver,
    const graphlib::Node *op,
    const OpModel &selected_op_model,
    std::uint32_t current_ribbon_size)
{
    log_trace(
        LogBalancer,
        "Selected grid for op {}: {}, {}, t-stream: {}, current_ribon={}",
        op->name(),
        selected_op_model.grid_shape.r,
        selected_op_model.grid_shape.c,
        selected_op_model.t_stream_factor,
        current_ribbon_size);
    graph_solver.set(op, selected_op_model);
}

int ribbon_buffering_factor(const OpModel &op_model) { return op_model.grid_shape.r; }

void cut_graph_solver_ribbon(
    const graphlib::Graph *graph,
    const graphlib::Node *op,
    placer::InteractivePlacer &placer,
    legalizer::GraphSolver &graph_solver)
{
    CutEdges pre_cut_edges = graph_solver.get_cut_edges();

    // Only cut edges from ops that have been placed already
    std::vector<graphlib::Edge> edges_to_cut;
    for (auto &edge : graph->operand_data_edges(op))
    {
        if (placer.op_placed(graph->node_by_id(edge.producer_node_id)->name()) && pre_cut_edges.count(edge) == 0)
        {
            edges_to_cut.push_back(edge);
        }
    }

    if (edges_to_cut.size() > 0)
    {
        log_debug(LogBalancer, "Cutting {} edges to {}", edges_to_cut.size(), op->name());
        graph_solver.cut(edges_to_cut);
    }
}

bool is_matmul(const graphlib::BudaOpNode *op)
{
    if (!op->is_matmul_not_sparse())
        return false;

    if (op->has_tag("reduce_r") || op->has_tag("reduce_c"))
        return false;

    return true;
}

bool prologue_ok(const OpModel &op_model)
{
    bool needs_prologue = op_model.buda_op_node->is_matmul();  // others don't matter much, as they are small
    bool has_prologue = false;
    if (needs_prologue)
    {
        if (op_model.buda_op_node->is_sparse_matmul())
        {
            TT_ASSERT(op_model.parameter_buffers.size() == 3);
            has_prologue = op_model.parameter_buffers[0] && op_model.parameter_buffers[2];
        }
        else if (op_model.buda_op_node->is_dense_matmul())
        {
            TT_ASSERT(op_model.parameter_buffers.size() > 1);
            has_prologue = op_model.parameter_buffers[1];
        }
        else
        {
            has_prologue = op_model.parameter_buffers.size() > 1 and op_model.parameter_buffers[1];
        }
    }

    bool prologue_ok = !needs_prologue || has_prologue;

    return prologue_ok;
}

bool ukt_ok(const OpModel &op_model)
{
    if (op_model.buda_op_node->is_matmul_not_sparse())
    {
        return op_model.input_buffers[0].block_shape.ublock.ct >= 4;
    }
    else if (op_model.buda_op_node->is_sparse_matmul())
    {
        return op_model.input_buffers[1].block_shape.ublock.rt >= 4;
    }

    return true;
}

bool mblock_size_ok(const OpModel &op_model)
{
    if (op_model.block_shape().t > 1)
    {
        return op_model.block_shape().volume_no_t() >= 8;
    }

    return true;
}

bool close_to_target_exec_cycles(int kernel_exec_cycles, int limiter_cycles, int target)
{
    return (limiter_cycles < target) && (kernel_exec_cycles > target * 0.8);
}

// OpModel preference comparison function. Returns true if candidate is better than current pick.
//
bool is_candidate_better_than_current(
    const OpModel &current,
    const OpModel &candidate,
    const Graph *graph,
    int ribbon_size,
    int target_exec_cycles,
    const DeviceConfig &device_config)
{
    TT_ASSERT(current.buda_op_node == candidate.buda_op_node);

    // Op model compare version. If making major changes increment version and put the newest behaviour under that
    // version.
    //
    int op_model_compare_version = env_as<int>("PYBUDA_OP_MODEL_COMPARE_VERSION", 2);

    if (std::abs(ribbon_size - candidate.grid_shape.r) < std::abs(ribbon_size - current.grid_shape.r))
    {
        return true;
    }
    else if (std::abs(ribbon_size - candidate.grid_shape.r) > std::abs(ribbon_size - current.grid_shape.r))
    {
        return false;
    }

    // If both are same diff from target ribbon size, prefer smaller one.
    // It makes smaller "disturbance" to targeted ribbon and uses smaller number of cores.
    //
    if (candidate.grid_shape.r != current.grid_shape.r)
    {
        return candidate.grid_shape.r < current.grid_shape.r;
    }

    bool candidate_prologue_ok = prologue_ok(candidate);
    bool current_prologue_ok = prologue_ok(current);

    if (candidate_prologue_ok > current_prologue_ok)
    {
        return true;
    }
    else if (candidate_prologue_ok < current_prologue_ok)
    {
        return false;
    }

    int current_cycles = get_limiter_cycles(current, graph, device_config);
    int candidate_cycles = get_limiter_cycles(candidate, graph, device_config);

    // Both op_models are within target. Prefer smaller number of columns.
    //
    if (candidate_cycles <= target_exec_cycles and current_cycles <= target_exec_cycles)
    {
        if (candidate.grid_shape.c < current.grid_shape.c)
        {
            return true;
        }
        else if (candidate.grid_shape.c > current.grid_shape.c)
        {
            return false;
        }
    }

    bool ukt_ok_candidate = ukt_ok(candidate);
    bool ukt_ok_current = ukt_ok(current);

    if (ukt_ok_candidate > ukt_ok_current)
    {
        return true;
    }
    else if (ukt_ok_candidate < ukt_ok_current)
    {
        return false;
    }

    bool mblock_size_ok_candidate = mblock_size_ok(candidate);
    bool mblock_size_ok_current = mblock_size_ok(current);
    if (mblock_size_ok_candidate > mblock_size_ok_current)
    {
        return true;
    }
    else if (mblock_size_ok_candidate < mblock_size_ok_current)
    {
        return false;
    }

    // (1) if both are close to target, pick the one with the largest block (volume_no_t)
    // (2) if only one is close to target, pick that one
    // (3) if both are far from target, pick the one that is closer to target (in terms of execution
    // cycles)

    int current_exec_cycles = current.get_execution_cycles(device_config.arch_name);
    int candidate_exec_cycles = candidate.get_execution_cycles(device_config.arch_name);
    float current_exec_util = (float)current_exec_cycles / (float)current_cycles;
    float candidate_exec_util = (float)candidate_exec_cycles / (float)candidate_cycles;

    if (op_model_compare_version == 2)
    {
        if (close_to_target_exec_cycles(current_exec_cycles, current_cycles, target_exec_cycles))
        {
            if (close_to_target_exec_cycles(candidate_exec_cycles, candidate_cycles, target_exec_cycles))
            {
                if (candidate.block_shape().volume_no_t() > current.block_shape().volume_no_t())
                {
                    return true;
                }
                else if (candidate.block_shape().volume_no_t() == current.block_shape().volume_no_t())
                {
                    if (candidate_exec_util > current_exec_util)
                    {
                        return true;
                    }
                }
            }
        }
        else if (close_to_target_exec_cycles(candidate_exec_cycles, candidate_cycles, target_exec_cycles))
        {
            return true;
        }
        else
        {
            if (candidate_cycles <= target_exec_cycles)
            {
                if (current_cycles > target_exec_cycles)
                {
                    return true;
                }
                else
                {
                    if (candidate.block_shape().volume_no_t() > current.block_shape().volume_no_t())
                    {
                        return true;
                    }
                    else if (candidate.block_shape().volume_no_t() == current.block_shape().volume_no_t())
                    {
                        if (candidate_exec_util > current_exec_util)
                        {
                            return true;
                        }
                    }
                }
            }
            else if (candidate_cycles < current_cycles)
            {
                return true;
            }
        }
    }
    else if (op_model_compare_version == 1)
    {
        if (close_to_target(current_cycles, target_exec_cycles))
        {
            if (close_to_target(candidate_cycles, target_exec_cycles))
            {
                if (candidate.block_shape().volume_no_t() > current.block_shape().volume_no_t())
                {
                    return true;
                }
            }
        }
        else if (close_to_target(candidate_cycles, target_exec_cycles))
        {
            return true;
        }
        else if (std::abs(target_exec_cycles - candidate_cycles) < std::abs(target_exec_cycles - current_cycles))
        {
            return true;
        }
    }

    return false;
}

bool validate_sparse_matmul_model(
    const graphlib::BudaOpNode *op,
    const OpModel &op_model,
    const graphlib::Graph *graph,
    std::unordered_set<std::uint64_t> &validated_cache)
{
    if (validated_cache.count(op_model.id.id) > 0)
        return true;

    TT_ASSERT(op->is_sparse_matmul());

    int grid_r = op_model.grid_shape.r;
    int u_rt = op_model.output_buffers[0].block_shape.ublock.rt;
    int u_kt = op_model.input_buffers[1].block_shape.ublock.rt;
    bool has_buffer_op = op_model.has_sparse_buffer();
    bool force_buffer_op_layout = env_as<bool>("PYBUDA_FORCE_SPARSE_BUFFER_LAYOUT");
    bool buffer_op_layout = has_buffer_op or force_buffer_op_layout;
    const sparse::SparseBUDA &sparse_buda =
        graph->data_operands(op)[0]->as<graphlib::ConstantInputNode>()->get_sparse_buda();
    auto layout = sparse::SparseBUDA::create_layout(
        buffer_op_layout, op_model.t_stream_factor.dir.z_major(), op_model.fracture_factor);

    std::string visualize_sparse_path = "";
    try
    {
        auto [sparse, encodings, sparse_s, encodings_s, num_strips_per_row] =
            sparse_buda.get_sparse_tiles_and_encodings(
                grid_r,
                op_model.t_stream_factor.r,
                op_model.t_stream_factor.c,
                u_rt,
                u_kt,
                op_model.fracture_factor,
                layout,
                visualize_sparse_path);
    }
    catch (...)
    {
        log_trace(LogBalancer, "RIBBON2: Rejecting sparse matmul that can't be encoded: {}", op->name());
        return false;  // we can't encode this model
    }
    validated_cache.insert(op_model.id.id);
    return true;
}

bool can_fit_on_single_epoch(
    tt::placer::InteractivePlacer &ip_fittment_tester,
    const std::string &op_name_1,
    const tt::balancer::GridShape &op_shape_1,
    const std::string &op_name_2,
    const tt::balancer::GridShape &op_shape_2,
    bool enable_transpose)
{
    TT_ASSERT(ip_fittment_tester.current_epoch_empty(), "Test placer epoch must be empty!");
    std::optional<placer::CoordRange> test_placement;

    test_placement = ip_fittment_tester.place_op(op_name_1, op_shape_1, enable_transpose);

    TT_ASSERT(test_placement.has_value(), "Single op must always fit!");

    test_placement = ip_fittment_tester.place_op(op_name_2, op_shape_2, enable_transpose);

    ip_fittment_tester.rewind_epoch();
    return test_placement.has_value();
}

// Pick ribbon size for a given window of ops. The assumption is that all of them have the same r/c image dimension.
//
std::uint32_t pick_ribbon_size(
    std::uint32_t start_index,
    std::uint32_t end_index,  // end is not inclusive
    const Graph *graph,
    const legalizer::GraphSolver &graph_solver,
    const std::vector<std::string> &scheduled_ops,
    std::uint32_t device_rows)
{
    // set some tile limits. Min number ensures big enough blocks to keep perf running reasonably, and max avoids
    // blob sizes from exploding.
    std::uint32_t min_tile_height = env_as<int>("PYBUDA_RIBBON_MIN_TILE_HEIGHT", 1);
    std::uint32_t max_tile_height = env_as<int>("PYBUDA_RIBBON_MAX_TILE_HEIGHT", 200);

    // pick smallest legal ribbon
    bool minimize_ribbon = !env_as<bool>("PYBUDA_RIBBON_MAXIMIZE");

    bool skip_streaming = env_as<bool>("PYBUDA_RIBBON_SKIP_STREAMING");

    // override the max ribon size
    std::uint32_t max_ribbon_size = std::min(env_as<int>("PYBUDA_RIBBON_MAX_HEIGHT", device_rows), (int)device_rows);

    // Try to find a ribbon size that work for all ops in the ribbon
    std::unordered_set<std::uint32_t> candidates;
    std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>>
        valid_map;  // map of ribbons that are valid for each op
    for (std::uint32_t i = 1; i <= max_ribbon_size; i++) candidates.insert(i);

    log_trace(LogBalancer, "Starting ribbon size search for {} ops", end_index - start_index);
    for (std::uint32_t i = start_index; i < end_index; i++)
    {
        graphlib::BudaOpNode *op = graph->get_node_by_name(scheduled_ops[i])->as<graphlib::BudaOpNode>();
        log_trace(LogBalancer, "  Checking op {}", op->name());
        for (auto grid : graph_solver.at(op))
        {
            if (skip_streaming && (grid.t_stream_factor.r > 1))
                continue;

            log_trace(
                LogBalancer,
                "    - Grid: {}, t-stream: {}, block shape rt: {}",
                grid.grid_shape,
                grid.t_stream_factor,
                grid.block_shape().rt());
            if (prologue_ok(grid) && ((std::uint32_t)grid.block_shape().rt() >= min_tile_height) &&
                ((std::uint32_t)grid.block_shape().rt() <= max_tile_height))
            {
                log_trace(LogBalancer, "     - valid");
                valid_map[i].insert(grid.grid_shape.r);
            }
        }

        std::unordered_set<std::uint32_t> to_erase;
        for (auto it : candidates)
            if (valid_map[i].count(it) == 0)
                to_erase.insert(it);
        for (auto it : to_erase) candidates.erase(it);

        if (candidates.empty())
            break;  // stop searching, we don't have anything
    }

    // If there are candidates available, pick smallest / largest
    if (!candidates.empty())
    {
        return minimize_ribbon ? *std::min_element(candidates.begin(), candidates.end())
                               : *std::max_element(candidates.begin(), candidates.end());
    }

    // std::cout << "No valid ribbon size found, looking for partials" << std::endl;
    //  TT_THROW("No valid ribbon size found"); // TODO: handle this case... right now, it hangs

    // No candidates available for everything. Need to find the best choice, so that everyone at least fits under
    // some ribbon size and nobody goes beyond it
    std::vector<std::uint32_t> partial_candidates;
    if (minimize_ribbon)
        for (std::uint32_t i = 1; i <= max_ribbon_size; i++) partial_candidates.push_back(i);
    else
        for (std::uint32_t i = max_ribbon_size; i > 0; i--) partial_candidates.push_back(i);

    // For each candidate, find if all ops would fit in something equal or smaller, and then take that.
    for (auto candidate : partial_candidates)
    {
        // At least one op should fit on this ribbon, otherwise it's not a real choice
        bool one_match = false;
        for (std::uint32_t i = start_index; i < end_index; i++)
        {
            if (valid_map[i].count(candidate) > 0)
            {
                one_match = true;
                break;
            }
        }

        if (!one_match)
            continue;

        bool all_ok = true;
        for (std::uint32_t i = start_index; i < end_index; i++)
        {
            bool ok = false;
            for (std::uint32_t ribbon = 1; ribbon <= candidate; ribbon++)
            {
                if (valid_map[i].count(ribbon) > 0)
                {
                    ok = true;
                    break;
                }
            }
            if (!ok)
            {
                all_ok = false;
                break;
            }
        }

        if (all_ok)
            return candidate;
    }

    return 1;  // we couldn't find anything... so we'll just have to pick smallest legal values
}

// Return the index of the next op that should change the ribbon size. It's either matmul or sparse
// matmul feeding it. Size of the array returned if no more changes found.
// In case we are recomputing within current ribbon, pass in current_matmul_dim_r from previous computation.
//
std::pair<std::uint32_t, std::uint32_t> get_next_ribbon_change_op(
    const graphlib::Graph *graph,
    std::uint32_t current_index,
    const std::vector<std::string> &scheduled_ops,
    std::uint32_t current_matmul_dim_r)
{
    for (std::uint32_t i = current_index; i < scheduled_ops.size(); i++)
    {
        graphlib::Node *node = graph->get_node_by_name(scheduled_ops[i]);

        if (node->node_type() != NodeType::kBudaOp)
            continue;

        const graphlib::BudaOpNode *op = node->as<graphlib::BudaOpNode>();
        if (!is_matmul(op))
            continue;

        std::uint32_t dim_r = op->shape().rt();
        if (current_matmul_dim_r == 0)
        {
            current_matmul_dim_r = dim_r;
            continue;
        }

        if (dim_r == current_matmul_dim_r)
            continue;

        // Matmul with different row shape. Let's see if there's a sparse matmul feeding it
        for (Node *operand : graph->data_operands(op))
        {
            // Skip through buffering queue.
            //
            if (operand->node_type() == NodeType::kQueue)
            {
                if (operand->as<graphlib::QueueNode>()->is_buffering())
                {
                    auto data_operands = graph->data_operands(operand);
                    TT_ASSERT(data_operands.size() == 1);
                    operand = data_operands.back();
                }
            }

            if (operand->node_type() != NodeType::kBudaOp)
                continue;

            if (operand->as<graphlib::BudaOpNode>()->is_sparse_matmul())
            {
                // Find the index. Should be a quick search back.
                for (int sparse_i = i - 1; sparse_i >= 0; sparse_i--)
                {
                    if (operand->name() == scheduled_ops[sparse_i])
                    {
                        return std::make_pair(sparse_i, current_matmul_dim_r);
                    }
                }
            }

            // No sparse matmul, switch on matmul itself
            return std::make_pair(i, current_matmul_dim_r);
        }
    }

    // No change until the end
    return std::make_pair(scheduled_ops.size(), current_matmul_dim_r);
}

// Can we bind sparse matmul and matmul and place them atomically together in a single block.
//
bool can_bind_sparse_dense_matmul_pair(
    const Graph *graph,
    const graphlib::BudaOpNode *sparse_op,
    OpModel const &sparse_op_model,
    const graphlib::BudaOpNode *dense_op,
    OpModel const &dense_op_model,
    placer::InteractivePlacer const &interactive_placer,
    bool allow_transpose)
{
    return sparse_op and sparse_op->is_sparse_matmul() and dense_op and
           dense_op->should_pair_with_sparse(sparse_op, graph) and
           sparse_op_model.grid_shape.r == dense_op_model.grid_shape.r and
           interactive_placer.can_fit_on_single_epoch(
               sparse_op_model.grid_shape.r,
               sparse_op_model.grid_shape.c + dense_op_model.grid_shape.c,
               allow_transpose) and
           dense_op == graph->data_users(sparse_op)[0];
}

// Test whether provided value is within specified range from the target execution cycles.
//
bool close_to_target(std::uint32_t test, std::uint32_t target) { return (test < target) && (test > target * 0.8); }

int get_limiter_cycles(
    const OpModel &op_model,
    const Graph *graph,
    const DeviceConfig &device_config,
    const int dram_access_core_count,
    const std::unordered_set<const tt::graphlib::Node *> *current_epoch_nodes,
    bool invalidate_cached)
{
    const float inefficency_divider = 2.0;
    const float subchannel_oversub_coeff = 1.5;
    TT_ASSERT(op_model.buda_op_node);
    int kernel_cycles = op_model.get_execution_cycles(device_config.arch_name, false, invalidate_cached);

    if (env_as<bool>("PYBUDA_BALANCER_LEGACY_CYCLES_CALC", false))
    {
        return kernel_cycles;
    }

    std::vector<Edge> data_operands = graph->operand_data_edges(op_model.buda_op_node);
    std::vector<Edge> data_users = graph->user_data_edges(op_model.buda_op_node);

    // Use half of theoretical max for better average estimate for now.
    //
    float noc_bw = static_cast<float>(device_config.get_noc_bandwidth_bytes_per_cycle()) / inefficency_divider;
    float dram_bw_divider = std::max(
        inefficency_divider,
        std::ceil(
            dram_access_core_count / (device_config.get_dram_num_channels() * device_config.get_dram_num_subchannels() /
                                      subchannel_oversub_coeff)));

    // API is currently returning wrong value for WH
    // tenstorrent/budabackend#2423
    //
    float dram_bw = device_config.is_wormhole()
                        ? 20.4 / dram_bw_divider
                        : static_cast<float>(device_config.get_dram_bandwidth_bytes_per_cycle()) / dram_bw_divider;
    int memory_read_cycles = 0;

    for (const Edge &edge : data_operands)
    {
        bool producer_is_queue = graph->node_by_id(edge.producer_node_id)->node_type() == NodeType::kQueue ||
                                 graph->node_by_id(edge.producer_node_id)->node_type() == NodeType::kInput;

        if (producer_is_queue and !op_model.parameter_buffers[edge.consumer_input_port_id])
        {
            memory_read_cycles = std::max(
                memory_read_cycles,
                static_cast<int>(op_model.input_buffers[edge.consumer_input_port_id].total_size_bytes() / dram_bw));
        }
        else
        {
            memory_read_cycles = std::max(
                memory_read_cycles,
                static_cast<int>(op_model.input_buffers[edge.consumer_input_port_id].total_size_bytes() / noc_bw));
        }
    }

    int memory_write_cycles = 0;

    for (const Edge &edge : data_users)
    {
        const tt::graphlib::Node *user_node = graph->node_by_id(edge.consumer_node_id);
        bool consumer_is_queue = user_node->node_type() == NodeType::kQueue ||
                                 user_node->node_type() == NodeType::kOutput ||
                                 (nullptr != current_epoch_nodes && current_epoch_nodes->count(user_node) == 0);

        if (consumer_is_queue)
        {
            memory_write_cycles = std::max(
                memory_write_cycles,
                static_cast<int>(op_model.output_buffers[edge.producer_output_port_id].total_size_bytes() / dram_bw));
        }
        else
        {
            memory_write_cycles = std::max(
                memory_write_cycles,
                static_cast<int>(op_model.output_buffers[edge.producer_output_port_id].total_size_bytes() / noc_bw));
        }
    }

    return std::max({kernel_cycles, memory_read_cycles, memory_write_cycles});
}

bool is_output_write_to_dram_over_target(
    const OpModel &op_model, const DeviceConfig &device_config, const int target_exec_cycles)
{
    int memory_write_cycles = 0;

    // API is currently returning wrong value for WH
    // tenstorrent/budabackend#2423
    //
    float dram_bw = device_config.is_wormhole()
                        ? 20.4 / 2
                        : static_cast<float>(device_config.get_dram_bandwidth_bytes_per_cycle()) / 2;

    for (const BufferModel &output_buffer : op_model.output_buffers)
    {
        memory_write_cycles =
            std::max(memory_write_cycles, static_cast<int>(output_buffer.total_size_bytes() / dram_bw));
    }

    return memory_write_cycles > target_exec_cycles;
}

// Depending on insertion instructions insert NOPs or queues directly into GraphSolver.
//
bool buffer_graph(
    Graph *graph,
    tt::ordered_map<InsInstructionUniqueId, std::shared_ptr<InsertionInstruction>, InsInstructionUniqueIdHash> &inst,
    legalizer::GraphSolver &graph_solver)
{
    vector<legalizer::BufferInfo> buffer_info;
    vector<graphlib::Edge> edges_to_cut;
    bool graph_modified = false;

    for (auto it : inst)
    {
        if (it.second->instr_type == InsructionType::NopInstruction)
        {
            NopInsertionInstruction *nopInsertInst = static_cast<NopInsertionInstruction *>(it.second.get());
            for (graphlib::Edge edge : graph->get_edges(
                     graph->get_node_by_name(nopInsertInst->src), graph->get_node_by_name(nopInsertInst->dest)))
            {
                if (edge.edge_type != graphlib::EdgeType::kData)
                {
                    continue;
                }

                buffer_info.emplace_back(edge, nopInsertInst->nop_count, nopInsertInst->hoist_tms);
            }
        }
        else if (it.second->instr_type == InsructionType::QueueInstruction)
        {
            QueueInsertionInstruction *qInsertInst = static_cast<QueueInsertionInstruction *>(it.second.get());
            std::function<bool(Edge)> edge_filter = [qInsertInst](Edge edge)
            { return edge.consumer_input_port_id == qInsertInst->input_id.value(); };
            std::vector<tt::graphlib::Edge> operand_edges =
                graph->operand_data_edges(graph->get_node_by_name(qInsertInst->dest), edge_filter);
            TT_ASSERT(operand_edges.size() == 1, "Expected exactly one operand edge per queue instruction!");
            edges_to_cut.push_back(operand_edges[0]);
        }
        else
        {
            TT_THROW("Unexpected insertion instruction type!");
        }
    }

    if (buffer_info.size() > 0)
    {
        auto result = graph_solver.buffer(buffer_info);
        graph_modified = true;
        TT_ASSERT(result.size() > 0, "Expected buffering to occur but nothing was buffered!");
    }

    if (edges_to_cut.size() > 0)
    {
        graph_solver.cut(edges_to_cut);
    }

    return graph_modified;
}

}  // namespace tt::balancer
