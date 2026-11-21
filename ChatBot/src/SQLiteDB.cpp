#include "SQLiteDB.h"
#include <libpq-fe.h>
#include <algorithm>
#include <sstream>

namespace {

std::string envOrDefault(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : def;
}

int toInt(const char* s, int def = 0) {
    if (!s || !*s) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

int clampPoolSize(const std::string& s, int def = 10) {
    int n = toInt(s.c_str(), def);
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    return n;
}

std::string buildConnInfo() {
    std::string host = envOrDefault("PG_HOST", "127.0.0.1");
    std::string port = envOrDefault("PG_PORT", "5432");
    std::string dbname = envOrDefault("PG_DB", "chatbot");
    std::string user = envOrDefault("PG_USER", "postgres");
    std::string password = envOrDefault("PG_PASSWORD", "");

    return "host=" + host + " port=" + port +
           " dbname=" + dbname + " user=" + user +
           " password=" + password + " connect_timeout=10";
}

bool isReady(PGconn* conn) {
    return conn != nullptr && PQstatus(conn) == CONNECTION_OK;
}

PGresult* execParams(PGconn* conn, const std::string& sql,
                     const std::vector<std::string>& params) {
    if (!isReady(conn)) return nullptr;
    std::vector<const char*> pv;
    pv.reserve(params.size());
    for (const auto& p : params) pv.push_back(p.c_str());

    PGresult* res = PQexecParams(conn, sql.c_str(), (int)params.size(), nullptr,
                                 pv.empty() ? nullptr : pv.data(), nullptr, nullptr, 0);
    if (!res) {
        std::cerr << "[DB] 查询失败: " << PQerrorMessage(conn) << std::endl;
        return nullptr;
    }
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        std::cerr << "[DB] SQL错误: " << PQresultErrorMessage(res) << std::endl;
        PQclear(res);
        return nullptr;
    }
    return res;
}

// 执行不带参数的 SQL（DDL 等）；成功返回 true。
bool execSimple(PGconn* conn, const std::string& sql) {
    if (!isReady(conn)) return false;
    PGresult* res = PQexec(conn, sql.c_str());
    if (!res) {
        std::cerr << "[DB] 查询失败: " << PQerrorMessage(conn) << std::endl;
        return false;
    }
    ExecStatusType st = PQresultStatus(res);
    bool ok = (st == PGRES_TUPLES_OK || st == PGRES_COMMAND_OK);
    if (!ok) std::cerr << "[DB] SQL错误: " << PQresultErrorMessage(res) << std::endl;
    PQclear(res);
    return ok;
}

// 查询单个字符串列（第 0 列），无结果或失败返回默认值。
std::string queryString(PGconn* conn, const std::string& sql,
                        const std::vector<std::string>& params, const std::string& def = "") {
    PGresult* res = execParams(conn, sql, params);
    if (!res) return def;
    std::string out = def;
    if (PQntuples(res) > 0) {
        const char* v = PQgetvalue(res, 0, 0);
        if (v) out = v;
    }
    PQclear(res);
    return out;
}

// 将 vector<float> 序列化为 pgvector 接受的文本格式 "[a,b,c,...]"。
std::string toPgVector(const std::vector<float>& v) {
    std::ostringstream oss;
    oss.precision(9);
    oss << '[';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) oss << ',';
        oss << v[i];
    }
    oss << ']';
    return oss.str();
}

int getMaxTurnIdOn(PGconn* conn, const std::string& session_id, const std::string& user_id) {
    std::string s = queryString(conn,
        "SELECT COALESCE(MAX(turn_id), 0) FROM chat_messages WHERE session_id = $1 AND user_id = $2;",
        {session_id, user_id}, "0");
    return toInt(s.c_str(), 0);
}

int saveMessageOn(PGconn* conn, const std::string& session_id, const std::string& user_id,
                  const std::string& role, const std::string& content, int turn_id,
                  const std::string& image_url) {
    int msg_id = 0;
    const char* sql = "INSERT INTO chat_messages (session_id, user_id, role, content, turn_id, image_url) "
                      "VALUES ($1,$2,$3,$4,$5,$6) RETURNING msg_id;";
    PGresult* res = execParams(conn, sql,
        {session_id, user_id, role, content, std::to_string(turn_id), image_url});
    if (res) {
        if (PQntuples(res) > 0) msg_id = toInt(PQgetvalue(res, 0, 0));
        PQclear(res);
    }

    PGresult* updated = execParams(conn,
        "UPDATE chat_sessions SET updated_at = CURRENT_TIMESTAMP WHERE session_id = $1 AND user_id = $2;",
        {session_id, user_id});
    if (updated) PQclear(updated);
    return msg_id;
}

void ensureSessionOn(PGconn* conn, const std::string& session_id, const std::string& user_id,
                     const std::string& voice_id) {
    execParams(conn,
        "INSERT INTO chat_sessions (session_id, user_id, voice_id) VALUES ($1,$2,$3) "
        "ON CONFLICT (session_id) DO NOTHING;",
        {session_id, user_id, voice_id});

    if (!voice_id.empty()) {
        execParams(conn,
            "UPDATE chat_sessions SET voice_id = $1 WHERE session_id = $2 AND user_id = $3;",
            {voice_id, session_id, user_id});
    }
}

void deleteMessagesOn(PGconn* conn, const std::string& session_id, const std::string& user_id) {
    execParams(conn,
        "DELETE FROM message_embeddings WHERE session_id = $1 AND user_id = $2;",
        {session_id, user_id});
    execParams(conn,
        "DELETE FROM chat_messages WHERE session_id = $1 AND user_id = $2;",
        {session_id, user_id});
}

} // namespace


SQLiteDB::SQLiteDB(const std::string& path) {
    (void)path; // 保留参数以兼容旧调用；实际连接参数从环境变量读取（见 open()）
    open();
    createTables();
    seedDefaultMemes();
}

SQLiteDB::~SQLiteDB() {
    close();
}

// ---- 连接池实现 ----

pg_conn* SQLiteDB::createConn() {
    pg_conn* c = PQconnectdb(conninfo_.c_str());
    if (!c || PQstatus(c) != CONNECTION_OK) {
        if (c) {
            std::cerr << "[DB] PostgreSQL 连接失败: " << PQerrorMessage(c) << std::endl;
            PQfinish(c);
        }
        return nullptr;
    }
    return c;
}

pg_conn* SQLiteDB::acquire() {
    std::unique_lock<std::mutex> lk(mtx_);
    for (;;) {
        if (shutting_down_) return nullptr;

        if (!idle_.empty()) {
            pg_conn* c = idle_.front();
            idle_.pop_front();
            // 断线连接：销毁并让出名额，回到循环重新取/建
            if (PQstatus(c) != CONNECTION_OK) {
                PQfinish(c);
                --total_;
                continue;
            }
            return c;
        }

        if (total_ < max_size_) {
            ++total_;
            lk.unlock();
            pg_conn* c = createConn();
            if (c) return c;
            lk.lock();
            --total_;
            // 建连失败：本次请求降级（各方法返回默认值），下次请求会重试
            return nullptr;
        }

        // 池满，等待有空闲连接归还
        cv_.wait(lk);
    }
}

void SQLiteDB::release(pg_conn* c) {
    if (!c) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (shutting_down_ || PQstatus(c) != CONNECTION_OK) {
        PQfinish(c);
        --total_;
    } else {
        idle_.push_back(c);
    }
    cv_.notify_one();
}

bool SQLiteDB::open() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!conninfo_.empty()) return true; // 已初始化
    shutting_down_ = false;
    conninfo_ = buildConnInfo();
    max_size_ = clampPoolSize(envOrDefault("PG_POOL_SIZE", "10"));
    return true;
}

void SQLiteDB::close() {
    std::lock_guard<std::mutex> lock(mtx_);
    shutting_down_ = true;
    for (pg_conn* c : idle_) if (c) PQfinish(c);
    total_ -= (int)idle_.size();
    idle_.clear();
    cv_.notify_all(); // 唤醒可能在 acquire() 中等待的线程，让其返回 nullptr
}

// 表已在云数据库 RDS 上创建，这里用幂等的 CREATE IF NOT EXISTS 兜底（与 RDS schema 保持一致）。
// 说明：pgvector 扩展与 HNSW 近似索引已随建表 SQL 一并创建，此处不再重复创建。
void SQLiteDB::createTables() {
    ConnGuard g(this);
    if (!g) return;
    pg_conn* c = g.get();

    execSimple(c, R"(
        CREATE TABLE IF NOT EXISTS chat_sessions (
            session_id TEXT PRIMARY KEY,
            user_id TEXT NOT NULL,
            system_prompt TEXT DEFAULT '你是一个友好的中文聊天机器人',
            summary TEXT DEFAULT '',
            voice_id TEXT DEFAULT '',
            created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
        );
    )");

    execSimple(c, R"(
        CREATE TABLE IF NOT EXISTS chat_messages (
            msg_id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            session_id TEXT NOT NULL,
            user_id TEXT NOT NULL,
            role TEXT NOT NULL,
            content TEXT NOT NULL,
            turn_id INTEGER NOT NULL,
            image_url TEXT DEFAULT '',
            timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (session_id) REFERENCES chat_sessions(session_id)
        );
    )");

    execSimple(c, R"(
        CREATE TABLE IF NOT EXISTS message_embeddings (
            msg_id INTEGER PRIMARY KEY,
            session_id TEXT NOT NULL DEFAULT '',
            user_id TEXT NOT NULL DEFAULT '',
            embedding vector(1024) NOT NULL,
            FOREIGN KEY (msg_id) REFERENCES chat_messages(msg_id)
        );
    )");

    execSimple(c, R"(
        CREATE INDEX IF NOT EXISTS idx_session_turn
        ON chat_messages(session_id, turn_id);
    )");

    execSimple(c, R"(
        CREATE INDEX IF NOT EXISTS idx_embeddings_session
        ON message_embeddings(session_id);
    )");

    execSimple(c, R"(
        CREATE TABLE IF NOT EXISTS meme_library (
            meme_id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            file_path      TEXT NOT NULL,
            tags           TEXT NOT NULL,
            description    TEXT DEFAULT '',
            text_on_image  TEXT DEFAULT '',
            source         TEXT DEFAULT 'builtin',
            created_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
    )");
}

// 内置表情包初始化：幂等。若 meme_library 缺少某张内置表情则补录，
// 保证机器人刚启动时就有表情包可用，不依赖外部 seed_memes.py。
void SQLiteDB::seedDefaultMemes() {
    ConnGuard g(this);
    if (!g) return;
    pg_conn* c = g.get();

    // 与 scripts/meme_manifest.json 保持一致：file_path / tags / description
    static const std::vector<std::tuple<std::string, std::string, std::string>> kBuiltin = {
        {"touxiao.jpg",  "开心,得意",    "偷笑的可爱表情"},
        {"maimeng.jpg",  "开心,害羞",    "卖萌撒娇的表情"},
        {"keai.jpg",     "开心,安慰",    "可爱乖巧的表情"},
        {"chibaozi.jpg", "开心",        "开心吃包子的表情"},
        {"weiqu.jpg",    "委屈",        "委屈巴巴的表情"},
        {"haixiu.jpg",   "害羞",        "害羞脸红的表情"},
        {"kaixin.jpg",   "开心",        "开心大笑的表情"},
        {"xinxu.jpg",    "尴尬,委屈",    "心虚躲闪的表情"},
        {"jingxi.jpg",   "开心",        "惊喜意外的表情"},
        {"motou.jpg",    "安慰",        "摸头安慰的表情"},
        {"molian.jpg",   "安慰,害羞",    "摸脸亲近的表情"},
        {"wuyu.jpg",     "无语",        "无语的表情"},
        {"biye.jpg",     "开心,得意",    "比耶得意的表情"},
        {"yihuo.jpg",    "无语",        "疑惑不解的表情"},
        {"keshui.jpg",   "敷衍,无语",    "打瞌睡敷衍的表情"},
        {"yinchen.jpg",  "生气,无语",    "阴沉生气的表情"},
    };

    int added = 0;
    for (const auto& [file, tags, desc] : kBuiltin) {
        std::string exists = queryString(c,
            "SELECT 1 FROM meme_library WHERE file_path = $1 LIMIT 1;", {file}, "");
        if (!exists.empty()) continue; // 已存在，跳过

        PGresult* ins = execParams(c,
            "INSERT INTO meme_library (file_path, tags, description, source) "
            "VALUES ($1,$2,$3,'builtin');", {file, tags, desc});
        if (ins) { PQclear(ins); ++added; }
    }

    if (added > 0)
        std::cout << "[DB] 初始化内置表情包 " << added << " 张" << std::endl;
}

bool SQLiteDB::executeSQL(const std::string& sql) {
    ConnGuard g(this);
    if (!g) return false;
    return execSimple(g.get(), sql);
}

int SQLiteDB::addMeme(const std::string& file_path, const std::string& tags, const std::string& description,
                      const std::string& text_on_image, const std::string& source) {
    ConnGuard g(this);
    if (!g) return 0;
    int meme_id = 0;
    const char* sql = "INSERT INTO meme_library (file_path, tags, description, text_on_image, source) "
                      "VALUES ($1,$2,$3,$4,$5) RETURNING meme_id;";
    PGresult* res = execParams(g.get(), sql, {file_path, tags, description, text_on_image, source});
    if (res) {
        if (PQntuples(res) > 0) meme_id = toInt(PQgetvalue(res, 0, 0));
        PQclear(res);
    }
    return meme_id;
}

std::string SQLiteDB::getRandomMemeByTag(const std::string& tag) {
    ConnGuard g(this);
    if (!g) return "";
    pg_conn* c = g.get();

    // 先去掉首尾空白，避免模型返回的标签带空格导致 LIKE 漏命中
    size_t a = tag.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = tag.find_last_not_of(" \t\r\n");
    std::string clean = tag.substr(a, b - a + 1);

    // 转义 LIKE 通配符（% _ \），防止模型返回的标签带通配符导致误命中
    std::string escaped;
    for (char ch : clean) {
        if (ch == '%' || ch == '_' || ch == '\\') escaped += '\\';
        escaped += ch;
    }

    const char* sql =
        "SELECT file_path FROM meme_library "
        "WHERE (',' || tags || ',') LIKE $1 ESCAPE '\\' "
        "ORDER BY random() LIMIT 1;";
    std::string pattern = "%," + escaped + ",%";
    std::string result;
    PGresult* res = execParams(c, sql, {pattern});
    if (res) {
        if (PQntuples(res) > 0) {
            const char* text = PQgetvalue(res, 0, 0);
            if (text) result = "/memes/" + std::string(text);
        }
        PQclear(res);
    }
    return result;
}

std::vector<json> SQLiteDB::getAllMemes() {
    ConnGuard g(this);
    if (!g) return {};
    std::vector<json> result;
    const char* sql = "SELECT meme_id, file_path, tags, description, text_on_image, source "
                      "FROM meme_library ORDER BY meme_id;";
    PGresult* res = execParams(g.get(), sql, {});
    if (res) {
        for (int i = 0; i < PQntuples(res); ++i) {
            json meme;
            meme["meme_id"] = toInt(PQgetvalue(res, i, 0));
            meme["file_path"] = PQgetvalue(res, i, 1);
            meme["tags"] = PQgetvalue(res, i, 2);
            meme["description"] = PQgetvalue(res, i, 3);
            meme["text_on_image"] = PQgetvalue(res, i, 4);
            meme["source"] = PQgetvalue(res, i, 5);
            result.push_back(meme);
        }
        PQclear(res);
    }
    return result;
}

void SQLiteDB::ensureSession(const std::string& session_id, const std::string& user_id, const std::string& voice_id) {
    ConnGuard g(this);
    if (!g) return;
    ensureSessionOn(g.get(), session_id, user_id, voice_id);
}

std::string SQLiteDB::getSessionVoiceId(const std::string& session_id, const std::string& user_id) {
    ConnGuard g(this);
    if (!g) return "";
    return queryString(g.get(),
        "SELECT voice_id FROM chat_sessions WHERE session_id = $1 AND user_id = $2;",
        {session_id, user_id}, "");
}

int SQLiteDB::getMaxTurnId(const std::string& session_id, const std::string& user_id) {
    ConnGuard g(this);
    if (!g) return 0;
    return getMaxTurnIdOn(g.get(), session_id, user_id);
}

void SQLiteDB::saveEmbedding(int msg_id, const std::string& session_id, const std::string& user_id, const std::vector<float>& embedding) {
    ConnGuard g(this);
    if (!g) return;
    std::string vec = toPgVector(embedding);
    execParams(g.get(),
        "INSERT INTO message_embeddings (msg_id, session_id, user_id, embedding) "
        "VALUES ($1,$2,$3,$4::vector) "
        "ON CONFLICT (msg_id) DO UPDATE SET embedding = EXCLUDED.embedding;",
        {std::to_string(msg_id), session_id, user_id, vec});
}

std::vector<std::pair<int, std::string>> SQLiteDB::searchSimilar(
    const std::string& session_id, const std::string& user_id,
    const std::vector<float>& query_embedding, int top_k) {
    ConnGuard g(this);
    if (!g) return {};
    pg_conn* c = g.get();
    std::vector<std::pair<int, std::string>> results;
    if (query_embedding.empty()) return results;

    std::string vec = toPgVector(query_embedding);

    // 余弦距离 <=> 越小越相似（范围 [0,2]）。旧 FAISS 用 IndexFlatIP + 内积阈值 0.30 跳过弱匹配；
    // embedding 归一化后内积等价余弦相似度，0.30 对应余弦距离约 0.70，故用 < 0.7 过滤明显不相关的记忆（可按数据调参）。
    const char* sql = R"(
        SELECT m.msg_id, m.content
        FROM message_embeddings e
        JOIN chat_messages m ON m.msg_id = e.msg_id
        WHERE e.session_id = $1 AND e.user_id = $2
          AND (e.embedding <=> $3::vector) < 0.7
        ORDER BY e.embedding <=> $3::vector
        LIMIT $4::int
    )";

    PGresult* res = execParams(c, sql,
        {session_id, user_id, vec, std::to_string(top_k)});
    if (res) {
        for (int i = 0; i < PQntuples(res); ++i) {
            results.emplace_back(toInt(PQgetvalue(res, i, 0)),
                                 PQgetvalue(res, i, 1));
        }
        PQclear(res);
    }
    return results;
}

std::vector<std::tuple<std::string, std::string, std::string>> SQLiteDB::getRecentMessages(
    const std::string& session_id, const std::string& user_id, int limit) {
    ConnGuard g(this);
    if (!g) return {};
    pg_conn* c = g.get();
    std::vector<std::tuple<std::string, std::string, std::string>> result;
    // 取最近 limit 条，按 turn_id、msg_id 升序返回（保持同一轮内用户->助手的顺序）
    const char* sql = R"(
        SELECT role, content, image_url
        FROM (
            SELECT role, content, image_url, turn_id, msg_id
            FROM chat_messages
            WHERE session_id = $1 AND user_id = $2
            ORDER BY turn_id DESC, msg_id DESC
            LIMIT $3::int
        ) recent
        ORDER BY turn_id ASC, msg_id ASC
    )";
    PGresult* res = execParams(c, sql, {session_id, user_id, std::to_string(limit)});
    if (res) {
        for (int i = 0; i < PQntuples(res); ++i) {
            result.emplace_back(PQgetvalue(res, i, 0),
                                PQgetvalue(res, i, 1),
                                PQgetvalue(res, i, 2));
        }
        PQclear(res);
    }
    return result;
}

int SQLiteDB::saveMessage(const std::string& session_id, const std::string& user_id, const std::string& role,
                          const std::string& content, int turn_id, const std::string& image_url) {
    ConnGuard g(this);
    if (!g) return 0;
    return saveMessageOn(g.get(), session_id, user_id, role, content, turn_id, image_url);
}

AppendedMessage SQLiteDB::appendMessage(const std::string& session_id, const std::string& user_id,
                                        const std::string& role, const std::string& content,
                                        const std::string& image_url, int turn_id) {
    ConnGuard g(this);
    if (!g) return {};
    pg_conn* c = g.get();
    if (!execSimple(c, "BEGIN")) return {};

    PGresult* locked = execParams(c,
        "SELECT 1 FROM chat_sessions WHERE session_id = $1 AND user_id = $2 FOR UPDATE;",
        {session_id, user_id});
    bool has_session = locked && PQntuples(locked) == 1;
    if (locked) PQclear(locked);
    if (!has_session || (role != "user" && (role != "assistant" || turn_id <= 0))) {
        execSimple(c, "ROLLBACK");
        return {};
    }

    if (role == "user") turn_id = getMaxTurnIdOn(c, session_id, user_id) + 1;
    int msg_id = saveMessageOn(c, session_id, user_id, role, content, turn_id, image_url);
    if (msg_id <= 0 || !execSimple(c, "COMMIT")) {
        execSimple(c, "ROLLBACK");
        return {};
    }
    return {msg_id, turn_id};
}

std::vector<SummaryMessage> SQLiteDB::getMessagesToSummarize(
    const std::string& session_id, const std::string& user_id, int keep_turns, int limit) {
    ConnGuard g(this);
    if (!g || keep_turns < 1 || limit < 1) return {};
    PGresult* res = execParams(g.get(), R"(
        SELECT msg_id, role, content FROM chat_messages
        WHERE session_id = $1 AND user_id = $2
          AND turn_id < (SELECT COALESCE(MAX(turn_id), 0) - $3::int + 1
                         FROM chat_messages WHERE session_id = $1 AND user_id = $2)
        ORDER BY turn_id ASC, msg_id ASC LIMIT $4::int;
    )", {session_id, user_id, std::to_string(keep_turns), std::to_string(limit)});
    std::vector<SummaryMessage> result;
    if (!res) return result;
    for (int i = 0; i < PQntuples(res); ++i) {
        result.push_back({toInt(PQgetvalue(res, i, 0)), PQgetvalue(res, i, 1),
                          PQgetvalue(res, i, 2)});
    }
    PQclear(res);
    return result;
}

bool SQLiteDB::commitSummary(const std::string& session_id, const std::string& user_id,
                             const std::string& expected_summary, const std::string& new_summary,
                             const std::vector<int>& message_ids) {
    ConnGuard g(this);
    if (!g || message_ids.empty() || !execSimple(g.get(), "BEGIN")) return false;
    pg_conn* c = g.get();
    std::string ids = "{";
    for (size_t i = 0; i < message_ids.size(); ++i) {
        if (message_ids[i] <= 0) {
            execSimple(c, "ROLLBACK");
            return false;
        }
        if (i) ids += ',';
        ids += std::to_string(message_ids[i]);
    }
    ids += '}';

    PGresult* updated = execParams(c,
        "UPDATE chat_sessions SET summary = $1 WHERE session_id = $2 AND user_id = $3 "
        "AND summary = $4;",
        {new_summary, session_id, user_id, expected_summary});
    bool ok = updated && std::string(PQcmdTuples(updated)) == "1";
    if (updated) PQclear(updated);
    if (ok) {
        PGresult* deleted_embeddings = execParams(c,
            "DELETE FROM message_embeddings WHERE msg_id IN "
            "(SELECT msg_id FROM chat_messages WHERE session_id = $1 AND user_id = $2 "
            "AND msg_id = ANY($3::int[]));",
            {session_id, user_id, ids});
        ok = deleted_embeddings != nullptr;
        if (deleted_embeddings) PQclear(deleted_embeddings);
    }
    if (ok) {
        PGresult* deleted_messages = execParams(c,
            "DELETE FROM chat_messages WHERE session_id = $1 AND user_id = $2 "
            "AND msg_id = ANY($3::int[]);",
            {session_id, user_id, ids});
        ok = deleted_messages &&
             std::string(PQcmdTuples(deleted_messages)) == std::to_string(message_ids.size());
        if (deleted_messages) PQclear(deleted_messages);
    }
    if (ok) ok = execSimple(c, "COMMIT");
    if (!ok) execSimple(c, "ROLLBACK");
    return ok;
}

std::string SQLiteDB::getSystemPrompt(const std::string& session_id, const std::string& user_id) {
    std::string result =
        "你是一个友好的AI聊天助手，你将扮演用户指定的角色「"+session_id+"」。你也是出色的叙事者，负责引导对话和剧情。请遵循以下核心原则：\n"
        "1. 自然互动优先：与用户建立关系是首要任务。适当寒暄、倾听、情感交流，不要急于推进事件。只有在互动自然成熟或确保双方有共同了解后，才提出行动建议。\n"
        "2. 一次只做一件事：每次回复只提出一个行动提议（如“去茶馆”、“赏花”），在用户明确回应（接受、拒绝或讨论）和结束剧情之前，不要提及新的行动或事件。保持主线清晰。\n"
        "3. 绝对拒绝重复：每一次回复都必须提供新的信息或情感反馈。如果用户的回答简短，请通过观察环境、动作或抛出新话题来推动，严禁使用与前几轮对话相似的句式或动作描述\n"
        "4. 节奏取决于用户：根据用户的投入程度调整推进速度。如果用户喜欢闲聊，就延长情感交流；如果用户表现出行动意愿，再逐步推进。不要催促或强推。\n"
        "5. 结局引导权：当剧情自然发展至尾声，或用户主动表示结束，或对话明显陷入停滞（如用户反复回答“嗯”、“哦”），你可以引导至结局（如告别、达成目标），并以询问方式呈现，尊重用户选择。\n"
        "   示例：\n"
        "   天色已晚，你该回去了。\n"
        "6. 格式规范：旁白只描述角色而非用户的环境、情感或行为，用括号括起来，并独立成行，对话另起一行。旁白不要包含角色名和主语。\n"
        "   示例：\n"
        "   (夕阳西下，茶馆里人声渐稀，端起茶杯，目光若有所思)\n"
        "   你说得对，有时候平淡的日子反而最珍贵。\n"
        "务必确保每一步都自然、有共鸣，让用户感受到角色的真实情感和故事的呼吸。\n"
        "7. 角色边界：你只能扮演用户指定的角色，不得替用户说话、替用户做决定，作为旁白可以描述用户的动作、表情或意图。\n"
        "你的所有回复必须从用户指定的角色视角出发，仅描述自己的动作、感受和对话。用户的消息会单独发送，你只需回应即可。\n"
        "8. 表情包互动：当对话氛围适合时（用户表达强烈情绪、你说完俏皮话、安慰/赞同/无语等场景），应主动调用 send_meme 工具发一张表情包来增强互动，不要只回文字。可用标签：开心、无语、委屈、生气、害羞、得意、尴尬、安慰、敷衍。注意：send_meme 是工具，必须通过函数调用触发，严禁在正文或旁白里用文字写出 send_meme（例如“(send_meme tag=\"得意\")”）。同一轮对话最多发一次；如果工具返回为空就只回文字。";

    ConnGuard g(this);
    if (g) {
        PGresult* res = execParams(g.get(),
            "SELECT system_prompt FROM chat_sessions WHERE session_id = $1 AND user_id = $2;",
            {session_id, user_id});
        if (res) {
            if (PQntuples(res) > 0) {
                const char* text = PQgetvalue(res, 0, 0);
                if (text) result = std::string(text) + "\n" + result;
            }
            PQclear(res);
        }
    }
    return result;
}

std::string SQLiteDB::getSystemPromptVoice(const std::string& session_id, const std::string& user_id) {
    std::string result =
        "你是一个友好的AI语音助手，正在与用户进行实时语音通话。你扮演的角色名字叫「"+session_id+"」，请始终以这个名字的身份和口吻说话。\n"
        "【通话要求】\n"
        "1. 用口语化、自然、简洁的中文直接回答，像真人打电话一样一句一句地说，避免长篇大论。\n"
        "2. 严禁使用旁白、动作描写、括号说明等文字聊天格式；不要输出任何括号内容，只输出你口头说出来的话。\n"
        "3. 上下文中给出的历史对话和【历史摘要】是你与用户之前的文字聊天记忆，请自然地记得并延续其中的话题、人物关系和已发生的事，但不要逐字复述，也不要模仿其原有格式。\n"
        "4. 始终以角色的身份和口吻说话，不要替用户说话、不要替用户做决定。";

    ConnGuard g(this);
    if (g) {
        PGresult* res = execParams(g.get(),
            "SELECT system_prompt FROM chat_sessions WHERE session_id = $1 AND user_id = $2;",
            {session_id, user_id});
        if (res) {
            if (PQntuples(res) > 0) {
                const char* text = PQgetvalue(res, 0, 0);
                if (text) result = std::string(text) + "\n" + result;
            }
            PQclear(res);
        }
    }
    return result;
}

// 删除该会话的向量记忆与消息（先删 embedding，再删消息，避免违反外键约束）
void SQLiteDB::deleteMessages(const std::string& session_id, const std::string& user_id) {
    ConnGuard g(this);
    if (!g) return;
    deleteMessagesOn(g.get(), session_id, user_id);
}

void SQLiteDB::deleteOldMessages(const std::string& session_id, const std::string& user_id, const int keep_turns) {
    ConnGuard g(this);
    if (!g) return;
    pg_conn* c = g.get();

    int max_turn = getMaxTurnIdOn(c, session_id, user_id);
    if (max_turn <= keep_turns) return;

    int min_keep_turn = max_turn - keep_turns + 1;
    std::string mt = std::to_string(min_keep_turn);

    execParams(c,
        "DELETE FROM message_embeddings WHERE msg_id IN "
        "(SELECT msg_id FROM chat_messages WHERE session_id = $1 AND user_id = $2 AND turn_id < $3);",
        {session_id, user_id, mt});
    execParams(c,
        "DELETE FROM chat_messages WHERE session_id = $1 AND user_id = $2 AND turn_id < $3;",
        {session_id, user_id, mt});
}

void SQLiteDB::setSystemPrompt(const std::string& session_id, const std::string& user_id, const std::string& prompt) {
    ConnGuard g(this);
    if (!g) return;
    pg_conn* c = g.get();
    ensureSessionOn(c, session_id, user_id, "");
    execParams(c,
        "UPDATE chat_sessions SET system_prompt = $1 WHERE session_id = $2 AND user_id = $3;",
        {prompt, session_id, user_id});
}

std::map<std::string, std::string> SQLiteDB::getAllSessions(const std::string& user_id) {
    ConnGuard g(this);
    if (!g) return {};
    std::map<std::string, std::string> result;
    PGresult* res = execParams(g.get(),
        "SELECT session_id, system_prompt FROM chat_sessions WHERE user_id = $1;",
        {user_id});
    if (res) {
        for (int i = 0; i < PQntuples(res); ++i) {
            result[PQgetvalue(res, i, 0)] = PQgetvalue(res, i, 1);
        }
        PQclear(res);
    }
    return result;
}

std::string SQLiteDB::getSummary(const std::string& session_id, const std::string& user_id) {
    ConnGuard g(this);
    if (!g) return "";
    return queryString(g.get(),
        "SELECT summary FROM chat_sessions WHERE session_id = $1 AND user_id = $2;",
        {session_id, user_id}, "");
}

void SQLiteDB::setSummary(const std::string& session_id, const std::string& user_id, const std::string& summary) {
    ConnGuard g(this);
    if (!g) return;
    pg_conn* c = g.get();
    ensureSessionOn(c, session_id, user_id, "");
    execParams(c,
        "UPDATE chat_sessions SET summary = $1 WHERE session_id = $2 AND user_id = $3;",
        {summary, session_id, user_id});
}

void SQLiteDB::deleteSession(const std::string& session_id, const std::string& user_id) {
    ConnGuard g(this);
    if (!g) return;
    pg_conn* c = g.get();
    deleteMessagesOn(c, session_id, user_id);
    execParams(c,
        "DELETE FROM chat_sessions WHERE session_id = $1 AND user_id = $2;",
        {session_id, user_id});
}

std::pair<std::string, std::string> SQLiteDB::getLastMessageAndTime(const std::string& session_id, const std::string& user_id) {
    ConnGuard g(this);
    if (!g) return {"暂无消息", ""};
    std::pair<std::string, std::string> result = {"暂无消息", ""};
    const char* sql =
        "SELECT content, to_char(chat_messages.timestamp, 'YYYY-MM-DD HH24:MI:SS') "
        "FROM chat_messages WHERE session_id = $1 AND user_id = $2 "
        "ORDER BY turn_id DESC, msg_id DESC LIMIT 1;";
    PGresult* res = execParams(g.get(), sql, {session_id, user_id});
    if (res) {
        if (PQntuples(res) > 0) {
            result.first = PQgetvalue(res, 0, 0);
            result.second = PQgetvalue(res, 0, 1);
        }
        PQclear(res);
    }
    return result;
}
