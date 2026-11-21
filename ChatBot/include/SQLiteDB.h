#pragma once
#include "common.h"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <tuple>

struct pg_conn;

struct AppendedMessage {
    int msg_id = 0;
    int turn_id = 0;
};

struct SummaryMessage {
    int msg_id;
    std::string role;
    std::string content;
};

class SQLiteDB {
private:
    std::deque<pg_conn*> idle_;
    std::mutex mtx_;
    std::condition_variable cv_;
    int total_ = 0;
    int max_size_ = 10;
    bool shutting_down_ = false;
    std::string conninfo_;

    pg_conn* createConn();
    pg_conn* acquire();
    void release(pg_conn* c);
    class ConnGuard {
        SQLiteDB* db_;
        pg_conn* conn_;
    public:
        ConnGuard(SQLiteDB* db) : db_(db), conn_(db ? db->acquire() : nullptr) {}
        ~ConnGuard() { if (db_ && conn_) db_->release(conn_); }
        ConnGuard(const ConnGuard&) = delete;
        ConnGuard& operator=(const ConnGuard&) = delete;
        pg_conn* get() const { return conn_; }
        explicit operator bool() const { return conn_ != nullptr; }
    };

public:
    SQLiteDB(const std::string& path = "chatbot.db");
    ~SQLiteDB();

    bool open();
    void close();
    void createTables();
    void seedDefaultMemes();
    std::string getRandomMemeByTag(const std::string& tag);
    std::vector<json> getAllMemes();
    bool executeSQL(const std::string& sql);
    void ensureSession(const std::string& session_id, const std::string& user_id,
        const std::string& voice_id);
    std::string getSessionVoiceId(const std::string& session_id, const std::string& user_id);
    int getMaxTurnId(const std::string& session_id, const std::string& user_id);
    std::vector<std::tuple<std::string, std::string, std::string>> getRecentMessages(const std::string& session_id,
        const std::string& user_id, int limit);
    int saveMessage(const std::string& session_id, const std::string& user_id, const std::string& role,
        const std::string& content, int turn_id, const std::string& image_url = "");
    AppendedMessage appendMessage(const std::string& session_id, const std::string& user_id,
        const std::string& role, const std::string& content, const std::string& image_url = "",
        int turn_id = 0);
    std::vector<SummaryMessage> getMessagesToSummarize(const std::string& session_id,
        const std::string& user_id, int keep_turns, int limit);
    bool commitSummary(const std::string& session_id, const std::string& user_id,
        const std::string& expected_summary, const std::string& new_summary,
        const std::vector<int>& message_ids);
    std::string getSystemPrompt(const std::string& session_id, const std::string& user_id);
    std::string getSystemPromptVoice(const std::string& session_id, const std::string& user_id);
    void deleteMessages(const std::string& session_id, const std::string& user_id);
    void deleteOldMessages(const std::string& session_id, const std::string& user_id, const int keep_turns);
    void setSystemPrompt(const std::string& session_id, const std::string& user_id, const std::string& prompt);
    std::map<std::string, std::string> getAllSessions(const std::string& user_id);
    std::string getSummary(const std::string& session_id, const std::string& user_id);
    void saveEmbedding(int msg_id, const std::string& session_id, const std::string& user_id,
        const std::vector<float>& embedding);
    std::vector<std::pair<int, std::string>> searchSimilar(const std::string& session_id,
        const std::string& user_id, const std::vector<float>& query_embedding, int top_k = 3);
    void setSummary(const std::string& session_id, const std::string& user_id, const std::string& summary);
    int addMeme(const std::string& file_path, const std::string& tags, const std::string& description,
        const std::string& text_on_image, const std::string& source = "builtin");
    void deleteSession(const std::string& session_id, const std::string& user_id);
    std::pair<std::string, std::string> getLastMessageAndTime(const std::string& session_id,
        const std::string& user_id);
};
