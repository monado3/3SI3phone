#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>

#define N 1024

void convert(long n, long min, long max, void *buf);

int ss, s, c, pid;
FILE *fp_play, *fp_rec;

void abort_handler(int signal) {
    if (fp_play != NULL) {
        fclose(fp_play);
    }
    if (fp_rec != NULL) {
        fclose(fp_rec);
    }
    close(c);
    close(ss);
    close(s);
    kill(pid, SIGKILL);
    exit(0);
}

// ./iphone -s [source_port] -t [target_ip:port]
int main(int argc, char **argv) {

    if (signal(SIGINT, abort_handler) == SIG_ERR) {
        exit(1);
    }

    // parse options
    int opt;
    int server_port, client_port;
    char *client_ip, *client_port_str;
    while ((opt = getopt(argc, argv, "s:t:")) != -1) {
        switch (opt) {
            case 's':
                server_port = atoi(optarg);
                if (server_port == 0) {
                    fprintf(stderr, "source port %s is invalid.", optarg);
                    exit(1);
                }
                break;
            case 't':
                client_ip = strtok(optarg, ":");
                client_port_str = strtok(NULL, ":");
                client_port = atoi(client_port_str);
                if (client_port == 0) {
                    fprintf(stderr, "target port %s is invalid.", client_port_str);
                    exit(1);
                }
                break;
            default:
                printf("Usage: %s [-s source_port] [-c target_ip:port]\n", argv[0]);
                exit(1);
        }
    }

    if (server_port == 0 || client_port == 0 || sizeof(client_ip) == 0) {
        fprintf(stderr, "input argument is wrong.");
        exit(1);
    }

    int status;

    if ((pid = fork()) == 0) {

        printf("waiting for connection");
        fflush(stdout);
        // client settings
        while (1) {
            c = socket(AF_INET, SOCK_STREAM, 0);
            if (c == -1) {
                perror("timeout");
                exit(1);
            }

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            inet_aton(client_ip, &addr.sin_addr);
            addr.sin_port = htons(client_port);
            printf(".");
            fflush(stdout);
            int ret = connect(c, (struct sockaddr *) &addr, sizeof(addr));
            if (ret == 0) break;
            if (ret == -1) {
                close(c);
                sleep(1);
            }
        }
        printf("\nconnection has been established!\n");
        fflush(stdout);

        // start play command
        char data_play[N];
        fp_play = popen("play -q -t raw -b 16 -c 2 -e s -r 44100 - 2>/dev/null", "w");
        if (fp_play == NULL) {
            perror("command");
            exit(1);
        }

        while (1) {
            // play
            int n = read(c, data_play, N);
            if (n == -1) {
                perror("read");
                exit(1);
            }
            if (n == 0) break;
            fwrite(data_play, sizeof(data_play), 1, fp_play);
        }
        fclose(fp_play);
        close(c);
    } else {
        if (pid == -1) {
            fprintf(stderr, "parent process failed");
            return (1);
        } else {
            // create socket
            ss = socket(AF_INET, SOCK_STREAM, 0);
            if (ss == -1) {
                perror("timeout");
                exit(1);
            }

            if (setsockopt(ss, SOL_SOCKET, SO_REUSEADDR, &(int) {1}, sizeof(int)) < 0) {
                fprintf(stderr, "setsockopt(SO_REUSEADDR) failed");
                exit(1);
            }

            // server settings
            struct sockaddr_in server_addr;
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(server_port);
            server_addr.sin_addr.s_addr = INADDR_ANY;

            bind(ss, (struct sockaddr *) &server_addr, sizeof(server_addr));

            printf("server is now ready!\n");

            listen(ss, 10);
            struct sockaddr_in client_addr;
            socklen_t len = sizeof(struct sockaddr_in);
            s = accept(ss, (struct sockaddr *) &client_addr, &len);
            if (s == -1) {
                perror("timeout");
                exit(1);
            }

            close(ss);

            // start rec command
            char data_rec[N];
            fp_rec = popen("rec -V1 -t raw -b 16 -c 2 -e s -r 44100 -", "r");
            if (fp_rec == NULL) {
                perror("command");
                exit(1);
            }

            while (1) {
                // rec
                size_t size = fread(data_rec, N, 1, fp_rec);
                if (size == 0) break;
                if (feof(fp_rec)) {
                    perror("read");
                    exit(1);
                }
                convert(N, 300, 3400, data_rec);
                write(s, data_rec, sizeof(data_rec));
            }
            fclose(fp_rec);
            close(s);
            kill(pid, SIGKILL);
        }
    }
}

typedef short sample_t;

void die(char *s) {
    perror(s);
    exit(1);
}

/* 標本(整数)を複素数へ変換 */
void sample_to_complex(sample_t *s,
                       complex double *X,
                       long n) {
    long i;
    for (i = 0; i < n; i++) X[i] = s[i];
}

/* 複素数を標本(整数)へ変換. 虚数部分は無視 */
void complex_to_sample(complex double *X,
                       sample_t *s,
                       long n) {
    long i;
    for (i = 0; i < n; i++) {
        s[i] = creal(X[i]);
    }
}

/* 高速(逆)フーリエ変換;
   w は1のn乗根.
   フーリエ変換の場合   偏角 -2 pi / n
   逆フーリエ変換の場合 偏角  2 pi / n
   xが入力でyが出力.
   xも破壊される
 */
void fft_r(complex double *x,
           complex double *y,
           long n,
           complex double w) {
    if (n == 1) { y[0] = x[0]; }
    else {
        complex double W = 1.0;
        long i;
        for (i = 0; i < n / 2; i++) {
            y[i] = (x[i] + x[i + n / 2]); /* 偶数行 */
            y[i + n / 2] = W * (x[i] - x[i + n / 2]); /* 奇数行 */
            W *= w;
        }
        fft_r(y, x, n / 2, w * w);
        fft_r(y + n / 2, x + n / 2, n / 2, w * w);
        for (i = 0; i < n / 2; i++) {
            y[2 * i] = x[i];
            y[2 * i + 1] = x[i + n / 2];
        }
    }
}

void fft(complex double *x,
         complex double *y,
         long n) {
    long i;
    double arg = 2.0 * M_PI / n;
    complex double w = cos(arg) - 1.0j * sin(arg);
    fft_r(x, y, n, w);
    for (i = 0; i < n; i++) y[i] /= n;
}

void ifft(complex double *y,
          complex double *x,
          long n) {
    double arg = 2.0 * M_PI / n;
    complex double w = cos(arg) + 1.0j * sin(arg);
    fft_r(y, x, n, w);
}

void bandpass(long min, long max, complex double *Y, long n) {
    for (long i = 0; i < min; i++) {
        Y[i] = 0;
    }
    for (long i = max; i < n; i++) {
        Y[i] = 0;
    }
}

void convert(long n, long min, long max, void *buf) {
    complex double *X = calloc(sizeof(complex double), n);
    complex double *Y = calloc(sizeof(complex double), n);
    sample_to_complex(buf, X, n);
    fft(X, Y, n);
    bandpass(min, max, Y, n);
    ifft(Y, X, n);
    complex_to_sample(X, buf, n);
}
