// g++ -std=c++20 -pthread -I src -o /tmp/console_replay_test tests/console_replay_test.cpp
// Server locale: BGFT (register/start/progress) e QoS (RTP si ferma se
// per 2 secondi non arriva un frame a schermo). Riproduce il log 01.48.
#include "display/flip_sched.h"
#include "payload/install_plan.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_fail;

static void expect(const char* name, bool cond) {
    if (cond) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    ++g_fail;
}

static int listen_local(int* port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001);
    addr.sin_port = 0;
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return -1;
    socklen_t len = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return -1;
    if (::listen(fd, 8) != 0) return -1;
    *port = ntohs(addr.sin_port);
    return fd;
}

static bool write_all(int fd, const char* s) {
    size_t n = std::strlen(s);
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::send(fd, s + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) return false;
        off += static_cast<size_t>(w);
    }
    return true;
}

static bool read_line(int fd, std::string& out) {
    out.clear();
    char c;
    while (out.size() < 2000) {
        ssize_t r = ::recv(fd, &c, 1, 0);
        if (r <= 0) return !out.empty();
        if (c == '\n') return true;
        if (c != '\r') out.push_back(c);
    }
    return false;
}

enum class SrvMode { Ok, Exists, BadProg, Qos };

struct ServerStats {
    std::atomic<int> reg{0};
    std::atomic<int> prep{0};
    std::atomic<int> appinstall{0};
    std::atomic<int> uninstall{0};
    std::atomic<int> start{0};
};

static const char* field_tok(const char* s) {
    if (!s) return "NULL";
    if (!s[0]) return "EMPTY";
    return s;
}

static const char* intern(const char* tok, std::vector<std::string>& keep) {
    if (std::strcmp(tok, "NULL") == 0) return nullptr;
    if (std::strcmp(tok, "EMPTY") == 0) return "";
    keep.emplace_back(tok);
    return keep.back().c_str();
}

static void serve_client(int fd, ServerStats* stats) {
    std::string line;
    if (!read_line(fd, line)) {
        ::close(fd);
        return;
    }
    SrvMode mode = SrvMode::Ok;
    if (line == "MODE exists") mode = SrvMode::Exists;
    else if (line == "MODE badprog") mode = SrvMode::BadProg;
    else if (line == "MODE qos") mode = SrvMode::Qos;
    else if (line != "MODE ok") {
        write_all(fd, "ERR mode\n");
        ::close(fd);
        return;
    }
    int prepared = 0;
    int task = -1;
    int started = 0;
    int ticks = 0;
    unsigned long size = 0;
    int idle = 0;
    int rtp_stopped = 0;
    while (read_line(fd, line)) {
        if (line == "QUIT") break;
        if (mode == SrvMode::Qos) {
            int presents = 0, late = 0, handle = 1;
            if (std::sscanf(line.c_str(), "SEC p=%d late=%d handle=%d", &presents, &late, &handle) != 3) {
                write_all(fd, "ERR sec\n");
                break;
            }
            (void)late;
            (void)handle;
            if (presents <= 0) ++idle;
            else idle = 0;
            if (idle >= 2) rtp_stopped = 1;
            write_all(fd, rtp_stopped ? "RTP_STOP\n" : "RTP_OK\n");
            continue;
        }
        if (line.rfind("REG ", 0) == 0) {
            int user = 0, ent = 0, opt = 0, slot = 0;
            unsigned long psz = 0;
            char id[160], url[240], exu[160], name[80], icon[160], sku[80], play[40], rel[40], ptype[40], psub[40];
            int n = std::sscanf(line.c_str(), "REG %d %d %d %lu %d %159s %239s %159s %79s %159s %79s %39s %39s %39s %39s",
                                &user, &ent, &opt, &psz, &slot, id, url, exu, name, icon, sku, play, rel, ptype, psub);
            stats->reg.fetch_add(1);
            if (n != 15) {
                std::fprintf(stderr, "REG parse %d line [%s]\n", n, line.c_str());
                write_all(fd, "RC 80990004 TASK -1\n");
                continue;
            }
            std::vector<std::string> keep;
            keep.reserve(16);
            XcBgftParamEx ex{};
            ex.param.user_id = user;
            ex.param.entitlement_type = ent;
            ex.param.option = opt;
            ex.param.package_size = psz;
            ex.slot = static_cast<unsigned>(slot);
            ex.param.id = intern(id, keep);
            ex.param.content_url = intern(url, keep);
            ex.param.content_ex_url = intern(exu, keep);
            ex.param.content_name = intern(name, keep);
            ex.param.icon_path = intern(icon, keep);
            ex.param.sku_id = intern(sku, keep);
            ex.param.playgo_scenario_id = intern(play, keep);
            ex.param.release_date = intern(rel, keep);
            ex.param.package_type = intern(ptype, keep);
            ex.param.package_sub_type = intern(psub, keep);
            int rc = xc_bgft_register_rc(&ex);
            if (rc == 0 && mode == SrvMode::Exists && !prepared) rc = static_cast<int>(XC_BGFT_ALREADY);
            if (rc == 0) {
                task = 7;
                size = psz;
                started = 0;
                ticks = 0;
            } else {
                task = -1;
            }
            char buf[80];
            std::snprintf(buf, sizeof buf, "RC %08x TASK %d\n", static_cast<unsigned>(rc), task);
            write_all(fd, buf);
            continue;
        }
        if (line.rfind("PREP ", 0) == 0) {
            stats->prep.fetch_add(1);
            prepared = 1;
            write_all(fd, "RC 00000000\n");
            continue;
        }
        if (line.rfind("START ", 0) == 0) {
            int id = -1;
            std::sscanf(line.c_str(), "START %d", &id);
            stats->start.fetch_add(1);
            if (id == task && task >= 0) {
                started = 1;
                write_all(fd, "RC 00000000\n");
            } else {
                write_all(fd, "RC ffffffff\n");
            }
            continue;
        }
        if (line.rfind("PROG ", 0) == 0) {
            unsigned long len = 0, got = 0;
            unsigned err = 0;
            if (started && size > 0) {
                ++ticks;
                len = size;
                got = ticks >= 4 ? size : size * static_cast<unsigned long>(ticks) / 4;
                if (mode == SrvMode::BadProg) err = static_cast<unsigned>(XC_BGFT_INVALID);
            }
            char buf[96];
            std::snprintf(buf, sizeof buf, "ERR %08x LEN %lu GOT %lu\n", err, len, got);
            write_all(fd, buf);
            continue;
        }
        if (line.rfind("APPINSTALL ", 0) == 0) {
            stats->appinstall.fetch_add(1);
            write_all(fd, "RC 80020012\n");
            continue;
        }
        if (line.rfind("UNINSTALL ", 0) == 0) {
            stats->uninstall.fetch_add(1);
            write_all(fd, "RC 00000000\n");
            continue;
        }
        write_all(fd, "ERR cmd\n");
        break;
    }
    ::close(fd);
}

static void serve_loop(int listen_fd, ServerStats* stats, std::atomic<bool>* stop) {
    while (!stop->load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        timeval tv{0, 200000};
        int sel = ::select(listen_fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) continue;
        serve_client(fd, stats);
    }
}

struct Conn {
    int fd = -1;
    explicit Conn(int port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(0x7f000001);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            ::close(fd);
            fd = -1;
        }
    }
    ~Conn() {
        if (fd >= 0) ::close(fd);
    }
    bool ok() const { return fd >= 0; }
    bool say(const char* s) { return fd >= 0 && write_all(fd, s); }
    bool line(std::string& o) { return fd >= 0 && read_line(fd, o); }
};

struct HookCtx {
    Conn* conn = nullptr;
    int regs = 0;
};

static int hook_reg(void* v, const XcBgftParamEx* ex, int* task) {
    auto* c = static_cast<HookCtx*>(v);
    c->regs++;
    char buf[1200];
    std::snprintf(buf, sizeof buf, "REG %d %d %d %lu %u %s %s %s %s %s %s %s %s %s %s\n", ex->param.user_id,
                  ex->param.entitlement_type, ex->param.option, ex->param.package_size, ex->slot, field_tok(ex->param.id),
                  field_tok(ex->param.content_url), field_tok(ex->param.content_ex_url), field_tok(ex->param.content_name),
                  field_tok(ex->param.icon_path), field_tok(ex->param.sku_id), field_tok(ex->param.playgo_scenario_id),
                  field_tok(ex->param.release_date), field_tok(ex->param.package_type),
                  field_tok(ex->param.package_sub_type));
    if (!c->conn->say(buf)) return -1;
    std::string line;
    if (!c->conn->line(line)) return -1;
    unsigned rc = 0;
    int id = -1;
    if (std::sscanf(line.c_str(), "RC %x TASK %d", &rc, &id) != 2) return -1;
    *task = id;
    return static_cast<int>(rc);
}

static int hook_start(void* v, int task) {
    auto* c = static_cast<HookCtx*>(v);
    char buf[64];
    std::snprintf(buf, sizeof buf, "START %d\n", task);
    if (!c->conn->say(buf)) return -1;
    std::string line;
    if (!c->conn->line(line)) return -1;
    unsigned rc = 0;
    if (std::sscanf(line.c_str(), "RC %x", &rc) != 1) return -1;
    return static_cast<int>(rc);
}

static int hook_prog(void* v, int task, int* error_result, unsigned long* length, unsigned long* transferred) {
    auto* c = static_cast<HookCtx*>(v);
    char buf[64];
    std::snprintf(buf, sizeof buf, "PROG %d\n", task);
    if (!c->conn->say(buf)) return -1;
    std::string line;
    if (!c->conn->line(line)) return -1;
    unsigned err = 0;
    unsigned long len = 0, got = 0;
    if (std::sscanf(line.c_str(), "ERR %x LEN %lu GOT %lu", &err, &len, &got) != 3) return -1;
    *error_result = static_cast<int>(err);
    *length = len;
    *transferred = got;
    return 0;
}

static int hook_prep(void* v, const char* path) {
    auto* c = static_cast<HookCtx*>(v);
    char buf[400];
    std::snprintf(buf, sizeof buf, "PREP %s\n", path);
    if (!c->conn->say(buf)) return -1;
    std::string line;
    if (!c->conn->line(line)) return -1;
    unsigned rc = 0;
    if (std::sscanf(line.c_str(), "RC %x", &rc) != 1) return -1;
    return static_cast<int>(rc);
}

static int drive_mode(int port, const char* mode, int user, const char* path, unsigned long size, int* regs) {
    Conn conn(port);
    expect("connessione al server BGFT", conn.ok());
    if (!conn.ok()) return -1;
    std::string hello = std::string("MODE ") + mode + "\n";
    expect("mode", conn.say(hello.c_str()));
    HookCtx ctx;
    ctx.conn = &conn;
    XcDriveHooks h{};
    h.reg = hook_reg;
    h.start = hook_start;
    h.prog = hook_prog;
    h.prep = hook_prep;
    h.ctx = &ctx;
    h.max_polls = 20;
    int rc = xc_drive_install(&h, user, path, size);
    if (regs) *regs = ctx.regs;
    conn.say("QUIT\n");
    return rc;
}

static int old_pick(int bufCount, int start, int current, int pending) {
    for (int n = 0; n < bufCount; ++n) {
        int i = (start + n) % bufCount;
        if (i != current || pending == 0) return i;
    }
    return -1;
}

struct Vout {
    int q[2]{};
    int qn = 0;
    int current = -1;
    int handle = 1;
    int presents = 0;
    int presentsTotal = 0;
    uint64_t now = 0;
    uint64_t nextVb = 16000;
    int pending() const { return handle >= 0 ? qn : 0; }
    void advance(uint64_t us) {
        now += us;
        while (now >= nextVb) {
            nextVb += 16000;
            if (handle >= 0 && qn > 0) {
                current = q[0];
                q[0] = q[1];
                --qn;
                ++presents;
                ++presentsTotal;
            }
        }
    }
    int submit(int idx) {
        if (handle < 0) return -1;
        if (qn >= 2) return static_cast<int>(0x8029000C);
        for (int i = 0; i < qn; ++i)
            if (q[i] == idx) return static_cast<int>(0x8029000C);
        q[qn++] = idx;
        return 0;
    }
};

struct FlipRun {
    bool killed = false;
    int acks = 0;
    int decoded = 0;
    int late = 0;
    int zeroDrawSeconds = 0;
    int presentsTotal = 0;
    std::string qos;
};

static FlipRun run_flip(int port, bool useNew) {
    FlipRun r;
    Vout v;
    int index = 0;
    int hist[8];
    int nhist = 0;
    for (int i = 0; i < 8; ++i) hist[i] = -1;
    Conn conn(port);
    expect(useNew ? "qos nuovo" : "qos vecchio", conn.ok() && conn.say("MODE qos\n"));
    if (!conn.ok()) return r;
    for (int sec = 0; sec < 6; ++sec) {
        v.presents = 0;
        int lateSec = 0;
        int drawUs = 0;
        uint64_t secEnd = v.now + 1000000;
        while (v.now < secEnd) {
            uint64_t step = sec == 0 ? 14000 : 1000;
            ++r.decoded;
            int shown = 0;
            if (v.handle < 0) {
                ++r.late;
                ++lateSec;
            } else {
                int pick = useNew ? flipPick(4, index, v.current, v.pending(), hist, nhist)
                                  : old_pick(4, index, v.current, v.pending());
                if (pick < 0) {
                    ++r.late;
                    ++lateSec;
                } else {
                    int rc = v.submit(pick);
                    int alive = 1;
                    if (useNew) {
                        shown = flipOnSubmit(rc, &alive);
                        if (!alive) {
                            /* Il device non chiude: flipErrorKillsPort è 0. */
                        }
                    } else if (rc < 0) {
                        alive = 0;
                        shown = 0;
                        v.handle = -1;
                        r.killed = true;
                    } else {
                        shown = 1;
                    }
                    if (shown) {
                        for (int i = 7; i > 0; --i) hist[i] = hist[i - 1];
                        hist[0] = pick;
                        if (nhist < 8) ++nhist;
                        index = (pick + 1) % 4;
                        ++r.acks;
                        drawUs += static_cast<int>(step);
                    } else {
                        ++r.late;
                        ++lateSec;
                    }
                }
            }
            v.advance(step);
        }
        if (sec > 0 && v.presents == 0 && drawUs == 0 && lateSec >= 50) ++r.zeroDrawSeconds;
        char buf[80];
        std::snprintf(buf, sizeof buf, "SEC p=%d late=%d handle=%d\n", v.presents, lateSec, v.handle);
        std::string reply;
        if (!conn.say(buf) || !conn.line(reply)) r.qos = "ERR";
        else r.qos = reply;
    }
    conn.say("QUIT\n");
    r.presentsTotal = v.presentsTotal;
    return r;
}

static bool file_has(const char* path, const char* needle) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    std::fclose(f);
    return data.find(needle) != std::string::npos;
}

static std::string fn_body(const char* path, const char* sig) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    std::fclose(f);
    auto at = data.find(sig);
    if (at == std::string::npos) return {};
    auto end = data.find("\nvoid ", at + 1);
    auto end2 = data.find("\nbool ", at + std::strlen(sig));
    if (end2 != std::string::npos && (end == std::string::npos || end2 < end)) end = end2;
    if (end == std::string::npos) end = data.size();
    return data.substr(at, end - at);
}

int main() {
    expect("init gia fatto", xc_user_init_ok(static_cast<int>(XC_USER_ALREADY_INIT)));
    expect("init zero", xc_user_init_ok(0));
    expect("init altro", !xc_user_init_ok(static_cast<int>(0x80020012)));
    /* Log 01.48: init 80960003, GetInitialUser ha scritto -1, il foreground
     * non è stato chiesto. Il pad della stessa sessione era 318019532. */
    expect("utente foreground", xc_pick_user(0, 318019532, -1, -1, nullptr, 0) == 318019532);
    expect("utente initial", xc_pick_user(-1, -1, 0, 42, nullptr, 0) == 42);
    int login[4] = {0, -1, 7, 9};
    expect("utente login", xc_pick_user(-1, -1, -1, -1, login, 4) == 7);
    expect("nessun utente", xc_pick_user(-1, -1, 0, -1, nullptr, 0) == -1);

    XcBgftParamEx old{};
    old.param.user_id = -1;
    old.param.entitlement_type = 5;
    old.param.id = "IV0000-XCLD00001_00-PSBOXCLOUD000001";
    old.param.content_url = "/user/data/pkg/PSBox-PS4-01.48.pkg";
    old.param.content_name = "PSCloud";
    old.param.option = XC_OPT_DISABLE_CDN | XC_OPT_FORCE_UPDATE;
    old.param.package_size = 9895936;
    expect("01.48 register", xc_bgft_register_rc(&old) == static_cast<int>(XC_BGFT_INVALID));

    XcBgftParamEx neu{};
    xc_fill_bgft(&neu, 318019532, "/data/pkg/PSBox-PS4-01.50.pkg", 9895936);
    expect("fill nuovo", xc_bgft_register_rc(&neu) == 0);
    expect("id vuoto", neu.param.id && neu.param.id[0] == 0);
    expect("playgo 0", neu.param.playgo_scenario_id && std::strcmp(neu.param.playgo_scenario_id, "0") == 0);
    expect("icona", neu.param.icon_path && neu.param.icon_path[0] != 0);

    int port = 0;
    int lfd = listen_local(&port);
    expect("bind server", lfd >= 0 && port > 0);
    if (lfd < 0) return 1;
    ServerStats stats;
    std::atomic<bool> stop{false};
    std::thread th(serve_loop, lfd, &stats, &stop);

    {
        Conn conn(port);
        expect("conn vecchio", conn.ok());
        conn.say("MODE ok\n");
        conn.say("REG -1 5 10008 9895936 0 IV0000-XCLD00001_00-PSBOXCLOUD000001 "
                 "/user/data/pkg/PSBox-PS4-01.48.pkg NULL PSCloud NULL NULL NULL EMPTY EMPTY EMPTY\n");
        std::string line;
        expect("risposta vecchia", conn.line(line));
        unsigned rc = 0;
        int task = 0;
        std::sscanf(line.c_str(), "RC %x TASK %d", &rc, &task);
        expect("server rifiuta la 01.48", rc == XC_BGFT_INVALID && task < 0);
        conn.say("APPINSTALL /data/pkg/PSBox-PS4-01.48.pkg\n");
        expect("dialogo", conn.line(line) && line.find("80020012") != std::string::npos);
        conn.say("QUIT\n");
    }

    int regs = -1;
    int rc = drive_mode(port, "ok", 318019532, "/data/pkg/PSBox-PS4-01.50.pkg", 9895936, &regs);
    expect("install nuova rc 0", rc == 0);
    expect("register chiamato", regs >= 1);
    int app_after_ok = stats.appinstall.load();
    int un_after_ok = stats.uninstall.load();
    expect("niente AppInstall sul path buono", app_after_ok == 1); /* solo il replay 01.48 sopra */
    expect("niente uninstall", un_after_ok == 0);
    expect("start partito", stats.start.load() >= 1);

    int prep_before = stats.prep.load();
    int regs_ex = 0;
    rc = drive_mode(port, "exists", 318019532, "/data/pkg/PSBox-PS4-01.50.pkg", 9895936, &regs_ex);
    expect("gia installato poi prepare", rc == 0);
    expect("due register", regs_ex >= 2);
    expect("prepare una volta", stats.prep.load() == prep_before + 1);
    expect("ancora niente uninstall", stats.uninstall.load() == 0);
    expect("AppInstall solo dal replay", stats.appinstall.load() == 1);

    rc = drive_mode(port, "badprog", 318019532, "/data/pkg/PSBox-PS4-01.50.pkg", 9895936, nullptr);
    expect("errore progress ferma", rc == XC_DRIVE_STARTED_FAIL);
    expect("progress non chiama AppInstall", stats.appinstall.load() == 1);

    regs = 99;
    rc = drive_mode(port, "ok", -1, "/data/pkg/PSBox-PS4-01.50.pkg", 9895936, &regs);
    expect("user -1 non registra", rc == XC_DRIVE_NO_USER && regs == 0);

    FlipRun oldr = run_flip(port, false);
    FlipRun newr = run_flip(port, true);
    std::printf("flip vecchio: killed=%d presents=%d acks=%d decoded=%d late=%d zeroDraw=%d qos=%s\n", oldr.killed,
                oldr.presentsTotal, oldr.acks, oldr.decoded, oldr.late, oldr.zeroDrawSeconds, oldr.qos.c_str());
    std::printf("flip nuovo: killed=%d presents=%d acks=%d decoded=%d late=%d zeroDraw=%d qos=%s\n", newr.killed,
                newr.presentsTotal, newr.acks, newr.decoded, newr.late, newr.zeroDrawSeconds, newr.qos.c_str());
    expect("vecchia porta muore", oldr.killed);
    expect("vecchio secondo a draw 0", oldr.zeroDrawSeconds >= 1);
    expect("vecchio QoS ferma RTP", oldr.qos == "RTP_STOP");
    expect("porta nuova viva", !newr.killed);
    expect("nuovo non ha il secondo fermo", newr.zeroDrawSeconds == 0);
    expect("nuovo QoS tiene RTP", newr.qos == "RTP_OK");
    expect("nuovi present", newr.presentsTotal > 250);
    expect("ack dei submit", newr.acks >= newr.presentsTotal && newr.acks <= newr.presentsTotal + 2);
    expect("ack meno dei decode", newr.acks < newr.decoded);
    expect("flipErrorKillsPort", flipErrorKillsPort(static_cast<int>(0x8029000C)) == 0);

    const char* pay = "src/payload/psbox_install.c";
    const char* disp = "src/display/display_ps4.cpp";
    expect("payload chiama il piano", file_has(pay, "xc_drive_install") && file_has(pay, "xc_pick_user"));
    expect("payload chiede il foreground", file_has(pay, "sceUserServiceGetForegroundUser"));
    expect("payload senza AppInstallPkg", !file_has(pay, "sceAppInstUtilAppInstallPkg"));
    std::string present = fn_body(disp, "bool present()");
    std::string pick = fn_body(disp, "bool pickFree(bool block)");
    expect("present usa flipOnSubmit", present.find("flipOnSubmit") != std::string::npos);
    expect("present non chiude la porta", present.find("g_video = -1") == std::string::npos);
    expect("pickFree usa flipPick", pick.find("flipPick") != std::string::npos);

    stop.store(true);
    th.join();
    ::close(lfd);

    if (g_fail) {
        std::fprintf(stderr, "%d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("console_replay: ok\n");
    return 0;
}
