#define _ENABLE_ATOMIC_ALIGNMENT_FIX
#include <shared_mutex>
#include <mutex>
#include <thread>
#include <vector>
#include <iomanip>
#include <iostream>
#include <atomic>
#include <cassert>


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
	using tag_type = uint32_t;

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
		std::atomic<mark_ptr_tag> a;
		std::cout << std::boolalpha << "Lockfree: " << a.is_lock_free() << " size: " << sizeof(mark_ptr_tag) << std::endl;
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
			mark_ptr_tag desired = { false, new_node, r.lesser_tag() + 1 };
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
				mark_ptr_tag desired = { true, next, r.cur_tag() + 1 };
				if (!std::atomic_compare_exchange_strong(&cur.mark_next_tag, &expected, desired))
					continue;
			}
			{
				mark_ptr_tag expected = { false, &cur, r.prev_tag() };
				mark_ptr_tag desired = { false, next, r.prev_tag() + 1};
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
				mark_ptr_tag desired = { false, r.next(), r.prev_tag() + 1 };
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






//mutex_set<int, std::shared_mutex, true> TheSet;
lockfree_set<int> TheSet;

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




int main()
{
	/*using namespace std;
	clock_t begin = clock();

	vector<thread> threads;

	for (int i = 0; i < 8; ++i)
	{
		run();
		//threads.emplace_back(std::thread(&run));
	}

	for (thread& t : threads)
	{
		t.join();
	}

	clock_t end = clock();
	double elapsed_secs = double(end - begin) / CLOCKS_PER_SEC;

	std::cout << elapsed_secs << std::endl;*/

	lockfree_set<int> set;

	set.insert(3);
	set.insert(5);
	set.insert(7);
	
#define OUT(n) std::cout << std::boolalpha << "has(" << n << "): " << set.has(n) << std::endl;

	OUT(2);
	OUT(3);
	OUT(4);
	OUT(5);
	OUT(6);
	OUT(7);
	OUT(8);

	std::cout << "===== Without: 5 ======\n";
	set.remove(5);

	OUT(2);
	OUT(3);
	OUT(4);
	OUT(5);
	OUT(6);
	OUT(7);
	OUT(8);


	std::cout << "===== Without: 4 ======\n";
	set.remove(4);

	OUT(2);
	OUT(3);
	OUT(4);
	OUT(5);
	OUT(6);
	OUT(7);
	OUT(8);


	std::cout << "===== Without: 3 ======\n";
	set.remove(3);

	OUT(2);
	OUT(3);
	OUT(4);
	OUT(5);
	OUT(6);
	OUT(7);
	OUT(8);


	std::cout << "===== Without: 6 ======\n";
	set.remove(6);

	OUT(2);
	OUT(3);
	OUT(4);
	OUT(5);
	OUT(6);
	OUT(7);
	OUT(8);


	std::cout << "===== Without: 7 ======\n";
	set.remove(7);

	OUT(2);
	OUT(3);
	OUT(4);
	OUT(5);
	OUT(6);
	OUT(7);
	OUT(8);//*/

	std::cin.get();
	return 0;
}