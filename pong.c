#include <math.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Constants ─────────────────────────────────────── */
#define PADDLE_LEN 4
#define WIN_SCORE 7
#define BALL_DELAY_US 30000 /* microseconds per frame (~33 fps) */

/* AI reaction accuracy (0.0 = perfect, 1.0 = very sloppy) */
#define AI_EASY_ERROR 3.5
#define AI_HARD_ERROR 0.6

/* AI speed limit (cols per frame) */
#define AI_EASY_SPEED 1
#define AI_HARD_SPEED 2

/* Ball speed multiplier after each rally hit */
#define SPEED_STEP 0.08

/* ── Game state ─────────────────────────────────────── */
typedef struct {
  double x, y;   /* ball position (sub-pixel) */
  double vx, vy; /* velocity (cols/frame, rows/frame) */
  double speed;  /* base speed scalar */
} Ball;

typedef struct {
  int y; /* top row of paddle */
  int score;
} Paddle;

typedef enum { EASY, HARD } Difficulty;

/* ── Helpers ────────────────────────────────────────── */
static double randf(void) { return (double)rand() / (double)RAND_MAX; }

/* Clamp integer */
static int clampi(int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

/* Reset ball to centre, heading toward a random side */
static void reset_ball(Ball *b, int rows, int cols, int toward_player) {
  b->x = cols / 2.0;
  b->y = rows / 2.0;
  b->speed = 1.0;

  double angle = (randf() * 40.0 - 20.0) * 3.14159 / 180.0;
  double dir = toward_player ? -1.0 : 1.0;
  b->vx = dir * 1.1 * cos(angle);
  b->vy = 1.1 * sin(angle);
}

/* Draw a paddle */
static void draw_paddle(int x, int y, int len, int attr) {
  attron(attr);
  for (int i = 0; i < len; i++) {
    mvaddch(y + i, x, ACS_BLOCK);
  }
  attroff(attr);
}

/* Draw the ball */
static void draw_ball(int y, int x) {
  attron(COLOR_PAIR(3) | A_BOLD);
  mvaddch(y, x, 'O');
  attroff(COLOR_PAIR(3) | A_BOLD);
}

/* Draw dashed centre line */
static void draw_net(int rows, int cols) {
  attron(COLOR_PAIR(4) | A_DIM);
  for (int r = 1; r < rows - 1; r++) {
    if (r % 2 == 0)
      mvaddch(r, cols / 2, ACS_VLINE);
  }
  attroff(COLOR_PAIR(4) | A_DIM);
}

/* Draw scoreboard */
static void draw_scores(int rows, int cols, int ps, int as, Difficulty diff) {
  attron(COLOR_PAIR(5) | A_BOLD);
  mvprintw(0, cols / 2 - 12, " YOU: %d   AI: %d   [%s] ", ps, as,
           diff == HARD ? "HARD" : "EASY");
  attroff(COLOR_PAIR(5) | A_BOLD);
}

/* ── Main menu ──────────────────────────────────────── */
static Difficulty show_menu(int rows, int cols) {
  clear();
  attron(COLOR_PAIR(5) | A_BOLD);
  const char *title = "P O N G";
  mvprintw(rows / 2 - 5, cols / 2 - (int)strlen(title) / 2, "%s", title);
  attroff(COLOR_PAIR(5) | A_BOLD);

  attron(COLOR_PAIR(2));
  mvprintw(rows / 2 - 2, cols / 2 - 10, "Select difficulty:");
  mvprintw(rows / 2, cols / 2 - 10, "  [E]  Easy");
  mvprintw(rows / 2 + 1, cols / 2 - 10, "  [H]  Hard");
  mvprintw(rows / 2 + 3, cols / 2 - 10, "  [Q]  Quit");
  attroff(COLOR_PAIR(2));

  refresh();

  while (1) {
    int ch = getch();
    if (ch == 'e' || ch == 'E')
      return EASY;
    if (ch == 'h' || ch == 'H')
      return HARD;
    if (ch == 'q' || ch == 'Q') {
      endwin();
      exit(0);
    }
  }
}

/* ── Victory screen */
static int show_end(int rows, int cols, int player_won) {
  clear();
  attron(COLOR_PAIR(player_won ? 5 : 1) | A_BOLD);
  const char *msg = player_won ? "YOU WIN!" : "AI WINS!";
  mvprintw(rows / 2 - 2, cols / 2 - 4, "%s", msg);
  attroff(COLOR_PAIR(player_won ? 5 : 1) | A_BOLD);

  attron(COLOR_PAIR(2));
  mvprintw(rows / 2 + 1, cols / 2 - 12, "[R] Play again   [Q] Quit");
  attroff(COLOR_PAIR(2));
  refresh();

  while (1) {
    int ch = getch();
    if (ch == 'r' || ch == 'R')
      return 1;
    if (ch == 'q' || ch == 'Q')
      return 0;
  }
}

/* ── Core game loop */
static void run_game(Difficulty diff) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);

  /* Paddle x positions */
  int px = 2;
  int ax = cols - 3;

  Paddle player = {rows / 2 - PADDLE_LEN / 2, 0};
  Paddle ai = {rows / 2 - PADDLE_LEN / 2, 0};

  Ball ball;
  reset_ball(&ball, rows, cols, 0);

  /* AI "target" with error offset, updated occasionally */
  double ai_target = rows / 2.0;
  int ai_update_timer = 0;

  /* Difficulty params */
  double ai_error = (diff == HARD) ? AI_HARD_ERROR : AI_EASY_ERROR;
  int ai_speed = (diff == HARD) ? AI_HARD_SPEED : AI_EASY_SPEED;

  nodelay(stdscr, TRUE); /* non-blocking input */
  curs_set(0);

  while (1) {
    /* ── Input ── */
    int ch = getch();
    if (ch == 'q' || ch == 'Q') {
      endwin();
      exit(0);
    }
    if (ch == KEY_UP || ch == 'w' || ch == 'W') {
      player.y = clampi(player.y - 1, 1, rows - 2 - PADDLE_LEN);
    }
    if (ch == KEY_DOWN || ch == 's' || ch == 'S') {
      player.y = clampi(player.y + 1, 1, rows - 2 - PADDLE_LEN);
    }

    /* ── AI logic ── */
    ai_update_timer++;
    int ai_interval = (diff == HARD) ? 3 : 6;
    if (ai_update_timer >= ai_interval) {
      ai_update_timer = 0;
      /* Only react when ball moving toward AI */
      if (ball.vx > 0) {
        double error = (randf() * 2.0 - 1.0) * ai_error;
        ai_target = ball.y + error;
      } else {
        /* Drift lazily to centre */
        ai_target = rows / 2.0 + (randf() * 2.0 - 1.0) * 2.0;
      }
    }

    /* Move AI paddle toward target */
    int ai_centre = ai.y + PADDLE_LEN / 2;
    if (ai_centre < (int)ai_target - 1) {
      ai.y = clampi(ai.y + ai_speed, 1, rows - 2 - PADDLE_LEN);
    } else if (ai_centre > (int)ai_target + 1) {
      ai.y = clampi(ai.y - ai_speed, 1, rows - 2 - PADDLE_LEN);
    }

    /* ── Ball physics ── */
    ball.x += ball.vx;
    ball.y += ball.vy;

    /* Top/bottom wall bounce */
    if (ball.y <= 1.0) {
      ball.y = 1.0;
      ball.vy = fabs(ball.vy);
    }
    if (ball.y >= rows - 2.0) {
      ball.y = rows - 2.0;
      ball.vy = -fabs(ball.vy);
    }

    int bx = (int)(ball.x + 0.5);
    int by = (int)(ball.y + 0.5);

    /* Player paddle collision */
    if (bx <= px + 1 && bx >= px && by >= player.y &&
        by < player.y + PADDLE_LEN) {
      ball.x = px + 1.0;
      ball.vx = fabs(ball.vx);
      /* Add spin based on hit position */
      double rel =
          (ball.y - (player.y + PADDLE_LEN / 2.0)) / (PADDLE_LEN / 2.0);
      ball.vy = rel * 1.2;
      ball.speed += SPEED_STEP;
      /* Normalise and scale */
      double mag = sqrt(ball.vx * ball.vx + ball.vy * ball.vy);
      ball.vx = (ball.vx / mag) * (1.1 + ball.speed);
      ball.vy = (ball.vy / mag) * (1.1 + ball.speed);
    }

    /* AI paddle collision */
    if (bx >= ax - 1 && bx <= ax && by >= ai.y && by < ai.y + PADDLE_LEN) {
      ball.x = ax - 1.0;
      ball.vx = -fabs(ball.vx);
      double rel = (ball.y - (ai.y + PADDLE_LEN / 2.0)) / (PADDLE_LEN / 2.0);
      ball.vy = rel * 1.2;
      ball.speed += SPEED_STEP;
      double mag = sqrt(ball.vx * ball.vx + ball.vy * ball.vy);
      ball.vx = (ball.vx / mag) * (1.1 + ball.speed);
      ball.vy = (ball.vy / mag) * (1.1 + ball.speed);
    }

    /* Scoring  */
    if (ball.x < 0) {
      ai.score++;
      if (ai.score >= WIN_SCORE)
        break;
      reset_ball(&ball, rows, cols, 0);
      usleep(600000);
    }
    if (ball.x > cols - 1) {
      player.score++;
      if (player.score >= WIN_SCORE)
        break;
      reset_ball(&ball, rows, cols, 1);
      usleep(600000);
    }

    /* Draw */
    clear();

    /* Border */
    attron(COLOR_PAIR(4) | A_DIM);
    for (int c = 0; c < cols; c++) {
      mvaddch(0, c, ACS_HLINE);
      mvaddch(rows - 1, c, ACS_HLINE);
    }
    attroff(COLOR_PAIR(4) | A_DIM);

    draw_net(rows, cols);
    draw_scores(rows, cols, player.score, ai.score, diff);
    draw_paddle(px, player.y, PADDLE_LEN, COLOR_PAIR(1) | A_BOLD);
    draw_paddle(ax, ai.y, PADDLE_LEN, COLOR_PAIR(2) | A_BOLD);
    draw_ball(by, bx);

    /* Controls hint */
    attron(COLOR_PAIR(4) | A_DIM);
    mvprintw(rows - 1, 1, " W/S or UP/DOWN to move | Q quit ");
    attroff(COLOR_PAIR(4) | A_DIM);

    refresh();
    usleep(BALL_DELAY_US);
  }

  /* End screen */
  int player_won = (player.score >= WIN_SCORE);
  int replay = show_end(rows, cols, player_won);
  if (!replay) {
    endwin();
    exit(0);
  }
}

/* ── Entry point ────────────────────────────────────── */
int main(void) {
  srand((unsigned)time(NULL));

  initscr();
  if (!has_colors()) {
    endwin();
    fprintf(stderr, "Your terminal does not support colours.\n");
    return 1;
  }
  start_color();
  use_default_colors();

  /* Colour pairs */
  init_pair(1, COLOR_CYAN, -1);   /* player paddle */
  init_pair(2, COLOR_RED, -1);    /* AI paddle / menu text */
  init_pair(3, COLOR_YELLOW, -1); /* ball */
  init_pair(4, COLOR_WHITE, -1);  /* net / border / hints */
  init_pair(5, COLOR_GREEN, -1);  /* score / title */

  keypad(stdscr, TRUE);
  noecho();
  cbreak();

  while (1) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)rows;
    (void)cols;

    Difficulty diff = show_menu(LINES, COLS);
    run_game(diff);
  }

  endwin();
  return 0;
}

