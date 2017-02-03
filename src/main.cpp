#define _ENABLE_ATOMIC_ALIGNMENT_FIX
#include <shared_mutex>
#include <mutex>
#include <thread>
#include <vector>
#include <iomanip>
#include <iostream>
#include <atomic>
#include <cassert>
#include <random>
#include <numeric>
#include <string>
#include <algorithm>


template<typename T>
struct std_allocator
{
	using ptr = std::unique_ptr<T>;

	template<typename... Args>
	ptr allocate(Args&&... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	template<typename... Args>
	T* allocate_raw(Args&&... args)
	{
		return new T(args...);
	}

	void deallocate_raw(T* e)
	{
	}
};


struct no_mutex
{
	using native_handle_type = void;

	void try_lock() {}
	void lock() {}
	void unlock() {}
	native_handle_type native_handle() {}
};


template<typename E, typename Mutex = no_mutex, bool supports_shared_mutex = false, template<class> class Allocator = std_allocator>
class mutex_set
{
private:
	struct node;
	typedef Allocator<node> allocator_type;
	typedef typename allocator_type::ptr ptr;
	using shared_lock = typename std::conditional<supports_shared_mutex, std::shared_lock<Mutex>, std::unique_lock<Mutex>>::type;

	struct node
	{
		node(const E& key, ptr next)
			: key(key)
			, next(std::move(next))
		{}

		const E key;
		ptr next = nullptr;
	};



	struct search_result
	{
		ptr& found;
		bool key_match;
	};
public:

	bool insert(const E& e)
	{
		std::unique_lock<Mutex> lck(mMutex);
		auto result = search(e);

		if (result.key_match)
			return false;

		result.found = mAllocator.allocate(e, std::move(result.found));
		return true;
	}

	bool remove(const E& e)
	{
		std::unique_lock<Mutex> lck(mMutex);
		auto result = search(e);

		if (!result.key_match)
			return false;
		result.found = std::move(result.found->next);
		return true;
	}

	bool has(const E& e)
	{
		shared_lock lck(mMutex);
		return search(e).key_match;
	}

private:
	search_result search(const E& e)
	{
		ptr* prev_ptr = &head;
		
		while (*prev_ptr)
		{
			auto& cur = **prev_ptr;
			const auto& key = cur.key;
			if (key >= e)
			{
				return{ *prev_ptr, key == e };
			}
			prev_ptr = &cur.next;
		}
		return{ *prev_ptr, false };
	}


private:
	allocator_type mAllocator;
	Mutex mMutex;
	ptr head = nullptr;
};



template<typename E, template<class> class Allocator = std_allocator>
class lockfree_set
{
private:
	struct node;
	typedef Allocator<node> allocator_type;
	using tag_type = uint16_t;

	struct mark_ptr_tag
	{
		mark_ptr_tag() = default;
		mark_ptr_tag(bool mark, node* ptr, tag_type tag)
			: mark(mark)
			, ptr(ptr)
			, tag(tag)
		{}

		bool operator ==(const mark_ptr_tag& rhs) const
		{
			return mark == rhs.mark && ptr == rhs.ptr && tag == rhs.tag;
		}

		bool operator !=(const mark_ptr_tag& rhs) const
		{
			return !(*this == rhs);
		}

		node* ptr = nullptr;
		tag_type tag = 0;
		bool mark = false;
	};

	struct node
	{
		node(const E& key)
			: key(key)
		{
		}

		const E key;
		std::atomic<mark_ptr_tag> mark_next_tag;
	};


	struct search_result
	{
		mark_ptr_tag prev_mnt_value;
		mark_ptr_tag next_mnt_value;
		std::atomic<mark_ptr_tag>* prev_mnt_addr;
		bool key_match;

		node* next()
		{
			assert(key_match);
			return next_mnt_value.ptr;
		}

		node* cur()
		{
			assert(key_match);
			return prev_mnt_value.ptr;
		}

		node* greater_ptr()
		{
			assert(!key_match);
			return prev_mnt_value.ptr;
		}

		tag_type lesser_tag()
		{
			assert(!key_match);
			return prev_mnt_value.tag;
		}

		tag_type prev_tag()
		{
			assert(key_match);
			return prev_mnt_value.tag;
		}

		tag_type cur_tag()
		{
			assert(key_match);
			return next_mnt_value.tag;
		}

		bool cur_marked()
		{
			assert(key_match);
			return next_mnt_value.mark;
		}
	};
public:
	lockfree_set()
	{
	}

	bool insert(const E& e)
	{
		node* new_node = mAllocator.allocate_raw(e);

		while (true)
		{
			auto r = search(e);
			if (r.key_match)
				return false;

			mark_ptr_tag new_mnt = { false, r.greater_ptr(), 0 };
			new_node->mark_next_tag.store(new_mnt);
			mark_ptr_tag expected = { false, r.greater_ptr(), r.lesser_tag() };
			mark_ptr_tag desired = { false, new_node, tag_type(r.lesser_tag() + 1) };
			if (std::atomic_compare_exchange_strong(r.prev_mnt_addr, &expected, desired))
				return true;
		}
	}

	bool remove(const E& e)
	{
		while (true)
		{
			auto r = search(e);
			if (!r.key_match)
				return false;

			node& cur = *r.cur();
			node* next = r.next();
			{
				mark_ptr_tag expected = { false, next, r.cur_tag() };
				mark_ptr_tag desired = { true, next, tag_type(r.cur_tag() + 1) };
				if (!std::atomic_compare_exchange_strong(&cur.mark_next_tag, &expected, desired))
					continue;
			}
			{
				mark_ptr_tag expected = { false, &cur, r.prev_tag() };
				mark_ptr_tag desired = { false, next, tag_type(r.prev_tag() +  1) };
				if (std::atomic_compare_exchange_strong(r.prev_mnt_addr, &expected, desired))
					mAllocator.deallocate_raw(&cur);
				else
					search(e);
			}
			return true;
		}
	}

	bool has(const E& e)
	{
		return search(e).key_match;
	}

private:
	search_result search(const E& e)
	{
	try_agian:
		search_result r;
		r.key_match = true;
		r.prev_mnt_addr = &head;
		r.prev_mnt_value = head.load();
		auto& prev = r.prev_mnt_addr;
		while (true)
		{
			if (r.cur() == nullptr)
			{
				r.key_match = false;
				return r;
			}
			r.next_mnt_value = r.cur()->mark_next_tag.load();
			const auto& ckey = r.cur()->key;
			{
				mark_ptr_tag expected = { false, r.cur(), r.prev_tag() };
				if (prev->load() != expected)
					goto try_agian;
			}
			if (!r.cur_marked())
			{
				if (ckey >= e)
				{
					r.key_match = (ckey == e);
					return r;
				}
				else {
					prev = &r.cur()->mark_next_tag;
				}
			}
			else {
				mark_ptr_tag expected = { false, r.cur(), r.prev_tag() };
				mark_ptr_tag desired = { false, r.next(), tag_type(r.prev_tag() + 1) };
				if (std::atomic_compare_exchange_strong(r.prev_mnt_addr, &expected, desired))
				{
					mAllocator.deallocate_raw(r.cur());
					r.next_mnt_value.tag = r.prev_tag() + 1;
				}
				else
					goto try_agian;
			}
			r.prev_mnt_value = r.next_mnt_value;
		}
	}


private:
	allocator_type mAllocator;
	std::atomic<mark_ptr_tag> head;
};



enum class Op
{
	Insert,
	Delete,
	Search
};


struct bench_statistic
{
	bench_statistic() = default;
	bench_statistic(const bench_statistic&) = default;
	bench_statistic(std::size_t i_s, std::size_t i_f, std::size_t d_s, std::size_t d_f, std::size_t s_s, std::size_t s_f)
		: insert_succ(i_s)
		, insert_fail(i_f)
		, delete_succ(d_s)
		, delete_fail(d_f)
		, search_succ(s_s)
		, search_fail(s_f)
	{}
	std::size_t insert_succ = 0;
	std::size_t insert_fail = 0;
	std::size_t delete_succ = 0;
	std::size_t delete_fail = 0;
	std::size_t search_succ = 0;
	std::size_t search_fail = 0;
};

struct bench_result: bench_statistic
{
	bench_result() = default;
	bench_result(const bench_statistic& stats, std::size_t threads, std::chrono::nanoseconds ns, std::size_t ops)
		: bench_statistic(stats)
		, num_threads(threads)
		, time_needed(ns)
		, operations(ops)
	{}

	std::size_t operations = 0;
	std::size_t num_threads = 0;
	std::chrono::nanoseconds time_needed;
};

class benchmark
{
public:
	benchmark(int times, int threads, int range, int inserts, int deletions, int searches)
		: mTimes(times)
		, mThreads(threads)
		, mInsertSupplier(threads)
		, mDeleteSupplier(threads)
		, mHasSupplier(threads)
		, mOpSupplier(threads)
	{
		assert(mTimes >= 3);
		for (int t = 0; t < threads; ++t)
		{
			auto& insert_vec = mInsertSupplier[t];
			auto& delete_vec = mDeleteSupplier[t];
			auto& search_vec = mHasSupplier[t];
			auto& op_vec = mOpSupplier[t];

			auto fill = [range](std::vector<int>& target, int num)
			{
				for (int i = 0; i < num; ++i)
					target.emplace_back(i % range);

				std::default_random_engine rd{ 10 };
				std::mt19937 g(rd());

				std::shuffle(target.begin(), target.end(), g);
			};

			fill(insert_vec, inserts);
			fill(delete_vec, deletions);
			fill(search_vec, searches);

			{
				op_vec.insert(op_vec.end(), inserts, Op::Insert);
				op_vec.insert(op_vec.end(), deletions, Op::Delete);
				op_vec.insert(op_vec.end(), searches, Op::Search);

				std::default_random_engine rd{ 10 };
				std::mt19937 g(rd());

				std::shuffle(op_vec.begin(), op_vec.end(), g);

				mOperations = op_vec.size();
			}
		}
	}


	template<typename SetImpl>
	bench_result run()
	{
		std::vector<bench_result> results{};
		for (int time = 0; time < mTimes; ++time)
		{
			SetImpl theSet{};
			std::atomic<bool> start = false;
			std::vector<std::thread> threads;
			std::vector<bench_statistic> stats{ unsigned(mThreads) , bench_statistic{} };

			for (int thread = 0; thread < mThreads; ++thread)
			{
				threads.emplace_back([&start, &stat = stats[thread], thread, &theSet, this]()
				{
					auto& insert_vec = mInsertSupplier[thread];
					auto& delete_vec = mDeleteSupplier[thread];
					auto& search_vec = mHasSupplier[thread];
					auto& op_vec = mOpSupplier[thread];

					while (!start.load())
					{
						// warmup
					}

					auto insert_it = insert_vec.cbegin();
					auto delete_it = delete_vec.cbegin();
					auto search_it = search_vec.cbegin();

					for (auto op : op_vec)
					{
						switch (op)
						{
						case Op::Insert:
							{
								bool succ = theSet.insert(*(insert_it++));
								++(succ ? stat.insert_succ : stat.insert_fail);
							}break;
						case Op::Delete:
							{
								bool succ = theSet.remove(*(delete_it++));
								++(succ ? stat.delete_succ : stat.delete_fail);
							}break;
						case Op::Search:
							{
								bool succ = theSet.has(*(search_it++));
								++(succ ? stat.search_succ : stat.search_fail);
							}break;
						}
					}
				});
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			start = true;
			auto start_time = std::chrono::high_resolution_clock::now();
			

			for (auto& t : threads)
				t.join();
			auto end_time = std::chrono::high_resolution_clock::now();

			bench_statistic combined = std::accumulate(stats.begin(), stats.end(), bench_statistic{}, 
				[](const bench_statistic& lhs, const bench_statistic& rhs)
				{
					const bench_statistic s = {
						lhs.insert_succ + rhs.insert_succ,
						lhs.insert_fail + rhs.insert_fail,
						lhs.delete_succ + rhs.delete_succ,
						lhs.delete_fail + rhs.delete_fail,
						lhs.search_succ + rhs.search_succ,
						lhs.search_fail + rhs.search_fail
					};
					return s;
				});
			results.emplace_back(combined, mThreads, std::chrono::nanoseconds{end_time - start_time}, mOperations);
		}
		
		bench_result end_res = results.front();
		std::vector<std::chrono::nanoseconds> time_result{ results.size(), std::chrono::nanoseconds(0) };
		std::transform(results.cbegin(), results.cend(), time_result.begin(), [](const bench_result res) { return res.time_needed; });
		std::sort(time_result.begin(), time_result.end());
		time_result.erase(time_result.begin());
		time_result.pop_back();
		end_res.time_needed = std::accumulate(time_result.cbegin(), time_result.cend(), std::chrono::nanoseconds(0), [](const std::chrono::nanoseconds& lhs, const std::chrono::nanoseconds& rhs){ return lhs + rhs; }) / time_result.size();
		return end_res;
	}


private:
	const int mTimes;
	const int mThreads;
	std::size_t mOperations;
	std::vector<std::vector<int>> mInsertSupplier;
	std::vector<std::vector<int>> mDeleteSupplier;
	std::vector<std::vector<int>> mHasSupplier;
	std::vector<std::vector<Op>> mOpSupplier;
	
};





//mutex_set<int, std::shared_mutex, true> TheSet;
/*lockfree_set<int> TheSet;

void run()
{
	for (int i = 0; i < 100000; ++i)
	{
		int e = rand() % 259;
		if (rand() % 3)
			TheSet.has(e);
		else if (rand() % 3)
			TheSet.insert(e);
		else
			TheSet.remove(e);
	}
}
*/



int main()
{

	for (int threads = 1; threads <= 32; ++threads)
	{
		for (int r = 1; r <= 5; ++r)
		{
			int range = 1000 << r;
			for (int nf = 1; nf <= 5; ++nf)
			{
				int num = 10000 << nf;
				benchmark b{ 10, threads, range, num, num, num };

				auto print = [threads, range, num](const std::string& name, const bench_result& res)
				{
					std::cout << name << ", " << threads << ", " << range << ", " << num << ", " << res.operations << ", "
						<< res.insert_succ << ", " << res.insert_fail << ", "
						<< res.delete_succ << ", " << res.delete_fail << ", "
						<< res.search_succ << ", " << res.search_fail << ", "
						<< res.time_needed.count() << std::endl;
				};

				if(threads == 1)
				{
					auto res = b.run<mutex_set<int, no_mutex, false>>();
					print("seq", res);
				}

				{
					auto res = b.run<mutex_set<int, std::mutex, false>>();
					print("mutex", res);
				}

				{
					auto res = b.run<mutex_set<int, std::shared_mutex, true>>();
					print("rw-mutex", res);
				}

				{
					auto res = b.run<lockfree_set<int>>();
					print("lockfree", res);
				}
			}
		}
	}

	std::cin.get();
	return 0;
}