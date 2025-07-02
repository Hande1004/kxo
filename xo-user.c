#include <fcntl.h>
#include <getopt.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "list.h"
#include "user_game.h"
#include "user_mcts.h"
#include "user_negamax.h"

// #include "game.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

struct task {
    jmp_buf env;
    struct list_head list;
};

struct user_attr {
    int display;
    int end;
};

static struct user_attr attr_obj;
static char board[N_GRIDS];
static LIST_HEAD(tasklist);
static jmp_buf sched;
static void (**tasks)(void);
static int ntasks;
static char display_buf[DRAWBUFFER_SIZE];
static struct task *cur_task;

struct AI_state {
    uint64_t total_cpu_ns_X;
    uint64_t total_cpu_ns_O;
    uint64_t total_wall_ns_X;
    uint64_t total_wall_ns_O;
};
struct AI_state ai_state = {0};

static void task_add(struct task *task)
{
    list_add_tail(&task->list, &tasklist);
}

static void task_switch()
{
    if (!list_empty(&tasklist)) {
        struct task *t = list_first_entry(&tasklist, struct task, list);
        list_del(&t->list);
        cur_task = t;
        longjmp(t->env, 1);
    }
}
void schedule(void)
{
    static int i;

    setjmp(sched);

    while (ntasks-- > 0) {
        tasks[i++]();
        printf("Never reached\n");
    }

    task_switch();
}

void task_O(void)
{
    struct task *task = malloc(sizeof(struct task));
    INIT_LIST_HEAD(&task->list);

    if (setjmp(task->env) == 0) {
        task_add(task);
        longjmp(sched, 1);
    }

    task = cur_task;

    while (!attr_obj.end) {
        if (setjmp(task->env) == 0) {
            int move;
            struct timespec start, end;

            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start);
            move = mcts(board, 'O');
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end);

            uint64_t delta_ns = (end.tv_sec - start.tv_sec) * 1000000000UL +
                                (end.tv_nsec - start.tv_nsec);
            ai_state.total_cpu_ns_O += delta_ns;

            if (move != -1)
                board[move] = 'O';
            task_add(task);
            task_switch();
        }
        task = cur_task;
        char win = check_win(board);
        if (win != ' ') {
            memset(board, ' ', N_GRIDS);
        }
    }
    free(task);
    longjmp(sched, 1);
}

void task_X(void)
{
    struct task *task = malloc(sizeof(struct task));
    INIT_LIST_HEAD(&task->list);

    if (setjmp(task->env) == 0) {
        task_add(task);
        longjmp(sched, 1);
    }

    task = cur_task;

    while (!attr_obj.end) {
        if (setjmp(task->env) == 0) {
            int move;
            struct timespec start, end;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start);
            move = negamax_predict(board, 'X').move;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end);
            uint64_t delta_ns = (end.tv_sec - start.tv_sec) * 1000000000UL +
                                (end.tv_nsec - start.tv_nsec);
            ai_state.total_cpu_ns_X += delta_ns;
            if (move != -1) {
                board[move] = 'X';
            }
            task_add(task);
            task_switch();
        }
        task = cur_task;
        char win = check_win(board);
        if (win != ' ') {
            memset(board, ' ', N_GRIDS);
        }
    }
    free(task);
    longjmp(sched, 1);
}

void task_draw(void)
{
    struct task *task = malloc(sizeof(struct task));
    INIT_LIST_HEAD(&task->list);

    if (setjmp(task->env) == 0) {
        task_add(task);
        longjmp(sched, 1);
    }

    task = cur_task;

    while (!attr_obj.end) {
        if (setjmp(task->env) == 0) {
            if (!attr_obj.display) {
                task_add(task);
                task_switch();
            }

            task = cur_task;

            int i = 0, k = 0;
            display_buf[i++] = '\n';
            display_buf[i++] = '\n';

            while (i < DRAWBUFFER_SIZE) {
                for (int j = 0; j < (BOARD_SIZE << 1) - 1 && k < N_GRIDS; j++) {
                    display_buf[i++] = j & 1 ? '|' : board[k++];
                }
                display_buf[i++] = '\n';
                for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
                    display_buf[i++] = '-';
                }
                display_buf[i++] = '\n';
            }
            time_t now = time(NULL);
            const struct tm *t = localtime(&now);
            printf("\033[H\033[J");
            display_buf[DRAWBUFFER_SIZE - 1] = '\0';
            printf("%s", display_buf);
            printf("\n%04d-%02d-%02d %02d:%02d:%02d\n", t->tm_year + 1900,
                   t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);

            task_add(task);
            task_switch();
        }
        task = cur_task;
    }
    free(task);
    longjmp(sched, 1);
}


static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~IXON;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}


static void task_keyboard_handler(void)
{
    struct task *task = malloc(sizeof(struct task));
    INIT_LIST_HEAD(&task->list);

    if (setjmp(task->env) == 0) {
        task_add(task);
        longjmp(sched, 1);
    }

    task = cur_task;

    char input;
    while (!attr_obj.end) {
        if (setjmp(task->env) == 0) {
            if (read(STDIN_FILENO, &input, 1) == 1) {
                switch (input) {
                case 16: /*CTRL P*/
                    attr_obj.display ^= 1;
                    if (!attr_obj.display)
                        printf("Stopping to display the chess board...\n");
                    break;
                case 17: /*CTRL Q*/
                    attr_obj.end = 1;
                    printf("\n\nStopping the tic-tac-toe game...\n");
                    break;
                }
            }
            task_add(task);
            task_switch();
        }
        task = cur_task;
    }
    free(task);
    longjmp(sched, 1);
}


#define FIXED_SHIFT 16
#define FIXED_ONE (1 << FIXED_SHIFT)
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

int main()
{
    struct timespec wall_start, wall_end;
    clock_gettime(CLOCK_MONOTONIC, &wall_start);
    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    attr_obj.display = 1;
    attr_obj.end = 0;

    mcts_init();
    negamax_init();
    INIT_LIST_HEAD(&tasklist);

    void (*registered_tasks[])(void) = {
        task_O, task_keyboard_handler, task_draw, task_keyboard_handler,
        task_X, task_keyboard_handler, task_draw, task_keyboard_handler};
    tasks = registered_tasks;
    ntasks = ARRAY_SIZE(registered_tasks);
    schedule();

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);
    clock_gettime(CLOCK_MONOTONIC, &wall_end);
    uint64_t delta_wall_ns =
        (wall_end.tv_sec - wall_start.tv_sec) * 1000000000UL +
        (wall_end.tv_nsec - wall_start.tv_nsec);
    uint64_t load_x = (ai_state.total_cpu_ns_X << FIXED_SHIFT) / delta_wall_ns;
    uint64_t load_o = (ai_state.total_cpu_ns_O << FIXED_SHIFT) / delta_wall_ns;

    printf("ai negamax load ratio : %lu.%04lu\n", load_x >> FIXED_SHIFT,
           ((load_x & (FIXED_ONE - 1)) * 10000) >> FIXED_SHIFT);
    printf("ai mcts load ratio : %lu.%04lu\n", load_o >> FIXED_SHIFT,
           ((load_o & (FIXED_ONE - 1)) * 10000) >> FIXED_SHIFT);
    return 0;
}
// static bool status_check(void)
// {
//     FILE *fp = fopen(XO_STATUS_FILE, "r");
//     if (!fp) {
//         printf("kxo status : not loaded\n");
//         return false;
//     }

//     char read_buf[20];
//     fgets(read_buf, 20, fp);
//     read_buf[strcspn(read_buf, "\n")] = 0;
//     if (strcmp("live", read_buf)) {
//         printf("kxo status : %s\n", read_buf);
//         fclose(fp);
//         return false;
//     }
//     fclose(fp);
//     return true;
// }



// static struct termios orig_termios;

// static void raw_mode_disable(void)
// {
//     tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
// }

// static void raw_mode_enable(void)
// {
//     tcgetattr(STDIN_FILENO, &orig_termios);
//     atexit(raw_mode_disable);
//     struct termios raw = orig_termios;
//     raw.c_iflag &= ~IXON;
//     raw.c_lflag &= ~(ECHO | ICANON);
//     tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
// }

// static bool read_attr, end_attr;

// static void listen_keyboard_handler(void)
// {
//     int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
//     char input;

//     if (read(STDIN_FILENO, &input, 1) == 1) {
//         char buf[20];
//         switch (input) {
//         case 16: /* Ctrl-P */
//             read(attr_fd, buf, 6);
//             buf[0] = (buf[0] - '0') ? '0' : '1';
//             read_attr ^= 1;
//             write(attr_fd, buf, 6);
//             if (!read_attr)
//                 printf("\n\nStopping to display the chess board...\n");
//             break;
//         case 17: /* Ctrl-Q */
//             read(attr_fd, buf, 6);
//             buf[4] = '1';
//             read_attr = false;
//             end_attr = true;
//             write(attr_fd, buf, 6);
//             printf("\n\nStopping the kernel space tic-tac-toe game...\n");
//             break;
//         }
//     }
//     close(attr_fd);
// }

// static void decoding(unsigned int decoding_val, char *display_buf)
// {
//     char table[N_GRIDS];
//     memset(table, ' ', sizeof(table));
//     for (int i = N_GRIDS - 1; i >= 0; i--) {
//         unsigned int val = (decoding_val >> (i << 1)) & 0x3;
//         switch (val) {
//         case 0x0:
//             table[N_GRIDS - 1 - i] = ' ';
//             break;
//         case 0x2:
//             table[N_GRIDS - 1 - i] = 'X';
//             break;
//         case 0x3:
//             table[N_GRIDS - 1 - i] = 'O';
//             break;
//         }
//     }

//     int i = 0, k = 0;
//     display_buf[i++] = '\n';
//     display_buf[i++] = '\n';

//     while (i < DRAWBUFFER_SIZE) {
//         for (int j = 0; j < (BOARD_SIZE << 1) - 1 && k < N_GRIDS; j++) {
//             display_buf[i++] = j & 1 ? '|' : table[k++];
//         }
//         display_buf[i++] = '\n';
//         for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
//             display_buf[i++] = '-';
//         }
//         display_buf[i++] = '\n';
//     }
// }

// int main(int argc, char *argv[])
// {
//     if (!status_check())
//         exit(1);

//     raw_mode_enable();
//     int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
//     fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

//     static char display_buf[DRAWBUFFER_SIZE] = {0};
//     unsigned int decoding_val __attribute__((aligned(64)));

//     fd_set readset;
//     int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
//     int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
//     read_attr = true;
//     end_attr = false;

//     while (!end_attr) {
//         FD_ZERO(&readset);
//         FD_SET(STDIN_FILENO, &readset);
//         FD_SET(device_fd, &readset);

//         int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
//         if (result < 0) {
//             printf("Error with select system call\n");
//             exit(1);
//         }

//         if (FD_ISSET(STDIN_FILENO, &readset)) {
//             FD_CLR(STDIN_FILENO, &readset);
//             listen_keyboard_handler();
//         } else if (read_attr && FD_ISSET(device_fd, &readset)) {
//             time_t now = time(NULL);
//             const struct tm *t = localtime(&now);
//             FD_CLR(device_fd, &readset);
//             printf("\033[H\033[J"); /* ASCII escape code to clear the screen
//             */ read(device_fd, &decoding_val, sizeof(decoding_val));
//             decoding(decoding_val, display_buf);
//             display_buf[DRAWBUFFER_SIZE - 1] = '\0';
//             printf("%s", display_buf);
//             printf("\n%04d-%02d-%02d %02d:%02d:%02d\n", t->tm_year + 1900,
//                    t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min,
//                    t->tm_sec);
//         }
//     }

//     raw_mode_disable();
//     fcntl(STDIN_FILENO, F_SETFL, flags);

//     close(device_fd);

//     return 0;
// }
