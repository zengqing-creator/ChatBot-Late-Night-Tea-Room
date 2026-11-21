#include "Conversation.h"
#include "SQLiteDB.h"
#include "utils.h"

Conversation::Conversation() : messages(json::array()), session_id(""), summary(""), db(nullptr) {}

void Conversation::setDisplayName(const std::string& name) { 
    display_name = name; 
}

void Conversation::initDB(std::shared_ptr<SQLiteDB> database, const std::string& sid, const std::string& uid, const std::string& vid) {
    db = database;
    session_id = sid;
    user_id = uid;
    db->ensureSession(session_id, user_id, vid);
    summary = db->getSummary(session_id, user_id);

    if(messages.empty()){
        messages = json::array();
    }

    loadSystemPrompt();
    loadFromDatabase();
}

void Conversation::loadSystemPrompt() {
    if(!db) return;

    std::string prompt = voice_mode_ ? db->getSystemPromptVoice(session_id, user_id)
                                     : db->getSystemPrompt(session_id, user_id);
    if(!summary.empty())prompt += "\n\n【历史摘要】\n" + summary;

    int system_index = -1;
    for (size_t i = 0; i < messages.size(); i++) {
        if (messages[i]["role"] == "system") {
            system_index = i;
            break;
        }
    }
    
    if (system_index >= 0)
        messages[system_index]["content"] = prompt;
    else {
        json system_msg;
        system_msg["role"] = "system";
        system_msg["content"] = prompt;
        if (messages.empty())
            messages = json::array({system_msg});
        else
            messages.insert(messages.begin(), system_msg);
    }
}

void Conversation::trim_messages() {
    if ((int)messages.size() <= MAX_MESSAGES) return;

    int remove_count = (int)messages.size() - MAX_MESSAGES;
    messages.erase(messages.begin() + 1, messages.begin() + remove_count + 1);
}

void Conversation::maybeSummarize() {
    if (!db || !persist_) return;
    auto old = db->getMessagesToSummarize(session_id, user_id, MAX_TURNS, 20);
    if ((int)old.size() < summary_batch) return;

    json to_summarize = json::array();
    for (const auto& msg : old)
        to_summarize.push_back({{"role", msg.role}, {"content", msg.content}});

    std::string previous_summary = db->getSummary(session_id, user_id);
    std::string new_summary = generateSummary(to_summarize, previous_summary);
    if (new_summary.empty()) return; // 保留原消息供下次重试
    std::vector<int> ids;
    ids.reserve(old.size());
    for (const auto& msg : old) ids.push_back(msg.msg_id);
    if (db->commitSummary(session_id, user_id, previous_summary, new_summary, ids)) {
        summary = std::move(new_summary);
        loadSystemPrompt();
    }
}

void Conversation::loadFromDatabase() {
    if (!db) return;
    auto history = db->getRecentMessages(session_id, user_id, MAX_TURNS * 2);
    for (const auto& [role, content, image_url] : history) {
        std::string c = content;
        // 语音通话需要文字聊天的记忆，但要去掉旁白格式，只留口语对话，避免模型模仿旁白
        if (voice_mode_ && role == "assistant") {
            c = stripNarration(c);
        }
        (void)image_url;   // 图片 URL 不进 LLM 上下文，仅用于前端回显
        messages.push_back({{"role", role}, {"content", c}});
    }
    std::cout << "[系统] 已从数据库加载 " << history.size() << " 条历史消息" << std::endl;
}

void Conversation::add_user_message(const std::string& content, const std::string& image_url) {
    messages.push_back({{"role", "user"}, {"content", content}});
    trim_messages();
    if (db && persist_) {
        auto saved = db->appendMessage(session_id, user_id, "user", content, image_url);
        pending_user_msg_id = saved.msg_id;
        pending_turn_id = saved.turn_id;
    }
}

void Conversation::flush_user_embeddings(const std::vector<float>& emb) {
    if (db && pending_user_msg_id > 0 && !emb.empty())
        db->saveEmbedding(pending_user_msg_id, session_id, user_id, emb);
    pending_user_msg_id = -1;
}


void Conversation::add_assistant_message(const std::string& content, const std::string& image_url) {
    messages.push_back({{"role", "assistant"}, {"content", content}});
    trim_messages();
    if (db && persist_) {
        auto saved = db->appendMessage(session_id, user_id, "assistant", content,
                                        image_url, pending_turn_id);
        // 向量化放到后台线程，避免阻塞聊天响应
        if (saved.msg_id > 0) {
            enqueue_embedding_job(db, saved.msg_id, session_id, user_id, content);
            maybeSummarize();
        }
    }
}

json Conversation::get_messages() const {
    return messages;
}

std::string Conversation::generateSummary(const json& old_messages, const std::string& old_summary) {
    std::string prompt = "请根据先前摘要和最新对话，生成一份详细完整的新摘要（不少于200字），包括主要事件、人物情感变化和关键对话。\n";
    if(!old_summary.empty())
        prompt += "先前摘要: " + old_summary + "\n";
    prompt += "最新对话:\n";

    const int MAX_SUMMARY_MESSAGES = 20;
    int count = 0;
    for(size_t i = 0; i < old_messages.size() && count < MAX_SUMMARY_MESSAGES; ++i, ++count) {
        const auto& msg = old_messages[i];
        std::string role = msg["role"];
        std::string content = msg["content"].get<std::string>();
        if (role == "user")
            prompt += "用户: " + content + "\n";
        else if (role == "assistant")
            prompt += display_name + "：" + content + "\n";
    }
    return call_llm_sync(prompt);
}

std::string Conversation::getSessionId() const {
    return session_id;
}
