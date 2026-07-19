#include "Logger.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>

namespace {

// Soft cap: under burst logging, drop the oldest lines so memory stays bounded
// and log() never waits on a full unbounded buffer.
constexpr std::size_t kMaxQueuedMessages = 4096;

} // namespace

struct Logger::State {
    SDL_Mutex              *mutex  = nullptr;
    SDL_Condition          *cond   = nullptr;
    SDL_Thread             *thread = nullptr;
    std::deque<std::string> queue;
    std::atomic<bool>       running{false};
    std::atomic<bool>       started{false};
    bool                    stopRequested = false;

    State() {
        mutex = SDL_CreateMutex();
        cond  = SDL_CreateCondition();
    }

    ~State() {
        shutdown();
        if (cond) {
            SDL_DestroyCondition(cond);
            cond = nullptr;
        }
        if (mutex) {
            SDL_DestroyMutex(mutex);
            mutex = nullptr;
        }
    }

    State(const State &)            = delete;
    State &operator=(const State &) = delete;

    static int threadEntry(void *data) {
        static_cast<State *>(data)->run();
        return 0;
    }

    void ensureStarted() {
        if (started.load(std::memory_order_acquire))
            return;

        SDL_LockMutex(mutex);
        if (!started.load(std::memory_order_relaxed)) {
            stopRequested = false;
            running.store(true, std::memory_order_release);
            thread = SDL_CreateThread(threadEntry, "md-logger", this);
            started.store(true, std::memory_order_release);
        }
        SDL_UnlockMutex(mutex);
    }

    void enqueue(std::string message) {
        ensureStarted();
        if (!mutex)
            return;

        SDL_LockMutex(mutex);
        if (stopRequested) {
            SDL_UnlockMutex(mutex);
            return;
        }
        if (queue.size() >= kMaxQueuedMessages)
            queue.pop_front();
        queue.push_back(std::move(message));
        SDL_SignalCondition(cond);
        SDL_UnlockMutex(mutex);
    }

    void run() {
        for (;;) {
            std::string message;
            {
                SDL_LockMutex(mutex);
                while (queue.empty() && !stopRequested)
                    SDL_WaitCondition(cond, mutex);

                if (queue.empty() && stopRequested) {
                    SDL_UnlockMutex(mutex);
                    break;
                }

                message = std::move(queue.front());
                queue.pop_front();
                SDL_UnlockMutex(mutex);
            }

            // I/O outside the lock so producers stay cheap.
            std::fputs(message.c_str(), stdout);
            if (message.empty() || message.back() != '\n')
                std::fputc('\n', stdout);
            std::fflush(stdout);
        }
        running.store(false, std::memory_order_release);
    }

    void flush() {
        if (!started.load(std::memory_order_acquire))
            return;
        for (;;) {
            SDL_LockMutex(mutex);
            const bool empty = queue.empty();
            SDL_UnlockMutex(mutex);
            if (empty)
                break;
            SDL_DelayNS(100'000); // 0.1 ms
        }
    }

    void shutdown() {
        if (!started.load(std::memory_order_acquire))
            return;

        SDL_LockMutex(mutex);
        stopRequested = true;
        SDL_SignalCondition(cond);
        SDL_UnlockMutex(mutex);

        if (thread) {
            SDL_WaitThread(thread, nullptr);
            thread = nullptr;
        }
        started.store(false, std::memory_order_release);
        running.store(false, std::memory_order_release);
    }
};

Logger::State &Logger::state() {
    static State s;
    return s;
}

void Logger::log(std::string_view message) {
    state().enqueue(std::string(message));
}

void Logger::log(const char *fmt, ...) {
    if (fmt == nullptr)
        return;

    va_list args;
    va_start(args, fmt);
    va_list argsCopy;
    va_copy(argsCopy, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, argsCopy);
    va_end(argsCopy);

    if (needed < 0) {
        va_end(args);
        return;
    }

    std::string buffer(static_cast<std::size_t>(needed), '\0');
    std::vsnprintf(buffer.data(), static_cast<std::size_t>(needed) + 1, fmt, args);
    va_end(args);

    state().enqueue(std::move(buffer));
}

void Logger::flush() {
    state().flush();
}

void Logger::shutdown() {
    state().shutdown();
}
