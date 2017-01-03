#pragma once

#include <set>
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>

#include "logging/default.hpp"

/**
 * @class Timer
 *
 * @brief The timer contains counter and handler.
 *
 * With every clock interval the counter should be decresed for
 * delta count. Delta count is one for now but it should be a variable in the
 * near future. The handler is function that will be called when counter
 * becomes zero or smaller than zero.
 */
struct Timer
{
    using sptr = std::shared_ptr<Timer>;
    using handler_t = std::function<void(void)>;

    Timer(int64_t counter, handler_t handler):
        counter(counter), handler(handler) 
    {
    }

    bool operator--()
    {
        if (--counter <= 0)
            return true;
        else
            return false;
    }

    int64_t counter;
    handler_t handler;
};

/**
 * Timer container knows how to add a new timer and remove the
 * existing container from itself. Also, time container object
 * has the process method whose responsibility is to iterate
 * over existing timers and call the appropriate handler function.
 * The handler method could be called on the same thread, on a
 * separate thread or on a thread pool, that is implementation detail of
 * the process method.
 */

/** 
 * @class TimerSet
 *
 * @brief Trivial timer container implementation.
 *
 * Internal data stucture for storage of timers is std::set. So, the
 * related timer complexities are:
 *     insertion: O(log(n))
 *     deletion: O(log(n))
 *     process: O(n)
 */
class TimerSet
{
public:
    void add(Timer::sptr timer)
    {
        timers.insert(timer);
    }

    void remove(Timer::sptr timer)
    {
        timers.erase(timer);
    }

    uint64_t size() const
    {
        return timers.size();
    }

    void process()
    {
        for (auto it = timers.begin(); it != timers.end(); ) {
            auto timer = *it;
            if (--*timer) {
                timer->handler();
                it = timers.erase(it);
                continue;
            }
            ++it;
        }
    }

private:
    std::set<std::shared_ptr<Timer>> timers;
};

/** 
 * @class TimerScheduler
 *
 * @brief TimerScheduler is a manager class and its responsibility is to
 * take care of the time and call the timer_container process method in the
 * appropriate time.
 *
 * @tparam timer_container_type implements a strategy how the timers 
 *                              are processed
 * @tparam delta_time_type type of a time distance between two events
 * @tparam delta_time granularity between the two events, default value is 1
 */
template <
    typename timer_container_type,
    typename delta_time_type,
    uint64_t delta_time = 1
> class TimerScheduler
{
public:

    /**
     * Adds a timer.
     *
     * @param timer shared pointer to the timer object \ref Timer
     */
    void add(Timer::sptr timer)
    {
        timer_container.add(timer);
    }

    /**
     * Removes a timer.
     *
     * @param timer shared pointer to the timer object \ref Timer
     */
    void remove(Timer::sptr timer)
    {
        timer_container.remove(timer);
    }

    /**
     * Provides the number of pending timers. The exact number has to be
     * provided by a timer_container.
     *
     * @return uint64_t the number of pending timers.
     */
    uint64_t size() const
    {
        return timer_container.size();
    }

    /**
     * Runs a separate thread which responsibility is to run the process method
     * at the appropriate time (every delta_time from the beginning of 
     * processing.
     */
    void run()
    {
        is_running.store(true);

        run_thread = std::thread([this]() {
            while (is_running.load()) {
                std::this_thread::sleep_for(delta_time_type(delta_time));
                timer_container.process();
                logging::info("timer_container.process()");
            }
        });
    }

    /**
     * Stops the whole processing.
     */
    void stop()
    {
        is_running.store(false); 
    }

    /**
     * Joins the processing thread.
     */
    ~TimerScheduler()
    {
        run_thread.join();
    }


private:
    timer_container_type timer_container;
    std::thread run_thread;
    std::atomic<bool> is_running;
};
