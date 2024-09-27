#define ZSKIPLIST_MAXLEVEL 32 // 32层足够2^64个元素
#define ZSKIPLIST_P 0.25	// sjiplist P = 1/2

typedef struct zskiplistNode zskiplistNode;
typedef void (*handle_pt) (zskiplistNode* node);

struct zskiplistNode {
    // double score
    unsigned long score;    // 时间戳
    handle_pt handler;
    /*struct zskiplistNode *backward; 从后向前遍历时使用*/
    struct zskiplistLevel {
        struct zskiplistNode* forward;
        /*unsigned long span; 这个存储的level间节点的个数,在定时器中并不重要*/
    } level[];
};

typedef struct zkiplist {
    // 添加一个free的函数
    struct zskiplistNode* header;
    int length;
    int level;
}zskiplist;

zskiplist* zslCreate(void);
void zslFree(zskiplist* zsl);
zskiplistNode* zslInsert(zskiplist* zsl, unsigned long score, handle_pt func);
zskiplistNode* zslMin(zskiplist* zsl);
void zslDeleteHead(zskiplist *zsl);
void zslDelete(zskiplist* zsl,zskiplistNode* zn);

void zslPrint(zskiplist* zsl);

