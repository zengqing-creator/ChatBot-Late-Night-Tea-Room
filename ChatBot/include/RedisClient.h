#pragma once

#include <string>
#include <chrono>
#include <mutex>

// 前置声明，避免把 hiredis 头暴露给所有调用方
struct redisContext;

// 简单的 Redis 客户端封装（基于 hiredis，同步阻塞）。
// 提供基于 ZSET + Lua 的滑动窗口限流，供 rate_limited 优先走 Redis、
// Redis 不可用时降级到本地内存逻辑使用。
class RedisClient {
public:
    RedisClient() = default;
    ~RedisClient();

    // 从环境变量读取连接参数并连接：
    //   REDIS_HOST        默认 127.0.0.1
    //   REDIS_PORT        默认 6379
    //   REDIS_PASSWORD    可选，设置了则执行 AUTH
    //   REDIS_TIMEOUT_MS  连接超时，默认 300
    bool connectFromEnv();

    // 当前是否已连接
    bool isConnected() const;

    // 滑动窗口限流（原子操作）。返回：
    //   1  -> 允许（未超限）
    //   0  -> 超限（应被限流）
    //  -1  -> Redis 不可用（调用方应降级到本地内存限流）
    int rateLimit(const std::string& key, int limit, int windowSeconds);

private:
    bool connect(const std::string& host, int port, int timeoutMs,
                 const std::string& password);
    void close();

    redisContext* ctx_ = nullptr;
    // 连接失败时间，用于冷却期（Redis 宕机时避免每个请求都阻塞等超时）
    std::chrono::steady_clock::time_point last_fail_;
    mutable std::mutex mtx_;
};
