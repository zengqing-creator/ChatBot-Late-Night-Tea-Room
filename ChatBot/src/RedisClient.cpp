#include "RedisClient.h"

// Windows 下 struct timeval 定义在 winsock2.h 中，而 hiredis.h 在 MSVC 下只做前置声明，
// 因此必须先包含 winsock2.h，否则 redisConnectWithTimeout 所需的完整 timeval 不可用
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <hiredis/hiredis.h>

#include <cstdlib>
#include <iostream>
#include <vector>
#include <atomic>

namespace {

// 读环境变量，为空时返回默认值
const char* envOrDefault(const char* name, const char* def) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : def;
}

// 滑动窗口限流 Lua 脚本（原子执行）：
//   KEYS[1] = 限流 key（ZSET，score 为时间戳）
//   ARGV[1] = 当前时间（unix 秒，带小数）
//   ARGV[2] = 窗口大小（秒）
//   ARGV[3] = 上限
//   ARGV[4] = 本次请求的唯一 member（避免同毫秒并发时 ZADD 覆盖计数）
// 返回 1 表示允许，0 表示超限。
const char* RATE_LIMIT_SCRIPT =
    "redis.call('ZREMRANGEBYSCORE', KEYS[1], '-inf', tonumber(ARGV[1]) - tonumber(ARGV[2])) "
    "if redis.call('ZCARD', KEYS[1]) >= tonumber(ARGV[3]) then return 0 end "
    "redis.call('ZADD', KEYS[1], ARGV[1], ARGV[4]) "
    "redis.call('EXPIRE', KEYS[1], ARGV[2]) "
    "return 1";

// 以 argv 方式执行命令（redisCommandArgv 不经过 printf 格式化，避免脚本/密码中的
// 空格、引号等特殊字符被错误拆分）
redisReply* commandArgv(redisContext* c, const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    std::vector<size_t> argvlen;
    argv.reserve(args.size());
    argvlen.reserve(args.size());
    for (const auto& s : args) {
        argv.push_back(s.c_str());
        argvlen.push_back(s.size());
    }
    return (redisReply*)redisCommandArgv(c, (int)argv.size(), argv.data(), argvlen.data());
}

}  // namespace

RedisClient::~RedisClient() {
    close();
}

bool RedisClient::connect(const std::string& host, int port, int timeoutMs,
                          const std::string& password) {
    close();

    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    redisContext* c = redisConnectWithTimeout(host.c_str(), port, tv);
    bool redis_ok = (c != nullptr && c->err == 0);

    // 可选密码认证
    if (redis_ok && !password.empty()) {
        redisReply* r = commandArgv(c, {"AUTH", password});
        redis_ok = (r != nullptr && c->err == 0 && r->type != REDIS_REPLY_ERROR);
        if (r) freeReplyObject(r);
    }

    if (redis_ok) {
        ctx_ = c;
        std::cout << "[Redis] 连接成功，限流和缓存已启用" << std::endl;
    } else {
        if (c) redisFree(c);
        std::cout << "[Redis] 连接失败，已降级为单机内存限流" << std::endl;
    }
    return redis_ok;
}

void RedisClient::close() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

bool RedisClient::connectFromEnv() {
    std::lock_guard<std::mutex> lock(mtx_);
    const std::string host = envOrDefault("REDIS_HOST", "127.0.0.1");
    const int port = std::atoi(envOrDefault("REDIS_PORT", "6379"));
    const int timeoutMs = std::atoi(envOrDefault("REDIS_TIMEOUT_MS", "300"));
    const std::string password = envOrDefault("REDIS_PASSWORD", "");
    return connect(host, port, timeoutMs, password);
}

bool RedisClient::isConnected() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return ctx_ != nullptr;
}

int RedisClient::rateLimit(const std::string& key, int limit, int windowSeconds) {
    std::lock_guard<std::mutex> lock(mtx_);

    // 未连接时懒连接；连接失败进入冷却期，避免 Redis 宕机时每个请求都阻塞等待超时
    if (!ctx_) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_fail_ < std::chrono::seconds(5)) {
            return -1;
        }
        const std::string host = envOrDefault("REDIS_HOST", "127.0.0.1");
        const int port = std::atoi(envOrDefault("REDIS_PORT", "6379"));
        const int timeoutMs = std::atoi(envOrDefault("REDIS_TIMEOUT_MS", "300"));
        const std::string password = envOrDefault("REDIS_PASSWORD", "");
        if (!connect(host, port, timeoutMs, password)) {
            last_fail_ = std::chrono::steady_clock::now();
            return -1;
        }
        last_fail_ = std::chrono::steady_clock::time_point{};
    }

    // 当前时间用系统时钟（跨进程共享计数必须用墙上时间）
    const double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // 唯一 member：时间 + 进程内自增序号，避免并发下 ZADD 覆盖
    static std::atomic<unsigned long long> seq{0};
    const std::string member = std::to_string(now) + "-" + std::to_string(seq.fetch_add(1));

    redisReply* reply = commandArgv(ctx_, {
        "EVAL",
        RATE_LIMIT_SCRIPT,
        "1",
        key,
        std::to_string(now),
        std::to_string(windowSeconds),
        std::to_string(limit),
        member,
    });

    // 连接级失败（断连/超时等）：关闭连接，下次重连，本次降级
    if (!reply || ctx_->err) {
        if (reply) freeReplyObject(reply);
        last_fail_ = std::chrono::steady_clock::now();
        close();
        return -1;
    }

    int result = -1;
    if (reply->type == REDIS_REPLY_INTEGER) {
        result = reply->integer != 0 ? 1 : 0;  // Lua 返回 1/0
    }
    // REDIS_REPLY_ERROR / NIL 等异常情况统一视为不可用，降级
    freeReplyObject(reply);
    return result;
}
