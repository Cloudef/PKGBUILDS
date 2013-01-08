#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <libgen.h>
#include <mpd/client.h>

/* really dirty code :)
 * it's funny how most useful tools are always like that.
 * will cleanup when I feel like it. */

#define MPD_TIMEOUT 3000
#define MUSIC_DIR "/mnt/東方/music"

#define _D "\1-\2!\1-\5"
#define ERR_SNTX _D" \3%d \2[\4%s \5:: \4%s\2]\5:"
#ifndef NDEBUG
#define OUT(x,...) \
   _prnt(stderr, _D" "x, ##__VA_ARGS__);
#else
#define OUT(x,...) ;
#endif
#define ERR(x,...) \
   _prnt(stderr, ERR_SNTX" "x, __LINE__, __FILE__, __func__, ##__VA_ARGS__);
#define MEMERR(x) \
   ERR("Could not allocate '%zu bytes' for \"%s\"", sizeof(x), __STRING(x));
#define MPDERR()                                                           \
   if (mpd && mpd->connection &&                                           \
         mpd_connection_get_error(mpd->connection) != MPD_ERROR_SUCCESS)   \
      ERR("MPD error: (%d) %s", mpd_connection_get_error(mpd->connection), \
            mpd_connection_get_error_message(mpd->connection));

/* mpd server definition */
typedef struct mpdserver {
   const unsigned int *version;
} mpdserver;

/* mpd queue definition */
typedef struct mpdqueue {
   unsigned int version;
} mpdqueue;

enum {
   PLAY_REPEAT  = 0x1,
   PLAY_RANDOM  = 0x2,
   PLAY_SINGLE  = 0x4,
   PLAY_CONSUME = 0x8
};

/* mpd state definition */
typedef struct mpdstate {
   unsigned int id;
   unsigned int queuever;
   unsigned int queuelen;
   unsigned int crossfade;
   unsigned int playmode;
   int volume;
   int song;
   enum mpd_state state;
} mpdstate;

/* mpd client definition */
typedef struct mpdclient {
   const char *host;
   unsigned int port;
   mpdstate state;
   mpdqueue queue;
   mpdserver server;
   struct mpd_connection *connection;
   struct mpd_status *status;
} mpdclient;
static mpdclient *mpd = NULL;

/* colors */
static const char *colors[] = {
   "\33[31m", /* red */
   "\33[32m", /* green */
   "\33[34m", /* blue */
   "\33[33m", /* yellow */
   "\33[37m", /* white */
   "\33[0m",  /* normal */
};

typedef int (*mpdoptfunc)(int argc, char **argv);

typedef struct mpdopt {
   const char *arg;
   char argc;
   mpdoptfunc func;
} mpdopt;

#define REGISTER_OPT(x) static int x(int argc, char **argv)
REGISTER_OPT(opt_play);
REGISTER_OPT(opt_stop);
REGISTER_OPT(opt_pause);
REGISTER_OPT(opt_toggle);
REGISTER_OPT(opt_next);
REGISTER_OPT(opt_prev);
REGISTER_OPT(opt_ls);
REGISTER_OPT(opt_repeat);
REGISTER_OPT(opt_random);
REGISTER_OPT(opt_single);
REGISTER_OPT(opt_consume);
REGISTER_OPT(opt_crossfade);
#undef REGISTER_OPT

static const mpdopt opts[] = {
   { "play", 0, opt_play },
   { "stop", 0, opt_stop },
   { "pause", 0, opt_pause },
   { "toggle", 0, opt_toggle },
   { "next", 0, opt_next },
   { "prev", 0, opt_prev},
   { "ls", 0, opt_ls },
   { "repeat", 0, opt_repeat },
   { "random", 0, opt_random },
   { "single", 0, opt_single },
   { "consume", 0, opt_consume },
   { "crossfade", 1, opt_crossfade },
   { NULL, 0, NULL },
};

enum {
   RETURN_OK,
   RETURN_FAIL,
};

/* uppercase strcmp */
int _strupcmp(const char *hay, const char *needle)
{
   size_t i, len;
   if ((len = strlen(hay)) != strlen(needle)) return 1;
   for (i = 0; i != len; ++i)
      if (toupper(hay[i]) != toupper(needle[i])) return 1;
   return 0;
}

/* uppercase strstr */
char* _strupstr(const char *hay, const char *needle)
{
   size_t i, r, p, len, len2;
   p = 0; r = 0;
   if (!_strupcmp(hay, needle)) return (char*)hay;
   if ((len = strlen(hay)) < (len2 = strlen(needle))) return NULL;
   for (i = 0; i != len; ++i) {
      if (p == len2) return (char*)&hay[r]; /* THIS IS IT! */
      if (toupper(hay[i]) == toupper(needle[p++])) {
         if (!r) r = i; /* could this be.. */
      } else { if (r) i = r; r = 0; p = 0; } /* ..nope, damn it! */
   }
   if (p == len2) return (char*)&hay[r]; /* THIS IS IT! */
   return NULL;
}

/* colored print */
static void _cprnt(FILE *out, char *buffer) {
   size_t i, len = strlen(buffer);
   for (i = 0; i != len; ++i) {
           if (buffer[i] == '\1') fprintf(out, "%s", colors[0]);
      else if (buffer[i] == '\2') fprintf(out, "%s", colors[1]);
      else if (buffer[i] == '\3') fprintf(out, "%s", colors[2]);
      else if (buffer[i] == '\4') fprintf(out, "%s", colors[3]);
      else if (buffer[i] == '\5') fprintf(out, "%s", colors[4]);
      else fprintf(out, "%c", buffer[i]);
   }
   fprintf(out, "%s\n", colors[5]);
   fflush(out);
}

/* printf wrapper */
static void _prnt(FILE *out, const char *fmt, ...) {
   va_list args;
   char buffer[LINE_MAX];

   memset(buffer, 0, LINE_MAX);
   va_start(args, fmt);
   vsnprintf(buffer, LINE_MAX-1, fmt, args);
   va_end(args);
   _cprnt(out, buffer);
}

/* get mpd status */
static struct mpd_status * get_status(void) {
   struct mpd_status *status;
   assert(mpd->connection);

   if (!(status = mpd_recv_status(mpd->connection)))
      goto mpd_error;
   if (mpd->status) mpd_status_free(mpd->status);
   return mpd->status = status;

mpd_error:
   MPDERR();
   return NULL;
}

/* fetch cover art */
static char* fetch_cover(const char *dir) {
   struct dirent **names; size_t len, n, i;
   char rdir[PATH_MAX], fcover[256], *cover;

   memset(rdir, 0, sizeof(rdir));
   memset(fcover, 0, sizeof(fcover));
   snprintf(rdir, sizeof(rdir)-1, "%s/%s", MUSIC_DIR, dir);
   if (!(n = scandir(rdir, &names, 0, alphasort)) || n == -1)
      return NULL;
   for (i = 0; i != n; ++i) {
      if (names[i]->d_type != DT_REG) continue;
      if (!_strupstr(names[i]->d_name, ".jpg") &&
          !_strupstr(names[i]->d_name, ".png")) continue;

      strncpy(fcover, names[i]->d_name, sizeof(fcover)-1);
      break;
   }
   for (i = 0; i != n; ++i) free(names[i]);
   free(names);

   if (!strlen(fcover))
      return NULL;

   len = strlen(rdir)+1+strlen(fcover)+2;
   if (!(cover = calloc(1, len)))
      return NULL;

   snprintf(cover, len-1, "%s/%s", rdir, fcover);
   return cover;
}

/* get cover art for song */
static char* get_cover_art(const struct mpd_song *song) {
   char *uric, *ret; const char *uri, *urid;
   if (!(uri = mpd_song_get_uri(song)) || !((uric = strdup(uri))))
      return NULL;
   urid = dirname(uric);
   ret = fetch_cover(urid);
   free(uric);
   return ret;
}

/* add song to queue */
static int queue_add_song(const struct mpd_song *song, const char *sep, int printimg) {
   if (!song) return RETURN_FAIL;
   char *basec, *cover = NULL;
   static char *lalbum = NULL;
   static char *lcover = NULL;
   const char *disc    = mpd_song_get_tag(song, MPD_TAG_DISC, 0);
   const char *track   = mpd_song_get_tag(song, MPD_TAG_TRACK, 0);
   const char *comment = mpd_song_get_tag(song, MPD_TAG_COMMENT, 0);
   const char *genre   = mpd_song_get_tag(song, MPD_TAG_GENRE, 0);
   const char *album   = mpd_song_get_tag(song, MPD_TAG_ALBUM, 0);
   const char *title   = mpd_song_get_tag(song, MPD_TAG_TITLE, 0);
   const char *artist  = mpd_song_get_tag(song, MPD_TAG_ARTIST, 0);
   if (!title)  title  = mpd_song_get_tag(song, MPD_TAG_NAME, 0);
   if (!artist) artist = mpd_song_get_tag(song, MPD_TAG_ALBUM_ARTIST, 0);
   if (!artist) artist = mpd_song_get_tag(song, MPD_TAG_COMPOSER, 0);
   if (!artist) artist = mpd_song_get_tag(song, MPD_TAG_PERFORMER, 0);
   if (!album && (basec = strdup(mpd_song_get_uri(song)))) {
      album = basename(basec); free(basec);
   }

#if 0
   OUT("URI:    %s", mpd_song_get_uri(song));
   OUT("Song [%d]: d:%d s:%d e:%d m:%lu q:%d",
         mpd_song_get_id(song),
         mpd_song_get_duration(song),
         mpd_song_get_start(song),
         mpd_song_get_end(song),
         mpd_song_get_last_modified(song),
         mpd_song_get_pos(song));
   OUT("D[%s] T[%s]", disc, track);
   OUT("GENRE:  %s", genre);
   OUT("ARTIST: %s", artist);
   OUT("ALBUM:  %s", album);
   OUT("TITLE:  %s", title);
#else
   if (artist && album && title) {
      if (printimg) {
         if (lcover && !strcmp(lalbum, album)) {
            cover = strdup(lcover);
         } else cover = get_cover_art(song);
         if (cover) printf("IMG:%s\t", cover);
      }
      printf("%s%s%s%s%s\n", artist, sep, album, sep, title);
   }
#endif

   /* remember last album && cover */
   if (lalbum) free(lalbum);
   if (lcover) free(lcover);
   if (album) lalbum = strdup(album);
   else lalbum = NULL;
   if (cover) {
      lcover = strdup(cover);
      free(cover);
   } else lcover = NULL;

   return RETURN_OK;
}

/* match song from queue */
static int queue_match_song(const struct mpd_song *song, const char *needle, const char *sep, int *exact) {
   if (exact) *exact = 0;
   if (!song) return RETURN_FAIL;
   char *basec;
   const char *disc    = mpd_song_get_tag(song, MPD_TAG_DISC, 0);
   const char *track   = mpd_song_get_tag(song, MPD_TAG_TRACK, 0);
   const char *comment = mpd_song_get_tag(song, MPD_TAG_COMMENT, 0);
   const char *genre   = mpd_song_get_tag(song, MPD_TAG_GENRE, 0);
   const char *album   = mpd_song_get_tag(song, MPD_TAG_ALBUM, 0);
   const char *title   = mpd_song_get_tag(song, MPD_TAG_TITLE, 0);
   const char *artist  = mpd_song_get_tag(song, MPD_TAG_ARTIST, 0);
   if (!title)  title  = mpd_song_get_tag(song, MPD_TAG_NAME, 0);
   if (!artist) artist = mpd_song_get_tag(song, MPD_TAG_ALBUM_ARTIST, 0);
   if (!artist) artist = mpd_song_get_tag(song, MPD_TAG_COMPOSER, 0);
   if (!artist) artist = mpd_song_get_tag(song, MPD_TAG_PERFORMER, 0);
   if (!album && (basec = strdup(mpd_song_get_uri(song)))) {
      album = basename(basec); free(basec);
   }

   if (!album || !artist || !title)
      return RETURN_FAIL;

   char found  = 0, *whole = NULL;
   size_t lsep = strlen(sep);
   size_t len  = strlen(artist)+lsep+strlen(album)+lsep+strlen(title)+2;
   if ((whole = malloc(len))) {
      snprintf(whole, len-1, "%s%s%s%s%s", artist, sep, album, sep, title);
      if (!strcmp(needle, whole)) {
         found = 1;
         if (exact) *exact = 1;
      } else if (_strupstr(whole, needle))
         found = 1;

      if (!found) {
         int tokc = 0;
         char *cpy = strdup(needle);
         if (cpy) {
            char *tok = strtok(cpy, " ");
            while (tok) {
               if (!strcmp(tok, whole)) {
                  found++;
               } else if (_strupstr(whole, tok))
                  found++;
               tok = strtok(NULL, " ");
               ++tokc;
            }
            if (tokc != found) found = 0;
            free(cpy);
         }
      }

      free(whole);
      if (found) return RETURN_OK;
   }

#if 0
   if (_strupstr(artist, needle) ||
       _strupstr(album, needle)  ||
       _strupstr(title, needle))
      return RETURN_OK;

   if (_strupstr(needle, artist) ||
       _strupstr(needle, album)  ||
       _strupstr(needle, title))
      return RETURN_OK;
#endif

   return RETURN_FAIL;
}

/* now playing */
static void now_playing(void) {
   char *cover;
   struct mpd_song *song = mpd_run_current_song(mpd->connection);
   queue_add_song(song, "│", 0);
   if ((cover = get_cover_art(song))) {
      printf("%s\n", cover);
      free(cover);
   }
   mpd_song_free(song);
}

/* update queue */
static int update_queue(void) {
   struct mpd_entity *entity;
   assert(mpd && mpd->connection);

#if 0
   mpd_send_list_meta(mpd->connection, "/");
   while ((entity = mpd_recv_entity(mpd->connection))) {
      if (mpd_entity_get_type(entity) == MPD_ENTITY_TYPE_SONG)
         queue_add_song(mpd_entity_get_song(entity));
      mpd_entity_free(entity);
   }
   mpd_response_finish(mpd->connection);
#endif

   if (!mpd_send_list_queue_meta(mpd->connection))
      goto fail;

   while ((entity = mpd_recv_entity(mpd->connection))) {
      if (mpd_entity_get_type(entity) == MPD_ENTITY_TYPE_SONG)
         queue_add_song(mpd_entity_get_song(entity), "│", 1);
      mpd_entity_free(entity);
   }

   mpd->queue.version = mpd_status_get_queue_version(mpd->status);
   if (!mpd_response_finish(mpd->connection))
      goto fail;

   return RETURN_OK;

fail:
   MPDERR();
   return RETURN_FAIL;
}

/* search queue */
static struct mpd_song* queue_search_song(const char *needle) {
   struct mpd_song *song = NULL;
   struct mpd_entity *entity;
   int exact = 0;
   assert(mpd && mpd->connection);

   if (!mpd_send_list_queue_meta(mpd->connection))
      goto fail;

   while ((entity = mpd_recv_entity(mpd->connection))) {
      if (mpd_entity_get_type(entity) == MPD_ENTITY_TYPE_SONG)
         if (!exact && queue_match_song(mpd_entity_get_song(entity), needle, "│", &exact) == RETURN_OK) {
               if (song) mpd_song_free(song);
               song = mpd_song_dup(mpd_entity_get_song(entity));
            }
      mpd_entity_free(entity);
   }

   mpd->queue.version = mpd_status_get_queue_version(mpd->status);
   if (!mpd_response_finish(mpd->connection))
         goto fail;

   return song;
fail:
   MPDERR();
   return NULL;
}

/* update status */
static void update_status(void) {
   assert(mpd && mpd->connection);
   if (mpd->status)    mpd_status_free(mpd->status);
   if (!(mpd->status = mpd_run_status(mpd->connection)))
         MPDERR();

   mpd->state.id        = mpd_status_get_update_id(mpd->status);
   mpd->state.volume    = mpd_status_get_volume(mpd->status);
   mpd->state.crossfade = mpd_status_get_crossfade(mpd->status);
   mpd->state.queuever  = mpd_status_get_queue_version(mpd->status);
   mpd->state.queuelen  = mpd_status_get_queue_length(mpd->status);
   mpd->state.song      = mpd_status_get_song_id(mpd->status);
   mpd->state.state     = mpd_status_get_state(mpd->status);

   if (mpd_status_get_repeat(mpd->status))
      mpd->state.playmode |= PLAY_REPEAT;
   if (mpd_status_get_random(mpd->status))
      mpd->state.playmode |= PLAY_RANDOM;
   if (mpd_status_get_single(mpd->status))
      mpd->state.playmode |= PLAY_SINGLE;
   if (mpd_status_get_consume(mpd->status))
      mpd->state.playmode |= PLAY_CONSUME;

   OUT("State [%d]: V:%d CF:%d, Q:%d QL:%d S:%d SP:%d ST:%s", mpd->state.id,
         mpd->state.volume, mpd->state.crossfade,
         mpd->state.queuever, mpd->state.queuelen, mpd->state.song,
         mpd_status_get_song_pos(mpd->status),
         mpd->state.state==MPD_STATE_STOP?"STOP":
         mpd->state.state==MPD_STATE_PLAY?"PLAY":
         mpd->state.state==MPD_STATE_PAUSE?"PAUSE":"UNKNOWN");
   OUT("Playmode: REPT:%d RAND:%d SING:%d CONS:%d",
         mpd->state.playmode & PLAY_REPEAT,
         mpd->state.playmode & PLAY_RANDOM,
         mpd->state.playmode & PLAY_SINGLE,
         mpd->state.playmode & PLAY_CONSUME);
}

/* quit mpd */
static void quit_mpd(void) {
   assert(mpd);
   if (mpd->connection) mpd_connection_free(mpd->connection);
   if (mpd->status)     mpd_status_free(mpd->status);
   free(mpd); mpd = NULL;
   OUT("Closed mpd connection");
}

/* init and connect mpd */
static int init_mpd(void) {
   unsigned int mpd_port = 6600;
   const char *host = getenv("MPD_HOST");
   const char *port = getenv("MPD_PORT");
   const char *pass = getenv("MPD_PASSWORD");

   if (!host)  host     = "localhost";
   if (port)   mpd_port = strtol(port, (char**) NULL, 10);

   if (mpd) quit_mpd();
   if (!(mpd = calloc(1, sizeof(mpdclient))))
      goto alloc_fail;

   if (!(mpd->connection = mpd_connection_new(host, mpd_port, MPD_TIMEOUT)) ||
         mpd_connection_get_error(mpd->connection))
      goto connect_fail;

   if (pass && !mpd_run_password(mpd->connection, pass))
      goto connect_fail;

   mpd->server.version = mpd_connection_get_server_version(mpd->connection);
   mpd->host = host; mpd->port = mpd_port;
   OUT("Connected to '%s:%d' - Server version [%d.%d.%d]", host, mpd_port,
         mpd->server.version[0], mpd->server.version[1],
         mpd->server.version[2]);

   update_status();
   return RETURN_OK;

alloc_fail:
   MEMERR(mpdclient);
connect_fail:
   ERR("Connection to MPD server '%s:%d' failed", host, mpd_port);
   MPDERR();
   return RETURN_FAIL;
}

#define FUNC_OPT(x) static int x(int argc, char **argv)
FUNC_OPT(opt_play) {
   struct mpd_song *song = NULL;
   size_t len = 0, i = 0;
   char *search;

   if (!argc) mpd_send_play(mpd->connection);
   else {
      for (i = 0; i != argc; ++i) {
         if (i) len += 1;
         len += strlen(argv[i]);
      } len += 2;

      if (!(search = calloc(1, len)))
         return EXIT_FAILURE;

      for (i = 0; i != argc; ++i) {
         if (i) search = strncat(search, " ", len);
         search = strncat(search, argv[i], len);
      }

      OUT("play: %s", search);
      if ((song = queue_search_song(search))) {
         queue_add_song(song, " - ", 0);
         mpd_send_play_id(mpd->connection, mpd_song_get_id(song));
         mpd_song_free(song);
      } else {
         printf("no match for: %s\n", search);
      }
      free(search);
   }

   return EXIT_SUCCESS;

fail:
   MPDERR();
   return EXIT_FAILURE;
}

FUNC_OPT(opt_stop) {
   OUT("stop");
   mpd_send_stop(mpd->connection);
   return EXIT_SUCCESS;
}

FUNC_OPT(opt_pause) {
   OUT("pause");
   mpd_send_pause(mpd->connection, !argc?1:strtol(argv[0], NULL, 10));
   return EXIT_SUCCESS;
}

FUNC_OPT(opt_toggle) {
   OUT("toggle");
   mpd_send_toggle_pause(mpd->connection);
   return EXIT_SUCCESS;
}

FUNC_OPT(opt_next) {
   OUT("next");
   mpd_send_next(mpd->connection);
   return EXIT_SUCCESS;
}

FUNC_OPT(opt_prev) {
   OUT("previous");
   mpd_send_previous(mpd->connection);
   return EXIT_SUCCESS;
}

FUNC_OPT(opt_ls) {
   OUT("ls");
   update_queue();
   return EXIT_SUCCESS;
}

FUNC_OPT(opt_repeat) {
   OUT("repeat");
   mpd_send_repeat(mpd->connection, !argc?1:strtol(argv[0], NULL, 10));
   return EXIT_SUCCESS;
}

FUNC_OPT(opt_random) {
   OUT("random");
   mpd_send_random(mpd->connection, !argc?1:strtol(argv[0], NULL, 10));
   return EXIT_SUCCESS;
}

FUNC_OPT(opt_single) {
   OUT("single");
   mpd_send_single(mpd->connection, !argc?1:strtol(argv[0], NULL, 10));
   return EXIT_SUCCESS;
}

FUNC_OPT(opt_consume) {
   OUT("consume");
   mpd_send_consume(mpd->connection, !argc?1:strtol(argv[0], NULL, 10));
   return EXIT_SUCCESS;
}

FUNC_OPT(opt_crossfade) {
   assert(argc);
   OUT("crossfade");
   mpd_send_crossfade(mpd->connection, strtol(argv[0], NULL, 10));
   return EXIT_SUCCESS;
}
#undef FUNC_OPT

static void usage(char *name) {
   int o;
   printf("usage: %s [", basename(name));
   for (o = 0; opts[o].arg; ++o)
      printf("%s%s", opts[o].arg, opts[o+1].arg?"│":"");
   printf("]\n");
   exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
   int o;
   if (argc >= 2) {
      for (o = 0; opts[o].arg && strcmp(argv[1], opts[o].arg); ++o);
      if (!opts[o].arg || opts[o].argc > argc-2) usage(argv[0]);
      OUT("%s: %d/%d", opts[o].arg, argc-2, opts[o].argc);
   }

   if (init_mpd() != RETURN_OK)
      goto fail;

   if (argc >= 2) {
      for (o = 0; opts[o].arg && strcmp(argv[1], opts[o].arg); ++o);
      if (opts[o].func) opts[o].func(argc-2, argv+2);
   } else now_playing();

   quit_mpd();
   return EXIT_SUCCESS;

fail:
   return EXIT_FAILURE;
}
