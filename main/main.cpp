#include "grpc/grpc_client.h"
#include "grpc/update_flow_control.h"
#include "env/maze_env.h"
#include "config/config_loader.h"
#include "viz/viz_recorder.h"
#include "log/logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <unistd.h>

namespace common = rl::common::v1;

namespace {

constexpr const char* kDefaultConfigPath = "configs/client_config.yaml";
constexpr std::uint32_t kSessionProtocolVersion = 3;
constexpr const char* kObservationSchemaId = "maze.observation.v3";
constexpr std::uint32_t kObservationSchemaVersion = 1;
constexpr const char* kObservationSchemaDigest =
    "7cee41136020f3ffc8c6ae799f630d55d0588c6a99ab7f717eac3b3d08aa18b4";
constexpr const char* kActionSchemaId = "maze.action.v1";
constexpr std::uint32_t kActionSchemaVersion = 1;
constexpr const char* kActionSchemaDigest =
    "ce84c564e128f98adcc48fd420ac0df5acea61774a25de8705b602464009cfd8";
constexpr const char* kPolicyDistributionSchema = "categorical.logits.v1";

const char* kActionNames[9] = {
    "不动", "上", "右上", "右", "右下", "下", "左下", "左", "左上"
};

std::atomic<bool> g_stop_requested{false};

void HandleSignal(int) { g_stop_requested.store(true); }

bool IsLowerSha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool IsDigest(const common::ContentDigest& digest) {
    return digest.algorithm() == common::DIGEST_ALGORITHM_SHA256 &&
           IsLowerSha256(digest.hex());
}

void SetSha256(common::ContentDigest* digest, const std::string& hex) {
    digest->set_algorithm(common::DIGEST_ALGORITHM_SHA256);
    digest->set_hex(hex);
}

void FillSchema(common::SchemaIdentity* schema,
                const char* id,
                std::uint32_t version,
                const char* digest) {
    schema->set_schema_id(id);
    schema->set_schema_version(version);
    SetSha256(schema->mutable_canonical_digest(), digest);
}

bool SameSchema(const common::SchemaIdentity& schema,
                const char* id,
                std::uint32_t version,
                const char* digest) {
    return schema.schema_id() == id &&
           schema.schema_version() == version &&
           schema.canonical_digest().algorithm() ==
               common::DIGEST_ALGORITHM_SHA256 &&
           schema.canonical_digest().hex() == digest;
}

bool ValidTaskIdentity(const maze::TaskIdentity& identity) {
    return !identity.task_contract_id().empty() &&
           !identity.task_id().empty() &&
           identity.task_revision() > 0 &&
           IsDigest(identity.task_config_digest()) &&
           !identity.fixed_map_id().empty() &&
           IsDigest(identity.fixed_map_digest());
}

bool SameTaskIdentity(const maze::TaskIdentity& lhs,
                      const maze::TaskIdentity& rhs) {
    return lhs.SerializeAsString() == rhs.SerializeAsString();
}

bool SamePolicy(const maze::BehaviorPolicyBinding& lhs,
                const maze::BehaviorPolicyBinding& rhs) {
    return lhs.SerializeAsString() == rhs.SerializeAsString();
}

bool ValidPolicy(const maze::BehaviorPolicyBinding& policy) {
    return !policy.model_lineage_id().empty() &&
           IsDigest(policy.model_artifact_digest()) &&
           IsDigest(policy.model_manifest_digest()) &&
           policy.distribution_schema_id() == kPolicyDistributionSchema &&
           IsDigest(policy.policy_spec_digest());
}

bool LifecycleAccepted(const maze::LifecycleReply& reply) {
    return reply.ret_code() == 0 &&
           (reply.result() == maze::LIFECYCLE_RESULT_APPLIED ||
            reply.result() == maze::LIFECYCLE_RESULT_ALREADY_APPLIED) &&
           reply.error_code() == maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED;
}

const char* WorkloadName(maze::WorkloadMode workload) {
    switch (workload) {
        case maze::WORKLOAD_MODE_TRAINING: return "training";
        case maze::WORKLOAD_MODE_INFERENCE_SMOKE: return "local-test";
        case maze::WORKLOAD_MODE_MODEL_EVALUATION: return "model-evaluation";
        case maze::WORKLOAD_MODE_MAP_VALIDATION: return "map-validation";
        default: return "";
    }
}

const char* ReplayPolicyName(maze::ReplayPolicy policy) {
    switch (policy) {
        case maze::REPLAY_POLICY_DISABLED: return "disabled";
        case maze::REPLAY_POLICY_RECORD_AND_SERVE: return "record-and-serve";
        default: return "";
    }
}

const char* EpisodeModeName(maze::EpisodeMode mode) {
    switch (mode) {
        case maze::EPISODE_MODE_TRAINING: return "training";
        case maze::EPISODE_MODE_EVALUATION_ARGMAX: return "evaluation-argmax";
        case maze::EPISODE_MODE_EVALUATION_STOCHASTIC:
            return "evaluation-stochastic";
        default: return "";
    }
}

int CurriculumMultiplier(maze::CurriculumStage stage) {
    switch (stage) {
        case maze::CURRICULUM_STAGE_8X: return 8;
        case maze::CURRICULUM_STAGE_4X: return 4;
        case maze::CURRICULUM_STAGE_2X: return 2;
        default: return 0;
    }
}

maze::MazeTerminationReason ToProtoTerminationReason(
    AgentTerminationReason reason) {
    switch (reason) {
        case AgentTerminationReason::GoalReached:
            return maze::MAZE_TERMINATION_REASON_GOAL_REACHED;
        case AgentTerminationReason::TimeLimit:
            return maze::MAZE_TERMINATION_REASON_TIME_LIMIT;
        case AgentTerminationReason::Active:
        default:
            return maze::MAZE_TERMINATION_REASON_ACTIVE;
    }
}

struct LifecycleCursor {
    maze::TaskIdentity task;
    std::string session_id;
    std::string episode_id;
    std::string evaluation_id;
    std::uint64_t lifecycle_epoch = 0;
    std::uint64_t next_sequence = 1;
    maze::TaskState task_state = maze::TASK_STATE_UNSPECIFIED;
    maze::SessionState session_state = maze::SESSION_STATE_UNSPECIFIED;
    maze::EpisodeState episode_state = maze::EPISODE_STATE_UNSPECIFIED;
    maze::EvaluationState evaluation_state =
        maze::EVALUATION_STATE_UNSPECIFIED;
};

void UpdateCursor(LifecycleCursor& cursor,
                  const maze::LifecycleReply& reply) {
    cursor.task_state = reply.task_state();
    cursor.session_state = reply.session_state();
    cursor.episode_state = reply.episode_state();
    cursor.evaluation_state = reply.evaluation_state();
}

void FillCommand(LifecycleCursor& cursor,
                 const std::string& operation,
                 maze::LifecycleCommand* command) {
    command->mutable_task()->CopyFrom(cursor.task);
    command->set_session_id(cursor.session_id);
    command->set_episode_id(cursor.episode_id);
    command->set_evaluation_id(cursor.evaluation_id);
    command->set_lifecycle_epoch(cursor.lifecycle_epoch);
    command->set_command_sequence(cursor.next_sequence);
    command->set_idempotency_key(
        cursor.session_id + ":" + std::to_string(cursor.lifecycle_epoch) +
        ":" + std::to_string(cursor.next_sequence) + ":" + operation);
    command->set_expected_task_state(cursor.task_state);
    command->set_expected_session_state(cursor.session_state);
    command->set_expected_episode_state(cursor.episode_state);
    command->set_expected_evaluation_state(cursor.evaluation_state);
    ++cursor.next_sequence;
}

bool AcceptCommandReply(LifecycleCursor& cursor,
                        const maze::LifecycleReply& reply) {
    const std::uint64_t expected_sequence = cursor.next_sequence - 1;
    if (!LifecycleAccepted(reply) ||
        reply.applied_sequence() != expected_sequence ||
        reply.task_state() == maze::TASK_STATE_UNSPECIFIED ||
        reply.session_state() == maze::SESSION_STATE_UNSPECIFIED ||
        reply.evaluation_state() == maze::EVALUATION_STATE_UNSPECIFIED) {
        return false;
    }
    UpdateCursor(cursor, reply);
    return true;
}

bool PublishSessionPolicy(const std::string& workload,
                          const std::string& replay_policy,
                          const maze::BehaviorPolicyBinding& policy) {
    const char* raw_path = std::getenv("RL_SESSION_POLICY_PATH");
    if (raw_path == nullptr || raw_path[0] == '\0') return true;

    namespace fs = std::filesystem;
    const fs::path path(raw_path);
    std::error_code error;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), error);
        if (error) return false;
    }
    const fs::path temporary =
        path.string() + ".tmp." + std::to_string(::getpid());
    const char* behavior_policy_scope = "none";
    const char* model_identity_role = "not-applicable";
    if (workload == "training") {
        behavior_policy_scope = "training-fragment";
        model_identity_role = "episode-start-snapshot";
    } else if (workload == "local-test" ||
               workload == "model-evaluation") {
        behavior_policy_scope = "evaluation-episode";
        model_identity_role = "episode-pinned";
    }
    {
        std::ofstream output(temporary);
        if (!output) return false;
        output << "workload=" << workload << "\n"
               << "replay_policy=" << replay_policy << "\n"
               << "behavior_policy_scope=" << behavior_policy_scope << "\n"
               << "model_identity_role=" << model_identity_role << "\n"
               << "model_lineage_id=" << policy.model_lineage_id() << "\n"
               << "model_version=" << policy.model_version() << "\n"
               << "model_artifact_digest="
               << policy.model_artifact_digest().hex() << "\n"
               << "model_manifest_digest="
               << policy.model_manifest_digest().hex() << "\n";
        output.flush();
        if (!output) {
            fs::remove(temporary, error);
            return false;
        }
    }
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

std::string ResolveTaskMapFile(const std::string& registry_dir,
                               const std::string& map_id) {
    if (registry_dir.empty() || map_id.empty()) return "";
    for (char character : map_id) {
        const bool valid =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '_' || character == '-';
        if (!valid) return "";
    }
    std::error_code error;
    const std::filesystem::path root =
        std::filesystem::weakly_canonical(registry_dir, error);
    if (error || !std::filesystem::is_directory(root)) return "";
    const std::filesystem::path candidate =
        std::filesystem::weakly_canonical(root / (map_id + ".json"), error);
    if (error || candidate.parent_path() != root ||
        !std::filesystem::is_regular_file(candidate)) {
        return "";
    }
    return candidate.string();
}

std::string BuildVizJson(const MazeEnv& env,
                         int frame_id,
                         int episode_number,
                         const std::vector<int>& actions) {
    std::ostringstream output;
    output << "{\"type\":\"frame_update\",\"frame_id\":" << frame_id
           << ",\"episode_id\":" << episode_number
           << ",\"map_id\":\"" << env.GetMapId() << "\",\"agents\":[";
    for (int index = 0; index < env.GetAgentNum(); ++index) {
        const auto& agent = env.GetAgent(index);
        if (index > 0) output << ',';
        output << "{\"id\":" << agent.id
               << ",\"x\":" << env.GetWorldX(agent.grid_x)
               << ",\"y\":" << env.GetWorldY(agent.grid_y)
               << ",\"done\":" << (agent.done ? "true" : "false")
               << ",\"action_id\":"
               << (index < static_cast<int>(actions.size()) ? actions[index] : 0)
               << '}';
    }
    output << "]}";
    return output.str();
}

}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    Logger::Instance().Init("log");
    Logger::Instance().SetConsoleLevel(LogLevel::INFO);
    Logger::Instance().SetFileLevel(LogLevel::DEBUG);

    const char* config_path = argc > 1 ? argv[1] : kDefaultConfigPath;
    ClientConfig config;
    if (!LoadClientConfig(config_path, config)) {
        Logger::Instance().Close();
        return 1;
    }

    GrpcClient client;
    if (!client.Connect(config.network.server_host,
                        config.network.server_port)) {
        Logger::Instance().Close();
        return 1;
    }

    maze::OpenSessionReq open_request;
    auto* client_identity = open_request.mutable_client();
    client_identity->set_component("maze-client");
    client_identity->set_instance_id(config.run.client_instance_id);
    client_identity->set_lifecycle_epoch(1);
    open_request.set_environment_instance_id(
        config.run.environment_instance_id);
    open_request.add_supported_session_protocol_versions(
        kSessionProtocolVersion);
    FillSchema(open_request.add_supported_observation_schemas(),
               kObservationSchemaId, kObservationSchemaVersion,
               kObservationSchemaDigest);
    FillSchema(open_request.add_supported_action_schemas(),
               kActionSchemaId, kActionSchemaVersion,
               kActionSchemaDigest);
    open_request.set_idempotency_key(
        config.run.client_instance_id + ":" + std::to_string(::getpid()) +
        ":open-session");

    maze::OpenSessionRsp open_response;
    if (!client.OpenSession(open_request, open_response) ||
        !LifecycleAccepted(open_response.lifecycle()) ||
        open_response.session_protocol_version() != kSessionProtocolVersion ||
        open_response.session_id().empty() ||
        open_response.lifecycle_epoch() == 0 ||
        open_response.aiserver().component() != "rl-aiserver" ||
        open_response.aiserver().instance_id().empty() ||
        !ValidTaskIdentity(open_response.task_spec().identity()) ||
        open_response.task_spec().agent_count() == 0 ||
        open_response.task_spec().fixed_map_id() !=
            open_response.task_spec().identity().fixed_map_id() ||
        open_response.task_spec().expected_map_digest().SerializeAsString() !=
            open_response.task_spec().identity().fixed_map_digest().SerializeAsString() ||
        !SameSchema(open_response.task_spec().observation_schema(),
                    kObservationSchemaId, kObservationSchemaVersion,
                    kObservationSchemaDigest) ||
        !SameSchema(open_response.task_spec().action_schema(),
                    kActionSchemaId, kActionSchemaVersion,
                    kActionSchemaDigest) ||
        open_response.task_spec().action_rule_id() !=
            "maze.action.9-way.no-corner-cut.v1") {
        LOG_ERROR("Main", "OpenSession 返回了无效的 0.10.0 任务身份");
        Logger::Instance().Close();
        return 1;
    }

    const std::string workload = WorkloadName(open_response.workload_mode());
    const std::string replay_policy =
        ReplayPolicyName(open_response.replay_policy());
    const bool replay_expected =
        open_response.workload_mode() == maze::WORKLOAD_MODE_INFERENCE_SMOKE ||
        open_response.workload_mode() == maze::WORKLOAD_MODE_MODEL_EVALUATION;
    if (workload.empty() || replay_policy.empty() ||
        replay_expected !=
            (open_response.replay_policy() ==
             maze::REPLAY_POLICY_RECORD_AND_SERVE)) {
        LOG_ERROR("Main", "OpenSession workload 与 Replay policy 无效");
        Logger::Instance().Close();
        return 1;
    }

    LifecycleCursor cursor;
    cursor.task.CopyFrom(open_response.task_spec().identity());
    cursor.session_id = open_response.session_id();
    cursor.lifecycle_epoch = open_response.lifecycle_epoch();
    UpdateCursor(cursor, open_response.lifecycle());
    config.run.agent_num =
        static_cast<int>(open_response.task_spec().agent_count());
    config.run.workload = workload;
    config.viz.recording_enabled = replay_expected;

    const std::string map_file = ResolveTaskMapFile(
        config.env.map_registry_dir,
        open_response.task_spec().fixed_map_id());
    if (map_file.empty()) {
        LOG_ERROR("Main", "TaskSpec 地图未在 registry 精确命中");
        Logger::Instance().Close();
        return 1;
    }
    config.env.map_file = map_file;
    MazeEnv environment;
    if (!environment.Init(config) ||
        environment.GetMapId() != open_response.task_spec().fixed_map_id() ||
        environment.GetMapChecksum() !=
            open_response.task_spec().expected_map_digest().hex()) {
        LOG_ERROR("Main", "Client 地图 canonical 身份不匹配");
        Logger::Instance().Close();
        return 1;
    }

    maze::InitReq init_request;
    FillCommand(cursor, "init", init_request.mutable_command());
    auto* map = init_request.mutable_map();
    map->set_map_id(environment.GetMapId());
    map->set_format_version(environment.GetMapFormatVersion());
    map->set_grid_columns(environment.GetGridCols());
    map->set_grid_rows(environment.GetGridRows());
    map->set_grid_size_microunits(environment.GetGridSizeMicrounits());
    map->set_start_grid_x(environment.GetStartGridX());
    map->set_start_grid_y(environment.GetStartGridY());
    map->set_goal_grid_x(environment.GetGoalGridX());
    map->set_goal_grid_y(environment.GetGoalGridY());
    map->set_blocked_bitmap(environment.GetBlockedBitmap());
    SetSha256(map->mutable_canonical_digest(), environment.GetMapChecksum());
    map->set_shortest_action_steps(environment.GetShortestActionSteps());
    map->set_action_rule_id(environment.GetActionRuleId());

    maze::InitRsp init_response;
    if (!client.Init(init_request, init_response) ||
        !AcceptCommandReply(cursor, init_response.lifecycle()) ||
        init_response.accepted_map_digest().SerializeAsString() !=
            map->canonical_digest().SerializeAsString() ||
        init_response.verified_shortest_action_steps() !=
            environment.GetShortestActionSteps()) {
        LOG_ERROR("Main", "Init 地图验证失败");
        Logger::Instance().Close();
        return 1;
    }

    if (open_response.workload_mode() ==
        maze::WORKLOAD_MODE_MAP_VALIDATION) {
        const maze::BehaviorPolicyBinding no_behavior_policy;
        bool validation_failed =
            !PublishSessionPolicy(workload, replay_policy,
                                  no_behavior_policy);
        maze::CloseSessionReq close_request;
        FillCommand(cursor, "close-map-validation",
                    close_request.mutable_command());
        maze::CloseSessionRsp close_response;
        if (!client.CloseSession(close_request, close_response) ||
            !AcceptCommandReply(cursor, close_response.lifecycle())) {
            LOG_ERROR("Main", "Map validation Session 未正常关闭");
            validation_failed = true;
        }
        client.Disconnect();
        Logger::Instance().Close();
        return validation_failed ? 1 : 0;
    }

    bool chain_failed = false;
    bool session_policy_published = false;
    bool episode_active = false;
    int episode_number = 0;
    VizRecorder recorder;

    while (!g_stop_requested.load()) {
        cursor.episode_id.clear();
        cursor.evaluation_id.clear();
        maze::BeginEpisodeReq begin_request;
        FillCommand(cursor, "begin-episode", begin_request.mutable_command());
        maze::BeginEpisodeRsp begin_response;
        if (!client.BeginEpisode(begin_request, begin_response) ||
            !AcceptCommandReply(cursor, begin_response.lifecycle())) {
            LOG_ERROR("Main", "BeginEpisode 生命周期被拒绝");
            chain_failed = true;
            break;
        }
        const auto& assignment = begin_response.assignment();
        if (!assignment.continue_task()) {
            LOG_INFO("Main", "AIServer 已结束固定地图任务");
            break;
        }
        const int multiplier = CurriculumMultiplier(
            assignment.curriculum_stage());
        const std::string mode = EpisodeModeName(assignment.mode());
        const bool training_assignment =
            assignment.mode() == maze::EPISODE_MODE_TRAINING;
        const bool evaluation_assignment =
            assignment.mode() == maze::EPISODE_MODE_EVALUATION_ARGMAX ||
            assignment.mode() == maze::EPISODE_MODE_EVALUATION_STOCHASTIC;
        if (assignment.episode_id().empty() ||
            !SameTaskIdentity(assignment.task(), cursor.task) ||
            multiplier == 0 || mode.empty() ||
            assignment.max_steps() !=
                static_cast<std::uint32_t>(
                    environment.GetShortestActionSteps() * multiplier) ||
            !ValidPolicy(assignment.behavior_policy()) ||
            assignment.collect_training_samples() != training_assignment ||
            (!training_assignment && !evaluation_assignment)) {
            LOG_ERROR("Main", "EpisodeAssignment 身份、模式或课程无效");
            chain_failed = true;
            break;
        }
        if (evaluation_assignment) {
            if (assignment.evaluation().evaluation_id().empty() ||
                assignment.evaluation().training_sample_emission_allowed() ||
                !SamePolicy(assignment.evaluation().pinned_policy(),
                            assignment.behavior_policy())) {
                LOG_ERROR("Main", "Evaluation 未固定模型或错误允许样本发送");
                chain_failed = true;
                break;
            }
            cursor.evaluation_id = assignment.evaluation().evaluation_id();
        }
        cursor.episode_id = assignment.episode_id();
        episode_active = true;

        if (!session_policy_published) {
            if (!PublishSessionPolicy(workload, replay_policy,
                                      assignment.behavior_policy())) {
                LOG_ERROR("Main", "无法发布已协商 Session policy");
                chain_failed = true;
                break;
            }
            session_policy_published = true;
        }

        environment.SetMaxSteps(static_cast<int>(assignment.max_steps()));
        environment.Reset();
        std::vector<int> last_actions(config.run.agent_num, 0);
        if (config.viz.recording_enabled &&
            !recorder.Begin(config.viz.output_dir, episode_number,
                            environment.GetMapId(),
                            environment.GetMapFilePath())) {
            LOG_ERROR("Main", "Replay 帧记录器启动失败");
            chain_failed = true;
            break;
        }
        if (training_assignment) {
            LOG_INFO(
                "Main",
                "Episode %s 开始 mode=%s max_steps=%u start_model=v%llu "
                "policy_scope=fragment",
                cursor.episode_id.c_str(), mode.c_str(),
                assignment.max_steps(),
                static_cast<unsigned long long>(
                    assignment.behavior_policy().model_version()));
        } else {
            LOG_INFO(
                "Main",
                "Episode %s 开始 mode=%s max_steps=%u pinned_model=v%llu "
                "policy_scope=episode",
                cursor.episode_id.c_str(), mode.c_str(),
                assignment.max_steps(),
                static_cast<unsigned long long>(
                    assignment.behavior_policy().model_version()));
        }

        while (!g_stop_requested.load()) {
            const bool terminal_report = environment.AllDone();
            maze::UpdateReq update_request;
            FillCommand(cursor, "update", update_request.mutable_command());
            update_request.set_frame_id(environment.GetFrameId());
            for (int index = 0; index < environment.GetAgentNum(); ++index) {
                const auto& agent = environment.GetAgent(index);
                auto* state = update_request.add_agents();
                state->set_agent_id(agent.id);
                state->mutable_position()->set_x(
                    static_cast<float>(agent.grid_x));
                state->mutable_position()->set_y(
                    static_cast<float>(agent.grid_y));
                state->set_is_done(agent.done);
                state->set_termination_reason(
                    ToProtoTerminationReason(agent.termination_reason));
                state->set_last_move_blocked(agent.last_move_blocked);
            }

            maze::UpdateRsp update_response;
            bool update_applied = false;
            while (!g_stop_requested.load()) {
                update_response.Clear();
                if (!client.Update(update_request, update_response)) {
                    LOG_ERROR("Main", "Update RPC 失败");
                    chain_failed = true;
                    break;
                }
                if (update_response.environment_control() ==
                    maze::ENVIRONMENT_CONTROL_WAIT_FOR_TRAINING_CAPACITY) {
                    if (!maze_client::IsTrainingCapacityWait(
                            update_response, cursor.next_sequence,
                            cursor.task_state, cursor.session_state,
                            cursor.episode_state,
                            cursor.evaluation_state)) {
                        LOG_ERROR("Main", "Update WAIT 响应无效");
                        chain_failed = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        update_response.retry_after_ms()));
                    continue;
                }
                if (update_response.environment_control() !=
                        maze::ENVIRONMENT_CONTROL_ADVANCE ||
                    !AcceptCommandReply(cursor,
                                        update_response.lifecycle())) {
                    const auto& reply = update_response.lifecycle();
                    LOG_ERROR(
                        "Main",
                        "Update 生命周期响应无效: frame=%llu control=%d "
                        "ret=%d result=%d error=%d applied=%llu "
                        "task=%d session=%d episode=%d evaluation=%d "
                        "actions=%d message=%s",
                        static_cast<unsigned long long>(
                            update_request.frame_id()),
                        static_cast<int>(
                            update_response.environment_control()),
                        reply.ret_code(), static_cast<int>(reply.result()),
                        static_cast<int>(reply.error_code()),
                        static_cast<unsigned long long>(
                            reply.applied_sequence()),
                        static_cast<int>(reply.task_state()),
                        static_cast<int>(reply.session_state()),
                        static_cast<int>(reply.episode_state()),
                        static_cast<int>(reply.evaluation_state()),
                        update_response.actions_size(),
                        reply.message().c_str());
                    chain_failed = true;
                } else {
                    update_applied = true;
                }
                break;
            }
            if (g_stop_requested.load() && !update_applied) {
                --cursor.next_sequence;
            }
            if (chain_failed || g_stop_requested.load()) break;
            if (update_response.task_stop_requested()) {
                if (update_response.task_stop_reason() !=
                        maze::MAZE_TERMINATION_REASON_TASK_STOP ||
                    update_response.actions_size() != 0) {
                    LOG_ERROR("Main", "AIServer 任务停止响应无效");
                    chain_failed = true;
                    break;
                }
                maze::AbortEpisodeReq abort_request;
                FillCommand(cursor, "abort-task-stop",
                            abort_request.mutable_command());
                abort_request.set_reason(
                    maze::MAZE_TERMINATION_REASON_TASK_STOP);
                abort_request.set_message("AIServer requested task stop");
                maze::AbortEpisodeRsp abort_response;
                if (!client.AbortEpisode(abort_request, abort_response) ||
                    !AcceptCommandReply(cursor,
                                        abort_response.lifecycle())) {
                    chain_failed = true;
                }
                episode_active = false;
                break;
            }
            if (terminal_report) break;
            if (update_response.actions_size() != environment.GetAgentNum()) {
                LOG_ERROR("Main", "AIServer 返回动作数量不匹配");
                chain_failed = true;
                break;
            }

            std::vector<bool> seen(
                static_cast<std::size_t>(environment.GetAgentNum()), false);
            for (const auto& action : update_response.actions()) {
                if (action.agent_id() >=
                        static_cast<std::uint32_t>(environment.GetAgentNum()) ||
                    action.action_id() < 0 || action.action_id() > 8 ||
                    seen[action.agent_id()]) {
                    LOG_ERROR("Main", "AIServer 返回动作身份无效");
                    chain_failed = true;
                    break;
                }
                seen[action.agent_id()] = true;
                const int agent_id = static_cast<int>(action.agent_id());
                const auto before = environment.GetAgent(agent_id);
                environment.Step(agent_id, action.action_id());
                const auto& after = environment.GetAgent(agent_id);
                last_actions[agent_id] = action.action_id();
                LOG_FILE("Frame", "episode=%s frame=%d agent=%d action=%d(%s) (%d,%d)->(%d,%d)",
                         cursor.episode_id.c_str(), environment.GetFrameId(),
                         agent_id, action.action_id(),
                         kActionNames[action.action_id()], before.grid_x,
                         before.grid_y, after.grid_x, after.grid_y);
            }
            if (chain_failed) break;
            environment.AdvanceFrame();
            if (config.viz.recording_enabled &&
                environment.GetFrameId() % config.viz.interval == 0) {
                recorder.RecordFrame(BuildVizJson(
                    environment, environment.GetFrameId(), episode_number,
                    last_actions));
            }
            if (environment.GetFrameId() % config.run.log_interval == 0) {
                const auto& agent = environment.GetAgent(0);
                LOG_INFO("Exec", "episode=%s frame=%d action=%d(%s) grid=(%d,%d)",
                         cursor.episode_id.c_str(), environment.GetFrameId(),
                         last_actions[0], kActionNames[last_actions[0]],
                         agent.grid_x, agent.grid_y);
            }
        }

        recorder.End();
        if (chain_failed) {
            if (episode_active) {
                maze::AbortEpisodeReq abort_request;
                FillCommand(cursor, "abort-chain-failure",
                            abort_request.mutable_command());
                abort_request.set_reason(
                    maze::MAZE_TERMINATION_REASON_CHAIN_FAILURE);
                abort_request.set_message(
                    "Client stopped the Episode after a chain failure");
                maze::AbortEpisodeRsp abort_response;
                if (client.AbortEpisode(abort_request, abort_response) &&
                    AcceptCommandReply(cursor,
                                       abort_response.lifecycle())) {
                    episode_active = false;
                }
            }
            break;
        }
        if (g_stop_requested.load() && episode_active) {
            maze::AbortEpisodeReq abort_request;
            FillCommand(cursor, "abort-client-stop",
                        abort_request.mutable_command());
            abort_request.set_reason(
                maze::MAZE_TERMINATION_REASON_CLIENT_ABORT);
            abort_request.set_message("Client received stop signal");
            maze::AbortEpisodeRsp abort_response;
            if (!client.AbortEpisode(abort_request, abort_response) ||
                !AcceptCommandReply(cursor, abort_response.lifecycle())) {
                chain_failed = true;
            }
            episode_active = false;
            break;
        }
        if (!episode_active) break;

        maze::EndEpisodeReq end_request;
        FillCommand(cursor, "end-episode", end_request.mutable_command());
        maze::EndEpisodeRsp end_response;
        if (!client.EndEpisode(end_request, end_response) ||
            !AcceptCommandReply(cursor, end_response.lifecycle())) {
            LOG_ERROR("Main", "EndEpisode 生命周期被拒绝");
            chain_failed = true;
            break;
        }
        episode_active = false;
        ++episode_number;
    }

    if (!episode_active) {
        maze::CloseSessionReq close_request;
        FillCommand(cursor, "close-session", close_request.mutable_command());
        maze::CloseSessionRsp close_response;
        if (!client.CloseSession(close_request, close_response) ||
            !AcceptCommandReply(cursor, close_response.lifecycle())) {
            LOG_ERROR("Main", "CloseSession 生命周期未收敛");
            chain_failed = true;
        }
    }
    client.Disconnect();
    Logger::Instance().Close();
    return chain_failed ? 1 : 0;
}
