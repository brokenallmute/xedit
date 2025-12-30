/***
 * xEdit - A simple terminal text editor
 * Copyright (C) 2024
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 ***/

/*** Includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <conio.h>
#include <direct.h>
#include <windows.h>
#define snprintf _snprintf
#define chdir _chdir
#ifndef __MINGW32__
typedef long ssize_t;
#endif
#else
#include <dirent.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

/*** Constants and Macros ***/

#define XEDIT_VERSION   "1.8.0"
#define TAB_STOP        4
#define SIDEBAR_WIDTH   24
#define LINENO_WIDTH    5
#define MAX_UNDO        10000

#define CTRL_KEY(k)     ((k) & 0x1f)

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

/*** Enumerations ***/

enum editor_key
  {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    SHIFT_ARROW_LEFT,
    SHIFT_ARROW_RIGHT,
    SHIFT_ARROW_UP,
    SHIFT_ARROW_DOWN
  };

enum editor_highlight
  {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
  };

enum undo_type
  {
    UNDO_INSERT,
    UNDO_DELETE,
    UNDO_SPLIT,
    UNDO_MERGE
  };

/*** Data Structures ***/

struct editor_syntax
{
  char *filetype;
  char **filematch;
  char **keywords;
  char *singleline_comment_start;
  int flags;
};

struct editor_row
{
  int size;
  char *chars;
  unsigned char *hl;
};

struct file_node
{
  char *name;
  int is_dir;
};

struct undo_action
{
  int type;
  int cx;
  int cy;
  int ch;
  int seq;
};

struct append_buffer
{
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

struct editor_config
{
  /* Cursor position.  */
  int cx;
  int cy;

  /* Scroll offsets.  */
  int row_offset;
  int col_offset;

  /* Terminal dimensions.  */
  int screen_rows;
  int screen_cols;

  /* File content.  */
  int num_rows;
  struct editor_row *row;
  int dirty;
  char *filename;
  struct editor_syntax *syntax;

  /* Sidebar state.  */
  int sidebar_open;
  int focus_sidebar;
  struct file_node *files;
  int file_count;
  int sidebar_cy;
  int sidebar_scroll;

  /* Selection state.  */
  int sel_cx;
  int sel_cy;
  int has_selection;

  /* Undo state.  */
  struct undo_action *undo_stack;
  int undo_size;
  int undo_capacity;
  int doing_undo;
  int undo_seq;
  int in_paste;

  /* Syntax database.  */
  struct editor_syntax *hldb;
  int hldb_entries;

  /* Status message.  */
  char status_msg[80];
  time_t status_msg_time;

#ifdef _WIN32
  HANDLE h_console;
  HANDLE h_input;
  DWORD original_mode;
  DWORD original_input_mode;
#else
  struct termios orig_termios;
#endif
};

/*** Global State ***/

static struct editor_config E;

/*** Function Prototypes ***/

/* Append buffer operations.  */
static void ab_append (struct append_buffer *ab, const char *s, int len);
static void ab_free (struct append_buffer *ab);

/* Terminal operations.  */
static void die (const char *s);
static void disable_raw_mode (void);
static void enable_raw_mode (void);
static int editor_read_key (void);
static int get_window_size (int *rows, int *cols);

/* Row operations.  */
static void editor_insert_row (int at, char *s, size_t len);
static void editor_free_row (struct editor_row *row);
static void editor_del_row (int at);
static void editor_row_insert_char (struct editor_row *row, int at, int c);
static void editor_row_del_char (struct editor_row *row, int at);
static void editor_row_append_string (struct editor_row *row,
                                      char *s, size_t len);

/* Editor operations.  */
static void editor_insert_char (int c);
static void editor_insert_newline (void);
static void editor_del_char (void);

/* Syntax highlighting.  */
static void editor_update_syntax (struct editor_row *row);
static void editor_select_syntax_highlight (void);
static int editor_syntax_to_color (int hl);

/* File I/O.  */
static void editor_open (char *filename);
static void editor_save (void);
static char *editor_rows_to_string (int *buflen);

/* Selection operations.  */
static void editor_clear_selection (void);
static void editor_start_selection (void);
static int editor_has_selection (void);
static void editor_get_selection_bounds (int *start_y, int *start_x,
                                         int *end_y, int *end_x);
static int is_selected (int row, int col);
static void editor_delete_selection (void);

/* Clipboard operations.  */
static void editor_copy_to_clipboard (const char *str);
static char *editor_get_from_clipboard (void);
static char *editor_get_selected_text (void);
static void editor_copy (void);
static void editor_paste (void);
static void editor_cut (void);

/* Undo operations.  */
static void editor_save_undo (int type, int cx, int cy, int ch);
static void editor_undo (void);

/* UI operations.  */
static void editor_set_status_message (const char *fmt, ...);
static void editor_refresh_screen (void);
static void editor_scroll (void);
static void editor_draw_rows (struct append_buffer *ab);
static void editor_draw_status_bar (struct append_buffer *ab);
static void editor_draw_message_bar (struct append_buffer *ab);

/* Input operations.  */
static char *editor_prompt (char *prompt, void (*callback) (char *, int));
static void editor_move_cursor (int key);
static void editor_process_keypress (void);

/* Search operations.  */
static void editor_find (void);
static void editor_find_callback (char *query, int key);

/* File list operations.  */
static void editor_refresh_file_list (void);
static int compare_files (const void *a, const void *b);

/* Syntax database operations.  */
static void load_syntax_database (void);
static void free_syntax (struct editor_syntax *s);
static char **split_string (char *str);
static int is_separator (int c);

/* Initialization.  */
static void init_editor (void);
static void cleanup (void);

/*** Windows Compatibility ***/

#ifdef _WIN32
static ssize_t
getline (char **lineptr, size_t *n, FILE *stream)
{
  size_t pos = 0;
  int c;

  if (lineptr == NULL || n == NULL || stream == NULL)
    return -1;

  if (*lineptr == NULL)
    {
      *n = 128;
      *lineptr = (char *) malloc (*n);
      if (*lineptr == NULL)
        return -1;
    }

  while ((c = fgetc (stream)) != EOF)
    {
      if (pos + 1 >= *n)
        {
          size_t new_len = *n + (*n >> 2);
          char *new_ptr;

          if (new_len < 128)
            new_len = 128;

          new_ptr = (char *) realloc (*lineptr, new_len);
          if (new_ptr == NULL)
            return -1;

          *lineptr = new_ptr;
          *n = new_len;
        }

      (*lineptr)[pos++] = (char) c;

      if (c == '\n')
        break;
    }

  if (c == EOF && pos == 0)
    return -1;

  (*lineptr)[pos] = '\0';
  return (ssize_t) pos;
}
#endif /* _WIN32 */

/*** Append Buffer Implementation ***/

static void
ab_append (struct append_buffer *ab, const char *s, int len)
{
  char *new_buf;

  new_buf = realloc (ab->b, ab->len + len);
  if (new_buf == NULL)
    return;

  memcpy (&new_buf[ab->len], s, len);
  ab->b = new_buf;
  ab->len += len;
}

static void
ab_free (struct append_buffer *ab)
{
  free (ab->b);
  ab->b = NULL;
  ab->len = 0;
}

/*** Terminal Implementation ***/

static void
die (const char *s)
{
#ifdef _WIN32
  SetConsoleMode (E.h_console, E.original_mode);
  SetConsoleMode (E.h_input, E.original_input_mode);
#endif

  printf ("\x1b[2J\x1b[H");
  perror (s);
  exit (EXIT_FAILURE);
}

static void
disable_raw_mode (void)
{
#ifdef _WIN32
  SetConsoleMode (E.h_console, E.original_mode);
  SetConsoleMode (E.h_input, E.original_input_mode);
#else
  if (tcsetattr (STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die ("tcsetattr");
#endif
}

static void
enable_raw_mode (void)
{
#ifdef _WIN32
  DWORD new_mode;
  DWORD new_input_mode;

  E.h_console = GetStdHandle (STD_OUTPUT_HANDLE);
  E.h_input = GetStdHandle (STD_INPUT_HANDLE);

  SetConsoleOutputCP (65001);
  SetConsoleCP (65001);

  GetConsoleMode (E.h_console, &E.original_mode);
  new_mode = E.original_mode
             | ENABLE_VIRTUAL_TERMINAL_PROCESSING
             | DISABLE_NEWLINE_AUTO_RETURN;
  SetConsoleMode (E.h_console, new_mode);

  GetConsoleMode (E.h_input, &E.original_input_mode);
  new_input_mode = ENABLE_VIRTUAL_TERMINAL_INPUT;
  SetConsoleMode (E.h_input, new_input_mode);
#else
  struct termios raw;

  if (tcgetattr (STDIN_FILENO, &E.orig_termios) == -1)
    die ("tcgetattr");

  atexit (disable_raw_mode);

  raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr (STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die ("tcsetattr");
#endif

#ifdef _WIN32
  atexit (disable_raw_mode);
#endif
}

static int
editor_read_key (void)
{
#ifdef _WIN32
  int c;
  int seq;
  int is_shift;

  c = _getch ();

  if (c == 0 || c == 224)
    {
      seq = _getch ();
      is_shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;

      switch (seq)
        {
        case 72:
          return is_shift ? SHIFT_ARROW_UP : ARROW_UP;
        case 80:
          return is_shift ? SHIFT_ARROW_DOWN : ARROW_DOWN;
        case 75:
          return is_shift ? SHIFT_ARROW_LEFT : ARROW_LEFT;
        case 77:
          return is_shift ? SHIFT_ARROW_RIGHT : ARROW_RIGHT;
        case 73:
          return PAGE_UP;
        case 81:
          return PAGE_DOWN;
        case 83:
          return DEL_KEY;
        case 71:
          return HOME_KEY;
        case 79:
          return END_KEY;
        }

      return '\x1b';
    }

  return c;
#else
  int nread;
  char c;
  char seq[6];

  while ((nread = read (STDIN_FILENO, &c, 1)) != 1)
    {
      if (nread == -1 && errno != EAGAIN)
        die ("read");
    }

  if (c == '\x1b')
    {
      if (read (STDIN_FILENO, &seq[0], 1) != 1)
        return '\x1b';
      if (read (STDIN_FILENO, &seq[1], 1) != 1)
        return '\x1b';

      if (seq[0] == '[')
        {
          if (seq[1] >= '0' && seq[1] <= '9')
            {
              if (read (STDIN_FILENO, &seq[2], 1) != 1)
                return '\x1b';

              if (seq[2] == '~')
                {
                  switch (seq[1])
                    {
                    case '1':
                      return HOME_KEY;
                    case '3':
                      return DEL_KEY;
                    case '4':
                      return END_KEY;
                    case '5':
                      return PAGE_UP;
                    case '6':
                      return PAGE_DOWN;
                    case '7':
                      return HOME_KEY;
                    case '8':
                      return END_KEY;
                    }
                }
              else if (seq[1] == '1' && seq[2] == ';')
                {
                  if (read (STDIN_FILENO, &seq[3], 1) != 1)
                    return '\x1b';
                  if (read (STDIN_FILENO, &seq[4], 1) != 1)
                    return '\x1b';

                  if (seq[3] == '2')
                    {
                      switch (seq[4])
                        {
                        case 'A':
                          return SHIFT_ARROW_UP;
                        case 'B':
                          return SHIFT_ARROW_DOWN;
                        case 'C':
                          return SHIFT_ARROW_RIGHT;
                        case 'D':
                          return SHIFT_ARROW_LEFT;
                        }
                    }
                }
            }
          else
            {
              switch (seq[1])
                {
                case 'A':
                  return ARROW_UP;
                case 'B':
                  return ARROW_DOWN;
                case 'C':
                  return ARROW_RIGHT;
                case 'D':
                  return ARROW_LEFT;
                case 'H':
                  return HOME_KEY;
                case 'F':
                  return END_KEY;
                }
            }
        }
      else if (seq[0] == 'O')
        {
          switch (seq[1])
            {
            case 'H':
              return HOME_KEY;
            case 'F':
              return END_KEY;
            }
        }

      return '\x1b';
    }

  return c;
#endif
}

static int
get_window_size (int *rows, int *cols)
{
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO csbi;

  if (GetConsoleScreenBufferInfo (GetStdHandle (STD_OUTPUT_HANDLE), &csbi))
    {
      *cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
      *rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
      return 0;
    }

  return -1;
#else
  struct winsize ws;

  if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
      char buf[32];
      unsigned int i = 0;

      if (write (STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
        return -1;
      if (write (STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

      while (i < sizeof (buf) - 1)
        {
          if (read (STDIN_FILENO, &buf[i], 1) != 1)
            break;
          if (buf[i] == 'R')
            break;
          i++;
        }

      buf[i] = '\0';

      if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
      if (sscanf (&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

      return 0;
    }

  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
#endif
}

/*** Undo System Implementation ***/

static void
editor_save_undo (int type, int cx, int cy, int ch)
{
  struct undo_action *act;
  struct undo_action *new_stack;

  if (E.doing_undo)
    return;

  /* Limit undo stack size.  */
  if (E.undo_size >= MAX_UNDO)
    {
      int keep = MAX_UNDO / 2;
      memmove (E.undo_stack, &E.undo_stack[E.undo_size - keep],
               sizeof (struct undo_action) * keep);
      E.undo_size = keep;
    }

  if (E.undo_size == E.undo_capacity)
    {
      E.undo_capacity = (E.undo_capacity == 0) ? 128 : E.undo_capacity * 2;
      if (E.undo_capacity > MAX_UNDO)
        E.undo_capacity = MAX_UNDO;

      new_stack = realloc (E.undo_stack,
                           sizeof (struct undo_action) * E.undo_capacity);
      if (new_stack == NULL)
        return;

      E.undo_stack = new_stack;
    }

  act = &E.undo_stack[E.undo_size++];
  act->type = type;
  act->cx = cx;
  act->cy = cy;
  act->ch = ch;
  act->seq = E.undo_seq;
}

static void
editor_undo (void)
{
  int current_seq;
  struct undo_action act;

  if (E.undo_size == 0)
    {
      editor_set_status_message ("Nothing to undo");
      return;
    }

  E.doing_undo = 1;
  current_seq = E.undo_stack[E.undo_size - 1].seq;

  do
    {
      act = E.undo_stack[--E.undo_size];
      E.cx = act.cx;
      E.cy = act.cy;

      switch (act.type)
        {
        case UNDO_INSERT:
          if (E.cy < E.num_rows && E.cx < E.row[E.cy].size)
            editor_row_del_char (&E.row[E.cy], E.cx);
          break;

        case UNDO_DELETE:
          if (E.cy >= E.num_rows)
            editor_insert_row (E.num_rows, "", 0);
          if (E.cy < E.num_rows)
            editor_row_insert_char (&E.row[E.cy], E.cx, act.ch);
          break;

        case UNDO_SPLIT:
          if (E.cy + 1 < E.num_rows)
            {
              struct editor_row *current = &E.row[E.cy];
              struct editor_row *next = &E.row[E.cy + 1];
              editor_row_append_string (current, next->chars, next->size);
              editor_del_row (E.cy + 1);
            }
          break;

        case UNDO_MERGE:
          if (E.cy < E.num_rows)
            {
              struct editor_row *row = &E.row[E.cy];
              char *remainder = strdup (&row->chars[E.cx]);

              row->size = E.cx;
              row->chars[E.cx] = '\0';
              editor_insert_row (E.cy + 1, remainder, strlen (remainder));
              free (remainder);
              E.cy++;
              E.cx = 0;
            }
          break;
        }
    }
  while (E.undo_size > 0
         && E.undo_stack[E.undo_size - 1].seq == current_seq);

  E.doing_undo = 0;
  editor_set_status_message ("Undone!");
}

/*** Selection Implementation ***/

static void
editor_clear_selection (void)
{
  E.sel_cx = -1;
  E.sel_cy = -1;
  E.has_selection = 0;
}

static void
editor_start_selection (void)
{
  if (!E.has_selection)
    {
      E.sel_cx = E.cx;
      E.sel_cy = E.cy;
      E.has_selection = 1;
    }
}

static int
editor_has_selection (void)
{
  return E.has_selection && (E.sel_cx != E.cx || E.sel_cy != E.cy);
}

static void
editor_get_selection_bounds (int *start_y, int *start_x,
                             int *end_y, int *end_x)
{
  if (E.sel_cy < E.cy || (E.sel_cy == E.cy && E.sel_cx < E.cx))
    {
      *start_y = E.sel_cy;
      *start_x = E.sel_cx;
      *end_y = E.cy;
      *end_x = E.cx;
    }
  else
    {
      *start_y = E.cy;
      *start_x = E.cx;
      *end_y = E.sel_cy;
      *end_x = E.sel_cx;
    }
}

static int
is_selected (int row, int col)
{
  int start_y, start_x, end_y, end_x;

  if (!editor_has_selection ())
    return 0;

  editor_get_selection_bounds (&start_y, &start_x, &end_y, &end_x);

  if (row < start_y || row > end_y)
    return 0;
  if (row > start_y && row < end_y)
    return 1;
  if (start_y == end_y)
    return (col >= start_x && col < end_x);
  if (row == start_y)
    return (col >= start_x);
  if (row == end_y)
    return (col < end_x);

  return 0;
}

/*** Clipboard Implementation ***/

static void
editor_copy_to_clipboard (const char *str)
{
  if (str == NULL)
    return;

#ifdef _WIN32
  {
    HGLOBAL h_glo;
    size_t len;

    if (!OpenClipboard (NULL))
      return;

    EmptyClipboard ();
    len = strlen (str) + 1;
    h_glo = GlobalAlloc (GMEM_MOVEABLE, len);

    if (h_glo != NULL)
      {
        char *ptr = (char *) GlobalLock (h_glo);
        if (ptr != NULL)
          {
            memcpy (ptr, str, len);
            GlobalUnlock (h_glo);
            SetClipboardData (CF_TEXT, h_glo);
          }
      }

    CloseClipboard ();
  }
#else
  {
    FILE *pipe;

    pipe = popen ("xclip -selection clipboard 2>/dev/null "
                  "|| pbcopy 2>/dev/null", "w");
    if (pipe != NULL)
      {
        fwrite (str, 1, strlen (str), pipe);
        pclose (pipe);
      }
  }
#endif
}

static char *
editor_get_from_clipboard (void)
{
  char *result = NULL;

#ifdef _WIN32
  {
    HANDLE h_data;

    if (!OpenClipboard (NULL))
      return NULL;

    h_data = GetClipboardData (CF_TEXT);
    if (h_data != NULL)
      {
        char *psz_text = (char *) GlobalLock (h_data);
        if (psz_text != NULL)
          result = strdup (psz_text);
        GlobalUnlock (h_data);
      }

    CloseClipboard ();
  }
#else
  {
    FILE *pipe;

    pipe = popen ("xclip -selection clipboard -o 2>/dev/null "
                  "|| pbpaste 2>/dev/null", "r");
    if (pipe != NULL)
      {
        size_t capacity = 1024;
        size_t len = 0;
        int c;

        result = malloc (capacity);
        if (result != NULL)
          {
            while ((c = fgetc (pipe)) != EOF)
              {
                if (len + 1 >= capacity)
                  {
                    char *new_result;
                    capacity *= 2;
                    new_result = realloc (result, capacity);
                    if (new_result == NULL)
                      {
                        free (result);
                        pclose (pipe);
                        return NULL;
                      }
                    result = new_result;
                  }
                result[len++] = (char) c;
              }

            result[len] = '\0';

            /* Remove trailing newlines.  */
            while (len > 0
                   && (result[len - 1] == '\n' || result[len - 1] == '\r'))
              result[--len] = '\0';

            if (len == 0)
              {
                free (result);
                result = NULL;
              }
          }

        pclose (pipe);
      }
  }
#endif

  return result;
}

static char *
editor_get_selected_text (void)
{
  int start_y, start_x, end_y, end_x;
  size_t total_len;
  char *buf;
  char *p;
  int y;

  if (!editor_has_selection ())
    return NULL;

  editor_get_selection_bounds (&start_y, &start_x, &end_y, &end_x);

  /* Calculate total length.  */
  total_len = 0;
  for (y = start_y; y <= end_y; y++)
    {
      int row_start, row_end;

      if (y >= E.num_rows)
        break;

      row_start = (y == start_y) ? start_x : 0;
      row_end = (y == end_y) ? end_x : E.row[y].size;

      if (row_start > E.row[y].size)
        row_start = E.row[y].size;
      if (row_end > E.row[y].size)
        row_end = E.row[y].size;

      total_len += (row_end - row_start);
      if (y < end_y)
        total_len++;  /* Newline.  */
    }

  buf = malloc (total_len + 1);
  if (buf == NULL)
    return NULL;

  p = buf;
  for (y = start_y; y <= end_y; y++)
    {
      struct editor_row *row;
      int row_start, row_end, len;

      if (y >= E.num_rows)
        break;

      row = &E.row[y];
      row_start = (y == start_y) ? start_x : 0;
      row_end = (y == end_y) ? end_x : row->size;

      if (row_start > row->size)
        row_start = row->size;
      if (row_end > row->size)
        row_end = row->size;

      len = row_end - row_start;
      if (len > 0)
        {
          memcpy (p, &row->chars[row_start], len);
          p += len;
        }

      if (y < end_y)
        *p++ = '\n';
    }

  *p = '\0';
  return buf;
}

static void
editor_delete_selection (void)
{
  int start_y, start_x, end_y, end_x;

  if (!editor_has_selection ())
    return;

  editor_get_selection_bounds (&start_y, &start_x, &end_y, &end_x);

  E.undo_seq++;

  /* Move cursor to start of selection.  */
  E.cy = start_y;
  E.cx = start_x;

  if (start_y == end_y)
    {
      /* Single line selection.  */
      struct editor_row *row = &E.row[start_y];
      int i;

      for (i = start_x; i < end_x && i < row->size; i++)
        {
          editor_save_undo (UNDO_DELETE, start_x, start_y,
                            row->chars[start_x]);
          editor_row_del_char (row, start_x);
        }
    }
  else
    {
      /* Multi-line selection.  */
      char *end_remainder = NULL;
      int y;

      /* Save the end part of the last line.  */
      if (end_y < E.num_rows)
        {
          struct editor_row *end_row = &E.row[end_y];
          if (end_x < end_row->size)
            end_remainder = strdup (&end_row->chars[end_x]);
        }

      /* Delete lines from end to start.  */
      for (y = end_y; y > start_y; y--)
        {
          editor_save_undo (UNDO_MERGE, E.row[y - 1].size, y - 1, 0);
          editor_del_row (y);
        }

      /* Truncate first line and append remainder.  */
      if (start_y < E.num_rows)
        {
          struct editor_row *row = &E.row[start_y];
          int i;

          for (i = row->size - 1; i >= start_x; i--)
            editor_save_undo (UNDO_DELETE, i, start_y, row->chars[i]);

          row->size = start_x;
          row->chars[start_x] = '\0';

          if (end_remainder != NULL)
            {
              editor_row_append_string (row, end_remainder,
                                        strlen (end_remainder));
              free (end_remainder);
            }

          editor_update_syntax (row);
        }
    }

  editor_clear_selection ();
}

static void
editor_copy (void)
{
  char *text;

  if (editor_has_selection ())
    {
      text = editor_get_selected_text ();
    }
  else
    {
      /* Copy current line.  */
      if (E.cy >= E.num_rows)
        return;
      text = strdup (E.row[E.cy].chars);
    }

  if (text != NULL)
    {
      editor_copy_to_clipboard (text);
      free (text);
      editor_set_status_message ("Copied!");
    }
}

static void
editor_paste (void)
{
  char *clip;
  size_t len;
  size_t i;

  clip = editor_get_from_clipboard ();
  if (clip == NULL || clip[0] == '\0')
    {
      editor_set_status_message ("Clipboard empty");
      free (clip);
      return;
    }

  /* Delete selection first if any.  */
  if (editor_has_selection ())
    editor_delete_selection ();

  E.undo_seq++;
  E.in_paste = 1;

  len = strlen (clip);
  for (i = 0; i < len; i++)
    {
      char c = clip[i];

      /* Skip carriage returns (Windows line endings).  */
      if (c == '\r')
        continue;

      if (c == '\n')
        editor_insert_newline ();
      else
        editor_insert_char ((unsigned char) c);
    }

  E.in_paste = 0;
  E.undo_seq++;

  free (clip);
  editor_set_status_message ("Pasted!");
}

static void
editor_cut (void)
{
  if (editor_has_selection ())
    {
      char *text = editor_get_selected_text ();
      if (text != NULL)
        {
          editor_copy_to_clipboard (text);
          free (text);
        }
      editor_delete_selection ();
      editor_set_status_message ("Cut!");
    }
  else
    {
      /* Cut current line.  */
      if (E.cy >= E.num_rows)
        return;

      editor_copy_to_clipboard (E.row[E.cy].chars);
      E.undo_seq++;
      editor_save_undo (UNDO_MERGE, 0, E.cy, 0);
      editor_del_row (E.cy);

      if (E.cy >= E.num_rows && E.num_rows > 0)
        E.cy = E.num_rows - 1;

      E.cx = 0;
      editor_set_status_message ("Line cut!");
    }
}

/*** File List Implementation ***/

static int
compare_files (const void *a, const void *b)
{
  const struct file_node *fa = (const struct file_node *) a;
  const struct file_node *fb = (const struct file_node *) b;

  /* Directories first.  */
  if (fa->is_dir && !fb->is_dir)
    return -1;
  if (!fa->is_dir && fb->is_dir)
    return 1;

  /* ".." always first among directories.  */
  if (fa->is_dir && fb->is_dir)
    {
      if (strcmp (fa->name, "..") == 0)
        return -1;
      if (strcmp (fb->name, "..") == 0)
        return 1;
    }

  return strcmp (fa->name, fb->name);
}

static void
editor_refresh_file_list (void)
{
  int i;

  for (i = 0; i < E.file_count; i++)
    free (E.files[i].name);

  free (E.files);
  E.files = NULL;
  E.file_count = 0;

#ifdef _WIN32
  {
    WIN32_FIND_DATA fd;
    HANDLE h_find;

    h_find = FindFirstFile ("*", &fd);
    if (h_find != INVALID_HANDLE_VALUE)
      {
        do
          {
            if (strcmp (fd.cFileName, ".") == 0)
              continue;

            E.files = realloc (E.files,
                               sizeof (struct file_node) * (E.file_count + 1));
            E.files[E.file_count].is_dir =
              (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            E.files[E.file_count].name = strdup (fd.cFileName);
            E.file_count++;
          }
        while (FindNextFile (h_find, &fd));

        FindClose (h_find);
      }
  }
#else
  {
    DIR *d;

    d = opendir (".");
    if (d != NULL)
      {
        struct dirent *dir;

        while ((dir = readdir (d)) != NULL)
          {
            struct stat st;

            if (strcmp (dir->d_name, ".") == 0)
              continue;

            E.files = realloc (E.files,
                               sizeof (struct file_node) * (E.file_count + 1));
            E.files[E.file_count].is_dir =
              (stat (dir->d_name, &st) == 0 && S_ISDIR (st.st_mode));
            E.files[E.file_count].name = strdup (dir->d_name);
            E.file_count++;
          }

        closedir (d);
      }
  }
#endif

  /* Sort files.  */
  if (E.file_count > 0)
    qsort (E.files, E.file_count, sizeof (struct file_node), compare_files);
}

/*** Syntax Highlighting Implementation ***/

static char **
split_string (char *str)
{
  int count = 1;
  char *p;
  char **result;
  int idx = 0;
  char *copy;
  char *token;

  for (p = str; *p != '\0'; p++)
    {
      if (*p == ' ')
        count++;
    }

  result = malloc (sizeof (char *) * (count + 1));
  if (result == NULL)
    return NULL;

  copy = strdup (str);
  token = strtok (copy, " \r\n");

  while (token != NULL)
    {
      result[idx++] = strdup (token);
      token = strtok (NULL, " \r\n");
    }

  result[idx] = NULL;
  free (copy);

  return result;
}

static void
free_syntax (struct editor_syntax *s)
{
  int i;

  if (s == NULL)
    return;

  free (s->filetype);
  free (s->singleline_comment_start);

  if (s->filematch != NULL)
    {
      for (i = 0; s->filematch[i] != NULL; i++)
        free (s->filematch[i]);
      free (s->filematch);
    }

  if (s->keywords != NULL)
    {
      for (i = 0; s->keywords[i] != NULL; i++)
        free (s->keywords[i]);
      free (s->keywords);
    }
}

static void
load_syntax_database (void)
{
  FILE *fp;
  char *line = NULL;
  size_t len = 0;
  ssize_t nread;
  struct editor_syntax current_syntax;
  int step = 0;

  E.hldb = NULL;
  E.hldb_entries = 0;

  fp = fopen ("syntax.db", "r");
  if (fp == NULL)
    return;

  memset (&current_syntax, 0, sizeof (current_syntax));

  while ((nread = getline (&line, &len, fp)) != -1)
    {
      /* Trim newlines.  */
      while (nread > 0
             && (line[nread - 1] == '\n' || line[nread - 1] == '\r'))
        line[--nread] = '\0';

      if (nread == 0 || strcmp (line, "---") == 0)
        {
          if (step == 4)
            {
              E.hldb = realloc (E.hldb,
                                sizeof (struct editor_syntax)
                                * (E.hldb_entries + 1));
              E.hldb[E.hldb_entries++] = current_syntax;
              memset (&current_syntax, 0, sizeof (current_syntax));
              step = 0;
            }
          continue;
        }

      switch (step)
        {
        case 0:
          current_syntax.filetype = strdup (line);
          step++;
          break;

        case 1:
          current_syntax.filematch = split_string (line);
          step++;
          break;

        case 2:
          current_syntax.singleline_comment_start = strdup (line);
          step++;
          break;

        case 3:
          current_syntax.keywords = split_string (line);
          current_syntax.flags = HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS;
          step++;
          break;
        }
    }

  /* Handle last entry if file doesn't end with separator.  */
  if (step == 4)
    {
      E.hldb = realloc (E.hldb,
                        sizeof (struct editor_syntax) * (E.hldb_entries + 1));
      E.hldb[E.hldb_entries++] = current_syntax;
    }

  free (line);
  fclose (fp);
}

static int
is_separator (int c)
{
  return isspace (c) || c == '\0'
         || strchr (",.()+-/*=~%<>[];:{}&|^!?", c) != NULL;
}

static void
editor_update_syntax (struct editor_row *row)
{
  char **keywords;
  char *scs;
  int scs_len;
  int prev_sep = 1;
  int in_string = 0;
  int i = 0;

  row->hl = realloc (row->hl, row->size);
  if (row->size > 0)
    memset (row->hl, HL_NORMAL, row->size);

  if (E.syntax == NULL)
    return;

  keywords = E.syntax->keywords;
  scs = E.syntax->singleline_comment_start;
  scs_len = scs ? (int) strlen (scs) : 0;

  while (i < row->size)
    {
      char c = row->chars[i];
      unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

      /* Comments.  */
      if (scs_len && !in_string)
        {
          if (strncmp (&row->chars[i], scs, scs_len) == 0)
            {
              memset (&row->hl[i], HL_COMMENT, row->size - i);
              break;
            }
        }

      /* Strings.  */
      if (E.syntax->flags & HL_HIGHLIGHT_STRINGS)
        {
          if (in_string)
            {
              row->hl[i] = HL_STRING;
              if (c == '\\' && i + 1 < row->size)
                {
                  row->hl[i + 1] = HL_STRING;
                  i += 2;
                  continue;
                }
              if (c == in_string)
                in_string = 0;
              i++;
              prev_sep = 1;
              continue;
            }
          else
            {
              if (c == '"' || c == '\'')
                {
                  in_string = c;
                  row->hl[i] = HL_STRING;
                  i++;
                  continue;
                }
            }
        }

      /* Numbers.  */
      if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS)
        {
          if ((isdigit (c) && (prev_sep || prev_hl == HL_NUMBER))
              || (c == '.' && prev_hl == HL_NUMBER))
            {
              row->hl[i] = HL_NUMBER;
              i++;
              prev_sep = 0;
              continue;
            }
        }

      /* Keywords.  */
      if (prev_sep && keywords != NULL)
        {
          int j;

          for (j = 0; keywords[j] != NULL; j++)
            {
              int klen = (int) strlen (keywords[j]);
              int kw2 = keywords[j][klen - 1] == '|';

              if (kw2)
                klen--;

              if (strncmp (&row->chars[i], keywords[j], klen) == 0
                  && is_separator (row->chars[i + klen]))
                {
                  memset (&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                  i += klen;
                  break;
                }
            }

          if (keywords[j] != NULL)
            {
              prev_sep = 0;
              continue;
            }
        }

      prev_sep = is_separator (c);
      i++;
    }
}

static int
editor_syntax_to_color (int hl)
{
  switch (hl)
    {
    case HL_COMMENT:
      return 36;  /* Cyan.  */
    case HL_KEYWORD1:
      return 33;  /* Yellow.  */
    case HL_KEYWORD2:
      return 32;  /* Green.  */
    case HL_STRING:
      return 35;  /* Magenta.  */
    case HL_NUMBER:
      return 31;  /* Red.  */
    case HL_MATCH:
      return 34;  /* Blue.  */
    default:
      return 37;  /* White.  */
    }
}

static void
editor_select_syntax_highlight (void)
{
  char *ext;
  int j;

  E.syntax = NULL;

  if (E.filename == NULL)
    return;

  ext = strrchr (E.filename, '.');

  for (j = 0; j < E.hldb_entries; j++)
    {
      struct editor_syntax *s = &E.hldb[j];
      int i;

      if (s->filematch == NULL)
        continue;

      for (i = 0; s->filematch[i] != NULL; i++)
        {
          int is_ext = (s->filematch[i][0] == '.');

          if ((is_ext && ext && strcmp (ext, s->filematch[i]) == 0)
              || (!is_ext && strstr (E.filename, s->filematch[i]) != NULL))
            {
              int filerow;

              E.syntax = s;

              for (filerow = 0; filerow < E.num_rows; filerow++)
                editor_update_syntax (&E.row[filerow]);

              return;
            }
        }
    }
}

/*** Row Operations Implementation ***/

static void
editor_insert_row (int at, char *s, size_t len)
{
  if (at < 0 || at > E.num_rows)
    return;

  E.row = realloc (E.row, sizeof (struct editor_row) * (E.num_rows + 1));
  memmove (&E.row[at + 1], &E.row[at],
           sizeof (struct editor_row) * (E.num_rows - at));

  E.row[at].size = (int) len;
  E.row[at].chars = malloc (len + 1);
  memcpy (E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.row[at].hl = NULL;

  E.num_rows++;
  E.dirty++;

  editor_update_syntax (&E.row[at]);
}

static void
editor_free_row (struct editor_row *row)
{
  free (row->chars);
  free (row->hl);
  row->chars = NULL;
  row->hl = NULL;
}

static void
editor_del_row (int at)
{
  if (at < 0 || at >= E.num_rows)
    return;

  editor_free_row (&E.row[at]);
  memmove (&E.row[at], &E.row[at + 1],
           sizeof (struct editor_row) * (E.num_rows - at - 1));
  E.num_rows--;
  E.dirty++;
}

static void
editor_row_insert_char (struct editor_row *row, int at, int c)
{
  if (at < 0 || at > row->size)
    at = row->size;

  row->chars = realloc (row->chars, row->size + 2);
  memmove (&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = (char) c;
  E.dirty++;

  editor_update_syntax (row);
}

static void
editor_row_del_char (struct editor_row *row, int at)
{
  if (at < 0 || at >= row->size)
    return;

  memmove (&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  E.dirty++;

  editor_update_syntax (row);
}

static void
editor_row_append_string (struct editor_row *row, char *s, size_t len)
{
  row->chars = realloc (row->chars, row->size + len + 1);
  memcpy (&row->chars[row->size], s, len);
  row->size += (int) len;
  row->chars[row->size] = '\0';
  E.dirty++;

  editor_update_syntax (row);
}

/*** Editor Operations Implementation ***/

static void
editor_insert_char (int c)
{
  if (E.cy == E.num_rows)
    editor_insert_row (E.num_rows, "", 0);

  if (!E.in_paste && !E.doing_undo)
    {
      if (isspace (c) || ispunct (c))
        E.undo_seq++;
    }

  editor_save_undo (UNDO_INSERT, E.cx, E.cy, c);
  editor_row_insert_char (&E.row[E.cy], E.cx, c);
  E.cx++;
}

static void
editor_insert_newline (void)
{
  if (!E.in_paste && !E.doing_undo)
    E.undo_seq++;

  editor_save_undo (UNDO_SPLIT, E.cx, E.cy, 0);

  if (E.cx == 0)
    {
      /* Insert empty line above.  */
      editor_insert_row (E.cy, "", 0);
      E.cy++;
      E.cx = 0;
    }
  else
    {
      struct editor_row *row = &E.row[E.cy];
      int indent = 0;
      int remainder_len;
      int new_len;
      char *new_chars;

      /* Calculate indent (only if not pasting).  */
      if (!E.in_paste)
        {
          while (indent < row->size
                 && (row->chars[indent] == ' ' || row->chars[indent] == '\t'))
            indent++;

          if (indent > E.cx)
            indent = E.cx;
        }

      /* Create new line content.  */
      remainder_len = row->size - E.cx;
      new_len = indent + remainder_len;
      new_chars = malloc (new_len + 1);

      /* Add indent (spaces).  */
      memset (new_chars, ' ', indent);

      /* Add rest of current line.  */
      memcpy (new_chars + indent, &row->chars[E.cx], remainder_len);
      new_chars[new_len] = '\0';

      /* Truncate current line.  */
      row->size = E.cx;
      row->chars[E.cx] = '\0';
      editor_update_syntax (row);

      /* Insert new row.  */
      editor_insert_row (E.cy + 1, new_chars, new_len);
      free (new_chars);

      /* Move cursor.  */
      E.cy++;
      E.cx = indent;
    }
}

static void
editor_del_char (void)
{
  struct editor_row *row;

  if (E.cy == E.num_rows)
    return;
  if (E.cx == 0 && E.cy == 0)
    return;

  if (!E.doing_undo)
    {
      if (E.undo_size > 0)
        {
          struct undo_action *last = &E.undo_stack[E.undo_size - 1];
          if (last->type != UNDO_DELETE || last->seq != E.undo_seq)
            E.undo_seq++;
        }
      else
        {
          E.undo_seq++;
        }
    }

  row = &E.row[E.cy];

  if (E.cx > 0)
    {
      editor_save_undo (UNDO_DELETE, E.cx - 1, E.cy, row->chars[E.cx - 1]);
      editor_row_del_char (row, E.cx - 1);
      E.cx--;
    }
  else
    {
      /* Merge with previous line.  */
      E.cx = E.row[E.cy - 1].size;
      editor_save_undo (UNDO_MERGE, E.cx, E.cy - 1, 0);
      editor_row_append_string (&E.row[E.cy - 1], row->chars, row->size);
      editor_del_row (E.cy);
      E.cy--;
    }
}

/*** File I/O Implementation ***/

static char *
editor_rows_to_string (int *buflen)
{
  int totlen = 0;
  int j;
  char *buf;
  char *p;

  for (j = 0; j < E.num_rows; j++)
    totlen += E.row[j].size + 1;  /* +1 for newline.  */

  *buflen = totlen;
  buf = malloc (totlen + 1);
  if (buf == NULL)
    return NULL;

  p = buf;
  for (j = 0; j < E.num_rows; j++)
    {
      memcpy (p, E.row[j].chars, E.row[j].size);
      p += E.row[j].size;
      *p++ = '\n';
    }
  *p = '\0';

  return buf;
}

static void
editor_open (char *filename)
{
  FILE *fp;
  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  int i;

  /* Free existing rows.  */
  for (i = 0; i < E.num_rows; i++)
    editor_free_row (&E.row[i]);

  free (E.row);
  E.row = NULL;
  E.num_rows = 0;

  /* Clear undo stack.  */
  free (E.undo_stack);
  E.undo_stack = NULL;
  E.undo_size = 0;
  E.undo_capacity = 0;
  E.undo_seq = 0;

  /* Reset cursor.  */
  E.cx = 0;
  E.cy = 0;
  E.col_offset = 0;
  E.row_offset = 0;

  /* Set filename.  */
  free (E.filename);
  E.filename = strdup (filename);

  editor_select_syntax_highlight ();

  fp = fopen (filename, "r");
  if (fp == NULL)
    {
      /* New file.  */
      editor_set_status_message ("New file: %s", filename);
      return;
    }

  while ((linelen = getline (&line, &linecap, fp)) != -1)
    {
      int tabs;
      char *expanded;
      int idx;
      int j;

      /* Strip newlines.  */
      while (linelen > 0
             && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
        linelen--;

      /* Count tabs.  */
      tabs = 0;
      for (j = 0; j < linelen; j++)
        {
          if (line[j] == '\t')
            tabs++;
        }

      /* Expand tabs.  */
      expanded = malloc (linelen + (tabs * (TAB_STOP - 1)) + 1);
      idx = 0;

      for (j = 0; j < linelen; j++)
        {
          if (line[j] == '\t')
            {
              expanded[idx++] = ' ';
              while (idx % TAB_STOP != 0)
                expanded[idx++] = ' ';
            }
          else
            {
              expanded[idx++] = line[j];
            }
        }

      expanded[idx] = '\0';
      editor_insert_row (E.num_rows, expanded, idx);
      free (expanded);
    }

  free (line);
  fclose (fp);
  E.dirty = 0;

  editor_set_status_message ("Opened: %s (%d lines)", filename, E.num_rows);
}

static void
editor_save (void)
{
  FILE *fp;
  int len;
  char *buf;

  if (E.filename == NULL)
    {
      E.filename = editor_prompt ("Save as: %s (ESC to cancel)", NULL);
      if (E.filename == NULL)
        {
          editor_set_status_message ("Save cancelled");
          return;
        }
      editor_select_syntax_highlight ();
    }

  buf = editor_rows_to_string (&len);
  if (buf == NULL)
    {
      editor_set_status_message ("Save failed: out of memory");
      return;
    }

  fp = fopen (E.filename, "w");
  if (fp != NULL)
    {
      if (fwrite (buf, 1, len, fp) == (size_t) len)
        {
          fclose (fp);
          E.dirty = 0;
          editor_set_status_message ("Saved %d bytes to %s", len, E.filename);
        }
      else
        {
          fclose (fp);
          editor_set_status_message ("Save failed: write error");
        }
    }
  else
    {
      editor_set_status_message ("Save failed: %s", strerror (errno));
    }

  free (buf);
}

/*** Output Implementation ***/

static void
editor_scroll (void)
{
  int sidebar_offset;
  int available_cols;

  /* Vertical scrolling.  */
  if (E.cy < E.row_offset)
    E.row_offset = E.cy;

  if (E.cy >= E.row_offset + E.screen_rows)
    E.row_offset = E.cy - E.screen_rows + 1;

  /* Horizontal scrolling.  */
  sidebar_offset = (E.sidebar_open ? SIDEBAR_WIDTH + 1 : 0) + LINENO_WIDTH;
  available_cols = E.screen_cols - sidebar_offset;

  if (available_cols < 1)
    available_cols = 1;

  if (E.cx < E.col_offset)
    E.col_offset = E.cx;

  if (E.cx >= E.col_offset + available_cols)
    E.col_offset = E.cx - available_cols + 1;

  /* Sidebar scrolling.  */
  if (E.sidebar_cy < E.sidebar_scroll)
    E.sidebar_scroll = E.sidebar_cy;

  if (E.sidebar_cy >= E.sidebar_scroll + E.screen_rows)
    E.sidebar_scroll = E.sidebar_cy - E.screen_rows + 1;
}

static void
editor_draw_rows (struct append_buffer *ab)
{
  int y;

  for (y = 0; y < E.screen_rows; y++)
    {
      int chars_drawn = 0;
      int filerow;
      char lnbuf[32];

      /* === SIDEBAR === */
      if (E.sidebar_open)
        {
          int file_idx = y + E.sidebar_scroll;

          ab_append (ab, "\x1b[48;5;236m", 11);

          if (file_idx < E.file_count)
            {
              char display[SIDEBAR_WIDTH + 1];
              int name_len;
              int k;

              if (E.focus_sidebar && file_idx == E.sidebar_cy)
                ab_append (ab, "\x1b[7m", 4);

              if (E.files[file_idx].is_dir)
                ab_append (ab, "\x1b[1;34m", 7);
              else
                ab_append (ab, "\x1b[37m", 5);

              /* Format name.  */
              if (E.files[file_idx].is_dir)
                snprintf (display, sizeof (display), "%s/",
                          E.files[file_idx].name);
              else
                snprintf (display, sizeof (display), "%s",
                          E.files[file_idx].name);

              name_len = (int) strlen (display);
              if (name_len > SIDEBAR_WIDTH)
                name_len = SIDEBAR_WIDTH;

              ab_append (ab, display, name_len);
              chars_drawn += name_len;

              /* Pad with spaces.  */
              for (k = name_len; k < SIDEBAR_WIDTH; k++)
                {
                  ab_append (ab, " ", 1);
                  chars_drawn++;
                }

              if (E.focus_sidebar && file_idx == E.sidebar_cy)
                ab_append (ab, "\x1b[27m", 5);
            }
          else
            {
              /* Empty line.  */
              int k;
              for (k = 0; k < SIDEBAR_WIDTH; k++)
                {
                  ab_append (ab, " ", 1);
                  chars_drawn++;
                }
            }

          ab_append (ab, "\x1b[m", 3);
          ab_append (ab, "\x1b[90m|\x1b[m", 10);
          chars_drawn++;
        }

      /* === LINE NUMBERS === */
      filerow = y + E.row_offset;

      if (filerow < E.num_rows)
        snprintf (lnbuf, sizeof (lnbuf), "%4d ", filerow + 1);
      else
        snprintf (lnbuf, sizeof (lnbuf), "   ~ ");

      ab_append (ab, "\x1b[90m", 5);
      ab_append (ab, lnbuf, LINENO_WIDTH);
      ab_append (ab, "\x1b[m", 3);
      chars_drawn += LINENO_WIDTH;

      /* === CONTENT === */
      if (filerow >= E.num_rows)
        {
          /* Welcome message.  */
          if (E.num_rows == 0 && y == E.screen_rows / 3)
            {
              char welcome[80];
              int welcomelen;
              int padding;

              welcomelen = snprintf (welcome, sizeof (welcome),
                                     "xEdit -- version %s", XEDIT_VERSION);

              if (welcomelen > E.screen_cols - chars_drawn)
                welcomelen = E.screen_cols - chars_drawn;

              padding = (E.screen_cols - chars_drawn - welcomelen) / 2;

              while (padding-- > 0)
                ab_append (ab, " ", 1);

              ab_append (ab, welcome, welcomelen);
            }
        }
      else
        {
          /* Text content.  */
          struct editor_row *row = &E.row[filerow];
          int len = row->size - E.col_offset;
          int max_width = E.screen_cols - chars_drawn;

          if (len < 0)
            len = 0;
          if (len > max_width)
            len = max_width;

          if (len > 0)
            {
              char *c = &row->chars[E.col_offset];
              unsigned char *hl = &row->hl[E.col_offset];
              int current_color = -1;
              int in_selection = 0;
              int j;

              for (j = 0; j < len; j++)
                {
                  int col_idx = E.col_offset + j;
                  int selected = is_selected (filerow, col_idx);

                  if (selected && !in_selection)
                    {
                      ab_append (ab, "\x1b[7m", 4);
                      in_selection = 1;
                    }
                  else if (!selected && in_selection)
                    {
                      ab_append (ab, "\x1b[27m", 5);
                      in_selection = 0;
                      current_color = -1;
                    }

                  if (iscntrl ((unsigned char) c[j]))
                    {
                      char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                      ab_append (ab, "\x1b[7m", 4);
                      ab_append (ab, &sym, 1);
                      ab_append (ab, "\x1b[m", 3);

                      if (current_color != -1)
                        {
                          char cbuf[16];
                          int clen = snprintf (cbuf, sizeof (cbuf),
                                               "\x1b[%dm", current_color);
                          ab_append (ab, cbuf, clen);
                        }

                      if (in_selection)
                        ab_append (ab, "\x1b[7m", 4);
                    }
                  else
                    {
                      int color = editor_syntax_to_color (hl[j]);

                      if (color != current_color)
                        {
                          char cbuf[16];
                          int clen;
                          current_color = color;
                          clen = snprintf (cbuf, sizeof (cbuf),
                                           "\x1b[%dm", color);
                          ab_append (ab, cbuf, clen);
                        }

                      ab_append (ab, &c[j], 1);
                    }
                }

              if (in_selection)
                ab_append (ab, "\x1b[27m", 5);
            }

          ab_append (ab, "\x1b[m", 3);
        }

      ab_append (ab, "\x1b[K", 3);
      ab_append (ab, "\r\n", 2);
    }
}

static void
editor_draw_status_bar (struct append_buffer *ab)
{
  char status[80];
  char rstatus[80];
  int len;
  int rlen;

  ab_append (ab, "\x1b[7m", 4);

  len = snprintf (status, sizeof (status), " %.20s%s %s",
                  E.filename ? E.filename : "[No Name]",
                  E.dirty ? " [+]" : "",
                  E.focus_sidebar ? "[SIDEBAR]" : "");

  rlen = snprintf (rstatus, sizeof (rstatus), "%s | Ln %d/%d Col %d ",
                   E.syntax ? E.syntax->filetype : "no ft",
                   E.cy + 1, E.num_rows, E.cx + 1);

  if (len > E.screen_cols)
    len = E.screen_cols;

  ab_append (ab, status, len);

  while (len < E.screen_cols)
    {
      if (E.screen_cols - len == rlen)
        {
          ab_append (ab, rstatus, rlen);
          break;
        }
      else
        {
          ab_append (ab, " ", 1);
          len++;
        }
    }

  ab_append (ab, "\x1b[m", 3);
  ab_append (ab, "\r\n", 2);
}

static void
editor_draw_message_bar (struct append_buffer *ab)
{
  int msglen;

  ab_append (ab, "\x1b[K", 3);

  msglen = (int) strlen (E.status_msg);

  if (msglen > E.screen_cols)
    msglen = E.screen_cols;

  if (msglen > 0 && time (NULL) - E.status_msg_time < 5)
    ab_append (ab, E.status_msg, msglen);
}

static void
editor_refresh_screen (void)
{
  struct append_buffer ab = ABUF_INIT;
  char buf[32];

  editor_scroll ();

  ab_append (&ab, "\x1b[?25l", 6);
  ab_append (&ab, "\x1b[H", 3);

  editor_draw_rows (&ab);
  editor_draw_status_bar (&ab);
  editor_draw_message_bar (&ab);

  /* Position cursor.  */
  if (E.focus_sidebar)
    {
      int sidebar_row = E.sidebar_cy - E.sidebar_scroll + 1;
      snprintf (buf, sizeof (buf), "\x1b[%d;%dH", sidebar_row, 2);
    }
  else
    {
      int content_start = 0;
      int cursor_y;
      int cursor_x;

      if (E.sidebar_open)
        content_start = SIDEBAR_WIDTH + 1;

      content_start += LINENO_WIDTH;

      cursor_y = (E.cy - E.row_offset) + 1;
      cursor_x = (E.cx - E.col_offset) + content_start + 1;

      snprintf (buf, sizeof (buf), "\x1b[%d;%dH", cursor_y, cursor_x);
    }

  ab_append (&ab, buf, (int) strlen (buf));
  ab_append (&ab, "\x1b[?25h", 6);

#ifdef _WIN32
  fwrite (ab.b, 1, ab.len, stdout);
  fflush (stdout);
#else
  write (STDOUT_FILENO, ab.b, ab.len);
#endif

  ab_free (&ab);
}

static void
editor_set_status_message (const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  vsnprintf (E.status_msg, sizeof (E.status_msg), fmt, ap);
  va_end (ap);

  E.status_msg_time = time (NULL);
}

/*** Input Implementation ***/

static char *
editor_prompt (char *prompt, void (*callback) (char *, int))
{
  size_t bufsize = 128;
  char *buf;
  size_t buflen = 0;

  buf = malloc (bufsize);
  buf[0] = '\0';

  while (1)
    {
      int c;

      editor_set_status_message (prompt, buf);
      editor_refresh_screen ();

      c = editor_read_key ();

      if (c == DEL_KEY || c == CTRL_KEY ('h') || c == BACKSPACE)
        {
          if (buflen != 0)
            buf[--buflen] = '\0';
        }
      else if (c == '\x1b')
        {
          editor_set_status_message ("");
          if (callback != NULL)
            callback (buf, c);
          free (buf);
          return NULL;
        }
      else if (c == '\r')
        {
          if (buflen != 0)
            {
              editor_set_status_message ("");
              if (callback != NULL)
                callback (buf, c);
              return buf;
            }
        }
      else if (!iscntrl (c) && c < 128)
        {
          if (buflen == bufsize - 1)
            {
              bufsize *= 2;
              buf = realloc (buf, bufsize);
            }
          buf[buflen++] = (char) c;
          buf[buflen] = '\0';
        }

      if (callback != NULL)
        callback (buf, c);
    }
}

static void
editor_move_cursor (int key)
{
  struct editor_row *row;
  int rowlen;

  row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];

  switch (key)
    {
    case ARROW_LEFT:
      if (E.cx != 0)
        {
          E.cx--;
        }
      else if (E.cy > 0)
        {
          E.cy--;
          E.cx = E.row[E.cy].size;
        }
      break;

    case ARROW_RIGHT:
      if (row != NULL && E.cx < row->size)
        {
          E.cx++;
        }
      else if (row != NULL && E.cx == row->size && E.cy < E.num_rows - 1)
        {
          E.cy++;
          E.cx = 0;
        }
      break;

    case ARROW_UP:
      if (E.cy != 0)
        E.cy--;
      break;

    case ARROW_DOWN:
      if (E.cy < E.num_rows - 1)
        E.cy++;
      break;
    }

  /* Snap cursor to end of line.  */
  row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];
  rowlen = row ? row->size : 0;

  if (E.cx > rowlen)
    E.cx = rowlen;
}

/*** Search Implementation ***/

static void
editor_find_callback (char *query, int key)
{
  static int last_match = -1;
  static int direction = 1;
  static int saved_hl_line = -1;
  static char *saved_hl = NULL;
  int current;
  int i;

  /* Restore previous highlight.  */
  if (saved_hl_line != -1 && saved_hl_line < E.num_rows)
    {
      memcpy (E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].size);
      free (saved_hl);
      saved_hl = NULL;
      saved_hl_line = -1;
    }

  if (key == '\r' || key == '\x1b')
    {
      last_match = -1;
      direction = 1;
      return;
    }
  else if (key == ARROW_RIGHT || key == ARROW_DOWN)
    {
      direction = 1;
    }
  else if (key == ARROW_LEFT || key == ARROW_UP)
    {
      direction = -1;
    }
  else
    {
      last_match = -1;
      direction = 1;
    }

  if (last_match == -1)
    direction = 1;

  current = last_match;

  for (i = 0; i < E.num_rows; i++)
    {
      struct editor_row *row;
      char *match;

      current += direction;

      if (current == -1)
        current = E.num_rows - 1;
      else if (current == E.num_rows)
        current = 0;

      row = &E.row[current];
      match = strstr (row->chars, query);

      if (match != NULL)
        {
          last_match = current;
          E.cy = current;
          E.cx = (int) (match - row->chars);
          E.row_offset = E.num_rows;

          /* Save current highlighting.  */
          saved_hl_line = current;
          saved_hl = malloc (row->size);
          memcpy (saved_hl, row->hl, row->size);

          /* Highlight match.  */
          memset (&row->hl[match - row->chars], HL_MATCH, strlen (query));
          break;
        }
    }
}

static void
editor_find (void)
{
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_col_offset = E.col_offset;
  int saved_row_offset = E.row_offset;
  char *query;

  query = editor_prompt ("Search: %s (ESC/Arrows/Enter)",
                         editor_find_callback);

  if (query != NULL)
    {
      free (query);
    }
  else
    {
      E.cx = saved_cx;
      E.cy = saved_cy;
      E.col_offset = saved_col_offset;
      E.row_offset = saved_row_offset;
    }
}

/*** Main Input Handler ***/

static void
editor_process_keypress (void)
{
  static int quit_times = 1;
  int c;

  c = editor_read_key ();

  switch (c)
    {
    case CTRL_KEY ('q'):
      if (E.dirty && quit_times > 0)
        {
          editor_set_status_message ("WARNING! File has unsaved changes. "
                                     "Press Ctrl-Q again to quit.");
          quit_times--;
          return;
        }
      printf ("\x1b[2J\x1b[H");
      exit (EXIT_SUCCESS);
      break;

    case CTRL_KEY ('s'):
      editor_save ();
      break;

    case CTRL_KEY ('f'):
      editor_find ();
      break;

    case CTRL_KEY ('b'):
      E.sidebar_open = !E.sidebar_open;
      break;

    case CTRL_KEY ('w'):
      if (E.sidebar_open)
        E.focus_sidebar = !E.focus_sidebar;
      break;

    case CTRL_KEY ('z'):
      editor_undo ();
      break;

    case CTRL_KEY ('c'):
      editor_copy ();
      break;

    case CTRL_KEY ('v'):
      editor_paste ();
      break;

    case CTRL_KEY ('x'):
      editor_cut ();
      break;

    case HOME_KEY:
      if (!E.focus_sidebar)
        {
          E.cx = 0;
          editor_clear_selection ();
        }
      break;

    case END_KEY:
      if (!E.focus_sidebar && E.cy < E.num_rows)
        {
          E.cx = E.row[E.cy].size;
          editor_clear_selection ();
        }
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      if (!E.focus_sidebar)
        {
          int times;

          if (c == PAGE_UP)
            E.cy = E.row_offset;
          else
            {
              E.cy = E.row_offset + E.screen_rows - 1;
              if (E.cy > E.num_rows - 1)
                E.cy = E.num_rows - 1;
            }

          times = E.screen_rows;
          while (times--)
            editor_move_cursor (c == PAGE_UP ? ARROW_UP : ARROW_DOWN);

          editor_clear_selection ();
        }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      if (E.focus_sidebar)
        {
          if (c == ARROW_UP && E.sidebar_cy > 0)
            E.sidebar_cy--;
          else if (c == ARROW_DOWN && E.sidebar_cy < E.file_count - 1)
            E.sidebar_cy++;
        }
      else
        {
          editor_move_cursor (c);
          editor_clear_selection ();
        }
      break;

    case SHIFT_ARROW_UP:
    case SHIFT_ARROW_DOWN:
    case SHIFT_ARROW_LEFT:
    case SHIFT_ARROW_RIGHT:
      if (!E.focus_sidebar)
        {
          int move_key;

          editor_start_selection ();

          switch (c)
            {
            case SHIFT_ARROW_UP:
              move_key = ARROW_UP;
              break;
            case SHIFT_ARROW_DOWN:
              move_key = ARROW_DOWN;
              break;
            case SHIFT_ARROW_LEFT:
              move_key = ARROW_LEFT;
              break;
            default:
              move_key = ARROW_RIGHT;
              break;
            }

          editor_move_cursor (move_key);
        }
      break;

    case '\r':
      if (E.focus_sidebar)
        {
          if (E.sidebar_cy < E.file_count)
            {
              struct file_node *f = &E.files[E.sidebar_cy];

              if (f->is_dir)
                {
                  if (chdir (f->name) == 0)
                    {
                      editor_refresh_file_list ();
                      E.sidebar_cy = 0;
                      E.sidebar_scroll = 0;
                    }
                }
              else
                {
                  editor_open (f->name);
                  E.focus_sidebar = 0;
                }
            }
        }
      else
        {
          if (editor_has_selection ())
            editor_delete_selection ();
          editor_insert_newline ();
        }
      break;

    case '\t':
      if (!E.focus_sidebar)
        {
          int i;

          if (editor_has_selection ())
            editor_delete_selection ();

          for (i = 0; i < TAB_STOP; i++)
            editor_insert_char (' ');
        }
      break;

    case BACKSPACE:
    case CTRL_KEY ('h'):
      if (!E.focus_sidebar)
        {
          if (editor_has_selection ())
            editor_delete_selection ();
          else
            editor_del_char ();
        }
      break;

    case DEL_KEY:
      if (!E.focus_sidebar)
        {
          if (editor_has_selection ())
            {
              editor_delete_selection ();
            }
          else
            {
              editor_move_cursor (ARROW_RIGHT);
              editor_del_char ();
            }
        }
      break;

    case '\x1b':
      editor_clear_selection ();
      break;

    default:
      if (!E.focus_sidebar && !iscntrl (c) && c < 256)
        {
          if (editor_has_selection ())
            editor_delete_selection ();
          editor_insert_char (c);
        }
      break;
    }

  /* Reset quit confirmation counter.  */
  if (c != CTRL_KEY ('q'))
    quit_times = 1;
}

/*** Initialization ***/

static void
init_editor (void)
{
  E.cx = 0;
  E.cy = 0;
  E.row_offset = 0;
  E.col_offset = 0;
  E.num_rows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.syntax = NULL;

  E.sidebar_open = 1;
  E.focus_sidebar = 0;
  E.files = NULL;
  E.file_count = 0;
  E.sidebar_cy = 0;
  E.sidebar_scroll = 0;

  E.sel_cx = -1;
  E.sel_cy = -1;
  E.has_selection = 0;

  E.undo_stack = NULL;
  E.undo_size = 0;
  E.undo_capacity = 0;
  E.doing_undo = 0;
  E.undo_seq = 0;
  E.in_paste = 0;

  E.status_msg[0] = '\0';
  E.status_msg_time = 0;

  E.hldb = NULL;
  E.hldb_entries = 0;

  if (get_window_size (&E.screen_rows, &E.screen_cols) == -1)
    die ("get_window_size");

  /* Room for status bar and message bar.  */
  E.screen_rows -= 2;

  load_syntax_database ();
  editor_refresh_file_list ();
}

static void
cleanup (void)
{
  int i;

  /* Free rows.  */
  for (i = 0; i < E.num_rows; i++)
    editor_free_row (&E.row[i]);
  free (E.row);

  /* Free filename.  */
  free (E.filename);

  /* Free undo stack.  */
  free (E.undo_stack);

  /* Free file list.  */
  for (i = 0; i < E.file_count; i++)
    free (E.files[i].name);
  free (E.files);

  /* Free syntax database.  */
  for (i = 0; i < E.hldb_entries; i++)
    free_syntax (&E.hldb[i]);
  free (E.hldb);
}

/*** Main Entry Point ***/

int
main (int argc, char *argv[])
{
  enable_raw_mode ();
  init_editor ();

  if (argc >= 2)
    editor_open (argv[1]);

  editor_set_status_message ("HELP: Ctrl-S = save | Ctrl-Q = quit | "
                             "Ctrl-F = find | Ctrl-B = sidebar");

  while (1)
    {
      editor_refresh_screen ();
      editor_process_keypress ();
    }

  cleanup ();
  return EXIT_SUCCESS;
}