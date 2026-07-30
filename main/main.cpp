#include "grpc/grpc_client.h"
#include "env/maze_env.h"
#include "maze.pb.h"
#include "config/config_loader.h"
#include "viz/viz_recorder.h"
#include "log/logger.h"

#include <atomic>
#include <csignal>
#include <cmath>
#include <string>
#include <sstream>
#include <vector>

// --- 默认配置文件路径 ---
static const char* kDefaultConfigPath = "configs/client_config.yaml";

// ---- 动作名称表（用于日志输出）----
static const char* kActionNames[9] = {
    "不动", "上", "右上", "右", "右下", "下", "左下", "左", "左上"
};

static std::atomic<bool> g_stop_requested{false};

static void HandleSignal(int) {
    g_stop_requested.store(true);
}

static maze::TerminationReason ToProtoTerminationReason(AgentTerminationReason reason) {
    switch (reason) {
        case AgentTerminationReason::GoalReached:
            return maze::TERMINATION_REASON_GOAL_REACHED;
        case AgentTerminationReason::TimeLimit:
            return maze::TERMINATION_REASON_TIME_LIMIT;
        case AgentTerminationReason::Countdown:
            return maze::TERMINATION_REASON_COUNTDOWN;
        case AgentTerminationReason::Active:
        default:
            return maze::TERMINATION_REASON_ACTIVE;
    }
}

static maze::WorkloadMode ToProtoWorkloadMode(const std::string& workload) {
    if (workload == "training") return maze::WORKLOAD_MODE_TRAINING;
    if (workload == "inference-smoke") {
        return maze::WORKLOAD_MODE_INFERENCE_SMOKE;
    }
    if (workload == "model-evaluation") {
        return maze::WORKLOAD_MODE_MODEL_EVALUATION;
    }
    return maze::WORKLOAD_MODE_UNSPECIFIED;
}

// ---- 构建可视化 JSON 数据 ----
// 帧数据不再存储任何地图信息，只存 map_id 引用，地图文件由播放器独立加载
static std::string BuildVizJson(const MazeEnv& env,
                                int frame_id, int episode_id,
                                const std::vector<int>& actions) {
    std::ostringstream ss;
    ss << "{";

    // 帧信息
    ss << "\"type\":\"frame_update\",";
    ss << "\"frame_id\":" << frame_id << ",";
    ss << "\"episode_id\":" << episode_id << ",";

    // 地图引用（仅存 map_id，播放器通过 /api/map 接口加载完整地图）
    ss << "\"map_id\":\"" << env.GetMapId() << "\",";

    // Agent 列表（网格坐标转换为连续坐标用于可视化）
    ss << "\"agents\":[";
    for (int i = 0; i < env.GetAgentNum(); ++i) {
        const AgentInfo& a = env.GetAgent(i);
        float world_x = env.GetWorldX(a.grid_x);
        float world_y = env.GetWorldY(a.grid_y);
        if (i > 0) ss << ",";
        ss << "{\"id\":" << a.id
           << ",\"x\":" << world_x
           << ",\"y\":" << world_y
           << ",\"done\":" << (a.done ? "true" : "false")
           << ",\"action_id\":" << (i < static_cast<int>(actions.size()) ? actions[i] : 0)
           << "}";
    }
    ss << "]";

    ss << "}";
    return ss.str();
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::printf("============================================\n");
    std::printf("  迷宫训练框架 - TrainClient\n");
    std::printf("============================================\n\n");

    // ---- 0. 加载配置 ----
    const char* config_path = (argc > 1) ? argv[1] : kDefaultConfigPath;
    ClientConfig cfg;
    LoadClientConfig(config_path, cfg);
    const maze::WorkloadMode workload_mode =
        ToProtoWorkloadMode(cfg.run.workload);
    if (workload_mode == maze::WORKLOAD_MODE_UNSPECIFIED) {
        std::fprintf(
            stderr,
            "Client workload is required: inference-smoke, training, or "
            "model-evaluation\n");
        return 2;
    }

    // ---- 0a. 初始化日志系统 ----
    Logger::Instance().Init("log");
    Logger::Instance().SetConsoleLevel(LogLevel::INFO);
    Logger::Instance().SetFileLevel(LogLevel::DEBUG);

    // ---- 1. 初始化环境 ----
    MazeEnv env;
    env.Init(cfg);

    // ---- 2. 初始化帧数据记录器 ----
    VizRecorder viz_recorder;

    // ---- 3. 建立 gRPC 连接 ----
    GrpcClient client;
    if (!client.Connect(cfg.network.server_host, cfg.network.server_port)) {
        LOG_ERROR("Main", "无法连接 AIServer %s:%d，退出",
                  cfg.network.server_host.c_str(), cfg.network.server_port);
        Logger::Instance().Close();
        return 1;
    }

    // ---- 4. 发送 InitReq ----
    {
        maze::InitReq req;
        req.set_agent_num(cfg.run.agent_num);
        req.set_session_id(cfg.run.session_id);
        req.set_client_id(cfg.run.client_id);
        req.set_env_id(cfg.run.env_id);
        req.set_workload_mode(workload_mode);
        req.set_grid_size(env.GetGridSize());
        req.set_grid_cols(env.GetGridCols());
        req.set_grid_rows(env.GetGridRows());
        auto* end_grid = req.mutable_end_grid();
        end_grid->set_x(static_cast<float>(env.ToGridX(env.GetEndX())));
        end_grid->set_y(static_cast<float>(env.ToGridY(env.GetEndY())));

        auto* map_size = req.mutable_map_size();
        map_size->set_x(env.GetMapWidth());
        map_size->set_y(env.GetMapHeight());

        auto* start_pos = req.mutable_start_pos();
        start_pos->set_x(env.GetStartX());
        start_pos->set_y(env.GetStartY());

        auto* end_pos = req.mutable_end_pos();
        end_pos->set_x(env.GetEndX());
        end_pos->set_y(env.GetEndY());

        maze::InitRsp rsp;
        if (!client.Init(req, rsp)) {
            LOG_ERROR("Main", "Init RPC 失败");
            Logger::Instance().Close();
            return 1;
        }
        LOG_INFO("Main", "初始化成功: ret_code=%d", rsp.ret_code());

        if (rsp.ret_code() != 0) {
            LOG_ERROR("Main", "初始化失败，退出");
            Logger::Instance().Close();
            return 1;
        }
    }

    // ---- 5. Episode 循环 ----
    bool chain_failed = false;
    for (int ep = 0; ep < cfg.run.max_episodes && !g_stop_requested.load(); ++ep) {
        env.Reset();
        LOG_INFO("Main", "===== Episode %d 开始 =====", ep);

        maze::BeginEpisodeReq begin_req;
        begin_req.set_session_id(cfg.run.session_id);
        begin_req.set_episode_id(ep);
        maze::EpisodeLifecycleRsp begin_rsp;
        if (!client.BeginEpisode(begin_req, begin_rsp) ||
            begin_rsp.result() == maze::LIFECYCLE_RESULT_REJECTED) {
            LOG_ERROR("Main", "BeginEpisode 失败: episode=%d, message=%s",
                      ep, begin_rsp.message().c_str());
            chain_failed = true;
            break;
        }

        // 开始帧数据记录
        if (cfg.viz.enabled) {
            viz_recorder.Begin(cfg.viz.output_dir, ep,
                               env.GetMapId(), env.GetMapFilePath());
        }

        // 每帧记录各 Agent 的动作（用于可视化）
        std::vector<int> last_actions(cfg.run.agent_num, 0);

        // ---- 帧循环 ----
        while (!g_stop_requested.load()) {
            bool reporting_terminal_state = env.AllDone();

            // ---- 5a. 构建 UpdateReq（上报网格坐标，通过 Vec2 float 传输）----
            maze::UpdateReq update_req;
            update_req.set_frame_id(env.GetFrameId());
            update_req.set_session_id(cfg.run.session_id);
            update_req.set_episode_id(ep);

            for (int i = 0; i < env.GetAgentNum(); ++i) {
                const AgentInfo& info = env.GetAgent(i);
                auto* agent_state = update_req.add_agents();
                agent_state->set_agent_id(info.id);
                auto* pos = agent_state->mutable_pos();
                pos->set_x(static_cast<float>(info.grid_x));
                pos->set_y(static_cast<float>(info.grid_y));
                agent_state->set_is_done(info.done);
                agent_state->set_termination_reason(
                    ToProtoTerminationReason(info.termination_reason));

                RayResult rays = env.CastRays(info.grid_x, info.grid_y);
                for (int d = 0; d < 8; ++d) {
                    agent_state->add_obs(rays.distances[d]);
                }
            }

            // ---- 5b. 发送 UpdateReq，接收 UpdateRsp ----
            maze::UpdateRsp update_rsp;
            if (!client.Update(update_req, update_rsp)) {
                LOG_ERROR("Main", "Update RPC 失败，终止当前 Episode");
                maze::AbortEpisodeReq abort_req;
                abort_req.set_session_id(cfg.run.session_id);
                abort_req.set_episode_id(ep);
                abort_req.set_reason(maze::TERMINATION_REASON_CHAIN_FAILURE);
                abort_req.set_message("Update RPC failed");
                maze::EpisodeLifecycleRsp abort_rsp;
                client.AbortEpisode(abort_req, abort_rsp);
                viz_recorder.End();
                chain_failed = true;
                break;
            }

            if (reporting_terminal_state) {
                break;
            }

            // ---- 5c. 执行动作 ----
            for (int i = 0; i < update_rsp.actions_size(); ++i) {
                const auto& action = update_rsp.actions(i);
                int aid = action.agent_id();
                int act = action.action_id();

                // 记录执行前位置
                const AgentInfo& before = env.GetAgent(aid);
                int prev_gx = before.grid_x;
                int prev_gy = before.grid_y;

                env.Step(aid, act);

                // 记录执行后位置
                const AgentInfo& after = env.GetAgent(aid);
                const char* act_name = (act >= 0 && act < 9) ? kActionNames[act] : "未知";

                // 帧级详细日志（仅写文件）
                LOG_FILE("Frame", "EP:%d F:%d A:%d | 指令:%d(%s) | (%d,%d)->(%d,%d) %s",
                         ep, env.GetFrameId(), aid, act, act_name,
                         prev_gx, prev_gy, after.grid_x, after.grid_y,
                         (prev_gx == after.grid_x && prev_gy == after.grid_y) ? "[未移动]" : "[已移动]");

                // 记录动作用于可视化
                if (aid >= 0 && aid < static_cast<int>(last_actions.size())) {
                    last_actions[aid] = act;
                }
            }

            // ---- 5d. 帧号递增 ----
            env.AdvanceFrame();

            // ---- 5e. 记录帧数据（切片记录，用于离线回放）----
            if (cfg.viz.enabled) {
                if (env.GetFrameId() % cfg.viz.interval == 0) {
                    std::string json = BuildVizJson(env, env.GetFrameId(), ep, last_actions);
                    viz_recorder.RecordFrame(json);
                }
            }

            // ---- 5f. 定期日志：打印接收到的指令和执行效果 ----
            if (env.GetFrameId() % cfg.run.log_interval == 0) {
                const AgentInfo& a = env.GetAgent(0);
                int act = last_actions.empty() ? 0 : last_actions[0];
                const char* act_name = (act >= 0 && act < 9) ? kActionNames[act] : "未知";
                LOG_INFO("Exec", "EP:%d F:%d | 指令:action=%d(%s) | 网格:(%d,%d) 终点:(%d,%d)",
                         ep, env.GetFrameId(), act, act_name,
                         a.grid_x, a.grid_y, env.ToGridX(env.GetEndX()), env.ToGridY(env.GetEndY()));
            }
        }

        if (chain_failed) {
            break;
        }

        if (g_stop_requested.load()) {
            maze::AbortEpisodeReq abort_req;
            abort_req.set_session_id(cfg.run.session_id);
            abort_req.set_episode_id(ep);
            abort_req.set_reason(maze::TERMINATION_REASON_CLIENT_ABORT);
            abort_req.set_message("Client received stop signal");
            maze::EpisodeLifecycleRsp abort_rsp;
            client.AbortEpisode(abort_req, abort_rsp);
            viz_recorder.End();
            break;
        }

        // ---- 6. Episode 结束 ----
        {
            // 结束帧数据记录
            viz_recorder.End();

            // 统计结果
            const AgentInfo& a = env.GetAgent(0);
            bool passed = (a.grid_x == env.ToGridX(env.GetEndX()) &&
                           a.grid_y == env.ToGridY(env.GetEndY()));
            int grid_dist = std::abs(a.grid_x - env.ToGridX(env.GetEndX())) +
                            std::abs(a.grid_y - env.ToGridY(env.GetEndY()));

            LOG_INFO("Result", "EP:%d 结束 | 帧数:%d | %s | 网格距离:%d",
                     ep, env.GetFrameId(), passed ? "通关" : "超时", grid_dist);

            // 发送 EpisodeEndReq
            maze::EpisodeEndReq ep_end_req;
            ep_end_req.set_episode_id(ep);
            ep_end_req.set_session_id(cfg.run.session_id);

            maze::EpisodeEndRsp ep_end_rsp;
            if (!client.EndEpisode(ep_end_req, ep_end_rsp) ||
                ep_end_rsp.result() == maze::LIFECYCLE_RESULT_REJECTED) {
                LOG_ERROR("Main", "EndEpisode 失败: episode=%d, message=%s",
                          ep, ep_end_rsp.message().c_str());
                chain_failed = true;
                break;
            }
        }
    }

    // ---- 7. 结束 ----
    LOG_INFO("Main", "Client 结束");
    Logger::Instance().Close();

    return chain_failed ? 1 : 0;
}
