#pragma once

#include <string_view>

/**
 * @class Logger
 * @brief Process-wide non-blocking logger.
 *
 * Logger::log() only enqueues a message (mutex + push + signal). A dedicated
 * worker thread dequeues and writes to stdout so hot paths (recompiled 68K,
 * audio, VDP) never block on I/O.
 */
class Logger {
    public:
    Logger() = delete;

    /// Enqueue @p message for asynchronous stdout write. Thread-safe and
    /// non-blocking aside from a short critical section.
    static void log(std::string_view message);

    /// Enqueue a formatted message (printf-style).
    static void log(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 1, 2)))
#endif
        ;

    /// Block until the queue is empty (or the worker has stopped). Useful in
    /// tests / process shutdown; not required for normal play.
    static void flush();

    /// Stop the worker after draining (best-effort). Safe to call more than once.
    static void shutdown();

    private:
    struct State;
    static State &state();
};
