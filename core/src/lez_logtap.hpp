#pragma once
// LogTap - tees this process's stdout and stderr through a pipe so the module can show the local
// chain's log and find revert reasons in it. The wallet library and the embedded sequencer only
// report details there (core_ffi print_error, guest panics such as "Missing required
// authorization"). Everything is still written to the original streams, so the host log is
// unchanged. Lines are kept in a bounded ring with increasing sequence numbers.
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace lez {

class LogTap {
public:
    struct Line { long long seq; std::string text; };

    ~LogTap() { stop(); }

    bool start() {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_running) return true;
        int fds[2];
        if (pipe(fds) != 0) return false;
        fflush(stdout); fflush(stderr);
        m_origOut = dup(STDOUT_FILENO);
        m_origErr = dup(STDERR_FILENO);
        if (m_origOut < 0 || m_origErr < 0) { close(fds[0]); close(fds[1]); return false; }
        // only stderr goes back to stderr; stdout lines go back to stdout
        m_outPipe[0] = fds[0]; m_outPipe[1] = fds[1];
        int efds[2];
        if (pipe(efds) != 0) { close(fds[0]); close(fds[1]); return false; }
        m_errPipe[0] = efds[0]; m_errPipe[1] = efds[1];
        dup2(m_outPipe[1], STDOUT_FILENO);
        dup2(m_errPipe[1], STDERR_FILENO);
        setvbuf(stdout, nullptr, _IOLBF, 0);
        m_running = true;
        m_outThread = std::thread([this] { pump(m_outPipe[0], m_origOut); });
        m_errThread = std::thread([this] { pump(m_errPipe[0], m_origErr); });
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (!m_running) return;
            m_running = false;
            fflush(stdout); fflush(stderr);
            dup2(m_origOut, STDOUT_FILENO);
            dup2(m_origErr, STDERR_FILENO);
            close(m_outPipe[1]); close(m_errPipe[1]);
        }
        if (m_outThread.joinable()) m_outThread.join();
        if (m_errThread.joinable()) m_errThread.join();
        close(m_outPipe[0]); close(m_errPipe[0]);
        close(m_origOut); close(m_origErr);
    }

    long long lastSeq() { std::lock_guard<std::mutex> lk(m_mtx); return m_seq; }

    std::vector<Line> since(long long seq, size_t max) {
        std::lock_guard<std::mutex> lk(m_mtx);
        std::vector<Line> out;
        for (const auto& l : m_lines) if (l.seq > seq) out.push_back(l);
        if (out.size() > max) out.erase(out.begin(), out.end() - long(max));
        return out;
    }

    // The first guest panic after `seq`: "<message> (at <file:line>)", or "" if there is none.
    // risc0 prints a guest panic as "thread 'main' (1) panicked at <file:line:col>:" followed by
    // the message on the next line.
    std::string panicSince(long long seq) {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto it = m_lines.begin(); it != m_lines.end(); ++it) {
            if (it->seq <= seq) continue;
            auto p = it->text.find("panicked at ");
            if (p == std::string::npos) continue;
            std::string where = it->text.substr(p + 12);
            if (!where.empty() && where.back() == ':') where.pop_back();
            auto slash = where.rfind('/');
            if (slash != std::string::npos) where = where.substr(slash + 1);
            auto next = std::next(it);
            std::string msg = next != m_lines.end() ? next->text : "";
            return msg.empty() ? "guest panicked at " + where : msg + " (" + where + ")";
        }
        return "";
    }

    // The last "[ffi] ..." line after `seq` (the wallet library's reason for a refused send).
    std::string ffiErrorSince(long long seq) {
        std::lock_guard<std::mutex> lk(m_mtx);
        std::string last;
        for (const auto& l : m_lines) if (l.seq > seq && l.text.find("[ffi]") != std::string::npos) last = l.text;
        auto p = last.find("[ffi]");
        return p == std::string::npos ? "" : last.substr(p + 6);
    }

private:
    static std::string stripAnsi(const std::string& s) {
        std::string out; out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
                i += 2;
                while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) i++;
                continue;
            }
            out += s[i];
        }
        return out;
    }

    void pump(int from, int to) {
        char buf[4096];
        std::string partial;
        for (;;) {
            ssize_t n = read(from, buf, sizeof buf);
            if (n <= 0) break;
            ssize_t off = 0;
            while (off < n) { ssize_t w = write(to, buf + off, size_t(n - off)); if (w <= 0) break; off += w; }
            partial.append(buf, size_t(n));
            size_t nl;
            while ((nl = partial.find('\n')) != std::string::npos) {
                add(stripAnsi(partial.substr(0, nl)));
                partial.erase(0, nl + 1);
            }
            if (partial.size() > 65536) { add(stripAnsi(partial)); partial.clear(); }
        }
        if (!partial.empty()) add(stripAnsi(partial));
    }

    void add(std::string text) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_lines.push_back({++m_seq, std::move(text)});
        while (m_lines.size() > 4000) m_lines.pop_front();
    }

    std::mutex m_mtx;
    bool m_running = false;
    int m_origOut = -1, m_origErr = -1;
    int m_outPipe[2] = {-1, -1}, m_errPipe[2] = {-1, -1};
    std::thread m_outThread, m_errThread;
    std::deque<Line> m_lines;
    long long m_seq = 0;
};

}  // namespace lez
