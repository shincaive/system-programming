#include "csapp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#define STOCK_FILE  "stock.txt"
#define MAXCLIENT   100


typedef struct item {
    int ID, left_stock, price;
    struct item *left, *right;
} item_t;

static item_t *root = NULL;
static int listenfd;


item_t* insert_item(item_t *n, item_t *it) {
    if (!n) return it;
    if (it->ID < n->ID) n->left  = insert_item(n->left,  it);
    else                n->right = insert_item(n->right, it);
    return n;
}

item_t* find_item(item_t *n, int ID) {
    if (!n) return NULL;
    if (n->ID == ID) return n;
    return (ID < n->ID) ? find_item(n->left, ID)
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
    append_show(n->left, out, off);
    int w = snprintf(out + *off, MAXLINE - *off,
                     "%d %d %d\n", n->ID, n->left_stock, n->price);
    *off += w;
    append_show(n->right, out, off);
}


void send_response(int connfd, const char *cmd_line,
                   const char *status, int do_show) {
    char out[MAXLINE];
    memset(out, 0, MAXLINE);
    int off = 0;
    
    int clen = strlen(cmd_line);
    if (clen >= MAXLINE) clen = MAXLINE - 1;
    memcpy(out + off, cmd_line, clen);
    off += clen;
    
    int sl = snprintf(out + off, MAXLINE - off, "%s", status);
    off += sl;
    
    if (do_show) append_show(root, out, &off);
    
    Rio_writen(connfd, out, MAXLINE);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }
    root = load_stock(STOCK_FILE);
    Signal(SIGINT, sigint_handler);
    listenfd = Open_listenfd(argv[1]);

    fd_set allset, rset;
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    int maxfd = listenfd;

    int client[MAXCLIENT];
    rio_t rio_arr[MAXCLIENT];
    for (int i = 0; i < MAXCLIENT; i++) client[i] = -1;

    while (1) {
        rset = allset;
        Select(maxfd+1, &rset, NULL, NULL, NULL);
        // new connection
        if (FD_ISSET(listenfd, &rset)) {
            struct sockaddr_storage addr;
            socklen_t len = sizeof(addr);
            int fd = Accept(listenfd, (SA*)&addr, &len);
            char h[MAXLINE], p[MAXLINE];
            Getnameinfo((SA*)&addr, len, h, MAXLINE, p, MAXLINE, 0);
            printf("Connected to (%s, %s)\n", h, p);
            fflush(stdout);
            for (int i = 0; i < MAXCLIENT; i++) {
                if (client[i] < 0) {
                    client[i] = fd;
                    Rio_readinitb(&rio_arr[i], fd);
                    FD_SET(fd, &allset);
                    if (fd > maxfd) maxfd = fd;
                    break;
                }
            }
        }
        // handle clients
        for (int i = 0; i < MAXCLIENT; i++) {
            int fd = client[i];
            if (fd < 0) continue;
            if (FD_ISSET(fd, &rset)) {
                char buf[MAXLINE];
                ssize_t n = Rio_readlineb(&rio_arr[i], buf, MAXLINE);
                if (n <= 0) {
                    Close(fd);
                    FD_CLR(fd, &allset);
                    client[i] = -1;
                } else {
                    printf("server received %zd bytes\n", n);
                    fflush(stdout);
                    char cmd[16]; int id, num;
                    int args = sscanf(buf, "%15s %d %d", cmd, &id, &num);
                    if (strcmp(cmd, "show") == 0) {
                        send_response(fd, buf, "", 1);
                    }
                    else if (strcmp(cmd, "buy") == 0 && args == 3) {
                        item_t *it = find_item(root, id);
                        if (it && it->left_stock >= num) {
                            it->left_stock -= num;
                            persist_stock();
                            send_response(fd, buf, "[buy] success\n", 0);
                        } else {
                            send_response(fd, buf, "Not enough left stock\n", 0);
                        }
                    }
                    else if (strcmp(cmd, "sell") == 0 && args == 3) {
                        item_t *it = find_item(root, id);
                        if (it) {
                            it->left_stock += num;
                            persist_stock();
                        }
                        send_response(fd, buf, "[sell] success\n", 0);
                    }
                    else if (strcmp(cmd, "exit") == 0) {
                        persist_stock();
                        exit(0);
                    }
                }
            }
        }
    }
    return 0;
}

