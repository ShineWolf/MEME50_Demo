    #include <sys/types.h>
    #include <sys/socket.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <netinet/in.h>
    #include <sys/time.h>
    #include <sys/ioctl.h>
    #include <unistd.h>
    #include <string.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <mariadb/mysql.h>
    #include <pthread.h>
    #include <time.h>
    #include <signal.h>
    #include <stdatomic.h>

    #define DEVICE_NORMAL_NAME "/dev/ads1115"
    #define DEVICE_ALERT_NAME "/dev/ads1115-alert"

    static MYSQL *conn;
    static char mysql_ip[] = "Database_IP";
    static char mysql_username[] = "Database_Username";
    static char mysql_password[] = "Database_Password";
    static char mysql_dbname[] = "Database_Name";

    static long sum_val = 0;
    static int sample_count = 0;
    pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t mysql_lock = PTHREAD_MUTEX_INITIALIZER;
    static int normal_fd = -1;
    static int connect_status = 0;
    static int alert_read_fd = -1, alert_write_fd = -1;
    static int server_sockfd = -1;
    static pthread_t normal_thread;
    static volatile sig_atomic_t stop_flag = 0;

    void handle_sigint(int sig) {
        stop_flag = 1;
    }

    int open_connect(){        
        conn = mysql_init(NULL);
        if (!conn) {
            fprintf(stderr, "MySQL init error\n");
            return -1;
        }
        // Connect to MariaDB server on remote host 
        if (!mysql_real_connect(conn, mysql_ip, mysql_username, mysql_password, mysql_dbname, 0, NULL, 0)) {
            fprintf(stderr, "MySQL connection error: %s\n", mysql_error(conn));
            mysql_close(conn);
            return -1;
        }
        connect_status = 1;
        return 0;
    }

    int close_connect(){
        mysql_close(conn);
        return 0;
    }

    int insert_record(const char *device_id, const char *value, const char *status) {
        char query[512];
        pthread_mutex_lock(&mysql_lock);

        if (mysql_ping(conn)) {
            fprintf(stderr, "MySQL ping failed: %s\n", mysql_error(conn));
            mysql_close(conn);
            connect_status = 0;
            if (open_connect() != 0) {
                fprintf(stderr, "Reconnection failed\n");
                pthread_mutex_unlock(&mysql_lock);
                return -1;
            }
        }

        snprintf(query, sizeof(query),
            "INSERT INTO sensor_data (device_id, value, status) VALUES ('%s', '%s', '%s')",
                device_id, value, status);
    
        if (mysql_query(conn, query)) {
            fprintf(stderr, "Insert error: %s\n", mysql_error(conn));
        } else {
            printf("Inserted: %s, %s, %s\n", device_id, value, status);
        }
        pthread_mutex_unlock(&mysql_lock);

        return 0;
    }
    
    void *normal_thread_fn(void *arg) {
        int fd = *(int *)arg;
        free(arg);
        char buf[16];
    
        time_t last_time = time(NULL);
        printf("thread is on! fd = %d\n", fd);
    
        while (atomic_load(&stop_flag) == 0) {
            int nread;
            
            memset(buf, 0, sizeof(buf));
            int len = read(fd, buf, sizeof(buf) - 1);
            if (len > 0) {
                buf[len] = '\0';
                char *newline = strtok(buf, "\r\n\t ");
                long val = strtol(buf, NULL, 10);
    
                pthread_mutex_lock(&data_lock);
                sum_val += val;
                sample_count++;
                pthread_mutex_unlock(&data_lock);
            }
    
            time_t now = time(NULL);
            if (now - last_time >= 60) {
                last_time = now;
    
                pthread_mutex_lock(&data_lock);
                long sum = sum_val;
                int count = sample_count;
                sum_val = 0;
                sample_count = 0;
                pthread_mutex_unlock(&data_lock);
    
                if (count > 0) {
                    long avg = sum / count;
                    char avg_str[32];
                    snprintf(avg_str, sizeof(avg_str), "%ld", avg);
                    if (connect_status)
                        insert_record("sensor_noise_001", avg_str, "normal");
                    else
                        printf("Connect error in sensor normal!\n");
                } else {
                    printf("No data collected in last minute.\n");
                }
            }
    
            usleep(50000); // 小延遲避免過度佔用 CPU
        }

        return NULL;
    }

    void cleanup() {
        printf("\n[INFO] Cleaning up resources...\n");
    
        if (normal_fd >= 0) close(normal_fd);
        if (alert_read_fd >= 0) close(alert_read_fd);
        if (alert_write_fd >= 0) close(alert_write_fd);
        if (server_sockfd >= 0) close(server_sockfd);
    
        pthread_cancel(normal_thread);
        pthread_join(normal_thread, NULL);
    
        close_connect();
        mysql_library_end();
    
        printf("[INFO] Server shutdown complete.\n");
        mysql_thread_end();
    }

    int main(void){
        int client_sockfd;
        int server_len, client_len;
        struct sockaddr_in server_address;
        struct sockaddr_in client_address;
        int result;
        fd_set readfds, testfds;
        
        char string[10];
        int fd, max_fd = 0;
        int nread;
    
        int res, read_mode = O_RDONLY  | O_NONBLOCK, write_mode = O_WRONLY | O_NONBLOCK;

        /* Signal Handling */
        signal(SIGINT, handle_sigint);

        /*  Create and name a socket for the server.  */
        server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    
        server_address.sin_family = AF_INET;
        server_address.sin_addr.s_addr = htonl(INADDR_ANY);
        server_address.sin_port = htons(5077);
        server_len = sizeof(server_address);
    
        bind(server_sockfd, (struct sockaddr *)&server_address, server_len);

        if (open_connect() != 0) {
            fprintf(stderr, "Failed to connect to MariaDB\n");
            exit(1);
        }

    /*  Create a connection queue and initialize readfds to handle input from server_sockfd.  */
        listen(server_sockfd, 5);
    
        FD_ZERO(&readfds);
        FD_SET(server_sockfd, &readfds);
        if (server_sockfd > max_fd) max_fd = server_sockfd;

        normal_fd = open(DEVICE_NORMAL_NAME, read_mode);
        printf("normal fd = %d\n", normal_fd);

        /* Create a thread for calculating value from /dev/normal */
        int *arg = malloc(sizeof(int));
        *arg = normal_fd;
        if (pthread_create(&normal_thread, NULL, normal_thread_fn, arg) != 0) {
            perror("Failed to create normal_fd thread");
            exit(1);
        }

        /* Open the /dev/alert and clear it.*/
        alert_write_fd = open(DEVICE_ALERT_NAME, write_mode);
        write(alert_write_fd, "clear\n", 6);

        alert_read_fd = open(DEVICE_ALERT_NAME, read_mode);
        FD_SET(alert_read_fd, &readfds);
        if (alert_read_fd > max_fd) max_fd = alert_read_fd;

    
    /*  Now wait for clients and requests.
        Since we have passed a null pointer as the timeout parameter, no timeout will occur.
        The program will exit and report an error if select returns a value of less than 1.  */
    
        while(!stop_flag) {
            testfds = readfds;

            result = select(FD_SETSIZE, &testfds, (fd_set *)0, (fd_set *)0, (struct timeval *) 0);
    
            if(result < 1) {
                perror("server");
                exit(1);
            }
    
    /*  Once we know we've got activity,
        we find which descriptor it's on by checking each in turn using FD_ISSET.  */
            for(fd = 0; fd <= max_fd; fd++) {
                if(FD_ISSET(fd, &testfds)) {
                    if(fd == server_sockfd) {
                        client_len = sizeof(client_address);
                        client_sockfd = accept(server_sockfd, (struct sockaddr *)&client_address, &client_len);
                        FD_SET(client_sockfd, &readfds);
                        if (client_sockfd > max_fd) max_fd = client_sockfd;
                        printf("adding client on fd %d\n", client_sockfd);
                    }
                    else if(fd == alert_read_fd){
                        printf("alert got noise\n");
                        ioctl(fd, FIONREAD, &nread);
                        if(nread == 0){
                            write(alert_write_fd, "clear\n", 6);
                        }
                        else{
                            memset(string, '\0', sizeof(string));
                            int len = read(fd, string, sizeof(string)-1);
                            char *newline = strtok(string, "\r\n\t ");
                            if (len > 0)
                                string[len] = '\0';
                            printf("pico got alert message: %s\n", string);
                            if (len > 1){
                                string[sizeof(string) - 1] = '\0';
                            }
                            for(int tmpfd = 0; tmpfd < FD_SETSIZE; tmpfd++){
                                if(tmpfd == server_sockfd || tmpfd == fd || tmpfd == 0 || tmpfd == 1 || tmpfd == 2 || tmpfd == normal_fd || tmpfd == alert_read_fd)
                                    continue;
                                write(tmpfd, string, strlen(string));
                            }
                            insert_record("sensor_noise_001", string, "ALERT");
                            sleep(5);
                            write(alert_write_fd, "clear\n", 6);
                        }
                    }
                    else {
                        ioctl(fd, FIONREAD, &nread);
                        if(nread == 0) {
                            close(fd);
                            FD_CLR(fd, &readfds);
                            if (fd == max_fd) {
                                max_fd = 0;
                                for (int i = 0; i < FD_SETSIZE; i++) {
                                    if (FD_ISSET(i, &readfds) && i > max_fd)
                                        max_fd = i;
                                }
                            }
                        }
                    }
                }
            }
        }
        cleanup();
        return 0;
    }