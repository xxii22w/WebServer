#include <atomic>
#include <utility>

// 非侵入式就是插入节点
template <typename T>
class MPSCQueueNonIntrusive
{
public:
    MPSCQueueNonIntrusive() : _head(new Node()), _tail(_head.load(std::memory_order_relaxed))
    {
        Node* front = _head.load(std::memory_order_relaxed);
        front->Next.store(nullptr,std::memory_order_relaxed);
    }

    ~MPSCQueueNonIntrusive()
    {
        T* output;
        while(Dequeue(output))
            delete output;

        Node* front = _head.load(std::memory_order_relaxed);
        delete front;
    }

// wait-free
    void Enqueue(T* input)
    {
        Node* node = new Node(input);
        // memory_order_acq_rel保证前面的内存序不能排在后面，后面的内存序不能排在前面
        Node* prevHead = _head.exchange(node,std::memory_order_acq_rel);
        prevHead->Next.store(node,std::memory_order_release);
    }

    bool Dequeue(T*& result)
    {
        Node* tail = _tail.load(std::memory_order_relaxed);
        Node* next = tail->Next.load(std::memory_order_acquire); // 保证内存序后面的不能重排到前面
        if(!next)
            return false;
        
        result = next->Data;
        _tail.store(next,std::memory_order_release); // 保证前面的操作已经完成
        delete tail;
        return true;
    }


private:
    struct Node
    {
        Node() = default;
        explicit Node(T* data) : Data(data){
            Next.store(nullptr,std::memory_order_relaxed);  // 松散的内存序 不保证有序
        }

        T* Data;
        std::atomic<Node*> Next;
    };
	
    std::atomic<Node*> _head;
    std::atomic<Node*> _tail;
    MPSCQueueNonIntrusive(MPSCQueueNonIntrusive const&) = delete;
    MPSCQueueNonIntrusive& operator=(MPSCQueueNonIntrusive const&) = delete;

};

// 侵入式就是在节点里面的
template <typename T, std::atomic<T*>T::* IntrusiveLink>
class MPSCQueueIntrusive
{
public:
    MPSCQueueIntrusive() :_dummyPtr(reinterpret_cast<T*>(std::addressof(_dummy))),_head(_dummyPtr),_tail(_dummyPtr)
    {
        std::atomic<T*>* dummyNext = new (&(_dummyPtr->*IntrusiveLink)) std::atomic<T*>();
        dummyNext->store(nullptr, std::memory_order_relaxed);
    }

    ~MPSCQueueIntrusive()
    {
        T* output;
        while(Dequeue(output))
            delete output;
    } 

    void Enqueue(T* input)
    {
        (input->*IntrusiveLink).store(nullptr,std::memory_order_release);
        T* prevHead = _head.exchange(input,std::memory_order_acq_rel);
        (prevHead->*IntrusiveLink).store(input,std::memory_order_release);
    }

    bool Dequeue(T*& result)
    {
        T* tail = _tail.load(std::memory_order_relaxed);
        T* next = (tail->*IntrusiveLink).load(std::memory_order_acquire);
        if(tail == _dummyPtr)
        {
            if(!next)
                return false;
            _tail.store(next,std::memory_order_release);
            tail = next;
            next = (next->*IntrusiveLink).load(std::memory_order_acquire);
        }

        if(next)
        {
            _tail.store(next,std::memory_order_release);
            result = tail;
            return true;
        }

        T* head = _head.load(std::memory_order_acquire);
        if(tail != head)
            return false;
        
        Enqueue(_dummyPtr);
        next = (tail->*IntrusiveLink).load(std::memory_order_acquire);
        if(next)
        {
            _tail.store(next,std::memory_order_release);
            result = tail;
            return true;
        }
        return false;
    }

private:
    std::aligned_storage_t<sizeof(T),alignof(T)> _dummy;
    T* _dummyPtr;
    std::atomic<T*> _head;
    std::atomic<T*>  _tail;

    MPSCQueueIntrusive(MPSCQueueIntrusive const&) = delete;
    MPSCQueueIntrusive operator=(MPSCQueueIntrusive const&) = delete;
};

template<typename T, std::atomic<T*> T::* IntrusiveLink = nullptr>
using MPSCQueue = std::conditional_t<IntrusiveLink != nullptr, MPSCQueueIntrusive<T, IntrusiveLink>, MPSCQueueNonIntrusive<T>>;

/*
#include "MPSCQueue.h"
#include <thread>
#include <iostream>

struct Count {
    Count(int _v) : v(_v) {}
    int v;
};


int main()
{
    MPSCQueue<Count> queue;
    std::thread pd1([&](){
        queue.Enqueue(new Count(100));
        queue.Enqueue(new Count(200));
        queue.Enqueue(new Count(300));
        queue.Enqueue(new Count(400));
    });


    std::thread pd2([&]() {
        queue.Enqueue(new Count(500));
        queue.Enqueue(new Count(600));
        queue.Enqueue(new Count(700));
        queue.Enqueue(new Count(800));
    });

    std::cout << "begin: " << std::endl;
    std::thread cs1([&]() {
        Count* ele;
        while(queue.Dequeue(ele)) {
            std::cout << std::this_thread::get_id() << " : pop " << ele->v << std::endl;
            delete ele;
        }
    });

    std::cout << "end: " << std::endl;

    pd1.join();
    pd2.join();
    cs1.join();
    
    return 0;
}
*/