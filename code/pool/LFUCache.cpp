#include "LFUCache.h"
#include "MemPool.h"
#include <assert.h>

void KeyList::init(int freq)
{
    freq_ = freq;
    // dummyhead_ = tail_ = new Node<Key>
    Dummyhead_ = newElement<Node<Key>>();
    tail_ = Dummyhead_;
    Dummyhead_->setNext(nullptr);
}

// 删除整个链表
void KeyList::destory()
{
    while(Dummyhead_ != nullptr)
    {
        key_node pre = Dummyhead_;
        Dummyhead_ = Dummyhead_->getNext();
        // delete pre;
        deleteElement(pre);
    }
}

int KeyList::getFreq() { return freq_;}

// 将节点添加到链表头部
void KeyList::add(key_node& node)
{
    if(Dummyhead_->getNext() == nullptr)
    {
        tail_ = node;
    }
    else
    {
        Dummyhead_->getNext()->setPre(node);
    }
    node->setNext(Dummyhead_->getNext());
    node->setPre(Dummyhead_);
    Dummyhead_->setNext(node);

    assert(!isEmpty());
}

// 删除小链表中的节点
void KeyList::del(key_node& node)
{
    node->getPre()->setNext(node->getNext());
    if(node->getNext() == nullptr)
    {
        tail_ = node->getPre();
    }
    else
    {
        node->getNext()->setPre(node->getPre());
    }
}

bool KeyList::isEmpty()
{
    return Dummyhead_ == tail_;
}

key_node KeyList::getLast()
{
    return tail_;
}

LFUCache::LFUCache(int capacity) : capacity_(capacity)
{
    init();
}

LFUCache::~LFUCache()
{
    while(Dummyhead_)
    {
        freq_node pre = Dummyhead_;
        Dummyhead_ = Dummyhead_->getNext();
        pre->getValue().destory();
        // delete pre;
        deleteElement(pre);
    }
}

void LFUCache::init()
{
    // FIXME:缓存的容量动态变化
    // Dummyhead_ = new Node<KeyList>();
    Dummyhead_ = newElement<Node<KeyList>>();
    Dummyhead_->getValue().init(0);
    Dummyhead_->setNext(nullptr);
}

// 更新节点频度
// 如果不存在下一个频度的链表,则增加一个
// 然后将当前节点放到下一个频度的链表的头位置
void LFUCache::addFreq(key_node& nowk,freq_node& nowf)
{
    freq_node nxt;
    // FIXME:频数可能溢出
    if(nowf->getNext() == nullptr || nowf->getNext()->getValue().getFreq() != nowf->getValue().getFreq() +1)
    {
        // 新建一个下一个平度的大链表，加到nowf后面
        nxt = newElement<Node<KeyList>>();
        nxt->getValue().init(nowf->getValue().getFreq()  +1);
        if(nowf->getNext() != nullptr)
            nowf->getNext()->setPre(nxt);
        nxt->setNext(nowf->getNext());
        nowf->setNext(nxt);
        nxt->setPre(nowf);
    }
    else
    {
        nxt = nowf->getNext();
    }
    nxt->getValue().add(nowk);
    
    assert(!nxt->getValue().isEmpty());

    // 如果该频度的小链表已经空了
    if(nowf != Dummyhead_ && nowf->getValue().isEmpty())
        del(nowf);

}

bool LFUCache::get(string& key,string& val)
{
    if(!capacity_) return false;
    MutexLockGuard lock(mutex_);
    if(fmap_.find(key) != fmap_.end())
    {
        // 缓存命中
        key_node nowk = kmap_[key];
        freq_node nowf = fmap_[key];
        val += nowk->getValue().value_;
        addFreq(nowk,nowf);
        return true;
    }
    // 未命中
    return false;
}

void LFUCache::set(string& key,string& val)
{
    if(!capacity_) return;
    MutexLockGuard lock(mutex_);

    // 缓存满了
    // 从频度最小的小链表中的节点中删除最后一个节点(小链表中的删除符合LRU)
    if(kmap_.size() == capacity_)
    {
        freq_node head = Dummyhead_->getNext();
 		key_node last = head->getValue().getLast();
        head->getValue().del(last);
        kmap_.erase(last->getValue().key_);
        fmap_.erase(last->getValue().key_);
        // delete last;
        deleteElement(last);
        // 如果频度最⼩的链表已经没有节点，就删除
        if(head->getValue().isEmpty()) {
            del(head);

        }
    }
    // 使用内存池
    key_node nowk = newElement<Node<Key>>();
    nowk->getValue().key_ = key;
    nowk->getValue().value_ = val;
    addFreq(nowk,Dummyhead_);
    kmap_[key] = nowk;
    fmap_[key] = Dummyhead_->getNext();
}

void LFUCache::del(freq_node& node)
{
    node->getPre()->setNext(node->getNext());
    if(node->getNext() != nullptr)
    {
        node->getNext()->setPre(node->getPre());
    }
    node->getValue().destory();
    deleteElement(node);
}

LFUCache& getCache()
{
    static LFUCache cache(LFU_CAPACITY);
    return cache;
}