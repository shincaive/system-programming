#include "csapp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>

#define STOCK_FILE "stock.txt"
#define NTHREADS   8
#define SBUFSIZE   16

typedef struct item {
    int ID, left_stock, price;
    struct item *left, *right;
} item_t;

static item_t *root = NULL;
static int listenfd;

typedef struct {
    int *buf;
    int n, front, rear;
    sem_t mutex, slots, items;
} sbuf_t;

static sbuf_t sbuf;

void sbuf_init(sbuf_t *sp, int n) {
    sp->buf   = Malloc(sizeof(int) * n);
    sp->n     = n;
    sp->front = sp->rear = 0;
    Sem_init(&sp->mutex, 0, 1);
    Sem_init(&sp->slots, 0, n);
    Sem_init(&sp->items, 0, 0);
}

void sbuf_insert(sbuf_t *sp, int item) {
    P(&sp->slots);
    P(&sp->mutex);
    sp->buf[sp->rear] = item;
    sp->rear = (sp->rear + 1) % sp->n;
    V(&sp->mutex);
    V(&sp->items);
}

int sbuf_remove(sbuf_t *sp) {
    int item;
    P(&sp->items);
    P(&sp->mutex);
    item = sp->buf[sp->front];
    sp->front = (sp->front + 1) % sp->n;
    V(&sp->mutex);
    V(&sp->slots);
    return item;
}

item_t* insert_item(item_t *n, item_t *it) {
    if (!n) return it;
    if (it->ID < n->ID) n->left  = insert_item(n->left,  it);
    else                n->right = insert_item(n->right, it);
    return n;
}

item_t* find_item(item_t *n, int ID) {
    if (!n) return NULL;
    if (n->ID == ID) return n;
    return ID < n->ID ? find_item(n->left, ID)
                      : find_item(n->right, ID);
}

void free_tree(item_t *n) {
    if (!n) return;
    free_tree(n->left);
    free_tree(n->right);
    Free(n);
}

item_t* load_stock(const char *fn) {
    FILE *fp = Fopen(fn, "r");
    item_t *t = NULL;
    int id, l, p;
    while (fscanf(fp, "%d %d %d", &id, &l, &p) == 3) {
        item_t *it = Malloc(sizeof(*it));
        it->ID = id; it->left_stock = l; it->price = p;
        it->left = it->right = NULL;
        t = insert_item(t, it);
    }
    Fclose(fp);
    return t;
}

void save_stock(item_t *n, FILE *fp) {
    if (!n) return;
    save_stock(n->left,  fp);
    fprintf(fp, "%d %d %d\n", n->ID, n->left_stock, n->price);
    save_stock(n->right, fp);
}

void persist_stock() {
    FILE *fp = fopen(STOCK_FILE, "w");
    if (!fp) return;
    save_stock(root, fp);
    fclose(fp);
}

void sigint_handler(int sig) {
    persist_stock();
    free_tree(root);
    Close(listenfd);
    exit(0);
}

void append_show(item_t *n, char *out, int *off) {
    if (!n) return;
    append_show(n->left,  out, off);
    int w = snprintf(out + *off, MAXLINE - *off,
                     "%d %d %d\n", n->ID, n->left_stock, n->price);
    *off += w;
    append_show(n->right, out, off);
}

void send_response(int connfd, const char *cmd,
                   const char *status, int do_show) {
    char out[MAXLINE];
    int off = 0;
    int clen = strlen(cmd);
    if (clen >= MAXLINE) clen = MAXLINE - 1;
    memcpy(out + off, cmd, clen); off += clen;
    off += snprintf(out + off, MAXLINE - off, "%s", status);
    if (do_show) append_show(root, out, &off);
    Rio_writen(connfd, out, MAXLINE);
}

void *worker(void *v) {
    Pthread_detach(Pthread_self());
    rio_t rio;
    while (1) {
        int connfd = sbuf_remove(&sbuf);
        Rio_readinitb(&rio, connfd);
        char buf[MAXLINE];
        ssize_t n;
        while ((n = Rio_readlineb(&rio, buf, MAXLINE)) > 0) {
            printf("server received %zd bytes\n", n);
            fflush(stdout);
            char cmd[16]; int id, num;
            int args = sscanf(buf, "%15s %d %d", cmd, &id, &num);
            if (strcmp(cd, "show") == 0) {
                send_response(connfd, buf, "", 1);
            } else if (strcmmp(cmd, "buy") == 0 && args == 3) {
                item_t *it = find_item(root, id);
                if (it && it->left_stock >= num) {
                    it->left_stock -= num;
                    persist_stock();
                    send_response(connfd, buf, "[buy] success\n", 0);
                } else {
                    send_response(connfd, buf, "Not enough left stock\n", 0);
                }
            } else if (strcmp(cmd, "sell") == 0 && args == 3) {
                item_t *it = find_item(root, id);
                if (it) { it->left_stock += num; persist_stock(); }
                send_response(connfd, buf, "[sell] success\n", 0);
            } else if (strcmp(cmd, "exit") == 0) {
                persist_stock();
                Close(connfd);
                break;
            }
        }
        if (n <= 0) Close(connfd);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }
    root     = load_stock(STOCK_FILE);
    Signal(SIGINT, sigint_handler);
    listenfd = Open_listenfd(argv[1]);
    sbuf_init(&sbuf, SBUFSIZE);
    for (int i = 0; i < NTHREADS; i++) {
        pthread_t tid;
        Pthread_create(&tid, NULL, worker, NULL);
    }
    while (1) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        int connfd = Accept(listenfd, (SA *)&addr, &len);
        char h[MAXLINE], p[MAXLINE];
        Getnameinfo((SA *)&addr, len, h, MAXLINE, p, MAXLINE, 0);
        printf("Connected to (%s, %s)\n", h, p);
        fflush(stdout);
        sbuf_insert(&sbuf, connfd);
    }
    return 0;
}

