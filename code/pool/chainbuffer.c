
#include <string.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "buffer.h"

// 链表
struct buf_chain_s {
    struct buf_chain_s *next;
    uint32_t buffer_len;
    uint32_t misalign;
    uint32_t off;
    uint8_t *buffer;
};

// 数组，first指向第一个链表，last指向最有一个链表，如果要加链表，只需要创建出一个，然后用next指向就行，然后调整尾部指针
/*

	用户态读缓冲区用定长buffer， c++可用std::vector来实现
int
event_buffer_read(event_t *e) {
    int fd = e->fd;
    int num = 0;
    while (1) {
        char buf[1024] = {0};
        int n = read(fd, buf, 1024);
        if (n == 0) {
            printf("close connection fd = %d\n", fd);
            if (e->error_fn)
                e->error_fn(fd, "close socket");
            del_event(e->r, e);
            close(fd);
            return 0;
        } else if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EWOULDBLOCK)
                break;
            printf("read error fd = %d err = %s\n", fd, strerror(errno));
            if (e->error_fn)
                e->error_fn(fd, strerror(errno));
            del_event(e->r, e);
            close(fd);
            return 0;
        } else {
            printf("recv data from client:%s", buf);
            buffer_add(evbuf_in(e), buf, n);
        }
        num += n;
    }
    return num;
}

    用户态写缓冲区用 chainbuffer, c++可以std::queue来实现

int
event_buffer_write(event_t *e, void * buf, int sz) {
    buffer_t *out = evbuf_out(e);
    if (buffer_len(out) == 0) {
        int n = _write_socket(e, buf, sz);
        if (n == 0 || n < sz) {
            // 发送失败，除了将没有发送出去的数据写入缓冲区，还要注册写事件
            buffer_add(out, (char *)buf+n, sz-n);
            enable_event(e->r, e, 1, 1);
            return 0;
        } else if (n < 0) 
            return 0;
        return 1;
    }
    buffer_add(out, (char *)buf, sz);
    return 1;
}


    定长数组:
    chae buff[16 * 1024*1024];
    uint offset
    优点: 结构简单，易于实现
    缺点: 需要频繁罗东数据 需要实现扩容机制

    ringbuffer:
    char buff[16* 1024*1024];
    uint head;
    uint tail;
    逻辑上连续的空间，通过取余操作实现
    head % size tail % size
    优点: 不需要挪动数据
    缺点: 需要事先扩容机制  造成不连续空间，可能引发多次系统调用

    chainbuff:
    |------|        |--------------------------------------------------|
    |first | -----> |  next  | misalign | off | buffer                 |                                        |
    |      |        |--------------------------------------------------|
    |      |            |
    |      |            |
    |      |        |--------------------------------------------------|
    |last  | -----> |  next  | misalign | off  | buffer	               |										   |
    |------|		|--------------------------------------------------|
	优点: 不需要挪动数据 动态扩缩容，不挪动数据

*/
struct buffer_s {
    buf_chain_t *first;
    buf_chain_t *last;
    buf_chain_t **last_with_datap;
    uint32_t total_len;
    uint32_t last_read_pos; // for sep read
};

#define CHAIN_SPACE_LEN(ch) ((ch)->buffer_len - ((ch)->misalign + (ch)->off))
#define MIN_BUFFER_SIZE 1024
#define MAX_TO_COPY_IN_EXPAND 4096
#define BUFFER_CHAIN_MAX_AUTO_SIZE 4096
#define MAX_TO_REALIGN_IN_EXPAND 2048
#define BUFFER_CHAIN_MAX 16*1024*1024  // 16M
#define BUFFER_CHAIN_EXTRA(t, c) (t *)((buf_chain_t *)(c) + 1)
#define BUFFER_CHAIN_SIZE sizeof(buf_chain_t)

uint32_t
buffer_len(buffer_t *buf) {
    return buf->total_len;
}

buffer_t *
buffer_new(uint32_t sz) {
    (void)sz;
    buffer_t * buf = (buffer_t *) malloc(sizeof(buffer_t));
    if (!buf) {
        return NULL;
    }
    memset(buf, 0, sizeof(*buf));
    buf->last_with_datap = &buf->first;
    return buf;
}

static buf_chain_t *
buf_chain_new(uint32_t size) {
    buf_chain_t *chain;
    uint32_t to_alloc;
    if (size > BUFFER_CHAIN_MAX - BUFFER_CHAIN_SIZE)
        return (NULL);
    size += BUFFER_CHAIN_SIZE;
    
    if (size < BUFFER_CHAIN_MAX / 2) {
        to_alloc = MIN_BUFFER_SIZE;
        while (to_alloc < size) {
            to_alloc <<= 1;
        }
    } else {
        to_alloc = size;
    }
    if ((chain = malloc(to_alloc)) == NULL)
        return (NULL);
    memset(chain, 0, BUFFER_CHAIN_SIZE);
    chain->buffer_len = to_alloc - BUFFER_CHAIN_SIZE;
    chain->buffer = BUFFER_CHAIN_EXTRA(uint8_t, chain);
    return (chain);
}

static void 
buf_chain_free_all(buf_chain_t *chain) {
    buf_chain_t *next;
    for (; chain; chain = next) {
        next = chain->next;
        free(chain);
    }
}

void
buffer_free(buffer_t *buf) {
    buf_chain_free_all(buf->first);
}

// 从last_with_datap开始遍历链表，找到第一个偏移量为0的节点。
// 释放从当前节点到链表末尾的所有节点。
static buf_chain_t **
free_empty_chains(buffer_t *buf) {
    buf_chain_t **ch = buf->last_with_datap;
    while ((*ch) && (*ch)->off != 0)
        ch = &(*ch)->next;
    if (*ch) {
        buf_chain_free_all(*ch);
        *ch = NULL;
    }
    return ch;
}

static void
buf_chain_insert(buffer_t *buf, buf_chain_t *chain) {
    if (*buf->last_with_datap == NULL) {
        buf->first = buf->last = chain;
    } else {
        // 如果有数据，释放空的链表节点，并将新节点插入到链表中。
        buf_chain_t **chp;
        chp = free_empty_chains(buf);
        *chp = chain;
        // 更新last_with_datap和last指针
        if (chain->off)
            buf->last_with_datap = chp;
        buf->last = chain;
    }
    buf->total_len += chain->off;
}

static inline buf_chain_t *
buf_chain_insert_new(buffer_t *buf, uint32_t datlen) {
    buf_chain_t *chain;
    if ((chain = buf_chain_new(datlen)) == NULL)
        return NULL;
    buf_chain_insert(buf, chain);
    return chain;
}

// 判断是否需要重新对齐链表节点中的数据。
static int
buf_chain_should_realign(buf_chain_t *chain, uint32_t datlen)
{
    // 检查当前节点剩余空间是否足够，并且当前偏移量小于缓冲区长度的一半，以及当前偏移量小于最大重新对齐限制。
    return chain->buffer_len - chain->off >= datlen &&
        (chain->off < chain->buffer_len / 2) &&
        (chain->off <= MAX_TO_REALIGN_IN_EXPAND);
}


// 将数据从错位的位置移动到缓冲区的起始位置。
static void
buf_chain_align(buf_chain_t *chain) {
    memmove(chain->buffer, chain->buffer + chain->misalign, chain->off);
    chain->misalign = 0;
}

int buffer_add(buffer_t *buf, const void *data_in, uint32_t datlen) {
    buf_chain_t *chain, *tmp;
    const uint8_t *data = data_in;
    uint32_t remain, to_alloc;
    int result = -1;

    // 检查数据长度是否超出缓冲区最大限制
    if (datlen > BUFFER_CHAIN_MAX - buf->total_len) {
        goto done;
    }
	
    // 获取最后一个有数据的链表节点
    if (*buf->last_with_datap == NULL) {
        chain = buf->last;
    } else {
        chain = *buf->last_with_datap;
    }
	
    // 如果没有可用的链表节点，创建一个新的节点
    if (chain == NULL) {
        chain = buf_chain_insert_new(buf, datlen);
        if (!chain)
            goto done;
    }
	
    // 计算当前链表节点的剩余空间
    remain = chain->buffer_len - chain->misalign - chain->off;
    // 如果剩余空间足够，直接将数据复制到当前节点
    if (remain >= datlen) {
        memcpy(chain->buffer + chain->misalign + chain->off, data, datlen);
        chain->off += datlen;
        buf->total_len += datlen;
        // buf->n_add_for_cb += datlen;
        goto out;
    } else if (buf_chain_should_realign(chain, datlen)) {
        // 如果需要重新对齐，调整数据位置
        buf_chain_align(chain);

        memcpy(chain->buffer + chain->off, data, datlen);
        chain->off += datlen;
        buf->total_len += datlen;
        // buf->n_add_for_cb += datlen;
        goto out;
    }
    // 计算新节点需要的分配大小
    to_alloc = chain->buffer_len;
    if (to_alloc <= BUFFER_CHAIN_MAX_AUTO_SIZE/2)
        to_alloc <<= 1;
    if (datlen > to_alloc)
        to_alloc = datlen;
    // 创建一个新的链表节点
    tmp = buf_chain_new(to_alloc);
    if (tmp == NULL)
        goto done;
    // 将剩余数据复制到当前节点
    if (remain) {
        memcpy(chain->buffer + chain->misalign + chain->off, data, remain);
        chain->off += remain;
        buf->total_len += remain;
        // buf->n_add_for_cb += remain;
    }
	// 更新数据指针和长度，准备复制到新节点
    data += remain;
    datlen -= remain;
	// 将新节点插入到缓冲区
    memcpy(tmp->buffer, data, datlen);
    tmp->off = datlen;
    buf_chain_insert(buf, tmp);
    // buf->n_add_for_cb += datlen;
out:
    result = 0;
done:
    return result;
}

static uint32_t
buf_copyout(buffer_t *buf, void *data_out, uint32_t datlen) {
    buf_chain_t *chain;
    char *data = data_out;
    uint32_t nread;

    // 初始化链表节点和要复制的数据长度
    chain = buf->first;
    if (datlen > buf->total_len)
        datlen = buf->total_len;
    if (datlen == 0)
        return 0;
    nread = datlen;
	
    // 遍历链表节点，复制数据到输出缓冲区
    while (datlen && datlen >= chain->off) {
        uint32_t copylen = chain->off;
        memcpy(data,
            chain->buffer + chain->misalign,
            copylen);
        data += copylen;
        datlen -= copylen;

        chain = chain->next;
    }
    if (datlen) {
        memcpy(data, chain->buffer + chain->misalign, datlen);
    }

    return nread;
}

static inline void
ZERO_CHAIN(buffer_t *dst) {
    // 将缓冲区重置为空状态
    dst->first = NULL;
    dst->last = NULL;
    dst->last_with_datap = &(dst)->first;
    dst->total_len = 0;
}

int buffer_drain(buffer_t *buf, uint32_t len) {
    buf_chain_t *chain, *next;
    uint32_t remaining, old_len;

    // 保存当前缓冲区总长度
    old_len = buf->total_len;
    if (old_len == 0)
        return 0;
	
    // 如果清除长度大于等于当前总长度，重置缓冲区
    if (len >= old_len) {
        len = old_len;
        for (chain = buf->first; chain != NULL; chain = next) {
            next = chain->next;
            free(chain);
        }
        ZERO_CHAIN(buf);
    } else {
        // 否则,部分清除缓冲区
        buf->total_len -= len;
        remaining = len;
        for (chain = buf->first; remaining >= chain->off; chain = next) {
            next = chain->next;
            remaining -= chain->off;

            if (chain == *buf->last_with_datap) {
                buf->last_with_datap = &buf->first;
            }
            if (&chain->next == buf->last_with_datap)
                buf->last_with_datap = &buf->first;

            free(chain);
        }

        buf->first = chain;
        chain->misalign += remaining;
        chain->off -= remaining;
    }
    
    // buf->n_del_for_cb += len;
    return len;
}

int buffer_remove(buffer_t *buf, void *data_out, uint32_t datlen) {
    // 从缓冲区复制数据到输出缓冲区，并清除已复制的数据
    uint32_t n = buf_copyout(buf, data_out, datlen);
    if (n > 0) {
        if (buffer_drain(buf, n) < 0)
            n = -1;
    }
    return (int)n;
}

static bool
check_sep(buf_chain_t * chain, int from, const char *sep, int seplen) {
    // 检查指定位置是否包含分隔符
    for (;;) {
        int sz = chain->off - from;
        if (sz >= seplen) {
            return memcmp(chain->buffer + chain->misalign + from, sep, seplen) == 0;
        }
        if (sz > 0) {
            if (memcmp(chain->buffer + chain->misalign + from, sep, sz)) {
                return false;
            }
        }
        chain = chain->next;
        sep += sz;
        seplen -= sz;
        from = 0;
    }
}

int buffer_search(buffer_t *buf, const char* sep, const int seplen) {
    buf_chain_t *chain;
    int i;
    chain = buf->first;
    if (chain == NULL)
        return 0;
    int bytes = chain->off;
    // 找到last_read_pos之后的第一个节点
    while (bytes <= buf->last_read_pos) {
        chain = chain->next;
        if (chain == NULL)
            return 0;
        bytes += chain->off;
    }
    bytes -= buf->last_read_pos;
    int from = chain->off - bytes;
    // 从last_read_pos开始搜索分隔符
    for (i = buf->last_read_pos; i <= buf->total_len - seplen; i++) {
        if (check_sep(chain, from, sep, seplen)) {
            buf->last_read_pos = 0;
            return i+seplen;
        }
        ++from;
        --bytes;
        if (bytes == 0) {
            chain = chain->next;
            from = 0;
            if (chain == NULL)
                break;
            bytes = chain->off;
        }
    }
    buf->last_read_pos = i;
    return 0;
}

uint8_t * buffer_write_atmost(buffer_t *p) {
    buf_chain_t *chain, *next, *tmp, *last_with_data;
    uint8_t *buffer;
    uint32_t remaining;
    int removed_last_with_data = 0;
    int removed_last_with_datap = 0;

    chain = p->first;
    uint32_t size = p->total_len;
	
    // 如果第一个节点就有足够的空间，直接使用
    if (chain->off >= size) {
        return chain->buffer + chain->misalign;
    }
	
    // 计算除了第一个节点外，还需要多少空间来存储数据
    remaining = size - chain->off;
    // 遍历链表，计算剩余需要的空间
    for (tmp=chain->next; tmp; tmp=tmp->next) {
        if (tmp->off >= (size_t)remaining)
            break;
        remaining -= tmp->off;
    }

    // 如果第一个节点的空间加上剩余节点的空间足够，则不需要创建新的节点
    if (chain->buffer_len - chain->misalign >= (size_t)size) {
        /* already have enough space in the first chain */
        size_t old_off = chain->off; // 保存当前偏移量
        buffer = chain->buffer + chain->misalign + chain->off; // 指向当前节点的缓冲区的开始位置
        tmp = chain; // 使用当前节点
        tmp->off = size; // 更新当前节点的偏移量
        size -= old_off; // 减少剩余需要处理的数据大小
        chain = chain->next; // 移动到下一个节点
    } else {
        // 如果空间不足，创建一个新的链表节点
        if ((tmp = buf_chain_new(size)) == NULL) {
            return NULL;
        }
        buffer = tmp->buffer; // 指向新节点的缓冲区
        tmp->off = size; // 设置新节点的偏移量
        p->first = tmp; // 将新节点设置为链表的第一个节点
    }
	
    // 保存最后一个有数据的节点
    last_with_data = *p->last_with_datap;
    // 遍历链表，将数据复制到新缓冲区
    for (; chain != NULL && (size_t)size >= chain->off; chain = next) {
        next = chain->next;// 保存下一个节点
		
        // 复制当前节点的数据到新缓冲区
        if (chain->buffer) {
            memcpy(buffer, chain->buffer + chain->misalign, chain->off);
            size -= chain->off;	    // 减少剩余需要处理的数据大小
            buffer += chain->off;	// 移动缓冲区指针
        }

        // 检查是否一出来最后一个有数据的节点
        if (chain == last_with_data)
            removed_last_with_data = 1;
        // 检查是否已处理last_with_datap指向的节点
        if (&chain->next == p->last_with_datap)
            removed_last_with_datap = 1;

        free(chain);
    }
	
    // 如果还有剩余数据未复制，继续复制到新缓冲区
    if (chain != NULL) {
        memcpy(buffer, chain->buffer + chain->misalign, size);
        chain->misalign += size;
        chain->off -= size;
    } else {
        p->last = tmp;
    }

    tmp->next = chain;

    if (removed_last_with_data) {
        p->last_with_datap = &p->first;
    } else if (removed_last_with_datap) {
        if (p->first->next && p->first->next->off)
            p->last_with_datap = &p->first->next;
        else
            p->last_with_datap = &p->first;
    }
    // 返回新缓冲区的起始地址
    return tmp->buffer + tmp->misalign;
}
