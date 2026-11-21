# ChatBot 多模态聊天助手

> 仓库只保存示例配置。首次运行请将 `ChatBot/config.local.example.bat` 复制为 `config.local.bat`，将 `ChatBot/config.runtime.example.bat` 复制为 `config.runtime.bat`，在副本中填写本机依赖路径、数据库地址及凭据。两个本地配置文件均被 Git 忽略，请勿把真实密钥写入公开脚本或提交构建产物。

> 基于 C++ 和 Web 前端的多模态聊天应用，支持多会话管理、角色人设定制、长期记忆、语音合成与实时语音通话。

## 一、功能特性

1. 多会话管理：创建、切换、删除独立聊天会话。
2. AI 对话：基于阿里云百炼大模型，支持流式回复与工具调用。
3. 人设系统：自定义 AI 角色 Prompt（角色扮演 / 剧情叙事），保存至数据库。
4. 长期记忆：基于向量检索的历史消息摘要与语义搜索。
5. 语音合成：将 AI 回复转为语音播放。
6. 实时语音通话：浏览器语音识别 + WebSocket + TTS 实现对话。
7. 云端存储：PostgreSQL（RDS，含 pgvector）保存会话与消息，Redis（DCS）做限流。
8. 表情包互动：AI 可在对话中自动发表情包，前端支持表情包选择器与本地录入。
9. 多模态识图：上传图片自动识别情绪 / 关键词 / 配文并录入素材库。

## 二、技术栈

| 层级 | 技术 |
| --- | --- |
| 后端 | C++17、cpp-httplib（含 SSL）、PostgreSQL(libpq + pgvector)、hiredis、nlohmann/json、OpenSSL |
| 前端 | Vue 3（CDN）、TailwindCSS（CDN） |
| AI 服务 | 阿里云百炼 DashScope API（对话 qwen-plus / 视觉 qwen-vl-plus / 向量 / TTS） |

## 三、项目技术详解

### （1）整体架构

```text
前端
index.html（Vue3 + TailwindCSS）：会话列表、聊天窗口、输入框等；Web Speech API 语音识别；Audio API 播放 TTS
        │
        │ HTTP(8080)、WebSocket(8081)
        ▼
C++ 后端
HTTP Server（httplib）                 WebSocket Server（httplib，独立线程）
        │                                       │
        ▼                                       ▼
Conversation.cpp（会话上下文）          messages（JSON 数组）、system_prompt / summary
        │
        ▼
SQLiteDB.cpp（chat_sessions、chat_messages、message_embeddings、meme_library）
        │
        ▼
utils.cpp：call_ai_with_tools（LLM 对话）、call_vision（识图）、get_embedding（向量化）、synthesize_speech（TTS）
        │ HTTPS
        ▼
阿里云百炼 DashScope API
  - Chat Completions（qwen-plus）
  - Vision 识图（qwen-vl-plus）
  - Embeddings（text-embedding-v4）
  - TTS（qwen-audio-3.1-tts-flash）
```

### （2）目录结构

```text
ICT/                         # 项目根目录
├── README.md
└── ChatBot/                   # C++ 项目本体
    ├── CMakeLists.txt         # 构建配置（find_package 定位 PostgreSQL / hiredis / OpenSSL）
    ├── start.bat              # 一键启动脚本（编译 + 启动 exe）
    ├── Clone and TTS.py       # 声音复刻脚本（上传 OSS 音频后填入 URL，运行得到 voice_id）
    ├── include/               # 头文件（第三方库 + 自有模块声明）
    │   ├── common.h           # 公共头（平台宏 + 依赖）
    │   ├── Conversation.h     # 会话 / 记忆压缩接口（实现见 src/Conversation.cpp）
    │   ├── SQLiteDB.h         # 数据库 + 向量检索接口（PostgreSQL 连接池，实现见 src/SQLiteDB.cpp）
    │   ├── utils.h            # LLM 调用 / embedding / TTS 接口（实现见 src/utils.cpp）
    │   ├── RedisClient.h      # Redis 限流客户端（基于 hiredis）
    │   ├── httplib.h          # cpp-httplib（HTTP / HTTPS / WebSocket）
    │   └── json.hpp           # nlohmann/json
    ├── src/
    │   ├── main.cpp           # 服务入口 + HTTP 路由 + WebSocket 语音服务
    │   ├── index.html         # 前端单页应用（Vue3 + TailwindCSS）
    │   ├── Conversation.cpp   # 会话 / 记忆压缩实现
    │   ├── SQLiteDB.cpp       # 数据库 + 向量检索实现（PostgreSQL / libpq / pgvector + 连接池）
    │   ├── RedisClient.cpp    # Redis 限流实现（hiredis）
    │   └── utils.cpp          # LLM / embedding / TTS 实现
    ├── memes/                 # 内置表情包素材（启动时同步到 build 输出目录并入库）
    ├── scripts/               # 辅助脚本
    │   ├── seed_memes.py      # 表情包入库脚本（幂等，扫描 memes/ 注册到 meme_library）
    │   └── meme_manifest.json # 表情包的标签 / 描述 / 配文清单
    ├── build/                 # CMake 构建产物（含 bin/Release/ChatBot.exe，脚本自动生成）
    └── venv/                  # Python 虚拟环境（供 Clone and TTS.py）
```

### （3）HTTP 服务层

选用 `httplib.h` 实现 HTTP/HTTPS 功能和跨平台支持，同时支持 SSL 的内置流式响应。

路由设计如下：

| 方法 | 路径 | 职责 |
| --- | --- | --- |
| GET | `/` | 前端入口，返回 `index.html` |
| GET | `/api/sessions` | 列出所有会话 |
| POST | `/api/sessions` | 创建新会话 |
| GET | `/api/history` | 拉取会话历史（最近 50 条） |
| GET | `/api/summary` | 拉取会话摘要 |
| POST | `/api/chat` | 处理聊天消息 |
| POST | `/api/personality` | 修改人设 |
| POST | `/api/clear` | 清空记忆 |
| POST | `/api/delete_session` | 删除会话 |
| POST | `/api/tts` | 语音合成 |
| GET | `/memes/<name>` | 表情包静态资源 |
| GET | `/api/memes` | 列出所有表情包 |
| POST | `/api/memes` | 录入表情包到素材库 |
| POST | `/api/upload_image` | 上传图片并识图（VL），自动录入素材库 |

### （4）数据库层

1. 选用 PostgreSQL（华为云 RDS）并启用 pgvector 扩展做向量检索；连接参数从环境变量 `PG_*` 读取，通过小型连接池复用连接（见「五（5）网络与连接池配置」）。
2. 设计核心表：
   - 会话表 `chat_sessions`：以 `session_id` 为主键，保存人设提示词 `system_prompt`、长期摘要 `summary` 及创建/更新时间。
   - 消息表 `chat_messages`：以 `msg_id` 为主键，记录 `session_id`、角色 `user/assistant`、内容、表情包 `image_url`、所属回合号 `turn_id` 与时间戳。
   - 向量表 `message_embeddings`：以 `msg_id` 为主键，用 pgvector 的 `vector(1536)` 列存储对应消息的向量。
   - 表情包表 `meme_library`：以 `meme_id` 为主键，保存文件路径 `file_path`、标签 `tags`、描述 `description`、图上文字 `text_on_image` 与来源 `source`。
3. 在 `(session_id, turn_id)` 上建立索引，加速“按会话拉取历史”的查询。
4. 所有 SQL 均使用占位符绑定参数，杜绝字符串拼接，天然防注入。
5. 使用回合号 `turn_id` 而非时间戳作为排序依据，保证顺序稳定，不会因同一秒内的多条消息而错乱。
6. 数据库对象由 `shared_ptr` 持有，主线程与语音线程共享；内部使用连接池（`PG_POOL_SIZE`）并发读写，每个请求借出 / 归还一条连接。
7. 前三张核心表仍保留 `user_id` 列（历史遗留，当前固定为 `local_user`），查询仍按 `user_id` 过滤，便于将来重新接入多用户认证；表情包表 `meme_library` 为全局共享，不区分用户。
8. 启动时执行 `createTables()`，用幂等的 `CREATE TABLE IF NOT EXISTS` 建表 / 建索引（与 RDS 上的 schema 保持一致）；pgvector 扩展与 HNSW 近似索引已随建表 SQL 一并创建。

### （5）会话上下文管理

1. 每个会话在内存中维护一份 `messages` 数组，结构完全兼容 OpenAI 消息格式，并始终以 `system` 消息打头，携带人设与摘要。上下文采用滑动窗口和滚动摘要策略控制长度：
   - 设定上限约 15 轮对话（`MAX_TURNS = 15`，`MAX_MESSAGES = 15 * 2 + 1`），超出时截取最早的若干条旧消息；
   - 调用 LLM 将这些旧消息连同旧摘要一起压缩成一份新摘要，写回数据库的 `summary` 字段；
   - 从内存 `messages` 中删除这些旧消息，并重新刷新 `system` 消息，把新摘要注入人设末尾。
2. 摘要的生成是递归式滚动更新——旧的摘要参与新的摘要生成，避免历史信息丢失。
3. 摘要累计到一定批量（`summary_batch = 8` 条）才触发一次 LLM 摘要；若摘要生成失败，则回退为“原文片段拼接”，保证记忆不因一次失败而丢失。
4. 每次会话初始化时，先读人设与摘要，再从数据库加载最近若干条历史消息，共同组成完整上下文。

### （6）LLM 调用

- 所有 AI 能力统一走阿里云百炼 DashScope 的 OpenAI 兼容接口，后端只做代理，不承担模型推理。
- 对话采用流式模式 SSE，边接收边拼接，前端可获得“打字机”效果。
- 请求体固定使用 `qwen-plus`，参数为 `temperature = 0.75`、`repetition_penalty = 1.2`、`presence_penalty = 0.6`、`max_tokens = 2000`；`temperature` 偏高以鼓励自然叙事，重复惩罚用于抑制车轱辘话。
- SSE 解析的关键在于跨分片缓冲：TCP 字节流可能把一条事件切成几段，也可能一次送来多条，因此维护一个字符串缓冲，循环截取完整事件后再解析 JSON，直到遇到 `[DONE]` 标志结束。
- 支持工具调用 Function Calling：模型若判断需要外部信息，会返回 `tool_calls`，后端根据工具名查表执行对应函数，把结果作为工具消息追加到上下文，再次请求模型生成最终回复。
- 内置两个工具：
  - `get_current_time`：当用户询问“现在几点 / 今天几号”时，模型返回该工具调用，后端执行并回填当前时间。
  - `send_meme`：当对话氛围适合发表情包时，模型按情绪标签（开心、无语、委屈等）调用，后端从 `meme_library` 随机取一张对应标签的图，把图片 URL 通过 `image_url` 一并返回前端展示；带概率限流避免刷屏。
- 迭代最多三次，避免“调用—返回—再调用”的死循环。
- 另有一个同步版 `call_llm_sync`，专供摘要生成等不需要流式的场景使用，走非流式接口，一次拿回完整回复。
- API Key、Endpoint、路径等敏感配置均从环境变量读取，不在代码中硬编码。

### （7）长期记忆

1. 后端使用 pgvector（PostgreSQL 扩展）做语义检索。每条消息通过 Embeddings 接口映射为高维向量，语义相近的文本在向量空间中彼此靠近。检索时把用户问句向量化，与当前会话内的历史向量逐一计算相似度，取分数最高的若干条作为“相关记忆”。
2. 向量持久化在 `message_embeddings` 表。用户消息与 AI 回复落库时同步保存向量。
3. 每轮对话前，后端对用户问句做一次 embedding，调用 `searchSimilar` 检索 Top-3（pgvector 余弦距离）；余弦距离 ≥ 0.7（约等价旧 FAISS 内积 0.30）的条目直接丢弃，宁可空着也不把噪声灌进 prompt。
4. 检索结果以 `system` 消息形式插在 system 与历史消息之间，附加“仅供参考，不要原样复述”的说明。
5. 代价是每轮对话产生 2 次 embedding 调用（用户消息 1 次、AI 回复 1 次），配额消耗相对较高。

记忆由三层构成，全部存储在 PostgreSQL（RDS）中：

| 表 | 作用 |
| --- | --- |
| `chat_sessions` | 会话/角色：人设 `system_prompt`、摘要 `summary` |
| `chat_messages` | 每条消息（含 `session_id`、`role`、`content`、`turn_id`） |
| `message_embeddings` | 每条消息的向量（含 `session_id`，用于按会话隔离检索） |

工作流程：

1. 用户发送消息 → 消息写入 `chat_messages`，并调用 `text-embedding-v4` 生成向量写入 `message_embeddings`（记录所属 `session_id`）。
2. 回复前，用当前用户消息的向量在当前会话的向量库中做相似度检索（pgvector 余弦距离，Top-3），把命中的“相关记忆”作为 `system` 消息注入提示词。
3. 消息数量超过阈值（默认 15 轮 × 2）时触发 `trim_messages()`：把最旧的若干条消息交给 LLM 生成摘要存入 `summary`，并从上下文和数据库中裁剪。
4. 下次加载会话时，摘要会拼接到 `system prompt`，保证长期记忆不丢失。

### （8）语音合成

- TTS 走 DashScope 的 `SpeechSynthesizer` 接口，提交文本与音色参数，返回一个可播放的音频 URL。
- 请求模型为 `qwen-audio-3.1-tts-flash`（**当前 TTS 为 3.1 版本**），输出 `mp3`、采样率 24kHz；接口路径 `/api/v1/services/audio/tts/SpeechSynthesizer`。
- 自定义音色必须针对同一模型（3.1）复刻，否则 voice_id 与模型不匹配，合成会报 `InvalidParameter / Engine error`。
- 前端在朗读前会先清洗文本：去除括号旁白、Markdown 标记、多余空白，把换行替换为句号，并截断超长内容，保证朗读自然流畅。
- 播放使用浏览器原生 `Audio` 对象，播放中禁用按钮，结束后自动恢复状态。
- 音色优先级：创建角色时填写的「专属音色 ID」> 按性别默认音色（女 `longanhuan_v3.1` / 男 `anmingyuan_v3.1`）> 程序内置兜底 `longanhuan_v3.1`；无需再配置 `DASHSCOPE_VOICE_ID` 环境变量。

### （9）语音通话

- 通话建立在 WebSocket 之上，运行于独立的 `8081` 端口，与 HTTP 服务并存但互不干扰。
- 前端使用浏览器 Web Speech API 做语音识别，识别结果为文本后，通过 WebSocket 发送给后端。
- 后端收到文本，走独立的语音对话逻辑，得到回复后再调用 TTS 拿到音频 URL，一并通过 WebSocket 返回。
- **通话与文字聊天分离**：
  - 语音通话使用专属系统提示词（`getSystemPromptVoice`），明确告诉 AI 它的角色名、要求用口语化语句直接回答，并**严禁输出旁白、括号、动作描写等文字聊天格式**；
  - 通话内容不写入记忆库（`Conversation` 以 `persist = false` 运行），但**仍保留文字聊天记忆**：从数据库加载历史时会把 AI 回复里的旁白（括号内容）剥离、只留口语对话，再叠加长期摘要，作为记忆注入；
  - 前端有「说话状态」提示：`我在听，请开始说话`（绿）/ `正在说话，请稍候`（黄）/ `正在准备麦克风`（灰），避免用户在麦克风尚未就绪时过早开口导致漏录。
- 关键状态是“AI 正在说话”标志：AI 播放音频期间暂停语音识别并静音麦克风，播放结束后重新启动，避免麦克风录到 AI 的声音形成回声死循环。
- 浏览器兼容性上，语音识别目前仅 Chrome 与 Edge 支持。
- WebSocket 服务通过独立线程承载，主线程专注 HTTP，二者共享同一份数据库与互斥锁。
- 消息协议为 JSON：客户端发 `{"type":"chat","session_id":"...","text":"..."}` 发起对话、发 `{"type":"ping"}` 做心跳；服务端回 `{"type":"reply","text":"...","audio_url":"..."}`、`{"type":"pong"}` 或 `{"type":"error","message":"..."}`。

### （10）前端架构

- 前端为单页应用，使用 Vue 3 的响应式系统驱动视图，TailwindCSS 负责样式，均通过 CDN 引入，无构建步骤。
- 核心状态包括：会话列表、当前会话 ID、消息数组、输入内容、加载状态、各类模态框开关、通话状态等，全部用 `ref` 声明，修改即自动触发重渲染。
- 交互流程：
  - 进入页面拉取会话列表，默认选中第一个；
  - 切换会话时拉取该会话的历史记录并替换消息数组；
  - 发送消息时先乐观地把用户消息推入界面，再请求后端，得到回复后追加显示。
- 输入框支持 Enter 发送、Shift+Enter 换行，通过 `keyup` 事件配合修饰符阻止默认换行，并在发送前 `trim` 首尾空白。
- 消息支持附带图片（`image_url`），聊天记录中直接以缩略图展示；AI 发来的表情包图片也会渲染在消息气泡中。
- 输入区提供「表情包选择器」按钮：点击弹出素材库网格，选中后暂存到输入区随文本发送；左上角“+”按钮可上传本地图片录入素材库。
- AI 消息旁提供“播放语音”按钮，点击后请求 TTS 接口并播放。
- 通话按钮切换语音通话状态，配合 WebSocket 与语音识别完成整个闭环。

### （11）并发与线程安全

- `httplib` 的服务器是多线程模型，每个请求由一个独立工作线程处理，多个请求可能同时访问数据库。
- PostgreSQL 通过一个小型连接池（`PG_POOL_SIZE`，默认 10）管理：每个请求从池中借出一条连接、用后自动归还（RAII 守卫），避免为每个请求新建连接，也避免多线程争用同一条连接。
- 数据库对象用 `shared_ptr` 管理，主线程与语音线程各持一份引用，引用计数归零时才真正析构，保证生命周期安全。
- 语音线程以按值方式捕获 `shared_ptr`，即使主线程先行退出，语音线程仍持有有效引用。
- 线程间通过互斥锁串行化对共享资源的访问，避免竞争条件。

### （12）表情包互动与多模态识图

- 表情包素材以 `meme_library` 表 + `memes/` 目录的形式管理：表里记录文件路径、标签（情绪/场景）、描述与图上文字，图片文件本身存放在 `memes/` 目录，通过 `/memes/<文件名>` 路由对外提供。
- AI 主动发图：系统提示词里加入了「表情包互动」规则，模型在合适场景（用户情绪强烈、俏皮话、安慰/赞同/无语等）可调用 `send_meme` 工具，按标签随机取图，后端把图片 URL 作为 `image_url` 随回复返回前端展示；同一轮最多一次、带概率限流，避免刷屏。
- 用户主动发图：前端「表情包选择器」列出素材库，选中后随文本一起发送；消息落库时保存 `image_url`，历史回显时直接展示图片。
- 本地录入（“+”加号）：选择本地图片后走 `/api/upload_image`，后端先把图片存到 `memes/`，再调用视觉模型 `qwen-vl-plus`（`call_vision`）识别图片，输出情绪、关键词、图上文字与一句话描述，自动写入 `meme_library` 完成素材入库；识别结果按图片内容做了内存缓存，重复上传同一张图不会重复计费。
- 内置素材：仓库自带一套 `memes/` 表情包，`start.bat` 启动时会把 `memes/` 同步到 build 输出目录，并调用 `scripts/seed_memes.py` 幂等入库（已存在的文件自动跳过）。

## 四、工具

| 工具 | 版本 | 下载/安装 |
| --- | --- | --- |
| Visual Studio 2022 Build Tools | 最新版 | [Microsoft](https://visualstudio.microsoft.com/zh-hans/downloads/) |
| CMake | ≥ 3.21 | [https://cmake.org/download/](https://cmake.org/download/) |
| OpenSSL | 与本机导入库匹配的版本 | `vcpkg install openssl:x64-windows`，或使用已安装的 Windows OpenSSL |

## 五、准备工作

### （1）获取 API 密钥

访问阿里云百炼控制台：<https://bailian.console.aliyun.com/>，登录/注册后点击 `API_Key` 创建，复制生成的 API Key，在 Windows 下配置环境变量 `DASHSCOPE_API_KEY`。

### （2）获取业务空间 ID（Workspace ID）

访问千问 AI 平台：<https://platform.qianwenai.com/home/settings/workspaces>，获取 Workspace ID。该 ID **仅供声音复刻脚本 `Clone and TTS.py` 使用**（直接写进脚本的 `workspace_id` 变量），C++ 主程序不读取，无需配置成环境变量。

### （3）获取音色 ID（Voice ID）

> 当前项目 TTS 统一使用 **3.1 版本**模型 `qwen-audio-3.1-tts-flash`。自定义音色必须针对同一模型复刻，否则填入的 voice_id 在合成时会报错（`InvalidParameter / Engine error`）。

获取自定义音色有两种方式：

**方式一：控制台手动创建**

访问：<https://help.aliyun.com/zh/model-studio/cosyvoice-tts-http-api>，利用声音设计或声音复刻功能创建自定义音色。创建成功后，系统会返回一个形如 `qwen-audio-3.1-tts-flash-myvoice-xxxxxx` 的 ID，可在创建角色时填入「专属音色 ID」字段。

**方式二：用 `Clone and TTS.py` 脚本复刻**

1. **准备声音样本并上传 OSS**：准备一段声音样本（MP3），上传到阿里云对象存储 OSS 的 bucket，并把该对象设为**公共读**（或使用带签名的 URL），得到公网可访问的 MP3 地址，形如 `https://<你的bucket>.oss-cn-<region>.aliyuncs.com/<文件>.mp3`。
2. **修改脚本参数**：打开 `ChatBot/Clone and TTS.py`，改两处：
   - 顶部 `workspace_id = "ws-xxxx"` 改成你自己的业务空间 ID（见「（2）获取业务空间 ID」）；
   - 请求体里的 `"audio": "https://..."` 改成上一步得到的 MP3 地址；
   - （可选）`"prefix": "myvoice"` 可改成你喜欢的音色命名前缀。
3. **配置 API Key**：确保已配置环境变量 `DASHSCOPE_API_KEY`（脚本通过 `os.environ['DASHSCOPE_API_KEY']` 读取）。
4. **运行脚本**：`python "Clone and TTS.py"`。脚本会调用声音复刻接口创建音色并轮询等待就绪，最后打印 `voice_id`（形如 `qwen-audio-3.1-tts-flash-myvoice-xxxxxx`）。
5. **使用 voice_id**：把打印出的 `voice_id` 在创建角色时填入「专属音色 ID」字段，即可让该角色使用此音色。

> 脚本中的 `target_model` 已固定为 `qwen-audio-3.1-tts-flash`，与项目合成模型保持一致，请勿改动，否则生成的 voice_id 无法被当前项目使用。
>
> 脚本 `base_url` 中的 `cn-beijing` 为地域段；若你的业务空间不在北京地域，需相应修改该地域。

### （4）环境变量清单

项目运行时通过 `std::getenv` 读取以下环境变量：

| 环境变量 | 说明 | 示例 |
| --- | --- | --- |
| `DASHSCOPE_API_KEY` | 阿里云百炼 API Key（必需） | `sk-xxxxxxxxxxxxxxxx` |
| `PG_HOST` | PostgreSQL（RDS）地址，默认 `127.0.0.1` | `192.168.0.10`（内网地址） |
| `PG_PORT` | PostgreSQL 端口，默认 `5432` | `5432` |
| `PG_DB` | 数据库名，默认 `chatbot` | `chatbot` |
| `PG_USER` | 数据库用户，默认 `postgres` | `postgres` |
| `PG_PASSWORD` | 数据库密码；启动脚本要求填写 | `xxxxxxxxxxx` |
| `PG_POOL_SIZE` | 连接池大小上限，默认 `10`（建议 10~20） | `15` |
| `REDIS_HOST` | Redis（DCS）地址，默认 `127.0.0.1` | DCS 内网地址 |
| `REDIS_PORT` | Redis 端口，默认 `6379` | `6379` |
| `REDIS_PASSWORD` | Redis 密码（可选，设置了则执行 AUTH） | `xxxxxxxxxxx` |
| `REDIS_TIMEOUT_MS` | Redis 连接超时（毫秒），默认 `300` | `300` |
| `CHATBOT_API_TOKEN` | HTTP API 访问令牌；启动脚本要求填写 | `xxxxxxxxxxx` |

> 说明：`API_ENDPOINT`（`https://dashscope.aliyuncs.com`）与 `API_PATH`（`/compatible-mode/v1/chat/completions`）在 `utils.cpp` 中写死，无需配置。
>
> 说明：业务空间 ID（Workspace ID）不通过环境变量传递，仅在声音复刻脚本 `Clone and TTS.py` 中作为 `workspace_id` 变量硬编码使用。
>
> 说明：`start.bat` 和 `start in ECS.bat` 中的 `xxxxxxxxxxx` 都是占位符。请复制 `config.runtime.example.bat` 为 `config.runtime.bat`，在副本中填写 `PG_HOST`、`PG_USER`、`PG_PASSWORD`、`DASHSCOPE_API_KEY` 和 `CHATBOT_API_TOKEN`；ECS 上还需配置 `REDIS_HOST`、`REDIS_PASSWORD`。Redis 无密码时把 `REDIS_PASSWORD` 留空。脚本启动时会读取同目录的本地配置文件；该文件已被 Git 忽略。未填写必需凭据时脚本会停止。
>
> 如果旧凭据曾提交到 Git 或共享给他人，替换当前文件中的值无法撤销旧凭据，应在对应服务中轮换。

Windows 下配置示例（PowerShell，写入用户级环境变量，永久生效）：

```powershell
[Environment]::SetEnvironmentVariable("DASHSCOPE_API_KEY", "xxxxxxxxxxx", "User")
[Environment]::SetEnvironmentVariable("PG_HOST", "192.168.0.10", "User")
[Environment]::SetEnvironmentVariable("PG_PASSWORD", "xxxxxxxxxxx", "User")
[Environment]::SetEnvironmentVariable("CHATBOT_API_TOKEN", "xxxxxxxxxxx", "User")
```

配置完成后需重启终端 / IDE 使环境变量生效。
上例中的 `xxxxxxxxxxx` 必须替换为自己的真实值。

### （5）网络与连接池配置（华为云 RDS / DCS 部署）

**网络连通**

- 若 ChatBot 运行在本地 Windows：RDS（PostgreSQL）与 DCS（Redis）都需要绑定 EIP，并在各自安全组入方向放通你的公网 IP + 对应端口。
- 若 ChatBot 部署到华为云 ECS：将 ECS 与 RDS / DCS 放在同一 VPC，使用内网地址连接，无需 EIP，延迟更低、也更安全。

**连接数规划**

- ChatBot 是 HTTP 服务，每个 `/api/chat` 请求会在流式 provider 中执行数据库操作。后端**不为每个请求新建 PG 连接**，而是使用一个小型连接池（队列式实现，见 `SQLiteDB`），池大小由 `PG_POOL_SIZE` 控制（默认 10，建议 10~20）。连接按需创建、空闲复用，断线自动重建。
- 同时在 RDS 参数管理中检查 `max_connections`。Web 应用建议 200 左右，但**活跃连接数应控制在 vCPU 数量的 2~3 倍以内**。若实例规格较小（如 2 vCPU），请主动调小 `PG_POOL_SIZE`，避免打满数据库连接数。
- Redis 侧使用长连接（`RedisClient` 持有 `redisContext*` 不释放），用完不要 `redisFree`，否则高并发下会频繁建连；连接断开时 `rateLimit` 内部会自动重连。

## 六、华为云部署指南（详细步骤）

> 后端依赖两个华为云托管服务：**RDS（PostgreSQL + pgvector，存数据）** 与 **DCS（Redis，限流）**，程序本体跑在 **ECS** 上。三者须放**同一 VPC**，靠内网互通。
>
> ⚠️ **华为云 ≠ 阿里云**：华为云的访问控制只有「**安全组**」（入方向规则），**没有「白名单」这个概念**。下文所有"放行某个 IP"的操作，都是在对应资源的安全组里添加**入方向规则**，不是去配白名单。

### （1）网络拓扑

```text
                             互联网
       ┌───────────────────────┼───────────────────────┐
       │                       │                       │
  本地电脑(公网)         RDS 公网 EIP             ECS 公网 EIP
       │               198.51.100.20              203.0.113.10
       │                       │                       │
       └───────────────────────┼───────────────────────┘
                               │
                         VPC vpc-default
       ┌───────────────────────┼───────────────────────┐
       │                       │                       │
   RDS 内网               DCS 内网                ECS 内网
 192.168.0.11          192.168.0.12           192.168.0.10
  (PostgreSQL)             (Redis)              (ChatBot.exe)
```

- **ECS 上运行**：ChatBot 用**内网地址**连 RDS / DCS（快、安全、不出公网）。
- **本地运行**：用 **RDS 公网地址**连库；DCS 无公网，连不上会自动降级为内存限流（不影响功能）。

### （2）创建 VPC（一次性）

1. 控制台 →「虚拟私有云 VPC」→ 新建 VPC（示例 `vpc-default`），选择地域，记下网段（如 `192.168.0.0/24`）。
2. 后续 ECS / RDS / DCS 都选这个 VPC 和子网。

### （3）创建 ECS 并放行端口

1. 控制台 →「弹性云服务器 ECS」→ 购买，关键项：
   - 镜像：Windows Server（exe 是 Windows 程序）；
   - VPC / 子网：`vpc-default`；
   - 安全组：新建 `chatbot-sg`。
2. 购买后**绑定弹性公网 IP（EIP）** → 公网 `203.0.113.10`；内网自动分配 `192.168.0.10`。
3. 在 `chatbot-sg` 添加**入方向规则**：

| 协议 | 端口 | 源地址 | 用途 |
| --- | --- | --- | --- |
| TCP | 8080 | 0.0.0.0/0 | Web 前端 |
| TCP | 8081 | 0.0.0.0/0 | 语音 WebSocket |
| TCP | 3389 | 你的管理 IP | 远程桌面（可选） |

### （4）创建 RDS（PostgreSQL）并绑定

1. 控制台 →「云数据库 RDS」→ 购买，关键项：
   - 引擎：PostgreSQL（建议 15+，支持 pgvector）；
   - VPC / 子网：`vpc-default`；实例名 `chatbot_ict`；
   - 账号：`postgres`，密码 `********`（换成你自己的）。
2. 得到**内网地址** `192.168.0.11:5432`。
3. **绑定公网地址（本地调试才需要）**：实例「基本信息」开启公网访问 → 绑定 EIP → 得到 `198.51.100.20`。
4. **安全组放行**（华为云 RDS 靠安全组，不是白名单）：
   - 实例详情 → 安全组 → 绑定/复用 `chatbot-sg`；
   - 加**入方向规则**：
     - 内网（ECS）：源 = `chatbot-sg`（或 ECS 内网 IP）→ TCP 5432；
     - 公网（本地调试）：源 = **本地公网出口 IP** → TCP 5432。
   > 家庭宽带是动态 IP，连不上先查出口 IP（见（8））再更新安全组。
5. **初始化**：用 psql / Navicat 连上执行：
   ```sql
   CREATE EXTENSION IF NOT EXISTS vector;   -- pgvector
   ```
   > 建表由程序启动时幂等执行（`CREATE TABLE IF NOT EXISTS`），无需手动建表。

### （5）创建 DCS（Redis）并绑定

1. 控制台 →「分布式缓存服务 DCS」→ 购买，关键项：
   - 引擎：Redis；VPC / 子网：`vpc-default`；实例名 `chatbot-redis`；
   - 密码 `********`（换成你自己的）。
2. 得到**内网地址** `192.168.0.12:6379`。
3. **安全组放行**：DCS 安全组加入方向规则，源 = `chatbot-sg` → TCP 6379。
   > 本示例 DCS **未绑定公网**，本地连不上会打印「[Redis] 连接失败，已降级为单机内存限流」并自动降级，不影响聊天/语音。本地要用真 Redis，需给 DCS 绑公网并在安全组放行本地 IP。

### （6）安全组规则汇总

| 资源 | 入方向规则 |
| --- | --- |
| ECS `chatbot-sg` | 公网 → 80、443（HTTPS 入口）；管理 IP → 3389。过渡期如需直接访问旧端口，可暂留 8080、8081；确认 HTTPS 正常后收紧其公网入方向规则 |
| RDS | ECS 安全组 → 5432；本地公网 IP → 5432 |
| DCS | ECS 安全组 → 6379 |

> 华为云安全组支持「按安全组」互相放行（源选安全组而非 IP），比逐个 IP 省事。

### （7）部署程序到 ECS

1. 本地跑 `start.bat` 构建出 `build/bin/Release/`。
2. 把**整个 Release 目录**上传到 ECS（如 `C:\ChatBot\`）：`ChatBot.exe`、`index.html`、`Caddyfile.example`、`config.runtime.example.bat`、`cacert.pem`、`Clone and TTS.py`、`start in ECS.bat`、各依赖 DLL、`memes/`（POST_BUILD 已自动拷贝齐全，直接打包上传即可）。
3. 在 ECS 的 Release 目录将 `config.runtime.example.bat` 复制为 `config.runtime.bat`，填写该实例的内网地址和凭据，例如：

```bat
set "PG_HOST=192.168.0.11"
set "REDIS_HOST=192.168.0.12"
set "PG_PASSWORD=xxxxxxxxxxx"
set "REDIS_PASSWORD=xxxxxxxxxxx"
set "DASHSCOPE_API_KEY=xxxxxxxxxxx"
set "CHATBOT_API_TOKEN=xxxxxxxxxxx"
```

这些值须在 ECS 本地的 `config.runtime.bat` 中填好；Redis 无密码时将 `REDIS_PASSWORD` 留空。该私有文件不要上传到 GitHub。

4. 双击运行，看到「AI Web Server 已启动！」与「语音通话服务已启动」即成功。

> 远程桌面中的 `\\tsclient\...` 是映射的**本机磁盘**，不是 ECS 本地磁盘。在该路径双击批处理文件可能出现“UNC 路径不受支持”，程序也无法按预期读取 ECS 本地文件。先把整个 Release 目录复制到 ECS 的本地盘（如 `C:\ChatBot\`），再从该目录运行 `start in ECS.bat`。

### （7.1）为 ECS 配置 HTTPS 和语音 WSS

浏览器通过公网 HTTP 地址访问时，麦克风权限可能被拒绝并显示 `识别错误: not-allowed`。部署 HTTPS 后，前端会自动使用同域名的 `wss://<域名>/voice`；本地 `http://localhost:8080` 调试仍使用 `ws://localhost:8081/voice`。

1. 准备指向 ECS 弹性公网 IP 的域名 A 记录。ECS 位于中国大陆区域，公网网站域名还需完成适用的 ICP 备案或接入备案。在 ECS 安全组和 Windows 防火墙放通入方向 TCP 80、443。确认这两个端口没有被其他程序占用。
2. 在 ECS 安装 Windows 版 Caddy。将 Release 目录中的 `Caddyfile.example` 复制为 `Caddyfile`，把第一行的 `chat.example.com` 换成真实域名。模板会将 `/voice` 代理到本机 8081，其余请求代理到本机 8080。
3. 保持 `ChatBot.exe` 运行，在 `Caddyfile` 所在目录执行 `caddy validate --config Caddyfile`，再执行 `caddy run --config Caddyfile`。Caddy 会尝试申请并续期证书，以及将 HTTP 重定向到 HTTPS。长期运行时请按 Caddy 官方文档配置 Windows 服务。
4. 用 `https://<域名>` 打开页面，允许浏览器麦克风权限；开发者工具中确认语音连接为 `wss://<域名>/voice`。HTTPS 与语音都验证成功后，再限制 ECS 安全组对公网开放的 8080、8081。

仅有 Caddy 配置文件还不能使公网地址获得可信证书；需要域名解析和 80、443 可达。不要把 `cacert.pem` 当作网站证书，它用于客户端验证外部 HTTPS 服务。

### （8）验证连通性

ECS 上（内网）：
```powershell
Test-NetConnection 192.168.0.11 -Port 5432   # RDS
Test-NetConnection 192.168.0.12  -Port 6379   # DCS
```
本地（公网，需 RDS 已放行本地 IP）：
```powershell
Test-NetConnection 198.51.100.20 -Port 5432     # RDS 公网
pg_isready -h 198.51.100.20 -p 5432 -t 10       # PostgreSQL 握手状态
```
`TcpTestSucceeded: True` 只表示 TCP 端口可达，不代表 PostgreSQL 已接受连接。`pg_isready` 若显示 `no response`，应继续检查 RDS 实例状态、公网 EIP 绑定、访问控制及服务端日志；不要直接判断为密码错误。`pg_isready` 不需要正确的数据库密码即可判断服务是否接受连接。
查本地出口公网 IP（比 ip.cn 准，走 DNS 直连）：
```powershell
nslookup myip.opendns.com resolver1.opendns.com
```
把返回地址（如 `198.51.100.30`）加到 RDS 安全组入方向规则（源填 `IP/32`）。

### （9）本地 vs ECS 地址对照

| 环境变量 | 本地运行 | ECS 运行 |
| --- | --- | --- |
| `PG_HOST` | `198.51.100.20`（公网） | `192.168.0.11`（内网） |
| `REDIS_HOST` | 连不上，自动降级 | `192.168.0.12`（内网） |
| `CHATBOT_CA_BUNDLE` | 可省略（有兜底路径） | `%~dp0cacert.pem` |

## 七、编译命令

先复制 `ChatBot/config.runtime.example.bat` 为 `ChatBot/config.runtime.bat`，在本地副本填写地址与凭据，或设置同名环境变量。之后运行 `ChatBot\start.bat`，脚本会自动编译并运行程序。

`CMakeLists.txt` 不固定 OpenSSL 的安装盘符或 DLL 版本。CMake 会查找 OpenSSL、PostgreSQL 和 hiredis；动态链接 OpenSSL 时，从所选安装目录复制运行时 DLL。首次使用时，把 `ChatBot/config.local.example.bat` 复制为 `ChatBot/config.local.bat`，按本机位置填写以下三项：

```bat
set "VCPKG_ROOT=C:\path\to\vcpkg"
set "PostgreSQL_ROOT=C:\path\to\PostgreSQL"
set "OPENSSL_ROOT_DIR=C:\path\to\OpenSSL"
```

`config.local.bat` 和 `config.runtime.bat` 已加入 `.gitignore`，不会随项目提交。`start.bat` 会检查 vcpkg 工具链、hiredis、PostgreSQL 和 OpenSSL 文件；每次运行都会用 `cmake --fresh` 重新配置，再执行 Release 编译，因此需要支持 `--fresh` 的 CMake。编译产物仍保留在 `build/` 中。

> 注意：必须使用 MSVC 构建。本项目依赖 PostgreSQL(libpq) 与 hiredis，用 MinGW/GCC 需自行配置对应开发库，链接阶段易报 `undefined reference`。

## 八、快速开始（运行指南）

1. **安装工具**：Visual Studio 2022 Build Tools、支持 `--fresh` 的 CMake、vcpkg、与本机导入库匹配的 OpenSSL。
2. **核对依赖**：按「七、编译命令」填写 `config.local.bat`，确认 vcpkg 已安装 x64-windows 版本的 hiredis。
3. **填写凭据**：按「五、准备工作」填写本地 `config.runtime.bat` 或设置环境变量，然后重启终端 / IDE。
4. **一键启动**：运行 `ChatBot\start.bat`，脚本会依次完成 CMake 配置、Release 编译、同步 `memes/` 素材、启动 `ChatBot.exe`，并调用 `scripts/seed_memes.py` 把内置表情包入库（需本机安装 Python）。（脚本通过 `%~dp0` 自动定位自身目录，项目放在任意路径都能正常运行，无需手动改路径。）
5. **访问**：
   - 本机访问：<http://localhost:8080>
   - 局域网访问：`http://<本机局域网IP>:8080`（后端监听 `0.0.0.0:8080`）
6. **语音通话**：点击通话按钮即可。语音通道由 `ChatBot.exe` 内置的 WebSocket 服务提供（`ws://<主机>:8081/voice`）。

> 后端启动时会打印 `AI Web Server 已启动！` 与 `语音通话服务已启动: ws://0.0.0.0:8081/voice`，看到这两行即表示服务就绪。

## 九、常见问题（FAQ）

**Q1：编译 / 链接报 `undefined reference`？**
本项目依赖 PostgreSQL(libpq) 与 hiredis。请使用 Visual Studio 2022 构建，并确保已安装 OpenSSL、PostgreSQL 开发库和 vcpkg 的 x64-windows 版 hiredis。

**Q2：链接报找不到 PostgreSQL / hiredis / OpenSSL 库？**
`CMakeLists.txt` 通过 `find_package` 定位依赖。检查 `ChatBot/config.local.bat` 中的 `VCPKG_ROOT`、`PostgreSQL_ROOT` 和 `OPENSSL_ROOT_DIR`；`start.bat` 会清理旧配置缓存并重新查找依赖。

**Q3：对话返回「找不到 API_KEY 环境变量」？**
说明 `DASHSCOPE_API_KEY` 未配置或未生效。按「五、准备工作」配置后，重启终端 / IDE 再启动。

**Q4：语音通话没声音 / 连不上 8081？**
- 语音识别依赖浏览器 Web Speech API，目前仅 Chrome / Edge 支持；
- 确认 `ChatBot.exe` 已在监听 8081（启动日志会打印对应提示）。

**Q5：网页能打开但会话为空 / 数据不保存？**
数据保存在 PostgreSQL（RDS）中。通过 `start.bat` 启动时，数据库连接信息来自环境变量或本地 `config.runtime.bat`；未配置时默认尝试 `127.0.0.1:5432`。先确认实际生效的 `PG_HOST` / `PG_PORT` / `PG_DB` / `PG_USER`，再按上面的「验证连通性」检查。

**Q6：本地启动提示 `server closed the connection unexpectedly`，但 5432 端口可达？**

这表示 TCP 已建立，但连接在 PostgreSQL 完成握手前被关闭；单凭这条错误不能确定是数据库实例、网络中间设备还是访问控制导致。按顺序排查：

1. 在 RDS 控制台确认实例处于正常运行状态，当前绑定的公网 EIP、数据库端口与 `start.bat` 中的 `PG_HOST` / `PG_PORT` 一致。
2. 检查 RDS 安全组入方向是否允许**当前本机出口公网 IP/32**访问 5432；家庭网络的出口 IP 可能变化。TCP 检测成功也不能代替检查实际数据库握手。
3. 在本机运行 `pg_isready -h 198.51.100.20 -p 5432 -t 10`。若仍为 `no response`，查看 RDS 连接日志、实例事件及相关网络设备日志，确认是谁关闭了连接。
4. 若 `pg_isready` 显示 `accepting connections`，再用 `psql "host=198.51.100.20 port=5432 dbname=postgres user=<数据库用户名> sslmode=require" -W -c "SELECT 1"` 验证登录。密码在交互提示中输入，不要写进命令行或日志；此时再根据具体报错检查用户名、密码、数据库名及 SSL 配置。

## 十、发布到 GitHub

本 README 中的公网地址均为文档示例；实际地址只写入本地 `config.runtime.bat`。发布前检查 `git status --short` 和 `git diff --cached --name-only`，确认没有上传本地配置、证书私钥、数据库文件、构建产物或个人截图。`config.local.bat`、`config.runtime.bat`、`build/`、`venv/` 等已列入 `.gitignore`；以前跟踪过的虚拟环境文件已在当前工作区标记删除。

```powershell
git add -A
git diff --cached --check
git diff --cached --name-only
git commit -m "Prepare project for public repository"
git push origin main
```

执行提交和推送前，再查看暂存文件清单。若凭据曾进入旧提交或共享记录，删除当前文件中的值不能使旧凭据失效，应在对应服务中轮换。
