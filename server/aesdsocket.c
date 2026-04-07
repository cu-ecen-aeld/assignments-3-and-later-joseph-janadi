#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netdb.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
//#include <sys/queue.h>
#include "queue.h"
#include <pthread.h>
#include <sys/ioctl.h>
#include "../aesd_ioctl.h"

/* Linked list */
struct conn {
    pthread_t thread_id;
    int completed;
    SLIST_ENTRY(conn) conns;
};
SLIST_HEAD(slisthead, conn);
struct slisthead head;

/* Phtread argument struct */
struct conn_info {
    int fd;
    struct sockaddr_in accepted_sockaddr;
};

/* Mutex */
pthread_mutex_t datafd_mutex;

/* Prototypes */
void *send_receive(void *arg);
long seek(int datafd, char *packet_buf);
int set_exit_handler(void);
int set_time_handler(void);
int create_daemon(void);
int bind_socket(char *port_str);
int resize_buf(char **buf, size_t *buf_len, size_t add_len);

/* Globals */
int server_fd = -1;
#ifdef USE_AESD_CHAR_DEVICE
const char *data_file = "/dev/aesdchar";
#else
const char *data_file = "/var/tmp/aesdsocketdata";
#endif
int datafd;
const size_t BUF_SIZE = 500;
size_t packet_buf_len = 500;
int ret;

int main(int argc, char *argv[])
{
    /* Set signal handler for SIGINT and SIGTERM */
    ret = set_exit_handler();
    if (ret == -1) { return ret; }

    /* Create socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) { perror("socket"); return -1; }

    /* Bind socket to port */
    char *port_str = "9000";
    ret = bind_socket(port_str);
    if (ret == -1) { return -1; }

    /* If '-d' option passed, run as daemon */
    int opt = getopt(argc, argv, "d");
    if (opt == 'd') {
        ret = create_daemon();
        if (ret == -1) { return -1; }
    }

    /* Set socket to listen for connections */
    int backlog = 10;
    ret = listen(server_fd, backlog);
    if (ret == -1) { perror("listen"); return -1; }

    /* Open or create data file */
    datafd = open(data_file, O_RDWR | O_APPEND | O_CREAT, 0755);
    if (datafd == -1) { perror("open"); return -1; }

    /* Initialize mutex */
    pthread_mutex_init(&datafd_mutex, NULL);

    /* Set time handler */
    //ret = set_time_handler();
    //if (ret == -1) { return ret; }

    /* Initialize linked list */
    SLIST_INIT(&head);
    struct conn *new_np;
    struct conn *np;
    struct conn *next_np;

    struct sockaddr_in accepted_sockaddr;
    socklen_t addrlen = sizeof(accepted_sockaddr);

    int client_fd;

    pthread_t new_thread_id;

    /* Accept connections until SIGINT or SIGTERM */
    while (1) {
        /* Accept next connection */
        client_fd = accept(server_fd, (struct sockaddr *)&accepted_sockaddr, &addrlen);
        if (client_fd == -1) { perror("accept"); return -1; }
        syslog(LOG_DEBUG, "Accepted connection from %u", accepted_sockaddr.sin_addr.s_addr);

        /* Create new thread */
        struct conn_info *pthread_arg = (struct conn_info *)malloc(sizeof(struct conn_info));
        pthread_arg->fd = client_fd;
        pthread_arg->accepted_sockaddr = accepted_sockaddr;
        ret = pthread_create(&new_thread_id, NULL, send_receive, pthread_arg);
        if (ret != 0) {
            fprintf(stderr, "Failed to create new thread: %s\n", strerror(ret));
            exit(EXIT_FAILURE);
        }

        /* Add to linked list */
        new_np = (struct conn *)malloc(sizeof(struct conn));
        new_np->thread_id = new_thread_id;
        new_np->completed = 0;
        SLIST_INSERT_HEAD(&head, new_np, conns);

        /* Iterate over linked list */
        SLIST_FOREACH_SAFE(np, &head, conns, next_np) {
            /* Join if complete */
            if (np->completed == 1) {
                ret = pthread_join(np->thread_id, NULL);
                if (ret != 0) {
                    fprintf(stderr, "Failed to join thread: %s\n", strerror(ret));
                    exit(EXIT_FAILURE);
                }
                //np->completed = 2;
                SLIST_REMOVE(&head, np, conn, conns);
                free(np);
            }
        }
    }
}

void *send_receive(void *arg)
{
    struct conn_info *info = (struct conn_info *)arg;
    int fd = info->fd;
    struct sockaddr_in accepted_sockaddr = info->accepted_sockaddr;
    free(arg);

    struct conn *np;
    char *packet_buf = (char *)malloc(packet_buf_len);
    size_t packet_len = 0;
    ssize_t nread;
    char buf[BUF_SIZE];
    char *newline_ptr;
    ptrdiff_t newline_pos;
    size_t str_len;
    ssize_t nwritten;
    ssize_t nsent;
    off_t offset = 0;
    int ret;

    /* Read from socket until closed or no more data */
    while (1) {
        /* Peek at stream contents */
        nread = recv(fd, buf, BUF_SIZE, MSG_PEEK);
        if (nread == -1) { perror("recv"); break; }
        /* If socket closed or no more data, break */
        else if (nread == 0) { break; }
        /* Find newline character */
        newline_ptr = memchr(buf, '\n', nread);
        newline_pos = newline_ptr == NULL ? nread - 1 : newline_ptr - buf;
        str_len = newline_pos + 1;
        /* Increase packet buffer size if exceeded */
        if ((packet_len + str_len) > packet_buf_len) {
            ret = resize_buf(&packet_buf, &packet_buf_len, str_len);
            if (ret == -1) { break; }
        }
        /* Append str_len from stream to packet buffer */
        nread = recv(fd, packet_buf + packet_len, str_len, 0);
        if (nread == -1) { perror("recv"); break; }
        packet_len += str_len;
        /* If packet not complete, loop back to receive again */
        if (newline_ptr == NULL)
            continue;

        /* Packet complete */
        ret = pthread_mutex_lock(&datafd_mutex);      // Lock datafd
        if (ret != 0) { 
            fprintf(stderr, "Error locking mutex: %s\n", strerror(ret));
            exit(EXIT_FAILURE);
        }
        // If seekto command, call ioctl
        if (strncmp(packet_buf, "AESDCHAR_IOCSEEKTO", 18) == 0) {
            if (seek(datafd, packet_buf) < 0) {
                fprintf(stderr, "Error seeking in data file\n");
                exit(EXIT_FAILURE);
            }
        }
        // If not seekto command, write packet to data file
        else {
            nwritten = write(datafd, packet_buf, packet_len);
            if (nwritten == -1) { perror("write"); break; }
        }

        /* Return contents of data file to client */
        ret = close(datafd);
        if (ret == -1) { perror("write"); break; }
        datafd = open(data_file, O_RDWR | O_APPEND | O_CREAT, 0755);
        do {
            nread = read(datafd, buf, BUF_SIZE);
            if (nread == -1) { perror("read"); break; }
            nsent = send(fd, buf, nread, 0);
            if (nsent == -1) { perror("send"); break; }
            offset += nread;
        } while (nsent > 0);
        if (nsent == -1) { perror("sendfile"); break; }
        ret = pthread_mutex_unlock(&datafd_mutex);    // Unlock datafd
        if (ret != 0) { 
            fprintf(stderr, "Error unlocking mutex: %s\n", strerror(ret));
            exit(EXIT_FAILURE);
        }

        /* Reset packet_len */
        packet_len = 0;
    }
    free(packet_buf);

    /* Close connection */
    ret = close(fd);
    if (ret == -1 ) { perror("close"); }
    syslog(LOG_DEBUG, "Closed connection from %u", accepted_sockaddr.sin_addr.s_addr);

    /* Set complete flag */
    SLIST_FOREACH(np, &head, conns) {
        if (np->thread_id == pthread_self()) {
            np->completed = 1;
            break;
        }
    }

    pthread_exit(NULL);
}

long seek(int datafd, char *packet_buf)
{
    struct aesd_seekto seekto;
    seekto.write_cmd = packet_buf[19] - '0';
    seekto.write_cmd_offset = packet_buf[21] - '0';
    int retval = ioctl(datafd, AESDCHAR_IOCSEEKTO, &seekto);

    return retval;
}

void exit_handler(int sig)
{
    /* Wait for any open connections to close */
    struct conn *np;
    SLIST_FOREACH(np, &head, conns) {
        if (np->completed == 0 || np->completed == 1) {
            ret = pthread_join(np->thread_id, NULL);
            if (ret != 0)
                fprintf(stderr, "Failed to join thread: %s\n", strerror(ret));
        }
    }

    /* Close any open sockets */
    close(server_fd);

    /* Delete data file */
    if (strcmp(data_file, "/dev/aesdchar") != 0) {
        ret = remove(data_file);
        if (ret == -1) { perror("remove"); exit(EXIT_FAILURE); }
        syslog(LOG_DEBUG, "Caught signal, exiting");
    }

    exit(EXIT_SUCCESS);
}

int set_exit_handler(void)
{
    struct sigaction act;
    act.sa_handler = exit_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    ret = sigaction(SIGINT, &act, NULL);
    if (ret == -1) { perror("Error"); return -1; }
    ret = sigaction(SIGTERM, &act, NULL);
    if (ret == -1) { perror("Error"); return -1; }

    return 0;
}

void time_handler(union sigval arg)
{
    int ret;

    /* Get system wall clock time */
    struct timespec tp;
    ret = clock_gettime(CLOCK_REALTIME, &tp);
    if (ret == -1) { perror("Failed to get current time"); exit(EXIT_FAILURE); }

    struct tm *t = localtime(&tp.tv_sec);
    
    char cur_time[64];
    size_t len = strftime(cur_time, sizeof(cur_time), "%a, %d %b %Y %T %z\n", t);
    if (len == 0) {
        perror("Failed to get current time");
        exit(EXIT_FAILURE);
    }
    char timestamp[128] = "timestamp:";
    strncat(timestamp, cur_time, len);

    /* Write time to data file */
    ret = pthread_mutex_lock(&datafd_mutex);      // Lock datafd
    if (ret != 0) { 
        fprintf(stderr, "Error locking mutex: %s\n", strerror(ret));
        exit(EXIT_FAILURE);
    }
    ssize_t nwritten = write(datafd, &timestamp, strlen(timestamp));
    if (nwritten == -1) { perror("write"); exit(EXIT_FAILURE); }
    ret = pthread_mutex_unlock(&datafd_mutex);    // Unlock datafd
    if (ret != 0) { 
        fprintf(stderr, "Error unlocking mutex: %s\n", strerror(ret));
        exit(EXIT_FAILURE);
    }
}

int set_time_handler(void)
{
    struct sigevent sev;
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = time_handler;
    sev.sigev_notify_attributes = NULL;
    sev.sigev_value.sival_ptr = NULL;

    timer_t timerid;

    if (timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
        perror("Failed to create timer");
        exit(EXIT_FAILURE);
    }

    /* Set timer to expire every 10 s */
    struct itimerspec itimer = {
        .it_value = {10, 0},
        .it_interval = {10, 0},
    };

    if (timer_settime(timerid, 0, &itimer, NULL) == -1) {
        perror("Failed to set timer");
        exit(EXIT_FAILURE);
    }

    return 0;
}

int create_daemon(void)
{
    pid_t pid = fork();                             // fork
    if (pid == -1) { perror("fork"); return -1; }
    if (pid > 0) { exit(EXIT_SUCCESS); }            // exit parent

    pid_t sid = setsid();                           // create new session
    if (sid == -1) { perror("setsid"); return -1; }

    pid = fork();                                   // fork again
    if (pid == -1) { perror("fork"); return -1; }
    if (pid > 0) { exit(EXIT_SUCCESS); }            // exit parent

    ret = chdir("/");                               // chdir
    if (ret == -1) { perror("chdir"); return -1; }

    umask(0);                                       // Reset file permissions

    /* Redirect std fd's */
    int fd = open("/dev/null", O_RDWR);
    if (fd == -1) { perror("open /dev/null"); return -1; }
    int new_fd;
    new_fd = dup2(fd, 0);
    if (new_fd == -1) { perror("dup2"); return -1; }
    new_fd = dup2(fd, 1);
    if (new_fd == -1) { perror("dup2"); return -1; }
    new_fd = dup2(fd, 2);
    if (new_fd == -1) { perror("dup2"); return -1; }
    if (fd > 2)
        close(fd);

    return 0;
}

int bind_socket(char *port_str)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res;
    ret = getaddrinfo(NULL, port_str, &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }
    ret = bind(server_fd, res->ai_addr, res->ai_addrlen);
    if (ret == -1) { perror("bind"); return -1; }
    freeaddrinfo(res);

    return 0;
}

int resize_buf(char **buf, size_t *buf_len, size_t add_len)
{
    size_t new_len = *buf_len + add_len;
    char *tmp = realloc(*buf, new_len);
    if (tmp == NULL) {
        fprintf(stderr, "realloc failed\n");
        return -1;
    }
    *buf = tmp;
    *buf_len = new_len;

    return 0;
}
