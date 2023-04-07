#pragma once
#if defined(_MSC_VER)
#include <memory>
#include <concurrent_queue.h>
namespace xrconcurrency
{
    template < typename Tp, typename Alloc = std::allocator<Tp> >
    using concurrent_queue = concurrency::concurrent_queue<Tp, Alloc>;
}
#else
#include <memory>
#include <queue>
#include <mutex>
#include <shared_mutex>
namespace xrconcurrency
{
	template < typename Tp, typename Alloc = std::allocator<Tp> >
	class concurrent_queue
	{
		using QueueT = std::queue<Tp, std::deque<Tp,Alloc> >;
		mutable std::shared_mutex m_mutex;
		QueueT m_queue;
	public:
        concurrent_queue() = default;

        // TODO: Implement these deleted functions.
        concurrent_queue(const concurrent_queue&) = delete;
        concurrent_queue(concurrent_queue&&) = delete;
        concurrent_queue& operator=(const concurrent_queue&) = delete;
        concurrent_queue& operator=(concurrent_queue&&) = delete;

        void push(const Tp& x)
        {
            std::unique_lock l(m_mutex);
            m_queue.push(std::move(x));
        }

        void push(Tp&& x)
        {
            std::unique_lock l(m_mutex);
            m_queue.push(std::move(x));
        }

        bool try_pop(Tp& x)
        {
            std::unique_lock l(m_mutex, std::try_to_lock_t{});
            if (!l.owns_lock())
                return false;
            if (m_queue.empty()) return false;
            x = std::move(m_queue.front());
            m_queue.pop();
            return true;
        }
	};
}
#endif
