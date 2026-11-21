#include "common.h"
#include "SQLiteDB.h"
#include "Conversation.h"
#include "utils.h"
#include "RedisClient.h"
#include <fstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <regex>

// base64 解码
static std::string base64_decode(const std::string& in) {
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int T[256];
    for (int i = 0; i < 256; i++) T[i] = -1;
    for (int i = 0; i < 64; i++) T[(unsigned char)chars[i]] = i;
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back((char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// Redis 客户端：优先走 Redis 做跨进程限流，Redis 不可用时自动降级到内存
static RedisClient g_redis;

// 原有内存滑动窗口限流（降级路径）
static bool rate_limited_memory(const std::string& key, int limit, int window_seconds) {
    static std::mutex mtx;
    static std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> hits;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mtx);

    const auto window = std::chrono::seconds(window_seconds);
    // 超阈值时清扫已过期的空条目，防止内存缓慢泄漏
    if (hits.size() > 4096) {
        for (auto it = hits.begin(); it != hits.end(); ) {
            while (!it->second.empty() && now - it->second.front() > window) it->second.pop_front();
            if (it->second.empty()) it = hits.erase(it); else ++it;
        }
    }

    auto& q = hits[key];
    while (!q.empty() && now - q.front() > window) q.pop_front();
    if ((int)q.size() >= limit) return true;
    q.push_back(now);
    return false;
}

// 滑动窗口限流：优先走 Redis，Redis 不可用时降级到本地内存
static bool rate_limited(const std::string& key, int limit, int window_seconds) {
    int r = g_redis.rateLimit(key, limit, window_seconds);
    if (r == 1) return false;   // Redis：允许
    if (r == 0) return true;    // Redis：超限
    return rate_limited_memory(key, limit, window_seconds); // r == -1：降级
}

// 读取环境变量
static const std::string API_TOKEN = [] {
    const char* v = std::getenv("CHATBOT_API_TOKEN");
    return v ? v : "";
}();

// 校验 HTTP 请求头 Authorization: Bearer <token>
static bool authorized(const httplib::Request& req) {
    if (API_TOKEN.empty()) return true;
    return req.get_header_value("Authorization") == ("Bearer " + API_TOKEN);
}

// 通过文件魔数判断图片真实类型，返回扩展名；无法识别返回空串
static std::string detect_image_ext(const std::string& d) {
    if (d.size() >= 3 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8 && (unsigned char)d[2] == 0xFF)
        return "jpg";
    if (d.size() >= 8 && (unsigned char)d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' &&
        (unsigned char)d[4] == 0x0D && (unsigned char)d[5] == 0x0A && (unsigned char)d[6] == 0x1A && (unsigned char)d[7] == 0x0A)
        return "png";
    if (d.size() >= 6 && (d.compare(0, 6, "GIF87a") == 0 || d.compare(0, 6, "GIF89a") == 0))
        return "gif";
    if (d.size() >= 12 && d.compare(0, 4, "RIFF") == 0 && d.compare(8, 4, "WEBP") == 0)
        return "webp";
    return "";
}

// 避免同一会话短时间内连发表情包
static std::mutex meme_cool_mtx;
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_meme_time;

// 尝试按标签发送表情，返回图片 URL；带冷却与日志，失败返回空串
static std::string try_send_meme(const std::shared_ptr<SQLiteDB>& db, const std::string& tag, const std::string& sid) {
    if (!sid.empty()) {
        std::lock_guard<std::mutex> lock(meme_cool_mtx);
        auto now = std::chrono::steady_clock::now();
        auto it = last_meme_time.find(sid);
        if (it != last_meme_time.end() && now - it->second < std::chrono::seconds(30)) {
            std::cout << "[send_meme] 冷却中，跳过 (session=" << sid << ", tag=" << tag << ")" << std::endl;
            return "";
        }
    }
    std::string url = db->getRandomMemeByTag(tag);
    if (url.empty()) {
        std::cout << "[send_meme] 标签未命中，未发送 (tag=" << tag << ")" << std::endl;
        return "";
    }
    if (!sid.empty()) {
        std::lock_guard<std::mutex> lock(meme_cool_mtx);
        last_meme_time[sid] = std::chrono::steady_clock::now();
    }
    std::cout << "[send_meme] 发送表情: " << url << " (tag=" << tag << ", session=" << sid << ")" << std::endl;
    return url;
}

static const std::regex TEXTUAL_MEME_RE(
    R"(\(\s*send_meme\b[^)]*?tag\s*=\s*["']([^"']+)["'][^)]*\))",
    std::regex::icase);

// 把全角括号/引号归一化为半角，使上面的纯 ASCII 正则能覆盖全角变体
static std::string normalize_punct(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xEF && i + 2 < s.size() && (unsigned char)s[i + 1] == 0xBC) {
            unsigned char c2 = (unsigned char)s[i + 2];
            if (c2 == 0x88) { out += '('; i += 3; continue; } // （
            if (c2 == 0x89) { out += ')'; i += 3; continue; } // ）
        }
        if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80) {
            unsigned char c2 = (unsigned char)s[i + 2];
            if (c2 == 0x9C || c2 == 0x9D) { out += '"'; i += 3; continue; } // “ ”
            if (c2 == 0x98 || c2 == 0x99) { out += '\''; i += 3; continue; } // ‘ ’
        }
        out += s[i++];
    }
    return out;
}

// SSE 流式聊天的共享状态
struct ChatStreamState {
    std::string full_response;
    std::string image_url;
};

int main() {
    try {
        SetConsoleOutputCP(CP_UTF8);

        // 初始化数据库
        auto db = std::make_shared<SQLiteDB>("chatbot.db");
        httplib::Server svr;
        // 限制请求体大小
        svr.set_payload_max_length(10 * 1024 * 1024); // 10 MB

        svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
            if (req.path.rfind("/api/", 0) != 0) return httplib::Server::HandlerResponse::Unhandled;
            if (authorized(req)) return httplib::Server::HandlerResponse::Unhandled;
            res.status = 401;
            res.set_content(R"({"error":"未授权，请提供有效的访问令牌"})", "application/json; charset=utf-8");
            return httplib::Server::HandlerResponse::Handled;
        });
        if (API_TOKEN.empty()) {
            std::cerr << "[警告] 未设置 CHATBOT_API_TOKEN，API 未启用鉴权（局域网内任何设备都可访问）" << std::endl;
        }

        // 注册 send_meme 工具实现
        tool_functions["send_meme"] = [&db](const json& args) -> std::string {
            return try_send_meme(db, args.value("tag", ""), args.value("__session_id", ""));
        };

        // 1. 路由：提供前端单页应用 (index.html)
        svr.Get("/", [](const httplib::Request& req, httplib::Response& res) {
            std::ifstream file("index.html");
            if (!file.is_open()) {
                res.status = 404;
                res.set_content("错误：未找到 index.html 文件！请确保它放置在可执行文件同级目录。", "text/plain; charset=utf-8");
                return;
            }
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            res.set_content(content, "text/html; charset=utf-8");
        });

        // 2. API：获取所有会话列表及最后一条消息
        svr.Get("/api/sessions", [&db](const httplib::Request& req, httplib::Response& res) {
            const std::string user_id = "local_user";
            try {
                auto sessions = db->getAllSessions(user_id);
                json j_sessions = json::array();
                
                for (const auto& [id, prompt] : sessions) {
                    auto [last_msg, timestamp] = db->getLastMessageAndTime(id, user_id);    
                    j_sessions.push_back({
                        {"id", id},
                        {"prompt", prompt},
                        {"last_message", last_msg},
                        {"timestamp", timestamp}
                    });
                }

                res.set_content(j_sessions.dump(-1,' ',false,nlohmann::json::error_handler_t::replace), 
                "application/json; charset=utf-8");
            } catch (const std::exception& e) {
                std::cerr << "[后端报错]获取会话列表失败：" << e.what() << std::endl;
                res.status = 500;
                res.set_content("[]", "application/json; charset=utf-8");
            }
        });

        // 3. API：创建新会话
        svr.Post("/api/sessions", [&db](const httplib::Request& req, httplib::Response& res) {
            const std::string user_id = "local_user";
            try {
                auto body = json::parse(req.body);
                std::string sid = body["session_id"];
                std::string vid = body.value("voice_id", ""); // 接收音色，没传就是空
                
                db->ensureSession(sid, user_id, vid);
                res.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(e.what(), "text/plain");
            }
        });

        // 4. API：获取历史记录
        svr.Get("/api/history", [&db](const httplib::Request& req, httplib::Response& res) {
            const std::string user_id = "local_user";
            
            if (!req.has_param("session_id")) {
                res.status = 400;
                return;
            }
            std::string sid = req.get_param_value("session_id");
            
            auto history = db->getRecentMessages(sid, user_id, 50); // 拉取最近50条
            json j_history = json::array();
            for (const auto& [role, content, image_url] : history) {
                json msg = {{"role", role}, {"content", content}};
                if (!image_url.empty()) msg["image_url"] = image_url;
                j_history.push_back(msg);
            }
            res.set_content(j_history.dump(), "application/json; charset=utf-8");
        });

        // 5. API：获取摘要内容
        svr.Get("/api/summary", [&db](const httplib::Request& req, httplib::Response& res) {
            const std::string user_id = "local_user";
            if (!req.has_param("session_id")) {
                res.status = 400;
                return;
            }
            std::string sid = req.get_param_value("session_id");
            std::string summary = db->getSummary(sid, user_id);
            json response = {{"summary", summary}};
            res.set_content(response.dump(), "application/json; charset=utf-8");
        });

        // 6. API：处理聊天信息
        svr.Post("/api/chat", [&db](const httplib::Request& req, httplib::Response& res) {
            const std::string user_id = "local_user";

            if (rate_limited("chat:" + req.remote_addr, 30, 60)) {
                res.status = 429;
                res.set_content(R"({"error":"请求过于频繁，请稍后再试"})", "application/json; charset=utf-8");
                return;
            }
            
            std::string sid, msg, user_image_url;
            try {
                auto body = json::parse(req.body);
                sid = body["session_id"];
                msg = body["message"];
                user_image_url = body.value("image_url", "");
            } catch (const std::exception&) {
                res.status = 400;
                res.set_content(R"({"error":"invalid request body"})", "application/json; charset=utf-8");
                return;
            }

            auto state = std::make_shared<ChatStreamState>();
            res.set_header("Cache-Control", "no-cache");
            res.set_chunked_content_provider("text/event-stream",
                [db, user_id, sid, msg, user_image_url, state](size_t /*offset*/, httplib::DataSink& sink) {
                    auto emit = [&sink](const std::string& payload) {
                        std::string frame = "data: " + payload + "\n\n";
                        return sink.write(frame.data(), frame.size());
                    };
                    auto emit_content = [&emit](const std::string& c) {
                        emit(json{{"content", c}}.dump());
                    };

                    try {
                        Conversation conv;
                        conv.initDB(db, sid, user_id, "");
                        conv.setDisplayName(sid);
                        conv.add_user_message(msg, user_image_url);

                        std::string ret_msg = call_ai_with_tools(conv,
                            [&](const std::string& c) {
                                state->full_response += c;
                                emit_content(c);
                            },
                            &state->image_url);

                        if (state->full_response.empty() && !ret_msg.empty()) {
                            state->full_response = ret_msg;
                            emit_content(ret_msg);
                        }

                        if (state->image_url.empty()) {
                            std::string normalized = normalize_punct(state->full_response);
                            std::smatch m;
                            if (std::regex_search(normalized, m, TEXTUAL_MEME_RE)) {
                                std::string url = try_send_meme(db, m[1].str(), sid);
                                if (!url.empty()) state->image_url = url;
                                state->full_response = std::regex_replace(normalized, TEXTUAL_MEME_RE, "");
                                emit(json{{"final_content", state->full_response}}.dump());
                            }
                        }

                        if (!state->full_response.empty()) {
                            conv.add_assistant_message(state->full_response, state->image_url);
                        }

                        emit(json{{"image_url", state->image_url}}.dump());
                        emit("[DONE]");
                    } catch (const std::exception& e) {
                        emit(json{{"error", std::string("Internal Error: ") + e.what()}}.dump());
                    }
                    sink.done();
                    return true;
                });
        });

        // 7. API：修改人设
        svr.Post("/api/personality", [&db](const httplib::Request& req, httplib::Response& res) {
            const std::string user_id = "local_user";
            try {
                auto body = json::parse(req.body);
                std::string sid = body["session_id"];
                std::string prompt = body["prompt"];
                
                db->setSystemPrompt(sid, user_id, prompt);
                res.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (...) {
                res.status = 400;
            }
        });

        // 8. API：清空记忆
        svr.Post("/api/clear", [&db](const httplib::Request& req, httplib::Response& res) {
            const std::string user_id = "local_user";

            try {
                auto body = json::parse(req.body);
                std::string sid = body["session_id"];
                
                db->deleteMessages(sid, user_id);
                db->setSummary(sid, user_id, "");
                res.set_content("{\"status\":\"ok\"}", "application/json; charset=utf-8");
            } catch (...) {
                res.status = 400;
            }
        });

        // 9. API：删除角色及其所有信息
        svr.Post("/api/delete_session", [&db](const httplib::Request& req, httplib::Response& res) {
            const std::string user_id = "local_user";

            try {
                auto body = json::parse(req.body);
                std::string sid = body["session_id"];
                
                db->deleteSession(sid, user_id);
                res.set_content("{\"status\":\"ok\"}", "application/json; charset=utf-8");
            } catch (...) {
                res.status = 400;
                res.set_content("{\"status\":\"error\"}", "application/json; charset=utf-8");
            }
        });

        // 10. API：合成语音
        svr.Post("/api/tts", [&db](const httplib::Request& req, httplib::Response& res) {
            const std::string user_id = "local_user";

            if (rate_limited("tts:" + req.remote_addr, 30, 60)) {
                res.status = 429;
                res.set_content(R"({"error":"请求过于频繁，请稍后再试"})", "application/json; charset=utf-8");
                return;
            }
            try {
                auto body = json::parse(req.body);
                std::string text = body.value("text", "");
                std::string sid = body.value("session_id", "");

                if (text.empty()) {
                    res.status = 400;
                    res.set_content("{\"error\":\"Missing text parameter\"}", "application/json");
                    return;
                }

                std::string vid = VOICE_ID;
                if (!sid.empty()) {
                    std::string custom_vid = db->getSessionVoiceId(sid, user_id);
                    if (!custom_vid.empty()) vid = custom_vid;
                }

                std::string audio_url = synthesize_speech(API_KEY, vid, text);
                if (audio_url.empty()) {
                    res.status = 500;
                    res.set_content("{\"error\":\"TTS synthesis failed\"}", "application/json");
                    return;
                }

                json response = {{"audio_url", audio_url}};
                res.set_content(response.dump(), "application/json; charset=utf-8");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(std::string("Error: ") + e.what(), "text/plain");
            }
        });

        // 11. API：表情互动
        svr.Get(R"(/memes/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
            std::string name = req.matches[1];
            // 防路径穿越：拒绝上级目录、路径分隔符、盘符/ADS 等非法字符
            if (name.empty()
                || name.find("..") != std::string::npos
                || name.find('\\') != std::string::npos
                || name.find('/') != std::string::npos
                || name.find(':') != std::string::npos) {
                res.status = 400;
                return;
            }

            // 扩展名白名单（转小写比较），只允许图片
            size_t dot = name.find_last_of('.');
            if (dot == std::string::npos) { res.status = 404; return; }
            std::string ext = name.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            static const std::vector<std::string> allowed_exts = {"jpg", "jpeg", "png", "gif", "webp"};
            if (std::find(allowed_exts.begin(), allowed_exts.end(), ext) == allowed_exts.end()) {
                res.status = 404;
                return;
            }

            std::ifstream f("memes/" + name, std::ios::binary);
            if (!f) { res.status = 404; return; }
            std::string body((std::istreambuf_iterator<char>(f)), {});
            std::string mime = (ext == "png") ? "image/png"
                            : (ext == "gif") ? "image/gif"
                            : (ext == "webp") ? "image/webp" : "image/jpeg";
            res.set_content(body, mime.c_str());
        });

        // 12. API：获取所有表情
        svr.Get("/api/memes", [&db](const httplib::Request&, httplib::Response& res) {
            res.set_content(json(db->getAllMemes()).dump(), "application/json; charset=utf-8");
        });

        // 13. API：添加表情
        svr.Post("/api/memes", [&db](const httplib::Request& req, httplib::Response& res) {
            try {
                auto b = json::parse(req.body);
                if (!b.contains("file_path") || !b.contains("tags")) {
                    res.status = 400;
                    res.set_content(R"({"status":"error","message":"missing file_path or tags"})", "application/json; charset=utf-8");
                    return;
                }
                db->addMeme(b.value("file_path", ""), b.value("tags", ""), b.value("description", ""),
                            b.value("text_on_image", ""), b.value("source", "builtin"));
                res.set_content(R"({"status":"ok"})", "application/json");
            } catch (const std::exception&) {
                res.status = 400;
                res.set_content(R"({"status":"error","message":"invalid request body"})", "application/json; charset=utf-8");
            }
        });

        // 14. API：上传图片并识图（VL，阶段二）
        svr.Post("/api/upload_image", [&db](const httplib::Request& req, httplib::Response& res) {
            if (rate_limited("upload:" + req.remote_addr, 10, 60)) {
                res.status = 429;
                res.set_content(R"({"error":"请求过于频繁，请稍后再试"})", "application/json; charset=utf-8");
                return;
            }
            try {
                auto body = json::parse(req.body);
                std::string data_url = body.value("data_url", "");
                if (data_url.empty()) {
                    res.status = 400;
                    res.set_content(R"({"error":"missing data_url"})", "application/json");
                    return;
                }

                // 解析 data URL
                std::string base64_part = data_url;
                size_t comma = data_url.find(',');
                if (comma != std::string::npos)
                    base64_part = data_url.substr(comma + 1);

                std::string decoded = base64_decode(base64_part);
                if (decoded.empty()) {
                    res.status = 400;
                    res.set_content(R"({"error":"图片数据为空"})", "application/json; charset=utf-8");
                    return;
                }

                std::string ext = detect_image_ext(decoded);
                if (ext.empty()) {
                    res.status = 400;
                    res.set_content(R"({"error":"文件不是有效的图片"})", "application/json; charset=utf-8");
                    return;
                }

                // 保存图片到 memes/
                CreateDirectoryA("memes", nullptr);
                std::string filename = "upload_" +
                    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()) + "." + ext;
                std::ofstream out("memes/" + filename, std::ios::binary);
                out.write(decoded.data(), decoded.size());
                out.close();
                if (!out) {
                    res.status = 500;
                    res.set_content(R"({"error":"图片保存失败，请重试"})", "application/json; charset=utf-8");
                    return;
                }

                // VL 识图
                std::string raw = call_vision(data_url);
                std::string description = raw;
                json v;
                try { v = json::parse(raw); } catch (...) {}
                if (!v.is_null() && v.contains("description"))
                    description = v["description"].get<std::string>();

                // 录入素材库
                std::string tags;
                std::string text_on_image;
                if (!v.is_null()) {
                    if (v.contains("emotion") && v["emotion"].is_string())
                        tags = v["emotion"].get<std::string>();
                    if (v.contains("keywords") && v["keywords"].is_array()) {
                        for (auto& kw : v["keywords"]) {
                            if (!kw.is_string()) continue;
                            std::string k = kw.get<std::string>();
                            if (!k.empty() && tags.find(k) == std::string::npos)
                                tags += (tags.empty() ? "" : ",") + k;
                        }
                    }
                    if (v.contains("text_on_image") && v["text_on_image"].is_string())
                        text_on_image = v["text_on_image"].get<std::string>();
                }
                if (tags.empty()) tags = "开心";
                db->addMeme(filename, tags, description, text_on_image, "upload");

                json resp = {
                    {"description", description},
                    {"file_path", "/memes/" + filename},
                    {"tags", tags}
                };
                if (!v.is_null()) {
                    if (v.contains("emotion")) resp["emotion"] = v["emotion"];
                    if (v.contains("keywords")) resp["keywords"] = v["keywords"];
                    if (v.contains("text_on_image")) resp["text_on_image"] = v["text_on_image"];
                }
                res.set_content(resp.dump(), "application/json; charset=utf-8");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(std::string("Error: ") + e.what(), "text/plain");
            }
        });

        std::cout << " AI Web Server 已启动！" << std::endl;
        std::cout << " 局域网访问地址: http://<你的局域网IP>:8080" << std::endl;

        // 语音通话线程
        std::thread voice_thread([db]() {
            httplib::Server ws_svr;

            ws_svr.WebSocket("/voice", [db](const httplib::Request& req, httplib::ws::WebSocket& ws) {
                if (!API_TOKEN.empty() && req.get_param_value("token") != API_TOKEN) return; // 鉴权失败，直接断开
                std::string msg;
                const std::string user_id = "local_user";

                // 通话上下文：整个通话复用同一个 Conversation，通话内容不进库
                Conversation conv;
                conv.setPersist(false);
                std::string conv_sid;

                while (ws.is_open()) {
                    auto result = ws.read(msg);
                    if (result == httplib::ws::ReadResult::Fail) {
                        std::cout << "[语音通话] 连接关闭" << std::endl;
                        break;
                    }
                    if (result != httplib::ws::ReadResult::Text) continue;

                    try {
                        json body = json::parse(msg);
                        std::string type = body.value("type", "");

                        if (type == "chat") {
                            if (rate_limited("voice:" + req.remote_addr, 30, 60)) {
                                ws.send(json({{"type","error"},{"message","请求过于频繁，请稍后再试"}}).dump());
                                continue;
                            }
                            std::string sid = body.value("session_id", "");
                            std::string user_text = body.value("text", "");
                            if (sid.empty() || user_text.empty()) {
                                ws.send(json({{"type","error"},{"message","missing session_id or text"}}).dump());
                                continue;
                            }

                            std::cout << "[语音通话] 用户: " << user_text << std::endl;

                            if (conv_sid != sid) {
                                conv = Conversation();
                                conv.setPersist(false);
                                conv.setVoiceMode(true);
                                conv.initDB(db, sid, user_id, "");
                                conv.setDisplayName(sid);
                                conv_sid = sid;
                            }

                            conv.add_user_message(user_text);

                            std::string reply = call_ai_with_tools(conv, nullptr);
                            if (reply.empty()) reply = "无响应内容";

                            conv.add_assistant_message(reply);

                            // 合成语音
                            std::cout << "[语音通话] AI: " << reply << std::endl;
                            std::string vid = VOICE_ID;
                            std::string custom_vid = db->getSessionVoiceId(sid, user_id);
                            if (!custom_vid.empty()) vid = custom_vid;
                            std::string audio_url = synthesize_speech(API_KEY, vid, reply);
                            json response = {
                                {"type", "reply"},
                                {"text", reply},
                                {"audio_url", audio_url}
                            };
                            ws.send(response.dump());
                        }
                        else if (type == "ping") {
                            ws.send(json({{"type","pong"}}).dump());
                        }
                    } catch (const std::exception& e) {
                        ws.send(json({{"type","error"},{"message", e.what()}}).dump());
                    }
                }
            });

            std::cout << " 语音通话服务已启动: ws://0.0.0.0:8081/voice" << std::endl;
            ws_svr.listen("0.0.0.0", 8081);
        });
        voice_thread.detach();


        // 监听 0.0.0.0 以允许局域网设备访问，端口设为 8080
        svr.listen("0.0.0.0", 8080);

        // 服务器停止后优雅关闭后台向量化线程
        shutdown_embedding_worker();

    } catch (const std::exception& e) {
        std::cerr << "致命错误: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}