/**
 * @file test_multi_reactor_bench.cc
 * @brief 主从 Reactor + Gateway 路由架构 — 完整压测套件
 *
 * 跑 5 组对比测试，验证不同维度下的性能特征：
 *   Bench 1: SubReactor 数量扫描（1/2/4/8）—— 验证多 Reactor 线性扩展
 *   Bench 2: 客户端数量扫描（16/64/200）—— 验证高并发承载
 *   Bench 3: 单包大小扫描（64B/1KB/4KB/16KB）—— 验证带宽 vs 包数
 *   Bench 4: 异步发送 vs 同步发送 —— 验证服务端真实极限
 *   Bench 5: 业务模块数扫描（2/5/10）—— 验证路由表开销
 *
 * 每组测试输出：吞吐(QPS)、带宽(MB/s)、平均延迟、客户端成功率
 * 最后聚合成一张对比表
 *
 * 协议复用 test_multi_reactor.cc 的设计
 *
 * 编译: make -f Makefile.multi_reactor_bench
 * 运行: ./test_multi_reactor_bench
 */

#include "precompiled.h"
#include "Actor.h"
#include "ActorSystem.h"
#include "EventLoop.h"
#include "EventLoopThreadPool.h"
#include "Acceptor.h"
#include "Message.h"

#include <cassert>
#include <chrono>
#include <sstream>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <iomanip>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

using namespace bllsll;
using namespace std::chrono;

// ============================================================
//  协议（同 test_multi_reactor.cc）
// ============================================================
static inline uint16_t cmdModule(uint16_t cmd) { return cmd / 1000; }

static std::string encodeFrame(uint16_t cmd, const std::string& payload) {
    uint32_t bodyLen = 2 + (uint32_t)payload.size();
    uint32_t netLen = htonl(bodyLen);
    uint16_t netCmd = htons(cmd);
    std::string frame;
    frame.reserve(4 + bodyLen);
    frame.append(reinterpret_cast<const char*>(&netLen), 4);
    frame.append(reinterpret_cast<const char*>(&netCmd), 2);
    frame.append(payload);
    return frame;
}

static uint16_t parseCmd(const std::string& fullFrame) {
    if (fullFrame.size() < 6) return 0;
    uint16_t netCmd;
    std::memcpy(&netCmd, fullFrame.data() + 4, 2);
    return ntohs(netCmd);
}

// ============================================================
//  通用业务 Actor — 直接 echo 回包（不区分 Scene/Chat，按 module 注册）
// ============================================================
class EchoBizActor : public Actor
{
public:
    int moduleId = 0;
    std::atomic<int> framesRecv{0};
    std::atomic<int> framesSent{0};
    std::atomic<uint64_t> bytesRecv{0};

    ActorTask OnCoroutineMessage(ActorMessage msg) override {
        if (msg.type == MsgType::NetworkRecv) {
            framesRecv.fetch_add(1);
            bytesRecv.fetch_add(msg.data.size());
            SendToNetwork(msg.fd, msg.data.data(), (int)msg.data.size());
            framesSent.fetch_add(1);
        }
        co_return;
    }
};

// ============================================================
//  Gateway — map 路由
// ============================================================
class GatewayActor : public Actor
{
public:
    std::unordered_map<uint16_t, uint32_t> moduleRoutes_;
    std::atomic<int> connCount{0};
    std::atomic<int> framesRouted{0};
    std::atomic<int> badFrames{0};

    void RegisterModule(uint16_t module, uint32_t actorId) {
        moduleRoutes_[module] = actorId;
    }

    std::unordered_map<int, std::string> fdInputBuf_;

    ActorTask OnCoroutineMessage(ActorMessage msg) override {
        switch (msg.type) {
            case MsgType::Connected:
                connCount.fetch_add(1);
                fdInputBuf_[msg.fd] = {};
                break;
            case MsgType::NetworkRecv:
                onRecv(msg.fd, msg.data);
                break;
            case MsgType::Disconnected:
                connCount.fetch_sub(1);
                fdInputBuf_.erase(msg.fd);
                break;
            default: break;
        }
        co_return;
    }

private:
    void onRecv(int fd, const std::string& data) {
        auto& buf = fdInputBuf_[fd];
        buf.append(data);
        while (true) {
            if (buf.size() < 4) break;
            uint32_t netLen;
            std::memcpy(&netLen, buf.data(), 4);
            uint32_t bodyLen = ntohl(netLen);
            uint32_t totalLen = 4 + bodyLen;
            if (buf.size() < totalLen) break;

            std::string fullFrame = buf.substr(0, totalLen);
            buf.erase(0, totalLen);

            if (fullFrame.size() < 6) { badFrames.fetch_add(1); continue; }
            uint16_t cmd = parseCmd(fullFrame);
            uint16_t module = cmdModule(cmd);
            auto it = moduleRoutes_.find(module);
            if (it == moduleRoutes_.end()) { badFrames.fetch_add(1); continue; }

            ActorMessage forward(MsgType::NetworkRecv, GetActorId(), fd, std::move(fullFrame));
            GetSystem()->Send(it->second, std::move(forward));
            framesRouted.fetch_add(1);
        }
    }
};

// ============================================================
//  Server 上下文（可重复启停）
// ============================================================
struct ServerCtx {
    int port;
    int subReactorNum;
    int workerNum;
    int moduleNum;          // 业务模块数

    std::unique_ptr<EventLoop> mainLoop;
    std::unique_ptr<EventLoopThreadPool> pool;
    std::unique_ptr<ActorSystem> actorSys;
    Acceptor* acceptor = nullptr;
    GatewayActor* gateway = nullptr;
    std::vector<EchoBizActor*> bizActors;

    void Start() {
        mainLoop = std::make_unique<EventLoop>();
        pool = std::make_unique<EventLoopThreadPool>();
        actorSys = std::make_unique<ActorSystem>();

        mainLoop->Create();
        if (subReactorNum > 0) {
            pool->SetThreadNum(subReactorNum);
            pool->Start(actorSys.get());
        }
        actorSys->Start(workerNum, mainLoop.get());
        mainLoop->SetActorSystem(actorSys.get());

        // 注册 N 个业务 Actor
        std::vector<uint32_t> bizIds;
        for (int i = 0; i < moduleNum; ++i) {
            auto a = std::make_unique<EchoBizActor>();
            a->moduleId = i + 1;
            bizActors.push_back(a.get());
            bizIds.push_back(actorSys->RegisterActor(std::move(a)));
        }

        // Gateway
        auto gw = std::make_unique<GatewayActor>();
        gateway = gw.get();
        uint32_t gwId = actorSys->RegisterActor(std::move(gw));
        for (int i = 0; i < moduleNum; ++i) {
            gateway->RegisterModule((uint16_t)(i + 1), bizIds[i]);
        }

        // Acceptor
        acceptor = new Acceptor(port, mainLoop.get(), gwId);
        if (pool->Enabled()) acceptor->SetThreadPool(pool.get());
    }

    void Stop() {
        if (pool) pool->Stop();
        if (mainLoop) mainLoop->Stop();
        // actorSys/loop 析构会等线程退出
        actorSys.reset();
        pool.reset();
        mainLoop.reset();
        acceptor = nullptr;
        gateway = nullptr;
        bizActors.clear();
    }

    uint64_t TotalServerFramesRecv() const {
        uint64_t s = 0;
        for (auto* a : bizActors) s += a->framesRecv.load();
        return s;
    }
};

// ============================================================
//  通用客户端工具
// ============================================================
static bool readN(int fd, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r > 0) got += r;
        else if (r == 0) return false;
        else if (errno == EINTR) continue;
        else return false;
    }
    return true;
}

static bool writeN(int fd, const char* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = ::send(fd, buf + sent, n - sent, 0);
        if (w > 0) sent += w;
        else if (w == 0) return false;
        else if (errno == EINTR) continue;
        else return false;
    }
    return true;
}

static int connectServer(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in svrAddr{};
    svrAddr.sin_family = AF_INET;
    svrAddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &svrAddr.sin_addr);
    if (::connect(fd, (sockaddr*)&svrAddr, sizeof(svrAddr)) < 0) {
        ::close(fd); return -1;
    }
    timeval tv{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

// ============================================================
//  客户端：同步模式（发一帧等一帧）
// ============================================================
struct ClientResult {
    int sent = 0, recv = 0;
    bool ok = false;
};

static void syncClient(int port, int msgCount, int payloadSize, int moduleNum,
                       ClientResult* result) {
    int fd = connectServer(port);
    if (fd < 0) return;

    std::string payload(payloadSize, 'x');

    for (int i = 0; i < msgCount; ++i) {
        uint16_t module = (uint16_t)((i % moduleNum) + 1);
        uint16_t cmd = module * 1000 + (i % 3);
        std::string frame = encodeFrame(cmd, payload);
        if (!writeN(fd, frame.data(), frame.size())) break;
        result->sent++;

        // 收一帧
        char header[4];
        if (!readN(fd, header, 4)) break;
        uint32_t bodyLen = ntohl(*(uint32_t*)header);
        std::string body(bodyLen, '\0');
        if (!readN(fd, &body[0], bodyLen)) break;
        result->recv++;
    }
    result->ok = (result->sent == msgCount && result->recv == msgCount);
    ::close(fd);
}

// ============================================================
//  客户端：异步模式（流水线，一次发一批再统一收）
//  服务端真实吞吐由这种模式压出来
// ============================================================
static void asyncClient(int port, int msgCount, int payloadSize, int moduleNum,
                        int pipelineDepth, ClientResult* result) {
    int fd = connectServer(port);
    if (fd < 0) return;

    std::string payload(payloadSize, 'x');
    int sent = 0, recv = 0;

    // 维护一个"已发未收"的窗口 = pipelineDepth
    while (sent < msgCount || recv < sent) {
        // 尽量先填满窗口
        while (sent < msgCount && (sent - recv) < pipelineDepth) {
            uint16_t module = (uint16_t)((sent % moduleNum) + 1);
            uint16_t cmd = module * 1000 + (sent % 3);
            std::string frame = encodeFrame(cmd, payload);
            if (!writeN(fd, frame.data(), frame.size())) goto done;
            sent++;
        }
        // 收一帧
        if (recv < sent) {
            char header[4];
            if (!readN(fd, header, 4)) goto done;
            uint32_t bodyLen = ntohl(*(uint32_t*)header);
            std::string body(bodyLen, '\0');
            if (!readN(fd, &body[0], bodyLen)) goto done;
            recv++;
        }
    }
done:
    result->sent = sent;
    result->recv = recv;
    result->ok = (sent == msgCount && recv == msgCount);
    ::close(fd);
}

// ============================================================
//  单次 benchmark 运行
// ============================================================
struct BenchResult {
    std::string name;
    int subReactor;
    int clients;
    int msgPerClient;
    int payloadSize;
    int moduleNum;
    bool async;
    int pipelineDepth;

    int okClients = 0;
    uint64_t totalFrames = 0;
    uint64_t totalBytes = 0;
    int64_t elapsedMs = 0;
    uint64_t qps = 0;
    double mbps = 0.0;
    double avgLatencyUs = 0.0;
};

static int g_nextPort = 19600;

static BenchResult runBench(const std::string& name,
                            int subReactorNum, int workerNum, int moduleNum,
                            int clientNum, int msgPerClient, int payloadSize,
                            bool async, int pipelineDepth)
{
    BenchResult br;
    br.name = name;
    br.subReactor = subReactorNum;
    br.clients = clientNum;
    br.msgPerClient = msgPerClient;
    br.payloadSize = payloadSize;
    br.moduleNum = moduleNum;
    br.async = async;
    br.pipelineDepth = pipelineDepth;

    ServerCtx srv;
    srv.port = g_nextPort++;
    srv.subReactorNum = subReactorNum;
    srv.workerNum = workerNum;
    srv.moduleNum = moduleNum;
    srv.Start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<ClientResult> results(clientNum);
    std::vector<std::thread> threads;
    threads.reserve(clientNum);

    auto t0 = steady_clock::now();
    for (int i = 0; i < clientNum; ++i) {
        if (async) {
            threads.emplace_back(asyncClient, srv.port, msgPerClient, payloadSize,
                                 moduleNum, pipelineDepth, &results[i]);
        } else {
            threads.emplace_back(syncClient, srv.port, msgPerClient, payloadSize,
                                 moduleNum, &results[i]);
        }
    }
    for (auto& t : threads) t.join();
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();

    // 汇总
    for (auto& r : results) {
        if (r.ok) br.okClients++;
        br.totalFrames += r.recv;
        br.totalBytes += (uint64_t)r.recv * (4 + 2 + payloadSize);  // 含包头
    }
    br.elapsedMs = elapsed;
    if (elapsed > 0) {
        br.qps = br.totalFrames * 1000ULL / elapsed;
        br.mbps = br.totalBytes * 1000.0 / elapsed / (1024.0 * 1024.0);
    }
    if (br.totalFrames > 0) {
        // 平均延迟 = 总时间 × 并发客户端数 / 总帧数
        // （表示单帧从发起到收到回包的等价平均时长）
        br.avgLatencyUs = (double)elapsed * 1000.0 * clientNum / br.totalFrames;
    }

    srv.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // 等端口 TIME_WAIT 部分释放

    return br;
}

// ============================================================
//  打印结果表格
// ============================================================
static void printHeader() {
    std::cout << std::endl;
    std::cout << std::string(115, '=') << std::endl;
    std::cout << std::left
              << std::setw(35) << "Benchmark"
              << std::setw(7)  << "Sub"
              << std::setw(7)  << "Cli"
              << std::setw(8)  << "MsgPC"
              << std::setw(8)  << "Size"
              << std::setw(7)  << "Mod"
              << std::setw(8)  << "Mode"
              << std::setw(7)  << "OK"
              << std::setw(10) << "Frames"
              << std::setw(8)  << "Ms"
              << std::setw(12) << "QPS"
              << std::setw(10) << "MB/s"
              << "Lat(us)"
              << std::endl;
    std::cout << std::string(115, '-') << std::endl;
}

static void printRow(const BenchResult& r) {
    std::ostringstream mode;
    if (r.async) mode << "async" << r.pipelineDepth;
    else         mode << "sync";

    std::cout << std::left
              << std::setw(35) << r.name
              << std::setw(7)  << r.subReactor
              << std::setw(7)  << r.clients
              << std::setw(8)  << r.msgPerClient
              << std::setw(8)  << r.payloadSize
              << std::setw(7)  << r.moduleNum
              << std::setw(8)  << mode.str()
              << std::setw(7)  << (std::to_string(r.okClients) + "/" + std::to_string(r.clients))
              << std::setw(10) << r.totalFrames
              << std::setw(8)  << r.elapsedMs
              << std::setw(12) << r.qps
              << std::setw(10) << std::fixed << std::setprecision(2) << r.mbps
              << std::fixed << std::setprecision(1) << r.avgLatencyUs
              << std::endl;
}

// ============================================================
//  main
// ============================================================
int main() {
    std::vector<BenchResult> results;

    std::cout << "\n##########################################" << std::endl;
    std::cout <<   "#  Multi-Reactor Benchmark Suite          " << std::endl;
    std::cout <<   "##########################################\n" << std::endl;

    // ===== Bench 1: SubReactor 数量扫描 =====
    std::cout << ">>> Bench 1: SubReactor scan (固定 16 客户端 × 500 同步消息 × 256B)" << std::endl;
    for (int n : {0, 1, 2, 4, 8}) {
        std::string name = "SubReactor=" + std::to_string(n);
        auto r = runBench(name, n, 8, 2, 16, 500, 256, false, 0);
        results.push_back(r);
    }

    // ===== Bench 2: 客户端数量扫描 =====
    std::cout << "\n>>> Bench 2: Client scan (固定 4 SubReactor × 500 同步消息 × 256B)" << std::endl;
    for (int c : {16, 64, 128, 200}) {
        std::string name = "Clients=" + std::to_string(c);
        auto r = runBench(name, 4, 8, 2, c, 500, 256, false, 0);
        results.push_back(r);
    }

    // ===== Bench 3: 包大小扫描 =====
    std::cout << "\n>>> Bench 3: Payload size scan (固定 32 客户端 × 200 同步消息)" << std::endl;
    for (int sz : {64, 1024, 4096, 16384}) {
        std::string name = "Payload=" + std::to_string(sz) + "B";
        auto r = runBench(name, 4, 8, 2, 32, 200, sz, false, 0);
        results.push_back(r);
    }

    // ===== Bench 4: 异步流水线 vs 同步对比 =====
    std::cout << "\n>>> Bench 4: Async pipelining (固定 4 SubReactor × 16 客户端 × 5000 消息 × 256B)" << std::endl;
    for (int d : {1, 8, 64, 256}) {
        std::string name = (d == 1) ? "sync(window=1)" : "async(window=" + std::to_string(d) + ")";
        auto r = runBench(name, 4, 8, 2, 16, 5000, 256, (d > 1), d);
        results.push_back(r);
    }

    // ===== Bench 5: 业务模块数扫描 =====
    std::cout << "\n>>> Bench 5: Business module count scan (固定 32 客户端 × 500 同步消息 × 256B)" << std::endl;
    for (int m : {2, 5, 10, 20}) {
        std::string name = "Modules=" + std::to_string(m);
        auto r = runBench(name, 4, 8, m, 32, 500, 256, false, 0);
        results.push_back(r);
    }

    // 汇总
    std::cout << "\n\n##########################################" << std::endl;
    std::cout <<     "#  FINAL RESULTS                          " << std::endl;
    std::cout <<     "##########################################" << std::endl;
    printHeader();
    for (auto& r : results) {
        printRow(r);
    }
    std::cout << std::string(115, '=') << std::endl;
    std::cout << "\nNotes:" << std::endl;
    std::cout << "  Sub      = SubReactor 数量 (0=单 Reactor 模式)" << std::endl;
    std::cout << "  Cli      = 并发客户端数" << std::endl;
    std::cout << "  MsgPC    = 每客户端消息数" << std::endl;
    std::cout << "  Size     = payload 大小（不含 6 字节包头）" << std::endl;
    std::cout << "  Mod      = 业务模块数（每模块一个 EchoBizActor）" << std::endl;
    std::cout << "  Mode     = sync=逐条等回包; async N=流水线窗口=N" << std::endl;
    std::cout << "  QPS      = 服务端每秒处理帧数" << std::endl;
    std::cout << "  MB/s     = 单向带宽（不含回包，含包头）" << std::endl;
    std::cout << "  Lat(us)  = 单帧等效平均延迟 = elapsed × clients / frames" << std::endl;

    return 0;
}
