/*
 * Test helper that behaves like a "renamed" process.
 *
 * Controlled by environment variables (so argv stays exactly what the test
 * passed):
 *   PT_MODE=keep      leave argv alone (normal process)
 *   PT_MODE=join      Chromium/Electron setproctitle: join every argument with
 *                     spaces into argv[0]'s memory, NUL-pad the rest, so
 *                     /proc/PID/cmdline becomes ONE string with all params
 *   PT_MODE=title     write PT_TITLE into the argv area (avahi/sshd/postgres
 *                     style "prog: doing something")
 *   PT_COMM=xyz       prctl(PR_SET_NAME) on the main thread (renames comm)
 *   PT_THREADS=N      start N threads, each named with pthread_setname_np
 *                     ("Chrome_IOThread", "ThreadPoolFor", ...)
 * Writes "ready\n" to fd 3 if open, then sleeps until killed.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

static void *idle_thread(void *arg) {
  (void)arg;
  for (;;) pause();
  return NULL;
}

int main(int argc, char **argv) {
  const char *mode = getenv("PT_MODE");
  char *area_begin = argv[0];
  char *area_end = argv[argc - 1] + strlen(argv[argc - 1]); /* last NUL */
  size_t area = (size_t)(area_end - area_begin);

  if (mode && strcmp(mode, "join") == 0) {
    /* Build the joined title first (the strings overlap the target). */
    char *title = malloc(area + 1);
    size_t len = 0;
    for (int i = 0; i < argc; ++i) {
      size_t l = strlen(argv[i]);
      if (i) title[len++] = ' ';
      memcpy(title + len, argv[i], l);
      len += l;
    }
    title[len] = '\0';
    memset(area_begin, 0, area + 1);
    memcpy(area_begin, title, len < area ? len : area);
    free(title);
  } else if (mode && strcmp(mode, "title") == 0) {
    const char *t = getenv("PT_TITLE");
    size_t l = strlen(t);
    memset(area_begin, 0, area + 1);
    memcpy(area_begin, t, l < area ? l : area);
  }

  const char *comm = getenv("PT_COMM");
  if (comm) prctl(PR_SET_NAME, comm, 0, 0, 0);

  const char *nthreads = getenv("PT_THREADS");
  static const char *names[] = {"Chrome_IOThread", "ThreadPoolForeg",
                                "CompositorTileW", "VizCompositorTh",
                                "Renamed-Worker"};
  for (int i = 0; nthreads && i < atoi(nthreads); ++i) {
    pthread_t t;
    pthread_create(&t, NULL, idle_thread, NULL);
    pthread_setname_np(t, names[i % 5]);
  }

  /* Zygote style: fork a child that rewrites its title only afterwards,
   * like Chromium/Electron renderers forked from the zygote (no exec). */
  const char *fork_delay = getenv("PT_FORK_DELAY_MS");
  if (fork_delay) {
    pid_t child = fork();
    if (child == 0) {
      usleep((useconds_t)atoi(fork_delay) * 1000);
      const char *t = getenv("PT_CHILD_TITLE");
      size_t l = strlen(t);
      memset(area_begin, 0, area + 1);
      memcpy(area_begin, t, l < area ? l : area);
      const char *ccomm = getenv("PT_CHILD_COMM");
      if (ccomm) prctl(PR_SET_NAME, ccomm, 0, 0, 0);
      for (;;) pause();
    }
    dprintf(3, "%d\n", child);
  }

  if (write(3, "ready\n", 6) < 0) { /* fd 3 not open: fine */ }
  for (;;) pause();
}
