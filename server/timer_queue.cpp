#include "timer_queue.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <sys/timerfd.h>
#include <unistd.h>

#include "event_loop.hpp"

// Timer 的静态序号计数器，在这里唯一定义
std::atomic<uint64_t> Timer::s_numCreated_{0};

namespace {

int createTimerfd() {
    // CLOCK_MONOTONIC：与 steady_clock 对齐，不受系统时间调整影响
    const int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("timerfd_create() failed: " << std::strerror(errno));
        std::abort();
    }
    return fd;
}

// 把"绝对时刻 when"换算成"从现在起还有多久"，写进 itimerspec
struct timespec howMuchTimeFromNow(Timestamp when) {
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                      when - Clock::now())
                      .count();
    if (micros < 100) {
        micros = 100;  // 至少 100us，避免 0 值导致定时器被"解除武装"
    }
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(micros / 1000000);
    ts.tv_nsec = static_cast<long>((micros % 1000000) * 1000);
    return ts;
}

// 读走 timerfd 的计数，清掉可读状态（否则电平触发会一直通知）
void readTimerfd(int timerfd) {
    uint64_t howmany = 0;
    const ssize_t n = ::read(timerfd, &howmany, sizeof(howmany));
    (void)n;
}

// 重设内核定时器：下一次在 when 时刻到期
void resetTimerfd(int timerfd, Timestamp when) {
    struct itimerspec newValue;
    std::memset(&newValue, 0, sizeof(newValue));
    newValue.it_value = howMuchTimeFromNow(when);
    if (::timerfd_settime(timerfd, 0, &newValue, nullptr) < 0) {
        LOG_ERROR("timerfd_settime() failed: " << std::strerror(errno));
    }
}

}  // namespace

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop), timerfd_(createTimerfd()), timerfdChannel_(loop, timerfd_) {
    // 这里只绑定回调，不 enableReading —— 因为 enableReading 会回调
    // loop->updateChannel，而此刻 EventLoop 尚未构造完毕。
    timerfdChannel_.setReadCallback([this]() { handleRead(); });
}

TimerQueue::~TimerQueue() {
    timerfdChannel_.disableAll();
    timerfdChannel_.remove();
    ::close(timerfd_);

    // 清掉所有尚未触发的 Timer 对象
    for (const Entry& entry : timers_) {
        delete entry.second;
    }
}

void TimerQueue::start() {
    timerfdChannel_.enableReading();
}

void TimerQueue::addTimer(Timer::TimerCallback cb, Timestamp when, double interval) {
    auto* timer = new Timer(std::move(cb), when, interval);
    // 若这个新定时器最早到期，需要重设 timerfd
    const bool earliestChanged = insert(timer);
    if (earliestChanged) {
        resetTimerfd(timerfd_, when);
    }
}

bool TimerQueue::insert(Timer* timer) {
    bool earliest = false;
    const Timestamp when = timer->expiration();
    const auto it = timers_.begin();
    if (it == timers_.end() || when < it->first) {
        earliest = true;
    }
    timers_.insert(Entry(when, timer));
    return earliest;
}

void TimerQueue::handleRead() {
    loop_->assertInLoopThread();

    const Timestamp nowT = Clock::now();
    readTimerfd(timerfd_);

    // 取出所有已到期的定时器（从 timers_ 中摘除），逐个执行
    const std::vector<Entry> expired = getExpired(nowT);
    for (const Entry& entry : expired) {
        entry.second->run();
    }
    // 重复定时器重新排入队列
    reset(expired, nowT);
}

std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp nowT) {
    std::vector<Entry> expired;

    // 哨兵：时间=nowT、指针=最大地址。lower_bound 会落在"所有 <= nowT 的条目"之后，
    // 于是 [begin, end) 恰好是所有到期条目（含恰好等于 nowT 的）。
    const Entry sentry(nowT, reinterpret_cast<Timer*>(UINTPTR_MAX));
    const auto end = timers_.lower_bound(sentry);

    std::copy(timers_.begin(), end, std::back_inserter(expired));
    timers_.erase(timers_.begin(), end);
    return expired;
}

void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp nowT) {
    for (const Entry& entry : expired) {
        Timer* timer = entry.second;
        if (timer->repeat()) {
            timer->restart(nowT);       // 排下一轮
            insert(timer);
        } else {
            delete timer;               // 一次性定时器：用完即销
        }
    }

    // 若队列里还有定时器，把内核定时器重设到"最近那个"
    if (!timers_.empty()) {
        resetTimerfd(timerfd_, timers_.begin()->second->expiration());
    }
}
